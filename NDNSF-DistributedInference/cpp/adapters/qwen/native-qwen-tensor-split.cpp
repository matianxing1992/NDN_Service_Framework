#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"

#include <onnx/onnx_pb.h>

#include <ndn-cxx/util/sha256.hpp>

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di::qwen {

namespace {

/** Turn a unit id or kind into a file-name component.
 *
 * Unit ids carry role separators and report characters, so the name is reduced
 * to [A-Za-z0-9_-] rather than escaped: an entry file is internal to one
 * assembly root, and a predictable name is worth more there than a reversible
 * one.
 */
std::string
sanitizeEntryFileName(const std::string& value)
{
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    const auto candidate = static_cast<unsigned char>(ch);
    if (std::isalnum(candidate) != 0 || ch == '-' || ch == '_') {
      result.push_back(ch);
    }
    else if (!result.empty() && result.back() != '_') {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') result.pop_back();
  if (result.empty()) {
    throw std::invalid_argument("tensor-parallel entry name has no usable characters");
  }
  return result;
}

/** Read a file back for verification. */
std::vector<std::uint8_t>
readEntryFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("tensor-parallel entry file is unreadable: " +
                             path.string());
  }
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

/** Canonical lowercase SHA-256 over the bytes. */
std::string
sha256Hex(const std::vector<std::uint8_t>& bytes)
{
  ndn::util::Sha256 digest;
  digest.update(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

} // namespace
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
    // Official com.microsoft.MatMulNBits input order:
    //   A (activation), B (packed weight), scales,
    //   zero_points?, g_idx?, bias?
    // Input 0 is the activation, not a weight: reading B from it would take a
    // graph tensor for the packed initializer. The optional slots sit after
    // scales and may be present-but-empty, which ONNX uses for "absent".
    if (node.input_size() < 3 ||
        node.input(1).empty() || node.input(2).empty()) {
      throw std::invalid_argument(
        "tensor-parallel graph has a MatMulNBits without packed weight or scales");
    }
    const auto present = [&node] (int index) {
      return node.input_size() > index && !node.input(index).empty();
    };
    facts.weightInitializer = node.input(1);
    facts.scalesInitializer = node.input(2);
    facts.hasZeroPoint = present(3);
    facts.hasGroupIndex = present(4);
    facts.hasBias = present(5);
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
                     std::uint64_t headDim, std::uint64_t ffn,
                     std::uint64_t vocab)
{
  QwenTpShape shape;
  shape.hidden = hidden;
  shape.layers = layers;
  shape.queryHeads = queryHeads;
  shape.kvHeads = kvHeads;
  shape.headDim = headDim;
  shape.ffn = ffn;
  shape.vocab = vocab;
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

namespace {

QwenTpUnit
unitFor(std::uint64_t rank, const QwenTpShape& shape)
{
  QwenTpUnit unit;
  unit.roleId = "S0R" + std::to_string(rank);
  unit.rank = rank;
  unit.layerBegin = 0;
  unit.layerEnd = shape.layers;
  return unit;
}

ndnsf::di::NativeStepSlotSpec
makeSlot(const std::string& id, const std::string& source,
         const std::string& producerStep = {}, std::uint64_t lastDim = 0)
{
  ndnsf::di::NativeStepSlotSpec slot;
  slot.slotId = id;
  // Every value crossing a step boundary is a float32 activation. The first two
  // dimensions stay symbolic because the batch and sequence lengths are chosen
  // per request; the trailing width is a verified model fact and is therefore
  // concrete whenever the caller knows it.
  slot.dtype = "float32";
  slot.shape = {std::int64_t(1), std::string("sequence"),
                lastDim == 0 ? std::variant<std::int64_t, std::string>(std::string("hidden"))
                             : std::variant<std::int64_t, std::string>(std::int64_t(lastDim))};
  // One stable activation layout for the whole template: contiguous row-major
  // over the trailing dimension. A single digest is right because every slot
  // shares it; different layouts would need different digests.
  slot.layoutDigest = QWEN_TP_ACTIVATION_LAYOUT_DIGEST;
  slot.source = source;
  slot.producerStepId = producerStep;
  return slot;
}

/** Same as makeSlot but binds the exact producer output slot as well. The data
 * model requires that binding: a consumer names its own input slot, so the
 * producer's output cannot be found by name.
 */
ndnsf::di::NativeStepSlotSpec
makeFedSlot(const std::string& id, const std::string& source,
            const std::string& producerStep, const std::string& producerSlot,
            std::uint64_t lastDim)
{
  auto slot = makeSlot(id, source, producerStep, lastDim);
  slot.producerSlotId = producerSlot;
  return slot;
}

/** Integer slot for token ids, positions and the attention mask.
 *
 * These are not activations: the frozen export feeds int64 ids through a Gather
 * and casts the mask to int32 before GroupQueryAttention, so declaring them
 * float32 would misdescribe the wire. Their widths are batch/sequence only,
 * which stay symbolic for the same reason the activations do.
 */
ndnsf::di::NativeStepSlotSpec
makeIndexSlot(const std::string& id, const std::string& source, const char* dtype,
              const std::string& producerStep = {})
{
  ndnsf::di::NativeStepSlotSpec slot;
  slot.slotId = id;
  slot.dtype = dtype;
  slot.shape = {std::int64_t(1), std::string("sequence")};
  slot.layoutDigest = QWEN_TP_INDEX_LAYOUT_DIGEST;
  slot.source = source;
  slot.producerStepId = producerStep;
  return slot;
}

ndnsf::di::NativeStepSlotSpec
makeFedIndexSlot(const std::string& id, const std::string& source, const char* dtype,
                 const std::string& producerStep, const std::string& producerSlot)
{
  auto slot = makeIndexSlot(id, source, dtype, producerStep);
  slot.producerSlotId = producerSlot;
  return slot;
}

std::string
initStep(const std::string& unit)
{
  return unit + "/INIT";
}

std::string
attnStep(const std::string& unit, std::uint64_t layer)
{
  return unit + "/ATTN/" + std::to_string(layer);
}

std::string
sumAttnStep(const std::string& unit, std::uint64_t layer)
{
  return unit + "/SUM_ATTN/" + std::to_string(layer);
}

std::string
mlpStep(const std::string& unit, std::uint64_t layer)
{
  return unit + "/MLP/" + std::to_string(layer);
}

std::string
sumMlpStep(const std::string& unit, std::uint64_t layer)
{
  return unit + "/SUM_MLP/" + std::to_string(layer);
}

} // namespace

namespace {

/** Shared validation for both template builders. */
void
validateTemplateInputs(const QwenTpShape& shape, std::uint64_t degree,
                       const std::string& entryPrefix)
{
  shape.validate();
  if (degree < 2) {
    throw std::invalid_argument("tensor-parallel template requires degree >= 2");
  }
  if (entryPrefix.empty()) {
    throw std::invalid_argument("tensor-parallel template requires an entry prefix");
  }
}

/** A caller-supplied unit list must describe exactly this degree and cover every
 * layer: a tensor-parallel rank holds a shard of all layers, never a range.
 */
void
validateUnits(const std::vector<QwenTpUnit>& units, std::uint64_t degree,
              std::uint64_t layers)
{
  if (units.size() != degree) {
    throw std::invalid_argument(
      "tensor-parallel units do not cover the declared degree");
  }
  std::set<std::uint64_t> ranks;
  std::set<std::string> roleIds;
  for (const auto& unit : units) {
    if (unit.roleId.empty() || !roleIds.insert(unit.roleId).second) {
      throw std::invalid_argument(
        "tensor-parallel units have an empty or duplicate role id");
    }
    if (unit.rank >= degree || !ranks.insert(unit.rank).second) {
      throw std::invalid_argument(
        "tensor-parallel units have a duplicate or out-of-range rank");
    }
    if (unit.layerBegin != 0 || unit.layerEnd != layers) {
      throw std::invalid_argument(
        "tensor-parallel unit does not cover every layer");
    }
  }
}

} // namespace

QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix)
{
  validateTemplateInputs(shape, degree, entryPrefix);
  std::vector<QwenTpUnit> units;
  units.reserve(degree);
  for (std::uint64_t rank = 0; rank < degree; ++rank)
    units.push_back(unitFor(rank, shape));
  return buildQwenTpTemplate(shape, degree, entryPrefix, units);
}

QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix,
                    const std::vector<QwenTpUnit>& units)
{
  validateTemplateInputs(shape, degree, entryPrefix);
  validateUnits(units, degree, shape.layers);
  QwenTpTemplate result;
  result.degree = degree;
  result.units = units;
  // Slices are indexed by rank, so the per-rank plans follow the unit ranks
  // rather than the list order.
  result.slicesByRank.resize(degree);
  for (const auto& unit : units)
    result.slicesByRank[unit.rank] = planQwenTensorSlices(shape, degree, unit.rank);
  return result;
}

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole)
{
  std::vector<QwenTpUnit> units;
  units.reserve(degree);
  for (std::uint64_t rank = 0; rank < degree; ++rank)
    units.push_back(unitFor(rank, shape));
  projectQwenTpPlan(plan, shape, degree, entryPrefix, egressRole, units);
}

namespace {

/** Shared projection body.
 *
 * `verified` is null when the caller let the projection name its own entries,
 * and non-null when the caller supplied assembled entry contracts. The body
 * resolves every CALL_SUBGRAPH step through whichever rule applies, so there is
 * exactly one projector rather than two that can drift apart.
 */
void
projectTemplate(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                std::uint64_t degree, const std::string& entryPrefix,
                const std::string& egressRole, const QwenTpTemplate& tp,
                const std::vector<QwenTpEntryContract>* verified)
{
  // The egress unit owns the head, so it must be one of the declared units.
  if (std::none_of(tp.units.begin(), tp.units.end(),
                   [&egressRole] (const auto& unit) { return unit.roleId == egressRole; })) {
    throw std::invalid_argument(
      "tensor-parallel egress role is not one of the declared units");
  }
  plan.roles.clear();
  plan.steps.clear();
  plan.dependencies.clear();
  for (const auto& unit : tp.units) plan.roles.push_back(unit.roleId);
  plan.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V1;
  plan.tensorDegree = degree;
  plan.modelFamily = "qwen";
  plan.plannerKind = "native-qwen-tensor";

  // Resolve one step's artifact entry. With verified contracts the id must come
  // from a contract owned by the same unit and carrying the same kind and layer;
  // inventing a name would let a plan reference an entry the assembler never
  // produced, which only fails much later inside the worker.
  const auto entry = [&] (const std::string& unit, const char* kind,
                          std::uint64_t layer) {
    if (verified == nullptr)
      return entryPrefix + "/" + unit + "/" + kind + "/" + std::to_string(layer);
    const auto matched = std::find_if(verified->begin(), verified->end(),
      [&] (const auto& contract) {
        return contract.unitId == unit && contract.kind == kind &&
               contract.layer == layer;
      });
    if (matched == verified->end()) {
      throw std::invalid_argument(
        "tensor-parallel step has no verified entry contract for its unit, "
        "kind and layer");
    }
    return matched->entryId;
  };

  for (const auto& unit : tp.units) {
    // INIT runs once per unit before any layer: it turns the request's raw
    // inputs into the typed side inputs every attention step consumes. Without
    // it the template would have no admitted producer for positions and mask,
    // and each attention entry would have to invent them.
    ndnsf::di::NativeExecutionStepSpec init;
    init.stepId = initStep(unit.roleId);
    init.unitId = unit.roleId;
    init.operation = "CALL_SUBGRAPH";
    init.artifactEntry = entry(unit.roleId, "INIT", 0);
    init.inputSlots = {makeIndexSlot("input_ids", "EPOCH_INPUT", "int64"),
                       makeIndexSlot("attention_mask", "EPOCH_INPUT", "int64")};
    // The export derives positions from the input length and casts the mask to
    // int32 before attention, so the produced side inputs carry those types.
    //
    // INIT also owns the embedding: the first attention entry must read a
    // residual X0 that some step actually produced, not the request input.
    // Reading EPOCH_INPUT there would skip the embedding lookup entirely, which
    // the frozen export performs as a Gather over the shared table.
    init.outputSlots = {makeIndexSlot("position_ids", "LOCAL_STEP", "int64"),
                        makeIndexSlot("attn_mask", "LOCAL_STEP", "int32"),
                        makeSlot("x0", "LOCAL_STEP", {}, shape.hidden)};
    plan.steps.push_back(std::move(init));

    for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
      // Attention partial on this rank. Layer 0 reads the epoch input; later
      // layers read the MLP sum of the previous layer.
      ndnsf::di::NativeExecutionStepSpec attn;
      attn.stepId = attnStep(unit.roleId, layer);
      attn.unitId = unit.roleId;
      attn.operation = "CALL_SUBGRAPH";
      attn.artifactEntry = entry(unit.roleId, "ATTN", layer);
      // Layer 0 reads the embedded X0 from INIT; every later layer reads the
      // previous layer's MLP sum, which already includes both residuals. Neither
      // comes from the peer, so no network edge is created for it.
      attn.inputSlots = {layer == 0
        ? makeFedSlot("x", "LOCAL_STEP", initStep(unit.roleId), "x0", shape.hidden)
        : makeFedSlot("x", "LOCAL_STEP", sumMlpStep(unit.roleId, layer - 1), "merged", shape.hidden)};
      // Both side inputs come from this unit's INIT, never from the peer: every
      // rank owns the same positions and mask, so fetching them would be a
      // pointless network dependency.
      attn.inputSlots.push_back(
        makeFedIndexSlot("position_ids", "LOCAL_STEP", "int64", initStep(unit.roleId), "position_ids"));
      attn.inputSlots.push_back(
        makeFedIndexSlot("attn_mask", "LOCAL_STEP", "int32", initStep(unit.roleId), "attn_mask"));
      // Two outputs with different roles. `partial` is this rank's reduction
      // contribution and travels to both SUM steps. `residual` is the local X
      // the MLP entry still needs: it is deliberately a named slot so the
      // consumer is explicit, and it must never be added into any rank's
      // contribution set, otherwise every rank would add X once and the SUM
      // would produce degree times the residual.
      attn.outputSlots = {makeSlot("partial", "LOCAL_STEP", {}, shape.hidden),
                          makeSlot("residual", "LOCAL_STEP", {}, shape.hidden)};
      attn.localStateSlots = {"kv." + std::to_string(layer) + ".key",
                              "kv." + std::to_string(layer) + ".value"};
      plan.steps.push_back(std::move(attn));

      // SUM over this rank's partial and every peer's partial. The contributor
      // set comes from the degree, never from a hard-coded pair.
      ndnsf::di::NativeExecutionStepSpec sumAttn;
      sumAttn.stepId = sumAttnStep(unit.roleId, layer);
      sumAttn.unitId = unit.roleId;
      sumAttn.operation = "SUM";
      sumAttn.inputSlots.push_back(
        makeFedSlot("local", "LOCAL_STEP", attnStep(unit.roleId, layer), "partial", shape.hidden));
      std::size_t peerIndex = 0;
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) continue;
        sumAttn.inputSlots.push_back(
          makeFedSlot("remote" + std::to_string(peerIndex++), "REMOTE_STEP",
                      attnStep(peer.roleId, layer), "partial", shape.hidden));
      }
      sumAttn.outputSlots = {makeSlot("merged", "LOCAL_STEP", {}, shape.hidden)};
      plan.steps.push_back(std::move(sumAttn));

      ndnsf::di::NativeExecutionStepSpec mlp;
      mlp.stepId = mlpStep(unit.roleId, layer);
      mlp.unitId = unit.roleId;
      mlp.operation = "CALL_SUBGRAPH";
      mlp.artifactEntry = entry(unit.roleId, "MLP", layer);
      // The entry performs the original skip/norm in-graph, so it needs both the
      // merged attention partial and this rank's residual, and it keeps the
      // post-attention residual X' for the next layer and the head.
      mlp.inputSlots = {makeFedSlot("merged", "LOCAL_STEP",
                                 sumAttnStep(unit.roleId, layer), "merged", shape.hidden),
                        makeFedSlot("residual", "LOCAL_STEP",
                                 attnStep(unit.roleId, layer), "residual", shape.hidden)};
      // The produced residual is a different value from the consumed one, so it
      // needs its own name: within one step an input and an output may not share
      // a slot id, and reusing "residual" here would hide that they are the
      // pre- and post-MLP activations.
      mlp.outputSlots = {makeSlot("partial", "LOCAL_STEP", {}, shape.hidden),
                         makeSlot("post_residual", "LOCAL_STEP", {}, shape.hidden)};
      plan.steps.push_back(std::move(mlp));

      ndnsf::di::NativeExecutionStepSpec sumMlp;
      sumMlp.stepId = sumMlpStep(unit.roleId, layer);
      sumMlp.unitId = unit.roleId;
      sumMlp.operation = "SUM";
      sumMlp.inputSlots.push_back(
        makeFedSlot("local", "LOCAL_STEP", mlpStep(unit.roleId, layer), "partial", shape.hidden));
      peerIndex = 0;
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) continue;
        sumMlp.inputSlots.push_back(
          makeFedSlot("remote" + std::to_string(peerIndex++), "REMOTE_STEP",
                      mlpStep(peer.roleId, layer), "partial", shape.hidden));
      }
      sumMlp.outputSlots = {makeSlot("merged", "LOCAL_STEP", {}, shape.hidden)};
      plan.steps.push_back(std::move(sumMlp));
    }

    // Only the egress unit owns the complete head; every rank still keeps its
    // own KV slices and receipts.
    if (unit.roleId == egressRole) {
      ndnsf::di::NativeExecutionStepSpec head;
      head.stepId = unit.roleId + "/HEAD";
      head.unitId = unit.roleId;
      head.operation = "CALL_SUBGRAPH";
      head.artifactEntry = entry(unit.roleId, "HEAD", shape.layers);
      head.inputSlots = {makeFedSlot("merged", "LOCAL_STEP",
                                  sumMlpStep(unit.roleId, shape.layers - 1),"merged",
                                  shape.hidden),
                         makeFedSlot("post_residual", "LOCAL_STEP",
                                  mlpStep(unit.roleId, shape.layers - 1), "post_residual",
                                  shape.hidden)};
      // Logits are vocabulary-wide, not hidden-wide: the head output slot must
      // describe the real width or a consumer would size its read buffer from
      // the residual stream.
      head.outputSlots = {makeSlot("logits", "LOCAL_STEP", {}, shape.vocab)};
      plan.steps.push_back(std::move(head));
    }
  }

  // One dependency per remote contribution, bound to the exact producing and
  // consuming step/slot. The identity reuses keyScope and stays unique.
  for (const auto& unit : tp.units) {
    for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) continue;
        for (const auto* kind : {"ATTN", "MLP"}) {
          const auto producerStep = std::string(peer.roleId) + "/" + kind + "/" +
                                    std::to_string(layer);
          const auto consumerStep = std::string(unit.roleId) + "/SUM_" + kind + "/" +
                                    std::to_string(layer);
          std::string consumerSlot;
          std::size_t peerIndex = 0;
          for (const auto& candidate : tp.units) {
            if (candidate.rank == unit.rank) continue;
            if (candidate.rank == peer.rank) {
              consumerSlot = "remote" + std::to_string(peerIndex);
              break;
            }
            ++peerIndex;
          }
          const bool attn = std::string(kind) == "ATTN";
          ndnsf::di::NativeDependencySpec dependency;
          dependency.producers = {peer.roleId};
          dependency.consumers = {unit.roleId};
          dependency.keyScope = std::string("tp-") + (attn ? "attn" : "mlp") + "-" +
                                std::to_string(layer) + "-r" + std::to_string(unit.rank) +
                                "-from-r" + std::to_string(peer.rank);
          dependency.dependencyId = dependency.keyScope;
          dependency.topicPrefix = "/NDNSF/DI/QWEN";
          dependency.objectNameTemplate =
            "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
          dependency.tensors = {"partial"};
          dependency.operationKind = "TENSOR_PARTIAL";
          dependency.useNdnsfDataV1 = true;
          dependency.producerStepId = producerStep;
          dependency.producerSlotId = "partial";
          dependency.consumerStepId = consumerStep;
          dependency.consumerSlotId = consumerSlot;
          plan.dependencies.push_back(std::move(dependency));
        }
      }
    }
  }
  plan.validateSteps();
}

} // namespace

std::vector<QwenTpEntryNodes>
classifyQwenTpEntryNodes(const onnx::ModelProto& model, std::uint64_t layers)
{
  const auto& graph = model.graph();
  std::vector<std::string> names;
  names.reserve(graph.node_size());
  for (const auto& node : graph.node()) names.push_back(node.name());
  auto entries = classifyQwenTpEntryNodes(names, layers);

  // A node with no layer and only initializer inputs is constant-derived. Naming
  // puts it in INIT, but its value is really a weight rearrangement that belongs
  // wherever its consumer is: the frozen export's single such node transposes the
  // tied embedding table for lm_head, so INIT would carry a piece of the egress
  // computation and HEAD would be missing an input.
  std::set<std::string> initializerNames;
  for (int i = 0; i < graph.initializer_size(); ++i)
    initializerNames.insert(graph.initializer(i).name());
  std::vector<bool> relocated(names.size(), false);
  for (int i = 0; i < graph.node_size(); ++i) {
    const auto& node = graph.node(i);
    // Layer nodes keep their place; only the unclassified ones are candidates.
    if (node.name().find("layers.") != std::string::npos) continue;
    if (node.input().empty()) continue;
    bool constantOnly = true;
    for (const auto& input : node.input()) {
      if (input.empty()) continue;
      if (initializerNames.count(input) == 0) {
        constantOnly = false;
        break;
      }
    }
    if (!constantOnly) continue;

    // Find every entry that consumes this node's output.
    std::set<std::size_t> consumers;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      for (const auto nodeIndex : entries[index].nodeIndices) {
        if (nodeIndex == static_cast<std::uint64_t>(i)) continue;
        for (const auto& input : graph.node(static_cast<int>(nodeIndex)).input()) {
          for (const auto& output : node.output()) {
            if (input == output) consumers.insert(index);
          }
        }
      }
    }
    if (consumers.size() != 1) {
      // Zero consumers means the node is dead; more than one would need the same
      // value in two entries, which a per-entry file cannot express.
      continue;
    }
    // Remove first, then insert: doing it the other way round would delete the
    // node from the entry it was just added to, silently dropping it from the
    // graph -- which the coverage check below then reports as a lost node.
    for (auto& entry : entries) {
      auto& nodes = entry.nodeIndices;
      nodes.erase(std::remove(nodes.begin(), nodes.end(),
                              static_cast<std::uint64_t>(i)), nodes.end());
    }
    auto& targetNodes = entries[*consumers.begin()].nodeIndices;
    targetNodes.push_back(static_cast<std::uint64_t>(i));
    std::sort(targetNodes.begin(), targetNodes.end());
    relocated[i] = true;
  }
  // Coverage is invariant: relocation moves a node, never drops one.
  std::size_t covered = 0;
  for (const auto& entry : entries) covered += entry.nodeIndices.size();
  if (covered != names.size()) {
    throw std::invalid_argument(
      "tensor-parallel entry relocation lost a graph node");
  }
  for (const auto& entry : entries) {
    if (entry.nodeIndices.empty()) {
      throw std::invalid_argument(
        "tensor-parallel entry relocation emptied an entry");
    }
  }
  return entries;
}

onnx::ModelProto
extractQwenTpEntryModel(onnx::ModelProto& source, const QwenTpEntryNodes& entry,
                        const std::string& entryId)
{
  if (entryId.empty() || entry.nodeIndices.empty()) {
    throw std::invalid_argument("tensor-parallel entry extraction needs an id and nodes");
  }
  const auto& graph = source.graph();
  const auto nodeCount = static_cast<std::uint64_t>(graph.node_size());
  if (*std::max_element(entry.nodeIndices.begin(), entry.nodeIndices.end()) >= nodeCount) {
    throw std::invalid_argument(
      "tensor-parallel entry names a node outside the source graph");
  }

  // Membership first: a boundary is defined by what the entry does not contain.
  std::set<std::uint64_t> owned(entry.nodeIndices.begin(), entry.nodeIndices.end());
  // Every tensor name the entry mentions, so initializers and value_info that
  // its nodes need travel with it.
  std::set<std::string> referenced;
  for (const auto index : owned) {
    const auto& node = graph.node(static_cast<int>(index));
    for (const auto& input : node.input())
      if (!input.empty()) referenced.insert(input);
    for (const auto& output : node.output())
      if (!output.empty()) referenced.insert(output);
  }
  std::set<std::string> initializerNames;
  for (int i = 0; i < graph.initializer_size(); ++i)
    initializerNames.insert(graph.initializer(i).name());
  // Names *consumed* outside this entry. An output boundary is exactly this: a
  // tensor the entry produces that something else reads. Collecting the outside
  // nodes' outputs instead would describe the wrong direction and leave every
  // non-terminal entry with no declared outputs at all.
  std::set<std::string> consumedOutside;
  for (std::uint64_t index = 0; index < nodeCount; ++index) {
    if (owned.count(index) != 0) continue;
    for (const auto& input : graph.node(static_cast<int>(index)).input())
      if (!input.empty()) consumedOutside.insert(input);
  }

  onnx::ModelProto result;
  result.set_ir_version(source.ir_version());
  result.set_producer_name("ndnsf-di-tp-entry");
  for (const auto& opset : source.opset_import())
    *result.add_opset_import() = opset;
  auto* out = result.mutable_graph();
  out->set_name("TP entry " + entryId);

  // Nodes stay in source order so the extracted graph is deterministic.
  for (const auto index : owned)
    *out->add_node() = graph.node(static_cast<int>(index));

  // An initializer the entry references is shared, immutable data: move it
  // rather than copy it, because the frozen export's initializer set is
  // gigabytes and a per-entry copy is exactly the blowup the rules forbid.
  std::set<std::string> moved;
  for (int i = 0; i < graph.initializer_size(); ++i) {
    const auto& name = graph.initializer(i).name();
    if (referenced.count(name) == 0) continue;
    auto* target = out->add_initializer();
    target->Swap(source.mutable_graph()->mutable_initializer(i));
    moved.insert(name);
  }
  for (const auto& value : graph.value_info())
    if (referenced.count(value.name()) != 0)
      *out->add_value_info() = value;

  // Boundary tensors: referenced, not produced inside, and not an initializer of
  // this entry. The type comes from the source's value_info or graph input.
  // A tensor's type may be declared as a graph input, a graph output, or a
  // value_info entry. The frozen export declares the KV state only as outputs,
  // so omitting that source would make the attention entry look type-less.
  const auto findValue = [&graph] (const std::string& name) -> const onnx::ValueInfoProto* {
    for (const auto& input : graph.input())
      if (input.name() == name) return &input;
    for (const auto& output : graph.output())
      if (output.name() == name) return &output;
    for (const auto& value : graph.value_info())
      if (value.name() == name) return &value;
    return nullptr;
  };
  for (const auto& name : referenced) {
    const bool producedInside = [&] {
      for (const auto index : owned)
        for (const auto& output : graph.node(static_cast<int>(index)).output())
          if (output == name) return true;
      return false;
    }();
    if (producedInside) continue;
    if (moved.count(name) != 0) continue;
    if (initializerNames.count(name) != 0) continue;
    const auto* value = findValue(name);
    if (value == nullptr) {
      // Without a declared type ORT cannot load the subgraph, and inventing one
      // would decide the entry's interface from a guess.
      throw std::invalid_argument(
        "tensor-parallel entry input has no declared type in the source: " + name);
    }
    *out->add_input() = *value;
    if (out->input_size() > 4096) {
      throw std::invalid_argument(
        "tensor-parallel entry has an implausible number of boundary inputs");
    }
  }

  // Outputs: referenced, produced inside, and consumed outside this entry.
  for (const auto& name : referenced) {
    bool producedInside = false;
    for (const auto index : owned)
      for (const auto& output : graph.node(static_cast<int>(index)).output())
        if (output == name) producedInside = true;
    if (!producedInside) continue;
    // A tensor consumed outside this entry, or one the source declares as a
    // model output, must be exposed; anything else internal stays internal.
    const auto declaredOutput = std::any_of(graph.output().begin(), graph.output().end(),
      [&name] (const auto& value) { return value.name() == name; });
    if (consumedOutside.count(name) == 0 && !declaredOutput) continue;
    const auto* value = findValue(name);
    if (value == nullptr) {
      throw std::invalid_argument(
        "tensor-parallel entry output has no declared type in the source: " + name);
    }
    *out->add_output() = *value;
  }
  if (out->input_size() == 0 || out->output_size() == 0) {
    throw std::invalid_argument(
      "tensor-parallel entry has no boundary, so it cannot be a separate graph");
  }
  for (const auto& function : source.functions())
    *result.add_functions() = function;
  return result;
}

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers)
{
  if (root.empty() || unitId.empty() || graphBytes.empty() || layers == 0) {
    throw std::invalid_argument("tensor-parallel entry assembly needs a root, unit, graph and layers");
  }
  // The source is parsed once per stack. Extraction consumes the model, and
  // re-parsing the frozen export for every entry would be ~59 full protobuf
  // builds of a 918 MB initializer set.
  onnx::ModelProto probe;
  if (!probe.ParseFromString(std::string(graphBytes.begin(), graphBytes.end()))) {
    throw std::runtime_error("tensor-parallel entry assembly could not parse the source graph");
  }
  // Graph-aware classification, so a constant-derived node follows its consumer
  // instead of being filed under INIT by name.
  const auto classified = classifyQwenTpEntryNodes(probe, layers);

  std::error_code directoryError;
  std::filesystem::create_directories(root, directoryError);
  if (directoryError) {
    throw std::runtime_error(
      "tensor-parallel entry root is unavailable: " + directoryError.message());
  }

  std::vector<QwenTpEntryContract> table;
  table.reserve(classified.size());
  // Extraction moves initializers out of the source, so each entry needs its own
  // parsed copy. The copy shares no bytes with the caller's buffer, which also
  // means a failure half-way cannot leave the caller's graph partially consumed.
  for (const auto& entry : classified) {
    onnx::ModelProto source;
    if (!source.ParseFromString(std::string(graphBytes.begin(), graphBytes.end()))) {
      throw std::runtime_error(
        "tensor-parallel entry assembly could not re-parse the source graph");
    }
    const auto fileName = sanitizeEntryFileName(unitId) + "-" +
      sanitizeEntryFileName(entry.kind) + "-" + std::to_string(entry.layer) + ".onnx";
    table.push_back(writeQwenTpEntry(root, unitId, entry, source, fileName));
  }
  if (table.empty()) {
    throw std::runtime_error("tensor-parallel entry assembly produced no entries");
  }
  // Validate the finished table as a whole: each contract already validated
  // itself, but a duplicate id or a disagreeing unit could still slip through.
  std::set<std::string> ids;
  for (const auto& entry : table) {
    if (entry.unitId != unitId) {
      throw std::logic_error("tensor-parallel entry assembly mixed units");
    }
    if (!ids.insert(entry.entryId).second) {
      throw std::logic_error("tensor-parallel entry assembly repeated an entry id");
    }
  }
  return table;
}

void
configureQwenTpRankSpec(NativeModelRunnerSpec& spec, std::uint64_t layers)
{
  if (spec.path.empty() || layers == 0 || spec.role.empty()) {
    throw std::invalid_argument(
      "tensor-parallel rank spec needs an assembled path, a role and layers");
  }
  const std::filesystem::path modelPath(spec.path);
  if (!std::filesystem::is_regular_file(modelPath)) {
    throw std::runtime_error(
      "tensor-parallel rank spec names an unreadable model: " + spec.path);
  }
  const auto graphBytes = readEntryFile(modelPath);
  auto table = assembleQwenTpEntryTable(modelPath.parent_path(), spec.role,
                                        graphBytes, layers);
  spec.metadata["tensorParallelEntries"] = qwenTpEntryTableJson(table);
  // The parent locates its children relative to this directory.
  spec.metadata["tensorParallelEntryRoot"] = modelPath.parent_path().string();
  spec.metadata["tensorParallelEntryCount"] = std::to_string(table.size());
  // The parent runner is selected by backend name, and the single-file rank model
  // is no longer what executes.
  spec.backend = "native-qwen-tensor";
}

std::string
qwenTpEntryTableJson(const std::vector<QwenTpEntryContract>& entries)
{
  if (entries.empty()) {
    throw std::invalid_argument("tensor-parallel entry table must not be empty");
  }
  NativeJson table = NativeJson::array();
  for (const auto& entry : entries) {
    entry.validate();
    if (!entry.hasGraphFile()) {
      throw std::invalid_argument(
        "tensor-parallel entry table requires an assembled graph for: " +
        entry.entryId);
    }
    NativeJson item = NativeJson::object();
    item["entryId"] = entry.entryId;
    item["unitId"] = entry.unitId;
    item["kind"] = entry.kind;
    item["layer"] = entry.layer;
    item["relativeGraphPath"] = entry.relativeGraphPath;
    item["graphDigest"] = entry.graphDigest;
    item["inputNames"] = NativeJson(entry.inputNames);
    item["outputNames"] = NativeJson(entry.outputNames);
    table.push_back(std::move(item));
  }
  return table.dump();
}

std::vector<QwenTpEntryContract>
parseQwenTpEntryTable(const std::string& json)
{
  if (json.empty()) {
    throw std::invalid_argument("tensor-parallel entry table is empty");
  }
  const auto parsed = nativeParseJson(json);
  if (!parsed.is_array() || parsed.empty()) {
    throw std::invalid_argument("tensor-parallel entry table is not a non-empty array");
  }
  std::vector<QwenTpEntryContract> entries;
  entries.reserve(parsed.size());
  std::set<std::string> ids;
  for (const auto& item : parsed) {
    QwenTpEntryContract entry;
    // Read every field the writer emits; a missing one is a corrupted table, not
    // a default.
    entry.entryId = item.at("entryId").get<std::string>();
    entry.unitId = item.at("unitId").get<std::string>();
    entry.kind = item.at("kind").get<std::string>();
    entry.layer = item.at("layer").get<std::uint64_t>();
    entry.relativeGraphPath = item.at("relativeGraphPath").get<std::string>();
    entry.graphDigest = item.at("graphDigest").get<std::string>();
    for (const auto& name : item.at("inputNames"))
      entry.inputNames.push_back(name.get<std::string>());
    for (const auto& name : item.at("outputNames"))
      entry.outputNames.push_back(name.get<std::string>());
    entry.validate();
    if (!ids.insert(entry.entryId).second) {
      throw std::invalid_argument(
        "tensor-parallel entry table repeats an entry id: " + entry.entryId);
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

QwenTpEntryContract
writeQwenTpEntry(const std::filesystem::path& root, const std::string& unitId,
                 const QwenTpEntryNodes& entry, onnx::ModelProto& source,
                 const std::string& graphName)
{
  if (root.empty() || unitId.empty()) {
    throw std::invalid_argument("tensor-parallel entry write needs a root and a unit");
  }
  const std::filesystem::path name(graphName);
  if (graphName.empty() || name.is_absolute() || name.has_parent_path() ||
      name.has_root_path()) {
    throw std::invalid_argument(
      "tensor-parallel entry file name must be a plain name: " + graphName);
  }
  const auto entryId = unitId + "/" + entry.kind + "/" + std::to_string(entry.layer);
  auto model = extractQwenTpEntryModel(source, entry, entryId);

  // Boundary names come from the extracted graph itself, in its own declaration
  // order, so a consumer and the file cannot disagree about the interface.
  QwenTpEntryContract contract;
  contract.entryId = entryId;
  contract.unitId = unitId;
  contract.kind = entry.kind;
  contract.layer = entry.layer;
  for (const auto& input : model.graph().input()) contract.inputNames.push_back(input.name());
  for (const auto& output : model.graph().output()) contract.outputNames.push_back(output.name());

  std::string wire;
  if (!model.SerializeToString(&wire) || wire.empty()) {
    throw std::runtime_error("tensor-parallel entry graph failed to serialize");
  }
  const std::vector<std::uint8_t> bytes(wire.begin(), wire.end());

  // Verify after writing, not before: the digest must describe the file that is
  // actually there, or a truncated write would still match its contract.
  const auto target = root / name;
  {
    std::error_code directoryError;
    std::filesystem::create_directories(target.parent_path(), directoryError);
    if (directoryError) {
      throw std::runtime_error(
        "tensor-parallel entry directory is unavailable: " + directoryError.message());
    }
    const auto staging = target.string() + ".staging";
    {
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      if (!out) {
        throw std::runtime_error("tensor-parallel entry file is not writable: " +
                                 staging);
      }
      out.write(wire.data(), static_cast<std::streamsize>(wire.size()));
      out.flush();
      if (!out) {
        throw std::runtime_error("tensor-parallel entry write failed: " + staging);
      }
    }
    std::error_code renameError;
    std::filesystem::rename(staging, target, renameError);
    if (renameError) {
      std::error_code cleanupError;
      std::filesystem::remove(staging, cleanupError);
      throw std::runtime_error("tensor-parallel entry rename failed: " +
                               renameError.message());
    }
  }
  const auto written = readEntryFile(target);
  if (written.size() != bytes.size() ||
      !std::equal(written.begin(), written.end(), bytes.begin())) {
    throw std::runtime_error(
      "tensor-parallel entry file does not match the serialized graph: " +
      target.string());
  }
  contract.relativeGraphPath = name.generic_string();
  contract.graphDigest = sha256Hex(written);
  contract.validate();
  return contract;
}

std::vector<QwenTpEntryNodes>
classifyQwenTpEntryNodes(const std::vector<std::string>& nodeNames,
                         std::uint64_t layers)
{
  if (nodeNames.empty() || layers == 0) {
    throw std::invalid_argument("tensor-parallel entry classification needs a graph");
  }
  // Layer indices run [0, layers); the export also carries a final
  // normalization under layers.<layers>, which belongs to HEAD rather than to a
  // transformer layer.
  std::vector<QwenTpEntryNodes> entries;
  QwenTpEntryNodes init;
  init.kind = "INIT";
  QwenTpEntryNodes head;
  head.kind = "HEAD";
  std::vector<std::pair<QwenTpEntryNodes, QwenTpEntryNodes>> perLayer(layers);
  for (auto& pair : perLayer) {
    pair.first.kind = "ATTN";
    pair.second.kind = "MLP";
  }

  const auto layerOf = [layers] (const std::string& name) -> std::optional<std::uint64_t> {
    const auto marker = name.find("layers.");
    if (marker == std::string::npos) return std::nullopt;
    const auto begin = marker + 7;
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
      value = std::stoull(name.substr(begin), &consumed);
    }
    catch (const std::exception&) {
      return std::nullopt;
    }
    if (consumed == 0) return std::nullopt;
    // The digit run must end at the path separator, or "layers.1" would match
    // "layers.10".
    if (begin + consumed < name.size() && name[begin + consumed] != '/') return std::nullopt;
    if (value >= layers + 1) return std::nullopt;
    return static_cast<std::uint64_t>(value);
  };

  for (std::size_t index = 0; index < nodeNames.size(); ++index) {
    const auto& name = nodeNames[index];
    const auto nodeIndex = static_cast<std::uint64_t>(index);
    const auto layer = layerOf(name);
    if (!layer) {
      // Everything outside a layer is either embedding/position/mask
      // preprocessing (INIT) or the tied lm_head.
      if (name.find("lm_head") != std::string::npos) head.nodeIndices.push_back(nodeIndex);
      else init.nodeIndices.push_back(nodeIndex);
      continue;
    }
    if (*layer == layers) {
      // layers.<N> is the final normalization, which HEAD consumes.
      head.nodeIndices.push_back(nodeIndex);
      continue;
    }
    auto& attention = perLayer[*layer].first;
    auto& mlp = perLayer[*layer].second;
    attention.layer = *layer;
    mlp.layer = *layer;
    // The split inside a layer is the post-attention normalization: everything
    // before it produces the attention partial, everything from it onward is the
    // MLP entry. Using the node name keeps this independent of ONNX ordinals.
    if (name.find("post_attention_layernorm") != std::string::npos) {
      mlp.nodeIndices.push_back(nodeIndex);
      // The normalization itself belongs to the MLP entry, and so does every
      // later node of the layer.
      for (std::size_t later = index + 1; later < nodeNames.size(); ++later) {
        if (layerOf(nodeNames[later]) != *layer) break;
        mlp.nodeIndices.push_back(static_cast<std::uint64_t>(later));
        index = later;
      }
      continue;
    }
    attention.nodeIndices.push_back(nodeIndex);
  }

  if (init.nodeIndices.empty()) {
    throw std::invalid_argument(
      "tensor-parallel entry classification found no INIT nodes");
  }
  for (const auto& pair : perLayer) {
    if (pair.first.nodeIndices.empty() || pair.second.nodeIndices.empty()) {
      throw std::invalid_argument(
        "tensor-parallel entry classification left a layer without attention or MLP nodes");
    }
  }
  entries.push_back(std::move(init));
  for (auto& pair : perLayer) {
    entries.push_back(std::move(pair.first));
    entries.push_back(std::move(pair.second));
  }
  if (!head.nodeIndices.empty()) entries.push_back(std::move(head));
  // The taxonomy must cover the graph exactly: a node in no entry would never be
  // executed, which reads as a silently missing computation.
  std::size_t covered = 0;
  for (const auto& entry : entries) covered += entry.nodeIndices.size();
  if (covered != nodeNames.size()) {
    throw std::invalid_argument(
      "tensor-parallel entry classification does not cover every graph node");
  }
  return entries;
}

void
QwenTpEntryContract::validate() const
{
  if (entryId.empty() || unitId.empty() || inputNames.empty() ||
      outputNames.empty() || kind.empty()) {
    throw std::invalid_argument("tensor-parallel entry contract is incomplete");
  }
  if (kind != "INIT" && kind != "HEAD" && kind != "ATTN" && kind != "MLP") {
    throw std::invalid_argument(
      "tensor-parallel entry contract has an unknown kind: " + kind);
  }
  // A file reference is all-or-nothing: a path without a digest would let a
  // cached entry be trusted by name alone.
  if (relativeGraphPath.empty() != graphDigest.empty()) {
    throw std::invalid_argument(
      "tensor-parallel entry contract has a partial file reference");
  }
  if (!relativeGraphPath.empty()) {
    const std::filesystem::path relative(relativeGraphPath);
    if (relative.is_absolute()) {
      throw std::invalid_argument(
        "tensor-parallel entry graph path must be relative to its assembly root");
    }
    for (const auto& part : relative) {
      if (part == "..") {
        throw std::invalid_argument(
          "tensor-parallel entry graph path must not escape its assembly root");
      }
    }
    if (graphDigest.size() != 71 || graphDigest.compare(0, 7, "sha256:") != 0) {
      throw std::invalid_argument(
        "tensor-parallel entry graph digest is not a canonical sha256");
    }
  }
}

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units)
{
  const auto tp = buildQwenTpTemplate(shape, degree, entryPrefix, units);
  projectTemplate(plan, shape, degree, entryPrefix, egressRole, tp, nullptr);
}

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units,
                  const std::vector<QwenTpEntryContract>& entries)
{
  // A distinct placeholder prefix: no id is derived from it, because every entry
  // must come from a verified contract.
  auto tp = buildQwenTpTemplate(shape, degree, "/verified", units);
  tp.entries = entries;
  for (const auto& contract : tp.entries) contract.validate();
  std::set<std::string> ids;
  for (const auto& contract : tp.entries)
    if (!ids.insert(contract.entryId).second)
      throw std::invalid_argument("tensor-parallel entry ids are duplicated");
  projectTemplate(plan, shape, degree, "/verified", egressRole, tp, &tp.entries);
}

} // namespace ndnsf::di::qwen
