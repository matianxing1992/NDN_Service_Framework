#ifndef NDNSF_DI_WAN_HEAD_PROJECTION_HPP
#define NDNSF_DI_WAN_HEAD_PROJECTION_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

enum class ProjectionParallelism { OUTPUT_CHANNELS, INPUT_CHANNELS };

struct HeadProjectionLayout
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> ranks;
  OnnxEntryCut finish;
};

/** Partition a canonical FP32 MatMul + Add along complete head ranges.
 * Output-channel parallelism is used for V and input-channel parallelism
 * for O. Each V entry returns local columns with bias. Each O entry accepts
 * its local input columns and returns a full-width unbiased partial. The
 * finish entry concatenates V columns or adds O bias after native SUM.
 */
HeadProjectionLayout
lowerHeadProjection(const onnx::ModelProto& source, std::uint64_t headWidth,
                     ProjectionParallelism parallelism,
                     const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_HEAD_PROJECTION_HPP
