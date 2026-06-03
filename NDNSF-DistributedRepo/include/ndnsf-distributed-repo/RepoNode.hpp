#ifndef NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <functional>
#include <map>
#include <mutex>

namespace ndn_service_framework {
class LocalServiceRegistry;
} // namespace ndn_service_framework

namespace ndnsf_distributed_repo {

class RepoNode
{
public:
  using DataReferenceFetcher =
    std::function<std::vector<std::vector<uint8_t>>(const RepoDataReference&)>;

  RepoNode(ndn::Name servicePrefix, StorageCapability capability);
  RepoNode(ndn::Name servicePrefix,
           StorageCapability capability,
           std::shared_ptr<RepoStoreBackend> store);

  const ndn::Name& servicePrefix() const;

  RepoCore& core();

  const RepoCore& core() const;

  void registerServices(ndn_service_framework::ServiceProvider& provider);

  void registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry);

  void registerDeploymentServices(
    ndn_service_framework::ServiceProvider* provider,
    ndn_service_framework::LocalServiceRegistry* registry,
    RepoDeploymentMode mode);

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

  void setDataReferenceFetcher(DataReferenceFetcher fetcher);

  RepoOperationStatus insertWirePackets(
    const RepoDataReference& reference,
    const std::vector<std::vector<uint8_t>>& wirePackets);

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleInsert(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStatus(const std::vector<uint8_t>& request) const;

private:
  ndn_service_framework::ResponseMessage makeResponse(const std::vector<uint8_t>& payload) const;

  ndn_service_framework::ResponseMessage makeError(const std::string& error) const;

  std::string allocateOperationId();

  void rememberStatus(const RepoOperationStatus& status);

private:
  ndn::Name m_servicePrefix;
  RepoCore m_core;
  DataReferenceFetcher m_dataReferenceFetcher;
  mutable std::mutex m_statusMutex;
  std::map<std::string, RepoOperationStatus> m_statusById;
  uint64_t m_nextOperationId = 0;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
