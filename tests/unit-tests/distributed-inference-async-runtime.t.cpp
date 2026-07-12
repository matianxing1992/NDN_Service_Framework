#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderResourceProbe.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "ndn-service-framework/NegativeAckReason.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
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
};

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
  BOOST_CHECK_GE(snapshot.pendingWorkCount(), 2);
  BOOST_CHECK_EQUAL(snapshot.idleWorkerCount(), 0);

  releasePromise.set_value();
  BOOST_CHECK_EQUAL(payloadText(first.get().outputsByScope.at("final-response")), "ok");
  BOOST_CHECK_EQUAL(payloadText(second.get().outputsByScope.at("final-response")), "ok");
}

BOOST_AUTO_TEST_CASE(NativeProviderHandlerRejectsMissingRunnerFactory)
{
  NativeProviderHandlerConfig config;
  BOOST_CHECK_THROW(makeNativeProviderCollaborationHandler(std::move(config)),
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
    "/provider/A", "boot-a", 7,
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
  evidence.deviceId = "GPU-1";
  evidence.runtimeVersion = "onnxruntime=1;cuda=1";
  evidence.modelDigest = "sha256:model";
  evidence.planDigest = "sha256:plan";
  evidence.artifactDigests["/LLM/Stage/0"] = "sha256:stage0";
  evidence.roles = {"/LLM/Stage/0"};
  evidence.createdAtMs = 1234;
  const auto json = executionEvidenceToJson(evidence);
  BOOST_CHECK(json.find("token") == std::string::npos);
  BOOST_CHECK(json.find("prompt") == std::string::npos);
  const auto decoded = executionEvidenceFromJson(json);
  BOOST_CHECK_EQUAL(decoded.providerBootId, "boot-a");
  BOOST_CHECK(decoded.runnerKind == RunnerKind::OnnxRuntimeCuda);
  BOOST_CHECK_EQUAL(decoded.artifactDigests.at("/LLM/Stage/0"), "sha256:stage0");
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

} // namespace ndnsf::di::test
