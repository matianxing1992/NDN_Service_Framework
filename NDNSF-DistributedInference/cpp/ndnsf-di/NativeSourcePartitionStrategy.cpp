#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeSourcePartitionStrategy.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <algorithm>
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
       m_profile.activationShape.empty() || m_profile.partialShape.empty()))
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

std::vector<NativeSplitCandidate>
NativeSourcePartitionStrategy::enumerateImpl(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl* control) const
{
  if (control) control->requireActive();
  budget.validate();
  model.validate();
  graph.validate(model);
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
    if (boundaryNode == graph.nodes.end() || boundaryNode->opType != "Conv")
      throw std::invalid_argument("source hybrid boundary is not a Conv");
    const auto boundaryIndex = static_cast<std::size_t>(
      std::distance(graph.topologicalOrder.begin(), boundary));
    if (boundaryIndex == 0 || boundaryIndex + 1 >= graph.topologicalOrder.size())
      throw std::invalid_argument("source hybrid boundary leaves an empty region");
    for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
      if (control) control->requireActive();
      const auto& node = graph.topologicalOrder[i];
      if (i < boundaryIndex) ownerByNode.emplace(node, m_profile.roles[0]);
      else if (i > boundaryIndex) ownerByNode.emplace(node, m_profile.roles[3]);
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
    const auto boundary = std::find(graph.topologicalOrder.begin(),
                                    graph.topologicalOrder.end(),
                                    m_profile.boundaryNode);
    const auto boundaryIndex = static_cast<std::size_t>(
      std::distance(graph.topologicalOrder.begin(), boundary));
    candidate.roleNodeIdsByRole[m_profile.roles[1]] = {m_profile.boundaryNode};
    candidate.roleNodeIdsByRole[m_profile.roles[2]] = {m_profile.boundaryNode};
    for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
      const auto& node = graph.topologicalOrder[i];
      if (i < boundaryIndex) candidate.roleNodeIdsByRole[m_profile.roles[0]].push_back(node);
      else if (i > boundaryIndex) candidate.roleNodeIdsByRole[m_profile.roles[3]].push_back(node);
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
        crossings.push_back({&edge, producer->second, consumer->second});
      }
    }
    const auto half = m_profile.inputChannels / 2;
    hybridStageDigest = nativePlanningDigest("source-hybrid|" + graph.graphDigest + "|" +
                                             m_profile.boundaryNode + "|" +
                                             std::to_string(m_profile.inputChannels));
    auto first = NativeJson::array();
    first.push_back(NativeJson{{"axis", m_profile.convInputAxis}, {"begin", 0},
                               {"end", half}, {"extent", m_profile.inputChannels}});
    auto second = NativeJson::array();
    second.push_back(NativeJson{{"axis", m_profile.convInputAxis}, {"begin", half},
                                {"end", m_profile.inputChannels},
                                {"extent", m_profile.inputChannels}});
    auto ranked = NativeJson::array();
    ranked.push_back(NativeJson{{"role", m_profile.roles[1]}, {"axes", first}});
    ranked.push_back(NativeJson{{"role", m_profile.roles[2]}, {"axes", second}});
    const auto layoutDigest = nativeTensorStageLayoutDigest(
      model.adapter.descriptorDigest(), graph.graphDigest,
      m_profile.roles[1] + "/conv", hybridStageDigest, ranked);
    auto shard0 = NativeJson{{"stage_layout_digest", layoutDigest}, {"axes", first}};
    auto shard1 = NativeJson{{"stage_layout_digest", layoutDigest}, {"axes", second}};
    candidate.tensorIdentityByRole[m_profile.roles[1]] = {
      2, 0, nativeCanonicalJson(shard0)};
    candidate.tensorIdentityByRole[m_profile.roles[2]] = {
      2, 1, nativeCanonicalJson(shard1)};
    candidate.executionPlan.tensorDegree = 2;
    candidate.executionPlan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
    const auto rankTransform = [&] (std::uint64_t begin, std::uint64_t end) {
      NativeOnnxSourceTransformV1 value;
      value.kind = "conv-input-channel-partial";
      value.node = m_profile.boundaryNode;
      value.axis = m_profile.convInputAxis;
      value.input = m_profile.convInputTensor;
      value.weight = m_profile.convWeightInitializer;
      value.output = m_profile.convOutputTensor;
      value.bias = m_profile.convBiasInitializer;
      value.begin = begin;
      value.end = end;
      value.extent = m_profile.inputChannels;
      value.pads = m_profile.convPads;
      value.strides = m_profile.convStrides;
      value.dilations = m_profile.convDilations;
      value.validate();
      return value.canonicalJson();
    };
    candidate.sourceTransformByRole[m_profile.roles[1]] = rankTransform(0, half);
    candidate.sourceTransformByRole[m_profile.roles[2]] = rankTransform(half, m_profile.inputChannels);
    if (!m_profile.convBiasInitializer.empty()) {
      // A fused Conv bias has no explicit Add in the source graph; the suffix
      // still has to apply it exactly once. Bind the transform either way and
      // let lowering decide whether the source already spells the Add out.
      std::string biasNode = m_profile.boundaryNode;
      if (boundaryIndex + 1 < graph.topologicalOrder.size()) {
        const auto& nextId = graph.topologicalOrder[boundaryIndex + 1];
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
    candidate.executionPlan.stageLayout = {
      {m_profile.roles[0] + "/stage", nativePlanningDigest("source-prefix|" + graph.graphDigest),
       {m_profile.roles[0]}},
      {m_profile.roles[1] + "/conv", hybridStageDigest,
       {m_profile.roles[1], m_profile.roles[2]}},
      {m_profile.roles[3] + "/stage", nativePlanningDigest("source-suffix|" + graph.graphDigest),
       {m_profile.roles[3]}},
    };
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
    const std::string activation = m_profile.convInputTensor;
    const std::string partial = m_profile.convOutputTensor;
    if (activation == sourceInput || activation == partial || partial == sourceOutput)
      throw std::invalid_argument(
        "hybrid source partition reuses a graph boundary tensor as an internal port");
    const std::string prefixStep = m_profile.roles[0] + "/RUN";
    const std::string sumStep = m_profile.roles[1] + "/SUM";
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
    for (const auto& rank : {m_profile.roles[1], m_profile.roles[2]}) {
      NativeExecutionStepSpec step;
      step.stepId = rank + "/RUN";
      step.unitId = rank;
      step.operation = "CALL_SUBGRAPH";
      step.artifactEntry = rank + "/artifact";
      step.inputSlots.push_back(slot(activation, m_profile.activationShape, "REMOTE_STEP",
                                     prefixStep, activation));
      step.outputSlots.push_back(slot(partial, m_profile.partialShape,
                                      "LOCAL_STEP", "", ""));
      candidate.executionPlan.steps.push_back(std::move(step));
    }
    NativeExecutionStepSpec sum;
    sum.stepId = sumStep;
    sum.unitId = m_profile.roles[1];
    sum.operation = "SUM";
    sum.inputSlots.push_back(slot("rank0", m_profile.partialShape, "LOCAL_STEP",
                                  m_profile.roles[1] + "/RUN", partial));
    sum.inputSlots.push_back(slot("rank1", m_profile.partialShape, "REMOTE_STEP",
                                  m_profile.roles[2] + "/RUN", partial));
    sum.outputSlots.push_back(slot(partial, m_profile.partialShape, "LOCAL_STEP", "", ""));
    candidate.executionPlan.steps.push_back(std::move(sum));
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
      step.inputSlots.push_back(graph.modelInputs.empty()
        ? contractSlot(sourceInput, {"", m_profile.activationDtype, {}, {}}, "EPOCH_INPUT", "", "")
        : contractSlot(sourceInput, graph.modelInputs.front(), "EPOCH_INPUT", "", ""));
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
    for (const auto& rank : {m_profile.roles[1], m_profile.roles[2]})
      candidate.executionPlan.dependencies.push_back(dependency(
        "prefix-to-" + rank, m_profile.roles[0], rank,
        m_profile.roles[0] + "/RUN", m_profile.convInputTensor, rank + "/RUN",
        m_profile.convInputTensor));
    auto partial = dependency(
      "rank1-to-sum", m_profile.roles[2], m_profile.roles[1],
      m_profile.roles[2] + "/RUN", m_profile.convOutputTensor,
      m_profile.roles[1] + "/SUM", "rank1");
    partial.operationKind = "TENSOR_PARTIAL";
    partial.useNdnsfDataV1 = true;
    candidate.executionPlan.dependencies.push_back(std::move(partial));
    candidate.executionPlan.dependencies.push_back(dependency(
      "sum-to-suffix", m_profile.roles[1], m_profile.roles[3],
      m_profile.roles[1] + "/SUM", m_profile.convOutputTensor,
      m_profile.roles[3] + "/RUN", m_profile.convOutputTensor));
    // Direct bypass dependencies carry every non-SUM crossing into the suffix.
    for (const auto& edge : graph.edges) {
      const auto producer = ownerByNode.find(edge.producer);
      if (producer == ownerByNode.end()) continue;
      const bool fromRank = producer->second == m_profile.roles[1] ||
                            producer->second == m_profile.roles[2];
      if (producer->second != m_profile.roles[0] && !fromRank) continue;
      if (edge.id == m_profile.convOutputTensor ||
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

  const auto roleBytes = std::max<std::uint64_t>(1, knownBytes / m_profile.roles.size());
  const auto transformedRoles = !candidate.sourceTransformByRole.empty();
  for (const auto& role : m_profile.roles) {
    if (control) control->requireActive();
    std::vector<std::string> roleNodes;
    for (const auto& node : graph.topologicalOrder)
      if (ownerByNode.at(node) == role) roleNodes.push_back(node);
    candidate.fragmentsByRole[role] = nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"candidate", candidate.splitter.name}, {"graph", graph.graphDigest},
      {"role", role}, {"nodes", roleNodes}}));
    candidate.artifactsByRole[role] = {candidate.fragmentsByRole[role]};
    std::uint64_t residentBytes = roleBytes;
    if (transformedRoles && m_profile.convWeightBytes != 0) {
      if (role == m_profile.roles[1] || role == m_profile.roles[2]) {
        const auto channels = m_profile.inputChannels;
        if (m_profile.convWeightBytes % channels != 0)
          throw std::invalid_argument("Conv initializer bytes do not fit the channel split");
        const auto rankChannels = role == m_profile.roles[1] ?
          channels / 2 : channels - channels / 2;
        residentBytes = (m_profile.convWeightBytes / channels) * rankChannels;
      }
      else if (role == m_profile.roles[3] && m_profile.suffixWeightBytes)
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
