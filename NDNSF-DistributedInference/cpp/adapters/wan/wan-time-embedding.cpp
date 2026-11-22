#include "wan-time-embedding.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ndnsf::di::wan {

TimeEmbeddingLayout
lowerTimeEmbedding(const std::array<TimeLinearParameters, 3>& parameters,
                   const WanInitializerAccess& material)
{
  for (const auto& [weight, bias] : parameters) {
    if (weight.data_type() != onnx::TensorProto::FLOAT || weight.dims_size() != 2 ||
        weight.dims(0) <= 0 || weight.dims(1) <= 0 ||
        bias.data_type() != onnx::TensorProto::FLOAT || bias.dims_size() != 1 ||
        bias.dims(0) != weight.dims(1)) {
      throw std::invalid_argument("invalid Wan time projection initializer");
    }
  }
  const auto& [up, upBias] = parameters[0];
  const auto& [down, downBias] = parameters[1];
  const auto& [modulation, modulationBias] = parameters[2];
  const auto frequencies = up.dims(0);
  const auto hidden = up.dims(1);
  constexpr std::int64_t GROUPS = 6;
  constexpr std::int64_t TRAINING_STEPS = 1000;
  if (frequencies % 2 != 0 || hidden < 2 || down.dims(0) != hidden ||
      down.dims(1) != hidden || modulation.dims(0) != hidden ||
      hidden > std::numeric_limits<std::int64_t>::max() / GROUPS ||
      modulation.dims(1) != GROUPS * hidden) {
    throw std::invalid_argument("Wan time projection dimensions disagree");
  }
  if (static_cast<std::uint64_t>(frequencies) >
      NativeCanonicalMaterialBundleMaxBytes / TRAINING_STEPS / sizeof(float)) {
    throw std::invalid_argument("Wan timestep table exceeds its material budget");
  }
  const auto ranges = nativeConvInputChannelRanges(hidden, 2);

  TimeEmbeddingLayout layout;
  auto& model = layout.embedding.model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-time-embedding");
  const auto port = [] (onnx::ValueInfoProto& value, const std::string& name,
                         int type, std::initializer_list<std::int64_t> shape) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    for (const auto dim : shape) tensor->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> inputs) {
    auto* node = graph->add_node();
    node->set_op_type(op);
    node->set_name(name);
    for (const auto& input : inputs) node->add_input(input);
    node->add_output(name);
    return node;
  };
  const auto setInt = [] (onnx::NodeProto* node, const char* name, std::int64_t value) {
    auto* attribute = node->add_attribute();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::INT);
    attribute->set_i(value);
  };
  const auto addSlice = [&] (const std::string& name, const onnx::TensorProto& tensor,
                             std::size_t axis, std::uint64_t first, std::uint64_t last) {
    auto* value = graph->add_initializer();
    *value = material.slice(tensor, axis, first, last);
    value->set_name(name);
  };
  const auto silu = [&] (const std::string& input, const std::string& output) {
    const auto gate = output + "/sigmoid";
    emit("Sigmoid", gate, {input});
    emit("Mul", output, {input, gate});
  };
  const auto integer = [&] (const char* name, std::int64_t value) {
    auto* initializer = graph->add_initializer();
    initializer->set_name(name);
    initializer->set_data_type(onnx::TensorProto::INT64);
    initializer->add_dims(1);
    initializer->add_int64_data(value);
  };
  const auto entryCut = [&] (int firstNode, NativeAssemblyTensorContractV3 input,
                             NativeAssemblyTensorContractV3 output) {
    OnnxEntryCut cut{{}, {std::move(input)}, {std::move(output)}};
    for (int index = firstNode; index < graph->node_size(); ++index) {
      cut.nodeIndices.push_back(static_cast<std::uint64_t>(index));
    }
    return cut;
  };

  port(*graph->add_input(), "timestep", onnx::TensorProto::INT64, {1});
  port(*graph->add_output(), "embedding", onnx::TensorProto::FLOAT, {1, hidden});
  integer("axis-1", 1);
  integer("first-step", 0);
  integer("past-last-step", TRAINING_STEPS);
  auto* table = graph->add_initializer();
  table->set_name("sinusoidal-table");
  table->set_data_type(onnx::TensorProto::FLOAT);
  table->add_dims(TRAINING_STEPS);
  table->add_dims(frequencies);
  std::vector<double> inverseFrequencies;
  inverseFrequencies.reserve(frequencies / 2);
  for (std::int64_t k = 0; k < frequencies / 2; ++k) {
    inverseFrequencies.push_back(1.0 / std::pow(10000.0, static_cast<double>(2 * k) / frequencies));
  }
  // Evaluate the source's trigonometry in FP64 before storing FP32. This
  // avoids relying on FP64 trigonometric kernels in the chosen ONNX backend.
  for (std::int64_t step = 0; step < TRAINING_STEPS; ++step) {
    for (const bool sine : {false, true}) {
      for (const auto frequency : inverseFrequencies) {
        const auto phase = step * frequency;
        table->add_float_data(static_cast<float>(sine ? std::sin(phase) : std::cos(phase)));
      }
    }
  }
  const NativeAssemblyTensorContractV3 timestepPort{"timestep", "int64", {std::int64_t{1}}};
  const NativeAssemblyTensorContractV3 embeddingPort{
    "embedding", "float32", {std::int64_t{1}, hidden}};
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto firstNode = graph->node_size();
    const auto name = "rank-" + std::to_string(rank) + "/";
    const auto [first, last] = ranges[rank];
    emit("GreaterOrEqual", name + "step-nonnegative", {"timestep", "first-step"});
    emit("Less", name + "step-in-range", {"timestep", "past-last-step"});
    emit("And", name + "step-valid", {name + "step-nonnegative", name + "step-in-range"});
    // Gather accepts negative indices. Route every invalid step to a positive
    // out-of-range index. Both ranks share the weight-free table.
    emit("Where", name + "checked-step",
         {name + "step-valid", "timestep", "past-last-step"});
    emit("Gather", name + "sinusoidal", {"sinusoidal-table", name + "checked-step"});
    addSlice(name + "up-weight", up, 1, first, last);
    addSlice(name + "up-bias", upBias, 0, first, last);
    addSlice(name + "down-weight", down, 0, first, last);
    emit("MatMul", name + "linear", {name + "sinusoidal", name + "up-weight"});
    emit("Add", name + "biased", {name + "linear", name + "up-bias"});
    silu(name + "biased", name + "activated");
    auto partial = embeddingPort;
    partial.name = name + "partial";
    emit("MatMul", partial.name, {name + "activated", name + "down-weight"});
    layout.embedding.partials[rank] = entryCut(firstNode, timestepPort, std::move(partial));
  }
  addSlice("embedding-bias", downBias, 0, 0, hidden);
  emit("Add", "embedding-sum",
       {layout.embedding.partials[0].outputs.front().name,
        layout.embedding.partials[1].outputs.front().name});
  const auto biasNode = graph->node_size();
  emit("Add", "embedding", {"embedding-sum", "embedding-bias"});
  auto summed = embeddingPort;
  summed.name = "embedding-sum";
  layout.embedding.addBias = entryCut(biasNode, std::move(summed), embeddingPort);

  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto firstNode = graph->node_size();
    const auto [first, last] = ranges[rank];
    const auto name = "rank-" + std::to_string(rank) + "/";
    const auto result = "modulation-rank-" + std::to_string(rank);
    const auto rankWidth = static_cast<std::int64_t>(last - first);
    port(*graph->add_output(), result, onnx::TensorProto::FLOAT,
         {1, GROUPS, rankWidth});
    const auto modulationInput = name + "modulation-input";
    silu("embedding", modulationInput);
    onnx::NodeProto concat;
    concat.set_op_type("Concat");
    concat.set_name(result);
    concat.add_output(result);
    setInt(&concat, "axis", 1);
    for (std::int64_t group = 0; group < GROUPS; ++group) {
      const auto base = name + "group-" + std::to_string(group) + "/";
      const auto offset = static_cast<std::uint64_t>(group * hidden);
      addSlice(base + "weight", modulation, 1, offset + first, offset + last);
      addSlice(base + "bias", modulationBias, 0, offset + first, offset + last);
      emit("MatMul", base + "linear", {modulationInput, base + "weight"});
      emit("Add", base + "biased", {base + "linear", base + "bias"});
      emit("Unsqueeze", base + "modulation", {base + "biased", "axis-1"});
      concat.add_input(base + "modulation");
    }
    *graph->add_node() = std::move(concat);
    layout.modulation[rank] = entryCut(firstNode, embeddingPort,
                                     {result, "float32", {std::int64_t{1}, GROUPS, rankWidth}});
  }
  return layout;
}

} // namespace ndnsf::di::wan
