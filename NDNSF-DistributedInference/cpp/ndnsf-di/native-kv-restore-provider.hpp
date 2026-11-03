#ifndef NDNSF_DI_NATIVE_KV_RESTORE_PROVIDER_HPP
#define NDNSF_DI_NATIVE_KV_RESTORE_PROVIDER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

namespace ndnsf::di {

struct NativeProviderHandoffConfig;
struct NativeSelectionProjectionV3;

/** Request-local owned import/use. The synchronous handler retains the runtime. */
struct NativeKvRestoreAdmission
{
  explicit NativeKvRestoreAdmission(NativeProviderRuntime& owner) : runtime(owner) {}
  NativeKvRestoreAdmission(const NativeKvRestoreAdmission&) = delete;
  NativeKvRestoreAdmission& operator=(const NativeKvRestoreAdmission&) = delete;
  ~NativeKvRestoreAdmission() noexcept
  {
    use.reset();
    if (ownership && !retain) {
      try {
        // Busy/unconfirmed releases remain charged in the runtime's existing
        // deferred-cleanup owner; never release another import's state.
        (void)runtime.releaseImportedConversationState(*ownership);
      }
      catch (...) {}
    }
  }

  NativeProviderRuntime& runtime;
  bool retain = false;
  std::optional<ConversationStateImportOwnershipV1> ownership;
  std::optional<ConversationStateUseLeaseV1> use;
};

std::unique_ptr<NativeKvRestoreAdmission>
nativeRestoreKvForInvocation(NativeProviderRuntime& runtime,
  ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeProviderHandoffConfig& policy, const NativeSelectionProjectionV3& projection,
  const DecodeStateIdentityV1& trustedTemplate,
  std::function<std::shared_ptr<NativeModelRunner>()> prepareRunner,
  const ConversationStateStore::ExecutionAdmission* admission,
  std::function<void()> requireCurrent);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_RESTORE_PROVIDER_HPP
