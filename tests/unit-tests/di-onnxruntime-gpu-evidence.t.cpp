#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::tests {

namespace {

std::filesystem::path
findSpec175Fixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/two-role");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::exists(candidate / "role-0.onnx") &&
        std::filesystem::exists(candidate / "role-1.onnx")) {
      return candidate;
    }
  }
  return {};
}

TensorBundle
makeZeroState(const std::string& name)
{
  return makeEncodedTensorBundle(
    name,
    {NamedTensor{name, TensorElementType::Float32, {2, 8},
                 std::vector<std::uint8_t>(2 * 8 * sizeof(float), 0)}});
}

TensorBundle
makeInputIds(std::int64_t token)
{
  std::vector<std::uint8_t> bytes(sizeof(token));
  std::memcpy(bytes.data(), &token, sizeof(token));
  return makeEncodedTensorBundle(
    "input_ids",
    {NamedTensor{"input_ids", TensorElementType::Int64, {1, 1}, std::move(bytes)}});
}

TensorBundle
stateOutputAsInput(const std::vector<NamedTensor>& tensors,
                   const std::string& outputName)
{
  const auto& output = findTensor(tensors, outputName);
  const auto inputName = outputName.substr(0, outputName.size() - 4) + "_in";
  return makeEncodedTensorBundle(
    inputName,
    {NamedTensor{inputName, output.elementType, output.shape, output.payload}});
}

std::vector<NamedTensor>
decodeOutput(const std::map<std::string, TensorBundle>& outputs)
{
  const auto found = outputs.find("onnx-output-bundle");
  if (found == outputs.end()) {
    throw std::runtime_error("stateful ONNX runner did not return an output bundle");
  }
  return decodeTensorBundle(found->second.payload);
}

NativeModelRunnerSpec
makeStatefulSpec(const std::filesystem::path& path,
                 const std::string& role)
{
  NativeModelRunnerSpec spec;
  spec.role = role;
  spec.kind = "spec175-tiny-stateful-onnx";
  spec.backend = "onnxruntime";
  spec.path = path.string();
  spec.metadata["executionProvider"] = "cpu";
  spec.metadata["statefulModel"] = "true";
  spec.metadata["inputNames"] =
    "input_ids,attention_kv_in,recurrent_state_in,convolution_state_in";
  spec.metadata["outputNames"] =
    "hidden_out,attention_kv_out,recurrent_state_out,convolution_state_out";
  if (role == "/LLM/Pipeline/Stage/1") {
    spec.metadata["inputNames"] =
      "hidden_in,attention_kv_in,recurrent_state_in,convolution_state_in";
    spec.metadata["outputNames"] =
      "logits,attention_kv_out,recurrent_state_out,convolution_state_out";
  }
  spec.metadata["stateInputNames"] =
    "attention_kv_in,recurrent_state_in,convolution_state_in";
  spec.metadata["stateOutputNames"] =
    "attention_kv_out,recurrent_state_out,convolution_state_out";
  return spec;
}

NativeModelRunnerSpec
makeCoordinatorMetadataSpec(const std::filesystem::path& path,
                            const std::string& role)
{
  auto spec = makeStatefulSpec(path, role);
  spec.metadata["inputScope.input_ids"] = "request-input";
  spec.metadata["inputScope.hidden_in"] = "activation";
  spec.metadata["outputAlias.hidden_out"] = "hidden_in";
  spec.metadata["evidence.providerName"] = "/test/provider/spec175";
  spec.metadata["evidence.providerBootId"] = "spec175-boot";
  spec.metadata["evidence.modelDigest"] = "sha256:" + std::string(64, '1');
  spec.metadata["evidence.planDigest"] = "sha256:" + std::string(64, '1');
  spec.metadata["evidence.artifactDigest"] = "sha256:" + std::string(64, '2');
  spec.metadata["evidence.createdAtMs"] = "1";
  return spec;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DiOnnxRuntimeGpuEvidence)

BOOST_AUTO_TEST_CASE(CudaSelectionRejectsFallbackAndCpuOnlyAllocation)
{
  NativeModelRunnerSpec spec;
  spec.metadata["executionProvider"] = "cuda";
  spec.metadata["deviceId"] = "1";
  spec.metadata["allowCpuFallback"] = "false";
  BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, {"CPUExecutionProvider"}),
                    std::runtime_error);
  const auto selected = resolveOnnxRuntimeProviderSelection(
    spec, {"CUDAExecutionProvider", "CPUExecutionProvider"});
  BOOST_CHECK_EQUAL(selected.selectedProvider, "cuda");
  BOOST_CHECK_EQUAL(selected.deviceId, "1");
  BOOST_CHECK(!selected.usedCpuFallback);
}

BOOST_AUTO_TEST_CASE(StatefulDeviceOutputMapsToSuccessorInput)
{
  StatefulOnnxIoContractV1 contract;
  contract.inputNames = {
    "input_ids", "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  contract.outputNames = {
    "logits", "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
  contract.stateInputNames = {
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  contract.stateOutputNames = {
    "attention_kv_out", "recurrent_state_out", "convolution_state_out"};

  BOOST_REQUIRE_NO_THROW(contract.validate());
  BOOST_CHECK_EQUAL(contract.stateInputForOutput("attention_kv_out"),
                    "attention_kv_in");
  BOOST_CHECK_EQUAL(contract.stateInputForOutput("recurrent_state_out"),
                    "recurrent_state_in");
  BOOST_CHECK_THROW(contract.stateInputForOutput("logits"),
                    std::invalid_argument);

  contract.stateOutputNames.back() = "wrong_state_out";
  BOOST_CHECK_THROW(contract.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CausalPositionInputsComeOnlyFromAuthenticatedLineage)
{
  StatefulOnnxIoContractV1 io;
  io.inputNames = {
    "input_ids", "attention_mask", "position_ids", "cache_position",
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  io.outputNames = {
    "logits", "attention_kv_out", "recurrent_state_out",
    "convolution_state_out"};
  io.stateInputNames = {
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  io.stateOutputNames = {
    "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
  CausalPositionInputContractV1 contract{
    "qwen-causal-position-v1", "attention_mask", "position_ids",
    "cache_position"};
  GenerationEpochLineageV1 lineage;
  lineage.requestId = "/request/1";
  lineage.attemptEpoch = 1;
  lineage.planDigest = "sha256:" + std::string(64, '1');
  lineage.generationId = "0123456789abcdef0123456789abcdef";
  lineage.streamEpoch = 1;
  lineage.inferenceEpoch = 3;
  lineage.logicalPrefixDigest = "sha256:" + std::string(64, '2');
  lineage.logicalPrefixTokenCount = 5;
  lineage.positionDigest = "sha256:" + std::string(64, '3');
  lineage.producerRole = "/LLM/Pipeline/Stage/0";
  lineage.consumerRole = "/LLM/Pipeline/Stage/0";

  const auto values = materializeCausalPositionInputsV1(
    contract, io, lineage, 2);
  const auto decodedMask =
    decodeTensorBundle(values.at("attention_mask").payload);
  const auto decodedPositions =
    decodeTensorBundle(values.at("position_ids").payload);
  const auto decodedCache =
    decodeTensorBundle(values.at("cache_position").payload);
  const auto& mask = findTensor(
    decodedMask, "attention_mask");
  const auto& positions = findTensor(
    decodedPositions, "position_ids");
  const auto& cache = findTensor(
    decodedCache, "cache_position");
  BOOST_TEST(mask.shape == std::vector<std::int64_t>({1, 5}),
             boost::test_tools::per_element());
  BOOST_TEST(positions.shape == std::vector<std::int64_t>({1, 2}),
             boost::test_tools::per_element());
  BOOST_TEST(cache.shape == std::vector<std::int64_t>({2}),
             boost::test_tools::per_element());
  std::vector<std::int64_t> positionValues(2);
  std::memcpy(positionValues.data(), positions.payload.data(),
              positions.payload.size());
  BOOST_TEST(positionValues == std::vector<std::int64_t>({3, 4}),
             boost::test_tools::per_element());

  BOOST_CHECK_THROW(
    materializeCausalPositionInputsV1(contract, io, lineage, 0),
    std::invalid_argument);
  auto missingPolicy = contract;
  missingPolicy.policy.clear();
  BOOST_CHECK_THROW(
    materializeCausalPositionInputsV1(missingPolicy, io, lineage, 2),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OrtProfileRecordsEveryModelNodeProviderAndGpuUuid)
{
  const auto path = std::filesystem::temp_directory_path() /
                    "ndnsf-spec109-ort-profile.json";
  std::ofstream(path) << R"json([
    {"cat":"Session","name":"model_loading_uri"},
    {"cat":"Node","name":"MatMul_0_kernel_time","args":{"provider":"CUDAExecutionProvider"}},
    {"cat":"Node","name":"Add_1_kernel_time","args":{"provider":"CUDAExecutionProvider"}}
  ])json";
  ExecutionEvidence evidence;
  applyOnnxRuntimeProviderProfile(evidence, path.string(), "/LLM/Stage/0", false,
                                  "GPU-test-uuid");
  BOOST_REQUIRE_EQUAL(evidence.nodeProviderAssignments.size(), 2);
  BOOST_CHECK_EQUAL(evidence.nodeProviderAssignments.front().provider,
                    "CUDAExecutionProvider");
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-test-uuid");
  BOOST_CHECK(!evidence.cpuFallbackUsed);
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(StatefulTinyOnnxRunsTwoRolesWithPersistentState)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 tiny two-role ONNX fixture is unavailable");

  OnnxRuntimeModelRunner stage0(makeStatefulSpec(
    fixture / "role-0.onnx", "/LLM/Pipeline/Stage/0"));
  OnnxRuntimeModelRunner stage1(makeStatefulSpec(
    fixture / "role-1.onnx", "/LLM/Pipeline/Stage/1"));

  std::map<std::string, TensorBundle> stage0State{
    {"attention_kv_in", makeZeroState("attention_kv_in")},
    {"recurrent_state_in", makeZeroState("recurrent_state_in")},
    {"convolution_state_in", makeZeroState("convolution_state_in")},
  };
  std::map<std::string, TensorBundle> stage1State = stage0State;
  std::int64_t token = 3;
  const std::vector<std::int64_t> expected{4, 5, 6, 7, 8, 9, 10, 2};

  for (const auto expectedToken : expected) {
    RoleExecutionContext firstContext;
    firstContext.sessionId = "spec175-native-stateful";
    firstContext.role = "/LLM/Pipeline/Stage/0";
    firstContext.inputsByScope = stage0State;
    firstContext.inputsByScope.emplace("input_ids", makeInputIds(token));
    const auto firstOutputs = decodeOutput(stage0.run(firstContext));

    RoleExecutionContext secondContext;
    secondContext.sessionId = firstContext.sessionId;
    secondContext.role = "/LLM/Pipeline/Stage/1";
    secondContext.inputsByScope = stage1State;
    const auto& hidden = findTensor(firstOutputs, "hidden_out");
    secondContext.inputsByScope.emplace(
      "hidden_in", makeEncodedTensorBundle(
        "hidden_in", {NamedTensor{"hidden_in", hidden.elementType,
                                   hidden.shape, hidden.payload}}));
    const auto secondOutputs = decodeOutput(stage1.run(secondContext));
    const auto& logits = findTensor(secondOutputs, "logits");
    BOOST_REQUIRE_EQUAL(static_cast<int>(logits.elementType),
                        static_cast<int>(TensorElementType::Float32));
    BOOST_REQUIRE_EQUAL(logits.payload.size(), 32U * sizeof(float));
    const auto* values = reinterpret_cast<const float*>(logits.payload.data());
    const auto* best = std::max_element(values, values + 32);
    BOOST_REQUIRE(best != values + 32);
    BOOST_CHECK_EQUAL(std::distance(values, best), expectedToken);

    stage0State["attention_kv_in"] = stateOutputAsInput(
      firstOutputs, "attention_kv_out");
    stage0State["recurrent_state_in"] = stateOutputAsInput(
      firstOutputs, "recurrent_state_out");
    stage0State["convolution_state_in"] = stateOutputAsInput(
      firstOutputs, "convolution_state_out");
    stage1State["attention_kv_in"] = stateOutputAsInput(
      secondOutputs, "attention_kv_out");
    stage1State["recurrent_state_in"] = stateOutputAsInput(
      secondOutputs, "recurrent_state_out");
    stage1State["convolution_state_in"] = stateOutputAsInput(
      secondOutputs, "convolution_state_out");
    token = expectedToken;
  }

  const auto stage0Metrics = stage0.runtimeMetricsSnapshot();
  const auto stage1Metrics = stage1.runtimeMetricsSnapshot();
  BOOST_REQUIRE(stage0Metrics);
  BOOST_REQUIRE(stage1Metrics);
  BOOST_CHECK_GT(stage0Metrics->activationInputBytes, 0U);
  BOOST_CHECK_GT(stage0Metrics->activationOutputBytes, 0U);
  BOOST_CHECK_GT(stage1Metrics->activationInputBytes, 0U);
  BOOST_CHECK_GT(stage1Metrics->activationOutputBytes, 0U);
  BOOST_CHECK_EQUAL(stage0Metrics->stateRecomputes, expected.size());
  BOOST_CHECK_EQUAL(stage1Metrics->stateRecomputes, expected.size());
  BOOST_CHECK_EQUAL(stage0Metrics->stateInputHits, 0U);
  BOOST_CHECK_EQUAL(stage1Metrics->stateInputHits, 0U);
  BOOST_CHECK_EQUAL(stage0Metrics->stateDeviceToHostBytes, 0U);
  BOOST_CHECK_EQUAL(stage0Metrics->stateHostToDeviceBytes, 0U);
  BOOST_CHECK_EQUAL(stage1Metrics->stateDeviceToHostBytes, 0U);
  BOOST_CHECK_EQUAL(stage1Metrics->stateHostToDeviceBytes, 0U);
}

BOOST_AUTO_TEST_CASE(StatefulTinyOnnxRunsWithCoordinatorMetadataAndMissingState)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 tiny two-role ONNX fixture is unavailable");

  OnnxRuntimeModelRunner stage0(makeCoordinatorMetadataSpec(
    fixture / "role-0.onnx", "/LLM/Pipeline/Stage/0"));
  RoleExecutionContext context;
  context.sessionId = "spec175-coordinator-metadata";
  context.role = "/LLM/Pipeline/Stage/0";
  context.inputsByScope.emplace("request-input", makeInputIds(3));
  const auto outputs = decodeOutput(stage0.run(context));
  BOOST_REQUIRE(!outputs.empty());
  BOOST_REQUIRE_NO_THROW(findTensor(outputs, "hidden_in"));
}

BOOST_AUTO_TEST_CASE(AuthenticatedLineageCannotEnterStandaloneStreamedLoop)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 tiny two-role ONNX fixture is unavailable");

  auto spec = makeStatefulSpec(
    fixture / "role-0.onnx", "/LLM/Pipeline/Stage/0");
  spec.metadata["streamingGeneration"] = "true";
  spec.metadata["samplingDigest"] = "sha256:" + std::string(64, '1');
  OnnxRuntimeModelRunner runner(std::move(spec));

  RoleExecutionContext context;
  context.sessionId = "spec175-coordinator-owned-loop";
  context.role = "/LLM/Pipeline/Stage/0";
  context.generationLineage = GenerationEpochLineageV1{};
  context.streamEventSink = [] (const std::vector<std::uint8_t>&) {
    return true;
  };
  BOOST_CHECK_EXCEPTION(
    runner.runStreamed(context),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find(
        "must be driven by NativeEpochCoordinator") != std::string::npos;
    });
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
