#ifndef NDNSF_DI_AUTOMATIC_ONNX_CATALOG_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_CATALOG_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"

#include <cstdint>
#include <string>

namespace ndnsf::di {

/** Caller-owned catalog identities that are not derived from the source bytes.
 * They must match the maintained recipe/assembler/publication configuration. */
struct AutomaticOnnxCatalogSpec
{
  std::string sourceFile = "model.onnx";
  /** Physical size of the pinned graph file. The inspection consumes the
   * source bytes, so the caller records the size before inspection. */
  std::uint64_t sourceBytes = 0;
  /** Optional external-initializer file (for example model.onnx_data) and its
   * pinned object digest. Empty means the source is self-contained. */
  std::string initializerFile;
  std::string initializerDigest;
  std::string dataName;
  std::string modelManifestDigest;
  std::string artifactProfileDigest;
  std::string assemblerDescriptorDigest;
  std::string backendAbi = "onnxruntime";
  std::string layout = "native";
  std::string protectionEpoch = "plaintext-v1";
  std::uint64_t maxSourceBytes = 1ULL << 30;
  std::uint64_t maxAssembledBytes = 1ULL << 30;
  std::uint64_t maxNodes = 4096;
};

/** Build the maintained native request catalog entry for one inspected
 * automatic source. The result is the same schema a user-declared catalog
 * uses; nothing here claims an assembled package or a finished preparation. */
NativeJson
automaticOnnxCatalogJson(const AutomaticOnnxModel& model,
                         const AutomaticOnnxCatalogSpec& spec,
                         const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_CATALOG_HPP
