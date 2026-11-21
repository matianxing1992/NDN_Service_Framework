#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeSourcePartitionStrategy.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

bool
contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

NativeSourcePartitionStrategy::NativeSourcePartitionStrategy(
  NativeSourcePartitionProfile profile)
  : m_profile(std::move(profile))
{
  if (m_profile.roles.empty())
    throw std::invalid_argument("source partition has no roles");
  const std::set<std::string> roles(m_profile.roles.begin(), m_profile.roles.end());
  if (roles.size() != m_profile.roles.size())
    throw std::invalid_argument("source partition roles are duplicated");
  if (!contains(m_profile.roles, m_profile.inputIngressRole) ||
      !contains(m_profile.roles, m_profile.resultEgressRole))
    throw std::invalid_argument("source partition ingress or egress is undeclared");
  if (m_profile.mode == NativeSourcePartitionMode::FULL && m_profile.roles.size() != 1)
    throw std::invalid_argument("full source partition requires one role");
  if (m_profile.mode == NativeSourcePartitionMode::COMPONENT &&
      (m_profile.roles.size() != 2 || m_profile.boundaryNode.empty()))
    throw std::invalid_argument("component source partition requires two roles and a boundary");
  if (m_profile.mode == NativeSourcePartitionMode::HYBRID &&
      (m_profile.roles.size() != 4 || m_profile.boundaryNode.empty() ||
       m_profile.inputChannels < 2 || m_profile.convInputAxis.empty() ||
       (m_profile.blockEntries.empty() &&
        (m_profile.activationShape.empty() || m_profile.partialShape.empty()))))
    throw std::invalid_argument("hybrid source partition requires four roles, boundary, channels and shapes");
}

NativeStrategyIdentity
NativeSourcePartitionStrategy::identity() const
{
  return {"native-source-partition", "1",
          nativePlanningDigest("native-source-partition|1")};
}

std::vector<NativeSplitCandidate>
NativeSourcePartitionStrategy::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget) const
{
  return enumerateImpl(model, graph, budget, nullptr);
}

std::vector<NativeSplitCandidate>
NativeSourcePartitionStrategy::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl& control) const
{
  control.requireActive();
  auto result = enumerateImpl(model, graph, budget, &control);
  control.requireActive();
  return result;
}

void
NativeSourcePartitionStrategy::buildBlockSteps(NativeSplitCandidate& candidate,
                                               const NativeGraphSnapshot& graph,
                                               const ExtensionControl* control) const
{
  const auto& roles = m_profile.roles;
  auto& plan = candidate.executionPlan;
  const auto layout = nativePlanningDigest("source-activation-layout-v1");
  struct Value
  {
    std::string role;
    std::string step;
  };
  std::array<std::map<std::string, Value>, 2> values;
  std::array<NativeOnnxSourceEntryProgramV1, 2> programs;
  const auto makeSlot = [&] (const NativeTensorContract& port) {
    NativeStepSlotSpec slot;
    slot.slotId = port.name;
    slot.dtype = port.dtype;
    slot.shape = port.shape;
    slot.layoutDigest = layout;
    slot.source = "LOCAL_STEP";
    return slot;
  };
  const auto connect = [&] (NativeStepSlotSpec& slot, const std::string& role,
                            const std::map<std::string, Value>& available) {
    const auto producer = available.find(slot.slotId);
    if (producer == available.end()) {
      if (std::none_of(graph.modelInputs.begin(), graph.modelInputs.end(), [&] (const auto& input) {
            return input.name == slot.slotId && input.dtype == slot.dtype &&
                   input.shape == slot.shape;
          }))
        throw std::invalid_argument("source block CALL input has no earlier source producer: " +
                                    slot.slotId);
      slot.source = "EPOCH_INPUT";
      return;
    }
    slot.source = producer->second.role == role ? "LOCAL_STEP" : "REMOTE_STEP";
    slot.producerStepId = producer->second.step;
    slot.producerSlotId = slot.slotId;
  };
  const auto outerCall = [&] (std::size_t index, const std::map<std::string, Value>& available) {
    const auto& window = m_profile.outerEntries.at(index);
    const auto& role = index == 0 ? roles.front() : roles.back();
    NativeExecutionStepSpec call;
    call.stepId = role + "/RUN";
    call.unitId = role;
    call.operation = "CALL_SUBGRAPH";
    call.artifactEntry = role + "/artifact";
    for (const auto& port : window.inputs) {
      auto slot = makeSlot(port);
      connect(slot, role, available);
      call.inputSlots.push_back(std::move(slot));
    }
    for (const auto& port : window.outputs) call.outputSlots.push_back(makeSlot(port));
    return call;
  };
  auto prefix = outerCall(0, {});
  for (auto& available : values)
    for (const auto& slot : prefix.outputSlots)
      available.emplace(slot.slotId, Value{prefix.unitId, prefix.stepId});
  plan.steps.push_back(std::move(prefix));

  for (std::size_t index = 0; index < m_profile.blockEntries.size(); ++index) {
    if (control) control->requireActive();
    const auto& window = m_profile.blockEntries[index];
    const auto& recipe = window.recipe.transform;
    const auto label = "/" + std::to_string(index);
    const bool finalFfn = index + 2 == m_profile.blockEntries.size() &&
                          recipe.kind == "ffn-pair-partial";
    const bool finalComponent = index + 1 == m_profile.blockEntries.size();
    const bool stats = recipe.kind == "attention-rms-stat";
    const bool reduce = stats || recipe.kind == "attention-heads" ||
                        recipe.kind == "ffn-pair-partial";
    std::array<std::string, 2> contributions;
    NativeStepSlotSpec partial;
    for (std::size_t rank = 0; rank < (finalComponent ? 1U : 2U); ++rank) {
      const auto& role = roles[rank + 1];
      auto& available = values[rank];
      auto entry = window.recipe;
      entry.entryId = role + "/artifact" + label;
      auto& transform = entry.transform;
      if (transform.extent != 0) {
        const auto range = nativeConvInputChannelRanges(transform.extent, 2)[rank];
        transform.begin = range.first;
        transform.end = range.second;
      }
      NativeExecutionStepSpec call;
      call.stepId = role + "/RUN" + label;
      call.unitId = role;
      call.operation = "CALL_SUBGRAPH";
      call.artifactEntry = entry.entryId;
      const auto localWidth = static_cast<std::int64_t>((transform.end - transform.begin) *
                                                        transform.headWidth);
      for (const auto& port : window.inputs) {
        auto slot = makeSlot(port);
        if ((transform.isRmsNorm() && port.name == transform.input) ||
            (transform.kind == "attention-heads" && contains(transform.attentionPorts, port.name)))
          slot.shape.back() = localWidth;
        connect(slot, role, available);
        call.inputSlots.push_back(std::move(slot));
      }
      if (transform.kind == "attention-rms-normalize") {
        auto global = call.inputSlots.front();
        global.slotId = transform.globalStatistic;
        global.shape.back() = std::int64_t{1};
        connect(global, role, available);
        call.inputSlots.push_back(std::move(global));
      }
      for (const auto& port : window.outputs) {
        auto slot = makeSlot(port);
        if (stats) {
          slot.slotId = transform.statistic;
          slot.shape.back() = std::int64_t{1};
        }
        else if (transform.kind == "attention-qkv" || transform.isRmsNorm()) {
          slot.shape.back() = localWidth;
        }
        available[slot.slotId] = {role, call.stepId};
        call.outputSlots.push_back(std::move(slot));
      }
      if (entry.transform.isAttention()) {
        for (const auto& slot : call.inputSlots) entry.inputPorts.push_back(slot.slotId);
        for (const auto& slot : call.outputSlots) entry.outputPorts.push_back(slot.slotId);
      }
      if (reduce) {
        if (call.outputSlots.size() != 1)
          throw std::invalid_argument("source reduction requires one output port");
        partial = call.outputSlots.front();
        contributions[rank] = call.stepId;
      }
      programs[rank].entries.push_back(std::move(entry));
      plan.steps.push_back(std::move(call));
    }
    if (!reduce) continue;
    for (std::size_t rank = 0; rank < (finalFfn ? 1U : 2U); ++rank) {
      const auto& role = roles[rank + 1];
      NativeExecutionStepSpec sum;
      sum.stepId = role + "/SUM" + label;
      sum.unitId = role;
      sum.operation = "SUM";
      for (std::size_t sender = 0; sender < 2; ++sender) {
        auto operand = partial;
        operand.slotId = "rank" + std::to_string(sender);
        operand.source = sender == rank ? "LOCAL_STEP" : "REMOTE_STEP";
        operand.producerStepId = contributions[sender];
        operand.producerSlotId = partial.slotId;
        sum.inputSlots.push_back(std::move(operand));
      }
      auto result = partial;
      if (stats) result.slotId = recipe.globalStatistic;
      values[rank][result.slotId] = {role, sum.stepId};
      sum.outputSlots.push_back(std::move(result));
      plan.steps.push_back(std::move(sum));
    }
  }
  auto suffix = outerCall(1, values.front());
  if (std::none_of(suffix.inputSlots.begin(), suffix.inputSlots.end(), [&] (const auto& slot) {
        return slot.producerStepId == roles[1] + "/RUN/" +
          std::to_string(m_profile.blockEntries.size() - 1);
      }) ||
      std::none_of(plan.steps.begin(), plan.steps.end(), [&] (const auto& step) {
        return step.unitId == roles[2] &&
          std::any_of(step.inputSlots.begin(), step.inputSlots.end(), [&] (const auto& slot) {
            return slot.producerStepId == roles[0] + "/RUN";
          });
      }))
    throw std::invalid_argument("source block program does not carry both pipeline handoffs");
  plan.steps.push_back(std::move(suffix));
  for (std::size_t rank = 0; rank < programs.size(); ++rank) {
    programs[rank].validate();
    const auto& role = roles[rank + 1];
    candidate.roleNodeIdsByRole[role] = programs[rank].sourceNodeCover();
    candidate.sourceTransformByRole[role] = programs[rank].canonicalJson();
  }
}

std::vector<NativeSplitCandidate>
NativeSourcePartitionStrategy::enumerateImpl(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl* control) const
{
  if (control) control->requireActive();
  budget.validate();
  model.validate();
  graph.validate(model);
  const auto& windows = m_profile.ffnWindows;
  const bool blockProgram = !m_profile.blockEntries.empty();
  const auto* pair = blockProgram || windows.empty() ? nullptr : &windows.front().transform;
  const bool multiEntry = blockProgram || windows.size() > 1;
  const auto& activationTensor = pair ? pair->input : m_profile.convInputTensor;
  const auto& partialTensor = pair ? windows.back().transform.output : m_profile.convOutputTensor;
  const auto& shardAxis = pair ? pair->axis : m_profile.convInputAxis;
  const std::string stageSuffix = blockProgram ? "/blocks" : pair ? "/ffn" : "/conv";
  std::size_t firstRankNode = 0;
  std::size_t lastRankNode = 0;
  std::map<std::string, std::string> ownerByNode;
  if (m_profile.mode == NativeSourcePartitionMode::FULL) {
    for (const auto& node : graph.nodes) {
      if (control) control->requireActive();
      ownerByNode.emplace(node.id, m_profile.roles.front());
    }
  }
  else if (m_profile.mode == NativeSourcePartitionMode::HYBRID) {
    const auto boundary = std::find(graph.topologicalOrder.begin(),
                                    graph.topologicalOrder.end(),
                                    m_profile.boundaryNode);
    if (boundary == graph.topologicalOrder.end())
      throw std::invalid_argument("source partition boundary is not in the graph");
    const auto boundaryNode = std::find_if(graph.nodes.begin(), graph.nodes.end(),
      [&] (const auto& node) { return node.id == m_profile.boundaryNode; });
    if (boundaryNode == graph.nodes.end() ||
        (!blockProgram && boundaryNode->opType != (pair ? "MatMul" : "Conv")))
      throw std::invalid_argument("source hybrid boundary has the wrong operator");
    firstRankNode = static_cast<std::size_t>(
      std::distance(graph.topologicalOrder.begin(), boundary));
    lastRankNode = firstRankNode;
    if (blockProgram) {
      const auto suffix = std::find(std::next(boundary), graph.topologicalOrder.end(),
                                    m_profile.suffixNode);
      if (suffix == graph.topologicalOrder.end())
        throw std::invalid_argument("source block stage has no ordered suffix boundary");
      lastRankNode = static_cast<std::size_t>(suffix - graph.topologicalOrder.begin() - 1);
    }
    if (pair) {
      if (graph.modelInputs.size() != 1 || graph.modelOutputs.size() != 1)
        throw std::invalid_argument("paired FFN source window is incomplete");
      auto cursor = boundary;
      std::map<std::string, std::size_t> windowByNode;
      for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto& window = windows[index];
        for (const auto& node : window.nodes) {
          if (cursor == graph.topologicalOrder.end() || *cursor != node ||
              !windowByNode.emplace(node, index).second)
            throw std::invalid_argument("paired FFN window has incomplete source node cover");
          ++cursor;
        }
      }
      if (windowByNode.empty())
        throw std::invalid_argument("paired FFN source window has no nodes");
      for (const auto& edge : graph.edges) {
        const auto producer = windowByNode.find(edge.producer);
        if (producer == windowByNode.end()) continue;
        for (const auto& name : edge.consumers) {
          const auto consumer = windowByNode.find(name);
          if (consumer == windowByNode.end() || consumer->second == producer->second) continue;
          if (consumer->second != producer->second + 1 ||
              edge.id != windows[producer->second].transform.output ||
              edge.id != windows[consumer->second].transform.input)
            throw std::invalid_argument("paired FFN windows expose an undeclared internal edge");
        }
      }
      lastRankNode = static_cast<std::size_t>(
        std::distance(graph.topologicalOrder.begin(), cursor) - 1);
    }
    if (firstRankNode == 0 || lastRankNode + 1 >= graph.topologicalOrder.size())
      throw std::invalid_argument("source hybrid boundary leaves an empty region");
    for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
      if (control) control->requireActive();
      const auto& node = graph.topologicalOrder[i];
      if (i < firstRankNode) ownerByNode.emplace(node, m_profile.roles[0]);
      else if (i > lastRankNode) ownerByNode.emplace(node, m_profile.roles[3]);
      else {
        ownerByNode.emplace(node, m_profile.roles[1]);
      }
    }
  }
  else {
    const auto boundary = std::find(graph.topologicalOrder.begin(),
                                    graph.topologicalOrder.end(),
                                    m_profile.boundaryNode);
    if (boundary == graph.topologicalOrder.end())
      throw std::invalid_argument("source partition boundary is not in the graph");
    const auto boundaryIndex = static_cast<std::size_t>(
      std::distance(graph.topologicalOrder.begin(), boundary));
    if (boundaryIndex == 0 || boundaryIndex >= graph.topologicalOrder.size())
      throw std::invalid_argument("source partition boundary leaves an empty region");
    for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
      if (control) control->requireActive();
      ownerByNode.emplace(graph.topologicalOrder[i],
                          i < boundaryIndex ? m_profile.roles[0] : m_profile.roles[1]);
    }
  }
  if (ownerByNode.size() != graph.nodes.size())
    throw std::invalid_argument("source partition does not cover the graph");

  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter = identity();
  candidate.model = model;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan.version = 1;
  candidate.executionPlan.modelName = model.modelName;
  candidate.executionPlan.modelFamily = "onnx-source";
  candidate.executionPlan.modelFormat = model.modelFormat;
  candidate.executionPlan.plannerKind = m_profile.mode == NativeSourcePartitionMode::FULL
    ? "native-source-full" : "native-source-component";
  candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
  candidate.executionPlan.roles = m_profile.roles;
  candidate.nodeRoles = ownerByNode;
  candidate.selectionPriority = 0;
  candidate.inputIngressRole = m_profile.inputIngressRole;
  candidate.resultEgressRole = m_profile.resultEgressRole;
  candidate.postprocessingJson = m_profile.postprocessingJson;

  std::string hybridStageDigest;
  if (m_profile.mode == NativeSourcePartitionMode::HYBRID) {
    const auto rankBegin = graph.topologicalOrder.begin() + firstRankNode;
    const auto rankEnd = graph.topologicalOrder.begin() + lastRankNode + 1;
    candidate.roleNodeIdsByRole[m_profile.roles[1]].assign(rankBegin, rankEnd);
    candidate.roleNodeIdsByRole[m_profile.roles[2]].assign(rankBegin, rankEnd);
    for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
      const auto& node = graph.topologicalOrder[i];
      if (i < firstRankNode) candidate.roleNodeIdsByRole[m_profile.roles[0]].push_back(node);
      else if (i > lastRankNode) candidate.roleNodeIdsByRole[m_profile.roles[3]].push_back(node);
    }
    struct CrossingEdge
    {
      const NativeGraphEdge* edge = nullptr;
      std::string producerRole;
      std::string consumerRole;
    };
    // Every tensor that leaves one role region and enters another. V2 binds each
    // one explicitly; a bypass must not be silently dropped just because the
    // planner models the Conv output as a SUM.
    std::vector<CrossingEdge> crossings;
    for (const auto& edge : graph.edges) {
      const auto producer = ownerByNode.find(edge.producer);
      if (producer == ownerByNode.end()) continue;
      for (const auto& consumerNode : edge.consumers) {
        const auto consumer = ownerByNode.find(consumerNode);
        if (consumer == ownerByNode.end() || consumer->second == producer->second) continue;
        if (pair &&
            ((producer->second == m_profile.roles[1] && edge.id != partialTensor) ||
             (consumer->second == m_profile.roles[1] && edge.id != activationTensor)))
          throw std::invalid_argument("paired FFN exposes an undeclared interior tensor");
        crossings.push_back({&edge, producer->second, consumer->second});
      }
    }
    if (pair &&
        (std::none_of(crossings.begin(), crossings.end(), [&] (const auto& edge) {
           return edge.edge->id == activationTensor && edge.producerRole == m_profile.roles[0] &&
                  edge.consumerRole == m_profile.roles[1];
         }) ||
         std::none_of(crossings.begin(), crossings.end(), [&] (const auto& edge) {
           return edge.edge->id == partialTensor && edge.producerRole == m_profile.roles[1] &&
                  edge.consumerRole == m_profile.roles[3];
         })))
      throw std::invalid_argument("paired FFN does not have both source pipeline handoffs");
    const auto half = m_profile.inputChannels / 2 +
                      (pair || blockProgram ? m_profile.inputChannels % 2 : 0);
    hybridStageDigest = nativePlanningDigest("source-hybrid|" + graph.graphDigest + "|" +
                                             m_profile.boundaryNode + "|" +
                                             std::to_string(m_profile.inputChannels) +
                                             (blockProgram ? "|" + m_profile.suffixNode :
                                              pair ? "|" + windows.back().transform.downNode : ""));
    auto first = NativeJson::array();
    first.push_back(NativeJson{{"axis", shardAxis}, {"begin", 0},
                               {"end", half}, {"extent", m_profile.inputChannels}});
    auto second = NativeJson::array();
    second.push_back(NativeJson{{"axis", shardAxis}, {"begin", half},
                                {"end", m_profile.inputChannels},
                                {"extent", m_profile.inputChannels}});
    auto ranked = NativeJson::array();
    ranked.push_back(NativeJson{{"role", m_profile.roles[1]}, {"axes", first}});
    ranked.push_back(NativeJson{{"role", m_profile.roles[2]}, {"axes", second}});
    const auto layoutDigest = nativeTensorStageLayoutDigest(
      model.adapter.descriptorDigest(), graph.graphDigest,
      m_profile.roles[1] + stageSuffix, hybridStageDigest, ranked);
    auto shard0 = NativeJson{{"stage_layout_digest", layoutDigest}, {"axes", first}};
    auto shard1 = NativeJson{{"stage_layout_digest", layoutDigest}, {"axes", second}};
    candidate.tensorIdentityByRole[m_profile.roles[1]] = {
      2, 0, nativeCanonicalJson(shard0)};
    candidate.tensorIdentityByRole[m_profile.roles[2]] = {
      2, 1, nativeCanonicalJson(shard1)};
    candidate.executionPlan.tensorDegree = 2;
    candidate.executionPlan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
    const auto rankTransform = [&] (std::uint64_t rank) {
      if (multiEntry) {
        NativeOnnxSourceEntryProgramV1 program;
        const auto& role = m_profile.roles[rank + 1];
        for (std::size_t i = 0; i < windows.size(); ++i) {
          const auto& window = windows[i];
          auto transform = window.transform;
          const auto range = nativeConvInputChannelRanges(transform.extent, 2)[rank];
          transform.begin = range.first;
          transform.end = range.second;
          program.entries.push_back({role + "/artifact/" + std::to_string(i), window.nodes,
                                     std::move(transform)});
        }
        program.validate();
        return program.canonicalJson();
      }
      NativeOnnxSourceTransformV1 value;
      if (pair) {
        value = *pair;
      }
      else {
        value.kind = "conv-input-channel-partial";
        value.node = m_profile.boundaryNode;
        value.axis = shardAxis;
        value.input = activationTensor;
        value.weight = m_profile.convWeightInitializer;
        value.output = partialTensor;
        value.bias = m_profile.convBiasInitializer;
        value.pads = m_profile.convPads;
        value.strides = m_profile.convStrides;
        value.dilations = m_profile.convDilations;
      }
      value.begin = rank == 0 ? 0 : half;
      value.end = rank == 0 ? half : m_profile.inputChannels;
      value.extent = m_profile.inputChannels;
      value.validate();
      return value.canonicalJson();
    };
    if (!blockProgram) {
      candidate.sourceTransformByRole[m_profile.roles[1]] = rankTransform(0);
      candidate.sourceTransformByRole[m_profile.roles[2]] = rankTransform(1);
      if (!pair && !m_profile.convBiasInitializer.empty()) {
        // A fused Conv bias has no explicit Add in the source graph; the suffix
        // still has to apply it exactly once. Bind the transform either way and
        // let lowering decide whether the source already spells the Add out.
        std::string biasNode = m_profile.boundaryNode;
        if (lastRankNode + 1 < graph.topologicalOrder.size()) {
          const auto& nextId = graph.topologicalOrder[lastRankNode + 1];
          const auto next = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&] (const auto& node) { return node.id == nextId; });
          if (next != graph.nodes.end() && next->opType == "Add") biasNode = nextId;
        }
        NativeOnnxSourceTransformV1 bias;
        bias.kind = "add-conv-bias";
        bias.sourceConv = m_profile.boundaryNode;
        bias.node = biasNode;
        bias.bias = m_profile.convBiasInitializer;
        bias.output = m_profile.convOutputTensor;
        bias.validate();
        candidate.sourceTransformByRole[m_profile.roles[3]] = bias.canonicalJson();
      }
    }
    candidate.executionPlan.stageLayout = {
      {m_profile.roles[0] + "/stage", nativePlanningDigest("source-prefix|" + graph.graphDigest),
       {m_profile.roles[0]}},
      {m_profile.roles[1] + stageSuffix, hybridStageDigest,
       {m_profile.roles[1], m_profile.roles[2]}},
      {m_profile.roles[3] + "/stage", nativePlanningDigest("source-suffix|" + graph.graphDigest),
       {m_profile.roles[3]}},
    };
    if (blockProgram) {
      buildBlockSteps(candidate, graph, control);
    }
    else {
    const auto layout = [&] {
      if (!m_profile.activationLayoutDigest.empty()) return m_profile.activationLayoutDigest;
      return nativePlanningDigest("source-activation-layout-v1");
    }();
    const auto slot = [&] (const std::string& id, const std::vector<std::int64_t>& shape,
                           const std::string& source, const std::string& producerStep,
                           const std::string& producerSlot) {
      NativeStepSlotSpec result;
      result.slotId = id;
      result.dtype = m_profile.activationDtype;
      for (const auto value : shape) result.shape.push_back(value);
      result.layoutDigest = layout;
      result.source = source;
      result.producerStepId = producerStep;
      result.producerSlotId = producerSlot;
      return result;
    };
    const std::string sourceInput = graph.modelInputs.empty() ? "input"
      : graph.modelInputs.front().name;
    const std::string sourceOutput = graph.modelOutputs.empty() ? "output"
      : graph.modelOutputs.front().name;
    // Build the request-ingress and terminal slots from the verified graph
    // contracts, not from the Conv activation shapes: the entry binder compares
    // name, dtype and shape exactly against the real source ports.
    const auto contractSlot = [&] (const std::string& id, const NativeTensorContract& contract,
                                   const std::string& source,
                                   const std::string& producerStep = {},
                                   const std::string& producerSlot = {}) {
      NativeStepSlotSpec result;
      result.slotId = id;
      result.dtype = contract.dtype.empty() ? m_profile.activationDtype : contract.dtype;
      for (const auto& dim : contract.shape) {
        if (const auto* value = std::get_if<std::int64_t>(&dim)) result.shape.push_back(*value);
      }
      result.layoutDigest = layout;
      result.source = source;
      result.producerStepId = producerStep;
      result.producerSlotId = producerSlot;
      return result;
    };
    const auto& activation = activationTensor;
    const auto& partial = partialTensor;
    if (activation == sourceInput || activation == partial || partial == sourceOutput)
      throw std::invalid_argument(
        "hybrid source partition reuses a graph boundary tensor as an internal port");
    const std::string prefixStep = m_profile.roles[0] + "/RUN";
    const auto stepName = [multiEntry] (const std::string& role, const char* kind,
                                        std::size_t index) {
      return role + "/" + kind + (multiEntry ? "/" + std::to_string(index) : "");
    };
    const auto windowCount = pair ? windows.size() : 1;
    const auto sumStep = stepName(m_profile.roles[1], "SUM", windowCount - 1);
    NativeExecutionStepSpec prefix;
    prefix.stepId = prefixStep;
    prefix.unitId = m_profile.roles[0];
    prefix.operation = "CALL_SUBGRAPH";
    prefix.artifactEntry = m_profile.roles[0] + "/artifact";
    // The ingress reads the request's own named tensor; internal handoffs keep
    // the actual ONNX tensor names so a runner port binding is never a guess.
    prefix.inputSlots.push_back(graph.modelInputs.empty()
      ? contractSlot(sourceInput, {"", m_profile.activationDtype, {}, {}}, "EPOCH_INPUT", "", "")
      : contractSlot(sourceInput, graph.modelInputs.front(), "EPOCH_INPUT"));
    prefix.outputSlots.push_back(slot(activation, m_profile.activationShape,
                                      "LOCAL_STEP", "", ""));
    for (const auto& crossing : crossings) {
      if (crossing.producerRole != m_profile.roles[0] ||
          crossing.consumerRole != m_profile.roles[3] ||
          crossing.edge->id == partial ||
          crossing.edge->producer == m_profile.boundaryNode)
        continue;
      if (!std::any_of(prefix.outputSlots.begin(), prefix.outputSlots.end(),
            [&] (const auto& value) { return value.slotId == crossing.edge->id; }))
        prefix.outputSlots.push_back(contractSlot(crossing.edge->id, crossing.edge->tensor,
                                                  "LOCAL_STEP", "", ""));
    }
    candidate.executionPlan.steps.push_back(std::move(prefix));
    for (std::size_t index = 0; index < windowCount; ++index) {
      if (control) control->requireActive();
      const auto& inlet = pair ? windows[index].transform.input : activation;
      const auto& outlet = pair ? windows[index].transform.output : partial;
      const auto& inputShape = pair ? windows[index].inputShape : m_profile.activationShape;
      const auto& outputShape = pair ? windows[index].outputShape : m_profile.partialShape;
      for (std::size_t rank = 0; rank < 2; ++rank) {
        const auto& role = m_profile.roles[rank + 1];
        NativeExecutionStepSpec call;
        call.stepId = stepName(role, "RUN", index);
        call.unitId = role;
        call.operation = "CALL_SUBGRAPH";
        call.artifactEntry = stepName(role, "artifact", index);
        call.inputSlots.push_back(slot(inlet, inputShape, index == 0 ? "REMOTE_STEP" : "LOCAL_STEP",
          index == 0 ? prefixStep : stepName(role, "SUM", index - 1), inlet));
        call.outputSlots.push_back(slot(outlet, outputShape, "LOCAL_STEP", "", ""));
        candidate.executionPlan.steps.push_back(std::move(call));
      }
      const std::size_t reducers = index + 1 == windowCount ? 1 : 2;
      for (std::size_t rank = 0; rank < reducers; ++rank) {
        const auto& role = m_profile.roles[rank + 1];
        NativeExecutionStepSpec sum;
        sum.stepId = stepName(role, "SUM", index);
        sum.unitId = role;
        sum.operation = "SUM";
        for (std::size_t contribution = 0; contribution < 2; ++contribution) {
          const auto& producer = m_profile.roles[contribution + 1];
          sum.inputSlots.push_back(slot("rank" + std::to_string(contribution), outputShape,
            contribution == rank ? "LOCAL_STEP" : "REMOTE_STEP",
            stepName(producer, "RUN", index), outlet));
        }
        sum.outputSlots.push_back(slot(outlet, outputShape, "LOCAL_STEP", "", ""));
        candidate.executionPlan.steps.push_back(std::move(sum));
      }
    }
    NativeExecutionStepSpec suffix;
    suffix.stepId = m_profile.roles[3] + "/RUN";
    suffix.unitId = m_profile.roles[3];
    suffix.operation = "CALL_SUBGRAPH";
    suffix.artifactEntry = m_profile.roles[3] + "/artifact";
    suffix.inputSlots.push_back(slot(partial, m_profile.partialShape, "REMOTE_STEP",
                                     sumStep, partial));
    for (const auto& crossing : crossings) {
      if (crossing.consumerRole != m_profile.roles[3] || crossing.edge->id == partial ||
          crossing.edge->producer == m_profile.boundaryNode)
        continue;
      if (crossing.producerRole != m_profile.roles[0] &&
          crossing.producerRole != m_profile.roles[1] &&
          crossing.producerRole != m_profile.roles[2])
        continue;
      if (!std::any_of(suffix.inputSlots.begin(), suffix.inputSlots.end(),
            [&] (const auto& value) { return value.slotId == crossing.edge->id; }))
        suffix.inputSlots.push_back(contractSlot(crossing.edge->id, crossing.edge->tensor,
                                                 "REMOTE_STEP",
                                                 crossing.producerRole + "/RUN",
                                                 crossing.edge->id));
    }
    suffix.outputSlots.push_back(graph.modelOutputs.empty()
      ? contractSlot(sourceOutput, {"", m_profile.activationDtype, {}, {}}, "LOCAL_STEP", "", "")
      : contractSlot(sourceOutput, graph.modelOutputs.front(), "LOCAL_STEP"));
    candidate.executionPlan.steps.push_back(std::move(suffix));
    }
  }

  // Ordinary source roles execute through the same V2 step template as the
  // hybrid split: one CALL_SUBGRAPH for a full role, two stages for a component
  // cut. A step-produced terminal value is materialized to wire bytes at the
  // response boundary by the Provider.
  if (m_profile.mode == NativeSourcePartitionMode::FULL ||
      m_profile.mode == NativeSourcePartitionMode::COMPONENT) {
    const std::string sourceInput = graph.modelInputs.empty() ? "input"
      : graph.modelInputs.front().name;
    const std::string sourceOutput = graph.modelOutputs.empty() ? "output"
      : graph.modelOutputs.front().name;
    const auto layout = m_profile.activationLayoutDigest.empty()
      ? nativePlanningDigest("source-activation-layout-v1")
      : m_profile.activationLayoutDigest;
    const auto contractSlot = [&] (const std::string& id, const NativeTensorContract& contract,
                                   const std::string& source, const std::string& producerStep,
                                   const std::string& producerSlot) {
      NativeStepSlotSpec result;
      result.slotId = id;
      result.dtype = contract.dtype.empty() ? m_profile.activationDtype : contract.dtype;
      for (const auto& dim : contract.shape)
        if (const auto* value = std::get_if<std::int64_t>(&dim)) result.shape.push_back(*value);
      result.layoutDigest = layout;
      result.source = source;
      result.producerStepId = producerStep;
      result.producerSlotId = producerSlot;
      return result;
    };
    candidate.executionPlan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
    if (m_profile.mode == NativeSourcePartitionMode::FULL) {
      NativeExecutionStepSpec step;
      step.stepId = m_profile.roles.front() + "/RUN";
      step.unitId = m_profile.roles.front();
      step.operation = "CALL_SUBGRAPH";
      step.artifactEntry = m_profile.roles.front() + "/artifact";
      for (const auto& input : graph.modelInputs)
        step.inputSlots.push_back(contractSlot(input.name, input, "EPOCH_INPUT", "", ""));
      if (step.inputSlots.empty())
        step.inputSlots.push_back(contractSlot(sourceInput, {"", m_profile.activationDtype, {}, {}},
                                               "EPOCH_INPUT", "", ""));
      step.outputSlots.push_back(graph.modelOutputs.empty()
        ? contractSlot(sourceOutput, {"", m_profile.activationDtype, {}, {}}, "LOCAL_STEP", "", "")
        : contractSlot(sourceOutput, graph.modelOutputs.front(), "LOCAL_STEP", "", ""));
      candidate.executionPlan.steps.push_back(std::move(step));
      candidate.executionPlan.stageLayout.push_back(
        {m_profile.roles.front() + "/stage",
         nativePlanningDigest("source-full|" + graph.graphDigest), {m_profile.roles.front()}});
    }
    else {
      // Every tensor crossing the component cut becomes an explicit V2 port and
      // dependency, not just the first one: a region that keeps a residual path
      // otherwise loses that edge and the entry binder rejects the boundary.
      std::vector<const NativeGraphEdge*> cut;
      std::set<std::string> selectedCuts;
      for (const auto& edge : graph.edges) {
        const auto producer = ownerByNode.find(edge.producer);
        if (producer == ownerByNode.end() || producer->second != m_profile.roles[0]) continue;
        const bool crossesToSuffix = std::any_of(edge.consumers.begin(), edge.consumers.end(),
          [&] (const auto& consumerNode) {
            const auto consumer = ownerByNode.find(consumerNode);
            return consumer != ownerByNode.end() && consumer->second == m_profile.roles[1];
          });
        if (crossesToSuffix && selectedCuts.insert(edge.id).second) cut.push_back(&edge);
      }
      if (cut.empty())
        throw std::invalid_argument("component cut has no crossing tensor");
      const std::string prefixStep = m_profile.roles[0] + "/RUN";
      const std::string suffixStep = m_profile.roles[1] + "/RUN";
      NativeExecutionStepSpec prefix;
      prefix.stepId = prefixStep;
      prefix.unitId = m_profile.roles[0];
      prefix.operation = "CALL_SUBGRAPH";
      prefix.artifactEntry = m_profile.roles[0] + "/artifact";
      prefix.inputSlots.push_back(graph.modelInputs.empty()
        ? contractSlot(sourceInput, {"", m_profile.activationDtype, {}, {}}, "EPOCH_INPUT", "", "")
        : contractSlot(sourceInput, graph.modelInputs.front(), "EPOCH_INPUT", "", ""));
      NativeExecutionStepSpec suffix;
      suffix.stepId = suffixStep;
      suffix.unitId = m_profile.roles[1];
      suffix.operation = "CALL_SUBGRAPH";
      suffix.artifactEntry = m_profile.roles[1] + "/artifact";
      for (const auto* edge : cut) {
        prefix.outputSlots.push_back(contractSlot(edge->id, edge->tensor,
                                                  "LOCAL_STEP", "", ""));
        suffix.inputSlots.push_back(contractSlot(edge->id, edge->tensor,
                                                 "REMOTE_STEP", prefixStep, edge->id));
      }
      suffix.outputSlots.push_back(graph.modelOutputs.empty()
        ? contractSlot(sourceOutput, {"", m_profile.activationDtype, {}, {}}, "LOCAL_STEP", "", "")
        : contractSlot(sourceOutput, graph.modelOutputs.front(), "LOCAL_STEP", "", ""));
      candidate.executionPlan.steps.push_back(std::move(prefix));
      candidate.executionPlan.steps.push_back(std::move(suffix));
      candidate.executionPlan.stageLayout = {
        {m_profile.roles[0] + "/stage",
         nativePlanningDigest("source-prefix|" + graph.graphDigest), {m_profile.roles[0]}},
        {m_profile.roles[1] + "/stage",
         nativePlanningDigest("source-suffix|" + graph.graphDigest), {m_profile.roles[1]}},
      };
      for (const auto* edge : cut) {
        NativeDependencySpec dependency;
        dependency.dependencyId = "component-" + edge->id;
        dependency.producers = {m_profile.roles[0]};
        dependency.consumers = {m_profile.roles[1]};
        dependency.keyScope = "native-source-activation";
        dependency.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
        dependency.objectNameTemplate =
          "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
        dependency.tensors = {edge->id};
        dependency.operationKind = "ACTIVATION";
        dependency.producerStepId = prefixStep;
        dependency.producerSlotId = edge->id;
        dependency.consumerStepId = suffixStep;
        dependency.consumerSlotId = edge->id;
        candidate.executionPlan.dependencies.push_back(std::move(dependency));
      }
    }
  }

  std::uint64_t knownBytes = 0;
  std::uint64_t crossedBytes = 0;
  for (const auto& edge : graph.edges) {
    if (control) control->requireActive();
    const auto size = edge.tensor.estimatedBytes.value_or(0);
    if (size > std::numeric_limits<std::uint64_t>::max() - knownBytes)
      throw std::invalid_argument("source graph tensor byte estimate overflows");
    knownBytes += size;
    const auto& producerRole = ownerByNode.at(edge.producer);
    std::set<std::string> consumerRoles;
    for (const auto& consumer : edge.consumers) {
      const auto& role = ownerByNode.at(consumer);
      if (role != producerRole) consumerRoles.insert(role);
    }
    if (consumerRoles.empty()) continue;
    crossedBytes += size;
    if (!contains(graph.legalCutEdges, edge.id))
      throw std::invalid_argument("source candidate crosses an illegal tensor edge");
    candidate.crossPartitionTensors.push_back(edge.id);
    // A V2 step plan binds dependencies to step ports explicitly; a plain
    // graph-edge dependency has no step owner and the projection rejects it.
    if (!candidate.executionPlan.steps.empty()) continue;
    for (const auto& consumerRole : consumerRoles) {
      NativeDependencySpec dependency;
      dependency.producers = {producerRole};
      dependency.consumers = {consumerRole};
      dependency.keyScope = "native-source-activation";
      dependency.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
      dependency.objectNameTemplate =
        "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
      dependency.tensors = {edge.id};
      dependency.operationKind = "ACTIVATION";
      candidate.executionPlan.dependencies.push_back(std::move(dependency));
    }
  }

  if (m_profile.mode == NativeSourcePartitionMode::HYBRID) {
    const auto dependency = [&] (const std::string& id, const std::string& producer,
                                 const std::string& consumer, const std::string& producerStep,
                                 const std::string& producerSlot,
                                 const std::string& consumerStep,
                                 const std::string& consumerSlot) {
      NativeDependencySpec result;
      result.dependencyId = id;
      result.producers = {producer};
      result.consumers = {consumer};
      result.keyScope = id;
      result.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
      result.objectNameTemplate =
        "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
      result.tensors = {producerSlot};
      result.operationKind = "ACTIVATION";
      result.producerStepId = producerStep;
      result.producerSlotId = producerSlot;
      result.consumerStepId = consumerStep;
      result.consumerSlotId = consumerSlot;
      return result;
    };
    if (multiEntry) {
      std::map<std::string, const NativeExecutionStepSpec*> stepById;
      for (const auto& step : candidate.executionPlan.steps)
        stepById.emplace(step.stepId, &step);
      for (const auto& step : candidate.executionPlan.steps) {
        for (const auto& port : step.inputSlots) {
          if (port.source != "REMOTE_STEP") continue;
          const auto& producer = *stepById.at(port.producerStepId);
          auto edge = dependency("source-entry-" +
            std::to_string(candidate.executionPlan.dependencies.size()), producer.unitId,
            step.unitId, producer.stepId, port.producerSlotId, step.stepId, port.slotId);
          if (step.operation == "SUM") {
            edge.operationKind = "TENSOR_PARTIAL";
            edge.useNdnsfDataV1 = true;
          }
          candidate.executionPlan.dependencies.push_back(std::move(edge));
        }
      }
    }
    else {
      for (const auto& rank : {m_profile.roles[1], m_profile.roles[2]})
        candidate.executionPlan.dependencies.push_back(dependency(
          "prefix-to-" + rank, m_profile.roles[0], rank,
          m_profile.roles[0] + "/RUN", activationTensor, rank + "/RUN",
          activationTensor));
      auto partial = dependency(
        "rank1-to-sum", m_profile.roles[2], m_profile.roles[1],
        m_profile.roles[2] + "/RUN", partialTensor,
        m_profile.roles[1] + "/SUM", "rank1");
      partial.operationKind = "TENSOR_PARTIAL";
      partial.useNdnsfDataV1 = true;
      candidate.executionPlan.dependencies.push_back(std::move(partial));
      candidate.executionPlan.dependencies.push_back(dependency(
        "sum-to-suffix", m_profile.roles[1], m_profile.roles[3],
        m_profile.roles[1] + "/SUM", partialTensor,
        m_profile.roles[3] + "/RUN", partialTensor));
      // Direct bypass dependencies carry every non-SUM crossing into the suffix.
      for (const auto& edge : graph.edges) {
        const auto producer = ownerByNode.find(edge.producer);
        if (producer == ownerByNode.end()) continue;
        const bool fromRank = producer->second == m_profile.roles[1] ||
                              producer->second == m_profile.roles[2];
        if (producer->second != m_profile.roles[0] && !fromRank) continue;
        if (edge.id == partialTensor ||
            edge.producer == m_profile.boundaryNode)
          continue;
        const bool toSuffix = std::any_of(edge.consumers.begin(), edge.consumers.end(),
          [&] (const auto& node) {
            const auto consumer = ownerByNode.find(node);
            return consumer != ownerByNode.end() &&
                   consumer->second == m_profile.roles[3];
          });
        if (!toSuffix) continue;
        const auto dependencyId = std::string("bypass-") + edge.id;
        if (std::any_of(candidate.executionPlan.dependencies.begin(),
                        candidate.executionPlan.dependencies.end(),
                        [&] (const auto& value) { return value.dependencyId == dependencyId; }))
          continue;
        candidate.executionPlan.dependencies.push_back(dependency(
          dependencyId, producer->second, m_profile.roles[3],
          producer->second + "/RUN", edge.id,
          m_profile.roles[3] + "/RUN", edge.id));
      }
    }
  }

  const auto roleBytes = std::max<std::uint64_t>(1, knownBytes / m_profile.roles.size());
  const auto transformedRoles = !candidate.sourceTransformByRole.empty();
  for (const auto& role : m_profile.roles) {
    if (control) control->requireActive();
    std::vector<std::string> roleNodes;
    if (pair || blockProgram) {
      roleNodes = candidate.roleNodeIdsByRole.at(role);
    }
    else {
      for (const auto& node : graph.topologicalOrder)
        if (ownerByNode.at(node) == role) roleNodes.push_back(node);
    }
    candidate.fragmentsByRole[role] = nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"candidate", candidate.splitter.name}, {"graph", graph.graphDigest},
      {"role", role}, {"nodes", roleNodes}}));
    candidate.artifactsByRole[role] = {candidate.fragmentsByRole[role]};
    std::uint64_t residentBytes = roleBytes;
    const auto denseWeights = m_profile.denseWeightBytesByRole.find(role);
    if (denseWeights != m_profile.denseWeightBytesByRole.end())
      residentBytes = std::max<std::uint64_t>(1, denseWeights->second);
    if (pair && role == m_profile.roles[0] && m_profile.ffnPrefixWeightBytes)
      residentBytes = std::max<std::uint64_t>(1, *m_profile.ffnPrefixWeightBytes);
    if (transformedRoles && (role == m_profile.roles[1] || role == m_profile.roles[2])) {
      const std::size_t rank = role == m_profile.roles[1] ? 0 : 1;
      if (blockProgram) {
        residentBytes = 0;
        for (const auto& entry : m_profile.blockEntries) {
          if (rank == 1 && &entry == &m_profile.blockEntries.back()) continue;
          if (entry.initializerBytes > std::numeric_limits<std::uint64_t>::max() - residentBytes)
            throw std::overflow_error("source block rank initializer memory exceeds uint64");
          residentBytes += entry.initializerBytes;
        }
        residentBytes = std::max<std::uint64_t>(1, residentBytes);
      }
      else if (pair) {
        residentBytes = 0;
        for (const auto& window : windows) {
          const auto extent = window.transform.extent;
          const auto bounds = nativeConvInputChannelRanges(extent, 2)[rank];
          const auto sliced = nativeFfnSliceBytes(window.weightBytes, extent,
                                                 bounds.second - bounds.first);
          if (sliced > std::numeric_limits<std::uint64_t>::max() - residentBytes)
            throw std::overflow_error("FFN rank resident weights exceed uint64");
          residentBytes += sliced;
        }
      }
      else if (m_profile.convWeightBytes != 0) {
        const auto channels = m_profile.inputChannels;
        if (m_profile.convWeightBytes % channels != 0)
          throw std::invalid_argument("initializer bytes do not fit the channel split");
        const auto rankChannels = rank == 0 ? channels / 2 : channels - channels / 2;
        residentBytes = (m_profile.convWeightBytes / channels) * rankChannels;
      }
    }
    else if (transformedRoles && role == m_profile.roles[3] && m_profile.suffixWeightBytes &&
             (pair || m_profile.convWeightBytes != 0)) {
      residentBytes = std::max<std::uint64_t>(1, *m_profile.suffixWeightBytes);
    }
    candidate.requirementsByRole[role] = {
      {"onnxruntime-cpu", "onnxruntime-cuda"},
      residentBytes, 256ULL * 1024ULL * 1024ULL, 0,
      256ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL, 1.1};
  }
  std::uint64_t dependencyIndex = 0;
  for (auto& dependency : candidate.executionPlan.dependencies)
    if (dependency.dependencyId.empty())
      dependency.dependencyId = "native-source-dependency-" + std::to_string(dependencyIndex++);

  if (m_profile.mode == NativeSourcePartitionMode::HYBRID) {
    for (const auto& role : m_profile.roles) {
      candidate.tensorDegreesByRole[role] = 1;
      candidate.rankArtifactDigestsByRole[role] = {candidate.artifactsByRole[role].front()};
    }
  }
  candidate.estimatedCosts = {{"role_count", std::uint64_t(m_profile.roles.size())},
                              {"known_transfer_bytes", crossedBytes}};
  candidate.candidateDigest = candidate.computedDigest();
  candidate.validate(graph);
  return {std::move(candidate)};
}

} // namespace ndnsf::di
