#ifndef NDNSF_DI_WAN_VIDEO_BLOCK_HPP
#define NDNSF_DI_WAN_VIDEO_BLOCK_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-attention-context.hpp"
#include "wan-summed-projection.hpp"
#include "wan-time-embedding.hpp"
#include "wan-video-head.hpp"

namespace ndnsf::di {
class NativeCanonicalByteRangeSource;
}

namespace ndnsf::di::wan {

class WanCheckpoint;
struct VideoBlockStage
{
  std::array<OnnxEntryCut, 2> ranks;
  OnnxEntryCut join;
};

struct VideoBlockLayout
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> modulation;
  VideoBlockStage selfInput;
  VideoBlockStage selfResidual;
  VideoBlockStage crossInput;
  OnnxEntryCut crossResidual;
  VideoBlockStage ffnInput;
  VideoBlockStage ffnResidual;
};

struct VideoBlockAttention
{
  AttentionContextLayout context;
  AttentionQueryLayout query;
};

struct ConnectedVideoBlock
{
  onnx::ModelProto model;
  NativeExecutionPlan tensorParallel;
  std::map<std::string, OnnxEntryCut> entries;
};

/** Compile one Wan2.1 T2V block's conditioning, normalization and residuals.
 * The graph exposes inputs and outputs for the independently compiled attention
 * and FFN branches. Learned modulation [1, 6, hidden] and the cross-attention
 * affine parameters [hidden] are stored in disjoint rank ranges.
 */
VideoBlockLayout
lowerVideoBlock(std::int64_t tokens, const onnx::TensorProto& modulation,
                const onnx::TensorProto& crossScale, const onnx::TensorProto& crossBias,
                float epsilon, const WanInitializerAccess& material = {});

/** Compile one block's PP graph and its two-rank native execution template.
 * The compiled attention fragments supply internal rank K/V ports. Cross context
 * remains an external text input, and the self-attention Boolean mask is preserved.
 */
ConnectedVideoBlock
connectVideoBlock(VideoBlockLayout conditioning, VideoBlockAttention self,
                  VideoBlockAttention cross, SummedProjection ffn,
                  const NativeStageLayout& stage);

struct VideoDenoiserLayout
{
  onnx::ModelProto model;
  OnnxParallelPlan tensorParallel;
  std::vector<std::size_t> blockEnds;
};

struct WanDenoiserGeometry
{
  VideoHeadGrid grid;
  std::int64_t heads;
  std::int64_t textTokens;
  std::int64_t videoKeyTile;
  std::int64_t textKeyTile;
  std::uint64_t scoreBytes;
  std::uint64_t rotaryBytes;
};

struct CheckpointDenoiser
{
  VideoDenoiserLayout layout;
  std::shared_ptr<const NativeCanonicalByteRangeSource> initializerRangeSource;
};

/** Bind Wan2.1 DiT state dict parameters to a portable graph and one bounded sidecar. */
CheckpointDenoiser
lowerWan21Denoiser(const WanCheckpoint& checkpoint, const WanDenoiserGeometry& geometry,
                   const NativeStageLayout& stage);

/** Connect all denoiser stages for one fixed rectangular patch grid. */
VideoDenoiserLayout
connectVideoDenoiser(SummedProjection patch, SummedProjection text, TimeEmbeddingLayout time,
                     std::vector<ConnectedVideoBlock> blocks, SummedProjection head,
                     const VideoHeadGrid& grid, const NativeStageLayout& stage);

/** Cut between complete denoiser blocks, retaining the first `prefixBlocks`. */
OnnxParallelPlan
makeVideoDenoiserStagedPlan(const VideoDenoiserLayout& layout, std::size_t prefixBlocks,
                           const std::array<NativeStageLayout, 2>& stages);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_VIDEO_BLOCK_HPP
