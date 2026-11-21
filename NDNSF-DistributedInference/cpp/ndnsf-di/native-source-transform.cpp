#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace ndnsf::di {

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
  else if (kind != "attention-source-component") {
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
  if ((result.kind == "ffn-pair-partial" || result.isAttention()) &&
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
  for (const auto& entry : entries) {
    entry.transform.validate();
    if (entry.entryId.empty() || !entryIds.insert(entry.entryId).second)
      throw std::invalid_argument("source entry program has an invalid entry identity");
    const auto& transform = entry.transform;
    if (transform.isAttention()) {
      if (entry.sourceNodes.empty() ||
          std::find(entry.sourceNodes.begin(), entry.sourceNodes.end(), transform.node) ==
            entry.sourceNodes.end())
        throw std::invalid_argument("source attention entry does not cover its anchor");
      const auto check = [] (const auto& ports) {
        const std::set<std::string> unique(ports.begin(), ports.end());
        return !ports.empty() && unique.size() == ports.size() && unique.count("") == 0;
      };
      if (!check(entry.inputPorts) || !check(entry.outputPorts))
        throw std::invalid_argument("source attention entry has an incomplete graph ABI");
      if (transform.isRmsNorm() &&
          (entry.sourceNodes.size() != 6 || entry.sourceNodes.front() != transform.node))
        throw std::invalid_argument("source RMSNorm entry must cover its six original nodes");
    }
    else if (transform.kind != "ffn-pair-partial" ||
        entry.sourceNodes.size() != (transform.bias.empty() ? 3 : 4) ||
        entry.sourceNodes.front() != transform.node || entry.sourceNodes.back() != transform.downNode)
      throw std::invalid_argument("source entry program does not describe a complete FFN entry");
    if (!transform.isAttention() && (!entry.inputPorts.empty() || !entry.outputPorts.empty()))
      throw std::invalid_argument("legacy FFN entry has a duplicate CALL ABI");
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
    if (transform.kind != "attention-source-component") {
      const auto ranges = nativeConvInputChannelRanges(transform.extent, 2);
      if (ranges[rank] != std::make_pair(transform.begin, transform.end))
        throw std::invalid_argument("source entry program range differs from the rank");
    }
    if (step.artifactEntry != entry.entryId || !step.localStateSlots.empty() ||
        step.inputSlots.empty() || step.outputSlots.empty())
      throw std::invalid_argument("source entry program does not bind the ordered CALL ports");
    if (transform.isAttention()) {
      const auto matches = [] (const auto& names, const auto& slots) {
        return std::equal(names.begin(), names.end(), slots.begin(), slots.end(),
                          [] (const auto& name, const auto& slot) { return name == slot.slotId; });
      };
      if (!matches(entry.inputPorts, step.inputSlots) ||
          !matches(entry.outputPorts, step.outputSlots))
        throw std::invalid_argument("source attention graph ABI differs from its sealed CALL");
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
    if (entry.transform.isAttention()) {
      value["inputPorts"] = entry.inputPorts;
      value["outputPorts"] = entry.outputPorts;
    }
    values.push_back(std::move(value));
  }
  return nativeCanonicalJson({{"schema", NATIVE_SOURCE_ENTRY_PROGRAM_V1},
                             {"entries", std::move(values)}});
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
                        roleOrdinals[next] - roleOrdinals[next - 1] != 1)))
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
      entry.value("outputPorts", std::vector<std::string>{})});
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
