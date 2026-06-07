#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace ndnsf::di {

class NativeProviderRuntime
{
public:
  explicit NativeProviderRuntime(std::size_t workerCount = std::thread::hardware_concurrency())
    : m_worker(workerCount)
  {
  }

  void
  registerRunner(std::string role, std::shared_ptr<NativeModelRunner> runner)
  {
    if (role.empty()) {
      throw std::invalid_argument("NativeProviderRuntime role must not be empty");
    }
    if (!runner) {
      throw std::invalid_argument("NativeProviderRuntime runner must not be null");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_runners[std::move(role)] = std::move(runner);
  }

  void
  registerRunner(std::string role, RoleRunner runner)
  {
    registerRunner(std::move(role), makeNativeModelRunner(std::move(runner)));
  }

  bool
  hasRunner(const std::string& role) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_runners.find(role) != m_runners.end();
  }

  std::future<ProviderRoleResult>
  executeRoleAsync(std::string sessionId,
                   RoleSpec role,
                   std::shared_ptr<DependencyIo> io)
  {
    auto runner = findRunner(role.role);
    return m_worker.executeAsync(std::move(sessionId),
                                 std::move(role),
                                 std::move(io),
                                 std::move(runner));
  }

private:
  std::shared_ptr<NativeModelRunner>
  findRunner(const std::string& role) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_runners.find(role);
    if (found == m_runners.end()) {
      throw std::out_of_range("NativeProviderRuntime has no runner for role: " + role);
    }
    return found->second;
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_runners;
  ProviderRoleWorker m_worker;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
