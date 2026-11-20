#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"

#include <onnx/checker.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ndnsf::di {
namespace {

onnx::NodeProto*
findNode(onnx::ModelProto& model, const std::string& name)
{
  for (auto& node : *model.mutable_graph()->mutable_node())
    if (node.name() == name) return &node;
  return nullptr;
}

onnx::TensorProto*
findInitializer(onnx::ModelProto& model, const std::string& name)
{
  for (auto& initializer : *model.mutable_graph()->mutable_initializer())
    if (initializer.name() == name) return &initializer;
  return nullptr;
}

std::uint64_t
elementBytes(int dataType)
{
  if (dataType == onnx::TensorProto::FLOAT) return 4;
  throw std::invalid_argument("source transform supports float32 initializers only");
}

std::uint64_t
countOf(const onnx::TensorProto& tensor)
{
  std::uint64_t count = 1;
  for (const auto dim : tensor.dims()) {
    if (dim < 0) throw std::invalid_argument("source transform initializer is dynamic");
    count *= static_cast<std::uint64_t>(dim);
  }
  return count;
}

std::vector<std::uint8_t>
rawBytes(const onnx::TensorProto& tensor, const NativeOnnxExternalReader& external)
{
  const auto bytes = countOf(tensor) * elementBytes(tensor.data_type());
  if (tensor.data_location() == onnx::TensorProto::EXTERNAL || !tensor.external_data().empty()) {
    std::string location, offset, length;
    for (const auto& entry : tensor.external_data()) {
      if (entry.key() == "location") location = entry.value();
      else if (entry.key() == "offset") offset = entry.value();
      else if (entry.key() == "length") length = entry.value();
    }
    if (location.empty() || !external)
      throw std::invalid_argument("source transform requires external initializer bytes");
    const auto start = offset.empty() ? 0 : std::stoull(offset);
    const auto want = length.empty() ? bytes : std::stoull(length);
    auto result = external(location, start, want);
    if (result.size() != bytes)
      throw std::invalid_argument("source transform external initializer length differs");
    return result;
  }
  if (!tensor.raw_data().empty()) {
    const auto& raw = tensor.raw_data();
    return {raw.begin(), raw.end()};
  }
  if (tensor.float_data_size() != 0) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(bytes));
    for (int i = 0; i < tensor.float_data_size(); ++i) {
      const auto value = tensor.float_data(i);
      std::memcpy(result.data() + static_cast<std::size_t>(i) * 4, &value, 4);
    }
    return result;
  }
  throw std::invalid_argument("source transform initializer has no bytes");
}

void
setInlineBytes(onnx::TensorProto& tensor, std::vector<std::uint8_t> bytes)
{
  tensor.clear_external_data();
  tensor.set_data_location(onnx::TensorProto::DEFAULT);
  tensor.clear_raw_data();
  tensor.clear_float_data();
  tensor.set_raw_data(bytes.data(), bytes.size());
}

bool
hasExternalInitializers(const onnx::ModelProto& model)
{
  for (const auto& initializer : model.graph().initializer())
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL ||
        !initializer.external_data().empty())
      return true;
  return false;
}

void
setInts(onnx::NodeProto& node, const char* name, const std::vector<std::int64_t>& values)
{
  for (int index = 0; index < node.attribute_size(); ++index) {
    if (node.attribute(index).name() != name) continue;
    auto* attribute = node.mutable_attribute(index);
    attribute->Clear();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::INTS);
    for (const auto value : values) attribute->add_ints(value);
    return;
  }
  auto* attribute = node.add_attribute();
  attribute->set_name(name);
  attribute->set_type(onnx::AttributeProto::INTS);
  for (const auto value : values) attribute->add_ints(value);
}

} // namespace

onnx::ModelProto
applyNativeOnnxSourceTransform(const onnx::ModelProto& source,
                               const NativeOnnxSourceTransformV1& transform,
                               const NativeOnnxExternalReader& external)
{
  transform.validate();
  onnx::ModelProto result = source;
  auto* graph = result.mutable_graph();
  if (graph->name().empty()) graph->set_name("source-transform");

  if (transform.kind == "conv-input-channel-partial") {
    auto* conv = findNode(result, transform.node);
    if (conv == nullptr || conv->op_type() != "Conv")
      throw std::invalid_argument("source transform Conv node is missing");
    if (conv->input_size() < 2 || conv->input(0) != transform.input)
      throw std::invalid_argument("source transform Conv input differs");
    if (conv->output_size() != 1 || conv->output(0) != transform.output)
      throw std::invalid_argument("source transform Conv output differs");
    auto* weight = findInitializer(result, transform.weight);
    if (weight == nullptr || weight->dims_size() != 4)
      throw std::invalid_argument("source transform Conv weight is missing or not 4-D");
    if (static_cast<std::uint64_t>(weight->dims(1)) != transform.extent)
      throw std::invalid_argument("source transform extent differs from the Conv weight");
    const auto inChannels = static_cast<std::uint64_t>(weight->dims(1));
    if (transform.end > inChannels || transform.begin >= transform.end)
      throw std::invalid_argument("source transform channel range is outside the weight");

    // Insert the rank-local channel slice immediately before its Conv.
    const std::string prefix = transform.node + "/source-transform/";
    const std::string startsName = prefix + "starts";
    const std::string endsName = prefix + "ends";
    const std::string axesName = prefix + "axes";
    const std::string sliceName = prefix + "input";
    auto addScalar = [&] (const std::string& name, std::int64_t value) {
      auto* initializer = graph->add_initializer();
      initializer->set_name(name);
      initializer->set_data_type(onnx::TensorProto::INT64);
      initializer->add_dims(1);
      initializer->add_int64_data(value);
    };
    addScalar(startsName, static_cast<std::int64_t>(transform.begin));
    addScalar(endsName, static_cast<std::int64_t>(transform.end));
    addScalar(axesName, 1);

    onnx::NodeProto slice;
    slice.set_name(prefix + "slice");
    slice.set_op_type("Slice");
    slice.add_input(transform.input);
    slice.add_input(startsName);
    slice.add_input(endsName);
    slice.add_input(axesName);
    slice.add_output(sliceName);

    std::vector<onnx::NodeProto> nodes;
    nodes.reserve(graph->node_size() + 1);
    bool inserted = false;
    for (const auto& node : graph->node()) {
      if (node.name() == transform.node && !inserted) {
        nodes.push_back(slice);
        inserted = true;
      }
      nodes.push_back(node);
    }
    if (!inserted) throw std::invalid_argument("source transform Conv node was not copied");
    graph->clear_node();
    for (auto& node : nodes) *graph->add_node() = std::move(node);

    conv = findNode(result, transform.node);
    const auto biasName = conv->input_size() > 2 ? conv->input(2) : std::string{};
    conv->set_input(0, sliceName);
    if (!biasName.empty()) {
      conv->clear_input();
      conv->add_input(sliceName);
      conv->add_input(transform.weight);
    }
    if (!transform.pads.empty()) setInts(*conv, "pads", transform.pads);
    if (!transform.strides.empty()) setInts(*conv, "strides", transform.strides);
    if (!transform.dilations.empty()) setInts(*conv, "dilations", transform.dilations);

    // Slice the weight along the input-channel axis (axis 1) into rank bytes.
    weight = findInitializer(result, transform.weight);
    auto full = rawBytes(*weight, external);
    const auto channels = static_cast<std::uint64_t>(weight->dims(1));
    const auto kernel = static_cast<std::uint64_t>(weight->dims(2)) *
                        static_cast<std::uint64_t>(weight->dims(3));
    const auto outChannels = static_cast<std::uint64_t>(weight->dims(0));
    const auto width = transform.end - transform.begin;
    const auto block = kernel * 4;
    std::vector<std::uint8_t> sliced(static_cast<std::size_t>(outChannels * width * block));
    for (std::uint64_t channel = 0; channel < outChannels; ++channel) {
      const auto base = (channel * channels + transform.begin) * block;
      std::memcpy(sliced.data() + static_cast<std::size_t>(channel * width * block),
                  full.data() + static_cast<std::size_t>(base),
                  static_cast<std::size_t>(width * block));
    }
    weight->set_dims(1, static_cast<std::int64_t>(width));
    setInlineBytes(*weight, std::move(sliced));
    return result;
  }

  if (transform.kind == "add-conv-bias") {
    auto* bias = findInitializer(result, transform.bias);
    if (bias == nullptr)
      throw std::invalid_argument("source transform bias initializer is missing");
    if (bias->dims_size() == 1) {
      // A fused Conv bias is commonly exported as [C]. Reshape it in place to
      // the [1,C,1,1] broadcast Add needs; the bytes are unchanged.
      const auto channels = bias->dims(0);
      bias->clear_dims();
      bias->add_dims(1);
      bias->add_dims(channels);
      bias->add_dims(1);
      bias->add_dims(1);
    }
    if (bias->dims_size() != 4 || bias->dims(0) != 1)
      throw std::invalid_argument("source transform bias is not [1,C,1,1]");
    // A source graph that already has an explicit bias Add needs no rewrite;
    // only a fused-bias Conv export requires the synthesized Add.
    for (const auto& node : graph->node()) {
      if (node.op_type() == "Add" && node.input_size() == 2 &&
          node.input(0) == transform.output && node.input(1) == transform.bias)
        return result;
    }
    const std::string rewritten = transform.output + "/bias-added";
    onnx::NodeProto add;
    add.set_name(transform.node + "/source-transform/bias");
    add.set_op_type("Add");
    add.add_input(transform.output);
    add.add_input(transform.bias);
    add.add_output(rewritten);
    // Insert immediately before the first consumer so the derived graph stays
    // topologically sorted; appending it last would place the producer after
    // its readers and ONNX rejects the model.
    std::vector<onnx::NodeProto> nodes;
    nodes.reserve(graph->node_size() + 1);
    bool inserted = false;
    for (const auto& node : graph->node()) {
      bool consumes = false;
      for (int input = 0; input < node.input_size(); ++input)
        if (node.input(input) == transform.output) consumes = true;
      if (consumes && !inserted) {
        nodes.push_back(add);
        inserted = true;
      }
      auto copy = node;
      for (int input = 0; input < copy.input_size(); ++input)
        if (copy.input(input) == transform.output) copy.set_input(input, rewritten);
      nodes.push_back(std::move(copy));
    }
    if (!inserted) throw std::invalid_argument(
      "source transform bias has no consumer to precede");
    graph->clear_node();
    for (auto& node : nodes) *graph->add_node() = std::move(node);
    for (auto& output : *graph->mutable_output())
      if (output.name() == transform.output) output.set_name(rewritten);
    // Do not rename the source value_info: `transform.output` may be a graph
    // input, in which case the synthesized Add still consumes that exact name.
    // External initializers live in the protected sidecar, not on disk at the
    // path their metadata names. The structural rewrite above is complete; a
    // checker that insists on loading external bytes would reject a valid role.
    if (!hasExternalInitializers(result)) onnx::checker::check_model(result);
    return result;
  }

  throw std::invalid_argument("source transform kind is not supported");
}

} // namespace ndnsf::di
