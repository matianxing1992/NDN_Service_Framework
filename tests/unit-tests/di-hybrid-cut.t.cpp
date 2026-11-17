/**
 * Spec 242 T001 - sealed stage layout and the step-plan v2 contract.
 *
 * These are native contract tests over the production NativeExecutionPlan and
 * NativePlanning types. They check that one plan can describe stages of different
 * tensor degrees, that a unit's degree is its own stage's member count, and that the
 * capability a Provider must advertise follows the declared step-plan version. They do
 * not build rank materials (T002) and do not run a Provider (T003).
 */
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ndnsf::di;

namespace {

NativeStepSlotSpec
callSlot()
{
  NativeStepSlotSpec slot;
  slot.slotId = "out";
  slot.dtype = "float32";
  slot.shape = {std::int64_t(1), std::int64_t(4), std::int64_t(8)};
  slot.layoutDigest = "sha256:stage-layout";
  slot.source = "LOCAL_STEP";
  return slot;
}

NativeExecutionStepSpec
callStep(const std::string& stepId, const std::string& unitId)
{
  NativeExecutionStepSpec step;
  step.stepId = stepId;
  step.unitId = unitId;
  step.operation = "CALL_SUBGRAPH";
  step.artifactEntry = "entry/" + unitId + "/call";
  step.outputSlots = {callSlot()};
  return step;
}

/** Two stages of unequal degree: [2, 1] over three units. */
NativeExecutionPlan
hybridPlan()
{
  NativeExecutionPlan plan;
  plan.serviceName = "/service/hybrid";
  plan.modelName = "qwen3-0.6b";
  plan.modelFamily = "qwen";
  plan.roles = {"S0R0", "S0R1", "S1R0"};
  plan.steps = {callStep("S0R0/CALL/0", "S0R0"), callStep("S0R1/CALL/0", "S0R1"),
                callStep("S1R0/CALL/0", "S1R0")};
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.tensorDegree = 2;
  plan.stageLayout = {{"stage-0", "sha256:region-0", {"S0R0", "S0R1"}},
                      {"stage-1", "sha256:region-1", {"S1R0"}}};
  return plan;
}

/** One local role of a two-rank stage window, complete enough for the fixed
 * assignment factory: the factory validates the whole Selection projection before it
 * seals the execution template. The plan is the caller's sealed stage plan.
 */
NativeSelectionProjectionV3
stageWindowProjection(const NativeExecutionPlan& sealed)
{
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/0";
  projection.requestId = "/request/hybrid-cut";
  projection.attempt = 1;
  projection.planCoreDigest = nativePlanningDigest("hybrid-cut-core");
  projection.planDigest = nativePlanningDigest("hybrid-cut-plan");
  projection.ackClosedDigest = nativePlanningDigest("hybrid-cut-ack");
  projection.offerDigest = nativePlanningDigest("hybrid-cut-offer");
  projection.securityPolicySnapshotDigest = nativePlanningDigest("hybrid-cut-security");
  projection.deadlineMs = 120000;

  // The sealed template names both stage units, so the fixed Owner cover must say
  // where the remote contribution comes from instead of leaving it unowned.
  projection.fixedProviderByRole = {{"S0R0", projection.provider},
                                    {"S0R1", "/provider/1"}};

  NativeSelectionRoleV3 role;
  role.role = "S0R0";
  role.selectedRole = "S0R0";
  role.rank = 0;
  role.layerBegin = 0;
  role.layerEnd = 1;
  role.backend = "onnxruntime-cpu";
  role.roleKind = "TENSOR_RANK";
  role.artifactDigest = nativePlanningDigest("hybrid-cut-artifact");
  role.recipeDigest = nativePlanningDigest("hybrid-cut-recipe");
  role.adapterId = "qwen";
  role.adapterVersion = "1";
  role.modelManifestDigest = nativePlanningDigest("hybrid-cut-manifest");
  role.artifactProfileDigest = nativePlanningDigest("hybrid-cut-profile");
  role.graphDigest = nativePlanningDigest("hybrid-cut-graph");
  role.canonicalInitializerDigest = nativePlanningDigest("hybrid-cut-initializer");
  role.adapterDescriptorDigest = nativePlanningDigest("hybrid-cut-adapter");
  role.assemblerDescriptorDigest = nativePlanningDigest("hybrid-cut-assembler");
  role.backendAbi = "onnxruntime-1-cpu";
  role.nodeIndices = {1, 2};
  role.expectedInputs = {{"x", "float32", {std::int64_t(1), std::int64_t(4)}}};
  role.expectedOutputs = {{"y", "float32", {std::int64_t(1), std::int64_t(4)}}};
  role.precision = "fp32";
  role.quantization = "int4";
  role.layout = "native";
  role.padding = "none";
  role.maxSourceBytes = 4096;
  role.maxAssembledBytes = 2048;
  role.maxNodes = 4;
  projection.selectedRole = role;
  projection.assembly = role;

  projection.executionRole.roleId = role.role;
  projection.executionRole.stageId = role.role;
  projection.executionRole.rank = role.rank;
  projection.executionRole.layerBegin = role.layerBegin;
  projection.executionRole.layerEnd = role.layerEnd;
  projection.executionRole.backend = role.backend;
  projection.executionRole.adapterId = role.adapterId;
  projection.executionRole.adapterVersion = role.adapterVersion;

  projection.dataflow.role = role.role;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.dataflowDigest = nativePlanningDigest("hybrid-cut-dataflow");
  projection.dataflow.terminalResponseOwner = true;

  projection.deviceBinding.mode = "CPU";
  projection.deviceBinding.provider = projection.provider;
  projection.deviceBinding.role = role.role;
  projection.deviceBinding.offerDigest = projection.offerDigest;
  projection.deviceBinding.topologyProfileDigest =
    nativePlanningDigest("hybrid-cut-topology");
  projection.deviceBinding.resourceSnapshotDigest =
    nativePlanningDigest("hybrid-cut-resources");
  projection.deviceBinding.resourceSequence = 1;
  projection.deviceBinding.sharingPolicy = "EXCLUSIVE_ROLE";

  projection.plan = sealed;
  return projection;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DiHybridCut)

BOOST_AUTO_TEST_CASE(StageDegreesFollowTheirOwnStage)
{
  const auto plan = hybridPlan();
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R0"), 2u);
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R1"), 2u);
  BOOST_CHECK_EQUAL(plan.degreeForRole("S1R0"), 1u);
  // A unit outside the layout keeps the plan-wide value; only declared members are
  // reinterpreted.
  BOOST_CHECK_EQUAL(plan.degreeForRole("missing"), 2u);
}

BOOST_AUTO_TEST_CASE(StageLayoutMustCoverEveryUnitExactlyOnce)
{
  auto duplicated = hybridPlan();
  duplicated.stageLayout.at(1).unitRoleIds.push_back("S0R0");
  BOOST_CHECK_THROW(duplicated.validateSteps(), std::invalid_argument);

  auto incomplete = hybridPlan();
  incomplete.stageLayout.pop_back();
  BOOST_CHECK_THROW(incomplete.validateSteps(), std::invalid_argument);

  auto undeclared = hybridPlan();
  undeclared.stageLayout.at(1).unitRoleIds = {"S1R9"};
  BOOST_CHECK_THROW(undeclared.validateSteps(), std::invalid_argument);

  auto duplicateStage = hybridPlan();
  duplicateStage.stageLayout.at(1).stageId = "stage-0";
  BOOST_CHECK_THROW(duplicateStage.validateSteps(), std::invalid_argument);

  auto emptyStage = hybridPlan();
  emptyStage.stageLayout.at(1).unitRoleIds.clear();
  BOOST_CHECK_THROW(emptyStage.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlanDegreeMustEqualTheWidestStage)
{
  auto plan = hybridPlan();
  plan.tensorDegree = 3;
  BOOST_CHECK_THROW(plan.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(VersionOneCannotCarryAStageLayout)
{
  auto plan = hybridPlan();
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  BOOST_CHECK_THROW(plan.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(VersionTwoAdmitsADegreeOnePlan)
{
  NativeExecutionPlan plan;
  plan.serviceName = "/service/hybrid";
  plan.roles = {"S0R0"};
  plan.steps = {callStep("S0R0/CALL/0", "S0R0")};
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.tensorDegree = 1;
  // v2 must name its stages, even when there is only one unit: the layout is what makes the
  // per-unit geometry explicit rather than inherited.
  plan.stageLayout = {{"stage-0", "sha256:region-0", {"S0R0"}}};
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R0"), 1u);

  // The v1 rule stays exactly as it was: a degree-one step plan is still refused.
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  BOOST_CHECK_THROW(plan.validateSteps(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CapabilitiesFollowTheDeclaredVersion)
{
  auto plan = hybridPlan();
  const auto v2 = nativePlanRequiredCapabilities(plan);
  BOOST_REQUIRE_EQUAL(v2.size(), 1u);
  BOOST_CHECK_EQUAL(v2.at(0), NATIVE_STEP_PLAN_CAPABILITY_V2);

  NativeExecutionStepSpec sum;
  sum.stepId = "S0R0/SUM/0";
  sum.unitId = "S0R0";
  sum.operation = "SUM";
  plan.steps.push_back(sum);
  const auto withSum = nativePlanRequiredCapabilities(plan);
  BOOST_REQUIRE_EQUAL(withSum.size(), 2u);
  BOOST_CHECK_EQUAL(withSum.at(0), NATIVE_STEP_PLAN_CAPABILITY_V2);
  BOOST_CHECK_EQUAL(withSum.at(1), NATIVE_SUM_CAPABILITY_V1);

  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  const auto v1 = nativePlanRequiredCapabilities(plan);
  BOOST_REQUIRE_EQUAL(v1.size(), 2u);
  BOOST_CHECK_EQUAL(v1.at(0), NATIVE_STEP_PLAN_CAPABILITY_V1);
}

BOOST_AUTO_TEST_CASE(PlanJsonCarriesTheStageLayout)
{
  const std::string wire = R"({
    "version": 2,
    "services": {"k": {
      "service": "/service/hybrid",
      "roles": ["S0R0", "S0R1", "S1R0"],
      "step_plan_version": "step-plan-v2",
      "tensor_degree": 2,
      "steps": [
        {"step_id": "S0R0/CALL/0", "unit_id": "S0R0", "operation": "CALL_SUBGRAPH",
         "artifact_entry": "entry/S0R0", "input_slots": [], "output_slots": [], "local_state_slots": []},
        {"step_id": "S0R1/CALL/0", "unit_id": "S0R1", "operation": "CALL_SUBGRAPH",
         "artifact_entry": "entry/S0R1", "input_slots": [], "output_slots": [], "local_state_slots": []},
        {"step_id": "S1R0/CALL/0", "unit_id": "S1R0", "operation": "CALL_SUBGRAPH",
         "artifact_entry": "entry/S1R0", "input_slots": [], "output_slots": [], "local_state_slots": []}
      ],
      "stage_layout": [
        {"stage_id": "stage-0", "source_region_digest": "sha256:region-0",
         "unit_role_ids": ["S0R0", "S0R1"]},
        {"stage_id": "stage-1", "source_region_digest": "sha256:region-1",
         "unit_role_ids": ["S1R0"]}
      ]
    }}
  })";
  std::istringstream input(wire);
  const auto plan = nativeExecutionPlanForServiceFromJson(input, "/service/hybrid");
  BOOST_REQUIRE_EQUAL(plan.stageLayout.size(), 2u);
  BOOST_CHECK_EQUAL(plan.stageLayout.at(0).stageId, "stage-0");
  BOOST_CHECK_EQUAL(plan.stageLayout.at(0).sourceRegionDigest, "sha256:region-0");
  BOOST_CHECK_EQUAL(plan.stageLayout.at(1).unitRoleIds.at(0), "S1R0");
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R1"), 2u);
  BOOST_CHECK_EQUAL(plan.degreeForRole("S1R0"), 1u);
}

BOOST_AUTO_TEST_CASE(VersionOneWireCannotCarryTheStageLayout)
{
  const std::string wire = R"({
    "version": 2,
    "services": {"k": {
      "service": "/service/hybrid",
      "roles": ["S0R0", "S0R1"],
      "step_plan_version": "step-plan-v1",
      "tensor_degree": 2,
      "steps": [
        {"step_id": "S0R0/CALL/0", "unit_id": "S0R0", "operation": "CALL_SUBGRAPH",
         "artifact_entry": "entry/S0R0", "input_slots": [], "output_slots": [], "local_state_slots": []},
        {"step_id": "S0R1/CALL/0", "unit_id": "S0R1", "operation": "CALL_SUBGRAPH",
         "artifact_entry": "entry/S0R1", "input_slots": [], "output_slots": [], "local_state_slots": []}
      ],
      "stage_layout": [
        {"stage_id": "stage-0", "source_region_digest": "sha256:region-0",
         "unit_role_ids": ["S0R0", "S0R1"]}
      ]
    }}
  })";
  std::istringstream input(wire);
  BOOST_CHECK_THROW(nativeExecutionPlanForServiceFromJson(input, "/service/hybrid"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SlicePlanningCoversOnlyItsOwnLayerRange)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  // The whole-model entry point keeps its old shape: one entry per layer, in order.
  const auto whole = qwen::planQwenTensorSlices(shape, 2, 0);
  BOOST_REQUIRE_EQUAL(whole.size(), 4u);
  BOOST_CHECK_EQUAL(whole.front().layer, 0u);
  BOOST_CHECK_EQUAL(whole.back().layer, 3u);

  // A stage covering layers 1..3 plans only those, and each entry keeps the global layer
  // index so the caller never renumbers what it left out.
  const auto region = qwen::planQwenTensorSlices(shape, 2, 1, 1, 4);
  BOOST_REQUIRE_EQUAL(region.size(), 3u);
  BOOST_CHECK_EQUAL(region.front().layer, 1u);
  BOOST_CHECK_EQUAL(region.back().layer, 3u);

  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(shape, 2, 0, 2, 2), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(shape, 2, 0, 3, 1), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::planQwenTensorSlices(shape, 2, 0, 0, 5), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TemplateAcceptsAStageLayerRange)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S1R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 1;
    unit.layerEnd = 4;
    units.push_back(unit);
  }

  const auto templ = qwen::buildQwenTpTemplate(shape, 2, "entry/S1", units, 1, 4);
  BOOST_REQUIRE_EQUAL(templ.slicesByRank.size(), 2u);
  BOOST_REQUIRE_EQUAL(templ.slicesByRank.at(0).size(), 3u);
  BOOST_CHECK_EQUAL(templ.slicesByRank.at(0).front().layer, 1u);
  BOOST_CHECK_EQUAL(templ.slicesByRank.at(1).back().layer, 3u);

  // A unit that claims the whole model while the template declares a window is refused,
  // and so is a window that runs past the model.
  auto mismatched = units;
  mismatched.at(0).layerBegin = 0;
  mismatched.at(0).layerEnd = shape.layers;
  BOOST_CHECK_THROW(qwen::buildQwenTpTemplate(shape, 2, "entry/S1", mismatched, 1, 4),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::buildQwenTpTemplate(shape, 2, "entry/S1", units, 1, 5),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::buildQwenTpTemplate(shape, 2, "entry/S1", units, 2, 2),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StageProjectionSealsAVersionTwoLayout)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  // The first stage of a [0,1) [1,4) split: it owns the embedding, so it is the stage whose
  // projection can close today. A later stage is refused until T003 delivers its input.
  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = 1;
    units.push_back(unit);
  }

  qwen::QwenTpStageWindow window{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry/S0", "S0R0", units, window);

  BOOST_CHECK_EQUAL(plan.stepPlanVersion, std::string(ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2));
  BOOST_REQUIRE_EQUAL(plan.stageLayout.size(), 1u);
  BOOST_CHECK_EQUAL(plan.stageLayout.at(0).stageId, "stage-0");
  BOOST_CHECK_EQUAL(plan.stageLayout.at(0).sourceRegionDigest, "sha256:region-0");
  BOOST_REQUIRE_EQUAL(plan.stageLayout.at(0).unitRoleIds.size(), 2u);
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R0"), 2u);
  BOOST_CHECK_EQUAL(plan.degreeForRole("S0R1"), 2u);
  BOOST_CHECK_NO_THROW(plan.validateSteps());

  // Every referenced entry belongs to the stage's own window: the window is not just a
  // label, it bounds what the plan asks the materializer to assemble.
  for (const auto& step : plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    if (step.artifactEntry.find("/HEAD/") != std::string::npos) continue;
    const auto slash = step.artifactEntry.rfind('/');
    BOOST_REQUIRE(slash != std::string::npos);
    const auto layer = std::stoull(step.artifactEntry.substr(slash + 1));
    BOOST_CHECK_LT(layer, 1u);
  }

  // The unwindowed projection keeps sealing a v1 plan with no layout.
  std::vector<qwen::QwenTpUnit> wholeUnits;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = shape.layers;
    wholeUnits.push_back(unit);
  }
  ndnsf::di::NativeExecutionPlan whole;
  qwen::projectQwenTpPlan(whole, shape, 2, "entry/S0", "S0R0", wholeUnits);
  BOOST_CHECK_EQUAL(whole.stepPlanVersion, std::string(ndnsf::di::NATIVE_STEP_PLAN_VERSION_V1));
  BOOST_CHECK(whole.stageLayout.empty());
}

BOOST_AUTO_TEST_CASE(StagePlansMergeInOrder)
{
  const auto stage = [] (const std::string& id, const std::vector<std::string>& units) {
    ndnsf::di::NativeExecutionPlan plan;
    plan.serviceName = "/service/hybrid";
    plan.modelFamily = "qwen";
    plan.plannerKind = "native-qwen-tensor";
    plan.roles = units;
    for (const auto& unit : units)
      plan.steps.push_back(callStep(unit + "/CALL/0", unit));
    plan.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2;
    // The stage's degree is its member count; a stage that claims more would be refused by
    // validateSteps(), so the fixture keeps the two consistent.
    plan.tensorDegree = units.size();
    plan.stageLayout = {{id, "sha256:" + id, units}};
    return plan;
  };

  const auto merged = qwen::mergeQwenStagePlans(
    {stage("stage-0", {"S0R0", "S0R1"}), stage("stage-1", {"S1R0"})});
  BOOST_REQUIRE_EQUAL(merged.stageLayout.size(), 2u);
  BOOST_CHECK_EQUAL(merged.stageLayout.at(0).stageId, "stage-0");
  BOOST_CHECK_EQUAL(merged.stageLayout.at(1).stageId, "stage-1");
  BOOST_CHECK_EQUAL(merged.roles.size(), 3u);
  BOOST_CHECK_EQUAL(merged.tensorDegree, 2u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S0R0"), 2u);
  // stage-1 owns a single unit in this fixture
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R0"), 1u);
  BOOST_CHECK_EQUAL(merged.stepPlanVersion,
                    std::string(ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2));
  BOOST_CHECK_EQUAL(merged.plannerKind, "native-qwen-tensor");

  BOOST_CHECK_THROW(qwen::mergeQwenStagePlans({}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::mergeQwenStagePlans({stage("same", {"A"}),
                                               stage("same", {"B"})}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::mergeQwenStagePlans({stage("s0", {"A"}),
                                               stage("s1", {"A"})}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StageWindowAndBoundaryPublisherFollowTheSealedFacts)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = 1;
    units.push_back(unit);
  }
  qwen::QwenTpStageWindow window{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan stagePlan;
  qwen::projectQwenTpPlan(stagePlan, shape, 2, "entry/S0", "S0R0", units, window);

  const auto layers = qwen::stageLayerWindow(stagePlan);
  BOOST_CHECK_EQUAL(layers.first, 0u);
  BOOST_CHECK_EQUAL(layers.second, 0u);
  // The first unit of the layout row publishes the boundary activation.
  BOOST_CHECK_EQUAL(qwen::boundaryPublisher(stagePlan), "S0R0");

  ndnsf::di::NativeExecutionPlan empty;
  BOOST_CHECK_THROW(qwen::stageLayerWindow(empty), std::invalid_argument);
  BOOST_CHECK_THROW(qwen::boundaryPublisher(empty), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(VersionTwoWithoutALayoutIsRefused)
{
  // Three units and a degree of two look plausible and used to pass, which then gave every
  // role degree two - the exact defect v2 exists to remove.
  NativeExecutionPlan plan;
  plan.serviceName = "/service/hybrid";
  plan.roles = {"S0R0", "S0R1", "S1R0"};
  plan.steps = {callStep("S0R0/CALL/0", "S0R0"), callStep("S0R1/CALL/0", "S0R1"),
                callStep("S1R0/CALL/0", "S1R0")};
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.tensorDegree = 2;
  BOOST_CHECK_THROW(plan.validateSteps(), std::invalid_argument);

  plan.stageLayout = {{"stage-0", "sha256:r0", {"S0R0", "S0R1"}},
                      {"stage-1", "sha256:r1", {"S1R0"}}};
  BOOST_CHECK_NO_THROW(plan.validateSteps());
}

BOOST_AUTO_TEST_CASE(BoundTemplateRestoresPerUnitDegrees)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = 1;
    units.push_back(unit);
  }
  qwen::QwenTpStageWindow window{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan sealed;
  qwen::projectQwenTpPlan(sealed, shape, 2, "entry/S0", "S0R0", units, window);

  // What the fixed assignment carries: the steps plus the facts that describe them.
  ndnsf::di::NativeFixedExecutionTemplateV1 fixed;
  fixed.steps = sealed.steps;
  fixed.stepPlanVersion = sealed.stepPlanVersion;
  fixed.tensorDegree = sealed.tensorDegree;
  fixed.stageLayout = sealed.stageLayout;

  ndnsf::di::NativeExecutionPlan restored;
  // The production path binds the template's dependencies before the steps
  // (NativeExecutionPlanJson.cpp:1887-1889): a remote step input with no admitting
  // dependency has nothing to wait on, and validateSteps() refuses it.
  restored.dependencies = sealed.dependencies;
  ndnsf::di::bindFixedTemplateSteps(restored, fixed);
  BOOST_REQUIRE_EQUAL(restored.stageLayout.size(), 1u);
  BOOST_CHECK_EQUAL(restored.stageLayout.at(0).stageId, "stage-0");
  BOOST_CHECK_EQUAL(restored.degreeForRole("S0R0"), 2u);
  BOOST_CHECK_EQUAL(restored.degreeForRole("S0R1"), 2u);
  BOOST_CHECK_NO_THROW(restored.validateSteps());

  // A v2 template that dropped the layout is refused rather than restored with a guessed
  // geometry.
  auto withoutLayout = fixed;
  withoutLayout.stageLayout.clear();
  ndnsf::di::NativeExecutionPlan refused;
  BOOST_CHECK_THROW(ndnsf::di::bindFixedTemplateSteps(refused, withoutLayout),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FixedAssignmentWireCarriesTheStageLayout)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  std::vector<qwen::QwenTpUnit> units;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpUnit unit;
    unit.roleId = "S0R" + std::to_string(rank);
    unit.rank = rank;
    unit.layerBegin = 0;
    unit.layerEnd = 1;
    units.push_back(unit);
  }
  qwen::QwenTpStageWindow window{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan sealed;
  qwen::projectQwenTpPlan(sealed, shape, 2, "entry/S0", "S0R0", units, window);
  BOOST_REQUIRE_EQUAL(sealed.stageLayout.size(), 1u);

  // Production writer: the sealed plan becomes a fixed assignment source.
  const auto assignment = ndnsf::di::nativeFixedRoleAssignmentV1FromProjection(
    stageWindowProjection(sealed));
  BOOST_REQUIRE(assignment != nullptr);
  BOOST_REQUIRE_EQUAL(assignment->executionTemplate()->stageLayout.size(), 1u);

  // Production reader: the same bytes a Provider fetches must restore the layout,
  // not just the steps and the plan-wide degree.
  const std::string wire = ndnsf::di::nativeFixedRoleAssignmentV1ToJson(*assignment);
  std::istringstream input(wire);
  const auto restored = ndnsf::di::nativeFixedRoleAssignmentV1FromJson(input);
  const auto& templateAfter = restored->executionTemplate();
  BOOST_REQUIRE_EQUAL(templateAfter->stageLayout.size(), 1u);
  BOOST_CHECK_EQUAL(templateAfter->stageLayout.at(0).stageId,
                    sealed.stageLayout.at(0).stageId);
  BOOST_CHECK_EQUAL(templateAfter->stageLayout.at(0).sourceRegionDigest,
                    sealed.stageLayout.at(0).sourceRegionDigest);
  BOOST_CHECK(templateAfter->stageLayout.at(0).unitRoleIds ==
              sealed.stageLayout.at(0).unitRoleIds);
  BOOST_CHECK_EQUAL(templateAfter->stepPlanVersion,
                    std::string(ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2));
  BOOST_CHECK_EQUAL(templateAfter->tensorDegree, 2u);

  // Binding the restored template is what a Provider does before execution. The
  // dependencies are admitted first, as on the production decode path, so each
  // remote step input has its edge before validateSteps() sees it.
  ndnsf::di::NativeExecutionPlan bound;
  bound.dependencies = templateAfter->dependencies;
  ndnsf::di::bindFixedTemplateSteps(bound, *templateAfter);
  BOOST_REQUIRE_EQUAL(bound.stageLayout.size(), 1u);
  BOOST_CHECK_EQUAL(bound.degreeForRole("S0R0"), 2u);
  BOOST_CHECK_EQUAL(bound.degreeForRole("S0R1"), 2u);
  BOOST_CHECK_NO_THROW(bound.validateSteps());
}

BOOST_AUTO_TEST_CASE(AsymmetricAndAllPipelineLayoutsValidate)
{
  const auto plan = [] (const std::vector<std::vector<std::string>>& stages) {
    ndnsf::di::NativeExecutionPlan value;
    value.serviceName = "/service/hybrid";
    value.modelFamily = "qwen";
    std::uint64_t widest = 0;
    for (std::size_t index = 0; index < stages.size(); ++index) {
      value.stageLayout.push_back({"stage-" + std::to_string(index),
                                   "sha256:region-" + std::to_string(index), stages.at(index)});
      widest = std::max<std::uint64_t>(widest, stages.at(index).size());
      for (const auto& unit : stages.at(index)) {
        value.roles.push_back(unit);
        value.steps.push_back(callStep(unit + "/CALL/0", unit));
      }
    }
    value.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2;
    value.tensorDegree = widest;
    return value;
  };

  // [1,2]: a plain stage in front of a sharded one, the mirror of the [2,1] case.
  const auto asymmetric = plan({{"S0R0"}, {"S1R0", "S1R1"}});
  BOOST_CHECK_NO_THROW(asymmetric.validateSteps());
  BOOST_CHECK_EQUAL(asymmetric.degreeForRole("S0R0"), 1u);
  BOOST_CHECK_EQUAL(asymmetric.degreeForRole("S1R0"), 2u);
  BOOST_CHECK_EQUAL(asymmetric.degreeForRole("S1R1"), 2u);

  // Pure pipeline: every stage degree one. v2 must describe it rather than inherit a degree.
  const auto pipeline = plan({{"S0R0"}, {"S1R0"}});
  BOOST_CHECK_NO_THROW(pipeline.validateSteps());
  BOOST_CHECK_EQUAL(pipeline.degreeForRole("S0R0"), 1u);
  BOOST_CHECK_EQUAL(pipeline.degreeForRole("S1R0"), 1u);
  BOOST_CHECK_EQUAL(pipeline.tensorDegree, 1u);
}

BOOST_AUTO_TEST_CASE(TwoStagesCloseThroughAnActivationEdge)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  const auto unitsFor = [] (const std::string& prefix, std::uint64_t degree,
                            std::uint64_t begin, std::uint64_t end) {
    std::vector<qwen::QwenTpUnit> units;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      qwen::QwenTpUnit unit;
      unit.roleId = prefix + std::to_string(rank);
      unit.rank = rank;
      unit.layerBegin = begin;
      unit.layerEnd = end;
      units.push_back(unit);
    }
    return units;
  };

  // Both stages are sharded here: a degree-one stage still needs the whole-region subgraph
  // path, which is T002's remaining item, so this case proves the edge itself.
  const auto stage0Units = unitsFor("S0R", 2, 0, 1);
  const auto stage1Units = unitsFor("S1R", 1, 1, 4);

  qwen::QwenTpStageWindow first{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan stage0;
  qwen::projectQwenTpPlan(stage0, shape, 2, "entry/S0", "S0R0", stage0Units, first);

  qwen::QwenTpStageWindow second{"stage-1", "sha256:region-1", 1, 4,
                                 "S0R0", "S0R0/MLP/0", "post_residual"};
  ndnsf::di::NativeExecutionPlan stage1;
  qwen::projectQwenTpPlan(stage1, shape, 1, "entry/S1", "S1R0", stage1Units, second);
  // Alone it cannot close: its input producer lives in stage 0. The merged plan is the first
  // object that contains every producer, and it is validated below.
  BOOST_CHECK_THROW(stage1.validateSteps(), std::invalid_argument);

  const auto merged = qwen::mergeQwenStagePlans({stage0, stage1});
  BOOST_CHECK_NO_THROW(merged.validateSteps());
  BOOST_REQUIRE_EQUAL(merged.stageLayout.size(), 2u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S0R0"), 2u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R0"), 1u);

  // The stage-1 input is the predecessor's activation, and exactly one ACTIVATION edge binds
  // it per consumer unit.
  bool bound = false;
  for (const auto& dependency : merged.dependencies) {
    if (dependency.operationKind != "ACTIVATION") continue;
    BOOST_CHECK_EQUAL(dependency.producers.size(), 1u);
    BOOST_CHECK_EQUAL(dependency.consumers.size(), 1u);
    BOOST_CHECK_EQUAL(dependency.tensors.size(), 1u);
    BOOST_CHECK(dependency.redistributions.empty());
    if (dependency.consumers.at(0) != "S1R0") continue;
    bound = true;
    BOOST_CHECK_EQUAL(dependency.producers.at(0), "S0R0");
    BOOST_CHECK_EQUAL(dependency.producerStepId, "S0R0/MLP/0");
    BOOST_CHECK_EQUAL(dependency.producerSlotId, "post_residual");
    BOOST_CHECK_EQUAL(dependency.consumerSlotId, "x");
  }
  BOOST_CHECK(bound);

  // And the slot itself points at that producer rather than at a local INIT.
  bool sawRemote = false;
  for (const auto& step : merged.steps) {
    if (step.unitId != "S1R0" || step.stepId.find("/ATTN/1") == std::string::npos) continue;
    for (const auto& slot : step.inputSlots) {
      if (slot.slotId != "x") continue;
      sawRemote = true;
      BOOST_CHECK_EQUAL(slot.source, "REMOTE_STEP");
      BOOST_CHECK_EQUAL(slot.producerStepId, "S0R0/MLP/0");
    }
  }
  BOOST_CHECK(sawRemote);
}

BOOST_AUTO_TEST_CASE(MirroredLayoutClosesThroughTheSameEdge)
{
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  shape.validate();

  const auto unitsFor = [] (const std::string& prefix, std::uint64_t degree,
                            std::uint64_t begin, std::uint64_t end) {
    std::vector<qwen::QwenTpUnit> units;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      qwen::QwenTpUnit unit;
      unit.roleId = prefix + std::to_string(rank);
      unit.rank = rank;
      unit.layerBegin = begin;
      unit.layerEnd = end;
      units.push_back(unit);
    }
    return units;
  };

  // [1,2]: a plain stage in front of a sharded one. The edge is the same one, so the mirror
  // layout must close without any special case.
  qwen::QwenTpStageWindow first{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan stage0;
  qwen::projectQwenTpPlan(stage0, shape, 1, "entry/S0", "S0R0", unitsFor("S0R", 1, 0, 1), first);

  qwen::QwenTpStageWindow second{"stage-1", "sha256:region-1", 1, 4,
                                 "S0R0", "S0R0/MLP/0", "post_residual"};
  ndnsf::di::NativeExecutionPlan stage1;
  qwen::projectQwenTpPlan(stage1, shape, 2, "entry/S1", "S1R0", unitsFor("S1R", 2, 1, 4), second);

  const auto merged = qwen::mergeQwenStagePlans({stage0, stage1});
  BOOST_CHECK_NO_THROW(merged.validateSteps());
  BOOST_CHECK_EQUAL(merged.degreeForRole("S0R0"), 1u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R0"), 2u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R1"), 2u);

  // One edge per consumer unit, both naming the same producer.
  std::set<std::string> consumers;
  for (const auto& dependency : merged.dependencies) {
    if (dependency.operationKind != "ACTIVATION") continue;
    BOOST_CHECK_EQUAL(dependency.producers.at(0), "S0R0");
    consumers.insert(dependency.consumers.at(0));
  }
  BOOST_CHECK_EQUAL(consumers.size(), 2u);
  BOOST_CHECK(consumers.count("S1R0") == 1 && consumers.count("S1R1") == 1);
}

BOOST_AUTO_TEST_CASE(FourLayerFixtureMatchesItsDeclaredShape)
{
  // The fixture is generated by tests/fixtures/spec242/make-four-layer-qwen.py with a fixed
  // seed; its digest is stable, so these facts are reproducible rather than recorded by hand.
  const std::string path = "../tests/fixtures/spec242/four-layer-qwen.onnx";
  std::ifstream input(path, std::ios::binary);
  BOOST_REQUIRE_MESSAGE(static_cast<bool>(input), "fixture missing: " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  BOOST_REQUIRE(!bytes.empty());

  std::vector<std::uint8_t> copy = bytes;
  // `deriveQwenTpShape` reads the graph facts the adapter will actually slice.
  const auto shape = qwen::deriveQwenTpShape(copy, 4);
  BOOST_CHECK_EQUAL(shape.hidden, 64u);
  BOOST_CHECK_EQUAL(shape.queryHeads, 4u);
  BOOST_CHECK_EQUAL(shape.kvHeads, 2u);
  BOOST_CHECK_EQUAL(shape.headDim, 16u);
  BOOST_CHECK_EQUAL(shape.ffn, 128u);
  BOOST_CHECK_EQUAL(shape.vocab, 64u);

  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  const auto entries = qwen::classifyQwenTpEntryNodes(model, 4);
  std::set<std::string> kinds;
  std::size_t attention = 0;
  std::size_t mlp = 0;
  for (const auto& entry : entries) {
    kinds.insert(entry.kind);
    if (entry.kind == "ATTN") ++attention;
    if (entry.kind == "MLP") ++mlp;
  }
  BOOST_CHECK(kinds.count("INIT") == 1);
  BOOST_CHECK(kinds.count("HEAD") == 1);
  BOOST_CHECK_EQUAL(attention, 4u);
  BOOST_CHECK_EQUAL(mlp, 4u);
}

BOOST_AUTO_TEST_CASE(PackedSlicesOfTheFixtureReassembleExactly)
{
  const std::string path = "../tests/fixtures/spec242/four-layer-qwen.onnx";
  std::ifstream input(path, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));

  std::vector<std::uint8_t> copy = bytes;
  const auto shape = qwen::deriveQwenTpShape(copy, 4);
  const auto rank0 = qwen::planQwenTensorSlices(shape, 2, 0, 0, 1);
  const auto rank1 = qwen::planQwenTensorSlices(shape, 2, 1, 0, 1);
  BOOST_REQUIRE_EQUAL(rank0.size(), 1u);
  BOOST_REQUIRE_EQUAL(rank1.size(), 1u);

  // q_proj shards its output channels: the two ranks must reassemble the original packed
  // bytes exactly, because the slice copies whole rows and never re-packs a nibble.
  const auto* source = [&] () -> const onnx::TensorProto* {
    for (const auto& init : model.graph().initializer()) {
      if (init.name() == "model.layers.0.attn.q_proj.MatMul.weight_Q4")
        return &init;
    }
    return nullptr;
  }();
  BOOST_REQUIRE(source != nullptr);
  const std::string raw = source->raw_data();
  const std::vector<std::uint8_t> packed(raw.begin(), raw.end());
  const std::uint64_t n = 64;    // q heads 4 * head_dim 16
  const std::uint64_t k = 64;    // hidden
  BOOST_REQUIRE_EQUAL(packed.size(), n * (k / 32) * 16);

  const auto left = qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::OutputChannels,
                                          0, 32);
  const auto right = qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::OutputChannels,
                                           32, 64);
  BOOST_REQUIRE_EQUAL(left.size(), 32 * (k / 32) * 16);
  BOOST_REQUIRE_EQUAL(right.size(), 32 * (k / 32) * 16);
  std::vector<std::uint8_t> rejoined(left);
  rejoined.insert(rejoined.end(), right.begin(), right.end());
  BOOST_CHECK(rejoined == packed);

  // And the plan's own range for rank 0 is the first half, not an arbitrary window.
  bool sawQProj = false;
  for (const auto& slice : rank0.front().initializers) {
    if (slice.name != "model.layers.0.attn.q_proj.MatMul.weight") continue;
    sawQProj = true;
    BOOST_CHECK_EQUAL(slice.begin, 0u);
    BOOST_CHECK_EQUAL(slice.end, 32u);
  }
  BOOST_CHECK(sawQProj);
}

BOOST_AUTO_TEST_CASE(StageMaterialsCarryTheRankGeometry)
{
  const std::string path = "../tests/fixtures/spec242/four-layer-qwen.onnx";
  std::ifstream input(path, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> copy = bytes;
  const auto shape = qwen::deriveQwenTpShape(copy, 4);

  // Rank 0 of the [0,1) stage: it owns layers 0..0 and the first half of every sharded
  // width. The assembled entries must describe exactly that geometry.
  qwen::QwenTpAssemblyShards shards;
  shards.slicesByLayer = qwen::planQwenTensorSlices(shape, 2, 0, 0, 1);
  qwen::QwenTpRankGeometry geometry;
  geometry.degree = 2;
  geometry.rank = 0;
  geometry.headDim = shape.headDim;
  geometry.queryWidth = shape.queryWidth();
  geometry.kvWidth = shape.kvWidth();
  geometry.hidden = shape.hidden;
  geometry.ffn = shape.ffn;
  shards.geometry = geometry;
  // The fixture keeps its weights inline, so nothing should need a sidecar read; a reader that
  // reports being asked makes that assumption fail loudly instead of silently.
  shards.readExternal = [] (std::uint64_t, std::uint64_t) -> std::vector<std::uint8_t> {
    throw std::runtime_error("unexpected sidecar read");
  };

  const auto root = std::filesystem::temp_directory_path() / "spec242-stage0-rank0";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, 4, shards, nullptr, 0, 1);

  std::set<std::string> kinds;
  std::size_t attention = 0;
  std::size_t mlp = 0;
  for (const auto& entry : table) {
    kinds.insert(entry.kind.empty() ? "UNIT" : entry.kind);
    if (entry.kind == "ATTN") ++attention;
    if (entry.kind == "MLP") ++mlp;
    BOOST_CHECK(!entry.relativeGraphPath.empty());
    BOOST_CHECK(std::filesystem::exists(root / entry.relativeGraphPath));
  }
  BOOST_CHECK_EQUAL(attention, 1u);   // only layer 0 belongs to this stage
  BOOST_CHECK_EQUAL(mlp, 1u);
  BOOST_CHECK(kinds.count("HEAD") == 0);  // the head sits on the last stage

  // The attention entry's q_proj is narrowed to this rank: 32 rows, not 64.
  for (const auto& entry : table) {
    if (entry.kind != "ATTN") continue;
    onnx::ModelProto entryModel;
    std::ifstream file(root / entry.relativeGraphPath, std::ios::binary);
    BOOST_REQUIRE(static_cast<bool>(file));
    const std::string raw((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    BOOST_REQUIRE(entryModel.ParseFromArray(raw.data(), static_cast<int>(raw.size())));
    bool sawQProj = false;
    bool sawScales = false;
    for (const auto& init : entryModel.graph().initializer()) {
      if (init.name().find("q_proj.MatMul.weight_scales") != std::string::npos) {
        sawScales = true;
        // Scales are a flattened `[rows * blocks]` tensor, so the shard's 32 rows over two
        // block columns give 64 values - half of the source's 128.
        BOOST_CHECK_EQUAL(init.dims_size(), 1);
        BOOST_CHECK_EQUAL(init.dims(0), 64);
        BOOST_CHECK_EQUAL(init.raw_data().size(), 64u * sizeof(float));
      }
      if (init.name().find("q_proj.MatMul.weight") == std::string::npos) continue;
      // The scales initializer shares the prefix; the packed weight is the one under test.
      if (init.name().size() > 7 &&
          init.name().compare(init.name().size() - 7, 7, "_scales") == 0)
        continue;
      sawQProj = true;
      BOOST_CHECK_EQUAL(init.dims(0), 32);
    }
    BOOST_CHECK(sawQProj);
    BOOST_CHECK(sawScales);
  }
  std::filesystem::remove_all(root);
}

namespace {

/** One MatMulNBits kernel over given packed weights, for a numeric slice check. */
std::vector<float>
runMatMulNBits(const std::vector<std::uint8_t>& packed,
               const std::vector<float>& scales, std::uint64_t n, std::uint64_t k,
               const std::vector<float>& input, std::uint64_t rows)
{
  onnx::ModelProto model;
  auto* graph = model.mutable_graph();
  graph->set_name("spec242-slice-check");
  auto* node = graph->add_node();
  node->set_op_type("MatMulNBits");
  node->set_domain("com.microsoft");
  node->add_input("x");
  node->add_input("w");
  node->add_input("s");
  node->add_output("y");
  const auto attribute = [node] (const char* name, std::int64_t value) {
    auto* attr = node->add_attribute();
    attr->set_name(name);
    attr->set_type(onnx::AttributeProto::INT);
    attr->set_i(value);
  };
  attribute("block_size", 32);
  attribute("K", static_cast<std::int64_t>(k));
  attribute("N", static_cast<std::int64_t>(n));
  attribute("bits", 4);

  const auto addInitializer = [graph] (const char* name, const void* data,
                                       std::size_t bytes, bool isFloat,
                                       const std::vector<std::int64_t>& dims) {
    auto* init = graph->add_initializer();
    init->set_name(name);
    init->set_data_type(isFloat ? onnx::TensorProto::FLOAT : onnx::TensorProto::UINT8);
    for (const auto dim : dims) init->add_dims(dim);
    init->set_raw_data(data, bytes);
  };
  addInitializer("w", packed.data(), packed.size(), false,
                 {static_cast<std::int64_t>(n), static_cast<std::int64_t>(k / 32), 16});
  addInitializer("s", scales.data(), scales.size() * sizeof(float), true,
                 {static_cast<std::int64_t>(scales.size())});

  const auto fillValue = [] (onnx::ValueInfoProto* value, const char* name,
                             const std::vector<std::int64_t>& dims) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    auto* shape = tensor->mutable_shape();
    for (const auto dim : dims) shape->add_dim()->set_dim_value(dim);
  };
  fillValue(graph->add_input(), "x",
            {1, static_cast<std::int64_t>(rows), static_cast<std::int64_t>(k)});
  fillValue(graph->add_output(), "y",
            {1, static_cast<std::int64_t>(rows), static_cast<std::int64_t>(n)});

  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(17);
  auto* contrib = model.add_opset_import();
  contrib->set_domain("com.microsoft");
  contrib->set_version(1);

  const auto root = std::filesystem::temp_directory_path() / "spec242-slice-check";
  std::filesystem::create_directories(root);
  const auto path = root / ("kernel-" + std::to_string(n) + ".onnx");
  const std::string serialized = model.SerializeAsString();
  {
    std::ofstream out(path, std::ios::binary);
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
  }

  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-slice-check");
  Ort::SessionOptions options;
  Ort::Session session(env, path.c_str(), options);
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<std::int64_t, 3> shape{1, static_cast<std::int64_t>(rows),
                                          static_cast<std::int64_t>(k)};
  auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()),
                                                input.size(), shape.data(), shape.size());
  const char* inputNames[] = {"x"};
  const char* outputNames[] = {"y"};
  auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
  const float* data = outputs[0].GetTensorData<float>();
  std::vector<float> result(data, data + rows * n);
  std::filesystem::remove_all(root);
  return result;
}

} // namespace

BOOST_AUTO_TEST_CASE(SlicedKernelMatchesTheUnsplitColumns)
{
  const std::string path = "../tests/fixtures/spec242/four-layer-qwen.onnx";
  std::ifstream input(path, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));

  const onnx::TensorProto* weight = nullptr;
  const onnx::TensorProto* scales = nullptr;
  for (const auto& init : model.graph().initializer()) {
    if (init.name() == "model.layers.0.attn.q_proj.MatMul.weight_Q4") weight = &init;
    if (init.name() == "model.layers.0.attn.q_proj.MatMul.weight_scales") scales = &init;
  }
  BOOST_REQUIRE(weight != nullptr);
  BOOST_REQUIRE(scales != nullptr);

  const std::uint64_t n = 64;
  const std::uint64_t k = 64;
  const std::uint64_t rows = 2;
  const std::vector<std::uint8_t> packed(weight->raw_data().begin(), weight->raw_data().end());
  std::vector<float> scaleValues(scales->raw_data().size() / sizeof(float));
  std::memcpy(scaleValues.data(), scales->raw_data().data(), scales->raw_data().size());

  std::vector<float> x(rows * k);
  for (std::size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>((i % 7) - 3) * 0.25f;

  const auto unsplit = runMatMulNBits(packed, scaleValues, n, k, x, rows);

  // The C++ slicer's own bytes for rank 0, and the matching half of the flattened scales.
  const auto left = qwen::slicePackedInt4(packed, n, k, qwen::QwenSliceAxis::OutputChannels, 0, 32);
  std::vector<float> leftScales(scaleValues.begin(),
                                scaleValues.begin() + 32 * (k / 32));
  const auto shard = runMatMulNBits(left, leftScales, 32, k, x, rows);

  BOOST_REQUIRE_EQUAL(shard.size(), rows * 32);
  for (std::uint64_t row = 0; row < rows; ++row) {
    for (std::uint64_t column = 0; column < 32; ++column) {
      const float expected = unsplit[row * n + column];
      BOOST_CHECK_SMALL(shard[row * 32 + column] - expected, 1e-4f);
    }
  }
}

BOOST_AUTO_TEST_CASE(AssembledAttentionEntryExecutes)
{
  const std::string fixturePath = "../tests/fixtures/spec242/four-layer-qwen.onnx";
  std::ifstream input(fixturePath, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> copy = bytes;
  const auto shape = qwen::deriveQwenTpShape(copy, 4);

  qwen::QwenTpAssemblyShards shards;
  shards.slicesByLayer = qwen::planQwenTensorSlices(shape, 2, 0, 0, 1);
  qwen::QwenTpRankGeometry geometry;
  geometry.degree = 2;
  geometry.rank = 0;
  geometry.headDim = shape.headDim;
  geometry.queryWidth = shape.queryWidth();
  geometry.kvWidth = shape.kvWidth();
  geometry.hidden = shape.hidden;
  geometry.ffn = shape.ffn;
  shards.geometry = geometry;
  shards.readExternal = [] (std::uint64_t, std::uint64_t) -> std::vector<std::uint8_t> {
    throw std::runtime_error("unexpected sidecar read");
  };

  const auto root = std::filesystem::temp_directory_path() / "spec242-entry-run";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto table = qwen::assembleQwenTpEntryTable(root, "S0R0", bytes, 4, shards, nullptr, 0, 1);
  BOOST_REQUIRE(!table.empty());

  // The rank's attention entry must load and run: its head counts are per rank now, so the KV
  // state buffers have to be narrowed with them or the op refuses to execute.
  bool ran = false;
  for (const auto& entry : table) {
    if (entry.kind != "ATTN") continue;
    const auto path = root / entry.relativeGraphPath;
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-entry-run");
    Ort::SessionOptions options;
    Ort::Session session(env, path.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;

    std::vector<Ort::Value> values;
    std::vector<std::string> owned;
    for (std::size_t index = 0; index < session.GetInputCount(); ++index) {
      auto name = session.GetInputNameAllocated(index, allocator);
      const auto info = session.GetInputTypeInfo(index).GetTensorTypeAndShapeInfo();
      std::vector<std::int64_t> dims = info.GetShape();
      // The assembled entry leaves the boundary rank implicit, so the caller supplies it:
      // x0 is one batch, the fixture's three tokens, and the hidden width.
      if (dims.empty())
        dims = {1, 3, static_cast<std::int64_t>(shape.hidden)};
      std::size_t count = 1;
      for (const auto dim : dims) count *= static_cast<std::size_t>(dim > 0 ? dim : 1);
      std::vector<float> data(count, 0.05f);
      values.push_back(Ort::Value::CreateTensor<float>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault), data.data(),
        data.size(), dims.data(), dims.size()));
      owned.emplace_back(name.get());
    }
    std::vector<const char*> names;
    for (const auto& name : owned) names.push_back(name.c_str());
    std::vector<const char*> outputNames;
    std::vector<std::string> outputOwned;
    for (std::size_t index = 0; index < session.GetOutputCount(); ++index) {
      auto name = session.GetOutputNameAllocated(index, allocator);
      outputOwned.emplace_back(name.get());
      outputNames.push_back(outputOwned.back().c_str());
    }
    auto outputs = session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(),
                               values.size(), outputNames.data(), outputNames.size());
    BOOST_REQUIRE(!outputs.empty());
    const auto count = outputs.front().GetTensorTypeAndShapeInfo().GetElementCount();
    const float* data = outputs.front().GetTensorData<float>();
    double magnitude = 0;
    for (std::size_t i = 0; i < count; ++i) magnitude += std::abs(data[i]);
    BOOST_CHECK(std::isfinite(magnitude));
    BOOST_CHECK(magnitude > 0);
    ran = true;
  }
  BOOST_CHECK(ran);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_SUITE_END()
