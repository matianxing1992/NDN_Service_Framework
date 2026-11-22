#ifndef NDNSF_DI_WAN_ATTENTION_CONTEXT_HPP
#define NDNSF_DI_WAN_ATTENTION_CONTEXT_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-summed-projection.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-video-rotary.hpp"

namespace ndnsf::di::wan {

struct AttentionContextRank
{
  OnnxEntryCut project;
  OnnxEntryCut prepare;
};

struct AttentionContextLayout
{
  onnx::ModelProto model;
  std::array<AttentionContextRank, 2> ranks;
};

struct AttentionQueryRank
{
  OnnxEntryCut project;
  OnnxEntryCut attend;
};

struct AttentionQueryLayout
{
  onnx::ModelProto model;
  std::array<AttentionQueryRank, 2> ranks;
  OnnxEntryCut finish;
};

/** Compile video self-attention K/V projections from their canonical fragments.
 *
 * Both projections consume the same [1, tokens, hidden] input. The graph
 * exposes key-rank-0, value-rank-0, key-rank-1, value-rank-1 in attention layout.
 * Keys include full-width RMS normalization and video RoPE. Only statistics
 * cross the key rank boundary. The rotary budget covers both rank fragments.
 */
AttentionContextLayout
lowerVideoAttentionContext(const onnx::ModelProto& key, const onnx::ModelProto& value,
                           const VideoRotaryTile& context, std::uint64_t rotaryBytes,
                           const WanInitializerAccess& material = {});

/** Compile Q projection, local self-attention and row-parallel O projection.
 * Connect the four matching K/V outputs of lowerVideoAttentionContext and
 * provide a shared Boolean key mask [1, 1, 1, contextTokens].
 */
AttentionQueryLayout
lowerVideoAttentionQuery(const onnx::ModelProto& query, const onnx::ModelProto& output,
                         const VideoRotaryTile& tile, std::int64_t contextTokens,
                         std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                         std::uint64_t rotaryBytes, const WanInitializerAccess& material = {});

/** Prepare K/V for every projected text slot, including Wan's projected padding. */
AttentionContextLayout
lowerTextAttentionContext(const onnx::ModelProto& key, const onnx::ModelProto& value,
                          std::int64_t heads, std::int64_t headWidth,
                          const WanInitializerAccess& material = {});

/** Attend the prepared text context without video RoPE or an UMT5 padding mask. */
AttentionQueryLayout
lowerTextAttentionQuery(const onnx::ModelProto& query, const onnx::ModelProto& output,
                        std::int64_t heads, std::int64_t headWidth, std::int64_t contextTokens,
                        std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                        const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_ATTENTION_CONTEXT_HPP
