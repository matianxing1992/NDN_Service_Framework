#ifndef NDNSF_DI_CUDA_DEVICE_MEMORY_HPP
#define NDNSF_DI_CUDA_DEVICE_MEMORY_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <dlfcn.h>

namespace ndnsf::di {

/** Measure memory for one runtime-visible GPU, resolving its physical UUID first. */
inline NativeDeviceResourceObservationV3
queryCudaDeviceMemory(int ordinal, std::uint64_t capturedAtMs,
                      std::uint64_t sequence, std::uint64_t activeRequests)
{
  const auto uuid = queryCudaDeviceUuid(ordinal);
  const auto closeLibrary = [] (void* handle) { if (handle) dlclose(handle); };
  using Library = std::unique_ptr<void, decltype(closeLibrary)>;
  Library library(dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL), closeLibrary);
  if (!library) throw std::runtime_error("CUDA_MEMORY_NVML_UNAVAILABLE");
  using Initialize = int (*)();
  using GetDevice = int (*)(const char*, void**);
  struct Memory { unsigned long long total; unsigned long long free; unsigned long long used; };
  using GetMemory = int (*)(void*, Memory*);
  const auto initialize = reinterpret_cast<Initialize>(dlsym(library.get(), "nvmlInit_v2"));
  const auto shutdown = reinterpret_cast<Initialize>(dlsym(library.get(), "nvmlShutdown"));
  const auto getDevice = reinterpret_cast<GetDevice>(
    dlsym(library.get(), "nvmlDeviceGetHandleByUUID"));
  const auto getMemory = reinterpret_cast<GetMemory>(
    dlsym(library.get(), "nvmlDeviceGetMemoryInfo"));
  if (!initialize || !shutdown || !getDevice || !getMemory || initialize() != 0)
    throw std::runtime_error("CUDA_MEMORY_NVML_INIT_FAILED");
  const auto closeSession = [shutdown] (void*) { shutdown(); };
  const std::unique_ptr<void, decltype(closeSession)> session(library.get(), closeSession);
  void* device = nullptr;
  Memory memory{};
  if (getDevice(uuid.c_str(), &device) != 0 || getMemory(device, &memory) != 0 ||
      memory.total == 0 || memory.free > memory.total || memory.used > memory.total)
    throw std::runtime_error("CUDA_MEMORY_QUERY_FAILED");
  constexpr std::uint64_t MIB = 1024 * 1024;
  NativeDeviceResourceObservationV3 result;
  result.device = "cuda:" + std::to_string(ordinal);
  result.totalMemoryMb = memory.total / MIB;
  result.freeMemoryMb = memory.free / MIB;
  result.activeRequests = activeRequests;
  result.resourceSequence = sequence;
  result.capturedAtMs = capturedAtMs;
  return result;
}

} // namespace ndnsf::di

#endif // NDNSF_DI_CUDA_DEVICE_MEMORY_HPP
