#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <memory>
#include <mutex>

namespace ndnsf_distributed_repo {

class RepoCore
{
public:
  explicit RepoCore(StorageCapability capability);
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

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

private:
  void refreshCapabilityUsage();

private:
  StorageCapability m_capability;
  uint64_t m_capacityBytes = 0;
  mutable std::mutex m_mutex;
  std::shared_ptr<RepoStoreBackend> m_store;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
