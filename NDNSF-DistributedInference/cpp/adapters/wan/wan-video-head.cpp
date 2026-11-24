#include "wan-video-head.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace ndnsf::di::wan {

SummedProjection
lowerVideoHead(const VideoHeadGrid& grid, const onnx::TensorProto& modulation,
               const onnx::TensorProto& weight, const onnx::TensorProto& bias, float epsilon,
               const WanInitializerAccess& material)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (grid.frames <= 0 || grid.height <= 0 || grid.width <= 0 ||
      grid.height > LIMIT / 2 || grid.width > LIMIT / 2 ||
      grid.frames > LIMIT / grid.height / grid.width ||
      !std::isfinite(epsilon) || epsilon <= 0) {
    throw std::invalid_argument("invalid Wan head grid or epsilon");
  }
  const auto tokens = grid.frames * grid.height * grid.width;
  if (grid.paddedTokens < tokens || weight.data_type() != onnx::TensorProto::FLOAT ||
      weight.dims_size() != 2 || weight.dims(0) < 2 || weight.dims(1) < 4 ||
      weight.dims(1) % 4 != 0 || bias.data_type() != onnx::TensorProto::FLOAT ||
      bias.dims_size() != 1 || bias.dims(0) != weight.dims(1) ||
      modulation.data_type() != onnx::TensorProto::FLOAT || modulation.dims_size() != 3 ||
      modulation.dims(0) != 1 || modulation.dims(1) != 2 ||
      modulation.dims(2) != weight.dims(0)) {
    throw std::invalid_argument("Wan head ports and parameter geometry disagree");
  }
  const auto hidden = weight.dims(0);
  const auto patchWidth = weight.dims(1);
  const auto channels = patchWidth / 4;
  constexpr auto FLOAT_BYTES = static_cast<std::int64_t>(sizeof(float));
  if (grid.paddedTokens > LIMIT / hidden / FLOAT_BYTES ||
      grid.paddedTokens > LIMIT / patchWidth / FLOAT_BYTES ||
      static_cast<std::uint64_t>(hidden) > NativeCanonicalMaterialBundleMaxBytes / sizeof(float)) {
    throw std::invalid_argument("Wan head tensor extent exceeds its arithmetic bounds");
  }

  SummedProjection layout;
  auto& model = layout.model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-video-head");

  const NativeAssemblyTensorContractV3 hiddenPort{
    "hidden", "float32", {std::int64_t{1}, grid.paddedTokens, hidden}};
  const NativeAssemblyTensorContractV3 embeddingPort{
    "embedding", "float32", {std::int64_t{1}, hidden}};
  const NativeAssemblyTensorContractV3 output{
    "latent", "float32", {std::int64_t{1}, channels, grid.frames, 2 * grid.height, 2 * grid.width}};
  const auto declare = [] (onnx::ValueInfoProto& port, const NativeAssemblyTensorContractV3& spec) {
    port.set_name(spec.name);
    auto* tensor = port.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto& dim : spec.shape) {
      tensor->mutable_shape()->add_dim()->set_dim_value(std::get<std::int64_t>(dim));
    }
  };
  declare(*graph->add_input(), hiddenPort);
  declare(*graph->add_input(), embeddingPort);
  declare(*graph->add_output(), output);

  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> inputs) {
    auto* node = graph->add_node();
    node->set_name(name);
    node->set_op_type(op);
    for (const auto& input : inputs) node->add_input(input);
    node->add_output(name);
    return node;
  };
  const auto indexes = [&] (const std::string& name,
                            std::initializer_list<std::int64_t> values) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(values.size());
    for (const auto value : values) tensor->add_int64_data(value);
  };
  const auto scalar = [&] (const char* name, float value, std::int64_t width) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    tensor->add_dims(width);
    tensor->mutable_float_data()->Resize(static_cast<int>(width), value);
  };
  const auto parameter = [&] (const std::string& name, const onnx::TensorProto& source,
                               std::size_t axis, std::uint64_t begin, std::uint64_t end) {
    auto* tensor = graph->add_initializer();
    *tensor = material.slice(source, axis, begin, end);
    tensor->set_name(name);
  };
  const auto cut = [&] (int first, std::vector<NativeAssemblyTensorContractV3> inputs,
                         NativeAssemblyTensorContractV3 result) {
    OnnxEntryCut entry{{}, std::move(inputs), {std::move(result)}};
    for (int index = first; index < graph->node_size(); ++index) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(index));
    }
    return entry;
  };

  indexes("axis-1", {1});
  indexes("axis-2", {2});
  indexes("shift-index", {0});
  indexes("scale-index", {1});
  scalar("one", 1.0F, 1);
  scalar("norm-scale", 1.0F, hidden);

  const auto ranges = nativeConvInputChannelRanges(hidden, 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto firstNode = graph->node_size();
    const auto [first, last] = ranges[rank];
    const auto name = "head/rank-" + std::to_string(rank) + "/";
    indexes(name + "first", {static_cast<std::int64_t>(first)});
    indexes(name + "last", {static_cast<std::int64_t>(last)});
    parameter(name + "modulation", modulation, 2, first, last);
    parameter(name + "weight", weight, 0, first, last);

    auto* norm = emit("LayerNormalization", name + "norm", {"hidden", "norm-scale"});
    auto* eps = norm->add_attribute();
    eps->set_name("epsilon");
    eps->set_type(onnx::AttributeProto::FLOAT);
    eps->set_f(epsilon);
    emit("Slice", name + "local-hidden",
         {name + "norm", name + "first", name + "last", "axis-2"});
    emit("Slice", name + "local-embedding",
         {"embedding", name + "first", name + "last", "axis-1"});
    emit("Unsqueeze", name + "broadcast-embedding", {name + "local-embedding", "axis-1"});
    emit("Add", name + "condition", {name + "broadcast-embedding", name + "modulation"});
    for (const auto* group : {"shift", "scale"}) {
      auto* gather = emit("Gather", name + group,
                          {name + "condition", std::string(group) + "-index"});
      auto* axis = gather->add_attribute();
      axis->set_name("axis");
      axis->set_type(onnx::AttributeProto::INT);
      axis->set_i(1);
    }
    emit("Add", name + "gain", {name + "scale", "one"});
    emit("Mul", name + "scaled", {name + "local-hidden", name + "gain"});
    emit("Add", name + "shifted", {name + "scaled", name + "shift"});
    emit("MatMul", name + "partial", {name + "shifted", name + "weight"});
    layout.partials[rank] = cut(firstNode, {hiddenPort, embeddingPort},
      {name + "partial", "float32", {std::int64_t{1}, grid.paddedTokens, patchWidth}});
  }

  emit("Add", "head/sum",
       {layout.partials[0].outputs.front().name, layout.partials[1].outputs.front().name});
  const auto resultStart = graph->node_size();
  parameter("head/bias", bias, 0, 0, patchWidth);
  emit("Add", "head/biased", {"head/sum", "head/bias"});
  auto patches = std::string("head/biased");
  if (grid.paddedTokens != tokens) {
    indexes("head/first-token", {0});
    indexes("head/token-count", {tokens});
    patches = "head/unpadded";
    emit("Slice", patches, {"head/biased", "head/first-token", "head/token-count", "axis-1"});
  }
  indexes("head/patch-shape", {1, grid.frames, grid.height, grid.width, 2, 2, channels});
  indexes("head/latent-shape", {1, channels, grid.frames, 2 * grid.height, 2 * grid.width});
  emit("Reshape", "head/patch-grid", {patches, "head/patch-shape"});
  auto* shuffle = emit("Transpose", "head/video-grid", {"head/patch-grid"})->add_attribute();
  shuffle->set_name("perm");
  shuffle->set_type(onnx::AttributeProto::INTS);
  for (const auto axis : {0, 6, 1, 2, 4, 3, 5}) shuffle->add_ints(axis);
  emit("Reshape", output.name, {"head/video-grid", "head/latent-shape"});
  layout.addBias = cut(resultStart,
    {{"head/sum", "float32", {std::int64_t{1}, grid.paddedTokens, patchWidth}}}, output);
  return layout;
}

} // namespace ndnsf::di::wan
