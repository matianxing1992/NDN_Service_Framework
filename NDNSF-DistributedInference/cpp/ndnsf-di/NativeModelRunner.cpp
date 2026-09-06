#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

void
NativeOpaqueStateHandleV1::validate() const
{
  if (providerIdentity.empty() || providerBootId.empty() || sessionId.empty() ||
      role.empty() || token.empty()) {
    throw std::invalid_argument("opaque Provider state handle is incomplete");
  }
  if (token.rfind("ndnsf-device-state-v1:", 0) != 0) {
    throw std::invalid_argument("opaque Provider state handle has invalid schema");
  }
}

void
NativeConversationStateHandleV1::validate() const
{
  opaque.validate();
  if (conversationKey.empty() || conversationKey.find('\n') != std::string::npos ||
      logicalBytes == 0) {
    throw std::invalid_argument(
      "Provider conversation state handle is incomplete");
  }
}

const std::optional<ExecutionEvidence>&
NativeModelRunner::executionEvidence() const
{
  static const std::optional<ExecutionEvidence> none;
  return none;
}

std::optional<std::map<std::string, TensorBundle>>
NativeModelRunner::runStreamed(const RoleExecutionContext&)
{
  return std::nullopt;
}

std::optional<ExecutionEvidence>
NativeModelRunner::executionEvidenceSnapshot() const
{
  return executionEvidence();
}

std::optional<NativeRuntimeMetrics>
NativeModelRunner::runtimeMetricsSnapshot() const
{
  return std::nullopt;
}

bool
NativeModelRunner::supportsOpaqueStateHandles() const
{
  return false;
}

std::optional<NativeOpaqueStateHandleV1>
NativeModelRunner::stateHandleSnapshot(const std::string&) const
{
  return std::nullopt;
}

void
NativeModelRunner::releaseSessionState(const std::string&)
{
}

bool
NativeModelRunner::supportsConversationStateTransfer() const
{
  return false;
}

std::optional<NativeConversationStateHandleV1>
NativeModelRunner::promoteSessionStateToConversation(
  const std::string&, const std::string&)
{
  return std::nullopt;
}

bool
NativeModelRunner::restoreConversationState(
  const NativeConversationStateHandleV1&, const std::string&)
{
  return false;
}

bool
NativeModelRunner::pauseConversationStateToHost(
  const NativeConversationStateHandleV1&)
{
  return false;
}

std::future<bool>
NativeModelRunner::prefetchConversationStateToGpu(
  const NativeConversationStateHandleV1&)
{
  std::promise<bool> promise;
  promise.set_value(false);
  return promise.get_future();
}

bool
NativeModelRunner::cancelConversationStatePrefetch(
  const NativeConversationStateHandleV1&)
{
  return false;
}

bool
NativeModelRunner::releaseConversationState(
  const NativeConversationStateHandleV1&)
{
  return false;
}

LambdaModelRunner::LambdaModelRunner(RoleRunner runner,
                                     std::optional<ExecutionEvidence> evidence)
  : m_runner(std::move(runner))
  , m_evidence(std::move(evidence))
{
  if (!m_runner) {
    throw std::invalid_argument("LambdaModelRunner requires a runner");
  }
}

const std::optional<ExecutionEvidence>&
LambdaModelRunner::executionEvidence() const
{
  return m_evidence;
}

std::map<std::string, TensorBundle>
LambdaModelRunner::run(const RoleExecutionContext& ctx)
{
  return m_runner(ctx);
}

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner, ExecutionEvidence evidence)
{
  evidence.validate();
  return std::make_shared<LambdaModelRunner>(std::move(runner), std::move(evidence));
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
