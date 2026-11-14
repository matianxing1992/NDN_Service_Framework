#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_COMPAT_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_COMPAT_HPP

#include "ndnsf-distributed-repo/RepoClient.hpp"

namespace ndnsf_distributed_repo {

/** Compatibility entry points kept while their consumers migrate to the unified
 *  client. This header is deliberately not installed: nothing new should be built on
 *  it, and each family leaves it once its callers use the facade. */
class RepoClientCompat
{
public:
  static ndn::Name requestUserStorageGrant(
    ndn_service_framework::ServiceUser& user, const RepoArtifactTarget& target,
    const RepoUserStorageGrantRequest& request, int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
  static ndn::Name requestUserStorageList(
    ndn_service_framework::ServiceUser& user, const RepoArtifactTarget& target,
    const RepoUserStorageListRequest& request, int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
  static ndn::Name requestUserStorageUsage(
    ndn_service_framework::ServiceUser& user, const RepoArtifactTarget& target,
    const RepoUserStorageUsageRequest& request, int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
  /** PENDING retains charge; only a bound DELETED reply confirms cleanup. */
  static ndn::Name requestUserStorageDelete(
    ndn_service_framework::ServiceUser& user, const RepoArtifactTarget& target,
    const RepoUserStorageDeleteRequest& request, int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
  static std::vector<std::vector<uint8_t>> fetchOriginalDataPackets(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, const RepoArtifactTarget& target,
    const std::vector<ndn::Name>& names, uint64_t maxWireBytes,
    const std::function<bool(const ndn::Data&)>& verifyPacket, int timeoutMs,
    const std::function<void()>& requireCurrent = {},
    std::shared_ptr<void> operationLifetime = {});
  static std::vector<std::vector<uint8_t>> fetchOriginalDataPacketSet(
    ndn_service_framework::ServiceUser& user, const RepoDataReference& reference,
    const std::function<bool(const ndn::Data&)>& verifyProducerSignature, int timeoutMs,
    RepoUserStoragePacketSetFetchLimits limits = {},
    const std::function<void()>& requireCurrent = {});
  static std::vector<uint8_t> fetchOriginalDataPacket(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& dataName,
    const std::string& expectedWireSha256,
    const std::vector<OriginalDataReplica>& replicas,
    const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
    int timeoutMs);
  static std::vector<uint8_t> fetchOriginalDataPacketFromUserStorage(
    ndn_service_framework::ServiceUser& user,
    const RepoArtifactTarget& target,
    const ndn::Name& dataName,
    uint64_t expectedWireBytes,
    const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
    int timeoutMs);
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_COMPAT_HPP
