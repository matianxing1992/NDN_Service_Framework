#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <limits>
#include <locale>
#include <set>

namespace ndnsf::di {
namespace {

void expandStateContractsFromCatalog(NativeSplitCandidate& candidate,
                                     const NativeStateTensorMapping& mapping,
                                     const NativeModelDescriptor& model)
{
  const auto expand = [&model] (const NativeStateTensorMapping::Roles& mapped,
                                auto& declared) {
    if (mapped.empty()) return;
    for (const auto& item : mapped) {
      const auto found = declared.find(item.first);
      if (found == declared.end() || item.second.size() == found->second.size())
        continue;
      std::vector<NativeTensorContract> replacement;
      for (const auto& semantic : item.second) {
        for (const auto& name : semantic.second)
          replacement.push_back({name, model.precision,
            {"batch", "heads", "sequence", "head-dimension"}, std::nullopt});
      }
      if (!replacement.empty()) found->second = std::move(replacement);
    }
  };
  expand(mapping.inputs, candidate.roleStateInputsByRole);
  expand(mapping.outputs, candidate.roleStateOutputsByRole);
  candidate.candidateDigest = candidate.computedDigest();
}

NativeJson tensorContractJson(const NativeTensorContract& value)
{
  NativeJson shape = NativeJson::array();
  for (const auto& dimension : value.shape)
    shape.push_back(std::holds_alternative<std::string>(dimension)
      ? NativeJson(std::get<std::string>(dimension))
      : NativeJson(std::get<std::int64_t>(dimension)));
  return NativeJson{{"name", value.name}, {"dtype", value.dtype}, {"shape", std::move(shape)},
    {"estimatedBytes", value.estimatedBytes ? NativeJson(*value.estimatedBytes) : NativeJson(nullptr)}};
}

NativeTensorContract tensorContractFromJson(const NativeJson& value)
{
  NativeTensorContract result;
  result.name = value.at("name").get<std::string>();
  result.dtype = value.at("dtype").get<std::string>();
  for (const auto& dimension : value.at("shape")) {
    if (dimension.is_string()) result.shape.emplace_back(dimension.get<std::string>());
    else result.shape.emplace_back(dimension.get<std::int64_t>());
  }
  if (!value.at("estimatedBytes").is_null())
    result.estimatedBytes = value.at("estimatedBytes").get<std::uint64_t>();
  result.validate();
  return result;
}

std::string preparedMetadataJson(const NativeInspectedModel& model,
                                 const NativeOnnxGraphInspection& sourceGraph)
{
  NativeJson nodes = NativeJson::array();
  for (const auto& node : sourceGraph.graph.nodes)
    nodes.push_back({{"id", node.id}, {"opType", node.opType}, {"ordinal", node.ordinal}});
  NativeJson outputs = NativeJson::array();
  for (const auto& output : sourceGraph.graph.modelOutputs)
    outputs.push_back(tensorContractJson(output));
  NativeJson nodeIndices = NativeJson::object();
  for (const auto& item : sourceGraph.canonicalNodeIndices)
    nodeIndices[item.first] = item.second;
  return nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-metadata-v1"},
    {"descriptor", nativeParseJson(model.descriptor.canonicalJson())},
    {"source", {{"name", model.canonicalSourceName},
                 {"digest", model.canonicalSourceDigest},
                 {"bytes", model.canonicalSourceBytes},
                 {"manifest_digest", model.modelManifestDigest},
                 {"graph_digest", model.canonicalGraphDigest},
                 {"initializer_object_digest", model.canonicalInitializerObjectDigest},
                 {"initializer_bytes", model.canonicalInitializerBytes},
                 {"initializer_digest", model.canonicalInitializerDigest}}},
    {"sourceGraph", {{"graphDigest", sourceGraph.graph.graphDigest},
                      {"nodes", std::move(nodes)},
                      {"modelOutputs", std::move(outputs)},
                      {"graphMetadataJson", sourceGraph.graphMetadataJson},
                      {"canonicalNodeIndices", std::move(nodeIndices)},
                      {"canonicalIdentity", {{"graphDigest", sourceGraph.canonicalIdentity.graphDigest},
                                               {"initializerDigest", sourceGraph.canonicalIdentity.initializerDigest}}}}}
  });
}

NativeOnnxGraphInspection sourceGraphFromPreparedMetadata(const NativeJson& metadata)
{
  if (metadata.value("schema", std::string{}) != "ndnsf-di-prepared-metadata-v1")
    throw std::invalid_argument("unsupported native prepared metadata schema");
  const auto& encoded = metadata.at("sourceGraph");
  NativeOnnxGraphInspection result;
  result.graph.graphDigest = encoded.at("graphDigest").get<std::string>();
  result.graphMetadataJson = encoded.at("graphMetadataJson").get<std::string>();
  if (result.graphMetadataJson.empty())
    throw std::invalid_argument("native prepared graph metadata is empty");
  for (const auto& node : encoded.at("nodes"))
    result.graph.nodes.push_back({node.at("id").get<std::string>(),
      node.at("opType").get<std::string>(), node.at("ordinal").get<std::uint64_t>()});
  for (const auto& output : encoded.at("modelOutputs"))
    result.graph.modelOutputs.push_back(tensorContractFromJson(output));
  for (const auto& item : encoded.at("canonicalNodeIndices").items())
    result.canonicalNodeIndices[item.key()] = item.value().get<std::uint64_t>();
  const auto& identity = encoded.at("canonicalIdentity");
  result.canonicalIdentity.graphDigest = identity.at("graphDigest").get<std::string>();
  result.canonicalIdentity.initializerDigest = identity.at("initializerDigest").get<std::string>();
  result.nodeNames.reserve(result.graph.nodes.size());
  for (const auto& node : result.graph.nodes)
    result.nodeNames.push_back(node.id);
  return result;
}

} // namespace

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
  const auto& sourceConfig = root.at("source");
  const bool referenceOnly = source.modelBytes.empty() && !source.preparedMetadataJson.empty();
  model.canonicalSourceName = sourceConfig.at("data_name").get<std::string>();
  model.canonicalSourceDigest = sourceConfig.at("digest").get<std::string>();
  model.modelManifestDigest = sourceConfig.at("model_manifest_digest").get<std::string>();
  model.canonicalGraphDigest = sourceConfig.at("canonical_graph_digest").get<std::string>();
  if (referenceOnly) {
    const auto metadata = nativeParseJson(source.preparedMetadataJson);
    if (metadata.value("schema", std::string{}) != "ndnsf-di-prepared-metadata-v1")
      throw std::invalid_argument("unsupported native prepared metadata schema");
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
    entry.sourceGraphInspection = sourceGraphFromPreparedMetadata(metadata);
  }
  else {
    model.canonicalSourceBytes = source.modelBytes.size();
    if (nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) != model.canonicalSourceDigest)
      throw std::invalid_argument("request source bytes differ from pinned digest");
    if (source.initializerBytes) {
      model.canonicalInitializerBytes = source.initializerBytes->size();
      model.canonicalInitializerObjectDigest = sourceConfig.at("initializer_digest").get<std::string>();
      if (nativePlanningDigest(source.initializerBytes->data(), source.initializerBytes->size()) !=
          model.canonicalInitializerObjectDigest)
        throw std::invalid_argument("request initializer bytes differ from pinned digest");
    }
    else if (sourceConfig.contains("initializer_digest"))
      throw std::invalid_argument("pinned initializer object is missing");
    if (source.initializerBytes && model.descriptor.modelFormat == "onnx") {
      // The catalog pins the fetched external object above. Assembly recipes
      // bind a separate digest over normalized ONNX initializer contents.
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
  const auto& split = root.at("splitter");
  const auto splitterKind = split.at("kind").get<std::string>();
  if (splitterKind == "QWEN" || splitterKind == "LLAMA") {
    const auto modelFamily = splitterKind == "LLAMA" ? "llama" : "qwen";
    auto strategy = std::make_shared<qwen::NativeQwenLayerSplit>(
      split.at("layer_ranges").get<std::vector<qwen::NativeQwenLayerSplit::LayerRange>>(),
      split.at("artifact_digests_by_role").get<std::map<std::string, std::string>>(),
      split.at("weight_bytes_by_role").get<std::map<std::string, std::uint64_t>>(),
      split.at("roles").get<std::vector<std::string>>(), split.at("tensor_degrees").get<std::vector<std::uint64_t>>(),
      split.value("input_ingress_role", std::string{}), split.value("result_egress_role", std::string{}),
      modelFamily);
    model.graph = strategy->inspectGraph(model.descriptor, model.descriptor.sourceRevision, entry.recipe.maxNodes);
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
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
  else throw std::invalid_argument("unsupported native request splitter");
  result.stateMapping.inputs = root.value("state_inputs", NativeStateTensorMapping::Roles{});
  result.stateMapping.outputs = root.value("state_outputs", NativeStateTensorMapping::Roles{});
  result.model = model;
  if (!referenceOnly) {
    entry.sourceGraphInspection = inspectNativeOnnxSourceGraph(source, model.descriptor, control);
    // Only adapters with a reference-only restore contract publish this
    // metadata. Unsupported adapters remain on the source-backed path and
    // therefore cannot create a receipt that a later prepare cannot restore.
    if (splitterKind == "QWEN" || splitterKind == "LLAMA")
      source.preparedMetadataJson = preparedMetadataJson(model, *entry.sourceGraphInspection);
  }
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
  const auto enumerated = result.cooperativeSplitter->enumerate(
    result.model.descriptor, result.model.graph, preparationBudget, extensionControl);
  preparedCandidates.reserve(enumerated.size());
  for (auto candidate : enumerated) {
    expandStateContractsFromCatalog(candidate, result.stateMapping, result.model.descriptor);
    auto bound = result.preparation->bindStateContracts(
      result.model, candidate, result.stateMapping, planningControl);
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
