#include "wan-attention-context.hpp"

#include "wan-attention-tile.hpp"
#include "wan-graph-fragment.hpp"
#include "wan-head-projection.hpp"
#include "wan-normalized-projection.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di::wan {
namespace {

std::vector<std::int64_t>
staticFloatShape(const onnx::ValueInfoProto& port)
{
  if (!port.type().has_tensor_type() ||
      port.type().tensor_type().elem_type() != onnx::TensorProto::FLOAT) {
    throw std::invalid_argument("Wan attention context port must be a FP32 tensor");
  }
  std::vector<std::int64_t> dimensions;
  for (const auto& dim : port.type().tensor_type().shape().dim()) {
    if (!dim.has_dim_value() || dim.dim_value() <= 0) {
      throw std::invalid_argument("Wan attention context port must have a fixed positive shape");
    }
    dimensions.push_back(dim.dim_value());
  }
  return dimensions;
}

std::int64_t
standardOpset(const onnx::ModelProto& model)
{
  if (model.opset_import_size() != 1 || !model.opset_import(0).domain().empty()) {
    throw std::invalid_argument("Wan attention context requires one standard ONNX opset");
  }
  return model.opset_import(0).version();
}

std::array<std::string, 2>
detachJoin(onnx::GraphProto& graph, const std::string& prefix)
{
  const auto& join = graph.node(graph.node_size() - 1);
  if (join.op_type() != "Concat" || join.input_size() != 2) {
    throw std::logic_error("Wan projection must end with a two-rank join");
  }
  const std::array<std::string, 2> rankValues{prefix + join.input(0), prefix + join.input(1)};
  graph.mutable_node()->RemoveLast();
  return rankValues;
}

void
requireProjectionMatrix(const onnx::GraphProto& graph, std::int64_t inputWidth,
                        std::int64_t outputWidth)
{
  // The validated projection always takes its weight as MatMul's second operand.
  const auto& name = graph.node(0).input(1);
  const auto matrix = std::find_if(graph.initializer().begin(), graph.initializer().end(),
                                  [&] (const auto& tensor) { return tensor.name() == name; });
  if (matrix == graph.initializer().end() || matrix->dims(0) != inputWidth ||
      matrix->dims(1) != outputWidth) {
    throw std::invalid_argument("Wan attention projection weight dimensions disagree with ports");
  }
}

struct HeadLayout
{
  std::int64_t heads;
  std::int64_t headWidth;
  std::int64_t tokens;
};

using TensorPort = NativeAssemblyTensorContractV3;

TensorPort
attentionPort(std::string name, const HeadLayout& tile)
{
  return {std::move(name), "float32", {std::int64_t{1}, tile.heads, tile.tokens, tile.headWidth}};
}

void
appendNodesFrom(OnnxEntryCut& cut, const onnx::GraphProto& graph, int first)
{
  for (int i = first; i < graph.node_size(); ++i)
    cut.nodeIndices.push_back(static_cast<std::uint64_t>(i));
}

std::int64_t
tokenCount(const onnx::ModelProto& source)
{
  if (source.graph().input_size() != 1) {
    throw std::invalid_argument("Wan projection requires one input");
  }
  const auto dims = staticFloatShape(source.graph().input(0));
  if (dims.size() != 3 || dims[0] != 1) {
    throw std::invalid_argument("Wan projection requires one batch of token-major input");
  }
  return dims[1];
}

void
addHeadLayoutTransform(onnx::GraphProto& graph, const std::string& input,
                       const std::string& output, const HeadLayout& tile, bool toHeads)
{
  auto* shape = graph.add_initializer();
  shape->set_name(output + "/shape");
  shape->set_data_type(onnx::TensorProto::INT64);
  const std::vector<std::int64_t> dims = toHeads
    ? std::vector<std::int64_t>{1, tile.tokens, tile.heads, tile.headWidth}
    : std::vector<std::int64_t>{1, tile.tokens, tile.heads * tile.headWidth};
  shape->add_dims(dims.size());
  for (auto dimension : dims) shape->add_int64_data(dimension);
  auto* first = graph.add_node();
  auto* second = graph.add_node();
  auto* reshape = toHeads ? first : second;
  auto* transpose = toHeads ? second : first;
  const auto tokenMajor = output + "/token-major";
  reshape->set_op_type("Reshape");
  reshape->add_input(toHeads ? input : tokenMajor);
  reshape->add_input(shape->name());
  reshape->add_output(toHeads ? tokenMajor : output);
  transpose->set_op_type("Transpose");
  transpose->add_input(toHeads ? tokenMajor : input);
  transpose->add_output(toHeads ? output : tokenMajor);
  auto* perm = transpose->add_attribute();
  perm->set_name("perm");
  perm->set_type(onnx::AttributeProto::INTS);
  for (auto axis : {0, 2, 1, 3}) perm->add_ints(axis);
}

AttentionContextLayout
lowerAttentionContext(const onnx::ModelProto& key, const onnx::ModelProto& value,
                      const HeadLayout& context, const VideoRotaryTile* video,
                      std::uint64_t rotaryBytes, const WanInitializerAccess& material)
{
  if (context.heads < 2 || context.headWidth <= 0 || context.tokens <= 0 ||
      context.heads > std::numeric_limits<std::int64_t>::max() / context.headWidth) {
    throw std::invalid_argument("invalid Wan attention context head geometry");
  }
  const auto opset = standardOpset(key);
  if (opset < 18 || opset != standardOpset(value)) {
    throw std::invalid_argument("Wan key and value must share a supported ONNX opset");
  }

  auto keyLayout = lowerNormalizedProjection(key, context.headWidth, material);
  auto& keys = keyLayout.model;
  auto valueLayout = lowerHeadProjection(value, context.headWidth,
                                          ProjectionParallelism::OUTPUT_CHANNELS, material);
  auto& values = valueLayout.model;
  const auto keyInput = staticFloatShape(keys.graph().input(0));
  const std::vector<std::int64_t> expected{1, context.tokens, context.heads * context.headWidth};
  if (keyInput.size() != 3 || keyInput[0] != 1 || keyInput[1] != context.tokens ||
      keyInput != staticFloatShape(values.graph().input(0)) ||
      expected != staticFloatShape(keys.graph().output(0)) ||
      expected != staticFloatShape(values.graph().output(0))) {
    throw std::invalid_argument("Wan attention context projection ports disagree with the grid");
  }
  requireProjectionMatrix(key.graph(), keyInput.back(), expected.back());
  requireProjectionMatrix(value.graph(), keyInput.back(), expected.back());
  const auto keyRanks = detachJoin(*keys.mutable_graph(), "key/");
  const auto valueRanks = detachJoin(*values.mutable_graph(), "value/");

  AttentionContextLayout layout;
  auto& model = layout.model;
  model.set_ir_version(std::max({std::int64_t{9}, key.ir_version(), value.ir_version()}));
  *model.mutable_opset_import() = key.opset_import();
  auto* graph = model.mutable_graph();
  graph->set_name(video ? "wan-video-attention-context" : "wan-text-attention-context");
  *graph->add_input() = keys.graph().input(0);
  graph->mutable_input(0)->set_name("hidden");
  const std::map<std::string, std::string> keyPorts{{keys.graph().input(0).name(), "hidden"}};
  const std::map<std::string, std::string> valuePorts{{values.graph().input(0).name(), "hidden"}};
  appendFragment(*graph, std::move(*keys.mutable_graph()), "key/", keyPorts);
  const auto valueOffset = static_cast<std::uint64_t>(graph->node_size());
  appendFragment(*graph, std::move(*values.mutable_graph()), "value/", valuePorts);

  const auto ranges = nativeConvInputChannelRanges(context.heads, 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    auto& cuts = layout.ranks[rank];
    cuts.project = rebaseCut(std::move(keyLayout.ranks[rank].project), 0, "key/", keyPorts);
    cuts.prepare = rebaseCut(std::move(keyLayout.ranks[rank].normalize), 0, "key/", keyPorts);
    const auto localValue = rebaseCut(std::move(valueLayout.ranks[rank]),
                                      valueOffset, "value/", valuePorts);
    cuts.project.nodeIndices.insert(cuts.project.nodeIndices.end(),
                                    localValue.nodeIndices.begin(), localValue.nodeIndices.end());
    cuts.project.outputs.push_back(localValue.outputs.front());
    cuts.prepare.inputs.push_back(localValue.outputs.front());
    const auto prepareStart = graph->node_size();
    auto tile = context;
    tile.heads = static_cast<std::int64_t>(ranges[rank].second - ranges[rank].first);
    const auto rankName = "rank-" + std::to_string(rank);
    const auto keyOutput = "key-" + rankName;
    const auto valueOutput = "value-" + rankName;
    addHeadLayoutTransform(*graph, keyRanks[rank], video ? keyOutput + "/unrotated" : keyOutput,
                           tile, true);
    addHeadLayoutTransform(*graph, valueRanks[rank], valueOutput, tile, true);
    for (const auto* name : {&keyOutput, &valueOutput}) {
      auto* port = graph->add_output();
      port->set_name(*name);
      auto* tensor = port->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto::FLOAT);
      for (auto dim : {std::int64_t{1}, tile.heads, tile.tokens, tile.headWidth}) {
        tensor->mutable_shape()->add_dim()->set_dim_value(dim);
      }
    }
    if (video != nullptr) {
      auto local = *video;
      local.heads = tile.heads;
      auto rotary = makeVideoRotaryEmbedding(local, rotaryBytes / 2);
      appendFragment(*graph, std::move(*rotary.mutable_graph()), keyOutput + "/rotary/",
                     {{"input", keyOutput + "/unrotated"}, {"rotated", keyOutput}});
    }
    appendNodesFrom(cuts.prepare, *graph, prepareStart);
    cuts.prepare.outputs = {attentionPort(keyOutput, tile), attentionPort(valueOutput, tile)};
  }
  return layout;
}

AttentionQueryLayout
lowerAttentionQuery(const onnx::ModelProto& query, const onnx::ModelProto& output,
                    const HeadLayout& tile, const VideoRotaryTile* video,
                    std::int64_t contextTokens, std::int64_t keyTileTokens,
                    std::uint64_t scoreBytes, std::uint64_t rotaryBytes,
                    const WanInitializerAccess& material)
{
  const auto opset = standardOpset(query);
  if (opset < 18 || opset != standardOpset(output) || tile.heads < 2 ||
      tile.headWidth <= 0 || tile.tokens <= 0 ||
      tile.heads > std::numeric_limits<std::int64_t>::max() / tile.headWidth) {
    throw std::invalid_argument("invalid Wan attention query opset or head geometry");
  }
  auto queryLayout = lowerNormalizedProjection(query, tile.headWidth, material);
  auto& queries = queryLayout.model;
  auto projectionLayout = lowerHeadProjection(output, tile.headWidth,
                                               ProjectionParallelism::INPUT_CHANNELS, material);
  auto& projection = projectionLayout.model;
  const auto hiddenShape = staticFloatShape(queries.graph().input(0));
  const std::vector<std::int64_t> projectedShape{1, tile.tokens, tile.heads * tile.headWidth};
  if (hiddenShape.size() != 3 || hiddenShape[0] != 1 || hiddenShape[1] != tile.tokens ||
      projectedShape != staticFloatShape(queries.graph().output(0)) ||
      projectedShape != staticFloatShape(projection.graph().input(0)) ||
      hiddenShape != staticFloatShape(projection.graph().output(0))) {
    throw std::invalid_argument("Wan attention Q/O ports disagree with the query tile");
  }
  requireProjectionMatrix(query.graph(), hiddenShape.back(), projectedShape.back());
  requireProjectionMatrix(output.graph(), projectedShape.back(), hiddenShape.back());
  const auto queryRanks = detachJoin(*queries.mutable_graph(), "query/");
  const auto& split = projection.graph().node(0);
  if (split.op_type() != "Split" || split.output_size() != 2 ||
      projection.graph().initializer(0).name() != split.input(1)) {
    throw std::logic_error("Wan attention O lowering must start with the rank split");
  }
  std::map<std::string, std::string> outputBindings{
    {split.output(0), "attention-rank-0"}, {split.output(1), "attention-rank-1"},
    {projection.graph().output(0).name(), "output"}
  };
  projection.mutable_graph()->mutable_node()->DeleteSubrange(0, 1);
  projection.mutable_graph()->mutable_initializer()->DeleteSubrange(0, 1);

  AttentionQueryLayout layout;
  auto& model = layout.model;
  model.set_ir_version(std::max({std::int64_t{9}, query.ir_version(), output.ir_version()}));
  *model.mutable_opset_import() = query.opset_import();
  auto* graph = model.mutable_graph();
  graph->set_name(video ? "wan-video-attention-query" : "wan-text-attention-query");
  *graph->add_input() = queries.graph().input(0);
  graph->mutable_input(0)->set_name("hidden");
  *graph->add_output() = projection.graph().output(0);
  graph->mutable_output(0)->set_name("output");
  const std::map<std::string, std::string> queryBindings{{queries.graph().input(0).name(), "hidden"}};
  appendFragment(*graph, std::move(*queries.mutable_graph()), "query/", queryBindings);

  if (video == nullptr) {
    auto* maskShape = graph->add_initializer();
    maskShape->set_name("text-mask-shape");
    maskShape->set_data_type(onnx::TensorProto::INT64);
    maskShape->add_dims(4);
    for (const auto extent : {std::int64_t{1}, std::int64_t{1}, std::int64_t{1}, contextTokens}) {
      maskShape->add_int64_data(extent);
    }
  }

  const auto ranges = nativeConvInputChannelRanges(tile.heads, 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    auto& cuts = layout.ranks[rank];
    cuts.project = rebaseCut(std::move(queryLayout.ranks[rank].project),
                             0, "query/", queryBindings);
    cuts.attend = rebaseCut(std::move(queryLayout.ranks[rank].normalize),
                            0, "query/", queryBindings);
    const auto attendStart = graph->node_size();
    auto local = tile;
    local.heads = static_cast<std::int64_t>(ranges[rank].second - ranges[rank].first);
    const auto rankName = "rank-" + std::to_string(rank);
    const auto queryName = "query-" + rankName;
    const auto keyName = "key-" + rankName;
    const auto valueName = "value-" + rankName;
    const auto maskName = video != nullptr ? std::string("valid") : "text-valid-" + rankName;
    if (video == nullptr) {
      auto* mask = graph->add_node();
      mask->set_op_type("ConstantOfShape");
      mask->add_input("text-mask-shape");
      mask->add_output(maskName);
      auto* attribute = mask->add_attribute();
      attribute->set_name("value");
      attribute->set_type(onnx::AttributeProto::TENSOR);
      auto* scalar = attribute->mutable_t();
      scalar->set_data_type(onnx::TensorProto::BOOL);
      scalar->add_dims(1);
      scalar->add_int32_data(1);
    }
    addHeadLayoutTransform(*graph, queryRanks[rank], queryName, local, true);
    auto attention = [&] {
      if (video == nullptr) {
        return makeAttentionQueryBlock(
          {1, local.heads, local.tokens, keyTileTokens, local.headWidth, true},
          contextTokens, scoreBytes);
      }
      auto rotary = *video;
      rotary.heads = local.heads;
      return makeVideoAttentionQueryBlock(rotary, contextTokens, keyTileTokens,
                                          scoreBytes, rotaryBytes / 2);
    }();
    for (int i = 1; i <= 2; ++i) {
      auto* port = graph->add_input();
      *port = attention.graph().input(i);
      port->set_name(i == 1 ? keyName : valueName);
    }
    auto context = local;
    context.tokens = contextTokens;
    cuts.attend.inputs.push_back(attentionPort(keyName, context));
    cuts.attend.inputs.push_back(attentionPort(valueName, context));
    if (video != nullptr) {
      cuts.attend.inputs.push_back(
        {maskName, "bool", {std::int64_t{1}, std::int64_t{1}, std::int64_t{1}, contextTokens}});
    }
    if (video != nullptr && rank == ranges.size() - 1) {
      auto* mask = graph->add_input();
      *mask = attention.graph().input(3);
      mask->set_name("valid");
    }
    const auto localResult = "attended-" + rankName;
    appendFragment(*graph, std::move(*attention.mutable_graph()), rankName + "/",
                   {{video ? "query-unrotated" : "query", queryName}, {"context-key", keyName},
                    {"context-value", valueName}, {"context-valid", maskName},
                    {"attention", localResult}});
    addHeadLayoutTransform(*graph, localResult, "attention-" + rankName, local, false);
    appendNodesFrom(cuts.attend, *graph, attendStart);
  }
  // Projection indices still include the standalone model's omitted Split.
  const auto outputOffset = static_cast<std::uint64_t>(graph->node_size() - 1);
  appendFragment(*graph, std::move(*projection.mutable_graph()), "output/", outputBindings);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    auto& entry = layout.ranks[rank].attend;
    const auto localOutput = rebaseCut(std::move(projectionLayout.ranks[rank]), outputOffset,
                                       "output/", outputBindings);
    entry.nodeIndices.insert(entry.nodeIndices.end(),
                             localOutput.nodeIndices.begin(), localOutput.nodeIndices.end());
    entry.outputs = localOutput.outputs;
  }
  layout.finish = rebaseCut(std::move(projectionLayout.finish), outputOffset,
                              "output/", outputBindings);
  return layout;
}

} // namespace

AttentionContextLayout
lowerVideoAttentionContext(const onnx::ModelProto& key, const onnx::ModelProto& value,
                           const VideoRotaryTile& context, std::uint64_t rotaryBytes,
                           const WanInitializerAccess& material)
{
  if (context.firstToken != 0 || context.frames <= 0 || context.height <= 0 ||
      context.width <= 0 || context.frames > context.tokens / context.height / context.width) {
    throw std::invalid_argument("invalid Wan attention context grid");
  }
  return lowerAttentionContext(key, value, {context.heads, context.headWidth, context.tokens},
                                &context, rotaryBytes, material);
}

AttentionQueryLayout
lowerVideoAttentionQuery(const onnx::ModelProto& query, const onnx::ModelProto& output,
                         const VideoRotaryTile& tile, std::int64_t contextTokens,
                         std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                         std::uint64_t rotaryBytes, const WanInitializerAccess& material)
{
  return lowerAttentionQuery(query, output, {tile.heads, tile.headWidth, tile.tokens},
                              &tile, contextTokens, keyTileTokens, scoreBytes, rotaryBytes, material);
}

AttentionContextLayout
lowerTextAttentionContext(const onnx::ModelProto& key, const onnx::ModelProto& value,
                          std::int64_t heads, std::int64_t headWidth,
                          const WanInitializerAccess& material)
{
  return lowerAttentionContext(key, value, {heads, headWidth, tokenCount(key)}, nullptr, 0, material);
}

AttentionQueryLayout
lowerTextAttentionQuery(const onnx::ModelProto& query, const onnx::ModelProto& output,
                        std::int64_t heads, std::int64_t headWidth, std::int64_t contextTokens,
                        std::int64_t keyTileTokens, std::uint64_t scoreBytes,
                        const WanInitializerAccess& material)
{
  return lowerAttentionQuery(query, output, {heads, headWidth, tokenCount(query)},
                              nullptr, contextTokens, keyTileTokens, scoreBytes, 0, material);
}

} // namespace ndnsf::di::wan
