#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <chrono>
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

} // namespace ndnsf::di::test
