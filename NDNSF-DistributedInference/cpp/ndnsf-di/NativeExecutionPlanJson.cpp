#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include <ndn-cxx/encoding/tlv.hpp>
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <initializer_list>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ndnsf::di {

namespace {
NativeRoleDataflowContractV3 bindFixedTemplateDataflow(
  const NativeJson& bindings, const NativeSelectionProjectionV3& context,
  const NativeFixedExecutionTemplateV1& fixed);
std::vector<NativeDependencySpec> bindFixedTemplateDependencies(
  const NativeJson& bindings, const NativeFixedExecutionTemplateV1& fixed);
void requireOnlyBindingKeys(const NativeJson& value, std::initializer_list<const char*> keys);

NativeJson jsonValue(const NativeAssemblyTensorContractV3& value);
NativeJson jsonValue(const NativeSelectionRoleV3& value);
NativeJson jsonValue(const NativeExecutionRoleV3& value);
NativeJson jsonValue(const NativeTensorEndpointV3& value);
NativeJson jsonValue(const NativeReadinessPredicateV3& value);
NativeJson jsonValue(const NativeRoleDataflowContractV3& value);
NativeJson jsonValue(const NativeDeviceBindingV3& value);
NativeJson jsonValue(const NativeGenerationExecutionContractV1& value);
NativeJson jsonValue(const RedistributionSpec& value);
NativeJson jsonValue(const NativeDependencySpec& value);
NativeJson jsonValue(const ConversationStateReferenceV1& value);
NativeJson jsonValue(const ConversationTurnBindingV1& value);

NativeJson jsonValue(const NativeGrantLeaseScope& value)
{
  return NativeJson{{"schema", "ndnsf-di-grant-lease-scope-v1"}, {"version", 1},
    {"conversation_id", value.conversationId}, {"requester_identity", value.requesterIdentity},
    {"service_name", value.serviceName}, {"request_id", value.requestId},
    {"attempt", value.attempt}, {"plan_core_digest", value.planCoreDigest},
    {"scope_digest", value.scopeDigest},
    {"security_policy_snapshot_digest", value.securityPolicySnapshotDigest},
    {"protection_epoch", value.protectionEpoch}, {"expires_at_ms", value.expiresAtMs},
    {"provider_by_role", value.providerByRole}};
}

template<typename T>
NativeJson jsonArray(const std::vector<T>& values)
{
  auto result = NativeJson::array();
  for (const auto& value : values) result.push_back(jsonValue(value));
  return result;
}

NativeJson jsonValue(const NativeAssemblyTensorContractV3& value)
{
  auto result = NativeJson::object();
  result["name"] = value.name;
  result["dtype"] = value.dtype;
  result["shape"] = NativeJson::array();
  for (const auto& dimension : value.shape) {
    std::visit([&result] (const auto& item) { result["shape"].push_back(item); }, dimension);
  }
  return result;
}

template<typename Sink>
void visitRoleFields(const NativeSelectionRoleV3& value, Sink&& sink)
{
  sink("role", value.role);
  sink("rank", value.rank);
  sink("layer_begin", value.layerBegin);
  sink("layer_end", value.layerEnd);
  sink("backend", value.backend);
  sink("device_set", value.deviceSet);
  sink("required_device_memory_mb", value.requiredDeviceMemoryMb);
  sink("artifact_digest", value.artifactDigest);
  sink("recipe_digest", value.recipeDigest);
  sink("role_kind", value.roleKind);
  sink("adapter_id", value.adapterId);
  sink("adapter_version", value.adapterVersion);
  sink("model_manifest_digest", value.modelManifestDigest);
  sink("artifact_profile_digest", value.artifactProfileDigest);
  sink("graph_digest", value.graphDigest);
  sink("canonical_initializer_digest", value.canonicalInitializerDigest);
  sink("adapter_descriptor_digest", value.adapterDescriptorDigest);
  sink("assembler_descriptor_digest", value.assemblerDescriptorDigest);
  sink("backend_abi", value.backendAbi);
  sink("node_indices", value.nodeIndices);
  sink("expected_inputs", value.expectedInputs);
  sink("expected_outputs", value.expectedOutputs);
  sink("precision", value.precision);
  sink("quantization", value.quantization);
  sink("layout", value.layout);
  sink("padding", value.padding);
  sink("protection_epoch", value.protectionEpoch);
  sink("merge_kind", value.mergeKind);
  sink("postprocess_identity", value.postprocessIdentity);
  sink("postprocess_output_name", value.postprocessOutputName);
  sink("postprocess_confidence_threshold", value.postprocessConfidenceThreshold);
  sink("postprocess_sort", value.postprocessSort);
}

template<typename T>
NativeJson roleFieldJson(const T& value) { return NativeJson(value); }
NativeJson roleFieldJson(const std::vector<NativeAssemblyTensorContractV3>& value)
{ return jsonArray(value); }

NativeJson jsonValue(const NativeSelectionRoleV3& value)
{
  ++detail::fixedAssignmentWork().roleEncodes;
  auto result = NativeJson::object();
  visitRoleFields(value, [&](const char* key, const auto& field) {
    result[key] = roleFieldJson(field);
  });
  result["resource_envelope"] = NativeJson::object();
  if (value.maxSourceBytes || value.maxAssembledBytes || value.maxNodes) {
    result["resource_envelope"] = {{"maxSourceBytes", value.maxSourceBytes},
      {"maxAssembledBytes", value.maxAssembledBytes}, {"maxNodes", value.maxNodes}};
  }
  return result;
}

template<typename T>
bool roleFieldMatches(const NativeJson& json, const T& value)
{
  const NativeJson expected(value);
  if (json.is_number_float() != expected.is_number_float()) return false;
  // JSON signed/unsigned equality alone can alias a negative integer to a
  // large unsigned value, although their canonical wire spellings differ.
  if (json.is_number_integer() && expected.is_number_integer() &&
      json.is_number_unsigned() != expected.is_number_unsigned()) {
    const auto& signedValue = json.is_number_unsigned() ? expected : json;
    if (signedValue.template get<std::int64_t>() < 0) return false;
  }
  if (json.is_number_float() &&
      std::signbit(json.template get<double>()) != std::signbit(expected.template get<double>()))
    return false;
  return json == expected;
}

bool roleFieldMatches(const NativeJson& json, const NativeAssemblyTensorContractV3& value)
{
  if (!json.is_object() || json.size() != 3 || !json.contains("name") ||
      !json.contains("dtype") || !json.contains("shape") ||
      !roleFieldMatches(json.at("name"), value.name) ||
      !roleFieldMatches(json.at("dtype"), value.dtype)) return false;
  const auto& shape = json.at("shape");
  if (!shape.is_array() || shape.size() != value.shape.size()) return false;
  for (std::size_t i = 0; i < value.shape.size(); ++i)
    if (!std::visit([&](const auto& item) { return roleFieldMatches(shape[i], item); }, value.shape[i]))
      return false;
  return true;
}

template<typename T>
bool roleFieldMatches(const NativeJson& json, const std::vector<T>& value)
{
  if (!json.is_array() || json.size() != value.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i)
    if (!roleFieldMatches(json[i], value[i])) return false;
  return true;
}

bool assemblyJsonMatches(const NativeSelectionRoleV3& role, const NativeJson& value)
{
  // Preserve the canonical scalar renderer's finite/rounding failure
  // behavior, while avoiding a complete role object or wire encoding.
  (void)nativeCanonicalJson(NativeJson(role.postprocessConfidenceThreshold));
  if (!value.is_object()) return false;
  bool matches = true;
  std::size_t fields = 1; // resource_envelope
  visitRoleFields(role, [&](const char* key, const auto& field) {
    ++fields;
    const auto found = value.find(key);
    matches = matches && found != value.end() && roleFieldMatches(*found, field);
  });
  const auto envelope = value.find("resource_envelope");
  if (!matches || value.size() != fields || envelope == value.end() || !envelope->is_object())
    return false;
  if (!role.maxSourceBytes && !role.maxAssembledBytes && !role.maxNodes) return envelope->empty();
  return envelope->size() == 3 && envelope->contains("maxSourceBytes") &&
    envelope->contains("maxAssembledBytes") && envelope->contains("maxNodes") &&
    roleFieldMatches(envelope->at("maxSourceBytes"), role.maxSourceBytes) &&
    roleFieldMatches(envelope->at("maxAssembledBytes"), role.maxAssembledBytes) &&
    roleFieldMatches(envelope->at("maxNodes"), role.maxNodes);
}

NativeJson jsonValue(const NativeExecutionRoleV3& value)
{
  auto result = NativeJson::object();
  result["role_id"] = value.roleId;
  result["stage_id"] = value.stageId;
  result["rank"] = value.rank;
  result["layer_begin"] = value.layerBegin;
  result["layer_end"] = value.layerEnd;
  result["backend"] = value.backend;
  result["adapter_id"] = value.adapterId;
  result["adapter_version"] = value.adapterVersion;
  return result;
}

NativeJson jsonValue(const NativeTensorEndpointV3& value)
{
  auto result = NativeJson::object();
  result["producer_namespace"] = value.producerNamespace;
  result["requester"] = value.requester;
  result["request_id"] = value.requestId;
  result["attempt"] = value.attempt;
  result["plan_digest"] = value.planDigest;
  result["group_id"] = value.groupId;
  result["group_epoch"] = value.groupEpoch;
  result["operation"] = value.operation;
  result["round"] = value.round;
  result["naming_operation_stride"] = value.namingOperationStride;
  result["source_kind"] = value.sourceKind;
  result["producer_role"] = value.producerRole;
  result["producer_rank"] = value.producerRank;
  result["consumer_role"] = value.consumerRole;
  result["consumer_roles"] = value.consumerRoles;
  result["tensor_id"] = value.tensorId;
  result["bundle_tensor_names"] = value.bundleTensorNames;
  result["tensor_digest"] = value.tensorDigest;
  result["layout_digest"] = value.layoutDigest;
  result["target_layout_digest"] = value.targetLayoutDigest;
  result["microbatch"] = value.microbatch;
  result["segment_count"] = value.segmentCount;
  if (value.prefetchBundleBytes != 0 || value.prefetchSegmentBytes != 0) {
    result["prefetch_bundle_bytes"] = value.prefetchBundleBytes;
    result["prefetch_segment_bytes"] = value.prefetchSegmentBytes;
  }
  result["manifest_digest"] = value.manifestDigest;
  if (value.inlineFirstSegment) result["inline_first_segment"] = true;
  result["security_profile"] = value.securityProfile;
  result["no_progress_deadline_ms"] = value.noProgressDeadlineMs;
  result["hard_deadline_ms"] = value.hardDeadlineMs;
  result["endpoint_digest"] = value.endpointDigest;
  return result;
}

NativeJson jsonValue(const NativeReadinessPredicateV3& value)
{
  auto result = NativeJson::object();
  result["mode"] = value.mode;
  result["endpoint_digests"] = value.endpointDigests;
  result["quorum"] = value.quorum;
  return result;
}

NativeJson jsonValue(const NativeRoleDataflowContractV3& value)
{
  auto result = NativeJson::object();
  result["request_id"] = value.requestId;
  result["attempt"] = value.attempt;
  result["plan_digest"] = value.planDigest;
  result["role"] = value.role;
  result["may_publish"] = jsonArray(value.mayPublish);
  result["must_fetch"] = jsonArray(value.mustFetch);
  result["wait_for"] = jsonArray(value.waitFor);
  result["terminal_response_owner"] = value.terminalResponseOwner;
  result["dataflow_digest"] = value.dataflowDigest;
  return result;
}

NativeJson jsonValue(const NativeDeviceBindingV3& value)
{
  auto result = NativeJson::object();
  result["mode"] = value.mode;
  result["provider"] = value.provider;
  result["role"] = value.role;
  result["offer_digest"] = value.offerDigest;
  result["topology_profile_digest"] = value.topologyProfileDigest;
  result["resource_snapshot_digest"] = value.resourceSnapshotDigest;
  result["resource_sequence"] = value.resourceSequence;
  result["offer_scoped_device_handle"] = value.offerScopedDeviceHandle;
  result["sharing_policy"] = value.sharingPolicy;
  return result;
}

NativeJson jsonValue(const NativeGenerationExecutionContractV1& value)
{
  auto result = NativeJson::object();
  result["mode"] = value.mode;
  result["max_generated_tokens"] = value.maxGeneratedTokens;
  result["token_input_name"] = value.tokenInputName;
  result["state_input_names"] = value.stateInputNames;
  result["state_output_names"] = value.stateOutputNames;
  if (!value.stateSuccessorMap.empty())
    result["state_successor_map"] = value.stateSuccessorMap;
  if (!value.positionInputPolicy.empty())
    result["position_input_policy"] = value.positionInputPolicy;
  if (!value.attentionMaskInputName.empty())
    result["attention_mask_input_name"] = value.attentionMaskInputName;
  if (!value.positionIdsInputName.empty())
    result["position_ids_input_name"] = value.positionIdsInputName;
  if (!value.cachePositionInputName.empty())
    result["cache_position_input_name"] = value.cachePositionInputName;
  result["eos_token_ids"] = value.eosTokenIds;
  result["sampling_digest"] = value.samplingDigest;
  result["tokenizer_digest"] = value.tokenizerDigest;
  result["sampling_mode"] = value.samplingMode;
  result["sampling_temperature"] = value.samplingTemperature;
  result["sampling_top_k"] = value.samplingTopK;
  result["sampling_top_p"] = value.samplingTopP;
  result["sampling_repetition_penalty"] = value.samplingRepetitionPenalty;
  result["sampling_seed"] = value.samplingSeed;
  result["stop_strings"] = value.stopStrings;
  result["generation_id"] = value.generationId;
  result["committed_prefix_token_ids"] = value.committedPrefixTokenIds;
  result["streaming_operation_stride"] = value.streamingOperationStride;
  if (value.generationId.empty()) result.erase("generation_id");
  return result;
}

NativeJson jsonValue(const RedistributionSpec& value)
{
  auto result = NativeJson::object();
  result["producerRanks"] = value.producerRanks;
  result["consumerRanks"] = value.consumerRanks;
  result["tensor"] = value.tensor;
  result["operation"] = value.operation;
  result["epoch"] = value.epoch;
  result["integrityDigest"] = value.integrityDigest;
  result["sourceLayoutDigest"] = value.sourceLayoutDigest;
  result["targetLayoutDigest"] = value.targetLayoutDigest;
  result["axis"] = value.axis;
  result["temporaryMemoryBytes"] = value.temporaryMemoryBytes;
  result["completeOutput"] = value.completeOutput;
  return result;
}

NativeJson jsonValue(const NativeDependencySpec& value)
{
  auto result = NativeJson::object();
  result["tensors"] = value.tensors;
  result["producers"] = value.producers;
  result["consumers"] = value.consumers;
  result["key_scope"] = value.keyScope;
  result["topic_prefix"] = value.topicPrefix;
  result["object_name_template"] = value.objectNameTemplate;
  result["expected_segments"] = value.expectedSegments;
  result["expected_bytes"] = value.expectedBytes;
  result["operationKind"] = value.operationKind;
  result["collectiveOperationIndex"] = value.collectiveOperationIndex;
  result["collectiveProducerRank"] = value.collectiveProducerRank;
  result["collectiveSourceLayoutDigest"] = value.collectiveSourceLayoutDigest;
  result["collectiveTargetLayoutDigest"] = value.collectiveTargetLayoutDigest;
  result["collectiveTensorDigest"] = value.collectiveTensorDigest;
  result["redistributions"] = jsonArray(value.redistributions);
  result["transportProfile"] = value.useNdnsfDataV1 ? "NDNSF_DATA_V1" : "COLLAB_LARGE_V1";
  return result;
}

NativeJson jsonValue(const ConversationStateReferenceV1& value)
{
  auto result = NativeJson::object();
  result["conversation_id"] = value.conversationId;
  result["context_epoch"] = value.contextEpoch;
  result["service_name"] = value.serviceName;
  result["plan_role_map_digest"] = value.planRoleMapDigest;
  result["checkpoint_digest"] = value.checkpointDigest;
  result["role_name"] = value.roleName;
  result["role_receipt_digest"] = value.roleReceiptDigest;
  result["expires_at_ms"] = value.expiresAtMs;
  result["schema"] = "ndnsf-di-conversation-state-reference-v1";
  result["version"] = 1;
  return result;
}

NativeJson jsonValue(const ConversationTurnBindingV1& value)
{
  auto result = NativeJson::object();
  result["conversation_id"] = value.conversationId;
  result["parent_context_epoch"] = value.parentContextEpoch;
  result["successor_context_epoch"] = value.successorContextEpoch;
  result["service_name"] = value.serviceName;
  result["plan_role_map_digest"] = value.planRoleMapDigest;
  result["request_contract_digest"] = value.requestContractDigest;
  result["retention_deadline_ms"] = value.retentionDeadlineMs;
  result["parent_checkpoint_digest"] = value.parentCheckpointDigest;
  result["schema"] = "ndnsf-di-conversation-turn-binding-v1";
  result["version"] = 1;
  return result;
}

} // namespace

NativeJson nativeAssemblyJson(const NativeSelectionRoleV3& role) { return jsonValue(role); }
bool nativeAssemblyJsonMatches(const NativeSelectionRoleV3& role, const NativeJson& value)
{ return assemblyJsonMatches(role, value); }
NativeJson nativeDependenciesJson(const std::vector<NativeDependencySpec>& values) { return jsonArray(values); }
NativeJson nativeGenerationJson(const NativeGenerationExecutionContractV1& value) { return jsonValue(value); }
NativeJson nativeEndpointJson(const NativeTensorEndpointV3& value) { return jsonValue(value); }
NativeJson nativeDataflowJson(const NativeRoleDataflowContractV3& value) { return jsonValue(value); }
NativeJson nativeDeviceBindingJson(const NativeDeviceBindingV3& value) { return jsonValue(value); }


std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::string> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : child.get()) {
    values.push_back(item.second.get_value<std::string>());
  }
  return values;
}

namespace {

std::vector<std::uint64_t>
uintArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::uint64_t> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : *child) {
    values.push_back(item.second.get_value<std::uint64_t>());
  }
  return values;
}

std::vector<std::int64_t>
int64ArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::int64_t> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : *child) {
    values.push_back(item.second.get_value<std::int64_t>());
  }
  return values;
}

bool
validStateSuccessorMap(const std::string& encoded,
                        const std::vector<std::string>& stateInputs,
                        const std::vector<std::string>& stateOutputs)
{
  if (encoded.empty()) {
    return true;
  }
  std::set<std::string> inputs;
  std::set<std::string> outputs;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const auto end = encoded.find(',', start);
    const auto item = encoded.substr(start, end == std::string::npos
      ? std::string::npos : end - start);
    const auto separator = item.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= item.size() || item.find('=', separator + 1) != std::string::npos ||
        !inputs.insert(item.substr(0, separator)).second ||
        !outputs.insert(item.substr(separator + 1)).second) {
      return false;
    }
    start = end == std::string::npos ? encoded.size() : end + 1;
  }
  return inputs.size() == stateInputs.size() && outputs.size() == stateOutputs.size() &&
    std::set<std::string>(stateInputs.begin(), stateInputs.end()) == inputs &&
    std::set<std::string>(stateOutputs.begin(), stateOutputs.end()) == outputs;
}

void
validateAssemblyTensorContract(const NativeAssemblyTensorContractV3& value)
{
  if (value.name.empty() || value.dtype.empty()) {
    throw std::invalid_argument("invalid V3 assembly tensor contract");
  }
}

std::vector<NativeAssemblyTensorContractV3>
assemblyTensorContractsFromJson(const boost::property_tree::ptree& node,
                                const std::string& key)
{
  std::vector<NativeAssemblyTensorContractV3> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : *child) {
    NativeAssemblyTensorContractV3 value;
    value.name = item.second.get<std::string>("name", "");
    value.dtype = item.second.get<std::string>("dtype", "");
    for (const auto& dimension : stringArrayFromJson(item.second, "shape")) {
      value.shape.emplace_back(dimension);
    }
    validateAssemblyTensorContract(value);
    values.push_back(std::move(value));
  }
  return values;
}

bool
sameAssemblyTensorContracts(
  const std::vector<NativeAssemblyTensorContractV3>& left,
  const std::vector<NativeAssemblyTensorContractV3>& right)
{
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].name != right[index].name ||
        left[index].dtype != right[index].dtype ||
        left[index].shape != right[index].shape) {
      return false;
    }
  }
  return true;
}

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
           return std::isxdigit(ch) != 0;
         });
}

bool
isGenerationId(const std::string& value)
{
  return value.size() == 32 &&
         std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
           return (ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f');
         });
}

bool
hasCompleteAdapterIdentity(const std::string& adapterId,
                           const std::string& adapterVersion)
{
  return adapterId.empty() == adapterVersion.empty();
}

bool
isCpuBackend(const std::string& backend)
{
  return backend == "cpu" ||
         (backend.size() > 4 &&
          backend.compare(backend.size() - 4, 4, "-cpu") == 0);
}

void
validateSelectionRoleFields(const NativeSelectionRoleV3& role,
                            double decodedConfidence)
{
  const bool hasAssemblyIdentity =
    !role.modelManifestDigest.empty() || !role.artifactProfileDigest.empty() ||
    !role.graphDigest.empty() || !role.canonicalInitializerDigest.empty() ||
    !role.adapterDescriptorDigest.empty() ||
    !role.assemblerDescriptorDigest.empty() || !role.backendAbi.empty();
  const bool completeAssemblyIdentity =
    isSha256Digest(role.modelManifestDigest) &&
    isSha256Digest(role.artifactProfileDigest) &&
    isSha256Digest(role.graphDigest) &&
    isSha256Digest(role.canonicalInitializerDigest) &&
    isSha256Digest(role.adapterDescriptorDigest) &&
    isSha256Digest(role.assemblerDescriptorDigest) && !role.backendAbi.empty();
  const bool orderedUniqueNodes = std::adjacent_find(role.nodeIndices.begin(), role.nodeIndices.end(),
    [](std::uint64_t left, std::uint64_t right) { return left >= right; }) == role.nodeIndices.end();
  const bool completeAssemblyRecipe =
    !role.nodeIndices.empty() && orderedUniqueNodes &&
    !role.expectedInputs.empty() && !role.expectedOutputs.empty() &&
    !role.precision.empty() && !role.quantization.empty() &&
    !role.layout.empty() && !role.padding.empty() &&
    role.maxSourceBytes > 0 && role.maxAssembledBytes > 0 && role.maxNodes > 0;
  const bool componentSet = role.roleKind == "COMPONENT_SET";
  const bool hasPostprocessMetadata =
    !role.mergeKind.empty() || !role.postprocessIdentity.empty() ||
    !role.postprocessOutputName.empty() || decodedConfidence != 0.0 ||
    !role.postprocessSort.empty();
  const bool validNativePostprocess =
    role.mergeKind == "NATIVE_POSTPROCESS" && componentSet &&
    !role.postprocessIdentity.empty() && !role.postprocessOutputName.empty() &&
    role.postprocessSort == "confidence-desc,class-asc,xyxy-asc" &&
    std::isfinite(decodedConfidence) &&
    decodedConfidence >= 0.0 &&
    decodedConfidence <= 1.0;
  const bool validMergeKind = role.mergeKind.empty() ||
    role.mergeKind == "ONNX_MERGE_GRAPH" || validNativePostprocess;
  const bool validRoleCover = componentSet
    ? (role.layerBegin == 0 && role.layerEnd == 0 && !role.nodeIndices.empty())
    : role.layerEnd > role.layerBegin;
  if (role.role.empty() || role.selectedRole.empty() ||
      !validRoleCover || role.backend.empty() ||
      (isCpuBackend(role.backend) && !role.deviceSet.empty()) ||
      (!isCpuBackend(role.backend) && role.deviceSet.size() != 1) ||
      !isSha256Digest(role.artifactDigest) ||
      !isSha256Digest(role.recipeDigest) ||
      (role.roleKind != "PIPELINE_RANGE" &&
       role.roleKind != "TENSOR_RANK" &&
       role.roleKind != "HYBRID_RANK" &&
       role.roleKind != "COMPONENT_SET") ||
      !hasCompleteAdapterIdentity(role.adapterId, role.adapterVersion) ||
      role.protectionEpoch.empty() || !validMergeKind ||
      (!role.mergeKind.empty() && !validNativePostprocess &&
       role.mergeKind != "ONNX_MERGE_GRAPH") ||
      (role.mergeKind.empty() && hasPostprocessMetadata)) {
    throw std::invalid_argument(
      "V3 Selection projection contains an incomplete local role");
  }
  const bool completeNativeIdentity = isSha256Digest(role.modelManifestDigest) &&
    isSha256Digest(role.artifactProfileDigest) && isSha256Digest(role.graphDigest) &&
    isSha256Digest(role.adapterDescriptorDigest);
  if (validNativePostprocess &&
      (!isCpuBackend(role.backend) || role.expectedInputs.empty() ||
       role.expectedOutputs.size() != 1 ||
       role.expectedOutputs.front().name != role.postprocessOutputName ||
       role.expectedOutputs.front().dtype != "float32"))
    throw std::invalid_argument("V3 native postprocess role has invalid CPU or tensor contracts");
  if (hasAssemblyIdentity && (validNativePostprocess ? !completeNativeIdentity :
      (!completeAssemblyIdentity || !completeAssemblyRecipe))) {
    throw std::invalid_argument(
      "V3 Selection projection contains an incomplete assembly identity");
  }
}

NativeSelectionRoleV3
selectionRoleFromV3Json(const boost::property_tree::ptree& node,
                        const std::string& selectedRole)
{
  NativeSelectionRoleV3 role;
  role.role = node.get<std::string>("role", "");
  role.selectedRole = selectedRole;
  role.rank = node.get<std::uint64_t>("rank", 0);
  role.layerBegin = node.get<std::uint64_t>("layer_begin", 0);
  role.layerEnd = node.get<std::uint64_t>("layer_end", 0);
  role.backend = node.get<std::string>("backend", "");
  role.deviceSet = stringArrayFromJson(node, "device_set");
  role.requiredDeviceMemoryMb =
    node.get<std::uint64_t>("required_device_memory_mb", 0);
  role.artifactDigest = node.get<std::string>("artifact_digest", "");
  role.recipeDigest = node.get<std::string>("recipe_digest", "");
  role.roleKind = node.get<std::string>("role_kind", "");
  role.adapterId = node.get<std::string>("adapter_id", "");
  role.adapterVersion = node.get<std::string>("adapter_version", "");
  role.modelManifestDigest = node.get<std::string>("model_manifest_digest", "");
  role.artifactProfileDigest = node.get<std::string>("artifact_profile_digest", "");
  role.graphDigest = node.get<std::string>("graph_digest", "");
  role.canonicalInitializerDigest = node.get<std::string>(
    "canonical_initializer_digest", "");
  role.adapterDescriptorDigest = node.get<std::string>(
    "adapter_descriptor_digest", "");
  role.assemblerDescriptorDigest = node.get<std::string>(
    "assembler_descriptor_digest", "");
  role.backendAbi = node.get<std::string>("backend_abi", "");
  role.nodeIndices = uintArrayFromJson(node, "node_indices");
  role.expectedInputs = assemblyTensorContractsFromJson(node, "expected_inputs");
  role.expectedOutputs = assemblyTensorContractsFromJson(node, "expected_outputs");
  role.precision = node.get<std::string>("precision", "");
  role.quantization = node.get<std::string>("quantization", "");
  role.layout = node.get<std::string>("layout", "");
  role.padding = node.get<std::string>("padding", "");
  role.protectionEpoch = node.get<std::string>(
    "protection_epoch", "plaintext-v1");
  role.mergeKind = node.get<std::string>("merge_kind", "");
  role.postprocessIdentity = node.get<std::string>(
    "postprocess_identity", "");
  role.postprocessOutputName = node.get<std::string>(
    "postprocess_output_name", "");
  role.postprocessConfidenceThreshold = node.get<double>(
    "postprocess_confidence_threshold", 0.0);
  role.postprocessSort = node.get<std::string>("postprocess_sort", "");
  if (const auto envelope = node.get_child_optional("resource_envelope")) {
    role.maxSourceBytes = envelope->get<std::uint64_t>("maxSourceBytes", 0);
    role.maxAssembledBytes = envelope->get<std::uint64_t>("maxAssembledBytes", 0);
    role.maxNodes = envelope->get<std::uint64_t>("maxNodes", 0);
  }
  validateSelectionRoleFields(role, role.postprocessConfidenceThreshold);
  return role;
}

bool
sameAssembly(const NativeSelectionRoleV3& left,
             const NativeSelectionRoleV3& right, bool compareManifest = true, bool compareRecipe = true)
{
  return left.role == right.role && left.selectedRole == right.selectedRole &&
         left.rank == right.rank && left.layerBegin == right.layerBegin &&
         left.layerEnd == right.layerEnd && left.backend == right.backend &&
         left.deviceSet == right.deviceSet &&
         left.protectionEpoch == right.protectionEpoch &&
         left.requiredDeviceMemoryMb == right.requiredDeviceMemoryMb &&
         left.artifactDigest == right.artifactDigest &&
          (!compareRecipe || left.recipeDigest == right.recipeDigest) &&
         left.roleKind == right.roleKind && left.adapterId == right.adapterId &&
         left.adapterVersion == right.adapterVersion &&
          (!compareManifest || left.modelManifestDigest == right.modelManifestDigest) &&
         left.artifactProfileDigest == right.artifactProfileDigest &&
         left.graphDigest == right.graphDigest &&
         left.canonicalInitializerDigest == right.canonicalInitializerDigest &&
         left.adapterDescriptorDigest == right.adapterDescriptorDigest &&
         left.assemblerDescriptorDigest == right.assemblerDescriptorDigest &&
         left.backendAbi == right.backendAbi &&
         left.nodeIndices == right.nodeIndices &&
         sameAssemblyTensorContracts(left.expectedInputs, right.expectedInputs) &&
         sameAssemblyTensorContracts(left.expectedOutputs, right.expectedOutputs) &&
         left.precision == right.precision &&
         left.quantization == right.quantization && left.layout == right.layout &&
         left.padding == right.padding &&
         left.mergeKind == right.mergeKind &&
         left.postprocessIdentity == right.postprocessIdentity &&
          left.postprocessOutputName == right.postprocessOutputName &&
          std::isfinite(left.postprocessConfidenceThreshold) &&
          std::isfinite(right.postprocessConfidenceThreshold) &&
          left.postprocessConfidenceThreshold == right.postprocessConfidenceThreshold &&
          std::signbit(left.postprocessConfidenceThreshold) ==
            std::signbit(right.postprocessConfidenceThreshold) &&
         left.postprocessSort == right.postprocessSort &&
         left.maxSourceBytes == right.maxSourceBytes &&
         left.maxAssembledBytes == right.maxAssembledBytes &&
         left.maxNodes == right.maxNodes;
}

NativeExecutionRoleV3
executionRoleFromV3Json(const boost::property_tree::ptree& node)
{
  NativeExecutionRoleV3 role;
  role.roleId = node.get<std::string>("role_id", "");
  role.stageId = node.get<std::string>("stage_id", "");
  role.rank = node.get<std::uint64_t>("rank", 0);
  role.layerBegin = node.get<std::uint64_t>("layer_begin", 0);
  role.layerEnd = node.get<std::uint64_t>("layer_end", 0);
  role.backend = node.get<std::string>("backend", "");
  role.adapterId = node.get<std::string>("adapter_id", "");
  role.adapterVersion = node.get<std::string>("adapter_version", "");
  // COMPONENT_SET roles deliberately carry no layer interval.  The selected
  // role/assembly parser below validates the role kind and exact node cover;
  // this execution-role object must therefore accept the same canonical 0..0
  // representation as the Python V3 sealer.
  const bool componentSet = role.layerBegin == 0 && role.layerEnd == 0;
  if (role.roleId.empty() || role.stageId.empty() ||
      (!componentSet && role.layerEnd <= role.layerBegin) ||
      role.backend.empty() ||
      !hasCompleteAdapterIdentity(role.adapterId, role.adapterVersion)) {
    throw std::invalid_argument("invalid V3 execution role");
  }
  return role;
}

void validateNativeTensorEndpointV3(const NativeTensorEndpointV3& endpoint)
{
  if (endpoint.inlineFirstSegment && endpoint.sourceKind != "ROLE")
    throw std::invalid_argument("inline first segment requires a ROLE endpoint");
  if ((endpoint.prefetchBundleBytes == 0) != (endpoint.prefetchSegmentBytes == 0) ||
      endpoint.prefetchSegmentBytes > ndn::MAX_NDN_PACKET_SIZE ||
      (endpoint.prefetchBundleBytes != 0 &&
       (endpoint.sourceKind != "ROLE" ||
        1 + (endpoint.prefetchBundleBytes - 1) / endpoint.prefetchSegmentBytes >
          endpoint.segmentCount)))
    throw std::invalid_argument("invalid V3 tensor prefetch sizing");
  const std::set<std::string> bundleTensorNames(
    endpoint.bundleTensorNames.begin(), endpoint.bundleTensorNames.end());
  if (bundleTensorNames.size() != endpoint.bundleTensorNames.size() ||
      std::any_of(endpoint.bundleTensorNames.begin(), endpoint.bundleTensorNames.end(),
                  [] (const auto& name) { return name.empty(); })) {
    throw std::invalid_argument("invalid V3 bundle tensor names");
  }
  const bool roleSource = endpoint.sourceKind == "ROLE";
  const bool applicationInput = endpoint.sourceKind == "APPLICATION_INPUT";
  if (endpoint.namingOperationStride > (1U << 20) ||
      (applicationInput && (endpoint.namingOperationStride != 0 ||
        endpoint.round != 0 || endpoint.producerRank != 0 ||
        endpoint.operation != "APPLICATION_INPUT" ||
        ndn::Name(endpoint.producerNamespace) != ndn::Name(endpoint.requester))))
    throw std::invalid_argument("tensor naming stride or application input owner is invalid");
  if (endpoint.producerNamespace.empty() ||
      endpoint.producerNamespace.front() != '/' || endpoint.requester.empty() ||
      endpoint.requester.front() != '/' || endpoint.requestId.empty() ||
      endpoint.attempt == 0 || !isSha256Digest(endpoint.planDigest) ||
      endpoint.groupId.empty() || endpoint.groupEpoch.empty() ||
      endpoint.operation.empty() || (!roleSource && !applicationInput) ||
      (roleSource && endpoint.producerRole.empty()) ||
      (applicationInput && !endpoint.producerRole.empty()) ||
      endpoint.consumerRole.empty() || endpoint.consumerRoles.empty() ||
      std::find(endpoint.consumerRoles.begin(), endpoint.consumerRoles.end(),
                endpoint.consumerRole) == endpoint.consumerRoles.end() ||
      std::set<std::string>(endpoint.consumerRoles.begin(),
                            endpoint.consumerRoles.end()).size() !=
        endpoint.consumerRoles.size() || endpoint.tensorId.empty() ||
      !isSha256Digest(endpoint.tensorDigest) ||
      !isSha256Digest(endpoint.layoutDigest) ||
      !isSha256Digest(endpoint.targetLayoutDigest) ||
      endpoint.segmentCount == 0 ||
      !isSha256Digest(endpoint.manifestDigest) ||
      endpoint.securityProfile.empty() || endpoint.noProgressDeadlineMs == 0 ||
      endpoint.hardDeadlineMs == 0 ||
      !isSha256Digest(endpoint.endpointDigest)) {
    throw std::invalid_argument("invalid V3 tensor endpoint");
  }
}

NativeTensorEndpointV3
tensorEndpointFromV3Json(const boost::property_tree::ptree& node)
{
  NativeTensorEndpointV3 endpoint;
  endpoint.producerNamespace =
    node.get<std::string>("producer_namespace", "");
  endpoint.requester = node.get<std::string>("requester", "");
  endpoint.requestId = node.get<std::string>("request_id", "");
  endpoint.attempt = node.get<std::uint64_t>("attempt", 0);
  endpoint.planDigest = node.get<std::string>("plan_digest", "");
  endpoint.groupId = node.get<std::string>("group_id", "");
  endpoint.groupEpoch = node.get<std::string>("group_epoch", "");
  endpoint.operation = node.get<std::string>("operation", "");
  endpoint.round = node.get<std::uint64_t>("round", 0);
  endpoint.namingOperationStride = node.get<std::uint64_t>("naming_operation_stride");
  endpoint.sourceKind = node.get<std::string>("source_kind", "");
  endpoint.producerRole = node.get<std::string>("producer_role", "");
  endpoint.producerRank = node.get<std::uint64_t>("producer_rank", 0);
  endpoint.consumerRole = node.get<std::string>("consumer_role", "");
  endpoint.consumerRoles = stringArrayFromJson(node, "consumer_roles");
  if (endpoint.consumerRoles.empty() && !endpoint.consumerRole.empty()) {
    endpoint.consumerRoles = {endpoint.consumerRole};
  }
  endpoint.tensorId = node.get<std::string>("tensor_id", "");
  endpoint.bundleTensorNames = stringArrayFromJson(node, "bundle_tensor_names");
  endpoint.tensorDigest = node.get<std::string>("tensor_digest", "");
  endpoint.layoutDigest = node.get<std::string>("layout_digest", "");
  endpoint.targetLayoutDigest = node.get<std::string>(
    "target_layout_digest", endpoint.layoutDigest);
  endpoint.microbatch = node.get<std::uint64_t>("microbatch", 0);
  endpoint.segmentCount = node.get<std::size_t>("segment_count", 0);
  endpoint.prefetchBundleBytes = node.get<std::uint64_t>("prefetch_bundle_bytes", 0);
  endpoint.prefetchSegmentBytes = node.get<std::uint64_t>("prefetch_segment_bytes", 0);
  endpoint.inlineFirstSegment = node.get<bool>("inline_first_segment", false);
  endpoint.manifestDigest = node.get<std::string>("manifest_digest", "");
  endpoint.securityProfile = node.get<std::string>("security_profile", "");
  endpoint.noProgressDeadlineMs =
    node.get<std::uint64_t>("no_progress_deadline_ms", 0);
  endpoint.hardDeadlineMs =
    node.get<std::uint64_t>("hard_deadline_ms", 0);
  endpoint.endpointDigest = node.get<std::string>("endpoint_digest", "");
  validateNativeTensorEndpointV3(endpoint);
  return endpoint;
}

std::vector<NativeTensorEndpointV3>
tensorEndpointArrayFromV3Json(const boost::property_tree::ptree& node,
                              const std::string& key)
{
  std::vector<NativeTensorEndpointV3> endpoints;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return endpoints;
  }
  for (const auto& item : *child) {
    endpoints.push_back(tensorEndpointFromV3Json(item.second));
  }
  return endpoints;
}

void validateNativeReadinessPredicateV3(const NativeReadinessPredicateV3& readiness)
{
  const std::set<std::string> unique(
    readiness.endpointDigests.begin(), readiness.endpointDigests.end());
  if ((readiness.mode != "ALL" && readiness.mode != "ANY" &&
       readiness.mode != "QUORUM") || readiness.endpointDigests.empty() ||
      unique.size() != readiness.endpointDigests.size() ||
      std::any_of(readiness.endpointDigests.begin(),
                  readiness.endpointDigests.end(),
                  [] (const std::string& value) {
                    return !isSha256Digest(value);
                  }) ||
      (readiness.mode == "QUORUM" &&
       (readiness.quorum == 0 ||
        readiness.quorum > readiness.endpointDigests.size())) ||
      (readiness.mode != "QUORUM" && readiness.quorum != 0)) {
    throw std::invalid_argument("invalid V3 readiness predicate");
  }
}

NativeReadinessPredicateV3
readinessFromV3Json(const boost::property_tree::ptree& node)
{
  NativeReadinessPredicateV3 readiness;
  readiness.mode = node.get<std::string>("mode", "");
  readiness.endpointDigests = stringArrayFromJson(node, "endpoint_digests");
  readiness.quorum = node.get<std::size_t>("quorum", 0);
  validateNativeReadinessPredicateV3(readiness);
  return readiness;
}

void validateNativeRoleDataflowContractV3(const NativeRoleDataflowContractV3& dataflow)
{
  if (dataflow.requestId.empty() || dataflow.attempt == 0 ||
      !isSha256Digest(dataflow.planDigest) || dataflow.role.empty() ||
      !isSha256Digest(dataflow.dataflowDigest)) {
    throw std::invalid_argument("invalid V3 role dataflow contract");
  }

  std::set<std::string> endpointDigests;
  for (const auto& endpoint : dataflow.mayPublish) {
    validateNativeTensorEndpointV3(endpoint);
    if (!endpointDigests.insert(endpoint.endpointDigest).second ||
        endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.producerRole != dataflow.role) {
      throw std::invalid_argument("invalid V3 mayPublish endpoint binding");
    }
  }
  std::set<std::string> mustFetchDigests;
  for (const auto& endpoint : dataflow.mustFetch) {
    validateNativeTensorEndpointV3(endpoint);
    if (!endpointDigests.insert(endpoint.endpointDigest).second ||
        !mustFetchDigests.insert(endpoint.endpointDigest).second ||
        endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.consumerRole != dataflow.role) {
      throw std::invalid_argument("invalid V3 mustFetch endpoint binding");
    }
  }
  for (const auto& readiness : dataflow.waitFor) {
    validateNativeReadinessPredicateV3(readiness);
    for (const auto& endpointDigest : readiness.endpointDigests) {
      const auto endpoint = std::find_if(dataflow.mustFetch.begin(), dataflow.mustFetch.end(),
        [&](const auto& item) { return item.endpointDigest == endpointDigest; });
      if (endpoint != dataflow.mustFetch.end() && endpoint->operation == "TOKEN_FEEDBACK")
        throw std::invalid_argument("TOKEN_FEEDBACK cannot be an initial readiness dependency");
      if (mustFetchDigests.count(endpointDigest) == 0) {
        throw std::invalid_argument(
          "V3 waitFor references an undeclared mustFetch endpoint");
      }
    }
  }
}

void validateTensorNamingStrideContract(const NativeRoleDataflowContractV3& flow,
  const NativeGenerationExecutionContractV1& generation, bool boundEpochs = true)
{
  for (const auto* endpoints : {&flow.mayPublish, &flow.mustFetch}) {
    for (const auto& endpoint : *endpoints) {
      const bool role = endpoint.sourceKind == "ROLE";
      const auto expected = role && generation.enabled ? generation.streamingOperationStride : 0;
      if (endpoint.namingOperationStride != expected ||
          (role && generation.enabled && (!expected || expected > (1U << 20) ||
            (boundEpochs && endpoint.round / expected > generation.maxGeneratedTokens))))
        throw std::invalid_argument("tensor naming stride differs from its signed generation contract");
    }
  }
}

NativeRoleDataflowContractV3
dataflowFromV3Json(const boost::property_tree::ptree& node)
{
  NativeRoleDataflowContractV3 dataflow;
  dataflow.requestId = node.get<std::string>("request_id", "");
  dataflow.attempt = node.get<std::uint64_t>("attempt", 0);
  dataflow.planDigest = node.get<std::string>("plan_digest", "");
  dataflow.role = node.get<std::string>("role", "");
  dataflow.mayPublish = tensorEndpointArrayFromV3Json(node, "may_publish");
  dataflow.mustFetch = tensorEndpointArrayFromV3Json(node, "must_fetch");
  if (const auto waitFor = node.get_child_optional("wait_for")) {
    for (const auto& item : *waitFor) {
      dataflow.waitFor.push_back(readinessFromV3Json(item.second));
    }
  }
  dataflow.terminalResponseOwner =
    node.get<bool>("terminal_response_owner", false);
  dataflow.dataflowDigest = node.get<std::string>("dataflow_digest", "");
  validateNativeRoleDataflowContractV3(dataflow);
  return dataflow;
}

NativeDeviceBindingV3
deviceBindingFromV3Json(const boost::property_tree::ptree& node)
{
  NativeDeviceBindingV3 binding;
  binding.mode = node.get<std::string>("mode", "");
  binding.provider = node.get<std::string>("provider", "");
  binding.role = node.get<std::string>("role", "");
  binding.offerDigest = node.get<std::string>("offer_digest", "");
  binding.topologyProfileDigest =
    node.get<std::string>("topology_profile_digest", "");
  binding.resourceSnapshotDigest =
    node.get<std::string>("resource_snapshot_digest", "");
  binding.resourceSequence = node.get<std::uint64_t>("resource_sequence", 0);
  binding.offerScopedDeviceHandle =
    node.get<std::string>("offer_scoped_device_handle", "");
  binding.sharingPolicy = node.get<std::string>("sharing_policy", "");
  if ((binding.mode != "CPU" && binding.mode != "SINGLE_DEVICE") ||
      binding.provider.empty() || binding.role.empty() ||
      !isSha256Digest(binding.offerDigest) ||
      !isSha256Digest(binding.topologyProfileDigest) ||
      !isSha256Digest(binding.resourceSnapshotDigest) ||
      binding.resourceSequence == 0 || binding.sharingPolicy.empty() ||
      (binding.mode == "CPU" && !binding.offerScopedDeviceHandle.empty()) ||
      (binding.mode == "SINGLE_DEVICE" &&
       binding.offerScopedDeviceHandle.empty())) {
    throw std::invalid_argument("invalid V3 device binding");
  }
  return binding;
}

bool
sameTensorEndpoint(const NativeTensorEndpointV3& left,
                   const NativeTensorEndpointV3& right)
{
  return left.producerNamespace == right.producerNamespace &&
         left.requester == right.requester && left.requestId == right.requestId &&
         left.attempt == right.attempt && left.planDigest == right.planDigest &&
         left.groupId == right.groupId && left.groupEpoch == right.groupEpoch &&
         left.operation == right.operation && left.round == right.round &&
         left.sourceKind == right.sourceKind &&
         left.producerRole == right.producerRole &&
         left.producerRank == right.producerRank &&
         left.consumerRoles == right.consumerRoles &&
         left.tensorId == right.tensorId &&
         left.bundleTensorNames == right.bundleTensorNames &&
         left.tensorDigest == right.tensorDigest &&
         left.layoutDigest == right.layoutDigest &&
         left.targetLayoutDigest == right.targetLayoutDigest &&
         left.microbatch == right.microbatch &&
         left.segmentCount == right.segmentCount &&
         left.prefetchBundleBytes == right.prefetchBundleBytes &&
         left.prefetchSegmentBytes == right.prefetchSegmentBytes &&
         left.inlineFirstSegment == right.inlineFirstSegment &&
         left.manifestDigest == right.manifestDigest &&
         left.securityProfile == right.securityProfile &&
         left.noProgressDeadlineMs == right.noProgressDeadlineMs &&
         left.hardDeadlineMs == right.hardDeadlineMs &&
         left.endpointDigest == right.endpointDigest;
}

RedistributionSpec
redistributionFromV3Json(const boost::property_tree::ptree& node)
{
  RedistributionSpec spec;
  spec.producerRanks = uintArrayFromJson(node, "producerRanks");
  spec.consumerRanks = uintArrayFromJson(node, "consumerRanks");
  spec.tensor = node.get<std::string>("tensor", "");
  spec.operation = node.get<std::string>("operation", "");
  spec.epoch = node.get<std::string>("epoch", "");
  spec.integrityDigest = node.get<std::string>("integrityDigest", "");
  spec.sourceLayoutDigest = node.get<std::string>("sourceLayoutDigest", "");
  spec.targetLayoutDigest = node.get<std::string>("targetLayoutDigest", "");
  spec.axis = node.get<std::int64_t>("axis", -1);
  spec.temporaryMemoryBytes =
    node.get<std::size_t>("temporaryMemoryBytes", 0);
  spec.completeOutput = node.get<bool>("completeOutput", false);

  const std::set<std::uint64_t> producers(
    spec.producerRanks.begin(), spec.producerRanks.end());
  const std::set<std::uint64_t> consumers(
    spec.consumerRanks.begin(), spec.consumerRanks.end());
  std::vector<std::uint64_t> overlap;
  std::set_intersection(producers.begin(), producers.end(),
                        consumers.begin(), consumers.end(),
                        std::back_inserter(overlap));
  const bool duplicateRank = producers.size() != spec.producerRanks.size() ||
                             consumers.size() != spec.consumerRanks.size();
  const bool validOperationForRankShape =
    (spec.producerRanks.size() == 1 && spec.consumerRanks.size() > 1 &&
     spec.operation == "SCATTER") ||
    (spec.producerRanks.size() > 1 && spec.consumerRanks.size() == 1 &&
     spec.operation == "GATHER") ||
    (spec.producerRanks.size() > 1 && spec.consumerRanks.size() > 1 &&
     spec.operation == "RESHARD") ||
    (spec.producerRanks.size() == 1 && spec.consumerRanks.size() == 1 &&
     spec.sourceLayoutDigest != spec.targetLayoutDigest &&
     spec.operation == "RESHARD");
  if (spec.producerRanks.empty() || spec.consumerRanks.empty() ||
      duplicateRank || !overlap.empty() || spec.tensor.empty() ||
      (spec.operation != "GATHER" && spec.operation != "SCATTER" &&
       spec.operation != "RESHARD") || !validOperationForRankShape ||
      spec.epoch.empty() ||
      !isSha256Digest(spec.integrityDigest) ||
      !isSha256Digest(spec.sourceLayoutDigest) ||
      !isSha256Digest(spec.targetLayoutDigest) || spec.axis < -16 ||
      spec.axis >= 16 || !spec.completeOutput) {
    throw std::invalid_argument("invalid V3 redistribution edge");
  }
  return spec;
}

std::string
redistributionIdentity(const RedistributionSpec& spec)
{
  std::ostringstream key;
  key << spec.tensor << '\n' << spec.operation << '\n' << spec.epoch << '\n'
      << spec.integrityDigest << '\n' << spec.sourceLayoutDigest << '\n'
      << spec.targetLayoutDigest << '\n';
  for (const auto rank : spec.producerRanks) {
    key << 'p' << rank << ',';
  }
  key << '\n';
  for (const auto rank : spec.consumerRanks) {
    key << 'c' << rank << ',';
  }
  return key.str();
}

NativeDependencySpec
dependencyFromV3Json(const boost::property_tree::ptree& dep)
{
  NativeDependencySpec spec;
  spec.producers = stringArrayFromJson(dep, "producers");
  spec.consumers = stringArrayFromJson(dep, "consumers");
  spec.keyScope = dep.get<std::string>("key_scope", "");
  spec.topicPrefix = dep.get<std::string>("topic_prefix", "");
  spec.objectNameTemplate = dep.get<std::string>("object_name_template", "");
  spec.expectedSegments = dep.get<std::size_t>("expected_segments", 0);
  spec.expectedBytes = dep.get<std::size_t>("expected_bytes", 0);
  spec.tensors = stringArrayFromJson(dep, "tensors");
  spec.operationKind = dep.get<std::string>("operationKind",
                                             dep.get<std::string>("operation_kind", ""));
  spec.useNdnsfDataV1 =
    dep.get<std::string>("transportProfile", "COLLAB_LARGE_V1") ==
    "NDNSF_DATA_V1";
  spec.collectiveOperationIndex =
    dep.get<std::uint64_t>("collectiveOperationIndex", 0);
  spec.collectiveProducerRank =
    dep.get<std::string>("collectiveProducerRank", "");
  spec.collectiveSourceLayoutDigest =
    dep.get<std::string>("collectiveSourceLayoutDigest", "");
  spec.collectiveTargetLayoutDigest =
    dep.get<std::string>("collectiveTargetLayoutDigest", "");
  spec.collectiveTensorDigest =
    dep.get<std::string>("collectiveTensorDigest", "");
  if (const auto redistributions = dep.get_child_optional("redistributions")) {
    std::set<std::string> redistributionIdentities;
    for (const auto& item : *redistributions) {
      auto redistribution = redistributionFromV3Json(item.second);
      if (!redistributionIdentities.insert(
            redistributionIdentity(redistribution)).second) {
        throw std::invalid_argument(
          "V3 Selection dependency has duplicate redistribution");
      }
      spec.redistributions.push_back(std::move(redistribution));
    }
  }
  if (spec.producers.empty() || spec.consumers.empty() ||
      spec.keyScope.empty() || spec.topicPrefix.empty() ||
      spec.objectNameTemplate.empty()) {
    throw std::invalid_argument("V3 Selection dependency is incomplete");
  }
  if (!spec.redistributions.empty() && !spec.useNdnsfDataV1) {
    throw std::invalid_argument(
      "V3 redistribution requires NDNSF_DATA_V1 transport");
  }
  for (const auto& redistribution : spec.redistributions) {
    const bool tensorDeclared = std::find(
      spec.tensors.begin(), spec.tensors.end(), redistribution.tensor) !=
      spec.tensors.end();
    const bool sourceLayoutBound =
      spec.collectiveSourceLayoutDigest.empty() ||
      redistribution.sourceLayoutDigest == spec.collectiveSourceLayoutDigest;
    const bool targetLayoutBound =
      spec.collectiveTargetLayoutDigest.empty() ||
      redistribution.targetLayoutDigest == spec.collectiveTargetLayoutDigest;
    if (redistribution.producerRanks.size() != spec.producers.size() ||
        redistribution.consumerRanks.size() != spec.consumers.size() ||
        !tensorDeclared || !sourceLayoutBound || !targetLayoutBound) {
      throw std::invalid_argument(
        "V3 redistribution does not bind the dependency rank/layout contract");
    }
  }
  const bool degreeChanges = spec.producers.size() != spec.consumers.size();
  const bool layoutChanges =
    !spec.collectiveSourceLayoutDigest.empty() &&
    !spec.collectiveTargetLayoutDigest.empty() &&
    spec.collectiveSourceLayoutDigest != spec.collectiveTargetLayoutDigest;
  if ((degreeChanges || layoutChanges) && spec.redistributions.empty()) {
    throw std::invalid_argument(
      "V3 Selection dependency requires explicit redistribution");
  }
  return spec;
}

SegmentNamingSpec
segmentNamingFromJson(const boost::property_tree::ptree& dependency,
                      std::size_t expectedSegments)
{
  SegmentNamingSpec spec;
  spec.staticSegmentCount = expectedSegments;
  spec.dynamicFallback = expectedSegments == 0;

  const auto segmentNaming = dependency.get_child_optional("segmentNaming");
  if (!segmentNaming) {
    return spec;
  }

  spec.mode = segmentNaming->get<std::string>("mode", spec.mode);
  spec.staticSegmentCount =
    segmentNaming->get<std::size_t>("staticSegmentCount", spec.staticSegmentCount);
  spec.dynamicFallback =
    segmentNaming->get<bool>("dynamicFallback", spec.dynamicFallback);
  if (spec.staticSegmentCount == 0) {
    spec.dynamicFallback = true;
  }
  return spec;
}

} // namespace

void detail::validateTensorEndpointForNaming(const NativeTensorEndpointV3& endpoint)
{
  validateNativeTensorEndpointV3(endpoint);
  auto identity = jsonValue(endpoint);
  identity.erase("endpoint_digest");
  identity.erase("consumer_role");
  if (endpoint.endpointDigest != nativePlanningDigest(nativeCanonicalJson(identity)))
    throw std::invalid_argument("tensor endpoint binding digest mismatch");
}

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);
  const auto version = root.get<int>("version", 0);
  if (version != 1 && version != 2) {
    throw std::invalid_argument("unsupported native execution plan version");
  }

  std::map<std::string, NativeExecutionPlan> plans;
  const auto services = root.get_child_optional("services");
  if (!services) {
    throw std::invalid_argument("native execution plan missing services");
  }

  for (const auto& serviceNode : services.get()) {
    const auto& service = serviceNode.second;
    const auto serviceName = service.get<std::string>("service", "");
    if (serviceName.empty()) {
      throw std::invalid_argument("native execution plan service missing name");
    }

    NativeExecutionPlan plan;
    plan.version = version;
    plan.serviceName = serviceName;
    plan.modelName = service.get<std::string>("model", "");
    plan.modelFamily = service.get<std::string>("modelFamily", "generic-onnx");
    plan.modelFormat = service.get<std::string>("modelFormat", "unknown");
    plan.plannerKind = service.get<std::string>("plannerKind", "onnx-dag");
    plan.executionPolicy = service.get<std::string>(
      "executionPolicy", "DATA_DRIVEN_V2");
    plan.streamingOperationStride = service.get<std::uint64_t>(
      "streamingOperationStride", service.get<std::uint64_t>(
        "streaming_operation_stride", 0));
    if (plan.executionPolicy != "DATA_DRIVEN_V2" &&
        plan.executionPolicy != "LEGACY_READY_SET_V1") {
      throw std::invalid_argument(
        "unsupported native execution plan executionPolicy");
    }
    plan.roles = stringArrayFromJson(service, "roles");
    const auto dependencies = service.get_child_optional("dependencies");
    if (dependencies) {
      for (const auto& depNode : dependencies.get()) {
        const auto& dep = depNode.second;
        NativeDependencySpec spec;
        spec.producers = stringArrayFromJson(dep, "producers");
        spec.consumers = stringArrayFromJson(dep, "consumers");
        spec.keyScope = dep.get<std::string>("keyScope", "");
        spec.topicPrefix = dep.get<std::string>("topicPrefix", "");
        spec.objectNameTemplate = dep.get<std::string>("objectNameTemplate", "");
        spec.expectedSegments = dep.get<std::size_t>("expectedSegments", 0);
        spec.expectedBytes = dep.get<std::size_t>("expectedBytes", 0);
        spec.useNdnsfDataV1 =
          dep.get<std::string>("transportProfile", "COLLAB_LARGE_V1") ==
          "NDNSF_DATA_V1";
        spec.collectiveOperationIndex =
          dep.get<std::uint64_t>("collectiveOperationIndex", 0);
        spec.collectiveProducerRank =
          dep.get<std::string>("collectiveProducerRank", "");
        spec.collectiveSourceLayoutDigest =
          dep.get<std::string>("collectiveSourceLayoutDigest", "");
        spec.collectiveTargetLayoutDigest =
          dep.get<std::string>("collectiveTargetLayoutDigest", "");
        spec.collectiveTensorDigest =
          dep.get<std::string>("collectiveTensorDigest", "");
        spec.operationKind = dep.get<std::string>("operationKind",
                                                   dep.get<std::string>("operation_kind", ""));
        if (const auto redistributions = dep.get_child_optional("redistributions")) {
          for (const auto& item : *redistributions) {
            spec.redistributions.push_back(redistributionFromV3Json(item.second));
          }
        }
        if (!spec.redistributions.empty() && !spec.useNdnsfDataV1) {
          throw std::invalid_argument(
            "native execution plan redistribution requires NDNSF_DATA_V1");
        }
        spec.tensors = stringArrayFromJson(dep, "tensors");
        spec.segmentNaming = segmentNamingFromJson(dep, spec.expectedSegments);
        if (spec.keyScope.empty()) {
          throw std::invalid_argument(
            "native execution plan dependency missing keyScope");
        }
        plan.dependencies.push_back(std::move(spec));
      }
    }
    plans.emplace(serviceName, std::move(plan));
  }
  return plans;
}

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName)
{
  auto plans = nativeExecutionPlansByServiceFromJson(input);
  const auto found = plans.find(serviceName);
  if (found == plans.end()) {
    throw std::out_of_range("native execution plan has no service: " + serviceName);
  }
  return found->second;
}

namespace {
// Reproduce read_json's tree from an owned JSON value. Scalar text must use
// the same canonical spelling as the wire, including bool/null and floats.
void fillLegacyTreeFromValue(const NativeJson& value, boost::property_tree::ptree& root)
{
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      const auto child = root.push_back({it.key(), {}});
      fillLegacyTreeFromValue(it.value(), child->second);
    }
  }
  else if (value.is_array()) {
    for (const auto& item : value) {
      const auto child = root.push_back({"", {}});
      fillLegacyTreeFromValue(item, child->second);
    }
  }
  else if (value.is_string()) {
    root.data() = value.get<std::string>();
  }
  else {
    root.data() = nativeCanonicalJson(value);
  }
}

boost::property_tree::ptree legacyTreeFromValue(const NativeJson& value)
{
  boost::property_tree::ptree root;
  fillLegacyTreeFromValue(value, root);
  return root;
}

bool sameGeneratedJson(const NativeJson& left, const NativeJson& right)
{
  if (left.type() != right.type()) return false;
  if (left.is_object() || left.is_array()) {
    if (left.size() != right.size()) return false;
    auto l = left.begin();
    auto r = right.begin();
    for (; l != left.end(); ++l, ++r) {
      if (left.is_object() && l.key() != r.key()) return false;
      if (!sameGeneratedJson(l.value(), r.value())) return false;
    }
    return true;
  }
  if (left.is_number_float()) {
    const auto l = left.get<double>();
    const auto r = right.get<double>();
    return l == r && std::signbit(l) == std::signbit(r);
  }
  return left == right;
}

NativeSelectionRoleV3 checkedGeneratedRole(const NativeSelectionRoleV3& source,
                                         const NativeJson& node,
                                         const std::string& planRole,
                                          bool generatedHere)
{
  if (!generatedHere && !sameGeneratedJson(nativeAssemblyJson(source), node)) {
    throw std::invalid_argument("generated Selection role does not match its JSON value");
  }
  auto role = source;
  role.selectedRole = planRole;
  role.materializedRole = false;
  for (const auto& tensor : role.expectedInputs) validateAssemblyTensorContract(tensor);
  for (const auto& tensor : role.expectedOutputs) validateAssemblyTensorContract(tensor);
  role.postprocessConfidenceThreshold =
    legacyTreeFromValue(node.at("postprocess_confidence_threshold")).get_value<double>(0.0);
  validateSelectionRoleFields(role, role.postprocessConfidenceThreshold);
  return role;
}

void restoreSelectionRoleShapes(NativeSelectionRoleV3& role, const NativeJson& node)
{
    const auto restore = [&node] (auto& contracts, const char* key) {
      if (contracts.empty()) return;
      if (!node.contains(key) || !node.at(key).is_array() ||
          node.at(key).size() != contracts.size()) {
        throw std::invalid_argument("invalid typed assembly tensor contracts");
      }
      for (std::size_t i = 0; i < contracts.size(); ++i) {
        const auto& tensor = node.at(key).at(i);
        if (!tensor.contains("shape") || !tensor.at("shape").is_array()) {
          throw std::invalid_argument("invalid typed assembly tensor shape");
        }
        contracts[i].shape.clear();
        for (const auto& dimension : tensor.at("shape")) {
          if (dimension.is_string()) contracts[i].shape.emplace_back(dimension.get<std::string>());
          else if (dimension.is_number_integer() &&
                   (!dimension.is_number_unsigned() ||
                    dimension.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX))) {
            contracts[i].shape.emplace_back(dimension.get<std::int64_t>());
          }
          else throw std::invalid_argument("assembly dimension must be int64 or string");
        }
      }
    };
    restore(role.expectedInputs, "expected_inputs");
    restore(role.expectedOutputs, "expected_outputs");
}

NativeSelectionProjectionV3 decodeSelectionProjectionV3(
  const NativeJson& typedRoot, const boost::property_tree::ptree& root,
  const std::string& selectedRole,
  const NativeSelectionProjectionV3* generated = nullptr,
  bool generatedHere = false,
  const NativeFixedRoleAssignmentV1* fixedAssignment = nullptr)
{
  if (fixedAssignment) {
    validateNativeSelectionInvocationBindingShapeV1(typedRoot);
    if (!typedRoot.contains("fixed_role_ref") ||
        !typedRoot.at("fixed_role_ref").is_string() ||
        typedRoot.at("fixed_role_ref").get<std::string>() != fixedAssignment->digest() ||
        !typedRoot.contains("schema_version") ||
        !typedRoot.at("schema_version").is_number_integer() ||
        typedRoot.at("schema_version") != NativeJson(1) ||
        typedRoot.contains("roles") || typedRoot.contains("assembly") ||
        typedRoot.contains("assembly_same_as_role")) {
      throw std::invalid_argument("invalid cached role invocation reference");
    }
  }
  else if (typedRoot.contains("fixed_role_ref")) {
    throw std::invalid_argument("cached role invocation requires its validated owner");
  }
  if (typedRoot.contains("assembly_same_as_role")) {
    const auto& marker = typedRoot.at("assembly_same_as_role");
    if (!marker.is_boolean() || !marker.get<bool>() || typedRoot.contains("assembly") ||
        !typedRoot.contains("roles") || !typedRoot.at("roles").is_array() ||
        typedRoot.at("roles").size() != 1)
      throw std::invalid_argument("invalid compact Selection assembly alias");
    auto expanded = typedRoot;
    expanded.erase("assembly_same_as_role");
    expanded["assembly"] = expanded.at("roles").at(0);
    auto expandedTree = root;
    expandedTree.erase("assembly_same_as_role");
    const auto roles = expandedTree.get_child_optional("roles");
    if (!roles || roles->size() != 1)
      throw std::invalid_argument("compact Selection has no unique legacy role");
    expandedTree.put_child("assembly", roles->front().second);
    auto projection = decodeSelectionProjectionV3(expanded, expandedTree,
      selectedRole, generated, generatedHere);
    projection.compactAssembly = true;
    return projection;
  }
  if (root.get<std::string>("schema", "") !=
        (fixedAssignment ? "ndnsf-di-selection-invocation-v1" : "ndnsf-di-selection-v3") ||
      root.get<int>("schema_version", 0) != (fixedAssignment ? 1 : 3)) {
    throw std::invalid_argument("V3 Selection projection schema mismatch");
  }

  NativeSelectionProjectionV3 projection;
  projection.provider = root.get<std::string>("provider", "");
  if (fixedAssignment && projection.provider != fixedAssignment->provider()) {
    throw std::invalid_argument("cached role invocation Provider mismatch");
  }
  projection.requestId = root.get<std::string>("request_id", "");
  projection.attempt = root.get<std::uint64_t>("attempt", 0);
  projection.planCoreDigest = root.get<std::string>("plan_core_digest", "");
  projection.planDigest = root.get<std::string>("plan_digest", "");
  projection.ackClosedDigest =
    root.get<std::string>("ack_closed_digest", "");
  projection.offerDigest = root.get<std::string>("offer_digest", "");
  projection.securityPolicySnapshotDigest =
    root.get<std::string>("security_policy_snapshot_digest", "");
  projection.requestContractDigest =
    root.get<std::string>("request_contract_digest", "");
  projection.deadlineMs = root.get<std::uint64_t>("deadline_ms", 0);
  projection.groupCapabilityV1 =
    root.get<std::string>("group_capability_v1", "");
  if (projection.provider.empty() || projection.requestId.empty() ||
      projection.attempt == 0 || projection.deadlineMs == 0 ||
      !isSha256Digest(projection.planCoreDigest) ||
      !isSha256Digest(projection.planDigest) ||
      !isSha256Digest(projection.ackClosedDigest) ||
      !isSha256Digest(projection.offerDigest) ||
      !isSha256Digest(projection.securityPolicySnapshotDigest) ||
      (!projection.requestContractDigest.empty() &&
       !isSha256Digest(projection.requestContractDigest))) {
    throw std::invalid_argument("V3 Selection projection binding is incomplete");
  }

  projection.plan.version = 3;
  projection.plan.executionPolicy = "DATA_DRIVEN_V2";
  std::set<std::string> roleSet;
  const auto appendRole = [&] (const std::string& role) {
    if (!role.empty() && roleSet.insert(role).second) {
      projection.plan.roles.push_back(role);
    }
  };

  if (fixedAssignment) {
    projection.plan.dependencies = bindFixedTemplateDependencies(
      typedRoot.at("dependencies"), *fixedAssignment->executionTemplate());
    for (const auto& dependency : projection.plan.dependencies) {
      for (const auto& role : dependency.producers) appendRole(role);
      for (const auto& role : dependency.consumers) appendRole(role);
    }
  }
  else if (const auto dependencies = root.get_child_optional("dependencies")) {
    for (const auto& item : *dependencies) {
      auto dependency = dependencyFromV3Json(item.second);
      for (const auto& role : dependency.producers) {
        appendRole(role);
      }
      for (const auto& role : dependency.consumers) {
        appendRole(role);
      }
      projection.plan.dependencies.push_back(std::move(dependency));
    }
  }

  if (const auto generation = root.get_child_optional("generation_contract")) {
    auto& contract = projection.generationContract;
    if (fixedAssignment) {
      if (!fixedAssignment->executionTemplate()->generation.enabled)
        throw std::invalid_argument("invocation cannot enable generation outside its fixed template");
      requireOnlyBindingKeys(typedRoot.at("generation_contract"),
        {"max_generated_tokens", "sampling_digest", "sampling_mode", "sampling_temperature", "sampling_top_k",
         "sampling_top_p", "sampling_repetition_penalty", "sampling_seed", "stop_strings", "generation_id",
         "committed_prefix_token_ids"});
      contract = fixedAssignment->executionTemplate()->generation;
    }
    contract.enabled = true;
    contract.maxGeneratedTokens = generation->get<std::size_t>(
      "max_generated_tokens", 0);
    if (!fixedAssignment) {
      contract.mode = generation->get<std::string>("mode", "");
      contract.tokenInputName = generation->get<std::string>(
        "token_input_name", "");
      contract.stateInputNames = stringArrayFromJson(
        *generation, "state_input_names");
      contract.stateOutputNames = stringArrayFromJson(
        *generation, "state_output_names");
      contract.stateSuccessorMap = generation->get<std::string>(
        "state_successor_map", generation->get<std::string>("kv_tensor_map", ""));
      contract.positionInputPolicy = generation->get<std::string>(
        "position_input_policy", "");
      contract.attentionMaskInputName = generation->get<std::string>(
        "attention_mask_input_name", "");
      contract.positionIdsInputName = generation->get<std::string>(
        "position_ids_input_name", "");
      contract.cachePositionInputName = generation->get<std::string>(
        "cache_position_input_name", "");
      contract.eosTokenIds = int64ArrayFromJson(*generation, "eos_token_ids");
      contract.tokenizerDigest = generation->get<std::string>("tokenizer_digest", "");
      contract.streamingOperationStride = generation->get<std::uint64_t>("streaming_operation_stride", 0);
    }
    contract.samplingDigest = generation->get<std::string>(
      "sampling_digest", "");
    contract.samplingMode = generation->get<std::string>(
      "sampling_mode", "Greedy");
    contract.samplingTemperature = generation->get<double>(
      "sampling_temperature", 0.0);
    contract.samplingTopK = generation->get<std::uint64_t>(
      "sampling_top_k", 1);
    contract.samplingTopP = generation->get<double>(
      "sampling_top_p", 1.0);
    contract.samplingRepetitionPenalty = generation->get<double>(
      "sampling_repetition_penalty", 1.0);
    contract.samplingSeed = generation->get<std::uint64_t>(
      "sampling_seed", 1'750'001);
    contract.stopStrings = stringArrayFromJson(*generation, "stop_strings");
    contract.generationId = generation->get<std::string>(
      "generation_id", "");
    contract.committedPrefixTokenIds = int64ArrayFromJson(
      *generation, "committed_prefix_token_ids");
    const std::set<std::string> uniqueInputs(
      contract.stateInputNames.begin(), contract.stateInputNames.end());
    const std::set<std::string> uniqueOutputs(
      contract.stateOutputNames.begin(), contract.stateOutputNames.end());
    if (!validStateSuccessorMap(contract.stateSuccessorMap,
                                contract.stateInputNames,
                                contract.stateOutputNames)) {
      throw std::invalid_argument(
        "V3 Selection generation successor map is incomplete");
    }
    const auto feedbackCount = std::count_if(
      projection.plan.dependencies.begin(), projection.plan.dependencies.end(),
      [] (const auto& dependency) {
        return dependency.operationKind == "TOKEN_FEEDBACK";
      });
    if (projection.plan.roles.size() != 1 && std::any_of(
          projection.plan.dependencies.begin(), projection.plan.dependencies.end(),
          [](const auto& dependency) {
            return dependency.operationKind == "TOKEN_FEEDBACK" &&
              dependency.producers.size() == 1 && dependency.producers == dependency.consumers;
          }))
      throw std::invalid_argument("self feedback requires the authenticated single-role local plan");
    const bool validSamplingMode =
      contract.samplingMode == "Greedy" ||
      contract.samplingMode == "SeededTopKTopP";
    const bool validSampling = validSamplingMode &&
      std::isfinite(contract.samplingTemperature) &&
      std::isfinite(contract.samplingTopP) &&
      std::isfinite(contract.samplingRepetitionPenalty) &&
      contract.samplingTopK > 0 &&
      contract.samplingTopP > 0.0 && contract.samplingTopP <= 1.0 &&
      contract.samplingRepetitionPenalty >= 0.1 &&
      contract.samplingRepetitionPenalty <= 2.0 &&
      ((contract.samplingMode == "Greedy" &&
        contract.samplingTemperature == 0.0) ||
       (contract.samplingMode == "SeededTopKTopP" &&
        contract.samplingTemperature > 0.0 &&
        contract.samplingTemperature <= 5.0));
    const bool validStops = contract.stopStrings.size() <= 16 &&
      std::all_of(contract.stopStrings.begin(), contract.stopStrings.end(),
                  [] (const auto& value) {
                    return !value.empty() && value.size() <= 256;
                  });
    if (contract.mode != "TOKEN_STREAMING" ||
        contract.maxGeneratedTokens == 0 || contract.maxGeneratedTokens > MAX_NATIVE_GENERATED_TOKENS ||
        contract.tokenInputName.empty() || contract.stateInputNames.empty() ||
        contract.stateInputNames.size() != contract.stateOutputNames.size() ||
        uniqueInputs.size() != contract.stateInputNames.size() ||
        uniqueOutputs.size() != contract.stateOutputNames.size() ||
        contract.eosTokenIds.empty() ||
        std::any_of(contract.eosTokenIds.begin(), contract.eosTokenIds.end(),
                    [] (std::int64_t value) { return value < 0; }) ||
        !isSha256Digest(contract.samplingDigest) ||
        !isSha256Digest(contract.tokenizerDigest) ||
        !validSampling || !validStops ||
        (!contract.generationId.empty() &&
         !isGenerationId(contract.generationId)) ||
        contract.streamingOperationStride < projection.plan.dependencies.size() ||
        contract.streamingOperationStride > (1U << 20) ||
        contract.committedPrefixTokenIds.size() >= contract.maxGeneratedTokens ||
        std::any_of(
          contract.committedPrefixTokenIds.begin(),
          contract.committedPrefixTokenIds.end(),
          [&contract] (std::int64_t value) {
            return value < 0 || std::find(
              contract.eosTokenIds.begin(), contract.eosTokenIds.end(), value) !=
              contract.eosTokenIds.end();
          }) ||
        feedbackCount != 1) {
      throw std::invalid_argument(
        "V3 Selection generation contract is incomplete");
    }
    projection.plan.streamingOperationStride =
      contract.streamingOperationStride;
  }
  else if (std::any_of(
             projection.plan.dependencies.begin(),
             projection.plan.dependencies.end(),
             [] (const auto& dependency) {
               return dependency.operationKind == "TOKEN_FEEDBACK";
             })) {
    throw std::invalid_argument(
      "V3 Selection TOKEN_FEEDBACK requires a generation contract");
  }

  const auto roles = root.get_child_optional("roles");
  if (fixedAssignment && fixedAssignment->executionTemplate()->generation.enabled != projection.generationContract.enabled)
    throw std::invalid_argument("invocation generation mode differs from its fixed template");
  if (!fixedAssignment && (!roles || roles->size() != 1)) {
    throw std::invalid_argument(
      "V3 Selection projection must contain exactly one local role");
  }
  boost::property_tree::ptree fixedRoleNode;
  if (fixedAssignment) {
    fixedRoleNode.put("role", fixedAssignment->roleOwner()->role);
    fixedRoleNode.put("rank", fixedAssignment->roleOwner()->rank);
  }
  const auto& roleNode = fixedAssignment ? fixedRoleNode : roles->front().second;
  const auto logicalRole = roleNode.get<std::string>("role", "");
  const auto rank = roleNode.get<std::uint64_t>("rank", 0);
  const auto rankedRole = logicalRole + "#" + std::to_string(rank);
  const auto planRole = roleSet.count(logicalRole) != 0
    ? logicalRole
    : roleSet.count(rankedRole) != 0
      ? rankedRole
      : rank == 0 ? logicalRole : rankedRole;
  if (!selectedRole.empty() && selectedRole != logicalRole &&
      selectedRole != planRole) {
    throw std::invalid_argument(
      "V3 Selection projection does not contain the requested role");
  }
  if (fixedAssignment && fixedAssignment->roleOwner()->selectedRole != planRole) {
    throw std::invalid_argument("cached role invocation role identity mismatch");
  }
  if (fixedAssignment) {
    projection.fixedRoleOwner = fixedAssignment->roleOwner();
    projection.fixedExecutionTemplateOwner = fixedAssignment->executionTemplate();
    projection.fixedRoleReference = fixedAssignment->digest();
  }
  else {
    projection.selectedRole = generated
      ? checkedGeneratedRole(generated->selectedRoleContract(), typedRoot.at("roles").at(0), planRole, generatedHere)
      : selectionRoleFromV3Json(roleNode, planRole);
  }
  appendRole(planRole);

  const auto executionRole = root.get_child_optional("execution_role");
  const auto assembly = root.get_child_optional("assembly");
  const auto dataflow = root.get_child_optional("dataflow");
  const auto deviceBinding = root.get_child_optional("device_binding");
  if (!executionRole || (!fixedAssignment && !assembly) || !dataflow || !deviceBinding) {
    throw std::invalid_argument(
      "V3 Selection projection is missing its complete role contract");
  }
  // PropertyTree loses scalar JSON types. Validate optional prefetch numbers
  // from the already parsed typed root before translating the endpoint.
  const auto& typedDataflow = typedRoot.at("dataflow");
  for (const auto* key : {"may_publish", "must_fetch"}) {
    if (!typedDataflow.contains(key)) continue;
    for (const auto& endpoint : typedDataflow.at(key)) {
      if (!fixedAssignment && (!endpoint.contains("naming_operation_stride") ||
          !endpoint.at("naming_operation_stride").is_number_unsigned()))
        throw std::invalid_argument("missing or invalid signed tensor naming stride");
      if (endpoint.contains("inline_first_segment") &&
          !endpoint.at("inline_first_segment").is_boolean())
        throw std::invalid_argument("invalid typed inline first segment flag");
      const bool bytes = endpoint.contains("prefetch_bundle_bytes");
      const bool chunk = endpoint.contains("prefetch_segment_bytes");
      if (bytes != chunk || (bytes &&
          (!endpoint.at("prefetch_bundle_bytes").is_number_unsigned() ||
           !endpoint.at("prefetch_segment_bytes").is_number_unsigned() ||
           endpoint.at("prefetch_bundle_bytes").get<std::uint64_t>() == 0 ||
           endpoint.at("prefetch_segment_bytes").get<std::uint64_t>() == 0)))
        throw std::invalid_argument("invalid typed V3 tensor prefetch sizing");
    }
  }
  if (fixedAssignment) {
    const auto& routingRole = typedRoot.at("execution_role");
    if (!routingRole.is_object() || routingRole.size() != 1 ||
        !routingRole.contains("role_id") || !routingRole.at("role_id").is_string() ||
        routingRole.at("role_id").get<std::string>() != fixedAssignment->executionRole().roleId)
      throw std::invalid_argument("cached execution role routing identity mismatch");
    projection.executionRole = fixedAssignment->executionRole();
  }
  else {
    projection.executionRole = executionRoleFromV3Json(*executionRole);
  }
  // Reuse only an exactly equal parsed tree. Restore each side's typed shapes
  // independently below: PropertyTree equality alone loses scalar JSON types.
  if (!fixedAssignment) {
    projection.assembly = generated
      ? checkedGeneratedRole(generated->assemblyContract(), typedRoot.at("assembly"), planRole, generatedHere)
      : roleNode == *assembly
        ? projection.selectedRole
        : selectionRoleFromV3Json(*assembly, planRole);
  }
  // PropertyTree is retained for existing semantic checks, but cannot retain
  // scalar JSON types. Restore typed dimensions before same-assembly checks.

  // Generated roles already own typed dimensions copied after exact JSON
  // equality checks. Only PropertyTree-decoded roles need type restoration.
  if (!generated && !fixedAssignment) {
    restoreSelectionRoleShapes(projection.selectedRole, typedRoot.at("roles").at(0));
    restoreSelectionRoleShapes(projection.assembly, typedRoot.at("assembly"));
  }
  projection.dataflow = fixedAssignment
    ? bindFixedTemplateDataflow(typedDataflow, projection, *fixedAssignment->executionTemplate())
    : dataflowFromV3Json(*dataflow);
  validateTensorNamingStrideContract(projection.dataflow, projection.generationContract);
  projection.deviceBinding = deviceBindingFromV3Json(*deviceBinding);

  if (const auto stateReference =
        root.get_child_optional("conversation_state_reference")) {
    ConversationStateReferenceV1 reference;
    reference.conversationId = stateReference->get<std::string>(
      "conversation_id", "");
    reference.contextEpoch = stateReference->get<std::uint64_t>(
      "context_epoch", 0);
    reference.serviceName = stateReference->get<std::string>(
      "service_name", "");
    reference.planRoleMapDigest = stateReference->get<std::string>(
      "plan_role_map_digest", "");
    reference.checkpointDigest = stateReference->get<std::string>(
      "checkpoint_digest", "");
    reference.roleName = stateReference->get<std::string>(
      "role_name", "");
    reference.roleReceiptDigest = stateReference->get<std::string>(
      "role_receipt_digest", "");
    reference.expiresAtMs = stateReference->get<std::uint64_t>(
      "expires_at_ms", 0);
    if (stateReference->get<std::string>("schema", "") !=
          "ndnsf-di-conversation-state-reference-v1" ||
        stateReference->get<int>("version", 0) != 1) {
      throw std::invalid_argument(
        "V3 Selection conversation state reference schema mismatch");
    }
    reference.validate();
    projection.conversationStateReference = std::move(reference);
  }

  if (const auto turnBinding =
        root.get_child_optional("conversation_turn_binding")) {
    if (turnBinding->get<std::string>("schema", "") !=
          "ndnsf-di-conversation-turn-binding-v1" ||
        turnBinding->get<int>("version", 0) != 1) {
      throw std::invalid_argument(
        "V3 Selection conversation turn binding schema mismatch");
    }
    ConversationTurnBindingV1 binding;
    binding.conversationId = turnBinding->get<std::string>(
      "conversation_id", "");
    binding.parentContextEpoch = turnBinding->get<std::uint64_t>(
      "parent_context_epoch", 0);
    binding.successorContextEpoch = turnBinding->get<std::uint64_t>(
      "successor_context_epoch", 0);
    binding.serviceName = turnBinding->get<std::string>(
      "service_name", "");
    binding.planRoleMapDigest = turnBinding->get<std::string>(
      "plan_role_map_digest", "");
    binding.requestContractDigest = turnBinding->get<std::string>(
      "request_contract_digest", "");
    binding.retentionDeadlineMs = turnBinding->get<std::uint64_t>(
      "retention_deadline_ms", 0);
    binding.parentCheckpointDigest = turnBinding->get<std::string>(
      "parent_checkpoint_digest", "");
    binding.validate();
    if (projection.requestContractDigest.empty() ||
        binding.requestContractDigest != projection.requestContractDigest) {
      throw std::invalid_argument(
        "V3 Selection conversation/request contract mismatch");
    }
    projection.conversationTurnBinding = std::move(binding);
  }
  if (projection.conversationStateReference) {
    if (!projection.conversationTurnBinding ||
        projection.conversationStateReference->conversationId !=
          projection.conversationTurnBinding->conversationId ||
        projection.conversationStateReference->contextEpoch !=
          projection.conversationTurnBinding->parentContextEpoch ||
        projection.conversationStateReference->serviceName !=
          projection.conversationTurnBinding->serviceName ||
        projection.conversationStateReference->planRoleMapDigest !=
          projection.conversationTurnBinding->planRoleMapDigest ||
        projection.conversationStateReference->checkpointDigest !=
          projection.conversationTurnBinding->parentCheckpointDigest) {
      throw std::invalid_argument(
        "V3 Selection conversation parent/turn binding mismatch");
    }
  }
  if (projection.conversationTurnBinding &&
      projection.conversationTurnBinding->parentContextEpoch > 0 &&
      !projection.conversationStateReference) {
    throw std::invalid_argument(
      "V3 Selection append turn is missing its role-local parent state");
  }

  const auto grantBinding = root.get_child_optional("grant_binding");
  const bool isProtected =
    projection.selectedRoleContract().protectionEpoch != "plaintext-v1";
  if (isProtected && !grantBinding) {
    throw std::invalid_argument(
      "protected V3 Selection requires its Provider grant binding");
  }
  if (!isProtected && grantBinding) {
    throw std::invalid_argument(
      "plaintext V3 Selection must not carry a grant binding");
  }
  if (grantBinding) {
    projection.hasGrantBinding = true;
    projection.grantName = grantBinding->get<std::string>("grant_name", "");
    projection.grantDigest = grantBinding->get<std::string>("grant_digest", "");
    const auto grantProvider =
      grantBinding->get<std::string>("provider", "");
    const auto grantRequestId =
      grantBinding->get<std::string>("request_id", "");
    const auto grantAttempt =
      grantBinding->get<std::uint64_t>("attempt", 0);
    const auto grantPlanCoreDigest =
      grantBinding->get<std::string>("plan_core_digest", "");
    const auto grantPolicyDigest = grantBinding->get<std::string>(
      "security_policy_snapshot_digest", "");
    const auto grantProtectionEpoch =
      grantBinding->get<std::string>("protection_epoch", "");
    if (const auto lease = grantBinding->get_child_optional("lease_scope")) {
      NativeGrantLeaseScope scope;
      scope.conversationId = lease->get<std::string>("conversation_id", "");
      scope.requesterIdentity = lease->get<std::string>("requester_identity", "");
      scope.serviceName = lease->get<std::string>("service_name", "");
      scope.requestId = lease->get<std::string>("request_id", "");
      scope.attempt = lease->get<std::uint64_t>("attempt", 0);
      scope.planCoreDigest = lease->get<std::string>("plan_core_digest", "");
      scope.scopeDigest = lease->get<std::string>("scope_digest", "");
      scope.securityPolicySnapshotDigest = lease->get<std::string>(
        "security_policy_snapshot_digest", "");
      scope.protectionEpoch = lease->get<std::string>("protection_epoch", "");
      scope.expiresAtMs = lease->get<std::uint64_t>("expires_at_ms", 0);
      if (lease->get<std::string>("schema", "") !=
            "ndnsf-di-grant-lease-scope-v1" || lease->get<int>("version", 0) != 1) {
        throw std::invalid_argument("V3 Selection grant lease scope schema mismatch");
      }
      if (const auto providers = lease->get_child_optional("provider_by_role")) {
        for (const auto& item : *providers)
          scope.providerByRole.emplace(item.first, item.second.get_value<std::string>());
      }
      const auto assigned = projection.selectedRoleContract().selectedRole.empty()
        ? projection.selectedRoleContract().role : projection.selectedRoleContract().selectedRole;
      if (scope.conversationId.empty() || scope.requesterIdentity.empty() ||
          scope.serviceName.empty() || scope.requestId.empty() || scope.attempt == 0 ||
          !isSha256Digest(scope.planCoreDigest) || !isSha256Digest(scope.scopeDigest) ||
          !isSha256Digest(scope.securityPolicySnapshotDigest) || scope.protectionEpoch.empty() ||
          scope.expiresAtMs < projection.deadlineMs ||
          scope.providerByRole.find(assigned) == scope.providerByRole.end() ||
          scope.providerByRole.at(assigned) != projection.provider) {
        throw std::invalid_argument("V3 Selection grant lease scope is incomplete or misbound");
      }
      projection.grantLeaseScope = std::move(scope);
    }
    if (projection.grantName.empty() || projection.grantName.front() != '/' ||
        !isSha256Digest(projection.grantDigest) ||
        grantProvider != projection.provider ||
        grantRequestId != projection.requestId ||
        grantAttempt != projection.attempt ||
        grantPlanCoreDigest != projection.planCoreDigest ||
        grantPolicyDigest != projection.securityPolicySnapshotDigest ||
        grantProtectionEpoch != projection.selectedRoleContract().protectionEpoch) {
      throw std::invalid_argument(
        "V3 Selection Provider grant binding mismatch");
    }
  }

  if (!sameAssembly(projection.selectedRoleContract(), projection.assemblyContract()) ||
      projection.executionRole.roleId != planRole ||
      projection.executionRole.stageId != projection.selectedRoleContract().role ||
      projection.executionRole.rank != projection.selectedRoleContract().rank ||
      projection.executionRole.layerBegin != projection.selectedRoleContract().layerBegin ||
      projection.executionRole.layerEnd != projection.selectedRoleContract().layerEnd ||
      projection.executionRole.backend != projection.selectedRoleContract().backend ||
      projection.executionRole.adapterId != projection.selectedRoleContract().adapterId ||
      projection.executionRole.adapterVersion !=
        projection.selectedRoleContract().adapterVersion ||
      projection.dataflow.requestId != projection.requestId ||
      projection.dataflow.attempt != projection.attempt ||
      projection.dataflow.planDigest != projection.planDigest ||
      projection.dataflow.role != planRole ||
      projection.deviceBinding.provider != projection.provider ||
      projection.deviceBinding.role != planRole ||
      projection.deviceBinding.offerDigest != projection.offerDigest ||
      (projection.conversationStateReference &&
       projection.conversationStateReference->roleName != planRole) ||
      (isCpuBackend(projection.selectedRoleContract().backend) &&
       projection.deviceBinding.mode != "CPU") ||
      (!isCpuBackend(projection.selectedRoleContract().backend) &&
       (projection.deviceBinding.mode != "SINGLE_DEVICE" ||
        projection.selectedRoleContract().deviceSet.front() !=
          projection.deviceBinding.offerScopedDeviceHandle))) {
    throw std::invalid_argument(
      "V3 Selection role/assembly/dataflow/device binding mismatch");
  }
  return projection;
}

} // namespace

NativeSelectionProjectionV3
nativeSelectionProjectionV3FromJson(std::istream& input,
                                    const std::string& selectedRole)
{
  const std::string wire{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto typedRoot = nativeParseJson(wire);
  std::istringstream legacyInput(wire);
  boost::property_tree::ptree root;
  boost::property_tree::read_json(legacyInput, root);
  return decodeSelectionProjectionV3(typedRoot, root, selectedRole);
}

NativeSelectionProjectionV3 detail::decodeNativeSelectionProjectionV3WithLegacyTree(
  const std::string& wire, const boost::property_tree::ptree& root,
  const std::string& selectedRole)
{
  const auto typedRoot = nativeParseJson(wire);
  return decodeSelectionProjectionV3(typedRoot, root, selectedRole);
}

namespace {
// Only the local serializer constructs a root and validates it with generatedHere.
// External value validation always retains the exact typed JSON comparison.
void validateGeneratedSelectionValue(
  const NativeJson& value, const NativeSelectionProjectionV3& generated, bool generatedHere)
{
  if (std::locale() != std::locale::classic()) {
    decodeSelectionProjectionV3(value, legacyTreeFromValue(value), generated.executionRole.roleId);
    return;
  }
  boost::property_tree::ptree root;
  for (auto it = value.begin(); it != value.end(); ++it) {
    const auto child = root.push_back({it.key(), {}});
    if (it.key() == "roles" && it.value().is_array()) {
      for (const auto& role : it.value()) {
        const auto stub = child->second.push_back({"", {}});
        for (const auto* field : {"role", "rank"}) {
          if (role.contains(field)) {
            const auto entry = stub->second.push_back({field, {}});
            fillLegacyTreeFromValue(role.at(field), entry->second);
          }
        }
      }
    }
    else if (it.key() != "assembly") {
      fillLegacyTreeFromValue(it.value(), child->second);
    }
  }
  decodeSelectionProjectionV3(value, root, generated.executionRole.roleId, &generated, generatedHere);
}

} // namespace

void detail::validateNativeSelectionProjectionV3Value(
  const NativeJson& value, const NativeSelectionProjectionV3& generated)
{
  validateGeneratedSelectionValue(value, generated, false);
}

const std::map<std::string, std::string>& NativeSelectionProjectionV3::providerOwnership() const
{
  return fixedProviderByRole.empty() && fixedExecutionTemplateOwner
    ? fixedExecutionTemplateOwner->providerByRole : fixedProviderByRole;
}

std::shared_ptr<const NativeSelectionRoleV3>
NativeSelectionProjectionV3::shareFixedRoleStorage()
{
  if (!fixedRoleOwner) {
    if (!sameAssembly(selectedRole, assembly)) {
      throw std::invalid_argument("fixed role storage requires an identical assembly contract");
    }
    fixedRoleOwner = std::make_shared<const NativeSelectionRoleV3>(std::move(selectedRole));
    selectedRole = {};
    assembly = {};
  }
  return fixedRoleOwner;
}

namespace {
const std::string& fixedTemplateDigestPlaceholder()
{
  static const std::string value = "sha256:" + std::string(64, '0');
  return value;
}

NativeJson fixedGenerationJson(const NativeGenerationExecutionContractV1& value)
{
  if (!value.enabled) return nullptr;
  return NativeJson{{"mode", value.mode}, {"token_input_name", value.tokenInputName},
    {"state_input_names", value.stateInputNames}, {"state_output_names", value.stateOutputNames},
    {"state_successor_map", value.stateSuccessorMap}, {"position_input_policy", value.positionInputPolicy},
    {"attention_mask_input_name", value.attentionMaskInputName}, {"position_ids_input_name", value.positionIdsInputName},
    {"cache_position_input_name", value.cachePositionInputName}, {"eos_token_ids", value.eosTokenIds},
    {"tokenizer_digest", value.tokenizerDigest}, {"streaming_operation_stride", value.streamingOperationStride}};
}

NativeJson fixedDependenciesJson(const std::vector<NativeDependencySpec>& dependencies)
{
  auto result = NativeJson::array();
  for (const auto& dependency : dependencies) {
    auto value = jsonValue(dependency);
    value["segment_naming"] = {{"mode", dependency.segmentNaming.mode},
      {"static_segment_count", dependency.segmentNaming.staticSegmentCount},
      {"dynamic_fallback", dependency.segmentNaming.dynamicFallback}};
    result.push_back(std::move(value));
  }
  return result;
}

NativeJson fixedTemplateJson(const NativeFixedExecutionTemplateV1& value)
{
  return NativeJson{{"provider_by_role", value.providerByRole}, {"dependencies", fixedDependenciesJson(value.dependencies)},
    {"dataflow", jsonValue(value.dataflow)}, {"generation_interface", fixedGenerationJson(value.generation)},
    {"expand_role_epochs", value.expandRoleEpochs}};
}

bool sameFixedGeneration(const NativeGenerationExecutionContractV1& left,
                         const NativeGenerationExecutionContractV1& right)
{
  return std::tie(left.enabled, left.mode, left.tokenInputName, left.stateInputNames, left.stateOutputNames,
    left.stateSuccessorMap, left.positionInputPolicy, left.attentionMaskInputName, left.positionIdsInputName,
    left.cachePositionInputName, left.eosTokenIds, left.tokenizerDigest, left.streamingOperationStride) ==
    std::tie(right.enabled, right.mode, right.tokenInputName, right.stateInputNames, right.stateOutputNames,
    right.stateSuccessorMap, right.positionInputPolicy, right.attentionMaskInputName, right.positionIdsInputName,
    right.cachePositionInputName, right.eosTokenIds, right.tokenizerDigest, right.streamingOperationStride);
}

void validateFixedTemplateProviderCover(const NativeFixedExecutionTemplateV1& fixed,
                                       const std::string& role, const std::string& provider)
{
  const auto local = fixed.providerByRole.find(role);
  if (local == fixed.providerByRole.end() || local->second != provider || fixed.dataflow.role != role)
    throw std::invalid_argument("fixed execution template local Provider ownership mismatch");
  for (const auto& [name, owner] : fixed.providerByRole)
    if (name.empty() || owner.empty() || owner.front() != '/')
      throw std::invalid_argument("fixed execution template Provider cover is invalid");
  const auto requireOwner = [&](const std::string& name) {
    if (!fixed.providerByRole.count(name)) throw std::invalid_argument("fixed execution template Provider owner is missing");
  };
  for (const auto& dependency : fixed.dependencies) {
    for (const auto& name : dependency.producers) requireOwner(name);
    for (const auto& name : dependency.consumers) requireOwner(name);
  }
  for (const auto* endpoints : {&fixed.dataflow.mayPublish, &fixed.dataflow.mustFetch})
    for (const auto& endpoint : *endpoints) {
      if (endpoint.sourceKind == "ROLE") requireOwner(endpoint.producerRole);
      for (const auto& consumer : endpoint.consumerRoles) requireOwner(consumer);
    }
}

bool sameFixedDependency(const NativeDependencySpec& left, const NativeDependencySpec& right)
{
  if (std::tie(left.producers, left.consumers, left.topicPrefix, left.objectNameTemplate, left.tensors,
      left.operationKind, left.useNdnsfDataV1, left.collectiveOperationIndex, left.collectiveProducerRank,
      left.collectiveSourceLayoutDigest, left.collectiveTargetLayoutDigest, left.collectiveTensorDigest,
      left.segmentNaming.mode, left.segmentNaming.staticSegmentCount, left.segmentNaming.dynamicFallback) !=
      std::tie(right.producers, right.consumers, right.topicPrefix, right.objectNameTemplate, right.tensors,
      right.operationKind, right.useNdnsfDataV1, right.collectiveOperationIndex, right.collectiveProducerRank,
      right.collectiveSourceLayoutDigest, right.collectiveTargetLayoutDigest, right.collectiveTensorDigest,
      right.segmentNaming.mode, right.segmentNaming.staticSegmentCount, right.segmentNaming.dynamicFallback) ||
      (left.operationKind != "TOKEN_FEEDBACK" && left.keyScope != right.keyScope) ||
      left.redistributions.size() != right.redistributions.size()) return false;
  for (std::size_t index = 0; index < left.redistributions.size(); ++index) {
    const auto& l = left.redistributions[index];
    const auto& r = right.redistributions[index];
    if (std::tie(l.producerRanks, l.consumerRanks, l.tensor, l.operation, l.epoch, l.integrityDigest,
        l.sourceLayoutDigest, l.targetLayoutDigest, l.axis, l.temporaryMemoryBytes, l.completeOutput) !=
        std::tie(r.producerRanks, r.consumerRanks, r.tensor, r.operation, r.epoch, r.integrityDigest,
        r.sourceLayoutDigest, r.targetLayoutDigest, r.axis, r.temporaryMemoryBytes, r.completeOutput)) return false;
  }
  return true;
}

bool sameFixedEndpointAtRound(const NativeTensorEndpointV3& left, const NativeTensorEndpointV3& right,
                              std::uint64_t expectedRound)
{
  return right.round == expectedRound &&
    std::tie(left.operation, left.sourceKind, left.producerRole, left.producerRank,
    left.consumerRole, left.consumerRoles, left.tensorId, left.bundleTensorNames, left.securityProfile, left.namingOperationStride) ==
    std::tie(right.operation, right.sourceKind, right.producerRole, right.producerRank,
    right.consumerRole, right.consumerRoles, right.tensorId, right.bundleTensorNames, right.securityProfile, right.namingOperationStride) &&
    (left.sourceKind == "APPLICATION_INPUT" ||
      std::tie(left.tensorDigest, left.layoutDigest, left.targetLayoutDigest) ==
      std::tie(right.tensorDigest, right.layoutDigest, right.targetLayoutDigest));
}

// GroupProjectionBuilder stores consecutive epochs for each base ROLE slot.
// Collapse only that exact pattern; arbitrary valid V3 flows remain supported.
void collapseFixedRoleEpochs(NativeFixedExecutionTemplateV1& fixed)
{
  const auto& generation = fixed.generation;
  if (!generation.enabled || generation.streamingOperationStride == 0) return;
  const auto epochs = generation.maxGeneratedTokens + 1;
  const auto stride = generation.streamingOperationStride;
  if (epochs < 2 || epochs > 1026 || stride > (1U << 20)) return;
  bool hasRole = false;
  const auto collapse = [&](const auto& expanded, auto& base) {
    for (std::size_t index = 0; index < expanded.size();) {
      const auto& first = expanded[index];
      base.push_back(first);
      if (first.sourceKind != "ROLE") { ++index; continue; }
      hasRole = true;
      if (first.round >= stride || epochs > expanded.size() - index) return false;
      for (std::uint64_t epoch = 0; epoch < epochs; ++epoch)
        if (!sameFixedEndpointAtRound(first, expanded[index + epoch], first.round + epoch * stride)) return false;
      index += epochs;
    }
    return true;
  };
  std::vector<NativeTensorEndpointV3> publish, fetch;
  if (!collapse(fixed.dataflow.mayPublish, publish) ||
      !collapse(fixed.dataflow.mustFetch, fetch) || !hasRole) return;
  // Readiness must be for the retained prefill slots, not a future epoch.
  for (const auto& wait : fixed.dataflow.waitFor)
    for (const auto& digest : wait.endpointDigests)
      if (std::none_of(fetch.begin(), fetch.end(), [&](const auto& endpoint) {
            return endpoint.endpointDigest == digest;
          })) return;
  fixed.dataflow.mayPublish = std::move(publish);
  fixed.dataflow.mustFetch = std::move(fetch);
  fixed.expandRoleEpochs = true;
}

std::size_t expandedEndpointOffset(const std::vector<NativeTensorEndpointV3>& base,
                                   std::size_t slot, std::uint64_t epochs)
{
  std::size_t result = 0;
  for (std::size_t index = 0; index < slot; ++index)
    result += base[index].sourceKind == "ROLE" ? epochs : 1;
  return result;
}

void expandFixedRoleEpochs(NativeRoleDataflowContractV3& flow,
                           const NativeGenerationExecutionContractV1& generation)
{
  if (!generation.enabled || generation.maxGeneratedTokens == 0 || generation.maxGeneratedTokens > 1025 ||
      generation.streamingOperationStride == 0 || generation.streamingOperationStride > (1U << 20))
    throw std::invalid_argument("invalid invocation epoch expansion");
  const auto epochs = generation.maxGeneratedTokens + 1;
  const auto expand = [&](auto& base) {
    const auto count = expandedEndpointOffset(base, base.size(), epochs);
    if (count > (1U << 20) / 128)
      throw std::invalid_argument("generation endpoint expansion exceeds wire bounds");
    std::vector<NativeTensorEndpointV3> expanded;
    expanded.reserve(count);
    for (const auto& endpoint : base) {
      if (endpoint.sourceKind == "ROLE" &&
          (endpoint.namingOperationStride != generation.streamingOperationStride ||
           endpoint.round >= generation.streamingOperationStride ||
           generation.streamingOperationStride >
             (std::numeric_limits<std::uint64_t>::max() - endpoint.round) / (epochs - 1)))
        throw std::invalid_argument("fixed tensor naming stride or epoch expansion is invalid");
      const auto repetitions = endpoint.sourceKind == "ROLE" ? epochs : 1;
      for (std::uint64_t epoch = 0; epoch < repetitions; ++epoch) {
        auto current = endpoint;
        current.round += epoch * generation.streamingOperationStride;
        expanded.push_back(std::move(current));
      }
    }
    base = std::move(expanded);
  };
  expand(flow.mayPublish);
  expand(flow.mustFetch);
}

NativeFixedExecutionTemplateV1 normalizeExecutionTemplate(NativeFixedExecutionTemplateV1 value)
{
  validateTensorNamingStrideContract(value.dataflow, value.generation, false);
  for (auto& dependency : value.dependencies) {
    if (dependency.operationKind == "TOKEN_FEEDBACK") dependency.keyScope = "fixed-token-feedback";
    dependency.expectedSegments = 0;
    dependency.expectedBytes = 0;
  }
  auto& flow = value.dataflow;
  flow.requestId = "/fixed-template";
  flow.attempt = 1;
  flow.planDigest = fixedTemplateDigestPlaceholder();
  std::map<std::string, std::string> readinessBindings;
  const auto normalizeEndpoints = [&](auto& endpoints, const char* section) {
    std::size_t slot = 0;
    for (auto& endpoint : endpoints) {
      const auto oldDigest = endpoint.endpointDigest;
      endpoint.producerNamespace = endpoint.requester = endpoint.requestId = "/fixed-template";
      endpoint.attempt = 1;
      endpoint.planDigest = fixedTemplateDigestPlaceholder();
      endpoint.groupId = endpoint.groupEpoch = "fixed-template";
      endpoint.microbatch = 0;
      endpoint.segmentCount = 1;
      endpoint.noProgressDeadlineMs = endpoint.hardDeadlineMs = 1;
      endpoint.prefetchBundleBytes = endpoint.prefetchSegmentBytes = 0;
      endpoint.inlineFirstSegment = false;
      if (endpoint.sourceKind == "APPLICATION_INPUT")
        endpoint.tensorDigest = endpoint.layoutDigest = endpoint.targetLayoutDigest = fixedTemplateDigestPlaceholder();
      endpoint.manifestDigest = fixedTemplateDigestPlaceholder();
      endpoint.endpointDigest.clear();
      auto identity = jsonValue(endpoint);
      identity.erase("endpoint_digest");
      identity.erase("consumer_role");
      // Fixed interfaces can repeat across current microbatches. Bind the
      // template slot, not invocation state, to keep their identities unique.
      identity["template_slot"] = NativeJson::array({flow.role, section, slot++});
      endpoint.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
      readinessBindings.emplace(oldDigest, endpoint.endpointDigest);
    }
  };
  normalizeEndpoints(flow.mayPublish, "may_publish");
  normalizeEndpoints(flow.mustFetch, "must_fetch");
  for (auto& readiness : flow.waitFor)
    for (auto& digest : readiness.endpointDigests) digest = readinessBindings.at(digest);
  flow.dataflowDigest.clear();
  auto identity = jsonValue(flow);
  identity.erase("dataflow_digest");
  flow.dataflowDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  if (value.generation.enabled) {
    // Retain only the model interface. Sampling and prefix state belong to
    // the invocation; their defaults never enter the fixed source body.
    const auto& original = value.generation;
    NativeGenerationExecutionContractV1 fixed;
    fixed.enabled = true;
    fixed.mode = original.mode;
    fixed.tokenInputName = original.tokenInputName;
    fixed.stateInputNames = original.stateInputNames;
    fixed.stateOutputNames = original.stateOutputNames;
    fixed.stateSuccessorMap = original.stateSuccessorMap;
    fixed.positionInputPolicy = original.positionInputPolicy;
    fixed.attentionMaskInputName = original.attentionMaskInputName;
    fixed.positionIdsInputName = original.positionIdsInputName;
    fixed.cachePositionInputName = original.cachePositionInputName;
    fixed.eosTokenIds = original.eosTokenIds;
    fixed.tokenizerDigest = original.tokenizerDigest;
    fixed.streamingOperationStride = original.streamingOperationStride;
    value.generation = std::move(fixed);
  }
  return value;
}

NativeFixedExecutionTemplateV1 fixedTemplateFromJson(const NativeJson& value)
{
  NativeFixedExecutionTemplateV1 result;
  if (!value.is_object() || value.size() != 5 || !value.at("dependencies").is_array() ||
      !value.at("expand_role_epochs").is_boolean())
    throw std::invalid_argument("invalid fixed execution template fields");
  result.providerByRole = value.at("provider_by_role").get<std::map<std::string, std::string>>();
  result.expandRoleEpochs = value.at("expand_role_epochs").get<bool>();
  for (const auto& item : value.at("dependencies")) {
    auto dependency = dependencyFromV3Json(legacyTreeFromValue(item));
    const auto& naming = item.at("segment_naming");
    dependency.segmentNaming.mode = naming.at("mode").get<std::string>();
    dependency.segmentNaming.staticSegmentCount = naming.at("static_segment_count").get<std::size_t>();
    dependency.segmentNaming.dynamicFallback = naming.at("dynamic_fallback").get<bool>();
    result.dependencies.push_back(std::move(dependency));
  }
  for (const auto* endpoints : {"may_publish", "must_fetch"}) {
    for (const auto& endpoint : value.at("dataflow").at(endpoints)) {
      if (!endpoint.contains("naming_operation_stride") ||
          !endpoint.at("naming_operation_stride").is_number_unsigned())
        throw std::invalid_argument("fixed tensor naming stride is not UInt64");
    }
  }
  result.dataflow = dataflowFromV3Json(legacyTreeFromValue(value.at("dataflow")));
  const auto& generation = value.at("generation_interface");
  if (!generation.is_null()) {
    auto& fixed = result.generation;
    fixed.enabled = true;
    fixed.mode = generation.at("mode").get<std::string>();
    fixed.tokenInputName = generation.at("token_input_name").get<std::string>();
    fixed.stateInputNames = generation.at("state_input_names").get<std::vector<std::string>>();
    fixed.stateOutputNames = generation.at("state_output_names").get<std::vector<std::string>>();
    fixed.stateSuccessorMap = generation.at("state_successor_map").get<std::string>();
    fixed.positionInputPolicy = generation.at("position_input_policy").get<std::string>();
    fixed.attentionMaskInputName = generation.at("attention_mask_input_name").get<std::string>();
    fixed.positionIdsInputName = generation.at("position_ids_input_name").get<std::string>();
    fixed.cachePositionInputName = generation.at("cache_position_input_name").get<std::string>();
    fixed.eosTokenIds = generation.at("eos_token_ids").get<std::vector<std::int64_t>>();
    fixed.tokenizerDigest = generation.at("tokenizer_digest").get<std::string>();
    fixed.streamingOperationStride = generation.at("streaming_operation_stride").get<std::uint64_t>();
  }
  if (result.expandRoleEpochs) {
    if (!result.generation.enabled || result.generation.streamingOperationStride == 0 ||
        result.generation.streamingOperationStride > (1U << 20))
      throw std::invalid_argument("fixed epoch template requires a generation stride");
    bool hasRole = false;
    for (const auto* endpoints : {&result.dataflow.mayPublish, &result.dataflow.mustFetch})
      for (const auto& endpoint : *endpoints)
        if (endpoint.sourceKind == "ROLE") {
          hasRole = true;
          if (endpoint.round >= result.generation.streamingOperationStride)
            throw std::invalid_argument("fixed epoch template contains an expanded round");
        }
    if (!hasRole) throw std::invalid_argument("fixed epoch template has no ROLE slots");
  }
  auto normalized = normalizeExecutionTemplate(std::move(result));
  if (nativeCanonicalJson(fixedTemplateJson(normalized)) != nativeCanonicalJson(value))
    throw std::invalid_argument("fixed execution template is not canonical or contains invocation state");
  return normalized;
}

void requireOnlyBindingKeys(const NativeJson& value, std::initializer_list<const char*> keys)
{
  if (!value.is_object()) throw std::invalid_argument("invocation bindings must be an object");
  for (auto it = value.begin(); it != value.end(); ++it)
    if (std::none_of(keys.begin(), keys.end(), [&](const char* key) { return it.key() == key; }))
      throw std::invalid_argument("invocation bindings contain a fixed or unknown field");
}

std::uint64_t unsignedBinding(const NativeJson& value, const char* key)
{
  const auto& number = value.at(key);
  if (!number.is_number_unsigned()) throw std::invalid_argument("invocation binding is not an unsigned integer");
  return number.get<std::uint64_t>();
}

NativeJson dependencyBindingsJson(const std::vector<NativeDependencySpec>& dependencies)
{
  auto result = NativeJson::array();
  for (const auto& dependency : dependencies) {
    NativeJson binding{{"expected_segments", dependency.expectedSegments}, {"expected_bytes", dependency.expectedBytes}};
    if (dependency.operationKind == "TOKEN_FEEDBACK") binding["key_scope"] = dependency.keyScope;
    result.push_back(std::move(binding));
  }
  return result;
}

std::vector<NativeDependencySpec> bindFixedTemplateDependencies(
  const NativeJson& bindings, const NativeFixedExecutionTemplateV1& fixed)
{
  if (!bindings.is_array() || bindings.size() != fixed.dependencies.size())
    throw std::invalid_argument("invocation dependency binding count mismatch");
  auto dependencies = fixed.dependencies;
  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    const auto& binding = bindings.at(index);
    auto& dependency = dependencies[index];
    requireOnlyBindingKeys(binding, {"expected_segments", "expected_bytes", "key_scope"});
    const bool feedback = dependency.operationKind == "TOKEN_FEEDBACK";
    if (binding.size() != (feedback ? 3U : 2U) || binding.contains("key_scope") != feedback)
      throw std::invalid_argument("invocation dependency binding fields mismatch");
    dependency.expectedSegments = unsignedBinding(binding, "expected_segments");
    dependency.expectedBytes = unsignedBinding(binding, "expected_bytes");
    if (feedback) {
      dependency.keyScope = binding.at("key_scope").get<std::string>();
      if (dependency.keyScope.empty()) throw std::invalid_argument("invocation feedback scope is empty");
    }
  }
  return dependencies;
}

NativeJson endpointBindingsJson(const std::vector<NativeTensorEndpointV3>& endpoints)
{
  auto result = NativeJson::array();
  for (const auto& endpoint : endpoints) {
    NativeJson binding{{"producer_namespace", endpoint.producerNamespace}, {"requester", endpoint.requester},
      {"group_id", endpoint.groupId}, {"group_epoch", endpoint.groupEpoch}, {"microbatch", endpoint.microbatch},
      {"segment_count", endpoint.segmentCount}, {"manifest_digest", endpoint.manifestDigest},
      {"endpoint_digest", endpoint.endpointDigest}, {"no_progress_deadline_ms", endpoint.noProgressDeadlineMs},
      {"hard_deadline_ms", endpoint.hardDeadlineMs}};
    if (endpoint.sourceKind == "APPLICATION_INPUT") {
      binding["tensor_digest"] = endpoint.tensorDigest;
      binding["layout_digest"] = endpoint.layoutDigest;
      binding["target_layout_digest"] = endpoint.targetLayoutDigest;
    }
    if (endpoint.prefetchBundleBytes != 0 || endpoint.prefetchSegmentBytes != 0) {
      binding["prefetch_bundle_bytes"] = endpoint.prefetchBundleBytes;
      binding["prefetch_segment_bytes"] = endpoint.prefetchSegmentBytes;
    }
    if (endpoint.inlineFirstSegment) binding["inline_first_segment"] = true;
    result.push_back(std::move(binding));
  }
  return result;
}

NativeJson dataflowBindingsJson(const NativeRoleDataflowContractV3& flow)
{
  auto readiness = NativeJson::array();
  for (const auto& wait : flow.waitFor) readiness.push_back(wait.endpointDigests);
  return NativeJson{{"may_publish", endpointBindingsJson(flow.mayPublish)},
    {"must_fetch", endpointBindingsJson(flow.mustFetch)}, {"wait_for", std::move(readiness)},
    {"dataflow_digest", flow.dataflowDigest}};
}

NativeRoleDataflowContractV3 bindFixedTemplateDataflow(
  const NativeJson& bindings, const NativeSelectionProjectionV3& context,
  const NativeFixedExecutionTemplateV1& fixed)
{
  requireOnlyBindingKeys(bindings, {"may_publish", "must_fetch", "wait_for", "dataflow_digest"});
  if (bindings.size() != 4) throw std::invalid_argument("invocation dataflow binding fields mismatch");
  auto flow = fixed.dataflow;
  if (fixed.expandRoleEpochs) expandFixedRoleEpochs(flow, context.generationContract);
  flow.requestId = context.requestId;
  flow.attempt = context.attempt;
  flow.planDigest = context.planDigest;
  flow.dataflowDigest = bindings.at("dataflow_digest").get<std::string>();
  const auto bind = [&](auto& endpoints, const char* key) {
    const auto& values = bindings.at(key);
    if (!values.is_array() || values.size() != endpoints.size())
      throw std::invalid_argument("invocation endpoint binding count mismatch");
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
      auto& endpoint = endpoints[index];
      const auto& binding = values.at(index);
      requireOnlyBindingKeys(binding, {"producer_namespace", "requester", "group_id", "group_epoch", "microbatch",
        "segment_count", "manifest_digest", "endpoint_digest", "no_progress_deadline_ms", "hard_deadline_ms",
        "tensor_digest", "layout_digest", "target_layout_digest", "prefetch_bundle_bytes", "prefetch_segment_bytes",
        "inline_first_segment"});
      const bool input = endpoint.sourceKind == "APPLICATION_INPUT";
      for (const auto* field : {"tensor_digest", "layout_digest", "target_layout_digest"})
        if (binding.contains(field) != input) throw std::invalid_argument("invocation input binding fields mismatch");
      endpoint.requestId = context.requestId;
      endpoint.attempt = context.attempt;
      endpoint.planDigest = context.planDigest;
      endpoint.producerNamespace = binding.at("producer_namespace").get<std::string>();
      endpoint.requester = binding.at("requester").get<std::string>();
      endpoint.groupId = binding.at("group_id").get<std::string>();
      endpoint.groupEpoch = binding.at("group_epoch").get<std::string>();
      endpoint.microbatch = unsignedBinding(binding, "microbatch");
      endpoint.segmentCount = unsignedBinding(binding, "segment_count");
      endpoint.manifestDigest = binding.at("manifest_digest").get<std::string>();
      endpoint.endpointDigest = binding.at("endpoint_digest").get<std::string>();
      endpoint.noProgressDeadlineMs = unsignedBinding(binding, "no_progress_deadline_ms");
      endpoint.hardDeadlineMs = unsignedBinding(binding, "hard_deadline_ms");
      endpoint.prefetchBundleBytes = binding.contains("prefetch_bundle_bytes") ? unsignedBinding(binding, "prefetch_bundle_bytes") : 0;
      endpoint.prefetchSegmentBytes = binding.contains("prefetch_segment_bytes") ? unsignedBinding(binding, "prefetch_segment_bytes") : 0;
      if (binding.contains("prefetch_bundle_bytes") != binding.contains("prefetch_segment_bytes"))
        throw std::invalid_argument("invocation prefetch binding is incomplete");
      endpoint.inlineFirstSegment = binding.value("inline_first_segment", false);
      if (input) {
        endpoint.tensorDigest = binding.at("tensor_digest").get<std::string>();
        endpoint.layoutDigest = binding.at("layout_digest").get<std::string>();
        endpoint.targetLayoutDigest = binding.at("target_layout_digest").get<std::string>();
      }
    }
  };
  bind(flow.mayPublish, "may_publish");
  bind(flow.mustFetch, "must_fetch");
  const auto& readiness = bindings.at("wait_for");
  if (!readiness.is_array() || readiness.size() != flow.waitFor.size())
    throw std::invalid_argument("invocation readiness binding count mismatch");
  for (std::size_t index = 0; index < flow.waitFor.size(); ++index) {
    auto& digests = flow.waitFor[index].endpointDigests;
    const auto current = readiness.at(index).get<std::vector<std::string>>();
    if (current.size() != digests.size())
      throw std::invalid_argument("invocation readiness slot count mismatch");
    for (std::size_t slot = 0; slot < digests.size(); ++slot) {
      const auto found = std::find_if(fixed.dataflow.mustFetch.begin(), fixed.dataflow.mustFetch.end(),
        [&](const auto& endpoint) { return endpoint.endpointDigest == digests[slot]; });
      if (found == fixed.dataflow.mustFetch.end())
        throw std::invalid_argument("fixed readiness slot is missing");
      const auto offset = expandedEndpointOffset(fixed.dataflow.mustFetch,
        std::distance(fixed.dataflow.mustFetch.begin(), found),
        fixed.expandRoleEpochs ? context.generationContract.maxGeneratedTokens + 1 : 1);
      if (current[slot] != flow.mustFetch.at(offset).endpointDigest)
        throw std::invalid_argument("invocation readiness changed its fixed slot");
    }
    digests = current;
  }
  validateNativeRoleDataflowContractV3(flow);
  validateTensorNamingStrideContract(flow, context.generationContract);
  return flow;
}

NativeJson generationBindingsJson(const NativeGenerationExecutionContractV1& generation)
{
  NativeJson result{{"max_generated_tokens", generation.maxGeneratedTokens}, {"sampling_digest", generation.samplingDigest},
    {"sampling_mode", generation.samplingMode}, {"sampling_temperature", generation.samplingTemperature},
    {"sampling_top_k", generation.samplingTopK}, {"sampling_top_p", generation.samplingTopP},
    {"sampling_repetition_penalty", generation.samplingRepetitionPenalty}, {"sampling_seed", generation.samplingSeed},
    {"stop_strings", generation.stopStrings}, {"committed_prefix_token_ids", generation.committedPrefixTokenIds}};
  if (!generation.generationId.empty()) result["generation_id"] = generation.generationId;
  return result;
}

NativeJson selectionInvocationFields(const NativeSelectionProjectionV3& value,
                                    bool referenceExecutionRole = false)
{
  NativeJson root = {
    {"schema", "ndnsf-di-selection-v3"}, {"schema_version", 3},
    {"provider", value.provider}, {"request_id", value.requestId}, {"attempt", value.attempt},
    {"plan_core_digest", value.planCoreDigest}, {"plan_digest", value.planDigest},
    {"ack_closed_digest", value.ackClosedDigest}, {"offer_digest", value.offerDigest},
    {"security_policy_snapshot_digest", value.securityPolicySnapshotDigest},
    {"deadline_ms", value.deadlineMs}, {"group_capability_v1", value.groupCapabilityV1},
    {"execution_role", referenceExecutionRole
      ? NativeJson{{"role_id", value.executionRole.roleId}} : jsonValue(value.executionRole)},
    {"dataflow", referenceExecutionRole ? dataflowBindingsJson(value.dataflow) : jsonValue(value.dataflow)},
    {"device_binding", jsonValue(value.deviceBinding)},
    {"dependencies", referenceExecutionRole ? dependencyBindingsJson(value.plan.dependencies) : jsonArray(value.plan.dependencies)},
  };
  if (!value.requestContractDigest.empty()) root["request_contract_digest"] = value.requestContractDigest;
  if (value.generationContract.enabled) root["generation_contract"] = referenceExecutionRole
    ? generationBindingsJson(value.generationContract) : jsonValue(value.generationContract);
  if (value.conversationStateReference) root["conversation_state_reference"] = jsonValue(*value.conversationStateReference);
  if (value.conversationTurnBinding) root["conversation_turn_binding"] = jsonValue(*value.conversationTurnBinding);
  if (value.hasGrantBinding) {
    root["grant_binding"] = {{"provider", value.provider}, {"request_id", value.requestId},
      {"attempt", value.attempt}, {"plan_core_digest", value.planCoreDigest},
      {"security_policy_snapshot_digest", value.securityPolicySnapshotDigest},
      {"protection_epoch", value.selectedRoleContract().protectionEpoch},
      {"grant_name", value.grantName}, {"grant_digest", value.grantDigest}};
    if (value.grantLeaseScope)
      root["grant_binding"]["lease_scope"] = jsonValue(*value.grantLeaseScope);
  }
  return root;
}
} // namespace

void validateNativeSelectionInvocationBindingShapeV1(const NativeJson& value)
{
  const auto& dependencies = value.at("dependencies");
  if (!dependencies.is_array()) throw std::invalid_argument("invocation dependencies must be an array");
  for (const auto& binding : dependencies) {
    requireOnlyBindingKeys(binding, {"expected_segments", "expected_bytes", "key_scope"});
    unsignedBinding(binding, "expected_segments");
    unsignedBinding(binding, "expected_bytes");
    if (binding.contains("key_scope") && (!binding.at("key_scope").is_string() ||
        binding.at("key_scope").get<std::string>().empty()))
      throw std::invalid_argument("invocation feedback scope is invalid");
  }
  const auto& flow = value.at("dataflow");
  requireOnlyBindingKeys(flow, {"may_publish", "must_fetch", "wait_for", "dataflow_digest"});
  if (flow.size() != 4 || !flow.at("dataflow_digest").is_string() ||
      !flow.at("wait_for").is_array())
    throw std::invalid_argument("invocation dataflow binding fields mismatch");
  for (const auto& wait : flow.at("wait_for")) {
    if (!wait.is_array()) throw std::invalid_argument("invocation readiness bindings must be arrays");
    for (const auto& digest : wait)
      if (!digest.is_string()) throw std::invalid_argument("invocation readiness digest must be a string");
  }
  for (const auto* key : {"may_publish", "must_fetch"}) {
    if (!flow.at(key).is_array()) throw std::invalid_argument("invocation endpoint bindings must be arrays");
    for (const auto& binding : flow.at(key)) {
      requireOnlyBindingKeys(binding, {"producer_namespace", "requester", "group_id", "group_epoch", "microbatch",
        "segment_count", "manifest_digest", "endpoint_digest", "no_progress_deadline_ms", "hard_deadline_ms",
        "tensor_digest", "layout_digest", "target_layout_digest", "prefetch_bundle_bytes", "prefetch_segment_bytes",
        "inline_first_segment"});
      for (const auto* field : {"producer_namespace", "requester", "group_id", "group_epoch", "manifest_digest", "endpoint_digest"})
        if (!binding.at(field).is_string()) throw std::invalid_argument("invocation endpoint binding must be a string");
      for (const auto* field : {"microbatch", "segment_count", "no_progress_deadline_ms", "hard_deadline_ms"})
        unsignedBinding(binding, field);
      for (const auto* field : {"tensor_digest", "layout_digest", "target_layout_digest"})
        if (binding.contains(field) && !binding.at(field).is_string())
          throw std::invalid_argument("invocation input binding must be a string");
      if (binding.contains("prefetch_bundle_bytes") != binding.contains("prefetch_segment_bytes"))
        throw std::invalid_argument("invocation prefetch binding is incomplete");
      if (binding.contains("prefetch_bundle_bytes")) {
        unsignedBinding(binding, "prefetch_bundle_bytes");
        unsignedBinding(binding, "prefetch_segment_bytes");
      }
      if (binding.contains("inline_first_segment") && !binding.at("inline_first_segment").is_boolean())
        throw std::invalid_argument("invocation inline binding must be boolean");
    }
  }
  if (value.contains("generation_contract"))
    requireOnlyBindingKeys(value.at("generation_contract"), {"max_generated_tokens", "sampling_digest", "sampling_mode",
      "sampling_temperature", "sampling_top_k", "sampling_top_p", "sampling_repetition_penalty", "sampling_seed",
      "stop_strings", "generation_id", "committed_prefix_token_ids"});
}

std::string nativeSelectionProjectionV3ToJson(const NativeSelectionProjectionV3& value)
{
  auto root = selectionInvocationFields(value);
  root["roles"] = NativeJson::array({jsonValue(value.selectedRoleContract())});
  root["assembly"] = jsonValue(value.assemblyContract());
  if (value.compactAssembly) {
    if (!sameGeneratedJson(root.at("roles").at(0), root.at("assembly")))
      throw std::invalid_argument("compact Selection assembly differs from its role");
    root.erase("assembly");
    root["assembly_same_as_role"] = true;
  }
  const auto wire = nativeCanonicalJson(root);
  // Keep the production decoder's complete role, grant and endpoint binding
  // checks, reusing the generated value instead of parsing our own wire twice.
  validateGeneratedSelectionValue(root, value, true);
  return wire;
}

NativeFixedRoleAssignmentV1::NativeFixedRoleAssignmentV1(
  std::string provider, std::string digest,
  std::shared_ptr<const NativeSelectionRoleV3> role,
  std::shared_ptr<const NativeFixedExecutionTemplateV1> executionTemplate)
  : m_provider(std::move(provider))
  , m_digest(std::move(digest))
  , m_role(std::move(role))
  , m_template(std::move(executionTemplate))
{
  ++detail::fixedAssignmentWork().roleBuilds;
  // These fields are already bound by the full projection validator and are
  // uniquely determined by the fixed role. Keep one typed cached snapshot.
  m_executionRole = {m_role->selectedRole, m_role->role, m_role->rank,
    m_role->layerBegin, m_role->layerEnd, m_role->backend,
    m_role->adapterId, m_role->adapterVersion};
}

bool NativeFixedRoleAssignmentV1::matchesExecutionTemplate(const NativeSelectionProjectionV3& projection) const
{
  const auto& fixed = *m_template;
  const auto& current = projection.dataflow;
  if (projection.provider != m_provider ||
      !sameAssembly(projection.selectedRoleContract(), *m_role) ||
      !sameAssembly(projection.assemblyContract(), *m_role) ||
      !sameFixedGeneration(projection.generationContract, fixed.generation) ||
      projection.plan.dependencies.size() != fixed.dependencies.size() ||
      current.role != fixed.dataflow.role || current.terminalResponseOwner != fixed.dataflow.terminalResponseOwner ||
      current.waitFor.size() != fixed.dataflow.waitFor.size()) return false;
  if (projection.providerOwnership().empty()) {
    if (fixed.providerByRole.size() != 1 || fixed.providerByRole.begin()->first != m_role->selectedRole ||
        fixed.providerByRole.begin()->second != m_provider) return false;
  }
  else if (projection.providerOwnership() != fixed.providerByRole) return false;
  for (std::size_t index = 0; index < fixed.dependencies.size(); ++index)
    if (!sameFixedDependency(projection.plan.dependencies[index], fixed.dependencies[index])) return false;
  const auto epochs = fixed.expandRoleEpochs ? projection.generationContract.maxGeneratedTokens + 1 : 1;
  if (fixed.expandRoleEpochs && (epochs < 2 || epochs > 1026)) return false;
  const auto sameEndpoints = [&](const auto& expanded, const auto& base) {
    if (expanded.size() != expandedEndpointOffset(base, base.size(), epochs)) return false;
    std::size_t index = 0;
    for (const auto& endpoint : base) {
      const auto repetitions = endpoint.sourceKind == "ROLE" ? epochs : 1;
      for (std::uint64_t epoch = 0; epoch < repetitions; ++epoch)
        if (!sameFixedEndpointAtRound(endpoint, expanded[index++],
              endpoint.round + epoch * fixed.generation.streamingOperationStride)) return false;
    }
    return true;
  };
  if (!sameEndpoints(current.mayPublish, fixed.dataflow.mayPublish) ||
      !sameEndpoints(current.mustFetch, fixed.dataflow.mustFetch)) return false;
  for (std::size_t index = 0; index < current.waitFor.size(); ++index) {
    const auto& left = current.waitFor[index];
    const auto& right = fixed.dataflow.waitFor[index];
    if (left.mode != right.mode || left.quorum != right.quorum ||
        left.endpointDigests.size() != right.endpointDigests.size()) return false;
    for (std::size_t endpoint = 0; endpoint < left.endpointDigests.size(); ++endpoint) {
      const auto l = std::find_if(current.mustFetch.begin(), current.mustFetch.end(),
        [&](const auto& value) { return value.endpointDigest == left.endpointDigests[endpoint]; });
      const auto r = std::find_if(fixed.dataflow.mustFetch.begin(), fixed.dataflow.mustFetch.end(),
        [&](const auto& value) { return value.endpointDigest == right.endpointDigests[endpoint]; });
      if (l == current.mustFetch.end() || r == fixed.dataflow.mustFetch.end() ||
          static_cast<std::size_t>(std::distance(current.mustFetch.begin(), l)) !=
            expandedEndpointOffset(fixed.dataflow.mustFetch,
              std::distance(fixed.dataflow.mustFetch.begin(), r), epochs)) return false;
    }
  }
  return true;
}

std::shared_ptr<const NativeFixedRoleAssignmentV1>
nativeFixedRoleAssignmentV1FromProjection(NativeSelectionProjectionV3 projection)
{
  // Cold-only proof of the full wire contract. This token grants no execution
  // authority; the Provider must still verify every current invocation.
  (void)nativeSelectionProjectionV3ToJson(projection);
  // The projection's public owner can originate from a mutable shared_ptr.
  // Freeze our own cold snapshot so a foreign alias cannot change its digest.
  auto role = std::make_shared<const NativeSelectionRoleV3>(projection.selectedRoleContract());
  auto providerByRole = projection.providerOwnership();
  if (providerByRole.empty()) providerByRole.emplace(role->selectedRole, projection.provider);
  NativeFixedExecutionTemplateV1 configuration{std::move(providerByRole), projection.plan.dependencies,
    projection.dataflow, projection.generationContract};
  collapseFixedRoleEpochs(configuration);
  auto executionTemplate = std::make_shared<const NativeFixedExecutionTemplateV1>(
    normalizeExecutionTemplate(std::move(configuration)));
  validateFixedTemplateProviderCover(*executionTemplate, role->selectedRole, projection.provider);
  const auto digest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-fixed-assignment-v1"}, {"provider", projection.provider},
    {"selected_role", role->selectedRole},
    {"role", jsonValue(*role)}, {"execution_template", fixedTemplateJson(*executionTemplate)}}));
  return std::make_shared<const NativeFixedRoleAssignmentV1>(
    NativeFixedRoleAssignmentV1(std::move(projection.provider), digest, std::move(role), std::move(executionTemplate)));
}

std::string nativeFixedRoleAssignmentV1ToJson(const NativeFixedRoleAssignmentV1& assignment)
{
  ++detail::fixedAssignmentWork().sourceEncodes;
  return nativeCanonicalJson(NativeJson{{"schema", "ndnsf-di-fixed-assignment-v1"},
    {"schema_version", 1}, {"provider", assignment.provider()},
    {"selected_role", assignment.roleOwner()->selectedRole},
    {"role", jsonValue(*assignment.roleOwner())},
    {"execution_template", fixedTemplateJson(*assignment.executionTemplate())}, {"digest", assignment.digest()}});
}

std::shared_ptr<const NativeFixedRoleAssignmentV1> nativeFixedRoleAssignmentV1FromJson(std::istream& input)
{
  ++detail::fixedAssignmentWork().sourceParses;
  const std::string wire{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (wire.size() > (4U << 20)) throw std::invalid_argument("fixed role source exceeds wire limit");
  const auto root = nativeParseJson(wire);
  if (!root.is_object() || root.size() != 7 ||
      root.at("schema").get<std::string>() != "ndnsf-di-fixed-assignment-v1" ||
      !root.at("schema_version").is_number_integer() || root.at("schema_version") != NativeJson(1))
    throw std::invalid_argument("fixed role source schema mismatch");
  const auto provider = root.at("provider").get<std::string>();
  const auto selected = root.at("selected_role").get<std::string>();
  if (provider.empty() || provider.front() != '/' || selected.empty())
    throw std::invalid_argument("fixed role source scope is incomplete");
  const auto& roleJson = root.at("role");
  auto role = selectionRoleFromV3Json(legacyTreeFromValue(roleJson), selected);
  restoreSelectionRoleShapes(role, roleJson);
  // Parsed positive integers are unsigned, while tensor dimensions are int64.
  // Compare canonical wire identity, retaining numeric versus symbolic types.
  if (nativeCanonicalJson(jsonValue(role)) != nativeCanonicalJson(roleJson))
    throw std::invalid_argument("fixed role source contains a noncanonical role contract");
  auto executionTemplate = std::make_shared<const NativeFixedExecutionTemplateV1>(
    fixedTemplateFromJson(root.at("execution_template")));
  validateFixedTemplateProviderCover(*executionTemplate, selected, provider);
  const auto digest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-fixed-assignment-v1"}, {"provider", provider},
    {"selected_role", selected}, {"role", jsonValue(role)},
    {"execution_template", fixedTemplateJson(*executionTemplate)}}));
  if (!root.at("digest").is_string() || root.at("digest").get<std::string>() != digest)
    throw std::invalid_argument("fixed role source digest mismatch");
  return std::make_shared<const NativeFixedRoleAssignmentV1>(
  NativeFixedRoleAssignmentV1(provider, digest, std::make_shared<const NativeSelectionRoleV3>(std::move(role)),
      std::move(executionTemplate)));
}

bool nativeSelectionRoleContractsEqualV3(
  const NativeSelectionRoleV3& left, const NativeSelectionRoleV3& right)
{
  return sameAssembly(left, right);
}

bool nativeFixedDependencyContractsEqualV1(const NativeDependencySpec& left,
                                         const NativeDependencySpec& right)
{
  return sameFixedDependency(left, right);
}

bool nativeSelectionRoleContractsEqualExceptCertificatesV3(const NativeSelectionRoleV3& left,
  const NativeSelectionRoleV3& right, bool ignoreManifest, bool ignoreRecipe)
{
  return sameAssembly(left, right, !ignoreManifest, !ignoreRecipe);
}

bool nativeFixedGenerationInterfacesEqualV1(const NativeGenerationExecutionContractV1& left,
                                          const NativeGenerationExecutionContractV1& right)
{
  return sameFixedGeneration(left, right);
}

namespace {
void validateFixedRoleSource(const NativeFixedRoleSourceV1& source)
{
  if (source.dataName.empty() || source.dataName.front() != '/' || source.dataName.size() > 4096 ||
      !isSha256Digest(source.plaintextDigest) || source.plaintextSize == 0 ||
      source.plaintextSize > (4U << 20))
    throw std::invalid_argument("fixed role source identity or bounds are invalid");
}
} // namespace

void nativeValidateFixedRoleSourceProducer(const NativeFixedRoleSourceV1& source,
  const std::string& requester, const std::string& role)
{
  validateFixedRoleSource(source);
  const ndn::Name producer(requester);
  const ndn::Name name(source.dataName);
  const auto inferred = ndn_service_framework::ProtectedBlobManifest::inferDataProducer(name);
  const auto offset = producer.size();
  if (producer.empty() || !inferred || *inferred != producer || name.size() != offset + 10 ||
      name[offset] != ndn::name::Component("NDNSF-DI") ||
      name[offset + 1] != ndn::name::Component("assignment") ||
      name[offset + 2] != ndn::name::Component("model") ||
      name[offset + 4] != ndn::name::Component("ROLE") ||
      !ndn_service_framework::matchesNamingLabelComponent(name[offset + 5], role) ||
      name[offset + 6] != ndn_service_framework::makeNamingDigestComponent(source.plaintextDigest.substr(7)))
    throw std::invalid_argument("fixed source Producer, role or plaintext name commitment mismatch");
}

ndn_service_framework::PublishedContentName nativeFixedRoleSourceContentName(
  const NativeSelectionProjectionV3& projection, const NativeFixedRoleAssignmentV1& fixed)
{
  if (!fixed.matchesExecutionTemplate(projection))
    throw std::invalid_argument("fixed source metadata differs from its execution owner");
  ndn_service_framework::PublishedContentName content;
  content.kind = ndn_service_framework::ContentDataKind::Assignment;
  content.label = fixed.roleOwner()->selectedRole;
  content.modelName = projection.plan.modelName;
  const auto& modelIdentity = fixed.roleOwner()->modelManifestDigest;
  if (modelIdentity.size() != 71 || modelIdentity.compare(0, 7, "sha256:") != 0)
    throw std::invalid_argument("fixed role source lacks verified model identity");
  content.modelIdentity = modelIdentity.substr(7);
  content.encode();
  return content;
}

std::string nativeSelectionInvocationV1ToJson(
  const NativeSelectionProjectionV3& projection,
  const NativeFixedRoleAssignmentV1& fixedAssignment,
  const NativeFixedRoleSourceV1* source)
{
  if (!fixedAssignment.matchesExecutionTemplate(projection))
    throw std::invalid_argument("invocation cannot reuse a different fixed execution template");
  validateNativeRoleDataflowContractV3(projection.dataflow);
  validateTensorNamingStrideContract(projection.dataflow, projection.generationContract);
  if (projection.dataflow.requestId != projection.requestId || projection.dataflow.attempt != projection.attempt ||
      projection.dataflow.planDigest != projection.planDigest)
    throw std::invalid_argument("invocation dataflow current identity mismatch");
  if (projection.provider != fixedAssignment.provider() ||
      !sameAssembly(projection.selectedRoleContract(), *fixedAssignment.roleOwner()) ||
      !sameAssembly(projection.assemblyContract(), *fixedAssignment.roleOwner())) {
    throw std::invalid_argument("invocation cannot reuse a different fixed role");
  }
  const auto& currentRole = projection.executionRole;
  const auto& cachedRole = fixedAssignment.executionRole();
  if (std::tie(currentRole.roleId, currentRole.stageId, currentRole.rank,
               currentRole.layerBegin, currentRole.layerEnd, currentRole.backend,
               currentRole.adapterId, currentRole.adapterVersion) !=
      std::tie(cachedRole.roleId, cachedRole.stageId, cachedRole.rank,
               cachedRole.layerBegin, cachedRole.layerEnd, cachedRole.backend,
               cachedRole.adapterId, cachedRole.adapterVersion))
    throw std::invalid_argument("invocation cannot reuse a different execution role");
  auto root = selectionInvocationFields(projection, true);
  root["schema"] = "ndnsf-di-selection-invocation-v1";
  root["schema_version"] = 1;
  root["fixed_role_ref"] = fixedAssignment.digest();
  if (source) {
    validateFixedRoleSource(*source);
    root["fixed_role_source"] = {{"data_name", source->dataName},
      {"plaintext_digest", source->plaintextDigest}, {"plaintext_size", source->plaintextSize}};
  }
  // No role expansion or typed role parse on this path.
  decodeSelectionProjectionV3(root, legacyTreeFromValue(root),
    projection.executionRole.roleId, nullptr, false, &fixedAssignment);
  return nativeCanonicalJson(root);
}

NativeSelectionProjectionV3 nativeSelectionInvocationV1FromJson(
  std::istream& input, const std::string& selectedRole,
  const NativeFixedRoleAssignmentV1& fixedAssignment)
{
  const std::string wire{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const auto root = nativeParseJson(wire);
  return decodeSelectionProjectionV3(root, legacyTreeFromValue(root), selectedRole,
    nullptr, false, &fixedAssignment);
}

NativeFixedRoleAssignmentCache::NativeFixedRoleAssignmentCache(
  std::size_t capacity, std::chrono::milliseconds retention)
  : m_capacity(capacity), m_retention(retention)
{
  if (capacity == 0 || capacity > 1024 || retention.count() <= 0 ||
      retention > std::chrono::hours(24)) {
    throw std::invalid_argument("fixed role cache capacity or retention is invalid");
  }
}

void NativeFixedRoleAssignmentCache::validateScope(
  const std::string& requester, const std::string& service)
{
  if (requester.empty() || requester.front() != '/' ||
      service.empty() || service.front() != '/') {
    throw std::invalid_argument("fixed role cache requires authenticated requester/service scope");
  }
}

void NativeFixedRoleAssignmentCache::pruneExpired(std::chrono::steady_clock::time_point now)
{
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (it->second.expires <= now) it = m_entries.erase(it);
    else ++it;
  }
}

std::shared_ptr<const NativeFixedRoleAssignmentV1> NativeFixedRoleAssignmentCache::find(
  const std::string& requester, const std::string& service,
  const std::string& provider, const std::string& role, const std::string& digest)
{
  validateScope(requester, service);
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto it = m_entries.find(Key{requester, service, provider, role});
  return it != m_entries.end() && it->second.assignment && it->second.assignment->digest() == digest
    ? it->second.assignment : nullptr;
}

std::shared_ptr<const NativeFixedRoleAssignmentV1> NativeFixedRoleAssignmentCache::findMatching(
  const std::string& requester, const std::string& service,
  const std::string& provider, const NativeSelectionRoleV3& role)
{
  validateScope(requester, service);
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto it = m_entries.find(Key{requester, service, provider, role.selectedRole});
  return it != m_entries.end() && it->second.assignment && sameAssembly(role, *it->second.assignment->roleOwner())
    ? it->second.assignment : nullptr;
}

std::shared_ptr<const NativeFixedRoleAssignmentV1> NativeFixedRoleAssignmentCache::findMatching(
  const std::string& requester, const std::string& service,
  const NativeSelectionProjectionV3& projection)
{
  validateScope(requester, service);
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto it = m_entries.find(Key{requester, service, projection.provider, projection.selectedRoleContract().selectedRole});
  return it != m_entries.end() && it->second.assignment && it->second.assignment->matchesExecutionTemplate(projection)
    ? it->second.assignment : nullptr;
}

std::shared_ptr<const NativeFixedRoleAssignmentV1> NativeFixedRoleAssignmentCache::remember(
  const std::string& requester, const std::string& service,
  std::shared_ptr<const NativeFixedRoleAssignmentV1> assignment)
{
  validateScope(requester, service);
  if (!assignment) throw std::invalid_argument("fixed role cache cannot install a null owner");
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto now = std::chrono::steady_clock::now();
  pruneExpired(now);
  const Key key{requester, service, assignment->provider(), assignment->roleOwner()->selectedRole};
  const auto existing = m_entries.find(key);
  if (existing != m_entries.end()) {
    if (existing->second.assignment && existing->second.assignment->digest() == assignment->digest())
      return existing->second.assignment;
    m_entries.erase(existing);
  }
  while (m_entries.size() >= m_capacity) {
    const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
      [](const auto& left, const auto& right) { return left.second.expires < right.second.expires; });
    m_entries.erase(oldest);
  }
  m_entries.emplace(key, Entry{assignment, now + m_retention, {}, {}});
  return assignment;
}

std::shared_ptr<const NativeFixedRoleSourceV1> NativeFixedRoleAssignmentCache::source(
  const std::string& requester, const std::string& service,
  const NativeFixedRoleAssignmentV1& assignment)
{
  validateScope(requester, service);
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto it = m_entries.find(Key{requester, service, assignment.provider(), assignment.roleOwner()->selectedRole});
  return it != m_entries.end() && it->second.assignment && it->second.assignment->digest() == assignment.digest()
    ? it->second.source : nullptr;
}

std::shared_ptr<const NativeFixedRoleSourceV1> NativeFixedRoleAssignmentCache::rememberSource(
  const std::string& requester, const std::string& service,
  const NativeFixedRoleAssignmentV1& assignment, NativeFixedRoleSourceV1 source)
{
  validateScope(requester, service);
  nativeValidateFixedRoleSourceProducer(source, requester, assignment.roleOwner()->selectedRole);
  auto published = std::make_shared<const NativeFixedRoleSourceV1>(std::move(source));
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto it = m_entries.find(Key{requester, service, assignment.provider(), assignment.roleOwner()->selectedRole});
  if (it == m_entries.end() || !it->second.assignment || it->second.assignment->digest() != assignment.digest()) {
    // A concurrent model change may replace the slot while publication runs.
    // The current operation still retains its own valid source serving pin.
    return published;
  }
  if (!it->second.source) it->second.source = published;
  return it->second.source;
}

std::shared_ptr<const NativeFixedPlacementCoreV1> NativeFixedRoleAssignmentCache::placementCore(
  const std::string& requester, const std::string& service)
{
  validateScope(requester, service);
  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(std::chrono::steady_clock::now());
  const auto found = m_entries.find(Key{requester, service, "", ""});
  return found == m_entries.end() ? nullptr : found->second.placementCore;
}

void NativeFixedRoleAssignmentCache::rememberPlacementCore(const std::string& requester,
  const std::string& service, std::shared_ptr<const NativeFixedPlacementCoreV1> owner)
{
  validateScope(requester, service);
  if (!owner) throw std::invalid_argument("fixed core cache cannot install a null owner");
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto now = std::chrono::steady_clock::now();
  pruneExpired(now);
  const Key key{requester, service, "", ""};
  const auto found = m_entries.find(key);
  if (found != m_entries.end() && found->second.placementCore == owner) return;
  m_entries.erase(key);
  while (m_entries.size() >= m_capacity) {
    const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
      [](const auto& left, const auto& right) { return left.second.expires < right.second.expires; });
    m_entries.erase(oldest);
  }
  m_entries.emplace(key, Entry{{}, now + m_retention, {}, std::move(owner)});
}

void NativeFixedRoleAssignmentCache::clear()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_entries.clear();
}

namespace {
void validateAssemblyStringEncoding(const std::string& text)
{
  if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x80; }))
    (void)nativeCanonicalJson(NativeJson(text));
}

void validateAssemblyEncoding(const NativeJson& value)
{
  const auto isAscii = [](const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; });
  };
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!isAscii(it.key())) (void)nativeCanonicalJson(NativeJson(it.key()));
      validateAssemblyEncoding(it.value());
    }
  }
  else if (value.is_array()) {
    for (const auto& item : value) validateAssemblyEncoding(item);
  }
  else if (value.is_string()) {
    if (!isAscii(value.get_ref<const NativeJson::string_t&>()))
      (void)nativeCanonicalJson(value);
  }
  else if (!value.is_null() && !value.is_boolean() &&
           !value.is_number_unsigned() && !value.is_number_integer()) {
    // Float conversion can fail under a non-default rounding mode; retain
    // the exact renderer rather than checking finiteness alone. Unsupported
    // JSON types likewise keep their original rejection.
    (void)nativeCanonicalJson(value);
  }
}

void validateAssemblyEncoding(const NativeSelectionRoleV3& role)
{
  const auto strings = [](std::initializer_list<const std::string*> fields) {
    for (const auto* field : fields) validateAssemblyStringEncoding(*field);
  };
  const auto tensors = [](const std::vector<NativeAssemblyTensorContractV3>& contracts) {
    for (const auto& tensor : contracts) {
      validateAssemblyStringEncoding(tensor.dtype);
      validateAssemblyStringEncoding(tensor.name);
      for (const auto& dimension : tensor.shape)
        std::visit([](const auto& value) {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>)
            validateAssemblyStringEncoding(value);
        }, dimension);
    }
  };
  // Keep the canonical field order and its UTF-8/float failures, without
  // allocating fixed node/interface arrays merely to validate their values.
  strings({&role.adapterDescriptorDigest, &role.adapterId, &role.adapterVersion,
    &role.artifactDigest, &role.artifactProfileDigest, &role.assemblerDescriptorDigest,
    &role.backend, &role.backendAbi, &role.canonicalInitializerDigest});
  for (const auto& device : role.deviceSet) validateAssemblyStringEncoding(device);
  tensors(role.expectedInputs);
  tensors(role.expectedOutputs);
  strings({&role.graphDigest, &role.layout, &role.mergeKind, &role.modelManifestDigest, &role.padding});
  (void)nativeCanonicalJson(NativeJson(role.postprocessConfidenceThreshold));
  strings({&role.postprocessIdentity, &role.postprocessOutputName, &role.postprocessSort,
    &role.precision, &role.protectionEpoch, &role.quantization, &role.recipeDigest, &role.role, &role.roleKind});
}
} // namespace

void validateNativeAssembly(const NativeSelectionRoleV3& role)
{
  // Custom numeric locale facets can change the legacy integer conversions.
  // Preserve that path; typed canonical integers round trip in classic locale.
  if (std::locale() != std::locale::classic()) {
    const auto value = nativeAssemblyJson(role);
    validateAssemblyEncoding(value);
    selectionRoleFromV3Json(legacyTreeFromValue(value), role.selectedRole);
    return;
  }
  validateAssemblyEncoding(role);
  for (const auto& tensor : role.expectedInputs) validateAssemblyTensorContract(tensor);
  for (const auto& tensor : role.expectedOutputs) validateAssemblyTensorContract(tensor);
  // Keep Boost's original conversion and default-zero behavior for this float.
  const auto confidence = legacyTreeFromValue(NativeJson(role.postprocessConfidenceThreshold))
    .get_value<double>(0.0);
  validateSelectionRoleFields(role, confidence);
}

void
validateNativeSelectionProjectionSetV3(
  const std::vector<NativeSelectionProjectionV3>& projections)
{
  if (projections.empty()) {
    throw std::invalid_argument("V3 projection set is empty");
  }
  const auto& first = projections.front();
  std::set<std::string> roles;
  using Publisher = std::pair<const NativeTensorEndpointV3*,
                              const NativeGenerationExecutionContractV1*>;
  std::map<std::string, std::vector<Publisher>> publishers;
  std::map<std::string, std::set<std::string>> edges;
  std::map<std::string, std::size_t> incoming;
  std::size_t terminalOwners = 0;

  for (const auto& projection : projections) {
    const auto& role = projection.executionRole.roleId;
    if (projection.requestId != first.requestId ||
        projection.attempt != first.attempt ||
        projection.planDigest != first.planDigest ||
        !roles.insert(role).second ||
        projection.selectedRoleContract().selectedRole != role ||
        projection.dataflow.role != role) {
      throw std::invalid_argument(
        "V3 projection set violates one-projection-per-role ownership");
    }
    incoming.emplace(role, 0);
    terminalOwners += projection.dataflow.terminalResponseOwner ? 1 : 0;
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      publishers[endpoint.endpointDigest].emplace_back(&endpoint, &projection.generationContract);
    }
  }
  if (terminalOwners != 1) {
    throw std::invalid_argument(
      "V3 projection set must have exactly one terminal Response owner");
  }

  for (const auto& projection : projections) {
    const auto& consumer = projection.executionRole.roleId;
    for (const auto& endpoint : projection.dataflow.mustFetch) {
      if (endpoint.sourceKind == "APPLICATION_INPUT") {
        continue;
      }
      const auto found = publishers.find(endpoint.endpointDigest);
      if (found == publishers.end() || found->second.size() != 1 ||
          endpoint.producerRole.empty() ||
          roles.count(endpoint.producerRole) == 0 ||
          !sameTensorEndpoint(endpoint, *found->second.front().first)) {
        throw std::invalid_argument(
          "V3 mustFetch has no exact single mayPublish owner");
      }
      if (endpoint.operation == "TOKEN_FEEDBACK") {
        // Feedback is still signed and has one exact publisher above. It
        // belongs to the next epoch, not the initial readiness DAG.
        const auto& generation = projection.generationContract;
        const auto& producerGeneration = *found->second.front().second;
        if (nativeGenerationJson(generation) != nativeGenerationJson(producerGeneration) ||
            !generation.enabled || !generation.streamingOperationStride ||
            generation.streamingOperationStride > (1U << 20) ||
            endpoint.namingOperationStride != generation.streamingOperationStride ||
            endpoint.round / generation.streamingOperationStride > generation.maxGeneratedTokens)
          throw std::invalid_argument("V3 feedback endpoint differs from its signed generation");
        continue;
      }
      if (edges[endpoint.producerRole].insert(consumer).second) {
        ++incoming[consumer];
      }
    }
  }

  std::vector<std::string> ready;
  for (const auto& item : incoming) {
    if (item.second == 0) {
      ready.push_back(item.first);
    }
  }
  std::sort(ready.begin(), ready.end());
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto role = ready.front();
    ready.erase(ready.begin());
    ++visited;
    for (const auto& consumer : edges[role]) {
      auto& degree = incoming[consumer];
      if (--degree == 0) {
        ready.push_back(consumer);
        std::sort(ready.begin(), ready.end());
      }
    }
  }
  if (visited != roles.size()) {
    throw std::invalid_argument("V3 role dataflow contains a cycle");
  }
}

namespace {

RoleSpec
roleSpecFromSelectionProjectionV3Impl(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider,
  std::uint64_t sequence,
  bool validateGenerationContract)
{
  const auto& dataflow = projection.dataflow;
  const auto& generation = projection.generationContract;
  if ((!generation.enabled && sequence != 0) ||
      (generation.enabled && (generation.streamingOperationStride == 0 ||
       generation.maxGeneratedTokens == 0 || generation.maxGeneratedTokens > MAX_NATIVE_GENERATED_TOKENS ||
       sequence > generation.maxGeneratedTokens))) {
    throw std::invalid_argument("V3 generation sequence exceeds sealed bounds");
  }
  if (generation.enabled) {
    const auto verifyEpochs = [&](const std::vector<NativeTensorEndpointV3>& endpoints) {
      std::map<std::string, std::set<std::uint64_t>> epochsByTransfer;
      for (const auto& endpoint : endpoints) {
        if (endpoint.sourceKind != "ROLE") continue;
        const auto epoch = endpoint.round / generation.streamingOperationStride;
        auto base = endpoint;
        base.round %= generation.streamingOperationStride;
        base.endpointDigest.clear();
        base.manifestDigest.clear();
        // Sizing can vary by epoch (prefill versus decode); it is advisory,
        // while the signed transfer identity and complete epoch cover stay fixed.
        base.prefetchBundleBytes = 0;
        base.prefetchSegmentBytes = 0;
        const auto key = nativeCanonicalJson(nativeEndpointJson(base));
        if (epoch > generation.maxGeneratedTokens ||
            !epochsByTransfer[key].insert(epoch).second)
          throw std::invalid_argument("V3 generation endpoint epoch is repeated or outside bounds");
      }
      for (const auto& transfer : epochsByTransfer)
        if (transfer.second.size() != generation.maxGeneratedTokens + 1)
          throw std::invalid_argument("V3 generation endpoint epochs are incomplete");
    };
    if (validateGenerationContract) {
      verifyEpochs(dataflow.mayPublish);
      verifyEpochs(dataflow.mustFetch);
    }
  }
  const auto atSequence = [&](const NativeTensorEndpointV3& endpoint) {
    if (!generation.enabled) return true;
    if (endpoint.sourceKind == "APPLICATION_INPUT") return sequence == 0;
    return endpoint.round / generation.streamingOperationStride == sequence;
  };
  if (dataflow.role.empty() ||
      dataflow.role != projection.executionRole.roleId ||
      dataflow.requestId != projection.requestId ||
      dataflow.attempt != projection.attempt ||
      dataflow.planDigest != projection.planDigest ||
      (!localProvider.empty() && projection.provider != localProvider)) {
    throw std::invalid_argument(
      "V3 Selection dataflow cannot be projected for this Provider");
  }

  // A runtime scope is the key used by AsyncDataflowRuntime to retain one
  // reconstructed TensorBundle.  A normal PIPELINE dependency may carry
  // several named tensors (for example the two YOLO DetectShard1 inputs),
  // while a redistribution deliberately groups producer-rank bundles under
  // one transport scope for the certified GATHER/SCATTER transition.
  // Count both sides of the local projection so producer and consumer derive
  // the same scope; counting mustFetch alone would leave a multi-output
  // producer on the unsuffixed scope.
  std::map<std::string, std::set<std::string>> endpointKeysByGroup;
  for (const auto* endpoints : {&dataflow.mayPublish, &dataflow.mustFetch}) {
    for (const auto& endpoint : *endpoints) {
      endpointKeysByGroup[endpoint.groupId].insert(
        endpoint.producerRole + "\x1f" + endpoint.tensorId);
    }
  }
  const auto hasRedistribution = [&projection] (const std::string& groupId) {
    return std::any_of(
      projection.plan.dependencies.begin(), projection.plan.dependencies.end(),
      [&groupId] (const NativeDependencySpec& dependency) {
        return dependency.keyScope == groupId &&
               !dependency.redistributions.empty();
      });
  };
  const auto runtimeScopeFor = [&] (const NativeTensorEndpointV3& endpoint) {
    if (hasRedistribution(endpoint.groupId)) {
      if (endpointKeysByGroup[endpoint.groupId].size() > 1) {
        auto producerRole = endpoint.producerRole;
        while (!producerRole.empty() && producerRole.front() == '/') {
          producerRole.erase(producerRole.begin());
        }
        return endpoint.groupId + "/from/" + producerRole;
      }
      return endpoint.groupId;
    }
    if (endpointKeysByGroup[endpoint.groupId].size() <= 1) {
      return endpoint.groupId;
    }
    auto producerRole = endpoint.producerRole;
    while (!producerRole.empty() && producerRole.front() == '/') {
      producerRole.erase(producerRole.begin());
    }
    return endpoint.groupId + "/from/" + producerRole +
           "/tensor/" + endpoint.tensorId;
  };

  auto makeEdge = [&projection, &dataflow, &localProvider, &runtimeScopeFor](
                    const NativeTensorEndpointV3& endpoint,
                    bool output) {
    if (endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.securityProfile != "NDNSF_DATA_V1" ||
        (output && endpoint.producerRole != dataflow.role) ||
        (!output && endpoint.consumerRole != dataflow.role)) {
      throw std::invalid_argument(
        "V3 tensor endpoint is outside the local role authority");
    }
    const auto expectedStride = endpoint.sourceKind == "ROLE" && projection.generationContract.enabled
      ? projection.generationContract.streamingOperationStride : 0;
    if (endpoint.namingOperationStride != expectedStride)
      throw std::invalid_argument("runtime tensor naming stride differs from sealed generation");
    DependencyEdge edge;
    // Separate local tensor storage scopes without changing the transport
    // authorization group. tensorId remains the adapter tensor identity.
    edge.scope = runtimeScopeFor(endpoint);
    edge.producerRole = endpoint.producerRole;
    edge.consumerRole = endpoint.consumerRole;
    edge.consumerRoles = endpoint.consumerRoles;
    edge.plannedDataName = tensorObjectNamePrefix(endpoint);
    edge.expectedSegments = 0;
    edge.expectedBytes = 0;
    edge.prefetchBundleBytes = endpoint.prefetchBundleBytes;
    edge.prefetchSegmentBytes = endpoint.prefetchSegmentBytes;
    edge.inlineFirstSegment = endpoint.inlineFirstSegment;
    edge.tensors = {endpoint.tensorId};
    edge.bundleTensorNames = endpoint.bundleTensorNames;
    edge.requestId = projection.requestId;
    edge.attemptEpoch = projection.attempt;
    edge.useNdnsfDataV1 = true;
    edge.collectiveOperationIndex = endpoint.round;
    edge.collectiveProducerRank = std::to_string(endpoint.producerRank);
    edge.collectiveSourceLayoutDigest = endpoint.layoutDigest;
    edge.collectiveTargetLayoutDigest = endpoint.targetLayoutDigest;
    edge.collectiveTensorDigest = endpoint.tensorDigest;
    edge.transportScope = endpoint.groupId;
    edge.producerProvider = output ?
      (localProvider.empty() ? projection.provider : localProvider) :
      endpoint.producerNamespace;
    edge.topicPrefix = "/tensor";
    edge.declaredByV3 = true;
    edge.manifestDataName = tensorObjectManifestName(endpoint);
    edge.maxSegments = endpoint.segmentCount;
    edge.endpointDigest = endpoint.endpointDigest;
    edge.executionObjectDigest = computeTensorExecutionObjectDigest(endpoint);
    edge.planDigest = endpoint.planDigest;
    edge.manifestContractDigest = endpoint.manifestDigest;
    edge.tensorDigest = endpoint.tensorDigest;
    edge.layoutDigest = endpoint.layoutDigest;
    edge.securityProfile = endpoint.securityProfile;
    edge.operationKind = endpoint.operation;
    edge.round = endpoint.round;
    edge.namingOperationStride = endpoint.namingOperationStride;
    edge.microbatch = endpoint.microbatch;
    edge.noProgressDeadlineMs = endpoint.noProgressDeadlineMs;
    edge.hardDeadlineMs = endpoint.hardDeadlineMs;
    for (const auto& dependency : projection.plan.dependencies) {
      if (dependency.keyScope != endpoint.groupId ||
          std::find(dependency.producers.begin(), dependency.producers.end(),
                    endpoint.producerRole) == dependency.producers.end() ||
          std::find(dependency.consumers.begin(), dependency.consumers.end(),
                    endpoint.consumerRole) == dependency.consumers.end()) {
        continue;
      }
      edge.redistributions = dependency.redistributions;
      if (!edge.redistributions.empty()) {
        const auto& redistribution = edge.redistributions.front();
        const auto producerIndex = static_cast<std::size_t>(
          std::distance(dependency.producers.begin(),
                        std::find(dependency.producers.begin(),
                                  dependency.producers.end(),
                                  endpoint.producerRole)));
        const auto consumerIndex = static_cast<std::size_t>(
          std::distance(dependency.consumers.begin(),
                        std::find(dependency.consumers.begin(),
                                  dependency.consumers.end(),
                                  endpoint.consumerRole)));
        if (producerIndex < redistribution.producerRanks.size()) {
          edge.redistributionProducerRank =
            redistribution.producerRanks[producerIndex];
        }
        if (consumerIndex < redistribution.consumerRanks.size()) {
          edge.redistributionConsumerRank =
            redistribution.consumerRanks[consumerIndex];
        }
      }
      break;
    }
    return edge;
  };

  RoleSpec spec;
  spec.role = dataflow.role;
  spec.requestId = projection.requestId;
  spec.attemptEpoch = projection.attempt;
  spec.outputs.reserve(dataflow.mayPublish.size());
  for (const auto& endpoint : dataflow.mayPublish) {
    if (atSequence(endpoint)) spec.outputs.push_back(makeEdge(endpoint, true));
  }
  spec.inputs.reserve(dataflow.mustFetch.size());
  for (const auto& endpoint : dataflow.mustFetch) {
    if (atSequence(endpoint)) spec.inputs.push_back(makeEdge(endpoint, false));
  }
  return spec;
}

} // namespace

RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider)
{
  return roleSpecFromSelectionProjectionV3(projection, localProvider, 0);
}

RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider,
  std::uint64_t sequence)
{
  return roleSpecFromSelectionProjectionV3Impl(
    projection, localProvider, sequence, true);
}

struct NativeSelectionProjectionRoleFactory::State
{
  NativeSelectionProjectionV3 projection;
  std::string localProvider;
  mutable std::mutex mutex;
  bool validated = false;
};

NativeSelectionProjectionRoleFactory::NativeSelectionProjectionRoleFactory(
  NativeSelectionProjectionV3 projection,
  std::string localProvider)
  : m_state(std::make_shared<State>())
{
  m_state->projection = std::move(projection);
  m_state->localProvider = std::move(localProvider);
}

RoleSpec
NativeSelectionProjectionRoleFactory::operator()(std::uint64_t sequence) const
{
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (!m_state->validated) {
    auto first = roleSpecFromSelectionProjectionV3Impl(
      m_state->projection, m_state->localProvider, 0, true);
    m_state->validated = true;
    if (sequence == 0) {
      return first;
    }
  }
  return roleSpecFromSelectionProjectionV3Impl(
    m_state->projection, m_state->localProvider, sequence, false);
}

} // namespace ndnsf::di
