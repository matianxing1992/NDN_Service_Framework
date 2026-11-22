#include "wan-video-rotary.hpp"

#include "wan-attention-tile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::wan {

onnx::ModelProto
makeVideoRotaryEmbedding(const VideoRotaryTile& tile, std::uint64_t tableBytes)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (tile.headWidth < 6 || tile.headWidth % 2 != 0 || tile.heads <= 0 ||
      tile.firstToken < 0 || tile.tokens <= 0 || tile.firstToken > LIMIT - (tile.tokens - 1) ||
      tile.frames <= 0 || tile.height <= 0 || tile.width <= 0 ||
      tile.height > LIMIT / tile.width || tile.frames > LIMIT / (tile.height * tile.width)) {
    throw std::invalid_argument("invalid Wan video rotary grid or tile");
  }
  const auto pairs = tile.headWidth / 2;
  if (static_cast<std::uint64_t>(tile.tokens) > tableBytes / (2 * sizeof(double)) / pairs ||
      tile.heads > LIMIT / tile.tokens / tile.headWidth) {
    throw std::invalid_argument("Wan video rotary tile exceeds its size budget");
  }
  const auto spatial = tile.height * tile.width;
  const auto gridTokens = tile.frames * spatial;
  const auto spatialPairs = pairs / 3;
  const std::array<std::int64_t, 3> axisPairs{pairs - 2 * spatialPairs, spatialPairs, spatialPairs};
  const std::array<std::int64_t, 3> geometry{tile.frames, tile.height, tile.width};
  // Account for the small graph needed to derive and gather three coordinates.
  constexpr std::uint64_t AXIS_GRAPH_OVERHEAD = 4096;
  const auto denseCoefficients = static_cast<std::uint64_t>(tile.tokens) * pairs;
  auto remaining = denseCoefficients -
    std::min(denseCoefficients, AXIS_GRAPH_OVERHEAD / (2 * sizeof(double)));
  bool useAxes = true;
  for (std::size_t axis = 0; axis < geometry.size(); ++axis) {
    if (static_cast<std::uint64_t>(geometry[axis]) > remaining / axisPairs[axis]) {
      useAxes = false;
      break;
    }
    remaining -= static_cast<std::uint64_t>(geometry[axis]) * axisPairs[axis];
  }

  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-video-rotary");
  const auto declare = [&] (onnx::ValueInfoProto& info, const char* name) {
    info.set_name(name);
    auto* tensor = info.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (auto extent : {std::int64_t{1}, tile.heads, tile.tokens, tile.headWidth}) {
      tensor->mutable_shape()->add_dim()->set_dim_value(extent);
    }
  };
  declare(*graph->add_input(), "input");
  declare(*graph->add_output(), "rotated");
  const auto integers = [&] (const char* name, std::initializer_list<std::int64_t> values,
                             bool scalar = false) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    if (!scalar) tensor->add_dims(static_cast<std::int64_t>(values.size()));
    for (auto value : values) tensor->add_int64_data(value);
  };
  integers("complex-shape", {1, tile.heads, tile.tokens, pairs, 2});
  integers("output-shape", {1, tile.heads, tile.tokens, tile.headWidth});
  integers("real-index", {0}, true);
  integers("imag-index", {1}, true);
  integers("last-axis", {-1});

  const auto emit = [&] (const char* op, const char* name,
                         std::initializer_list<const char*> inputs) {
    auto* node = graph->add_node();
    node->set_name(name);
    node->set_op_type(op);
    for (const auto* input : inputs) node->add_input(input);
    node->add_output(name);
    return node;
  };
  const auto attribute = [] (onnx::NodeProto& node, const char* name, std::int64_t value) {
    auto* attr = node.add_attribute();
    attr->set_name(name);
    attr->set_type(onnx::AttributeProto::INT);
    attr->set_i(value);
  };

  const auto trig = [](bool sine, auto position, auto pair, auto count) {
    const auto theta = static_cast<double>(position) /
      std::pow(10000.0, static_cast<double>(pair) / static_cast<double>(count));
    return sine ? std::sin(theta) : std::cos(theta);
  };
  if (useAxes) {
    // Store only axis tables. Deriving the same token coordinates in ONNX keeps
    // repeated transformer layers from embedding a complete RoPE table each time.
    integers("token-first", {tile.firstToken}, true);
    integers("token-count", {tile.tokens}, true);
    integers("token-step", {1}, true);
    integers("token-zero", {0}, true);
    integers("grid-tokens", {gridTokens}, true);
    integers("grid-spatial", {spatial}, true);
    integers("grid-width", {tile.width}, true);
    emit("Range", "relative-token", {"token-zero", "token-count", "token-step"});
    emit("Add", "absolute-token", {"relative-token", "token-first"});
    emit("Less", "in-grid", {"absolute-token", "grid-tokens"});
    emit("Where", "position", {"in-grid", "absolute-token", "token-zero"});
    emit("Div", "frame-index", {"position", "grid-spatial"});
    emit("Mod", "spatial-index", {"position", "grid-spatial"});
    emit("Div", "height-index", {"spatial-index", "grid-width"});
    emit("Mod", "width-index", {"spatial-index", "grid-width"});
    const std::array<const char*, 3> coordinates{"frame-index", "height-index", "width-index"};
    for (const bool sine : {false, true}) {
      const std::string name = sine ? "sine" : "cosine";
      onnx::NodeProto join;
      join.set_op_type("Concat");
      join.set_name(name);
      join.add_output(name);
      attribute(join, "axis", 1);
      for (std::size_t axis = 0; axis < geometry.size(); ++axis) {
        const auto selected = name + "-axis-" + std::to_string(axis);
        const auto tableName = selected + "-table";
        auto* table = graph->add_initializer();
        table->set_name(tableName);
        table->set_data_type(onnx::TensorProto::DOUBLE);
        table->add_dims(geometry[axis]);
        table->add_dims(axisPairs[axis]);
        for (std::int64_t position = 0; position < geometry[axis]; ++position) {
          for (std::int64_t pair = 0; pair < axisPairs[axis]; ++pair) {
            table->add_double_data(trig(sine, position, pair, axisPairs[axis]));
          }
        }
        emit("Gather", selected.c_str(), {tableName.c_str(), coordinates[axis]});
        join.add_input(selected);
      }
      *graph->add_node() = std::move(join);
    }
  }
  else {
    // A narrow tile can use fewer coefficients by materializing just its own rows.
    for (const bool sine : {false, true}) {
      auto* table = graph->add_initializer();
      table->set_name(sine ? "sine" : "cosine");
      table->set_data_type(onnx::TensorProto::DOUBLE);
      table->add_dims(tile.tokens);
      table->add_dims(pairs);
      for (std::int64_t row = 0; row < tile.tokens; ++row) {
        const auto absolute = tile.firstToken + row;
        const auto token = absolute < gridTokens ? absolute : 0;
        const std::array<std::int64_t, 3> positions{
          token / spatial, token % spatial / tile.width, token % tile.width};
        for (std::size_t axis = 0; axis < positions.size(); ++axis) {
          for (std::int64_t pair = 0; pair < axisPairs[axis]; ++pair)
            table->add_double_data(trig(sine, positions[axis], pair, axisPairs[axis]));
        }
      }
    }
  }
  attribute(*emit("Cast", "input-fp64", {"input"}), "to", onnx::TensorProto::DOUBLE);
  emit("Reshape", "complex", {"input-fp64", "complex-shape"});
  attribute(*emit("Gather", "real", {"complex", "real-index"}), "axis", -1);
  attribute(*emit("Gather", "imag", {"complex", "imag-index"}), "axis", -1);
  emit("Mul", "real-cos", {"real", "cosine"});
  emit("Mul", "imag-sin", {"imag", "sine"});
  emit("Mul", "real-sin", {"real", "sine"});
  emit("Mul", "imag-cos", {"imag", "cosine"});
  emit("Sub", "rotated-real", {"real-cos", "imag-sin"});
  emit("Add", "rotated-imag", {"real-sin", "imag-cos"});
  emit("Unsqueeze", "real-column", {"rotated-real", "last-axis"});
  emit("Unsqueeze", "imag-column", {"rotated-imag", "last-axis"});
  attribute(*emit("Concat", "rotated-complex", {"real-column", "imag-column"}), "axis", -1);
  emit("Reshape", "rotated-fp64", {"rotated-complex", "output-shape"});
  attribute(*emit("Cast", "rotated", {"rotated-fp64"}), "to", onnx::TensorProto::FLOAT);
  return model;
}

onnx::ModelProto
makeVideoAttentionQueryBlock(const VideoRotaryTile& query, std::int64_t contextTokens,
                             std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                             std::uint64_t rotaryBytes)
{
  if (contextTokens <= 0 || query.tokens <= 0 || query.tokens > contextTokens ||
      query.firstToken < 0 || query.firstToken > contextTokens - query.tokens ||
      query.frames <= 0 || query.height <= 0 || query.width <= 0 ||
      query.frames > contextTokens / query.height / query.width) {
    throw std::invalid_argument("Wan attention query or grid exceeds the padded context");
  }
  auto rotary = makeVideoRotaryEmbedding(query, rotaryBytes);
  auto model = makeAttentionQueryBlock(
    {1, query.heads, query.tokens, keyTileTokens, query.headWidth, true},
    contextTokens, scoreBytes);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-video-attention-query-block");
  graph->mutable_input(0)->set_name("query-unrotated");

  // The key Loop captures the outer value named "query". Produce that name
  // before the existing attention nodes to preserve the Loop's bindings.
  onnx::GraphProto attention;
  attention.mutable_node()->Swap(graph->mutable_node());
  const auto rotaryPort = [] (const std::string& name) -> std::string {
    if (name == "input") return "query-unrotated";
    if (name == "rotated") return "query";
    return "query-rope/" + name;
  };
  auto& rotaryGraph = *rotary.mutable_graph();
  for (auto& tensor : *rotaryGraph.mutable_initializer()) {
    auto* initializer = graph->add_initializer();
    *initializer = std::move(tensor);
    initializer->set_name(rotaryPort(initializer->name()));
  }
  for (auto& node : *rotaryGraph.mutable_node()) {
    auto* emitted = graph->add_node();
    *emitted = std::move(node);
    emitted->set_name("query-rope/" + emitted->name());
    for (auto& port : *emitted->mutable_input()) port = rotaryPort(port);
    for (auto& port : *emitted->mutable_output()) port = rotaryPort(port);
  }
  for (auto& node : *attention.mutable_node()) *graph->add_node() = std::move(node);
  return model;
}

} // namespace ndnsf::di::wan
