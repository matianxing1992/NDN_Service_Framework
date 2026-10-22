#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/execution-evidence-aggregation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::test {
namespace {

using EvidenceByRole = std::map<std::string, ExecutionEvidence>;
using detail::aggregateExecutionEvidence;

ExecutionEvidence
makeEvidence(std::string role = "/Stage/0", std::size_t nodes = 1)
{
  ExecutionEvidence value;
  value.providerName = "/provider/A";
  value.providerBootId = "boot-a";
  value.evidenceEpoch = 9;
  value.runnerKind = RunnerKind::OnnxRuntimeCpu;
  value.realCompute = true;
  value.deviceKind = "cpu";
  value.deviceId = "cpu-0";
  value.runtimeVersion = "ort";
  value.modelDigest = "sha256:model";
  value.planDigest = "sha256:plan";
  value.artifactDigests[role] = "sha256:artifact";
  value.roles = {role};
  value.loadCompleted = true;
  value.warmupCompleted = true;
  value.createdAtMs = 1;
  value.processId = 7;
  value.requestId = "/request/old";
  value.attemptEpoch = 3;
  value.executionCompleted = true;
  value.providerProfilePath = "/profile/old";
  value.profileRequestId = "/request/profile";
  value.profileAttemptEpoch = 2;
  for (std::size_t i = 0; i < nodes; ++i) {
    value.nodeProviderAssignments.push_back(
      {role, "/model/layer/attention/MatMul_" + std::to_string(i),
       "CPUExecutionProvider", true});
  }
  value.validate();
  return value;
}

void
updateReadiness(NativeProviderReadinessState& state, const EvidenceByRole& items,
                bool useOwningIntermediate)
{
  // Counterfactual uses the removed call-site copies, with the same maintained
  // native aggregation/validation/readiness APIs as the direct-map path.
  if (useOwningIntermediate) {
    std::vector<ExecutionEvidence> current;
    current.reserve(items.size());
    for (const auto& item : items) current.push_back(item.second);
    const auto aggregate = aggregateExecutionEvidence(current);
    state.setExecutionEvidence(aggregate);
  }
  else {
    auto aggregate = aggregateExecutionEvidence(items);
    state.setExecutionEvidence(std::move(aggregate));
  }
  state.setExecutionEvidenceByRole(items);
}

std::uint64_t
threadCpuNs()
{
  timespec stamp{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stamp) != 0) {
    throw std::runtime_error("thread CPU clock unavailable");
  }
  return static_cast<std::uint64_t>(stamp.tv_sec) * 1000000000 + stamp.tv_nsec;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec216WarmEvidence)

BOOST_AUTO_TEST_CASE(SingleObservationPreservesFieldsAndOwnsInventory)
{
  auto value = makeEvidence();
  value.roles = {"/Stage/1", "/Stage/0", "/Stage/1"};
  const auto output = aggregateExecutionEvidence(EvidenceByRole{{"a", value}});
  BOOST_CHECK_EQUAL(output.providerName, "/provider/A");
  BOOST_CHECK_EQUAL(output.providerBootId, "boot-a");
  BOOST_CHECK_EQUAL(output.evidenceEpoch, 9);
  BOOST_CHECK(output.executionCompleted);
  BOOST_CHECK_EQUAL(output.requestId, "/request/old");
  BOOST_CHECK_EQUAL(output.attemptEpoch, 3);
  BOOST_CHECK_EQUAL(output.profileRequestId, "/request/profile");
  BOOST_CHECK_EQUAL(output.profileAttemptEpoch, 2);
  BOOST_CHECK_EQUAL(output.providerProfilePath, "/profile/old");
  BOOST_REQUIRE_EQUAL(output.roles.size(), 2);
  BOOST_CHECK_EQUAL(output.roles[0], "/Stage/0");
  BOOST_CHECK_EQUAL(output.roles[1], "/Stage/1");
  BOOST_REQUIRE_EQUAL(output.deviceIds.size(), 1);
  BOOST_CHECK_EQUAL(output.deviceIds.front(), "cpu-0");
  BOOST_REQUIRE_EQUAL(output.nodeProviderAssignments.size(), 1);
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[0].nodeName, "/model/layer/attention/MatMul_0");
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[0].provider, "CPUExecutionProvider");
  BOOST_CHECK(output.nodeProviderAssignments[0].modelNode);
  BOOST_CHECK_EQUAL(executionEvidenceToJson(output), executionEvidenceToJson(
    aggregateExecutionEvidence(std::vector<ExecutionEvidence>{value})));
  value.nodeProviderAssignments[0].nodeName = "mutated";
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[0].nodeName, "/model/layer/attention/MatMul_0");
}

BOOST_AUTO_TEST_CASE(MultipleRolesRetainOrderAndClearPerRequestClaims)
{
  auto first = makeEvidence("/Stage/1");
  auto second = makeEvidence("/Stage/0");
  first.deviceIds = {"cpu-1", "cpu-0", "cpu-1"};
  second.deviceId = "cpu-2";
  first.gpuUuids = {"uuid-1"};
  second.gpuUuid = "uuid-2";
  first.providerProfilePath.clear();
  second.providerProfilePath = "/profile/second";
  second.requestId = "/request/new";
  EvidenceByRole items{{"b", second}, {"a", first}};
  const auto output = aggregateExecutionEvidence(items);
  BOOST_CHECK_EQUAL(output.deviceId, "multi");
  BOOST_REQUIRE_EQUAL(output.deviceIds.size(), 3);
  BOOST_CHECK_EQUAL(output.deviceIds[0], "cpu-1");
  BOOST_CHECK_EQUAL(output.deviceIds[1], "cpu-0");
  BOOST_CHECK_EQUAL(output.deviceIds[2], "cpu-2");
  BOOST_CHECK_EQUAL(output.gpuUuid, "multi");
  BOOST_REQUIRE_EQUAL(output.gpuUuids.size(), 2);
  BOOST_CHECK_EQUAL(output.gpuUuids[0], "uuid-1");
  BOOST_CHECK_EQUAL(output.gpuUuids[1], "uuid-2");
  BOOST_REQUIRE_EQUAL(output.nodeProviderAssignments.size(), 2);
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[0].role, "/Stage/1");
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[1].role, "/Stage/0");
  BOOST_CHECK_EQUAL(output.providerProfilePath, "/profile/second");
  BOOST_CHECK_EQUAL(output.artifactDigests.size(), 2);
  BOOST_CHECK(!output.executionCompleted);
  BOOST_CHECK(!output.exactForwardCacheHit);
  BOOST_CHECK(output.requestId.empty());
  BOOST_CHECK_EQUAL(output.attemptEpoch, 0);
  BOOST_CHECK(output.profileRequestId.empty());
  BOOST_CHECK_EQUAL(output.profileAttemptEpoch, 0);
  BOOST_CHECK_EQUAL(executionEvidenceToJson(output), executionEvidenceToJson(
    aggregateExecutionEvidence(std::vector<ExecutionEvidence>{first, second})));
}

BOOST_AUTO_TEST_CASE(RejectEmptyConflictingAndInvalidObservations)
{
  BOOST_CHECK_THROW(aggregateExecutionEvidence(EvidenceByRole{}), std::runtime_error);
  BOOST_CHECK_THROW(aggregateExecutionEvidence(std::vector<ExecutionEvidence>{}),
                    std::runtime_error);
  const std::vector<std::function<void(ExecutionEvidence&)>> mutations{
    [](auto& item) { item.providerName = "/foreign"; },
    [](auto& item) { item.providerBootId = "other-boot"; },
    [](auto& item) { item.runnerKind = RunnerKind::WiringOnly; },
    [](auto& item) { item.realCompute = false; },
    [](auto& item) { item.modelDigest = "different-model"; },
    [](auto& item) { item.planDigest = "different-plan"; },
    [](auto& item) { item.runtimeVersion = "different-runtime"; },
    [](auto& item) { item.deviceKind = "cuda"; },
    [](auto& item) { item.cpuFallbackUsed = true; },
  };
  for (const auto& mutate : mutations) {
    auto second = makeEvidence();
    mutate(second);
    BOOST_CHECK_THROW(aggregateExecutionEvidence(EvidenceByRole{
      {"a", makeEvidence()}, {"b", second}}), std::runtime_error);
  }
  auto invalid = makeEvidence();
  invalid.artifactDigests.clear();
  BOOST_CHECK_THROW(aggregateExecutionEvidence(EvidenceByRole{{"a", invalid}}),
                    std::invalid_argument);
  invalid = makeEvidence();
  invalid.requestId.clear();
  BOOST_CHECK_THROW(aggregateExecutionEvidence(EvidenceByRole{{"a", invalid}}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LargeAndRepeatedRoleInventoriesRemainOwned)
{
  auto output = [] {
    const auto value = makeEvidence("/Stage/0", 5432);
    return aggregateExecutionEvidence(EvidenceByRole{{"a", value}, {"b", value}});
  }();
  BOOST_REQUIRE_EQUAL(output.nodeProviderAssignments.size(), 10864);
  BOOST_REQUIRE_EQUAL(output.roles.size(), 1);
  BOOST_CHECK_EQUAL(output.roles[0], "/Stage/0");
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[5431].nodeName,
                    "/model/layer/attention/MatMul_5431");
  BOOST_CHECK_EQUAL(output.nodeProviderAssignments[5432].nodeName,
                    "/model/layer/attention/MatMul_0");
  BOOST_CHECK(!output.executionCompleted);
  output.validate();
}

BOOST_AUTO_TEST_CASE(ReadinessRetainsIndependentUpdatedSnapshots)
{
  EvidenceByRole items{{"/Stage/0", makeEvidence()}};
  NativeProviderReadinessState oldState, newState;
  oldState.markReady("ready");
  newState.markReady("ready");
  updateReadiness(oldState, items, true);
  updateReadiness(newState, items, false);
  const auto before = newState.makeAckDecision("/Stage/0");
  BOOST_REQUIRE(before.status);
  const auto oldAck = oldState.makeAckDecision("/Stage/0");
  BOOST_CHECK_EQUAL_COLLECTIONS(before.payload.begin(), before.payload.end(),
                                oldAck.payload.begin(), oldAck.payload.end());
  items.begin()->second.requestId = "/request/updated";
  items.begin()->second.nodeProviderAssignments[0].nodeName = "updated-node";
  const auto unchanged = newState.makeAckDecision("/Stage/0");
  BOOST_CHECK_EQUAL_COLLECTIONS(before.payload.begin(), before.payload.end(),
                                unchanged.payload.begin(), unchanged.payload.end());
  updateReadiness(newState, items, false);
  const auto after = newState.makeAckDecision("/Stage/0");
  BOOST_CHECK(before.payload != after.payload);
  auto invalid = aggregateExecutionEvidence(items);
  invalid.planDigest.clear();
  BOOST_CHECK_THROW(newState.setExecutionEvidence(std::move(invalid)), std::invalid_argument);
  const auto retained = newState.makeAckDecision("/Stage/0");
  BOOST_CHECK_EQUAL_COLLECTIONS(after.payload.begin(), after.payload.end(),
                                retained.payload.begin(), retained.payload.end());
}

BOOST_AUTO_TEST_CASE(LargeInventoryCopyProbe)
{
  if (std::getenv("NDNSF_DI_EVIDENCE_COPY_PROBE") == nullptr) {
    BOOST_TEST_MESSAGE("Opt-in copy-cost probe not requested");
    return;
  }
  for (const auto nodes : {5568, 5432}) {
    const EvidenceByRole items{{"/Stage/0", makeEvidence("/Stage/0", nodes)}};
    for (unsigned repetition = 0; repetition < 3; ++repetition) {
      for (unsigned position = 0; position < 2; ++position) {
        const bool owning = (position + repetition) % 2 == 0;
        NativeProviderReadinessState state;
        state.markReady("ready");
        const auto began = std::chrono::steady_clock::now();
        const auto cpu = threadCpuNs();
        for (unsigned i = 0; i < 129; ++i) updateReadiness(state, items, owning);
        const auto cpuUs = (threadCpuNs() - cpu) / 1000;
        const auto wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - began).count();
        const auto ack = state.makeAckDecision("/Stage/0");
        BOOST_REQUIRE(ack.status);
        NativeProviderReadinessState reference;
        reference.markReady("ready");
        updateReadiness(reference, items, !owning);
        const auto expected = reference.makeAckDecision("/Stage/0");
        BOOST_CHECK_EQUAL_COLLECTIONS(ack.payload.begin(), ack.payload.end(),
                                      expected.payload.begin(), expected.payload.end());
        std::cout << "NDNSF_DI_EVIDENCE_COPY_PROBE nodes=" << nodes
                  << " repetition=" << repetition << " owning=" << owning
                  << " cycles=129 wall_us=" << wallUs << " thread_cpu_us=" << cpuUs << '\n';
      }
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
