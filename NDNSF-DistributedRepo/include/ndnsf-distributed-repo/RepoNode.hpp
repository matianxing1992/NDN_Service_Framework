#ifndef NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <mutex>

namespace ndnsf_distributed_repo {

class RepoNode
{
public:
  RepoNode(ndn::Name servicePrefix, StorageCapability capability);

  const ndn::Name& servicePrefix() const;

  void registerServices(ndn_service_framework::ServiceProvider& provider);

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

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

private:
  ndn_service_framework::ResponseMessage makeResponse(const std::vector<uint8_t>& payload) const;

  ndn_service_framework::ResponseMessage makeError(const std::string& error) const;

private:
  ndn::Name m_servicePrefix;
  StorageCapability m_capability;
  mutable std::mutex m_mutex;
  InMemoryRepoStore m_store;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
