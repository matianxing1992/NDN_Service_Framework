#ifndef NDNSF_DI_NATIVE_RUNNER_PREPARATION_HPP
#define NDNSF_DI_NATIVE_RUNNER_PREPARATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

namespace ndnsf::di {

/** Provider-owned observations shared by all post-Selection model adapters. */
struct NativeRunnerPreparationContext
{
  std::string providerName;
  std::string providerBootId;
  std::uint64_t providerStartedAtMs = 0;
  std::string cacheDirectory;
};

/** Bind trusted observations after the adapter constructs its runner spec.
 * Adapter metadata cannot override this context. Authorization, leases and
 * cancellation remain owned by ProtectedRuntime and ProviderRoleWorker.
 */
void
bindNativeRunnerPreparationContext(NativeModelRunnerSpec& spec,
                                   const NativeSelectionProjectionV3& projection,
                                   const NativeRunnerPreparationContext& context);

} // namespace ndnsf::di

#endif
