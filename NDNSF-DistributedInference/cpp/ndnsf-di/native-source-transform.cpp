#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di {

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
  else {
    throw std::invalid_argument("source transform kind is unsupported");
  }
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
  else {
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
  if (result.kind == "ffn-pair-partial") {
    result.downNode = value.value("downNode", std::string{});
    result.downWeight = value.value("downWeight", std::string{});
  }
  result.validate();
  if (result.kind == "ffn-pair-partial" &&
      result.canonicalJson() != nativeCanonicalJson(value))
    throw std::invalid_argument("paired FFN transform has unexpected fields");
  return result;
}

void
NativeOnnxSourceEntryProgramV1::validate() const
{
  if (entries.size() < 2)
    throw std::invalid_argument("source entry program requires multiple derived graphs");
  std::set<std::string> entryIds;
  std::set<std::string> sourceNodes;
  for (const auto& entry : entries) {
    entry.transform.validate();
    if (entry.entryId.empty() || !entryIds.insert(entry.entryId).second)
      throw std::invalid_argument("source entry program has an invalid entry identity");
    const auto& transform = entry.transform;
    if (transform.kind != "ffn-pair-partial" ||
        entry.sourceNodes.size() != (transform.bias.empty() ? 3 : 4) ||
        entry.sourceNodes.front() != transform.node || entry.sourceNodes.back() != transform.downNode)
      throw std::invalid_argument("source entry program does not describe a complete FFN entry");
    for (const auto& node : entry.sourceNodes) {
      if (node.empty() || !sourceNodes.insert(node).second)
        throw std::invalid_argument("source entry program overlaps or omits a node identity");
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

  std::size_t next = 0;
  for (const auto& step : plan.steps) {
    if (step.unitId != role || step.operation == "SUM") continue;
    if (step.operation != "CALL_SUBGRAPH" || next == entries.size())
      throw std::invalid_argument("source entry program does not cover the rank CALLs");
    const auto& entry = entries[next++];
    const auto& transform = entry.transform;
    const auto ranges = nativeConvInputChannelRanges(transform.extent, 2);
    if (ranges[rank] != std::make_pair(transform.begin, transform.end))
      throw std::invalid_argument("source entry program range differs from the rank");
    if (step.artifactEntry != entry.entryId || !step.localStateSlots.empty() ||
        step.inputSlots.size() != 1 || step.inputSlots.front().slotId != transform.input ||
        step.outputSlots.size() != 1 || step.outputSlots.front().slotId != transform.output)
      throw std::invalid_argument("source entry program does not bind the ordered CALL ports");
    for (const auto* port : {&step.inputSlots.front(), &step.outputSlots.front()}) {
      if (port->dtype != "float32" || port->shape.empty() ||
          std::any_of(port->shape.begin(), port->shape.end(), [] (const auto& dim) {
            const auto* value = std::get_if<std::int64_t>(&dim);
            return value == nullptr || *value <= 0;
          }))
        throw std::invalid_argument("source entry program requires exact static FP32 CALL ports");
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
    values.push_back({{"entryId", entry.entryId}, {"sourceNodes", entry.sourceNodes},
                      {"transform", nativeParseJson(entry.transform.canonicalJson())}});
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
  std::size_t next = 0;
  for (const auto& entry : entries) {
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
      NativeOnnxSourceTransformV1::parse(nativeCanonicalJson(entry.at("transform")))});
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
