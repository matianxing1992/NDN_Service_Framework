#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

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
  auto normalized = expected;
  // A failed commit may have become durable before its return value reached
  // the caller. The unique operation identity still fences its rollback.
  if (normalized.generation == 0)
    normalized.generation = current.generation;
  return normalized.toJson() == current.toJson();
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
  return m_store->has(objectName);
}

RepoObjectManifest
RepoCore::getManifest(const std::string& objectName) const
{
  return parseManifestJson(toString(handleManifest(toBytes(objectName))));
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

RepoObjectManifest
RepoCore::putManifest(const RepoObjectManifest& manifest)
{
  return parseManifestJson(toString(handleStoreManifest(encodeManifestRequest(manifest))));
}

void
RepoCore::putRange(const RepoObjectManifest& manifest, RepoByteRange range,
                   const std::vector<uint8_t>& bytes)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  putRangeLocked(manifest, range, bytes);
}

void
RepoCore::putRangeIfAbsent(const RepoObjectManifest& manifest, RepoByteRange range,
                          const std::vector<uint8_t>& bytes)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (manifest.operationId.empty() || m_store->has(manifest.objectName))
    throw std::runtime_error("repo-publication-name-conflict");
  putRangeLocked(manifest, range, bytes);
}

void
RepoCore::putRangeLocked(const RepoObjectManifest& manifest, RepoByteRange range,
                         const std::vector<uint8_t>& bytes)
{
  if (range.lengthBytes != bytes.size() ||
      range.offsetBytes > manifest.size ||
      range.lengthBytes > manifest.size - range.offsetBytes) {
    throw std::out_of_range("repo-core-range-write-out-of-bounds");
  }
  uint64_t generation = manifest.generation;
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    const auto previous = m_store->getManifest(manifest.objectName);
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
  auto rangeManifest = manifest;
  rangeManifest.generation = generation;
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
                       generation, additionalBytes}).first;
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
  }
  catch (...) {
    try {
      m_store->abortRanges(manifest.objectName);
    }
    catch (...) {
    }
    clearRangeReservation(manifest.objectName);
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
  std::lock_guard<std::mutex> lock(m_mutex);
  return commitRangesLocked(manifest);
}

RepoObjectManifest
RepoCore::commitRangesIfOwned(const RepoObjectManifest& manifest)
{
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
    oldSize = previous.segmentCount > 1 || !previous.packetNames.empty()
                ? 0 : previous.size;
  }
  try {
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
      if (isAmbiguousCommitError(e) &&
          recoverAmbiguousCommit(manifest.objectName, durable)) {
        refreshCapabilityUsageAfterCommit(oldSize, durable.size);
        clearRangeReservation(manifest.objectName);
        return durable;
      }
    }
    // Commit failures must close both sides of the Core/backend reservation.
    // The backend also removes an unreferenced finalized payload when its
    // manifest publication failed; this call is idempotent for a durable
    // manifest that was already published.
    try {
      m_store->abortRanges(manifest.objectName);
    }
    catch (...) {
    }
    clearRangeReservation(manifest.objectName);
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

RepoObjectManifest
RepoCore::putDataPacket(const std::string& dataName,
                        const std::vector<uint8_t>& wire)
{
  if (dataName.empty()) {
    throw std::invalid_argument("NDN Data name must not be empty");
  }
  std::string encodedName;
  try {
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    block.parse();
    encodedName = ndn::Data(block).getName().toUri();
  }
  catch (const std::exception& e) {
    throw std::invalid_argument(std::string("repo-invalid-data-wire: ") + e.what());
  }
  if (encodedName != dataName) {
    throw std::invalid_argument(
      "repo-data-name-mismatch: declared=" + dataName + " encoded=" + encodedName);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_store->has(dataName)) {
    const auto stored = m_store->get(dataName);
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
  manifest.objectName = dataName;
  manifest.objectType = "ndn-data-wire";
  manifest.sha256 = sha256Hex(wire);
  manifest.size = wire.size();
  manifest.segmentCount = 1;
  manifest.packetNames = {dataName};
  manifest.generation = 1;
  try {
    m_store->put(manifest, wire);
  }
  catch (const std::exception& e) {
    RepoObjectManifest durable;
    if (!isAmbiguousCommitError(e) ||
        !recoverAmbiguousCommit(dataName, durable)) {
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
    durable = m_store->getManifest(dataName);
  }
  catch (...) {
    m_catalogReconciliationRequired = true;
  }
  rememberCatalogChange(durable, "AVAILABLE");
  refreshCapabilityUsageAfterCommit(0, durable.size);
  return durable;
}

std::vector<uint8_t>
RepoCore::getDataPacket(const std::string& dataName) const
{
  const auto stored = m_store->get(dataName);
  if (stored.manifest.objectType != "ndn-data-wire") {
    throw std::runtime_error("repo object is not an exact NDN Data packet: " + dataName);
  }
  return stored.payload;
}

bool
RepoCore::hasDataPacket(const std::string& dataName) const
{
  if (!m_store->has(dataName)) {
    return false;
  }
  return m_store->get(dataName).manifest.objectType == "ndn-data-wire";
}

std::vector<uint8_t>
RepoCore::handleStore(const std::vector<uint8_t>& request)
{
  RepoObjectManifest manifest;
  std::vector<uint8_t> payload;
  decodeStoreRequest(request, manifest, payload);

  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  const auto objectName = toString(request);
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
  const auto objectName = toString(request);
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
  const auto manifest = m_store->getManifest(objectName);
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
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
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
