#ifndef NDNSF_DI_AUTOMATIC_ONNX_IO_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_IO_HPP

#include "OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr std::size_t NATIVE_AUTOMATIC_ONNX_IO_MAX_BYTES = 4U << 20;

/** Bounded preparation metadata. Contains no request state or model payload. */
struct AutomaticOnnxIoContract
{
  AutomaticOnnxIoContract();

  StatefulOnnxIoContractV1 io;
  std::vector<std::string> unrankedInputNames;
  std::vector<std::string> passthroughTensors;

  void validate() const;
  std::string canonicalJson() const;
  static AutomaticOnnxIoContract fromCanonicalJson(const std::string& encoded);
};

/** Zip global state pairs before selecting the complete local pairs. */
AutomaticOnnxIoContract
automaticOnnxIoContractForProjection(const NativeSelectionProjectionV3& projection);

/** Replace legacy name encodings only for an automatic projection. */
void
bindAutomaticOnnxRunnerIo(NativeModelRunnerSpec& spec,
                          const NativeSelectionProjectionV3& projection);

/** Missing means legacy; malformed or mixed automatic metadata always throws. */
std::optional<AutomaticOnnxIoContract>
readAutomaticOnnxRunnerIo(const NativeModelRunnerSpec& spec);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_IO_HPP
