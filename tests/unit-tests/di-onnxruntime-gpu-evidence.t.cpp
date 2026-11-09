#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-runtime-graph-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/cuda-device-memory.hpp"

#include <onnx/onnx_pb.h>

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
#include <memory>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include <unistd.h>

namespace ndnsf::di::tests {

namespace {

struct GraphMetadataFixture
{
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
    ("ndnsf-graph-metadata-" + std::to_string(getpid()) + "-" +
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  GraphMetadataFixture() { std::filesystem::create_directory(directory); }
  ~GraphMetadataFixture() { std::filesystem::remove_all(directory); }

  std::string
  write(const onnx::ModelProto& model) const
  {
    const auto path = directory / "model.onnx";
    std::ofstream output(path, std::ios::binary);
    if (!model.SerializeToOstream(&output)) throw std::runtime_error("cannot write fixture");
    return path.string();
  }

  std::string
  profile(const std::string& contents) const
  {
    const auto path = directory / "profile.json";
    std::ofstream(path) << contents;
    return path.string();
  }
};

onnx::ModelProto
makeShapeMetadataModel()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(18);
  auto& graph = *model.mutable_graph();
  auto addValue = [] (auto* value, const std::string& name) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  addValue(graph.add_input(), "input");
  addValue(graph.add_output(), "output");
  auto* shape = graph.add_node();
  shape->set_name("shape"); shape->set_op_type("Shape");
  shape->add_input("input"); shape->add_output("dims");
  auto* cast = graph.add_node();
  cast->set_name("shape-cast"); cast->set_op_type("Cast");
  cast->add_input("dims"); cast->add_output("shape-dims");
  auto* to = cast->add_attribute();
  to->set_name("to"); to->set_type(onnx::AttributeProto::INT);
  to->set_i(onnx::TensorProto::INT64);
  auto* reshape = graph.add_node();
  reshape->set_name("reshape"); reshape->set_op_type("Reshape");
  reshape->add_input("input"); reshape->add_input("shape-dims");
  reshape->add_output("reshaped");
  auto* identity = graph.add_node();
  identity->set_name("identity"); identity->set_op_type("Identity");
  identity->add_input("reshaped"); identity->add_output("output");
  return model;
}

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

// Run this selector alone in a fresh GPU process, before any CPU runner creates Env.
BOOST_AUTO_TEST_CASE(FirstCudaRunnerOwnsEnvironmentBeforeProviderOptions,
                     *boost::unit_test::disabled())
{
  const auto* enabled = std::getenv("NDNSF_TEST_FIRST_CUDA_SESSION");
  BOOST_REQUIRE(enabled && std::string(enabled) == "1");
  std::unique_ptr<CudaDeviceMemoryQuery> deviceBinding;
  if (const auto* binding = std::getenv("NDNSF_TEST_CUDA_BINDING_BEFORE_SESSION")) {
    BOOST_REQUIRE_EQUAL(std::string(binding), "1");
    deviceBinding = std::make_unique<CudaDeviceMemoryQuery>(0);
    deviceBinding->prepare();
  }
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  graph.set_name("first-cuda-session");
  graph.clear_node();
  auto* square = graph.add_node();
  square->set_name("square");
  square->set_op_type("Mul");
  square->add_input("input");
  square->add_input("input");
  square->add_output("output");
  NativeModelRunnerSpec spec;
  spec.role = "/Test/FirstCuda/Role";
  spec.backend = "onnxruntime-cuda";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"},
    {"forceOutputBundle", "true"}, {"outputBundleScope", "squared"},
    {"allowCpuFallback", "false"}, {"evidence.providerName", "/test/provider/first-cuda"},
    {"evidence.providerBootId", "first-cuda-boot"},
    {"evidence.modelDigest", "sha256:" + std::string(64, '1')},
    {"evidence.planDigest", "sha256:" + std::string(64, '2')},
    {"evidence.artifactDigest", "sha256:" + std::string(64, '3')},
    {"evidence.createdAtMs", "1"}};
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    spec.metadata["providerProfilePrefix"] =
      (fixture.directory / ("cuda-" + std::to_string(attempt))).string();
    OnnxRuntimeModelRunner runner(spec);
    RoleExecutionContext context;
    context.requestId = "/test/first-cuda/request/" + std::to_string(attempt);
    context.attemptEpoch = 1;
    context.sessionId = "first-cuda-session";
    context.providerBootId = "first-cuda-boot";
    context.role = spec.role;
    context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
      {"input", TensorElementType::Float32, {2, 2}, float32Payload({1, 2, 3, 4})}}));
    const auto outputs = runner.run(context);
    const auto tensors = decodeTensorBundle(outputs.at("squared").payload);
    BOOST_CHECK(findTensor(tensors, "output").payload == float32Payload({1, 4, 9, 16}));
    const auto evidence = runner.executionEvidenceSnapshot();
    BOOST_REQUIRE(evidence);
    BOOST_CHECK(evidence->runnerKind == RunnerKind::OnnxRuntimeCuda);
    BOOST_CHECK(!evidence->cpuFallbackUsed);
    const auto& assignments = evidence->getNodeProviderAssignments();
    BOOST_REQUIRE(!assignments.empty());
    BOOST_CHECK(std::any_of(assignments.begin(), assignments.end(), [](const auto& item) {
      return item.nodeName == "square_kernel_time" && item.provider == "CUDAExecutionProvider";
    }));
  }
}

BOOST_AUTO_TEST_CASE(GroupedAttentionWarmupUsesGraphLengthWithoutChangingRequests)
{
  GraphMetadataFixture fixture;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(18);
  auto* contrib = model.add_opset_import();
  contrib->set_domain("com.microsoft");
  contrib->set_version(1);
  auto* graph = model.mutable_graph();
  graph->set_name("grouped-attention-warmup");
  const auto addValue = [] (onnx::ValueInfoProto* value, const std::string& name,
                           int type, std::initializer_list<std::int64_t> shape) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    auto* dimensions = tensor->mutable_shape();
    for (auto size : shape) dimensions->add_dim()->set_dim_value(size);
  };
  for (const auto* name : {"query", "key", "value"})
    addValue(graph->add_input(), name, onnx::TensorProto::FLOAT, {1, 1, 8});
  addValue(graph->add_input(), "lengths", onnx::TensorProto::INT32, {1});
  addValue(graph->add_input(), "total", onnx::TensorProto::INT32, {});
  addValue(graph->add_output(), "output", onnx::TensorProto::FLOAT, {1, 1, 8});
  addValue(graph->add_output(), "present_key", onnx::TensorProto::FLOAT, {1, 1, 1, 8});
  addValue(graph->add_output(), "present_value", onnx::TensorProto::FLOAT, {1, 1, 1, 8});
  auto* attention = graph->add_node();
  attention->set_name("attention");
  attention->set_domain("com.microsoft");
  attention->set_op_type("GroupQueryAttention");
  for (const auto* name : {"query", "key", "value", "", "", "lengths", "total"})
    attention->add_input(name);
  for (const auto* name : {"output", "present_key", "present_value"})
    attention->add_output(name);
  for (const auto* name : {"num_heads", "kv_num_heads"}) {
    auto* attribute = attention->add_attribute();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::INT);
    attribute->set_i(1);
  }
  NativeModelRunnerSpec spec;
  spec.role = "/Grouped/Attention";
  spec.backend = "onnxruntime-cpu";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "query,key,value,lengths,total"},
    {"outputNames", "output,present_key,present_value"},
    {"forceOutputBundle", "true"}, {"outputBundleScope", "gqa"}};
  const auto facts = detail::inspectOnnxRuntimeGraphMetadata(spec.path, false);
  BOOST_CHECK(facts.attentionWarmupLengthInputs == std::set<std::string>({"total"}));
  BOOST_CHECK(facts.hostMetadataNodes.empty());
  const auto strict = detail::inspectOnnxRuntimeGraphMetadata(spec.path);
  BOOST_CHECK(strict.attentionWarmupLengthInputs == facts.attentionWarmupLengthInputs);
  BOOST_CHECK(strict.hostMetadataNodes.empty());
  {
    auto unsupported = model;
    unsupported.mutable_opset_import(1)->set_version(2);
    GraphMetadataFixture wrongVersion;
    BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(wrongVersion.write(unsupported), false)
                  .attentionWarmupLengthInputs.empty());
    unsupported = model;
    unsupported.mutable_graph()->mutable_node(0)->set_domain("foreign");
    GraphMetadataFixture wrongDomain;
    BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(wrongDomain.write(unsupported), false)
                  .attentionWarmupLengthInputs.empty());
    unsupported = model;
    auto* function = unsupported.add_functions();
    function->set_domain("com.microsoft");
    function->set_name("GroupQueryAttention");
    GraphMetadataFixture localFunction;
    BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(localFunction.write(unsupported), false)
                  .attentionWarmupLengthInputs.empty());
  }
  OnnxRuntimeModelRunner runner(spec);
  std::vector<NamedTensor> inputs{
    {"query", TensorElementType::Float32, {1, 1, 8}, float32Payload({0, 0, 0, 0, 0, 0, 0, 0})},
    {"key", TensorElementType::Float32, {1, 1, 8}, float32Payload({0, 0, 0, 0, 0, 0, 0, 0})},
    {"value", TensorElementType::Float32, {1, 1, 8}, float32Payload({1, 2, 3, 4, 5, 6, 7, 8})},
    {"lengths", TensorElementType::Int32, {1}, {0, 0, 0, 0}},
    {"total", TensorElementType::Int32, {}, {1, 0, 0, 0}}};
  RoleExecutionContext context;
  context.sessionId = "grouped-attention";
  context.role = spec.role;
  context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", inputs));
  const auto output = runner.run(context);
  const auto tensors = decodeTensorBundle(output.at("gqa").payload);
  BOOST_CHECK(findTensor(tensors, "output").payload == inputs[2].payload);
  inputs.back().payload = {0, 0, 0, 0};
  context.inputsByScope.at("activation") = makeEncodedTensorBundle("activation", inputs);
  BOOST_CHECK_THROW(runner.run(context), std::exception);
}

BOOST_AUTO_TEST_CASE(Int32AuxiliaryTensorsKeepExactValuesAcrossCpuExecution)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  graph.clear_node();
  graph.mutable_input(0)->mutable_type()->mutable_tensor_type()->set_elem_type(
    onnx::TensorProto::INT32);
  graph.mutable_output(0)->mutable_type()->mutable_tensor_type()->set_elem_type(
    onnx::TensorProto::INT32);
  auto* identity = graph.add_node();
  identity->set_op_type("Identity");
  identity->add_input("input");
  identity->add_output("output");
  NativeModelRunnerSpec spec;
  spec.role = "/Int32/Auxiliary";
  spec.backend = "onnxruntime-cpu";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"},
    {"forceOutputBundle", "true"}, {"outputBundleScope", "int32-output"}};
  OnnxRuntimeModelRunner runner(spec);
  // Independent little-endian INT32_MIN, -1, 0, INT32_MAX values.
  const std::vector<std::uint8_t> values{
    0, 0, 0, 128, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 127};
  const auto int32Type = static_cast<TensorElementType>(6);
  RoleExecutionContext context;
  context.sessionId = "int32-auxiliary";
  context.role = spec.role;
  context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
    {"input", int32Type, {2, 2}, values}}));
  const auto outputs = runner.run(context);
  const auto tensors = decodeTensorBundle(outputs.at("int32-output").payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors[0].name, "output");
  BOOST_CHECK_EQUAL(static_cast<std::uint32_t>(tensors[0].elementType), 6U);
  BOOST_CHECK(tensors[0].shape == std::vector<std::int64_t>({2, 2}));
  BOOST_CHECK(tensors[0].payload == values);
  BOOST_CHECK_EQUAL(tensorElementByteSize(int32Type), 4U);
  auto malformed = values;
  malformed.pop_back();
  BOOST_CHECK_THROW(encodeTensorBundle({{"input", int32Type, {2, 2}, malformed}}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(encodeTensorBundle({{"input", static_cast<TensorElementType>(7),
                                        {2, 2}, values}}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NBitsEightBitCpuMatchesIndependentBlockDequantization)
{
  GraphMetadataFixture fixture;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  auto* contrib = model.add_opset_import();
  contrib->set_domain("com.microsoft");
  contrib->set_version(1);
  auto& graph = *model.mutable_graph();
  graph.set_name("spec220-q8-layout");
  const auto addValue = [] (auto* value, const char* name, std::int64_t columns) {
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    type->mutable_shape()->add_dim()->set_dim_value(1);
    type->mutable_shape()->add_dim()->set_dim_value(columns);
  };
  addValue(graph.add_input(), "input", 35);
  addValue(graph.add_output(), "output", 3);
  auto* node = graph.add_node();
  node->set_domain("com.microsoft");
  node->set_op_type("MatMulNBits");
  for (const char* input : {"input", "B", "scales"}) node->add_input(input);
  node->add_output("output");
  for (const auto& field : std::map<std::string, int>{
         {"K", 35}, {"N", 3}, {"bits", 8}, {"block_size", 32}, {"accuracy_level", 0}}) {
    auto* attr = node->add_attribute();
    attr->set_name(field.first);
    attr->set_type(onnx::AttributeProto::INT);
    attr->set_i(field.second);
  }
  // N columns, two K blocks, 32 bytes per block. Tail lanes dequantize to zero.
  std::vector<std::uint8_t> packed(3 * 2 * 32, 128);
  const std::vector<float> scales{0.125f, 0.25f, 0.5f, 0.125f, 1.0f, 1.0f};
  const auto encodeFloats = [] (const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes;
    for (const float value : values) {
      const auto item = float32Payload({value});
      bytes.insert(bytes.end(), item.begin(), item.end());
    }
    return bytes;
  };
  for (std::size_t n = 0; n < 3; ++n) {
    for (std::size_t k = 0; k < 35; ++k) {
      const int value = n == 0 ? static_cast<int>(k % 9) - 4 :
        (n == 1 ? 2 * (static_cast<int>(k % 7) - 3) : 0);
      packed[n * 64 + k] = static_cast<std::uint8_t>(value + 128);
    }
  }
  auto* weight = graph.add_initializer();
  weight->set_name("B");
  weight->set_data_type(onnx::TensorProto::UINT8);
  for (const int dim : {3, 2, 32}) weight->add_dims(dim);
  weight->set_raw_data(packed.data(), packed.size());
  auto* scale = graph.add_initializer();
  scale->set_name("scales");
  scale->set_data_type(onnx::TensorProto::FLOAT);
  scale->add_dims(3);
  scale->add_dims(2);
  const auto scaleBytes = encodeFloats(scales);
  scale->set_raw_data(scaleBytes.data(), scaleBytes.size());
  NativeModelRunnerSpec spec;
  spec.role = "/NBits/Cpu";
  spec.backend = "onnxruntime-cpu";
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"}, {"outputScope", "raw"}};
  spec.path = fixture.write(model);
  {
    OnnxRuntimeModelRunner runner(spec);
    for (const float factor : {1.0f, -2.0f}) {
      std::vector<float> input(35);
      for (std::size_t k = 0; k < input.size(); ++k)
        input[k] = factor * static_cast<float>(static_cast<int>(k % 5) - 2);
      std::vector<float> expected(3, 0.0f);
      for (std::size_t n = 0; n < 3; ++n)
        for (std::size_t k = 0; k < input.size(); ++k)
          expected[n] += input[k] * (static_cast<int>(packed[n * 64 + k]) - 128) *
            scales[n * 2 + k / 32];
      RoleExecutionContext context;
      context.sessionId = "nbits-cpu";
      context.role = spec.role;
      context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
        {"input", TensorElementType::Float32, {1, 35}, encodeFloats(input)}}));
      const auto result = runner.run(context);
      BOOST_REQUIRE(result.count("raw") == 1U);
      BOOST_REQUIRE_EQUAL(result.at("raw").payload.size(), 3 * sizeof(float));
      std::vector<float> actual(3);
      std::memcpy(actual.data(), result.at("raw").payload.data(), result.at("raw").payload.size());
      for (std::size_t n = 0; n < actual.size(); ++n)
        BOOST_CHECK_SMALL(actual[n] - expected[n], 0.0001f);
    }
  }
  weight->set_dims(2, 31);
  spec.path = fixture.write(model);
  BOOST_CHECK_THROW(OnnxRuntimeModelRunner{spec}, std::exception);
}

BOOST_AUTO_TEST_CASE(ExternalMaterializedWeightsMatchInlineCpuAndRetainDirectory)
{
  GraphMetadataFixture inlineFixture;
  auto pairFixture = std::make_shared<GraphMetadataFixture>();
  const auto pairDirectory = pairFixture->directory;
  std::weak_ptr<GraphMetadataFixture> weakDirectory = pairFixture;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(18);
  auto& graph = *model.mutable_graph();
  const auto addValue = [] (auto* value, const char* name, std::int64_t columns) {
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    type->mutable_shape()->add_dim()->set_dim_value(1);
    type->mutable_shape()->add_dim()->set_dim_value(columns);
  };
  addValue(graph.add_input(), "input", 1024);
  addValue(graph.add_output(), "output", 257);
  auto* node = graph.add_node();
  node->set_op_type("MatMul");
  node->add_input("input");
  node->add_input("weight");
  node->add_output("output");
  const auto floats = [] (std::size_t count, float value) {
    const auto one = float32Payload({value});
    std::vector<std::uint8_t> bytes;
    bytes.reserve(count * one.size());
    for (std::size_t i = 0; i < count; ++i) bytes.insert(bytes.end(), one.begin(), one.end());
    return bytes;
  };
  const auto weights = floats(1024U * 257U, 2.0f / 1024.0f);
  auto* weight = graph.add_initializer();
  weight->set_name("weight");
  weight->set_data_type(onnx::TensorProto::FLOAT);
  weight->add_dims(1024);
  weight->add_dims(257);
  weight->set_raw_data(weights.data(), weights.size());
  NativeModelRunnerSpec spec;
  spec.role = "/FilePair/Cpu";
  spec.backend = "onnxruntime-cpu";
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"}, {"outputScope", "raw"}};
  spec.path = inlineFixture.write(model);
  OnnxRuntimeModelRunner reference(spec);
  NativeCanonicalSource source;
  const auto wire = model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 8U << 20, 16U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  source.materialPayloads = source.materialManifest->payloads;
  std::ofstream sidecar(pairDirectory / "model.onnx.data", std::ios::binary);
  const auto files = materializeNativeCanonicalModelFiles(source, {0}, {}, {}, control,
    [&] (const auto& bytes) {
      sidecar.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      if (!sidecar.good()) throw std::runtime_error("fixture sidecar write failed");
    });
  sidecar.close();
  BOOST_REQUIRE_EQUAL(files.initializerBytes, weights.size());
  BOOST_REQUIRE_LT(files.modelBytes.size(), 4096U);
  spec.path = (pairDirectory / "model.onnx").string();
  {
    std::ofstream output(spec.path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(files.modelBytes.data()), files.modelBytes.size());
    BOOST_REQUIRE(output.good());
  }
  spec.lifetime = pairFixture;
  {
    OnnxRuntimeModelRunner runner(spec);
    spec.lifetime.reset();
    pairFixture.reset();
    BOOST_CHECK(!weakDirectory.expired());
    for (const float input : {1.0f, 2.0f}) {
      RoleExecutionContext context;
      context.sessionId = "external-material-cpu";
      context.role = spec.role;
      context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
        {"input", TensorElementType::Float32, {1, 1024}, floats(1024, input)}}));
      const auto actual = runner.run(context);
      const auto expected = reference.run(context);
      BOOST_REQUIRE(actual.count("raw") == 1U && expected.count("raw") == 1U);
      BOOST_CHECK(actual.at("raw").payload == expected.at("raw").payload);
      BOOST_CHECK(actual.at("raw").payload == floats(257, 2.0f * input));
    }
  }
  BOOST_CHECK(weakDirectory.expired());
  BOOST_CHECK(!std::filesystem::exists(pairDirectory));
}

BOOST_AUTO_TEST_CASE(ShapeConstantsMustRemainModelMaterialDuringWarmup)
{
  GraphMetadataFixture fixture;
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(18);
  auto& graph = *model.mutable_graph();
  const auto addValue = [] (auto* value, const std::string& name,
                            int type, const std::vector<int64_t>& shape) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    for (const auto dimension : shape)
      tensor->mutable_shape()->add_dim()->set_dim_value(dimension);
  };
  addValue(graph.add_input(), "hidden", onnx::TensorProto::FLOAT, {1, 128});
  addValue(graph.add_input(), "target", onnx::TensorProto::INT64, {4});
  addValue(graph.add_output(), "output", onnx::TensorProto::FLOAT, {1, 1, 1, 128});
  auto* reshape = graph.add_node();
  reshape->set_name("boundary-reshape");
  reshape->set_op_type("Reshape");
  reshape->add_input("hidden");
  reshape->add_input("target");
  reshape->add_output("output");
  NativeModelRunnerSpec spec;
  spec.role = "/Warmup/Stage/1";
  spec.backend = "onnxruntime-cpu";
  spec.metadata["forceOutputBundle"] = "true";
  spec.path = fixture.write(model);
  // Reproduce the FP32 split failure independently of CUDA: shape-valid
  // zero activation placeholders are not valid values for shape constants.
  BOOST_CHECK_EXCEPTION(OnnxRuntimeModelRunner{spec}, std::exception,
    [] (const std::exception& error) {
      return std::string(error.what()).find(
        "dimension with value zero exceeds") != std::string::npos;
    });

  graph.mutable_input()->RemoveLast();
  auto* target = graph.add_initializer();
  target->set_name("target");
  target->set_data_type(onnx::TensorProto::INT64);
  target->add_dims(4);
  for (const auto dimension : {1, 1, -1, 128}) target->add_int64_data(dimension);
  spec.path = fixture.write(model);
  OnnxRuntimeModelRunner runner(spec);
  RoleExecutionContext context;
  context.sessionId = "normalized-shape-constant";
  context.role = spec.role;
  context.requestId = "tensor-accounting-request";
  context.providerBootId = "tensor-accounting-boot";
  context.attemptEpoch = 1;
  context.workStatistics = makeNativeWorkStatistics();
  BOOST_REQUIRE(context.workStatistics);
  std::vector<float> values(128, 3.5f);
  std::vector<std::uint8_t> payload(values.size() * sizeof(float));
  std::memcpy(payload.data(), values.data(), payload.size());
  context.inputsByScope.emplace("hidden", makeEncodedTensorBundle(
    "hidden", {NamedTensor{"hidden", TensorElementType::Float32, {1, 128}, payload}}));
  const auto originalInput = context.inputsByScope.at("hidden").payload;
  const auto output = decodeOutput(runner.run(context));
  BOOST_REQUIRE_EQUAL(output.size(), 1);
  BOOST_CHECK(output.front().shape == std::vector<int64_t>({1, 1, 1, 128}));
  BOOST_CHECK(output.front().payload == payload);
  BOOST_CHECK(context.inputsByScope.at("hidden").payload == originalInput);
  values.assign(128, 7.25f);
  std::memcpy(payload.data(), values.data(), payload.size());
  context.inputsByScope.at("hidden") = makeEncodedTensorBundle(
    "hidden", {NamedTensor{"hidden", TensorElementType::Float32, {1, 128}, payload}});
  const auto secondInput = context.inputsByScope.at("hidden").payload;
  ++context.inferenceEpoch;
  const auto secondOutput = decodeOutput(runner.run(context));
  BOOST_REQUIRE_EQUAL(secondOutput.size(), 1U);
  BOOST_CHECK(secondOutput.front().payload == payload);
  BOOST_CHECK(context.inputsByScope.at("hidden").payload == secondInput);
  const auto work = context.workStatistics->snapshot();
  BOOST_REQUIRE(work.complete);
  BOOST_CHECK_EQUAL(work.activeSpans, 0U);
  for (const auto* phase : {"onnx-input-context", "onnx-input-tensor", "onnx-output-convert"}) {
    BOOST_REQUIRE(work.phases.count(phase) == 1U);
    BOOST_CHECK(work.phases.at(phase).kind == NativeWorkKind::LOCAL);
    BOOST_CHECK_EQUAL(work.phases.at(phase).calls, 2U);
  }
  BOOST_REQUIRE(work.phases.count("onnx-run-envelope") == 1U);
  BOOST_CHECK(work.phases.at("onnx-run-envelope").kind == NativeWorkKind::BACKEND);
  BOOST_CHECK_EQUAL(work.phases.at("onnx-run-envelope").calls, 2U);
  BOOST_CHECK_EQUAL(work.counts.at("onnx.outputBorrowedPayloadBytes"), 2U * payload.size());
  BOOST_CHECK_EQUAL(work.counts.at("onnx.outputOrtIntermediateCopyBytes"), 0U);
  const auto metrics = runner.runtimeMetricsSnapshot();
  BOOST_REQUIRE(metrics && metrics->lastBackendExecution);
  BOOST_CHECK(metrics->lastBackendExecution->completed);
  BOOST_CHECK_EQUAL(metrics->lastBackendExecution->inferenceEpoch, context.inferenceEpoch);
  BOOST_CHECK_GE(metrics->lastBackendExecution->finishedSteadyNs,
                 metrics->lastBackendExecution->startedSteadyNs);
  context.inputsByScope.at("hidden").payload.pop_back();
  BOOST_CHECK_THROW(runner.run(context), std::exception);
  const auto failed = context.workStatistics->snapshot();
  BOOST_CHECK(failed.complete);
  BOOST_CHECK_EQUAL(failed.activeSpans, 0U);
  BOOST_CHECK_EQUAL(failed.phases.at("onnx-run-envelope").calls, 2U);
}

BOOST_AUTO_TEST_CASE(BorrowedOrtOutputsKeepAuxiliaryWireAndIndependentLifetime)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  graph.clear_node();
  auto* identity = graph.add_node();
  identity->set_op_type("Identity");
  identity->add_input("input");
  identity->add_output("output");
  *graph.add_output() = graph.output(0);
  graph.mutable_output(1)->set_name("negative");
  auto* negative = graph.add_node();
  negative->set_op_type("Neg");
  negative->add_input("input");
  negative->add_output("negative");
  auto* mask = graph.add_input();
  *mask = graph.input(0);
  mask->set_name("mask");
  mask->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::BOOL);
  NativeModelRunnerSpec spec;
  spec.role = "/Borrowed/Outputs";
  spec.backend = "onnxruntime-cpu";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "input,mask"}, {"outputNames", "output,negative"},
    {"outputAlias.output", "positive"}, {"outputBundleScope", "converted"},
    {"passthroughTensors", "mask"}, {"outputBundlePadBytes", "8"},
    {"outputBundlePadTensor", "padding"}, {"kvOutputTensors", "negative,positive,negative"},
    {"kvOutputScope", "kv"}};
  auto runner = std::make_unique<OnnxRuntimeModelRunner>(spec);
  RoleExecutionContext context;
  context.sessionId = "borrowed-outputs";
  context.role = spec.role;
  context.workStatistics = makeNativeWorkStatistics();
  BOOST_REQUIRE(context.workStatistics);
  context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float32, {2, 2}, float32Payload({1, 2, 3, 4})},
    {"mask", TensorElementType::Bool, {2, 2}, {1, 0, 1, 0}}}));
  const auto inputBefore = context.inputsByScope.at("activation").payload;
  const auto first = runner->run(context);
  const std::vector<NamedTensor> expected{
    {"positive", TensorElementType::Float32, {2, 2}, float32Payload({1, 2, 3, 4})},
    {"negative", TensorElementType::Float32, {2, 2}, float32Payload({-1, -2, -3, -4})},
    {"mask", TensorElementType::Bool, {2, 2}, {1, 0, 1, 0}},
    {"padding", TensorElementType::Float32, {2}, std::vector<std::uint8_t>(8, 0)}};
  BOOST_REQUIRE(first.count("converted") == 1U && first.count("kv") == 1U);
  BOOST_CHECK(first.at("converted").payload == encodeTensorBundle(expected));
  BOOST_CHECK(first.at("kv").payload == encodeTensorBundle({expected[1], expected[0]}));
  BOOST_CHECK(context.inputsByScope.at("activation").payload == inputBefore);
  context.inputsByScope.at("activation") = makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float32, {2, 2}, float32Payload({5, 6, 7, 8})},
    {"mask", TensorElementType::Bool, {2, 2}, {0, 1, 0, 1}}});
  const auto second = runner->run(context);
  auto expectedSecond = expected;
  expectedSecond[0].payload = float32Payload({5, 6, 7, 8});
  expectedSecond[1].payload = float32Payload({-5, -6, -7, -8});
  expectedSecond[2].payload = {0, 1, 0, 1};
  BOOST_CHECK(second.at("converted").payload == encodeTensorBundle(expectedSecond));
  BOOST_CHECK(first.at("converted").payload == encodeTensorBundle(expected));
  const auto measured = context.workStatistics->snapshot();
  BOOST_CHECK_EQUAL(measured.counts.at("onnx.outputBorrowedPayloadBytes"), 64U);
  BOOST_CHECK_EQUAL(measured.counts.at("onnx.outputOrtIntermediateCopyBytes"), 0U);
  BOOST_CHECK_EQUAL(measured.counts.at("onnx.outputAuxiliaryBytes"), 24U);
  BOOST_CHECK(measured.complete);
  BOOST_CHECK_EQUAL(measured.activeSpans, 0U);
  BOOST_CHECK(measured.phases.at("onnx-host-cleanup").kind == NativeWorkKind::LOCAL);
  BOOST_CHECK_EQUAL(measured.phases.at("onnx-host-cleanup").calls, 2U);
  BOOST_CHECK(measured.phases.at("onnx-input-release").kind == NativeWorkKind::UNCLASSIFIED);
  BOOST_CHECK_EQUAL(measured.phases.at("onnx-input-release").calls, 2U);
  auto missing = context;
  missing.inputsByScope.at("activation") = makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float32, {2, 2}, float32Payload({1, 2, 3, 4})}});
  BOOST_CHECK_THROW(runner->run(missing), std::out_of_range);
  runner.reset();
  BOOST_CHECK(first.at("converted").payload == encodeTensorBundle(expected));
  BOOST_CHECK(second.at("converted").payload == encodeTensorBundle(expectedSecond));
}

BOOST_AUTO_TEST_CASE(BorrowedOrtFloat16RawOutputRemainsOwned)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  graph.clear_node();
  graph.mutable_input(0)->mutable_type()->mutable_tensor_type()->set_elem_type(
    onnx::TensorProto::FLOAT16);
  graph.mutable_output(0)->mutable_type()->mutable_tensor_type()->set_elem_type(
    onnx::TensorProto::FLOAT16);
  auto* identity = graph.add_node();
  identity->set_op_type("Identity");
  identity->add_input("input");
  identity->add_output("output");
  NativeModelRunnerSpec spec;
  spec.role = "/Borrowed/Half";
  spec.backend = "onnxruntime-cpu";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"}, {"outputScope", "raw"}};
  auto runner = std::make_unique<OnnxRuntimeModelRunner>(spec);
  RoleExecutionContext context;
  context.sessionId = "borrowed-half";
  context.role = spec.role;
  const std::vector<std::uint8_t> half{0, 60, 0, 64, 0, 66, 0, 68};
  context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float16, {2, 2}, half}}));
  const auto output = runner->run(context);
  runner.reset();
  BOOST_REQUIRE(output.count("raw") == 1U);
  BOOST_CHECK(!isEncodedTensorBundle(output.at("raw").payload));
  BOOST_CHECK(output.at("raw").payload == half);
}

BOOST_AUTO_TEST_CASE(BorrowedOrtContextRetainsCertifiedRedistribution)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  graph.clear_node();
  auto* identity = graph.add_node();
  identity->set_op_type("Identity");
  identity->add_input("input");
  identity->add_output("output");
  NativeModelRunnerSpec spec;
  spec.role = "S1R1";
  spec.backend = "onnxruntime-cpu";
  spec.path = fixture.write(model);
  spec.metadata = {{"inputNames", "input"}, {"outputNames", "output"}, {"outputScope", "raw"}};
  OnnxRuntimeModelRunner runner(spec);
  RoleExecutionContext context;
  context.sessionId = "borrowed-scatter";
  context.role = spec.role;
  context.workStatistics = makeNativeWorkStatistics();
  BOOST_REQUIRE(context.workStatistics);
  context.inputsByScope.emplace("activation", makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float32, {2, 4},
     float32Payload({1, 2, 3, 4, 5, 6, 7, 8})}}));
  const auto original = context.inputsByScope.at("activation").payload;
  RedistributionSpec redistribution;
  redistribution.producerRanks = {0};
  redistribution.consumerRanks = {1, 2};
  redistribution.tensor = "input";
  redistribution.operation = "SCATTER";
  redistribution.axis = 1;
  redistribution.epoch = "epoch-1";
  redistribution.integrityDigest = "sha256:" + std::string(64, 'a');
  redistribution.sourceLayoutDigest = "sha256:" + std::string(64, 'b');
  redistribution.targetLayoutDigest = "sha256:" + std::string(64, 'c');
  redistribution.temporaryMemoryBytes = 32;
  redistribution.completeOutput = true;
  DependencyEdge edge;
  edge.scope = "activation";
  edge.transportScope = "activation";
  edge.producerRole = "S0R0";
  edge.consumerRole = spec.role;
  edge.redistributionProducerRank = 0;
  edge.redistributionConsumerRank = 2;
  edge.redistributions = {redistribution};
  context.inputEdgesByScope.emplace("activation", edge);
  const auto result = runner.run(context);
  BOOST_REQUIRE(result.count("raw") == 1U);
  BOOST_CHECK(result.at("raw").payload == float32Payload({3, 4, 7, 8}));
  BOOST_CHECK(context.inputsByScope.at("activation").payload == original);
  context.inputsByScope.at("activation") = makeEncodedTensorBundle("activation", {
    {"input", TensorElementType::Float32, {2, 4},
     float32Payload({9, 10, 11, 12, 13, 14, 15, 16})}});
  const auto changedInput = context.inputsByScope.at("activation").payload;
  const auto second = runner.run(context);
  BOOST_CHECK(second.at("raw").payload == float32Payload({11, 12, 15, 16}));
  BOOST_CHECK(result.at("raw").payload == float32Payload({3, 4, 7, 8}));
  BOOST_CHECK(context.inputsByScope.at("activation").payload == changedInput);
  const auto measured = context.workStatistics->snapshot();
  BOOST_CHECK(measured.complete);
  BOOST_CHECK_EQUAL(measured.activeSpans, 0U);
  BOOST_CHECK_EQUAL(measured.phases.at("onnx-host-cleanup").calls, 2U);
}

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

BOOST_AUTO_TEST_CASE(ExplicitBackendSelectsProviderWithoutDuplicateMetadata)
{
  NativeModelRunnerSpec spec;
  spec.backend = "onnxruntime-cuda";
  const std::vector<std::string> available{"CUDAExecutionProvider", "CPUExecutionProvider"};
  const auto cuda = resolveOnnxRuntimeProviderSelection(spec, available);
  BOOST_CHECK_EQUAL(cuda.requestedProvider, "cuda");
  BOOST_CHECK_EQUAL(cuda.selectedProvider, "cuda");
  BOOST_CHECK_EQUAL(cuda.deviceId, "0");
  BOOST_CHECK(!cuda.usedCpuFallback);
  BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, {"CPUExecutionProvider"}),
                    std::runtime_error);
  spec.backend = "onnxruntime-cpu";
  const auto cpu = resolveOnnxRuntimeProviderSelection(spec, available);
  BOOST_CHECK_EQUAL(cpu.selectedProvider, "cpu");
  BOOST_CHECK_EQUAL(cpu.deviceId, "cpu0");
  BOOST_CHECK(!cpu.usedCpuFallback);
}

BOOST_AUTO_TEST_CASE(ExplicitBackendRejectsConflictingProviderMetadata)
{
  const std::vector<std::string> available{"CUDAExecutionProvider", "CPUExecutionProvider"};
  for (const auto* key : {"executionProvider", "execution_provider", "device.kind", "deviceKind"}) {
    NativeModelRunnerSpec spec;
    spec.backend = "onnxruntime-cuda";
    spec.metadata[key] = "CPUExecutionProvider";
    BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, available), std::invalid_argument);
    spec.backend = "onnxruntime-cpu";
    spec.metadata[key] = "CUDAExecutionProvider";
    BOOST_CHECK_THROW(resolveOnnxRuntimeProviderSelection(spec, available), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(GenericBackendRetainsMetadataSelectionAndCpuDefault)
{
  NativeModelRunnerSpec spec;
  spec.backend = "onnxruntime";
  const std::vector<std::string> available{"CUDAExecutionProvider", "CPUExecutionProvider"};
  BOOST_CHECK_EQUAL(resolveOnnxRuntimeProviderSelection(spec, available).selectedProvider, "cpu");
  spec.metadata["executionProvider"] = "CUDAExecutionProvider";
  BOOST_CHECK_EQUAL(resolveOnnxRuntimeProviderSelection(spec, available).selectedProvider, "cuda");
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

BOOST_AUTO_TEST_CASE(GraphMetadataComesFromStructuralUsesAndFailsClosed)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_REQUIRE_EQUAL(inventory.hostMetadataNodes.size(), 2);
  BOOST_CHECK_EQUAL(inventory.hostMetadataNodes.at("shape"), "Shape");
  BOOST_CHECK_EQUAL(inventory.hostMetadataNodes.at("shape-cast"), "Cast");
  BOOST_REQUIRE_EQUAL(inventory.modelNodes.size(), 1);
  BOOST_CHECK_EQUAL(inventory.modelNodes.at("reshape"), "Reshape");
  BOOST_CHECK_EQUAL(inventory.optionalModelNodes.at("identity"), "Identity");
  BOOST_CHECK_NO_THROW(detail::verifyOnnxRuntimeGraphMetadataSource(
    (fixture.directory / "model.onnx").string(), inventory));
  const auto original = inventory;

  // A shape value exposed as application output is no longer private geometry.
  auto* output = model.mutable_graph()->add_output();
  output->set_name("dims");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::INT64);
  inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_CHECK_THROW(detail::verifyOnnxRuntimeGraphMetadataSource(
    (fixture.directory / "model.onnx").string(), original), std::runtime_error);
  BOOST_CHECK(inventory.hostMetadataNodes.count("shape") == 0);
  BOOST_CHECK(inventory.modelNodes.count("shape") == 1);

  // Integer model inputs do not become host metadata based on dtype alone.
  model = makeShapeMetadataModel();
  model.mutable_graph()->mutable_input(0)->mutable_type()->mutable_tensor_type()->
    set_elem_type(onnx::TensorProto::INT64);
  auto* shape = model.mutable_graph()->mutable_node(0);
  shape->set_op_type("Add"); shape->add_input("input");
  inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_CHECK(inventory.hostMetadataNodes.empty());

  // A dimension can scale real position/token values without the dimension
  // Gather becoming model data. The payload multiplication still needs CUDA.
  model = makeShapeMetadataModel();
  model.mutable_graph()->mutable_input(0)->mutable_type()->mutable_tensor_type()->
    set_elem_type(onnx::TensorProto::INT64);
  model.mutable_graph()->mutable_output(0)->mutable_type()->mutable_tensor_type()->
    set_elem_type(onnx::TensorProto::INT64);
  model.mutable_graph()->mutable_node(2)->set_op_type("Mul");
  inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_CHECK_EQUAL(inventory.hostMetadataNodes.at("shape"), "Shape");
  BOOST_CHECK_EQUAL(inventory.hostMetadataNodes.at("shape-cast"), "Cast");
  BOOST_CHECK_EQUAL(inventory.modelNodes.at("reshape"), "Mul");

  model = makeShapeMetadataModel();
  model.mutable_graph()->mutable_node(1)->set_name("shape");
  BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model)).hostMetadataNodes.empty());
  model = makeShapeMetadataModel();
  auto* nested = model.mutable_graph()->mutable_node(0)->add_attribute();
  nested->set_name("body"); nested->set_type(onnx::AttributeProto::GRAPH);
  nested->mutable_g();
  BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model)).hostMetadataNodes.empty());
  std::ofstream(fixture.directory / "model.onnx", std::ios::binary) << "broken";
  BOOST_CHECK(detail::inspectOnnxRuntimeGraphMetadata(
    (fixture.directory / "model.onnx").string()).hostMetadataNodes.empty());
}

BOOST_AUTO_TEST_CASE(GraphProfileRequiresCudaModelCoverageAndBoundedMetadata)
{
  GraphMetadataFixture fixture;
  const auto inventory = detail::inspectOnnxRuntimeGraphMetadata(
    fixture.write(makeShapeMetadataModel()));
  BOOST_REQUIRE(!inventory.hostMetadataNodes.empty());
  const std::string shape = R"json({"cat":"Node","name":"shape_kernel_time","args":{
    "provider":"CPUExecutionProvider","op_name":"Shape",
    "input_type_shape":[{"float":[2,2]}],"output_type_shape":[{"int64":[2]}]}})json";
  const std::string reshape = R"json({"cat":"Node","name":"reshape_kernel_time","args":{
    "provider":"CUDAExecutionProvider","op_name":"Reshape"}})json";
  ExecutionEvidence evidence;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  auto apply = [&] (const std::string& json) {
    applyOnnxRuntimeProviderProfile(evidence, fixture.profile(json), "role", false, "GPU-one",
      inventory.hostMetadataNodes, inventory.modelNodes, inventory.optionalModelNodes);
  };
  apply("[" + shape + "," + reshape + "]");
  BOOST_REQUIRE_EQUAL(evidence.getNodeProviderAssignments().size(), 2);
  BOOST_CHECK(!evidence.getNodeProviderAssignments().front().modelNode);
  BOOST_CHECK(evidence.getNodeProviderAssignments().back().modelNode);
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-one");
  BOOST_CHECK_THROW(apply("[" + shape + "]"), std::runtime_error);
  BOOST_CHECK_THROW(apply("[" + reshape + R"json(,
    {"cat":"Node","name":"shape_kernel_time","args":{"provider":"CPUExecutionProvider",
    "op_name":"Shape","output_type_shape":[{"int64":[65]}]}}])json"), std::runtime_error);
  BOOST_CHECK_THROW(apply("[" + shape + R"json(,
    {"cat":"Node","name":"reshape_kernel_time","args":{"provider":"CPUExecutionProvider",
    "op_name":"Reshape"}}])json"), std::runtime_error);
  BOOST_CHECK_THROW(apply("[" + reshape + R"json(,
    {"cat":"Node","name":"unknown_kernel_time","args":{"provider":"CPUExecutionProvider",
    "op_name":"Gather","input_type_shape":[{"int64":[1]}],
    "output_type_shape":[{"int64":[1]}]}}])json"), std::runtime_error);
  BOOST_CHECK_THROW(apply("[" + reshape + R"json(,
    {"cat":"Node","name":"shape_kernel_time","args":{"op_name":"Shape"}}])json"),
    std::runtime_error);
  BOOST_REQUIRE_EQUAL(evidence.getNodeProviderAssignments().size(), 2);
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-one");
  BOOST_CHECK(!evidence.cpuFallbackUsed);
}

BOOST_AUTO_TEST_CASE(GraphMetadataSkipsBulkWeightsAndRejectsOversizedNodeMessages)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto* weight = model.mutable_graph()->add_initializer();
  weight->set_name("unused-large-weight");
  weight->set_data_type(onnx::TensorProto::FLOAT);
  weight->add_dims(1024); weight->add_dims(1024);
  weight->set_raw_data(std::string(1024 * 1024 * sizeof(float), '\0'));
  auto inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_REQUIRE_EQUAL(inventory.hostMetadataNodes.size(), 2);
  BOOST_CHECK_EQUAL(inventory.modelNodes.at("reshape"), "Reshape");
  auto* oversized = model.mutable_graph()->add_node();
  oversized->set_name("oversized-constant"); oversized->set_op_type("Constant");
  oversized->add_output("unused-constant");
  auto* value = oversized->add_attribute();
  value->set_name("value"); value->set_type(onnx::AttributeProto::TENSOR);
  *value->mutable_t() = *weight;
  inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_CHECK(inventory.hostMetadataNodes.empty());
  BOOST_CHECK(inventory.modelNodes.empty());
}

BOOST_AUTO_TEST_CASE(PortableGraphProfileMatchesFrozenCudaRun)
{
  const auto* modelPath = std::getenv("NDNSF_TEST_PORTABLE_ONNX_GRAPH");
  const auto* profilePath = std::getenv("NDNSF_TEST_PORTABLE_CUDA_PROFILE");
  if (!modelPath || !profilePath) {
    BOOST_TEST_MESSAGE("Frozen portable graph/profile check not requested");
    return;
  }
  const auto inventory = detail::inspectOnnxRuntimeGraphMetadata(modelPath);
  BOOST_REQUIRE(!inventory.hostMetadataNodes.empty());
  BOOST_REQUIRE(!inventory.modelNodes.empty());
  BOOST_TEST_MESSAGE("Frozen graph inventory: metadata=" << inventory.hostMetadataNodes.size()
    << ", required-model=" << inventory.modelNodes.size()
    << ", optional-model=" << inventory.optionalModelNodes.size()
    << ", cast-aliases=" << inventory.modelNodeAliases.size());
  ExecutionEvidence evidence;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  applyOnnxRuntimeProviderProfile(evidence, profilePath, "frozen-role", false, "frozen-GPU",
    inventory.hostMetadataNodes, inventory.modelNodes, inventory.optionalModelNodes,
    inventory.modelNodeAliases);
  BOOST_CHECK(!evidence.cpuFallbackUsed);
  BOOST_CHECK_GT(evidence.getNodeProviderAssignments().size(), 1000);
}

BOOST_AUTO_TEST_CASE(GraphProfileAcceptsOnlyProvenCudaCastChainAliases)
{
  GraphMetadataFixture fixture;
  auto model = makeShapeMetadataModel();
  auto& graph = *model.mutable_graph();
  auto identity = graph.node(3);
  graph.mutable_node()->RemoveLast();
  auto addCast = [&] (const std::string& name, const std::string& input,
                     const std::string& output) {
    auto* node = graph.add_node();
    node->set_name(name); node->set_op_type("Cast");
    node->add_input(input); node->add_output(output);
    auto* to = node->add_attribute();
    to->set_name("to"); to->set_type(onnx::AttributeProto::INT);
    to->set_i(onnx::TensorProto::INT64);
  };
  addCast("cast-first", "reshaped", "integer");
  addCast("cast-last", "integer", "integer-final");
  identity.set_input(0, "integer-final");
  *graph.add_node() = identity;
  graph.mutable_output(0)->mutable_type()->mutable_tensor_type()->
    set_elem_type(onnx::TensorProto::INT64);
  const auto inventory = detail::inspectOnnxRuntimeGraphMetadata(fixture.write(model));
  BOOST_REQUIRE_EQUAL(inventory.modelNodeAliases.size(), 1);
  BOOST_CHECK_EQUAL(inventory.modelNodeAliases.at("cast-last"), "cast-first");
  BOOST_CHECK(inventory.modelNodes.count("cast-first") == 1);
  ExecutionEvidence evidence;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
  auto apply = [&] (const std::string& json) {
    applyOnnxRuntimeProviderProfile(evidence, fixture.profile(json), "role", false, "GPU-cast",
      inventory.hostMetadataNodes, inventory.modelNodes, inventory.optionalModelNodes,
      inventory.modelNodeAliases);
  };
  const std::string reshape = R"json({"cat":"Node","name":"reshape_kernel_time",
    "args":{"provider":"CUDAExecutionProvider","op_name":"Reshape"}})json";
  const std::string cast = R"json({"cat":"Node","name":"cast-last_kernel_time",
    "args":{"provider":"CUDAExecutionProvider","op_name":"Cast"}})json";
  BOOST_CHECK_NO_THROW(apply("[" + reshape + "," + cast + "]"));
  BOOST_CHECK_THROW(apply("[" + reshape + "]"), std::runtime_error);
  BOOST_CHECK_THROW(apply("[" + reshape + R"json(,
    {"cat":"Node","name":"cast-last_kernel_time",
     "args":{"provider":"CPUExecutionProvider","op_name":"Cast"}}])json"),
    std::runtime_error);
  BOOST_CHECK_EQUAL(evidence.gpuUuid, "GPU-cast");
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

BOOST_AUTO_TEST_CASE(WorkerBindsReusedRunnerEvidenceToCurrentPlan)
{
  const std::string roleName = "/Evidence/Role";
  const std::string planA = "sha256:" + std::string(64, 'a');
  const std::string planB = "sha256:" + std::string(64, 'b');
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/A";
  evidence.providerBootId = "boot-a";
  evidence.evidenceEpoch = 1;
  evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
  evidence.realCompute = true;
  evidence.deviceKind = "cpu";
  evidence.deviceId = "cpu-0";
  evidence.runtimeVersion = "fixture";
  evidence.modelDigest = "sha256:model";
  evidence.planDigest = planA;
  evidence.artifactDigests[roleName] = "sha256:artifact";
  evidence.roles = {roleName};
  evidence.loadCompleted = true;
  evidence.warmupCompleted = true;
  evidence.createdAtMs = 1;
  evidence.providerProfilePath = "/profile/first";
  evidence.profileRequestId = "/request/A";
  evidence.profileAttemptEpoch = 1;
  evidence.setNodeProviderAssignments({
    {roleName, "/model/MatMul", "CPUExecutionProvider", true}});
  evidence.validate();

  const auto compute = [](const RoleExecutionContext&) {
    return std::map<std::string, TensorBundle>{{"output", TensorBundle{"output", {42}}}};
  };
  auto runner = std::make_shared<LambdaModelRunner>(compute, evidence);
  NativeProviderRuntime runtime(1);
  runtime.registerRunner(roleName, runner);
  auto io = std::make_shared<NoTimingDependencyIo>();
  RoleSpec role(roleName, {}, {}, "/request/A", 1);
  GenerationEpochLineageV1 lineage;
  lineage.requestId = role.requestId;
  lineage.attemptEpoch = role.attemptEpoch;
  lineage.planDigest = planA;
  lineage.generationId = "0123456789abcdef0123456789abcdef";
  lineage.streamEpoch = 1;
  lineage.logicalPrefixDigest = "sha256:" + std::string(64, '2');
  lineage.positionDigest = "sha256:" + std::string(64, '3');
  lineage.producerRole = roleName;
  lineage.consumerRole = roleName;
  role.generationLineage = lineage;
  const auto first = runtime.executeRoleAsync("evidence-session", role, io).get();
  role.requestId = "/request/B";
  role.attemptEpoch = 2;
  role.generationLineage->requestId = role.requestId;
  role.generationLineage->attemptEpoch = role.attemptEpoch;
  role.generationLineage->planDigest = planB;
  const auto second = runtime.executeRoleAsync("evidence-session", role, io).get();
  BOOST_REQUIRE(first.executionEvidence);
  BOOST_REQUIRE(second.executionEvidence);
  BOOST_CHECK_EQUAL(first.executionEvidence->planDigest, planA);
  BOOST_CHECK_EQUAL(first.executionEvidence->requestId, "/request/A");
  BOOST_CHECK_EQUAL(first.executionEvidence->attemptEpoch, 1);
  BOOST_CHECK_EQUAL(second.executionEvidence->planDigest, planB);
  BOOST_CHECK_EQUAL(second.executionEvidence->requestId, "/request/B");
  BOOST_CHECK_EQUAL(second.executionEvidence->attemptEpoch, 2);
  BOOST_CHECK(second.executionEvidence->executionCompleted);
  BOOST_CHECK(!second.exactForwardCacheHit);
  BOOST_CHECK_EQUAL(second.executionEvidence->providerProfilePath, evidence.providerProfilePath);
  BOOST_CHECK_EQUAL(second.executionEvidence->profileRequestId, evidence.profileRequestId);
  BOOST_CHECK_EQUAL(second.executionEvidence->profileAttemptEpoch, evidence.profileAttemptEpoch);
  BOOST_CHECK(&second.executionEvidence->getNodeProviderAssignments() ==
              &evidence.getNodeProviderAssignments());
  BOOST_CHECK_EQUAL(runner->executionEvidenceSnapshot()->planDigest, planA);
  BOOST_CHECK(runner->executionEvidenceSnapshot()->requestId.empty());

  role.generationLineage.reset();
  const auto legacy = runtime.executeRoleAsync("legacy-session", role, io).get();
  BOOST_REQUIRE(legacy.executionEvidence);
  BOOST_CHECK_EQUAL(legacy.executionEvidence->planDigest, planA);
  runtime.registerRunner("/NoEvidence/Role", std::make_shared<LambdaModelRunner>(compute));
  RoleSpec noEvidence("/NoEvidence/Role", {}, {}, "/request/no-evidence", 1);
  const auto absent = runtime.executeRoleAsync("no-evidence-session", noEvidence, io).get();
  BOOST_CHECK(!absent.executionEvidence);
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

BOOST_AUTO_TEST_CASE(PortableCpuStateContinuesAfterSourceRelease)
{
  const auto fixture = findSpec175Fixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(), "stateful ONNX fixture required");
  const std::string role = "/LLM/Pipeline/Stage/0";
  auto sourceSpec = makeStatefulSpec(fixture / "role-0.onnx", role);
  sourceSpec.metadata["providerIdentity"] = "/spec221/source";
  sourceSpec.metadata["providerBootId"] = "source-boot";
  auto destinationSpec = sourceSpec;
  destinationSpec.metadata["providerIdentity"] = "/spec221/destination";
  destinationSpec.metadata["providerBootId"] = "destination-boot";
  OnnxRuntimeModelRunner source(sourceSpec);
  OnnxRuntimeModelRunner destination(destinationSpec);
  OnnxRuntimeModelRunner baseline(sourceSpec);
  BOOST_REQUIRE(source.supportsPortableConversationState());
  BOOST_REQUIRE(destination.supportsPortableConversationState());
  RoleExecutionContext context;
  context.requestId = "/spec221/source-request";
  context.sessionId = "source-session";
  context.providerBootId = "source-boot";
  context.role = role;
  context.attemptEpoch = 1;
  context.streamingStateExecution = true;
  auto reference = context;
  reference.sessionId = "reference-session";
  for (std::uint64_t epoch = 0; epoch < 3; ++epoch) {
    context.inferenceEpoch = reference.inferenceEpoch = epoch;
    context.inputsByScope = {{"input_ids", makeInputIds(3 + epoch)}};
    reference.inputsByScope = context.inputsByScope;
    source.run(context);
    baseline.run(reference);
  }
  const auto handle = source.promoteSessionStateToConversation(
    context.sessionId, "source-conversation");
  BOOST_REQUIRE(handle);
  const auto digest = "sha256:" + std::string(64, 'a');
  DecodeStateIdentityV1 origin;
  origin.modelDigest = origin.graphSemanticDigest = origin.artifactDigest = digest;
  origin.adapterDigest = origin.tokenizerDigest = origin.runnerDigest = digest;
  origin.roleName = role;
  origin.roleSplitDigest = origin.prefixDigest = origin.positionDigest = digest;
  origin.layerEnd = 1;
  origin.prefixTokenCount = 3;
  origin.precision = "float32";
  origin.layoutDigest = origin.stateSchemaDigest = origin.runtimeAbiDigest = digest;
  origin.stateComponentDigests = {digest};
  origin.securityDomainDigest = digest;
  origin.providerIdentity = "/spec221/source";
  origin.providerBootId = "source-boot";
  origin.stateInferenceEpoch = handle->opaque.stateInferenceEpoch;
  origin.predecessorInferenceEpoch = origin.stateInferenceEpoch - 1;
  origin.requestId = context.requestId;
  origin.generationId = "source-generation";
  const NativePortableKvLimitsV1 limits{1U << 20, 2U << 20, {}};
  const auto snapshot = source.exportConversationState(*handle, origin, limits);
  BOOST_REQUIRE_EQUAL(snapshot.components.size(), 3U);
  BOOST_CHECK_EQUAL(snapshot.logicalBytes, handle->logicalBytes);
  auto small = limits;
  small.maxPayloadBytes = snapshot.logicalBytes - 1;
  BOOST_CHECK_THROW(source.exportConversationState(*handle, origin, small),
                    std::invalid_argument);
  auto target = origin;
  target.providerIdentity = "/spec221/destination";
  target.providerBootId = "destination-boot";
  target.requestId = "/spec221/destination-request";
  target.generationId = "destination-generation";
  target.cacheEpoch = 7;
  auto wrongIo = snapshot;
  wrongIo.stateIoDigest = "sha256:" + std::string(64, 'b');
  BOOST_CHECK_THROW(destination.importConversationState(wrongIo, target, "destination", limits),
                    std::invalid_argument);
  auto wrongOrder = snapshot;
  std::swap(wrongOrder.components[0], wrongOrder.components[1]);
  BOOST_CHECK_THROW(destination.importConversationState(wrongOrder, target, "destination", limits),
                    std::invalid_argument);
  for (const auto& shape : {std::vector<std::int64_t>{1, 16},
                           std::vector<std::int64_t>{16}}) {
    auto wrongShape = snapshot;
    auto& component = wrongShape.components.front();
    const auto tensor = decodeTensorBundle(component.tensorWire).front();
    component.shape = shape;
    component.tensorWire = encodeTensorBundle({
      NamedTensor{component.inputName, component.elementType, shape, tensor.payload}});
    component.contentDigest = sha256TensorBytes(component.tensorWire);
    wrongShape.encodedBytes = 0;
    for (const auto& value : wrongShape.components) {
      wrongShape.encodedBytes += value.tensorWire.size();
    }
    BOOST_CHECK_THROW(destination.importConversationState(wrongShape, target, "destination", limits),
                      std::invalid_argument);
  }
  auto wrongType = snapshot;
  auto& typedComponent = wrongType.components.front();
  const auto typedPayload = decodeTensorBundle(typedComponent.tensorWire).front().payload;
  typedComponent.elementType = TensorElementType::Int32;
  typedComponent.tensorWire = encodeTensorBundle({NamedTensor{
    typedComponent.inputName, typedComponent.elementType, typedComponent.shape, typedPayload}});
  typedComponent.contentDigest = sha256TensorBytes(typedComponent.tensorWire);
  BOOST_CHECK_THROW(destination.importConversationState(wrongType, target, "destination", limits),
                    std::invalid_argument);
  auto incompatible = target;
  incompatible.modelDigest = "sha256:" + std::string(64, 'b');
  BOOST_CHECK_THROW(destination.importConversationState(snapshot, incompatible, "destination", limits),
                    std::invalid_argument);
  auto corrupt = snapshot;
  corrupt.components.front().tensorWire.back() ^= 1;
  BOOST_CHECK_THROW(destination.importConversationState(corrupt, target, "destination", limits),
                    std::invalid_argument);
  // Sweep the bounded cancellation checkpoints, including allocation/copy,
  // without depending on their exact count. Every failed import must permit
  // an immediate retry under the same key and leave no published state.
  unsigned cancelledAttempts = 0;
  bool reachedUncancelledImport = false;
  for (unsigned threshold = 1; threshold <= 64; ++threshold) {
    unsigned checks = 0;
    auto cancelled = limits;
    cancelled.cancelled = [&checks, threshold] { return ++checks >= threshold; };
    try {
      const auto state = destination.importConversationState(
        snapshot, target, "destination", cancelled);
      BOOST_REQUIRE(destination.releaseConversationState(state));
      reachedUncancelledImport = true;
      break;
    }
    catch (const std::runtime_error& error) {
      BOOST_CHECK(std::string(error.what()).find("cancel") != std::string::npos);
      const auto retry = destination.importConversationState(
        snapshot, target, "destination", limits);
      BOOST_REQUIRE(destination.releaseConversationState(retry));
      ++cancelledAttempts;
    }
  }
  BOOST_REQUIRE(reachedUncancelledImport);
  BOOST_CHECK_GT(cancelledAttempts, snapshot.components.size());
  const auto imported = destination.importConversationState(snapshot, target, "destination", limits);
  BOOST_CHECK_THROW(destination.importConversationState(snapshot, target, "destination", limits),
                    std::runtime_error);
  BOOST_REQUIRE(source.releaseConversationState(*handle));
  source.releaseSessionState(context.sessionId);
  BOOST_REQUIRE(destination.restoreConversationState(imported, "destination-session"));
  auto continued = context;
  continued.requestId = target.requestId;
  continued.providerBootId = target.providerBootId;
  continued.sessionId = "destination-session";
  continued.inferenceEpoch = 0;
  continued.inputsByScope = {{"input_ids", makeInputIds(9)}};
  reference.inferenceEpoch = 3;
  reference.inputsByScope = continued.inputsByScope;
  const auto actual = decodeOutput(destination.run(continued));
  const auto expected = decodeOutput(baseline.run(reference));
  BOOST_CHECK(findTensor(actual, "hidden_out").payload ==
              findTensor(expected, "hidden_out").payload);
  BOOST_CHECK_GT(destination.runtimeMetricsSnapshot()->stateInputHits, 0U);
  destination.releaseSessionState(continued.sessionId);
  baseline.releaseSessionState(reference.sessionId);
  BOOST_CHECK(destination.releaseConversationState(imported));
  BOOST_CHECK(!destination.releaseConversationState(imported));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
