#ifndef NDNSF_TESTS_FIXTURES_NATIVE_WORKER_LOCATION_HPP
#define NDNSF_TESTS_FIXTURES_NATIVE_WORKER_LOCATION_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <unistd.h>

namespace ndnsf::test {

/** Use the production resolver; retain only the explicit legacy bin-directory override. */
inline di::NativeOnnxWorkerLocation
testNativeWorkerLocation()
{
  const auto* worker = std::getenv("NDNSF_DI_WORKER_BINARY");
  const auto* directory = std::getenv("NDNSF_SPEC182_BIN_DIR");
  if ((!worker || !*worker) && directory) {
    const std::filesystem::path root(directory);
    if (root.empty() || !root.is_absolute()) {
      throw std::runtime_error("NDNSF_SPEC182_BIN_DIR must be an absolute binary directory");
    }
    for (const char* basename : {"di-native-assembly-worker", "DI_NativeOnnxAssemblyWorker"}) {
      const auto path = root / basename;
      if (std::filesystem::is_regular_file(path) && ::access(path.c_str(), X_OK) == 0) {
        // The production registration/spawn path captures and rechecks the binary digest.
        return {path.string(), ""};
      }
    }
    throw std::runtime_error("assembly worker missing from explicit NDNSF_SPEC182_BIN_DIR");
  }
  const auto location = di::resolveNativeOnnxWorkerLocation();
  if (location.path.empty()) {
    throw std::runtime_error("installed assembly worker missing; configure NDNSF_DI_WORKER_BINARY");
  }
  return location;
}

} // namespace ndnsf::test

#endif
