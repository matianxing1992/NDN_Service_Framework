#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-split.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-state-role-mapping.hpp"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <limits>
#include <locale>
#include <set>

namespace ndnsf::di {
namespace {

std::map<std::string, NativeRoleResourceRequirement>
resourceProfilesFromCatalog(const NativeJson& split, const std::vector<std::string>& roles,
                            const std::vector<std::string>& allowedBackends)
{
  const auto profiles = split.find("resources_by_role");
  if (roles.empty() || std::set<std::string>(roles.begin(), roles.end()).size() != roles.size() ||
      profiles == split.end() || !profiles->is_object() || profiles->size() != roles.size())
    throw std::invalid_argument("native transformer requires explicit per-role resource profiles");
  std::map<std::string, NativeRoleResourceRequirement> result;
  for (const auto& role : roles) {
    if (role.empty()) throw std::invalid_argument("native role resource identity is empty");
    const auto entry = profiles->find(role);
    if (entry == profiles->end() || !entry->is_object() || entry->size() != 7 ||
        !entry->contains("backends") || !entry->at("backends").is_array() ||
        !entry->contains("safety_margin") || !entry->at("safety_margin").is_number())
      throw std::invalid_argument("native role resource profile is incomplete");
    NativeRoleResourceRequirement profile;
    for (const auto& backend : entry->at("backends")) {
      if (!backend.is_string())
        throw std::invalid_argument("native role resource backend must be a string");
      const auto name = backend.get<std::string>();
      if (name.empty() || std::find(allowedBackends.begin(), allowedBackends.end(), name) ==
                            allowedBackends.end() ||
          std::find(profile.backends.begin(), profile.backends.end(), name) != profile.backends.end())
        throw std::invalid_argument("native role resource backend is unsupported or repeated");
      profile.backends.push_back(name);
    }
    const auto bytes = [&](const char* key) {
      const auto value = entry->find(key);
      if (value == entry->end() || !value->is_number_unsigned())
        throw std::invalid_argument("native role resource budget is unknown or not uint64");
      return value->get<std::uint64_t>();
    };
    profile.weightBytes = bytes("weight_bytes");
    profile.workspaceBytes = bytes("workspace_bytes");
    profile.kvBytes = bytes("kv_bytes");
    profile.activationBytes = bytes("activation_bytes");
    profile.transientBytes = bytes("transient_bytes");
    profile.safetyMargin = entry->at("safety_margin").get<double>();
    if (!profile.estimatedPeakGpuMemoryBytes())
      throw std::invalid_argument("native role resource peak is unknown");
    result.emplace(role, std::move(profile));
  }
  return result;
}


} // namespace

NativeStateTensorMapping
detail::remapNativeStateRoles(const NativeStateTensorMapping& source,
                             const std::map<std::string, std::string>& sourceRoleByUnit)
{
  if (sourceRoleByUnit.empty())
    throw std::invalid_argument("native state role expansion has no units");
  NativeStateTensorMapping result;
  for (const auto& [unit, sourceRole] : sourceRoleByUnit) {
    const auto inputs = source.inputs.find(sourceRole);
    const auto outputs = source.outputs.find(sourceRole);
    if (unit.empty() || sourceRole.empty() || inputs == source.inputs.end() ||
        outputs == source.outputs.end() ||
        inputs->second.empty() != outputs->second.empty())
      throw std::invalid_argument("native state role expansion omits a source state contract");
    result.inputs.emplace(unit, inputs->second);
    result.outputs.emplace(unit, outputs->second);
  }
  return result;
}

NativePreparedPlanningTemplate::NativePreparedPlanningTemplate(
  std::shared_ptr<const NativeInspectedModel> model,
  std::shared_ptr<const CooperativeModelSplitStrategy> splitter,
  std::vector<NativePreparedPlanningCandidate> candidates)
  : m_model(std::move(model)), m_splitter(std::move(splitter)),
    m_candidates(std::move(candidates)), m_retainedBytes(sizeof(*this))
{
  if (!m_model || !m_splitter || m_candidates.empty())
    throw std::invalid_argument("prepared planning template is incomplete");
  const auto charge = [this](std::size_t bytes) {
    // Include both the compatibility copy and owned static facts, plus
    // conservative parser/container overhead, in the existing package budget.
    if (bytes > (std::numeric_limits<std::size_t>::max() - m_retainedBytes) / 8)
      throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
    m_retainedBytes += bytes * 8;
  };
  m_execution.reserve(m_candidates.size());
  m_roleJson.reserve(m_candidates.size());
  m_snapshotFixedCanonical.reserve(m_candidates.size());
  for (const auto& prepared : m_candidates) {
    NativeRequestPreparation::validateRoles(*m_model, prepared.candidate, prepared.roles);
    charge(prepared.candidate.canonicalJson().size());
    auto execution = prepared.candidate.executionPlan;
    execution.roles.clear();
    std::set<std::string> selected;
    std::map<std::string, std::vector<std::string>> selectedByStage;
    NativeJson roleValues = NativeJson::array();
    for (const auto& role : prepared.roles) {
      if (role.selectedRole.empty() || !selected.insert(role.selectedRole).second)
        throw std::invalid_argument("prepared planning template role cover is invalid");
      auto roleValue = nativeAssemblyJson(role);
      const auto roleBytes = nativeCanonicalJson(roleValue).size();
      charge(roleBytes);
      charge(roleBytes); // retained JSON fields in addition to typed roles
      roleValues.push_back(std::move(roleValue));
      execution.roles.push_back(role.selectedRole);
      selectedByStage[role.role].push_back(role.selectedRole);
    }
    m_roleJson.push_back(std::move(roleValues));
    std::map<std::string, std::string> fixedCanonical;
    if (std::fegetround() == FE_TONEAREST && std::locale() == std::locale::classic()) {
      const NativeJson fixedFields{
        {"candidate_digest", prepared.candidate.candidateDigest},
        {"graph_digest", prepared.candidate.graphDigest},
        {"node_roles", prepared.candidate.nodeRoles},
        {"roles", m_roleJson.back()},
        {"stage_order", prepared.candidate.executionPlan.roles}};
      for (auto field = fixedFields.begin(); field != fixedFields.end(); ++field) {
        ++detail::fixedAssignmentWork().snapshotFixedEncodes;
        auto bytes = nativeCanonicalJson(field.value());
        charge(bytes.size());
        fixedCanonical.emplace(field.key(), std::move(bytes));
      }
    }
    m_snapshotFixedCanonical.push_back(std::move(fixedCanonical));
    const auto expand = [&selectedByStage](const std::vector<std::string>& stages) {
      std::vector<std::string> result;
      for (const auto& stage : stages) {
        const auto& roles = selectedByStage.at(stage);
        result.insert(result.end(), roles.begin(), roles.end());
      }
      return result;
    };
    for (auto& dependency : execution.dependencies) {
      dependency.producers = expand(dependency.producers);
      dependency.consumers = expand(dependency.consumers);
    }
    // A stage's steps run on every rank of that stage, so one stage step becomes one
    // step per rank. Rewriting only the dependencies would leave each step naming the
    // stage, which is no longer one of the plan's roles -- and the worker looks a step's
    // unit up in that list. Slot references between steps follow the same rename, per
    // rank, so an intra-unit edge keeps pointing at its own rank's producer rather than
    // at whichever rank was rewritten last.
    if (!execution.steps.empty()) {
      std::vector<NativeExecutionStepSpec> expandedSteps;
      expandedSteps.reserve(execution.steps.size() * selectedByStage.size());
      for (const auto& step : execution.steps) {
        const auto stageStep = selectedByStage.find(step.unitId);
        if (stageStep == selectedByStage.end())
          throw std::invalid_argument(
            "prepared step names a unit with no selected roles: " + step.unitId);
        const auto renamed = [&stageStep] (const std::string& stepId) {
          // Step ids are "<stage>/<kind>/<index>"; a rank's copy is
          // "<rank>/<kind>/<index>". Only ids that begin with this stage's prefix
          // belong to it, so an edge from another stage is left alone.
          const std::string prefix = stageStep->first + "/";
          if (stepId.rfind(prefix, 0) != 0) return stepId;
          return stepId.substr(prefix.size());
        };
        for (const auto& selected : stageStep->second) {
          auto copy = step;
          copy.unitId = selected;
          copy.stepId = selected + "/" + renamed(step.stepId);
          for (auto& slot : copy.inputSlots) {
            if (slot.producerStepId.empty()) continue;
            // The producer is in the same stage: its copy on this rank. A producer in
            // another stage keeps its own expansion, which that stage's loop renames.
            if (slot.producerStepId.rfind(stageStep->first + "/", 0) == 0) {
              slot.producerStepId = selected + "/" + renamed(slot.producerStepId);
            }
          }
          expandedSteps.push_back(std::move(copy));
        }
      }
      execution.steps = std::move(expandedSteps);
    }
    m_execution.push_back(std::move(execution));
  }
}

const NativeJson* NativePreparedPlanningCache::preparedRoleJson(std::size_t index) const
{
  if (!m_template || index >= candidates.size() || index >= m_template->candidates().size())
    return nullptr;
  const auto& current = candidates[index].roles;
  const auto& prepared = m_template->candidates()[index].roles;
  if (current.size() != prepared.size() ||
      !std::equal(current.begin(), current.end(), prepared.begin(),
        [](const auto& left, const auto& right) {
          return nativeSelectionRoleContractsEqualV3(left, right);
        }))
    return nullptr;
  return &m_template->roleJson(index);
}

std::string nativePlacementObservationCanonicalJson(const NativeJson& snapshot,
  const NativePreparedPlanningTemplate* prepared)
{
  if (!snapshot.is_object()) return nativeCanonicalJson(snapshot);
  const bool canReuse = prepared && std::fegetround() == FE_TONEAREST &&
    std::locale() == std::locale::classic();
  const auto fixedField = [](const std::string& key) {
    return key == "candidate_digest" || key == "graph_digest" ||
      key == "node_roles" || key == "roles" || key == "stage_order";
  };
  // Match the actual request-owned JSON after every callback. The owner is
  // an optimization hint; it never certifies mutable snapshot contents.
  const auto matches = [&](const NativeJson& graph, std::size_t index) {
    if (!canReuse || index >= prepared->m_candidates.size() ||
        prepared->m_snapshotFixedCanonical[index].empty()) return false;
    const auto& expected = prepared->m_candidates[index];
    const auto stringMatches = [&](const char* field, const std::string& text) {
      const auto found = graph.find(field);
      return found != graph.end() && found->is_string() &&
        found->get_ref<const NativeJson::string_t&>() == text;
    };
    if (!stringMatches("candidate_digest", expected.candidate.candidateDigest) ||
        !stringMatches("graph_digest", expected.candidate.graphDigest)) return false;
    const auto nodes = graph.find("node_roles");
    if (nodes == graph.end() || !nodes->is_object() ||
        nodes->size() != expected.candidate.nodeRoles.size()) return false;
    for (const auto& entry : expected.candidate.nodeRoles) {
      const auto found = nodes->find(entry.first);
      if (found == nodes->end() || !found->is_string() ||
          found->get_ref<const NativeJson::string_t&>() != entry.second) return false;
    }
    const auto stages = graph.find("stage_order");
    const auto& expectedStages = expected.candidate.executionPlan.roles;
    if (stages == graph.end() || !stages->is_array() ||
        stages->size() != expectedStages.size()) return false;
    for (std::size_t i = 0; i < expectedStages.size(); ++i) {
      if (!(*stages)[i].is_string() ||
          (*stages)[i].get_ref<const NativeJson::string_t&>() != expectedStages[i]) return false;
    }
    const auto roles = graph.find("roles");
    if (roles == graph.end() || !roles->is_array() ||
        roles->size() != expected.roles.size()) return false;
    for (std::size_t i = 0; i < expected.roles.size(); ++i) {
      if (!nativeAssemblyJsonMatches(expected.roles[i], (*roles)[i])) return false;
    }
    return true;
  };
  std::string output;
  const auto appendKey = [&](const std::string& key) {
    canonical_json_detail::append(output, NativeJson(key));
    output += ':';
  };
  output += '{';
  bool firstRoot = true;
  for (auto root = snapshot.begin(); root != snapshot.end(); ++root) {
    if (!firstRoot) output += ',';
    firstRoot = false;
    appendKey(root.key());
    if (root.key() != "prepared_role_graph" || !root.value().is_array()) {
      canonical_json_detail::append(output, root.value());
      continue;
    }
    output += '[';
    for (std::size_t index = 0; index < root.value().size(); ++index) {
      if (index) output += ',';
      const auto& graph = root.value()[index];
      if (!graph.is_object()) {
        canonical_json_detail::append(output, graph);
        continue;
      }
      const auto* cached = matches(graph, index)
        ? &prepared->m_snapshotFixedCanonical[index] : nullptr;
      output += '{';
      bool firstGraph = true;
      for (auto field = graph.begin(); field != graph.end(); ++field) {
        if (!firstGraph) output += ',';
        firstGraph = false;
        appendKey(field.key());
        if (cached && fixedField(field.key())) output += cached->at(field.key());
        else {
          if (fixedField(field.key())) ++detail::fixedAssignmentWork().snapshotFixedEncodes;
          canonical_json_detail::append(output, field.value());
        }
      }
      output += '}';
    }
    output += ']';
  }
  output += '}';
  return output;
}

NativeRequestCatalog NativeRequestCatalog::load(const std::string& configurationJson,
  NativeCanonicalSource source, const NativeAssemblyControl& control)
{
  control.requireActive();
  if (configurationJson.size() > 4 * 1024 * 1024)
    throw std::invalid_argument("request catalog configuration exceeds limit");
  const auto root = nativeParseJson(configurationJson);
  if (root.at("schema") != "ndnsf-di-native-request-catalog-v1")
    throw std::invalid_argument("unsupported native request catalog schema");
  NativeCanonicalCatalogEntry entry;
  auto& model = entry.model;
  model.descriptor = NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(root.at("model")));
  const auto& split = root.at("splitter");
  const auto splitterKind = split.at("kind").get<std::string>();
  const bool sourceDefined = splitterKind == "QWEN" || splitterKind == "LLAMA";
  const auto resourceProfiles = sourceDefined
    ? resourceProfilesFromCatalog(split, split.at("roles").get<std::vector<std::string>>(),
                                  model.descriptor.adapter.backends)
    : std::map<std::string, NativeRoleResourceRequirement>{};
  const auto& sourceConfig = root.at("source");
  std::shared_ptr<const AutomaticOnnxContract> automaticContract;
  std::shared_ptr<const AutomaticOnnxModel> automaticModel;
  const bool referenceOnly = source.modelBytes.empty() && !source.preparedMetadataJson.empty();
  model.canonicalSourceName = sourceConfig.at("data_name").get<std::string>();
  model.canonicalSourceDigest = sourceConfig.at("digest").get<std::string>();
  model.modelManifestDigest = sourceConfig.at("model_manifest_digest").get<std::string>();
  model.canonicalGraphDigest = sourceConfig.at("canonical_graph_digest").get<std::string>();
  if (referenceOnly) {
    if (source.preparedMetadataJson.size() > NativeCanonicalMaterialPublicationMaxBytes)
      throw std::invalid_argument("native prepared metadata exceeds publication bound");
    const auto metadata = nativeParseJson(source.preparedMetadataJson);
    auto decodedMetadata = decodeNativePreparedOnnxMetadata(metadata, control);
    const auto& facts = metadata.at("source");
    if (facts.at("name").get<std::string>() != model.canonicalSourceName ||
        facts.at("digest").get<std::string>() != model.canonicalSourceDigest ||
        facts.at("manifest_digest").get<std::string>() != model.modelManifestDigest ||
        facts.at("graph_digest").get<std::string>() != model.canonicalGraphDigest ||
        metadata.at("descriptor") != nativeParseJson(model.descriptor.canonicalJson()))
      throw std::invalid_argument("native prepared metadata differs from catalog identity");
    model.canonicalSourceBytes = facts.at("bytes").get<std::uint64_t>();
    model.canonicalInitializerBytes = facts.value("initializer_bytes", std::uint64_t{0});
    model.canonicalInitializerObjectDigest = facts.value("initializer_object_digest", std::string{});
    model.canonicalInitializerDigest = facts.value("initializer_digest", std::string{});
    entry.sourceGraphInspection = std::move(decodedMetadata.graph);
    automaticContract = std::move(decodedMetadata.automaticContract);
  }
  else {
    model.canonicalSourceBytes = source.modelBytes.size();
    if (nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) != model.canonicalSourceDigest)
      throw std::invalid_argument("request source bytes differ from pinned digest");
    if (source.initializerBytes || source.initializerRangeSource) {
      model.canonicalInitializerBytes = nativeCanonicalInitializerByteSize(source);
      model.canonicalInitializerObjectDigest = sourceConfig.at("initializer_digest").get<std::string>();
      if (nativeCanonicalInitializerDigest(source, control) !=
          model.canonicalInitializerObjectDigest)
        throw std::invalid_argument("request initializer bytes differ from pinned digest");
    }
    else if (sourceConfig.contains("initializer_digest"))
      throw std::invalid_argument("pinned initializer object is missing");
    if (model.descriptor.modelFormat == "onnx") {
      // Internal initializers live in the model bytes; external ones are pinned
      // above. Both need the normalized initializer identity for prepared
      // metadata and the assembly recipe.
      model.canonicalInitializerDigest = canonicalOnnxSourceIdentity(
        source, control).initializerDigest;
    }
  }
  const auto& recipe = root.at("recipe");
  entry.recipe = {recipe.at("artifact_profile_digest"), recipe.at("assembler_descriptor_digest"),
    recipe.at("backend_abi"), recipe.at("precision"), recipe.at("quantization"), recipe.at("layout"),
    recipe.at("padding"), recipe.at("protection_epoch"), recipe.at("max_source_bytes"),
    recipe.at("max_assembled_bytes"), recipe.at("max_nodes")};
  if (entry.recipe.quantization != model.descriptor.quantizationSubtype)
    throw std::invalid_argument("request recipe quantization differs from model identity");
  const auto& publication = root.at("publication");
  entry.publication.artifactRoot = publication.at("artifact_root").get<std::string>();
  entry.publication.packageManifestDigest = publication.value("package_manifest_digest", model.modelManifestDigest);
  if (entry.publication.packageManifestDigest != model.modelManifestDigest)
    throw std::invalid_argument("publication package differs from pinned manifest");
  entry.publication.layerManifestDigests = publication.value("layer_manifest_digests", std::vector<std::string>{});
  entry.publication.maxPublicationBytes = publication.value(
    "max_publication_bytes", std::uint64_t{0});
  // Bind model/source/recipe/split changes while keeping advisory transfer
  // geometry from forcing an identical model publication and assembly again.
  entry.publication.publicationIdentityDigest = nativeCanonicalPublicationIdentity(root);
  entry.nodes = root.value("node_mapping", NativeCanonicalRolePreparer::NodeMap{});
  entry.maxPayloadBytes = root.at("max_payload_bytes").get<std::size_t>();
  const auto format = root.at("input_format").get<std::string>();
  if (format == "JSON") entry.format = NativeCatalogModelAdapter::Format::JsonBytes;
  else if (format == "OPAQUE") entry.format = NativeCatalogModelAdapter::Format::OpaqueBytes;
  else throw std::invalid_argument("unsupported request input format");
  if (root.contains("conversation_input")) {
    const auto& conversationInput = root.at("conversation_input");
    if (!conversationInput.is_object())
      throw std::invalid_argument("unsupported native conversation input encoder");
    const auto kind = conversationInput.value("kind", std::string{});
    if (kind == "OPAQUE_BYTE_TOKEN_IDS") {
      // This operator-pinned fixture contract derives one canonical token from
      // each encoded byte.  Callers cannot supply or replace this encoder; it
      // is captured by the immutable adapter registry during preparation.
      entry.conversationTokenEncoder = [] (const std::vector<std::uint8_t>& bytes) {
        std::vector<std::int64_t> tokens;
        tokens.reserve(bytes.size());
        for (const auto byte : bytes)
          tokens.push_back(static_cast<std::int64_t>(byte));
        return tokens;
      };
    }
    else if (kind == "TENSOR_BUNDLE_TOKEN_IDS") {
      const auto tensorName = conversationInput.value("tensor_name", "input_ids");
      if (tensorName.empty())
        throw std::invalid_argument("tensor-bundle conversation input tensor name is empty");
      // The native request payload is an authenticated tensor bundle.  A
      // tensor-bundle encoder must derive the same token suffix as the
      // execution coordinator, rather than hashing transport framing bytes.
      entry.conversationTokenEncoder = [tensorName] (const std::vector<std::uint8_t>& bytes) {
        if (!isEncodedTensorBundle(bytes))
          throw std::invalid_argument("conversation input is not an encoded tensor bundle");
        const auto tensors = decodeTensorBundle(bytes);
        const auto found = std::find_if(tensors.begin(), tensors.end(),
          [&tensorName] (const auto& tensor) { return tensor.name == tensorName; });
        if (found == tensors.end() || found->elementType != TensorElementType::Int64 ||
            found->shape.empty() || found->payload.empty() ||
            found->payload.size() % sizeof(std::int64_t) != 0)
          throw std::invalid_argument("conversation input token tensor is invalid");
        std::vector<std::int64_t> tokens(found->payload.size() / sizeof(std::int64_t));
        std::memcpy(tokens.data(), found->payload.data(), found->payload.size());
        if (std::any_of(tokens.begin(), tokens.end(), [] (const auto token) { return token < 0; }))
          throw std::invalid_argument("conversation input token ID is negative");
        return tokens;
      };
    }
    else {
      throw std::invalid_argument("unsupported native conversation input encoder");
    }
  }
  NativeRequestCatalog result;
  result.automaticContract = std::move(automaticContract);
  // Candidates built from a projected plan skip the splitter's per-stage enumeration, so
  // they are collected separately and prepared through the same state binding and role
  // preparation as an enumerated candidate. A projected candidate with empty prepared
  // roles would fail role binding later, which is why it is not pushed alone.
  std::optional<NativeSplitCandidate> projectedCandidate;
  std::map<std::string, std::string> sourceRoleByUnit;
  std::shared_ptr<qwen::NativeQwenLayerSplit> transformerStrategy;
  if (splitterKind == "QWEN" || splitterKind == "LLAMA") {
    const auto modelFamily = splitterKind == "LLAMA" ? "llama" : "qwen";
    // A sharded stage needs one artifact per rank. The catalog accepts the
    // per-rank map when present and leaves it empty otherwise, in which case the
    // splitter keeps the legacy single-artifact path for degree-one stages only.
    std::map<std::string, std::vector<std::string>> rankArtifacts;
    if (split.contains("rank_artifact_digests_by_role")) {
      for (const auto& item : split.at("rank_artifact_digests_by_role").items()) {
        rankArtifacts.emplace(item.key(),
          item.value().get<std::vector<std::string>>());
      }
    }
    transformerStrategy = std::make_shared<qwen::NativeQwenLayerSplit>(
      split.at("layer_ranges").get<std::vector<qwen::NativeQwenLayerSplit::LayerRange>>(),
      split.at("artifact_digests_by_role").get<std::map<std::string, std::string>>(),
      split.at("weight_bytes_by_role").get<std::map<std::string, std::uint64_t>>(),
      split.at("roles").get<std::vector<std::string>>(), split.at("tensor_degrees").get<std::vector<std::uint64_t>>(),
      split.value("input_ingress_role", std::string{}), split.value("result_egress_role", std::string{}),
      modelFamily, std::move(rankArtifacts));
    model.graph = transformerStrategy->inspectGraph(
      model.descriptor, model.descriptor.sourceRevision, entry.recipe.maxNodes);
    result.cooperativeSplitter = transformerStrategy;
    result.splitter = transformerStrategy;
  }
  else if (split.at("kind") == "YOLO") {
    if (referenceOnly)
      throw std::invalid_argument("DI_NATIVE_PREPARATION_REFERENCE_RESTORE_UNSUPPORTED");
    model.graph = inspectNativeOnnxPlanningGraph(source, model.descriptor, control).graph;
    std::vector<yolo::NativeYoloCatalogComponent> components;
    for (const auto& item : split.at("components")) {
      yolo::NativeYoloComponentSpec component;
      component.candidateId = item.at("candidate_id").get<std::string>();
      component.priority = item.at("priority").get<int>();
      component.roles = item.at("roles").get<std::vector<std::string>>();
      component.nodeNamesByRole = item.at("node_names_by_role").get<std::map<std::string, std::vector<std::string>>>();
      component.inputIngressRole = item.at("input_ingress_role").get<std::string>();
      component.resultEgressRole = item.at("result_egress_role").get<std::string>();
      component.mergeKind = item.at("merge_kind").get<std::string>();
      component.candidateDigest = item.at("candidate_digest").get<std::string>();
      components.push_back({std::move(component), nativeCanonicalJson(item.at("semantic_partition"))});
    }
    auto strategy = std::make_shared<yolo::NativeYoloComponentSplit>(
      yolo::NativeYoloComponentSplit::fromOnnxCatalog(model.descriptor, source, control,
        std::move(components), nativeCanonicalJson(split.value("postprocessing", NativeJson::object()))));
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  else if (splitterKind == "AUTOMATIC") {
    if (referenceOnly || source.externalFileRanges.size() > 1)
      throw std::invalid_argument("DI_NATIVE_PREPARATION_AUTOMATIC_SOURCE_UNSUPPORTED");
    const auto initializerFile = sourceConfig.value("initializer_file", std::string{});
    OpenedAutomaticOnnxSource opened;
    opened.source = source;
    opened.modelDigest = nativePlanningDigest(opened.source.modelBytes.data(),
                                              opened.source.modelBytes.size());
    if (source.initializerBytes || source.initializerRangeSource) {
      // One external object is supported; its location comes from the pinned
      // catalog entry and its identity was already checked by the load above.
      if (initializerFile.empty() || entry.model.canonicalInitializerObjectDigest.empty() ||
          entry.model.canonicalInitializerBytes == 0)
        throw std::invalid_argument("automatic external initializer identity is incomplete");
      // The catalog's initializer_file is the on-disk path; the source
      // inventory keeps the model's external location, which is the file name
      // for maintained exports (a single external object is accepted here).
      opened.externalFiles.push_back(AutomaticOnnxSourceFile{
        std::filesystem::path(initializerFile).filename().string(),
        entry.model.canonicalInitializerBytes,
        entry.model.canonicalInitializerObjectDigest});
    }
    else if (!initializerFile.empty())
      throw std::invalid_argument("pinned automatic initializer object is missing");
    opened.sourceDigest = automaticOnnxSourceDigest(opened.modelDigest,
                                                    opened.externalFiles, control);
    auto automatic = inspectAutomaticOnnxModel(std::move(opened), control);
    automaticModel = std::make_shared<const AutomaticOnnxModel>(std::move(automatic));
    auto strategy = std::make_shared<AutomaticOnnxSplitStrategy>(automaticModel);
    model.graph = strategy->inspectGraph(model.descriptor, model.descriptor.sourceRevision,
                                         entry.recipe.maxNodes);
    // result.automaticContract was already assigned (empty) before this branch;
    // set it here as well so the automatic catalog carries its contract.
    result.automaticContract = std::make_shared<const AutomaticOnnxContract>(
      automaticModel->contract);
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  else throw std::invalid_argument("unsupported native request splitter");
  result.stateMapping.inputs = root.value("state_inputs", NativeStateTensorMapping::Roles{});
  result.stateMapping.outputs = root.value("state_outputs", NativeStateTensorMapping::Roles{});
  result.model = model;
  if (!referenceOnly) {
    // The automatic splitter's planning graph is the automatic inspection's
    // source graph. Reusing it keeps the role preparer's source/planning
    // identity check meaningful without an explicit ONNX node mapping.
    if (splitterKind == "AUTOMATIC" && automaticModel)
      entry.sourceGraphInspection = automaticModel->inspection;
    else
      entry.sourceGraphInspection = inspectNativeOnnxSourceGraph(source, model.descriptor, control);
    // Only adapters with a reference-only restore contract publish this
    // metadata. Unsupported adapters remain on the source-backed path and
    // therefore cannot create a receipt that a later prepare cannot restore.
    if (splitterKind == "QWEN" || splitterKind == "LLAMA")
      source.preparedMetadataJson = encodeNativePreparedOnnxMetadata(
        model, *entry.sourceGraphInspection, nullptr, control);
    else if (splitterKind == "AUTOMATIC") {
      if (!automaticModel)
        throw std::logic_error("automatic source model was released before metadata");
      // The automatic inspection owns the source graph facts that back the
      // automatic contract. The legacy source-graph pass derives a different
      // graph digest for the same bytes, so reuse the automatic inspection
      // rather than comparing two representations of one source.
      source.preparedMetadataJson = encodeNativePreparedOnnxMetadata(
        model, automaticModel->inspection, automaticModel.get(), control);
    }
  }
  // The projected-plan branch runs after the source is moved into the catalog entry, so
  // the bytes the shape derivation needs are captured here. `deriveQwenTpShape` parses
  // the serialized graph, which only exists before the move.
  // A sharded Qwen plan needs its source geometry whether it has one stage
  // (pure TP) or several (hybrid); the role count alone does not decide it.
  const bool sourceBoundHybrid = sourceDefined && splitterKind == "QWEN" &&
    std::any_of(split.at("tensor_degrees").begin(), split.at("tensor_degrees").end(),
      [] (const auto& degree) { return degree.template get<std::uint64_t>() > 1; });
  std::map<std::string, std::vector<std::uint64_t>> rankWeights;
  if (const auto weights = split.find("tensor_rank_weights_by_role"); weights != split.end()) {
    if (!sourceBoundHybrid || !weights->is_object() || weights->empty())
      throw std::invalid_argument("tensor rank weights require a source-bound Qwen hybrid");
    for (const auto& stage : weights->items()) {
      if (!stage.value().is_array() || stage.value().empty())
        throw std::invalid_argument("tensor rank weights require a nonempty array per stage");
      auto& values = rankWeights[stage.key()];
      for (const auto& weight : stage.value()) {
        if (!weight.is_number_integer() || weight < 1 ||
            weight > std::numeric_limits<std::int32_t>::max())
          throw std::invalid_argument("tensor rank weights must be bounded positive integers");
        values.push_back(weight.get<std::uint64_t>());
      }
    }
  }
  std::optional<qwen::QwenTpShape> hybridColdShape;
  if (sourceBoundHybrid && !source.modelBytes.empty()) {
    // Preserve full byte-based operator/quantization admission before releasing the bytes.
    // Only the small geometry survives the move; port descriptions use inspected facts.
    hybridColdShape = qwen::deriveQwenTpShape(source.modelBytes,
      split.at("layer_ranges").back().at(1).get<std::uint64_t>());
  }
  // Hybrid cold/restore planning consumes the same inspected facts, without retaining
  // another model-sized byte copy after the source moves into the preparation catalog.
  const std::vector<std::uint8_t> planningGraphBytes =
    sourceBoundHybrid ? std::vector<std::uint8_t>{} : source.modelBytes;
  // The same holds for the recorded graph facts: the sharded branch runs after the entry
  // has moved into the preparation catalog, so a reference-only restore could not reach
  // them there.
  const std::string planningGraphMetadata =
    entry.sourceGraphInspection ? entry.sourceGraphInspection->graphMetadataJson
                                : std::string{};
  const auto planningGraphInspection = sourceBoundHybrid
    ? entry.sourceGraphInspection : std::optional<NativeOnnxGraphInspection>{};
  entry.source = std::move(source);
  std::vector<NativeCanonicalCatalogEntry> entries;
  entries.push_back(std::move(entry));
  result.preparation = std::make_shared<const NativeCanonicalPreparationCatalog>(std::move(entries), control);

  // Freeze the graph-dependent planning facts at prepare time.  The request
  // planner still rechecks the candidate/roles and performs all request-bound
  // admission, placement, grant, and projection work, but it no longer walks
  // the ONNX semantic graph after ACK closure.
  NativeRequestControl planningControl{
    "/NDNSF/DI/PREPARE/" + result.model.canonicalSourceDigest.substr(7), 1,
    control.deadline, [&control] { control.requireActive(); return false; }};
  NativeCandidateBudget preparationBudget{1024, 60'000, 16};
  ExtensionControl extensionControl{
    control.deadline, [&control] { control.requireActive(); return false; }};
  std::vector<NativePreparedPlanningCandidate> preparedCandidates;
  // A sharded Qwen catalog produces a projected plan rather than a pipeline candidate.
  // The stage list still describes the topology, but each stage's ranks are separate
  // units with their own plan, so the candidate is built from the projection instead of
  // from the splitter's per-stage enumeration.
  if (sourceDefined && splitterKind == "QWEN" &&
      std::any_of(split.at("tensor_degrees").begin(), split.at("tensor_degrees").end(),
                  [] (const auto& degree) {
                    return degree.template get<std::uint64_t>() > 1;
                  })) {
    // A single stage uses the existing tensor-parallel projection.
    const auto& degrees = split.at("tensor_degrees");
    const auto& layerRanges = split.at("layer_ranges");
    const auto& stageRoles = split.at("roles");
    const bool hybridLayout =
      stageRoles.size() > 1 || layerRanges.size() > 1 || degrees.size() > 1;
    // Weighted plans (single-stage pure TP included) lower through the
    // source-bound path so the declared rank weights decide the shard ranges.
    if (hybridLayout || !rankWeights.empty()) {
      const auto totalLayers = layerRanges.back().at(1).get<std::uint64_t>();
      const auto hybridShape = hybridColdShape ? *hybridColdShape
        : qwen::deriveQwenTpShapeFromGraphMetadata(planningGraphMetadata, totalLayers);
      if (!planningGraphInspection)
        throw std::invalid_argument("hybrid Qwen catalog requires inspected source port facts");
      const qwen::QwenTpSourceRegionPort region = [&] (const auto& nodes, const auto& outputs) {
        return result.preparation->sourceRegion(result.model, nodes, outputs, planningControl);
      };
      auto [candidate, unitStages] = transformerStrategy->lowerSourceHybrid(
        result.model.descriptor, result.model.graph, hybridShape, resourceProfiles,
        *planningGraphInspection, region, result.stateMapping, extensionControl, rankWeights);
      projectedCandidate = std::move(candidate);
      sourceRoleByUnit = std::move(unitStages);
    }
    else {
    const auto layers = layerRanges.at(0).at(1).get<std::uint64_t>();
    const auto degree = degrees.at(0).get<std::uint64_t>();
    const auto stageRole = stageRoles.at(0).get<std::string>();
    // A complete preparation-cache hit restores bounded graph facts instead of the
    // serialized graph, so the byte-based derivation has nothing to parse there. The
    // recorded metadata carries the same facts -- attention attributes, projection widths
    // and the embedding table -- and deriving from those is what the tensor-parallel
    // contract asks for: read recorded facts instead of guessing dimensions.
    const auto shape = planningGraphBytes.empty()
      ? qwen::deriveQwenTpShapeFromGraphMetadata(planningGraphMetadata, layers)
      : qwen::deriveQwenTpShape(planningGraphBytes, layers);

    std::map<std::string, std::vector<std::string>> rankArtifacts;
    if (split.contains("rank_artifact_digests_by_role")) {
      for (const auto& item : split.at("rank_artifact_digests_by_role").items()) {
        rankArtifacts.emplace(item.key(),
          item.value().get<std::vector<std::string>>());
      }
    }
    const auto ranked = rankArtifacts.find(stageRole);
    if (ranked == rankArtifacts.end() || ranked->second.size() != degree) {
      throw std::invalid_argument(
        "sharded Qwen catalog needs one artifact digest per rank");
    }
    const auto profile = resourceProfiles.find(stageRole);
    if (profile == resourceProfiles.end()) {
      throw std::invalid_argument(
        "sharded Qwen catalog has no resource profile for its stage");
    }

    std::vector<qwen::QwenTpUnit> units;
    std::map<std::string, std::string> artifactByUnit;
    std::map<std::string, NativeRoleResourceRequirement> requirementByUnit;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      qwen::QwenTpUnit unit;
      // The role name decides the prepared role's kind: a name with no stage or rank
      // component is treated as a component set, and a component set is not given a layer
      // range. A sharded unit does cover a layer range, so the name says so.
      unit.roleId = stageRole + "/Tensor/Rank/" + std::to_string(rank);
      unit.rank = rank;
      // Every unit covers every layer: a shard is a weight slice, not a layer slice.
      unit.layerBegin = 0;
      unit.layerEnd = layers;
      artifactByUnit[unit.roleId] = ranked->second.at(rank);
      requirementByUnit[unit.roleId] = profile->second;
      sourceRoleByUnit.emplace(unit.roleId, stageRole);
      units.push_back(std::move(unit));
    }
    // The head lives on exactly one unit, and which one is configuration, not convention: the
    // catalog's `result_egress_role` names the role that owns the terminal response, and the
    // projection derives `terminalResponseOwner` from it. Picking the last unit instead made the
    // head's owner a function of the member count, so adding a rank would silently move it --
    // and C6, which says rank 0 samples, disagreed with both. The configured role is the one
    // authority here; the last unit is accepted only when the configured value names a role the
    // plan does not contain, which is a configuration error and is reported as one.
    const auto egress = split.value("result_egress_role", std::string{});
    if (egress.empty())
      throw std::invalid_argument("sharded Qwen catalog has no result egress role");
    // The configured value may name the unit outright, or name the stage -- which is what the
    // maintained catalogs do, and which maps to that stage's last rank because a stage's egress
    // is the rank that owns its head. Both forms resolve here rather than one being assumed, so a
    // catalog that names a rank explicitly is honoured and one that names a stage keeps working.
    const auto egressMember = std::find_if(units.begin(), units.end(),
      [&egress] (const auto& unit) { return unit.roleId == egress; });
    const auto egressUnit = egressMember != units.end()
      ? egressMember->roleId
      : [&] {
          const auto stagePrefix = egress + "/";
          std::optional<std::string> lastOfStage;
          std::string known;
          for (const auto& unit : units) {
            if (!known.empty()) known += ',';
            known += unit.roleId;
            if (unit.roleId.rfind(stagePrefix, 0) == 0) lastOfStage = unit.roleId;
          }
          if (!lastOfStage)
            throw std::invalid_argument(
              "sharded Qwen catalog names a result egress role the plan does not contain: " +
              egress + " (known: " + known + ")");
          return *lastOfStage;
        }();
    projectedCandidate = qwen::buildQwenTpCandidate(
      result.model.graph, shape, units, "entry", egressUnit, result.model.descriptor,
      artifactByUnit, requirementByUnit);
    // The candidate belongs to the strategy that built it, and the planner compares that
    // identity against the registered strategy before it places anything. A projected
    // candidate carries its own builder name, which is not the registered strategy, so the
    // request was refused at ACK_CLOSED as a foreign identity. Adopt the catalog's identity
    // here, where the candidate is produced under that strategy.
    projectedCandidate->splitter = result.cooperativeSplitter->identity();
    }
  }
  const auto enumerated = projectedCandidate
    ? std::vector<NativeSplitCandidate>{}
    : result.cooperativeSplitter->enumerate(
        result.model.descriptor, result.model.graph, preparationBudget, extensionControl);
  if (splitterKind == "AUTOMATIC" && !enumerated.empty() &&
      result.stateMapping.inputs.empty()) {
    // The catalog did not declare a state map; derive it from the checked
    // automatic projection instead of inventing one.
    const auto familyMap = [] (const auto& byRole) {
      // One mapping entry per declared state contract: the semantic name is the
      // contract name and the value is its physical tensor (usually itself).
      NativeStateTensorMapping::Roles result;
      for (const auto& [role, tensors] : byRole) {
        auto& entry = result[role];
        for (const auto& tensor : tensors) entry[tensor.name] = {tensor.name};
      }
      return result;
    };
    result.stateMapping.inputs = familyMap(enumerated.front().roleStateInputsByRole);
    result.stateMapping.outputs = familyMap(enumerated.front().roleStateOutputsByRole);
  }
  // The automatic splitter may return many cuts with different state covers.
  // Preparation binds one state map, so it prepares the first (lowest-transfer)
  // candidate only instead of mixing maps across candidates.
  const std::vector<NativeSplitCandidate> selectedCandidates =
    splitterKind == "AUTOMATIC" && enumerated.size() > 1 ?
      std::vector<NativeSplitCandidate>{enumerated.front()} : enumerated;
  preparedCandidates.reserve(selectedCandidates.size());
  for (auto candidate : selectedCandidates) {
    if (splitterKind == "AUTOMATIC") {
      // The assembly budget is an explicit upper bound, not a measurement:
      // ORT workspace and activation peaks stay within it or preparation fails
      // at the actual stage owner. Transient memory is declared inapplicable
      // for this CPU profile instead of being treated as free.
      for (auto& [role, requirement] : candidate.requirementsByRole) {
        if (!requirement.workspaceBytes)
          requirement.workspaceBytes = entry.recipe.maxAssembledBytes;
        if (!requirement.transientBytes) requirement.transientBytes = std::uint64_t{0};
        if (!requirement.activationBytes)
          requirement.activationBytes = entry.recipe.maxAssembledBytes;
      }
      candidate.candidateDigest = candidate.computedDigest();
    }
    if (sourceDefined) {
      candidate.requirementsByRole = resourceProfiles;
      candidate.candidateDigest = candidate.computedDigest();
    }
    auto bound = result.preparation->bindStateContracts(
      result.model, candidate, result.stateMapping, planningControl,
      sourceDefined ? NativeStateContractBinding::SOURCE : NativeStateContractBinding::DECLARED);
    auto roles = result.preparation->prepareRoles(result.model, bound, planningControl);
    preparedCandidates.push_back({std::move(bound), std::move(roles)});
  }
  if (projectedCandidate) {
    auto candidate = std::move(*projectedCandidate);
    // Every unit inherits only its logical stage's source tensors. The role preparer
    // still validates these names and their shapes against the actual source boundary.
    if (sourceRoleByUnit.size() != candidate.executionPlan.roles.size() ||
        std::any_of(candidate.executionPlan.roles.begin(), candidate.executionPlan.roles.end(),
          [&] (const auto& unit) { return sourceRoleByUnit.count(unit) != 1; }))
      throw std::invalid_argument("native state role expansion has an incomplete unit cover");
    result.stateMapping = detail::remapNativeStateRoles(result.stateMapping, sourceRoleByUnit);
    // No resource override here. `resourceProfiles` is keyed by the catalog's stage
    // roles, while this candidate's roles are the expanded unit names, so assigning it
    // would replace a complete per-unit cover with a per-stage one and the candidate
    // would be refused for an incomplete role cover. The unit requirements were derived
    // from each stage's profile when the units were built.
    candidate.candidateDigest = candidate.computedDigest();
    auto bound = result.preparation->bindStateContracts(
      result.model, candidate, result.stateMapping, planningControl,
      NativeStateContractBinding::SOURCE);
    auto roles = result.preparation->prepareRoles(result.model, bound, planningControl);
    preparedCandidates.push_back({std::move(bound), std::move(roles)});
  }
  if (preparedCandidates.empty())
    throw std::runtime_error("DI_NATIVE_PREPARATION_NO_PLANNING_CANDIDATE");
  auto transferProfiles = root.value("request_transfer_profiles", NativeJson::array());
  if (!transferProfiles.is_array() || transferProfiles.size() > 8)
    throw std::invalid_argument("invalid request transfer profile collection");
  for (const auto& profile : transferProfiles) {
    if (!profile.is_object() ||
        profile.value("schema", std::string{}) != "ndnsf-di-request-transfer-profile-v1" ||
        profile.value("source_digest", std::string{}) != result.model.canonicalSourceDigest ||
        !profile.value("initial_context", false) ||
        !profile.contains("dependencies") || !profile.at("dependencies").is_array() ||
        profile.at("dependencies").size() > 64)
      throw std::invalid_argument("request transfer profile differs from pinned source");
  }
  auto ownedTemplate = std::shared_ptr<const NativePreparedPlanningTemplate>(
    new const NativePreparedPlanningTemplate(
      result.preparation->modelRefFor(result.model.descriptor), result.cooperativeSplitter,
      preparedCandidates));
  result.planningCache = std::shared_ptr<const NativePreparedPlanningCache>(
    new const NativePreparedPlanningCache(result.cooperativeSplitter->identity(),
      std::move(preparedCandidates), std::move(transferProfiles), std::move(ownedTemplate)));
  control.requireActive();
  return result;
}
} // namespace ndnsf::di
