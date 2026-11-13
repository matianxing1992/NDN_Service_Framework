#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-split.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::string
bareName(const std::string& name)
{
  return name.size() > 6 && name.compare(0, 6, "graph/") == 0 ? name.substr(6) : name;
}

NativeTensorContract
stateTensor(const std::string& name, const AutomaticAttentionState& state)
{
  NativeTensorContract result;
  result.name = bareName(name);
  result.dtype = tensorElementTypeName(state.elementType);
  result.shape = {"batch", static_cast<std::int64_t>(state.heads), "sequence",
                  static_cast<std::int64_t>(state.headSize)};
  return result;
}

/** Producer node of every source tensor, including final outputs that have no
 * consumer edge and therefore do not appear in the planning edge set. */
std::map<std::string, std::string>
producerNodes(const NativeOnnxGraphInspection& inspection)
{
  std::map<std::string, std::string> result;
  for (const auto& edge : inspection.graph.edges) result[edge.id] = edge.producer;
  if (inspection.graphMetadataJson.empty()) return result;
  const auto metadata = nativeParseJson(inspection.graphMetadataJson);
  const auto producers = metadata.find("tensorProducers");
  if (producers == metadata.end()) return result;
  for (const auto& [name, index] : producers->items()) {
    if (!result.count(name))
      result.emplace(name, "onnx-node-" + std::to_string(index.get<std::uint64_t>()));
  }
  return result;
}

/** Sum of packed initializers consumed by one role; shared weights are charged
 * to every consuming role instead of being treated as free. */
std::uint64_t
regionWeightBytes(const NativeOnnxGraphInspection& inspection,
                  const NativeSplitCandidate& candidate, const std::string& role)
{
  if (inspection.graphMetadataJson.empty()) return 0;
  const auto metadata = nativeParseJson(inspection.graphMetadataJson);
  const auto tensors = metadata.find("tensors");
  if (tensors == metadata.end()) return 0;
  std::uint64_t bytes = 0;
  for (const auto& [name, consumers] : inspection.initializerConsumers) {
    bool used = false;
    for (const auto index : consumers) {
      const auto owner = candidate.nodeRoles.find("onnx-node-" + std::to_string(index));
      if (owner != candidate.nodeRoles.end() && owner->second == role) {
        used = true;
        break;
      }
    }
    if (!used) continue;
    const auto info = tensors->find(name);
    if (info == tensors->end()) continue;
    const auto size = info->find("sizeBytes");
    if (size == info->end() || size->is_null()) continue;
    bytes += size->get<std::uint64_t>();
  }
  return bytes;
}

std::uint64_t
statePairBytes(const AutomaticAttentionState& state, std::uint64_t context)
{
  return 2ULL * context * state.heads * state.headSize *
         tensorElementByteSize(state.elementType);
}

std::string
roleDigest(const NativeGraphSnapshot& graph, const std::string& role,
           const std::vector<std::string>& nodes)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-onnx-role-plan-v1"},
    {"graph", graph.graphDigest},
    {"role", role},
    {"nodes", nodes}}));
}

} // namespace

AutomaticOnnxSplitStrategy::AutomaticOnnxSplitStrategy(
  std::shared_ptr<const AutomaticOnnxModel> model)
  : m_model(std::move(model))
{
  if (!m_model)
    throw std::invalid_argument("automatic split requires an admitted source model");
}

NativeStrategyIdentity
AutomaticOnnxSplitStrategy::identity() const
{
  NativeStrategyIdentity result;
  result.name = "native-automatic-onnx-stage";
  result.version = "1";
  result.configurationDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"strategy", result.name},
    {"version", result.version},
    {"graph", m_model->inspection.graph.graphDigest},
    {"facts", m_model->contract.factsDigest}}));
  result.deterministic = true;
  return result;
}

NativeGraphSnapshot
AutomaticOnnxSplitStrategy::inspectGraph(const NativeModelDescriptor& model,
                                         const std::string& revision,
                                         std::uint64_t maxNodes) const
{
  model.validate();
  if (model.adapterId != m_model->descriptor.adapterId ||
      model.modelFormat != m_model->descriptor.modelFormat ||
      model.modelName != m_model->descriptor.modelName)
    throw std::invalid_argument("automatic split received another model");
  if (!revision.empty() && revision != m_model->descriptor.sourceRevision)
    throw std::invalid_argument("automatic split received another source revision");
  if (maxNodes == 0 || m_model->inspection.graph.nodes.size() > maxNodes)
    throw std::invalid_argument("automatic split exceeds the node budget");
  return m_model->inspection.graph;
}

std::vector<NativeSplitCandidate>
AutomaticOnnxSplitStrategy::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget) const
{
  return enumerateImpl(model, graph, budget, nullptr);
}

std::vector<NativeSplitCandidate>
AutomaticOnnxSplitStrategy::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl& control) const
{
  control.requireActive();
  auto result = enumerateImpl(model, graph, budget, &control);
  control.requireActive();
  return result;
}

std::vector<NativeSplitCandidate>
AutomaticOnnxSplitStrategy::enumerateImpl(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl* control) const
{
  const auto active = [&] {
    if (control) control->requireActive();
  };
  budget.validate();
  model.validate();
  graph.validate(model);
  if (graph.graphDigest != m_model->inspection.graph.graphDigest ||
      model.adapterId != m_model->descriptor.adapterId ||
      model.modelName != m_model->descriptor.modelName)
    throw std::invalid_argument("automatic split received another graph or model");
  // A generation cut must keep a source-proven state pair on both sides. A
  // source without that proof has no legal cut here; returning no candidate is
  // the honest outcome, not a stateless generation role.
  if (!m_model->contract.generation) return {};
  const auto& generation = *m_model->contract.generation;

  std::map<std::string, std::size_t> topoIndex;
  for (std::size_t index = 0; index < graph.topologicalOrder.size(); ++index)
    topoIndex.emplace(graph.topologicalOrder[index], index);
  const auto producerByTensor = producerNodes(m_model->inspection);

  struct StatePair
  {
    const AutomaticAttentionState* key = nullptr;
    const AutomaticAttentionState* value = nullptr;
    long owner = -1;
    std::string keyName;
    std::string valueName;
  };
  std::vector<StatePair> pairs;
  for (std::size_t index = 0; index + 1 < generation.states.size(); ++index) {
    const auto& key = generation.states[index];
    const auto& value = generation.states[index + 1];
    if (key.role != AutomaticAttentionState::Role::KEY ||
        value.role != AutomaticAttentionState::Role::VALUE)
      continue;
    StatePair pair;
    pair.key = &key;
    pair.value = &value;
    pair.keyName = bareName(key.output);
    pair.valueName = bareName(value.output);
    const auto keyProducer = producerByTensor.find(pair.keyName);
    const auto valueProducer = producerByTensor.find(pair.valueName);
    const auto keyAt = keyProducer == producerByTensor.end() ? topoIndex.end() :
      topoIndex.find(keyProducer->second);
    const auto valueAt = valueProducer == producerByTensor.end() ? topoIndex.end() :
      topoIndex.find(valueProducer->second);
    if (keyAt == topoIndex.end() || valueAt == topoIndex.end() ||
        keyAt->second != valueAt->second)
      continue;
    pair.owner = static_cast<long>(keyAt->second);
    pairs.push_back(std::move(pair));
  }
  if (pairs.empty()) return {};

  struct Cut
  {
    const NativeGraphEdge* edge = nullptr;
    std::size_t producerIndex = 0;
    std::uint64_t transferBytes = 0;
  };
  // State inputs and outputs are not activation cuts: moving one would give a
  // single cache component two concurrent owners.
  std::set<std::string> stateTensors;
  for (const auto& state : generation.states) {
    stateTensors.insert(bareName(state.input));
    stateTensors.insert(bareName(state.output));
  }
  std::vector<Cut> cuts;
  for (const auto& edge : graph.edges) {
    active();
    if (stateTensors.count(edge.id)) continue;
    const auto at = topoIndex.find(edge.producer);
    if (at == topoIndex.end() || edge.consumers.empty()) continue;
    // The cut must be a declared legal source edge, not an inferred boundary.
    if (std::find(graph.legalCutEdges.begin(), graph.legalCutEdges.end(), edge.id) ==
        graph.legalCutEdges.end())
      continue;
    if (std::find(graph.topologicalOrder.begin(), graph.topologicalOrder.end(),
                  edge.consumers.front()) == graph.topologicalOrder.end())
      continue;
    cuts.push_back(Cut{&edge, at->second, edge.tensor.estimatedBytes.value_or(0)});
  }
  std::stable_sort(cuts.begin(), cuts.end(), [] (const auto& left, const auto& right) {
    return left.transferBytes < right.transferBytes;
  });

  std::vector<NativeSplitCandidate> result;
  for (const auto& cut : cuts) {
    active();
    if (result.size() >= budget.maxCandidates) break;
    std::size_t prefixPairs = 0;
    std::size_t suffixPairs = 0;
    bool mixed = false;
    for (const auto& pair : pairs) {
      if (pair.owner < 0) { mixed = true; break; }
      if (static_cast<std::size_t>(pair.owner) <= cut.producerIndex) ++prefixPairs;
      else ++suffixPairs;
    }
    if (mixed || prefixPairs == 0 || suffixPairs == 0) continue;

    const std::string prefixRole = "/AutomaticSplit/Stage/0";
    const std::string suffixRole = "/AutomaticSplit/Stage/1";
    NativeSplitCandidate candidate;
    candidate.source = "PRE_SPLIT";
    candidate.splitter = identity();
    candidate.model = model;
    candidate.graphDigest = graph.graphDigest;
    candidate.executionPlan.version = 1;
    candidate.executionPlan.modelName = model.modelName;
    candidate.executionPlan.modelFamily = "automatic-onnx";
    candidate.executionPlan.modelFormat = model.modelFormat;
    candidate.executionPlan.plannerKind = "native-automatic-onnx-stage";
    candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
    candidate.executionPlan.roles = {prefixRole, suffixRole};
    candidate.inputIngressRole = prefixRole;
    candidate.resultEgressRole = suffixRole;
    std::vector<std::string> prefixNodes;
    std::vector<std::string> suffixNodes;
    for (std::size_t index = 0; index < graph.topologicalOrder.size(); ++index) {
      const auto& node = graph.topologicalOrder[index];
      const auto& role = index <= cut.producerIndex ? prefixRole : suffixRole;
      candidate.nodeRoles[node] = role;
      (index <= cut.producerIndex ? prefixNodes : suffixNodes).push_back(node);
    }
    // The full frontier, not only the inspected edge: a nonlinear source graph
    // can cross the same node interval on more than one tensor.
    std::vector<std::string> frontier;
    bool invalidFrontier = false;
    for (const auto& edge : graph.edges) {
      const auto at = topoIndex.find(edge.producer);
      if (at == topoIndex.end()) continue;
      const bool prefixProducer = at->second <= cut.producerIndex;
      bool crosses = false;
      for (const auto& consumer : edge.consumers) {
        const auto consumerAt = topoIndex.find(consumer);
        if (consumerAt == topoIndex.end()) continue;
        if ((consumerAt->second <= cut.producerIndex) != prefixProducer) crosses = true;
      }
      if (!crosses) continue;
      if (stateTensors.count(edge.id)) { invalidFrontier = true; break; }
      frontier.push_back(edge.id);
    }
    if (invalidFrontier || frontier.empty()) continue;
    if (std::any_of(frontier.begin(), frontier.end(), [&] (const auto& tensor) {
          return std::find(graph.legalCutEdges.begin(), graph.legalCutEdges.end(), tensor) ==
            graph.legalCutEdges.end();
        }))
      continue;
    NativeDependencySpec dependency;
    dependency.producers = {prefixRole};
    dependency.consumers = {suffixRole};
    dependency.keyScope = "native-automatic-onnx-activation";
    dependency.topicPrefix = "/NDNSF/DI/AUTOMATIC";
    dependency.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/AUTOMATIC/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
    dependency.tensors = frontier;
    dependency.operationKind = "ACTIVATION";
    candidate.executionPlan.dependencies.push_back(std::move(dependency));
    candidate.crossPartitionTensors = frontier;
    candidate.fragmentsByRole[prefixRole] = roleDigest(graph, prefixRole, prefixNodes);
    candidate.fragmentsByRole[suffixRole] = roleDigest(graph, suffixRole, suffixNodes);
    candidate.artifactsByRole[prefixRole] = {candidate.fragmentsByRole.at(prefixRole)};
    candidate.artifactsByRole[suffixRole] = {candidate.fragmentsByRole.at(suffixRole)};
    std::uint64_t prefixKv = 0, suffixKv = 0;
    for (const auto& pair : pairs) {
      const auto bytes = statePairBytes(*pair.key, generation.contextLimit) +
                         statePairBytes(*pair.value, generation.contextLimit);
      (static_cast<std::size_t>(pair.owner) <= cut.producerIndex ? prefixKv : suffixKv) += bytes;
    }
    std::uint64_t frontierBytes = 0;
    bool frontierKnown = true;
    for (const auto& tensor : frontier) {
      const auto edge = std::find_if(graph.edges.begin(), graph.edges.end(),
        [&] (const auto& value) { return value.id == tensor; });
      if (edge == graph.edges.end() || !edge->tensor.estimatedBytes) {
        frontierKnown = false;
        continue;
      }
      frontierBytes += *edge->tensor.estimatedBytes;
    }
    NativeRoleResourceRequirement prefixRequirement;
    prefixRequirement.backends = {"onnxruntime"};
    prefixRequirement.weightBytes = regionWeightBytes(m_model->inspection, candidate,
                                                      prefixRole);
    prefixRequirement.kvBytes = prefixKv;
    prefixRequirement.workspaceBytes = std::nullopt;
    prefixRequirement.transientBytes = std::nullopt;
    if (frontierKnown && frontierBytes != 0) prefixRequirement.activationBytes = frontierBytes;
    prefixRequirement.safetyMargin = 1.10;
    NativeRoleResourceRequirement suffixRequirement = prefixRequirement;
    suffixRequirement.weightBytes = regionWeightBytes(m_model->inspection, candidate,
                                                      suffixRole);
    suffixRequirement.kvBytes = suffixKv;
    candidate.requirementsByRole[prefixRole] = prefixRequirement;
    candidate.requirementsByRole[suffixRole] = suffixRequirement;
    auto& prefixStateInputs = candidate.roleStateInputsByRole[prefixRole];
    auto& prefixStateOutputs = candidate.roleStateOutputsByRole[prefixRole];
    auto& suffixStateInputs = candidate.roleStateInputsByRole[suffixRole];
    auto& suffixStateOutputs = candidate.roleStateOutputsByRole[suffixRole];
    for (const auto& pair : pairs) {
      const bool prefix = static_cast<std::size_t>(pair.owner) <= cut.producerIndex;
      auto& inputs = prefix ? prefixStateInputs : suffixStateInputs;
      auto& outputs = prefix ? prefixStateOutputs : suffixStateOutputs;
      inputs.push_back(stateTensor(pair.key->input, *pair.key));
      outputs.push_back(stateTensor(pair.key->output, *pair.key));
      inputs.push_back(stateTensor(pair.value->input, *pair.value));
      outputs.push_back(stateTensor(pair.value->output, *pair.value));
    }
    candidate.estimatedCosts = {
      {"role_count", std::uint64_t(2)},
      {"transfer_bytes", cut.transferBytes},
      {"unknown_transfer_tensors", cut.transferBytes == 0 ? std::uint64_t(1) : std::uint64_t(0)}};
    candidate.candidateDigest = candidate.computedDigest();
    candidate.validate(graph);
    result.push_back(std::move(candidate));
  }
  return result;
}


std::vector<AutomaticOnnxStageBinding>
deriveAutomaticOnnxStageBindings(const AutomaticOnnxModel& model,
                                 const NativeSplitCandidate& candidate)
{
  const auto& graph = model.inspection.graph;
  if (candidate.graphDigest != graph.graphDigest ||
      !model.contract.generation)
    throw std::invalid_argument("stage projection requires the split graph and a generation proof");
  const auto& roles = candidate.executionPlan.roles;
  if (roles.empty()) throw std::invalid_argument("stage projection has no roles");
  std::set<std::string> roleSet(roles.begin(), roles.end());
  if (roleSet.size() != roles.size())
    throw std::invalid_argument("stage projection repeats a role");

  const auto producerNode = producerNodes(model.inspection);
  std::map<std::string, std::string> producerRole;
  std::map<std::string, std::set<std::string>> consumerRoles;
  for (const auto& edge : graph.edges) {
    const auto producer = candidate.nodeRoles.find(edge.producer);
    if (producer == candidate.nodeRoles.end())
      throw std::invalid_argument("stage projection has an unassigned producer node");
    producerRole[edge.id] = producer->second;
    for (const auto& consumer : edge.consumers) {
      const auto at = candidate.nodeRoles.find(consumer);
      if (at == candidate.nodeRoles.end())
        throw std::invalid_argument("stage projection has an unassigned consumer node");
      if (at->second != producer->second) consumerRoles[edge.id].insert(at->second);
    }
  }
  for (const auto& node : graph.nodes)
    if (candidate.nodeRoles.find(node.id) == candidate.nodeRoles.end())
      throw std::invalid_argument("stage projection has an unassigned node");

  std::map<std::string, AutomaticOnnxStageBinding> byRole;
  for (const auto& role : roles) {
    AutomaticOnnxStageBinding binding;
    binding.role = role;
    byRole.emplace(role, std::move(binding));
  }
  for (const auto& [node, role] : candidate.nodeRoles) byRole.at(role).nodeIds.push_back(node);
  for (const auto& edge : graph.edges) {
    const auto& producer = producerRole.at(edge.id);
    const auto consumers = consumerRoles.find(edge.id);
    if (consumers == consumerRoles.end()) continue;
    byRole.at(producer).activationOutputs.push_back(edge.id);
    for (const auto& consumer : consumers->second)
      byRole.at(consumer).activationInputs.push_back(edge.id);
  }

  const auto& generation = *model.contract.generation;
  std::size_t pairs = 0;
  for (std::size_t index = 0; index + 1 < generation.states.size(); ++index) {
    const auto& key = generation.states[index];
    const auto& value = generation.states[index + 1];
    if (key.role != AutomaticAttentionState::Role::KEY ||
        value.role != AutomaticAttentionState::Role::VALUE)
      continue;
    ++pairs;
    const auto keyNode = producerNode.find(bareName(key.output));
    const auto valueNode = producerNode.find(bareName(value.output));
    const auto keyRole = keyNode == producerNode.end() ? candidate.nodeRoles.end() :
      candidate.nodeRoles.find(keyNode->second);
    const auto valueRole = valueNode == producerNode.end() ? candidate.nodeRoles.end() :
      candidate.nodeRoles.find(valueNode->second);
    if (keyRole == candidate.nodeRoles.end() || valueRole == candidate.nodeRoles.end() ||
        keyRole->second != valueRole->second)
      throw std::invalid_argument("state pair does not have one stage owner");
    auto& stage = byRole.at(keyRole->second);
    stage.stateInputs.push_back(bareName(key.input));
    stage.stateInputs.push_back(bareName(value.input));
    stage.stateOutputs.push_back(bareName(key.output));
    stage.stateOutputs.push_back(bareName(value.output));
  }
  if (pairs == 0) throw std::invalid_argument("stage projection has no state pair");
  for (const auto& role : roles)
    if (byRole.at(role).stateOutputs.empty())
      throw std::invalid_argument("stage projection leaves a role without a state pair");

  const auto logitsNode = producerNode.find(bareName(generation.logitsOutput));
  const auto logitsRole = logitsNode == producerNode.end() ? candidate.nodeRoles.end() :
    candidate.nodeRoles.find(logitsNode->second);
  if (logitsRole == candidate.nodeRoles.end() ||
      logitsRole->second != candidate.resultEgressRole)
    throw std::invalid_argument("stage projection does not put logits on the terminal role");
  byRole.at(candidate.resultEgressRole).terminal = true;

  std::set<std::string> actualCuts;
  for (const auto& [tensor, consumers] : consumerRoles) actualCuts.insert(tensor);
  const std::set<std::string> declared(candidate.crossPartitionTensors.begin(),
                                       candidate.crossPartitionTensors.end());
  if (actualCuts != declared)
    throw std::invalid_argument("stage projection cuts do not match the frontier");

  std::vector<AutomaticOnnxStageBinding> result;
  result.reserve(roles.size());
  for (const auto& role : roles) result.push_back(std::move(byRole.at(role)));
  return result;
}


std::string
AutomaticOnnxStageRunnerContract::canonicalJson() const
{
  if (role.empty()) throw std::invalid_argument("stage runner contract has no role");
  auto inputsJson = NativeJson::array();
  for (const auto& name : inputs) inputsJson.push_back(name);
  auto outputsJson = NativeJson::array();
  for (const auto& name : outputs) outputsJson.push_back(name);
  return nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-stage-runner-v1"}, {"role", role},
    {"terminal", terminal}, {"inputs", std::move(inputsJson)},
    {"outputs", std::move(outputsJson)}});
}

AutomaticOnnxStageRunnerContract
AutomaticOnnxStageRunnerContract::fromJson(const std::string& encoded)
{
  const auto value = nativeParseJson(encoded);
  if (value.at("schema") != "ndnsf-di-automatic-stage-runner-v1")
    throw std::invalid_argument("stage runner contract schema is unknown");
  AutomaticOnnxStageRunnerContract result;
  result.role = value.at("role").get<std::string>();
  result.terminal = value.at("terminal").get<bool>();
  for (const auto& name : value.at("inputs")) result.inputs.push_back(name.get<std::string>());
  for (const auto& name : value.at("outputs")) result.outputs.push_back(name.get<std::string>());
  if (result.role.empty() || result.inputs.empty() || result.outputs.empty())
    throw std::invalid_argument("stage runner contract is incomplete");
  return result;
}

std::optional<std::string>
checkAutomaticOnnxStageRunner(const NativeModelRunnerSpec& spec,
                              const AutomaticOnnxStageBinding& binding,
                              const NativeSplitCandidate& candidate,
                              const AutomaticOnnxContract& contract)
{
  const auto found = spec.metadata.find("automaticStageBinding");
  if (found == spec.metadata.end()) return "AUTOMATIC_STAGE_RUNNER_CONTRACT_MISSING";
  AutomaticOnnxStageRunnerContract declared;
  try {
    declared = AutomaticOnnxStageRunnerContract::fromJson(found->second);
  }
  catch (const std::exception&) {
    return "AUTOMATIC_STAGE_RUNNER_CONTRACT_FORMAT";
  }
  if (declared.role != binding.role) return "AUTOMATIC_STAGE_RUNNER_ROLE";
  if (declared.terminal != binding.terminal) return "AUTOMATIC_STAGE_RUNNER_TERMINAL";
  if (!contract.generation) return "AUTOMATIC_STAGE_RUNNER_GENERATION";
  const auto& generation = *contract.generation;
  std::set<std::string> expectedInputs(binding.activationInputs.begin(),
                                       binding.activationInputs.end());
  expectedInputs.insert(binding.stateInputs.begin(), binding.stateInputs.end());
  if (binding.role == candidate.inputIngressRole) {
    for (const auto& name : {generation.tokenInput, generation.maskInput,
                             generation.positionInput})
      if (!name.empty()) expectedInputs.insert(bareName(name));
  }
  std::set<std::string> expectedOutputs(binding.activationOutputs.begin(),
                                        binding.activationOutputs.end());
  expectedOutputs.insert(binding.stateOutputs.begin(), binding.stateOutputs.end());
  if (binding.terminal) expectedOutputs.insert(bareName(generation.logitsOutput));
  const std::set<std::string> declaredInputs(declared.inputs.begin(), declared.inputs.end());
  const std::set<std::string> declaredOutputs(declared.outputs.begin(), declared.outputs.end());
  if (declaredInputs != expectedInputs) return "AUTOMATIC_STAGE_RUNNER_INPUTS";
  if (declaredOutputs != expectedOutputs) return "AUTOMATIC_STAGE_RUNNER_OUTPUTS";
  return std::nullopt;
}

} // namespace ndnsf::di
