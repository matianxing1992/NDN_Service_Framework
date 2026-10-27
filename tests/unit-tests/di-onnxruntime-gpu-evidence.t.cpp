#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"

#include <boost/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <ndn-cxx/util/logging.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ndnsf::di::tests {

namespace {

class TimingSnapshotRunner final : public NativeModelRunner
{
public:
  explicit TimingSnapshotRunner(bool stale) : m_stale(stale) {}

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& context) override
  {
    NativeBackendExecutionTiming timing;
    auto identity = std::make_shared<NativeBackendExecutionIdentity>();
    identity->requestId = m_stale ? "/another/request" : context.requestId;
    identity->sessionId = context.sessionId;
    identity->role = context.role;
    identity->providerBootId = context.providerBootId;
    identity->attemptEpoch = context.attemptEpoch;
    timing.identity = std::move(identity);
    timing.inferenceEpoch = context.inferenceEpoch;
    timing.completed = true;
    m_metrics.lastBackendExecution = std::move(timing);
    m_metrics.activationOutputBytes = 123;
    return {{"output", TensorBundle{"output", {42}}}};
  }

  std::optional<NativeRuntimeMetrics>
  runtimeMetricsSnapshot() const override
  {
    return m_metrics;
  }

private:
  bool m_stale;
  NativeRuntimeMetrics m_metrics;
};

class NoTimingDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    throw std::runtime_error("timing fixture has no dependency inputs");
  }

  void
  publishOutput(const std::string&, const DependencyEdge&, const TensorBundle&) override
  {
    throw std::runtime_error("timing fixture has no dependency outputs");
  }
};

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
  BOOST_REQUIRE_EQUAL(evidence.getNodeProviderAssignments().size(), 2);
  BOOST_CHECK_EQUAL(evidence.getNodeProviderAssignments().front().provider,
                    "CUDAExecutionProvider");
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-test-uuid");
  BOOST_CHECK(!evidence.cpuFallbackUsed);
  const auto retained = evidence;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  std::ofstream(path) << R"json([
    {"cat":"Node","name":"New_0_kernel_time","args":{"provider":"CUDAExecutionProvider"}},
    {"cat":"Node","name":"New_1_kernel_time","args":{"provider":"CPUExecutionProvider"}}
  ])json";
  BOOST_CHECK_THROW(applyOnnxRuntimeProviderProfile(
    evidence, path.string(), "/LLM/Stage/0", true, "GPU-rejected"), std::runtime_error);
  BOOST_REQUIRE_EQUAL(evidence.getNodeProviderAssignments().size(), 2);
  BOOST_CHECK_EQUAL(evidence.getNodeProviderAssignments().front().nodeName, "MatMul_0_kernel_time");
  BOOST_CHECK_EQUAL(evidence.getNodeProviderAssignments().back().nodeName, "Add_1_kernel_time");
  BOOST_CHECK_EQUAL(retained.getNodeProviderAssignments().front().nodeName, "MatMul_0_kernel_time");
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

BOOST_AUTO_TEST_CASE(BackendExecutionStatisticsWorkWithDiagnosticsDisabled)
{
  static_assert(std::is_nothrow_copy_constructible<NativeBackendExecutionTiming>::value,
                "backend timing snapshot copies must not allocate");
  ScopedRunnerTimingCapture capture;
  const char* previous = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  const bool hadRuntimeTiming = previous != nullptr;
  const std::string previousRuntimeTiming = previous ? previous : "";
  BOOST_SCOPE_EXIT(&hadRuntimeTiming, &previousRuntimeTiming) {
    if (hadRuntimeTiming) ::setenv("NDNSF_DI_RUNTIME_TIMING", previousRuntimeTiming.c_str(), 1);
    else ::unsetenv("NDNSF_DI_RUNTIME_TIMING");
    ndn::util::Logging::setLevel("*=WARN");
  } BOOST_SCOPE_EXIT_END
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_PHASE_TIMING", "0", 1), 0);
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_DI_RUNTIME_TIMING", "0", 1), 0);
  ndn::util::Logging::setLevel("*=NONE");
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE(!fixture.empty());
  OnnxRuntimeModelRunner runner(makeStatefulSpec(
    fixture / "role-1.onnx", "/LLM/Pipeline/Stage/1"));
  RoleExecutionContext context;
  context.requestId = "/quiet/request-a";
  context.sessionId = "quiet-session-a";
  context.role = "/LLM/Pipeline/Stage/1";
  context.providerBootId = "quiet-boot";
  context.attemptEpoch = 1;
  context.inputsByScope = {
    {"attention_kv_in", makeZeroState("attention_kv_in")},
    {"recurrent_state_in", makeZeroState("recurrent_state_in")},
    {"convolution_state_in", makeZeroState("convolution_state_in")}};
  const auto setToken = [&] (float token) {
    std::vector<float> hidden{token, 1, 0, 0, 0, 0, 0, 0};
    std::vector<std::uint8_t> bytes(hidden.size() * sizeof(float));
    std::memcpy(bytes.data(), hidden.data(), bytes.size());
    context.inputsByScope["hidden_in"] = makeEncodedTensorBundle(
      "hidden_in", {NamedTensor{"hidden_in", TensorElementType::Float32,
                                {1, 1, 8}, std::move(bytes)}});
  };
  const auto now = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  setToken(3);
  const auto before = now();
  const auto outputs = decodeOutput(runner.run(context));
  const auto after = now();
  BOOST_CHECK_EQUAL(findTensor(outputs, "logits").payload.size(), 32U * sizeof(float));
  const auto firstMetrics = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(firstMetrics && firstMetrics->lastBackendExecution);
  const auto first = *firstMetrics->lastBackendExecution;
  BOOST_CHECK(first.matches(context));
  BOOST_CHECK(first.completed);
  BOOST_CHECK_GE(first.startedSteadyNs, before);
  BOOST_CHECK_GE(first.finishedSteadyNs, first.startedSteadyNs);
  BOOST_CHECK_LE(first.finishedSteadyNs, after);
  BOOST_CHECK_GE(first.startedProcessCpuNs, -1);
  BOOST_CHECK_GE(first.finishedProcessCpuNs, -1);
  BOOST_CHECK_GE(first.startedThreadCpuNs, -1);
  BOOST_CHECK_GE(first.finishedThreadCpuNs, -1);
  if (first.startedProcessCpuNs >= 0 && first.finishedProcessCpuNs >= 0)
    BOOST_CHECK_GE(first.finishedProcessCpuNs, first.startedProcessCpuNs);
  if (first.startedThreadCpuNs >= 0 && first.finishedThreadCpuNs >= 0)
    BOOST_CHECK_GE(first.finishedThreadCpuNs, first.startedThreadCpuNs);
#if defined(CLOCK_THREAD_CPUTIME_ID) || defined(CLOCK_PROCESS_CPUTIME_ID)
  BOOST_CHECK_EQUAL(detail::nativeClockCpuTimeNs(static_cast<clockid_t>(-1)), -1);
#endif
  auto mismatch = context;
  mismatch.requestId += "-other";
  BOOST_CHECK(!first.matches(mismatch));
  mismatch = context;
  mismatch.sessionId += "-other";
  BOOST_CHECK(!first.matches(mismatch));
  mismatch = context;
  mismatch.role += "-other";
  BOOST_CHECK(!first.matches(mismatch));
  mismatch = context;
  mismatch.providerBootId += "-other";
  BOOST_CHECK(!first.matches(mismatch));
  mismatch = context;
  ++mismatch.attemptEpoch;
  BOOST_CHECK(!first.matches(mismatch));
  mismatch = context;
  ++mismatch.inferenceEpoch;
  BOOST_CHECK(!first.matches(mismatch));

  context.requestId = "/quiet/request-b";
  context.sessionId = "quiet-session-b";
  context.attemptEpoch = 2;
  context.inferenceEpoch = 7;
  BOOST_REQUIRE_NO_THROW(runner.run(context));
  const auto secondMetrics = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(secondMetrics && secondMetrics->lastBackendExecution);
  BOOST_CHECK(secondMetrics->lastBackendExecution->matches(context));
  BOOST_CHECK(secondMetrics->lastBackendExecution->completed);
  BOOST_CHECK_GE(secondMetrics->lastBackendExecution->startedSteadyNs, first.finishedSteadyNs);
  BOOST_CHECK_EQUAL(first.identity->requestId, "/quiet/request-a");
  BOOST_CHECK(first.identity != secondMetrics->lastBackendExecution->identity);

  // The fixture's real Gather rejects this index inside ORT, after admission.
  setToken(1000000);
  BOOST_CHECK_THROW(runner.run(context), std::exception);
  const auto failedMetrics = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(failedMetrics && failedMetrics->lastBackendExecution);
  BOOST_CHECK(failedMetrics->lastBackendExecution->matches(context));
  BOOST_CHECK(!failedMetrics->lastBackendExecution->completed);
  BOOST_CHECK_GE(failedMetrics->lastBackendExecution->finishedSteadyNs,
                 failedMetrics->lastBackendExecution->startedSteadyNs);
  setToken(4);
  BOOST_REQUIRE_NO_THROW(runner.run(context));
  const auto recovered = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(recovered && recovered->lastBackendExecution);
  BOOST_CHECK(recovered->lastBackendExecution->completed);
  BOOST_CHECK(recovered->lastBackendExecution->matches(context));
  BOOST_CHECK(recovered->lastBackendExecution->identity ==
              secondMetrics->lastBackendExecution->identity);
  ndn::util::Logging::flush();
  BOOST_CHECK(capture.output.str().empty());
}

BOOST_AUTO_TEST_CASE(WorkerDiscardsStaleAndCachedBackendTimingOnly)
{
  for (bool stale : {false, true}) {
    NativeProviderRuntime runtime(1);
    const std::string roleName = "/Statistics/Role";
    runtime.registerRunner(roleName, std::make_shared<TimingSnapshotRunner>(stale));
    auto io = std::make_shared<NoTimingDependencyIo>();
    const RoleSpec role(roleName, {}, {}, "/worker/request", 3);
    const auto first = runtime.executeRoleAsync("statistics-session", role, io).get();
    BOOST_REQUIRE(first.runtimeMetrics);
    BOOST_CHECK_EQUAL(first.outputsByScope.at("output").payload.at(0), 42);
    BOOST_CHECK_EQUAL(first.runtimeMetrics->activationOutputBytes, 123);
    BOOST_CHECK(!first.exactForwardCacheHit);
    BOOST_CHECK_EQUAL(first.runtimeMetrics->lastBackendExecution.has_value(), !stale);
    const auto cached = runtime.executeRoleAsync("statistics-session", role, io).get();
    BOOST_REQUIRE(cached.runtimeMetrics);
    BOOST_CHECK(cached.exactForwardCacheHit);
    BOOST_CHECK(!cached.runtimeMetrics->lastBackendExecution);
    BOOST_CHECK_EQUAL(cached.runtimeMetrics->activationOutputBytes, 123);
    BOOST_CHECK_EQUAL(cached.outputsByScope.at("output").payload.at(0), 42);
  }
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
  std::shared_ptr<const NativeBackendExecutionIdentity> timingIdentity;
  for (std::uint64_t epoch = 0; epoch < 3; ++epoch) {
    context.inferenceEpoch = epoch;
    context.inputsByScope.clear();
    context.inputsByScope.emplace(
      "input_ids", makeInputIds(static_cast<std::int64_t>(3 + epoch)));
    const auto outputs = runner.run(context);
    const auto metrics = runner.runtimeMetricsSnapshot();
    BOOST_REQUIRE(metrics && metrics->lastBackendExecution);
    BOOST_CHECK(metrics->lastBackendExecution->matches(context));
    BOOST_CHECK(metrics->lastBackendExecution->completed);
    if (!timingIdentity) timingIdentity = metrics->lastBackendExecution->identity;
    BOOST_CHECK(timingIdentity == metrics->lastBackendExecution->identity);
    BOOST_CHECK_GE(metrics->lastBackendExecution->finishedSteadyNs,
                   metrics->lastBackendExecution->startedSteadyNs);
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
