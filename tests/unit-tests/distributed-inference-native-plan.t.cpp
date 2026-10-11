#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <ctime>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <cfenv>
#include <exception>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

void certifyNamingEndpoint(NativeTensorEndpointV3& value)
{
  auto identity = nativeEndpointJson(value);
  identity.erase("endpoint_digest"); identity.erase("consumer_role");
  value.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
}

void certifyNamingFlow(NativeSelectionProjectionV3& projection)
{
  std::map<std::string, std::string> replaced;
  for (auto* endpoints : {&projection.dataflow.mayPublish, &projection.dataflow.mustFetch}) {
    for (auto& value : *endpoints) {
      const auto old = value.endpointDigest;
      value.namingOperationStride = value.sourceKind == "ROLE" && projection.generationContract.enabled ?
        projection.generationContract.streamingOperationStride : 0;
      certifyNamingEndpoint(value);
      replaced[old] = value.endpointDigest;
    }
  }
  for (auto& ready : projection.dataflow.waitFor)
    for (auto& digest : ready.endpointDigests) digest = replaced.at(digest);
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
  value.consumerRoles = {value.consumerRole};
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
  certifyNamingEndpoint(value);
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
  auto multiTransfer = streamingProjectionJson();
  const auto stride = multiTransfer.find("\"streaming_operation_stride\":1");
  BOOST_REQUIRE_NE(stride, std::string::npos);
  multiTransfer.replace(stride, std::string("\"streaming_operation_stride\":1").size(),
                        "\"streaming_operation_stride\":3");
  BOOST_CHECK_EQUAL(parseProjection(multiTransfer).plan.streamingOperationStride, 3);
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

BOOST_AUTO_TEST_CASE(NativeGenerationBudgetAccepts1025AndRetainsWireBounds)
{
  NativeJson options{{"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
    {"maxNewTokens", 64}, {"tokenizerDigest", digest('8')},
    {"eosTokenIds", NativeJson::array({2})}};
  const auto decodeOptions = [](const NativeJson& value) {
    const auto wire = value.dump();
    return nativeGenerationFromOptions({wire.begin(), wire.end()}, std::string(32, 'a'));
  };
  const auto samplingDigest = decodeOptions(options).samplingDigest;
  for (const auto budget : {1, 64, 1024, 1025}) {
    BOOST_TEST_CONTEXT("generation budget=" << budget) {
      options["maxNewTokens"] = budget;
      const auto generation = decodeOptions(options);
      BOOST_CHECK_EQUAL(generation.maxGeneratedTokens, budget);
      BOOST_CHECK_EQUAL(generation.samplingDigest, samplingDigest);
      auto wire = nativeParseJson(streamingProjectionJson());
      wire["generation_contract"]["max_generated_tokens"] = budget;
      const auto projection = parseProjection(wire.dump());
      BOOST_CHECK_EQUAL(projection.generationContract.maxGeneratedTokens, budget);
      BOOST_CHECK_EQUAL(parseProjection(nativeSelectionProjectionV3ToJson(projection))
                          .generationContract.maxGeneratedTokens, budget);
    }
  }
  for (const auto budget : {0, 1026}) {
    options["maxNewTokens"] = budget;
    BOOST_CHECK_THROW(decodeOptions(options), std::invalid_argument);
    auto wire = nativeParseJson(streamingProjectionJson());
    wire["generation_contract"]["max_generated_tokens"] = budget;
    BOOST_CHECK_THROW(parseProjection(wire.dump()), std::invalid_argument);
    auto projection = parseProjection(streamingProjectionJson());
    projection.generationContract.maxGeneratedTokens = budget;
    BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(projection, projection.provider, 0),
                      std::invalid_argument);
  }
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
  application.producerNamespace = application.requester;
  certifyNamingEndpoint(application);
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
  BOOST_CHECK_EQUAL(std::get<std::int64_t>(value.assembly.expectedInputs.front().shape.at(1)), 4);
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

BOOST_AUTO_TEST_CASE(NativeV3ProjectionCompleteEncoderRoundTripsProductionParser)
{
  const auto original = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto wire = nativeSelectionProjectionV3ToJson(original);
  const auto parsed = parseProjection(wire);
  BOOST_CHECK_EQUAL(parsed.planDigest, original.planDigest);
  BOOST_CHECK_EQUAL(parsed.offerDigest, original.offerDigest);
  BOOST_CHECK_EQUAL(parsed.dataflow.dataflowDigest, original.dataflow.dataflowDigest);
  BOOST_CHECK_EQUAL(parsed.deviceBinding.resourceSnapshotDigest, original.deviceBinding.resourceSnapshotDigest);
  BOOST_CHECK(parsed.assembly.expectedInputs.front().shape == original.assembly.expectedInputs.front().shape);
  BOOST_CHECK_EQUAL(nativeSelectionProjectionV3ToJson(parsed), wire);
  auto incomplete = original;
  incomplete.deadlineMs = 0;
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(incomplete), std::invalid_argument);
  incomplete = original;
  incomplete.deviceBinding.offerDigest = digest('9');
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(incomplete), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3CompactAssemblyPreservesCompleteContract)
{
  auto original = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto legacy = nativeSelectionProjectionV3ToJson(original);
  original.compactAssembly = true;
  const auto wire = nativeSelectionProjectionV3ToJson(original);
  const auto value = nativeParseJson(wire);
  BOOST_CHECK(!value.contains("assembly"));
  BOOST_CHECK(value.at("assembly_same_as_role").is_boolean());
  BOOST_CHECK(value.at("assembly_same_as_role").get<bool>());
  BOOST_CHECK_LT(wire.size(), legacy.size());
  const auto decoded = parseProjection(wire);
  BOOST_CHECK(decoded.compactAssembly);
  BOOST_CHECK_EQUAL(decoded.planDigest, original.planDigest);
  BOOST_CHECK_EQUAL(decoded.assembly.recipeDigest, original.assembly.recipeDigest);
  BOOST_CHECK(decoded.assembly.nodeIndices == original.assembly.nodeIndices);
  BOOST_CHECK(decoded.assembly.expectedInputs.front().shape == original.assembly.expectedInputs.front().shape);
  BOOST_CHECK_EQUAL(nativeSelectionProjectionV3ToJson(decoded), wire);
  auto bad = value;
  bad["assembly_same_as_role"] = "true";
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  bad = value; bad["assembly_same_as_role"] = false;
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  bad = value; bad["assembly"] = value.at("roles").at(0);
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  bad = value; bad.erase("assembly_same_as_role");
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  bad = value; bad["roles"].push_back(value.at("roles").at(0));
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  bad = value; bad["roles"] = NativeJson::array();
  BOOST_CHECK_THROW(parseProjection(nativeCanonicalJson(bad)), std::invalid_argument);
  auto mismatch = original;
  mismatch.assembly.layerEnd += 1;
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(mismatch), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3FixedRoleStorageKeepsIdentityAndWireContract)
{
  auto projection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  projection.compactAssembly = true;
  const auto before = nativeSelectionProjectionV3ToJson(projection);
  const auto nodes = projection.selectedRole.nodeIndices;
  const auto inputs = projection.selectedRole.expectedInputs.front().shape;
  const auto owner = projection.shareFixedRoleStorage();
  BOOST_REQUIRE(owner);
  BOOST_CHECK(projection.selectedRole.nodeIndices.empty());
  BOOST_CHECK(projection.assembly.nodeIndices.empty());
  BOOST_CHECK(projection.selectedRoleContract().nodeIndices == nodes);
  BOOST_CHECK(projection.assemblyContract().expectedInputs.front().shape == inputs);
  BOOST_CHECK_EQUAL(&projection.selectedRoleContract(), owner.get());
  BOOST_CHECK_EQUAL(&projection.assemblyContract(), owner.get());
  BOOST_CHECK_EQUAL(nativeSelectionProjectionV3ToJson(projection), before);
  auto copy = projection;
  BOOST_CHECK_EQUAL(copy.shareFixedRoleStorage().get(), owner.get());
  copy.requestId = "/request/independent-context";
  BOOST_CHECK_EQUAL(projection.requestId, "/request/1");
  BOOST_CHECK_EQUAL(copy.fixedRoleOwner.get(), owner.get());
  copy = projection;
  copy.executionRole.layerEnd += 1;
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(copy), std::invalid_argument);
  copy = projection;
  copy.provider = "/provider/other";
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(copy), std::invalid_argument);
  copy = projection;
  copy.deviceBinding.role = "wrong-role";
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(copy), std::invalid_argument);
  copy = projection;
  copy.hasGrantBinding = true;
  copy.grantName = "/grant/invalid";
  copy.grantDigest = "invalid-digest";
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(copy), std::invalid_argument);
  copy = projection;
  projection = {};
  BOOST_CHECK(copy.selectedRoleContract().nodeIndices == nodes);

  auto mismatch = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  mismatch.assembly.layerEnd += 1;
  BOOST_CHECK_THROW(mismatch.shareFixedRoleStorage(), std::invalid_argument);
  BOOST_CHECK(!mismatch.fixedRoleOwner);

  auto protectedProjection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  protectedProjection.selectedRole.protectionEpoch = "policy-epoch-7";
  protectedProjection.assembly.protectionEpoch = "policy-epoch-7";
  protectedProjection.hasGrantBinding = true;
  protectedProjection.grantName = "/authority/grants/request-1/provider-0";
  protectedProjection.grantDigest = digest('9');
  const auto protectedWire = nativeSelectionProjectionV3ToJson(protectedProjection);
  protectedProjection.shareFixedRoleStorage();
  BOOST_CHECK_EQUAL(nativeSelectionProjectionV3ToJson(protectedProjection), protectedWire);
  protectedProjection.grantDigest = "invalid-digest";
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(protectedProjection), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeAssemblyJsonMatcherPreservesWireFieldIdentity)
{
  auto projection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  auto role = projection.selectedRoleContract();
  role.expectedInputs = {{"input", "float32", {std::int64_t(-1), std::string("sequence")}}};
  role.expectedOutputs = {{"output", "float32", {std::int64_t(1), std::int64_t(8)}}};
  role.nodeIndices = {0, 3, 9};
  role.maxSourceBytes = 4096;
  role.maxAssembledBytes = 8192;
  role.maxNodes = 16;
  const auto value = nativeAssemblyJson(role);
  const auto before = detail::fixedAssignmentWork();
  BOOST_CHECK(nativeAssemblyJsonMatches(role, value));
  for (auto it = value.begin(); it != value.end(); ++it) {
    auto missing = value;
    missing.erase(it.key());
    BOOST_CHECK_MESSAGE(!nativeAssemblyJsonMatches(role, missing), it.key());
    auto changed = value;
    changed[it.key()] = nullptr;
    BOOST_CHECK_MESSAGE(!nativeAssemblyJsonMatches(role, changed), it.key());
  }
  auto changed = value;
  changed["extra"] = true;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["rank"] = double(role.rank);
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["expected_inputs"][0]["shape"][0] = std::numeric_limits<std::uint64_t>::max();
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["expected_inputs"][0]["extra"] = true;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["expected_outputs"][0]["shape"][1] = 7;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["node_indices"][1] = 4;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed = value;
  changed["resource_envelope"]["maxNodes"] = 17;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  role.postprocessConfidenceThreshold = -0.0;
  changed = value;
  changed["postprocess_confidence_threshold"] = 0.0;
  BOOST_CHECK(!nativeAssemblyJsonMatches(role, changed));
  changed["postprocess_confidence_threshold"] = -0.0;
  BOOST_CHECK(nativeAssemblyJsonMatches(role, changed));
  const auto after = detail::fixedAssignmentWork();
  BOOST_CHECK_EQUAL(after.roleEncodes, before.roleEncodes);
  for (const auto invalid : {std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()}) {
    role.postprocessConfidenceThreshold = invalid;
    auto invalidValue = value;
    invalidValue["postprocess_confidence_threshold"] = invalid;
    BOOST_CHECK_THROW(nativeAssemblyJsonMatches(role, invalidValue), std::invalid_argument);
  }
  struct RestoreRounding {
    int previous = std::fegetround();
    ~RestoreRounding() { std::fesetround(previous); }
  } restoreRounding;
  role.postprocessConfidenceThreshold = 0.1;
  auto fractionalValue = value;
  fractionalValue["postprocess_confidence_threshold"] = 0.1;
  for (const auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    std::string oldFailure;
    try { (void)nativeCanonicalJson(nativeAssemblyJson(role)); }
    catch (const std::invalid_argument& error) { oldFailure = error.what(); }
    if (oldFailure.empty()) BOOST_CHECK(nativeAssemblyJsonMatches(role, fractionalValue));
    else BOOST_CHECK_EXCEPTION(nativeAssemblyJsonMatches(role, fractionalValue), std::invalid_argument,
      [&](const std::invalid_argument& error) { return oldFailure == error.what(); });
  }
  NativePreparedPlanningCache legacyCache;
  BOOST_CHECK(!legacyCache.preparedRoleJson(0));
}

BOOST_AUTO_TEST_CASE(NativeFixedRoleInvocationReusesOwnerWithoutRoleWire)
{
  auto projection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto coldBefore = detail::fixedAssignmentWork();
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(projection);
  const auto coldAfter = detail::fixedAssignmentWork();
  BOOST_CHECK_EQUAL(coldAfter.roleBuilds, coldBefore.roleBuilds + 1);
  BOOST_CHECK_GT(coldAfter.roleEncodes, coldBefore.roleEncodes);
  BOOST_REQUIRE(fixed);
  const auto wire = nativeSelectionInvocationV1ToJson(projection, *fixed);
  const auto root = nativeParseJson(wire);
  BOOST_CHECK_EQUAL(root.at("schema").get<std::string>(), "ndnsf-di-selection-invocation-v1");
  BOOST_CHECK_EQUAL(root.at("fixed_role_ref").get<std::string>(), fixed->digest());
  BOOST_CHECK(!root.contains("roles"));
  BOOST_CHECK(!root.contains("assembly"));
  BOOST_REQUIRE(root.at("execution_role").is_object());
  BOOST_CHECK_EQUAL(root.at("execution_role").size(), 1);
  BOOST_CHECK_EQUAL(root.at("execution_role").at("role_id").get<std::string>(), "S0R0");
  const auto fullRoot = nativeParseJson(nativeSelectionProjectionV3ToJson(projection));
  BOOST_CHECK_EQUAL(fullRoot.at("execution_role").size(), 8);
  BOOST_CHECK_EQUAL(fullRoot.at("execution_role").at("backend").get<std::string>(),
                    fixed->executionRole().backend);
  const auto sourceBefore = detail::fixedAssignmentWork();
  std::istringstream fixedSource(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restoredFixed = nativeFixedRoleAssignmentV1FromJson(fixedSource);
  const auto sourceAfter = detail::fixedAssignmentWork();
  BOOST_CHECK_EQUAL(sourceAfter.sourceEncodes, sourceBefore.sourceEncodes + 1);
  BOOST_CHECK_EQUAL(sourceAfter.sourceParses, sourceBefore.sourceParses + 1);
  BOOST_CHECK_EQUAL(sourceAfter.roleBuilds, sourceBefore.roleBuilds + 1);
  BOOST_CHECK_EQUAL(restoredFixed->digest(), fixed->digest());
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().roleId, projection.executionRole.roleId);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().stageId, projection.executionRole.stageId);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().rank, projection.executionRole.rank);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().layerBegin, projection.executionRole.layerBegin);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().layerEnd, projection.executionRole.layerEnd);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().backend, projection.executionRole.backend);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().adapterId, projection.executionRole.adapterId);
  BOOST_CHECK_EQUAL(restoredFixed->executionRole().adapterVersion, projection.executionRole.adapterVersion);
  BOOST_CHECK_LT(wire.size(), nativeSelectionProjectionV3ToJson(projection).size());
  const auto decode = [&fixed](const NativeJson& value) {
    std::istringstream input(nativeCanonicalJson(value));
    return nativeSelectionInvocationV1FromJson(input, "S0R0", *fixed);
  };
  const auto decoded = decode(root);
  BOOST_CHECK_EQUAL(decoded.fixedRoleOwner.get(), fixed->roleOwner().get());
  const ndn::Buffer invocationPayload(wire.begin(), wire.end());
  const auto fields = parseNativeProviderAssignmentFields(invocationPayload, "S0R0", &decoded);
  BOOST_CHECK_EQUAL(fields.at("executionRequestId"), projection.requestId);
  BOOST_CHECK_EQUAL(fields.at("artifactDigest"), fixed->roleOwner()->artifactDigest);
  NativeModelRunnerSpec preassembled;
  preassembled.role = "S0R0";
  preassembled.metadata["fragmentDigest"] = fixed->roleOwner()->artifactDigest;
  BOOST_CHECK(!validateNativeProviderAssignmentPayload({preassembled}, "S0R0", invocationPayload, &decoded));
  preassembled.metadata["fragmentDigest"] = digest('f');
  BOOST_CHECK_EQUAL(*validateNativeProviderAssignmentPayload({preassembled}, "S0R0", invocationPayload, &decoded),
                    "DI_BINDING_FRAGMENT_MISMATCH");
  BOOST_CHECK_THROW(parseNativeProviderAssignmentFields(invocationPayload, "S0R0"), std::invalid_argument);
  auto foreignContext = decoded;
  foreignContext.requestId = "/request/foreign";
  BOOST_CHECK_THROW(parseNativeProviderAssignmentFields(invocationPayload, "S0R0", &foreignContext),
                    std::invalid_argument);
  BOOST_CHECK(decoded.selectedRole.nodeIndices.empty());
  BOOST_CHECK(decoded.assembly.nodeIndices.empty());
  BOOST_CHECK(decoded.selectedRoleContract().nodeIndices == projection.selectedRole.nodeIndices);
  BOOST_CHECK_EQUAL(nativeSelectionInvocationV1ToJson(decoded, *fixed), wire);
  auto mutableAlias = std::make_shared<NativeSelectionRoleV3>(projection.selectedRole);
  auto aliasedProjection = projection;
  aliasedProjection.fixedRoleOwner = mutableAlias;
  const auto frozenAlias = nativeFixedRoleAssignmentV1FromProjection(aliasedProjection);
  BOOST_CHECK_NE(frozenAlias->roleOwner().get(), mutableAlias.get());
  BOOST_CHECK_EQUAL(frozenAlias->digest(), fixed->digest());
  mutableAlias->nodeIndices.push_back(99);
  BOOST_CHECK(frozenAlias->roleOwner()->nodeIndices == projection.selectedRole.nodeIndices);
  BOOST_CHECK_EQUAL(nativeSelectionInvocationV1ToJson(projection, *frozenAlias), wire);
  auto current = projection;
  current.requestId = "/request/2";
  current.dataflow.requestId = current.requestId;
  const auto workBefore = detail::fixedAssignmentWork();
  std::istringstream nextInput(nativeSelectionInvocationV1ToJson(current, *fixed));
  const auto next = nativeSelectionInvocationV1FromJson(nextInput, "S0R0", *fixed);
  const auto workAfter = detail::fixedAssignmentWork();
  BOOST_CHECK_EQUAL(workAfter.placementBuilds, workBefore.placementBuilds);
  BOOST_CHECK_EQUAL(workAfter.roleBuilds, workBefore.roleBuilds);
  BOOST_CHECK_EQUAL(workAfter.roleEncodes, workBefore.roleEncodes);
  BOOST_CHECK_EQUAL(workAfter.sourceEncodes, workBefore.sourceEncodes);
  BOOST_CHECK_EQUAL(workAfter.sourceParses, workBefore.sourceParses);
  BOOST_CHECK_EQUAL(next.requestId, "/request/2");
  BOOST_CHECK_EQUAL(next.fixedRoleOwner.get(), decoded.fixedRoleOwner.get());

  // A factory running on a different executor must not inflate this
  // request thread's work. Join before examining the worker's results.
  detail::FixedAssignmentWork workerBefore, workerAfter;
  std::exception_ptr workerFailure;
  std::thread worker([&] {
    try {
      workerBefore = detail::fixedAssignmentWork();
      const auto workerFixed = nativeFixedRoleAssignmentV1FromProjection(projection);
      workerAfter = detail::fixedAssignmentWork();
    }
    catch (...) { workerFailure = std::current_exception(); }
  });
  worker.join();
  if (workerFailure) std::rethrow_exception(workerFailure);
  BOOST_CHECK_EQUAL(workerAfter.roleBuilds, workerBefore.roleBuilds + 1);
  BOOST_CHECK_GT(workerAfter.roleEncodes, workerBefore.roleEncodes);
  const auto parentAfter = detail::fixedAssignmentWork();
  BOOST_CHECK_EQUAL(parentAfter.placementBuilds, workAfter.placementBuilds);
  BOOST_CHECK_EQUAL(parentAfter.roleBuilds, workAfter.roleBuilds);
  BOOST_CHECK_EQUAL(parentAfter.roleEncodes, workAfter.roleEncodes);
  BOOST_CHECK_EQUAL(parentAfter.sourceEncodes, workAfter.sourceEncodes);
  BOOST_CHECK_EQUAL(parentAfter.sourceParses, workAfter.sourceParses);

  auto bad = root;
  bad["fixed_role_ref"] = digest('f');
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_role"]["backend"] = "CPU";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_role"]["role_id"] = "foreign";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_role"] = "S0R0";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_role"]["role_id"] = 1;
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  auto changedExecution = projection;
  changedExecution.executionRole.backend = "foreign";
  BOOST_CHECK_THROW(nativeSelectionInvocationV1ToJson(changedExecution, *fixed), std::invalid_argument);
  bad = root;
  bad["provider"] = "/provider/other";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["roles"] = NativeJson::array();
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["schema_version"] = "1";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  for (const auto version : {std::uint64_t{2}, std::uint64_t{4294967297}}) {
    bad = root;
    bad["schema_version"] = version;
    BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  }
  bad = root;
  bad["request_id"] = "/request/foreign-context";
  // The flow header now derives its request binding from the invocation.
  // Reusing a fixed assignment for another valid request is intentional.
  BOOST_CHECK_EQUAL(decode(bad).dataflow.requestId, "/request/foreign-context");
  bad["request_id"] = "";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  BOOST_CHECK_THROW(parseProjection(wire), std::invalid_argument);
  auto foreignProjection = projection;
  foreignProjection.provider = "/provider/other";
  foreignProjection.deviceBinding.provider = foreignProjection.provider;
  const auto foreignFixed = nativeFixedRoleAssignmentV1FromProjection(foreignProjection);
  BOOST_CHECK_NE(foreignFixed->digest(), fixed->digest());
  std::istringstream foreignInput(wire);
  BOOST_CHECK_THROW(nativeSelectionInvocationV1FromJson(foreignInput, "S0R0", *foreignFixed),
                    std::invalid_argument);
  current = projection;
  current.assembly.layerEnd += 1;
  BOOST_CHECK_THROW(nativeSelectionInvocationV1ToJson(current, *fixed), std::invalid_argument);

  auto protectedProjection = projection;
  protectedProjection.selectedRole.protectionEpoch = "policy-epoch-7";
  protectedProjection.assembly.protectionEpoch = "policy-epoch-7";
  protectedProjection.hasGrantBinding = true;
  protectedProjection.grantName = "/authority/grants/request-1/provider-0";
  protectedProjection.grantDigest = digest('9');
  const auto protectedFixed = nativeFixedRoleAssignmentV1FromProjection(protectedProjection);
  std::istringstream protectedInput(nativeSelectionInvocationV1ToJson(protectedProjection, *protectedFixed));
  const auto protectedDecoded = nativeSelectionInvocationV1FromJson(protectedInput, "S0R0", *protectedFixed);
  BOOST_CHECK_EQUAL(protectedDecoded.fixedRoleOwner.get(), protectedFixed->roleOwner().get());
  protectedProjection.grantDigest = "invalid-digest";
  BOOST_CHECK_THROW(nativeSelectionInvocationV1ToJson(protectedProjection, *protectedFixed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeFixedRoleCacheScopesIdentityReplacementAndLifetime)
{
  const auto projection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(projection);
  NativeFixedRoleAssignmentCache cache(1);
  const auto installed = cache.remember("/user/A", "/service", fixed);
  BOOST_CHECK_EQUAL(installed.get(), fixed.get());
  BOOST_CHECK_EQUAL(cache.find("/user/A", "/service", fixed->provider(), "S0R0", fixed->digest()).get(), fixed.get());
  BOOST_CHECK_EQUAL(cache.findMatching("/user/A", "/service", fixed->provider(), projection.selectedRole).get(), fixed.get());
  BOOST_CHECK(!cache.find("/user/B", "/service", fixed->provider(), "S0R0", fixed->digest()));
  BOOST_CHECK(!cache.find("/user/A", "/foreign-service", fixed->provider(), "S0R0", fixed->digest()));
  BOOST_CHECK(!cache.find("/user/A", "/service", "/provider/foreign", "S0R0", fixed->digest()));
  BOOST_CHECK(!cache.find("/user/A", "/service", fixed->provider(), "S0R0", digest('f')));
  const auto duplicate = nativeFixedRoleAssignmentV1FromProjection(projection);
  BOOST_CHECK_EQUAL(cache.remember("/user/A", "/service", duplicate).get(), fixed.get());

  auto changed = projection;
  changed.selectedRole.protectionEpoch = "policy-epoch-7";
  changed.assembly.protectionEpoch = "policy-epoch-7";
  changed.hasGrantBinding = true;
  changed.grantName = "/authority/grants/request-1/provider-0";
  changed.grantDigest = digest('9');
  const auto replacement = nativeFixedRoleAssignmentV1FromProjection(changed);
  BOOST_CHECK(!cache.findMatching("/user/A", "/service", fixed->provider(), changed.selectedRole));
  BOOST_CHECK_EQUAL(cache.remember("/user/A", "/service", replacement).get(), replacement.get());
  BOOST_CHECK(!cache.find("/user/A", "/service", fixed->provider(), "S0R0", fixed->digest()));
  BOOST_CHECK_EQUAL(installed->digest(), fixed->digest());
  BOOST_CHECK(installed->roleOwner()->nodeIndices == projection.selectedRole.nodeIndices);
  cache.remember("/user/B", "/service", fixed);
  BOOST_CHECK(!cache.find("/user/A", "/service", replacement->provider(), "S0R0", replacement->digest()));
  cache.clear();
  BOOST_CHECK(!cache.find("/user/B", "/service", fixed->provider(), "S0R0", fixed->digest()));
  BOOST_CHECK_EQUAL(installed->roleOwner().get(), fixed->roleOwner().get());

  NativeFixedRoleAssignmentCache expiring(1, std::chrono::milliseconds(1));
  expiring.remember("/user/A", "/service", fixed);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  BOOST_CHECK(!expiring.find("/user/A", "/service", fixed->provider(), "S0R0", fixed->digest()));
  BOOST_CHECK_THROW(NativeFixedRoleAssignmentCache(0), std::invalid_argument);
  BOOST_CHECK_THROW(NativeFixedRoleAssignmentCache(1025), std::invalid_argument);
  BOOST_CHECK_THROW(NativeFixedRoleAssignmentCache(1, std::chrono::milliseconds(0)), std::invalid_argument);
  BOOST_CHECK_THROW(cache.remember("", "/service", fixed), std::invalid_argument);
  BOOST_CHECK_THROW(cache.remember("/user/A", "/service", nullptr), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeFixedExecutionTemplateBindsCurrentGenerationWithoutInterfaceWire)
{
  const auto projection = parseProjection(streamingProjectionJson());
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(projection);
  const auto root = nativeParseJson(nativeSelectionInvocationV1ToJson(projection, *fixed));
  BOOST_CHECK(!root.at("generation_contract").contains("state_input_names"));
  BOOST_CHECK(!root.at("generation_contract").contains("token_input_name"));
  BOOST_CHECK(!root.at("dependencies").at(0).contains("producers"));
  BOOST_CHECK(!root.at("dataflow").contains("role"));
  BOOST_CHECK(!root.at("dataflow").contains("terminal_response_owner"));
  const auto decode = [&](const NativeJson& value) {
    std::istringstream input(nativeCanonicalJson(value));
    return nativeSelectionInvocationV1FromJson(input, "S0R0", *fixed);
  };
  const auto decoded = decode(root);
  BOOST_CHECK(decoded.generationContract.stateInputNames == projection.generationContract.stateInputNames);
  BOOST_CHECK_EQUAL(decoded.fixedExecutionTemplateOwner.get(), fixed->executionTemplate().get());
  BOOST_CHECK_EQUAL(&decoded.providerOwnership(), &fixed->executionTemplate()->providerByRole);
  auto current = projection;
  current.requestId = current.dataflow.requestId = "/request/next-generation";
  current.plan.dependencies.front().keyScope = "next-token-feedback";
  current.generationContract.samplingSeed = 91;
  BOOST_CHECK(fixed->matchesExecutionTemplate(current));
  const auto next = decode(nativeParseJson(nativeSelectionInvocationV1ToJson(current, *fixed)));
  BOOST_CHECK_EQUAL(next.plan.dependencies.front().keyScope, "next-token-feedback");
  BOOST_CHECK_EQUAL(next.generationContract.samplingSeed, 91);
  BOOST_CHECK_EQUAL(next.fixedExecutionTemplateOwner.get(), decoded.fixedExecutionTemplateOwner.get());
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1FromProjection(current)->digest(), fixed->digest());
  auto changed = current;
  changed.generationContract.stateInputNames.front() = "foreign-state";
  BOOST_CHECK(!fixed->matchesExecutionTemplate(changed));
  BOOST_CHECK_THROW(nativeSelectionInvocationV1ToJson(changed, *fixed), std::invalid_argument);
  auto bad = root;
  bad["generation_contract"]["token_input_name"] = "foreign-input";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["dependencies"][0]["producers"] = NativeJson::array({"foreign"});
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["dependencies"][0]["key_scope"] = "";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad.erase("generation_contract");
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  NativeFixedRoleAssignmentCache cache(1);
  cache.remember("/user", "/service", fixed);
  BOOST_CHECK_EQUAL(cache.findMatching("/user", "/service", current).get(), fixed.get());
  cache.clear();
  BOOST_CHECK_EQUAL(next.fixedExecutionTemplateOwner.get(), fixed->executionTemplate().get());
  BOOST_CHECK_EQUAL(next.fixedRoleReference, fixed->digest());
  auto differentOwners = projection;
  differentOwners.fixedProviderByRole = {{"S0R0", projection.provider}, {"S1R0", "/provider/peer-1"}};
  const auto withPeer = nativeFixedRoleAssignmentV1FromProjection(differentOwners);
  differentOwners.fixedProviderByRole.at("S1R0") = "/provider/peer-2";
  BOOST_CHECK(!withPeer->matchesExecutionTemplate(differentOwners));
  const auto changedPeer = nativeFixedRoleAssignmentV1FromProjection(differentOwners);
  BOOST_CHECK_NE(changedPeer->digest(), withPeer->digest());
  BOOST_CHECK_EQUAL(withPeer->executionTemplate()->providerByRole.at("S1R0"), "/provider/peer-1");
  cache.remember("/user", "/service", changedPeer);
  BOOST_CHECK_EQUAL(next.fixedExecutionTemplateOwner.get(), fixed->executionTemplate().get());
  BOOST_CHECK_EQUAL(next.fixedRoleReference, fixed->digest());
}

BOOST_AUTO_TEST_CASE(NativeFixedTemplateReusesBaseEpochAcrossTokenBudgets)
{
  const auto make = [](std::size_t budget) {
    auto current = parseProjection(streamingProjectionJson());
    current.generationContract.maxGeneratedTokens = budget;
    for (std::size_t epoch = 0; epoch <= budget; ++epoch) {
      auto transfer = endpoint("S0R0", "S0R0", nativePlanningDigest("fetch-" + std::to_string(epoch)));
      transfer.consumerRoles = {"S0R0"};
      transfer.requestId = current.requestId;
      transfer.planDigest = current.planDigest;
      transfer.round = epoch;
      transfer.namingOperationStride = current.generationContract.streamingOperationStride;
      certifyNamingEndpoint(transfer);
      current.dataflow.mustFetch.push_back(transfer);
      transfer.endpointDigest = nativePlanningDigest("publish-" + std::to_string(epoch));
      current.dataflow.mayPublish.push_back(std::move(transfer));
    }
    NativeReadinessPredicateV3 ready;
    ready.mode = "ALL";
    ready.endpointDigests = {current.dataflow.mustFetch.front().endpointDigest};
    current.dataflow.waitFor = {ready};
    return parseProjection(nativeSelectionProjectionV3ToJson(current));
  };
  const auto cold = make(2);
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(cold);
  BOOST_REQUIRE(fixed->executionTemplate()->expandRoleEpochs);
  BOOST_CHECK_EQUAL(fixed->executionTemplate()->dataflow.mustFetch.size(), 1U);
  BOOST_CHECK_EQUAL(fixed->executionTemplate()->dataflow.mayPublish.size(), 1U);
  std::istringstream source(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restored = nativeFixedRoleAssignmentV1FromJson(source);
  BOOST_CHECK(restored->executionTemplate()->expandRoleEpochs);
  BOOST_CHECK_EQUAL(restored->digest(), fixed->digest());
  const auto warm = make(4);
  BOOST_CHECK(restored->matchesExecutionTemplate(warm));
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1FromProjection(warm)->digest(), fixed->digest());
  const auto wire = nativeParseJson(nativeSelectionInvocationV1ToJson(warm, *restored));
  const auto decode = [&](const NativeJson& value) {
    std::istringstream input(nativeCanonicalJson(value));
    return nativeSelectionInvocationV1FromJson(input, "S0R0", *restored);
  };
  const auto decoded = decode(wire);
  BOOST_REQUIRE_EQUAL(decoded.dataflow.mustFetch.size(), 5U);
  BOOST_REQUIRE_EQUAL(decoded.dataflow.mayPublish.size(), 5U);
  for (std::size_t epoch = 0; epoch < 5; ++epoch) {
    BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[epoch].round, epoch);
    BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[epoch].endpointDigest, warm.dataflow.mustFetch[epoch].endpointDigest);
  }
  BOOST_CHECK_EQUAL(decoded.dataflow.waitFor.front().endpointDigests.front(),
                    decoded.dataflow.mustFetch.front().endpointDigest);
  BOOST_CHECK_EQUAL(decoded.fixedExecutionTemplateOwner.get(), restored->executionTemplate().get());
  auto changed = warm;
  changed.dataflow.mustFetch.back().tensorId = "different-interface";
  BOOST_CHECK(!restored->matchesExecutionTemplate(changed));
  changed = warm;
  changed.dataflow.mustFetch.pop_back();
  BOOST_CHECK(!restored->matchesExecutionTemplate(changed));
  auto bad = wire;
  bad["dataflow"]["must_fetch"].erase(4);
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = wire;
  bad["dataflow"]["wait_for"][0][0] = warm.dataflow.mustFetch.back().endpointDigest;
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  const auto irregular = parseProjection(streamingProjectionJson());
  BOOST_CHECK(!nativeFixedRoleAssignmentV1FromProjection(irregular)->executionTemplate()->expandRoleEpochs);
  auto sourceRoot = nativeParseJson(nativeFixedRoleAssignmentV1ToJson(*restored));
  sourceRoot["execution_template"]["expand_role_epochs"] = "true";
  std::istringstream badSource(nativeCanonicalJson(sourceRoot));
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromJson(badSource), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeFixedTemplateRepeatedInterfacesKeepDistinctSlots)
{
  auto current = parseProjection(projectionJson());
  auto first = endpoint("S0R0", "S0R0", digest('1'));
  first.consumerRoles = {"S0R0"};
  first.planDigest = current.planDigest;
  first.requestId = current.requestId;
  auto second = first;
  second.microbatch = 1;
  second.endpointDigest = digest('2');
  current.dataflow.mustFetch = {first, second};
  NativeReadinessPredicateV3 ready;
  ready.mode = "ALL";
  ready.endpointDigests = {first.endpointDigest, second.endpointDigest};
  current.dataflow.waitFor = {ready};
  // Establish that the pre-existing full V3 path accepts this contract.
  current = parseProjection(nativeSelectionProjectionV3ToJson(current));
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(current);
  std::istringstream source(nativeFixedRoleAssignmentV1ToJson(*fixed));
  const auto restored = nativeFixedRoleAssignmentV1FromJson(source);
  const auto& flow = restored->executionTemplate()->dataflow;
  BOOST_REQUIRE_EQUAL(flow.mustFetch.size(), 2U);
  BOOST_CHECK_NE(flow.mustFetch[0].endpointDigest, flow.mustFetch[1].endpointDigest);
  BOOST_CHECK_EQUAL(flow.waitFor.front().endpointDigests[0], flow.mustFetch[0].endpointDigest);
  BOOST_CHECK_EQUAL(flow.waitFor.front().endpointDigests[1], flow.mustFetch[1].endpointDigest);
  BOOST_CHECK_EQUAL(restored->digest(), fixed->digest());
  std::istringstream invocation(nativeSelectionInvocationV1ToJson(current, *restored));
  const auto decoded = nativeSelectionInvocationV1FromJson(invocation, "S0R0", *restored);
  BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[0].microbatch, 0U);
  BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[1].microbatch, 1U);
  BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[0].endpointDigest, first.endpointDigest);
  BOOST_CHECK_EQUAL(decoded.dataflow.mustFetch[1].endpointDigest, second.endpointDigest);
  BOOST_CHECK_EQUAL(decoded.fixedExecutionTemplateOwner.get(), restored->executionTemplate().get());
  current.dataflow.mustFetch[0].microbatch = 7;
  current.dataflow.mustFetch[1].microbatch = 8;
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1FromProjection(current)->digest(), fixed->digest());
}

BOOST_AUTO_TEST_CASE(NativeFixedRoleSourceRoundTripAndServingLifetime)
{
  const auto projection = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(projection);
  const auto wire = nativeFixedRoleAssignmentV1ToJson(*fixed);
  const auto root = nativeParseJson(wire);
  BOOST_CHECK_EQUAL(root.size(), 7U);
  BOOST_CHECK_EQUAL(root.at("schema").get<std::string>(), "ndnsf-di-fixed-assignment-v1");
  BOOST_REQUIRE(root.contains("execution_template"));
  BOOST_CHECK_EQUAL(root.at("execution_template").at("dataflow").at("request_id").get<std::string>(),
                    "/fixed-template");
  auto nextRequest = projection;
  nextRequest.requestId = nextRequest.dataflow.requestId = "/request/next";
  const auto nextFixed = nativeFixedRoleAssignmentV1FromProjection(nextRequest);
  BOOST_CHECK_EQUAL(nextFixed->digest(), fixed->digest());
  for (const auto* field : {"request_id", "grant_name", "grant_digest", "group_capability", "deadline_ms"})
    BOOST_CHECK(!root.contains(field));
  const auto decode = [](const NativeJson& value) {
    std::istringstream input(nativeCanonicalJson(value));
    return nativeFixedRoleAssignmentV1FromJson(input);
  };
  const auto restored = decode(root);
  BOOST_CHECK_EQUAL(restored->provider(), fixed->provider());
  BOOST_CHECK_EQUAL(restored->digest(), fixed->digest());
  BOOST_CHECK(nativeSelectionRoleContractsEqualV3(*restored->roleOwner(), *fixed->roleOwner()));
  BOOST_REQUIRE(restored->executionTemplate());
  BOOST_CHECK_EQUAL(restored->executionTemplate()->dataflow.role, projection.executionRole.roleId);
  BOOST_CHECK_EQUAL(restored->executionTemplate()->dataflow.requestId, "/fixed-template");
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1ToJson(*restored), wire);
  BOOST_CHECK_NE(restored->roleOwner().get(), fixed->roleOwner().get());
  BOOST_CHECK(restored->roleOwner()->expectedInputs.front().shape ==
              fixed->roleOwner()->expectedInputs.front().shape);
  auto symbolicProjection = projection;
  symbolicProjection.selectedRole.expectedInputs.front().shape[1] = std::string("4");
  symbolicProjection.assembly.expectedInputs.front().shape[1] = std::string("4");
  const auto symbolicFixed = nativeFixedRoleAssignmentV1FromProjection(symbolicProjection);
  const auto symbolicRestored = decode(nativeParseJson(nativeFixedRoleAssignmentV1ToJson(*symbolicFixed)));
  BOOST_CHECK(symbolicRestored->roleOwner()->expectedInputs.front().shape ==
              symbolicFixed->roleOwner()->expectedInputs.front().shape);
  BOOST_CHECK_NE(symbolicFixed->digest(), fixed->digest());
  auto bad = root;
  bad["provider"] = "/provider/foreign";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["selected_role"] = "foreign-role";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["digest"] = digest('f');
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["request_id"] = "/stale-request";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["role"]["unknown_fixed_field"] = 1;
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_template"]["dataflow"]["request_id"] = "/stale-request";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["execution_template"]["unknown_fixed_field"] = 1;
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  bad = root;
  bad["schema_version"] = "1";
  BOOST_CHECK_THROW(decode(bad), std::invalid_argument);

  for (const auto version : {std::uint64_t{2}, std::uint64_t{4294967297}}) {
    bad = root;
    bad["schema_version"] = version;
    BOOST_CHECK_THROW(decode(bad), std::invalid_argument);
  }
  NativeFixedRoleAssignmentCache cache(1);
  cache.remember("/user", "/service", fixed);
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> weakLease = lease;
  NativeFixedRoleSourceV1 source;
  source.plaintextDigest = nativePlanningDigest(wire);
  const auto sourceName = [&](const std::string& producer, const std::string& category,
                              const std::string& role, char protection) {
    return ndn::Name(producer).append("NDNSF-DI").append(category).append("model")
      .append(ndn_service_framework::makeNamingLabelComponent("fixture-model"))
      .append("ROLE").append(ndn_service_framework::makeNamingLabelComponent(role))
      .append(ndn_service_framework::makeNamingDigestComponent(source.plaintextDigest.substr(7)))
      .append("PROTECTED").append(ndn_service_framework::makeNamingDigestComponent(std::string(64, protection)))
      .append("DATA").toUri();
  };
  source.dataName = sourceName("/user", "assignment", "S0R0", 'a');
  source.plaintextSize = wire.size();
  source.servingLease = lease;
  auto pinned = cache.rememberSource("/user", "/service", *fixed, source);
  BOOST_CHECK_EQUAL(cache.source("/user", "/service", *fixed).get(), pinned.get());
  BOOST_CHECK(!cache.source("/other-user", "/service", *fixed));
  auto duplicate = source;
  duplicate.dataName = sourceName("/user", "assignment", "S0R0", 'b');
  BOOST_CHECK_EQUAL(cache.rememberSource("/user", "/service", *fixed, duplicate).get(), pinned.get());
  for (const auto& invalid : {sourceName("/foreign", "assignment", "S0R0", 'a'),
                              sourceName("/user", "input", "S0R0", 'a'),
                              sourceName("/user", "assignment", "S1R0", 'a')}) {
    auto rejected = source;
    rejected.dataName = invalid;
    BOOST_CHECK_THROW(nativeValidateFixedRoleSourceProducer(rejected, "/user", "S0R0"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(cache.rememberSource("/user", "/service", *fixed, rejected),
                      std::invalid_argument);
    BOOST_CHECK_EQUAL(cache.source("/user", "/service", *fixed).get(), pinned.get());
  }
  auto wrongCommitment = source;
  wrongCommitment.plaintextDigest = digest('f');
  BOOST_CHECK_THROW(nativeValidateFixedRoleSourceProducer(wrongCommitment, "/user", "S0R0"),
                    std::invalid_argument);
  wrongCommitment.servingLease.reset();
  const auto invocation = nativeParseJson(nativeSelectionInvocationV1ToJson(projection, *fixed, pinned.get()));
  std::istringstream requestWire(nativeCanonicalJson(invocation));
  const auto runningRequest = nativeSelectionInvocationV1FromJson(requestWire, "S0R0", *fixed);
  BOOST_CHECK_EQUAL(runningRequest.fixedExecutionTemplateOwner.get(), fixed->executionTemplate().get());
  BOOST_CHECK_EQUAL(invocation.at("fixed_role_source").at("data_name").get<std::string>(), source.dataName);
  BOOST_CHECK_EQUAL(invocation.at("fixed_role_source").size(), 3U);
  duplicate.servingLease.reset();
  source.servingLease.reset();
  lease.reset();
  cache.clear();
  // Clearing shared reuse storage must not change an in-flight request.
  BOOST_CHECK_EQUAL(runningRequest.fixedRoleOwner.get(), fixed->roleOwner().get());
  BOOST_CHECK_EQUAL(runningRequest.fixedExecutionTemplateOwner.get(), fixed->executionTemplate().get());
  BOOST_CHECK_EQUAL(runningRequest.fixedRoleReference, fixed->digest());
  BOOST_CHECK(!cache.source("/user", "/service", *fixed));
  BOOST_CHECK(!weakLease.expired());
  pinned.reset();
  BOOST_CHECK(weakLease.expired());
  source.dataName = "relative-source";
  BOOST_CHECK_THROW(cache.rememberSource("/user", "/service", *fixed, source), std::invalid_argument);
  source.dataName = "/source";
  source.plaintextSize = (4U << 20) + 1;
  BOOST_CHECK_THROW(cache.rememberSource("/user", "/service", *fixed, source), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionPreservesNumericAndSymbolicDimensions)
{
  auto wire = projectionJson(1, {}, "CPU", {}, false, true);
  const std::string before = "\"shape\":[1,4]";
  const std::string after = "\"shape\":[1,\"4\"]";
  std::size_t position = 0;
  while ((position = wire.find(before, position)) != std::string::npos) {
    wire.replace(position, before.size(), after);
    position += after.size();
  }
  const auto parsed = parseProjection(wire);
  const auto& shape = parsed.assembly.expectedInputs.front().shape;
  BOOST_REQUIRE_EQUAL(shape.size(), 2U);
  BOOST_CHECK_EQUAL(std::get<std::int64_t>(shape.at(0)), 1);
  BOOST_CHECK_EQUAL(std::get<std::string>(shape.at(1)), "4");
  const auto encoded = nativeSelectionProjectionV3ToJson(parsed);
  const auto roundTrip = parseProjection(encoded);
  BOOST_CHECK(roundTrip.selectedRole.expectedInputs.front().shape == shape);
  BOOST_CHECK(roundTrip.assembly.expectedInputs.front().shape == shape);
  auto mismatched = parsed;
  mismatched.assembly.expectedInputs.front().shape[1] = std::int64_t{4};
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(mismatched), std::invalid_argument);
  // Equal text is not equal wire identity: roles and assembly must agree on type.
  wire.replace(wire.find(after), after.size(), before);
  BOOST_CHECK_THROW(parseProjection(wire), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeCanonicalIntegerFormattingPreservesBoundaryBytes)
{
  for (const auto value : {std::numeric_limits<std::int64_t>::min(),
                          std::int64_t{-1}, std::int64_t{0}, std::int64_t{1},
                          std::numeric_limits<std::int64_t>::max()}) {
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(value)), std::to_string(value));
  }
  for (const auto value : {std::uint64_t{0}, std::uint64_t{1},
                          std::numeric_limits<std::uint64_t>::max()}) {
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(value)), std::to_string(value));
  }
  const NativeJson value{{"unsigned", std::numeric_limits<std::uint64_t>::max()},
                        {"signed", std::numeric_limits<std::int64_t>::min()},
                        {"values", NativeJson::array({std::int64_t{-1}, std::uint64_t{0}, 1.0, -0.0})}};
  BOOST_CHECK_EQUAL(nativeCanonicalJson(value),
    "{\"signed\":-9223372036854775808,\"unsigned\":18446744073709551615,\"values\":[-1,0,1.0,-0.0]}");
  struct Grouped : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
  };
  struct RestoreLocale {
    std::locale previous = std::locale();
    ~RestoreLocale() { std::locale::global(previous); }
  } restore;
  const auto expected = nativeCanonicalJson(value);
  std::locale::global(std::locale(std::locale::classic(), new Grouped));
  BOOST_CHECK_EQUAL(nativeCanonicalJson(value), expected);
}

BOOST_AUTO_TEST_CASE(NativeGeneratedProjectionKeepsOwnedTypedShapes)
{
  auto source = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const std::vector<std::variant<std::int64_t, std::string>> dimensions{
    std::numeric_limits<std::int64_t>::min(),
    std::numeric_limits<std::int64_t>::max(), std::string("4"),
    std::string("batch\"\\symbol")};
  source.selectedRole.expectedInputs.front().shape = dimensions;
  source.assembly.expectedInputs.front().shape = dimensions;
  const auto wire = nativeSelectionProjectionV3ToJson(source);
  const auto parsed = parseProjection(wire);
  BOOST_CHECK(parsed.selectedRole.expectedInputs.front().shape == dimensions);
  BOOST_CHECK(parsed.assembly.expectedInputs.front().shape == dimensions);
  BOOST_CHECK(source.selectedRole.expectedInputs.front().shape == dimensions);
  source.selectedRole.expectedInputs.front().shape.clear();
  BOOST_CHECK(parsed.selectedRole.expectedInputs.front().shape == dimensions);

  auto value = nativeParseJson(wire);
  // Parsing positive JSON integers changes signedness. This internal path
  // expects the generator's typed values, so restore both role values first
  // and prove the unchanged control succeeds before testing substitutions.
  value["roles"][0] = nativeAssemblyJson(parsed.selectedRole);
  value["assembly"] = nativeAssemblyJson(parsed.assembly);
  BOOST_REQUIRE_NO_THROW(detail::validateNativeSelectionProjectionV3Value(value, parsed));
  for (const auto& invalid : {NativeJson(true), NativeJson(1.0), NativeJson(nullptr),
                             NativeJson(std::numeric_limits<std::uint64_t>::max())}) {
    auto changed = value;
    changed["roles"][0]["expected_inputs"][0]["shape"][0] = invalid;
    BOOST_CHECK_THROW(detail::validateNativeSelectionProjectionV3Value(changed, parsed),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(NativeGeneratedProjectionEncoderCpuSample)
{
  const auto source = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  const auto expected = nativeSelectionProjectionV3ToJson(source);
  constexpr std::size_t iterations = 512;
  bool identical = true;
  const auto started = std::clock();
  BOOST_REQUIRE(started != std::clock_t(-1));
  for (std::size_t i = 0; i < iterations; ++i) {
    const auto wire = nativeSelectionProjectionV3ToJson(source);
    identical = (wire == expected) && identical;
  }
  const auto ended = std::clock();
  BOOST_REQUIRE(ended != std::clock_t(-1) && ended >= started);
  const auto elapsed = ended - started;
  BOOST_CHECK(identical);
  std::cout << "SPEC199_ENCODER_CPU_SAMPLE iterations=" << iterations
            << " process_cpu_ms=" << (1000.0 * elapsed / CLOCKS_PER_SEC) << '\n';
  std::cout << "SPEC199_ENCODER_WIRE_SAMPLE " << expected << '\n';
}

BOOST_AUTO_TEST_CASE(NativeGeneratedProjectionRejectsSemanticSubstitution)
{
  const auto source = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  BOOST_REQUIRE_NO_THROW(nativeSelectionProjectionV3ToJson(source));
  auto invalid = source;
  invalid.deadlineMs = 0;
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(invalid), std::invalid_argument);
  invalid = source;
  invalid.deviceBinding.offerDigest = digest('9');
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(invalid), std::invalid_argument);
  invalid = source;
  ++invalid.assembly.layerEnd;
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(invalid), std::invalid_argument);
  invalid = source;
  invalid.selectedRole.expectedInputs.front().dtype.clear();
  invalid.assembly.expectedInputs.front().dtype.clear();
  BOOST_CHECK_THROW(nativeSelectionProjectionV3ToJson(invalid), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeGeneratedProjectionPreservesShapesWithNonClassicLocale)
{
  struct Comma final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  struct RestoreLocale {
    std::locale previous = std::locale();
    ~RestoreLocale() { std::locale::global(previous); }
  } restore;
  auto source = parseProjection(projectionJson(1, {}, "CPU", {}, false, true));
  source.selectedRole.expectedInputs.front().shape[1] = std::string("4");
  source.assembly.expectedInputs.front().shape[1] = std::string("4");
  const auto expected = nativeSelectionProjectionV3ToJson(source);
  std::locale::global(std::locale(std::locale::classic(), new Comma));
  const auto wire = nativeSelectionProjectionV3ToJson(source);
  BOOST_CHECK_EQUAL(wire, expected);
  const auto parsed = parseProjection(wire);
  BOOST_CHECK(parsed.selectedRole.expectedInputs.front().shape ==
              source.selectedRole.expectedInputs.front().shape);
  BOOST_CHECK(parsed.assembly.expectedInputs.front().shape ==
              source.assembly.expectedInputs.front().shape);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionRejectsNonIntegerDimensions)
{
  for (const auto& shape : {"[true,4]", "[1.0,4]", "[null,4]", "[18446744073709551615,4]"}) {
    auto wire = projectionJson(1, {}, "CPU", {}, false, true);
    const std::string before = "\"shape\":[1,4]";
    const std::string after = std::string("\"shape\":") + shape;
    std::size_t position = 0;
    while ((position = wire.find(before, position)) != std::string::npos) {
      wire.replace(position, before.size(), after);
      position += after.size();
    }
    BOOST_CHECK_THROW(parseProjection(wire), std::invalid_argument);
  }
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

BOOST_AUTO_TEST_CASE(NativeV3GenerationSelectsOnlySealedEpochEndpoints)
{
  auto values = validProjectionSet();
  for (auto& value : values) {
    value.generationContract.enabled = true;
    value.generationContract.maxGeneratedTokens = 1;
    value.generationContract.streamingOperationStride = 4;
  }
  values[0].dataflow.mayPublish.front().round = 1;
  values[1].dataflow.mustFetch.front().round = 1;
  auto finalEndpoint = values[0].dataflow.mayPublish.front();
  finalEndpoint.round = 5;
  // Explicit sealed fixture identities: this case tests selection, not the
  // trusted builder's canonical digest calculation.
  finalEndpoint.endpointDigest = digest('1');
  finalEndpoint.manifestDigest = digest('2');
  values[0].dataflow.mayPublish.push_back(finalEndpoint);
  values[1].dataflow.mustFetch.push_back(finalEndpoint);
  for (auto& value : values) certifyNamingFlow(value);
  finalEndpoint = values[0].dataflow.mayPublish.back();
  for (std::uint64_t epoch = 0; epoch != 2; ++epoch) {
    const auto source = roleSpecFromSelectionProjectionV3(values[0], values[0].provider, epoch);
    const auto sink = roleSpecFromSelectionProjectionV3(values[1], values[1].provider, epoch);
    BOOST_REQUIRE_EQUAL(source.outputs.size(), 1);
    BOOST_REQUIRE_EQUAL(sink.inputs.size(), 1);
    BOOST_CHECK_EQUAL(source.outputs[0].collectiveOperationIndex, 1 + 4 * epoch);
    BOOST_CHECK_EQUAL(source.outputs[0].plannedDataName, sink.inputs[0].plannedDataName);
    BOOST_CHECK_EQUAL(source.outputs[0].endpointDigest, sink.inputs[0].endpointDigest);
    BOOST_CHECK_EQUAL(source.outputs[0].manifestContractDigest, sink.inputs[0].manifestContractDigest);
  }
  const auto prefill = roleSpecFromSelectionProjectionV3(values[0], values[0].provider);
  const auto finalize = roleSpecFromSelectionProjectionV3(values[0], values[0].provider, 1);
  BOOST_CHECK_NE(prefill.outputs[0].plannedDataName, finalize.outputs[0].plannedDataName);
  BOOST_CHECK_NE(prefill.outputs[0].endpointDigest, finalize.outputs[0].endpointDigest);
  auto incomplete = values[0];
  incomplete.dataflow.mayPublish.pop_back();
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(incomplete, incomplete.provider, 0), std::invalid_argument);
  auto repeated = values[0];
  repeated.dataflow.mayPublish.push_back(finalEndpoint);
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(repeated, repeated.provider, 1), std::invalid_argument);
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(values[0], values[0].provider, 2), std::invalid_argument);
  values[0].generationContract.streamingOperationStride = 0;
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(values[0], values[0].provider, 0), std::invalid_argument);
  values[0].generationContract.enabled = false;
  BOOST_CHECK_THROW(roleSpecFromSelectionProjectionV3(values[0], values[0].provider, 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeV3SelectionRoleFactoryReusesValidatedGenerationContract)
{
  auto values = validProjectionSet();
  for (auto& value : values) {
    value.generationContract.enabled = true;
    value.generationContract.maxGeneratedTokens = 1;
    value.generationContract.streamingOperationStride = 4;
  }
  values[0].dataflow.mayPublish.front().round = 1;
  values[1].dataflow.mustFetch.front().round = 1;
  auto finalEndpoint = values[0].dataflow.mayPublish.front();
  finalEndpoint.round = 5;
  finalEndpoint.endpointDigest = digest('1');
  finalEndpoint.manifestDigest = digest('2');
  values[0].dataflow.mayPublish.push_back(finalEndpoint);
  values[1].dataflow.mustFetch.push_back(finalEndpoint);
  for (auto& value : values) certifyNamingFlow(value);

  NativeSelectionProjectionRoleFactory factory(values[0], values[0].provider);
  const auto factoryPrefill = factory(0);
  const auto factoryFinalize = factory(1);
  const auto directPrefill = roleSpecFromSelectionProjectionV3(
    values[0], values[0].provider, 0);
  const auto directFinalize = roleSpecFromSelectionProjectionV3(
    values[0], values[0].provider, 1);

  BOOST_REQUIRE_EQUAL(factoryPrefill.outputs.size(), 1);
  BOOST_REQUIRE_EQUAL(factoryFinalize.outputs.size(), 1);
  BOOST_CHECK_EQUAL(factoryPrefill.outputs[0].plannedDataName,
                    directPrefill.outputs[0].plannedDataName);
  BOOST_CHECK_EQUAL(factoryPrefill.outputs[0].endpointDigest,
                    directPrefill.outputs[0].endpointDigest);
  BOOST_CHECK_EQUAL(factoryFinalize.outputs[0].plannedDataName,
                    directFinalize.outputs[0].plannedDataName);
  BOOST_CHECK_EQUAL(factoryFinalize.outputs[0].endpointDigest,
                    directFinalize.outputs[0].endpointDigest);
  BOOST_CHECK_NE(factoryPrefill.outputs[0].plannedDataName,
                 factoryFinalize.outputs[0].plannedDataName);
}

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSeparatesMultiplePipelineTensors)
{
  auto source = projection("/provider/0", "S0R0", false);
  auto sink = projection("/provider/1", "S1R0", true);
  auto first = endpoint("S0R0", "S1R0", digest('f'));
  auto second = endpoint("S0R0", "S1R0", digest('1'));
  second.tensorId = "activation-2";
  certifyNamingEndpoint(first); certifyNamingEndpoint(second);
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
  auto identity = nativeEndpointJson(value);
  identity.erase("endpoint_digest"); identity.erase("consumer_role");
  value.endpointDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  const auto prefix = tensorObjectNamePrefix(value);
  const ndn::Name parsed(prefix);
  BOOST_REQUIRE_EQUAL(parsed.size(), 22U);
  BOOST_CHECK(ndn::Name("/provider/0").isPrefixOf(parsed));
  BOOST_CHECK(parsed.get(2) == ndn::name::Component("NDNSF-DI"));
  BOOST_CHECK(parsed.get(3) == ndn::name::Component("tensor"));
  BOOST_CHECK(parsed.get(4) == ndn::name::Component("REQUEST"));
  BOOST_CHECK(parsed.get(5) == ndn_service_framework::makeNamingLabelComponent("/8=request/8=1"));
  BOOST_CHECK(parsed.get(7) == ndn::name::Component("1"));
  BOOST_CHECK(parsed.get(9) == ndn::name::Component("0"));
  BOOST_CHECK(parsed.get(11) == ndn_service_framework::makeNamingLabelComponent("S0R0"));
  BOOST_CHECK(ndn::Name(tensorObjectManifestName(value)) == ndn::Name(prefix).append("MANIFEST"));
  BOOST_CHECK(ndn::Name(tensorObjectSegmentName(value, 0)) == ndn::Name(prefix).append("DATA").appendSegment(0));
  BOOST_CHECK(ndn::Name(tensorObjectSegmentName(value, 1)) == ndn::Name(prefix).append("DATA").appendSegment(1));
  BOOST_CHECK_THROW(tensorObjectSegmentName(value, 2), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(NativeTensorC04WireGoldensBindCompleteEndpointAndSignedStride)
{
  auto value = endpoint(std::string(80, 'r'), "sink", digest('f'));
  value.requestId = "/request/54=%00/50=%01%00";
  value.round = 9; value.namingOperationStride = 4;
  value.producerRank = 2; value.microbatch = 3;
  value.segmentCount = 512; value.tensorId = std::string(96, 't');
  value.bundleTensorNames = {"α", "hidden"};
  certifyNamingEndpoint(value);
  BOOST_CHECK_EQUAL(value.endpointDigest,
    "sha256:1592bea674cefe6332119e31b6d04c42a2acd24512dbfeda48b2969c0f465a20");
  BOOST_CHECK_EQUAL(computeTensorExecutionObjectDigest(value),
    "75044022a4a034031e2376d29fe686349867beac1b314767bd547ef6aa210364");
  const auto raw = [](const std::string& text) {
    return ndn::name::Component(ndn::tlv::GenericNameComponent, text.begin(), text.end());
  };
  // Independently assembled paper components and externally calculated C01
  // uint16/tag + uint8/kind + uint64/length transcript golden.
  ndn::Name expected("/provider/0/NDNSF-DI/tensor/REQUEST");
  expected.append(raw("name=/8=request/54=%00/50=%01%00"));
  expected.append("ATTEMPT").append("1").append("ROUND").append("2");
  expected.append("POSITION").append(raw("name=" + std::string(64, 'r')));
  expected.append("OP").append("PIPELINE").append("INDEX").append("1");
  expected.append("RANK").append("2").append("BATCH").append("3");
  expected.append(raw("name=" + std::string(64, 't')));
  const std::string key = "4fa08da752faefe896fabc2e823efd6de25173f72894352bc074483e6c3952f5";
  std::vector<std::uint8_t> keyBytes;
  for (std::size_t i = 0; i < key.size(); i += 2)
    keyBytes.push_back(static_cast<std::uint8_t>(std::stoul(key.substr(i, 2), nullptr, 16)));
  expected.append(ndn::name::Component(ndn::tlv::GenericNameComponent, keyBytes.begin(), keyBytes.end()));
  const auto actual = ndn::Name(tensorObjectNamePrefix(value));
  BOOST_CHECK(actual.wireEncode() == expected.wireEncode());
  BOOST_CHECK(ndn::Name(tensorObjectManifestName(value)).wireEncode() == ndn::Name(expected).append("MANIFEST").wireEncode());
  for (const auto index : {0U, 256U})
    BOOST_CHECK(ndn::Name(tensorObjectSegmentName(value, index)).wireEncode() == ndn::Name(expected).append("DATA").appendSegment(index).wireEncode());
  auto altered = value; altered.endpointDigest = digest('0');
  BOOST_CHECK_THROW(tensorObjectNamePrefix(altered), std::invalid_argument);
  altered = value; altered.namingOperationStride = 8;
  BOOST_CHECK_THROW(tensorObjectNamePrefix(altered), std::invalid_argument);
  altered = value; altered.producerRole.back() = 's'; certifyNamingEndpoint(altered);
  BOOST_CHECK(ndn::Name(tensorObjectNamePrefix(altered)) != actual);
  altered = value; altered.tensorId.back() = 'u'; certifyNamingEndpoint(altered);
  BOOST_CHECK(ndn::Name(tensorObjectNamePrefix(altered)) != actual);
  const auto hexWire = [](const ndn::Name& name) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (const auto byte : name.wireEncode()) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
  };
  if (const auto path = std::getenv("SPEC201_TENSOR_RECORDS")) {
    NativeJson record{{"endpoint", nativeEndpointJson(value)},
      {"prefix_wire", hexWire(actual)}, {"manifest_wire", hexWire(ndn::Name(tensorObjectManifestName(value)))},
      {"segment0_wire", hexWire(ndn::Name(tensorObjectSegmentName(value, 0)))},
      {"segment256_wire", hexWire(ndn::Name(tensorObjectSegmentName(value, 256)))},
      {"execution_object_digest", computeTensorExecutionObjectDigest(value)}};
    std::ofstream output(path); BOOST_REQUIRE(output.good());
    output << nativeCanonicalJson(record) << '\n'; BOOST_REQUIRE(output.good());
  }
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

BOOST_AUTO_TEST_CASE(NativeV3ProjectionSetAllowsProviderColocationButRejectsDuplicateRole)
{
  auto duplicateProvider = validProjectionSet();
  duplicateProvider[1].provider = duplicateProvider[0].provider;
  BOOST_CHECK_NO_THROW(validateNativeSelectionProjectionSetV3(duplicateProvider));

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
