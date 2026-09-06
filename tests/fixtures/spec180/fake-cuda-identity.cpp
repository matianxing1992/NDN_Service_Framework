// Test double only. Never included in the production source archive.
#include <cstdlib>
#include <cstring>
static bool fails(const char* operation)
{
  const char* requested = std::getenv("TEST_CUDA_FAILURE");
  return requested && std::strcmp(operation, requested) == 0;
}
extern "C" int cudaDeviceGetPCIBusId(char* buffer, int size, int ordinal)
{
  if (fails("pci") || ordinal != 0 || size < 13) return 1;
  std::strcpy(buffer, "0000:18:00.0");
  return 0;
}
extern "C" int cuInit(unsigned int) { return fails("init") ? 1 : 0; }
extern "C" int cuDeviceGetByPCIBusId(int* device, const char* pci)
{
  if (fails("lookup") || std::strcmp(pci, "0000:18:00.0") != 0) return 1;
  *device = 42; // Deliberately not the CUDA runtime ordinal.
  return 0;
}
extern "C" int cuDeviceGetUuid(void* uuid, int device)
{
  if (fails("uuid") || device != 42) return 1;
  for (int i = 0; i < 16; ++i) static_cast<unsigned char*>(uuid)[i] = fails("zero") ? 0 : i;
  return 0;
}
