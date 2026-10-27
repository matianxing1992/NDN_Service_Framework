#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/execution-evidence-aggregation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/statistics-file.hpp"

#include <ndn-cxx/util/io.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::test {
namespace {

using EvidenceByRole = std::map<std::string, ExecutionEvidence>;
using AckDecision = ndn_service_framework::ServiceProvider::AckDecision;
using detail::aggregateExecutionEvidence;

boost::property_tree::ptree
ackJson(const AckDecision& decision)
{
  const std::string payload(decision.payload.begin(), decision.payload.end());
  const std::string prefix = "providerCapabilityHint=json64:";
  BOOST_REQUIRE_EQUAL(payload.find(prefix), 0);
  const auto end = payload.find(';', prefix.size());
  BOOST_REQUIRE(end != std::string::npos);
  auto encoded = payload.substr(prefix.size(), end - prefix.size());
  std::replace(encoded.begin(), encoded.end(), '-', '+');
  std::replace(encoded.begin(), encoded.end(), '_', '/');
  while (encoded.size() % 4 != 0) encoded.push_back('=');
  std::istringstream source(encoded);
  const auto decoded = ndn::io::loadBuffer(source);
  BOOST_REQUIRE(decoded);
  std::istringstream json(std::string(decoded->begin(), decoded->end()));
  boost::property_tree::ptree result;
  boost::property_tree::read_json(json, result);
  return result.get_child("servicePayload");
}

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
  ExecutionEvidence::NodeProviderAssignments assignments;
  assignments.reserve(nodes);
  for (std::size_t i = 0; i < nodes; ++i) {
    assignments.push_back(
      {role, "/model/layer/attention/MatMul_" + std::to_string(i),
       "CPUExecutionProvider", true});
  }
  value.setNodeProviderAssignments(assignments);
  value.validate();
  return value;
}

void
mutateAssignments(ExecutionEvidence& value,
                  const std::function<void(ExecutionEvidence::NodeProviderAssignments&)>& mutate)
{
  auto assignments = value.getNodeProviderAssignments();
  mutate(assignments);
  value.setNodeProviderAssignments(assignments);
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

BOOST_AUTO_TEST_CASE(ImmutableInventoryOwnsCallerStorageAndIndependentCopies)
{
  auto value = makeEvidence("/Stage/0", 2);
  const auto original = executionEvidenceToJson(value);
  auto callerRows = value.getNodeProviderAssignments();
  auto& retainedRow = callerRows.front();
  value.setNodeProviderAssignments(std::move(callerRows));
  BOOST_REQUIRE_EQUAL(callerRows.size(), 2);
  retainedRow.nodeName = "changed-through-retained-reference";
  callerRows.back().provider = "changed-through-caller-vector";
  BOOST_CHECK_EQUAL(executionEvidenceToJson(value), original);

  auto previous = value;
  value.setNodeProviderAssignments({});
  bindExecutionObservation(value, "/request/new", 7, false);
  BOOST_CHECK(value.getNodeProviderAssignments().empty());
  BOOST_CHECK_EQUAL(previous.requestId, "/request/old");
  BOOST_CHECK_EQUAL(executionEvidenceToJson(previous), original);
  auto moved = std::move(previous);
  BOOST_CHECK(previous.getNodeProviderAssignments().empty());
  BOOST_CHECK_EQUAL(executionEvidenceToJson(moved), original);

  std::optional<ExecutionEvidence> surviving;
  {
    auto owner = makeEvidence("/Stage/0", 2);
    surviving = owner;
    owner.setNodeProviderAssignments({});
  }
  BOOST_REQUIRE(surviving.has_value());
  BOOST_CHECK_EQUAL(executionEvidenceToJson(*surviving), original);
}

BOOST_AUTO_TEST_CASE(ImmutableInventoryConcatenatesOrderedBorrowedGroups)
{
  auto value = makeEvidence("/Stage/0", 2);
  const auto previous = value;
  auto outsideRows = value.getNodeProviderAssignments();
  outsideRows.front().nodeName = "external-first";
  outsideRows.back().nodeName = "external-second";
  ExecutionEvidence empty;
  BOOST_CHECK(empty.getNodeProviderAssignments().empty());
  const auto& ownRows = value.getNodeProviderAssignments();
  value.concatenateNodeProviderAssignments({std::cref(ownRows), std::cref(outsideRows),
    std::cref(ownRows), std::cref(empty.getNodeProviderAssignments())});

  auto expectedRows = previous.getNodeProviderAssignments();
  expectedRows.insert(expectedRows.end(), outsideRows.begin(), outsideRows.end());
  const auto& repeated = previous.getNodeProviderAssignments();
  expectedRows.insert(expectedRows.end(), repeated.begin(), repeated.end());
  auto expected = previous;
  expected.setNodeProviderAssignments(expectedRows);
  const auto wire = executionEvidenceToJson(expected);
  BOOST_REQUIRE_EQUAL(value.getNodeProviderAssignments().size(), 6);
  BOOST_CHECK_EQUAL(executionEvidenceToJson(value), wire);
  outsideRows.front().nodeName = "caller-mutated-after-concatenation";
  outsideRows.clear();
  BOOST_CHECK_EQUAL(executionEvidenceToJson(value), wire);
  const auto retained = value;
  value.concatenateNodeProviderAssignments({});
  BOOST_CHECK(value.getNodeProviderAssignments().empty());
  BOOST_CHECK_EQUAL(executionEvidenceToJson(retained), wire);

  auto onlyOwner = makeEvidence("/Stage/0", 2);
  const auto before = executionEvidenceToJson(onlyOwner);
  onlyOwner.concatenateNodeProviderAssignments({
    std::cref(onlyOwner.getNodeProviderAssignments())});
  BOOST_CHECK_EQUAL(executionEvidenceToJson(onlyOwner), before);
}

BOOST_AUTO_TEST_CASE(RequestStatisticsPreserveTimingIdentityAndIntegerPrecision)
{
  NativeRequestStatistics statistics;
  statistics.requestId = "/request/outer";
  statistics.admittedModelDigest = "sha256:admitted-model";
  statistics.admittedArtifactDigest = "sha256:admitted-artifact";
  statistics.failureReason = "failed\nwith quoted \"detail\"";
  statistics.startedSteadyNs = 9007199254740993LL;
  NativeEpochCoordinatorResult::RuntimeMetricsObservation observation;
  observation.role = "/Stage/0";
  observation.inferenceEpoch = 7;
  observation.metrics.stateInputHits = 19;
  NativeBackendExecutionTiming timing;
  auto identity = std::make_shared<NativeBackendExecutionIdentity>();
  identity->requestId = "/request/sample";
  identity->sessionId = "/session/sample";
  identity->role = observation.role;
  identity->providerBootId = "boot";
  identity->attemptEpoch = 3;
  timing.identity = identity;
  timing.inferenceEpoch = 7;
  timing.startedSteadyNs = statistics.startedSteadyNs;
  timing.finishedSteadyNs = timing.startedSteadyNs + 17;
  timing.completed = true;
  observation.metrics.lastBackendExecution = timing;
  statistics.runtimeMetrics.push_back(observation);
  statistics.runtimeMetrics.push_back({"/Stage/missing", 8, {}});
  NativeEpochCoordinatorResult::CacheObservation cache;
  cache.inferenceEpoch = 7;
  cache.actualNewInputExtent = 1;
  cache.representedPrefixTokenCount = 128;
  cache.prefixWorkAvoided = 127;
  cache.decodeStateHit = true;
  statistics.cacheObservations.push_back(cache);
  const auto row = nativeRequestStatisticsToJson(statistics);
  BOOST_CHECK_EQUAL(row.find_first_of("\r\n"), std::string::npos);
  const auto decoded = NativeJson::parse(row);
  BOOST_CHECK_EQUAL(decoded.at("admittedModelDigest").get<std::string>(), statistics.admittedModelDigest);
  BOOST_CHECK_EQUAL(decoded.at("admittedArtifactDigest").get<std::string>(), statistics.admittedArtifactDigest);
  BOOST_CHECK_EQUAL(decoded.at("startedSteadyNs").get<std::int64_t>(),
                    statistics.startedSteadyNs);
  BOOST_CHECK_EQUAL(decoded.at("failureReason").get<std::string>(), statistics.failureReason);
  BOOST_CHECK(decoded.at("startedProcessCpuNs").is_null());
  const auto& sample = decoded.at("runtimeMetrics").at(0);
  BOOST_CHECK_EQUAL(sample.at("stateInputHits").get<unsigned>(), 19U);
  BOOST_CHECK_EQUAL(sample.at("lastBackendExecution").at("requestId").get<std::string>(),
                    "/request/sample");
  BOOST_CHECK_EQUAL(sample.at("lastBackendExecution").at("finishedSteadyNs").get<std::int64_t>(),
                    timing.finishedSteadyNs);
  BOOST_CHECK(sample.at("lastBackendExecution").at("startedThreadCpuNs").is_null());
  BOOST_CHECK(decoded.at("runtimeMetrics").at(1).at("lastBackendExecution").is_null());
  BOOST_CHECK_EQUAL(decoded.at("counterSemantics").get<std::string>(),
                    "cumulative-adapter-snapshots");
  BOOST_CHECK_EQUAL(decoded.at("cacheObservations").at(0).at("prefixWorkAvoided").get<unsigned>(),
                    127U);
  BOOST_CHECK(decoded.at("finalExecutionEvidence").is_null());
  auto evidence = makeEvidence();
  evidence.requestId = statistics.requestId;
  evidence.attemptEpoch = 3;
  statistics.finalExecutionEvidence = evidence;
  const auto retained = NativeJson::parse(nativeRequestStatisticsToJson(statistics));
  const auto roundTrip = executionEvidenceFromJson(retained.at("finalExecutionEvidence").dump());
  BOOST_CHECK_EQUAL(roundTrip.requestId, statistics.requestId);
  BOOST_CHECK_EQUAL(roundTrip.attemptEpoch, 3U);
  BOOST_CHECK_EQUAL(roundTrip.gpuUuid, evidence.gpuUuid);
  BOOST_CHECK_EQUAL(roundTrip.cpuFallbackUsed, evidence.cpuFallbackUsed);
  BOOST_CHECK_EQUAL(roundTrip.executionCompleted, evidence.executionCompleted);
}

BOOST_AUTO_TEST_CASE(StatisticsFilePreservesCompleteRowsAndReportsWriteFailures)
{
  struct TemporaryDirectory
  {
    std::filesystem::path path;
    TemporaryDirectory()
    {
      auto pattern = (std::filesystem::temp_directory_path() /
                      "ndnsf-statistics-XXXXXX").string();
      const auto created = ::mkdtemp(pattern.data());
      if (!created) throw std::runtime_error("cannot create statistics fixture directory");
      path = created;
    }
    ~TemporaryDirectory()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } temporary;
  const auto path = (temporary.path / "statistics.jsonl").string();
  {
    detail::StatisticsFile output(path);
    BOOST_CHECK_THROW(output.write(""), std::invalid_argument);
    BOOST_CHECK_THROW(output.write("{}\n{}"), std::invalid_argument);
    BOOST_CHECK_THROW(output.write("{}\r{}"), std::invalid_argument);
    std::vector<std::future<void>> writers;
    for (unsigned writer = 0; writer < 4; ++writer) {
      writers.push_back(std::async(std::launch::async, [&, writer] {
        for (unsigned row = 0; row < 8; ++row)
          output.write(NativeJson{{"id", writer * 8 + row}}.dump());
      }));
    }
    for (auto& writer : writers) writer.get();
    // Flush makes complete rows readable before the owned sink is destroyed.
    std::ifstream input(path);
    std::set<unsigned> ids;
    std::string line;
    while (std::getline(input, line)) ids.insert(NativeJson::parse(line).at("id").get<unsigned>());
    BOOST_REQUIRE_EQUAL(ids.size(), 32U);
  }
  {
    detail::StatisticsFile output(path);
    output.write(NativeJson{{"id", 32}}.dump());
  }
  std::ifstream input(path);
  unsigned rows = 0;
  std::string line;
  while (std::getline(input, line)) {
    BOOST_CHECK(NativeJson::parse(line).contains("id"));
    ++rows;
  }
  BOOST_CHECK_EQUAL(rows, 33U);
  BOOST_CHECK_THROW(detail::StatisticsFile{temporary.path.string()}, std::runtime_error);
  if (std::filesystem::exists("/dev/full")) {
    detail::StatisticsFile full("/dev/full");
    BOOST_CHECK_THROW(full.write("{}"), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(SingleObservationPreservesFieldsAndOwnsInventory)
{
  auto value = makeEvidence();
  value.roles = {"/Stage/1", "/Stage/0", "/Stage/1"};
  const auto output = aggregateExecutionEvidence(EvidenceByRole{{"a", value}});
  const auto borrowed = aggregateExecutionEvidence(
    std::vector<std::reference_wrapper<const ExecutionEvidence>>{std::cref(value)});
  BOOST_CHECK_EQUAL(executionEvidenceToJson(borrowed), executionEvidenceToJson(output));
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
  BOOST_REQUIRE_EQUAL(output.getNodeProviderAssignments().size(), 1);
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[0].nodeName,
                    "/model/layer/attention/MatMul_0");
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[0].provider, "CPUExecutionProvider");
  BOOST_CHECK(output.getNodeProviderAssignments()[0].modelNode);
  BOOST_CHECK_EQUAL(executionEvidenceToJson(output), executionEvidenceToJson(
    aggregateExecutionEvidence(std::vector<ExecutionEvidence>{value})));
  mutateAssignments(value, [] (auto& assignments) { assignments[0].nodeName = "mutated"; });
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[0].nodeName,
                    "/model/layer/attention/MatMul_0");
  BOOST_CHECK_EQUAL(borrowed.getNodeProviderAssignments()[0].nodeName,
                    "/model/layer/attention/MatMul_0");
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
  BOOST_REQUIRE_EQUAL(output.getNodeProviderAssignments().size(), 2);
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[0].role, "/Stage/1");
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[1].role, "/Stage/0");
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
  BOOST_CHECK_THROW(aggregateExecutionEvidence(
    std::vector<std::reference_wrapper<const ExecutionEvidence>>{}), std::runtime_error);
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
  BOOST_REQUIRE_EQUAL(output.getNodeProviderAssignments().size(), 10864);
  BOOST_REQUIRE_EQUAL(output.roles.size(), 1);
  BOOST_CHECK_EQUAL(output.roles[0], "/Stage/0");
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[5431].nodeName,
                    "/model/layer/attention/MatMul_5431");
  BOOST_CHECK_EQUAL(output.getNodeProviderAssignments()[5432].nodeName,
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
  mutateAssignments(items.begin()->second,
                    [] (auto& assignments) { assignments[0].nodeName = "updated-node"; });
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

BOOST_AUTO_TEST_CASE(AtomicUpdatePreservesSummaryAndLegacyAck)
{
  auto value = makeEvidence("/Stage/0", 2);
  NativeProviderReadinessState state, legacy;
  state.markReady("ready");
  legacy.markReady("ready");
  const auto summary = state.updateExecutionEvidence(value);
  BOOST_CHECK_EQUAL(summary,
    "NDNSF_DI_EXECUTION_EVIDENCE_UPDATE provider=/provider/A evidenceEpoch=9"
    " runnerKind=onnxruntime-cpu realCompute=true loadCompleted=true warmupCompleted=true"
    " executionCompleted=true exactForwardCacheHit=false roles=1 artifacts=1"
    " nodeProviderAssignments=2 requestId=/request/old attemptEpoch=3 profileCaptured=true"
    " profileRequestId=/request/profile profileAttemptEpoch=2");
  updateReadiness(legacy, {{"/Stage/0", value}}, false);
  const auto before = state.makeAckDecision("/Stage/0");
  BOOST_REQUIRE(before.status);
  BOOST_CHECK(before.payload == legacy.makeAckDecision("/Stage/0").payload);
  const auto original = ackJson(before);
  BOOST_CHECK_EQUAL(original.get<std::string>("executionEvidence.requestId"), "/request/old");
  BOOST_CHECK_EQUAL(original.get_child("executionEvidence.nodeProviderAssignments").size(), 2);
  BOOST_CHECK_EQUAL(original.get_child("executionEvidenceByRole").size(), 1);
  value.requestId = "/request/new";
  value.evidenceEpoch = 10;
  mutateAssignments(value, [] (auto& assignments) { assignments[0].nodeName = "new-node"; });
  BOOST_CHECK(before.payload == state.makeAckDecision("/Stage/0").payload);
  state.updateExecutionEvidence(value);
  updateReadiness(legacy, {{"/Stage/0", value}}, false);
  const auto after = state.makeAckDecision("/Stage/0");
  BOOST_CHECK(before.payload != after.payload);
  BOOST_CHECK(after.payload == legacy.makeAckDecision("/Stage/0").payload);
  const auto updated = ackJson(after);
  BOOST_CHECK_EQUAL(updated.get<std::string>("executionEvidence.requestId"), "/request/new");
  BOOST_CHECK_EQUAL(updated.get<unsigned>("executionEvidence.evidenceEpoch"), 10);
  const auto& byRole = updated.get_child("executionEvidenceByRole").get_child("/Stage/0");
  BOOST_CHECK_EQUAL(byRole.get<unsigned>("evidenceEpoch"), 10);
  BOOST_CHECK_EQUAL(byRole.get_child("nodeProviderAssignments").front().second.get<std::string>(
                      "nodeName"), "new-node");
}

BOOST_AUTO_TEST_CASE(AtomicUpdateRejectsInvalidAndInconsistentWithoutMutation)
{
  NativeProviderReadinessState state;
  state.markReady("ready");
  state.updateExecutionEvidence(makeEvidence());
  const auto before = state.makeAckDecision("/Stage/0");
  const std::vector<std::function<void(ExecutionEvidence&)>> mutations{
    [](auto& item) { item.providerName = "/foreign"; },
    [](auto& item) { item.providerBootId = "other-boot"; },
    [](auto& item) { item.runnerKind = RunnerKind::WiringOnly; },
    [](auto& item) { item.realCompute = false; },
    [](auto& item) { item.modelDigest = "other-model"; },
    [](auto& item) { item.planDigest = "other-plan"; },
    [](auto& item) { item.runtimeVersion = "other-runtime"; },
    [](auto& item) { item.deviceKind = "cuda"; },
    [](auto& item) { item.cpuFallbackUsed = true; },
    [](auto& item) { item.roles.clear(); },
    [](auto& item) { item.artifactDigests.clear(); },
    [](auto& item) { item.requestId.clear(); },
  };
  for (const auto& mutate : mutations) {
    auto invalid = makeEvidence("/Stage/1");
    mutate(invalid);
    BOOST_CHECK_THROW(state.updateExecutionEvidence(invalid), std::exception);
    BOOST_CHECK(before.payload == state.makeAckDecision("/Stage/0").payload);
  }
  auto invalid = makeEvidence();
  invalid.planDigest.clear();
  BOOST_CHECK_THROW(state.setExecutionEvidenceByRole({{"/Stage/0", invalid}}),
                    std::invalid_argument);
  BOOST_CHECK(before.payload == state.makeAckDecision("/Stage/0").payload);

  // Legacy setters still replace their respective observation independently.
  auto replacement = makeEvidence();
  replacement.requestId = "/aggregate-only";
  state.setExecutionEvidence(replacement);
  auto json = ackJson(state.makeAckDecision("/Stage/0"));
  BOOST_CHECK_EQUAL(json.get<std::string>("executionEvidence.requestId"), "/aggregate-only");
  BOOST_CHECK_EQUAL(json.get_child("executionEvidenceByRole").get_child("/Stage/0")
                      .get<std::string>("requestId"), "/request/old");
  replacement.requestId = "/role-only";
  state.setExecutionEvidenceByRole({{"/Stage/0", replacement}});
  json = ackJson(state.makeAckDecision("/Stage/0"));
  BOOST_CHECK_EQUAL(json.get<std::string>("executionEvidence.requestId"), "/aggregate-only");
  BOOST_CHECK_EQUAL(json.get_child("executionEvidenceByRole").get_child("/Stage/0")
                      .get<std::string>("requestId"), "/role-only");
}

BOOST_AUTO_TEST_CASE(AtomicUpdateOwnsLargeDuplicatedRoleObservations)
{
  NativeProviderReadinessState state;
  state.markReady("ready");
  std::string summary;
  {
    auto value = makeEvidence("/Stage/0", 5432);
    value.roles = {"/Stage/0", "/Stage/1", "/Stage/0"};
    summary = state.updateExecutionEvidence(value);
    value.setNodeProviderAssignments({});
    value.requestId = "mutated-after-update";
  }
  BOOST_CHECK(summary.find("roles=2 artifacts=1 nodeProviderAssignments=10864") !=
              std::string::npos);
  auto json = ackJson(state.makeAckDecision("/Stage/0,/Stage/1"));
  const auto& evidence = json.get_child("executionEvidence");
  BOOST_CHECK_EQUAL(evidence.get_child("nodeProviderAssignments").size(), 10864);
  BOOST_CHECK_EQUAL(evidence.get_child("roles").size(), 2);
  BOOST_CHECK(!evidence.get<bool>("executionCompleted"));
  BOOST_CHECK(evidence.get<std::string>("requestId").empty());
  const auto& roles = json.get_child("executionEvidenceByRole");
  BOOST_CHECK_EQUAL(roles.size(), 2);
  for (const auto& role : roles) {
    BOOST_CHECK_EQUAL(role.second.get_child("nodeProviderAssignments").size(), 5432);
    BOOST_CHECK_EQUAL(role.second.get<std::string>("requestId"), "/request/old");
  }
  state.updateExecutionEvidence(makeEvidence("/Stage/0", 5568));
  json = ackJson(state.makeAckDecision("/Stage/0,/Stage/1"));
  BOOST_CHECK_EQUAL(json.get_child("executionEvidence.nodeProviderAssignments").size(), 11000);
  const auto& after = json.get_child("executionEvidenceByRole");
  BOOST_CHECK_EQUAL(after.get_child("/Stage/0").get_child("nodeProviderAssignments").size(), 5568);
  BOOST_CHECK_EQUAL(after.get_child("/Stage/1").get_child("nodeProviderAssignments").size(), 5432);
}

BOOST_AUTO_TEST_CASE(InFlightAckKeepsMatchingImmutablePairAndFactorySemantics)
{
  NativeProviderReadinessState state;
  state.markReady("ready");
  state.updateExecutionEvidence(makeEvidence());
  const auto before = state.makeAckDecision("/Stage/0");
  std::promise<void> captured, release;
  auto capturedFuture = captured.get_future();
  auto releaseFuture = release.get_future();
  auto pending = std::async(std::launch::async, [&] {
    return state.makeAckDecision("/Stage/0", {}, {}, [&] (const AckDecision&)
      -> std::optional<AckDecision> {
      captured.set_value();
      if (releaseFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        throw std::runtime_error("bounded ACK snapshot test release timed out");
      }
      return std::nullopt;
    });
  });
  if (capturedFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    release.set_value();
    pending.wait();
    BOOST_FAIL("ACK did not capture its snapshot within the test deadline");
  }
  auto replacement = makeEvidence();
  replacement.requestId = "/request/concurrent";
  replacement.evidenceEpoch = 10;
  try {
    state.updateExecutionEvidence(replacement);
  }
  catch (...) {
    release.set_value();
    pending.wait();
    throw;
  }
  release.set_value();
  const auto retained = pending.get();
  BOOST_CHECK(before.payload == retained.payload);
  auto current = state.makeAckDecision("/Stage/0");
  BOOST_CHECK(before.payload != current.payload);
  const auto json = ackJson(current);
  BOOST_CHECK_EQUAL(json.get<unsigned>("executionEvidence.evidenceEpoch"), 10);
  BOOST_CHECK_EQUAL(json.get_child("executionEvidenceByRole").get_child("/Stage/0")
                      .get<unsigned>("evidenceEpoch"), 10);
  unsigned calls = 0;
  const auto rejected = state.makeAckDecision("/Stage/0", {}, {},
    [&] (const AckDecision& value) -> std::optional<AckDecision> {
      ++calls;
      auto result = value;
      result.status = false;
      result.payload = {'r'};
      return result;
    });
  BOOST_CHECK_EQUAL(calls, 1);
  BOOST_CHECK(!rejected.status);
  BOOST_CHECK(rejected.payload == ndn::Buffer{'r'});
  BOOST_CHECK_THROW(state.makeAckDecision("/Stage/0", {}, {},
    [] (const AckDecision&) -> std::optional<AckDecision> {
      throw std::runtime_error("factory failure");
    }), std::runtime_error);
  BOOST_CHECK(current.payload == state.makeAckDecision("/Stage/0").payload);
}

BOOST_AUTO_TEST_CASE(SingleRoleAtomicUpdatePreservesRawAndNormalizedEvidence)
{
  std::string firstSummary;
  for (unsigned variant = 0; variant < 13; ++variant) {
    auto value = makeEvidence("/Stage/0", 3);
    value.deviceIds = {value.deviceId};
    switch (variant) {
      case 1: value.deviceIds.clear(); break;
      case 2: value.deviceIds.push_back(value.deviceId); break;
      case 3: value.deviceIds = {""}; break;
      case 4: value.deviceId = "stale-device"; break;
      case 5: value.gpuUuid = "GPU-a"; break;
      case 6: value.gpuUuid = "GPU-a"; value.gpuUuids = {"GPU-a"}; break;
      case 7: value.gpuUuid = "GPU-a"; value.gpuUuids = {"GPU-a", "GPU-a"}; break;
      case 8: value.gpuUuid = "GPU-a"; value.gpuUuids = {""}; break;
      case 9: value.gpuUuid = "stale-gpu"; value.gpuUuids = {"GPU-a"}; break;
      case 10: value.roles.push_back("/Stage/0"); break;
      case 11:
        value.deviceId = "multi";
        value.deviceIds = {"cpu-0", "cpu-1"};
        value.gpuUuid = "multi";
        value.gpuUuids = {"GPU-a", "GPU-b"};
        break;
      case 12: value.deviceId.clear(); value.deviceIds.clear(); break;
      default: break;
    }
    NativeProviderReadinessState state, legacy;
    state.markReady("ready");
    legacy.markReady("ready");
    const auto summary = state.updateExecutionEvidence(value);
    if (variant == 0) {
      firstSummary = summary;
    }
    BOOST_CHECK_EQUAL(summary, firstSummary);
    updateReadiness(legacy, {{"/Stage/0", value}}, false);
    const auto expected = legacy.makeAckDecision("/Stage/0");
    const auto observed = state.makeAckDecision("/Stage/0");
    BOOST_REQUIRE(expected.status);
    BOOST_REQUIRE(observed.status);
    BOOST_CHECK_MESSAGE(observed.payload == expected.payload, "variant=" << variant);
    value.setNodeProviderAssignments({});
    value.requestId = "/request/caller-mutated";
    BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == expected.payload);
  }
}

BOOST_AUTO_TEST_CASE(RepeatedObservationPreservesEveryFieldAndOrderedInventory)
{
  using Mutation = std::pair<std::string, std::function<void(ExecutionEvidence&)>>;
  const std::vector<Mutation> mutations{
    {"schema", [](auto& v) { v.schema = "unsupported"; }},
    {"providerName", [](auto& v) { v.providerName += "/new"; }},
    {"providerBootId", [](auto& v) { v.providerBootId += "-new"; }},
    {"evidenceEpoch", [](auto& v) { ++v.evidenceEpoch; }},
    {"runnerKind", [](auto& v) { v.runnerKind = RunnerKind::OnnxRuntimeCuda; }},
    {"realCompute", [](auto& v) { v.realCompute = false; }},
    {"deviceKind", [](auto& v) { v.deviceKind = "cuda"; }},
    {"deviceId", [](auto& v) { v.deviceId = "cpu-1"; }},
    {"deviceIds", [](auto& v) { v.deviceIds.push_back("cpu-1"); }},
    {"runtimeVersion", [](auto& v) { v.runtimeVersion += "-new"; }},
    {"modelDigest", [](auto& v) { v.modelDigest += "-new"; }},
    {"planDigest", [](auto& v) { v.planDigest += "-new"; }},
    {"artifactDigests", [](auto& v) { v.artifactDigests.begin()->second += "-new"; }},
    {"roles", [](auto& v) { v.roles.push_back("/Stage/0"); }},
    {"nodeProviderAssignments", [](auto& v) { v.setNodeProviderAssignments({}); }},
    {"cpuFallbackUsed", [](auto& v) { v.cpuFallbackUsed = true; }},
    {"loadCompleted", [](auto& v) { v.loadCompleted = false; }},
    {"warmupCompleted", [](auto& v) { v.warmupCompleted = false; }},
    {"gpuUuid", [](auto& v) { v.gpuUuid = "GPU-new"; }},
    {"gpuUuids", [](auto& v) { v.gpuUuids = {"GPU-new"}; }},
    {"providerProfilePath", [](auto& v) { v.providerProfilePath += "-new"; }},
    {"createdAtMs", [](auto& v) { ++v.createdAtMs; }},
    {"processId", [](auto& v) { ++v.processId; }},
    {"cudaVisibleDevices", [](auto& v) { v.cudaVisibleDevices = "1"; }},
    {"gpuIdentitySource", [](auto& v) { v.gpuIdentitySource = "new-source"; }},
    {"requestId", [](auto& v) { v.requestId += "/new"; }},
    {"attemptEpoch", [](auto& v) { ++v.attemptEpoch; }},
    {"executionCompleted", [](auto& v) { v.executionCompleted = false; }},
    {"exactForwardCacheHit", [](auto& v) { v.exactForwardCacheHit = true; }},
    {"profileRequestId", [](auto& v) { v.profileRequestId += "/new"; }},
    {"profileAttemptEpoch", [](auto& v) { ++v.profileAttemptEpoch; }},
    {"node.role", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) { rows[0].role = "/Stage/new"; });
     }},
    {"node.nodeName", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) { rows[0].nodeName += "/new"; });
     }},
    {"node.provider", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) { rows[0].provider = "CUDA"; });
     }},
    {"node.modelNode", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) { rows[0].modelNode = false; });
     }},
    {"node.order", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) { std::swap(rows[0], rows[1]); });
     }},
    {"node.duplicate", [](auto& v) {
       mutateAssignments(v, [] (auto& rows) {
         const auto first = rows.front();
         rows.push_back(first);
       });
     }},
  };
  for (const auto& mutation : mutations) {
    BOOST_TEST_CONTEXT(mutation.first) {
      auto value = makeEvidence("/Stage/0", 3);
      value.deviceIds = {value.deviceId};
      if (mutation.first == "loadCompleted") {
        value.warmupCompleted = false;
      }
      if (mutation.first == "exactForwardCacheHit") {
        value.executionCompleted = false;
      }
      NativeProviderReadinessState state;
      state.markReady("ready");
      const auto summary = state.updateExecutionEvidence(value);
      const auto before = state.makeAckDecision("/Stage/0");
      BOOST_REQUIRE(before.status);
      for (unsigned i = 0; i < 3; ++i) {
        BOOST_CHECK_EQUAL(state.updateExecutionEvidence(value), summary);
        BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == before.payload);
      }
      auto changed = value;
      mutation.second(changed);
      NativeProviderReadinessState reference;
      reference.markReady("ready");
      bool accepted = true;
      try {
        updateReadiness(reference, {{"/Stage/0", changed}}, false);
      }
      catch (const std::exception&) {
        accepted = false;
      }
      if (!accepted) {
        BOOST_CHECK_THROW(state.updateExecutionEvidence(changed), std::exception);
        BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == before.payload);
      }
      else {
        state.updateExecutionEvidence(changed);
        const auto expected = reference.makeAckDecision("/Stage/0");
        const auto after = state.makeAckDecision("/Stage/0");
        BOOST_CHECK(after.payload == expected.payload);
        BOOST_CHECK(after.payload != before.payload);
        state.updateExecutionEvidence(changed);
        BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == expected.payload);
        changed.setNodeProviderAssignments({});
        changed.requestId = "/caller-mutated";
        BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == expected.payload);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(RepeatedObservationReconcilesIndependentLegacySetters)
{
  auto value = makeEvidence("/Stage/0", 3);
  value.deviceIds = {value.deviceId};
  for (const bool aggregateOnly : {false, true}) {
    NativeProviderReadinessState state;
    state.markReady("ready");
    const auto summary = state.updateExecutionEvidence(value);
    const auto before = state.makeAckDecision("/Stage/0");
    auto replacement = value;
    replacement.requestId = "/legacy-replacement";
    if (aggregateOnly) {
      state.setExecutionEvidence(std::move(replacement));
    }
    else {
      state.setExecutionEvidenceByRole({{"/Stage/0", replacement}});
    }
    BOOST_CHECK(state.makeAckDecision("/Stage/0").payload != before.payload);
    BOOST_CHECK_EQUAL(state.updateExecutionEvidence(value), summary);
    BOOST_CHECK(state.makeAckDecision("/Stage/0").payload == before.payload);
  }
}

BOOST_AUTO_TEST_CASE(RunnerEvidenceSnapshotCopyProbe)
{
  if (std::getenv("NDNSF_DI_EVIDENCE_COPY_PROBE") == nullptr) {
    BOOST_TEST_MESSAGE("Opt-in runner evidence snapshot probe not requested");
    return;
  }
  constexpr std::size_t CYCLES = 129;
  const std::string requestId = "/request/snapshot-probe";
  for (const auto nodes : {5568, 5432}) {
    const auto value = makeEvidence("/Stage/0", nodes);
    const auto original = executionEvidenceToJson(value);
    std::shared_ptr<NativeModelRunner> runner = std::make_shared<LambdaModelRunner>(
      [] (const RoleExecutionContext&) {
        return std::map<std::string, TensorBundle>{};
      }, value);
    auto expected = value;
    bindExecutionObservation(expected, requestId, 7, false);
    const auto expectedJson = executionEvidenceToJson(expected);
    for (unsigned repetition = 0; repetition < 3; ++repetition) {
      std::size_t totalNodes = 0;
      std::size_t admitted = 0;
      const auto began = std::chrono::steady_clock::now();
      const auto cpu = threadCpuNs();
      const auto processCpu = std::clock();
      for (std::size_t i = 0; i < CYCLES; ++i) {
        auto snapshot = runner->executionEvidenceSnapshot();
        if (snapshot) {
          bindExecutionObservation(*snapshot, requestId, 7, false);
          totalNodes += snapshot->getNodeProviderAssignments().size();
          admitted += snapshot->requestId == requestId && snapshot->attemptEpoch == 7 &&
                      snapshot->executionCompleted && !snapshot->exactForwardCacheHit;
        }
      }
      const auto processFinished = std::clock();
      const auto cpuNs = threadCpuNs() - cpu;
      const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - began).count();
      BOOST_REQUIRE(processCpu != std::clock_t(-1) && processFinished != std::clock_t(-1));
      BOOST_CHECK_EQUAL(totalNodes, CYCLES * static_cast<std::size_t>(nodes));
      BOOST_CHECK_EQUAL(admitted, CYCLES);
      auto observed = runner->executionEvidenceSnapshot();
      BOOST_REQUIRE(observed.has_value());
      BOOST_CHECK_EQUAL(executionEvidenceToJson(*observed), original);
      bindExecutionObservation(*observed, requestId, 7, false);
      BOOST_CHECK_EQUAL(executionEvidenceToJson(*observed), expectedJson);
      BOOST_REQUIRE(runner->executionEvidence().has_value());
      BOOST_CHECK_EQUAL(executionEvidenceToJson(*runner->executionEvidence()), original);
      std::cout << "NDNSF_RUNNER_SNAPSHOT_PROBE nodes=" << nodes
                << " repetition=" << repetition << " cycles=" << CYCLES
                << " wall_ns=" << wallNs << " thread_cpu_ns=" << cpuNs
                << " process_cpu_ns=" << static_cast<std::uint64_t>(
                     (processFinished - processCpu) * (1e9 / CLOCKS_PER_SEC)) << '\n';
    }
  }
}

BOOST_AUTO_TEST_CASE(SingleRoleAtomicInventoryCopyProbe)
{
  if (std::getenv("NDNSF_DI_EVIDENCE_COPY_PROBE") == nullptr) {
    BOOST_TEST_MESSAGE("Opt-in atomic inventory copy probe not requested");
    return;
  }
  for (const auto nodes : {5568, 5432}) {
    auto value = makeEvidence("/Stage/0", nodes);
    // The ONNX factory seeds its single device list before warm execution.
    value.deviceIds = {value.deviceId};
    for (unsigned repetition = 0; repetition < 3; ++repetition) {
      NativeProviderReadinessState state;
      state.markReady("ready");
      const auto began = std::chrono::steady_clock::now();
      const auto cpu = threadCpuNs();
      for (unsigned i = 0; i < 129; ++i) {
        state.updateExecutionEvidence(value);
      }
      const auto cpuUs = (threadCpuNs() - cpu) / 1000;
      const auto wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - began).count();
      NativeProviderReadinessState reference;
      reference.markReady("ready");
      updateReadiness(reference, {{"/Stage/0", value}}, false);
      const auto ack = state.makeAckDecision("/Stage/0");
      const auto expected = reference.makeAckDecision("/Stage/0");
      BOOST_REQUIRE(ack.status);
      BOOST_REQUIRE(expected.status);
      BOOST_CHECK(ack.payload == expected.payload);
      std::cout << "NDNSF_SINGLE_EVIDENCE_PROBE nodes=" << nodes
                << " repetition=" << repetition << " cycles=129 wall_us=" << wallUs
                << " thread_cpu_us=" << cpuUs << '\n';
    }
  }
}

BOOST_AUTO_TEST_CASE(MultiRoleAtomicInventoryCopyProbe)
{
  if (std::getenv("NDNSF_DI_EVIDENCE_COPY_PROBE") == nullptr) {
    BOOST_TEST_MESSAGE("Opt-in multi-role inventory probe not requested");
    return;
  }
  constexpr std::size_t CYCLES = 129;
  EvidenceByRole items{{"/Stage/0", makeEvidence("/Stage/0", 5568)},
                       {"/Stage/1", makeEvidence("/Stage/1", 5432)}};
  for (auto& item : items) {
    item.second.deviceIds = {item.second.deviceId};
  }
  for (unsigned repetition = 0; repetition < 3; ++repetition) {
    NativeProviderReadinessState state;
    state.markReady("ready");
    for (const auto& item : items) state.updateExecutionEvidence(item.second);
    const auto before = state.makeAckDecision("/Stage/0,/Stage/1");
    std::string summary;
    const auto began = std::chrono::steady_clock::now();
    const auto cpu = threadCpuNs();
    const auto processCpu = std::clock();
    for (std::size_t i = 0; i < CYCLES; ++i) {
      summary = state.updateExecutionEvidence(items.at(i % 2 == 0 ? "/Stage/0" : "/Stage/1"));
    }
    const auto processFinished = std::clock();
    const auto cpuNs = threadCpuNs() - cpu;
    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - began).count();
    BOOST_REQUIRE(processCpu != std::clock_t(-1) && processFinished != std::clock_t(-1));
    const auto after = state.makeAckDecision("/Stage/0,/Stage/1");
    BOOST_REQUIRE(before.status);
    BOOST_REQUIRE(after.status);
    BOOST_CHECK(before.payload == after.payload);
    BOOST_CHECK(summary.find("roles=2 artifacts=2 nodeProviderAssignments=11000") !=
                std::string::npos);
    NativeProviderReadinessState reference;
    reference.markReady("ready");
    updateReadiness(reference, items, false);
    BOOST_CHECK(reference.makeAckDecision("/Stage/0,/Stage/1").payload == after.payload);
    std::cout << "NDNSF_MULTI_EVIDENCE_PROBE repetition=" << repetition
              << " cycles=" << CYCLES << " nodes=11000 wall_ns=" << wallNs
              << " thread_cpu_ns=" << cpuNs << " process_cpu_ns="
              << static_cast<std::uint64_t>(
                   (processFinished - processCpu) * (1e9 / CLOCKS_PER_SEC)) << '\n';
  }
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
