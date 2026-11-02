#include "tests/boost-test.hpp"
#include "tests/fixtures/spec220/transfer-receipt.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>

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

// Prefetch may precede feedback publication. Return promptly and bound the
// owned future's wait; synchronous readiness would prevent the publication.
class RunnerReuseIo final : public DependencyIo {
public:
  std::future<TensorBundle> prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    return std::async(std::launch::async, [this, edge] {
      std::unique_lock<std::mutex> lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return objects.count(edge.plannedDataName);
          })) {
        throw std::runtime_error("missing fixture feedback");
      }
      return observedBundle(edge, objects.at(edge.plannedDataName), "receive");
    });
  }
  void publishOutput(const std::string&, const DependencyEdge& edge, const TensorBundle& bundle) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto stored = observedBundle(edge, bundle, "send");
    bundle.transferObservation = stored.transferObservation;
    if (!objects.emplace(edge.plannedDataName, std::move(stored)).second)
      throw std::runtime_error("duplicate fixture feedback");
    changed.notify_all();
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, TensorBundle> objects;

private:
  static TensorBundle
  observedBundle(const DependencyEdge& edge, const TensorBundle& bundle,
                 const std::string& direction)
  {
    // Keep receive receipts independent of the sender's mutable receipt.
    auto observed = bundle;
    observed.transferObservation = fixture::makeTransportReceipt(edge, observed, direction);
    observed.transferObservation->wireBytes = 999;
    observed.transferObservation->interestCount = 2;
    observed.transferObservation->retryCount = 0;
    return observed;
  }
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
          auto work = std::make_shared<NativeWorkStatistics>();
          config.workStatistics = work;
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
          const auto workResult = work->snapshot();
          BOOST_CHECK(workResult.complete);
          BOOST_CHECK_EQUAL(workResult.activeSpans, 0U);
          if (behavior == 0) {
            BOOST_REQUIRE_MESSAGE(result, error);
            BOOST_CHECK_EQUAL(workResult.phases.at("epoch-sample").calls, 3U);
            BOOST_CHECK_EQUAL(workResult.phases.at("epoch-event-admission").calls, 3U);
            BOOST_CHECK_EQUAL(workResult.phases.at("role-worker").calls, 3U);
            BOOST_CHECK_EQUAL(workResult.phases.at("role-input-bind").calls, 3U);
            std::map<std::string, std::uint64_t> publishedEpochs;
            std::size_t fetches = 0, admissions = 0;
            for (const auto& transfer : workResult.transfers) {
              if (transfer.point == NativeTransferPoint::OUTPUT_PUBLICATION) {
                BOOST_CHECK_EQUAL(transfer.direction, "send");
                BOOST_REQUIRE(publishedEpochs.emplace(transfer.plannedDataName,
                                                      transfer.inferenceEpoch).second);
                BOOST_CHECK_GT(transfer.startedSteadyNs, 0);
                BOOST_CHECK_GE(transfer.finishedSteadyNs, transfer.startedSteadyNs);
                BOOST_CHECK_GT(transfer.outputReadySteadyNs, 0);
                BOOST_CHECK_LE(transfer.outputReadySteadyNs, transfer.startedSteadyNs);
                BOOST_REQUIRE(transfer.wireBytes);
                BOOST_CHECK_EQUAL(*transfer.wireBytes, 999U);
              }
            }
            BOOST_REQUIRE_EQUAL(publishedEpochs.size(), 3U);
            std::set<std::uint64_t> observedFetchEpochs;
            for (const auto& transfer : workResult.transfers) {
              if (transfer.point == NativeTransferPoint::COORDINATOR_FETCH) {
                ++fetches;
                BOOST_REQUIRE(publishedEpochs.count(transfer.plannedDataName));
                BOOST_CHECK_EQUAL(transfer.inferenceEpoch,
                                  publishedEpochs.at(transfer.plannedDataName));
                BOOST_CHECK_GT(transfer.startedSteadyNs, 0);
                BOOST_CHECK_GE(transfer.finishedSteadyNs, transfer.startedSteadyNs);
                BOOST_CHECK_EQUAL(transfer.admittedSteadyNs, 0);
                observedFetchEpochs.insert(transfer.inferenceEpoch);
              }
              else if (transfer.point == NativeTransferPoint::WORKER_INPUT) {
                ++admissions;
                BOOST_REQUIRE(publishedEpochs.count(transfer.plannedDataName));
                BOOST_CHECK_EQUAL(transfer.inferenceEpoch,
                                  publishedEpochs.at(transfer.plannedDataName));
                BOOST_CHECK_EQUAL(transfer.startedSteadyNs, 0);
                BOOST_CHECK_EQUAL(transfer.finishedSteadyNs, 0);
                BOOST_CHECK_GT(transfer.admittedSteadyNs, 0);
              }
            }
            BOOST_CHECK_EQUAL(fetches, 2U);
            BOOST_CHECK_EQUAL(admissions, 2U);
            const std::set<std::uint64_t> expectedFetchEpochs{1, 2};
            BOOST_CHECK(observedFetchEpochs == expectedFetchEpochs);
          }
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

BOOST_AUTO_TEST_CASE(Spec220WorkerFetchTimingPreservesOriginalProvenance)
{
  // Exercise both real fetch branches and the coordinator-supplied branch.
  // Resolving the pending promise after executeAsync forces scheduler admission.
  class InputIo final : public DependencyIo
  {
  public:
    std::future<TensorBundle>
    prefetchInput(const std::string&, const DependencyEdge&) override
    {
      ++fetches;
      return input.get_future();
    }
    void
    publishOutput(const std::string&, const DependencyEdge&, const TensorBundle& bundle) override
    {
      ++publications;
      published = bundle;
      if (onPublish) onPublish();
    }
    std::promise<TensorBundle> input;
    unsigned fetches = 0;
    unsigned publications = 0;
    TensorBundle published;
    std::function<void()> onPublish;
  };
  for (unsigned mode = 0; mode != 3; ++mode) {
    BOOST_TEST_CONTEXT("input provenance mode=" << mode) {
      const auto bundle = makeEncodedTensorBundle("activation", {
        {"value", TensorElementType::Int64, {1}, bytes<std::int64_t>({7})}});
      auto io = std::make_shared<InputIo>();
      if (mode == 0) io->input.set_value(bundle);
      std::atomic<std::int64_t> clock{1000};
      auto work = std::make_shared<NativeWorkStatistics>([&clock] {
        const auto now = clock.load();
        return NativeWorkClock{now, now};
      });
      io->onPublish = [&clock] { clock.fetch_add(500); };
      RoleSpec role;
      role.role = "/Stage/1";
      role.requestId = "timing-request";
      role.attemptEpoch = 1;
      role.workStatistics = work;
      role.inputs.emplace_back("activation", "/Stage/0", role.role,
                               "/provider/0/activation/1");
      role.outputs.emplace_back("output", role.role, "/Stage/2",
                                "/provider/1/activation/2");
      auto runner = makeNativeModelRunner([](const RoleExecutionContext& context) {
        const auto& input = context.inputsByScope.at("activation");
        return std::map<std::string, TensorBundle>{{"output",
          makeEncodedTensorBundle(input.name, decodeTensorBundle(input.payload))}};
      });
      ProviderRoleWorker worker(1);
      std::map<std::string, TensorBundle> supplied;
      if (mode == 2) supplied.emplace("activation", bundle);
      auto result = worker.executeAsync("timing-session", role, io, runner, supplied, {}, [&clock] {
        clock.fetch_add(100);
      });
      if (mode == 1) io->input.set_value(bundle);
      BOOST_REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
      const auto completed = result.get();
      BOOST_CHECK(completed.outputsByScope.at("output").payload == bundle.payload);
      BOOST_CHECK_EQUAL(io->publications, 1U);
      BOOST_CHECK(io->published.payload == bundle.payload);
      BOOST_REQUIRE_EQUAL(completed.inputTimings.size(), 1U);
      BOOST_CHECK_EQUAL(completed.inputTimings.front().preSatisfied, mode == 2);
      BOOST_CHECK_EQUAL(io->fetches, mode == 2 ? 0U : 1U);
      const auto snapshot = work->snapshot();
      BOOST_REQUIRE(snapshot.complete);
      BOOST_CHECK_EQUAL(snapshot.activeSpans, 0U);
      BOOST_REQUIRE(snapshot.phases.count("role-input-authorization") == 1U);
      const auto& authorization = snapshot.phases.at("role-input-authorization");
      BOOST_CHECK(authorization.kind == NativeWorkKind::UNCLASSIFIED);
      BOOST_CHECK_EQUAL(authorization.calls, 1U);
      BOOST_CHECK_EQUAL(authorization.exclusiveWallNs, 100U);
      BOOST_REQUIRE(snapshot.phases.count("role-input-bind") == 1U);
      BOOST_CHECK_EQUAL(snapshot.phases.at("role-input-bind").exclusiveWallNs, 0U);
      BOOST_REQUIRE(snapshot.phases.count("role-output-pack") == 1U);
      BOOST_CHECK(snapshot.phases.at("role-output-pack").kind == NativeWorkKind::LOCAL);
      BOOST_REQUIRE(snapshot.phases.count("role-output-stage") == 1U);
      BOOST_CHECK(snapshot.phases.at("role-output-stage").kind == NativeWorkKind::LOCAL);
      BOOST_CHECK_EQUAL(snapshot.phases.at("role-output-stage").exclusiveWallNs, 0U);
      const auto wire = nativeParseJson(nativeWorkStatisticsToJson(snapshot));
      BOOST_REQUIRE_EQUAL(wire.at("transfers").size(), 2U);
      const auto& timing = wire.at("transfers").at(0);
      BOOST_CHECK_EQUAL(timing.at("startedSteadyNs").is_null(), mode == 2);
      BOOST_CHECK_EQUAL(timing.at("finishedSteadyNs").is_null(), mode == 2);
      BOOST_CHECK(!timing.at("admittedSteadyNs").is_null());
      if (mode != 2) {
        BOOST_CHECK_LE(timing.at("startedSteadyNs").get<std::int64_t>(),
                       timing.at("finishedSteadyNs").get<std::int64_t>());
        BOOST_CHECK_LE(timing.at("finishedSteadyNs").get<std::int64_t>(),
                       timing.at("admittedSteadyNs").get<std::int64_t>());
      }
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

BOOST_AUTO_TEST_CASE(Spec216GreedyLargeVocabularyPreservesFirstMaximum)
{
  // Match the current Qwen vocabulary scale without loading a backend. The
  // coordinator's own sampling phase timer isolates the production sampler
  // from fixture setup, worker scheduling, feedback and teardown.
  constexpr std::size_t VOCABULARY = 151936;
  std::vector<float> values(VOCABULARY, -1.0F);
  values[VOCABULARY - 2] = 42.0F;
  values[VOCABULARY - 1] = 42.0F;
  NamedTensor logits{"logits", TensorElementType::Float32,
    {1, static_cast<std::int64_t>(VOCABULARY)},
    std::vector<std::uint8_t>(values.size() * sizeof(float))};
  std::memcpy(logits.payload.data(), values.data(), logits.payload.size());
  for (unsigned repetition = 0; repetition != 16; ++repetition) {
    BOOST_TEST_CONTEXT("large-vocabulary sampling repetition=" << repetition) {
      requireToken(sample(logits), static_cast<std::int64_t>(VOCABULARY - 2));
    }
  }
}

BOOST_AUTO_TEST_CASE(Spec216GreedyPrefillVocabularyUsesFinalRow)
{
  constexpr std::size_t VOCABULARY = 151936;
  const float maximum = 42.0F;
  for (const std::size_t rows : {1U, 351U, 531U}) {
    NamedTensor logits{"logits", TensorElementType::Float32,
      {1, static_cast<std::int64_t>(rows), static_cast<std::int64_t>(VOCABULARY)},
      std::vector<std::uint8_t>(rows * VOCABULARY * sizeof(float))};
    for (const auto index : {rows * VOCABULARY - 2, rows * VOCABULARY - 1}) {
      std::memcpy(logits.payload.data() + index * sizeof(float), &maximum, sizeof(float));
    }
    for (unsigned repetition = 0; repetition != 4; ++repetition) {
      BOOST_TEST_CONTEXT("prefill rows=" << rows << " repetition=" << repetition) {
        requireToken(sample(logits), static_cast<std::int64_t>(VOCABULARY - 2));
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(Spec216TensorBundleViewsValidateCompleteWire)
{
  // Independent NDITB001 fixture: one float32 tensor named x with shape [1].
  const std::vector<std::uint8_t> wire = {
    'N', 'D', 'I', 'T', 'B', '0', '0', '1',
    1, 0, 0, 0, 1, 0, 0, 0, 'x', 1, 0, 0, 0, 1, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x3f};
  const auto views = decodeTensorBundleViews(wire);
  BOOST_REQUIRE_EQUAL(views.size(), 1);
  const auto& view = views.front();
  BOOST_CHECK_EQUAL(view.name, "x");
  BOOST_CHECK(view.elementType == TensorElementType::Float32);
  BOOST_REQUIRE_EQUAL(view.shape.size(), 1);
  BOOST_CHECK_EQUAL(view.shape.front(), 1);
  BOOST_REQUIRE_EQUAL(view.payload.size(), 4);
  BOOST_CHECK(view.payload.data() == wire.data() + 41);
  BOOST_CHECK_EQUAL_COLLECTIONS(view.payload.begin(), view.payload.end(),
                                wire.begin() + 41, wire.end());
  const auto owning = decodeTensorBundle(std::vector<std::uint8_t>(wire));
  BOOST_REQUIRE_EQUAL(owning.size(), 1);
  BOOST_CHECK(owning.front().payload.data() != view.payload.data());
  BOOST_CHECK_EQUAL_COLLECTIONS(owning.front().payload.begin(), owning.front().payload.end(),
                                view.payload.begin(), view.payload.end());

  const auto rejects = [] (const std::vector<std::uint8_t>& input) {
    BOOST_CHECK_THROW(decodeTensorBundle(input), std::invalid_argument);
    BOOST_CHECK_THROW(decodeTensorBundleViews(input), std::invalid_argument);
  };
  for (std::size_t size = 0; size < wire.size(); ++size) {
    BOOST_TEST_CONTEXT("truncated prefix bytes=" << size) {
      rejects(std::vector<std::uint8_t>(wire.begin(), wire.begin() + size));
    }
  }
  for (const auto field : {8U, 12U, 17U, 21U, 32U, 33U}) {
    auto malformed = wire;
    // Invalid count257, empty name, unsupported dtype, rank17, negative
    // dimension, or a declared uint64 payload far beyond the codec bound.
    if (field == 8) malformed[9] = 1;
    else if (field == 12) malformed[field] = 0;
    else if (field == 17) malformed[field] = 99;
    else if (field == 21) malformed[field] = 17;
    else if (field == 32) malformed[field] = 0x80;
    else std::fill(malformed.begin() + 33, malformed.begin() + 41, 0xff);
    BOOST_TEST_CONTEXT("invalid field offset=" << field) {
      rejects(malformed);
    }
  }
  auto trailing = wire;
  trailing.push_back(0);
  rejects(trailing);
  auto laterMalformed = wire;
  laterMalformed[8] = 2;
  laterMalformed.insert(laterMalformed.end(), wire.begin() + 12, wire.end());
  laterMalformed.pop_back();
  rejects(laterMalformed);
  auto empty = std::vector<std::uint8_t>(wire.begin(), wire.begin() + 12);
  empty[8] = 0;
  BOOST_CHECK(decodeTensorBundle(empty).empty());
  BOOST_CHECK(decodeTensorBundleViews(empty).empty());
  auto zeroLength = wire;
  zeroLength[25] = 0;
  zeroLength[33] = 0;
  zeroLength.resize(41);
  const auto zeroViews = decodeTensorBundleViews(zeroLength);
  BOOST_REQUIRE_EQUAL(zeroViews.size(), 1);
  BOOST_CHECK(zeroViews.front().payload.empty());

  NamedTensor ranked{"ranked", TensorElementType::Float32, std::vector<std::int64_t>(16, 1),
                     float32Payload({1.0F})};
  const auto rankedWire = encodeTensorBundle({ranked});
  const auto rankedViews = decodeTensorBundleViews(rankedWire);
  BOOST_REQUIRE_EQUAL(rankedViews.size(), 1);
  BOOST_CHECK_EQUAL(rankedViews.front().shape.size(), 16);
  std::vector<NamedTensor> members;
  for (unsigned index = 0; index != 256; ++index) {
    members.push_back({"empty-" + std::to_string(index), TensorElementType::UInt8, {0}, {}});
  }
  const auto maximumCountWire = encodeTensorBundle(members);
  BOOST_CHECK_EQUAL(decodeTensorBundleViews(maximumCountWire).size(), 256);
}


BOOST_AUTO_TEST_CASE(Spec220BorrowedTensorEncodingPreservesFrozenWireAndOwnership)
{
  std::vector<NamedTensor> tensors{
    {"x", TensorElementType::Float32, {2}, {0, 0, 128, 63, 0, 0, 0, 192}},
    {"h", TensorElementType::Float16, {2}, {0, 60, 0, 192}},
    {"mask", TensorElementType::Bool, {0}, {}}};
  std::vector<NamedTensorView> views;
  for (const auto& tensor : tensors) {
    views.push_back({tensor.name, tensor.elementType, tensor.shape,
      ndn::span<const std::uint8_t>(tensor.payload.data(), tensor.payload.size())});
  }
  // Fixed NDITB001 bytes: little-endian framing, FP32/FP16 values and an empty Bool.
  const std::vector<std::uint8_t> expected{
    0x4e, 0x44, 0x49, 0x54, 0x42, 0x30, 0x30, 0x31, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x78, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00,
    0xc0, 0x01, 0x00, 0x00, 0x00, 0x68, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0xc0, 0x04, 0x00,
    0x00, 0x00, 0x6d, 0x61, 0x73, 0x6b, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  BOOST_CHECK(encodeTensorBundle(tensors) == expected);
  BOOST_CHECK(encodeTensorBundleViews(views) == expected);
  const auto bundle = makeEncodedTensorBundleViews("borrowed", views);
  BOOST_CHECK_EQUAL(bundle.name, "borrowed");
  BOOST_CHECK_EQUAL(bundle.expectedBytes, expected.size());
  BOOST_CHECK(bundle.payload == expected);
  BOOST_CHECK(!bundle.transferObservation);
  const auto selected = selectTensorViews(views, {"h", "x", "h"});
  BOOST_REQUIRE_EQUAL(selected.size(), 2U);
  BOOST_CHECK_EQUAL(selected[0].name, "h");
  BOOST_CHECK_EQUAL(selected[1].name, "x");
  BOOST_CHECK_THROW(selectTensorViews(views, {"absent"}), std::out_of_range);
  BOOST_CHECK_EQUAL(selectTensorViews(views, {}).size(), views.size());
  auto invalid = views.front();
  invalid.shape = {3};
  BOOST_CHECK_THROW(encodeTensorBundleViews({invalid}), std::invalid_argument);
  invalid.shape = {-1};
  BOOST_CHECK_THROW(validateNamedTensorView(invalid), std::invalid_argument);
  invalid = views.front();
  invalid.name.clear();
  BOOST_CHECK_THROW(encodeTensorBundleViews({invalid}), std::invalid_argument);
  invalid = views.front();
  invalid.payload = ndn::span<const std::uint8_t>(tensors.front().payload.data(), 1);
  BOOST_CHECK_THROW(encodeTensorBundleViews({invalid}), std::invalid_argument);
  BOOST_CHECK_THROW(encodeTensorBundleViews(std::vector<NamedTensorView>(257, views.front())),
                    std::invalid_argument);
  const std::vector<std::uint8_t> empty{
    0x4e, 0x44, 0x49, 0x54, 0x42, 0x30, 0x30, 0x31, 0, 0, 0, 0};
  BOOST_CHECK(encodeTensorBundleViews({}) == empty);
  tensors.front().payload.front() = 255;
  views.clear();
  tensors.clear();
  BOOST_CHECK(bundle.payload == expected);
  const auto decoded = decodeTensorBundle(bundle.payload);
  BOOST_REQUIRE_EQUAL(decoded.size(), 3U);
  BOOST_CHECK(decoded[0].payload == bytes<float>({1.0f, -2.0f}));
  BOOST_CHECK(decoded[1].elementType == TensorElementType::Float16);
  BOOST_CHECK(decoded[2].payload.empty());
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

BOOST_AUTO_TEST_CASE(Spec189GreedyViewsReadFreshValuesAndPreserveFirstTies)
{
  for (std::size_t winner = 0; winner < 4; ++winner) {
    std::vector<float> values(4, -2.0f);
    values[winner] = 3.0f;
    std::vector<std::uint8_t> payload(values.size() * sizeof(float));
    std::memcpy(payload.data(), values.data(), payload.size());
    requireToken(sample({"logits", TensorElementType::Float32, {1, 4},
                         std::move(payload)}), winner);
  }
  requireToken(sample({"logits", TensorElementType::Float32, {1, 4},
                       bytes<float>({-0.0f, 0.0f, -1.0f, -2.0f})}), 0);
  requireToken(sample({"logits", TensorElementType::Float16, {1, 4},
                       bytes<std::uint16_t>({0x3c00, 0x3c00, 0, 0})}), 0);
  // A later invalid value must still reject, even after the maximum finite value.
  const auto invalid = sample({"logits", TensorElementType::Float32, {1, 4},
    bytes<float>({std::numeric_limits<float>::max(), 0, -1,
                  std::numeric_limits<float>::quiet_NaN()})});
  BOOST_CHECK(!invalid.error.empty());
  BOOST_CHECK_EQUAL(invalid.events, 0);
  BOOST_CHECK(invalid.feedback.empty());
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
