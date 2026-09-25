#ifndef NDNSF_DI_NATIVE_PROVIDER_RUNNER_REUSE_CACHE_HPP
#define NDNSF_DI_NATIVE_PROVIDER_RUNNER_REUSE_CACHE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace ndnsf::di {

/**
 * Provider-process cache for a fully constructed model runner.
 *
 * The cache key contains only the immutable model/role/backend/IO/KV
 * contract.  The caller still performs the current authorization,
 * Selection, placement and generation checks before lookup.  Protected
 * entries hold only a residency lease; request runtimes and content keys are
 * never retained by the cache.
 */
class NativeProviderRunnerReuseCache final
{
public:
  explicit NativeProviderRunnerReuseCache(std::size_t maxEntries = 8);

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
