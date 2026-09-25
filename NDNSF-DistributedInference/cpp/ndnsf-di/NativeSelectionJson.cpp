#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <sstream>

namespace ndnsf::di {
namespace {
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

NativeJson jsonValue(const NativeSelectionRoleV3& value)
{
  auto result = NativeJson::object();
  result["role"] = value.role;
  result["rank"] = value.rank;
  result["layer_begin"] = value.layerBegin;
  result["layer_end"] = value.layerEnd;
  result["backend"] = value.backend;
  result["device_set"] = value.deviceSet;
  result["required_device_memory_mb"] = value.requiredDeviceMemoryMb;
  result["artifact_digest"] = value.artifactDigest;
  result["recipe_digest"] = value.recipeDigest;
  result["role_kind"] = value.roleKind;
  result["adapter_id"] = value.adapterId;
  result["adapter_version"] = value.adapterVersion;
  result["model_manifest_digest"] = value.modelManifestDigest;
  result["artifact_profile_digest"] = value.artifactProfileDigest;
  result["graph_digest"] = value.graphDigest;
  result["canonical_initializer_digest"] = value.canonicalInitializerDigest;
  result["adapter_descriptor_digest"] = value.adapterDescriptorDigest;
  result["assembler_descriptor_digest"] = value.assemblerDescriptorDigest;
  result["backend_abi"] = value.backendAbi;
  result["node_indices"] = value.nodeIndices;
  result["expected_inputs"] = jsonArray(value.expectedInputs);
  result["expected_outputs"] = jsonArray(value.expectedOutputs);
  result["precision"] = value.precision;
  result["quantization"] = value.quantization;
  result["layout"] = value.layout;
  result["padding"] = value.padding;
  result["protection_epoch"] = value.protectionEpoch;
  result["merge_kind"] = value.mergeKind;
  result["postprocess_identity"] = value.postprocessIdentity;
  result["postprocess_output_name"] = value.postprocessOutputName;
  result["postprocess_confidence_threshold"] = value.postprocessConfidenceThreshold;
  result["postprocess_sort"] = value.postprocessSort;
  result["resource_envelope"] = NativeJson::object();
  if (value.maxSourceBytes || value.maxAssembledBytes || value.maxNodes) {
    result["resource_envelope"] = {{"maxSourceBytes", value.maxSourceBytes},
      {"maxAssembledBytes", value.maxAssembledBytes}, {"maxNodes", value.maxNodes}};
  }
  return result;
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
  result["manifest_digest"] = value.manifestDigest;
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
NativeJson nativeDependenciesJson(const std::vector<NativeDependencySpec>& values) { return jsonArray(values); }
NativeJson nativeGenerationJson(const NativeGenerationExecutionContractV1& value) { return jsonValue(value); }
NativeJson nativeEndpointJson(const NativeTensorEndpointV3& value) { return jsonValue(value); }
NativeJson nativeDataflowJson(const NativeRoleDataflowContractV3& value) { return jsonValue(value); }
NativeJson nativeDeviceBindingJson(const NativeDeviceBindingV3& value) { return jsonValue(value); }

std::string nativeSelectionProjectionV3ToJson(const NativeSelectionProjectionV3& value)
{
  NativeJson root = {
    {"schema", "ndnsf-di-selection-v3"}, {"schema_version", 3},
    {"provider", value.provider}, {"request_id", value.requestId}, {"attempt", value.attempt},
    {"plan_core_digest", value.planCoreDigest}, {"plan_digest", value.planDigest},
    {"ack_closed_digest", value.ackClosedDigest}, {"offer_digest", value.offerDigest},
    {"security_policy_snapshot_digest", value.securityPolicySnapshotDigest},
    {"deadline_ms", value.deadlineMs}, {"group_capability_v1", value.groupCapabilityV1},
    {"roles", NativeJson::array({jsonValue(value.selectedRole)})},
    {"assembly", jsonValue(value.assembly)}, {"execution_role", jsonValue(value.executionRole)},
    {"dataflow", jsonValue(value.dataflow)}, {"device_binding", jsonValue(value.deviceBinding)},
    {"dependencies", jsonArray(value.plan.dependencies)},
  };
  if (!value.requestContractDigest.empty()) root["request_contract_digest"] = value.requestContractDigest;
  if (value.generationContract.enabled) root["generation_contract"] = jsonValue(value.generationContract);
  if (value.conversationStateReference) root["conversation_state_reference"] = jsonValue(*value.conversationStateReference);
  if (value.conversationTurnBinding) root["conversation_turn_binding"] = jsonValue(*value.conversationTurnBinding);
  if (value.hasGrantBinding) {
    root["grant_binding"] = {{"provider", value.provider}, {"request_id", value.requestId},
      {"attempt", value.attempt}, {"plan_core_digest", value.planCoreDigest},
      {"security_policy_snapshot_digest", value.securityPolicySnapshotDigest},
      {"protection_epoch", value.selectedRole.protectionEpoch},
      {"grant_name", value.grantName}, {"grant_digest", value.grantDigest}};
    if (value.grantLeaseScope)
      root["grant_binding"]["lease_scope"] = jsonValue(*value.grantLeaseScope);
  }
  const auto wire = nativeCanonicalJson(root);
  // The existing production parser owns semantic validation, including the
  // complete role, grant and endpoint binding checks. Never emit a fragment.
  std::istringstream input(wire);
  nativeSelectionProjectionV3FromJson(input, value.executionRole.roleId);
  return wire;
}

} // namespace ndnsf::di
