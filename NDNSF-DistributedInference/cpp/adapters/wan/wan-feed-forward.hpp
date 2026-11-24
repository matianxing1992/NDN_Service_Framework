#ifndef NDNSF_DI_WAN_FEED_FORWARD_HPP
#define NDNSF_DI_WAN_FEED_FORWARD_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

/** Lower an isolated Wan FFN: MatMul, bias, tanh Gelu, MatMul, bias.
 * The source has the canonical ONNX opset-20 Gelu form and FP32 parameters.
 * The model retains its source ports. TP uses the independent partial and bias cuts.
 */
SummedProjection
lowerFeedForward(const onnx::ModelProto& source, const WanInitializerAccess& material = {});

/** Pad a trimmed [1, tokens, textWidth] context before tensor-parallel text embedding.
 * The source may declare the middle axis as valid_tokens for variable prompt lengths.
 */
SummedProjection
lowerTextEmbedding(const onnx::ModelProto& source, std::int64_t textLength,
                   const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_FEED_FORWARD_HPP
