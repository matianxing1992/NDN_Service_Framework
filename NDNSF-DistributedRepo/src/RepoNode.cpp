#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/name.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

// Current Spec192 clients may collect Repo ACKs for 30 seconds. Keep the
// corresponding Request/Selection state for another 30 seconds so the final
// Selection can reach its Provider after collection closes.
constexpr uint64_t REPO_PENDING_SELECTION_TTL_MS = 60'000;

std::vector<uint8_t>
payloadOf(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

std::string
digestHex(const std::vector<uint8_t>& bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.data(), bytes.size(), digest);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

uint64_t
nowMilliseconds()
{
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  return value <= 0 ? 1 : static_cast<uint64_t>(value);
}

bool
isPublishTargetPending(const PublishOperationTargetStatus& target)
{
  return target.state == "QUEUED" || target.state == "IN_FLIGHT" ||
         target.state == "STAGED";
}

void
validateCanonicalRepoName(const std::string& value, const ArtifactLimits& limits,
                          const std::string& field)
{
  validateName(value, limits, field);
  if (ndn::Name(value).toUri() != value) {
    throw std::invalid_argument(field + " must use canonical NDN URI spelling");
  }
}

uint32_t
publishRequiredAcks(size_t targetCount, const std::string& consistency)
{
  if (targetCount == 0 || targetCount > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("repo-publish-target-count-invalid: selected target count is invalid");
  }
  if (consistency == "ONE") return 1;
  if (consistency == "QUORUM") return static_cast<uint32_t>(targetCount / 2 + 1);
  if (consistency == "ALL") return static_cast<uint32_t>(targetCount);
  throw std::invalid_argument("repo-publish-consistency-invalid: unsupported consistency value");
}

bool
isBootstrapDeferred(const std::string& error)
{
  return error.find("deferred collaboration ControllerVersion is not ready") !=
         std::string::npos;
}

bool
isRequestTimeout(const std::string& error)
{
  return error.find("timeout") != std::string::npos ||
         error.find("timed out") != std::string::npos;
}

std::optional<std::string>
jsonTopLevelOperation(const std::string& wire)
{
  if (wire.size() > 256 * 1024) {
    return std::nullopt;
  }
  try {
    const auto root = detail::JsonParser(wire).parse();
    if (root.kind != detail::JsonValue::Kind::Object) {
      return std::nullopt;
    }
    const auto operation = root.object.find("operation");
    if (operation == root.object.end() ||
        operation->second.kind != detail::JsonValue::Kind::String) {
      return std::nullopt;
    }
    return operation->second.string;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

RepoObjectManifest
artifactManifest(const ArtifactReference& reference,
                 const std::string& operationId, uint64_t generation)
{
  RepoObjectManifest manifest;
  manifest.objectName = artifactStorageObjectName(reference);
  manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = reference.contentDigest;
  std::transform(manifest.sha256.begin(), manifest.sha256.end(),
                 manifest.sha256.begin(), [] (unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  manifest.size = reference.sizeBytes;
  manifest.segmentCount = 1;
  manifest.replicationFactor = 1;
  manifest.policyEpoch = reference.policyEpoch;
  manifest.generation = generation;
  manifest.writeConsistency = "ONE";
  manifest.requiredWriteAcks = 1;
  manifest.operationId = operationId;
  manifest.lifecycleState = "COMMITTED";
  return manifest;
}

std::string
resolveRepoNode(const std::string& coreRepoNode, std::string configuredRepoNode)
{
  if (configuredRepoNode.empty()) {
    return coreRepoNode;
  }
  if (coreRepoNode.empty() || configuredRepoNode != coreRepoNode) {
    throw std::invalid_argument(
      "repo-artifact-identity-mismatch: artifact and RepoCore logical Repo IDs must match");
  }
  return configuredRepoNode;
}

void
verifyArtifactBytes(const RepoCore& core, const RepoObjectManifest& manifest,
                    const ArtifactReference& reference)
{
  std::string expectedDigest = reference.contentDigest;
  std::transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (!core.has(manifest.objectName) || manifest.size != reference.sizeBytes ||
      manifest.sha256 != expectedDigest) {
    throw std::runtime_error(
      "repo-artifact-byte-verification-failed: stored manifest does not match full reference");
  }

  EVP_MD_CTX* rawContext = EVP_MD_CTX_new();
  if (rawContext == nullptr || EVP_DigestInit_ex(rawContext, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(rawContext);
    throw std::runtime_error("repo-artifact-digest-failed: cannot initialize SHA-256");
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(rawContext, EVP_MD_CTX_free);
  constexpr uint64_t VERIFY_RANGE_BYTES = 8 * 1024 * 1024;
  uint64_t offset = 0;
  while (offset < reference.sizeBytes) {
    const auto length = std::min<uint64_t>(VERIFY_RANGE_BYTES, reference.sizeBytes - offset);
    const auto bytes = core.getRange(manifest.objectName, RepoByteRange{offset, length});
    if (bytes.size() != length || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
      throw std::runtime_error(
        "repo-artifact-byte-verification-failed: stored range length or digest update is invalid");
    }
    offset += length;
  }
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digestSize = 0;
  if (EVP_DigestFinal_ex(context.get(), digest, &digestSize) != 1 || digestSize != 32) {
    throw std::runtime_error("repo-artifact-digest-failed: SHA-256 finalization failed");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string actualDigest;
  actualDigest.reserve(digestSize * 2);
  for (unsigned int i = 0; i < digestSize; ++i) {
    actualDigest.push_back(HEX[digest[i] >> 4]);
    actualDigest.push_back(HEX[digest[i] & 0x0f]);
  }
  if (actualDigest != expectedDigest) {
    throw std::runtime_error(
      "repo-artifact-byte-verification-failed: stored bytes do not match full reference digest");
  }
}

bool
matchesArtifactManifest(const RepoObjectManifest& manifest,
                        const ArtifactReference& reference,
                        uint64_t generation)
{
  std::string expectedDigest = reference.contentDigest;
  std::transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return manifest.objectName == artifactStorageObjectName(reference) &&
         manifest.objectType == "artifact-v2-bytes" &&
         manifest.sha256 == expectedDigest &&
         manifest.size == reference.sizeBytes &&
         manifest.policyEpoch == reference.policyEpoch &&
         manifest.generation == generation &&
         manifest.lifecycleState == "COMMITTED";
}

} // namespace

RepoNode::RepoNode(ndn::Name servicePrefix,
                   StorageCapability capability,
                   std::shared_ptr<RepoStoreBackend> store,
                   std::string repoNode,
                   std::string providerIdentity,
                   std::string artifactMetadataPath,
                   ArtifactLimits artifactLimits,
                   uint64_t availabilityFreshnessMs)
  : m_servicePrefix(std::move(servicePrefix))
  , m_repoNode(resolveRepoNode(capability.repoNode, std::move(repoNode)))
  , m_providerIdentity(std::move(providerIdentity))
  , m_artifactLimits(std::move(artifactLimits))
  , m_availabilityFreshnessMs(availabilityFreshnessMs)
  , m_core(std::move(capability), std::move(store))
{
  if (m_servicePrefix.empty()) {
    throw std::invalid_argument("repo-service-prefix-invalid: service prefix is required");
  }
  if (m_availabilityFreshnessMs == 0) {
    throw std::invalid_argument("repo-availability-freshness-invalid: value must be positive");
  }
  if (!artifactMetadataPath.empty()) {
    validateName(m_repoNode, m_artifactLimits, "repoNode");
    validateName(m_providerIdentity, m_artifactLimits, "providerIdentity");
    m_artifactMetadataStore = std::make_unique<SqliteArtifactMetadataStore>(
      std::move(artifactMetadataPath), true, 15, true);
    if (m_artifactMetadataStore->schemaGeneration() != 15 ||
        !m_artifactMetadataStore->artifactWritesEnabled()) {
      throw std::runtime_error("repo-artifact-catalog-unavailable: Persistent catalog must be writable at generation 15");
    }
  }
}

RepoNode::~RepoNode()
{
  m_originalDataRouteReady.store(false);
  m_originalDataRoute.cancel();
  m_originalDataFilter.cancel();
  stopPublishOperationWorker();
  stopRangeReservationReaper();
}

void
RepoNode::registerOriginalDataServing(ndn::Face& face)
{
  if (m_artifactMetadataStore) {
    throw std::logic_error("original Data serving requires the object-protocol Repo");
  }
  if (m_providerIdentity.empty()) {
    throw std::logic_error("original Data serving requires a Provider identity route");
  }
  if (m_originalDataServingRegistered) {
    throw std::logic_error("original Data serving is already registered");
  }

  const ndn::Name route(m_providerIdentity);
  const std::weak_ptr<RepoNode> weakSelf = shared_from_this();
  // A local-only filter accepts the original producer name. Registering /
  // with NFD would hijack unrelated traffic; only the distinct Repo identity
  // is advertised as a forwarding-hint route.
  m_originalDataFilter = face.setInterestFilter(
    ndn::InterestFilter(ndn::Name("/")),
    [weakSelf, &face] (const ndn::InterestFilter&, const ndn::Interest& interest) {
      if (interest.getCanBePrefix()) return;
      const auto self = weakSelf.lock();
      if (!self) return;
      const auto name = interest.getName().toUri();
      try {
        if (!self->hasDataPacket(name)) return;
        const auto wire = self->getDataPacket(name);
        ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
        block.parse();
        const ndn::Data data(block);
        if (data.getName() == interest.getName()) {
          face.put(data);
          std::fprintf(stdout, "repo-original-data-served name=%s sha256=%s\n",
                       name.c_str(), sha256Hex(wire).c_str());
          std::fflush(stdout);
        }
      }
      catch (const std::exception& error) {
        std::fprintf(stderr, "original Data serving failed name=%s reason=%s\n",
                     name.c_str(), error.what());
      }
    });
  m_originalDataRoute = face.registerPrefix(
    route,
    [weakSelf] (const ndn::Name&) {
      if (const auto self = weakSelf.lock()) {
        self->m_originalDataRouteReady.store(true);
      }
    },
    [weakSelf] (const ndn::Name& name, const std::string& error) {
      if (const auto self = weakSelf.lock()) {
        self->m_originalDataRouteReady.store(false);
      }
      std::fprintf(stderr, "original Data route registration failed prefix=%s reason=%s\n",
                   name.toUri().c_str(), error.c_str());
    });
  m_originalDataServingRegistered = true;
}

void
RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider,
                           std::shared_ptr<ndn_service_framework::ServiceUser> publisherUser,
                           int ackCollectionTimeMs,
                           int requestTimeoutMs)
{
  if (!m_artifactMetadataStore || !publisherUser || ackCollectionTimeMs <= 0 ||
      requestTimeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument(
      "repo-publish-worker-config-invalid: persistent store and valid request deadlines are required");
  }
  registerServices(provider);
  startPublishOperationWorker(std::move(publisherUser), ackCollectionTimeMs,
                              requestTimeoutMs);
}

void
RepoNode::startPublishOperationWorker(
  std::shared_ptr<ndn_service_framework::ServiceUser> publisherUser,
  int ackCollectionTimeMs, int requestTimeoutMs)
{
  if (!m_artifactMetadataStore || !publisherUser || ackCollectionTimeMs <= 0 ||
      requestTimeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument(
      "repo-publish-worker-config-invalid: persistent store and valid request deadlines are required");
  }

  std::vector<std::string> recoverableOperationIds;
  {
    // Serialize the durable recovery snapshot with every acceptance enqueue.
    // A commit completed before this snapshot is discovered below; a commit
    // completed after it blocks in enqueue until the worker is ready.
    std::lock_guard<std::mutex> guard(m_publishWorkerMutex);
    if (m_publishWorkerThread.joinable()) {
      if (m_publishUser != publisherUser ||
          m_publishAckCollectionTimeMs != ackCollectionTimeMs ||
          m_publishRequestTimeoutMs != requestTimeoutMs) {
        throw std::logic_error(
          "repo-publish-worker-already-started: worker configuration cannot be replaced");
      }
      return;
    }
    for (const auto& operation :
         m_artifactMetadataStore->recoverablePublishOperations()) {
      if (!operation.operationId.empty() &&
          m_publishQueued.insert(operation.operationId).second) {
        m_publishQueue.push_back(operation.operationId);
        recoverableOperationIds.push_back(operation.operationId);
      }
    }
    m_publishUser = std::move(publisherUser);
    m_publishAckCollectionTimeMs = ackCollectionTimeMs;
    m_publishRequestTimeoutMs = requestTimeoutMs;
    m_stopPublishWorker = false;
    m_publishWorkerThread = std::thread([this] { runPublishOperationWorker(); });
  }
  if (!recoverableOperationIds.empty()) m_publishWorkerWake.notify_one();
}

void
RepoNode::stopPublishOperationWorker() noexcept
{
  {
    std::lock_guard<std::mutex> guard(m_publishWorkerMutex);
    m_stopPublishWorker = true;
  }
  m_publishWorkerWake.notify_all();
  if (m_publishWorkerThread.joinable()) {
    m_publishWorkerThread.join();
  }
  std::lock_guard<std::mutex> guard(m_publishWorkerMutex);
  m_publishUser = nullptr;
  m_publishQueue.clear();
  m_publishQueued.clear();
}

void
RepoNode::enqueuePublishOperation(const std::string& operationId)
{
  if (operationId.empty()) return;
  {
    std::lock_guard<std::mutex> guard(m_publishWorkerMutex);
    if (m_stopPublishWorker || m_publishUser == nullptr ||
        !m_publishQueued.insert(operationId).second) {
      return;
    }
    m_publishQueue.push_back(operationId);
  }
  m_publishWorkerWake.notify_one();
}

void
RepoNode::runPublishOperationWorker()
{
  constexpr auto RetryBackoff = std::chrono::seconds(5);
  for (;;) {
    std::string operationId;
    {
      std::unique_lock<std::mutex> lock(m_publishWorkerMutex);
      m_publishWorkerWake.wait(lock, [this] {
        return m_stopPublishWorker || !m_publishQueue.empty();
      });
      if (m_stopPublishWorker) return;
      operationId = std::move(m_publishQueue.front());
      m_publishQueue.pop_front();
      m_publishQueued.erase(operationId);
    }

    bool retry = false;
    try {
      retry = runPublishOperationPass(operationId);
    }
    catch (...) {
      // The durable IN_FLIGHT snapshot remains the restart recovery point if
      // the catalog itself could not record this pass. Retry at bounded rate.
      retry = true;
    }
    if (!retry) continue;

    {
      std::unique_lock<std::mutex> lock(m_publishWorkerMutex);
      if (m_publishWorkerWake.wait_for(lock, RetryBackoff, [this] {
            return m_stopPublishWorker;
          })) {
        return;
      }
    }
    enqueuePublishOperation(operationId);
  }
}

bool
RepoNode::runPublishOperationPass(const std::string& operationId)
{
  auto recoverable = m_artifactMetadataStore->recoverablePublishOperations();
  const auto operation = std::find_if(recoverable.begin(), recoverable.end(),
    [&operationId] (const auto& item) { return item.operationId == operationId; });
  if (operation == recoverable.end()) return false;
  auto status = operation->status;
  status.validate(m_artifactLimits);
  if (m_publishUser == nullptr) return false;

  const auto required = publishRequiredAcks(status.targets.size(), status.consistency);
  const auto committedCount = [&status] {
    return static_cast<uint32_t>(std::count_if(
      status.targets.begin(), status.targets.end(), [] (const auto& target) {
        return target.state == "COMMITTED" && target.receipt.has_value();
      }));
  };
  const auto completionTimeForThreshold = [&status, required] {
    std::vector<uint64_t> times;
    for (const auto& target : status.targets) {
      if (target.state == "COMMITTED" && target.receipt) {
        times.push_back(target.receipt->completedAtMs);
      }
    }
    std::sort(times.begin(), times.end());
    if (times.size() < required) return nowMilliseconds();
    return times[required - 1];
  };

  const auto finalize = [this, &status, required, operationId]
    (const std::string& terminalState, uint64_t terminalAtMs) {
      for (auto& target : status.targets) {
        if (isPublishTargetPending(target)) {
          target.state = target.attemptCount == 0 ? "NOT_ATTEMPTED" : "UNKNOWN";
          target.receipt.reset();
          target.failureCode = target.attemptCount == 0
            ? "" : "repo-target-outcome-unknown";
        }
        target.updatedAtMs = terminalAtMs;
      }
      ArtifactCommitReceipt receipt;
      receipt.operationId = operationId;
      receipt.reference = status.reference;
      receipt.generation = status.generation;
      receipt.lifecycleMode = "PERSISTENT";
      receipt.requestedReplicas = static_cast<uint32_t>(status.targets.size());
      receipt.requiredAcks = required;
      receipt.writeConsistency = status.consistency;
      receipt.state = terminalState;
      receipt.completedAtMs = terminalAtMs;
      for (const auto& target : status.targets) {
        ArtifactReplicaCommit replica;
        replica.repoNode = target.target.repoNode;
        replica.providerIdentity = target.target.providerIdentity;
        replica.operationId = operationId;
        replica.reference = status.reference;
        replica.completedAtMs = terminalAtMs;
        if (target.receipt) {
          const auto& commit = *target.receipt;
          replica.state = "COMMITTED";
          replica.receiptId = commit.receiptId;
          replica.generation = commit.generation;
          replica.contentDigest = status.reference.contentDigest;
          replica.persistedBytes = commit.persistedBytes;
          replica.completedAtMs = commit.completedAtMs;
          replica.locationVersion = commit.locationVersion;
          replica.routeState = commit.routeState;
          replica.forwardingHint = commit.forwardingHint;
          ++receipt.achievedReplicas;
        }
        else if (target.state == "NOT_ATTEMPTED") {
          replica.state = "SKIPPED";
          replica.routeState = "UNKNOWN";
        }
        else {
          replica.state = "FAILED";
          replica.routeState = "UNKNOWN";
          replica.failureReason = target.failureCode.empty()
            ? "repo-target-outcome-unknown" : target.failureCode;
        }
        receipt.replicaCommits.push_back(std::move(replica));
      }
      receipt.placementState = receipt.achievedReplicas >= required
        ? "SATISFIED" : receipt.achievedReplicas > 0 ? "PARTIAL" : "UNSATISFIED";
      receipt.receiptId = makeArtifactCommitReceiptId(receipt);
      status.state = terminalState;
      status.updatedAtMs = terminalAtMs;
      status.receiptId = receipt.receiptId;
      const auto receiptJson = receipt.toJson();
      m_artifactMetadataStore->finalizePublishOperation(
        operationId, status, receiptJson, terminalAtMs);
    };

  if (committedCount() >= required) {
    finalize("COMMITTED", completionTimeForThreshold());
    return false;
  }

  // The operation owner may replicate only from its own accepted seed. Verify
  // the seed's current durable bytes before selecting any remote target.
  try {
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    const auto objectName = artifactStorageObjectName(status.reference);
    if (!m_core.has(objectName)) {
      throw std::runtime_error("repo-seed-source-missing: accepted seed bytes are absent");
    }
    const auto manifest = m_core.getManifest(objectName);
    verifyArtifactBytes(m_core, manifest, status.reference);
  }
  catch (...) {
    for (auto& target : status.targets) {
      if (isPublishTargetPending(target)) {
        target.state = "FAILED";
        target.failureCode = "repo-seed-source-invalid";
        target.receipt.reset();
      }
    }
    finalize("FAILED", nowMilliseconds());
    return false;
  }

  const uint64_t chunkBytes = std::min<uint64_t>(
    m_artifactLimits.maxChunkBytes,
    std::max<uint64_t>(1, m_artifactLimits.maxPacketPayloadBytes / 2));
  if (chunkBytes == 0) {
    for (auto& target : status.targets) {
      if (isPublishTargetPending(target)) {
        target.state = "FAILED";
        target.failureCode = "repo-worker-chunk-limit-invalid";
      }
    }
    finalize("FAILED", nowMilliseconds());
    return false;
  }

  bool ownerSourceFailed = false;
  bool deferredForBootstrap = false;
  bool retryForUncertainOutcome = false;
  for (size_t index = 0; index < status.targets.size(); ++index) {
    auto& targetStatus = status.targets[index];
    if (!isPublishTargetPending(targetStatus)) continue;
    if (targetStatus.target == status.seed) {
      // The seed receipt is the acceptance barrier and is never re-written by
      // its own placement worker.
      continue;
    }

    const ndn::Name providerIdentity(targetStatus.target.providerIdentity);
    const ndn::Name servicePrefix(targetStatus.target.servicePrefix);
    const auto serviceName = makeRepoServiceName(servicePrefix, "STORE");
    const std::vector<ndn::Name> providers{providerIdentity};
    bool ready = false;
    try {
      ready = m_publishUser->isRequestBootstrapReady(serviceName, providers);
    }
    catch (...) {
      ready = false;
    }
    if (!ready) {
      deferredForBootstrap = true;
      if (targetStatus.state != "QUEUED") {
        targetStatus.state = "QUEUED";
        targetStatus.receipt.reset();
        targetStatus.failureCode.clear();
        targetStatus.updatedAtMs = nowMilliseconds();
        m_artifactMetadataStore->updatePublishOperationTarget(
          operationId, targetStatus, targetStatus.updatedAtMs);
        status.state = "IN_PROGRESS";
        status.updatedAtMs = targetStatus.updatedAtMs;
      }
      continue;
    }

    const uint32_t previousAttemptCount = targetStatus.attemptCount;
    const bool reconcilingPriorAttempt = previousAttemptCount != 0;
    if (previousAttemptCount >= 65535) {
      targetStatus.state = "FAILED";
      targetStatus.failureCode = "repo-target-attempt-limit";
      targetStatus.receipt.reset();
      targetStatus.updatedAtMs = nowMilliseconds();
    }
    else {
      targetStatus.state = "IN_FLIGHT";
      targetStatus.attemptCount = previousAttemptCount + 1;
      targetStatus.failureCode.clear();
      targetStatus.receipt.reset();
      targetStatus.updatedAtMs = nowMilliseconds();
      status.state = "IN_PROGRESS";
      status.updatedAtMs = targetStatus.updatedAtMs;
      m_artifactMetadataStore->updatePublishOperationTarget(
        operationId, targetStatus, targetStatus.updatedAtMs);

      bool readingSeed = false;
      auto requestMayHaveBeenEmitted = std::make_shared<std::atomic<bool>>(false);
      try {
        std::vector<uint8_t> response;
        if (status.reference.sizeBytes <= chunkBytes) {
          readingSeed = true;
          std::vector<uint8_t> bytes;
          {
            std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
            bytes = m_core.get(artifactStorageObjectName(status.reference));
          }
          readingSeed = false;
          ArtifactStoreRequest write;
          write.operationId = operationId;
          write.generation = status.generation;
          write.reference = status.reference;
          write.operationOwner = status.seed;
          write.bytes = std::move(bytes);
          response = RepoClient::requestSelectedService(
            *m_publishUser, providerIdentity, serviceName,
            write.toWire(m_artifactLimits), m_publishAckCollectionTimeMs,
            m_publishRequestTimeoutMs, requestMayHaveBeenEmitted);
        }
        else {
          uint64_t offset = 0;
          while (offset < status.reference.sizeBytes) {
            const auto length = std::min<uint64_t>(
              chunkBytes, status.reference.sizeBytes - offset);
            readingSeed = true;
            std::vector<uint8_t> bytes;
            {
              std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
              bytes = m_core.getRange(artifactStorageObjectName(status.reference),
                                      RepoByteRange{offset, length});
            }
            readingSeed = false;
            ArtifactRangeWriteRequest write;
            write.operationId = operationId;
            write.generation = status.generation;
            write.reference = status.reference;
            write.operationOwner = status.seed;
            write.offsetBytes = offset;
            write.lengthBytes = length;
            write.bytes = std::move(bytes);
            const auto rangeService = makeRepoServiceName(servicePrefix, "STORE_RANGE");
            const auto staged = RepoClient::requestSelectedService(
              *m_publishUser, providerIdentity, rangeService,
              write.toWire(m_artifactLimits), m_publishAckCollectionTimeMs,
              m_publishRequestTimeoutMs, requestMayHaveBeenEmitted);
            if (std::string(staged.begin(), staged.end()) != "staged") {
              throw std::runtime_error(
                "repo-target-range-stage-invalid: selected Repo returned an unexpected stage response");
            }
            offset += length;
          }
          targetStatus.state = "STAGED";
          targetStatus.updatedAtMs = nowMilliseconds();
          m_artifactMetadataStore->updatePublishOperationTarget(
            operationId, targetStatus, targetStatus.updatedAtMs);
          status.state = "IN_PROGRESS";
          status.updatedAtMs = targetStatus.updatedAtMs;
          ArtifactRangeCommitRequest commit;
          commit.operationId = operationId;
          commit.generation = status.generation;
          commit.reference = status.reference;
          commit.operationOwner = status.seed;
          const auto commitService = makeRepoServiceName(servicePrefix, "COMMIT_RANGES");
          response = RepoClient::requestSelectedService(
            *m_publishUser, providerIdentity, commitService,
            commit.toWire(m_artifactLimits), m_publishAckCollectionTimeMs,
            m_publishRequestTimeoutMs, requestMayHaveBeenEmitted);
        }

        const std::string receiptWire(response.begin(), response.end());
        auto receipt = RepoArtifactCommitReceipt::fromJson(receiptWire, m_artifactLimits);
        receipt.validate(m_artifactLimits);
        if (receipt.repoNode != targetStatus.target.repoNode ||
            receipt.providerIdentity != targetStatus.target.providerIdentity ||
            receipt.operationId != operationId || receipt.reference != status.reference ||
            receipt.generation != status.generation ||
            receipt.persistedBytes != status.reference.sizeBytes ||
            receipt.routeState != "ROUTABLE" ||
            receipt.forwardingHint != targetStatus.target.servicePrefix) {
          throw std::runtime_error(
            "repo-target-receipt-binding-failed: commit receipt differs from selected target or intent");
        }
        targetStatus.state = "COMMITTED";
        targetStatus.receipt = std::move(receipt);
        targetStatus.failureCode.clear();
        targetStatus.updatedAtMs = targetStatus.receipt->completedAtMs;
      }
      catch (const std::exception& error) {
        if (readingSeed) {
          ownerSourceFailed = true;
          targetStatus.state = "FAILED";
          targetStatus.failureCode = "repo-seed-source-invalid";
        }
        else if (isBootstrapDeferred(error.what()) &&
                 !requestMayHaveBeenEmitted->load(std::memory_order_acquire)) {
          targetStatus.state = "QUEUED";
          targetStatus.attemptCount = previousAttemptCount;
          targetStatus.failureCode.clear();
          deferredForBootstrap = true;
        }
        else {
          const bool uncertain = isRequestTimeout(error.what()) ||
            requestMayHaveBeenEmitted->load(std::memory_order_acquire);
          if (uncertain && !reconcilingPriorAttempt) {
            // Keep the durable operation recoverable and replay the same
            // operation ID once before exposing an unknown terminal result.
            targetStatus.state = "IN_FLIGHT";
            retryForUncertainOutcome = true;
          }
          else {
            targetStatus.state = uncertain ? "UNKNOWN" : "FAILED";
            targetStatus.failureCode = uncertain
              ? "repo-target-outcome-unknown" : "repo-target-store-rejected";
          }
        }
        targetStatus.receipt.reset();
        targetStatus.updatedAtMs = nowMilliseconds();
      }
    }

    status.targets[index] = targetStatus;
    const auto currentTime = targetStatus.updatedAtMs;
    const bool currentPending = isPublishTargetPending(targetStatus);
    const bool pendingAfterThis = std::any_of(
      status.targets.begin(), status.targets.end(),
      [&targetStatus, &status] (const auto& other) {
        return &other != &targetStatus && isPublishTargetPending(other);
      });

    if (ownerSourceFailed) {
      finalize("FAILED", nowMilliseconds());
      return false;
    }

    if (committedCount() >= required) {
      finalize("COMMITTED", completionTimeForThreshold());
      return false;
    }
    if (!pendingAfterThis && !currentPending) {
      finalize(committedCount() > 0 ? "PARTIAL" : "FAILED", currentTime);
      return false;
    }
    if (targetStatus.state == "QUEUED") {
      m_artifactMetadataStore->updatePublishOperationTarget(
        operationId, targetStatus, targetStatus.updatedAtMs);
      status.state = "IN_PROGRESS";
      status.updatedAtMs = targetStatus.updatedAtMs;
    }
    else if (isPublishTargetPending(targetStatus)) {
      m_artifactMetadataStore->updatePublishOperationTarget(
        operationId, targetStatus, targetStatus.updatedAtMs);
      status.state = "IN_PROGRESS";
      status.updatedAtMs = targetStatus.updatedAtMs;
    }
    else {
      m_artifactMetadataStore->updatePublishOperationTarget(
        operationId, targetStatus, targetStatus.updatedAtMs);
      status.state = "IN_PROGRESS";
      status.updatedAtMs = targetStatus.updatedAtMs;
    }
  }

  if (std::any_of(status.targets.begin(), status.targets.end(), isPublishTargetPending)) {
    return deferredForBootstrap || retryForUncertainOutcome;
  }
  finalize(committedCount() > 0 ? "PARTIAL" : "FAILED", nowMilliseconds());
  return false;
}

void
RepoNode::startRangeReservationReaper()
{
  if (!m_artifactMetadataStore) {
    return;
  }
  std::lock_guard<std::mutex> guard(m_rangeReaperMutex);
  if (m_rangeReaperThread.joinable()) {
    return;
  }
  m_stopRangeReaper = false;
  m_rangeReaperThread = std::thread([this] {
    constexpr auto ReapInterval = std::chrono::seconds(30);
    constexpr auto IdleTimeout = std::chrono::minutes(10);
    std::unique_lock<std::mutex> lock(m_rangeReaperMutex);
    while (!m_rangeReaperWake.wait_for(lock, ReapInterval,
                                       [this] { return m_stopRangeReaper; })) {
      lock.unlock();
      try {
        (void)m_core.reapExpiredRangeReservations(IdleTimeout);
      }
      catch (...) {
        // A failed cleanup pass leaves the reservation owned and retryable.
      }
      lock.lock();
    }
  });
}

void
RepoNode::stopRangeReservationReaper() noexcept
{
  {
    std::lock_guard<std::mutex> guard(m_rangeReaperMutex);
    m_stopRangeReaper = true;
  }
  m_rangeReaperWake.notify_all();
  if (m_rangeReaperThread.joinable()) {
    m_rangeReaperThread.join();
  }
}

const ndn::Name&
RepoNode::servicePrefix() const
{
  return m_servicePrefix;
}

RepoCore&
RepoNode::core()
{
  return m_core;
}

const RepoCore&
RepoNode::core() const
{
  return m_core;
}

RepoObjectManifest
RepoNode::put(const std::string& objectName,
              const std::vector<uint8_t>& payload,
              const std::string& objectType,
              uint32_t replicationFactor,
              const std::string& policyEpoch,
              std::vector<std::string> replicaNodes)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.put(objectName, payload, objectType, replicationFactor,
                    policyEpoch, std::move(replicaNodes));
}

RepoObjectManifest
RepoNode::putManifest(const RepoObjectManifest& manifest)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.putManifest(manifest);
}

bool
RepoNode::isPersistentArtifactVisible(const RepoObjectManifest& manifest) const
{
  if (manifest.objectType != "artifact-v2-bytes" || manifest.operationId.empty()) {
    return true;
  }
  if (!m_artifactMetadataStore) {
    return false;
  }
  return m_artifactMetadataStore->hasPersistentCommitReceiptForObject(
    manifest.objectName, m_repoNode, m_providerIdentity, manifest.sha256,
    manifest.size, manifest.policyEpoch, manifest.generation);
}

void
RepoNode::requirePersistentArtifactVisible(const RepoObjectManifest& manifest) const
{
  if (!isPersistentArtifactVisible(manifest)) {
    throw std::out_of_range(
      "repo-artifact-not-accepted: no durable local commit receipt authorizes this artifact");
  }
}

std::vector<uint8_t>
RepoNode::get(const std::string& objectName) const
{
  requirePersistentArtifactVisible(m_core.getManifest(objectName));
  return m_core.get(objectName);
}

void
RepoNode::putRange(const RepoObjectManifest& manifest,
                   RepoByteRange range,
                   const std::vector<uint8_t>& bytes)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  m_core.putRange(manifest, range, bytes);
}

RepoObjectManifest
RepoNode::commitRanges(const RepoObjectManifest& manifest)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.commitRanges(manifest);
}

void
RepoNode::abortRanges(const std::string& objectName)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  m_core.abortRanges(objectName);
}

std::vector<uint8_t>
RepoNode::getRange(const std::string& objectName, RepoByteRange range) const
{
  requirePersistentArtifactVisible(m_core.getManifest(objectName));
  return m_core.getRange(objectName, range);
}

RepoObjectManifest
RepoNode::getManifest(const std::string& objectName) const
{
  auto manifest = m_core.getManifest(objectName);
  requirePersistentArtifactVisible(manifest);
  return manifest;
}

std::vector<RepoObjectManifest>
RepoNode::list() const
{
  auto manifests = m_core.list();
  manifests.erase(std::remove_if(manifests.begin(), manifests.end(), [this] (const auto& manifest) {
    return !isPersistentArtifactVisible(manifest);
  }), manifests.end());
  return manifests;
}

RepoCacheStatus
RepoNode::cacheStatus() const
{
  return m_core.cacheStatus();
}

bool
RepoNode::remove(const std::string& objectName)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.remove(objectName);
}

RepoObjectManifest
RepoNode::putDataPacket(const std::string& dataName,
                        const std::vector<uint8_t>& wire)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.putDataPacket(dataName, wire, m_repoNode, m_providerIdentity);
}

RepoWriteReceipt
RepoNode::importLegacyDataPacketReceipt(const std::string& dataName)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.importLegacyDataPacketReceipt(dataName, m_repoNode,
                                               m_providerIdentity);
}

std::vector<uint8_t>
RepoNode::getDataPacket(const std::string& dataName) const
{
  return m_core.getDataPacket(dataName);
}

bool
RepoNode::hasDataPacket(const std::string& dataName) const
{
  return m_core.hasDataPacket(dataName);
}

void
RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider)
{
  const auto self = weak_from_this().lock();
  if (!self) {
    throw std::logic_error(
      "RepoNode network service registration requires shared ownership");
  }
  auto ack = [] (const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "repo-ready";
    decision.pendingStateTtlMs = REPO_PENDING_SELECTION_TTL_MS;
    return decision;
  };

  const auto registerRepoService =
    [&provider, &ack, self](
      const ndn::Name& serviceName,
      ndn_service_framework::ServiceProvider::RequestHandler handler,
      ndn_service_framework::ServiceProvider::AckStrategyHandler serviceAck = {}) {
      const auto traceServiceName = serviceName.toUri();
      if (!serviceAck) {
        serviceAck = ack;
      }
      ndn_service_framework::ServiceProvider::RequestHandler retainedHandler =
        [self, traceServiceName, handler = std::move(handler)](
          const ndn::Name& requesterIdentity,
          const ndn::Name& providerName,
          const ndn::Name& resolvedServiceName,
          const ndn::Name& requestId,
          const ndn_service_framework::RequestMessage& request) {
          (void)self;
          const char* traceSetting =
            std::getenv("NDNSF_SPEC192_REPO_OPERATION_TRACE");
          if (traceSetting != nullptr && traceSetting[0] == '1' &&
              traceSetting[1] == '\0') {
            const auto requester = requesterIdentity.toUri();
            std::fprintf(stderr, "SPEC192_REPO_REQUEST service=%s requester=%s\n",
                         traceServiceName.c_str(), requester.c_str());
          }
          return handler(requesterIdentity, providerName, resolvedServiceName,
                         requestId, request);
        };
      provider.addService(serviceName, std::move(serviceAck), std::move(retainedHandler));
    };

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request), requesterIdentity.toUri(), true));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE_RANGE"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreRange(payloadOf(request), requesterIdentity.toUri(), true));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "COMMIT_RANGES"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCommitRanges(payloadOf(request), requesterIdentity.toUri(), true));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "ABORT_RANGES"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleAbortRanges(payloadOf(request), requesterIdentity.toUri(), true));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "INSERT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        if (m_artifactMetadataStore) {
          throw std::runtime_error(
            "repo-persistent-operation-required: INSERT is disabled for persistent artifact repos");
        }
        return makeResponse(handleInsert(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        if (m_artifactMetadataStore) {
          throw std::runtime_error(
            "repo-persistent-operation-required: STORE_MANIFEST is disabled for persistent artifact repos");
        }
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "FETCH_RANGE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetchRange(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "DATA_RECEIPT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDataReceipt(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "DATA_LOCATION"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDataLocation(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    },
    [this] (const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "repo-artifact-capability-v1";
      decision.pendingStateTtlMs = REPO_PENDING_SELECTION_TTL_MS;
      // A fresh capability inquiry is the maintenance boundary before a new
      // placement operation. The periodic owner sweep bounds abandoned client
      // staging; backend startup also removes orphaned staging after a Repo
      // process restart.
      (void)m_core.reapExpiredRangeReservations(std::chrono::minutes(10));
      const auto wire = ArtifactCapabilityAck{1, artifactCapability()}.toJson();
      decision.payload = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
      return decision;
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CACHE_STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCacheStatus());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STATUS"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStatus(payloadOf(request), requesterIdentity.toUri()));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogStatus());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_SNAPSHOT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogSnapshot());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_DELTA"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogDelta(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_LOOKUP"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogLookup(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        if (m_artifactMetadataStore) {
          throw std::runtime_error(
            "repo-persistent-operation-required: DELETE is disabled for persistent artifact repos");
        }
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  startRangeReservationReaper();
}

void
RepoNode::registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry)
{
  auto registerLocalRepoService =
    [&] (const std::string& operation,
         std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> handler) {
      registry.registerLocalService(
        makeRepoServiceName(m_servicePrefix, operation),
        [this, handler = std::move(handler)] (
          const ndn::Name&, const ndn::Name&,
          const ndn_service_framework::RequestMessage& request,
          ndn_service_framework::ResponseMessage& response) {
          try {
            response = makeResponse(handler(payloadOf(request)));
          }
          catch (const std::exception& e) {
            response = makeError(e.what());
          }
        });
    };

  registerLocalRepoService("STORE", [this] (const std::vector<uint8_t>& request) {
    return handleStore(request);
  });
  registerLocalRepoService("STORE_RANGE", [this] (const std::vector<uint8_t>& request) {
    return handleStoreRange(request);
  });
  registerLocalRepoService("COMMIT_RANGES", [this] (const std::vector<uint8_t>& request) {
    return handleCommitRanges(request);
  });
  registerLocalRepoService("ABORT_RANGES", [this] (const std::vector<uint8_t>& request) {
    return handleAbortRanges(request);
  });
  registerLocalRepoService("INSERT", [this] (const std::vector<uint8_t>& request) {
    return handleInsert(request);
  });
  registerLocalRepoService("STORE_MANIFEST", [this] (const std::vector<uint8_t>& request) {
    return handleStoreManifest(request);
  });
  registerLocalRepoService("FETCH", [this] (const std::vector<uint8_t>& request) {
    return handleFetch(request);
  });
  registerLocalRepoService("FETCH_RANGE", [this] (const std::vector<uint8_t>& request) {
    return handleFetchRange(request);
  });
  registerLocalRepoService("MANIFEST", [this] (const std::vector<uint8_t>& request) {
    return handleManifest(request);
  });
  registerLocalRepoService("INVENTORY", [this] (const std::vector<uint8_t>&) {
    return handleInventory();
  });
  registerLocalRepoService("CAPABILITY", [this] (const std::vector<uint8_t>&) {
    return handleCapability();
  });
  registerLocalRepoService("CACHE_STATUS", [this] (const std::vector<uint8_t>&) {
    return handleCacheStatus();
  });
  registerLocalRepoService("STATUS", [this] (const std::vector<uint8_t>& request) {
    return handleStatus(request);
  });
  registerLocalRepoService("CATALOG_STATUS", [this] (const std::vector<uint8_t>&) {
    return handleCatalogStatus();
  });
  registerLocalRepoService("CATALOG_SNAPSHOT", [this] (const std::vector<uint8_t>&) {
    return handleCatalogSnapshot();
  });
  registerLocalRepoService("CATALOG_DELTA", [this] (const std::vector<uint8_t>& request) {
    return handleCatalogDelta(request);
  });
  registerLocalRepoService("CATALOG_LOOKUP", [this] (const std::vector<uint8_t>& request) {
    return handleCatalogLookup(request);
  });
  registerLocalRepoService("DELETE", [this] (const std::vector<uint8_t>& request) {
    return handleDelete(request);
  });
}

void
RepoNode::setDataReferenceFetcher(DataReferenceFetcher fetcher)
{
  m_dataReferenceFetcher = std::move(fetcher);
}

RepoOperationStatus
RepoNode::insertWirePackets(const RepoDataReference& reference,
                            const std::vector<std::vector<uint8_t>>& wirePackets)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  if (reference.objectName.empty()) {
    throw std::invalid_argument("repo data reference objectName must not be empty");
  }

  RepoOperationStatus status;
  status.operationId = allocateOperationId();
  status.operation = "INSERT";
  status.state = "STORING";
  status.objectName = reference.objectName;
  status.totalSegments = wirePackets.size();
  status.message = "storing opaque Data wire packets";
  rememberStatus(status);

  try {
    std::vector<uint8_t> concatenated;
    uint64_t totalSize = 0;
    std::vector<std::string> packetNames;
    packetNames.reserve(wirePackets.size());
    const ndn::Name expectedPrefix(reference.dataPrefix);
    for (size_t i = 0; i < wirePackets.size(); ++i) {
      totalSize += wirePackets[i].size();
      concatenated.insert(concatenated.end(), wirePackets[i].begin(), wirePackets[i].end());
      ndn::Block block(ndn::span<const uint8_t>(wirePackets[i].data(),
                                                wirePackets[i].size()));
      block.parse();
      const ndn::Data data(block);
      if (!expectedPrefix.isPrefixOf(data.getName())) {
        throw std::runtime_error("repo-packet-set-invalid: Data name is outside "
                                 "declared dataPrefix: " +
                                 data.getName().toUri());
      }
      const auto dataName = data.getName().toUri();
      if (std::find(packetNames.begin(), packetNames.end(), dataName) != packetNames.end()) {
        throw std::runtime_error(
          "repo-packet-set-invalid: duplicate NDN Data name: " + dataName);
      }
      if (m_core.hasDataPacket(dataName) &&
          m_core.getDataPacket(dataName) != wirePackets[i]) {
        throw std::runtime_error(
          "repo-data-wire-conflict: immutable NDN Data name conflict: " + dataName);
      }
      packetNames.push_back(dataName);
    }

    if (reference.expectedSize != 0 && totalSize != reference.expectedSize) {
      status.state = "FAILED";
      status.message = "fetched wire packet size mismatch";
      rememberStatus(status);
      return status;
    }
    if (!reference.expectedSha256.empty() &&
        sha256Hex(concatenated) != reference.expectedSha256) {
      status.state = "FAILED";
      status.message = "fetched wire packet sha256 mismatch";
      rememberStatus(status);
      return status;
    }

    for (size_t i = 0; i < wirePackets.size(); ++i) {
      m_core.putDataPacket(packetNames[i], wirePackets[i],
                           m_repoNode, m_providerIdentity);
      status.completedSegments = i + 1;
      rememberStatus(status);
    }

    RepoObjectManifest manifest;
    manifest.objectName = reference.objectName;
    manifest.objectType = reference.objectType;
    manifest.sha256 = sha256Hex(concatenated);
    manifest.size = totalSize;
    manifest.segmentCount = static_cast<uint32_t>(wirePackets.size());
    manifest.packetNames = packetNames;
    m_core.putManifest(manifest);

    status.state = "DONE";
    status.message = "stored app-owned segmented Data wire packets";
    status.completedSegments = wirePackets.size();
    status.totalSegments = wirePackets.size();
    rememberStatus(status);
    return status;
  }
  catch (const std::exception& e) {
    status.state = "FAILED";
    status.message = e.what();
    rememberStatus(status);
    return status;
  }
}

void
RepoNode::validatePersistentWriteIdentity(const ArtifactReference& reference,
                                          const std::string& operationId,
                                          uint64_t generation,
                                          const std::string& requesterIdentity,
                                          const std::optional<RepoArtifactTarget>& operationOwner,
                                          bool allowRemoteOwnerQuery,
                                          bool forceRemoteOwnerQuery) const
{
  if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error(
      "repo-artifact-identity-unconfigured: Persistent receipt owner is unavailable");
  }
  if (requesterIdentity.empty()) {
    throw std::runtime_error(
      "repo-publish-intent-required: Persistent writes require an authenticated principal");
  }

  if (operationOwner) {
    validateCanonicalRepoName(operationOwner->repoNode, m_artifactLimits,
                              "operationOwner.repoNode");
    validateCanonicalRepoName(operationOwner->providerIdentity, m_artifactLimits,
                              "operationOwner.providerIdentity");
    validateCanonicalRepoName(operationOwner->servicePrefix, m_artifactLimits,
                              "operationOwner.servicePrefix");
    if (requesterIdentity != operationOwner->providerIdentity ||
        operationOwner->repoNode == m_repoNode ||
        operationOwner->providerIdentity == m_providerIdentity) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unauthorized: replica write must come from another authenticated seed Provider");
    }

    const RepoArtifactTarget localTarget{
      m_repoNode, m_providerIdentity, m_servicePrefix.toUri()};
    constexpr auto CacheLifetime = std::chrono::seconds(30);
    constexpr size_t MaxCacheEntries = 256;
    const auto matchesBinding = [&] (const VerifiedRemotePublishBinding& binding) {
      return binding.reference == reference && binding.generation == generation &&
             binding.owner == *operationOwner && binding.target == localTarget;
    };
    const auto requireCachedBinding = [&] (bool allowExpiredQuery) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> guard(m_remotePublishBindingMutex);
      auto found = m_remotePublishBindings.find(operationId);
      if (found != m_remotePublishBindings.end()) {
        if (now - found->second.verifiedAt > CacheLifetime) {
          m_remotePublishBindings.erase(found);
        }
        else {
          if (!matchesBinding(found->second)) {
            throw std::runtime_error(
              "repo-publish-replica-owner-conflict: operation ID is already bound to another verified target");
          }
          return true;
        }
      }
      if (!allowExpiredQuery) {
        throw std::runtime_error(
          "repo-publish-replica-owner-unverified: selected seed proof expired or was evicted");
      }
      return false;
    };
    if (!forceRemoteOwnerQuery && requireCachedBinding(allowRemoteOwnerQuery)) return;

    std::shared_ptr<ndn_service_framework::ServiceUser> verifier;
    int configuredAckMs = 0;
    int configuredTimeoutMs = 0;
    {
      std::lock_guard<std::mutex> guard(m_publishWorkerMutex);
      verifier = m_publishUser;
      configuredAckMs = m_publishAckCollectionTimeMs;
      configuredTimeoutMs = m_publishRequestTimeoutMs;
    }
    if (!verifier) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unverified: target Provider ServiceUser is unavailable");
    }

    const int timeoutMs = std::min(configuredTimeoutMs, 2000);
    const int ackCollectionMs = std::min(configuredAckMs, timeoutMs - 1);
    if (ackCollectionMs <= 0 || timeoutMs <= ackCollectionMs) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unverified: target Provider query deadline is invalid");
    }
    PublishOperationStatusRequest query;
    query.operationId = operationId;
    query.target = localTarget;
    const auto queryJson = query.toJson();
    const std::vector<uint8_t> queryWire(queryJson.begin(), queryJson.end());
    const auto response = RepoClient::requestSelectedService(
      *verifier, ndn::Name(operationOwner->providerIdentity),
      makeRepoServiceName(ndn::Name(operationOwner->servicePrefix), "STATUS"),
      queryWire, ackCollectionMs, timeoutMs);
    const std::string responseJson(response.begin(), response.end());
    const auto status = PublishOperationStatus::fromJson(responseJson, m_artifactLimits);
    status.validate(m_artifactLimits);
    if (status.operationId != operationId || !(status.reference == reference) ||
        status.generation != generation || !(status.seed == *operationOwner) ||
        status.state == "PREPARING") {
      throw std::runtime_error(
        "repo-publish-replica-owner-unverified: seed status does not bind the exact operation");
    }
    const auto targetStatus = std::find_if(
      status.targets.begin(), status.targets.end(), [&localTarget] (const auto& item) {
        return item.target == localTarget;
      });
    if (targetStatus == status.targets.end() || targetStatus->attemptCount == 0 ||
        (targetStatus->state != "IN_FLIGHT" && targetStatus->state != "STAGED" &&
         targetStatus->state != "COMMITTED")) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unverified: seed did not select and attempt this exact target");
    }

    VerifiedRemotePublishBinding verified{
      reference, generation, *operationOwner, localTarget,
      std::chrono::steady_clock::now()};
    {
      std::lock_guard<std::mutex> guard(m_remotePublishBindingMutex);
      const auto now = std::chrono::steady_clock::now();
      for (auto it = m_remotePublishBindings.begin(); it != m_remotePublishBindings.end();) {
        if (now - it->second.verifiedAt > CacheLifetime) it = m_remotePublishBindings.erase(it);
        else ++it;
      }
      const auto found = m_remotePublishBindings.find(operationId);
      if (found != m_remotePublishBindings.end() && !matchesBinding(found->second)) {
        throw std::runtime_error(
          "repo-publish-replica-owner-conflict: operation ID is already bound to another verified target");
      }
      if (found == m_remotePublishBindings.end() &&
          m_remotePublishBindings.size() >= MaxCacheEntries) {
        const auto oldest = std::min_element(
          m_remotePublishBindings.begin(), m_remotePublishBindings.end(),
          [] (const auto& lhs, const auto& rhs) {
            return lhs.second.verifiedAt < rhs.second.verifiedAt;
          });
        if (oldest != m_remotePublishBindings.end()) m_remotePublishBindings.erase(oldest);
      }
      m_remotePublishBindings[operationId] = std::move(verified);
    }
  }
  else {
    const auto operation = m_artifactMetadataStore->lookupPublishOperation(
      operationId, requesterIdentity);
    if (!operation) {
      throw std::runtime_error(
        "repo-publish-intent-required: seed write has no matching durable PUBLISH_INTENT");
    }
    const RepoArtifactTarget expectedSeed{
      m_repoNode, m_providerIdentity, m_servicePrefix.toUri()};
    if (!(operation->intent.reference == reference) ||
        operation->intent.generation != generation ||
        !(operation->intent.seed == expectedSeed)) {
      throw std::runtime_error(
        "repo-publish-intent-mismatch: seed write differs from its accepted operation");
    }
  }

  const auto expected = artifactManifest(reference, operationId, generation);
  if (m_core.has(expected.objectName)) {
    const auto stored = m_core.getManifest(expected.objectName);
    if (stored.objectType != expected.objectType || stored.sha256 != expected.sha256 ||
        stored.size != expected.size || stored.policyEpoch != expected.policyEpoch ||
        (stored.generation != 0 && stored.generation != generation)) {
      throw std::runtime_error(
        "repo-artifact-byte-commit-conflict: stored manifest differs from the complete artifact reference");
    }
    verifyArtifactBytes(m_core, stored, reference);
  }
}

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request)
{
  return handleStore(request, {}, false);
}

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request,
                      const std::string& requesterIdentity,
                      bool networkRequest)
{
  if (!request.empty() && request.front() == '{') {
    const std::string wire(request.begin(), request.end());
    if (jsonTopLevelOperation(wire) == "PUBLISH_INTENT") {
      return handlePublishIntent(request, requesterIdentity);
    }
  }
  static constexpr uint8_t MAGIC_V1[] = {'A', 'R', 'V', '2', 'P', 'U', 'T', '1'};
  static constexpr uint8_t MAGIC_V2[] = {'A', 'R', 'V', '2', 'P', 'U', 'T', '2'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()))) {
    const auto write = ArtifactStoreRequest::fromWire(request, m_artifactLimits);
    std::string expectedDigest = write.reference.contentDigest;
    std::transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(),
                   [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (digestHex(write.bytes) != expectedDigest) {
      throw std::runtime_error(
        "repo-artifact-byte-verification-failed: STORE payload does not match full reference digest");
    }
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner, false);
    const auto manifest = artifactManifest(write.reference, write.operationId,
                                           write.generation);
    if (m_core.has(manifest.objectName)) {
      const auto stored = m_core.getManifest(manifest.objectName);
      verifyArtifactBytes(m_core, stored, write.reference);
    }
    else {
      (void)m_core.handleStore(encodeStoreRequest(manifest, write.bytes));
    }
    ArtifactRangeCommitRequest commit;
    commit.operationId = write.operationId;
    commit.generation = write.generation;
    commit.reference = write.reference;
    commit.operationOwner = write.operationOwner;
    return handlePersistentCommitRanges(commit, true, requesterIdentity);
  }
  if (networkRequest && m_artifactMetadataStore) {
    throw std::runtime_error(
      "repo-persistent-operation-required: STORE requires a versioned artifact request");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleStore(request);
}

std::vector<uint8_t>
RepoNode::handleStoreRange(const std::vector<uint8_t>& request)
{
  return handleStoreRange(request, {}, false);
}

std::vector<uint8_t>
RepoNode::handleStoreRange(const std::vector<uint8_t>& request,
                           const std::string& requesterIdentity,
                           bool networkRequest)
{
  static constexpr uint8_t MAGIC_V1[] = {'A', 'R', 'V', '2', 'R', 'N', 'G', '1'};
  static constexpr uint8_t MAGIC_V2[] = {'A', 'R', 'V', '2', 'R', 'N', 'G', '2'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()))) {
    const auto write = ArtifactRangeWriteRequest::fromWire(request, m_artifactLimits);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner, false);
    auto manifest = artifactManifest(write.reference, write.operationId, write.generation);
    if (m_core.has(manifest.objectName)) {
      const auto existing = m_core.getRange(
        manifest.objectName, RepoByteRange{write.offsetBytes, write.lengthBytes});
      if (existing != write.bytes) {
        throw std::runtime_error(
          "repo-artifact-byte-commit-conflict: existing bytes differ from retried range");
      }
    }
    else {
      m_core.putRange(manifest,
                      RepoByteRange{write.offsetBytes, write.lengthBytes},
                      write.bytes);
    }
    return toBytes("staged");
  }
  if (networkRequest && m_artifactMetadataStore) {
    throw std::runtime_error(
      "repo-persistent-operation-required: STORE_RANGE requires a versioned artifact request");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleStoreRange(request);
}

std::vector<uint8_t>
RepoNode::handleCommitRanges(const std::vector<uint8_t>& request)
{
  return handleCommitRanges(request, {}, false);
}

std::vector<uint8_t>
RepoNode::handleCommitRanges(const std::vector<uint8_t>& request,
                             const std::string& requesterIdentity,
                             bool networkRequest)
{
  static constexpr uint8_t MAGIC_V1[] = {'A', 'R', 'V', '2', 'C', 'O', 'M', '1'};
  static constexpr uint8_t MAGIC_V2[] = {'A', 'R', 'V', '2', 'C', 'O', 'M', '2'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()))) {
    const auto commit = ArtifactRangeCommitRequest::fromWire(request, m_artifactLimits);
    validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                    commit.generation, requesterIdentity,
                                    commit.operationOwner);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                    commit.generation, requesterIdentity,
                                    commit.operationOwner, false);
    return handlePersistentCommitRanges(commit, false, requesterIdentity);
  }
  if (networkRequest && m_artifactMetadataStore) {
    throw std::runtime_error(
      "repo-persistent-operation-required: COMMIT_RANGES requires a versioned artifact request");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleCommitRanges(request);
}

std::vector<uint8_t>
RepoNode::handlePersistentCommitRanges(const ArtifactRangeCommitRequest& commit,
                                       bool payloadAlreadyCommitted,
                                       const std::string& requesterIdentity)
{
  auto manifest = artifactManifest(commit.reference, commit.operationId, commit.generation);
  if (!m_core.has(manifest.objectName)) {
    if (commit.reference.sizeBytes == 0) {
      manifest = m_core.put(manifest.objectName, {}, manifest.objectType, 1,
                            manifest.policyEpoch);
      manifest.generation = commit.generation;
      manifest.operationId = commit.operationId;
    }
    else {
      m_core.commitRanges(manifest);
    }
  }
  else if (!payloadAlreadyCommitted) {
    // Existing immutable bytes were compared with every retried range before
    // commit. Do not create a second RepoCore reservation for the new operation.
  }

  auto stored = m_core.getManifest(manifest.objectName);
  if (stored.objectType != manifest.objectType ||
      stored.sha256 != manifest.sha256 ||
      stored.size != commit.reference.sizeBytes ||
      stored.policyEpoch != commit.reference.policyEpoch ||
      (stored.generation != 0 && stored.generation != commit.generation)) {
    throw std::runtime_error(
      "repo-artifact-byte-commit-conflict: durable object manifest differs from the complete reference");
  }
  verifyArtifactBytes(m_core, stored, commit.reference);
  if (stored.operationId.empty() || stored.generation == 0) {
    auto firstCommitManifest = manifest;
    if (!stored.operationId.empty()) firstCommitManifest.operationId = stored.operationId;
    if (stored.generation != 0) firstCommitManifest.generation = stored.generation;
    m_core.putManifest(firstCommitManifest);
    stored = m_core.getManifest(manifest.objectName);
  }
  if (stored.size != commit.reference.sizeBytes ||
      stored.sha256 != manifest.sha256 ||
      (stored.generation != 0 && stored.generation != commit.generation)) {
    throw std::runtime_error(
      "repo-artifact-manifest-binding-failed: durable manifest no longer matches the artifact");
  }

  // Range staging and the earlier STORE admission can outlive the owner
  // operation's active attempt. Recheck the selected seed immediately before
  // making the durable local receipt/current LOCATION visible, and reject if
  // this point-in-time STATUS already reports terminal UNKNOWN. This check is
  // not a lock on the seed journal: a timeout after the response may still
  // leave a Repo-local receipt, which the immutable owner aggregate excludes.
  if (commit.operationOwner) {
    validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                    commit.generation, requesterIdentity,
                                    commit.operationOwner, true, true);
  }

  RepoArtifactCommitReceipt receipt;
  receipt.repoNode = m_repoNode;
  receipt.providerIdentity = m_providerIdentity;
  receipt.operationId = commit.operationId;
  const auto referenceWire = commit.reference.toJson();
  const auto receiptMaterial = m_repoNode + "\n" + m_providerIdentity + "\n" +
                               commit.operationId + "\n" + referenceWire;
  const std::vector<uint8_t> receiptBytes(receiptMaterial.begin(), receiptMaterial.end());
  receipt.receiptId = "repo-artifact-" + digestHex(receiptBytes);
  receipt.reference = commit.reference;
  receipt.generation = commit.generation;
  receipt.persistedBytes = commit.reference.sizeBytes;
  receipt.completedAtMs = nowMilliseconds();
  receipt.routeState = "ROUTABLE";
  receipt.forwardingHint = m_servicePrefix.toUri();
  const auto expiresAt = receipt.completedAtMs + 60 * 60 * 1000;
  auto committed = m_artifactMetadataStore->commitPersistentArtifact(
    receipt, expiresAt, requesterIdentity);
  if (committed.acceptedSeedOperation) {
    enqueuePublishOperation(receipt.operationId);
  }
  return toBytes(committed.receipt.toJson());
}

std::vector<uint8_t>
RepoNode::handleAbortRanges(const std::vector<uint8_t>& request)
{
  return handleAbortRanges(request, {}, false);
}

std::vector<uint8_t>
RepoNode::handleAbortRanges(const std::vector<uint8_t>& request,
                            const std::string& requesterIdentity,
                            bool networkRequest)
{
  static constexpr uint8_t MAGIC[] = {'A', 'R', 'V', '2', 'A', 'B', 'T', '1'};
  if (request.size() >= sizeof(MAGIC) &&
      std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin())) {
    if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
      throw std::runtime_error(
        "repo-artifact-identity-unconfigured: Persistent range owner is unavailable");
    }
    const auto abort = ArtifactRangeAbortRequest::fromWire(request, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(abort.reference, abort.operationId,
                                    abort.generation, requesterIdentity,
                                    std::nullopt);
    const auto manifest = artifactManifest(abort.reference, abort.operationId,
                                           abort.generation);
    (void)m_core.abortRangesIfOwned(manifest);
    return toBytes("aborted");
  }
  if (networkRequest && m_artifactMetadataStore) {
    throw std::runtime_error(
      "repo-persistent-operation-required: ABORT_RANGES requires a versioned artifact request");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  m_core.abortRanges(decodeRangeAbortRequest(request));
  return toBytes("aborted");
}

std::vector<uint8_t>
RepoNode::handleInsert(const std::vector<uint8_t>& request)
{
  const auto reference = parseDataReferenceJson(toString(request));
  if (reference.objectName.empty()) {
    throw std::invalid_argument("repo data reference objectName must not be empty");
  }
  if (reference.dataPrefix.empty()) {
    throw std::invalid_argument("repo data reference dataPrefix must not be empty");
  }

  if (m_dataReferenceFetcher) {
    try {
      auto wirePackets = m_dataReferenceFetcher(reference);
      const auto storedStatus = insertWirePackets(reference, wirePackets);
      return toBytes(storedStatus.toJson());
    }
    catch (const std::exception& e) {
      RepoOperationStatus status;
      status.operationId = allocateOperationId();
      status.operation = "INSERT";
      status.state = "FAILED";
      status.objectName = reference.objectName;
      status.message = e.what();
      rememberStatus(status);
      return toBytes(status.toJson());
    }
  }

  RepoOperationStatus status;
  status.operationId = allocateOperationId();
  status.operation = "INSERT";
  status.state = "FAILED";
  status.objectName = reference.objectName;
  status.totalSegments = reference.hasFinalSegment
    ? (reference.finalSegment >= reference.firstSegment
       ? reference.finalSegment - reference.firstSegment + 1
       : 0)
    : 0;
  status.message = "no SegmentFetcher adapter configured";
  rememberStatus(status);
  return toBytes(status.toJson());
}

std::vector<uint8_t>
RepoNode::handleStoreManifest(const std::vector<uint8_t>& request)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleStoreManifest(request);
}

std::vector<uint8_t>
RepoNode::handleFetch(const std::vector<uint8_t>& request) const
{
  requirePersistentArtifactVisible(m_core.getManifest(toString(request)));
  return m_core.handleFetch(request);
}

std::vector<uint8_t>
RepoNode::handleFetchRange(const std::vector<uint8_t>& request) const
{
  std::string objectName;
  RepoByteRange range;
  decodeRangeReadRequest(request, objectName, range);
  (void)range;
  requirePersistentArtifactVisible(m_core.getManifest(objectName));
  return m_core.handleFetchRange(request);
}

std::vector<uint8_t>
RepoNode::handleManifest(const std::vector<uint8_t>& request) const
{
  requirePersistentArtifactVisible(m_core.getManifest(toString(request)));
  return m_core.handleManifest(request);
}

std::vector<uint8_t>
RepoNode::handleDataReceipt(const std::vector<uint8_t>& request) const
{
  if (m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error("repo-data-receipt-object-mode-required");
  }
  const auto query = RepoDataPacketQuery::fromJson(toString(request));
  if (ndn::Name(query.dataName).toUri() != query.dataName) {
    throw std::invalid_argument("repo-data-query-name-not-canonical");
  }
  const auto receipt = m_core.getDataPacketReceipt(
    query.dataName, query.expectedWireSha256);
  if (receipt.repoNode != m_repoNode ||
      receipt.providerIdentity != m_providerIdentity ||
      receipt.digest != query.expectedWireSha256) {
    throw std::out_of_range("repo-data-receipt-mismatch");
  }
  return toBytes(receipt.toJson());
}

std::vector<uint8_t>
RepoNode::handleDataLocation(const std::vector<uint8_t>& request) const
{
  if (m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error("repo-data-location-object-mode-required");
  }
  const auto query = RepoDataPacketQuery::fromJson(toString(request));
  if (ndn::Name(query.dataName).toUri() != query.dataName) {
    throw std::invalid_argument("repo-data-query-name-not-canonical");
  }
  const auto receipt = m_core.getDataPacketReceipt(
    query.dataName, query.expectedWireSha256);
  if (receipt.repoNode != m_repoNode ||
      receipt.providerIdentity != m_providerIdentity ||
      receipt.digest != query.expectedWireSha256) {
    throw std::out_of_range("repo-data-location-receipt-mismatch");
  }
  RepoDataPacketLocation location;
  location.receiptId = receipt.operationId;
  location.repoNode = m_repoNode;
  location.providerIdentity = m_providerIdentity;
  location.dataName = query.dataName;
  location.wireSha256 = receipt.digest;
  location.observedAtMs = nowMilliseconds();
  location.expiresAtMs = location.observedAtMs +
                         std::min<uint64_t>(m_availabilityFreshnessMs, 5000);
  std::optional<RepoObjectManifest> current;
  try {
    current = m_core.getManifest(query.dataName);
  }
  catch (const std::out_of_range&) {
    location.state = "ABSENT";
  }
  catch (const std::exception&) {
    return toBytes(location.toJson());
  }
  if (current) {
    try {
      if (current->objectType != "ndn-data-wire" ||
          current->sha256 != receipt.digest) {
        location.state = "ABSENT";
      }
      else {
        const auto currentDigest = sha256Hex(m_core.getDataPacket(query.dataName));
        if (currentDigest == receipt.digest && m_originalDataRouteReady.load()) {
          location.state = "PRESENT";
          location.forwardingHint = m_providerIdentity;
        }
      }
    }
    catch (const std::exception&) {
      // A storage failure or corrupt wire is UNKNOWN, never a claim of absence.
    }
  }
  return toBytes(location.toJson());
}

std::vector<uint8_t>
RepoNode::handleInventory() const
{
  return toBytes(encodeInventory(list()));
}

std::vector<uint8_t>
RepoNode::handleCapability() const
{
  return m_core.handleCapability();
}

ArtifactCapability
RepoNode::artifactCapability() const
{
  ArtifactCapability capability;
  capability.repoNode = m_repoNode;
  capability.formatVersions = {"artifact-manifest-v2", "exact-packet-v1"};
  capability.digestAlgorithms = {"sha256"};
  capability.signatureAlgorithms = {"rsa-sha256", "ecdsa-sha256", "ed25519"};
  capability.maxArtifactBytes = m_artifactLimits.maxArtifactBytes;
  capability.maxChunkBytes = m_artifactLimits.maxChunkBytes;
  capability.maxRootEncodedBytes = m_artifactLimits.maxRootEncodedBytes;
  capability.maxPageEncodedBytes = m_artifactLimits.maxPageEncodedBytes;
  capability.maxPageEntries = m_artifactLimits.maxPageEntries;
  capability.maxManifestDepth = m_artifactLimits.maxManifestDepth;
  capability.supportsResume = true;
  capability.supportsReplicaReceipts = static_cast<bool>(m_artifactMetadataStore);
  capability.policyEpoch = "repo-policy-v1";
  return capability;
}

std::vector<uint8_t>
RepoNode::handleCacheStatus() const
{
  return m_core.handleCacheStatus();
}

std::vector<uint8_t>
RepoNode::handleCatalogStatus() const
{
  auto status = m_core.catalogStatus();
  status.objectCount = list().size();
  return toBytes(status.toJson());
}

std::vector<uint8_t>
RepoNode::handleCatalogSnapshot() const
{
  auto snapshot = m_core.catalogSnapshot();
  snapshot.entries.erase(std::remove_if(
    snapshot.entries.begin(), snapshot.entries.end(), [this] (const auto& entry) {
      return !isPersistentArtifactVisible(entry.manifest);
    }), snapshot.entries.end());
  return toBytes(snapshot.toJson());
}

std::vector<uint8_t>
RepoNode::handleCatalogDelta(const std::vector<uint8_t>& request) const
{
  const auto text = toString(request);
  const auto sinceEpoch = text.empty() ? 0 : static_cast<uint64_t>(std::stoull(text));
  auto delta = m_core.catalogDelta(sinceEpoch);
  delta.entries.erase(std::remove_if(
    delta.entries.begin(), delta.entries.end(), [this] (const auto& entry) {
      return !isPersistentArtifactVisible(entry.manifest);
    }), delta.entries.end());
  return toBytes(delta.toJson());
}

std::vector<uint8_t>
RepoNode::handleCatalogLookup(const std::vector<uint8_t>& request) const
{
  auto entry = m_core.catalogLookup(toString(request));
  requirePersistentArtifactVisible(entry.manifest);
  return toBytes(entry.toJson());
}

std::vector<uint8_t>
RepoNode::handleDelete(const std::vector<uint8_t>& request)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleDelete(request);
}

std::vector<uint8_t>
RepoNode::handleStatus(const std::vector<uint8_t>& request) const
{
  return handleStatus(request, {});
}

std::vector<uint8_t>
RepoNode::handleStatus(const std::vector<uint8_t>& request,
                       const std::string& requesterIdentity) const
{
  const auto wire = toString(request);
  if (!request.empty() && request.front() == '{') {
    if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
      throw std::runtime_error(
        "repo-artifact-location-unavailable: Persistent receipt owner is not configured");
    }
    const auto operation = jsonTopLevelOperation(wire);
    if (operation == "PUBLISH_STATUS") {
      const char* traceSetting = std::getenv("NDNSF_SPEC192_SELECTED_REQUEST_TRACE");
      const bool traceEnabled = traceSetting != nullptr &&
                                traceSetting[0] == '1' && traceSetting[1] == '\0';
      const auto traceStage = [traceEnabled] (const char* stage) {
        if (!traceEnabled) return;
        std::fprintf(stderr, "Spec192 owner STATUS handler: stage=%s\n", stage);
        std::fflush(stderr);
      };
      traceStage("entered");
      if (requesterIdentity.empty()) {
        throw std::runtime_error(
          "repo-publish-operation-unauthorized: authenticated requester is required");
      }
      const auto query = PublishOperationStatusRequest::fromJson(wire);
      traceStage("request-parsed");
      const auto operation = m_artifactMetadataStore->lookupPublishOperation(
        query.operationId, requesterIdentity, query.target);
      traceStage(operation ? "operation-found" : "operation-not-found");
      if (!operation || operation->state == "PREPARING" ||
          operation->status.operationId.empty()) {
        throw std::out_of_range(
          "repo-publish-operation-not-found: accepted operation status is unavailable");
      }
      const auto statusJson = operation->status.toJson();
      traceStage("response-ready");
      return toBytes(statusJson);
    }
    if (operation == "RECEIPT") {
      if (requesterIdentity.empty()) {
        throw std::runtime_error(
          "repo-receipt-unauthorized: authenticated requester is required");
      }
      const auto query = RepoReceiptLookupRequest::fromJson(wire, m_artifactLimits);
      if (query.repoNode != m_repoNode ||
          query.providerIdentity != m_providerIdentity) {
        throw std::runtime_error(
          "repo-receipt-owner-mismatch: request does not name this Repo and Provider");
      }
      const auto receiptJson = m_artifactMetadataStore->lookupAggregateReceipt(
        query.receiptId);
      if (!receiptJson) {
        throw std::out_of_range("repo-receipt-not-found: aggregate receipt is absent");
      }
      const auto receipt = ArtifactCommitReceipt::fromJson(*receiptJson, m_artifactLimits);
      const auto owner = m_artifactMetadataStore->lookupPublishOperation(
        receipt.operationId, requesterIdentity);
      const auto expectedService = m_servicePrefix.toUri();
      if (!owner || owner->state != receipt.state ||
          (owner->status.state != "COMMITTED" && owner->status.state != "PARTIAL" &&
           owner->status.state != "FAILED") ||
          owner->status.receiptId != query.receiptId ||
          owner->status.operationId != receipt.operationId ||
          !(owner->intent.reference == query.reference) ||
          !(receipt.reference == query.reference) ||
          owner->status.seed.repoNode != m_repoNode ||
          owner->status.seed.providerIdentity != m_providerIdentity ||
          owner->status.seed.servicePrefix != expectedService ||
          owner->intent.seed.repoNode != m_repoNode ||
          owner->intent.seed.providerIdentity != m_providerIdentity ||
          owner->intent.seed.servicePrefix != expectedService ||
          receipt.receiptId != query.receiptId ||
          receipt.lifecycleMode != "PERSISTENT") {
        throw std::runtime_error(
          "repo-receipt-binding-invalid: stored aggregate receipt does not match the authorized seed query");
      }
      return toBytes(receipt.toJson());
    }
    if (operation == "AVAILABILITY") {
      const auto query = RepoAvailabilityProbeRequest::fromJson(wire, m_artifactLimits);
      const auto& expected = query.expectedLocation;
      AvailabilityLocationReport report;
      report.locationKind = "PERSISTENT";
      report.repoNode = expected.repoNode;
      report.providerIdentity = expected.providerIdentity;
      report.generation = expected.generation;
      report.reachability = "REACHABLE";
      report.probeKind = query.probeKind;
      const auto observedAt = nowMilliseconds();
      report.observedAtMs = observedAt;
      report.validUntilMs = observedAt;
      report.reason = "current location tuple is absent, stale, expired, or mismatched";

      std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
      const bool expectedOwner = expected.repoNode == m_repoNode &&
        expected.providerIdentity == m_providerIdentity;
      const auto location = expectedOwner
        ? m_artifactMetadataStore->lookupPersistentLocation(
            query.reference, m_repoNode, m_providerIdentity, observedAt)
        : std::optional<ArtifactLocationRecord>{};
      const bool currentTuple = location &&
        location->repoNode == expected.repoNode &&
        location->providerIdentity == expected.providerIdentity &&
        location->reference == query.reference &&
        location->operationId == expected.operationId &&
        location->receiptId == expected.receiptId &&
        location->generation == expected.generation &&
        location->locationVersion == expected.locationVersion &&
        location->routeState == "ROUTABLE" &&
        location->forwardingHint == m_servicePrefix.toUri() &&
        m_artifactMetadataStore->hasPersistentCommitReceipt(
          query.reference, m_repoNode, m_providerIdentity,
          expected.operationId, expected.receiptId, expected.generation);
      if (currentTuple) {
        report.operationId = location->operationId;
        report.receiptId = location->receiptId;
        report.locationVersion = location->locationVersion;
        report.reason.clear();
        try {
          const auto probe = m_core.probeObjectMetadata(
            artifactStorageObjectName(query.reference));
          if (!probe.manifestPresent) {
            report.objectPresence = "ABSENT";
          }
          else if (!matchesArtifactManifest(probe.manifest, query.reference,
                                            expected.generation) ||
                   !probe.payloadMetadataAvailable) {
            report.objectPresence = "UNKNOWN";
            report.reason = "Repo manifest or committed payload metadata does not bind the current receipt";
          }
          else if (!probe.payloadPresent || !probe.payloadRepresentationValid ||
                   probe.payloadSize != query.reference.sizeBytes) {
            report.objectPresence = "PARTIAL";
          }
          else {
            report.objectPresence = "PRESENT";
            if (query.probeKind == "verify_content") {
              try {
                verifyArtifactBytes(m_core, probe.manifest, query.reference);
                report.integrity = "VERIFIED";
              }
              catch (const std::exception& error) {
                if (std::string(error.what()).find(
                      "stored bytes do not match full reference digest") != std::string::npos) {
                  report.integrity = "FAILED";
                  report.reason = "full payload digest does not match the immutable reference";
                }
                else {
                  report.reason = "full payload verifier could not complete reliably";
                }
              }
            }
          }
        }
        catch (const std::exception&) {
          report.objectPresence = "UNKNOWN";
          report.integrity = "NOT_CHECKED";
          report.reason = "Repo metadata probe could not complete reliably";
        }
        const auto completedAt = nowMilliseconds();
        report.observedAtMs = completedAt;
        if (completedAt >= location->expiresAtMs) {
          report.objectPresence = "UNKNOWN";
          report.integrity = "NOT_CHECKED";
          report.validUntilMs = completedAt;
          report.reason = "current location expired while availability was being checked";
        }
        else {
          const auto freshnessEnd = completedAt >
              std::numeric_limits<uint64_t>::max() - m_availabilityFreshnessMs
            ? std::numeric_limits<uint64_t>::max()
            : completedAt + m_availabilityFreshnessMs;
          report.validUntilMs = std::min(freshnessEnd, location->expiresAtMs);
        }
      }
      RepoAvailabilityProbeResponse response;
      response.reference = query.reference;
      response.expectedLocation = expected;
      response.report = std::move(report);
      return toBytes(response.toJson());
    }
    if (operation && *operation != "LOCATION") {
      throw std::invalid_argument("repo-status-operation-unsupported: " + *operation);
    }
    const auto query = ArtifactLocationRequest::fromJson(wire, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    const auto now = nowMilliseconds();
    auto location = m_artifactMetadataStore->lookupPersistentLocation(
      query.reference, m_repoNode, m_providerIdentity, now);
    if (!location) {
      throw std::out_of_range("repo-artifact-location-not-found: no current Repo-local location");
    }
    if (!m_artifactMetadataStore->hasPersistentCommitReceipt(
          query.reference, m_repoNode, m_providerIdentity,
          location->operationId, location->receiptId, location->generation)) {
      throw std::runtime_error(
        "repo-artifact-location-invalid: current catalog receipt does not bind the requested artifact generation");
    }
    return toBytes(location->toJson());
  }
  const auto operationId = wire;
  std::lock_guard<std::mutex> guard(m_statusMutex);
  const auto it = m_statusById.find(operationId);
  if (it == m_statusById.end()) {
    RepoOperationStatus status;
    status.operationId = operationId;
    status.operation = "UNKNOWN";
    status.state = "UNKNOWN";
    status.message = "repo operation status not found";
    return toBytes(status.toJson());
  }
  return toBytes(it->second.toJson());
}

std::vector<uint8_t>
RepoNode::handlePublishIntent(const std::vector<uint8_t>& request,
                              const std::string& requesterIdentity)
{
  if (!m_artifactMetadataStore || requesterIdentity.empty()) {
    throw std::runtime_error(
      "repo-publish-intent-unauthorized: Persistent seed and authenticated principal are required");
  }
  const std::string wire(request.begin(), request.end());
  const auto intent = PublishOperationIntent::fromJson(wire, m_artifactLimits);
  if (intent.seed.repoNode != m_repoNode ||
      intent.seed.providerIdentity != m_providerIdentity ||
      intent.seed.servicePrefix != m_servicePrefix.toUri()) {
    throw std::runtime_error(
      "repo-publish-seed-mismatch: intent must be submitted to its exact selected seed tuple");
  }
  const auto operationId = makePublishOperationId(
    requesterIdentity, intent.seed, intent.idempotencyKeyDigest);
  return toBytes(m_artifactMetadataStore->reservePublishOperation(
    operationId, requesterIdentity, intent, nowMilliseconds()));
}

ndn_service_framework::ResponseMessage
RepoNode::makeResponse(const std::vector<uint8_t>& payload) const
{
  ndn::Buffer responsePayload(payload.data(), payload.size());
  ndn_service_framework::ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("No error");
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

ndn_service_framework::ResponseMessage
RepoNode::makeError(const std::string& error) const
{
  ndn_service_framework::ResponseMessage response;
  response.setStatus(false);
  response.setErrorInfo(error);
  auto payload = toBytes(error);
  ndn::Buffer responsePayload(payload.data(), payload.size());
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

std::string
RepoNode::allocateOperationId()
{
  std::lock_guard<std::mutex> guard(m_statusMutex);
  std::ostringstream os;
  os << "repo-op-" << ++m_nextOperationId;
  return os.str();
}

void
RepoNode::rememberStatus(const RepoOperationStatus& status)
{
  std::lock_guard<std::mutex> guard(m_statusMutex);
  m_statusById[status.operationId] = status;
  constexpr size_t MaxRetainedStatuses = 1024;
  while (m_statusById.size() > MaxRetainedStatuses) {
    auto oldest = m_statusById.begin();
    for (auto it = std::next(m_statusById.begin()); it != m_statusById.end(); ++it) {
      if (it->second.updatedAtMs < oldest->second.updatedAtMs) {
        oldest = it;
      }
    }
    m_statusById.erase(oldest);
  }
}

} // namespace ndnsf_distributed_repo
