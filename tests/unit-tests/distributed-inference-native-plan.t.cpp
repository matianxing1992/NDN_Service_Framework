#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di::test {

namespace {

std::string
digest(char value)
{
  return "sha256:" + std::string(64, value);
}

std::string
roleJson(const std::string& digestValue)
{
  return std::string("{\"adapter_id\":\"onnx\",") +
         "\"adapter_version\":\"1\",\"artifact_digest\":\"" +
         digestValue +
         "\",\"backend\":\"onnxruntime-cpu\",\"device_set\":[]," +
         "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
         "\"protection_epoch\":\"plaintext-v1\"," +
         "\"required_device_memory_mb\":0," +
         "\"recipe_digest\":\"" + digestValue +
         "\",\"role\":\"S0R0\",\"role_kind\":\"PIPELINE_RANGE\"}";
}

std::string
certifiedRoleJson(const std::string& digestValue)
{
  auto value = roleJson(digestValue);
  if (value.empty() || value.back() != '}') {
    throw std::logic_error("role fixture is malformed");
  }
  value.pop_back();
  value += std::string(",\"model_manifest_digest\":\"") + digest('b') +
    "\",\"artifact_profile_digest\":\"" + digest('c') +
    "\",\"graph_digest\":\"" + digest('d') +
    "\",\"canonical_initializer_digest\":\"" + digest('e') +
    "\",\"adapter_descriptor_digest\":\"" + digest('f') +
    "\",\"assembler_descriptor_digest\":\"" + digest('1') +
    "\",\"backend_abi\":\"onnxruntime-1.26-cpu\"" +
    ",\"node_indices\":[0,1,2,3]" +
    ",\"expected_inputs\":[{\"name\":\"x\",\"dtype\":\"float32\"," +
    "\"shape\":[1,4]}]" +
    ",\"expected_outputs\":[{\"name\":\"y\",\"dtype\":\"float32\"," +
    "\"shape\":[1,4]}]" +
    ",\"precision\":\"fp32\",\"quantization\":\"none\"," +
    "\"layout\":\"native\",\"padding\":\"none\"," +
    "\"resource_envelope\":{\"maxSourceBytes\":4096," +
    "\"maxAssembledBytes\":2048,\"maxNodes\":4}}";
  return value;
}

std::string
projectionJson(std::uint64_t dataflowAttempt = 1,
               std::string dataflowPlanDigest = {},
               std::string deviceMode = "CPU",
               std::string deviceHandle = {},
               bool duplicateLocalRole = false,
               bool certifiedAssembly = false)
{
  const auto d = digest('a');
  if (dataflowPlanDigest.empty()) {
    dataflowPlanDigest = d;
  }
  const auto role = certifiedAssembly ? certifiedRoleJson(d) : roleJson(d);
  return std::string("{\"ack_closed_digest\":\"") + d +
    "\",\"assembly\":" + role +
    ",\"attempt\":1,\"dataflow\":{" +
    "\"attempt\":" + std::to_string(dataflowAttempt) +
    ",\"dataflow_digest\":\"" + d +
    "\",\"may_publish\":[],\"must_fetch\":[],\"plan_digest\":\"" +
    dataflowPlanDigest +
    "\",\"request_id\":\"/request/1\",\"role\":\"S0R0\"," +
    "\"terminal_response_owner\":true,\"wait_for\":[]}," +
    "\"deadline_ms\":120000,\"dependencies\":[],\"device_binding\":{" +
    "\"mode\":\"" + deviceMode + "\",\"offer_digest\":\"" + d +
    "\",\"offer_scoped_device_handle\":\"" + deviceHandle +
    "\",\"provider\":\"/provider/0\",\"resource_sequence\":1," +
    "\"resource_snapshot_digest\":\"" + d +
    "\",\"role\":\"S0R0\",\"sharing_policy\":\"EXCLUSIVE_ROLE\"," +
    "\"topology_profile_digest\":\"" + d + "\"}," +
    "\"execution_role\":{\"adapter_id\":\"onnx\"," +
    "\"adapter_version\":\"1\",\"backend\":\"onnxruntime-cpu\"," +
    "\"layer_begin\":0,\"layer_end\":4,\"rank\":0," +
    "\"role_id\":\"S0R0\",\"stage_id\":\"S0R0\"}," +
    "\"group_capability_v1\":\"\",\"offer_digest\":\"" + d +
    "\",\"plan_core_digest\":\"" + d +
    "\",\"plan_digest\":\"" + d +
    "\",\"provider\":\"/provider/0\",\"request_id\":\"/request/1\"," +
    "\"request_contract_digest\":\"" + digest('9') + "\"," +
    "\"roles\":[" + role + (duplicateLocalRole ? "," + role : "") +
    "],\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3," +
    "\"security_policy_snapshot_digest\":\"" + d + "\"}";
}

NativeSelectionProjectionV3
parseProjection(const std::string& wire)
{
  std::istringstream input(wire);
  return nativeSelectionProjectionV3FromJson(input, "S0R0");
}

std::string
streamingProjectionJson()
{
  auto wire = projectionJson();
  const auto dependencies = wire.find("\"dependencies\":[]");
  if (dependencies == std::string::npos) {
    throw std::logic_error("projection fixture has no dependencies field");
  }
  wire.replace(
    dependencies, std::string("\"dependencies\":[]").size(),
    "\"dependencies\":[{\"producers\":[\"S0R0\"],"
    "\"consumers\":[\"S0R0\"],\"key_scope\":\"token-feedback\","
    "\"topic_prefix\":\"/feedback\",\"object_name_template\":"
    "\"{producerProvider}/{sessionId}/{sequence}\","
    "\"tensors\":[\"input_ids\"],\"operationKind\":\"TOKEN_FEEDBACK\","
    "\"transportProfile\":\"NDNSF_DATA_V1\","
    "\"collectiveOperationIndex\":0}]");
  const auto insertion = wire.find("\"group_capability_v1\"");
  if (insertion == std::string::npos) {
    throw std::logic_error("projection fixture has no capability field");
  }
  wire.insert(
    insertion,
    "\"generation_contract\":{\"mode\":\"TOKEN_STREAMING\","
    "\"max_generated_tokens\":8,\"token_input_name\":\"input_ids\","
    "\"state_input_names\":[\"attention_kv_in\",\"recurrent_state_in\","
    "\"convolution_state_in\"],\"state_output_names\":["
    "\"attention_kv_out\",\"recurrent_state_out\","
    "\"convolution_state_out\"],\"eos_token_ids\":[2],"
    "\"sampling_digest\":\"" + digest('7') + "\","
    "\"tokenizer_digest\":\"" + digest('8') + "\","
    "\"committed_prefix_token_ids\":[],"
    "\"streaming_operation_stride\":1},");
  return wire;
}

std::string
conversationProjectionJson()
{
  auto wire = projectionJson();
  const auto insertion = wire.find("\"group_capability_v1\"");
  if (insertion == std::string::npos) {
    throw std::logic_error("projection fixture has no capability field");
  }
  wire.insert(
    insertion,
    "\"conversation_turn_binding\":{"
    "\"schema\":\"ndnsf-di-conversation-turn-binding-v1\","
    "\"version\":1,\"conversation_id\":\"0123456789abcdef0123456789abcdef\","
    "\"parent_context_epoch\":2,\"successor_context_epoch\":3,"
    "\"service_name\":\"/LLM/Qwen\","
    "\"plan_role_map_digest\":\"" + digest('2') + "\","
    "\"request_contract_digest\":\"" + digest('9') + "\","
    "\"retention_deadline_ms\":120000,"
    "\"parent_checkpoint_digest\":\"" + digest('3') + "\"},"
    "\"conversation_state_reference\":{"
    "\"schema\":\"ndnsf-di-conversation-state-reference-v1\","
    "\"version\":1,\"conversation_id\":\"0123456789abcdef0123456789abcdef\","
    "\"context_epoch\":2,\"service_name\":\"/LLM/Qwen\","
    "\"plan_role_map_digest\":\"" + digest('2') + "\","
    "\"checkpoint_digest\":\"" + digest('3') + "\","
    "\"role_name\":\"S0R0\",\"role_receipt_digest\":\"" +
      digest('4') + "\",\"expires_at_ms\":120000},");
  return wire;
}

NativeTensorEndpointV3
endpoint(std::string producer,
         std::string consumer,
         std::string endpointDigest)
{
  NativeTensorEndpointV3 value;
  value.producerNamespace = "/provider/0";
  value.requester = "/requester";
  value.requestId = "/request/1";
  value.attempt = 1;
  value.planDigest = digest('b');
  value.groupId = "group-0";
  value.groupEpoch = "epoch-0";
  value.operation = "PIPELINE";
  value.sourceKind = "ROLE";
  value.producerRole = std::move(producer);
  value.consumerRole = std::move(consumer);
  value.tensorId = "activation";
  value.tensorDigest = digest('c');
  value.layoutDigest = digest('d');
  value.targetLayoutDigest = value.layoutDigest;
  value.segmentCount = 1;
  value.manifestDigest = digest('e');
  value.securityProfile = "NDNSF_DATA_V1";
  value.noProgressDeadlineMs = 1000;
  value.hardDeadlineMs = 5000;
  value.endpointDigest = std::move(endpointDigest);
  return value;
}

NativeSelectionProjectionV3
projection(std::string provider, std::string role, bool terminal)
{
  NativeSelectionProjectionV3 value;
  value.provider = std::move(provider);
  value.requestId = "/request/1";
  value.attempt = 1;
  value.planCoreDigest = digest('a');
  value.planDigest = digest('b');
  value.ackClosedDigest = digest('c');
  value.offerDigest = digest('d');
  value.securityPolicySnapshotDigest = digest('e');
  value.deadlineMs = 5000;
  value.selectedRole.role = role;
  value.selectedRole.selectedRole = role;
  value.executionRole.roleId = role;
  value.dataflow.requestId = value.requestId;
  value.dataflow.attempt = value.attempt;
  value.dataflow.planDigest = value.planDigest;
  value.dataflow.role = role;
  value.dataflow.terminalResponseOwner = terminal;
  value.deviceBinding.provider = value.provider;
  value.deviceBinding.role = role;
  return value;
}

std::vector<NativeSelectionProjectionV3>
validProjectionSet()
{
  auto source = projection("/provider/0", "S0R0", false);
  auto sink = projection("/provider/1", "S1R0", true);
  const auto edge = endpoint("S0R0", "S1R0", digest('f'));
  source.dataflow.mayPublish.push_back(edge);
  sink.dataflow.mustFetch.push_back(edge);
  return {source, sink};
}

} // namespace

BOOST_AUTO_TEST_CASE(NativeV3ProjectionDecodesCompleteSingleRoleContract)
{
  const auto value = parseProjection(projectionJson());
  BOOST_CHECK_EQUAL(value.selectedRole.selectedRole, "S0R0");
  BOOST_CHECK_EQUAL(value.executionRole.roleId, "S0R0");
  BOOST_CHECK_EQUAL(value.dataflow.role, "S0R0");
  BOOST_CHECK(value.dataflow.terminalResponseOwner);
  BOOST_CHECK_EQUAL(value.deviceBinding.mode, "CPU");
  BOOST_CHECK_EQUAL(value.requestContractDigest, digest('9'));
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionParsesAndBindsNativePostprocess)
{
  auto wire = projectionJson(1, {}, "CPU", {}, false, true);
  const std::vector<std::pair<std::string, std::string>> replacements{
    {"\"layer_end\":4", "\"layer_end\":0"},
    {"\"role_kind\":\"PIPELINE_RANGE\"", "\"role_kind\":\"COMPONENT_SET\""},
    {"\"protection_epoch\":\"plaintext-v1\"",
     "\"protection_epoch\":\"plaintext-v1\",\"merge_kind\":\"NATIVE_POSTPROCESS\","
     "\"postprocess_identity\":\"YOLO26n-canonical-detection-rows\","
     "\"postprocess_output_name\":\"y\","
     "\"postprocess_confidence_threshold\":0.001,"
     "\"postprocess_sort\":\"confidence-desc,class-asc,xyxy-asc\""},
  };
  for (const auto& [from, to] : replacements) {
    BOOST_REQUIRE_NE(wire.find(from), std::string::npos);
    for (auto pos = wire.find(from); pos != std::string::npos;
         pos = wire.find(from, pos + to.size())) {
      wire.replace(pos, from.size(), to);
    }
  }
  NativeSelectionProjectionV3 value;
  BOOST_REQUIRE_NO_THROW(value = parseProjection(wire));
  BOOST_CHECK_EQUAL(value.assembly.roleKind, "COMPONENT_SET");
  BOOST_CHECK_EQUAL(value.executionRole.layerEnd, 0);
  BOOST_CHECK_EQUAL(value.assembly.mergeKind, "NATIVE_POSTPROCESS");
  BOOST_CHECK_EQUAL(value.assembly.postprocessOutputName, "y");
  BOOST_CHECK_EQUAL(value.assembly.postprocessConfidenceThreshold, .001);

  auto modelBound = wire;
  const std::string epoch = "\"protection_epoch\":\"plaintext-v1\"";
  const std::string bound = epoch +
    ",\"model_manifest_digest\":\"" + digest('m') + "\"";
  BOOST_REQUIRE_NE(modelBound.find(epoch), std::string::npos);
  for (auto pos = modelBound.find(epoch); pos != std::string::npos;
       pos = modelBound.find(epoch, pos + bound.size())) {
    modelBound.replace(pos, epoch.size(), bound);
  }
  BOOST_CHECK_NO_THROW(parseProjection(modelBound));

  auto mismatched = wire;
  const std::string outputName = "\"postprocess_output_name\":\"y\"";
  const auto output = mismatched.find(outputName);
  BOOST_REQUIRE_NE(output, std::string::npos);
  mismatched.replace(output, outputName.size(),
                     "\"postprocess_output_name\":\"other\"");
  BOOST_CHECK_THROW(parseProjection(mismatched), std::invalid_argument);

  const std::string threshold = "\"postprocess_confidence_threshold\":0.001";
  const std::string invalid = "\"postprocess_confidence_threshold\":2.0";
  for (auto pos = wire.find(threshold); pos != std::string::npos;
       pos = wire.find(threshold, pos + invalid.size())) {
    wire.replace(pos, threshold.size(), invalid);
  }
  BOOST_CHECK_THROW(parseProjection(wire), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionNeverTakesRootAuthorityFromJson)
{
  auto wire = projectionJson();
  wire.insert(1, "\"canonicalArtifactName\":\"/untrusted/camel\","
                 "\"canonical_artifact_name\":\"/untrusted/snake\",");
  BOOST_CHECK(parseProjection(wire).canonicalArtifactName.empty());
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionBindsStreamedGenerationContract)
{
  const auto value = parseProjection(streamingProjectionJson());
  BOOST_CHECK(value.generationContract.enabled);
  BOOST_CHECK_EQUAL(value.generationContract.maxGeneratedTokens, 8);
  BOOST_CHECK_EQUAL(value.generationContract.stateInputNames.size(), 3);
  BOOST_CHECK_EQUAL(value.plan.streamingOperationStride, 1);
  BOOST_REQUIRE_EQUAL(value.plan.dependencies.size(), 1);
  BOOST_CHECK_EQUAL(value.plan.dependencies.front().operationKind,
                    "TOKEN_FEEDBACK");

  auto missing = streamingProjectionJson();
  const auto begin = missing.find("\"generation_contract\"");
  const auto end = missing.find("\"group_capability_v1\"", begin);
  BOOST_REQUIRE_NE(begin, std::string::npos);
  BOOST_REQUIRE_NE(end, std::string::npos);
  missing.erase(begin, end - begin);
  BOOST_CHECK_THROW(parseProjection(missing), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionBindsRoleLocalConversationReference)
{
  const auto value = parseProjection(conversationProjectionJson());
  BOOST_REQUIRE(value.conversationStateReference);
  BOOST_REQUIRE(value.conversationTurnBinding);
  BOOST_CHECK_EQUAL(value.conversationStateReference->roleName, "S0R0");
  BOOST_CHECK_EQUAL(value.conversationStateReference->contextEpoch, 2);
  BOOST_CHECK_EQUAL(value.conversationStateReference->roleReceiptDigest,
                    digest('4'));
  BOOST_CHECK_EQUAL(value.conversationTurnBinding->successorContextEpoch, 3);

  auto wrongRole = conversationProjectionJson();
  const auto role = wrongRole.find("\"role_name\":\"S0R0\"");
  BOOST_REQUIRE_NE(role, std::string::npos);
  wrongRole.replace(role, std::string("\"role_name\":\"S0R0\"").size(),
                    "\"role_name\":\"S1R0\"");
  BOOST_CHECK_THROW(parseProjection(wrongRole), std::invalid_argument);

  auto invalidReceipt = conversationProjectionJson();
  const auto receipt = invalidReceipt.find(digest('4'));
  BOOST_REQUIRE_NE(receipt, std::string::npos);
  invalidReceipt.replace(receipt, digest('4').size(), "not-a-digest");
  BOOST_CHECK_THROW(parseProjection(invalidReceipt), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeRequestContractDigestBindsExactPayload)
{
  const std::string prompt = "prompt";
  const ndn::Buffer payload(
    reinterpret_cast<const std::uint8_t*>(prompt.data()), prompt.size());
  const auto expected = std::string("sha256:") +
    "cf07194ee232eb531e15f690000d19846dea69cf05504782658afcfacb9228a2";
  BOOST_CHECK(nativeRequestContractDigestMatches(expected, payload));

  auto changed = payload;
  changed.back() ^= 0x01;
  BOOST_CHECK(!nativeRequestContractDigestMatches(expected, changed));
  BOOST_CHECK(!nativeRequestContractDigestMatches(digest('A'), payload));
  BOOST_CHECK(!nativeRequestContractDigestMatches("", payload));
}

BOOST_AUTO_TEST_CASE(NativeV3ProtectedProjectionRequiresExactGrantBinding)
{
  auto missing = projectionJson();
  const auto plaintext = std::string("\"protection_epoch\":\"plaintext-v1\"");
  const auto protectedEpoch = std::string(
    "\"protection_epoch\":\"policy-epoch-7\"");
  for (auto pos = missing.find(plaintext); pos != std::string::npos;
       pos = missing.find(plaintext, pos + protectedEpoch.size())) {
    missing.replace(pos, plaintext.size(), protectedEpoch);
  }
  BOOST_CHECK_THROW(parseProjection(missing), std::invalid_argument);

  auto bound = missing;
  const auto insertion = bound.find("\"group_capability_v1\"");
  BOOST_REQUIRE(insertion != std::string::npos);
  bound.insert(insertion,
    "\"grant_binding\":{\"attempt\":1,"
    "\"grant_digest\":\"" + digest('9') + "\","
    "\"grant_name\":\"/authority/grants/request-1/provider-0\","
    "\"plan_core_digest\":\"" + digest('a') + "\","
    "\"protection_epoch\":\"policy-epoch-7\","
    "\"provider\":\"/provider/0\","
    "\"request_id\":\"/request/1\","
    "\"security_policy_snapshot_digest\":\"" + digest('a') + "\"},");
  const auto value = parseProjection(bound);
  BOOST_CHECK(value.hasGrantBinding);
  BOOST_CHECK_EQUAL(value.grantName,
                    "/authority/grants/request-1/provider-0");
  BOOST_CHECK_EQUAL(value.grantDigest, digest('9'));
  BOOST_CHECK_EQUAL(value.selectedRole.protectionEpoch, "policy-epoch-7");
}

BOOST_AUTO_TEST_CASE(NativeV3ProtectedRuntimeMustMatchSelectionBeforeExecution)
{
  auto selected = projection("/provider/0", "S0R0", true);
  selected.selectedRole.protectionEpoch = "policy-epoch-7";
  selected.assembly.protectionEpoch = "policy-epoch-7";
  selected.hasGrantBinding = true;
  selected.grantName = "/authority/grants/request-1/provider-0";
  selected.grantDigest = digest('9');
  auto endpointValue = endpoint("S0R0", "S1R0", digest('f'));
  selected.dataflow.mayPublish = {endpointValue};

  ProtectedRuntimeBindingV1 runtimeBinding;
  runtimeBinding.provider = selected.provider;
  runtimeBinding.role = selected.executionRole.roleId;
  runtimeBinding.requestId = selected.requestId;
  runtimeBinding.attempt = selected.attempt;
  runtimeBinding.planCoreDigest = selected.planCoreDigest;
  runtimeBinding.planDigest = selected.planDigest;
  runtimeBinding.securityPolicySnapshotDigest =
    selected.securityPolicySnapshotDigest;
  runtimeBinding.protectionEpoch = selected.selectedRole.protectionEpoch;
  runtimeBinding.grantName = selected.grantName;
  runtimeBinding.grantDigest = selected.grantDigest;
  runtimeBinding.providerBootId = "boot-1";
  runtimeBinding.fencingToken = "fence-1";
  runtimeBinding.revocationSequence = 4;
  runtimeBinding.expiresAtMs = 5000;
  runtimeBinding.mayPublishEndpointDigests = {endpointValue.endpointDigest};
  runtimeBinding.mayPublishConsumerByEndpoint = {
    {endpointValue.endpointDigest, endpointValue.consumerRole}};
  ProtectedRuntime runtime(runtimeBinding);
  // spec181 R002: binding consistency grants no execution authority and the
  // real verification entry fails closed until T002 installs the verifier.
  runtime.verifyBindingConsistency(runtimeBinding, 1000);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::NoGrant);
  BOOST_CHECK_THROW(runtime.verifyGrant(runtimeBinding, 1001),
                    std::runtime_error);

  BOOST_CHECK(!validateProtectedRuntimeBinding(
    selected, runtime, nullptr, "boot-1", "fence-1"));
  BOOST_CHECK_EQUAL(
    *validateProtectedRuntimeBinding(
      selected, runtime, nullptr, "boot-wrong", "fence-1"),
    "DI_PROTECTED_RUNTIME_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(NativeV3ProtectedRuntimeExcludesApplicationInputProducer)
{
  auto selected = projection("/provider/0", "S0R0", true);
  selected.selectedRole.protectionEpoch = "policy-epoch-7";
  selected.assembly.protectionEpoch = "policy-epoch-7";
  selected.hasGrantBinding = true;
  selected.grantName = "/authority/grants/request-1/provider-0";
  selected.grantDigest = digest('9');
  auto application = endpoint("", "S0R0", digest('f'));
  application.sourceKind = "APPLICATION_INPUT";
  application.operation = "APPLICATION_INPUT";
  application.producerNamespace = "/service";
  application.producerRole.clear();
  selected.dataflow.mustFetch = {application};

  ProtectedRuntimeBindingV1 runtimeBinding;
  runtimeBinding.provider = selected.provider;
  runtimeBinding.role = selected.executionRole.roleId;
  runtimeBinding.requestId = selected.requestId;
  runtimeBinding.attempt = selected.attempt;
  runtimeBinding.planCoreDigest = selected.planCoreDigest;
  runtimeBinding.planDigest = selected.planDigest;
  runtimeBinding.securityPolicySnapshotDigest =
    selected.securityPolicySnapshotDigest;
  runtimeBinding.protectionEpoch = selected.selectedRole.protectionEpoch;
  runtimeBinding.grantName = selected.grantName;
  runtimeBinding.grantDigest = selected.grantDigest;
  runtimeBinding.providerBootId = "boot-1";
  runtimeBinding.fencingToken = "fence-1";
  runtimeBinding.expiresAtMs = 5000;
  ProtectedRuntime runtime(runtimeBinding);
  // spec181 R002: binding consistency grants no execution authority and the
  // real verification entry fails closed until T002 installs the verifier.
  runtime.verifyBindingConsistency(runtimeBinding, 1000);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::NoGrant);
  BOOST_CHECK_THROW(runtime.verifyGrant(runtimeBinding, 1001),
                    std::runtime_error);

  BOOST_CHECK(!validateProtectedRuntimeBinding(
    selected, runtime, nullptr, "boot-1", "fence-1"));
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionDecodesCertifiedAssemblyRecipe)
{
  const auto value = parseProjection(
    projectionJson(1, {}, "CPU", {}, false, true));
  BOOST_CHECK_EQUAL(value.assembly.canonicalInitializerDigest, digest('e'));
  BOOST_CHECK_EQUAL(value.assembly.nodeIndices.size(), 4);
  BOOST_REQUIRE_EQUAL(value.assembly.expectedInputs.size(), 1);
  BOOST_CHECK_EQUAL(value.assembly.expectedInputs.front().shape.at(1), "4");
  BOOST_CHECK_EQUAL(value.assembly.precision, "fp32");
  BOOST_CHECK_EQUAL(value.assembly.maxSourceBytes, 4096);

  auto incomplete = projectionJson(1, {}, "CPU", {}, false, true);
  const auto field = incomplete.find("\"canonical_initializer_digest\"");
  BOOST_REQUIRE_NE(field, std::string::npos);
  const auto valueStart = incomplete.find('"', incomplete.find(':', field) + 1);
  const auto valueEnd = incomplete.find('"', valueStart + 1);
  incomplete.replace(valueStart + 1, valueEnd - valueStart - 1, "");
  BOOST_CHECK_THROW(parseProjection(incomplete), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionRejectsMultipleLocalRoles)
{
  BOOST_CHECK_THROW(parseProjection(projectionJson(1, {}, "CPU", {}, true)),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionRejectsWrongPlanOrAttempt)
{
  BOOST_CHECK_THROW(parseProjection(projectionJson(2)), std::invalid_argument);
  BOOST_CHECK_THROW(parseProjection(projectionJson(1, digest('9'))),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionRejectsExecutionAssemblySubstitution)
{
  auto wire = projectionJson();
  const auto execution = wire.find("\"execution_role\"");
  BOOST_REQUIRE_NE(execution, std::string::npos);
  const auto layerEnd = wire.find("\"layer_end\":4", execution);
  BOOST_REQUIRE_NE(layerEnd, std::string::npos);
  wire.replace(layerEnd, std::string("\"layer_end\":4").size(),
               "\"layer_end\":5");
  BOOST_CHECK_THROW(parseProjection(wire), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionRejectsInvalidCpuAndDeviceBindings)
{
  BOOST_CHECK_THROW(parseProjection(projectionJson(1, {}, "CPU", "cuda:0")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(parseProjection(projectionJson(1, {}, "SINGLE_DEVICE")),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSetAcceptsExactAcyclicEndpointCover)
{
  const auto values = validProjectionSet();
  BOOST_CHECK_NO_THROW(validateNativeSelectionProjectionSetV3(values));
}

BOOST_AUTO_TEST_CASE(NativeV3RuntimeEdgesComeOnlyFromSealedRoleDataflow)
{
  auto values = validProjectionSet();
  values[0].plan.dependencies.push_back(NativeDependencySpec{
    {"unrelated"}, {"other"}, "legacy-scope", "/legacy",
    "/legacy/{sessionId}"});
  const auto source = roleSpecFromSelectionProjectionV3(
    values[0], values[0].provider);
  BOOST_REQUIRE_EQUAL(source.outputs.size(), 1);
  BOOST_CHECK(source.inputs.empty());
  BOOST_CHECK(source.outputs.front().declaredByV3);
  BOOST_CHECK_EQUAL(source.outputs.front().scope, "group-0");
  BOOST_CHECK_EQUAL(source.outputs.front().manifestDataName,
                    tensorObjectManifestName(
                      values[0].dataflow.mayPublish.front()));
  BOOST_CHECK_EQUAL(source.outputs.front().maxSegments, 1);
  BOOST_CHECK_EQUAL(source.outputs.front().securityProfile, "NDNSF_DATA_V1");

  const auto sink = roleSpecFromSelectionProjectionV3(
    values[1], values[1].provider);
  BOOST_REQUIRE_EQUAL(sink.inputs.size(), 1);
  BOOST_CHECK(sink.outputs.empty());
  BOOST_CHECK(sink.inputs.front().declaredByV3);
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(
                      values[1], "/provider/wrong"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSeparatesMultiplePipelineTensors)
{
  auto source = projection("/provider/0", "S0R0", false);
  auto sink = projection("/provider/1", "S1R0", true);
  auto first = endpoint("S0R0", "S1R0", digest('f'));
  auto second = endpoint("S0R0", "S1R0", digest('1'));
  second.tensorId = "activation-2";
  source.dataflow.mayPublish = {first, second};
  sink.dataflow.mustFetch = {first, second};

  const auto sourceSpec = roleSpecFromSelectionProjectionV3(source, source.provider);
  const auto sinkSpec = roleSpecFromSelectionProjectionV3(sink, sink.provider);
  BOOST_REQUIRE_EQUAL(sourceSpec.outputs.size(), 2);
  BOOST_REQUIRE_EQUAL(sinkSpec.inputs.size(), 2);
  BOOST_CHECK_NE(sourceSpec.outputs[0].scope, sourceSpec.outputs[1].scope);
  BOOST_CHECK_EQUAL(sourceSpec.outputs[0].scope, sinkSpec.inputs[0].scope);
  BOOST_CHECK_EQUAL(sourceSpec.outputs[1].scope, sinkSpec.inputs[1].scope);
  BOOST_CHECK_EQUAL(sourceSpec.outputs[0].transportScope, first.groupId);
  BOOST_CHECK_EQUAL(sinkSpec.inputs[1].transportScope, second.groupId);
}

BOOST_AUTO_TEST_CASE(NativeTensorEndpointUsesOneExactManifestAndSegmentGrammar)
{
  auto value = endpoint("S0R0", "S1R0", digest('f'));
  value.producerNamespace = "/provider/0";
  value.requester = "/requester/app";
  value.requestId = "request/1";
  value.segmentCount = 2;
  const auto prefix = tensorObjectNamePrefix(value);
  BOOST_CHECK_EQUAL(prefix.find("/NDNSF-DI/TENSOR/v1/"),
                    std::string("/provider/0").size());
  BOOST_CHECK_NE(prefix.find("/REQUESTER/"), std::string::npos);
  BOOST_CHECK_NE(prefix.find("/SOURCE-ROLE/"), std::string::npos);
  BOOST_CHECK_EQUAL(tensorObjectManifestName(value), prefix + "/MANIFEST");
  BOOST_CHECK_EQUAL(tensorObjectSegmentName(value, 0), prefix + "/SEG/seg=0");
  BOOST_CHECK_EQUAL(tensorObjectSegmentName(value, 1), prefix + "/SEG/seg=1");
  BOOST_CHECK_THROW(tensorObjectSegmentName(value, 2), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(TensorObjectManifestCodecRoundTripsAndRejectsMutation)
{
  TensorObjectManifestV1 value;
  value.capabilityDigest = digest('1');
  value.epochKeyId = "epoch-key";
  value.requester = "/requester/app";
  value.requestId = "request/1";
  value.attemptId = "1";
  value.planDigest = digest('2');
  value.groupId = "group-1";
  value.epoch = "epoch-1";
  value.operationIndex = 3;
  value.round = 2;
  value.operationKind = "ALL_REDUCE";
  value.producerRole = "S1R0";
  value.producerRank = 0;
  value.consumerRoles = {"S1R1"};
  value.microbatch = 4;
  value.sourceLayoutDigest = digest('3');
  value.targetLayoutDigest = digest('4');
  value.tensorId = "hidden";
  value.tensorDigest = digest('5');
  value.contentDigest = digest('6');
  value.totalBytes = 14;
  value.segmentSize = 7;
  value.segmentCount = 2;
  value.orderedSegmentDigests = {digest('7'), digest('8')};
  value.createdAtMs = 1;
  value.noProgressMs = 500;
  value.hardDeadlineMs = 5000;
  value.endpointDigest = digest('9');
  value.manifestContractDigest = digest('a');
  value.producerSignature = {1, 2, 3, 4};
  value.objectManifestDigest = value.digest();

  const auto wire = encodeTensorObjectManifest(value);
  const auto decoded = decodeTensorObjectManifest(wire);
  BOOST_CHECK_EQUAL(decoded.objectManifestDigest, value.objectManifestDigest);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.consumerRoles.begin(),
                                decoded.consumerRoles.end(),
                                value.consumerRoles.begin(),
                                value.consumerRoles.end());

  auto corrupted = wire;
  corrupted.back() ^= 0x01;
  BOOST_CHECK_THROW(decodeTensorObjectManifest(corrupted),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TensorObjectManifestLegacyCodecPreservesManySegments)
{
  TensorObjectManifestV1 value;
  value.capabilityDigest = digest('1');
  value.epochKeyId = "epoch-key";
  value.requester = "/requester/app";
  value.requestId = "request/1";
  value.attemptId = "1";
  value.planDigest = digest('2');
  value.groupId = "group-1";
  value.epoch = "epoch-1";
  value.operationKind = "PIPELINE";
  value.producerRole = "BackboneNeck";
  value.consumerRoles = {"DetectShard0"};
  value.sourceLayoutDigest = digest('3');
  value.targetLayoutDigest = digest('4');
  value.tensorId = "/model/model.16/cv2/act/Mul_output_0";
  value.tensorDigest = digest('5');
  value.contentDigest = digest('6');
  value.totalBytes = 235 * 7000;
  value.segmentSize = 7000;
  value.segmentCount = 235;
  value.orderedSegmentDigests.assign(235, digest('7'));
  value.createdAtMs = 1;
  value.noProgressMs = 10000;
  value.hardDeadlineMs = 30000;
  value.endpointDigest = digest('8');
  value.manifestContractDigest = digest('9');
  value.producerSignature.assign(64, 0x42);
  value.objectManifestDigest = value.digest();

  const auto wire = encodeTensorObjectManifest(value);
  // The legacy representation remains decodable, but large manifests use
  // ContextCompact on the network (Spec181 exact-tensor-wire contract).
  const auto decoded = decodeTensorObjectManifest(wire);
  BOOST_CHECK(!decoded.compactContextRequired);
  BOOST_CHECK_EQUAL(decoded.digest(), value.digest());
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.producerSignature.begin(),
                                decoded.producerSignature.end(),
                                value.producerSignature.begin(),
                                value.producerSignature.end());
  BOOST_CHECK_EQUAL(decoded.segmentCount, value.segmentCount);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.orderedSegmentDigests.begin(),
                                decoded.orderedSegmentDigests.end(),
                                value.orderedSegmentDigests.begin(),
                                value.orderedSegmentDigests.end());
}

BOOST_AUTO_TEST_CASE(TensorObjectManifestContextCompactFitsAndRoundTrips)
{
  TensorObjectManifestV1 value;
  value.capabilityDigest = digest('1');
  value.epochKeyId = "epoch-key";
  value.requester = "/requester/app";
  value.requestId = "request/1";
  value.attemptId = "1";
  value.planDigest = digest('2');
  value.groupId = "group-1";
  value.epoch = "1";
  value.operationKind = "PIPELINE";
  value.producerRole = "BackboneNeck";
  value.consumerRoles = {"DetectShard0"};
  value.sourceLayoutDigest = digest('3');
  value.targetLayoutDigest = digest('4');
  value.tensorId = "/model/model.16/cv2/act/Mul_output_0";
  value.tensorDigest = digest('5');
  value.contentDigest = digest('6');
  value.totalBytes = 235 * 7400;
  value.segmentSize = 7400;
  value.segmentCount = 235;
  value.orderedSegmentDigests.assign(235, digest('7'));
  value.createdAtMs = 1;
  value.noProgressMs = 10000;
  value.hardDeadlineMs = 30000;
  value.endpointDigest = digest('8');
  value.manifestContractDigest = digest('9');
  value.producerSignature.assign(64, 0x42);
  value.objectManifestDigest = value.digest();

  const auto wire = encodeTensorObjectManifestContextCompact(value);
  BOOST_CHECK_LT(wire.size(), ndn::MAX_NDN_PACKET_SIZE);
  const auto decoded = decodeTensorObjectManifest(wire);
  BOOST_CHECK(decoded.compactContextRequired);
  BOOST_CHECK_EQUAL(decoded.contentDigest, value.contentDigest);
  BOOST_CHECK_EQUAL(decoded.totalBytes, value.totalBytes);
  BOOST_CHECK_EQUAL(decoded.segmentCount, value.segmentCount);
  std::vector<std::uint8_t> digestCommitmentInput;
  for (const auto& digest : value.orderedSegmentDigests) {
    digestCommitmentInput.insert(digestCommitmentInput.end(),
                                 digest.begin(), digest.end());
  }
  BOOST_CHECK_EQUAL(decoded.compactSegmentDigestCommitment,
                    sha256TensorBytes(digestCommitmentInput));
  BOOST_CHECK(decoded.orderedSegmentDigests.empty());
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSetRejectsDuplicateProviderOrRole)
{
  auto duplicateProvider = validProjectionSet();
  duplicateProvider[1].provider = duplicateProvider[0].provider;
  BOOST_CHECK_THROW(validateNativeSelectionProjectionSetV3(duplicateProvider),
                    std::invalid_argument);

  auto duplicateRole = validProjectionSet();
  duplicateRole[1].selectedRole.selectedRole = "S0R0";
  duplicateRole[1].executionRole.roleId = "S0R0";
  duplicateRole[1].dataflow.role = "S0R0";
  BOOST_CHECK_THROW(validateNativeSelectionProjectionSetV3(duplicateRole),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSetRejectsMissingEndpointOwner)
{
  auto values = validProjectionSet();
  values[1].dataflow.mustFetch.front().endpointDigest = digest('8');
  BOOST_CHECK_THROW(validateNativeSelectionProjectionSetV3(values),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSetRejectsCycleAndMultipleTerminalOwners)
{
  auto cycle = validProjectionSet();
  const auto reverse = endpoint("S1R0", "S0R0", digest('7'));
  cycle[1].dataflow.mayPublish.push_back(reverse);
  cycle[0].dataflow.mustFetch.push_back(reverse);
  BOOST_CHECK_THROW(validateNativeSelectionProjectionSetV3(cycle),
                    std::invalid_argument);

  auto terminal = validProjectionSet();
  terminal[0].dataflow.terminalResponseOwner = true;
  BOOST_CHECK_THROW(validateNativeSelectionProjectionSetV3(terminal),
                    std::invalid_argument);
}

} // namespace ndnsf::di::test
