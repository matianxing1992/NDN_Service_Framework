#ifndef NDNSF_DI_NATIVE_KV_OWNER_HPP
#define NDNSF_DI_NATIVE_KV_OWNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"

#include <cstdint>

namespace ndn_service_framework {
class ServiceUser;
}

namespace ndnsf_distributed_repo {
struct RepoUserStoragePacketSetFetchLimits;
}

namespace ndnsf::di {

/** Runtime binds this to its actual certificate/TPM and existing bounded Face port. */
struct NativeKvOwnerKeyOperations
{
  ndn::security::Certificate certificate;
  std::function<ndn::Buffer(const NativeProtectedKvCheckpointReferenceV1&, const ndn::Buffer&,
    std::chrono::steady_clock::time_point, std::function<void()>)> unwrap;
  std::function<ndn_service_framework::RecipientEncryptedAssignment(
    const NativeProtectedKvCheckpointReferenceV1&, const ndn::Buffer&,
    const NativeProtectedKvRecipientBindingV1&, std::chrono::steady_clock::time_point,
    std::function<void()>)> wrap;
};

NativeDurableStorageGrantRoleSubmissionV1
nativeGrantKvRoleStorage(ndn_service_framework::ServiceUser& user,
  const std::string& operationId, const NativeDurableStorageGrantRoleChallengeV1& challenge,
  std::uint64_t expiresAtMs, std::chrono::steady_clock::time_point deadline);

/** Owner proof/promotion leaf used by the original asynchronous handoff transaction. */
class NativeKvHandoffOwner
{
public:
  /** Reject aggregate read work before any role starts a Repo transaction. */
  static void
  validateReadBudget(std::uint64_t roleCount,
    const ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits& limits);

  static NativeConversationRecord verifyAndPromote(
    std::shared_ptr<ndn_service_framework::ServiceUser> user, std::shared_ptr<void> coreOwner,
    std::shared_ptr<void> activity,
    std::shared_ptr<NativeConversationCoordinator> conversations,
    std::shared_ptr<const NativeKvOwnerKeyOperations> keys,
    const std::string& operationId, const std::string& conversationId,
    const std::string& expectedCheckpoint,
    const NativeJson& references, const std::map<std::string, std::string>& destinations,
    const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>& trustedSourcePolicies,
    std::chrono::steady_clock::time_point deadline,
    std::function<void()> requireCurrent);
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_OWNER_HPP
