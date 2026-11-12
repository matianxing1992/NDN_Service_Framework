#ifndef NDNSF_DI_PREPARED_ONNX_METADATA_HPP
#define NDNSF_DI_PREPARED_ONNX_METADATA_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace ndnsf::di {

struct AutomaticOnnxContract;
struct AutomaticOnnxModel;
struct NativeInspectedModel;

/** Decoded preparation facts; neither signature trust nor execution permission. */
struct NativePreparedOnnxMetadata
{
  NativeOnnxGraphInspection graph;
  std::shared_ptr<const AutomaticOnnxContract> automaticContract;
};

/** Decode bounded source facts against their original physical inventory.
 * This checks declarations, not object trust, authorization or backend readiness.
 */
std::shared_ptr<const AutomaticOnnxContract>
decodeNativeAutomaticOnnxSourceMetadata(const NativeJson& automatic,
                                      const std::string& modelDigest,
                                      std::uint64_t initializerBytes,
                                      const std::string& initializerObjectDigest,
                                      const NativeAssemblyControl& control);

/** Preserve v1 bytes when automaticModel is null; otherwise encode strict v2. */
std::string
encodeNativePreparedOnnxMetadata(const NativeInspectedModel& model,
                                const NativeOnnxGraphInspection& graph,
                                const AutomaticOnnxModel* automaticModel,
                                const NativeAssemblyControl& control);

/** Decode graph facts and, for v2, the inventory-bound immutable contract. */
NativePreparedOnnxMetadata
decodeNativePreparedOnnxMetadata(const NativeJson& metadata,
                                const NativeAssemblyControl& control);

/** Preserve historical v1 payloads; fully validate automatic v2 before reuse. */
void
validateNativePreparedMetadataVersion(const NativeJson& metadata,
                                     const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_PREPARED_ONNX_METADATA_HPP
