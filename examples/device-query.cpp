#include "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <dlfcn.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
using Library = std::unique_ptr<void, decltype(&dlclose)>;

template<typename Function>
Function
getSymbol(void* library, const char* name)
{
  const auto function = reinterpret_cast<Function>(dlsym(library, name));
  if (!function) {
    throw std::runtime_error(std::string("GPU_QUERY_SYMBOL_UNAVAILABLE:") + name);
  }
  return function;
}

std::string
requireEnvironment(const char* name)
{
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    throw std::runtime_error(std::string("GPU_QUERY_BINDING_MISSING:") + name);
  }
  return value;
}

Json
queryInventory()
{
  Library library(dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL), dlclose);
  if (!library) {
    throw std::runtime_error("GPU_QUERY_NVML_UNAVAILABLE");
  }
  const auto initialize = getSymbol<int (*)()>(library.get(), "nvmlInit_v2");
  const auto shutdown = getSymbol<int (*)()>(library.get(), "nvmlShutdown");
  const auto getCount = getSymbol<int (*)(unsigned int*)>(library.get(), "nvmlDeviceGetCount_v2");
  const auto getDevice = getSymbol<int (*)(unsigned int, void**)>(
    library.get(), "nvmlDeviceGetHandleByIndex_v2");
  const auto getUuid = getSymbol<int (*)(void*, char*, unsigned int)>(
    library.get(), "nvmlDeviceGetUUID");
  if (initialize() != 0) {
    throw std::runtime_error("GPU_QUERY_NVML_INIT_FAILED");
  }
  // Close the NVML session before the separately owned library handle.
  const auto closeSession = [shutdown](void*) { shutdown(); };
  const std::unique_ptr<void, decltype(closeSession)> session(library.get(), closeSession);
  unsigned int count = 0;
  if (getCount(&count) != 0 || count == 0 || count > 256) {
    throw std::runtime_error("GPU_QUERY_NVML_COUNT_INVALID");
  }
  Json devices = Json::array();
  std::set<std::string> uuids;
  for (unsigned int index = 0; index < count; ++index) {
    void* device = nullptr;
    char uuid[96]{};
    // A job cgroup may deny access to GPUs outside its allocation. Such an
    // index cannot satisfy a requested device or a visible CUDA UUID below.
    if (getDevice(index, &device) != 0 || getUuid(device, uuid, sizeof(uuid)) != 0) {
      continue;
    }
    if (uuid[sizeof(uuid) - 1] != '\0') {
      throw std::runtime_error("GPU_QUERY_NVML_UUID_UNTERMINATED");
    }
    const std::string identity(uuid);
    if (identity.compare(0, 4, "GPU-") != 0 || !uuids.insert(identity).second) {
      throw std::runtime_error("GPU_QUERY_NVML_UUID_INVALID");
    }
    devices.push_back({{"physical_id", std::to_string(index)}, {"uuid", identity}});
  }
  return devices;
}

Json
queryVisible(unsigned int expected)
{
  Library runtime(dlopen("libcudart.so.12", RTLD_NOW | RTLD_LOCAL), dlclose);
  if (!runtime) {
    runtime.reset(dlopen("libcudart.so", RTLD_NOW | RTLD_LOCAL));
  }
  if (!runtime) {
    throw std::runtime_error("GPU_QUERY_CUDA_RUNTIME_UNAVAILABLE");
  }
  const auto getCount = getSymbol<int (*)(int*)>(runtime.get(), "cudaGetDeviceCount");
  int count = 0;
  if (getCount(&count) != 0 || count != static_cast<int>(expected)) {
    throw std::runtime_error("GPU_QUERY_VISIBLE_COUNT_MISMATCH");
  }
  Json devices = Json::array();
  std::set<std::string> uuids;
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    const auto uuid = ndnsf::di::queryCudaDeviceUuid(ordinal);
    if (!uuids.insert(uuid).second) {
      throw std::runtime_error("GPU_QUERY_VISIBLE_UUID_NOT_UNIQUE");
    }
    devices.push_back(uuid);
  }
  return devices;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 3 || std::string(argv[1]) != "--count") {
      throw std::invalid_argument("GPU_QUERY_USAGE: --count N (1..64)");
    }
    const std::string text(argv[2]);
    if (text.empty() || text.size() > 2 || text[0] == '0' ||
        text.find_first_not_of("0123456789") != std::string::npos) {
      throw std::invalid_argument("GPU_QUERY_COUNT_INVALID");
    }
    const auto count = std::stoul(text);
    if (count == 0 || count > 64) {
      throw std::invalid_argument("GPU_QUERY_COUNT_INVALID");
    }
    Json result = {{"event", "NDNSF_DI_GPU_PROBE"}, {"schema", "ndnsf-di-gpu-probe-v1"}};
    for (const auto& binding : {
           std::pair{"boot_id", "NDNSF_DEPLOYMENT_BOOT_ID"},
           std::pair{"configuration_sha256", "NDNSF_DEPLOYMENT_CONFIG_SHA256"},
           std::pair{"release_sha256", "NDNSF_DEPLOYMENT_RELEASE_SHA256"},
           std::pair{"role_id", "NDNSF_DEPLOYMENT_ACTOR_ID"},
           std::pair{"job_id", "SLURM_JOB_ID"}, std::pair{"step_id", "SLURM_STEP_ID"}}) {
      result[binding.first] = requireEnvironment(binding.second);
    }
    result["device_order"] = requireEnvironment("CUDA_DEVICE_ORDER");
    result["visibility"] = requireEnvironment("CUDA_VISIBLE_DEVICES");
    result["step_gpus"] = requireEnvironment("SLURM_STEP_GPUS");
    char hostname[256]{};
    if (::gethostname(hostname, sizeof(hostname) - 1) != 0) {
      throw std::runtime_error("GPU_QUERY_HOST_UNAVAILABLE");
    }
    result["host"] = hostname;
    if (result["device_order"] != "PCI_BUS_ID") {
      throw std::runtime_error("GPU_QUERY_DEVICE_ORDER_INVALID");
    }
    result["visible_uuids"] = queryVisible(static_cast<unsigned int>(count));
    result["inventory"] = queryInventory();
    for (const auto& uuid : result["visible_uuids"]) {
      unsigned int matches = 0;
      for (const auto& device : result["inventory"]) {
        matches += device.at("uuid") == uuid ? 1 : 0;
      }
      if (matches != 1) {
        throw std::runtime_error("GPU_QUERY_VISIBLE_UUID_NOT_IN_INVENTORY");
      }
    }
    std::cout << result.dump() << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
