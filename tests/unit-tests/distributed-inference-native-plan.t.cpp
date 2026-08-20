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
  runtime.verifyGrant(runtimeBinding, 1000);

  BOOST_CHECK(!validateProtectedRuntimeBinding(
    selected, runtime, nullptr, "boot-1", "fence-1"));
  BOOST_CHECK_EQUAL(
    *validateProtectedRuntimeBinding(
      selected, runtime, nullptr, "boot-wrong", "fence-1"),
    "DI_PROTECTED_RUNTIME_BINDING_MISMATCH");
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
