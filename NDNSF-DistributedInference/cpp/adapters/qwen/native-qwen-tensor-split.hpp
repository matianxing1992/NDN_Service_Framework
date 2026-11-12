#ifndef NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
#define NDNSF_DI_QWEN_TENSOR_SPLIT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di::qwen {

/** Block width of the frozen weight-only INT4 export (`MatMulNBits`). */
inline constexpr std::uint64_t QWEN_INT4_BITS = 4;
inline constexpr std::uint64_t QWEN_INT4_BLOCK_SIZE = 32;
/** Nibbles packed per byte, i.e. the innermost `MatMulNBits` weight stride. */
inline constexpr std::uint64_t QWEN_INT4_BLOB_BYTES =
  QWEN_INT4_BLOCK_SIZE * QWEN_INT4_BITS / 8;

/** Shape facts a tensor-parallel plan needs. These are read from the frozen
 * export; nothing here may be guessed from a model name. */
/** Layout digest of every activation this template moves: contiguous row-major
 * float32 over the trailing width. One constant is correct because every slot
 * shares that layout; a slot with a different physical layout would need its
 * own value.
 */
inline constexpr const char* QWEN_TP_ACTIVATION_LAYOUT_DIGEST =
  "sha256:6f2f1e2b9a0d4c3e8b7a5f6d1c2e3b4a5f60718293a4b5c6d7e8f90a1b2c3d4e";

/** Layout digest of the integer side inputs: token ids, position ids and the
 * attention mask. They are a different physical layout from the activations,
 * which is why they must not reuse the activation digest.
 */
inline constexpr const char* QWEN_TP_INDEX_LAYOUT_DIGEST =
  "sha256:2b8e4d1f7c3a5960a4d2e8b1c6f7093a5e7d2b4c8f1a3e6d9b0c2f5a7e1d3b6c";

struct QwenTpShape
{
  std::uint64_t hidden = 0;
  std::uint64_t layers = 0;
  std::uint64_t queryHeads = 0;
  std::uint64_t kvHeads = 0;
  std::uint64_t headDim = 0;
  std::uint64_t ffn = 0;
  /** Vocabulary width of the head output. The frozen export ties it to the
   * embedding table (151936 rows for Qwen3-0.6B), so it is a verified fact and
   * not a guess from the hidden width.
   */
  std::uint64_t vocab = 0;

  /** Reject a shape that the TP arithmetic cannot split exactly. */
  void validate() const;
  /** Query projection output width: `queryHeads * headDim`. */
  std::uint64_t queryWidth() const { return queryHeads * headDim; }
  /** Key/value projection output width: `kvHeads * headDim`. */
  std::uint64_t kvWidth() const { return kvHeads * headDim; }
};

/** Which tensor dimension a slice removes. */
enum class QwenSliceAxis
{
  /** Split the output channels (the `N` axis of `MatMulNBits`). */
  OutputChannels,
  /** Split the reduction channels (the `K` axis of `MatMulNBits`). */
  InputChannels,
};

/** One initializer range a rank must own. */
struct QwenInitializerSlice
{
  std::string name;
  QwenSliceAxis axis = QwenSliceAxis::OutputChannels;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  /** `K` after slicing; only meaningful for InputChannels slices. */
  std::uint64_t slicedK = 0;
  /** `N` after slicing; only meaningful for OutputChannels slices. */
  std::uint64_t slicedN = 0;

  void validate() const;
};

/** Every slice one rank of one layer owns. */
struct QwenRankLayerSlices
{
  std::uint64_t layer = 0;
  std::vector<QwenInitializerSlice> initializers;
};

/**
 * Derive the exact per-rank slice list for one tensor-parallel degree.
 *
 * The partition follows the frozen export: Q/K/V/gate/up split their output
 * channels (whole heads for the projections, the same FFN interval for gate
 * and up), while O and down split the reduction channels and therefore emit a
 * full-width `D` partial that the runtime sums.
 *
 * @param shape  Verified shape facts.
 * @param degree Tensor-parallel degree (first acceptance: 2).
 * @param rank   Rank in `[0, degree)`.
 * @returns The initializer slices this rank owns, in layer order.
 * @throws std::invalid_argument for a shape/degree/rank that cannot split exactly.
 */
std::vector<QwenRankLayerSlices>
planQwenTensorSlices(const QwenTpShape& shape, std::uint64_t degree,
                     std::uint64_t rank);

/**
 * Slice one packed INT4 initializer along the plan's axis.
 *
 * Packed weights are `[N, K / block, blob]` little-endian nibbles. An output
 * slice copies whole outer rows; an input slice copies whole block columns,
 * which is exactly why the cut must stay block aligned. Neither path re-packs a
 * nibble, so the original quantized values survive unchanged.
 *
 * @param packed     Source bytes of the packed tensor.
 * @param n          Source output width.
 * @param k          Source reduction width.
 * @param axis       Slice axis.
 * @param begin,end  Half-open range on that axis.
 * @returns The rank's packed bytes.
 */
std::vector<std::uint8_t>
slicePackedInt4(const std::vector<std::uint8_t>& packed, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end);

/**
 * Slice the per-block scales that accompany a packed INT4 initializer.
 *
 * Scales are `[N, K / block]` flattened: one float per (output channel, block)
 * pair, matching the packed weight's own (row, block-column) indexing.
 *
 * @param scales      Source scale bytes (`N * K / block * 4`).
 * @param n           Source output width.
 * @param k           Source reduction width.
 * @param axis        Slice axis; must match the weight slice.
 * @param begin,end   Half-open range on that axis.
 * @param scaleBytes  Width of one scale element (4 for float32).
 * @returns The rank's scale bytes.
 */
std::vector<std::uint8_t>
sliceInt4Scales(const std::vector<std::uint8_t>& scales, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end, std::size_t scaleBytes = 4);

/** Total packed-weight bytes of one `[N, K]` INT4 initializer. */
std::uint64_t packedInt4Bytes(std::uint64_t n, std::uint64_t k);

/** One `MatMulNBits` node found while reading the frozen export. */
struct QwenNBitsNode
{
  std::uint64_t nodeIndex = 0;
  std::string nodeName;
  std::string weightInitializer;
  std::string scalesInitializer;
  std::uint64_t k = 0;
  std::uint64_t n = 0;
  std::uint64_t bits = 0;
  std::uint64_t blockSize = 0;
  bool hasBias = false;
  bool hasZeroPoint = false;
  bool hasGroupIndex = false;
};

/**
 * Read the frozen export's `MatMulNBits` facts.
 *
 * This is the only place TP shape facts come from. It parses the graph on the
 * caller's behalf so `planQwenTensorSlices` never trusts a model name or a
 * hand-written constant: an unsupported export (extra operator, non-INT4
 * bits, block size mismatch, zero-point/g_idx/bias) is rejected by name.
 *
 * @param modelBytes Serialized ONNX bytes (external data may stay on disk).
 * @param layers     Expected transformer layer count; 0 disables the check.
 * @returns Per-node facts in graph order.
 * @throws std::invalid_argument for an export this TP partition cannot handle.
 */
std::vector<QwenNBitsNode>
inspectQwenNBitsGraph(const std::vector<std::uint8_t>& modelBytes,
                      std::uint64_t expectedLayers = 0);

/** Derive the TP shape facts from an already-inspected node list. */
QwenTpShape
qwenTpShapeFromNodes(const std::vector<QwenNBitsNode>& nodes,
                     std::uint64_t hidden, std::uint64_t layers,
                     std::uint64_t queryHeads, std::uint64_t kvHeads,
                     std::uint64_t headDim, std::uint64_t ffn,
                     std::uint64_t vocab);

/** One rank unit the TP plan exposes to placement. */
struct QwenTpUnit
{
  std::string roleId;
  std::uint64_t rank = 0;
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
};

/** The complete sealed TP template for one model and degree. */
struct QwenTpTemplate
{
  std::uint64_t degree = 1;
  std::vector<QwenTpUnit> units;
  // Per rank, per layer: the initializer slices that rank owns.
  std::vector<std::vector<QwenRankLayerSlices>> slicesByRank;
};

/**
 * Build the unit/step template for one verified shape and degree.
 *
 * Each unit covers every layer; inside a token a unit runs one attention step,
 * one SUM, one MLP step and one SUM per layer, then the head on rank 0 only.
 * The steps reference recipe entries by name, so the caller supplies the entry
 * naming rule it will actually assemble (`entryPrefix`).
 *
 * @param shape       Verified shape facts.
 * @param degree      Tensor-parallel degree.
 * @param entryPrefix Prefix for generated recipe entry ids.
 * @returns Units and per-rank slice plans.
 * @throws std::invalid_argument for an unsplittable shape or degree.
 */
QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix);

/**
 * Project one template into sealed execution steps and dependencies.
 *
 * The result is deliberately data, not a second scheduler: `roleSpecFor` and
 * the existing worker consume it exactly like any other plan. Step ids encode
 * their token-local position so the runtime can bind a contribution to the
 * exact step that produced it.
 *
 * @param plan       Plan receiving the roles, steps and dependencies.
 * @param shape      Verified shape facts.
 * @param degree     Tensor-parallel degree.
 * @param entryPrefix Prefix for generated recipe entry ids.
 * @param egressRole  Role that owns the final response.
 */
void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole);

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
