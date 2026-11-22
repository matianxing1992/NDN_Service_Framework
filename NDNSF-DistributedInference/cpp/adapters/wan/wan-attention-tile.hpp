#ifndef NDNSF_DI_WAN_ATTENTION_TILE_HPP
#define NDNSF_DI_WAN_ATTENTION_TILE_HPP

#include <onnx/onnx_pb.h>

#include <cstdint>

namespace ndnsf::di::wan {

struct AttentionTileShape
{
  std::int64_t batches;
  std::int64_t heads;
  std::int64_t queries;
  std::int64_t keys;
  std::int64_t headWidth;
  bool keyOnlyMask = false;
};

/** Create one portable FP32 softmax update over a bounded key/value tile.
 *
 * Q/K/V use [batch, heads, tokens, headWidth]. The Boolean mask is
 * [batch, 1, queries, keys], or [batch, 1, 1, keys] for keyOnlyMask.
 * State is max and denominator shaped
 * [batch, heads, queries, 1], plus unnormalized weighted values shaped like Q.
 * Start with the lowest finite FP32 max and zero denominator/weighted values.
 * A completely masked tile leaves the normalization state unchanged.
 * The caller divides the final weighted values by the nonzero denominator
 * for admitted query rows. scoreBytes bounds one dense score plane.
 */
onnx::ModelProto
makeAttentionTileUpdate(const AttentionTileShape& shape, std::uint64_t scoreBytes);

/** Attend one query tile over padded context using a bounded ONNX Loop body.
 * contextTokens must be a multiple of shape.keys. The caller's mask excludes
 * padding. A query with no valid keys produces zeros.
 */
onnx::ModelProto
makeAttentionQueryBlock(const AttentionTileShape& shape, std::int64_t contextTokens,
                        std::uint64_t scoreBytes);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_ATTENTION_TILE_HPP
