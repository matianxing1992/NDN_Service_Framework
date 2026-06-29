#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <stdexcept>
#include <utility>

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <onnxruntime_cxx_api.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ndnsf::di {
namespace {

Ort::Env&
ortEnv()
{
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-di");
  return env;
}

Ort::SessionOptions
makeSessionOptions()
{
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  return options;
}

std::vector<int64_t>
parseShape(const std::string& value)
{
  std::vector<int64_t> shape;
  std::string current;
  for (const auto ch : value) {
    if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '-') {
      current.push_back(ch);
      continue;
    }
    if (!current.empty()) {
      shape.push_back(std::stoll(current));
      current.clear();
    }
  }
  if (!current.empty()) {
    shape.push_back(std::stoll(current));
  }
  return shape;
}

std::vector<std::string>
splitNames(const std::string& value)
{
  std::vector<std::string> names;
  std::string current;
  std::stringstream input(value);
  while (std::getline(input, current, ',')) {
    current.erase(current.begin(),
                  std::find_if(current.begin(), current.end(), [] (unsigned char ch) {
                    return !std::isspace(ch);
                  }));
    current.erase(std::find_if(current.rbegin(), current.rend(), [] (unsigned char ch) {
                    return !std::isspace(ch);
                  }).base(),
                  current.end());
    if (!current.empty()) {
      names.push_back(current);
    }
  }
  return names;
}

double
elapsedMs(std::chrono::steady_clock::time_point start,
          std::chrono::steady_clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - start).count();
}

bool
runtimeTimingEnabled()
{
  const char* value = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (value == nullptr) {
    return false;
  }
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
           text == "off" || text == "OFF");
}

std::string
metadataValue(const NativeModelRunnerSpec& spec,
              const std::vector<std::string>& keys)
{
  for (const auto& key : keys) {
    const auto found = spec.metadata.find(key);
    if (found != spec.metadata.end() && !found->second.empty()) {
      return found->second;
    }
  }
  return "";
}

std::size_t
metadataSizeValue(const NativeModelRunnerSpec& spec,
                  const std::vector<std::string>& keys,
                  std::size_t fallback = 0)
{
  const auto value = metadataValue(spec, keys);
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<std::size_t>(std::stoull(value));
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid ONNX Runtime runner size metadata: " + value);
  }
}

double
metadataDoubleValue(const NativeModelRunnerSpec& spec,
                    const std::vector<std::string>& keys,
                    double fallback = 0.0)
{
  const auto value = metadataValue(spec, keys);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stod(value);
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid ONNX Runtime runner double metadata: " + value);
  }
}

std::vector<std::string>
metadataNames(const NativeModelRunnerSpec& spec,
              const std::vector<std::string>& keys)
{
  const auto value = metadataValue(spec, keys);
  if (value.empty()) {
    return {};
  }
  return splitNames(value);
}

std::size_t
elementCount(const std::vector<int64_t>& shape)
{
  std::size_t count = 1;
  for (const auto dim : shape) {
    if (dim <= 0) {
      throw std::invalid_argument("ONNX Runtime runner requires static positive tensor shapes");
    }
    count *= static_cast<std::size_t>(dim);
  }
  return count;
}

std::vector<float>
payloadAsFloat32(const std::vector<uint8_t>& payload)
{
  if (payload.size() % sizeof(float) != 0) {
    throw std::invalid_argument("ONNX Runtime tensor payload is not float32-aligned");
  }
  std::vector<float> values(payload.size() / sizeof(float));
  if (!values.empty()) {
    std::memcpy(values.data(), payload.data(), payload.size());
  }
  return values;
}

NamedTensor
tensorForInput(const TensorBundle& bundle,
               const std::string& inputName,
               const std::vector<int64_t>& fallbackShape)
{
  if (isEncodedTensorBundle(bundle.payload)) {
    const auto tensors = decodeTensorBundle(bundle.payload);
    return findTensor(tensors, inputName);
  }
  return makeFloat32Tensor(inputName, fallbackShape, bundle.payload);
}

std::vector<uint8_t>
float32AsPayload(const float* values, std::size_t count)
{
  std::vector<uint8_t> payload(count * sizeof(float));
  if (!payload.empty()) {
    std::memcpy(payload.data(), values, payload.size());
  }
  return payload;
}

std::vector<int64_t>
shapeForInput(const NativeModelRunnerSpec& spec,
              const std::string& inputName,
              std::size_t index,
              const std::vector<int64_t>& modelShape)
{
  auto shape = parseShape(metadataValue(
    spec,
    {
      "inputShape." + inputName,
      "input_shape." + inputName,
      "inputShape." + std::to_string(index),
      "input_shape." + std::to_string(index),
      index == 0 ? "inputShape" : "",
      index == 0 ? "input_shape" : "",
    }));
  if (!shape.empty()) {
    return shape;
  }
  shape = modelShape;
  for (auto& dim : shape) {
    if (dim <= 0) {
      dim = 1;
    }
  }
  return shape;
}

const TensorBundle&
inputBundleFor(const RoleExecutionContext& ctx,
               const NativeModelRunnerSpec& spec,
               const std::string& inputName,
               std::size_t index)
{
  const auto exact = ctx.inputsByScope.find(inputName);
  if (exact != ctx.inputsByScope.end()) {
    return exact->second;
  }

  const auto configuredScope = metadataValue(
    spec,
    {
      "inputScope." + inputName,
      "input_scope." + inputName,
      "inputScope." + std::to_string(index),
      "input_scope." + std::to_string(index),
    });
  if (!configuredScope.empty()) {
    const auto configured = ctx.inputsByScope.find(configuredScope);
    if (configured != ctx.inputsByScope.end()) {
      return configured->second;
    }
  }

  if (ctx.inputsByScope.size() == 1) {
    return ctx.inputsByScope.begin()->second;
  }

  for (const auto& item : ctx.inputsByScope) {
    if (!item.second.name.empty() && item.second.name == inputName) {
      return item.second;
    }
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    try {
      const auto tensors = decodeTensorBundle(item.second.payload);
      (void)findTensor(tensors, inputName);
      return item.second;
    }
    catch (const std::out_of_range&) {
    }
  }

  throw std::out_of_range("missing ONNX Runtime input bundle for input: " + inputName);
}

std::string
outputScopeFor(const NativeModelRunnerSpec& spec,
               const std::string& outputName,
               std::size_t index)
{
  const auto scope = metadataValue(
    spec,
    {
      "outputScope." + outputName,
      "output_scope." + outputName,
      "outputScope." + std::to_string(index),
      "output_scope." + std::to_string(index),
      index == 0 ? "outputScope" : "",
      index == 0 ? "output_scope" : "",
    });
  return scope.empty() ? outputName : scope;
}

} // namespace

class OnnxRuntimeModelRunner::Impl
{
public:
  explicit Impl(const NativeModelRunnerSpec& spec)
    : sessionOptions(makeSessionOptions())
    , session(ortEnv(), spec.path.c_str(), sessionOptions)
  {
  }

  Ort::SessionOptions sessionOptions;
  Ort::Session session;
};

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
  : m_spec(std::move(spec))
{
  if (m_spec.path.empty()) {
    throw std::invalid_argument("ONNX Runtime runner requires model path");
  }
  m_impl = std::make_unique<Impl>(m_spec);
}

OnnxRuntimeModelRunner::~OnnxRuntimeModelRunner() = default;

std::map<std::string, TensorBundle>
OnnxRuntimeModelRunner::run(const RoleExecutionContext& ctx)
{
  const auto collectStart = std::chrono::steady_clock::now();
  Ort::AllocatorWithDefaultOptions allocator;
  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(
    OrtAllocatorType::OrtArenaAllocator,
    OrtMemTypeDefault);

  std::vector<std::string> inputNames = metadataNames(
    m_spec,
    {"inputNames", "input_names", "input_tensors", "input_tensor"});
  if (inputNames.empty()) {
    const auto count = m_impl->session.GetInputCount();
    for (std::size_t i = 0; i < count; ++i) {
      auto name = m_impl->session.GetInputNameAllocated(i, allocator);
      inputNames.emplace_back(name.get());
    }
  }

  std::vector<std::vector<float>> inputBuffers;
  std::vector<std::vector<int64_t>> inputShapes;
  std::vector<Ort::Value> inputValues;
  std::vector<const char*> inputNamePtrs;
  inputBuffers.reserve(inputNames.size());
  inputShapes.reserve(inputNames.size());
  inputValues.reserve(inputNames.size());
  inputNamePtrs.reserve(inputNames.size());

  for (std::size_t i = 0; i < inputNames.size(); ++i) {
    const auto& inputName = inputNames[i];
    auto typeInfo = m_impl->session.GetInputTypeInfo(i);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    inputShapes.push_back(shapeForInput(m_spec, inputName, i, tensorInfo.GetShape()));
    const auto& bundle = inputBundleFor(ctx, m_spec, inputName, i);
    auto tensor = tensorForInput(bundle, inputName, inputShapes.back());
    if (tensor.elementType != TensorElementType::Float32) {
      throw std::invalid_argument("ONNX Runtime runner currently supports float32 inputs only");
    }
    if (!tensor.shape.empty()) {
      inputShapes.back() = tensor.shape;
    }
    inputBuffers.push_back(payloadAsFloat32(tensor.payload));
    const auto expected = elementCount(inputShapes.back());
    if (inputBuffers.back().size() != expected) {
      throw std::invalid_argument(
        "ONNX Runtime input element count mismatch for " + inputName);
    }

    inputNamePtrs.push_back(inputName.c_str());
    inputValues.push_back(Ort::Value::CreateTensor<float>(
      memoryInfo,
      inputBuffers.back().data(),
      inputBuffers.back().size(),
      inputShapes.back().data(),
      inputShapes.back().size()));
  }

  std::vector<std::string> outputNames = metadataNames(
    m_spec,
    {"outputNames", "output_names", "output_tensors", "output_tensor"});
  if (outputNames.empty()) {
    const auto count = m_impl->session.GetOutputCount();
    for (std::size_t i = 0; i < count; ++i) {
      auto name = m_impl->session.GetOutputNameAllocated(i, allocator);
      outputNames.emplace_back(name.get());
    }
  }

  std::vector<const char*> outputNamePtrs;
  outputNamePtrs.reserve(outputNames.size());
  for (const auto& name : outputNames) {
    outputNamePtrs.push_back(name.c_str());
  }

  const auto runStart = std::chrono::steady_clock::now();
  auto outputs = m_impl->session.Run(
    Ort::RunOptions{nullptr},
    inputNamePtrs.data(),
    inputValues.data(),
    inputValues.size(),
    outputNamePtrs.data(),
    outputNamePtrs.size());
  const auto runDone = std::chrono::steady_clock::now();
  const auto executionDelayMs = metadataDoubleValue(
    m_spec,
    {"executionDelayMs", "execution_delay_ms", "roleExecutionDelayMs",
     "role_execution_delay_ms"});
  if (executionDelayMs > 0.0) {
    std::this_thread::sleep_for(
      std::chrono::duration<double, std::milli>(executionDelayMs));
  }
  const auto delayDone = std::chrono::steady_clock::now();

  std::vector<NamedTensor> namedOutputs;
  namedOutputs.reserve(outputs.size());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    auto& value = outputs[i];
    if (!value.IsTensor()) {
      throw std::runtime_error("ONNX Runtime output is not a tensor");
    }
    auto tensorInfo = value.GetTensorTypeAndShapeInfo();
    if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      throw std::runtime_error("ONNX Runtime runner currently supports float32 outputs only");
    }
    const auto count = tensorInfo.GetElementCount();
    const auto* data = value.GetTensorData<float>();
    NamedTensor tensor;
    tensor.name = outputNames[i];
    tensor.elementType = TensorElementType::Float32;
    tensor.shape = tensorInfo.GetShape();
    tensor.payload = float32AsPayload(data, count);
    namedOutputs.push_back(std::move(tensor));
  }

  std::map<std::string, TensorBundle> result;
  const bool forceEncodedOutput =
    !metadataValue(m_spec, {"output_tensor", "outputTensor", "forceOutputBundle",
                            "force_output_bundle"}).empty() ||
    metadataValue(m_spec, {"final", "is_final"}) == "true";
  if (namedOutputs.size() == 1 && !forceEncodedOutput) {
    const auto scope = outputScopeFor(m_spec, namedOutputs.front().name, 0);
    TensorBundle bundle;
    bundle.name = namedOutputs.front().name;
    bundle.payload = namedOutputs.front().payload;
    bundle.expectedBytes = bundle.payload.size();
    result.emplace(scope, std::move(bundle));
  }
  else {
    const auto padBytes = metadataSizeValue(
      m_spec,
      {"outputBundlePadBytes", "output_bundle_pad_bytes", "padOutputBytes",
       "pad_output_bytes"});
    if (padBytes > 0) {
      NamedTensor padding;
      padding.name = metadataValue(
        m_spec,
        {"outputBundlePadTensor", "output_bundle_pad_tensor",
         "padTensorName", "pad_tensor_name"});
      if (padding.name.empty()) {
        padding.name = "__ndnsf_padding";
      }
      padding.elementType = TensorElementType::Float32;
      padding.shape = {static_cast<std::int64_t>((padBytes + sizeof(float) - 1) /
                                                 sizeof(float))};
      padding.payload.assign(padding.shape.front() * sizeof(float), 0);
      padding.payload.resize(padBytes, 0);
      namedOutputs.push_back(std::move(padding));
    }
    TensorBundle bundle = makeEncodedTensorBundle("onnx-output-bundle", namedOutputs);
    const auto bundleScope = metadataValue(
      m_spec,
      {"outputBundleScope", "output_bundle_scope", "outputScope", "output_scope"});
    result.emplace(bundleScope.empty() ? "onnx-output-bundle" : bundleScope,
                   std::move(bundle));
  }
  const auto packageDone = std::chrono::steady_clock::now();
  if (runtimeTimingEnabled()) {
    std::cout << std::fixed << std::setprecision(3)
              << "\nNDNSF_DI_ONNX_TIMING"
              << " session=" << ctx.sessionId
              << " role=" << ctx.role
              << " collect_ms=" << elapsedMs(collectStart, runStart)
              << " session_ms=0"
              << " run_ms=" << elapsedMs(runStart, runDone)
              << " delay_ms=" << elapsedMs(runDone, delayDone)
              << " publish_ms=" << elapsedMs(delayDone, packageDone)
              << " session_cache=hit"
              << std::endl;
  }
  return result;
}

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  factory.registerBackend(
    "onnxruntime",
    [] (const NativeModelRunnerSpec& spec) -> std::shared_ptr<NativeModelRunner> {
      return std::make_shared<OnnxRuntimeModelRunner>(spec);
    });
}

} // namespace ndnsf::di

#else

namespace ndnsf::di {

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
  : m_spec(std::move(spec))
{
  throw std::runtime_error(
    "C++ ONNX Runtime backend is not enabled; install the ONNX Runtime "
    "C++ development package and build with NDNSF_DI_ENABLE_ONNXRUNTIME_CPP");
}

OnnxRuntimeModelRunner::~OnnxRuntimeModelRunner() = default;

std::map<std::string, TensorBundle>
OnnxRuntimeModelRunner::run(const RoleExecutionContext&)
{
  throw std::runtime_error("C++ ONNX Runtime backend is not enabled");
}

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  factory.registerBackend(
    "onnxruntime",
    [] (const NativeModelRunnerSpec& spec) -> std::shared_ptr<NativeModelRunner> {
      return std::make_shared<OnnxRuntimeModelRunner>(spec);
    });
}

} // namespace ndnsf::di

#endif // NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
