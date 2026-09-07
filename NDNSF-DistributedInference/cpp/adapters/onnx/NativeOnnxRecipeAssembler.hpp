#ifndef NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP
#define NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Authenticated canonical ONNX bytes fetched after Selection. */
struct NativeCanonicalSource
{
  std::vector<std::uint8_t> modelBytes;
  std::optional<std::vector<std::uint8_t>> initializerBytes;
};

using NativeCertifiedRecipe = NativeSelectionRoleV3;

/** Request-owned limits and cancellation callback for native assembly. */
struct NativeAssemblyControl
{
  std::chrono::steady_clock::time_point deadline;
  std::function<void()> requireActive;
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;
};

struct NativeCertifiedAssembly
{
  std::vector<std::uint8_t> modelBytes;
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  std::uint64_t nodeCount = 0;
  std::string modelDigest;
};

/**
 * Assemble one authenticated ONNX role without an interpreter subprocess.
 * The function owns no cache or network state; callers retain those
 * responsibilities and must re-check authorization before activation.
 */
NativeCertifiedAssembly
assembleNativeCertifiedOnnxModel(const NativeCanonicalSource& source,
                                 const NativeCertifiedRecipe& recipe,
                                 const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP
