#ifndef NDNSF_DI_WAN_UMT5_HPP
#define NDNSF_DI_WAN_UMT5_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

#include <set>

namespace ndnsf::di::wan {

class WanCheckpoint;

struct Umt5EmbeddingLayout
{
  NativeCanonicalSource source;
  std::array<OnnxEntryCut, 2> ranks;
  OnnxEntryCut join;
};

/** Compile a token embedding with one column slice per rank.
 * The input table is little-endian FP32 row-major [vocab, hidden] at tableOffset
 * within the caller's immutable material source. The derived sidecar reads it
 * in bounded windows, with no full initializer copied into a protobuf.
 */
Umt5EmbeddingLayout
lowerUmt5Embedding(std::int64_t tokens, std::int64_t vocab, std::int64_t hidden,
                   std::shared_ptr<const NativeCanonicalByteRangeSource> table,
                   std::uint64_t tableOffset = 0);

struct Umt5ReductionLayout
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> partials;
  // The physical result is the runtime SUM. The full graph retains an ONNX Add.
  NativeAssemblyTensorContractV3 output;
};

/** Compile UMT5's bias-free fc2(fc1(x) * GELU(tanh)(gate(x))).
 * The inputs are canonical MatMul matrices, with the same intermediate channel
 * range assigned to each rank across all three projections.
 */
Umt5ReductionLayout
lowerUmt5FeedForward(std::int64_t tokens, const onnx::TensorProto& gate,
                     const onnx::TensorProto& expansion, const onnx::TensorProto& contraction,
                     const NativeOnnxExternalReader& external = {});

struct Umt5AttentionParameters
{
  const onnx::TensorProto& query;
  const onnx::TensorProto& key;
  const onnx::TensorProto& value;
  const onnx::TensorProto& output;
  const onnx::TensorProto& relativeBias;
};

/** Compile masked UMT5-XXL attention over up to 512 token slots.
 * Q/K/V and the relative bias are partitioned by complete heads. The O
 * projection emits unbiased partials for native SUM. scoreBytes bounds one
 * rank's score plane. The Boolean key mask marks valid tokenizer positions.
 */
Umt5ReductionLayout
lowerUmt5Attention(std::int64_t tokens, const Umt5AttentionParameters& parameters,
                   std::uint64_t scoreBytes, const NativeOnnxExternalReader& external = {});

struct Umt5RmsLayout
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> statistics;
  std::array<OnnxEntryCut, 2> normalize;
  OnnxEntryCut join;
};

/** Lower T5LayerNorm with one learned gain slice per rank and no learned bias. */
Umt5RmsLayout
lowerUmt5Rms(std::int64_t tokens, const onnx::TensorProto& gain, float epsilon,
             const NativeOnnxExternalReader& external = {});

struct Umt5EncoderLayer
{
  Umt5RmsLayout preAttention;
  Umt5ReductionLayout attention;
  Umt5RmsLayout preFfn;
  Umt5ReductionLayout ffn;
};

struct Umt5EncoderLayout
{
  NativeCanonicalSource source;
  std::map<std::string, OnnxEntryCut> entries;
  std::set<std::string> rankOneEntries;
  std::map<std::uint64_t, OnnxReductionCut> reductions;
  std::vector<std::size_t> layerEnds;
};

/** Connect the encoder in checkpoint layer order and preserve each rank cut.
 * The result uses the embedding's owned sidecar. The returned context is
 * trimmed to the number of valid positions in the tokenizer's key mask.
 */
Umt5EncoderLayout
connectUmt5Encoder(Umt5EmbeddingLayout embedding, std::vector<Umt5EncoderLayer> layers,
                   Umt5RmsLayout finalRms);

/** Bind all tensors in a Wan2.1 UMT5 encoder checkpoint to ordinary ONNX partitions. */
Umt5EncoderLayout
lowerWan21Umt5Encoder(const WanCheckpoint& checkpoint, std::int64_t tokens,
                      std::uint64_t scoreBytes);

/** Compile the full encoder into two rank roles with a symbolic-length context result. */
OnnxParallelPlan
makeUmt5TensorParallelPlan(const Umt5EncoderLayout& layout, const NativeStageLayout& stage);

/** Cut the encoder between complete layers and assign each region to one unit. */
OnnxParallelPlan
makeUmt5PipelinePlan(const Umt5EncoderLayout& layout, std::size_t prefixLayers,
                     const std::array<NativeStageLayout, 2>& stages);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_UMT5_HPP
