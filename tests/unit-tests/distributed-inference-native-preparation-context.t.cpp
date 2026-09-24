#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"

namespace ndnsf::di::test {

BOOST_AUTO_TEST_CASE(NativePreparationContextBindsBothModelAndPostprocessAdapters)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  projection.assembly.modelManifestDigest = "sha256:" + std::string(64, 'b');
  projection.assembly.artifactDigest = "sha256:" + std::string(64, 'c');
  const NativeRunnerPreparationContext context{
    "/provider/selected", "boot-1", 1234, "/private/cache"};
  std::string previousProfile;
  for (const auto* kind : {"onnx-model", "native-yolo-postprocess"}) {
    NativeModelRunnerSpec spec;
    spec.kind = kind;
    spec.role = "selected-role";
    spec.path = "adapter-owned-path";
    spec.metadata["stateMode"] = "adapter-owned-state-mode";
    spec.metadata["evidence.providerName"] = "/adapter-supplied-provider";
    spec.metadata["evidence.planDigest"] = "adapter-supplied-plan";
    bindNativeRunnerPreparationContext(spec, projection, context);
    BOOST_CHECK_EQUAL(spec.kind, kind);
    BOOST_CHECK_EQUAL(spec.role, "selected-role");
    BOOST_CHECK_EQUAL(spec.path, "adapter-owned-path");
    BOOST_CHECK_EQUAL(spec.metadata.at("stateMode"), "adapter-owned-state-mode");
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.providerName"), context.providerName);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.providerBootId"), context.providerBootId);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.createdAtMs"), "1234");
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.planDigest"), projection.planDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.modelDigest"), projection.assembly.modelManifestDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.artifactDigest"), projection.assembly.artifactDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("profileAfterRequest"), "true");
    const auto profile = spec.metadata.at("providerProfilePrefix");
    BOOST_CHECK_EQUAL(profile.find("/private/cache/ort-profile-"), 0);
    BOOST_CHECK_NE(profile, previousProfile);
    previousProfile = profile;
  }
}

BOOST_AUTO_TEST_CASE(NativePreparationContextPreservesPlanFallbackForPathlessRoles)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  NativeModelRunnerSpec spec;
  bindNativeRunnerPreparationContext(spec, projection,
    {"/provider/merge", "boot-1", 1234, "/private/cache"});
  BOOST_CHECK_EQUAL(spec.metadata.at("evidence.modelDigest"), projection.planDigest);
  BOOST_CHECK(spec.path.empty());
}

BOOST_AUTO_TEST_CASE(NativePreparationContextLeavesResidentSessionsUnprofiled)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  NativeModelRunnerSpec spec;
  spec.metadata["residentSession"] = "true";
  spec.metadata["providerProfilePrefix"] = "stale-profile";
  spec.metadata["profileAfterRequest"] = "true";
  bindNativeRunnerPreparationContext(spec, projection,
    {"/provider/resident", "boot-1", 1234, "/private/cache"});
  BOOST_CHECK(spec.metadata.find("providerProfilePrefix") == spec.metadata.end());
  BOOST_CHECK(spec.metadata.find("profileAfterRequest") == spec.metadata.end());
  BOOST_CHECK_EQUAL(spec.metadata.at("residentSession"), "true");
}

BOOST_AUTO_TEST_CASE(NativePreparationContextClearsAndBindsGenerationMetadata)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  projection.assembly.expectedInputs = {
    {"state_in", "float32", {}},
    {"attention_mask", "int64", {}},
    {"position_ids", "int64", {}}};
  projection.assembly.expectedOutputs = {{"state_out", "float32", {}}};
  projection.generationContract.enabled = true;
  projection.generationContract.stateInputNames = {"state_in"};
  projection.generationContract.stateOutputNames = {"state_out"};
  projection.generationContract.positionInputPolicy = "";
  NativeModelRunnerSpec spec;
  spec.metadata["stateSuccessorMap"] = "stale_in=stale_out";
  spec.metadata["kvTensorMap"] = "stale_in=stale_out";
  spec.metadata["positionInputPolicy"] = "stale-policy";
  spec.metadata["attentionMaskInputName"] = "stale-mask";
  spec.metadata["positionIdsInputName"] = "stale-position";
  spec.metadata["cachePositionInputName"] = "stale-cache";

  bindNativeRunnerPreparationContext(spec, projection,
    {"/provider/generation", "boot-1", 1234, "/private/cache"});
  BOOST_CHECK_EQUAL(spec.metadata.at("stateInputNames"), "state_in");
  BOOST_CHECK_EQUAL(spec.metadata.at("stateOutputNames"), "state_out");
  for (const auto* key : {"stateSuccessorMap", "kvTensorMap", "positionInputPolicy",
                          "attentionMaskInputName", "positionIdsInputName",
                          "cachePositionInputName"}) {
    BOOST_CHECK(spec.metadata.find(key) == spec.metadata.end());
  }

  projection.generationContract.stateSuccessorMap = "state_in=state_out";
  projection.generationContract.positionInputPolicy = "qwen-causal-position-v1";
  projection.generationContract.attentionMaskInputName = "attention_mask";
  projection.generationContract.positionIdsInputName = "position_ids";
  projection.generationContract.cachePositionInputName = "";
  bindNativeRunnerPreparationContext(spec, projection,
    {"/provider/generation", "boot-1", 1234, "/private/cache"});
  BOOST_CHECK_EQUAL(spec.metadata.at("stateSuccessorMap"), "state_in=state_out");
  BOOST_CHECK_EQUAL(spec.metadata.at("kvTensorMap"), "state_in=state_out");
  BOOST_CHECK_EQUAL(spec.metadata.at("positionInputPolicy"),
                    "qwen-causal-position-v1");
  BOOST_CHECK_EQUAL(spec.metadata.at("attentionMaskInputName"), "attention_mask");
  BOOST_CHECK_EQUAL(spec.metadata.at("positionIdsInputName"), "position_ids");
  BOOST_CHECK(spec.metadata.find("cachePositionInputName") == spec.metadata.end());
}

BOOST_AUTO_TEST_CASE(NativePreparationContextProjectsPositionPolicyToLocalInputs)
{
  NativeSelectionProjectionV3 projection;
  projection.generationContract.enabled = true;
  projection.generationContract.stateInputNames = {"state_in"};
  projection.generationContract.stateOutputNames = {"state_out"};
  projection.generationContract.stateSuccessorMap = "state_in=state_out";
  projection.generationContract.positionInputPolicy = "qwen-causal-position-v1";
  projection.generationContract.attentionMaskInputName = "attention_mask";
  projection.generationContract.positionIdsInputName = "position_ids";
  projection.generationContract.cachePositionInputName = "cache_position";
  projection.assembly.expectedOutputs = {{"state_out", "float32", {}}};
  const NativeRunnerPreparationContext context{
    "/provider/downstream", "boot-1", 1234, "/private/cache"};

  // The downstream graph consumes derived activations, not raw positions.
  projection.assembly.expectedInputs = {
    {"state_in", "float32", {}}, {"derived_position", "float32", {}}};
  NativeModelRunnerSpec spec;
  for (const auto* key : {"positionInputPolicy", "attentionMaskInputName",
                          "positionIdsInputName", "cachePositionInputName"})
    spec.metadata[key] = "stale";
  BOOST_CHECK_NO_THROW(bindNativeRunnerPreparationContext(spec, projection, context));
  for (const auto* key : {"positionInputPolicy", "attentionMaskInputName",
                          "positionIdsInputName", "cachePositionInputName"})
    BOOST_CHECK(spec.metadata.count(key) == 0);
  BOOST_CHECK_EQUAL(spec.metadata.at("kvTensorMap"), "state_in=state_out");

  // Each incomplete local interface must fail, including cache-position-only.
  for (const auto* name : {"attention_mask", "position_ids", "cache_position"}) {
    projection.assembly.expectedInputs.push_back({name, "int64", {}});
    BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, projection, context),
                      std::invalid_argument);
    projection.assembly.expectedInputs.pop_back();
  }
  projection.assembly.expectedInputs.push_back({"attention_mask", "int64", {}});
  projection.assembly.expectedInputs.push_back({"position_ids", "int64", {}});
  BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, projection, context),
                    std::invalid_argument);
  projection.assembly.expectedInputs.push_back({"cache_position", "int64", {}});
  BOOST_CHECK_NO_THROW(bindNativeRunnerPreparationContext(spec, projection, context));
  BOOST_CHECK_EQUAL(spec.metadata.at("cachePositionInputName"), "cache_position");

  // Absence on this role must not excuse malformed global policy names.
  projection.assembly.expectedInputs = {{"state_in", "float32", {}}};
  projection.generationContract.attentionMaskInputName.clear();
  BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, projection, context),
                    std::invalid_argument);
  projection.generationContract.attentionMaskInputName = "attention_mask";
  projection.generationContract.positionIdsInputName.clear();
  BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, projection, context),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeGenerationControlPreservesSealedRankForPartialAssignment)
{
  NativeExecutionPlan plan;
  plan.roles = {"stage-0", "stage-1"};
  plan.streamingOperationStride = 4;
  NativeDependencySpec feedback;
  feedback.producers = {"stage-1"};
  feedback.consumers = {"stage-0"};
  feedback.keyScope = "token-feedback";
  feedback.tensors = {"input_ids"};
  feedback.operationKind = "TOKEN_FEEDBACK";
  feedback.useNdnsfDataV1 = true;
  feedback.collectiveProducerRank = "1";
  feedback.collectiveOperationIndex = 3;
  feedback.collectiveTensorDigest = "sealed-token-digest";
  plan.dependencies = {feedback};
  NativeProviderAssignment partial;
  partial.providerByRole["stage-0"] = "/provider/0";

  // The V3 control-edge caller deliberately supplies no legacy local fallback.
  const auto consumer = roleSpecFor(plan, "stage-0", "request", partial, "", 1);
  BOOST_REQUIRE_EQUAL(consumer.inputs.size(), 1);
  const auto& input = consumer.inputs.front();
  BOOST_CHECK(input.producerProvider.empty());
  BOOST_CHECK_EQUAL(input.collectiveProducerRank, "1");
  BOOST_CHECK_EQUAL(input.collectiveOperationIndex, 7);
  BOOST_CHECK_EQUAL(input.producerRole, "stage-1");

  NativeProviderAssignment producerAssignment;
  producerAssignment.providerByRole["stage-1"] = "/provider/1";
  const auto producer = roleSpecFor(
    plan, "stage-1", "request", producerAssignment, "", 1);
  BOOST_REQUIRE_EQUAL(producer.outputs.size(), 1);
  const auto& output = producer.outputs.front();
  BOOST_CHECK_EQUAL(output.producerProvider, "/provider/1");
  BOOST_CHECK_EQUAL(input.collectiveProducerRank, output.collectiveProducerRank);
  BOOST_CHECK_EQUAL(input.collectiveOperationIndex, output.collectiveOperationIndex);
  BOOST_CHECK_EQUAL(input.collectiveTensorDigest, output.collectiveTensorDigest);

  partial.providerByRole["stage-1"] = "/provider/1";
  const auto complete = roleSpecFor(plan, "stage-0", "request", partial, "", 1);
  BOOST_REQUIRE_EQUAL(complete.inputs.size(), 1);
  BOOST_CHECK_EQUAL(complete.inputs.front().producerProvider, "/provider/1");
  BOOST_CHECK_EQUAL(complete.inputs.front().collectiveProducerRank, "1");
}

BOOST_AUTO_TEST_CASE(NativePreparationContextBindsActivationAroundPassthroughOutputs)
{
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/generation";
  projection.requestId = "request-1";
  projection.attempt = 1;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  projection.executionRole.roleId = "/LLM/Pipeline/Stage/0";
  projection.dataflow.role = projection.executionRole.roleId;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  for (const auto* tensor : {"hidden-layer-13-to-14"}) {
    NativeTensorEndpointV3 endpoint;
    endpoint.producerNamespace = "/provider/generation";
    endpoint.requester = "/requester";
    endpoint.requestId = projection.requestId;
    endpoint.attempt = projection.attempt;
    endpoint.planDigest = projection.planDigest;
    endpoint.securityProfile = "NDNSF_DATA_V1";
    endpoint.groupEpoch = "attempt-1";
    endpoint.producerRole = projection.dataflow.role;
    endpoint.consumerRole = "/LLM/Pipeline/Stage/1";
    endpoint.groupId = "pipeline-stage-0-to-1";
    endpoint.operation = "PIPELINE";
    endpoint.tensorId = tensor;
    endpoint.tensorDigest = "sha256:" + std::string(64, 'b');
    endpoint.segmentCount = 1;
    projection.dataflow.mayPublish.push_back(std::move(endpoint));
  }
  projection.assembly.expectedInputs = {
    {"input_ids", "int64", {}},
    {"attention_mask", "int64", {}},
    {"position_ids", "int64", {}},
    {"past_key.0", "float32", {}}};
  projection.assembly.expectedOutputs = {
    {"/Add_2_output_0", "float16", {}},
    {"/Add_30_output_0", "float16", {}},
    {"/self_attn/Unsqueeze_6_output_0", "float16", {}},
    {"/self_attn/Unsqueeze_7_output_0", "float16", {}},
    {"present_key.0", "float32", {}}};
  projection.generationContract.enabled = true;
  projection.generationContract.stateInputNames = {"past_key.0"};
  projection.generationContract.stateOutputNames = {"present_key.0"};

  NativeModelRunnerSpec spec;
  spec.metadata["outputNames"] =
    "/Add_2_output_0,/Add_30_output_0,/self_attn/Unsqueeze_6_output_0,"
    "/self_attn/Unsqueeze_7_output_0,present_key.0";
  bindNativeRunnerPreparationContext(spec, projection,
    {projection.provider, "boot-1", 1234, "/private/cache"});

  BOOST_CHECK(spec.metadata.find("outputAlias./Add_2_output_0") ==
              spec.metadata.end());
  BOOST_CHECK(spec.metadata.find("outputAlias./Add_30_output_0") ==
              spec.metadata.end());
  BOOST_CHECK(spec.metadata.find(
                "outputAlias./self_attn/Unsqueeze_6_output_0") ==
              spec.metadata.end());
  BOOST_CHECK(spec.metadata.find(
                "outputAlias./self_attn/Unsqueeze_7_output_0") ==
              spec.metadata.end());
  BOOST_CHECK(spec.metadata.find("outputAlias.present_key.0") ==
              spec.metadata.end());
}

} // namespace ndnsf::di::test
