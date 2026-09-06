#ifndef NDNSF_DI_CUDA_DEVICE_IDENTITY_HPP
#define NDNSF_DI_CUDA_DEVICE_IDENTITY_HPP

#include <dlfcn.h>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ndnsf::di {

/** Resolve the selected CUDA runtime ordinal through PCI to a physical UUID.
 * No toolkit link dependency, shell command, or caller-supplied identity.
 * PCI mapping avoids confusing runtime-visible and driver device ordinals.
 */
inline std::string queryCudaDeviceUuid(int runtimeOrdinal)
{
  if (runtimeOrdinal < 0) throw std::invalid_argument("CUDA_IDENTITY_INVALID_ORDINAL");
  const auto closeLibrary = [](void* handle) { if (handle) dlclose(handle); };
  using Library = std::unique_ptr<void, decltype(closeLibrary)>;
  Library runtime(dlopen("libcudart.so.12", RTLD_NOW | RTLD_LOCAL), closeLibrary);
  if (!runtime) runtime.reset(dlopen("libcudart.so", RTLD_NOW | RTLD_LOCAL));
  Library driver(dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL), closeLibrary);
  if (!runtime || !driver) throw std::runtime_error("CUDA_IDENTITY_LIBRARY_UNAVAILABLE");
  using GetPci = int (*)(char*, int, int);
  using Init = int (*)(unsigned int);
  using GetByPci = int (*)(int*, const char*);
  struct Uuid { unsigned char bytes[16]; };
  using GetUuid = int (*)(Uuid*, int);
  const auto getPci = reinterpret_cast<GetPci>(dlsym(runtime.get(), "cudaDeviceGetPCIBusId"));
  const auto init = reinterpret_cast<Init>(dlsym(driver.get(), "cuInit"));
  const auto getByPci = reinterpret_cast<GetByPci>(dlsym(driver.get(), "cuDeviceGetByPCIBusId"));
  const auto getUuid = reinterpret_cast<GetUuid>(dlsym(driver.get(), "cuDeviceGetUuid"));
  if (!getPci || !init || !getByPci || !getUuid) {
    throw std::runtime_error("CUDA_IDENTITY_SYMBOL_UNAVAILABLE");
  }
  char pci[32]{};
  int device = -1;
  Uuid uuid{};
  if (getPci(pci, sizeof(pci), runtimeOrdinal) != 0 || pci[0] == '\0' ||
      init(0) != 0 || getByPci(&device, pci) != 0 || getUuid(&uuid, device) != 0 ||
      std::all_of(std::begin(uuid.bytes), std::end(uuid.bytes),
                  [](unsigned char value) { return value == 0; })) {
    throw std::runtime_error("CUDA_IDENTITY_QUERY_FAILED");
  }
  std::ostringstream output;
  output << "GPU-" << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) output << '-';
    output << std::setw(2) << static_cast<unsigned int>(uuid.bytes[i]);
  }
  return output.str();
}

} // namespace ndnsf::di
#endif
