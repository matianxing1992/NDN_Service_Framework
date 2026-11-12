#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "packet-store-lookup.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndn-service-framework/TimelineTrace.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <fstream>
#include <set>
#include <map>

namespace ndnsf_distributed_repo {

NDN_LOG_INIT(ndnsf_distributed_repo.RepoCore);

namespace {

void
validatePacketIndex(const RepoObjectManifest& manifest)
{
  if (manifest.objectType != "original-packet-set" || manifest.packetNames.empty() ||
      manifest.packetOffsets.size() != manifest.packetNames.size() + 1 ||
      manifest.packetOffsets.front() != 0 || manifest.packetOffsets.back() != manifest.size ||
      manifest.objectName != manifest.packetNames.front())
    throw std::invalid_argument("repo-invalid-original-packet-index");
  // A manifest and its DATA segments need not be immediate siblings. Preserve
  // the writer's URI spelling, but compare typed names to reject URI aliases.
  std::set<ndn::Name> names;
  for (size_t i = 0; i < manifest.packetNames.size(); ++i) {
    const auto& text = manifest.packetNames[i];
    if (text.empty() || text.front() != '/' || text.size() > 4096)
      throw std::invalid_argument("repo-invalid-original-packet-index");
    const ndn::Name name(text);
    if (name.empty() ||
        (text != name.toUri(ndn::name::UriFormat::CANONICAL) &&
         text != name.toUri(ndn::name::UriFormat::ALTERNATE)) ||
        !names.insert(name).second ||
        manifest.packetOffsets[i + 1] <= manifest.packetOffsets[i] ||
        manifest.packetOffsets[i + 1] - manifest.packetOffsets[i] > ndn::MAX_NDN_PACKET_SIZE)
      throw std::invalid_argument("repo-invalid-original-packet-index");
  }
}

bool
hasDataReceiptMetadata(const RepoObjectManifest& manifest)
{
  return !manifest.dataReceiptId.empty() ||
         !manifest.dataReceiptRepoNode.empty() ||
         !manifest.dataReceiptProviderIdentity.empty() ||
         manifest.dataReceiptCommittedAtMs != 0;
}

void
rejectExternallySuppliedDataReceipt(const RepoObjectManifest& manifest)
{
  if (hasDataReceiptMetadata(manifest)) {
    throw std::invalid_argument("repo-data-receipt-is-server-owned");
  }
}

void
rejectDataPacketReplacement(const RepoObjectManifest& previous)
{
  if (previous.objectType == "ndn-data-wire") {
    throw std::runtime_error(
      "repo-data-wire-conflict: immutable NDN Data name conflict: " +
      previous.objectName);
  }
}

bool
isAmbiguousCommitError(const std::exception& error)
{
  constexpr char prefix[] = "repo-file-ambiguous-commit:";
  const std::string message = error.what();
  return message.compare(0, sizeof(prefix) - 1, prefix) == 0;
}

bool
matchesPublication(const RepoObjectManifest& expected, const RepoObjectManifest& current)
{
  if (expected.operationId.empty() || expected.operationId != current.operationId)
    return false;
  if (expected.generation != 0)
    return expected.hasSameMetadata(current);
  auto normalized = expected;
  // A failed commit may have become durable before its return value reached
  // the caller. The unique operation identity still fences its rollback.
  if (normalized.generation == 0)
    normalized.generation = current.generation;
  return normalized.hasSameMetadata(current);
}

std::string
packetBaseName(const std::string& alternatePacket)
{
  const auto slash = alternatePacket.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return {};
  return alternatePacket.substr(0, slash);
}

size_t
packetBaseKeyCharge(const std::string& base)
{
  return sizeof(std::string) + sizeof(std::vector<std::string>) +
    4 * sizeof(void*) + base.size() + 1;
}

size_t
packetBaseHolderCharge(const std::string& object)
{
  return sizeof(std::string) + object.size() + 1;
}

bool
addPacketBaseHint(std::unordered_map<std::string, std::vector<std::string>>& index,
                  size_t& bytes, size_t limit, const std::string& base,
                  const std::string& object)
{
  if (base.empty() || base.size() > limit || object.size() > limit) return false;
  auto found = index.find(base);
  if (found != index.end() &&
      std::find(found->second.begin(), found->second.end(), object) != found->second.end())
    return true;
  const auto keyBytes = found == index.end() ? packetBaseKeyCharge(base) : 0;
  const auto holderBytes = packetBaseHolderCharge(object);
  if (bytes > limit || keyBytes > limit - bytes || holderBytes > limit - bytes - keyBytes)
    return false;
  const bool inserted = found == index.end();
  if (inserted) found = index.emplace(base, std::vector<std::string>{}).first;
  try {
    found->second.push_back(object);
  }
  catch (...) {
    if (inserted) index.erase(found);
    throw;
  }
  bytes += keyBytes + holderBytes;
  return true;
}

} // namespace

RepoCore::RepoCore(StorageCapability capability, std::shared_ptr<RepoStoreBackend> store)
  : m_capability(std::move(capability))
  , m_capacityBytes(m_capability.freeBytes + m_capability.usedBytes)
  , m_store(std::move(store))
{
  if (m_store == nullptr) {
    throw std::invalid_argument("repo store backend must not be null");
  }
  refreshCapabilityUsage();
  // The change journal is process-local. An empty durable store may still
  // have deleted objects whose tombstones disappeared across a restart.
  m_catalogReconciliationRequired = true;
  try {
    buildPacketBaseIndex();
  }
  catch (const std::exception& error) {
    // Lookups keep the durable fallback if an unusual legacy manifest
    // cannot be enumerated during startup.
    NDN_LOG_WARN("NDNSF_REPO_PACKET_INDEX_UNAVAILABLE " << error.what());
  }
}

void
RepoCore::buildPacketBaseIndex() const
{
  {
    std::lock_guard<std::mutex> lock(m_packetBaseIndexMutex);
    if (m_packetBaseIndexBuilt) return;
  }
  std::unordered_map<std::string, std::vector<std::string>> index;
  size_t bytes = 0;
  for (const auto& manifest : m_store->listManifests()) {
    if (manifest.objectType != "original-packet-set" || manifest.objectName.empty() ||
        manifest.packetNames.empty() || manifest.objectName != manifest.packetNames.front())
      continue;
    const auto canonical = ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::CANONICAL);
    addPacketBaseHint(index, bytes, MAX_PACKET_INDEX_BYTES,
                      packetBaseName(canonical), manifest.objectName);
  }
  std::scoped_lock lock(m_packetBaseIndexMutex, m_packetIndexProofMutex);
  if (m_packetBaseIndexBuilt) return;
  m_packetIndexProofs.clear();
  m_packetBaseIndex = std::move(index);
  m_packetIndexProofBytes = bytes;
  m_packetBaseIndexBuilt = true;
}

std::optional<std::pair<std::shared_ptr<const RepoObjectManifest>, size_t>>
RepoCore::findIndexedOriginalPacket(const ndn::Name& name, const std::string& canonical,
  const std::string& alternate,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject) const
{
  buildPacketBaseIndex();
  const auto base = packetBaseName(canonical);
  std::vector<std::string> candidates;
  {
    std::scoped_lock lock(m_packetBaseIndexMutex, m_packetIndexProofMutex);
    const auto found = m_packetBaseIndex.find(base);
    if (found == m_packetBaseIndex.end()) return std::nullopt;
    candidates = found->second;
  }
  for (const auto& objectName : candidates) {
    std::shared_ptr<const RepoObjectManifest> manifest;
    try {
      manifest = detail::readManifestSnapshot(*m_store, objectName);
    }
    catch (const std::out_of_range&) {
      continue;
    }
    if (manifest->objectType != "original-packet-set" || manifest->objectName != objectName)
      continue;
    if (!m_store->hasCommittedPayload(*manifest)) continue;
    const auto proof = getPacketIndexProof(*manifest, manifest);
    auto index = manifest->packetNames.size();
    const auto matches = [&](size_t candidate) {
      return candidate < manifest->packetNames.size() &&
        (manifest->packetNames[candidate] == canonical ||
         manifest->packetNames[candidate] == alternate);
    };
    bool numeric = false;
    if (proof && !name.empty() && name[-1].isSegment()) {
      try {
        const auto segment = name[-1].toSegment();
        numeric = true;
        const auto table = proof->segments.find(base);
        if (table != proof->segments.end() && !table->second.empty()) {
          const auto& positions = table->second;
          const auto first = positions.front().segment;
          // Dense segments use one ordinal access. No allocation depends on the
          // numeric value; sparse/high values use the same bounded sorted table.
          if (segment >= first && segment - first < positions.size()) {
            const auto& position = positions[static_cast<size_t>(segment - first)];
            if (position.segment == segment && matches(position.packetIndex))
              index = position.packetIndex;
          }
          if (index == manifest->packetNames.size()) {
            auto position = std::lower_bound(positions.begin(), positions.end(), segment,
              [](const auto& item, uint64_t wanted) { return item.segment < wanted; });
            for (; position != positions.end() && position->segment == segment; ++position) {
              if (matches(position->packetIndex)) {
                index = position->packetIndex;
                break;
              }
            }
          }
        }
      }
      catch (const ndn::tlv::Error&) {
        // Unusual typed components retain exact-name lookup without numeric conversion.
      }
    }
    if (!numeric) {
      auto found = std::find(manifest->packetNames.begin(), manifest->packetNames.end(), canonical);
      if (found == manifest->packetNames.end() && alternate != canonical)
        found = std::find(manifest->packetNames.begin(), manifest->packetNames.end(), alternate);
      if (found != manifest->packetNames.end())
        index = static_cast<size_t>(found - manifest->packetNames.begin());
    }
    if (index != manifest->packetNames.size()) {
      if (!acceptObject || acceptObject(*manifest))
        return std::make_pair(std::move(manifest), index);
    }
  }
  return std::nullopt;
}

RepoObjectManifest
RepoCore::put(const std::string& objectName,
              const std::vector<uint8_t>& payload,
              const std::string& objectType,
              uint32_t replicationFactor,
              const std::string& policyEpoch,
              std::vector<std::string> replicaNodes)
{
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.objectType = objectType;
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.replicationFactor = replicationFactor;
  manifest.replicaNodes = std::move(replicaNodes);
  manifest.policyEpoch = policyEpoch;
  const auto response = handleStore(encodeStoreRequest(manifest, payload));
  return parseManifestJson(toString(response));
}

std::vector<uint8_t>
RepoCore::get(const std::string& objectName) const
{
  return handleFetch(toBytes(objectName));
}

bool
RepoCore::has(const std::string& objectName) const
{
  return m_store->has(resolveObjectKey(objectName)) || hasDataPacket(objectName);
}

std::string
RepoCore::resolveObjectKey(const std::string& objectName) const
{
  // Generic Repo objects retain their literal identifiers. Only exact Data
  // packet records use Name identity; never reinterpret legacy envelopes.
  if (m_store->has(objectName))
    return objectName;
  std::string canonical;
  try {
    canonical = ndn::Name(objectName).toUri(ndn::name::UriFormat::CANONICAL);
  }
  catch (const std::exception&) {
    return objectName;
  }
  if (canonical != objectName && m_store->has(canonical) &&
      (m_store->getManifest(canonical).objectType == "ndn-data-wire" ||
       m_store->getManifest(canonical).objectType == "original-packet-set"))
    return canonical;
  return objectName;
}

RepoObjectManifest
RepoCore::getManifest(const std::string& objectName) const
{
  return parseManifestJson(toString(handleManifest(toBytes(objectName))));
}

RepoObjectMetadataProbe
RepoCore::probeObjectMetadata(const std::string& objectName) const
{
  std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
  if (!supportsConcurrentCommittedReads())
    lock.lock();
  return m_store->probeObjectMetadata(objectName);
}

RepoObjectMetadataSnapshot
RepoCore::probeObjectMetadataSnapshot(const std::string& objectName) const
{
  std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
  if (!supportsConcurrentCommittedReads())
    lock.lock();
  return detail::probeObjectMetadataSnapshot(*m_store, objectName);
}

std::vector<RepoObjectManifest>
RepoCore::list() const
{
  return m_store->listManifests();
}

bool
RepoCore::remove(const std::string& objectName)
{
  return toString(handleDelete(toBytes(objectName))) == "deleted";
}

bool
RepoCore::confirmUserStorageRemoval(const RepoUserStorageObject& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!canRemoveUserStorageObjectLocked(expected) ||
      !m_store->confirmUserStorageRemoval(expected)) {
    return false;
  }
  refreshCapabilityUsage();
  return true;
}

bool
RepoCore::removeUserStorageObject(const RepoUserStorageObject& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!canRemoveUserStorageObjectLocked(expected)) return false;
  if (m_store->probeObjectMetadata(expected.objectName).manifestPresent)
    (void)deleteLocked(expected.objectName);
  if (!m_store->confirmUserStorageRemoval(expected)) return false;
  refreshCapabilityUsage(); // Confirmation may have removed a previously charged orphan.
  return true;
}

bool
RepoCore::canRemoveUserStorageObject(const RepoUserStorageObject& expected) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return canRemoveUserStorageObjectLocked(expected);
}

bool
RepoCore::canRemoveUserStorageObjectLocked(const RepoUserStorageObject& expected) const
{
  validateRepoUserStorageObject(expected);
  for (const auto& reservation : m_rangeReservations) {
    if (reservation.first == expected.objectName || reservation.second.digest == expected.wireSha256)
      return false;
  }
  if (!m_store->canRemoveUserStorageObject(expected)) return false;
  const auto probe = m_store->probeObjectMetadata(expected.objectName);
  if (probe.manifestPresent) {
    if (!probe.manifestMeasurementAvailable || probe.manifest.objectName != expected.objectName ||
        probe.manifest.sha256 != expected.wireSha256 || probe.manifest.generation != expected.generation ||
        probe.manifest.size != expected.payloadWireBytes || probe.manifestWireBytes != expected.manifestBytes)
      return false;
  }
  return true;
}

RepoObjectManifest
RepoCore::putManifest(const RepoObjectManifest& manifest)
{
  return parseManifestJson(toString(handleStoreManifest(encodeManifestRequest(manifest))));
}

void
RepoCore::putRange(const RepoObjectManifest& manifest, RepoByteRange range,
                   const std::vector<uint8_t>& bytes)
{
  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");
  std::lock_guard<std::mutex> lock(m_mutex);
  putRangeLocked(manifest, range, bytes);
}

void
RepoCore::putRangeIfAbsent(const RepoObjectManifest& manifest, RepoByteRange range,
                          const std::vector<uint8_t>& bytes)
{
  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");
  std::lock_guard<std::mutex> lock(m_mutex);
  if (manifest.operationId.empty() || m_store->has(manifest.objectName))
    throw std::runtime_error("repo-publication-name-conflict");
  putRangeLocked(manifest, range, bytes);
}

void
RepoCore::putRangeLocked(const RepoObjectManifest& manifest, RepoByteRange range,
                         const std::vector<uint8_t>& bytes)
{
  if (hasDataPacket(manifest.objectName))
    throw std::runtime_error("repo-original-packet-is-immutable");
  putAdmittedRangeLocked(manifest, range, bytes);
}

void
RepoCore::putAdmittedRangeLocked(const RepoObjectManifest& manifest, RepoByteRange range,
                                const std::vector<uint8_t>& bytes)
{
  rejectExternallySuppliedDataReceipt(manifest);
  if (range.lengthBytes != bytes.size() ||
      range.offsetBytes > manifest.size ||
      range.lengthBytes > manifest.size - range.offsetBytes) {
    throw std::out_of_range("repo-core-range-write-out-of-bounds");
  }
  uint64_t generation = manifest.generation;
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    rejectDataPacketReplacement(previous);
    oldSize = previous.segmentCount > 1 || !previous.packetNames.empty()
                ? 0 : previous.size;
    if (generation == 0) {
      if (previous.generation == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("repo-generation-exhausted");
      }
      generation = previous.generation == 0 ? 2 : previous.generation + 1;
    }
  }
  if (generation == 0) {
    generation = 1;
  }
  auto found = m_rangeReservations.find(manifest.objectName);
  std::optional<RepoObjectManifest> normalized;
  if (manifest.generation != generation) {
    normalized = manifest;
    normalized->generation = generation;
  }
  const auto& rangeManifest = normalized ? *normalized : manifest;
  if (found == m_rangeReservations.end()) {
    const auto additionalBytes = manifest.size > oldSize
      ? manifest.size - oldSize : 0;
    const auto availableForReservation = m_capability.freeBytes > m_reservedRangeBytes
      ? m_capability.freeBytes - m_reservedRangeBytes : 0;
    if (additionalBytes > availableForReservation) {
      throw std::runtime_error("repo node has insufficient free space for range object: " +
                               manifest.objectName);
    }
    found = m_rangeReservations.emplace(
      manifest.objectName,
      RangeReservation{rangeManifest.toJson(), manifest.sha256, manifest.size,
                       generation, additionalBytes,
                       std::chrono::steady_clock::now()}).first;
    m_reservedRangeBytes += additionalBytes;
  }
  else if (found->second.digest != manifest.sha256 ||
           found->second.size != manifest.size ||
           found->second.generation != generation ||
           found->second.canonicalIdentity != rangeManifest.toJson()) {
    throw std::runtime_error("repo-generation-conflict: core range reservation identity mismatch");
  }
  try {
    m_store->putRange(rangeManifest, range, bytes);
    found->second.lastActivity = std::chrono::steady_clock::now();
  }
  catch (...) {
    if (!rangeManifest.operationId.empty()) {
      (void)abortFailedOwnedRangeIfUncommittedLocked(rangeManifest);
    }
    else {
      // Trusted-local legacy writes do not carry an owned transaction identity.
      try { m_store->abortRanges(manifest.objectName); } catch (...) {}
      clearRangeReservation(manifest.objectName);
    }
    // A failed finalize/metadata publication may have left a physical
    // payload residue even after the best-effort abort.  Reconcile the
    // capacity view before exposing the failure to the next caller.
    m_catalogReconciliationRequired = true;
    refreshCapabilityUsageAfterCommit(oldSize, oldSize);
    throw;
  }
}

RepoObjectManifest
RepoCore::commitRanges(const RepoObjectManifest& manifest)
{
  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");
  std::lock_guard<std::mutex> lock(m_mutex);
  return commitRangesLocked(manifest);
}

RepoObjectManifest
RepoCore::confirmDurablePublicationIfCurrent(const RepoObjectManifest& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if ((expected.objectType != "original-packet-set" &&
       expected.objectType != "encrypted-large-data-envelope") ||
      expected.generation == 0 || expected.operationId.empty() ||
      !m_store->has(expected.objectName) ||
      !m_store->getManifest(expected.objectName).hasSameMetadata(expected))
    throw std::runtime_error("repo-publication-identity-mismatch");
  const auto reservation = m_rangeReservations.find(expected.objectName);
  if (reservation != m_rangeReservations.end() &&
      (reservation->second.generation != expected.generation ||
       reservation->second.canonicalIdentity != expected.toJson()))
    throw std::runtime_error("repo-publication-identity-mismatch");
  if (expected.objectType == "original-packet-set") validatePacketIndex(expected);
  return commitRangesLocked(expected);
}

RepoObjectManifest
RepoCore::commitRangesIfOwned(const RepoObjectManifest& manifest)
{
  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto reservation = m_rangeReservations.find(manifest.objectName);
  if (m_store->has(manifest.objectName) || reservation == m_rangeReservations.end() ||
      !matchesPublication(manifest, parseManifestJson(reservation->second.canonicalIdentity)))
    throw std::runtime_error("repo-publication-identity-mismatch");
  return commitRangesLocked(manifest);
}

RepoObjectManifest
RepoCore::commitRangesLocked(const RepoObjectManifest& manifest)
{
  rejectExternallySuppliedDataReceipt(manifest);
  auto commitManifest = manifest;
  const auto reservation = m_rangeReservations.find(manifest.objectName);
  if (reservation != m_rangeReservations.end()) {
    commitManifest.generation = reservation->second.generation;
    auto identityManifest = parseManifestJson(reservation->second.canonicalIdentity);
    auto candidateManifest = manifest;
    candidateManifest.generation = reservation->second.generation;
    if (candidateManifest.toJson() != identityManifest.toJson()) {
      throw std::runtime_error("repo-generation-conflict: commit reservation identity mismatch");
    }
    commitManifest = std::move(identityManifest);
  }
  else if (commitManifest.generation == 0 && m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    if (previous.generation == std::numeric_limits<uint64_t>::max()) {
      throw std::runtime_error("repo-generation-exhausted");
    }
    commitManifest.generation = previous.generation == 0 ? 2 : previous.generation + 1;
  }
  else if (commitManifest.generation == 0) {
    commitManifest.generation = 1;
  }
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    rejectDataPacketReplacement(previous);
    oldSize = previous.segmentCount > 1 || !previous.packetNames.empty()
                ? 0 : previous.size;
  }
  bool payloadAlreadyCommitted = false;
  try {
    if (commitManifest.objectType == "original-packet-set") {
      payloadAlreadyCommitted = m_store->hasCommittedPayload(commitManifest);
    }
    m_store->commitRanges(commitManifest);
    // A successful backend commit is the durable-manifest contract.  The
    // lookup normally confirms the exact stored value; if a transient read
    // fails, return the canonical identity used for the commit instead of
    // reporting failure after the object is already visible.
    auto durable = commitManifest;
    try {
      durable = m_store->getManifest(manifest.objectName);
    }
    catch (...) {
    }
    rememberCatalogChange(durable, "AVAILABLE");
    if (commitManifest.objectType == "original-packet-set" &&
        !payloadAlreadyCommitted)
      updateCapabilityUsage(oldSize, durable.size);
    else
      refreshCapabilityUsageAfterCommit(oldSize, durable.size);
    clearRangeReservation(manifest.objectName);
    return durable;
  }
  catch (...) {
    try {
      throw;
    }
    catch (const std::exception& e) {
      RepoObjectManifest durable;
      bool settled = commitManifest.operationId.empty();
      if (!settled && isAmbiguousCommitError(e)) {
        try {
          const auto recovered = m_store->getManifest(commitManifest.objectName);
          if (recovered.hasSameMetadata(commitManifest)) {
            m_store->commitRanges(commitManifest);
            settled = true;
          }
        }
        catch (...) {
          // Visible metadata does not confirm a failed directory fsync.
        }
      }
      if (settled && isAmbiguousCommitError(e) &&
          recoverAmbiguousCommit(manifest.objectName, durable)) {
        if (commitManifest.objectType == "original-packet-set" &&
            !payloadAlreadyCommitted)
          updateCapabilityUsage(oldSize, durable.size);
        else
          refreshCapabilityUsageAfterCommit(oldSize, durable.size);
        clearRangeReservation(manifest.objectName);
        return durable;
      }
    }
    catch (...) {
      // Non-standard failures need the same guarded rollback confirmation.
    }
    if (!commitManifest.operationId.empty()) {
      (void)abortFailedOwnedRangeIfUncommittedLocked(commitManifest);
    }
    else {
      try { m_store->abortRanges(manifest.objectName); } catch (...) {}
      clearRangeReservation(manifest.objectName);
    }
    m_catalogReconciliationRequired = true;
    refreshCapabilityUsageAfterCommit(oldSize, oldSize);
    throw;
  }
}

void
RepoCore::abortRanges(const std::string& objectName)
{
  if (objectName.empty()) {
    throw std::invalid_argument("repo range abort object name must not be empty");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_store->abortRanges(objectName);
  clearRangeReservation(objectName);
}

std::vector<uint8_t>
RepoCore::getRange(const std::string& objectName, RepoByteRange range) const
{
  if (!m_store->supportsRange()) {
    if (!m_store->supportsManifestLookup()) {
      throw std::runtime_error("repo-range-backend-metadata-lookup-not-supported");
    }
    const auto manifest = m_store->getManifest(objectName);
    constexpr uint64_t LegacyVectorThreshold = 1U << 20;
    if (manifest.size > LegacyVectorThreshold) {
      throw std::runtime_error("repo-range-backend-not-supported-for-large-object");
    }
  }
  return m_store->getRange(objectName, range);
}

std::vector<uint8_t>
RepoCore::getRangeIfCurrent(const RepoObjectManifest& expected, RepoByteRange range) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (expected.generation == 0 || !m_store->has(expected.objectName) ||
      !matchesPublication(expected, m_store->getManifest(expected.objectName)))
    throw std::runtime_error("repo-publication-identity-mismatch");
  return getRange(expected.objectName, range);
}

RepoRangeReadSnapshot
RepoCore::openRangeSnapshot(std::shared_ptr<RepoCore> owner, RepoObjectManifest expected)
{
  if (!owner || expected.generation == 0 || expected.operationId.empty())
    throw std::invalid_argument("repo-range-snapshot-owner-required");
  auto manifest = std::make_shared<const RepoObjectManifest>(std::move(expected));
  std::lock_guard<std::mutex> lock(owner->m_mutex);
  if (!owner->m_store->supportsRange() &&
      (!owner->m_store->supportsManifestLookup() || manifest->size > (1U << 20)))
    throw std::runtime_error("repo-range-backend-not-supported-for-large-object");
  auto read = owner->m_store->bindCurrentRangeReader(manifest);
  return {manifest, [owner, read = std::move(read)](RepoByteRange range) {
    std::lock_guard<std::mutex> guard(owner->m_mutex);
    return read(range);
  }};
}

bool
RepoCore::removeIfCurrent(const RepoObjectManifest& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_store->has(expected.objectName) ||
      !matchesPublication(expected, m_store->getManifest(expected.objectName)))
    return false;
  // Do not abort another transaction's in-progress replacement.
  const auto reservation = m_rangeReservations.find(expected.objectName);
  if (reservation != m_rangeReservations.end())
    return false;
  return toString(deleteLocked(expected.objectName)) == "deleted";
}

bool
RepoCore::abortRangesIfOwned(const RepoObjectManifest& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto reservation = m_rangeReservations.find(expected.objectName);
  if (reservation == m_rangeReservations.end() ||
      !matchesPublication(expected, parseManifestJson(reservation->second.canonicalIdentity)))
    return false;
  m_store->abortRanges(expected.objectName);
  clearRangeReservation(expected.objectName);
  return true;
}

bool
RepoCore::abortUserStorageRangesIfOwned(const RepoObjectManifest& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return abortUserStorageRangesIfOwnedLocked(expected);
}

bool
RepoCore::abortUserStorageWriteRangesIfOwned(const RepoUserStorageWrite& expected)
{
  validateRepoUserStorageObject(expected.object);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (expected.state != RepoUserStorageWriteState::CANCELLED || expected.reservedBytes != 0 ||
      expected.operationId.empty() || expected.grant.repoNode != m_capability.repoNode ||
      expected.grant.ownerIdentity.empty() || expected.grant.grantId.empty() ||
      expected.grant.writerIdentity.empty() || expected.grant.ownerEpoch == 0 ||
      expected.grant.objectName != expected.object.objectName)
    return false;
  const auto owned = m_rangeReservations.find(expected.object.objectName);
  if (owned == m_rangeReservations.end()) return false;
  const auto manifest = parseManifestJson(owned->second.canonicalIdentity);
  if (manifest.toJson() != owned->second.canonicalIdentity ||
      manifest.operationId != expected.operationId || manifest.objectName != expected.object.objectName ||
      manifest.sha256 != expected.object.wireSha256 || manifest.generation != expected.object.generation ||
      manifest.size != expected.object.payloadWireBytes ||
      owned->second.canonicalIdentity.size() != expected.object.manifestBytes)
    return false;
  return abortUserStorageRangesIfOwnedLocked(manifest);
}

bool
RepoCore::abortFailedRangesIfOwned(const RepoObjectManifest& expected)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return abortFailedOwnedRangeIfUncommittedLocked(expected);
}

bool
RepoCore::abortUserStorageRangesIfOwnedLocked(const RepoObjectManifest& expected)
{
  if (expected.generation == 0 || expected.operationId.empty()) return false;
  const auto owned = m_rangeReservations.find(expected.objectName);
  if (owned == m_rangeReservations.end() || owned->second.canonicalIdentity != expected.toJson())
    return false;
  for (const auto& reservation : m_rangeReservations) {
    if (reservation.first != expected.objectName && reservation.second.digest == expected.sha256)
      return false;
  }
  if (!m_store->abortUserStorageRangesIfOwned(expected)) return false;
  clearRangeReservation(expected.objectName);
  return true;
}

bool
RepoCore::abortFailedOwnedRangeIfUncommittedLocked(const RepoObjectManifest& expected)
{
  try {
    if (expected.generation == 0 || expected.operationId.empty()) return false;
    const auto owned = m_rangeReservations.find(expected.objectName);
    if (owned != m_rangeReservations.end() && owned->second.canonicalIdentity != expected.toJson())
      return false;
    for (const auto& reservation : m_rangeReservations) {
      if (reservation.first != expected.objectName && reservation.second.digest == expected.sha256)
        return false;
    }
    // Stage absence cannot prove rollback after a manifest was published.
    // Probe the backend directly: the caller already holds the Core mutex.
    const auto probe = m_store->probeObjectMetadata(expected.objectName);
    const bool present = m_store->has(expected.objectName);
    if (present != probe.manifestPresent) {
      m_catalogReconciliationRequired = true;
      return false;
    }
    if (present && (!probe.manifestMeasurementAvailable ||
                    ndn::Name(probe.manifest.objectName) != ndn::Name(expected.objectName) ||
                    (probe.manifest.operationId == expected.operationId &&
                     probe.manifest.generation == expected.generation))) {
      m_catalogReconciliationRequired = true;
      return false;
    }
    // A measured different transaction's committed bytes remain untouched.
    if (m_store->abortFailedRangesIfOwned(expected)) {
      clearRangeReservation(expected.objectName);
      return true;
    }
  }
  catch (...) {
    // Cleanup uncertainty must retain capacity without replacing the original error.
  }
  m_catalogReconciliationRequired = true;
  return false;
}

uint64_t
RepoCore::reapExpiredRangeReservations(std::chrono::steady_clock::duration idleTimeout)
{
  if (idleTimeout < std::chrono::steady_clock::duration::zero()) {
    throw std::invalid_argument("repo-range-idle-timeout-invalid");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto expiredBefore = std::chrono::steady_clock::now() - idleTimeout;
  uint64_t reaped = 0;
  for (auto reservation = m_rangeReservations.begin();
       reservation != m_rangeReservations.end();) {
    if (reservation->second.lastActivity > expiredBefore) {
      ++reservation;
      continue;
    }
    try {
      const auto expected = parseManifestJson(reservation->second.canonicalIdentity);
      if (!expected.operationId.empty()) {
        ++reservation; // The protected helper may erase this reservation.
        if (abortFailedOwnedRangeIfUncommittedLocked(expected)) ++reaped;
        continue;
      }
    }
    catch (...) {
      m_catalogReconciliationRequired = true;
      ++reservation;
      continue;
    }
    try {
      m_store->abortRanges(reservation->first);
    }
    catch (...) {
      ++reservation;
      continue;
    }
    m_reservedRangeBytes -= std::min(m_reservedRangeBytes,
                                     reservation->second.additionalBytes);
    reservation = m_rangeReservations.erase(reservation);
    ++reaped;
  }
  return reaped;
}

RepoObjectManifest
RepoCore::putDataPacket(const std::string& dataName,
                        const std::vector<uint8_t>& wire,
                        const std::string& repoNode,
                        const std::string& providerIdentity)
{
  if (dataName.empty()) {
    throw std::invalid_argument("NDN Data name must not be empty");
  }
  if (repoNode.empty() != providerIdentity.empty()) {
    throw std::invalid_argument("repo-data-receipt-identity-incomplete");
  }
  std::string encodedName;
  try {
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    block.parse();
    const ndn::Data data(block);
    if (data.wireEncode().size() != wire.size())
      throw std::invalid_argument("trailing bytes after Data packet");
    encodedName = data.getName().toUri(ndn::name::UriFormat::CANONICAL);
  }
  catch (const std::exception& e) {
    throw std::invalid_argument(std::string("repo-invalid-data-wire: ") + e.what());
  }
  if (ndn::Name(encodedName) != ndn::Name(dataName)) {
    throw std::invalid_argument(
      "repo-data-name-mismatch: declared=" + dataName + " encoded=" + encodedName);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (hasDataPacket(encodedName)) {
    if (getDataPacket(encodedName) != wire)
      throw std::runtime_error("repo-data-wire-conflict: " + encodedName);
    const auto record = findPacketRecord(ndn::Name(encodedName));
    if (record && record->first.objectType == "ndn-data-wire")
      return record->first;
    if (!m_store->has(encodedName) ||
        m_store->getManifest(encodedName).objectType == "original-packet-set") {
      RepoObjectManifest packet;
      packet.objectName = encodedName;
      packet.objectType = "ndn-data-wire";
      packet.sha256 = sha256Hex(wire);
      packet.size = wire.size();
      packet.packetNames = {encodedName};
      packet.generation = 1;
      return packet;
    }
  }
  if (m_store->has(encodedName)) {
    const auto stored = m_store->get(encodedName);
    if (stored.manifest.objectType != "ndn-data-wire" || stored.payload != wire) {
      throw std::runtime_error(
        "repo-data-wire-conflict: immutable NDN Data name conflict: " + dataName);
    }
    return stored.manifest;
  }
  // Range publications reserve their eventual payload bytes before the
  // manifest becomes visible.  A packet write must compete for the same
  // logical quota, otherwise it can consume bytes promised to another
  // in-flight publication and make the admission decision optimistic.
  const auto availableBytes = m_capability.freeBytes > m_reservedRangeBytes
    ? m_capability.freeBytes - m_reservedRangeBytes : 0;
  if (wire.size() > availableBytes) {
    throw std::runtime_error("repo node has insufficient free space for Data packet: " +
                             dataName);
  }

  RepoObjectManifest manifest;
  manifest.objectName = encodedName;
  manifest.objectType = "ndn-data-wire";
  manifest.sha256 = sha256Hex(wire);
  manifest.size = wire.size();
  manifest.segmentCount = 1;
  manifest.packetNames = {encodedName};
  manifest.generation = 1;
  if (!repoNode.empty()) {
    const auto committedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream nonce;
    nonce << std::hex << ndn::random::generateWord64()
          << ndn::random::generateWord64();
    manifest.dataReceiptId = "repo-data-" + sha256Hex(toBytes(
      repoNode + "\n" + providerIdentity + "\n" + dataName + "\n" +
      manifest.sha256 + "\n" + nonce.str()));
    manifest.dataReceiptRepoNode = repoNode;
    manifest.dataReceiptProviderIdentity = providerIdentity;
    manifest.dataReceiptCommittedAtMs = static_cast<uint64_t>(committedAt);
  }
  try {
    m_store->put(manifest, wire);
  }
  catch (const std::exception& e) {
    RepoObjectManifest durable;
    if (!isAmbiguousCommitError(e) ||
        !recoverAmbiguousCommit(encodedName, durable)) {
      // The backend may have finalized a payload before a metadata/cleanup
      // failure was reported.  Refresh physical usage before rethrowing so a
      // residue cannot make the capacity view optimistic.
      m_catalogReconciliationRequired = true;
      refreshCapabilityUsageAfterCommit(0, 0);
      throw;
    }
    refreshCapabilityUsageAfterCommit(0, durable.size);
    return durable;
  }
  auto durable = manifest;
  try {
    durable = m_store->getManifest(encodedName);
  }
  catch (...) {
    m_catalogReconciliationRequired = true;
  }
  rememberCatalogChange(durable, "AVAILABLE");
  refreshCapabilityUsageAfterCommit(0, durable.size);
  return durable;
}

RepoWriteReceipt
RepoCore::importLegacyDataPacketReceipt(const std::string& dataName,
                                        const std::string& repoNode,
                                        const std::string& providerIdentity)
{
  if (dataName.empty() || repoNode.empty() || providerIdentity.empty() ||
      repoNode != m_capability.repoNode) {
    throw std::invalid_argument("repo-data-import-identity-invalid");
  }
  std::string digest;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto stored = m_store->get(dataName);
    const auto& manifest = stored.manifest;
    if (manifest.objectName != dataName || manifest.objectType != "ndn-data-wire" ||
        manifest.size == 0 || manifest.size != stored.payload.size() ||
        manifest.sha256 != sha256Hex(stored.payload) ||
        manifest.packetNames != std::vector<std::string>{dataName} ||
        manifest.generation == 0) {
      throw std::runtime_error("repo-data-import-wire-invalid");
    }
    try {
      ndn::Block block(ndn::span<const uint8_t>(stored.payload.data(),
                                               stored.payload.size()));
      block.parse();
      if (ndn::Data(block).getName() != ndn::Name(dataName)) {
        throw std::runtime_error("repo-data-import-name-mismatch");
      }
    }
    catch (const std::exception& e) {
      throw std::runtime_error(std::string("repo-data-import-wire-invalid: ") + e.what());
    }
    digest = manifest.sha256;
    if (!manifest.dataReceiptId.empty()) {
      if (manifest.dataReceiptRepoNode != repoNode ||
          manifest.dataReceiptProviderIdentity != providerIdentity) {
        throw std::runtime_error("repo-data-import-identity-conflict");
      }
    }
    else {
      auto updated = manifest;
      const auto importedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      std::ostringstream nonce;
      nonce << std::hex << ndn::random::generateWord64()
            << ndn::random::generateWord64();
      updated.dataReceiptId = "repo-import-data-" + sha256Hex(toBytes(
        repoNode + "\n" + providerIdentity + "\n" + dataName + "\n" +
        digest + "\n" + nonce.str()));
      updated.dataReceiptRepoNode = repoNode;
      updated.dataReceiptProviderIdentity = providerIdentity;
      updated.dataReceiptCommittedAtMs = static_cast<uint64_t>(importedAt);
      m_store->attachImportedDataReceipt(manifest, updated);
    }
  }
  return getDataPacketReceipt(dataName, digest);
}

std::vector<uint8_t>
RepoCore::getDataPacket(const std::string& dataName) const
{
  return getDataPacket(dataName, {});
}

std::vector<uint8_t>
RepoCore::getDataPacket(const std::string& dataName,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject) const
{
  const ndn::Name name(dataName);
  const auto readPacket = [&](const RepoObjectManifest& manifest, size_t index) {
    auto wire = manifest.objectType == "ndn-data-wire"
      ? m_store->get(manifest.objectName).payload
      : m_store->getRange(manifest.objectName,
          {manifest.packetOffsets[index],
           manifest.packetOffsets[index + 1] - manifest.packetOffsets[index]});
    const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    if (data.getName() != name)
      throw std::runtime_error("repo-original-packet-name-mismatch");
    if (acceptObject && !acceptObject(manifest))
      throw std::runtime_error("repo-original-packet-admission-changed");
    return wire;
  };
  if (acceptObject) {
    const auto key = name.toUri(ndn::name::UriFormat::CANONICAL);
    const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
    if (!m_store->has(key) && (alternate == key || !m_store->has(alternate))) {
      if (const auto indexed = findIndexedOriginalPacket(name, key, alternate, acceptObject))
        return readPacket(*indexed->first, indexed->second);
      std::vector<std::pair<std::string, size_t>> hints;
      {
        std::lock_guard<std::mutex> lock(m_packetIndexProofMutex);
        for (const auto& [object, proof] : m_packetIndexProofs) {
          const auto found = std::find(proof->names.begin(), proof->names.end(), key);
          if (found != proof->names.end())
            hints.emplace_back(object, static_cast<size_t>(found - proof->names.begin()));
        }
      }
      // A proof is only a locator hint. Current metadata and admission remain mandatory;
      // never run storage calls or a caller's visibility callback under the proof mutex.
      for (const auto& [object, index] : hints) {
        std::shared_ptr<const RepoObjectManifest> current;
        try {
          if (m_store->has(object)) current = detail::readManifestSnapshot(*m_store, object);
        }
        catch (const std::out_of_range&) {
          continue;
        }
        if (!current || current->objectType != "original-packet-set" ||
            index >= current->packetNames.size() || current->packetNames[index] != key)
          continue;
        validateReadPacketIndex(*current);
        if (acceptObject(*current))
          return readPacket(*current, index);
      }
    }
  }
  const auto record = findPacketRecord(name, acceptObject);
  if (!record) {
    throw std::runtime_error("repo object is not an exact NDN Data packet: " + dataName);
  }
  return readPacket(record->first, record->second);
}

std::vector<uint8_t>
RepoCore::getDataPacket(
  const ndn::Interest& interest,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject) const
{
  const auto data = getDataPacketObject(interest, acceptObject);
  const auto& wire = data.wireEncode();
  return std::vector<uint8_t>(wire.begin(), wire.end());
}

ndn::Data
RepoCore::getDataPacketObject(
  const ndn::Interest& interest,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject) const
{
  auto indexedName = interest.getName();
  const bool implicitDigest = !indexedName.empty() && indexedName[-1].isImplicitSha256Digest();
  if (implicitDigest) {
    indexedName = indexedName.getPrefix(-1);
  }
  if (!interest.getCanBePrefix() || implicitDigest) {
    auto wire = getDataPacket(indexedName.toUri(ndn::name::UriFormat::CANONICAL), acceptObject);
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    if (interest.matchesData(data)) {
      return data;
    }
    throw std::runtime_error("repo-original-packet-interest-mismatch");
  }
  // SegmentFetcher starts with an object/locator prefix. Read the admitted
  // holder itself: a second exact lookup could select a different owner.
  const auto canonicalPrefix = indexedName.toUri(ndn::name::UriFormat::CANONICAL);
  for (const auto& manifest : detail::findPacketManifests(
         *m_store, canonicalPrefix, indexedName.toUri(ndn::name::UriFormat::ALTERNATE), true)) {
    if (manifest.objectType != "ndn-data-wire" && manifest.objectType != "original-packet-set") {
      continue;
    }
    const bool packetSet = manifest.objectType == "original-packet-set";
    if (packetSet) {
      validateReadPacketIndex(manifest);
    }
    const auto count = packetSet ? manifest.packetNames.size() : size_t{1};
    bool admitted = false;
    for (size_t index = 0; index < count; ++index) {
      const ndn::Name name(packetSet ? manifest.packetNames[index] : manifest.objectName);
      if (!indexedName.isPrefixOf(name)) {
        continue;
      }
      // Object bootstrap is narrower than directory/Producer enumeration.
      const auto suffix = name.size() - indexedName.size();
      const bool direct = suffix == 1 && (name[-1].isSegment() || name[-1].isVersion());
      const bool committed = suffix == 2 && name[-2].type() == 8 &&
        name[-2].value_size() == 32 && name[-1].isSegment();
      if (suffix != 0 && !direct && !committed) {
        continue;
      }
      if (!admitted) {
        if (acceptObject && !acceptObject(manifest)) {
          break;
        }
        admitted = true;
      }
      auto wire = packetSet ? m_store->getRange(manifest.objectName,
          {manifest.packetOffsets[index], manifest.packetOffsets[index + 1] - manifest.packetOffsets[index]})
        : m_store->get(manifest.objectName).payload;
      const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      if (data.getName() != name) {
        throw std::runtime_error("repo-original-packet-name-mismatch");
      }
      if (acceptObject && !acceptObject(manifest))
        break;
      if (interest.matchesData(data)) {
        return data;
      }
    }
  }
  throw std::runtime_error("repo-original-packet-interest-not-found");
}

std::optional<std::vector<ndn::Data>>
RepoCore::tryGetDataPacketBatch(const std::vector<ndn::Interest>& interests,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject,
  const std::function<void()>& requireActive) const
{
  if (interests.empty() || interests.size() > MAX_ORIGINAL_PACKET_READ_BATCH ||
      !acceptObject || !requireActive)
    throw std::invalid_argument("repo-original-packet-batch-invalid");
  if (interests.size() == 1 || !m_store->supportsConcurrentCommittedReads() ||
      !m_store->supportsRange() || !m_store->supportsManifestLookup())
    return std::nullopt;

  std::vector<ndn::Name> names;
  for (const auto& interest : interests) {
    requireActive();
    if (interest.getCanBePrefix() || interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
      return std::nullopt;
    auto name = interest.getName();
    if (!name.empty() && name[-1].isImplicitSha256Digest()) name = name.getPrefix(-1);
    if (name.empty() || !name[-1].isSegment()) return std::nullopt;
    names.push_back(std::move(name));
  }

  std::shared_ptr<const RepoObjectManifest> manifest;
  std::vector<std::size_t> indices;
  const auto requireSameManifest = [&] {
    requireActive();
    const auto current = detail::readManifestSnapshot(*m_store, manifest->objectName);
    if (!current || (current.get() != manifest.get() && !current->hasSameMetadata(*manifest)))
      throw std::runtime_error("repo-original-packet-batch-metadata-changed");
    if (!m_store->hasCommittedPayload(*current))
      throw std::runtime_error("repo-original-packet-batch-payload-missing");
    return current;
  };
  for (const auto& name : names) {
    requireActive();
    if (manifest) {
      // Same-object fast path: this batch already resolved and will re-validate
      // the manifest once at the end, so later names only need their index in
      // that immutable manifest instead of another snapshot read and payload
      // probe per packet.
      const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
      // The alternate spelling is only needed when the canonical form does not
      // match, so it stays unbuilt on the normal path.
      std::optional<std::string> alternate;
      const auto matchesName = [&](std::size_t candidate) {
        if (candidate >= manifest->packetNames.size())
          return false;
        const auto& stored = manifest->packetNames[candidate];
        if (stored == canonical)
          return true;
        if (!alternate)
          alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
        return stored == *alternate;
      };
      std::optional<std::size_t> index;
      const auto proof = getPacketIndexProof(*manifest, manifest);
      if (proof && !name.empty() && name[-1].isSegment()) {
        try {
          const auto segment = name[-1].toSegment();
          const auto table = proof->segments.find(packetBaseName(canonical));
          if (table != proof->segments.end() && !table->second.empty()) {
            const auto& positions = table->second;
            const auto first = positions.front().segment;
            // Dense segments use one ordinal access; sparse or high values use
            // the bounded sorted table, exactly as the exact-read lookup does.
            if (segment >= first && segment - first < positions.size()) {
              const auto& position = positions[static_cast<std::size_t>(segment - first)];
              if (position.segment == segment && matchesName(position.packetIndex))
                index = position.packetIndex;
            }
            if (!index) {
              auto position = std::lower_bound(positions.begin(), positions.end(), segment,
                [](const auto& item, uint64_t wanted) { return item.segment < wanted; });
              for (; position != positions.end() && position->segment == segment; ++position) {
                if (matchesName(position->packetIndex)) {
                  index = position->packetIndex;
                  break;
                }
              }
            }
          }
        }
        catch (const ndn::tlv::Error&) {
          // Non-numeric segments fall back to the exact name comparison below.
        }
      }
      if (!index) {
        for (std::size_t candidate = 0; candidate < manifest->packetNames.size(); ++candidate) {
          if (matchesName(candidate)) {
            index = candidate;
            break;
          }
        }
      }
      // A name outside the already admitted object means this batch spans
      // objects. That is not an admission or integrity failure: the batch is
      // simply not applicable and the caller falls back to exact reads, which
      // repeat the full per-packet validation.
      if (!index)
        return std::nullopt;
      indices.push_back(*index);
      continue;
    }
    bool rejected = false;
    const auto sameHolder = [&](const RepoObjectManifest& candidate) {
      if (!manifest) {
        // Preserve lookup across rejected holders; an admitted holder supplies
        // the batch's pre-read admission without caching that decision.
        const bool accepted = acceptObject(candidate);
        rejected = rejected || !accepted;
        return accepted;
      }
      if (candidate.objectName != manifest->objectName) return false;
      if (&candidate != manifest.get() && !candidate.hasSameMetadata(*manifest))
        throw std::runtime_error("repo-original-packet-batch-metadata-changed");
      return true;
    };
    const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
    const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
    auto record = findIndexedOriginalPacket(name, canonical, alternate, sameHolder);
    if (!record && !manifest) {
      // Restart's base index initially knows root prefixes only. Reuse the
      // authoritative candidate filter for heterogeneous packet-set prefixes.
      for (const auto& candidate : detail::findPacketManifests(
             *m_store, canonical, alternate, false)) {
        requireActive();
        if (candidate.objectType != "original-packet-set") continue;
        std::shared_ptr<const RepoObjectManifest> current;
        try {
          current = detail::readManifestSnapshot(*m_store, candidate.objectName);
        }
        catch (const std::out_of_range&) {
          continue;
        }
        if (!current->hasSameMetadata(candidate))
          throw std::runtime_error("repo-original-packet-batch-metadata-changed");
        if (!m_store->hasCommittedPayload(*current)) continue;
        (void)getPacketIndexProof(*current, current);
        auto position = std::find(current->packetNames.begin(), current->packetNames.end(),
                                  canonical);
        if (position == current->packetNames.end() && alternate != canonical)
          position = std::find(current->packetNames.begin(), current->packetNames.end(), alternate);
        if (position != current->packetNames.end() && sameHolder(*current)) {
          record = std::make_pair(current,
            static_cast<std::size_t>(position - current->packetNames.begin()));
          break;
        }
      }
    }
    if (!record) {
      if (!manifest && rejected)
        throw std::runtime_error("repo-original-packet-batch-admission-rejected");
      if (manifest) (void)requireSameManifest();
      return std::nullopt;
    }
    if (!manifest) manifest = record->first;
    indices.push_back(record->second);
  }

  // From this point failures are authoritative. A caller must discard the
  // entire prepared batch rather than downgrade to individually admitted reads.
  (void)requireSameManifest();
  std::vector<ndn::Data> packets;
  for (std::size_t i = 0; i < indices.size(); ++i) {
    requireActive();
    const auto index = indices[i];
    const auto offset = manifest->packetOffsets[index];
    const auto length = manifest->packetOffsets[index + 1] - offset;
    const auto wire = m_store->getRange(manifest->objectName, {offset, length});
    if (wire.size() != length)
      throw std::runtime_error("repo-original-packet-batch-truncated");
    ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    if (packet.wireEncode().size() != wire.size() || packet.getName() != names[i] ||
        !interests[i].matchesData(packet))
      throw std::runtime_error("repo-original-packet-interest-mismatch");
    packets.push_back(std::move(packet));
  }
  const auto current = requireSameManifest();
  if (!acceptObject(*current))
    throw std::runtime_error("repo-original-packet-batch-admission-rejected");
  return packets;
}

RepoWriteReceipt
RepoCore::getDataPacketReceipt(const std::string& dataName,
                               const std::string& wireSha256) const
{
  const auto encodedName = ndn::Name(dataName).toUri(ndn::name::UriFormat::CANONICAL);
  std::lock_guard<std::mutex> lock(m_mutex);
  std::optional<RepoObjectManifest> found;
  try {
    RepoObjectManifest current;
    try {
      current = m_store->getManifest(encodedName);
    }
    catch (const std::out_of_range&) {
      if (encodedName == dataName) throw;
      current = m_store->getManifest(dataName);
    }
    if (current.sha256 == wireSha256 && !current.dataReceiptId.empty()) {
      found = std::move(current);
    }
  }
  catch (const std::out_of_range&) {
    // The current object was deleted; a retained historical commit may exist.
  }
  if (!found) {
    found = m_store->findRetainedDataReceipt(encodedName, wireSha256);
    if (!found && encodedName != dataName)
      found = m_store->findRetainedDataReceipt(dataName, wireSha256);
  }
  if (!found) {
    throw std::out_of_range("repo-data-receipt-not-found: " + dataName);
  }
  const auto& manifest = *found;
  if (ndn::Name(manifest.objectName) != ndn::Name(encodedName) || manifest.objectType != "ndn-data-wire" ||
      manifest.sha256 != wireSha256 ||
      manifest.dataReceiptId.empty() || manifest.dataReceiptRepoNode.empty() ||
      manifest.dataReceiptProviderIdentity.empty() ||
      manifest.dataReceiptCommittedAtMs == 0 || manifest.sha256.size() != 64 ||
      manifest.size == 0 || manifest.generation == 0 ||
      manifest.packetNames.size() != 1 ||
      ndn::Name(manifest.packetNames.front()) != ndn::Name(encodedName)) {
    throw std::out_of_range("repo-data-receipt-not-found: " + dataName);
  }
  RepoWriteReceipt receipt;
  receipt.operationId = manifest.dataReceiptId;
  receipt.repoNode = manifest.dataReceiptRepoNode;
  receipt.providerIdentity = manifest.dataReceiptProviderIdentity;
  receipt.objectName = encodedName;
  receipt.generation = manifest.generation;
  receipt.digest = manifest.sha256;
  receipt.persistedBytes = manifest.size;
  receipt.completedAtMs = manifest.dataReceiptCommittedAtMs;
  return receipt;
}

bool
RepoCore::hasDataPacket(const std::string& dataName) const
{
  ndn::Name name;
  try { name = ndn::Name(dataName); }
  catch (const std::exception&) { return false; }
  return findPacketRecord(name).has_value();
}

std::optional<std::pair<RepoObjectManifest, size_t>>
RepoCore::findPacketRecord(const ndn::Name& name,
  const std::function<bool(const RepoObjectManifest&)>& acceptObject) const
{
  const auto key = name.toUri(ndn::name::UriFormat::CANONICAL);
  // Stored manifests and their packet name lists use the ALTERNATE URI form, so an
  // exact lookup must compare against that form; a canonical string never matches.
  const auto alternateKey = name.toUri(ndn::name::UriFormat::ALTERNATE);
  if (alternateKey != key && m_store->has(alternateKey)) {
    const auto manifest = m_store->getManifest(alternateKey);
    if (manifest.objectType == "original-packet-set" &&
        (acceptObject ? acceptObject(manifest) : manifest.objectName == alternateKey)) {
      const auto found = std::find(manifest.packetNames.begin(), manifest.packetNames.end(),
                                   alternateKey);
      if (found != manifest.packetNames.end()) {
        validateReadPacketIndex(manifest);
        return std::make_pair(manifest,
          static_cast<size_t>(found - manifest.packetNames.begin()));
      }
    }
    else if (manifest.objectType == "ndn-data-wire") {
      if (!acceptObject || acceptObject(manifest)) {
        return std::make_pair(manifest, 0);
      }
    }
  }
  if (m_store->has(key)) {
    const auto manifest = m_store->getManifest(key);
    if (manifest.objectType == "ndn-data-wire") {
      if (!acceptObject || acceptObject(manifest)) {
        return std::make_pair(manifest, 0);
      }
    }
    else if (!acceptObject && manifest.objectType == "original-packet-set" &&
             manifest.objectName == key && !manifest.packetNames.empty() &&
             manifest.packetNames.front() == key) {
      // The requested packet is this object's root. Filtered searches retain
      // their existing admission/search order across containing objects.
      validateReadPacketIndex(manifest);
      return std::make_pair(manifest, 0);
    }
    else if (manifest.objectType != "original-packet-set") {
      return std::nullopt;
    }
  }
  // Fast durable path: resolve the containing packet-set manifest from the
  // packet-name base index built from all stored manifests at startup.
  try {
    // The index is keyed by the numeric canonical URI form, which is what the
    // stored manifests record; the mnemonic ALTERNATE form never matches.
    if (const auto indexed = findIndexedOriginalPacket(name, key,
          name.toUri(ndn::name::UriFormat::ALTERNATE), acceptObject))
      return std::make_pair(*indexed->first, indexed->second);
  }
  catch (const std::exception&) {
    // The durable fallback below remains authoritative for unusual stores.
  }
  // A stored packet name is <objectName>/<segment>. Resolving that containing
  // object directly avoids scanning every manifest (and every packet name in it)
  // whenever the Repo process did not insert the object itself.
  if (name.size() > 1) {
    // The object name is the first packet name, stored in ALTERNATE form.
    const auto object = ndn::Name(name).getPrefix(-1).appendSegment(0);
    const auto parentKey = object.toUri(ndn::name::UriFormat::ALTERNATE);
    if (m_store->has(parentKey)) {
      const auto manifest = m_store->getManifest(parentKey);
      if (manifest.objectType == "original-packet-set" && manifest.objectName == parentKey) {
        const auto found = std::find(manifest.packetNames.begin(), manifest.packetNames.end(),
                                     alternateKey);
        if (found != manifest.packetNames.end()) {
          validateReadPacketIndex(manifest);
          if (!acceptObject || acceptObject(manifest)) {
            return std::make_pair(manifest,
              static_cast<size_t>(found - manifest.packetNames.begin()));
          }
        }
      }
    }
  }
  const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
  if (alternate != key && m_store->has(alternate)) {
    const auto legacy = m_store->getManifest(alternate);
    if (legacy.objectType == "ndn-data-wire" && (!acceptObject || acceptObject(legacy))) {
      return std::make_pair(legacy, 0);
    }
  }
  for (auto& manifest : detail::findPacketManifests(*m_store, key, alternateKey, false)) {
    if (manifest.objectType != "original-packet-set") continue;
    auto found = std::find(manifest.packetNames.begin(), manifest.packetNames.end(),
                           alternateKey);
    if (found == manifest.packetNames.end() && alternateKey != key)
      found = std::find(manifest.packetNames.begin(), manifest.packetNames.end(), key);
    if (found != manifest.packetNames.end()) {
      validateReadPacketIndex(manifest);
      if (acceptObject && !acceptObject(manifest)) {
        continue;
      }
      const auto index = static_cast<size_t>(found - manifest.packetNames.begin());
      return std::make_pair(std::move(manifest), index);
    }
  }
  return std::nullopt;
}

void
RepoCore::validateReadPacketIndex(const RepoObjectManifest& manifest) const
{
  getPacketIndexProof(manifest);
}

std::shared_ptr<const RepoCore::PacketIndexProof>
RepoCore::getPacketIndexProof(const RepoObjectManifest& manifest,
  const std::shared_ptr<const RepoObjectManifest>& owner) const
{
  // Do not reuse a publication/writer lock: callers may already own those.
  // No storage call or visibility callback runs under this private cache lock.
  const auto matches = [&manifest](const PacketIndexProof& proof) {
    return manifest.objectType == "original-packet-set" && proof.size == manifest.size &&
      proof.names == manifest.packetNames && proof.offsets == manifest.packetOffsets;
  };
  {
    std::lock_guard<std::mutex> lock(m_packetIndexProofMutex);
    const auto found = m_packetIndexProofs.find(manifest.objectName);
    if (found != m_packetIndexProofs.end()) {
      const auto& proof = found->second;
      if ((owner && owner.get() == &manifest && proof->validatedManifest.lock() == owner) ||
          matches(*proof)) {
        if (owner.get() == &manifest) proof->validatedManifest = owner;
        return proof;
      }
    }
  }
  validatePacketIndex(manifest);
  try {
    size_t bytes = sizeof(PacketIndexProof) + sizeof(std::string) + 4 * sizeof(void*);
    const auto charge = [&bytes](size_t amount) {
      if (amount > MAX_PACKET_INDEX_BYTES - bytes)
        return false;
      bytes += amount;
      return true;
    };
    if (manifest.packetNames.size() > MAX_PACKET_INDEX_BYTES / sizeof(std::string) ||
        manifest.packetOffsets.size() > MAX_PACKET_INDEX_BYTES / sizeof(uint64_t) ||
        !charge(manifest.objectName.size() + 1) ||
        !charge(manifest.packetNames.size() * sizeof(std::string)) ||
        !charge(manifest.packetOffsets.size() * sizeof(uint64_t)))
      return {};
    for (const auto& name : manifest.packetNames) {
      if (!charge(name.size() + 1))
        return {};
    }
    auto proof = std::make_shared<PacketIndexProof>();
    proof->size = manifest.size;
    proof->names = manifest.packetNames;
    proof->offsets = manifest.packetOffsets;
    for (size_t i = 0; i < manifest.packetNames.size(); ++i) {
      const ndn::Name name(manifest.packetNames[i]);
      if (name.empty() || !name[-1].isSegment()) continue;
      try {
        const auto segment = name[-1].toSegment();
        const auto base = packetBaseName(name.toUri(ndn::name::UriFormat::CANONICAL));
        proof->segments[base].push_back({segment, i});
      }
      catch (const ndn::tlv::Error&) {
        // Non-numeric segment encodings keep the exact-name fallback.
      }
    }
    size_t hints = 0;
    for (auto& [base, positions] : proof->segments) {
      std::sort(positions.begin(), positions.end(), [](const auto& a, const auto& b) {
        return a.segment < b.segment || (a.segment == b.segment && a.packetIndex < b.packetIndex);
      });
      if (!charge(sizeof(std::string) + base.size() + 1 + 4 * sizeof(void*) +
                  sizeof(positions) + positions.capacity() * sizeof(PacketIndexProof::SegmentPosition)))
        return {};
      const auto hint = packetBaseKeyCharge(base) + packetBaseHolderCharge(manifest.objectName);
      if (hint > MAX_PACKET_INDEX_BYTES - hints) return {};
      hints += hint;
    }
    if (hints > MAX_PACKET_INDEX_BYTES - bytes) return {};
    proof->chargedBytes = bytes;
    if (owner.get() == &manifest) proof->validatedManifest = owner;
    std::scoped_lock lock(m_packetBaseIndexMutex, m_packetIndexProofMutex);
    const auto found = m_packetIndexProofs.find(manifest.objectName);
    if (found != m_packetIndexProofs.end()) {
      if (matches(*found->second)) {
        if (owner.get() == &manifest) found->second->validatedManifest = owner;
        return found->second;
      }
      m_packetIndexProofBytes -= found->second->chargedBytes;
      m_packetIndexProofs.erase(found);
    }
    if (m_packetIndexProofs.size() >= MAX_PACKET_INDEX_PROOFS ||
        bytes + hints > MAX_PACKET_INDEX_BYTES - m_packetIndexProofBytes) {
      m_packetIndexProofs.clear();
      m_packetBaseIndex.clear();
      m_packetIndexProofBytes = 0;
    }
    try {
      m_packetIndexProofs.emplace(manifest.objectName, proof);
      m_packetIndexProofBytes += bytes;
      for (const auto& [base, positions] : proof->segments)
        addPacketBaseHint(m_packetBaseIndex, m_packetIndexProofBytes, MAX_PACKET_INDEX_BYTES,
                          base, manifest.objectName);
    }
    catch (const std::bad_alloc&) {
      // Validation already succeeded; retaining its optimization is optional.
      m_packetIndexProofs.clear();
      m_packetBaseIndex.clear();
      m_packetIndexProofBytes = 0;
    }
    return proof;
  }
  catch (const std::bad_alloc&) {
    // The authoritative geometry is valid even when optional acceleration cannot fit.
    return {};
  }
}

RepoObjectManifest
RepoCore::describeDataPacket(const std::string& name) const
{
  const auto record = findPacketRecord(ndn::Name(name));
  if (!record) throw std::runtime_error("repo-original-packet-not-found");
  if (record->first.objectType == "ndn-data-wire") return record->first;
  const auto wire = getDataPacket(name);
  auto result = record->first;
  result.objectName = ndn::Name(name).toUri(ndn::name::UriFormat::CANONICAL);
  result.objectType = "ndn-data-wire";
  result.sha256 = sha256Hex(wire);
  result.size = wire.size();
  result.segmentCount = 1;
  result.packetNames = {result.objectName};
  result.packetOffsets.clear();
  result.envelopeBytes = 0;
  return result;
}

RepoObjectManifest
RepoCore::commitOriginalPacketFile(const RepoObjectManifest& manifest,
                                  const std::filesystem::path& file,
                                  const std::function<void()>& requireActive)
{
  validatePacketIndex(manifest);
  if (manifest.operationId.empty() || !m_store->supportsRange() ||
      std::filesystem::file_size(file) != manifest.size)
    throw std::invalid_argument("repo-original-packet-transaction-invalid");
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("repo-original-packet-input-unreadable");
  const OriginalPacketRangeReader readRange = [&input](uint64_t offset, std::size_t length) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> bytes(length);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (!input) {
      throw std::runtime_error("repo-original-packet-input-truncated");
    }
    return bytes;
  };
  return commitOriginalPacketReaderImpl(manifest, readRange, requireActive, file);
}

RepoObjectManifest
RepoCore::commitOriginalPacketReader(const RepoObjectManifest& manifest,
                                     const OriginalPacketRangeReader& readRange,
                                     const std::function<void()>& requireActive)
{
  return commitOriginalPacketReaderImpl(manifest, readRange, requireActive, {});
}

RepoObjectManifest
RepoCore::commitOriginalPacketReaderImpl(const RepoObjectManifest& manifest,
                                         const OriginalPacketRangeReader& readRange,
                                         const std::function<void()>& requireActive,
                                         const std::filesystem::path& sourceFile)
{
  auto stageStarted = std::chrono::steady_clock::now();
  const auto finishStage = [&](const char* stage) noexcept {
    try {
      if (!ndn_service_framework::controlTimingEnvEnabled()) return;
      const auto finished = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finished - stageStarted).count();
      stageStarted = finished;
      ndn_service_framework::logControlTiming("repo-core", "original-commit",
        ndn::Name(manifest.objectName), {
          {"operation_id", manifest.operationId}, {"stage", stage},
          {"elapsed_ns", std::to_string(elapsed)},
          {"packets", std::to_string(manifest.packetNames.size())}});
    }
    catch (...) {
      // Observation cannot affect immutable admission or transaction cleanup.
    }
  };
  validatePacketIndex(manifest);
  if (manifest.operationId.empty() || !m_store->supportsRange() || !readRange) {
    throw std::invalid_argument("repo-original-packet-transaction-invalid");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (requireActive) requireActive();
  finishStage("validate-and-lock");
  std::optional<RepoObjectManifest> existing;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    auto normalized = manifest;
    normalized.operationId = previous.operationId;
    normalized.generation = previous.generation;
    if (manifest.publicationIdentity.empty() && previous.objectType == "original-packet-set")
      normalized.publicationIdentity = previous.publicationIdentity;
    if (!normalized.hasSameMetadata(previous))
      throw std::runtime_error("repo-data-wire-conflict");
    existing = previous;
  }
  finishStage("existing-object");
  // Admission and final publication share the lock with single packet writes.
  // Misses must not enumerate and parse every stored manifest for every packet.
  // Keep the index local to this transaction; out-of-process metadata changes
  // are checked again before accepting reuse or publishing the new ranges.
  const std::set<std::string> incomingNames(manifest.packetNames.begin(), manifest.packetNames.end());
  std::set<std::string> indexedPacketNames;
  std::map<std::string, RepoObjectManifest> catalogMetadata;
  size_t catalogEntries = 0;
  for (const auto& stored : m_store->listManifests()) {
    ++catalogEntries;
    catalogMetadata.emplace(stored.objectName, stored);
    if (stored.objectType == "ndn-data-wire") {
      // Legacy rows may use ALTERNATE URI spelling for the same typed Name.
      const auto canonical = ndn::Name(stored.objectName).toUri(ndn::name::UriFormat::CANONICAL);
      if (incomingNames.count(canonical)) indexedPacketNames.insert(canonical);
    }
    if (stored.objectType != "original-packet-set") continue;
    for (const auto& name : stored.packetNames) {
      if (incomingNames.count(name)) indexedPacketNames.insert(name);
    }
  }
  finishStage("catalog");
  const auto requireCurrentCatalog = [&] {
    if (requireActive) requireActive();
    const auto current = m_store->listManifests();
    const auto rejectCatalog = [&] (const char* kind, const std::string* affected = nullptr,
                                   const std::string* before = nullptr,
                                   const std::string* after = nullptr) {
      try {
        const auto safePrefix = [] (const std::string& value, size_t limit) {
          auto result = value.substr(0, limit);
          for (auto& character : result) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte <= 32 || byte >= 127) character = '?';
          }
          return result;
        };
        std::set<std::string> observedNames;
        for (const auto& stored : current) {
          const bool unique = observedNames.insert(stored.objectName).second;
          if (!affected && (!unique || !catalogMetadata.count(stored.objectName)))
            affected = &stored.objectName;
        }
        if (!affected) {
          for (const auto& [name, metadata] : catalogMetadata) {
            if (!observedNames.count(name)) {
              affected = &name;
              break;
            }
          }
        }
        size_t difference = 0;
        std::string field = "unknown";
        if (before && after) {
          const auto mismatch = std::mismatch(before->begin(), before->end(),
                                              after->begin(), after->end());
          difference = static_cast<size_t>(mismatch.first - before->begin());
          const auto end = before->rfind("\":", difference);
          if (end != std::string::npos && end != 0) {
            const auto start = before->rfind('"', end - 1);
            if (start != std::string::npos && end - start <= 65) {
              const auto candidate = before->substr(start + 1, end - start - 1);
              const bool safe = !candidate.empty() && std::all_of(
                candidate.begin(), candidate.end(), [] (char character) {
                  return (character >= 'A' && character <= 'Z') ||
                         (character >= 'a' && character <= 'z') ||
                         (character >= '0' && character <= '9') || character == '_';
                });
              if (safe) field = candidate;
            }
          }
        }
        NDN_LOG_WARN("NDNSF_REPO_ORIGINAL_CATALOG_REJECTED kind=" << kind
                     << " snapshotEntries=" << catalogEntries
                     << " expectedUnique=" << catalogMetadata.size()
                     << " observedEntries=" << current.size()
                     << " observedUnique=" << observedNames.size()
                     << " incomingPrefix=" << safePrefix(manifest.objectName, 160)
                     << " affectedPrefix=" << (affected ? safePrefix(*affected, 160) : "none")
                     << " inputFile=" << safePrefix(
                          sourceFile.empty() ? "range-reader" : sourceFile.string(), 240)
                     << " field=" << field << " firstDifference=" << difference
                     << " expectedBytes=" << (before ? before->size() : 0)
                     << " observedBytes=" << (after ? after->size() : 0));
      }
      catch (...) {
        // Diagnostics cannot bypass the original catalog admission failure.
      }
      throw std::runtime_error("repo-original-packet-catalog-changed");
    };
    if (current.size() != catalogMetadata.size())
      rejectCatalog("ENTRY_COUNT");
    for (const auto& stored : current) {
      const auto previous = catalogMetadata.find(stored.objectName);
      if (previous == catalogMetadata.end())
        rejectCatalog("OBJECT_NAME", &stored.objectName);
      if (!previous->second.hasSameMetadata(stored)) {
        const auto before = previous->second.toJson();
        const auto after = stored.toJson();
        rejectCatalog("METADATA", &stored.objectName, &before, &after);
      }
    }
  };
  for (size_t i = 0; i < manifest.packetNames.size(); ++i) {
    if (requireActive) requireActive();
    const auto length = manifest.packetOffsets[i + 1] - manifest.packetOffsets[i];
    const auto wire = readRange(manifest.packetOffsets[i], static_cast<std::size_t>(length));
    if (wire.size() != length) {
      throw std::runtime_error("repo-original-packet-input-truncated");
    }
    const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
    if (data.getName() != ndn::Name(manifest.packetNames[i]) || data.wireEncode().size() != wire.size())
      throw std::invalid_argument("repo-data-name-mismatch");
    const auto stored = catalogMetadata.find(manifest.packetNames[i]);
    // A catalog miss is already known for this transaction. For a hit, retain
    // the physical check: metadata may outlive a missing committed payload.
    const bool hasStoredPacket = stored != catalogMetadata.end() &&
      m_store->has(manifest.packetNames[i]);
    if (hasStoredPacket && stored->second.objectType != "ndn-data-wire" &&
        (!existing || manifest.packetNames[i] != existing->objectName))
      throw std::runtime_error("repo-original-packet-format-conflict");
    if ((hasStoredPacket || indexedPacketNames.count(manifest.packetNames[i])) &&
        getDataPacket(manifest.packetNames[i]) != wire)
      throw std::runtime_error("repo-data-wire-conflict");
  }
  finishStage("packet-validation");
  if (existing) {
    requireCurrentCatalog();
    finishStage("catalog-recheck-reuse");
    return *existing;
  }
  // objectName is the first incoming packet name. The transaction snapshot
  // already proves whether it is indexed by an existing original packet set.
  // Reuse that admission result rather than rescanning for every range window.
  if (indexedPacketNames.count(manifest.objectName))
    throw std::runtime_error("repo-original-packet-is-immutable");
  try {
    constexpr uint64_t window = 1U << 20;
    for (uint64_t offset = 0; offset < manifest.size;) {
      if (requireActive) requireActive();
      const auto length = static_cast<std::size_t>(std::min(window, manifest.size - offset));
      const auto bytes = readRange(offset, length);
      if (bytes.size() != length) {
        throw std::runtime_error("repo-original-packet-input-truncated");
      }
      putAdmittedRangeLocked(manifest, {offset, bytes.size()}, bytes);
      offset += bytes.size();
    }
    finishStage("range-write");
    requireCurrentCatalog();
    finishStage("catalog-recheck");
    auto committed = commitRangesLocked(manifest);
    finishStage("publish");
    return committed;
  }
  catch (...) {
    try {
      const auto reservation = m_rangeReservations.find(manifest.objectName);
      if (reservation != m_rangeReservations.end()) {
        const auto expected = parseManifestJson(reservation->second.canonicalIdentity);
        auto submitted = manifest;
        if (submitted.generation == 0) submitted.generation = expected.generation;
        if (submitted.hasSameMetadata(expected)) {
          (void)abortFailedOwnedRangeIfUncommittedLocked(expected);
        }
      }
    }
    catch (...) {
      // Keep the original transaction failure and uncertain capacity reservation.
    }
    m_catalogReconciliationRequired = true;
    throw;
  }
}

std::vector<uint8_t>
RepoCore::handleStore(const std::vector<uint8_t>& request)
{
  RepoObjectManifest manifest;
  std::vector<uint8_t> payload;
  decodeStoreRequest(request, manifest, payload);
  rejectExternallySuppliedDataReceipt(manifest);

  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");

  std::lock_guard<std::mutex> lock(m_mutex);
  if (hasDataPacket(manifest.objectName))
    throw std::runtime_error("repo-original-packet-is-immutable");
  if (m_store->has(manifest.objectName)) {
    rejectDataPacketReplacement(m_store->getManifest(manifest.objectName));
  }
  if (m_rangeReservations.find(manifest.objectName) != m_rangeReservations.end()) {
    m_store->abortRanges(manifest.objectName);
    clearRangeReservation(manifest.objectName);
  }
  if (manifest.generation == 0) {
    if (m_store->has(manifest.objectName)) {
      const auto previous = m_store->getManifest(manifest.objectName);
      if (previous.generation == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("repo-generation-exhausted");
      }
      manifest.generation = previous.generation == 0 ? 2 : previous.generation + 1;
    }
    else {
      manifest.generation = 1;
    }
  }
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    oldSize = previous.segmentCount > 1 || !previous.packetNames.empty()
                ? 0 : previous.size;
  }
  // The replacement may reclaim its own committed bytes, but outstanding
  // reservations for other objects remain committed quota.  Keep vector,
  // exact-packet, and range admission on one logical byte budget.
  const auto availableBase = m_capability.freeBytes > m_reservedRangeBytes
    ? m_capability.freeBytes - m_reservedRangeBytes : 0;
  const auto availableBytes = availableBase + oldSize;
  if (payload.size() > availableBytes) {
    throw std::runtime_error("repo node has insufficient free space for object: " +
                             manifest.objectName);
  }
  try {
    m_store->put(manifest, std::move(payload));
  }
  catch (const std::exception& e) {
    RepoObjectManifest durable;
    if (!isAmbiguousCommitError(e) ||
        !recoverAmbiguousCommit(manifest.objectName, durable)) {
      // Keep admission accounting aligned with the backend even when a
      // vector write fails after creating an unreferenced physical residue.
      m_catalogReconciliationRequired = true;
      refreshCapabilityUsageAfterCommit(oldSize, oldSize);
      throw;
    }
    refreshCapabilityUsageAfterCommit(oldSize, durable.size);
    return toBytes(durable.toJson());
  }
  auto durable = manifest;
  try {
    durable = m_store->getManifest(manifest.objectName);
  }
  catch (...) {
    m_catalogReconciliationRequired = true;
  }
  rememberCatalogChange(durable, "AVAILABLE");
  refreshCapabilityUsageAfterCommit(oldSize, durable.size);
  return toBytes(durable.toJson());
}

std::vector<uint8_t>
RepoCore::handleStoreRange(const std::vector<uint8_t>& request)
{
  RepoObjectManifest manifest;
  RepoByteRange range;
  std::vector<uint8_t> bytes;
  decodeRangeWriteRequest(request, manifest, range, bytes);
  putRange(manifest, range, bytes);
  return toBytes("accepted");
}

std::vector<uint8_t>
RepoCore::handleCommitRanges(const std::vector<uint8_t>& request)
{
  return toBytes(commitRanges(parseManifestJson(toString(request))).toJson());
}

std::vector<uint8_t>
RepoCore::handleStoreManifest(const std::vector<uint8_t>& request)
{
  auto manifest = parseManifestJson(toString(request));
  if (manifest.objectType == "original-packet-set" || manifest.objectType == "ndn-data-wire")
    throw std::invalid_argument("repo-original-packets-require-atomic-packet-api");
  std::lock_guard<std::mutex> lock(m_mutex);
  if (hasDataPacket(manifest.objectName))
    throw std::runtime_error("repo-original-packet-is-immutable");
  rejectExternallySuppliedDataReceipt(manifest);
  if (m_store->has(manifest.objectName)) {
    rejectDataPacketReplacement(m_store->getManifest(manifest.objectName));
  }
  if (m_rangeReservations.find(manifest.objectName) != m_rangeReservations.end()) {
    m_store->abortRanges(manifest.objectName);
    clearRangeReservation(manifest.objectName);
  }
  if (manifest.generation == 0) {
    if (m_store->has(manifest.objectName)) {
      const auto previous = m_store->getManifest(manifest.objectName);
      if (previous.generation == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("repo-generation-exhausted");
      }
      manifest.generation = previous.generation == 0 ? 2 : previous.generation + 1;
    }
    else {
      manifest.generation = 1;
    }
  }
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
    oldSize = previous.segmentCount > 1 || !previous.packetNames.empty()
                ? 0 : previous.size;
  }
  try {
    m_store->putManifest(manifest);
  }
  catch (const std::exception& e) {
    RepoObjectManifest durable;
    if (!isAmbiguousCommitError(e) ||
        !recoverAmbiguousCommit(manifest.objectName, durable)) {
      throw;
    }
    refreshCapabilityUsageAfterCommit(oldSize, durable.size);
    return toBytes(durable.toJson());
  }
  auto durable = manifest;
  try {
    durable = m_store->getManifest(manifest.objectName);
  }
  catch (...) {
    m_catalogReconciliationRequired = true;
  }
  rememberCatalogChange(durable, "AVAILABLE");
  refreshCapabilityUsageAfterCommit(oldSize, durable.size);
  return toBytes(durable.toJson());
}

std::vector<uint8_t>
RepoCore::handleFetch(const std::vector<uint8_t>& request) const
{
  const auto objectName = resolveObjectKey(toString(request));
  // Legacy vector backends cannot store atomic packet sets. Keep their
  // ordinary object read to one fetch rather than copying it for inspection.
  if (!m_store->supportsManifestLookup()) return m_store->get(objectName).payload;
  if (hasDataPacket(objectName)) return getDataPacket(objectName);
  return m_store->get(objectName).payload;
}

std::vector<uint8_t>
RepoCore::handleFetchRange(const std::vector<uint8_t>& request) const
{
  std::string objectName;
  RepoByteRange range;
  decodeRangeReadRequest(request, objectName, range);
  return getRange(objectName, range);
}

std::vector<uint8_t>
RepoCore::handleManifest(const std::vector<uint8_t>& request) const
{
  const auto objectName = resolveObjectKey(toString(request));
  if (!m_store->supportsManifestLookup()) return toBytes(m_store->getManifest(objectName).toJson());
  if (m_store->has(objectName) && m_store->getManifest(objectName).objectType == "original-packet-set")
    return toBytes(m_store->getManifest(objectName).toJson());
  if (hasDataPacket(objectName)) return toBytes(describeDataPacket(objectName).toJson());
  return toBytes(m_store->getManifest(objectName).toJson());
}

std::vector<uint8_t>
RepoCore::handleInventory() const
{
  return toBytes(encodeInventory(m_store->listManifests()));
}

std::vector<uint8_t>
RepoCore::handleCapability() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(m_capability.toJson());
}

RepoCacheStatus
RepoCore::cacheStatus() const
{
  return m_store->cacheStatus();
}

std::vector<uint8_t>
RepoCore::handleCacheStatus() const
{
  return toBytes(cacheStatus().toJson());
}

RepoCatalogStatus
RepoCore::catalogStatus() const
{
  const auto objectCount = m_store->listManifests().size();
  std::lock_guard<std::mutex> lock(m_mutex);
  RepoCatalogStatus status;
  status.repoNode = m_capability.repoNode;
  status.repoMode = m_capability.repoMode;
  status.catalogEpoch = m_catalogEpoch;
  status.objectCount = objectCount;
  status.acceptsBackupReplica = m_capability.acceptsBackupReplica;
  status.reconciliationRequired = m_catalogReconciliationRequired;
  return status;
}

RepoCatalogDelta
RepoCore::catalogSnapshot() const
{
  const auto manifests = m_store->listManifests();
  std::lock_guard<std::mutex> lock(m_mutex);
  RepoCatalogDelta snapshot;
  snapshot.repoNode = m_capability.repoNode;
  snapshot.repoMode = m_capability.repoMode;
  snapshot.sinceEpoch = 0;
  snapshot.catalogEpoch = m_catalogEpoch;
  snapshot.entries.reserve(manifests.size());
  for (const auto& manifest : manifests) {
    snapshot.entries.push_back(makeCatalogEntry(manifest, "AVAILABLE", m_catalogEpoch));
  }
  return snapshot;
}

RepoCatalogDelta
RepoCore::catalogDelta(uint64_t sinceEpoch) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  RepoCatalogDelta delta;
  delta.repoNode = m_capability.repoNode;
  delta.repoMode = m_capability.repoMode;
  delta.sinceEpoch = sinceEpoch;
  delta.catalogEpoch = m_catalogEpoch;
  for (const auto& entry : m_catalogChanges) {
    if (entry.catalogEpoch > sinceEpoch) {
      delta.entries.push_back(entry);
    }
  }
  return delta;
}

RepoCatalogEntry
RepoCore::catalogLookup(const std::string& objectName) const
{
  const auto key = resolveObjectKey(objectName);
  const auto manifest = m_store->has(key) ? m_store->getManifest(key)
    : hasDataPacket(objectName) ? describeDataPacket(objectName)
    : m_store->getManifest(resolveObjectKey(objectName));
  std::lock_guard<std::mutex> lock(m_mutex);
  return makeCatalogEntry(manifest, "AVAILABLE", m_catalogEpoch);
}

std::vector<uint8_t>
RepoCore::handleCatalogStatus() const
{
  return toBytes(catalogStatus().toJson());
}

std::vector<uint8_t>
RepoCore::handleCatalogSnapshot() const
{
  return toBytes(catalogSnapshot().toJson());
}

std::vector<uint8_t>
RepoCore::handleCatalogDelta(const std::vector<uint8_t>& request) const
{
  const auto text = toString(request);
  const auto sinceEpoch = text.empty() ? 0 : static_cast<uint64_t>(std::stoull(text));
  return toBytes(catalogDelta(sinceEpoch).toJson());
}

std::vector<uint8_t>
RepoCore::handleCatalogLookup(const std::vector<uint8_t>& request) const
{
  return toBytes(catalogLookup(toString(request)).toJson());
}

std::vector<uint8_t>
RepoCore::handleDelete(const std::vector<uint8_t>& request)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto objectName = resolveObjectKey(toString(request));
  if (const auto packet = hasDataPacket(objectName) ? findPacketRecord(ndn::Name(objectName)) : std::nullopt;
      packet && packet->first.objectType == "original-packet-set" &&
      ndn::Name(objectName) != ndn::Name(packet->first.objectName))
    throw std::invalid_argument("repo-delete-original-packet-set-by-root");
  return deleteLocked(objectName);
}

std::vector<uint8_t>
RepoCore::deleteLocked(const std::string& objectName)
{
  if (m_rangeReservations.find(objectName) != m_rangeReservations.end()) {
    m_store->abortRanges(objectName);
    clearRangeReservation(objectName);
  }
  RepoObjectManifest manifest;
  uint64_t oldSize = 0;
  const bool hadObject = m_store->has(objectName);
  if (hadObject) {
    manifest = m_store->getManifest(objectName);
    oldSize = manifest.segmentCount > 1 || !manifest.packetNames.empty()
                ? 0 : manifest.size;
  }
  bool removed = false;
  try {
    removed = m_store->erase(objectName);
  }
  catch (const std::exception& e) {
    bool noLongerVisible = false;
    if (isAmbiguousCommitError(e)) {
      try {
        noLongerVisible = !m_store->has(objectName);
      }
      catch (...) {
      }
    }
    if (!noLongerVisible) {
      throw;
    }
    m_catalogReconciliationRequired = true;
    rememberCatalogChange(manifest, "DELETED");
    refreshCapabilityUsageAfterCommit(oldSize, 0);
    return toBytes("deleted");
  }
  if (removed) {
    rememberCatalogChange(manifest, "DELETED");
  }
  if (removed) refreshCapabilityUsageAfterCommit(oldSize, 0);
  return toBytes(removed ? "deleted" : "not-found");
}

void
RepoCore::refreshCapabilityUsage()
{
  m_capability.usedBytes = m_store->usedBytes();
  m_capability.freeBytes = m_capacityBytes > m_capability.usedBytes
    ? m_capacityBytes - m_capability.usedBytes
    : 0;
}

void
RepoCore::updateCapabilityUsage(uint64_t oldSize, uint64_t newSize)
{
  if (newSize >= oldSize) {
    m_capability.usedBytes += newSize - oldSize;
  }
  else {
    m_capability.usedBytes -= std::min(m_capability.usedBytes, oldSize - newSize);
  }
  m_capability.freeBytes = m_capacityBytes > m_capability.usedBytes
    ? m_capacityBytes - m_capability.usedBytes
    : 0;
}

RepoCatalogEntry
RepoCore::makeCatalogEntry(const RepoObjectManifest& manifest,
                           std::string state,
                           uint64_t epoch) const
{
  RepoCatalogEntry entry;
  entry.manifest = manifest;
  entry.sourceRepo = m_capability.repoNode;
  entry.repoMode = m_capability.repoMode;
  entry.state = std::move(state);
  entry.catalogEpoch = epoch;
  if (entry.manifest.replicaNodes.empty() && !m_capability.repoNode.empty()) {
    entry.manifest.replicaNodes.push_back(m_capability.repoNode);
  }
  return entry;
}

void
RepoCore::rememberCatalogChange(const RepoObjectManifest& manifest,
                                const std::string& state)
{
  try {
    if (state == "AVAILABLE" && manifest.objectType == "original-packet-set") {
      validateReadPacketIndex(manifest);
    }
    else if (state == "DELETED") {
      std::scoped_lock lock(m_packetBaseIndexMutex, m_packetIndexProofMutex);
      const auto proof = m_packetIndexProofs.find(manifest.objectName);
      if (proof != m_packetIndexProofs.end()) {
        m_packetIndexProofBytes -= proof->second->chargedBytes;
        m_packetIndexProofs.erase(proof);
      }
      for (auto base = m_packetBaseIndex.begin(); base != m_packetBaseIndex.end();) {
        auto& holders = base->second;
        const auto holder = std::find(holders.begin(), holders.end(), manifest.objectName);
        if (holder != holders.end()) {
          m_packetIndexProofBytes -= packetBaseHolderCharge(*holder);
          holders.erase(holder);
        }
        if (holders.empty()) {
          m_packetIndexProofBytes -= packetBaseKeyCharge(base->first);
          base = m_packetBaseIndex.erase(base);
        }
        else ++base;
      }
    }
  }
  catch (...) {
    // Locator retention is optional; the durable catalog remains authoritative.
  }
  try {
    const auto nextEpoch = m_catalogEpoch + 1;
    m_catalogChanges.push_back(makeCatalogEntry(manifest, state, nextEpoch));
    m_catalogEpoch = nextEpoch;
  }
  catch (...) {
    // The durable store remains authoritative and catalogSnapshot() can
    // reconstruct the current object set.  Keep the discrepancy observable
    // instead of turning a successful durable commit into an API failure.
    m_catalogReconciliationRequired = true;
  }
}

bool
RepoCore::recoverAmbiguousCommit(const std::string& objectName,
                                  RepoObjectManifest& durable)
{
  try {
    durable = m_store->getManifest(objectName);
  }
  catch (...) {
    return false;
  }
  m_catalogReconciliationRequired = true;
  rememberCatalogChange(durable, "AVAILABLE");
  return true;
}

void
RepoCore::refreshCapabilityUsageAfterCommit(uint64_t oldSize,
                                             uint64_t newSize) noexcept
{
  try {
    refreshCapabilityUsage();
  }
  catch (...) {
    m_catalogReconciliationRequired = true;
    updateCapabilityUsage(oldSize, newSize);
  }
}

void
RepoCore::clearRangeReservation(const std::string& objectName)
{
  const auto found = m_rangeReservations.find(objectName);
  if (found == m_rangeReservations.end()) {
    return;
  }
  m_reservedRangeBytes -= std::min(m_reservedRangeBytes,
                                   found->second.additionalBytes);
  m_rangeReservations.erase(found);
}

} // namespace ndnsf_distributed_repo
