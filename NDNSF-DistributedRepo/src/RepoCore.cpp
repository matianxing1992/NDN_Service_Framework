#include "ndnsf-distributed-repo/RepoCore.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

RepoCore::RepoCore(StorageCapability capability)
  : RepoCore(std::move(capability), makeMemoryRepoStore())
{
}

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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  refreshCapabilityUsage();
  return toBytes(manifest.toJson());
}

std::vector<uint8_t>
RepoCore::handleStoreManifest(const std::vector<uint8_t>& request)
{
  auto manifest = parseManifestJson(toString(request));
  std::lock_guard<std::mutex> lock(m_mutex);
  m_store->putManifest(manifest);
  refreshCapabilityUsage();
  return toBytes(manifest.toJson());
}

std::vector<uint8_t>
RepoCore::handleFetch(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_store->get(objectName).payload;
}

std::vector<uint8_t>
RepoCore::handleManifest(const std::vector<uint8_t>& request) const
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(m_store->get(objectName).manifest.toJson());
}

std::vector<uint8_t>
RepoCore::handleInventory() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(encodeInventory(m_store->listManifests()));
}

std::vector<uint8_t>
RepoCore::handleCapability() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return toBytes(m_capability.toJson());
}

std::vector<uint8_t>
RepoCore::handleDelete(const std::vector<uint8_t>& request)
{
  const auto objectName = toString(request);
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool removed = m_store->erase(objectName);
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

} // namespace ndnsf_distributed_repo
