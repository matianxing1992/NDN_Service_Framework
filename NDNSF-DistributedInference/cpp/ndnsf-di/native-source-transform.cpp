#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

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
