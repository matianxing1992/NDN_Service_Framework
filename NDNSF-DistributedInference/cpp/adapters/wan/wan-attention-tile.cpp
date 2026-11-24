#include "wan-attention-tile.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace ndnsf::di::wan {

onnx::ModelProto
makeAttentionTileUpdate(const AttentionTileShape& shape, std::uint64_t scoreBytes)
{
  if (shape.headWidth <= 0) {
    throw std::invalid_argument("Wan attention head width must be positive");
  }
  std::uint64_t bytes = sizeof(float);
  for (const auto extent : {shape.batches, shape.heads, shape.queries, shape.keys}) {
    if (extent <= 0 || static_cast<std::uint64_t>(extent) > scoreBytes / bytes) {
      throw std::invalid_argument("Wan attention score tile exceeds its byte budget");
    }
    bytes *= static_cast<std::uint64_t>(extent);
  }

  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-attention-tile");
  const auto declare = [] (onnx::ValueInfoProto& value, const char* name,
                           std::initializer_list<std::int64_t> dims,
                           int type = onnx::TensorProto::FLOAT) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    for (auto extent : dims) tensor->mutable_shape()->add_dim()->set_dim_value(extent);
  };
  const auto valueShape = {shape.batches, shape.heads, shape.queries, shape.headWidth};
  const auto statisticShape = {shape.batches, shape.heads, shape.queries, std::int64_t{1}};
  const auto contextShape = {shape.batches, shape.heads, shape.keys, shape.headWidth};
  declare(*graph->add_input(), "query", valueShape);
  declare(*graph->add_input(), "key", contextShape);
  declare(*graph->add_input(), "value", contextShape);
  declare(*graph->add_input(), "valid",
          {shape.batches, 1, shape.keyOnlyMask ? 1 : shape.queries, shape.keys},
          onnx::TensorProto::BOOL);
  declare(*graph->add_input(), "prior-max", statisticShape);
  declare(*graph->add_input(), "prior-denominator", statisticShape);
  declare(*graph->add_input(), "prior-weighted", valueShape);
  declare(*graph->add_output(), "next-max", statisticShape);
  declare(*graph->add_output(), "next-denominator", statisticShape);
  declare(*graph->add_output(), "next-weighted", valueShape);

  const auto scalar = [&] (const char* name, float value) {
    auto* constant = graph->add_initializer();
    constant->set_name(name);
    constant->set_data_type(onnx::TensorProto::FLOAT);
    constant->add_float_data(value);
  };
  const auto headScale = 1.0 / std::sqrt(static_cast<double>(shape.headWidth));
  scalar("operand-scale", static_cast<float>(std::sqrt(headScale)));
  scalar("masked-score", std::numeric_limits<float>::lowest());
  scalar("zero", 0);
  auto* axes = graph->add_initializer();
  axes->set_name("last-axis");
  axes->set_data_type(onnx::TensorProto::INT64);
  axes->add_dims(1);
  axes->add_int64_data(-1);

  const auto emit = [&] (const char* op, const char* name,
                         std::initializer_list<const char*> inputs) {
    auto* node = graph->add_node();
    node->set_op_type(op);
    node->set_name(name);
    for (const auto* input : inputs) node->add_input(input);
    node->add_output(name);
    return node;
  };
  auto* transpose = emit("Transpose", "transposed-key", {"key"})->add_attribute();
  transpose->set_name("perm");
  transpose->set_type(onnx::AttributeProto::INTS);
  for (auto value : {0, 1, 3, 2}) transpose->add_ints(value);
  // Scale both operands before the dot product, matching Wan's math attention.
  emit("Mul", "scaled-query", {"query", "operand-scale"});
  emit("Mul", "scaled-key", {"transposed-key", "operand-scale"});
  emit("MatMul", "scaled-scores", {"scaled-query", "scaled-key"});
  emit("Where", "scores", {"valid", "scaled-scores", "masked-score"});
  emit("ReduceMax", "tile-max", {"scores", "last-axis"});
  emit("Max", "next-max", {"prior-max", "tile-max"});
  emit("Sub", "prior-centered", {"prior-max", "next-max"});
  emit("Exp", "prior-rescale", {"prior-centered"});
  emit("Sub", "tile-centered", {"scores", "next-max"});
  emit("Exp", "tile-exp", {"tile-centered"});
  // An entirely masked row can have max == masked-score. Zero it after Exp
  // so that Exp(masked-score - masked-score) cannot affect the denominator.
  emit("Where", "tile-weights", {"valid", "tile-exp", "zero"});
  emit("ReduceSum", "tile-denominator", {"tile-weights", "last-axis"});
  emit("Mul", "rescaled-denominator", {"prior-denominator", "prior-rescale"});
  emit("Add", "next-denominator", {"rescaled-denominator", "tile-denominator"});
  emit("MatMul", "tile-weighted", {"tile-weights", "value"});
  emit("Mul", "rescaled-weighted", {"prior-weighted", "prior-rescale"});
  emit("Add", "next-weighted", {"rescaled-weighted", "tile-weighted"});
  return model;
}

onnx::ModelProto
makeAttentionQueryBlock(const AttentionTileShape& shape, std::int64_t contextTokens,
                        std::uint64_t scoreBytes)
{
  auto update = makeAttentionTileUpdate(shape, scoreBytes);
  if (contextTokens <= 0 || contextTokens % shape.keys != 0) {
    throw std::invalid_argument("Wan attention context must cover complete key tiles");
  }
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (contextTokens > LIMIT / shape.batches / shape.heads / shape.headWidth ||
      contextTokens > LIMIT / shape.batches / (shape.keyOnlyMask ? 1 : shape.queries)) {
    throw std::invalid_argument("Wan attention context shape exceeds addressable range");
  }

  onnx::ModelProto model;
  model.set_ir_version(update.ir_version());
  *model.mutable_opset_import() = update.opset_import();
  auto* graph = model.mutable_graph();
  graph->set_name("wan-attention-query-block");
  auto& tile = *update.mutable_graph();
  *graph->add_input() = tile.input(0);
  constexpr const char* CONTEXT_PORTS[] = {"context-key", "context-value", "context-valid"};
  for (int i = 0; i < 3; ++i) {
    auto* value = graph->add_input();
    *value = tile.input(i + 1);
    value->set_name(CONTEXT_PORTS[i]);
    const auto axis = i == 2 ? 3 : 2;
    value->mutable_type()->mutable_tensor_type()->mutable_shape()
      ->mutable_dim(axis)->set_dim_value(contextTokens);
  }
  *graph->add_output() = tile.output(2);
  graph->mutable_output(0)->set_name("attention");

  const auto constant = [] (onnx::GraphProto& owner, const char* name, int type) {
    auto* tensor = owner.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(type);
    return tensor;
  };
  const auto emit = [] (onnx::GraphProto& owner, const char* op, const char* output,
                        std::initializer_list<const char*> inputs) {
    auto* node = owner.add_node();
    node->set_name(output);
    node->set_op_type(op);
    for (const auto* input : inputs) node->add_input(input);
    node->add_output(output);
    return node;
  };
  const auto integer = [&] (onnx::GraphProto& owner, const char* name,
                            std::int64_t value, bool vector) {
    auto* tensor = constant(owner, name, onnx::TensorProto::INT64);
    if (vector) tensor->add_dims(1);
    tensor->add_int64_data(value);
  };

  integer(*graph, "trip-count", contextTokens / shape.keys, false);
  constant(*graph, "keep-running", onnx::TensorProto::BOOL)->add_int32_data(1);
  constant(*graph, "initial-lowest", onnx::TensorProto::FLOAT)
    ->add_float_data(std::numeric_limits<float>::lowest());
  constant(*graph, "initial-zero", onnx::TensorProto::FLOAT)->add_float_data(0);
  constant(*graph, "safe-unit", onnx::TensorProto::FLOAT)->add_float_data(1);
  auto* statsShape = constant(*graph, "stats-shape", onnx::TensorProto::INT64);
  statsShape->add_dims(4);
  for (const auto dim : {shape.batches, shape.heads, shape.queries, std::int64_t{1}}) {
    statsShape->add_int64_data(dim);
  }
  auto* valueShape = constant(*graph, "value-shape", onnx::TensorProto::INT64);
  valueShape->add_dims(4);
  for (const auto dim : {shape.batches, shape.heads, shape.queries, shape.headWidth}) {
    valueShape->add_int64_data(dim);
  }
  emit(*graph, "Expand", "initial-max", {"initial-lowest", "stats-shape"});
  emit(*graph, "Expand", "initial-denominator", {"initial-zero", "stats-shape"});
  emit(*graph, "Expand", "initial-weighted", {"initial-zero", "value-shape"});

  auto* loop = emit(*graph, "Loop", "final-max",
                    {"trip-count", "keep-running", "initial-max",
                     "initial-denominator", "initial-weighted"});
  loop->add_output("final-denominator");
  loop->add_output("final-weighted");
  auto* bodyAttribute = loop->add_attribute();
  bodyAttribute->set_name("body");
  bodyAttribute->set_type(onnx::AttributeProto::GRAPH);
  auto& body = *bodyAttribute->mutable_g();
  body.set_name("wan-attention-key-block");
  const auto scalarPort = [] (onnx::ValueInfoProto& out, const char* name, int type) {
    out.set_name(name);
    auto* tensor = out.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(type);
    tensor->mutable_shape();
  };
  scalarPort(*body.add_input(), "iteration", onnx::TensorProto::INT64);
  scalarPort(*body.add_input(), "condition", onnx::TensorProto::BOOL);
  for (int i = 4; i < tile.input_size(); ++i) *body.add_input() = tile.input(i);
  scalarPort(*body.add_output(), "condition-out", onnx::TensorProto::BOOL);
  *body.mutable_output()->Add() = tile.output(0);
  *body.mutable_output()->Add() = tile.output(1);
  *body.mutable_output()->Add() = tile.output(2);
  *body.mutable_initializer() = tile.initializer();
  integer(body, "key-tile-width", shape.keys, false);
  integer(body, "to-vector", 0, true);
  integer(body, "token-axis", 2, true);
  integer(body, "mask-axis", 3, true);
  emit(body, "Identity", "condition-out", {"condition"});
  emit(body, "Mul", "start", {"iteration", "key-tile-width"});
  emit(body, "Add", "end", {"start", "key-tile-width"});
  emit(body, "Unsqueeze", "starts", {"start", "to-vector"});
  emit(body, "Unsqueeze", "ends", {"end", "to-vector"});
  emit(body, "Slice", "key", {"context-key", "starts", "ends", "token-axis"});
  emit(body, "Slice", "value", {"context-value", "starts", "ends", "token-axis"});
  emit(body, "Slice", "valid", {"context-valid", "starts", "ends", "mask-axis"});
  for (const auto& node : tile.node()) *body.add_node() = node;

  emit(*graph, "Equal", "empty-query", {"final-denominator", "initial-zero"});
  emit(*graph, "Where", "safe-denominator", {"empty-query", "safe-unit", "final-denominator"});
  emit(*graph, "Div", "attention", {"final-weighted", "safe-denominator"});
  return model;
}

} // namespace ndnsf::di::wan
