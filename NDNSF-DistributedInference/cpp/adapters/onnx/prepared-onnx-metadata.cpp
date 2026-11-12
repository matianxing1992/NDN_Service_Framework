#include "prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

void active(const NativeAssemblyControl& control);

NativeJson
tensorContractJson(const NativeTensorContract& value)
{
  NativeJson shape = NativeJson::array();
  for (const auto& dimension : value.shape)
    shape.push_back(std::holds_alternative<std::string>(dimension)
      ? NativeJson(std::get<std::string>(dimension))
      : NativeJson(std::get<std::int64_t>(dimension)));
  return NativeJson{{"name", value.name}, {"dtype", value.dtype}, {"shape", std::move(shape)},
    {"estimatedBytes", value.estimatedBytes ? NativeJson(*value.estimatedBytes)
                                            : NativeJson(nullptr)}};
}

NativeTensorContract
tensorContractFromJson(const NativeJson& value)
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

NativeJson
preparedMetadataValue(const NativeInspectedModel& model,
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
  return NativeJson{
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
                      {"initializerConsumers", sourceGraph.initializerConsumers},
                      {"canonicalIdentity", {{"graphDigest", sourceGraph.canonicalIdentity.graphDigest},
                                               {"initializerDigest", sourceGraph.canonicalIdentity.initializerDigest}}}}}
  };
}

NativeOnnxGraphInspection
sourceGraphFromPreparedMetadata(
  const NativeJson& metadata, const NativeAssemblyControl& control)
{
  const auto& encoded = metadata.at("sourceGraph");
  NativeOnnxGraphInspection result;
  result.graph.graphDigest = encoded.at("graphDigest").get<std::string>();
  result.graphMetadataJson = encoded.at("graphMetadataJson").get<std::string>();
  if (result.graphMetadataJson.empty())
    throw std::invalid_argument("native prepared graph metadata is empty");
  for (const auto& node : encoded.at("nodes")) {
    active(control);
    result.graph.nodes.push_back({node.at("id").get<std::string>(),
      node.at("opType").get<std::string>(), node.at("ordinal").get<std::uint64_t>()});
  }
  for (const auto& output : encoded.at("modelOutputs")) {
    active(control);
    result.graph.modelOutputs.push_back(tensorContractFromJson(output));
  }
  for (const auto& item : encoded.at("canonicalNodeIndices").items()) {
    active(control);
    result.canonicalNodeIndices[item.key()] = item.value().get<std::uint64_t>();
  }
  if (!encoded.contains("initializerConsumers") ||
      !encoded.at("initializerConsumers").is_object())
    throw std::invalid_argument(
      "native prepared metadata requires initializer ownership; reprepare source");
  for (const auto& item : encoded.at("initializerConsumers").items()) {
    active(control);
    if (!item.value().is_array())
      throw std::invalid_argument("native prepared initializer consumers must be an array");
    auto& consumers = result.initializerConsumers[item.key()];
    for (const auto& index : item.value()) {
      active(control);
      if (!index.is_number_unsigned() || index.get<std::uint64_t>() >= result.graph.nodes.size())
        throw std::invalid_argument("native prepared initializer consumer is outside source graph");
      consumers.push_back(index.get<std::uint64_t>());
    }
  }
  const auto& identity = encoded.at("canonicalIdentity");
  result.canonicalIdentity.graphDigest = identity.at("graphDigest").get<std::string>();
  result.canonicalIdentity.initializerDigest = identity.at("initializerDigest").get<std::string>();
  result.nodeNames.reserve(result.graph.nodes.size());
  for (const auto& node : result.graph.nodes)
    result.nodeNames.push_back(node.id);
  return result;
}


[[noreturn]] void
rejectMetadata(const char* reason)
{
  throw std::invalid_argument(reason);
}

void
active(const NativeAssemblyControl& control)
{
  if (!control.requireActive) rejectMetadata("PREPARED_METADATA_OWNER");
  control.requireActive();
  if (std::chrono::steady_clock::now() >= control.deadline)
    throw std::runtime_error("PREPARED_METADATA_DEADLINE");
}

void
requireKeys(const NativeJson& value, std::initializer_list<const char*> required,
            std::initializer_list<const char*> optional = {})
{
  if (!value.is_object()) rejectMetadata("PREPARED_METADATA_FIELDS");
  for (const auto* name : required)
    if (!value.contains(name)) rejectMetadata("PREPARED_METADATA_FIELDS");
  for (const auto& item : value.items()) {
    const auto matches = [&](const char* name) { return item.key() == name; };
    if (std::none_of(required.begin(), required.end(), matches) &&
        std::none_of(optional.begin(), optional.end(), matches))
      rejectMetadata("PREPARED_METADATA_FIELDS");
  }
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

std::string
stringField(const NativeJson& value, const char* field)
{
  if (!value.at(field).is_string()) rejectMetadata("PREPARED_METADATA_STRING");
  return value.at(field).get<std::string>();
}

std::uint64_t
unsignedValue(const NativeJson& number)
{
  if (!number.is_number_integer() ||
      (!number.is_number_unsigned() && number.get<std::int64_t>() < 0))
    rejectMetadata("PREPARED_METADATA_INTEGER");
  return number.get<std::uint64_t>();
}

std::uint64_t
unsignedField(const NativeJson& value, const char* field)
{
  return unsignedValue(value.at(field));
}

bool
automaticVersion(const NativeJson& metadata, const NativeAssemblyControl& control)
{
  active(control);
  if (!metadata.is_object() || !metadata.contains("schema") ||
      !metadata.at("schema").is_string()) rejectMetadata("PREPARED_METADATA_VERSION");
  const auto schema = metadata.at("schema").get<std::string>();
  if (schema == "ndnsf-di-prepared-metadata-v1") {
    if (metadata.contains("automatic_onnx")) rejectMetadata("PREPARED_METADATA_DOWNGRADE");
    if (metadata.contains("descriptor") && metadata.at("descriptor").is_object()) {
      const auto& descriptor = metadata.at("descriptor");
      if (descriptor.contains("adapter") && descriptor.at("adapter").is_object() &&
          descriptor.at("adapter").value("name", std::string{}) == "automatic-onnx")
        rejectMetadata("PREPARED_METADATA_DOWNGRADE");
    }
    return false;
  }
  if (schema != "ndnsf-di-prepared-metadata-v2") rejectMetadata("PREPARED_METADATA_VERSION");
  requireKeys(metadata, {"schema", "descriptor", "source", "sourceGraph", "automatic_onnx"},
              {"protectedObjects"});
  if (metadata.contains("protectedObjects") && !metadata.at("protectedObjects").is_array())
    rejectMetadata("PREPARED_METADATA_PROTECTED_OBJECTS");
  const auto limit = std::min(control.maxAssembledBytes,
                              NativeCanonicalMaterialPublicationMaxBytes);
  if (limit == 0 || nativeCanonicalJson(metadata).size() > limit)
    rejectMetadata("PREPARED_METADATA_LIMIT");
  active(control);
  return true;
}

std::shared_ptr<const AutomaticOnnxContract>
decodeAutomatic(const NativeJson& metadata, const NativeAssemblyControl& control)
{
  const auto& automatic = metadata.at("automatic_onnx");
  const auto& source = metadata.at("source");
  requireKeys(source, {"name", "digest", "bytes", "manifest_digest", "graph_digest",
                       "initializer_object_digest", "initializer_bytes", "initializer_digest"});
  const auto modelDigest = stringField(source, "digest");
  if (stringField(source, "name").empty() || unsignedField(source, "bytes") == 0 ||
      !isDigest(stringField(source, "manifest_digest")) ||
      !isDigest(stringField(source, "graph_digest")) ||
      !isDigest(stringField(source, "initializer_digest")))
    rejectMetadata("PREPARED_METADATA_SOURCE");
  const auto contract = decodeNativeAutomaticOnnxSourceMetadata(
    automatic, modelDigest, unsignedField(source, "initializer_bytes"),
    stringField(source, "initializer_object_digest"), control);
  const auto descriptorJson = nativeCanonicalJson(metadata.at("descriptor"));
  const auto descriptor = NativeModelDescriptor::fromCanonicalJson(descriptorJson);
  if (descriptor.canonicalJson() != descriptorJson || descriptor.modelFormat != "onnx" ||
      descriptor.adapter.name != "automatic-onnx" ||
      descriptor.contentDigest != contract->sourceDigest)
    rejectMetadata("PREPARED_METADATA_CONTRACT_IDENTITY");
  const auto& graph = metadata.at("sourceGraph");
  requireKeys(graph, {"graphDigest", "nodes", "modelOutputs", "graphMetadataJson",
                      "canonicalNodeIndices", "initializerConsumers", "canonicalIdentity"});
  requireKeys(graph.at("canonicalIdentity"), {"graphDigest", "initializerDigest"});
  if (stringField(graph.at("canonicalIdentity"), "graphDigest") !=
        stringField(source, "graph_digest") ||
      stringField(graph.at("canonicalIdentity"), "initializerDigest") !=
        stringField(source, "initializer_digest") || !isDigest(stringField(graph, "graphDigest")) ||
      stringField(graph, "graphDigest") != descriptor.graphDigest ||
      !graph.at("nodes").is_array() || !graph.at("modelOutputs").is_array() ||
      !graph.at("canonicalNodeIndices").is_object())
    rejectMetadata("PREPARED_METADATA_GRAPH");
  std::uint64_t ordinal = 0;
  for (const auto& node : graph.at("nodes")) {
    active(control);
    requireKeys(node, {"id", "opType", "ordinal"});
    if (stringField(node, "id").empty() || stringField(node, "opType").empty())
      rejectMetadata("PREPARED_METADATA_GRAPH");
    if (unsignedField(node, "ordinal") != ordinal++) rejectMetadata("PREPARED_METADATA_GRAPH");
  }
  const auto& indices = graph.at("canonicalNodeIndices");
  if (indices.size() != graph.at("nodes").size()) rejectMetadata("PREPARED_METADATA_GRAPH");
  for (const auto& item : indices.items()) {
    active(control);
    const auto index = unsignedValue(item.value());
    if (index >= graph.at("nodes").size() ||
        graph.at("nodes").at(index).at("id") != item.key())
      rejectMetadata("PREPARED_METADATA_NODE_INDEX");
  }
  for (const auto& output : graph.at("modelOutputs")) {
    active(control);
    requireKeys(output, {"name", "dtype", "shape", "estimatedBytes"});
    if (stringField(output, "name").empty() || stringField(output, "dtype").empty() ||
        !output.at("shape").is_array()) rejectMetadata("PREPARED_METADATA_TENSOR");
    if (!output.at("estimatedBytes").is_null()) unsignedField(output, "estimatedBytes");
    for (const auto& axis : output.at("shape")) {
      active(control);
      // Symbol strings retain the original graph owner's representation.
      if (axis.is_string()) continue;
      if (unsignedValue(axis) > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        rejectMetadata("PREPARED_METADATA_DIMENSION");
    }
  }
  active(control);
  return contract;
}

} // namespace

std::shared_ptr<const AutomaticOnnxContract>
decodeNativeAutomaticOnnxSourceMetadata(const NativeJson& automatic,
                                      const std::string& modelDigest,
                                      std::uint64_t initializerBytes,
                                      const std::string& initializerObjectDigest,
                                      const NativeAssemblyControl& control)
{
  active(control);
  requireKeys(automatic, {"contract", "externalFiles"});
  if (!automatic.at("externalFiles").is_array() ||
      automatic.at("externalFiles").size() > NATIVE_ONNX_MAX_EXTERNAL_FILES)
    rejectMetadata("PREPARED_METADATA_INVENTORY");
  if (nativeCanonicalJson(automatic).size() >
      std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes))
    rejectMetadata("PREPARED_METADATA_LIMIT");
  std::vector<AutomaticOnnxSourceFile> files;
  std::uint64_t inventoryBytes = 0;
  for (const auto& file : automatic.at("externalFiles")) {
    active(control);
    requireKeys(file, {"location", "bytes", "digest"});
    AutomaticOnnxSourceFile item{stringField(file, "location"), unsignedField(file, "bytes"),
                                 stringField(file, "digest")};
    if (item.bytes > std::numeric_limits<std::uint64_t>::max() - inventoryBytes)
      rejectMetadata("PREPARED_METADATA_INVENTORY");
    inventoryBytes += item.bytes;
    files.push_back(std::move(item));
  }
  if (inventoryBytes != initializerBytes ||
      (inventoryBytes != 0 && !isDigest(initializerObjectDigest)) ||
      (inventoryBytes == 0 && !initializerObjectDigest.empty()))
    rejectMetadata("PREPARED_METADATA_INITIALIZER");
  const auto bundle = automaticOnnxSourceDigest(modelDigest, files, control);
  auto contract = AutomaticOnnxContract::fromCanonicalJson(
    nativeCanonicalJson(automatic.at("contract")), control);
  if (contract.sourceDigest != bundle)
    rejectMetadata("PREPARED_METADATA_CONTRACT_IDENTITY");
  active(control);
  return std::make_shared<const AutomaticOnnxContract>(std::move(contract));
}

std::string
encodeNativePreparedOnnxMetadata(const NativeInspectedModel& model,
                                const NativeOnnxGraphInspection& graph,
                                const AutomaticOnnxModel* automaticModel,
                                const NativeAssemblyControl& control)
{
  active(control);
  if (!automaticModel && model.descriptor.adapter.name == "automatic-onnx")
    rejectMetadata("PREPARED_METADATA_DOWNGRADE");
  auto value = preparedMetadataValue(model, graph);
  if (automaticModel) {
    if (automaticModel->descriptor.canonicalJson() != model.descriptor.canonicalJson() ||
        automaticModel->opened.modelDigest != model.canonicalSourceDigest ||
        automaticModel->opened.sourceDigest != automaticModel->contract.sourceDigest ||
        automaticModel->opened.source.modelBytes.size() != model.canonicalSourceBytes ||
        (&graph != &automaticModel->inspection &&
         preparedMetadataValue(model, automaticModel->inspection).at("sourceGraph") !=
           value.at("sourceGraph")))
      rejectMetadata("PREPARED_METADATA_SOURCE_MISMATCH");
    auto files = NativeJson::array();
    for (const auto& file : automaticModel->opened.externalFiles) {
      active(control);
      files.push_back({{"location", file.location}, {"bytes", file.bytes},
                       {"digest", file.digest}});
    }
    value["schema"] = "ndnsf-di-prepared-metadata-v2";
    value["automatic_onnx"] = {
      {"contract", nativeParseJson(automaticModel->contract.canonicalJson(control))},
      {"externalFiles", std::move(files)}};
    // This checks declarations, not the original physical files a second time.
    decodeNativePreparedOnnxMetadata(value, control);
  }
  active(control);
  return nativeCanonicalJson(value);
}

NativePreparedOnnxMetadata
decodeNativePreparedOnnxMetadata(const NativeJson& metadata, const NativeAssemblyControl& control)
{
  NativePreparedOnnxMetadata result;
  if (automaticVersion(metadata, control))
    result.automaticContract = decodeAutomatic(metadata, control);
  result.graph = sourceGraphFromPreparedMetadata(metadata, control);
  active(control);
  return result;
}

void
validateNativePreparedMetadataVersion(const NativeJson& metadata,
                                     const NativeAssemblyControl& control)
{
  // V1 publication also supports its historical opaque fixture metadata.
  // Only automatic v2 promises a complete graph/contract codec here.
  if (automaticVersion(metadata, control)) decodeNativePreparedOnnxMetadata(metadata, control);
}

} // namespace ndnsf::di
