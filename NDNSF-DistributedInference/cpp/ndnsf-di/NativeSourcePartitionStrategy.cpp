#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeSourcePartitionStrategy.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
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
       m_profile.inputChannels < 2 || m_profile.convInputAxis.empty()))
    throw std::invalid_argument("hybrid source partition requires four roles, boundary and channels");
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
    candidate.executionPlan.stageLayout = {
      {m_profile.roles[0] + "/stage", nativePlanningDigest("source-prefix|" + graph.graphDigest),
       {m_profile.roles[0]}},
      {m_profile.roles[1] + "/conv", hybridStageDigest,
       {m_profile.roles[1], m_profile.roles[2]}},
      {m_profile.roles[3] + "/stage", nativePlanningDigest("source-suffix|" + graph.graphDigest),
       {m_profile.roles[3]}},
    };
    // Structural step entries. T002 replaces these with the source-bound
    // CALL_SUBGRAPH/SUM recipes; they exist here so rank units can share the
    // boundary node without claiming a private graph node.
    for (const auto& role : m_profile.roles) {
      NativeExecutionStepSpec step;
      step.stepId = role + "/RUN";
      step.unitId = role;
      step.operation = "CALL_SUBGRAPH";
      step.artifactEntry = role + "/artifact";
      candidate.executionPlan.steps.push_back(std::move(step));
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
    NativeDependencySpec first;
    first.producers = {m_profile.roles[0]};
    first.consumers = {m_profile.roles[1], m_profile.roles[2]};
    first.keyScope = "native-source-activation";
    first.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
    first.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
    first.tensors = {m_profile.boundaryNode + "-input"};
    first.operationKind = "ACTIVATION";
    candidate.executionPlan.dependencies.push_back(std::move(first));
    for (const auto& rank : {m_profile.roles[1], m_profile.roles[2]}) {
      NativeDependencySpec partial;
      partial.producers = {rank};
      partial.consumers = {m_profile.roles[3]};
      partial.keyScope = "native-source-activation";
      partial.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
      partial.objectNameTemplate =
        "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
      partial.tensors = {rank + "-partial"};
      partial.operationKind = "ACTIVATION";
      candidate.executionPlan.dependencies.push_back(std::move(partial));
    }
  }

  const auto roleBytes = std::max<std::uint64_t>(1, knownBytes / m_profile.roles.size());
  for (const auto& role : m_profile.roles) {
    if (control) control->requireActive();
    std::vector<std::string> roleNodes;
    for (const auto& node : graph.topologicalOrder)
      if (ownerByNode.at(node) == role) roleNodes.push_back(node);
    candidate.fragmentsByRole[role] = nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"candidate", candidate.splitter.name}, {"graph", graph.graphDigest},
      {"role", role}, {"nodes", roleNodes}}));
    candidate.artifactsByRole[role] = {candidate.fragmentsByRole[role]};
    candidate.requirementsByRole[role] = {
      {"onnxruntime-cpu", "onnxruntime-cuda"},
      roleBytes, 256ULL * 1024ULL * 1024ULL, 0,
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
