#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include <cctype>
#include <limits>
#include <regex>

namespace ndnsf::di {
namespace {
bool digest(const std::string& s)
{
  return s.size() == 71 && s.compare(0, 7, "sha256:") == 0 &&
    std::all_of(s.begin() + 7, s.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string roleKind(std::string name)
{
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  std::istringstream parts(name);
  std::string part; bool tensor = false, stage = false, shard = false;
  while (std::getline(parts, part, '/')) {
    tensor |= part.find("tensor") != std::string::npos || part.find("rank") != std::string::npos;
    shard |= part.find("shard") != std::string::npos;
    stage |= part == "pipeline" || part == "stage" || part == "stages" || part.find("stage-") == 0;
  }
  return tensor || (shard && stage) ? "TENSOR_RANK" : stage ? "PIPELINE_RANGE" : "COMPONENT_SET";
}
NativeAssemblyTensorContractV3 tensor(const NativeJson& value)
{
  NativeAssemblyTensorContractV3 result;
  result.name = value.at("name"); result.dtype = value.at("dtype");
  for (const auto& d : value.at("shape")) {
    if (d.is_string()) result.shape.emplace_back(d.get<std::string>());
    else result.shape.emplace_back(d.get<std::int64_t>());
  }
  return result;
}
}

NativeCanonicalRolePreparer::NativeCanonicalRolePreparer(NativeInspectedModel model,
  const NativeCanonicalSource& source, NativeRoleRecipeProfile profile,
  const NativeAssemblyControl& control, NodeMap mapping)
  : m_model(std::move(model)), m_profile(std::move(profile)), m_mapping(std::move(mapping))
{
  if (!control.requireActive) throw std::invalid_argument("native role preparation requires an owner fence");
  control.requireActive();
  m_model.validate();
  const auto& p = m_profile;
  if (!digest(p.artifactProfileDigest) || !digest(p.assemblerDescriptorDigest) ||
      p.backendAbi.empty() || p.precision != m_model.descriptor.precision || p.quantization.empty() ||
      p.layout.empty() || p.padding.empty() || p.protectionEpoch.empty() || !p.maxSourceBytes ||
      !p.maxAssembledBytes || !p.maxNodes || p.maxNodes > std::uint64_t(std::numeric_limits<int>::max()))
    throw std::invalid_argument("native role recipe profile is incomplete");
  if (source.modelBytes.empty() || source.modelBytes.size() != m_model.canonicalSourceBytes ||
      source.modelBytes.size() > p.maxSourceBytes ||
      nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) != m_model.canonicalSourceDigest ||
      source.initializerBytes.has_value() != (m_model.canonicalInitializerBytes != 0) ||
      (source.initializerBytes && (source.initializerBytes->size() != m_model.canonicalInitializerBytes ||
        source.initializerBytes->size() > p.maxSourceBytes ||
        nativePlanningDigest(source.initializerBytes->data(), source.initializerBytes->size()) !=
          m_model.canonicalInitializerObjectDigest)))
    throw std::invalid_argument("native role source differs from inspected catalog");
  auto bounded = control;
  bounded.maxSourceBytes = std::min(control.maxSourceBytes, p.maxSourceBytes);
  bounded.maxAssembledBytes = std::min(control.maxAssembledBytes, p.maxAssembledBytes);
  m_sourceGraph = inspectNativeOnnxSourceGraph(source, m_model.descriptor, bounded);
  if (m_sourceGraph.canonicalIdentity.graphDigest != m_model.canonicalGraphDigest ||
      m_sourceGraph.graph.nodes.size() > p.maxNodes)
    throw std::invalid_argument("native role canonical source identity or node bound differs");
  checkOnnxAssemblerDescriptorBinding(p.assemblerDescriptorDigest, source, bounded);
  if (m_mapping.empty()) {
    if (m_sourceGraph.graph.graphDigest != m_model.graph.graphDigest)
      throw std::invalid_argument("native semantic graph requires an explicit ONNX node mapping");
    for (const auto& item : m_sourceGraph.canonicalNodeIndices) m_mapping[item.first] = {item.second};
  }
  std::set<std::uint64_t> covered;
  if (m_mapping.size() != m_model.graph.nodes.size())
    throw std::invalid_argument("native semantic mapping has a foreign node cover");
  for (const auto& node : m_model.graph.nodes) {
    const auto found = m_mapping.find(node.id);
    if (found == m_mapping.end() || found->second.empty())
      throw std::invalid_argument("native semantic mapping omits a planning node");
    for (auto index : found->second)
      if (index >= m_sourceGraph.graph.nodes.size() || !covered.insert(index).second)
        throw std::invalid_argument("native semantic mapping repeats or exceeds source nodes");
  }
  if (covered.size() != m_sourceGraph.graph.nodes.size())
    throw std::invalid_argument("native semantic mapping omits source nodes");
}

NativeRequestPreparation::RolePort NativeCanonicalRolePreparer::rolePort() const
{
  return [owner = *this](const auto& model, const auto& candidate, const auto& control) {
    return owner.prepare(model, candidate, control);
  };
}

std::vector<NativeSelectionRoleV3> NativeCanonicalRolePreparer::prepare(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeRequestControl& control) const
{
  control.requireActive(); model.validate();
  if (model.descriptor.canonicalJson() != m_model.descriptor.canonicalJson() ||
      model.graph.graphDigest != m_model.graph.graphDigest || model.modelManifestDigest != m_model.modelManifestDigest ||
      model.canonicalGraphDigest != m_model.canonicalGraphDigest || model.canonicalSourceName != m_model.canonicalSourceName ||
      model.canonicalSourceDigest != m_model.canonicalSourceDigest || model.canonicalSourceBytes != m_model.canonicalSourceBytes ||
      model.canonicalInitializerBytes != m_model.canonicalInitializerBytes ||
      model.canonicalInitializerObjectDigest != m_model.canonicalInitializerObjectDigest)
    throw std::invalid_argument("native role preparation model differs from frozen source");
  candidate.validate(m_model.graph);
  const auto metadata = nativeParseJson(m_sourceGraph.graphMetadataJson);
  const auto postprocessing = nativeParseJson(candidate.postprocessingJson);
  const std::regex layerPattern("layer-([0-9]+)");
  std::vector<NativeSelectionRoleV3> result;
  for (const auto& name : candidate.executionPlan.roles) {
    control.requireActive();
    std::set<std::uint64_t> owned, layers;
    std::vector<std::uint64_t> planningIndices;
    for (std::size_t i = 0; i < m_model.graph.topologicalOrder.size(); ++i) {
      const auto& id = m_model.graph.topologicalOrder[i];
      if (candidate.nodeRoles.at(id) != name) continue;
      planningIndices.push_back(i);
      const auto& indices = m_mapping.at(id); owned.insert(indices.begin(), indices.end());
      std::smatch match;
      if (std::regex_match(id, match, layerPattern) && !layers.insert(std::stoull(match[1])).second)
        throw std::invalid_argument("native role repeats a semantic layer number");
    }
    if (!layers.empty() && (*layers.rbegin() == std::numeric_limits<std::uint64_t>::max() ||
        *layers.rbegin() - *layers.begin() + 1 != layers.size()))
      throw std::invalid_argument("native role layer nodes are not a contiguous range");
    std::map<std::string, NativeAssemblyTensorContractV3> inputs, outputs;
    const auto initializers = metadata.at("initializers").get<std::vector<std::string>>();
    for (const auto& item : metadata.at("tensorConsumers").items()) {
      if (std::find(initializers.begin(), initializers.end(), item.key()) != initializers.end()) continue;
      const auto consumers = item.value().get<std::vector<std::uint64_t>>();
      const auto producer = metadata.at("tensorProducers").find(item.key());
      if (std::any_of(consumers.begin(), consumers.end(), [&](auto i) { return owned.count(i); }) &&
          (producer == metadata.at("tensorProducers").end() || !owned.count(producer->get<std::uint64_t>())))
        inputs.emplace(item.key(), tensor(metadata.at("tensors").at(item.key())));
      if (producer != metadata.at("tensorProducers").end() && owned.count(producer->get<std::uint64_t>()) &&
          std::any_of(consumers.begin(), consumers.end(), [&](auto i) { return !owned.count(i); }))
        outputs.emplace(item.key(), tensor(metadata.at("tensors").at(item.key())));
    }
    for (const auto& item : m_sourceGraph.graph.modelOutputs) {
      const auto producer = metadata.at("tensorProducers").find(item.name);
      if (producer != metadata.at("tensorProducers").end() && owned.count(producer->get<std::uint64_t>()))
        outputs.emplace(item.name, NativeAssemblyTensorContractV3{item.name, item.dtype, item.shape});
    }
    if (inputs.empty() || outputs.empty())
      throw std::invalid_argument("native role source boundary requires input and output tensors");
    const auto checkState = [&](const auto& declared, const auto& actual) {
      const auto found = declared.find(name);
      if (found == declared.end()) return;
      for (const auto& item : found->second) {
        const auto value = actual.find(item.name);
        if (value == actual.end() || value->second.dtype != item.dtype || value->second.shape != item.shape)
          throw std::invalid_argument("native role state contract differs from source boundary");
      }
    };
    checkState(candidate.roleStateInputsByRole, inputs); checkState(candidate.roleStateOutputsByRole, outputs);
    const auto degreeEntry = candidate.tensorDegreesByRole.find(name);
    const auto degree = degreeEntry == candidate.tensorDegreesByRole.end() ? 1 : degreeEntry->second;
    if (!degree || degree > 1024) throw std::invalid_argument("native role degree is invalid");
    const auto& requirement = candidate.requirementsByRole.at(name);
    const auto peak = requirement.estimatedPeakGpuMemoryBytes();
    if (!peak) throw std::invalid_argument("native role requires a known runtime memory bound");
    const auto artifacts = candidate.rankArtifactDigestsByRole.find(name);
    const auto& digests = artifacts == candidate.rankArtifactDigestsByRole.end() ? candidate.artifactsByRole.at(name) : artifacts->second;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      NativeSelectionRoleV3 role;
      role.role = name; role.selectedRole = degree == 1 ? name : name + "#" + std::to_string(rank);
      role.rank = rank; role.roleKind = degree > 1 ? "HYBRID_RANK" : roleKind(name);
      if (role.roleKind != "COMPONENT_SET") {
        role.layerBegin = layers.empty() ? 0 : *layers.begin();
        role.layerEnd = layers.empty() ? planningIndices.size() : *layers.rbegin() + 1;
      }
      role.backend = requirement.backends.front(); role.requiredDeviceMemoryMb = *peak / 1048576 + (*peak % 1048576 != 0);
      role.artifactDigest = digests.at(rank); role.adapterId = model.descriptor.adapterId;
      role.adapterVersion = model.descriptor.adapterVersion; role.modelManifestDigest = model.modelManifestDigest;
      role.artifactProfileDigest = m_profile.artifactProfileDigest; role.graphDigest = model.canonicalGraphDigest;
      role.adapterDescriptorDigest = model.descriptor.adapter.descriptorDigest(); role.protectionEpoch = m_profile.protectionEpoch;
      for (const auto& item : inputs) role.expectedInputs.push_back(item.second);
      for (const auto& item : outputs) role.expectedOutputs.push_back(item.second);
      if (name == candidate.resultEgressRole) {
        role.mergeKind = candidate.mergeKind;
        role.postprocessIdentity = postprocessing.value("identity", std::string{});
        role.postprocessOutputName = postprocessing.value("outputName", std::string{});
        role.postprocessConfidenceThreshold = postprocessing.value("confidenceThreshold", 0.0);
        role.postprocessSort = postprocessing.value("sort", std::string{});
      }
      if (role.mergeKind == "NATIVE_POSTPROCESS") {
        role.nodeIndices = planningIndices;
        role.recipeDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{{"candidate", candidate.candidateDigest},
          {"role", name}, {"rank", rank}, {"tensorDegree", degree}, {"begin", role.layerBegin}, {"end", role.layerEnd},
          {"backends", requirement.backends}}));
      }
      else {
        role.nodeIndices.assign(owned.begin(), owned.end());
        role.canonicalInitializerDigest = m_sourceGraph.canonicalIdentity.initializerDigest;
        role.assemblerDescriptorDigest = m_profile.assemblerDescriptorDigest; role.backendAbi = m_profile.backendAbi;
        role.precision = m_profile.precision; role.quantization = m_profile.quantization;
        role.layout = m_profile.layout; role.padding = m_profile.padding;
        role.maxSourceBytes = m_profile.maxSourceBytes; role.maxAssembledBytes = m_profile.maxAssembledBytes; role.maxNodes = m_profile.maxNodes;
        role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
      }
      result.push_back(std::move(role));
    }
  }
  control.requireActive();
  NativeRequestPreparation::validateRoles(m_model, candidate, result);
  return result;
}

} // namespace ndnsf::di
