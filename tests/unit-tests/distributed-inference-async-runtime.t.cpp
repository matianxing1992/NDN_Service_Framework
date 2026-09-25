#include "tests/boost-test.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderResourceProbe.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/QwenGenerationSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "ndn-service-framework/NegativeAckReason.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <sstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ndnsf::di::test {

namespace {

TensorBundle
bundle(std::string name, std::string text)
{
  return TensorBundle{
    std::move(name),
    std::vector<uint8_t>(text.begin(), text.end()),
    1,
  };
}

std::string
stateDigest(char value)
{
  return "sha256:" + std::string(64, value);
}

DecodeStateIdentityV1
exactStateIdentity(std::string requestId = "request-a",
                   std::uint64_t attemptEpoch = 1,
                   std::string generationId = "generation-a",
                   std::uint64_t stateInferenceEpoch = 0,
                   std::optional<std::uint64_t> predecessorInferenceEpoch =
                     std::nullopt)
{
  DecodeStateIdentityV1 identity;
  identity.modelDigest = stateDigest('1');
  identity.graphSemanticDigest = stateDigest('2');
  identity.artifactDigest = stateDigest('3');
  identity.adapterDigest = stateDigest('4');
  identity.tokenizerDigest = stateDigest('5');
  identity.runnerDigest = stateDigest('6');
  identity.roleName = "/Stage/0";
  identity.roleSplitDigest = stateDigest('7');
  identity.layerBegin = 0;
  identity.layerEnd = 4;
  identity.prefixDigest = stateDigest('8');
  identity.prefixTokenCount = 3;
  identity.positionDigest = stateDigest('9');
  identity.precision = "fp32";
  identity.layoutDigest = stateDigest('a');
  identity.stateSchemaDigest = stateDigest('b');
  identity.stateComponentDigests = {stateDigest('c'), stateDigest('d')};
  identity.runtimeAbiDigest = stateDigest('e');
  identity.securityDomainDigest = stateDigest('f');
  identity.providerIdentity = "/provider/A";
  identity.providerBootId = "boot-a";
  identity.cacheEpoch = 7;
  identity.stateInferenceEpoch = stateInferenceEpoch;
  identity.predecessorInferenceEpoch = predecessorInferenceEpoch;
  identity.requestId = std::move(requestId);
  identity.attemptEpoch = attemptEpoch;
  identity.generationId = std::move(generationId);
  identity.validate();
  return identity;
}

ConversationStateBinding
exactConversationBinding(const DecodeStateIdentityV1& origin,
                         std::uint64_t contextEpoch = 1,
                         std::uint64_t expiresAtMs = 10'000)
{
  ConversationStateBinding binding;
  binding.conversationId = "conversation-a-0001";
  binding.contextEpoch = contextEpoch;
  binding.serviceName = "/LLM/Pipeline/Generate";
  binding.planRoleMapDigest = stateDigest('0');
  binding.receiptDigest = stateDigest('9');
  binding.expiresAtMs = expiresAtMs;
  binding.identity = origin;
  binding.validate();
  return binding;
}

BOOST_AUTO_TEST_CASE(DecodeStateIdentityRequiresExactPredecessorEpoch)
{
  auto prefill = exactStateIdentity();
  BOOST_CHECK_NO_THROW(prefill.validate());

  prefill.predecessorInferenceEpoch = 0;
  BOOST_CHECK_THROW(prefill.validate(), std::invalid_argument);

  auto decode = exactStateIdentity("request-a", 1, "generation-a", 1, 0);
  BOOST_CHECK_NO_THROW(decode.validate());

  decode.predecessorInferenceEpoch = std::nullopt;
  BOOST_CHECK_THROW(decode.validate(), std::invalid_argument);
  decode.predecessorInferenceEpoch = 1;
  BOOST_CHECK_THROW(decode.validate(), std::invalid_argument);
}

std::string
sha256Hex(const std::string& text)
{
  ndn::util::Sha256 digest;
  digest.update(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(text.data()),
    text.size()));
  return digest.toString();
}

std::string
payloadText(const TensorBundle& value)
{
  return std::string(value.payload.begin(), value.payload.end());
}

template<typename T>
std::vector<std::uint8_t>
rawTensorPayload(std::initializer_list<T> values)
{
  std::vector<T> typed(values);
  std::vector<std::uint8_t> payload(typed.size() * sizeof(T));
  if (!payload.empty()) {
    std::memcpy(payload.data(), typed.data(), payload.size());
  }
  return payload;
}

std::string
ackPayloadText(const ndn_service_framework::ServiceProvider::AckDecision& decision)
{
  return std::string(decision.payload.begin(), decision.payload.end());
}

std::string
typedCapabilityJson(const ndn_service_framework::ServiceProvider::AckDecision& decision)
{
  const auto payload = ackPayloadText(decision);
  const std::string prefix = "providerCapabilityHint=json64:";
  const auto begin = payload.find(prefix);
  BOOST_REQUIRE(begin != std::string::npos);
  const auto encodedBegin = begin + prefix.size();
  const auto encodedEnd = payload.find(';', encodedBegin);
  const auto encoded = payload.substr(
    encodedBegin,
    encodedEnd == std::string::npos ? std::string::npos : encodedEnd - encodedBegin);
  auto valueOf = [] (char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-' || ch == '+') return 62;
    if (ch == '_' || ch == '/') return 63;
    return -1;
  };
  std::string decoded;
  int bits = 0;
  int bitCount = 0;
  for (const char ch : encoded) {
    if (ch == '=') break;
    const int value = valueOf(ch);
    BOOST_REQUIRE(value >= 0);
    bits = (bits << 6) | value;
    bitCount += 6;
    if (bitCount >= 8) {
      bitCount -= 8;
      decoded.push_back(static_cast<char>((bits >> bitCount) & 0xff));
    }
  }
  return decoded;
}

std::vector<uint8_t>
floatPayload(std::initializer_list<float> values)
{
  std::vector<float> floats(values);
  std::vector<uint8_t> payload(floats.size() * sizeof(float));
  std::memcpy(payload.data(), floats.data(), payload.size());
  return payload;
}

std::vector<float>
payloadFloats(const TensorBundle& value)
{
  BOOST_REQUIRE(value.payload.size() % sizeof(float) == 0);
  std::vector<float> floats(value.payload.size() / sizeof(float));
  std::memcpy(floats.data(), value.payload.data(), value.payload.size());
  return floats;
}

class FakeDependencyIo : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.push_back(sessionId);
      prefetchedScopes.push_back(edge.scope);
    }
    return std::async(std::launch::async, [edge] {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      return bundle(edge.scope, "input:" + edge.scope);
    });
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& value) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    sessions.push_back(sessionId);
    publishedByScope[edge.scope] = value;
  }

public:
  std::mutex mutex;
  std::vector<std::string> sessions;
  std::vector<std::string> prefetchedScopes;
  std::map<std::string, TensorBundle> publishedByScope;
};

class BlockingDependencyIo : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override
  {
    auto promise = std::make_shared<std::promise<TensorBundle>>();
    auto future = promise->get_future();
    const auto itemKey = key(sessionId, edge);
    {
      std::lock_guard<std::mutex> lock(mutex);
      prefetchedNames.push_back(edge.plannedDataName);
      if (prefetchObserver) {
        prefetchObserver(edge.plannedDataName);
      }
      const auto found = available.find(itemKey);
      if (found != available.end()) {
        promise->set_value(found->second);
        return future;
      }
      waiters[itemKey].push_back(std::move(promise));
    }
    return future;
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& value) override
  {
    std::vector<std::shared_ptr<std::promise<TensorBundle>>> ready;
    {
      std::lock_guard<std::mutex> lock(mutex);
      publishedNames.push_back(edge.plannedDataName);
      if (publishObserver) {
        publishObserver(edge.plannedDataName);
      }
      const auto itemKey = key(sessionId, edge);
      available[itemKey] = value;
      const auto found = waiters.find(itemKey);
      if (found != waiters.end()) {
        ready = std::move(found->second);
        waiters.erase(found);
      }
    }
    for (auto& promise : ready) {
      promise->set_value(value);
    }
  }

private:
  static std::string
  key(const std::string& sessionId, const DependencyEdge& edge)
  {
    return sessionId + "|" + edge.plannedDataName;
  }

public:
  std::mutex mutex;
  std::map<std::string, TensorBundle> available;
  std::map<std::string, std::vector<std::shared_ptr<std::promise<TensorBundle>>>> waiters;
  std::vector<std::string> prefetchedNames;
  std::vector<std::string> publishedNames;
  std::function<void(const std::string&)> prefetchObserver;
  std::function<void(const std::string&)> publishObserver;
};

class FailingPublishDependencyIo : public DependencyIo
{
public:
  explicit FailingPublishDependencyIo(bool failPublication)
    : failPublication(failPublication)
  {
  }

  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    std::promise<TensorBundle> promise;
    promise.set_exception(std::make_exception_ptr(
      std::runtime_error("unexpected publication-failure prefetch")));
    return promise.get_future();
  }

  void
  publishOutput(const std::string&,
                const DependencyEdge&,
                const TensorBundle&) override
  {
    ++publicationCalls;
    if (failPublication) {
      throw std::runtime_error("injected dependency publication failure");
    }
  }

public:
  bool failPublication = false;
  std::atomic<std::size_t> publicationCalls{0};
};

enum class NativeEpochPublicationFault
{
  EventAdmission,
  FeedbackPublication,
  ActivationPublication,
};

struct NativeEpochPublicationFailureResult
{
  std::string error;
  std::size_t runnerCalls = 0;
  std::size_t eventCalls = 0;
  std::size_t publicationCalls = 0;
  ProviderDecodeStateSnapshot state;
};

NativeEpochPublicationFailureResult
runNativeEpochPublicationFailure(NativeEpochPublicationFault fault)
{
  NativeProviderRuntime runtime(1);
  const auto identityTemplate = exactStateIdentity();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-stateful";
  runnerSpec.path = "/tmp/stage-0.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", identityTemplate.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", identityTemplate.providerIdentity},
    {"evidence.providerBootId", identityTemplate.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identityTemplate.generationId},
    {"state.schemaDigest", identityTemplate.stateSchemaDigest},
  };

  std::atomic<std::size_t> runnerCalls{0};
  auto runner = makeNativeModelRunner(
    [&runnerCalls] (const RoleExecutionContext&) {
      ++runnerCalls;
      NamedTensor logits;
      logits.name = "logits";
      logits.elementType = TensorElementType::Float32;
      logits.shape = {1, 2};
      logits.payload = floatPayload({0.0f, 1.0f});
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({1});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle(
           "onnx-output-bundle", {std::move(logits), std::move(state)})},
      };
    });
  runtime.registerRunner(runnerSpec, runner);

  NativeExecutionPlan plan;
  plan.roles = {runnerSpec.role};
  NativeDependencySpec output;
  output.producers = {runnerSpec.role};
  output.keyScope = fault == NativeEpochPublicationFault::ActivationPublication
    ? "activation" : "token-feedback";
  output.topicPrefix = "/ndnsf-di";
  output.objectNameTemplate = fault ==
      NativeEpochPublicationFault::ActivationPublication
    ? "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{producerRole}/bundle/{sequence}"
    : "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  output.operationKind = fault ==
      NativeEpochPublicationFault::ActivationPublication
    ? "ACTIVATION" : "TOKEN_FEEDBACK";
  if (fault == NativeEpochPublicationFault::ActivationPublication) {
    plan.roles.push_back("/Consumer");
    output.consumers = {"/Consumer"};
  }
  else {
    output.consumers = {runnerSpec.role};
  }
  plan.dependencies = {output};

  NativeProviderAssignment assignment;
  assignment.providerByRole[runnerSpec.role] = "/provider/A";
  if (fault == NativeEpochPublicationFault::ActivationPublication) {
    assignment.providerByRole["/Consumer"] = "/provider/B";
  }
  auto io = std::make_shared<FailingPublishDependencyIo>(
    fault != NativeEpochPublicationFault::EventAdmission);

  std::atomic<std::size_t> eventCalls{0};
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "publication-failure-session";
  config.requestId = "publication-failure-request";
  config.attemptEpoch = 1;
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = "/provider/A";
  config.role = runnerSpec.role;
  config.initialInputs = {
    {"input_ids",
     makeEncodedTensorBundle(
       "prompt",
       {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                    rawTensorPayload<std::int64_t>({11, 12, 13})}})},
  };
  config.maxEpochs = 1;
  config.tokenInputName = "input_ids";
  config.stateInputNames = {"attention_kv_in"};
  config.stateOutputNames = {"attention_kv_out"};
  config.stateIdentityTemplate = identityTemplate;
  config.positionPolicyDigest = identityTemplate.positionDigest;
  config.samplingDigest = "sha256:sampling";
  config.prepareRunner = [runner] { return runner; };
  if (fault != NativeEpochPublicationFault::ActivationPublication) {
    config.eventSink = [&eventCalls, fault] (const std::vector<std::uint8_t>&) {
      ++eventCalls;
      return fault != NativeEpochPublicationFault::EventAdmission;
    };
  }

  NativeEpochPublicationFailureResult result;
  try {
    runNativeEpochCoordinator(std::move(config));
  }
  catch (const std::exception& error) {
    result.error = error.what();
  }
  result.runnerCalls = runnerCalls.load();
  result.eventCalls = eventCalls.load();
  result.publicationCalls = io->publicationCalls.load();
  result.state = runtime.decodeStateSnapshot();
  return result;
}

class ImmediateDependencyIo : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    sessions.push_back(sessionId);
    prefetchedScopes.push_back(edge.scope);
    std::promise<TensorBundle> promise;
    promise.set_value(bundle(edge.scope, "immediate:" + edge.scope));
    return promise.get_future();
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& value) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    sessions.push_back(sessionId);
    publishedByScope[edge.scope] = value;
  }

public:
  std::mutex mutex;
  std::vector<std::string> sessions;
  std::vector<std::string> prefetchedScopes;
  std::map<std::string, TensorBundle> publishedByScope;
};

class EchoNativeRunner : public NativeModelRunner
{
public:
  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final
  {
    BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
    return {
      {"native-to-user", bundle("native-result",
                                "native:" + payloadText(ctx.inputsByScope.begin()->second))},
    };
  }
};

class StreamEventNativeRunner : public NativeModelRunner
{
public:
  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final
  {
    BOOST_REQUIRE(static_cast<bool>(ctx.streamEventSink));
    const std::vector<std::uint8_t> event{'t', 'o', 'k', 'e', 'n', '-', '1'};
    BOOST_REQUIRE(ctx.streamEventSink(event));
    return {{"terminal", bundle("terminal", "result")}};
  }
};

class IncrementalTestRunner final : public NativeModelRunner
{
public:
  IncrementalTestRunner(std::atomic<int>& oneShotRuns,
                        std::atomic<int>& streamedRuns)
    : m_oneShotRuns(oneShotRuns)
    , m_streamedRuns(streamedRuns)
  {
  }

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext&) final
  {
    ++m_oneShotRuns;
    return {{"final-response", bundle("final-response", "one-shot-result")}};
  }

  std::optional<std::map<std::string, TensorBundle>>
  runStreamed(const RoleExecutionContext& ctx) final
  {
    ++m_streamedRuns;
    BOOST_REQUIRE(static_cast<bool>(ctx.streamEventSink));
    BOOST_REQUIRE(ctx.streamEventSink(
      std::vector<std::uint8_t>{'s', 't', 'r', 'e', 'a', 'm'}));
    return std::map<std::string, TensorBundle>{
      {"final-response", bundle("final-response", "incremental-result")},
    };
  }

private:
  std::atomic<int>& m_oneShotRuns;
  std::atomic<int>& m_streamedRuns;
};

/**
 * Test-only adapter that makes ownership transitions observable.  It models
 * the CUDA adapter boundary without fabricating a TensorBundle in the
 * ConversationStateStore: state lives behind the conversation key and every
 * residency transition is an explicit adapter callback.
 */
class ConversationTransferRunner final : public NativeModelRunner
{
public:
  struct Counters
  {
    std::atomic<int> promote{0};
    std::atomic<int> restore{0};
    std::atomic<int> pause{0};
    std::atomic<int> prefetch{0};
    std::atomic<int> release{0};
  } counters;

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final
  {
    if (ctx.inputsByScope.count("__ndnsf_provider_decode_state") != 0) {
      // A conversation restore must reach the adapter as a binding, not as a
      // serialized state bundle supplied by the runtime coordinator.
      restoredWithoutBundle.store(false);
    }
    NamedTensor state;
    state.name = "attention_kv_out";
    state.elementType = TensorElementType::Int64;
    state.shape = {1};
    state.payload = rawTensorPayload<std::int64_t>({42});
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessionState[ctx.sessionId] = true;
    }
    return {
      {"onnx-output-bundle",
       makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
    };
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
    std::lock_guard<std::mutex> lock(mutex);
    if (!sessionState.count(sessionId) || conversationState.count(conversationKey)) {
      return std::nullopt;
    }
    sessionState.erase(sessionId);
    conversationState[conversationKey] = true;
    ++counters.promote;
    return makeHandle(sessionId, conversationKey);
  }

  bool
  restoreConversationState(const NativeConversationStateHandleV1& state,
                           const std::string& sessionId) final
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!conversationState.count(state.conversationKey)) {
      return false;
    }
    restoredWithoutBundle.store(true);
    restoredSession = sessionId;
    ++counters.restore;
    return true;
  }

  bool
  pauseConversationStateToHost(const NativeConversationStateHandleV1& state) final
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!conversationState.count(state.conversationKey) ||
        hostState.count(state.conversationKey)) {
      return false;
    }
    hostState[state.conversationKey] = true;
    conversationState.erase(state.conversationKey);
    ++counters.pause;
    return true;
  }

  std::future<bool>
  prefetchConversationStateToGpu(const NativeConversationStateHandleV1& state) final
  {
    ++counters.prefetch;
    return std::async(
      std::launch::async,
      [this, state] {
        std::lock_guard<std::mutex> lock(mutex);
        if (!hostState.count(state.conversationKey)) {
          return false;
        }
        hostState.erase(state.conversationKey);
        conversationState[state.conversationKey] = true;
        return true;
      });
  }

  bool
  releaseConversationState(const NativeConversationStateHandleV1& state) final
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto removed = conversationState.erase(state.conversationKey) +
      hostState.erase(state.conversationKey);
    ++counters.release;
    return removed != 0;
  }

  std::atomic<bool> restoredWithoutBundle{false};
  std::string restoredSession;

private:
  NativeConversationStateHandleV1
  makeHandle(const std::string& sessionId, const std::string& conversationKey) const
  {
    NativeConversationStateHandleV1 handle;
    handle.opaque.providerIdentity = "/provider/A";
    handle.opaque.providerBootId = "boot-a";
    handle.opaque.sessionId = sessionId;
    handle.opaque.role = "/Stage/0";
    handle.opaque.token = "ndnsf-device-state-v1:" + sessionId;
    handle.conversationKey = conversationKey;
    handle.logicalBytes = sizeof(std::int64_t);
    handle.validate();
    return handle;
  }

  mutable std::mutex mutex;
  std::map<std::string, bool> sessionState;
  std::map<std::string, bool> conversationState;
  std::map<std::string, bool> hostState;
};

} // namespace

BOOST_AUTO_TEST_CASE(AsyncDataflowRuntimeRunsStageShardsInParallelAndBatchesMergeInputs)
{
  const std::vector<RoleSpec> roles = {
    RoleSpec{
      "/Stage/0/Shard/0",
      {},
      {DependencyEdge{"stage0-shard0-to-merge", "/Stage/0/Shard/0", "/Merge",
                      "/run/1/stage0/shard0/bundle/0", 1}},
    },
    RoleSpec{
      "/Stage/0/Shard/1",
      {},
      {DependencyEdge{"stage0-shard1-to-merge", "/Stage/0/Shard/1", "/Merge",
                      "/run/1/stage0/shard1/bundle/0", 1}},
    },
    RoleSpec{
      "/Merge",
      {DependencyEdge{"stage0-shard0-to-merge", "/Stage/0/Shard/0", "/Merge",
                      "/run/1/stage0/shard0/bundle/0", 1},
       DependencyEdge{"stage0-shard1-to-merge", "/Stage/0/Shard/1", "/Merge",
                      "/run/1/stage0/shard1/bundle/0", 1}},
      {DependencyEdge{"merge-to-user", "/Merge", "",
                      "/run/1/merge/result/bundle/0", 1}},
    },
  };

  std::mutex observedMutex;
  std::set<std::string> mergeInputScopes;

  AsyncDataflowRuntime runtime(2);
  const auto started = std::chrono::steady_clock::now();
  const auto result = runtime.run(
    "run-1",
    roles,
    {},
    [&] (const RoleExecutionContext& ctx) {
      if (ctx.role == "/Stage/0/Shard/0") {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return std::map<std::string, TensorBundle>{
          {"stage0-shard0-to-merge", bundle("s0", "left")},
        };
      }
      if (ctx.role == "/Stage/0/Shard/1") {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return std::map<std::string, TensorBundle>{
          {"stage0-shard1-to-merge", bundle("s1", "right")},
        };
      }

      BOOST_CHECK_EQUAL(ctx.role, "/Merge");
      BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 2);
      {
        std::lock_guard<std::mutex> lock(observedMutex);
        for (const auto& item : ctx.inputsByScope) {
          mergeInputScopes.insert(item.first);
        }
      }
      const auto merged =
        payloadText(ctx.inputsByScope.at("stage0-shard0-to-merge")) + "+" +
        payloadText(ctx.inputsByScope.at("stage0-shard1-to-merge"));
      return std::map<std::string, TensorBundle>{
        {"merge-to-user", bundle("result", merged)},
      };
    });
  const auto elapsed = durationMs(started, std::chrono::steady_clock::now());

  BOOST_REQUIRE(result.outputsByScope.count("merge-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("merge-to-user")), "left+right");
  BOOST_CHECK_LT(elapsed, 155.0);
  BOOST_REQUIRE_EQUAL(result.roleTimings.size(), 3);

  std::lock_guard<std::mutex> lock(observedMutex);
  BOOST_CHECK(mergeInputScopes.count("stage0-shard0-to-merge") == 1);
  BOOST_CHECK(mergeInputScopes.count("stage0-shard1-to-merge") == 1);
}

BOOST_AUTO_TEST_CASE(AsyncDataflowRuntimeRunsStageFrontierHeadsInParallelBeforeMerge)
{
  const std::vector<RoleSpec> roles = {
    RoleSpec{
      "/Backbone",
      {},
      {DependencyEdge{"backbone-to-head", "/Backbone", "/Head/0",
                      "/run/frontier/backbone/bundle/0", 1, 12000},
       DependencyEdge{"backbone-to-head", "/Backbone", "/Head/1",
                      "/run/frontier/backbone/bundle/0", 1, 12000}},
    },
    RoleSpec{
      "/Head/0",
      {DependencyEdge{"backbone-to-head", "/Backbone", "/Head/0",
                      "/run/frontier/backbone/bundle/0", 1, 12000}},
      {DependencyEdge{"head0-to-merge", "/Head/0", "/Merge",
                      "/run/frontier/head0/bundle/0", 1, 6000}},
    },
    RoleSpec{
      "/Head/1",
      {DependencyEdge{"backbone-to-head", "/Backbone", "/Head/1",
                      "/run/frontier/backbone/bundle/0", 1, 12000}},
      {DependencyEdge{"head1-to-merge", "/Head/1", "/Merge",
                      "/run/frontier/head1/bundle/0", 1, 6000}},
    },
    RoleSpec{
      "/Merge",
      {DependencyEdge{"head0-to-merge", "/Head/0", "/Merge",
                      "/run/frontier/head0/bundle/0", 1, 6000},
       DependencyEdge{"head1-to-merge", "/Head/1", "/Merge",
                      "/run/frontier/head1/bundle/0", 1, 6000}},
      {DependencyEdge{"merge-to-user", "/Merge", "",
                      "/run/frontier/merge/bundle/0", 1, 3000}},
    },
  };

  AsyncDataflowRuntime runtime(4);
  const auto started = std::chrono::steady_clock::now();
  const auto result = runtime.run(
    "frontier-run",
    roles,
    {},
    [] (const RoleExecutionContext& ctx) {
      if (ctx.role == "/Backbone") {
        BOOST_CHECK(ctx.inputsByScope.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::map<std::string, TensorBundle>{
          {"backbone-to-head", bundle("backbone", "features")},
        };
      }
      if (ctx.role == "/Head/0" || ctx.role == "/Head/1") {
        BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
        BOOST_CHECK_EQUAL(payloadText(ctx.inputsByScope.at("backbone-to-head")),
                          "features");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto scope = ctx.role == "/Head/0" ? "head0-to-merge" : "head1-to-merge";
        const auto value = ctx.role == "/Head/0" ? "h0" : "h1";
        return std::map<std::string, TensorBundle>{
          {scope, bundle(scope, value)},
        };
      }

      BOOST_CHECK_EQUAL(ctx.role, "/Merge");
      BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 2);
      const auto merged =
        payloadText(ctx.inputsByScope.at("head0-to-merge")) + "+" +
        payloadText(ctx.inputsByScope.at("head1-to-merge"));
      return std::map<std::string, TensorBundle>{
        {"merge-to-user", bundle("result", merged)},
      };
    });
  const auto elapsed = durationMs(started, std::chrono::steady_clock::now());

  BOOST_REQUIRE(result.outputsByScope.count("merge-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("merge-to-user")), "h0+h1");
  BOOST_CHECK_LT(elapsed, 170.0);

  std::map<std::string, RoleTiming> timingByRole;
  for (const auto& timing : result.roleTimings) {
    timingByRole.emplace(timing.role, timing);
  }
  BOOST_REQUIRE(timingByRole.count("/Backbone") == 1);
  BOOST_REQUIRE(timingByRole.count("/Head/0") == 1);
  BOOST_REQUIRE(timingByRole.count("/Head/1") == 1);
  BOOST_REQUIRE(timingByRole.count("/Merge") == 1);

  const auto& backbone = timingByRole.at("/Backbone");
  const auto& head0 = timingByRole.at("/Head/0");
  const auto& head1 = timingByRole.at("/Head/1");
  const auto& merge = timingByRole.at("/Merge");

  BOOST_CHECK_GE(durationMs(backbone.finishedAt, head0.startedAt), 0.0);
  BOOST_CHECK_GE(durationMs(backbone.finishedAt, head1.startedAt), 0.0);
  BOOST_CHECK_LT(durationMs(head0.startedAt, head1.finishedAt), 120.0);
  BOOST_CHECK_LT(durationMs(head1.startedAt, head0.finishedAt), 120.0);
  BOOST_CHECK_GE(durationMs(head0.finishedAt, merge.startedAt), 0.0);
  BOOST_CHECK_GE(durationMs(head1.finishedAt, merge.startedAt), 0.0);
}

BOOST_AUTO_TEST_CASE(AsyncDataflowRuntimeRejectsMissingDeclaredOutput)
{
  const std::vector<RoleSpec> roles = {
    RoleSpec{
      "/Role/A",
      {},
      {DependencyEdge{"a-to-user", "/Role/A", "", "/run/2/a/bundle/0", 1}},
    },
  };

  AsyncDataflowRuntime runtime(1);
  BOOST_CHECK_THROW(
    runtime.run("run-2", roles, {}, [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    }),
    std::logic_error);
}

BOOST_AUTO_TEST_CASE(AsyncDataflowRuntimeRejectsIncompleteV3InputBeforeRunner)
{
  DependencyEdge input{"v3-input", "/Producer", "/Consumer",
                       "/provider/NDNSF-DI/TENSOR/v1/object", 0, 0};
  input.declaredByV3 = true;
  input.maxSegments = 4;
  const std::vector<RoleSpec> roles = {
    RoleSpec{"/Consumer", {input}, {}},
  };
  TensorBundle incomplete = bundle("v3-input", "partial");
  bool runnerCalled = false;
  AsyncDataflowRuntime runtime(1);
  BOOST_CHECK_THROW(
    runtime.run("v3-incomplete", roles, {{"v3-input", incomplete}},
                [&] (const RoleExecutionContext&) {
                  runnerCalled = true;
                  return std::map<std::string, TensorBundle>{};
                }),
    std::runtime_error);
  BOOST_CHECK(!runnerCalled);
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPrefetchesAllInputsBeforeRunningRole)
{
  RoleSpec role{
    "/Merge",
    {DependencyEdge{"head0-to-merge", "/Head/0", "/Merge",
                    "/run/3/head0/bundle/0", 2, 14000},
     DependencyEdge{"head1-to-merge", "/Head/1", "/Merge",
                    "/run/3/head1/bundle/0", 1, 7000}},
    {DependencyEdge{"merge-to-user", "/Merge", "",
                    "/run/3/merge/bundle/0", 1, 4000}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(2);

  const auto started = std::chrono::steady_clock::now();
  auto future = worker.executeAsync(
    "run-3",
    role,
    io,
    [] (const RoleExecutionContext& ctx) {
      BOOST_CHECK_EQUAL(ctx.role, "/Merge");
      BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 2);
      const auto merged =
        payloadText(ctx.inputsByScope.at("head0-to-merge")) + "|" +
        payloadText(ctx.inputsByScope.at("head1-to-merge"));
      return std::map<std::string, TensorBundle>{
        {"merge-to-user", bundle("result", merged)},
      };
    });

  const auto result = future.get();
  const auto elapsed = durationMs(started, std::chrono::steady_clock::now());

  BOOST_CHECK_LT(elapsed, 150.0);
  BOOST_REQUIRE_EQUAL(result.inputTimings.size(), 2);
  BOOST_CHECK_EQUAL(result.inputTimings[0].plannedDataName, "/run/3/head0/bundle/0");
  BOOST_REQUIRE_EQUAL(result.inputTimings[0].plannedSegmentNames.size(), 2);
  BOOST_CHECK_EQUAL(result.inputTimings[0].plannedSegmentNames[0],
                    plannedSegmentName("/run/3/head0/bundle/0", 0));
  BOOST_CHECK_EQUAL(result.inputTimings[0].plannedSegmentNames[1],
                    plannedSegmentName("/run/3/head0/bundle/0", 1));
  BOOST_CHECK_EQUAL(result.inputTimings[1].expectedSegments, 1);
  BOOST_CHECK_EQUAL(result.inputTimings[1].expectedBytes, 7000);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("merge-to-user")),
                    "input:head0-to-merge|input:head1-to-merge");

  std::lock_guard<std::mutex> lock(io->mutex);
  BOOST_REQUIRE_EQUAL(io->prefetchedScopes.size(), 2);
  BOOST_CHECK_EQUAL(io->prefetchedScopes[0], "head0-to-merge");
  BOOST_CHECK_EQUAL(io->prefetchedScopes[1], "head1-to-merge");
  BOOST_REQUIRE(io->publishedByScope.count("merge-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(io->publishedByScope.at("merge-to-user")),
                    "input:head0-to-merge|input:head1-to-merge");
  BOOST_REQUIRE_EQUAL(result.outputTimings.size(), 1);
  BOOST_REQUIRE_EQUAL(result.outputTimings[0].plannedSegmentNames.size(), 1);
  BOOST_CHECK_EQUAL(result.outputTimings[0].plannedSegmentNames[0],
                    plannedSegmentName("/run/3/merge/bundle/0", 0));
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerRejectsIncompleteV3FetchBeforeRunner)
{
  DependencyEdge input{"v3-input", "/Producer", "/Consumer",
                       "/provider/NDNSF-DI/TENSOR/v1/object", 0, 0};
  input.declaredByV3 = true;
  input.maxSegments = 4;
  RoleSpec role{"/Consumer", {input}, {}};
  auto io = std::make_shared<ImmediateDependencyIo>();
  ProviderRoleWorker worker(1);
  bool runnerCalled = false;
  auto future = worker.executeAsync(
    "v3-incomplete-worker", role, io,
    [&] (const RoleExecutionContext&) {
      runnerCalled = true;
      return std::map<std::string, TensorBundle>{};
    });
  BOOST_CHECK_THROW(future.get(), std::runtime_error);
  BOOST_CHECK(!runnerCalled);
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerStagesAllOutputsBeforePublishingAny)
{
  RoleSpec role{
    "/Producer", {},
    {DependencyEdge{"first", "/Producer", "/First", "/first", 1},
     DependencyEdge{"second", "/Producer", "/Second", "/second", 1}},
  };
  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);
  auto future = worker.executeAsync(
    "atomic-output-stage", role, io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"first", bundle("first", "would-have-been-partial")},
        {"debug", bundle("debug", "not-the-second-output")},
      };
    });
  BOOST_CHECK_THROW(future.get(), std::logic_error);
  std::lock_guard<std::mutex> lock(io->mutex);
  BOOST_CHECK(io->publishedByScope.empty());
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerDoesNotOccupyComputeWorkerWhileWaitingForInputs)
{
  RoleSpec consumer{
    "/Consumer",
    {DependencyEdge{"producer-to-consumer", "/Producer", "/Consumer",
                    "/run/ready/producer/bundle/0", 1}},
    {DependencyEdge{"consumer-to-user", "/Consumer", "",
                    "/run/ready/consumer/bundle/0", 1}},
  };
  RoleSpec producer{
    "/Producer",
    {},
    {DependencyEdge{"producer-to-consumer", "/Producer", "/Consumer",
                    "/run/ready/producer/bundle/0", 1}},
  };

  auto io = std::make_shared<BlockingDependencyIo>();
  ProviderRoleWorker worker(1);

  auto consumerFuture = worker.executeAsync(
    "ready-run",
    consumer,
    io,
    [] (const RoleExecutionContext& ctx) {
      BOOST_REQUIRE(ctx.inputsByScope.count("producer-to-consumer") == 1);
      return std::map<std::string, TensorBundle>{
        {"consumer-to-user", bundle("consumer-to-user",
                                    "consumer:" +
                                    payloadText(ctx.inputsByScope.at("producer-to-consumer")))},
      };
    });

  auto producerFuture = worker.executeAsync(
    "ready-run",
    producer,
    io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"producer-to-consumer", bundle("producer-to-consumer", "producer-output")},
      };
    });

  BOOST_REQUIRE(producerFuture.wait_for(std::chrono::milliseconds(300)) ==
                std::future_status::ready);
  BOOST_REQUIRE(consumerFuture.wait_for(std::chrono::milliseconds(300)) ==
                std::future_status::ready);

  const auto producerResult = producerFuture.get();
  const auto consumerResult = consumerFuture.get();

  BOOST_REQUIRE(producerResult.outputsByScope.count("producer-to-consumer") == 1);
  BOOST_REQUIRE(consumerResult.outputsByScope.count("consumer-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(consumerResult.outputsByScope.at("consumer-to-user")),
                    "consumer:producer-output");
  BOOST_CHECK_GE(durationMs(consumerResult.inputTimings[0].prefetchStartedAt,
                            consumerResult.inputTimings[0].fetchCompletedAt),
                 0.0);
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerEnqueuesImmediatelyWhenInputsAreReady)
{
  RoleSpec role{
    "/ReadyInput",
    {DependencyEdge{"source-to-ready", "/Source", "/ReadyInput",
                    "/run/immediate/source/bundle/0", 1}},
    {DependencyEdge{"ready-to-user", "/ReadyInput", "",
                    "/run/immediate/ready/bundle/0", 1}},
  };

  auto io = std::make_shared<ImmediateDependencyIo>();
  ProviderRoleWorker worker(1);

  auto future = worker.executeAsync(
    "immediate-run",
    role,
    io,
    [] (const RoleExecutionContext& ctx) {
      BOOST_REQUIRE(ctx.inputsByScope.count("source-to-ready") == 1);
      return std::map<std::string, TensorBundle>{
        {"ready-to-user", bundle("ready-to-user",
                                 "ready:" +
                                 payloadText(ctx.inputsByScope.at("source-to-ready")))},
      };
    });

  const auto snapshot = worker.snapshot();
  BOOST_CHECK_EQUAL(snapshot.waitingForInputCount, 0);

  BOOST_REQUIRE(future.wait_for(std::chrono::milliseconds(200)) ==
                std::future_status::ready);
  const auto result = future.get();
  BOOST_REQUIRE_EQUAL(result.inputTimings.size(), 1);
  BOOST_CHECK_EQUAL(result.inputTimings[0].scope, "source-to-ready");
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("ready-to-user")),
                    "ready:immediate:source-to-ready");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerAcceptsNativeModelRunnerObject)
{
  RoleSpec role{
    "/NativeRole",
    {DependencyEdge{"input-to-native", "/Input", "/NativeRole",
                    "/run/4/input/bundle/0", 1}},
    {DependencyEdge{"native-to-user", "/NativeRole", "",
                    "/run/4/native/bundle/0", 1}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  auto runner = std::make_shared<EchoNativeRunner>();
  ProviderRoleWorker worker(1);

  const auto result = worker.executeAsync("run-4", role, io, runner).get();

  BOOST_REQUIRE(result.outputsByScope.count("native-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("native-to-user")),
                    "native:input:input-to-native");

  std::lock_guard<std::mutex> lock(io->mutex);
  BOOST_REQUIRE(io->publishedByScope.count("native-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(io->publishedByScope.at("native-to-user")),
                    "native:input:input-to-native");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPassesStreamEventSinkToNativeRunner)
{
  RoleSpec role{
    "/Terminal",
    {},
    {},
  };
  auto io = std::make_shared<FakeDependencyIo>();
  auto runner = std::make_shared<StreamEventNativeRunner>();
  std::vector<std::string> events;
  std::mutex eventsMutex;
  ProviderRoleWorker worker(1);
  auto sink = [&] (const std::vector<std::uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(eventsMutex);
    events.emplace_back(payload.begin(), payload.end());
    return true;
  };

  const auto result = worker.executeAsync(
    "run-stream-sink", role, io, runner, {}, std::move(sink)).get();
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("terminal")), "result");
  std::lock_guard<std::mutex> lock(eventsMutex);
  BOOST_REQUIRE_EQUAL(events.size(), 1);
  BOOST_CHECK_EQUAL(events.front(), "token-1");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerDoesNotCacheStreamEventSideEffects)
{
  RoleSpec role;
  role.role = "/LLM/Pipeline/Stage/0";

  auto io = std::make_shared<FakeDependencyIo>();
  std::atomic<int> runCount{0};
  std::atomic<int> eventCount{0};
  ProviderRoleWorker worker(1);
  auto runner = makeNativeModelRunner(
    [&runCount] (const RoleExecutionContext& ctx) {
      ++runCount;
      BOOST_REQUIRE(static_cast<bool>(ctx.streamEventSink));
      BOOST_REQUIRE(ctx.streamEventSink(
        std::vector<std::uint8_t>{'e', 'v', 'e', 'n', 't'}));
      return std::map<std::string, TensorBundle>{
        {"final-response", bundle("final-response", "result")},
      };
    });
  auto eventSink = [&eventCount] (const std::vector<std::uint8_t>& payload) {
    if (payload != std::vector<std::uint8_t>{'e', 'v', 'e', 'n', 't'}) {
      return false;
    }
    ++eventCount;
    return true;
  };

  const auto first = worker.executeAsync(
    "stream-request-1", role, io, runner, {}, eventSink).get();
  const auto second = worker.executeAsync(
    "stream-request-2", role, io, runner, {}, eventSink).get();

  BOOST_CHECK_EQUAL(runCount.load(), 2);
  BOOST_CHECK_EQUAL(eventCount.load(), 2);
  BOOST_CHECK(!first.exactForwardCacheHit);
  BOOST_CHECK(!second.exactForwardCacheHit);
  BOOST_CHECK_EQUAL(payloadText(second.outputsByScope.at("final-response")),
                    "result");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerUsesIncrementalRunnerWhenAvailable)
{
  RoleSpec role;
  role.role = "/LLM/Pipeline/Stage/0";
  auto io = std::make_shared<FakeDependencyIo>();
  std::atomic<int> oneShotRuns{0};
  std::atomic<int> streamedRuns{0};
  auto runner = std::make_shared<IncrementalTestRunner>(
    oneShotRuns, streamedRuns);
  ProviderRoleWorker worker(1);
  auto sink = [] (const std::vector<std::uint8_t>& payload) {
    return payload == std::vector<std::uint8_t>{'s', 't', 'r', 'e', 'a', 'm'};
  };
  const auto result = worker.executeAsync(
    "stream-request-incremental", role, io, runner, {}, sink).get();
  BOOST_CHECK_EQUAL(oneShotRuns.load(), 0);
  BOOST_CHECK_EQUAL(streamedRuns.load(), 1);
  BOOST_CHECK(!result.exactForwardCacheHit);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("final-response")),
                    "incremental-result");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerUsesOneShotRunnerForCoordinatorEpoch)
{
  RoleSpec role;
  role.role = "/LLM/Pipeline/Stage/0";
  role.requestId = "coordinator-request";
  role.attemptEpoch = 1;
  GenerationEpochLineageV1 lineage;
  lineage.requestId = role.requestId;
  lineage.attemptEpoch = role.attemptEpoch;
  lineage.inferenceEpoch = 0;
  role.generationLineage = lineage;

  auto io = std::make_shared<FakeDependencyIo>();
  std::atomic<int> oneShotRuns{0};
  std::atomic<int> streamedRuns{0};
  auto runner = std::make_shared<IncrementalTestRunner>(
    oneShotRuns, streamedRuns);
  ProviderRoleWorker worker(1);
  auto sink = [] (const std::vector<std::uint8_t>&) {
    return true;
  };

  const auto result = worker.executeAsync(
    "coordinator-epoch", role, io, runner, {}, sink).get();
  BOOST_CHECK_EQUAL(oneShotRuns.load(), 1);
  BOOST_CHECK_EQUAL(streamedRuns.load(), 0);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("final-response")),
                    "one-shot-result");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPassesInitialInputsToSourceRole)
{
  RoleSpec role{
    "/Source",
    {},
    {DependencyEdge{"source-to-next", "/Source", "/Next",
                    "/run/source/output/bundle/0", 1}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);
  std::map<std::string, TensorBundle> initialInputs;
  initialInputs.emplace("images", bundle("images", "image-bytes"));

  const auto result = worker.executeAsync(
    "initial-input-run",
    role,
    io,
    [] (const RoleExecutionContext& ctx) {
      BOOST_REQUIRE(ctx.inputsByScope.count("images") == 1);
      return std::map<std::string, TensorBundle>{
        {"source-to-next", bundle("source-to-next",
                                  "features:" + payloadText(ctx.inputsByScope.at("images")))},
      };
    },
    std::move(initialInputs)).get();

  BOOST_REQUIRE(result.outputsByScope.count("source-to-next") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("source-to-next")),
                    "features:image-bytes");
  std::lock_guard<std::mutex> lock(io->mutex);
  BOOST_REQUIRE(io->publishedByScope.count("source-to-next") == 1);
  BOOST_CHECK_EQUAL(payloadText(io->publishedByScope.at("source-to-next")),
                    "features:image-bytes");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPreservesFinalResponseBundle)
{
  RoleSpec role{
    "/Merge",
    {},
    {},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);

  const auto result = worker.executeAsync(
    "final-response-run",
    role,
    io,
    [] (const RoleExecutionContext& ctx) {
      BOOST_CHECK_EQUAL(ctx.role, "/Merge");
      return std::map<std::string, TensorBundle>{
        {"final-response", bundle("final-response", "predictions")},
      };
    }).get();

  BOOST_REQUIRE(result.outputsByScope.count("final-response") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("final-response")),
                    "predictions");

  std::lock_guard<std::mutex> lock(io->mutex);
  BOOST_CHECK(io->publishedByScope.empty());
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerSnapshotReportsActiveAndQueuedWork)
{
  RoleSpec role{
    "/SlowRole",
    {},
    {},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);
  std::promise<void> started;
  std::promise<void> releasePromise;
  auto release = releasePromise.get_future().share();

  auto first = worker.executeAsync(
    "snapshot-first",
    role,
    io,
    [&] (const RoleExecutionContext&) {
      started.set_value();
      release.wait();
      return std::map<std::string, TensorBundle>{
        {"final-response", bundle("first", "ok")},
      };
    });
  started.get_future().wait();

  auto second = worker.executeAsync(
    "snapshot-second",
    role,
    io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"final-response", bundle("second", "ok")},
      };
    });

  auto snapshot = worker.snapshot();
  BOOST_CHECK_EQUAL(snapshot.workerCount, 1);
  BOOST_CHECK_EQUAL(snapshot.activeWorkerCount, 1);
  BOOST_CHECK_GE(snapshot.readyQueueDepth, 1);
  BOOST_CHECK_EQUAL(snapshot.readyQueueCapacity, 1024);
  BOOST_CHECK_GE(snapshot.pendingWorkCount(), 2);
  BOOST_CHECK_EQUAL(snapshot.idleWorkerCount(), 0);

  releasePromise.set_value();
  BOOST_CHECK_EQUAL(payloadText(first.get().outputsByScope.at("final-response")), "ok");
  BOOST_CHECK_EQUAL(payloadText(second.get().outputsByScope.at("final-response")), "ok");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerReadyQueueIsBounded)
{
  RoleSpec role{"/SlowRole", {}, {}};
  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(
    1, 4, 1024, std::chrono::seconds(120), 1);
  std::promise<void> started;
  std::promise<void> releasePromise;
  auto release = releasePromise.get_future().share();

  auto first = worker.executeAsync(
    "bounded-first", role, io,
    [&] (const RoleExecutionContext&) {
      started.set_value();
      release.wait();
      return std::map<std::string, TensorBundle>{};
    });
  started.get_future().wait();
  auto second = worker.executeAsync(
    "bounded-second", role, io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    });
  BOOST_REQUIRE_EQUAL(worker.snapshot().readyQueueDepth, 1);
  auto rejected = worker.executeAsync(
    "bounded-rejected", role, io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    });
  BOOST_REQUIRE(rejected.wait_for(std::chrono::milliseconds(100)) ==
                std::future_status::ready);
  BOOST_CHECK_THROW(rejected.get(), std::runtime_error);

  releasePromise.set_value();
  BOOST_CHECK_NO_THROW(first.get());
  BOOST_CHECK_NO_THROW(second.get());
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPreparesRunnerOnlyAfterBoundedQueueAdmission)
{
  RoleSpec role{"/PreparedRole", {}, {}};
  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(
    1, 4, 1024, std::chrono::seconds(120), 1);
  std::promise<void> started;
  std::promise<void> releasePromise;
  auto release = releasePromise.get_future().share();

  auto blocking = worker.executeAsync(
    "prepared-blocking", role, io,
    [&] (const RoleExecutionContext&) {
      started.set_value();
      release.wait();
      return std::map<std::string, TensorBundle>{};
    });
  started.get_future().wait();

  std::atomic<std::size_t> preparationCalls{0};
  auto prepared = worker.executePreparedAsync(
    "prepared-after-selection", role, io,
    [&] {
      ++preparationCalls;
      return makeNativeModelRunner(
        [] (const RoleExecutionContext&) {
          return std::map<std::string, TensorBundle>{
            {"final-response", bundle("prepared", "ok")},
          };
        });
    });
  BOOST_CHECK_EQUAL(preparationCalls.load(), 0);
  BOOST_CHECK_EQUAL(worker.snapshot().readyQueueDepth, 1);

  releasePromise.set_value();
  BOOST_CHECK_NO_THROW(blocking.get());
  const auto result = prepared.get();
  BOOST_CHECK_EQUAL(preparationCalls.load(), 1);
  BOOST_CHECK_EQUAL(
    payloadText(result.outputsByScope.at("final-response")), "ok");
}

BOOST_AUTO_TEST_CASE(NativePreparedRunnerSpecBindsExactSealedAssembly)
{
  const auto digest = [] (char value) {
    return "sha256:" + std::string(64, value);
  };
  NativeSelectionProjectionV3 projection;
  projection.canonicalArtifactName = "/repo/canonical/root/v1";
  projection.assembly.selectedRole = "/Stage0";
  projection.assembly.backend = "onnxruntime-cpu";
  projection.assembly.artifactDigest = digest('a');
  projection.assembly.recipeDigest = digest('b');
  projection.assembly.modelManifestDigest = digest('c');
  projection.assembly.artifactProfileDigest = digest('d');
  projection.assembly.graphDigest = digest('e');
  projection.assembly.canonicalInitializerDigest = digest('2');
  projection.assembly.adapterDescriptorDigest = digest('f');
  projection.assembly.assemblerDescriptorDigest = digest('1');
  projection.assembly.backendAbi = "onnxruntime-1.26-cpu";
  projection.assembly.nodeIndices = {0, 1};
  projection.assembly.expectedInputs = {{"x", "float32", {"1", "4"}}};
  projection.assembly.expectedOutputs = {{"y", "float32", {"1", "4"}}};
  projection.assembly.precision = "fp32";
  projection.assembly.quantization = "none";
  projection.assembly.layout = "native";
  projection.assembly.padding = "none";
  projection.assembly.maxSourceBytes = 4096;
  projection.assembly.maxAssembledBytes = 2048;
  projection.assembly.maxNodes = 2;

  NativeModelRunnerSpec spec;
  spec.role = "/Stage0";
  spec.backend = "onnxruntime-cpu";
  spec.path = "/var/tmp/ndnsf/assembled/model.onnx";
  spec.metadata = {
    {"fragmentDigest", projection.assembly.artifactDigest},
    {"recipeDigest", projection.assembly.recipeDigest},
    {"modelManifestDigest", projection.assembly.modelManifestDigest},
    {"artifactProfileDigest", projection.assembly.artifactProfileDigest},
    {"graphDigest", projection.assembly.graphDigest},
    {"canonicalInitializerDigest",
     projection.assembly.canonicalInitializerDigest},
    {"adapterDescriptorDigest", projection.assembly.adapterDescriptorDigest},
    {"assemblerDescriptorDigest", projection.assembly.assemblerDescriptorDigest},
    {"backendAbi", projection.assembly.backendAbi},
    {"precision", projection.assembly.precision},
    {"quantization", projection.assembly.quantization},
    {"layout", projection.assembly.layout},
    {"padding", projection.assembly.padding},
    {"maxSourceBytes", std::to_string(projection.assembly.maxSourceBytes)},
    {"maxAssembledBytes",
     std::to_string(projection.assembly.maxAssembledBytes)},
    {"maxNodes", std::to_string(projection.assembly.maxNodes)},
  };
  BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, spec));

  auto missingRoot = projection;
  missingRoot.canonicalArtifactName.clear();
  BOOST_REQUIRE(validateNativePreparedRunnerSpec(missingRoot, spec));
  BOOST_CHECK_EQUAL(*validateNativePreparedRunnerSpec(missingRoot, spec),
                    "DI_PROVIDER_ASSEMBLY_ROOT_MISSING");

  auto wrongPath = spec;
  wrongPath.path = "../model.onnx";
  BOOST_REQUIRE(validateNativePreparedRunnerSpec(projection, wrongPath));
  BOOST_CHECK_EQUAL(*validateNativePreparedRunnerSpec(projection, wrongPath),
                    "DI_PROVIDER_ASSEMBLY_PATH_UNSAFE");

  auto wrongAdapter = spec;
  wrongAdapter.metadata["adapterDescriptorDigest"] = digest('9');
  BOOST_REQUIRE(validateNativePreparedRunnerSpec(projection, wrongAdapter));
  BOOST_CHECK_EQUAL(*validateNativePreparedRunnerSpec(projection, wrongAdapter),
                    "DI_PROVIDER_ASSEMBLY_METADATA_MISMATCH");
}

BOOST_AUTO_TEST_CASE(NativeProviderHandlerRejectsMissingRunnerFactory)
{
  NativeProviderHandlerConfig config;
  BOOST_CHECK_THROW(makeNativeProviderCollaborationHandler(std::move(config)),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeProviderTimeoutBudgetKeepsDataAndControlDeadlinesIndependent)
{
  NativeProviderHandlerConfig config;
  config.dependencyFetchTimeoutMs = 900000;
  config.fetchTimeoutMs = 5000;
  const auto budget = nativeProviderTimeoutBudget(config);
  BOOST_CHECK_EQUAL(budget.dependencyFetchMs, 900000);
  BOOST_CHECK_EQUAL(budget.readinessMs, 5000);
  BOOST_CHECK_EQUAL(budget.conversationControlMs, 5000);
}

BOOST_AUTO_TEST_CASE(NativeProviderTimeoutEnvironmentOverrideStaysOnDependencyFetch)
{
  NativeProviderHandlerConfig config;
  config.dependencyFetchTimeoutMs = 900000;
  config.fetchTimeoutMs = 5000;
  const char* previous = std::getenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS");
  const std::string previousValue = previous == nullptr ? std::string() : previous;
  const bool hadPrevious = previous != nullptr;
  BOOST_REQUIRE_EQUAL(setenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS", "12000", 1), 0);
  const auto budget = nativeProviderTimeoutBudget(config);
  if (hadPrevious) {
    BOOST_REQUIRE_EQUAL(setenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS",
                              previousValue.c_str(), 1), 0);
  }
  else {
    BOOST_REQUIRE_EQUAL(unsetenv("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS"), 0);
  }
  BOOST_CHECK_EQUAL(budget.dependencyFetchMs, 12000);
  BOOST_CHECK_EQUAL(budget.readinessMs, 5000);
  BOOST_CHECK_EQUAL(budget.conversationControlMs, 5000);
}

BOOST_AUTO_TEST_CASE(NativeProviderExecutionPolicyRejectsMixedFallback)
{
  NativeProviderHandlerConfig dataDriven;
  BOOST_CHECK_NO_THROW(validateNativeProviderExecutionPolicy(dataDriven));

  dataDriven.requireExecutionActivation = true;
  BOOST_CHECK_THROW(validateNativeProviderExecutionPolicy(dataDriven),
                    std::invalid_argument);

  NativeProviderHandlerConfig legacy;
  legacy.executionPolicy = "LEGACY_READY_SET_V1";
  BOOST_CHECK_THROW(validateNativeProviderExecutionPolicy(legacy),
                    std::invalid_argument);
  legacy.requireExecutionActivation = true;
  legacy.allowLegacyPeerReadinessBarrier = true;
  BOOST_CHECK_THROW(validateNativeProviderExecutionPolicy(legacy),
                    std::invalid_argument);
  legacy.plan.executionPolicy = "LEGACY_READY_SET_V1";
  BOOST_CHECK_NO_THROW(validateNativeProviderExecutionPolicy(legacy));

  NativeProviderHandlerConfig unknown;
  unknown.executionPolicy = "AUTOMATIC_FALLBACK";
  BOOST_CHECK_THROW(validateNativeProviderExecutionPolicy(unknown),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeProviderHandlerExtractsOnlyFinalRoleResponse)
{
  RoleSpec finalRole{
    "/Merge",
    {},
    {},
  };
  ProviderRoleResult finalResult;
  finalResult.outputsByScope.emplace(
    "final-response",
    bundle("final-response", "predictions"));

  const auto finalPayload = nativeProviderFinalResponsePayload(
    finalRole,
    finalResult,
    "final-response");
  BOOST_REQUIRE(finalPayload.has_value());
  BOOST_CHECK_EQUAL(std::string(finalPayload->begin(), finalPayload->end()),
                    "predictions");

  RoleSpec intermediateRole{
    "/Backbone",
    {},
    {DependencyEdge{"backbone-to-head", "/Backbone", "/Head/Shard/0", "", 0, 0}},
  };
  const auto intermediatePayload = nativeProviderFinalResponsePayload(
    intermediateRole,
    finalResult,
    "final-response");
  BOOST_CHECK(!intermediatePayload.has_value());

  ProviderRoleResult missingFinalResult;
  missingFinalResult.outputsByScope.emplace(
    "merge-debug",
    bundle("merge-debug", "not-final"));
  const auto missingPayload = nativeProviderFinalResponsePayload(
    finalRole,
    missingFinalResult,
    "final-response");
  BOOST_CHECK(!missingPayload.has_value());

  const auto disabledPayload = nativeProviderFinalResponsePayload(
    finalRole,
    finalResult,
    "");
  BOOST_CHECK(!disabledPayload.has_value());
}

BOOST_AUTO_TEST_CASE(NativeProviderLocalPlanDoesNotRequireCurrentRoleToBeFinal)
{
  NativeExecutionPlan plan;
  plan.roles = {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"};

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/provider/single";
  }

  // The first callback normally arrives for /Backbone, which has dependency
  // outputs.  Those outputs are precisely why the full local plan must be
  // selected; only the assignment determines locality.
  RoleSpec backbone{
    "/Backbone",
    {},
    {DependencyEdge{"backbone-to-head0", "/Backbone", "/Head/Shard/0", "", 1}},
  };
  BOOST_CHECK(nativeProviderShouldExecuteLocalPlan(
    plan, assignment, backbone, "/provider/single"));

  assignment.providerByRole["/Merge"] = "/provider/other";
  BOOST_CHECK(!nativeProviderShouldExecuteLocalPlan(
    plan, assignment, backbone, "/provider/single"));

  assignment.providerByRole.erase("/Merge");
  BOOST_CHECK(!nativeProviderShouldExecuteLocalPlan(
    plan, assignment, backbone, "/provider/single"));
}

BOOST_AUTO_TEST_CASE(NativeProviderAssignmentPayloadValidatesRoleAndFragment)
{
  NativeModelRunnerSpec mergeSpec;
  mergeSpec.role = "/Merge";
  mergeSpec.backend = "test-backend";
  mergeSpec.metadata["fragmentDigest"] = "sha256:merge";
  const std::vector<NativeModelRunnerSpec> specs{mergeSpec};

  const char* okText = "role=/Merge;fragmentDigest=sha256:merge;";
  const auto okPayload = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(okText),
    std::strlen(okText));
  BOOST_CHECK(!validateNativeProviderAssignmentPayload(specs, "/Merge", okPayload));

  const char* upperDigestText = "role=/Merge;fragmentDigest=SHA256:MERGE;";
  const auto upperDigestPayload = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(upperDigestText),
    std::strlen(upperDigestText));
  BOOST_CHECK(!validateNativeProviderAssignmentPayload(
    specs, "/Merge", upperDigestPayload));

  const char* wrongRoleText = "role=/Backbone;fragmentDigest=sha256:merge;";
  const auto wrongRolePayload = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(wrongRoleText),
    std::strlen(wrongRoleText));
  auto wrongRole =
    validateNativeProviderAssignmentPayload(specs, "/Merge", wrongRolePayload);
  BOOST_REQUIRE(wrongRole);
  BOOST_CHECK_EQUAL(*wrongRole, "DI_BINDING_ROLE_MISMATCH");

  const char* wrongFragmentText = "role=/Merge;fragmentDigest=sha256:other;";
  const auto wrongFragmentPayload = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(wrongFragmentText),
    std::strlen(wrongFragmentText));
  auto wrongFragment =
    validateNativeProviderAssignmentPayload(specs, "/Merge", wrongFragmentPayload);
  BOOST_REQUIRE(wrongFragment);
  BOOST_CHECK_EQUAL(*wrongFragment, "DI_BINDING_FRAGMENT_MISMATCH");

  NativeModelRunnerSpec legacySpec;
  legacySpec.role = "/Merge";
  const std::vector<NativeModelRunnerSpec> legacySpecs{legacySpec};
  BOOST_CHECK(!validateNativeProviderAssignmentPayload(
    legacySpecs,
    "/Merge",
    wrongFragmentPayload));
}

std::string
completeProviderProjectionJson(const std::string& logicalRole,
                               const std::string& roleKey,
                               std::uint64_t rank,
                               const std::string& provider,
                               const std::string& requestId,
                               const std::string& backend,
                               const std::string& device,
                               const std::string& executionBindings = {},
                               bool duplicateRole = false)
{
  const auto d = "sha256:" + std::string(64, 'a');
  const bool cpu = backend == "cpu" ||
                   (backend.size() > 4 &&
                    backend.compare(backend.size() - 4, 4, "-cpu") == 0);
  const auto deviceSet = cpu ? std::string("[]")
                             : std::string("[\"") + device + "\"]";
  const auto role =
    std::string("{\"adapter_id\":\"onnx\",\"adapter_version\":\"1\",") +
    "\"artifact_digest\":\"" + d + "\",\"backend\":\"" + backend +
    "\",\"device_set\":" + deviceSet +
    ",\"layer_begin\":0,\"layer_end\":4,\"rank\":" +
    std::to_string(rank) + ",\"required_device_memory_mb\":0," +
    "\"recipe_digest\":\"" + d +
    "\",\"role\":\"" + logicalRole +
    "\",\"role_kind\":\"PIPELINE_RANGE\"}";
  return std::string("{\"ack_closed_digest\":\"") + d +
    "\",\"assembly\":" + role +
    ",\"attempt\":2,\"dataflow\":{\"attempt\":2," +
    "\"dataflow_digest\":\"" + d +
    "\",\"may_publish\":[],\"must_fetch\":[],\"plan_digest\":\"" + d +
    "\",\"request_id\":\"" + requestId + "\",\"role\":\"" + roleKey +
    "\",\"terminal_response_owner\":true,\"wait_for\":[]}," +
    "\"deadline_ms\":9999,\"dependencies\":[],\"device_binding\":{" +
    "\"mode\":\"" + (cpu ? "CPU" : "SINGLE_DEVICE") +
    "\",\"offer_digest\":\"" + d +
    "\",\"offer_scoped_device_handle\":\"" + (cpu ? "" : device) +
    "\",\"provider\":\"" + provider +
    "\",\"resource_sequence\":1,\"resource_snapshot_digest\":\"" + d +
    "\",\"role\":\"" + roleKey +
    "\",\"sharing_policy\":\"EXCLUSIVE_ROLE\"," +
    "\"topology_profile_digest\":\"" + d + "\"}," +
    (executionBindings.empty()
      ? std::string()
      : "\"execution_bindings\":" + executionBindings + ",") +
    "\"execution_role\":{\"adapter_id\":\"onnx\",\"adapter_version\":\"1\"," +
    "\"backend\":\"" + backend +
    "\",\"layer_begin\":0,\"layer_end\":4,\"rank\":" +
    std::to_string(rank) + ",\"role_id\":\"" + roleKey +
    "\",\"stage_id\":\"" + logicalRole + "\"}," +
    "\"group_capability_v1\":\"aabbcc\",\"offer_digest\":\"" + d +
    "\",\"plan_core_digest\":\"" + d +
    "\",\"plan_digest\":\"" + d + "\",\"provider\":\"" + provider +
    "\",\"request_id\":\"" + requestId + "\",\"roles\":[" + role +
    (duplicateRole ? "," + role : "") +
    "],\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3," +
    "\"security_policy_snapshot_digest\":\"" + d + "\"}";
}

BOOST_AUTO_TEST_CASE(NativeProviderAssignmentParsesCanonicalV3Projection)
{
  const auto text = completeProviderProjectionJson(
    "/Backbone", "/Backbone", 0, "/provider/a", "/request/7",
    "onnxruntime-cpu", "");
  const ndn::Buffer payload(
    reinterpret_cast<const uint8_t*>(text.data()), text.size());

  const auto fields = parseNativeProviderAssignmentFields(payload, "/Backbone");
  BOOST_CHECK_EQUAL(fields.at("provider"), "/provider/a");
  BOOST_CHECK_EQUAL(fields.at("executionRequestId"), "/request/7");
  BOOST_CHECK_EQUAL(fields.at("executionAttemptEpoch"), "2");
  BOOST_CHECK_EQUAL(fields.at("executionPlanDigest"),
                    "sha256:" + std::string(64, 'a'));
  BOOST_CHECK_EQUAL(fields.at("groupCapabilityV1"), "aabbcc");
  BOOST_CHECK_EQUAL(fields.at("role"), "/Backbone");
  BOOST_CHECK_EQUAL(fields.at("rank"), "0");
  BOOST_CHECK_EQUAL(fields.at("backend"), "onnxruntime-cpu");
  BOOST_CHECK_EQUAL(fields.at("device"), "cpu:0");
  BOOST_CHECK_EQUAL(fields.at("artifactDigest"),
                    "sha256:" + std::string(64, 'a'));
  BOOST_CHECK_EQUAL(fields.at("fragmentDigest"),
                    "sha256:" + std::string(64, 'a'));
}

BOOST_AUTO_TEST_CASE(NativeProviderAssignmentRejectsMultiRoleV3Projection)
{
  const auto text = completeProviderProjectionJson(
    "/Backbone", "/Backbone", 0, "/provider/a", "/request/7",
    "onnxruntime-cpu", "", {}, true);
  const ndn::Buffer payload(
    reinterpret_cast<const uint8_t*>(text.data()), text.size());
  BOOST_CHECK_THROW(parseNativeProviderAssignmentFields(payload, "/Backbone"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeProviderAssignmentParsesRankSpecificV3Projection)
{
  // Provider projections contain only the ranks assigned to that Provider.
  // The collaboration role nevertheless remains the global role#rank key, so
  // the native decoder must not infer uniqueness from this local subset.
  const auto text = completeProviderProjectionJson(
    "/Stage/1", "/Stage/1#1", 1, "/provider/b", "/request/hybrid",
    "onnxruntime-cpu", "");
  const ndn::Buffer payload(
    reinterpret_cast<const uint8_t*>(text.data()), text.size());

  const auto fields = parseNativeProviderAssignmentFields(payload, "/Stage/1#1");
  BOOST_CHECK_EQUAL(fields.at("role"), "/Stage/1#1");
  BOOST_CHECK_EQUAL(fields.at("rank"), "1");
  BOOST_CHECK_EQUAL(fields.at("backend"), "onnxruntime-cpu");
  BOOST_CHECK_EQUAL(fields.at("device"), "cpu:0");
  BOOST_CHECK_EQUAL(fields.at("artifactDigest"),
                    "sha256:" + std::string(64, 'a'));
}

BOOST_AUTO_TEST_CASE(NativeProviderAssignmentParsesPerRoleExecutionBinding)
{
  const auto text = completeProviderProjectionJson(
    "S0R0", "S0R0", 0, "/provider/a", "/request/lease",
    "onnxruntime-cpu", "",
    R"({"S0R0":{"activation_digest":"sha256:activation","activation_local_member":"member-s0","activation_members":"member-s0,member-s1","lease_binding_proof":"proof-s0","lease_epoch":"epoch-s0","lease_id":"lease-s0","lease_plan_digest":"sha256:plan","lease_provider_role_count":"2","provider_boot_id":"boot-s0"}})");
  const ndn::Buffer payload(
    reinterpret_cast<const uint8_t*>(text.data()), text.size());

  const auto fields = parseNativeProviderAssignmentFields(payload, "S0R0");
  BOOST_CHECK_EQUAL(fields.at("executionProviderBootId"), "boot-s0");
  BOOST_CHECK_EQUAL(fields.at("executionLeaseId"), "lease-s0");
  BOOST_CHECK_EQUAL(fields.at("executionLeaseEpoch"), "epoch-s0");
  BOOST_CHECK_EQUAL(fields.at("executionLeasePlanDigest"), "sha256:plan");
  BOOST_CHECK_EQUAL(fields.at("executionLeaseBindingProof"), "proof-s0");
  BOOST_CHECK_EQUAL(fields.at("executionLeaseProviderRoleCount"), "2");
  BOOST_CHECK_EQUAL(fields.at("executionActivationDigest"), "sha256:activation");
  BOOST_CHECK_EQUAL(fields.at("executionActivationMembers"),
                    "member-s0,member-s1");
  BOOST_CHECK_EQUAL(fields.at("executionActivationLocalMember"), "member-s0");
  BOOST_CHECK_EQUAL(fields.at("groupCapabilityV1"), "aabbcc");
}

BOOST_AUTO_TEST_CASE(NativeSelectionProjectionBuildsRequestScopedHybridPlan)
{
  const std::string digestA = "sha256:" + std::string(64, 'a');
  const std::string digestB = "sha256:" + std::string(64, 'B');
  const std::string digestC = "sha256:" + std::string(64, 'c');
  const std::string localRole =
    std::string("{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\",") +
    "\"artifact_digest\":\"" + digestC +
    "\",\"backend\":\"onnxruntime-cpu\",\"device_set\":[]," +
    "\"layer_begin\":4,\"layer_end\":8,\"rank\":1," +
    "\"recipe_digest\":\"" + digestA +
    "\",\"role\":\"/Stage/1\",\"role_kind\":\"TENSOR_RANK\"}";
  const std::string text =
    std::string("{\"ack_closed_digest\":\"") + digestA +
    "\",\"assembly\":" + localRole +
    ",\"attempt\":3,\"deadline_ms\":12000,\"dependencies\":[{" +
    "\"consumers\":[\"/Stage/1#0\",\"/Stage/1#1\"]," +
    "\"key_scope\":\"tensor-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"/Stage/0\"],\"required\":true," +
    "\"topic_prefix\":\"/activation\",\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveOperationIndex\":7,\"collectiveProducerRank\":\"0\"," +
    "\"collectiveSourceLayoutDigest\":\"" + digestA + "\"," +
    "\"collectiveTargetLayoutDigest\":\"" + digestB + "\"," +
    "\"collectiveTensorDigest\":\"" + digestC + "\"," +
    "\"tensors\":[\"activation-0\"],\"redistributions\":[{" +
    "\"producerRanks\":[0],\"consumerRanks\":[1,2]," +
    "\"tensor\":\"activation-0\",\"operation\":\"SCATTER\"," +
    "\"epoch\":\"epoch-3\",\"integrityDigest\":\"" + digestC + "\"," +
    "\"sourceLayoutDigest\":\"" + digestA + "\"," +
    "\"targetLayoutDigest\":\"" + digestB + "\"," +
    "\"temporaryMemoryBytes\":4096,\"completeOutput\":true}]}]," +
    "\"device_binding\":{\"mode\":\"CPU\",\"offer_digest\":\"" +
    digestA +
    "\",\"offer_scoped_device_handle\":\"\",\"provider\":\"/provider/b\"," +
    "\"resource_sequence\":1,\"resource_snapshot_digest\":\"" +
    digestA +
    "\",\"role\":\"/Stage/1#1\",\"sharing_policy\":\"EXCLUSIVE_ROLE\"," +
    "\"topology_profile_digest\":\"" + digestA + "\"}," +
    "\"execution_role\":{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\"," +
    "\"backend\":\"onnxruntime-cpu\",\"layer_begin\":4,\"layer_end\":8," +
    "\"rank\":1,\"role_id\":\"/Stage/1#1\",\"stage_id\":\"/Stage/1\"}," +
    "\"group_capability_v1\":\"aabb\",\"plan_core_digest\":\"" + digestA +
    "\",\"plan_digest\":\"" + digestB +
    "\",\"offer_digest\":\"" + digestA +
    "\",\"provider\":\"/provider/b\",\"request_id\":\"/request/hybrid\"," +
    "\"dataflow\":{\"attempt\":3,\"dataflow_digest\":\"" + digestC +
    "\",\"may_publish\":[],\"must_fetch\":[],\"plan_digest\":\"" +
    digestB +
    "\",\"request_id\":\"/request/hybrid\",\"role\":\"/Stage/1#1\"," +
    "\"terminal_response_owner\":true,\"wait_for\":[]}," +
    "\"roles\":[" + localRole + "]," +
    "\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3," +
    "\"security_policy_snapshot_digest\":\"" + digestA + "\"}";

  std::istringstream input(text);
  const auto projection = nativeSelectionProjectionV3FromJson(
    input, "/Stage/1#1");
  BOOST_CHECK_EQUAL(projection.provider, "/provider/b");
  BOOST_CHECK_EQUAL(projection.requestId, "/request/hybrid");
  BOOST_CHECK_EQUAL(projection.attempt, 3U);
  BOOST_CHECK_EQUAL(projection.selectedRole.selectedRole, "/Stage/1#1");
  BOOST_CHECK_EQUAL(projection.selectedRole.rank, 1U);
  BOOST_CHECK_EQUAL(projection.plan.roles.size(), 3U);
  BOOST_REQUIRE_EQUAL(projection.plan.dependencies.size(), 1U);
  const auto& dependency = projection.plan.dependencies.front();
  BOOST_CHECK_EQUAL(dependency.keyScope, "tensor-0");
  BOOST_CHECK_EQUAL(dependency.objectNameTemplate,
                    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  BOOST_CHECK(dependency.useNdnsfDataV1);
  BOOST_REQUIRE_EQUAL(dependency.redistributions.size(), 1U);
  const auto& redistribution = dependency.redistributions.front();
  BOOST_CHECK_EQUAL(redistribution.operation, "SCATTER");
  BOOST_CHECK_EQUAL(redistribution.producerRanks.front(), 0U);
  BOOST_CHECK_EQUAL(redistribution.consumerRanks.back(), 2U);
  BOOST_CHECK_EQUAL(redistribution.temporaryMemoryBytes, 4096U);
  BOOST_CHECK(redistribution.completeOutput);
}

BOOST_AUTO_TEST_CASE(NativeSelectionProjectionRejectsMissingRequiredRedistribution)
{
  const std::string digest = "sha256:" + std::string(64, 'a');
  const std::string text =
    std::string("{\"attempt\":1,\"deadline_ms\":12000,\"dependencies\":[{") +
    "\"consumers\":[\"S1R0\",\"S1R1\"]," +
    "\"key_scope\":\"boundary-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S0R0\"],\"required\":true," +
    "\"topic_prefix\":\"/activation\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveSourceLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTargetLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTensorDigest\":\"" + digest + "\"," +
    "\"tensors\":[\"activation\"],\"redistributions\":[]}]," +
    "\"group_capability_v1\":\"aabb\",\"plan_core_digest\":\"" + digest +
    "\",\"plan_digest\":\"" + digest +
    "\",\"provider\":\"/provider/a\",\"request_id\":\"/request/missing-redistribution\"," +
    "\"roles\":[{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\"," +
    "\"artifact_digest\":\"" + digest +
    "\",\"backend\":\"onnxruntime\",\"device_set\":[\"cpu:0\"]," +
    "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
    "\"recipe_digest\":\"" + digest +
    "\",\"role\":\"S0R0\",\"role_kind\":\"TENSOR_RANK\"}]," +
    "\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3}";

  std::istringstream input(text);
  BOOST_CHECK_THROW(
    nativeSelectionProjectionV3FromJson(input, "S0R0"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeSelectionProjectionRejectsDuplicateRedistribution)
{
  const std::string digest = "sha256:" + std::string(64, 'b');
  const std::string redistribution =
    std::string("{\"producerRanks\":[0],\"consumerRanks\":[1,2],") +
    "\"tensor\":\"activation\",\"operation\":\"SCATTER\"," +
    "\"epoch\":\"epoch-1\",\"integrityDigest\":\"" + digest + "\"," +
    "\"sourceLayoutDigest\":\"" + digest + "\"," +
    "\"targetLayoutDigest\":\"" + digest + "\"," +
    "\"temporaryMemoryBytes\":1024,\"completeOutput\":true}";
  const std::string text =
    std::string("{\"attempt\":1,\"deadline_ms\":12000,\"dependencies\":[{") +
    "\"consumers\":[\"S1R0\",\"S1R1\"]," +
    "\"key_scope\":\"boundary-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S0R0\"],\"required\":true," +
    "\"topic_prefix\":\"/activation\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveSourceLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTargetLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTensorDigest\":\"" + digest + "\"," +
    "\"tensors\":[\"activation\"],\"redistributions\":[" +
    redistribution + "," + redistribution + "]}]," +
    "\"group_capability_v1\":\"aabb\",\"plan_core_digest\":\"" + digest +
    "\",\"plan_digest\":\"" + digest +
    "\",\"provider\":\"/provider/a\",\"request_id\":\"/request/duplicate-redistribution\"," +
    "\"roles\":[{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\"," +
    "\"artifact_digest\":\"" + digest +
    "\",\"backend\":\"onnxruntime\",\"device_set\":[\"cpu:0\"]," +
    "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
    "\"recipe_digest\":\"" + digest +
    "\",\"role\":\"S0R0\",\"role_kind\":\"TENSOR_RANK\"}]," +
    "\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3}";

  std::istringstream input(text);
  BOOST_CHECK_THROW(
    nativeSelectionProjectionV3FromJson(input, "S0R0"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeSelectionProjectionRejectsWrongRedistributionOperation)
{
  const std::string digest = "sha256:" + std::string(64, 'c');
  const std::string text =
    std::string("{\"attempt\":1,\"deadline_ms\":12000,\"dependencies\":[{") +
    "\"consumers\":[\"S1R0\",\"S1R1\"]," +
    "\"key_scope\":\"boundary-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S0R0\"],\"required\":true," +
    "\"topic_prefix\":\"/activation\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveSourceLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTargetLayoutDigest\":\"" + digest + "\"," +
    "\"collectiveTensorDigest\":\"" + digest + "\"," +
    "\"tensors\":[\"activation\"],\"redistributions\":[{" +
    "\"producerRanks\":[0],\"consumerRanks\":[1,2]," +
    "\"tensor\":\"activation\",\"operation\":\"GATHER\"," +
    "\"epoch\":\"epoch-1\",\"integrityDigest\":\"" + digest + "\"," +
    "\"sourceLayoutDigest\":\"" + digest + "\"," +
    "\"targetLayoutDigest\":\"" + digest + "\"," +
    "\"temporaryMemoryBytes\":1024,\"completeOutput\":true}]}]," +
    "\"group_capability_v1\":\"aabb\",\"plan_core_digest\":\"" + digest +
    "\",\"plan_digest\":\"" + digest +
    "\",\"provider\":\"/provider/a\",\"request_id\":\"/request/wrong-operation\"," +
    "\"roles\":[{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\"," +
    "\"artifact_digest\":\"" + digest +
    "\",\"backend\":\"onnxruntime\",\"device_set\":[\"cpu:0\"]," +
    "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
    "\"recipe_digest\":\"" + digest +
    "\",\"role\":\"S0R0\",\"role_kind\":\"TENSOR_RANK\"}]," +
    "\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3}";

  std::istringstream input(text);
  BOOST_CHECK_THROW(
    nativeSelectionProjectionV3FromJson(input, "S0R0"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeSelectionProjectionRejectsRedistributionRankCountMismatch)
{
  const std::string sourceDigest = "sha256:" + std::string(64, 'd');
  const std::string targetDigest = "sha256:" + std::string(64, 'e');
  const std::string tensorDigest = "sha256:" + std::string(64, 'f');
  const std::string text =
    std::string("{\"attempt\":1,\"deadline_ms\":12000,\"dependencies\":[{") +
    "\"consumers\":[\"S1R0\"],\"key_scope\":\"boundary-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S0R0\",\"S0R1\"],\"required\":true," +
    "\"topic_prefix\":\"/activation\",\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveSourceLayoutDigest\":\"" + sourceDigest +
    "\",\"collectiveTargetLayoutDigest\":\"" + targetDigest +
    "\",\"collectiveTensorDigest\":\"" + tensorDigest +
    "\",\"tensors\":[\"activation\"],\"redistributions\":[{" +
    "\"producerRanks\":[0,1,9],\"consumerRanks\":[2]," +
    "\"tensor\":\"activation\",\"operation\":\"GATHER\"," +
    "\"epoch\":\"epoch-1\",\"integrityDigest\":\"" + tensorDigest +
    "\",\"sourceLayoutDigest\":\"" + sourceDigest +
    "\",\"targetLayoutDigest\":\"" + targetDigest +
    "\",\"axis\":1,\"temporaryMemoryBytes\":1024," +
    "\"completeOutput\":true}]}],\"group_capability_v1\":\"aabb\"," +
    "\"plan_core_digest\":\"" + sourceDigest +
    "\",\"plan_digest\":\"" + targetDigest +
    "\",\"provider\":\"/provider/a\",\"request_id\":\"/request/rank-mismatch\"," +
    "\"roles\":[{\"adapter_id\":\"qwen\",\"adapter_version\":\"1\"," +
    "\"artifact_digest\":\"" + tensorDigest +
    "\",\"backend\":\"onnxruntime\",\"device_set\":[\"cpu:0\"]," +
    "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
    "\"recipe_digest\":\"" + sourceDigest +
    "\",\"role\":\"S1R0\",\"role_kind\":\"TENSOR_RANK\"}]," +
    "\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3}";

  std::istringstream input(text);
  BOOST_CHECK_THROW(
    nativeSelectionProjectionV3FromJson(input, "S1R0"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(HybridRoleProjectionPublishesOnceAndKeepsGatherInputsDistinct)
{
  const auto digest = "sha256:" + std::string(64, 'a');
  const auto redistribution = [&] (std::vector<std::uint64_t> producers,
                                   std::vector<std::uint64_t> consumers,
                                   std::string operation) {
    RedistributionSpec spec;
    spec.producerRanks = std::move(producers);
    spec.consumerRanks = std::move(consumers);
    spec.tensor = "activation";
    spec.operation = std::move(operation);
    spec.epoch = "epoch-1";
    spec.integrityDigest = digest;
    spec.sourceLayoutDigest = digest;
    spec.targetLayoutDigest = digest;
    spec.temporaryMemoryBytes = 1024;
    spec.completeOutput = true;
    return spec;
  };

  NativeDependencySpec scatter(
    {"S0R0"}, {"S1R0", "S1R1"}, "boundary-0", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  scatter.redistributions = {redistribution({0}, {1, 2}, "SCATTER")};
  NativeDependencySpec gather(
    {"S1R0", "S1R1"}, {"S2R0"}, "boundary-1", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  gather.redistributions = {redistribution({1, 2}, {3}, "GATHER")};

  NativeExecutionPlan plan;
  plan.roles = {"S0R0", "S1R0", "S1R1", "S2R0"};
  plan.dependencies = {scatter, gather};
  NativeProviderAssignment assignment;
  assignment.providerByRole = {
    {"S0R0", "/p0"}, {"S1R0", "/p0"},
    {"S1R1", "/p1"}, {"S2R0", "/p1"},
  };

  const auto stage0 = roleSpecFor(plan, "S0R0", "/request/1", assignment, "/p0");
  BOOST_REQUIRE_EQUAL(stage0.outputs.size(), 1U);
  BOOST_CHECK_EQUAL(stage0.outputs.front().scope, "boundary-0");

  const auto rank0 = roleSpecFor(plan, "S1R0", "/request/1", assignment, "/p0");
  BOOST_REQUIRE_EQUAL(rank0.outputs.size(), 1U);
  BOOST_CHECK_EQUAL(rank0.outputs.front().scope, "boundary-1/from/S1R0");
  BOOST_CHECK_EQUAL(rank0.outputs.front().transportScope, "boundary-1");

  const auto rank1 = roleSpecFor(plan, "S1R1", "/request/1", assignment, "/p1");
  BOOST_REQUIRE_EQUAL(rank1.outputs.size(), 1U);
  BOOST_CHECK_EQUAL(rank1.outputs.front().scope, "boundary-1/from/S1R1");

  const auto stage2 = roleSpecFor(plan, "S2R0", "/request/1", assignment, "/p1");
  BOOST_REQUIRE_EQUAL(stage2.inputs.size(), 2U);
  BOOST_CHECK_NE(stage2.inputs[0].scope, stage2.inputs[1].scope);
  BOOST_CHECK_EQUAL(stage2.inputs[0].transportScope, "boundary-1");
  BOOST_CHECK_EQUAL(stage2.inputs[1].transportScope, "boundary-1");
  BOOST_CHECK_EQUAL(stage2.inputs[0].redistributions.front().operation, "GATHER");
}

BOOST_AUTO_TEST_CASE(CertifiedRedistributionScattersActivationForConsumerRank)
{
  NamedTensor activation;
  activation.name = "activation";
  activation.elementType = TensorElementType::Float32;
  activation.shape = {1, 4};
  activation.payload = rawTensorPayload<float>({1.0F, 2.0F, 3.0F, 4.0F});

  RoleExecutionContext context;
  context.sessionId = "/request/scatter";
  context.role = "S1R1";
  context.inputsByScope.emplace(
    "boundary-0", makeEncodedTensorBundle("activation", {activation}));

  RedistributionSpec redistribution;
  redistribution.producerRanks = {0};
  redistribution.consumerRanks = {1, 2};
  redistribution.tensor = "activation";
  redistribution.operation = "SCATTER";
  redistribution.axis = 1;
  redistribution.epoch = "epoch-1";
  redistribution.integrityDigest = "sha256:" + std::string(64, 'a');
  redistribution.sourceLayoutDigest = "sha256:" + std::string(64, 'b');
  redistribution.targetLayoutDigest = "sha256:" + std::string(64, 'c');
  redistribution.temporaryMemoryBytes = activation.payload.size();
  redistribution.completeOutput = true;

  DependencyEdge edge;
  edge.scope = "boundary-0";
  edge.transportScope = "boundary-0";
  edge.producerRole = "S0R0";
  edge.consumerRole = "S1R1";
  edge.redistributionProducerRank = 0;
  edge.redistributionConsumerRank = 2;
  edge.redistributions = {redistribution};
  context.inputEdgesByScope.emplace("boundary-0", edge);

  const auto transformed = applyCertifiedTensorRedistributions(context);
  BOOST_REQUIRE_EQUAL(transformed.size(), 1U);
  const auto tensors = decodeTensorBundle(transformed.at("boundary-0").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors.front().shape.size(), 2U);
  BOOST_CHECK_EQUAL(tensors.front().shape[0], 1);
  BOOST_CHECK_EQUAL(tensors.front().shape[1], 2);
  BOOST_REQUIRE_EQUAL(tensors.front().payload.size(), 2U * sizeof(float));
  std::array<float, 2> values{};
  std::memcpy(values.data(), tensors.front().payload.data(),
              tensors.front().payload.size());
  BOOST_CHECK_CLOSE(values[0], 3.0F, 0.001);
  BOOST_CHECK_CLOSE(values[1], 4.0F, 0.001);
}

BOOST_AUTO_TEST_CASE(CertifiedRedistributionGathersEveryProducerRankInOrder)
{
  const auto shard = [] (std::initializer_list<float> values) {
    NamedTensor activation;
    activation.name = "activation";
    activation.elementType = TensorElementType::Float32;
    activation.shape = {1, 2};
    activation.payload = rawTensorPayload<float>(values);
    return makeEncodedTensorBundle("activation", {activation});
  };

  RoleExecutionContext context;
  context.sessionId = "/request/gather";
  context.role = "S2R0";
  context.inputsByScope.emplace(
    "boundary-1/from/S1R0", shard({1.0F, 2.0F}));
  context.inputsByScope.emplace(
    "boundary-1/from/S1R1", shard({3.0F, 4.0F}));

  RedistributionSpec redistribution;
  redistribution.producerRanks = {1, 2};
  redistribution.consumerRanks = {3};
  redistribution.tensor = "activation";
  redistribution.operation = "GATHER";
  redistribution.axis = 1;
  redistribution.epoch = "epoch-1";
  redistribution.integrityDigest = "sha256:" + std::string(64, 'a');
  redistribution.sourceLayoutDigest = "sha256:" + std::string(64, 'b');
  redistribution.targetLayoutDigest = "sha256:" + std::string(64, 'c');
  redistribution.temporaryMemoryBytes = 4U * sizeof(float);
  redistribution.completeOutput = true;

  for (const auto& rankAndRole :
       std::vector<std::pair<std::uint64_t, std::string>>{
         {1, "S1R0"}, {2, "S1R1"}}) {
    DependencyEdge edge;
    edge.scope = "boundary-1/from/" + rankAndRole.second;
    edge.transportScope = "boundary-1";
    edge.producerRole = rankAndRole.second;
    edge.consumerRole = "S2R0";
    edge.redistributionProducerRank = rankAndRole.first;
    edge.redistributionConsumerRank = 3;
    edge.redistributions = {redistribution};
    context.inputEdgesByScope.emplace(edge.scope, edge);
  }

  const auto transformed = applyCertifiedTensorRedistributions(context);
  BOOST_REQUIRE_EQUAL(transformed.size(), 1U);
  const auto tensors = decodeTensorBundle(transformed.at("boundary-1").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors.front().shape[0], 1);
  BOOST_CHECK_EQUAL(tensors.front().shape[1], 4);
  BOOST_REQUIRE_EQUAL(tensors.front().payload.size(), 4U * sizeof(float));
  std::array<float, 4> values{};
  std::memcpy(values.data(), tensors.front().payload.data(),
              tensors.front().payload.size());
  BOOST_CHECK_CLOSE(values[0], 1.0F, 0.001);
  BOOST_CHECK_CLOSE(values[1], 2.0F, 0.001);
  BOOST_CHECK_CLOSE(values[2], 3.0F, 0.001);
  BOOST_CHECK_CLOSE(values[3], 4.0F, 0.001);
}

BOOST_AUTO_TEST_CASE(CertifiedRedistributionReshardsAcrossUnequalRankGroups)
{
  const auto shard = [] (std::initializer_list<float> values) {
    NamedTensor activation;
    activation.name = "activation";
    activation.elementType = TensorElementType::Float32;
    activation.shape = {1, 4};
    activation.payload = rawTensorPayload<float>(values);
    return makeEncodedTensorBundle("activation", {activation});
  };

  RoleExecutionContext context;
  context.sessionId = "/request/reshard";
  context.role = "S1R3";
  context.inputsByScope.emplace(
    "boundary/from/S0R0", shard({1.0F, 2.0F, 3.0F, 4.0F}));
  context.inputsByScope.emplace(
    "boundary/from/S0R1", shard({5.0F, 6.0F, 7.0F, 8.0F}));

  RedistributionSpec redistribution;
  redistribution.producerRanks = {0, 1};
  redistribution.consumerRanks = {2, 3, 4, 5};
  redistribution.tensor = "activation";
  redistribution.operation = "RESHARD";
  redistribution.axis = 1;
  redistribution.epoch = "epoch-1";
  redistribution.integrityDigest = "sha256:" + std::string(64, 'a');
  redistribution.sourceLayoutDigest = "sha256:" + std::string(64, 'b');
  redistribution.targetLayoutDigest = "sha256:" + std::string(64, 'c');
  redistribution.temporaryMemoryBytes = 8U * sizeof(float);
  redistribution.completeOutput = true;

  for (const auto& rankAndRole :
       std::vector<std::pair<std::uint64_t, std::string>>{
         {0, "S0R0"}, {1, "S0R1"}}) {
    DependencyEdge edge;
    edge.scope = "boundary/from/" + rankAndRole.second;
    edge.transportScope = "boundary";
    edge.producerRole = rankAndRole.second;
    edge.consumerRole = "S1R3";
    edge.redistributionProducerRank = rankAndRole.first;
    edge.redistributionConsumerRank = 5;
    edge.redistributions = {redistribution};
    context.inputEdgesByScope.emplace(edge.scope, edge);
  }

  const auto transformed = applyCertifiedTensorRedistributions(context);
  BOOST_REQUIRE_EQUAL(transformed.size(), 1U);
  const auto tensors = decodeTensorBundle(transformed.at("boundary").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors.front().shape[0], 1);
  BOOST_CHECK_EQUAL(tensors.front().shape[1], 2);
  std::array<float, 2> values{};
  std::memcpy(values.data(), tensors.front().payload.data(),
              tensors.front().payload.size());
  BOOST_CHECK_CLOSE(values[0], 7.0F, 0.001);
  BOOST_CHECK_CLOSE(values[1], 8.0F, 0.001);
}

BOOST_AUTO_TEST_CASE(NativeModelRunnerFactoryCreatesRuntimeRunnerFromSpec)
{
  NativeModelRunnerSpec spec{
    "/FactoryRole",
    "onnx-model",
    "test-backend",
    "/tmp/factory-role.onnx",
    {{"outputScope", "factory-to-user"}},
  };

  RegistryNativeModelRunnerFactory factory;
  BOOST_CHECK(!factory.hasBackend("test-backend"));
  factory.registerBackend(
    "test-backend",
    [] (const NativeModelRunnerSpec& runnerSpec) {
      BOOST_CHECK_EQUAL(runnerSpec.role, "/FactoryRole");
      BOOST_CHECK_EQUAL(runnerSpec.kind, "onnx-model");
      BOOST_CHECK_EQUAL(runnerSpec.path, "/tmp/factory-role.onnx");
      const auto outputScope = runnerSpec.metadata.at("outputScope");
      return makeNativeModelRunner(
        [outputScope] (const RoleExecutionContext& ctx) {
          BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
          return std::map<std::string, TensorBundle>{
            {outputScope,
             bundle("factory-result",
                    "factory:" + payloadText(ctx.inputsByScope.begin()->second))},
          };
        });
    });
  BOOST_CHECK(factory.hasBackend("test-backend"));

  NativeProviderRuntime runtime(1);
  runtime.registerRunner(spec.role, factory.create(spec));

  RoleSpec role{
    spec.role,
    {DependencyEdge{"input-to-factory", "/Input", spec.role,
                    "/run/factory/input/bundle/0", 1}},
    {DependencyEdge{"factory-to-user", spec.role, "",
                    "/run/factory/output/bundle/0", 1}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  const auto result = runtime.executeRoleAsync("factory-run", role, io).get();

  BOOST_REQUIRE(result.outputsByScope.count("factory-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("factory-to-user")),
                    "factory:input:input-to-factory");
  BOOST_CHECK_THROW(
    factory.create(NativeModelRunnerSpec{"/Missing", "onnx-model", "onnxruntime", "", {}}),
    std::out_of_range);
}

BOOST_AUTO_TEST_CASE(ProviderRoleResultCarriesPostRunExecutionEvidence)
{
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/A";
  evidence.providerBootId = "boot-a";
  evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
  evidence.realCompute = true;
  evidence.deviceKind = "cpu";
  evidence.deviceId = "cpu0";
  evidence.runtimeVersion = "test-runtime";
  evidence.modelDigest = "sha256:model";
  evidence.planDigest = "sha256:plan";
  evidence.artifactDigests["/EvidenceRole"] = "sha256:artifact";
  evidence.roles = {"/EvidenceRole"};
  evidence.createdAtMs = 1;

  auto runner = makeNativeModelRunner(
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"final-response", bundle("final-response", "done")},
      };
    },
    evidence);
  ProviderRoleWorker worker(1);
  RoleSpec role{"/EvidenceRole", {}, {}};
  const auto result = worker.executeAsync(
    "evidence-run", role, std::make_shared<FakeDependencyIo>(), runner).get();
  BOOST_REQUIRE(result.executionEvidence);
  BOOST_CHECK_EQUAL(result.executionEvidence->roles.front(), "/EvidenceRole");
  BOOST_CHECK(result.executionEvidence->runnerKind == RunnerKind::OnnxRuntimeCpu);
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeBackendRegistersAndReportsBuildState)
{
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  BOOST_CHECK(factory.hasBackend("onnxruntime"));

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_CHECK_THROW(
    factory.create(NativeModelRunnerSpec{
      "/OnnxRole",
      "onnx-model",
      "onnxruntime",
      "/tmp/model.onnx",
      {},
    }),
    std::runtime_error);
#else
  BOOST_CHECK_THROW(
    factory.create(NativeModelRunnerSpec{
      "/OnnxRole",
      "onnx-model",
      "onnxruntime",
      "/tmp/ndnsf-di-missing-model.onnx",
      {},
    }),
    std::exception);
#endif
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeBackendRunsFloat32ModelWhenFixtureProvided)
{
  const auto* modelPath = std::getenv("NDNSF_DI_TEST_ONNX_MODEL");
  if (modelPath == nullptr || std::string(modelPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_TEST_ONNX_MODEL not set; skipping real ONNX Runtime model smoke");
    BOOST_CHECK(true);
    return;
  }

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("NDNSF_DI_TEST_ONNX_MODEL requires C++ ONNX Runtime backend");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = factory.create(NativeModelRunnerSpec{
    "/OnnxRole",
    "onnx-model",
    "onnxruntime",
    modelPath,
    {
      {"inputNames", "x"},
      {"inputShape", "1,3"},
      {"outputNames", "y"},
      {"outputScope", "onnx-to-user"},
    },
  });

  RoleExecutionContext ctx;
  ctx.sessionId = "onnx-runtime-smoke";
  ctx.role = "/OnnxRole";
  TensorBundle input;
  input.name = "x";
  input.payload = floatPayload({1.0f, 2.0f, 3.0f});
  input.expectedBytes = input.payload.size();
  ctx.inputsByScope.emplace("x", std::move(input));

  const auto outputs = runner->run(ctx);
  BOOST_REQUIRE(outputs.count("onnx-to-user") == 1);
  const auto floats = payloadFloats(outputs.at("onnx-to-user"));
  BOOST_REQUIRE_EQUAL(floats.size(), 3);
  BOOST_CHECK_CLOSE(floats[0], 2.0f, 0.001);
  BOOST_CHECK_CLOSE(floats[1], 3.0f, 0.001);
  BOOST_CHECK_CLOSE(floats[2], 4.0f, 0.001);
#endif
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeProviderSelectionRequiresExplicitCpuFallback)
{
  NativeModelRunnerSpec spec;
  spec.metadata["executionProvider"] = "cuda";
  spec.metadata["deviceId"] = "2";
  const std::vector<std::string> cpuOnly{"CPUExecutionProvider"};
  BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, cpuOnly),
                    std::runtime_error);

  spec.metadata["allowCpuFallback"] = "true";
  const auto fallback = resolveOnnxRuntimeProviderSelection(spec, cpuOnly);
  BOOST_CHECK_EQUAL(fallback.requestedProvider, "cuda");
  BOOST_CHECK_EQUAL(fallback.selectedProvider, "cpu");
  BOOST_CHECK_EQUAL(fallback.deviceId, "cpu0");
  BOOST_CHECK(fallback.usedCpuFallback);

  spec.metadata["allowCpuFallback"] = "false";
  const auto cuda = resolveOnnxRuntimeProviderSelection(
    spec, {"CUDAExecutionProvider", "CPUExecutionProvider"});
  BOOST_CHECK_EQUAL(cuda.selectedProvider, "cuda");
  BOOST_CHECK_EQUAL(cuda.deviceId, "2");
  BOOST_CHECK(!cuda.usedCpuFallback);

  spec.metadata["executionProvider"] = "tensorrt";
  BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, cpuOnly),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeBackendRunsDynamicPilotDtypesAndReportsDeviceEvidence)
{
  const auto* modelPath = std::getenv("NDNSF_DI_TEST_ONNX_TYPED_MODEL");
  if (modelPath == nullptr || std::string(modelPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_TEST_ONNX_TYPED_MODEL not set; skipping typed ONNX smoke");
    return;
  }
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("NDNSF_DI_TEST_ONNX_TYPED_MODEL requires C++ ONNX Runtime backend");
#else
  NativeModelRunnerSpec spec;
  spec.role = "/LLM/Stage/0";
  spec.backend = "onnxruntime";
  spec.path = modelPath;
  spec.metadata = {
    {"executionProvider", "cpu"},
    {"inputNames", "input_ids,attention_mask,hidden_states"},
    {"outputNames", "ids_out,mask_out,hidden_out"},
    {"outputAlias.hidden_out", "hidden_states"},
    {"outputBundleScope", "pilot-output"},
    {"kvOutputTensors", "hidden_states"},
    {"kvOutputScope", "kv-state"},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"evidence.evidenceEpoch", "1"},
    {"evidence.roles", "/LLM/Stage/0"},
    {"evidence.modelDigest", "sha256:model"},
    {"evidence.planDigest", "sha256:plan"},
    {"evidence.artifactDigests", "/LLM/Stage/0=sha256:artifact"},
    {"evidence.createdAtMs", "1"},
  };
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto unavailableCudaSpec = spec;
  unavailableCudaSpec.metadata["executionProvider"] = "cuda";
  unavailableCudaSpec.metadata["allowCpuFallback"] = "false";
  BOOST_CHECK_THROW(factory.create(unavailableCudaSpec), std::runtime_error);
  auto runner = factory.create(spec);

  RoleExecutionContext ctx;
  ctx.sessionId = "typed-onnx";
  ctx.role = spec.role;
  ctx.inputsByScope["pilot-input"] = makeEncodedTensorBundle(
    "pilot-input",
    {
      NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                  rawTensorPayload<std::int64_t>({1, 2, 3})},
      NamedTensor{"attention_mask", TensorElementType::Bool, {1, 3},
                  rawTensorPayload<std::uint8_t>({1, 1, 1})},
      NamedTensor{"hidden_states", TensorElementType::Float16, {1, 3, 4},
                  std::vector<std::uint8_t>(24, 0)},
    });
  const auto outputs = runner->run(ctx);
  BOOST_REQUIRE(outputs.count("pilot-output") == 1);
  BOOST_REQUIRE(outputs.count("kv-state") == 1);
  const auto decoded = decodeTensorBundle(outputs.at("pilot-output").payload);
  BOOST_CHECK(findTensor(decoded, "ids_out").elementType == TensorElementType::Int64);
  BOOST_CHECK(findTensor(decoded, "mask_out").elementType == TensorElementType::Bool);
  BOOST_CHECK(findTensor(decoded, "hidden_states").elementType == TensorElementType::Float16);
  BOOST_CHECK_EQUAL(findTensor(decoded, "hidden_states").shape[1], 3);
  const auto kvDecoded = decodeTensorBundle(outputs.at("kv-state").payload);
  BOOST_CHECK_EQUAL(kvDecoded.size(), 1);
  BOOST_CHECK_EQUAL(kvDecoded.front().name, "hidden_states");
  BOOST_REQUIRE(runner->executionEvidence());
  BOOST_CHECK(runner->executionEvidence()->runnerKind == RunnerKind::OnnxRuntimeCpu);
  BOOST_CHECK_EQUAL(runner->executionEvidence()->deviceKind, "cpu");
  BOOST_CHECK(!runner->executionEvidence()->runtimeVersion.empty());
#endif
}

BOOST_AUTO_TEST_CASE(NativeTensorBundleCodecRoundTripsMultipleFloat32Tensors)
{
  const auto payload = encodeTensorBundle({
    makeFloat32Tensor("x", {1, 2}, floatPayload({1.0f, 2.0f})),
    makeFloat32Tensor("y", {1, 2}, floatPayload({3.0f, 4.0f})),
  });
  BOOST_CHECK(isEncodedTensorBundle(payload));

  const auto tensors = decodeTensorBundle(payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 2);
  BOOST_CHECK_EQUAL(findTensor(tensors, "x").name, "x");
  BOOST_CHECK_EQUAL(findTensor(tensors, "y").name, "y");
  BOOST_CHECK_EQUAL(findTensor(tensors, "x").shape.size(), 2);
  BOOST_CHECK_EQUAL(findTensor(tensors, "x").shape[0], 1);
  BOOST_CHECK_EQUAL(findTensor(tensors, "x").shape[1], 2);

  TensorBundle xBundle;
  xBundle.name = "x";
  xBundle.payload = findTensor(tensors, "x").payload;
  const auto values = payloadFloats(xBundle);
  BOOST_REQUIRE_EQUAL(values.size(), 2);
  BOOST_CHECK_CLOSE(values[0], 1.0f, 0.001);
  BOOST_CHECK_CLOSE(values[1], 2.0f, 0.001);
  BOOST_CHECK_THROW(findTensor(tensors, "missing"), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(NativeTensorBundleCodecSelectsNamedTensorSubset)
{
  const auto bundle = makeEncodedTensorBundle(
    "all-tensors",
    {
      makeFloat32Tensor("x", {1, 1}, floatPayload({1.0f})),
      makeFloat32Tensor("y", {1, 1}, floatPayload({2.0f})),
      makeFloat32Tensor("z", {1, 1}, floatPayload({3.0f})),
    });

  const auto subset = selectTensorBundle("edge-yz", bundle, {"y", "z"});
  BOOST_CHECK_EQUAL(subset.name, "edge-yz");
  const auto tensors = decodeTensorBundle(subset.payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 2);
  BOOST_CHECK_EQUAL(tensors[0].name, "y");
  BOOST_CHECK_EQUAL(tensors[1].name, "z");
  BOOST_CHECK_THROW(selectTensorBundle("missing", bundle, {"missing"}),
                    std::out_of_range);
}

BOOST_AUTO_TEST_CASE(GenerationEpochLineageRoundTripsInsideTensorBundle)
{
  GenerationEpochLineageV1 lineage;
  lineage.requestId = "request-a";
  lineage.attemptEpoch = 1;
  lineage.planDigest = stateDigest('1');
  lineage.generationId = "generation-a";
  lineage.streamEpoch = 3;
  lineage.inferenceEpoch = 2;
  lineage.transitionKind = GenerationEpochLineageV1::DECODE;
  lineage.logicalPrefixDigest = stateDigest('2');
  lineage.logicalPrefixTokenCount = 5;
  lineage.positionDigest = stateDigest('3');
  lineage.producerRole = "/Stage/0";
  lineage.consumerRole = "/Stage/1";
  lineage.operationIndex = 9;

  const auto original = makeEncodedTensorBundle(
    "activation",
    {NamedTensor{"hidden_states", TensorElementType::Float32, {1},
                 rawTensorPayload<float>({1.0f})}});
  const auto attached = attachGenerationEpochLineage(original, lineage);
  const auto decoded = extractGenerationEpochLineage(attached);
  BOOST_REQUIRE(decoded.has_value());
  BOOST_CHECK(*decoded == lineage);

  const auto stripped = stripGenerationEpochLineage(attached);
  BOOST_CHECK(!extractGenerationEpochLineage(stripped).has_value());
  BOOST_REQUIRE_EQUAL(decodeTensorBundle(stripped.payload).size(), 1);
  BOOST_CHECK_EQUAL(decodeTensorBundle(stripped.payload).front().name,
                    "hidden_states");
}

BOOST_AUTO_TEST_CASE(GenerationEpochLineageRejectsUnknownOrInconsistentTransition)
{
  GenerationEpochLineageV1 lineage;
  lineage.requestId = "request-transition";
  lineage.attemptEpoch = 1;
  lineage.planDigest = stateDigest('1');
  lineage.generationId = "generation-transition";
  lineage.streamEpoch = 1;
  lineage.logicalPrefixDigest = stateDigest('2');
  lineage.logicalPrefixTokenCount = 1;
  lineage.positionDigest = stateDigest('3');
  lineage.producerRole = "/Stage/0";
  lineage.consumerRole = "/Stage/1";

  lineage.transitionKind = GenerationEpochLineageV1::PREFILL;
  lineage.inferenceEpoch = 0;
  BOOST_CHECK_NO_THROW(lineage.validate());

  lineage.transitionKind = GenerationEpochLineageV1::DECODE;
  lineage.inferenceEpoch = 1;
  BOOST_CHECK_NO_THROW(lineage.validate());

  lineage.transitionKind = GenerationEpochLineageV1::CHECKPOINT_FINALIZE;
  BOOST_CHECK_NO_THROW(lineage.validate());

  lineage.transitionKind = "UNKNOWN";
  BOOST_CHECK_THROW(lineage.validate(), std::invalid_argument);

  lineage.transitionKind = GenerationEpochLineageV1::PREFILL;
  lineage.inferenceEpoch = 1;
  BOOST_CHECK_THROW(lineage.validate(), std::invalid_argument);

  lineage.transitionKind = GenerationEpochLineageV1::DECODE;
  lineage.inferenceEpoch = 0;
  BOOST_CHECK_THROW(lineage.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeTensorBundleCodecRoundTripsPilotDtypesDynamicShapesAndKvOutputs)
{
  const std::vector<NamedTensor> tensors{
    NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                rawTensorPayload<std::int64_t>({1, 2, 3})},
    NamedTensor{"attention_mask", TensorElementType::Bool, {1, 3},
                rawTensorPayload<std::uint8_t>({1, 1, 1})},
    NamedTensor{"hidden_states", TensorElementType::Float16, {1, 3, 4},
                std::vector<std::uint8_t>(1 * 3 * 4 * 2, 0)},
    NamedTensor{"present.0.key", TensorElementType::Float32, {1, 2, 3, 4},
                std::vector<std::uint8_t>(1 * 2 * 3 * 4 * 4, 0)},
    NamedTensor{"present.0.value", TensorElementType::Float32, {1, 2, 3, 4},
                std::vector<std::uint8_t>(1 * 2 * 3 * 4 * 4, 0)},
    NamedTensor{"past.1.key", TensorElementType::Float32, {1, 2, 0, 4}, {}},
  };
  const auto decoded = decodeTensorBundle(encodeTensorBundle(tensors));
  BOOST_REQUIRE_EQUAL(decoded.size(), tensors.size());
  BOOST_CHECK(findTensor(decoded, "input_ids").elementType == TensorElementType::Int64);
  BOOST_CHECK(findTensor(decoded, "attention_mask").elementType == TensorElementType::Bool);
  BOOST_CHECK(findTensor(decoded, "hidden_states").elementType == TensorElementType::Float16);
  BOOST_CHECK_EQUAL(findTensor(decoded, "present.0.key").shape[2], 3);

  auto dynamic = tensors;
  dynamic[0].shape = {1, 5};
  dynamic[0].payload = rawTensorPayload<std::int64_t>({1, 2, 3, 4, 5});
  BOOST_CHECK_EQUAL(
    findTensor(decodeTensorBundle(encodeTensorBundle(dynamic)), "input_ids").shape[1],
    5);
}

BOOST_AUTO_TEST_CASE(NativeTensorBundleCodecRejectsMalformedShapesTypesAndPayloadSizes)
{
  BOOST_CHECK_THROW(
    encodeTensorBundle({NamedTensor{"bad", TensorElementType::Int64, {1, 2},
                                    rawTensorPayload<std::int64_t>({1})}}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    encodeTensorBundle({NamedTensor{"bad", TensorElementType::Float32, {1, -1}, {}}}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    encodeTensorBundle({NamedTensor{"bad", static_cast<TensorElementType>(999), {1},
                                    std::vector<std::uint8_t>(4, 0)}}),
    std::invalid_argument);

  auto encoded = encodeTensorBundle({
    NamedTensor{"x", TensorElementType::Float32, {1},
                rawTensorPayload<float>({1.0f})},
  });
  encoded.pop_back();
  BOOST_CHECK_THROW(decodeTensorBundle(encoded), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerPublishesEdgeTensorSubsetFromBundle)
{
  RoleSpec role;
  role.role = "/Backbone";
  role.outputs = {
    DependencyEdge{"backbone-to-head0", "/Backbone", "/Head/0", "/run/backbone/h0", 1, 4, {"y0"}},
    DependencyEdge{"backbone-to-head1", "/Backbone", "/Head/1", "/run/backbone/h1", 1, 4, {"y1"}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);
  auto future = worker.executeAsync(
    "tensor-subset-run",
    role,
    io,
    [] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle(
           "onnx-output-bundle",
           {
             makeFloat32Tensor("y0", {1, 1}, floatPayload({10.0f})),
             makeFloat32Tensor("y1", {1, 1}, floatPayload({20.0f})),
           })},
      };
    });

  const auto result = future.get();
  BOOST_REQUIRE(result.outputsByScope.count("backbone-to-head0") == 1);
  BOOST_REQUIRE(result.outputsByScope.count("backbone-to-head1") == 1);
  BOOST_CHECK_EQUAL(
    decodeTensorBundle(result.outputsByScope.at("backbone-to-head0").payload)[0].name,
    "y0");
  BOOST_CHECK_EQUAL(
    decodeTensorBundle(result.outputsByScope.at("backbone-to-head1").payload)[0].name,
    "y1");

  {
    std::lock_guard<std::mutex> lock(io->mutex);
    BOOST_REQUIRE_EQUAL(io->publishedByScope.size(), 2);
    BOOST_CHECK(io->publishedByScope.count("backbone-to-head0") == 1);
    BOOST_CHECK(io->publishedByScope.count("backbone-to-head1") == 1);
  }
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerUsesProviderLocalExactForwardCache)
{
  RoleSpec role;
  role.role = "/LLM/Stage/0";
  role.outputs = {
    DependencyEdge{"stage0-to-stage1", "/LLM/Stage/0", "/LLM/Stage/1",
                   "/run/cache/stage0", 1},
  };

  int runCount = 0;
  auto runner = makeNativeModelRunner(
    [&runCount] (const RoleExecutionContext& ctx) {
      ++runCount;
      BOOST_REQUIRE(ctx.inputsByScope.count("prompt") == 1);
      return std::map<std::string, TensorBundle>{
        {"stage0-to-stage1",
         bundle("stage0-output", "forward:" + payloadText(ctx.inputsByScope.at("prompt")))},
      };
    });

  auto io = std::make_shared<FakeDependencyIo>();
  ProviderRoleWorker worker(1);
  auto first = worker.executeAsync(
    "request-1",
    role,
    io,
    runner,
    {{"prompt", bundle("prompt", "same-token-prefix")}}).get();
  auto second = worker.executeAsync(
    "request-2",
    role,
    io,
    runner,
    {{"prompt", bundle("prompt", "same-token-prefix")}}).get();

  BOOST_CHECK_EQUAL(runCount, 1);
  BOOST_CHECK(!first.exactForwardCacheHit);
  BOOST_CHECK(second.exactForwardCacheHit);
  BOOST_CHECK_EQUAL(first.exactForwardCacheKey, second.exactForwardCacheKey);
  BOOST_CHECK_EQUAL(payloadText(second.outputsByScope.at("stage0-to-stage1")),
                    "forward:same-token-prefix");

  auto third = worker.executeAsync(
    "request-3",
    role,
    io,
    runner,
    {{"prompt", bundle("prompt", "different-token-prefix")}}).get();
  BOOST_CHECK_EQUAL(runCount, 2);
  BOOST_CHECK(!third.exactForwardCacheHit);
  BOOST_CHECK_NE(second.exactForwardCacheKey, third.exactForwardCacheKey);
}

BOOST_AUTO_TEST_CASE(DependencyWaitSchedulerBoundsOneThousandWaitsAndCompletesOnce)
{
  DependencyWaitScheduler scheduler(4, 1000);
  std::atomic<bool> release{false};
  std::atomic<std::size_t> terminalCount{0};
  std::atomic<std::size_t> unexpectedTerminals{0};
  std::atomic<std::size_t> duplicateTerminals{0};
  std::mutex terminalMutex;
  std::set<std::string> terminalIds;

  for (std::size_t i = 0; i < 1000; ++i) {
    const auto id = "wait-" + std::to_string(i);
    const auto admitted = scheduler.submit(
      id,
      std::chrono::steady_clock::now() + std::chrono::seconds(5),
      [&release] (const DependencyWaitControl& control) {
        while (!release.load()) {
          if (control.isCancelled()) return DependencyWaitStatus::Cancelled;
          if (control.deadlineExpired()) return DependencyWaitStatus::DeadlineExpired;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return DependencyWaitStatus::Completed;
      },
      [&] (const DependencyWaitResult& result) {
        if (result.status != DependencyWaitStatus::Completed) {
          ++unexpectedTerminals;
        }
        std::lock_guard<std::mutex> lock(terminalMutex);
        if (!terminalIds.insert(result.waitId).second) {
          ++duplicateTerminals;
        }
        ++terminalCount;
      });
    BOOST_REQUIRE_EQUAL(admitted, DependencyWaitSubmitResult::Accepted);
  }

  const auto loaded = scheduler.snapshot();
  BOOST_CHECK_EQUAL(loaded.workerCount, 4);
  BOOST_CHECK_LE(loaded.activeCount, 4);
  BOOST_CHECK_EQUAL(loaded.activeCount + loaded.queuedCount, 1000);
  release = true;
  BOOST_REQUIRE(scheduler.waitForIdle(std::chrono::seconds(5)));
  const auto done = scheduler.snapshot();
  BOOST_CHECK_EQUAL(done.activeCount, 0);
  BOOST_CHECK_EQUAL(done.queuedCount, 0);
  BOOST_CHECK_EQUAL(done.completed, 1000);
  BOOST_CHECK_EQUAL(terminalCount.load(), 1000);
  BOOST_CHECK_EQUAL(unexpectedTerminals.load(), 0);
  BOOST_CHECK_EQUAL(duplicateTerminals.load(), 0);
}

BOOST_AUTO_TEST_CASE(DependencyWaitSchedulerRejectsOverflowExpiresAndCancels)
{
  DependencyWaitScheduler scheduler(1, 1);
  std::atomic<bool> release{false};
  auto blockingTask = [&release] (const DependencyWaitControl& control) {
    while (!release.load()) {
      if (control.isCancelled()) return DependencyWaitStatus::Cancelled;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return DependencyWaitStatus::Completed;
  };
  auto ignore = [] (const DependencyWaitResult&) {};

  BOOST_REQUIRE_EQUAL(scheduler.submit(
    "active", std::chrono::steady_clock::now() + std::chrono::seconds(5),
    blockingTask, ignore), DependencyWaitSubmitResult::Accepted);
  while (scheduler.snapshot().activeCount == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE_EQUAL(scheduler.submit(
    "queued", std::chrono::steady_clock::now() + std::chrono::seconds(5),
    blockingTask, ignore), DependencyWaitSubmitResult::Accepted);
  BOOST_CHECK_EQUAL(scheduler.submit(
    "overflow", std::chrono::steady_clock::now() + std::chrono::seconds(5),
    blockingTask, ignore), DependencyWaitSubmitResult::QueueFull);

  BOOST_CHECK(scheduler.cancel("active"));
  BOOST_CHECK(scheduler.cancel("queued"));
  BOOST_REQUIRE(scheduler.waitForIdle(std::chrono::seconds(2)));
  auto cancelled = scheduler.snapshot();
  BOOST_CHECK_EQUAL(cancelled.cancelled, 2);
  BOOST_CHECK_EQUAL(cancelled.rejected, 1);

  std::promise<DependencyWaitResult> expiredPromise;
  auto expiredFuture = expiredPromise.get_future();
  BOOST_REQUIRE_EQUAL(scheduler.submit(
    "expired", std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
    [] (const DependencyWaitControl& control) {
      while (!control.deadlineExpired()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return DependencyWaitStatus::DeadlineExpired;
    },
    [&expiredPromise] (const DependencyWaitResult& result) {
      expiredPromise.set_value(result);
    }), DependencyWaitSubmitResult::Accepted);
  BOOST_REQUIRE(expiredFuture.wait_for(std::chrono::seconds(1)) ==
                std::future_status::ready);
  BOOST_CHECK_EQUAL(expiredFuture.get().status,
                    DependencyWaitStatus::DeadlineExpired);
  BOOST_CHECK_EQUAL(scheduler.snapshot().deadlineExpired, 1);
}

BOOST_AUTO_TEST_CASE(DependencyWaitSchedulerShutdownCancelsPendingWork)
{
  std::atomic<std::size_t> terminals{0};
  DependencyWaitScheduler scheduler(2, 16);
  for (std::size_t i = 0; i < 16; ++i) {
    BOOST_REQUIRE_EQUAL(scheduler.submit(
      "shutdown-" + std::to_string(i),
      std::chrono::steady_clock::now() + std::chrono::seconds(5),
      [] (const DependencyWaitControl& control) {
        while (!control.isCancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return DependencyWaitStatus::Cancelled;
      },
      [&terminals] (const DependencyWaitResult&) { ++terminals; }),
      DependencyWaitSubmitResult::Accepted);
  }
  scheduler.shutdown();
  const auto stopped = scheduler.snapshot();
  BOOST_CHECK(stopped.stopping);
  BOOST_CHECK_EQUAL(stopped.activeCount, 0);
  BOOST_CHECK_EQUAL(stopped.queuedCount, 0);
  BOOST_CHECK_EQUAL(terminals.load(), 16);
  BOOST_CHECK_EQUAL(scheduler.submit(
    "late", std::chrono::steady_clock::now() + std::chrono::seconds(1),
    [] (const DependencyWaitControl&) { return DependencyWaitStatus::Completed; },
    [] (const DependencyWaitResult&) {}),
    DependencyWaitSubmitResult::ShuttingDown);
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeBackendAcceptsEncodedTensorBundleInput)
{
  const auto* modelPath = std::getenv("NDNSF_DI_TEST_ONNX_MODEL");
  if (modelPath == nullptr || std::string(modelPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_TEST_ONNX_MODEL not set; skipping encoded-input ONNX smoke");
    BOOST_CHECK(true);
    return;
  }

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("NDNSF_DI_TEST_ONNX_MODEL requires C++ ONNX Runtime backend");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = factory.create(NativeModelRunnerSpec{
    "/OnnxRole",
    "onnx-model",
    "onnxruntime",
    modelPath,
    {
      {"inputNames", "x"},
      {"outputNames", "y"},
      {"outputScope", "onnx-to-user"},
    },
  });

  RoleExecutionContext ctx;
  ctx.sessionId = "onnx-runtime-encoded-smoke";
  ctx.role = "/OnnxRole";
  ctx.inputsByScope.emplace(
    "activation",
    makeEncodedTensorBundle(
      "activation",
      {makeFloat32Tensor("x", {1, 3}, floatPayload({1.0f, 2.0f, 3.0f}))}));

  const auto outputs = runner->run(ctx);
  BOOST_REQUIRE(outputs.count("onnx-to-user") == 1);
  const auto floats = payloadFloats(outputs.at("onnx-to-user"));
  BOOST_REQUIRE_EQUAL(floats.size(), 3);
  BOOST_CHECK_CLOSE(floats[0], 2.0f, 0.001);
  BOOST_CHECK_CLOSE(floats[1], 3.0f, 0.001);
  BOOST_CHECK_CLOSE(floats[2], 4.0f, 0.001);
#endif
}

BOOST_AUTO_TEST_CASE(OnnxRuntimeBackendProducesEncodedMultiOutputBundle)
{
  const auto* modelPath = std::getenv("NDNSF_DI_TEST_ONNX_MULTI_MODEL");
  if (modelPath == nullptr || std::string(modelPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_TEST_ONNX_MULTI_MODEL not set; skipping multi-output ONNX smoke");
    BOOST_CHECK(true);
    return;
  }

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("NDNSF_DI_TEST_ONNX_MULTI_MODEL requires C++ ONNX Runtime backend");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = factory.create(NativeModelRunnerSpec{
    "/OnnxRole",
    "onnx-model",
    "onnxruntime",
    modelPath,
    {
      {"inputNames", "x"},
      {"inputShape", "1,3"},
      {"outputNames", "y,z"},
      {"outputBundleScope", "multi-output"},
    },
  });

  RoleExecutionContext ctx;
  ctx.sessionId = "onnx-runtime-multi-output-smoke";
  ctx.role = "/OnnxRole";
  TensorBundle input;
  input.name = "x";
  input.payload = floatPayload({1.0f, 2.0f, 3.0f});
  input.expectedBytes = input.payload.size();
  ctx.inputsByScope.emplace("x", std::move(input));

  const auto outputs = runner->run(ctx);
  BOOST_REQUIRE(outputs.count("multi-output") == 1);
  BOOST_CHECK(isEncodedTensorBundle(outputs.at("multi-output").payload));
  const auto tensors = decodeTensorBundle(outputs.at("multi-output").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 2);
  TensorBundle y;
  y.name = "y";
  y.payload = findTensor(tensors, "y").payload;
  TensorBundle z;
  z.name = "z";
  z.payload = findTensor(tensors, "z").payload;
  const auto yValues = payloadFloats(y);
  const auto zValues = payloadFloats(z);
  BOOST_REQUIRE_EQUAL(yValues.size(), 3);
  BOOST_REQUIRE_EQUAL(zValues.size(), 3);
  BOOST_CHECK_CLOSE(yValues[0], 2.0f, 0.001);
  BOOST_CHECK_CLOSE(zValues[0], 3.0f, 0.001);
#endif
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeDispatchesRegisteredRoleRunner)
{
  RoleSpec role{
    "/RuntimeRole",
    {DependencyEdge{"input-to-runtime", "/Input", "/RuntimeRole",
                    "/run/5/input/bundle/0", 1}},
    {DependencyEdge{"runtime-to-user", "/RuntimeRole", "",
                    "/run/5/runtime/bundle/0", 1}},
  };

  auto io = std::make_shared<FakeDependencyIo>();
  NativeProviderRuntime runtime(1);
  BOOST_CHECK(!runtime.hasRunner("/RuntimeRole"));
  runtime.registerRunner(
    "/RuntimeRole",
    [] (const RoleExecutionContext& ctx) {
      BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
      return std::map<std::string, TensorBundle>{
        {"runtime-to-user", bundle("runtime-result",
                                   "runtime:" + payloadText(ctx.inputsByScope.begin()->second))},
      };
    });
  BOOST_CHECK(runtime.hasRunner("/RuntimeRole"));

  const auto result = runtime.executeRoleAsync("run-5", role, io).get();
  BOOST_REQUIRE(result.outputsByScope.count("runtime-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("runtime-to-user")),
                    "runtime:input:input-to-runtime");
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeRejectsMissingRoleRunner)
{
  NativeProviderRuntime runtime(1);
  RoleSpec role{
    "/MissingRole",
    {},
    {DependencyEdge{"missing-to-user", "/MissingRole", "",
                    "/run/6/missing/bundle/0", 1}},
  };
  auto io = std::make_shared<FakeDependencyIo>();

  BOOST_CHECK_THROW(runtime.executeRoleAsync("run-6", role, io), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(KvStateStoreBindsReplacesEvictsAndInvalidatesOnBoot)
{
  KvStateStore store(8, 2);
  store.setProviderBootId("boot-a");
  KvStateBinding first{
    "session-a", "/LLM/Stage/0", 1, "sha256:model", "sha256:plan",
    "/provider/A", "boot-a", 7, "request-a", 1, "generation-a",
    "sha256:state-schema", std::nullopt,
  };
  BOOST_CHECK(store.put(first, bundle("kv-a", "1234")));
  BOOST_REQUIRE(store.lookup(first));
  BOOST_CHECK_EQUAL(payloadText(*store.lookup(first)), "1234");

  auto wrongEpoch = first;
  wrongEpoch.contextEpoch = 2;
  BOOST_CHECK(!store.lookup(wrongEpoch));
  auto wrongPlan = first;
  wrongPlan.planDigest = "sha256:other";
  BOOST_CHECK(!store.lookup(wrongPlan));
  auto wrongSecurity = first;
  wrongSecurity.securityEpoch = 8;
  BOOST_CHECK(!store.lookup(wrongSecurity));

  BOOST_CHECK(store.put(first, bundle("kv-a-new", "5678")));
  BOOST_CHECK_EQUAL(store.size(), 1);
  BOOST_CHECK_EQUAL(payloadText(*store.lookup(first)), "5678");

  auto second = first;
  second.sessionId = "session-b";
  BOOST_CHECK(store.put(second, bundle("kv-b", "abcd")));
  BOOST_REQUIRE(store.lookup(first)); // make first most recently used
  auto third = first;
  third.sessionId = "session-c";
  BOOST_CHECK(store.put(third, bundle("kv-c", "WXYZ")));
  BOOST_CHECK(store.lookup(first));
  BOOST_CHECK(!store.lookup(second));
  BOOST_CHECK(store.lookup(third));
  BOOST_CHECK_EQUAL(store.usedBytes(), 8);

  store.setProviderBootId("boot-b");
  BOOST_CHECK_EQUAL(store.size(), 0);
  BOOST_CHECK_EQUAL(store.usedBytes(), 0);
  BOOST_CHECK(!store.put(first, bundle("old-boot", "1234")));
  auto newBoot = first;
  newBoot.providerBootId = "boot-b";
  BOOST_CHECK(store.put(newBoot, bundle("new-boot", "1234")));
  BOOST_CHECK(store.erase(newBoot.sessionId, newBoot.stage));
  BOOST_CHECK_EQUAL(store.size(), 0);

  BOOST_CHECK(!store.put(newBoot, bundle("too-large", "123456789")));
}

BOOST_AUTO_TEST_CASE(KvStateStoreRequiresCompleteIdentityForExactReuse)
{
  KvStateStore store(1024, 2);
  store.setProviderBootId("boot-a");
  KvStateBinding binding{
    "session-a", "/Stage/0", 0, stateDigest('1'), stateDigest('0'),
    "/provider/A", "boot-a", 7, "request-a", 1, "generation-a",
    stateDigest('b'), exactStateIdentity(),
  };
  BOOST_REQUIRE(store.put(binding, bundle("state", "AAAA")));
  BOOST_REQUIRE(store.lookup(binding));

  const auto rejectMutation = [&] (const auto& mutate) {
    auto changed = binding;
    mutate(*changed.exactIdentity);
    try {
      BOOST_CHECK(!store.lookup(changed));
    }
    catch (const std::invalid_argument&) {
      // A compact/full-identity disagreement is rejected during validation;
      // an unmirrored full-identity change is rejected by exact comparison.
    }
  };
  rejectMutation([] (auto& value) { value.modelDigest = stateDigest('2'); });
  rejectMutation([] (auto& value) { value.graphSemanticDigest = stateDigest('3'); });
  rejectMutation([] (auto& value) { value.artifactDigest = stateDigest('4'); });
  rejectMutation([] (auto& value) { value.adapterDigest = stateDigest('5'); });
  rejectMutation([] (auto& value) { value.tokenizerDigest = stateDigest('6'); });
  rejectMutation([] (auto& value) { value.runnerDigest = stateDigest('7'); });
  rejectMutation([] (auto& value) { value.roleName = "/Stage/1"; });
  rejectMutation([] (auto& value) { value.roleSplitDigest = stateDigest('8'); });
  rejectMutation([] (auto& value) { value.layerEnd += 1; });
  rejectMutation([] (auto& value) { value.prefixDigest = stateDigest('9'); });
  rejectMutation([] (auto& value) { value.prefixTokenCount += 1; });
  rejectMutation([] (auto& value) { value.positionDigest = stateDigest('a'); });
  rejectMutation([] (auto& value) { value.precision = "fp16"; });
  rejectMutation([] (auto& value) { value.layoutDigest = stateDigest('0'); });
  rejectMutation([] (auto& value) { value.stateSchemaDigest = stateDigest('c'); });
  rejectMutation([] (auto& value) {
    value.stateComponentDigests.front() = stateDigest('0');
  });
  rejectMutation([] (auto& value) { value.runtimeAbiDigest = stateDigest('0'); });
  rejectMutation([] (auto& value) { value.securityDomainDigest = stateDigest('0'); });
  rejectMutation([] (auto& value) { value.providerIdentity = "/provider/B"; });
  rejectMutation([] (auto& value) { value.providerBootId = "boot-b"; });
  rejectMutation([] (auto& value) { value.cacheEpoch += 1; });
  rejectMutation([] (auto& value) { value.stateInferenceEpoch += 1; });
  rejectMutation([] (auto& value) { value.predecessorInferenceEpoch = 0; });
  rejectMutation([] (auto& value) { value.requestId = "request-b"; });
  rejectMutation([] (auto& value) { value.attemptEpoch = 2; });
  rejectMutation([] (auto& value) { value.generationId = "generation-b"; });

  BOOST_REQUIRE(store.lookup(binding));
  BOOST_CHECK_EQUAL(payloadText(*store.lookup(binding)), "AAAA");
}

BOOST_AUTO_TEST_CASE(KvStateStorePinsAndAtomicallyRollsBackCandidates)
{
  KvStateStore store(12, 2);
  store.setProviderBootId("boot-a");
  KvStateBinding first{
    "session-a", "/Stage/0", 0, "sha256:model", "sha256:plan",
    "/provider/A", "boot-a", 7, "request-a", 1, "generation-a",
    "sha256:state-schema", std::nullopt,
  };
  auto second = first;
  second.sessionId = "session-b";
  second.requestId = "request-b";
  second.generationId = "generation-b";
  BOOST_REQUIRE(store.put(first, bundle("state-a", "AAAA")));
  BOOST_REQUIRE(store.put(second, bundle("state-b", "BBBB")));

  BOOST_REQUIRE(store.beginTransition(first));
  auto candidate = first;
  candidate.contextEpoch = 1;
  BOOST_REQUIRE(store.stageCandidate(
    first, candidate, bundle("candidate-a", "CCCC")));
  BOOST_CHECK_EQUAL(store.pinnedCount(), 1);
  BOOST_CHECK_EQUAL(store.candidateCount(), 1);

  auto third = first;
  third.sessionId = "session-c";
  third.requestId = "request-c";
  third.generationId = "generation-c";
  BOOST_REQUIRE(store.put(third, bundle("state-c", "DDDD")));
  BOOST_CHECK(store.lookup(first));
  BOOST_CHECK(!store.lookup(second));
  BOOST_CHECK(store.lookup(third));
  BOOST_CHECK_EQUAL(store.evictionCount(), 1);

  BOOST_REQUIRE(store.rollbackTransition(candidate));
  BOOST_CHECK_EQUAL(payloadText(*store.lookup(first)), "AAAA");
  BOOST_CHECK(!store.lookup(candidate));
  BOOST_CHECK_EQUAL(store.pinnedCount(), 0);
  BOOST_CHECK_EQUAL(store.candidateCount(), 0);

  BOOST_REQUIRE(store.beginTransition(first));
  BOOST_REQUIRE(store.stageCandidate(
    first, candidate, bundle("candidate-a", "EEEE")));
  BOOST_REQUIRE(store.commitCandidate(candidate));
  BOOST_CHECK(!store.lookup(first));
  BOOST_CHECK_EQUAL(payloadText(*store.lookup(candidate)), "EEEE");
  BOOST_CHECK_EQUAL(store.pinnedCount(), 0);
  BOOST_CHECK_EQUAL(store.candidateCount(), 0);
}

BOOST_AUTO_TEST_CASE(ConversationStateStoreSeparatesCrossRequestState)
{
  ConversationStateStore store(4, 1, 4, 1, 1'000);
  store.setProviderBinding("/provider/A", "boot-a", 7);
  const auto origin = exactStateIdentity();
  const auto binding = exactConversationBinding(origin, 1, 10'000);

  BOOST_REQUIRE(store.stagePromotion(
    origin.requestId, origin.roleName, binding, bundle("state", "AAAA"), 100));
  BOOST_REQUIRE(store.commitStagedPromotion(binding, stateDigest('8')));
  auto snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 1U);
  BOOST_CHECK_EQUAL(snapshot.gpuEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.hostEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.promotions, 1U);

  // A resumed Request has fresh request/generation authority.  Those origin
  // fields are audited but must not prevent exact conversation lookup.
  auto resumed = binding;
  resumed.identity.requestId = "request-b";
  resumed.identity.generationId = "generation-b";
  BOOST_REQUIRE(store.lookup(resumed, 200));
  BOOST_CHECK_EQUAL(store.snapshot().hits, 1U);

  auto wrongPrefix = resumed;
  wrongPrefix.identity.prefixDigest = stateDigest('0');
  BOOST_CHECK(!store.lookup(wrongPrefix, 200));
  BOOST_CHECK_EQUAL(store.snapshot().misses, 1U);

  BOOST_REQUIRE(store.pin(resumed, 250));
  BOOST_CHECK_EQUAL(store.snapshot().pinnedEntries, 1U);
  BOOST_CHECK(!store.pauseToHost(resumed, 250));
  BOOST_REQUIRE(store.unpin(resumed));
  BOOST_REQUIRE(store.pauseToHost(resumed, 300));
  snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.gpuEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.hostEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.hostBytes, 4U);

  // Two callers attempting the same host restore share one in-flight
  // transition; the second call is rejected rather than duplicating work.
  // The transfer is asynchronous, so a very fast first transfer may already
  // have completed by the time the duplicate call is made; in that case the
  // idempotent GPU-resident result is also valid.
  auto first = store.prefetchToGpu(resumed, 350);
  auto duplicate = store.prefetchToGpu(resumed, 350);
  const auto duplicateResult = duplicate.get();
  BOOST_CHECK(first.get());
  snapshot = store.snapshot();
  // The duplicate may either observe PREFETCHING and return false, or arrive
  // after the first transfer and return the idempotent GPU-resident result.
  // Do not inspect the transient lifecycle after duplicate.get(): the first
  // worker may have changed PREFETCHING to IDLE between those operations.
  BOOST_CHECK(duplicateResult || snapshot.gpuEntries == 1U);
  BOOST_CHECK_EQUAL(snapshot.gpuEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.hostEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.prefetchedEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.prefetchBytes, 4U);

  BOOST_REQUIRE(store.release(resumed));
  snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 0U);
  BOOST_CHECK_EQUAL(snapshot.cleanups, 1U);

  // Provider boot/cache changes invalidate every retained conversation entry.
  BOOST_REQUIRE(store.promote(
    origin.requestId, origin.roleName, binding, bundle("state", "BBBB"), 400));
  store.setProviderBinding("/provider/A", "boot-b", 8);
  snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 0U);
  BOOST_CHECK_EQUAL(snapshot.cleanups, 2U);
  BOOST_CHECK(!store.lookup(binding, 500));
}

BOOST_AUTO_TEST_CASE(ConversationStatePromotionIsInvisibleUntilAggregateCommit)
{
  ConversationStateStore store(16, 2, 16, 2, 1'000);
  store.setProviderBinding("/provider/A", "boot-a", 7);
  const auto origin = exactStateIdentity();
  const auto binding = exactConversationBinding(origin, 1, 10'000);

  BOOST_REQUIRE(store.stagePromotion(
    origin.requestId, origin.roleName, binding, bundle("state", "AAAA"), 100));
  auto snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 1U);
  BOOST_CHECK_EQUAL(snapshot.committingEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.stagedPromotions, 1U);
  BOOST_CHECK_EQUAL(snapshot.promotions, 0U);
  BOOST_CHECK(!store.lookup(binding, 110));

  ConversationStateReferenceV1 reference;
  reference.conversationId = binding.conversationId;
  reference.contextEpoch = binding.contextEpoch;
  reference.serviceName = binding.serviceName;
  reference.planRoleMapDigest = binding.planRoleMapDigest;
  reference.checkpointDigest = stateDigest('8');
  reference.roleName = binding.identity.roleName;
  reference.roleReceiptDigest = binding.receiptDigest;
  reference.expiresAtMs = binding.expiresAtMs;
  BOOST_CHECK(!store.resolve(reference, 110));

  BOOST_REQUIRE(store.commitStagedPromotion(binding));
  snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.committingEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.promotions, 1U);
  BOOST_REQUIRE(store.lookup(binding, 120));

  auto second = binding;
  second.contextEpoch = 2;
  second.receiptDigest = stateDigest('9');
  BOOST_REQUIRE(store.stagePromotion(
    origin.requestId, origin.roleName, second, bundle("state", "BBBB"), 130));
  BOOST_REQUIRE(store.rollbackStagedPromotion(second));
  snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 1U);
  BOOST_CHECK_EQUAL(snapshot.committingEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.promotionRollbacks, 1U);
  BOOST_CHECK(!store.lookup(second, 140));
  BOOST_REQUIRE(store.lookup(binding, 140));
}

BOOST_AUTO_TEST_CASE(ConversationStateStoreCleansExpiredStagedPromotion)
{
  ConversationStateStore store(16, 2, 16, 2, 1'000);
  store.setProviderBinding("/provider/A", "boot-a", 7);
  const auto identity = exactStateIdentity();
  auto binding = exactConversationBinding(identity, 1, 150);
  BOOST_REQUIRE(store.stagePromotion(
    identity.requestId, identity.roleName, binding,
    bundle("state", "AAAA"), 100));
  BOOST_CHECK_EQUAL(store.snapshot().committingEntries, 1U);
  BOOST_CHECK_EQUAL(store.cleanupExpired(150), 1U);
  const auto snapshot = store.snapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 0U);
  BOOST_CHECK_EQUAL(snapshot.committingEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.cleanups, 1U);
  BOOST_CHECK(!store.lookup(binding, 151));
}

BOOST_AUTO_TEST_CASE(ProviderConversationReceiptMatchesPythonCanonicalDigest)
{
  const auto identity = exactStateIdentity();
  ProviderConversationStateReceiptV1 receipt;
  receipt.conversationId = "conversation-a-0001";
  receipt.parentContextEpoch = 0;
  receipt.successorContextEpoch = 1;
  receipt.originRequestId = identity.requestId;
  receipt.originGenerationId = identity.generationId;
  receipt.serviceName = "/LLM/Pipeline/Generate";
  receipt.requesterIdentity = "/requester/A";
  receipt.securityDomainDigest = identity.securityDomainDigest;
  receipt.modelDigest = identity.modelDigest;
  receipt.graphSemanticDigest = identity.graphSemanticDigest;
  receipt.adapterDigest = identity.adapterDigest;
  receipt.roleName = identity.roleName;
  receipt.roleSplitDigest = identity.roleSplitDigest;
  receipt.layoutDigest = identity.layoutDigest;
  receipt.planRoleMapDigest = stateDigest('0');
  receipt.providerIdentity = identity.providerIdentity;
  receipt.providerBootId = identity.providerBootId;
  receipt.cacheEpoch = identity.cacheEpoch;
  receipt.prefixDigest = identity.prefixDigest;
  receipt.prefixTokenCount = identity.prefixTokenCount;
  receipt.positionDigest = identity.positionDigest;
  receipt.stateSchemaDigest = identity.stateSchemaDigest;
  receipt.stateComponentDigests = identity.stateComponentDigests;
  receipt.expiresAtMs = 10'000;

  BOOST_CHECK_EQUAL(
    receipt.computedDigest(),
    "sha256:e3351841b1ad9ef264fdb28a9890224b965513a09640d1cfed8ce786dcf53bdf");
  const auto wire = receipt.toJson();
  BOOST_CHECK(wire.find("\"receiptDigest\":\"" + receipt.computedDigest() +
                        "\"") != std::string::npos);
  BOOST_CHECK(wire.find("\"signature\":\"\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ConversationStateStoreResolvesOnlyExactSelectionCommitment)
{
  ConversationStateStore store(16, 2, 16, 2, 1'000);
  store.setProviderBinding("/provider/A", "boot-a", 7);
  const auto origin = exactStateIdentity();
  const auto binding = exactConversationBinding(origin, 1, 10'000);
  BOOST_REQUIRE(store.stagePromotion(
    origin.requestId, origin.roleName, binding, bundle("state", "AAAA"), 100));
  BOOST_REQUIRE(store.commitStagedPromotion(binding, stateDigest('8')));

  // A caller cannot smuggle a conflicting aggregate checkpoint through the
  // binding argument while supplying a different final checkpoint digest.
  auto conflictingBinding = binding;
  conflictingBinding.checkpointDigest = stateDigest('7');
  BOOST_CHECK(!store.commitStagedPromotion(conflictingBinding, stateDigest('8')));

  ConversationStateReferenceV1 reference;
  reference.conversationId = binding.conversationId;
  reference.contextEpoch = binding.contextEpoch;
  reference.serviceName = binding.serviceName;
  reference.planRoleMapDigest = binding.planRoleMapDigest;
  reference.checkpointDigest = stateDigest('8');
  reference.roleName = binding.identity.roleName;
  reference.roleReceiptDigest = binding.receiptDigest;
  reference.expiresAtMs = binding.expiresAtMs;
  auto resolved = store.resolve(reference, 200);
  BOOST_REQUIRE(resolved);
  auto expectedBinding = binding;
  expectedBinding.checkpointDigest = reference.checkpointDigest;
  BOOST_CHECK(*resolved == expectedBinding);

  auto wrongReceipt = reference;
  wrongReceipt.roleReceiptDigest = stateDigest('7');
  BOOST_CHECK(!store.resolve(wrongReceipt, 200));
  auto wrongCheckpoint = reference;
  wrongCheckpoint.checkpointDigest = stateDigest('7');
  BOOST_CHECK(!store.resolve(wrongCheckpoint, 200));
  auto wrongRoleMap = reference;
  wrongRoleMap.planRoleMapDigest = stateDigest('6');
  BOOST_CHECK(!store.resolve(wrongRoleMap, 200));
  BOOST_CHECK(!store.resolve(reference, 10'000));

  // The legacy commit overload cannot authenticate an aggregate checkpoint.
  // It must reject a caller that tries to attach one there instead of using
  // the explicit checkpoint-bearing overload.
  ConversationStateStore legacyStore(16, 2, 16, 2, 1'000);
  legacyStore.setProviderBinding("/provider/A", "boot-a", 7);
  BOOST_REQUIRE(legacyStore.stagePromotion(
    origin.requestId, origin.roleName, binding, bundle("state", "BBBB"), 100));
  auto forgedLegacyBinding = binding;
  forgedLegacyBinding.checkpointDigest = stateDigest('8');
  BOOST_CHECK(!legacyStore.commitStagedPromotion(forgedLegacyBinding));
  BOOST_REQUIRE(legacyStore.rollbackStagedPromotion(binding));
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeAutomaticallyReusesCommittedDecodeState)
{
  NativeProviderRuntime runtime(1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-stateful";
  runnerSpec.path = "/tmp/stage-0.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };

  std::atomic<std::size_t> calls{0};
  auto runner = makeNativeModelRunner(
    [&calls] (const RoleExecutionContext& ctx) {
      const auto call = calls.fetch_add(1);
      bool foundCommittedState = false;
      for (const auto& input : ctx.inputsByScope) {
        if (!isEncodedTensorBundle(input.second.payload)) {
          continue;
        }
        try {
          const auto& state = findTensor(
            decodeTensorBundle(input.second.payload), "attention_kv_in");
          BOOST_CHECK(state.payload == rawTensorPayload<std::int64_t>({1}));
          foundCommittedState = true;
        }
        catch (const std::out_of_range&) {
        }
      }
      BOOST_CHECK_EQUAL(foundCommittedState, call == 1);

      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>(
        {static_cast<std::int64_t>(call + 1)});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    });
  runtime.registerRunner(runnerSpec, std::move(runner));

  RoleSpec role;
  role.role = runnerSpec.role;
  role.requestId = "request-a";
  role.attemptEpoch = 1;
  role.stateInputNames = {"attention_kv_in"};
  role.stateOutputNames = {"attention_kv_out"};

  role.inferenceEpoch = 0;
  role.candidateDecodeStateIdentity = exactStateIdentity();
  runtime.executeRoleAsync(
    "session-a", role, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt")}}).get();

  role.inferenceEpoch = 1;
  role.predecessorDecodeStateIdentity = role.candidateDecodeStateIdentity;
  role.candidateDecodeStateIdentity->prefixDigest = stateDigest('0');
  ++role.candidateDecodeStateIdentity->prefixTokenCount;
  role.candidateDecodeStateIdentity->positionDigest = stateDigest('0');
  role.candidateDecodeStateIdentity->stateInferenceEpoch = 1;
  role.candidateDecodeStateIdentity->predecessorInferenceEpoch = 0;
  runtime.executeRoleAsync(
    "session-a", role, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "next-token")}}).get();

  auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(calls.load(), 2);
  BOOST_CHECK_EQUAL(state.commits, 2);
  BOOST_CHECK_EQUAL(state.hits, 1);
  BOOST_CHECK_EQUAL(state.misses, 0);

  role.inferenceEpoch = 2;
  role.predecessorDecodeStateIdentity = role.candidateDecodeStateIdentity;
  role.predecessorDecodeStateIdentity->adapterDigest = stateDigest('0');
  role.candidateDecodeStateIdentity->prefixDigest = stateDigest('2');
  ++role.candidateDecodeStateIdentity->prefixTokenCount;
  role.candidateDecodeStateIdentity->positionDigest = stateDigest('2');
  role.candidateDecodeStateIdentity->stateInferenceEpoch = 2;
  role.candidateDecodeStateIdentity->predecessorInferenceEpoch = 1;
  BOOST_CHECK_THROW(
    runtime.executeRoleAsync(
      "session-a", role, std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", "next-token-2")}}),
    std::runtime_error);
  state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(calls.load(), 2);
  BOOST_CHECK_EQUAL(state.hits, 1);
  BOOST_CHECK_EQUAL(state.misses, 1);
  BOOST_CHECK_EQUAL(state.commits, 2);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimePromotesDecodeStateAcrossRequests)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 4, 1024, 4, 1024, 4, 1'000);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([] (const RoleExecutionContext&) {
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({42});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  const auto identity = exactStateIdentity();
  RoleSpec role;
  role.role = runnerSpec.role;
  role.requestId = identity.requestId;
  role.attemptEpoch = identity.attemptEpoch;
  role.inferenceEpoch = 0;
  role.stateInputNames = {"attention_kv_in"};
  role.stateOutputNames = {"attention_kv_out"};
  role.candidateDecodeStateIdentity = identity;
  runtime.executeRoleAsync(
    "session-a", role, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt")}}).get();

  const auto binding = exactConversationBinding(identity, 1, 10'000);
  BOOST_REQUIRE(runtime.stageDecodeStatePromotion(
    "session-a", role, binding, 100));
  auto local = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(local.entries, 1U);
  auto conversation = runtime.conversationStateSnapshot();
  BOOST_CHECK_EQUAL(conversation.entries, 1U);
  BOOST_CHECK_EQUAL(conversation.committingEntries, 1U);
  BOOST_CHECK_EQUAL(conversation.promotions, 0U);
  BOOST_CHECK(!runtime.lookupConversationState(binding, 150));

  ConversationStateReferenceV1 stagedReference;
  stagedReference.conversationId = binding.conversationId;
  stagedReference.contextEpoch = binding.contextEpoch;
  stagedReference.serviceName = binding.serviceName;
  stagedReference.planRoleMapDigest = binding.planRoleMapDigest;
  stagedReference.checkpointDigest = stateDigest('8');
  stagedReference.roleName = binding.identity.roleName;
  stagedReference.roleReceiptDigest = binding.receiptDigest;
  stagedReference.expiresAtMs = binding.expiresAtMs;
  BOOST_REQUIRE(runtime.resolveStagedConversationState(stagedReference, 150));
  BOOST_CHECK(!runtime.resolveConversationState(stagedReference, 150));

  BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(binding));
  local = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(local.entries, 0U);
  conversation = runtime.conversationStateSnapshot();
  BOOST_CHECK_EQUAL(conversation.entries, 1U);
  BOOST_CHECK_EQUAL(conversation.committingEntries, 0U);
  BOOST_CHECK_EQUAL(conversation.promotions, 1U);
  BOOST_CHECK_EQUAL(conversation.requestLocalReleases, 1U);

  auto resumed = binding;
  resumed.identity.requestId = "request-b";
  resumed.identity.generationId = "generation-b";
  auto restored = runtime.lookupConversationState(resumed, 200);
  BOOST_REQUIRE(restored);
  const auto& restoredTensor = findTensor(
    decodeTensorBundle(restored->payload), "attention_kv_in");
  BOOST_CHECK(restoredTensor.payload == rawTensorPayload<std::int64_t>({42}));
  BOOST_REQUIRE(runtime.pauseConversationStateToHost(resumed, 300));
  BOOST_REQUIRE(runtime.prefetchConversationStateToGpu(resumed, 400).get());
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().prefetchedEntries, 1U);

  // A later Request cannot accidentally re-enter the request-local store by
  // using the old request identity; it must explicitly restore conversation
  // state first.
  auto later = role;
  later.requestId = "request-b";
  later.inferenceEpoch = 1;
  later.predecessorDecodeStateIdentity = identity;
  later.candidateDecodeStateIdentity = identity;
  later.candidateDecodeStateIdentity->requestId = "request-b";
  later.candidateDecodeStateIdentity->generationId = "generation-b";
  later.candidateDecodeStateIdentity->stateInferenceEpoch = 1;
  later.candidateDecodeStateIdentity->predecessorInferenceEpoch = 0;
  BOOST_CHECK_EXCEPTION(
    runtime.executeRoleAsync(
      "session-b", later, std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", "next")}}),
    std::exception,
    [] (const std::exception& error) {
      // The old request identity is rejected before any runner call.  Depending
      // on which exact compact/full binding check fires, this is either the
      // explicit missing-state error or the equivalent fail-closed mismatch.
      return std::string(error.what()) == "PROVIDER_DECODE_STATE_MISSING" ||
             std::string(error.what()).find("compact binding") !=
               std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeUsesAdapterOwnedConversationTransfers)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 4, 1024, 4, 1024, 4, 1'000);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };
  auto transferRunner = std::make_shared<ConversationTransferRunner>();
  runtime.registerRunner(runnerSpec, transferRunner);

  const auto origin = exactStateIdentity();
  RoleSpec first;
  first.role = runnerSpec.role;
  first.requestId = origin.requestId;
  first.attemptEpoch = origin.attemptEpoch;
  first.stateInputNames = {"attention_kv_in"};
  first.stateOutputNames = {"attention_kv_out"};
  first.candidateDecodeStateIdentity = origin;
  BOOST_REQUIRE(runtime.executeRoleAsync(
    "session-a", first, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt")}}).get().outputsByScope.size() == 1);

  const auto binding = exactConversationBinding(origin, 1, 10'000);
  BOOST_REQUIRE(runtime.promoteDecodeStateToConversation(
    "session-a", first, binding, 100));
  BOOST_CHECK_EQUAL(transferRunner->counters.promote.load(), 1);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().gpuBytes,
                    sizeof(std::int64_t));
  BOOST_CHECK(!runtime.lookupConversationState(binding, 150));

  BOOST_REQUIRE(runtime.pauseConversationStateToHost(binding, 200));
  BOOST_CHECK_EQUAL(transferRunner->counters.pause.load(), 1);
  auto snapshot = runtime.conversationStateSnapshot();
  BOOST_CHECK_EQUAL(snapshot.gpuEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.hostEntries, 1U);

  BOOST_REQUIRE(runtime.prefetchConversationStateToGpu(binding, 300).get());
  BOOST_CHECK_EQUAL(transferRunner->counters.prefetch.load(), 1);
  snapshot = runtime.conversationStateSnapshot();
  BOOST_CHECK_EQUAL(snapshot.gpuEntries, 1U);
  BOOST_CHECK_EQUAL(snapshot.hostEntries, 0U);

  auto resumed = binding;
  resumed.identity.requestId = "request-b";
  resumed.identity.generationId = "generation-b";
  ++resumed.identity.prefixTokenCount;
  resumed.identity.positionDigest = stateDigest('a');
  auto resumedRole = first;
  resumedRole.requestId = resumed.identity.requestId;
  resumedRole.candidateDecodeStateIdentity = resumed.identity;
  // The binding names the committed parent state; only the candidate request
  // identity is fresh for the continuation turn.
  resumedRole.conversationStateBinding = binding;
  resumedRole.conversationStateLookupNowMs = 400;
  const auto resumedResult = runtime.executeRoleAsync(
    "session-b", resumedRole, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "continuation")}}).get();
  BOOST_CHECK_EQUAL(resumedResult.outputsByScope.size(), 1U);
  BOOST_CHECK_EQUAL(transferRunner->counters.restore.load(), 1);
  BOOST_CHECK(transferRunner->restoredWithoutBundle.load());
  BOOST_CHECK_EQUAL(transferRunner->restoredSession, "session-b");

  BOOST_REQUIRE(runtime.releaseConversationState(binding));
  BOOST_CHECK_EQUAL(transferRunner->counters.release.load(), 1);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimePromotesThePreparedSelectionRunner)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 4, 1024, 4, 1024, 4, 1'000);
  auto transferRunner = std::make_shared<ConversationTransferRunner>();
  const auto origin = exactStateIdentity();
  RoleSpec first;
  first.role = origin.roleName;
  first.requestId = origin.requestId;
  first.attemptEpoch = origin.attemptEpoch;
  first.stateInputNames = {"attention_kv_in"};
  first.stateOutputNames = {"attention_kv_out"};
  first.candidateDecodeStateIdentity = origin;
  GenerationEpochLineageV1 lineage;
  lineage.requestId = origin.requestId;
  lineage.attemptEpoch = origin.attemptEpoch;
  lineage.planDigest = stateDigest('0');
  lineage.generationId = origin.generationId;
  lineage.streamEpoch = 1;
  lineage.inferenceEpoch = 0;
  lineage.transitionKind = GenerationEpochLineageV1::PREFILL;
  lineage.logicalPrefixDigest = origin.prefixDigest;
  lineage.logicalPrefixTokenCount = origin.prefixTokenCount;
  lineage.positionDigest = origin.positionDigest;
  lineage.producerRole = origin.roleName;
  lineage.consumerRole = origin.roleName;
  first.generationLineage = lineage;

  const auto result = runtime.executePreparedRoleAsync(
    "session-a", first, std::make_shared<FakeDependencyIo>(),
    [transferRunner] { return transferRunner; },
    {{"input_ids", bundle("input_ids", "prompt")}}).get();
  BOOST_REQUIRE(result.runner == transferRunner);

  const auto binding = exactConversationBinding(origin, 1, 10'000);
  BOOST_REQUIRE(runtime.promoteDecodeStateToConversation(
    "session-a", first, binding, 100));
  BOOST_CHECK_EQUAL(transferRunner->counters.promote.load(), 1);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().gpuBytes,
                    sizeof(std::int64_t));

  auto resumed = first;
  resumed.requestId = "request-b";
  auto resumedIdentity = exactStateIdentity(
    "request-b", 1, "generation-b");
  resumedIdentity.prefixTokenCount = origin.prefixTokenCount + 1;
  resumedIdentity.prefixDigest = stateDigest('0');
  resumedIdentity.positionDigest = stateDigest('a');
  resumedIdentity.validate();
  resumed.candidateDecodeStateIdentity = resumedIdentity;
  auto resumedLineage = lineage;
  resumedLineage.requestId = resumedIdentity.requestId;
  resumedLineage.generationId = resumedIdentity.generationId;
  resumedLineage.logicalPrefixDigest = resumedIdentity.prefixDigest;
  resumedLineage.logicalPrefixTokenCount = resumedIdentity.prefixTokenCount;
  resumedLineage.positionDigest = resumedIdentity.positionDigest;
  resumed.generationLineage = resumedLineage;
  resumed.conversationStateBinding = binding;
  resumed.conversationStateLookupNowMs = 200;
  std::atomic<std::size_t> preparationCalls{0};
  const auto resumedResult = runtime.executePreparedRoleAsync(
    "session-b", resumed, std::make_shared<FakeDependencyIo>(),
    [&preparationCalls, transferRunner] {
      ++preparationCalls;
      return transferRunner;
    },
    {{"input_ids", bundle("input_ids", "continuation")}}).get();
  BOOST_REQUIRE(resumedResult.runner == transferRunner);
  BOOST_CHECK_EQUAL(preparationCalls.load(), 0U);
  BOOST_CHECK_EQUAL(transferRunner->counters.restore.load(), 1);
  BOOST_REQUIRE(runtime.releaseDecodeState("session-b", resumed.role));
  BOOST_REQUIRE(runtime.releaseConversationState(binding));
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeRestoresConversationStateForFreshRequest)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 4, 1024, 4, 1024, 4, 1'000);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };

  std::atomic<std::size_t> calls{0};
  std::atomic<bool> restored{false};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls, &restored] (const RoleExecutionContext& ctx) {
      const auto call = calls.fetch_add(1);
      if (call == 1) {
        const auto found = ctx.inputsByScope.find(
          "__ndnsf_provider_decode_state");
        BOOST_REQUIRE(found != ctx.inputsByScope.end());
        BOOST_REQUIRE(isEncodedTensorBundle(found->second.payload));
        const auto& state = findTensor(
          decodeTensorBundle(found->second.payload), "attention_kv_in");
        BOOST_CHECK(state.payload == rawTensorPayload<std::int64_t>({42}));
        restored.store(true);
      }
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({42});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  const auto origin = exactStateIdentity();
  RoleSpec first;
  first.role = runnerSpec.role;
  first.requestId = origin.requestId;
  first.attemptEpoch = origin.attemptEpoch;
  first.stateInputNames = {"attention_kv_in"};
  first.stateOutputNames = {"attention_kv_out"};
  first.candidateDecodeStateIdentity = origin;
  runtime.executeRoleAsync(
    "session-a", first, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt")}}).get();

  auto binding = exactConversationBinding(origin, 1, 10'000);
  BOOST_REQUIRE(runtime.promoteDecodeStateToConversation(
    "session-a", first, binding, 100));

  RoleSpec resumed = first;
  resumed.requestId = "request-b";
  auto resumedIdentity = exactStateIdentity(
    "request-b", 1, "generation-b");
  // A resumed request executes only the appended suffix, but its candidate
  // identity commits the complete successor prefix.  The retained parent
  // represents three tokens and this fixture supplies one delta token.
  resumedIdentity.prefixTokenCount = binding.identity.prefixTokenCount + 1;
  resumedIdentity.prefixDigest = stateDigest('4');
  resumedIdentity.positionDigest = stateDigest('5');
  resumedIdentity.validate();
  resumed.candidateDecodeStateIdentity = resumedIdentity;
  resumed.conversationStateBinding = binding;
  resumed.conversationStateLookupNowMs = 200;
  runtime.executeRoleAsync(
    "session-b", resumed, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "delta")}}).get();

  BOOST_CHECK_EQUAL(calls.load(), 2U);
  BOOST_CHECK(restored.load());
  const auto conversation = runtime.conversationStateSnapshot();
  BOOST_CHECK_EQUAL(conversation.hits, 1U);
  BOOST_CHECK_EQUAL(conversation.misses, 0U);
  BOOST_CHECK_EQUAL(runtime.decodeStateSnapshot().commits, 2U);

  auto zeroGrowth = resumed;
  zeroGrowth.requestId = "request-zero-growth";
  auto zeroGrowthIdentity = exactStateIdentity(
    zeroGrowth.requestId, 1, "generation-zero-growth");
  zeroGrowthIdentity.prefixTokenCount = binding.identity.prefixTokenCount;
  zeroGrowthIdentity.validate();
  zeroGrowth.candidateDecodeStateIdentity = zeroGrowthIdentity;
  BOOST_CHECK_EXCEPTION(
    runtime.executeRoleAsync(
      "session-zero-growth", zeroGrowth,
      std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", "delta")}}).get(),
    std::logic_error,
    [] (const std::logic_error& error) {
      return std::string(error.what()) ==
        "Provider generation input token count is not a strict prefix extension";
    });
  BOOST_CHECK_EQUAL(calls.load(), 2U);

  auto missing = resumed;
  missing.conversationStateBinding->contextEpoch = 2;
  BOOST_CHECK_EXCEPTION(
    runtime.executeRoleAsync(
      "session-c", missing, std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", "delta-2")}}),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "PROVIDER_CONVERSATION_STATE_MISSING";
    });
  BOOST_CHECK_EQUAL(calls.load(), 2U);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRestoresConversationStateAndExtendsPrefix)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 4, 1024, 4, 1024, 4, 1'000);
  const auto origin = exactStateIdentity();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = origin.roleName;
  runnerSpec.metadata = {
    {"evidence.modelDigest", origin.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", origin.providerIdentity},
    {"evidence.providerBootId", origin.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", origin.generationId},
    {"state.schemaDigest", origin.stateSchemaDigest},
  };

  std::atomic<std::size_t> calls{0};
  std::atomic<bool> restored{false};
  auto runner = makeNativeModelRunner(
    [&calls, &restored] (const RoleExecutionContext& ctx) {
      const auto call = calls.fetch_add(1);
      if (call == 1) {
        const auto found = ctx.inputsByScope.find(
          "__ndnsf_provider_decode_state");
        BOOST_REQUIRE(found != ctx.inputsByScope.end());
        const auto& state = findTensor(
          decodeTensorBundle(found->second.payload), "attention_kv_in");
        BOOST_CHECK(state.payload == rawTensorPayload<std::int64_t>({42}));
        restored.store(true);
      }
      NamedTensor logits;
      logits.name = "logits";
      logits.elementType = TensorElementType::Float32;
      logits.shape = {1, 2};
      logits.payload = floatPayload({0.0f, 1.0f});
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({42});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle",
                                 {std::move(logits), std::move(state)})},
      };
    });
  runtime.registerRunner(runnerSpec, runner);

  RoleSpec first;
  first.role = runnerSpec.role;
  first.requestId = origin.requestId;
  first.attemptEpoch = origin.attemptEpoch;
  first.stateInputNames = {"attention_kv_in"};
  first.stateOutputNames = {"attention_kv_out"};
  first.candidateDecodeStateIdentity = origin;
  auto io = std::make_shared<FakeDependencyIo>();
  runtime.executeRoleAsync(
    "session-a", first, io,
    {{"input_ids", makeEncodedTensorBundle(
      "prompt", {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                               rawTensorPayload<std::int64_t>({1, 2, 3})}})}})
    .get();
  const auto binding = exactConversationBinding(origin, 1, 10'000);
  BOOST_REQUIRE(runtime.promoteDecodeStateToConversation(
    "session-a", first, binding, 100));

  NativeExecutionPlan plan;
  plan.roles = {runnerSpec.role};
  NativeDependencySpec feedback;
  feedback.producers = {runnerSpec.role};
  feedback.consumers = {runnerSpec.role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  plan.dependencies = {feedback};
  NativeProviderAssignment assignment;
  assignment.providerByRole[runnerSpec.role] = origin.providerIdentity;

  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "session-b";
  config.requestId = "request-b";
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = origin.providerIdentity;
  config.role = runnerSpec.role;
  config.initialInputs = {
    {"input_ids", makeEncodedTensorBundle(
      "delta", {NamedTensor{"input_ids", TensorElementType::Int64, {1, 2},
                              rawTensorPayload<std::int64_t>({4, 5})}})},
  };
  config.maxEpochs = 1;
  config.stateInputNames = {"attention_kv_in"};
  config.stateOutputNames = {"attention_kv_out"};
  config.stateIdentityTemplate = exactStateIdentity(
    "request-b", 1, "generation-b");
  config.positionPolicyDigest = origin.positionDigest;
  config.samplingDigest = "sha256:sampling";
  config.conversationStateBinding = binding;
  config.conversationStateLookupNowMs = 200;
  config.prepareRunner = [runner] { return runner; };
  std::size_t events = 0;
  config.eventSink = [&events] (const std::vector<std::uint8_t>&) {
    ++events;
    return true;
  };

  const auto result = runNativeEpochCoordinator(std::move(config));
  BOOST_CHECK_EQUAL(calls.load(), 2U);
  BOOST_CHECK(restored.load());
  BOOST_CHECK_EQUAL(events, 1U);
  BOOST_REQUIRE_EQUAL(result.cacheObservations.size(), 1U);
  BOOST_CHECK(result.cacheObservations.front().conversationStateHit);
  BOOST_CHECK_EQUAL(result.cacheObservations.front().actualNewInputExtent, 2U);
  BOOST_CHECK_EQUAL(result.cacheObservations.front().representedPrefixTokenCount, 5U);
  BOOST_CHECK_EQUAL(result.cacheObservations.front().prefixWorkAvoided, 3U);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().hits, 1U);
  BOOST_CHECK_EQUAL(runtime.decodeStateSnapshot().entries, 0U);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorOverlapsRunnerPreparationWithInputWait)
{
  NativeProviderRuntime runtime(1, 8);
  const auto identity = exactStateIdentity("overlap-request", 1,
                                           "overlap-generation");
  auto io = std::make_shared<BlockingDependencyIo>();
  const DependencyEdge activation(
    "activation-input", "/upstream", identity.roleName,
    "activation-input");
  auto runnerCalls = std::make_shared<std::atomic<std::size_t>>(0);
  auto runner = makeNativeModelRunner(
    [runnerCalls] (const RoleExecutionContext&) {
      runnerCalls->fetch_add(1);
      return std::map<std::string, TensorBundle>{};
    });
  std::promise<void> preparationStarted;
  auto preparationStartedFuture = preparationStarted.get_future();

  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  assignment.providerByRole[identity.roleName] = identity.providerIdentity;
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "overlap-session";
  config.requestId = identity.requestId;
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = identity.providerIdentity;
  config.role = identity.roleName;
  config.initialInputs = {
    {"input_ids", makeEncodedTensorBundle(
      "prompt", {NamedTensor{"input_ids", TensorElementType::Int64, {1, 1},
                               rawTensorPayload<std::int64_t>({11})}})}};
  config.maxEpochs = 2;
  config.stateIdentityTemplate = identity;
  config.positionPolicyDigest = identity.positionDigest;
  config.samplingDigest = stateDigest('1');
  config.roleSpecFactory = [activation, role = identity.roleName]
    (std::size_t) {
      return RoleSpec(role, {activation}, {});
    };
  config.prepareRunner = [runner, &preparationStarted] {
    preparationStarted.set_value();
    return runner;
  };
  config.stopCheck = [runnerCalls] {
    return runnerCalls->load() > 0
      ? std::optional<NativeEpochStopReason>{NativeEpochStopReason::Cancelled}
      : std::nullopt;
  };

  auto coordination = std::async(
    std::launch::async,
    [config = std::move(config)] () mutable {
      return runNativeEpochCoordinator(std::move(config));
    });
  BOOST_REQUIRE(preparationStartedFuture.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready);

  io->publishOutput(
    "overlap-session", activation,
    makeEncodedTensorBundle(
      "activation-input",
      {NamedTensor{"activation", TensorElementType::Float32, {1, 1},
                   rawTensorPayload<float>({1.0f})}}));
  BOOST_CHECK_EXCEPTION(
    coordination.get(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "ATTEMPT_CANCELLED";
    });
  BOOST_CHECK_EQUAL(runnerCalls->load(), 1U);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeRejectsEveryPredecessorIdentityMutation)
{
  NativeProviderRuntime runtime(1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };

  std::atomic<std::size_t> calls{0};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls] (const RoleExecutionContext&) {
      ++calls;
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({1});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  RoleSpec prefill;
  prefill.role = runnerSpec.role;
  prefill.requestId = "request-a";
  prefill.attemptEpoch = 1;
  prefill.inferenceEpoch = 0;
  prefill.stateInputNames = {"attention_kv_in"};
  prefill.stateOutputNames = {"attention_kv_out"};
  prefill.candidateDecodeStateIdentity = exactStateIdentity();
  runtime.executeRoleAsync(
    "session-a", prefill, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt")}}).get();
  BOOST_REQUIRE_EQUAL(calls.load(), 1U);

  RoleSpec decode = prefill;
  decode.inferenceEpoch = 1;
  decode.predecessorDecodeStateIdentity = prefill.candidateDecodeStateIdentity;
  decode.candidateDecodeStateIdentity->prefixDigest = stateDigest('0');
  ++decode.candidateDecodeStateIdentity->prefixTokenCount;
  decode.candidateDecodeStateIdentity->positionDigest = stateDigest('0');
  decode.candidateDecodeStateIdentity->stateInferenceEpoch = 1;
  decode.candidateDecodeStateIdentity->predecessorInferenceEpoch = 0;

  using Mutation = std::function<void(DecodeStateIdentityV1&)>;
  const std::vector<std::pair<std::string, Mutation>> mutations{
    {"modelDigest", [] (auto& v) { v.modelDigest = stateDigest('2'); }},
    {"graphSemanticDigest", [] (auto& v) { v.graphSemanticDigest = stateDigest('3'); }},
    {"artifactDigest", [] (auto& v) { v.artifactDigest = stateDigest('4'); }},
    {"adapterDigest", [] (auto& v) { v.adapterDigest = stateDigest('5'); }},
    {"tokenizerDigest", [] (auto& v) { v.tokenizerDigest = stateDigest('6'); }},
    {"runnerDigest", [] (auto& v) { v.runnerDigest = stateDigest('7'); }},
    {"roleName", [] (auto& v) { v.roleName = "/Stage/1"; }},
    {"roleSplitDigest", [] (auto& v) { v.roleSplitDigest = stateDigest('8'); }},
    {"layerRange", [] (auto& v) { ++v.layerEnd; }},
    {"prefixDigest", [] (auto& v) { v.prefixDigest = stateDigest('9'); }},
    {"prefixTokenCount", [] (auto& v) { ++v.prefixTokenCount; }},
    {"positionDigest", [] (auto& v) { v.positionDigest = stateDigest('a'); }},
    {"precision", [] (auto& v) { v.precision = "fp16"; }},
    {"layoutDigest", [] (auto& v) { v.layoutDigest = stateDigest('0'); }},
    {"stateSchemaDigest", [] (auto& v) { v.stateSchemaDigest = stateDigest('c'); }},
    {"stateComponentDigests", [] (auto& v) {
       v.stateComponentDigests.front() = stateDigest('0');
     }},
    {"stateComponentOrder", [] (auto& v) {
       std::reverse(v.stateComponentDigests.begin(), v.stateComponentDigests.end());
     }},
    {"runtimeAbiDigest", [] (auto& v) { v.runtimeAbiDigest = stateDigest('0'); }},
    {"securityDomainDigest", [] (auto& v) { v.securityDomainDigest = stateDigest('0'); }},
    {"providerIdentity", [] (auto& v) { v.providerIdentity = "/provider/B"; }},
    {"providerBootId", [] (auto& v) { v.providerBootId = "boot-b"; }},
    {"cacheEpoch", [] (auto& v) { ++v.cacheEpoch; }},
    {"stateInferenceEpoch", [] (auto& v) { ++v.stateInferenceEpoch; }},
    {"predecessorInferenceEpoch", [] (auto& v) { v.predecessorInferenceEpoch = 0; }},
    {"requestId", [] (auto& v) { v.requestId = "request-b"; }},
    {"attemptEpoch", [] (auto& v) { v.attemptEpoch = 2; }},
    {"generationId", [] (auto& v) { v.generationId = "generation-b"; }},
  };

  for (const auto& mutation : mutations) {
    BOOST_TEST_CONTEXT("mutated predecessor field=" << mutation.first) {
      auto changed = decode;
      mutation.second(*changed.predecessorDecodeStateIdentity);
      BOOST_CHECK_THROW(
        runtime.executeRoleAsync(
          "session-a", changed, std::make_shared<FakeDependencyIo>(),
          {{"input_ids", bundle("input_ids", "next-token")}}).get(),
        std::exception);
      BOOST_CHECK_EQUAL(calls.load(), 1U);
      const auto snapshot = runtime.decodeStateSnapshot();
      BOOST_CHECK_EQUAL(snapshot.entries, 1U);
      BOOST_CHECK_EQUAL(snapshot.pinnedEntries, 0U);
      BOOST_CHECK_EQUAL(snapshot.candidates, 0U);
      BOOST_CHECK_EQUAL(snapshot.commits, 1U);
    }
  }
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeAppliesConfiguredDecodeStateCapacity)
{
  NativeProviderRuntime runtime(1, 1024, 1024, 1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };

  std::atomic<std::size_t> calls{0};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls] (const RoleExecutionContext&) {
      const auto value = static_cast<std::int64_t>(++calls);
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({value});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  auto makePrefillRole = [&runnerSpec] (const std::string& requestId) {
    RoleSpec role;
    role.role = runnerSpec.role;
    role.requestId = requestId;
    role.attemptEpoch = 1;
    role.inferenceEpoch = 0;
    role.stateInputNames = {"attention_kv_in"};
    role.stateOutputNames = {"attention_kv_out"};
    role.candidateDecodeStateIdentity = exactStateIdentity(
      requestId, 1, "generation-a");
    return role;
  };

  auto first = makePrefillRole("request-a");
  auto second = makePrefillRole("request-b");
  runtime.executeRoleAsync(
    "session-a", first, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt-a")}}).get();
  runtime.executeRoleAsync(
    "session-b", second, std::make_shared<FakeDependencyIo>(),
    {{"input_ids", bundle("input_ids", "prompt-b")}}).get();

  auto snapshot = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 1U);
  BOOST_CHECK_EQUAL(snapshot.evictions, 1U);
  BOOST_CHECK_EQUAL(snapshot.commits, 2U);

  first.inferenceEpoch = 1;
  first.predecessorDecodeStateIdentity = first.candidateDecodeStateIdentity;
  first.candidateDecodeStateIdentity->prefixDigest = stateDigest('0');
  ++first.candidateDecodeStateIdentity->prefixTokenCount;
  first.candidateDecodeStateIdentity->positionDigest = stateDigest('0');
  first.candidateDecodeStateIdentity->stateInferenceEpoch = 1;
  first.candidateDecodeStateIdentity->predecessorInferenceEpoch = 0;
  BOOST_CHECK_EXCEPTION(
    runtime.executeRoleAsync(
      "session-a", first, std::make_shared<FakeDependencyIo>()),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "PROVIDER_DECODE_STATE_MISSING";
    });
  BOOST_CHECK_EQUAL(calls.load(), 2U);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeIsolatesConcurrentGenerationsAndAttempts)
{
  NativeProviderRuntime runtime(3);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", stateDigest('1')},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", stateDigest('b')},
  };

  std::atomic<std::size_t> calls{0};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls] (const RoleExecutionContext& ctx) {
      ++calls;
      const auto input = payloadText(ctx.inputsByScope.at("input_ids"));
      const std::map<std::string, std::int64_t> prefillValues{
        {"prompt-generation-a", 11},
        {"prompt-generation-b", 21},
        {"prompt-attempt-2", 31},
      };
      const std::map<std::string, std::int64_t> decodeValues{
        {"next-generation-a", 11},
        {"next-generation-b", 21},
        {"next-attempt-2", 31},
      };

      std::int64_t outputValue = 0;
      const auto prefill = prefillValues.find(input);
      if (prefill != prefillValues.end()) {
        if (ctx.inputsByScope.count("__ndnsf_provider_decode_state") != 0) {
          throw std::runtime_error("prefill unexpectedly received decode state");
        }
        outputValue = prefill->second;
      }
      else {
        const auto decode = decodeValues.find(input);
        if (decode == decodeValues.end()) {
          throw std::runtime_error("unexpected concurrent identity test input");
        }
        const auto& state = findTensor(
          decodeTensorBundle(
            ctx.inputsByScope.at("__ndnsf_provider_decode_state").payload),
          "attention_kv_in");
        const auto expected = rawTensorPayload<std::int64_t>({decode->second});
        if (state.payload != expected) {
          throw std::runtime_error("decode state crossed generation/attempt");
        }
        outputValue = decode->second + 1;
      }

      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({outputValue});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  auto makePrefillRole = [&runnerSpec] (std::string requestId,
                                        std::uint64_t attemptEpoch,
                                        std::string generationId) {
    RoleSpec role;
    role.role = runnerSpec.role;
    role.requestId = requestId;
    role.attemptEpoch = attemptEpoch;
    role.inferenceEpoch = 0;
    role.stateInputNames = {"attention_kv_in"};
    role.stateOutputNames = {"attention_kv_out"};
    role.candidateDecodeStateIdentity = exactStateIdentity(
      std::move(requestId), attemptEpoch, std::move(generationId));
    return role;
  };
  auto makeDecodeRole = [] (RoleSpec role, char digest) {
    role.inferenceEpoch = 1;
    role.predecessorDecodeStateIdentity = role.candidateDecodeStateIdentity;
    role.candidateDecodeStateIdentity->prefixDigest = stateDigest(digest);
    ++role.candidateDecodeStateIdentity->prefixTokenCount;
    role.candidateDecodeStateIdentity->positionDigest = stateDigest(digest);
    role.candidateDecodeStateIdentity->stateInferenceEpoch = 1;
    role.candidateDecodeStateIdentity->predecessorInferenceEpoch = 0;
    return role;
  };

  const std::array<std::string, 3> sessions{
    "request-a/attempt/1", "request-b/attempt/1", "request-a/attempt/2",
  };
  std::array<RoleSpec, 3> roles{
    makePrefillRole("request-a", 1, "generation-a"),
    makePrefillRole("request-b", 1, "generation-b"),
    makePrefillRole("request-a", 2, "generation-a"),
  };
  const std::array<std::string, 3> promptInputs{
    "prompt-generation-a", "prompt-generation-b", "prompt-attempt-2",
  };
  std::array<std::future<ProviderRoleResult>, 3> prefillFutures;
  for (std::size_t i = 0; i < roles.size(); ++i) {
    prefillFutures[i] = runtime.executeRoleAsync(
      sessions[i], roles[i], std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", promptInputs[i])}});
  }
  for (auto& future : prefillFutures) {
    BOOST_CHECK_NO_THROW(future.get());
  }

  roles[0] = makeDecodeRole(std::move(roles[0]), '0');
  roles[1] = makeDecodeRole(std::move(roles[1]), '2');
  roles[2] = makeDecodeRole(std::move(roles[2]), '3');
  const std::array<std::string, 3> decodeInputs{
    "next-generation-a", "next-generation-b", "next-attempt-2",
  };
  std::array<std::future<ProviderRoleResult>, 3> decodeFutures;
  for (std::size_t i = 0; i < roles.size(); ++i) {
    decodeFutures[i] = runtime.executeRoleAsync(
      sessions[i], roles[i], std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", decodeInputs[i])}});
  }
  for (auto& future : decodeFutures) {
    BOOST_CHECK_NO_THROW(future.get());
  }

  auto snapshot = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(calls.load(), 6U);
  BOOST_CHECK_EQUAL(snapshot.entries, 3U);
  BOOST_CHECK_EQUAL(snapshot.hits, 3U);
  BOOST_CHECK_EQUAL(snapshot.commits, 6U);
  BOOST_CHECK_EQUAL(snapshot.pinnedEntries, 0U);
  BOOST_CHECK_EQUAL(snapshot.candidates, 0U);

  auto rebootedSpec = runnerSpec;
  rebootedSpec.metadata["evidence.providerBootId"] = "boot-b";
  runtime.registerRunner(
    std::move(rebootedSpec),
    makeNativeModelRunner([] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    }));
  snapshot = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(snapshot.entries, 0U);
  BOOST_CHECK_EQUAL(snapshot.cleanups, 3U);
  for (std::size_t i = 0; i < roles.size(); ++i) {
    BOOST_CHECK(!runtime.releaseDecodeState(sessions[i], roles[i].role));
  }
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeRejectsMissingDecodeStateBeforeRunner)
{
  NativeProviderRuntime runtime(1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", "sha256:model"},
    {"evidence.planDigest", "sha256:plan"},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", "sha256:state-schema"},
  };

  std::atomic<std::size_t> calls{0};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls] (const RoleExecutionContext&) {
      ++calls;
      return std::map<std::string, TensorBundle>{};
    }));

  RoleSpec role;
  role.role = runnerSpec.role;
  role.requestId = "request-a";
  role.attemptEpoch = 1;
  role.inferenceEpoch = 1;
  role.stateInputNames = {"attention_kv_in"};
  role.stateOutputNames = {"attention_kv_out"};

  BOOST_CHECK_EXCEPTION(
    runtime.executeRoleAsync(
      "session-a", role, std::make_shared<FakeDependencyIo>(),
      {{"input_ids", bundle("input_ids", "next-token")}}),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) == "PROVIDER_DECODE_STATE_MISSING";
    });
  BOOST_CHECK_EQUAL(calls.load(), 0);
  const auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.hits, 0);
  BOOST_CHECK_EQUAL(state.misses, 1);
  BOOST_CHECK_EQUAL(state.commits, 0);
  BOOST_CHECK_EQUAL(state.entries, 0);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimePreservesCommittedStateAfterFailedDecode)
{
  NativeProviderRuntime runtime(1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.metadata = {
    {"evidence.modelDigest", "sha256:model"},
    {"evidence.planDigest", "sha256:plan"},
    {"evidence.providerName", "/provider/A"},
    {"evidence.providerBootId", "boot-a"},
    {"state.securityEpoch", "7"},
    {"state.generationId", "generation-a"},
    {"state.schemaDigest", "sha256:state-schema"},
  };
  std::atomic<std::size_t> calls{0};
  runtime.registerRunner(
    runnerSpec,
    makeNativeModelRunner([&calls] (const RoleExecutionContext& ctx) {
      const auto call = calls.fetch_add(1);
      if (call > 0) {
        const auto& committed = findTensor(
          decodeTensorBundle(
            ctx.inputsByScope.at("__ndnsf_provider_decode_state").payload),
          "attention_kv_in");
        BOOST_CHECK(committed.payload == rawTensorPayload<std::int64_t>({1}));
      }
      if (call == 1) {
        throw std::runtime_error("injected decode failure");
      }
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>(
        {static_cast<std::int64_t>(call == 0 ? 1 : 2)});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle("onnx-output-bundle", {std::move(state)})},
      };
    }));

  RoleSpec role;
  role.role = runnerSpec.role;
  role.requestId = "request-a";
  role.attemptEpoch = 1;
  role.stateInputNames = {"attention_kv_in"};
  role.stateOutputNames = {"attention_kv_out"};
  auto io = std::make_shared<FakeDependencyIo>();

  role.inferenceEpoch = 0;
  runtime.executeRoleAsync("session-a", role, io).get();
  role.inferenceEpoch = 1;
  BOOST_CHECK_THROW(runtime.executeRoleAsync("session-a", role, io).get(),
                    std::runtime_error);
  auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.commits, 1);
  BOOST_CHECK_EQUAL(state.rollbacks, 1);
  BOOST_CHECK_EQUAL(state.entries, 1);
  BOOST_CHECK_EQUAL(state.pinnedEntries, 0);
  BOOST_CHECK_EQUAL(state.candidates, 0);

  runtime.executeRoleAsync("session-a", role, io).get();
  state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.commits, 2);
  BOOST_CHECK_EQUAL(state.hits, 2);
  BOOST_CHECK_EQUAL(state.misses, 0);
  BOOST_CHECK(runtime.releaseDecodeState("session-a", role.role));
  state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.entries, 0);
  BOOST_CHECK_EQUAL(state.cleanups, 1);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeCarriesOpaqueStateHandleWithoutHostRoundTrip)
{
  struct SharedState
  {
    std::mutex mutex;
    std::map<std::string, NativeOpaqueStateHandleV1> handles;
    std::atomic<std::size_t> calls{0};
    std::atomic<bool> sawCompactState{false};
    std::atomic<bool> sawHostStateOutput{false};
    bool omitHandle = false;
  };

  class OpaqueRunner final : public NativeModelRunner
  {
  public:
    OpaqueRunner(std::shared_ptr<SharedState> shared,
                 DecodeStateIdentityV1 identity)
      : m_shared(std::move(shared))
      , m_identity(std::move(identity))
    {
    }

    std::map<std::string, TensorBundle>
    run(const RoleExecutionContext& ctx) final
    {
      const auto call = m_shared->calls.fetch_add(1);
      if (ctx.inferenceEpoch > 0) {
        const auto state = ctx.inputsByScope.find(
          "__ndnsf_provider_decode_state");
        if (state == ctx.inputsByScope.end() ||
            !isEncodedTensorBundle(state->second.payload)) {
          m_shared->sawCompactState.store(false);
        }
        else {
          const auto tensors = decodeTensorBundle(state->second.payload);
          const auto& handle = findTensor(tensors, "attention_kv_in");
          m_shared->sawCompactState.store(
            handle.elementType == TensorElementType::UInt8 &&
            handle.payload.size() < 256);
        }
      }

      NativeOpaqueStateHandleV1 handle;
      handle.providerIdentity = m_identity.providerIdentity;
      handle.providerBootId = m_identity.providerBootId;
      handle.sessionId = ctx.sessionId;
      handle.role = ctx.role;
      handle.token = std::string("ndnsf-device-state-v1:") +
        ctx.sessionId + ":" + ctx.role + ":" +
        std::to_string(ctx.inferenceEpoch);
      handle.stateInferenceEpoch = ctx.inferenceEpoch;
      handle.validate();
      if (!m_shared->omitHandle) {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->handles[ctx.sessionId] = handle;
      }

      NamedTensor logits;
      logits.name = "logits";
      logits.elementType = TensorElementType::Float32;
      logits.shape = {1, 2};
      logits.payload = rawTensorPayload<float>({0.0f, 1.0f});
      NamedTensor state;
      state.name = "attention_kv_out";
      state.elementType = TensorElementType::Int64;
      state.shape = {1};
      state.payload = rawTensorPayload<std::int64_t>({
        static_cast<std::int64_t>(call + 1)});
      m_shared->sawHostStateOutput.store(true);
      return {
        {"onnx-output-bundle",
         makeEncodedTensorBundle(
           "onnx-output-bundle", {std::move(logits), std::move(state)})},
      };
    }

    bool
    supportsOpaqueStateHandles() const final
    {
      return true;
    }

    std::optional<NativeOpaqueStateHandleV1>
    stateHandleSnapshot(const std::string& sessionId) const final
    {
      std::lock_guard<std::mutex> lock(m_shared->mutex);
      if (m_shared->omitHandle) {
        return std::nullopt;
      }
      const auto found = m_shared->handles.find(sessionId);
      return found == m_shared->handles.end()
        ? std::nullopt : std::optional<NativeOpaqueStateHandleV1>(found->second);
    }

    void
    releaseSessionState(const std::string& sessionId) final
    {
      std::lock_guard<std::mutex> lock(m_shared->mutex);
      m_shared->handles.erase(sessionId);
    }

  private:
    std::shared_ptr<SharedState> m_shared;
    DecodeStateIdentityV1 m_identity;
  };

  const auto identity0 = exactStateIdentity();
  auto identity1 = exactStateIdentity(
    identity0.requestId, identity0.attemptEpoch, identity0.generationId, 1, 0);
  identity1.prefixTokenCount = 4;
  identity1.prefixDigest = stateDigest('0');
  identity1.positionDigest = stateDigest('1');
  identity1.validate();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = identity0.roleName;
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-opaque-state";
  runnerSpec.path = "/tmp/opaque-state.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", identity0.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", identity0.providerIdentity},
    {"evidence.providerBootId", identity0.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identity0.generationId},
    {"state.schemaDigest", identity0.stateSchemaDigest},
  };

  auto shared = std::make_shared<SharedState>();
  NativeProviderRuntime runtime(1);
  runtime.registerRunner(
    runnerSpec, std::make_shared<OpaqueRunner>(shared, identity0));
  auto io = std::make_shared<FakeDependencyIo>();

  RoleSpec role;
  role.role = runnerSpec.role;
  role.requestId = identity0.requestId;
  role.attemptEpoch = identity0.attemptEpoch;
  role.stateInputNames = {"attention_kv_in"};
  role.stateOutputNames = {"attention_kv_out"};
  role.streamingStateExecution = true;
  role.inferenceEpoch = 0;
  role.candidateDecodeStateIdentity = identity0;
  runtime.executeRoleAsync("opaque-session", role, io).get();

  role.inferenceEpoch = 1;
  role.predecessorDecodeStateIdentity = identity0;
  role.candidateDecodeStateIdentity = identity1;
  runtime.executeRoleAsync("opaque-session", role, io).get();

  BOOST_CHECK_EQUAL(shared->calls.load(), 2);
  BOOST_CHECK(shared->sawCompactState.load());
  BOOST_CHECK(shared->sawHostStateOutput.load());
  const auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.commits, 2);
  BOOST_CHECK_EQUAL(state.hits, 1);
  BOOST_CHECK_EQUAL(state.entries, 1);
  BOOST_CHECK(runtime.releaseDecodeState("opaque-session", role.role));

  // Exercise the same handle through the epoch coordinator, rather than
  // only through the lower-level runtime entry point above.
  NativeProviderRuntime coordinatorRuntime(1);
  auto coordinatorShared = std::make_shared<SharedState>();
  auto coordinatorRunner = std::make_shared<OpaqueRunner>(coordinatorShared, identity0);
  coordinatorRuntime.registerRunner(runnerSpec, coordinatorRunner);
  NativeExecutionPlan coordinatorPlan;
  coordinatorPlan.roles = {runnerSpec.role};
  NativeDependencySpec feedback;
  feedback.producers = {runnerSpec.role};
  feedback.consumers = {runnerSpec.role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  coordinatorPlan.dependencies = {feedback};
  NativeProviderAssignment coordinatorAssignment;
  coordinatorAssignment.providerByRole[runnerSpec.role] =
    identity0.providerIdentity;
  auto coordinatorIo = std::make_shared<BlockingDependencyIo>();
  NativeEpochCoordinatorConfig coordinatorConfig{
    coordinatorRuntime, coordinatorPlan, coordinatorAssignment, coordinatorIo};
  coordinatorConfig.sessionId = "opaque-coordinator";
  coordinatorConfig.requestId = "opaque-coordinator-request";
  coordinatorConfig.attemptEpoch = 1;
  coordinatorConfig.lineagePlanDigest = stateDigest('0');
  coordinatorConfig.localProvider = identity0.providerIdentity;
  coordinatorConfig.role = runnerSpec.role;
  coordinatorConfig.initialInputs = {
    {"input_ids",
     makeEncodedTensorBundle(
       "prompt",
       {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                    rawTensorPayload<std::int64_t>({11, 12, 13})}})},
  };
  coordinatorConfig.maxEpochs = 2;
  coordinatorConfig.tokenInputName = "input_ids";
  coordinatorConfig.stateInputNames = {"attention_kv_in"};
  coordinatorConfig.stateOutputNames = {"attention_kv_out"};
  coordinatorConfig.stateIdentityTemplate = identity0;
  coordinatorConfig.positionPolicyDigest = identity0.positionDigest;
  coordinatorConfig.samplingDigest = "sha256:opaque-sampling";
  coordinatorConfig.prepareRunner = [coordinatorRunner] { return coordinatorRunner; };
  coordinatorConfig.eventSink = [] (const std::vector<std::uint8_t>&) {
    return true;
  };
  const auto coordinatorResult = runNativeEpochCoordinator(
    std::move(coordinatorConfig));
  BOOST_CHECK_EQUAL(coordinatorResult.epochsExecuted, 2);
  BOOST_CHECK_EQUAL(coordinatorResult.eventsPublished, 2);
  BOOST_CHECK(coordinatorResult.finalPayload.has_value());
  BOOST_CHECK_EQUAL(coordinatorShared->calls.load(), 2);
  BOOST_CHECK(coordinatorShared->sawCompactState.load());
  BOOST_CHECK_EQUAL(coordinatorRuntime.decodeStateSnapshot().entries, 0);

  auto failShared = std::make_shared<SharedState>();
  failShared->omitHandle = true;
  NativeProviderRuntime failRuntime(1);
  failRuntime.registerRunner(
    runnerSpec, std::make_shared<OpaqueRunner>(failShared, identity0));
  RoleSpec failRole;
  failRole.role = runnerSpec.role;
  failRole.requestId = identity0.requestId;
  failRole.attemptEpoch = identity0.attemptEpoch;
  failRole.stateInputNames = {"attention_kv_in"};
  failRole.stateOutputNames = {"attention_kv_out"};
  failRole.streamingStateExecution = true;
  failRole.candidateDecodeStateIdentity = identity0;
  BOOST_CHECK_THROW(
    failRuntime.executeRoleAsync("opaque-fail", failRole, io).get(),
    std::runtime_error);
  BOOST_CHECK_EQUAL(failRuntime.decodeStateSnapshot().entries, 0);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorKeepsDecodeStateProviderLocal)
{
  NativeProviderRuntime runtime(1);
  const auto identityTemplate = exactStateIdentity();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-stateful";
  runnerSpec.path = "/tmp/stage-0.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", identityTemplate.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", identityTemplate.providerIdentity},
    {"evidence.providerBootId", identityTemplate.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identityTemplate.generationId},
    {"state.schemaDigest", identityTemplate.stateSchemaDigest},
  };

  std::atomic<std::size_t> calls{0};
  auto runner = makeNativeModelRunner(
    [&calls] (const RoleExecutionContext& ctx) {
      const auto call = calls.fetch_add(1);
      const auto state = ctx.inputsByScope.find("__ndnsf_provider_decode_state");
      BOOST_CHECK_EQUAL(state != ctx.inputsByScope.end(), call == 1);
      if (state != ctx.inputsByScope.end()) {
        const auto& cached = findTensor(
          decodeTensorBundle(state->second.payload), "attention_kv_in");
        BOOST_CHECK(cached.payload == rawTensorPayload<std::int64_t>({1}));
      }

      NamedTensor logits;
      logits.name = "logits";
      logits.elementType = TensorElementType::Float32;
      logits.shape = {1, 2};
      logits.payload = floatPayload({0.0f, 1.0f});
      NamedTensor nextState;
      nextState.name = "attention_kv_out";
      nextState.elementType = TensorElementType::Int64;
      nextState.shape = {1};
      nextState.payload = rawTensorPayload<std::int64_t>(
        {static_cast<std::int64_t>(call + 1)});
      return std::map<std::string, TensorBundle>{
        // Deliberately collide with the dependency scope.  Qwen's native
        // runner uses the same outputBundleScope as its pipeline edge; the
        // worker must preserve the local state successor before replacing
        // this scope with the state-stripped handoff bundle.
        {"activation",
         makeEncodedTensorBundle(
           "activation", {std::move(logits), std::move(nextState)})},
      };
    });
  runtime.registerRunner(runnerSpec, runner);

  NativeExecutionPlan plan;
  plan.roles = {runnerSpec.role, "/Consumer"};
  NativeDependencySpec activation;
  activation.producers = {runnerSpec.role};
  activation.consumers = {"/Consumer"};
  activation.keyScope = "activation";
  activation.topicPrefix = "/ndnsf-di";
  activation.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{producerRole}/bundle/{sequence}";
  activation.operationKind = "ACTIVATION";
  NativeDependencySpec feedback;
  feedback.producers = {runnerSpec.role};
  feedback.consumers = {runnerSpec.role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  plan.dependencies = {activation, feedback};

  NativeProviderAssignment assignment;
  assignment.providerByRole[runnerSpec.role] = "/provider/A";
  assignment.providerByRole["/Consumer"] = "/provider/B";
  auto io = std::make_shared<BlockingDependencyIo>();
  std::vector<DecodeStateIdentityV1> observedIdentities;

  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "session-a";
  config.requestId = "request-a";
  config.attemptEpoch = 1;
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = "/provider/A";
  config.role = runnerSpec.role;
  config.initialInputs = {
    {"application-input",
     makeEncodedTensorBundle(
       "prompt",
       {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                    rawTensorPayload<std::int64_t>({11, 12, 13})}})},
  };
  config.finalResponseScope = "final-response";
  config.maxEpochs = 2;
  config.tokenInputName = "input_ids";
  config.stateInputNames = {"attention_kv_in"};
  config.stateOutputNames = {"attention_kv_out"};
  config.stateIdentityTemplate = identityTemplate;
  config.positionPolicyDigest = identityTemplate.positionDigest;
  config.samplingDigest = "sha256:sampling";
  config.prepareRunner = [runner] { return runner; };
  config.eventSink = [] (const std::vector<std::uint8_t>&) { return true; };
  config.roleSpecFactory = [plan, assignment] (std::size_t sequence) {
    auto projected = roleSpecFor(
      plan, "/Stage/0", "session-a", assignment, "/provider/A", sequence);
    DependencyEdge applicationInput;
    applicationInput.scope = "application-input";
    applicationInput.producerRole = "";
    applicationInput.consumerRole = "/Stage/0";
    applicationInput.plannedDataName = "/request/application-input";
    applicationInput.tensors = {"input_ids"};
    applicationInput.operationKind = "APPLICATION_INPUT";
    projected.inputs.insert(projected.inputs.begin(), std::move(applicationInput));
    return projected;
  };
  config.resultObserver = [&observedIdentities] (
    const RoleSpec& role, const ProviderRoleResult&) {
      BOOST_REQUIRE(role.candidateDecodeStateIdentity.has_value());
      observedIdentities.push_back(*role.candidateDecodeStateIdentity);
    };

  const auto result = runNativeEpochCoordinator(std::move(config));
  BOOST_CHECK_EQUAL(result.epochsExecuted, 2);
  BOOST_CHECK_EQUAL(result.eventsPublished, 2);
  BOOST_CHECK(result.finalPayload.has_value());
  BOOST_CHECK_EQUAL(calls.load(), 2);
  BOOST_REQUIRE(result.finalizedRole.has_value());
  BOOST_REQUIRE(result.finalizedRole->candidateDecodeStateIdentity.has_value());
  BOOST_CHECK_EQUAL(
    result.finalizedRole->candidateDecodeStateIdentity->stateInferenceEpoch, 1U);
  BOOST_CHECK_EQUAL(
    result.finalizedRole->candidateDecodeStateIdentity->prefixTokenCount, 4U);
  BOOST_REQUIRE_EQUAL(result.cacheObservations.size(), 2U);
  BOOST_CHECK_EQUAL(result.cacheObservations[0].inferenceEpoch, 0U);
  BOOST_CHECK_EQUAL(result.cacheObservations[0].actualNewInputExtent, 3U);
  BOOST_CHECK_EQUAL(result.cacheObservations[0].representedPrefixTokenCount, 3U);
  BOOST_CHECK_EQUAL(result.cacheObservations[0].prefixWorkAvoided, 0U);
  BOOST_CHECK(!result.cacheObservations[0].decodeStateHit);
  BOOST_CHECK_EQUAL(result.cacheObservations[1].inferenceEpoch, 1U);
  BOOST_CHECK_EQUAL(result.cacheObservations[1].actualNewInputExtent, 1U);
  BOOST_CHECK_EQUAL(result.cacheObservations[1].representedPrefixTokenCount, 4U);
  BOOST_CHECK_EQUAL(result.cacheObservations[1].prefixWorkAvoided, 3U);
  BOOST_CHECK(result.cacheObservations[1].decodeStateHit);
  BOOST_REQUIRE_EQUAL(observedIdentities.size(), 2);
  BOOST_CHECK_EQUAL(observedIdentities[0].stateInferenceEpoch, 0);
  BOOST_CHECK(!observedIdentities[0].predecessorInferenceEpoch.has_value());
  BOOST_CHECK_EQUAL(observedIdentities[0].prefixTokenCount, 3);
  BOOST_CHECK_EQUAL(observedIdentities[1].stateInferenceEpoch, 1);
  BOOST_REQUIRE(observedIdentities[1].predecessorInferenceEpoch.has_value());
  BOOST_CHECK_EQUAL(*observedIdentities[1].predecessorInferenceEpoch, 0);
  BOOST_CHECK_EQUAL(observedIdentities[1].prefixTokenCount, 4);
  BOOST_CHECK_EQUAL(observedIdentities[0].cacheEpoch,
                    observedIdentities[1].cacheEpoch);
  BOOST_CHECK_NE(observedIdentities[0].prefixDigest,
                 observedIdentities[1].prefixDigest);
  BOOST_CHECK_NE(observedIdentities[0].positionDigest,
                 observedIdentities[1].positionDigest);

  const auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.commits, 2);
  BOOST_CHECK_EQUAL(state.hits, 1);
  BOOST_CHECK_EQUAL(state.misses, 0);
  BOOST_CHECK_EQUAL(state.entries, 0);
  BOOST_CHECK_EQUAL(state.pinnedEntries, 0);
  BOOST_CHECK_EQUAL(state.candidates, 0);
  BOOST_CHECK_EQUAL(state.cleanups, 1);
  BOOST_REQUIRE_EQUAL(io->publishedNames.size(), 4);
  for (const auto& name : io->publishedNames) {
    BOOST_CHECK(name.find("attention_kv") == std::string::npos);
    BOOST_CHECK(name.find("provider_decode_state") == std::string::npos);
  }
  for (const auto& item : io->available) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    const auto tensors = decodeTensorBundle(item.second.payload);
    BOOST_CHECK(std::none_of(tensors.begin(), tensors.end(), [] (const auto& tensor) {
      return tensor.name == "attention_kv_in" ||
             tensor.name == "attention_kv_out";
    }));
  }
}

static void
checkNativeEpochStopBeforeRunner(bool queued, NativeEpochStopReason reason,
                                bool useAuthorityGuard = false)
{
  NativeProviderRuntime runtime(1);
  const auto identityTemplate = exactStateIdentity();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-stateful";
  runnerSpec.path = "/tmp/stage-0.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", identityTemplate.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", identityTemplate.providerIdentity},
    {"evidence.providerBootId", identityTemplate.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identityTemplate.generationId},
    {"state.schemaDigest", identityTemplate.stateSchemaDigest},
  };

  std::atomic<std::size_t> calls{0};
  auto runner = makeNativeModelRunner(
    [&calls] (const RoleExecutionContext&) {
      calls.fetch_add(1);
      return std::map<std::string, TensorBundle>{};
    });
  runtime.registerRunner(runnerSpec, runner);

  NativeExecutionPlan plan;
  plan.roles = {runnerSpec.role};
  NativeDependencySpec feedback;
  feedback.producers = {runnerSpec.role};
  feedback.consumers = {runnerSpec.role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  plan.dependencies = {feedback};

  NativeProviderAssignment assignment;
  assignment.providerByRole[runnerSpec.role] = "/provider/A";
  auto io = std::make_shared<BlockingDependencyIo>();
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "session-cancel";
  config.requestId = "request-a";
  config.attemptEpoch = 1;
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = "/provider/A";
  config.role = runnerSpec.role;
  config.initialInputs = {
    {"input_ids",
     makeEncodedTensorBundle(
       "prompt",
       {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                    rawTensorPayload<std::int64_t>({11, 12, 13})}})},
  };
  config.maxEpochs = 2;
  config.stateInputNames = {"attention_kv_in"};
  config.stateOutputNames = {"attention_kv_out"};
  config.stateIdentityTemplate = identityTemplate;
  config.positionPolicyDigest = identityTemplate.positionDigest;
  config.samplingDigest = "sha256:sampling";
  config.prepareRunner = [runner] { return runner; };
  std::atomic<bool> stopped{!queued};
  config.stopCheck = [&stopped, reason] {
    return stopped.load() ? std::optional<NativeEpochStopReason>{reason}
                          : std::nullopt;
  };
  if (useAuthorityGuard) {
    config.stopCheck = {};
    config.executionGuard = [&stopped] {
      if (stopped.load()) {
        throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED:test");
      }
    };
  }

  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  auto released = releaseBlocker.get_future().share();
  std::future<ProviderRoleResult> blocker;
  bool admitted = !queued;
  if (queued) {
    runtime.registerRunner("/blocker", [&] (const RoleExecutionContext&) {
      blockerStarted.set_value();
      released.wait_for(std::chrono::seconds(10));
      return std::map<std::string, TensorBundle>{};
    });
    RoleSpec blockerRole;
    blockerRole.role = "/blocker";
    blocker = runtime.executeRoleAsync("blocker", blockerRole, io);
    const auto started = blockerStarted.get_future().wait_for(std::chrono::seconds(5));
    if (started != std::future_status::ready) {
      releaseBlocker.set_value();
      blocker.get();
      BOOST_FAIL("blocking role did not occupy the worker");
      return;
    }
  }
  auto coordination = std::async(std::launch::async,
    [config = std::move(config)] () mutable {
      return runNativeEpochCoordinator(std::move(config));
    });
  if (queued) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime.snapshot().readyQueueDepth == 0 &&
           std::chrono::steady_clock::now() < limit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    admitted = runtime.snapshot().readyQueueDepth == 1;
    stopped.store(true);
    releaseBlocker.set_value();
    blocker.get();
  }
  try {
    coordination.get();
    BOOST_FAIL("stopped coordinator should not start the runner");
  }
  catch (const std::runtime_error& error) {
    const auto expected = useAuthorityGuard ? "DI_PROTECTED_GRANT_REJECTED:test"
      : reason == NativeEpochStopReason::Cancelled ? "ATTEMPT_CANCELLED" : "REQUEST_DEADLINE";
    BOOST_CHECK_EQUAL(error.what(), expected);
  }

  BOOST_CHECK(admitted);
  BOOST_CHECK_EQUAL(calls.load(), 0);
  const auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.entries, 0);
  BOOST_CHECK_EQUAL(state.pinnedEntries, 0);
  BOOST_CHECK_EQUAL(state.candidates, 0);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRejectsCancellationBeforeRunner)
{
  checkNativeEpochStopBeforeRunner(false, NativeEpochStopReason::Cancelled);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRejectsCancellationWhileQueued)
{
  checkNativeEpochStopBeforeRunner(true, NativeEpochStopReason::Cancelled);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRejectsDeadlineWhileQueued)
{
  checkNativeEpochStopBeforeRunner(true, NativeEpochStopReason::Deadline);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRejectsAuthorityLossWhileQueued)
{
  checkNativeEpochStopBeforeRunner(true, NativeEpochStopReason::Cancelled, true);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRollsBackWhenDeadlineExpiresAfterRunner)
{
  NativeProviderRuntime runtime(1);
  const auto identityTemplate = exactStateIdentity();
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "/Stage/0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "test-stateful";
  runnerSpec.path = "/tmp/stage-0.onnx";
  runnerSpec.metadata = {
    {"evidence.modelDigest", identityTemplate.modelDigest},
    {"evidence.planDigest", stateDigest('0')},
    {"evidence.providerName", identityTemplate.providerIdentity},
    {"evidence.providerBootId", identityTemplate.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identityTemplate.generationId},
    {"state.schemaDigest", identityTemplate.stateSchemaDigest},
  };

  std::atomic<std::size_t> calls{0};
  auto runner = makeNativeModelRunner(
    [&calls] (const RoleExecutionContext&) {
      calls.fetch_add(1);
      NamedTensor logits;
      logits.name = "logits";
      logits.elementType = TensorElementType::Float32;
      logits.shape = {1, 2};
      logits.payload = floatPayload({0.0f, 1.0f});
      NamedTensor nextState;
      nextState.name = "attention_kv_out";
      nextState.elementType = TensorElementType::Int64;
      nextState.shape = {1};
      nextState.payload = rawTensorPayload<std::int64_t>({1});
      return std::map<std::string, TensorBundle>{
        {"onnx-output-bundle",
         makeEncodedTensorBundle(
           "onnx-output-bundle", {std::move(logits), std::move(nextState)})},
      };
    });
  runtime.registerRunner(runnerSpec, runner);

  NativeExecutionPlan plan;
  plan.roles = {runnerSpec.role};
  NativeDependencySpec feedback;
  feedback.producers = {runnerSpec.role};
  feedback.consumers = {runnerSpec.role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  plan.dependencies = {feedback};

  NativeProviderAssignment assignment;
  assignment.providerByRole[runnerSpec.role] = "/provider/A";
  auto io = std::make_shared<BlockingDependencyIo>();
  std::atomic<bool> expired{false};
  std::atomic<std::size_t> events{0};
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "session-deadline";
  config.requestId = "request-a";
  config.attemptEpoch = 1;
  config.lineagePlanDigest = stateDigest('0');
  config.localProvider = "/provider/A";
  config.role = runnerSpec.role;
  config.initialInputs = {
    {"input_ids",
     makeEncodedTensorBundle(
       "prompt",
       {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                    rawTensorPayload<std::int64_t>({11, 12, 13})}})},
  };
  config.maxEpochs = 1;
  config.stateInputNames = {"attention_kv_in"};
  config.stateOutputNames = {"attention_kv_out"};
  config.stateIdentityTemplate = identityTemplate;
  config.positionPolicyDigest = identityTemplate.positionDigest;
  config.samplingDigest = "sha256:sampling";
  config.prepareRunner = [runner] { return runner; };
  config.stopCheck = [&expired] {
    return expired.load()
      ? std::optional<NativeEpochStopReason>{NativeEpochStopReason::Deadline}
      : std::nullopt;
  };
  config.resultObserver = [&expired] (const RoleSpec&, const ProviderRoleResult&) {
    expired.store(true);
  };
  config.eventSink = [&events] (const std::vector<std::uint8_t>&) {
    events.fetch_add(1);
    return true;
  };

  try {
    runNativeEpochCoordinator(std::move(config));
    BOOST_FAIL("expired coordinator should roll back before event publication");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(error.what(), "REQUEST_DEADLINE");
  }

  BOOST_CHECK_EQUAL(calls.load(), 1);
  BOOST_CHECK_EQUAL(events.load(), 0);
  const auto state = runtime.decodeStateSnapshot();
  BOOST_CHECK_EQUAL(state.entries, 0);
  BOOST_CHECK_EQUAL(state.pinnedEntries, 0);
  BOOST_CHECK_EQUAL(state.candidates, 0);
  BOOST_CHECK_EQUAL(state.rollbacks, 1);
  BOOST_CHECK_EQUAL(state.cleanups, 0);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRollsBackRejectedEventAdmission)
{
  const auto result = runNativeEpochPublicationFailure(
    NativeEpochPublicationFault::EventAdmission);
  BOOST_CHECK_EQUAL(result.error,
                    "native epoch token event admission was rejected");
  BOOST_CHECK_EQUAL(result.runnerCalls, 1U);
  BOOST_CHECK_EQUAL(result.eventCalls, 1U);
  BOOST_CHECK_EQUAL(result.publicationCalls, 0U);
  BOOST_CHECK_EQUAL(result.state.commits, 0U);
  BOOST_CHECK_EQUAL(result.state.rollbacks, 1U);
  BOOST_CHECK_EQUAL(result.state.entries, 0U);
  BOOST_CHECK_EQUAL(result.state.pinnedEntries, 0U);
  BOOST_CHECK_EQUAL(result.state.candidates, 0U);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorRollsBackFailedFeedbackPublication)
{
  const auto result = runNativeEpochPublicationFailure(
    NativeEpochPublicationFault::FeedbackPublication);
  BOOST_CHECK_EQUAL(result.error, "injected dependency publication failure");
  BOOST_CHECK_EQUAL(result.runnerCalls, 1U);
  BOOST_CHECK_EQUAL(result.eventCalls, 1U);
  BOOST_CHECK_EQUAL(result.publicationCalls, 1U);
  BOOST_CHECK_EQUAL(result.state.commits, 0U);
  BOOST_CHECK_EQUAL(result.state.rollbacks, 1U);
  BOOST_CHECK_EQUAL(result.state.entries, 0U);
  BOOST_CHECK_EQUAL(result.state.pinnedEntries, 0U);
  BOOST_CHECK_EQUAL(result.state.candidates, 0U);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorLeavesNoStateAfterActivationFailure)
{
  const auto result = runNativeEpochPublicationFailure(
    NativeEpochPublicationFault::ActivationPublication);
  BOOST_CHECK_EQUAL(result.error, "injected dependency publication failure");
  BOOST_CHECK_EQUAL(result.runnerCalls, 1U);
  BOOST_CHECK_EQUAL(result.eventCalls, 0U);
  BOOST_CHECK_EQUAL(result.publicationCalls, 1U);
  BOOST_CHECK_EQUAL(result.state.commits, 0U);
  BOOST_CHECK_EQUAL(result.state.rollbacks, 0U);
  BOOST_CHECK_EQUAL(result.state.entries, 0U);
  BOOST_CHECK_EQUAL(result.state.pinnedEntries, 0U);
  BOOST_CHECK_EQUAL(result.state.candidates, 0U);
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanBuildsRoleLocalSpecsWithDeterministicNames)
{
  NativeExecutionPlan plan;
  plan.roles = {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"};
  plan.dependencies = {
    NativeDependencySpec{
      {"/Backbone"},
      {"/Head/Shard/0", "/Head/Shard/1"},
      "backbone-to-head",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      3,
      17000,
    },
    NativeDependencySpec{
      {"/Head/Shard/0", "/Head/Shard/1"},
      {"/Merge"},
      "heads-to-merge",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      2,
      9000,
    },
  };

  NativeProviderAssignment assignment;
  assignment.providerByRole["/Backbone"] = "/example/provider/backbone";
  assignment.providerByRole["/Head/Shard/0"] = "/example/provider/head0";
  assignment.providerByRole["/Head/Shard/1"] = "/example/provider/head1";
  assignment.providerByRole["/Merge"] = "/example/provider/merge";

  const auto head0 = roleSpecFor(plan, "/Head/Shard/0", "/run-7", assignment);
  BOOST_CHECK_EQUAL(head0.role, "/Head/Shard/0");
  BOOST_REQUIRE_EQUAL(head0.inputs.size(), 1);
  BOOST_CHECK_EQUAL(head0.inputs[0].scope, "backbone-to-head");
  BOOST_CHECK_EQUAL(head0.inputs[0].producerRole, "/Backbone");
  BOOST_CHECK_EQUAL(head0.inputs[0].consumerRole, "/Head/Shard/0");
  BOOST_CHECK_EQUAL(head0.inputs[0].expectedSegments, 3);
  BOOST_CHECK_EQUAL(head0.inputs[0].expectedBytes, 17000);
  BOOST_CHECK_EQUAL(
    head0.inputs[0].plannedDataName,
    "/example/provider/backbone/NDNSF/DI/ACTIVATION/run-7/backbone-to-head/Backbone/bundle/0");
  const auto head0Segments = plannedSegmentNamesForEdge(head0.inputs[0]);
  BOOST_REQUIRE_EQUAL(head0Segments.size(), 3);
  BOOST_CHECK_EQUAL(
    head0Segments[0],
    plannedSegmentName(
      "/example/provider/backbone/NDNSF/DI/ACTIVATION/run-7/backbone-to-head/Backbone/bundle/0",
      0));
  BOOST_CHECK_EQUAL(
    head0Segments[2],
    plannedSegmentName(
      "/example/provider/backbone/NDNSF/DI/ACTIVATION/run-7/backbone-to-head/Backbone/bundle/0",
      2));

  const auto backbone = roleSpecFor(plan, "/Backbone", "/run-7", assignment);
  BOOST_REQUIRE_EQUAL(backbone.outputs.size(), 2);
  BOOST_CHECK_EQUAL(backbone.outputs[0].plannedDataName,
                    backbone.outputs[1].plannedDataName);
  BOOST_CHECK_EQUAL(
    backbone.outputs[0].plannedDataName,
    "/example/provider/backbone/NDNSF/DI/ACTIVATION/run-7/backbone-to-head/Backbone/bundle/0");

  const auto merge = roleSpecFor(plan, "/Merge", "/run-7", assignment);
  BOOST_REQUIRE_EQUAL(merge.inputs.size(), 2);
  BOOST_CHECK_EQUAL(
    merge.inputs[0].plannedDataName,
    "/example/provider/head0/NDNSF/DI/ACTIVATION/run-7/heads-to-merge/Head/Shard/0/bundle/0");
  BOOST_CHECK_EQUAL(
    merge.inputs[1].plannedDataName,
    "/example/provider/head1/NDNSF/DI/ACTIVATION/run-7/heads-to-merge/Head/Shard/1/bundle/0");

  BOOST_CHECK_THROW(roleSpecFor(plan, "/Missing", "/run-7", assignment), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(Spec111NativePlanSessionAndAttemptDefaultsAreCharacterized)
{
  NativeExecutionPlan plan;
  BOOST_CHECK_EQUAL(plan.version, 1);
  BOOST_CHECK_EQUAL(plan.modelFamily, "generic-onnx");
  BOOST_CHECK_EQUAL(plan.modelFormat, "unknown");
  BOOST_CHECK_EQUAL(plan.plannerKind, "onnx-dag");
  BOOST_CHECK_EQUAL(plan.executionPolicy, "DATA_DRIVEN_V2");

  const ExecutionAttemptKey attempt{"request-spec111", 7};
  BOOST_CHECK_EQUAL(attempt.scopedSessionId(), "request-spec111/attempt/7");
  BOOST_CHECK_NO_THROW(ndn::Name("/" + attempt.scopedSessionId()));
  const auto fields = attempt.assignmentFields();
  BOOST_CHECK_EQUAL(fields.at("executionRequestId"), "request-spec111");
  BOOST_CHECK_EQUAL(fields.at("executionAttemptEpoch"), "7");
}

BOOST_AUTO_TEST_CASE(ExecutionAttemptEpochScopesDependencyNamesAndMetadata)
{
  NativeExecutionPlan plan;
  plan.serviceName = "/Inference/Test";
  plan.roles = {"/Stage/0", "/Stage/1"};
  plan.dependencies.push_back(NativeDependencySpec{
    {"/Stage/0"}, {"/Stage/1"}, "stage-0-to-1", "/DI",
    "/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}", 1,
  });
  NativeProviderAssignment assignment;
  assignment.providerByRole["/Stage/0"] = "/provider/A";
  assignment.providerByRole["/Stage/1"] = "/provider/B";

  const ExecutionAttemptKey first{"request-7", 1};
  const ExecutionAttemptKey replacement{"request-7", 2};
  const auto firstRole = roleSpecFor(
    plan, "/Stage/1", first, assignment, "/provider/B");
  const auto replacementRole = roleSpecFor(
    plan, "/Stage/1", replacement, assignment, "/provider/B");
  BOOST_REQUIRE_EQUAL(firstRole.inputs.size(), 1);
  BOOST_REQUIRE_EQUAL(replacementRole.inputs.size(), 1);
  BOOST_CHECK_NE(firstRole.inputs[0].plannedDataName,
                 replacementRole.inputs[0].plannedDataName);
  BOOST_CHECK_EQUAL(firstRole.requestId, "request-7");
  BOOST_CHECK_EQUAL(firstRole.attemptEpoch, 1);
  BOOST_CHECK_EQUAL(replacementRole.attemptEpoch, 2);
  BOOST_CHECK(firstRole.inputs[0].plannedDataName.find("/attempt/1/") !=
              std::string::npos);
  BOOST_CHECK(replacementRole.inputs[0].plannedDataName.find("/attempt/2/") !=
              std::string::npos);
  BOOST_CHECK_NO_THROW(ndn::Name(firstRole.inputs[0].plannedDataName));
  BOOST_CHECK_NO_THROW(ndn::Name(replacementRole.inputs[0].plannedDataName));
}

BOOST_AUTO_TEST_CASE(ExecutionAttemptAuthorityRejectsOldCancelledAndDuplicateTerminal)
{
  ExecutionAttemptAuthority authority;
  const ExecutionAttemptKey first{"request-9", 1};
  const ExecutionAttemptKey replacement{"request-9", 2};
  const ExecutionAttemptKey finalAttempt{"request-9", 3};

  BOOST_CHECK_EQUAL(authority.admit(first), ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK(authority.isAuthoritative(first));
  BOOST_CHECK(authority.complete(first));
  BOOST_CHECK(!authority.complete(first));

  BOOST_CHECK_EQUAL(authority.admit(replacement),
                    ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK(!authority.isAuthoritative(first));
  BOOST_CHECK_EQUAL(authority.admit(first), ExecutionAttemptAdmission::Stale);
  BOOST_CHECK(authority.cancel(replacement));
  BOOST_CHECK(!authority.isAuthoritative(replacement));
  BOOST_CHECK(!authority.complete(replacement));

  BOOST_CHECK_EQUAL(authority.admit(finalAttempt),
                    ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK(authority.isAuthoritative(finalAttempt));
  BOOST_CHECK(authority.complete(finalAttempt));
  BOOST_CHECK(!authority.complete(finalAttempt));
}

BOOST_AUTO_TEST_CASE(Spec111CacheSessionAndAttemptEpochRemainIndependentBindings)
{
  KvStateStore store(16, 4);
  store.setProviderBootId("boot-spec111");
  KvStateBinding binding{
    "session-spec111", "/Stage/0", 3, "sha256:model", "sha256:plan",
    "/provider/spec111", "boot-spec111", 11, "request-spec111", 3,
    "generation-spec111", "sha256:state-schema", std::nullopt,
  };
  BOOST_REQUIRE(store.put(binding, bundle("kv", "state")));
  BOOST_CHECK(store.lookup(binding));

  auto changedContextEpoch = binding;
  changedContextEpoch.contextEpoch += 1;
  BOOST_CHECK(!store.lookup(changedContextEpoch));
  auto changedSecurityEpoch = binding;
  changedSecurityEpoch.securityEpoch += 1;
  BOOST_CHECK(!store.lookup(changedSecurityEpoch));

  ExecutionAttemptAuthority attempts;
  BOOST_CHECK_EQUAL(attempts.admit({"request-spec111", 3}),
                    ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK_EQUAL(attempts.admit({"request-spec111", 2}),
                    ExecutionAttemptAdmission::Stale);
  BOOST_CHECK(store.lookup(binding));
}

BOOST_AUTO_TEST_CASE(NativeProviderExecutionBindingValidatesAttemptBootAndPlan)
{
  ExecutionAttemptAuthority authority;
  const std::map<std::string, std::string> fields{
    {"executionRequestId", "request-11"},
    {"executionAttemptEpoch", "1"},
    {"executionProviderBootId", "boot-a"},
    {"executionPlanDigest", "sha256:plan"},
  };
  const auto accepted = validateNativeProviderExecutionBinding(
    fields, "boot-a", "sha256:plan", authority);
  BOOST_REQUIRE(accepted.status);
  BOOST_CHECK_EQUAL(accepted.attempt.requestId, "request-11");
  BOOST_CHECK_EQUAL(accepted.attempt.attemptEpoch, 1);
  BOOST_CHECK(authority.complete(accepted.attempt));

  const auto duplicate = validateNativeProviderExecutionBinding(
    fields, "boot-a", "sha256:plan", authority);
  BOOST_CHECK(!duplicate.status);
  BOOST_CHECK_EQUAL(duplicate.reason, "DI_ATTEMPT_DUPLICATE_TERMINAL");

  auto staleFields = fields;
  staleFields["executionAttemptEpoch"] = "2";
  const auto replacement = validateNativeProviderExecutionBinding(
    staleFields, "boot-a", "sha256:plan", authority);
  BOOST_REQUIRE(replacement.status);
  const auto stale = validateNativeProviderExecutionBinding(
    fields, "boot-a", "sha256:plan", authority);
  BOOST_CHECK(!stale.status);
  BOOST_CHECK_EQUAL(stale.reason, "DI_ATTEMPT_STALE");

  auto wrongBoot = staleFields;
  wrongBoot["executionAttemptEpoch"] = "3";
  wrongBoot["executionProviderBootId"] = "boot-b";
  BOOST_CHECK_EQUAL(validateNativeProviderExecutionBinding(
    wrongBoot, "boot-a", "sha256:plan", authority).reason,
    "DI_PROVIDER_BOOT_MISMATCH");
  auto wrongPlan = wrongBoot;
  wrongPlan["executionProviderBootId"] = "boot-a";
  wrongPlan["executionPlanDigest"] = "sha256:other";
  BOOST_CHECK_EQUAL(validateNativeProviderExecutionBinding(
    wrongPlan, "boot-a", "sha256:plan", authority).reason,
    "DI_PLAN_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(NativeProviderExecutionControlCancelsAndSupersedesInPayload)
{
  ExecutionAttemptAuthority authority;
  BOOST_REQUIRE_EQUAL(authority.admit({"request-control", 1}),
                      ExecutionAttemptAdmission::Accepted);
  const auto cancelled = applyNativeProviderExecutionControl({
    {"schema", "ndnsf-di-execution-control-v1"},
    {"operation", "CANCEL"},
    {"requestId", "request-control"},
    {"attemptEpoch", "1"},
    {"supersededByAttemptEpoch", "2"},
  }, authority);
  BOOST_REQUIRE(cancelled.recognized);
  BOOST_CHECK(cancelled.status);
  BOOST_CHECK(!authority.isAuthoritative({"request-control", 1}));

  ExecutionAttemptAuthority supersedeAuthority;
  BOOST_REQUIRE_EQUAL(supersedeAuthority.admit({"request-control", 1}),
                      ExecutionAttemptAdmission::Accepted);
  const auto superseded = applyNativeProviderExecutionControl({
    {"schema", "ndnsf-di-execution-control-v1"},
    {"operation", "SUPERSEDE"},
    {"requestId", "request-control"},
    {"attemptEpoch", "1"},
    {"supersededByAttemptEpoch", "2"},
  }, supersedeAuthority);
  BOOST_REQUIRE(superseded.recognized);
  BOOST_CHECK(superseded.status);
  BOOST_CHECK(!supersedeAuthority.isAuthoritative({"request-control", 1}));
  BOOST_CHECK(supersedeAuthority.isAuthoritative({"request-control", 2}));
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanReturnsNoStaticSegmentsForDynamicEdges)
{
  DependencyEdge dynamicEdge{
    "dynamic-edge",
    "/Producer",
    "/Consumer",
    "/example/provider/NDNSF/DI/ACTIVATION/run-dynamic/dynamic-edge/Producer/bundle/0",
    0,
    0,
  };
  BOOST_CHECK(plannedSegmentNamesForEdge(dynamicEdge).empty());
  BOOST_CHECK_EQUAL(
    plannedSegmentName(dynamicEdge.plannedDataName, 0),
    dynamicEdge.plannedDataName + "/seg=0");
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanLoadsFromGeneratedJsonShape)
{
  std::istringstream input(R"JSON({
    "version": 2,
    "services": [
      {
        "service": "/AI/Toy/Inference",
        "model": "/Model/Toy/v1",
        "modelFamily": "yolo-onnx",
        "modelFormat": "onnx",
        "plannerKind": "yolo-detect-auto",
        "executionPolicy": "LEGACY_READY_SET_V1",
        "roles": ["/Stage/0", "/Stage/1"],
        "dependencies": [
          {
            "producers": ["/Stage/0"],
            "consumers": ["/Stage/1"],
            "keyScope": "stage0-to-stage1",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 3,
            "expectedBytes": 17000,
            "segmentNaming": {
              "mode": "ndn-segment-component",
              "staticSegmentCount": 3,
              "dynamicFallback": false
            },
            "tensors": ["features"],
            "required": true
          }
        ]
      }
    ]
  })JSON");

  const auto plan = nativeExecutionPlanForServiceFromJson(input, "/AI/Toy/Inference");
  BOOST_CHECK_EQUAL(plan.version, 2);
  BOOST_CHECK_EQUAL(plan.serviceName, "/AI/Toy/Inference");
  BOOST_CHECK_EQUAL(plan.modelName, "/Model/Toy/v1");
  BOOST_CHECK_EQUAL(plan.modelFamily, "yolo-onnx");
  BOOST_CHECK_EQUAL(plan.modelFormat, "onnx");
  BOOST_CHECK_EQUAL(plan.plannerKind, "yolo-detect-auto");
  BOOST_CHECK_EQUAL(plan.executionPolicy, "LEGACY_READY_SET_V1");
  BOOST_REQUIRE_EQUAL(plan.roles.size(), 2);
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 1);
  BOOST_CHECK_EQUAL(plan.dependencies[0].keyScope, "stage0-to-stage1");
  BOOST_CHECK_EQUAL(plan.dependencies[0].expectedSegments, 3);
  BOOST_CHECK_EQUAL(plan.dependencies[0].expectedBytes, 17000);
  BOOST_CHECK_EQUAL(plan.dependencies[0].segmentNaming.mode, "ndn-segment-component");
  BOOST_CHECK_EQUAL(plan.dependencies[0].segmentNaming.staticSegmentCount, 3);
  BOOST_CHECK(!plan.dependencies[0].segmentNaming.dynamicFallback);
  BOOST_CHECK(hasStaticSegmentPlan(plan.dependencies[0]));
  BOOST_REQUIRE_EQUAL(plan.dependencies[0].tensors.size(), 1);
  BOOST_CHECK_EQUAL(plan.dependencies[0].tensors[0], "features");

  NativeProviderAssignment assignment;
  assignment.providerByRole["/Stage/0"] = "/example/provider/stage0";
  assignment.providerByRole["/Stage/1"] = "/example/provider/stage1";
  const auto session = deployNativePlanSession(plan, "/run-json", assignment);
  BOOST_CHECK_EQUAL(session.sessionId, "/run-json");
  BOOST_REQUIRE(session.rolesByName.count("/Stage/1") == 1);
  const auto& stage1 = session.rolesByName.at("/Stage/1");
  BOOST_REQUIRE_EQUAL(stage1.inputs.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs[0].expectedBytes, 17000);
  BOOST_CHECK_EQUAL(stage1.inputs[0].expectedSegments, 3);
  BOOST_REQUIRE_EQUAL(plannedSegmentNamesForEdge(stage1.inputs[0]).size(), 3);
  BOOST_REQUIRE_EQUAL(stage1.inputs[0].tensors.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs[0].tensors[0], "features");
  BOOST_CHECK_EQUAL(
    stage1.inputs[0].plannedDataName,
                    "/example/provider/stage0/NDNSF/DI/ACTIVATION/run-json/stage0-to-stage1/Stage/0/bundle/0");
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanRejectsAutomaticPolicyFallback)
{
  std::istringstream input(R"JSON({
    "version": 2,
    "services": [{
      "service": "/AI/Toy/Inference",
      "model": "/Model/Toy/v1",
      "executionPolicy": "AUTOMATIC_FALLBACK",
      "roles": ["/Stage/0"],
      "dependencies": []
    }]
  })JSON");
  BOOST_CHECK_THROW(
    nativeExecutionPlanForServiceFromJson(input, "/AI/Toy/Inference"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanJsonSupportsDynamicSegmentFallback)
{
  std::istringstream input(R"JSON({
    "version": 1,
    "services": [
      {
        "service": "/AI/Toy/DynamicInference",
        "roles": ["/Stage/0", "/Stage/1"],
        "dependencies": [
          {
            "producers": ["/Stage/0"],
            "consumers": ["/Stage/1"],
            "keyScope": "dynamic-edge",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 0,
            "expectedBytes": 0,
            "segmentNaming": {
              "mode": "ndn-segment-component",
              "staticSegmentCount": 0,
              "dynamicFallback": true
            },
            "required": true
          }
        ]
      }
    ]
  })JSON");

  const auto plan = nativeExecutionPlanForServiceFromJson(input, "/AI/Toy/DynamicInference");
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 1);
  BOOST_CHECK_EQUAL(plan.dependencies[0].segmentNaming.mode, "ndn-segment-component");
  BOOST_CHECK_EQUAL(plan.dependencies[0].segmentNaming.staticSegmentCount, 0);
  BOOST_CHECK(plan.dependencies[0].segmentNaming.dynamicFallback);
  BOOST_CHECK(!hasStaticSegmentPlan(plan.dependencies[0]));

  NativeProviderAssignment assignment;
  assignment.providerByRole["/Stage/0"] = "/example/provider/stage0";
  assignment.providerByRole["/Stage/1"] = "/example/provider/stage1";
  const auto session = deployNativePlanSession(plan, "/run-dynamic-json", assignment);
  const auto& stage1 = session.rolesByName.at("/Stage/1");
  BOOST_REQUIRE_EQUAL(stage1.inputs.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs[0].expectedSegments, 0);
  BOOST_CHECK(plannedSegmentNamesForEdge(stage1.inputs[0]).empty());
}

BOOST_AUTO_TEST_CASE(NativeServiceManifestBuildsRunnerSpecsByRole)
{
  std::istringstream input(R"JSON({
    "services": [
      {
        "name": "/AI/YOLO/2x2Inference",
        "model": "/Model/YOLO/v1",
        "roles": ["/Backbone", "/Head/Shard/0"],
        "artifacts": [
          {
            "role": "/Backbone",
            "path": "/tmp/backbone.onnx",
            "artifact": "backbone.onnx",
            "filename": "backbone.onnx",
            "kind": "onnx-model",
            "backend": "onnxruntime",
            "metadata": {
              "input_tensors": ["images"],
              "output_tensors": ["feat0", "feat1"],
              "layout": "2x2",
              "role_type": "backbone"
            }
          },
          {
            "role": "/Head/Shard/0",
            "path": "/tmp/head0.onnx",
            "artifact": "head0.onnx",
            "filename": "head0.onnx",
            "kind": "onnx-model",
            "backend": "onnxruntime",
            "metadata": {
              "input_tensors": ["feat0"],
              "output_tensors": ["pred0"]
            }
          }
        ]
      }
    ]
  })JSON");

  const auto specs = nativeModelRunnerSpecsByRoleForServiceManifestFromJson(
    input, "/AI/YOLO/2x2Inference");
  BOOST_REQUIRE_EQUAL(specs.size(), 2);
  BOOST_REQUIRE(specs.count("/Backbone") == 1);
  const auto& backbone = specs.at("/Backbone");
  BOOST_CHECK_EQUAL(backbone.backend, "onnxruntime");
  BOOST_CHECK_EQUAL(backbone.path, "/tmp/backbone.onnx");
  BOOST_CHECK_EQUAL(backbone.metadata.at("input_tensors"), "images");
  BOOST_CHECK_EQUAL(backbone.metadata.at("output_tensors"), "feat0,feat1");
  BOOST_CHECK_EQUAL(backbone.metadata.at("kind"), "onnx-model");
}

BOOST_AUTO_TEST_CASE(NativeArtifactMaterializerCachesLocalPayloadReferences)
{
  const auto root = std::filesystem::temp_directory_path() /
                    "ndnsf-di-native-artifact-materializer-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto payloadPath = root / "source.onnx";
  const std::string payload = "fake-native-onnx-model";
  {
    std::ofstream output(payloadPath, std::ios::binary);
    output << payload;
  }

  NativeModelRunnerSpec spec;
  spec.role = "/Backbone";
  spec.backend = "onnxruntime";
  spec.kind = "onnx-model";
  spec.path = "/old/path/backbone.onnx";
  std::map<std::string, NativeModelRunnerSpec> specs{{spec.role, spec}};

  std::ostringstream json;
  json << R"JSON({
    "schemaVersion": 1,
    "roles": {
      "/Backbone": {
        "model": {
          "filename": "backbone.onnx",
          "localPayloadPath": ")JSON" << payloadPath.string() << R"JSON(",
          "repoManifest": {
            "objectName": "/repo/model/backbone",
            "objectType": "model-artifact",
            "sha256": ")JSON" << sha256Hex(payload) << R"JSON(",
            "size": )JSON" << payload.size() << R"JSON(,
            "segmentCount": 1,
            "replicaNodes": ["/repo/A"]
          },
          "largeDataReference": {
            "source": "repo-manifest",
            "dataName": "/repo/model/backbone"
          }
        }
      }
    }
  })JSON";
  std::istringstream input(json.str());
  NativeArtifactMaterializerOptions options;
  options.cacheDir = (root / "cache").string();
  const auto materialized = materializeNativeModelArtifactsFromReferencesJson(
    specs,
    input,
    options);

  BOOST_REQUIRE(materialized.count("/Backbone") == 1);
  const auto& updated = materialized.at("/Backbone");
  BOOST_CHECK_NE(updated.path, spec.path);
  BOOST_CHECK_EQUAL(updated.metadata.at("materializedFrom"), "artifact-references");
  BOOST_CHECK(std::filesystem::exists(updated.path));
  std::ifstream cached(updated.path, std::ios::binary);
  const std::string cachedPayload{
    std::istreambuf_iterator<char>(cached),
    std::istreambuf_iterator<char>()};
  BOOST_CHECK_EQUAL(cachedPayload, payload);
}

BOOST_AUTO_TEST_CASE(NativeArtifactMaterializerRejectsHashMismatch)
{
  const auto root = std::filesystem::temp_directory_path() /
                    "ndnsf-di-native-artifact-materializer-hash-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto payloadPath = root / "source.onnx";
  {
    std::ofstream output(payloadPath, std::ios::binary);
    output << "bad-payload";
  }

  NativeModelRunnerSpec spec;
  spec.role = "/Backbone";
  spec.backend = "onnxruntime";
  spec.kind = "onnx-model";
  std::map<std::string, NativeModelRunnerSpec> specs{{spec.role, spec}};

  std::ostringstream json;
  json << R"JSON({
    "roles": {
      "/Backbone": {
        "model": {
          "filename": "backbone.onnx",
          "localPayloadPath": ")JSON" << payloadPath.string() << R"JSON(",
          "repoManifest": {
            "objectName": "/repo/model/backbone",
            "objectType": "model-artifact",
            "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
            "size": 11,
            "segmentCount": 1
          }
        }
      }
    }
  })JSON";
  std::istringstream input(json.str());
  BOOST_CHECK_THROW(
    materializeNativeModelArtifactsFromReferencesJson(specs, input),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NativeArtifactMaterializerFetchesRepoOnlyReference)
{
  const auto root = std::filesystem::temp_directory_path() /
                    "ndnsf-di-native-artifact-materializer-repo-fetch-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::string payload = "repo-backed-native-onnx-model";

  NativeModelRunnerSpec spec;
  spec.role = "/Backbone";
  spec.backend = "onnxruntime";
  spec.kind = "onnx-model";
  spec.path = "/old/path/backbone.onnx";
  std::map<std::string, NativeModelRunnerSpec> specs{{spec.role, spec}};

  std::ostringstream json;
  json << R"JSON({
    "schemaVersion": 1,
    "roles": {
      "/Backbone": {
        "model": {
          "filename": "backbone.onnx",
          "repoManifest": {
            "objectName": "/repo/model/backbone",
            "objectType": "model-artifact",
            "sha256": ")JSON" << sha256Hex(payload) << R"JSON(",
            "size": )JSON" << payload.size() << R"JSON(,
            "segmentCount": 1,
            "replicaNodes": ["/repo/A"]
          },
          "largeDataReference": {
            "source": "repo-manifest",
            "dataName": "/repo/model/backbone"
          }
        }
      }
    }
  })JSON";

  NativeArtifactMaterializerOptions options;
  options.cacheDir = (root / "cache").string();
  bool fetched = false;
  options.repoFetchFromManifest = [&] (const std::string& objectName,
                                       const std::string& repoManifestJson) {
    BOOST_CHECK_EQUAL(objectName, "/repo/model/backbone");
    BOOST_CHECK(repoManifestJson.find("\"segmentCount\":\"1\"") != std::string::npos ||
                repoManifestJson.find("\"segmentCount\": \"1\"") != std::string::npos ||
                repoManifestJson.find("\"segmentCount\": 1") != std::string::npos);
    fetched = true;
    return std::vector<std::uint8_t>(payload.begin(), payload.end());
  };

  std::istringstream input(json.str());
  const auto materialized = materializeNativeModelArtifactsFromReferencesJson(
    specs,
    input,
    options);

  BOOST_CHECK(fetched);
  const auto& updated = materialized.at("/Backbone");
  BOOST_CHECK_NE(updated.path, spec.path);
  BOOST_CHECK_EQUAL(updated.metadata.at("materializedFrom"), "artifact-references");
  BOOST_CHECK(std::filesystem::exists(updated.path));
}

BOOST_AUTO_TEST_CASE(NativeArtifactMaterializerRejectsRepoOnlyReferenceWithoutFetcher)
{
  NativeModelRunnerSpec spec;
  spec.role = "/Backbone";
  spec.backend = "onnxruntime";
  spec.kind = "onnx-model";
  std::map<std::string, NativeModelRunnerSpec> specs{{spec.role, spec}};

  std::istringstream input(R"JSON({
    "roles": {
      "/Backbone": {
        "model": {
          "filename": "backbone.onnx",
          "repoManifest": {
            "objectName": "/repo/model/backbone",
            "sha256": "00",
            "size": 1
          }
        }
      }
    }
  })JSON");

  BOOST_CHECK_THROW(
    materializeNativeModelArtifactsFromReferencesJson(specs, input),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanJsonDrivesAsyncFrontierRuntime)
{
  std::istringstream input(R"JSON({
    "version": 1,
    "services": [
      {
        "service": "/AI/YOLO/ParallelDetectScale",
        "model": "/Model/YOLO/v1",
        "roles": ["/Backbone", "/Head/0", "/Head/1", "/Merge"],
        "dependencies": [
          {
            "producers": ["/Backbone"],
            "consumers": ["/Head/0", "/Head/1"],
            "keyScope": "backbone-to-heads",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 4,
            "expectedBytes": 24000,
            "required": true
          },
          {
            "producers": ["/Head/0"],
            "consumers": ["/Merge"],
            "keyScope": "head0-to-merge",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 2,
            "expectedBytes": 9000,
            "required": true
          },
          {
            "producers": ["/Head/1"],
            "consumers": ["/Merge"],
            "keyScope": "head1-to-merge",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 2,
            "expectedBytes": 9000,
            "required": true
          },
          {
            "producers": ["/Merge"],
            "consumers": [""],
            "keyScope": "merge-to-user",
            "topicPrefix": "/activation",
            "objectNameTemplate": "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
            "expectedSegments": 1,
            "expectedBytes": 3000,
            "required": true
          }
        ]
      }
    ]
  })JSON");

  const auto plan = nativeExecutionPlanForServiceFromJson(
    input, "/AI/YOLO/ParallelDetectScale");
  NativeProviderAssignment assignment;
  assignment.providerByRole["/Backbone"] = "/example/provider/backbone";
  assignment.providerByRole["/Head/0"] = "/example/provider/head0";
  assignment.providerByRole["/Head/1"] = "/example/provider/head1";
  assignment.providerByRole["/Merge"] = "/example/provider/merge";

  std::vector<RoleSpec> roles;
  roles.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    roles.push_back(roleSpecFor(plan, role, "/run-json-frontier", assignment));
  }

  const auto merge = roleSpecFor(plan, "/Merge", "/run-json-frontier", assignment);
  BOOST_REQUIRE_EQUAL(merge.inputs.size(), 2);
  BOOST_CHECK_EQUAL(merge.inputs[0].scope, "head0-to-merge");
  BOOST_CHECK_EQUAL(merge.inputs[1].scope, "head1-to-merge");
  BOOST_CHECK_EQUAL(
    merge.inputs[0].plannedDataName,
    "/example/provider/head0/NDNSF/DI/ACTIVATION/run-json-frontier/head0-to-merge/Head/0/bundle/0");
  BOOST_CHECK_EQUAL(
    merge.inputs[1].plannedDataName,
    "/example/provider/head1/NDNSF/DI/ACTIVATION/run-json-frontier/head1-to-merge/Head/1/bundle/0");

  AsyncDataflowRuntime runtime(4);
  const auto started = std::chrono::steady_clock::now();
  const auto result = runtime.run(
    "run-json-frontier",
    roles,
    {},
    [] (const RoleExecutionContext& ctx) {
      if (ctx.role == "/Backbone") {
        BOOST_CHECK(ctx.inputsByScope.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::map<std::string, TensorBundle>{
          {"backbone-to-heads", bundle("backbone", "features")},
        };
      }
      if (ctx.role == "/Head/0" || ctx.role == "/Head/1") {
        BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
        BOOST_CHECK_EQUAL(payloadText(ctx.inputsByScope.at("backbone-to-heads")),
                          "features");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto scope = ctx.role == "/Head/0" ? "head0-to-merge" : "head1-to-merge";
        const auto value = ctx.role == "/Head/0" ? "h0" : "h1";
        return std::map<std::string, TensorBundle>{
          {scope, bundle(scope, value)},
        };
      }

      BOOST_CHECK_EQUAL(ctx.role, "/Merge");
      BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 2);
      return std::map<std::string, TensorBundle>{
        {"merge-to-user", bundle("result",
                                 payloadText(ctx.inputsByScope.at("head0-to-merge")) +
                                 "+" +
                                 payloadText(ctx.inputsByScope.at("head1-to-merge")))},
      };
    });
  const auto elapsed = durationMs(started, std::chrono::steady_clock::now());

  BOOST_REQUIRE(result.outputsByScope.count("merge-to-user") == 1);
  BOOST_CHECK_EQUAL(payloadText(result.outputsByScope.at("merge-to-user")), "h0+h1");
  BOOST_CHECK_LT(elapsed, 170.0);

  std::map<std::string, RoleTiming> timingByRole;
  for (const auto& timing : result.roleTimings) {
    timingByRole.emplace(timing.role, timing);
  }
  BOOST_REQUIRE(timingByRole.count("/Head/0") == 1);
  BOOST_REQUIRE(timingByRole.count("/Head/1") == 1);
  BOOST_REQUIRE(timingByRole.count("/Merge") == 1);
  const auto latestHeadStart = std::max(timingByRole.at("/Head/0").startedAt,
                                        timingByRole.at("/Head/1").startedAt);
  const auto earliestHeadFinish = std::min(timingByRole.at("/Head/0").finishedAt,
                                           timingByRole.at("/Head/1").finishedAt);
  BOOST_CHECK_GE(durationMs(latestHeadStart, earliestHeadFinish), 0.0);
  BOOST_CHECK_GE(durationMs(timingByRole.at("/Head/0").finishedAt,
                            timingByRole.at("/Merge").startedAt),
                 0.0);
  BOOST_CHECK_GE(durationMs(timingByRole.at("/Head/1").finishedAt,
                            timingByRole.at("/Merge").startedAt),
                 0.0);
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanGeneratedJsonDrivesAsyncFrontierRuntime)
{
  const char* planPath = std::getenv("NDNSF_DI_NATIVE_PLAN_JSON");
  if (planPath == nullptr || std::string(planPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_NATIVE_PLAN_JSON not set; generated-plan smoke skipped");
    return;
  }

  const std::string serviceName = [] {
    const char* value = std::getenv("NDNSF_DI_NATIVE_PLAN_SERVICE");
    if (value == nullptr || std::string(value).empty()) {
      return std::string("/AI/YOLO/2x2Inference");
    }
    return std::string(value);
  }();

  std::ifstream input(planPath);
  BOOST_REQUIRE_MESSAGE(input.good(), "cannot open native plan: " << planPath);
  const auto plan = nativeExecutionPlanForServiceFromJson(input, serviceName);
  BOOST_REQUIRE(plan.roles.size() >= 3);
  BOOST_REQUIRE(std::find(plan.roles.begin(), plan.roles.end(), "/Merge") != plan.roles.end());

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  std::vector<RoleSpec> roles;
  roles.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    roles.push_back(roleSpecFor(plan, role, "/generated-plan-run", assignment));
  }
  for (const auto& role : roles) {
    for (const auto& edge : role.inputs) {
      const auto expectedPrefix = assignment.providerByRole.at(edge.producerRole) + "/NDNSF/DI/ACTIVATION/";
      BOOST_CHECK_MESSAGE(edge.plannedDataName.rfind(expectedPrefix, 0) == 0,
                          "input activation name is not under producer provider namespace: "
                            << edge.plannedDataName);
      const auto segments = plannedSegmentNamesForEdge(edge);
      BOOST_CHECK_EQUAL(segments.size(), edge.expectedSegments);
      if (edge.expectedSegments > 0) {
        BOOST_CHECK_EQUAL(segments.front(), plannedSegmentName(edge.plannedDataName, 0));
        BOOST_CHECK_EQUAL(segments.back(), plannedSegmentName(edge.plannedDataName, edge.expectedSegments - 1));
      }
    }
    for (const auto& edge : role.outputs) {
      const auto expectedPrefix = assignment.providerByRole.at(edge.producerRole) + "/NDNSF/DI/ACTIVATION/";
      BOOST_CHECK_MESSAGE(edge.plannedDataName.rfind(expectedPrefix, 0) == 0,
                          "output activation name is not under producer provider namespace: "
                            << edge.plannedDataName);
      const auto segments = plannedSegmentNamesForEdge(edge);
      BOOST_CHECK_EQUAL(segments.size(), edge.expectedSegments);
      if (edge.expectedSegments > 0) {
        BOOST_CHECK_EQUAL(segments.front(), plannedSegmentName(edge.plannedDataName, 0));
        BOOST_CHECK_EQUAL(segments.back(), plannedSegmentName(edge.plannedDataName, edge.expectedSegments - 1));
      }
    }
  }

  const auto merge = roleSpecFor(plan, "/Merge", "/generated-plan-run", assignment);
  BOOST_REQUIRE_GE(merge.inputs.size(), 2);
  std::set<std::string> mergeScopes;
  for (const auto& edge : merge.inputs) {
    BOOST_CHECK(!edge.scope.empty());
    BOOST_CHECK(!edge.plannedDataName.empty());
    mergeScopes.insert(edge.scope);
  }
  BOOST_CHECK_EQUAL(mergeScopes.size(), merge.inputs.size());

  std::mutex observedMutex;
  std::set<std::string> mergeInputScopes;
  AsyncDataflowRuntime runtime(4);
  const auto result = runtime.run(
    "generated-plan-run",
    roles,
    {},
    [&] (const RoleExecutionContext& ctx) {
      if (ctx.role == "/Backbone") {
        BOOST_CHECK(ctx.inputsByScope.empty());
        std::map<std::string, TensorBundle> outputs;
        for (const auto& role : roles) {
          if (role.role == ctx.role) {
            for (const auto& edge : role.outputs) {
              outputs.emplace(edge.scope, bundle(edge.scope, "features"));
            }
            break;
          }
        }
        return outputs;
      }
      if (ctx.role.find("/Head/Shard/") == 0) {
        BOOST_REQUIRE(ctx.inputsByScope.size() <= 1);
        std::map<std::string, TensorBundle> outputs;
        for (const auto& role : roles) {
          if (role.role == ctx.role) {
            for (const auto& edge : role.outputs) {
              outputs.emplace(edge.scope, bundle(edge.scope, ctx.role));
            }
            break;
          }
        }
        return outputs;
      }

      if (ctx.role == "/Merge") {
        BOOST_REQUIRE_GE(ctx.inputsByScope.size(), 2);
        std::lock_guard<std::mutex> lock(observedMutex);
        for (const auto& item : ctx.inputsByScope) {
          mergeInputScopes.insert(item.first);
        }
        return std::map<std::string, TensorBundle>{};
      }

      return std::map<std::string, TensorBundle>{};
    });

  std::map<std::string, RoleTiming> timingByRole;
  for (const auto& timing : result.roleTimings) {
    timingByRole.emplace(timing.role, timing);
  }
  BOOST_REQUIRE(timingByRole.count("/Merge") == 1);
  BOOST_REQUIRE_GE(std::count_if(
                     timingByRole.begin(),
                     timingByRole.end(),
                     [] (const auto& item) {
                       return item.first.find("/Head/Shard/") == 0;
                     }),
                   2);

  std::lock_guard<std::mutex> lock(observedMutex);
  BOOST_CHECK_EQUAL(mergeInputScopes.size(), merge.inputs.size());
  for (const auto& edge : merge.inputs) {
    BOOST_CHECK(mergeInputScopes.count(edge.scope) == 1);
  }
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanGeneratedJsonDrivesProviderRoleWorkers)
{
  const char* planPath = std::getenv("NDNSF_DI_NATIVE_PLAN_JSON");
  if (planPath == nullptr || std::string(planPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_NATIVE_PLAN_JSON not set; generated provider-role smoke skipped");
    return;
  }

  const std::string serviceName = [] {
    const char* value = std::getenv("NDNSF_DI_NATIVE_PLAN_SERVICE");
    if (value == nullptr || std::string(value).empty()) {
      return std::string("/AI/YOLO/2x2Inference");
    }
    return std::string(value);
  }();

  std::ifstream input(planPath);
  BOOST_REQUIRE_MESSAGE(input.good(), "cannot open native plan: " << planPath);
  const auto plan = nativeExecutionPlanForServiceFromJson(input, serviceName);
  BOOST_REQUIRE(plan.roles.size() >= 3);
  BOOST_REQUIRE(std::find(plan.roles.begin(), plan.roles.end(), "/Merge") != plan.roles.end());

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  std::map<std::string, RoleSpec> roleSpecs;
  for (const auto& role : plan.roles) {
    roleSpecs.emplace(role, roleSpecFor(plan, role, "/generated-provider-run", assignment));
  }
  BOOST_REQUIRE(roleSpecs.count("/Merge") == 1);
  BOOST_REQUIRE_GE(roleSpecs.at("/Merge").inputs.size(), 2);
  BOOST_REQUIRE_GE(std::count_if(
                     roleSpecs.begin(),
                     roleSpecs.end(),
                     [] (const auto& item) {
                       return item.first.find("/Head/Shard/") == 0;
                     }),
                   2);

  auto io = std::make_shared<BlockingDependencyIo>();
  NativeProviderRuntime runtime(plan.roles.size());
  std::mutex observedMutex;
  std::set<std::string> mergeInputScopes;

  for (const auto& item : roleSpecs) {
    runtime.registerRunner(
      item.first,
      [&roleSpecs, &observedMutex, &mergeInputScopes] (const RoleExecutionContext& ctx) {
        const auto found = roleSpecs.find(ctx.role);
        BOOST_REQUIRE(found != roleSpecs.end());
        const auto& role = found->second;

        if (ctx.role == "/Backbone") {
          BOOST_CHECK(ctx.inputsByScope.empty());
          std::map<std::string, TensorBundle> outputs;
          for (const auto& edge : role.outputs) {
            outputs.emplace(edge.scope, bundle(edge.scope, "features:" + edge.scope));
          }
          return outputs;
        }

        if (ctx.role.find("/Head/Shard/") == 0) {
          BOOST_REQUIRE(ctx.inputsByScope.size() <= 1);
          std::map<std::string, TensorBundle> outputs;
          for (const auto& edge : role.outputs) {
            outputs.emplace(edge.scope, bundle(edge.scope, "head:" + ctx.role));
          }
          return outputs;
        }

        if (ctx.role == "/Merge") {
          BOOST_REQUIRE_GE(ctx.inputsByScope.size(), 2);
          std::lock_guard<std::mutex> lock(observedMutex);
          for (const auto& inputScope : ctx.inputsByScope) {
            mergeInputScopes.insert(inputScope.first);
          }
          return std::map<std::string, TensorBundle>{};
        }

        return std::map<std::string, TensorBundle>{};
      });
  }

  std::vector<std::future<ProviderRoleResult>> futures;
  futures.reserve(roleSpecs.size());
  for (const auto& item : roleSpecs) {
    futures.push_back(runtime.executeRoleAsync("generated-provider-run", item.second, io));
  }

  std::map<std::string, ProviderRoleResult> resultsByRole;
  for (std::size_t i = 0; i < plan.roles.size(); ++i) {
    auto result = futures[i].get();
    resultsByRole.emplace(result.timing.role, std::move(result));
  }

  BOOST_REQUIRE(resultsByRole.count("/Merge") == 1);
  if (resultsByRole.count("/Backbone") == 1) {
    BOOST_CHECK(resultsByRole.at("/Backbone").inputTimings.empty());
  }
  BOOST_REQUIRE_GE(std::count_if(
                     resultsByRole.begin(),
                     resultsByRole.end(),
                     [] (const auto& item) {
                       return item.first.find("/Head/Shard/") == 0;
                     }),
                   2);
  BOOST_CHECK_EQUAL(resultsByRole.at("/Merge").inputTimings.size(),
                    roleSpecs.at("/Merge").inputs.size());

  {
    std::lock_guard<std::mutex> lock(observedMutex);
    BOOST_CHECK_EQUAL(mergeInputScopes.size(), roleSpecs.at("/Merge").inputs.size());
    for (const auto& edge : roleSpecs.at("/Merge").inputs) {
      BOOST_CHECK(mergeInputScopes.count(edge.scope) == 1);
    }
  }

  {
    std::lock_guard<std::mutex> lock(io->mutex);
    BOOST_CHECK_GE(io->prefetchedNames.size(), plan.dependencies.size());
    BOOST_CHECK_GE(io->publishedNames.size(), plan.dependencies.size());
    for (const auto& name : io->prefetchedNames) {
      BOOST_CHECK(!name.empty());
    }
    for (const auto& name : io->publishedNames) {
      BOOST_CHECK(!name.empty());
    }
  }
}

BOOST_AUTO_TEST_CASE(NativeExecutionPlanGeneratedJsonDrivesProviderSessionSkeleton)
{
  const char* planPath = std::getenv("NDNSF_DI_NATIVE_PLAN_JSON");
  if (planPath == nullptr || std::string(planPath).empty()) {
    BOOST_TEST_MESSAGE("NDNSF_DI_NATIVE_PLAN_JSON not set; generated provider-session smoke skipped");
    return;
  }

  const std::string serviceName = [] {
    const char* value = std::getenv("NDNSF_DI_NATIVE_PLAN_SERVICE");
    if (value == nullptr || std::string(value).empty()) {
      return std::string("/AI/YOLO/2x2Inference");
    }
    return std::string(value);
  }();

  std::ifstream input(planPath);
  BOOST_REQUIRE_MESSAGE(input.good(), "cannot open native plan: " << planPath);
  const auto plan = nativeExecutionPlanForServiceFromJson(input, serviceName);
  BOOST_REQUIRE(plan.roles.size() >= 3);
  BOOST_REQUIRE(std::find(plan.roles.begin(), plan.roles.end(), "/Merge") != plan.roles.end());

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  auto io = std::make_shared<BlockingDependencyIo>();
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  std::mutex observedMutex;
  std::set<std::string> mergeInputScopes;

  factory->registerBackend(
    "test-backend",
    [&observedMutex, &mergeInputScopes] (const NativeModelRunnerSpec& spec) {
      return makeNativeModelRunner(
        [spec, &observedMutex, &mergeInputScopes] (const RoleExecutionContext& ctx) {
          BOOST_CHECK_EQUAL(ctx.role, spec.role);
          if (ctx.role == "/Backbone") {
            BOOST_CHECK(ctx.inputsByScope.empty());
            std::map<std::string, TensorBundle> outputs;
            for (const auto& item : spec.metadata) {
              if (item.first.find("outputScope.") == 0) {
                outputs.emplace(item.second, bundle(item.second, "features:" + item.second));
              }
            }
            return outputs;
          }
          if (ctx.role.find("/Head/Shard/") == 0) {
            BOOST_REQUIRE(ctx.inputsByScope.size() <= 1);
            std::map<std::string, TensorBundle> outputs;
            for (const auto& item : spec.metadata) {
              if (item.first.find("outputScope.") == 0) {
                outputs.emplace(item.second, bundle(item.second, "head:" + ctx.role));
              }
            }
            return outputs;
          }
          if (ctx.role == "/Merge") {
            BOOST_REQUIRE_GE(ctx.inputsByScope.size(), 2);
            std::lock_guard<std::mutex> lock(observedMutex);
            for (const auto& inputScope : ctx.inputsByScope) {
              mergeInputScopes.insert(inputScope.first);
            }
            return std::map<std::string, TensorBundle>{};
          }
          return std::map<std::string, TensorBundle>{};
        });
    });

  NativeProviderSession session(plan, assignment, io, factory, plan.roles.size());
  for (const auto& role : plan.roles) {
    const auto spec = session.roleSpec(role, "generated-session-run");
    NativeModelRunnerSpec runnerSpec;
    runnerSpec.role = role;
    runnerSpec.kind = "onnx-model";
    runnerSpec.backend = "test-backend";
    runnerSpec.path = "/tmp/" + trimSlashes(role) + ".onnx";
    for (std::size_t i = 0; i < spec.outputs.size(); ++i) {
      runnerSpec.metadata["outputScope." + std::to_string(i)] = spec.outputs[i].scope;
    }
    session.registerRunner(runnerSpec);
    BOOST_CHECK(session.hasRunner(role));
  }

  std::vector<std::future<ProviderRoleResult>> futures;
  futures.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    futures.push_back(session.executeRoleAsync("generated-session-run", role));
  }

  std::map<std::string, ProviderRoleResult> resultsByRole;
  for (auto& future : futures) {
    auto result = future.get();
    resultsByRole.emplace(result.timing.role, std::move(result));
  }

  BOOST_REQUIRE(resultsByRole.count("/Merge") == 1);
  if (resultsByRole.count("/Backbone") == 1) {
    BOOST_CHECK(resultsByRole.at("/Backbone").inputTimings.empty());
  }
  BOOST_REQUIRE_GE(std::count_if(
                     resultsByRole.begin(),
                     resultsByRole.end(),
                     [] (const auto& item) {
                       return item.first.find("/Head/Shard/") == 0;
                     }),
                   2);
  BOOST_CHECK_GE(resultsByRole.at("/Merge").inputTimings.size(), 2);

  {
    std::lock_guard<std::mutex> lock(observedMutex);
    BOOST_CHECK_EQUAL(mergeInputScopes.size(),
                      resultsByRole.at("/Merge").inputTimings.size());
  }

BOOST_CHECK_THROW(
    session.registerRunner(NativeModelRunnerSpec{"/Unknown", "onnx-model", "test-backend", "", {}}),
    std::out_of_range);
}

BOOST_AUTO_TEST_CASE(NativeProviderReadinessAckControlsSelectionEligibility)
{
  NativeProviderReadinessState readiness;

  readiness.markInstalling("downloading role artifacts");
  auto installingAck = readiness.makeAckDecision("/Backbone,/Merge");
  BOOST_CHECK(!installingAck.status);
  BOOST_CHECK_EQUAL(readiness.statusText(), "installing");
  BOOST_CHECK_EQUAL(installingAck.message,
                    ndn_service_framework::negative_ack_reason::ModelUnavailable);
  const auto installingJson = typedCapabilityJson(installingAck);
  BOOST_CHECK(installingJson.find("\"runtimeStatus\":\"installing\"") !=
              std::string::npos);
  BOOST_CHECK(installingJson.find("\"hasModel\":false") != std::string::npos);

  readiness.markFailed("artifact hash mismatch");
  auto failedAck = readiness.makeAckDecision("/Backbone,/Merge");
  BOOST_CHECK(!failedAck.status);
  BOOST_CHECK_EQUAL(readiness.statusText(), "failed");
  BOOST_CHECK_EQUAL(failedAck.message,
                    ndn_service_framework::negative_ack_reason::InternalError);
  const auto failedJson = typedCapabilityJson(failedAck);
  BOOST_CHECK(failedJson.find("\"runtimeStatus\":\"failed\"") !=
              std::string::npos);
  BOOST_CHECK(failedJson.find("\"hasModel\":false") != std::string::npos);

  readiness.markReady("native runner specs installed");
  ExecutionEvidence readinessEvidence;
  readinessEvidence.providerName = "/provider/A";
  readinessEvidence.providerBootId = "boot-a";
  readinessEvidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  readinessEvidence.realCompute = true;
  readinessEvidence.deviceKind = "cuda";
  readinessEvidence.deviceId = "GPU-1";
  readinessEvidence.runtimeVersion = "ort";
  readinessEvidence.modelDigest = "sha256:model";
  readinessEvidence.planDigest = "sha256:plan";
  readinessEvidence.artifactDigests["/Backbone"] = "sha256:artifact";
  readinessEvidence.roles = {"/Backbone"};
  readinessEvidence.createdAtMs = 1;
  readiness.setExecutionEvidence(readinessEvidence);
  readiness.setExecutionEvidenceByRole({{"/Backbone", readinessEvidence}});
  auto readyAck = readiness.makeAckDecision("/Backbone,/Merge");
  BOOST_CHECK(readyAck.status);
  BOOST_CHECK(readiness.isReady());
  BOOST_CHECK_EQUAL(readiness.statusText(), "ready");
  BOOST_CHECK(readyAck.message.find("native runner specs installed") !=
              std::string::npos);
  const auto readyJson = typedCapabilityJson(readyAck);
  BOOST_CHECK(readyJson.find("\"runtimeStatus\":\"ready\"") !=
              std::string::npos);
  BOOST_CHECK(readyJson.find("\"hasModel\":true") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"executionEvidence\"") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"executionEvidenceByRole\"") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"/Backbone\"") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"runnerKind\":\"onnxruntime-cuda\"") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"queue\":0") != std::string::npos);
  BOOST_CHECK(readyJson.find("\"workers\":0") != std::string::npos);
  BOOST_CHECK(readyJson.find(
    "\"configuredCapability\":{\"schema\":\"ndnsf-di-configured-capability-v1\"") !=
              std::string::npos);
  BOOST_CHECK(readyJson.find(
    "\"measuredTelemetry\":{\"schema\":\"ndnsf-di-measured-telemetry-v1\","
    "\"source\":\"unavailable\",\"status\":\"unsupported\"") !=
              std::string::npos);
  BOOST_CHECK(ackPayloadText(readyAck).find("providerCapabilityHint=json64:") !=
              std::string::npos);

  ProviderRoleWorkerSnapshot capacity;
  capacity.workerCount = 4;
  capacity.readyQueueDepth = 2;
  capacity.waitingForInputCount = 1;
  capacity.activeWorkerCount = 3;
  readiness.setCapacitySnapshotProvider([capacity] { return capacity; });
  auto capacityAck = readiness.makeAckDecision("/Backbone,/Merge");
  const auto capacityPayload = ackPayloadText(capacityAck);
  const auto capacityJson = typedCapabilityJson(capacityAck);
  BOOST_CHECK(capacityAck.status);
  BOOST_CHECK(capacityJson.find("\"queue\":6") != std::string::npos);
  BOOST_CHECK(capacityJson.find("\"readyQueue\":2") != std::string::npos);
  BOOST_CHECK(capacityJson.find("\"waitingInputs\":1") != std::string::npos);
  BOOST_CHECK(capacityJson.find("\"activeWorkers\":3") != std::string::npos);
  BOOST_CHECK(capacityJson.find("\"workers\":4") != std::string::npos);
  BOOST_CHECK(capacityJson.find("\"idleWorkers\":1") != std::string::npos);
  BOOST_CHECK(capacityPayload.find("providerCapabilityHint=json64:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ProviderResourceSnapshotFreshnessFailsClosed)
{
  ProviderResourceSnapshot snapshot;
  snapshot.status = ResourceProbeStatus::Measured;
  snapshot.source = "linux-proc";
  snapshot.providerName = "/provider/A";
  snapshot.providerBootId = "boot-a";
  snapshot.sequence = 7;
  snapshot.measuredAtMs = 1'000;
  snapshot.hostTotalMemoryBytes = 8'000;
  snapshot.hostAvailableMemoryBytes = 4'000;
  snapshot.processRssBytes = 1'000;

  BOOST_CHECK(snapshot.isMeasured());
  BOOST_CHECK(snapshot.isFresh(2'999, 2'000));
  BOOST_CHECK(!snapshot.isFresh(3'001, 2'000));
  BOOST_CHECK(!snapshot.isFresh(999, 2'000));

  snapshot.status = ResourceProbeStatus::ReadError;
  BOOST_CHECK(!snapshot.isMeasured());
  BOOST_CHECK(!snapshot.isFresh(1'001, 2'000));
}

BOOST_AUTO_TEST_CASE(ProviderResourceSnapshotV3TopologyIsExplicit)
{
  ProviderResourceSnapshot snapshot;
  snapshot.visibleDevices = {"cuda:0", "cuda:1"};
  snapshot.topologyDigest = "sha256:topology";
  BOOST_CHECK(snapshot.hasValidTopology());
  snapshot.visibleDevices.push_back("cuda:1");
  BOOST_CHECK(!snapshot.hasValidTopology());
  snapshot.visibleDevices = {"cpu"};
  snapshot.topologyDigest = "sha256:cpu";
  BOOST_CHECK(!snapshot.hasValidTopology());
  snapshot.visibleDevices.clear();
  snapshot.topologyDigest = "sha256:zero-accelerators";
  BOOST_CHECK(snapshot.hasValidTopology());
  snapshot.topologyDigest.clear();
  BOOST_CHECK(!snapshot.hasValidTopology());
}

BOOST_AUTO_TEST_CASE(LinuxProviderResourceProbeParsesExactMemoryFacts)
{
  ProviderResourceProbeConfig config;
  config.providerName = "/provider/A";
  config.providerBootId = "boot-a";
  LinuxProviderResourceProbe probe(config, [] (const std::string& path,
                                                std::chrono::milliseconds) {
    if (path == "/proc/meminfo") {
      return ResourceTextRead{
        ResourceProbeStatus::Measured,
        "MemTotal:       8000 kB\nMemAvailable:   3000 kB\n",
        ""};
    }
    if (path == "/proc/self/status") {
      return ResourceTextRead{
        ResourceProbeStatus::Measured,
        "Name:\tdi-native\nVmRSS:\t1250 kB\n",
        ""};
    }
    return ResourceTextRead{ResourceProbeStatus::Unsupported, "", "unsupported-source"};
  });

  const auto snapshot = probe.sample(std::chrono::milliseconds(25));
  BOOST_CHECK(snapshot.isMeasured());
  BOOST_CHECK(snapshot.matchesIdentity("/provider/A", "boot-a"));
  BOOST_CHECK_EQUAL(snapshot.sequence, 1);
  BOOST_CHECK_EQUAL(snapshot.hostTotalMemoryBytes, 8'192'000);
  BOOST_CHECK_EQUAL(snapshot.hostAvailableMemoryBytes, 3'072'000);
  BOOST_CHECK_EQUAL(snapshot.processRssBytes, 1'280'000);
  BOOST_CHECK_EQUAL(snapshot.source, "linux-proc");
  BOOST_CHECK(snapshot.measuredAtMs > 0);
  BOOST_CHECK(snapshot.errorCode.empty());
}

BOOST_AUTO_TEST_CASE(LinuxProviderResourceProbeFailsClosedAndRedactsReaderErrors)
{
  const std::vector<ResourceProbeStatus> statuses = {
    ResourceProbeStatus::ReadError,
    ResourceProbeStatus::Unsupported,
    ResourceProbeStatus::TimedOut,
  };
  for (const auto status : statuses) {
    ProviderResourceProbeConfig config;
    config.providerName = "/provider/A";
    config.providerBootId = "boot-a";
    LinuxProviderResourceProbe probe(config, [status] (const std::string&,
                                                        std::chrono::milliseconds) {
      return ResourceTextRead{status, "secret=/private/raw/proc-data", "bad /secret"};
    });
    const auto snapshot = probe.sample(std::chrono::milliseconds(25));
    BOOST_CHECK_EQUAL(static_cast<int>(snapshot.status), static_cast<int>(status));
    BOOST_CHECK(!snapshot.isMeasured());
    BOOST_CHECK_EQUAL(snapshot.errorCode, "meminfo-read-failed");
    BOOST_CHECK(snapshot.errorCode.find("secret") == std::string::npos);
    BOOST_CHECK_EQUAL(snapshot.hostTotalMemoryBytes, 0);
    BOOST_CHECK_EQUAL(snapshot.hostAvailableMemoryBytes, 0);
    BOOST_CHECK_EQUAL(snapshot.processRssBytes, 0);
  }
}

BOOST_AUTO_TEST_CASE(LinuxProviderResourceProbeRejectsMalformedAndMissingIdentity)
{
  ProviderResourceProbeConfig config;
  config.providerName = "/provider/A";
  config.providerBootId = "boot-a";
  LinuxProviderResourceProbe malformed(config, [] (const std::string& path,
                                                     std::chrono::milliseconds) {
    if (path == "/proc/meminfo") {
      return ResourceTextRead{
        ResourceProbeStatus::Measured,
        "MemTotal: 8 MB\nMemTotal: 9 kB\nMemAvailable: 3 kB\n",
        ""};
    }
    return ResourceTextRead{ResourceProbeStatus::Measured, "VmRSS: 1 kB\n", ""};
  });
  const auto malformedSnapshot = malformed.sample(std::chrono::milliseconds(25));
  BOOST_CHECK_EQUAL(static_cast<int>(malformedSnapshot.status),
                    static_cast<int>(ResourceProbeStatus::MalformedInput));
  BOOST_CHECK_EQUAL(malformedSnapshot.errorCode, "proc-memory-malformed");
  BOOST_CHECK_EQUAL(malformedSnapshot.hostTotalMemoryBytes, 0);

  config.providerBootId.clear();
  bool readerCalled = false;
  LinuxProviderResourceProbe missingIdentity(
    config, [&readerCalled] (const std::string&, std::chrono::milliseconds) {
      readerCalled = true;
      return ResourceTextRead{};
    });
  const auto identitySnapshot =
    missingIdentity.sample(std::chrono::milliseconds(25));
  BOOST_CHECK_EQUAL(static_cast<int>(identitySnapshot.status),
                    static_cast<int>(ResourceProbeStatus::IdentityMismatch));
  BOOST_CHECK_EQUAL(identitySnapshot.errorCode, "probe-identity-missing");
  BOOST_CHECK(!readerCalled);
  BOOST_CHECK(!identitySnapshot.matchesIdentity("/provider/A", "boot-a"));
}

BOOST_AUTO_TEST_CASE(LinuxProviderResourceProbeBackgroundStopsAndMarksStale)
{
  ProviderResourceProbeConfig config;
  config.providerName = "/provider/A";
  config.providerBootId = "boot-a";
  config.sampleInterval = std::chrono::milliseconds(1);
  config.readTimeout = std::chrono::milliseconds(25);
  config.maximumAge = std::chrono::milliseconds(0);
  LinuxProviderResourceProbe probe(config, [] (const std::string& path,
                                                std::chrono::milliseconds) {
    return ResourceTextRead{
      ResourceProbeStatus::Measured,
      path == "/proc/meminfo" ?
        "MemTotal: 8 kB\nMemAvailable: 3 kB\n" : "VmRSS: 1 kB\n",
      ""};
  });
  probe.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  probe.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto snapshot = probe.latest();
  BOOST_CHECK(snapshot.sequence > 0);
  BOOST_CHECK_EQUAL(static_cast<int>(snapshot.status),
                    static_cast<int>(ResourceProbeStatus::Stale));
  BOOST_CHECK_EQUAL(snapshot.errorCode, "sample-stale");
}

BOOST_AUTO_TEST_CASE(LinuxProviderResourceProbeReadsLocalProc)
{
#ifdef __linux__
  ProviderResourceProbeConfig config;
  config.providerName = "/provider/local";
  config.providerBootId = "boot-local";
  LinuxProviderResourceProbe probe(config);
  const auto snapshot = probe.sample(std::chrono::milliseconds(250));
  BOOST_REQUIRE(snapshot.isMeasured());
  BOOST_CHECK(snapshot.hostTotalMemoryBytes > 0);
  BOOST_CHECK(snapshot.hostAvailableMemoryBytes > 0);
  BOOST_CHECK(snapshot.processRssBytes > 0);
#endif
}

BOOST_AUTO_TEST_CASE(NativeProviderTelemetryCollectorCachesMergedFactsAndEwma)
{
  class FakeProbe final : public ProviderResourceProbe
  {
  public:
    void start() override { ++starts; }
    void stop() noexcept override { ++stops; }
    ProviderResourceSnapshot sample(std::chrono::milliseconds) override
    {
      return latest();
    }
    ProviderResourceSnapshot latest() const override
    {
      ++reads;
      ProviderResourceSnapshot value;
      value.status = ResourceProbeStatus::Measured;
      value.source = "fixture";
      value.providerName = "/provider/A";
      value.providerBootId = "boot-a";
      value.sequence = 9;
      value.measuredAtMs = 1'000;
      value.hostTotalMemoryBytes = 8'000;
      value.hostAvailableMemoryBytes = 4'000;
      value.processRssBytes = 1'000;
      return value;
    }

    int starts = 0;
    int stops = 0;
    mutable int reads = 0;
  };

  auto probe = std::make_shared<FakeProbe>();
  int capacityReads = 0;
  NativeProviderTelemetryCollector collector(
    probe,
    [&capacityReads] {
      ++capacityReads;
      ProviderRoleWorkerSnapshot value;
      value.workerCount = 4;
      value.readyQueueDepth = 2;
      value.waitingForInputCount = 1;
      value.activeWorkerCount = 3;
      value.dependencyWaitWorkerCount = 4;
      value.dependencyWaitQueueCapacity = 1024;
      value.dependencyWaitQueuedCount = 2;
      value.dependencyWaitActiveCount = 1;
      value.dependencyWaitCompleted = 10;
      value.dependencyWaitCancelled = 2;
      value.dependencyWaitDeadlineExpired = 1;
      value.dependencyWaitFailed = 3;
      value.dependencyWaitRejected = 4;
      return value;
    },
    std::chrono::seconds(1),
    0.5);
  collector.recordStageServiceTime(std::chrono::milliseconds(100));
  collector.recordStageServiceTime(std::chrono::milliseconds(300));
  collector.refresh();
  const auto snapshot = collector.snapshot();
  BOOST_CHECK_EQUAL(probe->reads, 1);
  BOOST_CHECK_EQUAL(capacityReads, 1);
  BOOST_CHECK_EQUAL(snapshot.resources.processRssBytes, 1'000);
  BOOST_CHECK_EQUAL(snapshot.capacity.pendingWorkCount(), 6);
  BOOST_CHECK_EQUAL(snapshot.completedStages, 2);
  BOOST_CHECK_CLOSE(snapshot.stageServiceTimeEwmaMs, 200.0, 0.001);
  BOOST_CHECK_CLOSE(snapshot.stageServiceRateEwmaPerSecond,
                    6.6666666667, 0.001);

  NativeProviderReadinessState readiness;
  readiness.markReady("ready");
  readiness.setTelemetrySnapshotProvider([&collector] { return collector.snapshot(); });
  std::string lastJson;
  for (int i = 0; i < 10; ++i) {
    const auto decision = readiness.makeAckDecision(
      "/Backbone", ndn::Name("/provider/A"), ndn::Name("/service"));
    BOOST_CHECK(decision.status);
    lastJson = typedCapabilityJson(decision);
  }
  BOOST_CHECK_EQUAL(probe->reads, 1);
  BOOST_CHECK_EQUAL(capacityReads, 1);
  BOOST_CHECK(lastJson.find(
    "\"configuredCapability\":{\"schema\":\"ndnsf-di-configured-capability-v1\","
    "\"source\":\"configured\"") != std::string::npos);
  BOOST_CHECK(lastJson.find(
    "\"measuredTelemetry\":{\"schema\":\"ndnsf-di-measured-telemetry-v1\","
    "\"source\":\"fixture\",\"status\":\"measured\"") != std::string::npos);
  BOOST_CHECK(lastJson.find("\"providerBootId\":\"boot-a\"") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"resourceSequence\":9") != std::string::npos);
  BOOST_CHECK(lastJson.find("\"hostTotalMemoryBytes\":8000") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"processRssBytes\":1000") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"stageServiceTimeEwmaMs\":200") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"dependencyWaitWorkers\":4") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"dependencyWaitQueueCapacity\":1024") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"dependencyWaitQueued\":2") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"dependencyWaitCancelled\":2") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find("\"dependencyWaitRejected\":4") !=
              std::string::npos);
  BOOST_CHECK(lastJson.find(
    "\"dependencyWaitOverloadReason\":\"DEPENDENCY_WAIT_SCHEDULER_OVERLOAD\"") !=
              std::string::npos);
}

BOOST_AUTO_TEST_CASE(ExecutionEvidenceRoundTripsAndExcludesSecrets)
{
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/A";
  evidence.providerBootId = "boot-a";
  evidence.evidenceEpoch = 3;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  evidence.realCompute = true;
  evidence.deviceKind = "cuda";
  evidence.deviceId = "multi";
  evidence.deviceIds = {"GPU-0", "GPU-1"};
  evidence.runtimeVersion = "onnxruntime=1;cuda=1";
  evidence.modelDigest = "sha256:model";
  evidence.planDigest = "sha256:plan";
  evidence.artifactDigests["/LLM/Stage/0"] = "sha256:stage0";
  evidence.roles = {"/LLM/Stage/0"};
  evidence.loadCompleted = true;
  evidence.warmupCompleted = true;
  evidence.gpuUuid = "multi";
  evidence.gpuUuids = {"GPU-uuid-0", "GPU-uuid-1"};
  evidence.createdAtMs = 1234;
  const auto json = executionEvidenceToJson(evidence);
  BOOST_CHECK(json.find("token") == std::string::npos);
  BOOST_CHECK(json.find("prompt") == std::string::npos);
  const auto decoded = executionEvidenceFromJson(json);
  BOOST_CHECK_EQUAL(decoded.providerBootId, "boot-a");
  BOOST_CHECK(decoded.runnerKind == RunnerKind::OnnxRuntimeCuda);
  BOOST_CHECK_EQUAL(decoded.deviceId, "multi");
  BOOST_REQUIRE_EQUAL(decoded.deviceIds.size(), 2);
  BOOST_CHECK_EQUAL(decoded.deviceIds.at(1), "GPU-1");
  BOOST_CHECK_EQUAL(decoded.gpuUuid, "multi");
  BOOST_REQUIRE_EQUAL(decoded.gpuUuids.size(), 2);
  BOOST_CHECK_EQUAL(decoded.artifactDigests.at("/LLM/Stage/0"), "sha256:stage0");
  BOOST_CHECK(decoded.loadCompleted);
  BOOST_CHECK(decoded.warmupCompleted);
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeReadinessRequiresExactCudaLoadAndWarmup)
{
  auto validEvidence = [] {
    ExecutionEvidence evidence;
    evidence.providerName = "/provider/A";
    evidence.providerBootId = "boot-a";
    evidence.evidenceEpoch = 3;
    evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
    evidence.realCompute = true;
    evidence.deviceKind = "cuda";
    evidence.deviceId = "0";
    evidence.runtimeVersion = "onnxruntime=1;cuda=1";
    evidence.modelDigest = "sha256:model";
    evidence.planDigest = "sha256:plan";
    evidence.artifactDigests["stage-0"] = "sha256:stage0";
    evidence.roles = {"stage-0"};
    evidence.loadCompleted = true;
    evidence.warmupCompleted = true;
    evidence.createdAtMs = 1234;
    return evidence;
  };

  const auto accepted = validateNativeProviderRuntimeReadiness(
    validEvidence(), "stage-0", "onnxruntime-cuda", "cuda:0", "sha256:stage0");
  BOOST_CHECK(!accepted);
  const auto acceptedUppercaseDigest = validateNativeProviderRuntimeReadiness(
    validEvidence(), "stage-0", "onnxruntime-cuda", "cuda:0", "SHA256:STAGE0");
  BOOST_CHECK(!acceptedUppercaseDigest);

  auto expectReason = [&] (ExecutionEvidence evidence,
                           const std::string& backend,
                           const std::string& device,
                           const std::string& artifact,
                           const std::string& expectedReason) {
    const auto result = validateNativeProviderRuntimeReadiness(
      evidence, "stage-0", backend, device, artifact);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(*result, expectedReason);
  };

  auto evidence = validEvidence();
  evidence.cpuFallbackUsed = true;
  expectReason(evidence, "onnxruntime-cuda", "cuda:0", "sha256:stage0",
               "DI_RUNTIME_CUDA_REQUIRED");

  evidence = validEvidence();
  evidence.loadCompleted = false;
  evidence.warmupCompleted = false;
  expectReason(evidence, "onnxruntime-cuda", "cuda:0", "sha256:stage0",
               "DI_RUNTIME_MODEL_NOT_LOADED");

  evidence = validEvidence();
  evidence.warmupCompleted = false;
  expectReason(evidence, "onnxruntime-cuda", "cuda:0", "sha256:stage0",
               "DI_RUNTIME_WARMUP_INCOMPLETE");

  expectReason(validEvidence(), "pytorch", "cuda:0", "sha256:stage0",
               "DI_RUNTIME_BACKEND_MISMATCH");
  expectReason(validEvidence(), "onnxruntime-cuda", "cuda:1", "sha256:stage0",
               "DI_RUNTIME_DEVICE_MISMATCH");
  expectReason(validEvidence(), "onnxruntime-cuda", "cuda:0", "sha256:other",
               "DI_RUNTIME_ARTIFACT_MISMATCH");

  const auto incomplete = validateNativeProviderRuntimeReadiness(
    validEvidence(), "stage-0", "", "cuda:0", "sha256:stage0");
  BOOST_REQUIRE(incomplete);
  BOOST_CHECK_EQUAL(*incomplete, "DI_RUNTIME_ASSIGNMENT_INCOMPLETE");

  evidence = validEvidence();
  evidence.loadCompleted = false;
  const auto invalid = validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime-cuda", "cuda:0", "sha256:stage0");
  BOOST_REQUIRE(invalid);
  BOOST_CHECK_EQUAL(*invalid, "DI_RUNTIME_EVIDENCE_INVALID");
}

BOOST_AUTO_TEST_CASE(NativeProviderRuntimeReadinessAcceptsCanonicalCpuDeviceId)
{
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/A";
  evidence.providerBootId = "boot-a";
  evidence.evidenceEpoch = 3;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
  evidence.realCompute = true;
  evidence.deviceKind = "cpu";
  evidence.deviceId = "cpu0";
  evidence.runtimeVersion = "onnxruntime=1.20.1";
  evidence.modelDigest = "sha256:model";
  evidence.planDigest = "sha256:plan";
  evidence.artifactDigests["stage-0"] = "sha256:stage0";
  evidence.roles = {"stage-0"};
  evidence.loadCompleted = true;
  evidence.warmupCompleted = true;
  evidence.createdAtMs = 1234;

  const auto accepted = validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime", "cpu", "sha256:stage0");
  BOOST_CHECK(!accepted);

  evidence.cpuFallbackUsed = true;
  const auto fallback = validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime", "cpu", "sha256:stage0");
  BOOST_REQUIRE(fallback);
  BOOST_CHECK_EQUAL(*fallback, "DI_RUNTIME_CPU_FALLBACK_USED");

  evidence.cpuFallbackUsed = false;
  const auto wrongDevice = validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime", "cpu:1", "sha256:stage0");
  BOOST_REQUIRE(wrongDevice);
  BOOST_CHECK_EQUAL(*wrongDevice, "DI_RUNTIME_DEVICE_MISMATCH");

  evidence.deviceId = "0";
  BOOST_CHECK(!validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime", "cpu:0", "sha256:stage0"));
  evidence.deviceId = "cpu1";
  const auto mismatch = validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime", "cpu:0", "sha256:stage0");
  BOOST_REQUIRE(mismatch);
  BOOST_CHECK_EQUAL(*mismatch, "DI_RUNTIME_DEVICE_MISMATCH");

  evidence.runnerKind = RunnerKind::NativeYoloPostprocess;
  evidence.realCompute = false;
  evidence.loadCompleted = false;
  evidence.warmupCompleted = false;
  evidence.deviceKind = "cpu";
  evidence.deviceId = "native";
  BOOST_CHECK(!validateNativeProviderRuntimeReadiness(
    evidence, "stage-0", "onnxruntime-cpu", "cpu:0", "sha256:stage0"));
  BOOST_CHECK_EQUAL(
    validateNativeProviderRuntimeReadiness(
      evidence, "stage-0", "onnxruntime-cpu", "cuda:0", "sha256:stage0")
      .value_or(""), "DI_RUNTIME_NATIVE_MERGE_ASSIGNMENT_MISMATCH");
}

BOOST_AUTO_TEST_CASE(ExecutionEvidenceRejectsMissingUnknownAndSecretFields)
{
  BOOST_CHECK_THROW(executionEvidenceFromJson("{\"schema\":\"ndnsf-di-execution-evidence-v2\"}"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(executionEvidenceFromJson("{\"schema\":\"ndnsf-di-execution-evidence-v1\"}"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(executionEvidenceFromJson(
    "{\"schema\":\"ndnsf-di-execution-evidence-v1\",\"token\":\"secret\"}"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ExecutionAttemptAndTerminalReasonRoundTrip)
{
  ExecutionAttemptMetadata attempt;
  attempt.requestId = "req-1";
  attempt.attemptEpoch = 1;
  attempt.planId = "plan-1";
  attempt.terminalReason = TerminalReason::ProviderLost;
  const auto decoded = executionAttemptFromJson(executionAttemptToJson(attempt));
  BOOST_CHECK_EQUAL(decoded.requestId, "req-1");
  BOOST_CHECK_EQUAL(decoded.attemptEpoch, 1);
  BOOST_CHECK(decoded.terminalReason == TerminalReason::ProviderLost);
  attempt.attemptEpoch = 2;
  BOOST_CHECK_THROW(executionAttemptToJson(attempt), std::invalid_argument);
  BOOST_CHECK_THROW(terminalReasonFromString("MAGIC_FAILURE"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeRunnerFactoryPreservesObservedExecutionEvidence)
{
  auto makeEvidence = [] (RunnerKind kind, bool real, std::string device) {
    ExecutionEvidence evidence;
    evidence.providerName = "/provider/A";
    evidence.providerBootId = "boot-a";
    evidence.runnerKind = kind;
    evidence.realCompute = real;
    evidence.deviceKind = real ? "cpu" : "synthetic";
    evidence.deviceId = std::move(device);
    evidence.runtimeVersion = "test-runtime";
    evidence.modelDigest = "sha256:model";
    evidence.planDigest = "sha256:plan";
    evidence.artifactDigests["/role"] = "sha256:artifact";
    evidence.roles = {"/role"};
    evidence.createdAtMs = 1;
    return evidence;
  };
  auto noop = [] (const RoleExecutionContext&) {
    return std::map<std::string, TensorBundle>{};
  };
  auto synthetic = makeNativeModelRunner(noop,
    makeEvidence(RunnerKind::SyntheticDelay, false, ""));
  auto wiring = makeNativeModelRunner(noop,
    makeEvidence(RunnerKind::WiringOnly, false, ""));
  auto cpu = makeNativeModelRunner(noop,
    makeEvidence(RunnerKind::OnnxRuntimeCpu, true, "cpu0"));
  auto cudaEvidence = makeEvidence(RunnerKind::OnnxRuntimeCuda, true, "GPU-1");
  cudaEvidence.deviceKind = "cuda";
  auto cuda = makeNativeModelRunner(noop, std::move(cudaEvidence));
  BOOST_REQUIRE(synthetic->executionEvidence());
  BOOST_REQUIRE(wiring->executionEvidence());
  BOOST_REQUIRE(cpu->executionEvidence());
  BOOST_REQUIRE(cuda->executionEvidence());
  BOOST_CHECK(synthetic->executionEvidence()->runnerKind == RunnerKind::SyntheticDelay);
  BOOST_CHECK(wiring->executionEvidence()->runnerKind == RunnerKind::WiringOnly);
  BOOST_CHECK(cpu->executionEvidence()->runnerKind == RunnerKind::OnnxRuntimeCpu);
  BOOST_CHECK(cuda->executionEvidence()->runnerKind == RunnerKind::OnnxRuntimeCuda);
}

BOOST_AUTO_TEST_CASE(QwenGenerationResourceQueuesAreBoundedAndObservable)
{
  QwenGenerationResourceLimits limits;
  limits.generationCapacity = 1;
  limits.requestCapacity = 1;
  limits.waitCapacity = 1;
  limits.callbackCapacity = 1;
  limits.tokenPairCapacity = 1;
  limits.assignmentCapacity = 1;
  limits.tensorCapacity = 1;
  limits.metricsCapacity = 1;
  QwenGenerationResourceLedger ledger(limits);

  for (const auto kind : {
         QwenResourceKind::Generation,
         QwenResourceKind::Request,
         QwenResourceKind::Wait,
         QwenResourceKind::Callback,
         QwenResourceKind::TokenPair,
         QwenResourceKind::Assignment,
         QwenResourceKind::Tensor,
         QwenResourceKind::Metrics,
       }) {
    const auto accepted = ledger.tryAcquire(kind);
    BOOST_CHECK(accepted.accepted);
    BOOST_CHECK_EQUAL(accepted.reason, "ACCEPTED");
    const auto rejected = ledger.tryAcquire(kind);
    BOOST_CHECK(!rejected.accepted);
    BOOST_CHECK_EQUAL(rejected.reason, "QUEUE_FULL");
    const auto snapshot = ledger.snapshot(kind);
    BOOST_CHECK_EQUAL(snapshot.occupancy, 1);
    BOOST_CHECK_EQUAL(snapshot.capacity, 1);
    BOOST_CHECK_EQUAL(snapshot.rejected, 1);
    ledger.release(kind);
    BOOST_CHECK_EQUAL(ledger.snapshot(kind).occupancy, 0);
    BOOST_CHECK_THROW(ledger.release(kind), std::logic_error);
  }
}

BOOST_AUTO_TEST_CASE(QwenGenerationResourceLimitsRejectZeroOrUnboundedCapacity)
{
  QwenGenerationResourceLimits limits;
  limits.tensorCapacity = 0;
  BOOST_CHECK_THROW(QwenGenerationResourceLedger invalid(limits),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeEpochCoordinatorProducesTextAndTerminalFeedback)
{
  struct GenerationRun
  {
    NativeEpochCoordinatorResult result;
    std::vector<std::string> events;
    std::vector<std::string> ioEvents;
    std::shared_ptr<BlockingDependencyIo> io;
  };

  const auto run = [] (std::vector<std::vector<float>> logitsByEpoch,
                       std::vector<std::string> stopStrings,
                       std::string samplingMode = "Greedy",
                       std::uint64_t samplingSeed = 1'750'001,
                       bool stateful = false) {
    NativeProviderRuntime runtime(1);
    const auto maxEpochs = logitsByEpoch.size();
    const auto identity = exactStateIdentity();
    NativeModelRunnerSpec runnerSpec;
    runnerSpec.role = identity.roleName;
    runnerSpec.kind = "onnx-model";
    runnerSpec.backend = "test-native-terminal";
    runnerSpec.path = "/tmp/native-terminal.onnx";
    runnerSpec.metadata = {
      {"evidence.modelDigest", identity.modelDigest},
      {"evidence.planDigest", stateDigest('0')},
      {"evidence.providerName", identity.providerIdentity},
      {"evidence.providerBootId", identity.providerBootId},
      {"state.securityEpoch", "7"},
      {"state.generationId", identity.generationId},
      {"state.schemaDigest", identity.stateSchemaDigest},
    };

    auto runner = makeNativeModelRunner(
        [logitsByEpoch = std::move(logitsByEpoch), stateful]
        (const RoleExecutionContext& ctx) {
          BOOST_REQUIRE(ctx.inferenceEpoch < logitsByEpoch.size());
          if (stateful) {
            BOOST_REQUIRE(ctx.streamingStateExecution);
          }
          const auto& values = logitsByEpoch[ctx.inferenceEpoch];
          NamedTensor logits;
          logits.name = "logits";
          logits.elementType = TensorElementType::Float32;
          logits.shape = {1, static_cast<std::int64_t>(values.size())};
          logits.payload = floatPayload({});
          logits.payload.resize(values.size() * sizeof(float));
          std::memcpy(logits.payload.data(), values.data(), logits.payload.size());
          std::vector<NamedTensor> outputs;
          outputs.push_back(std::move(logits));
          if (stateful) {
            outputs.push_back(NamedTensor{
              "attention_kv_out", TensorElementType::Int64, {1},
              rawTensorPayload<std::int64_t>({
                static_cast<std::int64_t>(ctx.inferenceEpoch + 1)})});
          }
          return std::map<std::string, TensorBundle>{
            {"onnx-output-bundle",
             makeEncodedTensorBundle("onnx-output-bundle", std::move(outputs))},
          };
        });
    runtime.registerRunner(runnerSpec, runner);

    NativeExecutionPlan plan;
    plan.roles = {runnerSpec.role};
    NativeDependencySpec feedback;
    feedback.producers = {runnerSpec.role};
    feedback.consumers = {runnerSpec.role};
    feedback.keyScope = "token-feedback";
    feedback.topicPrefix = "/ndnsf-di";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
    feedback.operationKind = "TOKEN_FEEDBACK";
    plan.dependencies = {feedback};

    NativeProviderAssignment assignment;
    assignment.providerByRole[runnerSpec.role] = identity.providerIdentity;
    auto io = std::make_shared<BlockingDependencyIo>();
    std::vector<std::string> events;
    std::vector<std::string> ioEvents;
    io->prefetchObserver = [&ioEvents] (const std::string& name) {
      ioEvents.push_back("prefetch:" + name);
    };
    io->publishObserver = [&ioEvents] (const std::string& name) {
      ioEvents.push_back("publish:" + name);
    };

    NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
    config.sessionId = "native-terminal-session";
    config.requestId = "native-terminal-request";
    config.attemptEpoch = 1;
    config.lineagePlanDigest = stateDigest('0');
    config.localProvider = identity.providerIdentity;
    config.role = runnerSpec.role;
    config.initialInputs = {
      {"input_ids",
       makeEncodedTensorBundle(
         "prompt",
         {NamedTensor{"input_ids", TensorElementType::Int64, {1, 3},
                      rawTensorPayload<std::int64_t>({11, 12, 13})}})},
    };
    config.maxEpochs = maxEpochs;
    if (stateful) {
      config.stateInputNames = {"attention_kv_in"};
      config.stateOutputNames = {"attention_kv_out"};
    }
    config.stateIdentityTemplate = identity;
    config.positionPolicyDigest = identity.positionDigest;
    config.samplingDigest = "sha256:native-terminal-sampling";
    config.prepareRunner = [runner] { return runner; };
    config.samplingMode = std::move(samplingMode);
    config.samplingSeed = samplingSeed;
    config.stopStrings = std::move(stopStrings);
    if (config.samplingMode == "SeededTopKTopP") {
      config.samplingTemperature = 1.0;
      config.samplingTopK = 3;
      config.samplingTopP = 1.0;
    }
    config.requireTextOutput = true;
    const auto decodeText = [] (const std::vector<std::int64_t>& tokens) {
      std::string text;
      for (const auto token : tokens) {
        switch (token) {
          case 1: text += "你"; break;
          case 2: text += "好"; break;
          case 3: text += "🙂"; break;
          default: text += "?"; break;
        }
      }
      return text;
    };
    config.textDecoder = decodeText;
    config.stableTextDecoder = [decodeText] (
      const std::vector<std::int64_t>& tokens, bool) {
      return decodeText(tokens);
    };
    config.eventSink = [&events] (const std::vector<std::uint8_t>& wire) {
      events.emplace_back(wire.begin(), wire.end());
      return true;
    };

    auto result = runNativeEpochCoordinator(std::move(config));
    return GenerationRun{std::move(result), std::move(events),
                         std::move(ioEvents), std::move(io)};
  };

  const auto maxTokens = run({
    {0.0F, 5.0F, 0.0F, 0.0F},
    {0.0F, 0.0F, 5.0F, 0.0F},
  }, {}, "Greedy", 1'750'001, true);
  BOOST_REQUIRE(maxTokens.result.finalPayload.has_value());
  const auto maxPayload = payloadText(TensorBundle{
    "final", *maxTokens.result.finalPayload, 0});
  BOOST_CHECK(maxPayload.find("\"finishReason\":\"max_tokens\"") !=
              std::string::npos);
  BOOST_CHECK(maxPayload.find("\"text\":\"你好\"") != std::string::npos);
  BOOST_REQUIRE_EQUAL(maxTokens.ioEvents.size(), 3U);
  BOOST_CHECK(maxTokens.ioEvents[0].find("prefetch:/provider/A/NDNSF/DI/FEEDBACK/") == 0);
  BOOST_CHECK(maxTokens.ioEvents[0].find("/bundle/1") != std::string::npos);
  BOOST_CHECK_EQUAL(maxTokens.ioEvents[1],
                    "publish:" + maxTokens.ioEvents[0].substr(9));
  BOOST_CHECK(maxTokens.ioEvents[2].find("publish:/provider/A/NDNSF/DI/FEEDBACK/") == 0);
  BOOST_CHECK(maxTokens.ioEvents[2].find("/bundle/2") != std::string::npos);
  BOOST_REQUIRE_EQUAL(maxTokens.events.size(), 2U);
  BOOST_CHECK(maxTokens.events[0].find("\"textDelta\":\"你\"") !=
              std::string::npos);
  BOOST_CHECK(maxTokens.events[1].find("\"textDelta\":\"好\"") !=
              std::string::npos);

  bool terminalFeedbackSeen = false;
  for (const auto& item : maxTokens.io->available) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    const auto tensors = decodeTensorBundle(item.second.payload);
    const auto marker = std::find_if(
      tensors.begin(), tensors.end(), [] (const auto& tensor) {
        return tensor.name == "__ndnsf_token_feedback_terminal";
      });
    if (marker != tensors.end() && marker->elementType == TensorElementType::Bool &&
        marker->payload.size() == 1 && marker->payload.front() != 0) {
      terminalFeedbackSeen = true;
    }
  }
  BOOST_CHECK(terminalFeedbackSeen);

  const auto stopped = run({
    {0.0F, 5.0F, 0.0F, 0.0F},
    {0.0F, 0.0F, 5.0F, 0.0F},
    {0.0F, 0.0F, 0.0F, 5.0F},
  }, {"好🙂"});
  BOOST_REQUIRE(stopped.result.finalPayload.has_value());
  const auto stopPayload = payloadText(TensorBundle{
    "final", *stopped.result.finalPayload, 0});
  BOOST_CHECK(stopPayload.find("\"finishReason\":\"stop_sequence\"") !=
              std::string::npos);
  BOOST_CHECK(stopPayload.find("\"text\":\"你好🙂\"") != std::string::npos);
  BOOST_CHECK_EQUAL(stopped.result.epochsExecuted, 3U);
  BOOST_CHECK_EQUAL(stopped.result.eventsPublished, 3U);

  const auto seededA = run({
    {1.0F, 2.0F, 3.0F, 0.0F},
    {2.0F, 1.0F, 3.0F, 0.0F},
    {0.0F, 3.0F, 2.0F, 1.0F},
  }, {}, "SeededTopKTopP", 42);
  const auto seededB = run({
    {1.0F, 2.0F, 3.0F, 0.0F},
    {2.0F, 1.0F, 3.0F, 0.0F},
    {0.0F, 3.0F, 2.0F, 1.0F},
  }, {}, "SeededTopKTopP", 42);
  BOOST_REQUIRE_EQUAL(seededA.events.size(), seededB.events.size());
  BOOST_CHECK(seededA.events == seededB.events);
  BOOST_CHECK(std::all_of(
    seededA.events.begin(), seededA.events.end(), [] (const auto& event) {
      return event.find("sha256:native-terminal-sampling") != std::string::npos;
    }));
}

NativeEpochCoordinatorResult runSamplingEpochs(
  std::vector<std::vector<float>> logits,
  const std::function<void(NativeEpochCoordinatorConfig&)>& configure)
{
  NativeProviderRuntime runtime(1);
  const auto identity = exactStateIdentity();
  NativeModelRunnerSpec spec;
  spec.role = identity.roleName; spec.kind = "onnx-model";
  spec.backend = "test-sampling"; spec.path = "/fixture/sampling.onnx";
  spec.metadata = {{"evidence.modelDigest", identity.modelDigest},
    {"evidence.planDigest", stateDigest('0')}, {"evidence.providerName", identity.providerIdentity},
    {"evidence.providerBootId", identity.providerBootId}, {"state.securityEpoch", "7"},
    {"state.generationId", identity.generationId}, {"state.schemaDigest", identity.stateSchemaDigest}};
  const auto count = logits.size();
  auto samplingRunner = makeNativeModelRunner(
    [logits = std::move(logits)](const RoleExecutionContext& context) {
      const auto& values = logits.at(context.inferenceEpoch);
      NamedTensor tensor;
      tensor.name = "logits"; tensor.elementType = TensorElementType::Float32;
      tensor.shape = {1, static_cast<std::int64_t>(values.size())};
      tensor.payload.resize(values.size() * sizeof(float));
      if (!tensor.payload.empty()) std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      return std::map<std::string, TensorBundle>{{"onnx-output-bundle",
        makeEncodedTensorBundle("onnx-output-bundle", {std::move(tensor)})}};
    });
  runtime.registerRunner(spec, samplingRunner);
  NativeExecutionPlan plan; plan.roles = {spec.role};
  NativeDependencySpec feedback;
  feedback.producers = feedback.consumers = {spec.role};
  feedback.keyScope = "token-feedback"; feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate = "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK"; plan.dependencies = {feedback};
  NativeProviderAssignment assignment; assignment.providerByRole[spec.role] = identity.providerIdentity;
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, std::make_shared<BlockingDependencyIo>()};
  config.sessionId = "sampling-session"; config.requestId = "sampling-request";
  config.localProvider = identity.providerIdentity; config.role = spec.role;
  config.lineagePlanDigest = stateDigest('0'); config.maxEpochs = count;
  config.stateIdentityTemplate = identity; config.positionPolicyDigest = identity.positionDigest;
  config.samplingDigest = stateDigest('1');
  config.initialInputs = {{"input_ids", makeEncodedTensorBundle("prompt", {
    NamedTensor{"input_ids", TensorElementType::Int64, {1, 3}, rawTensorPayload<std::int64_t>({11, 12, 13})}})}};
  // NativeEpochCoordinator always uses the prepared-role seam.  Keep this
  // fixture on the same production boundary while reusing the deterministic
  // runner registered above; a missing callback would test only the guard and
  // fail before sampling behavior is exercised.
  config.prepareRunner = [samplingRunner] { return samplingRunner; };
  configure(config);
  return runNativeEpochCoordinator(std::move(config));
}

} // namespace ndnsf::di::test
