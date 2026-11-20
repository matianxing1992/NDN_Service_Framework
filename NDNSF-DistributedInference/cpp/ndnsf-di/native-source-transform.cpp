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
    if (node.empty() || bias.empty() || output.empty())
      throw std::invalid_argument("conv-bias transform is incomplete");
  }
  else {
    throw std::invalid_argument("source transform kind is unsupported");
  }
}

std::string
NativeOnnxSourceTransformV1::canonicalJson() const
{
  auto value = NativeJson{{"schema", schema}, {"kind", kind}, {"node", node}};
  if (kind == "conv-input-channel-partial") {
    value["axis"] = axis;
    value["input"] = input;
    value["weight"] = weight;
    value["output"] = output;
    value["begin"] = begin;
    value["end"] = end;
    value["extent"] = extent;
    if (!pads.empty()) value["pads"] = pads;
    if (!strides.empty()) value["strides"] = strides;
    if (!dilations.empty()) value["dilations"] = dilations;
  }
  else {
    value["sourceConv"] = node;
    value["node"] = node;
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
  result.axis = value.value("axis", std::string{});
  result.input = value.value("input", std::string{});
  result.weight = value.value("weight", std::string{});
  result.bias = value.value("bias", std::string{});
  result.output = value.value("output", std::string{});
  result.begin = value.value("begin", std::uint64_t{0});
  result.end = value.value("end", std::uint64_t{0});
  result.extent = value.value("extent", std::uint64_t{0});
  result.pads = value.value("pads", std::vector<std::int64_t>{});
  result.strides = value.value("strides", std::vector<std::int64_t>{});
  result.dilations = value.value("dilations", std::vector<std::int64_t>{});
  result.validate();
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
