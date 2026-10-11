#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/segmenter.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

std::vector<uint8_t>
RepoClient::fetchOriginalDataPacket(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& dataName,
  const std::string& expectedWireSha256,
  const std::vector<OriginalDataReplica>& replicas,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs)
{
  if (dataName.empty() || expectedWireSha256.size() != 64 || replicas.empty() ||
      !verifyProducerSignature || timeoutMs <= 0) {
    throw std::invalid_argument("repo-original-data-request-invalid");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  std::set<std::string> visitedProviders;
  std::string lastError = "no committed, current Repo location";
  for (size_t index = 0; index < replicas.size(); ++index) {
    const auto& replica = replicas[index];
    if (replica.repoNode.empty() || replica.providerIdentity.empty() ||
        replica.servicePrefix.empty() ||
        !visitedProviders.insert(replica.providerIdentity.toUri()).second) {
      continue;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining < 3000) break;
    const auto candidateBudget = remaining / static_cast<int64_t>(replicas.size() - index);
    if (candidateBudget < 3000) break;
    const auto candidateDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(candidateBudget);
    const RepoDataPacketQuery query{1, dataName.toUri(), expectedWireSha256};
    const auto request = toBytes(query.toJson());
    bool originalInterestExpressed = false;
    try {
      const int probeBudget = static_cast<int>(candidateBudget / 3);
      const int ackBudget = std::max(1, std::min(250, probeBudget / 4));
      const auto receiptWire = requestSelectedService(
        user, replica.providerIdentity,
        makeRepoServiceName(replica.servicePrefix, "DATA_RECEIPT"),
        request, ackBudget, probeBudget);
      const auto receipt = RepoWriteReceipt::dataPacketFromJson(toString(receiptWire));
      if (receipt.objectName != query.dataName ||
          receipt.digest != query.expectedWireSha256 ||
          receipt.repoNode != replica.repoNode ||
          receipt.providerIdentity != replica.providerIdentity.toUri()) {
        throw std::runtime_error("repo-original-data-receipt-mismatch");
      }
      const auto locationWire = requestSelectedService(
        user, replica.providerIdentity,
        makeRepoServiceName(replica.servicePrefix, "DATA_LOCATION"),
        request, ackBudget, probeBudget);
      const auto location = RepoDataPacketLocation::fromJson(toString(locationWire));
      const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      if (location.receiptId != receipt.operationId ||
          location.repoNode != receipt.repoNode ||
          location.providerIdentity != receipt.providerIdentity ||
          location.dataName != query.dataName ||
          location.wireSha256 != query.expectedWireSha256 ||
          location.state != "PRESENT" ||
          location.forwardingHint != receipt.providerIdentity ||
          location.observedAtMs > static_cast<uint64_t>(nowMs + 5000) ||
          location.expiresAtMs <= static_cast<uint64_t>(nowMs)) {
        throw std::runtime_error("repo-original-data-location-not-current");
      }

      ndn::Face face;
      ndn::Interest interest(dataName);
      interest.setCanBePrefix(false);
      interest.setMustBeFresh(true);
      interest.setForwardingHint({replica.providerIdentity});
      const auto fetchRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        candidateDeadline - std::chrono::steady_clock::now()).count();
      if (fetchRemaining <= 0) throw std::runtime_error("repo-original-data-deadline");
      interest.setInterestLifetime(ndn::time::milliseconds(fetchRemaining));
      bool done = false;
      std::vector<uint8_t> wire;
      std::string fetchError;
      auto pending = face.expressInterest(
        interest,
        [&] (const ndn::Interest&, const ndn::Data& data) {
          if (data.getName() != dataName) {
            fetchError = "repo-original-data-name-mismatch";
          }
          else {
            const auto& block = data.wireEncode();
            wire.assign(block.begin(), block.end());
          }
          done = true;
        },
        [&] (const ndn::Interest&, const ndn::lp::Nack&) {
          fetchError = "repo-original-data-nack";
          done = true;
        },
        [&] (const ndn::Interest&) {
          fetchError = "repo-original-data-timeout";
          done = true;
        });
      originalInterestExpressed = true;
      while (!done && std::chrono::steady_clock::now() < candidateDeadline) {
        face.processEvents(ndn::time::milliseconds(25));
        face.getIoContext().restart();
      }
      if (!done || !fetchError.empty()) {
        throw std::runtime_error(fetchError.empty() ?
          "repo-original-data-deadline" : fetchError);
      }
      if (wire.size() != receipt.persistedBytes ||
          sha256Hex(wire) != expectedWireSha256) {
        throw std::runtime_error("repo-original-data-wire-mismatch");
      }
      ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
      block.parse();
      const ndn::Data data(block);
      if (data.getName() != dataName || !verifyProducerSignature(data)) {
        throw std::runtime_error("repo-original-data-producer-signature-invalid");
      }
      return wire;
    }
    catch (const std::exception& error) {
      lastError = error.what();
      if (originalInterestExpressed) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
        if (remaining > 1500) {
          try {
            const int recheckBudget = static_cast<int>(std::min<int64_t>(3000, remaining / 3));
            (void)RepoDataPacketLocation::fromJson(toString(requestSelectedService(
              user, replica.providerIdentity,
              makeRepoServiceName(replica.servicePrefix, "DATA_LOCATION"),
              request, std::min(250, recheckBudget / 4), recheckBudget)));
          }
          catch (const std::exception&) {
            // A failed recheck cannot make the old location usable; continue
            // with a distinct candidate under the original deadline.
          }
        }
      }
    }
  }
  throw std::runtime_error("repo-original-data-unavailable: " + lastError);
}

namespace {

struct SelectedRequestResult
{
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  std::string error;
  std::vector<uint8_t> payload;
};

struct SelectedRequestStart
{
  enum class CommitState
  {
    Available,
    Claimed,
    Abandoned
  };

  std::atomic<bool> abandoned{false};
  std::atomic<CommitState> commitState{CommitState::Available};
  std::atomic<bool> cancellationIssued{false};
};

void
completeSelectedRequest(const std::shared_ptr<SelectedRequestResult>& result,
                        const std::string& error)
{
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->done) return;
    result->error = error;
    result->done = true;
  }
  result->condition.notify_all();
}

class ExactRepoParticipantSelection final :
  public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  ExactRepoParticipantSelection(ndn::Name provider, ndn::Name service)
    : m_provider(std::move(provider))
    , m_service(std::move(service))
  {
  }

  std::vector<ndn_service_framework::SelectedParticipant>
  select(const std::vector<ndn_service_framework::AckCandidate>& candidates,
         const std::vector<ndn_service_framework::CollaborationRoleSpec>& roles) const override
  {
    if (roles.size() != 1) return {};
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&] (const auto& candidate) {
      return candidate.providerName == m_provider && candidate.serviceName == m_service &&
             candidate.ack.getStatus() &&
             candidate.authenticationEvidence.trustSchemaValidated &&
             candidate.authenticationEvidence.signerIdentity == m_provider.toUri();
    });
    if (found == candidates.end()) return {};
    ndn_service_framework::SelectedParticipant selected;
    selected.role = roles.front().role;
    selected.service = roles.front().service;
    selected.provider = found->providerName;
    selected.assignedArtifact = roles.front().requiredArtifact;
    selected.requiresProvisioning = roles.front().allowDynamicProvisioning;
    selected.provisioningTimeoutMs = roles.front().provisioningTimeoutMs;
    selected.assignmentPayload = roles.front().assignmentPayload;
    selected.ack = *found;
    return {std::move(selected)};
  }

private:
  ndn::Name m_provider;
  ndn::Name m_service;
};

std::vector<uint8_t>
localRequest(ndn_service_framework::LocalServiceRegistry& registry,
             const ndn::Name& repoServicePrefix,
             const std::string& operation,
             const std::vector<uint8_t>& payload)
{
  auto request = RepoClient::makeRequest(payload);
  ndn_service_framework::ResponseMessage response;
  registry.localInvokeRawInto(
    makeRepoServiceName(repoServicePrefix, operation),
    request,
    response);
  if (!response.getStatus()) {
    throw std::runtime_error("local repo " + operation + " failed: " +
                             response.getErrorInfo());
  }
  const auto responsePayload = response.getPayload();
  return std::vector<uint8_t>(responsePayload.begin(), responsePayload.end());
}

std::string
segmentObjectName(const std::string& objectName, size_t segmentIndex)
{
  return objectName + "/seg/" + std::to_string(segmentIndex);
}

RepoObjectManifest
makeParentManifest(const std::string& objectName,
                   const std::vector<uint8_t>& payload,
                   const StoreOptions& options,
                   size_t segmentCount)
{
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.objectType = options.objectType;
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = static_cast<uint32_t>(segmentCount);
  manifest.replicationFactor = options.replicationFactor;
  manifest.replicaNodes = options.replicaNodes;
  manifest.policyEpoch = options.policyEpoch;
  return manifest;
}

void
verifySegmentedPayload(const RepoObjectManifest& manifest,
                       const std::vector<uint8_t>& payload)
{
  if (payload.size() != manifest.size) {
    throw std::runtime_error("segmented repo object size mismatch: " +
                             manifest.objectName);
  }
  if (!manifest.sha256.empty() && sha256Hex(payload) != manifest.sha256) {
    throw std::runtime_error("segmented repo object sha256 mismatch: " +
                             manifest.objectName);
  }
}

} // namespace

RepoObjectManifest
RepoClient::put(RepoNode& node,
                const std::string& objectName,
                const std::vector<uint8_t>& payload,
                StoreOptions options)
{
  return node.put(objectName,
                  payload,
                  options.objectType,
                  options.replicationFactor,
                  options.policyEpoch,
                  std::move(options.replicaNodes));
}

std::vector<uint8_t>
RepoClient::get(const RepoNode& node, const std::string& objectName)
{
  return node.get(objectName);
}

void
RepoClient::putRange(RepoNode& node,
                     const RepoObjectManifest& manifest,
                     RepoByteRange range,
                     const std::vector<uint8_t>& bytes)
{
  node.putRange(manifest, range, bytes);
}

RepoObjectManifest
RepoClient::commitRanges(RepoNode& node, const RepoObjectManifest& manifest)
{
  return node.commitRanges(manifest);
}

void
RepoClient::abortRanges(RepoNode& node, const std::string& objectName)
{
  node.abortRanges(objectName);
}

std::vector<uint8_t>
RepoClient::getRange(const RepoNode& node,
                    const std::string& objectName,
                    RepoByteRange range)
{
  return node.getRange(objectName, range);
}

RepoObjectManifest
RepoClient::getManifest(const RepoNode& node, const std::string& objectName)
{
  return node.getManifest(objectName);
}

std::vector<RepoObjectManifest>
RepoClient::list(const RepoNode& node)
{
  return node.list();
}

bool
RepoClient::remove(RepoNode& node, const std::string& objectName)
{
  return node.remove(objectName);
}

RepoObjectManifest
RepoClient::putDataPacket(RepoNode& node, const std::vector<uint8_t>& wire)
{
  ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
  block.parse();
  const ndn::Data data(block);
  return node.putDataPacket(data.getName().toUri(), wire);
}

std::vector<uint8_t>
RepoClient::getDataPacket(const RepoNode& node, const std::string& dataName)
{
  return node.getDataPacket(dataName);
}

std::vector<std::vector<uint8_t>>
RepoClient::getDataPackets(const RepoNode& node,
                           const RepoObjectManifest& manifest)
{
  if (manifest.packetNames.empty()) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: manifest has no packet names: " +
      manifest.objectName);
  }
  if (manifest.packetNames.size() != manifest.segmentCount) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: packet count does not match segmentCount: " +
      manifest.objectName);
  }
  const std::set<std::string> uniqueNames(manifest.packetNames.begin(),
                                          manifest.packetNames.end());
  if (uniqueNames.size() != manifest.packetNames.size()) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: manifest contains duplicate packet names: " +
      manifest.objectName);
  }

  std::vector<std::vector<uint8_t>> wires;
  wires.reserve(manifest.packetNames.size());
  for (const auto& expectedName : manifest.packetNames) {
    auto wire = getDataPacket(node, expectedName);
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    block.parse();
    const ndn::Data data(block);
    const auto actualName = data.getName().toUri();
    if (actualName != expectedName) {
      throw std::runtime_error(
        "repo-data-name-mismatch: manifest=" + expectedName +
        " wire=" + actualName);
    }
    wires.push_back(std::move(wire));
  }
  return wires;
}

RepoOperationStatus
RepoClient::insert(RepoNode& node,
                   const RepoDataReference& reference)
{
  return parseOperationStatusJson(toString(
    node.handleInsert(encodeDataReferenceRequest(reference))));
}

RepoOperationStatus
RepoClient::insertPayload(RepoNode& node,
                          const std::string& objectName,
                          const std::vector<uint8_t>& payload,
                          ndn::KeyChain& keyChain,
                          const ndn::security::SigningInfo& signingInfo,
                          StoreOptions options,
                          size_t maxSegmentPayload)
{
  ndn::Segmenter segmenter(keyChain, signingInfo);
  auto dataPackets = segmenter.segment(
    ndn::span<const uint8_t>(payload.data(), payload.size()),
    ndn::Name(objectName),
    std::max<size_t>(1, maxSegmentPayload),
    ndn::time::hours(1));

  std::vector<std::vector<uint8_t>> wirePackets;
  wirePackets.reserve(dataPackets.size());
  std::vector<uint8_t> concatenated;
  for (const auto& data : dataPackets) {
    const auto wire = data->wireEncode();
    std::vector<uint8_t> wireBytes(wire.begin(), wire.end());
    concatenated.insert(concatenated.end(), wireBytes.begin(), wireBytes.end());
    wirePackets.push_back(std::move(wireBytes));
  }

  RepoDataReference reference;
  reference.objectName = objectName;
  reference.dataPrefix = objectName;
  reference.firstSegment = 0;
  reference.hasFinalSegment = !wirePackets.empty();
  reference.finalSegment = wirePackets.empty() ? 0 : wirePackets.size() - 1;
  reference.expectedSize = concatenated.size();
  reference.expectedSha256 = sha256Hex(concatenated);
  reference.storeWirePackets = true;
  reference.objectType = options.objectType.empty() ? "ndn-segmented-data" : options.objectType;

  return node.insertWirePackets(reference, wirePackets);
}

RepoOperationStatus
RepoClient::status(const RepoNode& node, const std::string& operationId)
{
  return parseOperationStatusJson(toString(node.handleStatus(encodeStatusRequest(operationId))));
}

RepoCatalogStatus
RepoClient::catalogStatus(const RepoNode& node)
{
  return parseCatalogStatusJson(toString(node.handleCatalogStatus()));
}

RepoCacheStatus
RepoClient::cacheStatus(const RepoNode& node)
{
  return parseCacheStatusJson(toString(node.handleCacheStatus()));
}

RepoCatalogDelta
RepoClient::catalogSnapshot(const RepoNode& node)
{
  return parseCatalogDeltaJson(toString(node.handleCatalogSnapshot()));
}

RepoCatalogDelta
RepoClient::catalogDelta(const RepoNode& node, uint64_t sinceEpoch)
{
  return parseCatalogDeltaJson(toString(
    node.handleCatalogDelta(encodeCatalogDeltaRequest(sinceEpoch))));
}

RepoCatalogEntry
RepoClient::catalogLookup(const RepoNode& node,
                          const std::string& objectName)
{
  return parseCatalogEntryJson(toString(
    node.handleCatalogLookup(encodeCatalogLookupRequest(objectName))));
}

RepoObjectManifest
RepoClient::putSegmented(RepoNode& node,
                         const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         StoreOptions options,
                         size_t maxSegmentPayload)
{
  maxSegmentPayload = std::max<size_t>(1, maxSegmentPayload);
  if (payload.size() <= maxSegmentPayload) {
    return put(node, objectName, payload, std::move(options));
  }

  const size_t segmentCount =
    (payload.size() + maxSegmentPayload - 1) / maxSegmentPayload;
  StoreOptions segmentOptions = options;
  segmentOptions.objectType = options.objectType + ".segment";
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto start = i * maxSegmentPayload;
    const auto end = std::min(payload.size(), start + maxSegmentPayload);
    const std::vector<uint8_t> segmentPayload(payload.begin() + start,
                                              payload.begin() + end);
    put(node, segmentObjectName(objectName, i), segmentPayload, segmentOptions);
  }

  auto manifest = makeParentManifest(objectName, payload, options, segmentCount);
  return node.putManifest(manifest);
}

std::vector<uint8_t>
RepoClient::getSegmented(const RepoNode& node, const RepoObjectManifest& manifest)
{
  if (manifest.segmentCount <= 1) {
    auto payload = get(node, manifest.objectName);
    verifySegmentedPayload(manifest, payload);
    return payload;
  }

  std::vector<uint8_t> payload;
  payload.reserve(static_cast<size_t>(manifest.size));
  for (uint32_t i = 0; i < manifest.segmentCount; ++i) {
    auto segment = get(node, segmentObjectName(manifest.objectName, i));
    payload.insert(payload.end(), segment.begin(), segment.end());
  }
  verifySegmentedPayload(manifest, payload);
  return payload;
}

std::vector<uint8_t>
RepoClient::getObject(const RepoNode& node, const RepoObjectManifest& manifest)
{
  return getSegmented(node, manifest);
}

RepoObjectManifest
RepoClient::localPut(ndn_service_framework::LocalServiceRegistry& registry,
                     const ndn::Name& repoServicePrefix,
                     const std::string& objectName,
                     const std::vector<uint8_t>& payload,
                     StoreOptions options)
{
  auto manifest = makeManifest(objectName,
                               options.objectType,
                               payload,
                               options.replicationFactor,
                               std::move(options.replicaNodes),
                               std::move(options.policyEpoch));
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "STORE", encodeStoreRequest(manifest, payload))));
}

std::vector<uint8_t>
RepoClient::localGet(ndn_service_framework::LocalServiceRegistry& registry,
                     const ndn::Name& repoServicePrefix,
                     const std::string& objectName)
{
  return localRequest(registry, repoServicePrefix, "FETCH", toBytes(objectName));
}

void
RepoClient::localPutRange(ndn_service_framework::LocalServiceRegistry& registry,
                          const ndn::Name& repoServicePrefix,
                          const RepoObjectManifest& manifest,
                          RepoByteRange range,
                          const std::vector<uint8_t>& bytes)
{
  const auto response = localRequest(
    registry, repoServicePrefix, "STORE_RANGE",
    encodeRangeWriteRequest(manifest, range, bytes));
  if (toString(response) != "accepted") {
    throw std::runtime_error("local repo STORE_RANGE returned unexpected response");
  }
}

RepoObjectManifest
RepoClient::localCommitRanges(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const RepoObjectManifest& manifest)
{
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "COMMIT_RANGES", encodeManifestRequest(manifest))));
}

void
RepoClient::localAbortRanges(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix,
                             const std::string& objectName)
{
  const auto response = localRequest(
    registry, repoServicePrefix, "ABORT_RANGES",
    encodeRangeAbortRequest(objectName));
  if (toString(response) != "aborted") {
    throw std::runtime_error("local repo ABORT_RANGES returned unexpected response");
  }
}

std::vector<uint8_t>
RepoClient::localGetRange(ndn_service_framework::LocalServiceRegistry& registry,
                          const ndn::Name& repoServicePrefix,
                          const std::string& objectName,
                          RepoByteRange range)
{
  return localRequest(registry, repoServicePrefix, "FETCH_RANGE",
                      encodeRangeReadRequest(objectName, range));
}

RepoObjectManifest
RepoClient::localGetManifest(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix,
                             const std::string& objectName)
{
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "MANIFEST", toBytes(objectName))));
}

std::vector<RepoObjectManifest>
RepoClient::localList(ndn_service_framework::LocalServiceRegistry& registry,
                      const ndn::Name& repoServicePrefix)
{
  return parseInventoryJson(toString(localRequest(
    registry, repoServicePrefix, "INVENTORY", {})));
}

bool
RepoClient::localRemove(ndn_service_framework::LocalServiceRegistry& registry,
                        const ndn::Name& repoServicePrefix,
                        const std::string& objectName)
{
  return toString(localRequest(registry, repoServicePrefix, "DELETE",
                               toBytes(objectName))) == "deleted";
}

RepoOperationStatus
RepoClient::localInsert(ndn_service_framework::LocalServiceRegistry& registry,
                                       const ndn::Name& repoServicePrefix,
                                       const RepoDataReference& reference)
{
  return parseOperationStatusJson(toString(localRequest(
    registry, repoServicePrefix, "INSERT",
    encodeDataReferenceRequest(reference))));
}

RepoOperationStatus
RepoClient::localStatus(ndn_service_framework::LocalServiceRegistry& registry,
                        const ndn::Name& repoServicePrefix,
                        const std::string& operationId)
{
  return parseOperationStatusJson(toString(localRequest(
    registry, repoServicePrefix, "STATUS", encodeStatusRequest(operationId))));
}

RepoCatalogStatus
RepoClient::localCatalogStatus(ndn_service_framework::LocalServiceRegistry& registry,
                               const ndn::Name& repoServicePrefix)
{
  return parseCatalogStatusJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_STATUS", {})));
}

RepoCacheStatus
RepoClient::localCacheStatus(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix)
{
  return parseCacheStatusJson(toString(localRequest(
    registry, repoServicePrefix, "CACHE_STATUS", {})));
}

RepoCatalogDelta
RepoClient::localCatalogSnapshot(ndn_service_framework::LocalServiceRegistry& registry,
                                 const ndn::Name& repoServicePrefix)
{
  return parseCatalogDeltaJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_SNAPSHOT", {})));
}

RepoCatalogDelta
RepoClient::localCatalogDelta(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              uint64_t sinceEpoch)
{
  return parseCatalogDeltaJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_DELTA",
    encodeCatalogDeltaRequest(sinceEpoch))));
}

RepoCatalogEntry
RepoClient::localCatalogLookup(ndn_service_framework::LocalServiceRegistry& registry,
                               const ndn::Name& repoServicePrefix,
                               const std::string& objectName)
{
  return parseCatalogEntryJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_LOOKUP",
    encodeCatalogLookupRequest(objectName))));
}

RepoObjectManifest
RepoClient::localPutSegmented(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const std::string& objectName,
                              const std::vector<uint8_t>& payload,
                              StoreOptions options,
                              size_t maxSegmentPayload)
{
  maxSegmentPayload = std::max<size_t>(1, maxSegmentPayload);
  if (payload.size() <= maxSegmentPayload) {
    return localPut(registry, repoServicePrefix, objectName, payload,
                    std::move(options));
  }

  const size_t segmentCount =
    (payload.size() + maxSegmentPayload - 1) / maxSegmentPayload;
  StoreOptions segmentOptions = options;
  segmentOptions.objectType = options.objectType + ".segment";
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto start = i * maxSegmentPayload;
    const auto end = std::min(payload.size(), start + maxSegmentPayload);
    const std::vector<uint8_t> segmentPayload(payload.begin() + start,
                                              payload.begin() + end);
    localPut(registry, repoServicePrefix, segmentObjectName(objectName, i),
             segmentPayload, segmentOptions);
  }

  auto manifest = makeParentManifest(objectName, payload, options, segmentCount);
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "STORE_MANIFEST", encodeManifestRequest(manifest))));
}

std::vector<uint8_t>
RepoClient::localGetSegmented(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const RepoObjectManifest& manifest)
{
  if (manifest.segmentCount <= 1) {
    auto payload = localGet(registry, repoServicePrefix, manifest.objectName);
    verifySegmentedPayload(manifest, payload);
    return payload;
  }

  std::vector<uint8_t> payload;
  payload.reserve(static_cast<size_t>(manifest.size));
  for (uint32_t i = 0; i < manifest.segmentCount; ++i) {
    auto segment = localGet(registry, repoServicePrefix,
                            segmentObjectName(manifest.objectName, i));
    payload.insert(payload.end(), segment.begin(), segment.end());
  }
  verifySegmentedPayload(manifest, payload);
  return payload;
}

std::vector<uint8_t>
RepoClient::localGetObject(ndn_service_framework::LocalServiceRegistry& registry,
                           const ndn::Name& repoServicePrefix,
                           const RepoObjectManifest& manifest)
{
  return localGetSegmented(registry, repoServicePrefix, manifest);
}

RepoObjectManifest
RepoClient::makeManifest(std::string objectName,
                         std::string objectType,
                         const std::vector<uint8_t>& payload,
                         uint32_t replicationFactor,
                         std::vector<std::string> replicaNodes,
                         std::string policyEpoch)
{
  RepoObjectManifest manifest;
  manifest.objectName = std::move(objectName);
  manifest.objectType = std::move(objectType);
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.replicationFactor = replicationFactor;
  manifest.replicaNodes = std::move(replicaNodes);
  manifest.policyEpoch = std::move(policyEpoch);
  return manifest;
}

ndn_service_framework::RequestMessage
RepoClient::makeRequest(const std::vector<uint8_t>& payload)
{
  ndn::Buffer buffer;
  if (!payload.empty()) {
    buffer = ndn::Buffer(payload.data(), payload.size());
  }
  ndn_service_framework::RequestMessage request;
  request.setPayload(buffer, buffer.size());
  return request;
}

ndn::Name
RepoClient::requestCapability(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "CAPABILITY"),
                             makeRequest({}),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestCacheStatus(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "CACHE_STATUS"),
                             makeRequest({}),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestStore(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  const std::vector<uint8_t>& payload,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "STORE"),
                             makeRequest(encodeStoreRequest(manifest, payload)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestStoreRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  RepoByteRange range,
  const std::vector<uint8_t>& bytes,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE_RANGE"),
    makeRequest(encodeRangeWriteRequest(manifest, range, bytes)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactStoreRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactRangeWriteRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE_RANGE"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactStore(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactStoreRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

ndn::Name
RepoClient::requestCommitRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "COMMIT_RANGES"),
    makeRequest(encodeManifestRequest(manifest)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactCommitRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactRangeCommitRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "COMMIT_RANGES"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

std::vector<uint8_t>
RepoClient::requestSelectedService(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& providerIdentity,
  const ndn::Name& serviceName,
  const std::vector<uint8_t>& payload,
  int ackCollectionTimeMs,
  int timeoutMs,
  std::shared_ptr<std::atomic<bool>> requestMayHaveBeenEmitted)
{
  if (providerIdentity.empty() || serviceName.empty() || ackCollectionTimeMs <= 0 ||
      timeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument("repo-selected-request-invalid: Provider, service, or deadline is invalid");
  }
  if (user.isOnIoThread()) {
    throw std::logic_error(
      "repo-selected-request-on-io-thread: blocking selected requests cannot run on the Face I/O thread");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  const std::string timeoutError =
    "repo-request-timeout: Provider request timed out";
  auto result = std::make_shared<SelectedRequestResult>();
  auto startState = std::make_shared<SelectedRequestStart>();
  ndn::Buffer requestPayload;
  if (!payload.empty()) requestPayload = ndn::Buffer(payload.data(), payload.size());
  const std::vector<ndn::Name> providers{providerIdentity};
  const auto cancelRequest = [&user, startState] (const ndn::Name& requestId) {
    if (!user.isOnIoThread() ||
        startState->cancellationIssued.exchange(true,
                                                 std::memory_order_acq_rel)) {
      return;
    }
    try {
      user.CancelCollaboration(requestId);
    }
    catch (...) {
      // Cancellation is best-effort after the caller's absolute deadline.
    }
  };
  auto startRequest = [&user, result, startState, providerIdentity, serviceName,
                       requestPayload, ackCollectionTimeMs, deadline,
                       timeoutError, requestMayHaveBeenEmitted,
                       providers, cancelRequest] {
    try {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
        return;
      }
      const auto remainingTimeoutMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remainingTimeoutMs <= ackCollectionTimeMs) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
        return;
      }
      const auto invocationTimeoutMs = static_cast<int>(remainingTimeoutMs);
      const auto requestId = user.BeginCollaborationWithProviders(
        serviceName, requestPayload, ackCollectionTimeMs, invocationTimeoutMs,
        [&user, result, startState, providerIdentity, serviceName,
         ackCollectionTimeMs, invocationTimeoutMs, deadline, timeoutError,
         requestMayHaveBeenEmitted, cancelRequest]
        (const ndn_service_framework::CollaborationAckClosure& closure) {
          try {
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            size_t matching = 0;
            for (const auto& candidate : closure.candidates) {
              if (candidate.providerName == providerIdentity &&
                  candidate.serviceName == serviceName && candidate.ack.getStatus() &&
                  candidate.authenticationEvidence.trustSchemaValidated &&
                  candidate.authenticationEvidence.signerIdentity ==
                    providerIdentity.toUri()) {
                ++matching;
              }
            }
            if (matching != 1) {
              throw std::runtime_error(
                std::string("repo-ack-authentication-failed: selected Provider ACK was absent, duplicated, or untrusted") +
                " (matching=" + std::to_string(matching) +
                ", observed=" + std::to_string(closure.candidates.size()) + ")");
            }
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            ndn_service_framework::CollaborationRoleSpec role;
            role.role = "artifact-repo-target";
            role.service = serviceName;
            role.minProviders = 1;
            role.maxProviders = 1;
            role.terminalResponseOwner = true;
            role.assignmentPayload = ndn::Buffer(1);
            role.assignmentPayload[0] = 1;
            ndn_service_framework::CollaborationPlan plan;
            plan.ackCollectionTimeMs = ackCollectionTimeMs;
            plan.timeoutMs = invocationTimeoutMs;
            plan.roles.push_back(std::move(role));
            plan.participantSelector = std::make_shared<ExactRepoParticipantSelection>(
              providerIdentity, serviceName);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            // CommitCollaborationPlan can publish a Selection before reporting
            // an error. This atomic claim races the caller's deadline so a
            // timed-out request cannot newly cross the emission boundary.
            auto expectedCommitState = SelectedRequestStart::CommitState::Available;
            if (!startState->commitState.compare_exchange_strong(
                  expectedCommitState, SelectedRequestStart::CommitState::Claimed,
                  std::memory_order_acq_rel)) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              startState->commitState.store(
                SelectedRequestStart::CommitState::Abandoned,
                std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
      if (requestMayHaveBeenEmitted) {
        requestMayHaveBeenEmitted->store(true, std::memory_order_release);
      }
      if (!user.CommitCollaborationPlan(closure.requestId, closure.digest,
                                        std::move(plan))) {
        throw std::runtime_error(
          "repo-selection-commit-failed: Core did not commit selected Provider");
      }
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        startState->commitState.store(
          SelectedRequestStart::CommitState::Abandoned,
          std::memory_order_release);
        cancelRequest(closure.requestId);
        completeSelectedRequest(result, timeoutError);
      }
          }
          catch (const std::exception& error) {
            cancelRequest(closure.requestId);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              completeSelectedRequest(result, timeoutError);
            }
            else {
              completeSelectedRequest(result, error.what());
            }
          }
          catch (...) {
            cancelRequest(closure.requestId);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              completeSelectedRequest(result, timeoutError);
            }
            else {
              completeSelectedRequest(
                result, "repo-selection-commit-failed: selected Provider request failed");
            }
          }
        },
        [result, startState, deadline, timeoutError]
        (const ndn_service_framework::ResponseMessage& response) {
          const auto responsePayload = response.getPayload();
          {
            std::lock_guard<std::mutex> guard(result->mutex);
            if (result->done) return;
            if (std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              result->error = timeoutError;
            }
            else if (!response.getStatus()) {
              result->error = response.getErrorInfo();
            }
            else {
              result->payload.assign(responsePayload.begin(), responsePayload.end());
            }
            result->done = true;
          }
          result->condition.notify_all();
        },
        [result, startState, timeoutError] (const ndn::Name&) {
          startState->abandoned.store(true, std::memory_order_release);
          completeSelectedRequest(result, timeoutError);
        },
        ndn::Name(),
        [providerIdentity, serviceName] (
          const std::vector<ndn_service_framework::AckCandidate>& candidates) {
          // The fixed Provider set is covered by one authenticated ACK. Core
          // still freezes ACK_CLOSED; the callback above rechecks eligibility.
          return std::count_if(candidates.begin(), candidates.end(),
            [&providerIdentity, &serviceName] (const auto& candidate) {
              return candidate.providerName == providerIdentity &&
                     candidate.serviceName == serviceName && candidate.ack.getStatus() &&
                     candidate.authenticationEvidence.trustSchemaValidated &&
                     candidate.authenticationEvidence.signerIdentity ==
                       providerIdentity.toUri();
            }) == 1;
        }, ndn_service_framework::RequestCapabilities(),
        std::nullopt, {}, {}, {}, providers);
      // Begin can return an ID after local admission rejection; the lifecycle
      // check below handles that case. Otherwise the Request is admitted or
      // queued, and ACK-close runs after this Face-thread start returns, so an
      // empty ACK set leaves the provider outcome uncertain before Selection.
      if (!requestId.empty()) {
        const auto requestStatus = user.getRequestStatus(requestId);
        const bool admissionRejected = requestStatus &&
          requestStatus->state ==
            ndn_service_framework::ServiceUser::RequestLifecycleState::ADMISSION_REJECTED;
        if (admissionRejected) {
          completeSelectedRequest(
            result,
            "repo-request-admission-rejected: local admission queue rejected Provider request");
          return;
        }
        if (requestMayHaveBeenEmitted) {
          requestMayHaveBeenEmitted->store(true, std::memory_order_release);
        }
      }
      if (requestId.empty()) {
        if (startState->abandoned.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
          startState->abandoned.store(true, std::memory_order_release);
          completeSelectedRequest(result, timeoutError);
        }
        else {
          completeSelectedRequest(
            result, "repo-request-start-failed: Core rejected selected Provider request");
        }
        return;
      }
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        cancelRequest(requestId);
        completeSelectedRequest(result, timeoutError);
      }
    }
    catch (const std::exception& error) {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
      }
      else {
        completeSelectedRequest(result, error.what());
      }
    }
    catch (...) {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
      }
      else {
        completeSelectedRequest(
          result, "repo-request-start-failed: selected Provider request could not start");
      }
    }
  };
  try {
    user.postToIo(std::move(startRequest));
  }
  catch (const std::exception& error) {
    startState->abandoned.store(true, std::memory_order_release);
    if (std::chrono::steady_clock::now() >= deadline) {
      completeSelectedRequest(result, timeoutError);
    }
    else {
      completeSelectedRequest(result, error.what());
    }
  }
  catch (...) {
    startState->abandoned.store(true, std::memory_order_release);
    if (std::chrono::steady_clock::now() >= deadline) {
      completeSelectedRequest(result, timeoutError);
    }
    else {
      completeSelectedRequest(
        result, "repo-request-start-failed: selected Provider request could not start");
    }
  }
  std::unique_lock<std::mutex> lock(result->mutex);
  const bool completed = result->condition.wait_until(
    lock, deadline, [&] { return result->done; });
  if (!completed) {
    startState->abandoned.store(true, std::memory_order_release);
    auto expectedCommitState = SelectedRequestStart::CommitState::Available;
    if (!startState->commitState.compare_exchange_strong(
          expectedCommitState, SelectedRequestStart::CommitState::Abandoned,
          std::memory_order_acq_rel) &&
        expectedCommitState == SelectedRequestStart::CommitState::Claimed &&
        requestMayHaveBeenEmitted) {
      // The ACK callback claimed the Selection boundary just before timeout;
      // keep fallback logic from treating that in-flight publish as absent.
      requestMayHaveBeenEmitted->store(true, std::memory_order_release);
    }
    result->error = timeoutError;
    result->done = true;
  }
  auto error = std::move(result->error);
  auto responsePayload = std::move(result->payload);
  lock.unlock();
  if (!completed) {
    result->condition.notify_all();
  }
  if (!error.empty()) throw std::runtime_error(error);
  return responsePayload;
}

ndn::Name
RepoClient::requestAbortRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "ABORT_RANGES"),
    makeRequest(encodeRangeAbortRequest(objectName)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestInsert(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoDataReference& reference,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "INSERT"),
    makeRequest(encodeDataReferenceRequest(reference)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestFetch(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "FETCH"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestReceipt(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const RepoReceiptLookupRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto payload = toBytes(request.toJson());
  const char* traceSetting =
    std::getenv("NDNSF_SPEC192_SELECTED_REQUEST_TRACE");
  const bool traceEnabled = traceSetting != nullptr &&
                            traceSetting[0] == '1' && traceSetting[1] == '\0';
  if (!traceEnabled) {
    return user.RequestService(
      std::vector<ndn::Name>{provider},
      makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
      timeoutMs, std::move(onTimeout), std::move(onResponse),
      ndn_service_framework::tlv::FirstResponding);
  }
  const auto traceStage = [] (const char* stage) {
    std::fprintf(stderr, "Spec192 receipt request: stage=%s service=STATUS\n",
                 stage);
    std::fflush(stderr);
  };
  traceStage("request-service-begin");
  auto tracedOnTimeout = [traceStage, onTimeout = std::move(onTimeout)]
                         (const ndn::Name& timedOutRequest) mutable {
    traceStage("timeout-callback");
    if (onTimeout) onTimeout(timedOutRequest);
  };
  auto tracedOnResponse = [traceStage, onResponse = std::move(onResponse)]
                          (const ndn_service_framework::ResponseMessage& response)
                          mutable {
    traceStage("response-callback");
    if (onResponse) onResponse(response);
  };
  auto requestId = user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(tracedOnTimeout), std::move(tracedOnResponse),
    ndn_service_framework::tlv::FirstResponding);
  traceStage("request-service-returned");
  return requestId;
}

ndn::Name
RepoClient::requestAvailability(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const RepoAvailabilityProbeRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto payload = toBytes(request.toJson());
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestFetchRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  RepoByteRange range,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "FETCH_RANGE"),
    makeRequest(encodeRangeReadRequest(objectName, range)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestManifest(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "MANIFEST"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestInventory(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "INVENTORY"),
                             makeRequest({}),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestDelete(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "DELETE"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestStatus(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& operationId,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "STATUS"),
                             makeRequest(encodeStatusRequest(operationId)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestLocation(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const ArtifactLocationRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto wire = request.toJson();
  const auto payload = toBytes(wire);
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestFetchRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  RepoByteRange range,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "FETCH_RANGE"),
    makeRequest(encodeRangeReadRequest(objectName, range)), timeoutMs,
    std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

} // namespace ndnsf_distributed_repo
