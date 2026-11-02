#ifndef NDNSF_DI_CUDA_DEVICE_MEMORY_HPP
#define NDNSF_DI_CUDA_DEVICE_MEMORY_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <dlfcn.h>

namespace ndnsf::di {

/** Provider-owned device binding; memory values are queried afresh on every call. */
class CudaDeviceMemoryQuery
{
private:
  using Initialize = int (*)();
  using GetDevice = int (*)(const char*, void**);
  struct Memory { unsigned long long total; unsigned long long free; unsigned long long used; };
  using GetMemory = int (*)(void*, Memory*);

  struct LibraryCloser
  {
    void operator()(void* handle) const
    {
      if (handle) dlclose(handle);
    }
  };

public:
  explicit CudaDeviceMemoryQuery(int ordinal)
    : m_ordinal(ordinal)
  {
    if (ordinal < 0) throw std::invalid_argument("CUDA_IDENTITY_INVALID_ORDINAL");
  }

  CudaDeviceMemoryQuery(const CudaDeviceMemoryQuery&) = delete;
  CudaDeviceMemoryQuery& operator=(const CudaDeviceMemoryQuery&) = delete;

  ~CudaDeviceMemoryQuery()
  {
    if (m_device) m_shutdown();
  }

  NativeDeviceResourceObservationV3
  query(std::uint64_t capturedAtMs, std::uint64_t sequence, std::uint64_t activeRequests,
        const std::shared_ptr<NativeWorkStatistics>& statistics = {})
  {
    NativeWorkScope wrapper(statistics, "gpu-resource-wrapper", NativeWorkKind::LOCAL);
    NativeWorkScope lockWait(statistics, "gpu-resource-lock", NativeWorkKind::WAIT);
    std::lock_guard<std::mutex> lock(m_mutex);
    lockWait.finish();
    if (!m_device) {
      NativeWorkScope initializeWork(statistics, "gpu-resource-initialize",
                                     NativeWorkKind::UNCLASSIFIED);
      initialize();
    }
    Memory memory{};
    const auto status = [&] {
      NativeWorkScope queryWork(statistics, "gpu-resource-query", NativeWorkKind::UNCLASSIFIED);
      return m_getMemory(m_device, &memory);
    }();
    NativeWorkScope fieldsWork(statistics, "gpu-resource-fields", NativeWorkKind::LOCAL);
    if (status != 0 ||
        memory.total == 0 || memory.free > memory.total || memory.used > memory.total)
      throw std::runtime_error("CUDA_MEMORY_QUERY_FAILED");
    constexpr std::uint64_t MIB = 1024 * 1024;
    NativeDeviceResourceObservationV3 result;
    result.device = "cuda:" + std::to_string(m_ordinal);
    result.totalMemoryMb = memory.total / MIB;
    result.freeMemoryMb = memory.free / MIB;
    result.activeRequests = activeRequests;
    result.resourceSequence = sequence;
    result.capturedAtMs = capturedAtMs;
    return result;
  }

private:
  void
  initialize()
  {
    const auto uuid = queryCudaDeviceUuid(m_ordinal);
    std::unique_ptr<void, LibraryCloser> library(dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL));
    if (!library) throw std::runtime_error("CUDA_MEMORY_NVML_UNAVAILABLE");
    const auto initialize = reinterpret_cast<Initialize>(dlsym(library.get(), "nvmlInit_v2"));
    const auto shutdown = reinterpret_cast<Initialize>(dlsym(library.get(), "nvmlShutdown"));
    const auto getDevice = reinterpret_cast<GetDevice>(
      dlsym(library.get(), "nvmlDeviceGetHandleByUUID"));
    const auto getMemory = reinterpret_cast<GetMemory>(dlsym(library.get(), "nvmlDeviceGetMemoryInfo"));
    if (!initialize || !shutdown || !getDevice || !getMemory || initialize() != 0)
      throw std::runtime_error("CUDA_MEMORY_NVML_INIT_FAILED");
    void* device = nullptr;
    if (getDevice(uuid.c_str(), &device) != 0 || !device) {
      shutdown();
      throw std::runtime_error("CUDA_MEMORY_QUERY_FAILED");
    }
    m_library = std::move(library);
    m_shutdown = shutdown;
    m_getMemory = getMemory;
    m_device = device;
  }

private:
  int m_ordinal;
  std::unique_ptr<void, LibraryCloser> m_library;
  Initialize m_shutdown = nullptr;
  GetMemory m_getMemory = nullptr;
  void* m_device = nullptr;
  std::mutex m_mutex;
};

/** Compatibility helper for a single independent observation. */
inline NativeDeviceResourceObservationV3
queryCudaDeviceMemory(int ordinal, std::uint64_t capturedAtMs,
                      std::uint64_t sequence, std::uint64_t activeRequests,
                      const std::shared_ptr<NativeWorkStatistics>& statistics = {})
{
  NativeWorkScope ownerWork(statistics, "gpu-resource-query-owner", NativeWorkKind::UNCLASSIFIED);
  CudaDeviceMemoryQuery query(ordinal);
  return query.query(capturedAtMs, sequence, activeRequests, statistics);
}

} // namespace ndnsf::di

#endif // NDNSF_DI_CUDA_DEVICE_MEMORY_HPP
