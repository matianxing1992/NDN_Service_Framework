#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <future>
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
payloadText(const TensorBundle& value)
{
  return std::string(value.payload.begin(), value.payload.end());
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
  BOOST_CHECK_LT(durationMs(head0.startedAt, head1.finishedAt), 90.0);
  BOOST_CHECK_LT(durationMs(head1.startedAt, head0.finishedAt), 90.0);
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

BOOST_AUTO_TEST_CASE(NativeProviderHandlerRejectsMissingRunnerFactory)
{
  NativeProviderHandlerConfig config;
  BOOST_CHECK_THROW(makeNativeProviderCollaborationHandler(std::move(config)),
                    std::invalid_argument);
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

BOOST_AUTO_TEST_CASE(NativeExecutionPlanLoadsFromGeneratedJsonShape)
{
  std::istringstream input(R"JSON({
    "version": 1,
    "services": [
      {
        "service": "/AI/Toy/Inference",
        "model": "/Model/Toy/v1",
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
            "required": true
          }
        ]
      }
    ]
  })JSON");

  const auto plan = nativeExecutionPlanForServiceFromJson(input, "/AI/Toy/Inference");
  BOOST_REQUIRE_EQUAL(plan.roles.size(), 2);
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 1);
  BOOST_CHECK_EQUAL(plan.dependencies[0].keyScope, "stage0-to-stage1");
  BOOST_CHECK_EQUAL(plan.dependencies[0].expectedSegments, 3);
  BOOST_CHECK_EQUAL(plan.dependencies[0].expectedBytes, 17000);

  NativeProviderAssignment assignment;
  assignment.providerByRole["/Stage/0"] = "/example/provider/stage0";
  assignment.providerByRole["/Stage/1"] = "/example/provider/stage1";
  const auto stage1 = roleSpecFor(plan, "/Stage/1", "/run-json", assignment);
  BOOST_REQUIRE_EQUAL(stage1.inputs.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs[0].expectedBytes, 17000);
  BOOST_CHECK_EQUAL(
    stage1.inputs[0].plannedDataName,
                    "/example/provider/stage0/NDNSF/DI/ACTIVATION/run-json/stage0-to-stage1/Stage/0/bundle/0");
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
  BOOST_CHECK_LT(durationMs(timingByRole.at("/Head/0").startedAt,
                            timingByRole.at("/Head/1").finishedAt),
                 90.0);
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
  BOOST_REQUIRE(plan.roles.size() >= 4);

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  std::vector<RoleSpec> roles;
  roles.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
    roles.push_back(roleSpecFor(plan, role, "/generated-plan-run", assignment));
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
        BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
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
  BOOST_REQUIRE(timingByRole.count("/Backbone") == 1);
  BOOST_REQUIRE(timingByRole.count("/Merge") == 1);

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
  BOOST_REQUIRE(plan.roles.size() >= 4);

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  std::map<std::string, RoleSpec> roleSpecs;
  for (const auto& role : plan.roles) {
    roleSpecs.emplace(role, roleSpecFor(plan, role, "/generated-provider-run", assignment));
  }
  BOOST_REQUIRE(roleSpecs.count("/Backbone") == 1);
  BOOST_REQUIRE(roleSpecs.count("/Merge") == 1);
  BOOST_REQUIRE_GE(roleSpecs.at("/Merge").inputs.size(), 2);

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
          BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
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

  BOOST_REQUIRE(resultsByRole.count("/Backbone") == 1);
  BOOST_REQUIRE(resultsByRole.count("/Merge") == 1);
  BOOST_CHECK(resultsByRole.at("/Backbone").inputTimings.empty());
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
  BOOST_REQUIRE(plan.roles.size() >= 4);

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
            BOOST_REQUIRE_EQUAL(ctx.inputsByScope.size(), 1);
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

  BOOST_REQUIRE(resultsByRole.count("/Backbone") == 1);
  BOOST_REQUIRE(resultsByRole.count("/Merge") == 1);
  BOOST_CHECK(resultsByRole.at("/Backbone").inputTimings.empty());
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

} // namespace ndnsf::di::test
