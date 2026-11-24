#ifndef NDNSF_DI_WAN_PATCH_EMBEDDING_HPP
#define NDNSF_DI_WAN_PATCH_EMBEDDING_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

class WanCheckpoint;

/** Lower an isolated Wan2.1 patch Conv3D to a canonical graph and its physical TP cuts.
 * A full stage executes the graph. TP materializes the two partials independently,
 * sums their outputs using the ordinary local SUM step, and runs addBias once.
 */
SummedProjection
lowerPatchEmbedding(const onnx::ModelProto& source, const WanInitializerAccess& material = {});

/** Bind Wan2.1 checkpoint patch weights to the existing Conv3D lowering. */
SummedProjection
lowerWan21PatchEmbedding(const WanCheckpoint& checkpoint,
                        const std::array<std::int64_t, 4>& latentShape,
                        const NativeOnnxInitializerSlicer& slicer = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_PATCH_EMBEDDING_HPP
