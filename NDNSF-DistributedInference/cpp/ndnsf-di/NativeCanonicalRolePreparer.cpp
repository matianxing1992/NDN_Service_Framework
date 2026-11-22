#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <ndn-cxx/name.hpp>

#include <cctype>
#include <limits>
#include <optional>
#include <regex>
#include <set>

namespace ndnsf::di {
namespace {
bool digest(const std::string& s)
{
  return s.size() == 71 && s.compare(0, 7, "sha256:") == 0 &&
    std::all_of(s.begin() + 7, s.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string
assembledArtifactCoordinate(const NativeSplitCandidate& candidate,
                            const NativeSelectionRoleV3& role,
                            const std::vector<std::uint64_t>& planningIndices,
                            const std::set<std::uint64_t>& layers, bool wholeGraph,
                            const std::optional<qwen::QwenTpRankRange>& uniformSlice)
{
  const auto& plan = candidate.executionPlan;
  const auto stage = std::find_if(plan.stageLayout.begin(), plan.stageLayout.end(),
    [&](const auto& value) {
      return std::find(value.unitRoleIds.begin(), value.unitRoleIds.end(), role.role) !=
             value.unitRoleIds.end();
    });
  const auto roleIndex = std::distance(plan.roles.begin(),
    std::find(plan.roles.begin(), plan.roles.end(), role.role));
  const auto stageIndex = stage == plan.stageLayout.end() ? roleIndex :
    std::distance(plan.stageLayout.begin(), stage);
  // V1 step plans have one logical stage whose roles are the tensor ranks.
  const auto stageCount = plan.stepPlanVersion == NATIVE_STEP_PLAN_VERSION_V1 ? 1U :
    plan.stageLayout.empty() ? plan.roles.size() : plan.stageLayout.size();
  const bool sharded = !candidate.tensorIdentityByRole.empty() ||
    std::any_of(candidate.tensorDegreesByRole.begin(), candidate.tensorDegreesByRole.end(),
      [](const auto& value) { return value.second > 1; });
  const bool hybrid = sharded && stageCount > 1;
  const auto decimal = [](auto value) { return std::to_string(value); };
  ndn::Name coordinate;
  if (candidate.splitter.name == "native-automatic-onnx-stage") {
    if (planningIndices.empty())
      throw std::invalid_argument("automatic artifact has no source nodes");
    const auto begin = planningIndices.front();
    const auto end = planningIndices.back() + 1;
    const bool prefix = begin == 0;
    if (stageCount != 2 || end - begin != planningIndices.size() ||
        (prefix ? role.role != candidate.inputIngressRole
                : role.role != candidate.resultEgressRole))
      throw std::invalid_argument("automatic artifact has an invalid source cut");
    coordinate.append("graph").append("automatic").append("cut")
      .append(decimal(prefix ? end : begin)).append("side").append(prefix ? "prefix" : "suffix");
  }
  else if (!layers.empty() && (hybrid || role.tensorParallelDegree > 1 || !wholeGraph)) {
    if (hybrid)
      coordinate.append("hybrid").append("stage").append(decimal(stageIndex));
    else if (role.tensorParallelDegree > 1)
      coordinate.append("tp");
    else
      coordinate.append("pp").append("stage").append(decimal(stageIndex));
    coordinate.append("layers").append(decimal(*layers.begin())).append(decimal(*layers.rbegin() + 1));
    if (hybrid || role.tensorParallelDegree > 1)
      coordinate.append("degree").append(decimal(std::max<std::uint64_t>(1, role.tensorParallelDegree)))
        .append("rank").append(decimal(role.tensorParallelRank));
  }
  else if (wholeGraph) {
    coordinate.append("graph").append("full");
  }
  else {
    coordinate.append("graph").append("semantic").append("component")
      .append(ndn::name::Component(role.role));
  }

  if (!role.sourceTransformV1.empty() && !role.tensorShardV1.empty()) {
    if (nativeParseJson(role.sourceTransformV1).value("schema", std::string{}) ==
        NATIVE_SOURCE_ENTRY_PROGRAM_V1) {
      const auto program = NativeOnnxSourceEntryProgramV1::parse(role.sourceTransformV1);
      coordinate.append("entry").append("region").append("profile")
        .append("program-" + nativePlanningDigest(program.canonicalJson()).substr(7));
      return coordinate.toUri(ndn::name::UriFormat::ALTERNATE);
    }
    const auto transform = NativeOnnxSourceTransformV1::parse(role.sourceTransformV1);
    const auto shard = parseNativeTensorShardV1(role.tensorShardV1);
    const auto& axes = shard.at("axes");
    const bool ffnPair = transform.kind == "ffn-pair-partial";
    if ((!ffnPair && transform.kind != "conv-input-channel-partial") || axes.size() != 1 ||
        axes[0].at("axis") != transform.axis || axes[0].at("begin") != transform.begin ||
        axes[0].at("end") != transform.end || axes[0].at("extent") != transform.extent)
      throw std::invalid_argument("ONNX source artifact has invalid shard coordinates");
    coordinate.append("entry").append("region").append("profile")
      .append(std::string(ffnPair ? "ffn-channels-" : "channels-") +
              decimal(transform.begin) + "-" + decimal(transform.end));
    return coordinate.toUri(ndn::name::UriFormat::ALTERNATE);
  }

  auto slice = uniformSlice;
  if (!role.tensorShardV1.empty()) {
    const auto shard = parseNativeTensorShardV1(role.tensorShardV1);
    const auto& axes = shard.at("axes");
    if (axes.size() != 2 || axes[0].at("axis") != "ffn-channel" || axes[1].at("axis") != "kv-group")
      throw std::invalid_argument("Qwen artifact has invalid shard coordinates");
    slice = qwen::QwenTpRankRange{
      axes[1].at("begin").get<std::uint64_t>(), axes[1].at("end").get<std::uint64_t>(),
      axes[0].at("begin").get<std::uint64_t>(), axes[0].at("end").get<std::uint64_t>()};
  }
  const auto profile = slice ? "kv-" + decimal(slice->kvBegin) + "-" + decimal(slice->kvEnd) +
    "-ffn-" + decimal(slice->ffnBegin) + "-" + decimal(slice->ffnEnd) :
    role.tensorParallelDegree > 1 ? "uniform" : "whole";
  coordinate.append("entry").append("region").append("profile").append(profile);
  return coordinate.toUri(ndn::name::UriFormat::ALTERNATE);
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

} // namespace

NativeSourceRegionBoundary
inspectNativeSourceBoundary(const NativeGraphSnapshot& graph, const NativeJson& metadata,
               const std::set<std::uint64_t>& owned,
               const std::vector<std::string>& additionalOutputs,
               const std::function<void()>& requireActive)
{
  NativeSourceRegionBoundary result;
  const auto initializers = metadata.at("initializers").get<std::set<std::string>>();
  for (const auto& item : metadata.at("tensorConsumers").items()) {
    requireActive();
    if (initializers.count(item.key()) != 0)
      continue;
    const auto consumers = item.value().get<std::vector<std::uint64_t>>();
    const auto producer = metadata.at("tensorProducers").find(item.key());
    if (std::any_of(consumers.begin(), consumers.end(),
                    [&] (auto i) { return owned.count(i); }) &&
        (producer == metadata.at("tensorProducers").end() ||
         !owned.count(producer->get<std::uint64_t>())))
      result.inputs.emplace(item.key(), tensor(metadata.at("tensors").at(item.key())));
    if (producer != metadata.at("tensorProducers").end() &&
        owned.count(producer->get<std::uint64_t>()) &&
        std::any_of(consumers.begin(), consumers.end(),
                    [&] (auto i) { return !owned.count(i); }))
      result.outputs.emplace(item.key(), tensor(metadata.at("tensors").at(item.key())));
  }
  for (const auto& item : graph.modelOutputs) {
    requireActive();
    const auto producer = metadata.at("tensorProducers").find(item.name);
    if (producer != metadata.at("tensorProducers").end() &&
        owned.count(producer->get<std::uint64_t>()))
      result.outputs.emplace(item.name,
        NativeAssemblyTensorContractV3{item.name, item.dtype, item.shape});
  }
  for (const auto& name : additionalOutputs) {
    requireActive();
    const auto producer = metadata.at("tensorProducers").find(name);
    if (producer == metadata.at("tensorProducers").end() ||
        !owned.count(producer->get<std::uint64_t>()))
      throw std::invalid_argument("native role state output is not produced by its source role");
    result.outputs.emplace(name, tensor(metadata.at("tensors").at(name)));
  }
  if (result.inputs.empty() || result.outputs.empty())
    throw std::invalid_argument("native role source boundary requires input and output tensors");
  requireActive();
  return result;
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
  auto bounded = control;
  bounded.maxSourceBytes = std::min(control.maxSourceBytes, p.maxSourceBytes);
  bounded.maxAssembledBytes = std::min(control.maxAssembledBytes, p.maxAssembledBytes);
  if (source.modelBytes.empty() || source.modelBytes.size() != m_model.canonicalSourceBytes ||
      source.modelBytes.size() > p.maxSourceBytes ||
      nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) != m_model.canonicalSourceDigest ||
      (source.initializerBytes || source.initializerRangeSource) !=
        (m_model.canonicalInitializerBytes != 0) ||
      ((source.initializerBytes || source.initializerRangeSource) &&
       (nativeCanonicalInitializerByteSize(source) != m_model.canonicalInitializerBytes ||
        nativeCanonicalInitializerByteSize(source) > p.maxSourceBytes ||
        nativeCanonicalInitializerDigest(source, bounded) !=
          m_model.canonicalInitializerObjectDigest)))
    throw std::invalid_argument("native role source differs from inspected catalog");
  m_sourceGraph = inspectNativeOnnxSourceGraph(source, m_model.descriptor, bounded);
  checkOnnxAssemblerDescriptorBinding(p.assemblerDescriptorDigest, source, bounded);
  validateFrozen(control);
}

NativeCanonicalRolePreparer::NativeCanonicalRolePreparer(NativeInspectedModel model,
  NativeOnnxGraphInspection sourceGraph, NativeRoleRecipeProfile profile,
  const NativeAssemblyControl& control, NodeMap mapping)
  : m_model(std::move(model)), m_profile(std::move(profile)),
    m_sourceGraph(std::move(sourceGraph)), m_mapping(std::move(mapping))
{
  if (!control.requireActive) throw std::invalid_argument("native role preparation requires an owner fence");
  control.requireActive();
  validateFrozen(control);
}

void NativeCanonicalRolePreparer::validateFrozen(const NativeAssemblyControl& control)
{
  control.requireActive();
  m_model.validate();
  const auto& p = m_profile;
  if (!digest(p.artifactProfileDigest) || !digest(p.assemblerDescriptorDigest) ||
      p.backendAbi.empty() || p.precision != m_model.descriptor.precision ||
      p.quantization != m_model.descriptor.quantizationSubtype ||
      p.layout.empty() || p.padding.empty() || p.protectionEpoch.empty() || !p.maxSourceBytes ||
      !p.maxAssembledBytes || !p.maxNodes || p.maxNodes > std::uint64_t(std::numeric_limits<int>::max()))
    throw std::invalid_argument("native role recipe profile is incomplete");
  if (m_sourceGraph.canonicalIdentity.graphDigest != m_model.canonicalGraphDigest ||
      (m_model.canonicalInitializerBytes != 0 &&
       m_sourceGraph.canonicalIdentity.initializerDigest != m_model.canonicalInitializerDigest) ||
      m_sourceGraph.graph.nodes.size() > p.maxNodes || m_sourceGraph.graphMetadataJson.empty())
    throw std::invalid_argument("native role canonical source identity or node bound differs");
  const auto metadata = nativeParseJson(m_sourceGraph.graphMetadataJson);
  const auto initializers = metadata.at("initializers").get<std::vector<std::string>>();
  if (m_sourceGraph.initializerConsumers.size() != initializers.size())
    throw std::invalid_argument("native initializer ownership is incomplete; reprepare source");
  for (const auto& name : initializers) {
    const auto found = m_sourceGraph.initializerConsumers.find(name);
    if (found == m_sourceGraph.initializerConsumers.end() ||
        std::any_of(found->second.begin(), found->second.end(), [&](auto index) {
          return index >= m_sourceGraph.graph.nodes.size();
        }))
      throw std::invalid_argument("native initializer ownership differs from source graph");
  }
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

NativeSourceRegionBoundary
NativeCanonicalRolePreparer::sourceRegion(const std::vector<std::string>& nodeIds,
  const std::vector<std::string>& additionalOutputs, const NativeRequestControl& control) const
{
  control.requireActive();
  if (nodeIds.empty() || nodeIds.size() > m_mapping.size())
    throw std::invalid_argument("native source region exceeds its semantic node bound");
  std::set<std::string> unique;
  std::set<std::uint64_t> owned;
  for (const auto& id : nodeIds) {
    control.requireActive();
    const auto found = m_mapping.find(id);
    if (found == m_mapping.end() || !unique.insert(id).second)
      throw std::invalid_argument("native source region repeats or names a foreign semantic node");
    owned.insert(found->second.begin(), found->second.end());
  }
  const auto metadata = nativeParseJson(m_sourceGraph.graphMetadataJson);
  if (additionalOutputs.size() > metadata.at("tensors").size())
    throw std::invalid_argument("native source region exceeds its output tensor bound");
  auto result = inspectNativeSourceBoundary(m_sourceGraph.graph, metadata, owned, additionalOutputs,
                                                   [&] { control.requireActive(); });
  const auto ports = [&control] (const auto& contracts) {
    auto values = NativeJson::object();
    for (const auto& [name, contract] : contracts) {
      control.requireActive();
      auto shape = NativeJson::array();
      for (const auto& dimension : contract.shape)
        std::visit([&shape] (const auto& value) { shape.push_back(value); }, dimension);
      values[name] = {{"dtype", contract.dtype}, {"shape", std::move(shape)}};
    }
    return values;
  };
  result.identityDigest = nativePlanningDigest(nativeCanonicalJson({
    {"schema", "ndnsf-di-onnx-source-region-v1"},
    {"source_digest", m_model.canonicalSourceDigest},
    {"initializer_digest", m_model.canonicalInitializerObjectDigest},
    {"graph_digest", m_model.canonicalGraphDigest},
    {"node_indices", owned}, {"inputs", ports(result.inputs)}, {"outputs", ports(result.outputs)}
  }));
  return result;
}

NativeSplitCandidate NativeCanonicalRolePreparer::bindStateContracts(const NativeInspectedModel& model,
  const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
  const NativeRequestControl& control, NativeStateContractBinding binding) const
{
  control.requireActive();
  candidate.validate(m_model.graph);
  const bool sourceDefined = binding == NativeStateContractBinding::SOURCE;
  if (!sourceDefined && binding != NativeStateContractBinding::DECLARED)
    throw std::invalid_argument("unknown native state contract binding");
  if (sourceDefined) {
    for (const auto* side : {&mapping.inputs, &mapping.outputs}) {
      if (side->size() != candidate.executionPlan.roles.size())
        throw std::invalid_argument("source state mapping has an incomplete role cover");
      for (const auto& role : candidate.executionPlan.roles) {
        const auto found = side->find(role);
        if (found == side->end())
          throw std::invalid_argument("source state mapping omits a role state contract");
      }
    }
    // Empty is an explicit V2 contract, not permission to omit a role or erase required state.
    for (const auto& role : candidate.executionPlan.roles) {
      const bool noInputs = mapping.inputs.at(role).empty();
      const bool noOutputs = mapping.outputs.at(role).empty();
      if (noInputs != noOutputs)
        throw std::invalid_argument("source state mapping has one-sided stateless I/O");
      if (!noInputs) continue;
      const auto requiredInput = candidate.roleStateInputsByRole.find(role);
      const auto requiredOutput = candidate.roleStateOutputsByRole.find(role);
      if (candidate.executionPlan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 ||
          std::any_of(candidate.executionPlan.steps.begin(), candidate.executionPlan.steps.end(),
            [&role] (const auto& step) {
              return step.unitId == role && !step.localStateSlots.empty();
            }) ||
          (requiredInput != candidate.roleStateInputsByRole.end() &&
           !requiredInput->second.empty()) ||
          (requiredOutput != candidate.roleStateOutputsByRole.end() &&
           !requiredOutput->second.empty()))
        throw std::invalid_argument("source stateless mapping erases a required state contract");
    }
  }
  // Reuse the checked source-boundary producer without treating abstract
  // state shapes as concrete export shapes. This temporary candidate is never
  // published or returned, and all non-state identity checks still run.
  auto sourceCandidate = candidate;
  sourceCandidate.roleStateInputsByRole.clear();
  sourceCandidate.roleStateOutputsByRole.clear();
  sourceCandidate.candidateDigest = sourceCandidate.computedDigest();
  const auto roles = prepare(model, sourceCandidate, control);
  const auto metadata = nativeParseJson(m_sourceGraph.graphMetadataJson);
  auto result = candidate;
  const auto bind = [&](const auto& declared, const NativeStateTensorMapping::Roles& mapped,
                        auto& destination, bool input) {
    if (!sourceDefined && declared.size() != mapped.size())
      throw std::invalid_argument("native state mapping has an incomplete role cover");
    destination.clear();
    const auto bindRole = [&](const std::string& roleName,
                              const std::vector<NativeTensorContract>* expected) {
      control.requireActive();
      const auto found = mapped.find(roleName);
      if (found == mapped.end() || (expected && found->second.size() != expected->size()))
        throw std::invalid_argument("native state mapping has an incomplete semantic cover");
      const auto role = std::find_if(roles.begin(), roles.end(), [&](const auto& value) {
        return value.role == roleName && value.rank == 0;
      });
      if (role == roles.end()) throw std::invalid_argument("native state mapping has a foreign role");
      const auto& boundary = input ? role->expectedInputs : role->expectedOutputs;
      auto& contracts = destination[roleName];
      std::set<std::string> used;
      const auto append = [&](const std::string& semanticName, const std::string* expectedDtype) {
        const auto names = found->second.find(semanticName);
        if (semanticName.empty() || names == found->second.end() || names->second.empty())
          throw std::invalid_argument("native state mapping omits a semantic state");
        for (const auto& name : names->second) {
          const auto actual = std::find_if(boundary.begin(), boundary.end(), [&](const auto& value) {
            return value.name == name;
          });
          if (actual == boundary.end() ||
              (expectedDtype && actual->dtype != *expectedDtype) || !used.insert(name).second)
            throw std::invalid_argument("native state mapping differs from the source boundary");
          const auto& size = metadata.at("tensors").at(name).at("sizeBytes");
          contracts.push_back({actual->name, actual->dtype, actual->shape,
            size.is_null() ? std::nullopt : std::optional<std::uint64_t>(size.template get<std::uint64_t>())});
        }
      };
      if (expected) {
        for (const auto& semantic : *expected) append(semantic.name, &semantic.dtype);
      }
      else {
        for (const auto& semantic : found->second) append(semantic.first, nullptr);
      }
    };
    if (sourceDefined) {
      for (const auto& item : mapped) bindRole(item.first, nullptr);
    }
    else {
      for (const auto& item : declared) bindRole(item.first, &item.second);
    }
  };
  bind(candidate.roleStateInputsByRole, mapping.inputs, result.roleStateInputsByRole, true);
  bind(candidate.roleStateOutputsByRole, mapping.outputs, result.roleStateOutputsByRole, false);
  result.candidateDigest = result.computedDigest();
  result.validate(m_model.graph);
  control.requireActive();
  return result;
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
      model.canonicalInitializerObjectDigest != m_model.canonicalInitializerObjectDigest ||
      model.canonicalInitializerDigest != m_model.canonicalInitializerDigest)
    throw std::invalid_argument("native role preparation model differs from frozen source");
  candidate.validate(m_model.graph);
  const auto metadata = nativeParseJson(m_sourceGraph.graphMetadataJson);
  const auto initializers = metadata.at("initializers").get<std::vector<std::string>>();
  const auto postprocessing = nativeParseJson(candidate.postprocessingJson);
  const std::regex layerPattern("layer-([0-9]+)");
  std::map<std::string, std::vector<const NativeExecutionStepSpec*>> callsByRole;
  if (candidate.splitter.name == "native-source-partition") {
    for (const auto& step : candidate.executionPlan.steps) {
      if (step.operation == "CALL_SUBGRAPH") callsByRole[step.unitId].push_back(&step);
    }
  }
  const bool sourceProgram = std::any_of(callsByRole.begin(), callsByRole.end(),
    [] (const auto& item) {
      return item.second.size() > 1;
    });
  std::optional<qwen::QwenTpShape> qwenShape;
  std::vector<NativeSelectionRoleV3> result;
  for (const auto& name : candidate.executionPlan.roles) {
    control.requireActive();
    std::set<std::uint64_t> owned, layers;
    // A role whose nodes are declared explicitly -- two tensor-parallel ranks sharing a
    // layer interval -- owns every node in that list. `nodeRoles` remains the
    // single-valued source-slice owner and cannot express that relation, so reading its
    // inverse here would give a rank nothing and the role preparation would refuse it for
    // an empty source boundary.
    const auto explicitNodes = candidate.roleNodeIdsByRole.find(name);
    std::set<std::string> declaredNodes;
    if (explicitNodes != candidate.roleNodeIdsByRole.end())
      declaredNodes.insert(explicitNodes->second.begin(), explicitNodes->second.end());
    std::vector<std::uint64_t> planningIndices;
    for (std::size_t i = 0; i < m_model.graph.topologicalOrder.size(); ++i) {
      const auto& id = m_model.graph.topologicalOrder[i];
      if (declaredNodes.empty()) {
        if (candidate.nodeRoles.at(id) != name) continue;
      }
      else if (declaredNodes.count(id) == 0) {
        continue;
      }
      planningIndices.push_back(i);
      const auto& indices = m_mapping.at(id); owned.insert(indices.begin(), indices.end());
      std::smatch match;
      if (std::regex_match(id, match, layerPattern) && !layers.insert(std::stoull(match[1])).second)
        throw std::invalid_argument("native role repeats a semantic layer number");
    }
    if (!layers.empty() && (*layers.rbegin() == std::numeric_limits<std::uint64_t>::max() ||
        *layers.rbegin() - *layers.begin() + 1 != layers.size()))
      throw std::invalid_argument("native role layer nodes are not a contiguous range");
    std::vector<std::string> additionalOutputs;
    const auto stateOutputs = candidate.roleStateOutputsByRole.find(name);
    if (stateOutputs != candidate.roleStateOutputsByRole.end())
      for (const auto& item : stateOutputs->second) additionalOutputs.push_back(item.name);
    const auto boundary = inspectNativeSourceBoundary(m_sourceGraph.graph, metadata, owned, additionalOutputs,
                                                   [&] { control.requireActive(); });
    const auto& inputs = boundary.inputs;
    const auto& outputs = boundary.outputs;
    if (sourceProgram) {
      if (name == candidate.resultEgressRole &&
          std::any_of(model.graph.modelOutputs.begin(), model.graph.modelOutputs.end(),
            [&outputs] (const auto& port) { return outputs.count(port.name) == 0; }))
        throw std::invalid_argument("source program terminal owner does not produce the model output");
      const auto position = callsByRole.find(name);
      if (position == callsByRole.end())
        throw std::invalid_argument("source program role is missing its executable CALL");
      const auto& calls = position->second;
      if (calls.size() > 1) {
        const auto recipe = candidate.sourceTransformByRole.find(name);
        if (recipe == candidate.sourceTransformByRole.end() ||
            nativeParseJson(recipe->second).value("schema", std::string{}) !=
              NATIVE_SOURCE_ENTRY_PROGRAM_V1)
          throw std::invalid_argument("source program role is missing its entry recipe");
      }
      else {
        const auto matches = [] (const auto& ports, const auto& slots) {
          return ports.size() == slots.size() && std::all_of(slots.begin(), slots.end(),
            [&ports] (const auto& slot) {
              const auto port = ports.find(slot.slotId);
              return port != ports.end() && port->second.dtype == slot.dtype &&
                     port->second.shape == slot.shape;
            });
        };
        if (!matches(inputs, calls.front()->inputSlots) ||
            !matches(outputs, calls.front()->outputSlots))
          throw std::invalid_argument("source program outer CALL differs from its canonical frontier");
        for (const auto& slot : calls.front()->inputSlots) {
          const auto isModelInput = metadata.at("tensorProducers").count(slot.slotId) == 0;
          if ((isModelInput && slot.source != "EPOCH_INPUT") ||
              (!isModelInput &&
               (name == candidate.inputIngressRole || slot.source != "REMOTE_STEP" ||
                slot.producerSlotId != slot.slotId)))
            throw std::invalid_argument("source program outer CALL changes source tensor lineage");
        }
      }
    }
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
    // An already-expanded shard: this role is one rank of a wider split, so its placement
    // degree stays one while its recipe still carries the split geometry. Without this the
    // recipe digest is identical for every rank and both assemble the same weights.
    const auto tensorIdentity = candidate.tensorIdentityByRole.find(name);
    const auto shardDegree = tensorIdentity == candidate.tensorIdentityByRole.end()
      ? 0 : tensorIdentity->second.degree;
    const auto shardRank = tensorIdentity == candidate.tensorIdentityByRole.end()
      ? 0 : tensorIdentity->second.rank;
    if (shardDegree != 0 && (shardRank >= shardDegree || shardDegree < 2))
      throw std::invalid_argument("native role tensor identity is out of range");
    const auto& requirement = candidate.requirementsByRole.at(name);
    // This is a source-derived lower bound, not backend peak memory. Rank
    // sharding and native postprocessing do not load this full ONNX weight set.
    const auto transform = candidate.sourceTransformByRole.find(name);
    std::optional<NativeOnnxSourceTransformV1> sourceTransform;
    std::vector<NativeOnnxSourceEntryRecipeV1> sourceEntries;
    bool tensorLayoutProgram = false;
    if (transform != candidate.sourceTransformByRole.end()) {
      const auto schema = nativeParseJson(transform->second).value("schema", std::string{});
      if (schema == NATIVE_SOURCE_ENTRY_PROGRAM_V1) {
        auto program = NativeOnnxSourceEntryProgramV1::parse(transform->second);
        program.validateRank(candidate.executionPlan, name, shardRank);
        tensorLayoutProgram = !program.tensorLayouts.empty();
        sourceEntries = std::move(program.entries);
        for (const auto& entry : sourceEntries) {
          control.requireActive();
          std::set<std::uint64_t> entryNodes;
          for (const auto& node : entry.sourceNodes) {
            const auto& indices = m_mapping.at(node);
            if (indices.size() != 1 ||
                (!entryNodes.empty() && indices.front() != *entryNodes.rbegin() + 1))
              throw std::invalid_argument("source entry does not have contiguous canonical nodes");
            entryNodes.insert(indices.front());
          }
          const auto frontier = inspectNativeSourceBoundary(m_sourceGraph.graph, metadata, entryNodes, {},
                                                            [&] { control.requireActive(); });
          const auto call = std::find_if(candidate.executionPlan.steps.begin(),
            candidate.executionPlan.steps.end(), [&] (const auto& step) {
              return step.unitId == name && step.artifactEntry == entry.entryId;
            });
          if (tensorLayoutProgram) {
            if (call == candidate.executionPlan.steps.end())
              throw std::invalid_argument("tensor source entry has no CALL");
            const auto check = [&] (const auto& slots, const auto& original) {
              for (const auto& slot : slots) {
                const auto& binding = program.tensorPorts.at(slot.slotId);
                const auto port = original.find(binding.sourceName);
                if (port == original.end())
                  throw std::invalid_argument("tensor source slot is outside the original frontier");
                const auto& value = port->second;
                binding.validateSourceTensor(slot, {value.name, value.dtype, value.shape, {}});
              }
            };
            check(call->inputSlots, frontier.inputs);
            check(call->outputSlots, frontier.outputs);
            continue;
          }
          const auto matchesPorts = [] (const auto& ports, const auto& slots) {
            return ports.size() == slots.size() &&
              std::all_of(slots.begin(), slots.end(), [&] (const auto& slot) {
                const auto port = ports.find(slot.slotId);
                return port != ports.end() && port->second.dtype == slot.dtype &&
                       port->second.shape == slot.shape;
              });
          };
          if (call == candidate.executionPlan.steps.end() ||
              !matchesPorts(frontier.inputs,
                            entry.transform.originalPorts(call->inputSlots, false)) ||
              !matchesPorts(frontier.outputs,
                            entry.transform.originalPorts(call->outputSlots, true)))
            throw std::invalid_argument("source entry CALL differs from its canonical frontier");
        }
      }
      else {
        sourceTransform = NativeOnnxSourceTransformV1::parse(transform->second);
        if (sourceTransform->kind == "ffn-pair-partial")
          sourceEntries.push_back({{}, {}, *sourceTransform, {}, {}});
      }
    }
    if (!sourceEntries.empty() ||
        (sourceTransform && sourceTransform->kind == "conv-input-channel-partial")) {
      if (!requirement.weightBytes || *requirement.weightBytes == 0)
        throw std::invalid_argument("transformed native role has no resident weight bound");
      std::uint64_t residentBytes = 0;
      const auto chargeBytes = [&] (std::uint64_t bytes) {
        if (bytes > *requirement.weightBytes - residentBytes)
          throw std::invalid_argument("source entry weight budget is below its derived bytes");
        residentBytes += bytes;
      };
      const auto charge = [&] (const std::string& initializer, std::uint64_t extent,
                                std::uint64_t width) {
        const auto& size = metadata.at("tensors").at(initializer).at("sizeBytes");
        if (!size.is_number_unsigned())
          throw std::invalid_argument("source entry initializer memory is unknown");
        const auto bytes = size.get<std::uint64_t>();
        const auto shardBytes = extent == width ? bytes : nativeFfnSliceBytes(bytes, extent, width);
        chargeBytes(shardBytes);
      };
      for (const auto& entry : sourceEntries) {
        const auto& slice = entry.transform;
        const auto width = slice.end - slice.begin;
        const auto& consumers = m_sourceGraph.initializerConsumers;
        if (tensorLayoutProgram) {
          std::set<std::uint64_t> entryNodes;
          for (const auto& node : entry.sourceNodes) entryNodes.insert(m_mapping.at(node).front());
          for (const auto& initializer : initializers) {
            const auto& users = consumers.at(initializer);
            if (std::any_of(users.begin(), users.end(),
                            [&] (auto node) { return entryNodes.count(node) != 0; })) {
              const auto& bytes = metadata.at("tensors").at(initializer).at("sizeBytes");
              if (!bytes.is_number_unsigned())
                throw std::invalid_argument("tensor source entry initializer size is unknown");
              chargeBytes(bytes.get<std::uint64_t>());
            }
          }
          continue;
        }
        if (slice.isAttention()) {
          std::set<std::uint64_t> entryNodes;
          for (const auto& node : entry.sourceNodes) entryNodes.insert(m_mapping.at(node).front());
          const auto outputProjection = *entryNodes.rbegin();
          if (slice.isRmsNorm()) {
            const auto first = *entryNodes.begin();
            for (const auto& initializer : initializers) {
              const auto& users = consumers.at(initializer);
              const auto usedBy = [&users] (std::uint64_t index) {
                return std::find(users.begin(), users.end(), index) != users.end();
              };
              if (slice.kind == "attention-rms-stat") {
                if (usedBy(first + 1)) charge(initializer, slice.extent, slice.extent);
              }
              else {
                if (usedBy(first + 2)) charge(initializer, slice.extent, slice.extent);
                if (usedBy(first + 5)) charge(initializer, slice.extent, width);
              }
            }
            if (slice.kind == "attention-rms-normalize") chargeBytes(sizeof(float));
            continue;
          }
          for (const auto& initializer : initializers) {
            const auto& users = consumers.at(initializer);
            if (std::none_of(users.begin(), users.end(),
                [&] (auto node) { return entryNodes.count(node) != 0; }))
              continue;
            const bool sliced = slice.kind == "attention-qkv" ||
              (slice.kind == "attention-heads" &&
               users == std::vector<std::uint64_t>{outputProjection});
            charge(initializer, slice.extent, sliced ? width : slice.extent);
          }
          continue;
        }
        const auto projectionOrdinals = [&] (const std::string& node) {
          const auto& indices = m_mapping.at(node);
          if (indices.size() != 1)
            throw std::invalid_argument("paired FFN projection must own one source node");
          return indices;
        };
        std::set<std::string> material;
        const auto addProjection = [&] (const std::string& node, const std::string& weight) {
          const auto indices = projectionOrdinals(node);
          const auto& info = metadata.at("nodes").at(indices.front());
          if (consumers.at(weight) != indices || !material.insert(weight).second)
            throw std::invalid_argument("paired FFN initializer does not belong to its projection");
          if (info.at("opType") != "MatMulNBits") return;
          const auto scale = std::find_if(initializers.begin(), initializers.end(),
            [&] (const auto& initializer) {
              return initializer != weight && consumers.at(initializer) == indices;
            });
          if (scale == initializers.end() || !material.insert(*scale).second)
            throw std::invalid_argument("packed FFN projection has no scale initializer");
        };
        addProjection(slice.node, slice.weight);
        addProjection(slice.downNode, slice.downWeight);
        if (!slice.bias.empty()) material.insert(slice.bias);
        for (const auto& initializer : material) charge(initializer, slice.extent, width);
      }
    }
    else if (degree == 1 && !(name == candidate.resultEgressRole &&
                        candidate.mergeKind == "NATIVE_POSTPROCESS")) {
      std::uint64_t consumedBytes = 0;
      for (const auto& initializer : initializers) {
        const auto& initializerConsumers = m_sourceGraph.initializerConsumers;
        const auto consumers = initializerConsumers.find(initializer);
        if (consumers == initializerConsumers.end()) continue;
        const auto& users = consumers->second;
        const bool injectedBias = sourceTransform && sourceTransform->kind == "add-conv-bias" &&
                                  initializer == sourceTransform->bias;
        if (!injectedBias &&
            std::none_of(users.begin(), users.end(), [&](auto node) { return owned.count(node); }))
          continue;
        const auto& bytes = metadata.at("tensors").at(initializer).at("sizeBytes");
        if (!bytes.is_number_unsigned())
          throw std::invalid_argument("native role initializer memory is unknown");
        const auto count = bytes.get<std::uint64_t>();
        if (count > std::numeric_limits<std::uint64_t>::max() - consumedBytes)
          throw std::overflow_error("native role initializer memory exceeds uint64");
        consumedBytes += count;
      }
      if (!requirement.weightBytes || *requirement.weightBytes < consumedBytes)
        throw std::invalid_argument("native role weight budget is below its consumed initializers");
    }
    const auto peak = requirement.estimatedPeakGpuMemoryBytes();
    if (!peak) throw std::invalid_argument("native role requires a known runtime memory bound");
    const auto artifacts = candidate.rankArtifactDigestsByRole.find(name);
    const auto& digests = artifacts == candidate.rankArtifactDigestsByRole.end() ? candidate.artifactsByRole.at(name) : artifacts->second;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      NativeSelectionRoleV3 role;
      role.role = name; role.selectedRole = degree == 1 ? name : name + "#" + std::to_string(rank);
      role.rank = rank; role.roleKind = degree > 1 ? "HYBRID_RANK" : roleKind(name);
      if (tensorLayoutProgram) role.roleKind = "HYBRID_RANK";
      if (role.roleKind == "COMPONENT_SET" && !layers.empty() &&
          (model.descriptor.adapterId == "qwen" || model.descriptor.adapterId == "llama"))
        role.roleKind = "PIPELINE_RANGE";
      if (tensorLayoutProgram) {
        // CALL source intervals use canonical ONNX ordinals. Rank1 owns a
        // sparse set, so its ordinal envelope differs from its node count.
        role.layerBegin = *owned.begin();
        role.layerEnd = *owned.rbegin() + 1;
      }
      else if (role.roleKind != "COMPONENT_SET") {
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
        // Two ranks of one tensor-parallel stage own the same layer interval and
        // therefore the same node set, so the plain recipe JSON would derive one
        // digest for both. Bind the rank identity; a pipeline role leaves the
        // fields at zero and keeps its legacy digest bytes.
        //
        // An expanded shard reports its geometry through tensorIdentityByRole rather than
        // through placement degree, because one Provider hosts the whole unit: without the
        // separate field its degree is one and the rank would never reach the recipe.
        if (shardDegree != 0) {
          role.tensorParallelDegree = shardDegree;
          role.tensorParallelRank = shardRank;
          role.tensorShardV1 = tensorIdentity->second.tensorShardV1;
        }
        const auto transform = candidate.sourceTransformByRole.find(name);
        if (transform != candidate.sourceTransformByRole.end())
          role.sourceTransformV1 = transform->second;
        else if (degree > 1) {
          role.tensorParallelDegree = degree;
          role.tensorParallelRank = rank;
        }
        std::optional<qwen::QwenTpRankRange> uniformSlice;
        if (role.adapterId == "qwen" && role.tensorParallelDegree > 1 && role.tensorShardV1.empty()) {
          if (!qwenShape) {
            const auto totalLayers = std::count_if(m_model.graph.topologicalOrder.begin(),
              m_model.graph.topologicalOrder.end(),
              [&](const auto& node) { return std::regex_match(node, layerPattern); });
            qwenShape = qwen::deriveQwenTpShapeFromGraphMetadata(m_sourceGraph.graphMetadataJson,
              static_cast<std::uint64_t>(totalLayers));
          }
          const auto ranges = qwen::resolveQwenTpRankRanges(*qwenShape,
            std::vector<std::uint64_t>(role.tensorParallelDegree, 1));
          uniformSlice = ranges.at(role.tensorParallelRank);
        }
        role.artifactCoordinate = assembledArtifactCoordinate(candidate, role, planningIndices,
          layers, owned.size() == m_sourceGraph.nodeNames.size(), uniformSlice);
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
