#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>

namespace ndnsf::di::test {
namespace {

template<typename T>
std::vector<std::uint8_t> bytes(std::initializer_list<T> values)
{
  std::vector<std::uint8_t> result(values.size() * sizeof(T));
  if (!result.empty()) std::memcpy(result.data(), values.begin(), result.size());
  return result;
}

struct Observations
{
  std::string error;
  std::string final;
  std::size_t events = 0;
  std::vector<std::int64_t> feedback;
};

class SamplingIo final : public DependencyIo
{
public:
  explicit SamplingIo(Observations& observations) : m_observations(observations) {}
  std::future<TensorBundle> prefetchInput(const std::string&, const DependencyEdge&) override
  {
    throw std::runtime_error("unexpected prefill fetch");
  }
  void publishOutput(const std::string&, const DependencyEdge& edge,
                     const TensorBundle& bundle) override
  {
    if (edge.operationKind != "TOKEN_FEEDBACK")
      throw std::runtime_error("unexpected output edge");
    const auto tensors = decodeTensorBundle(bundle.payload);
    const auto& token = findTensor(tensors, "input_ids");
    if (token.elementType != TensorElementType::Int64 || token.payload.size() != 8)
      throw std::runtime_error("invalid feedback token");
    std::int64_t value;
    std::memcpy(&value, token.payload.data(), sizeof(value));
    m_observations.feedback.push_back(value);
  }
private:
  Observations& m_observations;
};

DecodeStateIdentityV1 samplingIdentity()
{
  const auto digest = [](char ch) { return "sha256:" + std::string(64, ch); };
  DecodeStateIdentityV1 identity;
  identity.modelDigest = digest('1'); identity.graphSemanticDigest = digest('2');
  identity.artifactDigest = digest('3'); identity.adapterDigest = digest('4');
  identity.tokenizerDigest = digest('5'); identity.runnerDigest = digest('6');
  identity.roleName = "/Stage/0"; identity.roleSplitDigest = digest('7');
  identity.layerBegin = 0; identity.layerEnd = 1;
  identity.prefixDigest = digest('8'); identity.prefixTokenCount = 1;
  identity.positionDigest = digest('9'); identity.precision = "fp16";
  identity.layoutDigest = digest('a'); identity.stateSchemaDigest = digest('b');
  identity.stateComponentDigests = {digest('c')}; identity.runtimeAbiDigest = digest('d');
  identity.securityDomainDigest = digest('e'); identity.providerIdentity = "/provider/test";
  identity.providerBootId = "boot"; identity.cacheEpoch = 1;
  identity.requestId = "request"; identity.attemptEpoch = 1; identity.generationId = "generation";
  identity.validate();
  return identity;
}

Observations sample(NamedTensor logits, bool seeded = false, std::uint64_t seed = 0)
{
  // Observations outlive IO and the worker runtime, including exception drain.
  Observations observed;
  NativeProviderRuntime runtime(1);
  const auto digest = [](char ch) { return "sha256:" + std::string(64, ch); };
  const auto identity = samplingIdentity();
  NativeModelRunnerSpec spec;
  spec.role = identity.roleName; spec.kind = "onnx-model";
  spec.backend = "test-sampling"; spec.path = "/fixture/no-model-file";
  spec.metadata = {{"evidence.modelDigest", identity.modelDigest},
    {"evidence.planDigest", digest('0')}, {"evidence.providerName", identity.providerIdentity},
    {"evidence.providerBootId", identity.providerBootId}, {"state.securityEpoch", "1"},
    {"state.generationId", identity.generationId}, {"state.schemaDigest", identity.stateSchemaDigest}};
  auto runner = makeNativeModelRunner([logits](const RoleExecutionContext&) {
    return std::map<std::string, TensorBundle>{{"onnx-output-bundle",
      makeEncodedTensorBundle("onnx-output-bundle", {logits})}};
  });
  runtime.registerRunner(spec, runner);
  NativeExecutionPlan plan; plan.roles = {spec.role};
  NativeDependencySpec feedback;
  feedback.producers = feedback.consumers = {spec.role};
  feedback.keyScope = "token-feedback"; feedback.topicPrefix = "/ndnsf-di";
  feedback.objectNameTemplate = "{producerProvider}/FEEDBACK/{sessionId}/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK"; plan.dependencies = {feedback};
  NativeProviderAssignment assignment;
  assignment.providerByRole[spec.role] = identity.providerIdentity;
  auto io = std::make_shared<SamplingIo>(observed);
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "session"; config.requestId = "request";
  config.localProvider = identity.providerIdentity; config.role = spec.role;
  config.lineagePlanDigest = digest('0'); config.maxEpochs = 1;
  config.stateIdentityTemplate = identity; config.positionPolicyDigest = identity.positionDigest;
  config.samplingDigest = digest('1'); config.prepareRunner = [runner] { return runner; };
  config.initialInputs = {{"input_ids", makeEncodedTensorBundle("prompt", {
    NamedTensor{"input_ids", TensorElementType::Int64, {1, 1}, bytes<std::int64_t>({1})}})}};
  config.eventSink = [&observed](const std::vector<std::uint8_t>&) {
    ++observed.events; return true;
  };
  if (seeded) {
    config.samplingMode = "SeededTopKTopP"; config.samplingSeed = seed;
    config.samplingTemperature = 1.0; config.samplingTopK = 4; config.samplingTopP = 1.0;
  }
  try {
    const auto result = runNativeEpochCoordinator(std::move(config));
    if (result.finalPayload)
      observed.final.assign(result.finalPayload->begin(), result.finalPayload->end());
  }
  catch (const std::exception& error) { observed.error = error.what(); }
  return observed;
}

void requireToken(const Observations& result, std::int64_t token)
{
  BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
  BOOST_CHECK_EQUAL(result.events, 1);
  BOOST_REQUIRE_EQUAL(result.feedback.size(), 1);
  BOOST_CHECK_EQUAL(result.feedback.front(), token);
  BOOST_CHECK(result.final.find("\"tokenIds\":[" + std::to_string(token) + "]") != std::string::npos);
}

// Self-feedback is published before the next epoch fetch. Never block a worker
// waiting for another producer: a missing exact name fails immediately.
class RunnerReuseIo final : public DependencyIo {
public:
  std::future<TensorBundle> prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    std::promise<TensorBundle> promise;
    auto future = promise.get_future();
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = objects.find(edge.plannedDataName);
    if (found == objects.end())
      promise.set_exception(std::make_exception_ptr(std::runtime_error("missing fixture feedback")));
    else promise.set_value(found->second);
    return future;
  }
  void publishOutput(const std::string&, const DependencyEdge& edge, const TensorBundle& bundle) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!objects.emplace(edge.plannedDataName, bundle).second)
      throw std::runtime_error("duplicate fixture feedback");
  }
  std::mutex mutex;
  std::map<std::string, TensorBundle> objects;
};
} // namespace

BOOST_AUTO_TEST_CASE(Spec189EpochRunnerPreparationLifetimeAndFailureIsolation)
{
  // Success, factory throw/null, cancellation before preparation, cancellation
  // after one accepted token, and failure during the second model execution.
  for (unsigned mode = 0; mode != 6; ++mode) {
    BOOST_TEST_CONTEXT("runner lifecycle mode=" << mode) {
      std::atomic<unsigned> preparations{0}, executions{0}, destructions{0};
      std::vector<std::weak_ptr<NativeModelRunner>> owners;
      std::atomic<bool> cancelled{false};
      unsigned events = 0;
      auto io = std::make_shared<RunnerReuseIo>();
      // Every captured observation and IO owner precedes runtime. Its worker
      // destructor joins before we inspect weak owners; no startup registry
      // retains a runner and no callback captures a temporary config by ref.
      {
        NativeProviderRuntime runtime(1);
        for (unsigned invocation = 0; invocation != 2; ++invocation) {
          const unsigned behavior = invocation == 0 ? mode : 0;
          const auto beforePrepare = preparations.load();
          const auto beforeRun = executions.load();
          const auto beforeEvents = events;
          cancelled = behavior == 3;
          auto identity = samplingIdentity();
          identity.requestId = "request-" + std::to_string(invocation);
          identity.generationId = "generation-" + std::to_string(invocation);
          NativeExecutionPlan plan; plan.roles = {identity.roleName};
          NativeDependencySpec feedback;
          feedback.producers = feedback.consumers = plan.roles;
          feedback.keyScope = "feedback"; feedback.topicPrefix = "/reuse";
          feedback.objectNameTemplate = "{producerProvider}/FEEDBACK/{sessionId}/{sequence}";
          feedback.operationKind = "TOKEN_FEEDBACK"; plan.dependencies = {feedback};
          NativeProviderAssignment assignment;
          assignment.providerByRole[identity.roleName] = identity.providerIdentity;
          NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
          config.sessionId = identity.requestId; config.requestId = identity.requestId;
          config.role = identity.roleName; config.localProvider = identity.providerIdentity;
          config.lineagePlanDigest = "sha256:" + std::string(64, '0');
          config.stateIdentityTemplate = identity; config.positionPolicyDigest = identity.positionDigest;
          config.samplingDigest = identity.modelDigest; config.maxEpochs = 3;
          config.stateInputNames = {"kv_in"}; config.stateOutputNames = {"kv_out"};
          config.initialInputs = {{"prompt", makeEncodedTensorBundle("prompt", {
            {"input_ids", TensorElementType::Int64, {1, 1}, bytes<std::int64_t>({1})}})}};
          config.stopCheck = [&]() -> std::optional<NativeEpochStopReason> {
            return cancelled.load() ? std::optional<NativeEpochStopReason>(NativeEpochStopReason::Cancelled)
                                    : std::nullopt;
          };
          config.prepareRunner = [&preparations, &executions, &destructions, &owners,
                                  behavior, beforeRun]() -> std::shared_ptr<NativeModelRunner> {
            ++preparations;
            if (behavior == 1) throw std::runtime_error("fixture factory failure");
            if (behavior == 2) return {};
            // The deleter observes destruction, not just the end of run().
            auto lifetime = std::shared_ptr<int>(new int(0), [&destructions](int* value) {
              ++destructions; delete value;
            });
            auto runner = makeNativeModelRunner([&executions, behavior, beforeRun, lifetime](const RoleExecutionContext&) {
              const auto call = executions.fetch_add(1) - beforeRun;
              if (behavior == 5 && call == 1)
                throw std::runtime_error("fixture later execution failure");
              return std::map<std::string, TensorBundle>{{"output", makeEncodedTensorBundle("output", {
                {"kv_out", TensorElementType::Int64, {1}, bytes<std::int64_t>({static_cast<std::int64_t>(call + 1)})},
                {"logits", TensorElementType::Float32, {1, 2}, bytes<float>({0, 1})}})}};
            });
            owners.push_back(runner);
            return runner;
          };
          config.eventSink = [&](const std::vector<std::uint8_t>&) { ++events; return true; };
          config.resultObserver = [&, behavior](const RoleSpec& role, const ProviderRoleResult&) {
            if (behavior == 4 && role.inferenceEpoch == 1) cancelled = true;
          };
          std::string error;
          std::optional<NativeEpochCoordinatorResult> result;
          try { result = runNativeEpochCoordinator(std::move(config)); }
          catch (const std::exception& e) { error = e.what(); }
          BOOST_CHECK_EQUAL(preparations.load() - beforePrepare, behavior == 3 ? 0U : 1U);
          BOOST_CHECK_EQUAL(executions.load() - beforeRun,
                            behavior == 0 ? 3U : behavior >= 4 ? 2U : 0U);
          BOOST_CHECK_EQUAL(events - beforeEvents, behavior == 0 ? 3U : behavior >= 4 ? 1U : 0U);
          if (behavior == 0) {
            BOOST_REQUIRE_MESSAGE(result, error);
            BOOST_REQUIRE(result->finalPayload);
            BOOST_CHECK_EQUAL(result->epochsExecuted, 3U);
          }
          else {
            BOOST_CHECK(!result);
            const std::string expected = behavior == 1 ? "fixture factory failure" :
              behavior == 2 ? "preparation returned no NativeModelRunner" :
              behavior == 5 ? "fixture later execution failure" : "ATTEMPT_CANCELLED";
            BOOST_CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
          }
          BOOST_CHECK_EQUAL(runtime.decodeStateSnapshot().entries, 0U);
          BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
        }
      }
      for (const auto& owner : owners) BOOST_CHECK(owner.expired());
      BOOST_CHECK_EQUAL(destructions.load(), owners.size());
      BOOST_CHECK_EQUAL(owners.size(), mode == 0 || mode >= 4 ? 2U : 1U);
    }
  }
}

BOOST_AUTO_TEST_CASE(Spec189WorkerOutputCacheDoesNotSuppressEpochExecution)
{
  // Isolate both exclusion flags. The lineage-only role deliberately has no
  // state fields, while the stateful role deliberately has no lineage.
  for (unsigned mode = 0; mode != 3; ++mode) {
    BOOST_TEST_CONTEXT("exact-forward mode=" << mode) {
      std::atomic<unsigned> calls{0};
      auto io = std::make_shared<RunnerReuseIo>();
      auto runner = makeNativeModelRunner([&](const RoleExecutionContext&) {
        const auto count = ++calls;
        return std::map<std::string, TensorBundle>{{"output", makeEncodedTensorBundle("output", {
          {"value", TensorElementType::Int64, {1}, bytes<std::int64_t>({count})}})}};
      });
      ProviderRoleWorker worker(1);
      RoleSpec role;
      role.role = "/Stage/0"; role.requestId = "cache-request"; role.attemptEpoch = 1;
      role.streamingStateExecution = mode == 1;
      if (mode == 1) {
        role.stateInputNames = {"kv_in"};
        role.stateOutputNames = {"value"};
      }
      const std::map<std::string, TensorBundle> inputs{{"prompt", makeEncodedTensorBundle("prompt", {
        {"input_ids", TensorElementType::Int64, {1, 1}, bytes<std::int64_t>({1})}})}};
      const auto execute = [&](unsigned epoch) {
        role.inferenceEpoch = epoch;
        if (mode == 2) {
          const auto identity = samplingIdentity();
          GenerationEpochLineageV1 lineage;
          lineage.requestId = role.requestId; lineage.attemptEpoch = role.attemptEpoch;
          lineage.planDigest = identity.modelDigest; lineage.generationId = "cache-generation";
          lineage.streamEpoch = 1; lineage.inferenceEpoch = epoch;
          lineage.transitionKind = epoch == 0 ? GenerationEpochLineageV1::PREFILL
                                              : GenerationEpochLineageV1::DECODE;
          lineage.logicalPrefixDigest = identity.prefixDigest;
          lineage.logicalPrefixTokenCount = epoch + 1;
          lineage.positionDigest = identity.positionDigest;
          lineage.validateCore();
          role.generationLineage = lineage;
        }
        auto future = worker.executeAsync("cache-session", role, io, runner, inputs);
        BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        return future.get();
      };
      const auto first = execute(0), second = execute(1);
      BOOST_CHECK(!first.exactForwardCacheHit);
      BOOST_CHECK_EQUAL(second.exactForwardCacheHit, mode == 0);
      BOOST_CHECK_EQUAL(calls.load(), mode == 0 ? 1U : 2U);
      BOOST_CHECK_EQUAL(second.providerDecodeState.has_value(), mode == 1);
      // Demonstrate that the exclusion, not unequal tensor bytes/cache keys,
      // forced the second compute. The returned value also detects replay.
      BOOST_CHECK_EQUAL(first.exactForwardCacheKey, second.exactForwardCacheKey);
      const auto actual = decodeTensorBundle(second.outputsByScope.at("output").payload);
      BOOST_CHECK(findTensor(actual, "value").payload ==
                  bytes<std::int64_t>({mode == 0 ? 1 : 2}));
    }
  }
}

BOOST_AUTO_TEST_CASE(Spec189LogitsHalfAndFloatUseLastTimestep)
{
  const NamedTensor half{"logits", TensorElementType::Float16, {1, 2, 4},
    bytes<std::uint16_t>({0x4900, 0, 0, 0, 0xbc00, 0, 0x3c00, 0x4000})};
  const NamedTensor full{"logits", TensorElementType::Float32, {1, 2, 4},
    bytes<float>({10, 0, 0, 0, -1, 0, 1, 2})};
  requireToken(sample(half), 3); requireToken(sample(full), 3);
  for (std::uint64_t seed = 0; seed < 8; ++seed) {
    const auto a = sample(half, true, seed); const auto b = sample(full, true, seed);
    BOOST_REQUIRE_MESSAGE(a.error.empty(), a.error);
    BOOST_REQUIRE_MESSAGE(b.error.empty(), b.error);
    BOOST_CHECK_EQUAL(a.final, b.final);
    BOOST_CHECK_EQUAL_COLLECTIONS(a.feedback.begin(), a.feedback.end(), b.feedback.begin(), b.feedback.end());
  }
}

BOOST_AUTO_TEST_CASE(Spec189LogitsHalfBoundaryValues)
{
  requireToken(sample({"logits", TensorElementType::Float16, {1, 4},
    bytes<std::uint16_t>({0x8001, 0x8000, 0, 0x0001})}), 3);
  requireToken(sample({"logits", TensorElementType::Float16, {1, 4},
    bytes<std::uint16_t>({0x7bff, 0x0400, 0x0001, 0xfbff})}), 0);
  // Preserve existing final-row-only finite validation.
  requireToken(sample({"logits", TensorElementType::Float16, {1, 2, 2},
    bytes<std::uint16_t>({0x7e00, 0x7c00, 0, 0x3c00})}), 1);
}

BOOST_AUTO_TEST_CASE(Spec189LogitsRejectNonFiniteAndMalformedTensors)
{
  std::vector<NamedTensor> invalid;
  for (std::uint16_t bits : {0x7c00, 0xfc00, 0x7e00})
    invalid.push_back({"logits", TensorElementType::Float16, {1, 2}, bytes<std::uint16_t>({0, bits})});
  for (float value : {std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    invalid.push_back({"logits", TensorElementType::Float32, {1, 2}, bytes<float>({0, value})});
  invalid.push_back({"logits", TensorElementType::Int64, {1, 1}, bytes<std::int64_t>({1})});
  for (auto type : {TensorElementType::Float16, TensorElementType::Float32}) {
    const auto width = tensorElementByteSize(type);
    invalid.push_back({"logits", type, {}, std::vector<std::uint8_t>(width)});
    invalid.push_back({"logits", type, {1, 0, 2}, {}});
    invalid.push_back({"logits", type, {1, 2, 0}, {}});
    invalid.push_back({"logits", type, {1, 2}, std::vector<std::uint8_t>(2 * width - 1)});
    invalid.push_back({"logits", type, {1, 2}, std::vector<std::uint8_t>(width)});
  }
  for (const auto& tensor : invalid) {
    const auto result = sample(tensor);
    BOOST_CHECK(!result.error.empty());
    BOOST_CHECK_EQUAL(result.events, 0);
    BOOST_CHECK(result.feedback.empty());
    BOOST_CHECK(result.final.empty());
  }
}
} // namespace ndnsf::di::test
