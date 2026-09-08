#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <sstream>

namespace ndnsf::di::qwen {
namespace {

bool isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

NativeQwenLayerSplit::NativeQwenLayerSplit(
  std::vector<LayerRange> layerRanges,
  std::map<std::string, std::string> artifactDigestsByRole,
  std::map<std::string, std::uint64_t> weightBytesByRole,
  std::vector<std::string> roles,
  std::vector<std::uint64_t> tensorDegrees)
  : m_layerRanges(std::move(layerRanges))
  , m_artifactDigestsByRole(std::move(artifactDigestsByRole))
  , m_weightBytesByRole(std::move(weightBytesByRole))
  , m_roles(std::move(roles))
  , m_tensorDegrees(std::move(tensorDegrees))
{
  if (m_roles.empty() || m_layerRanges.size() != m_roles.size() ||
      m_tensorDegrees.size() != m_roles.size() ||
      std::set<std::string>(m_roles.begin(), m_roles.end()).size() != m_roles.size() ||
      m_artifactDigestsByRole.size() != m_roles.size() ||
      m_weightBytesByRole.size() != m_roles.size()) {
    throw std::invalid_argument("Qwen native splitter role cover is incomplete");
  }
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
    if (m_layerRanges[i].first >= m_layerRanges[i].second ||
        (i != 0 && m_layerRanges[i - 1].second != m_layerRanges[i].first) ||
        m_tensorDegrees[i] != 1 || m_weightBytesByRole.at(m_roles[i]) == 0 ||
        !isDigest(m_artifactDigestsByRole.at(m_roles[i]))) {
      throw std::invalid_argument("Qwen native splitter configuration is unsupported");
    }
  }
  if (m_layerRanges.front().first != 0) {
    throw std::invalid_argument("Qwen native splitter must start at layer zero");
  }
}

NativeStrategyIdentity NativeQwenLayerSplit::identity() const
{
  std::ostringstream canonical;
  canonical << "qwen-layer-split|1|";
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
    canonical << m_roles[i] << ':' << m_layerRanges[i].first << '-' <<
      m_layerRanges[i].second << ':' << m_artifactDigestsByRole.at(m_roles[i]) << ';';
  }
  return {"native-qwen-layer-split", "1", nativePlanningDigest(canonical.str())};
}

std::vector<NativeSplitCandidate>
NativeQwenLayerSplit::enumerate(const NativeModelDescriptor& model,
                                 const NativeGraphSnapshot& graph,
                                 const NativeCandidateBudget& budget) const
{
  budget.validate();
  model.validate();
  graph.validate(model);
  if (model.adapterId.find("qwen") == std::string::npos &&
      model.modelName.find("Qwen") == std::string::npos) {
    throw std::invalid_argument("Qwen native splitter received another adapter");
  }
  const auto layerCount = m_layerRanges.back().second;
  if (graph.nodes.size() != layerCount + 2 ||
      graph.nodes.front().id != "embedding" ||
      graph.nodes.back().id != "final-norm-head") {
    throw std::invalid_argument("Qwen graph does not match the supported layer cover");
  }
  for (std::size_t i = 0; i < layerCount; ++i) {
    const auto expected = std::string("layer-") +
      (i < 10 ? "0" : "") + std::to_string(i);
    if (graph.nodes[i + 1].id != expected) {
      throw std::invalid_argument("Qwen graph layer order is not canonical");
    }
  }

  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter = identity();
  candidate.model = model;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan.version = 1;
  candidate.executionPlan.modelName = model.modelName;
  candidate.executionPlan.modelFamily = "qwen";
  candidate.executionPlan.modelFormat = model.modelFormat;
  candidate.executionPlan.plannerKind = "native-qwen-layer";
  candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
  candidate.executionPlan.roles = m_roles;
  for (std::size_t i = 0; i + 1 < m_roles.size(); ++i) {
    NativeDependencySpec dependency;
    dependency.producers = {m_roles[i]};
    dependency.consumers = {m_roles[i + 1]};
    dependency.keyScope = "native-qwen-activation";
    dependency.topicPrefix = "/NDNSF/DI/QWEN";
    dependency.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
    dependency.tensors = {"hidden-layer-" + std::to_string(m_layerRanges[i].second - 1) +
                          "-to-" + std::to_string(m_layerRanges[i].second)};
    dependency.operationKind = "ACTIVATION";
    candidate.executionPlan.dependencies.push_back(std::move(dependency));
    candidate.crossPartitionTensors.push_back(
      "hidden-layer-" + std::to_string(m_layerRanges[i].second - 1) +
      "-to-" + std::to_string(m_layerRanges[i].second));
  }
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
    const auto& role = m_roles[i];
    const auto& artifact = m_artifactDigestsByRole.at(role);
    candidate.fragmentsByRole[role] = artifact;
    candidate.artifactsByRole[role] = {artifact};
    candidate.rankArtifactDigestsByRole[role] = {artifact};
    candidate.tensorDegreesByRole[role] = 1;
    candidate.requirementsByRole[role] = {
      {"onnxruntime"},
      m_weightBytesByRole.at(role), 1024ULL * 1024ULL * 1024ULL,
      512ULL * 1024ULL * 1024ULL, 512ULL * 1024ULL * 1024ULL, 1.10};
  }
  candidate.inputIngressRole = m_roles.front();
  candidate.resultEgressRole = m_roles.back();
  candidate.candidateDigest = nativePlanningDigest(
    "qwen-candidate|" + model.contentDigest + "|" + graph.graphDigest + "|" +
    identity().configurationDigest);
  candidate.validate(graph);
  return {std::move(candidate)};
}

} // namespace ndnsf::di::qwen
