// Backend capability probe, not an NDNSF or complete-model qualification.
// Contracts: ORT v1.31.1 docs/ContribOperators.md and contrib_ops/{cpu,cuda}/bert/.
#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int64_t CHANNELS = 4;
constexpr int64_t KERNEL = 4;
constexpr int64_t HEAD_DIM = 128;

double
milliseconds(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double
cpuMilliseconds(std::clock_t start)
{
  return 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
}

void
declareValue(onnx::GraphProto& graph, const std::string& name, int type,
             const std::vector<int64_t>& shape, bool output = false)
{
  auto* value = output ? graph.add_output() : graph.add_input();
  value->set_name(name);
  auto* tensor = value->mutable_type()->mutable_tensor_type();
  tensor->set_elem_type(type);
  for (const auto extent : shape) {
    auto* dim = tensor->mutable_shape()->add_dim();
    if (extent < 0) {
      dim->set_dim_param("tokens");
    }
    else {
      dim->set_dim_value(extent);
    }
  }
}

void
integerAttribute(onnx::NodeProto& node, const std::string& name, int64_t value)
{
  auto* attribute = node.add_attribute();
  attribute->set_name(name);
  attribute->set_type(onnx::AttributeProto::INT);
  attribute->set_i(value);
}

std::string
makeModel(bool linear, int type)
{
  onnx::ModelProto model;
  model.set_ir_version(10);
  model.add_opset_import()->set_version(13);
  auto* custom = model.add_opset_import();
  custom->set_domain("com.microsoft");
  custom->set_version(1);
  auto& graph = *model.mutable_graph();
  graph.set_name("required-recurrent-operator-probe");
  auto& node = *graph.add_node();
  node.set_domain("com.microsoft");
  node.set_op_type(linear ? "LinearAttention" : "CausalConvWithState");
  if (linear) {
    for (const auto* name : {"query", "key", "value"}) {
      declareValue(graph, name, type, {1, -1, HEAD_DIM});
      node.add_input(name);
    }
    declareValue(graph, "past_state", type, {1, 1, HEAD_DIM, HEAD_DIM});
    node.add_input("past_state");
    for (const auto* name : {"decay", "beta"}) {
      declareValue(graph, name, type, {1, -1, 1});
      node.add_input(name);
    }
    integerAttribute(node, "q_num_heads", 1);
    integerAttribute(node, "kv_num_heads", 1);
    auto* scale = node.add_attribute();
    scale->set_name("scale");
    scale->set_type(onnx::AttributeProto::FLOAT);
    scale->set_f(1.0f);
    auto* rule = node.add_attribute();
    rule->set_name("update_rule");
    rule->set_type(onnx::AttributeProto::STRING);
    rule->set_s("gated_delta");
    declareValue(graph, "output", type, {1, -1, HEAD_DIM}, true);
    declareValue(graph, "present_state", type, {1, 1, HEAD_DIM, HEAD_DIM}, true);
  }
  else {
    declareValue(graph, "input", type, {1, -1, CHANNELS});
    declareValue(graph, "weight", type, {CHANNELS, 1, KERNEL});
    declareValue(graph, "past_state", type, {1, KERNEL - 1, CHANNELS});
    for (const auto* name : {"input", "weight", "", "past_state"}) {
      node.add_input(name);
    }
    integerAttribute(node, "channels_last", 1);
    integerAttribute(node, "ndim", 1);
    declareValue(graph, "output", type, {1, -1, CHANNELS}, true);
    declareValue(graph, "present_state", type, {1, KERNEL - 1, CHANNELS}, true);
  }
  node.add_output("output");
  node.add_output("present_state");
  std::string bytes;
  if (!model.SerializeToString(&bytes)) {
    throw std::runtime_error("PROBE_MODEL_SERIALIZATION_FAILED");
  }
  return bytes;
}

template<typename T>
float
checkOutput(const Ort::Value& output, const std::vector<int64_t>& shape,
            const std::vector<float>& expected)
{
  const auto info = output.GetTensorTypeAndShapeInfo();
  const auto type = sizeof(T) == 2 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16
                                 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  if (output.GetTensorMemoryInfo().GetDeviceType() != OrtMemoryInfoDeviceType_CPU ||
      info.GetShape() != shape || info.GetElementCount() != expected.size() ||
      info.GetElementType() != type) {
    throw std::runtime_error("PROBE_OUTPUT_SHAPE_OR_TYPE_MISMATCH");
  }
  const auto* actual = output.GetTensorData<T>();
  float maximum = 0.0f;
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto value = static_cast<float>(actual[i]);
    const auto difference = std::abs(value - expected[i]);
    if (!std::isfinite(value) || difference > 0.001f) {
      throw std::runtime_error("PROBE_OUTPUT_OR_STATE_VALUE_MISMATCH");
    }
    maximum = std::max(maximum, difference);
  }
  return maximum;
}

template<typename T>
void
runOperator(Ort::Env& environment, Ort::SessionOptions& options, bool linear,
            const std::string& backend)
{
  const auto type = sizeof(T) == 2 ? onnx::TensorProto::FLOAT16 : onnx::TensorProto::FLOAT;
  const auto model = makeModel(linear, type);
  const auto sessionStart = Clock::now();
  const auto sessionCpu = std::clock();
  Ort::Session session(environment, model.data(), model.size(), options);
  const auto sessionWallMs = milliseconds(sessionStart);
  const auto sessionCpuMs = cpuMilliseconds(sessionCpu);
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::vector<int64_t> stateShape = linear
    ? std::vector<int64_t>{1, 1, HEAD_DIM, HEAD_DIM}
    : std::vector<int64_t>{1, KERNEL - 1, CHANNELS};
  const auto stateSize = std::accumulate(stateShape.begin(), stateShape.end(), int64_t{1},
                                       std::multiplies<int64_t>());
  std::vector<T> state(static_cast<size_t>(stateSize), T{0.0f});
  const std::vector<const char*> names = linear
    ? std::vector<const char*>{"query", "key", "value", "past_state", "decay", "beta"}
    : std::vector<const char*>{"input", "weight", "past_state"};
  const char* outputNames[] = {"output", "present_state"};
  for (int round = 0; round < 2; ++round) {
    const int64_t tokens = round == 0 ? 3 : 1;
    const int64_t offset = round == 0 ? 0 : 3;
    const auto width = linear ? HEAD_DIM : CHANNELS;
    std::vector<std::vector<T>> values;
    std::vector<std::vector<int64_t>> shapes;
    if (linear) {
      std::vector<T> query(static_cast<size_t>(tokens * HEAD_DIM), T{0.0f});
      auto key = query;
      auto value = query;
      for (int64_t t = 0; t < tokens; ++t) {
        query[static_cast<size_t>(t * HEAD_DIM)] = T{1.0f};
        key[static_cast<size_t>(t * HEAD_DIM)] = T{1.0f};
        for (int64_t j = 0; j < HEAD_DIM; ++j) {
          value[static_cast<size_t>(t * HEAD_DIM + j)] =
            static_cast<T>(static_cast<float>((offset + t + 1) * (j % 4 + 1)));
        }
      }
      values = {std::move(query), std::move(key), std::move(value), state,
                std::vector<T>(static_cast<size_t>(tokens), T{0.0f}),
                std::vector<T>(static_cast<size_t>(tokens), T{0.5f})};
      shapes = {{1, tokens, HEAD_DIM}, {1, tokens, HEAD_DIM}, {1, tokens, HEAD_DIM},
                stateShape, {1, tokens, 1}, {1, tokens, 1}};
    }
    else {
      std::vector<T> input(static_cast<size_t>(tokens * CHANNELS));
      for (int64_t t = 0; t < tokens; ++t) {
        for (int64_t j = 0; j < CHANNELS; ++j) {
          input[static_cast<size_t>(t * CHANNELS + j)] =
            static_cast<T>(static_cast<float>((offset + t + 1) * (j + 1)));
        }
      }
      values = {std::move(input), std::vector<T>(CHANNELS * KERNEL, T{1.0f}), state};
      shapes = {{1, tokens, CHANNELS}, {CHANNELS, 1, KERNEL}, stateShape};
    }
    std::vector<Ort::Value> inputs;
    size_t inputBytes = 0;
    for (size_t i = 0; i < values.size(); ++i) {
      inputBytes += values[i].size() * sizeof(T);
      inputs.push_back(Ort::Value::CreateTensor<T>(memory, values[i].data(), values[i].size(),
                                                   shapes[i].data(), shapes[i].size()));
    }
    const auto runStart = Clock::now();
    const auto runCpu = std::clock();
    auto result = session.Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(),
                              outputNames, 2);
    const auto runWallMs = milliseconds(runStart);
    const auto runCpuMs = cpuMilliseconds(runCpu);
    if (result.size() != 2) {
      throw std::runtime_error("PROBE_OUTPUT_COUNT_MISMATCH");
    }
    std::vector<float> expectedOutput(static_cast<size_t>(tokens * width));
    float coefficient = round == 0 ? 0.0f : 2.125f;
    for (int64_t t = 0; t < tokens; ++t) {
      if (linear) {
        // k=q=e_0, beta=1/2, exp(decay)=1: s_t=(s_(t-1)+v_t)/2.
        coefficient = 0.5f * (coefficient + static_cast<float>(offset + t + 1));
      }
      else {
        coefficient = 0.0f;
        for (int64_t k = std::max<int64_t>(1, offset + t - KERNEL + 2);
             k <= offset + t + 1; ++k) {
          coefficient += static_cast<float>(k);
        }
      }
      for (int64_t j = 0; j < width; ++j) {
        expectedOutput[static_cast<size_t>(t * width + j)] =
          coefficient * static_cast<float>(linear ? j % 4 + 1 : j + 1);
      }
    }
    std::vector<float> expectedState(static_cast<size_t>(stateSize), 0.0f);
    if (linear) {
      for (int64_t j = 0; j < HEAD_DIM; ++j) {
        expectedState[static_cast<size_t>(j)] = coefficient * static_cast<float>(j % 4 + 1);
      }
    }
    else {
      for (int64_t t = 0; t < KERNEL - 1; ++t) {
        for (int64_t j = 0; j < CHANNELS; ++j) {
          expectedState[static_cast<size_t>(t * CHANNELS + j)] =
            static_cast<float>((round + t + 1) * (j + 1));
        }
      }
    }
    const auto error = std::max(checkOutput<T>(result[0], {1, tokens, width}, expectedOutput),
                                checkOutput<T>(result[1], stateShape, expectedState));
    const auto* actualState = result[1].GetTensorData<T>();
    state.assign(actualState, actualState + stateSize);
    std::cout << std::setprecision(10)
              << "{\"operator\":\"" << (linear ? "LinearAttention" : "CausalConvWithState")
              << "\",\"backend\":\"" << backend << "\",\"tensorDtype\":\""
              << (sizeof(T) == 2 ? "float16" : "float32") << "\",\"phase\":\""
              << (round == 0 ? "prefill" : "decode") << "\",\"tokens\":" << tokens
              << ",\"sessionWallMs\":" << (round == 0 ? sessionWallMs : 0.0)
              << ",\"sessionProcessCpuMs\":" << (round == 0 ? sessionCpuMs : 0.0)
              << ",\"runWallMs\":" << runWallMs << ",\"runProcessCpuMs\":" << runCpuMs
              << ",\"inputTensorBytes\":" << inputBytes << ",\"outputTensorBytes\":"
              << (expectedOutput.size() + expectedState.size()) * sizeof(T)
              << ",\"maxAbsError\":" << error << ",\"stateAndShapePass\":true}\n";
  }
}

void
probeModel(const char* path, const char* logitsPath)
{
  Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "cpu-model-probe");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  options.DisableMemPattern();
  Ort::Session session(environment, path, options);
  Ort::AllocatorWithDefaultOptions allocator;
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<std::string> names;
  std::vector<Ort::Value> values;
  std::vector<std::vector<int64_t>> integers;
  std::vector<std::vector<float>> floats;
  const auto count = session.GetInputCount();
  names.reserve(count);
  values.reserve(count);
  integers.reserve(count);
  floats.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    names.emplace_back(session.GetInputNameAllocated(index, allocator).get());
    const auto& name = names.back();
    const auto typeInfo = session.GetInputTypeInfo(index);
    const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    auto shape = tensorInfo.GetShape();
    if (name.rfind("past_key_values.", 0) == 0) {
      if (shape.size() != 4 || tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("MODEL_PROBE_REQUIRES_FLOAT32_KV_INPUTS");
      for (size_t axis = 0; axis < shape.size(); ++axis)
        if (shape[axis] < 0) shape[axis] = axis == 2 ? 0 : 1;
      if (shape[2] != 0) throw std::runtime_error("MODEL_PROBE_REQUIRES_EMPTY_INITIAL_KV");
      floats.emplace_back();
      values.emplace_back(Ort::Value::CreateTensor<float>(memory, floats.back().data(), 0,
                                                         shape.data(), shape.size()));
    }
    else {
      if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ||
          (name != "input_ids" && name != "attention_mask" && name != "position_ids"))
        throw std::runtime_error("MODEL_PROBE_INPUT_CONTRACT_UNSUPPORTED");
      for (auto& size : shape) if (size < 0) size = 1;
      const auto elements = std::accumulate(shape.begin(), shape.end(), int64_t{1},
                                             std::multiplies<int64_t>());
      if (elements != 1) throw std::runtime_error("MODEL_PROBE_REQUIRES_SINGLE_TOKEN");
      integers.push_back({name == "input_ids" ? 42 : name == "attention_mask" ? 1 : 0});
      values.emplace_back(Ort::Value::CreateTensor<int64_t>(
        memory, integers.back().data(), 1, shape.data(), shape.size()));
    }
  }
  std::vector<const char*> inputs;
  for (const auto& name : names) inputs.push_back(name.c_str());
  const char* output = "logits";
  const auto started = Clock::now();
  auto result = session.Run(Ort::RunOptions{nullptr}, inputs.data(), values.data(), values.size(),
                            &output, 1);
  const auto info = result.at(0).GetTensorTypeAndShapeInfo();
  if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetElementCount() == 0)
    throw std::runtime_error("MODEL_PROBE_LOGITS_CONTRACT");
  const auto* logits = result[0].GetTensorData<float>();
  if (!std::all_of(logits, logits + info.GetElementCount(),
                   [] (float value) { return std::isfinite(value); }))
    throw std::runtime_error("MODEL_PROBE_NONFINITE_LOGITS");
  if (logitsPath != nullptr) {
    std::ofstream file(logitsPath, std::ios::binary);
    file.write(reinterpret_cast<const char*>(logits), info.GetElementCount() * sizeof(float));
    if (!file) throw std::runtime_error("MODEL_PROBE_LOGITS_WRITE_FAILED");
  }
  std::cout << "{\"status\":\"PASS\",\"scope\":\"CPU_MODEL_SINGLE_PREFILL_ONLY\","
               "\"runSeconds\":" << std::chrono::duration<double>(Clock::now() - started).count()
            << ",\"logits\":" << info.GetElementCount() << "}\n";
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if ((argc == 4 || argc == 5) && std::string(argv[2]) == "cpu-model") {
      if (Ort::GetVersionString() != argv[1])
        throw std::runtime_error("PROBE_RUNTIME_VERSION_MISMATCH");
      probeModel(argv[3], argc == 5 ? argv[4] : nullptr);
      return 0;
    }
    if (argc != 3 || Ort::GetVersionString() != argv[1] ||
        (std::string(argv[2]) != "cpu" && std::string(argv[2]) != "cpu-fp16" &&
         std::string(argv[2]) != "cuda")) {
      throw std::runtime_error("PROBE_RUNTIME_OR_BACKEND_MISMATCH");
    }
    const std::string mode = argv[2];
    const std::string backend = mode == "cpu-fp16" ? "cpu" : mode;
    Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "required-operators");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    if (backend == "cuda") {
      options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
      OrtCUDAProviderOptions cudaOptions{};
      cudaOptions.device_id = 0;
      options.AppendExecutionProvider_CUDA(cudaOptions);
    }
    if (mode != "cpu") {
      runOperator<Ort::Float16_t>(environment, options, false, backend);
      runOperator<Ort::Float16_t>(environment, options, true, backend);
    }
    else {
      runOperator<float>(environment, options, false, backend);
      runOperator<float>(environment, options, true, backend);
    }
    std::cout << "{\"status\":\"PASS\",\"scope\":\"TINY_BACKEND_OPERATORS_ONLY\","
                 "\"operators\":2,\"rounds\":4,\"backend\":\"" << backend
              << "\",\"tensorDtype\":\"" << (mode == "cpu" ? "float32" : "float16") << "\"}\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
