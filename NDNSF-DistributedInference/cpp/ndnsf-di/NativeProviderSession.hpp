#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <future>
#include <memory>
#include <string>
#include <thread>

namespace ndnsf::di {

class NativeProviderSession
{
public:
  NativeProviderSession(NativeExecutionPlan plan,
                        NativeProviderAssignment assignment,
                        std::shared_ptr<DependencyIo> dependencyIo,
                        std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
                        std::size_t workerCount = std::thread::hardware_concurrency());

  void
  registerRunner(const NativeModelRunnerSpec& spec);

  bool
  hasRunner(const std::string& role) const;

  RoleSpec
  roleSpec(const std::string& role, const std::string& sessionId) const;

  std::future<ProviderRoleResult>
  executeRoleAsync(const std::string& sessionId, const std::string& role);

  std::future<ProviderRoleResult>
  executeRoleAsync(const std::string& sessionId,
                   const std::string& role,
                   std::map<std::string, TensorBundle> initialInputsByScope);

private:
  void
  ensureKnownRole(const std::string& role) const;

private:
  NativeExecutionPlan m_plan;
  NativeProviderAssignment m_assignment;
  std::shared_ptr<DependencyIo> m_dependencyIo;
  std::shared_ptr<NativeModelRunnerFactory> m_runnerFactory;
  NativeProviderRuntime m_runtime;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
