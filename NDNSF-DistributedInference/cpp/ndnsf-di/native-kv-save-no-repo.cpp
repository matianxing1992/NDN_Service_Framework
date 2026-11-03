#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-owner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore-provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-publication.hpp"

#include <stdexcept>

namespace ndnsf::di {

NativeDurableStorageGrantRoleSubmissionV1
nativeGrantKvRoleStorage(ndn_service_framework::ServiceUser&, const std::string&,
  const NativeDurableStorageGrantRoleChallengeV1&, std::uint64_t,
  std::chrono::steady_clock::time_point)
{
  throw std::invalid_argument("KV handoff requires the Repo component");
}

void
bindNativeProviderDurableKvPublisher(NativeProviderCollaborationRuntime& runtime,
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer)
{
  if (!producer) throw std::invalid_argument("HANDOFF_PRODUCER_REQUIRED");
  if (runtime.publishProtectedKvCheckpoint)
    throw std::logic_error("HANDOFF_PUBLICATION_ALREADY_BOUND");
  runtime.publishProtectedKvCheckpoint = [](const ndn::Name&, const ndn::Name&,
    const ndn::Name&, const std::filesystem::path&,
    const NativeProtectedKvCheckpointReferenceV1&, const NativeDurableStorageOwner&,
    const NativeProtectedKvCheckpointLimitsV1&, const std::function<void()>&)
      -> NativeProtectedKvPublicationV1 {
    throw std::invalid_argument("KV handoff requires the Repo component");
  };
}

std::unique_ptr<NativeKvRestoreAdmission>
nativeRestoreKvForInvocation(NativeProviderRuntime&,
  ndn_service_framework::ServiceProvider::CollaborationContext&,
  const NativeProviderHandoffConfig&, const NativeSelectionProjectionV3&,
  const DecodeStateIdentityV1&, std::function<std::shared_ptr<NativeModelRunner>()>,
  const ConversationStateStore::ExecutionAdmission*, std::function<void()>)
{
  throw std::invalid_argument("KV recovery requires the Repo component");
}

void
nativeValidateKvSaveConfig(const NativeProviderHandoffConfig&, const ndn::Name&)
{
  throw std::invalid_argument("KV handoff requires the Repo component");
}

NativeConversationRecord
NativeKvHandoffOwner::verifyAndPromote(
  std::shared_ptr<ndn_service_framework::ServiceUser>, std::shared_ptr<void>,
  std::shared_ptr<void>, std::shared_ptr<NativeConversationCoordinator>,
  std::shared_ptr<const NativeKvOwnerKeyOperations>, const std::string&, const std::string&,
  const std::string&, const NativeJson&, const std::map<std::string, std::string>&,
  const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>&,
  std::chrono::steady_clock::time_point, std::function<void()>)
{
  throw std::invalid_argument("KV handoff requires the Repo component");
}

} // namespace ndnsf::di
