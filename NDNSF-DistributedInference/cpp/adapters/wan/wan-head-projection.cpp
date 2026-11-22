#include "wan-head-projection.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ndnsf::di::wan {

HeadProjectionLayout
lowerHeadProjection(const onnx::ModelProto& source, std::uint64_t headWidth,
                     ProjectionParallelism parallelism, const WanInitializerAccess& material)
{
  const auto& graph = source.graph();
  if (graph.input_size() != 1 || graph.output_size() != 1 ||
      graph.node_size() != 2 || graph.initializer_size() != 2 ||
      !std::any_of(source.opset_import().begin(), source.opset_import().end(),
                   [] (const auto& opset) {
                     return opset.domain().empty() && opset.version() >= 13;
                   })) {
    throw std::invalid_argument("Wan head projection requires an isolated opset-13 MatMul + Add");
  }
  const auto& input = graph.input(0).name();
  const auto& output = graph.output(0).name();
  const std::string prefix = "ndnsf-wan-head/";
  const auto& multiply = graph.node(0);
  const auto& biasAdd = graph.node(1);
  if (input.empty() || output.empty() || input == output ||
      input.rfind(prefix, 0) == 0 || output.rfind(prefix, 0) == 0 ||
      multiply.op_type() != "MatMul" || biasAdd.op_type() != "Add" ||
      !multiply.domain().empty() || !biasAdd.domain().empty() ||
      multiply.attribute_size() != 0 || biasAdd.attribute_size() != 0 ||
      multiply.input_size() != 2 || biasAdd.input_size() != 2 ||
      multiply.output_size() != 1 || biasAdd.output_size() != 1 ||
      multiply.output(0).empty() || multiply.output(0) == input ||
      multiply.output(0) == output || multiply.input(0) != input ||
      biasAdd.input(0) != multiply.output(0) || biasAdd.output(0) != output ||
      multiply.input(1) == biasAdd.input(1)) {
    throw std::invalid_argument("invalid Wan head projection bindings");
  }
  const auto initializer = [&] (const std::string& name) -> const onnx::TensorProto& {
    const auto it = std::find_if(graph.initializer().begin(), graph.initializer().end(),
                                 [&] (const auto& value) { return value.name() == name; });
    if (name.empty() || name == input || name == output || name == multiply.output(0) ||
        it == graph.initializer().end() || it->data_type() != onnx::TensorProto::FLOAT) {
      throw std::invalid_argument("Wan head projection parameter must be a unique FP32 initializer");
    }
    return *it;
  };
  const auto& matrix = initializer(multiply.input(1));
  const auto& bias = initializer(biasAdd.input(1));
  const bool columns = parallelism == ProjectionParallelism::OUTPUT_CHANNELS;
  const int splitAxis = columns ? 1 : 0;
  if (matrix.dims_size() != 2 || matrix.dims(0) <= 0 || matrix.dims(1) <= 0 ||
      bias.dims_size() != 1 || bias.dims(0) != matrix.dims(1)) {
    throw std::invalid_argument("Wan head projection initializer geometry differs");
  }
  const auto channels = static_cast<std::uint64_t>(matrix.dims(splitAxis));
  if (headWidth == 0 || channels % headWidth != 0 || channels / headWidth < 2) {
    throw std::invalid_argument("Wan head projection requires complete head partitions");
  }
  const auto ranges = nativeConvInputChannelRanges(channels / headWidth, 2);
  const std::array<std::string, 2> rankPrefix{prefix + "rank-0/", prefix + "rank-1/"};

  const auto fixedPort = [] (const onnx::ValueInfoProto& value, std::int64_t width) {
    const auto& type = value.type().tensor_type();
    const auto& dims = type.shape().dim();
    if (type.elem_type() != onnx::TensorProto::FLOAT || dims.size() != 3 ||
        dims.Get(0).dim_value() != 1 || dims.Get(1).dim_value() <= 0 ||
        dims.Get(2).dim_value() != width) {
      throw std::invalid_argument("Wan head projection requires fixed FP32 token-major ports");
    }
    return NativeAssemblyTensorContractV3{
      value.name(), "float32", {std::int64_t{1}, dims.Get(1).dim_value(), width}};
  };
  const auto inputPort = fixedPort(graph.input(0), matrix.dims(0));
  const auto outputPort = fixedPort(graph.output(0), matrix.dims(1));
  if (inputPort.shape[1] != outputPort.shape[1])
    throw std::invalid_argument("Wan head projection input and output token counts differ");

  HeadProjectionLayout layout;
  auto& result = layout.model;
  result.set_ir_version(source.ir_version());
  *result.mutable_opset_import() = source.opset_import();
  auto* lowered = result.mutable_graph();
  lowered->set_name(graph.name());
  *lowered->add_input() = graph.input(0);
  *lowered->add_output() = graph.output(0);
  const auto emit = [&] (const char* op, const std::string& destination,
                         std::initializer_list<std::string> inputs) {
    auto* node = lowered->add_node();
    node->set_name(destination);
    node->set_op_type(op);
    for (const auto& port : inputs) node->add_input(port);
    node->add_output(destination);
    return node;
  };
  const auto lastAxis = [] (onnx::NodeProto& node) {
    auto* axis = node.add_attribute();
    axis->set_name("axis");
    axis->set_type(onnx::AttributeProto::INT);
    axis->set_i(-1);
  };
  const auto cut = [&] (int begin, std::vector<NativeAssemblyTensorContractV3> inputs,
                         NativeAssemblyTensorContractV3 out) {
    OnnxEntryCut entry{{}, std::move(inputs), {std::move(out)}};
    for (int n = begin; n < lowered->node_size(); ++n) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(n));
    }
    return entry;
  };
  if (!columns) {
    auto* splits = lowered->add_initializer();
    splits->set_name(prefix + "split-widths");
    splits->set_data_type(onnx::TensorProto::INT64);
    splits->add_dims(2);
    for (const auto& [first, last] : ranges) splits->add_int64_data((last - first) * headWidth);
    auto* split = emit("Split", rankPrefix[0] + "input", {input, splits->name()});
    split->add_output(rankPrefix[1] + "input");
    lastAxis(*split);
  }
  std::array<std::string, 2> partials;
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto& name = rankPrefix[rank];
    const auto [first, last] = ranges[rank];
    const auto addSlice = [&] (const onnx::TensorProto& tensor, int axis, const char* suffix) {
      auto* value = lowered->add_initializer();
      *value = material.slice(tensor, axis, first * headWidth, last * headWidth);
      value->set_name(name + suffix);
      return value->name();
    };
    const auto weight = addSlice(matrix, splitAxis, "weight");
    auto rankInput = inputPort;
    auto rankOutput = outputPort;
    const auto rankWidth = static_cast<std::int64_t>((last - first) * headWidth);
    if (columns) {
      rankOutput.shape[2] = rankWidth;
    }
    else {
      rankInput.name = name + "input";
      rankInput.shape[2] = rankWidth;
    }
    const auto firstNode = lowered->node_size();
    emit("MatMul", name + "linear", {rankInput.name, weight});
    partials[rank] = name + "linear";
    if (columns) {
      const auto localBias = addSlice(bias, 0, "bias");
      partials[rank] = name + "biased";
      emit("Add", partials[rank], {name + "linear", localBias});
    }
    rankOutput.name = partials[rank];
    layout.ranks[rank] = cut(firstNode, {rankInput}, rankOutput);
  }
  if (columns) {
    const auto firstNode = lowered->node_size();
    lastAxis(*emit("Concat", output, {partials[0], partials[1]}));
    layout.finish = cut(firstNode,
      {layout.ranks[0].outputs.front(), layout.ranks[1].outputs.front()}, outputPort);
  }
  else {
    const auto sum = prefix + "unbiased";
    emit("Add", sum, {partials[0], partials[1]});
    const auto firstNode = lowered->node_size();
    auto* globalBias = lowered->add_initializer();
    *globalBias = material.slice(bias, 0, 0, bias.dims(0));
    globalBias->set_name(prefix + "bias");
    emit("Add", output, {sum, globalBias->name()});
    auto reduced = outputPort;
    reduced.name = sum;
    layout.finish = cut(firstNode, {reduced}, outputPort);
  }
  return layout;
}

} // namespace ndnsf::di::wan
