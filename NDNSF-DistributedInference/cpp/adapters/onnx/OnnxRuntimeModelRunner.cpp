#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <stdexcept>
#include <utility>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>

namespace ndnsf::di {
namespace {

std::string
runnerMetadataValue(const NativeModelRunnerSpec& spec,
                    std::initializer_list<const char*> keys)
{
  for (const auto* key : keys) {
    const auto found = spec.metadata.find(key);
    if (found != spec.metadata.end() && !found->second.empty()) {
      return found->second;
    }
  }
  return "";
}

bool
runnerMetadataBool(const NativeModelRunnerSpec& spec,
                   std::initializer_list<const char*> keys)
{
  auto value = runnerMetadataValue(spec, keys);
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool
isControlInputName(const std::string& name)
{
  return name == "input_ids" || name == "token_ids" || name == "token_id" ||
         name == "attention_mask" || name == "position_ids" ||
         name == "cache_position";
}

bool
hasProvider(const std::vector<std::string>& providers, const std::string& name)
{
  return std::find(providers.begin(), providers.end(), name) != providers.end();
}

} // namespace

OnnxRuntimeProviderSelection
resolveOnnxRuntimeProviderSelection(const NativeModelRunnerSpec& spec,
                                    const std::vector<std::string>& availableProviders)
{
  auto requested = runnerMetadataValue(
    spec, {"executionProvider", "execution_provider", "device.kind", "deviceKind"});
  if (requested.empty()) {
    requested = "cpu";
  }
  std::transform(requested.begin(), requested.end(), requested.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (requested == "cudaexecutionprovider") requested = "cuda";
  if (requested == "cpuexecutionprovider") requested = "cpu";
  if (requested != "cuda" && requested != "cpu") {
    throw std::invalid_argument("unsupported ONNX Runtime execution provider: " + requested);
  }

  OnnxRuntimeProviderSelection result;
  result.requestedProvider = requested;
  result.deviceId = runnerMetadataValue(
    spec, {"deviceId", "device_id", "device.id", "cudaDeviceId", "cuda_device_id"});
  if (result.deviceId.empty()) {
    result.deviceId = requested == "cuda" ? "0" : "cpu0";
  }
  if (requested == "cpu") {
    if (!hasProvider(availableProviders, "CPUExecutionProvider")) {
      throw std::runtime_error("required ONNX Runtime CPUExecutionProvider is unavailable");
    }
    result.selectedProvider = "cpu";
    result.deviceId = "cpu0";
    return result;
  }
  if (hasProvider(availableProviders, "CUDAExecutionProvider")) {
    result.selectedProvider = "cuda";
    return result;
  }
  if (!runnerMetadataBool(spec, {"allowCpuFallback", "allow_cpu_fallback"})) {
    throw std::runtime_error(
      "required ONNX Runtime CUDAExecutionProvider is unavailable; CPU fallback is disabled");
  }
  if (!hasProvider(availableProviders, "CPUExecutionProvider")) {
    throw std::runtime_error("CUDA provider unavailable and CPU fallback provider is unavailable");
  }
  result.selectedProvider = "cpu";
  result.deviceId = "cpu0";
  result.usedCpuFallback = true;
  return result;
}

void
CausalPositionInputContractV1::validate(const StatefulOnnxIoContractV1& io) const
{
  if (policy != "qwen-causal-position-v1" ||
      attentionMaskInputName.empty() || positionIdsInputName.empty()) {
    throw std::invalid_argument(
      "stateful ONNX causal position policy is incomplete");
  }
  const auto contains = [&io] (const std::string& name) {
    return std::find(io.inputNames.begin(), io.inputNames.end(), name) !=
           io.inputNames.end();
  };
  if (!contains(attentionMaskInputName) || !contains(positionIdsInputName) ||
      (!cachePositionInputName.empty() && !contains(cachePositionInputName))) {
    throw std::invalid_argument(
      "stateful ONNX causal position inputs do not match the graph signature");
  }
}

std::map<std::string, TensorBundle>
materializeCausalPositionInputsV1(
  const CausalPositionInputContractV1& contract,
  const StatefulOnnxIoContractV1& io,
  const GenerationEpochLineageV1& lineage,
  std::uint32_t newTokenCount)
{
  contract.validate(io);
  lineage.validate();
  if (newTokenCount == 0 ||
      lineage.logicalPrefixTokenCount < newTokenCount) {
    throw std::invalid_argument(
      "causal position materialization has invalid logical token extent");
  }
  const auto current = static_cast<std::int64_t>(
    lineage.logicalPrefixTokenCount);
  const auto first = current - static_cast<std::int64_t>(newTokenCount);
  const auto makeInt64 = [] (const std::string& name,
                             std::vector<std::int64_t> shape,
                             const std::vector<std::int64_t>& values) {
    NamedTensor tensor;
    tensor.name = name;
    tensor.elementType = TensorElementType::Int64;
    tensor.shape = std::move(shape);
    tensor.payload.resize(values.size() * sizeof(std::int64_t));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    return makeEncodedTensorBundle(name, {std::move(tensor)});
  };
  std::vector<std::int64_t> positions(newTokenCount);
  for (std::uint32_t index = 0; index < newTokenCount; ++index) {
    positions[index] = first + static_cast<std::int64_t>(index);
  }
  std::vector<std::int64_t> attention(
    lineage.logicalPrefixTokenCount, 1);
  std::map<std::string, TensorBundle> result;
  result.emplace(
    contract.attentionMaskInputName,
    makeInt64(contract.attentionMaskInputName,
              {1, current}, attention));
  result.emplace(
    contract.positionIdsInputName,
    makeInt64(contract.positionIdsInputName,
              {1, static_cast<std::int64_t>(newTokenCount)}, positions));
  if (!contract.cachePositionInputName.empty()) {
    result.emplace(
      contract.cachePositionInputName,
      makeInt64(contract.cachePositionInputName,
                {static_cast<std::int64_t>(newTokenCount)}, positions));
  }
  return result;
}

} // namespace ndnsf

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <onnxruntime_cxx_api.h>
#pragma GCC diagnostic pop

#include <ndn-cxx/util/sha256.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ndnsf::di {
namespace {

/**
 * Dynamically loaded CUDA runtime used only for conversation-state transfers.
 * The ONNX adapter is linked against ORT rather than the CUDA toolkit, so a
 * CPU build remains independent of CUDA while a missing runtime makes a
 * transfer fail closed.  ORT does not own raw device memory passed to
 * CreateTensor; the adapter therefore tracks those allocations explicitly.
 */
class CudaRuntimeApi
{
public:
  using CudaMalloc = int (*)(void**, std::size_t);
  using CudaMemcpy = int (*)(void*, const void*, std::size_t, int);
  using CudaFree = int (*)(void*);

  CudaRuntimeApi()
  {
    for (const char* library : {"libcudart.so.12", "libcudart.so"}) {
      m_handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
      if (m_handle != nullptr) {
        break;
      }
    }
    if (m_handle == nullptr) {
      return;
    }
    m_malloc = reinterpret_cast<CudaMalloc>(dlsym(m_handle, "cudaMalloc"));
    m_memcpy = reinterpret_cast<CudaMemcpy>(dlsym(m_handle, "cudaMemcpy"));
    m_free = reinterpret_cast<CudaFree>(dlsym(m_handle, "cudaFree"));
    if (m_malloc == nullptr || m_memcpy == nullptr || m_free == nullptr) {
      dlclose(m_handle);
      m_handle = nullptr;
      m_malloc = nullptr;
      m_memcpy = nullptr;
      m_free = nullptr;
    }
  }

  CudaRuntimeApi(const CudaRuntimeApi&) = delete;
  CudaRuntimeApi& operator=(const CudaRuntimeApi&) = delete;

  ~CudaRuntimeApi()
  {
    if (m_handle != nullptr) {
      dlclose(m_handle);
    }
  }

  bool available() const { return m_handle != nullptr; }

  int malloc(void** pointer, std::size_t bytes) const
  {
    return m_malloc(pointer, bytes);
  }

  int memcpy(void* destination, const void* source,
             std::size_t bytes, int kind) const
  {
    return m_memcpy(destination, source, bytes, kind);
  }

  int free(void* pointer) const
  {
    return m_free(pointer);
  }

private:
  void* m_handle = nullptr;
  CudaMalloc m_malloc = nullptr;
  CudaMemcpy m_memcpy = nullptr;
  CudaFree m_free = nullptr;
};

constexpr int cudaMemcpyHostToDevice = 1;
constexpr int cudaMemcpyDeviceToHost = 2;

void
freeCudaPointers(CudaRuntimeApi& cuda,
                 std::map<std::string, void*>& pointers)
{
  if (!cuda.available()) {
    return;
  }
  for (auto& item : pointers) {
    if (item.second != nullptr) {
      (void)cuda.free(item.second);
      item.second = nullptr;
    }
  }
  pointers.clear();
}

Ort::Env&
ortEnv()
{
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-di");
  return env;
}

Ort::SessionOptions
makeSessionOptions(const OnnxRuntimeProviderSelection& selection,
                   const NativeModelRunnerSpec& spec)
{
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  const auto profilePrefix = runnerMetadataValue(
    spec, {"providerProfilePrefix", "provider_profile_prefix"});
  if (!profilePrefix.empty()) {
    options.EnableProfiling(profilePrefix.c_str());
  }
  if (selection.selectedProvider == "cuda") {
    int deviceId;
    try {
      deviceId = std::stoi(selection.deviceId);
    }
    catch (const std::exception&) {
      throw std::invalid_argument("invalid ONNX Runtime CUDA device ID: " + selection.deviceId);
    }
    // FP32 graph/oracle contracts must not silently use reduced-precision
    // TF32 kernels. Tiger YOLO run 209982 crossed the detection threshold
    // under the CUDA default; the same model passed with use_tf32=0 (209983).
    // ORT 1.20 exposes the V2 C API, but not the newer C++ options owner.
    // Keep ownership exception-safe without requiring a newer runtime SDK.
    OrtCUDAProviderOptionsV2* rawOptions = nullptr;
    Ort::ThrowOnError(Ort::GetApi().CreateCUDAProviderOptions(&rawOptions));
    const auto release = [](OrtCUDAProviderOptionsV2* value) {
      Ort::GetApi().ReleaseCUDAProviderOptions(value);
    };
    std::unique_ptr<OrtCUDAProviderOptionsV2, decltype(release)> cudaOptions(rawOptions, release);
    const auto device = std::to_string(deviceId);
    const char* keys[] = {"device_id", "use_tf32"};
    const char* values[] = {device.c_str(), "0"};
    Ort::ThrowOnError(Ort::GetApi().UpdateCUDAProviderOptions(cudaOptions.get(), keys, values, 2));
    options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
    if (!runnerMetadataBool(spec, {"allowCpuFallback", "allow_cpu_fallback"})) {
      options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
  }
  return options;
}

void
requireCudaDevice(const OnnxRuntimeProviderSelection& selection)
{
  int requestedDevice = 0;
  try {
    requestedDevice = std::stoi(selection.deviceId);
  }
  catch (const std::exception&) {
    throw std::invalid_argument(
      "invalid ONNX Runtime CUDA device ID: " + selection.deviceId);
  }
  if (requestedDevice < 0) {
    throw std::invalid_argument(
      "invalid ONNX Runtime CUDA device ID: " + selection.deviceId);
  }

  void* runtime = nullptr;
  for (const char* library : {"libcudart.so.12", "libcudart.so"}) {
    runtime = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (runtime != nullptr) {
      break;
    }
  }
  if (runtime == nullptr) {
    throw std::runtime_error(
      "required ONNX Runtime CUDA device unavailable: CUDA runtime could not be loaded");
  }

  using CudaGetDeviceCount = int (*)(int*);
  auto* getDeviceCount = reinterpret_cast<CudaGetDeviceCount>(
    dlsym(runtime, "cudaGetDeviceCount"));
  if (getDeviceCount == nullptr) {
    dlclose(runtime);
    throw std::runtime_error(
      "required ONNX Runtime CUDA device unavailable: cudaGetDeviceCount is unavailable");
  }

  int deviceCount = 0;
  const int status = getDeviceCount(&deviceCount);
  dlclose(runtime);
  if (status != 0 || deviceCount <= requestedDevice) {
    std::ostringstream message;
    message << "required ONNX Runtime CUDA device unavailable: requested device "
            << requestedDevice << ", visible device count " << deviceCount
            << ", cuda status " << status;
    throw std::runtime_error(message.str());
  }
}

/**
 * Copy an ORT tensor into a host-owned buffer without depending on the
 * post-1.20 Ort::Env::CopyTensor wrapper.  The exact SIF currently carries
 * ONNX Runtime 1.20, whose C++ API exposes tensor memory information but not
 * Env::CopyTensor.  Keeping the CUDA call dynamically loaded preserves the
 * CPU-only build and avoids adding a hard CUDA toolkit link dependency.
 */
void
copyOrtTensorToHost(const Ort::Value& source, void* destination, std::size_t bytes)
{
  if (bytes == 0) {
    return;
  }
  if (destination == nullptr || source.GetTensorRawData() == nullptr) {
    throw std::runtime_error("cannot copy an empty ONNX Runtime tensor buffer");
  }

  const auto memoryInfo = source.GetTensorMemoryInfo();
  if (memoryInfo.GetDeviceType() == OrtMemoryInfoDeviceType_CPU) {
    std::memcpy(destination, source.GetTensorRawData(), bytes);
    return;
  }
  if (memoryInfo.GetDeviceType() != OrtMemoryInfoDeviceType_GPU) {
    throw std::runtime_error(
      "unsupported ONNX Runtime tensor device for host export");
  }

  CudaRuntimeApi cuda;
  if (!cuda.available()) {
    throw std::runtime_error(
      "CUDA tensor export requires a loadable CUDA runtime");
  }
  // cudaMemcpyDeviceToHost from cuda_runtime_api.h.  Do not include the CUDA
  // toolkit header here: CPU-only builds must remain independent of it.
  const int status = cuda.memcpy(
    destination, source.GetTensorRawData(), bytes, cudaMemcpyDeviceToHost);
  if (status != 0) {
    throw std::runtime_error(
      "failed to copy CUDA ONNX tensor to host (cudaMemcpy status " +
      std::to_string(status) + ")");
  }
}

OnnxRuntimeProviderSelection
resolveRuntimeProviderSelection(const NativeModelRunnerSpec& spec)
{
  auto selection = resolveOnnxRuntimeProviderSelection(spec, Ort::GetAvailableProviders());
  if (selection.selectedProvider == "cuda") {
    // Ort reports CUDAExecutionProvider when its shared libraries are present,
    // even on a CPU-only node.  Probe the actual visible CUDA device before
    // constructing an Ort::Session; otherwise ORT may terminate the process
    // while initializing the CUDA provider instead of returning an admission
    // error to the native Provider.
    requireCudaDevice(selection);
  }
  return selection;
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

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
           return std::isxdigit(ch) != 0;
         });
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
    if (dim < 0) {
      throw std::invalid_argument("ONNX Runtime runner requires resolved non-negative tensor shapes");
    }
    count *= static_cast<std::size_t>(dim);
  }
  return count;
}

ONNXTensorElementDataType
toOnnxElementType(TensorElementType type)
{
  switch (type) {
    case TensorElementType::Float32:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case TensorElementType::Float16:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    case TensorElementType::Int64:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    case TensorElementType::Bool:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
    case TensorElementType::UInt8:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
  }
  throw std::invalid_argument("unsupported NDNSF tensor element type");
}

TensorElementType
fromOnnxElementType(ONNXTensorElementDataType type)
{
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return TensorElementType::Float32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      return TensorElementType::Float16;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      return TensorElementType::Int64;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      return TensorElementType::Bool;
    default:
      throw std::runtime_error(
        "ONNX Runtime tensor dtype is not supported by the pilot codec: " +
        std::to_string(static_cast<int>(type)));
  }
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
    const auto& only = ctx.inputsByScope.begin()->second;
    // A single transport scope is not necessarily a bundle for every model
    // input.  In particular, a request-input bundle may contain only
    // input_ids while state inputs are intentionally omitted on the first
    // epoch.  Do not return that bundle and defer the name error to
    // tensorForInput(), otherwise the state-zero fallback below cannot run.
    if (!isEncodedTensorBundle(only.payload) ||
        only.name == inputName) {
      return only;
    }
    try {
      const auto tensors = decodeTensorBundle(only.payload);
      (void)findTensor(tensors, inputName);
      return only;
    }
    catch (const std::out_of_range&) {
      // Continue to the explicit missing-input error below.
    }
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

NamedTensor
passthroughTensorFor(const RoleExecutionContext& ctx, const std::string& name)
{
  for (const auto& item : ctx.inputsByScope) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    const auto tensors = decodeTensorBundle(item.second.payload);
    try {
      return findTensor(tensors, name);
    }
    catch (const std::out_of_range&) {
    }
  }
  throw std::out_of_range("missing ONNX Runtime passthrough tensor: " + name);
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
    : selection(resolveRuntimeProviderSelection(spec))
    , sessionOptions(makeSessionOptions(selection, spec))
    , session(ortEnv(), spec.path.c_str(), sessionOptions)
    , profilingEnabled(!runnerMetadataValue(
        spec, {"providerProfilePrefix", "provider_profile_prefix"}).empty())
  {
    const auto statefulInputMetadata = metadataNames(
      spec, {"stateInputNames", "state_input_names",
             "stateInputTensors", "state_input_tensors"});
    const auto statefulOutputMetadata = metadataNames(
      spec, {"stateOutputNames", "state_output_names",
             "stateOutputTensors", "state_output_tensors"});
    const bool statefulDeclared = runnerMetadataBool(
      spec, {"statefulModel", "stateful_model", "stateful"}) ||
      !statefulInputMetadata.empty() || !statefulOutputMetadata.empty();
    if (!statefulDeclared) {
      return;
    }

    StatefulOnnxIoContractV1 contract;
    contract.inputNames = metadataNames(
      spec, {"inputNames", "input_names", "input_tensors", "input_tensor"});
    contract.outputNames = metadataNames(
      spec, {"outputNames", "output_names", "output_tensors", "output_tensor"});
    if (contract.inputNames.empty()) {
      Ort::AllocatorWithDefaultOptions allocator;
      for (std::size_t i = 0; i < session.GetInputCount(); ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        contract.inputNames.emplace_back(name.get());
      }
    }
    if (contract.outputNames.empty()) {
      Ort::AllocatorWithDefaultOptions allocator;
      for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
        auto name = session.GetOutputNameAllocated(i, allocator);
        contract.outputNames.emplace_back(name.get());
      }
    }
    contract.stateInputNames = statefulInputMetadata;
    contract.stateOutputNames = statefulOutputMetadata;
    if (contract.stateInputNames.empty()) {
      for (const auto& name : contract.inputNames) {
        if (name.size() > 3 && name.compare(name.size() - 3, 3, "_in") == 0) {
          contract.stateInputNames.push_back(name);
        }
      }
    }
    if (contract.stateOutputNames.empty()) {
      for (const auto& name : contract.outputNames) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, "_out") == 0) {
          contract.stateOutputNames.push_back(name);
        }
      }
    }
    contract.validate();
    const auto sessionNames = [this] (bool inputs) {
      Ort::AllocatorWithDefaultOptions allocator;
      std::vector<std::string> names;
      const auto count = inputs ? session.GetInputCount() : session.GetOutputCount();
      names.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        auto name = inputs ? session.GetInputNameAllocated(i, allocator) :
                             session.GetOutputNameAllocated(i, allocator);
        names.emplace_back(name.get());
      }
      return names;
    };
    if (sessionNames(true) != contract.inputNames ||
        sessionNames(false) != contract.outputNames) {
      throw std::invalid_argument(
        "stateful ONNX I/O manifest does not match the graph signature");
    }
    const auto positionPolicy = metadataValue(
      spec, {"positionInputPolicy", "position_input_policy"});
    const bool graphDeclaresCausalPositions = std::any_of(
      contract.inputNames.begin(), contract.inputNames.end(),
      [] (const std::string& name) {
        return name == "attention_mask" || name == "position_ids" ||
               name == "cache_position";
      });
    if (graphDeclaresCausalPositions && positionPolicy.empty()) {
      throw std::invalid_argument(
        "stateful ONNX graph requires an adapter-certified position input policy");
    }
    if (!positionPolicy.empty()) {
      CausalPositionInputContractV1 positions;
      positions.policy = positionPolicy;
      positions.attentionMaskInputName = metadataValue(
        spec, {"attentionMaskInputName", "attention_mask_input_name"});
      positions.positionIdsInputName = metadataValue(
        spec, {"positionIdsInputName", "position_ids_input_name"});
      positions.cachePositionInputName = metadataValue(
        spec, {"cachePositionInputName", "cache_position_input_name"});
      positions.validate(contract);
      causalPositionInputs = std::move(positions);
    }
    statefulIo = std::move(contract);
  }

  ~Impl()
  {
    std::lock_guard<std::mutex> lock(executionMutex);
    std::set<std::string> conversationKeys;
    for (const auto& item : deviceStateByConversation) {
      conversationKeys.insert(item.first);
    }
    for (const auto& item : hostStateByConversation) {
      conversationKeys.insert(item.first);
    }
    for (const auto& item : externalDeviceAllocationsByConversation) {
      conversationKeys.insert(item.first);
    }
    for (const auto& key : conversationKeys) {
      releaseConversationLocked(key);
    }
  }

  OnnxRuntimeProviderSelection selection;
  Ort::SessionOptions sessionOptions;
  Ort::Session session;
  std::optional<StatefulOnnxIoContractV1> statefulIo;
  std::optional<CausalPositionInputContractV1> causalPositionInputs;
  bool profilingEnabled = false;
  std::atomic<bool> profilingCaptured{false};
  mutable std::mutex profileMutex;
  // A stateful CUDA session keeps the state tensors in ORT-managed device
  // memory between request-scoped epochs.  The map is keyed by the
  // request/session identity, so concurrent generations cannot accidentally
  // consume one another's recurrent state.  Host TensorBundle bytes remain a
  // diagnostic/transport representation only; they are not used when a
  // device-resident predecessor is available.
  mutable std::mutex executionMutex;
  std::map<std::string, std::map<std::string, Ort::Value>> deviceStateBySession;
  struct HostStateTensor
  {
    TensorElementType elementType = TensorElementType::Float32;
    std::vector<std::int64_t> shape;
    std::vector<std::uint8_t> payload;
  };
  using HostStateMap = std::map<std::string, HostStateTensor>;
  // Conversation state is retained by the adapter, not by the coordinator.
  // Host tensors are a bounded pause tier; device tensors stay in ORT/CUDA
  // memory until a terminal release or a host pause.
  std::map<std::string, std::map<std::string, Ort::Value>> deviceStateByConversation;
  std::map<std::string, HostStateMap> hostStateByConversation;
  std::map<std::string, std::map<std::string, void*>> externalDeviceAllocationsByConversation;
  std::map<std::string, std::string> conversationBindingBySession;
  std::map<std::string, NativeConversationStateHandleV1> stateHandleByConversation;
  // The coordinator receives only this authenticated opaque reference.  The
  // actual Ort::Value allocations remain owned by deviceStateBySession and
  // never cross ProviderRoleWorker, the state store, or an NDN bundle.
  std::map<std::string, NativeOpaqueStateHandleV1> stateHandleBySession;
  NativeRuntimeMetrics runtimeMetrics;

  void
  releaseConversationLocked(const std::string& conversationKey)
  {
    deviceStateByConversation.erase(conversationKey);
    hostStateByConversation.erase(conversationKey);
    stateHandleByConversation.erase(conversationKey);
    auto allocations = externalDeviceAllocationsByConversation.find(conversationKey);
    if (allocations != externalDeviceAllocationsByConversation.end()) {
      CudaRuntimeApi cuda;
      freeCudaPointers(cuda, allocations->second);
      externalDeviceAllocationsByConversation.erase(allocations);
    }
    for (auto it = conversationBindingBySession.begin();
         it != conversationBindingBySession.end();) {
      if (it->second == conversationKey) {
        it = conversationBindingBySession.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::size_t
  eraseSessionStateLocked(const std::string& sessionId)
  {
    const auto found = deviceStateBySession.find(sessionId);
    const auto handle = stateHandleBySession.find(sessionId);
    if (found == deviceStateBySession.end() &&
        handle == stateHandleBySession.end()) {
      return 0;
    }
    if (found != deviceStateBySession.end()) {
      deviceStateBySession.erase(found);
    }
    if (handle != stateHandleBySession.end()) {
      stateHandleBySession.erase(handle);
    }
    conversationBindingBySession.erase(sessionId);
    ++runtimeMetrics.stateReleases;
    return 1;
  }
};

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
  : m_spec(std::move(spec))
{
  if (m_spec.path.empty()) {
    throw std::invalid_argument("ONNX Runtime runner requires model path");
  }
  m_impl = std::make_unique<Impl>(m_spec);
  if (m_spec.metadata.count("evidence.providerBootId") != 0) {
    const bool isCuda = m_impl->selection.selectedProvider == "cuda";
    m_evidence = executionEvidenceFromRunnerSpec(
      m_spec,
      isCuda ? RunnerKind::OnnxRuntimeCuda : RunnerKind::OnnxRuntimeCpu,
      Ort::GetVersionString(),
      isCuda ? "cuda" : "cpu",
      m_impl->selection.deviceId);
    m_evidence->loadCompleted = true;
    if (isCuda) {
      m_evidence->gpuUuid = queryCudaDeviceUuid(std::stoi(m_impl->selection.deviceId));
      m_evidence->gpuUuids = {m_evidence->gpuUuid};
      m_evidence->gpuIdentitySource = "cuda-runtime-pci+driver-uuid";
    }
  }

  // Session construction proves model load, but not executable readiness.
  // Execute one shape-valid zero input through the real selected provider so
  // DATA_DRIVEN_V2 cannot publish READY for an unavailable CUDA graph.
  RoleExecutionContext warmup;
  warmup.sessionId = "native-runtime-warmup";
  warmup.role = m_spec.role;
  Ort::AllocatorWithDefaultOptions allocator;
  const auto inputCount = m_impl->session.GetInputCount();
  for (std::size_t index = 0; index < inputCount; ++index) {
    auto name = m_impl->session.GetInputNameAllocated(index, allocator);
    const std::string inputName(name.get());
    auto typeInfo = m_impl->session.GetInputTypeInfo(index);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    NamedTensor tensor;
    tensor.name = inputName;
    tensor.elementType = fromOnnxElementType(tensorInfo.GetElementType());
    tensor.shape = shapeForInput(
      m_spec, inputName, index, tensorInfo.GetShape());
    tensor.payload.assign(
      elementCount(tensor.shape) * tensorElementByteSize(tensor.elementType),
      0);
    warmup.inputsByScope.emplace(
      inputName, makeEncodedTensorBundle(inputName, {std::move(tensor)}));
  }
  (void)run(warmup);
  if (m_evidence) {
    m_evidence->warmupCompleted = true;
    m_evidence->validate();
  }
}

OnnxRuntimeModelRunner::~OnnxRuntimeModelRunner() = default;

void
OnnxRuntimeModelRunner::releaseSessionState(const std::string& sessionId)
{
  if (!m_impl || sessionId.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  m_impl->eraseSessionStateLocked(sessionId);
}

std::optional<std::map<std::string, TensorBundle>>
OnnxRuntimeModelRunner::runStreamed(const RoleExecutionContext& ctx)
{
  return runStreamedImpl(ctx);
}

std::map<std::string, TensorBundle>
OnnxRuntimeModelRunner::run(const RoleExecutionContext& ctx)
{
  // The CUDA state map below is part of the persistent session transaction.
  // Serialize transitions so concurrent requests cannot observe or replace
  // one another's recurrent state.
  std::unique_lock<std::mutex> executionLock(m_impl->executionMutex);
  std::unique_lock<std::mutex> firstProfileRunLock;
  if (m_impl->profilingEnabled &&
      !m_impl->profilingCaptured.load(std::memory_order_acquire)) {
    firstProfileRunLock = std::unique_lock<std::mutex>(m_impl->profileMutex);
    if (m_impl->profilingCaptured.load(std::memory_order_acquire)) {
      firstProfileRunLock.unlock();
    }
  }
  const auto collectStart = std::chrono::steady_clock::now();
  RoleExecutionContext effectiveContext = ctx;
  effectiveContext.inputsByScope = applyCertifiedTensorRedistributions(ctx);
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
  if (m_impl->statefulIo) {
    inputNames = m_impl->statefulIo->inputNames;
  }
  std::map<std::string, TensorBundle> causalPositionInputs;
  if (m_impl->causalPositionInputs) {
    if (effectiveContext.sessionId == "native-runtime-warmup") {
      // Constructor warm-up supplies an explicit graph-shaped zero tensor for
      // every input and has no request token lineage.
    }
    else {
      if (!effectiveContext.generationLineage ||
          effectiveContext.generationInputTokenCount == 0) {
        throw std::invalid_argument(
          "stateful ONNX execution is missing authenticated generation lineage");
      }
      causalPositionInputs = materializeCausalPositionInputsV1(
        *m_impl->causalPositionInputs,
        *m_impl->statefulIo,
        *effectiveContext.generationLineage,
        effectiveContext.generationInputTokenCount);
    }
  }

  const bool deviceResidentState = m_impl->statefulIo.has_value() &&
    m_impl->selection.selectedProvider == "cuda";
  const auto conversationBinding = m_impl->conversationBindingBySession.find(
    effectiveContext.sessionId);
  const bool restoringConversation = deviceResidentState &&
    conversationBinding != m_impl->conversationBindingBySession.end();
  if (m_impl->statefulIo && effectiveContext.sessionId != "native-runtime-warmup" &&
      effectiveContext.inferenceEpoch == 0) {
    ++m_impl->runtimeMetrics.stateRecomputes;
  }
  if (deviceResidentState && effectiveContext.inferenceEpoch == 0 &&
      !restoringConversation) {
    // Session reuse is allowed, but state is request-scoped and must not cross
    // a fresh generation boundary.
    m_impl->eraseSessionStateLocked(effectiveContext.sessionId);
  }
  std::map<std::string, const Ort::Value*> boundDeviceInputs;
  std::map<std::string, std::size_t> cpuInputIndexes;

  std::vector<std::vector<std::uint8_t>> inputBuffers;
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
    const bool isStateInput = m_impl->statefulIo &&
      std::find(m_impl->statefulIo->stateInputNames.begin(),
                m_impl->statefulIo->stateInputNames.end(), inputName) !=
        m_impl->statefulIo->stateInputNames.end();
    if (deviceResidentState && isStateInput &&
        (effectiveContext.inferenceEpoch > 0 || restoringConversation)) {
      const std::map<std::string, Ort::Value>* stateMap = nullptr;
      const auto sessionState = m_impl->deviceStateBySession.find(
        effectiveContext.sessionId);
      if (effectiveContext.inferenceEpoch > 0 &&
          sessionState != m_impl->deviceStateBySession.end()) {
        stateMap = &sessionState->second;
      }
      else if (restoringConversation) {
        const auto retained = m_impl->deviceStateByConversation.find(
          conversationBinding->second);
        if (retained != m_impl->deviceStateByConversation.end()) {
          stateMap = &retained->second;
        }
      }
      if (stateMap != nullptr) {
        const auto deviceState = stateMap->find(inputName);
        if (deviceState != stateMap->end()) {
          // Use the previous output directly on the CUDA device.  The host
          // state bundle is intentionally not materialized on this path.
          boundDeviceInputs.emplace(inputName, &deviceState->second);
          ++m_impl->runtimeMetrics.stateInputHits;
          continue;
        }
      }
      // A streamed CUDA epoch must consume the exact device-resident
      // predecessor produced by the previous epoch.  ``runStreamedImpl``
      // seeds the first epoch's zero-state entries in the request context;
      // allowing those entries to satisfy a later lookup would silently
      // restart the recurrent/KV state after an eviction or failed transfer.
      // Fail closed before inputBundleFor() can see that bootstrap value.
      ++m_impl->runtimeMetrics.stateInputMisses;
      throw std::runtime_error(
        "stateful ONNX decode is missing Provider-owned device predecessor state: " +
        inputName);
    }
    TensorBundle zeroState;
    const TensorBundle* inputBundle = nullptr;
    const auto causalInput = causalPositionInputs.find(inputName);
    if (causalInput != causalPositionInputs.end()) {
      inputBundle = &causalInput->second;
    }
    else {
      try {
        inputBundle = &inputBundleFor(effectiveContext, m_spec, inputName, i);
      }
      catch (const std::out_of_range&) {
        if (!isStateInput) {
          throw;
        }
        if (effectiveContext.inferenceEpoch > 0) {
          throw std::runtime_error(
            "stateful ONNX decode is missing Provider-owned predecessor state: " +
            inputName);
        }
        NamedTensor state;
        state.name = inputName;
        state.elementType = fromOnnxElementType(tensorInfo.GetElementType());
        state.shape = inputShapes.back();
        state.payload.assign(
          elementCount(state.shape) * tensorElementByteSize(state.elementType), 0);
        zeroState = makeEncodedTensorBundle(inputName, {std::move(state)});
        inputBundle = &zeroState;
      }
    }
    auto tensor = tensorForInput(*inputBundle, inputName, inputShapes.back());
    validateNamedTensor(tensor);
    const auto onnxType = toOnnxElementType(tensor.elementType);
    if (tensorInfo.GetElementType() != onnxType) {
      throw std::invalid_argument("ONNX Runtime input dtype mismatch for " + inputName);
    }
    if (!tensor.shape.empty()) {
      inputShapes.back() = tensor.shape;
    }
    inputBuffers.push_back(std::move(tensor.payload));
    const auto expected = elementCount(inputShapes.back());
    if (inputBuffers.back().size() != expected * tensorElementByteSize(tensor.elementType)) {
      throw std::invalid_argument(
        "ONNX Runtime input byte count mismatch for " + inputName);
    }
    const auto inputBytes = static_cast<std::uint64_t>(inputBuffers.back().size());
    if (isStateInput) {
      if (deviceResidentState) {
        // Initial/host state is uploaded by ORT. Successor state bound directly
        // from deviceStateBySession is device-to-device and was counted above.
        m_impl->runtimeMetrics.stateHostToDeviceBytes += inputBytes;
      }
    }
    else {
      m_impl->runtimeMetrics.activationInputBytes += inputBytes;
      if (deviceResidentState) {
        m_impl->runtimeMetrics.activationHostToDeviceBytes += inputBytes;
      }
    }
    if (isControlInputName(inputName)) {
      m_impl->runtimeMetrics.controlBytes += inputBytes;
    }

    inputNamePtrs.push_back(inputName.c_str());
    inputValues.push_back(Ort::Value::CreateTensor(
      memoryInfo,
      inputBuffers.back().data(),
      inputBuffers.back().size(),
      inputShapes.back().data(),
      inputShapes.back().size(),
      onnxType));
    cpuInputIndexes.emplace(inputName, inputValues.size() - 1);
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
  if (m_impl->statefulIo) {
    outputNames = m_impl->statefulIo->outputNames;
  }

  const auto runStart = std::chrono::steady_clock::now();
  std::vector<Ort::Value> outputs;
  std::optional<Ort::IoBinding> ioBinding;
  std::optional<Ort::MemoryInfo> cudaMemoryInfo;
  std::size_t deviceStateOutputsBound = 0;
  if (deviceResidentState) {
    ioBinding.emplace(m_impl->session);
    cudaMemoryInfo.emplace(
      "Cuda", OrtAllocatorType::OrtArenaAllocator,
      std::stoi(m_impl->selection.deviceId), OrtMemTypeDefault);
    for (const auto& name : inputNames) {
      const auto deviceInput = boundDeviceInputs.find(name);
      if (deviceInput != boundDeviceInputs.end()) {
        ioBinding->BindInput(name.c_str(), *deviceInput->second);
        continue;
      }
      const auto cpuInput = cpuInputIndexes.find(name);
      if (cpuInput == cpuInputIndexes.end()) {
        throw std::runtime_error("missing prepared ONNX Runtime input: " + name);
      }
      ioBinding->BindInput(name.c_str(), inputValues[cpuInput->second]);
    }
    for (const auto& name : outputNames) {
      const bool stateOutput = std::find(
        m_impl->statefulIo->stateOutputNames.begin(),
        m_impl->statefulIo->stateOutputNames.end(), name) !=
          m_impl->statefulIo->stateOutputNames.end();
      if (stateOutput) {
        // Let ORT allocate state outputs on CUDA.  The resulting Ort::Value is
        // retained in the session map and rebound on the next epoch.
        ioBinding->BindOutput(name.c_str(), cudaMemoryInfo->GetConst());
        ++deviceStateOutputsBound;
      }
      else {
        ioBinding->BindOutput(name.c_str(), memoryInfo.GetConst());
      }
    }
    m_impl->session.Run(Ort::RunOptions{nullptr}, *ioBinding);
    ioBinding->SynchronizeOutputs();
    outputs = ioBinding->GetOutputValues();
  }
  else {
    std::vector<const char*> outputNamePtrs;
    outputNamePtrs.reserve(outputNames.size());
    for (const auto& name : outputNames) {
      outputNamePtrs.push_back(name.c_str());
    }
    outputs = m_impl->session.Run(
      Ort::RunOptions{nullptr},
      inputNamePtrs.data(),
      inputValues.data(),
      inputValues.size(),
      outputNamePtrs.data(),
      outputNamePtrs.size());
  }
  const auto runDone = std::chrono::steady_clock::now();
  const bool captureRequestProfile =
    !runnerMetadataBool(m_spec, {"profileAfterRequest"}) ||
    (!ctx.requestId.empty() && ctx.attemptEpoch != 0);
  if (m_impl->profilingEnabled && captureRequestProfile &&
      !m_impl->profilingCaptured.load(std::memory_order_relaxed) && m_evidence) {
    auto profile = m_impl->session.EndProfilingAllocated(allocator);
    const std::string profilePath(profile.get());
    applyOnnxRuntimeProviderProfile(
      *m_evidence,
      profilePath,
      m_spec.role,
      m_impl->selection.usedCpuFallback,
      m_evidence->gpuUuid);
    m_evidence->profileRequestId = ctx.requestId;
    m_evidence->profileAttemptEpoch = ctx.attemptEpoch;
    m_impl->profilingCaptured.store(true, std::memory_order_release);
  }
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
  std::vector<std::vector<std::uint8_t>> outputHostBuffers;
  outputHostBuffers.reserve(outputs.size());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    auto& value = outputs[i];
    if (!value.IsTensor()) {
      throw std::runtime_error("ONNX Runtime output is not a tensor");
    }
    auto tensorInfo = value.GetTensorTypeAndShapeInfo();
    const auto elementType = fromOnnxElementType(tensorInfo.GetElementType());
    const auto count = tensorInfo.GetElementCount();
    const auto outputBytes = static_cast<std::uint64_t>(
      count * tensorElementByteSize(elementType));
    const bool stateOutput = deviceResidentState &&
      std::find(m_impl->statefulIo->stateOutputNames.begin(),
                m_impl->statefulIo->stateOutputNames.end(), outputNames[i]) !=
        m_impl->statefulIo->stateOutputNames.end();
    const void* hostData = value.GetTensorRawData();
    if (stateOutput) {
      // The next epoch asks for the corresponding *_in tensor.  Store the
      // device allocation under that successor input name; retaining it under
      // *_out makes every lookup miss and silently reintroduces a host upload.
      const auto successorInput =
        m_impl->statefulIo->stateInputForOutput(outputNames[i]);
      m_impl->deviceStateBySession[effectiveContext.sessionId].insert_or_assign(
        successorInput, std::move(value));
      if (effectiveContext.streamingStateExecution) {
        // A streamed decode keeps the complete state on the CUDA device
        // between token epochs.  Do not materialize a host copy for every
        // token; runStreamedImpl exports the terminal state once, after the
        // event/response boundary has accepted the final token.
        continue;
      }
      outputHostBuffers.emplace_back(
        count * tensorElementByteSize(elementType), 0);
      copyOrtTensorToHost(
        m_impl->deviceStateBySession[effectiveContext.sessionId]
          .at(successorInput), outputHostBuffers.back().data(),
        outputHostBuffers.back().size());
      m_impl->runtimeMetrics.stateDeviceToHostBytes += outputBytes;
      hostData = outputHostBuffers.back().data();
    }
    else {
      m_impl->runtimeMetrics.activationOutputBytes += outputBytes;
      if (deviceResidentState) {
        m_impl->runtimeMetrics.activationDeviceToHostBytes += outputBytes;
      }
    }
    const auto* data = static_cast<const std::uint8_t*>(hostData);
    NamedTensor tensor;
    tensor.name = metadataValue(
      m_spec,
      {"outputAlias." + outputNames[i], "output_alias." + outputNames[i]});
    if (tensor.name.empty()) {
      tensor.name = outputNames[i];
    }
    tensor.elementType = elementType;
    tensor.shape = tensorInfo.GetShape();
    tensor.payload.assign(data, data + count * tensorElementByteSize(elementType));
    validateNamedTensor(tensor);
    namedOutputs.push_back(std::move(tensor));
  }
  for (const auto& name : metadataNames(
         m_spec, {"passthroughTensors", "passthrough_tensors"})) {
    const auto duplicate = std::find_if(
      namedOutputs.begin(), namedOutputs.end(), [&name] (const NamedTensor& tensor) {
        return tensor.name == name;
      });
    if (duplicate == namedOutputs.end()) {
      namedOutputs.push_back(passthroughTensorFor(effectiveContext, name));
    }
  }

  // Atomic YOLO execution owns the complete ONNX graph, but the graph's
  // [1,300,6] predictions are still an intermediate result. Apply the
  // adapter-certified terminal contract before the response-size guard sees
  // the payload. Shared candidates use the standalone native Merge runner;
  // this branch preserves real ORT execution evidence for FullModel.
  const auto postprocessKind = metadataValue(
    m_spec, {"mergeKind", "merge_kind"});
  if (postprocessKind == "ONNX_POSTPROCESS") {
    const auto identity = metadataValue(
      m_spec, {"postprocessIdentity", "postprocess_identity"});
    const auto outputName = metadataValue(
      m_spec, {"postprocessOutputName", "postprocess_output_name"});
    const auto sort = metadataValue(m_spec, {"postprocessSort", "postprocess_sort"});
    const auto thresholdText = metadataValue(
      m_spec, {"postprocessConfidenceThreshold", "postprocess_confidence_threshold"});
    const auto maxRowsText = metadataValue(
      m_spec, {"postprocessMaxRows", "postprocess_max_rows"});
    if (identity != "YOLO26n-canonical-detection-rows" || outputName.empty() ||
        sort != "confidence-desc,class-asc,xyxy-asc" || thresholdText.empty() ||
        maxRowsText.empty()) {
      throw std::invalid_argument("ONNX postprocessing contract is incomplete");
    }
    std::size_t thresholdConsumed = 0;
    const auto threshold = std::stod(thresholdText, &thresholdConsumed);
    if (thresholdConsumed != thresholdText.size() || !std::isfinite(threshold) ||
        threshold < 0.0 || threshold > 1.0) {
      throw std::invalid_argument("ONNX postprocessing confidence threshold is invalid");
    }
    std::size_t rowsConsumed = 0;
    const auto maxRows = std::stoull(maxRowsText, &rowsConsumed);
    if (rowsConsumed != maxRowsText.size() || maxRows == 0) {
      throw std::invalid_argument("ONNX postprocessing row budget is invalid");
    }
    const auto output = std::find_if(
      namedOutputs.begin(), namedOutputs.end(), [&outputName] (const NamedTensor& tensor) {
        return tensor.name == outputName;
      });
    if (output == namedOutputs.end()) {
      throw std::invalid_argument("ONNX postprocessing output tensor is missing");
    }
    *output = nativeYoloCanonicalizePredictions(
      *output, outputName, threshold, static_cast<std::size_t>(maxRows));
  }

  std::map<std::string, TensorBundle> result;
  const bool forceEncodedOutput =
    effectiveContext.streamingStateExecution ||
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
      namedOutputs.push_back(std::move(padding));
    }
    TensorBundle bundle = makeEncodedTensorBundle("onnx-output-bundle", namedOutputs);
    const auto bundleScope = metadataValue(
      m_spec,
      {"outputBundleScope", "output_bundle_scope", "outputScope", "output_scope"});
    result.emplace(bundleScope.empty() ? "onnx-output-bundle" : bundleScope,
                   std::move(bundle));
  }
  if (deviceResidentState && effectiveContext.streamingStateExecution) {
    // The Provider decode-state transaction still needs an exact predecessor
    // record, but the complete CUDA allocation must not cross the host/NDN
    // boundary. Return only a small opaque handle; the next epoch ignores its
    // bytes and rebinds the retained Ort::Value from deviceStateBySession.
    NativeOpaqueStateHandleV1 stateHandle;
    stateHandle.providerIdentity = runnerMetadataValue(
      m_spec,
      {"providerIdentity", "provider_identity", "evidence.providerName",
       "providerName", "provider_name"});
    stateHandle.providerBootId = runnerMetadataValue(
      m_spec,
      {"providerBootId", "provider_boot_id", "evidence.providerBootId"});
    stateHandle.sessionId = effectiveContext.sessionId;
    stateHandle.role = effectiveContext.role.empty() ? m_spec.role : effectiveContext.role;
    stateHandle.token = std::string("ndnsf-device-state-v1:") +
      effectiveContext.sessionId + ":" + stateHandle.role + ":" +
      std::to_string(effectiveContext.inferenceEpoch);
    stateHandle.stateInferenceEpoch = effectiveContext.inferenceEpoch;
    stateHandle.validate();
    m_impl->stateHandleBySession[effectiveContext.sessionId] = stateHandle;

    std::vector<NamedTensor> handles;
    handles.reserve(m_impl->statefulIo->stateOutputNames.size());
    for (const auto& outputName : m_impl->statefulIo->stateOutputNames) {
      const auto successorInput =
        m_impl->statefulIo->stateInputForOutput(outputName);
      const auto handle = stateHandle.token + ":" + successorInput;
      handles.push_back(NamedTensor{
        outputName, TensorElementType::UInt8,
        {static_cast<std::int64_t>(handle.size())},
        std::vector<std::uint8_t>(handle.begin(), handle.end())});
      m_impl->runtimeMetrics.controlBytes += handle.size();
    }
    result.emplace(
      "__ndnsf_provider_decode_state",
      makeEncodedTensorBundle("__ndnsf_provider_decode_state", std::move(handles)));
  }
  const auto kvOutputNames = metadataNames(
    m_spec, {"kvOutputTensors", "kv_output_tensors"});
  if (!kvOutputNames.empty()) {
    auto kvTensors = selectTensors(namedOutputs, kvOutputNames);
    const auto kvScope = metadataValue(
      m_spec, {"kvOutputScope", "kv_output_scope"});
    result.emplace(
      kvScope.empty() ? "kv-state" : kvScope,
      makeEncodedTensorBundle("kv-state", kvTensors));
  }
  const auto packageDone = std::chrono::steady_clock::now();
  if (runtimeTimingEnabled()) {
    std::ostringstream record;
    record << std::fixed << std::setprecision(3)
           << "NDNSF_DI_ONNX_TIMING"
           << " session=" << ctx.sessionId
           << " role=" << ctx.role
           << " collect_ms=" << elapsedMs(collectStart, runStart)
           << " session_ms=0"
           << " run_ms=" << elapsedMs(runStart, runDone)
           << " delay_ms=" << elapsedMs(runDone, delayDone)
           << " publish_ms=" << elapsedMs(delayDone, packageDone)
           << " session_cache=hit"
           << " state_io_binding=" << (deviceResidentState ? "cuda" : "host")
           << " state_device_inputs=" << boundDeviceInputs.size()
           << " state_device_outputs=" << deviceStateOutputsBound
           << " state_d2h_bytes=" << m_impl->runtimeMetrics.stateDeviceToHostBytes
           << " state_h2d_bytes=" << m_impl->runtimeMetrics.stateHostToDeviceBytes
           << " activation_input_bytes=" << m_impl->runtimeMetrics.activationInputBytes
           << " activation_output_bytes=" << m_impl->runtimeMetrics.activationOutputBytes
           << " activation_h2d_bytes=" << m_impl->runtimeMetrics.activationHostToDeviceBytes
           << " activation_d2h_bytes=" << m_impl->runtimeMetrics.activationDeviceToHostBytes
           << " control_bytes=" << m_impl->runtimeMetrics.controlBytes
           << " state_hits=" << m_impl->runtimeMetrics.stateInputHits
           << " state_misses=" << m_impl->runtimeMetrics.stateInputMisses
           << " state_recomputes=" << m_impl->runtimeMetrics.stateRecomputes
           << " state_releases=" << m_impl->runtimeMetrics.stateReleases;
    logRuntimeEvidence(record.str());
  }
  return result;
}

std::optional<std::map<std::string, TensorBundle>>
OnnxRuntimeModelRunner::runStreamedImpl(const RoleExecutionContext& ctx)
{
  if (!runnerMetadataBool(m_spec, {"streamingGeneration", "streaming_generation"}) ||
      !ctx.streamEventSink) {
    return std::nullopt;
  }
  if (!m_impl->statefulIo) {
    throw std::invalid_argument(
      "streaming ONNX generation requires a declared stateful I/O contract");
  }
  if (ctx.generationLineage) {
    throw std::invalid_argument(
      "authenticated epoch lineage must be driven by NativeEpochCoordinator; "
      "runStreamed() cannot own a coordinator generation loop");
  }

  const auto tokenInputName = runnerMetadataValue(
    m_spec, {"streamTokenInput", "stream_token_input", "tokenInputName"}).empty()
    ? std::string("input_ids")
    : runnerMetadataValue(
        m_spec, {"streamTokenInput", "stream_token_input", "tokenInputName"});
  const auto maxTokens = metadataSizeValue(
    m_spec, {"maxGeneratedTokens", "max_generated_tokens"}, 64);
  if (maxTokens == 0) {
    throw std::invalid_argument("streaming ONNX generation token bound is zero");
  }
  const auto eosText = runnerMetadataValue(
    m_spec, {"eosTokenIds", "eos_token_ids"});
  std::set<std::int64_t> eosIds;
  if (!eosText.empty()) {
    std::stringstream eosStream(eosText);
    std::string item;
    while (std::getline(eosStream, item, ',')) {
      if (!item.empty()) {
        try {
          eosIds.insert(std::stoll(item));
        }
        catch (const std::exception&) {
          throw std::invalid_argument("invalid streaming ONNX EOS token ID: " + item);
        }
      }
    }
  }
  const auto samplingDigest = runnerMetadataValue(
    m_spec, {"samplingDigest", "sampling_digest"});
  if (samplingDigest.empty()) {
    throw std::invalid_argument(
      "streaming ONNX generation requires samplingDigest metadata");
  }
  if (!isSha256Digest(samplingDigest)) {
    throw std::invalid_argument(
      "streaming ONNX generation requires a canonical sha256 samplingDigest");
  }

  RoleExecutionContext epochContext = ctx;
  const bool deviceResidentState =
    m_impl->selection.selectedProvider == "cuda";
  struct DeviceStateCleanup
  {
    Impl* impl = nullptr;
    std::string sessionId;

    ~DeviceStateCleanup()
    {
      if (impl == nullptr || sessionId.empty()) {
        return;
      }
      std::lock_guard<std::mutex> lock(impl->executionMutex);
      impl->eraseSessionStateLocked(sessionId);
    }
  } deviceStateCleanup{
    deviceResidentState ? m_impl.get() : nullptr,
    deviceResidentState ? ctx.sessionId : std::string(),
  };
  epochContext.streamingStateExecution = deviceResidentState;
  const auto makeZeroState = [this] (const std::string& name) {
    Ort::AllocatorWithDefaultOptions allocator;
    std::size_t inputIndex = 0;
    for (; inputIndex < m_impl->session.GetInputCount(); ++inputIndex) {
      auto allocated = m_impl->session.GetInputNameAllocated(inputIndex, allocator);
      if (name == allocated.get()) {
        break;
      }
    }
    if (inputIndex == m_impl->session.GetInputCount()) {
      throw std::invalid_argument("streaming ONNX state input is not in graph: " + name);
    }
    auto typeInfo = m_impl->session.GetInputTypeInfo(inputIndex);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    const auto elementType = fromOnnxElementType(tensorInfo.GetElementType());
    const auto shape = shapeForInput(
      m_spec, name, inputIndex, tensorInfo.GetShape());
    NamedTensor tensor;
    tensor.name = name;
    tensor.elementType = elementType;
    tensor.shape = shape;
    tensor.payload.assign(
      elementCount(shape) * tensorElementByteSize(elementType), 0);
    return makeEncodedTensorBundle(name, {std::move(tensor)});
  };
  for (const auto& stateInput : m_impl->statefulIo->stateInputNames) {
    if (epochContext.inputsByScope.find(stateInput) == epochContext.inputsByScope.end()) {
      epochContext.inputsByScope.emplace(stateInput, makeZeroState(stateInput));
    }
  }

  std::vector<std::int64_t> generated;
  std::map<std::string, TensorBundle> finalOutputs;
  std::string finishHint = "MAX_TOKENS";
  for (std::size_t tokenEpoch = 1; tokenEpoch <= maxTokens; ++tokenEpoch) {
    // The first iteration is prefill (state epoch 0); each subsequent
    // iteration is a one-token decode that must consume the preceding
    // Provider-owned state epoch.  Keeping this field explicit is what lets
    // the CUDA IoBinding path distinguish a fresh request from a continuation.
    epochContext.inferenceEpoch = tokenEpoch - 1;
    const auto outputs = run(epochContext);
    auto outputBundle = outputs.find("onnx-output-bundle");
    if (outputBundle == outputs.end()) {
      outputBundle = std::find_if(
        outputs.begin(), outputs.end(), [] (const auto& item) {
          return isEncodedTensorBundle(item.second.payload);
        });
    }
    if (outputBundle == outputs.end()) {
      throw std::runtime_error("streaming ONNX runner returned no encoded output bundle");
    }
    const auto tensors = decodeTensorBundle(outputBundle->second.payload);
    const auto& logits = findTensor(tensors, "logits");
    if (logits.elementType != TensorElementType::Float32 || logits.shape.size() < 2) {
      throw std::invalid_argument(
        "streaming ONNX runner requires float32 logits with a sequence dimension");
    }
    const auto vocabulary = static_cast<std::size_t>(logits.shape.back());
    if (vocabulary == 0 || logits.payload.size() % (vocabulary * sizeof(float)) != 0) {
      throw std::invalid_argument("streaming ONNX logits shape is invalid");
    }
    const auto sequence = logits.payload.size() / (vocabulary * sizeof(float));
    const auto* values = reinterpret_cast<const float*>(logits.payload.data());
    const auto* begin = values + (sequence - 1) * vocabulary;
    const auto* best = std::max_element(begin, begin + vocabulary);
    const auto tokenId = static_cast<std::int64_t>(std::distance(begin, best));
    generated.push_back(tokenId);

    std::ostringstream prefix;
    for (std::size_t index = 0; index < generated.size(); ++index) {
      if (index != 0) prefix << ',';
      prefix << generated[index];
    }
    ndn::util::Sha256 prefixDigest;
    prefixDigest << prefix.str();
    const bool eos = eosIds.count(tokenId) != 0;
    const bool atMax = tokenEpoch == maxTokens;
    finishHint = eos ? "EOS" : atMax ? "MAX_TOKENS" : "NONE";
    std::ostringstream event;
    event << "{\"schema\":\"GenerationTokenEventV1\","
          << "\"tokenId\":" << tokenId
          << ",\"tokenEpoch\":" << tokenEpoch
          << ",\"cumulativeTokenCount\":" << tokenEpoch
          << ",\"textDelta\":\"\","
          << "\"finishHint\":\"" << finishHint << "\","
          << "\"samplingDigest\":\"" << samplingDigest << "\","
          << "\"acceptedPrefixDigest\":\"sha256:" << prefixDigest.toString()
          << "\"}";
    const auto eventText = event.str();
    if (!ctx.streamEventSink(
          std::vector<std::uint8_t>(eventText.begin(), eventText.end()))) {
      throw std::runtime_error("streaming ONNX event admission was rejected");
    }

    if (eos || atMax) {
      break;
    }

    if (!deviceResidentState) {
      for (const auto& stateOutput : m_impl->statefulIo->stateOutputNames) {
        const auto& tensor = findTensor(tensors, stateOutput);
        if (stateOutput.size() <= 4 ||
            stateOutput.compare(stateOutput.size() - 4, 4, "_out") != 0) {
          throw std::invalid_argument("streaming ONNX state output is not suffixed _out");
        }
        const auto nextInput = stateOutput.substr(0, stateOutput.size() - 4) + "_in";
        epochContext.inputsByScope[nextInput] = makeEncodedTensorBundle(
          nextInput,
          {NamedTensor{nextInput, tensor.elementType, tensor.shape, tensor.payload}});
      }
    }
    std::vector<std::uint8_t> tokenBytes(sizeof(tokenId));
    std::memcpy(tokenBytes.data(), &tokenId, sizeof(tokenId));
    epochContext.inputsByScope[tokenInputName] = makeEncodedTensorBundle(
      tokenInputName,
      {NamedTensor{tokenInputName, TensorElementType::Int64, {1, 1},
                   std::move(tokenBytes)}});
  }

  if (deviceResidentState) {
    std::lock_guard<std::mutex> stateLock(m_impl->executionMutex);
    const auto found = m_impl->deviceStateBySession.find(ctx.sessionId);
    if (found == m_impl->deviceStateBySession.end()) {
      throw std::runtime_error(
        "streaming ONNX generation completed without device state");
    }
    Ort::AllocatorWithDefaultOptions allocator;
    const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
      OrtAllocatorType::OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<NamedTensor> stateTensors;
    stateTensors.reserve(m_impl->statefulIo->stateOutputNames.size());
    for (const auto& outputName : m_impl->statefulIo->stateOutputNames) {
      const auto inputName = m_impl->statefulIo->stateInputForOutput(outputName);
      const auto state = found->second.find(inputName);
      if (state == found->second.end() || !state->second.IsTensor()) {
        throw std::runtime_error(
          "streaming ONNX generation completed without device state: " +
          inputName);
      }
      const auto info = state->second.GetTensorTypeAndShapeInfo();
      const auto elementType = fromOnnxElementType(info.GetElementType());
      const auto count = info.GetElementCount();
      std::vector<std::uint8_t> host(count * tensorElementByteSize(elementType));
      const auto shape = info.GetShape();
      copyOrtTensorToHost(state->second, host.data(), host.size());
      m_impl->runtimeMetrics.stateDeviceToHostBytes += host.size();
      stateTensors.push_back(NamedTensor{
        outputName, elementType, shape, std::move(host)});
    }
    finalOutputs.emplace(
      "onnx-state-bundle",
      makeEncodedTensorBundle("onnx-state-bundle", std::move(stateTensors)));
  }

  const auto finalText = [&] {
    std::ostringstream text;
    text << "{\"schema\":\"NDNSF-DI-FINAL-V1\",\"finishHint\":\""
         << finishHint << "\",\"tokenIds\":[";
    for (std::size_t index = 0; index < generated.size(); ++index) {
      if (index != 0) text << ',';
      text << generated[index];
    }
    text << "]}";
    return text.str();
  }();
  const auto finalBytes = std::vector<std::uint8_t>(finalText.begin(), finalText.end());
  finalOutputs.emplace(
    "final-response",
    TensorBundle{"final-response", finalBytes, 1, finalBytes.size()});
  return finalOutputs;
}

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  const auto creator = [] (const NativeModelRunnerSpec& spec) {
    return std::make_shared<OnnxRuntimeModelRunner>(spec);
  };
  // Keep the public adapter backend names distinct from the internal
  // execution-provider selection stored in runner metadata.
  factory.registerBackend("onnxruntime", creator);
  factory.registerBackend("onnxruntime-cpu", creator);
  factory.registerBackend("onnxruntime-cuda", creator);
}

const std::optional<ExecutionEvidence>&
OnnxRuntimeModelRunner::executionEvidence() const
{
  return m_evidence;
}

std::optional<ExecutionEvidence>
OnnxRuntimeModelRunner::executionEvidenceSnapshot() const
{
  std::lock_guard<std::mutex> lock(m_impl->profileMutex);
  return m_evidence;
}

std::optional<NativeRuntimeMetrics>
OnnxRuntimeModelRunner::runtimeMetricsSnapshot() const
{
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  return m_impl->runtimeMetrics;
}

bool
OnnxRuntimeModelRunner::supportsOpaqueStateHandles() const
{
  return m_impl != nullptr && m_impl->statefulIo.has_value() &&
         m_impl->selection.selectedProvider == "cuda";
}

std::optional<NativeOpaqueStateHandleV1>
OnnxRuntimeModelRunner::stateHandleSnapshot(const std::string& sessionId) const
{
  if (!m_impl || sessionId.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto found = m_impl->stateHandleBySession.find(sessionId);
  if (found == m_impl->stateHandleBySession.end()) {
    return std::nullopt;
  }
  return found->second;
}

bool
OnnxRuntimeModelRunner::supportsConversationStateTransfer() const
{
  return m_impl != nullptr && m_impl->statefulIo.has_value() &&
         m_impl->selection.selectedProvider == "cuda";
}

std::optional<NativeConversationStateHandleV1>
OnnxRuntimeModelRunner::promoteSessionStateToConversation(
  const std::string& sessionId,
  const std::string& conversationKey)
{
  if (!supportsConversationStateTransfer() || sessionId.empty() ||
      conversationKey.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto found = m_impl->deviceStateBySession.find(sessionId);
  if (found == m_impl->deviceStateBySession.end() || found->second.empty() ||
      m_impl->deviceStateByConversation.count(conversationKey) != 0 ||
      m_impl->hostStateByConversation.count(conversationKey) != 0) {
    return std::nullopt;
  }

  std::size_t logicalBytes = 0;
  for (const auto& item : found->second) {
    if (!item.second.IsTensor()) {
      return std::nullopt;
    }
    const auto info = item.second.GetTensorTypeAndShapeInfo();
    const auto bytes = static_cast<std::size_t>(
      info.GetElementCount() *
      tensorElementByteSize(fromOnnxElementType(info.GetElementType())));
    if (bytes == 0 || logicalBytes > std::numeric_limits<std::size_t>::max() - bytes) {
      return std::nullopt;
    }
    logicalBytes += bytes;
  }

  NativeOpaqueStateHandleV1 opaque;
  const auto prior = m_impl->stateHandleBySession.find(sessionId);
  if (prior != m_impl->stateHandleBySession.end()) {
    opaque = prior->second;
  }
  else {
    opaque.providerIdentity = runnerMetadataValue(
      m_spec,
      {"providerIdentity", "provider_identity", "evidence.providerName",
       "providerName", "provider_name"});
    opaque.providerBootId = runnerMetadataValue(
      m_spec,
      {"providerBootId", "provider_boot_id", "evidence.providerBootId"});
    opaque.sessionId = sessionId;
    opaque.role = m_spec.role;
    opaque.token = std::string("ndnsf-device-state-v1:") + sessionId + ":" +
      m_spec.role + ":conversation";
    opaque.stateInferenceEpoch = 0;
  }
  opaque.sessionId = sessionId;
  opaque.role = m_spec.role;
  try {
    opaque.validate();
  }
  catch (const std::exception&) {
    return std::nullopt;
  }

  NativeConversationStateHandleV1 result;
  result.opaque = opaque;
  result.conversationKey = conversationKey;
  result.logicalBytes = logicalBytes;
  try {
    result.validate();
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
  m_impl->deviceStateByConversation.emplace(
    conversationKey, std::move(found->second));
  m_impl->deviceStateBySession.erase(found);
  m_impl->stateHandleByConversation[conversationKey] = result;
  m_impl->stateHandleBySession.erase(sessionId);
  return result;
}

bool
OnnxRuntimeModelRunner::restoreConversationState(
  const NativeConversationStateHandleV1& state,
  const std::string& sessionId)
{
  if (!m_impl || sessionId.empty()) {
    return false;
  }
  try {
    state.validate();
  }
  catch (const std::exception&) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto retained = m_impl->deviceStateByConversation.find(state.conversationKey);
  if (retained == m_impl->deviceStateByConversation.end() ||
      retained->second.empty() ||
      state.opaque.providerIdentity != runnerMetadataValue(
        m_spec, {"providerIdentity", "provider_identity", "evidence.providerName",
                 "providerName", "provider_name"}) ||
      state.opaque.providerBootId != runnerMetadataValue(
        m_spec, {"providerBootId", "provider_boot_id", "evidence.providerBootId"}) ||
      state.opaque.role != m_spec.role) {
    return false;
  }
  m_impl->conversationBindingBySession[sessionId] = state.conversationKey;
  auto sessionHandle = state.opaque;
  sessionHandle.sessionId = sessionId;
  m_impl->stateHandleBySession[sessionId] = std::move(sessionHandle);
  return true;
}

bool
OnnxRuntimeModelRunner::pauseConversationStateToHost(
  const NativeConversationStateHandleV1& state)
{
  if (!m_impl) {
    return false;
  }
  try {
    state.validate();
  }
  catch (const std::exception&) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto found = m_impl->deviceStateByConversation.find(state.conversationKey);
  if (found == m_impl->deviceStateByConversation.end() || found->second.empty()) {
    return false;
  }
  Impl::HostStateMap host;
  std::size_t totalBytes = 0;
  try {
    for (const auto& item : found->second) {
      const auto info = item.second.GetTensorTypeAndShapeInfo();
      const auto elementType = fromOnnxElementType(info.GetElementType());
      const auto bytes = static_cast<std::size_t>(
        info.GetElementCount() * tensorElementByteSize(elementType));
      if (bytes == 0) {
        return false;
      }
      Impl::HostStateTensor tensor;
      tensor.elementType = elementType;
      tensor.shape = info.GetShape();
      tensor.payload.resize(bytes);
      copyOrtTensorToHost(item.second, tensor.payload.data(), bytes);
      totalBytes += bytes;
      host.emplace(item.first, std::move(tensor));
    }
  }
  catch (const std::exception&) {
    return false;
  }
  if (totalBytes != state.logicalBytes) {
    return false;
  }
  m_impl->deviceStateByConversation.erase(found);
  auto allocations = m_impl->externalDeviceAllocationsByConversation.find(
    state.conversationKey);
  if (allocations != m_impl->externalDeviceAllocationsByConversation.end()) {
    CudaRuntimeApi cuda;
    freeCudaPointers(cuda, allocations->second);
    m_impl->externalDeviceAllocationsByConversation.erase(allocations);
  }
  m_impl->hostStateByConversation[state.conversationKey] = std::move(host);
  m_impl->runtimeMetrics.stateDeviceToHostBytes += totalBytes;
  return true;
}

std::future<bool>
OnnxRuntimeModelRunner::prefetchConversationStateToGpu(
  const NativeConversationStateHandleV1& state)
{
  state.validate();
  return std::async(
    std::launch::async,
    [this, state] {
      if (!m_impl || m_impl->selection.selectedProvider != "cuda") {
        return false;
      }
      std::lock_guard<std::mutex> lock(m_impl->executionMutex);
      if (m_impl->deviceStateByConversation.count(state.conversationKey) != 0) {
        return true;
      }
      const auto host = m_impl->hostStateByConversation.find(state.conversationKey);
      if (host == m_impl->hostStateByConversation.end() || host->second.empty()) {
        return false;
      }
      CudaRuntimeApi cuda;
      if (!cuda.available()) {
        return false;
      }
      int deviceId = 0;
      try {
        deviceId = std::stoi(m_impl->selection.deviceId);
      }
      catch (const std::exception&) {
        return false;
      }
      Ort::MemoryInfo memoryInfo(
        "Cuda", OrtAllocatorType::OrtDeviceAllocator,
        deviceId, OrtMemTypeDefault);
      std::map<std::string, Ort::Value> device;
      std::map<std::string, void*> allocations;
      std::size_t totalBytes = 0;
      try {
        for (const auto& item : host->second) {
          if (item.second.payload.empty() || item.second.shape.empty()) {
            throw std::runtime_error("empty conversation state tensor");
          }
          void* raw = nullptr;
          if (cuda.malloc(&raw, item.second.payload.size()) != 0 || raw == nullptr) {
            throw std::runtime_error("cudaMalloc failed for conversation state");
          }
          allocations.emplace(item.first, raw);
          if (cuda.memcpy(raw, item.second.payload.data(),
                          item.second.payload.size(), cudaMemcpyHostToDevice) != 0) {
            throw std::runtime_error("cudaMemcpy H2D failed for conversation state");
          }
          auto value = Ort::Value::CreateTensor(
            memoryInfo, raw, item.second.payload.size(),
            item.second.shape.data(), item.second.shape.size(),
            toOnnxElementType(item.second.elementType));
          device.emplace(item.first, std::move(value));
          totalBytes += item.second.payload.size();
        }
      }
      catch (const std::exception&) {
        device.clear();
        freeCudaPointers(cuda, allocations);
        return false;
      }
      if (totalBytes != state.logicalBytes) {
        device.clear();
        freeCudaPointers(cuda, allocations);
        return false;
      }
      m_impl->deviceStateByConversation[state.conversationKey] = std::move(device);
      m_impl->externalDeviceAllocationsByConversation[state.conversationKey] =
        std::move(allocations);
      m_impl->hostStateByConversation.erase(host);
      m_impl->runtimeMetrics.stateHostToDeviceBytes += totalBytes;
      return true;
    });
}

bool
OnnxRuntimeModelRunner::cancelConversationStatePrefetch(
  const NativeConversationStateHandleV1& state)
{
  try {
    state.validate();
  }
  catch (const std::exception&) {
    return false;
  }
  // The store's generation fence is the cancellation boundary.  A CUDA
  // memcpy in progress is allowed to finish, but a stale completion cannot
  // change residency or resurrect an evicted conversation entry.
  return false;
}

bool
OnnxRuntimeModelRunner::releaseConversationState(
  const NativeConversationStateHandleV1& state)
{
  if (!m_impl) {
    return false;
  }
  try {
    state.validate();
  }
  catch (const std::exception&) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const bool known = m_impl->deviceStateByConversation.count(state.conversationKey) != 0 ||
    m_impl->hostStateByConversation.count(state.conversationKey) != 0 ||
    m_impl->externalDeviceAllocationsByConversation.count(state.conversationKey) != 0;
  if (!known) {
    return false;
  }
  m_impl->releaseConversationLocked(state.conversationKey);
  ++m_impl->runtimeMetrics.stateReleases;
  return true;
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

void
OnnxRuntimeModelRunner::releaseSessionState(const std::string&)
{
}

const std::optional<ExecutionEvidence>&
OnnxRuntimeModelRunner::executionEvidence() const
{
  return m_evidence;
}

std::optional<ExecutionEvidence>
OnnxRuntimeModelRunner::executionEvidenceSnapshot() const
{
  return m_evidence;
}

std::optional<NativeRuntimeMetrics>
OnnxRuntimeModelRunner::runtimeMetricsSnapshot() const
{
  return std::nullopt;
}

bool
OnnxRuntimeModelRunner::supportsOpaqueStateHandles() const
{
  return false;
}

std::optional<NativeOpaqueStateHandleV1>
OnnxRuntimeModelRunner::stateHandleSnapshot(const std::string&) const
{
  return std::nullopt;
}

bool
OnnxRuntimeModelRunner::supportsConversationStateTransfer() const
{
  return false;
}

std::optional<NativeConversationStateHandleV1>
OnnxRuntimeModelRunner::promoteSessionStateToConversation(
  const std::string&, const std::string&)
{
  return std::nullopt;
}

bool
OnnxRuntimeModelRunner::restoreConversationState(
  const NativeConversationStateHandleV1&, const std::string&)
{
  return false;
}

bool
OnnxRuntimeModelRunner::pauseConversationStateToHost(
  const NativeConversationStateHandleV1&)
{
  return false;
}

std::future<bool>
OnnxRuntimeModelRunner::prefetchConversationStateToGpu(
  const NativeConversationStateHandleV1& state)
{
  state.validate();
  std::promise<bool> promise;
  promise.set_value(false);
  return promise.get_future();
}

bool
OnnxRuntimeModelRunner::cancelConversationStatePrefetch(
  const NativeConversationStateHandleV1&)
{
  return false;
}

bool
OnnxRuntimeModelRunner::releaseConversationState(
  const NativeConversationStateHandleV1&)
{
  return false;
}

std::map<std::string, TensorBundle>
OnnxRuntimeModelRunner::run(const RoleExecutionContext&)
{
  throw std::runtime_error("C++ ONNX Runtime backend is not enabled");
}

std::optional<std::map<std::string, TensorBundle>>
OnnxRuntimeModelRunner::runStreamedImpl(const RoleExecutionContext&)
{
  return std::nullopt;
}

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  const auto creator = [] (const NativeModelRunnerSpec& spec) {
    return std::make_shared<OnnxRuntimeModelRunner>(spec);
  };
  // Keep the public adapter backend names distinct from the internal
  // execution-provider selection stored in runner metadata.
  factory.registerBackend("onnxruntime", creator);
  factory.registerBackend("onnxruntime-cpu", creator);
  factory.registerBackend("onnxruntime-cuda", creator);
}

} // namespace ndnsf::di

#endif // NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
