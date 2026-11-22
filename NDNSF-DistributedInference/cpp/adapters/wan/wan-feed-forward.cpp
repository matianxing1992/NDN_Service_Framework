#include "wan-feed-forward.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::wan {
namespace {

SummedProjection
lowerProjection(const onnx::ModelProto& source, const WanInitializerAccess& material,
                std::optional<std::int64_t> textLength)
{
  const auto& graph = source.graph();
  if (!std::any_of(source.opset_import().begin(), source.opset_import().end(),
                   [] (const auto& opset) {
                     return opset.domain().empty() && opset.version() >= 20;
                   })) {
    throw std::invalid_argument("Wan FFN requires canonical Gelu from ONNX opset 20 or later");
  }
  if (graph.node_size() != 5 || graph.input_size() != 1 || graph.output_size() != 1 ||
      graph.initializer_size() != 4) {
    throw std::invalid_argument("Wan FFN source must isolate two biased projections and Gelu");
  }
  const std::string& input = graph.input(0).name();
  const std::string& output = graph.output(0).name();
  const std::string prefix = textLength ? "ndnsf-wan-text-embedding/" : "ndnsf-wan-ffn/";
  if (input.empty() || output.empty() || input == output ||
      input.rfind(prefix, 0) == 0 || output.rfind(prefix, 0) == 0) {
    throw std::invalid_argument("invalid Wan FFN ports");
  }
  const std::array<const char*, 5> operations{"MatMul", "Add", "Gelu", "MatMul", "Add"};
  std::set<std::string> values{input};
  std::string previous = input;
  for (int i = 0; i < graph.node_size(); ++i) {
    const auto& node = graph.node(i);
    const bool gelu = i == 2;
    if (!node.domain().empty() || node.op_type() != operations[i] ||
        node.input_size() != (gelu ? 1 : 2) || node.input(0) != previous ||
        node.output_size() != 1 || node.output(0).empty() ||
        !values.insert(node.output(0)).second ||
        node.attribute_size() != (gelu ? 1 : 0)) {
      throw std::invalid_argument("Wan FFN source has an unsupported connection or operator");
    }
    previous = node.output(0);
  }
  const auto& approximation = graph.node(2).attribute(0);
  if (previous != output || approximation.name() != "approximate" ||
      approximation.type() != onnx::AttributeProto::STRING || approximation.s() != "tanh") {
    throw std::invalid_argument("Wan FFN requires tanh Gelu and an exact output port");
  }

  const auto parameter = [&] (int node) -> const onnx::TensorProto& {
    const auto& name = graph.node(node).input(1);
    if (name.empty() || !values.insert(name).second) {
      throw std::invalid_argument("Wan FFN parameter is aliased");
    }
    const onnx::TensorProto* result = nullptr;
    for (const auto& initializer : graph.initializer()) {
      if (initializer.name() == name) {
        if (result != nullptr) {
          throw std::invalid_argument("duplicate Wan FFN initializer");
        }
        result = &initializer;
      }
    }
    if (result == nullptr || result->data_type() != onnx::TensorProto::FLOAT) {
      throw std::invalid_argument("Wan FFN parameter must be a FP32 initializer");
    }
    return *result;
  };
  const auto& expansion = parameter(0);
  const auto& expansionBias = parameter(1);
  const auto& contraction = parameter(3);
  const auto& contractionBias = parameter(4);
  if (expansion.dims_size() != 2 || contraction.dims_size() != 2 ||
      expansionBias.dims_size() != 1 || contractionBias.dims_size() != 1 ||
      expansion.dims(0) <= 0 || expansion.dims(1) < 2 ||
      contraction.dims(0) != expansion.dims(1) ||
      contraction.dims(1) != expansion.dims(textLength ? 1 : 0) ||
      expansionBias.dims(0) != expansion.dims(1) ||
      contractionBias.dims(0) != contraction.dims(1)) {
    throw std::invalid_argument("Wan FFN initializer geometry differs across projections");
  }

  const auto readPort = [textLength] (const onnx::ValueInfoProto& port, std::int64_t width) {
    const auto& tensor = port.type().tensor_type();
    const auto& shape = tensor.shape();
    if (tensor.elem_type() != onnx::TensorProto::FLOAT ||
        (shape.dim_size() != 2 && shape.dim_size() != 3) ||
        shape.dim(shape.dim_size() - 1).dim_value() != width) {
      throw std::invalid_argument("Wan FFN requires fixed FP32 token-major ports");
    }
    NativeAssemblyTensorContractV3 contract{port.name(), "float32", {}};
    for (int axis = 0; axis < shape.dim_size(); ++axis) {
      const auto& extent = shape.dim(axis);
      if (textLength && shape.dim_size() == 3 && axis == 1 &&
          extent.dim_param() == "valid_tokens") {
        contract.shape.emplace_back(extent.dim_param());
        continue;
      }
      if (!extent.has_dim_value() || extent.dim_value() <= 0)
        throw std::invalid_argument("Wan FFN requires fixed positive dimensions");
      contract.shape.emplace_back(extent.dim_value());
    }
    return contract;
  };
  const auto inputPort = readPort(graph.input(0), expansion.dims(0));
  auto outputPort = readPort(graph.output(0), contraction.dims(1));
  auto leading = inputPort.shape;
  leading.back() = outputPort.shape.back();
  if (leading != outputPort.shape)
    throw std::invalid_argument("Wan FFN token extents differ across projections");

  SummedProjection lowered;
  lowered.model.set_ir_version(source.ir_version());
  *lowered.model.mutable_opset_import() = source.opset_import();
  auto* result = lowered.model.mutable_graph();
  result->set_name(graph.name());
  *result->add_input() = graph.input(0);
  *result->add_output() = graph.output(0);
  const auto emit = [&] (const char* op, const std::string& name, const std::string& left,
                         const std::string& right, const std::string& destination) {
    auto* node = result->add_node();
    node->set_name(name);
    node->set_op_type(op);
    node->add_input(left);
    if (!right.empty()) node->add_input(right);
    node->add_output(destination);
    return node;
  };
  std::string padsName;
  bool dynamicText = false;
  const auto intVector = [&] (const std::string& name, std::initializer_list<std::int64_t> data) {
    auto* value = result->add_initializer();
    value->set_name(prefix + name);
    value->set_data_type(onnx::TensorProto::INT64);
    value->add_dims(data.size());
    for (const auto integer : data) value->add_int64_data(integer);
    return value->name();
  };
  std::string contextLimit, contextWidth, batchSize;
  if (textLength) {
    if (inputPort.shape.size() != 3 || std::get<std::int64_t>(inputPort.shape.front()) != 1)
      throw std::invalid_argument("Wan text embedding requires batch-one token-major ports");
    const auto* tokens = std::get_if<std::int64_t>(&inputPort.shape[1]);
    if (tokens && *tokens > *textLength) {
      throw std::invalid_argument("Wan text embedding length differs from source ports");
    }
    outputPort.shape[1] = *textLength;
    result->mutable_output(0)->mutable_type()->mutable_tensor_type()->mutable_shape()
      ->mutable_dim(1)->set_dim_value(*textLength);
    dynamicText = tokens == nullptr;
    if (dynamicText) {
      contextLimit = intVector("context-limit", {*textLength});
      contextWidth = intVector("context-width", {expansion.dims(0)});
      batchSize = intVector("context-batch", {1});
    }
    else if (*tokens < *textLength) {
      padsName = intVector("pads", {0, 0, 0, 0, *textLength - *tokens, 0});
    }
  }
  const auto ranges = nativeConvInputChannelRanges(expansion.dims(1), 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto name = prefix + "rank-" + std::to_string(rank) + "/";
    const auto [begin, end] = ranges[rank];
    const auto addSlice = [&] (const onnx::TensorProto& tensor, std::size_t axis,
                               const char* suffix) {
      auto* slice = result->add_initializer();
      *slice = material.slice(tensor, axis, begin, end);
      slice->set_name(name + suffix);
      return slice->name();
    };
    const auto upWeight = addSlice(expansion, 1, "up-weight");
    const auto upBias = addSlice(expansionBias, 0, "up-bias");
    const auto downWeight = addSlice(contraction, 0, "down-weight");
    auto& entry = lowered.partials[rank];
    entry.inputs = {inputPort};
    entry.outputs = {outputPort};
    entry.outputs.front().name = name + "partial";
    const auto firstNode = result->node_size();
    auto projectionInput = input;
    if (dynamicText) {
      auto* shape = emit("Shape", name + "token-axis", input, {}, name + "tokens");
      for (const auto& [key, extent] : {std::pair{"start", 1}, std::pair{"end", 2}}) {
        auto* bound = shape->add_attribute();
        bound->set_name(key);
        bound->set_type(onnx::AttributeProto::INT);
        bound->set_i(extent);
      }
      emit("Sub", name + "remaining", contextLimit, name + "tokens", name + "remaining");
      auto* joinShape = emit("Concat", name + "pad-shape", batchSize, name + "remaining",
                             name + "pad-shape");
      joinShape->add_input(contextWidth);
      auto* shapeAxis = joinShape->add_attribute();
      shapeAxis->set_name("axis");
      shapeAxis->set_type(onnx::AttributeProto::INT);
      shapeAxis->set_i(0);
      // A negative extent fails rather than truncating an oversized context.
      emit("ConstantOfShape", name + "zeros", name + "pad-shape", {}, name + "zeros");
      projectionInput = name + "padded";
      auto* join = emit("Concat", name + "pad", input, name + "zeros", projectionInput);
      auto* tokenAxis = join->add_attribute();
      tokenAxis->set_name("axis");
      tokenAxis->set_type(onnx::AttributeProto::INT);
      tokenAxis->set_i(1);
    }
    else if (!padsName.empty()) {
      // NodeMap has a single unit per node. The initializer can serve both units.
      projectionInput = name + "padded";
      emit("Pad", name + "pad", input, padsName, projectionInput);
    }
    emit("MatMul", name + "up", projectionInput, upWeight, name + "expanded");
    emit("Add", name + "add", name + "expanded", upBias, name + "biased");
    auto* gelu = emit("Gelu", name + "gelu", name + "biased", {}, name + "activated");
    *gelu->add_attribute() = approximation;
    emit("MatMul", name + "down", name + "activated", downWeight, entry.outputs.front().name);
    for (auto index = firstNode; index < result->node_size(); ++index) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(index));
    }
  }
  const auto unbiased = prefix + "unbiased";
  emit("Add", prefix + "sum", lowered.partials[0].outputs.front().name,
       lowered.partials[1].outputs.front().name, unbiased);
  auto* bias = result->add_initializer();
  *bias = material.slice(contractionBias, 0, 0, contractionBias.dims(0));
  bias->set_name(prefix + "down-bias");
  lowered.addBias = {{static_cast<std::uint64_t>(result->node_size())}, {outputPort}, {outputPort}};
  lowered.addBias.inputs.front().name = unbiased;
  emit("Add", prefix + "add-bias", unbiased, bias->name(), output);
  return lowered;
}

} // namespace

SummedProjection
lowerFeedForward(const onnx::ModelProto& source, const WanInitializerAccess& material)
{
  return lowerProjection(source, material, std::nullopt);
}

SummedProjection
lowerTextEmbedding(const onnx::ModelProto& source, std::int64_t textLength,
                   const WanInitializerAccess& material)
{
  if (textLength <= 0) {
    throw std::invalid_argument("Wan text embedding length must be positive");
  }
  return lowerProjection(source, material, textLength);
}

} // namespace ndnsf::di::wan
