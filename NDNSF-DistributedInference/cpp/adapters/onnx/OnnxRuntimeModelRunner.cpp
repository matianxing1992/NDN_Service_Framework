#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-runtime-graph-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

#include <stdexcept>
#include <utility>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace ndnsf::di {
namespace {

class ScopedBackendExecutionTiming
{
public:
  ScopedBackendExecutionTiming(NativeRuntimeMetrics& metrics,
                               const RoleExecutionContext& context) noexcept
  {
    try {
      if (!metrics.lastBackendExecution) metrics.lastBackendExecution.emplace();
      auto& timing = *metrics.lastBackendExecution;
      if (!timing.identity || !timing.identity->matches(context)) {
        auto identity = std::make_shared<NativeBackendExecutionIdentity>();
        identity->requestId = context.requestId;
        identity->sessionId = context.sessionId;
        identity->role = context.role;
        identity->providerBootId = context.providerBootId;
        identity->attemptEpoch = context.attemptEpoch;
        timing.identity = std::move(identity);
      }
      timing.inferenceEpoch = context.inferenceEpoch;
      timing.completed = false;
      timing.finishedSteadyNs = 0;
      timing.finishedProcessCpuNs = -1;
      timing.finishedThreadCpuNs = -1;
      timing.startedProcessCpuNs = detail::nativeProcessCpuTimeNs();
      timing.startedThreadCpuNs = detail::nativeThreadCpuTimeNs();
      timing.startedSteadyNs = steadyTimeNs();
      m_timing = &timing;
    }
    catch (...) {
      // A diagnostic identity allocation must not replace backend execution.
      metrics.lastBackendExecution.reset();
    }
  }

  ~ScopedBackendExecutionTiming()
  {
    if (m_timing == nullptr) return;
    m_timing->finishedSteadyNs = steadyTimeNs();
    m_timing->finishedThreadCpuNs = detail::nativeThreadCpuTimeNs();
    m_timing->finishedProcessCpuNs = detail::nativeProcessCpuTimeNs();
  }

  void
  complete() noexcept
  {
    if (m_timing != nullptr) m_timing->completed = true;
  }

  ScopedBackendExecutionTiming(const ScopedBackendExecutionTiming&) = delete;
  ScopedBackendExecutionTiming& operator=(const ScopedBackendExecutionTiming&) = delete;

private:
  static std::int64_t
  steadyTimeNs() noexcept
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  NativeBackendExecutionTiming* m_timing = nullptr;
};

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

void
logOnnxSessionLoad(const NativeModelRunnerSpec& spec,
                   const OnnxRuntimeProviderSelection& selection,
                   bool resident,
                   bool cacheHit,
                   std::uint64_t loads,
                   std::uint64_t hits,
                   std::uint64_t residentEntries)
{
  std::ostringstream record;
  record << "NDNSF_DI_ONNX_SESSION_LOAD"
         << " role=" << spec.role
         << " provider=" << selection.selectedProvider
         << " resident=" << (resident ? "true" : "false")
         << " cacheHit=" << (cacheHit ? "true" : "false")
         << " loads=" << loads
         << " hits=" << hits
         << " residentEntries=" << residentEntries;
  logRuntimeEvidence(record.str());
}

} // namespace

OnnxRuntimeProviderSelection
resolveOnnxRuntimeProviderSelection(const NativeModelRunnerSpec& spec,
                                    const std::vector<std::string>& availableProviders)
{
  auto requested = runnerMetadataValue(
    spec, {"executionProvider", "execution_provider", "device.kind", "deviceKind"});
  const std::string backendProvider = spec.backend == "onnxruntime-cuda" ? "cuda" :
    spec.backend == "onnxruntime-cpu" ? "cpu" : "";
  if (requested.empty()) {
    requested = backendProvider.empty() ? "cpu" : backendProvider;
  }
  std::transform(requested.begin(), requested.end(), requested.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (requested == "cudaexecutionprovider") requested = "cuda";
  if (requested == "cpuexecutionprovider") requested = "cpu";
  if (requested != "cuda" && requested != "cpu") {
    throw std::invalid_argument("unsupported ONNX Runtime execution provider: " + requested);
  }
  if (!backendProvider.empty() && requested != backendProvider) {
    throw std::invalid_argument("ONNX Runtime execution provider conflicts with backend: " +
                                spec.backend);
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
  if ((policy != "qwen-causal-position-v1" &&
       policy != "llama-causal-position-v1") ||
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
  // The initial input lineage is core-authenticated before this Provider has
  // bound the output edge's producer/consumer roles.  Full validation remains
  // mandatory when lineage is encoded or published on the wire.
  lineage.validateCore();
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

std::string
sessionIdentity(const NativeModelRunnerSpec& spec,
                const OnnxRuntimeProviderSelection& selection)
{
  const auto metadata = [&spec] (std::initializer_list<const char*> keys) {
    return runnerMetadataValue(spec, keys);
  };
  const auto protectedIdentity = metadata({"protectedResidentIdentity"});
  if (!protectedIdentity.empty()) {
    return protectedIdentity;
  }
  const auto modelDigest = metadata({"assembledModelDigest", "artifactDigest",
                                     "evidence.artifactDigest", "digest", "sha256"});
  const auto graphDigest = metadata({"graphDigest", "graph_digest"});
  const auto initializerDigest = metadata({
    "canonicalInitializerDigest", "initializerDigest", "initializer_digest"});
  const auto recipeDigest = metadata({"recipeDigest", "recipe_digest"});
  const auto backendAbi = metadata({"backendAbi", "backend_abi"});
  if (spec.role.empty() || modelDigest.empty() || graphDigest.empty() ||
      initializerDigest.empty() || recipeDigest.empty() || backendAbi.empty()) {
    return {};
  }
  std::ostringstream canonical;
  const auto frame = [&canonical] (const std::string& value) {
    canonical << value.size() << ':' << value;
  };
  frame("ndnsf-di-onnx-resident-session-v1");
  for (const auto* value : {&modelDigest, &graphDigest, &initializerDigest,
                            &recipeDigest, &backendAbi}) {
    frame(*value);
  }
  frame(spec.role);
  frame(spec.kind);
  frame(spec.backend);
  frame(selection.selectedProvider);
  frame(selection.deviceId);
  frame(Ort::GetVersionString());
  for (const auto* key : {"artifactProfileDigest", "adapterDescriptorDigest",
                          "assemblerDescriptorDigest", "precision", "quantization",
                          "layout", "statefulModel", "inputNames", "outputNames",
                          "passthroughTensors", "outputAlias", "outputScope",
                          "outputBundleScope", "forceOutputBundle", "final",
                          "stateInputNames", "stateOutputNames",
                          "stateSuccessorMap", "positionInputPolicy",
                          "attentionMaskInputName", "positionIdsInputName",
                          "cachePositionInputName", "securityDomain",
                          "protectionEpoch"}) {
    frame(key);
    frame(metadata({key}));
  }
  if (!metadata({"assembledFileSetDigest"}).empty()) {
    frame("assembledArtifactSchema");
    frame(metadata({"assembledArtifactSchema"}));
    frame("assembledFileSetDigest");
    frame(metadata({"assembledFileSetDigest"}));
  }
  const auto bytes = canonical.str();
  return sha256TensorBytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

} // namespace


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
  using CudaGetDevice = int (*)(int*);
  using CudaSetDevice = int (*)(int);

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
    m_getDevice = reinterpret_cast<CudaGetDevice>(dlsym(m_handle, "cudaGetDevice"));
    m_setDevice = reinterpret_cast<CudaSetDevice>(dlsym(m_handle, "cudaSetDevice"));
    if (m_malloc == nullptr || m_memcpy == nullptr || m_free == nullptr) {
      dlclose(m_handle);
      m_handle = nullptr;
      m_malloc = nullptr;
      m_memcpy = nullptr;
      m_free = nullptr;
      m_getDevice = nullptr;
      m_setDevice = nullptr;
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

  bool
  canSelectDevice() const
  {
    return available() && m_getDevice != nullptr && m_setDevice != nullptr;
  }

  int
  getDevice(int* device) const
  {
    return canSelectDevice() ? m_getDevice(device) : -1;
  }

  int
  setDevice(int device) const
  {
    return canSelectDevice() ? m_setDevice(device) : -1;
  }

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
  CudaGetDevice m_getDevice = nullptr;
  CudaSetDevice m_setDevice = nullptr;
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

Ort::SessionOptions
makeSessionOptions(const OnnxRuntimeProviderSelection& selection,
                   const NativeModelRunnerSpec& spec,
                   const detail::OnnxRuntimeGraphMetadata& graphMetadata)
{
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  // CPU kernels prepare constant weights once instead of converting them on
  // every token. Keep the bounded CUDA session policy separate from CPU tuning.
  if (selection.selectedProvider == "cpu") {
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  }
  else {
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL);
    options.AddConfigEntry("session.disable_prepacking", "1");
  }
  // Qwen's KV/state dimensions change between prefill and decode.  A session
  // memory pattern learned from the warmup shape can retain a larger plan
  // than the next token needs while both role sessions remain resident.
  options.DisableMemPattern();
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
    // Match the portable FP32 graph's qualified numerical mode on CPU and CUDA.
    // Use the stable V2 C API; older supported SDKs lack its C++ owner wrapper.
    const auto& api = Ort::GetApi();
    OrtCUDAProviderOptionsV2* rawOptions = nullptr;
    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&rawOptions));
    std::unique_ptr<OrtCUDAProviderOptionsV2, decltype(api.ReleaseCUDAProviderOptions)>
      cudaOptions(rawOptions, api.ReleaseCUDAProviderOptions);
    const auto device = std::to_string(deviceId);
    const char* keys[] = {"device_id", "use_tf32"};
    const char* values[] = {device.c_str(), "0"};
    Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cudaOptions.get(), keys, values, 2));
    options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
    if (!runnerMetadataBool(spec, {"allowCpuFallback", "allow_cpu_fallback"}) &&
        graphMetadata.hostMetadataNodes.empty()) {
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

namespace {

OnnxKvStateSample
sampleKvState(const Ort::Value& value, const std::string& inputName)
{
  const auto info = value.GetTensorTypeAndShapeInfo();
  const auto count = info.GetElementCount();
  OnnxKvStateSample sample;
  sample.inputName = inputName;
  sample.shape = info.GetShape();
  std::size_t width = 0;
  switch (info.GetElementType()) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: sample.elementType = "float32"; width = 4; break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: sample.elementType = "float16"; width = 2; break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: sample.elementType = "int64"; width = 8; break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: sample.elementType = "int32"; width = 4; break;
    default: throw std::runtime_error("unsupported qualification KV scalar type");
  }
  if (!count || count > std::numeric_limits<std::size_t>::max() / width)
    throw std::runtime_error("invalid qualification KV extent");
  const auto device = value.GetTensorMemoryInfo().GetDeviceType();
  std::optional<CudaRuntimeApi> cuda;
  if (device == OrtMemoryInfoDeviceType_GPU) {
    cuda.emplace();
    if (!cuda->available())
      throw std::runtime_error("qualification KV copy requires CUDA runtime");
  }
  else if (device != OrtMemoryInfoDeviceType_CPU)
    throw std::runtime_error("unsupported qualification KV device");
  const auto* source = static_cast<const std::uint8_t*>(value.GetTensorRawData());
  if (!source)
    throw std::runtime_error("missing qualification KV allocation");
  const auto points = std::min<std::size_t>(16, count);
  for (std::size_t i = 0; i < points; ++i) {
    const auto index = points == 1 ? 0 :
      ((count - 1) / (points - 1)) * i + ((count - 1) % (points - 1)) * i / (points - 1);
    std::array<std::uint8_t, 8> bytes{};
    if (cuda) {
      if (cuda->memcpy(bytes.data(), source + index * width, width, cudaMemcpyDeviceToHost) != 0)
        throw std::runtime_error("qualification KV scalar CUDA copy failed");
    }
    else
      std::memcpy(bytes.data(), source + index * width, width);
    double number = 0;
    if (sample.elementType == "float32") {
      float scalar;
      std::memcpy(&scalar, bytes.data(), sizeof(scalar));
      number = scalar;
    }
    else if (sample.elementType == "float16") {
      Ort::Float16_t scalar;
      std::memcpy(&scalar, bytes.data(), sizeof(scalar));
      number = scalar.ToFloat();
    }
    else if (sample.elementType == "int64") {
      std::int64_t scalar;
      std::memcpy(&scalar, bytes.data(), sizeof(scalar));
      if (scalar < -(INT64_C(1) << 53) || scalar > (INT64_C(1) << 53))
        throw std::runtime_error("qualification KV integer exceeds exact scalar range");
      number = static_cast<double>(scalar);
    }
    else {
      std::int32_t scalar;
      std::memcpy(&scalar, bytes.data(), sizeof(scalar));
      number = scalar;
    }
    if (!std::isfinite(number))
      throw std::runtime_error("nonfinite qualification KV scalar");
    sample.indices.push_back(index);
    sample.values.push_back(number);
  }
  return sample;
}

} // namespace

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
    case TensorElementType::Int32:
      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
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
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      return TensorElementType::Int32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      return TensorElementType::Bool;
    default:
      throw std::runtime_error(
        "ONNX Runtime tensor dtype is not supported by the pilot codec: " +
        std::to_string(static_cast<int>(type)));
  }
}

NamedTensor
takeOwnedInputTensor(std::vector<NamedTensor> tensors,
                     const std::string& inputName)
{
  auto selected = std::find_if(tensors.begin(), tensors.end(),
    [&inputName](const NamedTensor& tensor) { return tensor.name == inputName; });
  if (selected == tensors.end()) {
    throw std::out_of_range("tensor bundle has no tensor: " + inputName);
  }
  // The decoded vector owns its payloads; transfer the selected allocation
  // instead of copying it before inputBuffers takes ownership for Ort::Value.
  return std::move(*selected);
}

NamedTensor
tensorForInput(const TensorBundle& bundle,
               const std::string& inputName,
               const std::vector<int64_t>& fallbackShape)
{
  if (bundle.localTensors) {
    // A local value is read through the same view entry as an encoded one, so a bundle that
    // arrived by either route is handled identically here. ORT binds its own input buffer, so
    // this is a copy of the values, not a re-encoding of a bundle: no framing, no decode.
    for (const auto& view : decodeTensorBundleViews(bundle)) {
      if (view.name != inputName) continue;
      NamedTensor tensor;
      tensor.name = view.name;
      tensor.elementType = view.elementType;
      tensor.shape = view.shape;
      tensor.payload.assign(view.payload.begin(), view.payload.end());
      return tensor;
    }
    throw std::out_of_range("tensor bundle has no tensor: " + inputName);
  }
  if (isEncodedTensorBundle(bundle.payload)) {
    return takeOwnedInputTensor(decodeTensorBundle(bundle.payload), inputName);
  }
  return makeFloat32Tensor(inputName, fallbackShape, bundle.payload);
}

std::vector<int64_t>
shapeForInput(const NativeModelRunnerSpec& spec,
              const std::string& inputName,
              std::size_t index,
              const std::vector<int64_t>& modelShape,
              bool preserveZeroDynamicDimensions = false,
              const std::vector<std::string>& symbolicDimensions = {})
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
  for (std::size_t dimensionIndex = 0;
       dimensionIndex < shape.size(); ++dimensionIndex) {
    auto& dim = shape[dimensionIndex];
    if (dim <= 0) {
      std::string symbolic;
      if (dimensionIndex < symbolicDimensions.size()) {
        symbolic = symbolicDimensions[dimensionIndex];
        std::transform(symbolic.begin(), symbolic.end(), symbolic.begin(),
                       [] (unsigned char ch) {
                         return static_cast<char>(std::tolower(ch));
                       });
      }
      const bool isSequenceDimension =
        symbolic.find("past") != std::string::npos ||
        symbolic.find("cache") != std::string::npos ||
        symbolic.find("sequence") != std::string::npos ||
        symbolic.find("seq") != std::string::npos;
      // A dynamic KV/past dimension is a real zero-length tensor at the
      // beginning of a Qwen prefill.  The historical Python pipeline creates
      // exactly that state; replacing it with one makes the causal mask one
      // token longer than the input and fails in the first Add node.  Keep
      // the old conservative fallback for ordinary inputs and warm-up, but
      // let the caller opt into the initial-state contract explicitly for
      // sequence-like symbolic dimensions.
      dim = preserveZeroDynamicDimensions && isSequenceDimension ? 0 : 1;
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
    auto tensors = decodeTensorBundle(item.second.payload);
    try {
      return takeOwnedInputTensor(std::move(tensors), name);
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

class OnnxRuntimeModelRunner::SharedSession
{
public:
  explicit SharedSession(const NativeModelRunnerSpec& spec)
    : environment(ORT_LOGGING_LEVEL_WARNING, "ndnsf-di")
    , selection(resolveRuntimeProviderSelection(spec))
    , graphMetadata(detail::inspectOnnxRuntimeGraphMetadata(spec.path,
                    selection.selectedProvider == "cuda" &&
                    spec.metadata.count("evidence.providerBootId") != 0 &&
                    !runnerMetadataBool(spec, {"allowCpuFallback", "allow_cpu_fallback"}) &&
                    !runnerMetadataValue(
                      spec, {"providerProfilePrefix", "provider_profile_prefix"}).empty()))
    , sessionOptions(makeSessionOptions(selection, spec, graphMetadata))
    , session(environment, spec.path.c_str(), sessionOptions)
  {
    detail::verifyOnnxRuntimeGraphMetadataSource(spec.path, graphMetadata);
  }

  // ORT shares the native environment. Own its reference through session teardown,
  // then release it here rather than during process-wide static destruction.
  Ort::Env environment;
  OnnxRuntimeProviderSelection selection;
  detail::OnnxRuntimeGraphMetadata graphMetadata;
  Ort::SessionOptions sessionOptions;
  Ort::Session session;

  // Shared by the load owner and every cache-hit wrapper so a skipped
  // unranked-input warm-up is not reported as a completed warm-up.
  bool warmupSkippedUnrankedInput = false;
};

class OnnxRuntimeModelRunner::Impl
{
public:
  explicit Impl(const NativeModelRunnerSpec& spec)
    : Impl(spec, std::make_shared<SharedSession>(spec), {})
  {
  }

  Impl(const NativeModelRunnerSpec& spec,
       std::shared_ptr<SharedSession> shared,
       OnnxRuntimeSessionCache::Lease sessionLease)
    : sharedSession(std::move(shared))
    , selection(sharedSession->selection)
    , session(sharedSession->session)
    , sessionLease(std::move(sessionLease))
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
    const auto stateMap = runnerMetadataValue(
      spec, {"stateSuccessorMap", "state_successor_map", "kvTensorMap",
             "kv_tensor_map"});
    if (!stateMap.empty()) {
      for (const auto& entry : splitNames(stateMap)) {
        const auto separator = entry.find('=');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= entry.size()) {
          throw std::invalid_argument(
            "stateful ONNX successor map contains an invalid pair");
        }
        const auto inputName = entry.substr(0, separator);
        const auto outputName = entry.substr(separator + 1);
        contract.successorInputByOutput[outputName] = inputName;
      }
      if (!contract.successorInputByOutput.empty()) {
        contract.stateFamilies.clear();
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

  std::shared_ptr<SharedSession> sharedSession;
  OnnxRuntimeProviderSelection selection;
  Ort::Session& session;
  OnnxRuntimeSessionCache::Lease sessionLease;
  std::optional<StatefulOnnxIoContractV1> statefulIo;
  std::optional<CausalPositionInputContractV1> causalPositionInputs;
  bool profilingEnabled = false;
  bool profilingFailed = false;
  std::atomic<bool> profilingCaptured{false};
  mutable std::mutex profileMutex;
  // A stateful session keeps the state tensors in ORT-managed memory between
  // request-scoped epochs.  CUDA and CPU use separate maps because their
  // Ort::Value memory domains are different.  The maps are keyed by the
  // request/session identity, so concurrent generations cannot accidentally
  // consume one another's recurrent state.  Host TensorBundle bytes remain a
  // diagnostic/transport representation only; they are not used when a
  // resident predecessor is available.
  mutable std::mutex executionMutex;
  std::map<std::string, std::map<std::string, Ort::Value>> deviceStateBySession;
  std::map<std::string, std::map<std::string, Ort::Value>> hostStateBySession;
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
  // CPU conversation state remains in host ORT memory and can be rebound
  // directly on the next turn.  Keep it separate from hostStateByConversation,
  // whose HostStateTensor values are serialized pause snapshots for CUDA.
  std::map<std::string, std::map<std::string, Ort::Value>> residentHostStateByConversation;
  std::map<std::string, HostStateMap> hostStateByConversation;
  std::map<std::string, std::map<std::string, void*>> externalDeviceAllocationsByConversation;
  std::map<std::string, std::string> conversationBindingBySession;
  std::map<std::string, NativeConversationStateHandleV1> stateHandleByConversation;
  // The coordinator receives only this authenticated opaque reference.  The
  // actual Ort::Value allocations remain owned by deviceStateBySession and
  // never cross ProviderRoleWorker, the state store, or an NDN bundle.
  std::map<std::string, NativeOpaqueStateHandleV1> stateHandleBySession;
  NativeRuntimeMetrics runtimeMetrics;
  std::shared_ptr<OnnxKvStateObserver> stateObserver;

  void
  releaseConversationLocked(const std::string& conversationKey)
  {
    deviceStateByConversation.erase(conversationKey);
    residentHostStateByConversation.erase(conversationKey);
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
    const auto hostFound = hostStateBySession.find(sessionId);
    const auto handle = stateHandleBySession.find(sessionId);
    if (found == deviceStateBySession.end() &&
        hostFound == hostStateBySession.end() &&
        handle == stateHandleBySession.end()) {
      return 0;
    }
    if (found != deviceStateBySession.end()) {
      deviceStateBySession.erase(found);
    }
    if (hostFound != hostStateBySession.end()) {
      hostStateBySession.erase(hostFound);
    }
    if (handle != stateHandleBySession.end()) {
      stateHandleBySession.erase(handle);
    }
    conversationBindingBySession.erase(sessionId);
    ++runtimeMetrics.stateReleases;
    return 1;
  }
};

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(
  NativeModelRunnerSpec spec,
  std::shared_ptr<SharedSession> sharedSession,
  bool runWarmup)
  : m_spec(std::move(spec))
{
  if (m_spec.path.empty()) {
    throw std::invalid_argument("ONNX Runtime runner requires model path");
  }
  m_impl = sharedSession
    ? std::make_unique<Impl>(m_spec, std::move(sharedSession),
                             OnnxRuntimeSessionCache::Lease{})
    : std::make_unique<Impl>(m_spec);
  initializeEvidence();
  if (runWarmup) {
    warmup();
  }
}

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
  : OnnxRuntimeModelRunner(std::move(spec), nullptr, true)
{
}

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(
  NativeModelRunnerSpec spec,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache)
  : OnnxRuntimeModelRunner(std::move(spec), std::move(sessionCache), nullptr)
{
}

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(
  NativeModelRunnerSpec spec,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache,
  std::shared_ptr<OnnxKvStateObserver> stateObserver)
  : m_spec(std::move(spec))
{
  if (m_spec.path.empty()) {
    throw std::invalid_argument("ONNX Runtime runner requires model path");
  }
  const auto selection = resolveRuntimeProviderSelection(m_spec);
  const bool protectedBacking =
    m_spec.metadata.count("encryptedArtifactPath") != 0 ||
    runnerMetadataBool(m_spec, {"protectedRuntime", "protected_runtime"});
  const bool protectedResidentAdmitted =
    static_cast<bool>(m_spec.protectedResidentUse) &&
    !runnerMetadataValue(m_spec, {"protectedResidentIdentity"}).empty();
  const bool profilingRequested = !runnerMetadataValue(
    m_spec, {"providerProfilePrefix", "provider_profile_prefix"}).empty();
  const bool sessionEligible = !profilingRequested &&
    selection.selectedProvider == "cpu" &&
    (!protectedBacking || protectedResidentAdmitted);
  // Session reuse is an immutable ONNX admission property, not a residency
  // hint. The identity includes the model, graph, backend, ORT, IO/KV and
  // protection contract; residentSession remains reserved for the separate
  // protected-residency authority path in Provider.
  const auto identity = sessionEligible ? sessionIdentity(m_spec, selection) : std::string{};
  if (!sessionCache || identity.empty()) {
    m_impl = std::make_unique<Impl>(m_spec);
    m_impl->stateObserver = std::move(stateObserver);
    initializeEvidence();
    warmup();
    logOnnxSessionLoad(m_spec, selection, false, false, 1, 0, 0);
    return;
  }
  std::vector<NativeProviderResidencyIdentityV4> residencyIdentities;
  if (m_spec.residencyIdentity)
    residencyIdentities.push_back(*m_spec.residencyIdentity);
  auto lease = sessionCache->acquire(identity, [spec = m_spec] {
    return loadSharedSession(spec);
  }, std::chrono::steady_clock::time_point::max(), {},
     std::move(residencyIdentities));
  const auto sharedValue = lease.value();
  auto sharedSession = std::static_pointer_cast<SharedSession>(sharedValue);
  if (!sharedSession) {
    throw std::runtime_error("DI_ONNX_SESSION_CACHE_VALUE_INVALID");
  }
  const bool cacheHit = lease.cacheHit();
  m_impl = std::make_unique<Impl>(m_spec, std::move(sharedSession),
                                  std::move(lease));
  m_impl->stateObserver = std::move(stateObserver);
  initializeEvidence();
  if (m_evidence) {
    // The load owner's warmup ran (or was skipped for an unranked input)
    // before acquire publishes the value; this wrapper has its own evidence
    // object but does not repeat warmup.
    if (m_impl->sharedSession->warmupSkippedUnrankedInput)
      m_evidence->warmupSkippedUnrankedInput = true;
    else
      m_evidence->warmupCompleted = true;
    m_evidence->validate();
  }
  const auto counters = sessionCache->counters();
  logOnnxSessionLoad(m_spec, selection, true, cacheHit,
                     counters.loads, counters.hits, counters.residentEntries);
}

void
OnnxRuntimeModelRunner::initializeEvidence()
{
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
}

void
OnnxRuntimeModelRunner::warmup()
{
  // Session construction proves model load, but not executable readiness.
  // Execute one shape-valid zero input through the real selected provider so
  // DATA_DRIVEN_V2 cannot publish READY for an unavailable CUDA graph.
  RoleExecutionContext warmup;
  warmup.sessionId = "native-runtime-warmup";
  warmup.role = m_spec.role;
  // A boundary input whose authenticated contract carries no rank (the
  // canonical source value_info has no usable shape) cannot be given a valid
  // synthetic value: structural consumers such as Reshape require a concrete
  // rank-1 shape vector, and no zero placeholder satisfies that.  The first
  // real request supplies the producer's actual tensors and is the executable
  // readiness evidence for this CPU session.  Record the skip explicitly
  // instead of feeding a rank-0 placeholder that fails the probe.
  const auto unrankedInputs = metadataNames(
    m_spec, {"unrankedInputNames", "unranked_input_names"});
  if (m_impl->selection.selectedProvider == "cpu" && !unrankedInputs.empty()) {
    m_impl->sharedSession->warmupSkippedUnrankedInput = true;
    if (m_evidence) {
      m_evidence->warmupSkippedUnrankedInput = true;
      m_evidence->validate();
    }
    return;
  }
  Ort::AllocatorWithDefaultOptions allocator;
  const auto inputCount = m_impl->session.GetInputCount();
  for (std::size_t index = 0; index < inputCount; ++index) {
    auto name = m_impl->session.GetInputNameAllocated(index, allocator);
    const std::string inputName(name.get());
    auto typeInfo = m_impl->session.GetInputTypeInfo(index);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    std::vector<const char*> symbolicDimensions(tensorInfo.GetDimensionsCount());
    if (!symbolicDimensions.empty()) {
      tensorInfo.GetSymbolicDimensions(symbolicDimensions.data(), symbolicDimensions.size());
    }
    std::vector<std::string> symbolicDimensionNames;
    symbolicDimensionNames.reserve(symbolicDimensions.size());
    for (const auto* symbolic : symbolicDimensions) {
      symbolicDimensionNames.emplace_back(symbolic == nullptr ? "" : symbolic);
    }
    const bool isStateInput = m_impl->statefulIo &&
      std::find(m_impl->statefulIo->stateInputNames.begin(),
                m_impl->statefulIo->stateInputNames.end(), inputName) !=
        m_impl->statefulIo->stateInputNames.end();
    NamedTensor tensor;
    tensor.name = inputName;
    tensor.elementType = fromOnnxElementType(tensorInfo.GetElementType());
    tensor.shape = shapeForInput(
      m_spec, inputName, index, tensorInfo.GetShape(), isStateInput,
      symbolicDimensionNames);
    tensor.payload.assign(
      elementCount(tensor.shape) * tensorElementByteSize(tensor.elementType),
      0);
    if (m_impl->sharedSession->graphMetadata.attentionWarmupLengthInputs.count(inputName)) {
      if (tensor.elementType != TensorElementType::Int32 || tensor.payload.size() != 4)
        throw std::invalid_argument("attention warmup length must be one Int32 scalar");
      const std::int32_t length = 1;
      std::memcpy(tensor.payload.data(), &length, sizeof(length));
    }
    warmup.inputsByScope.emplace(
      inputName, makeEncodedTensorBundle(inputName, {std::move(tensor)}));
  }
  if (m_impl->causalPositionInputs) {
    // A causal graph does not accept an all-zero attention mask as a
    // readiness probe: Qwen derives a gather index from that mask and can
    // reject the probe before any request lineage exists.  Keep warmup local
    // and unauthenticated, but use the smallest semantically valid prefill
    // values.  Real requests still materialize these inputs from authenticated
    // GenerationEpochLineageV1 below.
    const auto setWarmupInt64 = [&] (const std::string& name,
                                    std::int64_t value) {
      if (name.empty()) {
        return;
      }
      const auto found = warmup.inputsByScope.find(name);
      if (found == warmup.inputsByScope.end()) {
        throw std::invalid_argument(
          "causal ONNX warmup input is missing from the graph: " + name);
      }
      const auto templateTensor = tensorForInput(found->second, name, {});
      if (templateTensor.elementType != TensorElementType::Int64) {
        throw std::invalid_argument(
          "causal ONNX warmup input is not Int64: " + name);
      }
      const auto count = elementCount(templateTensor.shape);
      std::vector<std::int64_t> values(count, value);
      std::vector<std::uint8_t> payload(values.size() * sizeof(std::int64_t));
      std::memcpy(payload.data(), values.data(), payload.size());
      warmup.inputsByScope[name] = makeEncodedTensorBundle(
        name,
        {NamedTensor{name, TensorElementType::Int64,
                     templateTensor.shape, std::move(payload)}});
    };
    const auto& causal = *m_impl->causalPositionInputs;
    setWarmupInt64(causal.attentionMaskInputName, 1);
    setWarmupInt64(causal.positionIdsInputName, 0);
    setWarmupInt64(causal.cachePositionInputName, 0);
  }
  (void)run(warmup);
  if (m_evidence) {
    m_evidence->warmupCompleted = true;
    m_evidence->validate();
  }
}

std::shared_ptr<void>
OnnxRuntimeModelRunner::loadSharedSession(const NativeModelRunnerSpec& spec)
{
  auto sharedSession = std::make_shared<SharedSession>(spec);
  OnnxRuntimeModelRunner warmupRunner(spec, sharedSession, false);
  warmupRunner.warmup();
  return sharedSession;
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
  NativeWorkScope adapterWork(ctx.workStatistics, "onnx-adapter", NativeWorkKind::UNCLASSIFIED);
  NativeWorkScope lockWork(ctx.workStatistics, "onnx-session-lock", NativeWorkKind::WAIT);
  // The CUDA state map below is part of the persistent session transaction.
  // Serialize transitions so concurrent requests cannot observe or replace
  // one another's recurrent state.
  std::unique_lock<std::mutex> executionLock(m_impl->executionMutex);
  if (m_impl->profilingFailed)
    throw std::runtime_error("ONNX Runtime provider profile was previously rejected");
  std::unique_lock<std::mutex> firstProfileRunLock;
  if (m_impl->profilingEnabled &&
      !m_impl->profilingCaptured.load(std::memory_order_acquire)) {
    firstProfileRunLock = std::unique_lock<std::mutex>(m_impl->profileMutex);
    if (m_impl->profilingCaptured.load(std::memory_order_acquire)) {
      firstProfileRunLock.unlock();
    }
  }
  lockWork.finish();
  const auto collectStart = std::chrono::steady_clock::now();
  NativeWorkScope contextWork(ctx.workStatistics, "onnx-input-context", NativeWorkKind::LOCAL);
  std::optional<RoleExecutionContext> redistributedContext;
  if (std::any_of(ctx.inputEdgesByScope.begin(), ctx.inputEdgesByScope.end(),
                  [] (const auto& input) { return !input.second.redistributions.empty(); })) {
    redistributedContext.emplace(ctx);
    redistributedContext->inputsByScope = applyCertifiedTensorRedistributions(ctx);
  }
  const auto& effectiveContext = redistributedContext ? *redistributedContext : ctx;
  contextWork.finish();
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
  // CPU state is request-resident only for the coordinator-owned streaming
  // path.  Keep ordinary run() callers on the explicit TensorBundle contract.
  const bool hostResidentState = m_impl->statefulIo.has_value() &&
    m_impl->selection.selectedProvider != "cuda" &&
    effectiveContext.streamingStateExecution;
  const bool residentState = deviceResidentState || hostResidentState;
  const auto conversationBinding = m_impl->conversationBindingBySession.find(
    effectiveContext.sessionId);
  const bool restoringConversation = m_impl->statefulIo.has_value() &&
    conversationBinding != m_impl->conversationBindingBySession.end();
  if (m_impl->statefulIo && effectiveContext.sessionId != "native-runtime-warmup" &&
      effectiveContext.inferenceEpoch == 0) {
    ++m_impl->runtimeMetrics.stateRecomputes;
  }
  if (residentState && effectiveContext.inferenceEpoch == 0 &&
      !restoringConversation) {
    // Session reuse is allowed, but state is request-scoped and must not cross
    // a fresh generation boundary.
    m_impl->eraseSessionStateLocked(effectiveContext.sessionId);
  }
  std::map<std::string, const Ort::Value*> boundDeviceInputs;
  std::map<std::string, const Ort::Value*> boundHostInputs;
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
    std::vector<const char*> symbolicDimensions(tensorInfo.GetDimensionsCount());
    if (!symbolicDimensions.empty()) {
      tensorInfo.GetSymbolicDimensions(symbolicDimensions.data(), symbolicDimensions.size());
    }
    std::vector<std::string> symbolicDimensionNames;
    symbolicDimensionNames.reserve(symbolicDimensions.size());
    for (const auto* symbolic : symbolicDimensions) {
      symbolicDimensionNames.emplace_back(symbolic == nullptr ? "" : symbolic);
    }
    const bool isStateInput = m_impl->statefulIo &&
      std::find(m_impl->statefulIo->stateInputNames.begin(),
                m_impl->statefulIo->stateInputNames.end(), inputName) !=
        m_impl->statefulIo->stateInputNames.end();
    inputShapes.push_back(shapeForInput(
      m_spec, inputName, i, tensorInfo.GetShape(),
      isStateInput && effectiveContext.inferenceEpoch == 0,
      symbolicDimensionNames));
    if (residentState && isStateInput &&
        (effectiveContext.inferenceEpoch > 0 || restoringConversation)) {
      const std::map<std::string, Ort::Value>* stateMap = nullptr;
      if (deviceResidentState) {
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
      }
      else {
        const auto sessionState = m_impl->hostStateBySession.find(
          effectiveContext.sessionId);
        if (effectiveContext.inferenceEpoch > 0 &&
            sessionState != m_impl->hostStateBySession.end()) {
          stateMap = &sessionState->second;
        }
        else if (restoringConversation) {
          const auto retained = m_impl->residentHostStateByConversation.find(
            conversationBinding->second);
          if (retained != m_impl->residentHostStateByConversation.end()) {
            stateMap = &retained->second;
          }
        }
      }
      if (stateMap != nullptr) {
        const auto deviceState = stateMap->find(inputName);
        if (deviceState != stateMap->end()) {
          // Use the previous output directly in the same ORT memory domain.
          // The host state bundle is intentionally not materialized on this
          // request-resident path.
          if (deviceResidentState) {
            boundDeviceInputs.emplace(inputName, &deviceState->second);
          }
          else {
            boundHostInputs.emplace(inputName, &deviceState->second);
          }
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
        std::string("stateful ONNX decode is missing Provider-owned ") +
        (deviceResidentState ? "device" : "host") +
        " predecessor state: " + inputName);
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
    const auto expectedType = tensorInfo.GetElementType();
    NativeWorkScope tensorWork(ctx.workStatistics, "onnx-input-tensor", NativeWorkKind::LOCAL);
    auto tensor = tensorForInput(*inputBundle, inputName, inputShapes.back());
    validateNamedTensor(tensor);
    const auto onnxType = toOnnxElementType(tensor.elementType);
    if (expectedType != onnxType) {
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
    tensorWork.finish();
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

  if (runtimeTimingEnabled()) {
    std::ostringstream record;
    record << "NDNSF_DI_NATIVE_OUTPUT_CONTRACT"
           << " role=" << m_spec.role
           << " outputs=";
    for (std::size_t index = 0; index < outputNames.size(); ++index) {
      if (index != 0) record << ',';
      const auto alias = metadataValue(
        m_spec,
        {"outputAlias." + outputNames[index],
         "output_alias." + outputNames[index]});
      record << outputNames[index] << "->" << (alias.empty() ? "<identity>" : alias);
    }
    logRuntimeInfo(record.str());
  }

  const auto runStart = std::chrono::steady_clock::now();
  std::vector<Ort::Value> outputs;
  std::optional<Ort::IoBinding> ioBinding;
  std::optional<Ort::MemoryInfo> cudaMemoryInfo;
  std::size_t deviceStateOutputsBound = 0;
  const auto phaseAttempt = effectiveContext.attemptEpoch == 0
    ? std::string("request") : std::to_string(effectiveContext.attemptEpoch);
  const auto phaseFields = std::vector<std::pair<std::string, std::string>>{
    {"executionRole", m_spec.role},
    {"providerBootId", effectiveContext.providerBootId.empty() ?
                           std::string("none") : effectiveContext.providerBootId},
    {"sessionId", effectiveContext.sessionId},
    {"inferenceEpoch", std::to_string(effectiveContext.inferenceEpoch)},
  };
  if (residentState) {
    ioBinding.emplace(m_impl->session);
    if (deviceResidentState) {
      cudaMemoryInfo.emplace(
        "Cuda", OrtAllocatorType::OrtArenaAllocator,
        std::stoi(m_impl->selection.deviceId), OrtMemTypeDefault);
    }
    for (const auto& name : inputNames) {
      const auto deviceInput = boundDeviceInputs.find(name);
      if (deviceInput != boundDeviceInputs.end()) {
        ioBinding->BindInput(name.c_str(), *deviceInput->second);
        continue;
      }
      const auto hostInput = boundHostInputs.find(name);
      if (hostInput != boundHostInputs.end()) {
        ioBinding->BindInput(name.c_str(), *hostInput->second);
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
      if (stateOutput && deviceResidentState) {
        // Let ORT allocate state outputs on CUDA.  The resulting Ort::Value is
        // retained in the session map and rebound on the next epoch.
        ioBinding->BindOutput(name.c_str(), cudaMemoryInfo->GetConst());
        ++deviceStateOutputsBound;
      }
      else {
        ioBinding->BindOutput(name.c_str(), memoryInfo.GetConst());
      }
    }
    logRuntimePhase("di-provider", "ortRunBegin", effectiveContext.requestId,
                    phaseAttempt, phaseFields);
    {
      NativeWorkScope backendWork(ctx.workStatistics, "onnx-run-envelope", NativeWorkKind::BACKEND);
      ScopedBackendExecutionTiming timing(m_impl->runtimeMetrics, effectiveContext);
      m_impl->session.Run(Ort::RunOptions{nullptr}, *ioBinding);
      // The engine interval ends only when device outputs are ready. Leaving
      // synchronization outside it misattributes inference wait to DI work.
      ioBinding->SynchronizeOutputs();
      timing.complete();
    }
    logRuntimePhase("di-provider", "ortRunEnd", effectiveContext.requestId,
                    phaseAttempt, phaseFields);
    outputs = ioBinding->GetOutputValues();
  }
  else {
    std::vector<const char*> outputNamePtrs;
    outputNamePtrs.reserve(outputNames.size());
    for (const auto& name : outputNames) {
      outputNamePtrs.push_back(name.c_str());
    }
    logRuntimePhase("di-provider", "ortRunBegin", effectiveContext.requestId,
                    phaseAttempt, phaseFields);
    {
      NativeWorkScope backendWork(ctx.workStatistics, "onnx-run-envelope", NativeWorkKind::BACKEND);
      ScopedBackendExecutionTiming timing(m_impl->runtimeMetrics, effectiveContext);
      outputs = m_impl->session.Run(
        Ort::RunOptions{nullptr},
        inputNamePtrs.data(),
        inputValues.data(),
        inputValues.size(),
        outputNamePtrs.data(),
        outputNamePtrs.size());
      timing.complete();
    }
    logRuntimePhase("di-provider", "ortRunEnd", effectiveContext.requestId,
                    phaseAttempt, phaseFields);
  }
  const auto runDone = std::chrono::steady_clock::now();
  const bool captureRequestProfile =
    !runnerMetadataBool(m_spec, {"profileAfterRequest"}) ||
    (!ctx.requestId.empty() && ctx.attemptEpoch != 0) ||
    (!m_impl->sharedSession->graphMetadata.hostMetadataNodes.empty() &&
     m_evidence && m_evidence->warmupCompleted);
  if (m_impl->profilingEnabled && captureRequestProfile &&
      !m_impl->profilingCaptured.load(std::memory_order_relaxed) && m_evidence) {
    // EndProfiling is terminal. A rejected or unreadable profile must not let
    // a later call execute again without a usable admission record.
    m_impl->profilingFailed = true;
    auto profile = m_impl->session.EndProfilingAllocated(allocator);
    const std::string profilePath(profile.get());
    applyOnnxRuntimeProviderProfile(
      *m_evidence,
      profilePath,
      m_spec.role,
      m_impl->selection.usedCpuFallback,
      m_evidence->gpuUuid,
      m_impl->sharedSession->graphMetadata.hostMetadataNodes,
      m_impl->sharedSession->graphMetadata.modelNodes,
      m_impl->sharedSession->graphMetadata.optionalModelNodes,
      m_impl->sharedSession->graphMetadata.modelNodeAliases);
    m_evidence->profileRequestId = ctx.requestId;
    m_evidence->profileAttemptEpoch = ctx.attemptEpoch;
    m_impl->profilingCaptured.store(true, std::memory_order_release);
    m_impl->profilingFailed = false;
  }
  const auto executionDelayMs = metadataDoubleValue(
    m_spec,
    {"executionDelayMs", "execution_delay_ms", "roleExecutionDelayMs",
     "role_execution_delay_ms"});
  if (executionDelayMs > 0.0) {
    NativeWorkScope delayWork(ctx.workStatistics, "onnx-configured-delay", NativeWorkKind::WAIT);
    std::this_thread::sleep_for(
      std::chrono::duration<double, std::milli>(executionDelayMs));
  }
  const auto delayDone = std::chrono::steady_clock::now();

  NativeWorkScope outputWork(ctx.workStatistics, "onnx-output-convert", NativeWorkKind::LOCAL);
  // Backing storage for a local result. `namedOutputs` borrows from these buffers (and from
  // `auxiliaryOutputs` for passthrough values), so a LocalTensorBundle must keep all of them
  // alive for exactly as long as it exposes views -- the whole reason a local value carries an
  // owner instead of borrowing a frame.
  struct LocalOutputOwner
  {
    std::vector<std::vector<std::uint8_t>> hostBuffers;
    std::vector<NamedTensor> ownedTensors;
  };
  std::vector<NamedTensorView> namedOutputs;
  namedOutputs.reserve(outputs.size());
  std::vector<NamedTensor> auxiliaryOutputs;
  const auto appendOwnedOutput = [&] (NamedTensor tensor) {
    auxiliaryOutputs.push_back(std::move(tensor));
    const auto& owned = auxiliaryOutputs.back();
    namedOutputs.push_back({owned.name, owned.elementType, owned.shape,
      ndn::span<const std::uint8_t>(owned.payload.data(), owned.payload.size())});
  };
  std::vector<std::vector<std::uint8_t>> outputHostBuffers;
  outputHostBuffers.reserve(outputs.size());
  std::uint64_t borrowedOutputBytes = 0;
  const bool observeState = m_impl->stateObserver && m_impl->stateObserver->wants(effectiveContext);
  std::vector<OnnxKvStateSample> stateSamples;
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
    const bool stateOutput = residentState &&
      std::find(m_impl->statefulIo->stateOutputNames.begin(),
                m_impl->statefulIo->stateOutputNames.end(), outputNames[i]) !=
        m_impl->statefulIo->stateOutputNames.end();
    const void* hostData = value.GetTensorRawData();
    if (stateOutput) {
      NativeWorkScope stateWork(ctx.workStatistics, "onnx-output-state",
                                NativeWorkKind::UNCLASSIFIED);
      // The next epoch asks for the corresponding *_in tensor.  Store the
      // device allocation under that successor input name; retaining it under
      // *_out makes every lookup miss and silently reintroduces a host upload.
      const auto successorInput =
        m_impl->statefulIo->stateInputForOutput(outputNames[i]);
      if (observeState) {
        if (stateSamples.size() >= 256)
          throw std::runtime_error("qualification KV component bound exceeded");
        NativeWorkScope sampleWork(ctx.workStatistics, "onnx-kv-sample", NativeWorkKind::LOCAL);
        stateSamples.push_back(sampleKvState(value, successorInput));
      }
      auto& stateBySession = deviceResidentState
        ? m_impl->deviceStateBySession[effectiveContext.sessionId]
        : m_impl->hostStateBySession[effectiveContext.sessionId];
      stateBySession.insert_or_assign(successorInput, std::move(value));
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
        stateBySession.at(successorInput), outputHostBuffers.back().data(),
        outputHostBuffers.back().size());
      if (deviceResidentState) {
        m_impl->runtimeMetrics.stateDeviceToHostBytes += outputBytes;
      }
      hostData = outputHostBuffers.back().data();
    }
    else {
      if (value.GetTensorMemoryInfo().GetDeviceType() != OrtMemoryInfoDeviceType_CPU) {
        NativeWorkScope copyWork(ctx.workStatistics, "onnx-output-device-copy",
                                 NativeWorkKind::UNCLASSIFIED);
        outputHostBuffers.emplace_back(count * tensorElementByteSize(elementType), 0);
        copyOrtTensorToHost(value, outputHostBuffers.back().data(), outputHostBuffers.back().size());
        hostData = outputHostBuffers.back().data();
      }
      m_impl->runtimeMetrics.activationOutputBytes += outputBytes;
      if (deviceResidentState) {
        m_impl->runtimeMetrics.activationDeviceToHostBytes += outputBytes;
      }
    }
    const auto* data = static_cast<const std::uint8_t*>(hostData);
    NamedTensorView tensor;
    tensor.name = metadataValue(
      m_spec,
      {"outputAlias." + outputNames[i], "output_alias." + outputNames[i]});
    if (tensor.name.empty()) {
      tensor.name = outputNames[i];
    }
    tensor.elementType = elementType;
    tensor.shape = tensorInfo.GetShape();
    tensor.payload = ndn::span<const std::uint8_t>(
      data, count * tensorElementByteSize(elementType));
    validateNamedTensorView(tensor);
    borrowedOutputBytes += tensor.payload.size();
    namedOutputs.push_back(std::move(tensor));
  }
  if (observeState) {
    if (stateSamples.empty())
      throw std::runtime_error("qualification requires resident KV state outputs");
    NativeWorkScope sampleWork(ctx.workStatistics, "onnx-kv-sample", NativeWorkKind::LOCAL);
    m_impl->stateObserver->record(effectiveContext, std::move(stateSamples));
  }
  for (const auto& name : metadataNames(
         m_spec, {"passthroughTensors", "passthrough_tensors"})) {
    const auto duplicate = std::find_if(
      namedOutputs.begin(), namedOutputs.end(), [&name] (const NamedTensorView& tensor) {
        return tensor.name == name;
      });
    if (duplicate == namedOutputs.end()) {
      try {
        appendOwnedOutput(passthroughTensorFor(effectiveContext, name));
      }
      catch (const std::out_of_range&) {
        const auto causal = causalPositionInputs.find(name);
        if (causal == causalPositionInputs.end())
          throw;
        appendOwnedOutput(tensorForInput(causal->second, name, {}));
      }
    }
  }

  // A CPU caller that asked for local values gets typed views instead of one encoded buffer per
  // call. The bytes are the same ones the wire path would have produced -- this only skips
  // encoding them for a hop that is about to decode them again. Anything that is not a plain
  // CPU typed output keeps the wire path: resident state is opaque to the caller, and CUDA,
  // legacy PP and standalone reference runs never set the preference.
  const bool localOutputs = effectiveContext.preferLocalTensorOutputs &&
    !deviceResidentState && !effectiveContext.streamingStateExecution &&
    !namedOutputs.empty();
  std::shared_ptr<LocalOutputOwner> localOwner;
  if (localOutputs) {
    // Take ownership of every output's bytes before returning. The views above borrow ORT's
    // output values (or the transient host buffers), and `outputs` dies when this function
    // returns -- so a local value that kept those spans would read freed memory after the call.
    // One copy into an owned record is what makes the local representation safe, and it is still
    // an order of magnitude less work than encoding a bundle and decoding it again.
    auto owner = std::make_shared<LocalOutputOwner>();
    owner->ownedTensors.reserve(namedOutputs.size());
    for (const auto& view : namedOutputs) {
      NamedTensor copy;
      copy.name = view.name;
      copy.elementType = view.elementType;
      copy.shape = view.shape;
      copy.payload.assign(view.payload.begin(), view.payload.end());
      owner->ownedTensors.push_back(std::move(copy));
    }
    // Rebuild the views only after the record stops growing, so each span points at storage that
    // will not move again.
    std::vector<NamedTensorView> ownedViews;
    ownedViews.reserve(owner->ownedTensors.size());
    for (const auto& tensor : owner->ownedTensors) {
      NamedTensorView view;
      view.name = tensor.name;
      view.elementType = tensor.elementType;
      view.shape = tensor.shape;
      view.payload = ndn::span<const std::uint8_t>(tensor.payload.data(),
                                                   tensor.payload.size());
      ownedViews.push_back(std::move(view));
    }
    namedOutputs = std::move(ownedViews);
    localOwner = owner;
  }

  std::map<std::string, TensorBundle> result;
  // A local value is always one named bundle, so a spec that asks for a bundle gets one either
  // way; only the representation differs. That is why the bundle request must not override the
  // preference -- it did, and the local path was unreachable through the parent runner even
  // though every reader understood it.
  const bool forceEncodedOutput = localOutputs ? false :
    (effectiveContext.streamingStateExecution ||
     !metadataValue(m_spec, {"output_tensor", "outputTensor", "forceOutputBundle",
                             "force_output_bundle"}).empty() ||
     metadataValue(m_spec, {"final", "is_final"}) == "true");
  if (localOutputs && !forceEncodedOutput) {
    const auto localScope = [&] (const std::string& single) {
      return namedOutputs.size() == 1 ? outputScopeFor(m_spec, single, 0)
                                      : metadataValue(m_spec,
                                          {"outputBundleScope", "output_bundle_scope",
                                           "outputScope", "output_scope"});
    };
    TensorBundle bundle;
    bundle.name = namedOutputs.size() == 1 ? namedOutputs.front().name
                                           : std::string("onnx-output-bundle");
    auto local = std::make_shared<LocalTensorBundle>();
    local->tensors = namedOutputs;
    local->owner = localOwner;
    bundle.localTensors = std::move(local);
    // The wire size is what this value would cost if it crossed a boundary, so a budget that
    // reads it plans for the real object rather than for zero.
    std::vector<TensorBundleEncodingMember> members;
    members.reserve(namedOutputs.size());
    for (const auto& tensor : namedOutputs) {
      TensorBundleEncodingMember member;
      member.nameBytes = tensor.name.size();
      member.rank = tensor.shape.size();
      member.payloadBytes = tensor.payload.size();
      members.push_back(member);
    }
    bundle.expectedBytes = tensorBundleEncodedSize(members);
    auto scope = localScope(bundle.name);
    if (scope.empty()) scope = "onnx-output-bundle";
    result.emplace(std::move(scope), std::move(bundle));
  }
  else if (namedOutputs.size() == 1 && !forceEncodedOutput) {
    const auto scope = outputScopeFor(m_spec, namedOutputs.front().name, 0);
    TensorBundle bundle;
    bundle.name = namedOutputs.front().name;
    if (!namedOutputs.front().payload.empty()) {
      bundle.payload.assign(namedOutputs.front().payload.begin(), namedOutputs.front().payload.end());
    }
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
      appendOwnedOutput(std::move(padding));
    }
    NativeWorkScope packWork(ctx.workStatistics, "onnx-output-bundle-encode",
                             NativeWorkKind::LOCAL);
    TensorBundle bundle = makeEncodedTensorBundleViews("onnx-output-bundle", namedOutputs);
    packWork.finish();
    const auto bundleScope = metadataValue(
      m_spec,
      {"outputBundleScope", "output_bundle_scope", "outputScope", "output_scope"});
    result.emplace(bundleScope.empty() ? "onnx-output-bundle" : bundleScope,
                   std::move(bundle));
  }
  if (residentState && effectiveContext.streamingStateExecution) {
    // The Provider decode-state transaction still needs an exact predecessor
    // record, but the complete resident allocation must not cross the
    // host/NDN boundary. Return only a small opaque handle; the next epoch
    // ignores its bytes and rebinds the retained Ort::Value from the request
    // state map. Conversation promotion separately moves the typed state into
    // the adapter-owned conversation cache.
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
    stateHandle.token = std::string(deviceResidentState
      ? "ndnsf-device-state-v1:" : "ndnsf-resident-state-v1:") +
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
    auto kvTensors = selectTensorViews(namedOutputs, kvOutputNames);
    const auto kvScope = metadataValue(
      m_spec, {"kvOutputScope", "kv_output_scope"});
    result.emplace(
      kvScope.empty() ? "kv-state" : kvScope,
      makeEncodedTensorBundleViews("kv-state", kvTensors));
  }
  outputWork.finish();
  if (ctx.workStatistics) {
    std::uint64_t auxiliaryBytes = 0;
    for (const auto& tensor : auxiliaryOutputs) auxiliaryBytes += tensor.payload.size();
    std::uint64_t finalBytes = 0;
    for (const auto& output : result) finalBytes += output.second.payload.size();
    ctx.workStatistics->addCount("onnx.outputBorrowedPayloadBytes", borrowedOutputBytes);
    ctx.workStatistics->addCount("onnx.outputOrtIntermediateCopyBytes", 0);
    ctx.workStatistics->addCount("onnx.outputAuxiliaryBytes", auxiliaryBytes);
    ctx.workStatistics->addCount("onnx.outputFinalBytes", finalBytes);
  }
  {
    // Ort input wrappers must release borrowed context buffers before host
    // cleanup. Their destruction may call the backend allocator.
    NativeWorkScope inputRelease(ctx.workStatistics, "onnx-input-release",
                                 NativeWorkKind::UNCLASSIFIED);
    ioBinding.reset();
    inputValues.clear();
  }
  {
    NativeWorkScope hostCleanup(ctx.workStatistics, "onnx-host-cleanup", NativeWorkKind::LOCAL);
    std::vector<NamedTensorView>{}.swap(namedOutputs);
    std::vector<NamedTensor>{}.swap(auxiliaryOutputs);
    std::vector<std::vector<std::uint8_t>>{}.swap(outputHostBuffers);
    std::vector<std::vector<std::uint8_t>>{}.swap(inputBuffers);
    redistributedContext.reset();
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
  const bool residentState = true;
  struct ResidentStateCleanup
  {
    Impl* impl = nullptr;
    std::string sessionId;

    ~ResidentStateCleanup()
    {
      if (impl == nullptr || sessionId.empty()) {
        return;
      }
      std::lock_guard<std::mutex> lock(impl->executionMutex);
      impl->eraseSessionStateLocked(sessionId);
    }
  } residentStateCleanup{
    residentState ? m_impl.get() : nullptr,
    residentState ? ctx.sessionId : std::string(),
  };
  epochContext.streamingStateExecution = residentState;
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
    std::vector<const char*> symbolicDimensions(tensorInfo.GetDimensionsCount());
    if (!symbolicDimensions.empty()) {
      tensorInfo.GetSymbolicDimensions(symbolicDimensions.data(), symbolicDimensions.size());
    }
    std::vector<std::string> symbolicDimensionNames;
    symbolicDimensionNames.reserve(symbolicDimensions.size());
    for (const auto* symbolic : symbolicDimensions) {
      symbolicDimensionNames.emplace_back(symbolic == nullptr ? "" : symbolic);
    }
    const auto shape = shapeForInput(
      m_spec, name, inputIndex, tensorInfo.GetShape(), true,
      symbolicDimensionNames);
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

    if (!residentState) {
      for (const auto& stateOutput : m_impl->statefulIo->stateOutputNames) {
        const auto& tensor = findTensor(tensors, stateOutput);
        const auto nextInput = m_impl->statefulIo->stateInputForOutput(stateOutput);
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

  if (residentState) {
    std::lock_guard<std::mutex> stateLock(m_impl->executionMutex);
    const auto deviceFound = m_impl->deviceStateBySession.find(ctx.sessionId);
    const auto hostFound = m_impl->hostStateBySession.find(ctx.sessionId);
    if (deviceFound == m_impl->deviceStateBySession.end() &&
        hostFound == m_impl->hostStateBySession.end()) {
      throw std::runtime_error(
        "streaming ONNX generation completed without resident state");
    }
    const auto& found = deviceResidentState ? deviceFound->second : hostFound->second;
    std::vector<NamedTensor> stateTensors;
    stateTensors.reserve(m_impl->statefulIo->stateOutputNames.size());
    for (const auto& outputName : m_impl->statefulIo->stateOutputNames) {
      const auto inputName = m_impl->statefulIo->stateInputForOutput(outputName);
      const auto state = found.find(inputName);
      if (state == found.end() || !state->second.IsTensor()) {
        throw std::runtime_error(
          "streaming ONNX generation completed without resident state: " +
          inputName);
      }
      const auto info = state->second.GetTensorTypeAndShapeInfo();
      const auto elementType = fromOnnxElementType(info.GetElementType());
      const auto count = info.GetElementCount();
      std::vector<std::uint8_t> host(count * tensorElementByteSize(elementType));
      const auto shape = info.GetShape();
      copyOrtTensorToHost(state->second, host.data(), host.size());
      if (deviceResidentState) {
        m_impl->runtimeMetrics.stateDeviceToHostBytes += host.size();
      }
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
  registerOnnxRuntimeBackend(factory, nullptr);
}

void
registerOnnxRuntimeBackend(
  RegistryNativeModelRunnerFactory& factory,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache)
{
  registerOnnxRuntimeBackend(factory, std::move(sessionCache), nullptr);
}

void
registerOnnxRuntimeBackend(
  RegistryNativeModelRunnerFactory& factory,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache,
  std::shared_ptr<OnnxKvStateObserver> stateObserver)
{
  const auto creator = [sessionCache = std::move(sessionCache),
                        stateObserver = std::move(stateObserver)] (
                         const NativeModelRunnerSpec& spec) {
    if (sessionCache) {
      // Keep eviction owner-local and wakeable at the next runner admission;
      // no background thread is needed for the bounded one-slot cache.
      sessionCache->evictIdle();
    }
    return std::make_shared<OnnxRuntimeModelRunner>(spec, sessionCache, stateObserver);
  };
  // Explicit CPU/CUDA backend names constrain provider selection. Generic
  // onnxruntime retains the metadata-selected provider and legacy CPU default.
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
  // Both CUDA and CPU retain the predecessor Ort::Value inside the Provider
  // for the duration of a streamed request.  The handle is only an internal
  // epoch/control reference; conversation promotion below moves the actual
  // typed state into the adapter-owned conversation cache.
  return m_impl != nullptr && m_impl->statefulIo.has_value();
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
  return m_impl != nullptr && m_impl->statefulIo.has_value();
}

namespace {

void
checkPortableOperation(const NativePortableKvLimitsV1& limits)
{
  if (limits.maxPayloadBytes == 0 || limits.maxEncodedBytes < limits.maxPayloadBytes)
    throw std::invalid_argument("portable-kv-invalid-limits");
  if (limits.cancelled && limits.cancelled())
    throw std::runtime_error("portable-kv-cancelled");
}

bool
sameConversationHandle(const NativeConversationStateHandleV1& a,
                       const NativeConversationStateHandleV1& b)
{
  return a.conversationKey == b.conversationKey && a.logicalBytes == b.logicalBytes &&
    a.opaque.providerIdentity == b.opaque.providerIdentity &&
    a.opaque.providerBootId == b.opaque.providerBootId &&
    a.opaque.sessionId == b.opaque.sessionId && a.opaque.role == b.opaque.role &&
    a.opaque.token == b.opaque.token &&
    a.opaque.stateInferenceEpoch == b.opaque.stateInferenceEpoch;
}

void
checkPortableRunnerIdentity(const NativeModelRunnerSpec& spec,
                            const DecodeStateIdentityV1& identity)
{
  identity.validate();
  if (identity.roleName != spec.role ||
      identity.providerIdentity != runnerMetadataValue(spec,
        {"providerIdentity", "provider_identity", "evidence.providerName", "providerName", "provider_name"}) ||
      identity.providerBootId != runnerMetadataValue(spec,
        {"providerBootId", "provider_boot_id", "evidence.providerBootId"}))
    throw std::invalid_argument("portable-kv-runner-identity-mismatch");
}

std::string
portableStateIoDigest(const StatefulOnnxIoContractV1& io)
{
  NativeJson pairs = NativeJson::array();
  for (const auto& output : io.stateOutputNames)
    pairs.push_back(NativeJson{{"input", io.stateInputForOutput(output)}, {"output", output}});
  auto families = io.stateFamilies;
  // An explicit map implementing the legacy pairs has the same semantics.
  const std::vector<std::string> legacy{"attention_kv", "recurrent_state", "convolution_state"};
  if (families.empty() && std::all_of(legacy.begin(), legacy.end(), [&](const auto& family) {
      const auto output = family + "_out";
      return std::find(io.stateOutputNames.begin(), io.stateOutputNames.end(), output) != io.stateOutputNames.end() &&
             io.stateInputForOutput(output) == family + "_in";
    })) families = legacy;
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-portable-state-io-v1"}, {"version", 1},
    {"inputs", io.stateInputNames}, {"outputs", io.stateOutputNames},
    {"families", families}, {"successors", pairs}}));
}

std::uint64_t
portableTensorBytes(TensorElementType type, const std::vector<std::int64_t>& shape)
{
  if (shape.size() > 16) throw std::invalid_argument("portable-kv-rank-exceeded");
  std::uint64_t count = 1;
  for (const auto dim : shape) {
    if (dim < 0 || dim > (std::int64_t{1} << 31) ||
        (dim != 0 && count > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(dim)))
      throw std::invalid_argument("portable-kv-shape-overflow");
    count *= static_cast<std::uint64_t>(dim);
  }
  const auto width = tensorElementByteSize(type);
  if (count > std::numeric_limits<std::uint64_t>::max() / width)
    throw std::invalid_argument("portable-kv-byte-overflow");
  return count * width;
}

} // namespace

bool
OnnxRuntimeModelRunner::supportsPortableConversationState() const
{
  if (!supportsConversationStateTransfer() || m_impl->statefulIo->stateInputNames.empty()) return false;
  try {
    const auto& io = *m_impl->statefulIo;
    for (const auto& name : io.stateInputNames) {
      const auto input = std::find(io.inputNames.begin(), io.inputNames.end(), name);
      if (input == io.inputNames.end()) return false;
      const auto type = m_impl->session.GetInputTypeInfo(static_cast<std::size_t>(input - io.inputNames.begin()));
      (void)fromOnnxElementType(type.GetTensorTypeAndShapeInfo().GetElementType());
    }
    if (m_impl->selection.selectedProvider == "cpu") return true;
    if (m_impl->selection.selectedProvider == "cuda") return CudaRuntimeApi{}.canSelectDevice();
  }
  catch (...) { }
  return false;
}

NativePortableKvSnapshotV1
OnnxRuntimeModelRunner::exportConversationState(
  const NativeConversationStateHandleV1& state,
  const DecodeStateIdentityV1& committedIdentity,
  const NativePortableKvLimitsV1& limits)
{
  checkPortableOperation(limits);
  state.validate();
  checkPortableRunnerIdentity(m_spec, committedIdentity);
  if (!supportsPortableConversationState()) throw std::runtime_error("portable-kv-unsupported");
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto handle = m_impl->stateHandleByConversation.find(state.conversationKey);
  if (handle == m_impl->stateHandleByConversation.end() || !sameConversationHandle(handle->second, state) ||
      state.opaque.providerIdentity != committedIdentity.providerIdentity ||
      state.opaque.providerBootId != committedIdentity.providerBootId ||
      state.opaque.role != committedIdentity.roleName ||
      state.opaque.stateInferenceEpoch != committedIdentity.stateInferenceEpoch)
    throw std::invalid_argument("portable-kv-origin-handle-mismatch");
  const auto device = m_impl->deviceStateByConversation.find(state.conversationKey);
  const auto cpu = m_impl->residentHostStateByConversation.find(state.conversationKey);
  const auto host = m_impl->hostStateByConversation.find(state.conversationKey);
  const unsigned tiers = (device != m_impl->deviceStateByConversation.end()) +
                         (cpu != m_impl->residentHostStateByConversation.end()) +
                         (host != m_impl->hostStateByConversation.end());
  if (tiers != 1) throw std::runtime_error("portable-kv-source-tier-invalid");
  const auto* ortState = device != m_impl->deviceStateByConversation.end() ? &device->second :
                        cpu != m_impl->residentHostStateByConversation.end() ? &cpu->second : nullptr;
  const auto& names = m_impl->statefulIo->stateInputNames;
  if (names.size() > 256) throw std::invalid_argument("portable-kv-component-count-exceeded");
  if ((ortState ? ortState->size() : host->second.size()) != names.size())
    throw std::runtime_error("portable-kv-source-component-set-invalid");
  NativePortableKvSnapshotV1 snapshot;
  snapshot.originIdentity = committedIdentity;
  snapshot.stateIoDigest = portableStateIoDigest(*m_impl->statefulIo);
  // Preflight every component before allocating any tensor payload.
  for (const auto& name : names) {
    checkPortableOperation(limits);
    NativePortableKvComponentV1 component;
    component.inputName = name;
    if (ortState) {
      const auto found = ortState->find(name);
      if (found == ortState->end() || !found->second.IsTensor())
        throw std::runtime_error("portable-kv-source-component-missing");
      const auto info = found->second.GetTensorTypeAndShapeInfo();
      component.elementType = fromOnnxElementType(info.GetElementType());
      component.shape = info.GetShape();
    }
    else {
      const auto found = host->second.find(name);
      if (found == host->second.end()) throw std::runtime_error("portable-kv-source-component-missing");
      component.elementType = found->second.elementType;
      component.shape = found->second.shape;
    }
    component.payloadBytes = portableTensorBytes(component.elementType, component.shape);
    if (!ortState && host->second.at(name).payload.size() != component.payloadBytes)
      throw std::runtime_error("portable-kv-source-byte-mismatch");
    const auto wireBytes = tensorBundleEncodedSize({{name.size(), component.shape.size(), component.payloadBytes}});
    if (component.payloadBytes > limits.maxPayloadBytes - snapshot.logicalBytes ||
        wireBytes > limits.maxEncodedBytes - snapshot.encodedBytes)
      throw std::invalid_argument("portable-kv-budget-exceeded");
    snapshot.logicalBytes += component.payloadBytes;
    snapshot.encodedBytes += wireBytes;
    snapshot.components.push_back(std::move(component));
  }
  if (snapshot.logicalBytes != state.logicalBytes) throw std::runtime_error("portable-kv-source-byte-mismatch");
  for (auto& component : snapshot.components) {
    checkPortableOperation(limits);
    NamedTensor tensor{component.inputName, component.elementType, component.shape, {}};
    tensor.payload.resize(static_cast<std::size_t>(component.payloadBytes));
    if (ortState) {
      if (!tensor.payload.empty()) {
        copyOrtTensorToHost(ortState->at(component.inputName), tensor.payload.data(), tensor.payload.size());
        if (device != m_impl->deviceStateByConversation.end())
          m_impl->runtimeMetrics.stateDeviceToHostBytes += tensor.payload.size();
      }
    }
    else tensor.payload = host->second.at(component.inputName).payload;
    std::vector<NamedTensor> scratch;
    scratch.emplace_back(std::move(tensor));
    component.tensorWire = encodeTensorBundle(scratch);
    component.contentDigest = sha256TensorBytes(component.tensorWire);
  }
  validateNativePortableKvSnapshot(snapshot, limits);
  return snapshot;
}

NativeConversationStateHandleV1
OnnxRuntimeModelRunner::importConversationState(
  const NativePortableKvSnapshotV1& snapshot,
  const DecodeStateIdentityV1& destinationIdentity,
  const std::string& destinationConversationKey,
  const NativePortableKvLimitsV1& limits)
{
  validateNativePortableKvSnapshot(snapshot, limits);
  checkPortableRunnerIdentity(m_spec, destinationIdentity);
  if (!supportsPortableConversationState()) throw std::runtime_error("portable-kv-unsupported");
  if (destinationConversationKey.empty() || !nativePortableKvIdentityCompatible(snapshot.originIdentity, destinationIdentity))
    throw std::invalid_argument("portable-kv-destination-incompatible");
  std::lock_guard<std::mutex> lock(m_impl->executionMutex);
  const auto& io = *m_impl->statefulIo;
  if (snapshot.stateIoDigest != portableStateIoDigest(io) || snapshot.components.size() != io.stateInputNames.size())
    throw std::invalid_argument("portable-kv-state-io-mismatch");
  for (std::size_t i = 0; i < snapshot.components.size(); ++i) {
    checkPortableOperation(limits);
    const auto& component = snapshot.components[i];
    if (component.inputName != io.stateInputNames[i]) throw std::invalid_argument("portable-kv-component-order-mismatch");
    const auto input = std::find(io.inputNames.begin(), io.inputNames.end(), component.inputName);
    if (input == io.inputNames.end()) throw std::invalid_argument("portable-kv-component-input-missing");
    const auto type = m_impl->session.GetInputTypeInfo(static_cast<std::size_t>(input - io.inputNames.begin()));
    const auto info = type.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (fromOnnxElementType(info.GetElementType()) != component.elementType || shape.size() != component.shape.size())
      throw std::invalid_argument("portable-kv-component-schema-mismatch");
    for (std::size_t axis = 0; axis < shape.size(); ++axis)
      if (shape[axis] >= 0 && shape[axis] != component.shape[axis])
        throw std::invalid_argument("portable-kv-component-shape-mismatch");
  }
  const auto& key = destinationConversationKey;
  const auto& session = destinationIdentity.requestId;
  if (m_impl->deviceStateByConversation.count(key) || m_impl->residentHostStateByConversation.count(key) ||
      m_impl->hostStateByConversation.count(key) || m_impl->externalDeviceAllocationsByConversation.count(key) ||
      m_impl->stateHandleByConversation.count(key) || m_impl->deviceStateBySession.count(session) ||
      m_impl->hostStateBySession.count(session) || m_impl->stateHandleBySession.count(session) ||
      m_impl->conversationBindingBySession.count(session))
    throw std::runtime_error("portable-kv-destination-collision");
  NativeConversationStateHandleV1 result;
  result.opaque.providerIdentity = destinationIdentity.providerIdentity;
  result.opaque.providerBootId = destinationIdentity.providerBootId;
  result.opaque.sessionId = session;
  result.opaque.role = destinationIdentity.roleName;
  result.opaque.stateInferenceEpoch = destinationIdentity.stateInferenceEpoch;
  const bool deviceResident = m_impl->selection.selectedProvider == "cuda";
  static std::atomic<std::uint64_t> importSequence{0};
  result.opaque.token = std::string(deviceResident ? "ndnsf-device-state-v1:" : "ndnsf-resident-state-v1:") +
                        "import:" + std::to_string(cacheIdentity()) + ":" +
                        std::to_string(++importSequence);
  result.conversationKey = key;
  result.logicalBytes = static_cast<std::size_t>(snapshot.logicalBytes);
  result.validate();
  CudaRuntimeApi cuda;
  Ort::AllocatorWithDefaultOptions allocator;
  std::map<std::string, Ort::Value> tensors;
  std::map<std::string, void*> allocations;
  struct Cleanup {
    CudaRuntimeApi& cuda;
    std::map<std::string, Ort::Value>& tensors;
    std::map<std::string, void*>& allocations;
    int priorDevice = 0;
    bool deviceSelected = false;
    ~Cleanup() {
      tensors.clear();
      freeCudaPointers(cuda, allocations);
      if (deviceSelected) (void)cuda.setDevice(priorDevice);
    }
  } cleanup{cuda, tensors, allocations};
  std::optional<Ort::MemoryInfo> deviceMemory;
  if (deviceResident) {
    const auto deviceId = std::stoi(m_impl->selection.deviceId);
    if (!cuda.canSelectDevice() || cuda.getDevice(&cleanup.priorDevice) != 0 ||
        cuda.setDevice(deviceId) != 0)
      throw std::runtime_error("portable-kv-cuda-unavailable");
    cleanup.deviceSelected = true;
    deviceMemory.emplace("Cuda", OrtAllocatorType::OrtDeviceAllocator, deviceId, OrtMemTypeDefault);
  }
  for (const auto& component : snapshot.components) {
    checkPortableOperation(limits);
    auto decoded = decodeTensorBundle(component.tensorWire);
    const auto& tensor = decoded.front();
    Ort::Value value{nullptr};
    if (deviceResident) {
      auto raw = allocations.emplace(component.inputName, nullptr).first;
      if (!tensor.payload.empty() &&
          (cuda.malloc(&raw->second, tensor.payload.size()) != 0 || raw->second == nullptr ||
           cuda.memcpy(raw->second, tensor.payload.data(), tensor.payload.size(), cudaMemcpyHostToDevice) != 0))
        throw std::runtime_error("portable-kv-cuda-copy-failed");
      value = Ort::Value::CreateTensor(*deviceMemory, raw->second, tensor.payload.size(),
        tensor.shape.data(), tensor.shape.size(), toOnnxElementType(tensor.elementType));
    }
    else {
      value = Ort::Value::CreateTensor(allocator, tensor.shape.data(), tensor.shape.size(), toOnnxElementType(tensor.elementType));
      if (!tensor.payload.empty()) std::memcpy(value.GetTensorMutableRawData(), tensor.payload.data(), tensor.payload.size());
    }
    tensors.emplace(component.inputName, std::move(value));
  }
  checkPortableOperation(limits);
  try {
    if (deviceResident) {
      m_impl->deviceStateByConversation.emplace(key, std::move(tensors));
      m_impl->externalDeviceAllocationsByConversation.emplace(key, std::move(allocations));
    }
    else m_impl->residentHostStateByConversation.emplace(key, std::move(tensors));
    m_impl->stateHandleByConversation.emplace(key, result);
  }
  catch (...) { m_impl->releaseConversationLocked(key); throw; }
  if (deviceResident) m_impl->runtimeMetrics.stateHostToDeviceBytes += snapshot.logicalBytes;
  return result;
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
  const bool deviceResident = m_impl->selection.selectedProvider == "cuda";
  const auto deviceFound = m_impl->deviceStateBySession.find(sessionId);
  const auto hostFound = m_impl->hostStateBySession.find(sessionId);
  const bool hasDeviceState = deviceFound != m_impl->deviceStateBySession.end() &&
    !deviceFound->second.empty();
  const bool hasHostState = hostFound != m_impl->hostStateBySession.end() &&
    !hostFound->second.empty();
  if ((!hasDeviceState && !hasHostState) ||
      m_impl->deviceStateByConversation.count(conversationKey) != 0 ||
      m_impl->residentHostStateByConversation.count(conversationKey) != 0 ||
      m_impl->hostStateByConversation.count(conversationKey) != 0) {
    return std::nullopt;
  }

  const auto& sessionState = deviceResident ? deviceFound->second : hostFound->second;
  std::size_t logicalBytes = 0;
  for (const auto& item : sessionState) {
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
    opaque.token = std::string(deviceResident
      ? "ndnsf-device-state-v1:" : "ndnsf-resident-state-v1:") + sessionId + ":" +
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
  if (deviceResident) {
    m_impl->deviceStateByConversation.emplace(
      conversationKey, std::move(deviceFound->second));
    m_impl->deviceStateBySession.erase(deviceFound);
  }
  else {
    m_impl->residentHostStateByConversation.emplace(
      conversationKey, std::move(hostFound->second));
    m_impl->hostStateBySession.erase(hostFound);
  }
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
  const bool deviceResident = m_impl->selection.selectedProvider == "cuda";
  const auto deviceRetained = m_impl->deviceStateByConversation.find(
    state.conversationKey);
  const bool retained = deviceResident
    ? deviceRetained != m_impl->deviceStateByConversation.end() &&
      !deviceRetained->second.empty()
    : m_impl->residentHostStateByConversation.count(state.conversationKey) != 0 &&
      !m_impl->residentHostStateByConversation.at(state.conversationKey).empty();
  if (!retained ||
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
  if (m_impl->selection.selectedProvider != "cuda") {
    return m_impl->residentHostStateByConversation.count(state.conversationKey) != 0;
  }
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
  if (!m_impl || m_impl->selection.selectedProvider != "cuda") {
    // CPU conversation state is already in the host ORT memory domain used
    // by the next request.  The store may still call this method after an
    // accounting-only host pause; no data movement is required.
    return std::async(std::launch::deferred, [] { return true; });
  }
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
    m_impl->residentHostStateByConversation.count(state.conversationKey) != 0 ||
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

// Keep the ABI-stable PImpl complete in a build without the optional ONNX
// Runtime development package.  The public header therefore has the same
// layout in enabled and disabled builds; only construction/execution reports
// that the backend is unavailable.
class OnnxRuntimeModelRunner::Impl
{
};

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
  : m_spec(std::move(spec))
{
  throw std::runtime_error(
    "C++ ONNX Runtime backend is not enabled; install the ONNX Runtime "
    "C++ development package and build with NDNSF_DI_ENABLE_ONNXRUNTIME_CPP");
}

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(
  NativeModelRunnerSpec spec,
  std::shared_ptr<OnnxRuntimeSessionCache>)
  : OnnxRuntimeModelRunner(std::move(spec))
{
}

OnnxRuntimeModelRunner::OnnxRuntimeModelRunner(
  NativeModelRunnerSpec spec, std::shared_ptr<OnnxRuntimeSessionCache> sessionCache,
  std::shared_ptr<OnnxKvStateObserver>)
  : OnnxRuntimeModelRunner(std::move(spec), std::move(sessionCache))
{
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

bool
OnnxRuntimeModelRunner::supportsPortableConversationState() const
{
  return false;
}

NativePortableKvSnapshotV1
OnnxRuntimeModelRunner::exportConversationState(
  const NativeConversationStateHandleV1& state,
  const DecodeStateIdentityV1& identity,
  const NativePortableKvLimitsV1& limits)
{
  return NativeModelRunner::exportConversationState(state, identity, limits);
}

NativeConversationStateHandleV1
OnnxRuntimeModelRunner::importConversationState(
  const NativePortableKvSnapshotV1& snapshot,
  const DecodeStateIdentityV1& identity,
  const std::string& key,
  const NativePortableKvLimitsV1& limits)
{
  return NativeModelRunner::importConversationState(snapshot, identity, key, limits);
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
OnnxRuntimeModelRunner::runStreamed(const RoleExecutionContext&)
{
  return std::nullopt;
}

std::optional<std::map<std::string, TensorBundle>>
OnnxRuntimeModelRunner::runStreamedImpl(const RoleExecutionContext&)
{
  return std::nullopt;
}

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  registerOnnxRuntimeBackend(factory, nullptr);
}

void
registerOnnxRuntimeBackend(
  RegistryNativeModelRunnerFactory& factory,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache)
{
  registerOnnxRuntimeBackend(factory, std::move(sessionCache), nullptr);
}

void
registerOnnxRuntimeBackend(
  RegistryNativeModelRunnerFactory& factory,
  std::shared_ptr<OnnxRuntimeSessionCache> sessionCache,
  std::shared_ptr<OnnxKvStateObserver> stateObserver)
{
  const auto creator = [sessionCache = std::move(sessionCache),
                        stateObserver = std::move(stateObserver)] (
                         const NativeModelRunnerSpec& spec) {
    return std::make_shared<OnnxRuntimeModelRunner>(spec, sessionCache, stateObserver);
  };
  // Keep the public adapter backend names distinct from the internal
  // execution-provider selection stored in runner metadata.
  factory.registerBackend("onnxruntime", creator);
  factory.registerBackend("onnxruntime-cpu", creator);
  factory.registerBackend("onnxruntime-cuda", creator);
}

} // namespace ndnsf::di

#endif // NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
