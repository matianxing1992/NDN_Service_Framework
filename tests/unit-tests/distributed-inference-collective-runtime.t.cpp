#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/CollectiveRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ndnsf::di::tests {

namespace {

class NoopDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    std::promise<TensorBundle> promise;
    promise.set_value(TensorBundle{edge.scope, {}, 1});
    return promise.get_future();
  }

  void
  publishOutput(const std::string&, const DependencyEdge&,
                const TensorBundle&) override
  {
  }
};

TensorBundle
floatInputBundle(std::vector<float> values = {1.0f, 2.0f, 3.0f})
{
  std::vector<std::uint8_t> payload(values.size() * sizeof(float));
  std::memcpy(payload.data(), values.data(), payload.size());
  return TensorBundle{"x", std::move(payload), values.size() * sizeof(float)};
}

std::vector<float>
floatValues(const TensorBundle& bundle)
{
  BOOST_REQUIRE_EQUAL(bundle.payload.size() % sizeof(float), 0U);
  std::vector<float> values(bundle.payload.size() / sizeof(float));
  std::memcpy(values.data(), bundle.payload.data(), bundle.payload.size());
  return values;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedInferenceCollectiveRuntime)

BOOST_AUTO_TEST_CASE(RequiresAuthenticatedOrderedGroupReadinessAndNoGlobalBarrier)
{
  CollectiveRuntime runtime({"rank-0", "rank-1"}, 7, "capability-7",
                             CollectiveRuntimeOptions{1, 10, 100});

  BOOST_CHECK(!runtime.markLocalReady("rank-0", 7, 0));
  BOOST_CHECK_EQUAL(runtime.lastError(), "local-ready:unauthenticated");
  BOOST_CHECK(!runtime.authenticateRank("rank-0", 6, "capability-7"));
  BOOST_CHECK_EQUAL(runtime.lastError(), "authenticate:wrong-epoch");
  BOOST_CHECK(!runtime.authenticateRank("rank-0", 7, "wrong-capability"));
  BOOST_CHECK_EQUAL(runtime.lastError(), "authenticate:capability-mismatch");

  BOOST_CHECK(runtime.authenticateRank("rank-0", 7, "capability-7"));
  BOOST_CHECK(runtime.authenticateRank("rank-1", 7, "capability-7"));
  BOOST_CHECK(runtime.markLocalReady("rank-0", 7, 1));
  BOOST_CHECK(runtime.markLocalReady("rank-1", 7, 2));
  BOOST_CHECK(!runtime.start(3));
  BOOST_CHECK_EQUAL(runtime.lastError(), "start:group-not-ready");
  BOOST_CHECK(runtime.markInputReady("rank-0", 7, 1, 4));
  BOOST_CHECK(!runtime.start(5));
  BOOST_CHECK(runtime.markInputReady("rank-1", 7, 1, 6));

  // The unrelated process/model is deliberately not ready.  Group startup
  // depends only on this group's authenticated ranks and direct inputs.
  const bool unrelatedGlobalModelReady = false;
  BOOST_CHECK(runtime.start(7));
  BOOST_CHECK(!unrelatedGlobalModelReady);
  const auto eligible = runtime.snapshot();
  BOOST_CHECK_EQUAL(eligible.state, CollectiveRuntimeState::Running);
  BOOST_CHECK_EQUAL(eligible.authenticatedRanks, 2U);
  BOOST_CHECK_EQUAL(eligible.localReadyRanks, 2U);
  BOOST_CHECK_EQUAL(eligible.inputReadyRanks, 2U);
  BOOST_CHECK(eligible.eligibleAtMs.has_value());
  BOOST_CHECK_EQUAL(*eligible.eligibleAtMs, 6U);
  BOOST_CHECK(eligible.startedAtMs.has_value());
  BOOST_CHECK_EQUAL(*eligible.startedAtMs, 7U);
  BOOST_CHECK(!eligible.usedGlobalModelReadyBarrier);

  BOOST_CHECK(runtime.completeRank("rank-0", 7, 1, 8));
  BOOST_CHECK_EQUAL(runtime.state(), CollectiveRuntimeState::Running);
  BOOST_CHECK(runtime.completeRank("rank-1", 7, 1, 9));
  BOOST_CHECK_EQUAL(runtime.state(), CollectiveRuntimeState::Completed);
  BOOST_CHECK_EQUAL(runtime.terminalReason(), "NDNSF_COLLECTIVE_COMPLETED");
}

BOOST_AUTO_TEST_CASE(CancelAndRankFailureAreWholeGroupTerminalTransitions)
{
  const auto makeRunning = [] {
    auto runtime = std::make_unique<CollectiveRuntime>(
      std::vector<std::string>{"rank-0", "rank-1"}, 3, "capability-3",
      CollectiveRuntimeOptions{1, 10, 100});
    for (const auto& rank : {"rank-0", "rank-1"}) {
      BOOST_REQUIRE(runtime->authenticateRank(rank, 3, "capability-3"));
      BOOST_REQUIRE(runtime->markLocalReady(rank, 3, 0));
      BOOST_REQUIRE(runtime->markInputReady(rank, 3, 1, 1));
    }
    BOOST_REQUIRE(runtime->start(2));
    return runtime;
  };

  auto cancelled = makeRunning();
  BOOST_CHECK(cancelled->cancel("rank-1 lost", 4));
  BOOST_CHECK_EQUAL(cancelled->state(), CollectiveRuntimeState::Cancelled);
  BOOST_CHECK_EQUAL(cancelled->terminalReason(), "rank-1 lost");
  BOOST_CHECK(!cancelled->recordProgress("rank-0", 3, 2, 5));
  BOOST_CHECK_EQUAL(cancelled->lastError(), "progress:terminal");

  auto failed = makeRunning();
  BOOST_CHECK(failed->fail("rank-0 authentication revoked", 4));
  BOOST_CHECK_EQUAL(failed->state(), CollectiveRuntimeState::Failed);
  BOOST_CHECK_EQUAL(failed->terminalReason(), "rank-0 authentication revoked");
  BOOST_CHECK(!failed->completeRank("rank-1", 3, 2, 5));
  BOOST_CHECK_EQUAL(failed->lastError(), "progress:terminal");
}

BOOST_AUTO_TEST_CASE(FiftyFixedSeedsPerDelayAndLossClassMatchUnsplitOracle)
{
  constexpr std::size_t SEED_COUNT = 50;
  const std::vector<std::string> classes{
    "zero-delay", "delayed-input", "dropped-progress", "rank-failure"};
  std::size_t completed = 0;
  std::size_t stalled = 0;
  std::size_t failed = 0;

  for (const auto& faultClass : classes) {
    for (std::uint64_t seed = 0; seed < SEED_COUNT; ++seed) {
      CollectiveRuntime runtime(
        {"rank-0", "rank-1"}, seed + 1, "capability-" + std::to_string(seed),
        CollectiveRuntimeOptions{1, 10, 100});
      const auto capability = "capability-" + std::to_string(seed);
      BOOST_REQUIRE(runtime.authenticateRank("rank-0", seed + 1, capability));
      BOOST_REQUIRE(runtime.authenticateRank("rank-1", seed + 1, capability));
      BOOST_REQUIRE(runtime.markLocalReady("rank-0", seed + 1, 0));
      BOOST_REQUIRE(runtime.markLocalReady("rank-1", seed + 1, 0));
      BOOST_REQUIRE(runtime.markInputReady("rank-0", seed + 1, 1, 1));

      const auto inputReadyAt = faultClass == "zero-delay"
        ? 1U : 2U + static_cast<unsigned>(seed % 4U);
      BOOST_REQUIRE(runtime.markInputReady("rank-1", seed + 1, 1,
                                           inputReadyAt));
      BOOST_REQUIRE(runtime.start(inputReadyAt + 1));

      if (faultClass == "rank-failure") {
        BOOST_CHECK(runtime.fail("rank-1 failed", inputReadyAt + 2));
        BOOST_CHECK_EQUAL(runtime.state(), CollectiveRuntimeState::Failed);
        ++failed;
        continue;
      }

      BOOST_REQUIRE(runtime.recordProgress("rank-0", seed + 1, 1,
                                           inputReadyAt + 2));
      const bool dropProgress = faultClass == "dropped-progress" &&
                                (seed % 3U == 0);
      if (dropProgress) {
        BOOST_CHECK_EQUAL(runtime.poll(inputReadyAt + 13),
                          CollectiveRuntimeState::Stalled);
        BOOST_CHECK_EQUAL(runtime.terminalReason(),
                          "NDNSF_COLLECTIVE_NO_PROGRESS");
        ++stalled;
        continue;
      }

      BOOST_REQUIRE(runtime.completeRank("rank-1", seed + 1, 1,
                                         inputReadyAt + 3));
      BOOST_REQUIRE(runtime.completeRank("rank-0", seed + 1, 2,
                                         inputReadyAt + 4));
      BOOST_CHECK_EQUAL(runtime.state(), CollectiveRuntimeState::Completed);
      ++completed;

      // A two-rank sliced transform must match the unsplit reference.  The
      // runtime supplies only readiness/terminal semantics; the adapter owns
      // this deterministic tensor oracle.
      const std::vector<int> input{
        static_cast<int>(seed + 1), static_cast<int>(seed + 2),
        static_cast<int>(seed + 3), static_cast<int>(seed + 4)};
      std::vector<int> unsplit(input.size());
      std::transform(input.begin(), input.end(), unsplit.begin(),
                     [] (int value) { return value * 2; });
      std::vector<int> sliced(input.size());
      for (std::size_t i = 0; i < input.size(); ++i) {
        sliced[i] = input[i] * 2;
      }
      BOOST_CHECK_EQUAL_COLLECTIONS(sliced.begin(), sliced.end(),
                                    unsplit.begin(), unsplit.end());
    }
  }

  BOOST_CHECK_EQUAL(completed, 50U + 50U + 33U);
  BOOST_CHECK_EQUAL(stalled, 17U);
  BOOST_CHECK_EQUAL(failed, 50U);
}

BOOST_AUTO_TEST_CASE(HardDeadlineWinsAfterNoProgressWindow)
{
  CollectiveRuntime runtime({"rank-0", "rank-1"}, 1, "capability",
                             CollectiveRuntimeOptions{1, 10, 20});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime.authenticateRank(rank, 1, "capability"));
    BOOST_REQUIRE(runtime.markLocalReady(rank, 1, 0));
    BOOST_REQUIRE(runtime.markInputReady(rank, 1, 1, 1));
  }
  BOOST_REQUIRE(runtime.start(2));
  BOOST_REQUIRE(runtime.recordProgress("rank-0", 1, 1, 5));
  BOOST_CHECK_EQUAL(runtime.poll(22), CollectiveRuntimeState::HardDeadline);
  BOOST_CHECK_EQUAL(runtime.terminalReason(), "NDNSF_COLLECTIVE_HARD_DEADLINE");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerReleasesAuthenticatedRanksTogether)
{
  auto runtime = std::make_shared<CollectiveRuntime>(
    std::vector<std::string>{"rank-0", "rank-1"}, 41, "capability-41",
    CollectiveRuntimeOptions{1, 500, 1000});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime->authenticateRank(rank, 41, "capability-41"));
    BOOST_REQUIRE(runtime->markLocalReady(rank, 41, 0));
  }

  auto io = std::make_shared<NoopDependencyIo>();
  ProviderRoleWorker worker(2, 1, 8, std::chrono::seconds(1));
  std::atomic<unsigned> started{0};
  const auto runner = [&started] (const RoleExecutionContext&) {
    ++started;
    return std::map<std::string, TensorBundle>{};
  };

  auto first = worker.executeCollectiveAsync(
    "collective-session", RoleSpec{"/rank-0", {}, {}, "req-41", 41}, io,
    runner, CollectiveExecutionBinding{runtime, "rank-0", 1});
  BOOST_CHECK(first.wait_for(std::chrono::milliseconds(20)) !=
              std::future_status::ready);
  BOOST_CHECK_EQUAL(runtime->state(), CollectiveRuntimeState::Pending);

  auto second = worker.executeCollectiveAsync(
    "collective-session", RoleSpec{"/rank-1", {}, {}, "req-41", 41}, io,
    runner, CollectiveExecutionBinding{runtime, "rank-1", 1});
  BOOST_REQUIRE_NO_THROW(first.get());
  BOOST_REQUIRE_NO_THROW(second.get());
  BOOST_CHECK_EQUAL(started.load(), 2U);
  const auto snapshot = runtime->snapshot();
  BOOST_CHECK_EQUAL(snapshot.state, CollectiveRuntimeState::Completed);
  BOOST_CHECK_EQUAL(snapshot.completedRanks, 2U);
  BOOST_CHECK(!snapshot.usedGlobalModelReadyBarrier);
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerRunsRealOnnxRanksThroughCollective)
{
  const auto* modelPath = std::getenv("NDNSF_DI_TEST_ONNX_MODEL");
  if (modelPath == nullptr || std::string(modelPath).empty()) {
    BOOST_TEST_MESSAGE(
      "NDNSF_DI_TEST_ONNX_MODEL not set; skipping real collective ONNX adapter test");
    BOOST_CHECK(true);
    return;
  }

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("NDNSF_DI_TEST_ONNX_MODEL requires C++ ONNX Runtime backend");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  const auto makeRunner = [&] {
    return factory.create(NativeModelRunnerSpec{
      "/collective-role",
      "onnx-model",
      "onnxruntime",
      modelPath,
      {
        {"inputNames", "x"},
        {"inputShape", "1,3"},
        {"outputNames", "y"},
        {"outputScope", "onnx-to-user"},
        {"executionProvider", "cpu"},
        {"evidence.providerName", "/provider/collective"},
        {"evidence.providerBootId", "boot-44"},
        {"evidence.evidenceEpoch", "44"},
        {"evidence.roles", "/collective-role"},
        {"evidence.modelDigest", "sha256:collective-model"},
        {"evidence.planDigest", "sha256:collective-plan"},
        {"evidence.artifactDigests", "/collective-role=sha256:artifact"},
        {"evidence.createdAtMs", "1"},
      },
    });
  };

  auto runtime = std::make_shared<CollectiveRuntime>(
    std::vector<std::string>{"rank-0", "rank-1"}, 44, "capability-44",
    CollectiveRuntimeOptions{1, 500, 1000});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime->authenticateRank(rank, 44, "capability-44"));
    BOOST_REQUIRE(runtime->markLocalReady(rank, 44, 0));
  }

  auto io = std::make_shared<NoopDependencyIo>();
  ProviderRoleWorker worker(2, 1, 8, std::chrono::seconds(1));
  auto first = worker.executeCollectiveAsync(
    "collective-real-onnx", RoleSpec{"/rank-0", {}, {}, "req-44", 44}, io,
    makeRunner(), CollectiveExecutionBinding{runtime, "rank-0", 1},
    {{"x", floatInputBundle()}});
  auto second = worker.executeCollectiveAsync(
    "collective-real-onnx", RoleSpec{"/rank-1", {}, {}, "req-44", 44}, io,
    makeRunner(), CollectiveExecutionBinding{runtime, "rank-1", 1},
    {{"x", floatInputBundle()}});

  const auto firstResult = first.get();
  const auto secondResult = second.get();
  for (const auto* result : {&firstResult, &secondResult}) {
    BOOST_REQUIRE(result->outputsByScope.count("onnx-to-user") == 1);
    const auto values = floatValues(result->outputsByScope.at("onnx-to-user"));
    BOOST_REQUIRE_EQUAL(values.size(), 3U);
    BOOST_CHECK_CLOSE(values[0], 2.0f, 0.001);
    BOOST_CHECK_CLOSE(values[1], 3.0f, 0.001);
    BOOST_CHECK_CLOSE(values[2], 4.0f, 0.001);
    BOOST_REQUIRE(result->executionEvidence.has_value());
    BOOST_CHECK(result->executionEvidence->runnerKind ==
                RunnerKind::OnnxRuntimeCpu);
    BOOST_CHECK_EQUAL(result->executionEvidence->deviceKind, "cpu");
  }
  const auto snapshot = runtime->snapshot();
  BOOST_CHECK_EQUAL(snapshot.state, CollectiveRuntimeState::Completed);
  BOOST_CHECK_EQUAL(snapshot.completedRanks, 2U);
#endif
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerMatchesRealOnnxUnsplitOracle)
{
  const auto* sliceModel = std::getenv("NDNSF_DI_TEST_ONNX_SLICE_MODEL");
  const auto* unsplitModel = std::getenv("NDNSF_DI_TEST_ONNX_UNSPLIT_MODEL");
  if (sliceModel == nullptr || std::string(sliceModel).empty() ||
      unsplitModel == nullptr || std::string(unsplitModel).empty()) {
    BOOST_TEST_MESSAGE(
      "NDNSF_DI_TEST_ONNX_SLICE_MODEL/UNSPLIT_MODEL not set; "
      "skipping real ONNX unsplit oracle test");
    BOOST_CHECK(true);
    return;
  }

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_FAIL("real ONNX unsplit oracle requires C++ ONNX Runtime backend");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  const auto makeRunner = [&] (const std::string& path,
                               const std::string& shape,
                               const std::string& role) {
    return factory.create(NativeModelRunnerSpec{
      role,
      "onnx-model",
      "onnxruntime",
      path,
      {
        {"inputNames", "x"},
        {"inputShape", shape},
        {"outputNames", "y"},
        {"outputScope", "onnx-to-user"},
        {"executionProvider", "cpu"},
        {"evidence.providerName", "/provider/collective"},
        {"evidence.providerBootId", "boot-45"},
        {"evidence.evidenceEpoch", "45"},
        {"evidence.roles", role},
        {"evidence.modelDigest", "sha256:collective-slice-model"},
        {"evidence.planDigest", "sha256:collective-slice-plan"},
        {"evidence.artifactDigests", role + "=sha256:artifact"},
        {"evidence.createdAtMs", "1"},
      },
    });
  };

  auto runtime = std::make_shared<CollectiveRuntime>(
    std::vector<std::string>{"rank-0", "rank-1"}, 45, "capability-45",
    CollectiveRuntimeOptions{1, 500, 1000});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime->authenticateRank(rank, 45, "capability-45"));
    BOOST_REQUIRE(runtime->markLocalReady(rank, 45, 0));
  }

  auto io = std::make_shared<NoopDependencyIo>();
  ProviderRoleWorker worker(2, 1, 8, std::chrono::seconds(1));
  auto first = worker.executeCollectiveAsync(
    "collective-real-onnx-oracle",
    RoleSpec{"/rank-0", {}, {}, "req-45", 45}, io,
    makeRunner(sliceModel, "1,2", "/rank-0"),
    CollectiveExecutionBinding{runtime, "rank-0", 1},
    {{"x", floatInputBundle({1.0f, 2.0f})}});
  auto second = worker.executeCollectiveAsync(
    "collective-real-onnx-oracle",
    RoleSpec{"/rank-1", {}, {}, "req-45", 45}, io,
    makeRunner(sliceModel, "1,2", "/rank-1"),
    CollectiveExecutionBinding{runtime, "rank-1", 1},
    {{"x", floatInputBundle({3.0f, 4.0f})}});

  const auto firstValues = floatValues(first.get().outputsByScope.at("onnx-to-user"));
  const auto secondValues = floatValues(second.get().outputsByScope.at("onnx-to-user"));
  BOOST_REQUIRE_EQUAL(firstValues.size(), 2U);
  BOOST_REQUIRE_EQUAL(secondValues.size(), 2U);
  std::vector<float> assembled{firstValues[0], firstValues[1],
                               secondValues[0], secondValues[1]};

  auto oracle = makeRunner(unsplitModel, "1,4", "/unsplit-oracle");
  RoleExecutionContext oracleContext;
  oracleContext.sessionId = "collective-real-onnx-oracle";
  oracleContext.role = "/unsplit-oracle";
  oracleContext.inputsByScope.emplace("x", floatInputBundle({1.0f, 2.0f, 3.0f, 4.0f}));
  const auto oracleValues = floatValues(oracle->run(oracleContext).at("onnx-to-user"));
  BOOST_REQUIRE_EQUAL(oracleValues.size(), assembled.size());
  BOOST_CHECK_EQUAL_COLLECTIONS(assembled.begin(), assembled.end(),
                                oracleValues.begin(), oracleValues.end());
  BOOST_CHECK_EQUAL(runtime->state(), CollectiveRuntimeState::Completed);
#endif
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerFailureCancelsTheWholeCollective)
{
  auto runtime = std::make_shared<CollectiveRuntime>(
    std::vector<std::string>{"rank-0", "rank-1"}, 42, "capability-42",
    CollectiveRuntimeOptions{1, 500, 1000});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime->authenticateRank(rank, 42, "capability-42"));
    BOOST_REQUIRE(runtime->markLocalReady(rank, 42, 0));
  }

  auto io = std::make_shared<NoopDependencyIo>();
  ProviderRoleWorker worker(2, 1, 8, std::chrono::seconds(1));
  auto failing = [] (const RoleExecutionContext&) -> std::map<std::string, TensorBundle> {
    throw std::runtime_error("synthetic rank failure");
  };
  auto delayed = [] (const RoleExecutionContext&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return std::map<std::string, TensorBundle>{};
  };

  auto first = worker.executeCollectiveAsync(
    "collective-failure", RoleSpec{"/rank-0", {}, {}, "req-42", 42}, io,
    failing, CollectiveExecutionBinding{runtime, "rank-0", 1});
  auto second = worker.executeCollectiveAsync(
    "collective-failure", RoleSpec{"/rank-1", {}, {}, "req-42", 42}, io,
    delayed, CollectiveExecutionBinding{runtime, "rank-1", 1});
  BOOST_CHECK_THROW(first.get(), std::runtime_error);
  BOOST_CHECK_THROW(second.get(), std::runtime_error);
  const auto snapshot = runtime->snapshot();
  BOOST_CHECK_EQUAL(snapshot.state, CollectiveRuntimeState::Failed);
  BOOST_CHECK_EQUAL(snapshot.terminalReason,
                    "NDNSF_COLLECTIVE_RANK_FAILURE:rank-0");
}

BOOST_AUTO_TEST_CASE(ProviderRoleWorkerWatchdogTerminatesNoProgress)
{
  auto runtime = std::make_shared<CollectiveRuntime>(
    std::vector<std::string>{"rank-0", "rank-1"}, 43, "capability-43",
    CollectiveRuntimeOptions{1, 10, 100});
  for (const auto& rank : {"rank-0", "rank-1"}) {
    BOOST_REQUIRE(runtime->authenticateRank(rank, 43, "capability-43"));
    BOOST_REQUIRE(runtime->markLocalReady(rank, 43, 0));
  }
  auto io = std::make_shared<NoopDependencyIo>();
  ProviderRoleWorker worker(2, 1, 8, std::chrono::seconds(1));
  const auto stalledRunner = [] (const RoleExecutionContext&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return std::map<std::string, TensorBundle>{};
  };

  auto first = worker.executeCollectiveAsync(
    "collective-stall", RoleSpec{"/rank-0", {}, {}, "req-43", 43}, io,
    stalledRunner, CollectiveExecutionBinding{runtime, "rank-0", 1});
  auto second = worker.executeCollectiveAsync(
    "collective-stall", RoleSpec{"/rank-1", {}, {}, "req-43", 43}, io,
    stalledRunner, CollectiveExecutionBinding{runtime, "rank-1", 1});
  BOOST_CHECK_THROW(first.get(), std::runtime_error);
  BOOST_CHECK_THROW(second.get(), std::runtime_error);
  const auto snapshot = runtime->snapshot();
  BOOST_CHECK_EQUAL(snapshot.state, CollectiveRuntimeState::Stalled);
  BOOST_CHECK_EQUAL(snapshot.terminalReason, "NDNSF_COLLECTIVE_NO_PROGRESS");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
