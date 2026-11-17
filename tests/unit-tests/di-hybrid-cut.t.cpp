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

BOOST_AUTO_TEST_SUITE_END()
