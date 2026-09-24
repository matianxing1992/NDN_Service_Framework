#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include <algorithm>
#include <cstring>

namespace ndnsf::di {
namespace {

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
  // The complete operator-pinned catalog is available before source loading.
  // Use it as the durable publication identity so initializer/profile/split
  // changes cannot collide under a source-only Repo path.
  entry.publication.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(root));
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
  control.requireActive();
  return result;
}
} // namespace ndnsf::di
