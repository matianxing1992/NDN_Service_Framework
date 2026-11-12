#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <onnx/onnx_pb.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
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

std::vector<QwenNBitsNode>
inspectQwenNBitsGraph(const std::vector<std::uint8_t>& modelBytes,
                      std::uint64_t expectedLayers)
{
  if (modelBytes.empty()) {
    throw std::invalid_argument("tensor-parallel graph source is empty");
  }
  onnx::ModelProto model;
  if (!model.ParseFromArray(modelBytes.data(), static_cast<int>(modelBytes.size()))) {
    throw std::invalid_argument("tensor-parallel graph source is not a valid ONNX model");
  }
  const auto& graph = model.graph();
  std::set<std::uint64_t> layers;
  std::vector<QwenNBitsNode> result;
  for (int index = 0; index < graph.node_size(); ++index) {
    const auto& node = graph.node(index);
    // Only the maintained INT4 linear operator is accepted; any other large
    // operator would need a partition rule this design does not define.
    if (node.op_type() != "MatMulNBits") continue;
    if (node.domain() != "com.microsoft") {
      throw std::invalid_argument("tensor-parallel graph uses a foreign MatMulNBits domain");
    }
    QwenNBitsNode facts;
    facts.nodeIndex = static_cast<std::uint64_t>(index);
    facts.nodeName = node.name();
    facts.bits = 0;
    facts.blockSize = 0;
    for (const auto& attribute : node.attribute()) {
      if (attribute.name() == "bits") facts.bits = static_cast<std::uint64_t>(attribute.i());
      else if (attribute.name() == "block_size") {
        facts.blockSize = static_cast<std::uint64_t>(attribute.i());
      }
      else if (attribute.name() == "K") facts.k = static_cast<std::uint64_t>(attribute.i());
      else if (attribute.name() == "N") facts.n = static_cast<std::uint64_t>(attribute.i());
    }
    if (facts.bits != QWEN_INT4_BITS || facts.blockSize != QWEN_INT4_BLOCK_SIZE) {
      throw std::invalid_argument("tensor-parallel graph is not weight-only INT4 with block " +
                                  std::to_string(QWEN_INT4_BLOCK_SIZE));
    }
    if (facts.k == 0 || facts.n == 0) {
      throw std::invalid_argument("tensor-parallel graph has a MatMulNBits without K/N");
    }
    // Inputs are (A, B[, bias][, zero_point][, g_idx]); only the packed weight
    // and its scales are expected in the frozen export.
    if (node.input_size() < 2) {
      throw std::invalid_argument("tensor-parallel graph has a MatMulNBits without weights");
    }
    facts.weightInitializer = node.input(0);
    facts.scalesInitializer = node.input(1);
    facts.hasBias = node.input_size() > 2 && !node.input(2).empty();
    facts.hasZeroPoint = node.input_size() > 3 && !node.input(3).empty();
    facts.hasGroupIndex = node.input_size() > 4 && !node.input(4).empty();
    if (facts.hasZeroPoint || facts.hasGroupIndex) {
      throw std::invalid_argument("tensor-parallel graph has an unsupported "
                                  "zero-point or group-index MatMulNBits");
    }
    // Layer index comes from the maintained export naming, not from ordinal.
    const auto marker = facts.nodeName.find("layers.");
    if (marker != std::string::npos) {
      const auto begin = marker + 7;
      std::uint64_t layer = 0;
      std::size_t digits = 0;
      while (begin + digits < facts.nodeName.size() &&
             std::isdigit(static_cast<unsigned char>(facts.nodeName[begin + digits]))) {
        layer = layer * 10 + static_cast<std::uint64_t>(facts.nodeName[begin + digits] - '0');
        ++digits;
      }
      if (digits == 0) {
        throw std::invalid_argument("tensor-parallel graph node name has no layer index");
      }
      layers.insert(layer);
    }
    result.push_back(std::move(facts));
  }
  if (result.empty()) {
    throw std::invalid_argument("tensor-parallel graph has no MatMulNBits operator");
  }
  if (expectedLayers != 0 && layers.size() != expectedLayers) {
    throw std::invalid_argument("tensor-parallel graph layer count differs from the plan");
  }
  return result;
}

QwenTpShape
qwenTpShapeFromNodes(const std::vector<QwenNBitsNode>& nodes,
                     std::uint64_t hidden, std::uint64_t layers,
                     std::uint64_t queryHeads, std::uint64_t kvHeads,
                     std::uint64_t headDim, std::uint64_t ffn)
{
  QwenTpShape shape;
  shape.hidden = hidden;
  shape.layers = layers;
  shape.queryHeads = queryHeads;
  shape.kvHeads = kvHeads;
  shape.headDim = headDim;
  shape.ffn = ffn;
  shape.validate();
  // Cross-check the declared geometry against the graph itself: a projection
  // whose width disagrees with `heads * headDim` (or the FFN width) means the
  // constants and the export diverge, and slicing them would be wrong.
  std::size_t query = 0, key = 0, value = 0, out = 0, gate = 0, up = 0, down = 0;
  for (const auto& node : nodes) {
    const auto& name = node.nodeName;
    if (name.find("q_proj") != std::string::npos) {
      if (node.n != shape.queryWidth()) {
        throw std::invalid_argument("tensor-parallel q_proj width disagrees with head geometry");
      }
      ++query;
    }
    else if (name.find("k_proj") != std::string::npos) {
      if (node.n != shape.kvWidth()) {
        throw std::invalid_argument("tensor-parallel k_proj width disagrees with head geometry");
      }
      ++key;
    }
    else if (name.find("v_proj") != std::string::npos) {
      if (node.n != shape.kvWidth()) {
        throw std::invalid_argument("tensor-parallel v_proj width disagrees with head geometry");
      }
      ++value;
    }
    else if (name.find("o_proj") != std::string::npos) {
      if (node.k != shape.queryWidth() || node.n != shape.hidden) {
        throw std::invalid_argument("tensor-parallel o_proj shape disagrees with head geometry");
      }
      ++out;
    }
    else if (name.find("gate_proj") != std::string::npos) {
      if (node.n != shape.ffn) {
        throw std::invalid_argument("tensor-parallel gate_proj width disagrees with ffn");
      }
      ++gate;
    }
    else if (name.find("up_proj") != std::string::npos) {
      if (node.n != shape.ffn) {
        throw std::invalid_argument("tensor-parallel up_proj width disagrees with ffn");
      }
      ++up;
    }
    else if (name.find("down_proj") != std::string::npos) {
      if (node.k != shape.ffn || node.n != shape.hidden) {
        throw std::invalid_argument("tensor-parallel down_proj shape disagrees with ffn");
      }
      ++down;
    }
  }
  if (query != shape.layers || key != shape.layers || value != shape.layers ||
      out != shape.layers || gate != shape.layers || up != shape.layers ||
      down != shape.layers) {
    throw std::invalid_argument("tensor-parallel graph does not cover every maintained projection");
  }
  return shape;
}

} // namespace ndnsf::di::qwen
