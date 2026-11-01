#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include "FaceDataReferenceFetcher.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

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
#include <openssl/evp.h>
#include <cstdio>
#include <unistd.h>
#include <iomanip>
#include <limits>

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
  if (ndn::Name(value).toUri(ndn::name::UriFormat::CANONICAL) != value &&
      ndn::Name(value).toUri(ndn::name::UriFormat::ALTERNATE) != value) {
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
  // Local Repo backends may enforce a 1 MiB range bound even though the
  // complete artifact is larger. Keep each verification read within it.
  constexpr uint64_t VERIFY_RANGE_BYTES = 1U << 20;
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
  , m_coreOwner(std::make_shared<RepoCore>(std::move(capability), std::move(store)))
  , m_core(*m_coreOwner)
  , m_storeWriteMutexOwner(std::make_shared<std::mutex>())
  , m_storeWriteMutex(*m_storeWriteMutexOwner)
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
    m_artifactMetadataStore = std::make_shared<SqliteArtifactMetadataStore>(
      std::move(artifactMetadataPath), true, 16, true);
    if (m_artifactMetadataStore->schemaGeneration() != 16 ||
        !m_artifactMetadataStore->artifactWritesEnabled()) {
      throw std::runtime_error("repo-artifact-catalog-unavailable: Persistent catalog must be writable at generation 16");
    }
    reconcilePersistentUserStorage();
  }
}

RepoUserStorageObject
RepoNode::measurePersistentUserStorageObject(const RepoObjectManifest& manifest,
                                           bool verifyPayload) const
{
  const auto probe = m_core.probeObjectMetadata(manifest.objectName);
  if (!probe.manifestPresent || !probe.manifestMeasurementAvailable ||
      !probe.payloadMetadataAvailable || !probe.payloadRepresentationValid ||
      probe.manifest.toJson() != manifest.toJson())
    throw std::runtime_error("repo-user-storage-inventory-conflict: physical measurement unavailable");
  std::string digest;
  if (probe.payloadPresent) {
    if (probe.payloadSize != manifest.size)
      throw std::runtime_error("repo-user-storage-inventory-conflict: payload size mismatch");
    if (verifyPayload) {
      ArtifactReference reference;
      reference.contentDigest = manifest.sha256; reference.sizeBytes = manifest.size;
      verifyArtifactBytes(m_core, manifest, reference);
    }
    digest = manifest.sha256;
    std::transform(digest.begin(), digest.end(), digest.begin(), [] (unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
  }
  else {
    // A backend-certified metadata-only object has no physical payload bytes.
    digest = digestHex({});
  }
  return {ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::ALTERNATE),
    std::move(digest), std::max<uint64_t>(1, manifest.generation),
    probe.payloadSize, probe.manifestWireBytes};
}

void
RepoNode::reconcilePersistentUserStorage()
{
  // Constructor-only: services and workers are not registered yet. A new
  // journal connection cannot inherit another connection's admission Ready bit.
  for (const auto& cleanup : m_artifactMetadataStore->pendingUserStorageCleanups(m_repoNode)) {
    const RepoUserStorageDeleteRequest request{1, cleanup.cleanupId,
      cleanup.object.objectName, cleanup.ownerEpoch};
    (void)handleUserStorageDelete(toBytes(request.toJson()), cleanup.ownerIdentity);
  }
  std::vector<RepoUserStorageInventoryEntry> inventory;
  for (const auto& manifest : m_core.list())
    inventory.push_back({measurePersistentUserStorageObject(manifest),
      manifest.generation == 0 || manifest.operationId.empty()});
  for (const auto& pending : m_artifactMetadataStore->pendingUserStorageWrites(m_repoNode)) {
    const auto physical = std::find_if(inventory.begin(), inventory.end(), [&pending] (const auto& entry) {
      return entry.object.objectName == pending.object.objectName;
    });
    if (physical == inventory.end()) continue; // Unknown staging remains charged.
    if (physical->legacyMetadata) continue; // Incomplete metadata cannot prove publication.
    const auto& measured = physical->object;
    if (measured.wireSha256 != pending.object.wireSha256 ||
        measured.generation != pending.object.generation ||
        measured.payloadWireBytes != pending.object.payloadWireBytes)
      throw std::runtime_error("repo-user-storage-inventory-conflict: pending publication mismatch");
    (void)m_artifactMetadataStore->settleUserStorageReservation(m_repoNode,
      pending.grant.ownerIdentity, pending.operationId, measured.manifestBytes, nowMilliseconds());
    (void)m_artifactMetadataStore->commitRecoveredUserStorage(m_repoNode,
      pending.grant.ownerIdentity, pending.operationId, nowMilliseconds());
  }
  (void)m_artifactMetadataStore->reconcileUserStorageInventory(m_repoNode, inventory, nowMilliseconds());
}

RepoUserStorageWrite
RepoNode::reservePersistentUserStorage(const ArtifactReference& reference,
  const std::string& operationId, uint64_t generation,
  const std::string& authenticatedWriter, const RepoUserStorageBinding& binding)
{
  (void)binding.toJson();
  const auto manifest = artifactManifest(reference, operationId, generation);
  RepoUserStorageObject object{manifest.objectName, manifest.sha256, generation,
    reference.sizeBytes, manifest.toJson().size()};
  if (m_core.has(object.objectName)) {
    const auto stored = m_core.getManifest(object.objectName);
    if (stored.generation == 0 || stored.operationId.empty())
      throw std::runtime_error("repo-user-storage-reconciliation-required: incomplete legacy metadata");
  }
  (void)m_artifactMetadataStore->validateUserStorageGrant(
    authenticatedWriter, m_repoNode, binding.ownerIdentity, binding.grantId,
    object.objectName, binding.ownerEpoch, nowMilliseconds());
  const auto previous = m_artifactMetadataStore->lookupUserStorageWrite(
    m_repoNode, binding.ownerIdentity, operationId);
  if (previous) {
    if (previous->grant.grantId != binding.grantId || previous->grant.ownerEpoch != binding.ownerEpoch ||
        previous->object.objectName != object.objectName || previous->object.wireSha256 != object.wireSha256 ||
        previous->object.generation != generation || previous->object.payloadWireBytes != reference.sizeBytes)
      throw std::runtime_error("repo-user-storage-operation-conflict");
    object = previous->object;
  }
  else {
    if (m_core.has(object.objectName))
      object = measurePersistentUserStorageObject(m_core.getManifest(object.objectName));
  }
  auto result = m_artifactMetadataStore->reserveUserStorage(authenticatedWriter, m_repoNode,
    binding.ownerIdentity, binding.grantId, operationId, object, nowMilliseconds());
  if (result.state == RepoUserStorageWriteState::Cancelled)
    throw std::runtime_error("repo-user-storage-operation-conflict: cancelled write cannot restart");
  return result;
}

RepoUserStorageWrite
RepoNode::reservePersistentOriginalPacketStorage(
  const RepoObjectManifest& manifest, const std::string& operationId,
  const std::string& authenticatedWriter, const RepoUserStorageBinding& binding)
{
  if (!m_artifactMetadataStore || manifest.objectType != "original-packet-set" ||
      manifest.operationId != operationId || manifest.generation == 0 ||
      operationId.empty() || authenticatedWriter.empty()) {
    throw std::invalid_argument("repo-user-storage-original-packet-request-invalid");
  }
  (void)binding.toJson();

  RepoUserStorageObject object{
    ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::ALTERNATE),
    manifest.sha256, manifest.generation, manifest.size, manifest.toJson().size()};
  if (m_core.has(manifest.objectName)) {
    const auto existing = m_core.getManifest(manifest.objectName);
    if (existing.generation == 0 || existing.operationId.empty())
      throw std::runtime_error(
        "repo-user-storage-reconciliation-required: incomplete legacy metadata");
    if (existing.generation != manifest.generation)
      throw std::runtime_error("repo-user-storage-object-generation-conflict");
    auto normalized = manifest;
    normalized.operationId = existing.operationId;
    normalized.generation = existing.generation;
    if (normalized.publicationIdentity.empty() && existing.objectType == "original-packet-set")
      normalized.publicationIdentity = existing.publicationIdentity;
    if (normalized.toJson() != existing.toJson())
      throw std::runtime_error("repo-data-wire-conflict");
    object = measurePersistentUserStorageObject(existing, false);
  }

  // Revalidate after packet retrieval, immediately before reserving or
  // resuming durable storage for this original Producer packet set.
  (void)m_artifactMetadataStore->validateUserStorageGrant(
    authenticatedWriter, m_repoNode, binding.ownerIdentity, binding.grantId,
    object.objectName, binding.ownerEpoch, nowMilliseconds());
  const auto previous = m_artifactMetadataStore->lookupUserStorageWrite(
    m_repoNode, binding.ownerIdentity, operationId);
  if (previous) {
    if (previous->grant.grantId != binding.grantId ||
        previous->grant.ownerEpoch != binding.ownerEpoch ||
        previous->object.objectName != object.objectName ||
        previous->object.wireSha256 != object.wireSha256 ||
        previous->object.generation != object.generation ||
        previous->object.payloadWireBytes != object.payloadWireBytes ||
        previous->object.manifestBytes != object.manifestBytes) {
      throw std::runtime_error("repo-user-storage-operation-conflict");
    }
    object = previous->object;
  }
  auto result = m_artifactMetadataStore->reserveUserStorage(
    authenticatedWriter, m_repoNode, binding.ownerIdentity, binding.grantId,
    operationId, object, nowMilliseconds());
  if (result.state == RepoUserStorageWriteState::Cancelled)
    throw std::runtime_error("repo-user-storage-operation-conflict: cancelled write cannot restart");
  return result;
}

std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore>
RepoNode::createEncryptedLargeDataStore()
{
  if (!m_artifactMetadataStore)
    throw std::logic_error("repo-user-storage-persistent-journal-required");
  return std::make_shared<RepoEncryptedLargeDataStore>(
    m_coreOwner, m_artifactMetadataStore, m_repoNode, m_storeWriteMutexOwner);
}

RepoNode::~RepoNode()
{
  m_originalDataRouteReady.store(false);
  m_originalDataRoute.cancel();
  m_originalDataFilter.cancel();
  stopOriginalDataWorker();
  stopPublishOperationWorker();
  stopRangeReservationReaper();
}

void
RepoNode::registerOriginalDataServing(ndn::Face& face)
{
  if (m_providerIdentity.empty()) {
    throw std::logic_error("original Data serving requires a Provider identity route");
  }
  if (m_originalDataServingRegistered || m_originalDataWorker.joinable()) {
    throw std::logic_error("original Data serving is already registered");
  }
  {
    std::lock_guard<std::mutex> guard(m_originalDataMutex);
    if (m_ownedInsertStopping.load(std::memory_order_acquire) || m_stopOriginalDataWorker)
      throw std::logic_error("original Data serving is stopped");
  }

  const ndn::Name route(m_providerIdentity);
  const std::weak_ptr<RepoNode> weakSelf = shared_from_this();
  m_originalDataWorker = std::thread([this, &face] {
    try {
      runOriginalDataWorker(face);
    }
    catch (...) {
      m_originalDataRouteReady.store(false);
      requestOriginalDataWorkerStop();
      std::fprintf(stderr, "original Data worker stopped reason=worker-loop-exception\n");
    }
  });
  try {
  // A local-only filter accepts the original producer name. Registering /
  // with NFD would hijack unrelated traffic; only the distinct Repo identity
  // is advertised as a forwarding-hint route.
  m_originalDataFilter = face.setInterestFilter(
    ndn::InterestFilter(ndn::Name("/")),
    [weakSelf] (const ndn::InterestFilter&, const ndn::Interest& interest) {
      const auto self = weakSelf.lock();
      if (!self) return;
      self->enqueueOriginalDataInterest(interest);
    });
  m_originalDataRoute = face.registerPrefix(
    route,
    [weakSelf] (const ndn::Name&) {
      if (const auto self = weakSelf.lock()) {
        std::lock_guard<std::mutex> guard(self->m_originalDataMutex);
        if (!self->m_ownedInsertStopping.load(std::memory_order_acquire) &&
            !self->m_stopOriginalDataWorker)
          self->m_originalDataRouteReady.store(true);
      }
    },
    [weakSelf] (const ndn::Name& name, const std::string& error) {
      if (const auto self = weakSelf.lock()) {
        self->m_originalDataRouteReady.store(false);
        self->m_originalDataFilter.cancel();
        self->requestOriginalDataWorkerStop();
      }
      std::fprintf(stderr, "original Data route registration failed prefix=%s reason=%s\n",
                   name.toUri().c_str(), error.c_str());
    });
  m_originalDataServingRegistered = true;
  }
  catch (...) {
    m_originalDataRoute.cancel();
    m_originalDataFilter.cancel();
    stopOriginalDataWorker();
    throw;
  }
}

void
RepoNode::enqueueOriginalDataInterest(const ndn::Interest& interest)
{
  // 256 packets of at most MAX_NDN_PACKET_SIZE also bound retained wire bytes.
  const auto& wire = interest.wireEncode();
  if (wire.size() > ndn::MAX_NDN_PACKET_SIZE) return;
  std::lock_guard<std::mutex> guard(m_originalDataMutex);
  if (m_stopOriginalDataWorker || m_ownedInsertStopping.load(std::memory_order_acquire) ||
      m_originalDataPending.load() >= 256) return;
  // The Worker must not retain the Face's shared packet/name buffers.
  m_originalDataQueue.emplace_back(
    ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
  ++m_originalDataPending;
  m_originalDataWake.notify_one();
}

void
RepoNode::runOriginalDataWorker(ndn::Face& face)
{
  for (;;) {
    ndn::Interest interest;
    {
      std::unique_lock<std::mutex> guard(m_originalDataMutex);
      m_originalDataWake.wait(guard, [this] {
        return m_stopOriginalDataWorker || !m_originalDataQueue.empty();
      });
      if (m_stopOriginalDataWorker) return;
      interest = std::move(m_originalDataQueue.front());
      m_originalDataQueue.pop_front();
    }
    struct PendingRelease
    {
      std::atomic<size_t>& count;
      ~PendingRelease() { --count; }
    } release{m_originalDataPending};
    try {
      serveOriginalDataInterest(face, interest);
    }
    catch (const std::exception& error) {
      std::fprintf(stderr, "original Data worker failed reason=%s\n", error.what());
    }
    catch (...) {
      std::fprintf(stderr, "original Data worker failed reason=unknown-exception\n");
    }
  }
}

void
RepoNode::requestOriginalDataWorkerStop() noexcept
{
  {
    std::lock_guard<std::mutex> guard(m_originalDataMutex);
    m_stopOriginalDataWorker = true;
    m_originalDataRouteReady.store(false);
    m_originalDataPending.fetch_sub(m_originalDataQueue.size());
    m_originalDataQueue.clear();
  }
  m_originalDataWake.notify_all();
}

void
RepoNode::stopOriginalDataWorker() noexcept
{
  std::lock_guard<std::mutex> joinGuard(m_originalDataJoinMutex);
  requestOriginalDataWorkerStop();
  if (m_originalDataWorker.joinable()) m_originalDataWorker.join();
}

void
RepoNode::serveOriginalDataInterest(ndn::Face& face, const ndn::Interest& interest)
{
  auto packetName = interest.getName();
  if (!packetName.empty() && packetName[-1].isImplicitSha256Digest())
    packetName = packetName.getPrefix(-1);
  const auto name = packetName.toUri();
  try {
    // Retain the physical cleanup/claim gate until the original wire is posted.
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    const auto acceptObject = [this] (const RepoObjectManifest& object) {
      if (object.generation == 0 || object.operationId.empty() ||
          object.lifecycleState != "COMMITTED") return false;
      const auto measured = measurePersistentUserStorageObject(object, false);
      for (const auto& candidate : m_artifactMetadataStore->liveUserStorageClaimsForObject(
             m_repoNode, measured.objectName)) {
        if (candidate.state != RepoUserStorageClaimState::Committed) continue;
        const auto current = m_artifactMetadataStore->lookupCommittedUserStorageClaim(
          candidate.ownerIdentity, m_repoNode, measured.objectName, candidate.ownerEpoch);
        if (current && current->object.objectName == measured.objectName &&
            current->object.wireSha256 == measured.wireSha256 &&
            current->object.generation == measured.generation &&
            current->object.payloadWireBytes == measured.payloadWireBytes &&
            current->object.manifestBytes == measured.manifestBytes) return true;
      }
      return false;
    };
    const auto wire = m_artifactMetadataStore
      ? m_core.getDataPacket(interest, acceptObject) : m_core.getDataPacket(interest);
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    block.parse();
    const ndn::Data data(block);
    if (interest.matchesData(data)) {
      face.put(data);
      std::fprintf(stdout, "repo-original-data-served name=%s sha256=%s\n",
                   data.getName().toUri().c_str(), sha256Hex(wire).c_str());
      std::fflush(stdout);
    }
  }
  catch (const std::exception& error) {
    std::fprintf(stderr, "original Data serving failed name=%s reason=%s\n",
                 name.c_str(), error.what());
  }
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

void
RepoNode::finalizePublishOperation(PublishOperationStatus& status,
                                   const std::string& terminalState, uint64_t terminalAtMs)
{
  const auto operationId = status.operationId;
  const auto required = publishRequiredAcks(status.targets.size(), status.consistency);
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
}

bool
RepoNode::runPublishOperationPass(const std::string& operationId)
{
  std::lock_guard<std::mutex> passGuard(m_publishPassMutex);
  auto recoverable = m_artifactMetadataStore->recoverablePublishOperations();
  const auto operation = std::find_if(recoverable.begin(), recoverable.end(),
    [&operationId] (const auto& item) { return item.operationId == operationId; });
  if (operation == recoverable.end()) return false;
  const auto bindDestination = [&operation] (auto& request, const RepoArtifactTarget& target) {
    if (operation->intent.schemaVersion != 2) return;
    const auto binding = std::find_if(operation->intent.userStorageBindings.begin(),
      operation->intent.userStorageBindings.end(), [&target] (const auto& entry) { return entry.target == target; });
    if (binding == operation->intent.userStorageBindings.end())
      throw std::runtime_error("repo-user-storage-intent-mismatch: destination binding missing");
    request.schemaVersion = 3; request.storageBinding = binding->binding;
  };
  auto status = operation->status;
  status.validate(m_artifactLimits);
  if (m_publishUser == nullptr) return false;

  const auto validateSeedGrant = [this, &operation] {
    if (operation->intent.schemaVersion != 2) return;
    const auto& intent = operation->intent;
    const auto seed = std::find_if(intent.userStorageBindings.begin(),
      intent.userStorageBindings.end(), [&intent] (const auto& entry) {
        return entry.target == intent.seed;
      });
    if (seed == intent.userStorageBindings.end())
      throw std::runtime_error("repo-user-storage-intent-mismatch: seed binding missing");
    (void)m_artifactMetadataStore->validateUserStorageGrant(
      seed->writerIdentity, m_repoNode, seed->binding.ownerIdentity, seed->binding.grantId,
      artifactStorageObjectName(intent.reference), seed->binding.ownerEpoch, nowMilliseconds());
  };

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

  const auto finalize = [this, &status] (const std::string& terminalState, uint64_t terminalAtMs) {
    finalizePublishOperation(status, terminalState, terminalAtMs);
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
    validateSeedGrant();
    if (!m_core.has(objectName)) {
      throw std::runtime_error("repo-seed-source-missing: accepted seed bytes are absent");
    }
    const auto manifest = m_core.getManifest(objectName);
    verifyArtifactBytes(m_core, manifest, status.reference);
  }
  catch (...) {
    // The seed receipt is the acceptance barrier. If its grant or bytes become
    // unavailable before replica work starts, preserve that proven commit and
    // report the unmet placement requirement as PARTIAL.
    finalize(committedCount() > 0 ? "PARTIAL" : "FAILED", nowMilliseconds());
    return false;
  }

  const uint64_t chunkBytes = std::min<uint64_t>(
    m_artifactLimits.maxChunkBytes,
    m_publishUser->hasRequestScopedDefault() ? uint64_t{1024 * 1024} :
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
            validateSeedGrant();
            bytes = m_core.getRange(artifactStorageObjectName(status.reference),
                                    RepoByteRange{0, status.reference.sizeBytes});
          }
          readingSeed = false;
          ArtifactStoreRequest write;
          write.operationId = operationId;
          write.generation = status.generation;
          write.reference = status.reference;
          write.operationOwner = status.seed;
          bindDestination(write, targetStatus.target);
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
              validateSeedGrant();
              bytes = m_core.getRange(artifactStorageObjectName(status.reference),
                                      RepoByteRange{offset, length});
            }
            readingSeed = false;
            ArtifactRangeWriteRequest write;
            write.operationId = operationId;
            write.generation = status.generation;
            write.reference = status.reference;
            write.operationOwner = status.seed;
            bindDestination(write, targetStatus.target);
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
          bindDestination(commit, targetStatus.target);
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
          const bool uncertain = previousAttemptCount != 0 ||
            requestMayHaveBeenEmitted->load(std::memory_order_acquire);
          targetStatus.state = uncertain ? "UNKNOWN" : "FAILED";
          targetStatus.failureCode = uncertain
            ? "repo-target-outcome-unknown" : "repo-seed-source-invalid";
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
            // Retry an uncertain emission only while the seed grant is still
            // current. Owner cleanup may have revoked that grant while the
            // request was in flight; in that case preserve UNKNOWN for this
            // target and stop before issuing another replica write.
            bool seedAuthorityCurrent = true;
            try {
              std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
              validateSeedGrant();
            }
            catch (...) {
              seedAuthorityCurrent = false;
              ownerSourceFailed = true;
            }
            if (seedAuthorityCurrent) {
              // Keep the durable operation recoverable and replay the same
              // operation ID once before exposing an unknown terminal result.
              targetStatus.state = "IN_FLIGHT";
              retryForUncertainOutcome = true;
            }
            else {
              targetStatus.state = "UNKNOWN";
              targetStatus.failureCode = "repo-target-outcome-unknown";
            }
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
      // A later owner fence cannot revoke the already verified seed receipt.
      finalize(committedCount() > 0 ? "PARTIAL" : "FAILED", nowMilliseconds());
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

    // Resolve an emitted request's uncertain outcome with the same operation
    // ID before starting another replica write. Otherwise a later target can
    // be mutated while the first target's commit is still unknown.
    if (retryForUncertainOutcome) return true;
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
  // Standalone storage has no authenticated Provider receipt owner. Do not
  // forward its logical Repo ID as half of a persistent receipt identity.
  if (m_providerIdentity.empty()) {
    return m_core.putDataPacket(dataName, wire);
  }
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
  m_ownedPacketValidator = [&provider](
      const std::vector<uint8_t>& wire, const ndn::Name& producer,
      std::chrono::steady_clock::time_point deadline) {
    return provider.validateProducerPacketWireUntil(wire, producer, deadline);
  };
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
            const auto requester = requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE);
            std::fprintf(stderr, "SPEC192_REPO_REQUEST service=%s requester=%s\n",
                         traceServiceName.c_str(), requester.c_str());
          }
          return handler(requesterIdentity, providerName, resolvedServiceName,
                         requestId, request);
        };
      provider.addService(serviceName, std::move(serviceAck), std::move(retainedHandler));
    };

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "USER_STORAGE_GRANT"),
    [this](const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleUserStorageGrant(payloadOf(request),
          requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
      }
      catch (const std::exception& e) { return makeError(e.what()); }
    });
  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "USER_STORAGE_USAGE"),
    [this](const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleUserStorageUsage(payloadOf(request),
          requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
      }
      catch (const std::exception& e) { return makeError(e.what()); }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "USER_STORAGE_LIST"),
    [this](const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleUserStorageList(payloadOf(request),
          requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
      }
      catch (const std::exception& e) { return makeError(e.what()); }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "USER_STORAGE_DELETE"),
    [this](const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleUserStorageDelete(payloadOf(request),
          requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
      }
      catch (const std::exception& e) { return makeError(e.what()); }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE), true));
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
        return makeResponse(handleStoreRange(payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE), true));
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
        return makeResponse(handleCommitRanges(payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE), true));
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
        return makeResponse(handleAbortRanges(payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE), true));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "INSERT"),
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleInsert(
          payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE), true));
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
    [this] (const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request),
          requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
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
        return makeResponse(handleStatus(payloadOf(request), requesterIdentity.toUri(ndn::name::UriFormat::ALTERNATE)));
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
RepoNode::beginShutdown() noexcept
{
  m_ownedInsertStopping.store(true, std::memory_order_release);
  m_originalDataRouteReady.store(false);
  stopOriginalDataWorker();
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
  std::lock_guard<std::mutex> fetchGuard(m_dataReferenceFetchMutex);
  m_dataReferenceFetcher = std::move(fetcher);
  m_dataReferenceSpoolFetcher = {};
}

void
RepoNode::setDataReferenceSpoolFetcher(DataReferenceSpoolFetcher fetcher)
{
  std::lock_guard<std::mutex> fetchGuard(m_dataReferenceFetchMutex);
  m_dataReferenceSpoolFetcher = std::move(fetcher);
  m_dataReferenceFetcher = {};
}

RepoOperationStatus
RepoNode::insertWirePackets(const RepoDataReference& reference,
                            const std::vector<std::vector<uint8_t>>& wirePackets)
{
  return insertWirePacketsImpl(reference, wirePackets.size(),
    [&wirePackets] (std::size_t index) { return wirePackets.at(index); }, {},
    {}, 0, {}, std::nullopt);
}

RepoOperationStatus
RepoNode::insertWirePackets(const RepoUserStorageInsertRequest& request,
                            const std::vector<std::vector<uint8_t>>& wirePackets,
                            const std::string& authenticatedWriter)
{
  return insertWirePacketsImpl(request.reference, wirePackets.size(),
                               [&wirePackets] (std::size_t index) {
                                 return wirePackets.at(index);
                               }, {}, request.operationId,
                               request.generation, authenticatedWriter,
                               request.storageBinding);
}

RepoOperationStatus
RepoNode::insertWirePacketsImpl(
  const RepoDataReference& reference,
  std::size_t packetCount,
  const std::function<std::vector<uint8_t>(std::size_t)>& readPacket,
  const RepoCore::OriginalPacketRangeReader& sourceReadRange,
  const std::string& operationId, uint64_t generation,
  const std::string& authenticatedWriter,
  const std::optional<RepoUserStorageBinding>& storageBinding,
  const std::optional<std::string>& validatedWireSha256)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  if (reference.objectName.empty()) {
    throw std::invalid_argument("repo data reference objectName must not be empty");
  }

  reference.validateExactPacketSet();
  if (reference.packetSetMetadata &&
      packetCount != reference.packetSetMetadata->packetNames.size())
    throw std::invalid_argument("repo-exact-packet-count-mismatch");

  RepoOperationStatus status;
  status.operationId = operationId.empty() ? allocateOperationId() : operationId;
  status.operation = "INSERT";
  status.state = "STORING";
  status.objectName = reference.objectName;
  status.totalSegments = packetCount;
  status.message = "storing opaque Data wire packets";
  rememberStatus(status);

  try {
    if (!m_core.supportsOriginalPacketTransactions() || packetCount == 0 || !readPacket)
      throw std::runtime_error("repo-original-packet-transaction-unsupported-or-empty");
    auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("repo-packet-spool-initialization-failed");
    uint64_t totalSize = 0;
    uint64_t contentBytes = 0;
    uint64_t measuredEnvelopeBytes = 0;
    std::optional<ndn_service_framework::ProtectedBlobManifest> transportRoot;
    std::vector<uint64_t> packetOffsets{0};
    std::vector<std::string> packetNames;
    packetNames.reserve(packetCount);
    const ndn::Name expectedPrefix(reference.dataPrefix);
    if (reference.hasFinalSegment) {
      if (expectedPrefix.empty() || reference.finalSegment < reference.firstSegment) {
        throw std::invalid_argument(
          "repo-packet-set-invalid: requested segment range is invalid");
      }
      const auto lastOffset = reference.finalSegment - reference.firstSegment;
      if (lastOffset == std::numeric_limits<uint64_t>::max() ||
           lastOffset + 1 != packetCount) {
        throw std::invalid_argument(
          "repo-packet-set-invalid: packet count does not match requested segment range");
      }
    }
    if (storageBinding) {
      const ndn::Name writerPrefix(authenticatedWriter);
      if (!m_artifactMetadataStore || authenticatedWriter.empty() || writerPrefix.empty() ||
          !writerPrefix.isPrefixOf(expectedPrefix) || generation == 0 || operationId.empty()) {
        throw std::invalid_argument(
          "repo-user-storage-original-packet-writer-prefix-invalid");
      }
    }
    for (size_t i = 0; i < packetCount; ++i) {
      const auto packetWire = readPacket(i);
      if (packetWire.empty() || packetWire.size() > ndn::MAX_NDN_PACKET_SIZE ||
          totalSize > std::numeric_limits<uint64_t>::max() - packetWire.size())
        throw std::invalid_argument("repo-packet-set-invalid: packet size");
      totalSize += packetWire.size();
      ndn::Block block(ndn::span<const uint8_t>(packetWire.data(),
                                                packetWire.size()));
      block.parse();
      const ndn::Data data(block);
      if (data.wireEncode().size() != packetWire.size())
        throw std::invalid_argument("repo-packet-set-invalid: trailing bytes");
      contentBytes += data.getContent().value_size();
      if (reference.packetSetMetadata &&
          (reference.packetSetMetadata->envelopeBytes != 0 ||
           !reference.packetSetMetadata->dataPrefix.empty())) {
        const auto& content = data.getContent();
        if (i == 0) {
          transportRoot = ndn_service_framework::ProtectedBlobManifest::decode(
            std::vector<uint8_t>(content.value(), content.value() + content.value_size()),
            expectedPrefix, ndn::Name(reference.objectName));
          if (transportRoot->segmentCount == 0 || transportRoot->segmentCount >= packetCount)
            throw std::invalid_argument("repo-exact-envelope-segment-count-invalid");
        }
        else if (i <= transportRoot->segmentCount) {
          auto expectedName = transportRoot->dataPrefix;
          expectedName.appendSegment(i - 1);
          if (data.getName() != expectedName ||
              measuredEnvelopeBytes > transportRoot->envelopeBytes ||
              content.value_size() != std::min<uint64_t>(transportRoot->segmentBytes,
                transportRoot->envelopeBytes - measuredEnvelopeBytes))
            throw std::invalid_argument("repo-exact-envelope-index-invalid");
          measuredEnvelopeBytes += content.value_size();
        }
      }
      if (!expectedPrefix.isPrefixOf(data.getName())) {
        throw std::runtime_error("repo-packet-set-invalid: Data name is outside "
                                 "declared dataPrefix: " +
                                 data.getName().toUri());
      }
      if (reference.hasFinalSegment) {
        auto expectedName = expectedPrefix;
        expectedName.appendSegment(reference.firstSegment + static_cast<uint64_t>(i));
        if (data.getName() != expectedName) {
          throw std::runtime_error(
            "repo-packet-set-invalid: Data name does not match requested segment order");
        }
      }
      if (reference.packetSetMetadata &&
          data.getName() != ndn::Name(reference.packetSetMetadata->packetNames.at(i)))
        throw std::runtime_error("repo-exact-packet-order-mismatch");
      const auto dataName = data.getName().toUri(ndn::name::UriFormat::CANONICAL);
      if (std::find(packetNames.begin(), packetNames.end(), dataName) != packetNames.end()) {
        throw std::runtime_error(
          "repo-packet-set-invalid: duplicate NDN Data name: " + dataName);
      }
      if (m_core.hasDataPacket(dataName) &&
          m_core.getDataPacket(dataName) != packetWire) {
        throw std::runtime_error(
          "repo-data-wire-conflict: immutable NDN Data name conflict: " + dataName);
      }
      packetNames.push_back(dataName);
      packetOffsets.push_back(totalSize);
      if (EVP_DigestUpdate(digest.get(), packetWire.data(), packetWire.size()) != 1)
        throw std::runtime_error("repo-packet-digest-update-failed");
    }

    if (ndn::Name(reference.objectName) != ndn::Name(packetNames.front()))
      throw std::invalid_argument("repo-original-set-root-name-mismatch");
    if (transportRoot &&
        (measuredEnvelopeBytes != transportRoot->envelopeBytes ||
         reference.packetSetMetadata->envelopeBytes != measuredEnvelopeBytes ||
         ndn::Name(reference.packetSetMetadata->dataPrefix) != transportRoot->dataPrefix))
      throw std::invalid_argument("repo-exact-envelope-metadata-mismatch");
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned hashSize = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash, &hashSize) != 1)
      throw std::runtime_error("repo-packet-digest-finalization-failed");
    std::ostringstream hashText;
    for (unsigned i = 0; i < hashSize; ++i)
      hashText << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);

    if (validatedWireSha256 && hashText.str() != *validatedWireSha256) {
      status.state = "FAILED";
      status.message = "repo-original-packet-validation-source-changed";
      rememberStatus(status);
      return status;
    }

    if (reference.expectedSize != 0 && totalSize != reference.expectedSize) {
      status.state = "FAILED";
      status.message = "fetched wire packet size mismatch";
      rememberStatus(status);
      return status;
    }
    if (!reference.expectedSha256.empty() &&
        hashText.str() != reference.expectedSha256) {
      status.state = "FAILED";
      status.message = "fetched wire packet sha256 mismatch";
      rememberStatus(status);
      return status;
    }

    RepoObjectManifest manifest;
    manifest.objectName = ndn::Name(reference.objectName).toUri(ndn::name::UriFormat::CANONICAL);
    manifest.objectType = "original-packet-set";
    manifest.sha256 = hashText.str();
    manifest.size = totalSize;
    if (packetNames.size() > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("repo-packet-count-overflow");
    manifest.segmentCount = static_cast<uint32_t>(packetNames.size());
    manifest.packetNames = packetNames;
    manifest.packetOffsets = std::move(packetOffsets);
    manifest.envelopeBytes = contentBytes;
    manifest.dataPrefix = expectedPrefix.toUri(ndn::name::UriFormat::CANONICAL);
    manifest.operationId = status.operationId;
    if (reference.packetSetMetadata) {
      // Publication metadata is opaque. Physical index, digest, generation,
      // operation and quota measurements above remain owned by Repo.
      const auto& metadata = *reference.packetSetMetadata;
      manifest.publicationIdentity = metadata.publicationIdentity;
      manifest.protectionEpoch = metadata.protectionEpoch;
      manifest.keyReferenceId = metadata.keyReferenceId;
      manifest.keyReferenceVersion = metadata.keyReferenceVersion;
      manifest.ciphertextManifestDigest = metadata.ciphertextManifestDigest;
      manifest.servingLocator = metadata.servingLocator;
      manifest.contentDigest = metadata.contentDigest;
      manifest.plaintextSize = metadata.plaintextSize;
      if (transportRoot) {
        manifest.envelopeBytes = measuredEnvelopeBytes;
        manifest.dataPrefix = transportRoot->dataPrefix.toUri(ndn::name::UriFormat::CANONICAL);
      }
    }
    if (storageBinding) {
      manifest.generation = generation;
      (void)reservePersistentOriginalPacketStorage(
        manifest, status.operationId, authenticatedWriter, *storageBinding);
    }
    auto commitReadRange = sourceReadRange;
    if (!commitReadRange) {
      const auto& commitOffsets = manifest.packetOffsets;
      commitReadRange = [&readPacket, &commitOffsets] (uint64_t offset, std::size_t length) {
        if (offset > commitOffsets.back() || length > commitOffsets.back() - offset)
          throw std::out_of_range("repo-packet-source-range-invalid");
        std::vector<uint8_t> bytes(length);
        uint64_t cursor = offset;
        std::size_t copied = 0;
        while (copied < length) {
          auto packet = std::upper_bound(commitOffsets.begin(), commitOffsets.end(), cursor);
          if (packet == commitOffsets.begin() || packet == commitOffsets.end())
            throw std::runtime_error("repo-packet-source-index-invalid");
          const auto index = static_cast<std::size_t>(
            std::distance(commitOffsets.begin(), packet) - 1);
          const auto packetBegin = commitOffsets[index];
          const auto packetLength = commitOffsets[index + 1] - packetBegin;
          const auto withinPacket = cursor - packetBegin;
          if (withinPacket >= packetLength)
            throw std::runtime_error("repo-packet-source-index-invalid");
          const auto chunk = static_cast<std::size_t>(std::min<uint64_t>(
            packetLength - withinPacket, length - copied));
          const auto packetWire = readPacket(index);
          if (packetWire.size() != packetLength)
            throw std::runtime_error("repo-packet-source-size-changed");
          std::copy_n(packetWire.begin() + static_cast<std::ptrdiff_t>(withinPacket),
                      chunk, bytes.begin() + static_cast<std::ptrdiff_t>(copied));
          copied += chunk;
          cursor += chunk;
        }
        return bytes;
      };
    }
    const auto storedManifest = m_core.commitOriginalPacketReader(manifest, commitReadRange);

    if (storageBinding) {
      const auto measured = measurePersistentUserStorageObject(storedManifest);
      auto admitted = m_artifactMetadataStore->lookupUserStorageWrite(
        m_repoNode, storageBinding->ownerIdentity, status.operationId);
      if (!admitted)
        throw std::runtime_error("repo-user-storage-operation-missing");
      if (admitted->state == RepoUserStorageWriteState::Reserved) {
        (void)m_artifactMetadataStore->settleUserStorageReservation(
          m_repoNode, storageBinding->ownerIdentity, status.operationId,
          measured.manifestBytes, nowMilliseconds());
      }
      else if (admitted->object.objectName != measured.objectName ||
               admitted->object.wireSha256 != measured.wireSha256 ||
               admitted->object.generation != measured.generation ||
               admitted->object.payloadWireBytes != measured.payloadWireBytes ||
               admitted->object.manifestBytes != measured.manifestBytes) {
        throw std::runtime_error(
          "repo-user-storage-inventory-conflict: committed original packet set changed");
      }
      (void)m_artifactMetadataStore->commitUserStorage(
        m_repoNode, storageBinding->ownerIdentity, status.operationId, nowMilliseconds());
    }

    status.state = "DONE";
    status.message = "stored app-owned segmented Data wire packets";
    status.completedSegments = packetCount;
    status.totalSegments = packetCount;
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
                                          bool forceRemoteOwnerQuery,
                                          const std::optional<RepoUserStorageBinding>& storageBinding) const
{
  if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error(
      "repo-artifact-identity-unconfigured: Persistent receipt owner is unavailable");
  }
  if (requesterIdentity.empty()) {
    throw std::runtime_error(
      "repo-publish-intent-required: Persistent writes require an authenticated principal");
  }
  reference.validateProducerOwnership();
  if (!operationOwner && ndn::Name(reference.publisherIdentity) != ndn::Name(requesterIdentity)) {
    throw std::runtime_error(
      "repo-publish-producer-mismatch: authenticated publisher must own the artifact namespace");
  }
  if (!storageBinding) {
    throw std::runtime_error(
      "repo-user-storage-binding-required: persistent writes require an owner grant binding");
  }

  if (operationOwner) {
    validateCanonicalRepoName(operationOwner->repoNode, m_artifactLimits,
                              "operationOwner.repoNode");
    validateCanonicalRepoName(operationOwner->providerIdentity, m_artifactLimits,
                              "operationOwner.providerIdentity");
    validateCanonicalRepoName(operationOwner->servicePrefix, m_artifactLimits,
                              "operationOwner.servicePrefix");
    if (ndn::Name(requesterIdentity) != ndn::Name(operationOwner->providerIdentity) ||
        ndn::Name(operationOwner->repoNode) == ndn::Name(m_repoNode) ||
        ndn::Name(operationOwner->providerIdentity) == ndn::Name(m_providerIdentity)) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unauthorized: replica write must come from another authenticated seed Provider");
    }

    const RepoArtifactTarget localTarget{
      m_repoNode, m_providerIdentity, m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
    constexpr auto CacheLifetime = std::chrono::seconds(30);
    constexpr size_t MaxCacheEntries = 256;
    const auto matchesBinding = [&] (const VerifiedRemotePublishBinding& binding) {
      return binding.reference == reference && binding.generation == generation &&
             binding.owner == *operationOwner && binding.target == localTarget &&
             binding.storageBinding.has_value() == storageBinding.has_value() &&
             (!storageBinding || binding.storageBinding->toJson() == storageBinding->toJson());
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

    if (configuredAckMs <= 0 || configuredTimeoutMs <= 1) {
      throw std::runtime_error(
        "repo-publish-replica-owner-unverified: target Provider query deadline is invalid");
    }
    const int ackCollectionMs = std::min(configuredAckMs, configuredTimeoutMs - 1);
    // Preserve the configured ACK window plus a bounded response allowance,
    // without exceeding the configured request ceiling or overflowing int.
    const int timeoutMs = ackCollectionMs +
      std::min(2000, configuredTimeoutMs - ackCollectionMs);
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
    if (status.schemaVersion == 2) {
      if (!storageBinding)
        throw std::runtime_error("repo-user-storage-binding-required: owned replica cannot use an unbound write");
      const auto bound = std::find_if(status.userStorageBindings.begin(), status.userStorageBindings.end(),
        [&localTarget] (const auto& entry) { return entry.target == localTarget; });
      if (bound == status.userStorageBindings.end() ||
          bound->binding.toJson() != storageBinding->toJson() ||
          bound->writerIdentity != ndn::Name(requesterIdentity).toUri(ndn::name::UriFormat::ALTERNATE))
        throw std::runtime_error("repo-user-storage-intent-mismatch: replica binding differs from accepted seed status");
    }
    else if (storageBinding)
      throw std::runtime_error("repo-user-storage-intent-mismatch: owned replica requires bound seed status");
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
      std::chrono::steady_clock::now(), storageBinding};
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
    if (operation->intent.schemaVersion == 2) {
      if (!storageBinding)
        throw std::runtime_error("repo-user-storage-binding-required: owned intent cannot authorize an unbound write");
      const auto expected = std::find_if(operation->intent.userStorageBindings.begin(),
        operation->intent.userStorageBindings.end(), [&operation] (const auto& entry) {
          return entry.target == operation->intent.seed;
        });
      if (expected == operation->intent.userStorageBindings.end() ||
          expected->binding.toJson() != storageBinding->toJson() ||
          expected->writerIdentity != ndn::Name(requesterIdentity).toUri(ndn::name::UriFormat::ALTERNATE))
        throw std::runtime_error("repo-user-storage-intent-mismatch: binding differs from accepted seed intent");
    }
    else if (storageBinding)
      throw std::runtime_error("repo-user-storage-intent-mismatch: bound write requires an owned intent");
    const RepoArtifactTarget expectedSeed{
      m_repoNode, m_providerIdentity, m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
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
  static constexpr uint8_t MAGIC_V3[] = {'A', 'R', 'V', '2', 'P', 'U', 'T', '3'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()) ||
       std::equal(std::begin(MAGIC_V3), std::end(MAGIC_V3), request.begin()))) {
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
                                    write.operationOwner, true, false, write.storageBinding);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner, false, false, write.storageBinding);
    const auto manifest = artifactManifest(write.reference, write.operationId,
                                           write.generation);
    if (write.storageBinding)
      (void)reservePersistentUserStorage(write.reference, write.operationId, write.generation,
        requesterIdentity, *write.storageBinding);
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
    commit.schemaVersion = write.schemaVersion;
    commit.storageBinding = write.storageBinding;
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
  static constexpr uint8_t MAGIC_V3[] = {'A', 'R', 'V', '2', 'R', 'N', 'G', '3'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()) ||
       std::equal(std::begin(MAGIC_V3), std::end(MAGIC_V3), request.begin()))) {
    const auto write = ArtifactRangeWriteRequest::fromWire(request, m_artifactLimits);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner, true, false, write.storageBinding);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation, requesterIdentity,
                                    write.operationOwner, false, false, write.storageBinding);
    auto manifest = artifactManifest(write.reference, write.operationId, write.generation);
    if (write.storageBinding)
      (void)reservePersistentUserStorage(write.reference, write.operationId, write.generation,
        requesterIdentity, *write.storageBinding);
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
  static constexpr uint8_t MAGIC_V3[] = {'A', 'R', 'V', '2', 'C', 'O', 'M', '3'};
  if (request.size() >= sizeof(MAGIC_V1) &&
      (std::equal(std::begin(MAGIC_V1), std::end(MAGIC_V1), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()) ||
       std::equal(std::begin(MAGIC_V3), std::end(MAGIC_V3), request.begin()))) {
    const auto commit = ArtifactRangeCommitRequest::fromWire(request, m_artifactLimits);
    validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                    commit.generation, requesterIdentity,
                                    commit.operationOwner, true, false, commit.storageBinding);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                    commit.generation, requesterIdentity,
                                    commit.operationOwner, false, false, commit.storageBinding);
    if (commit.storageBinding)
      (void)reservePersistentUserStorage(commit.reference, commit.operationId, commit.generation,
        requesterIdentity, *commit.storageBinding);
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
  const bool hadManifest = m_core.has(manifest.objectName);
  if (!hadManifest) {
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
  if (commit.storageBinding && hadManifest &&
      !stored.operationId.empty() && stored.generation != 0) {
    const auto admitted = m_artifactMetadataStore->lookupUserStorageWrite(
      m_repoNode, commit.storageBinding->ownerIdentity, commit.operationId);
    if (!admitted)
      throw std::runtime_error("repo-user-storage-operation-missing");
    const auto before = measurePersistentUserStorageObject(stored);
    if (admitted->state == RepoUserStorageWriteState::Committed &&
        admitted->object.manifestBytes != before.manifestBytes)
      throw std::runtime_error("repo-user-storage-inventory-conflict: committed manifest size changed");
    // A visible row may be left by a failed directory fsync. Confirm durability
    // before changing owner accounting or publishing a commit receipt.
    stored = m_core.commitRanges(stored);
  }
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
                                    commit.operationOwner, true, true,
                                    commit.storageBinding);
  }

  RepoArtifactCommitReceipt receipt;
  receipt.repoNode = m_repoNode;
  if (commit.storageBinding) {
    const auto measured = measurePersistentUserStorageObject(stored);
    auto admitted = m_artifactMetadataStore->lookupUserStorageWrite(
      m_repoNode, commit.storageBinding->ownerIdentity, commit.operationId);
    if (!admitted)
      throw std::runtime_error("repo-user-storage-operation-missing");
    if (admitted->state == RepoUserStorageWriteState::Reserved) {
      (void)m_artifactMetadataStore->settleUserStorageReservation(m_repoNode,
        commit.storageBinding->ownerIdentity, commit.operationId, measured.manifestBytes, nowMilliseconds());
    }
    else if (admitted->object.manifestBytes != measured.manifestBytes)
      throw std::runtime_error("repo-user-storage-inventory-conflict: committed manifest size changed");
    (void)m_artifactMetadataStore->commitUserStorage(m_repoNode,
      commit.storageBinding->ownerIdentity, commit.operationId, nowMilliseconds());
  }
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
  receipt.forwardingHint = m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE);
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
  static constexpr uint8_t MAGIC_V2[] = {'A', 'R', 'V', '2', 'A', 'B', 'T', '2'};
  if (request.size() >= sizeof(MAGIC) &&
      (std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin()) ||
       std::equal(std::begin(MAGIC_V2), std::end(MAGIC_V2), request.begin()))) {
    if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
      throw std::runtime_error(
        "repo-artifact-identity-unconfigured: Persistent range owner is unavailable");
    }
    const auto abort = ArtifactRangeAbortRequest::fromWire(request, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(abort.reference, abort.operationId,
                                    abort.generation, requesterIdentity,
                                    abort.operationOwner, false, false, abort.storageBinding);
    const auto manifest = artifactManifest(abort.reference, abort.operationId,
                                           abort.generation);
    if (abort.storageBinding) {
      const auto write = m_artifactMetadataStore->lookupUserStorageWrite(
        m_repoNode, abort.storageBinding->ownerIdentity, abort.operationId);
      if (!write || write->grant.grantId != abort.storageBinding->grantId ||
          write->grant.ownerEpoch != abort.storageBinding->ownerEpoch ||
          write->object.objectName != manifest.objectName || write->object.generation != abort.generation ||
          write->object.wireSha256 != manifest.sha256 || write->object.payloadWireBytes != abort.reference.sizeBytes)
        throw std::runtime_error("repo-user-storage-operation-conflict");
      if (write->state == RepoUserStorageWriteState::Committed)
        throw std::runtime_error("repo-user-storage-operation-conflict: committed bytes cannot be aborted");
      if (write->state == RepoUserStorageWriteState::Cancelled) return toBytes("aborted");
      if (!m_artifactMetadataStore->userStorageReconciliation(m_repoNode).ready)
        return toBytes("pending"); // Restart quarantine is not live rollback confirmation.
      if (!m_core.abortFailedRangesIfOwned(manifest)) return toBytes("pending");
      (void)m_artifactMetadataStore->cancelUserStorage(m_repoNode,
        abort.storageBinding->ownerIdentity, abort.operationId, nowMilliseconds());
    }
    else (void)m_core.abortRangesIfOwned(manifest);
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

  std::lock_guard<std::mutex> fetchGuard(m_dataReferenceFetchMutex);
  if (m_dataReferenceFetcher || m_dataReferenceSpoolFetcher) {
    try {
      RepoOperationStatus storedStatus;
      if (m_dataReferenceSpoolFetcher) {
        auto packetSpool = m_dataReferenceSpoolFetcher(reference);
        if (!packetSpool) {
          throw std::runtime_error("repo-original-packet-fetcher-returned-empty-spool");
        }
        storedStatus = insertWirePacketsImpl(
          reference, packetSpool->packetCount(),
          [packetSpool] (std::size_t index) { return packetSpool->readPacket(index); },
          [packetSpool] (uint64_t offset, std::size_t length) {
            return packetSpool->readRange(offset, length);
          },
          {}, 0, {}, std::nullopt);
      }
      else {
        const auto wirePackets = m_dataReferenceFetcher(reference);
        storedStatus = insertWirePackets(reference, wirePackets);
      }
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
RepoNode::handleInsert(const std::vector<uint8_t>& request,
                       const std::string& requesterIdentity,
                       bool networkRequest)
{
  static constexpr uint8_t OWNED_MAGIC[] = {'R', 'P', 'O', 'I', '1', '\n'};
  const bool isOwnedRequest = request.size() >= sizeof(OWNED_MAGIC) &&
    std::equal(std::begin(OWNED_MAGIC), std::end(OWNED_MAGIC), request.begin());
  if (!isOwnedRequest) {
    if (networkRequest && m_artifactMetadataStore)
      throw std::runtime_error("repo-user-storage-binding-required: INSERT");
    return handleInsert(request);
  }

  if (!networkRequest || !m_artifactMetadataStore)
    throw std::runtime_error("repo-user-storage-persistent-repo-required: INSERT");
  const auto owned = decodeUserStorageInsertRequest(request);
  owned.reference.validateExactPacketSet();
  if (requesterIdentity.empty() || owned.reference.objectName.empty() ||
      owned.reference.dataPrefix.empty() ||
      (!owned.reference.packetSetMetadata && (!owned.reference.hasFinalSegment ||
       owned.reference.finalSegment < owned.reference.firstSegment)) ||
      owned.reference.expectedSize == 0 ||
      owned.reference.expectedSize > REPO_USER_STORAGE_LIMIT_BYTES ||
      (!owned.reference.packetSetMetadata &&
       owned.reference.finalSegment - owned.reference.firstSegment >= owned.reference.expectedSize)) {
    throw std::invalid_argument("repo-user-storage-insert-reference-invalid-or-unbounded");
  }

  const ndn::Name writerPrefix(requesterIdentity);
  const ndn::Name dataPrefix(owned.reference.dataPrefix);
  const ndn::Name objectName(owned.reference.objectName);
  if (writerPrefix.empty() || dataPrefix.empty() || objectName.empty() ||
      !writerPrefix.isPrefixOf(dataPrefix) || !dataPrefix.isPrefixOf(objectName)) {
    throw std::runtime_error("repo-user-storage-insert-producer-prefix-unauthorized");
  }

  const auto ledgerObjectName = objectName.toUri(ndn::name::UriFormat::ALTERNATE);
  try {
    if (m_ownedInsertStopping.load(std::memory_order_acquire)) {
      throw std::runtime_error("repo-original-packet-insert-shutting-down");
    }
    int validationTimeoutMs = 0;
    {
      std::lock_guard<std::mutex> timeoutGuard(m_publishWorkerMutex);
      validationTimeoutMs = m_publishRequestTimeoutMs;
    }
    std::lock_guard<std::mutex> fetchGuard(m_dataReferenceFetchMutex);
    if (m_ownedInsertStopping.load(std::memory_order_acquire)) {
      throw std::runtime_error("repo-original-packet-insert-shutting-down");
    }
    // The live grant is checked after any wait for the serialized fetch path,
    // and before looking up a resumable operation or fetching Producer data.
    (void)m_artifactMetadataStore->validateUserStorageGrant(
      requesterIdentity, m_repoNode, owned.storageBinding.ownerIdentity,
      owned.storageBinding.grantId, ledgerObjectName,
      owned.storageBinding.ownerEpoch, nowMilliseconds());
    const auto previous = m_artifactMetadataStore->lookupUserStorageWrite(
      m_repoNode, owned.storageBinding.ownerIdentity, owned.operationId);
    if (previous) {
      if (previous->grant.grantId != owned.storageBinding.grantId ||
          previous->grant.ownerEpoch != owned.storageBinding.ownerEpoch ||
          previous->object.objectName != ledgerObjectName ||
          previous->object.generation != owned.generation) {
        throw std::runtime_error("repo-user-storage-operation-conflict");
      }
    }
    if (!m_dataReferenceFetcher && !m_dataReferenceSpoolFetcher)
      throw std::runtime_error("repo-original-packet-fetcher-unavailable");
    const auto validationDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::max(1, validationTimeoutMs));
    if (!m_ownedPacketValidator) {
      throw std::runtime_error("repo-original-packet-validator-unavailable");
    }
    const auto producerPrefix = ndn::Name(requesterIdentity);
    const auto validateSource = [this, &producerPrefix, validationDeadline](
        std::size_t packetCount,
        const std::function<std::vector<uint8_t>(std::size_t)>& readPacket) {
      auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("repo-packet-validation-digest-initialization-failed");
      }
      for (std::size_t i = 0; i < packetCount; ++i) {
        if (m_ownedInsertStopping.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= validationDeadline) {
          throw std::runtime_error("repo-original-packet-validation-deadline-or-shutdown");
        }
        const auto packetWire = readPacket(i);
        if (packetWire.empty() || packetWire.size() > ndn::MAX_NDN_PACKET_SIZE) {
          throw std::runtime_error("repo-original-packet-validation-wire-size-invalid");
        }
        if (!m_ownedPacketValidator(packetWire, producerPrefix, validationDeadline)) {
          throw std::runtime_error("repo-original-packet-signature-unauthorized-or-invalid");
        }
        if (EVP_DigestUpdate(digest.get(), packetWire.data(), packetWire.size()) != 1) {
          throw std::runtime_error("repo-packet-validation-digest-update-failed");
        }
      }
      unsigned char hash[EVP_MAX_MD_SIZE];
      unsigned hashSize = 0;
      if (EVP_DigestFinal_ex(digest.get(), hash, &hashSize) != 1) {
        throw std::runtime_error("repo-packet-validation-digest-finalization-failed");
      }
      std::ostringstream text;
      for (unsigned i = 0; i < hashSize; ++i) {
        text << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);
      }
      return text.str();
    };
    if (m_dataReferenceSpoolFetcher) {
      const auto packetSpool = m_dataReferenceSpoolFetcher(owned.reference);
      if (!packetSpool) {
        throw std::runtime_error("repo-original-packet-fetcher-returned-empty-spool");
      }
      const auto readPacket = [packetSpool] (std::size_t index) {
        return packetSpool->readPacket(index);
      };
      const auto validatedSha256 = validateSource(packetSpool->packetCount(), readPacket);
      return toBytes(insertWirePacketsImpl(
        owned.reference, packetSpool->packetCount(),
        readPacket,
        [packetSpool] (uint64_t offset, std::size_t length) {
          return packetSpool->readRange(offset, length);
        },
        owned.operationId, owned.generation, requesterIdentity,
        owned.storageBinding, validatedSha256).toJson());
    }
    const auto wirePackets = m_dataReferenceFetcher(owned.reference);
    const auto readPacket = [&wirePackets] (std::size_t index) {
      return wirePackets.at(index);
    };
    const auto validatedSha256 = validateSource(wirePackets.size(), readPacket);
    return toBytes(insertWirePacketsImpl(
      owned.reference, wirePackets.size(), readPacket, {}, owned.operationId,
      owned.generation, requesterIdentity, owned.storageBinding,
      validatedSha256).toJson());
  }
  catch (const std::exception& e) {
    RepoOperationStatus status;
    status.operationId = owned.operationId;
    status.operation = "INSERT";
    status.state = "FAILED";
    status.objectName = owned.reference.objectName;
    status.totalSegments = owned.reference.finalSegment - owned.reference.firstSegment + 1;
    status.message = e.what();
    rememberStatus(status);
    return toBytes(status.toJson());
  }
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
RepoNode::handleManifest(const std::vector<uint8_t>& request,
                        const std::string& authenticatedWriter) const
{
  static constexpr uint8_t OWNED_MAGIC[] = {'R', 'P', 'O', 'I', '1', '\n'};
  if (request.size() >= sizeof(OWNED_MAGIC) &&
      std::equal(std::begin(OWNED_MAGIC), std::end(OWNED_MAGIC), request.begin())) {
    if (authenticatedWriter.empty() || !m_artifactMetadataStore)
      throw std::runtime_error("repo-owned-manifest-authentication-required");
    const auto query = decodeUserStorageInsertRequest(request);
    const auto objectName = ndn::Name(query.reference.objectName)
      .toUri(ndn::name::UriFormat::ALTERNATE);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    (void)m_artifactMetadataStore->validateUserStorageGrant(
      authenticatedWriter, m_repoNode, query.storageBinding.ownerIdentity,
      query.storageBinding.grantId, objectName, query.storageBinding.ownerEpoch,
      nowMilliseconds());
    const auto claim = m_artifactMetadataStore->lookupCommittedUserStorageClaim(
      query.storageBinding.ownerIdentity, m_repoNode, objectName,
      query.storageBinding.ownerEpoch);
    const auto manifest = m_core.getManifest(objectName);
    const auto measured = measurePersistentUserStorageObject(manifest, false);
    if (!claim || claim->object.generation != query.generation ||
        claim->object.wireSha256 != manifest.sha256 ||
        claim->object.payloadWireBytes != manifest.size ||
        claim->object.generation != manifest.generation ||
        claim->object.wireSha256 != measured.wireSha256 ||
        claim->object.payloadWireBytes != measured.payloadWireBytes ||
        claim->object.manifestBytes != measured.manifestBytes)
      throw std::runtime_error("DURABLE_OWNER_CLAIM_UNAVAILABLE");
    requirePersistentArtifactVisible(manifest);
    return toBytes(manifest.toJson());
  }
  requirePersistentArtifactVisible(m_core.getManifest(toString(request)));
  return m_core.handleManifest(request);
}

std::vector<uint8_t>
RepoNode::handleDataReceipt(const std::vector<uint8_t>& request) const
{
  if (m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error("repo-data-receipt-object-mode-required");
  }
  auto query = RepoDataPacketQuery::fromJson(toString(request));
  if (ndn::Name(query.dataName).toUri(ndn::name::UriFormat::CANONICAL) != query.dataName &&
      ndn::Name(query.dataName).toUri(ndn::name::UriFormat::ALTERNATE) != query.dataName) {
    throw std::invalid_argument("repo-data-query-name-not-canonical");
  }
  query.dataName = ndn::Name(query.dataName).toUri(ndn::name::UriFormat::CANONICAL);
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
  auto query = RepoDataPacketQuery::fromJson(toString(request));
  if (ndn::Name(query.dataName).toUri(ndn::name::UriFormat::CANONICAL) != query.dataName &&
      ndn::Name(query.dataName).toUri(ndn::name::UriFormat::ALTERNATE) != query.dataName) {
    throw std::invalid_argument("repo-data-query-name-not-canonical");
  }
  query.dataName = ndn::Name(query.dataName).toUri(ndn::name::UriFormat::CANONICAL);
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
  capability.formatVersions = {"artifact-manifest-v2", "exact-packet-v1", "raw-file-v1"};
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
      const auto expectedService = m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE);
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
      if (query.schemaVersion == 2 && requesterIdentity.empty())
        throw std::runtime_error("repo-owned-availability-unauthorized: authenticated owner required");
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
      const auto ownedClaim = query.schemaVersion == 2
        ? m_artifactMetadataStore->lookupCommittedUserStorageClaim(requesterIdentity,
            m_repoNode, artifactStorageObjectName(query.reference), query.ownerEpoch)
        : std::optional<RepoUserStorageClaim>{};
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
        location->forwardingHint == m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE) &&
        m_artifactMetadataStore->hasPersistentCommitReceipt(
          query.reference, m_repoNode, m_providerIdentity,
          expected.operationId, expected.receiptId, expected.generation);
      if (query.schemaVersion == 2 && !ownedClaim)
        report.reason = "current owner claim is unavailable";
      if (currentTuple && (query.schemaVersion == 1 || ownedClaim)) {
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
                   !probe.payloadMetadataAvailable ||
                   (ownedClaim && (!probe.manifestMeasurementAvailable ||
                     ownedClaim->repoNode != m_repoNode ||
                     ownedClaim->ownerIdentity != requesterIdentity ||
                     ownedClaim->ownerEpoch != query.ownerEpoch ||
                     ownedClaim->state != RepoUserStorageClaimState::Committed ||
                     ownedClaim->object.objectName != probe.manifest.objectName ||
                     ownedClaim->object.wireSha256 != probe.manifest.sha256 ||
                     ownedClaim->object.generation != expected.generation ||
                     ownedClaim->object.payloadWireBytes != query.reference.sizeBytes ||
                     ownedClaim->object.manifestBytes != probe.manifestWireBytes))) {
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
      response.schemaVersion = query.schemaVersion;
      if (query.schemaVersion == 2) {
        response.ownerIdentity = requesterIdentity;
        response.ownerEpoch = query.ownerEpoch;
      }
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
  intent.reference.validateProducerOwnership();
  if (intent.reference.toJson() != intent.reference.canonicalized().toJson()) {
    throw std::runtime_error(
      "repo-publish-reference-noncanonical: normalize reference before binding its request digest");
  }
  if (ndn::Name(intent.reference.publisherIdentity) != ndn::Name(requesterIdentity)) {
    throw std::runtime_error(
      "repo-publish-producer-mismatch: authenticated publisher must own the artifact namespace");
  }
  if (ndn::Name(intent.seed.repoNode) != ndn::Name(m_repoNode) ||
      ndn::Name(intent.seed.providerIdentity) != ndn::Name(m_providerIdentity) ||
      ndn::Name(intent.seed.servicePrefix) != m_servicePrefix) {
    throw std::runtime_error(
      "repo-publish-seed-mismatch: intent must be submitted to its exact selected seed tuple");
  }
  if (intent.schemaVersion != 2) {
    throw std::runtime_error(
      "repo-user-storage-binding-required: persistent publish intent requires owner grant bindings");
  }
  const auto operationId = makePublishOperationId(
    requesterIdentity, intent.seed, intent.idempotencyKeyDigest);
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  if (intent.schemaVersion == 2) {
    const auto localBinding = std::find_if(intent.userStorageBindings.begin(),
      intent.userStorageBindings.end(), [&intent] (const auto& entry) {
        return entry.target == intent.seed;
      });
    // validate() guarantees one exact seed binding. Other nodes validate their
    // own live grant; this seed cannot assert a remote journal's authority.
    (void)m_artifactMetadataStore->validateUserStorageGrant(
      ndn::Name(requesterIdentity).toUri(ndn::name::UriFormat::ALTERNATE), m_repoNode,
      localBinding->binding.ownerIdentity, localBinding->binding.grantId,
      artifactStorageObjectName(intent.reference), localBinding->binding.ownerEpoch,
      nowMilliseconds());
  }
  return toBytes(m_artifactMetadataStore->reservePublishOperation(
    operationId, requesterIdentity, intent, nowMilliseconds()));
}

std::vector<uint8_t>
RepoNode::handleUserStorageGrant(const std::vector<uint8_t>& request,
                                 const std::string& authenticatedOwner)
{
  if (!m_artifactMetadataStore)
    throw std::runtime_error("repo-user-storage-unavailable");
  const auto parsed = RepoUserStorageGrantRequest::fromJson(toString(request));
  const auto grant = m_artifactMetadataStore->grantUserStorage(authenticatedOwner,
    m_repoNode, parsed.grantId, parsed.writerIdentity, parsed.objectName,
    parsed.expiresAtMs, nowMilliseconds());
  return toBytes(RepoUserStorageGrantReply{1, grant}.toJson());
}

std::vector<uint8_t>
RepoNode::handleUserStorageUsage(const std::vector<uint8_t>& request,
                                 const std::string& authenticatedOwner) const
{
  if (!m_artifactMetadataStore)
    throw std::runtime_error("repo-user-storage-unavailable");
  (void)RepoUserStorageUsageRequest::fromJson(toString(request));
  return toBytes(RepoUserStorageUsageReply{1,
    m_artifactMetadataStore->userStorageUsage(authenticatedOwner, m_repoNode)}.toJson());
}

std::vector<uint8_t>
RepoNode::handleUserStorageList(const std::vector<uint8_t>& request,
                                const std::string& authenticatedOwner) const
{
  if (!m_artifactMetadataStore)
    throw std::runtime_error("repo-user-storage-unavailable");
  const auto parsed = RepoUserStorageListRequest::fromJson(toString(request));
  RepoUserStorageListReply reply{1, m_repoNode, authenticatedOwner,
    m_artifactMetadataStore->listUserStorage(authenticatedOwner, m_repoNode,
      parsed.afterObjectName, parsed.limit), {}};
  bool hasNext = reply.objects.size() == parsed.limit;
  for (;;) {
    reply.nextAfterObjectName = hasNext && !reply.objects.empty()
      ? reply.objects.back().object.objectName : std::string{};
    try { return toBytes(reply.toJson()); }
    catch (const std::length_error&) {
      if (reply.objects.size() <= 1) throw;
      reply.objects.resize(reply.objects.size() / 2);
      hasNext = true;
    }
  }
}

std::vector<uint8_t>
RepoNode::handleUserStorageDelete(const std::vector<uint8_t>& request,
                                  const std::string& authenticatedOwner)
{
  if (!m_artifactMetadataStore)
    throw std::runtime_error("repo-user-storage-unavailable");
  const auto parsed = RepoUserStorageDeleteRequest::fromJson(toString(request));
  std::unique_lock<std::mutex> passGuard(m_publishPassMutex, std::try_to_lock);
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  const auto claim = m_artifactMetadataStore->prepareUserStorageCleanup(
    authenticatedOwner, m_repoNode, parsed.cleanupId, parsed.objectName,
    parsed.ownerEpoch, nowMilliseconds());
  RepoUserStorageDeleteReply reply{1, m_repoNode, authenticatedOwner,
    parsed.cleanupId, parsed.objectName, parsed.ownerEpoch, "PENDING", 0};
  if (claim.state == RepoUserStorageClaimState::Deleted) {
    reply.state = "DELETED";
    return toBytes(reply.toJson());
  }
  {
    std::lock_guard<std::mutex> bindingGuard(m_remotePublishBindingMutex);
    for (auto it = m_remotePublishBindings.begin(); it != m_remotePublishBindings.end();) {
      const auto& binding = it->second.storageBinding;
      if (binding && binding->ownerIdentity == authenticatedOwner &&
          binding->ownerEpoch == parsed.ownerEpoch &&
          artifactStorageObjectName(it->second.reference) == parsed.objectName)
        it = m_remotePublishBindings.erase(it);
      else ++it;
    }
  }
  const auto peers = m_artifactMetadataStore->userStorageCleanupPeers(
    m_repoNode, authenticatedOwner, parsed.cleanupId);
  if (!passGuard.owns_lock()) return toBytes(reply.toJson());
  if (peers.hasQuarantinedContent || !peers.reservedWrites.empty())
    return toBytes(reply.toJson());
  for (const auto& candidate : m_artifactMetadataStore->recoverablePublishOperations()) {
    const auto loaded = m_artifactMetadataStore->lookupPublishOperation(
      candidate.operationId, candidate.submittingPrincipal);
    if (!loaded) throw std::runtime_error("repo-user-storage-cleanup-conflict: publication missing");
    const auto& intent = loaded->intent;
    const auto& status = loaded->status;
    std::vector<RepoArtifactTarget> targets;
    for (const auto& target : status.targets) targets.push_back(target.target);
    const auto requestDigest = intent.schemaVersion == 2
      ? makePublishRequestDigest(intent.reference, intent.generation, intent.seed,
          intent.consistency, intent.targets, intent.userStorageBindings)
      : makePublishRequestDigest(intent.reference, intent.generation, intent.seed,
          intent.consistency, intent.targets);
    if (loaded->operationId != makePublishOperationId(loaded->submittingPrincipal,
          intent.seed, intent.idempotencyKeyDigest) ||
        loaded->idempotencyKeyDigest != intent.idempotencyKeyDigest ||
        loaded->requestDigest != intent.requestDigest || intent.requestDigest != requestDigest ||
        loaded->state != status.state || status.operationId != loaded->operationId ||
        status.schemaVersion != intent.schemaVersion || status.reference != intent.reference ||
        status.generation != intent.generation || !(status.seed == intent.seed) ||
        status.consistency != intent.consistency || targets != intent.targets)
      throw std::runtime_error("repo-user-storage-cleanup-conflict: publication identity mismatch");
    if (loaded->state != "ACCEPTED" && loaded->state != "IN_PROGRESS") continue;
    if (artifactStorageObjectName(intent.reference) != claim.object.objectName &&
        intent.reference.contentDigest != claim.object.wireSha256) continue;
    const RepoArtifactTarget localSeed{m_repoNode, m_providerIdentity,
      m_servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
    const auto seed = std::find_if(intent.userStorageBindings.begin(),
      intent.userStorageBindings.end(), [&intent] (const auto& entry) {
        return entry.target == intent.seed;
      });
    if (intent.schemaVersion != 2 || !(intent.seed == localSeed) ||
        seed == intent.userStorageBindings.end() ||
        seed->binding.ownerIdentity != authenticatedOwner || seed->binding.ownerEpoch != parsed.ownerEpoch ||
        artifactStorageObjectName(intent.reference) != claim.object.objectName ||
        intent.reference.contentDigest != claim.object.wireSha256 ||
        intent.reference.sizeBytes != claim.object.payloadWireBytes || intent.generation != claim.object.generation)
      return toBytes(reply.toJson());
    auto terminal = status;
    const auto committed = static_cast<uint32_t>(std::count_if(terminal.targets.begin(),
      terminal.targets.end(), [] (const auto& target) {
        return target.state == "COMMITTED" && target.receipt.has_value();
      }));
    const auto required = publishRequiredAcks(terminal.targets.size(), terminal.consistency);
    finalizePublishOperation(terminal, committed >= required ? "COMMITTED" :
      committed > 0 ? "PARTIAL" : "FAILED", nowMilliseconds());
  }
  bool retainObject = false;
  for (const auto& peer : peers.retainedClaims) {
    const auto probe = m_core.probeObjectMetadata(peer.object.objectName);
    if (!probe.manifestPresent || !probe.manifestMeasurementAvailable ||
        probe.manifest.objectName != peer.object.objectName ||
        probe.manifest.sha256 != peer.object.wireSha256 ||
        probe.manifest.generation != peer.object.generation ||
        probe.manifest.size != peer.object.payloadWireBytes ||
        probe.manifestWireBytes != peer.object.manifestBytes)
      return toBytes(reply.toJson());
    retainObject = retainObject || peer.object.objectName == claim.object.objectName;
  }
  for (const auto& source : peers.cancelledWrites)
    (void)m_core.abortUserStorageWriteRangesIfOwned(source);
  if (!m_core.canRemoveUserStorageObject(claim.object))
    return toBytes(reply.toJson());
  if (!retainObject && !m_core.removeUserStorageObject(claim.object))
    return toBytes(reply.toJson());
  const auto confirmed = m_artifactMetadataStore->confirmUserStorageCleanup(
    m_repoNode, authenticatedOwner, parsed.cleanupId, nowMilliseconds());
  if (confirmed.state != RepoUserStorageClaimState::Deleted)
    throw std::runtime_error("repo-user-storage-cleanup-unconfirmed");
  reply.state = "DELETED";
  reply.releasedBytes = claim.object.payloadWireBytes + claim.object.manifestBytes;
  return toBytes(reply.toJson());
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
