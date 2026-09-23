#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <vector>

namespace ndnsf::di::test {
namespace {
template<typename T> std::vector<std::uint8_t> raw(std::initializer_list<T> values)
{
  std::vector<std::uint8_t> result(values.size() * sizeof(T));
  std::memcpy(result.data(), values.begin(), result.size());
  return result;
}
std::string hash(char c) { return "sha256:" + std::string(64, c); }
// Exact immutable names, bounded waits, no files or model artifacts.
class EpochIo final : public DependencyIo
{
public:
  std::future<TensorBundle> prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    std::promise<TensorBundle> promise;
    auto future = promise.get_future();
    std::unique_lock<std::mutex> lock(mutex);
    fetchedEdges.push_back(edge);
    if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return objects.count(edge.plannedDataName); }))
      promise.set_exception(std::make_exception_ptr(std::runtime_error("epoch fixture fetch timeout")));
    else promise.set_value(objects.at(edge.plannedDataName));
    return future;
  }
  void publishOutput(const std::string&, const DependencyEdge& edge, const TensorBundle& bundle) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    publishedEdges.push_back(edge);
    if (failFinalPublish && edge.plannedDataName == "/test/activation/1")
      throw std::runtime_error("fixture finalization publish failure");
    if (!objects.emplace(edge.plannedDataName, bundle).second)
      throw std::runtime_error("immutable epoch object overwritten");
    changed.notify_all();
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, TensorBundle> objects;
  std::vector<DependencyEdge> fetchedEdges;
  std::vector<DependencyEdge> publishedEdges;
  bool failFinalPublish = false;
};
DecodeStateIdentityV1 identityFor(unsigned stage)
{
  DecodeStateIdentityV1 id;
  id.modelDigest = hash('1'); id.graphSemanticDigest = hash('2');
  id.artifactDigest = hash('3'); id.adapterDigest = hash('4');
  id.tokenizerDigest = hash('5'); id.runnerDigest = hash('6');
  id.roleName = "/Stage/" + std::to_string(stage); id.roleSplitDigest = hash('7');
  id.layerBegin = stage; id.layerEnd = stage + 1;
  id.prefixDigest = hash('8'); id.prefixTokenCount = 1;
  id.positionDigest = hash('9'); id.precision = "fp32";
  id.layoutDigest = hash('a'); id.stateSchemaDigest = hash('b');
  id.stateComponentDigests = {hash('c')}; id.runtimeAbiDigest = hash('d');
  id.securityDomainDigest = hash('e'); id.providerIdentity = "/provider/" + std::to_string(stage);
  id.providerBootId = "boot"; id.cacheEpoch = 1; id.requestId = "request";
  id.attemptEpoch = 1; id.generationId = "generation";
  id.validate(); return id;
}
}

BOOST_AUTO_TEST_CASE(Spec189TwoRoleFinalizationCarriesRealActivation)
{
  // 0: empty feedback digest; 1-4: existing finalization failure boundaries;
  // 5: correct feedback digest; 6: conflicting feedback digest; 7: ordinary
  // STOP; 8: unrelated nonempty PIPELINE digest must remain untouched.
  for (unsigned failure = 0; failure != 9; ++failure) {
  BOOST_TEST_CONTEXT("finalization scenario=" << failure) {
  // Observations/IO outlive both runtimes and the joined coordinators.
  std::array<unsigned, 2> calls{{0, 0}};
  std::array<unsigned, 2> preparations{{0, 0}};
  std::array<bool, 2> restored{{false, false}};
  unsigned events = 0;
  auto io = std::make_shared<EpochIo>();
  io->failFinalPublish = failure == 4;
  NativeProviderRuntime first(1), last(1);
  NativeExecutionPlan plan;
  plan.roles = {"/Stage/0", "/Stage/1"};
  NativeDependencySpec activation;
  activation.producers = {plan.roles[0]}; activation.consumers = {plan.roles[1]};
  activation.keyScope = "activation"; activation.topicPrefix = "/test";
  activation.objectNameTemplate = "/test/activation/{sequence}";
  activation.operationKind = "PIPELINE"; activation.tensors = {"hidden"};
  NativeDependencySpec feedback;
  feedback.producers = {plan.roles[1]}; feedback.consumers = {plan.roles[0]};
  feedback.keyScope = "feedback"; feedback.topicPrefix = "/test";
  feedback.objectNameTemplate = "/test/feedback/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  plan.dependencies = {activation, feedback};
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{plan.roles[0], "/provider/0"}, {plan.roles[1], "/provider/1"}};
  auto configure = [&](unsigned stage, NativeProviderRuntime& runtime) {
    const auto identity = identityFor(stage);
    NativeModelRunnerSpec spec;
    spec.role = identity.roleName; spec.kind = "onnx-model";
    spec.backend = "test-finalization"; spec.path = "/fixture/no-file";
    spec.metadata = {{"evidence.modelDigest", identity.modelDigest},
      {"evidence.planDigest", hash('0')}, {"evidence.providerName", identity.providerIdentity},
      {"evidence.providerBootId", identity.providerBootId}, {"state.securityEpoch", "1"},
      {"state.generationId", identity.generationId}, {"state.schemaDigest", identity.stateSchemaDigest}};
    auto runner = makeNativeModelRunner([&, stage](const RoleExecutionContext& context) {
      const auto call = calls[stage]++;
      if (call > 1) throw std::runtime_error("extra model execution");
      if (call == 1 && ((failure == 1 && stage == 0) || (failure == 2 && stage == 1)))
        throw std::runtime_error("fixture finalization runner failure");
      const auto cached = context.inputsByScope.find("__ndnsf_provider_decode_state");
      if (call == 1) {
        if (cached == context.inputsByScope.end()) throw std::runtime_error("state not restored");
        const auto tensors = decodeTensorBundle(cached->second.payload);
        restored[stage] = findTensor(tensors, "kv_in").payload == raw<std::int64_t>({1});
      }
      if (stage == 1) {
        const auto tensors = decodeTensorBundle(context.inputsByScope.at("activation").payload);
        if (findTensor(tensors, "hidden").payload != raw<std::int64_t>({static_cast<std::int64_t>(call + 1)}))
          throw std::runtime_error("activation did not advance with epoch");
      }
      std::vector<NamedTensor> tensors{
        {"kv_out", TensorElementType::Int64, {1}, raw<std::int64_t>({static_cast<std::int64_t>(call + 1)})}};
      if (stage == 0)
        tensors.push_back({"hidden", TensorElementType::Int64, {1}, raw<std::int64_t>({static_cast<std::int64_t>(call + 1)})});
      else tensors.push_back({"logits", TensorElementType::Float32, {1, 2}, raw<float>({0, 1})});
      return std::map<std::string, TensorBundle>{{"activation", makeEncodedTensorBundle("activation", tensors)}};
    });
    runtime.registerRunner(spec, runner);
    NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
    config.sessionId = "session"; config.requestId = "request"; config.role = spec.role;
    config.localProvider = identity.providerIdentity; config.lineagePlanDigest = hash('0');
    config.maxEpochs = 1; config.checkpointFinalize = failure != 7;
    config.attemptEpoch = 1;
    // Match the Handler's mixed V3/legacy factory boundary. Only feedback
    // needs normalization; pipeline identity remains owned by its projection.
    config.roleSpecFactory = [&, stage](std::size_t sequence) {
      auto role = roleSpecFor(plan, plan.roles[stage], "session", assignment,
                              "/provider/" + std::to_string(stage), sequence);
      const auto configureEdge = [&](DependencyEdge& edge) {
        if (edge.operationKind == "TOKEN_FEEDBACK") {
          edge.requestId.clear();
          edge.attemptEpoch = 0;
          edge.planDigest = failure == 5 ? hash('0') : failure == 6 ? hash('f') : "";
        }
        else if (edge.operationKind == "PIPELINE") {
          edge.planDigest = failure == 8 ? hash('e') : "";
        }
      };
      for (auto& edge : role.inputs) configureEdge(edge);
      for (auto& edge : role.outputs) configureEdge(edge);
      return role;
    };
    config.stateIdentityTemplate = identity; config.positionPolicyDigest = identity.positionDigest;
    config.samplingDigest = hash('1'); config.stateInputNames = {"kv_in"}; config.stateOutputNames = {"kv_out"};
    config.prepareRunner = [&, stage, runner] { ++preparations[stage]; return runner; };
    if (stage == 0 && failure == 3) {
      // Remove the staged candidate after execution but before coordinator
      // commit. This exercises the production commit-failure path directly.
      config.resultObserver = [&runtime](const RoleSpec& role, const ProviderRoleResult&) {
        if (role.inferenceEpoch == 1) runtime.releaseDecodeState("session", role.role);
      };
    }
    if (stage == 0)
      config.initialInputs = {{"prompt", makeEncodedTensorBundle("prompt", {
        {"input_ids", TensorElementType::Int64, {1, 1}, raw<std::int64_t>({7})}})}};
    else config.eventSink = [&](const std::vector<std::uint8_t>&) { ++events; return true; };
    return config;
  };
  auto a = configure(0, first), b = configure(1, last);
  auto firstRun = std::async(std::launch::async, [a = std::move(a)]() mutable { return runNativeEpochCoordinator(std::move(a)); });
  auto lastRun = std::async(std::launch::async, [b = std::move(b)]() mutable { return runNativeEpochCoordinator(std::move(b)); });
  std::optional<NativeEpochCoordinatorResult> firstResult, lastResult;
  std::array<std::string, 2> errors;
  try { firstResult = firstRun.get(); } catch (const std::exception& e) { errors[0] = e.what(); }
  try { lastResult = lastRun.get(); } catch (const std::exception& e) { errors[1] = e.what(); }
  for (unsigned stage = 0; stage != 2; ++stage)
    BOOST_CHECK_EQUAL(preparations[stage], failure == 6 ? 0U : 1U);
  if (failure == 6) {
    BOOST_CHECK(!firstResult); BOOST_CHECK(!lastResult);
    for (const auto& error : errors)
      BOOST_CHECK_MESSAGE(error.find("GENERATION_FEEDBACK_PLAN_DIGEST_MISMATCH") != std::string::npos, error);
    BOOST_CHECK_EQUAL(calls[0], 0); BOOST_CHECK_EQUAL(calls[1], 0);
    BOOST_CHECK_EQUAL(events, 0);
    BOOST_CHECK(io->fetchedEdges.empty()); BOOST_CHECK(io->publishedEdges.empty());
    continue;
  }
  if (failure >= 1 && failure <= 4) {
    BOOST_CHECK_EQUAL(events, 1);
    BOOST_CHECK(!lastResult);
    const auto failingStage = failure == 2 ? 1U : 0U;
    const auto expected = failure <= 2 ? "fixture finalization runner failure" :
      failure == 3 ? "PROVIDER_DECODE_STATE_COMMIT_FAILED" : "fixture finalization publish failure";
    BOOST_CHECK_MESSAGE(errors[failingStage].find(expected) != std::string::npos, errors[failingStage]);
    BOOST_CHECK_EQUAL(first.conversationStateSnapshot().entries, 0);
    BOOST_CHECK_EQUAL(last.conversationStateSnapshot().entries, 0);
    BOOST_CHECK_EQUAL((failingStage == 0 ? first : last).decodeStateSnapshot().entries, 0);
    // There is no complete role set to promote. Drain the surviving role's
    // request state as the request owner would do on aggregate failure.
    first.releaseDecodeState("session", "/Stage/0");
    last.releaseDecodeState("session", "/Stage/1");
    continue;
  }
  BOOST_REQUIRE_MESSAGE(firstResult, errors[0]);
  BOOST_REQUIRE_MESSAGE(lastResult, errors[1]);
  const auto& aResult = *firstResult;
  const auto& bResult = *lastResult;
  BOOST_CHECK_EQUAL(events, 1);
  // Both coordinators have joined: no IO writer can race these observations.
  for (const auto* edges : {&io->fetchedEdges, &io->publishedEdges}) {
    unsigned feedbackCount = 0, pipelineCount = 0;
    for (const auto& edge : *edges) {
      if (edge.operationKind == "TOKEN_FEEDBACK") {
        ++feedbackCount;
        BOOST_CHECK_EQUAL(edge.requestId, "request");
        BOOST_CHECK_EQUAL(edge.attemptEpoch, 1);
        BOOST_CHECK_EQUAL(edge.planDigest, hash('0'));
        BOOST_CHECK_EQUAL(edge.plannedDataName, "/test/feedback/1");
      }
      else if (edge.operationKind == "PIPELINE") {
        ++pipelineCount;
        BOOST_CHECK_EQUAL(edge.planDigest, failure == 8 ? hash('e') : "");
      }
    }
    BOOST_CHECK_EQUAL(feedbackCount, 1);
    BOOST_CHECK_GE(pipelineCount, 1);
  }
  BOOST_CHECK(!aResult.finalPayload);
  BOOST_REQUIRE(bResult.finalPayload);
  BOOST_CHECK(!bResult.stoppedByUpstream);
  if (failure == 7) {
    BOOST_CHECK(aResult.stoppedByUpstream);
    BOOST_CHECK_EQUAL(calls[0], 1); BOOST_CHECK_EQUAL(calls[1], 1);
    BOOST_CHECK(!restored[0]); BOOST_CHECK(!restored[1]);
    BOOST_CHECK_EQUAL(first.conversationStateSnapshot().entries, 0);
    BOOST_CHECK_EQUAL(last.conversationStateSnapshot().entries, 0);
    continue;
  }
  BOOST_CHECK(!aResult.stoppedByUpstream);
  BOOST_CHECK_EQUAL(calls[0], 2); BOOST_CHECK_EQUAL(calls[1], 2);
  BOOST_CHECK(restored[0]); BOOST_CHECK(restored[1]);
  BOOST_REQUIRE(aResult.finalizedRole); BOOST_REQUIRE(bResult.finalizedRole);
  BOOST_REQUIRE(aResult.finalizedRole->candidateDecodeStateIdentity);
  BOOST_REQUIRE(bResult.finalizedRole->candidateDecodeStateIdentity);
  const auto& left = *aResult.finalizedRole->candidateDecodeStateIdentity;
  const auto& right = *bResult.finalizedRole->candidateDecodeStateIdentity;
  BOOST_CHECK_EQUAL(left.prefixTokenCount, 2);
  BOOST_CHECK_EQUAL(left.prefixDigest, right.prefixDigest);
  BOOST_CHECK_EQUAL(left.prefixTokenCount, right.prefixTokenCount);
  BOOST_REQUIRE(bResult.finalPayload);
  const auto promoteAndRead = [&](NativeProviderRuntime& runtime, const RoleSpec& role) {
    ConversationStateBinding binding;
    binding.conversationId = "conversation-finalize"; binding.contextEpoch = 1;
    binding.serviceName = "/LLM/Pipeline/Generate"; binding.planRoleMapDigest = hash('0');
    binding.receiptDigest = hash('9'); binding.expiresAtMs = 10000;
    binding.identity = *role.candidateDecodeStateIdentity;
    binding.validate();
    BOOST_REQUIRE(runtime.stageDecodeStatePromotion("session", role, binding, 100));
    BOOST_CHECK(!runtime.lookupConversationState(binding, 100));
    BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(binding));
    const auto state = runtime.lookupConversationState(binding, 101);
    BOOST_REQUIRE(state);
    const auto tensors = decodeTensorBundle(state->payload);
    BOOST_REQUIRE_EQUAL(tensors.size(), 1);
    BOOST_CHECK_EQUAL(tensors.front().name, "kv_in");
    BOOST_CHECK(tensors.front().elementType == TensorElementType::Int64);
    BOOST_CHECK(tensors.front().shape == std::vector<std::int64_t>{1});
    BOOST_CHECK(tensors.front().payload == raw<std::int64_t>({2}));
  };
  promoteAndRead(first, *aResult.finalizedRole);
  promoteAndRead(last, *bResult.finalizedRole);
  }
  }
}
BOOST_AUTO_TEST_CASE(Spec189TwoRoleMultiTokenStopsAndPromotesFinalKv)
{
  // Three visible tokens then EOS / a distinct configured EOT; or exactly
  // three tokens at the budget. All paths finalize the final accepted token.
  for (unsigned mode = 0; mode != 3; ++mode) {
    BOOST_TEST_CONTEXT("multi-token stop mode=" << mode) {
      const std::int64_t stopToken = mode == 1 ? 4 : 0;
      const unsigned tokenCount = mode == 2 ? 3 : 4;
      // Independent input oracle: prompt, accepted ordinary tokens, then the
      // final accepted EOS/EOT (or the budget's last ordinary token). Inspect
      // actual runner inputs; model-call counts alone cannot prove feedback.
      std::vector<std::int64_t> expectedInputs{7, 1, 2, 3};
      if (mode != 2) expectedInputs.push_back(stopToken);
      std::vector<std::int64_t> observedInputs;
      std::array<unsigned, 2> calls{{0, 0}}, restores{{0, 0}};
      std::array<unsigned, 2> preparations{{0, 0}};
      std::vector<NativeJson> events;
      auto io = std::make_shared<EpochIo>();
      NativeProviderRuntime first(1), last(1);
      NativeExecutionPlan plan;
      plan.roles = {"/Stage/0", "/Stage/1"};
      NativeDependencySpec activation;
      activation.producers = {plan.roles[0]}; activation.consumers = {plan.roles[1]};
      activation.keyScope = "activation"; activation.topicPrefix = "/test";
      activation.objectNameTemplate = "/test/activation/{sequence}";
      activation.operationKind = "PIPELINE"; activation.tensors = {"hidden"};
      NativeDependencySpec feedback;
      feedback.producers = {plan.roles[1]}; feedback.consumers = {plan.roles[0]};
      feedback.keyScope = "feedback"; feedback.topicPrefix = "/test";
      feedback.objectNameTemplate = "/test/feedback/{sequence}";
      feedback.operationKind = "TOKEN_FEEDBACK";
      plan.dependencies = {activation, feedback};
      NativeProviderAssignment assignment;
      assignment.providerByRole = {{plan.roles[0], "/provider/0"}, {plan.roles[1], "/provider/1"}};
      const auto decode = [](const std::vector<std::int64_t>& ids) {
        std::string text;
        for (auto id : ids) text += id == 1 ? "你" : id == 2 ? "好" : id == 3 ? "呀" : "";
        return text;
      };
      const auto configure = [&](unsigned stage, NativeProviderRuntime& runtime) {
        const auto identity = identityFor(stage);
        NativeModelRunnerSpec spec;
        spec.role = identity.roleName; spec.kind = "onnx-model";
        spec.backend = "test-multi-token"; spec.path = "/fixture/no-file";
        spec.metadata = {{"evidence.modelDigest", identity.modelDigest},
          {"evidence.planDigest", hash('0')}, {"evidence.providerName", identity.providerIdentity},
          {"evidence.providerBootId", identity.providerBootId}, {"state.securityEpoch", "1"},
          {"state.generationId", identity.generationId}, {"state.schemaDigest", identity.stateSchemaDigest}};
        auto runner = makeNativeModelRunner([&, stage](const RoleExecutionContext& context) {
          const auto call = calls[stage]++;
          if (call > tokenCount) throw std::runtime_error("extra model execution after terminal");
          if (stage == 0) {
            const auto scope = observedInputs.empty() ? "prompt" : "feedback";
            const auto input = decodeTensorBundle(context.inputsByScope.at(scope).payload);
            const auto& token = findTensor(input, "input_ids");
            if (token.elementType != TensorElementType::Int64 ||
                token.shape != std::vector<std::int64_t>({1, 1}) ||
                token.payload.size() != sizeof(std::int64_t))
              throw std::runtime_error("multi-token stage0 input_ids contract mismatch");
            std::int64_t actualToken = 0;
            std::memcpy(&actualToken, token.payload.data(), sizeof(actualToken));
            if (observedInputs.size() >= expectedInputs.size() ||
                actualToken != expectedInputs[observedInputs.size()])
              throw std::runtime_error("multi-token stage0 input_ids value mismatch");
            observedInputs.push_back(actualToken);
          }
          if (call != 0) {
            const auto state = decodeTensorBundle(context.inputsByScope.at("__ndnsf_provider_decode_state").payload);
            if (findTensor(state, "kv_in").payload != raw<std::int64_t>({call}))
              throw std::runtime_error("multi-token KV predecessor mismatch");
            ++restores[stage];
          }
          if (stage == 1) {
            const auto input = decodeTensorBundle(context.inputsByScope.at("activation").payload);
            if (findTensor(input, "hidden").payload != raw<std::int64_t>({call + 1}))
              throw std::runtime_error("multi-token activation mismatch");
          }
          std::vector<NamedTensor> tensors{{"kv_out", TensorElementType::Int64, {1},
            raw<std::int64_t>({call + 1})}};
          if (stage == 0)
            tensors.push_back({"hidden", TensorElementType::Int64, {1}, raw<std::int64_t>({call + 1})});
          else {
            std::array<std::uint16_t, 5> half{};
            half[call < 3 ? call + 1 : static_cast<unsigned>(stopToken)] = 0x3c00;
            std::vector<std::uint8_t> payload(sizeof(half));
            std::memcpy(payload.data(), half.data(), payload.size());
            tensors.push_back({"logits", TensorElementType::Float16, {1, 5}, std::move(payload)});
          }
          return std::map<std::string, TensorBundle>{{"activation", makeEncodedTensorBundle("activation", tensors)}};
        });
        runtime.registerRunner(spec, runner);
        NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
        config.sessionId = "session"; config.requestId = "request"; config.role = spec.role;
        config.localProvider = identity.providerIdentity; config.lineagePlanDigest = hash('0');
        config.maxEpochs = mode == 2 ? 3 : 1024; config.checkpointFinalize = true;
        config.stateIdentityTemplate = identity; config.positionPolicyDigest = identity.positionDigest;
        config.samplingDigest = hash('1'); config.stateInputNames = {"kv_in"}; config.stateOutputNames = {"kv_out"};
        config.eosTokenIds = {stopToken};
        config.prepareRunner = [&, stage, runner] { ++preparations[stage]; return runner; };
        config.textDecoder = decode;
        config.stableTextDecoder = [decode](const auto& ids, bool) { return decode(ids); };
        config.requireTextOutput = true;
        if (stage == 0)
          config.initialInputs = {{"prompt", makeEncodedTensorBundle("prompt", {
            {"input_ids", TensorElementType::Int64, {1, 1}, raw<std::int64_t>({7})}})}};
        else config.eventSink = [&](const std::vector<std::uint8_t>& event) {
          events.push_back(NativeJson::parse(event.begin(), event.end())); return true;
        };
        return config;
      };
      auto a = configure(0, first), b = configure(1, last);
      auto firstRun = std::async(std::launch::async, [a = std::move(a)]() mutable { return runNativeEpochCoordinator(std::move(a)); });
      auto lastRun = std::async(std::launch::async, [b = std::move(b)]() mutable { return runNativeEpochCoordinator(std::move(b)); });
      std::optional<NativeEpochCoordinatorResult> left, right;
      std::array<std::string, 2> errors;
      try { left = firstRun.get(); } catch (const std::exception& e) { errors[0] = e.what(); }
      try { right = lastRun.get(); } catch (const std::exception& e) { errors[1] = e.what(); }
      BOOST_REQUIRE_MESSAGE(left, errors[0]); BOOST_REQUIRE_MESSAGE(right, errors[1]);
      BOOST_CHECK_EQUAL_COLLECTIONS(observedInputs.begin(), observedInputs.end(),
                                    expectedInputs.begin(), expectedInputs.end());
      BOOST_CHECK(!left->stoppedByUpstream); BOOST_CHECK(!right->stoppedByUpstream);
      BOOST_CHECK(!left->finalPayload); BOOST_REQUIRE(right->finalPayload);
      BOOST_REQUIRE_EQUAL(events.size(), tokenCount);
      const auto final = NativeJson::parse(right->finalPayload->begin(), right->finalPayload->end());
      const auto ids = final.at("tokenIds").get<std::vector<std::int64_t>>();
      BOOST_REQUIRE_EQUAL(ids.size(), tokenCount);
      const std::string hint = mode == 2 ? "MAX_TOKENS" : "EOS";
      BOOST_CHECK_EQUAL(final.at("finishReason").get<std::string>(), mode == 2 ? "max_tokens" : "eos");
      BOOST_CHECK_EQUAL(final.at("finishHint").get<std::string>(), hint);
      std::string streamedText;
      for (std::size_t i = 0; i != events.size(); ++i) {
        BOOST_CHECK_EQUAL(events[i].at("tokenId").get<std::int64_t>(), ids[i]);
        BOOST_CHECK_EQUAL(ids[i], i < 3 ? static_cast<std::int64_t>(i + 1) : stopToken);
        BOOST_CHECK_EQUAL(events[i].at("tokenEpoch").get<std::size_t>(), i + 1);
        BOOST_CHECK_EQUAL(events[i].at("cumulativeTokenCount").get<std::size_t>(), i + 1);
        BOOST_CHECK_EQUAL(events[i].at("finishHint").get<std::string>(), i + 1 == tokenCount ? hint : "NONE");
        streamedText += events[i].at("textDelta").get<std::string>();
      }
      BOOST_CHECK_EQUAL(streamedText, "你好呀");
      BOOST_CHECK_EQUAL(final.at("text").get<std::string>(), streamedText);
      for (unsigned stage = 0; stage != 2; ++stage) {
        BOOST_CHECK_EQUAL(preparations[stage], 1U);
        BOOST_CHECK_EQUAL(calls[stage], tokenCount + 1);
        BOOST_CHECK_EQUAL(restores[stage], tokenCount);
        auto& runtime = stage == 0 ? first : last;
        const auto& result = stage == 0 ? *left : *right;
        BOOST_REQUIRE(result.finalizedRole); BOOST_REQUIRE(result.finalizedRole->candidateDecodeStateIdentity);
        const auto& role = *result.finalizedRole;
        BOOST_CHECK_EQUAL(role.candidateDecodeStateIdentity->prefixTokenCount, tokenCount + 1);
        BOOST_CHECK_EQUAL(role.candidateDecodeStateIdentity->prefixDigest,
                          left->finalizedRole->candidateDecodeStateIdentity->prefixDigest);
        ConversationStateBinding binding;
        binding.conversationId = "multi-token"; binding.contextEpoch = 1;
        binding.serviceName = "/LLM/Pipeline/Generate"; binding.planRoleMapDigest = hash('0');
        binding.receiptDigest = hash('9'); binding.expiresAtMs = 10000;
        binding.identity = *role.candidateDecodeStateIdentity;
        BOOST_REQUIRE(runtime.stageDecodeStatePromotion("session", role, binding, 100));
        BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(binding));
        const auto state = runtime.lookupConversationState(binding, 101); BOOST_REQUIRE(state);
        const auto tensors = decodeTensorBundle(state->payload);
        BOOST_CHECK(findTensor(tensors, "kv_in").payload == raw<std::int64_t>({tokenCount + 1}));
      }
    }
  }
}
BOOST_AUTO_TEST_CASE(Spec189ThreeTurnProviderKvRestore)
{
  // Observations and dependency owner outlive runtimes; every async call is
  // joined before advancing a turn or releasing any captured fixture state.
  std::array<unsigned, 2> calls{{0, 0}}, restored{{0, 0}};
  auto io = std::make_shared<EpochIo>();
  NativeProviderRuntime first(1), last(1);
  for (unsigned stage = 0; stage != 2; ++stage) {
    auto& runtime = stage == 0 ? first : last;
    auto identity = identityFor(stage);
    NativeModelRunnerSpec spec;
    spec.role = identity.roleName;
    spec.metadata = {{"evidence.modelDigest", identity.modelDigest},
      {"evidence.planDigest", hash('0')}, {"evidence.providerName", identity.providerIdentity},
      {"evidence.providerBootId", identity.providerBootId}, {"state.securityEpoch", "1"},
      {"state.generationId", identity.generationId}, {"state.schemaDigest", identity.stateSchemaDigest}};
    runtime.registerRunner(spec, makeNativeModelRunner([&, stage](const RoleExecutionContext& ctx) {
      const auto turn = calls[stage];
      if (ctx.generationInputTokenCount != 1)
        throw std::runtime_error("continuation recomputed history instead of one-token delta");
      const auto parent = ctx.inputsByScope.find("__ndnsf_provider_decode_state");
      if (turn == 0) {
        if (parent != ctx.inputsByScope.end()) throw std::runtime_error("first turn has unexpected KV");
      }
      else {
        if (parent == ctx.inputsByScope.end()) throw std::runtime_error("continuation KV missing");
        const auto tensors = decodeTensorBundle(parent->second.payload);
        if (findTensor(tensors, "kv_in").payload != raw<std::int64_t>({turn}))
          throw std::runtime_error("continuation restored stale KV bytes");
        ++restored[stage];
      }
      ++calls[stage];
      return std::map<std::string, TensorBundle>{{"state", makeEncodedTensorBundle("state", {
        {"kv_out", TensorElementType::Int64, {1}, raw<std::int64_t>({turn + 1})}})}};
    }));
    std::optional<ConversationStateBinding> parent;
    for (unsigned turn = 0; turn != 3; ++turn) {
      RoleSpec role;
      role.role = spec.role;
      role.requestId = "request-turn-" + std::to_string(turn);
      role.attemptEpoch = identity.attemptEpoch;
      identity.requestId = role.requestId;
      identity.generationId = "generation-turn-" + std::to_string(turn);
      identity.prefixTokenCount = turn + 1;
      identity.prefixDigest = hash('1' + turn);
      identity.positionDigest = hash('4' + turn);
      role.candidateDecodeStateIdentity = identity;
      role.stateInputNames = {"kv_in"}; role.stateOutputNames = {"kv_out"};
      role.conversationStateBinding = parent;
      role.conversationStateLookupNowMs = 100 + turn;
      if (parent) {
        auto wrong = role;
        wrong.conversationStateBinding->checkpointDigest = hash('f');
        BOOST_CHECK_EXCEPTION(runtime.executeRoleAsync(role.requestId, wrong, io),
          std::runtime_error, [](const std::runtime_error& e) {
            return std::string(e.what()) == "PROVIDER_CONVERSATION_STATE_MISSING";
          });
        BOOST_CHECK_EQUAL(calls[stage], turn);
      }
      runtime.executeRoleAsync(role.requestId, role, io,
        {{"input", makeEncodedTensorBundle("input", {
          {"input_ids", TensorElementType::Int64, {1, 1}, raw<std::int64_t>({7})}})}}).get();
      ConversationStateBinding next;
      next.conversationId = "three-turn"; next.contextEpoch = turn + 1;
      next.serviceName = "/LLM/Pipeline/Generate"; next.planRoleMapDigest = hash('0');
      next.receiptDigest = hash('7' + turn); next.expiresAtMs = 10000; next.identity = identity;
      BOOST_REQUIRE(runtime.stageDecodeStatePromotion(role.requestId, role, next, 100 + turn));
      const auto checkpoint = hash('a' + turn);
      BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(next, checkpoint));
      next.checkpointDigest = checkpoint;
      const auto committed = runtime.lookupConversationState(next, 104);
      BOOST_REQUIRE(committed);
      const auto tensors = decodeTensorBundle(committed->payload);
      BOOST_CHECK(findTensor(tensors, "kv_in").payload == raw<std::int64_t>({turn + 1}));
      parent = next;
    }
    BOOST_CHECK_EQUAL(calls[stage], 3U);
    BOOST_CHECK_EQUAL(restored[stage], 2U);
  }
}
} // namespace ndnsf::di::test
