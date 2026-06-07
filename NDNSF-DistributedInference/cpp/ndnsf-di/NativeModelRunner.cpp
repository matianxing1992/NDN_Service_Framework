#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

LambdaModelRunner::LambdaModelRunner(RoleRunner runner)
  : m_runner(std::move(runner))
{
  if (!m_runner) {
    throw std::invalid_argument("LambdaModelRunner requires a runner");
  }
}

std::map<std::string, TensorBundle>
LambdaModelRunner::run(const RoleExecutionContext& ctx)
{
  return m_runner(ctx);
}

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner)
{
  return std::make_shared<LambdaModelRunner>(std::move(runner));
}

void
RegistryNativeModelRunnerFactory::registerBackend(std::string backend, Creator creator)
{
  if (backend.empty()) {
    throw std::invalid_argument("NativeModelRunner backend must not be empty");
  }
  if (!creator) {
    throw std::invalid_argument("NativeModelRunner creator must not be empty");
  }
  m_creators[std::move(backend)] = std::move(creator);
}

bool
RegistryNativeModelRunnerFactory::hasBackend(const std::string& backend) const
{
  return m_creators.find(backend) != m_creators.end();
}

std::shared_ptr<NativeModelRunner>
RegistryNativeModelRunnerFactory::create(const NativeModelRunnerSpec& spec) const
{
  if (spec.backend.empty()) {
    throw std::invalid_argument("NativeModelRunnerSpec.backend must not be empty");
  }
  const auto found = m_creators.find(spec.backend);
  if (found == m_creators.end()) {
    throw std::out_of_range("no NativeModelRunner backend registered: " +
                            spec.backend);
  }
  auto runner = found->second(spec);
  if (!runner) {
    throw std::logic_error("NativeModelRunner backend returned null: " +
                           spec.backend);
  }
  return runner;
}

} // namespace ndnsf::di
