#ifndef NDNSF_DI_NATIVE_PROVIDER_RUNNER_REUSE_CACHE_HPP
#define NDNSF_DI_NATIVE_PROVIDER_RUNNER_REUSE_CACHE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderResidencyV4.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

/**
 * Provider-process cache for a fully constructed model runner.
 *
 * The cache key contains the immutable model/role/backend/IO/KV contract and
 * bundle tensor names. ScopeBound additionally includes runner-bound output
 * scopes; RoleContract requires generic outputs routed by the current worker.
 * The caller still
 * performs current authorization, Selection, placement and generation checks
 * before lookup. Protected entries hold only a residency lease; request
 * runtimes and content keys are never retained by the cache.
 */
class NativeProviderRunnerReuseCache final
{
public:
  enum class OutputBinding { ScopeBound, RoleContract };

  explicit NativeProviderRunnerReuseCache(std::size_t maxEntries = 8);
  // RoleContract requires runners with generic tensor outputs. The current
  // authenticated role worker owns transport scopes; model/IO/lease checks stay.
  NativeProviderRunnerReuseCache(std::size_t maxEntries, OutputBinding outputBinding);

  static void prepareRoleContractOutputs(NativeModelRunnerSpec& spec);

  std::shared_ptr<NativeModelRunner>
  lookup(const NativeSelectionProjectionV3& projection,
         const std::string& providerIdentity,
         const std::string& providerBootId,
         const std::shared_ptr<ProtectedRuntime>& protectedRuntime);

  void
  publish(const NativeSelectionProjectionV3& projection,
          const std::string& providerIdentity,
          const std::string& providerBootId,
          const std::shared_ptr<ProtectedRuntime>& protectedRuntime,
          const std::shared_ptr<NativeModelRunner>& runner);

  NativeProviderResidencyOwnerResultV4 queryPlacementResidencyV4(
    const std::vector<NativeJson>& candidateArtifacts,
    const std::string& processEpoch,
    std::uint64_t capturedAtMs,
    std::uint64_t expiresAtMs) const;

  void
  setAuthority(const std::shared_ptr<ProtectedResidentAuthority>& authority) noexcept;

  void
  evictProtectedIdentity(const std::string& protectedIdentity) noexcept;

  void
  clear() noexcept;

private:
  struct State;
  std::shared_ptr<State> m_state;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_PROVIDER_RUNNER_REUSE_CACHE_HPP
