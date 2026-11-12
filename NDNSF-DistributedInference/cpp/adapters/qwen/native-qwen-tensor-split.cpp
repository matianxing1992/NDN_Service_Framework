#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ndnsf::di::qwen {
namespace {

/** Reject zero, non-divisible or overflowing partitions before any slicing. */
std::uint64_t
checkedPart(std::uint64_t total, std::uint64_t degree, const char* label)
{
  if (degree == 0 || total == 0) {
    throw std::invalid_argument(std::string("tensor-parallel ") + label + " is empty");
  }
  if (total % degree != 0) {
    throw std::invalid_argument(std::string("tensor-parallel ") + label +
                                " does not split evenly");
  }
  return total / degree;
}

void
addSlice(QwenRankLayerSlices& out, const std::string& base,
         QwenSliceAxis axis, std::uint64_t begin, std::uint64_t end,
         std::uint64_t slicedK, std::uint64_t slicedN)
{
  QwenInitializerSlice slice;
  slice.name = base;
  slice.axis = axis;
  slice.begin = begin;
  slice.end = end;
  slice.slicedK = slicedK;
  slice.slicedN = slicedN;
  slice.validate();
  out.initializers.push_back(std::move(slice));
}

} // namespace

void
QwenTpShape::validate() const
{
  if (hidden == 0 || layers == 0 || queryHeads == 0 || kvHeads == 0 ||
      headDim == 0 || ffn == 0) {
    throw std::invalid_argument("tensor-parallel shape facts are incomplete");
  }
  if (queryHeads % kvHeads != 0) {
    throw std::invalid_argument("tensor-parallel GQA grouping is not integral");
  }
  // The attention width is `queryHeads * headDim`, which need not equal the
  // residual width: the frozen export uses headDim 128 over a hidden width of
  // 1024, so the Q/O projections are twice as wide as the residual stream.
  // Only overflow is common to every accepted geometry.
  if (queryHeads * headDim < queryHeads ||
      kvHeads * headDim < kvHeads) {
    throw std::overflow_error("tensor-parallel head geometry exceeds uint64");
  }
  // The residual stream is the hidden width; every projection that produces a
  // full-width partial must therefore reduce over a multiple of it.
  if (hidden > std::numeric_limits<std::uint64_t>::max() / layers) {
    throw std::overflow_error("tensor-parallel shape exceeds uint64");
  }
}

void
QwenInitializerSlice::validate() const
{
  if (name.empty() || end <= begin) {
    throw std::invalid_argument("tensor-parallel initializer slice is empty");
  }
  if (axis == QwenSliceAxis::InputChannels) {
    if (slicedK == 0 || (begin % QWEN_INT4_BLOCK_SIZE) != 0 ||
        (end % QWEN_INT4_BLOCK_SIZE) != 0) {
      throw std::invalid_argument("tensor-parallel K slice is not block aligned");
    }
  }
  else if (slicedN == 0) {
    throw std::invalid_argument("tensor-parallel N slice is empty");
  }
}

std::vector<QwenRankLayerSlices>
planQwenTensorSlices(const QwenTpShape& shape, std::uint64_t degree,
                     std::uint64_t rank)
{
  shape.validate();
  if (degree < 2) {
    throw std::invalid_argument("tensor-parallel planning requires degree >= 2");
  }
  if (rank >= degree) {
    throw std::invalid_argument("tensor-parallel rank is outside its degree");
  }

  // Whole heads split; a partial head is never acceptable.
  const auto queryHeadsPerRank = checkedPart(shape.queryHeads, degree, "query heads");
  const auto kvHeadsPerRank = checkedPart(shape.kvHeads, degree, "kv heads");
  const auto ffnPerRank = checkedPart(shape.ffn, degree, "ffn width");

  const auto queryBegin = rank * queryHeadsPerRank * shape.headDim;
  const auto queryEnd = queryBegin + queryHeadsPerRank * shape.headDim;
  const auto kvBegin = rank * kvHeadsPerRank * shape.headDim;
  const auto kvEnd = kvBegin + kvHeadsPerRank * shape.headDim;
  const auto ffnBegin = rank * ffnPerRank;
  const auto ffnEnd = ffnBegin + ffnPerRank;

  // The frozen export's K width must let the O projection be cut on blocks.
  if (shape.queryWidth() % degree != 0 || (shape.queryWidth() / degree) % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("tensor-parallel O reduction width is not block aligned");
  }
  if ((shape.ffn / degree) % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("tensor-parallel down reduction width is not block aligned");
  }

  std::vector<QwenRankLayerSlices> result;
  result.reserve(shape.layers);
  for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
    const auto prefix = "model.layers." + std::to_string(layer) + ".";
    QwenRankLayerSlices slices;
    slices.layer = layer;

    // Q/K/V: whole heads, output-channel slices.
    addSlice(slices, prefix + "attn.q_proj.MatMul.weight", QwenSliceAxis::OutputChannels,
             queryBegin, queryEnd, shape.hidden, queryHeadsPerRank * shape.headDim);
    addSlice(slices, prefix + "attn.k_proj.MatMul.weight", QwenSliceAxis::OutputChannels,
             kvBegin, kvEnd, shape.hidden, kvHeadsPerRank * shape.headDim);
    addSlice(slices, prefix + "attn.v_proj.MatMul.weight", QwenSliceAxis::OutputChannels,
             kvBegin, kvEnd, shape.hidden, kvHeadsPerRank * shape.headDim);

    // O: reduction over the same attention channels, producing a full-D partial.
    addSlice(slices, prefix + "attn.o_proj.MatMul.weight", QwenSliceAxis::InputChannels,
             queryBegin, queryEnd, shape.queryWidth() / degree, shape.hidden);

    // gate/up share one FFN interval; down reduces over it.
    addSlice(slices, prefix + "mlp.gate_proj.MatMul.weight", QwenSliceAxis::OutputChannels,
             ffnBegin, ffnEnd, shape.hidden, ffnPerRank);
    addSlice(slices, prefix + "mlp.up_proj.MatMul.weight", QwenSliceAxis::OutputChannels,
             ffnBegin, ffnEnd, shape.hidden, ffnPerRank);
    addSlice(slices, prefix + "mlp.down_proj.MatMul.weight", QwenSliceAxis::InputChannels,
             ffnBegin, ffnEnd, shape.ffn / degree, shape.hidden);

    // Norm weights stay replicated: splitting a normalization axis would add a
    // second cross-rank reduction, so each rank keeps the full vector.
    result.push_back(std::move(slices));
  }
  return result;
}

std::uint64_t
packedInt4Bytes(std::uint64_t n, std::uint64_t k)
{
  if (n == 0 || k == 0 || k % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("packed INT4 initializer geometry is invalid");
  }
  const auto blocks = k / QWEN_INT4_BLOCK_SIZE;
  if (n > std::numeric_limits<std::uint64_t>::max() / blocks ||
      n * blocks > std::numeric_limits<std::uint64_t>::max() / QWEN_INT4_BLOB_BYTES) {
    throw std::overflow_error("packed INT4 initializer size exceeds uint64");
  }
  return n * blocks * QWEN_INT4_BLOB_BYTES;
}

std::vector<std::uint8_t>
slicePackedInt4(const std::vector<std::uint8_t>& packed, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end)
{
  if (packed.size() != packedInt4Bytes(n, k)) {
    throw std::invalid_argument("packed INT4 initializer byte count is inconsistent");
  }
  if (end <= begin || end > (axis == QwenSliceAxis::OutputChannels ? n : k)) {
    throw std::invalid_argument("packed INT4 slice range is outside the tensor");
  }
  const auto blocks = k / QWEN_INT4_BLOCK_SIZE;
  const auto rowBytes = blocks * QWEN_INT4_BLOB_BYTES;

  if (axis == QwenSliceAxis::OutputChannels) {
    // Whole outer rows: no nibble is split.
    const auto first = begin * rowBytes;
    const auto last = end * rowBytes;
    return std::vector<std::uint8_t>(packed.begin() + static_cast<std::ptrdiff_t>(first),
                                     packed.begin() + static_cast<std::ptrdiff_t>(last));
  }

  // Input-channel slice: keep every output row, but only the selected block
  // columns. Block alignment is what makes this a whole-byte copy.
  if (begin % QWEN_INT4_BLOCK_SIZE != 0 || end % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("packed INT4 K slice is not block aligned");
  }
  const auto firstBlock = begin / QWEN_INT4_BLOCK_SIZE;
  const auto blockCount = (end - begin) / QWEN_INT4_BLOCK_SIZE;
  const auto sliceRowBytes = blockCount * QWEN_INT4_BLOB_BYTES;
  std::vector<std::uint8_t> result;
  result.reserve(static_cast<std::size_t>(n) * sliceRowBytes);
  for (std::uint64_t row = 0; row < n; ++row) {
    const auto rowStart = row * rowBytes + firstBlock * QWEN_INT4_BLOB_BYTES;
    result.insert(result.end(),
                  packed.begin() + static_cast<std::ptrdiff_t>(rowStart),
                  packed.begin() + static_cast<std::ptrdiff_t>(rowStart + sliceRowBytes));
  }
  return result;
}

std::vector<std::uint8_t>
sliceInt4Scales(const std::vector<std::uint8_t>& scales, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end, std::size_t scaleBytes)
{
  if (scaleBytes == 0 || k % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("INT4 scale geometry is invalid");
  }
  const auto blocks = k / QWEN_INT4_BLOCK_SIZE;
  if (scales.size() != static_cast<std::size_t>(n * blocks) * scaleBytes) {
    throw std::invalid_argument("INT4 scale byte count is inconsistent");
  }
  if (end <= begin || end > (axis == QwenSliceAxis::OutputChannels ? n : k)) {
    throw std::invalid_argument("INT4 scale slice range is outside the tensor");
  }

  if (axis == QwenSliceAxis::OutputChannels) {
    // One contiguous run: rows are the outer axis of the flattened scales.
    const auto first = static_cast<std::size_t>(begin * blocks) * scaleBytes;
    const auto last = static_cast<std::size_t>(end * blocks) * scaleBytes;
    return std::vector<std::uint8_t>(scales.begin() + static_cast<std::ptrdiff_t>(first),
                                     scales.begin() + static_cast<std::ptrdiff_t>(last));
  }

  if (begin % QWEN_INT4_BLOCK_SIZE != 0 || end % QWEN_INT4_BLOCK_SIZE != 0) {
    throw std::invalid_argument("INT4 scale K slice is not block aligned");
  }
  const auto firstBlock = begin / QWEN_INT4_BLOCK_SIZE;
  const auto blockCount = (end - begin) / QWEN_INT4_BLOCK_SIZE;
  const auto rowBytes = static_cast<std::size_t>(blocks) * scaleBytes;
  std::vector<std::uint8_t> result;
  result.reserve(static_cast<std::size_t>(n) * blockCount * scaleBytes);
  for (std::uint64_t row = 0; row < n; ++row) {
    const auto rowStart = static_cast<std::size_t>(row) * rowBytes +
                          static_cast<std::size_t>(firstBlock) * scaleBytes;
    result.insert(result.end(),
                  scales.begin() + static_cast<std::ptrdiff_t>(rowStart),
                  scales.begin() + static_cast<std::ptrdiff_t>(
                    rowStart + static_cast<std::size_t>(blockCount) * scaleBytes));
  }
  return result;
}

} // namespace ndnsf::di::qwen
