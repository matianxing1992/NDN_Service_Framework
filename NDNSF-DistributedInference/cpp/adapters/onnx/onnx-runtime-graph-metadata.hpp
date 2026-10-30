#ifndef NDNSF_DI_ONNX_RUNTIME_GRAPH_METADATA_HPP
#define NDNSF_DI_ONNX_RUNTIME_GRAPH_METADATA_HPP

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace ndnsf::di::detail {

/** Adapter-owned facts derived from the actual ONNX graph, never caller labels.
 * An empty host inventory preserves the original strict CUDA admission path.
 */
struct OnnxRuntimeGraphMetadata
{
  std::map<std::string, std::string> hostMetadataNodes;
  std::map<std::string, std::string> modelNodes;
  std::map<std::string, std::string> optionalModelNodes;
  std::map<std::string, std::string> modelNodeAliases;
  std::array<std::uint64_t, 7> sourceFileStamp{};
};

/** Inspect a flat, uniquely named graph without materializing weight payloads.
 * Unsupported projections return an empty inventory and cannot relax admission.
 */
OnnxRuntimeGraphMetadata
inspectOnnxRuntimeGraphMetadata(const std::string& modelPath);

/** Reject replacement between graph inspection and ORT session creation. */
void
verifyOnnxRuntimeGraphMetadataSource(const std::string& modelPath,
                                    const OnnxRuntimeGraphMetadata& metadata);

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_ONNX_RUNTIME_GRAPH_METADATA_HPP
