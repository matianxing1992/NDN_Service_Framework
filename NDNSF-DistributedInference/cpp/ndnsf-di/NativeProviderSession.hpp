#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace ndnsf::di {

class NativeProviderSession
{
public:
  NativeProviderSession(NativeExecutionPlan plan,
                        NativeProviderAssignment assignment,
                        std::shared_ptr<DependencyIo> dependencyIo,
                        std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
                        std::size_t workerCount = std::thread::hardware_concurrency())
    : m_plan(std::move(plan))
    , m_assignment(std::move(assignment))
    , m_dependencyIo(std::move(dependencyIo))
    , m_runnerFactory(std::move(runnerFactory))
    , m_runtime(workerCount)
  {
    if (!m_dependencyIo) {
      throw std::invalid_argument("NativeProviderSession requires DependencyIo");
    }
    if (!m_runnerFactory) {
      throw std::invalid_argument("NativeProviderSession requires NativeModelRunnerFactory");
    }
  }

  void
  registerRunner(const NativeModelRunnerSpec& spec)
  {
    ensureKnownRole(spec.role);
    m_runtime.registerRunner(spec.role, m_runnerFactory->create(spec));
  }

  bool
  hasRunner(const std::string& role) const
  {
    return m_runtime.hasRunner(role);
  }

  RoleSpec
  roleSpec(const std::string& role, const std::string& sessionId) const
  {
    return roleSpecFor(m_plan, role, sessionId, m_assignment);
  }

  std::future<ProviderRoleResult>
  executeRoleAsync(const std::string& sessionId, const std::string& role)
  {
    return m_runtime.executeRoleAsync(sessionId, roleSpec(role, sessionId), m_dependencyIo);
  }

private:
  void
  ensureKnownRole(const std::string& role) const
  {
    if (role.empty()) {
      throw std::invalid_argument("NativeProviderSession role must not be empty");
    }
    for (const auto& item : m_plan.roles) {
      if (item == role) {
        return;
      }
    }
    throw std::out_of_range("NativeProviderSession has no role in plan: " + role);
  }

private:
  NativeExecutionPlan m_plan;
  NativeProviderAssignment m_assignment;
  std::shared_ptr<DependencyIo> m_dependencyIo;
  std::shared_ptr<NativeModelRunnerFactory> m_runnerFactory;
  NativeProviderRuntime m_runtime;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
