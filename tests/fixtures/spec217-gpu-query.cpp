// Controlled vendor ABI for the native device-query subprocess tests only.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool
scenario(const char* name)
{
  const auto* value = std::getenv("SPEC217_GPU_FIXTURE_SCENARIO");
  return value && std::strcmp(value, name) == 0;
}

} // namespace

extern "C" {

int
cudaGetDeviceCount(int* count)
{
  *count = scenario("count-mismatch") ? 1 : 2;
  return 0;
}

int
cudaDeviceGetPCIBusId(char* output, int size, int ordinal)
{
  if (ordinal < 0 || ordinal > 1 || size < 16) {
    return 1;
  }
  std::snprintf(output, static_cast<std::size_t>(size), "0000:%02d:00.0", ordinal == 0 ? 3 : 1);
  return 0;
}

int
cuInit(unsigned int)
{
  return 0;
}

int
cuDeviceGetByPCIBusId(int* device, const char* pci)
{
  *device = std::strcmp(pci, "0000:03:00.0") == 0 ? 3 : 1;
  return 0;
}

int
cuDeviceGetUuid(void* uuid, int device)
{
  std::memset(uuid, device == 3 || scenario("duplicate-visible") ? 0x33 : 0x11, 16);
  return 0;
}

int
nvmlInit_v2()
{
  return 0;
}

int
nvmlShutdown()
{
  return 0;
}

int
nvmlDeviceGetCount_v2(unsigned int* count)
{
  *count = 4;
  return 0;
}

int
nvmlDeviceGetHandleByIndex_v2(unsigned int index, void** device)
{
  *device = reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1));
  return 0;
}

int
nvmlDeviceGetUUID(void* device, char* output, unsigned int size)
{
  const auto index = reinterpret_cast<std::uintptr_t>(device) - 1;
  const char* uuids[] = {
    "GPU-00000000-0000-0000-0000-000000000000",
    "GPU-11111111-1111-1111-1111-111111111111",
    "GPU-22222222-2222-2222-2222-222222222222",
    "GPU-33333333-3333-3333-3333-333333333333"};
  const auto* uuid = index == 3 && scenario("missing-inventory") ?
    "GPU-55555555-5555-5555-5555-555555555555" : uuids[index];
  if (size <= std::strlen(uuid)) {
    return 1;
  }
  std::memcpy(output, uuid, std::strlen(uuid) + 1);
  return 0;
}

int
nvmlDeviceGetHandleByUUID(const char* uuid, void** device)
{
  if (scenario("memory-device-missing")) return 1;
  for (unsigned int index = 0; index < 4; ++index) {
    void* candidate = nullptr;
    char identity[96]{};
    nvmlDeviceGetHandleByIndex_v2(index, &candidate);
    nvmlDeviceGetUUID(candidate, identity, sizeof(identity));
    if (std::strcmp(uuid, identity) == 0) {
      *device = candidate;
      return 0;
    }
  }
  return 1;
}

int
nvmlDeviceGetMemoryInfo(void* device, void* result)
{
  if (scenario("memory-error")) return 1;
  struct Memory { unsigned long long total; unsigned long long free; unsigned long long used; };
  auto* memory = static_cast<Memory*>(result);
  const auto index = reinterpret_cast<std::uintptr_t>(device) - 1;
  constexpr unsigned long long MIB = 1024 * 1024;
  memory->total = 24576 * MIB;
  memory->free = (index == 3 ? 18000 : 1000) * MIB;
  memory->used = memory->total - memory->free;
  if (scenario("memory-zero-total")) memory->total = 0;
  if (scenario("memory-overflow")) memory->free = memory->total + 1;
  if (scenario("memory-bad-used")) memory->used = memory->total + 1;
  return 0;
}

} // extern "C"
