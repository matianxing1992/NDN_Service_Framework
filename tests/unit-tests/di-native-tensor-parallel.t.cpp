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
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/test/unit_test.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;

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

  NativeDependencySpec dependency;
  dependency.producers = {"S0R0", "S0R1"};
  dependency.consumers = {"S0R0", "S0R1"};
  dependency.keyScope = "tp-attn-0";
  dependency.dependencyId = "tp-attn-0";
  dependency.topicPrefix = "/service/tp";
  dependency.objectNameTemplate = "{producerProvider}/{keyScope}/{sequence}";
  dependency.tensors = {"partial"};
  dependency.operationKind = "TENSOR_PARTIAL";
  dependency.useNdnsfDataV1 = true;
  dependency.producerStepId = "S0R1/ATTN/0";
  dependency.producerSlotId = "partial";
  dependency.consumerStepId = "S0R0/SUM_ATTN/0";
  dependency.consumerSlotId = "remote";
  plan.dependencies = {dependency};
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
  BOOST_REQUIRE_EQUAL(parsed.dependencies.size(), 1U);
  BOOST_CHECK_EQUAL(parsed.dependencies[0].dependencyId, "tp-attn-0");
  BOOST_CHECK_EQUAL(parsed.dependencies[0].consumerStepId, "S0R0/SUM_ATTN/0");
  BOOST_CHECK_EQUAL(parsed.dependencies[0].consumerSlotId, "remote");
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

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()
