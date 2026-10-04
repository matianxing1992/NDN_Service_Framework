#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ndnsf::di::test {
namespace {

std::string
digest(char value)
{
  return "sha256:" + std::string(64, value);
}

DecodeStateIdentityV1
makeIdentity(const std::string& requestId, const std::string& generationId,
             std::uint64_t stateEpoch, std::uint32_t prefixTokenCount)
{
  DecodeStateIdentityV1 identity;
  identity.modelDigest = digest('1');
  identity.graphSemanticDigest = digest('2');
  identity.artifactDigest = digest('3');
  identity.adapterDigest = digest('4');
  identity.tokenizerDigest = digest('5');
  identity.runnerDigest = digest('6');
  identity.roleName = "/LLM/Pipeline/Stage/0";
  identity.roleSplitDigest = digest('7');
  identity.layerBegin = 0;
  identity.layerEnd = 1;
  identity.prefixDigest = digest('8');
  identity.prefixTokenCount = prefixTokenCount;
  identity.positionDigest = digest(stateEpoch == 0 ? '9' : '0');
  identity.precision = "fp32";
  identity.layoutDigest = digest('a');
  identity.stateSchemaDigest = digest('b');
  identity.stateComponentDigests = {digest('c')};
  identity.runtimeAbiDigest = digest('d');
  identity.securityDomainDigest = digest('e');
  identity.providerIdentity = "/provider/A";
  identity.providerBootId = "boot-a";
  identity.cacheEpoch = 7;
  identity.stateInferenceEpoch = stateEpoch;
  if (stateEpoch > 0) {
    identity.predecessorInferenceEpoch = stateEpoch - 1;
  }
  identity.requestId = requestId;
  identity.attemptEpoch = 1;
  identity.generationId = generationId;
  identity.validate();
  return identity;
}

ConversationStateBinding
makeConversationBinding(const DecodeStateIdentityV1& identity)
{
  ConversationStateBinding binding;
  binding.conversationId = "conversation-session-runner-test";
  binding.contextEpoch = 1;
  binding.serviceName = "/LLM/Pipeline/Generate";
  binding.planRoleMapDigest = digest('f');
  binding.receiptDigest = digest('a');
  binding.expiresAtMs = 10'000;
  binding.identity = identity;
  binding.validate();
  return binding;
}

RoleSpec
makeRole(const DecodeStateIdentityV1& identity,
         std::optional<DecodeStateIdentityV1> predecessor = std::nullopt,
         std::optional<ConversationStateBinding> conversation = std::nullopt)
{
  RoleSpec role;
  role.role = identity.roleName;
  role.requestId = identity.requestId;
  role.attemptEpoch = identity.attemptEpoch;
  role.inferenceEpoch = identity.stateInferenceEpoch;
  GenerationEpochLineageV1 lineage;
  lineage.requestId = identity.requestId;
  lineage.attemptEpoch = identity.attemptEpoch;
  lineage.planDigest = digest('f');
  lineage.generationId = identity.generationId;
  lineage.streamEpoch = 1;
  lineage.inferenceEpoch = identity.stateInferenceEpoch;
  lineage.transitionKind = identity.stateInferenceEpoch == 0
    ? GenerationEpochLineageV1::PREFILL
    : GenerationEpochLineageV1::DECODE;
  lineage.logicalPrefixDigest = identity.prefixDigest;
  lineage.logicalPrefixTokenCount = identity.prefixTokenCount;
  lineage.positionDigest = identity.positionDigest;
  lineage.producerRole = identity.roleName;
  lineage.consumerRole = "/LLM/Pipeline/Consumer";
  role.generationLineage = std::move(lineage);
  role.stateInputNames = {"past_key_values.0.key"};
  role.stateOutputNames = {"present.0.key"};
  role.predecessorDecodeStateIdentity = std::move(predecessor);
  role.candidateDecodeStateIdentity = identity;
  role.conversationStateBinding = std::move(conversation);
  role.conversationStateLookupNowMs = 2'000;
  role.deferStateCommit = true;
  role.streamingStateExecution = true;
  return role;
}

class EmptyDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    std::promise<TensorBundle> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(
      std::logic_error("unexpected dependency fetch in session runner test")));
    return future;
  }

  void
  publishOutput(const std::string&, const DependencyEdge&,
                const TensorBundle&) override
  {
    throw std::logic_error("unexpected dependency publish in session runner test");
  }
};

class ConversationStateRunner final : public NativeModelRunner
{
public:
  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& context) final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto state = m_stateBySession.find(context.sessionId);
    if (context.inferenceEpoch > 0 && state == m_stateBySession.end()) {
      throw std::runtime_error("test runner lost its Provider-owned predecessor state");
    }
    const auto next = state == m_stateBySession.end() ? 1 : state->second + 1;
    m_stateBySession[context.sessionId] = next;

    NativeOpaqueStateHandleV1 handle;
    handle.providerIdentity = "/provider/A";
    handle.providerBootId = context.providerBootId;
    handle.sessionId = context.sessionId;
    handle.role = context.role;
    handle.token = "ndnsf-resident-state-v1:test-state-" +
                   std::to_string(next);
    handle.stateInferenceEpoch = context.inferenceEpoch;
    handle.validate();
    m_handleBySession[context.sessionId] = std::move(handle);
    ++m_runCount;
    return {};
  }

  bool
  supportsOpaqueStateHandles() const final
  {
    return true;
  }

  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_handleBySession.find(sessionId);
    return found == m_handleBySession.end()
      ? std::nullopt : std::optional<NativeOpaqueStateHandleV1>{found->second};
  }

  void
  releaseSessionState(const std::string& sessionId) final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stateBySession.erase(sessionId);
    m_handleBySession.erase(sessionId);
  }

  bool
  supportsConversationStateTransfer() const final
  {
    return true;
  }

  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                   const std::string& conversationKey) final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto state = m_stateBySession.find(sessionId);
    const auto handle = m_handleBySession.find(sessionId);
    if (state == m_stateBySession.end() || handle == m_handleBySession.end()) {
      return std::nullopt;
    }
    m_stateByConversation[conversationKey] = state->second;
    NativeConversationStateHandleV1 result;
    result.opaque = handle->second;
    result.conversationKey = conversationKey;
    result.logicalBytes = sizeof(std::uint64_t);
    result.validate();
    m_stateBySession.erase(state);
    m_handleBySession.erase(handle);
    return result;
  }

  bool
  restoreConversationState(const NativeConversationStateHandleV1& state,
                           const std::string& sessionId) final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_stateByConversation.find(state.conversationKey);
    if (found == m_stateByConversation.end()) {
      return false;
    }
    m_stateBySession[sessionId] = found->second;
    return true;
  }

  bool
  pauseConversationStateToHost(const NativeConversationStateHandleV1&) final
  {
    return true;
  }

  std::future<bool>
  prefetchConversationStateToGpu(const NativeConversationStateHandleV1&) final
  {
    std::promise<bool> promise;
    promise.set_value(true);
    return promise.get_future();
  }

  bool
  cancelConversationStatePrefetch(const NativeConversationStateHandleV1&) final
  {
    return true;
  }

  bool
  releaseConversationState(const NativeConversationStateHandleV1& state) final
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stateByConversation.erase(state.conversationKey) != 0;
  }

  std::size_t
  runCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_runCount;
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::string, std::uint64_t> m_stateBySession;
  std::map<std::string, std::uint64_t> m_stateByConversation;
  std::map<std::string, NativeOpaqueStateHandleV1> m_handleBySession;
  std::size_t m_runCount = 0;
};

} // namespace

BOOST_AUTO_TEST_CASE(Spec194ConversationRestoreReusesRunnerAcrossDecodeEpochs)
{
  NativeProviderRuntime runtime(1);
  auto io = std::make_shared<EmptyDependencyIo>();
  std::atomic<std::size_t> factoryCalls{0};
  std::shared_ptr<ConversationStateRunner> firstRunner;
  const auto prepareRunner = [&] () -> std::shared_ptr<NativeModelRunner> {
    ++factoryCalls;
    auto runner = std::make_shared<ConversationStateRunner>();
    if (!firstRunner) {
      firstRunner = runner;
    }
    return runner;
  };

  const auto firstIdentity = makeIdentity("request-1", "generation-1", 0, 17);
  auto firstRole = makeRole(firstIdentity);
  auto firstResult = runtime.executePreparedRoleAsync(
    "session-1", firstRole, io, prepareRunner).get();
  BOOST_REQUIRE(firstResult.runner);
  BOOST_CHECK_EQUAL(factoryCalls.load(), 1U);
  BOOST_REQUIRE(runtime.commitDecodeStateTransition("session-1", firstRole));

  const auto conversation = makeConversationBinding(firstIdentity);
  BOOST_REQUIRE(runtime.stageDecodeStatePromotion(
    "session-1", firstRole, conversation, 1'000));
  BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(
    conversation, digest('b')));

  const auto resumedPrefill = makeIdentity("request-2", "generation-2", 0, 18);
  auto resumedRole = makeRole(resumedPrefill, std::nullopt, conversation);
  auto resumedResult = runtime.executePreparedRoleAsync(
    "session-2", resumedRole, io, prepareRunner).get();
  BOOST_REQUIRE(resumedResult.runner);
  BOOST_CHECK_EQUAL(resumedResult.runner.get(), firstRunner.get());
  BOOST_REQUIRE(runtime.commitDecodeStateTransition("session-2", resumedRole));

  const auto resumedDecode = makeIdentity("request-2", "generation-2", 1, 19);
  auto decodeRole = makeRole(resumedDecode, resumedPrefill);
  auto decodeResult = runtime.executePreparedRoleAsync(
    "session-2", decodeRole, io, prepareRunner).get();
  BOOST_REQUIRE(decodeResult.runner);
  BOOST_CHECK_EQUAL(decodeResult.runner.get(), firstRunner.get());
  BOOST_CHECK_EQUAL(factoryCalls.load(), 1U);
  BOOST_CHECK_EQUAL(firstRunner->runCount(), 3U);
  BOOST_REQUIRE(runtime.commitDecodeStateTransition("session-2", decodeRole));
  BOOST_CHECK(runtime.releaseDecodeState("session-2", decodeRole.role));
  BOOST_CHECK(runtime.releaseConversationState(conversation));
}

} // namespace ndnsf::di::test
