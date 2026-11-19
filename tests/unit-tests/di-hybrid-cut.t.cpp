/**
 * Spec 242 T001 - sealed stage layout and the step-plan v2 contract.
 *
 * Covers sealed geometry, material bindings and native worker/ORT prefill composition.
 * In-process dependency transport is not installed Provider or NDN qualification.
 */
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-runner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-state-role-mapping.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-generation-feedback.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-step-template-json.hpp"
#include "tests/fixtures/native-dependency-bus.hpp"
#include "tests/fixtures/owned-test-process.hpp"
#include "tests/fixtures/test-resource-paths.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace ndnsf::di;

namespace {

std::string
hybridFixturePath(const std::string& basename)
{
  return ndnsf::test::testFixturePath(std::filesystem::path("spec242") / basename).string();
}

class HybridScratch
{
public:
  explicit HybridScratch(const std::string& stem)
  {
    auto pattern = ndnsf::test::makeTestScratchTemplate(stem);
    const auto* directory = ::mkdtemp(pattern.data());
    if (directory == nullptr) {
      throw std::runtime_error("cannot create hybrid test workspace");
    }
    root = directory;
  }

  ~HybridScratch()
  {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  HybridScratch(const HybridScratch&) = delete;
  HybridScratch& operator=(const HybridScratch&) = delete;

  std::filesystem::path root;
};

NativeModelDescriptor
qwenFixtureDescriptor()
{
  const auto digest = nativePlanningDigest("spec242-planning-graph");
  NativeModelDescriptor model;
  model.modelName = "qwen-fixture";
  model.sourceRevision = "fixture-v1";
  model.contentDigest = model.semanticsDigest = model.graphDigest = digest;
  // Pinned semantic graph for fixture-v1, int4, and layer ranges [0,1), [1,4).
  model.graphDigest =
    "sha256:41b64660122c4bd042b455c1c7033dbd5756afcf209c9d8e82d88d677e7aaaee";
  model.modelFormat = "onnx";
  model.adapterId = "qwen";
  model.adapterVersion = "1";
  model.precision = "int4";
  model.quantizationSubtype = "weight_only_int4";
  model.adapter.name = "qwen";
  model.adapter.version = "1";
  model.adapter.abi = "onnxruntime";
  model.adapter.modelFormats = {"onnx"};
  model.adapter.tasks = {"text-generation"};
  model.adapter.backends = {"onnxruntime"};
  model.adapter.precisions = {"int4"};
  model.adapter.stateDigest = model.adapter.inputSchemaDigest = digest;
  model.adapter.optionsSchemaDigest = model.adapter.resultSchemaDigest = digest;
  model.adapter.graphSchemaDigest = model.adapter.splitSchemaDigest = digest;
  model.adapter.stateSchemaDigest = digest;
  return model;
}

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
  for (const auto& stage : sealed.stageLayout) {
    const auto member = std::find(stage.unitRoleIds.begin(), stage.unitRoleIds.end(),
                                  role.selectedRole);
    if (member == stage.unitRoleIds.end() || stage.degree() < 2) continue;
    role.tensorParallelDegree = stage.degree();
    role.tensorParallelRank = std::distance(stage.unitRoleIds.begin(), member);
  }
  role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
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

BOOST_AUTO_TEST_CASE(StateExpansionPreservesEachStagesSourceTensors)
{
  NativeStateTensorMapping source;
  source.inputs = {{"encoder", {{"key", {"past.0.key"}}, {"value", {"past.0.value"}}}},
                   {"decoder", {{"key", {"past.1.key", "past.2.key", "past.3.key"}},
                                {"value", {"past.1.value", "past.2.value", "past.3.value"}}}}};
  source.outputs = {{"encoder", {{"key", {"present.0.key"}}, {"value", {"present.0.value"}}}},
                    {"decoder", {{"key", {"present.1.key", "present.2.key", "present.3.key"}},
                                 {"value", {"present.1.value", "present.2.value",
                                            "present.3.value"}}}}};
  // Unit names deliberately carry no stage/rank syntax. Ownership is explicit.
  const std::map<std::string, std::string> owners{
    {"alpha", "encoder"}, {"beta", "encoder"}, {"gamma", "decoder"}};
  const auto mapped = detail::remapNativeStateRoles(source, owners);
  BOOST_REQUIRE_EQUAL(mapped.inputs.size(), 3);
  BOOST_REQUIRE_EQUAL(mapped.outputs.size(), 3);
  for (const auto* unit : {"alpha", "beta"}) {
    BOOST_CHECK(mapped.inputs.at(unit) == source.inputs.at("encoder"));
    BOOST_CHECK(mapped.outputs.at(unit) == source.outputs.at("encoder"));
  }
  BOOST_CHECK(mapped.inputs.at("gamma") == source.inputs.at("decoder"));
  BOOST_CHECK(mapped.outputs.at("gamma") == source.outputs.at("decoder"));
  BOOST_CHECK(mapped.inputs.at("gamma") != mapped.inputs.at("alpha"));

  // The same expansion supports a later tensor stage and the single-stage TP path.
  auto reversed = owners;
  reversed.at("beta") = "decoder";
  const auto laterTensor = detail::remapNativeStateRoles(source, reversed);
  BOOST_CHECK(laterTensor.inputs.at("beta") == source.inputs.at("decoder"));
  BOOST_CHECK(laterTensor.outputs.at("beta") == source.outputs.at("decoder"));
  const auto singleStage = detail::remapNativeStateRoles(
    source, {{"alpha", "encoder"}, {"beta", "encoder"}});
  BOOST_CHECK(singleStage.inputs.at("alpha") == singleStage.inputs.at("beta"));
  BOOST_CHECK(singleStage.outputs.at("alpha") == singleStage.outputs.at("beta"));

  BOOST_CHECK_THROW(detail::remapNativeStateRoles(source, {}), std::invalid_argument);
  BOOST_CHECK_THROW(detail::remapNativeStateRoles(source, {{"alpha", "missing"}}),
                    std::invalid_argument);
  auto missing = source;
  missing.outputs.erase("decoder");
  BOOST_CHECK_THROW(detail::remapNativeStateRoles(missing, owners), std::invalid_argument);
  missing = source;
  missing.inputs.at("decoder").clear();
  BOOST_CHECK_THROW(detail::remapNativeStateRoles(missing, owners), std::invalid_argument);
  missing.outputs.at("decoder").clear();
  const auto stateless = detail::remapNativeStateRoles(missing, owners);
  BOOST_CHECK(stateless.inputs.at("gamma").empty());
  BOOST_CHECK(stateless.outputs.at("gamma").empty());
  BOOST_CHECK(stateless.inputs.at("alpha") == source.inputs.at("encoder"));
  BOOST_CHECK_THROW(detail::remapNativeStateRoles(source, {{"", "encoder"}}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(detail::remapNativeStateRoles(source, {{"alpha", ""}}),
                    std::invalid_argument);
}

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

BOOST_AUTO_TEST_CASE(HybridOwnershipUsesTheCatalogPlanningGraph)
{
  auto model = qwenFixtureDescriptor();
  const auto digest = nativePlanningDigest("spec242-planning-graph");
  qwen::NativeQwenLayerSplit splitter({{0, 1}, {1, 4}},
    {{"first", digest}, {"last", digest}}, {{"first", 4096}, {"last", 4096}},
    {"first", "last"}, {1, 1});
  auto graph = splitter.inspectGraph(model, "fixture-v1", 100);
  qwen::QwenTpShape shape;
  shape.hidden = 64;
  shape.layers = 4;
  shape.queryHeads = 4;
  shape.kvHeads = 2;
  shape.headDim = 16;
  shape.ffn = 128;
  shape.vocab = 64;
  std::vector<std::vector<qwen::QwenTpUnit>> units(2);
  std::map<std::string, std::string> artifacts;
  std::map<std::string, NativeRoleResourceRequirement> requirements;
  for (std::size_t stage = 0; stage < 2; ++stage) {
    for (std::uint64_t rank = 0; rank < (stage == 0 ? 2u : 1u); ++rank) {
      qwen::QwenTpUnit unit;
      unit.roleId = "unit-" + std::to_string(stage) + "-" + std::to_string(rank);
      unit.rank = rank;
      unit.layerBegin = stage == 0 ? 0 : 1;
      unit.layerEnd = stage == 0 ? 1 : 4;
      artifacts[unit.roleId] = digest;
      requirements[unit.roleId] = NativeRoleResourceRequirement{{"onnxruntime"}, 4096};
      units[stage].push_back(std::move(unit));
    }
  }
  const std::vector<qwen::QwenTpStageWindow> windows{
    {"first", digest, 0, 1},
    {"last", digest, 1, 4, "unit-0-0", "unit-0-0/MLP/0", "post_residual",
     "unit-0-0/SUM_MLP/0", "merged"}};
  const auto build = [&] {
    return qwen::buildQwenHybridCandidate(graph, shape, windows, units, "entry",
      "unit-1-0", model, artifacts, requirements);
  };
  const auto candidate = build();
  BOOST_CHECK_NO_THROW(candidate.validate(graph));
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("embedding"), "unit-0-0");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("layer-00"), "unit-0-0");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("layer-01"), "unit-1-0");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("final-norm-head"), "unit-1-0");
  BOOST_CHECK(candidate.roleNodeIdsByRole.at("unit-0-0") ==
              candidate.roleNodeIdsByRole.at("unit-0-1"));
  BOOST_CHECK_EQUAL(candidate.roleNodeIdsByRole.at("unit-1-0").size(), 4u);
  // A terminal degree-three stage may explicitly choose its middle member as egress.
  shape.queryHeads = shape.kvHeads = 12;
  shape.headDim = 32;
  shape.ffn = 384;
  for (std::uint64_t rank = 1; rank < 3; ++rank) {
    auto unit = units[1].front();
    unit.rank = rank;
    unit.roleId = "unit-1-" + std::to_string(rank);
    artifacts[unit.roleId] = nativePlanningDigest(unit.roleId);
    requirements[unit.roleId] = requirements.at("unit-1-0");
    units[1].push_back(std::move(unit));
  }
  const auto middle = qwen::buildQwenHybridCandidate(graph, shape, windows, units, "entry",
    "unit-1-1", model, artifacts, requirements);
  BOOST_CHECK_NO_THROW(middle.validate(graph));
  std::size_t heads = 0;
  for (const auto& step : middle.executionPlan.steps) {
    if (step.stepId.find("/HEAD") == std::string::npos) continue;
    ++heads;
    BOOST_CHECK_EQUAL(step.unitId, "unit-1-1");
  }
  BOOST_CHECK_EQUAL(heads, 1u);
  BOOST_CHECK_THROW(qwen::buildQwenHybridCandidate(graph, shape, windows, units, "entry",
    "unit-0-0", model, artifacts, requirements), std::invalid_argument);
  graph.nodes.at(1).id = "unmapped-node";
  BOOST_CHECK_THROW(build(), std::invalid_argument);
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

  auto invalid = nativeParseJson(wire);
  auto& invalidTemplate = invalid.at("execution_template");
  invalidTemplate["steps"] = NativeJson::array();
  std::istringstream malformed(nativeCanonicalJson(invalid));
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromJson(malformed), std::invalid_argument);
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

  const std::vector<std::vector<std::string>> heterogeneous{
    {"entry-left", "entry-right"}, {"router-west", "router-east", "router-north"},
    {"bridge"}, {"compute-red", "compute-blue", "compute-green", "compute-yellow"},
    {"tail"}, {"sink-a", "sink-b"}};
  for (std::size_t count = 2; count <= heterogeneous.size(); ++count) {
    BOOST_TEST_CONTEXT("stage count " << count) {
      const auto layout = plan({heterogeneous.begin(), heterogeneous.begin() + count});
      BOOST_CHECK_NO_THROW(layout.validateSteps());
      BOOST_REQUIRE_EQUAL(layout.stageLayout.size(), count);
      for (std::size_t stage = 0; stage < count; ++stage) {
        const auto& members = heterogeneous.at(stage);
        BOOST_CHECK(layout.stageLayout.at(stage).unitRoleIds == members);
        for (const auto& member : members)
          BOOST_CHECK_EQUAL(layout.degreeForRole(member), members.size());
      }
      BOOST_CHECK_EQUAL(layout.tensorDegree, count >= 4 ? 4u : 3u);
    }
  }
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

  // This checks the projected ports; degree-one materialization is a separate boundary.
  const auto stage0Units = unitsFor("S0R", 2, 0, 1);
  const auto stage1Units = unitsFor("S1R", 1, 1, 4);

  qwen::QwenTpStageWindow first{"stage-0", "sha256:region-0", 0, 1};
  ndnsf::di::NativeExecutionPlan stage0;
  qwen::projectQwenTpPlan(stage0, shape, 2, "entry/S0", "S0R0", stage0Units, first);

  qwen::QwenTpStageWindow second{"stage-1", "sha256:region-1", 1, 4,
                                 "S0R0", "S0R0/MLP/0", "post_residual",
                                 "S0R0/SUM_MLP/0", "merged"};
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

  // The next layer's skip normalization needs two distinct complete values.
  std::set<std::string> bound;
  for (const auto& dependency : merged.dependencies) {
    if (dependency.operationKind != "ACTIVATION") continue;
    BOOST_CHECK_EQUAL(dependency.producers.size(), 1u);
    BOOST_CHECK_EQUAL(dependency.consumers.size(), 1u);
    BOOST_CHECK_EQUAL(dependency.tensors.size(), 1u);
    BOOST_CHECK(dependency.redistributions.empty());
    if (dependency.consumers.at(0) != "S1R0") continue;
    bound.insert(dependency.consumerSlotId);
    BOOST_CHECK_EQUAL(dependency.producers.at(0), "S0R0");
    const bool residual = dependency.consumerSlotId == "post_attention";
    BOOST_CHECK_EQUAL(dependency.producerStepId,
                      residual ? "S0R0/MLP/0" : "S0R0/SUM_MLP/0");
    BOOST_CHECK_EQUAL(dependency.producerSlotId, residual ? "post_residual" : "merged");
    BOOST_CHECK_EQUAL(dependency.consumerStepId, "S1R0/ATTN/1");
  }
  BOOST_CHECK(bound == std::set<std::string>({"post_attention", "previous_mlp"}));

  // And the slot itself points at that producer rather than at a local INIT.
  std::set<std::string> remoteSlots;
  for (const auto& step : merged.steps) {
    if (step.unitId != "S1R0" || step.stepId.find("/ATTN/1") == std::string::npos) continue;
    for (const auto& slot : step.inputSlots) {
      if (slot.slotId != "post_attention" && slot.slotId != "previous_mlp") continue;
      remoteSlots.insert(slot.slotId);
      BOOST_CHECK_EQUAL(slot.source, "REMOTE_STEP");
      const bool residual = slot.slotId == "post_attention";
      BOOST_CHECK_EQUAL(slot.producerStepId,
                        residual ? "S0R0/MLP/0" : "S0R0/SUM_MLP/0");
      BOOST_CHECK_EQUAL(slot.producerSlotId, residual ? "post_residual" : "merged");
    }
  }
  BOOST_CHECK(remoteSlots == bound);

  NativeFixedExecutionTemplateV1 fixed;
  fixed.steps = merged.steps;
  fixed.stepPlanVersion = merged.stepPlanVersion;
  fixed.tensorDegree = merged.tensorDegree;
  fixed.stageLayout = merged.stageLayout;
  NativeExecutionPlan restored;
  restored.dependencies = merged.dependencies;
  BOOST_CHECK_NO_THROW(bindFixedTemplateSteps(restored, fixed));
  BOOST_CHECK_NO_THROW(restored.validateSteps());
  // A Provider cannot bind the template after losing the MLP admission edge.
  restored.dependencies.erase(std::remove_if(restored.dependencies.begin(),
    restored.dependencies.end(), [] (const auto& dependency) {
      return dependency.consumerSlotId == "previous_mlp";
    }), restored.dependencies.end());
  BOOST_CHECK_THROW(restored.validateSteps(), std::invalid_argument);

  auto incomplete = second;
  incomplete.upstreamMlpStepId.clear();
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage1, shape, 1, "entry/S1", "S1R0",
                                           stage1Units, incomplete), std::invalid_argument);
  incomplete = second;
  incomplete.upstreamMlpSlotId = "partial";
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage1, shape, 1, "entry/S1", "S1R0",
                                           stage1Units, incomplete), std::invalid_argument);
  incomplete.upstreamMlpStepId = "S0R0/MLP/0";
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage1, shape, 1, "entry/S1", "S1R0",
                                           stage1Units, incomplete), std::invalid_argument);
  auto missingProducer = merged;
  missingProducer.steps.erase(std::remove_if(missingProducer.steps.begin(),
    missingProducer.steps.end(), [] (const auto& step) {
      return step.stepId == "S0R0/SUM_MLP/0";
    }), missingProducer.steps.end());
  BOOST_CHECK_THROW(missingProducer.validateSteps(), std::invalid_argument);
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
                                 "S0R0", "S0R0/MLP/0", "post_residual",
                                 "S0R0/SUM_MLP/0", "merged"};
  ndnsf::di::NativeExecutionPlan stage1;
  qwen::projectQwenTpPlan(stage1, shape, 2, "entry/S1", "S1R0", unitsFor("S1R", 2, 1, 4), second);

  const auto merged = qwen::mergeQwenStagePlans({stage0, stage1});
  BOOST_CHECK_NO_THROW(merged.validateSteps());
  BOOST_CHECK_EQUAL(merged.degreeForRole("S0R0"), 1u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R0"), 2u);
  BOOST_CHECK_EQUAL(merged.degreeForRole("S1R1"), 2u);

  // Both complete values fan out to both units of the later tensor stage.
  std::set<std::string> consumers;
  std::map<std::string, std::set<std::string>> portsByConsumer;
  std::size_t activationEdges = 0;
  for (const auto& dependency : merged.dependencies) {
    if (dependency.operationKind != "ACTIVATION") continue;
    ++activationEdges;
    BOOST_CHECK_EQUAL(dependency.producers.at(0), "S0R0");
    const bool residual = dependency.consumerSlotId == "post_attention";
    BOOST_CHECK_EQUAL(dependency.producerStepId,
                      residual ? "S0R0/MLP/0" : "S0R0/SUM_MLP/0");
    BOOST_CHECK_EQUAL(dependency.producerSlotId, residual ? "post_residual" : "merged");
    BOOST_CHECK_EQUAL(dependency.consumerStepId, dependency.consumers.at(0) + "/ATTN/1");
    BOOST_CHECK(dependency.tensors == std::vector<std::string>({dependency.consumerSlotId}));
    consumers.insert(dependency.consumers.at(0));
    portsByConsumer[dependency.consumers.at(0)].insert(dependency.consumerSlotId);
  }
  BOOST_CHECK_EQUAL(consumers.size(), 2u);
  BOOST_CHECK_EQUAL(activationEdges, 4u);
  BOOST_CHECK(consumers.count("S1R0") == 1 && consumers.count("S1R1") == 1);
  for (const auto& consumer : consumers)
    BOOST_CHECK(portsByConsumer.at(consumer) ==
                std::set<std::string>({"post_attention", "previous_mlp"}));

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"S0R0", "/test/source"},
                               {"S1R0", "/test/receiver-a"},
                               {"S1R1", "/test/receiver-b"}};
  std::map<std::string, std::string> outputNameByScope;
  for (const auto& role : merged.roles) {
    const auto spec = roleSpecFor(merged, role, "hybrid-session", assignment, "");
    for (const auto& edge : spec.outputs) {
      if (edge.operationKind != "ACTIVATION") continue;
      BOOST_CHECK(edge.plannedDataName.find('{') == std::string::npos);
      BOOST_CHECK(edge.plannedDataName.find(edge.producerProvider + "/") == 0);
      BOOST_CHECK(outputNameByScope.emplace(edge.scope, edge.plannedDataName).second);
    }
  }
  std::set<std::string> names;
  for (const auto& [scope, name] : outputNameByScope)
    BOOST_CHECK(names.insert(name).second);
  std::set<std::string> boundaryNames;
  for (const auto& consumer : consumers) {
    const auto spec = roleSpecFor(merged, consumer, "hybrid-session", assignment, "");
    for (const auto& edge : spec.inputs) {
      if (edge.operationKind != "ACTIVATION") continue;
      BOOST_CHECK_EQUAL(edge.plannedDataName, outputNameByScope.at(edge.scope));
      BOOST_CHECK(boundaryNames.insert(edge.plannedDataName).second);
    }
  }
  BOOST_CHECK_EQUAL(boundaryNames.size(), 4u);
}

BOOST_AUTO_TEST_CASE(FourLayerFixtureMatchesItsDeclaredShape)
{
  // The fixture is generated by tests/fixtures/spec242/make-four-layer-qwen.py with a fixed
  // seed; its digest is stable, so these facts are reproducible rather than recorded by hand.
  const std::string path = hybridFixturePath("four-layer-qwen.onnx");
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
  const std::string path = hybridFixturePath("four-layer-qwen.onnx");
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

BOOST_AUTO_TEST_CASE(RankAdaptationRejectsBiasOnlyOnShardedProjections)
{
  std::ifstream input(hybridFixturePath("four-layer-qwen.onnx"), std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  onnx::ModelProto original;
  BOOST_REQUIRE(original.ParseFromIstream(&input));
  const auto wire = original.SerializeAsString();
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());
  const auto shape = qwen::deriveQwenTpShape(bytes, 4);
  const qwen::QwenTpRankGeometry geometry{2, 0, shape.headDim, shape.queryWidth(),
    shape.kvWidth(), shape.hidden, shape.ffn};
  NativeExecutionPlan plan;
  qwen::projectQwenTpPlan(plan, shape, 2, "entry", "S0R0");
  std::vector<NativeExecutionStepSpec> steps;
  for (const auto& step : plan.steps)
    if (step.unitId == "S0R0") steps.push_back(step);
  HybridScratch workspace("spec242-bias-admission");
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-bias-admission");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  for (const auto* projection : {"q_proj", "k_proj", "v_proj", "o_proj",
                                  "gate_proj", "up_proj", "down_proj", "lm_head"}) {
    BOOST_TEST_CONTEXT("projection=" << projection) {
      auto model = original;
      auto* graph = model.mutable_graph();
      if (std::string(projection) == "lm_head") {
        // This fixture's ordinary head is MatMul. Make a legal unsharded NBits head
        // from its same-width Q projection; do not attach NBits operands to MatMul.
        BOOST_REQUIRE_EQUAL(shape.queryWidth(), shape.vocab);
        const auto head = std::find_if(graph->mutable_node()->begin(),
          graph->mutable_node()->end(), [] (const auto& node) {
            return node.name().find("lm_head") != std::string::npos;
          });
        const auto query = std::find_if(original.graph().node().begin(),
          original.graph().node().end(), [] (const auto& node) {
            return node.op_type() == "MatMulNBits" &&
                   node.name().find("q_proj") != std::string::npos;
          });
        BOOST_REQUIRE(head != graph->mutable_node()->end());
        BOOST_REQUIRE(query != original.graph().node().end());
        const auto name = head->name();
        const auto activation = head->input(0);
        const auto output = head->output(0);
        *head = *query;
        head->set_name(name);
        head->set_input(0, activation);
        head->set_output(0, output);
      }
      const auto found = std::find_if(graph->mutable_node()->begin(),
        graph->mutable_node()->end(), [&] (const auto& node) {
          return node.op_type() == "MatMulNBits" &&
                 node.name().find(projection) != std::string::npos;
        });
      BOOST_REQUIRE(found != graph->mutable_node()->end());
      const auto nodeName = found->name();
      std::int64_t width = 0;
      for (const auto& attr : found->attribute())
        if (attr.name() == "N") width = attr.i();
      BOOST_REQUIRE_GT(width, 0);
      const auto biasName = found->output(0) + ".test-bias";
      while (found->input_size() < 6) found->add_input("");
      found->set_input(5, biasName);
      auto* bias = graph->add_initializer();
      bias->set_name(biasName);
      bias->set_data_type(onnx::TensorProto::FLOAT);
      bias->add_dims(width);
      for (std::int64_t i = 0; i < width; ++i) bias->add_float_data(0.125f);
      // Bias is legal in the original full graph; inspection must not reject an ordinary
      // region merely because another region will later use tensor parallelism.
      const auto biasedWire = model.SerializeAsString();
      const std::vector<std::uint8_t> biasedBytes(biasedWire.begin(), biasedWire.end());
      const auto facts = qwen::inspectQwenNBitsGraph(biasedBytes, 4);
      BOOST_CHECK(std::any_of(facts.begin(), facts.end(), [&] (const auto& node) {
        return node.nodeName == nodeName && node.hasBias;
      }));
      Ort::Session session(env, biasedWire.data(), biasedWire.size(), options);
      std::array<std::int64_t, 3> tokens{1, 2, 3};
      const std::array<std::int64_t, 2> dimensions{1, 3};
      auto values = Ort::Value::CreateTensor<std::int64_t>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
        tokens.data(), tokens.size(), dimensions.data(), dimensions.size());
      const char* inputs[] = {"input_ids"};
      const char* outputs[] = {"logits"};
      const auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &values, 1, outputs, 1);
      const auto* logits = result.front().GetTensorData<float>();
      const auto count = result.front().GetTensorTypeAndShapeInfo().GetElementCount();
      BOOST_REQUIRE_GT(count, 0u);
      BOOST_CHECK(std::all_of(logits, logits + count, [] (float value) {
        return std::isfinite(value);
      }));
      auto rank = model;
      const auto directory = workspace.root / projection;
      BOOST_REQUIRE(std::filesystem::create_directory(directory));
      const auto modelPath = directory / "model.onnx";
      {
        std::ofstream output(modelPath, std::ios::binary);
        output.write(biasedWire.data(), static_cast<std::streamsize>(biasedWire.size()));
        BOOST_REQUIRE(static_cast<bool>(output));
      }
      NativeModelRunnerSpec spec;
      spec.role = "S0R0";
      spec.kind = "onnx";
      spec.backend = "onnxruntime-cpu";
      spec.path = modelPath.string();
      if (std::string(projection) == "lm_head") {
        BOOST_CHECK_NO_THROW(qwen::adaptQwenTpEntryParameters(rank, geometry));
        BOOST_CHECK_NO_THROW(qwen::configureQwenTpRankSpec(spec, 4, steps,
          8U << 20, 2, 0, "S0R0"));
      }
      else {
        BOOST_CHECK_EXCEPTION(qwen::adaptQwenTpEntryParameters(rank, geometry),
          std::invalid_argument, [&] (const auto& error) {
            return std::string(error.what()).find("unsupported bias: " + nodeName) !=
                   std::string::npos;
          });
        // Cold and disk-cache assembly share this production entry reconstruction path.
        BOOST_CHECK_EXCEPTION(qwen::configureQwenTpRankSpec(spec, 4, steps,
          8U << 20, 2, 0, "S0R0"), std::invalid_argument, [&] (const auto& error) {
            return std::string(error.what()).find("unsupported bias: " + nodeName) !=
                   std::string::npos;
          });
      }
      // ONNX represents absent optional operands with empty slots, not only short lists.
      found->set_input(5, "");
      BOOST_CHECK_NO_THROW(qwen::adaptQwenTpEntryParameters(model, geometry));
    }
  }
}

BOOST_AUTO_TEST_CASE(StageMaterialsCarryTheRankGeometry)
{
  const std::string path = hybridFixturePath("four-layer-qwen.onnx");
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

  HybridScratch workspace("spec242-stage0-rank0");
  const auto& root = workspace.root;
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
}

BOOST_AUTO_TEST_CASE(WindowAssemblyWritesOnlyOwnedEntriesAndAcceptsRegionGraphs)
{
  std::ifstream input(hybridFixturePath("four-layer-qwen.onnx"), std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  const auto entries = qwen::classifyQwenTpEntryNodes(model, 4);
  HybridScratch workspace("spec242-window-materials");
  const auto& root = workspace.root;
  const qwen::QwenTpAssemblyShards unsliced;
  const auto first = qwen::assembleQwenTpEntryTable(
    root / "first", "first-unit", bytes, 4, unsliced, nullptr, 0, 1);
  BOOST_REQUIRE_EQUAL(first.size(), 3u);
  std::uint64_t firstBytes = 0;
  for (const auto& entry : first) {
    BOOST_CHECK(entry.kind == "INIT" || entry.layer == 0);
    firstBytes += std::filesystem::file_size(root / "first" / entry.relativeGraphPath);
  }
  const auto fileCount = [] (const std::filesystem::path& directory) {
    return std::distance(std::filesystem::directory_iterator(directory),
                         std::filesystem::directory_iterator{});
  };
  BOOST_CHECK_EQUAL(fileCount(root / "first"), first.size());
  qwen::QwenTpAssemblyControl control;
  control.maxAssembledBytes = firstBytes;
  BOOST_CHECK_NO_THROW(qwen::assembleQwenTpEntryTable(
    root / "bounded", "first-unit", bytes, 4, unsliced, &control, 0, 1));
  BOOST_CHECK_EQUAL(fileCount(root / "bounded"), first.size());
  control.maxAssembledBytes = firstBytes - 1;
  BOOST_CHECK_THROW(qwen::assembleQwenTpEntryTable(
    root / "too-small", "first-unit", bytes, 4, unsliced, &control, 0, 1),
    std::runtime_error);

  // Extract the actual later source region; it has no embedding, INIT or layer zero.
  qwen::QwenTpEntryNodes region;
  region.kind = "REGION";
  for (const auto& entry : entries) {
    if (entry.kind == "HEAD" ||
        ((entry.kind == "ATTN" || entry.kind == "MLP") && entry.layer >= 1)) {
      region.nodeIndices.insert(region.nodeIndices.end(),
                                entry.nodeIndices.begin(), entry.nodeIndices.end());
    }
  }
  std::sort(region.nodeIndices.begin(), region.nodeIndices.end());
  auto source = model;
  auto later = qwen::extractQwenTpEntryModel(source, region, "later-region");
  // The maintained export may derive the tied head weight with a constant-only
  // transpose. It belongs to HEAD and must not force an otherwise empty INIT.
  auto* graph = later.mutable_graph();
  int headIndex = -1;
  for (int index = 0; index < graph->node_size(); ++index) {
    if (graph->node(index).name() == "lm_head") headIndex = index;
  }
  BOOST_REQUIRE(headIndex >= 0);
  const auto weight = graph->node(headIndex).input(1);
  auto* transpose = graph->add_node();
  transpose->set_name("head-weight-transpose");
  transpose->set_op_type("Transpose");
  transpose->add_input(weight);
  transpose->add_output("head-transposed-weight");
  auto* perm = transpose->add_attribute();
  perm->set_name("perm");
  perm->set_type(onnx::AttributeProto::INTS);
  perm->add_ints(1);
  perm->add_ints(0);
  graph->mutable_node()->SwapElements(headIndex, graph->node_size() - 1);
  graph->mutable_node(graph->node_size() - 1)->set_input(1, "head-transposed-weight");
  const auto serialized = later.SerializeAsString();
  const std::vector<std::uint8_t> regionBytes(serialized.begin(), serialized.end());
  // The full-model API still refuses an incomplete stack.
  BOOST_CHECK_THROW(qwen::classifyQwenTpEntryNodes(later, 4), std::invalid_argument);
  const auto table = qwen::assembleQwenTpEntryTable(
    root / "later", "later-unit", regionBytes, 4, unsliced, nullptr, 1, 4);
  BOOST_REQUIRE_EQUAL(table.size(), 7u);
  BOOST_CHECK_EQUAL(fileCount(root / "later"), table.size());
  std::set<std::uint64_t> attentionLayers;
  bool relocated = false;
  for (const auto& entry : table) {
    BOOST_CHECK(entry.kind != "INIT");
    BOOST_CHECK(entry.layer >= 1 && entry.layer <= 4);
    if (entry.kind == "ATTN") attentionLayers.insert(entry.layer);
    if (entry.kind == "HEAD") {
      BOOST_CHECK_EQUAL(entry.inputSlotByGraphName.at("model.layers.3/mlp.down_proj"), "merged");
      BOOST_CHECK_EQUAL(entry.inputSlotByGraphName.at("model.layers.3/attn.residual"),
                        "post_residual");
      for (const auto& name : {"model.layers.3/mlp.down_proj", "model.layers.3/attn.residual"})
        BOOST_CHECK(std::find(entry.synthesizedInputGraphNames.begin(),
                              entry.synthesizedInputGraphNames.end(), name) ==
                    entry.synthesizedInputGraphNames.end());
      std::ifstream headFile(root / "later" / entry.relativeGraphPath, std::ios::binary);
      const std::string headBytes((std::istreambuf_iterator<char>(headFile)),
                                   std::istreambuf_iterator<char>());
      onnx::ModelProto head;
      BOOST_REQUIRE(head.ParseFromString(headBytes));
      for (const auto& node : head.graph().node())
        if (node.name() == "head-weight-transpose") relocated = true;
    }
  }
  BOOST_CHECK(relocated);
  BOOST_CHECK(attentionLayers == std::set<std::uint64_t>({1, 2, 3}));

  // Exercise the same configure entry point used by cold and cached assembly.
  // These declarations verify material set agreement, not worker input admission.
  std::vector<NativeExecutionStepSpec> ownedSteps;
  for (const auto& entry : table) {
    NativeExecutionStepSpec step;
    step.stepId = entry.entryId;
    step.unitId = "later-unit";
    step.operation = "CALL_SUBGRAPH";
    step.artifactEntry = entry.entryId;
    ownedSteps.push_back(std::move(step));
  }
  std::filesystem::create_directory(root / "configured");
  const auto modelPath = root / "configured" / "model.onnx";
  {
    std::ofstream output(modelPath, std::ios::binary);
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    BOOST_REQUIRE(output.good());
  }
  NativeModelRunnerSpec spec;
  spec.role = "later-unit";
  spec.path = modelPath.string();
  for (const std::uint64_t degree : {1u, 2u}) {
    spec.backend = degree == 1 ? "onnxruntime-cpu" : "onnxruntime-cuda";
    const auto selectedBackend = spec.backend;
    qwen::configureQwenTpRankSpec(spec, 4, ownedSteps, 1024 * 1024,
                                  degree, 0, "later-unit", nullptr, 1);
    BOOST_CHECK_EQUAL(spec.backend, "native-qwen-tensor");
    BOOST_CHECK_EQUAL(spec.metadata.at("tensorParallelChildBackend"), selectedBackend);
    const auto configured = qwen::parseQwenTpEntryTable(spec.metadata.at("tensorParallelEntries"));
    BOOST_REQUIRE_EQUAL(configured.size(), 7u);
    for (const auto& entry : configured) {
      if (entry.kind != "ATTN") continue;
      std::ifstream file(modelPath.parent_path() / entry.relativeGraphPath, std::ios::binary);
      const std::string data((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
      onnx::ModelProto narrowed;
      BOOST_REQUIRE(narrowed.ParseFromString(data));
      bool sawProjection = false;
      for (const auto& node : narrowed.graph().node()) {
        if (node.name().find("q_proj") == std::string::npos) continue;
        for (const auto& attr : node.attribute()) {
          if (attr.name() != "N") continue;
          sawProjection = true;
          BOOST_CHECK_EQUAL(attr.i(), 64 / degree);
        }
      }
      BOOST_CHECK(sawProjection);
    }
    const auto firstTable = spec.metadata.at("tensorParallelEntries");
    qwen::configureQwenTpRankSpec(spec, 4, ownedSteps, 1024 * 1024,
                                  degree, 0, "later-unit", nullptr, 1);
    BOOST_CHECK_EQUAL(spec.metadata.at("tensorParallelEntries"), firstTable);
    BOOST_CHECK_EQUAL(spec.metadata.at("tensorParallelChildBackend"), selectedBackend);
  }
  // Inspect real registry-created children without requiring a CUDA installation. These
  // material-only steps have no input declarations, so no worker template is claimed here.
  class UnusedChild final : public NativeModelRunner
  {
  public:
    std::map<std::string, TensorBundle>
    run(const RoleExecutionContext&) override
    {
      throw std::logic_error("backend binding probe must not execute");
    }
  };
  RegistryNativeModelRunnerFactory recording;
  std::vector<NativeModelRunnerSpec> children;
  for (const auto* backend : {"onnxruntime", "onnxruntime-cpu", "onnxruntime-cuda"})
    recording.registerBackend(backend, [&children] (const NativeModelRunnerSpec& child) {
      children.push_back(child);
      return std::make_shared<UnusedChild>();
    });
  qwen::registerQwenTensorBackend(recording);
  auto probe = spec;
  probe.metadata.erase("tensorParallelSteps");
  probe.metadata["deviceId"] = "3";
  BOOST_REQUIRE(recording.create(probe));
  BOOST_REQUIRE_EQUAL(children.size(), 7u);
  for (const auto& child : children) {
    BOOST_CHECK_EQUAL(child.backend, "onnxruntime-cuda");
    BOOST_CHECK_EQUAL(child.metadata.at("deviceId"), "3");
    const auto selected = resolveOnnxRuntimeProviderSelection(child,
      {"CPUExecutionProvider", "CUDAExecutionProvider"});
    BOOST_CHECK_EQUAL(selected.selectedProvider, "cuda");
    BOOST_CHECK_EQUAL(selected.deviceId, "3");
    BOOST_CHECK(!selected.usedCpuFallback);
    BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(child, {"CPUExecutionProvider"}),
                      std::runtime_error);
    auto conflict = child;
    conflict.metadata["executionProvider"] = "cpu";
    BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(conflict,
      {"CPUExecutionProvider", "CUDAExecutionProvider"}), std::invalid_argument);
  }
  for (const auto* invalid : {"unknown", "native-qwen-tensor", ""}) {
    auto bad = probe;
    bad.metadata["tensorParallelChildBackend"] = invalid;
    BOOST_CHECK_THROW(recording.create(bad), std::invalid_argument);
  }
  children.clear();
  auto legacy = probe;
  legacy.metadata.erase("tensorParallelChildBackend");
  BOOST_REQUIRE(recording.create(legacy));
  BOOST_REQUIRE_EQUAL(children.size(), 7u);
  for (const auto& child : children) BOOST_CHECK_EQUAL(child.backend, "onnxruntime");
  auto unknown = spec;
  unknown.backend = "unknown";
  BOOST_CHECK_THROW(qwen::configureQwenTpRankSpec(unknown, 4, ownedSteps, 1024 * 1024,
                                                 2, 0, "later-unit", nullptr, 1),
                    std::invalid_argument);
  // Omitting the explicit begin still uses the legacy complete-source contract.
  BOOST_CHECK_THROW(qwen::configureQwenTpRankSpec(spec, 4, ownedSteps, 1024 * 1024,
                                                 2, 0, "later-unit"),
                    std::invalid_argument);

  auto peerSteps = ownedSteps;
  peerSteps.erase(std::remove_if(peerSteps.begin(), peerSteps.end(), [] (const auto& step) {
    return step.artifactEntry.find("/HEAD/") != std::string::npos;
  }), peerSteps.end());
  qwen::configureQwenTpRankSpec(spec, 4, peerSteps, 1024 * 1024,
                                2, 1, "other-unit", nullptr, 1);
  const auto peer = qwen::parseQwenTpEntryTable(spec.metadata.at("tensorParallelEntries"));
  BOOST_REQUIRE_EQUAL(peer.size(), 6u);
  bool checkedPacked = false;
  for (const auto& entry : peer) {
    BOOST_CHECK(entry.kind != "HEAD" && entry.kind != "INIT");
    if (entry.kind != "ATTN" || entry.layer != 1) continue;
    std::ifstream file(modelPath.parent_path() / entry.relativeGraphPath, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    onnx::ModelProto packed;
    BOOST_REQUIRE(packed.ParseFromString(data));
    const std::string weightName = "model.layers.1.attn.q_proj.MatMul.weight_Q4";
    for (const auto& init : packed.graph().initializer()) {
      if (init.name() != weightName) continue;
      for (const auto& original : later.graph().initializer()) {
        if (original.name() != weightName) continue;
        checkedPacked = true;
        BOOST_CHECK_EQUAL(init.dims(0), 32);
        BOOST_CHECK_EQUAL(init.raw_data(),
                          original.raw_data().substr(original.raw_data().size() / 2));
      }
    }
  }
  BOOST_CHECK(checkedPacked);

  const auto rejectGeometry = [&] (const onnx::ModelProto& damaged, const char* expected) {
    std::filesystem::create_directories(root / "bad-geometry");
    NativeModelRunnerSpec bad;
    bad.role = "later-unit";
    bad.path = (root / "bad-geometry" / "model.onnx").string();
    {
      const auto data = damaged.SerializeAsString();
      std::ofstream output(bad.path, std::ios::binary);
      output.write(data.data(), static_cast<std::streamsize>(data.size()));
      BOOST_REQUIRE(output.good());
    }
    BOOST_CHECK_EXCEPTION(qwen::configureQwenTpRankSpec(bad, 4, ownedSteps, 1024 * 1024,
                                                        2, 0, "later-unit", nullptr, 1),
      std::invalid_argument, [&] (const auto& error) {
        return std::string(error.what()).find(expected) != std::string::npos;
      });
  };
  auto inconsistent = later;
  bool changedHeads = false;
  for (auto& node : *inconsistent.mutable_graph()->mutable_node()) {
    if (node.op_type() != "GroupQueryAttention" || changedHeads) continue;
    for (auto& attr : *node.mutable_attribute()) {
      if (attr.name() != "num_heads") continue;
      attr.set_i(8);
      changedHeads = true;
    }
  }
  BOOST_REQUIRE(changedHeads);
  rejectGeometry(inconsistent, "inconsistent attention head counts");
  auto missingProjection = later;
  auto* projections = missingProjection.mutable_graph()->mutable_node();
  bool removedProjection = false;
  for (int index = 0; index < projections->size(); ++index) {
    if (projections->Get(index).name() != "model.layers.2/mlp.up_proj") continue;
    projections->DeleteSubrange(index, 1);
    removedProjection = true;
    break;
  }
  BOOST_REQUIRE(removedProjection);
  rejectGeometry(missingProjection, "does not cover every maintained projection");

  // Requiring a source layer that the region does not own must still fail.
  BOOST_CHECK_THROW(qwen::assembleQwenTpEntryTable(
    root / "wrong-range", "later-unit", regionBytes, 4, unsliced, nullptr, 0, 4),
    std::invalid_argument);
  std::set<std::string> missingMlpNodes;
  for (const auto& entry : entries) {
    if (entry.kind != "MLP" || entry.layer != 2) continue;
    for (const auto index : entry.nodeIndices)
      missingMlpNodes.insert(model.graph().node(static_cast<int>(index)).name());
  }
  BOOST_REQUIRE(!missingMlpNodes.empty());
  auto missingLayer = later;
  auto* nodes = missingLayer.mutable_graph()->mutable_node();
  for (int index = nodes->size() - 1; index >= 0; --index)
    if (missingMlpNodes.count(nodes->Get(index).name()) != 0) nodes->DeleteSubrange(index, 1);
  const auto broken = missingLayer.SerializeAsString();
  const std::vector<std::uint8_t> brokenBytes(broken.begin(), broken.end());
  BOOST_CHECK_THROW(qwen::assembleQwenTpEntryTable(
    root / "missing-mlp", "later-unit", brokenBytes, 4, unsliced, nullptr, 1, 4),
    std::invalid_argument);
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

  HybridScratch workspace("spec242-slice-check");
  const auto& root = workspace.root;
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
  return result;
}

} // namespace

BOOST_AUTO_TEST_CASE(SlicedKernelMatchesTheUnsplitColumns)
{
  const std::string path = hybridFixturePath("four-layer-qwen.onnx");
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
    x[i] = static_cast<float>(static_cast<int>(i % 7) - 3) * 0.25f;

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

BOOST_AUTO_TEST_CASE(AttentionHeadPackingPreservesTokenCoordinates)
{
  std::ifstream input(hybridFixturePath("four-layer-numeric.onnx"), std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromIstream(&input));
  const auto expose = [&] (const std::string& name,
                           const std::vector<std::int64_t>& dimensions) {
    auto* output = model.mutable_graph()->add_output();
    output->set_name(name);
    auto* tensor = output->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dimension : dimensions)
      tensor->mutable_shape()->add_dim()->set_dim_value(dimension);
  };
  expose("model.layers.0/attn.q_proj", {1, 3, 64});
  expose("model.layers.0/attn.q4d", {1, 4, 3, 16});
  const auto wire = model.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-head-coordinates");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, wire.data(), wire.size(), options);
  std::array<std::int64_t, 3> tokens{1, 2, 3};
  const std::array<std::int64_t, 2> dimensions{1, 3};
  auto values = Ort::Value::CreateTensor<std::int64_t>(
    Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
    tokens.data(), tokens.size(), dimensions.data(), dimensions.size());
  const char* inputs[] = {"input_ids"};
  const char* outputs[] = {"model.layers.0/attn.q_proj", "model.layers.0/attn.q4d"};
  auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &values, 1, outputs, 2);
  const auto* projected = result.at(0).GetTensorData<float>();
  const auto* heads = result.at(1).GetTensorData<float>();
  for (std::size_t token = 0; token < 3; ++token) {
    for (std::size_t head = 0; head < 4; ++head) {
      for (std::size_t channel = 0; channel < 16; ++channel) {
        const auto expected = projected[token * 64 + head * 16 + channel];
        BOOST_REQUIRE(std::isfinite(expected));
        BOOST_CHECK_EQUAL(heads[(head * 3 + token) * 16 + channel], expected);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(AssembledAttentionEntryExecutes)
{
  const std::string fixturePath = hybridFixturePath("four-layer-qwen.onnx");
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

  HybridScratch workspace("spec242-entry-run");
  const auto& root = workspace.root;
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
    std::vector<std::vector<float>> inputBuffers;
    inputBuffers.reserve(session.GetInputCount());
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
      inputBuffers.emplace_back(count, 0.05f);
      auto& data = inputBuffers.back();
      values.push_back(Ort::Value::CreateTensor<float>(
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault), data.data(),
        data.size(), dims.data(), dims.size()));
      owned.emplace_back(name.get());
    }
    std::vector<const char*> names;
    for (const auto& name : owned) names.push_back(name.c_str());
    std::vector<const char*> outputNames;
    std::vector<std::string> outputOwned;
    outputOwned.reserve(session.GetOutputCount());
    for (std::size_t index = 0; index < session.GetOutputCount(); ++index) {
      auto name = session.GetOutputNameAllocated(index, allocator);
      outputOwned.emplace_back(name.get());
      outputNames.push_back(outputOwned.back().c_str());
    }
    auto outputs = session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(),
                               values.size(), outputNames.data(), outputNames.size());
    BOOST_REQUIRE(!outputs.empty());
    BOOST_CHECK(outputs.front().GetTensorTypeAndShapeInfo().GetElementCount() > 0);
    for (const auto& output : outputs) {
      const auto info = output.GetTensorTypeAndShapeInfo();
      if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) continue;
      const auto* data = output.GetTensorData<float>();
      for (std::size_t i = 0; i < info.GetElementCount(); ++i)
        BOOST_CHECK(std::isfinite(data[i]));
    }
    ran = true;
  }
  BOOST_CHECK(ran);
}

namespace {

onnx::ModelProto pruneByNames(const onnx::ModelProto& source,
                              const onnx::ModelProto& boundary,
                              const std::set<std::string>& keep)
{
  onnx::ModelProto result;
  result.set_ir_version(source.ir_version());
  for (const auto& opset : source.opset_import()) *result.add_opset_import() = opset;
  auto* graph = result.mutable_graph();
  graph->set_name("spec242-pruned-reference");
  std::set<std::string> wanted;
  for (const auto& value : boundary.graph().input()) wanted.insert(value.name());
  for (const auto& node : source.graph().node()) {
    if (keep.count(node.name()) == 0) continue;
    *graph->add_node() = node;
    for (const auto& input : node.input())
      if (!input.empty()) wanted.insert(input);
  }
  for (const auto& init : source.graph().initializer())
    if (wanted.count(init.name()) != 0) *graph->add_initializer() = init;
  for (const auto& value : boundary.graph().input()) *graph->add_input() = value;
  for (const auto& value : boundary.graph().output()) *graph->add_output() = value;
  return result;
}

} // namespace

/** Write the stage's real artifacts so numeric questions can be asked without rebuilding.
 *
 * The test only insists the files exist; every comparison happens against these files, which
 * keeps the "is the slice right" question away from hand-built graphs - that approach produced
 * contradictory numbers three times.
 */
namespace {

/** Assemble one kind of entry for both ranks, prune the fixture to it, and require the two
 * ranks to recompose the reference. Only real artifacts are involved.
 */
void
verifyKindRecomposes(const std::string& kind, const std::string& label)
{
  const std::string fixturePath = hybridFixturePath("four-layer-qwen.onnx");
  std::ifstream fixtureFile(fixturePath, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(fixtureFile));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(fixtureFile)),
                                        std::istreambuf_iterator<char>());
  onnx::ModelProto fixture;
  BOOST_REQUIRE(fixture.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  std::vector<std::uint8_t> copy = bytes;
  const auto shape = qwen::deriveQwenTpShape(copy, 4);

  qwen::QwenTpRankGeometry geometry;
  geometry.degree = 2;
  geometry.headDim = shape.headDim;
  geometry.queryWidth = shape.queryWidth();
  geometry.kvWidth = shape.kvWidth();
  geometry.hidden = shape.hidden;
  geometry.ffn = shape.ffn;

  HybridScratch workspace("spec242-" + label);
  const auto& root = workspace.root;
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  qwen::registerQwenTensorBackend(factory);
  std::vector<std::shared_ptr<NativeModelRunner>> parents;
  std::vector<qwen::QwenTpEntryContract> contracts;
  onnx::ModelProto entryShapes;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    qwen::QwenTpAssemblyShards shards;
    shards.slicesByLayer = qwen::planQwenTensorSlices(shape, 2, rank, 0, 1);
    geometry.rank = rank;
    shards.geometry = geometry;
    shards.readExternal = [] (std::uint64_t, std::uint64_t) -> std::vector<std::uint8_t> {
      throw std::runtime_error("unexpected sidecar read");
    };
    const auto table = qwen::assembleQwenTpEntryTable(
      root, "S0R" + std::to_string(rank), bytes, 4, shards, nullptr, 0, 1);
    for (const auto& entry : table) {
      if (entry.kind != kind) continue;
      NativeModelRunnerSpec spec;
      spec.role = entry.unitId;
      spec.kind = "onnx";
      spec.backend = qwen::QWEN_TENSOR_RUNNER_BACKEND;
      spec.path = (root / entry.relativeGraphPath).string();
      spec.metadata["tensorParallelEntries"] = qwen::qwenTpEntryTableJson({entry});
      spec.metadata["tensorParallelEntryRoot"] = root.string();
      parents.emplace_back(factory.create(spec));
      BOOST_REQUIRE(dynamic_cast<qwen::NativeQwenTensorRunner*>(parents.back().get()) != nullptr);
      contracts.push_back(entry);
      if (rank == 0U) {
        std::ifstream file(root / entry.relativeGraphPath, std::ios::binary);
        BOOST_REQUIRE(static_cast<bool>(file));
        const std::string raw((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        BOOST_REQUIRE(entryShapes.ParseFromArray(raw.data(), static_cast<int>(raw.size())));
      }
    }
  }
  BOOST_REQUIRE_EQUAL(parents.size(), 2u);

  std::set<std::string> keep;
  for (const auto& node : entryShapes.graph().node()) keep.insert(node.name());
  const auto reference = pruneByNames(fixture, entryShapes, keep);
  const auto referencePath = root / "reference.onnx";
  {
    const std::string serialized = reference.SerializeAsString();
    std::ofstream out(referencePath, std::ios::binary);
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
  }

  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-stage-gate");
  Ort::SessionOptions options;
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<float> x0(1 * 3 * shape.hidden);
  for (std::size_t index = 0; index < x0.size(); ++index)
    x0[index] = static_cast<float>(static_cast<int>((index * 7) % 29) - 14) * 0.01f;
  std::vector<float> mergedInput(x0.size());
  for (std::size_t index = 0; index < mergedInput.size(); ++index)
    mergedInput[index] =
      static_cast<float>(static_cast<int>((index * 3) % 17) - 8) * 0.007f + 0.02f;
  const std::array<std::int64_t, 3> dims{1, 3, static_cast<std::int64_t>(shape.hidden)};
  const auto run = [&] (const std::filesystem::path& path) {
    Ort::Session session(env, path.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<Ort::Value> tensors;
    std::vector<std::string> inputOwned;
    inputOwned.reserve(session.GetInputCount());
    for (std::size_t index = 0; index < session.GetInputCount(); ++index) {
      BOOST_REQUIRE(session.GetInputTypeInfo(index).GetTensorTypeAndShapeInfo().GetElementType() ==
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
      auto name = session.GetInputNameAllocated(index, allocator);
      inputOwned.emplace_back(name.get());
      const std::string graphName = name.get();
      BOOST_REQUIRE(graphName == "x0" ||
                    (kind == "MLP" && graphName == "model.layers.0/attn.o_proj"));
      auto& data = graphName == "x0" ? x0 : mergedInput;
      tensors.push_back(Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                                       dims.data(), dims.size()));
    }
    std::vector<const char*> names;
    for (const auto& name : inputOwned) names.push_back(name.c_str());
    std::vector<const char*> outputs;
    std::vector<std::string> owned;
    owned.reserve(session.GetOutputCount());
    for (std::size_t index = 0; index < session.GetOutputCount(); ++index) {
      auto name = session.GetOutputNameAllocated(index, allocator);
      owned.emplace_back(name.get());
      outputs.push_back(owned.back().c_str());
    }
    auto values = session.Run(Ort::RunOptions{nullptr}, names.data(), tensors.data(),
                              tensors.size(), outputs.data(), outputs.size());
    std::vector<std::pair<std::string, std::vector<float>>> result;
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto info = values[index].GetTensorTypeAndShapeInfo();
      if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) continue;
      const auto count = info.GetElementCount();
      const float* data = values[index].GetTensorData<float>();
      result.emplace_back(owned[index], std::vector<float>(data, data + count));
    }
    return result;
  };

  const auto fullRun = run(referencePath);
  const std::string projection = "model.layers.0/" +
    std::string(kind == "ATTN" ? "attn.o_proj" : "mlp.down_proj");
  std::vector<float> expected;
  for (const auto& [name, values] : fullRun)
    if (name == projection) expected = values;
  BOOST_REQUIRE(!expected.empty());

  const auto runParent = [&] (std::size_t rank) {
    const auto& entry = contracts.at(rank);
    BOOST_CHECK_EQUAL(entry.outputSlotByGraphName.at(projection), "partial");
    BOOST_CHECK_EQUAL(entry.inputSlotByGraphName.at("x0"),
                      kind == "ATTN" ? "x" : "residual");
    if (kind == "MLP")
      BOOST_CHECK_EQUAL(entry.inputSlotByGraphName.at("model.layers.0/attn.o_proj"), "merged");
    if (kind == "MLP") {
      std::ifstream file(root / entry.relativeGraphPath, std::ios::binary);
      const std::string data((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
      onnx::ModelProto material;
      BOOST_REQUIRE(material.ParseFromString(data));
      std::size_t pointwise = 0;
      bool fullResidual = false;
      for (const auto& value : material.graph().value_info()) {
        if (value.name() == "model.layers.0/mlp.sigmoid" ||
            value.name() == "model.layers.0/mlp.silu" ||
            value.name() == "model.layers.0/mlp.act") {
          ++pointwise;
          BOOST_REQUIRE_EQUAL(value.type().tensor_type().shape().dim_size(), 3);
          BOOST_CHECK_EQUAL(value.type().tensor_type().shape().dim(2).dim_value(), shape.ffn / 2);
        }
        if (value.name() == "model.layers.0/attn.residual") {
          fullResidual = true;
          BOOST_CHECK_EQUAL(value.type().tensor_type().shape().dim(2).dim_value(), shape.hidden);
        }
      }
      BOOST_CHECK_EQUAL(pointwise, 3u);
      BOOST_CHECK(fullResidual);
    }
    NativeExecutionStepSpec step;
    step.stepId = entry.entryId;
    step.unitId = entry.unitId;
    step.operation = "CALL_SUBGRAPH";
    step.artifactEntry = entry.entryId;
    RoleExecutionContext context;
    context.role = entry.unitId;
    context.activeStep = std::make_shared<const NativeExecutionStepSpec>(step);
    context.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
      std::vector<NativeExecutionStepSpec>{step});
    const std::vector<std::string> slots = kind == "ATTN"
      ? std::vector<std::string>{"x"} : std::vector<std::string>{"merged", "residual"};
    for (const auto& slot : slots) {
      NamedTensor tensor;
      tensor.name = slot;
      tensor.elementType = TensorElementType::Float32;
      tensor.shape.assign(dims.begin(), dims.end());
      const auto& data = slot == "merged" ? mergedInput : x0;
      tensor.payload.resize(data.size() * sizeof(float));
      std::memcpy(tensor.payload.data(), data.data(), tensor.payload.size());
      context.stepInputsBySlot.emplace(slot, makeEncodedTensorBundle(slot, {tensor}));
    }
    const auto outputs = parents.at(rank)->run(context);
    BOOST_REQUIRE(outputs.count("partial") == 1);
    const std::string residualSlot = kind == "ATTN" ? "residual" : "post_residual";
    BOOST_REQUIRE(outputs.count(residualSlot) == 1);
    const auto residual = decodeTensorBundleViews(outputs.at(residualSlot));
    BOOST_REQUIRE_EQUAL(residual.size(), 1u);
    BOOST_REQUIRE_EQUAL(residual.front().payload.size(), x0.size() * sizeof(float));
    std::vector<float> actualResidual(x0.size());
    std::memcpy(actualResidual.data(), residual.front().payload.data(),
                  residual.front().payload.size());
    for (std::size_t index = 0; index < x0.size(); ++index) {
      const float expectedResidual = x0[index] + (kind == "ATTN" ? 0 : mergedInput[index]);
      BOOST_REQUIRE(std::isfinite(actualResidual[index]));
      BOOST_CHECK_SMALL(actualResidual[index] - expectedResidual, 1e-6f);
    }
    return outputs.at("partial");
  };
  const auto left = runParent(0);
  const auto right = runParent(1);
  // Use the actual worker arithmetic implementation, without residual compensation.
  const auto merged = sumTensorBundleViews("merged",
    {decodeTensorBundleViews(left), decodeTensorBundleViews(right)});
  const auto views = decodeTensorBundleViews(merged);
  BOOST_REQUIRE_EQUAL(views.size(), 1u);
  BOOST_CHECK_EQUAL(views.front().name, projection);
  BOOST_REQUIRE_EQUAL(views.front().payload.size(), expected.size() * sizeof(float));
  std::vector<float> composed(expected.size());
  std::memcpy(composed.data(), views.front().payload.data(), views.front().payload.size());

  float worst = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    BOOST_REQUIRE(std::isfinite(composed[index]) && std::isfinite(expected[index]));
    const auto error = std::abs(composed[index] - expected[index]);
    worst = std::max(worst, error);
    BOOST_CHECK_LE(error, 1e-5f + 1e-4f * std::abs(expected[index]));
  }
  BOOST_TEST_MESSAGE(label << ": worst |composed - reference| = " << worst);
}

} // namespace

BOOST_AUTO_TEST_CASE(StageEntriesRecomposeTheReference)
{
  verifyKindRecomposes("ATTN", "attn-gate");
  verifyKindRecomposes("MLP", "mlp-gate");
}

namespace {

class FixtureStateObserver : public OnnxKvStateObserver
{
public:
  bool wants(const RoleExecutionContext& context) const override
  {
    return context.sessionId == "three-worker-prefill" ||
      context.sessionId.rfind("three-worker-prefill/", 0) == 0;
  }

  void record(const RoleExecutionContext& context,
              std::vector<OnnxKvStateSample> samples) override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& sample : samples) {
      if (!m_samples.emplace(std::make_tuple(context.role, context.inferenceEpoch,
                                             sample.inputName), std::move(sample)).second)
        throw std::runtime_error("fixture observed a duplicate state transition");
    }
  }

  OnnxKvStateSample sample(const std::string& role, std::uint64_t epoch,
                           const std::string& input) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_samples.at(std::make_tuple(role, epoch, input));
  }

  void clear()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::tuple<std::string, std::uint64_t, std::string>, OnnxKvStateSample> m_samples;
};

class CompleteFixtureStateObserver final : public FixtureStateObserver,
                                           public OnnxCompleteKvStateObserver
{
public:
  bool wantsCompleteSmallState(const RoleExecutionContext& context) const override
  {
    return wants(context);
  }
};

enum class CoordinatorFault
{
  NONE,
  MISSING,
  WRONG_PRODUCER,
  PUBLICATION_FAILURE,
  MISSING_CONTRIBUTION,
};

auto
makeCompleteSourceReference(const std::string& source, bool externalControls = false)
{
  // Recompute the entire prefix with empty past for every epoch. This oracle never uses
  // splitter-selected nodes, sharded state or the worker's preceding outputs.
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-complete-source");
  Ort::SessionOptions options;
  Ort::Session reference(env, source.c_str(), options);
  BOOST_TEST_MESSAGE("native ORT version = " << OrtGetApiBase()->GetVersionString());
  Ort::AllocatorWithDefaultOptions allocator;
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  for (std::size_t i = 0; i < reference.GetInputCount(); ++i) {
    auto name = reference.GetInputNameAllocated(i, allocator);
    inputNames.emplace_back(name.get());
  }
  for (std::size_t i = 0; i < reference.GetOutputCount(); ++i) {
    auto name = reference.GetOutputNameAllocated(i, allocator);
    outputNames.emplace_back(name.get());
  }
  return [reference = std::move(reference), inputNames = std::move(inputNames),
          outputNames = std::move(outputNames), externalControls]
         (std::vector<std::int64_t> prefix, std::int32_t delta = 3,
          std::int64_t positionOffset = 0) mutable {
    Ort::AllocatorWithDefaultOptions allocator;
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> inputs;
    std::vector<std::int64_t> positions(prefix.size());
    for (std::size_t i = 0; i < positions.size(); ++i)
      positions[i] = static_cast<std::int64_t>(i) + positionOffset;
    for (std::size_t i = 0; i < inputNames.size(); ++i) {
      if (inputNames[i] == "input_ids") {
        const std::array<std::int64_t, 2> dims{1, static_cast<std::int64_t>(prefix.size())};
        inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, prefix.data(),
          prefix.size(), dims.data(), dims.size()));
      }
      else if (externalControls && inputNames[i] == "control.delta") {
        inputs.push_back(Ort::Value::CreateTensor<std::int32_t>(memory, &delta, 1,
                                                                nullptr, 0));
      }
      else if (externalControls && inputNames[i] == "control.positions") {
        const std::array<std::int64_t, 2> dims{1,
          static_cast<std::int64_t>(positions.size())};
        inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, positions.data(),
          positions.size(), dims.data(), dims.size()));
      }
      else {
        BOOST_REQUIRE(inputNames[i].find("past_key_values.") == 0);
        auto type = reference.GetInputTypeInfo(i);
        auto info = type.GetTensorTypeAndShapeInfo();
        auto dims = info.GetShape();
        BOOST_REQUIRE_EQUAL(dims.size(), 4u);
        dims[2] = 0;
        inputs.push_back(Ort::Value::CreateTensor<float>(allocator, dims.data(), dims.size()));
      }
    }
    std::vector<const char*> in;
    std::vector<const char*> out;
    for (const auto& name : inputNames) in.push_back(name.c_str());
    for (const auto& name : outputNames) out.push_back(name.c_str());
    auto values = reference.Run(Ort::RunOptions{nullptr}, in.data(), inputs.data(),
                                inputs.size(), out.data(), out.size());
    std::map<std::string, NamedTensor> result;
    for (std::size_t i = 0; i < values.size(); ++i) {
      auto info = values[i].GetTensorTypeAndShapeInfo();
      BOOST_REQUIRE(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
      NamedTensor tensor;
      tensor.name = outputNames[i];
      tensor.elementType = TensorElementType::Float32;
      tensor.shape = info.GetShape();
      tensor.payload.resize(info.GetElementCount() * sizeof(float));
      std::memcpy(tensor.payload.data(), values[i].GetTensorData<float>(), tensor.payload.size());
      result.emplace(tensor.name, std::move(tensor));
    }
    return result;
  };
}

void
verifyHybridExecution(const std::string& source, const NativeExecutionPlan& basePlan,
                      const qwen::QwenTpShape& shape,
                      const std::vector<std::vector<qwen::QwenTpUnit>>& units,
                      const std::string& egress,
                      const std::map<std::string, NativeModelRunnerSpec>& specs,
                      bool dynamic, const std::string& label, bool externalControls = false,
                      bool coordinator = false,
                      CoordinatorFault fault = CoordinatorFault::NONE,
                      bool finalizeCheckpoint = false,
                      const std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>&
                        expectedKvRanges = {})
{
  const std::size_t generationTokens = finalizeCheckpoint ? 32 : 2;
  const auto generationEpochs = generationTokens + (finalizeCheckpoint ? 1 : 0);
  const std::string baseSessionId = "three-worker-prefill/" + label +
    (dynamic ? "/decode" : "/prefill");
  auto fullReference = makeCompleteSourceReference(source, externalControls);

  auto observer = std::make_shared<CompleteFixtureStateObserver>();
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory, nullptr, observer);
  qwen::registerQwenTensorBackend(factory);
  NativeProviderAssignment assignment;
  BOOST_REQUIRE_EQUAL(specs.size(), basePlan.roles.size());
  for (const auto& role : basePlan.roles)
    assignment.providerByRole.emplace(role, specs.at(role).metadata.at("providerIdentity"));
  NativeGenerationExecutionContractV1 stateContract;
  if (dynamic) {
    stateContract.enabled = true;
    stateContract.mode = "TOKEN_STREAMING";
    qwen::bindQwenTpGenerationState(basePlan, stateContract);
    BOOST_REQUIRE_EQUAL(stateContract.stateOutputNames.size(), 8u);
  }
  std::map<std::string, RoleSpec> roleSpecs;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> runners;
  for (const auto& role : basePlan.roles) {
    auto roleSpec = roleSpecFor(basePlan, role, baseSessionId, assignment, "");
    std::set<std::string> ownedState;
    if (const auto& whole = specs.at(role).entryContract) {
      BOOST_CHECK_EQUAL(whole->unitId, role);
      for (const auto& state : whole->stateSuccessorByInputGraphName)
        ownedState.insert(state.second);
    }
    else {
      const auto entries = qwen::parseQwenTpEntryTable(
        specs.at(role).metadata.at("tensorParallelEntries"));
      for (const auto& entry : entries) {
        BOOST_REQUIRE_EQUAL(entry.unitId, role);
        ownedState.insert(entry.stateOutputGraphNames.begin(), entry.stateOutputGraphNames.end());
      }
    }
    for (std::size_t i = 0; i < stateContract.stateOutputNames.size(); ++i) {
      if (ownedState.count(stateContract.stateOutputNames[i]) == 0) continue;
      roleSpec.stateInputNames.push_back(stateContract.stateInputNames[i]);
      roleSpec.stateOutputNames.push_back(stateContract.stateOutputNames[i]);
    }
    auto spec = specs.at(role);
    spec.metadata["providerBootId"] = "none";
    runners.emplace(role, factory.create(spec));
    roleSpecs.emplace(role, std::move(roleSpec));
  }

  std::map<std::string, std::unique_ptr<NativeProviderRuntime>> coordinatedRuntimes;
  if (coordinator) {
    for (const auto& role : basePlan.roles)
      coordinatedRuntimes.emplace(role, std::make_unique<NativeProviderRuntime>(1));
  }
  std::map<std::string, ConversationStateBinding> parentStates;
  std::vector<std::int64_t> prefix;
  const auto runTurn = [&] (bool resumed) {
    const auto sessionId = resumed ? baseSessionId + "/next" : baseSessionId;
    const std::string requestId = resumed ? "/fixture/coordinator-next" :
                                            "/fixture/coordinator-request";
    const std::vector<std::int64_t> initialTokens = resumed ? std::vector<std::int64_t>{4} :
                                                             std::vector<std::int64_t>{1, 2, 3};
    const auto prefixLengthAtStart = prefix.size();
    auto plan = basePlan;
    std::map<std::size_t, std::map<std::string, ProviderRoleResult>> coordinatedResults;
    std::vector<std::int64_t> sampledTokens;
    if (coordinator) {
      BOOST_REQUIRE(dynamic && !externalControls);
      const auto topology = nativeGenerationFeedbackTopology(plan, egress, "input_ids");
      bindNativeGenerationFeedback(plan, topology, "input_ids", requestId, 1,
                                   assignment.providerByRole);
      auto bus = std::make_shared<tests::PartialBus>();
      tests::CancellableBusDependencyIo::PublicationMutation mutation;
      if (fault != CoordinatorFault::NONE) {
        mutation = [&, fault] (const DependencyEdge& edge, TensorBundle& value) {
          if (fault == CoordinatorFault::MISSING_CONTRIBUTION) {
            return edge.operationKind != "TENSOR_PARTIAL" || edge.consumerRole != egress ||
                   edge.producerRole != basePlan.stageLayout.front().unitRoleIds.front();
          }
          if (edge.operationKind != "ACTIVATION" && edge.operationKind != "PIPELINE") return true;
          auto lineage = extractGenerationEpochLineage(value);
          if (!lineage)
            throw std::runtime_error("fixture could not find the Producer's original lineage");
          if (fault == CoordinatorFault::PUBLICATION_FAILURE)
            throw std::runtime_error("injected activation publication failure");
          value = stripGenerationEpochLineage(value);
          if (fault == CoordinatorFault::WRONG_PRODUCER) {
            lineage->producerRole = "foreign/Tensor/Rank/0";
            value = attachGenerationEpochLineage(value, *lineage);
          }
          return true;
        };
      }
      auto io = std::make_shared<tests::CancellableBusDependencyIo>(bus, std::move(mutation));
      auto cancelled = std::make_shared<std::atomic<bool>>(false);
      std::mutex observedMutex;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      std::map<std::string, std::future<NativeEpochCoordinatorResult>> futures;
      const auto stopFailure = std::make_exception_ptr(
        std::runtime_error("bounded Coordinator fixture stopped"));
      struct Drain
      {
        std::shared_ptr<tests::PartialBus> bus;
        std::shared_ptr<std::atomic<bool>> cancelled;
        decltype(futures)& pending;
        std::exception_ptr failure;
        ~Drain()
        {
          cancelled->store(true);
          bus->cancel(failure);
          for (auto& item : pending)
            if (item.second.valid()) item.second.wait();
        }
      } drain{bus, cancelled, futures, stopFailure};
      const auto seed = nativePlanningDigest("spec242-coordinator-trusted-fixture");
      for (const auto& role : plan.roles) {
        NativeEpochCoordinatorConfig config{*coordinatedRuntimes.at(role), plan, assignment, io};
        config.sessionId = sessionId;
        config.requestId = requestId;
        config.role = role;
        config.attemptEpoch = 1;
        config.streamEpoch = 1;
        config.localProvider = assignment.providerByRole.at(role);
        config.lineagePlanDigest = seed;
        config.positionPolicyDigest = seed;
        config.samplingDigest = seed;
        config.maxEpochs = generationTokens;
        config.checkpointFinalize = finalizeCheckpoint;
        config.eosTokenIds = {63};
        config.terminalRole = role == egress;
        config.stateInputNames = roleSpecs.at(role).stateInputNames;
        config.stateOutputNames = roleSpecs.at(role).stateOutputNames;
        // This unit test supplies trusted identity values through the public config boundary;
        // it does not reproduce the Provider handler's grant/identity admission algorithm.
        DecodeStateIdentityV1 identity;
        identity.modelDigest = identity.graphSemanticDigest = identity.artifactDigest = seed;
        identity.adapterDigest = identity.tokenizerDigest = identity.runnerDigest = seed;
        identity.roleSplitDigest = identity.layoutDigest = identity.stateSchemaDigest = seed;
        identity.runtimeAbiDigest = identity.securityDomainDigest = seed;
        identity.roleName = role;
        identity.precision = "fp32";
        identity.providerIdentity = config.localProvider;
        identity.providerBootId = "none";
        identity.cacheEpoch = 1;
        identity.requestId = config.requestId;
        identity.attemptEpoch = 1;
        identity.generationId = resumed ? "spec242-next-generation" : "spec242-first-generation";
        identity.stateComponentDigests.assign(config.stateInputNames.size(), seed);
        for (const auto& stage : units)
          for (const auto& unit : stage)
            if (unit.roleId == role) {
              identity.layerBegin = unit.layerBegin;
              identity.layerEnd = unit.layerEnd;
            }
        config.stateIdentityTemplate = std::move(identity);
        if (resumed) {
          config.conversationStateBinding = parentStates.at(role);
          config.conversationStateLookupNowMs = 200;
          config.firstEpochConversationUse = config.runtime.acquireConversationUse(
            *config.conversationStateBinding, config.conversationStateLookupNowMs);
          BOOST_REQUIRE(config.firstEpochConversationUse);
        }
        else {
          auto runner = runners.at(role);
          config.prepareRunner = [runner] { return runner; };
        }
        config.stopCheck = [cancelled, deadline] () -> std::optional<NativeEpochStopReason> {
          if (cancelled->load()) return NativeEpochStopReason::Cancelled;
          if (std::chrono::steady_clock::now() >= deadline) return NativeEpochStopReason::Deadline;
          return std::nullopt;
        };
        if (std::find(topology.consumerRoles.begin(), topology.consumerRoles.end(), role) !=
            topology.consumerRoles.end()) {
          NamedTensor prompt;
          prompt.name = "input_ids";
          prompt.elementType = TensorElementType::Int64;
          prompt.shape = {1, static_cast<std::int64_t>(initialTokens.size())};
          prompt.payload.resize(initialTokens.size() * sizeof(std::int64_t));
          std::memcpy(prompt.payload.data(), initialTokens.data(), prompt.payload.size());
          config.initialInputs.emplace("input_ids", makeEncodedTensorBundle("input_ids", {prompt}));
        }
        config.resultObserver = [&] (const auto& executed, const auto& result) {
          std::lock_guard<std::mutex> lock(observedMutex);
          if (!coordinatedResults[executed.inferenceEpoch].emplace(executed.role, result).second)
            throw std::runtime_error("Coordinator fixture repeated a role epoch");
        };
        if (role == egress) {
          config.eventSink = [&] (const auto& bytes) {
            const auto event = nativeParseJson(std::string(bytes.begin(), bytes.end()));
            std::lock_guard<std::mutex> lock(observedMutex);
            sampledTokens.push_back(event.at("tokenId").template get<std::int64_t>());
            return true;
          };
        }
        futures.emplace(role, std::async(std::launch::async,
          [bus, cancelled, config = std::move(config)] () mutable {
            try {
              return runNativeEpochCoordinator(std::move(config));
            }
            catch (...) {
              cancelled->store(true);
              bus->cancel(std::current_exception());
              throw;
            }
          }));
      }
      std::map<std::string, NativeEpochCoordinatorResult> results;
      std::ostringstream failures;
      for (auto& item : futures) {
        if (item.second.wait_until(deadline) != std::future_status::ready) {
          cancelled->store(true);
          bus->cancel(stopFailure);
        }
        try {
          results.emplace(item.first, item.second.get());
        }
        catch (const std::exception& error) {
          failures << item.first << ": " << error.what() << "; ";
        }
      }
      if (fault != CoordinatorFault::NONE) {
        BOOST_REQUIRE_MESSAGE(!failures.str().empty(), "corrupt lineage was accepted");
        const auto expected = fault == CoordinatorFault::PUBLICATION_FAILURE
          ? "injected activation publication failure" : "GENERATION_EPOCH_LINEAGE_MISMATCH";
        if (fault != CoordinatorFault::MISSING_CONTRIBUTION)
          BOOST_CHECK_MESSAGE(failures.str().find(expected) != std::string::npos, failures.str());
        BOOST_CHECK(sampledTokens.empty());
        for (const auto& epoch : coordinatedResults) {
          BOOST_CHECK_EQUAL(epoch.first, 0u);
          BOOST_CHECK(epoch.second.count(egress) == 0);
        }
        for (const auto& item : runners)
          BOOST_CHECK_MESSAGE(!item.second->stateHandleSnapshot(sessionId),
                              "retained adapter state for " << item.first);
        for (const auto& item : coordinatedRuntimes) {
          const auto state = item.second->decodeStateSnapshot();
          BOOST_CHECK_EQUAL(state.entries, 0u);
          BOOST_CHECK_EQUAL(state.pinnedEntries, 0u);
          BOOST_CHECK_EQUAL(state.candidates, 0u);
        }
        BOOST_TEST_MESSAGE(label << " rejected Coordinator fault " << static_cast<int>(fault));
        return;
      }
      BOOST_REQUIRE_MESSAGE(failures.str().empty(), label << " Coordinator: " << failures.str());
      BOOST_REQUIRE_EQUAL(sampledTokens.size(), generationTokens);
      BOOST_REQUIRE_EQUAL(coordinatedResults.size(), generationEpochs);
      BOOST_TEST_MESSAGE(label << " resumed=" << resumed << " sampled token IDs="
                              << nativeCanonicalJson(NativeJson(sampledTokens)));
      for (const auto& item : results) {
        BOOST_CHECK_EQUAL(item.second.epochsExecuted, generationEpochs);
        BOOST_CHECK_EQUAL(item.second.eventsPublished,
                          item.first == egress ? generationTokens : 0u);
        if (finalizeCheckpoint) {
          BOOST_REQUIRE(item.second.finalizedRole);
          const auto& state = item.second.finalizedRole->candidateDecodeStateIdentity;
          BOOST_REQUIRE(state);
          BOOST_CHECK_EQUAL(state->prefixTokenCount,
                            prefixLengthAtStart + initialTokens.size() + generationTokens);
          BOOST_CHECK_EQUAL(item.second.finalizedRole->inferenceEpoch, generationTokens);
          auto& runtime = *coordinatedRuntimes.at(item.first);
          if (resumed) {
            const auto& observations = item.second.cacheObservations;
            BOOST_REQUIRE(!observations.empty());
            const auto& first = observations.front();
            BOOST_CHECK_EQUAL(first.inferenceEpoch, 0u);
            BOOST_CHECK(first.conversationStateHit);
            BOOST_CHECK_EQUAL(first.representedPrefixTokenCount,
                              prefixLengthAtStart + initialTokens.size());
            BOOST_CHECK_EQUAL(first.prefixWorkAvoided, prefixLengthAtStart);
            if (std::find(topology.consumerRoles.begin(), topology.consumerRoles.end(),
                          item.first) != topology.consumerRoles.end())
              BOOST_CHECK_EQUAL(first.actualNewInputExtent, initialTokens.size());
          }
          else {
            ConversationStateBinding binding;
            binding.conversationId = "spec242-conversation";
            binding.contextEpoch = 1;
            binding.serviceName = "/fixture/hybrid-inference";
            binding.planRoleMapDigest = seed;
            binding.receiptDigest = nativePlanningDigest(item.first);
            binding.expiresAtMs = 10000;
            binding.identity = *state;
            BOOST_REQUIRE(runtime.stageDecodeStatePromotion(
              sessionId, *item.second.finalizedRole, binding, 100));
            const auto checkpointDigest = nativePlanningDigest("spec242-first-checkpoint");
            BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(binding, checkpointDigest));
            binding.checkpointDigest = checkpointDigest;
            parentStates.emplace(item.first, std::move(binding));
          }
        }
      }
    }

    // Model independent Provider ownership rather than sharing one wait pool
    // across roles whose dependencies can unblock each other.
    std::map<std::string, std::unique_ptr<ProviderRoleWorker>> workers;
    if (!coordinator) {
      for (const auto& role : plan.roles)
        workers.emplace(role, std::make_unique<ProviderRoleWorker>(
          2, 2, 16, std::chrono::seconds(5)));
    }
    std::set<std::string> publicationNames;
    float worstLogits = 0;
    double worstState = 0;
    const std::size_t epochs = coordinator ? generationEpochs : dynamic ? 3 : 1;
    for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
      const std::vector<std::int64_t> tokens = epoch == 0
        ? initialTokens
        : std::vector<std::int64_t>{coordinator ? sampledTokens.at(epoch - 1)
                                                : static_cast<std::int64_t>(3 + epoch)};
      prefix.insert(prefix.end(), tokens.begin(), tokens.end());
      const auto expected = fullReference(prefix);
      if (externalControls && epoch == 0) {
        for (const auto& altered : {fullReference(prefix, 0), fullReference(prefix, 3, 7)}) {
          const auto& original = expected.at("logits").payload;
          const auto& changed = altered.at("logits").payload;
          BOOST_REQUIRE_EQUAL(original.size(), changed.size());
          float difference = 0;
          for (std::size_t offset = 0; offset < original.size(); offset += sizeof(float)) {
            float left;
            float right;
            std::memcpy(&left, original.data() + offset, sizeof(left));
            std::memcpy(&right, changed.data() + offset, sizeof(right));
            difference = std::max(difference, std::abs(left - right));
          }
          BOOST_CHECK_GT(difference, 1e-6f);
        }
      }
      std::map<std::string, ProviderRoleResult> results;
      if (coordinator) {
        results = coordinatedResults.at(epoch);
      }
      else {
        auto bus = std::make_shared<tests::PartialBus>();
        std::map<std::string, std::future<ProviderRoleResult>> futures;
        std::set<std::string> epochPublications;
        for (const auto& role : plan.roles) {
          auto roleSpec = roleSpecFor(plan, role, sessionId, assignment, "", epoch);
          roleSpec.stateInputNames = roleSpecs.at(role).stateInputNames;
          roleSpec.stateOutputNames = roleSpecs.at(role).stateOutputNames;
          roleSpec.streamingStateExecution = dynamic;
          roleSpec.inferenceEpoch = epoch;
          for (const auto& edge : roleSpec.outputs) {
            BOOST_CHECK(edge.plannedDataName.find('{') == std::string::npos);
            BOOST_CHECK(edge.plannedDataName.find(edge.producerProvider + "/") == 0);
            BOOST_CHECK(publicationNames.insert(edge.plannedDataName).second);
            BOOST_CHECK(epochPublications.insert(edge.plannedDataName).second);
          }
          NamedTensor ids;
          ids.name = "input_ids";
          ids.elementType = TensorElementType::Int64;
          ids.shape = {1, static_cast<std::int64_t>(tokens.size())};
          ids.payload.resize(tokens.size() * sizeof(std::int64_t));
          std::memcpy(ids.payload.data(), tokens.data(), ids.payload.size());
          auto io = std::make_shared<tests::BusDependencyIo>(bus);
          auto runner = runners.at(role);
          std::map<std::string, TensorBundle> epochInputs{
            {"input_ids", makeEncodedTensorBundle("input_ids", {ids})}};
          if (externalControls) {
            NamedTensor delta;
            delta.name = "control.delta";
            delta.elementType = TensorElementType::Int32;
            const std::int32_t value = 3;
            delta.payload.resize(sizeof(value));
            std::memcpy(delta.payload.data(), &value, sizeof(value));
            NamedTensor positions;
            positions.name = "control.positions";
            positions.elementType = TensorElementType::Int64;
            positions.shape = ids.shape;
            positions.payload.resize(tokens.size() * sizeof(std::int64_t));
            for (std::size_t i = 0; i < tokens.size(); ++i) {
              const auto position = static_cast<std::int64_t>(prefix.size() - tokens.size() + i);
              std::memcpy(positions.payload.data() + i * sizeof(position), &position,
                          sizeof(position));
            }
            epochInputs.emplace(delta.name, makeEncodedTensorBundle(delta.name, {delta}));
            epochInputs.emplace(positions.name,
                                makeEncodedTensorBundle(positions.name, {positions}));
          }
          futures.emplace(role, workers.at(role)->executePreparedAsync(sessionId, std::move(roleSpec),
            io, [runner] { return runner; }, std::move(epochInputs)));
        }
        std::ostringstream failures;
        for (auto& [role, future] : futures) {
          if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            failures << role << ": worker completion timed out; ";
            bus->cancel(std::make_exception_ptr(
              std::runtime_error("fixture worker completion timed out")));
          }
          try {
            results.emplace(role, future.get());
          }
          catch (const std::exception& error) {
            failures << role << ": " << error.what() << "; ";
          }
        }
        BOOST_REQUIRE_MESSAGE(failures.str().empty(),
          label << " epoch=" << epoch << " worker failures: " << failures.str());
        BOOST_CHECK_EQUAL(bus->publishedCount(), epochPublications.size());
      }
      BOOST_REQUIRE_EQUAL(results.size(), plan.roles.size());
      BOOST_REQUIRE(results.at(egress).outputsByScope.count("logits") == 1);
      const auto actual = decodeTensorBundleViews(results.at(egress).outputsByScope.at("logits"));
      BOOST_REQUIRE_EQUAL(actual.size(), 1u);
      BOOST_CHECK_EQUAL(actual.front().name, "logits");
      BOOST_CHECK(actual.front().elementType == TensorElementType::Float32);
      BOOST_CHECK(actual.front().shape ==
        std::vector<std::int64_t>({1, static_cast<std::int64_t>(tokens.size()),
                                   static_cast<std::int64_t>(shape.vocab)}));
      const auto count = tokens.size() * shape.vocab;
      BOOST_REQUIRE_EQUAL(actual.front().payload.size(), count * sizeof(float));
      std::vector<float> values(count);
      std::memcpy(values.data(), actual.front().payload.data(), actual.front().payload.size());
      const auto& expectedLogits = expected.at("logits");
      const auto offset = (prefix.size() - tokens.size()) * shape.vocab;
      std::vector<float> referenceLogits(prefix.size() * shape.vocab);
      BOOST_REQUIRE_EQUAL(expectedLogits.payload.size(), referenceLogits.size() * sizeof(float));
      std::memcpy(referenceLogits.data(), expectedLogits.payload.data(), expectedLogits.payload.size());
      if (coordinator && epoch < generationTokens) {
        const auto last = referenceLogits.end() - static_cast<std::ptrdiff_t>(shape.vocab);
        const auto wanted = std::distance(last, std::max_element(last, referenceLogits.end()));
        BOOST_CHECK_EQUAL(sampledTokens.at(epoch), wanted);
      }
      for (std::size_t index = 0; index < values.size(); ++index) {
        const auto wanted = referenceLogits.at(offset + index);
        BOOST_REQUIRE(std::isfinite(values[index]) && std::isfinite(wanted));
        const auto error = std::abs(values[index] - wanted);
        worstLogits = std::max(worstLogits, error);
        BOOST_CHECK_LE(error, 1e-5f + 1e-4f * std::abs(wanted));
      }
      if (!dynamic) continue;
      for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
        const auto stage = std::find_if(units.begin(), units.end(), [&] (const auto& members) {
          return !members.empty() && members.front().layerBegin <= layer &&
            layer < members.front().layerEnd;
        });
        BOOST_REQUIRE(stage != units.end());
        const auto degree = stage->size();
        for (const auto* kind : {"key", "value"}) {
          const auto suffix = std::to_string(layer) + "." + kind;
          const auto& whole = expected.at("present." + suffix);
          const auto total = shape.kvHeads * prefix.size() * shape.headDim;
          BOOST_REQUIRE(whole.shape == std::vector<std::int64_t>(
            {1, static_cast<std::int64_t>(shape.kvHeads), static_cast<std::int64_t>(prefix.size()),
             static_cast<std::int64_t>(shape.headDim)}));
          BOOST_REQUIRE_EQUAL(whole.payload.size(), total * sizeof(float));
          std::vector<float> referenceState(total);
          std::memcpy(referenceState.data(), whole.payload.data(), whole.payload.size());
          for (const auto& unit : *stage) {
            const auto sample = observer->sample(unit.roleId, epoch, "past_key_values." + suffix);
            const auto range = expectedKvRanges.find(unit.roleId);
            const auto headCount = range == expectedKvRanges.end() ? shape.kvHeads / degree
              : range->second.second - range->second.first;
            const auto headBegin = range == expectedKvRanges.end() ? unit.rank * headCount
              : range->second.first;
            const auto elementsPerHead = prefix.size() * shape.headDim;
            BOOST_CHECK_EQUAL(sample.elementType, "float32");
            BOOST_REQUIRE(sample.shape == std::vector<std::int64_t>(
              {1, static_cast<std::int64_t>(headCount),
               static_cast<std::int64_t>(prefix.size()), static_cast<std::int64_t>(shape.headDim)}));
            BOOST_REQUIRE_EQUAL(sample.values.size(), headCount * elementsPerHead);
            BOOST_REQUIRE_EQUAL(sample.indices.size(), sample.values.size());
            for (std::size_t i = 0; i < sample.values.size(); ++i) {
              BOOST_REQUIRE_EQUAL(sample.indices[i], i);
              const auto wanted = referenceState.at(headBegin * elementsPerHead + i);
              BOOST_REQUIRE(std::isfinite(sample.values[i]) && std::isfinite(wanted));
              const auto error = std::abs(sample.values[i] - wanted);
              worstState = std::max(worstState, error);
              BOOST_CHECK_LE(error, 1e-5 + 1e-4 * std::abs(wanted));
            }
          }
        }
      }
      if (!coordinator) {
        for (const auto& [role, runner] : runners) {
          const auto handle = runner->stateHandleSnapshot(sessionId);
          BOOST_REQUIRE(handle.has_value());
          BOOST_CHECK_EQUAL(handle->role, role);
          BOOST_CHECK_EQUAL(handle->stateInferenceEpoch, epoch);
        }
      }
    }
    for (const auto& [role, runner] : runners) {
      if (!coordinator) {
        runner->releaseSessionState(sessionId);
      }
      else if (finalizeCheckpoint) {
        auto& runtime = *coordinatedRuntimes.at(role);
        if (resumed) {
          BOOST_CHECK(runtime.releaseDecodeState(sessionId, role));
          BOOST_CHECK(runtime.releaseConversationState(parentStates.at(role)));
        }
        const auto decode = runtime.decodeStateSnapshot();
        BOOST_CHECK_EQUAL(decode.entries, 0u);
        BOOST_CHECK_EQUAL(decode.pinnedEntries, 0u);
        BOOST_CHECK_EQUAL(decode.candidates, 0u);
        const auto conversation = runtime.conversationStateSnapshot();
        BOOST_CHECK_EQUAL(conversation.entries, resumed ? 0u : 1u);
        BOOST_CHECK_EQUAL(conversation.pinnedEntries, 0u);
        BOOST_CHECK_EQUAL(conversation.committingEntries, 0u);
      }
      BOOST_CHECK(!runner->stateHandleSnapshot(sessionId));
    }
    BOOST_TEST_MESSAGE(label << " resumed=" << resumed << " "
                       << (dynamic ? "prefill/decode" : "prefill")
                       << " max errors: logits=" << worstLogits << " complete-KV=" << worstState);
  };
  runTurn(false);
  if (finalizeCheckpoint) {
    observer->clear();
    runTurn(true);
  }
}

void
verifyThreeUnitModel(bool dynamic)
{
  const std::string source = hybridFixturePath(
    dynamic ? "four-layer-decode.onnx" : "four-layer-qwen.onnx");
  std::ifstream input(source, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
  const auto shape = qwen::deriveQwenTpShape(bytes, 4);
  NativeCanonicalSource canonicalSource;
  canonicalSource.modelBytes = bytes;
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 8U << 20};
  const auto descriptor = qwenFixtureDescriptor();
  const auto inspection = inspectNativeOnnxSourceGraph(canonicalSource, descriptor, control);
  NativeInspectedModel inspected;
  inspected.descriptor = descriptor;
  inspected.canonicalSourceName = "/fixture/source";
  inspected.canonicalSourceDigest = nativePlanningDigest(bytes.data(), bytes.size());
  inspected.modelManifestDigest = nativePlanningDigest("fixture-manifest");
  inspected.canonicalSourceBytes = bytes.size();
  inspected.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
  inspected.canonicalInitializerDigest = inspection.canonicalIdentity.initializerDigest;
  const auto preparedMetadata = encodeNativePreparedOnnxMetadata(inspected, inspection,
                                                                 nullptr, control);
  const auto restored = decodeNativePreparedOnnxMetadata(nativeParseJson(preparedMetadata), control);
  const auto metadataShape = qwen::deriveQwenTpShapeFromGraphMetadata(
    restored.graph.graphMetadataJson, 4);
  BOOST_CHECK_EQUAL(metadataShape.layers, shape.layers);
  BOOST_CHECK_EQUAL(metadataShape.hidden, shape.hidden);
  BOOST_CHECK_EQUAL(metadataShape.queryHeads, shape.queryHeads);
  BOOST_CHECK_EQUAL(metadataShape.kvHeads, shape.kvHeads);
  BOOST_CHECK_EQUAL(metadataShape.headDim, shape.headDim);
  BOOST_CHECK_EQUAL(metadataShape.ffn, shape.ffn);
  BOOST_CHECK_EQUAL(metadataShape.vocab, shape.vocab);
  auto brokenInspection = inspection;
  brokenInspection.initializerConsumers.begin()->second = {inspection.graph.nodes.size()};
  BOOST_CHECK_THROW(qwen::describeQwenTpEntryContracts(brokenInspection, 4,
    std::vector<qwen::QwenTpUnit>{{"unit", 0, 0, 1}}, 0, 1, "unit"), std::invalid_argument);
  auto brokenMetadata = nativeParseJson(restored.graph.graphMetadataJson);
  std::size_t attention = 0;
  for (auto& node : brokenMetadata.at("nodes")) {
    if (node.at("opType") != "GroupQueryAttention" || ++attention != 2) continue;
    node.at("integerAttributes").erase("num_heads");
    break;
  }
  BOOST_CHECK_THROW(qwen::deriveQwenTpShapeFromGraphMetadata(
    nativeCanonicalJson(brokenMetadata), 4), std::invalid_argument);
  brokenMetadata = nativeParseJson(restored.graph.graphMetadataJson);
  for (auto& node : brokenMetadata.at("nodes")) {
    if (node.at("opType") != "MatMulNBits" ||
        node.at("name").get<std::string>().find("q_proj") == std::string::npos) continue;
    node["integerAttributes"]["N"] = 32;
    break;
  }
  BOOST_CHECK_THROW(qwen::deriveQwenTpShapeFromGraphMetadata(
    nativeCanonicalJson(brokenMetadata), 4), std::invalid_argument);
  HybridScratch workspace("spec242-three-workers");

  std::vector<std::vector<qwen::QwenTpUnit>> units(2);
  std::vector<std::vector<qwen::QwenTpEntryContract>> tables(2);
  std::map<std::string, std::filesystem::path> roots;
  for (std::size_t stage = 0; stage < 2; ++stage) {
    const std::uint64_t degree = stage == 0 ? 2 : 1;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      qwen::QwenTpUnit unit;
      unit.roleId = "stage" + std::to_string(stage) + "-member" + std::to_string(rank);
      unit.rank = rank;
      unit.layerBegin = stage == 0 ? 0 : 1;
      unit.layerEnd = stage == 0 ? 1 : 4;
      qwen::QwenTpAssemblyShards shards;
      if (degree > 1) {
        shards.slicesByLayer = qwen::planQwenTensorSlices(
          shape, degree, rank, unit.layerBegin, unit.layerEnd);
        shards.geometry.degree = degree;
        shards.geometry.rank = rank;
        shards.geometry.headDim = shape.headDim;
        shards.geometry.queryWidth = shape.queryWidth();
        shards.geometry.kvWidth = shape.kvWidth();
        shards.geometry.hidden = shape.hidden;
        shards.geometry.ffn = shape.ffn;
      }
      shards.readExternal = [] (std::uint64_t, std::uint64_t) -> std::vector<std::uint8_t> {
        throw std::runtime_error("self-contained fixture unexpectedly reads a sidecar");
      };
      const auto root = workspace.root / unit.roleId;
      const auto table = qwen::assembleQwenTpEntryTable(
        root, unit.roleId, bytes, 4, shards, nullptr, unit.layerBegin, unit.layerEnd);
      tables[stage].insert(tables[stage].end(), table.begin(), table.end());
      roots.emplace(unit.roleId, root);
      units[stage].push_back(std::move(unit));
    }
  }
  const auto& upstream = units[0].front().roleId;
  const auto& egress = units[1].front().roleId;
  qwen::QwenTpStageWindow first{"first", nativePlanningDigest("first"), 0, 1};
  qwen::QwenTpStageWindow last{"last", nativePlanningDigest("last"), 1, 4,
    upstream, upstream + "/MLP/0", "post_residual", upstream + "/SUM_MLP/0", "merged"};
  std::vector<std::vector<qwen::QwenTpEntryContract>> sourceTables;
  const auto samePorts = [] (const auto& left, const auto& right) {
    BOOST_REQUIRE_EQUAL(left.size(), right.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
      const auto& a = left[index];
      const auto& b = right[index];
      BOOST_CHECK_EQUAL(a.entryId, b.entryId);
      BOOST_CHECK_EQUAL(a.unitId, b.unitId);
      BOOST_CHECK_EQUAL(a.kind, b.kind);
      BOOST_CHECK_EQUAL(a.layer, b.layer);
      BOOST_CHECK(a.inputNames == b.inputNames && a.outputNames == b.outputNames);
      BOOST_CHECK(a.inputSlotByGraphName == b.inputSlotByGraphName);
      BOOST_CHECK(a.outputSlotByGraphName == b.outputSlotByGraphName);
      BOOST_CHECK(a.passthroughSlotByInputGraphName == b.passthroughSlotByInputGraphName);
      BOOST_CHECK(a.stateOutputGraphNames == b.stateOutputGraphNames);
      BOOST_CHECK(a.synthesizedInputGraphNames == b.synthesizedInputGraphNames);
    }
  };
  for (std::size_t stage = 0; stage < 2; ++stage) {
    const auto begin = stage == 0 ? 0u : 1u;
    const auto end = stage == 0 ? 1u : 4u;
    const auto& owner = stage == 0 ? upstream : egress;
    auto ports = qwen::describeQwenTpEntryContracts(inspection, 4, units[stage], begin, end, owner);
    const auto cached = qwen::describeQwenTpEntryContracts(restored.graph, 4, units[stage],
                                                         begin, end, owner);
    samePorts(ports, cached);
    samePorts(ports, tables[stage]);
    BOOST_CHECK_THROW(qwen::qwenTpEntryTableJson(ports), std::invalid_argument);
    for (const auto& entry : ports) BOOST_CHECK(!entry.hasGraphFile());
    sourceTables.push_back(std::move(ports));
  }
  NativeExecutionPlan stage0;
  NativeExecutionPlan stage1;
  qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0], sourceTables[0], first);
  qwen::projectQwenTpPlan(stage1, shape, 1, egress, units[1], sourceTables[1], last);
  std::map<std::string, std::string> artifactIds;
  std::map<std::string, NativeRoleResourceRequirement> resources;
  for (const auto& stage : units) {
    for (const auto& unit : stage) {
      artifactIds[unit.roleId] = nativePlanningDigest("fixture-artifact|" + unit.roleId);
      resources[unit.roleId] = NativeRoleResourceRequirement{{"onnxruntime"}, 8U << 20};
    }
  }
  const auto identity = nativePlanningDigest("fixture-stage");
  qwen::NativeQwenLayerSplit splitter({{0, 1}, {1, 4}},
    {{"first", identity}, {"last", identity}}, {{"first", 4096}, {"last", 4096}},
    {"first", "last"}, {1, 1});
  const auto planningGraph = splitter.inspectGraph(descriptor, "fixture-v1", 100);
  const auto candidate = qwen::buildQwenHybridCandidate(planningGraph, shape,
    {first, last}, units, "entry", egress, descriptor, artifactIds, resources, inspection);
  const auto cachedCandidate = qwen::buildQwenHybridCandidate(planningGraph, metadataShape,
    {first, last}, units, "entry", egress, descriptor, artifactIds, resources, restored.graph);
  BOOST_CHECK_NO_THROW(candidate.validate(planningGraph));
  BOOST_CHECK_EQUAL(candidate.candidateDigest, cachedCandidate.candidateDigest);
  const auto& plan = candidate.executionPlan;
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  for (const auto& step : plan.steps) {
    const bool attentionStep = step.artifactEntry.find("/ATTN/") != std::string::npos;
    BOOST_CHECK_EQUAL(step.localStateSlots.empty(), !(dynamic && attentionStep));
    if (step.unitId == egress)
      BOOST_CHECK(step.artifactEntry.find("/INIT/") == std::string::npos);
    for (const auto& port : step.inputSlots) {
      BOOST_CHECK(port.slotId != "attn_mask" && port.slotId != "position_ids" &&
                  port.slotId != "total_sequence_length");
    }
  }
  // Required real bindings cannot be replaced by synthesized inputs or unknown slots.
  auto invalid = tables[0];
  invalid.front().inputSlotByGraphName.begin()->second = "unplanned-input";
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0],
                                          invalid, first), std::invalid_argument);
  invalid = tables[0];
  invalid.front().synthesizedInputGraphNames = {"input_ids"};
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0],
                                          invalid, first), std::invalid_argument);
  invalid = tables[0];
  invalid.front().outputSlotByGraphName.clear();
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0],
                                          invalid, first), std::invalid_argument);
  invalid = tables[0];
  invalid.pop_back();
  BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0],
                                          invalid, first), std::invalid_argument);
  if (dynamic) {
    invalid = tables[0];
    const auto stateful = std::find_if(invalid.begin(), invalid.end(),
      [] (const auto& entry) { return !entry.stateOutputGraphNames.empty(); });
    BOOST_REQUIRE(stateful != invalid.end());
    stateful->stateOutputGraphNames.front() = "present.999.key";
    BOOST_CHECK_THROW(qwen::projectQwenTpPlan(stage0, shape, 2, upstream, units[0],
                                            invalid, first), std::invalid_argument);
  }

  std::map<std::string, NativeModelRunnerSpec> specs;
  for (const auto& role : plan.roles) {
    std::vector<qwen::QwenTpEntryContract> entries;
    for (const auto& table : tables)
      for (const auto& entry : table)
        if (entry.unitId == role) entries.push_back(entry);
    BOOST_REQUIRE(!entries.empty());
    NativeModelRunnerSpec spec;
    spec.role = role;
    spec.kind = "onnx";
    spec.backend = qwen::QWEN_TENSOR_RUNNER_BACKEND;
    spec.path = (roots.at(role) / entries.front().relativeGraphPath).string();
    spec.metadata["tensorParallelEntries"] = qwen::qwenTpEntryTableJson(entries);
    spec.metadata["tensorParallelEntryRoot"] = roots.at(role).string();
    std::vector<NativeExecutionStepSpec> steps;
    for (const auto& step : plan.steps)
      if (step.unitId == role) steps.push_back(step);
    spec.metadata["tensorParallelSteps"] = qwen::qwenTpStepDeclarationJson(steps);
    spec.metadata["providerIdentity"] = "/fixture/" + role;
    specs.emplace(role, std::move(spec));
  }
  verifyHybridExecution(source, plan, shape, units, egress, specs, dynamic, "direct material");
}

} // namespace

BOOST_AUTO_TEST_CASE(ThreeUnitsRecomposeTheCompletePrefillGraph)
{
  verifyThreeUnitModel(false);
}

BOOST_AUTO_TEST_CASE(CompleteSourceSupportsThirtyTwoTokensInBothConversationTurns)
{
  auto run = makeCompleteSourceReference(hybridFixturePath("four-layer-conversation.onnx"));
  constexpr std::size_t TOKENS_PER_TURN = 32;
  constexpr std::size_t VOCABULARY = 64;
  constexpr std::int64_t EOS_TOKEN = 63;
  const std::vector<std::vector<std::int64_t>> inputs{{1, 2, 3}, {4}};
  std::vector<std::int64_t> prefix;
  for (std::size_t turn = 0; turn < inputs.size(); ++turn) {
    prefix.insert(prefix.end(), inputs[turn].begin(), inputs[turn].end());
    std::vector<std::int64_t> generated;
    for (std::size_t index = 0; index < TOKENS_PER_TURN; ++index) {
      BOOST_TEST_CONTEXT("turn=" << turn + 1 << " token=" << index + 1) {
        const auto outputs = run(prefix);
        const auto& logits = outputs.at("logits");
        BOOST_REQUIRE_EQUAL(logits.payload.size(), prefix.size() * VOCABULARY * sizeof(float));
        std::array<float, VOCABULARY> row;
        std::memcpy(row.data(), logits.payload.data() + logits.payload.size() - sizeof(row),
                    sizeof(row));
        BOOST_REQUIRE(std::all_of(row.begin(), row.end(), [] (float value) {
          return std::isfinite(value);
        }));
        const auto token = std::distance(row.begin(), std::max_element(row.begin(), row.end()));
        BOOST_REQUIRE_NE(token, EOS_TOKEN);
        generated.push_back(token);
        prefix.push_back(token);
      }
    }
    // The last sampled token must be included in the reference state for the next turn.
    const auto finalized = run(prefix);
    BOOST_REQUIRE_EQUAL(finalized.size(), 9u);
    for (std::size_t layer = 0; layer < 4; ++layer) {
      for (const auto* kind : {"key", "value"}) {
        const auto& tensor = finalized.at("present." + std::to_string(layer) + "." + kind);
        const std::vector<std::int64_t> expectedShape{
          1, 2, static_cast<std::int64_t>(prefix.size()), 16};
        BOOST_REQUIRE(tensor.shape == expectedShape);
        std::vector<float> state(2 * prefix.size() * 16);
        BOOST_REQUIRE_EQUAL(tensor.payload.size(), state.size() * sizeof(float));
        std::memcpy(state.data(), tensor.payload.data(), tensor.payload.size());
        BOOST_CHECK(std::all_of(state.begin(), state.end(), [] (float value) {
          return std::isfinite(value);
        }));
      }
    }
    BOOST_TEST_MESSAGE("reference turn=" << turn + 1 << " prefix=" << prefix.size()
                       << " tokens=" << nativeCanonicalJson(NativeJson(generated)));
  }
  BOOST_CHECK_EQUAL(prefix.size(), 68u);
}

BOOST_AUTO_TEST_CASE(ThreeUnitsDecodeWithCompleteKvAndRotary)
{
  verifyThreeUnitModel(true);
}

BOOST_AUTO_TEST_CASE(LegacyKvObserverKeepsItsSixteenPointContract)
{
  auto observer = std::make_shared<FixtureStateObserver>();
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory, nullptr, observer);
  NativeModelRunnerSpec spec;
  spec.role = "probe";
  spec.kind = "onnx";
  spec.backend = "onnxruntime";
  spec.path = hybridFixturePath("four-layer-decode.onnx");
  spec.metadata["statefulModel"] = "true";
  spec.metadata["providerIdentity"] = "/fixture/probe";
  spec.metadata["providerBootId"] = "fixture-boot";
  for (std::size_t layer = 0; layer < 4; ++layer) {
    for (const auto* kind : {"key", "value"}) {
      const auto suffix = std::to_string(layer) + "." + kind;
      for (const auto* field : {"stateInputNames", "stateOutputNames", "stateSuccessorMap"})
        if (!spec.metadata[field].empty()) spec.metadata[field] += ',';
      spec.metadata["stateInputNames"] += "past_key_values." + suffix;
      spec.metadata["stateOutputNames"] += "present." + suffix;
      spec.metadata["stateSuccessorMap"] += "past_key_values." + suffix + "=present." + suffix;
    }
  }
  auto runner = factory.create(spec);
  RoleExecutionContext context;
  context.sessionId = "three-worker-prefill";
  context.role = "probe";
  context.providerBootId = "fixture-boot";
  context.streamingStateExecution = true;
  const std::array<std::int64_t, 3> tokens{1, 2, 3};
  NamedTensor ids;
  ids.name = "input_ids";
  ids.elementType = TensorElementType::Int64;
  ids.shape = {1, 3};
  ids.payload.resize(sizeof(tokens));
  std::memcpy(ids.payload.data(), tokens.data(), ids.payload.size());
  context.inputsByScope.emplace("input_ids", makeEncodedTensorBundle("input_ids", {ids}));
  BOOST_CHECK_NO_THROW(runner->run(context));
  for (std::size_t layer = 0; layer < 4; ++layer) {
    for (const auto* kind : {"key", "value"}) {
      const auto sample = observer->sample("probe", 0,
        "past_key_values." + std::to_string(layer) + "." + kind);
      BOOST_CHECK_EQUAL(sample.indices.size(), 16u);
      BOOST_CHECK_EQUAL(sample.values.size(), 16u);
      BOOST_CHECK_EQUAL(sample.indices.front(), 0u);
      BOOST_CHECK_EQUAL(sample.indices.back(), 95u);
    }
  }
  runner->releaseSessionState(context.sessionId);
  BOOST_CHECK(!runner->stateHandleSnapshot(context.sessionId));
}

namespace {

/** Add actual controls before SOURCE identity, retaining the complete original as oracle. */
void
addHybridFixtureControls(NativeCanonicalSource& source)
{
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(source.modelBytes.data(),
                                     static_cast<int>(source.modelBytes.size())));
  auto& graph = *model.mutable_graph();
  const std::vector<onnx::NodeProto> original(graph.node().begin(), graph.node().end());
  graph.clear_node();
  const auto info = [] (onnx::ValueInfoProto& value, const std::string& name, int type,
                        const std::vector<std::int64_t>& shape) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    auto* dimensions = tensor->mutable_shape();
    for (const auto size : shape) {
      auto* dimension = dimensions->add_dim();
      if (size < 0) dimension->set_dim_param("sequence");
      else dimension->set_dim_value(size);
    }
  };
  info(*graph.add_input(), "control.delta", onnx::TensorProto::INT32, {});
  info(*graph.add_input(), "control.positions", onnx::TensorProto::INT64, {1, -1});
  info(*graph.add_value_info(), "control.bias", onnx::TensorProto::FLOAT, {});
  info(*graph.add_value_info(), "control.offsets", onnx::TensorProto::INT64, {1, -1});
  const auto node = [&] (const std::string& kind, const std::string& name,
                         const std::vector<std::string>& inputs, const std::string& output) {
    auto* result = graph.add_node();
    result->set_op_type(kind);
    result->set_name(name);
    for (const auto& input : inputs) result->add_input(input);
    result->add_output(output);
    return result;
  };
  const auto cast = [&] (const std::string& name, const std::string& input,
                         const std::string& output) {
    auto* result = node("Cast", name, {input}, output);
    auto* attribute = result->add_attribute();
    attribute->set_name("to");
    attribute->set_type(onnx::AttributeProto::INT);
    attribute->set_i(onnx::TensorProto::FLOAT);
  };
  cast("INIT/control_bias", "control.delta", "control.bias");
  node("Identity", "INIT/control_offsets", {"control.positions"}, "control.offsets");
  auto* axes = graph.add_initializer();
  axes->set_name("fixture.control.axes");
  axes->set_data_type(onnx::TensorProto::INT64);
  axes->add_dims(1);
  axes->add_int64_data(-1);
  for (auto originalNode : original) {
    for (std::uint64_t layer = 0; layer < 4; ++layer) {
      const auto prefix = "model.layers." + std::to_string(layer) + "/input_layernorm";
      const auto hidden = layer == 0 ? std::string("x0")
        : "model.layers." + std::to_string(layer) + "/input";
      if (originalNode.name() == prefix + ".square") {
        cast(prefix + ".control_cast", "control.offsets", prefix + ".control_float");
        node("Unsqueeze", prefix + ".control_expand",
             {prefix + ".control_float", axes->name()}, prefix + ".control_vector");
        node("Add", prefix + ".control_positions",
             {hidden, prefix + ".control_vector"}, prefix + ".control_shift");
        node("Add", prefix + ".control_bias",
             {prefix + ".control_shift", "control.bias"}, prefix + ".controlled");
        info(*graph.add_value_info(), prefix + ".control_float",
              onnx::TensorProto::FLOAT, {1, -1});
        info(*graph.add_value_info(), prefix + ".control_vector",
              onnx::TensorProto::FLOAT, {1, -1, 1});
        for (const auto* suffix : {".control_shift", ".controlled"})
          info(*graph.add_value_info(), prefix + suffix, onnx::TensorProto::FLOAT, {1, -1, 64});
      }
      if (originalNode.name().rfind(prefix + ".", 0) == 0)
        for (auto& input : *originalNode.mutable_input())
          if (input == hidden) input = prefix + ".controlled";
    }
    *graph.add_node() = std::move(originalNode);
  }
  const auto wire = model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
}

std::pair<NativeJson, NativeCanonicalSource>
hybridCatalogFixture(const std::string& protectionEpoch = "fixture",
                     const std::string& backend = "onnxruntime", bool mirrored = false,
                     bool externalControls = false,
                     const std::vector<std::uint64_t>& degrees = {},
                     bool finalizeCheckpoint = false)
{
  const auto sourcePath = finalizeCheckpoint ? hybridFixturePath("four-layer-conversation.onnx")
    : degrees.empty() ? hybridFixturePath("four-layer-decode.onnx")
                      : hybridFixturePath("four-layer-degree-matrix.onnx");
  std::ifstream input(sourcePath, std::ios::binary);
  BOOST_REQUIRE(static_cast<bool>(input));
  NativeCanonicalSource source;
  source.modelBytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (externalControls) addHybridFixtureControls(source);
  auto descriptor = qwenFixtureDescriptor();
  descriptor.adapter.backends = {backend};
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 8U << 20};
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  const auto digest = nativePlanningDigest("spec242-catalog");
  std::vector<std::string> roles{"first", "last"};
  NativeJson configuration = {
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/source"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest}, {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest}, {"assembler_descriptor_digest", digest},
      {"backend_abi", "onnxruntime"}, {"precision", "int4"},
      {"quantization", "weight_only_int4"}, {"layout", "BNSH"}, {"padding", "none"},
      {"protection_epoch", protectionEpoch}, {"max_source_bytes", 8U << 20},
      {"max_assembled_bytes", 8U << 20}, {"max_nodes", 512}}},
    {"publication", {{"artifact_root", "/fixture/artifact"}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 1U << 20}};
  const auto resources = nativeParseJson(NativeRoleResourceRequirement{
    {backend}, 8U << 20, 0, 0, 0, 0, 1.0}.canonicalJson());
  configuration["splitter"] = {
    {"kind", "QWEN"}, {"roles", roles}, {"layer_ranges", {{0, 1}, {1, 4}}},
    {"tensor_degrees", {2, 1}},
    {"artifact_digests_by_role", {{"first", digest}, {"last", digest}}},
    {"rank_artifact_digests_by_role", {{"first",
      {digest, nativePlanningDigest("spec242-catalog-rank1")}}}},
    {"weight_bytes_by_role", {{"first", 8U << 20}, {"last", 8U << 20}}},
    {"resources_by_role", {{"first", resources}, {"last", resources}}},
    {"input_ingress_role", "first"}, {"result_egress_role", "last"}};
  if (mirrored) {
    configuration["splitter"]["tensor_degrees"] = {1, 2};
    configuration["splitter"]["rank_artifact_digests_by_role"] =
      {{"last", {digest, nativePlanningDigest("spec242-catalog-rank1")}}};
  }
  if (!degrees.empty()) {
    BOOST_REQUIRE(!mirrored && !externalControls);
    BOOST_REQUIRE(degrees.size() == 3 || degrees.size() == 4);
    roles.clear();
    auto& split = configuration["splitter"];
    split["roles"] = NativeJson::array();
    split["layer_ranges"] = NativeJson::array();
    split["tensor_degrees"] = degrees;
    for (const auto* key : {"artifact_digests_by_role", "rank_artifact_digests_by_role",
                            "weight_bytes_by_role", "resources_by_role"})
      split[key] = NativeJson::object();
    for (std::size_t stage = 0; stage < degrees.size(); ++stage) {
      const auto role = "stage-" + std::to_string(stage);
      roles.push_back(role);
      split["roles"].push_back(role);
      split["layer_ranges"].push_back({stage, stage + 1 == degrees.size() ? 4 : stage + 1});
      split["artifact_digests_by_role"][role] = nativePlanningDigest(role);
      split["weight_bytes_by_role"][role] = 8U << 20;
      split["resources_by_role"][role] = resources;
      if (degrees[stage] > 1) {
        for (std::uint64_t rank = 0; rank < degrees[stage]; ++rank)
          split["rank_artifact_digests_by_role"][role].push_back(
            nativePlanningDigest(role + "-rank-" + std::to_string(rank)));
      }
    }
    split["input_ingress_role"] = roles.front();
    split["result_egress_role"] = roles.back();
    // The semantic graph digest commits the configured layer ranges.
    configuration["model"]["graph_digest"] = degrees.size() == 3
      ? "sha256:fb1f2743a951cd523059ca361051bc9c58016d2f1e83cbd5ad28a37df7b31222"
      : "sha256:6fe1870bd05c837aff1048615347a3b546f206f5273bf8e596b78eb7873a7ee2";
  }
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(source.modelBytes.data(),
                                     static_cast<int>(source.modelBytes.size())));
  std::vector<std::string> nodeNames;
  for (const auto& node : model.graph().node()) nodeNames.push_back(node.name());
  for (const auto& entry : qwen::classifyQwenTpEntryNodes(nodeNames, 4)) {
    const auto semantic = entry.kind == "INIT" ? "embedding" : entry.kind == "HEAD"
      ? "final-norm-head" : "layer-0" + std::to_string(entry.layer);
    for (const auto index : entry.nodeIndices)
      configuration["node_mapping"][semantic].push_back(index);
  }
  for (std::uint64_t layer = 0; layer < 4; ++layer) {
    const auto& role = roles.at(std::min<std::size_t>(layer, roles.size() - 1));
    for (const std::string component : {"key", "value"}) {
      const auto suffix = std::to_string(layer) + "." + component;
      configuration["state_inputs"][role][suffix] = {"past_key_values." + suffix};
      configuration["state_outputs"][role][suffix] = {"present." + suffix};
    }
  }
  return {std::move(configuration), std::move(source)};
}

} // namespace

BOOST_AUTO_TEST_CASE(HybridCatalogBindsRealSourceStateBeforePreparingRoles)
{
  const auto fixture = hybridCatalogFixture();
  const auto& configuration = fixture.first;
  const auto& source = fixture.second;
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 8U << 20};
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source,
                                                 control);
  BOOST_REQUIRE(loaded.planningCache);
  BOOST_REQUIRE_EQUAL(loaded.planningCache->candidates.size(), 1u);
  const auto& prepared = loaded.planningCache->candidates.front();
  BOOST_REQUIRE_EQUAL(prepared.roles.size(), 3u);
  const NativeRequestControl regionControl{"/fixture/source-region", 1, control.deadline, {}};
  const auto& stages = prepared.candidate.executionPlan.stageLayout;
  BOOST_REQUIRE_EQUAL(stages.size(), 2u);
  BOOST_CHECK_EQUAL(stages.front().stageId, "first");
  BOOST_CHECK_EQUAL(stages.back().stageId, "last");
  {
    auto baseline = prepared.candidate.executionPlan;
    const auto& receiver = stages.back().unitRoleIds.front();
    const auto findStep = [&](const auto& id) {
      return std::find_if(baseline.steps.begin(), baseline.steps.end(),
        [&](const auto& step) { return step.stepId == id; });
    };
    const auto edge = std::find_if(baseline.dependencies.begin(), baseline.dependencies.end(),
      [&](const auto& dep) {
        if (dep.consumers != std::vector<std::string>{receiver}) return false;
        const auto step = findStep(dep.producerStepId);
        return step != baseline.steps.end() && step->operation == "SUM";
      });
    BOOST_REQUIRE(edge != baseline.dependencies.end());
    const auto cut = *edge;
    const auto oldSum = *findStep(cut.producerStepId);

    // Each side that needs the complete value performs its own SUM.
    auto upstreamRead = oldSum;
    upstreamRead.stepId += "/retained";
    upstreamRead.operation = "CALL_SUBGRAPH";
    upstreamRead.artifactEntry = "fixture-retained-sum";
    upstreamRead.inputSlots = oldSum.outputSlots;
    upstreamRead.inputSlots.front().slotId = "retained-sum";
    upstreamRead.inputSlots.front().producerStepId = oldSum.stepId;
    upstreamRead.inputSlots.front().producerSlotId = oldSum.outputSlots.front().slotId;
    baseline.steps.push_back(upstreamRead);
    const auto both = makeReceivingStageReduction(baseline, receiver);
    BOOST_REQUIRE(both.has_value());
    BOOST_CHECK(std::any_of(both->steps.begin(), both->steps.end(), [&](const auto& step) {
      return step.stepId == oldSum.stepId;
    }));
    BOOST_CHECK_EQUAL(std::count_if(both->steps.begin(), both->steps.end(), [&](const auto& step) {
      return step.unitId == receiver && step.operation == "SUM";
    }), 1U);
    BOOST_CHECK_EQUAL(std::count_if(both->dependencies.begin(), both->dependencies.end(),
      isSharedTensorPartial), 1U);
    baseline.steps.pop_back();

    auto moved = makeReceivingStageReduction(baseline, receiver);
    BOOST_REQUIRE(moved.has_value());

    // The previous planner may have omitted the unused mirror SUM already.
    const auto peer = std::find_if(baseline.steps.begin(), baseline.steps.end(),
      [&](const auto& step) {
        if (step.operation != "SUM" || step.unitId == oldSum.unitId ||
            step.inputSlots.size() != oldSum.inputSlots.size()) return false;
        return std::equal(step.inputSlots.begin(), step.inputSlots.end(),
          oldSum.inputSlots.begin(), [](const auto& left, const auto& right) {
            return left.producerStepId == right.producerStepId &&
                   left.producerSlotId == right.producerSlotId;
          });
      });
    BOOST_REQUIRE(peer != baseline.steps.end());
    const auto peerId = peer->stepId;
    baseline.steps.erase(peer);
    baseline.dependencies.erase(std::remove_if(baseline.dependencies.begin(),
      baseline.dependencies.end(), [&](const auto& dep) { return dep.consumerStepId == peerId; }),
      baseline.dependencies.end());
    auto single = makeReceivingStageReduction(baseline, receiver);
    BOOST_REQUIRE(single.has_value());
    BOOST_CHECK_EQUAL(single->steps.size(), moved->steps.size());
    BOOST_CHECK_EQUAL(single->dependencies.size(), moved->dependencies.size());
  }
  std::set<std::string> sourceIdentities;
  for (const auto& stage : stages) {
    BOOST_REQUIRE(!stage.unitRoleIds.empty());
    const auto& owner = stage.unitRoleIds.front();
    std::vector<std::string> stateOutputs;
    for (const auto& [slot, names] : loaded.stateMapping.outputs.at(owner))
      stateOutputs.insert(stateOutputs.end(), names.begin(), names.end());
    const auto region = loaded.preparation->sourceRegion(loaded.model,
      prepared.candidate.roleNodeIdsByRole.at(owner), stateOutputs, regionControl);
    BOOST_REQUIRE(!region.identityDigest.empty());
    BOOST_CHECK(sourceIdentities.insert(region.identityDigest).second);
    BOOST_CHECK_EQUAL(stage.sourceRegionDigest,
                      nativePlanningDigest("qwen-hybrid-source-v2|" + region.identityDigest));
  }
  // Invalid unit geometry must reject before querying any source or producing a candidate.
  const auto shape = qwen::deriveQwenTpShape(source.modelBytes, 4);
  const std::vector<qwen::QwenTpStageWindow> guardWindow{
    {"guard-stage", nativePlanningDigest("guard-region"), 0, 4}};
  const qwen::QwenTpSourceRegionPort unusedRegion = [] (const auto&, const auto&)
    -> NativeSourceRegionBoundary {
      throw std::logic_error("invalid geometry reached the source owner");
    };
  const auto rejectsUnit = [&] (const std::vector<qwen::QwenTpUnit>& units,
                                const std::string& expected) {
    BOOST_CHECK_EXCEPTION(qwen::buildQwenHybridCandidate(loaded.model.graph, shape,
      guardWindow, {units}, "entry", "guard-unit", loaded.model.descriptor, {}, {},
      NativeOnnxGraphInspection{}, unusedRegion, NativeStateTensorMapping{}),
      std::invalid_argument, [&] (const std::invalid_argument& error) {
        return std::string(error.what()).find(expected) != std::string::npos;
      });
  };
  rejectsUnit({{"guard-unit", 1, 0, 4}}, "out-of-range rank");
  rejectsUnit({{"guard-unit", 0, 1, 4}}, "template's layer range");
  rejectsUnit({{"guard-unit", 1, 0, 4}, {"guard-peer", 0, 0, 4}}, "member order");
  std::map<std::uint64_t, std::string> rankRecipes;
  for (const auto& role : prepared.roles) {
    const auto boundary = loaded.preparation->sourceRegion(loaded.model,
      prepared.candidate.roleNodeIdsByRole.at(role.role), {}, regionControl);
    BOOST_REQUIRE_EQUAL(boundary.inputs.size(), role.expectedInputs.size());
    BOOST_REQUIRE_EQUAL(boundary.outputs.size(), role.expectedOutputs.size());
    for (const auto& port : role.expectedInputs) {
      BOOST_CHECK_EQUAL(boundary.inputs.at(port.name).dtype, port.dtype);
      BOOST_CHECK(boundary.inputs.at(port.name).shape == port.shape);
    }
    for (const auto& port : role.expectedOutputs) {
      BOOST_CHECK_EQUAL(boundary.outputs.at(port.name).dtype, port.dtype);
      BOOST_CHECK(boundary.outputs.at(port.name).shape == port.shape);
    }
    const bool first = role.layerBegin == 0;
    const auto& inputs = prepared.candidate.roleStateInputsByRole.at(role.role);
    const auto& outputs = prepared.candidate.roleStateOutputsByRole.at(role.role);
    BOOST_REQUIRE_EQUAL(inputs.size(), first ? 2u : 6u);
    BOOST_REQUIRE_EQUAL(outputs.size(), inputs.size());
    const std::vector<std::variant<std::int64_t, std::string>> inputShape{
      std::int64_t{1}, std::int64_t{2}, std::string("past_sequence"), std::int64_t{16}};
    auto outputShape = inputShape;
    outputShape[2] = std::string("total_sequence");
    std::set<std::string> actualInputs;
    std::set<std::string> actualOutputs;
    for (const auto& tensor : inputs) {
      BOOST_CHECK_EQUAL(tensor.dtype, "float32");
      BOOST_CHECK(tensor.shape == inputShape);
      actualInputs.insert(tensor.name);
    }
    for (const auto& tensor : outputs) {
      BOOST_CHECK_EQUAL(tensor.dtype, "float32");
      BOOST_CHECK(tensor.shape == outputShape);
      actualOutputs.insert(tensor.name);
    }
    std::set<std::string> expectedInputs;
    std::set<std::string> expectedOutputs;
    for (std::uint64_t layer = first ? 0 : 1; layer < (first ? 1 : 4); ++layer) {
      for (const std::string component : {"key", "value"}) {
        const auto suffix = std::to_string(layer) + "." + component;
        expectedInputs.insert("past_key_values." + suffix);
        expectedOutputs.insert("present." + suffix);
      }
    }
    BOOST_CHECK(actualInputs == expectedInputs);
    BOOST_CHECK(actualOutputs == expectedOutputs);
    if (first) {
      BOOST_CHECK_EQUAL(role.tensorParallelDegree, 2u);
      BOOST_CHECK(rankRecipes.emplace(role.tensorParallelRank, role.recipeDigest).second);
    }
    else {
      BOOST_CHECK_EQUAL(role.tensorParallelDegree, 0u);
      BOOST_CHECK_EQUAL(role.tensorParallelRank, 0u);
      BOOST_CHECK_EQUAL(prepared.candidate.tensorIdentityByRole.count(role.role), 0u);
      std::vector<const NativeExecutionStepSpec*> owned;
      for (const auto& step : prepared.candidate.executionPlan.steps)
        if (step.unitId == role.role) owned.push_back(&step);
      BOOST_REQUIRE_EQUAL(owned.size(), 1u);
      BOOST_CHECK_EQUAL(owned.front()->operation, "CALL_SUBGRAPH");
      BOOST_CHECK_EQUAL(owned.front()->inputSlots.size(), 2u);
      BOOST_CHECK_EQUAL(owned.front()->outputSlots.size(), 1u);
      BOOST_CHECK_EQUAL(owned.front()->localStateSlots.size(), 6u);
      for (const auto& port : owned.front()->inputSlots) {
        BOOST_CHECK_EQUAL(port.source, "REMOTE_STEP");
        BOOST_CHECK_NE(port.producerStepId, owned.front()->stepId);
        BOOST_CHECK_EQUAL(port.dtype, boundary.inputs.at(port.slotId).dtype);
        BOOST_CHECK(port.shape == boundary.inputs.at(port.slotId).shape);
      }
    }
  }
  BOOST_REQUIRE_EQUAL(rankRecipes.size(), 2u);
  BOOST_CHECK_NE(rankRecipes.at(0), rankRecipes.at(1));
  auto foreignModel = loaded.model;
  foreignModel.canonicalSourceDigest = nativePlanningDigest("foreign source region");
  BOOST_CHECK_THROW(loaded.preparation->sourceRegion(foreignModel,
    prepared.candidate.roleNodeIdsByRole.at(prepared.roles.front().role), {}, regionControl),
    std::invalid_argument);
  const auto verifiedSource = loaded.preparation->sourceRefFor(loaded.model.descriptor);
  BOOST_REQUIRE(verifiedSource);
  BOOST_REQUIRE(verifiedSource->materialManifest);
  NativeCanonicalSource reference;
  reference.preparedMetadataJson = verifiedSource->preparedMetadataJson;
  auto references = std::make_shared<NativeCanonicalSource::MaterialManifest>(
    *verifiedSource->materialManifest);
  references->payloads.clear();
  references->payloadsComplete = false;
  references->validate();
  reference.materialManifest = std::move(references);
  const auto restored = NativeRequestCatalog::load(nativeCanonicalJson(configuration),
                                                   reference, control);
  BOOST_REQUIRE(restored.planningCache);
  BOOST_REQUIRE_EQUAL(restored.planningCache->candidates.size(), 1u);
  const auto& warm = restored.planningCache->candidates.front();
  BOOST_CHECK_EQUAL(warm.candidate.candidateDigest, prepared.candidate.candidateDigest);
  BOOST_REQUIRE_EQUAL(warm.roles.size(), prepared.roles.size());
  for (std::size_t index = 0; index < warm.roles.size(); ++index)
    BOOST_CHECK_EQUAL(warm.roles[index].recipeDigest, prepared.roles[index].recipeDigest);
  const auto& warmRole = warm.roles.back();
  const auto warmBoundary = restored.preparation->sourceRegion(restored.model,
    warm.candidate.roleNodeIdsByRole.at(warmRole.role), {}, regionControl);
  BOOST_REQUIRE_EQUAL(warmBoundary.inputs.size(), warmRole.expectedInputs.size());
  BOOST_REQUIRE_EQUAL(warmBoundary.outputs.size(), warmRole.expectedOutputs.size());
  for (const auto& port : warmRole.expectedOutputs) {
    BOOST_CHECK_EQUAL(warmBoundary.outputs.at(port.name).dtype, port.dtype);
    BOOST_CHECK(warmBoundary.outputs.at(port.name).shape == port.shape);
  }
  auto foreignState = configuration;
  foreignState["state_inputs"]["last"]["1.key"] = {"past_key_values.0.key"};
  BOOST_CHECK_EXCEPTION(NativeRequestCatalog::load(nativeCanonicalJson(foreignState), source,
                                                  control), std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("source boundary") != std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(HybridCatalogAdmitsExplicitBoundaryUnits)
{
  auto [configuration, source] = hybridCatalogFixture();
  auto& split = configuration.at("splitter");
  split["input_ingress_role"] = "first/Tensor/Rank/1";
  split["result_egress_role"] = "last/Tensor/Rank/0";
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 8U << 20};
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control);
  BOOST_REQUIRE(loaded.planningCache);
  BOOST_REQUIRE_EQUAL(loaded.planningCache->candidates.size(), 1u);
  const auto& candidate = loaded.planningCache->candidates.front().candidate;
  BOOST_CHECK_EQUAL(candidate.inputIngressRole, "first/Tensor/Rank/1");
  BOOST_CHECK_EQUAL(candidate.resultEgressRole, "last/Tensor/Rank/0");
  split["input_ingress_role"] = "last";
  BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(HybridGenerationUsesDeclaredEgressAndActualTokenInputs)
{
  for (const bool mirrored : {false, true}) {
    const auto fixture = hybridCatalogFixture("fixture", "onnxruntime", mirrored);
    NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
      [] {}, 8U << 20, 8U << 20};
    const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(fixture.first),
                                                   fixture.second, control);
    const auto& candidate = loaded.planningCache->candidates.front().candidate;
    const auto& plan = candidate.executionPlan;
    const auto topology = nativeGenerationFeedbackTopology(plan, candidate.resultEgressRole,
                                                            "input_ids");
    BOOST_CHECK_EQUAL(topology.producerRole, candidate.resultEgressRole);
    BOOST_CHECK(topology.consumerRoles == plan.stageLayout.front().unitRoleIds);
    BOOST_CHECK_EQUAL(topology.consumerRoles.size(), mirrored ? 1u : 2u);
    BOOST_CHECK(std::find(topology.consumerRoles.begin(), topology.consumerRoles.end(),
                          topology.producerRole) == topology.consumerRoles.end());
    if (!mirrored) {
      BOOST_CHECK(std::none_of(plan.steps.begin(), plan.steps.end(), [&] (const auto& step) {
        return step.unitId == topology.producerRole &&
          step.artifactEntry.find("/HEAD/") != std::string::npos;
      }));
    }
    auto opaqueEntries = plan;
    std::size_t entryIndex = 0;
    for (auto& step : opaqueEntries.steps)
      if (step.operation == "CALL_SUBGRAPH")
        step.artifactEntry = "opaque-entry-" + std::to_string(entryIndex++);
    const auto opaque = nativeGenerationFeedbackTopology(opaqueEntries,
      candidate.resultEgressRole, "input_ids");
    BOOST_CHECK(opaque.consumerRoles == topology.consumerRoles);
    BOOST_CHECK_EQUAL(opaque.producerRole, topology.producerRole);
    auto customToken = opaqueEntries;
    for (auto& step : customToken.steps)
      for (auto& slot : step.inputSlots)
        if (slot.source == "EPOCH_INPUT" && slot.slotId == "input_ids") slot.slotId = "tokens";
    const auto custom = nativeGenerationFeedbackTopology(customToken,
      candidate.resultEgressRole, "tokens");
    BOOST_CHECK(custom.consumerRoles == topology.consumerRoles);
    // Topology-only variant: a downstream unit that declares a token port needs feedback too.
    auto downstreamToken = plan;
    NativeStepSlotSpec token;
    for (const auto& step : plan.steps)
      for (const auto& slot : step.inputSlots)
        if (slot.source == "EPOCH_INPUT" && slot.slotId == "input_ids") token = slot;
    BOOST_REQUIRE_EQUAL(token.slotId, "input_ids");
    for (auto& step : downstreamToken.steps) {
      if (step.unitId != topology.producerRole) continue;
      step.inputSlots.push_back(token);
      break;
    }
    const auto downstream = nativeGenerationFeedbackTopology(downstreamToken,
      candidate.resultEgressRole, "input_ids");
    BOOST_CHECK_EQUAL(downstream.consumerRoles.size(), topology.consumerRoles.size() + 1);
    BOOST_CHECK(std::find(downstream.consumerRoles.begin(), downstream.consumerRoles.end(),
                          downstream.producerRole) != downstream.consumerRoles.end());
    BOOST_CHECK_THROW(nativeGenerationFeedbackTopology(plan, "foreign-unit", "input_ids"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(nativeGenerationFeedbackTopology(plan, candidate.resultEgressRole,
                                                        "absent-token"), std::invalid_argument);
    BOOST_CHECK_THROW(nativeGenerationFeedbackTopology(plan, candidate.resultEgressRole, ""),
                      std::invalid_argument);
    auto incomplete = plan;
    incomplete.steps.erase(std::remove_if(incomplete.steps.begin(), incomplete.steps.end(),
      [&] (const auto& step) { return step.unitId == candidate.resultEgressRole; }),
      incomplete.steps.end());
    BOOST_CHECK_THROW(nativeGenerationFeedbackTopology(incomplete, candidate.resultEgressRole,
                                                        "input_ids"), std::invalid_argument);
  }
}

namespace {

/** Generation contract codec only; no endpoint/grant or network execution is implied. */
NativeSelectionProjectionV3
verifyHybridGenerationCodec(NativeSelectionProjectionV3 projection,
                             const qwen::QwenTpShape& shape, const std::string& egress)
{
  const auto topology = nativeGenerationFeedbackTopology(projection.plan, egress, "input_ids");
  bindNativeGenerationFeedback(projection.plan, topology, "input_ids", projection.requestId,
                                projection.attempt, projection.fixedProviderByRole);
  qwen::QwenTpSealedGeneration sealed;
  sealed.maxGeneratedTokens = 3;
  sealed.samplingDigest = nativePlanningDigest("fixture-sampling");
  sealed.tokenizerDigest = nativePlanningDigest("fixture-tokenizer");
  sealed.eosTokenIds = {63};
  sealed.generationId = std::string(32, 'a');
  projection.generationContract = qwen::buildQwenTpGenerationContract(projection.plan,
                                                                     shape, sealed);
  std::istringstream fullWire(nativeSelectionProjectionV3ToJson(projection));
  const auto cold = nativeSelectionProjectionV3FromJson(fullWire,
    projection.executionRole.roleId);
  BOOST_CHECK(cold.generationContract.enabled);
  BOOST_CHECK_EQUAL(cold.plan.stepPlanVersion, NATIVE_STEP_PLAN_VERSION_V2);
  BOOST_CHECK_EQUAL(cold.plan.steps.size(), projection.plan.steps.size());
  BOOST_CHECK_EQUAL(cold.plan.stageLayout.size(), projection.plan.stageLayout.size());
  BOOST_CHECK_EQUAL(cold.plan.roles.size(), projection.plan.roles.size());
  BOOST_CHECK(std::set<std::string>(cold.plan.roles.begin(), cold.plan.roles.end()) ==
              std::set<std::string>(projection.plan.roles.begin(), projection.plan.roles.end()));
  BOOST_CHECK_EQUAL(nativeCanonicalJson(nativeV2StepTemplateJson(cold.plan)),
                    nativeCanonicalJson(nativeV2StepTemplateJson(projection.plan)));
  BOOST_CHECK(cold.generationContract.stateInputNames ==
              projection.generationContract.stateInputNames);
  BOOST_CHECK_EQUAL(cold.generationContract.stateSuccessorMap,
                    projection.generationContract.stateSuccessorMap);
  BOOST_CHECK_EQUAL(cold.plan.degreeForRole(cold.executionRole.roleId),
                    projection.plan.degreeForRole(projection.executionRole.roleId));
  BOOST_CHECK_NO_THROW(cold.plan.validateSteps());
  BOOST_CHECK(cold.providerOwnership().empty());
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromProjection(cold), std::invalid_argument);
  // Full Selection does not carry the complete assignment. Fixed construction also needs
  // the caller's admitted ownership context; this fixture supplies it separately.
  auto fixedSource = cold;
  fixedSource.fixedProviderByRole = projection.providerOwnership();
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(fixedSource);
  std::istringstream fixedWire(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restored = nativeFixedRoleAssignmentV1FromJson(fixedWire);
  BOOST_REQUIRE(restored);
  std::istringstream invocation(nativeSelectionInvocationV1ToJson(projection, *fixed));
  const auto decoded = nativeSelectionInvocationV1FromJson(invocation,
    projection.executionRole.roleId, *restored);
  BOOST_CHECK(decoded.generationContract.enabled);
  BOOST_CHECK_EQUAL(decoded.generationContract.tokenInputName, "input_ids");
  BOOST_CHECK(decoded.generationContract.stateInputNames ==
              projection.generationContract.stateInputNames);
  BOOST_CHECK_EQUAL(decoded.generationContract.stateSuccessorMap,
                    projection.generationContract.stateSuccessorMap);
  BOOST_CHECK_EQUAL(decoded.plan.roles.size(), projection.plan.roles.size());
  BOOST_CHECK_EQUAL(decoded.plan.steps.size(), projection.plan.steps.size());
  BOOST_CHECK_EQUAL(decoded.plan.stageLayout.size(), projection.plan.stageLayout.size());
  BOOST_CHECK_EQUAL(decoded.plan.degreeForRole(projection.executionRole.roleId),
                    projection.plan.degreeForRole(projection.executionRole.roleId));
  BOOST_CHECK_NO_THROW(decoded.plan.validateSteps());
  std::set<std::string> consumers;
  for (const auto& edge : decoded.plan.dependencies) {
    if (edge.operationKind != "TOKEN_FEEDBACK") continue;
    BOOST_REQUIRE_EQUAL(edge.producers.size(), 1u);
    BOOST_REQUIRE_EQUAL(edge.consumers.size(), 1u);
    BOOST_CHECK_EQUAL(edge.producers.front(), egress);
    BOOST_CHECK_EQUAL(edge.collectiveProducerRank, "0");
    BOOST_CHECK(consumers.insert(edge.consumers.front()).second);
  }
  BOOST_CHECK(consumers == std::set<std::string>(topology.consumerRoles.begin(),
                                                topology.consumerRoles.end()));
  if (topology.consumerRoles.size() > 1) {
    auto missingTemplate = nativeParseJson(nativeSelectionProjectionV3ToJson(projection));
    missingTemplate.erase("step_template_v2");
    std::istringstream missingWire(nativeCanonicalJson(missingTemplate));
    BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(missingWire,
      projection.executionRole.roleId), std::invalid_argument);
  }
  auto missing = projection;
  missing.plan.dependencies.pop_back();
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromProjection(missing), std::invalid_argument);
  auto wrongConsumer = projection;
  wrongConsumer.plan.dependencies.back().consumers = {egress};
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromProjection(wrongConsumer),
                    std::invalid_argument);
  if (topology.consumerRoles.size() > 1) {
    auto duplicate = projection;
    duplicate.plan.dependencies.back().consumers = {topology.consumerRoles.front()};
    BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromProjection(duplicate),
                      std::invalid_argument);
  }
  auto wrongOwnerFlag = projection;
  wrongOwnerFlag.dataflow.terminalResponseOwner = !wrongOwnerFlag.dataflow.terminalResponseOwner;
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromProjection(wrongOwnerFlag),
                    std::invalid_argument);
  return cold;
}

void
verifyHybridMaterials(bool mirrored, bool externalControls = false, bool coordinator = false,
                      CoordinatorFault fault = CoordinatorFault::NONE,
                      const std::vector<std::uint64_t>& degrees = {},
                      bool finalizeCheckpoint = false,
                      const std::vector<std::uint64_t>& firstRankWeights = {},
                      const std::vector<std::uint64_t>& expectedKvEnds = {},
                      bool receivingStageReduction = false)
{
  // These projections exercise post-admission assembly, not Selection or transport security.
  if (const auto* childPath = std::getenv("NDNSF_SPEC242_CACHE_REOPEN")) {
    BOOST_REQUIRE_LE(std::filesystem::file_size(childPath), 1U << 20);
    std::ifstream input(childPath);
    BOOST_REQUIRE(static_cast<bool>(input));
    const auto data = nativeParseJson(std::string(std::istreambuf_iterator<char>(input), {}));
    BOOST_REQUIRE_EQUAL(data.at("schema").get<std::string>(), "spec242-cache-reopen-v1");
    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = data.at("cache").get<std::string>();
    options.providerIdentity = "/fixture/provider";
    options.verifyCachedArtifactDigest = true;
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-fresh-cache");
    Ort::SessionOptions sessionOptions;
    sessionOptions.SetIntraOpNumThreads(1);
    BOOST_REQUIRE_EQUAL(data.at("requests").size(), data.value("unit_count", std::size_t(3)));
    for (const auto& request : data.at("requests")) {
      std::istringstream fixedWire(request.at("fixed").get<std::string>());
      const auto fixed = nativeFixedRoleAssignmentV1FromJson(fixedWire);
      std::istringstream wire(request.at("wire").get<std::string>());
      auto projection = nativeSelectionInvocationV1FromJson(wire,
        request.at("role").get<std::string>(), *fixed);
      BOOST_CHECK_EQUAL(nativeSelectionInvocationV1ToJson(projection, *fixed),
                        request.at("wire").get<std::string>());
      BOOST_REQUIRE_EQUAL(projection.plan.steps.size(),
                          request.at("step_count").get<std::size_t>());
      BOOST_REQUIRE_EQUAL(projection.plan.stageLayout.size(),
                          data.value("stage_count", std::size_t(2)));
      BOOST_CHECK_EQUAL(projection.plan.degreeForRole(projection.executionRole.roleId),
                        request.at("degree").get<std::uint64_t>());
      // This codec/cache probe supplies its state contract explicitly after wire decoding;
      // it does not qualify generation Selection or authorization.
      projection.generationContract.enabled = true;
      projection.generationContract.mode = "TOKEN_STREAMING";
      projection.generationContract.maxGeneratedTokens = 3;
      projection.generationContract.streamingOperationStride = projection.plan.dependencies.size();
      qwen::bindQwenTpGenerationState(projection.plan, projection.generationContract);
      // No catalog, canonical source, fetch port or parent cache owner exists in this process.
      const auto reopened = tryLoadNativeCanonicalOnnxRoleFromCache(projection, options);
      BOOST_REQUIRE(reopened);
      BOOST_CHECK_EQUAL(reopened->path, request.at("path").get<std::string>());
      std::vector<std::filesystem::path> graphs;
      if (request.at("degree").get<std::uint64_t>() == 1) {
        BOOST_REQUIRE(reopened->entryContract);
        BOOST_CHECK_EQUAL(reopened->entryContract->entryId,
                          request.at("entry").get<std::string>());
        BOOST_CHECK_EQUAL(reopened->backend, projection.assemblyContract().backend);
        graphs.emplace_back(reopened->path);
      }
      else {
        BOOST_CHECK_EQUAL(reopened->metadata.at("tensorParallelEntries"),
                          request.at("entries").get<std::string>());
        for (const auto& entry : qwen::parseQwenTpEntryTable(
               reopened->metadata.at("tensorParallelEntries")))
          graphs.push_back(std::filesystem::path(reopened->path).parent_path() /
                            entry.relativeGraphPath);
      }
      for (const auto& path : graphs) {
        Ort::Session session(env, path.c_str(), sessionOptions);
        BOOST_CHECK_GT(session.GetInputCount(), 0u);
        BOOST_CHECK_GT(session.GetOutputCount(), 0u);
      }
      auto foreign = projection;
      // Invocation decoding pins an immutable role; public compatibility copies are not
      // authoritative. Give this cache-key negative its own complete role allocation.
      auto foreignRole = projection.assemblyContract();
      foreignRole.backendAbi = "foreign-fixture-abi";
      foreignRole.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(foreignRole));
      foreign.fixedRoleOwner = std::make_shared<const NativeSelectionRoleV3>(std::move(foreignRole));
      BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(foreign, options));
    }
    BOOST_TEST_MESSAGE("fresh-process hybrid cache reopened " << data.at("requests").size()
                       << " roles");
    return;
  }
  auto fixture = hybridCatalogFixture("plaintext-v1", "onnxruntime-cpu", mirrored,
                                      externalControls, degrees, finalizeCheckpoint);
  auto& configuration = fixture.first;
  const auto& source = fixture.second;
  if (!firstRankWeights.empty())
    configuration["splitter"]["tensor_rank_weights_by_role"] = {{"stage-0", firstRankWeights}};
  const auto root = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", configuration.at("recipe").at("artifact_profile_digest")},
    {"modelIdentityDigest", configuration.at("model").at("content_digest")},
    {"modelName", "qwen-fixture"},
    {"metadata", {{"canonicalSourceDataName", "/fixture/source"},
      {"canonicalSourceDigest", configuration.at("source").at("digest")},
      {"canonicalSourceBytes", source.modelBytes.size()}}}});
  configuration["source"]["model_manifest_digest"] = nativePlanningDigest(root);
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 8U << 20};
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source,
                                                 control);
  const auto& prepared = loaded.planningCache->candidates.front();
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> expectedKvRanges;
  if (!firstRankWeights.empty()) {
    const auto capabilities = nativePlanRequiredCapabilities(prepared.candidate);
    BOOST_CHECK(std::find(capabilities.begin(), capabilities.end(),
                          NATIVE_TENSOR_SHARD_CAPABILITY_V1) != capabilities.end());
    const auto verifiedSource = loaded.preparation->sourceRefFor(loaded.model.descriptor);
    BOOST_REQUIRE(verifiedSource);
    BOOST_REQUIRE(verifiedSource->materialManifest);
    NativeCanonicalSource reference;
    reference.preparedMetadataJson = verifiedSource->preparedMetadataJson;
    auto references = std::make_shared<NativeCanonicalSource::MaterialManifest>(
      *verifiedSource->materialManifest);
    references->payloads.clear();
    references->payloadsComplete = false;
    references->validate();
    reference.materialManifest = std::move(references);
    const auto restored = NativeRequestCatalog::load(nativeCanonicalJson(configuration),
                                                     std::move(reference), control);
    BOOST_REQUIRE(restored.planningCache);
    BOOST_REQUIRE_EQUAL(restored.planningCache->candidates.size(), 1U);
    const auto& warm = restored.planningCache->candidates.front();
    BOOST_CHECK_EQUAL(warm.candidate.candidateDigest, prepared.candidate.candidateDigest);
    BOOST_REQUIRE_EQUAL(warm.roles.size(), prepared.roles.size());
    for (std::size_t i = 0; i < warm.roles.size(); ++i) {
      BOOST_CHECK_EQUAL(warm.roles[i].recipeDigest, prepared.roles[i].recipeDigest);
      BOOST_CHECK_EQUAL(warm.roles[i].tensorShardV1, prepared.roles[i].tensorShardV1);
    }
    const auto& units = prepared.candidate.executionPlan.stageLayout.front().unitRoleIds;
    BOOST_REQUIRE_EQUAL(units.size(), expectedKvEnds.size());
    std::uint64_t begin = 0;
    for (std::size_t rank = 0; rank < units.size(); ++rank) {
      expectedKvRanges.emplace(units[rank], std::make_pair(begin, expectedKvEnds[rank]));
      begin = expectedKvEnds[rank];
    }
  }
  std::size_t expectedUnits = 0;
  for (const auto& degree : configuration.at("splitter").at("tensor_degrees"))
    expectedUnits += degree.get<std::size_t>();
  const auto expectedStages = configuration.at("splitter").at("roles").size();
  BOOST_REQUIRE_EQUAL(prepared.roles.size(), expectedUnits);
  BOOST_REQUIRE_EQUAL(prepared.candidate.executionPlan.stageLayout.size(), expectedStages);
  if (externalControls) {
    std::size_t remoteControls = 0;
    for (const auto& step : prepared.candidate.executionPlan.steps) {
      for (const auto& slot : step.inputSlots) {
        if (slot.slotId != "control.bias" && slot.slotId != "control.offsets") continue;
        BOOST_CHECK_EQUAL(slot.dtype, slot.slotId == "control.bias" ? "float32" : "int64");
        BOOST_CHECK_EQUAL(slot.shape.size(), slot.slotId == "control.bias" ? 0u : 2u);
        if (slot.source != "REMOTE_STEP") continue;
        ++remoteControls;
        BOOST_CHECK(!slot.producerStepId.empty());
        BOOST_CHECK_EQUAL(slot.producerSlotId, slot.slotId);
      }
    }
    BOOST_CHECK_GE(remoteControls, 2u);
  }
  HybridScratch workspace("spec242-material-cache");
  const auto sourcePath = workspace.root / "source.onnx";
  {
    std::ofstream output(sourcePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(source.modelBytes.data()),
                  static_cast<std::streamsize>(source.modelBytes.size()));
    BOOST_REQUIRE(static_cast<bool>(output));
  }
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = workspace.root.string();
  options.providerIdentity = "/fixture/provider";
  options.workerLocation = resolveNativeOnnxWorkerLocation();
  BOOST_REQUIRE(!options.workerLocation.path.empty());
  BOOST_REQUIRE(!options.workerLocation.sha256.empty());
  BOOST_TEST_MESSAGE("assembly worker=" << options.workerLocation.path << " "
                     << options.workerLocation.sha256);
  std::size_t signatures = 0;
  options.signManifest = [&] (const std::string&) {
    ++signatures;
    return std::string("fixture-manifest-signature");
  };
  std::size_t sourceFetches = 0;
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
    BOOST_REQUIRE(name == ndn::Name("/fixture/root"));
    return ndn::Buffer(root.begin(), root.end());
  };
  fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    BOOST_REQUIRE(name == ndn::Name("/fixture/source"));
    BOOST_REQUIRE(service == ndn::Name("/fixture/service"));
    ++sourceFetches;
    return ndn::Buffer(source.modelBytes.begin(), source.modelBytes.end());
  };
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-material-cache");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);
  NativeJson reopenData{{"schema", "spec242-cache-reopen-v1"},
    {"cache", options.cacheDir}, {"requests", NativeJson::array()},
    {"unit_count", expectedUnits}, {"stage_count", expectedStages}};
  std::map<std::string, NativeModelRunnerSpec> coldSpecs;
  std::map<std::string, NativeModelRunnerSpec> cachedSpecs;
  const auto shape = qwen::deriveQwenTpShape(source.modelBytes, 4);
  if (!degrees.empty()) {
    BOOST_CHECK_EQUAL(shape.hidden, 64u);
    BOOST_CHECK_EQUAL(shape.queryHeads, 24u);
    BOOST_CHECK_EQUAL(shape.kvHeads, 12u);
    BOOST_CHECK_EQUAL(shape.headDim, 32u);
    BOOST_CHECK_EQUAL(shape.ffn, 384u);
  }
  auto executionPlan = prepared.candidate.executionPlan;
  if (receivingStageReduction) {
    const auto& egress = prepared.candidate.resultEgressRole;
    auto candidate = makeReceivingStageReduction(executionPlan, egress);
    BOOST_REQUIRE(candidate.has_value());
    const auto sum = std::find_if(candidate->steps.begin(), candidate->steps.end(),
      [&](const auto& step) { return step.unitId == egress && step.operation == "SUM"; });
    BOOST_REQUIRE(sum != candidate->steps.end());
    BOOST_REQUIRE_EQUAL(sum->inputSlots.size(), 2U);
    BOOST_CHECK_EQUAL(std::count_if(candidate->dependencies.begin(), candidate->dependencies.end(),
      [&](const auto& edge) {
        return edge.operationKind == "TENSOR_PARTIAL" && edge.consumerStepId == sum->stepId &&
               edge.consumers == std::vector<std::string>{egress};
      }), 2U);
    BOOST_CHECK_EQUAL(candidate->dependencies.size() + 1, executionPlan.dependencies.size());
    BOOST_CHECK_EQUAL(candidate->steps.size() + 1, executionPlan.steps.size());
    executionPlan = std::move(*candidate);
  }
  for (const auto& role : prepared.roles) {
    BOOST_CHECK_EQUAL(!role.tensorShardV1.empty(), expectedKvRanges.count(role.role) != 0);
    auto projection = stageWindowProjection(executionPlan);
    projection.provider = options.providerIdentity;
    projection.fixedProviderByRole.clear();
    for (const auto& unit : prepared.candidate.executionPlan.roles)
      projection.fixedProviderByRole[unit] = projection.provider;
    projection.deviceBinding.provider = projection.provider;
    projection.deviceBinding.role = role.role;
    projection.requestId = "/fixture/material-request";
    projection.attempt = 1;
    projection.planDigest = nativePlanningDigest("spec242-material-plan");
    projection.deadlineMs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) + 120000;
    projection.plan = executionPlan;
    projection.plan.serviceName = "/fixture/service";
    projection.canonicalArtifactName = "/fixture/root";
    projection.selectedRole = projection.assembly = role;
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
    projection.dataflow.terminalResponseOwner =
      role.role == prepared.candidate.resultEgressRole;
    projection.generationContract.enabled = true;
    projection.generationContract.mode = "TOKEN_STREAMING";
    projection.generationContract.maxGeneratedTokens = finalizeCheckpoint ? 32 : 3;
    projection.generationContract.streamingOperationStride = projection.plan.dependencies.size();
    qwen::bindQwenTpGenerationState(projection.plan, projection.generationContract);
    BOOST_TEST_CONTEXT("role=" << role.role) {
      auto assemblyProjection = projection;
      if (!role.tensorShardV1.empty()) {
        assemblyProjection = verifyHybridGenerationCodec(
          projection, shape, prepared.candidate.resultEgressRole);
        // Provider admission supplies the authenticated root and local plan metadata.
        assemblyProjection.canonicalArtifactName = projection.canonicalArtifactName;
        assemblyProjection.plan.serviceName = projection.plan.serviceName;
        assemblyProjection.plan.modelName = projection.plan.modelName;
        assemblyProjection.plan.modelFamily = projection.plan.modelFamily;
        assemblyProjection.plan.modelFormat = projection.plan.modelFormat;
        assemblyProjection.plan.plannerKind = projection.plan.plannerKind;
        auto encoded = nativeParseJson(nativeSelectionProjectionV3ToJson(assemblyProjection));
        BOOST_CHECK_EQUAL(encoded.at("schema").get<std::string>(),
                          NATIVE_TENSOR_SHARD_SELECTION_SCHEMA);
        BOOST_CHECK_EQUAL(assemblyProjection.assemblyContract().tensorShardV1, role.tensorShardV1);
        auto tampered = encoded;
        auto shard = parseNativeTensorShardV1(role.tensorShardV1);
        shard["stage_layout_digest"] = nativePlanningDigest("foreign weighted stage");
        const auto other = nativeCanonicalJson(shard);
        tampered["roles"].at(0)["tensor_shard_v1"] = other;
        if (tampered.contains("assembly"))
          tampered["assembly"]["tensor_shard_v1"] = other;
        std::istringstream misbound(nativeCanonicalJson(tampered));
        BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(misbound, role.selectedRole),
                          std::invalid_argument);
        encoded["schema"] = "ndnsf-di-selection-v3";
        encoded["schema_version"] = 3;
        std::istringstream downgraded(nativeCanonicalJson(encoded));
        BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(downgraded, role.selectedRole),
                          std::invalid_argument);
        encoded["roles"].at(0).erase("tensor_shard_v1");
        if (encoded.contains("assembly"))
          encoded["assembly"].erase("tensor_shard_v1");
        std::istringstream stripped(nativeCanonicalJson(encoded));
        BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(stripped, role.selectedRole),
                          std::invalid_argument);
      }
      const auto cold = prepareNativeCanonicalOnnxRole(fetchers, assemblyProjection, options);
      const auto cacheDir = std::filesystem::path(cold.path).parent_path();
      const auto leaf = role.recipeDigest.substr(7);
      BOOST_CHECK_EQUAL(cacheDir.filename().string(), role.tensorShardV1.empty() ?
        leaf : "tp-shard-v1-" + leaf);
      const auto beforeReopen = sourceFetches;
      const auto reopened = tryLoadNativeCanonicalOnnxRoleFromCache(projection, options);
      BOOST_REQUIRE(reopened);
      if (!role.tensorShardV1.empty()) {
        std::ifstream manifestInput(cacheDir / "manifest.json");
        BOOST_REQUIRE(static_cast<bool>(manifestInput));
        const auto manifest = nativeParseJson(std::string(
          std::istreambuf_iterator<char>(manifestInput), {}));
        BOOST_CHECK_EQUAL(manifest.at("tensorShardV1").get<std::string>(), role.tensorShardV1);
        auto other = projection;
        auto otherShard = parseNativeTensorShardV1(role.tensorShardV1);
        otherShard["stage_layout_digest"] = nativePlanningDigest("other stage layout");
        other.assembly.tensorShardV1 = nativeCanonicalJson(otherShard);
        other.assembly.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(other.assembly));
        other.selectedRole = other.assembly;
        BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(other, options));
        auto stripped = projection;
        stripped.selectedRole.tensorShardV1.clear();
        stripped.assembly.tensorShardV1.clear();
        BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(stripped, options),
                          std::invalid_argument);
        BOOST_CHECK_THROW(prepareNativeCanonicalOnnxRole(fetchers, stripped, options),
                          std::invalid_argument);
        stripped.selectedRole.tensorParallelDegree = stripped.assembly.tensorParallelDegree = 0;
        stripped.selectedRole.tensorParallelRank = stripped.assembly.tensorParallelRank = 0;
        BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(stripped, options),
                          std::invalid_argument);
      }
      BOOST_CHECK_EQUAL(sourceFetches, beforeReopen);
      BOOST_CHECK_EQUAL(reopened->path, cold.path);
      BOOST_CHECK_EQUAL(reopened->backend, cold.backend);
      BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, cold));
      BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, *reopened));
      const bool singleton = projection.plan.degreeForRole(role.role) == 1;
      if (!singleton) {
        BOOST_CHECK_EQUAL(cold.metadata.at("tensorParallelChildBackend"), role.backend);
        BOOST_CHECK_EQUAL(reopened->metadata.at("tensorParallelChildBackend"), role.backend);
        for (const auto* invalid : {"", "unknown", "native-qwen-tensor", "onnxruntime-cuda"}) {
          auto bad = cold;
          if (std::string(invalid).empty()) bad.metadata.erase("tensorParallelChildBackend");
          else bad.metadata["tensorParallelChildBackend"] = invalid;
          const auto rejected = validateNativePreparedRunnerSpec(projection, bad);
          BOOST_REQUIRE(rejected);
          BOOST_CHECK_EQUAL(*rejected, "DI_PROVIDER_ASSEMBLY_CHILD_BACKEND_MISMATCH");
        }
        auto foreignAdapter = projection;
        foreignAdapter.assembly.adapterId = foreignAdapter.selectedRole.adapterId = "foreign";
        const auto rejected = validateNativePreparedRunnerSpec(foreignAdapter, cold);
        BOOST_REQUIRE(rejected);
        BOOST_CHECK_EQUAL(*rejected, "DI_PROVIDER_ASSEMBLY_CHILD_BACKEND_MISMATCH");
        BOOST_CHECK_EQUAL(reopened->metadata.at("tensorParallelEntries"),
                          cold.metadata.at("tensorParallelEntries"));
      }
      coldSpecs.emplace(role.role, cold);
      cachedSpecs.emplace(role.role, *reopened);
      for (auto* specs : {&coldSpecs, &cachedSpecs})
        specs->at(role.role).metadata["providerIdentity"] = projection.provider;
      std::vector<qwen::QwenTpEntryContract> entries;
      if (!singleton)
        entries = qwen::parseQwenTpEntryTable(reopened->metadata.at("tensorParallelEntries"));
      const bool first = role.layerBegin == 0;
      BOOST_REQUIRE_EQUAL(entries.size(), singleton ? 0u :
        2 * (role.layerEnd - role.layerBegin) + (first ? 1 : 0) +
        (role.role == prepared.candidate.resultEgressRole ? 1 : 0));
      std::set<std::string> expectedEntries;
      std::set<std::string> actualEntries;
      for (const auto& step : projection.plan.steps)
        if (step.unitId == role.selectedRole && step.operation == "CALL_SUBGRAPH")
          expectedEntries.insert(step.artifactEntry);
      if (singleton) {
        BOOST_REQUIRE(cold.entryContract);
        BOOST_REQUIRE(reopened->entryContract);
        BOOST_CHECK(*cold.entryContract == *reopened->entryContract);
        BOOST_CHECK_EQUAL(reopened->backend, role.backend);
        BOOST_CHECK_EQUAL(expectedEntries.size(), 1u);
        actualEntries.insert(reopened->entryContract->entryId);
        Ort::Session session(env, reopened->path.c_str(), sessionOptions);
        BOOST_CHECK_GT(session.GetInputCount(), 0u);
        BOOST_CHECK_GT(session.GetOutputCount(), 0u);
      }
      for (const auto& entry : entries) {
        actualEntries.insert(entry.entryId);
        BOOST_CHECK_EQUAL(entry.unitId, role.selectedRole);
        if (entry.kind == "INIT") BOOST_CHECK(first);
        else if (entry.kind == "HEAD") {
          BOOST_CHECK(!first);
          BOOST_CHECK_EQUAL(role.role, prepared.candidate.resultEgressRole);
        }
        else BOOST_CHECK(entry.layer >= role.layerBegin && entry.layer < role.layerEnd);
        const auto path = std::filesystem::path(reopened->path).parent_path() /
          entry.relativeGraphPath;
        BOOST_REQUIRE(std::filesystem::is_regular_file(path));
        Ort::Session session(env, path.c_str(), sessionOptions);
        BOOST_CHECK_GT(session.GetInputCount(), 0u);
        BOOST_CHECK_GT(session.GetOutputCount(), 0u);
      }
      BOOST_CHECK(actualEntries == expectedEntries);
      if (role.tensorShardV1.empty())
        (void)verifyHybridGenerationCodec(projection, shape, prepared.candidate.resultEgressRole);
      auto transportProjection = projection;
      transportProjection.generationContract = {};
      const auto fixed = nativeFixedRoleAssignmentV1FromProjection(transportProjection);
      reopenData["requests"].push_back({{"role", role.selectedRole},
        {"fixed", nativeFixedRoleAssignmentV1ToJson(*fixed)},
        {"wire", nativeSelectionInvocationV1ToJson(transportProjection, *fixed)}, {"path", cold.path},
        {"step_count", projection.plan.steps.size()},
        {"degree", projection.plan.degreeForRole(role.selectedRole)},
        {"entry", singleton ? cold.entryContract->entryId : std::string{}},
        {"entries", singleton ? std::string{} : cold.metadata.at("tensorParallelEntries")}});
    }
  }
  BOOST_CHECK_EQUAL(sourceFetches, expectedUnits);
  BOOST_CHECK_EQUAL(signatures, expectedUnits);
  std::vector<std::vector<qwen::QwenTpUnit>> units;
  std::size_t stageIndex = 0;
  for (const auto& stage : prepared.candidate.executionPlan.stageLayout) {
    std::vector<qwen::QwenTpUnit> members;
    for (std::size_t rank = 0; rank < stage.unitRoleIds.size(); ++rank) {
      const auto& id = stage.unitRoleIds[rank];
      const auto role = std::find_if(prepared.roles.begin(), prepared.roles.end(),
        [&] (const auto& value) { return value.role == id; });
      BOOST_REQUIRE(role != prepared.roles.end());
      const auto& range = configuration.at("splitter").at("layer_ranges").at(stageIndex);
      members.push_back({id, rank, range.at(0).get<std::uint64_t>(),
                        range.at(1).get<std::uint64_t>()});
    }
    units.push_back(std::move(members));
    ++stageIndex;
  }
  for (const auto& item : std::vector<std::pair<std::string,
         const std::map<std::string, NativeModelRunnerSpec>*>>{
           {"cold-material", &coldSpecs}, {"cached-material", &cachedSpecs}}) {
    const auto layoutLabel = degrees.empty() ? std::string(mirrored ? "[1,2]" : "[2,1]")
      : nativeCanonicalJson(NativeJson(degrees));
    verifyHybridExecution(sourcePath.string(),
      executionPlan, shape, units, prepared.candidate.resultEgressRole,
      *item.second, true, item.first + "-" + layoutLabel +
        (externalControls ? "-controls" : "") + (coordinator ? "-coordinator" : "") +
        (receivingStageReduction ? "-receiving-sum" : ""),
      externalControls, coordinator, fault, finalizeCheckpoint, expectedKvRanges);
  }
  if (coordinator) return;
  const auto sidecar = workspace.root / "reopen.json";
  {
    std::ofstream output(sidecar);
    output << nativeCanonicalJson(reopenData);
    BOOST_REQUIRE(static_cast<bool>(output));
  }
  const auto executable = std::filesystem::read_symlink("/proc/self/exe");
  BOOST_REQUIRE(executable.is_absolute());
  const auto status = ndn_service_framework::test::runOwnedTestProcess(executable,
    {executable.string(), "--run_test=DiHybridCut/HybridMaterialsReopenThroughProductionCache",
      "--log_level=message", "--report_level=short"},
    {{"NDNSF_SPEC242_CACHE_REOPEN", sidecar.string()}}, std::chrono::seconds(30));
  BOOST_CHECK_EQUAL(status, 0);
}

} // namespace

BOOST_AUTO_TEST_CASE(HybridMaterialsReopenThroughProductionCache)
{
  verifyHybridMaterials(false);
  if (!std::getenv("NDNSF_SPEC242_CACHE_REOPEN")) {
    verifyHybridMaterials(true);
    verifyHybridMaterials(false, true);
    verifyHybridMaterials(true, true);
  }
}

BOOST_AUTO_TEST_CASE(ThreeUnequalStagesExecuteDegreeThreeFromSource)
{
  verifyHybridMaterials(false, false, false, CoordinatorFault::NONE, {2, 3, 1});
}

BOOST_AUTO_TEST_CASE(WeightedHybridRanksExecuteAndReopenFromSource)
{
  verifyHybridMaterials(false, false, false, CoordinatorFault::NONE, {2, 3, 1}, false,
                        {3, 1}, {9, 12});
  verifyHybridMaterials(false, false, false, CoordinatorFault::NONE, {2, 3, 1}, false,
                        {1, 3}, {3, 12});
}

BOOST_AUTO_TEST_CASE(ThreeWeightedHybridRanksExecuteAndReopenFromSource)
{
  verifyHybridMaterials(false, false, false, CoordinatorFault::NONE, {3, 1, 1}, false,
                        {3, 1, 2}, {6, 8, 12});
}

BOOST_AUTO_TEST_CASE(FourStagesExecuteDegreeFourFromSource)
{
  verifyHybridMaterials(false, false, false, CoordinatorFault::NONE, {1, 1, 1, 4});
}

BOOST_AUTO_TEST_CASE(GenerationBusKeepsEpochNamesAndCancelsReaders)
{
  auto bus = std::make_shared<tests::PartialBus>();
  tests::CancellableBusDependencyIo io(bus);
  DependencyEdge first;
  first.scope = "same-logical-edge";
  first.plannedDataName = "/fixture/data/0";
  TensorBundle value;
  value.payload = {1, 2, 3};
  io.publishOutput("session", first, value);
  BOOST_CHECK(io.prefetchInput("session", first).get().payload == value.payload);
  auto next = first;
  next.plannedDataName = "/fixture/data/1";
  auto waiting = io.prefetchInput("session", next);
  BOOST_CHECK(waiting.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  io.cancelPendingPrefetches();
  BOOST_CHECK_THROW(waiting.get(), std::runtime_error);
  auto late = io.prefetchInput("session", next);
  BOOST_REQUIRE(late.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
  BOOST_CHECK_THROW(late.get(), std::runtime_error);
  io.drainPendingPrefetches();
}

BOOST_AUTO_TEST_CASE(HybridCoordinatorsGenerateFromActualFeedback)
{
  verifyHybridMaterials(false, false, true);
  verifyHybridMaterials(true, false, true);
}

BOOST_AUTO_TEST_CASE(HybridReceiverReducesTheTwoRawStageContributions)
{
  verifyHybridMaterials(false, false, true, CoordinatorFault::NONE, {}, false, {}, {}, true);
}

BOOST_AUTO_TEST_CASE(HybridReceiverCannotFinishWithOneRawContributionMissing)
{
  verifyHybridMaterials(false, false, true, CoordinatorFault::MISSING_CONTRIBUTION,
                        {}, false, {}, {}, true);
}

BOOST_AUTO_TEST_CASE(HybridCoordinatorsFinalizeTheThirtySecondToken)
{
  verifyHybridMaterials(false, false, true, CoordinatorFault::NONE, {}, true);
}

BOOST_AUTO_TEST_CASE(HybridCoordinatorRejectsMissingAndForeignActivationLineage)
{
  verifyHybridMaterials(false, false, true, CoordinatorFault::MISSING);
  verifyHybridMaterials(false, false, true, CoordinatorFault::WRONG_PRODUCER);
}

BOOST_AUTO_TEST_CASE(HybridCoordinatorReleasesPreparedStateAfterPublicationFailure)
{
  verifyHybridMaterials(false, false, true, CoordinatorFault::PUBLICATION_FAILURE);
}

BOOST_AUTO_TEST_CASE(GenericEntryRunsAStatelessMultiportGraphThroughTheWorker)
{
  HybridScratch workspace("spec242-generic-entry");

  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_version(17);
  auto* graph = model.mutable_graph();
  graph->set_name("two-port-arithmetic");
  const auto signature = [] (onnx::ValueInfoProto* value, const std::string& name) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  for (const auto* name : {"a", "b"}) signature(graph->add_input(), name);
  signature(graph->add_output(), "sum-value");
  signature(graph->add_output(), "delta-value");
  for (const auto& item : std::vector<std::pair<std::string, std::string>>{
         {"Add", "sum-value"}, {"Sub", "delta-value"}}) {
    auto* node = graph->add_node();
    node->set_name(item.first);
    node->set_op_type(item.first);
    node->add_input("a");
    node->add_input("b");
    node->add_output(item.second);
  }
  std::string wire;
  BOOST_REQUIRE(model.SerializeToString(&wire));
  const auto path = workspace.root / "entry.onnx";
  {
    std::ofstream output(path, std::ios::binary);
    output.write(wire.data(), static_cast<std::streamsize>(wire.size()));
    BOOST_REQUIRE(static_cast<bool>(output));
  }
  NativeSubgraphEntryContract entry;
  entry.entryId = "arithmetic";
  entry.unitId = "ordinary-unit";
  entry.inputNames = {"a", "b"};
  entry.outputNames = {"sum-value", "delta-value"};
  entry.inputSlotByGraphName = {{"a", "left"}, {"b", "right"}};
  entry.outputSlotByGraphName = {{"sum-value", "total"}, {"delta-value", "difference"}};
  entry.relativeGraphPath = path.filename().string();
  entry.graphDigest = nativePlanningDigest(wire);
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = std::make_shared<NativeEntryRunner>(
    std::vector<NativeSubgraphEntryContract>{entry},
    [&factory] (const NativeModelRunnerSpec& spec) { return factory.create(spec); },
    [&path] (const NativeSubgraphEntryContract& contract) {
      NativeModelRunnerSpec spec;
      spec.role = contract.entryId;
      spec.kind = "onnx";
      spec.backend = "onnxruntime-cpu";
      spec.path = path.string();
      spec.metadata["forceOutputBundle"] = "true";
      for (std::size_t i = 0; i < contract.inputNames.size(); ++i)
        spec.metadata["inputScope." + std::to_string(i)] = contract.inputNames[i];
      for (std::size_t i = 0; i < contract.outputNames.size(); ++i)
        spec.metadata["outputScope." + std::to_string(i)] = contract.outputNames[i];
      return spec;
    });
  NativeModelRunnerSpec namedInputSpec;
  namedInputSpec.role = entry.unitId;
  namedInputSpec.kind = "onnx";
  namedInputSpec.backend = "onnxruntime-cpu";
  namedInputSpec.path = path.string();
  namedInputSpec.metadata["forceOutputBundle"] = "true";
  const auto namedInputRunner = factory.create(namedInputSpec);
  const auto floatTensor = [] (std::string name, const std::array<float, 2>& values) {
    NamedTensor tensor{std::move(name), TensorElementType::Float32, {1, 2},
                       std::vector<std::uint8_t>(sizeof(values))};
    std::memcpy(tensor.payload.data(), values.data(), sizeof(values));
    return tensor;
  };
  const auto checkNames = [&] (bool local, bool exact) {
    RoleExecutionContext context;
    context.sessionId = "named-input";
    context.role = entry.unitId;
    auto tensors = std::make_shared<std::vector<NamedTensor>>();
    tensors->push_back(floatTensor("graph/a", {20, 30}));
    if (exact) tensors->push_back(floatTensor("a", {1, 2}));
    if (local) {
      auto bundle = std::make_shared<LocalTensorBundle>();
      bundle->owner = tensors;
      for (const auto& tensor : *tensors) {
        bundle->tensors.push_back({tensor.name, tensor.elementType, tensor.shape, tensor.payload});
      }
      context.inputsByScope["a"].localTensors = std::move(bundle);
    }
    else {
      context.inputsByScope["a"] = makeEncodedTensorBundle("a", *tensors);
    }
    context.inputsByScope["b"] = makeEncodedTensorBundle("b", {floatTensor("b", {3, -1})});
    const auto outputs = namedInputRunner->run(context);
    bool found = false;
    for (const auto& item : outputs) {
      for (const auto& value : decodeTensorBundleViews(item.second)) {
        if (value.name != "sum-value") continue;
        found = true;
        const auto expected = exact ? std::array<float, 2>{4, 1} : std::array<float, 2>{23, 29};
        BOOST_REQUIRE_EQUAL(value.payload.size(), sizeof(expected));
        std::array<float, 2> actual;
        std::memcpy(actual.data(), value.payload.data(), sizeof(actual));
        BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
      }
    }
    BOOST_CHECK(found);
  };
  checkNames(false, true);
  checkNames(false, false);
  checkNames(true, true);
  checkNames(true, false);
  BOOST_CHECK(!runner->supportsOpaqueStateHandles());
  BOOST_CHECK(!runner->supportsConversationStateTransfer());
  const auto slot = [] (const std::string& name, const std::string& source) {
    NativeStepSlotSpec value;
    value.slotId = name;
    value.dtype = "float32";
    value.shape = {std::int64_t(1), std::int64_t(2)};
    value.layoutDigest = nativePlanningDigest("two-port-layout");
    value.source = source;
    return value;
  };
  auto step = callStep("arithmetic-call", entry.unitId);
  step.artifactEntry = entry.entryId;
  step.inputSlots = {slot("left", "EPOCH_INPUT"), slot("right", "EPOCH_INPUT")};
  step.outputSlots = {slot("total", "LOCAL_STEP"), slot("difference", "LOCAL_STEP")};
  NativeExecutionPlan plan;
  plan.serviceName = "/fixture/arithmetic";
  plan.modelName = "arithmetic";
  plan.roles = {entry.unitId};
  plan.tensorDegree = 1;
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"region", nativePlanningDigest(wire), {entry.unitId}}};
  plan.steps = {step};
  BOOST_REQUIRE_NO_THROW(plan.validateSteps());
  BOOST_CHECK(runner->checkTemplate(plan.steps, entry.unitId).empty());
  auto unknown = step;
  unknown.artifactEntry = "unavailable";
  BOOST_CHECK(!runner->checkTemplate({unknown}, entry.unitId).empty());
  NativeProviderAssignment assignment;
  assignment.providerByRole.emplace(entry.unitId, "/fixture/provider");
  ProviderRoleWorker worker(1, 1, 4, std::chrono::seconds(5));
  auto bus = std::make_shared<tests::PartialBus>();
  auto io = std::make_shared<tests::BusDependencyIo>(bus);
  const auto input = [] (const std::string& name, std::array<float, 2> values) {
    NamedTensor tensor;
    tensor.name = name;
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = {1, 2};
    tensor.payload.resize(sizeof(values));
    std::memcpy(tensor.payload.data(), values.data(), sizeof(values));
    return makeEncodedTensorBundle(name, {tensor});
  };
  const std::map<std::string, TensorBundle> inputs{
    {"left", input("left", {1, 2})}, {"right", input("right", {3, -1})}};
  auto future = worker.executePreparedAsync("generic-entry",
    roleSpecFor(plan, entry.unitId, "generic-entry", assignment, ""), io,
    [runner] { return runner; }, inputs);
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  const auto result = future.get();
  BOOST_REQUIRE_EQUAL(result.outputsByScope.size(), 2u);
  for (const auto& expected :
       std::map<std::string, std::array<float, 2>>{{"total", {4, 1}},
                                                {"difference", {-2, 3}}}) {
    const auto views = decodeTensorBundleViews(result.outputsByScope.at(expected.first));
    BOOST_REQUIRE_EQUAL(views.size(), 1u);
    BOOST_CHECK(views.front().elementType == TensorElementType::Float32);
    BOOST_CHECK(views.front().shape == std::vector<std::int64_t>({1, 2}));
    BOOST_REQUIRE_EQUAL(views.front().payload.size(), sizeof(expected.second));
    std::array<float, 2> values;
    std::memcpy(values.data(), views.front().payload.data(), sizeof(values));
    BOOST_CHECK_EQUAL_COLLECTIONS(values.begin(), values.end(),
                                 expected.second.begin(), expected.second.end());
  }
  BOOST_CHECK_EQUAL(bus->publishedCount(), 0u);
  auto missing = inputs;
  missing.erase("right");
  auto rejected = worker.executePreparedAsync("missing-entry-input",
    roleSpecFor(plan, entry.unitId, "missing-entry-input", assignment, ""), io,
    [runner] { return runner; }, missing);
  BOOST_REQUIRE(rejected.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  BOOST_CHECK_THROW(rejected.get(), std::exception);
  runner->releaseSessionState("generic-entry");
  runner->releaseSessionState("missing-entry-input");

  // The same arithmetic model now traverses real source admission, native cold assembly,
  // cache reconstruction and registry creation. Whole-region slots explicitly name source
  // ports, unlike the alias-mapping dispatcher check above.
  auto wholePlan = plan;
  wholePlan.steps.front().inputSlots = {slot("a", "EPOCH_INPUT"), slot("b", "EPOCH_INPUT")};
  wholePlan.steps.front().outputSlots = {
    slot("sum-value", "LOCAL_STEP"), slot("delta-value", "LOCAL_STEP")};
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  NativeAssemblyControl assemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  auto descriptor = qwenFixtureDescriptor();
  descriptor.modelName = "arithmetic";
  descriptor.contentDigest = nativePlanningDigest(wire);
  descriptor.adapterId = descriptor.adapter.name = "arithmetic";
  descriptor.precision = "fp32";
  descriptor.quantizationSubtype = "none";
  descriptor.adapter.precisions = {"fp32"};
  descriptor.adapter.backends = {"onnxruntime-cpu"};
  descriptor.adapter.tasks = {"arithmetic"};
  const auto inspection = inspectNativeOnnxSourceGraph(source, descriptor, assemblyControl);
  descriptor.graphDigest = inspection.graph.graphDigest;
  const auto profileDigest = nativePlanningDigest("arithmetic-profile");
  const auto root = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", profileDigest},
    {"modelIdentityDigest", descriptor.contentDigest}, {"modelName", descriptor.modelName},
    {"metadata", {{"canonicalSourceDataName", "/fixture/source"},
      {"canonicalSourceDigest", nativePlanningDigest(wire)},
      {"canonicalSourceBytes", wire.size()}}}});
  NativeInspectedModel inspected;
  inspected.descriptor = descriptor;
  inspected.graph = inspection.graph;
  inspected.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
  inspected.canonicalSourceName = "/fixture/source";
  inspected.canonicalSourceDigest = nativePlanningDigest(wire);
  inspected.canonicalSourceBytes = wire.size();
  inspected.canonicalInitializerDigest = inspection.canonicalIdentity.initializerDigest;
  inspected.modelManifestDigest = nativePlanningDigest(root);
  NativeSplitCandidate candidate;
  candidate.model = descriptor;
  candidate.graphDigest = inspected.graph.graphDigest;
  candidate.source = "GENERATED";
  candidate.splitter = {"arithmetic", "1", nativePlanningDigest("arithmetic-split")};
  candidate.executionPlan = wholePlan;
  candidate.inputIngressRole = candidate.resultEgressRole = entry.unitId;
  const auto artifact = nativePlanningDigest("arithmetic-region");
  candidate.artifactsByRole = {{entry.unitId, {artifact}}};
  candidate.fragmentsByRole = {{entry.unitId, artifact}};
  candidate.requirementsByRole = {{entry.unitId,
    {{"onnxruntime-cpu"}, 0, 1U << 20, 0, 1024, 0, 1.0}}};
  for (const auto& node : inspected.graph.nodes) candidate.nodeRoles[node.id] = entry.unitId;
  candidate.candidateDigest = candidate.computedDigest();
  NativeRequestControl requestControl{"/fixture/whole-region", 1, assemblyControl.deadline, {}};
  const NativeRoleRecipeProfile profile{profileDigest, nativePlanningDigest("assembler-v1"),
    "onnxruntime", "fp32", "none", "native", "none", "plaintext-v1",
    assemblyControl.maxSourceBytes, assemblyControl.maxAssembledBytes, 8};
  const NativeCanonicalRolePreparer preparer(inspected, source, profile, assemblyControl);
  NativeStateTensorMapping stateless;
  stateless.inputs[entry.unitId] = {};
  stateless.outputs[entry.unitId] = {};
  const auto bound = preparer.bindStateContracts(inspected, candidate, stateless,
    requestControl, NativeStateContractBinding::SOURCE);
  const auto prepared = preparer.prepare(inspected, bound, requestControl);
  BOOST_REQUIRE_EQUAL(prepared.size(), 1u);
  const auto& role = prepared.front();
  BOOST_CHECK_EQUAL(role.roleKind, "COMPONENT_SET");
  BOOST_CHECK_EQUAL(role.layerEnd, 0u);
  auto projection = stageWindowProjection(wholePlan);
  projection.provider = "/fixture/provider";
  projection.fixedProviderByRole = {{entry.unitId, projection.provider}};
  projection.deviceBinding.provider = projection.provider;
  projection.deviceBinding.role = entry.unitId;
  projection.requestId = requestControl.requestId;
  projection.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 120000;
  projection.canonicalArtifactName = "/fixture/root";
  projection.selectedRole = projection.assembly = role;
  projection.executionRole.roleId = projection.executionRole.stageId = role.role;
  projection.executionRole.rank = role.rank;
  projection.executionRole.layerBegin = role.layerBegin;
  projection.executionRole.layerEnd = role.layerEnd;
  projection.executionRole.backend = role.backend;
  projection.executionRole.adapterId = role.adapterId;
  projection.executionRole.adapterVersion = role.adapterVersion;
  projection.dataflow.role = role.role;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.terminalResponseOwner = true;
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = (workspace.root / "cache").string();
  options.providerIdentity = projection.provider;
  options.workerLocation = resolveNativeOnnxWorkerLocation();
  options.verifyCachedArtifactDigest = true;
  options.signManifest = [] (const std::string&) { return "fixture-signature"; };
  std::size_t fetches = 0;
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&root] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
    if (name != ndn::Name("/fixture/root")) return std::nullopt;
    return ndn::Buffer(root.begin(), root.end());
  };
  fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name&)
    -> std::optional<ndn::Buffer> {
    if (name != ndn::Name("/fixture/source")) return std::nullopt;
    ++fetches;
    return ndn::Buffer(source.modelBytes.begin(), source.modelBytes.end());
  };
  const auto cold = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  const auto beforeReopen = fetches;
  const auto cached = tryLoadNativeCanonicalOnnxRoleFromCache(projection, options);
  BOOST_REQUIRE(cached);
  BOOST_CHECK_EQUAL(fetches, beforeReopen);
  BOOST_CHECK_EQUAL(cold.path, cached->path);
  BOOST_CHECK_EQUAL(cold.backend, "onnxruntime-cpu");
  BOOST_CHECK_EQUAL(cached->backend, cold.backend);
  BOOST_CHECK(cold.metadata.count("tensorParallelEntries") == 0);
  for (const auto& spec : {cold, *cached}) {
    BOOST_REQUIRE(spec.entryContract);
    BOOST_CHECK_EQUAL(spec.entryContract->entryId, entry.entryId);
    BOOST_CHECK_EQUAL(spec.entryContract->inputNames.size(), 2u);
    BOOST_CHECK_EQUAL(spec.entryContract->outputNames.size(), 2u);
    auto materialRunner = factory.create(spec);
    BOOST_REQUIRE(std::dynamic_pointer_cast<NativeEntryRunner>(materialRunner));
    auto executed = worker.executePreparedAsync("whole-region",
      roleSpecFor(wholePlan, entry.unitId, "whole-region", assignment, ""), io,
      [materialRunner] { return materialRunner; },
      {{"a", input("a", {1, 2})}, {"b", input("b", {3, -1})}});
    BOOST_REQUIRE(executed.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    const auto output = executed.get();
    for (const auto& expected :
         std::map<std::string, std::array<float, 2>>{{"sum-value", {4, 1}},
                                                  {"delta-value", {-2, 3}}}) {
      const auto views = decodeTensorBundleViews(output.outputsByScope.at(expected.first));
      BOOST_REQUIRE_EQUAL(views.size(), 1u);
      BOOST_CHECK(views.front().elementType == TensorElementType::Float32);
      BOOST_CHECK(views.front().shape == std::vector<std::int64_t>({1, 2}));
      BOOST_REQUIRE_EQUAL(views.front().payload.size(), sizeof(expected.second));
      std::array<float, 2> values;
      std::memcpy(values.data(), views.front().payload.data(), sizeof(values));
      BOOST_CHECK_EQUAL_COLLECTIONS(values.begin(), values.end(),
                                   expected.second.begin(), expected.second.end());
    }
    materialRunner->releaseSessionState("whole-region");
  }
  auto foreign = cold;
  auto foreignEntry = *cold.entryContract;
  foreignEntry.unitId = "foreign";
  foreign.entryContract = std::make_shared<const NativeSubgraphEntryContract>(foreignEntry);
  BOOST_CHECK_THROW(factory.create(foreign), std::invalid_argument);
  BOOST_CHECK(validateNativePreparedRunnerSpec(projection, foreign).has_value());
  auto missingEntry = cold;
  missingEntry.entryContract.reset();
  BOOST_CHECK(validateNativePreparedRunnerSpec(projection, missingEntry).has_value());
  RegistryNativeModelRunnerFactory nullFactory;
  nullFactory.registerBackend(cold.backend, [] (const NativeModelRunnerSpec&) {
    return std::shared_ptr<NativeModelRunner>{};
  });
  BOOST_CHECK_THROW(nullFactory.create(cold), std::logic_error);

  // Real in-memory template hits must rebind plan-owned entry values without rebuilding
  // the artifact or retaining the previous invocation's entry ID/state map.
  ProviderArtifactCache artifactCache;
  const auto key = nativeProviderArtifactKey(projection, projection.provider,
    inspected.canonicalSourceName, inspected.canonicalSourceDigest);
  std::size_t builds = 0;
  const auto build = [&] (const NativeRequestControl&) {
    ++builds;
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = projection.canonicalArtifactName;
    artifact->ciphertextDigest = cold.metadata.at("assembledModelDigest");
    artifact->formatVersion = "fixture-plaintext";
    artifact->canonicalMetadataJson = root;
    artifact->ciphertextBytes = std::filesystem::file_size(cold.path);
    auto templateSpec = cold;
    // The facade publishes this stable cache descriptor before the cache strips path.
    templateSpec.metadata["assembledCachePath"] = templateSpec.path;
    return ProviderArtifactCache::BuildResult{
      artifact, std::make_shared<const NativeModelRunnerSpec>(std::move(templateSpec)), {}};
  };
  auto firstLease = artifactCache.acquireWithRunner(key, projection, requestControl, build);
  BOOST_REQUIRE(firstLease.runnerSpec());
  BOOST_CHECK(!firstLease.runnerSpec()->entryContract);
  auto reboundProjection = projection;
  reboundProjection.requestId = "/fixture/rebound-entry";
  reboundProjection.dataflow.requestId = reboundProjection.requestId;
  reboundProjection.planDigest = nativePlanningDigest("rebound-entry-plan");
  reboundProjection.dataflow.planDigest = reboundProjection.planDigest;
  reboundProjection.plan.steps.front().artifactEntry = "rebound-arithmetic";
  const auto reboundKey = nativeProviderArtifactKey(reboundProjection, projection.provider,
    inspected.canonicalSourceName, inspected.canonicalSourceDigest);
  BOOST_CHECK_EQUAL(key.canonicalKey(), reboundKey.canonicalKey());
  auto reboundControl = requestControl;
  reboundControl.requestId = reboundProjection.requestId;
  auto hit = artifactCache.acquireWithRunner(reboundKey, reboundProjection, reboundControl, build);
  BOOST_REQUIRE(hit.cacheHit());
  BOOST_CHECK_EQUAL(builds, 1u);
  auto reboundSpec = *hit.runnerSpec();
  reboundSpec.path = reboundSpec.metadata.at("assembledCachePath");
  bindNativeRunnerPreparationContext(reboundSpec, reboundProjection,
    {projection.provider, "fixture-boot", 1234, workspace.root.string()});
  BOOST_REQUIRE(reboundSpec.entryContract);
  BOOST_CHECK_EQUAL(reboundSpec.entryContract->entryId, "rebound-arithmetic");
  BOOST_CHECK_EQUAL(reboundSpec.metadata.at("evidence.createdAtMs"), "1234");
  BOOST_CHECK_EQUAL(cold.entryContract->entryId, entry.entryId);
  BOOST_CHECK(!validateNativePreparedRunnerSpec(reboundProjection, reboundSpec));
  auto reboundRunner = factory.create(reboundSpec);
  auto reboundFuture = worker.executePreparedAsync("rebound-entry",
    roleSpecFor(reboundProjection.plan, entry.unitId, "rebound-entry", assignment, ""), io,
    [reboundRunner] { return reboundRunner; },
    {{"a", input("a", {1, 2})}, {"b", input("b", {3, -1})}});
  BOOST_REQUIRE(reboundFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  const auto reboundResult = reboundFuture.get();
  BOOST_CHECK_EQUAL(reboundResult.outputsByScope.size(), 2u);
  reboundRunner->releaseSessionState("rebound-entry");
  auto unsupported = reboundProjection;
  auto extra = unsupported.plan.steps.front();
  extra.stepId = "another-call";
  extra.artifactEntry = "another-entry";
  extra.outputSlots.front().slotId = "another-output";
  unsupported.plan.steps.push_back(extra);
  BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(reboundSpec, unsupported,
    {projection.provider, "fixture-boot", 1234, workspace.root.string()}), std::invalid_argument);
  BOOST_CHECK(validateNativePreparedRunnerSpec(unsupported, missingEntry).has_value());

  auto statelessGeneration = projection;
  statelessGeneration.generationContract.enabled = true;
  statelessGeneration.generationContract.stateInputNames = {"remote-state"};
  statelessGeneration.generationContract.stateOutputNames = {"remote-successor"};
  statelessGeneration.generationContract.stateSuccessorMap = "remote-state=remote-successor";
  auto statelessSpec = cold;
  statelessSpec.metadata["statefulModel"] = "true";
  statelessSpec.metadata["stateInputNames"] = "stale-input";
  statelessSpec.metadata["stateOutputNames"] = "stale-output";
  bindNativeRunnerPreparationContext(statelessSpec, statelessGeneration,
    {projection.provider, "fixture-boot", 1234, workspace.root.string()});
  BOOST_CHECK(statelessSpec.metadata.count("statefulModel") == 0);
  BOOST_CHECK(statelessSpec.metadata.count("stateInputNames") == 0);
  BOOST_CHECK(!factory.create(statelessSpec)->supportsOpaqueStateHandles());
  auto badPorts = projection;
  badPorts.plan.steps.front().inputSlots.front().dtype = "int64";
  BOOST_CHECK_EXCEPTION(prepareNativeCanonicalOnnxRole(fetchers, badPorts, options),
    std::invalid_argument, [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("whole-region typed port") != std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(VersionTwoSumPreservesSharedMembersAcrossPortAliases)
{
  for (const auto members : {1u, 2u}) {
    NativeExecutionPlan plan;
    plan.roles = {"alias-unit"};
    plan.tensorDegree = 1;
    plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
    plan.stageLayout = {{"alias-stage", nativePlanningDigest("alias-source"), plan.roles}};
    const auto slot = [] (const std::string& name) {
      NativeStepSlotSpec value;
      value.slotId = name;
      value.dtype = "float32";
      value.shape = {std::int64_t(1), std::int64_t(2)};
      value.layoutDigest = nativePlanningDigest("alias-layout");
      value.source = "LOCAL_STEP";
      return value;
    };
    auto call = callStep("alias-call", plan.roles.front());
    call.outputSlots = {slot("left-port"), slot("right-port")};
    NativeExecutionStepSpec sum;
    sum.stepId = "alias-sum";
    sum.unitId = plan.roles.front();
    sum.operation = "SUM";
    sum.inputSlots = {slot("left"), slot("right")};
    for (std::size_t index = 0; index < sum.inputSlots.size(); ++index) {
      sum.inputSlots[index].producerStepId = call.stepId;
      sum.inputSlots[index].producerSlotId = call.outputSlots[index].slotId;
    }
    sum.outputSlots = {slot("merged")};
    plan.steps = {call, sum};
    BOOST_REQUIRE_NO_THROW(plan.validateSteps());
    // Contract-only regression: a backend may map one shared member inventory onto
    // different step ports. This is not the independent ONNX arithmetic qualification below.
    auto runner = makeNativeModelRunner([members] (const RoleExecutionContext&) {
      std::map<std::string, TensorBundle> outputs;
      for (std::size_t side = 0; side < 2; ++side) {
        std::vector<NamedTensor> values;
        for (std::size_t member = 0; member < members; ++member) {
          NamedTensor tensor{"member-" + std::to_string(member),
                             TensorElementType::Float32, {1, 2}, {}};
          const std::array<float, 2> data{static_cast<float>(1 + 2 * side + member),
                                         static_cast<float>(2 + 2 * side + member)};
          tensor.payload.resize(sizeof(data));
          std::memcpy(tensor.payload.data(), data.data(), sizeof(data));
          values.push_back(std::move(tensor));
        }
        const auto name = side == 0 ? "left-port" : "right-port";
        outputs.emplace(name, makeEncodedTensorBundle(name, std::move(values)));
      }
      return outputs;
    });
    NativeProviderAssignment assignment;
    assignment.providerByRole = {{plan.roles.front(), "/fixture/alias-provider"}};
    ProviderRoleWorker worker(1, 1, 4, std::chrono::seconds(5));
    auto bus = std::make_shared<tests::PartialBus>();
    auto io = std::make_shared<tests::BusDependencyIo>(bus);
    auto future = worker.executePreparedAsync("alias-session",
      roleSpecFor(plan, plan.roles.front(), "alias-session", assignment, ""), io,
      [runner] { return runner; }, {});
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
      bus->cancel(std::make_exception_ptr(std::runtime_error("alias fixture timeout")));
    const auto result = future.get();
    const auto values = decodeTensorBundleViews(result.outputsByScope.at("merged"));
    BOOST_REQUIRE_EQUAL(values.size(), members);
    const std::array<std::array<float, 2>, 2> expected{{{4, 6}, {6, 8}}};
    for (std::size_t member = 0; member < members; ++member) {
      BOOST_CHECK_EQUAL(values[member].name, "member-" + std::to_string(member));
      BOOST_REQUIRE_EQUAL(values[member].payload.size(), sizeof(expected[member]));
      std::array<float, 2> data;
      std::memcpy(data.data(), values[member].payload.data(), sizeof(data));
      BOOST_CHECK_EQUAL_COLLECTIONS(data.begin(), data.end(), expected[member].begin(),
                                    expected[member].end());
    }
  }
}

BOOST_AUTO_TEST_CASE(GenericMixedSourceEntriesMatchTheCompleteOriginalGraph)
{
  HybridScratch workspace("spec242-generic-mixed");

  onnx::ModelProto original;
  original.set_ir_version(8);
  original.add_opset_import()->set_version(17);
  auto* graph = original.mutable_graph();
  graph->set_name("mixed-arithmetic");
  const auto signature = [] (onnx::ValueInfoProto* value, const std::string& name) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  for (const auto* name : {"a", "b"}) signature(graph->add_input(), name);
  for (const auto* name : {"result.joined", "result.contrasted"})
    signature(graph->add_output(), name);
  for (const auto& [name, value] : std::map<std::string, float>{{"quarter", 0.25f},
                                                             {"remainder", 0.75f}}) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    tensor->add_dims(1);
    tensor->set_raw_data(std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
  }
  const auto node = [&] (const std::string& operation, const std::string& name,
                         const std::string& left, const std::string& right) {
    auto* operationNode = graph->add_node();
    operationNode->set_name(name);
    operationNode->set_op_type(operation);
    operationNode->add_input(left);
    operationNode->add_input(right);
    operationNode->add_output(name);
    if (name.find("result.") != 0) signature(graph->add_value_info(), name);
  };
  node("Add", "joined", "a", "b");
  node("Sub", "contrasted", "a", "b");
  for (const auto& [prefix, factor] :
       std::map<std::string, std::string>{{"left", "quarter"}, {"right", "remainder"}}) {
    node("Mul", prefix + ".joined", "joined", factor);
    node("Mul", prefix + ".contrasted", "contrasted", factor);
  }
  node("Add", "result.joined", "left.joined", "right.joined");
  node("Add", "result.contrasted", "left.contrasted", "right.contrasted");
  const auto wire = original.SerializeAsString();

  // The oracle runs the complete original, including both final Adds. No material selection
  // or entry descriptor constructs this session or its expected outputs.
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec242-generic-original");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);
  Ort::Session reference(env, wire.data(), wire.size(), sessionOptions);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 2> a{1, 2};
  std::array<float, 2> b{3, -1};
  const std::array<std::int64_t, 2> dimensions{1, 2};
  std::array<Ort::Value, 2> tensors{
    Ort::Value::CreateTensor<float>(memory, a.data(), a.size(), dimensions.data(), 2),
    Ort::Value::CreateTensor<float>(memory, b.data(), b.size(), dimensions.data(), 2)};
  const char* inputNames[] = {"a", "b"};
  const char* outputNames[] = {"result.joined", "result.contrasted"};
  const auto expected = reference.Run(Ort::RunOptions{nullptr}, inputNames, tensors.data(), 2,
                                      outputNames, 2);

  const std::vector<std::string> units{"ordinary", "part-a", "part-b"};
  const auto layout = nativePlanningDigest("generic-pair-layout");
  const auto slot = [&] (const std::string& name, const std::string& source,
                         const std::string& producer = {}, const std::string& port = {}) {
    NativeStepSlotSpec result;
    result.slotId = name;
    result.dtype = "float32";
    result.shape = {std::int64_t(1), std::int64_t(2)};
    result.layoutDigest = layout;
    result.source = source;
    result.producerStepId = producer;
    result.producerSlotId = port;
    return result;
  };
  NativeExecutionPlan plan;
  plan.serviceName = "/fixture/arithmetic";
  plan.modelName = "mixed-arithmetic";
  plan.roles = units;
  plan.tensorDegree = 2;
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  const auto regionDigest = [&] (const std::vector<std::uint64_t>& indices) {
    return nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"source", nativePlanningDigest(wire)}, {"nodes", indices}}));
  };
  plan.stageLayout = {{"front", regionDigest({0, 1}), {units[0]}},
                      {"branches", regionDigest({2, 3, 4, 5, 6, 7}), {units[1], units[2]}}};
  auto front = callStep("front-call", units[0]);
  front.inputSlots = {slot("a", "EPOCH_INPUT"), slot("b", "EPOCH_INPUT")};
  front.outputSlots = {slot("joined", "LOCAL_STEP"), slot("contrasted", "LOCAL_STEP")};
  plan.steps.push_back(front);
  for (std::size_t rank = 0; rank < 2; ++rank) {
    auto call = callStep("branch-" + std::to_string(rank), units[rank + 1]);
    call.inputSlots = {slot("joined", "REMOTE_STEP", front.stepId, "joined"),
                       slot("contrasted", "REMOTE_STEP", front.stepId, "contrasted")};
    const auto prefix = rank == 0 ? "left." : "right.";
    call.outputSlots = {slot(prefix + std::string("joined"), "LOCAL_STEP"),
                        slot(prefix + std::string("contrasted"), "LOCAL_STEP")};
    plan.steps.push_back(std::move(call));
  }
  for (std::size_t rank = 0; rank < 2; ++rank) {
    for (const auto* value : {"joined", "contrasted"}) {
      NativeExecutionStepSpec sum;
      sum.stepId = "sum-" + std::to_string(rank) + "-" + value;
      sum.unitId = units[rank + 1];
      sum.operation = "SUM";
      // The original graph's last two Add nodes become these Worker SUM operations.
      sum.inputSlots = {
        slot("left-input", rank == 0 ? "LOCAL_STEP" : "REMOTE_STEP", "branch-0",
             std::string("left.") + value),
        slot("right-input", rank == 1 ? "LOCAL_STEP" : "REMOTE_STEP", "branch-1",
             std::string("right.") + value)};
      sum.outputSlots = {slot(std::string("result.") + value, "LOCAL_STEP")};
      plan.steps.push_back(std::move(sum));
    }
  }
  for (const auto& step : plan.steps) {
    for (const auto& input : step.inputSlots) {
      if (input.source != "REMOTE_STEP") continue;
      const auto producer = std::find_if(plan.steps.begin(), plan.steps.end(), [&] (const auto& s) {
        return s.stepId == input.producerStepId;
      });
      BOOST_REQUIRE(producer != plan.steps.end());
      NativeDependencySpec edge;
      edge.producers = {producer->unitId};
      edge.consumers = {step.unitId};
      edge.keyScope = "edge-" + std::to_string(plan.dependencies.size());
      edge.dependencyId = edge.keyScope;
      edge.topicPrefix = "/NDNSF/DI/ARITHMETIC";
      edge.objectNameTemplate =
        "{producerProvider}/NDNSF/DI/ARITHMETIC/{sessionId}/{producerRole}/{keyScope}/{sequence}";
      edge.tensors = {input.producerSlotId};
      edge.operationKind = step.operation == "SUM" ? "TENSOR_PARTIAL" : "ACTIVATION";
      edge.producerStepId = input.producerStepId;
      edge.producerSlotId = input.producerSlotId;
      edge.consumerStepId = step.stepId;
      edge.consumerSlotId = input.slotId;
      plan.dependencies.push_back(std::move(edge));
    }
  }
  BOOST_REQUIRE_NO_THROW(plan.validateSteps());

  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(60), [] {}, 1U << 20, 1U << 20};
  auto descriptor = qwenFixtureDescriptor();
  descriptor.modelName = "mixed-arithmetic";
  descriptor.contentDigest = nativePlanningDigest(wire);
  descriptor.adapterId = descriptor.adapter.name = "arithmetic";
  descriptor.precision = "fp32";
  descriptor.quantizationSubtype = "none";
  descriptor.adapter.precisions = {"fp32"};
  descriptor.adapter.backends = {"onnxruntime-cpu"};
  descriptor.adapter.tasks = {"arithmetic"};
  const auto inspection = inspectNativeOnnxSourceGraph(source, descriptor, control);
  descriptor.graphDigest = inspection.graph.graphDigest;
  BOOST_REQUIRE_EQUAL(inspection.canonicalNodeIndices.size(), 8u);
  const auto profileDigest = nativePlanningDigest("mixed-arithmetic-profile");
  const auto root = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", profileDigest},
    {"modelIdentityDigest", descriptor.contentDigest}, {"modelName", descriptor.modelName},
    {"metadata", {{"canonicalSourceDataName", "/fixture/mixed-source"},
      {"canonicalSourceDigest", nativePlanningDigest(wire)},
      {"canonicalSourceBytes", wire.size()}}}});
  NativeInspectedModel inspected;
  inspected.descriptor = descriptor;
  inspected.graph = inspection.graph;
  inspected.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
  inspected.canonicalSourceName = "/fixture/mixed-source";
  inspected.canonicalSourceDigest = nativePlanningDigest(wire);
  inspected.canonicalSourceBytes = wire.size();
  inspected.canonicalInitializerDigest = inspection.canonicalIdentity.initializerDigest;
  inspected.modelManifestDigest = nativePlanningDigest(root);
  NativeSplitCandidate candidate;
  candidate.model = descriptor;
  candidate.graphDigest = inspected.graph.graphDigest;
  candidate.source = "GENERATED";
  candidate.splitter = {"arithmetic", "1", nativePlanningDigest("mixed-arithmetic-lowering")};
  candidate.executionPlan = plan;
  candidate.inputIngressRole = units.front();
  candidate.resultEgressRole = units.back();
  for (const auto& unit : units) {
    const auto artifact = nativePlanningDigest("mixed-material-" + unit);
    candidate.artifactsByRole[unit] = {artifact};
    candidate.fragmentsByRole[unit] = artifact;
    const std::uint64_t weightBytes = unit == units.front() ? 0 : sizeof(float);
    candidate.requirementsByRole[unit] = {
      {"onnxruntime-cpu"}, weightBytes, 1U << 20, 0, 1024, 0, 1.0};
  }
  for (const auto& [id, index] : inspection.canonicalNodeIndices) {
    const auto& unit = units[index < 2 ? 0 : index < 4 ? 1 : 2];
    candidate.nodeRoles[id] = unit;
    if (index < 6) candidate.roleNodeIdsByRole[unit].push_back(id);
  }
  candidate.tensorIdentityByRole = {{units[1], {2, 0}}, {units[2], {2, 1}}};
  candidate.candidateDigest = candidate.computedDigest();
  NativeRequestControl request{"/fixture/generic-mixed", 1, control.deadline, {}};
  const NativeRoleRecipeProfile profile{profileDigest, nativePlanningDigest("assembler-v1"),
    "onnxruntime", "fp32", "none", "native", "none", "plaintext-v1",
    control.maxSourceBytes, control.maxAssembledBytes, 32};
  const NativeCanonicalRolePreparer preparer(inspected, source, profile, control);
  NativeStateTensorMapping stateless;
  for (const auto& unit : units) {
    stateless.inputs[unit] = {};
    stateless.outputs[unit] = {};
  }
  const auto bound = preparer.bindStateContracts(inspected, candidate, stateless, request,
                                                 NativeStateContractBinding::SOURCE);
  const auto prepared = preparer.prepare(inspected, bound, request);
  BOOST_REQUIRE_EQUAL(prepared.size(), 3u);
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  NativeProviderAssignment assignment;
  for (const auto& unit : units) assignment.providerByRole[unit] = "/fixture/provider-" + unit;
  std::map<std::string, NativeModelRunnerSpec> cold;
  std::map<std::string, NativeModelRunnerSpec> cached;
  for (const auto& role : prepared) {
    BOOST_CHECK_EQUAL(role.roleKind, "COMPONENT_SET");
    BOOST_CHECK_EQUAL(role.nodeIndices.size(), 2u);
    auto projection = stageWindowProjection(plan);
    projection.provider = assignment.providerByRole.at(role.role);
    projection.fixedProviderByRole = assignment.providerByRole;
    projection.deviceBinding.provider = projection.provider;
    projection.deviceBinding.role = role.role;
    projection.requestId = request.requestId;
    projection.deadlineMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
      120000;
    projection.canonicalArtifactName = "/fixture/mixed-root";
    projection.selectedRole = projection.assembly = role;
    projection.executionRole.roleId = projection.executionRole.stageId = role.role;
    projection.executionRole.rank = role.rank;
    projection.executionRole.backend = role.backend;
    projection.executionRole.adapterId = role.adapterId;
    projection.executionRole.adapterVersion = role.adapterVersion;
    projection.dataflow.role = role.role;
    projection.dataflow.requestId = projection.requestId;
    projection.dataflow.terminalResponseOwner = role.role == units.back();
    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = (workspace.root / "cache").string();
    options.providerIdentity = projection.provider;
    options.workerLocation = resolveNativeOnnxWorkerLocation();
    options.verifyCachedArtifactDigest = true;
    options.signManifest = [] (const std::string&) { return "fixture-signature"; };
    NativeCanonicalOnnxFetchers fetchers;
    fetchers.getArtifact = [&root] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
      if (name != ndn::Name("/fixture/mixed-root")) return std::nullopt;
      return ndn::Buffer(root.begin(), root.end());
    };
    std::size_t reads = 0;
    fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name&)
      -> std::optional<ndn::Buffer> {
      if (name != ndn::Name("/fixture/mixed-source")) return std::nullopt;
      ++reads;
      return ndn::Buffer(source.modelBytes.begin(), source.modelBytes.end());
    };
    cold[role.role] = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
    const auto cachedRole = tryLoadNativeCanonicalOnnxRoleFromCache(projection, options);
    BOOST_REQUIRE(cachedRole);
    cached[role.role] = *cachedRole;
    BOOST_CHECK_EQUAL(reads, 1u);
    for (const auto* spec : {&cold.at(role.role), &cached.at(role.role)}) {
      BOOST_REQUIRE(spec->entryContract);
      BOOST_CHECK_EQUAL(spec->backend, "onnxruntime-cpu");
      BOOST_CHECK(!validateNativePreparedRunnerSpec(projection, *spec));
    }
    if (role.role != units.front()) {
      BOOST_CHECK_EQUAL(role.tensorParallelDegree, 2u);
      BOOST_CHECK_EQUAL(role.tensorParallelRank, role.role == units[1] ? 0u : 1u);
      auto wrongGeometry = projection;
      wrongGeometry.selectedRole.tensorParallelRank ^= 1;
      wrongGeometry.assembly = wrongGeometry.selectedRole;
      auto spec = cold.at(role.role);
      BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, wrongGeometry,
        {projection.provider, "fixture-boot", 1234, workspace.root.string()}),
        std::invalid_argument);
    }
  }
  const auto input = [] (const std::string& name, const std::array<float, 2>& values) {
    NamedTensor tensor{name, TensorElementType::Float32, {1, 2}, {}};
    tensor.payload.resize(sizeof(values));
    std::memcpy(tensor.payload.data(), values.data(), sizeof(values));
    return makeEncodedTensorBundle(name, {tensor});
  };
  class FaultIo final : public tests::BusDependencyIo
  {
  public:
    FaultIo(std::shared_ptr<tests::PartialBus> bus, std::string fault)
      : tests::BusDependencyIo(std::move(bus))
      , m_fault(std::move(fault))
    {
    }

    void
    publishOutput(const std::string& session, const DependencyEdge& edge,
                  const TensorBundle& bundle) override
    {
      if (edge.producerRole != "part-a" || edge.producerSlotId != "left.joined" ||
          m_fault == "cold" || m_fault == "cached" || m_fault == "layout") {
        tests::BusDependencyIo::publishOutput(session, edge, bundle);
        return;
      }
      if (m_fault == "missing") return;
      std::vector<NamedTensor> copies;
      for (const auto& value : decodeTensorBundleViews(bundle)) {
        copies.push_back({value.name, value.elementType, value.shape,
                          {value.payload.begin(), value.payload.end()}});
      }
      if (m_fault == "name") copies.front().name = "foreign";
      if (m_fault == "extra") {
        auto extra = copies.front();
        extra.name = "extra";
        copies.push_back(std::move(extra));
      }
      if (m_fault == "shape") copies.front().shape = {2, 1};
      if (m_fault == "type") copies.front().elementType = TensorElementType::Int32;
      tests::BusDependencyIo::publishOutput(session, edge,
        makeEncodedTensorBundle(bundle.name, std::move(copies)));
    }

  private:
    std::string m_fault;
  };
  for (const auto* mode : {"cold", "cached", "name", "extra", "shape", "type", "layout",
                           "missing"}) {
    const std::string label = mode;
    const auto& specs = label == "cold" ? cold : cached;
    auto runPlan = plan;
    if (label == "layout") {
      for (auto& step : runPlan.steps)
        if (step.operation == "SUM")
          step.outputSlots.front().layoutDigest = nativePlanningDigest("foreign-layout");
    }
    const auto session = "generic-mixed-" + label;
    ProviderRoleWorker worker(1, 2, 16, std::chrono::seconds(5));
    auto bus = std::make_shared<tests::PartialBus>();
    auto io = std::make_shared<FaultIo>(bus, label);
    std::map<std::string, std::future<ProviderRoleResult>> futures;
    std::map<std::string, std::shared_ptr<NativeModelRunner>> runners;
    for (const auto& unit : units) {
      auto runner = factory.create(specs.at(unit));
      BOOST_REQUIRE(std::dynamic_pointer_cast<NativeEntryRunner>(runner));
      runners[unit] = runner;
      std::map<std::string, TensorBundle> inputs;
      if (unit == units.front()) inputs = {{"a", input("a", a)}, {"b", input("b", b)}};
      futures.emplace(unit, worker.executePreparedAsync(session,
        roleSpecFor(runPlan, unit, session, assignment, ""), io,
        [runner] { return runner; }, inputs));
    }
    std::map<std::string, ProviderRoleResult> results;
    std::string failures;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (auto& [unit, future] : futures) {
      if (future.wait_until(deadline) != std::future_status::ready)
        bus->cancel(std::make_exception_ptr(std::runtime_error("generic mixed fixture timeout")));
      try {
        results.emplace(unit, future.get());
      }
      catch (const std::exception& error) {
        failures += unit + ": " + error.what() + "\n";
        bus->cancel(std::current_exception());
      }
    }
    for (const auto& [unit, runner] : runners) runner->releaseSessionState(session);
    if (label != "cold" && label != "cached") {
      BOOST_CHECK_MESSAGE(!failures.empty(), label << " unexpectedly accepted");
      BOOST_CHECK(results.count(units.back()) == 0);
      if (label != "missing")
        BOOST_CHECK_MESSAGE(failures.find("sealed single-value SUM") != std::string::npos,
                            label << ": " << failures);
      continue;
    }
    BOOST_REQUIRE_MESSAGE(failures.empty(), failures);
    BOOST_CHECK_EQUAL(bus->publishedCount(), 8u);
    for (const auto& unit : {units[1], units[2]}) {
      for (std::size_t index = 0; index < 2; ++index) {
        const auto actual = decodeTensorBundleViews(results.at(unit).outputsByScope.at(
          outputNames[index]));
        BOOST_REQUIRE_EQUAL(actual.size(), 1u);
        BOOST_CHECK_EQUAL(actual.front().name, outputNames[index]);
        BOOST_CHECK(actual.front().shape == std::vector<std::int64_t>({1, 2}));
        BOOST_REQUIRE_EQUAL(actual.front().payload.size(), 2u * sizeof(float));
        std::array<float, 2> values;
        std::memcpy(values.data(), actual.front().payload.data(), sizeof(values));
        const auto* oracle = expected[index].GetTensorData<float>();
        for (std::size_t element = 0; element < values.size(); ++element)
          BOOST_CHECK_SMALL(values[element] - oracle[element], 1e-6f);
      }
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
