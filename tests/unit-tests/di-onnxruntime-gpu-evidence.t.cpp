#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKv.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <boost/test/unit_test.hpp>
#include <ndn-cxx/util/logging.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ndnsf::di::tests {

namespace {

struct ScopedRunnerTimingCapture
{
  std::ostringstream output;
  std::optional<std::string> previous;

  ScopedRunnerTimingCapture()
  {
    if (const auto value = std::getenv("NDNSF_PHASE_TIMING")) previous = value;
    if (::setenv("NDNSF_PHASE_TIMING", "1", 1) != 0)
      throw std::runtime_error("cannot enable runner timing");
    ndn::util::Logging::setLevel("ndnsf.di.RuntimeEvidence=WARN");
    ndn::util::Logging::setDestination(output, true);
  }

  ~ScopedRunnerTimingCapture()
  {
    ndn::util::Logging::setDestination(std::clog, true);
    if (previous) ::setenv("NDNSF_PHASE_TIMING", previous->c_str(), 1);
    else ::unsetenv("NDNSF_PHASE_TIMING");
  }
};

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

std::filesystem::path
findSpec204DynamicSuccessorFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec204/dynamic-successor/role.onnx");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::exists(candidate)) {
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

  auto llamaPolicy = contract;
  llamaPolicy.policy = "llama-causal-position-v1";
  const auto llamaValues = materializeCausalPositionInputsV1(
    llamaPolicy, io, lineage, 2);
  BOOST_CHECK(llamaValues.count("attention_mask") == 1);
  BOOST_CHECK(llamaValues.count("position_ids") == 1);
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

BOOST_AUTO_TEST_CASE(StatefulTinyOnnxCpuStreamingRetainsResidentState)
{
  ScopedRunnerTimingCapture timing;
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 tiny two-role ONNX fixture is unavailable");

  auto spec = makeStatefulSpec(
    fixture / "role-0.onnx", "/LLM/Pipeline/Stage/0");
  spec.metadata["providerIdentity"] = "/test/provider/spec190-cpu-resident";
  spec.metadata["providerBootId"] = "spec190-cpu-resident-boot";
  OnnxRuntimeModelRunner runner(std::move(spec));

  RoleExecutionContext context;
  context.requestId = "/Spec190/Request/cpu-resident-state";
  context.attemptEpoch = 1;
  context.providerBootId = "spec190-cpu-resident-boot";
  context.sessionId = "spec190-cpu-resident-state";
  context.role = "/LLM/Pipeline/Stage/0";
  context.streamingStateExecution = true;
  for (std::uint64_t epoch = 0; epoch < 3; ++epoch) {
    context.inferenceEpoch = epoch;
    context.inputsByScope.clear();
    context.inputsByScope.emplace(
      "input_ids", makeInputIds(static_cast<std::int64_t>(3 + epoch)));
    const auto outputs = runner.run(context);
    const auto tensors = decodeOutput(outputs);
    BOOST_REQUIRE_NO_THROW(findTensor(tensors, "hidden_out"));
    BOOST_CHECK_THROW(findTensor(tensors, "attention_kv_out"), std::out_of_range);
    // The opaque handle is only a Provider-local epoch reference.  The
    // conversation promotion path below moves the typed host Ort::Value state
    // into the adapter-owned conversation cache instead of exporting this
    // UInt8 token as a model input.
    BOOST_CHECK_EQUAL(outputs.count("__ndnsf_provider_decode_state"), 1U);
    const auto handle = runner.stateHandleSnapshot(context.sessionId);
    BOOST_REQUIRE(handle);
    BOOST_CHECK(handle->token.rfind("ndnsf-resident-state-v1:", 0) == 0);
  }

  // Parse actual runner output, not synthetic timing input. Check each epoch
  // independently before the continuation creates another epoch-zero pair.
  ndn::util::Logging::flush();
  std::vector<RuntimePhaseObservation> engineEvents;
  std::istringstream records(timing.output.str());
  std::string line;
  while (std::getline(records, line)) {
    const auto event = parseRuntimePhaseObservation(line);
    if (event && event->requestId == context.requestId &&
        (event->phase == "ortRunBegin" || event->phase == "ortRunEnd"))
      engineEvents.push_back(*event);
  }
  BOOST_REQUIRE_EQUAL(engineEvents.size(), 6U);
  for (std::size_t epoch = 0; epoch < 3; ++epoch) {
    const auto& begin = engineEvents[epoch * 2];
    const auto& end = engineEvents[epoch * 2 + 1];
    BOOST_CHECK_EQUAL(begin.phase, "ortRunBegin");
    BOOST_CHECK_EQUAL(end.phase, "ortRunEnd");
    for (const auto* event : {&begin, &end}) {
      BOOST_CHECK_EQUAL(event->role, "di-provider");
      BOOST_CHECK_EQUAL(event->executionRole, context.role);
      BOOST_CHECK_EQUAL(event->sessionId, context.sessionId);
      BOOST_CHECK_EQUAL(event->providerBootId, context.providerBootId);
      BOOST_CHECK_EQUAL(event->attempt, "1");
      BOOST_CHECK_EQUAL(event->inferenceEpoch, std::to_string(epoch));
    }
    BOOST_CHECK_GE(end.steadyUs, begin.steadyUs);
  }

  const auto metrics = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(metrics);
  BOOST_CHECK_EQUAL(metrics->stateInputHits, 6U);
  BOOST_CHECK_EQUAL(metrics->stateInputMisses, 0U);
  BOOST_CHECK_EQUAL(metrics->stateRecomputes, 1U);
  BOOST_CHECK_EQUAL(metrics->stateDeviceToHostBytes, 0U);
  BOOST_CHECK_EQUAL(metrics->stateHostToDeviceBytes, 0U);
  BOOST_CHECK_GT(metrics->activationOutputBytes, 0U);

  const auto conversation = runner.promoteSessionStateToConversation(
    context.sessionId, "spec190-cpu-conversation");
  BOOST_REQUIRE(conversation);
  BOOST_CHECK_GT(conversation->logicalBytes, 0U);
  BOOST_CHECK(runner.restoreConversationState(
    *conversation, "spec190-cpu-conversation-next"));

  RoleExecutionContext continuation = context;
  continuation.sessionId = "spec190-cpu-conversation-next";
  continuation.inferenceEpoch = 0;
  continuation.inputsByScope.clear();
  continuation.inputsByScope.emplace("input_ids", makeInputIds(9));
  BOOST_REQUIRE_NO_THROW(runner.run(continuation));

  runner.releaseSessionState(context.sessionId);
  runner.releaseSessionState(continuation.sessionId);
  BOOST_CHECK(runner.releaseConversationState(*conversation));
  const auto released = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(released);
  BOOST_CHECK_EQUAL(released->stateReleases, 2U);
  BOOST_CHECK(!runner.stateHandleSnapshot(context.sessionId));
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

BOOST_AUTO_TEST_CASE(EncodedInputSelectionPreservesCallerAndRejectsInvalidPayload)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 tiny two-role ONNX fixture is unavailable");
  OnnxRuntimeModelRunner stage0(makeCoordinatorMetadataSpec(
    fixture / "role-0.onnx", "/LLM/Pipeline/Stage/0"));
  RoleExecutionContext context;
  context.sessionId = "spec199-owned-input";
  context.role = "/LLM/Pipeline/Stage/0";
  context.inputsByScope.emplace("request-input", makeInputIds(3));
  const auto expected = encodeTensorBundle(decodeOutput(stage0.run(context)));
  auto tensors = decodeTensorBundle(context.inputsByScope.at("request-input").payload);
  tensors.insert(tensors.begin(), NamedTensor{
    "unused", TensorElementType::Float32, {1}, std::vector<std::uint8_t>(4, 0)});
  context.inputsByScope.at("request-input") =
    makeEncodedTensorBundle("request-input", tensors);
  const auto original = context.inputsByScope.at("request-input").payload;
  const auto actual = encodeTensorBundle(decodeOutput(stage0.run(context)));
  BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(),
                                expected.begin(), expected.end());
  const auto& retained = context.inputsByScope.at("request-input").payload;
  BOOST_CHECK_EQUAL_COLLECTIONS(retained.begin(), retained.end(),
                                original.begin(), original.end());

  context.inputsByScope.at("request-input").payload.push_back(0);
  BOOST_CHECK_THROW(stage0.run(context), std::invalid_argument);
  tensors.back().name = "missing_input_ids";
  context.inputsByScope.at("request-input") =
    makeEncodedTensorBundle("request-input", tensors);
  BOOST_CHECK_THROW(stage0.run(context), std::out_of_range);
  tensors.back() = NamedTensor{
    "input_ids", TensorElementType::Float32, {1}, std::vector<std::uint8_t>(4, 0)};
  context.inputsByScope.at("request-input") =
    makeEncodedTensorBundle("request-input", tensors);
  BOOST_CHECK_THROW(stage0.run(context), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204PortableCpuStateSurvivesSourceDestructionAndContinuesSuffix)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(), "actual ONNX fixture required");
  const std::string role = "/LLM/Pipeline/Stage/0";
  const auto digest = [](char c) { return "sha256:" + std::string(64, c); };
  const auto prefixDigest = [&] (const std::vector<std::int64_t>& tokenIds) {
    return nativeGenerationStatePrefixDigest(tokenIds, 1, digest('5'));
  };
  BOOST_CHECK_EQUAL(prefixDigest({3, 4}),
                    "sha256:cdd20d7bb5fceeb5490bd01126d5d722d8e199e2b59dba1afd82749019412432");
  BOOST_CHECK_NE(prefixDigest({3, 4}), prefixDigest({3, 5}));
  const auto prefixThrough = [] (std::int64_t token) {
    std::vector<std::int64_t> tokens;
    for (std::int64_t current = 3; current <= token; ++current)
      tokens.push_back(current);
    return tokens;
  };
  auto makeSpec = [&](const std::string& provider, const std::string& boot) {
    auto spec = makeStatefulSpec(fixture / "role-0.onnx", role);
    spec.metadata["providerIdentity"] = provider;
    spec.metadata["providerBootId"] = boot;
    spec.metadata["workTokenInputName"] = "input_ids";
    spec.metadata["workTokenAxis"] = "1";
    return spec;
  };
  auto identity = [&](const std::string& provider, const std::string& boot,
                      const std::string& request) {
    DecodeStateIdentityV1 id;
    id.modelDigest = digest('1'); id.graphSemanticDigest = digest('2');
    id.artifactDigest = digest('3'); id.adapterDigest = digest('4');
    id.tokenizerDigest = digest('5'); id.runnerDigest = digest('6');
    id.roleName = role; id.roleSplitDigest = digest('7'); id.layerEnd = 4;
    const auto tokens = prefixThrough(5);
    id.prefixDigest = prefixDigest(tokens); id.prefixTokenCount = tokens.size();
    id.positionDigest = digest('9'); id.precision = "fp32";
    id.layoutDigest = digest('a'); id.stateSchemaDigest = digest('b');
    id.stateComponentDigests = {digest('c'), digest('d'), digest('e')};
    id.runtimeAbiDigest = digest('e'); id.securityDomainDigest = digest('f');
    id.providerIdentity = provider; id.providerBootId = boot; id.cacheEpoch = 7;
    id.requestId = request; id.attemptEpoch = 1; id.generationId = request + "-generation";
    id.validate();
    return id;
  };
  auto execute = [&](OnnxRuntimeModelRunner& runner, const std::string& session,
                     const std::string& request, std::uint64_t epoch, std::int64_t token,
                     const std::string& transition,
                     const std::vector<std::int64_t>& logicalPrefixTokens) {
    if (logicalPrefixTokens.empty() || logicalPrefixTokens.back() != token ||
        logicalPrefixTokens.size() != epoch + 1)
      throw std::logic_error("portable ONNX fixture prefix does not match its token sequence");
    RoleExecutionContext ctx;
    ctx.sessionId = session; ctx.requestId = request; ctx.role = role;
    ctx.attemptEpoch = 1; ctx.inferenceEpoch = epoch; ctx.streamingStateExecution = true;
    ctx.inputsByScope.emplace("input_ids", makeInputIds(token));
    GenerationEpochLineageV1 lineage;
    lineage.requestId = request; lineage.attemptEpoch = 1; lineage.planDigest = digest('1');
    lineage.generationId = request + "-generation"; lineage.inferenceEpoch = epoch;
    lineage.logicalPrefixDigest = prefixDigest(logicalPrefixTokens);
    lineage.logicalPrefixTokenCount = logicalPrefixTokens.size();
    lineage.positionDigest = digest('9');
    lineage.transitionKind = transition;
    ctx.generationLineage = lineage;
    return runner.run(ctx);
  };
  NativePortableKvLimitsV1 limits{1024, 2048, {}};
  auto source = std::make_unique<OnnxRuntimeModelRunner>(makeSpec("/provider/A", "boot-a"));
  BOOST_REQUIRE(source->supportsPortableConversationState());
  for (std::uint64_t epoch = 0; epoch < 3; ++epoch)
    execute(*source, "internal-session-a", "request-a", epoch, 3 + epoch,
            epoch == 0 ? GenerationEpochLineageV1::PREFILL : GenerationEpochLineageV1::DECODE,
            prefixThrough(3 + epoch));
  const auto sourceHandle = source->promoteSessionStateToConversation("internal-session-a", "conversation-a");
  BOOST_REQUIRE(sourceHandle);
  auto origin = identity("/provider/A", "boot-a", "request-a");
  origin.stateInferenceEpoch = sourceHandle->opaque.stateInferenceEpoch;
  if (origin.stateInferenceEpoch) origin.predecessorInferenceEpoch = origin.stateInferenceEpoch - 1;
  auto snapshot = source->exportConversationState(*sourceHandle, origin, limits);
  BOOST_REQUIRE_EQUAL(snapshot.components.size(), 3U);
  BOOST_CHECK_EQUAL(snapshot.logicalBytes, 192U);
  const auto checkValues = [](const NativePortableKvSnapshotV1& state, float expected) {
    for (const auto& component : state.components) {
      const auto tensors = decodeTensorBundle(component.tensorWire);
      BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
      BOOST_REQUIRE_EQUAL(tensors[0].payload.size(), 64U);
      for (std::size_t offset = 0; offset < 64; offset += sizeof(float)) {
        float value;
        std::memcpy(&value, tensors[0].payload.data() + offset, sizeof(value));
        BOOST_CHECK_EQUAL(value, expected);
      }
    }
  };
  checkValues(snapshot, 3.0f);
  BOOST_CHECK_EQUAL(source->runtimeMetricsSnapshot()->executedPrefillTokens, 1U);
  BOOST_CHECK_EQUAL(source->runtimeMetricsSnapshot()->executedDecodeTokens, 2U);
  source.reset();

  auto destinationOwner = std::make_shared<OnnxRuntimeModelRunner>(makeSpec("/provider/B", "boot-b"));
  auto& destination = *destinationOwner;
  auto target = origin;
  target.providerIdentity = "/provider/B"; target.providerBootId = "boot-b";
  target.requestId = "request-b"; target.generationId = "request-b-generation"; target.cacheEpoch = 8;
  auto corrupt = snapshot;
  corrupt.components[0].tensorWire.back() ^= 1;
  BOOST_CHECK_THROW(destination.importConversationState(corrupt, target, "conversation-b", limits), std::invalid_argument);
  auto tooSmall = limits; tooSmall.maxPayloadBytes = 191;
  BOOST_CHECK_THROW(destination.importConversationState(snapshot, target, "conversation-b", tooSmall), std::invalid_argument);
  auto cancelled = limits; cancelled.cancelled = [] { return true; };
  BOOST_CHECK_THROW(destination.importConversationState(snapshot, target, "conversation-b", cancelled), std::runtime_error);
  const auto imported = destination.importConversationState(snapshot, target, "conversation-b", limits);
  BOOST_CHECK(imported.opaque.token != sourceHandle->opaque.token);
  BOOST_CHECK_THROW(destination.importConversationState(snapshot, target, "conversation-b", limits), std::runtime_error);
  const std::vector<std::pair<std::string, std::function<void(DecodeStateIdentityV1&)>>>
    incompatibleIdentities{
      {"model", [&](auto& value) { value.modelDigest = digest('f'); }},
      {"tokenizer", [&](auto& value) { value.tokenizerDigest = digest('f'); }},
      {"precision", [](auto& value) { value.precision = "fp16"; }},
      {"role partition", [&](auto& value) { value.roleSplitDigest = digest('f'); }},
      {"layer range", [](auto& value) { ++value.layerEnd; }},
      {"prefix", [](auto& value) { ++value.prefixTokenCount; }},
      {"saved epoch", [](auto& value) {
        ++value.stateInferenceEpoch;
        value.predecessorInferenceEpoch = value.stateInferenceEpoch - 1;
      }},
      {"security domain", [&](auto& value) { value.securityDomainDigest = digest('0'); }}
    };
  const auto beforeInvalidImports = destination.runtimeMetricsSnapshot();
  BOOST_REQUIRE(beforeInvalidImports);
  for (const auto& [field, mutate] : incompatibleIdentities) {
    BOOST_TEST_CONTEXT(field) {
      auto incompatible = target;
      mutate(incompatible);
      BOOST_REQUIRE(!nativePortableKvIdentityCompatible(snapshot.originIdentity, incompatible));
      std::optional<NativeConversationStateHandleV1> rejected;
      BOOST_CHECK_THROW(rejected = destination.importConversationState(
        snapshot, incompatible, "rejected-conversation", limits), std::invalid_argument);
      BOOST_CHECK(!rejected); // No handle can be activated by the caller.
      if (rejected) destination.releaseConversationState(*rejected);
      const auto retained = destination.exportConversationState(imported, target, limits);
      BOOST_REQUIRE_EQUAL(retained.components.size(), snapshot.components.size());
      for (std::size_t i = 0; i < snapshot.components.size(); ++i)
        BOOST_CHECK(retained.components[i].tensorWire == snapshot.components[i].tensorWire);
      const auto metrics = destination.runtimeMetricsSnapshot();
      BOOST_REQUIRE(metrics);
      BOOST_CHECK_EQUAL(metrics->executedPrefillTokens, beforeInvalidImports->executedPrefillTokens);
      BOOST_CHECK_EQUAL(metrics->executedDecodeTokens, beforeInvalidImports->executedDecodeTokens);
    }
  }
  snapshot = {};
  std::vector<std::vector<std::uint8_t>> churn(200, std::vector<std::uint8_t>(64, 0xff));
  BOOST_REQUIRE(destination.restoreConversationState(imported, "internal-session-b"));
  const auto suffixOutput = decodeOutput(execute(destination, "internal-session-b", "request-b", 3, 6,
                                                   GenerationEpochLineageV1::DECODE,
                                                   prefixThrough(6)));
  const auto nextHandle = destination.promoteSessionStateToConversation("internal-session-b", "conversation-next");
  BOOST_REQUIRE(nextHandle);
  auto nextIdentity = target;
  nextIdentity.stateInferenceEpoch = nextHandle->opaque.stateInferenceEpoch;
  if (nextIdentity.stateInferenceEpoch)
    nextIdentity.predecessorInferenceEpoch = nextIdentity.stateInferenceEpoch - 1;
  const auto fourTokenPrefix = prefixThrough(6);
  nextIdentity.prefixDigest = prefixDigest(fourTokenPrefix);
  nextIdentity.prefixTokenCount = fourTokenPrefix.size();
  checkValues(destination.exportConversationState(*nextHandle, nextIdentity, limits), 4.0f);
  const auto metrics = destination.runtimeMetricsSnapshot();
  BOOST_REQUIRE(metrics);
  BOOST_CHECK(metrics->workCounterCertified);
  BOOST_CHECK_EQUAL(metrics->executedPrefillTokens, 0U);
  BOOST_CHECK_EQUAL(metrics->executedDecodeTokens, 1U);

  OnnxRuntimeModelRunner baseline(makeSpec("/provider/C", "boot-c"));
  std::vector<NamedTensor> baselineOutput;
  for (std::uint64_t epoch = 0; epoch < 4; ++epoch)
    baselineOutput = decodeOutput(execute(baseline, "baseline-session", "baseline-request", epoch, 3 + epoch,
      epoch == 0 ? GenerationEpochLineageV1::PREFILL : GenerationEpochLineageV1::DECODE,
      prefixThrough(3 + epoch)));
  BOOST_CHECK(findTensor(suffixOutput, "hidden_out").payload == findTensor(baselineOutput, "hidden_out").payload);
  BOOST_CHECK_EQUAL(baseline.runtimeMetricsSnapshot()->executedPrefillTokens, 1U);
  BOOST_CHECK_EQUAL(baseline.runtimeMetricsSnapshot()->executedDecodeTokens, 3U);
  BOOST_REQUIRE(destination.releaseConversationState(imported));
  auto nextSnapshot = destination.exportConversationState(*nextHandle, nextIdentity, limits);
  const auto replacement = destination.importConversationState(nextSnapshot, nextIdentity, "conversation-b", limits);
  BOOST_CHECK(!destination.releaseConversationState(imported));
  BOOST_CHECK(!destination.restoreConversationState(imported, "stale-session"));
  BOOST_CHECK(destination.releaseConversationState(replacement));

  // Exercise trusted Runtime admission/export with the actual imported ORT
  // tensors. This does not fabricate an authenticated network checkpoint.
  NativeProviderRuntime runtime(1, 8, 1024, 8, 256, 1, 256, 1);
  runtime.bindConversationStateProvider("/provider/B", "boot-b", 8);
  BOOST_CHECK(!runtime.hasRunner(role));
  auto runtimeIdentity = nextIdentity;
  runtimeIdentity.requestId = "runtime-import-request";
  runtimeIdentity.generationId = "runtime-import-generation";
  ConversationStateBinding binding;
  binding.conversationId = "runtime-import-conversation"; binding.contextEpoch = 1;
  binding.serviceName = "/LLM/Pipeline/Generate";
  binding.planRoleMapDigest = digest('0'); binding.receiptDigest = digest('9');
  binding.expiresAtMs = 10'000; binding.identity = runtimeIdentity;
  binding.validate();
  const auto runtimeHandle = destination.importConversationState(nextSnapshot, runtimeIdentity, "runtime-conversation", limits);
  BOOST_REQUIRE(runtime.stageImportedConversationState(binding, runtimeHandle, destinationOwner, 100));
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().gpuBytes, 192U);
  BOOST_CHECK(!runtime.stageImportedConversationState(binding, runtimeHandle, destinationOwner, 100));
  BOOST_CHECK_THROW(runtime.exportConversationState(binding, limits, 101), std::runtime_error);
  BOOST_CHECK(!runtime.commitStagedDecodeStatePromotion(binding));
  const auto checkpoint = digest('a');
  BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(binding, checkpoint));
  auto committed = binding; committed.checkpointDigest = checkpoint;
  checkValues(runtime.exportConversationState(committed, limits, 102), 4.0f);
  BOOST_CHECK_THROW(runtime.exportConversationState(committed, cancelled, 103), std::runtime_error);
  // Export exceptions must release the exact owned snapshot pin.
  BOOST_CHECK(runtime.pinConversationState(committed, 104));
  BOOST_CHECK(runtime.unpinConversationState(committed));
  auto rejected = binding; rejected.contextEpoch = 2;
  rejected.identity.requestId = "runtime-rejected-request";
  rejected.identity.generationId = "runtime-rejected-generation";
  const auto rejectedHandle = destination.importConversationState(nextSnapshot, rejected.identity, "runtime-rejected", limits);
  BOOST_CHECK(!runtime.stageImportedConversationState(rejected, rejectedHandle, destinationOwner, 105));
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().gpuBytes, 192U);
  // Capacity failure preserves the committed winner and leaves this handle
  // caller-owned, so explicit caller release remains valid.
  BOOST_CHECK(destination.releaseConversationState(rejectedHandle));
  checkValues(runtime.exportConversationState(committed, limits, 106), 4.0f);
  BOOST_REQUIRE(runtime.releaseConversationState(committed));
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
  BOOST_CHECK(!destination.restoreConversationState(runtimeHandle, "released-runtime-state"));

  auto rolledBack = binding; rolledBack.contextEpoch = 3;
  rolledBack.identity.requestId = "runtime-rollback-request";
  rolledBack.identity.generationId = "runtime-rollback-generation";
  const auto rollbackHandle = destination.importConversationState(nextSnapshot, rolledBack.identity, "runtime-rollback", limits);
  BOOST_REQUIRE(runtime.stageImportedConversationState(rolledBack, rollbackHandle, destinationOwner, 107));
  BOOST_REQUIRE(runtime.rollbackStagedDecodeStatePromotion(rolledBack));
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
  BOOST_CHECK(!destination.restoreConversationState(rollbackHandle, "rolled-back-runtime-state"));

  auto reserved = binding; reserved.contextEpoch = 4;
  reserved.identity.requestId = "runtime-reserved-request";
  reserved.identity.generationId = "runtime-reserved-generation";
  BOOST_REQUIRE(runtime.importConversationState(nextSnapshot, reserved, destinationOwner, limits, 108));
  BOOST_CHECK_EQUAL(runtime.conversationStateDrainSnapshot().activeTransitions, 0U);
  BOOST_CHECK_THROW(runtime.exportConversationState(reserved, limits, 109), std::runtime_error);
  BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(reserved, checkpoint));
  reserved.checkpointDigest = checkpoint;
  checkValues(runtime.exportConversationState(reserved, limits, 110), 4.0f);
  BOOST_REQUIRE(runtime.releaseConversationState(reserved));
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);

  // Reuse one actual ORT runner across two independently admitted conversations.
  // The first committed state represents destination prewarm occupancy.
  {
    NativeProviderRuntime warmRuntime(1, 8, 1024, 8, 384, 2, 384, 2);
    warmRuntime.bindConversationStateProvider("/provider/B", "boot-b", 8);
    auto warm = binding;
    warm.conversationId = "destination-prewarm-conversation";
    warm.identity.requestId = "destination-prewarm-request";
    warm.identity.generationId = "destination-prewarm-generation";
    warm.importedOriginIdentity = nextSnapshot.originIdentity;
    auto warmAdmission = warmRuntime.tryAcquireConversationExecution(
      "/requester/A", warm.serviceName, warm.conversationId);
    BOOST_REQUIRE(warmAdmission);
    auto warmOwnership = warmRuntime.importConversationStateOwned(
      nextSnapshot, warm, destinationOwner, limits, 111, "/requester/A", &*warmAdmission);
    BOOST_REQUIRE(warmOwnership);
    BOOST_REQUIRE(warmRuntime.commitImportedConversationState(*warmOwnership, checkpoint));
    auto append = warm;
    append.conversationId = "destination-append-conversation";
    append.identity.requestId = "destination-append-request";
    append.identity.generationId = "destination-append-generation";
    auto admission = warmRuntime.tryAcquireConversationExecution(
      "/requester/A", append.serviceName, append.conversationId);
    BOOST_REQUIRE(admission);
    auto wrongBoot = append; wrongBoot.identity.providerBootId = "foreign-boot";
    BOOST_CHECK(!warmRuntime.importConversationStateOwned(nextSnapshot, wrongBoot,
      destinationOwner, limits, 112, "/requester/A", &*admission));
    auto importedAppend = warmRuntime.importConversationStateOwned(nextSnapshot, append,
      destinationOwner, limits, 112, "/requester/A", &*admission);
    BOOST_REQUIRE(importedAppend);
    BOOST_REQUIRE(warmRuntime.commitImportedConversationState(*importedAppend, checkpoint));
    append.checkpointDigest = checkpoint;
    checkValues(warmRuntime.exportConversationState(append, limits, 113), 4.0f);
    BOOST_CHECK_EQUAL(warmRuntime.conversationStateSnapshot().entries, 2U);
    BOOST_CHECK_EQUAL(warmRuntime.conversationStateSnapshot().gpuBytes, 384U);
    auto excess = append;
    excess.checkpointDigest.clear(); excess.conversationId = "destination-excess-conversation";
    excess.identity.requestId = "destination-excess-request";
    excess.identity.generationId = "destination-excess-generation";
    auto excessAdmission = warmRuntime.tryAcquireConversationExecution(
      "/requester/A", excess.serviceName, excess.conversationId);
    BOOST_REQUIRE(excessAdmission);
    BOOST_CHECK(!warmRuntime.importConversationStateOwned(nextSnapshot, excess,
      destinationOwner, limits, 114, "/requester/A", &*excessAdmission));
    BOOST_CHECK(warmRuntime.releaseImportedConversationState(*importedAppend) ==
      ImportedStateReleaseResult::Released);
    BOOST_CHECK(warmRuntime.releaseImportedConversationState(*warmOwnership) ==
      ImportedStateReleaseResult::Released);
    BOOST_CHECK_EQUAL(warmRuntime.conversationStateSnapshot().entries, 0U);
  }

  // Restore the real four-token parent under this request's fresh destination
  // identity, then transfer READY's Store lease into the production worker.
  auto ownedBinding = binding;
  ownedBinding.contextEpoch = 5;
  ownedBinding.identity.requestId = "runtime-owned-request";
  ownedBinding.identity.generationId = "runtime-owned-generation";
  ownedBinding.importedOriginIdentity = nextSnapshot.originIdentity;
  auto ownership = runtime.importConversationStateOwned(
    nextSnapshot, ownedBinding, destinationOwner, limits, 111);
  BOOST_REQUIRE(ownership);
  BOOST_REQUIRE(runtime.commitImportedConversationState(*ownership, checkpoint));
  ownedBinding.checkpointDigest = checkpoint;
  auto readyUse = runtime.acquireConversationUse(ownedBinding, 112);
  BOOST_REQUIRE(readyUse);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().pinnedEntries, 1U);
  RoleSpec ownedRole;
  ownedRole.role = role;
  ownedRole.requestId = ownedBinding.identity.requestId;
  ownedRole.attemptEpoch = ownedBinding.identity.attemptEpoch;
  ownedRole.streamingStateExecution = true;
  ownedRole.stateInputNames = {
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  ownedRole.stateOutputNames = {
    "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
  ownedRole.conversationStateBinding = ownedBinding;
  ownedRole.conversationStateLookupNowMs = 112;
  ownedRole.outputs.emplace_back(
    "onnx-output-bundle", role, "/LLM/Pipeline/Stage/1", "", 0, 0,
    std::vector<std::string>{"hidden_out"});
  auto suffixIdentity = ownedBinding.identity;
  const auto fiveTokenPrefix = prefixThrough(7);
  suffixIdentity.prefixDigest = prefixDigest(fiveTokenPrefix);
  suffixIdentity.prefixTokenCount = fiveTokenPrefix.size();
  suffixIdentity.stateInferenceEpoch = 0;
  suffixIdentity.predecessorInferenceEpoch.reset();
  ownedRole.candidateDecodeStateIdentity = suffixIdentity;
  GenerationEpochLineageV1 suffixLineage;
  suffixLineage.requestId = ownedRole.requestId;
  suffixLineage.attemptEpoch = ownedRole.attemptEpoch;
  suffixLineage.planDigest = digest('1');
  suffixLineage.generationId = suffixIdentity.generationId;
  suffixLineage.streamEpoch = 1;
  suffixLineage.inferenceEpoch = 1;
  suffixLineage.logicalPrefixDigest = suffixIdentity.prefixDigest;
  suffixLineage.logicalPrefixTokenCount = suffixIdentity.prefixTokenCount;
  suffixLineage.positionDigest = suffixIdentity.positionDigest;
  suffixLineage.transitionKind = GenerationEpochLineageV1::DECODE;
  suffixLineage.producerRole = role;
  suffixLineage.consumerRole = role;
  ownedRole.generationLineage = suffixLineage;
  class RecordingDependencies final : public DependencyIo {
  public:
    std::optional<TensorBundle> published;
    std::future<TensorBundle> prefetchInput(const std::string&, const DependencyEdge&) override
    { throw std::runtime_error("unexpected dependency fetch in local ONNX continuation"); }
    void publishOutput(const std::string&, const DependencyEdge&, const TensorBundle& bundle) override
    { published = bundle; }
  };
  auto dependencies = std::make_shared<RecordingDependencies>();
  const auto decodeBefore = destination.runtimeMetricsSnapshot()->executedDecodeTokens;
  const auto ownedResult = runtime.executeRoleWithConversationUseAsync(
    "runtime-owned-session", ownedRole, dependencies,
    std::move(*readyUse), {{"input_ids", makeInputIds(7)}}).get();
  readyUse.reset();
  const auto ownedOutput = decodeOutput(ownedResult.outputsByScope);
  BOOST_REQUIRE(dependencies->published);
  const auto publishedTensors = decodeTensorBundle(dependencies->published->payload);
  const auto lineageTensor = std::find_if(
    publishedTensors.begin(), publishedTensors.end(), [] (const auto& tensor) {
      return tensor.name == generationEpochLineageTensorName();
    });
  BOOST_REQUIRE(lineageTensor != publishedTensors.end());
  const auto publishedLineage = extractGenerationEpochLineage(*dependencies->published);
  BOOST_REQUIRE(publishedLineage);
  BOOST_CHECK_EQUAL(publishedLineage->inferenceEpoch, 1U);
  BOOST_CHECK_EQUAL(publishedLineage->transitionKind,
                    GenerationEpochLineageV1::DECODE);
  BOOST_CHECK(!runtime.hasRunner(role));
  baselineOutput = decodeOutput(execute(baseline, "baseline-session", "baseline-request", 4, 7,
                                         GenerationEpochLineageV1::DECODE,
                                         prefixThrough(7)));
  BOOST_CHECK(findTensor(ownedOutput, "hidden_out").payload ==
              findTensor(baselineOutput, "hidden_out").payload);
  BOOST_CHECK_EQUAL(destination.runtimeMetricsSnapshot()->executedPrefillTokens, 0U);
  BOOST_CHECK_EQUAL(destination.runtimeMetricsSnapshot()->executedDecodeTokens,
                    decodeBefore + 1);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().pinnedEntries, 0U);
  BOOST_CHECK(runtime.releaseImportedConversationState(*ownership) ==
              ImportedStateReleaseResult::Released);
  BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
}

BOOST_AUTO_TEST_CASE(Spec204PortableDynamicSuccessorMappingContinuesDecodeWork)
{
  const auto fixture = findSpec204DynamicSuccessorFixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(), "dynamic-successor ONNX fixture required");
  const std::string role = "/LLM/Pipeline/Stage/0";
  const std::string inputName = "past_key_values.17.key";
  const std::string outputName = "present.17.key";
  const auto digest = [](char c) { return "sha256:" + std::string(64, c); };
  const auto prefixDigest = [&](const std::vector<std::int64_t>& tokenIds) {
    return nativeGenerationStatePrefixDigest(tokenIds, 1, digest('5'));
  };
  BOOST_CHECK_EQUAL(prefixDigest({3, 4}),
                    "sha256:cdd20d7bb5fceeb5490bd01126d5d722d8e199e2b59dba1afd82749019412432");
  BOOST_CHECK_NE(prefixDigest({3, 4}), prefixDigest({3, 5}));
  const auto makeSpec = [&](const std::string& provider, const std::string& boot) {
    NativeModelRunnerSpec spec;
    spec.role = role; spec.kind = "spec204-dynamic-successor";
    spec.backend = "onnxruntime"; spec.path = fixture.string();
    spec.metadata["executionProvider"] = "cpu";
    spec.metadata["statefulModel"] = "true";
    spec.metadata["inputNames"] = "input_ids," + inputName;
    spec.metadata["outputNames"] = outputName;
    spec.metadata["stateInputNames"] = inputName;
    spec.metadata["stateOutputNames"] = outputName;
    spec.metadata["stateSuccessorMap"] = inputName + "=" + outputName;
    spec.metadata["providerIdentity"] = provider;
    spec.metadata["providerBootId"] = boot;
    spec.metadata["workTokenInputName"] = "input_ids";
    spec.metadata["workTokenAxis"] = "1";
    return spec;
  };
  const auto makeIdentity = [&](const std::string& provider,
                                const std::string& boot,
                                const std::string& request) {
    DecodeStateIdentityV1 id;
    id.modelDigest = digest('1'); id.graphSemanticDigest = digest('2');
    id.artifactDigest = digest('3'); id.adapterDigest = digest('4');
    id.tokenizerDigest = digest('5'); id.runnerDigest = digest('6');
    id.roleName = role; id.roleSplitDigest = digest('7'); id.layerEnd = 1;
    const std::vector<std::int64_t> tokens{3, 4};
    id.prefixDigest = prefixDigest(tokens); id.prefixTokenCount = tokens.size();
    id.positionDigest = digest('9'); id.precision = "fp32";
    id.layoutDigest = digest('a'); id.stateSchemaDigest = digest('b');
    id.stateComponentDigests = {digest('c')}; id.runtimeAbiDigest = digest('d');
    id.securityDomainDigest = digest('e'); id.providerIdentity = provider;
    id.providerBootId = boot; id.cacheEpoch = 7; id.requestId = request;
    id.attemptEpoch = 1; id.generationId = request + "-generation";
    id.validate();
    return id;
  };
  const auto execute = [&](OnnxRuntimeModelRunner& runner,
                           const std::string& session,
                           const std::string& request,
                           const std::string& boot,
                           std::uint64_t epoch,
                           std::int64_t token,
                           const std::string& transition,
                           const std::vector<std::int64_t>& logicalPrefixTokens) {
    if (logicalPrefixTokens.empty() || logicalPrefixTokens.back() != token ||
        logicalPrefixTokens.size() != epoch + 1)
      throw std::logic_error("dynamic successor fixture prefix does not match its token sequence");
    RoleExecutionContext ctx;
    ctx.sessionId = session; ctx.requestId = request; ctx.role = role;
    ctx.providerBootId = boot; ctx.attemptEpoch = 1;
    ctx.inferenceEpoch = epoch; ctx.generationInputTokenCount = 1;
    ctx.streamingStateExecution = true;
    ctx.inputsByScope.emplace("input_ids", makeInputIds(token));
    GenerationEpochLineageV1 lineage;
    lineage.requestId = request; lineage.attemptEpoch = 1;
    lineage.planDigest = digest('1'); lineage.generationId = request + "-generation";
    lineage.inferenceEpoch = epoch;
    lineage.logicalPrefixDigest = prefixDigest(logicalPrefixTokens);
    lineage.logicalPrefixTokenCount = logicalPrefixTokens.size();
    lineage.positionDigest = digest('9');
    lineage.transitionKind = transition;
    ctx.generationLineage = lineage;
    return decodeOutput(runner.run(ctx));
  };

  NativePortableKvLimitsV1 limits{64, 1024, {}};
  auto source = std::make_unique<OnnxRuntimeModelRunner>(makeSpec("/provider/A", "boot-a"));
  BOOST_REQUIRE(source->supportsPortableConversationState());
  execute(*source, "session-a", "request-a", "boot-a", 0, 3,
          GenerationEpochLineageV1::PREFILL, {3});
  execute(*source, "session-a", "request-a", "boot-a", 1, 4,
          GenerationEpochLineageV1::DECODE, {3, 4});
  const auto sourceHandle = source->promoteSessionStateToConversation("session-a", "conversation-a");
  BOOST_REQUIRE(sourceHandle);
  auto origin = makeIdentity("/provider/A", "boot-a", "request-a");
  origin.stateInferenceEpoch = sourceHandle->opaque.stateInferenceEpoch;
  if (origin.stateInferenceEpoch) origin.predecessorInferenceEpoch = origin.stateInferenceEpoch - 1;
  const auto snapshot = source->exportConversationState(*sourceHandle, origin, limits);
  BOOST_REQUIRE_EQUAL(snapshot.components.size(), 1U);
  BOOST_CHECK_EQUAL(snapshot.components[0].inputName, inputName);
  BOOST_CHECK((snapshot.components[0].shape == std::vector<std::int64_t>{1, 2, 1}));
  BOOST_CHECK_EQUAL(snapshot.logicalBytes, 8U);
  const auto snapshotTensor = decodeTensorBundle(snapshot.components[0].tensorWire);
  BOOST_REQUIRE_EQUAL(snapshotTensor.size(), 1U);
  BOOST_CHECK_EQUAL(snapshotTensor[0].name, inputName);
  std::vector<float> savedValues(2);
  std::memcpy(savedValues.data(), snapshotTensor[0].payload.data(), snapshotTensor[0].payload.size());
  BOOST_CHECK_EQUAL(savedValues[0], 3.0f);
  BOOST_CHECK_EQUAL(savedValues[1], 4.0f);
  const auto sourceMetrics = source->runtimeMetricsSnapshot();
  BOOST_REQUIRE(sourceMetrics);
  BOOST_CHECK_EQUAL(sourceMetrics->executedPrefillTokens, 1U);
  BOOST_CHECK_EQUAL(sourceMetrics->executedDecodeTokens, 1U);
  source.reset();

  auto target = origin;
  target.providerIdentity = "/provider/B"; target.providerBootId = "boot-b";
  target.cacheEpoch = 8; target.requestId = "request-b";
  target.generationId = "request-b-generation";
  auto destinationOwner = std::make_shared<OnnxRuntimeModelRunner>(makeSpec("/provider/B", "boot-b"));
  auto& destination = *destinationOwner;
  const auto imported = destination.importConversationState(snapshot, target, "conversation-b", limits);
  BOOST_REQUIRE(destination.restoreConversationState(imported, "session-b"));
  (void)execute(destination, "session-b", "request-b", "boot-b", 2, 6,
                GenerationEpochLineageV1::DECODE, {3, 4, 6});
  const auto successorHandle = destination.promoteSessionStateToConversation(
    "session-b", "conversation-b-after-suffix");
  BOOST_REQUIRE(successorHandle);
  BOOST_REQUIRE_EQUAL(successorHandle->opaque.stateInferenceEpoch, 2U);
  auto successorIdentity = target;
  successorIdentity.stateInferenceEpoch = successorHandle->opaque.stateInferenceEpoch;
  successorIdentity.predecessorInferenceEpoch = successorIdentity.stateInferenceEpoch - 1;
  successorIdentity.prefixDigest = prefixDigest({3, 4, 6});
  successorIdentity.prefixTokenCount = 3;
  const auto successorSnapshot = destination.exportConversationState(
    *successorHandle, successorIdentity, limits);
  BOOST_REQUIRE_EQUAL(successorSnapshot.components.size(), 1U);
  BOOST_CHECK_EQUAL(successorSnapshot.components[0].inputName, inputName);
  BOOST_CHECK((successorSnapshot.components[0].shape ==
               std::vector<std::int64_t>{1, 3, 1}));

  OnnxRuntimeModelRunner baseline(makeSpec("/provider/C", "boot-c"));
  (void)execute(baseline, "session-c", "request-c", "boot-c", 0, 3,
                GenerationEpochLineageV1::PREFILL, {3});
  (void)execute(baseline, "session-c", "request-c", "boot-c", 1, 4,
                GenerationEpochLineageV1::DECODE, {3, 4});
  (void)execute(baseline, "session-c", "request-c", "boot-c", 2, 6,
                GenerationEpochLineageV1::DECODE, {3, 4, 6});
  const auto baselineHandle = baseline.promoteSessionStateToConversation(
    "session-c", "conversation-c-full");
  BOOST_REQUIRE(baselineHandle);
  BOOST_REQUIRE_EQUAL(baselineHandle->opaque.stateInferenceEpoch, 2U);
  auto baselineIdentity = makeIdentity("/provider/C", "boot-c", "request-c");
  baselineIdentity.stateInferenceEpoch = baselineHandle->opaque.stateInferenceEpoch;
  baselineIdentity.predecessorInferenceEpoch = baselineIdentity.stateInferenceEpoch - 1;
  baselineIdentity.prefixDigest = prefixDigest({3, 4, 6});
  baselineIdentity.prefixTokenCount = 3;
  const auto baselineSnapshot = baseline.exportConversationState(
    *baselineHandle, baselineIdentity, limits);
  BOOST_REQUIRE_EQUAL(baselineSnapshot.components.size(), 1U);
  BOOST_CHECK(successorSnapshot.components[0].tensorWire ==
              baselineSnapshot.components[0].tensorWire);
  const auto destinationMetrics = destination.runtimeMetricsSnapshot();
  const auto baselineMetrics = baseline.runtimeMetricsSnapshot();
  BOOST_REQUIRE(destinationMetrics);
  BOOST_REQUIRE(baselineMetrics);
  BOOST_CHECK(destinationMetrics->workCounterCertified);
  BOOST_CHECK_EQUAL(destinationMetrics->executedPrefillTokens, 0U);
  BOOST_CHECK_EQUAL(destinationMetrics->executedDecodeTokens, 1U);
  BOOST_CHECK_EQUAL(baselineMetrics->executedPrefillTokens, 1U);
  BOOST_CHECK_EQUAL(baselineMetrics->executedDecodeTokens, 2U);
  BOOST_CHECK(destination.releaseConversationState(imported));
  BOOST_CHECK(destination.releaseConversationState(*successorHandle));
  destination.releaseSessionState("session-b");
  BOOST_CHECK(baseline.releaseConversationState(*baselineHandle));
  baseline.releaseSessionState("session-c");
}

BOOST_AUTO_TEST_CASE(Spec204RuntimeRebindPreservesConcurrentlyAdmittedImportedState)
{
  // This ownership fixture deliberately does not run inference. The preceding
  // actual ONNX case proves tensor portability; here the oracle is commit plus
  // exactly-once confirmed release across a concurrent incarnation change.
  class Runner final : public NativeModelRunner {
  public:
    std::atomic<unsigned> releases{0};
    std::function<void()> releaseProbe;
    std::map<std::string, TensorBundle> run(const RoleExecutionContext&) override
    { throw std::logic_error("ownership fixture must not execute inference"); }
    bool releaseConversationState(const NativeConversationStateHandleV1&) override
    { if (releaseProbe) releaseProbe(); ++releases; return true; }
  };
  const auto digest = [](char c) { return "sha256:" + std::string(64, c); };
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    NativeProviderRuntime runtime(1, 8, 1024, 8, 256, 8, 256, 8);
    auto runner = std::make_shared<Runner>();
    NativeModelRunnerSpec spec;
    spec.role = "/Stage/0"; spec.metadata["evidence.providerName"] = "/provider/B";
    spec.metadata["providerBootId"] = "boot-old"; spec.metadata["state.cacheEpoch"] = "7";
    runtime.registerRunner(spec, runner);
    DecodeStateIdentityV1 id;
    id.modelDigest = digest('1'); id.graphSemanticDigest = digest('2');
    id.artifactDigest = digest('3'); id.adapterDigest = digest('4');
    id.tokenizerDigest = digest('5'); id.runnerDigest = digest('6');
    id.roleName = spec.role; id.roleSplitDigest = digest('7'); id.layerEnd = 4;
    id.prefixDigest = digest('8'); id.prefixTokenCount = 3; id.positionDigest = digest('9');
    id.precision = "fp32"; id.layoutDigest = digest('a'); id.stateSchemaDigest = digest('b');
    id.stateComponentDigests = {digest('c')}; id.runtimeAbiDigest = digest('e');
    id.securityDomainDigest = digest('f'); id.providerIdentity = "/provider/B";
    id.providerBootId = "boot-old"; id.cacheEpoch = 7; id.requestId = "old-request";
    id.generationId = "old-generation";
    ConversationStateBinding old;
    old.conversationId = "rebind-old-conversation"; old.contextEpoch = 1;
    old.serviceName = "/LLM/Pipeline/Generate"; old.planRoleMapDigest = digest('0');
    old.receiptDigest = digest('9'); old.expiresAtMs = 10'000; old.identity = id;
    const auto handleFor = [](const ConversationStateBinding& binding) {
      NativeConversationStateHandleV1 handle;
      handle.opaque.providerIdentity = binding.identity.providerIdentity;
      handle.opaque.providerBootId = binding.identity.providerBootId;
      handle.opaque.sessionId = binding.identity.requestId;
      handle.opaque.role = binding.identity.roleName;
      handle.opaque.token = "ndnsf-resident-state-v1:" + binding.identity.requestId;
      handle.conversationKey = binding.conversationId; handle.logicalBytes = 8;
      return handle;
    };
    BOOST_REQUIRE(runtime.stageImportedConversationState(old, handleFor(old), runner, 100));
    auto fresh = old; fresh.conversationId = "rebind-new-conversation";
    fresh.identity.providerBootId = "boot-new"; fresh.identity.cacheEpoch = 8;
    fresh.identity.requestId = "new-request"; fresh.identity.generationId = "new-generation";
    std::atomic<bool> releaseEntered{false};
    struct ClearProbe {
      std::shared_ptr<Runner> owner;
      ~ClearProbe() { owner->releaseProbe = {}; }
    } clearProbe{runner};
    runner->releaseProbe = [&] { releaseEntered = true; };
    auto admission = std::async(std::launch::async, [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline) {
        if (releaseEntered && runtime.stageImportedConversationState(fresh, handleFor(fresh), runner, 101))
          return true;
        std::this_thread::yield();
      }
      return false;
    });
    spec.metadata["providerBootId"] = "boot-new"; spec.metadata["state.cacheEpoch"] = "8";
    runtime.registerRunner(spec, runner);
    BOOST_REQUIRE(admission.get());
    runner->releaseProbe = {};
    BOOST_CHECK(!runtime.commitStagedDecodeStatePromotion(old, digest('a')));
    BOOST_REQUIRE(runtime.commitStagedDecodeStatePromotion(fresh, digest('a')));
    fresh.checkpointDigest = digest('a');
    BOOST_REQUIRE(runtime.releaseConversationState(fresh));
    BOOST_CHECK_EQUAL(runner->releases.load(), 2U);
    BOOST_CHECK_EQUAL(runtime.conversationStateSnapshot().entries, 0U);
  }
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

// Explicit diagnostic only: the normal suite does not require external weights.
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
