#ifndef NDNSF_DI_WAN_ENTRY_PLAN_HPP
#define NDNSF_DI_WAN_ENTRY_PLAN_HPP

#include "wan-summed-projection.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di::wan {

/** Translate topologically ordered canonical cuts to existing native steps. */
class EntryPlanBuilder
{
public:
  using Port = NativeAssemblyTensorContractV3;

  EntryPlanBuilder(const onnx::GraphProto& graph, const NativeStageLayout& stage,
                   NativeExecutionPlan& plan, std::map<std::string, OnnxEntryCut>& entries)
    : EntryPlanBuilder(graph, std::vector<NativeStageLayout>{stage}, plan, entries)
  {
    if (stage.degree() != 2)
      throw std::invalid_argument("Wan tensor stage requires two distinct rank roles");
  }

  EntryPlanBuilder(const onnx::GraphProto& graph, std::vector<NativeStageLayout> stages,
                   NativeExecutionPlan& plan, std::map<std::string, OnnxEntryCut>& entries)
    : m_graph(graph)
    , m_plan(plan)
    , m_entries(entries)
  {
    std::set<std::string> stageIds, units;
    if (stages.empty()) throw std::invalid_argument("Wan entry plan requires a stage");
    for (std::size_t index = 0; index < stages.size(); ++index) {
      const auto& stage = stages[index];
      stage.validate();
      if (stage.degree() > 2 || !stageIds.insert(stage.stageId).second)
        throw std::invalid_argument("Wan entry plan has an invalid or duplicate stage");
      for (std::size_t rank = 0; rank < stage.unitRoleIds.size(); ++rank) {
        const auto& unit = stage.unitRoleIds[rank];
        if (!units.insert(unit).second)
          throw std::invalid_argument("Wan entry plan has a duplicate role");
        plan.roles.push_back(unit);
        m_stageByUnit.emplace_back(index, rank);
      }
      plan.tensorDegree = std::max(plan.tensorDegree, stage.degree());
    }
    plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
    plan.stageLayout = std::move(stages);
  }

  void
  call(const std::string& label, std::size_t rank, const OnnxEntryCut& cut)
  {
    addStep(label, rank, cut, false);
  }

  void
  sum(const std::string& label, std::size_t rank,
      const Port& first, const Port& second, const Port& output)
  {
    if (output.dtype != "float32" || first.dtype != output.dtype ||
        second.dtype != output.dtype || first.shape != output.shape ||
        second.shape != output.shape)
      throw std::invalid_argument("Wan SUM ports must agree on FP32 layout");
    addStep(label, rank, {{}, {first, second}, {output}}, true);
  }

private:
  struct ProducedPort
  {
    std::string step;
    NativeStepSlotSpec slot;
    bool reduction;
  };

  static NativeStepSlotSpec
  makeSlot(const Port& port, std::string source)
  {
    NativeJson dims = NativeJson::array();
    for (const auto& dim : port.shape)
      std::visit([&] (const auto& value) { dims.push_back(value); }, dim);
    return {port.name, port.dtype, port.shape,
            nativePlanningDigest(nativeCanonicalJson(NativeJson{{"dtype", port.dtype},
                                                                 {"shape", dims}})),
            std::move(source), {}, {}};
  }

  void
  checkInput(const Port& port) const
  {
    const auto found = std::find_if(m_graph.input().begin(), m_graph.input().end(),
      [&] (const auto& value) { return value.name() == port.name; });
    if (found == m_graph.input().end())
      throw std::invalid_argument("Wan step has no producer for " + port.name);
    const auto& declared = found->type().tensor_type();
    int type = onnx::TensorProto::UNDEFINED;
    if (port.dtype == "bool") type = onnx::TensorProto::BOOL;
    else if (port.dtype == "float32") type = onnx::TensorProto::FLOAT;
    else if (port.dtype == "int64") type = onnx::TensorProto::INT64;
    if (type == onnx::TensorProto::UNDEFINED || declared.elem_type() != type ||
        !std::equal(declared.shape().dim().begin(), declared.shape().dim().end(),
                    port.shape.begin(), port.shape.end(), [] (const auto& left, const auto& right) {
                      if (const auto* extent = std::get_if<std::int64_t>(&right))
                        return left.has_dim_value() && left.dim_value() == *extent;
                      return left.has_dim_param() && left.dim_param() == std::get<std::string>(right);
                    }))
      throw std::invalid_argument("Wan epoch input contract disagrees: " + port.name);
  }

  void
  addStep(const std::string& label, std::size_t unit, const OnnxEntryCut& cut, bool reduction)
  {
    const auto [stageIndex, rank] = m_stageByUnit.at(unit);
    const auto& stage = m_plan.stageLayout.at(stageIndex);
    NativeExecutionStepSpec step;
    step.stepId = stage.stageId + "/" + label + "/rank-" + std::to_string(rank);
    step.unitId = m_plan.roles.at(unit);
    step.operation = reduction ? "SUM" : "CALL_SUBGRAPH";
    if (!reduction) {
      step.artifactEntry = step.stepId;
      for (const auto node : cut.nodeIndices) {
        if (node >= static_cast<std::uint64_t>(m_graph.node_size()) ||
            !m_ownedNodes.insert(node).second)
          throw std::invalid_argument("Wan entry has an overlapping node selection");
      }
      if (cut.nodeIndices.empty() || !m_entries.emplace(step.artifactEntry, cut).second)
        throw std::invalid_argument("Wan entry is empty or repeated");
    }
    for (const auto& port : cut.inputs) {
      auto slot = makeSlot(port, "EPOCH_INPUT");
      const auto producers = m_produced.find(port.name);
      if (producers == m_produced.end()) {
        checkInput(port);
      }
      else {
        const auto local = producers->second.find(unit);
        if (local == producers->second.end() && producers->second.size() != 1)
          throw std::invalid_argument("Wan step has ambiguous remote producers: " + port.name);
        const auto& [owner, previous] =
          local != producers->second.end() ? *local : *producers->second.begin();
        if (slot.dtype != previous.slot.dtype || slot.shape != previous.slot.shape)
          throw std::invalid_argument("Wan step tensor contract disagrees: " + port.name);
        slot.source = owner == unit ? "LOCAL_STEP" : "REMOTE_STEP";
        slot.producerStepId = previous.step;
        slot.producerSlotId = previous.slot.slotId;
        if (owner != unit) {
          if (previous.reduction)
            throw std::invalid_argument("Wan remote output requires a publishing CALL");
          NativeDependencySpec edge;
          edge.dependencyId = stage.stageId + "/edge-" +
                              std::to_string(m_plan.dependencies.size());
          edge.keyScope = edge.dependencyId;
          edge.topicPrefix = "/NDNSF/DI/WAN";
          edge.objectNameTemplate =
            "{producerProvider}/NDNSF/DI/WAN/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
          edge.producers = {m_plan.roles.at(owner)};
          edge.consumers = {step.unitId};
          edge.tensors = {port.name};
          edge.operationKind = reduction ? "TENSOR_PARTIAL" : "ACTIVATION";
          edge.useNdnsfDataV1 = true;
          edge.producerStepId = previous.step;
          edge.producerSlotId = previous.slot.slotId;
          edge.consumerStepId = step.stepId;
          edge.consumerSlotId = slot.slotId;
          m_plan.dependencies.push_back(std::move(edge));
        }
      }
      step.inputSlots.push_back(std::move(slot));
    }
    for (const auto& port : cut.outputs) {
      auto slot = makeSlot(port, "LOCAL_STEP");
      auto& owners = m_produced[port.name];
      if (owners.count(unit) != 0 ||
          std::any_of(owners.begin(), owners.end(), [&] (const auto& owner) {
            return !reduction || !owner.second.reduction ||
                   m_stageByUnit.at(owner.first).first != stageIndex;
          }))
        throw std::invalid_argument("Wan step tensor port is produced twice: " + port.name);
      owners.emplace(unit, ProducedPort{step.stepId, slot, reduction});
      step.outputSlots.push_back(std::move(slot));
    }
    m_plan.steps.push_back(std::move(step));
  }

  const onnx::GraphProto& m_graph;
  NativeExecutionPlan& m_plan;
  std::map<std::string, OnnxEntryCut>& m_entries;
  std::vector<std::pair<std::size_t, std::size_t>> m_stageByUnit;
  std::map<std::string, std::map<std::size_t, ProducedPort>> m_produced;
  std::set<std::uint64_t> m_ownedNodes;
};

/** Execute the same canonical graph in one Provider for a matched baseline. */
inline OnnxParallelPlan
makeWanSingleProviderPlan(const onnx::GraphProto& graph, const OnnxParallelPlan& ranked,
                          const NativeStageLayout& stage)
{
  stage.validate();
  ranked.plan.validateSteps();
  if (stage.degree() != 1 || graph.node_size() == 0)
    throw std::invalid_argument("Wan single Provider requires one role and a nonempty graph");
  using Port = NativeAssemblyTensorContractV3;
  std::map<std::string, Port> inputs, outputs;
  for (const auto& step : ranked.plan.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source == "EPOCH_INPUT")
        inputs.emplace(slot.slotId, Port{slot.slotId, slot.dtype, slot.shape});
    }
    for (const auto& slot : step.outputSlots)
      outputs.emplace(slot.slotId, Port{slot.slotId, slot.dtype, slot.shape});
  }
  OnnxEntryCut cut;
  for (int i = 0; i < graph.node_size(); ++i) cut.nodeIndices.push_back(i);
  for (const auto& input : graph.input()) cut.inputs.push_back(inputs.at(input.name()));
  for (const auto& output : graph.output()) cut.outputs.push_back(outputs.at(output.name()));
  OnnxParallelPlan result;
  EntryPlanBuilder builder(graph, std::vector<NativeStageLayout>{stage},
                           result.plan, result.entries);
  builder.call("full", 0, cut);
  result.plan.validateSteps();
  return result;
}

/** Prove disjoint graph coverage and assign the cuts in graph order. */
inline OnnxParallelPlan
compileParallelEntries(const onnx::GraphProto& graph, const NativeStageLayout& stage,
                       const std::map<std::string, OnnxEntryCut>& entries,
                       const std::set<std::string>& rankOneEntries,
                       const std::map<std::uint64_t, OnnxReductionCut>& reductions)
{
  OnnxParallelPlan result;
  EntryPlanBuilder planner(graph, stage, result.plan, result.entries);
  std::map<std::uint64_t, std::pair<const std::string&, const OnnxEntryCut&>> calls;
  std::set<std::uint64_t> coverage;
  for (const auto& [id, entry] : entries) {
    if (entry.nodeIndices.empty())
      throw std::invalid_argument("Wan physical entry is empty");
    for (const auto node : entry.nodeIndices) {
      if (node >= static_cast<std::uint64_t>(graph.node_size()) || !coverage.insert(node).second)
        throw std::invalid_argument("Wan physical entry has invalid node coverage");
    }
    const auto last = *std::max_element(entry.nodeIndices.begin(), entry.nodeIndices.end());
    calls.emplace(last, std::pair<const std::string&, const OnnxEntryCut&>(id, entry));
  }
  for (const auto& id : rankOneEntries) {
    if (entries.count(id) == 0)
      throw std::invalid_argument("Wan rank has no physical entry: " + id);
  }
  for (const auto& [node, reduction] : reductions) {
    if (node >= static_cast<std::uint64_t>(graph.node_size()) || !coverage.insert(node).second)
      throw std::invalid_argument("Wan reduction has invalid node coverage");
    const auto& add = graph.node(static_cast<int>(node));
    const auto& [cut, both, firstRank] = reduction;
    if (!add.domain().empty() || add.op_type() != "Add" || add.input_size() != 2 ||
        add.output_size() != 1 || cut.inputs.size() != 2 || cut.outputs.size() != 1 ||
        firstRank >= (both ? 1U : 2U) ||
        add.input(0) != cut.inputs[0].name || add.input(1) != cut.inputs[1].name ||
        add.output(0) != cut.outputs[0].name)
      throw std::invalid_argument("Wan native SUM differs from canonical Add");
  }
  if (coverage.size() != static_cast<std::size_t>(graph.node_size()))
    throw std::invalid_argument("Wan plan leaves canonical nodes unassigned");
  for (std::uint64_t node = 0; node < static_cast<std::uint64_t>(graph.node_size()); ++node) {
    const auto call = calls.find(node);
    if (call != calls.end()) {
      const auto& [id, cut] = call->second;
      planner.call(id, rankOneEntries.count(id), cut);
    }
    const auto reduction = reductions.find(node);
    if (reduction != reductions.end()) {
      const auto& [cut, both, firstRank] = reduction->second;
      const auto id = "sum-" + std::to_string(node);
      for (std::size_t rank = firstRank; rank < firstRank + (both ? 2U : 1U); ++rank)
        planner.sum(id, rank, cut.inputs[0], cut.inputs[1], cut.outputs[0]);
    }
  }
  result.plan.validateSteps();
  return result;
}

/** Coarsen whole regions of an admitted TP graph into ordinary one-unit CALLs.
 * A degree-two region retains its physical CALLs and SUMs. Stage membership is
 * supplied by the component at its legal boundaries.
 */
inline OnnxParallelPlan
makeStagedEntryPlan(const onnx::GraphProto& graph, const OnnxParallelPlan& tensorParallel,
                    const std::array<NativeStageLayout, 2>& stages,
                    const std::vector<std::size_t>& stageByNode)
{
  using Port = NativeAssemblyTensorContractV3;
  const auto& base = tensorParallel.plan;
  base.validateSteps();
  if (base.roles.size() != 2 || stageByNode.size() != static_cast<std::size_t>(graph.node_size()) ||
      (stages[0].degree() != 1 && stages[1].degree() != 1))
    throw std::invalid_argument("invalid Wan PP/Hybrid stage assignment");
  OnnxParallelPlan result;
  EntryPlanBuilder builder(graph, std::vector<NativeStageLayout>(stages.begin(), stages.end()),
                           result.plan, result.entries);
  std::map<std::string, std::size_t> producerNodes;
  for (std::size_t i = 0; i < stageByNode.size(); ++i) {
    if (stageByNode[i] >= stages.size())
      throw std::invalid_argument("Wan node has no stage");
    for (const auto& output : graph.node(static_cast<int>(i)).output()) {
      if (!output.empty() && !producerNodes.emplace(output, i).second)
        throw std::invalid_argument("Wan graph has multiple producers for: " + output);
    }
  }
  struct Region
  {
    std::vector<const NativeExecutionStepSpec*> steps;
    std::map<std::string, Port> inputs;
    std::map<std::string, Port> outputs;
  };
  std::array<Region, 2> regions;
  std::vector<const NativeExecutionStepSpec*> covered(stageByNode.size(), nullptr);
  std::map<std::string, std::size_t> stageByStep;
  for (const auto& step : base.steps) {
    std::vector<std::uint64_t> nodes;
    if (step.operation == "CALL_SUBGRAPH") {
      nodes = tensorParallel.entries.at(step.artifactEntry).nodeIndices;
    }
    else {
      const auto node = producerNodes.at(step.outputSlots.at(0).slotId);
      const auto& canonical = graph.node(static_cast<int>(node));
      if (!canonical.domain().empty() || canonical.op_type() != "Add" ||
          canonical.input_size() != 2 || canonical.output_size() != 1 ||
          step.inputSlots.size() != 2 || step.outputSlots.size() != 1 ||
          canonical.input(0) != step.inputSlots[0].slotId ||
          canonical.input(1) != step.inputSlots[1].slotId)
        throw std::invalid_argument("Wan stage SUM has no corresponding canonical Add");
      nodes = {node};
    }
    const auto region = stageByNode.at(nodes.at(0));
    for (const auto node : nodes) {
      if (stageByNode.at(node) != region)
        throw std::invalid_argument("Wan stage boundary splits a physical entry");
      const auto* previous = covered.at(node);
      if (previous && (step.operation != "SUM" || previous->operation != "SUM" ||
                       step.unitId == previous->unitId))
        throw std::invalid_argument("Wan stage assigns a canonical node twice");
      covered[node] = &step;
    }
    regions[region].steps.push_back(&step);
    stageByStep.emplace(step.stepId, region);
  }
  if (std::find(covered.begin(), covered.end(), nullptr) != covered.end())
    throw std::invalid_argument("Wan stage leaves canonical nodes unassigned");

  const auto port = [] (const NativeStepSlotSpec& slot) {
    return Port{slot.slotId, slot.dtype, slot.shape};
  };
  const auto insert = [] (auto& ports, Port value) {
    const auto [found, added] = ports.try_emplace(value.name, value);
    if (!added && (found->second.dtype != value.dtype || found->second.shape != value.shape))
      throw std::invalid_argument("Wan stage has inconsistent boundary port: " + value.name);
  };
  for (const auto& step : base.steps) {
    const auto current = stageByStep.at(step.stepId);
    for (const auto& slot : step.inputSlots) {
      if (slot.source == "EPOCH_INPUT") {
        insert(regions[current].inputs, port(slot));
        continue;
      }
      const auto upstream = stageByStep.at(slot.producerStepId);
      if (upstream == current) continue;
      if (upstream > current || slot.slotId != slot.producerSlotId)
        throw std::invalid_argument("Wan stage boundary has a reverse or mismatched dependency");
      insert(regions[current].inputs, port(slot));
      insert(regions[upstream].outputs, port(slot));
    }
  }
  for (const auto& output : graph.output()) {
    const auto node = producerNodes.at(output.name());
    const auto& outputs = covered[node]->outputSlots;
    const auto found = std::find_if(outputs.begin(), outputs.end(),
                                    [&] (const auto& slot) { return slot.slotId == output.name(); });
    if (found == outputs.end() || stageByNode[node] != 1)
      throw std::invalid_argument("Wan staged result must be produced by the final stage");
    insert(regions[1].outputs, port(*found));
  }

  std::size_t unitBase = 0;
  for (std::size_t region = 0; region < regions.size(); ++region) {
    const auto& group = regions[region];
    if (group.inputs.empty() || group.outputs.empty() || group.steps.empty())
      throw std::invalid_argument("Wan stage needs inputs, outputs and computation");
    if (stages[region].degree() == 1) {
      OnnxEntryCut combined;
      for (std::size_t node = 0; node < stageByNode.size(); ++node) {
        if (stageByNode[node] == region) combined.nodeIndices.push_back(node);
      }
      for (const auto& [name, value] : group.inputs) combined.inputs.push_back(value);
      for (const auto& [name, value] : group.outputs) combined.outputs.push_back(value);
      builder.call("region", unitBase, combined);
    }
    else {
      for (const auto* step : group.steps) {
        const auto unit = unitBase + (step->unitId == base.roles[0] ? 0 : 1);
        if (step->operation == "CALL_SUBGRAPH") {
          builder.call(step->stepId, unit, tensorParallel.entries.at(step->artifactEntry));
        }
        else {
          builder.sum(step->stepId, unit, port(step->inputSlots[0]), port(step->inputSlots[1]),
                      port(step->outputSlots[0]));
        }
      }
    }
    unitBase += stages[region].degree();
  }
  result.plan.validateSteps();
  return result;
}

inline OnnxParallelPlan
makeContiguousEntryPlan(const onnx::GraphProto& graph, const OnnxParallelPlan& tensorParallel,
                        const std::array<NativeStageLayout, 2>& stages, std::size_t cut)
{
  const auto nodes = static_cast<std::size_t>(graph.node_size());
  if (cut == 0 || cut >= nodes) throw std::invalid_argument("Wan cut leaves a stage empty");
  std::vector<std::size_t> stageByNode(nodes, 1);
  std::fill_n(stageByNode.begin(), cut, 0);
  return makeStagedEntryPlan(graph, tensorParallel, stages, stageByNode);
}

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_ENTRY_PLAN_HPP
