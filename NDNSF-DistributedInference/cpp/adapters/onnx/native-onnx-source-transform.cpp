#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <onnx/checker.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

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

std::uint64_t
parseFfnExternalCoordinate(std::string_view value)
{
  std::uint64_t result = 0;
  const auto* end = value.data() + value.size();
  const auto parsed = std::from_chars(value.data(), end, result);
  if (parsed.ec != std::errc() || parsed.ptr != end)
    throw std::invalid_argument("invalid paired FFN external coordinate");
  return result;
}

void
sliceFfnWeight(onnx::TensorProto& tensor, std::uint64_t rows, std::uint64_t columns,
               std::uint64_t rowBegin, std::uint64_t rowEnd,
               std::uint64_t columnBegin, std::uint64_t columnEnd,
               const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append,
               std::uint64_t& sidecarOffset)
{
  if (tensor.data_type() != onnx::TensorProto::FLOAT || rows == 0 || columns == 0 ||
      rows > std::numeric_limits<std::size_t>::max() / 4 / columns ||
      rowBegin >= rowEnd || rowEnd > rows || columnBegin >= columnEnd || columnEnd > columns)
    throw std::invalid_argument("invalid paired FFN initializer dimensions");
  const auto elements = rows * columns;
  const auto rowBytes = (columnEnd - columnBegin) * 4;
  const auto sliceBytes = (rowEnd - rowBegin) * rowBytes;
  if (tensor.data_location() == onnx::TensorProto::EXTERNAL || !tensor.external_data().empty()) {
    if (!external || !append || tensor.data_location() != onnx::TensorProto::EXTERNAL ||
        tensor.external_data_size() != 3 || !tensor.raw_data().empty() ||
        tensor.float_data_size() != 0)
      throw std::invalid_argument("invalid paired FFN external storage");
    std::string location, offset, length;
    for (const auto& field : tensor.external_data()) {
      if (field.key() == "location") location = field.value();
      else if (field.key() == "offset") offset = field.value();
      else if (field.key() == "length") length = field.value();
    }
    const auto start = parseFfnExternalCoordinate(offset);
    if (location != "model.onnx.data" || parseFfnExternalCoordinate(length) != elements * 4 ||
        start > std::numeric_limits<std::uint64_t>::max() - elements * 4 ||
        sidecarOffset > std::numeric_limits<std::uint64_t>::max() - sliceBytes)
      throw std::invalid_argument("invalid paired FFN external range");
    const auto copyRange = [&] (std::uint64_t position, std::uint64_t remaining) {
      while (remaining != 0) {
        const auto window = std::min(remaining, NativeCanonicalMaterialBundleMaxBytes);
        const auto bytes = external(location, position, window);
        if (bytes.size() != window)
          throw std::invalid_argument("short paired FFN external read");
        append(bytes);
        position += window;
        remaining -= window;
      }
    };
    if (columnBegin == 0 && columnEnd == columns) {
      copyRange(start + rowBegin * rowBytes, sliceBytes);
    }
    else {
      for (auto row = rowBegin; row < rowEnd; ++row)
        copyRange(start + (row * columns + columnBegin) * 4, rowBytes);
    }
    for (auto& field : *tensor.mutable_external_data()) {
      if (field.key() == "offset") field.set_value(std::to_string(sidecarOffset));
      else if (field.key() == "length") field.set_value(std::to_string(sliceBytes));
    }
    sidecarOffset += sliceBytes;
    return;
  }
  if ((!tensor.raw_data().empty() && tensor.raw_data().size() != elements * 4) ||
      (tensor.raw_data().empty() && static_cast<std::uint64_t>(tensor.float_data_size()) != elements))
    throw std::invalid_argument("invalid paired FFN initializer storage");

  const auto source = rawBytes(tensor, {});
  std::vector<std::uint8_t> slice;
  slice.reserve(sliceBytes);
  for (auto row = rowBegin; row < rowEnd; ++row) {
    const auto offset = (row * columns + columnBegin) * 4;
    slice.insert(slice.end(), source.data() + offset,
                 source.data() + offset + rowBytes);
  }
  setInlineBytes(tensor, std::move(slice));
}

void
lowerFfnPair(onnx::ModelProto& model, const NativeOnnxSourceTransformV1& transform,
             const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append)
{
  auto& graph = *model.mutable_graph();
  const bool biased = !transform.bias.empty();
  if (graph.node_size() != (biased ? 4 : 3) || graph.input_size() != 1 ||
      graph.output_size() != 1 || graph.initializer_size() != (biased ? 3 : 2) ||
      graph.sparse_initializer_size() != 0 || graph.input(0).name() != transform.input ||
      graph.output(0).name() != transform.output)
    throw std::invalid_argument("paired FFN region has an open or unsupported boundary");

  const auto requireOp = [] (const auto& node, std::string_view op, int inputs) {
    if (!node.domain().empty() || node.op_type() != op || node.input_size() != inputs ||
        node.output_size() != 1 || node.output(0).empty() ||
        (op != "Gelu" && node.attribute_size() != 0))
      throw std::invalid_argument("paired FFN region has an unsupported operator");
  };
  const auto& up = graph.node(0);
  requireOp(up, "MatMul", 2);
  if (up.name() != transform.node || up.input(0) != transform.input ||
      up.input(1) != transform.weight)
    throw std::invalid_argument("paired FFN up projection differs from recipe");
  auto activationInput = up.output(0);
  if (biased) {
    const auto& add = graph.node(1);
    requireOp(add, "Add", 2);
    if (!((add.input(0) == activationInput && add.input(1) == transform.bias) ||
          (add.input(1) == activationInput && add.input(0) == transform.bias)))
      throw std::invalid_argument("paired FFN up bias differs from recipe");
    activationInput = add.output(0);
  }
  const auto& activation = graph.node(biased ? 2 : 1);
  requireOp(activation, "Gelu", 1);
  if (activation.attribute_size() > 1 ||
      (activation.attribute_size() == 1 &&
       (activation.attribute(0).name() != "approximate" ||
        activation.attribute(0).type() != onnx::AttributeProto::STRING ||
        (activation.attribute(0).s() != "tanh" && activation.attribute(0).s() != "none"))))
    throw std::invalid_argument("paired FFN GELU has unsupported attributes");
  const auto& down = graph.node(graph.node_size() - 1);
  requireOp(down, "MatMul", 2);
  if (activation.input(0) != activationInput || down.name() != transform.downNode ||
      down.input(0) != activation.output(0) || down.input(1) != transform.downWeight ||
      down.output(0) != transform.output)
    throw std::invalid_argument("paired FFN down projection differs from recipe");
  const auto defaultOpset = std::count_if(model.opset_import().begin(),
    model.opset_import().end(), [] (const auto& opset) { return opset.domain().empty(); });
  const auto supportsGelu = std::any_of(model.opset_import().begin(),
    model.opset_import().end(), [] (const auto& opset) {
      return opset.domain().empty() && opset.version() >= 20;
    });
  if (defaultOpset != 1 || !supportsGelu || model.functions_size() != 0)
    throw std::invalid_argument("paired FFN requires standard ONNX GELU");
  std::set<std::string> valueNames{transform.input};
  for (const auto& tensor : graph.initializer()) {
    if (!valueNames.insert(tensor.name()).second)
      throw std::invalid_argument("paired FFN initializer names overlap");
  }
  for (const auto& node : graph.node()) {
    if (!valueNames.insert(node.output(0)).second)
      throw std::invalid_argument("paired FFN source values overlap");
  }

  auto* upWeight = findInitializer(model, transform.weight);
  auto* downWeight = findInitializer(model, transform.downWeight);
  if (transform.extent > std::numeric_limits<std::int64_t>::max())
    throw std::invalid_argument("paired FFN extent exceeds ONNX dimension limit");
  const auto extent = static_cast<std::int64_t>(transform.extent);
  if (upWeight == nullptr || downWeight == nullptr || upWeight->dims_size() != 2 ||
      downWeight->dims_size() != 2 || upWeight->dims(1) != extent ||
      downWeight->dims(0) != extent || upWeight->dims(0) <= 0 || downWeight->dims(1) <= 0)
    throw std::invalid_argument("paired FFN initializer geometry differs from recipe");
  const auto& inputType = graph.input(0).type().tensor_type();
  const auto& outputType = graph.output(0).type().tensor_type();
  const auto& inputShape = inputType.shape();
  const auto& outputShape = outputType.shape();
  const auto rank = inputShape.dim_size();
  if (inputType.elem_type() != onnx::TensorProto::FLOAT ||
      outputType.elem_type() != onnx::TensorProto::FLOAT || rank < 2 ||
      outputShape.dim_size() != rank || inputShape.dim(rank - 1).dim_value() != upWeight->dims(0) ||
      outputShape.dim(rank - 1).dim_value() != downWeight->dims(1))
    throw std::invalid_argument("paired FFN port shape differs from recipe");
  for (int dim = 0; dim < rank - 1; ++dim) {
    if (inputShape.dim(dim).dim_value() <= 0 ||
        inputShape.dim(dim).dim_value() != outputShape.dim(dim).dim_value())
      throw std::invalid_argument("paired FFN batch shape differs between ports");
  }
  const auto hasExternal = hasExternalInitializers(model);
  if (!hasExternal) onnx::checker::check_model(model);
  const auto width = static_cast<std::int64_t>(transform.end - transform.begin);
  const auto hiddenIn = static_cast<std::uint64_t>(upWeight->dims(0));
  const auto hiddenOut = static_cast<std::uint64_t>(downWeight->dims(1));
  std::uint64_t sidecarOffset = 0;
  sliceFfnWeight(*upWeight, hiddenIn, transform.extent, 0, hiddenIn, transform.begin,
                 transform.end, external, append, sidecarOffset);
  sliceFfnWeight(*downWeight, transform.extent, hiddenOut, transform.begin, transform.end,
                 0, hiddenOut, external, append, sidecarOffset);
  upWeight->set_dims(1, width);
  downWeight->set_dims(0, width);
  if (biased) {
    auto* bias = findInitializer(model, transform.bias);
    if (bias == nullptr || bias->dims_size() != 1 || bias->dims(0) != extent)
      throw std::invalid_argument("invalid paired FFN up bias geometry");
    sliceFfnWeight(*bias, 1, transform.extent, 0, 1, transform.begin, transform.end,
                   external, append, sidecarOffset);
    bias->set_dims(0, width);
  }
  // The copied output ABI is unchanged. Re-infer the derived intermediate shapes.
  graph.clear_value_info();
  if (!hasExternal) onnx::checker::check_model(model);
}

} // namespace

onnx::ModelProto
applyNativeOnnxSourceTransform(const onnx::ModelProto& source,
                               const NativeOnnxSourceTransformV1& transform,
                               const NativeOnnxExternalReader& external,
                               const NativeOnnxExternalAppender& append)
{
  transform.validate();
  onnx::ModelProto result = source;
  auto* graph = result.mutable_graph();
  if (graph->name().empty()) graph->set_name("source-transform");

  if (transform.kind == "ffn-pair-partial") {
    lowerFfnPair(result, transform, external, append);
    return result;
  }

  if (transform.kind == "conv-input-channel-partial") {
    auto* conv = findNode(result, transform.node);
    if (conv == nullptr || conv->op_type() != "Conv")
      throw std::invalid_argument("source transform Conv node is missing");
    if (conv->input_size() < 2 || conv->input(0) != transform.input)
      throw std::invalid_argument("source transform Conv input differs");
    // The transform must describe this exact Conv: operand order is part of the
    // source binding, not a catalog hint. A different weight or a dropped fused
    // bias would compute a different function while looking plausible.
    if (conv->input(1) != transform.weight)
      throw std::invalid_argument("source transform weight is not the Conv operand");
    if (!transform.bias.empty()) {
      if (conv->input_size() < 3 || conv->input(2) != transform.bias)
        throw std::invalid_argument("source transform bias is not the Conv operand");
    }
    else if (conv->input_size() > 2) {
      throw std::invalid_argument("source transform drops an undeclared Conv bias");
    }
    int group = 1;
    for (const auto& attribute : conv->attribute())
      if (attribute.name() == "group") group = static_cast<int>(attribute.i());
    if (group != 1)
      throw std::invalid_argument("source transform requires group-1 Conv");
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
    // Do not rewrite the source geometry: verify the declaration matches the
    // Conv that will actually execute.
    const auto verifyInts = [&] (const char* name, const std::vector<std::int64_t>& expected) {
      if (expected.empty()) return;
      // ONNX defaults apply when the attribute is absent: pads to zero, strides
      // and dilations to one. Compare against the effective geometry.
      std::vector<std::int64_t> actual;
      bool present = false;
      for (const auto& attribute : conv->attribute())
        if (attribute.name() == name) {
          actual.assign(attribute.ints().begin(), attribute.ints().end());
          present = true;
        }
      if (!present) {
        if (std::string(name) == "pads") actual.assign(expected.size(), 0);
        else actual.assign(expected.size(), 1);
      }
      if (actual != expected)
        throw std::invalid_argument(std::string("source transform ") + name +
                                    " differs from the Conv attribute");
    };
    verifyInts("pads", transform.pads);
    verifyInts("strides", transform.strides);
    verifyInts("dilations", transform.dilations);

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
