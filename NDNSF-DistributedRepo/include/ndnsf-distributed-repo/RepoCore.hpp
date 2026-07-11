#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace ndnsf_distributed_repo {

class RepoCore
{
public:
  RepoCore(StorageCapability capability, std::shared_ptr<RepoStoreBackend> store);

  RepoObjectManifest put(const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         const std::string& objectType = "object",
                         uint32_t replicationFactor = 1,
                         const std::string& policyEpoch = "",
                         std::vector<std::string> replicaNodes = {});

  std::vector<uint8_t> get(const std::string& objectName) const;

  RepoObjectManifest getManifest(const std::string& objectName) const;

  std::vector<RepoObjectManifest> list() const;

  bool remove(const std::string& objectName);

  RepoObjectManifest putManifest(const RepoObjectManifest& manifest);

  RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire);

  std::vector<uint8_t> getDataPacket(const std::string& dataName) const;

  bool hasDataPacket(const std::string& dataName) const;

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  RepoCacheStatus cacheStatus() const;

  std::vector<uint8_t> handleCacheStatus() const;

  RepoCatalogStatus catalogStatus() const;

  RepoCatalogDelta catalogSnapshot() const;

  RepoCatalogDelta catalogDelta(uint64_t sinceEpoch) const;

  RepoCatalogEntry catalogLookup(const std::string& objectName) const;

  std::vector<uint8_t> handleCatalogStatus() const;

  std::vector<uint8_t> handleCatalogSnapshot() const;

  std::vector<uint8_t> handleCatalogDelta(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleCatalogLookup(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

private:
  void refreshCapabilityUsage();

  RepoCatalogEntry makeCatalogEntry(const RepoObjectManifest& manifest,
                                    std::string state,
                                    uint64_t epoch) const;

  void rememberCatalogChange(const RepoObjectManifest& manifest,
                             const std::string& state);

private:
  StorageCapability m_capability;
  uint64_t m_capacityBytes = 0;
  mutable std::mutex m_mutex;
  std::shared_ptr<RepoStoreBackend> m_store;
  uint64_t m_catalogEpoch = 0;
  std::vector<RepoCatalogEntry> m_catalogChanges;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
