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
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
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
BOOST_AUTO_TEST_SUITE_END()
