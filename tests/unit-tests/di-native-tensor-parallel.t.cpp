/**
 * Spec 224 T001 — sealed unit/step contract and placement projection.
 *
 * These are native contract tests: they exercise the production
 * NativeExecutionPlan / NativeExecutionPlanJson types directly. They do not
 * model tensor parallelism numerically (that is T002) and do not run a
 * Provider (that is T003).
 */
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>

#include <cmath>
#include <cstring>
#include <random>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <onnx/onnx_pb.h>
#include <google/protobuf/text_format.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;
namespace qwen = ndnsf::di::qwen;

const char* DIGEST_A = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

NativeStepSlotSpec
slot(const std::string& id, const std::string& source,
     const std::string& producerStep = "")
{
  NativeStepSlotSpec result;
  result.slotId = id;
  result.dtype = "float32";
  result.shape = {std::int64_t(1), std::int64_t(25), std::int64_t(1024)};
  result.layoutDigest = DIGEST_A;
  result.source = source;
  result.producerStepId = producerStep;
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
  sum0.inputSlots = {slot("local", "LOCAL_STEP", "S0R0/ATTN/0"),
                     slot("remote", "REMOTE_STEP", "S0R1/ATTN/0")};
  sum0.outputSlots = {slot("merged", "LOCAL_STEP")};

  NativeExecutionStepSpec sum1;
  sum1.stepId = "S0R1/SUM_ATTN/0";
  sum1.unitId = "S0R1";
  sum1.operation = "SUM";
  sum1.inputSlots = {slot("local", "LOCAL_STEP", "S0R1/ATTN/0"),
                     slot("remote", "REMOTE_STEP", "S0R0/ATTN/0")};
  sum1.outputSlots = {slot("merged", "LOCAL_STEP")};

  NativeExecutionStepSpec mlp0;
  mlp0.stepId = "S0R0/MLP/0";
  mlp0.unitId = "S0R0";
  mlp0.operation = "CALL_SUBGRAPH";
  mlp0.artifactEntry = "entry/S0R0/MLP/0";
  mlp0.inputSlots = {slot("merged", "LOCAL_STEP", "S0R0/SUM_ATTN/0")};
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
  sum0.inputSlots = {slot("remote", "REMOTE_STEP", "S0R1/ATTN/0")};
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
