#include "wan-normalized-projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

namespace ndnsf::di::wan {

NormalizedProjectionLayout
lowerNormalizedProjection(const onnx::ModelProto& source, std::uint64_t headWidth,
                           const WanInitializerAccess& material)
{
  const auto& graph = source.graph();
  const std::array<const char*, 8> operations{
    "MatMul", "Add", "Mul", "ReduceMean", "Add", "Sqrt", "Div", "Mul"
  };
  if (graph.input_size() != 1 || graph.output_size() != 1 || graph.initializer_size() != 5 ||
      graph.node_size() != static_cast<int>(operations.size()) ||
      !std::any_of(source.opset_import().begin(), source.opset_import().end(),
                   [] (const auto& opset) {
                     return opset.domain().empty() && opset.version() >= 18;
                   })) {
    throw std::invalid_argument("Wan Q/K normalization requires an isolated opset-18 source");
  }
  const auto& input = graph.input(0).name();
  const auto& output = graph.output(0).name();
  const std::string prefix = "ndnsf-wan-qk/";
  if (input.empty() || output.empty() || input.rfind(prefix, 0) == 0 ||
      output.rfind(prefix, 0) == 0) {
    throw std::invalid_argument("invalid Wan Q/K port names");
  }

  const std::array<int, 8> first{-1, 0, 1, 2, 3, 4, 1, 6};
  std::set<std::string> names{input};
  for (int i = 0; i < graph.node_size(); ++i) {
    const auto& node = graph.node(i);
    if (node.op_type() != operations[i] || !node.domain().empty() ||
        node.input_size() != (i == 5 ? 1 : 2) || node.output_size() != 1 ||
        node.output(0).empty() || !names.insert(node.output(0)).second ||
        node.input(0) != (first[i] < 0 ? input : graph.node(first[i]).output(0)) ||
        node.attribute_size() > (i == 3 ? 1 : 0)) {
      throw std::invalid_argument("Wan Q/K normalization source connections differ");
    }
  }
  const auto& reduce = graph.node(3);
  if (graph.node(2).input(1) != graph.node(1).output(0) ||
      graph.node(6).input(1) != graph.node(5).output(0) ||
      graph.node(7).output(0) != output ||
      (reduce.attribute_size() == 1 &&
       (reduce.attribute(0).name() != "keepdims" ||
        reduce.attribute(0).type() != onnx::AttributeProto::INT ||
        reduce.attribute(0).i() != 1))) {
    throw std::invalid_argument("Wan Q/K normalization must keep full-width RMS semantics");
  }

  const auto parameter = [&] (int node, int type) -> const onnx::TensorProto& {
    const auto& name = graph.node(node).input(1);
    if (name.empty() || !names.insert(name).second) {
      throw std::invalid_argument("Wan Q/K normalization parameter is aliased");
    }
    const auto it = std::find_if(graph.initializer().begin(), graph.initializer().end(),
                                 [&] (const auto& value) { return value.name() == name; });
    if (it == graph.initializer().end() || it->data_type() != type) {
      throw std::invalid_argument("invalid Wan Q/K normalization initializer");
    }
    return *it;
  };
  const auto& weights = parameter(0, onnx::TensorProto::FLOAT);
  const auto& bias = parameter(1, onnx::TensorProto::FLOAT);
  const auto& axes = parameter(3, onnx::TensorProto::INT64);
  const auto& epsilon = parameter(4, onnx::TensorProto::FLOAT);
  const auto& gain = parameter(7, onnx::TensorProto::FLOAT);
  if (weights.dims_size() != 2 || weights.dims(0) <= 0 || weights.dims(1) <= 0 ||
      headWidth == 0 || static_cast<std::uint64_t>(weights.dims(1)) % headWidth != 0 ||
      static_cast<std::uint64_t>(weights.dims(1)) / headWidth < 2 ||
      bias.dims_size() != 1 || bias.dims(0) != weights.dims(1) ||
      gain.dims_size() != 1 || gain.dims(0) != weights.dims(1) ||
      axes.dims_size() != 1 || axes.dims(0) != 1 || axes.int64_data_size() != 1 ||
      axes.int64_data(0) != -1 || axes.has_raw_data() || axes.external_data_size() != 0 ||
      axes.data_location() != onnx::TensorProto::DEFAULT ||
      epsilon.dims_size() != 0 || epsilon.float_data_size() != 1 ||
      !std::isfinite(epsilon.float_data(0)) || epsilon.float_data(0) <= 0 ||
      epsilon.has_raw_data() || epsilon.external_data_size() != 0 ||
      epsilon.data_location() != onnx::TensorProto::DEFAULT) {
    throw std::invalid_argument("invalid Wan Q/K head geometry or normalization constants");
  }

  const auto width = static_cast<std::uint64_t>(weights.dims(1));
  const auto headRanges = nativeConvInputChannelRanges(width / headWidth, 2);
  const auto fixedPort = [] (const onnx::ValueInfoProto& value, std::int64_t channels) {
    const auto& type = value.type().tensor_type();
    const auto& dims = type.shape().dim();
    if (type.elem_type() != onnx::TensorProto::FLOAT || dims.size() != 3 ||
        dims.Get(0).dim_value() != 1 || dims.Get(1).dim_value() <= 0 ||
        dims.Get(2).dim_value() != channels) {
      throw std::invalid_argument("Wan Q/K requires fixed FP32 token-major ports");
    }
    return NativeAssemblyTensorContractV3{
      value.name(), "float32", {std::int64_t{1}, dims.Get(1).dim_value(), channels}};
  };
  const auto inputPort = fixedPort(graph.input(0), weights.dims(0));
  const auto outputPort = fixedPort(graph.output(0), weights.dims(1));
  if (inputPort.shape[1] != outputPort.shape[1])
    throw std::invalid_argument("Wan Q/K input and output token counts differ");
  const NativeAssemblyTensorContractV3 globalStats{
    prefix + "global-square-sum", "float32", {std::int64_t{1}, outputPort.shape[1],
                                              std::int64_t{1}}};

  NormalizedProjectionLayout layout;
  auto& lowered = layout.model;
  lowered.set_ir_version(source.ir_version());
  *lowered.mutable_opset_import() = source.opset_import();
  auto* result = lowered.mutable_graph();
  result->set_name(graph.name());
  *result->add_input() = graph.input(0);
  *result->add_output() = graph.output(0);
  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> inputs) {
    auto* node = result->add_node();
    node->set_name(name);
    node->set_op_type(op);
    for (const auto& value : inputs) node->add_input(value);
    node->add_output(name);
    return node;
  };
  const auto constant = [&] (const onnx::TensorProto& tensor, const std::string& name) {
    auto* value = result->add_initializer();
    *value = tensor;
    value->set_name(name);
  };
  const auto cut = [&] (int begin, std::vector<NativeAssemblyTensorContractV3> inputs,
                         std::vector<NativeAssemblyTensorContractV3> outputs) {
    OnnxEntryCut entry{{}, std::move(inputs), std::move(outputs)};
    for (int n = begin; n < result->node_size(); ++n) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(n));
    }
    return entry;
  };
  constant(axes, prefix + "axes");
  constant(epsilon, prefix + "epsilon");
  auto* divisor = result->add_initializer();
  divisor->set_name(prefix + "width");
  divisor->set_data_type(onnx::TensorProto::FLOAT);
  divisor->add_float_data(static_cast<float>(width));
  std::array<std::string, 2> rankNames;
  for (std::size_t rank = 0; rank < headRanges.size(); ++rank) {
    const auto name = prefix + "rank-" + std::to_string(rank) + "/";
    rankNames[rank] = name;
    const auto [firstHead, lastHead] = headRanges[rank];
    const auto slice = [&] (const onnx::TensorProto& value, std::size_t axis, const char* port) {
      auto* initializer = result->add_initializer();
      *initializer = material.slice(
        value, axis, firstHead * headWidth, lastHead * headWidth);
      initializer->set_name(name + port);
    };
    slice(weights, 1, "weights");
    slice(bias, 0, "bias");
    slice(gain, 0, "gain");
    const auto firstNode = result->node_size();
    emit("MatMul", name + "linear", {input, name + "weights"});
    emit("Add", name + "projected", {name + "linear", name + "bias"});
    emit("Mul", name + "squared", {name + "projected", name + "projected"});
    emit("ReduceSum", name + "statistics", {name + "squared", prefix + "axes"});
    auto local = outputPort;
    local.name = name + "projected";
    local.shape[2] = static_cast<std::int64_t>((lastHead - firstHead) * headWidth);
    auto statistics = globalStats;
    statistics.name = name + "statistics";
    layout.ranks[rank].project = cut(firstNode, {inputPort}, {local, statistics});
  }
  emit("Add", globalStats.name,
       {rankNames[0] + "statistics", rankNames[1] + "statistics"});
  for (std::size_t rank = 0; rank < rankNames.size(); ++rank) {
    const auto& name = rankNames[rank];
    const auto firstNode = result->node_size();
    emit("Div", name + "square-mean", {globalStats.name, prefix + "width"});
    emit("Add", name + "stabilized", {name + "square-mean", prefix + "epsilon"});
    emit("Sqrt", name + "rms", {name + "stabilized"});
    emit("Div", name + "normalized", {name + "projected", name + "rms"});
    emit("Mul", name + "scaled", {name + "normalized", name + "gain"});
    auto& entry = layout.ranks[rank];
    auto scaled = entry.project.outputs.front();
    scaled.name = name + "scaled";
    entry.normalize = cut(firstNode, {entry.project.outputs.front(), globalStats}, {scaled});
  }
  auto* concat = emit("Concat", output, {rankNames[0] + "scaled", rankNames[1] + "scaled"});
  auto* axis = concat->add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(-1);
  return layout;
}

} // namespace ndnsf::di::wan
