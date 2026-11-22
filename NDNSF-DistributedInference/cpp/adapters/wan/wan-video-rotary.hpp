#ifndef NDNSF_DI_WAN_VIDEO_ROTARY_HPP
#define NDNSF_DI_WAN_VIDEO_ROTARY_HPP

#include <onnx/onnx_pb.h>

#include <cstdint>

namespace ndnsf::di::wan {

struct VideoRotaryTile
{
  std::int64_t frames;
  std::int64_t height;
  std::int64_t width;
  std::int64_t heads;
  std::int64_t headWidth;
  std::int64_t firstToken;
  std::int64_t tokens;
};

/** Compile Wan2.1 3-D RoPE for one [1, heads, tokens, headWidth] tile.
 *
 * Grid positions are absolute in time-major, then height-major order. A tile
 * may extend into right padding, whose rotary factors are the identity.
 * The bound covers both FP64 trigonometric tables before graph construction.
 */
onnx::ModelProto
makeVideoRotaryEmbedding(const VideoRotaryTile& tile, std::uint64_t tableBytes);

/** Compile the requested Q tile into self-attention over a rank-local context.
 * The context-key input already carries the same grid's RoPE, so a Provider
 * can retain and reuse it for multiple query tiles. The mask excludes padding
 * keys and broadcasts one row across the queries.
 */
onnx::ModelProto
makeVideoAttentionQueryBlock(const VideoRotaryTile& query, std::int64_t contextTokens,
                             std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                             std::uint64_t rotaryBytes);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_VIDEO_ROTARY_HPP
