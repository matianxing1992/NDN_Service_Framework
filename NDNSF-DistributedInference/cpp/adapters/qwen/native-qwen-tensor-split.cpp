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

QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix)
{
  shape.validate();
  if (degree < 2) {
    throw std::invalid_argument("tensor-parallel template requires degree >= 2");
  }
  if (entryPrefix.empty()) {
    throw std::invalid_argument("tensor-parallel template requires an entry prefix");
  }
  QwenTpTemplate result;
  result.degree = degree;
  for (std::uint64_t rank = 0; rank < degree; ++rank) {
    result.units.push_back(unitFor(rank, shape));
    result.slicesByRank.push_back(planQwenTensorSlices(shape, degree, rank));
  }
  return result;
}

void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole)
{
  const auto tp = buildQwenTpTemplate(shape, degree, entryPrefix);
  plan.roles.clear();
  plan.steps.clear();
  plan.dependencies.clear();
  for (const auto& unit : tp.units) plan.roles.push_back(unit.roleId);
  plan.stepPlanVersion = ndnsf::di::NATIVE_STEP_PLAN_VERSION_V1;
  plan.tensorDegree = degree;
  plan.modelFamily = "qwen";
  plan.plannerKind = "native-qwen-tensor";

  const auto entry = [&] (const std::string& unit, const char* kind,
                          std::uint64_t layer) {
    return entryPrefix + "/" + unit + "/" + kind + "/" + std::to_string(layer);
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
    init.outputSlots = {makeIndexSlot("position_ids", "LOCAL_STEP", "int64"),
                        makeIndexSlot("attn_mask", "LOCAL_STEP", "int32")};
    plan.steps.push_back(std::move(init));

    for (std::uint64_t layer = 0; layer < shape.layers; ++layer) {
      // Attention partial on this rank. Layer 0 reads the epoch input; later
      // layers read the MLP sum of the previous layer.
      ndnsf::di::NativeExecutionStepSpec attn;
      attn.stepId = attnStep(unit.roleId, layer);
      attn.unitId = unit.roleId;
      attn.operation = "CALL_SUBGRAPH";
      attn.artifactEntry = entry(unit.roleId, "ATTN", layer);
      attn.inputSlots = {layer == 0
        ? makeSlot("x", "EPOCH_INPUT", {}, shape.hidden)
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

} // namespace ndnsf::di::qwen
