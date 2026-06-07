#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <chrono>
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

} // namespace ndnsf::di::test
