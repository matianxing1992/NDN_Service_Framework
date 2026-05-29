#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

namespace ndnsf_distributed_repo {

class RepoNode;

struct StoreOptions
{
  std::string objectType = "object";
  uint32_t replicationFactor = 1;
  std::vector<std::string> replicaNodes;
  std::string policyEpoch;
};

class RepoClient
{
public:
  static constexpr const char* DEFAULT_SERVICE_NAME = "/NDNSF/DistributedRepo";

  static RepoObjectManifest put(RepoNode& node,
                                const std::string& objectName,
                                const std::vector<uint8_t>& payload,
                                StoreOptions options = {});

  static std::vector<uint8_t> get(const RepoNode& node,
                                  const std::string& objectName);

  static RepoObjectManifest getManifest(const RepoNode& node,
                                        const std::string& objectName);

  static std::vector<RepoObjectManifest> list(const RepoNode& node);

  static bool remove(RepoNode& node,
                     const std::string& objectName);

  static RepoObjectManifest makeManifest(std::string objectName,
                                         std::string objectType,
                                         const std::vector<uint8_t>& payload,
                                         uint32_t replicationFactor,
                                         std::vector<std::string> replicaNodes,
                                         std::string policyEpoch);

  static ndn_service_framework::RequestMessage makeRequest(
    const std::vector<uint8_t>& payload);

  static ndn::Name requestCapability(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestStore(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest,
    const std::vector<uint8_t>& payload,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestFetch(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestManifest(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestInventory(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestDelete(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
