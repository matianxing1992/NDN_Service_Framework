#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

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
  std::vector<std::uint64_t> tensorDegrees,
  std::string inputIngressRole,
  std::string resultEgressRole,
  std::string modelFamily)
  : m_layerRanges(std::move(layerRanges))
  , m_artifactDigestsByRole(std::move(artifactDigestsByRole))
  , m_weightBytesByRole(std::move(weightBytesByRole))
  , m_roles(std::move(roles))
  , m_tensorDegrees(std::move(tensorDegrees))
  , m_inputIngressRole(std::move(inputIngressRole))
  , m_resultEgressRole(std::move(resultEgressRole))
  , m_modelFamily(std::move(modelFamily))
{
  if (m_modelFamily != "qwen" && m_modelFamily != "llama")
    throw std::invalid_argument("native splitter model family is unsupported");
  if (m_roles.empty() || m_layerRanges.size() != m_roles.size() ||
      m_tensorDegrees.size() != m_roles.size() ||
      std::set<std::string>(m_roles.begin(), m_roles.end()).size() != m_roles.size() ||
      m_artifactDigestsByRole.size() != m_roles.size() ||
      m_weightBytesByRole.size() != m_roles.size()) {
    throw std::invalid_argument("native transformer splitter role cover is incomplete");
  }
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
    if (m_layerRanges[i].first >= m_layerRanges[i].second ||
        (i != 0 && m_layerRanges[i - 1].second != m_layerRanges[i].first) ||
        m_tensorDegrees[i] != 1 || m_weightBytesByRole.at(m_roles[i]) == 0 ||
        !isDigest(m_artifactDigestsByRole.at(m_roles[i]))) {
      throw std::invalid_argument("native transformer splitter configuration is unsupported");
    }
  }
  if (m_layerRanges.front().first != 0) {
    throw std::invalid_argument("native transformer splitter must start at layer zero");
  }
  if (m_inputIngressRole.empty() != m_resultEgressRole.empty() ||
      (!m_inputIngressRole.empty() &&
       (std::find(m_roles.begin(), m_roles.end(), m_inputIngressRole) == m_roles.end() ||
        std::find(m_roles.begin(), m_roles.end(), m_resultEgressRole) == m_roles.end()))) {
    throw std::invalid_argument("native transformer splitter ingress/egress role is undeclared");
  }
}

NativeStrategyIdentity NativeQwenLayerSplit::identity() const
{
  std::ostringstream canonical;
  canonical << m_modelFamily << "-layer-split|1|";
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
      canonical << m_roles[i] << ':' << m_layerRanges[i].first << '-' <<
      m_layerRanges[i].second << ':' << m_artifactDigestsByRole.at(m_roles[i]) << ';';
  }
  if (!m_inputIngressRole.empty())
    canonical << "ingress=" << m_inputIngressRole << ";egress=" << m_resultEgressRole << ';';
  return {"native-" + m_modelFamily + "-layer-split", "1", nativePlanningDigest(canonical.str())};
}

NativeGraphSnapshot NativeQwenLayerSplit::inspectGraph(const NativeModelDescriptor& model,
  const std::string& revision, std::uint64_t maxNodes) const
{
  model.validate();
  const auto layers = m_layerRanges.back().second;
  if (revision.empty() || maxNodes < 2 || layers > maxNodes - 2)
    throw std::invalid_argument("native transformer metadata revision or graph size is invalid");
  NativeGraphSnapshot result;
  result.topologicalOrder.push_back("embedding");
  for (std::uint64_t i = 0; i < layers; ++i)
    result.topologicalOrder.push_back("layer-" + std::string(i < 10 ? "0" : "") + std::to_string(i));
  result.topologicalOrder.push_back("final-norm-head");
  for (std::size_t i = 0; i < result.topologicalOrder.size(); ++i) {
    const auto& id = result.topologicalOrder[i];
    result.nodes.push_back({id, id, i});
    if (i == 0) continue;
    const auto edge = i == 1 ? "hidden-embedding-to-layer-00" :
      i == result.topologicalOrder.size() - 1 ? "hidden-layer-" + std::to_string(layers - 1) + "-to-final" :
      "hidden-layer-" + std::to_string(i - 2) + "-to-" + std::to_string(i - 1);
    result.edges.push_back({edge, result.topologicalOrder[i - 1], {id},
      {edge, model.precision, {std::string("batch"), std::string("sequence"), std::string("hidden")}, std::nullopt}});
    result.legalCutEdges.push_back(edge);
  }
  result.modelInputs = {{"input_ids", "int64", {std::string("batch"), std::string("sequence")}, std::nullopt}};
  result.modelOutputs = {{"logits", model.precision,
    {std::string("batch"), std::string("sequence"), std::string("vocabulary")}, std::nullopt}};
  auto ranges = NativeJson::array();
  for (const auto& range : m_layerRanges) ranges.push_back(NativeJson::array({range.first, range.second}));
  result.graphDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"model", model.modelName}, {"revision", revision}, {"precision", model.precision},
    {"decode_mode", "single-token-autoregressive"}, {"modality", "text-only"},
    {"mtp_enabled", false}, {"thinking_mode", "disabled"}, {"layer_ranges", ranges},
    {"nodes", result.topologicalOrder}, {"edges", result.legalCutEdges}, {"legal_cuts", result.legalCutEdges}}));
  result.validate(model);
  return result;
}

std::vector<NativeSplitCandidate> NativeQwenLayerSplit::enumerateFromMetadata(
  const NativeModelDescriptor& model, const std::string& revision, std::uint64_t maxNodes,
  const NativeCandidateBudget& budget) const
{
  budget.validate();
  return enumerate(model, inspectGraph(model, revision, maxNodes), budget);
}

std::vector<NativeSplitCandidate>
NativeQwenLayerSplit::enumerateImpl(const NativeModelDescriptor& model,
                                    const NativeGraphSnapshot& graph,
                                    const NativeCandidateBudget& budget,
                                    const ExtensionControl* control) const
{
  if (control) control->requireActive();
  budget.validate();
  model.validate();
  graph.validate(model);
  const bool adapterMatches = m_modelFamily == "qwen"
    ? (model.adapterId.find("qwen") != std::string::npos ||
       model.modelName.find("Qwen") != std::string::npos)
    : (model.adapterId == "llama" || model.adapterId.find("smollm") != std::string::npos ||
       model.modelName.find("SmolLM") != std::string::npos);
  if (!adapterMatches)
    throw std::invalid_argument("native transformer splitter received another adapter");
  const auto layerCount = m_layerRanges.back().second;
  if (graph.nodes.size() != layerCount + 2 ||
      graph.nodes.front().id != "embedding" ||
      graph.nodes.back().id != "final-norm-head") {
    throw std::invalid_argument("native transformer graph does not match the supported layer cover");
  }
  for (std::size_t i = 0; i < layerCount; ++i) {
    if (control) control->requireActive();
    const auto expected = std::string("layer-") +
      (i < 10 ? "0" : "") + std::to_string(i);
    if (graph.nodes[i + 1].id != expected) {
      throw std::invalid_argument("native transformer graph layer order is not canonical");
    }
  }

  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter = identity();
  candidate.model = model;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan.version = 1;
  candidate.executionPlan.modelName = model.modelName;
  candidate.executionPlan.modelFamily = m_modelFamily;
  candidate.executionPlan.modelFormat = model.modelFormat;
  candidate.executionPlan.plannerKind = "native-" + m_modelFamily + "-layer";
  candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
  candidate.executionPlan.roles = m_roles;
  candidate.inputIngressRole = m_inputIngressRole;
  candidate.resultEgressRole = m_resultEgressRole;
  candidate.nodeRoles["embedding"] = m_roles.front();
  candidate.nodeRoles["final-norm-head"] = m_roles.back();
  for (std::size_t i = 0; i + 1 < m_roles.size(); ++i) {
    if (control) control->requireActive();
    NativeDependencySpec dependency;
    dependency.producers = {m_roles[i]};
    dependency.consumers = {m_roles[i + 1]};
    dependency.keyScope = "native-" + m_modelFamily + "-activation";
    const auto topicFamily = m_modelFamily == "qwen" ? "QWEN" : "LLAMA";
    dependency.topicPrefix = std::string("/NDNSF/DI/") + topicFamily;
    dependency.objectNameTemplate =
      std::string("{producerProvider}/NDNSF/DI/") + topicFamily +
      "/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
    dependency.tensors = {"hidden-layer-" + std::to_string(m_layerRanges[i].second - 1) +
                          "-to-" + std::to_string(m_layerRanges[i].second)};
    dependency.operationKind = "ACTIVATION";
    candidate.executionPlan.dependencies.push_back(std::move(dependency));
    candidate.crossPartitionTensors.push_back(
      "hidden-layer-" + std::to_string(m_layerRanges[i].second - 1) +
      "-to-" + std::to_string(m_layerRanges[i].second));
  }
  for (std::size_t i = 0; i < m_roles.size(); ++i) {
    if (control) control->requireActive();
    const auto& role = m_roles[i];
    const auto& artifact = m_artifactDigestsByRole.at(role);
    std::vector<NativeTensorContract> stateInputs;
    std::vector<NativeTensorContract> stateOutputs;
    // The real Qwen3-0.6B export exposes one dynamic-past KV pair per decoder
    // layer. Keep the historical three-family contract for the small Qwen
    // fixture so existing generic streaming tests remain compatible; the
    // authenticated catalog mapping expands only the real export contract.
    const bool dynamicPast = m_modelFamily == "llama" ||
      model.modelName.find("Qwen3-0.6B") != std::string::npos;
    if (dynamicPast) {
      for (std::size_t layer = m_layerRanges[i].first;
           layer < m_layerRanges[i].second; ++layer) {
        const auto suffix = std::to_string(layer);
        stateInputs.push_back({"past_key." + suffix, model.precision,
                               {"batch", "heads", "sequence", "head-dimension"},
                               std::nullopt});
        stateInputs.push_back({"past_value." + suffix, model.precision,
                               {"batch", "heads", "sequence", "head-dimension"},
                               std::nullopt});
        stateOutputs.push_back({"present_key." + suffix, model.precision,
                                {"batch", "heads", "sequence", "head-dimension"},
                                std::nullopt});
        stateOutputs.push_back({"present_value." + suffix, model.precision,
                                {"batch", "heads", "sequence", "head-dimension"},
                                std::nullopt});
      }
    }
    else {
      for (const auto& family : {"attention_kv", "recurrent_state", "convolution_state"}) {
        stateInputs.push_back({std::string(family) + "_in", model.precision,
                               {"batch", "sequence", "hidden"}, std::nullopt});
        stateOutputs.push_back({std::string(family) + "_out", model.precision,
                                {"batch", "sequence", "hidden"}, std::nullopt});
      }
    }
    for (std::size_t layer = m_layerRanges[i].first; layer < m_layerRanges[i].second; ++layer) {
      if (control) control->requireActive();
      candidate.nodeRoles[graph.nodes[layer + 1].id] = role;
    }
    candidate.roleStateInputsByRole[role] = stateInputs;
    candidate.roleStateOutputsByRole[role] = stateOutputs;
    candidate.fragmentsByRole[role] = artifact;
    candidate.artifactsByRole[role] = {artifact};
    candidate.rankArtifactDigestsByRole[role] = {artifact};
    candidate.tensorDegreesByRole[role] = 1;
    candidate.requirementsByRole[role] = {
      {"onnxruntime"},
      m_weightBytesByRole.at(role), 1024ULL * 1024ULL * 1024ULL, 0,
      512ULL * 1024ULL * 1024ULL, 512ULL * 1024ULL * 1024ULL, 1.10};
  }
  candidate.estimatedCosts = {{"role_count", std::uint64_t(m_roles.size())},
    {"rank_count", std::uint64_t(m_roles.size())}, {"decoder_layers", std::uint64_t(m_layerRanges.back().second)},
    {"known_transfer_bytes", std::uint64_t(0)}, {"unknown_transfer_tensors", std::uint64_t(2)}};
  candidate.candidateDigest = candidate.computedDigest();
  candidate.validate(graph);
  return {std::move(candidate)};
}

std::vector<NativeSplitCandidate> NativeQwenLayerSplit::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget) const
{
  return enumerateImpl(model, graph, budget, nullptr);
}

std::vector<NativeSplitCandidate> NativeQwenLayerSplit::enumerate(
  const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
  const NativeCandidateBudget& budget, const ExtensionControl& control) const
{
  control.requireActive();
  auto result = enumerateImpl(model, graph, budget, &control);
  control.requireActive();
  return result;
}

} // namespace ndnsf::di::qwen
