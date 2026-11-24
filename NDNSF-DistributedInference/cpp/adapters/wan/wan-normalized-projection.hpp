#ifndef NDNSF_DI_WAN_NORMALIZED_PROJECTION_HPP
#define NDNSF_DI_WAN_NORMALIZED_PROJECTION_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

struct NormalizedProjectionRank
{
  OnnxEntryCut project;
  OnnxEntryCut normalize;
};

struct NormalizedProjectionLayout
{
  onnx::ModelProto model;
  std::array<NormalizedProjectionRank, 2> ranks;
};

/** Lower an isolated FP32 Wan Q or K projection followed by full-width RMSNorm.
 *
 * The source is an opset-18 or later graph with MatMul, bias Add, square Mul,
 * last-axis ReduceMean (keepdims), epsilon Add, Sqrt, Div and gain Mul.
 * headWidth is established by the enclosing attention graph. No head may be
 * split across ranks. The model preserves the original ports. Each project
 * entry outputs the rank's projection and FP32 square sum. Its normalize
 * entry consumes that projection and the global square sum from native SUM.
 */
NormalizedProjectionLayout
lowerNormalizedProjection(const onnx::ModelProto& source, std::uint64_t headWidth,
                           const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_NORMALIZED_PROJECTION_HPP
