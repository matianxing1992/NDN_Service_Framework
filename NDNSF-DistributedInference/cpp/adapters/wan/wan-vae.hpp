#ifndef NDNSF_DI_WAN_VAE_HPP
#define NDNSF_DI_WAN_VAE_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

#include <memory>
#include <set>

namespace ndnsf::di {
class NativeCanonicalByteRangeSource;
}

namespace ndnsf::di::wan {

class WanCheckpoint;

struct VaeConvTile
{
  std::int64_t frames;
  std::int64_t height;
  std::int64_t width;
};

/** Compile a Wan decoder Conv3D with zero future padding and rank-local history.
 * Each partial consumes the current NCDHW tile and, for a temporal kernel
 * wider than one, the previous local history (zeros on the first call).
 * It returns an unbiased partial and the next local history. The bias entry
 * consumes the native SUM. History is an activation, never model material.
 */
SummedProjection
lowerVaeCausalConv(const VaeConvTile& tile, const onnx::TensorProto& weight,
                   const onnx::TensorProto& bias,
                   const WanInitializerAccess& material = {});

/** Compile nearest-exact 2x spatial upsampling after the optional time Conv3D.
 * The first chunk skips time Conv3D. Later temporal chunks provide two channel
 * groups; their frames are interleaved in source order before spatial resize.
 */
onnx::ModelProto
makeVaeUpsample(const VaeConvTile& tile, std::int64_t channels, bool interleaveTime);

struct VaeRmsLayout
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> statistics;
  std::array<OnnxEntryCut, 2> normalize;
  OnnxEntryCut join;
};

/** Lower the decoder's channel-first F.normalize followed by SiLU.
 * Gain is a canonical FP32 [channels] initializer.
 */
VaeRmsLayout
lowerVaeRmsSilu(const VaeConvTile& tile, const onnx::TensorProto& gain,
                const WanInitializerAccess& material = {});

struct VaeStageLayout
{
  onnx::ModelProto model;
  std::map<std::string, OnnxEntryCut> entries;
  std::map<std::string, std::string> historySuccessorByInput;
  std::set<std::string> rankOneEntries;
  std::map<std::uint64_t, OnnxReductionCut> reductions;
  std::set<std::string> parameterNames;
  std::optional<std::size_t> decoderMiddleEnd;
  std::vector<std::size_t> pipelineStageByNode;
};

/** Compile one latent phase into the existing two-rank CALL/SUM template.
 * A history port is an epoch input here. The component planner binds its
 * predecessor rank's output when composing a full latent sequence.
 */
OnnxParallelPlan
makeVaeTensorParallelPlan(const VaeStageLayout& layout, const NativeStageLayout& stage);

struct VaeConvolutionParameters
{
  const onnx::TensorProto& weight;
  const onnx::TensorProto& bias;
};

/** Compile the selected decoder resample phase. For time upsampling, the first
 * frame uses only the spatial projection. On the second invocation the caller
 * supplies zero time history, then reuses this stage's history successors.
 */
VaeStageLayout
lowerVaeResample(const VaeConvTile& tile, const VaeConvolutionParameters& spatial,
                 std::optional<VaeConvolutionParameters> temporal, bool firstChunk,
                 const WanInitializerAccess& material = {});

/** Add the Wan2.2 channel-repeat and spatiotemporal upsample shortcut. */
VaeStageLayout
connectVaeUpsampleResidual(VaeStageLayout main, const VaeConvTile& tile,
                          std::int64_t inputChannels, std::int64_t outputChannels,
                          bool temporal, bool firstChunk);

/** Compose two decoder RMS/SiLU/Conv groups and the residual shortcut.
 * Channel-changing blocks require a stateless 1x1x1 shortcut. History stays
 * separate for each physical convolution rank and is explicit at the boundary.
 */
VaeStageLayout
connectVaeResidual(VaeRmsLayout firstNorm, SummedProjection firstConv,
                   VaeRmsLayout secondNorm, SummedProjection secondConv,
                   std::optional<SummedProjection> shortcut = {});

struct VaeAttentionParameters
{
  const onnx::TensorProto& gain;
  const onnx::TensorProto& qkvWeight;
  const onnx::TensorProto& qkvBias;
  const onnx::TensorProto& outputWeight;
  const onnx::TensorProto& outputBias;
};

/** Compile per-frame VAE spatial attention with rank-owned pointwise convolutions.
 * Both Conv weights use canonical [out, in, 1, 1, 1] layout.
 */
VaeStageLayout
lowerVaeAttention(const VaeConvTile& tile, const VaeAttentionParameters& parameters,
                  std::int64_t keyTile, std::uint64_t scoreBytes,
                  const WanInitializerAccess& material = {});

struct VaeDecoderParameters
{
  std::vector<float> latentMean;
  std::vector<float> latentInverseStd;
  VaeConvolutionParameters latentProjection;
  VaeConvolutionParameters inputProjection;
  const onnx::TensorProto& outputGain;
  VaeConvolutionParameters rgbProjection;
  std::int64_t outputPatchSize = 1;
};

/** Connect one latent-frame decode with source-ordered middle and upsample stages.
 * Use the same stage IDs in both phases to preserve ordinary history identities.
 * The result is normalized, clipped FP32 RGB in the source NCDHW layout.
 */
VaeStageLayout
connectVaeDecoder(const VaeConvTile& latentTile, const VaeDecoderParameters& parameters,
                  std::vector<std::pair<std::string, VaeStageLayout>> stages,
                  const WanInitializerAccess& material = {});

/** Bind the effective Wan2.1 decoder topology to named FP32 checkpoint tensors.
 * The caller supplies converted tensor storage and the pinned latent constants.
 * Encoder-only tensors may be present. All decoder tensors must be accounted for.
 */
VaeStageLayout
lowerWan21VaeDecoder(const VaeConvTile& latentTile,
                     const std::map<std::string, onnx::TensorProto>& parameters,
                     std::vector<float> latentMean, std::vector<float> latentInverseStd,
                     bool firstChunk, std::int64_t keyTile, std::uint64_t scoreBytes,
                     const WanInitializerAccess& material = {});

/** Bind the Wan2.2 grouped decoder, upsample shortcuts and packed RGB head. */
VaeStageLayout
lowerWan22VaeDecoder(const VaeConvTile& latentTile,
                    const std::map<std::string, onnx::TensorProto>& parameters,
                    std::vector<float> latentMean, std::vector<float> latentInverseStd,
                    bool firstChunk, std::int64_t keyTile, std::uint64_t scoreBytes,
                    const WanInitializerAccess& material = {});

/** Combine one initial phase and repeated later phases, sharing matching initializers.
 * The clip has a single latent input and an ordered NCDHW RGB output. Causal
 * history remains local to its rank and is initialized by ordinary graph nodes.
 */
VaeStageLayout
connectVaeClip(const VaeStageLayout& first, const VaeStageLayout& later,
              std::int64_t latentFrames, std::size_t maxGraphNodes);

struct CheckpointVaeClip
{
  VaeStageLayout layout;
  std::shared_ptr<const NativeCanonicalByteRangeSource> sidecar;
};

/** Bind both causal phases to the same immutable checkpoint slice coordinates. */
CheckpointVaeClip
lowerWan21VaeClip(const WanCheckpoint& checkpoint, const VaeConvTile& tile,
                  const std::vector<float>& latentMean, const std::vector<float>& latentInverseStd,
                  std::int64_t latentFrames, std::int64_t keyTile, std::uint64_t scoreBytes,
                  std::size_t maxGraphNodes);

/** Bind Wan2.2 phases to the shared checkpoint slice owner. */
CheckpointVaeClip
lowerWan22VaeClip(const WanCheckpoint& checkpoint, const VaeConvTile& tile,
                  const std::vector<float>& latentMean, const std::vector<float>& latentInverseStd,
                  std::int64_t latentFrames, std::int64_t keyTile, std::uint64_t scoreBytes,
                  std::size_t maxGraphNodes);

/** Cut a Wan2.1 clip after the middle decoder group in every latent chunk. */
OnnxParallelPlan
makeVaePipelinePlan(const VaeStageLayout& layout, const std::array<NativeStageLayout, 2>& stages);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_VAE_HPP
