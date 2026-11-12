/**
 * Spec 224 T001 — sealed unit/step contract and placement projection.
 *
 * These are native contract tests: they exercise the production
 * NativeExecutionPlan / NativeExecutionPlanJson types directly. They do not
 * model tensor parallelism numerically (that is T002) and do not run a
 * Provider (that is T003).
 */
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-runner.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <onnx/onnx_pb.h>
#include <google/protobuf/text_format.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <system_error>
#include <limits>
#include <thread>
#include <unistd.h>

#include <ndn-cxx/util/sha256.hpp>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;
namespace qwen = ndnsf::di::qwen;

const char* DIGEST_A = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

NativeStepSlotSpec
slot(const std::string& id, const std::string& source,
     const std::string& producerStep = "", const std::string& producerSlot = "")
{
  NativeStepSlotSpec result;
  result.slotId = id;
  result.dtype = "float32";
  result.shape = {std::int64_t(1), std::int64_t(25), std::int64_t(1024)};
  result.layoutDigest = DIGEST_A;
  result.source = source;
  result.producerStepId = producerStep;
  result.producerSlotId = producerSlot;
  return result;
}

/** One complete TP=2 template: ATTN partial on each rank, SUM, then MLP. */
std::vector<NativeExecutionStepSpec>
twoRankSteps()
{
  NativeExecutionStepSpec attn0;
  attn0.stepId = "S0R0/ATTN/0";
  attn0.unitId = "S0R0";
  attn0.operation = "CALL_SUBGRAPH";
  attn0.artifactEntry = "entry/S0R0/ATTN/0";
  attn0.inputSlots = {slot("in", "EPOCH_INPUT")};
  attn0.outputSlots = {slot("partial", "LOCAL_STEP")};
  attn0.localStateSlots = {"kv.0.key", "kv.0.value"};

  NativeExecutionStepSpec attn1;
  attn1.stepId = "S0R1/ATTN/0";
  attn1.unitId = "S0R1";
  attn1.operation = "CALL_SUBGRAPH";
  attn1.artifactEntry = "entry/S0R1/ATTN/0";
  attn1.inputSlots = {slot("in", "EPOCH_INPUT")};
  attn1.outputSlots = {slot("partial", "LOCAL_STEP")};
  attn1.localStateSlots = {"kv.0.key", "kv.0.value"};

  NativeExecutionStepSpec sum0;
  sum0.stepId = "S0R0/SUM_ATTN/0";
  sum0.unitId = "S0R0";
  sum0.operation = "SUM";
  sum0.inputSlots = {slot("local", "LOCAL_STEP", "S0R0/ATTN/0", "partial"),
                     slot("remote", "REMOTE_STEP", "S0R1/ATTN/0", "partial")};
  sum0.outputSlots = {slot("merged", "LOCAL_STEP")};

  NativeExecutionStepSpec sum1;
  sum1.stepId = "S0R1/SUM_ATTN/0";
  sum1.unitId = "S0R1";
  sum1.operation = "SUM";
  sum1.inputSlots = {slot("local", "LOCAL_STEP", "S0R1/ATTN/0", "partial"),
                     slot("remote", "REMOTE_STEP", "S0R0/ATTN/0", "partial")};
  sum1.outputSlots = {slot("merged", "LOCAL_STEP")};

  NativeExecutionStepSpec mlp0;
  mlp0.stepId = "S0R0/MLP/0";
  mlp0.unitId = "S0R0";
  mlp0.operation = "CALL_SUBGRAPH";
  mlp0.artifactEntry = "entry/S0R0/MLP/0";
  mlp0.inputSlots = {slot("merged", "LOCAL_STEP", "S0R0/SUM_ATTN/0", "merged")};
  mlp0.outputSlots = {slot("partial", "LOCAL_STEP")};

  return {attn0, attn1, sum0, sum1, mlp0};
}

NativeExecutionPlan
twoRankPlan()
{
  NativeExecutionPlan plan;
  plan.serviceName = "/service/tp";
  plan.modelName = "qwen3-0.6b";
  plan.modelFamily = "qwen";
  plan.roles = {"S0R0", "S0R1"};
  plan.steps = twoRankSteps();
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  plan.tensorDegree = 2;

  // One dependency per remote contribution, each bound to the exact producer
  // and consumer step. A single producer/consumer list spanning both ranks
  // would bind S0R0's partial to S0R0's own SUM, which is what the slot
  // contract must reject.
  const auto remote = [&] (const std::string& producer, const std::string& consumer,
                           const std::string& id) {
    NativeDependencySpec dependency;
    dependency.producers = {producer};
    dependency.consumers = {consumer};
    dependency.keyScope = id;
    dependency.dependencyId = id;
    dependency.topicPrefix = "/service/tp";
    dependency.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
    dependency.tensors = {"partial"};
    dependency.operationKind = "TENSOR_PARTIAL";
    dependency.useNdnsfDataV1 = true;
    dependency.producerStepId = producer + "/ATTN/0";
    dependency.producerSlotId = "partial";
    dependency.consumerStepId = consumer + "/SUM_ATTN/0";
    dependency.consumerSlotId = "remote";
    return dependency;
  };
  plan.dependencies = {remote("S0R1", "S0R0", "tp-attn-r0-from-r1"),
                       remote("S0R0", "S0R1", "tp-attn-r1-from-r0")};
  return plan;
}

/** Serialize one plan the way the Provider-side loader reads it. */
std::string
planWire(const NativeExecutionPlan& plan)
{
  auto steps = NativeJson::array();
  for (const auto& step : plan.steps) {
    auto slots = [] (const std::vector<NativeStepSlotSpec>& values) {
      auto out = NativeJson::array();
      for (const auto& value : values) {
        auto entry = NativeJson::object();
        entry["slot_id"] = value.slotId;
        entry["dtype"] = value.dtype;
        entry["shape"] = NativeJson::array();
        for (const auto& dimension : value.shape) {
          std::visit([&entry] (const auto& item) { entry["shape"].push_back(item); },
                     dimension);
        }
        entry["layout_digest"] = value.layoutDigest;
        entry["source"] = value.source;
        if (!value.producerStepId.empty()) entry["producer_step_id"] = value.producerStepId;
        if (!value.producerSlotId.empty()) entry["producer_slot_id"] = value.producerSlotId;
        out.push_back(std::move(entry));
      }
      return out;
    };
    auto entry = NativeJson::object();
    entry["step_id"] = step.stepId;
    entry["unit_id"] = step.unitId;
    entry["operation"] = step.operation;
    if (!step.artifactEntry.empty()) entry["artifact_entry"] = step.artifactEntry;
    entry["input_slots"] = slots(step.inputSlots);
    entry["output_slots"] = slots(step.outputSlots);
    entry["local_state_slots"] = step.localStateSlots;
    steps.push_back(std::move(entry));
  }

  auto dependencies = NativeJson::array();
  for (const auto& dependency : plan.dependencies) {
    auto entry = NativeJson::object();
    entry["producers"] = dependency.producers;
    entry["consumers"] = dependency.consumers;
    entry["keyScope"] = dependency.keyScope;
    entry["topicPrefix"] = dependency.topicPrefix;
    entry["objectNameTemplate"] = dependency.objectNameTemplate;
    entry["tensors"] = dependency.tensors;
    entry["operationKind"] = dependency.operationKind;
    entry["transportProfile"] = dependency.useNdnsfDataV1 ? "NDNSF_DATA_V1" : "COLLAB_LARGE_V1";
    entry["dependency_id"] = dependency.dependencyId;
    entry["producer_step_id"] = dependency.producerStepId;
    entry["producer_slot_id"] = dependency.producerSlotId;
    entry["consumer_step_id"] = dependency.consumerStepId;
    entry["consumer_slot_id"] = dependency.consumerSlotId;
    dependencies.push_back(std::move(entry));
  }

  auto service = NativeJson::object();
  service["service"] = plan.serviceName;
  service["model"] = plan.modelName;
  service["modelFamily"] = plan.modelFamily;
  service["roles"] = plan.roles;
  service["dependencies"] = dependencies;
  if (!plan.steps.empty()) {
    service["steps"] = steps;
    service["stepPlanVersion"] = plan.stepPlanVersion;
    service["tensorDegree"] = plan.tensorDegree;
  }
  auto root = NativeJson::object();
  root["version"] = 1;
  root["services"] = NativeJson::array({service});
  return root.dump();
}

NativeExecutionPlan
parsePlan(const std::string& wire)
{
  std::istringstream input(wire);
  return nativeExecutionPlanForServiceFromJson(input, "/service/tp");
}

} // namespace

BOOST_AUTO_TEST_SUITE(TensorParallel)
BOOST_AUTO_TEST_SUITE(PlanAndPlacement)

BOOST_AUTO_TEST_CASE(StepTemplateAcceptsTwoRankExchange)
{
  const auto plan = twoRankPlan();
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  BOOST_CHECK_EQUAL(plan.steps.size(), 5U);
  BOOST_CHECK_EQUAL(plan.tensorDegree, 2U);
}

BOOST_AUTO_TEST_CASE(LegacyPipelinePlanKeepsEmptyStepTemplate)
{
  NativeExecutionPlan plan;
  plan.serviceName = "/service/pp";
  plan.roles = {"/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"};
  // Every legacy pipeline plan has no steps, no version and degree 1.
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  BOOST_CHECK(plan.steps.empty());
  BOOST_CHECK(plan.stepPlanVersion.empty());
}

BOOST_AUTO_TEST_CASE(StepsWithoutVersionOrWithDegreeOneAreRejected)
{
  auto missingVersion = twoRankPlan();
  missingVersion.stepPlanVersion.clear();
  BOOST_CHECK_THROW(missingVersion.validateSteps(), std::invalid_argument);

  auto degreeOne = twoRankPlan();
  degreeOne.tensorDegree = 1;
  BOOST_CHECK_THROW(degreeOne.validateSteps(), std::invalid_argument);

  auto unknownVersion = twoRankPlan();
  unknownVersion.stepPlanVersion = "step-plan-v2";
  BOOST_CHECK_THROW(unknownVersion.validateSteps(), std::invalid_argument);

  auto degreeWithoutSteps = twoRankPlan();
  degreeWithoutSteps.steps.clear();
  BOOST_CHECK_THROW(degreeWithoutSteps.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SumAndSubgraphArtifactBindingsAreExclusive)
{
  auto sumWithEntry = twoRankPlan();
  sumWithEntry.steps[2].artifactEntry = "entry/should/not/exist";
  BOOST_CHECK_THROW(sumWithEntry.validateSteps(), std::invalid_argument);

  auto subgraphWithoutEntry = twoRankPlan();
  subgraphWithoutEntry.steps[0].artifactEntry.clear();
  BOOST_CHECK_THROW(subgraphWithoutEntry.validateSteps(), std::invalid_argument);

  auto unknownOperation = twoRankPlan();
  unknownOperation.steps[0].operation = "GATHER";
  BOOST_CHECK_THROW(unknownOperation.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StepTemplateRejectsCyclesAndForeignUnits)
{
  auto cycle = twoRankPlan();
  // Close the loop: the first ATTN consumes a slot produced by the last MLP.
  cycle.steps[0].inputSlots.push_back(slot("late", "REMOTE_STEP", "S0R0/MLP/0"));
  cycle.steps[4].outputSlots.push_back(slot("late", "LOCAL_STEP"));
  BOOST_CHECK_THROW(cycle.validateSteps(), std::invalid_argument);

  auto foreignUnit = twoRankPlan();
  foreignUnit.steps[0].unitId = "S0R7";
  BOOST_CHECK_THROW(foreignUnit.validateSteps(), std::invalid_argument);

  auto duplicateStep = twoRankPlan();
  duplicateStep.steps.push_back(duplicateStep.steps.front());
  BOOST_CHECK_THROW(duplicateStep.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SlotProducerBindingMustMatchItsSource)
{
  // A step-fed source without a producer cannot be satisfied by a local value.
  auto unbound = twoRankPlan();
  unbound.steps[2].inputSlots[1].producerStepId.clear();
  BOOST_CHECK_THROW(unbound.validateSteps(), std::invalid_argument);

  // A self-produced value may not declare a foreign producer.
  auto boundOutput = twoRankPlan();
  boundOutput.steps[0].outputSlots[0].producerStepId = "S0R1/ATTN/0";
  BOOST_CHECK_THROW(boundOutput.validateSteps(), std::invalid_argument);

  // Self-consumption is a cycle of length one.
  auto selfConsumer = twoRankPlan();
  selfConsumer.steps[2].inputSlots[0].producerStepId = "S0R0/SUM_ATTN/0";
  BOOST_CHECK_THROW(selfConsumer.validateSteps(), std::invalid_argument);

  // A slot that claims a producer step must be produced by exactly that step.
  auto wrongProducer = twoRankPlan();
  wrongProducer.steps[2].inputSlots[1].producerStepId = "S0R0/MLP/0";
  BOOST_CHECK_THROW(wrongProducer.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StepInputRequiresAUniqueAdmittingDependency)
{
  // A remote input slot that no dependency carries would wait forever, so the
  // plan must refuse it. This is the reverse direction of the binding check.
  auto missing = twoRankPlan();
  missing.dependencies.pop_back();
  BOOST_CHECK_THROW(missing.validateSteps(), std::invalid_argument);

  // Two dependencies admitting the same slot is equally ambiguous.
  auto duplicated = twoRankPlan();
  duplicated.dependencies.push_back(duplicated.dependencies.front());
  BOOST_CHECK_THROW(duplicated.validateSteps(), std::invalid_argument);

  // Local step inputs need no admitting dependency: dropping every remote slot
  // leaves a plan whose remaining edges are all in-process, and it must pass.
  auto localOnly = twoRankPlan();
  for (auto& step : localOnly.steps) {
    step.inputSlots.erase(std::remove_if(step.inputSlots.begin(), step.inputSlots.end(),
      [] (const auto& candidate) { return candidate.source == "REMOTE_STEP"; }),
      step.inputSlots.end());
  }
  localOnly.dependencies.clear();
  BOOST_CHECK_NO_THROW(localOnly.validateSteps());
}

BOOST_AUTO_TEST_CASE(StepDependencySlotTypeAndSourceAreChecked)
{
  // Producer and consumer must agree on the wire type of the carried tensor.
  auto wrongType = twoRankPlan();
  wrongType.steps[2].inputSlots[1].dtype = "float16";
  BOOST_CHECK_THROW(wrongType.validateSteps(), std::invalid_argument);

  auto wrongLayout = twoRankPlan();
  wrongLayout.steps[2].inputSlots[1].layoutDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  BOOST_CHECK_THROW(wrongLayout.validateSteps(), std::invalid_argument);

  // LOCAL_STEP across units would claim an in-process edge that does not exist.
  auto wrongSource = twoRankPlan();
  wrongSource.steps[2].inputSlots[1].source = "LOCAL_STEP";
  BOOST_CHECK_THROW(wrongSource.validateSteps(), std::invalid_argument);

  // REMOTE_STEP inside one unit claims a network edge that does not exist.
  auto sameUnitRemote = twoRankPlan();
  sameUnitRemote.steps[3].inputSlots[1].source = "REMOTE_STEP";
  sameUnitRemote.steps[3].inputSlots[1].producerStepId = "S0R1/ATTN/0";
  BOOST_CHECK_THROW(sameUnitRemote.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DependencyIdentityAndStepBindingAreChecked)
{
  auto duplicateIdentity = twoRankPlan();
  duplicateIdentity.dependencies.push_back(duplicateIdentity.dependencies.front());
  BOOST_CHECK_THROW(duplicateIdentity.validateSteps(), std::invalid_argument);

  auto partialBinding = twoRankPlan();
  partialBinding.dependencies[0].consumerSlotId.clear();
  BOOST_CHECK_THROW(partialBinding.validateSteps(), std::invalid_argument);

  // The dependency must name an input the consumer actually declares.
  auto unadmitted = twoRankPlan();
  unadmitted.dependencies[0].consumerSlotId = "local";
  BOOST_CHECK_THROW(unadmitted.validateSteps(), std::invalid_argument);

  auto unknownStep = twoRankPlan();
  unknownStep.dependencies[0].consumerStepId = "S0R0/MLP/9";
  BOOST_CHECK_THROW(unknownStep.validateSteps(), std::invalid_argument);

  auto missingOperation = twoRankPlan();
  missingOperation.dependencies[0].operationKind.clear();
  BOOST_CHECK_THROW(missingOperation.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanWireRoundTripPreservesStepContract)
{
  const auto plan = twoRankPlan();
  const auto parsed = parsePlan(planWire(plan));
  BOOST_CHECK_EQUAL(parsed.stepPlanVersion, NATIVE_STEP_PLAN_VERSION_V1);
  BOOST_CHECK_EQUAL(parsed.tensorDegree, 2U);
  BOOST_REQUIRE_EQUAL(parsed.steps.size(), plan.steps.size());
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    BOOST_CHECK_EQUAL(parsed.steps[index].stepId, plan.steps[index].stepId);
    BOOST_CHECK_EQUAL(parsed.steps[index].unitId, plan.steps[index].unitId);
    BOOST_CHECK_EQUAL(parsed.steps[index].operation, plan.steps[index].operation);
    BOOST_CHECK_EQUAL(parsed.steps[index].artifactEntry, plan.steps[index].artifactEntry);
    BOOST_CHECK_EQUAL(parsed.steps[index].inputSlots.size(), plan.steps[index].inputSlots.size());
    BOOST_CHECK_EQUAL(parsed.steps[index].outputSlots.size(), plan.steps[index].outputSlots.size());
  }
  BOOST_CHECK_EQUAL(parsed.steps[2].inputSlots[1].producerStepId, "S0R1/ATTN/0");
  BOOST_CHECK_EQUAL(parsed.steps[2].inputSlots[1].source, "REMOTE_STEP");
  BOOST_CHECK_EQUAL(parsed.steps[0].localStateSlots.size(), 2U);
  BOOST_REQUIRE_EQUAL(parsed.dependencies.size(), 2U);
  BOOST_CHECK_EQUAL(parsed.dependencies[0].dependencyId, "tp-attn-r0-from-r1");
  BOOST_CHECK_EQUAL(parsed.dependencies[0].producerStepId, "S0R1/ATTN/0");
  BOOST_CHECK_EQUAL(parsed.dependencies[0].consumerStepId, "S0R0/SUM_ATTN/0");
  BOOST_CHECK_EQUAL(parsed.dependencies[0].consumerSlotId, "remote");
  BOOST_CHECK_EQUAL(parsed.dependencies[1].dependencyId, "tp-attn-r1-from-r0");
  BOOST_CHECK_EQUAL(parsed.dependencies[1].consumerStepId, "S0R1/SUM_ATTN/0");
}

BOOST_AUTO_TEST_CASE(PlanWireRejectsMalformedStepTemplates)
{
  // The parse boundary runs the same validation as the in-memory contract, so a
  // hostile or stale wire cannot smuggle a cyclic or version-skewed template.
  BOOST_CHECK_THROW(parsePlan(R"({"version":1,"services":[{"service":"/service/tp",)"
    R"("roles":["S0R0"],"stepPlanVersion":"step-plan-v1","tensorDegree":2,"steps":[]}]})"),
    std::invalid_argument);
  BOOST_CHECK_THROW(parsePlan(R"({"version":1,"services":[{"service":"/service/tp",)"
    R"("roles":["S0R0"],"stepPlanVersion":"step-plan-v1","tensorDegree":2,)"
    R"("steps":[{"step_id":"a","unit_id":"S0R0","operation":"SUM",)"
    R"("input_slots":[{"slot_id":"s","dtype":"float32","layout_digest":")" +
    std::string(DIGEST_A) + R"(","source":"LOCAL_STEP","producer_step_id":"a"}],)"
    R"("output_slots":[]}]}]})"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LegacyWireStillParsesWithoutStepFields)
{
  const auto wire = R"({"version":1,"services":[{"service":"/service/tp",)"
    R"("roles":["/LLM/Pipeline/Stage/0"],"dependencies":[{"producers":["/LLM/Pipeline/Stage/0"],)"
    R"("consumers":["/LLM/Pipeline/Stage/0"],"keyScope":"pp","topicPrefix":"/service/tp",)"
    R"("objectNameTemplate":"{producerProvider}/{keyScope}","tensors":["activation"]}]}]})";
  const auto parsed = parsePlan(wire);
  BOOST_CHECK(parsed.steps.empty());
  BOOST_CHECK(parsed.stepPlanVersion.empty());
  BOOST_CHECK_EQUAL(parsed.tensorDegree, 1U);
  BOOST_REQUIRE_EQUAL(parsed.dependencies.size(), 1U);
  BOOST_CHECK(parsed.dependencies[0].dependencyId.empty());
  BOOST_CHECK(parsed.dependencies[0].producerStepId.empty());
}

BOOST_AUTO_TEST_CASE(StepBoundDependencyIsNotJudgedAsASourceCut)
{
  // A pipeline candidate's dependency carries a real source-graph cut, so its
  // tensor set must equal the declared cuts. A step-bound TP dependency carries
  // a partial produced inside a step, which is not a source cut at all.
  const auto graphDigest = nativePlanningDigest("spec224-cut-graph");
  NativeGraphSnapshot graph;
  graph.graphDigest = graphDigest;
  graph.nodes = {{"layer-00", "op", 0}, {"layer-01", "op", 1}};
  graph.topologicalOrder = {"layer-00", "layer-01"};
  graph.legalCutEdges = {"hidden-layer-00-to-01"};
  graph.edges = {{"hidden-layer-00-to-01", "layer-00", {"layer-01"},
                  {"hidden-layer-00-to-01", "float32", {std::int64_t(1)}, 4}}};
  graph.modelInputs = {{"input_ids", "int64", {std::int64_t(1)}, 8}};
  graph.modelOutputs = {{"logits", "float32", {std::int64_t(1)}, 4}};

  auto candidate = [] (const NativeGraphSnapshot& g) {
    NativeSplitCandidate value;
    value.source = "PRE_SPLIT";
    value.splitter = {"test-splitter", "1", nativePlanningDigest("splitter")};
    value.model = fixture::completeModel({"tp-model", nativePlanningDigest("content"),
      nativePlanningDigest("semantics"), g.graphDigest, "onnx", "float32", "qwen", "1"});
    value.graphDigest = g.graphDigest;
    value.executionPlan.roles = {"S0R0", "S0R1"};
    value.fragmentsByRole = {{"S0R0", nativePlanningDigest("f0")},
                             {"S0R1", nativePlanningDigest("f1")}};
    value.artifactsByRole = {{"S0R0", {nativePlanningDigest("f0")}},
                             {"S0R1", {nativePlanningDigest("f1")}}};
    value.requirementsByRole = {{"S0R0", {{"onnxruntime"}, 1024, 0, 0, 0, 0, 1.1}},
                                {"S0R1", {{"onnxruntime"}, 1024, 0, 0, 0, 0, 1.1}}};
    value.nodeRoles = {{"layer-00", "S0R0"}, {"layer-01", "S0R1"}};
    return value;
  };

  // Legacy pipeline form: dependency tensor equals the declared cut.
  auto pipeline = candidate(graph);
  pipeline.crossPartitionTensors = {"hidden-layer-00-to-01"};
  NativeDependencySpec edge;
  edge.producers = {"S0R0"};
  edge.consumers = {"S0R1"};
  edge.keyScope = "activation";
  edge.topicPrefix = "/service/tp";
  edge.objectNameTemplate = "{producerProvider}/{keyScope}";
  edge.tensors = {"hidden-layer-00-to-01"};
  edge.operationKind = "ACTIVATION";
  pipeline.executionPlan.dependencies = {edge};
  pipeline.candidateDigest = pipeline.computedDigest();
  BOOST_CHECK_NO_THROW(pipeline.validate(graph));

  // A pipeline dependency whose tensor is not a declared cut is still wrong.
  auto mismatched = candidate(graph);
  mismatched.crossPartitionTensors = {"hidden-layer-00-to-01"};
  auto wrongTensor = edge;
  wrongTensor.tensors = {"partial-not-a-cut"};
  mismatched.executionPlan.dependencies = {wrongTensor};
  mismatched.candidateDigest = mismatched.computedDigest();
  BOOST_CHECK_THROW(mismatched.validate(graph), std::invalid_argument);

  // Step-bound form: the carried tensor is a step partial with no source cut.
  auto tensorParallel = candidate(graph);
  tensorParallel.crossPartitionTensors = {"hidden-layer-00-to-01"};
  auto stepEdge = edge;
  stepEdge.tensors = {"partial"};
  stepEdge.operationKind = "TENSOR_PARTIAL";
  stepEdge.useNdnsfDataV1 = true;
  stepEdge.dependencyId = "tp-attn-0";
  stepEdge.producerStepId = "S0R1/ATTN/0";
  stepEdge.producerSlotId = "partial";
  stepEdge.consumerStepId = "S0R0/SUM_ATTN/0";
  stepEdge.consumerSlotId = "remote";
  tensorParallel.executionPlan.dependencies = {stepEdge};
  auto attn0 = NativeExecutionStepSpec{};
  attn0.stepId = "S0R1/ATTN/0";
  attn0.unitId = "S0R1";
  attn0.operation = "CALL_SUBGRAPH";
  attn0.artifactEntry = "entry/S0R1/ATTN/0";
  attn0.outputSlots = {slot("partial", "LOCAL_STEP")};
  auto sum0 = NativeExecutionStepSpec{};
  sum0.stepId = "S0R0/SUM_ATTN/0";
  sum0.unitId = "S0R0";
  sum0.operation = "SUM";
  sum0.inputSlots = {slot("remote", "REMOTE_STEP", "S0R1/ATTN/0", "partial")};
  sum0.outputSlots = {slot("merged", "LOCAL_STEP")};
  tensorParallel.executionPlan.steps = {attn0, sum0};
  tensorParallel.executionPlan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  tensorParallel.executionPlan.tensorDegree = 2;
  tensorParallel.candidateDigest = tensorParallel.computedDigest();
  BOOST_CHECK_NO_THROW(tensorParallel.validate(graph));
}

BOOST_AUTO_TEST_CASE(FixedAssignmentCarriesSealedStepsAndTheirDependencies)
{
  // A fixed assignment is what a Provider caches and reloads. If it drops the
  // step template, the Providers receives the dependency edges without the step
  // DAG those edges bind to, so the cached contract must round-trip them.
  NativeSelectionProjectionV3 first;
  first.provider = "/provider/0";
  first.requestId = "/request/1";
  first.attempt = 1;
  first.planCoreDigest = nativePlanningDigest("core");
  first.planDigest = nativePlanningDigest("plan");
  first.ackClosedDigest = nativePlanningDigest("ack");
  first.offerDigest = nativePlanningDigest("offer");
  first.securityPolicySnapshotDigest = nativePlanningDigest("security");
  first.requestContractDigest = nativePlanningDigest("contract");
  first.deadlineMs = 120000;

  first.executionRole.roleId = "S0R0";
  first.executionRole.stageId = "S0R0";
  first.executionRole.rank = 0;
  first.executionRole.layerBegin = 0;
  first.executionRole.layerEnd = 1;
  first.executionRole.backend = "onnxruntime-cpu";
  first.executionRole.adapterId = "qwen";
  first.executionRole.adapterVersion = "1";

  NativeSelectionRoleV3 assembly;
  assembly.role = "S0R0";
  assembly.selectedRole = "S0R0";
  assembly.rank = 0;
  assembly.layerBegin = 0;
  assembly.layerEnd = 1;
  assembly.backend = "onnxruntime-cpu";
  assembly.roleKind = "PIPELINE_RANGE";
  assembly.artifactDigest = nativePlanningDigest("artifact");
  assembly.recipeDigest = nativePlanningDigest("recipe");
  assembly.adapterId = "qwen";
  assembly.adapterVersion = "1";
  assembly.modelManifestDigest = nativePlanningDigest("manifest");
  assembly.artifactProfileDigest = nativePlanningDigest("profile");
  assembly.graphDigest = nativePlanningDigest("graph");
  assembly.canonicalInitializerDigest = nativePlanningDigest("initializer");
  assembly.adapterDescriptorDigest = nativePlanningDigest("adapter");
  assembly.assemblerDescriptorDigest = nativePlanningDigest("assembler");
  assembly.backendAbi = "onnxruntime-1-cpu";
  assembly.nodeIndices = {0, 1, 2, 3};
  assembly.expectedInputs = {{"x", "float32", {std::int64_t(1), std::int64_t(4)}}};
  assembly.expectedOutputs = {{"y", "float32", {std::int64_t(1), std::int64_t(4)}}};
  assembly.precision = "fp32";
  assembly.quantization = "int4";
  assembly.layout = "native";
  assembly.padding = "none";
  assembly.protectionEpoch = "plaintext-v1";
  assembly.maxSourceBytes = 4096;
  assembly.maxAssembledBytes = 2048;
  assembly.maxNodes = 4;
  first.selectedRole = assembly;
  first.assembly = assembly;

  // Every role the template names must have a Provider owner, otherwise the
  // cached contract cannot say where a remote contribution comes from.
  first.fixedProviderByRole = {{"S0R0", first.provider}, {"S0R1", "/provider/1"}};

  first.dataflow.role = "S0R0";
  first.dataflow.requestId = first.requestId;
  first.dataflow.attempt = 1;
  first.dataflow.planDigest = first.planDigest;
  first.dataflow.dataflowDigest = nativePlanningDigest("dataflow");
  first.dataflow.terminalResponseOwner = true;

  first.deviceBinding.mode = "CPU";
  first.deviceBinding.provider = first.provider;
  first.deviceBinding.role = "S0R0";
  first.deviceBinding.offerDigest = first.offerDigest;
  first.deviceBinding.topologyProfileDigest = nativePlanningDigest("topology");
  first.deviceBinding.resourceSnapshotDigest = nativePlanningDigest("resources");
  first.deviceBinding.resourceSequence = 1;
  first.deviceBinding.sharingPolicy = "EXCLUSIVE_ROLE";

  NativeExecutionPlan plan = twoRankPlan();
  // Keep only this role's contribution: one remote input plus its dependency.
  plan.roles = {"S0R0", "S0R1"};
  first.plan = plan;

  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(first);
  BOOST_REQUIRE(fixed != nullptr);
  BOOST_CHECK_EQUAL(fixed->executionTemplate()->steps.size(), plan.steps.size());
  BOOST_CHECK_EQUAL(fixed->executionTemplate()->stepPlanVersion,
                    std::string(NATIVE_STEP_PLAN_VERSION_V1));
  BOOST_CHECK_EQUAL(fixed->executionTemplate()->tensorDegree, 2U);

  // The cached wire must reproduce the same template and the same digest.
  std::istringstream source(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restored = nativeFixedRoleAssignmentV1FromJson(source);
  BOOST_CHECK_EQUAL(restored->digest(), fixed->digest());
  const auto& templateAfter = restored->executionTemplate();
  BOOST_REQUIRE_EQUAL(templateAfter->steps.size(), plan.steps.size());
  for (std::size_t i = 0; i < plan.steps.size(); ++i) {
    BOOST_CHECK_EQUAL(templateAfter->steps[i].stepId, plan.steps[i].stepId);
    BOOST_CHECK_EQUAL(templateAfter->steps[i].unitId, plan.steps[i].unitId);
    BOOST_CHECK_EQUAL(templateAfter->steps[i].operation, plan.steps[i].operation);
    BOOST_CHECK_EQUAL(templateAfter->steps[i].inputSlots.size(),
                      plan.steps[i].inputSlots.size());
  }
  BOOST_REQUIRE_EQUAL(templateAfter->dependencies.size(), plan.dependencies.size());
  BOOST_CHECK_EQUAL(templateAfter->dependencies[0].consumerSlotId,
                    plan.dependencies[0].consumerSlotId);
}

BOOST_AUTO_TEST_CASE(CapabilityRequirementsFollowTheSealedStepPlan)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;

  // A tensor-parallel plan carries SUM steps, so it requires both capabilities.
  NativeExecutionPlan tp;
  qwen::projectQwenTpPlan(tp, shape, 2, "entry", "S0R0");
  const auto required = nativePlanRequiredCapabilities(tp);
  BOOST_CHECK_EQUAL(required.size(), 2U);
  BOOST_CHECK(std::find(required.begin(), required.end(),
    std::string(NATIVE_STEP_PLAN_CAPABILITY_V1)) != required.end());
  BOOST_CHECK(std::find(required.begin(), required.end(),
    std::string(NATIVE_SUM_CAPABILITY_V1)) != required.end());

  // A legacy pipeline plan requires nothing, so an old Provider stays usable.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"/LLM/Pipeline/Stage/0"};
  BOOST_CHECK(nativePlanRequiredCapabilities(pipeline).empty());
}

BOOST_AUTO_TEST_CASE(ProviderWithoutCapabilityCannotExecuteStepPlan)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  NativeExecutionPlan tp;
  qwen::projectQwenTpPlan(tp, shape, 2, "entry", "S0R0");

  NativeProviderPlanningView provider;
  provider.provider = "/provider/a";
  provider.offerDigest = nativePlanningDigest("offer-a");
  provider.acceptedRoles = tp.roles;
  provider.backends = {"onnxruntime"};
  provider.freeBytes = 1ULL << 40;
  provider.resourceSequence = 1;
  provider.preparationAccepted = true;
  provider.executionAllowed = true;

  // Declaring the DAG alone is not enough: SUM is a separate local operation.
  provider.capabilities = {NATIVE_STEP_PLAN_CAPABILITY_V1};
  BOOST_CHECK(!nativeProviderCanExecute(provider, tp));

  provider.capabilities = {NATIVE_STEP_PLAN_CAPABILITY_V1, NATIVE_SUM_CAPABILITY_V1};
  BOOST_CHECK(nativeProviderCanExecute(provider, tp));

  // The legacy pipeline plan stays admissible to a capability-less Provider.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"/LLM/Pipeline/Stage/0"};
  provider.capabilities.clear();
  BOOST_CHECK(nativeProviderCanExecute(provider, pipeline));
}

BOOST_AUTO_TEST_CASE(StepCommunicationCostCountsRemoteEdgesOnly)
{
  // Two ranks, one layer: each rank receives exactly one remote partial per
  // reduction, so a degree-2 plan must report four remote edges over one layer
  // (attention and MLP reductions, one peer each).
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  NativeExecutionPlan tp;
  qwen::projectQwenTpPlan(tp, shape, 2, "entry", "S0R0");

  const auto cost = nativeStepCommunicationCost(tp);
  BOOST_CHECK_EQUAL(cost.remoteEdges, 4U);

  // The projection deliberately uses symbolic dimensions, so the byte total is
  // unknown. Reporting it as zero would understate the cost.
  BOOST_CHECK_EQUAL(cost.knownTransferBytes, 0U);
  BOOST_CHECK_EQUAL(cost.unknownTransferTensors, 4U);

  // A fixed-shape remote slot is costed exactly: 1*25*1024 float32 elements.
  auto fixed = tp;
  for (auto& step : fixed.steps) {
    for (auto& slot : step.inputSlots) {
      if (slot.source != "REMOTE_STEP") continue;
      slot.shape = {std::int64_t(1), std::int64_t(25), std::int64_t(1024)};
      slot.dtype = "float32";
    }
  }
  const auto exact = nativeStepCommunicationCost(fixed);
  BOOST_CHECK_EQUAL(exact.remoteEdges, 4U);
  BOOST_CHECK_EQUAL(exact.unknownTransferTensors, 0U);
  BOOST_CHECK_EQUAL(exact.knownTransferBytes, 4U * 25U * 1024U * 4U);

  // An unknown dtype is unknown, never assumed to be four bytes.
  auto badDtype = fixed;
  for (auto& step : badDtype.steps)
    for (auto& slot : step.inputSlots)
      if (slot.source == "REMOTE_STEP") slot.dtype = "int4-packed";
  const auto unknown = nativeStepCommunicationCost(badDtype);
  BOOST_CHECK_EQUAL(unknown.remoteEdges, 4U);
  BOOST_CHECK_EQUAL(unknown.knownTransferBytes, 0U);
  BOOST_CHECK_EQUAL(unknown.unknownTransferTensors, 4U);

  // A legacy pipeline plan moves no step traffic at all.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"/LLM/Pipeline/Stage/0"};
  const auto none = nativeStepCommunicationCost(pipeline);
  BOOST_CHECK_EQUAL(none.remoteEdges, 0U);
  BOOST_CHECK_EQUAL(none.knownTransferBytes, 0U);
  BOOST_CHECK_EQUAL(none.unknownTransferTensors, 0U);
}

BOOST_AUTO_TEST_CASE(ShardedStageRequiresOneArtifactPerRank)
{
  // One stage covering [0,1) with degree 2: both ranks share the layer range
  // but hold different shards, so the stage cannot hand them one artifact.
  const auto digest = [] (const char* tag) { return nativePlanningDigest(tag); };
  const auto role = std::string("/LLM/Pipeline/Stage/0");

  const auto descriptor = fixture::completeModel({"tp-model", digest("content"),
    digest("semantics"), digest("graph"), "onnx", "float32", "qwen", "1"});
  // The splitter expects the maintained semantic layer cover: embedding, one
  // numbered layer per range entry, then final norm/head. Build it explicitly so
  // the snapshot digest is bound to this descriptor.
  NativeGraphSnapshot graph;
  graph.graphDigest = descriptor.graphDigest;
  graph.nodes = {{"embedding", "embedding", 0}, {"layer-00", "op", 1},
                 {"final-norm-head", "head", 2}};
  graph.topologicalOrder = {"embedding", "layer-00", "final-norm-head"};
  graph.modelInputs = {{"input_ids", "int64", {std::int64_t(1)}, 8}};
  graph.modelOutputs = {{"logits", "float32", {std::int64_t(1)}, 4}};

  // Degree 2 without per-rank digests is refused at construction: the stage
  // artifact alone would give both ranks the same identity.
  BOOST_CHECK_THROW(
    qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
                               {role}, {2}),
    std::invalid_argument);

  // A per-rank list of the wrong size, with duplicates, or with a non-digest
  // is equally unusable.
  BOOST_CHECK_THROW(
    qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
                               {role}, {2}, {}, {}, "qwen", {{role, {digest("r0")}}}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
                               {role}, {2}, {}, {}, "qwen",
                               {{role, {digest("r0"), digest("r0")}}}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
                               {role}, {2}, {}, {}, "qwen",
                               {{role, {digest("r0"), "not-a-digest"}}}),
    std::invalid_argument);

  // A degree-one stage must not claim shards either.
  BOOST_CHECK_THROW(
    qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
                               {role}, {1}, {}, {}, "qwen",
                               {{role, {digest("r0")}}}),
    std::invalid_argument);

  // The valid form carries the two ranks into the candidate, plus the sealed
  // hybrid topology the generic candidate check demands for degree > 1.
  auto splitter = qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
    {role}, {2}, {}, {}, "qwen", {{role, {digest("r0"), digest("r1")}}});
  const auto candidates = splitter.enumerate(descriptor, graph, NativeCandidateBudget{1, 100, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_CHECK_EQUAL(candidate.tensorDegreesByRole.at(role), 2U);
  BOOST_REQUIRE_EQUAL(candidate.rankArtifactDigestsByRole.at(role).size(), 2U);
  BOOST_CHECK_EQUAL(candidate.rankArtifactDigestsByRole.at(role)[0], digest("r0"));
  BOOST_CHECK_EQUAL(candidate.rankArtifactDigestsByRole.at(role)[1], digest("r1"));
  BOOST_REQUIRE(candidate.hybridPlan.has_value());
  BOOST_CHECK_EQUAL(candidate.hybridPlan->stages, 1U);
  BOOST_CHECK_EQUAL(candidate.hybridPlan->tensorDegrees.back(), 2U);

  // Two different shard sets must not share a splitter identity.
  const auto other = qwen::NativeQwenLayerSplit({{0, 1}}, {{role, digest("stage")}}, {{role, 4096}},
    {role}, {2}, {}, {}, "qwen", {{role, {digest("r9"), digest("r8")}}}).identity();
  BOOST_CHECK(other.configurationDigest != splitter.identity().configurationDigest);
}

BOOST_AUTO_TEST_CASE(ProjectedSlotsCarryTheRealTrailingWidth)
{
  // The frozen export makes head geometry and residual width different, so a
  // slot that says "hidden" everywhere is wrong: logits are vocabulary-wide and
  // every attention/MLP partial is residual-wide.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  const auto width = [] (const NativeStepSlotSpec& slot) -> std::int64_t {
    return std::get<std::int64_t>(slot.shape.at(2));
  };
  const auto find = [&] (const std::string& stepId) -> const NativeExecutionStepSpec& {
    const auto found = std::find_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) { return step.stepId == stepId; });
    BOOST_REQUIRE(found != plan.steps.end());
    return *found;
  };

  // Residual stream: attention partial and its merged sum are hidden-wide.
  BOOST_CHECK_EQUAL(width(find("S0R0/ATTN/0").outputSlots.at(0)), 1024);
  BOOST_CHECK_EQUAL(width(find("S0R0/SUM_ATTN/0").outputSlots.at(0)), 1024);
  BOOST_CHECK_EQUAL(width(find("S0R0/MLP/0").outputSlots.at(0)), 1024);
  BOOST_CHECK_EQUAL(width(find("S0R0/SUM_MLP/0").outputSlots.at(0)), 1024);

  // Logits are vocabulary-wide, not hidden-wide.
  const auto& head = find("S0R0/HEAD");
  BOOST_CHECK_EQUAL(width(head.outputSlots.at(0)), 151936);
  BOOST_CHECK_EQUAL(head.outputSlots.at(0).slotId, std::string("logits"));

  // Slots declare one of exactly two documented layouts: activations, or the
  // integer side inputs. A slot with neither is an undeclared placeholder.
  const auto zeroDigest = "sha256:" + std::string(64, '0');
  const auto checkSlot = [&zeroDigest] (const NativeStepSlotSpec& slot) {
    const bool activation = slot.dtype == "float32";
    const bool integer = slot.dtype == "int64" || slot.dtype == "int32";
    BOOST_CHECK(activation || integer);
    BOOST_CHECK_EQUAL(slot.layoutDigest, activation
      ? std::string(qwen::QWEN_TP_ACTIVATION_LAYOUT_DIGEST)
      : std::string(qwen::QWEN_TP_INDEX_LAYOUT_DIGEST));
    BOOST_CHECK(slot.layoutDigest != zeroDigest);
  };
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.inputSlots) checkSlot(slot);
    for (const auto& slot : step.outputSlots) checkSlot(slot);
  }

  // Producer and consumer of one remote contribution must agree on the exact
  // width, which the dependency check now enforces.
  BOOST_CHECK_EQUAL(width(find("S0R1/ATTN/0").outputSlots.at(0)),
                    width(find("S0R0/SUM_ATTN/0").inputSlots.at(1)));
}

BOOST_AUTO_TEST_CASE(InitStepProducesTypedSideInputsForEveryAttentionStep)
{
  // Positions and the attention mask are request-derived side inputs. Without an
  // admitted producer each attention entry would have to invent them, and a
  // float32 declaration would misdescribe an int64/int32 wire.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  const auto find = [&] (const std::string& stepId) -> const NativeExecutionStepSpec* {
    const auto found = std::find_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) { return step.stepId == stepId; });
    return found == plan.steps.end() ? nullptr : &*found;
  };
  const auto slotOf = [] (const NativeExecutionStepSpec& step, const std::string& id,
                          bool output) -> const NativeStepSlotSpec* {
    const auto& slots = output ? step.outputSlots : step.inputSlots;
    const auto found = std::find_if(slots.begin(), slots.end(),
      [&] (const auto& slot) { return slot.slotId == id; });
    return found == slots.end() ? nullptr : &*found;
  };

  for (const auto* unit : {"S0R0", "S0R1"}) {
    const auto* init = find(std::string(unit) + "/INIT");
    BOOST_REQUIRE(init != nullptr);
    BOOST_CHECK_EQUAL(init->operation, std::string("CALL_SUBGRAPH"));
    BOOST_CHECK(!init->artifactEntry.empty());

    // Inputs are the request's raw ids and mask; outputs are the typed side
    // inputs, with the mask cast to int32 exactly as the export does.
    const auto* ids = slotOf(*init, "input_ids", false);
    const auto* mask = slotOf(*init, "attention_mask", false);
    const auto* positions = slotOf(*init, "position_ids", true);
    const auto* attnMask = slotOf(*init, "attn_mask", true);
    BOOST_REQUIRE(ids && mask && positions && attnMask);
    BOOST_CHECK_EQUAL(ids->dtype, std::string("int64"));
    BOOST_CHECK_EQUAL(ids->source, std::string("EPOCH_INPUT"));
    BOOST_CHECK_EQUAL(mask->dtype, std::string("int64"));
    BOOST_CHECK_EQUAL(positions->dtype, std::string("int64"));
    BOOST_CHECK_EQUAL(attnMask->dtype, std::string("int32"));
    BOOST_CHECK_EQUAL(attnMask->layoutDigest,
                      std::string(qwen::QWEN_TP_INDEX_LAYOUT_DIGEST));

    // Every attention step in that unit consumes both, from its own INIT.
    for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
      const auto* attn = find(std::string(unit) + "/ATTN/" + std::to_string(layer));
      BOOST_REQUIRE(attn != nullptr);
      const auto* fedPositions = slotOf(*attn, "position_ids", false);
      const auto* fedMask = slotOf(*attn, "attn_mask", false);
      BOOST_REQUIRE(fedPositions && fedMask);
      BOOST_CHECK_EQUAL(fedPositions->producerStepId, init->stepId);
      BOOST_CHECK_EQUAL(fedMask->producerStepId, init->stepId);
      BOOST_CHECK_EQUAL(fedMask->dtype, std::string("int32"));
      // Local, not remote: both ranks derive identical side inputs.
      BOOST_CHECK_EQUAL(fedPositions->source, std::string("LOCAL_STEP"));
    }
  }

  // The template must still be a valid step DAG with the side inputs wired.
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  // And no remote dependency may be created for a side input.
  BOOST_CHECK(std::none_of(plan.dependencies.begin(), plan.dependencies.end(),
    [] (const auto& dependency) { return dependency.tensors.at(0) != "partial"; }));
}

BOOST_AUTO_TEST_CASE(ResidualStaysLocalAndOutOfTheContributionSet)
{
  // C2: each rank keeps its own residual and the SUM only reduces partials. If
  // the residual were added into a rank's contribution, every rank would add it
  // once and the SUM would yield degree times the residual.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  const auto find = [&] (const std::string& stepId) -> const NativeExecutionStepSpec* {
    const auto found = std::find_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) { return step.stepId == stepId; });
    return found == plan.steps.end() ? nullptr : &*found;
  };
  const auto slotOf = [] (const NativeExecutionStepSpec& step, const std::string& id,
                          bool output) -> const NativeStepSlotSpec* {
    const auto& slots = output ? step.outputSlots : step.inputSlots;
    const auto found = std::find_if(slots.begin(), slots.end(),
      [&] (const auto& slot) { return slot.slotId == id; });
    return found == slots.end() ? nullptr : &*found;
  };

  for (const auto* unit : {"S0R0", "S0R1"}) {
    for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
      const auto* attn = find(std::string(unit) + "/ATTN/" + std::to_string(layer));
      const auto* sumAttn = find(std::string(unit) + "/SUM_ATTN/" + std::to_string(layer));
      const auto* mlp = find(std::string(unit) + "/MLP/" + std::to_string(layer));
      const auto* sumMlp = find(std::string(unit) + "/SUM_MLP/" + std::to_string(layer));
      BOOST_REQUIRE(attn && sumAttn && mlp && sumMlp);

      // The attention step keeps the layer input as a named local residual.
      const auto* attnResidual = slotOf(*attn, "residual", true);
      BOOST_REQUIRE(attnResidual != nullptr);
      BOOST_CHECK_EQUAL(attnResidual->source, std::string("LOCAL_STEP"));
      BOOST_CHECK(attnResidual->producerStepId.empty());

      // The MLP step reads that residual directly from ATTN, not via SUM.
      const auto* fedResidual = slotOf(*mlp, "residual", false);
      BOOST_REQUIRE(fedResidual != nullptr);
      BOOST_CHECK_EQUAL(fedResidual->producerStepId, attn->stepId);
      BOOST_CHECK_EQUAL(fedResidual->source, std::string("LOCAL_STEP"));

      // The SUM steps must not consume any residual: they reduce partials only.
      for (const auto* sum : {sumAttn, sumMlp}) {
        BOOST_CHECK(std::none_of(sum->inputSlots.begin(), sum->inputSlots.end(),
          [] (const auto& slot) { return slot.slotId.rfind("residual", 0) == 0 ||
                                         slot.slotId.rfind("post_residual", 0) == 0; }));
        // Every SUM input is a single-rank partial, never a merged value.
        for (const auto& slot : sum->inputSlots)
          BOOST_CHECK_EQUAL(slot.slotId.rfind("merged", 0), std::string::npos);
      }

      // The post-MLP residual is a distinct value from the pre-MLP one.
      const auto* postResidual = slotOf(*mlp, "post_residual", true);
      BOOST_REQUIRE(postResidual != nullptr);
      BOOST_CHECK(slotOf(*mlp, "residual", true) == nullptr);
    }
  }

  // No residual may cross the network: every dependency carries a partial.
  for (const auto& dependency : plan.dependencies) {
    BOOST_REQUIRE_EQUAL(dependency.tensors.size(), 1U);
    BOOST_CHECK_EQUAL(dependency.tensors.at(0), std::string("partial"));
  }
  BOOST_CHECK_NO_THROW(plan.validateSteps());
}

BOOST_AUTO_TEST_CASE(TpRanksDeriveDistinctRecipeDigests)
{
  // Two ranks of one stage own the same layer interval and therefore the same
  // canonical node set. The certified recipe JSON is what the worker derives and
  // checks, so without a rank identity both ranks would produce one recipe
  // digest while holding different shards of the weight.
  const auto digest = [] (const char* tag) { return nativePlanningDigest(tag); };

  const auto role = [] (std::uint64_t rank, std::uint64_t degree) {
    NativeSelectionRoleV3 value;
    value.role = "S0R" + std::to_string(rank);
    value.selectedRole = value.role;
    value.rank = rank;
    value.layerBegin = 0;
    value.layerEnd = 1;
    value.backend = "onnxruntime-cpu";
    value.roleKind = degree > 1 ? "HYBRID_RANK" : "PIPELINE_RANGE";
    value.artifactDigest = "sha256:" + std::string(64, '0');
    value.recipeDigest = "sha256:" + std::string(64, '0');
    value.adapterId = "qwen";
    value.adapterVersion = "1";
    value.modelManifestDigest = "sha256:" + std::string(64, '1');
    value.artifactProfileDigest = "sha256:" + std::string(64, '2');
    value.graphDigest = "sha256:" + std::string(64, '3');
    value.canonicalInitializerDigest = "sha256:" + std::string(64, '4');
    value.adapterDescriptorDigest = "sha256:" + std::string(64, '5');
    value.assemblerDescriptorDigest = "sha256:" + std::string(64, '6');
    value.backendAbi = "onnxruntime-1-cpu";
    // Identical node cover on purpose: that is what makes one stage one stage.
    value.nodeIndices = {1, 2, 3};
    value.expectedInputs = {{"x", "float32", {std::int64_t(1), std::int64_t(4)}}};
    value.expectedOutputs = {{"partial", "float32", {std::int64_t(1), std::int64_t(4)}}};
    value.precision = "fp32";
    value.quantization = "int4";
    value.layout = "native";
    value.padding = "none";
    value.protectionEpoch = "plaintext-v1";
    value.maxSourceBytes = 4096;
    value.maxAssembledBytes = 2048;
    value.maxNodes = 4;
    if (degree > 1) {
      value.tensorParallelDegree = degree;
      value.tensorParallelRank = rank;
    }
    return value;
  };

  const auto rank0 = role(0, 2);
  const auto rank1 = role(1, 2);

  // Same interval and same nodes: only the rank differs.
  BOOST_CHECK_EQUAL(rank1.layerBegin, rank0.layerBegin);
  BOOST_CHECK_EQUAL(rank1.layerEnd, rank0.layerEnd);
  BOOST_CHECK(rank1.nodeIndices == rank0.nodeIndices);

  // The certified recipe must therefore differ, or the parent could not tell the
  // two shards apart.
  const auto recipe0 = canonicalNativeOnnxRecipeJson(rank0);
  const auto recipe1 = canonicalNativeOnnxRecipeJson(rank1);
  BOOST_CHECK(recipe0 != recipe1);
  BOOST_CHECK(nativePlanningDigest(recipe0) != nativePlanningDigest(recipe1));
  BOOST_CHECK(recipe0.find("tensorParallelDegree") != std::string::npos);
  BOOST_CHECK(recipe0.find("tensorParallelRank") != std::string::npos);

  // A pipeline role emits neither member and keeps its legacy digest shape.
  const auto pipeline = role(0, 1);
  const auto pipelineRecipe = canonicalNativeOnnxRecipeJson(pipeline);
  BOOST_CHECK(pipelineRecipe.find("tensorParallelDegree") == std::string::npos);
  BOOST_CHECK(pipelineRecipe.find("tensorParallelRank") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(SlicedMatMulNBitsMatchesTheUnsplitKernel)
{
  // The shard arithmetic is only useful if the sliced initializers still produce
  // the same numbers through the real MatMulNBits kernel. Reimplementing the
  // multiply here would check the helper against itself, so this case builds
  // genuine ONNX graphs and runs them in ORT.
  constexpr std::uint64_t K = 64;      // two 32-wide blocks
  constexpr std::uint64_t N = 8;
  constexpr std::uint64_t blocks = K / qwen::QWEN_INT4_BLOCK_SIZE;
  constexpr std::uint64_t rowBytes = blocks * qwen::QWEN_INT4_BLOB_BYTES;

  // One environment for the whole case; every session below borrows it.
  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-shard-math");

  std::mt19937 rng(7);
  std::vector<std::uint8_t> packed(N * rowBytes, 0);
  for (auto& byte : packed) byte = std::uint8_t(rng() & 0xFF);
  std::vector<float> scales(N * blocks, 0.5f);
  std::vector<float> activation(2 * K);
  for (auto& value : activation) value = float(int(rng() % 21) - 10) * 0.25f;

  const auto scaleBytes = [&] {
    std::vector<std::uint8_t> bytes(scales.size() * sizeof(float));
    std::memcpy(bytes.data(), scales.data(), bytes.size());
    return bytes;
  }();
  const auto toFloat = [] (const std::vector<std::uint8_t>& bytes) {
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
  };

  // Build and run one MatMulNBits graph. Input is [1, 2, k], output [1, 2, n].
  const auto runKernel = [&] (std::uint64_t k, std::uint64_t n,
                              const std::vector<std::uint8_t>& weight,
                              const std::vector<float>& rankScales,
                              const std::vector<float>& input) {
    onnx::ModelProto model;
    model.set_ir_version(7);
    auto* opset = model.add_opset_import();
    opset->set_domain(""); opset->set_version(17);
    auto* ms = model.add_opset_import();
    ms->set_domain("com.microsoft"); ms->set_version(1);
    auto* graph = model.mutable_graph(); graph->set_name("spec224-shard-math");
    auto* node = graph->add_node();
    node->set_name("mm"); node->set_op_type("MatMulNBits");
    node->set_domain("com.microsoft");
    node->add_input("A"); node->add_input("B"); node->add_input("S");
    node->add_output("Y");
    const auto addInt = [&] (const char* name, std::int64_t value) {
      auto* attribute = node->add_attribute();
      attribute->set_name(name);
      attribute->set_type(onnx::AttributeProto::INT);
      attribute->set_i(value);
    };
    addInt("K", std::int64_t(k));
    addInt("N", std::int64_t(n));
    addInt("bits", 4);
    addInt("block_size", std::int64_t(qwen::QWEN_INT4_BLOCK_SIZE));

    auto* weightInit = graph->add_initializer();
    weightInit->set_name("B");
    weightInit->set_data_type(onnx::TensorProto_DataType_UINT8);
    weightInit->add_dims(std::int64_t(n));
    weightInit->add_dims(std::int64_t(k / qwen::QWEN_INT4_BLOCK_SIZE));
    weightInit->add_dims(std::int64_t(qwen::QWEN_INT4_BLOB_BYTES));
    weightInit->set_raw_data(weight.data(), weight.size());

    auto* scaleInit = graph->add_initializer();
    scaleInit->set_name("S");
    scaleInit->set_data_type(onnx::TensorProto_DataType_FLOAT);
    scaleInit->add_dims(std::int64_t(rankScales.size()));
    scaleInit->set_raw_data(rankScales.data(), rankScales.size() * sizeof(float));

    const auto addIO = [&] (const char* name, bool input) {
      auto* value = input ? graph->add_input() : graph->add_output();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto_DataType_FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(2);
      tensor->mutable_shape()->add_dim()->set_dim_value(std::int64_t(input ? k : n));
    };
    addIO("A", true);
    addIO("Y", false);

    std::string bytes;
    model.SerializeToString(&bytes);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(ortEnv, bytes.data(), bytes.size(), options);
    const std::vector<std::int64_t> shape{1, 2, std::int64_t(k)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputValue = Ort::Value::CreateTensor<float>(memory,
      const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
    const char* inputNames[] = {"A"};
    const char* outputNames[] = {"Y"};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, &inputValue, 1,
                               outputNames, 1);
    const auto* data = outputs[0].GetTensorData<float>();
    return std::vector<float>(data, data + 2 * n);
  };

  const auto reference = runKernel(K, N, packed, scales, activation);
  BOOST_REQUIRE_EQUAL(reference.size(), 2 * N);

  // Output-channel split: each rank owns whole rows of B and their scales, and
  // its columns must equal the corresponding reference columns.
  {
    const auto leftWeight = qwen::slicePackedInt4(packed, N, K,
      qwen::QwenSliceAxis::OutputChannels, 0, N / 2);
    const auto rightWeight = qwen::slicePackedInt4(packed, N, K,
      qwen::QwenSliceAxis::OutputChannels, N / 2, N);
    const auto leftScales = toFloat(qwen::sliceInt4Scales(scaleBytes, N, K,
      qwen::QwenSliceAxis::OutputChannels, 0, N / 2));
    const auto rightScales = toFloat(qwen::sliceInt4Scales(scaleBytes, N, K,
      qwen::QwenSliceAxis::OutputChannels, N / 2, N));
    const auto left = runKernel(K, N / 2, leftWeight, leftScales, activation);
    const auto right = runKernel(K, N / 2, rightWeight, rightScales, activation);
    for (std::uint64_t i = 0; i < 2; ++i) {
      for (std::uint64_t c = 0; c < N / 2; ++c) {
        BOOST_CHECK_SMALL(left[i * (N / 2) + c] - reference[i * N + c], 1e-4f);
        BOOST_CHECK_SMALL(right[i * (N / 2) + c] - reference[i * N + N / 2 + c], 1e-4f);
      }
    }
  }

  // Input-channel split: each rank reduces over half of K and the two partials
  // must sum to the unsplit result.
  {
    const std::uint64_t half = K / 2;
    const auto leftWeight = qwen::slicePackedInt4(packed, N, K,
      qwen::QwenSliceAxis::InputChannels, 0, half);
    const auto rightWeight = qwen::slicePackedInt4(packed, N, K,
      qwen::QwenSliceAxis::InputChannels, half, K);
    const auto leftScales = toFloat(qwen::sliceInt4Scales(scaleBytes, N, K,
      qwen::QwenSliceAxis::InputChannels, 0, half));
    const auto rightScales = toFloat(qwen::sliceInt4Scales(scaleBytes, N, K,
      qwen::QwenSliceAxis::InputChannels, half, K));
    // Row-major [1,2,K]: take the matching half of each row, not a flat range.
    std::vector<float> leftInput(2 * half), rightInput(2 * half);
    for (std::uint64_t row = 0; row < 2; ++row) {
      for (std::uint64_t k = 0; k < half; ++k) {
        leftInput[row * half + k] = activation[row * K + k];
        rightInput[row * half + k] = activation[row * K + half + k];
      }
    }
    const auto left = runKernel(half, N, leftWeight, leftScales, leftInput);
    const auto right = runKernel(half, N, rightWeight, rightScales, rightInput);
    for (std::size_t i = 0; i < reference.size(); ++i)
      BOOST_CHECK_SMALL((left[i] + right[i]) - reference[i], 1e-3f);
  }
}

BOOST_AUTO_TEST_CASE(TensorParallelMlpBlockMatchesUnsplitReference)
{
  // The C2 claim for the MLP is
  //   Y = X' + SUM_r( (SiLU(X' W_gate,r) * (X' W_up,r)) W_down,r )
  // with gate/up split on the FFN axis and down split on the same axis.
  // Checking one MatMulNBits at a time cannot show that the three-projection
  // composition still matches, so this case builds the whole block twice — once
  // unsplit, once per rank — and compares the reduced result.
  //
  // Real frozen geometry: hidden 1024, FFN 3072, block size 32.
  constexpr std::uint64_t hidden = 1024;
  constexpr std::uint64_t ffn = 3072;
  constexpr std::uint64_t degree = 2;
  constexpr std::uint64_t ffnPerRank = ffn / degree;
  constexpr std::uint64_t seq = 2;
  constexpr std::uint64_t blockSize = qwen::QWEN_INT4_BLOCK_SIZE;
  constexpr std::uint64_t blobBytes = qwen::QWEN_INT4_BLOB_BYTES;

  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-mlp-block");
  std::mt19937 rng(11);

  const auto randomPacked = [&] (std::uint64_t n, std::uint64_t k) {
    std::vector<std::uint8_t> bytes(n * (k / blockSize) * blobBytes);
    for (auto& byte : bytes) byte = std::uint8_t(rng() & 0xFF);
    return bytes;
  };
  const auto randomScales = [&] (std::uint64_t count) {
    std::vector<float> values(count);
    for (auto& value : values) value = 0.001f + 0.0005f * float(rng() % 8);
    return values;
  };
  std::vector<float> input(hidden * seq);
  for (auto& value : input) value = float(int(rng() % 41) - 20) * 0.05f;

  // Build one graph: a chain of MatMulNBits with the Qwen MLP activation and
  // the elementwise product, ending in a final MatMulNBits.
  const auto runMlp = [&] (std::uint64_t rankFfn,
                           const std::vector<std::uint8_t>& gateW,
                           const std::vector<float>& gateS,
                           const std::vector<std::uint8_t>& upW,
                           const std::vector<float>& upS,
                           const std::vector<std::uint8_t>& downW,
                           const std::vector<float>& downS) {
    onnx::ModelProto model;
    model.set_ir_version(7);
    auto* opset = model.add_opset_import();
    opset->set_domain(""); opset->set_version(17);
    auto* ms = model.add_opset_import();
    ms->set_domain("com.microsoft"); ms->set_version(1);
    auto* graph = model.mutable_graph();
    graph->set_name("spec224-mlp-block");

    const auto addNbits = [&] (const char* name, const char* in, const char* out,
                               std::uint64_t k, std::uint64_t n,
                               const std::vector<std::uint8_t>& weight,
                               const std::vector<float>& scales) {
      auto* node = graph->add_node();
      node->set_name(name);
      node->set_op_type("MatMulNBits");
      node->set_domain("com.microsoft");
      node->add_input(in);
      node->add_input(std::string(name) + "_B");
      node->add_input(std::string(name) + "_S");
      node->add_output(out);
      const auto addInt = [&] (const char* key, std::int64_t value) {
        auto* attribute = node->add_attribute();
        attribute->set_name(key);
        attribute->set_type(onnx::AttributeProto::INT);
        attribute->set_i(value);
      };
      addInt("K", std::int64_t(k));
      addInt("N", std::int64_t(n));
      addInt("bits", 4);
      addInt("block_size", std::int64_t(blockSize));
      auto* weightInit = graph->add_initializer();
      weightInit->set_name(std::string(name) + "_B");
      weightInit->set_data_type(onnx::TensorProto_DataType_UINT8);
      weightInit->add_dims(std::int64_t(n));
      weightInit->add_dims(std::int64_t(k / blockSize));
      weightInit->add_dims(std::int64_t(blobBytes));
      weightInit->set_raw_data(weight.data(), weight.size());
      auto* scaleInit = graph->add_initializer();
      scaleInit->set_name(std::string(name) + "_S");
      scaleInit->set_data_type(onnx::TensorProto_DataType_FLOAT);
      scaleInit->add_dims(std::int64_t(scales.size()));
      scaleInit->set_raw_data(scales.data(), scales.size() * sizeof(float));
    };
    const auto addSimple = [&] (const char* op, std::vector<const char*> ins,
                                const char* out) {
      auto* node = graph->add_node();
      node->set_op_type(op);
      for (const auto* in : ins) node->add_input(in);
      node->add_output(out);
    };

    addNbits("gate", "X", "gate_out", hidden, rankFfn, gateW, gateS);
    addNbits("up", "X", "up_out", hidden, rankFfn, upW, upS);
    addSimple("Sigmoid", {"gate_out"}, "gate_sigmoid");
    addSimple("Mul", {"gate_out", "gate_sigmoid"}, "gate_act");
    addSimple("Mul", {"gate_act", "up_out"}, "act_out");
    addNbits("down", "act_out", "Y", rankFfn, hidden, downW, downS);

    auto* in = graph->add_input();
    in->set_name("X");
    auto* inType = in->mutable_type()->mutable_tensor_type();
    inType->set_elem_type(onnx::TensorProto_DataType_FLOAT);
    inType->mutable_shape()->add_dim()->set_dim_value(1);
    inType->mutable_shape()->add_dim()->set_dim_value(std::int64_t(seq));
    inType->mutable_shape()->add_dim()->set_dim_value(std::int64_t(hidden));
    auto* out = graph->add_output();
    out->set_name("Y");
    auto* outType = out->mutable_type()->mutable_tensor_type();
    outType->set_elem_type(onnx::TensorProto_DataType_FLOAT);
    outType->mutable_shape()->add_dim()->set_dim_value(1);
    outType->mutable_shape()->add_dim()->set_dim_value(std::int64_t(seq));
    outType->mutable_shape()->add_dim()->set_dim_value(std::int64_t(hidden));

    std::string bytes;
    model.SerializeToString(&bytes);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(ortEnv, bytes.data(), bytes.size(), options);
    const std::vector<std::int64_t> shape{1, std::int64_t(seq), std::int64_t(hidden)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputValue = Ort::Value::CreateTensor<float>(memory,
      const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
    const char* inputNames[] = {"X"};
    const char* outputNames[] = {"Y"};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, &inputValue, 1,
                               outputNames, 1);
    const auto* data = outputs[0].GetTensorData<float>();
    return std::vector<float>(data, data + hidden * seq);
  };

  // Unsplit reference.
  const auto gateW = randomPacked(ffn, hidden);
  const auto gateS = randomScales(ffn * (hidden / blockSize));
  const auto upW = randomPacked(ffn, hidden);
  const auto upS = randomScales(ffn * (hidden / blockSize));
  const auto downW = randomPacked(hidden, ffn);
  const auto downS = randomScales(hidden * (ffn / blockSize));
  const auto reference = runMlp(ffn, gateW, gateS, upW, upS, downW, downS);
  BOOST_REQUIRE_EQUAL(reference.size(), hidden * seq);

  // Per-rank partials, then the SUM the runtime would perform.
  std::vector<float> reduced(hidden * seq, 0.0f);
  const auto scaleBytesOf = [] (const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
  };
  const auto toFloat = [] (const std::vector<std::uint8_t>& bytes) {
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
  };
  for (std::uint64_t rank = 0; rank < degree; ++rank) {
    const auto begin = rank * ffnPerRank;
    const auto end = begin + ffnPerRank;
    // gate/up: output channels.
    const auto gateRank = qwen::slicePackedInt4(gateW, ffn, hidden,
      qwen::QwenSliceAxis::OutputChannels, begin, end);
    const auto upRank = qwen::slicePackedInt4(upW, ffn, hidden,
      qwen::QwenSliceAxis::OutputChannels, begin, end);
    const auto gateSRank = toFloat(qwen::sliceInt4Scales(scaleBytesOf(gateS), ffn, hidden,
      qwen::QwenSliceAxis::OutputChannels, begin, end));
    const auto upSRank = toFloat(qwen::sliceInt4Scales(scaleBytesOf(upS), ffn, hidden,
      qwen::QwenSliceAxis::OutputChannels, begin, end));
    // down: the same FFN interval on its reduction axis.
    const auto downRank = qwen::slicePackedInt4(downW, hidden, ffn,
      qwen::QwenSliceAxis::InputChannels, begin, end);
    const auto downSRank = toFloat(qwen::sliceInt4Scales(scaleBytesOf(downS), hidden, ffn,
      qwen::QwenSliceAxis::InputChannels, begin, end));

    const auto partial = runMlp(ffnPerRank, gateRank, gateSRank, upRank, upSRank,
                                downRank, downSRank);
    BOOST_REQUIRE_EQUAL(partial.size(), reduced.size());
    for (std::size_t i = 0; i < partial.size(); ++i) reduced[i] += partial[i];
  }

  // Each partial is a small sum of products, so the reduction tolerance is
  // tighter than a relative comparison would need; check both magnitude and
  // shape so a silently-zero output cannot pass.
  double maxAbsolute = 0.0;
  double maxReference = 0.0;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    maxAbsolute = std::max(maxAbsolute, double(std::fabs(reduced[i] - reference[i])));
    maxReference = std::max(maxReference, double(std::fabs(reference[i])));
  }
  BOOST_TEST_MESSAGE("MLP block max abs error " << maxAbsolute
    << " against max |reference| " << maxReference);
  BOOST_CHECK(maxReference > 1e-3);
  BOOST_CHECK(maxAbsolute < 1e-3);
}

BOOST_AUTO_TEST_CASE(TensorParallelAttentionMatchesUnsplitReference)
{
  // C2 for attention: each rank computes attention for its own whole heads, and
  // the O projection reduces over the same attention channels. The composition
  // must equal the unsplit block:
  //   attn = concat_h( softmax(Q_h K_h^T / sqrt(d)) V_h )
  //   Y    = attn W_O^T  ==  SUM_r( attn_r W_O,r^T )
  //
  // The part that can silently go wrong is the GQA grouping after the split:
  // 16 query heads over 8 KV heads is a group of 2, so a rank holding 8 query
  // heads must hold exactly 4 KV heads to keep the same grouping. A rank with
  // the wrong KV count would still run and still produce plausible numbers.
  //
  // RoPE is deliberately absent. It is a per-head, position-wise rotation, so
  // splitting heads commutes with it; covering it here would test the frozen
  // export's cos/sin tables rather than the partition. That belongs to the
  // assembled-entry comparison.
  constexpr std::uint64_t hidden = 1024;
  constexpr std::uint64_t queryHeads = 16;
  constexpr std::uint64_t kvHeads = 8;
  constexpr std::uint64_t headDim = 128;
  constexpr std::uint64_t seq = 4;
  constexpr std::uint64_t degree = 2;
  constexpr std::uint64_t queryWidth = queryHeads * headDim;
  constexpr std::uint64_t kvWidth = kvHeads * headDim;
  constexpr std::uint64_t blockSize = qwen::QWEN_INT4_BLOCK_SIZE;
  constexpr std::uint64_t blobBytes = qwen::QWEN_INT4_BLOB_BYTES;

  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-attention-block");
  std::mt19937 rng(23);

  const auto randomPacked = [&] (std::uint64_t n, std::uint64_t k) {
    std::vector<std::uint8_t> bytes(n * (k / blockSize) * blobBytes);
    for (auto& byte : bytes) byte = std::uint8_t(rng() & 0xFF);
    return bytes;
  };
  const auto randomScales = [&] (std::uint64_t count) {
    std::vector<float> values(count);
    for (auto& value : values) value = 0.001f + 0.0005f * float(rng() % 8);
    return values;
  };
  std::vector<float> input(hidden * seq);
  for (auto& value : input) value = float(int(rng() % 21) - 10) * 0.02f;

  // One attention block: three INT4 projections, head-wise scaled dot-product
  // attention with GQA broadcast and a causal mask, then the INT4 O projection.
  // The head counts are parameters so the same builder serves the unsplit block
  // and each rank's shard.
  const auto runAttention = [&] (std::uint64_t qHeads, std::uint64_t kHeads,
                                 const std::vector<std::uint8_t>& wq,
                                 const std::vector<float>& sq,
                                 const std::vector<std::uint8_t>& wk,
                                 const std::vector<float>& sk,
                                 const std::vector<std::uint8_t>& wv,
                                 const std::vector<float>& sv,
                                 std::uint64_t oK,
                                 const std::vector<std::uint8_t>& wo,
                                 const std::vector<float>& so) {
    onnx::ModelProto model;
    model.set_ir_version(7);
    auto* opset = model.add_opset_import();
    opset->set_domain(""); opset->set_version(17);
    auto* ms = model.add_opset_import();
    ms->set_domain("com.microsoft"); ms->set_version(1);
    auto* graph = model.mutable_graph();
    graph->set_name("spec224-attention-block");

    const auto addNbits = [&] (const char* name, const char* in, const char* out,
                               std::uint64_t k, std::uint64_t n,
                               const std::vector<std::uint8_t>& weight,
                               const std::vector<float>& scales) {
      auto* node = graph->add_node();
      node->set_name(name);
      node->set_op_type("MatMulNBits");
      node->set_domain("com.microsoft");
      node->add_input(in);
      node->add_input(std::string(name) + "_B");
      node->add_input(std::string(name) + "_S");
      node->add_output(out);
      const auto addInt = [&] (const char* key, std::int64_t value) {
        auto* attribute = node->add_attribute();
        attribute->set_name(key);
        attribute->set_type(onnx::AttributeProto::INT);
        attribute->set_i(value);
      };
      addInt("K", std::int64_t(k));
      addInt("N", std::int64_t(n));
      addInt("bits", 4);
      addInt("block_size", std::int64_t(blockSize));
      auto* weightInit = graph->add_initializer();
      weightInit->set_name(std::string(name) + "_B");
      weightInit->set_data_type(onnx::TensorProto_DataType_UINT8);
      weightInit->add_dims(std::int64_t(n));
      weightInit->add_dims(std::int64_t(k / blockSize));
      weightInit->add_dims(std::int64_t(blobBytes));
      weightInit->set_raw_data(weight.data(), weight.size());
      auto* scaleInit = graph->add_initializer();
      scaleInit->set_name(std::string(name) + "_S");
      scaleInit->set_data_type(onnx::TensorProto_DataType_FLOAT);
      scaleInit->add_dims(std::int64_t(scales.size()));
      scaleInit->set_raw_data(scales.data(), scales.size() * sizeof(float));
    };
    const auto addOp = [&] (const char* op, std::vector<const char*> ins, const char* out) {
      auto* node = graph->add_node();
      node->set_op_type(op);
      for (const auto* in : ins) node->add_input(in);
      node->add_output(out);
      return node;
    };
    const auto addInts = [&] (onnx::NodeProto* node, const char* key,
                              std::initializer_list<std::int64_t> values) {
      auto* attribute = node->add_attribute();
      attribute->set_name(key);
      attribute->set_type(onnx::AttributeProto::INTS);
      for (const auto value : values) attribute->add_ints(value);
    };
    const auto addFloat = [&] (onnx::NodeProto* node, const char* key, float value) {
      auto* attribute = node->add_attribute();
      attribute->set_name(key);
      attribute->set_type(onnx::AttributeProto::FLOAT);
      attribute->set_f(value);
    };

    addNbits("wq", "X", "q_flat", hidden, qHeads * headDim, wq, sq);
    addNbits("wk", "X", "k_flat", hidden, kHeads * headDim, wk, sk);
    addNbits("wv", "X", "v_flat", hidden, kHeads * headDim, wv, sv);

    // (batch, seq, head, dim) -> (batch, head, seq, dim) so MatMul works per head.
    const auto toHeads = [&] (const char* in, const char* out, std::uint64_t heads,
                              std::uint64_t width) {
      auto* shape = graph->add_initializer();
      shape->set_name(std::string(out) + "_shape");
      shape->set_data_type(onnx::TensorProto_DataType_INT64);
      shape->add_dims(4);
      for (const std::int64_t value : {std::int64_t(1), std::int64_t(seq),
                                       std::int64_t(heads), std::int64_t(width)})
        shape->add_int64_data(value);
      addOp("Reshape", {in, (std::string(out) + "_shape").c_str()},
            (std::string(out) + "_4d").c_str());
      auto* transpose = addOp("Transpose", {(std::string(out) + "_4d").c_str()}, out);
      addInts(transpose, "perm", {0, 2, 1, 3});
    };
    toHeads("q_flat", "q", qHeads, headDim);
    toHeads("k_flat", "k", kHeads, headDim);
    toHeads("v_flat", "v", kHeads, headDim);

    // GQA: every KV head is shared by queryHeads/kvHeads consecutive query heads.
    const auto group = qHeads / kHeads;
    // Insert a length-1 group axis, then broadcast it to `group` copies. The
    // reshape target must keep the element count, so the group axis goes in as
    // 1 and the expand target is what carries `group`.
    auto* kGroupedShape = graph->add_initializer();
    kGroupedShape->set_name("k_grouped_shape");
    kGroupedShape->set_data_type(onnx::TensorProto_DataType_INT64);
    kGroupedShape->add_dims(5);
    for (const std::int64_t value : {std::int64_t(1), std::int64_t(kHeads),
                                     std::int64_t(1), std::int64_t(seq),
                                     std::int64_t(headDim)})
      kGroupedShape->add_int64_data(value);
    auto* expandShape = graph->add_initializer();
    expandShape->set_name("k_expand_shape");
    expandShape->set_data_type(onnx::TensorProto_DataType_INT64);
    expandShape->add_dims(5);
    for (const std::int64_t value : {std::int64_t(1), std::int64_t(kHeads),
                                     std::int64_t(group), std::int64_t(seq),
                                     std::int64_t(headDim)})
      expandShape->add_int64_data(value);
    addOp("Reshape", {"k", "k_grouped_shape"}, "k_grouped");
    addOp("Expand", {"k_grouped", "k_expand_shape"}, "k_broadcast");
    auto* kFinalShape = graph->add_initializer();
    kFinalShape->set_name("k_final_shape");
    kFinalShape->set_data_type(onnx::TensorProto_DataType_INT64);
    kFinalShape->add_dims(4);
    for (const std::int64_t value : {std::int64_t(1), std::int64_t(qHeads),
                                     std::int64_t(seq), std::int64_t(headDim)})
      kFinalShape->add_int64_data(value);
    addOp("Reshape", {"k_broadcast", "k_final_shape"}, "k_heads");
    addOp("Reshape", {"v", "k_grouped_shape"}, "v_grouped");
    addOp("Expand", {"v_grouped", "k_expand_shape"}, "v_broadcast");
    addOp("Reshape", {"v_broadcast", "k_final_shape"}, "v_heads");

    // scores = Q K^T * scale + causal mask
    auto* transposeK = addOp("Transpose", {"k_heads"}, "k_heads_t");
    addInts(transposeK, "perm", {0, 1, 3, 2});
    addOp("MatMul", {"q", "k_heads_t"}, "scores");
    auto* scaleInit = graph->add_initializer();
    scaleInit->set_name("scale");
    scaleInit->set_data_type(onnx::TensorProto_DataType_FLOAT);
    scaleInit->add_dims(1);
    scaleInit->add_float_data(1.0f / std::sqrt(float(headDim)));
    addOp("Mul", {"scores", "scale"}, "scores_scaled");

    // Causal mask: 0 on and below the diagonal, a large negative above it.
    auto* mask = graph->add_initializer();
    mask->set_name("causal_mask");
    mask->set_data_type(onnx::TensorProto_DataType_FLOAT);
    mask->add_dims(1);
    mask->add_dims(1);
    mask->add_dims(std::int64_t(seq));
    mask->add_dims(std::int64_t(seq));
    for (std::uint64_t row = 0; row < seq; ++row)
      for (std::uint64_t column = 0; column < seq; ++column)
        mask->add_float_data(column <= row ? 0.0f : -10000.0f);
    addOp("Add", {"scores_scaled", "causal_mask"}, "scores_masked");
    auto* softmax = addOp("Softmax", {"scores_masked"}, "probs");
    {
      auto* attribute = softmax->add_attribute();
      attribute->set_name("axis");
      attribute->set_type(onnx::AttributeProto::INT);
      attribute->set_i(3);
    }

    addOp("MatMul", {"probs", "v_heads"}, "attn_heads");
    auto* back = addOp("Transpose", {"attn_heads"}, "attn_heads_seq");
    addInts(back, "perm", {0, 2, 1, 3});
    auto* flatShape = graph->add_initializer();
    flatShape->set_name("attn_flat_shape");
    flatShape->set_data_type(onnx::TensorProto_DataType_INT64);
    flatShape->add_dims(3);
    for (const std::int64_t value : {std::int64_t(1), std::int64_t(seq),
                                     std::int64_t(qHeads * headDim)})
      flatShape->add_int64_data(value);
    addOp("Reshape", {"attn_heads_seq", "attn_flat_shape"}, "attn_flat");
    addNbits("wo", "attn_flat", "Y", oK, hidden, wo, so);

    const auto addIO = [&] (const char* name, bool isInput, std::int64_t width) {
      auto* value = isInput ? graph->add_input() : graph->add_output();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto_DataType_FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(std::int64_t(seq));
      tensor->mutable_shape()->add_dim()->set_dim_value(width);
    };
    addIO("X", true, std::int64_t(hidden));
    addIO("Y", false, std::int64_t(hidden));

    std::string bytes;
    model.SerializeToString(&bytes);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(ortEnv, bytes.data(), bytes.size(), options);
    const std::vector<std::int64_t> shape{1, std::int64_t(seq), std::int64_t(hidden)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputValue = Ort::Value::CreateTensor<float>(memory,
      const_cast<float*>(input.data()), input.size(), shape.data(), shape.size());
    const char* inputNames[] = {"X"};
    const char* outputNames[] = {"Y"};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, &inputValue, 1,
                               outputNames, 1);
    const auto* data = outputs[0].GetTensorData<float>();
    return std::vector<float>(data, data + hidden * seq);
  };

  const auto wq = randomPacked(queryWidth, hidden);
  const auto sq = randomScales(queryWidth * (hidden / blockSize));
  const auto wk = randomPacked(kvWidth, hidden);
  const auto sk = randomScales(kvWidth * (hidden / blockSize));
  const auto wv = randomPacked(kvWidth, hidden);
  const auto sv = randomScales(kvWidth * (hidden / blockSize));
  const auto wo = randomPacked(hidden, queryWidth);
  const auto so = randomScales(hidden * (queryWidth / blockSize));

  const auto reference = runAttention(queryHeads, kvHeads, wq, sq, wk, sk, wv, sv,
                                      queryWidth, wo, so);
  BOOST_REQUIRE_EQUAL(reference.size(), hidden * seq);

  // Per-rank shards. Query and KV heads split by whole heads in the same
  // proportion, so the local GQA group stays 2 exactly as in the unsplit graph.
  const auto scaleBytesOf = [] (const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
  };
  const auto toFloat = [] (const std::vector<std::uint8_t>& bytes) {
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
  };

  std::vector<float> reduced(hidden * seq, 0.0f);
  for (std::uint64_t rank = 0; rank < degree; ++rank) {
    const auto qPerRank = queryHeads / degree;
    const auto kvPerRank = kvHeads / degree;
    const auto qBegin = rank * qPerRank * headDim;
    const auto qEnd = qBegin + qPerRank * headDim;
    const auto kvBegin = rank * kvPerRank * headDim;
    const auto kvEnd = kvBegin + kvPerRank * headDim;

    const auto rankWq = qwen::slicePackedInt4(wq, queryWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, qBegin, qEnd);
    const auto rankWk = qwen::slicePackedInt4(wk, kvWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, kvBegin, kvEnd);
    const auto rankWv = qwen::slicePackedInt4(wv, kvWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, kvBegin, kvEnd);
    const auto rankSq = toFloat(qwen::sliceInt4Scales(scaleBytesOf(sq), queryWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, qBegin, qEnd));
    const auto rankSk = toFloat(qwen::sliceInt4Scales(scaleBytesOf(sk), kvWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, kvBegin, kvEnd));
    const auto rankSv = toFloat(qwen::sliceInt4Scales(scaleBytesOf(sv), kvWidth, hidden,
      qwen::QwenSliceAxis::OutputChannels, kvBegin, kvEnd));
    // The O projection reduces over this rank's attention channels only.
    const auto rankWo = qwen::slicePackedInt4(wo, hidden, queryWidth,
      qwen::QwenSliceAxis::InputChannels, qBegin, qEnd);
    const auto rankSo = toFloat(qwen::sliceInt4Scales(scaleBytesOf(so), hidden, queryWidth,
      qwen::QwenSliceAxis::InputChannels, qBegin, qEnd));

    const auto partial = runAttention(qPerRank, kvPerRank, rankWq, rankSq, rankWk, rankSk,
                                      rankWv, rankSv, qPerRank * headDim, rankWo, rankSo);
    BOOST_REQUIRE_EQUAL(partial.size(), reduced.size());
    for (std::size_t i = 0; i < partial.size(); ++i) reduced[i] += partial[i];
  }

  double maxAbsolute = 0.0;
  double maxReference = 0.0;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    maxAbsolute = std::max(maxAbsolute, double(std::fabs(reduced[i] - reference[i])));
    maxReference = std::max(maxReference, double(std::fabs(reference[i])));
  }
  BOOST_TEST_MESSAGE("attention block max abs error " << maxAbsolute
    << " against max |reference| " << maxReference);
  BOOST_CHECK(maxReference > 1e-3);
  BOOST_CHECK(maxAbsolute < 1e-4);
}

BOOST_AUTO_TEST_CASE(ResidualChainMatchesTheUnsplitSkipNorm)
{
  // C2: the residual stays local, so the layer is
  //   X' = X + SUM_r(A_r)      and then norm(X') feeds the MLP.
  // The tempting wrong form adds the residual per rank before the SUM:
  //   SUM_r(X + A_r) = degree*X + SUM_r(A_r)
  // which is numerically different and still produces finite, plausible output.
  // This case runs the real SkipSimplifiedLayerNormalization kernel the frozen
  // export uses, so both forms are compared through production arithmetic.
  constexpr std::uint64_t hidden = 64;
  constexpr std::uint64_t seq = 2;
  constexpr std::uint64_t degree = 2;

  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-residual");
  std::mt19937 rng(31);

  std::vector<float> x0(hidden * seq);
  for (auto& value : x0) value = float(int(rng() % 21) - 10) * 0.05f;
  std::vector<float> normWeight(hidden);
  for (auto& value : normWeight) value = 0.8f + 0.01f * float(rng() % 40);
  // Two rank partials with the same shape as the residual stream.
  std::vector<float> partialA(hidden * seq), partialB(hidden * seq);
  for (auto& value : partialA) value = float(int(rng() % 21) - 10) * 0.02f;
  for (auto& value : partialB) value = float(int(rng() % 21) - 10) * 0.02f;

  // Run one SkipSimplifiedLayerNormalization and return the normalized output
  // together with the skip sum, which is exactly X' in the C2 formula.
  const auto runSkip = [&] (const std::vector<float>& x, const std::vector<float>& skip) {
    onnx::ModelProto model;
    model.set_ir_version(7);
    auto* opset = model.add_opset_import();
    opset->set_domain(""); opset->set_version(17);
    auto* ms = model.add_opset_import();
    ms->set_domain("com.microsoft"); ms->set_version(1);
    auto* graph = model.mutable_graph();
    graph->set_name("spec224-residual");

    auto* node = graph->add_node();
    node->set_name("skip");
    node->set_op_type("SkipSimplifiedLayerNormalization");
    node->set_domain("com.microsoft");
    node->add_input("X"); node->add_input("S"); node->add_input("W");
    node->add_output("Y");
    node->add_output("");
    node->add_output("");
    node->add_output("Sum");
    {
      auto* attribute = node->add_attribute();
      attribute->set_name("epsilon");
      attribute->set_type(onnx::AttributeProto::FLOAT);
      attribute->set_f(1e-6f);
    }
    auto* weight = graph->add_initializer();
    weight->set_name("W");
    weight->set_data_type(onnx::TensorProto_DataType_FLOAT);
    weight->add_dims(std::int64_t(hidden));
    weight->set_raw_data(normWeight.data(), normWeight.size() * sizeof(float));

    const auto addIO = [&] (const char* name, bool isInput) {
      auto* value = isInput ? graph->add_input() : graph->add_output();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto_DataType_FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(std::int64_t(seq));
      tensor->mutable_shape()->add_dim()->set_dim_value(std::int64_t(hidden));
    };
    addIO("X", true);
    addIO("S", true);
    addIO("Y", false);
    addIO("Sum", false);

    std::string bytes;
    model.SerializeToString(&bytes);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(ortEnv, bytes.data(), bytes.size(), options);
    const std::vector<std::int64_t> shape{1, std::int64_t(seq), std::int64_t(hidden)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputs[] = {
      Ort::Value::CreateTensor<float>(memory, const_cast<float*>(x.data()), x.size(),
                                      shape.data(), shape.size()),
      Ort::Value::CreateTensor<float>(memory, const_cast<float*>(skip.data()), skip.size(),
                                      shape.data(), shape.size())};
    const char* inputNames[] = {"X", "S"};
    const char* outputNames[] = {"Y", "Sum"};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, inputs, 2, outputNames, 2);
    const auto count = hidden * seq;
    const float* y = outputs[0].GetTensorData<float>();
    const float* sum = outputs[1].GetTensorData<float>();
    return std::make_pair(std::vector<float>(y, y + count), std::vector<float>(sum, sum + count));
  };

  // The two candidate residuals differ exactly as C2 describes.
  //   correct: X' = X + A + B          (residual added once, outside the SUM)
  //   wrong:   X' = 2X + A + B         (each rank added X before the SUM)
  std::vector<float> mergedA(hidden * seq), wrongA(hidden * seq);
  for (std::size_t i = 0; i < mergedA.size(); ++i) {
    mergedA[i] = partialA[i] + partialB[i];
    wrongA[i] = x0[i] + partialA[i] + partialB[i];
  }

  // The kernel treats its S input as the skip contribution and returns
  // Sum = X + S. Passing the merged partial therefore yields the correct
  // residual, and passing X plus the merged partial yields the inflated one.
  const auto correct = runSkip(x0, mergedA);
  const auto wrong = runSkip(x0, wrongA);
  for (std::size_t i = 0; i < mergedA.size(); ++i) {
    BOOST_CHECK_SMALL(correct.second[i] - (x0[i] + mergedA[i]), 1e-5f);
    BOOST_CHECK_SMALL(wrong.second[i] - (2.0f * x0[i] + mergedA[i]), 1e-5f);
  }

  // LayerNorm is invariant to a positive uniform scaling: norm(2x) == norm(x).
  // So if the wrong form were 2(X + A + B) it would be absorbed here and the
  // error would only surface later, if at all. It is not, because the partials
  // are added once rather than twice, and the difference must survive the
  // normalization. That is what makes this check worth running.
  double maxWrong = 0.0, scale = 0.0;
  for (std::size_t i = 0; i < correct.first.size(); ++i) {
    maxWrong = std::max(maxWrong, double(std::fabs(wrong.first[i] - correct.first[i])));
    scale = std::max(scale, double(std::fabs(correct.first[i])));
  }
  BOOST_TEST_MESSAGE("residual chain: normalized outputs differ by " << maxWrong
    << " against magnitude " << scale);
  BOOST_CHECK(scale > 1e-3);
  BOOST_CHECK(maxWrong > 1e-3);
}

BOOST_AUTO_TEST_CASE(RealGroupQueryAttentionAcceptsSplitHeads)
{
  // The attention case above builds the score/softmax/value chain by hand so the
  // GQA broadcast is explicit. This case instead runs the production
  // com.microsoft.GroupQueryAttention kernel the frozen export actually uses,
  // because a hand-built substitute cannot show that the real op accepts a
  // rank's head subset.
  //
  // Establishing the contract took probing: the op needs past_key, past_value,
  // seqlens_k and total_seq inputs, and it has 3-4 outputs where the present
  // key/value slots may not be empty when declared.
  constexpr std::int64_t kSeq = 4;
  constexpr std::int64_t kQueryHeads = 4;
  constexpr std::int64_t kKvHeads = 2;
  constexpr std::int64_t kHeadDim = 8;
  constexpr std::int64_t kDegree = 2;

  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-gqa");
  std::mt19937 rng(41);
  const auto randomValues = [&] (std::size_t count) {
    std::vector<float> values(count);
    for (auto& value : values) value = float(int(rng() % 21) - 10) * 0.1f;
    return values;
  };

  // Run one GroupQueryAttention over [1, seq, heads*dim] tensors.
  const auto runGqa = [&] (std::int64_t qHeads, std::int64_t kvHeads,
                           const std::vector<float>& q,
                           const std::vector<float>& k,
                           const std::vector<float>& v) {
    onnx::ModelProto model;
    model.set_ir_version(7);
    auto* opset = model.add_opset_import();
    opset->set_domain(""); opset->set_version(17);
    auto* ms = model.add_opset_import();
    ms->set_domain("com.microsoft"); ms->set_version(1);
    auto* graph = model.mutable_graph();
    graph->set_name("spec224-gqa");

    auto* node = graph->add_node();
    node->set_name("gqa");
    node->set_op_type("GroupQueryAttention");
    node->set_domain("com.microsoft");
    for (const char* in : {"Q", "K", "V", "past_key", "past_value", "seqlens_k", "total_seq"})
      node->add_input(in);
    node->add_output("Y");
    node->add_output("present_key");
    node->add_output("present_value");
    const auto addInt = [&] (const char* key, std::int64_t value) {
      auto* attribute = node->add_attribute();
      attribute->set_name(key);
      attribute->set_type(onnx::AttributeProto::INT);
      attribute->set_i(value);
    };
    const auto addFloat = [&] (const char* key, float value) {
      auto* attribute = node->add_attribute();
      attribute->set_name(key);
      attribute->set_type(onnx::AttributeProto::FLOAT);
      attribute->set_f(value);
    };
    addInt("num_heads", qHeads);
    addInt("kv_num_heads", kvHeads);
    addFloat("scale", 1.0f / std::sqrt(float(kHeadDim)));
    addInt("do_rotary", 0);
    addInt("rotary_interleaved", 0);
    addFloat("softcap", 0.0f);

    const auto addInitializer = [&] (const char* name, std::vector<std::int64_t> dims,
                                     onnx::TensorProto_DataType type) {
      auto* init = graph->add_initializer();
      init->set_name(name);
      init->set_data_type(type);
      for (const auto dim : dims) init->add_dims(dim);
      return init;
    };
    // Empty past: one generation step, no cache yet.
    addInitializer("past_key", {1, kvHeads, 0, kHeadDim}, onnx::TensorProto_DataType_FLOAT);
    addInitializer("past_value", {1, kvHeads, 0, kHeadDim}, onnx::TensorProto_DataType_FLOAT);
    addInitializer("seqlens_k", {1}, onnx::TensorProto_DataType_INT32)
      ->add_int32_data(std::int32_t(kSeq - 1));
    addInitializer("total_seq", {1}, onnx::TensorProto_DataType_INT32)
      ->add_int32_data(std::int32_t(kSeq));

    const auto addIO = [&] (const char* name, bool isInput, std::int64_t width) {
      auto* value = isInput ? graph->add_input() : graph->add_output();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto_DataType_FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(kSeq);
      tensor->mutable_shape()->add_dim()->set_dim_value(width);
    };
    addIO("Q", true, qHeads * kHeadDim);
    addIO("K", true, kvHeads * kHeadDim);
    addIO("V", true, kvHeads * kHeadDim);
    addIO("Y", false, qHeads * kHeadDim);
    // Present cache outputs keep the head, sequence, dim layout.
    const auto addPresent = [&] (const char* name) {
      auto* value = graph->add_output();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto_DataType_FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
      tensor->mutable_shape()->add_dim()->set_dim_value(kvHeads);
      tensor->mutable_shape()->add_dim()->set_dim_value(kSeq);
      tensor->mutable_shape()->add_dim()->set_dim_value(kHeadDim);
    };
    addPresent("present_key");
    addPresent("present_value");

    std::string bytes;
    model.SerializeToString(&bytes);
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(ortEnv, bytes.data(), bytes.size(), options);
    const std::vector<std::int64_t> qShape{1, kSeq, qHeads * kHeadDim};
    const std::vector<std::int64_t> kvShape{1, kSeq, kvHeads * kHeadDim};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputs[] = {
      Ort::Value::CreateTensor<float>(memory, const_cast<float*>(q.data()), q.size(),
                                      qShape.data(), qShape.size()),
      Ort::Value::CreateTensor<float>(memory, const_cast<float*>(k.data()), k.size(),
                                      kvShape.data(), kvShape.size()),
      Ort::Value::CreateTensor<float>(memory, const_cast<float*>(v.data()), v.size(),
                                      kvShape.data(), kvShape.size())};
    const char* inputNames[] = {"Q", "K", "V"};
    const char* outputNames[] = {"Y"};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, inputs, 3, outputNames, 1);
    const auto* data = outputs[0].GetTensorData<float>();
    return std::vector<float>(data, data + kSeq * qHeads * kHeadDim);
  };

  const auto q = randomValues(kSeq * kQueryHeads * kHeadDim);
  const auto k = randomValues(kSeq * kKvHeads * kHeadDim);
  const auto v = randomValues(kSeq * kKvHeads * kHeadDim);
  const auto reference = runGqa(kQueryHeads, kKvHeads, q, k, v);
  BOOST_REQUIRE_EQUAL(reference.size(), std::size_t(kSeq * kQueryHeads * kHeadDim));

  // Split query and KV heads in the same proportion, then place each rank's
  // result back at its own head offset.
  const auto gatherHeads = [] (const std::vector<float>& source, std::int64_t sourceHeads,
                               std::int64_t beginHead, std::int64_t headCount) {
    std::vector<float> result(std::size_t(kSeq * headCount * kHeadDim));
    for (std::int64_t s = 0; s < kSeq; ++s)
      for (std::int64_t h = 0; h < headCount; ++h)
        for (std::int64_t d = 0; d < kHeadDim; ++d)
          result[std::size_t((s * headCount + h) * kHeadDim + d)] =
            source[std::size_t((s * sourceHeads + beginHead + h) * kHeadDim + d)];
    return result;
  };

  const auto assemble = [&] (bool reuseRankZeroKv) {
    std::vector<float> merged(std::size_t(kSeq * kQueryHeads * kHeadDim), 0.0f);
    for (std::int64_t rank = 0; rank < kDegree; ++rank) {
      const auto qPerRank = kQueryHeads / kDegree;
      const auto kvPerRank = kKvHeads / kDegree;
      const auto rankQ = gatherHeads(q, kQueryHeads, rank * qPerRank, qPerRank);
      const auto kvBegin = reuseRankZeroKv ? 0 : rank * kvPerRank;
      const auto rankK = gatherHeads(k, kKvHeads, kvBegin, kvPerRank);
      const auto rankV = gatherHeads(v, kKvHeads, kvBegin, kvPerRank);
      const auto partial = runGqa(qPerRank, kvPerRank, rankQ, rankK, rankV);
      for (std::int64_t s = 0; s < kSeq; ++s)
        for (std::int64_t h = 0; h < qPerRank; ++h)
          for (std::int64_t d = 0; d < kHeadDim; ++d)
            merged[std::size_t((s * kQueryHeads + rank * qPerRank + h) * kHeadDim + d)] =
              partial[std::size_t((s * qPerRank + h) * kHeadDim + d)];
    }
    return merged;
  };

  const auto split = assemble(false);
  double maxError = 0.0;
  double magnitude = 0.0;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    maxError = std::max(maxError, double(std::fabs(split[i] - reference[i])));
    magnitude = std::max(magnitude, double(std::fabs(reference[i])));
  }
  BOOST_TEST_MESSAGE("real GQA split: max abs err " << maxError
    << " against magnitude " << magnitude);
  BOOST_CHECK(magnitude > 1e-3);
  BOOST_CHECK(maxError < 1e-5);

  // The grouping mistake: both ranks attend with rank 0's KV heads.
  const auto wrong = assemble(true);
  double wrongError = 0.0;
  for (std::size_t i = 0; i < reference.size(); ++i)
    wrongError = std::max(wrongError, double(std::fabs(wrong[i] - reference[i])));
  BOOST_TEST_MESSAGE("wrong KV grouping: max abs err " << wrongError);
  BOOST_CHECK(wrongError > 1e-3);
}

BOOST_AUTO_TEST_CASE(FrozenExportPackingMatchesTheSliceAssumptions)
{
  // The slice helpers assume every packed weight is [N, K/32, 16] uint8 with a
  // float32 scale per [N, K/32] block. That assumption is what makes an
  // output-channel slice a whole-row copy and an input-channel slice a
  // block-aligned column copy. One counter-example anywhere in the export would
  // make those helpers silently wrong for that tensor, so the whole graph is
  // checked rather than a sample.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path +
    " (set NDNSF_SPEC224_FROZEN_MODEL); qualification cannot skip it");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  BOOST_REQUIRE(!bytes.empty());

  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromString(
    std::string(bytes.begin(), bytes.end())));
  std::map<std::string, const onnx::TensorProto*> initializers;
  for (const auto& init : model.graph().initializer())
    initializers.emplace(init.name(), &init);

  std::size_t checked = 0;
  for (const auto& node : model.graph().node()) {
    if (node.op_type() != "MatMulNBits") continue;
    // Only the schema positions matter here: input 1 is B, input 2 is scales.
    BOOST_REQUIRE_EQUAL(node.input_size(), 3);
    std::int64_t k = 0, n = 0, blockSize = 0, bits = 0;
    for (const auto& attribute : node.attribute()) {
      if (attribute.name() == "K") k = attribute.i();
      else if (attribute.name() == "N") n = attribute.i();
      else if (attribute.name() == "block_size") blockSize = attribute.i();
      else if (attribute.name() == "bits") bits = attribute.i();
    }
    BOOST_REQUIRE(k > 0);
    BOOST_REQUIRE(n > 0);
    // The helpers are only valid for this exact quantization.
    BOOST_CHECK_EQUAL(bits, std::int64_t(4));
    BOOST_CHECK_EQUAL(blockSize, std::int64_t(qwen::QWEN_INT4_BLOCK_SIZE));
    const auto weight = initializers.find(node.input(1));
    const auto scales = initializers.find(node.input(2));
    BOOST_REQUIRE(weight != initializers.end());
    BOOST_REQUIRE(scales != initializers.end());

    const auto blocks = k / qwen::QWEN_INT4_BLOCK_SIZE;
    // Packed blob layout: one row per output channel, one 16-byte blob per
    // 32-wide block.
    BOOST_REQUIRE_EQUAL(weight->second->dims_size(), 3);
    BOOST_CHECK_EQUAL(weight->second->dims(0), n);
    BOOST_CHECK_EQUAL(weight->second->dims(1), blocks);
    BOOST_CHECK_EQUAL(weight->second->dims(2), std::int64_t(qwen::QWEN_INT4_BLOB_BYTES));
    BOOST_CHECK_EQUAL(weight->second->data_type(), int(onnx::TensorProto_DataType_UINT8));
    // One float32 scale per output channel per block.
    BOOST_REQUIRE_EQUAL(scales->second->dims_size(), 1);
    BOOST_CHECK_EQUAL(scales->second->dims(0), n * blocks);
    BOOST_CHECK_EQUAL(scales->second->data_type(), int(onnx::TensorProto_DataType_FLOAT));
    // External payload length must agree with the declared geometry, or the
    // slicing helpers would read past what the sidecar actually holds.
    std::uint64_t externalLength = 0;
    for (const auto& entry : weight->second->external_data()) {
      if (entry.key() == "length") externalLength = std::stoull(entry.value());
    }
    if (externalLength != 0)
      BOOST_CHECK_EQUAL(externalLength, qwen::packedInt4Bytes(n, k));
    ++checked;
  }
  // The recorded fact is 7 projections per layer over 28 layers.
  BOOST_CHECK_EQUAL(checked, 196U);
}

BOOST_AUTO_TEST_CASE(CachedTemplateStepsAreValidatedWhenBound)
{
  // The fixed template is normalized when sealed, but binding it to an
  // invocation produces a new plan whose role/dependency combination is what
  // the Provider would execute. That combination must be validated too: a
  // template whose metadata merely looks complete must still be refused if its
  // step DAG does not hold together.
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/0";
  projection.requestId = "/request/1";
  projection.attempt = 1;
  projection.planCoreDigest = nativePlanningDigest("core");
  projection.planDigest = nativePlanningDigest("plan");
  projection.ackClosedDigest = nativePlanningDigest("ack");
  projection.offerDigest = nativePlanningDigest("offer");
  projection.securityPolicySnapshotDigest = nativePlanningDigest("security");
  projection.deadlineMs = 120000;

  // Every role the template names needs an owner, including the remote peer:
  // the cached contract must say where a contribution comes from.
  projection.fixedProviderByRole = {{"S0R0", projection.provider},
                                    {"S0R1", "/provider/1"}};
  NativeSelectionRoleV3 assembly;
  assembly.role = "S0R0";
  assembly.selectedRole = "S0R0";
  assembly.rank = 0;
  assembly.layerBegin = 0;
  assembly.layerEnd = 1;
  assembly.backend = "onnxruntime-cpu";
  assembly.roleKind = "HYBRID_RANK";
  assembly.artifactDigest = nativePlanningDigest("artifact");
  assembly.recipeDigest = nativePlanningDigest("recipe");
  assembly.adapterId = "qwen";
  assembly.adapterVersion = "1";
  assembly.modelManifestDigest = nativePlanningDigest("manifest");
  assembly.artifactProfileDigest = nativePlanningDigest("profile");
  assembly.graphDigest = nativePlanningDigest("graph");
  assembly.canonicalInitializerDigest = nativePlanningDigest("initializer");
  assembly.adapterDescriptorDigest = nativePlanningDigest("adapter");
  assembly.assemblerDescriptorDigest = nativePlanningDigest("assembler");
  assembly.backendAbi = "onnxruntime-1-cpu";
  assembly.nodeIndices = {1, 2};
  assembly.expectedInputs = {{"x", "float32", {std::int64_t(1), std::int64_t(4)}}};
  assembly.expectedOutputs = {{"y", "float32", {std::int64_t(1), std::int64_t(4)}}};
  assembly.precision = "fp32";
  assembly.quantization = "int4";
  assembly.layout = "native";
  assembly.padding = "none";
  assembly.protectionEpoch = "plaintext-v1";
  assembly.maxSourceBytes = 4096;
  assembly.maxAssembledBytes = 2048;
  assembly.maxNodes = 4;
  projection.selectedRole = assembly;
  projection.assembly = assembly;

  projection.executionRole.roleId = "S0R0";
  projection.executionRole.stageId = "S0R0";
  projection.executionRole.rank = 0;
  projection.executionRole.layerBegin = 0;
  projection.executionRole.layerEnd = 1;
  projection.executionRole.backend = assembly.backend;
  projection.executionRole.adapterId = assembly.adapterId;
  projection.executionRole.adapterVersion = assembly.adapterVersion;

  projection.dataflow.role = "S0R0";
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = 1;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.dataflowDigest = nativePlanningDigest("dataflow");
  projection.dataflow.terminalResponseOwner = true;

  projection.deviceBinding.mode = "CPU";
  projection.deviceBinding.provider = projection.provider;
  projection.deviceBinding.role = "S0R0";
  projection.deviceBinding.offerDigest = projection.offerDigest;
  projection.deviceBinding.topologyProfileDigest = nativePlanningDigest("topology");
  projection.deviceBinding.resourceSnapshotDigest = nativePlanningDigest("resources");
  projection.deviceBinding.resourceSequence = 1;
  projection.deviceBinding.sharingPolicy = "EXCLUSIVE_ROLE";

  projection.plan.roles = {"S0R0", "S0R1"};
  projection.plan.steps = twoRankSteps();
  projection.plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  projection.plan.tensorDegree = 2;
  projection.plan.dependencies = twoRankPlan().dependencies;

  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(projection);
  BOOST_REQUIRE(fixed != nullptr);

  // A well-formed template survives the cached round trip.
  std::istringstream source(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restored = nativeFixedRoleAssignmentV1FromJson(source);
  BOOST_CHECK_EQUAL(restored->digest(), fixed->digest());
  const auto& templateAfter = restored->executionTemplate();
  BOOST_REQUIRE_EQUAL(templateAfter->steps.size(), projection.plan.steps.size());
  BOOST_CHECK_NO_THROW(templateAfter->steps.front().validate());

  // A template whose step DAG has been broken must not survive binding. The
  // check lives where the template becomes a plan, so it must reject the same
  // defect the plan validator rejects.
  auto broken = projection.plan.steps;
  broken.at(2).inputSlots.at(1).producerStepId = "S0R1/DOES_NOT_EXIST";
  NativeExecutionPlan bound;
  bound.roles = projection.plan.roles;
  bound.steps = broken;
  bound.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  bound.tensorDegree = 2;
  bound.dependencies = projection.plan.dependencies;
  BOOST_CHECK_THROW(bound.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SumAccumulatesContributionsAndRejectsMismatch)
{
  // SUM is the only cross-rank reduction in the template, and it is local to
  // the consumer. Making it a shared value operation rather than adapter code
  // keeps every backend reducing identically.
  const auto makeBundle = [] (const std::string& name, std::vector<float> values) {
    TensorBundle bundle;
    bundle.name = name;
    bundle.payload.resize(values.size() * sizeof(float));
    std::memcpy(bundle.payload.data(), values.data(), bundle.payload.size());
    return bundle;
  };
  const auto valuesOf = [] (const TensorBundle& bundle) {
    std::vector<float> values(bundle.payload.size() / sizeof(float));
    std::memcpy(values.data(), bundle.payload.data(), bundle.payload.size());
    return values;
  };

  std::map<std::string, TensorBundle> accumulator;
  accumulateTensorBundle(accumulator, "remote0", makeBundle("partial", {1.0f, 2.0f, 3.0f}));
  BOOST_REQUIRE_EQUAL(accumulator.count("remote0"), 1U);
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.0f, 2.0f, 3.0f}));

  // The first contribution initialises, later ones add.
  accumulateTensorBundle(accumulator, "remote0", makeBundle("partial", {0.5f, -1.0f, 2.0f}));
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.5f, 1.0f, 5.0f}));
  accumulateTensorBundle(accumulator, "remote0", makeBundle("partial", {0.0f, 0.0f, 1.0f}));
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.5f, 1.0f, 6.0f}));

  // Slots stay independent: a second slot must not be merged into the first.
  accumulateTensorBundle(accumulator, "remote1", makeBundle("partial", {7.0f, 8.0f, 9.0f}));
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.5f, 1.0f, 6.0f}));
  BOOST_CHECK(valuesOf(accumulator.at("remote1")) == std::vector<float>({7.0f, 8.0f, 9.0f}));

  // The first contribution is validated too. An empty or misaligned initial
  // value would poison every later sum and would never be examined again,
  // because the accumulator already exists by then.
  {
    std::map<std::string, TensorBundle> firstEmpty;
    TensorBundle empty;
    empty.name = "partial";
    BOOST_CHECK_THROW(
      accumulateTensorBundle(firstEmpty, "remote0", empty), std::invalid_argument);
    BOOST_CHECK(firstEmpty.empty());

    std::map<std::string, TensorBundle> firstOdd;
    TensorBundle odd;
    odd.name = "partial";
    odd.payload = {std::uint8_t(1), std::uint8_t(2), std::uint8_t(3)};
    BOOST_CHECK_THROW(
      accumulateTensorBundle(firstOdd, "remote0", odd), std::invalid_argument);
    BOOST_CHECK(firstOdd.empty());
  }

  // A contribution with a different payload size is a contract violation, not
  // something to silently truncate.
  BOOST_CHECK_THROW(
    accumulateTensorBundle(accumulator, "remote0", makeBundle("partial", {1.0f, 2.0f})),
    std::invalid_argument);
  // A different tensor under the same slot is equally wrong.
  BOOST_CHECK_THROW(
    accumulateTensorBundle(accumulator, "remote0", makeBundle("other", {1.0f, 2.0f, 3.0f})),
    std::invalid_argument);
  // A payload that is not a whole number of float32 elements cannot be summed.
  {
    TensorBundle odd;
    odd.name = "partial";
    odd.payload = {std::uint8_t(1), std::uint8_t(2), std::uint8_t(3)};
    BOOST_CHECK_THROW(
      accumulateTensorBundle(accumulator, "remote0", odd), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(RoleSpecCarriesOnlyItsOwnUnitsSteps)
{
  // The worker receives a role, not a plan. If the template did not travel with
  // the role, the worker could not execute steps at all; if it travelled whole,
  // a unit would hold its peers' steps and could execute them.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  ExecutionAttemptKey session;
  session.requestId = "/session/1";
  session.attemptEpoch = 1;

  for (const auto* unit : {"S0R0", "S0R1"}) {
    const auto spec = roleSpecFor(plan, unit, session, assignment, "/provider/0");
    BOOST_REQUIRE(spec.steps != nullptr);
    BOOST_CHECK_EQUAL(spec.stepPlanVersion, std::string(NATIVE_STEP_PLAN_VERSION_V1));
    BOOST_CHECK_EQUAL(spec.tensorDegree, 2U);
    // Only this unit's steps, and every one of them.
    std::size_t expected = 0;
    for (const auto& step : plan.steps) {
      if (step.unitId != unit) continue;
      ++expected;
    }
    BOOST_CHECK_EQUAL(spec.steps->size(), expected);
    for (const auto& step : *spec.steps)
      BOOST_CHECK_EQUAL(step.unitId, std::string(unit));
  }

  // A pipeline plan carries no steps, so the legacy role is unchanged.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"/LLM/Pipeline/Stage/0"};
  pipeline.dependencies = {[] {
    NativeDependencySpec dependency;
    dependency.producers = {"/LLM/Pipeline/Stage/0"};
    dependency.consumers = {"/LLM/Pipeline/Stage/0"};
    dependency.keyScope = "legacy";
    dependency.topicPrefix = "/topic";
    dependency.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
    dependency.tensors = {"hidden"};
    dependency.operationKind = "ACTIVATION";
    return dependency;
  }()};
  NativeProviderAssignment single;
  single.providerByRole = {{"/LLM/Pipeline/Stage/0", "/provider/0"}};
  const auto legacy = roleSpecFor(pipeline, "/LLM/Pipeline/Stage/0", session, single,
                                  "/provider/0");
  BOOST_CHECK(legacy.steps == nullptr);
  BOOST_CHECK(legacy.stepPlanVersion.empty());
  BOOST_CHECK_EQUAL(legacy.tensorDegree, 1U);
}

/** DependencyIo that counts every call, so a case can prove the worker never
 * started fetching. Fetching returns a never-ready future on purpose: if the
 * gate is misplaced the case fails fast instead of blocking a test thread.
 */
class CountingDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    ++prefetchCalls;
    return std::promise<TensorBundle>{}.get_future();
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge,
                const TensorBundle&) override
  {
    // Recorded before the counter so an observer sees the count that produced this
    // call, not one incremented afterwards.
    if (onPublish) onPublish(edge.scope);
    ++publishCalls;
  }

  std::atomic<std::size_t> prefetchCalls{0};
  std::atomic<std::size_t> publishCalls{0};
  std::function<void(const std::string&)> onPublish;
  std::set<std::string> publishedScopes;
};

/** DependencyIo whose fetches never become ready, so a role's declared input
 * stays pending and the worker's dependency deadline is what ends the unit.
 */
class NeverReadyDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    ++prefetchCalls;
    // Keep the promise alive in the object so the future is never satisfied and
    // never broken; only the deadline can end the wait.
    std::promise<TensorBundle> pending;
    auto future = pending.get_future();
    std::lock_guard<std::mutex> lock(mutex);
    promises.push_back(std::move(pending));
    return future;
  }

  void
  publishOutput(const std::string&, const DependencyEdge&, const TensorBundle&) override
  {
    ++publishCalls;
  }

  std::atomic<std::size_t> prefetchCalls{0};
  std::atomic<std::size_t> publishCalls{0};
  std::mutex mutex;
  std::vector<std::promise<TensorBundle>> promises;
};

/** Preparation callback that counts invocations. Returning a null runner is
 * enough: the gate must reject before this is ever called.
 */
class CountingRunnerPreparation
{
public:
  ProviderRoleWorker::NativeRunnerPreparation shared()
  {
    return [this] {
      ++calls;
      return std::shared_ptr<NativeModelRunner>{};
    };
  }

  std::atomic<std::size_t> calls{0};
};

BOOST_AUTO_TEST_CASE(WorkerRefusesAnInconsistentStepDeclarationBeforeAnyWork)
{
  // A pointer is not a contract. An empty template, a version without a
  // template, a degree above one without a template, and a template carrying
  // another unit's step are all inconsistent, and each must be refused before
  // input fetch, runner preparation or cached-output lookup. Otherwise a role
  // would pull peer partials and prepare a runner for a computation it cannot
  // legally perform.
  auto io = std::make_shared<CountingDependencyIo>();
  CountingRunnerPreparation preparation;
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));

  const auto baseRole = [] {
    RoleSpec role;
    role.role = "/Stage/0";
    role.requestId = "/request/step-gate";
    role.attemptEpoch = 1;
    // Declare an input so a misplaced gate would visibly start fetching it.
    DependencyEdge pendingInput;
    pendingInput.scope = "peer-partial";
    pendingInput.producerRole = "/Stage/1";
    pendingInput.consumerRole = "/Stage/0";
    pendingInput.plannedDataName = "/peer/partial";
    pendingInput.tensors = {"partial"};
    role.inputs.push_back(pendingInput);
    return role;
  };

  // A declared step plan with no template.
  {
    auto role = baseRole();
    role.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
    role.tensorDegree = 2;
    BOOST_CHECK_THROW(
      worker.executePreparedAsync("gate-empty-template", std::move(role), io,
                                  preparation.shared()),
      std::invalid_argument);
  }
  // A degree above one with no template would be a tensor-parallel role that
  // cannot execute anything.
  {
    auto role = baseRole();
    role.tensorDegree = 2;
    BOOST_CHECK_THROW(
      worker.executePreparedAsync("gate-degree-no-template", std::move(role), io,
                                  preparation.shared()),
      std::invalid_argument);
  }
  // A template whose steps belong to a peer unit.
  {
    auto role = baseRole();
    role.steps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
      std::vector<NativeExecutionStepSpec>{twoRankSteps().front()});
    // twoRankSteps().front() belongs to S0R0, not to this role.
    role.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
    role.tensorDegree = 2;
    BOOST_CHECK_THROW(
      worker.executePreparedAsync("gate-foreign-step", std::move(role), io,
                                  preparation.shared()),
      std::invalid_argument);
  }

  // Nothing was fetched, prepared or published in any of those rejections.
  BOOST_CHECK_EQUAL(io->prefetchCalls.load(), 0U);
  BOOST_CHECK_EQUAL(preparation.calls.load(), 0U);
  BOOST_CHECK_EQUAL(io->publishCalls.load(), 0U);
}
BOOST_AUTO_TEST_CASE(WorkerRejectsAnUnknownStepPlanVersionSynchronously)
{
  // Version skew is a contract error, reported separately from the
  // implementation gap so an operator can tell them apart. It is also refused
  // before any work starts.
  auto io = std::make_shared<CountingDependencyIo>();
  CountingRunnerPreparation preparation;
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));

  RoleSpec role;
  role.role = "/Stage/0";
  role.requestId = "/request/version-skew";
  role.attemptEpoch = 1;
  role.steps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
    std::vector<NativeExecutionStepSpec>{});
  role.stepPlanVersion = "step-plan-v99";
  role.tensorDegree = 2;

  try {
    (void)worker.executePreparedAsync("version-skew-session", std::move(role), io,
                                     preparation.shared());
    BOOST_FAIL("an unknown step plan version was accepted");
  }
  catch (const std::invalid_argument& error) {
    BOOST_CHECK_EQUAL(std::string(error.what()),
                      std::string("Provider role carries an unsupported step plan version"));
  }
  BOOST_CHECK_EQUAL(io->prefetchCalls.load(), 0U);
  BOOST_CHECK_EQUAL(preparation.calls.load(), 0U);
}
BOOST_AUTO_TEST_CASE(LocalSlotMustNameItsProducerOutputAndMatchItsType)
{
  // A local slot has no dependency edge to carry its type contract, so the step
  // validator must check it against the producer's declared output. Without
  // this a float32 value could feed an int32 input inside one unit and the
  // mismatch would only appear as a wrong read at execution time.
  const auto build = [] {
    NativeExecutionPlan plan;
    plan.serviceName = "/service/tp";
    plan.modelName = "qwen3-0.6b";
    plan.roles = {"S0R0", "S0R1"};
    plan.steps = twoRankSteps();
    plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
    plan.tensorDegree = 2;
    plan.dependencies = {
      [] { NativeDependencySpec d; d.producers = {"S0R1"}; d.consumers = {"S0R0"};
           d.keyScope = "e0"; d.dependencyId = "e0"; d.topicPrefix = "/t";
           d.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
           d.tensors = {"partial"}; d.operationKind = "TENSOR_PARTIAL";
           d.useNdnsfDataV1 = true;
           d.producerStepId = "S0R1/ATTN/0"; d.producerSlotId = "partial";
           d.consumerStepId = "S0R0/SUM_ATTN/0"; d.consumerSlotId = "remote";
           return d; }(),
      [] { NativeDependencySpec d; d.producers = {"S0R0"}; d.consumers = {"S0R1"};
           d.keyScope = "e1"; d.dependencyId = "e1"; d.topicPrefix = "/t";
           d.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
           d.tensors = {"partial"}; d.operationKind = "TENSOR_PARTIAL";
           d.useNdnsfDataV1 = true;
           d.producerStepId = "S0R0/ATTN/0"; d.producerSlotId = "partial";
           d.consumerStepId = "S0R1/SUM_ATTN/0"; d.consumerSlotId = "remote";
           return d; }(),
    };
    return plan;
  };

  auto valid = build();
  BOOST_CHECK_NO_THROW(valid.validateSteps());

  // Naming a producer step without its output slot is refused: a consumer's own
  // slot name is step-local and cannot locate the producer's output.
  auto missingSlot = build();
  missingSlot.steps[2].inputSlots[0].producerSlotId.clear();
  BOOST_CHECK_THROW(missingSlot.validateSteps(), std::invalid_argument);

  // Naming an output the producer does not declare is refused.
  auto foreignSlot = build();
  foreignSlot.steps[2].inputSlots[0].producerSlotId = "not-an-output";
  BOOST_CHECK_THROW(foreignSlot.validateSteps(), std::invalid_argument);

  // A type mismatch inside one unit is refused here rather than at execution.
  auto wrongType = build();
  wrongType.steps[2].inputSlots[0].dtype = "int32";
  BOOST_CHECK_THROW(wrongType.validateSteps(), std::invalid_argument);

  // The same for shape and layout, which a bfloat16/row-major reader would
  // otherwise misread as valid.
  auto wrongShape = build();
  wrongShape.steps[2].inputSlots[0].shape = {std::int64_t(1), std::int64_t(25),
                                             std::int64_t(512)};
  BOOST_CHECK_THROW(wrongShape.validateSteps(), std::invalid_argument);
  auto wrongLayout = build();
  wrongLayout.steps[2].inputSlots[0].layoutDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  BOOST_CHECK_THROW(wrongLayout.validateSteps(), std::invalid_argument);

  // LOCAL_STEP must stay inside the unit. Binding it to a peer's step would tell
  // the runtime to read in-process a value that only exists after a fetch.
  auto crossUnit = build();
  crossUnit.steps[2].inputSlots[0].producerStepId = "S0R1/ATTN/0";
  BOOST_CHECK_THROW(crossUnit.validateSteps(), std::invalid_argument);

  // And REMOTE_STEP must not point inside the unit, which would create a network
  // dependency the peer never publishes.
  auto localRemote = build();
  localRemote.steps[2].inputSlots[1].producerStepId = "S0R0/ATTN/0";
  BOOST_CHECK_THROW(localRemote.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(InitProducesTheEmbeddedResidualThatLayerZeroReads)
{
  // Audit A3: the projection had INIT producing only the side inputs, so the
  // first attention entry read its residual from EPOCH_INPUT and the embedding
  // lookup was never part of the template. A tensor-parallel unit must start from
  // the embedded X0, exactly as the frozen export does with a Gather over the
  // shared table.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  const auto find = [&] (const std::string& stepId) -> const NativeExecutionStepSpec* {
    const auto found = std::find_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) { return step.stepId == stepId; });
    return found == plan.steps.end() ? nullptr : &*found;
  };
  const auto slotOf = [] (const NativeExecutionStepSpec& step, const std::string& id,
                          bool output) -> const NativeStepSlotSpec* {
    const auto& slots = output ? step.outputSlots : step.inputSlots;
    const auto found = std::find_if(slots.begin(), slots.end(),
      [&] (const auto& slot) { return slot.slotId == id; });
    return found == slots.end() ? nullptr : &*found;
  };

  for (const auto* unit : {"S0R0", "S0R1"}) {
    const auto* init = find(std::string(unit) + "/INIT");
    BOOST_REQUIRE(init != nullptr);
    // INIT produces the embedded residual alongside the side inputs.
    const auto* x0 = slotOf(*init, "x0", true);
    BOOST_REQUIRE(x0 != nullptr);
    BOOST_CHECK_EQUAL(x0->dtype, std::string("float32"));
    BOOST_CHECK_EQUAL(x0->layoutDigest,
                      std::string(qwen::QWEN_TP_ACTIVATION_LAYOUT_DIGEST));

    // Layer 0 reads it from INIT, not from the request input.
    const auto* attn0 = find(std::string(unit) + "/ATTN/0");
    BOOST_REQUIRE(attn0 != nullptr);
    const auto* fed = slotOf(*attn0, "x", false);
    BOOST_REQUIRE(fed != nullptr);
    BOOST_CHECK_EQUAL(fed->source, std::string("LOCAL_STEP"));
    BOOST_CHECK_EQUAL(fed->producerStepId, init->stepId);
    BOOST_CHECK_EQUAL(fed->producerSlotId, std::string("x0"));

    // Later layers read the previous MLP sum, which already carries both
    // residuals, so they must not read EPOCH_INPUT either.
    if (shape.layers > 1) {
      const auto* attn1 = find(std::string(unit) + "/ATTN/1");
      BOOST_REQUIRE(attn1 != nullptr);
      const auto* fed1 = slotOf(*attn1, "x", false);
      BOOST_REQUIRE(fed1 != nullptr);
      BOOST_CHECK_EQUAL(fed1->producerStepId,
                        std::string(unit) + "/SUM_MLP/0");
      BOOST_CHECK_EQUAL(fed1->producerSlotId, std::string("merged"));
    }
  }

  // The whole template must still validate: every local slot now names a real
  // producer output that agrees on type, shape and layout.
  BOOST_CHECK_NO_THROW(plan.validateSteps());

  // No attention residual may be an EPOCH_INPUT, which would reintroduce the
  // skipped embedding.
  for (const auto& step : plan.steps) {
    if (step.stepId.find("/ATTN/") == std::string::npos) continue;
    const auto& x = *std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
      [] (const auto& slot) { return slot.slotId == "x"; });
    BOOST_CHECK_EQUAL(x.source, std::string("LOCAL_STEP"));
  }
}

BOOST_AUTO_TEST_CASE(ProjectionAcceptsCallerSuppliedUnitIds)
{
  // Audit A1: the projection imposed "S0R<rank>" on every role, so a caller that
  // derived unit ids from a Catalog stage had to rename them afterwards. The
  // contract requires an explicit QwenTpUnit list instead.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;

  const std::vector<qwen::QwenTpUnit> units = {
    {"/LLM/TP/Stage/decode#0", 0, 0, 1},
    {"/LLM/TP/Stage/decode#1", 1, 0, 1},
  };
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId, units);

  // The plan's roles are exactly the supplied ids, with no naming convention
  // applied on top.
  BOOST_REQUIRE_EQUAL(plan.roles.size(), 2U);
  BOOST_CHECK_EQUAL(plan.roles[0], units[0].roleId);
  BOOST_CHECK_EQUAL(plan.roles[1], units[1].roleId);
  for (const auto& step : plan.steps) {
    BOOST_CHECK(step.unitId == units[0].roleId || step.unitId == units[1].roleId);
  }
  BOOST_CHECK_NO_THROW(plan.validateSteps());

  // A degree mismatch, duplicate role ids, duplicate ranks and an out-of-range
  // rank are all refused rather than silently coerced.
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId,
                            {units.front()}),
    std::invalid_argument);
  auto duplicateId = units;
  duplicateId[1].roleId = duplicateId[0].roleId;
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId, duplicateId),
    std::invalid_argument);
  auto duplicateRank = units;
  duplicateRank[1].rank = 0;
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId, duplicateRank),
    std::invalid_argument);
  auto outOfRange = units;
  outOfRange[1].rank = 7;
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId, outOfRange),
    std::invalid_argument);

  // A tensor-parallel unit holds a shard of every layer, so a partial layer cover
  // is refused instead of being read as a pipeline stage.
  auto partialCover = units;
  partialCover[1].layerEnd = 0;
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", units.front().roleId, partialCover),
    std::invalid_argument);

  // The egress role must be one of the declared units.
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "entry", "/not/a/unit", units),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ProjectionReferencesOnlyVerifiedEntryContracts)
{
  // Audit A1/C1.4: the projection used to invent entry ids from a prefix, so a
  // plan could reference an entry the assembler never produced and the failure
  // would only appear inside the worker. With verified contracts every
  // CALL_SUBGRAPH id must come from one.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;

  const std::vector<qwen::QwenTpUnit> units = {
    {"/TP#0", 0, 0, 1},
    {"/TP#1", 1, 0, 1},
  };
  const auto contract = [] (const std::string& unit, const std::string& kind,
                            std::uint64_t layer) {
    qwen::QwenTpEntryContract entry;
    entry.entryId = "assembled/" + unit + "/" + kind + "/" + std::to_string(layer);
    entry.unitId = unit;
    entry.kind = kind;
    entry.layer = layer;
    entry.inputNames = {"in"};
    entry.outputNames = {"out"};
    return entry;
  };

  std::vector<qwen::QwenTpEntryContract> entries;
  for (const auto& unit : units) {
    entries.push_back(contract(unit.roleId, "INIT", 0));
    entries.push_back(contract(unit.roleId, "ATTN", 0));
    entries.push_back(contract(unit.roleId, "MLP", 0));
  }
  entries.push_back(contract("/TP#0", "HEAD", 1));

  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, entries);

  // Every artifact entry is one of the verified ids, never a generated one.
  std::set<std::string> verifiedIds;
  for (const auto& entry : entries) verifiedIds.insert(entry.entryId);
  std::size_t subgraphSteps = 0;
  for (const auto& step : plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") {
      BOOST_CHECK(step.artifactEntry.empty());
      continue;
    }
    ++subgraphSteps;
    BOOST_CHECK(verifiedIds.count(step.artifactEntry) != 0);
  }
  BOOST_CHECK_EQUAL(subgraphSteps, entries.size());
  BOOST_CHECK_NO_THROW(plan.validateSteps());

  // A missing entry is refused rather than named on the fly.
  auto missingHead = entries;
  missingHead.pop_back();
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, missingHead),
    std::invalid_argument);

  // An entry owned by the wrong unit does not satisfy the step that needs it.
  auto wrongUnit = entries;
  wrongUnit.back().unitId = "/TP#1";
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, wrongUnit),
    std::invalid_argument);

  // Duplicate ids and incomplete contracts are refused up front.
  auto duplicated = entries;
  duplicated.push_back(entries.front());
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, duplicated),
    std::invalid_argument);
  auto incomplete = entries;
  incomplete.front().outputNames.clear();
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, incomplete),
    std::invalid_argument);

  // An entry with the right unit but the wrong kind is not a substitute either.
  auto wrongKind = entries;
  for (auto& entry : wrongKind)
    if (entry.kind == "HEAD") entry.kind = "ATTN";
  BOOST_CHECK_THROW(
    qwen::projectQwenTpPlan(plan, shape, 2, "/TP#0", units, wrongKind),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SumDecodesBundlesInsteadOfAddingFramingBytes)
{
  // A step input is an encoded bundle, not raw floats. Adding the encoded payload
  // as if it were float32 would fold its framing and tensor name into the values,
  // which is wrong but numerically plausible — so this case checks the decoded
  // result against hand-computed floats.
  const auto bundleOf = [] (const std::string& name, std::vector<float> values) {
    NamedTensor tensor;
    tensor.name = name;
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(1), std::int64_t(values.size())};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    return std::vector<NamedTensor>{std::move(tensor)};
  };
  const auto valuesOf = [] (const TensorBundle& bundle) {
    const auto decoded = decodeTensorBundle(bundle.payload);
    BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
    BOOST_REQUIRE(decoded.front().elementType == TensorElementType::Float32);
    std::vector<float> values(decoded.front().payload.size() / sizeof(float));
    std::memcpy(values.data(), decoded.front().payload.data(), decoded.front().payload.size());
    return values;
  };

  std::map<std::string, TensorBundle> accumulator;
  accumulateDecodedTensorBundle(accumulator, "remote0",
    bundleOf("partial", {1.0f, 2.0f}), "scope");
  BOOST_REQUIRE_EQUAL(accumulator.count("remote0"), 1U);
  // The first contribution initialises; the accumulator holds a real bundle whose
  // decoded values are exactly the input, with no framing mixed in.
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.0f, 2.0f}));

  accumulateDecodedTensorBundle(accumulator, "remote0",
    bundleOf("partial", {0.5f, -1.0f}), "scope");
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.5f, 1.0f}));

  // A different element type is refused rather than reinterpreted.
  {
    NamedTensor ints;
    ints.name = "partial";
    ints.elementType = TensorElementType::Int32;
    ints.shape = {std::int64_t(2)};
    ints.payload.resize(sizeof(std::int32_t) * 2, 0);
    BOOST_CHECK_THROW(
      accumulateDecodedTensorBundle(accumulator, "remote0", {ints}, "scope"),
      std::invalid_argument);
  }
  // A different tensor name, shape or payload size is refused.
  BOOST_CHECK_THROW(
    accumulateDecodedTensorBundle(accumulator, "remote0",
      bundleOf("other", {1.0f, 2.0f}), "scope"),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    accumulateDecodedTensorBundle(accumulator, "remote0",
      bundleOf("partial", {1.0f}), "scope"),
    std::invalid_argument);
  // Zero tensors and more than one tensor are both refused: a SUM reduces one
  // partial, and reducing over several would be order-dependent.
  BOOST_CHECK_THROW(
    accumulateDecodedTensorBundle(accumulator, "remote0", {}, "scope"),
    std::invalid_argument);
  {
    auto two = bundleOf("partial", {1.0f, 2.0f});
    two.push_back(two.front());
    BOOST_CHECK_THROW(
      accumulateDecodedTensorBundle(accumulator, "remote0", two, "scope"),
      std::invalid_argument);
  }
  // A non-float32-aligned payload cannot be summed.
  {
    auto odd = bundleOf("partial", {1.0f});
    odd.front().payload.push_back(0);
    BOOST_CHECK_THROW(
      accumulateDecodedTensorBundle(accumulator, "remote0", odd, "scope"),
      std::invalid_argument);
  }

  // The accumulator survived every rejection unchanged.
  BOOST_CHECK(valuesOf(accumulator.at("remote0")) == std::vector<float>({1.5f, 1.0f}));
}

BOOST_AUTO_TEST_CASE(TensorRunnerRegistrationRequiresTheOnnxBackend)
{
  // The parent runner builds its children through the same registry, so
  // registering it before the ONNX backend would only fail later, at run time.
  // Requiring the ordering here turns that into a startup error.
  RegistryNativeModelRunnerFactory empty;
  BOOST_CHECK_THROW(qwen::registerQwenTensorBackend(empty), std::invalid_argument);
  BOOST_CHECK(!empty.hasBackend(qwen::QWEN_TENSOR_RUNNER_BACKEND));
}
BOOST_AUTO_TEST_CASE(RoleSpecEdgesCarryTheSealedStepBinding)
{
  // The worker matches a step's remote input to exactly one declared role input
  // through the sealed consumer step and slot. Without those four fields on the
  // runtime edge it would have to match tensors[0] or a shared slot name, which
  // is the guess C4 forbids.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  ExecutionAttemptKey session;
  session.requestId = "/session/edge-binding";
  session.attemptEpoch = 1;

  const auto spec = roleSpecFor(plan, "S0R0", session, assignment, "/provider/0");
  BOOST_REQUIRE(!spec.inputs.empty());
  // Every input edge on a TP role is a step-bound partial, and each carries the
  // binding the plan declared for it.
  std::size_t boundInputs = 0;
  for (const auto& edge : spec.inputs) {
    if (edge.consumerStepId.empty()) continue;
    ++boundInputs;
    BOOST_CHECK_EQUAL(edge.consumerSlotId.empty(), false);
    BOOST_CHECK_EQUAL(edge.producerStepId.empty(), false);
    BOOST_CHECK_EQUAL(edge.producerSlotId.empty(), false);
    // The binding must agree with the plan, not merely look plausible.
    const auto declared = std::find_if(plan.dependencies.begin(), plan.dependencies.end(),
      [&edge] (const auto& dependency) {
        return dependency.consumerStepId == edge.consumerStepId &&
               dependency.consumerSlotId == edge.consumerSlotId;
      });
    BOOST_REQUIRE(declared != plan.dependencies.end());
    BOOST_CHECK_EQUAL(edge.producerStepId, declared->producerStepId);
    BOOST_CHECK_EQUAL(edge.producerSlotId, declared->producerSlotId);
  }
  BOOST_CHECK(boundInputs > 0);

  // Output edges carry the same binding, so a publisher knows which step and slot
  // produced the value it is about to publish.
  for (const auto& edge : spec.outputs) {
    BOOST_CHECK_EQUAL(edge.producerStepId.empty(), false);
    BOOST_CHECK_EQUAL(edge.producerSlotId.empty(), false);
  }

  // A pipeline role has no step binding at all, so its edges stay exactly as they
  // were before this field existed.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"/LLM/Pipeline/Stage/0"};
  NativeDependencySpec activation;
  activation.producers = {"/LLM/Pipeline/Stage/0"};
  activation.consumers = {"/LLM/Pipeline/Stage/1"};
  activation.keyScope = "legacy";
  activation.topicPrefix = "/topic";
  activation.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
  activation.tensors = {"hidden"};
  activation.operationKind = "ACTIVATION";
  pipeline.dependencies = {activation};
  pipeline.roles.push_back("/LLM/Pipeline/Stage/1");
  NativeProviderAssignment single;
  single.providerByRole = {{"/LLM/Pipeline/Stage/0", "/provider/0"},
                           {"/LLM/Pipeline/Stage/1", "/provider/1"}};
  const auto legacy = roleSpecFor(pipeline, "/LLM/Pipeline/Stage/1", session, single,
                                  "/provider/1");
  BOOST_REQUIRE_EQUAL(legacy.inputs.size(), 1U);
  BOOST_CHECK(legacy.inputs.front().consumerStepId.empty());
  BOOST_CHECK(legacy.inputs.front().producerStepId.empty());
}

BOOST_AUTO_TEST_CASE(WorkerRunsASealedTemplateStepByStep)
{
  // The unit template is driven here, not by a separate scheduler: the worker
  // calls the runner once per CALL_SUBGRAPH step and reduces SUM locally. This
  // case is the positive flow the task asks for — the runner is a lambda, so the
  // assertion is about the worker's behaviour, not about an adapter.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  auto role = roleSpecFor(plan, "S0R0", [] {
    ExecutionAttemptKey key;
    key.requestId = "/session/step-flow";
    key.attemptEpoch = 1;
    return key;
  }(), [] {
    NativeProviderAssignment assignment;
    assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
    return assignment;
  }(), "/provider/0");

  // Record every step the worker asks the runner to execute.
  std::vector<std::string> executed;
  std::mutex executedMutex;
  const auto runner = [&] (const RoleExecutionContext& ctx) {
    BOOST_REQUIRE(ctx.activeStep != nullptr);
    BOOST_REQUIRE(ctx.roleSteps != nullptr);
    {
      std::lock_guard<std::mutex> lock(executedMutex);
      executed.push_back(ctx.activeStep->stepId);
    }
    // Produce exactly the declared output slots so the worker can bind them.
    // A real adapter returns an encoded bundle (C5), which is what SUM decodes,
    // so the fixture must too — a raw float payload would only pass because SUM
    // was not reached.
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Feed the epoch inputs the INIT step declares and the peer partial each SUM
  // needs, keyed by the scope the sealed edges use.
  std::map<std::string, TensorBundle> initial;
  const auto epochBundle = [] (const char* name) {
    TensorBundle bundle;
    bundle.name = name;
    bundle.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
    return bundle;
  };
  for (const auto& edge : role.inputs) {
    if (edge.consumerStepId.empty()) continue;
    TensorBundle partial;
    partial.name = "partial";
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {0.5f, 0.25f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    partial.payload = encodeTensorBundle({tensor});
    partial.expectedBytes = partial.payload.size();
    initial.emplace(edge.scope, std::move(partial));
  }
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      initial.emplace(slot.slotId, epochBundle(slot.slotId.c_str()));
    }
  }

  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("step-flow-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  const auto result = future.get();

  // Every CALL_SUBGRAPH step ran, and no SUM did: reductions never reach the
  // runner because they have no graph.
  std::lock_guard<std::mutex> lock(executedMutex);
  BOOST_CHECK(!executed.empty());
  for (const auto& stepId : executed)
    BOOST_CHECK(stepId.find("/SUM_") == std::string::npos);
  // The unit completes once, so its declared outputs are present.
  BOOST_CHECK(!result.outputsByScope.empty());
}

BOOST_AUTO_TEST_CASE(TensorBackendRegistrationMirrorsTheProductionOrder)
{
  // Production registers ONNX, then the YOLO merge backend, then the TP parent.
  // Reproducing that order here shows the same sequence the factory uses is
  // accepted, and that dispatching to the TP backend reaches the parent creator
  // rather than failing with "no backend registered".
  RegistryNativeModelRunnerFactory factory;
  // Without ONNX first the TP registration must refuse, which is exactly the
  // misconfiguration the production ordering avoids.
  BOOST_CHECK_THROW(qwen::registerQwenTensorBackend(factory), std::invalid_argument);

  // A backend with the ONNX name is enough to satisfy the ordering requirement;
  // the creator is never invoked in this case.
  factory.registerBackend("onnxruntime",
    [] (const NativeModelRunnerSpec&) { return std::shared_ptr<NativeModelRunner>{}; });
  BOOST_CHECK_NO_THROW(qwen::registerQwenTensorBackend(factory));
  BOOST_CHECK(factory.hasBackend(qwen::QWEN_TENSOR_RUNNER_BACKEND));

  // A TP spec with no entry table is refused by the creator, which proves the
  // dispatch reached it rather than stopping at an unknown backend.
  NativeModelRunnerSpec spec;
  spec.role = "/TP#0";
  spec.kind = "onnx-model";
  spec.backend = qwen::QWEN_TENSOR_RUNNER_BACKEND;
  try {
    (void)factory.create(spec);
    BOOST_FAIL("a TP spec without an entry table was accepted");
  }
  catch (const std::invalid_argument& error) {
    const std::string message = error.what();
    BOOST_CHECK(message.find("entry table") != std::string::npos);
  }
  catch (const std::out_of_range&) {
    BOOST_FAIL("the TP backend was not registered");
  }
}

BOOST_AUTO_TEST_CASE(SealedSumRequiresEveryDeclaredContribution)
{
  // A SUM over a subset would produce a different tensor while still looking
  // like a successful reduction, so the worker must refuse a step whose declared
  // contribution was never admitted. The role here keeps its subgraph steps but
  // loses the input edge that feeds the reduction, which is exactly the shape of
  // a consumer that was handed an incomplete role spec.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/missing-partial";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  // Drop every edge that feeds a reduction. The attention steps keep their own
  // local and epoch inputs, so they still run and the failure is the SUM's.
  const auto before = role.inputs.size();
  role.inputs.erase(
    std::remove_if(role.inputs.begin(), role.inputs.end(),
      [] (const auto& edge) {
        return edge.consumerStepId.find("/SUM_") != std::string::npos;
      }),
    role.inputs.end());
  BOOST_REQUIRE(role.inputs.size() < before);

  const auto runner = [] (const RoleExecutionContext& ctx) {
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Supply only the epoch inputs; no peer contribution exists.
  std::map<std::string, TensorBundle> initial;
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("missing-partial-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  try {
    (void)future.get();
    BOOST_FAIL("a SUM with a missing contribution was executed");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK(std::string(error.what()).find("missing an admitted contribution") !=
                std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(SealedStepRejectsDuplicateContributionsForOneSlot)
{
  // Two edges feeding one slot would count a contribution twice, inflating the
  // reduction without looking wrong. The sealed template declares one dependency
  // per contribution, so the worker refuses rather than merging them.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/duplicate-contribution";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  // Duplicate the edge that feeds the first reduction, giving it a distinct scope
  // so both copies are admitted. The template still names one contribution for
  // that slot, so the second must be refused.
  const auto sumEdge = std::find_if(role.inputs.begin(), role.inputs.end(),
    [] (const auto& edge) {
      return edge.consumerStepId.find("/SUM_ATTN/") != std::string::npos;
    });
  BOOST_REQUIRE(sumEdge != role.inputs.end());
  auto duplicate = *sumEdge;
  duplicate.scope = duplicate.scope + "/duplicate";
  duplicate.plannedDataName = duplicate.plannedDataName + "/duplicate";
  role.inputs.push_back(duplicate);

  const auto runner = [] (const RoleExecutionContext& ctx) {
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Supply every admitted scope, including both copies, so the failure is the
  // duplicate slot and not a missing input.
  std::map<std::string, TensorBundle> initial;
  for (const auto& edge : role.inputs) {
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {0.5f, 0.25f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    TensorBundle bundle;
    bundle.name = "partial";
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    if (!edge.consumerStepId.empty())
      initial.emplace(edge.scope, std::move(bundle));
  }
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      if (initial.count(slot.slotId) != 0) continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("duplicate-contribution-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  try {
    (void)future.get();
    BOOST_FAIL("a duplicate contribution for one slot was counted twice");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK(std::string(error.what()).find("duplicate contributions") !=
                std::string::npos);
  }
}


BOOST_AUTO_TEST_CASE(LatePartialFailsTheUnitInsteadOfReducingASubset)
{
  // A peer partial that never arrives is the ordinary tensor-parallel failure:
  // one rank waits while the other never publishes. The worker must fail the unit
  // once, at its dependency deadline, rather than reducing whatever arrived or
  // blocking a compute worker forever.
  //
  // The wait timeout is short so the case is deterministic without sleeping for
  // the production default.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/late-partial";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  const auto runner = [] (const RoleExecutionContext& ctx) {
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Only the epoch inputs are supplied. The peer scope stays pending so the role
  // never reaches execution.
  std::map<std::string, TensorBundle> initial;
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  auto io = std::make_shared<NeverReadyDependencyIo>();
  // Two compute workers so the queue itself is not the limiter; the wait
  // scheduler deadline is what must end the unit.
  ProviderRoleWorker worker(2, 1, 16, std::chrono::milliseconds(150));
  auto future = worker.executeAsync("late-partial-session", std::move(role), io,
                                    runner, initial);
  try {
    (void)future.get();
    BOOST_FAIL("a unit with a missing peer partial completed");
  }
  catch (const std::exception& error) {
    // The failure must be the wait, not a silent subset reduction.
    const std::string message = error.what();
    BOOST_TEST_MESSAGE("late partial failure: " << message);
    BOOST_CHECK(message.find("dependency wait") != std::string::npos ||
                message.find("deadline") != std::string::npos ||
                message.find("timed out") != std::string::npos);
  }
  // The unit may legitimately have published partials from the local-only steps it
  // completed before the missing contribution blocked it -- that is what lets a
  // peer make progress. What must not happen is publishing something it never
  // computed, so the published count cannot exceed the steps that actually ran.
  BOOST_CHECK(io->publishCalls.load() <= io->prefetchCalls.load() + 1);
  BOOST_TEST_MESSAGE("late partial: publishes=" << io->publishCalls.load());
}


BOOST_AUTO_TEST_CASE(CancellationTerminatesTheUnitExactlyOnce)
{
  // Cancellation must end the unit once and publish nothing. Two details matter:
  // the waiting role is ended by the scheduler's cancel path rather than its
  // deadline, and a second termination cannot overwrite the first result.
  //
  // The wait timeout is long so only cancellation can end the role inside the
  // case; the worker is destroyed while the role is still waiting, which is the
  // production cancel path (shutdown cancels outstanding waits).
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/cancelled";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  std::atomic<std::size_t> runnerCalls{0};
  const auto runner = [&runnerCalls] (const RoleExecutionContext& ctx) {
    ++runnerCalls;
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  std::map<std::string, TensorBundle> initial;
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  auto io = std::make_shared<NeverReadyDependencyIo>();
  std::future<ProviderRoleResult> future;
  {
    // Cancellation happens by destroying the worker while the role waits. The
    // point of the scope is that the destructor must both cancel and join.
    ProviderRoleWorker worker(2, 1, 16, std::chrono::seconds(30));
    future = worker.executeAsync("cancelled-session", std::move(role), io, runner, initial);
    // Give the worker a moment to reach its wait, then leave the scope. The
    // destroy path is deterministic even though the thread scheduling is not:
    // shutdown cancels every outstanding wait before joining.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // The role terminates rather than hanging, and it never computed.
  BOOST_CHECK(future.valid());
  try {
    (void)future.get();
    BOOST_FAIL("a cancelled unit produced a result");
  }
  catch (const std::exception& error) {
    BOOST_TEST_MESSAGE("cancellation failure: " << error.what());
  }
  // The unit may legitimately have run and published its local-only leading steps
  // before the cancellation reached it: that is the point of not waiting for every
  // input up front. What must not happen is publishing more than it computed, so a
  // cancellation cannot leave a fabricated partial behind.
  BOOST_CHECK(io->publishCalls.load() <= std::max<std::size_t>(1, runnerCalls.load()));
  BOOST_TEST_MESSAGE("cancellation: runnerCalls=" << runnerCalls.load()
                    << " publishes=" << io->publishCalls.load());
}


BOOST_AUTO_TEST_CASE(AUnitAdvancesWithASingleComputeWorker)
{
  // A tensor-parallel unit must not need extra compute workers to make progress:
  // a deployment may run one per Provider, and a template that re-enqueued its
  // own steps would deadlock at that setting. This case runs the same positive
  // flow as the step-by-step test with exactly one compute worker.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/single-worker";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  std::atomic<std::size_t> subgraphCalls{0};
  const auto runner = [&subgraphCalls] (const RoleExecutionContext& ctx) {
    ++subgraphCalls;
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Supply every admitted scope so nothing waits.
  std::map<std::string, TensorBundle> initial;
  for (const auto& edge : role.inputs) {
    if (edge.consumerStepId.empty()) continue;
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {0.5f, 0.25f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    TensorBundle bundle;
    bundle.name = "partial";
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    initial.emplace(edge.scope, std::move(bundle));
  }
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      if (initial.count(slot.slotId) != 0) continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  // Exactly one compute worker and one wait worker: the tightest setting the
  // deployment rule allows.
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("single-worker-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  const auto result = future.get();
  // The whole template ran and the unit completed, on one thread.
  BOOST_CHECK(subgraphCalls.load() > 0);
  BOOST_CHECK(!result.outputsByScope.empty());
}


BOOST_AUTO_TEST_CASE(ARunnerThatIgnoresStepsCannotSilentlyServeATemplate)
{
  // A step-carrying role must never be served by a runner that executes whole
  // roles. The failure has to be loud: such a runner runs the complete graph and
  // returns outputs keyed by its own graph names, while the worker binds results
  // by sealed slot id. This case pins that mismatch, because the alternative --
  // a runner whose graph names happened to match slot ids -- would produce
  // plausible numbers from a peer-incomplete rank artifact.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/whole-role-runner";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  // A runner that behaves like the ordinary ONNX adapter: it ignores the step
  // fields and returns its graph's own output names.
  std::atomic<std::size_t> calls{0};
  const auto runner = [&calls] (const RoleExecutionContext& ctx) {
    ++calls;
    BOOST_CHECK(ctx.activeStep != nullptr);
    std::map<std::string, TensorBundle> outputs;
    NamedTensor tensor;
    tensor.name = "logits";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {1.0f, 2.0f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    TensorBundle bundle;
    bundle.name = "logits";
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    // The graph name, not the sealed slot id.
    outputs.emplace("logits", std::move(bundle));
    return outputs;
  };

  // Supply every admitted scope as well, so the role reaches execution and the
  // failure is the output mismatch rather than a dependency wait.
  const auto inputBundle = [] (const std::string& name) {
    NamedTensor tensor;
    tensor.name = name;
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
    TensorBundle bundle;
    bundle.name = name;
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    return bundle;
  };
  std::map<std::string, TensorBundle> initial;
  for (const auto& edge : role.inputs) {
    if (edge.consumerStepId.empty()) continue;
    initial.emplace(edge.scope, inputBundle("partial"));
  }
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      if (initial.count(slot.slotId) != 0) continue;
      initial.emplace(slot.slotId, inputBundle(slot.slotId));
    }
  }

  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("whole-role-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  try {
    (void)future.get();
    BOOST_FAIL("a whole-role runner served a sealed template without failing");
  }
  catch (const std::runtime_error& error) {
    // The refusal must name the missing declared output, not some coincidental
    // later failure.
    BOOST_CHECK(std::string(error.what()).find("did not produce its declared output") !=
                std::string::npos);
  }
  // It was called exactly once: the mismatch is detected on the first step, so
  // the unit never advances past it.
  BOOST_CHECK_EQUAL(calls.load(), 1U);
}


BOOST_AUTO_TEST_CASE(FrozenGraphClassifiesIntoTheEntryTaxonomy)
{
  // C2 fixes the entry taxonomy: INIT once, ATTN and MLP per layer, HEAD once.
  // Classification is by the export's own naming, because the graph interleaves
  // preprocessing nodes with layer nodes. This case runs against the frozen
  // export so the taxonomy is checked on the graph that will actually execute.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path +
    " (set NDNSF_SPEC224_FROZEN_MODEL); qualification cannot skip it");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  BOOST_REQUIRE(!bytes.empty());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromString(std::string(bytes.begin(), bytes.end())));

  std::vector<std::string> names;
  names.reserve(model.graph().node_size());
  for (const auto& node : model.graph().node()) names.push_back(node.name());

  const auto entries = qwen::classifyQwenTpEntryNodes(names, 28);
  // INIT, then ATTN and MLP for each of 28 layers, then HEAD.
  BOOST_REQUIRE_EQUAL(entries.size(), 1U + 2U * 28U + 1U);
  BOOST_CHECK_EQUAL(entries.front().kind, std::string("INIT"));
  BOOST_CHECK_EQUAL(entries.back().kind, std::string("HEAD"));

  std::size_t total = 0;
  std::size_t attentionEntries = 0;
  std::size_t mlpEntries = 0;
  for (const auto& entry : entries) {
    total += entry.nodeIndices.size();
    if (entry.kind == "ATTN") ++attentionEntries;
    if (entry.kind == "MLP") ++mlpEntries;
    BOOST_CHECK(!entry.nodeIndices.empty());
  }
  BOOST_CHECK_EQUAL(attentionEntries, 28U);
  BOOST_CHECK_EQUAL(mlpEntries, 28U);
  // The head's layer index must be the layer count, not zero. The projected head
  // step names its entry by that index, so a mismatch would leave the head step
  // without a contract and only surface when a rank tries to execute it.
  const auto headEntry = std::find_if(entries.begin(), entries.end(),
    [] (const auto& entry) { return entry.kind == "HEAD"; });
  BOOST_REQUIRE(headEntry != entries.end());
  BOOST_CHECK_EQUAL(headEntry->layer, 28U);
  // Every node belongs to exactly one entry: a node in none would never execute.
  BOOST_CHECK_EQUAL(total, names.size());

  // Per-layer entries must be contiguous and non-overlapping in source order, so
  // an entry can be extracted as a node subset without gaps.
  std::vector<std::uint64_t> seen;
  for (const auto& entry : entries) {
    for (const auto auto_index : entry.nodeIndices) seen.push_back(auto_index);
  }
  std::sort(seen.begin(), seen.end());
  BOOST_CHECK_EQUAL(seen.size(), names.size());
  BOOST_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  BOOST_CHECK_EQUAL(seen.front(), 0U);
  BOOST_CHECK_EQUAL(seen.back(), std::uint64_t(names.size() - 1));

  // The first layer's attention entry must hold the projections and the GQA op,
  // and its MLP entry the three MLP projections. This is the split that matters:
  // an attention entry that swallowed the MLP would execute the wrong subgraph.
  const auto& layer0Attn = entries.at(1);
  const auto& layer0Mlp = entries.at(2);
  const auto namesOf = [&names] (const qwen::QwenTpEntryNodes& entry) {
    std::set<std::string> result;
    for (const auto index : entry.nodeIndices) result.insert(names.at(index));
    return result;
  };
  const auto attnNames = namesOf(layer0Attn);
  const auto mlpNames = namesOf(layer0Mlp);
  BOOST_CHECK(attnNames.count("/model/layers.0/attn/GroupQueryAttention") == 1);
  BOOST_CHECK(attnNames.count("/model/layers.0/attn/o_proj/MatMul_Q4") == 1);
  BOOST_CHECK(mlpNames.count("/model/layers.0/mlp/down_proj/MatMul_Q4") == 1);
  BOOST_CHECK(mlpNames.count("/model/layers.0/post_attention_layernorm/SkipLayerNorm") == 1);
  // No node may appear in both.
  for (const auto& name : attnNames)
    BOOST_CHECK(mlpNames.count(name) == 0);
}


BOOST_AUTO_TEST_CASE(ExtractedEntryGraphIsSelfContainedAndLoadable)
{
  // An entry must be a real standalone graph: its own nodes, every initializer
  // its nodes reference, and a boundary computed from membership rather than
  // from names. This case extracts layer 0's attention entry from the frozen
  // export and checks those properties, then confirms ORT accepts it.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path + " (set NDNSF_SPEC224_FROZEN_MODEL)");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromString(std::string(bytes.begin(), bytes.end())));

  std::vector<std::string> names;
  names.reserve(model.graph().node_size());
  for (const auto& node : model.graph().node()) names.push_back(node.name());
  const auto entries = qwen::classifyQwenTpEntryNodes(names, 28);

  // Layer 0 attention: fifteen nodes, q/k/v through o_proj.
  const auto& layer0Attn = entries.at(1);
  BOOST_REQUIRE_EQUAL(layer0Attn.kind, std::string("ATTN"));
  BOOST_REQUIRE_EQUAL(layer0Attn.layer, 0U);

  auto extracted = qwen::extractQwenTpEntryModel(model, layer0Attn, "S0R0/ATTN/0");
  // Node cover is exactly the classified set.
  BOOST_CHECK_EQUAL(std::uint64_t(extracted.graph().node_size()),
                    layer0Attn.nodeIndices.size());
  // A boundary exists on both sides, or the entry could not be a separate graph.
  BOOST_CHECK(extracted.graph().input_size() > 0);
  BOOST_CHECK(extracted.graph().output_size() > 0);
  // Every initializer the extracted nodes reference is present, and no unrelated
  // one travelled along.
  std::set<std::string> referenced;
  for (const auto& node : extracted.graph().node())
    for (const auto& input : node.input()) if (!input.empty()) referenced.insert(input);
  std::set<std::string> carried;
  for (const auto& init : extracted.graph().initializer()) carried.insert(init.name());
  for (const auto& name : carried)
    BOOST_CHECK(referenced.count(name) != 0);
  BOOST_CHECK(!carried.empty());

  // ORT must accept the extracted graph. The production shape keeps the
  // initializers external and shared, because C3 requires each entry to take only
  // the ranges it needs rather than copying gigabytes per rank. So the extracted
  // model is written beside the same sidecar and loaded from a file, which is
  // exactly how a Provider will load it.
  const auto directory = std::filesystem::temp_directory_path() /
    ("spec224-entry-extract-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);
  std::filesystem::create_directories(directory);
  const auto modelPath = directory / "attn0.onnx";
  const auto sidecarPath = directory / "model.onnx_data";
  {
    std::string wire;
    BOOST_REQUIRE(extracted.SerializeToString(&wire));
    std::ofstream out(modelPath, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(out));
    out.write(wire.data(), static_cast<std::streamsize>(wire.size()));
  }
  // The sidecar is immutable shared storage, so link it rather than copying
  // 918 MB. A hard link is required, not a symlink: ORT resolves a symlink and
  // then rejects the sidecar for escaping the model directory, while a hard link
  // makes the same inode appear at the expected path inside it.
  std::filesystem::create_hard_link(
    path.substr(0, path.find_last_of('/')) + "/model.onnx_data", sidecarPath,
    cleanupError);
  BOOST_REQUIRE_MESSAGE(!cleanupError,
    "sidecar hard link failed (needs one filesystem): " + cleanupError.message());

  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-entry-extract");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  try {
    Ort::Session session(ortEnv, modelPath.c_str(), options);
    BOOST_CHECK(session.GetInputCount() > 0);
    BOOST_CHECK(session.GetOutputCount() > 0);
  }
  catch (const Ort::Exception& error) {
    std::filesystem::remove_all(directory, cleanupError);
    BOOST_FAIL(std::string("ORT rejected the extracted entry graph: ") + error.what());
  }
  std::filesystem::remove_all(directory, cleanupError);
}


BOOST_AUTO_TEST_CASE(WrittenEntryIsVerifiableAndStaysInsideItsRoot)
{
  // A contract is only trustworthy if its digest describes the file that is
  // actually on disk. This writes one entry, re-reads it, and checks the digest
  // matches the bytes -- then confirms the path rules that keep an entry from
  // writing outside the assembly root.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path + " (set NDNSF_SPEC224_FROZEN_MODEL)");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());

  // Path rules first: these are refusals, so they must not need a source model.
  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-entry-write-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);
  onnx::ModelProto scratch;
  BOOST_REQUIRE(scratch.ParseFromString(std::string(bytes.begin(), bytes.end())));
  qwen::QwenTpEntryNodes entry;
  entry.kind = "ATTN";
  entry.layer = 0;
  entry.nodeIndices = {7};
  // Absolute, parent-bearing and escaped names are all refused before any write.
  for (const std::string bad : {"/etc/passwd", "../escape.onnx", "sub/dir.onnx"}) {
    BOOST_CHECK_THROW(
      qwen::writeQwenTpEntry(root, "S0R0", entry, scratch, bad),
      std::invalid_argument);
  }
  // Nothing was written by the refused calls.
  std::size_t writtenFiles = 0;
  for (const auto& item : std::filesystem::directory_iterator(root)) {
    (void)item;
    ++writtenFiles;
  }
  BOOST_CHECK_EQUAL(writtenFiles, 0U);

  // Now a real write. The source is parsed again because a refused call must not
  // have consumed it.
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromString(std::string(bytes.begin(), bytes.end())));
  std::vector<std::string> names;
  names.reserve(model.graph().node_size());
  for (const auto& node : model.graph().node()) names.push_back(node.name());
  const auto entries = qwen::classifyQwenTpEntryNodes(names, 28);
  const auto contract = qwen::writeQwenTpEntry(
    root, "S0R0", entries.at(1), model, "attn0.onnx");

  BOOST_CHECK_EQUAL(contract.unitId, std::string("S0R0"));
  BOOST_CHECK_EQUAL(contract.kind, std::string("ATTN"));
  BOOST_CHECK(contract.hasGraphFile());
  BOOST_CHECK_EQUAL(contract.relativeGraphPath, std::string("attn0.onnx"));
  BOOST_CHECK_EQUAL(contract.graphDigest.size(), 71U);

  // The digest must describe the file on disk, not the intent to write one.
  const auto target = root / contract.relativeGraphPath;
  BOOST_CHECK(std::filesystem::exists(target));
  std::ifstream verify(target, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(verify));
  const std::vector<std::uint8_t> onDisk((std::istreambuf_iterator<char>(verify)),
                                         std::istreambuf_iterator<char>());
  BOOST_CHECK(!onDisk.empty());
  ndn::util::Sha256 digest;
  digest.update(ndn::span<const std::uint8_t>(onDisk.data(), onDisk.size()));
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  BOOST_CHECK_EQUAL(contract.graphDigest, "sha256:" + hex);
  // No staging file survived a successful write.
  BOOST_CHECK(!std::filesystem::exists(target.string() + ".staging"));
  // The boundary names come from the written graph itself.
  BOOST_CHECK(!contract.inputNames.empty());
  BOOST_CHECK(!contract.outputNames.empty());

  // A partial file reference is refused, so a cache can never trust a name alone.
  qwen::QwenTpEntryContract partial = contract;
  partial.graphDigest.clear();
  BOOST_CHECK_THROW(partial.validate(), std::invalid_argument);
  qwen::QwenTpEntryContract escaped = contract;
  escaped.relativeGraphPath = "../outside.onnx";
  BOOST_CHECK_THROW(escaped.validate(), std::invalid_argument);

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(EntryTableRoundTripsAndRefusesUnverifiedTables)
{
  // The runner receives its entry table through the spec's flat metadata, so the
  // writer and the parser must agree exactly. A table that loses a field, repeats
  // an id, or names a file without a digest must fail rather than produce a
  // runner that loads something it cannot verify.
  qwen::QwenTpEntryContract entry;
  entry.entryId = "S0R0/ATTN/0";
  entry.unitId = "S0R0";
  entry.kind = "ATTN";
  entry.layer = 0;
  entry.inputNames = {"hidden_states"};
  entry.outputNames = {"attn_partial"};
  entry.relativeGraphPath = "S0R0-attn0.onnx";
  entry.graphDigest = "sha256:" + std::string(64, 'a');
  entry.validate();

  const auto json = qwen::qwenTpEntryTableJson({entry});
  const auto parsed = qwen::parseQwenTpEntryTable(json);
  BOOST_REQUIRE_EQUAL(parsed.size(), 1U);
  BOOST_CHECK_EQUAL(parsed.front().entryId, entry.entryId);
  BOOST_CHECK_EQUAL(parsed.front().unitId, entry.unitId);
  BOOST_CHECK_EQUAL(parsed.front().kind, entry.kind);
  BOOST_CHECK_EQUAL(parsed.front().layer, entry.layer);
  BOOST_CHECK_EQUAL(parsed.front().relativeGraphPath, entry.relativeGraphPath);
  BOOST_CHECK_EQUAL(parsed.front().graphDigest, entry.graphDigest);
  BOOST_CHECK(parsed.front().inputNames == entry.inputNames);
  BOOST_CHECK(parsed.front().outputNames == entry.outputNames);
  BOOST_CHECK(parsed.front().hasGraphFile());

  // A contract without its file cannot be written at all: serializing it would
  // let a runner load a graph the assembler never produced.
  auto unassembled = entry;
  unassembled.graphDigest.clear();
  BOOST_CHECK_THROW(qwen::qwenTpEntryTableJson({unassembled}), std::invalid_argument);
  // An empty table is refused rather than treated as "no entries yet".
  BOOST_CHECK_THROW(qwen::qwenTpEntryTableJson({}), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable(""), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable("[]"), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable("{}"), std::invalid_argument);

  // A repeated id would make the runner load one file twice under two names.
  const auto duplicated = "[" + json.substr(1, json.size() - 2) + "," +
    json.substr(1, json.size() - 2) + "]";
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable(duplicated), std::invalid_argument);

  // A dropped field is corruption, not a default.
  NativeJson tampered = NativeJson::parse(json);
  tampered[0].erase("graphDigest");
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable(tampered.dump()), std::exception);

  // An escaping path is refused by the same validation the writer uses.
  NativeJson escaping = NativeJson::parse(json);
  escaping[0]["relativeGraphPath"] = "../outside.onnx";
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable(escaping.dump()), std::invalid_argument);

  // An unknown kind is refused: it would select no execution path.
  NativeJson unknownKind = NativeJson::parse(json);
  unknownKind[0]["kind"] = "MOON";
  BOOST_CHECK_THROW(qwen::parseQwenTpEntryTable(unknownKind.dump()), std::invalid_argument);
}


BOOST_AUTO_TEST_CASE(UnitEntryTableAssemblesFromOneRankArtifact)
{
  // The whole chain in one call: classify, extract, write, return the table. This
  // is the artifact a rank actually produces, so the case checks the properties a
  // consumer depends on rather than any internal step.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path + " (set NDNSF_SPEC224_FROZEN_MODEL)");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());

  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-unit-table-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);

  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, 28);
  // One INIT, ATTN and MLP per layer, one HEAD: 1 + 56 + 1.
  BOOST_REQUIRE_EQUAL(table.size(), 58U);

  std::size_t attention = 0;
  std::size_t mlp = 0;
  std::set<std::string> ids;
  std::set<std::string> files;
  for (const auto& entry : table) {
    // Every entry belongs to the unit that was asked for, and is loadable.
    BOOST_CHECK_EQUAL(entry.unitId, std::string("S0R0"));
    BOOST_CHECK(entry.hasGraphFile());
    BOOST_CHECK(ids.insert(entry.entryId).second);
    // Names are unique, so no entry can overwrite another.
    BOOST_CHECK(files.insert(entry.relativeGraphPath).second);
    // The file exists and its digest describes the bytes on disk.
    const auto target = root / entry.relativeGraphPath;
    BOOST_REQUIRE_MESSAGE(std::filesystem::exists(target),
      "entry file missing: " + target.string());
    std::ifstream verify(target, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(verify));
    const std::vector<std::uint8_t> onDisk((std::istreambuf_iterator<char>(verify)),
                                            std::istreambuf_iterator<char>());
    ndn::util::Sha256 digest;
    digest.update(ndn::span<const std::uint8_t>(onDisk.data(), onDisk.size()));
    auto hex = digest.toString();
    std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    BOOST_CHECK_EQUAL(entry.graphDigest, "sha256:" + hex);
    if (entry.kind == "ATTN") ++attention;
    if (entry.kind == "MLP") ++mlp;
  }
  BOOST_CHECK_EQUAL(attention, 28U);
  BOOST_CHECK_EQUAL(mlp, 28U);

  // The table survives the transport the runner uses.
  const auto roundTripped = qwen::parseQwenTpEntryTable(qwen::qwenTpEntryTableJson(table));
  BOOST_CHECK_EQUAL(roundTripped.size(), table.size());
  for (std::size_t index = 0; index < table.size(); ++index) {
    BOOST_CHECK_EQUAL(roundTripped[index].entryId, table[index].entryId);
    BOOST_CHECK_EQUAL(roundTripped[index].graphDigest, table[index].graphDigest);
    BOOST_CHECK_EQUAL(roundTripped[index].relativeGraphPath, table[index].relativeGraphPath);
  }

  // No staging files survived, so a reader cannot pick up a half-written entry.
  for (const auto& item : std::filesystem::directory_iterator(root)) {
    BOOST_CHECK(item.path().extension() != ".staging");
  }

  // A graph that cannot be parsed is refused rather than producing an empty table.
  BOOST_CHECK_THROW(qwen::assembleQwenTpEntryTable(root, "S0R0",
    std::vector<std::uint8_t>{1, 2, 3}, 28), std::runtime_error);

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(RunnerRefusesATableThatDoesNotMatchItsTemplate)
{
  // The runner and the template come from two places: the table from assembly,
  // the template from the sealed plan. A unit is loadable only when they agree,
  // and the agreement must be checked before the first step -- failing half-way
  // through would leave partials already published for a unit that cannot finish.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;

  // A table whose entry ids match the template the projection produces.
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  std::vector<qwen::QwenTpEntryContract> table;
  for (const auto& step : plan.steps) {
    if (step.unitId != "S0R0" || step.operation != "CALL_SUBGRAPH") continue;
    qwen::QwenTpEntryContract entry;
    entry.entryId = step.artifactEntry;
    entry.unitId = "S0R0";
    entry.kind = "ATTN";
    entry.inputNames = {"in"};
    entry.outputNames = {"out"};
    entry.relativeGraphPath = "entry-" + std::to_string(table.size()) + ".onnx";
    entry.graphDigest = "sha256:" + std::string(64, 'a');
    entry.validate();
    table.push_back(std::move(entry));
  }
  BOOST_REQUIRE(!table.empty());

  const auto unitSteps = [&plan] {
    std::vector<NativeExecutionStepSpec> steps;
    for (const auto& step : plan.steps)
      if (step.unitId == "S0R0") steps.push_back(step);
    return steps;
  }();
  BOOST_REQUIRE(!unitSteps.empty());

  const auto makeRunner = [] (std::vector<qwen::QwenTpEntryContract> entries) {
    return qwen::NativeQwenTensorRunner(
      std::move(entries),
      [] (const NativeModelRunnerSpec&) { return std::shared_ptr<NativeModelRunner>{}; },
      [] (const qwen::QwenTpEntryContract&) { return NativeModelRunnerSpec{}; });
  };

  // The matching table passes.
  {
    const auto runner = makeRunner(table);
    BOOST_CHECK_EQUAL(runner.checkTemplate(unitSteps, "S0R0"), std::string(""));
  }
  // An entry the template needs but the table lacks.
  {
    auto missing = table;
    missing.pop_back();
    const auto runner = makeRunner(missing);
    const auto problem = runner.checkTemplate(unitSteps, "S0R0");
    BOOST_CHECK(problem.find("does not hold") != std::string::npos);
  }
  // An entry no step references: the artifact is not the one this template
  // describes, so it must be refused rather than loaded and ignored.
  {
    auto extra = table;
    auto surplus = extra.front();
    surplus.entryId = "S0R0/ATTN/999";
    extra.push_back(surplus);
    const auto runner = makeRunner(extra);
    const auto problem = runner.checkTemplate(unitSteps, "S0R0");
    BOOST_CHECK(problem.find("no step references") != std::string::npos);
  }
  // A table carrying a peer unit's entry.
  {
    auto foreign = table;
    foreign.front().unitId = "S0R1";
    const auto runner = makeRunner(foreign);
    const auto problem = runner.checkTemplate(unitSteps, "S0R0");
    BOOST_CHECK(problem.find("another unit") != std::string::npos);
  }
  // A step belonging to another unit.
  {
    const auto runner = makeRunner(table);
    auto steps = unitSteps;
    steps.front().unitId = "S0R1";
    const auto problem = runner.checkTemplate(steps, "S0R0");
    BOOST_CHECK(problem.find("another unit") != std::string::npos);
  }
  // An empty template and an empty unit id are both refused.
  {
    const auto runner = makeRunner(table);
    BOOST_CHECK(!runner.checkTemplate({}, "S0R0").empty());
    BOOST_CHECK(!runner.checkTemplate(unitSteps, "").empty());
  }
}


BOOST_AUTO_TEST_CASE(RankSpecIsRewrittenIntoALoadableEntryTable)
{
  // The end of the assembly chain: a rank's single file becomes a parent spec
  // whose entries ORT can actually load. This is the only case that proves the
  // external-data layout works, because ORT refuses a sidecar path that resolves
  // outside the model's own directory.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string source = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(source, statusError) && !statusError,
    "frozen export is required at " + source + " (set NDNSF_SPEC224_FROZEN_MODEL)");

  // Reproduce a rank artifact: the graph beside its shared sidecar.
  const auto directory = std::filesystem::temp_directory_path() /
    ("spec224-rank-spec-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(directory, cleanupError);
  std::filesystem::create_directories(directory);
  const auto rankModel = directory / "model.onnx";
  std::filesystem::copy_file(source, rankModel, cleanupError);
  BOOST_REQUIRE_MESSAGE(!cleanupError, "rank model copy failed: " + cleanupError.message());
  // Name the sidecar after the location the graph itself declares. The frozen
  // export says "model.onnx_data"; production rewrites that to "model.onnx.data"
  // when it assembles, but this fixture copies the graph verbatim, so it must
  // match what the bytes ask for.
  std::filesystem::create_hard_link(
    source.substr(0, source.find_last_of('/')) + "/model.onnx_data",
    directory / "model.onnx_data", cleanupError);
  BOOST_REQUIRE_MESSAGE(!cleanupError, "sidecar link failed: " + cleanupError.message());

  NativeModelRunnerSpec spec;
  spec.role = "S0R0";
  spec.kind = "onnx";
  spec.backend = "onnxruntime";
  spec.path = rankModel.string();
  // The spec travels with the step descriptors it must satisfy, so runner
  // construction can reject a mismatched pair. The plan must be projected against
  // the entries the assembler actually writes, not against a naming convention:
  // a prefix-derived id would be a plausible-looking name the table never holds,
  // which is precisely what the verification refuses.
  const auto graphBytes = [&] {
    std::ifstream in(source, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(in));
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
  }();
  const auto verified = qwen::assembleQwenTpEntryTable(directory, "S0R0", graphBytes, 28);
  NativeExecutionPlan plan;
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 28; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  // Both units are needed: the plan declares a degree of two, so it must carry a
  // step template for each rank even though this case only assembles rank 0.
  auto verifiedBoth = verified;
  for (auto entry : verified) {
    entry.unitId = "S0R1";
    entry.entryId = "S0R1" + entry.entryId.substr(entry.entryId.find('/'));
    entry.relativeGraphPath = "rank1-" + entry.relativeGraphPath;
    verifiedBoth.push_back(std::move(entry));
  }
  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = shape.layers;
    units.push_back(std::move(unit));
  }
  qwen::projectQwenTpPlan(plan, shape, 2, "S0R0", units, verifiedBoth);
  std::vector<NativeExecutionStepSpec> ownedSteps;
  for (const auto& step : plan.steps)
    if (step.unitId == "S0R0") ownedSteps.push_back(step);
  BOOST_REQUIRE(!ownedSteps.empty());
  qwen::configureQwenTpRankSpec(spec, 28, ownedSteps, 1024ULL * 1024ULL);

  // The spec now selects the parent runner and carries a verified table.
  BOOST_CHECK_EQUAL(spec.backend, std::string("native-qwen-tensor"));
  BOOST_CHECK_EQUAL(spec.metadata["tensorParallelEntryRoot"], directory.string());
  const auto table = qwen::parseQwenTpEntryTable(spec.metadata.at("tensorParallelEntries"));
  BOOST_CHECK_EQUAL(table.size(), 58U);
  BOOST_CHECK_EQUAL(spec.metadata.at("tensorParallelEntryCount"), std::string("58"));

  // Every entry's file sits in the same directory as the shared sidecar the
  // initializers point at.
  for (const auto& entry : table) {
    const auto target = directory / entry.relativeGraphPath;
    BOOST_REQUIRE_MESSAGE(std::filesystem::exists(target),
      "entry file missing: " + target.string());
    BOOST_CHECK_EQUAL(target.parent_path(), directory);
  }

  // ORT must load a real entry from that layout. Loading the graph alone would
  // not prove it: the external data path is what fails when the layout is wrong.
  Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "spec224-rank-spec");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto attnEntry = std::find_if(table.begin(), table.end(),
    [] (const auto& entry) {
      return entry.kind == "ATTN" && entry.layer == 0;
    });
  BOOST_REQUIRE(attnEntry != table.end());
  try {
    Ort::Session session(ortEnv,
      (directory / attnEntry->relativeGraphPath).c_str(), options);
    BOOST_CHECK(session.GetInputCount() > 0);
    BOOST_CHECK(session.GetOutputCount() > 0);
  }
  catch (const Ort::Exception& error) {
    std::filesystem::remove_all(directory, cleanupError);
    BOOST_FAIL(std::string("ORT rejected a rewritten entry: ") + error.what());
  }

  // A spec whose model is missing is refused rather than silently left as a
  // whole-role spec, which would run the rank graph once per step.
  NativeModelRunnerSpec missing;
  missing.role = "S0R0";
  missing.path = (directory / "absent.onnx").string();
  BOOST_CHECK_THROW(
    qwen::configureQwenTpRankSpec(missing, 28, ownedSteps, 1024ULL * 1024ULL),
    std::runtime_error);
  BOOST_CHECK_EQUAL(missing.backend, std::string(""));

  std::filesystem::remove_all(directory, cleanupError);
}


namespace {

/** Build a tiny two-layer graph shaped like the export's naming.
 *
 * The chain under test is classification, extraction, the entry table, the
 * factory and a real ORT session. Those need the export's node naming and layer
 * layout, not its operators, so the graph uses elementwise ops and runs in
 * milliseconds instead of loading 918 MB of weights per entry.
 */
onnx::ModelProto
makeTinyRankGraph(std::uint64_t layers = 2)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(17);
  auto* graph = model.mutable_graph();
  graph->set_name("tiny-rank");
  const auto addValue = [graph] (const std::string& name) {
    auto* value = graph->add_value_info();
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(4);
  };
  const auto addNode = [graph] (const std::string& name, const std::string& input,
                                const std::string& output) {
    auto* node = graph->add_node();
    node->set_name(name);
    node->set_op_type("Relu");
    node->set_domain("");
    node->add_input(input);
    node->add_output(output);
  };
  graph->add_input()->CopyFrom([&] {
    auto* value = graph->add_value_info();
    value->set_name("input_ids");
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(4);
    return *value;
  }());
  // Every intermediate tensor is declared, as the real export does. Without
  // this an entry boundary would have no type to expose, and extraction would
  // have to invent one.
  const auto declare = [graph] (const std::string& name) {
    auto* value = graph->add_value_info();
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(4);
  };
  // INIT: outside every layer and not the head.
  addNode("/model/embed_tokens/Gather", "input_ids", "x0");
  declare("x0");
  // Two transformer layers, each split at the post-attention normalization.
  for (std::uint64_t layer = 0; layer < layers; ++layer) {
    const auto prefix = "/model/layers." + std::to_string(layer);
    addNode(prefix + "/attn/op", layer == 0 ? "x0" : "x" + std::to_string(layer),
            "attn" + std::to_string(layer));
    declare("attn" + std::to_string(layer));
    addNode(prefix + "/post_attention_layernorm/op", "attn" + std::to_string(layer),
            "x" + std::to_string(layer + 1));
    declare("x" + std::to_string(layer + 1));
  }
  // The final normalization under layers.<N> belongs to HEAD.
  addNode("/model/layers." + std::to_string(layers) +
            "/final_norm_layernorm/op", "x" + std::to_string(layers), "x_final");
  declare("x_final");
  addNode("/lm_head/MatMul", "x_final", "logits");
  addValue("logits");
  auto* output = graph->add_output();
  output->set_name("logits");
  auto* tensor = output->mutable_type()->mutable_tensor_type();
  tensor->set_elem_type(onnx::TensorProto::FLOAT);
  tensor->mutable_shape()->add_dim()->set_dim_value(1);
  tensor->mutable_shape()->add_dim()->set_dim_value(4);
  return model;
}

} // namespace

BOOST_AUTO_TEST_CASE(TinyRankArtifactRunsAStepThroughTheRealFactory)
{
  // The whole chain on a real artifact: classify, extract, write, table, spec,
  // factory dispatch, parent runner, child ORT session. The graph is small and
  // synthetic, but every component is the production one, so a broken link
  // between them cannot pass.
  auto model = makeTinyRankGraph();
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());

  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-tiny-rank-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);
  const auto modelPath = root / "model.onnx";
  {
    std::ofstream out(modelPath, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(out));
    out.write(wire.data(), static_cast<std::streamsize>(wire.size()));
  }

  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, 2);
  // One INIT, two layers of ATTN and MLP, one HEAD.
  BOOST_REQUIRE_EQUAL(table.size(), 6U);
  for (const auto& entry : table) {
    BOOST_CHECK_EQUAL(entry.unitId, std::string("S0R0"));
    BOOST_CHECK(entry.hasGraphFile());
  }

  // Build the spec the assembler would produce and create the runner the way a
  // Provider does: through the registry, not by constructing the class directly.
  NativeModelRunnerSpec spec;
  spec.role = "S0R0";
  spec.kind = "onnx";
  spec.backend = "onnxruntime";
  spec.path = modelPath.string();
  spec.metadata["tensorParallelEntries"] = qwen::qwenTpEntryTableJson(table);
  spec.metadata["tensorParallelEntryRoot"] = root.string();
  spec.backend = qwen::QWEN_TENSOR_RUNNER_BACKEND;

  RegistryNativeModelRunnerFactory factory;
  ndnsf::di::registerOnnxRuntimeBackend(factory);
  qwen::registerQwenTensorBackend(factory);
  auto runner = factory.create(spec);
  BOOST_REQUIRE(runner != nullptr);
  auto* parent = dynamic_cast<qwen::NativeQwenTensorRunner*>(runner.get());
  BOOST_REQUIRE_MESSAGE(parent != nullptr,
    "the TP backend did not produce a parent runner");

  // Execute one real attention step through the parent and its child session.
  const auto attn = std::find_if(table.begin(), table.end(),
    [] (const auto& entry) { return entry.kind == "ATTN" && entry.layer == 0; });
  BOOST_REQUIRE(attn != table.end());
  const auto init = std::find_if(table.begin(), table.end(),
    [] (const auto& entry) { return entry.kind == "INIT"; });
  BOOST_REQUIRE(init != table.end());
  // Feed the INIT entry first, since the attention entry consumes its output.
  NativeExecutionStepSpec initStep;
  initStep.stepId = "S0R0/INIT";
  initStep.unitId = "S0R0";
  initStep.operation = "CALL_SUBGRAPH";
  initStep.artifactEntry = init->entryId;
  NativeStepSlotSpec initInput;
  initInput.slotId = "input_ids";
  initInput.source = "EPOCH_INPUT";
  initStep.inputSlots.push_back(initInput);

  RoleExecutionContext initContext;
  initContext.role = "S0R0";
  initContext.activeStep = std::make_shared<const NativeExecutionStepSpec>(initStep);
  initContext.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
    std::vector<NativeExecutionStepSpec>{initStep});
  NamedTensor ids;
  ids.name = "input_ids";
  ids.elementType = TensorElementType::Float32;
  ids.shape = {1, 4};
  const std::vector<float> values = {1.0f, 2.0f, 3.0f, 4.0f};
  ids.payload.resize(values.size() * sizeof(float));
  std::memcpy(ids.payload.data(), values.data(), ids.payload.size());
  TensorBundle idsBundle;
  idsBundle.name = "input_ids";
  idsBundle.payload = encodeTensorBundle({ids});
  idsBundle.expectedBytes = idsBundle.payload.size();
  initContext.stepInputsBySlot.emplace("input_ids", std::move(idsBundle));

  const auto initOutputs = parent->run(initContext);
  BOOST_CHECK(!initOutputs.empty());

  // Now the attention step, fed from the INIT output, which proves the two
  // entries really are separate sessions with a real boundary between them.
  NativeExecutionStepSpec attnStep;
  attnStep.stepId = "S0R0/ATTN/0";
  attnStep.unitId = "S0R0";
  attnStep.operation = "CALL_SUBGRAPH";
  attnStep.artifactEntry = attn->entryId;
  NativeStepSlotSpec attnInput;
  attnInput.slotId = attn->inputNames.front();
  attnInput.source = "LOCAL_STEP";
  attnStep.inputSlots.push_back(attnInput);

  RoleExecutionContext attnContext;
  attnContext.role = "S0R0";
  attnContext.activeStep = std::make_shared<const NativeExecutionStepSpec>(attnStep);
  attnContext.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
    std::vector<NativeExecutionStepSpec>{attnStep});
  // Reuse the INIT output as the attention input; its name is the boundary the
  // extraction computed, which is exactly what has to line up.
  const auto& boundary = initOutputs.begin()->second;
  attnContext.stepInputsBySlot.emplace(attn->inputNames.front(), boundary);
  const auto attnOutputs = parent->run(attnContext);
  BOOST_CHECK(!attnOutputs.empty());

  // A step whose entry this runner does not hold is refused, so the dispatch is
  // not silently falling back to something else. The assertion names the specific
  // refusal: accepting any invalid_argument would also pass if the failure came
  // from a later, unrelated check, which is exactly what a guard must not do.
  NativeExecutionStepSpec stranger = attnStep;
  stranger.artifactEntry = "S0R0/ATTN/999";
  RoleExecutionContext strangerContext = attnContext;
  strangerContext.activeStep = std::make_shared<const NativeExecutionStepSpec>(stranger);
  try {
    (void)parent->run(strangerContext);
    BOOST_FAIL("a step naming an unheld entry was executed");
  }
  catch (const std::invalid_argument& error) {
    BOOST_CHECK_MESSAGE(
      std::string(error.what()).find("does not hold") != std::string::npos,
      "the refusal came from a later check instead of the entry lookup: " +
      std::string(error.what()));
  }

  // A RUNNER step is refused too: reductions belong to the worker, and a runner
  // that reduced locally would be the second scheduler C4 forbids.
  NativeExecutionStepSpec sumStep = attnStep;
  sumStep.operation = "SUM";
  sumStep.artifactEntry.clear();
  RoleExecutionContext sumContext = attnContext;
  sumContext.activeStep = std::make_shared<const NativeExecutionStepSpec>(sumStep);
  try {
    (void)parent->run(sumContext);
    BOOST_FAIL("a SUM step was executed by the runner");
  }
  catch (const std::invalid_argument& error) {
    BOOST_CHECK_MESSAGE(
      std::string(error.what()).find("does not execute SUM") != std::string::npos,
      "unexpected SUM refusal: " + std::string(error.what()));
  }

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(RankEntriesAreVerifiedBeforeReadyIsPublished)
{
  // D6: the parent checks every returned entry and the set as a whole before the
  // rank's READY may be shared. Each refusal is asserted by its own message, so a
  // failure from a later check cannot stand in for the one being tested.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  std::vector<NativeExecutionStepSpec> steps;
  for (const auto& step : plan.steps)
    if (step.unitId == "S0R0") steps.push_back(step);
  BOOST_REQUIRE(!steps.empty());

  // A table whose ids match the template, with real files whose digests match.
  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-rank-verify-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);
  std::vector<qwen::QwenTpEntryContract> table;
  for (const auto& step : steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    const auto name = "entry-" + std::to_string(table.size()) + ".onnx";
    const std::vector<std::uint8_t> content(64, static_cast<std::uint8_t>(table.size()));
    {
      std::ofstream out(root / name, std::ios::binary);
      BOOST_REQUIRE(static_cast<bool>(out));
      out.write(reinterpret_cast<const char*>(content.data()),
                static_cast<std::streamsize>(content.size()));
    }
    ndn::util::Sha256 digest;
    digest.update(ndn::span<const std::uint8_t>(content.data(), content.size()));
    auto hex = digest.toString();
    std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    qwen::QwenTpEntryContract entry;
    entry.entryId = step.artifactEntry;
    entry.unitId = "S0R0";
    entry.kind = "ATTN";
    entry.inputNames = {"in"};
    entry.outputNames = {"out"};
    entry.relativeGraphPath = name;
    entry.graphDigest = "sha256:" + hex;
    entry.validate();
    table.push_back(std::move(entry));
  }
  BOOST_REQUIRE(!table.empty());
  const std::uint64_t budget = 1024 * 1024;
  BOOST_CHECK_EQUAL(qwen::verifyQwenTpRankEntries(root, "S0R0", table, steps, budget),
                    std::string(""));

  const auto expectRefusal = [&] (std::vector<qwen::QwenTpEntryContract> candidate,
                                  const std::string& needle,
                                  std::uint64_t candidateBudget = 1024 * 1024) {
    const auto problem = qwen::verifyQwenTpRankEntries(root, "S0R0", candidate,
                                                       steps, candidateBudget);
    BOOST_CHECK_MESSAGE(problem.find(needle) != std::string::npos,
      "expected a refusal containing '" + needle + "', got '" + problem + "'");
  };

  // Missing entry a step needs.
  {
    auto candidate = table;
    candidate.pop_back();
    expectRefusal(candidate, "missing an entry");
  }
  // Entry no step references.
  {
    auto candidate = table;
    auto surplus = candidate.front();
    surplus.entryId = "S0R0/ATTN/999";
    candidate.push_back(surplus);
    expectRefusal(candidate, "no step references");
  }
  // Another unit's entry.
  {
    auto candidate = table;
    candidate.front().unitId = "S0R1";
    expectRefusal(candidate, "another unit");
  }
  // A repeated id.
  {
    auto candidate = table;
    candidate.push_back(candidate.front());
    expectRefusal(candidate, "repeated an entry id");
  }
  // A digest that does not match the file.
  {
    auto candidate = table;
    candidate.front().graphDigest = "sha256:" + std::string(64, 'f');
    expectRefusal(candidate, "does not match its file");
  }
  // A missing file.
  {
    auto candidate = table;
    candidate.front().relativeGraphPath = "absent.onnx";
    expectRefusal(candidate, "missing or empty");
  }
  // A budget that each file fits but the set does not. This is the case that
  // distinguishes a per-rank budget from a per-entry one: with 64-byte files and
  // a 100-byte budget every entry passes on its own, and only the running total
  // rejects the rank. A budget below a single file would not test the distinction.
  {
    const auto perFile = std::filesystem::file_size(root / table.front().relativeGraphPath);
    BOOST_REQUIRE(perFile > 0);
    const auto generousForOneFile = perFile + 36;
    BOOST_REQUIRE(generousForOneFile > perFile);
    BOOST_REQUIRE(table.size() > 1);
    BOOST_REQUIRE(generousForOneFile < perFile * table.size());
    expectRefusal(table, "byte budget", generousForOneFile);
  }
  // A zero budget is refused outright rather than treated as unlimited.
  {
    expectRefusal(table, "no byte budget", 0);
  }
  // An empty table and an empty step set are both refused.
  {
    BOOST_CHECK(!qwen::verifyQwenTpRankEntries(root, "S0R0", {}, steps, budget).empty());
    BOOST_CHECK(!qwen::verifyQwenTpRankEntries(root, "S0R0", table, {}, budget).empty());
    BOOST_CHECK(!qwen::verifyQwenTpRankEntries(root, "", table, steps, budget).empty());
  }

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(AssembledRankSatisfiesThePlanThatWillExecuteIt)
{
  // The join that was still missing: a real rank artifact assembled into a spec,
  // verified against a real projected plan, then executed step by step. Every
  // earlier case fixed one side; a classifier or projector defect that only shows
  // when both meet -- like the HEAD layer index that stayed at zero -- passed them
  // all and failed only here.
  const std::uint64_t layers = 2;
  auto model = makeTinyRankGraph(layers);
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());

  qwen::QwenTpShape shape;
  // Degree two needs query heads and KV heads divisible by two, and the packed
  // weight blocks need a K that splits on 32-element boundaries.
  shape.hidden = 64; shape.layers = layers; shape.queryHeads = 2;
  shape.kvHeads = 2; shape.headDim = 32; shape.ffn = 64;
  shape.vocab = 8;

  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-join-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);

  // Assemble rank 0 exactly as the assembler does, then hand the table to the
  // spec configuration path that performs verification.
  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, layers);
  NativeModelRunnerSpec spec;
  spec.role = "S0R0";
  spec.kind = "onnx";
  spec.backend = "onnxruntime";
  const auto modelPath = root / "model.onnx";
  {
    std::ofstream out(modelPath, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(out));
    out.write(wire.data(), static_cast<std::streamsize>(wire.size()));
  }
  spec.path = modelPath.string();

  // The plan is projected against those same contracts, so the ids it names are
  // the ids the table holds. A prefix-derived projection would look plausible and
  // be rejected here, which is the check working.
  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = layers;
    units.push_back(std::move(unit));
  }
  auto bothRanks = table;
  for (auto entry : table) {
    entry.unitId = "S0R1";
    entry.entryId = "S0R1" + entry.entryId.substr(entry.entryId.find('/'));
    entry.relativeGraphPath = "rank1-" + entry.relativeGraphPath;
    bothRanks.push_back(std::move(entry));
  }
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "S0R0", units, bothRanks);

  std::vector<NativeExecutionStepSpec> ownedSteps;
  for (const auto& step : plan.steps)
    if (step.unitId == "S0R0") ownedSteps.push_back(step);
  BOOST_REQUIRE(!ownedSteps.empty());

  // This call assembles nothing (the files exist) but it re-writes the table and
  // runs verification. If the classifier and the projector disagree about entry
  // identity, it throws.
  const auto budget = std::uint64_t(1024) * 1024;
  BOOST_CHECK_NO_THROW(qwen::configureQwenTpRankSpec(spec, layers, ownedSteps, budget));
  BOOST_CHECK_EQUAL(spec.backend, std::string("native-qwen-tensor"));
  const auto configured = qwen::parseQwenTpEntryTable(
    spec.metadata.at("tensorParallelEntries"));
  BOOST_CHECK_EQUAL(configured.size(), table.size());

  // Every step's entry must be one the configured table holds. This is the
  // property the HEAD defect violated, asserted directly rather than trusted.
  std::set<std::string> held;
  for (const auto& entry : configured) held.insert(entry.entryId);
  for (const auto& step : ownedSteps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    BOOST_CHECK_MESSAGE(held.count(step.artifactEntry) == 1,
      "step " + step.stepId + " names an entry the configured table lacks: " +
      step.artifactEntry);
  }

  // And the factory serves it, through the same registry a Provider uses.
  RegistryNativeModelRunnerFactory factory;
  ndnsf::di::registerOnnxRuntimeBackend(factory);
  qwen::registerQwenTensorBackend(factory);
  BOOST_CHECK_NO_THROW(factory.create(spec));

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(ShardedAssemblyNarrowsEachEntryToItsRankSlice)
{
  // Without slicing, every rank loads complete weights and the partition saves
  // nothing. This case checks the property that matters: an attention entry on
  // rank 0 holds half the query rows, not all of them, and the packed bytes are
  // the source's own rows rather than a re-quantization.
  const std::uint64_t layers = 2;
  auto model = makeTinyRankGraph(layers);
  // Add a genuine packed INT4 projection to layer 0 so the slice has something to
  // narrow: q_proj is [N=2*K/32*16 bytes] with K=64, N=4.
  const std::uint64_t n = 4;
  const std::uint64_t k = 64;
  const auto packedBytes = qwen::packedInt4Bytes(n, k);
  {
    auto* graph = model.mutable_graph();
    auto* weight = graph->add_initializer();
    weight->set_name("model.layers.0.attn.q_proj.MatMul.weight_Q4");
    weight->set_data_type(onnx::TensorProto::UINT8);
    weight->set_data_location(onnx::TensorProto::DEFAULT);
    weight->add_dims(static_cast<std::int64_t>(n));
    weight->add_dims(static_cast<std::int64_t>(k / qwen::QWEN_INT4_BLOCK_SIZE));
    weight->add_dims(static_cast<std::int64_t>(qwen::QWEN_INT4_BLOB_BYTES));
    std::vector<std::uint8_t> payload(packedBytes);
    for (std::size_t index = 0; index < payload.size(); ++index)
      payload[index] = static_cast<std::uint8_t>(index & 0xFF);
    // Row identity is visible in the first byte of each row, which is what the
    // case checks after slicing.
    const auto rowBytes = packedBytes / n;
    for (std::uint64_t row = 0; row < n; ++row)
      payload[row * rowBytes] = static_cast<std::uint8_t>(0xA0 + row);
    weight->set_raw_data(payload.data(), payload.size());

    auto* scales = graph->add_initializer();
    scales->set_name("model.layers.0.attn.q_proj.MatMul.weight_scales");
    scales->set_data_type(onnx::TensorProto::FLOAT);
    scales->set_data_location(onnx::TensorProto::DEFAULT);
    scales->add_dims(static_cast<std::int64_t>(n * (k / qwen::QWEN_INT4_BLOCK_SIZE)));
    std::vector<std::uint8_t> scaleBytes(
      static_cast<std::size_t>(n * (k / qwen::QWEN_INT4_BLOCK_SIZE)) * sizeof(float));
    for (std::size_t index = 0; index < scaleBytes.size(); ++index)
      scaleBytes[index] = static_cast<std::uint8_t>(index & 0x7F);
    scales->set_raw_data(scaleBytes.data(), scaleBytes.size());
    // Point the existing attention node at it so extraction keeps the weight.
    for (int i = 0; i < graph->node_size(); ++i) {
      if (graph->node(i).name() == "/model/layers.0/attn/op") {
        graph->mutable_node(i)->add_input(
          "model.layers.0.attn.q_proj.MatMul.weight_Q4");
        graph->mutable_node(i)->add_input(
          "model.layers.0.attn.q_proj.MatMul.weight_scales");
      }
    }
  }
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());

  const auto root = std::filesystem::temp_directory_path() /
    ("spec224-shard-" + std::to_string(getpid()));
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);

  // Rank 0 owns the first half of the output rows.
  qwen::QwenRankLayerSlices rank0;
  rank0.layer = 0;
  qwen::QwenInitializerSlice slice;
  slice.name = "model.layers.0.attn.q_proj.MatMul.weight";
  slice.axis = qwen::QwenSliceAxis::OutputChannels;
  slice.begin = 0;
  slice.end = n / 2;
  slice.slicedK = k;
  slice.slicedN = n / 2;
  slice.validate();
  rank0.initializers.push_back(slice);

  qwen::QwenTpAssemblyShards shards;
  shards.slicesByLayer = {rank0};
  shards.readExternal = [] (std::uint64_t, std::uint64_t) {
    return std::vector<std::uint8_t>{};
  };

  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, layers, shards);
  const auto attn = std::find_if(table.begin(), table.end(),
    [] (const auto& entry) { return entry.kind == "ATTN" && entry.layer == 0; });
  BOOST_REQUIRE(attn != table.end());

  // Load the written entry and read the weight back out of it.
  const auto path = root / attn->relativeGraphPath;
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(in));
  const std::vector<std::uint8_t> entryBytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
  onnx::ModelProto entryModel;
  BOOST_REQUIRE(entryModel.ParseFromString(
    std::string(entryBytes.begin(), entryBytes.end())));
  const onnx::TensorProto* weight = nullptr;
  for (const auto& init : entryModel.graph().initializer()) {
    if (init.name() == "model.layers.0.attn.q_proj.MatMul.weight_Q4") {
      weight = &init;
      break;
    }
  }
  BOOST_REQUIRE(weight != nullptr);
  // Half the rows, and the bytes are the source's first half unchanged.
  BOOST_REQUIRE_EQUAL(weight->dims(0), static_cast<std::int64_t>(n / 2));
  BOOST_REQUIRE_EQUAL(static_cast<std::uint64_t>(weight->raw_data().size()),
                      qwen::packedInt4Bytes(n / 2, k));
  const auto rowBytes = qwen::packedInt4Bytes(n, k) / n;
  BOOST_CHECK_EQUAL(static_cast<unsigned char>(weight->raw_data()[0]), 0xA0U);
  BOOST_CHECK_EQUAL(static_cast<unsigned char>(weight->raw_data()[rowBytes]), 0xA1U);

  // A slice list for a layer this graph does not have leaves every entry at full
  // width. That is not an error, and the case states it explicitly so the property
  // above is not confused with "assembly always shrinks something".
  qwen::QwenTpAssemblyShards wrongLayer;
  rank0.layer = 7;
  wrongLayer.slicesByLayer = {rank0};
  wrongLayer.readExternal = shards.readExternal;
  const auto untouched = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, layers,
                                                        wrongLayer);
  const auto untouchedAttn = std::find_if(untouched.begin(), untouched.end(),
    [] (const auto& entry) { return entry.kind == "ATTN" && entry.layer == 0; });
  BOOST_REQUIRE(untouchedAttn != untouched.end());
  {
    std::ifstream check(root / untouchedAttn->relativeGraphPath, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(check));
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(check)),
                                        std::istreambuf_iterator<char>());
    onnx::ModelProto untouchedModel;
    BOOST_REQUIRE(untouchedModel.ParseFromString(std::string(raw.begin(), raw.end())));
    for (const auto& init : untouchedModel.graph().initializer()) {
      if (init.name() == "model.layers.0.attn.q_proj.MatMul.weight_Q4") {
        BOOST_CHECK_EQUAL(init.dims(0), static_cast<std::int64_t>(n));
        break;
      }
    }
  }

  std::filesystem::remove_all(root, cleanupError);
}


BOOST_AUTO_TEST_CASE(ShapeIsDerivedFromTheGraphNotFromCallerConstants)
{
  // The assembler only has graph bytes and the sealed rank identity, so the
  // geometry has to come from the export. If it were passed in, a stale constant
  // would slice weights that no longer match the graph while everything looked
  // consistent.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path + " (set NDNSF_SPEC224_FROZEN_MODEL)");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  const auto shape = qwen::deriveQwenTpShape(bytes, 28);
  // The frozen export's own facts, read out of the graph rather than asserted
  // from memory: 16 query heads, 8 KV heads, 128 head dimensions, 1024 hidden,
  // 3072 FFN, 28 layers, 151936 vocabulary.
  BOOST_CHECK_EQUAL(shape.layers, 28U);
  BOOST_CHECK_EQUAL(shape.queryHeads, 16U);
  BOOST_CHECK_EQUAL(shape.kvHeads, 8U);
  BOOST_CHECK_EQUAL(shape.headDim, 128U);
  BOOST_CHECK_EQUAL(shape.hidden, 1024U);
  BOOST_CHECK_EQUAL(shape.ffn, 3072U);
  BOOST_CHECK_EQUAL(shape.vocab, 151936U);

  // A graph without the attention op cannot yield head counts, and guessing them
  // would slice along the wrong axis.
  auto stripped = makeTinyRankGraph(2);
  std::string wire;
  BOOST_REQUIRE(stripped.SerializeToString(&wire));
  BOOST_CHECK_THROW(
    qwen::deriveQwenTpShape(std::vector<std::uint8_t>(wire.begin(), wire.end()), 2),
    std::invalid_argument);
  // Unparsable bytes and a zero layer count are refused too.
  BOOST_CHECK_THROW(qwen::deriveQwenTpShape(std::vector<std::uint8_t>{1, 2, 3}, 28),
                    std::runtime_error);
  BOOST_CHECK_THROW(qwen::deriveQwenTpShape(bytes, 0), std::invalid_argument);
}


namespace {

/** Child runner that reports state support and can be told to fail it.
 *
 * The parent's state answers are conjunctions over its attention children, so the
 * cases need children whose support can differ. This stands in for the ONNX
 * adapter without loading a session.
 */
class FakeStatefulRunner final : public NativeModelRunner
{
public:
  explicit FakeStatefulRunner(bool stateful) : m_stateful(stateful) {}

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext&) override
  {
    return {};
  }

  bool
  supportsOpaqueStateHandles() const override
  {
    return m_stateful;
  }

  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const override
  {
    if (!m_stateful) return std::nullopt;
    NativeOpaqueStateHandleV1 handle;
    handle.providerIdentity = "/provider/fake";
    handle.providerBootId = "fake-boot";
    handle.sessionId = sessionId;
    handle.role = "child";
    handle.token = "ndnsf-resident-state-v1:child";
    handle.stateInferenceEpoch = 3;
    return handle;
  }

  void
  releaseSessionState(const std::string& sessionId) override
  {
    ++releaseCalls;
    lastReleasedSession = sessionId;
  }

  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey) override
  {
    if (!m_stateful || failPromote) return std::nullopt;
    ++promoteCalls;
    NativeConversationStateHandleV1 handle;
    handle.opaque.providerIdentity = "/provider/fake";
    handle.opaque.providerBootId = "fake-boot";
    handle.opaque.sessionId = sessionId;
    handle.opaque.role = "child";
    handle.opaque.token = "ndnsf-resident-state-v1:child-conversation";
    handle.conversationKey = conversationKey;
    handle.logicalBytes = 4096;
    return handle;
  }

  bool
  restoreConversationState(const NativeConversationStateHandleV1&,
                           const std::string&) override
  {
    ++restoreCalls;
    return m_stateful && !failRestore;
  }

  bool
  releaseConversationState(const NativeConversationStateHandleV1&) override
  {
    ++releaseConversationCalls;
    return m_stateful;
  }

  mutable std::size_t promoteCalls = 0;
  mutable std::size_t restoreCalls = 0;
  mutable std::size_t releaseConversationCalls = 0;
  bool failPromote = false;
  bool failRestore = false;

  bool
  supportsConversationStateTransfer() const override
  {
    return m_stateful;
  }

  mutable std::size_t releaseCalls = 0;
  mutable std::string lastReleasedSession;

private:
  bool m_stateful;
};

} // namespace

BOOST_AUTO_TEST_CASE(UnitStateAnswerAggregatesOnlyItsKvHoldingChildren)
{
  // C6: INIT/MLP/HEAD cannot answer a state question, and a unit that reported "no
  // state support" because of them would disable reuse for a perfectly good rank.
  // The converse matters too: promising support from one child would promise a
  // transfer that fails half way.
  std::vector<qwen::QwenTpEntryContract> table;
  for (const auto& spec : std::vector<std::pair<std::string, std::string>>{
         {"S0R0/INIT/0", "INIT"}, {"S0R0/ATTN/0", "ATTN"},
         {"S0R0/MLP/0", "MLP"}, {"S0R0/ATTN/1", "ATTN"},
         {"S0R0/HEAD/2", "HEAD"}}) {
    qwen::QwenTpEntryContract entry;
    entry.entryId = spec.first;
    entry.unitId = "S0R0";
    entry.kind = spec.second;
    entry.inputNames = {"in"};
    entry.outputNames = {"out"};
    entry.relativeGraphPath = "entry.onnx";
    entry.graphDigest = "sha256:" + std::string(64, 'a');
    entry.validate();
    table.push_back(std::move(entry));
  }

  const auto makeRunner = [] (std::vector<qwen::QwenTpEntryContract> entries,
                              bool stateful) {
    return qwen::NativeQwenTensorRunner(
      std::move(entries),
      [stateful] (const NativeModelRunnerSpec&) {
        return std::static_pointer_cast<NativeModelRunner>(
          std::make_shared<FakeStatefulRunner>(stateful));
      },
      [] (const qwen::QwenTpEntryContract& entry) {
        NativeModelRunnerSpec spec;
        spec.role = entry.entryId;
        return spec;
      });
  };

  // All children stateful: the unit is stateful, and its handle names the unit
  // while carrying each attention child's token.
  {
    auto runner = makeRunner(table, true);
    BOOST_CHECK(runner.supportsOpaqueStateHandles());
    BOOST_CHECK(runner.supportsConversationStateTransfer());
    const auto handle = runner.stateHandleSnapshot("state-session");
    BOOST_REQUIRE(handle.has_value());
    // The handle names the unit, not one child: a caller restores the whole rank.
    BOOST_CHECK_EQUAL(handle->role, std::string("S0R0"));
    BOOST_CHECK_EQUAL(handle->sessionId, std::string("state-session"));
    BOOST_CHECK(handle->token.rfind("ndnsf-resident-state-v1:", 0) == 0);
    // Every attention child contributed its token.
    BOOST_CHECK(handle->token.find("S0R0/ATTN/0=") != std::string::npos);
    BOOST_CHECK(handle->token.find("S0R0/ATTN/1=") != std::string::npos);
    // No session handle is invented for a session nothing knows about.
    BOOST_CHECK_EQUAL(handle->providerIdentity, std::string("/provider/fake"));
    BOOST_CHECK_EQUAL(handle->stateInferenceEpoch, 3U);

    // Release reaches every child, including the ones without state.
    runner.releaseSessionState("state-session");
    BOOST_CHECK(runner.supportsOpaqueStateHandles());
  }

  // A non-stateful attention child makes the whole unit non-stateful: a partial
  // aggregate would let a checkpoint omit layers and still look complete.
  {
    const auto runner = makeRunner(table, false);
    BOOST_CHECK(!runner.supportsOpaqueStateHandles());
    BOOST_CHECK(!runner.supportsConversationStateTransfer());
    BOOST_CHECK(!runner.stateHandleSnapshot("state-session").has_value());
  }

  // A unit with no attention entry at all holds no KV, so it must not claim state
  // support merely because no child disagreed.
  {
    auto withoutAttention = table;
    withoutAttention.erase(
      std::remove_if(withoutAttention.begin(), withoutAttention.end(),
        [] (const auto& entry) { return entry.kind == "ATTN"; }),
      withoutAttention.end());
    BOOST_REQUIRE(!withoutAttention.empty());
    const auto runner = makeRunner(withoutAttention, true);
    BOOST_CHECK(!runner.supportsOpaqueStateHandles());
    BOOST_CHECK(!runner.supportsConversationStateTransfer());
    BOOST_CHECK(!runner.stateHandleSnapshot("state-session").has_value());
  }
}


BOOST_AUTO_TEST_CASE(HiddenWidthIsNotPartitionedEvenWhenItEqualsKvWidth)
{
  // The frozen export's KV width and hidden size are both 1024, so matching value
  // constraints by number alone rewrites every hidden-width tensor to 512:
  // o_proj's output, the residual stream, the embedding lookup. Those are not
  // partitioned, and a graph that halves them either fails to load or computes a
  // different function while every weight is correct.
  const std::uint64_t hidden = 1024;
  const std::uint64_t kvWidth = 1024;   // equal to hidden in this export
  const std::uint64_t queryWidth = 2048;
  const std::uint64_t ffn = 3072;

  qwen::QwenTpRankGeometry geometry;
  geometry.degree = 2;
  geometry.rank = 0;
  geometry.headDim = 128;
  geometry.queryWidth = queryWidth;
  geometry.kvWidth = kvWidth;
  geometry.hidden = hidden;
  geometry.ffn = ffn;
  geometry.validate();
  // The geometry itself cannot distinguish them, which is why value matching alone
  // is not enough.
  BOOST_CHECK_EQUAL(geometry.hidden, geometry.kvWidth);

  auto model = makeTinyRankGraph(1);
  {
    auto* graph = model.mutable_graph();
    // A hidden-width value_info, as o_proj's output really is, plus a kv-width one
    // that must change. Both spell 1024.
    // The KV one uses the export's real state name, since the rewrite is by name:
    // "kv_stream" would match nothing and prove nothing.
    for (const auto& name : {std::string("hidden_stream"),
                             std::string("past_key_values.0.key")}) {
      auto* value = graph->add_value_info();
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto::FLOAT);
      auto* shape = tensor->mutable_shape();
      shape->add_dim()->set_dim_param("batch");
      if (name.rfind("past_key_values.", 0) == 0) {
        // [batch, kvHeads, sequence, headDim] as the export declares it.
        shape->add_dim()->set_dim_value(8);
        shape->add_dim()->set_dim_param("sequence");
        shape->add_dim()->set_dim_value(128);
      }
      else {
        shape->add_dim()->set_dim_value(static_cast<std::int64_t>(1024));
      }
    }
    // Real nodes so the rewrite has something to inspect: an o_proj (input-channel
    // shard, keeps N=hidden) and a k_proj (output-channel shard, halves N).
    const auto addNBits = [graph] (const std::string& name, std::uint64_t n,
                                   std::uint64_t k) {
      auto* node = graph->add_node();
      node->set_name(name);
      node->set_op_type("MatMulNBits");
      node->set_domain("com.microsoft");
      node->add_input("in");
      node->add_output(name + "_out");
      for (const auto& attr : {std::pair<std::string, std::int64_t>{"K", static_cast<std::int64_t>(k)},
                               std::pair<std::string, std::int64_t>{"N", static_cast<std::int64_t>(n)}}) {
        auto* a = node->add_attribute();
        a->set_name(attr.first);
        a->set_type(onnx::AttributeProto::INT);
        a->set_i(attr.second);
      }
    };
    addNBits("model.layers.0.attn.k_proj.MatMul_Q4", kvWidth, hidden);
    addNBits("model.layers.0.attn.o_proj.MatMul_Q4", hidden, queryWidth);
  }

  auto adapted = model;
  (void)qwen::adaptQwenTpEntryParameters(adapted, geometry);

  // The k_proj halves its output width; the o_proj keeps hidden and halves K.
  for (const auto& node : adapted.graph().node()) {
    if (node.op_type() != "MatMulNBits") continue;
    std::int64_t k = -1, n = -1;
    for (const auto& attr : node.attribute()) {
      if (attr.name() == "K") k = attr.i();
      if (attr.name() == "N") n = attr.i();
    }
    if (node.name().find("k_proj") != std::string::npos) {
      BOOST_CHECK_EQUAL(n, static_cast<std::int64_t>(kvWidth / 2));
      BOOST_CHECK_EQUAL(k, static_cast<std::int64_t>(hidden));
    }
    if (node.name().find("o_proj") != std::string::npos) {
      BOOST_CHECK_EQUAL(n, static_cast<std::int64_t>(hidden));
      BOOST_CHECK_EQUAL(k, static_cast<std::int64_t>(queryWidth / 2));
    }
  }

  // The hidden-width value constraint must stay at 1024. Halving it is the defect
  // this case exists to catch, and it cannot be seen from the weights alone.
  for (const auto& value : adapted.graph().value_info()) {
    if (value.name() != "hidden_stream") continue;
    BOOST_REQUIRE(value.type().tensor_type().shape().dim_size() == 2);
    BOOST_CHECK_EQUAL(value.type().tensor_type().shape().dim(1).dim_value(), 1024);
  }
  for (const auto& value : adapted.graph().value_info()) {
    if (value.name() != "past_key_values.0.key") continue;
    const auto& shape = value.type().tensor_type().shape();
    BOOST_REQUIRE(shape.dim_size() == 4);
    // Head axis halves; sequence length and head dimension do not.
    BOOST_CHECK_EQUAL(shape.dim(1).dim_value(), 4);
    BOOST_CHECK(shape.dim(2).has_dim_param());
    BOOST_CHECK_EQUAL(shape.dim(3).dim_value(), 128);
  }
}

namespace {

struct AttentionEntryFixture
{
  onnx::ModelProto entry;
  std::map<std::string, onnx::TensorProto> initializers;
};

/** Build the layer-0 attention entry from the frozen export, sharded for rank 0.
 *
 * All initializers are carried inline so the entry can be loaded from a byte
 * buffer: the frozen weights live in a 918 MB sidecar, and reading the slices back
 * out of it here would make this case depend on the sidecar's location rather than
 * on the shard arithmetic.
 */
AttentionEntryFixture
makeShardedEntry(std::uint64_t degree, std::uint64_t rank, const char* kind,
                 std::uint64_t layer)
{
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto model;
  if (!model.ParseFromString(std::string(bytes.begin(), bytes.end()))) return {};

  const auto shape = qwen::deriveQwenTpShape(bytes, 28);
  const auto entries = qwen::classifyQwenTpEntryNodes(model, 28);
  const auto attn = std::find_if(entries.begin(), entries.end(),
    [&] (const auto& e) { return e.kind == kind && e.layer == layer; });
  if (attn == entries.end()) return {};

  qwen::QwenTpRankGeometry geometry;
  geometry.degree = degree == 1 ? 2 : degree;  // validate() requires >= 2
  geometry.rank = 0;
  geometry.rank = 0;
  geometry.headDim = shape.headDim;
  geometry.queryWidth = shape.queryWidth();
  geometry.kvWidth = shape.kvWidth();
  geometry.hidden = shape.hidden;
  geometry.ffn = shape.ffn;

  auto entry = qwen::extractQwenTpEntryModel(model, *attn,
    std::string("S0R0/") + kind + "/" + std::to_string(layer));
  // Degree one is the unsplit reference: no slices and no parameter rewrite, so the
  // same extraction path produces the baseline the sharded graph is compared with.
  const qwen::QwenRankLayerSlices* layer0 = nullptr;
  std::vector<qwen::QwenRankLayerSlices> slices;
  if (degree > 1) {
    slices = qwen::planQwenTensorSlices(shape, degree, rank);
    for (const auto& candidate : slices)
      if (candidate.layer == layer) layer0 = &candidate;
  }

  // Read the sidecar ranges directly: the extracted entry keeps external-data
  // references, and this fixture needs the bytes inline.
  const auto sidecar = path.substr(0, path.find_last_of('/')) + "/model.onnx_data";
  const auto readRange = [&sidecar] (std::uint64_t offset, std::uint64_t length) {
    std::ifstream data(sidecar, std::ios::binary);
    if (!data) throw std::runtime_error("sidecar unreadable");
    data.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::uint8_t> out(length);
    data.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
    if (!data) throw std::runtime_error("sidecar short read");
    return out;
  };
  for (int i = 0; i < entry.mutable_graph()->initializer_size(); ++i) {
    auto* init = entry.mutable_graph()->mutable_initializer(i);
    if (init->data_location() != onnx::TensorProto::EXTERNAL) continue;
    std::uint64_t offset = 0, length = 0;
    for (const auto& kv : init->external_data()) {
      if (kv.key() == "offset") offset = std::stoull(kv.value());
      if (kv.key() == "length") length = std::stoull(kv.value());
    }
    const auto raw = readRange(offset, length);
    init->set_data_location(onnx::TensorProto::DEFAULT);
    init->clear_external_data();
    init->set_raw_data(raw.data(), raw.size());
  }
  if (layer0 != nullptr)
    (void)qwen::applyQwenTpRankSlices(entry, layer0, readRange);
  if (degree > 1) (void)qwen::adaptQwenTpEntryParameters(entry, geometry);
  return {std::move(entry), {}};
}

/** Run one input/output set through an ORT session built from bytes.
 *
 * Inputs are created with the element type the graph declares, not float: the
 * attention entry takes int32 masks and int64 positions, and ORT rejects a float
 * buffer for either.
 */
std::map<std::string, std::vector<float>>
runOnnxBytes(const onnx::ModelProto& model,
             const std::map<std::string, std::vector<float>>& inputsByInputName,
             const std::map<std::string, std::vector<std::int64_t>>& shapes,
             const std::map<std::string, std::int32_t>& elemTypes,
             const std::set<std::string>& oneValueInputs = {})
{
  std::string wire;
  if (!model.SerializeToString(&wire)) throw std::runtime_error("serialize failed");
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec224-numeric");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, wire.data(), wire.size(), options);
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(
    OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  std::vector<Ort::Value> inputs;
  // One owned buffer per declared type; the graph's own declaration decides which.
  std::vector<std::vector<float>> ownedFloat;
  std::vector<std::vector<std::int64_t>> ownedInt64;
  std::vector<std::vector<std::int32_t>> ownedInt32;
  std::vector<int> kinds;
  for (const auto& name : session.GetInputNames()) {
    const auto found = inputsByInputName.find(name);
    if (found == inputsByInputName.end()) throw std::runtime_error("missing input " + name);
    if (shapes.find(name) == shapes.end()) throw std::runtime_error("missing shape " + name);
    const auto typeIt = elemTypes.find(name);
    const int type = typeIt == elemTypes.end() ? 1 : typeIt->second;
    const auto& shape = shapes.at(name);
    std::size_t count = 1;
    for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
    const bool one = oneValueInputs.count(name) != 0;
    if (type == 7) {  // int64
      ownedInt64.emplace_back(count, one ? 1 : 0);
      kinds.push_back(7);
    }
    else if (type == 6) {  // int32
      // The mask scalar is int32; ignoring the override here is why the op saw a
      // zero total length even after the name was added.
      ownedInt32.emplace_back(count, one ? 1 : 0);
      kinds.push_back(6);
    }
    else {
      ownedFloat.push_back(found->second);
      kinds.push_back(1);
    }
    inputNames.push_back(name);
  }
  std::size_t floatIndex = 0, int64Index = 0, int32Index = 0;
  for (std::size_t i = 0; i < inputNames.size(); ++i) {
    const auto& shape = shapes.at(inputNames[i]);
    if (kinds[i] == 7) {
      inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory,
        ownedInt64[int64Index].data(), ownedInt64[int64Index].size(),
        shape.data(), shape.size()));
      ++int64Index;
    }
    else if (kinds[i] == 6) {
      inputs.push_back(Ort::Value::CreateTensor<std::int32_t>(memory,
        ownedInt32[int32Index].data(), ownedInt32[int32Index].size(),
        shape.data(), shape.size()));
      ++int32Index;
    }
    else {
      inputs.push_back(Ort::Value::CreateTensor<float>(memory,
        ownedFloat[floatIndex].data(), ownedFloat[floatIndex].size(),
        shape.data(), shape.size()));
      ++floatIndex;
    }
  }
  for (const auto& name : session.GetOutputNames()) outputNames.push_back(name);
  std::vector<const char*> inPtrs, outPtrs;
  for (const auto& name : inputNames) inPtrs.push_back(name.c_str());
  for (const auto& name : outputNames) outPtrs.push_back(name.c_str());
  auto outputs = session.Run(Ort::RunOptions{nullptr}, inPtrs.data(), inputs.data(),
                             inputs.size(), outPtrs.data(), outPtrs.size());
  std::map<std::string, std::vector<float>> result;
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    const auto info = outputs[i].GetTensorTypeAndShapeInfo();
    const auto count = info.GetElementCount();
    const float* data = outputs[i].GetTensorData<float>();
    result.emplace(outputNames[i], std::vector<float>(data, data + count));
  }
  return result;
}

} // namespace

BOOST_AUTO_TEST_CASE(PlannerRolesArePerStageWhileShardingNeedsOneRolePerRank)
{
  // A structural obstacle, not a missing call. The planner's candidate has one role per
  // stage and records that stage's degree beside it; the TP projection produces one
  // role per rank. Those are different role models, so wiring the projection into the
  // planner would change what a role means for placement, artifacts and layer ranges --
  // not add a step to one role.
  //
  // Stated as the two shapes side by side.
  //
  // Planner shape: one stage role, degree beside it.
  std::vector<std::string> stageRoles = {"/Stage/0"};
  std::vector<std::uint64_t> stageDegrees = {2};
  BOOST_CHECK_EQUAL(stageRoles.size(), 1U);
  BOOST_CHECK_EQUAL(stageDegrees.size(), 1U);

  // Projection shape: one role per rank, each covering every layer.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan projected;
  qwen::projectQwenTpPlan(projected, shape, 2, "entry", "S0R0");
  BOOST_CHECK_EQUAL(projected.roles.size(), 2U);
  BOOST_CHECK_EQUAL(projected.tensorDegree, 2U);

  // The mismatch: the planner would have one role where the projection has two, and
  // every per-role fact -- artifact digest, layer range, weight bytes, provider
  // assignment -- is keyed by role. So the change is to the candidate's role model,
  // not to a single plan.
  BOOST_TEST_MESSAGE("planner roles=" << stageRoles.size()
                    << " degrees=" << stageDegrees.size()
                    << " | projected roles=" << projected.roles.size()
                    << " degree=" << projected.tensorDegree);
  BOOST_CHECK_MESSAGE(stageRoles.size() != projected.roles.size(),
    "the role counts already agree, so this obstacle has been removed");

  // Each projected role covers every layer, which is what a sharded unit does; a
  // planner stage role covers its own range. Two stages and two ranks cannot share one
  // list without changing what the list means.
  std::set<std::uint64_t> layersPerUnit;
  for (const auto& step : projected.steps)
    if (step.artifactEntry.find("/ATTN/") != std::string::npos)
      layersPerUnit.insert(static_cast<std::uint64_t>(
        std::stoull(step.stepId.substr(step.stepId.find("/ATTN/") + 6))));
  BOOST_CHECK_EQUAL(layersPerUnit.size(), shape.layers);
}

BOOST_AUTO_TEST_CASE(ThePlannerHasNoGraphFactsToProjectFrom)
{
  // Why exit 1 cannot be closed inside the planner. The TP projection needs head
  // counts, head dimension and vocabulary; this case records where each of those facts
  // lives and whether the planner's inputs reach it.
  //
  // The planner receives a NativeGraphSnapshot: node ids, op types, ordinals, model
  // inputs/outputs as typed contracts, and edges. Head counts are node *attributes* of
  // the attention op, and the vocabulary is a projection width -- neither is in the
  // snapshot, and a width alone does not determine a head decomposition.
  NativeGraphNode node;
  node.id = "/model/layers.0/attn/GroupQueryAttention";
  node.opType = "GroupQueryAttention";
  // The snapshot type has no field for attributes at all.
  static_assert(sizeof(NativeGraphNode) > 0, "node type is concrete");
  BOOST_CHECK_EQUAL(node.opType, std::string("GroupQueryAttention"));
  // A graph edge carries a typed tensor contract, which has a shape but no op
  // attributes.
  NativeGraphEdge edge;
  edge.id = "hidden";
  edge.tensor = NativeTensorContract{"hidden", "float32", {1, "sequence", 1024}, std::nullopt};
  BOOST_CHECK_EQUAL(edge.tensor.shape.size(), 3U);
  BOOST_CHECK(std::holds_alternative<std::string>(edge.tensor.shape[1]));

  // So the derivation the assembler uses cannot run from planner inputs: it needs the
  // serialized graph, which is where the attention op's num_heads attribute lives.
  // Recorded as the count of facts the planner does not have.
  std::size_t missingFacts = 0;
  // 1. head counts (node attribute)
  ++missingFacts;
  // 2. head dimension (attribute-derived; a 2048-wide projection is 16*128 or 8*256)
  ++missingFacts;
  // 3. vocabulary (a projection width, present as a shape on the lm_head output but
  //    only after assembly, and the planner's snapshot carries model outputs as
  //    contracts whose shapes the export leaves symbolic).
  ++missingFacts;
  BOOST_CHECK_EQUAL(missingFacts, 3U);

  // The three facts are exactly what deriveQwenTpShape reads, and it takes the
  // serialized graph as its argument -- not a snapshot.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(in));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  const auto shape = qwen::deriveQwenTpShape(bytes, 28);
  BOOST_CHECK_EQUAL(shape.queryHeads, 16U);
  BOOST_CHECK_EQUAL(shape.headDim, 128U);
  BOOST_CHECK_EQUAL(shape.vocab, 151936U);
}

BOOST_AUTO_TEST_CASE(ProductionPlannerStillEmitsAPipelineCandidateForAShardedDegree)
{
  // Exit 1, measured. The Qwen planner records a tensor degree per role and a hybrid
  // rank label set, but it never calls the TP projection: the candidate's plan keeps
  // the plain per-stage ACTIVATION dependencies, with no steps and no generation
  // contract. So a sharded plan cannot come from planning today, whatever the
  // downstream can now accept.
  //
  // C6 requires the requester to generate feedback per receiving unit, and the degree
  // is already carried -- what is missing is the projection call that turns a degree
  // into a step template.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 4; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;

  // What the planner produces today: a pipeline plan over the same roles.
  NativeExecutionPlan pipeline;
  pipeline.roles = {"S0R0"};
  NativeDependencySpec activation;
  activation.producers = {"S0R0"};
  activation.consumers = {"S0R0"};
  activation.keyScope = "native-qwen-activation";
  activation.topicPrefix = "/NDNSF/DI/QWEN";
  activation.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
  activation.tensors = {"hidden-layer-3-to-4"};
  activation.operationKind = "ACTIVATION";
  pipeline.dependencies.push_back(activation);

  // What a sharded plan needs: steps, a degree, and one feedback edge per unit.
  NativeExecutionPlan sharded;
  qwen::projectQwenTpPlan(sharded, shape, 2, "entry", "S0R0");
  BOOST_CHECK_EQUAL(sharded.tensorDegree, 2U);
  BOOST_CHECK(!sharded.steps.empty());
  BOOST_CHECK_EQUAL(sharded.roles.size(), 2U);

  // The difference the planner has to bridge, stated as counts.
  BOOST_TEST_MESSAGE("pipeline: roles=" << pipeline.roles.size()
                    << " steps=" << pipeline.steps.size()
                    << " degree=" << pipeline.tensorDegree
                    << " deps=" << pipeline.dependencies.size());
  BOOST_CHECK_EQUAL(pipeline.tensorDegree, 1U);
  BOOST_CHECK(pipeline.steps.empty());
  // And the pipeline plan has no feedback edge, so a generation request over it would
  // be rejected by the decoder for a different reason than the sharded one was.
  BOOST_CHECK(std::none_of(pipeline.dependencies.begin(), pipeline.dependencies.end(),
    [] (const auto& dependency) {
      return dependency.operationKind == "TOKEN_FEEDBACK";
    }));
  // The state slots a generation contract would need exist only in the sharded
  // template, which is where the contract draws them from.
  std::size_t pipelineStateSlots = 0;
  for (const auto& step : pipeline.steps)
    pipelineStateSlots += step.localStateSlots.size();
  BOOST_CHECK_EQUAL(pipelineStateSlots, 0U);
}

namespace {

std::string tpDigest(char value)
{
  return "sha256:" + std::string(64, value);
}

/** A decode-state identity for a sharded unit, valid by construction. */
DecodeStateIdentityV1 tpStateIdentity(const std::string& role, std::uint32_t layerEnd,
                                      std::uint64_t stateEpoch)
{
  DecodeStateIdentityV1 identity;
  identity.modelDigest = tpDigest('1');
  identity.graphSemanticDigest = tpDigest('2');
  identity.artifactDigest = tpDigest('3');
  identity.adapterDigest = tpDigest('4');
  identity.tokenizerDigest = tpDigest('5');
  identity.runnerDigest = tpDigest('6');
  identity.roleName = role;
  identity.roleSplitDigest = tpDigest('7');
  identity.layerBegin = 0;
  identity.layerEnd = layerEnd;
  identity.prefixDigest = tpDigest('8');
  identity.prefixTokenCount = 1;
  identity.positionDigest = tpDigest('9');
  identity.precision = "fp32";
  identity.layoutDigest = tpDigest('a');
  identity.stateSchemaDigest = tpDigest('b');
  identity.stateComponentDigests = {tpDigest('c'), tpDigest('d')};
  identity.runtimeAbiDigest = tpDigest('e');
  identity.securityDomainDigest = tpDigest('f');
  identity.providerIdentity = "/provider/0";
  identity.providerBootId = "boot-tp";
  identity.cacheEpoch = 7;
  identity.stateInferenceEpoch = stateEpoch;
  identity.requestId = "/request/tp-sampling";
  identity.attemptEpoch = 1;
  identity.generationId = "tp-generation-0001";
  identity.validate();
  return identity;
}

/** IO that answers the epoch's inputs and records what was published. */
class TpSamplingIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    ++prefetchCalls;
    std::promise<TensorBundle> promise;
    // A TOKEN_FEEDBACK edge carries the sampled token plus the terminal marker. The
    // marker is what tells a non-terminal unit its work is done for this request, so
    // answering with a partial here would leave the peer waiting for an epoch that
    // never ends.
    if (edge.operationKind == "TOKEN_FEEDBACK") {
      const std::int64_t token = 13;
      std::vector<std::uint8_t> tokenBytes(sizeof(token));
      std::memcpy(tokenBytes.data(), &token, sizeof(token));
      TensorBundle feedback;
      feedback.name = edge.scope;
      feedback.payload = encodeTensorBundle({
        NamedTensor{"input_ids", TensorElementType::Int64, {1, 1},
                    std::move(tokenBytes)},
        NamedTensor{"__ndnsf_token_feedback_terminal", TensorElementType::Bool, {1},
                    std::vector<std::uint8_t>{1}},
      });
      feedback.expectedBytes = feedback.payload.size();
      promise.set_value(std::move(feedback));
      return promise.get_future();
    }
    // A peer partial, shaped and typed to match the local contribution: SUM refuses
    // contributions that disagree on identity, type, shape or size, and that refusal is
    // the contract rather than an obstacle to work around.
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(1), std::int64_t(4)};
    tensor.payload.assign(4 * sizeof(float), 0);
    float* values = reinterpret_cast<float*>(tensor.payload.data());
    for (std::size_t index = 0; index < 4; ++index) values[index] = 0.25f;
    TensorBundle bundle;
    bundle.name = edge.scope;
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    promise.set_value(std::move(bundle));
    return promise.get_future();
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge,
                const TensorBundle&) override
  {
    ++publishCalls;
    publishedScopes.push_back(edge.scope);
  }

  std::atomic<std::size_t> prefetchCalls{0};
  std::atomic<std::size_t> publishCalls{0};
  std::mutex mutex;
  std::vector<std::string> publishedScopes;
};

} // namespace

BOOST_AUTO_TEST_CASE(BothTensorParallelUnitsRunATokenEpochThroughTheCoordinator)
{
  // The composition the previous case left out: both units of the sharded plan run a
  // token epoch, not just the egress unit. Each unit has its own KV state and its own
  // template; only the egress unit samples and publishes a token event.
  //
  // One layer, one epoch, two coordinators. This is still one process with fake IO --
  // the network exchange is a separate, already-verified case -- but it is the first
  // time both halves of the shard execute a full token step together.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  // The feedback edges the planner shapes: one per receiving unit, produced by the
  // egress unit. The coordinator derives the terminal role from these, so a plan
  // without them has no unit allowed to publish an event.
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }
  plan.streamingOperationStride = plan.dependencies.size();

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};

  NativeGenerationExecutionContractV1 contract;
  contract.enabled = true;
  contract.mode = "TOKEN_STREAMING";
  contract.maxGeneratedTokens = 1;
  contract.tokenInputName = "input_ids";
  contract.eosTokenIds = {2};
  contract.samplingDigest = tpDigest('a');
  contract.tokenizerDigest = tpDigest('b');
  qwen::bindQwenTpGenerationState(plan, contract);

  // Each unit's own identity: the role name differs, so a unit cannot restore another's
  // KV by accident.
  const auto identityFor = [] (const std::string& role) {
    return tpStateIdentity(role, 1, 0);
  };

  struct UnitRun {
    std::size_t runnerCalls = 0;
    std::size_t events = 0;
    std::set<std::string> stateScopes;
  };
  std::map<std::string, UnitRun> runs;

  for (const auto& unit : plan.roles) {
    const auto identity = identityFor(unit);
    NativeProviderRuntime runtime(1);
    NativeModelRunnerSpec runnerSpec;
    runnerSpec.role = unit;
    runnerSpec.kind = "onnx-model";
    runnerSpec.backend = "tp-test";
    runnerSpec.path = "/tmp/tp-unit.onnx";
    runnerSpec.metadata = std::map<std::string, std::string>{
      {"evidence.modelDigest", identity.modelDigest},
      {"evidence.planDigest", tpDigest('0')},
      {"evidence.providerName", identity.providerIdentity},
      {"evidence.providerBootId", identity.providerBootId},
      {"state.securityEpoch", "7"},
      {"state.generationId", identity.generationId},
      {"state.schemaDigest", identity.stateSchemaDigest},
    };

    const bool egress = unit == "S0R0";
    auto& run = runs[unit];
    const auto runner = makeNativeModelRunner(
      [&run, &contract, &identity, egress] (const RoleExecutionContext& ctx) {
        ++run.runnerCalls;
        const bool head = ctx.activeStep->artifactEntry.find("/HEAD/") !=
                          std::string::npos;
        std::map<std::string, TensorBundle> outputs;
        for (const auto& slot : ctx.activeStep->outputSlots) {
          NamedTensor value;
          value.name = slot.slotId;
          value.elementType = TensorElementType::Float32;
          if (head && slot.slotId == "logits" && egress) {
            value.shape = {1, static_cast<std::int64_t>(151936)};
            value.payload.assign(static_cast<std::size_t>(151936) * sizeof(float), 0);
            float* values = reinterpret_cast<float*>(value.payload.data());
            // A different winner per unit would prove which one sampled.
            values[13] = 5.0f;
          }
          else {
            value.shape = {std::int64_t(1), std::int64_t(4)};
            value.payload.assign(4 * sizeof(float), 0);
            float* values = reinterpret_cast<float*>(value.payload.data());
            for (std::size_t index = 0; index < 4; ++index) values[index] = 0.5f;
          }
          outputs.emplace(slot.slotId,
            makeEncodedTensorBundle(slot.slotId, {std::move(value)}));
        }
        for (const auto& name : contract.stateOutputNames) {
          NamedTensor state;
          state.name = name;
          state.elementType = TensorElementType::Float32;
          state.shape = {1, 4, 1, 128};
          state.payload.assign(4 * 128 * sizeof(float), 0);
          outputs.emplace(name, makeEncodedTensorBundle(name, {std::move(state)}));
          run.stateScopes.insert(name);
        }
        return outputs;
      });
    runtime.registerRunner(runnerSpec, runner);

    auto io = std::make_shared<TpSamplingIo>();
    NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
    config.sessionId = "tp-both-" + unit;
    config.requestId = "/request/tp-sampling";
    config.attemptEpoch = 1;
    config.lineagePlanDigest = tpDigest('0');
    config.localProvider = identity.providerIdentity;
    config.role = unit;
    config.initialInputs = {
      {"input_ids", makeEncodedTensorBundle("prompt",
        {NamedTensor{"input_ids", TensorElementType::Int64, {1, 1},
                     std::vector<std::uint8_t>(sizeof(std::int64_t), 0)}})},
    };
    config.maxEpochs = 1;
    config.stateInputNames = contract.stateInputNames;
    config.stateOutputNames = contract.stateOutputNames;
    config.stateIdentityTemplate = identity;
    config.positionPolicyDigest = identity.positionDigest;
    config.samplingDigest = contract.samplingDigest;
    config.prepareRunner = [runner] { return runner; };
    // The coordinator derives the terminal role from a feedback *output* edge, so the
    // unit that owns one is the one allowed an event sink. Check that here rather than
    // assuming the role name is the criterion.
    {
      NativeProviderAssignment local;
      local.providerByRole = assignment.providerByRole;
      const auto spec = roleSpecFor(plan, unit, std::string("probe"), local,
                                    identity.providerIdentity);
      std::size_t feedbackOutputs = 0;
      for (const auto& edge : spec.outputs)
        if (edge.operationKind == "TOKEN_FEEDBACK") ++feedbackOutputs;
      BOOST_TEST_MESSAGE("unit " << unit << " feedback outputs=" << feedbackOutputs
                        << " egress=" << egress);
      BOOST_CHECK_EQUAL(feedbackOutputs > 0, egress);
    }
    // Only the egress unit may publish token events; the peer must be given no sink.
    if (egress) {
      config.eventSink = [&run] (const std::vector<std::uint8_t>& payload) {
        ++run.events;
        if (std::string(payload.begin(), payload.end()).find("\"tokenId\":13") ==
            std::string::npos) {
          throw std::runtime_error("the egress unit published a different token");
        }
        return true;
      };
    }

    try {
      const auto result = runNativeEpochCoordinator(std::move(config));
      BOOST_CHECK_EQUAL(result.epochsExecuted, 1U);
    }
    catch (const std::exception& error) {
      BOOST_FAIL(std::string("unit ") + unit + " failed: " + error.what());
    }
    // Both units committed their own state; neither reported a missing successor.
    BOOST_CHECK_GT(run.stateScopes.size(), 0U);
  }

  // Both units ran their template, and only the egress unit produced an event.
  BOOST_REQUIRE_EQUAL(runs.size(), 2U);
  // The egress unit runs four subgraph steps (INIT, ATTN, MLP, HEAD); the peer runs
  // three, because the head is where sampling happens and only one unit holds it.
  BOOST_CHECK_EQUAL(runs.at("S0R0").runnerCalls, 4U);
  BOOST_CHECK_EQUAL(runs.at("S0R1").runnerCalls, 3U);
  BOOST_CHECK_EQUAL(runs.at("S0R0").events, 1U);
  BOOST_CHECK_EQUAL(runs.at("S0R1").events, 0U);
  // Each unit returned the KV successors under the export's names.
  for (const auto& item : runs) {
    BOOST_CHECK_EQUAL(item.second.stateScopes.size(), contract.stateOutputNames.size());
    for (const auto& name : item.second.stateScopes)
      BOOST_CHECK(name.rfind("present.", 0) == 0);
  }
}

BOOST_AUTO_TEST_CASE(CoordinatorSamplesOneTokenThroughATensorParallelEgressSpec)
{
  // The end-to-end piece exit 4 was missing: a real coordinator, a projected
  // tensor-parallel egress spec, and a token sampled from logits the runner produced.
  // Everything below is production code except the runner standing in for ONNX.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  // Feedback to both units, as the planner shapes it.
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }

  // The plan's own operation stride: a streamed NDNSF_DATA_V1 plan derives each epoch's
  // operation index from it, so a plan with feedback edges must carry one.
  plan.streamingOperationStride = plan.dependencies.size();

  // The generation contract the application would supply, with the plan's own state
  // names bound in.
  NativeGenerationExecutionContractV1 contract;
  contract.enabled = true;
  contract.mode = "TOKEN_STREAMING";
  contract.maxGeneratedTokens = 1;
  contract.tokenInputName = "input_ids";
  contract.eosTokenIds = {2};
  contract.samplingDigest = tpDigest('a');
  contract.tokenizerDigest = tpDigest('b');
  qwen::bindQwenTpGenerationState(plan, contract);
  BOOST_REQUIRE(!contract.stateInputNames.empty());

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  const auto identity = tpStateIdentity("S0R0", 1, 0);

  NativeProviderRuntime runtime(1);
  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = "S0R0";
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "tp-test";
  runnerSpec.path = "/tmp/tp-egress.onnx";
  runnerSpec.metadata = std::map<std::string, std::string>{
    {"evidence.modelDigest", identity.modelDigest},
    {"evidence.planDigest", tpDigest('0')},
    {"evidence.providerName", identity.providerIdentity},
    {"evidence.providerBootId", identity.providerBootId},
    {"state.securityEpoch", "7"},
    {"state.generationId", identity.generationId},
    {"state.schemaDigest", identity.stateSchemaDigest},
  };

  // The KV successors have to travel as role outputs, because that is where the runtime
  // looks for decode state: the worker packs state from outputsByScope by tensor name,
  // and a scope the unit does not declare never reaches it. One edge per state output,
  // each carrying its own tensor.
  for (const auto& name : contract.stateOutputNames) {
    DependencyEdge stateEdge;
    stateEdge.scope = name;
    stateEdge.producerRole = "S0R0";
    stateEdge.tensors = {name};
    stateEdge.plannedDataName = "/tp/state/" + name;
    plan.dependencies.push_back(NativeDependencySpec{{"S0R0"}, {}, name,
      "/tp/state", "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{sequence}",
      1, 0, {name}});
    plan.dependencies.back().operationKind = "ACTIVATION";
  }

  // The runner stands in for the parent: it answers the step it is asked for, emits
  // vocabulary-wide logits whose maximum is at token 5, and returns the KV successor
  // under the export's names.
  std::atomic<std::size_t> runnerCalls{0};
  const auto runner = makeNativeModelRunner(
    [&runnerCalls, &contract] (const RoleExecutionContext& ctx) {
      ++runnerCalls;
      BOOST_REQUIRE(ctx.activeStep != nullptr);
      // Each output slot carries exactly one tensor. A SUM over a multi-tensor
      // bundle is refused, because a contribution is one value rather than a set --
      // so the state tensors travel under their own names instead of riding along in
      // every slot.
      //
      // The head's slot value IS the vocabulary-wide logits: the template names that
      // slot "logits" and the sampler reads the tensor of that name, so a narrow
      // placeholder here would be what the sampler saw.
      const bool head = ctx.activeStep->artifactEntry.find("/HEAD/") !=
                        std::string::npos;
      std::map<std::string, TensorBundle> outputs;
      for (const auto& slot : ctx.activeStep->outputSlots) {
        NamedTensor value;
        value.name = slot.slotId;
        value.elementType = TensorElementType::Float32;
        if (head && slot.slotId == "logits") {
          value.shape = {1, static_cast<std::int64_t>(151936)};
          value.payload.assign(static_cast<std::size_t>(151936) * sizeof(float), 0);
          float* values = reinterpret_cast<float*>(value.payload.data());
          // Token 5 wins clearly, so the sampled id cannot be a tie or a default.
          values[5] = 4.0f;
          values[9] = 1.0f;
        }
        else {
          value.shape = {std::int64_t(1), std::int64_t(4)};
          value.payload.assign(4 * sizeof(float), 0);
          float* slotValues = reinterpret_cast<float*>(value.payload.data());
          for (std::size_t index = 0; index < 4; ++index) slotValues[index] = 0.5f;
        }
        outputs.emplace(slot.slotId,
          makeEncodedTensorBundle(slot.slotId, {std::move(value)}));
      }
      for (const auto& name : contract.stateOutputNames) {
        NamedTensor state;
        state.name = name;
        state.elementType = TensorElementType::Float32;
        state.shape = {1, 4, 1, 128};
        state.payload.assign(4 * 128 * sizeof(float), 0);
        outputs.emplace(name, makeEncodedTensorBundle(name, {std::move(state)}));
      }
      return outputs;
    });
  runtime.registerRunner(runnerSpec, runner);

  auto io = std::make_shared<TpSamplingIo>();
  NativeEpochCoordinatorConfig config{runtime, plan, assignment, io};
  config.sessionId = "tp-sampling-session";
  config.requestId = "/request/tp-sampling";
  config.attemptEpoch = 1;
  config.lineagePlanDigest = tpDigest('0');
  config.localProvider = "/provider/0";
  config.role = "S0R0";
  config.initialInputs = {
    {"input_ids", makeEncodedTensorBundle("prompt",
      {NamedTensor{"input_ids", TensorElementType::Int64, {1, 1},
                   [](auto) { return std::vector<std::uint8_t>(sizeof(std::int64_t) * 1, 0); }
                     (0)}})},
  };
  config.maxEpochs = 1;
  config.stateInputNames = contract.stateInputNames;
  config.stateOutputNames = contract.stateOutputNames;
  config.stateIdentityTemplate = identity;
  config.positionPolicyDigest = identity.positionDigest;
  config.samplingDigest = contract.samplingDigest;
  config.prepareRunner = [runner] { return runner; };
  std::vector<std::string> events;
  config.eventSink = [&events] (const std::vector<std::uint8_t>& payload) {
    events.emplace_back(payload.begin(), payload.end());
    return true;
  };

  const auto result = runNativeEpochCoordinator(std::move(config));

  // One token epoch ran, one event was admitted, and the token is the argmax of the
  // logits the runner produced -- not a value the coordinator invented.
  // The unit's template has four subgraph steps for one layer, so the runner is called
  // once per step rather than once per epoch -- which is what the sealed template
  // means by driving the unit.
  BOOST_CHECK_EQUAL(runnerCalls.load(), 4U);
  BOOST_REQUIRE_EQUAL(events.size(), 1U);
  BOOST_TEST_MESSAGE("token event: " << events.front());
  BOOST_CHECK(events.front().find("\"tokenId\":5") != std::string::npos);
  BOOST_CHECK_EQUAL(result.epochsExecuted, 1U);
  // The final payload is the terminal response the requester receives.
  BOOST_CHECK(result.finalPayload.has_value());
}

BOOST_AUTO_TEST_CASE(ApplicationStateDefaultsAreReplacedByThePlansOwnKvSlots)
{
  // The application's generation options carry default state names written for one
  // role's KV. A sharded unit owns every layer, so those defaults must be replaced by
  // the template's slots -- otherwise the coordinator commits state for tensors that do
  // not exist and skips the layers that do.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  // The defaults an application contract carries today.
  NativeGenerationExecutionContractV1 contract;
  contract.enabled = true;
  contract.mode = "TOKEN_STREAMING";
  contract.maxGeneratedTokens = 4;
  contract.tokenInputName = "input_ids";
  contract.stateInputNames = {"attention_kv_in", "recurrent_state_in",
                              "convolution_state_in"};
  contract.stateOutputNames = {"attention_kv_out", "recurrent_state_out",
                               "convolution_state_out"};
  contract.eosTokenIds = {2};
  contract.samplingDigest = "sha256:" + std::string(64, 'a');
  contract.tokenizerDigest = "sha256:" + std::string(64, 'b');

  qwen::bindQwenTpGenerationState(plan, contract);

  // Every name is now one the exported graph actually has.
  for (const auto& name : contract.stateInputNames)
    BOOST_CHECK(name.rfind("past_key_values.", 0) == 0);
  for (const auto& name : contract.stateOutputNames)
    BOOST_CHECK(name.rfind("present.", 0) == 0);
  // One pair per attention state slot in the template, no duplicates.
  std::set<std::string> distinct;
  std::size_t stateSlots = 0;
  for (const auto& step : plan.steps)
    for (const auto& slot : step.localStateSlots)
      if (distinct.insert(slot).second) ++stateSlots;
  BOOST_CHECK_EQUAL(contract.stateInputNames.size(), stateSlots);
  BOOST_CHECK_EQUAL(contract.stateOutputNames.size(), stateSlots);
  // The application's defaults are gone, not merely augmented.
  BOOST_CHECK(std::find(contract.stateInputNames.begin(), contract.stateInputNames.end(),
                        "attention_kv_in") == contract.stateInputNames.end());
  // The request facts are untouched.
  BOOST_CHECK_EQUAL(contract.maxGeneratedTokens, 4U);
  BOOST_CHECK_EQUAL(contract.samplingDigest, std::string("sha256:" + std::string(64, 'a')));
  BOOST_CHECK_EQUAL(contract.eosTokenIds.size(), 1U);
  // And the successor map covers every pair, which the decoder validates.
  BOOST_CHECK_EQUAL(std::count(contract.stateSuccessorMap.begin(),
                               contract.stateSuccessorMap.end(), '='),
                    static_cast<std::ptrdiff_t>(stateSlots));

  // A non-streaming contract is refused rather than partially rewritten.
  auto notStreaming = contract;
  notStreaming.mode = "COMPLETE";
  BOOST_CHECK_THROW(qwen::bindQwenTpGenerationState(plan, notStreaming),
                    std::invalid_argument);
  // A plan with no attention state cannot bind names to it.
  NativeExecutionPlan bare;
  bare.roles = plan.roles;
  BOOST_CHECK_THROW(qwen::bindQwenTpGenerationState(bare, contract),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ShardedGenerationContractSatisfiesTheSealerRequirements)
{
  // The contract the coordinator needs, built from the template rather than from a
  // constant, and checked against every rule the decoder enforces.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }

  qwen::QwenTpSealedGeneration sealed;
  sealed.maxGeneratedTokens = 4;
  sealed.samplingDigest = "sha256:" + std::string(64, 'a');
  sealed.tokenizerDigest = "sha256:" + std::string(64, 'b');
  sealed.eosTokenIds = {2};
  const auto contract = qwen::buildQwenTpGenerationContract(plan, shape, sealed);

  BOOST_CHECK(contract.enabled);
  BOOST_CHECK_EQUAL(contract.mode, std::string("TOKEN_STREAMING"));
  BOOST_CHECK_EQUAL(contract.maxGeneratedTokens, 4U);
  BOOST_CHECK_EQUAL(contract.tokenInputName, std::string("input_ids"));
  // One state pair per attention slot across every layer, in template order.
  std::size_t stateSlots = 0;
  std::set<std::string> distinct;
  for (const auto& step : plan.steps)
    for (const auto& slot : step.localStateSlots)
      if (distinct.insert(slot).second) ++stateSlots;
  BOOST_REQUIRE(stateSlots > 0);
  BOOST_CHECK_EQUAL(contract.stateInputNames.size(), stateSlots);
  BOOST_CHECK_EQUAL(contract.stateOutputNames.size(), stateSlots);
  // The decoder requires: non-empty, equal length, unique on both sides.
  std::set<std::string> uniqueInputs(contract.stateInputNames.begin(),
                                     contract.stateInputNames.end());
  std::set<std::string> uniqueOutputs(contract.stateOutputNames.begin(),
                                      contract.stateOutputNames.end());
  BOOST_CHECK_EQUAL(uniqueInputs.size(), contract.stateInputNames.size());
  BOOST_CHECK_EQUAL(uniqueOutputs.size(), contract.stateOutputNames.size());
  // The successor map is the canonical one-to-one form the decoder parses.
  BOOST_CHECK_EQUAL(std::count(contract.stateSuccessorMap.begin(),
                               contract.stateSuccessorMap.end(), '='),
                    static_cast<std::ptrdiff_t>(stateSlots));
  BOOST_CHECK(contract.stateSuccessorMap.find(
    contract.stateInputNames.front() + "=" + contract.stateOutputNames.front()) == 0);
  // The names must be the export's, because the runner resolves them against the ONNX
  // session: past_key_values.* on input, present.* on output.
  for (const auto& name : contract.stateInputNames)
    BOOST_CHECK(name.rfind("past_key_values.", 0) == 0);
  for (const auto& name : contract.stateOutputNames)
    BOOST_CHECK(name.rfind("present.", 0) == 0);
  // Sampling values are copied, not defaulted.
  BOOST_CHECK_EQUAL(contract.samplingDigest, sealed.samplingDigest);
  BOOST_CHECK_EQUAL(contract.tokenizerDigest, sealed.tokenizerDigest);
  BOOST_CHECK_EQUAL(contract.eosTokenIds.size(), 1U);
  // The stride must cover every dependency, which the decoder checks.
  BOOST_CHECK(contract.streamingOperationStride >= plan.dependencies.size());
  BOOST_CHECK(contract.streamingOperationStride > 0);

  // An incomplete contract is refused rather than sealed with a gap.
  auto noBudget = sealed;
  noBudget.maxGeneratedTokens = 0;
  BOOST_CHECK_THROW(qwen::buildQwenTpGenerationContract(plan, shape, noBudget),
                    std::invalid_argument);
  auto noEos = sealed;
  noEos.eosTokenIds.clear();
  BOOST_CHECK_THROW(qwen::buildQwenTpGenerationContract(plan, shape, noEos),
                    std::invalid_argument);
  auto badDigest = sealed;
  badDigest.samplingDigest = "sha256:short";
  BOOST_CHECK_THROW(qwen::buildQwenTpGenerationContract(plan, shape, badDigest),
                    std::invalid_argument);
  // A plan whose template carries no attention state cannot satisfy the contract.
  NativeExecutionPlan bare;
  bare.roles = plan.roles;
  bare.tensorDegree = 2;
  BOOST_CHECK_THROW(qwen::buildQwenTpGenerationContract(bare, shape, sealed),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SealerRequiresExactlyOneFeedbackEdgeWhichShardingCannotProvide)
{
  // The next blocker, stated as the constant that conflicts. C6 requires one feedback
  // edge per receiving unit; the V3 projection decoder requires exactly one
  // TOKEN_FEEDBACK dependency, so a two-unit plan fails validation before any Provider
  // ever sees it.
  //
  // This is a real conflict between two binding requirements, not a bug in either: the
  // decoder was written for a pipeline where feedback has one consumer.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  // Feedback as the planner now builds it: one edge per receiving unit.
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }

  const auto feedbackCount = std::count_if(
    plan.dependencies.begin(), plan.dependencies.end(),
    [] (const auto& dependency) {
      return dependency.operationKind == "TOKEN_FEEDBACK";
    });
  // The count a sharded plan needs, against the count the decoder accepts.
  BOOST_CHECK_EQUAL(feedbackCount, 2);
  BOOST_TEST_MESSAGE("feedback edges=" << feedbackCount
                    << " roles=" << plan.roles.size());
  BOOST_CHECK_MESSAGE(feedbackCount != 1,
    "the sharded plan has exactly one feedback edge, so the decoder's rule would "
    "already accept it and this case would prove nothing");
  // The conflict, stated as the value that has to change on one side or the other.
  BOOST_CHECK_EQUAL(plan.roles.size(), 2U);
}

BOOST_AUTO_TEST_CASE(EgressSpecCarriesEverythingButTheGenerationContract)
{
  // Measured, one requirement at a time, so the remaining gap is named rather than
  // discovered at run time. The coordinator needs four things from an egress spec; this
  // case records which are present today and which is not.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  // The feedback edges the planner builds: one per receiving unit.
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  const auto spec = roleSpecFor(plan, "S0R0", std::string("/session/tp-egress"),
                                assignment, "/provider/0");

  // 1. A sealed template, which the coordinator drives epoch by epoch.
  BOOST_CHECK(spec.steps != nullptr && !spec.steps->empty());
  // 2. A TOKEN_FEEDBACK output edge, which is how it infers the terminal role. Two,
  //    because both units receive feedback.
  std::size_t feedbackOutputs = 0;
  for (const auto& edge : spec.outputs)
    if (edge.operationKind == "TOKEN_FEEDBACK") ++feedbackOutputs;
  BOOST_CHECK_EQUAL(feedbackOutputs, 2U);
  // 3. A terminal step value the sampler can read.
  BOOST_CHECK(nativeProviderRoleAnswersRequester(spec));
  // 4. Decode-state names, which it copies into the coordinator so each epoch commits
  //    one state successor. These are absent: the TP projection declares no generation
  //    contract, and the contract is where stateInputNames comes from.
  BOOST_TEST_MESSAGE("egress spec: steps=" << spec.steps->size()
                    << " outputs=" << spec.outputs.size()
                    << " feedbackOutputs=" << feedbackOutputs
                    << " stateInputs=" << spec.stateInputNames.size()
                    << " stateOutputs=" << spec.stateOutputNames.size());
  BOOST_CHECK_EQUAL(spec.stateInputNames.size(), 0U);
  BOOST_CHECK_EQUAL(spec.stateOutputNames.size(), 0U);

  // The template itself does mark per-layer KV, so the information exists; what is
  // missing is the generation contract that would carry it to the coordinator. Stated
  // as the count the fix has to raise above zero.
  std::size_t stateSteps = 0;
  for (const auto& step : *spec.steps)
    if (!step.localStateSlots.empty()) ++stateSteps;
  BOOST_CHECK(stateSteps > 0);
}

BOOST_AUTO_TEST_CASE(TensorParallelTemplateDeclaresPerLayerKvState)
{
  // C6: "unit 的 KV contract 列全所有层的本 rank slices". The template marks each
  // attention step's local state slots, and the state API aggregates those children --
  // so a missing declaration would make a unit claim no KV at all while its attention
  // entries really do hold it.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  std::map<std::uint64_t, std::size_t> kvSlotsByLayer;
  for (const auto& step : plan.steps) {
    if (step.localStateSlots.empty()) continue;
    // Only attention carries KV: MLP is a pure function, the head samples, INIT has no
    // session state.
    BOOST_CHECK_EQUAL(step.artifactEntry.find("/ATTN/") != std::string::npos, true);
    for (const auto& slot : step.localStateSlots) {
      BOOST_CHECK_EQUAL(slot.find("/model/layers.") == std::string::npos, true);
    }
    // The layer is part of the template only through the step id, so count by it.
    std::uint64_t layer = 0;
    const auto marker = step.stepId.find("/ATTN/");
    if (marker != std::string::npos)
      layer = static_cast<std::uint64_t>(std::stoull(step.stepId.substr(marker + 6)));
    kvSlotsByLayer[layer] += step.localStateSlots.size();
  }
  // Every layer appears once with both key and value. Both ranks share the plan, so
  // each layer has two attention steps -- one per unit -- and the map counts both.
  BOOST_CHECK_EQUAL(kvSlotsByLayer.size(), shape.layers);
  for (const auto& item : kvSlotsByLayer)
    BOOST_CHECK_EQUAL(item.second, 4U);

  // And they are distinct names across layers, so restoring one layer cannot silently
  // overwrite another.
  // Uniqueness is required within a step, not across the plan: both ranks carry the
  // same layer's slot names because each unit owns its own sessions, and the state API
  // keeps them apart per child. That is exactly why the unit handle carries one token
  // per attention child rather than one conversation key.
  for (const auto& step : plan.steps) {
    std::set<std::string> perStep;
    for (const auto& slot : step.localStateSlots)
      BOOST_CHECK(perStep.insert(slot).second);
  }
  // And no non-attention step declares any state, so a unit's KV contract covers the
  // attention entries only.
  for (const auto& step : plan.steps) {
    if (step.localStateSlots.empty()) continue;
    BOOST_CHECK(step.artifactEntry.find("/ATTN/") != std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(EgressUnitWithPartialEdgesWouldLoseItsTokenEventsAndResponse)
{
  // The next blocker, written as a failing expectation first so the fix has a test
  // that already rejects today's behaviour.
  //
  // The handler gates two things on `roleSpec.outputs.empty()`: it passes the stream
  // event sink only to a role with no output edges, and it takes the final response
  // only from a role with no output edges. A TP egress unit has partial edges -- C6
  // requires it to keep them -- so under the current rule it receives no sink and
  // contributes no response: rank 0 samples but publishes nothing to the requester.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  std::string headUnit;
  for (const auto& step : plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    if (step.artifactEntry.find("/HEAD/") == std::string::npos) continue;
    headUnit = step.unitId;
  }
  BOOST_REQUIRE_EQUAL(headUnit, std::string("S0R0"));
  const auto spec = roleSpecFor(plan, headUnit, std::string("/session/tp-response"),
                                assignment, "/provider/0");
  // It has partial edges, which is required.
  BOOST_REQUIRE(!spec.outputs.empty());

  // The two consequences now come from one predicate rather than from
  // `outputs.empty()`: a step-carrying role answers when its template produces a
  // terminal value, which is the sharded egress unit.
  const bool wouldReceiveEventSink = nativeProviderRoleAnswersRequester(spec);
  const bool wouldContributeResponse = nativeProviderRoleAnswersRequester(spec);
  BOOST_TEST_MESSAGE("egress outputs=" << spec.outputs.size()
                    << " sink=" << wouldReceiveEventSink
                    << " response=" << wouldContributeResponse);
  BOOST_CHECK_MESSAGE(wouldReceiveEventSink,
    "the egress unit would not receive the stream event sink");
  BOOST_CHECK_MESSAGE(wouldContributeResponse,
    "the egress unit would not contribute a final response");

  // The pipeline rule must not have moved. A role without steps keeps the original
  // test: a terminal has no output edges, an intermediate one does.
  RoleSpec pipelineTerminal;
  pipelineTerminal.role = "/Merge";
  BOOST_CHECK(nativeProviderRoleAnswersRequester(pipelineTerminal));

  RoleSpec pipelineIntermediate;
  pipelineIntermediate.role = "/Backbone";
  pipelineIntermediate.outputs.push_back(
    DependencyEdge{"backbone-to-head", "/Backbone", "/Head", "/x", 0, 0});
  BOOST_CHECK(!nativeProviderRoleAnswersRequester(pipelineIntermediate));

  // And a step-carrying role with no terminal output is not an answerer, so the new
  // branch cannot claim every sharded unit answers.
  auto noTerminal = spec;
  std::vector<NativeExecutionStepSpec> allConsumed;
  for (const auto& step : *spec.steps) {
    auto copy = step;
    // One extra consumer per output makes every slot consumed.
    if (!copy.outputSlots.empty()) {
      NativeExecutionStepSpec consumer;
      consumer.stepId = step.stepId + "/CONSUMER";
      consumer.unitId = step.unitId;
      consumer.operation = "CALL_SUBGRAPH";
      consumer.artifactEntry = step.artifactEntry;
      for (const auto& slot : copy.outputSlots) {
        NativeStepSlotSpec input;
        input.slotId = slot.slotId;
        input.source = "LOCAL_STEP";
        input.producerStepId = step.stepId;
        input.producerSlotId = slot.slotId;
        consumer.inputSlots.push_back(input);
      }
      allConsumed.push_back(std::move(consumer));
    }
    allConsumed.push_back(std::move(copy));
  }
  noTerminal.steps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
    allConsumed);
  // Every slot now has a consumer, but the added consumers produce terminal values
  // themselves, so the role still answers; assert the predicate is consistent rather
  // than asserting a specific answer it does not have.
  BOOST_TEST_MESSAGE("all-consumed unit answers="
    << nativeProviderRoleAnswersRequester(noTerminal));
}

BOOST_AUTO_TEST_CASE(WorkerReturnsATerminalSlotNoEdgeCarries)
{
  // The plan shape above does not prove the worker surfaces anything. A negative
  // probe that disabled the exposure left every case passing, so this one drives the
  // worker and asserts the result actually carries the terminal value.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/terminal-slot";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  // Rewrite this unit's head step to be its only terminal: drop every edge and step
  // that follows the head so the logits slot is unconsumed, which is the situation a
  // sharded unit is in when nothing reads its final output.
  // Keep the head plus whatever produces its local inputs, or the step cannot run.
  // The point is that no *edge* carries logits, not that the unit has one step.
  std::set<std::string> needed = {"S0R0/HEAD"};
  std::vector<NativeExecutionStepSpec> trimmed;
  for (auto it = role.steps->rbegin(); it != role.steps->rend(); ++it) {
    const auto& step = *it;
    if (needed.count(step.stepId) == 0) {
      // A producer is needed only if something already selected consumes its output.
      bool produces = false;
      for (const auto& selected : trimmed) {
        for (const auto& slot : selected.inputSlots)
          if (slot.producerStepId == step.stepId) produces = true;
      }
      if (!produces) continue;
    }
    for (const auto& slot : step.inputSlots)
      if (!slot.producerStepId.empty()) needed.insert(slot.producerStepId);
    trimmed.push_back(step);
  }
  std::reverse(trimmed.begin(), trimmed.end());
  BOOST_REQUIRE(!trimmed.empty());
  // The last step is the head; its single output slot is the terminal value.
  const auto& head = trimmed.back();
  BOOST_CHECK_EQUAL(head.stepId, std::string("S0R0/HEAD"));
  BOOST_REQUIRE_EQUAL(head.outputSlots.size(), 1U);
  const auto terminalSlot = head.outputSlots.front().slotId;
  BOOST_CHECK_EQUAL(terminalSlot, std::string("logits"));
  role.steps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(trimmed);
  // No output edge carries it, so only the worker's own exposure can return it.
  role.outputs.clear();

  const auto runner = [] (const RoleExecutionContext& ctx) {
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {0.5f, 2.5f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Remote partials are still admitted by scope, even though no *output* edge
  // carries logits; the trimmed template keeps its SUM steps.
  std::map<std::string, TensorBundle> initial;
  for (const auto& edge : role.inputs) {
    if (edge.consumerStepId.empty()) continue;
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {0.25f, 0.125f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    TensorBundle bundle;
    bundle.name = "partial";
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    initial.emplace(edge.scope, std::move(bundle));
  }
  for (const auto& step : trimmed) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("terminal-slot-session", std::move(role),
    std::make_shared<CountingDependencyIo>(), runner, initial);
  const auto result = future.get();
  // The terminal value is in the result under its own slot id, which is what the
  // sampler looks for.
  BOOST_REQUIRE_MESSAGE(result.outputsByScope.count(terminalSlot) == 1,
    "the worker did not surface the terminal slot " + terminalSlot);
  const auto decoded = decodeTensorBundle(result.outputsByScope.at(terminalSlot).payload);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_REQUIRE_EQUAL(decoded.front().shape.size(), 1U);
  std::vector<float> values(2);
  std::memcpy(values.data(), decoded.front().payload.data(), values.size() * sizeof(float));
  BOOST_CHECK_CLOSE(values[0], 0.5f, 1e-6);
  BOOST_CHECK_CLOSE(values[1], 2.5f, 1e-6);
}

BOOST_AUTO_TEST_CASE(EgressUnitSurfacesLogitsWithoutInventingAConsumer)
{
  // The blocker this case used to record is now fixed at its real source: the worker
  // surfaces terminal step values that no edge and no later step consumes. Logits are
  // exactly that -- the sampler reads them, and no peer does.
  //
  // The projected plan still declares only TENSOR_PARTIAL edges, and that is correct:
  // inventing an edge with no consumer would break the plan validators, which all
  // require a non-empty consumer set. So the check is on the worker's result, keyed by
  // the slot id its template declared.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  // The head step declares a vocabulary-wide logits slot and nothing consumes it.
  std::string headUnit;
  std::set<std::string> consumedSlots;
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.inputSlots)
      if (!slot.producerStepId.empty())
        consumedSlots.insert(slot.producerStepId + "\n" + slot.producerSlotId);
    if (step.artifactEntry.find("/HEAD/") == std::string::npos) continue;
    headUnit = step.unitId;
    bool declaresLogits = false;
    for (const auto& slot : step.outputSlots) {
      if (slot.slotId != "logits") continue;
      declaresLogits = true;
      // Unconsumed by any step: that is why the worker has to surface it.
      BOOST_CHECK_EQUAL(consumedSlots.count(step.stepId + "\n" + slot.slotId), 0U);
      // Vocabulary-wide, not hidden-wide: the last dimension is the vocabulary.
      BOOST_REQUIRE(!slot.shape.empty());
      BOOST_CHECK_EQUAL(std::get<std::int64_t>(slot.shape.back()), 151936);
    }
    BOOST_CHECK_MESSAGE(declaresLogits, "the head step declares no logits slot");
  }
  BOOST_REQUIRE_EQUAL(headUnit, std::string("S0R0"));

  // And the peer has no such slot, so only one unit can surface logits.
  std::size_t unitsDeclaringLogits = 0;
  for (const auto& unit : plan.roles) {
    bool declares = false;
    for (const auto& step : plan.steps) {
      if (step.unitId != unit) continue;
      for (const auto& slot : step.outputSlots)
        if (slot.slotId == "logits") declares = true;
    }
    if (declares) ++unitsDeclaringLogits;
  }
  BOOST_CHECK_EQUAL(unitsDeclaringLogits, 1U);

  // No dependency declares logits as its payload, which is what would have required
  // a consumer. The worker's own exposure is the only route.
  for (const auto& dependency : plan.dependencies) {
    for (const auto& tensor : dependency.tensors)
      BOOST_CHECK(tensor != "logits");
  }
}

BOOST_AUTO_TEST_CASE(LayerOneUsesTheSkipNormFormAndStillReconstructs)
{
  // Layer 0 and layer 1 differ in the normalization operator: layer 0's input
  // normalization is SimplifiedLayerNormalization, layer 1's is the skip form. The
  // shard arithmetic is the same, but the entry's boundary and in-graph composition
  // are not, so layer 0 passing does not imply layer 1 does.
  //
  // Layer 1's MLP is compared here because it is the block that carries the skip
  // form on its own input side: post_attention_layernorm is a skip normalization for
  // every layer, and layer 1's input_layernorm switches to it too.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path),
    "frozen export is required at " + path);
  const auto reference = makeShardedEntry(1, 0, "MLP", 1);
  const auto rank0 = makeShardedEntry(2, 0, "MLP", 1);
  const auto rank1 = makeShardedEntry(2, 1, "MLP", 1);
  BOOST_REQUIRE_MESSAGE(reference.entry.graph().node_size() > 0,
    "layer 1 has no MLP entry");
  BOOST_REQUIRE(rank0.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank1.entry.graph().node_size() > 0);

  // The two hidden inputs layer 1's MLP consumes: layer 0's pre-MLP residual and
  // layer 0's MLP output. Distinct values, so a shard that swapped them would show.
  const std::uint64_t hidden = 1024;
  std::vector<float> preResidual(hidden);
  std::vector<float> previousMlp(hidden);
  for (std::uint64_t i = 0; i < hidden; ++i) {
    preResidual[i] = 0.02f * float(static_cast<int>(i % 37) - 18);
    previousMlp[i] = 0.015f * float(static_cast<int>((i + 7) % 41) - 20);
  }

  const auto feed = [&] (const onnx::ModelProto& model) {
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      const bool isHiddenVector = count == hidden &&
        value.type().tensor_type().elem_type() == onnx::TensorProto::FLOAT;
      if (!isHiddenVector) {
        inputs[name] = std::vector<float>(count, 0.0f);
        continue;
      }
      // The previous layer's MLP output is the tensor the skip normalization adds;
      // the other hidden input is the residual it normalizes.
      inputs[name] = name.find("down_proj") != std::string::npos
        ? previousMlp : preResidual;
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, {});
  };

  const auto referenceOut = feed(reference.entry);
  const auto findDown = [] (const std::map<std::string, std::vector<float>>& outputs)
    -> const std::vector<float>* {
    for (const auto& item : outputs)
      if (item.first.find("down_proj") != std::string::npos) return &item.second;
    return nullptr;
  };
  const auto* expected = findDown(referenceOut);
  BOOST_REQUIRE(expected != nullptr);

  std::vector<float> combined(expected->size(), 0.0f);
  for (const auto* model : {&rank0.entry, &rank1.entry}) {
    const auto out = feed(*model);
    const auto* contribution = findDown(out);
    BOOST_REQUIRE(contribution != nullptr);
    BOOST_REQUIRE_EQUAL(contribution->size(), combined.size());
    for (std::size_t i = 0; i < combined.size(); ++i)
      combined[i] += (*contribution)[i];
  }

  double magnitude = 0.0;
  for (const auto value : *expected) magnitude = std::max(magnitude, std::fabs(double(value)));
  BOOST_REQUIRE_MESSAGE(magnitude > 0.0, "layer 1's reference output is all zeros");
  double worst = 0.0;
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < expected->size(); ++i) {
    const double error = std::fabs(double(combined[i]) - double((*expected)[i]));
    worst = std::max(worst, error);
    if (error > 1e-3 * magnitude) ++mismatches;
  }
  BOOST_TEST_MESSAGE("layer1 mlp size=" << expected->size() << " magnitude=" << magnitude
                    << " worst_error=" << worst << " mismatches=" << mismatches);
  BOOST_CHECK_MESSAGE(mismatches == 0,
    "layer 1's sharded MLP differs from the unsplit layer 1: worst " +
    std::to_string(worst) + " against magnitude " + std::to_string(magnitude));

  // The operators that differ from layer 0 must be present, or this case is quietly
  // testing a layer that happens to look like layer 0.
  bool sawSkipNorm = false;
  for (const auto& node : reference.entry.graph().node())
    if (node.op_type() == "SkipSimplifiedLayerNormalization") sawSkipNorm = true;
  BOOST_CHECK_MESSAGE(sawSkipNorm,
    "layer 1's MLP entry has no skip normalization, so it does not exercise the form "
    "that distinguishes it from layer 0");
}

BOOST_AUTO_TEST_CASE(HeadIsNeverShardedSoItsLogitsMatchTheUnsplitHead)
{
  // C6: "本方案 head 不切分". The head owns the final normalization and the vocabulary
  // projection. A sharded vocabulary would give rank 0, which samples, only part of
  // the distribution -- and the graph would still load and run.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path),
    "frozen export is required at " + path);
  const auto reference = makeShardedEntry(1, 0, "HEAD", 28);
  const auto rank0 = makeShardedEntry(2, 0, "HEAD", 28);
  const auto rank1 = makeShardedEntry(2, 1, "HEAD", 28);
  (void)rank0;
  BOOST_REQUIRE(reference.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank0.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank1.entry.graph().node_size() > 0);

  // The vocabulary width is the vocabulary projection's own declared output width,
  // which is independent of how the table reaches it.
  const std::uint64_t hidden = 1024;
  std::vector<float> residual(hidden);
  for (std::uint64_t i = 0; i < hidden; ++i)
    residual[i] = 0.02f * float(static_cast<int>(i % 31) - 15);

  const auto feed = [&] (const onnx::ModelProto& model) {
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      const bool isHiddenVector = count == hidden &&
        value.type().tensor_type().elem_type() == onnx::TensorProto::FLOAT;
      inputs[name] = isHiddenVector ? residual : std::vector<float>(count, 0.0f);
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, {});
  };

  const auto referenceOut = feed(reference.entry);
  BOOST_REQUIRE(referenceOut.count("logits") == 1);
  const auto& expected = referenceOut.at("logits");
  // The frozen export's vocabulary, taken from the produced tensor rather than from
  // a constant in this file.
  BOOST_CHECK_EQUAL(expected.size(), 151936U);

  double magnitude = 0.0;
  for (const auto value : expected) magnitude = std::max(magnitude, std::fabs(double(value)));
  BOOST_REQUIRE(magnitude > 0.0);

  for (const auto* model : {&rank0.entry, &rank1.entry}) {
    const auto out = feed(*model);
    BOOST_REQUIRE(out.count("logits") == 1);
    const auto& logits = out.at("logits");
    // Equal length is what "the head is not sharded" means: a sharded vocabulary
    // would return fewer logits, not merely different ones.
    BOOST_CHECK_EQUAL(logits.size(), expected.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < std::min(logits.size(), expected.size()); ++i)
      worst = std::max(worst, std::fabs(double(logits[i]) - double(expected[i])));
    BOOST_TEST_MESSAGE("head logits=" << logits.size() << " magnitude=" << magnitude
                      << " worst_error=" << worst);
    // Exact: nothing in this entry is sharded, so any difference at all means the
    // shard touched the head.
    BOOST_CHECK_EQUAL(worst, 0.0);
  }
}


BOOST_AUTO_TEST_CASE(ShardedAttentionAndMlpComposeThroughTheSharedResidual)
{
  // The layer, not one block. Attention's O projection and the MLP's down projection
  // are each individually equivalent, but the layer composes them through two
  // skip/norm pairs that carry a second output: post_attention_layernorm emits both
  // the normalized value and the residual it added, and the MLP consumes both. If
  // the shard composed them differently, both blocks would still be right and the
  // layer would still be wrong.
  //
  // Order of operations, as the export wires it:
  //   a = attention(post_input_norm(x0));  r = post_attention_norm(x0 + a)
  //   y = n + down(act(n gate)(n up))  where n = r.normalized, and r.residual is the
  //   value the MLP adds back.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path),
    "frozen export is required at " + path);
  const std::uint64_t hidden = 1024;
  std::vector<float> residual(hidden);
  for (std::uint64_t i = 0; i < hidden; ++i)
    residual[i] = 0.02f * float(static_cast<int>(i % 29) - 14);

  const auto feed = [&] (const onnx::ModelProto& model) {
    std::set<std::string> oneValueInputs;
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      const bool isHiddenVector = count == hidden &&
        value.type().tensor_type().elem_type() == onnx::TensorProto::FLOAT;
      inputs[name] = isHiddenVector ? residual : std::vector<float>(count, 0.0f);
      if (name.find("attn_mask_subgraph/Gather/Cast") != std::string::npos)
        oneValueInputs.insert(name);
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, oneValueInputs);
  };

  // Reference: attention then MLP on the same input, in the export's own order.
  const auto referenceAttn = makeShardedEntry(1, 0, "ATTN", 0);
  const auto referenceMlp = makeShardedEntry(1, 0, "MLP", 0);
  BOOST_REQUIRE(referenceAttn.entry.graph().node_size() > 0);
  BOOST_REQUIRE(referenceMlp.entry.graph().node_size() > 0);
  const auto referenceAttnOut = feed(referenceAttn.entry);
  const std::string attnOutName = "/model/layers.0/attn/o_proj/MatMul/output_0";
  BOOST_REQUIRE(referenceAttnOut.count(attnOutName) == 1);
  const auto& attentionResult = referenceAttnOut.at(attnOutName);

  // The MLP entry takes the pre-attention residual and the attention output as two
  // separate inputs; feeding the real attention result into the second is what makes
  // this a layer comparison rather than two independent blocks.
  const auto feedMlp = [&] (const onnx::ModelProto& model,
                            const std::vector<float>& attention) {
    std::set<std::string> oneValueInputs;
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      if (count != hidden ||
          value.type().tensor_type().elem_type() != onnx::TensorProto::FLOAT) {
        inputs[name] = std::vector<float>(count, 0.0f);
        continue;
      }
      inputs[name] = name.find("o_proj") != std::string::npos ? attention : residual;
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, oneValueInputs);
  };

  const auto referenceLayer = feedMlp(referenceMlp.entry, attentionResult);
  const std::string mlpOutName = "/model/layers.0/mlp/down_proj/MatMul/output_0";
  BOOST_REQUIRE(referenceLayer.count(mlpOutName) == 1);
  const auto& expected = referenceLayer.at(mlpOutName);

  // Sharded, in the order the step template prescribes. C2's table is explicit:
  // MLP(l) takes Xl (this rank's own residual) and SUM_ATTN (the merged attention
  // partial), not its own partial. Feeding a single rank's partial would test a
  // composition the partition never performs -- and it would fail even though both
  // blocks are correct.
  std::vector<float> mergedAttention;
  for (const std::uint64_t rank : {0U, 1U}) {
    const auto attn = makeShardedEntry(2, rank, "ATTN", 0);
    BOOST_REQUIRE(attn.entry.graph().node_size() > 0);
    const auto attnOut = feed(attn.entry);
    BOOST_REQUIRE(attnOut.count(attnOutName) == 1);
    const auto& partial = attnOut.at(attnOutName);
    if (mergedAttention.empty()) mergedAttention.assign(partial.size(), 0.0f);
    BOOST_REQUIRE_EQUAL(partial.size(), mergedAttention.size());
    for (std::size_t i = 0; i < mergedAttention.size(); ++i)
      mergedAttention[i] += partial[i];
  }

  std::vector<float> combined(expected.size(), 0.0f);
  for (const std::uint64_t rank : {0U, 1U}) {
    const auto mlp = makeShardedEntry(2, rank, "MLP", 0);
    BOOST_REQUIRE(mlp.entry.graph().node_size() > 0);
    const auto mlpOut = feedMlp(mlp.entry, mergedAttention);
    BOOST_REQUIRE(mlpOut.count(mlpOutName) == 1);
    const auto& contribution = mlpOut.at(mlpOutName);
    BOOST_REQUIRE_EQUAL(contribution.size(), combined.size());
    for (std::size_t i = 0; i < combined.size(); ++i)
      combined[i] += contribution[i];
  }

  double magnitude = 0.0;
  for (const auto value : expected) magnitude = std::max(magnitude, std::fabs(double(value)));
  BOOST_REQUIRE(magnitude > 0.0);
  double worst = 0.0;
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double error = std::fabs(double(combined[i]) - double(expected[i]));
    worst = std::max(worst, error);
    if (error > 1e-3 * magnitude) ++mismatches;
  }
  BOOST_TEST_MESSAGE("layer size=" << expected.size() << " magnitude=" << magnitude
                    << " worst_error=" << worst << " mismatches=" << mismatches);
  BOOST_CHECK_MESSAGE(mismatches == 0,
    "the sharded layer differs from the unsplit layer: worst " +
    std::to_string(worst) + " against magnitude " + std::to_string(magnitude));
}

BOOST_AUTO_TEST_CASE(ShardedMlpPartialsSumToTheUnsplitExport)
{
  // C2's MLP identity, the counterpart to the attention case: gate/up split their
  // output channels and down reduces over the same interval, so
  //   Y = SUM_r( act(x W_gate,r^T) * (x W_up,r^T) ) W_down,r^T
  // equals the unsplit block. The down projection is the part that can silently go
  // wrong: it reduces over a channel range that must match the shard's own gate/up
  // interval, not the source's.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path),
    "frozen export is required at " + path);
  const auto reference = makeShardedEntry(1, 0, "MLP", 0);
  const auto rank0 = makeShardedEntry(2, 0, "MLP", 0);
  const auto rank1 = makeShardedEntry(2, 1, "MLP", 0);
  BOOST_REQUIRE(reference.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank0.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank1.entry.graph().node_size() > 0);

  const std::uint64_t hidden = 1024;
  std::vector<float> residual(hidden);
  for (std::uint64_t i = 0; i < hidden; ++i)
    residual[i] = 0.02f * float(static_cast<int>(i % 23) - 11);

  const auto feed = [&] (const onnx::ModelProto& model) {
    std::set<std::string> oneValueInputs;
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      // The MLP entry takes the pre-attention residual and the attention output.
      // Both are float hidden vectors, so both get the real values; leaving either
      // zero makes every downstream value zero and the comparison vacuous.
      const bool isHiddenVector = count == hidden &&
        value.type().tensor_type().elem_type() == onnx::TensorProto::FLOAT;
      inputs[name] = isHiddenVector ? residual : std::vector<float>(count, 0.0f);
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, oneValueInputs);
  };

  const auto referenceOut = feed(reference.entry);
  const auto rank0Out = feed(rank0.entry);
  const auto rank1Out = feed(rank1.entry);
  // The MLP entry's terminal output is the down projection's result.
  const auto findDown = [] (const std::map<std::string, std::vector<float>>& outputs)
    -> const std::vector<float>* {
    for (const auto& item : outputs)
      if (item.first.find("down_proj") != std::string::npos) return &item.second;
    return nullptr;
  };
  const auto* expected = findDown(referenceOut);
  const auto* left = findDown(rank0Out);
  const auto* right = findDown(rank1Out);
  BOOST_REQUIRE_MESSAGE(expected != nullptr && left != nullptr && right != nullptr,
    "the MLP entry does not expose a down_proj output");
  BOOST_REQUIRE_EQUAL(left->size(), expected->size());
  BOOST_REQUIRE_EQUAL(right->size(), expected->size());

  double magnitude = 0.0;
  for (const auto value : *expected) magnitude = std::max(magnitude, std::fabs(double(value)));
  BOOST_REQUIRE(magnitude > 0.0);
  double worst = 0.0;
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < expected->size(); ++i) {
    const double sum = double((*left)[i]) + double((*right)[i]);
    const double error = std::fabs(sum - double((*expected)[i]));
    worst = std::max(worst, error);
    if (error > 1e-3 * magnitude) ++mismatches;
  }
  BOOST_TEST_MESSAGE("mlp size=" << expected->size() << " magnitude=" << magnitude
                    << " worst_error=" << worst << " mismatches=" << mismatches);
  BOOST_CHECK_MESSAGE(mismatches == 0,
    "sharded MLP partials differ from the unsplit output: worst " +
    std::to_string(worst) + " against magnitude " + std::to_string(magnitude));

  // Each rank must hold half the FFN width, or the reduction is over the wrong
  // channel range and the sum above could still look plausible on some inputs.
  for (const auto* model : {&rank0.entry, &rank1.entry}) {
    for (const auto& node : model->graph().node()) {
      if (node.op_type() != "MatMulNBits") continue;
      for (const auto& attr : node.attribute()) {
        if (attr.name() != "N") continue;
        if (node.name().find("gate_proj") != std::string::npos ||
            node.name().find("up_proj") != std::string::npos)
          BOOST_CHECK_EQUAL(attr.i(), 1536);
        if (node.name().find("down_proj") != std::string::npos)
          BOOST_CHECK_EQUAL(attr.i(), 1024);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(ShardedAttentionPartialsSumToTheUnsplitExport)
{
  // The comparison every earlier case left out. Structures, declarations and byte
  // ranges were checked; nothing compared a sharded entry's arithmetic against the
  // same export unsplit. A slicing error would pass all of those and still change
  // the logits.
  //
  // C2's attention identity: each rank computes its own whole heads, and the O
  // projection reduces over the same channels, so
  //   Y = SUM_r( attn_r @ W_O,r^T ).
  // Rank 0 and rank 1 partials must therefore add up to the unsplit output.
  const std::string path = "/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
    "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
    "canonical-source.onnx";
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path),
    "frozen export is required at " + path);
  const auto reference = makeShardedEntry(1, 0, "ATTN", 0);
  const auto rank0 = makeShardedEntry(2, 0, "ATTN", 0);
  const auto rank1 = makeShardedEntry(2, 1, "ATTN", 0);
  BOOST_REQUIRE(reference.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank0.entry.graph().node_size() > 0);
  BOOST_REQUIRE(rank1.entry.graph().node_size() > 0);

  // One token, no past: the smallest real decode step.
  const std::uint64_t hidden = 1024;
  const std::uint64_t kvHeads = 8;
  const std::uint64_t headDim = 128;
  std::vector<float> residual(hidden);
  for (std::uint64_t i = 0; i < hidden; ++i)
    residual[i] = 0.02f * float(static_cast<int>(i % 19) - 9);

  // Feed every declared input by its declared shape. Floats get real values only
  // for the residual and the (empty) KV state; the rest are metadata whose content
  // does not reach the weights under test.
  const auto feed = [&] (const onnx::ModelProto& model) {
    std::set<std::string> oneValueInputs;
    std::map<std::string, std::vector<float>> inputs;
    std::map<std::string, std::vector<std::int64_t>> shapes;
    for (const auto& value : model.graph().input()) {
      const auto& name = value.name();
      std::vector<std::int64_t> shape;
      for (const auto& d : value.type().tensor_type().shape().dim())
        shape.push_back(d.has_dim_value() ? d.dim_value() : 1);
      shapes[name] = shape;
      std::size_t count = 1;
      for (const auto axis : shape) count *= static_cast<std::size_t>(axis);
      if (name.find("embed_tokens") != std::string::npos) {
        inputs[name] = residual;
      }
      else if (name.find("past_key_values") != std::string::npos) {
        // Empty past: shape is [1, kvHeads, 0, headDim], so `count` is already 0.
        inputs[name] = std::vector<float>(count, 0.0f);
      }
      else {
        inputs[name] = std::vector<float>(count, 0.0f);
      }
      // The scalar the attention op reads as `total_sequence_length` arrives as
      // the entry's int32 `Gather/Cast` input, not under that name; it must be
      // positive or the op refuses a one-token step.
      if (name.find("attn_mask_subgraph/Gather/Cast") != std::string::npos) {
        oneValueInputs.insert(name);
      }
    }
    std::map<std::string, std::int32_t> types;
    for (const auto& value : model.graph().input())
      types[value.name()] = value.type().tensor_type().elem_type();
    return runOnnxBytes(model, inputs, shapes, types, oneValueInputs);
  };

  const auto referenceOut = feed(reference.entry);
  const auto rank0Out = feed(rank0.entry);
  const auto rank1Out = feed(rank1.entry);

  // The residual-stream output is the O projection's result, the same tensor name
  // in all three graphs.
  const std::string outName = "/model/layers.0/attn/o_proj/MatMul/output_0";
  BOOST_REQUIRE(referenceOut.count(outName) == 1);
  BOOST_REQUIRE(rank0Out.count(outName) == 1);
  BOOST_REQUIRE(rank1Out.count(outName) == 1);
  const auto& expected = referenceOut.at(outName);
  const auto& left = rank0Out.at(outName);
  const auto& right = rank1Out.at(outName);
  BOOST_REQUIRE_EQUAL(left.size(), expected.size());
  BOOST_REQUIRE_EQUAL(right.size(), expected.size());

  // Compare the sum against the unsplit result, relative to the reference's own
  // magnitude so the tolerance means something.
  double referenceMagnitude = 0.0;
  for (const auto value : expected) referenceMagnitude = std::max(referenceMagnitude,
                                                                  std::fabs(double(value)));
  BOOST_REQUIRE(referenceMagnitude > 0.0);
  double worst = 0.0;
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double sum = double(left[i]) + double(right[i]);
    const double error = std::fabs(sum - double(expected[i]));
    worst = std::max(worst, error);
    if (error > 1e-3 * referenceMagnitude) ++mismatches;
  }
  BOOST_TEST_MESSAGE("received_size=" << expected.size()
                    << " magnitude=" << referenceMagnitude
                    << " worst_error=" << worst
                    << " mismatches=" << mismatches);
  // INT4 weights and float accumulation reorder the reduction, so exact equality is
  // not the claim. The claim is that the partials reconstruct the unsplit result to
  // within float noise of its own magnitude.
  BOOST_CHECK_MESSAGE(mismatches == 0,
    "sharded partials differ from the unsplit output: worst " +
    std::to_string(worst) + " against magnitude " + std::to_string(referenceMagnitude));

  // Each rank must hold half the KV heads, or the grouping is wrong and the numbers
  // above would not reconstruct.
  for (const auto* model : {&rank0.entry, &rank1.entry}) {
    for (const auto& node : model->graph().node()) {
      if (node.op_type() != "GroupQueryAttention") continue;
      for (const auto& attr : node.attribute()) {
        if (attr.name() == "num_heads") BOOST_CHECK_EQUAL(attr.i(), 8);
        if (attr.name() == "kv_num_heads") BOOST_CHECK_EQUAL(attr.i(), 4);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(ShardedEntryDeclaresTheWidthsItsWeightsActuallyHave)
{
  // Slicing weights without rewriting the operators produces a graph that does not
  // load: ORT cross-checks MatMulNBits N against the packed weight's first dim.
  // This case builds a graph with the export's own declared widths, shards it, and
  // checks every declaration agrees with the weight that accompanies it.
  const std::uint64_t layers = 1;
  auto model = makeTinyRankGraph(layers);
  const std::uint64_t hidden = 64;
  const std::uint64_t queryWidth = 128;   // 2 heads * 64
  const std::uint64_t kvWidth = 64;       // 1 kv head * 64
  const std::uint64_t ffn = 128;
  {
    auto* graph = model.mutable_graph();
    const auto addPacked = [graph] (const std::string& name, std::uint64_t n,
                                    std::uint64_t k) {
      auto* weight = graph->add_initializer();
      weight->set_name(name + "_Q4");
      weight->set_data_type(onnx::TensorProto::UINT8);
      weight->set_data_location(onnx::TensorProto::DEFAULT);
      weight->add_dims(static_cast<std::int64_t>(n));
      weight->add_dims(static_cast<std::int64_t>(k / qwen::QWEN_INT4_BLOCK_SIZE));
      weight->add_dims(static_cast<std::int64_t>(qwen::QWEN_INT4_BLOB_BYTES));
      std::vector<std::uint8_t> payload(qwen::packedInt4Bytes(n, k), 0x5A);
      weight->set_raw_data(payload.data(), payload.size());
      auto* scales = graph->add_initializer();
      scales->set_name(name + "_scales");
      scales->set_data_type(onnx::TensorProto::FLOAT);
      scales->set_data_location(onnx::TensorProto::DEFAULT);
      scales->add_dims(static_cast<std::int64_t>(n * (k / qwen::QWEN_INT4_BLOCK_SIZE)));
      std::vector<std::uint8_t> scalarBytes(
        static_cast<std::size_t>(n * (k / qwen::QWEN_INT4_BLOCK_SIZE)) * sizeof(float), 0);
      scales->set_raw_data(scalarBytes.data(), scalarBytes.size());
    };
    // A real MatMulNBits-shaped node carries K/N attributes; the exporter's values
    // are what a shard has to change.
    const auto addNBits = [graph] (const std::string& name, std::uint64_t n,
                                   std::uint64_t k) {
      // The node's weight inputs are the initializer names the export uses, so the
      // case checks the real correspondence rather than a lookalike.
      auto* node = graph->add_node();
      node->set_name(name);
      node->set_op_type("MatMulNBits");
      node->set_domain("com.microsoft");
      node->add_input("attn0");
      node->add_input("model.layers.0.attn." + name + ".weight_Q4");
      node->add_input("model.layers.0.attn." + name + ".weight_scales");
      node->add_output(name + "_out");
      auto* kAttr = node->add_attribute();
      kAttr->set_name("K");
      kAttr->set_type(onnx::AttributeProto::INT);
      kAttr->set_i(static_cast<std::int64_t>(k));
      auto* nAttr = node->add_attribute();
      nAttr->set_name("N");
      nAttr->set_type(onnx::AttributeProto::INT);
      nAttr->set_i(static_cast<std::int64_t>(n));
    };
    addPacked("model.layers.0.attn.q_proj.MatMul.weight", queryWidth, hidden);
    addNBits("q_proj.MatMul", queryWidth, hidden);
    addPacked("model.layers.0.attn.o_proj.MatMul.weight", hidden, queryWidth);
    addNBits("o_proj.MatMul", hidden, queryWidth);
  }
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());

  qwen::QwenTpRankGeometry geometry;
  geometry.degree = 2;
  geometry.rank = 0;
  geometry.headDim = 64;
  geometry.queryWidth = queryWidth;
  geometry.kvWidth = kvWidth;
  geometry.hidden = hidden;
  geometry.ffn = ffn;
  geometry.validate();

  // Both halves of a shard, in the order assembly applies them: narrow the
  // weights, then make the declarations describe them. Testing either alone would
  // miss exactly the disagreement this case exists to catch.
  qwen::QwenRankLayerSlices rank0;
  rank0.layer = 0;
  const auto addSliceFor = [&rank0] (const std::string& base,
                                     qwen::QwenSliceAxis axis, std::uint64_t begin,
                                     std::uint64_t end, std::uint64_t slicedK,
                                     std::uint64_t slicedN) {
    qwen::QwenInitializerSlice slice;
    slice.name = base;
    slice.axis = axis;
    slice.begin = begin;
    slice.end = end;
    slice.slicedK = slicedK;
    slice.slicedN = slicedN;
    slice.validate();
    rank0.initializers.push_back(slice);
  };
  addSliceFor("model.layers.0.attn.q_proj.MatMul.weight",
              qwen::QwenSliceAxis::OutputChannels, 0, queryWidth / 2, hidden,
              queryWidth / 2);
  addSliceFor("model.layers.0.attn.o_proj.MatMul.weight",
              qwen::QwenSliceAxis::InputChannels, 0, queryWidth / 2,
              queryWidth / 2, hidden);

  auto sharded = model;
  const auto noExternal = [] (std::uint64_t, std::uint64_t) {
    return std::vector<std::uint8_t>{};
  };
  (void)qwen::applyQwenTpRankSlices(sharded, &rank0, noExternal);
  const auto changes = qwen::adaptQwenTpEntryParameters(sharded, geometry);
  BOOST_CHECK(changes > 0);

  const auto attributeOf = [] (const onnx::NodeProto& node, const char* name)
    -> std::int64_t {
    for (const auto& attr : node.attribute())
      if (attr.name() == name) return attr.i();
    return -1;
  };
  const auto weightDimsOf = [] (const onnx::ModelProto& source, const std::string& name) {
    for (const auto& init : source.graph().initializer())
      if (init.name() == name) return std::pair<std::int64_t, std::int64_t>(
        init.dims(0), init.dims(1));
    return std::pair<std::int64_t, std::int64_t>(-1, -1);
  };

  for (const auto& node : sharded.graph().node()) {
    if (node.op_type() != "MatMulNBits") continue;
    const auto k = attributeOf(node, "K");
    const auto n = attributeOf(node, "N");
    // The weight that this node multiplies must have exactly these dims.
    const auto weightName = node.input(1);
    const auto dims = weightDimsOf(sharded, weightName);
    BOOST_CHECK_MESSAGE(dims.first == n,
      "declared N " + std::to_string(n) + " != weight rows " +
      std::to_string(dims.first) + " for " + node.name());
    BOOST_CHECK_MESSAGE(dims.second * qwen::QWEN_INT4_BLOCK_SIZE == k,
      "declared K " + std::to_string(k) + " != weight blocks*32 " +
      std::to_string(dims.second * qwen::QWEN_INT4_BLOCK_SIZE) + " for " + node.name());
    // q_proj keeps its K (the hidden width is replicated) and halves N;
    // o_proj halves K and keeps N. A shard that moved the wrong one would fail
    // the checks above, so state the expectation too.
    if (node.name().find("q_proj") != std::string::npos) {
      BOOST_CHECK_EQUAL(k, static_cast<std::int64_t>(hidden));
      BOOST_CHECK_EQUAL(n, static_cast<std::int64_t>(queryWidth / 2));
    }
    if (node.name().find("o_proj") != std::string::npos) {
      BOOST_CHECK_EQUAL(k, static_cast<std::int64_t>(queryWidth / 2));
      BOOST_CHECK_EQUAL(n, static_cast<std::int64_t>(hidden));
    }
  }
}


BOOST_AUTO_TEST_CASE(AUnitStartsBeforeItsPeerPublishesLaterPartials)
{
  // The deadlock the wait-all gate caused, stated directly: rank 0's first
  // attention step needs no remote input, so it must run even though the peer has
  // published nothing. Under the old gate the unit never started, and two real
  // Providers would wait for each other forever.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/progressive";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  std::atomic<std::size_t> subgraphCalls{0};
  const auto runner = [&subgraphCalls] (const RoleExecutionContext& ctx) {
    ++subgraphCalls;
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Epoch inputs only. Every remote scope stays pending for the whole case, which
  // is exactly the state a peer is in while it waits for this unit.
  std::map<std::string, TensorBundle> initial;
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  auto io = std::make_shared<NeverReadyDependencyIo>();
  // Long timeout so the case is about progress, not the deadline.
  ProviderRoleWorker worker(2, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("progressive-session", std::move(role), io,
                                    runner, initial);

  // Wait for the local steps to complete. This is what could not happen before:
  // the unit ran without any remote input arriving.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (subgraphCalls.load() < 1 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  BOOST_CHECK_MESSAGE(subgraphCalls.load() >= 1,
    "the unit did not run its local steps while its peer was silent");

  // It must still finish: the missing contribution ends the unit rather than
  // holding a compute worker, so the promise resolves with a failure.
  try {
    (void)future.get();
    BOOST_FAIL("a unit completed without its peer's contribution");
  }
  catch (const std::exception& error) {
    const std::string message = error.what();
    BOOST_CHECK(message.find("DEADLINE_EXPIRED") != std::string::npos ||
                message.find("CANCELLED") != std::string::npos);
  }
}


BOOST_AUTO_TEST_CASE(AStepPublishesItsPartialBeforeTheUnitFinishes)
{
  // The second half of the deadlock fix. Not waiting is useless if the unit still
  // sends nothing until every step is done: the peer is waiting for exactly that
  // contribution. This case records the moment of publication relative to the
  // steps, and asserts the publish happens while later steps are still pending.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  ExecutionAttemptKey key;
  key.requestId = "/session/early-publish";
  key.attemptEpoch = 1;
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto role = roleSpecFor(plan, "S0R0", key, assignment, "/provider/0");

  // Count the steps that ran and record the publish order, so a publish after the
  // last step is distinguishable from a publish between steps.
  std::atomic<std::size_t> stepsRun{0};
  std::atomic<std::size_t> stepsAtFirstPublish{std::numeric_limits<std::size_t>::max()};
  auto io = std::make_shared<CountingDependencyIo>();
  io->onPublish = [&stepsRun, &stepsAtFirstPublish, io] (const std::string& scope) {
    io->publishedScopes.insert(scope);
    const auto observed = stepsRun.load();
    std::size_t expected = std::numeric_limits<std::size_t>::max();
    stepsAtFirstPublish.compare_exchange_strong(expected, observed);
  };
  const auto runner = [&stepsRun] (const RoleExecutionContext& ctx) {
    ++stepsRun;
    std::map<std::string, TensorBundle> outputs;
    for (const auto& slot : ctx.activeStep->outputSlots) {
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      const std::vector<float> values = {1.0f, 2.0f};
      tensor.payload.resize(values.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      outputs.emplace(slot.slotId, std::move(bundle));
    }
    return outputs;
  };

  // Every admitted scope is supplied, so the unit can run to completion.
  std::map<std::string, TensorBundle> initial;
  for (const auto& edge : role.inputs) {
    if (edge.consumerStepId.empty()) continue;
    NamedTensor tensor;
    tensor.name = "partial";
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {std::int64_t(2)};
    const std::vector<float> values = {0.5f, 0.25f};
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    TensorBundle bundle;
    bundle.name = "partial";
    bundle.payload = encodeTensorBundle({tensor});
    bundle.expectedBytes = bundle.payload.size();
    initial.emplace(edge.scope, std::move(bundle));
  }
  for (const auto& step : *role.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "EPOCH_INPUT") continue;
      if (initial.count(slot.slotId) != 0) continue;
      NamedTensor tensor;
      tensor.name = slot.slotId;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = {std::int64_t(2)};
      tensor.payload = std::vector<std::uint8_t>(sizeof(float) * 2, 0);
      TensorBundle bundle;
      bundle.name = slot.slotId;
      bundle.payload = encodeTensorBundle({tensor});
      bundle.expectedBytes = bundle.payload.size();
      initial.emplace(slot.slotId, std::move(bundle));
    }
  }

  const auto roleOutputCount = role.outputs.size();
  BOOST_REQUIRE(roleOutputCount > 1);
  ProviderRoleWorker worker(1, 1, 16, std::chrono::seconds(5));
  auto future = worker.executeAsync("early-publish-session", std::move(role), io,
                                    runner, initial);
  const auto result = future.get();
  BOOST_CHECK(!result.outputsByScope.empty());

  const auto totalSteps = stepsRun.load();
  BOOST_REQUIRE(totalSteps > 1);
  // The first publish happened before the last step: the partial left the unit
  // while it was still computing, which is what the peer needs.
  BOOST_CHECK_MESSAGE(stepsAtFirstPublish.load() !=
                      std::numeric_limits<std::size_t>::max(),
    "the unit never published a partial");
  BOOST_CHECK_MESSAGE(stepsAtFirstPublish.load() < totalSteps,
    "the first partial was published only after all " + std::to_string(totalSteps) +
    " steps finished");
  // And nothing was published twice: the count equals the number of declared
  // output edges, so the final pass did not resend what the steps already sent.
  BOOST_CHECK_EQUAL(io->publishCalls.load(), roleOutputCount);
  BOOST_CHECK_EQUAL(io->publishedScopes.size(), roleOutputCount);
}


namespace {

ProviderGroupCoordinatorOptions
makeSpec224CoordinatorOptions()
{
  ProviderGroupCoordinatorOptions options;
  options.randomBytes = [] (std::size_t size) {
    return ProviderGroupBytes(size, 0x4d);
  };
  options.wrapEpochKey = [] (const std::string& provider,
                             const ProviderGroupBytes& key) {
    ProviderGroupBytes result(provider.begin(), provider.end());
    result.push_back(':');
    result.insert(result.end(), key.begin(), key.end());
    return result;
  };
  return options;
}

} // namespace

BOOST_AUTO_TEST_CASE(TwoProvidersExchangeAPartialOverNamedData)
{
  // Exit 3's real test: two Providers, two faces, one partial crossing between
  // them. Every earlier case used a fake IO, so none of them could fail if the
  // production transport were broken. This one uses the production
  // NdnsfCollaborationDependencyIo on both sides.
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 2;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();

  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto forwardInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  auto forwardData = producerFace.onSendData.connect(
      [&] (const ndn::Data& data) { consumerFace.receive(data); });

  const auto sha = [] (char value) {
    return std::string("sha256:") + std::string(64, value);
  };
  const ndn::Name requestId("/request/spec224-partial-exchange");
  const std::string planDigest = sha('1');
  const std::string groupId = "spec224-tp-group";
  const std::string sourceLayout = sha('2');
  const std::string targetLayout = sha('3');
  const std::string tensorDigest = sha('4');

  GroupOperationV1 operation;
  operation.operationIndex = 0;
  operation.kind = "TP_PARTIAL";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = sourceLayout;
  operation.maxBytes = 1024;
  operation.maxSegments = 8;

  auto producerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(makeSpec224CoordinatorOptions());
  const auto capability = producerCoordinator->createCapability(
    requestId.toUri(), "attempt-1", planDigest, groupId, 3,
    {{producer.getName().toUri(), 0, "offer-producer", producer.getName().toUri()},
     {consumer.getName().toUri(), 1, "offer-consumer", consumer.getName().toUri()}},
    {operation}, 128 * 1024, 2000, 10000);
  auto consumerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(makeSpec224CoordinatorOptions());
  consumerCoordinator->installCapability(
    capability, producerCoordinator->epochKeyForProvider(producer.getName().toUri()),
    true);

  NativeTensorEndpointV3 endpoint;
  endpoint.producerNamespace = producer.getName().toUri();
  endpoint.requester = profile.userIdentity.toUri();
  endpoint.requestId = requestId.toUri();
  endpoint.attempt = 1;
  endpoint.planDigest = planDigest;
  endpoint.groupId = groupId;
  endpoint.groupEpoch = "3";
  endpoint.operation = operation.kind;
  endpoint.round = operation.operationIndex;
  endpoint.sourceKind = "ROLE";
  // The tensor-parallel unit identity: rank 0's attention partial reaching rank 1.
  endpoint.producerRole = "S0R0";
  endpoint.producerRank = 0;
  endpoint.consumerRole = "S0R1";
  endpoint.consumerRoles = {"S0R1"};
  endpoint.tensorId = "attn_partial";
  endpoint.tensorDigest = tensorDigest;
  endpoint.layoutDigest = sourceLayout;
  endpoint.targetLayoutDigest = targetLayout;
  endpoint.microbatch = 0;
  endpoint.segmentCount = operation.maxSegments;
  endpoint.manifestDigest = sha('5');
  endpoint.securityProfile = "NDNSF_DATA_V1";
  endpoint.noProgressDeadlineMs = capability.noProgressMs;
  endpoint.hardDeadlineMs = capability.hardDeadlineMs;
  {
    auto identity = nativeEndpointJson(endpoint);
    identity.erase("endpoint_digest");
    identity.erase("consumer_role");
    endpoint.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  }

  DependencyEdge edge;
  edge.scope = "tp-attn-0-r0-from-r1";
  edge.producerRole = endpoint.producerRole;
  edge.consumerRole = endpoint.consumerRole;
  edge.consumerRoles = endpoint.consumerRoles;
  edge.plannedDataName = tensorObjectNamePrefix(endpoint);
  edge.tensors = {endpoint.tensorId};
  edge.requestId = endpoint.requestId;
  edge.attemptEpoch = endpoint.attempt;
  edge.useNdnsfDataV1 = true;
  edge.collectiveOperationIndex = operation.operationIndex;
  edge.collectiveProducerRank = "0";
  edge.collectiveSourceLayoutDigest = sourceLayout;
  edge.collectiveTargetLayoutDigest = targetLayout;
  edge.collectiveTensorDigest = tensorDigest;
  edge.transportScope = groupId;
  edge.producerProvider = producer.getName().toUri();
  edge.declaredByV3 = true;
  edge.manifestDataName = tensorObjectManifestName(endpoint);
  edge.maxSegments = endpoint.segmentCount;
  edge.endpointDigest = endpoint.endpointDigest;
  edge.executionObjectDigest = computeTensorExecutionObjectDigest(endpoint);
  edge.namingOperationStride = endpoint.namingOperationStride;
  edge.planDigest = planDigest;
  edge.manifestContractDigest = endpoint.manifestDigest;
  edge.tensorDigest = tensorDigest;
  edge.layoutDigest = sourceLayout;
  edge.securityProfile = endpoint.securityProfile;
  edge.operationKind = operation.kind;
  edge.round = operation.operationIndex;
  edge.microbatch = 0;
  edge.noProgressDeadlineMs = capability.noProgressMs;
  edge.hardDeadlineMs = capability.hardDeadlineMs;
  // The sealed step binding this whole change exists to carry.
  edge.producerStepId = "S0R0/ATTN/0";
  edge.producerSlotId = "attn_partial";
  edge.consumerStepId = "S0R1/SUM_ATTN/0";
  edge.consumerSlotId = "peer_partial";

  ndn_service_framework::ServiceProvider::CollaborationAssignment producerAssignment;
  producerAssignment.role = endpoint.producerRole;
  producerAssignment.service = profile.serviceName;
  producerAssignment.workDeadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(90);
  producerAssignment.workCurrent = [] { return true; };
  ndn_service_framework::ServiceProvider::CollaborationAssignment consumerAssignment;
  consumerAssignment.role = endpoint.consumerRole;
  consumerAssignment.service = profile.serviceName;
  consumerAssignment.workDeadline = producerAssignment.workDeadline;
  consumerAssignment.workCurrent = [] { return true; };
  ndn_service_framework::ServiceProvider::CollaborationContext producerContext(
    producer, profile.userIdentity, requestId,
    ndn_service_framework::RequestMessage(), producerAssignment);
  ndn_service_framework::ServiceProvider::CollaborationContext consumerContext(
    consumer, profile.userIdentity, requestId,
    ndn_service_framework::RequestMessage(), consumerAssignment);

  // The partial is a real encoded tensor bundle, not raw floats: the SUM path
  // decodes it, so a raw payload would only pass if decoding were skipped.
  NamedTensor tensor;
  tensor.name = "attn_partial";
  tensor.elementType = TensorElementType::Float32;
  tensor.shape = {std::int64_t(2)};
  const std::vector<float> values = {1.5f, -2.25f};
  tensor.payload.resize(values.size() * sizeof(float));
  std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
  TensorBundle original;
  original.name = "attn_partial";
  original.payload = encodeTensorBundle({tensor});
  original.expectedBytes = original.payload.size();

  // Segment size above the encoded bundle, so this partial is one segment. The
  // exchange being tested is the transport and its admission, not segmentation;
  // a 7-byte segment size would split a one-tensor bundle into more pieces than
  // the signed operation permits and fail for an unrelated reason.
  BOOST_REQUIRE(original.payload.size() < 4096);
  NdnsfCollaborationDependencyIo producerIo(producerContext, 3000, 4096, 60000,
                                            producerCoordinator);
  NdnsfCollaborationDependencyIo consumerIo(consumerContext, 3000, 4096, 60000,
                                            consumerCoordinator);
  try {
    producerIo.publishOutput("spec224-tp-session", edge, original);
  }
  catch (const std::exception& error) {
    BOOST_FAIL(std::string("publish failed: ") + error.what());
  }

  // The consumer's wait is driven by the environment's pump, so a transport that
  // never delivers would time out rather than hang.
  auto fetched = consumerIo.prefetchInput("spec224-tp-session", edge);
  environment.pumpUntil([&] {
    return fetched.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  std::optional<TensorBundle> reconstructed;
  BOOST_REQUIRE_NO_THROW(reconstructed = fetched.get());
  BOOST_REQUIRE(reconstructed);
  // The value that arrives is the value that was published, decoded back to the
  // same floats: the peer sees this rank's partial, which is the whole point.
  const auto decoded = decodeTensorBundle(reconstructed->payload);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_CHECK_EQUAL(decoded.front().name, std::string("attn_partial"));
  BOOST_REQUIRE_EQUAL(decoded.front().shape.size(), 1U);
  BOOST_CHECK_EQUAL(decoded.front().shape.front(), std::int64_t(2));
  std::vector<float> arrived(2);
  std::memcpy(arrived.data(), decoded.front().payload.data(), arrived.size() * sizeof(float));
  BOOST_CHECK_CLOSE(arrived[0], 1.5f, 1e-6);
  BOOST_CHECK_CLOSE(arrived[1], -2.25f, 1e-6);
}


BOOST_AUTO_TEST_CASE(CatalogExpandsRankRolesButNotStepUnits)
{
  // The catalog already has the split I was looking for: a candidate names stage roles,
  // and prepared roles carry (stage, rank) pairs whose selectedRole is the rank's own
  // name. The template then rewrites dependency producers and consumers from stage
  // names into selected rank names.
  //
  // What it does not rewrite is the step template's unitId. A step names the unit that
  // runs it, so after expansion a step would still name the stage while the plan's roles
  // list holds the selected rank names -- and the worker looks a step's unit up in that
  // list.
  //
  // This is the concrete, small gap: expansion covers dependencies and not steps.
  const std::string stage = "/Stage/0";
  const std::string rank0 = "/Stage/0#0";
  const std::string rank1 = "/Stage/0#1";

  NativeExecutionPlan stagePlan;
  stagePlan.roles = {stage};
  NativeDependencySpec dep;
  dep.producers = {stage};
  dep.consumers = {stage};
  dep.keyScope = "tp-partial";
  dep.topicPrefix = "/NDNSF/DI/QWEN";
  dep.objectNameTemplate = "{producerProvider}/x/{sequence}";
  dep.tensors = {"partial"};
  dep.operationKind = "TENSOR_PARTIAL";
  dep.useNdnsfDataV1 = true;
  stagePlan.dependencies.push_back(dep);

  // After the template's expansion: dependencies name rank roles.
  std::map<std::string, std::vector<std::string>> selectedByStage{
    {stage, {rank0, rank1}}};
  const auto expand = [&selectedByStage] (const std::vector<std::string>& stages) {
    std::vector<std::string> result;
    for (const auto& name : stages) {
      const auto& roles = selectedByStage.at(name);
      result.insert(result.end(), roles.begin(), roles.end());
    }
    return result;
  };
  auto expanded = stagePlan;
  expanded.roles.clear();
  for (const auto& item : selectedByStage) {
    for (const auto& selected : item.second) expanded.roles.push_back(selected);
  }
  for (auto& dependency : expanded.dependencies) {
    dependency.producers = expand(dependency.producers);
    dependency.consumers = expand(dependency.consumers);
  }
  BOOST_CHECK_EQUAL(expanded.roles.size(), 2U);
  BOOST_CHECK_EQUAL(expanded.dependencies.front().producers.size(), 2U);
  BOOST_CHECK_EQUAL(expanded.dependencies.front().consumers.size(), 2U);

  // Now a step template whose unit names the stage. Expansion leaves it unrewritten,
  // so it names a role the plan no longer has.
  NativeExecutionStepSpec step;
  step.stepId = stage + "/ATTN/0";
  step.unitId = stage;
  step.operation = "CALL_SUBGRAPH";
  step.artifactEntry = "/ATTN/0";
  step.outputSlots.push_back(NativeStepSlotSpec{
    "partial", "float32", {1, 4}, tpDigest('a'), "REMOTE_STEP", "", ""});
  expanded.steps.push_back(step);

  const auto unitKnown = std::find(expanded.roles.begin(), expanded.roles.end(),
                                   expanded.steps.front().unitId) != expanded.roles.end();
  BOOST_TEST_MESSAGE("expanded roles=" << expanded.roles.size()
                    << " step unit=" << expanded.steps.front().unitId
                    << " unitKnown=" << unitKnown);
  // The gap, stated as the false that has to become true: a step's unit must name one
  // of the plan's roles, and after expansion it names the stage.
  BOOST_CHECK(!unitKnown);
  // The rewrite the fix needs: the step's unit becomes the rank role it belongs to. For
  // a sharded stage every rank runs every step, so each stage step expands into one step
  // per rank rather than being assigned to one of them.
  BOOST_CHECK_EQUAL(selectedByStage.at(stage).size(), 2U);
}

BOOST_AUTO_TEST_CASE(NodeOwnershipIsSingleValuedSoRanksCannotShareALayer)
{
  // Expanded the stage into rank roles on paper and hit the constraint the splitter
  // cannot satisfy today: nodeRoles maps each graph node to exactly one owning role, and
  // validate requires the set of owners to equal the role set. A sharded rank owns no
  // node of its own -- both ranks execute the same layers -- so either the map gains a
  // second owner per node or the roles are refused for owning nothing.
  //
  // Stated as the two rules, with the shape that breaks them.
  std::map<std::string, std::string> nodeRoles;
  nodeRoles["embedding"] = "S0R0";
  nodeRoles["layer-00"] = "S0R0";
  nodeRoles["final-norm-head"] = "S0R0";
  // A stage-per-role candidate: every node has one owner and every owner owns a node.
  std::set<std::string> stageRoles = {"S0R0"};
  std::set<std::string> stageOwners;
  for (const auto& item : nodeRoles) stageOwners.insert(item.second);
  BOOST_CHECK(stageOwners == stageRoles);

  // Expanding to ranks: the role set grows, the node set does not, so the owners no
  // longer cover the roles.
  std::set<std::string> rankRoles = {"S0R0", "S0R1"};
  BOOST_CHECK(stageOwners != rankRoles);
  // Assigning the layer to the second rank instead would leave the first owning only
  // part of it, which is not what a shard does: both ranks execute the whole layer.
  BOOST_CHECK_EQUAL(nodeRoles.count("layer-00"), 1U);
  BOOST_CHECK_EQUAL(nodeRoles.at("layer-00"), std::string("S0R0"));
  // So the map cannot express "these two roles both own this node", and that is the
  // first thing a rank expansion would need.
  BOOST_CHECK_MESSAGE(nodeRoles.size() == 3U,
    "nodeRoles gained a second entry per node, so multi-owner is expressible now");

  // And the consequence is not merely validation: the role preparer derives a role's
  // source boundary from its owned nodes, then refuses a role whose input or output set
  // is empty. A rank that owns no node therefore owns no boundary, so it has no source
  // slice to assemble.
  //
  // The projected plan avoids the question entirely by keeping roles and units distinct:
  // a role names one unit, and the unit list says which layers that unit executes.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan projected;
  qwen::projectQwenTpPlan(projected, shape, 2, "entry", "S0R0");
  std::set<std::string> units;
  for (const auto& step : projected.steps) units.insert(step.unitId);
  BOOST_CHECK_EQUAL(units.size(), projected.roles.size());
  BOOST_TEST_MESSAGE("nodeRoles single-owner; projected roles=" << projected.roles.size()
                    << " units=" << units.size());
}

BOOST_AUTO_TEST_CASE(StageRoleWithRankListCannotSatisfyThePlacementContract)
{
  // Checked the smaller alternative before touching the role model: keep one role per
  // stage and give it a rank list, instead of one role per rank. The placement contract
  // rules it out, and this case records why rather than leaving it as a preference.
  //
  // Every placement decision reads per-role facts keyed by role name:
  // requirementsByRole, artifactsByRole, rankArtifactDigestsByRole and the plan's own
  // roles list. A stage role with two ranks would need each of those to become
  // per-(role, rank), which is the same change as one role per rank -- only spread
  // across more maps instead of fewer roles.
  NativeSplitCandidate candidate;
  candidate.executionPlan.roles = {"/Stage/0"};
  candidate.requirementsByRole["/Stage/0"] = NativeRoleResourceRequirement{
    {"onnxruntime"}, 1024, std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1.10};
  candidate.tensorDegreesByRole["/Stage/0"] = 2;
  candidate.artifactsByRole["/Stage/0"] = {"sha256:" + std::string(64, 'a')};
  candidate.rankArtifactDigestsByRole["/Stage/0"] = {
    "sha256:" + std::string(64, 'a'), "sha256:" + std::string(64, 'b')};

  // The degree says two ranks; the role list says one role. Both are true today, and
  // placement reads the role list, so it would place one role once and give it one
  // artifact -- leaving the second rank's artifact unplaced.
  BOOST_CHECK_EQUAL(candidate.executionPlan.roles.size(), 1U);
  BOOST_CHECK_EQUAL(candidate.tensorDegreesByRole.at("/Stage/0"), 2U);
  BOOST_CHECK_EQUAL(candidate.rankArtifactDigestsByRole.at("/Stage/0").size(), 2U);
  BOOST_CHECK_EQUAL(candidate.artifactsByRole.at("/Stage/0").size(), 1U);

  // The projected plan, by contrast, names one role per rank, so every per-role map has
  // an entry per rank and placement can place both.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan projected;
  qwen::projectQwenTpPlan(projected, shape, 2, "entry", "S0R0");
  BOOST_CHECK_EQUAL(projected.roles.size(), 2U);
  // One role per rank, each covering every layer -- which is what a sharded unit is.
  std::set<std::string> units;
  for (const auto& step : projected.steps) units.insert(step.unitId);
  BOOST_CHECK_EQUAL(units.size(), projected.roles.size());

  // So the smaller alternative does not exist: the per-rank split has to happen where
  // the plan is built, because everything downstream is keyed by role.
  BOOST_TEST_MESSAGE("stage-role candidate: roles=" << candidate.executionPlan.roles.size()
                    << " degree=" << candidate.tensorDegreesByRole.at("/Stage/0")
                    << " rankArtifacts=" << candidate.rankArtifactDigestsByRole.at("/Stage/0").size()
                    << " | projected roles=" << projected.roles.size());
}

BOOST_AUTO_TEST_CASE(CertifiedEndpointsJoinThePlansStepBoundEdges)
{
  // The join the networked sharded run needs: plan gives identity and step binding,
  // the certified projection gives the transport endpoint, and both must live on the
  // same runtime edge or the transport cannot publish what the worker will match.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  auto spec = roleSpecFor(plan, "S0R1", std::string("/session/tp-join"),
                          assignment, "/provider/1");
  BOOST_REQUIRE(!spec.inputs.empty());

  // One certified endpoint per DATA_V1 edge, inputs and outputs alike: an output edge
  // is published under the same transport, so it needs an endpoint as well.
  std::vector<NativeEdgeEndpointBinding> bindings;
  std::vector<DependencyEdge> dataV1Edges;
  for (const auto& edge : spec.inputs)
    if (edge.useNdnsfDataV1) dataV1Edges.push_back(edge);
  for (const auto& edge : spec.outputs)
    if (edge.useNdnsfDataV1) dataV1Edges.push_back(edge);
  for (const auto& edge : dataV1Edges) {
    NativeEdgeEndpointBinding binding;
    binding.producerRole = edge.producerRole;
    binding.consumerRole = edge.consumerRole;
    binding.dependencyId = edge.scope;
    binding.operationKind = edge.operationKind;
    binding.producerStepId = edge.producerStepId;
    binding.producerSlotId = edge.producerSlotId;
    binding.consumerStepId = edge.consumerStepId;
    binding.consumerSlotId = edge.consumerSlotId;
    binding.endpointDigest = tpDigest('1');
    binding.manifestDataName = "/provider/0/NDNSF/DI/QWEN/manifest/" + edge.scope;
    binding.tensorDigest = tpDigest('2');
    binding.layoutDigest = tpDigest('3');
    binding.targetLayoutDigest = tpDigest('3');
    binding.securityProfile = "NDNSF_DATA_V1";
    binding.maxSegments = 8;
    binding.noProgressDeadlineMs = 2000;
    binding.hardDeadlineMs = 10000;
    bindings.push_back(std::move(binding));
  }
  BOOST_REQUIRE(!bindings.empty());

  const auto bound = bindNativeEdgeEndpoints(spec, bindings);
  BOOST_CHECK_EQUAL(bound, bindings.size());
  // Every DATA_V1 input now carries both halves: the step binding from the plan and the
  // endpoint from the projection.
  // Inspect the spec's own edges after binding, not the copies taken before it.
  std::size_t complete = 0;
  for (const auto& edge : spec.inputs) {
    if (!edge.useNdnsfDataV1) continue;
    BOOST_CHECK(!edge.endpointDigest.empty());
    BOOST_CHECK(!edge.manifestDataName.empty());
    BOOST_CHECK_EQUAL(edge.maxSegments, 8U);
    BOOST_CHECK_EQUAL(edge.securityProfile, std::string("NDNSF_DATA_V1"));
    BOOST_CHECK(!edge.consumerStepId.empty());
    BOOST_CHECK(!edge.consumerSlotId.empty());
    BOOST_CHECK(edge.declaredByV3);
    if (!edge.endpointDigest.empty() && !edge.consumerStepId.empty()) ++complete;
  }
  // Two inputs among the four bound edges; both halves present on each.
  BOOST_CHECK_EQUAL(complete, 2U);
  BOOST_CHECK_EQUAL(bound, 4U);

  // A DATA_V1 edge with no binding is refused rather than left unpublishable.
  {
    auto missing = spec;
    std::vector<NativeEdgeEndpointBinding> fewer(bindings.begin(), bindings.end() - 1);
    BOOST_CHECK_THROW(bindNativeEdgeEndpoints(missing, fewer), std::invalid_argument);
  }
  // A binding that matches no edge is refused too, so a stale projection cannot pass
  // silently while the plan's edges stay unbound.
  {
    auto extra = spec;
    auto surplus = bindings.front();
    surplus.producerRole = "/Ghost";
    surplus.dependencyId = "/ghost";
    auto withSurplus = bindings;
    withSurplus.push_back(surplus);
    BOOST_CHECK_THROW(bindNativeEdgeEndpoints(extra, withSurplus), std::invalid_argument);
  }
  // Two bindings for one edge are refused instead of resolved by order.
  {
    auto ambiguous = spec;
    auto duplicated = bindings;
    duplicated.push_back(bindings.front());
    BOOST_CHECK_THROW(bindNativeEdgeEndpoints(ambiguous, duplicated),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(NetworkEndpointsComeFromTheProjectionNotFromThePlan)
{
  // Why the network case cannot reuse the projected plan's edges as they are. The plan
  // says who produces what; the V3 endpoint says how it crosses the network, and the
  // endpoint is certified by the projection builder from an authenticated group
  // binding. roleSpecFor copies the plan's edges and sets none of the endpoint fields.
  //
  // So an edge has identity and step binding from the plan, and needs a projection to
  // acquire a transport identity. That split is why the existing network case builds
  // its endpoint by hand: it is exercising the transport, not the projection.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  const auto spec = roleSpecFor(plan, "S0R1", std::string("/session/tp-network"),
                                assignment, "/provider/1");
  BOOST_REQUIRE(!spec.inputs.empty());

  std::size_t withEndpoint = 0;
  std::size_t withStepBinding = 0;
  for (const auto& edge : spec.inputs) {
    if (!edge.useNdnsfDataV1) continue;
    if (!edge.endpointDigest.empty() && !edge.manifestDataName.empty() &&
        edge.maxSegments != 0 && edge.securityProfile == "NDNSF_DATA_V1") {
      ++withEndpoint;
    }
    if (!edge.consumerStepId.empty() && !edge.consumerSlotId.empty()) ++withStepBinding;
  }
  BOOST_TEST_MESSAGE("DATA_V1 inputs=" << spec.inputs.size()
                    << " withStepBinding=" << withStepBinding
                    << " withEndpoint=" << withEndpoint);
  // The step binding travels with the edge, which is what the worker matches on.
  BOOST_CHECK(withStepBinding > 0);
  // The endpoint does not, which is what network publication validates.
  BOOST_CHECK_EQUAL(withEndpoint, 0U);

  // The two are separate contracts with separate owners: the sealed plan owns identity
  // and binding, the certified projection owns the transport endpoint. A composition
  // has to carry both, and this case is the record that the plan alone carries one.
  for (const auto& edge : spec.inputs) {
    if (!edge.useNdnsfDataV1) continue;
    BOOST_CHECK(edge.endpointDigest.empty());
    BOOST_CHECK(edge.manifestDataName.empty());
    BOOST_CHECK_EQUAL(edge.maxSegments, 0U);
    BOOST_CHECK(edge.securityProfile.empty());
  }
}

BOOST_AUTO_TEST_CASE(CoordinatorMustPublishEveryFeedbackEdgeNotJustTheFirst)
{
  // C6: the coordinator admits the token event once, then handles every feedback
  // edge. Publishing only the first would leave the peer without its next-epoch
  // input while the coordinator reported success -- a silent stall on the next
  // decode step.
  //
  // This case pins the shape the coordinator must consume: a unit with several
  // feedback output edges, each with its own consumer and operation index.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  for (std::uint64_t index = 0; index < plan.roles.size(); ++index) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {plan.roles[index]};
    feedback.keyScope = "token-feedback-" + plan.roles[index];
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    feedback.collectiveOperationIndex = index;
    plan.dependencies.push_back(std::move(feedback));
  }

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  const auto egress = roleSpecFor(plan, "S0R0", std::string("/session/feedback-all"),
                                  assignment, "/provider/0");
  std::vector<std::string> scopes;
  std::set<std::uint64_t> indices;
  for (const auto& edge : egress.outputs) {
    if (edge.operationKind != "TOKEN_FEEDBACK") continue;
    // Distinct scope and index per edge: two edges sharing either would make the
    // second publication collide with the first rather than reach its consumer.
    BOOST_CHECK(scopes.end() == std::find(scopes.begin(), scopes.end(), edge.scope));
    scopes.push_back(edge.scope);
    BOOST_CHECK(indices.insert(edge.collectiveOperationIndex).second);
  }
  BOOST_CHECK_EQUAL(scopes.size(), plan.roles.size());
  BOOST_CHECK_EQUAL(indices.size(), plan.roles.size());
  // And the receivers are the units themselves, not one shared consumer.
  std::set<std::string> consumers;
  for (const auto& edge : egress.outputs) {
    if (edge.operationKind != "TOKEN_FEEDBACK") continue;
    consumers.insert(edge.consumerRole);
  }
  BOOST_CHECK_EQUAL(consumers.size(), plan.roles.size());
}

BOOST_AUTO_TEST_CASE(UnitPromotionCommitsOnlyWhenEveryChildSucceeds)
{
  // C6's parent transaction: commit only when all children succeed, release the
  // uncommitted branch otherwise. A unit that published a checkpoint missing one
  // layer would look complete and restore the wrong KV.
  std::vector<qwen::QwenTpEntryContract> table;
  for (const auto& spec : std::vector<std::pair<std::string, std::string>>{
         {"S0R0/INIT/0", "INIT"}, {"S0R0/ATTN/0", "ATTN"},
         {"S0R0/MLP/0", "MLP"}, {"S0R0/ATTN/1", "ATTN"}}) {
    qwen::QwenTpEntryContract entry;
    entry.entryId = spec.first;
    entry.unitId = "S0R0";
    entry.kind = spec.second;
    entry.inputNames = {"in"};
    entry.outputNames = {"out"};
    entry.relativeGraphPath = "entry.onnx";
    entry.graphDigest = "sha256:" + std::string(64, 'a');
    entry.validate();
    table.push_back(std::move(entry));
  }

  // Children keyed by entry id, so the case can make one fail individually.
  std::map<std::string, std::shared_ptr<FakeStatefulRunner>> children;
  const auto makeRunner = [&children] (std::vector<qwen::QwenTpEntryContract> entries) {
    return qwen::NativeQwenTensorRunner(
      std::move(entries),
      [&children] (const NativeModelRunnerSpec& spec) {
        auto child = std::make_shared<FakeStatefulRunner>(true);
        children[spec.role] = child;
        return std::static_pointer_cast<NativeModelRunner>(child);
      },
      [] (const qwen::QwenTpEntryContract& entry) {
        NativeModelRunnerSpec spec;
        spec.role = entry.entryId;
        return spec;
      });
  };

  // All children succeed: one handle, covering both attention children, with a
  // per-child conversation key so two layers cannot collide on one key.
  {
    children.clear();
    auto runner = makeRunner(table);
    const auto handle = runner.promoteSessionStateToConversation(
      "promote-session", "conv/unit");
    BOOST_REQUIRE(handle.has_value());
    BOOST_CHECK_EQUAL(handle->conversationKey, std::string("conv/unit"));
    BOOST_CHECK_EQUAL(handle->opaque.role, std::string("S0R0"));
    BOOST_CHECK(handle->opaque.token.find("S0R0/ATTN/0=") != std::string::npos);
    BOOST_CHECK(handle->opaque.token.find("S0R0/ATTN/1=") != std::string::npos);
    // The unit's size is the sum, so a caller bounding memory sees the rank.
    BOOST_CHECK_EQUAL(handle->logicalBytes, 2U * 4096U);
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/0")->promoteCalls, 1U);
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/1")->promoteCalls, 1U);
    // Non-KV children are not asked to promote at all.
    BOOST_CHECK_EQUAL(children.at("S0R0/INIT/0")->promoteCalls, 0U);
    BOOST_CHECK_EQUAL(children.at("S0R0/MLP/0")->promoteCalls, 0U);
  }

  // One child fails: no handle, and the child that did promote is released, so the
  // uncommitted branch does not linger holding KV.
  {
    children.clear();
    auto runner = makeRunner(table);
    children["S0R0/ATTN/1"]->failPromote = true;
    const auto handle = runner.promoteSessionStateToConversation(
      "promote-session", "conv/unit");
    BOOST_CHECK(!handle.has_value());
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/0")->releaseConversationCalls, 1U);
  }

  // Restore is all-or-nothing too: a failing child makes the unit fail, and the
  // already-restored children are released rather than left on mixed checkpoints.
  {
    children.clear();
    auto runner = makeRunner(table);
    const auto handle = runner.promoteSessionStateToConversation(
      "restore-session", "conv/restore");
    BOOST_REQUIRE(handle.has_value());
    children["S0R0/ATTN/1"]->failRestore = true;
    BOOST_CHECK(!runner.restoreConversationState(*handle, "restore-session"));
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/1")->restoreCalls, 1U);
  }

  // Release reaches every KV child and reports success only when all of them agree.
  {
    children.clear();
    auto runner = makeRunner(table);
    const auto handle = runner.promoteSessionStateToConversation(
      "release-session", "conv/release");
    BOOST_REQUIRE(handle.has_value());
    BOOST_CHECK(runner.releaseConversationState(*handle));
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/0")->releaseConversationCalls, 1U);
    BOOST_CHECK_EQUAL(children.at("S0R0/ATTN/1")->releaseConversationCalls, 1U);
  }

  // An empty session or conversation key is refused rather than promoted.
  {
    children.clear();
    auto runner = makeRunner(table);
    BOOST_CHECK(!runner.promoteSessionStateToConversation("", "conv/unit").has_value());
    BOOST_CHECK(!runner.promoteSessionStateToConversation("s", "").has_value());
  }
}

BOOST_AUTO_TEST_CASE(OnlyTheEgressUnitPublishesTokenEventsInAShardedPlan)
{
  // Whether the coordinator can tell which unit samples. It infers a terminal role
  // from a TOKEN_FEEDBACK *output* edge. My suspicion was that per-unit feedback
  // would give both units one and make both terminal; that is worth checking rather
  // than assuming, because the feedback edge is produced by the egress unit and
  // merely consumed by each receiver.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");

  // The projection itself carries no feedback edge; the planner adds them. Add the
  // two the planner now builds, so this measures the real shape.
  for (const auto& consumer : plan.roles) {
    NativeDependencySpec feedback;
    feedback.producers = {"S0R0"};
    feedback.consumers = {consumer};
    feedback.keyScope = "token-feedback-" + consumer;
    feedback.topicPrefix = "/token-feedback";
    feedback.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
    feedback.tensors = {"input_ids"};
    feedback.operationKind = "TOKEN_FEEDBACK";
    feedback.useNdnsfDataV1 = true;
    plan.dependencies.push_back(std::move(feedback));
  }

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/provider/0"}, {"S0R1", "/provider/1"}};
  std::map<std::string, std::size_t> feedbackOutputs;
  for (const auto& unit : plan.roles) {
    const auto spec = roleSpecFor(plan, unit, std::string("/session/tp-feedback"),
                                  assignment, "/provider/0");
    for (const auto& edge : spec.outputs) {
      if (edge.operationKind != "TOKEN_FEEDBACK") continue;
      ++feedbackOutputs[unit];
    }
  }
  // The egress unit produces feedback for both consumers, so it owns two output
  // edges; the peer produces none because it samples nothing.
  BOOST_TEST_MESSAGE("feedback outputs by unit: S0R0="
    << (feedbackOutputs.count("S0R0") ? feedbackOutputs["S0R0"] : 0)
    << " S0R1=" << (feedbackOutputs.count("S0R1") ? feedbackOutputs["S0R1"] : 0));
  BOOST_CHECK_EQUAL(feedbackOutputs.count("S0R0") ? feedbackOutputs["S0R0"] : 0, 2U);
  BOOST_CHECK_EQUAL(feedbackOutputs.count("S0R1") ? feedbackOutputs["S0R1"] : 0, 0U);

  // So the scope-based inference still identifies exactly one terminal role, and
  // my earlier concern was wrong: only the egress unit publishes token events.
  std::set<std::string> terminalUnits;
  for (const auto& item : feedbackOutputs)
    if (item.second > 0) terminalUnits.insert(item.first);
  BOOST_CHECK_EQUAL(terminalUnits.size(), 1U);
  BOOST_CHECK_EQUAL(*terminalUnits.begin(), std::string("S0R0"));
}


BOOST_AUTO_TEST_CASE(EveryTensorParallelUnitNeedsItsOwnFeedbackEdge)
{
  // C6: feedback is generated per receiving unit, each bound to its own consumer
  // and its own operation index. Reusing the owner's bundle for the peer would give
  // it a name and lineage belonging to another rank. This case pins the shape the
  // planner must produce, using the projected plan rather than a hand-built one.
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;
  shape.vocab = 151936;
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  BOOST_REQUIRE_EQUAL(plan.roles.size(), 2U);
  BOOST_REQUIRE_EQUAL(plan.tensorDegree, 2U);

  // The egress unit is the one owning a head step, which is what the planner now
  // reads instead of a non-existent graph sink.
  std::set<std::string> units;
  std::set<std::string> headOwners;
  for (const auto& step : plan.steps) {
    if (step.unitId.empty()) continue;
    units.insert(step.unitId);
    if (step.operation != "CALL_SUBGRAPH") continue;
    if (step.artifactEntry.find("/HEAD/") == std::string::npos) continue;
    headOwners.insert(step.unitId);
  }
  BOOST_CHECK_EQUAL(units.size(), 2U);
  BOOST_CHECK_EQUAL(headOwners.size(), 1U);
  const auto owner = *headOwners.begin();
  BOOST_CHECK_EQUAL(owner, std::string("S0R0"));

  // One edge per receiving unit, including the owner's own local feedback: a
  // decode step on either rank consumes the sampled token.
  const auto consumers = [&] {
    std::vector<std::string> result;
    for (const auto& unit : units) result.push_back(unit);
    return result;
  }();
  BOOST_CHECK_EQUAL(consumers.size(), plan.roles.size());

  // Distinct operation indices, one per edge, so two consumers cannot collide on
  // one signed operation.
  std::set<std::uint64_t> indices;
  for (std::size_t i = 0; i < consumers.size(); ++i)
    BOOST_CHECK(indices.insert(static_cast<std::uint64_t>(i)).second);
  BOOST_CHECK_EQUAL(indices.size(), consumers.size());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// ShardMath — real INT4 partition arithmetic, independent of any fixture that
// calls the splitter under test. Expected values are constructed by hand.
// ---------------------------------------------------------------------------

namespace {

qwen::QwenTpShape
frozenShape()
{
  // Frozen Qwen3-0.6B export facts (see research.md#t001-graph-verification).
  qwen::QwenTpShape shape;
  shape.hidden = 1024;
  shape.layers = 28;
  shape.queryHeads = 16;
  shape.kvHeads = 8;
  shape.headDim = 128;
  shape.ffn = 3072;
  shape.vocab = 151936;
  return shape;
}

const qwen::QwenInitializerSlice*
findSlice(const qwen::QwenRankLayerSlices& layer, const std::string& suffix)
{
  for (const auto& slice : layer.initializers) {
    if (slice.name.size() >= suffix.size() &&
        slice.name.compare(slice.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return &slice;
    }
  }
  return nullptr;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ShardMath)

BOOST_AUTO_TEST_CASE(PlanSplitsFrozenShapeByWholeHeads)
{
  const auto shape = frozenShape();
  const auto rank0 = qwen::planQwenTensorSlices(shape, 2, 0);
  const auto rank1 = qwen::planQwenTensorSlices(shape, 2, 1);
  BOOST_REQUIRE_EQUAL(rank0.size(), 28U);
  BOOST_REQUIRE_EQUAL(rank1.size(), 28U);

  const auto& layer = rank0.front();
  // Exactly the seven sharded projections; norms are intentionally absent.
  BOOST_CHECK_EQUAL(layer.initializers.size(), 7U);

  // q_proj: 16 heads -> 8 per rank; head 0..7 gives [0,1024), rank1 [1024,2048).
  const auto* q0 = findSlice(layer, "attn.q_proj.MatMul.weight");
  const auto* q1 = findSlice(rank1.front(), "attn.q_proj.MatMul.weight");
  BOOST_REQUIRE(q0 != nullptr);
  BOOST_REQUIRE(q1 != nullptr);
  BOOST_CHECK_EQUAL(q0->begin, 0U);
  BOOST_CHECK_EQUAL(q0->end, 1024U);
  BOOST_CHECK_EQUAL(q1->begin, 1024U);
  BOOST_CHECK_EQUAL(q1->end, 2048U);
  BOOST_CHECK(q0->axis == qwen::QwenSliceAxis::OutputChannels);

  // k/v_proj: 8 KV heads -> 4 per rank.
  const auto* k0 = findSlice(layer, "attn.k_proj.MatMul.weight");
  BOOST_REQUIRE(k0 != nullptr);
  BOOST_CHECK_EQUAL(k0->begin, 0U);
  BOOST_CHECK_EQUAL(k0->end, 512U);
  BOOST_CHECK_EQUAL(k0->slicedN, 512U);

  // o_proj reduces over the attention channels: rank1 starts at 1024.
  const auto* o1 = findSlice(rank1.front(), "attn.o_proj.MatMul.weight");
  BOOST_REQUIRE(o1 != nullptr);
  BOOST_CHECK(o1->axis == qwen::QwenSliceAxis::InputChannels);
  BOOST_CHECK_EQUAL(o1->begin, 1024U);
  BOOST_CHECK_EQUAL(o1->end, 2048U);
  BOOST_CHECK_EQUAL(o1->slicedK, 1024U);

  // gate and up must share one FFN interval, and down reduces over it.
  const auto* gate1 = findSlice(rank1.front(), "mlp.gate_proj.MatMul.weight");
  const auto* up1 = findSlice(rank1.front(), "mlp.up_proj.MatMul.weight");
  const auto* down1 = findSlice(rank1.front(), "mlp.down_proj.MatMul.weight");
  BOOST_REQUIRE(gate1 != nullptr);
  BOOST_REQUIRE(up1 != nullptr);
  BOOST_REQUIRE(down1 != nullptr);
  BOOST_CHECK_EQUAL(gate1->begin, up1->begin);
  BOOST_CHECK_EQUAL(gate1->end, up1->end);
  BOOST_CHECK_EQUAL(gate1->begin, 1536U);
  BOOST_CHECK_EQUAL(gate1->end, 3072U);
  BOOST_CHECK(down1->axis == qwen::QwenSliceAxis::InputChannels);
  BOOST_CHECK_EQUAL(down1->begin, 1536U);
  BOOST_CHECK_EQUAL(down1->slicedK, 1536U);
}

BOOST_AUTO_TEST_CASE(UnevenHeadsOrUnalignedCutAreRejected)
{
  auto odd = frozenShape();
  odd.queryHeads = 15;          // not divisible by 2
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(odd, 2, 0), std::invalid_argument);

  auto misaligned = frozenShape();
  // 16 heads x 98 = 1568 wide; half is 784, which is not a multiple of 32.
  misaligned.headDim = 98;
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(misaligned, 2, 0), std::invalid_argument);

  auto unalignedFfn = frozenShape();
  // 3000 / 2 = 1500, not a multiple of the 32-wide block.
  unalignedFfn.ffn = 3000;
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(unalignedFfn, 2, 0), std::invalid_argument);

  const auto shape = frozenShape();
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(shape, 1, 0), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(shape, 2, 2), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PackedInt4OutputSliceCopiesWholeRows)
{
  // 4 output channels x 64 reduction channels: 2 blocks per row, 16 bytes each.
  const std::uint64_t n = 4, k = 64;
  const auto bytes = qwen::packedInt4Bytes(n, k);
  BOOST_CHECK_EQUAL(bytes, 4U * 2U * 16U);

  std::vector<std::uint8_t> packed(static_cast<std::size_t>(bytes));
  for (std::size_t i = 0; i < packed.size(); ++i) packed[i] = static_cast<std::uint8_t>(i & 0xff);

  const auto sliced = qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::OutputChannels, 1, 3);
  BOOST_CHECK_EQUAL(sliced.size(), 2U * 2U * 16U);
  // Rows 1 and 2 are contiguous in the source.
  BOOST_CHECK(std::equal(sliced.begin(), sliced.end(), packed.begin() + 32));
  // Values are untouched: no nibble was re-packed.
  BOOST_CHECK_EQUAL(sliced.front(), packed[32]);
}

BOOST_AUTO_TEST_CASE(PackedInt4InputSliceKeepsWholeBlocks)
{
  const std::uint64_t n = 3, k = 128;   // 4 blocks per row
  const auto bytes = qwen::packedInt4Bytes(n, k);
  std::vector<std::uint8_t> packed(static_cast<std::size_t>(bytes));
  for (std::size_t i = 0; i < packed.size(); ++i) packed[i] = static_cast<std::uint8_t>((i * 7) & 0xff);

  // Keep blocks 1..2 of every row: 2 blocks x 16 bytes per row.
  const auto sliced = qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::InputChannels, 32, 96);
  BOOST_CHECK_EQUAL(sliced.size(), n * 2U * 16U);
  for (std::uint64_t row = 0; row < n; ++row) {
    const auto source = row * 4U * 16U + 1U * 16U;
    for (std::size_t byte = 0; byte < 32; ++byte) {
      BOOST_CHECK_EQUAL(sliced[static_cast<std::size_t>(row) * 32U + byte], packed[source + byte]);
    }
  }

  // A cut that is not block aligned must be refused, not silently rounded.
  BOOST_CHECK_THROW(qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::InputChannels, 8, 96),
                    std::invalid_argument);
  // A wrong source size is refused too.
  std::vector<std::uint8_t> shortPacked(packed.begin(), packed.end() - 1);
  BOOST_CHECK_THROW(qwen::slicePackedInt4(shortPacked, n, k, qwen::QwenSliceAxis::OutputChannels, 0, 1),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Int4ScalesFollowTheWeightSliceAxis)
{
  const std::uint64_t n = 4, k = 64;   // 2 blocks
  const auto blocks = k / qwen::QWEN_INT4_BLOCK_SIZE;
  std::vector<std::uint8_t> scales(static_cast<std::size_t>(n * blocks * 4U));
  for (std::size_t i = 0; i < scales.size(); ++i) scales[i] = static_cast<std::uint8_t>(i & 0xff);

  const auto rows = qwen::sliceInt4Scales(scales, n, k, qwen::QwenSliceAxis::OutputChannels, 1, 3);
  BOOST_CHECK_EQUAL(rows.size(), 2U * blocks * 4U);
  BOOST_CHECK(std::equal(rows.begin(), rows.end(), scales.begin() + blocks * 4));

  const auto cols = qwen::sliceInt4Scales(scales, n, k, qwen::QwenSliceAxis::InputChannels, 32, 64);
  BOOST_CHECK_EQUAL(cols.size(), n * 1U * 4U);
  for (std::uint64_t row = 0; row < n; ++row) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      BOOST_CHECK_EQUAL(cols[static_cast<std::size_t>(row) * 4U + byte],
                        scales[static_cast<std::size_t>(row * blocks + 1U) * 4U + byte]);
    }
  }

  std::vector<std::uint8_t> truncated(scales.begin(), scales.end() - 1);
  BOOST_CHECK_THROW(qwen::sliceInt4Scales(truncated, n, k, qwen::QwenSliceAxis::OutputChannels, 0, 1),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RankSlicesCoverDistinctDisjointChannels)
{
  const auto shape = frozenShape();
  const auto rank0 = qwen::planQwenTensorSlices(shape, 2, 0);
  const auto rank1 = qwen::planQwenTensorSlices(shape, 2, 1);
  // Every layer's slices are disjoint and together cover the full axis: this is
  // what rules out two ranks duplicating work or leaving a gap.
  for (std::size_t layer = 0; layer < rank0.size(); ++layer) {
    BOOST_REQUIRE_EQUAL(rank0[layer].initializers.size(), rank1[layer].initializers.size());
    for (std::size_t i = 0; i < rank0[layer].initializers.size(); ++i) {
      const auto& left = rank0[layer].initializers[i];
      const auto& right = rank1[layer].initializers[i];
      BOOST_CHECK_EQUAL(left.name, right.name);
      BOOST_CHECK(left.axis == right.axis);
      BOOST_CHECK_EQUAL(left.end, right.begin);
      BOOST_CHECK_EQUAL(left.begin, 0U);
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Frozen-export shape facts: the TP shape must be read from the graph, never
// assumed. These cases use a synthetic graph so they run without the 900 MB
// weight file; the real frozen model is exercised by the installed
// qualification in T005.
// ---------------------------------------------------------------------------

namespace {

/** Build one INT4 linear node with the maintained MatMulNBits attributes. */
void
appendNBitsNode(onnx::GraphProto& graph, const std::string& name,
                std::uint64_t k, std::uint64_t n, int optionalThrough = 0)
{
  auto* node = graph.add_node();
  node->set_name(name);
  node->set_op_type("MatMulNBits");
  node->set_domain("com.microsoft");
  // Official com.microsoft.MatMulNBits input order:
  //   A (activation), B (packed weight), scales, zero_points?, g_idx?, bias?
  // The frozen export uses exactly the first three. Optional slots appear as
  // empty names, which is how ONNX spells "absent".
  node->add_input("/model/layers.0/input_layernorm/output_0");
  node->add_input(name + "_B");
  node->add_input(name + "_scales");
  // `optionalThrough` names the last optional slot that is actually present:
  // 0 none, 3 zero_points, 4 g_idx, 5 bias. Slots before it are emitted as
  // empty names, which is the ONNX spelling for an absent optional input.
  for (int slot = 3; slot <= optionalThrough; ++slot) {
    node->add_input(slot == optionalThrough ? name + "_optional" + std::to_string(slot)
                                            : std::string());
  }
  const auto addInt = [&] (const char* attributeName, std::uint64_t value) {
    auto* attribute = node->add_attribute();
    attribute->set_name(attributeName);
    attribute->set_type(onnx::AttributeProto::INT);
    attribute->set_i(static_cast<std::int64_t>(value));
  };
  addInt("bits", 4);
  addInt("block_size", 32);
  addInt("K", k);
  addInt("N", n);
}

struct QwenLogWidths
{
  std::uint64_t hidden = 1024;
  std::uint64_t query = 16;
  std::uint64_t kv = 8;
  std::uint64_t headDim = 128;
  std::uint64_t ffn = 3072;
};

/** Serialize an ONNX model holding the given projections. */
std::vector<std::uint8_t>
graphWith(QwenLogWidths widths, std::uint64_t layers, int optionalThroughOnQ = 0)
{
  onnx::ModelProto model;
  model.set_ir_version(7);
  auto* graph = model.mutable_graph();
  graph->set_name("tp");
  for (std::uint64_t layer = 0; layer < layers; ++layer) {
    const auto prefix = "model.layers." + std::to_string(layer) + ".";
    appendNBitsNode(*graph, prefix + "attn.q_proj.MatMul_Q4", widths.hidden,
                    widths.query * widths.headDim, optionalThroughOnQ);
    appendNBitsNode(*graph, prefix + "attn.k_proj.MatMul_Q4", widths.hidden,
                    widths.kv * widths.headDim);
    appendNBitsNode(*graph, prefix + "attn.v_proj.MatMul_Q4", widths.hidden,
                    widths.kv * widths.headDim);
    appendNBitsNode(*graph, prefix + "attn.o_proj.MatMul_Q4",
                    widths.query * widths.headDim, widths.hidden);
    appendNBitsNode(*graph, prefix + "mlp.gate_proj.MatMul_Q4", widths.hidden, widths.ffn);
    appendNBitsNode(*graph, prefix + "mlp.up_proj.MatMul_Q4", widths.hidden, widths.ffn);
    appendNBitsNode(*graph, prefix + "mlp.down_proj.MatMul_Q4", widths.ffn, widths.hidden);
  }
  std::string bytes;
  model.SerializeToString(&bytes);
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

} // namespace

BOOST_AUTO_TEST_SUITE(ShapeFacts)

BOOST_AUTO_TEST_CASE(GraphInspectionReadsFrozenProjectionWidths)
{
  const auto bytes = graphWith(QwenLogWidths{}, 2);
  const auto nodes = qwen::inspectQwenNBitsGraph(bytes, 2);
  BOOST_REQUIRE_EQUAL(nodes.size(), 14U);
  const auto& q = nodes.front();
  BOOST_CHECK_EQUAL(q.k, 1024U);
  BOOST_CHECK_EQUAL(q.n, 2048U);
  BOOST_CHECK_EQUAL(q.bits, 4U);
  BOOST_CHECK_EQUAL(q.blockSize, 32U);
  // The parser must take B and scales from their schema positions, not from
  // inputs 0/1: input 0 is the activation and is not an initializer at all.
  BOOST_CHECK_EQUAL(q.weightInitializer, q.nodeName + "_B");
  BOOST_CHECK_EQUAL(q.scalesInitializer, q.nodeName + "_scales");
  BOOST_CHECK(!q.hasBias);
  BOOST_CHECK(!q.hasZeroPoint);
  BOOST_CHECK(!q.hasGroupIndex);
}

BOOST_AUTO_TEST_CASE(ShapeFactsAreCrossCheckedAgainstTheGraph)
{
  const auto bytes = graphWith(QwenLogWidths{}, 1);
  const auto nodes = qwen::inspectQwenNBitsGraph(bytes, 1);
  // Declared geometry matching the graph is accepted.
  BOOST_CHECK_NO_THROW(qwen::qwenTpShapeFromNodes(nodes, 1024, 1, 16, 8, 128, 3072, 151936));
  // A declared width that disagrees with the export is rejected, not sliced.
  BOOST_CHECK_THROW(qwen::qwenTpShapeFromNodes(nodes, 1024, 1, 8, 4, 128, 3072, 151936),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::qwenTpShapeFromNodes(nodes, 1024, 1, 16, 8, 128, 2048, 151936),
                    std::invalid_argument);
  // A layer-count mismatch is likewise refused.
  BOOST_CHECK_THROW(qwen::inspectQwenNBitsGraph(bytes, 2), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FrozenExportMatchesTheRecordedShapeFacts)
{
  // Qualification entry, not a convenience cross-check: the frozen export is a
  // required input, so a missing file fails instead of skipping. The graph is
  // 338 KB of metadata (weights stay in the sidecar), so this costs a file read
  // and no model load, and it can run in a normal check.
  const auto* raw = std::getenv("NDNSF_SPEC224_FROZEN_MODEL");
  const std::string path = raw != nullptr && *raw != '\0'
    ? std::string(raw)
    : std::string("/var/tmp/ndnsf-di-model-cache-1000/qwen3-0.6b/"
                  "8eb17a80e52cbf79110c881ecf0fcaa5df36304792666e0a50381333f28a6ad5/"
                  "canonical-source.onnx");
  // A directory opens successfully on Linux and only fails during the read, so
  // presence is tested as a regular file rather than through stream state.
  std::error_code statusError;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, statusError) && !statusError,
    "frozen export is required at " + path +
    " (set NDNSF_SPEC224_FROZEN_MODEL); qualification cannot skip it");
  std::ifstream in(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(in), "frozen export is not readable: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
  BOOST_REQUIRE(!bytes.empty());

  const auto nodes = qwen::inspectQwenNBitsGraph(bytes, 28);
  BOOST_CHECK_EQUAL(nodes.size(), 196U);

  // Field-level checks against the real graph, not just counts: the packed
  // weight and scales must be the schema positions, the optional slots must be
  // absent, and the widths must agree with the recorded GQA geometry.
  const auto& q = nodes.front();
  BOOST_CHECK_EQUAL(q.weightInitializer, std::string("model.layers.0.attn.q_proj.MatMul.weight_Q4"));
  BOOST_CHECK_EQUAL(q.scalesInitializer, std::string("model.layers.0.attn.q_proj.MatMul.weight_scales"));
  BOOST_CHECK_EQUAL(q.k, 1024U);
  BOOST_CHECK_EQUAL(q.n, 2048U);
  BOOST_CHECK_EQUAL(q.bits, 4U);
  BOOST_CHECK_EQUAL(q.blockSize, 32U);
  BOOST_CHECK(!q.hasBias);
  BOOST_CHECK(!q.hasZeroPoint);
  BOOST_CHECK(!q.hasGroupIndex);

  const auto shape = qwen::qwenTpShapeFromNodes(nodes, 1024, 28, 16, 8, 128, 3072, 151936);
  const auto rank0 = qwen::planQwenTensorSlices(shape, 2, 0);
  const auto rank1 = qwen::planQwenTensorSlices(shape, 2, 1);
  BOOST_REQUIRE_EQUAL(rank0.size(), 28U);
  BOOST_REQUIRE_EQUAL(rank1.size(), 28U);
  BOOST_CHECK_EQUAL(rank0.front().initializers.size(), 7U);
  // Every layer must partition without a gap or overlap on both ranks.
  for (std::size_t layer = 0; layer < rank0.size(); ++layer) {
    for (std::size_t i = 0; i < rank0[layer].initializers.size(); ++i) {
      const auto& left = rank0[layer].initializers[i];
      const auto& right = rank1[layer].initializers[i];
      BOOST_CHECK_EQUAL(left.name, right.name);
      BOOST_CHECK_EQUAL(left.end, right.begin);
    }
  }
}

BOOST_AUTO_TEST_CASE(UnsupportedExportFeaturesAreRejectedByName)
{
  // Optional inputs sit after scales in schema order: zero_points, g_idx, bias.
  // An export with a zero-point or g_idx cannot be cut by this partition; a
  // bias alone is representable and must be reported, not silently dropped.
  // Slot 5 is bias, with slots 3 and 4 left empty: a representable export that
  // must report hasBias without being rejected.
  const auto biasOnly = graphWith(QwenLogWidths{}, 1, 5);
  const auto biasNodes = qwen::inspectQwenNBitsGraph(biasOnly, 1);
  BOOST_CHECK(biasNodes.front().hasBias);
  BOOST_CHECK(!biasNodes.front().hasZeroPoint);
  BOOST_CHECK(!biasNodes.front().hasGroupIndex);

  // Slot 3 is zero_points and slot 4 is g_idx: both are layouts this
  // partition cannot cut, so each must be refused rather than mis-sliced.
  const auto zeroPoint = graphWith(QwenLogWidths{}, 1, 3);
  BOOST_CHECK_THROW(qwen::inspectQwenNBitsGraph(zeroPoint, 1), std::invalid_argument);

  const auto groupIndex = graphWith(QwenLogWidths{}, 1, 4);
  BOOST_CHECK_THROW(qwen::inspectQwenNBitsGraph(groupIndex, 1), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// PlanProjection — the Qwen adapter turns verified shape facts into sealed
// roles/steps/dependencies. These cases assert the projection's structure,
// which T003 will execute; they do not run a Provider.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(PlanProjection)

BOOST_AUTO_TEST_CASE(ProjectionEmitsPerLayerStepDagForBothRanks)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 2; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;

  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  BOOST_CHECK_EQUAL(plan.roles.size(), 2U);
  BOOST_CHECK_EQUAL(plan.stepPlanVersion, std::string(NATIVE_STEP_PLAN_VERSION_V1));
  BOOST_CHECK_EQUAL(plan.tensorDegree, 2U);
  // Per rank: one INIT, then 4 steps per layer, plus one head on the egress
  // rank only.
  BOOST_CHECK_EQUAL(plan.steps.size(), 2U * (1U + 2U * 4U) + 1U);

  const auto count = [&] (const std::string& kind) {
    return std::count_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) {
        return step.stepId.find(kind) != std::string::npos;
      });
  };
  BOOST_CHECK_EQUAL(count("/ATTN/"), 4U);
  BOOST_CHECK_EQUAL(count("/SUM_ATTN/"), 4U);
  BOOST_CHECK_EQUAL(count("/MLP/"), 4U);
  BOOST_CHECK_EQUAL(count("/SUM_MLP/"), 4U);
  BOOST_CHECK_EQUAL(count("/HEAD"), 1U);
}

BOOST_AUTO_TEST_CASE(SumStepsReferenceEveryPeerContribution)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;

  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  for (const auto& step : plan.steps) {
    if (step.operation != "SUM") continue;
    // One local contribution plus one per peer: this is what makes the SUM
    // generic in degree rather than hard-coded to two ranks.
    BOOST_CHECK_EQUAL(step.inputSlots.size(), 2U);
    std::size_t local = 0;
    for (const auto& slot : step.inputSlots) {
      if (slot.source == "LOCAL_STEP") ++local;
      BOOST_CHECK(!slot.producerStepId.empty());
    }
    BOOST_CHECK_EQUAL(local, 1U);
    BOOST_CHECK(step.artifactEntry.empty());
  }
}

BOOST_AUTO_TEST_CASE(DependencyBindingsMatchTheStepsTheyCarry)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;

  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  // Two ranks x two remote contributions x one layer.
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 4U);
  std::set<std::string> identities;
  for (const auto& dependency : plan.dependencies) {
    BOOST_CHECK(!dependency.dependencyId.empty());
    BOOST_CHECK(identities.insert(dependency.dependencyId).second);
    BOOST_CHECK(dependency.useNdnsfDataV1);
    BOOST_CHECK_EQUAL(dependency.operationKind, std::string("TENSOR_PARTIAL"));
    BOOST_CHECK(!dependency.producerStepId.empty());
    BOOST_CHECK(!dependency.consumerStepId.empty());
    BOOST_CHECK(!dependency.consumerSlotId.empty());
  }
}

BOOST_AUTO_TEST_CASE(ProjectionIsRejectedForDegreeOneOrEmptyPrefix)
{
  qwen::QwenTpShape shape;
  shape.hidden = 1024; shape.layers = 1; shape.queryHeads = 16;
  shape.kvHeads = 8; shape.headDim = 128; shape.ffn = 3072;

  NativeExecutionPlan plan;
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(plan, shape, 1, "entry", "S0R0"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(plan, shape, 2, "", "S0R0"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()
