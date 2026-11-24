#ifndef NDNSF_DI_WAN_VIDEO_HEAD_HPP
#define NDNSF_DI_WAN_VIDEO_HEAD_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

struct VideoHeadGrid
{
  std::int64_t frames;
  std::int64_t height;
  std::int64_t width;
  std::int64_t paddedTokens;
};

/** Lower the T2V prediction head for a rectangular patch grid and right padding.
 * Modulation is [1, 2, hidden], weight is [hidden, 4 * outputChannels].
 * Partials read hidden [1, paddedTokens, hidden] and embedding [1, hidden].
 * The result entry adds bias, drops padding, and returns NCDHW with patch (1, 2, 2).
 */
SummedProjection
lowerVideoHead(const VideoHeadGrid& grid, const onnx::TensorProto& modulation,
               const onnx::TensorProto& weight, const onnx::TensorProto& bias, float epsilon,
               const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_VIDEO_HEAD_HPP
