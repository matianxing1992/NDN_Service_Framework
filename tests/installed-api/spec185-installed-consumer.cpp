#include "NDNSF-DistributedInference/cpp/ndnsf-di/api.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/extensions.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"

#include <stdexcept>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <string>
#include <type_traits>

namespace {

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
std::filesystem::path
writeTinyOnnxFixture()
{
  // ModelProto: opset 13, one FLOAT[1,1] input, Identity, one output.
  static constexpr char kHex[] =
    "080a1207737065633138353a4c0a1a0a01581201591a086964656e7469747922084964656e74697479"
    "120474696e795a130a0158120e0a0c080112080a0208010a02080162130a0159120e0a0c080112080a0208010a02080142040a00100d";
  const auto path = std::filesystem::path("/tmp") /
    ("spec185-installed-consumer-" + std::to_string(static_cast<long long>(::getpid())) + ".onnx");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create tiny ONNX fixture");
  for (std::size_t i = 0; kHex[i] != '\0'; i += 2) {
    const auto nibble = [] (char c) -> unsigned char {
      if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
      throw std::runtime_error("invalid tiny ONNX fixture encoding");
    };
    const auto byte = static_cast<char>((nibble(kHex[i]) << 4) | nibble(kHex[i + 1]));
    output.put(byte);
  }
  output.close();
  return path;
}
#endif

} // namespace

int
main()
{
  static_assert(ndnsf::di::kPreparedModelRuntimeApiVersion == 2);
  static_assert(ndnsf::di::kPreparedModelRuntimeProviderApiVersion == 1);
  static_assert(ndnsf::di::kPreparedModelRuntimeExtensionsApiVersion == 1);
  static_assert(std::is_destructible_v<ndnsf::di::OnnxRuntimeModelRunner>);

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  ndnsf::di::NativeModelRunnerSpec spec;
  spec.backend = "onnxruntime";
  spec.path = "/does/not/exist.onnx";
  try {
    ndnsf::di::RegistryNativeModelRunnerFactory factory;
    ndnsf::di::registerOnnxRuntimeBackend(factory);
    (void)factory.create(spec);
  }
  catch (const std::runtime_error& error) {
    return std::string(error.what()).find("not enabled") != std::string::npos ? 0 : 3;
  }
  return 2;
#else
  // Exercise the enabled factory/constructor/destructor ABI with a real tiny
  // ONNX graph.  The later native qualification batch owns full request
  // behavior; this B0 probe is limited to installed object lifetime.
  const auto fixture = writeTinyOnnxFixture();
  ndnsf::di::NativeModelRunnerSpec spec;
  spec.backend = "onnxruntime";
  spec.path = fixture.string();
  {
    ndnsf::di::RegistryNativeModelRunnerFactory factory;
    ndnsf::di::registerOnnxRuntimeBackend(factory);
    auto runner = factory.create(spec);
    if (!runner) return 2;
  }
  std::error_code error;
  std::filesystem::remove(fixture, error);
  return error ? 4 : 0;
#endif
}
