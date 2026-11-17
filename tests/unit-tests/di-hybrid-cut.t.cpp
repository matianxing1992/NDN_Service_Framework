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

BOOST_AUTO_TEST_SUITE_END()
