#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

constexpr auto TENSOR_LAYOUT_ENTRY = "tensor-layout-entry";

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char digit) {
      return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

std::string
computeTensorBoundaryDigest(const NativeExecutionStepSpec& step)
{
  const auto encode = [] (const auto& ports) {
    auto values = NativeJson::array();
    for (const auto& port : ports) {
      if ((port.dtype != "float32" && port.dtype != "int64") || !isDigest(port.layoutDigest))
        throw std::invalid_argument("source tensor entry has unsupported dtype or layout");
      auto shape = NativeJson::array();
      for (const auto& dim : port.shape) {
        const auto* value = std::get_if<std::int64_t>(&dim);
        if (value == nullptr || *value <= 0)
          throw std::invalid_argument("source tensor entry requires static dimensions");
        shape.push_back(*value);
      }
      values.push_back({{"slot", port.slotId}, {"dtype", port.dtype},
                        {"shape", std::move(shape)}, {"layout", port.layoutDigest}});
    }
    return values;
  };
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"inputs", encode(step.inputSlots)}, {"outputs", encode(step.outputSlots)}}));
}

} // namespace

std::string
NativeOnnxEntryPort::slotId() const
{
  if (sourceName.empty() || (range && range->begin >= range->end))
    throw std::invalid_argument("source tensor port has no operand or a reversed slice");
  if (!range) return sourceName;
  return sourceName + "/slice/" + std::to_string(range->axis) + "/" +
    std::to_string(range->begin) + "/" + std::to_string(range->end);
}

std::string
NativeOnnxEntryPort::layoutDigest(const std::string& graphDigest) const
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"graph", graphDigest}, {"source", sourceName}, {"physical_slot", slotId()}}));
}

void
NativeOnnxEntryPort::validateSourceTensor(const NativeStepSlotSpec& slot,
                                        const NativeTensorContract& source) const
{
  if (slot.slotId != slotId() || sourceName != source.name || slot.dtype != source.dtype)
    throw std::invalid_argument("physical CALL port differs from its original ONNX operand");
  auto shape = source.shape;
  if (range) {
    if (range->axis >= shape.size())
      throw std::invalid_argument("source tensor port slice axis is outside its original shape");
    auto& axis = shape[range->axis];
    const auto* extent = std::get_if<std::int64_t>(&axis);
    if (extent == nullptr || *extent <= 0 || range->end > static_cast<std::uint64_t>(*extent))
      throw std::invalid_argument("source tensor port slice exceeds the original axis extent");
    axis = static_cast<std::int64_t>(range->end - range->begin);
  }
  if (slot.shape != shape)
    throw std::invalid_argument("physical CALL port shape differs from its original ONNX operand");
}

NativeOnnxSourceEntryRecipeV1
NativeOnnxSourceEntryRecipeV1::fromTensorStep(const NativeExecutionStepSpec& step)
{
  const auto [begin, end] = step.sourceLayers;
  if (step.operation != "CALL_SUBGRAPH" || end <= begin)
    throw std::invalid_argument("tensor entry requires a CALL with an original source interval");
  NativeOnnxSourceEntryRecipeV1 entry;
  entry.entryId = step.artifactEntry;
  for (auto ordinal = begin; ordinal < end; ++ordinal)
    entry.sourceNodes.push_back("onnx-node-" + std::to_string(ordinal));
  entry.transform.kind = TENSOR_LAYOUT_ENTRY;
  entry.transform.node = entry.sourceNodes.front();
  for (const auto& port : step.inputSlots) entry.inputPorts.push_back(port.slotId);
  for (const auto& port : step.outputSlots) entry.outputPorts.push_back(port.slotId);
  entry.tensorBoundaryDigest = computeTensorBoundaryDigest(step);
  return entry;
}

std::uint64_t
nativeFfnSliceBytes(std::uint64_t sourceBytes, std::uint64_t extent, std::uint64_t width)
{
  if (sourceBytes == 0 || width == 0 || width > extent)
    throw std::invalid_argument("initializer bytes do not fit the FFN channel split");
  // Individual packed blocks may have fractional bytes per logical channel.
  const auto common = std::gcd(extent, width);
  const auto denominator = extent / common;
  if (sourceBytes % denominator != 0)
    throw std::invalid_argument("initializer bytes do not fit the FFN channel split");
  return (sourceBytes / denominator) * (width / common);
}

void
NativeOnnxSourceTransformV1::validate() const
{
  if (schema != "source_transform_v1")
    throw std::invalid_argument("source transform schema is unsupported");
  if (kind == "conv-input-channel-partial") {
    if (node.empty() || input.empty() || weight.empty() || output.empty())
      throw std::invalid_argument("channel-partial transform is incomplete");
    if (axis.empty() || extent < 2 || begin >= end || end > extent)
      throw std::invalid_argument("channel-partial transform range is invalid");
  }
  else if (kind == "add-conv-bias") {
    if (sourceConv.empty() || bias.empty() || output.empty())
      throw std::invalid_argument("conv-bias transform is incomplete");
  }
  else if (kind == "ffn-pair-partial") {
    if (node.empty() || downNode.empty() || node == downNode || input.empty() ||
        output.empty() || input == output || weight.empty() || downWeight.empty() ||
        weight == downWeight || (!bias.empty() && (bias == weight || bias == downWeight)))
      throw std::invalid_argument("paired FFN transform is incomplete");
    if (axis != "ffn-channel" || extent < 2 || begin >= end || end > extent ||
        end - begin == extent || !pads.empty() || !strides.empty() || !dilations.empty())
      throw std::invalid_argument("paired FFN transform range is invalid");
  }
  else if (isAttention()) {
    const std::set<std::string> ports(attentionPorts.begin(), attentionPorts.end());
    const std::set<std::string> rmsPorts{input, output, statistic, globalStatistic};
    const bool plain = kind == "attention-source-component";
    const bool validPorts = plain
      ? attentionPorts.empty() && input.empty() && output.empty() &&
        statistic.empty() && globalStatistic.empty()
      : isRmsNorm()
      ? attentionPorts.empty() && rmsPorts.size() == 4 && rmsPorts.count("") == 0
      : ports.size() == 3 && attentionPorts.size() == ports.size() && ports.count("") == 0 &&
        input.empty() && output.empty() && statistic.empty() && globalStatistic.empty();
    const bool validGeometry = plain
      ? axis.empty() && headWidth == 0 && extent == 0 && begin == 0 && end == 0
      : axis == "attention-head" && headWidth != 0 && extent >= 2 && begin < end &&
        end <= extent &&
        extent <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / headWidth;
    if (node.empty() || !validPorts || !validGeometry ||
        !sourceConv.empty() || !weight.empty() || !bias.empty() ||
        !downNode.empty() || !downWeight.empty() ||
        !pads.empty() || !strides.empty() || !dilations.empty())
      throw std::invalid_argument("invalid source attention head recipe");
  }
  else if (kind == TENSOR_LAYOUT_ENTRY) {
    if (node.empty() || !axis.empty() || begin != 0 || end != 0 || extent != 0 ||
        !sourceConv.empty() || !input.empty() || !output.empty() || !weight.empty() ||
        !bias.empty() || !downNode.empty() || !downWeight.empty() || !pads.empty() ||
        !strides.empty() || !dilations.empty())
      throw std::invalid_argument("source tensor layout entry has unexpected transform fields");
  }
  else {
    throw std::invalid_argument("source transform kind is unsupported");
  }
  if (!isAttention() &&
      (!attentionPorts.empty() || headWidth != 0 || !statistic.empty() || !globalStatistic.empty()))
    throw std::invalid_argument("non-attention source recipe has attention fields");
}

bool
NativeOnnxSourceTransformV1::isAttention() const
{
  return kind == "attention-qkv" || kind == "attention-heads" ||
         kind == "attention-source-component" || isRmsNorm();
}

bool
NativeOnnxSourceTransformV1::isRmsNorm() const
{
  return kind == "attention-rms-stat" || kind == "attention-rms-normalize";
}

bool
NativeOnnxSourceTransformV1::hasExplicitEntryPorts() const
{
  return isAttention() || kind == TENSOR_LAYOUT_ENTRY;
}

std::vector<NativeStepSlotSpec>
NativeOnnxSourceTransformV1::originalPorts(const std::vector<NativeStepSlotSpec>& slots,
                                          bool outputs) const
{
  auto original = slots;
  if (kind == "attention-source-component") return original;
  if (isRmsNorm()) {
    validate();
    const bool synthesizeStat = kind == "attention-rms-stat";
    const auto expected = outputs ? (synthesizeStat ? statistic : output) : input;
    if (!outputs && !synthesizeStat) {
      const auto global = std::find_if(original.begin(), original.end(), [&] (const auto& slot) {
        return slot.slotId == globalStatistic;
      });
      if (global == original.end() || global->shape.size() != 3 ||
          global->shape.back() != decltype(global->shape)::value_type{std::int64_t{1}})
        throw std::invalid_argument("source RMSNorm CALL has no per-token global statistic");
      original.erase(global);
    }
    const auto localWidth = static_cast<std::int64_t>((end - begin) * headWidth);
    const auto expectedWidth = outputs && synthesizeStat ? std::int64_t{1} : localWidth;
    if (original.size() != 1 || original.front().slotId != expected ||
        original.front().dtype != "float32" || original.front().shape.size() != 3 ||
        original.front().shape.back() != decltype(original.front().shape)::value_type{expectedWidth})
      throw std::invalid_argument("source RMSNorm CALL differs from the sealed phase and range");
    if (outputs) original.front().slotId = output;
    original.front().shape.back() = static_cast<std::int64_t>(extent * headWidth);
    return original;
  }
  if (!isAttention() || outputs != (kind == "attention-qkv")) return original;
  validate();
  std::size_t found = 0;
  for (auto& slot : original) {
    if (std::find(attentionPorts.begin(), attentionPorts.end(), slot.slotId) == attentionPorts.end())
      continue;
    ++found;
    const auto local = static_cast<std::int64_t>((end - begin) * headWidth);
    if (slot.dtype != "float32" || slot.shape.size() != 3 ||
        slot.shape.back() != decltype(slot.shape)::value_type{local})
      throw std::invalid_argument("source attention CALL port differs from the sealed head range");
    slot.shape.back() = static_cast<std::int64_t>(extent * headWidth);
  }
  if (found != attentionPorts.size())
    throw std::invalid_argument("source attention CALL omits a sealed Q/K/V port");
  return original;
}

std::string
NativeOnnxSourceTransformV1::canonicalJson() const
{
  auto value = NativeJson{{"schema", schema}, {"kind", kind}, {"node", node}};
  if (kind == "conv-input-channel-partial" || kind == "ffn-pair-partial") {
    value["axis"] = axis;
    value["input"] = input;
    value["weight"] = weight;
    value["output"] = output;
    value["begin"] = begin;
    value["end"] = end;
    value["extent"] = extent;
    if (!bias.empty()) value["bias"] = bias;
    if (!pads.empty()) value["pads"] = pads;
    if (!strides.empty()) value["strides"] = strides;
    if (!dilations.empty()) value["dilations"] = dilations;
    if (kind == "ffn-pair-partial") {
      value["downNode"] = downNode;
      value["downWeight"] = downWeight;
    }
  }
  else if (isAttention() && kind != "attention-source-component") {
    value["axis"] = axis;
    value["begin"] = begin;
    value["end"] = end;
    value["extent"] = extent;
    value["headWidth"] = headWidth;
    if (isRmsNorm()) {
      value["input"] = input;
      value["output"] = output;
      value["statistic"] = statistic;
      value["globalStatistic"] = globalStatistic;
    }
    else value["attentionPorts"] = attentionPorts;
  }
  else if (kind != "attention-source-component" && kind != TENSOR_LAYOUT_ENTRY) {
    value["sourceConv"] = sourceConv;
    value["bias"] = bias;
    value["partial"] = output;
  }
  return nativeCanonicalJson(value);
}

NativeOnnxSourceTransformV1
NativeOnnxSourceTransformV1::parse(const std::string& wire)
{
  const auto value = nativeParseJson(wire);
  if (!value.is_object())
    throw std::invalid_argument("source transform is not an object");
  NativeOnnxSourceTransformV1 result;
  result.schema = value.value("schema", std::string{});
  result.kind = value.value("kind", std::string{});
  result.node = value.value("node", std::string{});
  result.sourceConv = value.value("sourceConv", std::string{});
  result.axis = value.value("axis", std::string{});
  result.input = value.value("input", std::string{});
  result.weight = value.value("weight", std::string{});
  result.bias = value.value("bias", std::string{});
  // The wire name for a bias transform's partial port is `partial`; keep
  // accepting `output` so older records remain readable, then validate the
  // unified field.
  result.output = value.value("partial", value.value("output", std::string{}));
  result.begin = value.value("begin", std::uint64_t{0});
  result.end = value.value("end", std::uint64_t{0});
  result.extent = value.value("extent", std::uint64_t{0});
  result.pads = value.value("pads", std::vector<std::int64_t>{});
  result.strides = value.value("strides", std::vector<std::int64_t>{});
  result.dilations = value.value("dilations", std::vector<std::int64_t>{});
  result.attentionPorts = value.value("attentionPorts", std::vector<std::string>{});
  result.headWidth = value.value("headWidth", std::uint64_t{0});
  result.statistic = value.value("statistic", std::string{});
  result.globalStatistic = value.value("globalStatistic", std::string{});
  if (result.kind == "ffn-pair-partial") {
    result.downNode = value.value("downNode", std::string{});
    result.downWeight = value.value("downWeight", std::string{});
  }
  result.validate();
  if ((result.kind == "ffn-pair-partial" || result.hasExplicitEntryPorts()) &&
      result.canonicalJson() != nativeCanonicalJson(value))
    throw std::invalid_argument("source transform has unexpected fields");
  return result;
}

void
NativeOnnxSourceEntryProgramV1::validate() const
{
  if (entries.size() < 2)
    throw std::invalid_argument("source entry program requires multiple derived graphs");
  std::set<std::string> entryIds;
  std::set<std::string> sourceNodes;
  std::map<std::string, const NativeOnnxSourceEntryRecipeV1*> pendingStats;
  const bool tensorProgram = entries.front().transform.kind == TENSOR_LAYOUT_ENTRY;
  std::set<std::string> referencedPorts;
  for (const auto& entry : entries) {
    entry.transform.validate();
    if (entry.entryId.empty() || !entryIds.insert(entry.entryId).second)
      throw std::invalid_argument("source entry program has an invalid entry identity");
    const auto& transform = entry.transform;
    if (transform.hasExplicitEntryPorts()) {
      if (entry.sourceNodes.empty() ||
          std::find(entry.sourceNodes.begin(), entry.sourceNodes.end(), transform.node) ==
            entry.sourceNodes.end())
        throw std::invalid_argument("source graph entry does not cover its anchor");
      const auto check = [] (const auto& ports) {
        const std::set<std::string> unique(ports.begin(), ports.end());
        return !ports.empty() && unique.size() == ports.size() && unique.count("") == 0;
      };
      if (!check(entry.inputPorts) || !check(entry.outputPorts))
        throw std::invalid_argument("source graph entry has an incomplete graph ABI");
      if (transform.isRmsNorm() &&
          (entry.sourceNodes.size() != 6 || entry.sourceNodes.front() != transform.node))
        throw std::invalid_argument("source RMSNorm entry must cover its six original nodes");
    }
    else if (transform.kind != "ffn-pair-partial" ||
        entry.sourceNodes.size() != (transform.bias.empty() ? 3 : 4) ||
        entry.sourceNodes.front() != transform.node || entry.sourceNodes.back() != transform.downNode)
      throw std::invalid_argument("source entry program does not describe a complete FFN entry");
    if (!transform.hasExplicitEntryPorts() &&
        (!entry.inputPorts.empty() || !entry.outputPorts.empty()))
      throw std::invalid_argument("legacy FFN entry has a duplicate CALL ABI");
    if ((transform.kind == TENSOR_LAYOUT_ENTRY) != tensorProgram ||
        (tensorProgram ? !isDigest(entry.tensorBoundaryDigest) : !entry.tensorBoundaryDigest.empty()))
      throw std::invalid_argument("source entry has an invalid tensor boundary digest");
    if (tensorProgram) {
      referencedPorts.insert(entry.inputPorts.begin(), entry.inputPorts.end());
      referencedPorts.insert(entry.outputPorts.begin(), entry.outputPorts.end());
    }
    if (transform.kind == "attention-rms-stat") {
      if (!pendingStats.emplace(transform.node, &entry).second)
        throw std::invalid_argument("source RMSNorm repeats its statistics entry");
    }
    if (transform.kind == "attention-rms-normalize") {
      const auto localStats = pendingStats.find(transform.node);
      auto expected = transform;
      expected.kind = "attention-rms-stat";
      if (localStats == pendingStats.end() || localStats->second->sourceNodes != entry.sourceNodes ||
          localStats->second->transform.canonicalJson() != expected.canonicalJson())
        throw std::invalid_argument("source RMSNorm normalization has no matching source region");
      pendingStats.erase(localStats);
      continue;
    }
    for (const auto& node : entry.sourceNodes) {
      if (node.empty() || !sourceNodes.insert(node).second)
        throw std::invalid_argument("source entry program overlaps or omits a node identity");
    }
  }
  if (!pendingStats.empty())
    throw std::invalid_argument("source RMSNorm statistics have no normalization entry");
  if (tensorPorts.size() != referencedPorts.size())
    throw std::invalid_argument("source program tensor port inventory is incomplete or foreign");
  for (const auto& [slot, binding] : tensorPorts) {
    if (referencedPorts.count(slot) == 0 || slot != binding.slotId())
      throw std::invalid_argument("source program tensor port has a mismatched operand or slice");
  }
  if (tensorProgram == tensorLayouts.empty())
    throw std::invalid_argument("source program does not bind its operator layouts");
  std::set<std::string> layoutOutputs;
  for (const auto& layout : tensorLayouts) {
    if ((layout.convolution || !layout.concatPieces.empty()) && layout.outputs.size() != 1)
      throw std::invalid_argument("source operator layout has an invalid output count");
    if (layout.convolution && !layout.concatPieces.empty())
      throw std::invalid_argument("source operator layout mixes Conv and Concat");
    for (const auto& [name, split] : layout.outputs) {
      if (name.empty() || split.split == 0 || !layoutOutputs.insert(name).second)
        throw std::invalid_argument("source operator layout has an invalid tensor partition");
    }
    if (layout.convolution) {
      for (const auto& shard : *layout.convolution) {
        if (shard.inputBegin >= shard.inputEnd || shard.outputBegin >= shard.outputEnd ||
            shard.groups == 0)
          throw std::invalid_argument("source Conv layout has an invalid shard");
      }
    }
    for (const auto& piece : layout.concatPieces) {
      if (piece.producer >= 2 || piece.consumer >= 2 || piece.begin >= piece.end)
        throw std::invalid_argument("source Concat layout has an invalid piece");
    }
  }
}

void
NativeOnnxSourceEntryProgramV1::validateRank(const NativeExecutionPlan& plan,
                                            const std::string& role, std::uint64_t rank) const
{
  validate();
  const auto stage = std::find_if(plan.stageLayout.begin(), plan.stageLayout.end(),
    [&role] (const auto& candidate) {
      return std::find(candidate.unitRoleIds.begin(), candidate.unitRoleIds.end(), role) !=
             candidate.unitRoleIds.end();
    });
  if (plan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 ||
      stage == plan.stageLayout.end() || stage->degree() != 2 || rank >= 2 ||
      stage->unitRoleIds[rank] != role)
    throw std::invalid_argument("source entry program requires a bound degree-two V2 rank");
  if (!tensorLayouts.empty()) {
    std::vector<std::string> expected;
    for (std::size_t ordinal = 0; ordinal < tensorLayouts.size(); ++ordinal) {
      if (rank == 0 || !tensorLayouts[ordinal].outputs.empty())
        expected.push_back("onnx-node-" + std::to_string(ordinal));
    }
    if (expected != sourceNodeCover())
      throw std::invalid_argument("source operator layouts differ from the sealed rank cover");
  }

  const auto findStep = [&] (const std::string& id) -> const NativeExecutionStepSpec* {
    const auto found = std::find_if(plan.steps.begin(), plan.steps.end(),
      [&] (const auto& step) { return step.stepId == id; });
    return found == plan.steps.end() ? nullptr : &*found;
  };
  const auto sameTensor = [] (const auto& left, const auto& right) {
    return left.dtype == right.dtype && left.shape == right.shape &&
           left.layoutDigest == right.layoutDigest;
  };
  const auto checkRankSum = [&] (const auto& sum, const auto& localCall,
                                const auto& resultName, std::size_t consumerRank) {
    const auto& partial = localCall.outputSlots.front();
    if (sum.operation != "SUM" || sum.unitId != stage->unitRoleIds[consumerRank] ||
        sum.inputSlots.size() != 2 || sum.outputSlots.size() != 1 ||
        sum.outputSlots.front().slotId != resultName ||
        !sameTensor(sum.outputSlots.front(), partial))
      throw std::invalid_argument("source entry does not feed its sealed rank SUM");
    for (std::size_t index = 0; index < 2; ++index) {
      const auto& input = sum.inputSlots[index];
      const auto* producer = findStep(input.producerStepId);
      if (input.producerSlotId != partial.slotId || !sameTensor(input, partial) ||
          input.source != (index == consumerRank ? "LOCAL_STEP" : "REMOTE_STEP") ||
          producer == nullptr || producer->unitId != stage->unitRoleIds[index] ||
          producer->operation != "CALL_SUBGRAPH" || producer->outputSlots.size() != 1 ||
          producer->outputSlots.front().slotId != partial.slotId ||
          !sameTensor(producer->outputSlots.front(), partial) ||
          (index == rank && producer->stepId != localCall.stepId))
        throw std::invalid_argument("source SUM operand differs from its sealed rank CALL");
    }
  };

  std::size_t next = 0;
  std::map<std::string, const NativeExecutionStepSpec*> statSteps;
  for (const auto& step : plan.steps) {
    if (step.unitId != role || step.operation == "SUM") continue;
    if (step.operation != "CALL_SUBGRAPH" || next == entries.size())
      throw std::invalid_argument("source entry program does not cover the rank CALLs");
    const auto& entry = entries[next++];
    const auto& transform = entry.transform;
    if (transform.kind != "attention-source-component" && transform.kind != TENSOR_LAYOUT_ENTRY) {
      const auto ranges = nativeConvInputChannelRanges(transform.extent, 2);
      if (ranges[rank] != std::make_pair(transform.begin, transform.end))
        throw std::invalid_argument("source entry program range differs from the rank");
    }
    if (step.artifactEntry != entry.entryId || !step.localStateSlots.empty() ||
        step.inputSlots.empty() || step.outputSlots.empty())
      throw std::invalid_argument("source entry program does not bind the ordered CALL ports");
    if (transform.hasExplicitEntryPorts()) {
      const auto matches = [] (const auto& names, const auto& slots) {
        return std::equal(names.begin(), names.end(), slots.begin(), slots.end(),
                          [] (const auto& name, const auto& slot) { return name == slot.slotId; });
      };
      if (!matches(entry.inputPorts, step.inputSlots) ||
          !matches(entry.outputPorts, step.outputSlots))
        throw std::invalid_argument("source graph ABI differs from its sealed CALL");
      if (transform.kind == TENSOR_LAYOUT_ENTRY) {
        const auto [begin, end] = step.sourceLayers;
        if (plan.stageLayout.size() != 1 || end <= begin ||
            entry.sourceNodes.size() != end - begin ||
            entry.tensorBoundaryDigest != computeTensorBoundaryDigest(step))
          throw std::invalid_argument("source tensor entry differs from its sealed CALL");
        for (std::size_t index = 0; index < entry.sourceNodes.size(); ++index) {
          if (entry.sourceNodes[index] != "onnx-node-" + std::to_string(begin + index))
            throw std::invalid_argument("source tensor entry differs from its sealed ordinal");
        }
        for (const auto* slots : {&step.inputSlots, &step.outputSlots}) {
          for (const auto& slot : *slots) {
            if (slot.layoutDigest != tensorPorts.at(slot.slotId).layoutDigest(
                  stage->sourceRegionDigest))
              throw std::invalid_argument("source tensor entry layout differs from its source graph");
          }
        }
        continue;
      }
      for (const auto& input : step.inputSlots) {
        if ((input.source == "LOCAL_STEP" || input.source == "REMOTE_STEP") &&
            input.producerSlotId != input.slotId)
          throw std::invalid_argument("source attention input differs from its producer SSA port");
      }
      if (transform.kind != "attention-source-component" &&
          (transform.kind == "attention-qkv" ? step.outputSlots.size() != 3 :
                                               step.outputSlots.size() != 1))
        throw std::invalid_argument("source attention CALL output inventory is invalid");
      const auto inputs = transform.originalPorts(step.inputSlots, false);
      const auto outputs = transform.originalPorts(step.outputSlots, true);
      if (transform.isRmsNorm()) {
        if (inputs.front().shape != outputs.front().shape)
          throw std::invalid_argument("source RMSNorm changed its token geometry");
        if (transform.kind == "attention-rms-stat") {
          statSteps.emplace(transform.node, &step);
        }
        else {
          const auto global = std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
            [&] (const auto& slot) { return slot.slotId == transform.globalStatistic; });
          const auto local = std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
            [&] (const auto& slot) { return slot.slotId == transform.input; });
          const auto& stats = *statSteps.at(transform.node);
          const auto& statInput = stats.inputSlots.front();
          if (local->source != statInput.source || local->producerStepId != statInput.producerStepId ||
              local->producerSlotId != statInput.producerSlotId || !sameTensor(*local, statInput))
            throw std::invalid_argument("source RMSNorm statistical and normalized inputs differ");
          const auto* sum = findStep(global->producerStepId);
          if (!sameTensor(*global, stats.outputSlots.front()) || global->source != "LOCAL_STEP" ||
              global->producerSlotId != transform.globalStatistic || sum == nullptr)
            throw std::invalid_argument("source RMSNorm does not consume its sealed global SUM");
          checkRankSum(*sum, stats, transform.globalStatistic, rank);
        }
      }
      else if (transform.kind == "attention-heads") {
        const auto& partial = step.outputSlots.front().slotId;
        std::set<std::string> reducers;
        for (const auto& consumer : plan.steps) {
          const auto referencesPartial = std::any_of(consumer.inputSlots.begin(),
            consumer.inputSlots.end(), [&] (const auto& slot) {
              return slot.producerStepId == step.stepId && slot.producerSlotId == partial;
            });
          if (!referencesPartial) continue;
          const auto owner = std::find(stage->unitRoleIds.begin(), stage->unitRoleIds.end(),
                                       consumer.unitId);
          if (owner == stage->unitRoleIds.end() || !reducers.insert(consumer.unitId).second)
            throw std::invalid_argument("attention partial has an unexpected consumer");
          checkRankSum(consumer, step, partial, owner - stage->unitRoleIds.begin());
        }
        if (reducers.size() != 2)
          throw std::invalid_argument("attention partial requires one SUM on each rank");
      }
    }
    else if (step.inputSlots.size() != 1 || step.inputSlots.front().slotId != transform.input ||
             step.outputSlots.size() != 1 || step.outputSlots.front().slotId != transform.output)
      throw std::invalid_argument("source entry program does not bind the ordered CALL ports");
    for (const auto* ports : {&step.inputSlots, &step.outputSlots}) {
      for (const auto& port : *ports) {
        if (port.dtype != "float32" || port.shape.empty() ||
            std::any_of(port.shape.begin(), port.shape.end(), [] (const auto& dim) {
            const auto* value = std::get_if<std::int64_t>(&dim);
            return value == nullptr || *value <= 0;
          }))
          throw std::invalid_argument("source entry program requires exact static FP32 CALL ports");
      }
    }
  }
  if (next != entries.size())
    throw std::invalid_argument("source entry program carries a graph with no CALL");
}

std::string
NativeOnnxSourceEntryProgramV1::canonicalJson() const
{
  auto values = NativeJson::array();
  for (const auto& entry : entries) {
    auto value = NativeJson{{"entryId", entry.entryId}, {"sourceNodes", entry.sourceNodes},
                            {"transform", nativeParseJson(entry.transform.canonicalJson())}};
    if (entry.transform.hasExplicitEntryPorts()) {
      value["inputPorts"] = entry.inputPorts;
      value["outputPorts"] = entry.outputPorts;
    }
    if (!entry.tensorBoundaryDigest.empty())
      value["tensorBoundaryDigest"] = entry.tensorBoundaryDigest;
    values.push_back(std::move(value));
  }
  NativeJson result{{"schema", NATIVE_SOURCE_ENTRY_PROGRAM_V1}, {"entries", std::move(values)}};
  if (!tensorPorts.empty()) {
    auto& ports = result["tensorPorts"] = NativeJson::object();
    for (const auto& [slot, binding] : tensorPorts) {
      auto& port = ports[slot] = NativeJson{{"source", binding.sourceName}};
      if (binding.range)
        port["slice"] = {binding.range->axis, binding.range->begin, binding.range->end};
    }
  }
  if (!tensorLayouts.empty()) {
    auto& layouts = result["tensorLayouts"] = NativeJson::array();
    for (const auto& layout : tensorLayouts) {
      auto& value = layouts.emplace_back(NativeJson::object());
      if (!layout.outputs.empty()) {
        auto& outputs = value["outputs"] = NativeJson::object();
        for (const auto& [name, split] : layout.outputs)
          outputs[name] = {split.axis, split.split};
      }
      if (layout.convolution) {
        auto& conv = value["conv"] = NativeJson::array();
        for (const auto& shard : *layout.convolution)
          conv.push_back({shard.inputBegin, shard.inputEnd, shard.outputBegin,
                          shard.outputEnd, shard.groups});
      }
      if (!layout.concatPieces.empty()) {
        auto& concat = value["concat"] = NativeJson::array();
        for (const auto& piece : layout.concatPieces)
          concat.push_back({piece.input, piece.producer, piece.consumer,
                            piece.begin, piece.end, piece.outputBegin});
      }
    }
  }
  return nativeCanonicalJson(result);
}

std::vector<std::vector<std::uint64_t>>
NativeOnnxSourceEntryProgramV1::bindSourceOrdinals(
  const std::vector<std::uint64_t>& roleOrdinals) const
{
  std::vector<std::vector<std::uint64_t>> windows;
  windows.reserve(entries.size());
  validate();
  std::map<std::string, std::size_t> statistics;
  std::size_t next = 0;
  for (const auto& entry : entries) {
    if (entry.transform.kind == "attention-rms-normalize") {
      windows.push_back(windows.at(statistics.at(entry.transform.node)));
      continue;
    }
    if (entry.transform.kind == "attention-rms-stat")
      statistics.emplace(entry.transform.node, windows.size());
    auto& window = windows.emplace_back();
    window.reserve(entry.sourceNodes.size());
    for (const auto& node : entry.sourceNodes) {
      if (next == roleOrdinals.size() ||
          node != "onnx-node-" + std::to_string(roleOrdinals[next]) ||
          (next != 0 && (roleOrdinals[next] <= roleOrdinals[next - 1] ||
                        ((entry.transform.kind != TENSOR_LAYOUT_ENTRY || !window.empty()) &&
                         roleOrdinals[next] - roleOrdinals[next - 1] != 1))))
        throw std::invalid_argument("source entry ordinals differ from the sealed role");
      window.push_back(roleOrdinals[next++]);
    }
  }
  if (next != roleOrdinals.size())
    throw std::invalid_argument("source entry program leaves sealed source nodes uncovered");
  return windows;
}

std::vector<std::string>
NativeOnnxSourceEntryProgramV1::sourceNodeCover() const
{
  std::vector<std::string> covered;
  for (const auto& entry : entries) {
    if (entry.transform.kind != "attention-rms-normalize")
      covered.insert(covered.end(), entry.sourceNodes.begin(), entry.sourceNodes.end());
  }
  return covered;
}

NativeOnnxSourceEntryProgramV1
NativeOnnxSourceEntryProgramV1::parse(const std::string& wire)
{
  const auto value = nativeParseJson(wire);
  if (!value.is_object() || value.value("schema", std::string{}) != NATIVE_SOURCE_ENTRY_PROGRAM_V1 ||
      !value.contains("entries") || !value.at("entries").is_array())
    throw std::invalid_argument("source entry program schema is unsupported");
  NativeOnnxSourceEntryProgramV1 program;
  for (const auto& entry : value.at("entries")) {
    program.entries.push_back({entry.at("entryId").get<std::string>(),
      entry.at("sourceNodes").get<std::vector<std::string>>(),
      NativeOnnxSourceTransformV1::parse(nativeCanonicalJson(entry.at("transform"))),
      entry.value("inputPorts", std::vector<std::string>{}),
      entry.value("outputPorts", std::vector<std::string>{}),
      entry.value("tensorBoundaryDigest", std::string{})});
  }
  if (value.contains("tensorPorts")) {
    const auto& ports = value.at("tensorPorts");
    if (!ports.is_object())
      throw std::invalid_argument("source program tensor ports must be an object");
    for (auto item = ports.begin(); item != ports.end(); ++item) {
      NativeOnnxEntryPort binding{item->at("source").get<std::string>(), {}};
      if (item->contains("slice")) {
        const auto& range = item->at("slice");
        if (!range.is_array() || range.size() != 3 ||
            !std::all_of(range.begin(), range.end(), [] (const auto& n) {
              return n.is_number_unsigned();
            }))
          throw std::invalid_argument("source program tensor port has an invalid slice");
        binding.range = NativeOnnxTransferRange{range[0].get<std::size_t>(),
          range[1].get<std::uint64_t>(), range[2].get<std::uint64_t>()};
      }
      program.tensorPorts.emplace(item.key(), std::move(binding));
    }
  }
  if (value.contains("tensorLayouts")) {
    const auto& layouts = value.at("tensorLayouts");
    if (!layouts.is_array())
      throw std::invalid_argument("source operator layouts must be an array");
    for (const auto& row : layouts) {
      auto& layout = program.tensorLayouts.emplace_back();
      if (row.contains("outputs")) {
        for (const auto& [name, coordinates] : row.at("outputs").items()) {
          const auto axes = coordinates.get<std::array<std::uint64_t, 2>>();
          layout.outputs.emplace(name, NativeOnnxTwoRankLayout{axes[0], axes[1]});
        }
      }
      if (row.contains("conv")) {
        const auto shards = row.at("conv").get<std::array<std::array<std::uint64_t, 5>, 2>>();
        layout.convolution.emplace();
        for (std::size_t rank = 0; rank < shards.size(); ++rank) {
          const auto& v = shards[rank];
          layout.convolution->at(rank) = {v[0], v[1], v[2], v[3], v[4]};
        }
      }
      if (row.contains("concat")) {
        for (const auto& piece : row.at("concat")) {
          const auto v = piece.get<std::array<std::uint64_t, 6>>();
          layout.concatPieces.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
        }
      }
    }
  }
  program.validate();
  if (program.canonicalJson() != nativeCanonicalJson(value))
    throw std::invalid_argument("source entry program has unexpected fields");
  return program;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
nativeConvInputChannelRanges(std::uint64_t extent, std::uint64_t degree)
{
  if (extent < 2 || degree == 0 || degree > extent)
    throw std::invalid_argument("Conv channel ranges are not admissible");
  std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
  const auto base = extent / degree;
  const auto remainder = extent % degree;
  std::uint64_t cursor = 0;
  for (std::uint64_t rank = 0; rank < degree; ++rank) {
    const auto width = base + (rank < remainder ? 1 : 0);
    result.emplace_back(cursor, cursor + width);
    cursor += width;
  }
  return result;
}

} // namespace ndnsf::di
