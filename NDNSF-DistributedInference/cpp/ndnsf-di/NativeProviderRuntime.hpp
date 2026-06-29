#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ndnsf::di {

class NativeProviderRuntime
{
public:
  explicit NativeProviderRuntime(std::size_t workerCount = std::thread::hardware_concurrency());

  void
  registerRunner(std::string role, std::shared_ptr<NativeModelRunner> runner);

  void
  registerRunner(std::string role, RoleRunner runner);

  bool
  hasRunner(const std::string& role) const;

  std::future<ProviderRoleResult>
  executeRoleAsync(std::string sessionId,
                   RoleSpec role,
                   std::shared_ptr<DependencyIo> io,
                   std::map<std::string, TensorBundle> initialInputsByScope = {});

  ProviderRoleWorkerSnapshot
  snapshot() const;

private:
  std::shared_ptr<NativeModelRunner>
  findRunner(const std::string& role) const;

private:
  mutable std::mutex m_mutex;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_runners;
  ProviderRoleWorker m_worker;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
