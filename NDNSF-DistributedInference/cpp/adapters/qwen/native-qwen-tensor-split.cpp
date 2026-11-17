#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-tensor-type.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"

#include <onnx/onnx_pb.h>

#include <ndn-cxx/util/sha256.hpp>

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di::qwen {

namespace {

QwenTpShape
deriveQwenTpRegionShape(const std::vector<std::uint8_t>& modelBytes,
                        std::uint64_t layerBegin, std::uint64_t layerEnd);

void
retainQwenTpEgressHead(std::vector<QwenTpEntryContract>& table,
                       const std::string& unitId, const std::string& egressRole);

std::vector<QwenTpEntryNodes>
classifyQwenRegionNodes(const std::vector<std::string>& nodeNames, std::uint64_t layers,
                       std::uint64_t layerBegin, std::uint64_t layerEnd);

bool
ownsStateInput(const QwenTpEntryContract& entry, const std::string& name);

bool
ownsRegionEntry(const QwenTpEntryNodes& entry, std::uint64_t layers,
                std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  if (entry.kind == "ATTN" || entry.kind == "MLP")
    return entry.layer >= layerBegin && entry.layer < layerEnd;
  if (entry.kind == "HEAD") return layerEnd == layers;
  return layerBegin == 0;
}

std::vector<QwenTpEntryNodes>
classifyQwenRegionNodes(const onnx::ModelProto& model, std::uint64_t layers,
                       std::uint64_t layerBegin, std::uint64_t layerEnd);

void
requireRankAssemblyActive(const QwenTpAssemblyControl* control)
{
  if (control == nullptr) return;
  if (control->requireActive) control->requireActive();
  if (std::chrono::steady_clock::now() >= control->deadline) {
    throw std::runtime_error("tensor-parallel rank assembly deadline expired");
  }
}

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
                     std::uint64_t rank, std::uint64_t layerBegin,
                     std::uint64_t layerEnd)
{
  shape.validate();
  if (degree < 2) {
    throw std::invalid_argument("tensor-parallel planning requires degree >= 2");
  }
  if (rank >= degree) {
    throw std::invalid_argument("tensor-parallel rank is outside its degree");
  }
  // A unit owns a contiguous region of the decoder stack. The range is explicit rather
  // than implied by the shape so that one stage's ranks slice only their own layers.
  if (layerEnd > shape.layers) {
    throw std::invalid_argument("tensor-parallel layer range exceeds the model");
  }
  if (layerBegin >= layerEnd) {
    throw std::invalid_argument("tensor-parallel layer range is empty");
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
  result.reserve(layerEnd - layerBegin);
  for (std::uint64_t layer = layerBegin; layer < layerEnd; ++layer) {
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
  if (degree < 1) {
    throw std::invalid_argument("tensor-parallel template requires degree >= 1");
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
              std::uint64_t layers, std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  if (layerEnd > layers || layerBegin >= layerEnd) {
    throw std::invalid_argument("tensor-parallel unit range is empty or outside the model");
  }
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
    // Every unit of one template owns the same window; a rank holds a shard of its
    // stage's layers rather than of every layer in the model.
    if (unit.layerBegin != layerBegin || unit.layerEnd != layerEnd) {
      throw std::invalid_argument(
        "tensor-parallel unit does not cover the template's layer range");
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
  return buildQwenTpTemplate(shape, degree, entryPrefix, units, 0, shape.layers);
}

QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix,
                    const std::vector<QwenTpUnit>& units,
                    std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  validateTemplateInputs(shape, degree, entryPrefix);
  validateUnits(units, degree, shape.layers, layerBegin, layerEnd);
  QwenTpTemplate result;
  result.degree = degree;
  result.units = units;
  // Slices are indexed by rank, so the per-rank plans follow the unit ranks
  // rather than the list order.
  result.slicesByRank.resize(degree);
  for (const auto& unit : units) {
    // A single-rank stage owns whole tensors; there is no shard to plan.
    if (degree > 1)
      result.slicesByRank[unit.rank] =
        planQwenTensorSlices(shape, degree, unit.rank, layerBegin, layerEnd);
  }
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

NativeStepSlotSpec
sourceStepSlot(const NativeJson& metadata, const std::string& name,
               const std::string& slotId, const std::string& origin);

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
                const std::vector<QwenTpEntryContract>* verified,
                const QwenTpStageWindow* window = nullptr, bool sourceBindings = false,
                const NativeJson* sourcePorts = nullptr)
{
  // The egress unit owns the head, so it must be one of the declared units.
  if (std::none_of(tp.units.begin(), tp.units.end(),
                   [&egressRole] (const auto& unit) { return unit.roleId == egressRole; })) {
    throw std::invalid_argument(
      "tensor-parallel egress role is not one of the declared units");
  }
  // A stage owns a window of layers. The template's units all describe the same window,
  // so the step and dependency loops below walk that window rather than the whole model;
  // without this a windowed plan referenced ENTRIES for layers its template never sliced.
  const std::uint64_t firstLayer =
    window != nullptr ? window->layerBegin
                      : (tp.units.empty() ? 0 : tp.units.front().layerBegin);
  const std::uint64_t lastLayer =
    window != nullptr ? window->layerEnd
                      : (tp.units.empty() ? shape.layers : tp.units.front().layerEnd);
  if (window != nullptr && firstLayer > 0 &&
      (window->upstreamUnit.empty() || window->upstreamStepId.empty() ||
       window->upstreamSlotId.empty() || window->upstreamMlpStepId.empty() ||
       window->upstreamMlpSlotId.empty())) {
    throw std::invalid_argument("a later stage needs both residual and MLP input producers");
  }
  if (window != nullptr && firstLayer > 0 &&
      (window->upstreamStepId != mlpStep(window->upstreamUnit, firstLayer - 1) ||
       window->upstreamSlotId != "post_residual" ||
       window->upstreamMlpStepId != sumMlpStep(window->upstreamUnit, firstLayer - 1) ||
       window->upstreamMlpSlotId != "merged")) {
    throw std::invalid_argument("a Qwen stage boundary requires the complete previous layer");
  }
  plan.roles.clear();
  plan.steps.clear();
  plan.dependencies.clear();
  plan.stageLayout.clear();
  for (const auto& unit : tp.units) plan.roles.push_back(unit.roleId);
  if (window != nullptr) {
    // A stage window is what makes this a v2 plan: the units own part of the stack and
    // the layout says which part, so a consumer never infers it from the roles.
    plan.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2;
    ndnsf::di::NativeStageLayout layout;
    layout.stageId = window->stageId;
    layout.sourceRegionDigest = window->sourceRegionDigest;
    for (const auto& unit : tp.units) layout.unitRoleIds.push_back(unit.roleId);
    plan.stageLayout.push_back(std::move(layout));
  }
  else {
    plan.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V1;
  }
  plan.tensorDegree = degree;
  plan.modelFamily = "qwen";
  plan.plannerKind = "native-qwen-tensor";

  // Resolve one step's artifact entry. With verified contracts the id must come
  // from a contract owned by the same unit and carrying the same kind and layer;
  // inventing a name would let a plan reference an entry the assembler never
  // produced, which only fails much later inside the worker.
  const auto entry = [&] (const std::string& unit, const char* kind,
                          std::uint64_t layer) {
    if (verified == nullptr) {
      // The assembler names an entry `<unit>/<kind>/<layer>` and the prefix is the artifact
      // root, which is a directory rather than part of the id. The projected id is
      // therefore the same shape, and a caller-supplied prefix is not prepended: doing that
      // produced ids no assembled entry answers to, and the mismatch appeared only inside
      // the worker.
      return unit + "/" + kind + "/" + std::to_string(layer);
    }
    const auto matched = std::find_if(verified->begin(), verified->end(),
      [&] (const auto& contract) {
        return contract.unitId == unit && contract.kind == kind &&
               contract.layer == layer;
      });
    if (matched == verified->end()) {
      if (sourceBindings && firstLayer > 0 && std::string(kind) == "INIT")
        return std::string{};
      throw std::invalid_argument(
        "tensor-parallel step has no verified entry contract for its unit, "
        "kind and layer: " + unit + "/" + kind + "/" + std::to_string(layer));
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
    // The frozen export's INIT takes the token ids, the mask *and* the positions: the
    // reformat subgraph reads the position vector rather than deriving it from the token
    // shape, so an INIT with only the first two cannot run. All three are epoch inputs the
    // caller supplies for the prompt.
    // The export's INIT consumes the prompt's positions as well as its ids and mask, so
    // the caller supplies all three. The produced positions travel under the slot named
    // `position_ids` below, so the consumed one is named `prompt_positions`: a step's input
    // and output slots share one id space, and reusing the name would make the template
    // ambiguous by construction.
    init.inputSlots = {makeIndexSlot("input_ids", "EPOCH_INPUT", "int64"),
                       makeIndexSlot("attention_mask", "EPOCH_INPUT", "int64"),
                       makeIndexSlot("prompt_positions", "EPOCH_INPUT", "int64")};
    // The export derives positions from the input length and casts the mask to
    // int32 before attention, so the produced side inputs carry those types.
    //
    // INIT also owns the embedding: the first attention entry must read a
    // residual X0 that some step actually produced, not the request input.
    // Reading EPOCH_INPUT there would skip the embedding lookup entirely, which
    // the frozen export performs as a Gather over the shared table.
    // Three index outputs, not two. The export's reformat subgraph produces two mask
    // tensors that attention consumes separately: the additive mask it applies (rank 2)
    // and the scalar total length it reads. Both cross the INIT boundary, so both need a
    // slot -- declaring one of them `attn_mask` and leaving the other unbound is what made
    // the attention entry look like it needed a tensor nobody produced.
    // The total length is a rank-0 scalar in the export, not an index vector: declaring it
    // with the {1, sequence} index shape made its one element resolve against a symbolic
    // sequence axis, and the attention op then read the length as 0.
    auto totalLength = makeIndexSlot("total_sequence_length", "LOCAL_STEP", "int32");
    totalLength.shape.clear();
    init.outputSlots = {makeIndexSlot("position_ids", "LOCAL_STEP", "int64"),
                        makeIndexSlot("attn_mask", "LOCAL_STEP", "int32"),
                        std::move(totalLength)};
    // The embedding belongs to the stage that owns the model's first layer; a later stage
    // starts from the activation its predecessor publishes. The metadata above is computed
    // from the request, so every stage owns it locally rather than depending on a bundle
    // tensor the request may not carry.
    if (firstLayer == 0)
      init.outputSlots.push_back(makeSlot("x0", "LOCAL_STEP", {}, shape.hidden));
    if (!init.artifactEntry.empty())
      plan.steps.push_back(std::move(init));

    for (std::uint64_t layer = firstLayer; layer < lastLayer; ++layer) {
      // Attention partial on this rank. Layer 0 reads the epoch input; later
      // layers read the MLP sum of the previous layer.
      ndnsf::di::NativeExecutionStepSpec attn;
      attn.stepId = attnStep(unit.roleId, layer);
      attn.unitId = unit.roleId;
      attn.operation = "CALL_SUBGRAPH";
      attn.artifactEntry = entry(unit.roleId, "ATTN", layer);
      // Layer 0 reads the embedded X0 from INIT.
      //
      // Every later layer's input normalization is a skip normalization over *two*
      // separate values: the previous layer's post-attention residual X' and that
      // layer's MLP output. Feeding only their sum is wrong -- the export adds the MLP
      // output to X' inside the normalization, so a single pre-added tensor would be
      // added twice. The two contributions therefore stay separate slots, each bound to
      // the step that produced it on this unit; neither comes from the peer.
      // Layer 1 onwards, the skip normalization adds the previous layer's MLP projection to
      // its post-attention value. The post-attention value is that layer's `post_residual`
      // -- the residual plus the attention projection, which the MLP entry already formed --
      // not the attention reduction alone. Feeding the reduction alone would drop the
      // residual stream from every layer after the first.
      if (layer == 0) {
        attn.inputSlots = {
          makeFedSlot("x", "LOCAL_STEP", initStep(unit.roleId), "x0", shape.hidden)};
      }
      else if (layer == firstLayer) {
        attn.inputSlots = {
          makeFedSlot("post_attention", "REMOTE_STEP", window->upstreamStepId,
                      window->upstreamSlotId, shape.hidden),
          makeFedSlot("previous_mlp", "REMOTE_STEP", window->upstreamMlpStepId,
                      window->upstreamMlpSlotId, shape.hidden)};
      }
      else {
        attn.inputSlots = {
          makeFedSlot("post_attention", "LOCAL_STEP", mlpStep(unit.roleId, layer - 1),
                      "post_residual", shape.hidden)};
        attn.inputSlots.push_back(
          makeFedSlot("previous_mlp", "LOCAL_STEP", sumMlpStep(unit.roleId, layer - 1),
                      "merged", shape.hidden));
      }
      // Both side inputs come from this unit's INIT, never from the peer: every
      // rank owns the same positions and mask, so fetching them would be a
      // pointless network dependency.
      attn.inputSlots.push_back(
        makeFedIndexSlot("position_ids", "LOCAL_STEP", "int64", initStep(unit.roleId), "position_ids"));
      attn.inputSlots.push_back(
        makeFedIndexSlot("attn_mask", "LOCAL_STEP", "int32", initStep(unit.roleId), "attn_mask"));
      {
        auto totalLength = makeFedIndexSlot("total_sequence_length", "LOCAL_STEP", "int32",
                                            initStep(unit.roleId), "total_sequence_length");
        totalLength.shape.clear();
        attn.inputSlots.push_back(std::move(totalLength));
      }
      // Two outputs with different roles. `partial` is this rank's reduction
      // contribution and travels to both SUM steps. `residual` is the local X
      // the MLP entry still needs: it is deliberately a named slot so the
      // consumer is explicit, and it must never be added into any rank's
      // contribution set, otherwise every rank would add X once and the SUM
      // would produce degree times the residual.
      // Attention returns its projection and nothing else. The pre-MLP residual the MLP
      // step consumes is this step's own hidden input, so the runner passes it through under
      // the `residual` slot -- declaring it as an output would promise a tensor the graph
      // does not produce.
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
      // Contributions are listed in global rank order, not "local first". Floating-point
      // addition is not associative, so a per-rank order would make two units compute
      // different sums of the same contributions: with three ranks, unit 2 would add
      // (r2 + r0) + r1 while unit 0 adds (r0 + r1) + r2. Two terms commute, so a pair is
      // safe either way -- which is why this only shows up from degree three on.
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) {
          sumAttn.inputSlots.push_back(makeFedSlot(
            "local", "LOCAL_STEP", attnStep(unit.roleId, layer), "partial", shape.hidden));
          continue;
        }
        sumAttn.inputSlots.push_back(makeFedSlot(
          "remote" + std::to_string(peer.rank), "REMOTE_STEP",
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
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) {
          sumMlp.inputSlots.push_back(makeFedSlot(
            "local", "LOCAL_STEP", mlpStep(unit.roleId, layer), "partial", shape.hidden));
          continue;
        }
        sumMlp.inputSlots.push_back(makeFedSlot(
          "remote" + std::to_string(peer.rank), "REMOTE_STEP",
          mlpStep(peer.roleId, layer), "partial", shape.hidden));
      }
      sumMlp.outputSlots = {makeSlot("merged", "LOCAL_STEP", {}, shape.hidden)};
      plan.steps.push_back(std::move(sumMlp));
    }

    // Only the egress unit owns the complete head; every rank still keeps its
    // own KV slices and receipts.
    if (unit.roleId == egressRole && lastLayer == shape.layers) {
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

  if (sourceBindings) {
    std::set<std::string> referenced;
    for (auto& step : plan.steps) {
      if (step.operation != "CALL_SUBGRAPH") continue;
      const auto contract = std::find_if(verified->begin(), verified->end(),
        [&] (const auto& item) { return item.entryId == step.artifactEntry; });
      if (contract == verified->end() || contract->unitId != step.unitId)
        throw std::invalid_argument("material step has no matching entry: " + step.stepId);
      referenced.insert(contract->entryId);
      if (!contract->synthesizedInputGraphNames.empty())
        throw std::invalid_argument("material entry requires unbound synthesized inputs: " +
                                    contract->entryId);
      std::set<std::string> inputs;
      std::set<std::string> outputs;
      const auto collect = [&] (const auto& bindings, const auto& names, auto& slots) {
        for (const auto& [name, slot] : bindings) {
          if (std::find(names.begin(), names.end(), name) == names.end() || slot.empty())
            throw std::invalid_argument("material port binding names no graph value: " + name);
          slots.insert(slot);
        }
      };
      collect(contract->inputSlotByGraphName, contract->inputNames, inputs);
      collect(contract->outputSlotByGraphName, contract->outputNames, outputs);
      collect(contract->passthroughSlotByInputGraphName, contract->inputNames, outputs);
      if (sourcePorts != nullptr) {
        // Actual source ports precede semantic narrowing. Their producer is resolved only
        // after every region exists; no epoch input or synthesized value is invented here.
        const auto complete = [&] (auto& slots, const auto& bindings) {
          for (const auto& [name, id] : bindings) {
            if (std::none_of(slots.begin(), slots.end(),
                            [&] (const auto& slot) { return slot.slotId == id; }))
              slots.push_back(sourceStepSlot(*sourcePorts, name, id, "LOCAL_STEP"));
          }
        };
        complete(step.inputSlots, contract->inputSlotByGraphName);
        complete(step.outputSlots, contract->outputSlotByGraphName);
        complete(step.outputSlots, contract->passthroughSlotByInputGraphName);
      }
      std::set<std::string> producedSlots;
      for (const auto& [name, slot] : contract->outputSlotByGraphName) {
        if (!producedSlots.insert(slot).second)
          throw std::invalid_argument("material outputs ambiguously share a slot: " + slot);
      }
      for (const auto& name : contract->inputNames) {
        if (contract->inputSlotByGraphName.count(name) == 0 && !ownsStateInput(*contract, name))
          throw std::invalid_argument("material graph input has no slot: " + name);
      }
      for (const auto& name : contract->outputNames) {
        if (contract->outputSlotByGraphName.count(name) == 0 &&
            std::find(contract->stateOutputGraphNames.begin(),
                      contract->stateOutputGraphNames.end(), name) ==
              contract->stateOutputGraphNames.end())
          throw std::invalid_argument("material graph output has no slot or state binding: " + name);
      }
      const auto narrow = [&] (auto& slots, const auto& required) {
        for (const auto& id : required) {
          if (std::none_of(slots.begin(), slots.end(),
                          [&] (const auto& slot) { return slot.slotId == id; }))
            throw std::invalid_argument("material port has no planned slot: " + id);
        }
        slots.erase(std::remove_if(slots.begin(), slots.end(),
          [&] (const auto& slot) { return required.count(slot.slotId) == 0; }), slots.end());
      };
      narrow(step.inputSlots, inputs);
      narrow(step.outputSlots, outputs);
      if (contract->stateOutputGraphNames.empty()) step.localStateSlots.clear();
    }
    for (const auto& contract : *verified) {
      if (referenced.count(contract.entryId) == 0)
        throw std::invalid_argument("material entry is not used by its region: " +
                                    contract.entryId);
    }
  }

  // One dependency per remote contribution, bound to the exact producing and
  // consuming step/slot. The identity reuses keyScope and stays unique.
  for (const auto& unit : tp.units) {
    for (std::uint64_t layer = firstLayer; layer < lastLayer; ++layer) {
      for (const auto& peer : tp.units) {
        if (peer.rank == unit.rank) continue;
        for (const auto* kind : {"ATTN", "MLP"}) {
          const auto producerStep = std::string(peer.roleId) + "/" + kind + "/" +
                                    std::to_string(layer);
          const auto consumerStep = std::string(unit.roleId) + "/SUM_" + kind + "/" +
                                    std::to_string(layer);
          // The slot is named by the producer's own rank, which every unit derives the same
          // way, so the slot a dependency targets does not depend on which unit is asking.
          const std::string consumerSlot = "remote" + std::to_string(peer.rank);
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
  // Preserve both source graph inputs across the stage boundary. Each edge carries one
  // complete tensor, not an additive contribution or a partially reduced rank output.
  if (window != nullptr && firstLayer > 0) {
    for (const auto& unit : tp.units) {
      for (const auto* slot : {"post_attention", "previous_mlp"}) {
        const bool residual = std::string(slot) == "post_attention";
        ndnsf::di::NativeDependencySpec dependency;
        dependency.producers = {window->upstreamUnit};
        dependency.consumers = {unit.roleId};
        dependency.keyScope = "stage-input-" + window->stageId + "-" + unit.roleId + "-" + slot;
        dependency.dependencyId = dependency.keyScope;
        dependency.topicPrefix = "/NDNSF/DI/QWEN";
        dependency.objectNameTemplate =
          "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{keyScope}/{sequence}";
        dependency.tensors = {slot};
        dependency.operationKind = "ACTIVATION";
        dependency.useNdnsfDataV1 = true;
        dependency.producerStepId = residual ? window->upstreamStepId : window->upstreamMlpStepId;
        dependency.producerSlotId = residual ? window->upstreamSlotId : window->upstreamMlpSlotId;
        dependency.consumerStepId = attnStep(unit.roleId, firstLayer);
        dependency.consumerSlotId = slot;
        plan.dependencies.push_back(std::move(dependency));
      }
    }
  }
  // A later stage's input producer lives in the previous stage's plan, so the stage alone
  // cannot be validated - its remote slot would name a step this plan does not contain. The
  // merged plan is validated instead, where every producer exists.
  if (sourceBindings) {
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> producedNames;
    for (const auto& step : plan.steps) {
      if (step.operation == "SUM") {
        const auto local = std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
          [] (const auto& slot) { return slot.source == "LOCAL_STEP"; });
        if (local == step.inputSlots.end())
          throw std::invalid_argument("material reduction has no local contribution");
        producedNames[{step.stepId, "merged"}] =
          producedNames.at({local->producerStepId, local->producerSlotId});
        continue;
      }
      const auto contract = std::find_if(verified->begin(), verified->end(),
        [&] (const auto& item) { return item.entryId == step.artifactEntry; });
      for (const auto& [name, slot] : contract->outputSlotByGraphName)
        producedNames[{step.stepId, slot}].push_back(name);
      for (const auto& [name, slot] : contract->passthroughSlotByInputGraphName) {
        const auto key = std::make_pair(step.stepId, slot);
        if (producedNames.count(key) == 0) producedNames[key] = {name};
      }
    }
    for (auto& dependency : plan.dependencies) {
      dependency.objectNameTemplate =
        "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{keyScope}/{sequence}";
      const auto source = producedNames.find(
        {dependency.producerStepId, dependency.producerSlotId});
      if (source != producedNames.end()) {
        dependency.tensors = source->second;
        continue;
      }
      // The upstream stage is not in this regional plan. Its original tensor name is also
      // the source graph input declared by this consumer; never use a logical slot as a name.
      const auto consumer = std::find_if(plan.steps.begin(), plan.steps.end(),
        [&] (const auto& step) { return step.stepId == dependency.consumerStepId; });
      const auto contract = std::find_if(verified->begin(), verified->end(),
        [&] (const auto& item) { return item.entryId == consumer->artifactEntry; });
      dependency.tensors.clear();
      for (const auto& [name, slot] : contract->inputSlotByGraphName)
        if (slot == dependency.consumerSlotId) dependency.tensors.push_back(name);
      if (dependency.tensors.empty())
        throw std::invalid_argument("material dependency has no bound graph tensor");
    }
  }
  if (firstLayer == 0 && sourcePorts == nullptr)
    plan.validateSteps();
}

} // namespace

std::vector<QwenTpEntryNodes>
classifyQwenTpEntryNodes(const onnx::ModelProto& model, std::uint64_t layers)
{
  return classifyQwenRegionNodes(model, layers, 0, layers);
}

namespace {

std::vector<QwenTpEntryNodes>
classifyQwenRegionNodes(const onnx::ModelProto& model, std::uint64_t layers,
                       std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  const auto& graph = model.graph();
  std::vector<std::string> names;
  names.reserve(graph.node_size());
  for (const auto& node : graph.node()) names.push_back(node.name());
  auto entries = classifyQwenRegionNodes(names, layers, layerBegin, layerEnd);

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
  // A later region may contain only a constant-derived preprocessing node. Once
  // that node follows its consumer, there is no local INIT computation to emit.
  if (layerBegin > 0) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [] (const auto& entry) {
      return entry.kind == "INIT" && entry.nodeIndices.empty();
    }), entries.end());
  }
  for (const auto& entry : entries) {
    if (entry.nodeIndices.empty()) {
      throw std::invalid_argument(
        "tensor-parallel entry relocation emptied an entry");
    }
  }
  return entries;
}

} // namespace

std::pair<std::vector<onnx::ValueInfoProto>, std::vector<onnx::ValueInfoProto>>
describeQwenTpEntryBoundary(const onnx::ModelProto& source, const QwenTpEntryNodes& entry)
{
  if (entry.nodeIndices.empty()) {
    throw std::invalid_argument("tensor-parallel entry boundary needs nodes");
  }
  const auto& graph = source.graph();
  const auto nodeCount = static_cast<std::uint64_t>(graph.node_size());
  if (*std::max_element(entry.nodeIndices.begin(), entry.nodeIndices.end()) >= nodeCount) {
    throw std::invalid_argument(
      "tensor-parallel entry names a node outside the source graph");
  }
  // The same three sets `extractQwenTpEntryModel` uses, computed the same way: what the entry
  // mentions, what it produces, and what it consumes from outside itself.
  std::set<std::uint64_t> owned(entry.nodeIndices.begin(), entry.nodeIndices.end());
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
  std::set<std::string> produced;
  for (const auto index : owned)
    for (const auto& output : graph.node(static_cast<int>(index)).output())
      if (!output.empty()) produced.insert(output);
  std::set<std::string> consumedOutside;
  for (std::uint64_t index = 0; index < nodeCount; ++index) {
    if (owned.count(index) != 0) continue;
    for (const auto& input : graph.node(static_cast<int>(index)).input())
      if (!input.empty()) consumedOutside.insert(input);
  }
  const auto findValue = [&graph] (const std::string& name) -> const onnx::ValueInfoProto* {
    for (const auto& input : graph.input())
      if (input.name() == name) return &input;
    for (const auto& output : graph.output())
      if (output.name() == name) return &output;
    for (const auto& value : graph.value_info())
      if (value.name() == name) return &value;
    return nullptr;
  };
  std::vector<onnx::ValueInfoProto> inputs;
  for (const auto& name : referenced) {
    if (produced.count(name) != 0) continue;
    if (initializerNames.count(name) != 0) continue;
    const auto* value = findValue(name);
    if (value == nullptr) {
      throw std::invalid_argument(
        "tensor-parallel entry input has no declared type in the source: " + name);
    }
    inputs.push_back(*value);
  }
  std::vector<onnx::ValueInfoProto> outputs;
  for (const auto& name : referenced) {
    if (produced.count(name) == 0) continue;
    const auto declaredOutput = std::any_of(graph.output().begin(), graph.output().end(),
      [&name] (const auto& value) { return value.name() == name; });
    if (consumedOutside.count(name) == 0 && !declaredOutput) continue;
    const auto* value = findValue(name);
    if (value == nullptr) {
      throw std::invalid_argument(
        "tensor-parallel entry output has no declared type in the source: " + name);
    }
    outputs.push_back(*value);
  }
  if (inputs.empty() || outputs.empty()) {
    throw std::invalid_argument(
      "tensor-parallel entry has no boundary, so it cannot be a separate graph");
  }
  return {std::move(inputs), std::move(outputs)};
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
  return assembleQwenTpEntryTable(root, unitId, graphBytes, layers,
                                  QwenTpAssemblyShards{});
}

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards)
{
  return assembleQwenTpEntryTable(root, unitId, graphBytes, layers, shards, nullptr);
}

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards,
                         const QwenTpAssemblyControl* control)
{
  return assembleQwenTpEntryTable(root, unitId, graphBytes, layers, shards, control, 0, layers);
}

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards,
                         const QwenTpAssemblyControl* control,
                         std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  if (root.empty() || unitId.empty() || graphBytes.empty() || layers == 0) {
    throw std::invalid_argument("tensor-parallel entry assembly needs a root, unit, graph and layers");
  }
  if (layerEnd > layers || layerBegin >= layerEnd)
    throw std::invalid_argument("tensor-parallel stage window is outside the model");
  // The source is parsed once per stack. Extraction consumes the model, and
  // re-parsing the frozen export for every entry would be ~59 full protobuf
  // builds of a 918 MB initializer set.
  onnx::ModelProto probe;
  if (!probe.ParseFromString(std::string(graphBytes.begin(), graphBytes.end()))) {
    throw std::runtime_error("tensor-parallel entry assembly could not parse the source graph");
  }
  // Graph-aware classification, so a constant-derived node follows its consumer
  // instead of being filed under INIT by name.
  auto classified = classifyQwenRegionNodes(probe, layers, layerBegin, layerEnd);
  // Select before writing or charging the budget. A complete source is accepted too,
  // but a window never emits files or consumes another stage's assembly allowance.
  classified.erase(std::remove_if(classified.begin(), classified.end(),
    [&] (const auto& entry) {
      return !ownsRegionEntry(entry, layers, layerBegin, layerEnd);
    }), classified.end());

  std::error_code directoryError;
  std::filesystem::create_directories(root, directoryError);
  if (directoryError) {
    throw std::runtime_error(
      "tensor-parallel entry root is unavailable: " + directoryError.message());
  }

  // The budget and the deadline are the rank's, not each entry's: a caller that let every entry
  // reset them would accept a table far larger than the rank can hold, and a cancellation arriving
  // between entries would be ignored until the next one. `control` is optional so the in-process
  // tools that predate it keep working, but a Provider always supplies one.
  std::uint64_t assembledBytes = 0;
  const auto checkBudget = [&] (std::uint64_t bytes) {
    if (control == nullptr || control->maxAssembledBytes == 0) return;
    if (bytes > control->maxAssembledBytes) {
      throw std::runtime_error(
        "tensor-parallel rank exceeds its assembled-byte budget: " +
        std::to_string(bytes) + " > " + std::to_string(control->maxAssembledBytes));
    }
  };
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
    requireRankAssemblyActive(control);
    auto contract = writeQwenTpEntry(root, unitId, entry, source, fileName,
                                     shards.enabled() ? &shards : nullptr);
    // The written file is what the rank will load, so the budget is charged against its size.
    assembledBytes += readEntryFile(root / contract.relativeGraphPath).size();
    checkBudget(assembledBytes);
    table.push_back(std::move(contract));
  }
  requireRankAssemblyActive(control);
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

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards,
                         const std::string& egressRole)
{
  return assembleQwenTpEntryTable(root, unitId, graphBytes, layers, shards, nullptr,
                                  egressRole);
}

std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards,
                         const QwenTpAssemblyControl* control,
                         const std::string& egressRole)
{
  auto table = assembleQwenTpEntryTable(root, unitId, graphBytes, layers, shards, control);
  retainQwenTpEgressHead(table, unitId, egressRole);
  return table;
}

namespace {

void
retainQwenTpEgressHead(std::vector<QwenTpEntryContract>& table,
                       const std::string& unitId, const std::string& egressRole)
{
  if (egressRole.empty()) return;
  if (unitId == egressRole) {
    // The egress unit must have the head; its absence means the caller and the taxonomy
    // disagree about this model rather than that the entry is optional.
    const auto hasHead = std::any_of(table.begin(), table.end(),
      [] (const auto& entry) { return entry.kind == "HEAD"; });
    if (!hasHead) {
      throw std::invalid_argument(
        "tensor-parallel egress unit assembled no head entry");
    }
    return;
  }
  // A non-egress unit runs no head step, so holding a head entry would make its table
  // disagree with its own steps.
  table.erase(std::remove_if(table.begin(), table.end(),
    [] (const auto& entry) { return entry.kind == "HEAD"; }), table.end());
}

} // namespace

QwenTpShape
deriveQwenTpShape(const std::vector<std::uint8_t>& modelBytes, std::uint64_t layers)
{
  return deriveQwenTpRegionShape(modelBytes, 0, layers);
}

namespace {

QwenTpShape
deriveQwenTpRegionShape(const std::vector<std::uint8_t>& modelBytes,
                        std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  if (modelBytes.empty() || layerBegin >= layerEnd) {
    throw std::invalid_argument("tensor-parallel shape derivation needs a graph and layers");
  }
  onnx::ModelProto model;
  if (!model.ParseFromString(std::string(modelBytes.begin(), modelBytes.end()))) {
    throw std::runtime_error("tensor-parallel shape derivation could not parse the graph");
  }
  const auto& graph = model.graph();

  const auto attribute = [] (const onnx::NodeProto& node, const char* name)
    -> std::optional<std::int64_t> {
    for (const auto& attr : node.attribute()) {
      if (attr.name() == name && attr.has_i()) return attr.i();
    }
    return std::nullopt;
  };

  std::uint64_t queryHeads = 0;
  std::uint64_t kvHeads = 0;
  for (const auto& node : graph.node()) {
    if (node.op_type() != "GroupQueryAttention") continue;
    const auto heads = attribute(node, "num_heads");
    const auto kv = attribute(node, "kv_num_heads");
    if (!heads || !kv || *heads <= 0 || *kv <= 0) {
      throw std::invalid_argument(
        "tensor-parallel shape derivation found no head counts on the attention op");
    }
    const auto query = static_cast<std::uint64_t>(*heads);
    const auto keyValue = static_cast<std::uint64_t>(*kv);
    if (queryHeads != 0 && (queryHeads != query || kvHeads != keyValue))
      throw std::invalid_argument("tensor-parallel region has inconsistent attention head counts");
    queryHeads = query;
    kvHeads = keyValue;
  }
  if (queryHeads == 0) {
    throw std::invalid_argument(
      "tensor-parallel shape derivation found no GroupQueryAttention node");
  }

  // Projection widths give headDim, hidden and ffn without trusting a constant:
  // q_proj's N is queryHeads*headDim and o_proj's N is the hidden size. The widths
  // come from the existing INT4 inspector rather than from reading node inputs
  // here: MatMulNBits takes its packed weight as the second input, and a second
  // copy of that schema knowledge is exactly how the earlier parser bug happened.
  const auto localLayers = layerEnd - layerBegin;
  const auto nodes = inspectQwenNBitsGraph(modelBytes, localLayers);
  std::uint64_t queryWidth = 0;
  std::uint64_t outputWidth = 0;
  std::uint64_t gateWidth = 0;
  for (const auto& node : nodes) {
    const auto& name = node.nodeName;
    if (name.find("q_proj") != std::string::npos) queryWidth = node.n;
    else if (name.find("o_proj") != std::string::npos) outputWidth = node.n;
    else if (name.find("gate_proj") != std::string::npos) gateWidth = node.n;
  }
  if (queryWidth == 0 || outputWidth == 0 || gateWidth == 0) {
    throw std::invalid_argument(
      "tensor-parallel shape derivation found no sharded projection widths");
  }
  if (queryWidth % queryHeads != 0) {
    throw std::invalid_argument(
      "tensor-parallel q_proj width does not divide by its head count");
  }
  const auto headDim = queryWidth / queryHeads;

  // The tied embedding table gives the vocabulary; the head projects to it.
  std::uint64_t vocab = 0;
  for (const auto& init : graph.initializer()) {
    if (init.name().find("embed_tokens.weight") == std::string::npos) continue;
    if (init.dims_size() < 2) continue;
    vocab = static_cast<std::uint64_t>(init.dims(0));
    break;
  }
  if (vocab == 0 && layerBegin == 0) {
    throw std::invalid_argument(
      "tensor-parallel shape derivation found no embedding table");
  }

  auto shape = qwenTpShapeFromNodes(nodes, outputWidth, localLayers, queryHeads,
                                    kvHeads, headDim, gateWidth, vocab);
  // Geometry is checked against the local projection count. Slicing still uses
  // original global layer IDs, whose upper bound is the region end. A later
  // region need not contain an embedding: its vocabulary is unused for slicing.
  shape.layers = layerEnd;
  shape.validate();
  return shape;
}

} // namespace

QwenTpShape
deriveQwenTpShapeFromGraphMetadata(const std::string& graphMetadataJson,
                                   std::uint64_t layers)
{
  if (graphMetadataJson.empty() || layers == 0)
    throw std::invalid_argument(
      "tensor-parallel shape derivation needs graph metadata and layers");
  const auto metadata = nativeParseJson(graphMetadataJson);
  const auto& nodesJson = metadata.at("nodes");
  const auto& tensors = metadata.at("tensors");

  // Head counts live only as attributes on the attention op. The recorded integer
  // attributes are the export's own values, so this reads them rather than inferring.
  std::uint64_t queryHeads = 0;
  std::uint64_t kvHeads = 0;
  for (const auto& node : nodesJson) {
    if (node.value("opType", std::string{}) != "GroupQueryAttention") continue;
    const auto& attributes = node.at("integerAttributes");
    const auto heads = attributes.find("num_heads");
    const auto kv = attributes.find("kv_num_heads");
    if (heads == attributes.end() || kv == attributes.end())
      throw std::invalid_argument("tensor-parallel metadata attention head counts are missing");
    const auto headsValue = heads->get<std::int64_t>();
    const auto kvValue = kv->get<std::int64_t>();
    if (headsValue <= 0 || kvValue <= 0)
      throw std::invalid_argument("tensor-parallel metadata attention head counts are invalid");
    if (queryHeads != 0 &&
        (queryHeads != static_cast<std::uint64_t>(headsValue) ||
         kvHeads != static_cast<std::uint64_t>(kvValue)))
      throw std::invalid_argument("tensor-parallel metadata attention head counts disagree");
    queryHeads = static_cast<std::uint64_t>(headsValue);
    kvHeads = static_cast<std::uint64_t>(kvValue);
  }
  if (queryHeads == 0)
    throw std::invalid_argument(
      "tensor-parallel metadata derivation found no attention head counts");

  // N is the MatMulNBits output-channel count. Tensor inventories also contain packed
  // weights/scales whose final dimensions are storage geometry, never output widths.
  const auto widthOf = [&nodesJson] (const std::string& marker) -> std::uint64_t {
    std::uint64_t result = 0;
    for (const auto& node : nodesJson) {
      if (node.value("opType", std::string{}) != "MatMulNBits" ||
          node.at("name").get<std::string>().find(marker) == std::string::npos) continue;
      const auto width = node.at("integerAttributes").at("N").get<std::int64_t>();
      if (width <= 0 || (result != 0 && result != static_cast<std::uint64_t>(width)))
        throw std::invalid_argument("tensor-parallel metadata projection widths disagree");
      result = static_cast<std::uint64_t>(width);
    }
    return result;
  };
  const auto queryWidth = widthOf("q_proj");
  const auto outputWidth = widthOf("o_proj");
  const auto gateWidth = widthOf("gate_proj");
  if (queryWidth == 0 || outputWidth == 0 || gateWidth == 0)
    throw std::invalid_argument(
      "tensor-parallel metadata derivation found no sharded projection widths");
  if (queryWidth % queryHeads != 0)
    throw std::invalid_argument(
      "tensor-parallel metadata q_proj width does not divide by its head count");
  const auto headDim = queryWidth / queryHeads;

  // The tied embedding table gives the vocabulary; its first axis is the row count.
  std::uint64_t vocab = 0;
  for (const auto& item : tensors.items()) {
    if (item.key().find("embed_tokens.weight") == std::string::npos) continue;
    const auto& shape = item.value().at("shape");
    if (shape.empty() || !shape.front().is_number_integer()) continue;
    const auto rows = shape.front().get<std::int64_t>();
    if (rows <= 0) continue;
    vocab = static_cast<std::uint64_t>(rows);
    break;
  }
  if (vocab == 0)
    throw std::invalid_argument(
      "tensor-parallel metadata derivation found no embedding table");

  QwenTpShape shape;
  shape.hidden = outputWidth;
  shape.layers = layers;
  shape.queryHeads = queryHeads;
  shape.kvHeads = kvHeads;
  shape.headDim = headDim;
  shape.ffn = gateWidth;
  shape.vocab = vocab;
  shape.validate();
  return shape;
}

NativeSplitCandidate
buildQwenTpCandidate(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                     const std::vector<QwenTpUnit>& units, const std::string& entryPrefix,
                     const std::string& egressRole, const NativeModelDescriptor& model,
                     const std::map<std::string, std::string>& artifactByUnit,
                     const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit)
{
  if (units.size() < 2) {
    throw std::invalid_argument(
      "tensor-parallel candidate needs at least two units");
  }
  std::set<std::string> roleIds;
  for (const auto& unit : units) {
    if (unit.roleId.empty() || !roleIds.insert(unit.roleId).second) {
      throw std::invalid_argument(
        "tensor-parallel candidate units repeat or are unnamed");
    }
    if (artifactByUnit.count(unit.roleId) == 0 ||
        requirementByUnit.count(unit.roleId) == 0) {
      throw std::invalid_argument(
        "tensor-parallel candidate unit has no artifact or requirement: " + unit.roleId);
    }
  }
  if (roleIds.count(egressRole) == 0) {
    throw std::invalid_argument(
      "tensor-parallel candidate egress role is not one of its units");
  }

  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  // The strategy identity names what produced this candidate. It is part of the sealed
  // contract, so it is filled rather than left empty: an empty identity is refused at
  // validation and would make the candidate unusable.
  candidate.splitter.name = "native-qwen-tensor-parallel";
  candidate.splitter.version = "1";
  candidate.splitter.configurationDigest = nativePlanningDigest(
    std::string("qwen-tensor-parallel|1|") + std::to_string(units.size()) + "|" +
    entryPrefix + "|" + egressRole);
  candidate.model = model;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan.version = 3;
  candidate.executionPlan.modelName = model.modelName;
  candidate.executionPlan.modelFamily = "qwen";
  candidate.executionPlan.modelFormat = model.modelFormat;
  candidate.executionPlan.plannerKind = "native-qwen-tensor-parallel";
  candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
  candidate.executionPlan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V1;
  projectQwenTpPlan(candidate.executionPlan, shape, units.size(), entryPrefix,
                    egressRole, units);
  candidate.executionPlan.roles.clear();
  for (const auto& unit : units) candidate.executionPlan.roles.push_back(unit.roleId);

  // One role per unit, each executing one rank's share: no Provider hosts a fraction of
  // a role, so per-role degree stays one even though the plan's tensor degree is two.
  for (const auto& unit : units) {
    // A fragment names the recipe entry prefix this unit assembles from. Every per-role
    // map has to cover the role set exactly, so it is filled here rather than left empty.
    // The fragment is a digest, not a prefix string: validate requires `isDigest` on it.
    candidate.fragmentsByRole[unit.roleId] = nativePlanningDigest(
      entryPrefix + "|" + unit.roleId + "|" + std::to_string(unit.rank));
    candidate.tensorDegreesByRole[unit.roleId] = 1;
    // Placement degree one, shard geometry separate: one Provider hosts this unit, while
    // its recipe must still know it is rank r of a degree-N split. Without this both ranks
    // derive the same recipe digest and assemble identical weights.
    candidate.tensorIdentityByRole[unit.roleId] = {
      static_cast<std::uint64_t>(units.size()), unit.rank};
    candidate.rankArtifactDigestsByRole[unit.roleId] = {artifactByUnit.at(unit.roleId)};
    candidate.artifactsByRole[unit.roleId] = {artifactByUnit.at(unit.roleId)};
    candidate.requirementsByRole[unit.roleId] = requirementByUnit.at(unit.roleId);
  }

  // Source-graph ownership stays single-valued: every node belongs to the ingress unit,
  // because node ownership identifies which role assembles that node's source slice and
  // both units execute the same layers. The steps, not node ownership, say which unit
  // runs which subgraph.
  // Source-slice ownership stays single-valued and goes to the ingress unit, while the
  // execution relation is declared separately: every unit executes every node, because a
  // sharded rank runs the same layers with a different weight slice. Without the second
  // map the peer would own no node and role preparation would refuse it for an empty
  // source boundary.
  const std::string ingress = units.front().roleId;
  std::vector<std::string> allNodes;
  allNodes.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    candidate.nodeRoles[node.id] = ingress;
    allNodes.push_back(node.id);
  }
  for (const auto& unit : units)
    candidate.roleNodeIdsByRole[unit.roleId] = allNodes;
  candidate.inputIngressRole = ingress;
  candidate.resultEgressRole = egressRole;

  // State contracts are per unit; the projected steps already carry their own state
  // slots, so these declare the same tensors for every unit.
  for (const auto& unit : units) {
    std::vector<NativeTensorContract> stateInputs;
    std::vector<NativeTensorContract> stateOutputs;
    for (std::uint64_t layer = unit.layerBegin; layer < unit.layerEnd; ++layer) {
      const auto suffix = std::to_string(layer);
      for (const auto& kind : {"key", "value"}) {
        stateInputs.push_back({"past_key_values." + suffix + "." + kind, "uint8",
                               {"batch", "heads", "sequence", "head-dimension"},
                               std::nullopt});
        stateOutputs.push_back({"present." + suffix + "." + kind, "uint8",
                                {"batch", "heads", "sequence", "head-dimension"},
                                std::nullopt});
      }
    }
    candidate.roleStateInputsByRole[unit.roleId] = std::move(stateInputs);
    candidate.roleStateOutputsByRole[unit.roleId] = std::move(stateOutputs);
  }

  // No hybrid plan. The hybrid rule exists for a role whose execution is split across
  // Providers, and no role is: each unit is one Provider's whole share, degree one. The
  // plan's tensor degree stays two because the step template needs it, and the two
  // degrees measure different things -- how many partials a layer has, and how many
  // Providers host one role.
  //
  // A hybrid plan here would also be refused: its degree vector must equal the per-role
  // degrees, which are one each, so a plan declaring two for one stage would contradict
  // them.

  candidate.estimatedCosts = {
    {"role_count", std::uint64_t(units.size())},
    {"rank_count", std::uint64_t(units.size())},
    {"decoder_layers", std::uint64_t(shape.layers)},
  };
  candidate.candidateDigest = candidate.computedDigest();
  return candidate;
}

namespace {

bool
role_ids_contain(const std::vector<QwenTpUnit>& units, const std::string& role)
{
  return std::any_of(units.begin(), units.end(),
                     [&] (const auto& unit) { return unit.roleId == role; });
}

std::vector<std::vector<std::string>>
stageSemanticNodes(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                   const std::vector<QwenTpStageWindow>& windows)
{
  if (graph.nodes.size() < 2 || shape.layers != graph.nodes.size() - 2)
    throw std::invalid_argument("hybrid planning graph does not cover the model layers");
  std::vector<std::vector<std::string>> result(windows.size());
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    const auto layer = index == 0 ? 0 : index - 1;
    const auto expected = index == 0 ? std::string("embedding") :
      index + 1 == graph.nodes.size() ? std::string("final-norm-head") :
      std::string("layer-") + (layer < 10 ? "0" : "") + std::to_string(layer);
    if (graph.nodes[index].id != expected)
      throw std::invalid_argument("hybrid planning graph layer order is not canonical");
    bool found = false;
    for (std::size_t stage = 0; stage < windows.size(); ++stage) {
      const auto& window = windows[stage];
      if ((layer >= window.layerBegin && layer < window.layerEnd) ||
          (layer == shape.layers && stage + 1 == windows.size())) {
        result[stage].push_back(graph.nodes[index].id);
        found = true;
        break;
      }
    }
    if (!found)
      throw std::invalid_argument("hybrid candidate has a node outside every stage");
  }
  return result;
}

std::vector<std::string>
physicalStateNames(const NativeStateTensorMapping::Roles& roles, const std::string& unit)
{
  const auto found = roles.find(unit);
  if (found == roles.end())
    throw std::invalid_argument("source hybrid state mapping omits a unit: " + unit);
  std::vector<std::string> result;
  for (const auto& family : found->second)
    result.insert(result.end(), family.second.begin(), family.second.end());
  return result;
}

NativeStepSlotSpec
sourceStepSlot(const NativeJson& metadata, const std::string& name,
               const std::string& slotId, const std::string& origin)
{
  const auto& facts = metadata.at("tensors").at(name);
  NativeStepSlotSpec slot;
  slot.slotId = slotId;
  slot.dtype = facts.at("dtype").get<std::string>();
  for (const auto& dimension : facts.at("shape")) {
    if (dimension.is_string()) slot.shape.emplace_back(dimension.get<std::string>());
    else slot.shape.emplace_back(dimension.get<std::int64_t>());
  }
  slot.layoutDigest = slot.dtype == "int64" || slot.dtype == "int32"
    ? QWEN_TP_INDEX_LAYOUT_DIGEST : QWEN_TP_ACTIVATION_LAYOUT_DIGEST;
  slot.source = origin;
  return slot;
}

/** Close source graph values before the first merged-plan validation. */
void
bindHybridSourcePorts(std::vector<NativeExecutionPlan>& plans,
                      const std::vector<std::vector<QwenTpUnit>>& units,
                      const std::vector<std::vector<QwenTpEntryContract>>& tables,
                      const std::vector<NativeSourceRegionBoundary>& boundaries,
                      const NativeOnnxGraphInspection& source,
                      const NativeStateTensorMapping& stateMapping)
{
  struct Producer
  {
    std::string step;
    std::string slot;
  };
  std::map<std::string, std::map<std::string, Producer>> valuesByUnit;
  std::map<std::string, NativeExecutionStepSpec*> steps;
  std::map<std::pair<std::string, std::string>, std::string> outputNames;
  std::map<std::string, std::map<std::string, std::string>> inputNames;
  const auto metadata = nativeParseJson(source.graphMetadataJson);
  const auto modelInputs = metadata.at("inputs").get<std::vector<std::string>>();
  for (std::size_t stage = 0; stage < plans.size(); ++stage) {
    for (auto& step : plans[stage].steps) {
      if (!steps.emplace(step.stepId, &step).second)
        throw std::invalid_argument("source hybrid repeats a step");
      if (step.operation != "CALL_SUBGRAPH") continue;
      auto& inputs = inputNames[step.stepId];
      auto& values = valuesByUnit[step.unitId];
      if (units[stage].size() == 1) {
        for (const auto& slot : step.inputSlots) inputs.emplace(slot.slotId, slot.slotId);
        for (const auto& slot : step.outputSlots) {
          values.emplace(slot.slotId, Producer{step.stepId, slot.slotId});
          outputNames.emplace(std::make_pair(step.stepId, slot.slotId), slot.slotId);
        }
        continue;
      }
      const auto contract = std::find_if(tables[stage].begin(), tables[stage].end(),
        [&] (const auto& entry) { return entry.entryId == step.artifactEntry; });
      if (contract == tables[stage].end())
        throw std::invalid_argument("source hybrid CALL has no material contract");
      for (const auto& binding : contract->inputSlotByGraphName)
        if (!inputs.emplace(binding.second, binding.first).second)
          throw std::invalid_argument("source hybrid input slot ambiguously binds graph values");
      for (const auto& binding : contract->outputSlotByGraphName) {
        const bool partial = binding.second == "partial" &&
          (contract->kind == "ATTN" || contract->kind == "MLP");
        const auto producer = partial
          ? (contract->kind == "ATTN" ? sumAttnStep(step.unitId, contract->layer)
                                      : sumMlpStep(step.unitId, contract->layer))
          : step.stepId;
        if (!values.emplace(binding.first,
                             Producer{producer, partial ? "merged" : binding.second}).second)
          throw std::invalid_argument("source hybrid graph value has multiple local producers");
        outputNames.emplace(std::make_pair(step.stepId, binding.second), binding.first);
      }
      // A passthrough describes an alias, never a new source producer.
      for (const auto& binding : contract->passthroughSlotByInputGraphName)
        outputNames.emplace(std::make_pair(step.stepId, binding.second), binding.first);
    }
    for (const auto& step : plans[stage].steps) {
      if (step.operation != "SUM") continue;
      const auto local = std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
        [] (const auto& slot) { return slot.source == "LOCAL_STEP"; });
      if (local == step.inputSlots.end())
        throw std::invalid_argument("source hybrid reduction has no local contribution");
      outputNames.emplace(std::make_pair(step.stepId, "merged"),
        outputNames.at({local->producerStepId, local->producerSlotId}));
    }
  }
  for (std::size_t stage = 0; stage < plans.size(); ++stage) {
    auto& plan = plans[stage];
    plan.dependencies.clear();
    for (auto& step : plan.steps) {
      for (auto& slot : step.outputSlots)
        slot = sourceStepSlot(metadata, outputNames.at({step.stepId, slot.slotId}),
                              slot.slotId, "LOCAL_STEP");
      const auto state = physicalStateNames(stateMapping.inputs, step.unitId);
      for (auto& slot : step.inputSlots) {
        if (step.operation == "CALL_SUBGRAPH") {
          const auto& name = inputNames.at(step.stepId).at(slot.slotId);
          if (std::find(state.begin(), state.end(), name) != state.end())
            throw std::invalid_argument("source hybrid exposes private state as an ordinary input");
          const auto own = valuesByUnit.at(step.unitId).find(name);
          Producer producer;
          if (own != valuesByUnit.at(step.unitId).end() && own->second.step != step.stepId)
            producer = own->second;
          else if (std::find(modelInputs.begin(), modelInputs.end(), name) == modelInputs.end()) {
            for (std::size_t previous = 0; previous < stage; ++previous) {
              if (boundaries[previous].outputs.count(name) == 0) continue;
              const auto& owner = valuesByUnit.at(units[previous].front().roleId);
              const auto value = owner.find(name);
              if (value == owner.end() || !producer.step.empty())
                throw std::invalid_argument("source hybrid boundary has no unique complete producer");
              producer = value->second;
            }
            if (producer.step.empty())
              throw std::invalid_argument("source hybrid input has no producer: " + name);
          }
          slot = sourceStepSlot(metadata, name, slot.slotId,
            producer.step.empty() ? "EPOCH_INPUT" :
            steps.at(producer.step)->unitId == step.unitId ? "LOCAL_STEP" : "REMOTE_STEP");
          slot.producerStepId = producer.step;
          slot.producerSlotId = producer.slot;
        }
        else {
          const auto name = outputNames.at({slot.producerStepId, slot.producerSlotId});
          const auto old = slot;
          slot = sourceStepSlot(metadata, name, old.slotId, old.source);
          slot.producerStepId = old.producerStepId;
          slot.producerSlotId = old.producerSlotId;
        }
        if (slot.source != "REMOTE_STEP") continue;
        NativeDependencySpec dependency;
        dependency.producers = {steps.at(slot.producerStepId)->unitId};
        dependency.consumers = {step.unitId};
        dependency.keyScope = "hybrid-" + std::to_string(stage) + "-" +
          std::to_string(plan.dependencies.size());
        dependency.dependencyId = dependency.keyScope;
        dependency.topicPrefix = "/NDNSF/DI/QWEN";
        dependency.objectNameTemplate =
          "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/{producerRole}/{keyScope}/{sequence}";
        dependency.tensors = {outputNames.at({slot.producerStepId, slot.producerSlotId})};
        dependency.operationKind = step.operation == "SUM" ? "TENSOR_PARTIAL" : "ACTIVATION";
        dependency.useNdnsfDataV1 = true;
        dependency.producerStepId = slot.producerStepId;
        dependency.producerSlotId = slot.producerSlotId;
        dependency.consumerStepId = step.stepId;
        dependency.consumerSlotId = slot.slotId;
        plan.dependencies.push_back(std::move(dependency));
      }
    }
  }
}

} // namespace

static ndnsf::di::NativeSplitCandidate
buildQwenHybridCandidateImpl(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                         const std::vector<QwenTpStageWindow>& windows,
                         const std::vector<std::vector<QwenTpUnit>>& stageUnits,
                         const std::string& entryPrefix, const std::string& egressRole,
                         const NativeModelDescriptor& model,
                         const std::map<std::string, std::string>& artifactByUnit,
                         const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit,
                         const NativeOnnxGraphInspection* source,
                         const QwenTpSourceRegionPort* regionPort = nullptr,
                         const NativeStateTensorMapping* stateMapping = nullptr)
{
  if (windows.empty() || windows.size() != stageUnits.size())
    throw std::invalid_argument("hybrid candidate needs one window per stage");
  if (regionPort != nullptr && (!*regionPort || source == nullptr || stateMapping == nullptr))
    throw std::invalid_argument("source hybrid lowering requires its source and state owner");
  const auto semanticNodes = stageSemanticNodes(graph, shape, windows);
  NativeJson sourceMetadata;
  std::vector<NativeSourceRegionBoundary> boundaries(windows.size());
  std::vector<std::vector<QwenTpEntryContract>> tables(windows.size());
  std::vector<ndnsf::di::NativeExecutionPlan> plans;
  plans.reserve(windows.size());
  std::vector<QwenTpUnit> units;
  for (std::size_t index = 0; index < windows.size(); ++index) {
    auto window = windows.at(index);
    const auto& list = stageUnits.at(index);
    if (list.empty())
      throw std::invalid_argument("hybrid stage has no units: " + window.stageId);
    if (regionPort != nullptr) {
      validateUnits(list, list.size(), shape.layers, window.layerBegin, window.layerEnd);
      for (std::size_t rank = 0; rank < list.size(); ++rank)
        if (list[rank].rank != rank)
          throw std::invalid_argument("source hybrid rank differs from its member order");
    }
    if (index == 0 && window.layerBegin != 0)
      throw std::invalid_argument("the first hybrid stage must own layer zero");
    if (index > 0) {
      const auto& previous = windows.at(index - 1);
      if (window.layerBegin != previous.layerEnd)
        throw std::invalid_argument("hybrid stages must be contiguous");
      if (window.upstreamStepId.empty())
        throw std::invalid_argument("hybrid stage needs its upstream producer: " +
                                    window.stageId);
    }
    ndnsf::di::NativeExecutionPlan plan;
    plan.version = 3;
    plan.modelName = model.modelName;
    plan.modelFamily = "qwen";
    plan.modelFormat = model.modelFormat;
    plan.plannerKind = "native-qwen-hybrid";
    plan.executionPolicy = "DATA_DRIVEN_V2";
    const bool terminal = index + 1 == windows.size();
    if (terminal && !role_ids_contain(list, egressRole))
      throw std::invalid_argument("hybrid result owner must belong to the last stage");
    const auto stageEgress = terminal ? egressRole : list.front().roleId;
    if (regionPort != nullptr) {
      const auto additional = physicalStateNames(stateMapping->outputs, list.front().roleId);
      boundaries[index] = (*regionPort)(semanticNodes[index], additional);
      if (boundaries[index].identityDigest.empty())
        throw std::invalid_argument("source hybrid region is missing its checked identity");
      window.sourceRegionDigest =
        nativePlanningDigest("qwen-hybrid-source-v2|" + boundaries[index].identityDigest);
    }
    if (regionPort != nullptr && list.size() == 1) {
      const auto& unit = list.front();
      const auto stateInputs = physicalStateNames(stateMapping->inputs, unit.roleId);
      const auto stateOutputs = physicalStateNames(stateMapping->outputs, unit.roleId);
      plan.roles = {unit.roleId};
      plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
      plan.tensorDegree = 1;
      plan.stageLayout = {{window.stageId, window.sourceRegionDigest, {unit.roleId}}};
      NativeExecutionStepSpec step;
      step.stepId = step.artifactEntry = unit.roleId + "/REGION";
      step.unitId = unit.roleId;
      step.operation = "CALL_SUBGRAPH";
      const auto slotFor = [] (const NativeAssemblyTensorContractV3& port) {
        NativeStepSlotSpec slot;
        slot.slotId = port.name;
        slot.dtype = port.dtype;
        slot.shape = port.shape;
        slot.layoutDigest = port.dtype == "int64" || port.dtype == "int32"
          ? QWEN_TP_INDEX_LAYOUT_DIGEST : QWEN_TP_ACTIVATION_LAYOUT_DIGEST;
        slot.source = "LOCAL_STEP";
        return slot;
      };
      for (const auto& port : boundaries[index].inputs)
        if (std::find(stateInputs.begin(), stateInputs.end(), port.first) == stateInputs.end())
          step.inputSlots.push_back(slotFor(port.second));
      for (const auto& port : boundaries[index].outputs)
        if (std::find(stateOutputs.begin(), stateOutputs.end(), port.first) == stateOutputs.end())
          step.outputSlots.push_back(slotFor(port.second));
      for (std::uint64_t layer = window.layerBegin; layer < window.layerEnd; ++layer) {
        step.localStateSlots.push_back("kv." + std::to_string(layer) + ".key");
        step.localStateSlots.push_back("kv." + std::to_string(layer) + ".value");
      }
      plan.steps.push_back(std::move(step));
    }
    else if (source != nullptr) {
      tables[index] = describeQwenTpEntryContracts(*source, shape.layers, list,
        window.layerBegin, window.layerEnd, stageEgress);
      if (regionPort != nullptr) {
        if (sourceMetadata.is_null())
          sourceMetadata = nativeParseJson(source->graphMetadataJson);
        auto tp = buildQwenTpTemplate(shape, list.size(), "/verified", list,
                                      window.layerBegin, window.layerEnd);
        tp.entries = tables[index];
        std::set<std::string> ids;
        for (const auto& contract : tp.entries) {
          contract.validate();
          if (!ids.insert(contract.entryId).second)
            throw std::invalid_argument("region entries must have unique identities");
        }
        projectTemplate(plan, shape, list.size(), "/verified", stageEgress, tp,
                        &tp.entries, &window, true, &sourceMetadata);
      }
      else {
        projectQwenTpPlan(plan, shape, list.size(), stageEgress, list, tables[index], window);
      }
    }
    else {
      projectQwenTpPlan(plan, shape, list.size(), entryPrefix, stageEgress, list, window);
    }
    plans.push_back(std::move(plan));
    for (const auto& unit : list) units.push_back(unit);
  }
  if (!role_ids_contain(units, egressRole))
    throw std::invalid_argument("hybrid candidate egress role is not one of its units: " +
                                egressRole);
  if (regionPort != nullptr)
    bindHybridSourcePorts(plans, stageUnits, tables, boundaries, *source, *stateMapping);
  auto merged = mergeQwenStagePlans(plans);

  ndnsf::di::NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter.name = "native-qwen-hybrid";
  candidate.splitter.version = "1";
  candidate.splitter.configurationDigest = nativePlanningDigest(
    std::string("qwen-hybrid|1|") + std::to_string(windows.size()) + "|" + entryPrefix +
    "|" + egressRole);
  candidate.model = model;
  candidate.graphDigest = graph.graphDigest;
  candidate.executionPlan = std::move(merged);
  candidate.inputIngressRole = units.front().roleId;
  candidate.resultEgressRole = egressRole;

  for (const auto& unit : units) {
    if (artifactByUnit.count(unit.roleId) == 0 ||
        requirementByUnit.count(unit.roleId) == 0)
      throw std::invalid_argument("hybrid unit has no artifact or requirement: " + unit.roleId);
    candidate.fragmentsByRole[unit.roleId] = nativePlanningDigest(
      entryPrefix + "|" + unit.roleId + "|" + std::to_string(unit.rank));
    candidate.tensorDegreesByRole[unit.roleId] = 1;
    // The recipe needs the shard geometry of the unit's own stage, not of the widest one.
    std::uint64_t stageDegree = 1;
    for (const auto& list : stageUnits) {
      if (std::any_of(list.begin(), list.end(),
                      [&] (const auto& item) { return item.roleId == unit.roleId; })) {
        stageDegree = list.size();
        break;
      }
    }
    // Singleton stages retain the ordinary, unsharded recipe identity. Their degree and
    // membership remain sealed by stageLayout; only actual shards need a TP recipe binding.
    if (stageDegree > 1)
      candidate.tensorIdentityByRole[unit.roleId] = {stageDegree, unit.rank};
    candidate.rankArtifactDigestsByRole[unit.roleId] = {artifactByUnit.at(unit.roleId)};
    candidate.artifactsByRole[unit.roleId] = {artifactByUnit.at(unit.roleId)};
    candidate.requirementsByRole[unit.roleId] = requirementByUnit.at(unit.roleId);
  }

  // Node ownership follows the stage windows: a node belongs to the stage whose layer range
  // contains it, and every unit of that stage executes it. Handing the ingress unit every
  // node - the tensor-parallel rule - would tell a Provider to assemble layers it never runs.
  // The catalog supplies the semantic graph from NativeQwenLayerSplit::inspectGraph,
  // not individual ONNX operators. Entry classification belongs to materialization.
  for (std::size_t stage = 0; stage < semanticNodes.size(); ++stage) {
    const auto& ownerUnits = stageUnits[stage];
    for (const auto& id : semanticNodes[stage]) {
      candidate.nodeRoles[id] = ownerUnits.front().roleId;
      for (const auto& unit : ownerUnits) candidate.roleNodeIdsByRole[unit.roleId].push_back(id);
    }
  }
  // Every role must appear in the execution relation, even when it owns no node of its own.
  for (const auto& unit : units)
    candidate.roleNodeIdsByRole[unit.roleId];

  for (const auto& unit : units) {
    std::vector<NativeTensorContract> stateInputs;
    std::vector<NativeTensorContract> stateOutputs;
    for (std::uint64_t layer = unit.layerBegin; layer < unit.layerEnd; ++layer) {
      const auto suffix = std::to_string(layer);
      for (const auto& kind : {"key", "value"}) {
        stateInputs.push_back({"past_key_values." + suffix + "." + kind, "uint8",
                               {"batch", "heads", "sequence", "head-dimension"},
                               std::nullopt});
        stateOutputs.push_back({"present." + suffix + "." + kind, "uint8",
                                {"batch", "heads", "sequence", "head-dimension"},
                                std::nullopt});
      }
    }
    candidate.roleStateInputsByRole[unit.roleId] = std::move(stateInputs);
    candidate.roleStateOutputsByRole[unit.roleId] = std::move(stateOutputs);
  }

  candidate.estimatedCosts = {
    {"role_count", std::uint64_t(units.size())},
    {"stage_count", std::uint64_t(windows.size())},
    {"decoder_layers", std::uint64_t(shape.layers)},
  };
  candidate.candidateDigest = candidate.computedDigest();
  return candidate;
}

NativeSplitCandidate
buildQwenHybridCandidate(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                         const std::vector<QwenTpStageWindow>& windows,
                         const std::vector<std::vector<QwenTpUnit>>& stageUnits,
                         const std::string& entryPrefix, const std::string& egressRole,
                         const NativeModelDescriptor& model,
                         const std::map<std::string, std::string>& artifactByUnit,
                         const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit)
{
  return buildQwenHybridCandidateImpl(graph, shape, windows, stageUnits, entryPrefix, egressRole,
    model, artifactByUnit, requirementByUnit, nullptr);
}

NativeSplitCandidate
buildQwenHybridCandidate(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                         const std::vector<QwenTpStageWindow>& windows,
                         const std::vector<std::vector<QwenTpUnit>>& stageUnits,
                         const std::string& entryPrefix, const std::string& egressRole,
                         const NativeModelDescriptor& model,
                         const std::map<std::string, std::string>& artifactByUnit,
                         const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit,
                         const NativeOnnxGraphInspection& source)
{
  return buildQwenHybridCandidateImpl(graph, shape, windows, stageUnits, entryPrefix, egressRole,
    model, artifactByUnit, requirementByUnit, &source);
}

NativeSplitCandidate
buildQwenHybridCandidate(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                         const std::vector<QwenTpStageWindow>& windows,
                         const std::vector<std::vector<QwenTpUnit>>& stageUnits,
                         const std::string& entryPrefix, const std::string& egressRole,
                         const NativeModelDescriptor& model,
                         const std::map<std::string, std::string>& artifactByUnit,
                         const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit,
                         const NativeOnnxGraphInspection& source,
                         const QwenTpSourceRegionPort& regionPort,
                         const NativeStateTensorMapping& stateMapping)
{
  return buildQwenHybridCandidateImpl(graph, shape, windows, stageUnits, entryPrefix, egressRole,
    model, artifactByUnit, requirementByUnit, &source, &regionPort, &stateMapping);
}

void
bindQwenTpGenerationState(const NativeExecutionPlan& plan,
                          NativeGenerationExecutionContractV1& contract)
{
  if (!contract.enabled || contract.mode != "TOKEN_STREAMING") {
    throw std::invalid_argument(
      "tensor-parallel generation state needs a streaming generation contract");
  }
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::set<std::string> seen;
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.localStateSlots) {
      if (!seen.insert(slot).second) continue;
      const auto suffix = slot.substr(slot.find('.') + 1);
      if (slot.rfind("kv.", 0) != 0 || suffix.find('.') == std::string::npos) {
        throw std::invalid_argument(
          "tensor-parallel template state slot is not a KV slot: " + slot);
      }
      inputs.push_back("past_key_values." + suffix);
      outputs.push_back("present." + suffix);
    }
  }
  if (inputs.empty()) {
    throw std::invalid_argument(
      "tensor-parallel generation state found no attention state to carry");
  }
  contract.stateInputNames = std::move(inputs);
  contract.stateOutputNames = std::move(outputs);
  std::string successor;
  for (std::size_t index = 0; index < contract.stateInputNames.size(); ++index) {
    if (index != 0) successor += ',';
    successor += contract.stateInputNames[index] + "=" +
                 contract.stateOutputNames[index];
  }
  contract.stateSuccessorMap = std::move(successor);
}

NativeGenerationExecutionContractV1
buildQwenTpGenerationContract(const NativeExecutionPlan& plan, const QwenTpShape& shape,
                              const QwenTpSealedGeneration& sealed)
{
  if (sealed.maxGeneratedTokens == 0) {
    throw std::invalid_argument(
      "tensor-parallel generation contract needs a token budget");
  }
  if (sealed.samplingDigest.size() != 71 || sealed.tokenizerDigest.size() != 71) {
    throw std::invalid_argument(
      "tensor-parallel generation contract needs canonical digests");
  }
  if (sealed.eosTokenIds.empty()) {
    throw std::invalid_argument(
      "tensor-parallel generation contract needs at least one EOS id");
  }
  // The state pairs come from the template, so the contract cannot name a layer the
  // plan does not execute. One pair per attention state slot, taken in template order
  // so the successor map and the pairs agree.
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::set<std::string> seen;
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.localStateSlots) {
      if (!seen.insert(slot).second) continue;
      // The template names the value it owns as `kv.<layer>.<key|value>`; the export
      // spells the same tensors `past_key_values.<layer>.<key|value>` on input and
      // `present.<layer>.<key|value>` on output. The contract must use the export's
      // names because the runner resolves them against the ONNX session, so the
      // template name is a logical identity rather than a graph name.
      const auto suffix = slot.substr(slot.find('.') + 1);  // "<layer>.<key|value>"
      if (slot.rfind("kv.", 0) != 0 || suffix.find('.') == std::string::npos) {
        throw std::invalid_argument(
          "tensor-parallel template state slot is not a KV slot: " + slot);
      }
      inputs.push_back("past_key_values." + suffix);
      outputs.push_back("present." + suffix);
    }
  }
  if (inputs.empty()) {
    throw std::invalid_argument(
      "tensor-parallel generation contract found no attention state to carry");
  }

  NativeGenerationExecutionContractV1 contract;
  contract.enabled = true;
  contract.mode = "TOKEN_STREAMING";
  contract.maxGeneratedTokens = sealed.maxGeneratedTokens;
  contract.tokenInputName = "input_ids";
  contract.stateInputNames = std::move(inputs);
  contract.stateOutputNames = std::move(outputs);
  // One-to-one mapping in the same order, which is the canonical form the decoder
  // validates.
  std::string successor;
  for (std::size_t index = 0; index < contract.stateInputNames.size(); ++index) {
    if (index != 0) successor += ',';
    successor += contract.stateInputNames[index] + "=" +
                 contract.stateOutputNames[index];
  }
  contract.stateSuccessorMap = std::move(successor);
  contract.positionInputPolicy = "qwen-causal-position-v1";
  contract.attentionMaskInputName = "attention_mask";
  contract.positionIdsInputName = "position_ids";
  contract.eosTokenIds = sealed.eosTokenIds;
  contract.samplingDigest = sealed.samplingDigest;
  contract.tokenizerDigest = sealed.tokenizerDigest;
  contract.samplingMode = sealed.samplingMode;
  contract.samplingTemperature = sealed.samplingTemperature;
  contract.samplingTopK = sealed.samplingTopK;
  contract.samplingTopP = sealed.samplingTopP;
  contract.samplingRepetitionPenalty = sealed.samplingRepetitionPenalty;
  contract.samplingSeed = sealed.samplingSeed;
  contract.generationId = sealed.generationId;
  // One operation per transfer plus the feedback rounds: the decoder requires the
  // stride to be at least the dependency count, and each feedback edge owns its own
  // index, so the stride is the dependency count itself.
  contract.streamingOperationStride = plan.dependencies.size();
  if (contract.streamingOperationStride < plan.dependencies.size() ||
      contract.streamingOperationStride == 0) {
    throw std::invalid_argument(
      "tensor-parallel generation contract has no operation stride room");
  }
  // Vocabulary must match what the head produces, or the sampler reads past its logits.
  if (shape.vocab == 0) {
    throw std::invalid_argument(
      "tensor-parallel generation contract has no vocabulary");
  }
  return contract;
}

void
QwenTpRankGeometry::validate() const
{
  if (degree < 2) {
    throw std::invalid_argument("tensor-parallel geometry requires degree >= 2");
  }
  if (rank >= degree) {
    throw std::invalid_argument("tensor-parallel geometry has a rank outside its degree");
  }
  if (headDim == 0 || queryWidth == 0 || kvWidth == 0 || hidden == 0 || ffn == 0) {
    throw std::invalid_argument("tensor-parallel geometry is incomplete");
  }
  if (queryWidth % degree != 0 || kvWidth % degree != 0 || ffn % degree != 0) {
    throw std::invalid_argument(
      "tensor-parallel geometry does not divide evenly by its degree");
  }
}

std::uint64_t
adaptQwenTpEntryParameters(onnx::ModelProto& entry, const QwenTpRankGeometry& geometry)
{
  geometry.validate();
  auto* graph = entry.mutable_graph();
  const auto shard = [&] (std::uint64_t width) { return width / geometry.degree; };
  const auto queryShard = shard(geometry.queryWidth);
  const auto kvShard = shard(geometry.kvWidth);
  const auto ffnShard = shard(geometry.ffn);

  std::uint64_t changes = 0;

  // 1. Operator attributes that declare a sharded width.
  for (int i = 0; i < graph->node_size(); ++i) {
    auto* node = graph->mutable_node(i);
    if (node->op_type() == "MatMulNBits") {
      const bool outputSharded = node->name().find("q_proj") != std::string::npos ||
        node->name().find("k_proj") != std::string::npos ||
        node->name().find("v_proj") != std::string::npos ||
        node->name().find("gate_proj") != std::string::npos ||
        node->name().find("up_proj") != std::string::npos;
      const bool inputSharded = node->name().find("o_proj") != std::string::npos ||
        node->name().find("down_proj") != std::string::npos;
      // A full-width bias cannot follow column slicing, and a row-partial bias would be
      // added once per contributor by SUM. Ordinary, unsharded projections keep their bias.
      if ((outputSharded || inputSharded) && node->input_size() > 5 &&
          !node->input(5).empty())
        throw std::invalid_argument("tensor-parallel projection has an unsupported bias: " +
                                    node->name());
      for (int a = 0; a < node->attribute_size(); ++a) {
        auto* attr = node->mutable_attribute(a);
        if (!attr->has_i()) continue;
        if (attr->name() == "N" && outputSharded) {
          attr->set_i(attr->i() / static_cast<std::int64_t>(geometry.degree));
          ++changes;
        }
        else if (attr->name() == "K" && inputSharded) {
          attr->set_i(attr->i() / static_cast<std::int64_t>(geometry.degree));
          ++changes;
        }
      }
    }
    else if (node->op_type() == "GroupQueryAttention") {
      for (int a = 0; a < node->attribute_size(); ++a) {
        auto* attr = node->mutable_attribute(a);
        if (!attr->has_i()) continue;
        if (attr->name() == "num_heads") {
          attr->set_i(attr->i() / static_cast<std::int64_t>(geometry.degree));
          ++changes;
        }
        else if (attr->name() == "kv_num_heads") {
          attr->set_i(attr->i() / static_cast<std::int64_t>(geometry.degree));
          ++changes;
        }
      }
      // The head counts above are per rank now, so the KV state tensors must be narrowed to
      // the same heads; otherwise the op declares one KV head while its buffers carry all of
      // them, which ONNX Runtime rejects when the entry runs.
      const auto narrowState = [&] (const std::string& name) {
        for (int i = 0; i < graph->initializer_size(); ++i) {
          auto* state = graph->mutable_initializer(i);
          if (state->name() != name || state->dims_size() != 4) continue;
          const auto heads = static_cast<std::uint64_t>(state->dims(1));
          if (geometry.degree == 0 || heads % geometry.degree != 0) return;
          const auto perRank = heads / geometry.degree;
          const auto sliceBytes = static_cast<std::size_t>(state->dims(2)) *
            static_cast<std::size_t>(state->dims(3)) * sizeof(float);
          const auto offset = static_cast<std::size_t>(geometry.rank * perRank) * sliceBytes;
          const std::string raw = state->raw_data();
          if (offset + perRank * sliceBytes > raw.size()) return;
          std::vector<char> narrowed(raw.begin() + offset,
                                     raw.begin() + offset + perRank * sliceBytes);
          state->set_raw_data(narrowed.data(), narrowed.size());
          state->set_dims(1, static_cast<std::int64_t>(perRank));
          ++changes;
          return;
        }
      };
      if (node->input_size() > 4) {
        narrowState(node->input(3));
        narrowState(node->input(4));
      }
    }
  }

  // 2. Reshape targets, matched by the node that consumes them rather than by the
  // numeric value. KV width and hidden width are both 1024 in this export, so a
  // value match would halve the residual stream along with the KV head dimension.
  // The two normalizations are distinguishable only by their own names.
  for (int i = 0; i < graph->node_size(); ++i) {
    const auto& node = graph->node(i);
    if (node.op_type() != "Reshape") continue;
    std::optional<std::int64_t> target;
    if (node.name().find("q_norm/Reshape_2") != std::string::npos) {
      target = static_cast<std::int64_t>(queryShard);
    }
    else if (node.name().find("k_norm/Reshape_2") != std::string::npos) {
      target = static_cast<std::int64_t>(kvShard);
    }
    if (!target || node.input_size() < 2) continue;
    const auto& targetName = node.input(1);
    for (int j = 0; j < graph->initializer_size(); ++j) {
      auto* init = graph->mutable_initializer(j);
      if (init->name() != targetName) continue;
      if (init->data_type() != onnx::TensorProto::INT64) break;
      if (init->data_location() != onnx::TensorProto::DEFAULT) break;
      if (init->raw_data().empty() || init->raw_data().size() % sizeof(std::int64_t) != 0)
        break;
      std::string bytes = init->raw_data();
      bool changed = false;
      // The shape is [batch, sequence, width]; only the last entry is the sharded
      // one. Rewriting every slot would corrupt the batch and sequence markers.
      const auto slots = bytes.size() / sizeof(std::int64_t);
      if (slots >= 1) {
        const auto offset = (slots - 1) * sizeof(std::int64_t);
        std::memcpy(bytes.data() + offset, &*target, sizeof(std::int64_t));
        changed = true;
      }
      if (changed) {
        init->set_raw_data(bytes.data(), bytes.size());
        ++changes;
      }
      break;
    }
  }

  // 3. KV state dimensions, matched by name. These are the only value constraints
  // this partition changes: the state tensors are [batch, kvHeads, sequence,
  // headDim] and the head axis carries the sharded count. Rewriting value_info
  // dimensions by value would also halve the residual stream, because kvHeads *
  // headDim happens to equal hidden in this export.
  const auto shardKvState = [&] (onnx::ValueInfoProto* value) {
    const auto& name = value->name();
    const bool isState = name.find("past_key_values.") == 0 ||
                         name.find("present.") == 0;
    if (!isState) return;
    auto* shape = value->mutable_type()->mutable_tensor_type()->mutable_shape();
    // [batch, kvHeads, sequence, headDim]
    if (shape->dim_size() < 4) return;
    auto* heads = shape->mutable_dim(1);
    if (!heads->has_dim_value()) return;
    if (static_cast<std::uint64_t>(heads->dim_value()) != geometry.kvWidth / geometry.headDim) {
      return;
    }
    heads->set_dim_value(static_cast<std::int64_t>(kvShard / geometry.headDim));
    ++changes;
  };
  // 4. Intermediate tensor shapes. ORT cross-checks a node's K/N against its
  // input's declared width, so a sharded projection whose output still claims the
  // source width fails at load even though every weight is correct. The width is
  // taken from the producing node rather than from the number, for the same reason
  // as above.
  const auto widthOfProducer = [&] (const std::string& tensor)
    -> std::optional<std::int64_t> {
    for (int i = 0; i < graph->node_size(); ++i) {
      const auto& node = graph->node(i);
      bool produces = false;
      for (const auto& output : node.output())
        if (output == tensor) produces = true;
      if (!produces) continue;
      if (node.op_type() == "MatMulNBits") {
        for (const auto& attr : node.attribute())
          if (attr.name() == "N" && attr.has_i()) return attr.i();
        return std::nullopt;
      }
      if (node.op_type() == "GroupQueryAttention") {
        std::optional<std::int64_t> heads;
        for (const auto& attr : node.attribute())
          if (attr.name() == "num_heads" && attr.has_i()) heads = attr.i();
        if (!heads) return std::nullopt;
        return *heads * static_cast<std::int64_t>(geometry.headDim);
      }
      if (node.op_type() == "Reshape" && node.input_size() >= 2) {
        const auto& targetName = node.input(1);
        for (int j = 0; j < graph->initializer_size(); ++j) {
          const auto& init = graph->initializer(j);
          if (init.name() != targetName) continue;
          if (init.data_type() != onnx::TensorProto::INT64) return std::nullopt;
          if (init.raw_data().size() < sizeof(std::int64_t)) return std::nullopt;
          std::int64_t last = 0;
          const auto offset = init.raw_data().size() - sizeof(std::int64_t);
          std::memcpy(&last, init.raw_data().data() + offset, sizeof(last));
          return last;
        }
      }
      return std::nullopt;
    }
    return std::nullopt;
  };
  const auto rewriteProducerWidth = [&] (onnx::ValueInfoProto* value) {
    const auto width = widthOfProducer(value->name());
    if (!width) return;
    auto* shape = value->mutable_type()->mutable_tensor_type()->mutable_shape();
    if (shape->dim_size() == 0) return;
    auto* last = shape->mutable_dim(shape->dim_size() - 1);
    // Only rewrite a declared value; a symbolic axis stays symbolic.
    if (!last->has_dim_value()) return;
    if (last->dim_value() == *width) return;
    last->set_dim_value(*width);
    ++changes;
  };
  for (int i = 0; i < graph->value_info_size(); ++i)
    rewriteProducerWidth(graph->mutable_value_info(i));

  // SwiGLU's pointwise intermediates inherit their input width. Keep their
  // declarations consistent with the narrowed projections rather than relying
  // on ORT's lenient shape merge to discard stale full-FFN dimensions.
  std::map<std::string, std::int64_t> widths;
  const auto rememberWidth = [&] (const onnx::ValueInfoProto& value) {
    const auto& shape = value.type().tensor_type().shape();
    if (shape.dim_size() == 3 && shape.dim(2).has_dim_value() &&
        shape.dim(2).dim_value() > 0)
      widths[value.name()] = shape.dim(2).dim_value();
  };
  for (const auto& value : graph->input()) rememberWidth(value);
  for (const auto& value : graph->value_info()) rememberWidth(value);
  for (const auto& node : graph->node()) {
    if ((!node.domain().empty() && node.domain() != "ai.onnx") || node.output_size() != 1)
      continue;
    std::optional<std::int64_t> width;
    if (node.op_type() == "Sigmoid" && node.input_size() == 1) {
      const auto input = widths.find(node.input(0));
      if (input != widths.end()) width = input->second;
    }
    else if (node.op_type() == "Mul" && node.input_size() == 2) {
      const auto left = widths.find(node.input(0));
      const auto right = widths.find(node.input(1));
      if (left != widths.end() && right != widths.end() && left->second == right->second)
        width = left->second;
    }
    if (!width) continue;
    widths[node.output(0)] = *width;
    for (auto& value : *graph->mutable_value_info()) {
      if (value.name() != node.output(0)) continue;
      auto* shape = value.mutable_type()->mutable_tensor_type()->mutable_shape();
      if (shape->dim_size() != 3 || !shape->dim(2).has_dim_value() ||
          shape->dim(2).dim_value() == *width)
        continue;
      shape->mutable_dim(2)->set_dim_value(*width);
      ++changes;
    }
  }

  for (int i = 0; i < graph->value_info_size(); ++i)
    shardKvState(graph->mutable_value_info(i));
  for (int i = 0; i < graph->input_size(); ++i)
    shardKvState(graph->mutable_input(i));
  for (int i = 0; i < graph->output_size(); ++i)
    shardKvState(graph->mutable_output(i));

  return changes;
}

std::uint64_t
applyQwenTpRankSlices(onnx::ModelProto& entry,
                      const QwenRankLayerSlices* layerSlices,
                      const std::function<std::vector<std::uint8_t>(
                        std::uint64_t offset, std::uint64_t length)>& readExternal)
{
  if (layerSlices == nullptr) return 0;
  if (!readExternal) {
    throw std::invalid_argument("tensor-parallel slicing needs a sidecar reader");
  }
  auto* graph = entry.mutable_graph();
  if (graph->initializer_size() == 0) return 0;

  // Index the entry's initializers by name so a slice is matched by its declared
  // base name plus the suffixes the export uses, not by a positional guess.
  const auto findByName = [graph] (const std::string& name) -> onnx::TensorProto* {
    for (int i = 0; i < graph->initializer_size(); ++i) {
      if (graph->mutable_initializer(i)->name() == name) {
        return graph->mutable_initializer(i);
      }
    }
    return nullptr;
  };

  std::uint64_t referenced = 0;
  for (const auto& slice : layerSlices->initializers) {
    // A slice names the logical base; the export's real tensors carry the
    // _Q4/_scales suffix. Both are derived from the same base.
    const auto weightName = slice.name + "_Q4";
    const auto scaleName = slice.name + "_scales";
    auto* weight = findByName(weightName);
    if (weight == nullptr) {
      // Not this entry's weight. An attention entry has no MLP projection and
      // vice versa, so absence is expected rather than an error.
      continue;
    }
    auto* scales = findByName(scaleName);
    // Packed layout is [N, K/block, blob]; the dims are the authority on N and K.
    if (weight->dims_size() < 1 || weight->dims_size() > 3) {
      throw std::runtime_error(
        "tensor-parallel weight has an unexpected rank: " + weightName);
    }
    const auto n = static_cast<std::uint64_t>(weight->dims(0));
    const auto blocks = weight->dims_size() >= 2
      ? static_cast<std::uint64_t>(weight->dims(1)) : 0;
    const auto k = blocks * QWEN_INT4_BLOCK_SIZE;
    if (k == 0 || packedInt4Bytes(n, k) !=
        static_cast<std::uint64_t>(weight->raw_data().size()) &&
        weight->data_location() == onnx::TensorProto::DEFAULT) {
      // Inline weights are validated by size; external ones by dims alone.
      throw std::runtime_error(
        "tensor-parallel weight dimensions disagree with its packed size: " +
        weightName);
    }

    // The range a slice addresses on its axis.
    const auto limit = slice.axis == QwenSliceAxis::OutputChannels ? n : k;
    if (slice.end <= slice.begin || slice.end > limit) {
      throw std::runtime_error(
        "tensor-parallel slice range is outside the weight: " + weightName);
    }

    if (slice.axis == QwenSliceAxis::OutputChannels) {
      // Contiguous rows: keep the sidecar, narrow the window. This is the case
      // that keeps a rank from loading its peer's weight rows at all.
      const auto blocksPerRow = k / QWEN_INT4_BLOCK_SIZE;
      const auto rowBytes = blocksPerRow * QWEN_INT4_BLOB_BYTES;
      std::uint64_t baseOffset = 0;
      std::string location;
      for (const auto& kv : weight->external_data()) {
        if (kv.key() == "offset") baseOffset = std::stoull(kv.value());
        if (kv.key() == "location") location = kv.value();
      }
      if (weight->data_location() == onnx::TensorProto::EXTERNAL) {
        if (location.empty()) {
          throw std::runtime_error(
            "tensor-parallel external weight has no location: " + weightName);
        }
        const auto offset = baseOffset + slice.begin * rowBytes;
        const auto length = (slice.end - slice.begin) * rowBytes;
        weight->clear_external_data();
        auto* offsetEntry = weight->add_external_data();
        offsetEntry->set_key("location");
        offsetEntry->set_value(location);
        auto* offsetValue = weight->add_external_data();
        offsetValue->set_key("offset");
        offsetValue->set_value(std::to_string(offset));
        auto* lengthValue = weight->add_external_data();
        lengthValue->set_key("length");
        lengthValue->set_value(std::to_string(length));
        weight->set_dims(0, static_cast<std::int64_t>(slice.end - slice.begin));
        referenced += length;
      }
      else {
        const auto sliced = slicePackedInt4(
          std::vector<std::uint8_t>(weight->raw_data().begin(), weight->raw_data().end()),
          n, k, slice.axis, slice.begin, slice.end);
        weight->set_raw_data(sliced.data(), sliced.size());
        weight->set_dims(0, static_cast<std::int64_t>(slice.end - slice.begin));
        referenced += sliced.size();
      }
    }
    else {
      // Non-contiguous: read the weight, select block columns per row, inline the
      // result. Only this entry's own weight is materialized.
      std::vector<std::uint8_t> source;
      if (weight->data_location() == onnx::TensorProto::EXTERNAL) {
        std::uint64_t baseOffset = 0;
        std::uint64_t length = 0;
        for (const auto& kv : weight->external_data()) {
          if (kv.key() == "offset") baseOffset = std::stoull(kv.value());
          if (kv.key() == "length") length = std::stoull(kv.value());
        }
        if (length != packedInt4Bytes(n, k)) {
          throw std::runtime_error(
            "tensor-parallel external weight length disagrees with its dims: " +
            weightName);
        }
        source = readExternal(baseOffset, length);
      }
      else {
        source.assign(weight->raw_data().begin(), weight->raw_data().end());
      }
      const auto sliced = slicePackedInt4(source, n, k, slice.axis,
                                          slice.begin, slice.end);
      weight->set_data_location(onnx::TensorProto::DEFAULT);
      weight->clear_external_data();
      weight->set_raw_data(sliced.data(), sliced.size());
      weight->set_dims(0, static_cast<std::int64_t>(n));
      weight->set_dims(1, static_cast<std::int64_t>(
        (slice.end - slice.begin) / QWEN_INT4_BLOCK_SIZE));
      referenced += sliced.size();
    }

    // Scales share the weight's axis: one value per (row, block column).
    if (scales != nullptr && scales->data_type() == onnx::TensorProto::FLOAT) {
      std::vector<std::uint8_t> scaleBytes;
      if (scales->data_location() == onnx::TensorProto::EXTERNAL) {
        std::uint64_t baseOffset = 0;
        std::uint64_t length = 0;
        for (const auto& kv : scales->external_data()) {
          if (kv.key() == "offset") baseOffset = std::stoull(kv.value());
          if (kv.key() == "length") length = std::stoull(kv.value());
        }
        scaleBytes = readExternal(baseOffset, length);
      }
      else {
        scaleBytes.assign(scales->raw_data().begin(), scales->raw_data().end());
      }
      const auto slicedScales = sliceInt4Scales(scaleBytes, n, k, slice.axis,
                                                slice.begin, slice.end);
      scales->set_data_location(onnx::TensorProto::DEFAULT);
      scales->clear_external_data();
      scales->set_raw_data(slicedScales.data(), slicedScales.size());
      if (slice.axis == QwenSliceAxis::OutputChannels) {
        scales->set_dims(0, static_cast<std::int64_t>(
          (slice.end - slice.begin) * (k / QWEN_INT4_BLOCK_SIZE)));
      }
      else {
        scales->set_dims(0, static_cast<std::int64_t>(
          n * ((slice.end - slice.begin) / QWEN_INT4_BLOCK_SIZE)));
      }
      referenced += slicedScales.size();
    }
  }
  return referenced;
}

std::string
verifyQwenTpRankEntries(const std::filesystem::path& root, const std::string& unitId,
                        const std::vector<QwenTpEntryContract>& table,
                        const std::vector<NativeExecutionStepSpec>& steps,
                        std::uint64_t rankedByteBudget)
{
  if (unitId.empty()) return "the rank has no unit id";
  if (rankedByteBudget == 0) return "the rank has no byte budget";
  if (table.empty()) return "the rank returned no entries";
  if (steps.empty()) return "the unit has no sealed steps";

  // 1. Set agreement, both directions: a missing entry cannot execute a step, and
  // an unreferenced entry means the artifact is not the one the plan describes.
  std::set<std::string> referenced;
  for (const auto& step : steps) {
    if (step.unitId != unitId) {
      return "step " + step.stepId + " belongs to another unit";
    }
    if (step.operation == "SUM") continue;
    if (step.operation != "CALL_SUBGRAPH") {
      return "step " + step.stepId + " has an unsupported operation";
    }
    if (step.artifactEntry.empty()) {
      return "step " + step.stepId + " names no entry";
    }
    referenced.insert(step.artifactEntry);
  }
  std::set<std::string> held;
  for (const auto& entry : table) {
    if (entry.unitId != unitId) {
      return "the rank returned another unit's entry: " + entry.entryId;
    }
    if (!held.insert(entry.entryId).second) {
      return "the rank repeated an entry id: " + entry.entryId;
    }
  }
  for (const auto& id : referenced) {
    if (held.count(id) == 0) return "the rank is missing an entry a step needs: " + id;
  }
  for (const auto& id : held) {
    if (referenced.count(id) == 0) {
      return "the rank returned an entry no step references: " + id;
    }
  }

  // 2. Path and digest shape, before any file is opened.
  for (const auto& entry : table) {
    if (!entry.hasGraphFile()) {
      return "the entry carries no graph file: " + entry.entryId;
    }
    const std::filesystem::path relative(entry.relativeGraphPath);
    if (relative.is_absolute() || relative.has_parent_path()) {
      return "the entry path is not a plain relative name: " + entry.entryId;
    }
    if (entry.graphDigest.size() != 71 || entry.graphDigest.compare(0, 7, "sha256:") != 0) {
      return "the entry digest is not canonical: " + entry.entryId;
    }
  }

  // 3. Content and budget. The budget is checked as bytes are accumulated so a
  // rank that overruns stops without reading its remaining entries.
  std::uint64_t total = 0;
  for (const auto& entry : table) {
    const auto path = root / entry.relativeGraphPath;
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(path, sizeError);
    if (sizeError || bytes == 0) {
      return "the entry file is missing or empty: " + entry.entryId;
    }
    if (bytes > rankedByteBudget || total > rankedByteBudget - bytes) {
      return "the rank exceeds its total byte budget at entry: " + entry.entryId;
    }
    total += bytes;
    std::error_code readError;
    const auto content = readEntryFile(path);
    if (content.size() != bytes) {
      return "the entry file changed while being verified: " + entry.entryId;
    }
    if (sha256Hex(content) != entry.graphDigest) {
      return "the entry digest does not match its file: " + entry.entryId;
    }
  }
  return {};
}

std::string
qwenTpStepDeclarationJson(const std::vector<NativeExecutionStepSpec>& steps)
{
  if (steps.empty()) {
    throw std::invalid_argument("tensor-parallel step declaration must not be empty");
  }
  NativeJson array = NativeJson::array();
  for (const auto& step : steps) {
    if (step.stepId.empty() || step.unitId.empty() || step.operation.empty()) {
      throw std::invalid_argument("tensor-parallel step declaration is incomplete");
    }
    NativeJson item = NativeJson::object();
    item["stepId"] = step.stepId;
    item["unitId"] = step.unitId;
    item["operation"] = step.operation;
    item["artifactEntry"] = step.artifactEntry;
    array.push_back(std::move(item));
  }
  return array.dump();
}

void
configureQwenTpRankSpec(NativeModelRunnerSpec& spec, std::uint64_t layers,
                        const std::vector<NativeExecutionStepSpec>& steps,
                        std::uint64_t rankedByteBudget,
                        std::uint64_t tensorDegree, std::uint64_t tensorRank,
                        const std::string& egressRole,
                        const QwenTpAssemblyControl* control)
{
  configureQwenTpRankSpec(spec, layers, steps, rankedByteBudget, tensorDegree,
                          tensorRank, egressRole, control, 0);
}

void
configureQwenTpRankSpec(NativeModelRunnerSpec& spec, std::uint64_t layers,
                        const std::vector<NativeExecutionStepSpec>& steps,
                        std::uint64_t rankedByteBudget,
                        std::uint64_t tensorDegree, std::uint64_t tensorRank,
                        const std::string& egressRole,
                        const QwenTpAssemblyControl* control, std::uint64_t layerBegin)
{
  requireRankAssemblyActive(control);
  const auto savedBackend = spec.metadata.find("tensorParallelChildBackend");
  const auto childBackend = spec.backend == "native-qwen-tensor"
    ? (savedBackend == spec.metadata.end() ? "onnxruntime" : savedBackend->second)
    : (spec.backend.empty() ? "onnxruntime" : spec.backend);
  if (childBackend != "onnxruntime" && childBackend != "onnxruntime-cpu" &&
      childBackend != "onnxruntime-cuda")
    throw std::invalid_argument("tensor-parallel child requires an ordinary ONNX backend");
  if (spec.path.empty() || layerBegin >= layers || spec.role.empty()) {
    throw std::invalid_argument(
      "tensor-parallel rank spec needs an assembled path, a role and layers");
  }
  const std::filesystem::path modelPath(spec.path);
  if (!std::filesystem::is_regular_file(modelPath)) {
    throw std::runtime_error(
      "tensor-parallel rank spec names an unreadable model: " + spec.path);
  }
  const auto graphBytes = readEntryFile(modelPath);

  // A tensor-parallel rank narrows every entry to its own shard. The shape comes
  // from the graph rather than from a caller constant, so the slice cannot
  // disagree with the export it is cutting. A pipeline role keeps full width.
  QwenTpAssemblyShards shards;
  if (tensorDegree > 1) {
    if (tensorRank >= tensorDegree) {
      throw std::invalid_argument(
        "tensor-parallel rank is outside the sealed degree");
    }
    const auto shape = deriveQwenTpRegionShape(graphBytes, layerBegin, layers);
    shards.slicesByLayer = planQwenTensorSlices(shape, tensorDegree, tensorRank,
                                               layerBegin, layers);
    // The operators must describe the narrowed weights, or ORT rejects the entry
    // at load; the geometry is read from the same shape the slices came from.
    shards.geometry.degree = tensorDegree;
    shards.geometry.rank = tensorRank;
    shards.geometry.headDim = shape.headDim;
    shards.geometry.queryWidth = shape.queryWidth();
    shards.geometry.kvWidth = shape.kvWidth();
    shards.geometry.hidden = shape.hidden;
    shards.geometry.ffn = shape.ffn;
    // The sidecar sits beside the rank file; only a non-contiguous slice needs it
    // in memory, and only for that entry's own weight.
    const auto sidecar = modelPath.parent_path() / "model.onnx.data";
    const auto sidecarFrozen = modelPath.parent_path() / "model.onnx_data";
    const auto reader = std::filesystem::exists(sidecar) ? sidecar : sidecarFrozen;
    shards.readExternal = [reader] (std::uint64_t offset, std::uint64_t length) {
      std::ifstream in(reader, std::ios::binary);
      if (!in) {
        throw std::runtime_error(
          "tensor-parallel sidecar is unreadable: " + reader.string());
      }
      in.seekg(static_cast<std::streamoff>(offset));
      std::vector<std::uint8_t> bytes(length);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(length));
      if (!in) {
        throw std::runtime_error(
          "tensor-parallel sidecar read was short at offset " + std::to_string(offset));
      }
      return bytes;
    };
  }
  // The egress role decides whether this unit's table keeps the head: classification is
  // per rank, but only one unit runs the head step, so a non-egress table must drop it or
  // the strict verifier refuses an entry no step references.
  // The rank's own budget, and a deadline the caller's assembly control already carries. Before
  // this the table was produced with neither, so a rank could assemble a table larger than its
  // declared budget and a cancellation arriving between entries was only noticed afterwards.
  QwenTpAssemblyControl tableControl = control ? *control : QwenTpAssemblyControl{};
  tableControl.maxAssembledBytes = rankedByteBudget;
  auto table = assembleQwenTpEntryTable(modelPath.parent_path(), spec.role,
                                        graphBytes, layers, shards,
                                        &tableControl, layerBegin, layers);
  retainQwenTpEgressHead(table, spec.role, egressRole);
  // A caller that declares fewer steps than the taxonomy produces is building a partial
  // template -- a test driving one block, or a plan that intentionally executes a slice.
  // The verifier's set-agreement rule exists to catch an artifact that disagrees with the
  // plan, so it is applied to the declared steps and not to the whole taxonomy: an entry no
  // declared step references is dropped here rather than reported as a disagreement. Keeping
  // it would reject every partial template, and the alternative -- relaxing the verifier --
  // would lose the check for a complete one.
  {
    std::set<std::string> referenced;
    for (const auto& step : steps) {
      if (step.operation != "CALL_SUBGRAPH" || step.artifactEntry.empty()) continue;
      referenced.insert(step.artifactEntry);
    }
    if (!referenced.empty() &&
        referenced.size() < table.size()) {
      table.erase(std::remove_if(table.begin(), table.end(),
        [&referenced] (const auto& entry) {
          return referenced.count(entry.entryId) == 0;
        }), table.end());
    }
  }
  if (!egressRole.empty() && spec.role != egressRole) {
    // The non-egress table legitimately holds one entry fewer, so the byte budget and the
    // step set are checked against the table that remains.
    if (table.empty()) {
      throw std::runtime_error(
        "DI_NATIVE_ASSEMBLY_ENTRY_TABLE_EMPTY: the non-egress unit kept no entries");
    }
  }
  // Verify the finished set before the spec can leave the assembler. Checking
  // here means a rank whose entries are incomplete, mis-digested or over budget
  // never becomes a loadable spec, so the Provider cannot publish READY for it.
  // The budget is the assembled artifact's own declared maximum: D6 requires one
  // total for the rank rather than a limit per entry.
  const auto problem = verifyQwenTpRankEntries(modelPath.parent_path(), spec.role,
                                               table, steps, rankedByteBudget);
  if (!problem.empty()) {
    throw std::runtime_error(
      "DI_NATIVE_ASSEMBLY_ENTRY_SET_REJECTED: " + problem);
  }
  requireRankAssemblyActive(&tableControl);
  spec.metadata["tensorParallelEntries"] = qwenTpEntryTableJson(table);
  // The parent locates its children relative to this directory.
  spec.metadata["tensorParallelEntryRoot"] = modelPath.parent_path().string();
  spec.metadata["tensorParallelEntryCount"] = std::to_string(table.size());
  // Also declare the step descriptors the table must satisfy, so runner
  // construction can reject a mismatched pair instead of discovering it after a
  // partial has already been published.
  spec.metadata["tensorParallelSteps"] = qwenTpStepDeclarationJson(steps);
  // The parent runner is selected by backend name, and the single-file rank model
  // is no longer what executes.
  spec.metadata["tensorParallelChildBackend"] = childBackend;
  spec.backend = "native-qwen-tensor";
  requireRankAssemblyActive(&tableControl);
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
    // The binding travels with the table so a Provider reads it rather than deriving it.
    item["inputSlotByGraphName"] = entry.inputSlotByGraphName;
    item["outputSlotByGraphName"] = entry.outputSlotByGraphName;
    item["passthroughSlotByInputGraphName"] = entry.passthroughSlotByInputGraphName;
    item["stateOutputGraphNames"] = NativeJson(entry.stateOutputGraphNames);
    item["synthesizedInputGraphNames"] = NativeJson(entry.synthesizedInputGraphNames);
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
    // A table written before the binding existed has no mapping, and defaulting it to
    // "the slot id equals the graph name" would reintroduce exactly the guess the binding
    // removes. Absent means absent.
    if (item.contains("inputSlotByGraphName"))
      entry.inputSlotByGraphName =
        item.at("inputSlotByGraphName").get<std::map<std::string, std::string>>();
    if (item.contains("outputSlotByGraphName"))
      entry.outputSlotByGraphName =
        item.at("outputSlotByGraphName").get<std::map<std::string, std::string>>();
    if (item.contains("passthroughSlotByInputGraphName"))
      entry.passthroughSlotByInputGraphName =
        item.at("passthroughSlotByInputGraphName").get<std::map<std::string, std::string>>();
    if (item.contains("stateOutputGraphNames"))
      entry.stateOutputGraphNames =
        item.at("stateOutputGraphNames").get<std::vector<std::string>>();
    if (item.contains("synthesizedInputGraphNames"))
      entry.synthesizedInputGraphNames =
        item.at("synthesizedInputGraphNames").get<std::vector<std::string>>();
    entry.validate();
    if (!ids.insert(entry.entryId).second) {
      throw std::invalid_argument(
        "tensor-parallel entry table repeats an entry id: " + entry.entryId);
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

namespace {

/** Whether a graph tensor name looks like a decode-state successor. */
bool
isStateTensorName(const std::string& name)
{
  return name.rfind("present.", 0) == 0;
}

/** Whether a graph tensor name is a past KV input. */
bool
isPastTensorName(const std::string& name)
{
  return name.rfind("past_key_values.", 0) == 0;
}

bool
ownsStateInput(const QwenTpEntryContract& entry, const std::string& name)
{
  if (!isPastTensorName(name)) return false;
  const auto successor = "present." + name.substr(std::string("past_key_values.").size());
  return std::find(entry.stateOutputGraphNames.begin(), entry.stateOutputGraphNames.end(),
                   successor) != entry.stateOutputGraphNames.end();
}

} // namespace

/** Freeze the graph-to-slot binding for one assembled entry.
 *
 * The structure of the frozen export decides the binding, not a name pattern where the
 * structure is available:
 *
 *  - The attention entry returns the projected result and both KV successors, and it
 *    *consumes* the pre-MLP residual it does not return. Its hidden input for layer 0 is
 *    the embedding output; for later layers the input normalization is a skip
 *    normalization whose two inputs are the previous layer's post-attention residual and
 *    its MLP output.
 *  - The MLP entry consumes those same two values and returns the MLP partial plus the
 *    post-MLP residual.
 *  - INIT returns the positions, the reformatted mask and the embedded X0.
 *
 * Ports outside this adapter's semantic aliases are completed by bindIdentitySlots;
 * they retain their actual graph names and require explicit admitted values.
 */
void
bindQwenTpEntrySlots(const onnx::ModelProto& model, QwenTpEntryContract& contract)
{
  const auto& graph = model.graph();
  // Hidden inputs, in graph declaration order, are the float tensors with rank 3.
  std::vector<std::string> hiddenInputs;
  for (const auto& input : graph.input()) {
    const auto& type = input.type().tensor_type();
    if (type.elem_type() == onnx::TensorProto::FLOAT && type.shape().dim_size() == 3)
      hiddenInputs.push_back(input.name());
  }

  if (contract.kind == "INIT") {
    // input_ids and attention_mask are the epoch inputs the template declares.
    for (const auto& input : graph.input()) {
      const auto name = input.name();
      if (name.find("input_ids") != std::string::npos)
        contract.inputSlotByGraphName[name] = "input_ids";
      else if (name.find("attention_mask") != std::string::npos)
        contract.inputSlotByGraphName[name] = "attention_mask";
      else if (name == "position_ids")
        contract.inputSlotByGraphName[name] = "prompt_positions";
      else
        contract.synthesizedInputGraphNames.push_back(name);
    }
    // INIT declares three slots: positions, the reformatted mask and the embedded X0. The
    // export's INIT also returns the mask scalar it derives internally, which has no plan
    // slot; leaving it unbound records that rather than folding two different tensors into
    // one slot id, which the step validator would reject as ambiguous.
    bool positionsBound = false;
    bool maskBound = false;
    bool lengthBound = false;
    bool x0Bound = false;
    for (const auto& output : graph.output()) {
      const auto name = output.name();
      if (!positionsBound && name.find("pos_ids") != std::string::npos) {
        contract.outputSlotByGraphName[name] = "position_ids";
        positionsBound = true;
      }
      else if (!x0Bound && name.find("embed_tokens") != std::string::npos) {
        contract.outputSlotByGraphName[name] = "x0";
        x0Bound = true;
      }
      else if (name.find("attn_mask") != std::string::npos) {
        // Two mask-derived values cross this boundary: the additive mask, which has rank 2,
        // and the total length, which has none. Rank is the difference the attention op
        // itself makes when it consumes them as separate inputs, so it is a structural
        // distinction rather than a naming guess. An unshaped output belongs to the length,
        // because the export leaves that derived scalar unshaped.
        const auto& outputType = output.type().tensor_type();
        const bool additiveMask = outputType.has_shape() && outputType.shape().dim_size() >= 2;
        if (additiveMask && !maskBound) {
          contract.outputSlotByGraphName[name] = "attn_mask";
          maskBound = true;
        }
        else if (!additiveMask && !lengthBound) {
          contract.outputSlotByGraphName[name] = "total_sequence_length";
          lengthBound = true;
        }
      }
    }
    if (!positionsBound && !maskBound && !x0Bound) {
      // A graph whose INIT outputs carry none of the export's names is a different model,
      // and its three boundary values are still positions, mask and X0 in declaration
      // order -- the taxonomy C2 fixes for INIT, not a fact about the export's naming.
      // Binding by position here keeps that contract explicit instead of leaving the
      // entry unbound.
      std::vector<std::string> ordered;
      for (const auto& output : graph.output()) {
        const auto& type = output.type().tensor_type();
        if (type.has_shape() && type.shape().dim().empty()) continue;
        ordered.push_back(output.name());
      }
      if (ordered.size() == 3) {
        contract.outputSlotByGraphName[ordered[0]] = "position_ids";
        contract.outputSlotByGraphName[ordered[1]] = "attn_mask";
        contract.outputSlotByGraphName[ordered[2]] = "x0";
        positionsBound = maskBound = x0Bound = true;
      }
    }
    if (!positionsBound || !maskBound || !lengthBound || !x0Bound) {
      // An INIT that does not expose the export's three values is not the frozen model --
      // a small fixture or another export. Its binding is then identity, which the caller
      // of this function records, rather than a guess about which output is which. The
      // frozen export is checked by the cases that run it; refusing here would make this
      // function reject every other model.
      contract.outputSlotByGraphName.clear();
      return;
    }
    return;
  }

  if (contract.kind == "HEAD") {
    for (const auto& input : graph.input()) {
      if (std::find(hiddenInputs.begin(), hiddenInputs.end(), input.name()) != hiddenInputs.end())
        continue;
      contract.synthesizedInputGraphNames.push_back(input.name());
    }
    // Two hidden inputs carry the merged attention value and the post-MLP residual.
    if (hiddenInputs.size() == 2) {
      const bool firstMlp = hiddenInputs[0].find("down_proj") != std::string::npos;
      const bool secondMlp = hiddenInputs[1].find("down_proj") != std::string::npos;
      if (firstMlp != secondMlp) {
        contract.inputSlotByGraphName[hiddenInputs[firstMlp ? 0 : 1]] = "merged";
        contract.inputSlotByGraphName[hiddenInputs[firstMlp ? 1 : 0]] = "post_residual";
      }
      else {
        contract.inputSlotByGraphName[hiddenInputs[0]] = "merged";
        contract.inputSlotByGraphName[hiddenInputs[1]] = "post_residual";
      }
    }
    // The head returns the vocabulary projection and, in the frozen export, the final
    // normalization's own output as well. Only the projection is a plan slot; binding both
    // would give two graph tensors the same slot id and the step validator rejects that as
    // ambiguous, so the extra one is recorded as unbound rather than guessed at.
    bool logitsBound = false;
    for (const auto& output : graph.output()) {
      if (isStateTensorName(output.name())) {
        contract.stateOutputGraphNames.push_back(output.name());
        continue;
      }
      // The projection is the one declared with the vocabulary width, which is the only
      // output that wide.
      const auto& type = output.type().tensor_type();
      const bool vocabularyWide = type.has_shape() && type.shape().dim_size() >= 2 &&
        type.shape().dim(type.shape().dim_size() - 1).has_dim_value() &&
        static_cast<std::uint64_t>(
          type.shape().dim(type.shape().dim_size() - 1).dim_value()) > 4096;
      if (!logitsBound && (vocabularyWide || output.name() == "logits")) {
        contract.outputSlotByGraphName[output.name()] = "logits";
        logitsBound = true;
      }
    }
    if (!logitsBound) {
      // A head whose projection is not vocabulary-wide is not this model's head; fall back to
      // the first non-state output so a smaller fixture still binds something.
      for (const auto& output : graph.output()) {
        if (isStateTensorName(output.name())) continue;
        contract.outputSlotByGraphName[output.name()] = "logits";
        break;
      }
    }
    return;
  }

  // ATTN and MLP share the output shape: one reduction contribution, one residual the
  // next step needs, and the KV successors. Which is which is structural: the residual
  // is the hidden value the entry carries forward, and for attention that value is an
  // input rather than an output, so it is a passthrough.
  const auto hiddenCount = hiddenInputs.size();
  for (std::size_t index = 0; index < hiddenCount; ++index) {
    const auto& name = hiddenInputs[index];
    if (contract.kind == "ATTN") {
      if (hiddenCount == 1) {
        // Layer 0: the single hidden input is both the attention value and the residual the
        // MLP entry later adds to, so it is passed through rather than asked for.
        contract.inputSlotByGraphName[name] = "x";
        contract.passthroughSlotByInputGraphName[name] = "residual";
      }
      else {
        // Later layers: the skip normalization adds the previous layer's MLP output to its
        // post-attention residual, so the two inputs are told apart by which one the MLP
        // produced. Declaration order is not reliable -- the export's order follows its own
        // graph construction, not the semantic read-out order.
        // Layer 1's two inputs: the previous layer's post-attention normalization output and
        // its MLP projection output. Both live under `layers.0`, so the path does not
        // distinguish them -- the producing op does, and only the MLP's is a `down_proj`.
        const bool fromMlp = name.find("down_proj") != std::string::npos;
        if (fromMlp) {
          contract.inputSlotByGraphName[name] = "previous_mlp";
        }
        else {
          contract.inputSlotByGraphName[name] = "post_attention";
          contract.passthroughSlotByInputGraphName[name] = "residual";
        }
      }
      continue;
    }
    // MLP: the merged attention value and the residual it adds to. The merged value is what
    // the attention projection produced; the residual is the stream the skip normalization
    // adds it to. Which is which follows from the producing operation, not from position in
    // the declaration -- the export declares the residual first.
    const bool fromAttention = name.find("o_proj") != std::string::npos ||
                               name.find("attn") != std::string::npos;
    contract.inputSlotByGraphName[name] = fromAttention ? "merged" : "residual";
  }

  // Mask and total length are distinct ports with distinct ranks. Unknown control inputs
  // retain identity bindings below rather than borrowing either semantic slot.
  bool maskSlotTaken = false;
  bool lengthSlotTaken = false;
  for (const auto& input : graph.input()) {
    const auto name = input.name();
    if (contract.inputSlotByGraphName.count(name) != 0) continue;
    if (isPastTensorName(name)) continue;
    const auto& type = input.type().tensor_type();
    const bool additiveMask = type.has_shape() && type.shape().dim_size() >= 2;
    if (name.find("attn_mask") != std::string::npos) {
      // The same two mask-derived values INIT produces: the additive mask and the scalar
      // length, told apart by rank. Both are plan slots, because INIT produces both.
      if (additiveMask && !maskSlotTaken) {
        contract.inputSlotByGraphName[name] = "attn_mask";
        maskSlotTaken = true;
      }
      else if (!additiveMask && !lengthSlotTaken) {
        contract.inputSlotByGraphName[name] = "total_sequence_length";
        lengthSlotTaken = true;
      }
      else {
        contract.synthesizedInputGraphNames.push_back(name);
      }
    }
    else if (!additiveMask && name.find("pos_ids") != std::string::npos) {
      contract.inputSlotByGraphName[name] = "position_ids";
    }
    else {
      contract.synthesizedInputGraphNames.push_back(name);
    }
  }

  for (const auto& output : graph.output()) {
    const auto name = output.name();
    if (isStateTensorName(name)) {
      contract.stateOutputGraphNames.push_back(name);
      continue;
    }
    if (contract.kind == "ATTN" && name.find("o_proj") != std::string::npos)
      contract.outputSlotByGraphName[name] = "partial";
    else if (contract.kind == "MLP" && name.find("down_proj") != std::string::npos)
      contract.outputSlotByGraphName[name] = "partial";
    // An attention entry also returns its post-attention normalization; that value is the
    // residual slot, and the MLP entry returns its own post-MLP residual. Both are the
    // float output that is not the projection.
    else
      contract.outputSlotByGraphName[name] =
        contract.kind == "ATTN" ? "residual" : "post_residual";
  }
}

/** Fall back to an identity binding for a graph whose structure names no slot.
 *
 * The frozen export's structure decides its binding, so most entries are recognised.
 * A graph assembled from a different model -- a small fixture, another export -- keeps
 * its own tensor names as slot ids. That is a recorded contract rather than a runtime
 * guess: the binding is frozen into the table, so the parent still reads a declaration
 * instead of inferring one from a name it just saw.
 */
void
bindIdentitySlots(QwenTpEntryContract& contract)
{
  for (const auto& name : contract.inputNames)
    if (!ownsStateInput(contract, name) && contract.inputSlotByGraphName.count(name) == 0)
      contract.inputSlotByGraphName[name] = name;
  // Unknown real graph inputs remain explicit dependencies, never implicit defaults.
  contract.synthesizedInputGraphNames.erase(std::remove_if(
    contract.synthesizedInputGraphNames.begin(), contract.synthesizedInputGraphNames.end(),
    [&] (const auto& name) { return contract.inputSlotByGraphName.count(name) != 0; }),
    contract.synthesizedInputGraphNames.end());
  for (const auto& name : contract.outputNames) {
    if (contract.outputSlotByGraphName.count(name) != 0) continue;
    if (std::find(contract.stateOutputGraphNames.begin(),
                  contract.stateOutputGraphNames.end(), name) !=
        contract.stateOutputGraphNames.end())
      continue;
    contract.outputSlotByGraphName[name] = name;
  }
}

std::vector<QwenTpEntryContract>
describeQwenTpEntryContracts(const NativeOnnxGraphInspection& source, std::uint64_t layers,
                             const std::vector<QwenTpUnit>& units,
                             std::uint64_t layerBegin, std::uint64_t layerEnd,
                             const std::string& egressRole)
{
  if (units.empty() || layerBegin >= layerEnd || layerEnd > layers)
    throw std::invalid_argument("source entry description needs units and a valid region");
  const auto metadata = nativeParseJson(source.graphMetadataJson);
  const auto& nodes = metadata.at("nodes");
  if (nodes.size() != source.graph.nodes.size())
    throw std::invalid_argument("source entry metadata node cover differs from inspection");

  // A structural view only: no initializer payload, graph hash or executable file is made.
  // Boundary discovery reads sets of input names, so restored initializer operands need
  // ownership, not their original argument positions or weight bytes.
  onnx::ModelProto structure;
  auto& graph = *structure.mutable_graph();
  for (const auto& node : nodes) {
    if (node.at("index").get<std::uint64_t>() !=
        static_cast<std::uint64_t>(graph.node_size()))
      throw std::invalid_argument("source entry metadata is not in canonical node order");
    auto* item = graph.add_node();
    item->set_name(node.at("name").get<std::string>());
    item->set_op_type(node.at("opType").get<std::string>());
    for (const auto& name : node.at("inputs")) item->add_input(name.get<std::string>());
    for (const auto& name : node.at("outputs")) item->add_output(name.get<std::string>());
  }
  std::set<std::string> initializers;
  for (const auto& item : metadata.at("initializers")) {
    const auto name = item.get<std::string>();
    if (!initializers.insert(name).second)
      throw std::invalid_argument("source entry metadata repeats an initializer");
    graph.add_initializer()->set_name(name);
    const auto owners = source.initializerConsumers.find(name);
    if (owners == source.initializerConsumers.end())
      throw std::invalid_argument("source entry metadata lacks initializer ownership");
    for (const auto index : owners->second) {
      if (index >= static_cast<std::uint64_t>(graph.node_size()))
        throw std::invalid_argument("source entry initializer owner is outside the graph");
      graph.mutable_node(static_cast<int>(index))->add_input(name);
    }
  }
  if (initializers.size() != source.initializerConsumers.size())
    throw std::invalid_argument("source entry initializer ownership has foreign keys");

  const auto valueInfo = [&] (const std::string& name) {
    const auto& facts = metadata.at("tensors").at(name);
    onnx::ValueInfoProto value;
    value.set_name(name);
    auto* type = value.mutable_type()->mutable_tensor_type();
    const auto dtype = facts.at("dtype").get<std::string>();
    if (dtype.rfind("onnx_type_", 0) == 0) {
      const auto label = dtype.substr(10);
      std::size_t used = 0;
      const auto scalar = std::stol(label, &used);
      if (used != label.size() || scalar < 0 ||
          scalar > std::numeric_limits<std::int32_t>::max())
        throw std::invalid_argument("source entry has an invalid ONNX scalar type");
      type->set_elem_type(static_cast<std::int32_t>(scalar));
    }
    else {
      type->set_elem_type(onnxTensorScalarType(tensorElementTypeFromName(dtype)));
    }
    for (const auto& dimension : facts.at("shape")) {
      auto* dim = type->mutable_shape()->add_dim();
      if (dimension.is_string()) dim->set_dim_param(dimension.get<std::string>());
      else dim->set_dim_value(dimension.get<std::int64_t>());
    }
    return value;
  };
  for (const auto& name : metadata.at("inputs"))
    *graph.add_input() = valueInfo(name.get<std::string>());
  for (const auto& name : metadata.at("outputs"))
    *graph.add_output() = valueInfo(name.get<std::string>());
  for (const auto& item : metadata.at("tensors").items())
    if (initializers.count(item.key()) == 0) *graph.add_value_info() = valueInfo(item.key());

  const auto entries = classifyQwenRegionNodes(structure, layers, layerBegin, layerEnd);
  std::vector<QwenTpEntryContract> result;
  for (const auto& unit : units) {
    if (unit.layerBegin != layerBegin || unit.layerEnd != layerEnd)
      throw std::invalid_argument("source entry unit differs from its region");
    for (const auto& entry : entries) {
      if (!ownsRegionEntry(entry, layers, layerBegin, layerEnd) ||
          (entry.kind == "HEAD" && unit.roleId != egressRole)) continue;
      const auto boundary = describeQwenTpEntryBoundary(structure, entry);
      onnx::ModelProto ports;
      for (const auto& input : boundary.first) *ports.mutable_graph()->add_input() = input;
      for (const auto& output : boundary.second) *ports.mutable_graph()->add_output() = output;
      QwenTpEntryContract contract;
      contract.entryId = unit.roleId + "/" + entry.kind + "/" + std::to_string(entry.layer);
      contract.unitId = unit.roleId;
      contract.kind = entry.kind;
      contract.layer = entry.layer;
      for (const auto& input : boundary.first) contract.inputNames.push_back(input.name());
      for (const auto& output : boundary.second) contract.outputNames.push_back(output.name());
      bindQwenTpEntrySlots(ports, contract);
      bindIdentitySlots(contract);
      contract.validate();
      result.push_back(std::move(contract));
    }
  }
  return result;
}

/** Write an already-extracted entry: slice, declare, persist, digest.
 *
 * Declared at file scope rather than in the header because it takes the extracted model by value;
 * the public entry point extracts first and calls it. Both the direct path and the worker-certified
 * path must produce byte-identical entries, so they share this code instead of repeating it. My
 * first version put the declaration in an anonymous namespace and the definition outside it, which
 * linked against a symbol that did not exist.
 */
QwenTpEntryContract
finalizeQwenTpEntry(const std::filesystem::path& root, const std::string& unitId,
                    const QwenTpEntryNodes& entry, onnx::ModelProto model,
                    const std::string& graphName,
                    const QwenTpAssemblyShards* shards);

QwenTpEntryContract
writeQwenTpEntry(const std::filesystem::path& root, const std::string& unitId,
                 const QwenTpEntryNodes& entry, onnx::ModelProto& source,
                 const std::string& graphName,
                 const QwenTpAssemblyShards* shards)
{
  const auto entryId = unitId + "/" + entry.kind + "/" + std::to_string(entry.layer);
  return finalizeQwenTpEntry(root, unitId, entry,
                             extractQwenTpEntryModel(source, entry, entryId),
                             graphName, shards);
}

QwenTpEntryContract
finalizeQwenTpEntry(const std::filesystem::path& root, const std::string& unitId,
                     const QwenTpEntryNodes& entry, onnx::ModelProto model,
                     const std::string& graphName,
                     const QwenTpAssemblyShards* shards)
{
  // Everything after extraction: narrow the weights to this rank, declare the boundary from the
  // file itself, write it atomically and digest what landed. Split out of `writeQwenTpEntry` so a
  // caller that obtained the extracted model elsewhere -- the native assembly worker, which
  // extracts by node index and certifies the result -- produces an identical entry through the
  // same code. Duplicating this would let the two paths disagree about slicing or about the digest.
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

  // Narrow this entry's weights to the rank's own shard before the file is
  // written, so the digest covers what the rank will actually load. INIT and HEAD
  // own no sharded projection, so they pass no slices.
  if (shards != nullptr && shards->enabled()) {
    const QwenRankLayerSlices* owned = nullptr;
    for (const auto& candidate : shards->slicesByLayer) {
      if (candidate.layer == entry.layer) {
        owned = &candidate;
        break;
      }
    }
    if (owned != nullptr && (entry.kind == "ATTN" || entry.kind == "MLP")) {
      // Matching nothing is legal: an entry can hold only normalizations, and a
      // layer may have no sharded projection in a small fixture. The caller that
      // needs to know a real shard was narrowed asserts the resulting sizes, which
      // is stronger than rejecting here -- a rank with silently full weights would
      // still pass a "sliced something" check.
      (void)applyQwenTpRankSlices(model, owned, shards->readExternal);
      // The weights are narrow now, so the operators must describe them. Doing it
      // only when the caller supplied geometry keeps the slice-only path honest:
      // it produces a model whose weights and declarations disagree, which is a
      // tool for a byte-level test rather than something a Provider may load.
      if (shards->geometry.degree > 1) {
        (void)adaptQwenTpEntryParameters(model, shards->geometry);
      }
    }
  }

  // Boundary names come from the extracted graph itself, in its own declaration
  // order, so a consumer and the file cannot disagree about the interface.
  QwenTpEntryContract contract;
  contract.entryId = entryId;
  contract.unitId = unitId;
  contract.kind = entry.kind;
  contract.layer = entry.layer;
  for (const auto& input : model.graph().input()) contract.inputNames.push_back(input.name());
  for (const auto& output : model.graph().output()) contract.outputNames.push_back(output.name());
  bindQwenTpEntrySlots(model, contract);
  bindIdentitySlots(contract);

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
  return classifyQwenRegionNodes(nodeNames, layers, 0, layers);
}

namespace {

std::vector<QwenTpEntryNodes>
classifyQwenRegionNodes(const std::vector<std::string>& nodeNames, std::uint64_t layers,
                       std::uint64_t layerBegin, std::uint64_t layerEnd)
{
  if (nodeNames.empty() || layers == 0 || layerBegin >= layerEnd || layerEnd > layers) {
    throw std::invalid_argument("tensor-parallel entry classification needs a graph");
  }
  // Layer indices run [0, layers); the export also carries a final
  // normalization under layers.<layers>, which belongs to HEAD rather than to a
  // transformer layer.
  std::vector<QwenTpEntryNodes> entries;
  QwenTpEntryNodes init;
  init.kind = "INIT";
  init.layer = 0;
  QwenTpEntryNodes head;
  head.kind = "HEAD";
  // HEAD sits after the last transformer layer, so its layer index is the layer
  // count itself. The step template names the head entry by that index, and a
  // mismatch here would leave every projected head step without a contract.
  head.layer = layers;
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

  if (layerBegin == 0 && init.nodeIndices.empty()) {
    throw std::invalid_argument(
      "tensor-parallel entry classification found no INIT nodes");
  }
  for (std::uint64_t layer = layerBegin; layer < layerEnd; ++layer) {
    const auto& pair = perLayer[layer];
    if (pair.first.nodeIndices.empty() || pair.second.nodeIndices.empty()) {
      throw std::invalid_argument(
        "tensor-parallel entry classification left a layer without attention or MLP nodes");
    }
  }
  if (!init.nodeIndices.empty()) entries.push_back(std::move(init));
  for (auto& pair : perLayer) {
    if (!pair.first.nodeIndices.empty()) entries.push_back(std::move(pair.first));
    if (!pair.second.nodeIndices.empty()) entries.push_back(std::move(pair.second));
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

} // namespace

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

std::pair<std::uint64_t, std::uint64_t>
stageLayerWindow(const ndnsf::di::NativeExecutionPlan& stage)
{
  bool seen = false;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  for (const auto& step : stage.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    const auto slash = step.stepId.rfind('/');
    if (slash == std::string::npos || slash + 1 >= step.stepId.size()) continue;
    std::uint64_t layer = 0;
    try {
      layer = std::stoull(step.stepId.substr(slash + 1));
    }
    catch (const std::exception&) {
      continue;
    }
    if (!seen) {
      first = last = layer;
      seen = true;
    }
    else {
      first = std::min(first, layer);
      last = std::max(last, layer);
    }
  }
  if (!seen)
    throw std::invalid_argument("stage plan has no layer-bearing step");
  return {first, last};
}

std::string
boundaryPublisher(const ndnsf::di::NativeExecutionPlan& stage)
{
  if (stage.stageLayout.size() != 1 || stage.stageLayout.front().unitRoleIds.empty())
    throw std::invalid_argument("a hybrid stage needs one non-empty layout row");
  return stage.stageLayout.front().unitRoleIds.front();
}

ndnsf::di::NativeExecutionPlan
mergeQwenStagePlans(const std::vector<ndnsf::di::NativeExecutionPlan>& stages)
{
  if (stages.empty())
    throw std::invalid_argument("a hybrid plan needs at least one stage");
  ndnsf::di::NativeExecutionPlan merged;
  std::set<std::string> stageIds;
  std::set<std::string> unitIds;
  for (const auto& stage : stages) {
    if (stage.stepPlanVersion != ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2 ||
        stage.stageLayout.size() != 1) {
      throw std::invalid_argument(
        "every hybrid stage must be a single-window version-two plan");
    }
    const auto& layout = stage.stageLayout.front();
    if (!stageIds.insert(layout.stageId).second)
      throw std::invalid_argument("hybrid stages share a stage id: " + layout.stageId);
    for (const auto& unit : layout.unitRoleIds) {
      if (!unitIds.insert(unit).second)
        throw std::invalid_argument("hybrid stages share a unit: " + unit);
    }
    merged.roles.insert(merged.roles.end(), stage.roles.begin(), stage.roles.end());
    merged.steps.insert(merged.steps.end(), stage.steps.begin(), stage.steps.end());
    merged.dependencies.insert(merged.dependencies.end(), stage.dependencies.begin(),
                               stage.dependencies.end());
    merged.stageLayout.push_back(layout);
    merged.tensorDegree = std::max(merged.tensorDegree, stage.tensorDegree);
  }
  merged.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V2;
  merged.streamingOperationStride = merged.dependencies.size();
  // The merged plan is the first object that contains every producer, so it is the first one
  // that can be validated.
  merged.validateSteps();
  merged.modelFamily = stages.front().modelFamily;
  merged.plannerKind = stages.front().plannerKind;
  merged.serviceName = stages.front().serviceName;
  merged.modelName = stages.front().modelName;
  return merged;
}

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units,
                  const QwenTpStageWindow& window)
{
  const auto tp = buildQwenTpTemplate(shape, degree, entryPrefix, units,
                                      window.layerBegin, window.layerEnd);
  projectTemplate(plan, shape, degree, entryPrefix, egressRole, tp, nullptr, &window);
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

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units,
                  const std::vector<QwenTpEntryContract>& entries,
                  const QwenTpStageWindow& window)
{
  auto tp = buildQwenTpTemplate(shape, degree, "/verified", units,
                                window.layerBegin, window.layerEnd);
  tp.entries = entries;
  std::set<std::string> ids;
  for (const auto& contract : tp.entries) {
    contract.validate();
    if (!ids.insert(contract.entryId).second)
      throw std::invalid_argument("region entries must have unique identities");
  }
  projectTemplate(plan, shape, degree, "/verified", egressRole, tp, &tp.entries, &window, true);
}

} // namespace ndnsf::di::qwen
