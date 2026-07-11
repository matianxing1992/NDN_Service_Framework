#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>

#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

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
  if (wire.size() > m_capability.freeBytes) {
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
  m_store->put(manifest, wire);
  refreshCapabilityUsage();
  return manifest;
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
  uint64_t oldSize = 0;
  if (m_store->has(manifest.objectName)) {
    oldSize = m_store->get(manifest.objectName).payload.size();
  }
  const auto availableBytes = m_capability.freeBytes + oldSize;
  if (payload.size() > availableBytes) {
    throw std::runtime_error("repo node has insufficient free space for object: " +
                             manifest.objectName);
  }
  m_store->put(manifest, std::move(payload));
  rememberCatalogChange(manifest, "AVAILABLE");
  refreshCapabilityUsage();
  return toBytes(manifest.toJson());
}

std::vector<uint8_t>
RepoCore::handleStoreManifest(const std::vector<uint8_t>& request)
{
  auto manifest = parseManifestJson(toString(request));
  std::lock_guard<std::mutex> lock(m_mutex);
  m_store->putManifest(manifest);
  rememberCatalogChange(manifest, "AVAILABLE");
  refreshCapabilityUsage();
  return toBytes(manifest.toJson());
}

std::vector<uint8_t>
RepoCore::handleFetch(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  return m_store->get(objectName).payload;
}

std::vector<uint8_t>
RepoCore::handleManifest(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  return toBytes(m_store->get(objectName).manifest.toJson());
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
  const auto manifest = m_store->get(objectName).manifest;
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
  RepoObjectManifest manifest;
  const bool hadObject = m_store->has(objectName);
  if (hadObject) {
    manifest = m_store->get(objectName).manifest;
  }
  const bool removed = m_store->erase(objectName);
  if (removed) {
    rememberCatalogChange(manifest, "DELETED");
  }
  refreshCapabilityUsage();
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
  ++m_catalogEpoch;
  m_catalogChanges.push_back(makeCatalogEntry(manifest, state, m_catalogEpoch));
}

} // namespace ndnsf_distributed_repo
