#ifndef NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
#define NDNSF_DI_QWEN_TENSOR_SPLIT_HPP

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
struct QwenTpShape
{
  std::uint64_t hidden = 0;
  std::uint64_t layers = 0;
  std::uint64_t queryHeads = 0;
  std::uint64_t kvHeads = 0;
  std::uint64_t headDim = 0;
  std::uint64_t ffn = 0;

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

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
