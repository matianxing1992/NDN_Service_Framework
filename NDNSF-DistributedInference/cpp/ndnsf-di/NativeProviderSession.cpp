#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

NativeProviderSession::NativeProviderSession(NativeExecutionPlan plan,
                                             NativeProviderAssignment assignment,
                                             std::shared_ptr<DependencyIo> dependencyIo,
                                             std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
                                             std::size_t workerCount)
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
NativeProviderSession::registerRunner(const NativeModelRunnerSpec& spec)
{
  ensureKnownRole(spec.role);
  m_runtime.registerRunner(spec.role, m_runnerFactory->create(spec));
}

bool
NativeProviderSession::hasRunner(const std::string& role) const
{
  return m_runtime.hasRunner(role);
}

RoleSpec
NativeProviderSession::roleSpec(const std::string& role, const std::string& sessionId) const
{
  return roleSpecFor(m_plan, role, sessionId, m_assignment);
}

std::future<ProviderRoleResult>
NativeProviderSession::executeRoleAsync(const std::string& sessionId,
                                        const std::string& role)
{
  return m_runtime.executeRoleAsync(sessionId, roleSpec(role, sessionId), m_dependencyIo);
}

std::future<ProviderRoleResult>
NativeProviderSession::executeRoleAsync(const std::string& sessionId,
                                        const std::string& role,
                                        std::map<std::string, TensorBundle> initialInputsByScope)
{
  return m_runtime.executeRoleAsync(sessionId,
                                    roleSpec(role, sessionId),
                                    m_dependencyIo,
                                    std::move(initialInputsByScope));
}

void
NativeProviderSession::ensureKnownRole(const std::string& role) const
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

} // namespace ndnsf::di
