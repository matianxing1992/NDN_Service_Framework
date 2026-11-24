#include "wan-vae.hpp"

#include "wan-attention-tile.hpp"
#include "wan-checkpoint.hpp"
#include "wan-entry-plan.hpp"
#include "wan-graph-fragment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace ndnsf::di::wan {
namespace {

using TensorPort = NativeAssemblyTensorContractV3;

void
declareFloat(onnx::ValueInfoProto* value, const TensorPort& port)
{
  value->set_name(port.name);
  auto* tensor = value->mutable_type()->mutable_tensor_type();
  tensor->set_elem_type(onnx::TensorProto::FLOAT);
  for (const auto& extent : port.shape)
    tensor->mutable_shape()->add_dim()->set_dim_value(std::get<std::int64_t>(extent));
}

std::vector<std::uint64_t>
indicesFrom(const onnx::GraphProto& graph, int first)
{
  std::vector<std::uint64_t> result;
  for (int i = first; i < graph.node_size(); ++i) result.push_back(i);
  return result;
}

} // namespace

SummedProjection
lowerVaeCausalConv(const VaeConvTile& tile, const onnx::TensorProto& weight,
                   const onnx::TensorProto& bias, const WanInitializerAccess& material)
{
  if (weight.data_type() != onnx::TensorProto::FLOAT || weight.dims_size() != 5 ||
      weight.dims(0) <= 0 || weight.dims(1) < 2 ||
      bias.data_type() != onnx::TensorProto::FLOAT || bias.dims_size() != 1 ||
      bias.dims(0) != weight.dims(0) ||
      tile.frames <= 0 || tile.height <= 0 || tile.width <= 0) {
    throw std::invalid_argument("invalid Wan VAE convolution ports or parameters");
  }
  for (int axis = 2; axis < 5; ++axis) {
    if (weight.dims(axis) != 1 && weight.dims(axis) != 3)
      throw std::invalid_argument("unsupported Wan VAE convolution kernel");
  }
  const auto historyFrames = weight.dims(2) - 1;
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  const auto channels = std::max(weight.dims(0), weight.dims(1));
  if (tile.frames > LIMIT - historyFrames ||
      tile.frames + historyFrames > LIMIT / channels / tile.height / tile.width /
                                    static_cast<std::int64_t>(sizeof(float))) {
    throw std::invalid_argument("Wan VAE convolution extent overflows");
  }

  SummedProjection layout;
  auto& model = layout.model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-vae-causal-convolution");
  const auto port = [&] (std::string name, std::int64_t width, std::int64_t frames) {
    return NativeAssemblyTensorContractV3{
      std::move(name), "float32", {std::int64_t{1}, width, frames, tile.height, tile.width}};
  };
  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> inputs) {
    auto* node = graph.add_node();
    node->set_op_type(op);
    node->set_name(name);
    node->add_output(name);
    for (const auto& input : inputs) node->add_input(input);
    return node;
  };
  const auto index = [&] (const std::string& name, std::int64_t value) {
    auto* initializer = graph.add_initializer();
    initializer->set_name(name);
    initializer->set_data_type(onnx::TensorProto::INT64);
    initializer->add_dims(1);
    initializer->add_int64_data(value);
  };
  const auto input = port("frames", weight.dims(1), tile.frames);
  const auto output = port("decoded", weight.dims(0), tile.frames);
  declareFloat(graph.add_input(), input);
  declareFloat(graph.add_output(), output);
  index("channel-axis", 1);
  if (historyFrames != 0) {
    index("time-axis", 2);
    index("history-begin", tile.frames);
    index("history-end", tile.frames + historyFrames);
  }
  const auto ranges = nativeConvInputChannelRanges(weight.dims(1), 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto& [begin, end] = ranges[rank];
    const auto rankWidth = static_cast<std::int64_t>(end - begin);
    index(p + "begin", begin);
    index(p + "end", end);
    auto* kernel = graph.add_initializer();
    *kernel = material.slice(weight, 1, begin, end);
    kernel->set_name(p + "weight");
    const auto startNode = graph.node_size();
    auto& entry = layout.partials[rank];
    entry.inputs = {input};
    auto partial = port(p + "partial", weight.dims(0), tile.frames);
    entry.outputs = {partial};
    emit("Slice", p + "current", {input.name, p + "begin", p + "end", "channel-axis"});
    auto frames = p + "current";
    if (historyFrames != 0) {
      const auto history = port(p + "history", rankWidth, historyFrames);
      const auto next = port(p + "next-history", rankWidth, historyFrames);
      declareFloat(graph.add_input(), history);
      declareFloat(graph.add_output(), next);
      entry.inputs.push_back(history);
      entry.outputs.push_back(next);
      frames = p + "with-history";
      auto* attr = emit("Concat", frames, {history.name, p + "current"})->add_attribute();
      attr->set_name("axis");
      attr->set_type(onnx::AttributeProto::INT);
      attr->set_i(2);
      emit("Slice", next.name, {frames, "history-begin", "history-end", "time-axis"});
    }
    auto* pads = emit("Conv", partial.name, {frames, kernel->name()})->add_attribute();
    pads->set_name("pads");
    pads->set_type(onnx::AttributeProto::INTS);
    const auto h = (weight.dims(3) - 1) / 2;
    const auto w = (weight.dims(4) - 1) / 2;
    for (const auto value : {std::int64_t{0}, h, w, std::int64_t{0}, h, w}) pads->add_ints(value);
    entry.nodeIndices = indicesFrom(graph, startNode);
  }

  auto sum = output;
  sum.name = "unbiased";
  emit("Add", sum.name,
       {layout.partials[0].outputs.front().name, layout.partials[1].outputs.front().name});
  auto* offset = graph.add_initializer();
  *offset = material.slice(bias, 0, 0, bias.dims(0));
  offset->set_name("bias");
  offset->clear_dims();
  for (const auto extent : {std::int64_t{1}, bias.dims(0), std::int64_t{1},
                            std::int64_t{1}, std::int64_t{1}}) offset->add_dims(extent);
  layout.addBias = {{static_cast<std::uint64_t>(graph.node_size())}, {sum}, {output}};
  emit("Add", output.name, {sum.name, offset->name()});
  return layout;
}

onnx::ModelProto
makeVaeUpsample(const VaeConvTile& tile, std::int64_t channels, bool interleaveTime)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (tile.frames <= 0 || tile.height <= 0 || tile.width <= 0 || channels <= 0 ||
      tile.frames > LIMIT / 2 || tile.height > LIMIT / 2 || tile.width > LIMIT / 2 ||
      channels > LIMIT / 2) {
    throw std::invalid_argument("invalid Wan VAE upsampling extent");
  }
  const auto outputFrames = interleaveTime ? 2 * tile.frames : tile.frames;
  constexpr auto FLOAT_BYTES = static_cast<std::int64_t>(sizeof(float));
  if (outputFrames > LIMIT / channels / (2 * tile.height) / (2 * tile.width) / FLOAT_BYTES)
    throw std::invalid_argument("Wan VAE upsampling extent overflows");

  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-vae-upsample");
  const auto port = [] (onnx::ValueInfoProto* value, const char* name,
                         std::initializer_list<std::int64_t> shape) {
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dim : shape) tensor->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  port(graph.add_input(), "frames",
       {1, interleaveTime ? 2 * channels : channels, tile.frames, tile.height, tile.width});
  port(graph.add_output(), "upsampled", {1, channels, outputFrames,
                                        2 * tile.height, 2 * tile.width});
  const auto emit = [&] (const char* op, const char* output,
                         std::initializer_list<const char*> inputs) {
    auto* node = graph.add_node();
    node->set_op_type(op);
    node->set_name(output);
    node->add_output(output);
    for (const auto* input : inputs) node->add_input(input);
    return node;
  };
  const char* input = "frames";
  if (interleaveTime) {
    const auto dims = [&] (const char* name, std::initializer_list<std::int64_t> shape) {
      auto* tensor = graph.add_initializer();
      tensor->set_name(name);
      tensor->set_data_type(onnx::TensorProto::INT64);
      tensor->add_dims(shape.size());
      for (const auto dim : shape) tensor->add_int64_data(dim);
    };
    dims("groups-shape", {1, 2, channels, tile.frames, tile.height, tile.width});
    dims("time-shape", {1, channels, outputFrames, tile.height, tile.width});
    emit("Reshape", "groups", {"frames", "groups-shape"});
    auto* permutation = emit("Transpose", "ordered", {"groups"})->add_attribute();
    permutation->set_name("perm");
    permutation->set_type(onnx::AttributeProto::INTS);
    for (const auto axis : {0, 2, 3, 1, 4, 5}) permutation->add_ints(axis);
    emit("Reshape", "interleaved", {"ordered", "time-shape"});
    input = "interleaved";
  }
  auto* scales = graph.add_initializer();
  scales->set_name("spatial-scales");
  scales->set_data_type(onnx::TensorProto::FLOAT);
  scales->add_dims(5);
  for (const auto scale : {1.0F, 1.0F, 1.0F, 2.0F, 2.0F}) scales->add_float_data(scale);
  auto* resize = emit("Resize", "upsampled", {input, "", "spatial-scales"});
  for (const auto& [name, value] :
       std::initializer_list<std::pair<const char*, const char*>>{
         {"mode", "nearest"}, {"coordinate_transformation_mode", "half_pixel"},
         {"nearest_mode", "round_prefer_floor"}}) {
    auto* attr = resize->add_attribute();
    attr->set_name(name);
    attr->set_type(onnx::AttributeProto::STRING);
    attr->set_s(value);
  }
  return model;
}

static VaeRmsLayout
lowerVaeRms(const VaeConvTile& tile, const onnx::TensorProto& gain, bool silu,
             const WanInitializerAccess& material)
{
  if (tile.frames <= 0 || tile.height <= 0 || tile.width <= 0 ||
      gain.data_type() != onnx::TensorProto::FLOAT || gain.dims_size() != 1 ||
      gain.dims(0) < 2 || tile.frames > std::numeric_limits<std::int64_t>::max() /
        gain.dims(0) / tile.height / tile.width / static_cast<std::int64_t>(sizeof(float))) {
    throw std::invalid_argument("invalid Wan VAE RMS extent or gain");
  }
  const auto width = gain.dims(0);
  const auto ranges = nativeConvInputChannelRanges(width, 2);
  const TensorPort input{"frames", "float32",
    {std::int64_t{1}, width, tile.frames, tile.height, tile.width}};
  auto output = input;
  output.name = silu ? "activated" : "normalized";
  auto squares = input;
  squares.name = "square-sum";
  squares.shape[1] = std::int64_t{1};

  VaeRmsLayout layout;
  layout.model.set_ir_version(9);
  layout.model.add_opset_import()->set_version(18);
  auto& graph = *layout.model.mutable_graph();
  graph.set_name("wan-vae-rms-silu");
  declareFloat(graph.add_input(), input);
  declareFloat(graph.add_output(), output);
  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> inputs) {
    auto* node = graph.add_node();
    node->set_name(name);
    node->set_op_type(op);
    node->add_output(name);
    for (const auto& value : inputs) node->add_input(value);
    return node;
  };
  const auto index = [&] (const std::string& name, std::int64_t value) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(1);
    tensor->add_int64_data(value);
  };
  const auto scalar = [&] (const char* name, float value) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    tensor->add_float_data(value);
  };
  index("channel-axis", 1);
  scalar("l2-floor", 1e-12F);
  scalar("normalization-scale", std::sqrt(static_cast<float>(width)));
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto [begin, end] = ranges[rank];
    index(p + "begin", begin);
    index(p + "end", end);
    auto local = input;
    local.name = p + "local";
    local.shape[1] = static_cast<std::int64_t>(end - begin);
    auto partial = squares;
    partial.name = p + "squares";
    const auto first = graph.node_size();
    emit("Slice", local.name, {input.name, p + "begin", p + "end", "channel-axis"});
    emit("Mul", p + "square", {local.name, local.name});
    emit("ReduceSum", partial.name, {p + "square", "channel-axis"});
    layout.statistics[rank] = {indicesFrom(graph, first), {input}, {local, partial}};
  }
  emit("Add", squares.name,
       {layout.statistics[0].outputs[1].name, layout.statistics[1].outputs[1].name});
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto [begin, end] = ranges[rank];
    const auto& local = layout.statistics[rank].outputs.front();
    auto* parameter = graph.add_initializer();
    *parameter = material.slice(gain, 0, begin, end);
    parameter->set_name(p + "gain");
    parameter->clear_dims();
    for (const auto dim : {std::int64_t{1}, static_cast<std::int64_t>(end - begin),
                           std::int64_t{1}, std::int64_t{1}, std::int64_t{1}})
      parameter->add_dims(dim);
    const auto first = graph.node_size();
    emit("Sqrt", p + "l2", {squares.name});
    emit("Max", p + "stable", {p + "l2", "l2-floor"});
    emit("Div", p + "unit", {local.name, p + "stable"});
    emit("Mul", p + "scaled", {p + "unit", "normalization-scale"});
    auto activated = local;
    activated.name = p + output.name;
    const auto weighted = silu ? p + "weighted" : activated.name;
    emit("Mul", weighted, {p + "scaled", parameter->name()});
    if (silu) {
      emit("Sigmoid", p + "sigmoid", {weighted});
      emit("Mul", activated.name, {weighted, p + "sigmoid"});
    }
    layout.normalize[rank] = {indicesFrom(graph, first), {local, squares}, {activated}};
  }
  const auto first = graph.node_size();
  const auto& left = layout.normalize[0].outputs.front();
  const auto& right = layout.normalize[1].outputs.front();
  auto* axis = emit("Concat", output.name, {left.name, right.name})->add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(1);
  layout.join = {indicesFrom(graph, first), {left, right}, {output}};
  return layout;
}

VaeRmsLayout
lowerVaeRmsSilu(const VaeConvTile& tile, const onnx::TensorProto& gain,
                const WanInitializerAccess& material)
{
  return lowerVaeRms(tile, gain, true, material);
}

namespace {

void
requireCompatible(const TensorPort& left, const TensorPort& right)
{
  if (left.dtype != right.dtype || left.shape != right.shape)
    throw std::invalid_argument("Wan VAE port contract disagrees: " + left.name);
}

TensorPort
readVideoPort(const onnx::ValueInfoProto& value)
{
  const auto& tensor = value.type().tensor_type();
  if (tensor.elem_type() != onnx::TensorProto::FLOAT || tensor.shape().dim_size() != 5)
    throw std::invalid_argument("Wan VAE stage requires an NCDHW FP32 port");
  TensorPort result{value.name(), "float32", {}};
  for (const auto& extent : tensor.shape().dim()) {
    if (extent.dim_value() <= 0)
      throw std::invalid_argument("Wan VAE stage requires fixed positive extents");
    result.shape.emplace_back(extent.dim_value());
  }
  return result;
}

class StageComposer
{
public:
  StageComposer(VaeStageLayout& layout, const std::string& name)
    : m_layout(layout)
    , m_graph(*layout.model.mutable_graph())
  {
    layout.model.set_ir_version(9);
    layout.model.add_opset_import()->set_version(18);
    m_graph.set_name(name);
  }

  onnx::GraphProto&
  graph()
  {
    return m_graph;
  }

  TensorPort
  addStage(VaeStageLayout stage, const std::string& prefix, const TensorPort& source)
  {
    auto& fragment = *stage.model.mutable_graph();
    std::optional<TensorPort> output;
    std::map<std::string, std::string> bindings;
    std::set<std::string> successors;
    for (const auto& value : fragment.input()) {
      auto port = readVideoPort(value);
      const auto history = stage.historySuccessorByInput.find(port.name);
      if (history == stage.historySuccessorByInput.end()) {
        requireCompatible(port, source);
        bindings.emplace(port.name, source.name);
        continue;
      }
      const auto next = std::find_if(fragment.output().begin(), fragment.output().end(),
        [&] (const auto& candidate) { return candidate.name() == history->second; });
      if (next == fragment.output().end() || !successors.insert(history->second).second)
        throw std::invalid_argument("Wan VAE stage requires distinct history successors");
      auto nextPort = readVideoPort(*next);
      requireCompatible(port, nextPort);
      port.name = prefix + port.name;
      nextPort.name = prefix + nextPort.name;
      declareFloat(m_graph.add_input(), port);
      declareFloat(m_graph.add_output(), nextPort);
      m_layout.historySuccessorByInput.emplace(port.name, nextPort.name);
    }
    if (bindings.size() != 1 || successors.size() != stage.historySuccessorByInput.size())
      throw std::invalid_argument("Wan VAE stage must have one main input");
    for (const auto& value : fragment.output()) {
      auto port = readVideoPort(value);
      if (successors.count(port.name) != 0) continue;
      if (output) throw std::invalid_argument("Wan VAE stage must have one main output");
      port.name = prefix + port.name;
      output = std::move(port);
    }
    if (!output || fragment.output_size() != fragment.input_size())
      throw std::invalid_argument("Wan VAE stage must have one main output");
    const auto offset = static_cast<std::uint64_t>(m_graph.node_size());
    for (auto& [name, entry] : stage.entries)
      addCut(prefix + name, std::move(entry), offset, prefix, bindings);
    for (const auto& name : stage.rankOneEntries)
      m_layout.rankOneEntries.insert(prefix + name);
    for (const auto& name : stage.parameterNames)
      m_layout.parameterNames.insert(prefix + name);
    for (auto& [node, reduction] : stage.reductions) {
      m_layout.reductions.emplace(node + offset,
        OnnxReductionCut{rebaseCut(std::move(reduction.cut), offset, prefix, bindings),
                         reduction.bothRanks, reduction.firstRank});
    }
    appendFragment(m_graph, std::move(fragment), prefix, bindings);
    return *output;
  }

  TensorPort
  addNormalizedConv(VaeRmsLayout norm, SummedProjection conv, const std::string& normPrefix,
                    const std::string& convPrefix, const TensorPort& source, bool stateful)
  {
    const std::map<std::string, std::string> bindings{{"frames", source.name}};
    const auto shards = addNormCuts(norm, normPrefix, source, bindings);
    auto& normGraph = *norm.model.mutable_graph();
    if (norm.join.nodeIndices !=
        std::vector<std::uint64_t>{static_cast<std::uint64_t>(normGraph.node_size() - 1)})
      throw std::invalid_argument("Wan VAE normalization join must be the last node");
    normGraph.mutable_node()->RemoveLast();
    appendFragment(m_graph, std::move(normGraph), normPrefix, bindings);

    auto& convGraph = *conv.model.mutable_graph();
    const auto ranges = nativeConvInputChannelRanges(std::get<std::int64_t>(source.shape.at(1)), 2);
    std::array<std::uint64_t, 2> removed{};
    std::set<std::string> sliceConstants;
    for (std::size_t rank = 0; rank < shards.size(); ++rank) {
      auto& partial = conv.partials[rank];
      auto& current = partial.inputs.at(0);
      requireCompatible(current, source);
      removed[rank] = partial.nodeIndices.at(0);
      const auto& slice = convGraph.node(static_cast<int>(removed[rank]));
      if (slice.op_type() != "Slice" || slice.input_size() != 4 || slice.output_size() != 1 ||
          slice.input(0) != current.name)
        throw std::invalid_argument("Wan VAE convolution requires a channel slice");
      current.name = slice.output(0);
      current.shape[1] = static_cast<std::int64_t>(ranges[rank].second - ranges[rank].first);
      requireCompatible(current, shards[rank]);
      sliceConstants.insert(slice.input().begin() + 1, slice.input().end());
      partial.nodeIndices.erase(partial.nodeIndices.begin());
    }
    if (removed[0] >= removed[1])
      throw std::invalid_argument("Wan VAE convolution channel slices are out of rank order");
    for (auto it = removed.rbegin(); it != removed.rend(); ++it)
      convGraph.mutable_node()->DeleteSubrange(static_cast<int>(*it), 1);
    for (int i = convGraph.initializer_size(); i-- > 0;) {
      if (sliceConstants.count(convGraph.initializer(i).name()) != 0)
        convGraph.mutable_initializer()->DeleteSubrange(i, 1);
    }
    for (auto* cut : {&conv.partials[0], &conv.partials[1], &conv.addBias}) {
      for (auto& node : cut->nodeIndices)
        node -= static_cast<unsigned>(node > removed[0]) +
                static_cast<unsigned>(node > removed[1]);
    }
    return addConv(std::move(conv), convPrefix, shards, stateful);
  }

  TensorPort
  addConv(SummedProjection conv, const std::string& prefix, const TensorPort& source, bool stateful)
  {
    auto shard = conv.partials[1].inputs.at(0);
    requireCompatible(shard, source);
    const auto ranges = nativeConvInputChannelRanges(std::get<std::int64_t>(shard.shape.at(1)), 2);
    shard.shape[1] = static_cast<std::int64_t>(ranges[1].second - ranges[1].first);
    const auto peer = movePeerSlice(*conv.model.mutable_graph(), conv.partials[1],
                                    std::move(shard), prefix, {{"frames", source.name}});
    return addConv(std::move(conv), prefix, std::array<TensorPort, 2>{source, peer}, stateful);
  }

private:
  TensorPort
  movePeerSlice(onnx::GraphProto& graph, OnnxEntryCut& peer, TensorPort shard,
                const std::string& prefix, const std::map<std::string, std::string>& bindings)
  {
    auto& slice = *graph.mutable_node(static_cast<int>(peer.nodeIndices.at(0)));
    if (slice.op_type() != "Slice" || slice.input_size() != 4 || slice.output_size() != 1 ||
        slice.input(0) != peer.inputs.at(0).name)
      throw std::invalid_argument("Wan VAE peer input requires a channel slice");
    shard.name = slice.output(0) + "-input";
    onnx::GraphProto producer;
    auto& sendSlice = *producer.add_node();
    sendSlice = slice;
    sendSlice.set_name(shard.name);
    sendSlice.set_output(0, shard.name);
    auto& initializers = *graph.mutable_initializer();
    for (int port : {1, 2}) {
      const auto constant = std::find_if(initializers.begin(), initializers.end(),
        [&](const auto& value) { return value.name() == sendSlice.input(port); });
      if (constant == initializers.end())
        throw std::invalid_argument("Wan VAE peer slice has no channel bounds");
      constant->set_name(port == 1 ? "peer-slice/begin" : "peer-slice/end");
      sendSlice.set_input(port, constant->name());
    }
    const auto& sender = addCut(prefix + "rank-1/input-slice",
      {{0}, {peer.inputs.at(0)}, {shard}}, m_graph.node_size(), prefix, bindings);
    appendFragment(m_graph, std::move(producer), prefix, bindings);
    slice.set_op_type("Identity");
    slice.clear_input();
    slice.add_input(shard.name);
    peer.inputs.at(0) = shard;
    return sender.outputs.at(0);
  }

  std::array<TensorPort, 2>
  addNormCuts(VaeRmsLayout& norm, const std::string& prefix, const TensorPort& source,
              const std::map<std::string, std::string>& bindings)
  {
    for (const auto& stats : norm.statistics)
      requireCompatible(stats.inputs.at(0), source);

    // Send only the peer's channels. The peer keeps the local activation
    // between the statistics and normalization calls.
    auto& peer = norm.statistics[1];
    movePeerSlice(*norm.model.mutable_graph(), peer, peer.outputs.at(0), prefix, bindings);
    const auto offset = static_cast<std::uint64_t>(m_graph.node_size());
    const auto sumIndex = norm.statistics[1].nodeIndices.back() + 1;
    addReduction(sumIndex, {norm.statistics[0].outputs.at(1), norm.statistics[1].outputs.at(1)},
                 norm.normalize[0].inputs.at(1), true, offset, prefix, bindings);
    std::array<TensorPort, 2> shards;
    for (std::size_t rank = 0; rank < 2; ++rank) {
      const auto id = prefix + "rank-" + std::to_string(rank);
      m_layout.parameterNames.insert(id + "/gain");
      addCut(id + "/statistics", std::move(norm.statistics[rank]), offset, prefix, bindings);
      const auto& normalized =
        addCut(id + "/normalize", std::move(norm.normalize[rank]), offset, prefix, bindings);
      shards[rank] = normalized.outputs.at(0);
      if (rank == 1) {
        m_layout.rankOneEntries.insert(id + "/statistics");
        m_layout.rankOneEntries.insert(id + "/normalize");
      }
    }
    return shards;
  }

  TensorPort
  addConv(SummedProjection conv, const std::string& prefix,
          const std::array<TensorPort, 2>& sources, bool stateful)
  {
    const auto offset = static_cast<std::uint64_t>(m_graph.node_size());
    std::map<std::string, std::string> bindings;
    for (std::size_t rank = 0; rank < sources.size(); ++rank)
      bindings.emplace(conv.partials[rank].inputs.at(0).name, sources[rank].name);
    const auto sumIndex = conv.partials[1].nodeIndices.back() + 1;
    addReduction(sumIndex, {conv.partials[0].outputs.at(0), conv.partials[1].outputs.at(0)},
                 conv.addBias.inputs.at(0), false, offset, prefix, bindings);
    for (std::size_t rank = 0; rank < 2; ++rank) {
      const auto id = prefix + "rank-" + std::to_string(rank);
      m_layout.parameterNames.insert(id + "/weight");
      requireCompatible(conv.partials[rank].inputs.at(0), sources[rank]);
      const auto count = stateful ? 2U : 1U;
      if (conv.partials[rank].inputs.size() != count ||
          conv.partials[rank].outputs.size() != count)
        throw std::invalid_argument("Wan VAE convolution history contract disagrees");
      const auto& entry = addCut(id,
                                 std::move(conv.partials[rank]), offset, prefix, bindings);
      if (rank == 1) m_layout.rankOneEntries.insert(id);
      if (stateful) {
        const auto& history = entry.inputs.at(1);
        const auto& next = entry.outputs.at(1);
        requireCompatible(history, next);
        declareFloat(m_graph.add_input(), history);
        declareFloat(m_graph.add_output(), next);
        m_layout.historySuccessorByInput.emplace(history.name, next.name);
      }
    }
    const auto& bias = addCut(prefix + "bias", std::move(conv.addBias), offset, prefix, bindings);
    m_layout.parameterNames.insert(prefix + "bias");
    appendFragment(m_graph, std::move(*conv.model.mutable_graph()), prefix, bindings);
    return bias.outputs.at(0);
  }

public:
  void
  finishResidual(const TensorPort& main, const TensorPort& residual)
  {
    requireCompatible(residual, main);
    auto output = main;
    output.name = "decoded";
    const auto first = m_graph.node_size();
    auto* join = m_graph.add_node();
    join->set_name("residual");
    join->set_op_type("Add");
    join->add_input(main.name);
    join->add_input(residual.name);
    join->add_output(output.name);
    declareFloat(m_graph.add_output(), output);
    m_layout.entries.emplace("residual", OnnxEntryCut{
      indicesFrom(m_graph, first), {main, residual}, {output}});
  }

private:
  void
  addReduction(std::uint64_t node, std::vector<TensorPort> inputs, TensorPort output,
               bool onBothRanks, std::uint64_t offset, const std::string& prefix,
               const std::map<std::string, std::string>& bindings)
  {
    m_layout.reductions.emplace(node + offset,
      OnnxReductionCut{rebaseCut({{}, std::move(inputs), {std::move(output)}}, offset,
                                 prefix, bindings), onBothRanks});
  }

  const OnnxEntryCut&
  addCut(const std::string& id, OnnxEntryCut entry, std::uint64_t offset,
         const std::string& prefix, const std::map<std::string, std::string>& bindings)
  {
    return m_layout.entries.emplace(id, rebaseCut(std::move(entry), offset, prefix, bindings))
      .first->second;
  }

  VaeStageLayout& m_layout;
  onnx::GraphProto& m_graph;
};

} // namespace

static std::uint64_t
vaeTensorBytes(const TensorPort& port)
{
  std::uint64_t bytes = sizeof(float);
  for (const auto& dim : port.shape) {
    const auto extent = std::get<std::int64_t>(dim);
    if (extent <= 0 || static_cast<std::uint64_t>(extent) >
        std::numeric_limits<std::uint64_t>::max() / bytes)
      throw std::invalid_argument("Wan VAE transfer shape exceeds byte bounds");
    bytes *= extent;
  }
  return bytes;
}

static void
countVaeTransferBytes(std::uint64_t& total, std::uint64_t bytes)
{
  if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
    throw std::invalid_argument("Wan VAE plan total exceeds byte bounds");
  total += bytes;
}

OnnxParallelPlan
makeVaeTensorParallelPlan(const VaeStageLayout& layout, const NativeStageLayout& stage)
{
  auto original = compileParallelEntries(layout.model.graph(), stage, layout.entries,
                                         layout.rankOneEntries, layout.reductions);
  // A single Conv1 SUM and the following Norm2 can move together without
  // changing weights, ports, summation order, or the residual's output owner.
  // Group the repeated latent chunks to keep each Conv2 history on its rank.
  struct ConvolutionGroup
  {
    std::uint64_t shiftBytes = 0;
    std::vector<std::pair<std::string, std::uint64_t>> merges;
  };
  std::map<std::string_view, std::uint64_t> sumByOutput;
  for (const auto& [node, reduction] : layout.reductions) {
    if (!reduction.bothRanks && reduction.firstRank == 0)
      sumByOutput.emplace(reduction.cut.outputs.at(0).name, node);
  }
  constexpr std::string_view BIAS = "conv1/bias";
  std::map<std::string, ConvolutionGroup> groups;
  for (const auto& [id, entry] : layout.entries) {
    const auto phaseEnd = id.find('/');
    if (id.compare(0, 6, "chunk-") != 0 || phaseEnd == std::string::npos ||
        id.size() < phaseEnd + 1 + BIAS.size() ||
        id.compare(id.size() - BIAS.size(), BIAS.size(), BIAS) != 0)
      continue;
    const auto prefix = id.substr(0, id.size() - BIAS.size());
    const auto shard = layout.entries.find(prefix + "norm2/rank-1/input-slice");
    if (shard == layout.entries.end() || shard->second.inputs.at(0).name != entry.outputs.at(0).name)
      continue;
    auto& group = groups[prefix.substr(phaseEnd + 1)];
    group.merges.emplace_back(prefix, sumByOutput.at(entry.inputs.at(0).name));
    const auto fullBytes = vaeTensorBytes(entry.outputs.at(0));
    const auto shardBytes = vaeTensorBytes(shard->second.outputs.at(0));
    if (shardBytes >= fullBytes)
      throw std::invalid_argument("Wan VAE Norm2 peer slice does not reduce channels");
    countVaeTransferBytes(group.shiftBytes, fullBytes - shardBytes);
  }
  if (groups.empty()) return original;

  std::map<std::string_view, const NativeExecutionStepSpec*> steps;
  for (const auto& step : original.plan.steps) steps.emplace(step.stepId, &step);
  std::array<std::uint64_t, 2> outbound{};
  for (const auto& edge : original.plan.dependencies) {
    const auto& step = *steps.at(edge.producerStepId);
    const auto output = std::find_if(step.outputSlots.begin(), step.outputSlots.end(),
      [&](const auto& slot) { return slot.slotId == edge.producerSlotId; });
    const auto rank = step.unitId == stage.unitRoleIds.at(1) ? 1U : 0U;
    countVaeTransferBytes(outbound[rank],
      vaeTensorBytes({output->slotId, output->dtype, output->shape}));
  }
  if (outbound[0] >= outbound[1]) return original;
  std::vector<const ConvolutionGroup*> ordered;
  for (const auto& item : groups) ordered.push_back(&item.second);
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
    return left->shiftBytes > right->shiftBytes;
  });

  auto rankOne = layout.rankOneEntries;
  auto reductions = layout.reductions;
  for (const auto* group : ordered) {
    if (outbound[0] >= outbound[1]) break;
    if (group->shiftBytes >= outbound[1] - outbound[0]) continue;
    for (const auto& [prefix, node] : group->merges) {
      reductions.at(node).firstRank = 1;
      for (const auto* id : {"conv1/bias", "norm2/rank-1/input-slice",
                             "norm2/rank-0/statistics", "norm2/rank-0/normalize", "conv2/rank-0"})
        rankOne.insert(prefix + id);
      for (const auto* id : {"norm2/rank-1/statistics", "norm2/rank-1/normalize", "conv2/rank-1"})
        rankOne.erase(prefix + id);
    }
    outbound[0] += group->shiftBytes;
    outbound[1] -= group->shiftBytes;
  }
  if (rankOne == layout.rankOneEntries) return original;
  return compileParallelEntries(layout.model.graph(), stage, layout.entries, rankOne, reductions);
}

VaeStageLayout
lowerVaeResample(const VaeConvTile& tile, const VaeConvolutionParameters& spatial,
                 std::optional<VaeConvolutionParameters> temporal, bool firstChunk,
                 const WanInitializerAccess& material)
{
  if (spatial.weight.dims_size() != 5 || spatial.weight.dims(1) < 2 ||
      (spatial.weight.dims(0) != spatial.weight.dims(1) / 2 &&
       spatial.weight.dims(0) != spatial.weight.dims(1)) ||
      spatial.weight.dims(2) != 1 || spatial.weight.dims(3) != 3 ||
      spatial.weight.dims(4) != 3) {
    throw std::invalid_argument("invalid Wan VAE spatial resample projection");
  }
  const auto channels = spatial.weight.dims(1);
  const bool interleave = temporal.has_value() && !firstChunk;
  auto resize = makeVaeUpsample(tile, channels, interleave);
  if (temporal &&
      ((firstChunk && tile.frames != 1) ||
       temporal->weight.dims_size() != 5 || temporal->weight.dims(0) != 2 * channels ||
       temporal->weight.dims(1) != channels || temporal->weight.dims(2) != 3 ||
       temporal->weight.dims(3) != 1 || temporal->weight.dims(4) != 1 ||
       temporal->weight.data_type() != onnx::TensorProto::FLOAT ||
       temporal->bias.dims_size() != 1 || temporal->bias.dims(0) != 2 * channels ||
       temporal->bias.data_type() != onnx::TensorProto::FLOAT)) {
    throw std::invalid_argument("invalid Wan VAE temporal resample projection");
  }

  VaeStageLayout result;
  StageComposer composer(result, "wan-vae-resample");
  auto& graph = composer.graph();
  const TensorPort input{"frames", "float32",
                         {std::int64_t{1}, channels, tile.frames, tile.height, tile.width}};
  declareFloat(graph.add_input(), input);
  const auto source = interleave ?
    composer.addConv(lowerVaeCausalConv(tile, temporal->weight, temporal->bias, material),
                     "temporal/", input, true) : input;
  const VaeConvTile resizedTile{
    interleave ? 2 * tile.frames : tile.frames, 2 * tile.height, 2 * tile.width};
  const TensorPort resized{"upsampled", "float32",
                           {std::int64_t{1}, channels, resizedTile.frames,
                            resizedTile.height, resizedTile.width}};
  const auto first = graph.node_size();
  appendFragment(graph, std::move(*resize.mutable_graph()), "resize/",
                 {{"frames", source.name}, {"upsampled", resized.name}});
  result.entries.emplace("resize", OnnxEntryCut{
    indicesFrom(graph, first), {source}, {resized}});
  const auto output = composer.addConv(
    lowerVaeCausalConv(resizedTile, spatial.weight, spatial.bias, material),
    "spatial/", resized, false);
  declareFloat(graph.add_output(), output);
  return result;
}

VaeStageLayout
connectVaeUpsampleResidual(VaeStageLayout main, const VaeConvTile& tile,
                          std::int64_t inputChannels, std::int64_t outputChannels,
                          bool temporal, bool firstChunk)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  const std::int64_t timeFactor = temporal ? 2 : 1;
  if (inputChannels <= 0 || outputChannels <= 0 || outputChannels > LIMIT / 8 ||
      tile.frames <= 0 || tile.height <= 0 || tile.width <= 0 ||
      tile.frames > LIMIT / timeFactor || tile.height > LIMIT / 2 || tile.width > LIMIT / 2 ||
      (firstChunk && temporal && tile.frames != 1) ||
      outputChannels * timeFactor * 4 % inputChannels != 0)
    throw std::invalid_argument("invalid Wan2.2 VAE upsample shortcut geometry");
  const auto repeats = outputChannels * timeFactor * 4 / inputChannels;
  // The Gather index is also materialized metadata, so bound it before allocation.
  if (outputChannels * timeFactor * 4 > (1 << 20))
    throw std::invalid_argument("Wan2.2 VAE shortcut channel index exceeds its bound");
  VaeStageLayout result;
  StageComposer composer(result, "wan22-vae-upsample-residual");
  auto& graph = composer.graph();
  const TensorPort input{"frames", "float32",
    {std::int64_t{1}, inputChannels, tile.frames, tile.height, tile.width}};
  declareFloat(graph.add_input(), input);
  const auto decoded = composer.addStage(std::move(main), "main/", input);
  auto shortcut = input;
  shortcut.name = "shortcut";
  shortcut.shape = {std::int64_t{1}, outputChannels,
    timeFactor * tile.frames - (firstChunk ? timeFactor - 1 : 0),
    2 * tile.height, 2 * tile.width};
  requireCompatible(shortcut, decoded);
  const auto first = graph.node_size();
  const auto ints = [&](const char* name, const std::vector<std::int64_t>& values) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(values.size());
    for (auto value : values) tensor->add_int64_data(value);
  };
  const auto emit = [&](const char* op, const char* name,
                        std::initializer_list<const char*> inputs) {
    auto* node = graph.add_node();
    node->set_name(name);
    node->set_op_type(op);
    node->add_output(name);
    for (const auto* source : inputs) node->add_input(source);
    return node;
  };
  std::vector<std::int64_t> channels;
  for (std::int64_t channel = 0; channel < inputChannels; ++channel)
    channels.insert(channels.end(), repeats, channel);
  ints("shortcut-channels", channels);
  auto* axis = emit("Gather", "shortcut-repeated", {"frames", "shortcut-channels"})
    ->add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(1);
  ints("shortcut-factors", {1, outputChannels, timeFactor, 2, 2,
                             tile.frames, tile.height, tile.width});
  ints("shortcut-shape", {1, outputChannels, timeFactor * tile.frames,
                           2 * tile.height, 2 * tile.width});
  emit("Reshape", "shortcut-factored", {"shortcut-repeated", "shortcut-factors"});
  auto* perm = emit("Transpose", "shortcut-ordered", {"shortcut-factored"})->add_attribute();
  perm->set_name("perm");
  perm->set_type(onnx::AttributeProto::INTS);
  for (auto index : {0, 1, 5, 2, 6, 3, 7, 4}) perm->add_ints(index);
  const bool crop = firstChunk && temporal;
  emit("Reshape", crop ? "shortcut-expanded" : "shortcut",
       {"shortcut-ordered", "shortcut-shape"});
  if (crop) {
    ints("shortcut-start", {timeFactor - 1});
    ints("shortcut-end", {timeFactor * tile.frames});
    ints("shortcut-axis", {2});
    emit("Slice", "shortcut", {"shortcut-expanded", "shortcut-start", "shortcut-end",
                                 "shortcut-axis"});
  }
  result.entries.emplace("shortcut", OnnxEntryCut{
    indicesFrom(graph, first), {input}, {shortcut}});
  composer.finishResidual(decoded, shortcut);
  return result;
}

VaeStageLayout
connectVaeResidual(VaeRmsLayout firstNorm, SummedProjection firstConv,
                   VaeRmsLayout secondNorm, SummedProjection secondConv,
                   std::optional<SummedProjection> shortcut)
{
  VaeStageLayout result;
  StageComposer composer(result, "wan-vae-residual");
  const auto input = firstNorm.statistics[0].inputs.at(0);
  const auto expected = secondConv.addBias.outputs.at(0);
  if ((input.shape != expected.shape) != shortcut.has_value())
    throw std::invalid_argument("Wan VAE residual shortcut disagrees with channel change");
  declareFloat(composer.graph().add_input(), input);
  const auto residual = shortcut ? composer.addConv(std::move(*shortcut),
                                                     "shortcut/", input, false) : input;
  const auto hidden = composer.addNormalizedConv(std::move(firstNorm), std::move(firstConv),
                                                 "norm1/", "conv1/", input, true);
  const auto main = composer.addNormalizedConv(std::move(secondNorm), std::move(secondConv),
                                               "norm2/", "conv2/", hidden, true);
  composer.finishResidual(main, residual);
  return result;
}

VaeStageLayout
lowerVaeAttention(const VaeConvTile& tile, const VaeAttentionParameters& parameters,
                  std::int64_t keyTile, std::uint64_t scoreBytes,
                  const WanInitializerAccess& material)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (tile.frames <= 0 || tile.height <= 0 || tile.width <= 0 || keyTile <= 0 ||
      tile.height > LIMIT / tile.width || parameters.gain.dims_size() != 1 ||
      parameters.gain.dims(0) < 2 || parameters.gain.dims(0) > LIMIT / 3) {
    throw std::invalid_argument("invalid Wan VAE attention geometry");
  }
  const auto channels = parameters.gain.dims(0);
  const auto pixels = tile.height * tile.width;
  const auto remainder = pixels % keyTile;
  const auto padding = remainder ? keyTile - remainder : 0;
  if (pixels > LIMIT - padding)
    throw std::invalid_argument("Wan VAE spatial context extent overflows");
  const auto context = pixels + padding;
  const auto requirePointwise = [&] (const onnx::TensorProto& weight, std::int64_t out) {
    if (weight.dims_size() != 5 || weight.dims(0) != out || weight.dims(1) != channels ||
        weight.dims(2) != 1 || weight.dims(3) != 1 || weight.dims(4) != 1) {
      throw std::invalid_argument("Wan VAE attention needs pointwise projection weights");
    }
  };
  requirePointwise(parameters.qkvWeight, 3 * channels);
  requirePointwise(parameters.outputWeight, channels);
  auto attention = makeAttentionQueryBlock(
    {tile.frames, 1, pixels, keyTile, channels, true}, context, scoreBytes);

  auto norm = lowerVaeRms(tile, parameters.gain, false, material);
  const auto input = norm.statistics[0].inputs.at(0);
  VaeStageLayout result;
  StageComposer composer(result, "wan-vae-spatial-attention");
  auto& graph = composer.graph();
  declareFloat(graph.add_input(), input);
  const auto projected = composer.addNormalizedConv(std::move(norm),
    lowerVaeCausalConv(tile, parameters.qkvWeight, parameters.qkvBias, material),
    "norm/", "qkv/", input, false);
  const auto first = graph.node_size();
  const auto emit = [&] (const char* op, const char* name,
                         std::initializer_list<const char*> inputs) {
    auto* node = graph.add_node();
    node->set_name(name);
    node->set_op_type(op);
    node->add_output(name);
    for (const auto* value : inputs) node->add_input(value);
    return node;
  };
  const auto ints = [&] (const char* name, std::initializer_list<std::int64_t> values,
                         bool scalar = false) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    if (!scalar) tensor->add_dims(values.size());
    for (const auto value : values) tensor->add_int64_data(value);
  };
  const auto transpose = [&] (const char* name, const char* source,
                              std::initializer_list<std::int64_t> order) {
    auto* perm = emit("Transpose", name, {source})->add_attribute();
    perm->set_name("perm");
    perm->set_type(onnx::AttributeProto::INTS);
    for (const auto dim : order) perm->add_ints(dim);
  };
  ints("qkv-shape", {tile.frames, 1, pixels, 3 * channels});
  ints("split-heads", {channels, channels, channels});
  transpose("spatial-qkv", projected.name.c_str(), {0, 2, 3, 4, 1});
  emit("Reshape", "flat-qkv", {"spatial-qkv", "qkv-shape"});
  auto* split = emit("Split", "query", {"flat-qkv", "split-heads"});
  split->add_output("key");
  split->add_output("value");
  auto* axis = split->add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(-1);
  if (padding != 0) {
    ints("context-padding", {0, 0, 0, 0, 0, 0, padding, 0});
    emit("Pad", "padded-key", {"key", "context-padding"});
    emit("Pad", "padded-value", {"value", "context-padding"});
  }
  ints("first-pixel", {0}, true);
  ints("one-pixel", {1}, true);
  ints("valid-pixels", {pixels}, true);
  ints("context-pixels", {context}, true);
  ints("mask-row-shape", {1, 1, 1, context});
  ints("mask-shape", {tile.frames, 1, 1, context});
  emit("Range", "key-positions", {"first-pixel", "context-pixels", "one-pixel"});
  emit("Less", "valid-keys", {"key-positions", "valid-pixels"});
  emit("Reshape", "mask-row", {"valid-keys", "mask-row-shape"});
  emit("Expand", "key-mask", {"mask-row", "mask-shape"});
  appendFragment(graph, std::move(*attention.mutable_graph()), "softmax/",
    {{"query", "query"}, {"context-key", padding != 0 ? "padded-key" : "key"},
     {"context-value", padding != 0 ? "padded-value" : "value"},
     {"context-valid", "key-mask"}, {"attention", "attended"}});
  ints("restore-shape", {1, tile.frames, tile.height, tile.width, channels});
  emit("Reshape", "frame-pixels", {"attended", "restore-shape"});
  transpose("spatial-result", "frame-pixels", {0, 4, 1, 2, 3});
  auto weighted = input;
  weighted.name = "spatial-result";
  result.entries.emplace("attention", OnnxEntryCut{
    indicesFrom(graph, first), {projected}, {weighted}});
  const auto main = composer.addConv(
    lowerVaeCausalConv(tile, parameters.outputWeight, parameters.outputBias, material),
    "projection/", weighted, false);
  composer.finishResidual(main, input);
  return result;
}

VaeStageLayout
connectVaeDecoder(const VaeConvTile& latentTile, const VaeDecoderParameters& parameters,
                  std::vector<std::pair<std::string, VaeStageLayout>> stages,
                  const WanInitializerAccess& material)
{
  const auto& p = parameters;
  const auto kernelMatches = [] (const onnx::TensorProto& weight, int t, int h, int w) {
    return weight.dims_size() == 5 && weight.dims(2) == t &&
           weight.dims(3) == h && weight.dims(4) == w;
  };
  if (latentTile.frames != 1 || stages.empty() ||
      !kernelMatches(p.latentProjection.weight, 1, 1, 1) ||
      p.latentProjection.weight.dims(0) != p.latentProjection.weight.dims(1) ||
      !kernelMatches(p.inputProjection.weight, 3, 3, 3) ||
      !kernelMatches(p.rgbProjection.weight, 3, 3, 3) ||
      (p.outputPatchSize != 1 && p.outputPatchSize != 2) ||
      p.rgbProjection.weight.dims(0) != 3 * p.outputPatchSize * p.outputPatchSize) {
    throw std::invalid_argument("invalid Wan VAE decoder projections");
  }
  auto latent = lowerVaeCausalConv(latentTile, p.latentProjection.weight,
                                  p.latentProjection.bias, material);
  const auto channels = p.latentProjection.weight.dims(1);
  if (p.latentMean.size() != static_cast<std::size_t>(channels) ||
      p.latentInverseStd.size() != p.latentMean.size() ||
      std::any_of(p.latentMean.begin(), p.latentMean.end(),
                  [] (auto value) { return !std::isfinite(value); }) ||
      std::any_of(p.latentInverseStd.begin(), p.latentInverseStd.end(),
                  [] (auto value) { return !std::isfinite(value) || value <= 0; })) {
    throw std::invalid_argument("invalid Wan VAE latent scale");
  }
  auto input = latent.partials[0].inputs.at(0);
  input.name = "latent";
  VaeStageLayout result;
  StageComposer composer(result, "wan-vae-decoder");
  auto& graph = composer.graph();
  declareFloat(graph.add_input(), input);
  const auto emit = [&] (const char* op, const std::string& name,
                         std::initializer_list<std::string> sources) {
    auto* node = graph.add_node();
    node->set_name(name);
    node->set_op_type(op);
    for (const auto& source : sources) node->add_input(source);
    node->add_output(name);
  };
  const auto constant = [&] (const char* name, const auto& values, bool perChannel) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    if (perChannel) {
      for (const auto dim : {std::int64_t{1}, channels, std::int64_t{1},
                              std::int64_t{1}, std::int64_t{1}}) tensor->add_dims(dim);
    }
    for (const auto value : values) tensor->add_float_data(value);
  };
  constant("latent-mean", p.latentMean, true);
  constant("latent-inverse-std", p.latentInverseStd, true);
  result.parameterNames.insert({"latent-mean", "latent-inverse-std"});
  auto rescaled = input;
  rescaled.name = "rescaled";
  emit("Div", "scaled", {"latent", "latent-inverse-std"});
  emit("Add", "rescaled", {"scaled", "latent-mean"});
  result.entries.emplace("latent-scale", OnnxEntryCut{{0, 1}, {input}, {rescaled}});
  const auto projected = composer.addConv(std::move(latent), "latent-projection/", rescaled, false);
  auto hidden = composer.addConv(
    lowerVaeCausalConv(latentTile, p.inputProjection.weight, p.inputProjection.bias, material),
    "input-projection/", projected, true);
  std::set<std::string> ids;
  for (auto& [id, stage] : stages) {
    if (id.empty() || id.find('/') != std::string::npos || !ids.insert(id).second)
      throw std::invalid_argument("invalid or duplicate Wan VAE stage ID");
    hidden = composer.addStage(std::move(stage), "decoder/" + id + "/", hidden);
    if (id == "decoder.middle.2")
      result.decoderMiddleEnd = static_cast<std::size_t>(graph.node_size());
  }
  const VaeConvTile rgbTile{std::get<std::int64_t>(hidden.shape.at(2)),
                           std::get<std::int64_t>(hidden.shape.at(3)),
                           std::get<std::int64_t>(hidden.shape.at(4))};
  auto pixels = composer.addNormalizedConv(lowerVaeRmsSilu(rgbTile, p.outputGain, material),
    lowerVaeCausalConv(rgbTile, p.rgbProjection.weight, p.rgbProjection.bias, material),
    "rgb-norm/", "rgb-projection/", hidden, true);
  const std::array<float, 1> minimum{-1}, maximum{1};
  constant("rgb-min", minimum, false);
  constant("rgb-max", maximum, false);
  const auto last = static_cast<std::uint64_t>(graph.node_size());
  const auto packedPixels = pixels;
  if (p.outputPatchSize == 2) {
    constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
    if (rgbTile.height > LIMIT / 2 || rgbTile.width > LIMIT / 2)
      throw std::invalid_argument("Wan VAE output patch extent overflows");
    const auto shape = [&](const char* name, std::initializer_list<std::int64_t> dims) {
      auto* value = graph.add_initializer();
      value->set_name(name);
      value->set_data_type(onnx::TensorProto::INT64);
      value->add_dims(dims.size());
      for (auto dim : dims) value->add_int64_data(dim);
    };
    shape("rgb-packed-shape", {1, 3, 2, 2, rgbTile.frames, rgbTile.height, rgbTile.width});
    shape("rgb-image-shape", {1, 3, rgbTile.frames, 2 * rgbTile.height, 2 * rgbTile.width});
    emit("Reshape", "rgb-packed", {pixels.name, "rgb-packed-shape"});
    emit("Transpose", "rgb-spatial-order", {"rgb-packed"});
    auto* perm = graph.mutable_node(graph.node_size() - 1)->add_attribute();
    perm->set_name("perm");
    perm->set_type(onnx::AttributeProto::INTS);
    // Source channel order is (c r q), with q expanding height and r width.
    for (auto axis : {0, 1, 4, 5, 3, 6, 2}) perm->add_ints(axis);
    emit("Reshape", "rgb-unpacked", {"rgb-spatial-order", "rgb-image-shape"});
    pixels.name = "rgb-unpacked";
    pixels.shape = {std::int64_t{1}, std::int64_t{3}, rgbTile.frames,
                    2 * rgbTile.height, 2 * rgbTile.width};
  }
  emit("Clip", "rgb", {pixels.name, "rgb-min", "rgb-max"});
  auto output = pixels;
  output.name = "rgb";
  declareFloat(graph.add_output(), output);
  result.entries.emplace("rgb-clip", OnnxEntryCut{indicesFrom(graph, last),
                                                  {packedPixels}, {output}});
  return result;
}

static VaeStageLayout
lowerCheckpointVaeDecoder(const VaeConvTile& latentTile,
                     const std::map<std::string, onnx::TensorProto>& parameters,
                     std::vector<float> latentMean, std::vector<float> latentInverseStd,
                     bool firstChunk, std::int64_t keyTile, std::uint64_t scoreBytes,
                     const WanInitializerAccess& material, bool wan22)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (latentTile.frames != 1 || latentTile.height <= 0 || latentTile.width <= 0 ||
      latentTile.height > LIMIT / 8 || latentTile.width > LIMIT / 8 ||
      latentMean.size() < 2 || latentMean.size() != latentInverseStd.size() ||
      latentMean.size() > static_cast<std::size_t>(LIMIT)) {
    throw std::invalid_argument("invalid Wan2.1 decoder latent geometry");
  }
  std::set<std::string> bound;
  const auto get = [&] (const std::string& name) -> const onnx::TensorProto& {
    const auto found = parameters.find(name);
    if (found == parameters.end() || found->second.data_type() != onnx::TensorProto::FLOAT)
      throw std::invalid_argument("missing or non-FP32 Wan2.1 decoder parameter: " + name);
    if (!bound.insert(name).second)
      throw std::invalid_argument("duplicate Wan2.1 decoder binding: " + name);
    return found->second;
  };
  const auto requireShape = [] (const onnx::TensorProto& value,
                               std::initializer_list<std::int64_t> shape,
                               const std::string& name) {
    if (!std::equal(value.dims().begin(), value.dims().end(), shape.begin(), shape.end()))
      throw std::invalid_argument("invalid Wan2.1 decoder parameter shape: " + name);
  };
  const auto conv = [&] (const std::string& name, std::int64_t out, std::int64_t in,
                         int time, int space) {
    const auto& weight = get(name + ".weight");
    const auto& bias = get(name + ".bias");
    if (time == 0) requireShape(weight, {out, in, space, space}, name);
    else requireShape(weight, {out, in, time, space, space}, name);
    requireShape(bias, {out}, name);
    return VaeConvolutionParameters{weight, bias};
  };
  const auto asSpatial = [] (const onnx::TensorProto& weight) {
    auto result = weight;
    result.clear_dims();
    for (const auto dim : {weight.dims(0), weight.dims(1), std::int64_t{1},
                           weight.dims(2), weight.dims(3)}) result.add_dims(dim);
    return result;
  };
  const auto asGain = [] (const onnx::TensorProto& tensor) {
    auto result = tensor;
    result.mutable_dims()->Truncate(1);
    return result;
  };
  const auto& headGain = get("decoder.head.0.gamma");
  const auto base = headGain.dims_size() == 4 ? headGain.dims(0) : 0;
  if (base < 2 || base > LIMIT / 12)
    throw std::invalid_argument("invalid Wan2.1 decoder base width");
  requireShape(headGain, {base, 1, 1, 1}, "decoder.head.0.gamma");
  const auto outputGain = asGain(headGain);
  const auto latent = static_cast<std::int64_t>(latentMean.size());
  const auto latentProjection = conv("conv2", latent, latent, 1, 1);
  const auto inputProjection = conv("decoder.conv1", 4 * base, latent, 3, 3);
  const auto rgbProjection = conv("decoder.head.2", wan22 ? 12 : 3, base, 3, 3);

  auto tile = latentTile;
  std::vector<std::pair<std::string, VaeStageLayout>> stages;
  const auto lowerConv = [&] (const VaeConvolutionParameters& parameters) {
    return lowerVaeCausalConv(tile, parameters.weight, parameters.bias, material);
  };
  const auto residual = [&] (const std::string& name, std::int64_t in, std::int64_t out) {
    const auto path = name + ".residual.";
    const auto& firstGain = get(path + "0.gamma");
    const auto& secondGain = get(path + "3.gamma");
    requireShape(firstGain, {in, 1, 1, 1}, path + "0.gamma");
    requireShape(secondGain, {out, 1, 1, 1}, path + "3.gamma");
    std::optional<SummedProjection> shortcut;
    if (in != out) shortcut = lowerConv(conv(name + ".shortcut", out, in, 1, 1));
    stages.emplace_back(name, connectVaeResidual(
      lowerVaeRmsSilu(tile, asGain(firstGain), material), lowerConv(conv(path + "2", out, in, 3, 3)),
      lowerVaeRmsSilu(tile, asGain(secondGain), material), lowerConv(conv(path + "6", out, out, 3, 3)),
      std::move(shortcut)));
  };
  auto channels = 4 * base;
  residual("decoder.middle.0", channels, channels);
  const auto& middleGain = get("decoder.middle.1.norm.gamma");
  requireShape(middleGain, {channels, 1, 1}, "decoder.middle.1.norm.gamma");
  const auto qkv = conv("decoder.middle.1.to_qkv", 3 * channels, channels, 0, 1);
  const auto projection = conv("decoder.middle.1.proj", channels, channels, 0, 1);
  stages.emplace_back("decoder.middle.1", lowerVaeAttention(
    tile, {asGain(middleGain), asSpatial(qkv.weight), qkv.bias,
           asSpatial(projection.weight), projection.bias}, keyTile, scoreBytes, material));
  residual("decoder.middle.2", channels, channels);

  const std::array<std::int64_t, 4> widths{4 * base, 4 * base, 2 * base, base};
  int index = 0;
  for (std::size_t scale = 0; scale < widths.size(); ++scale) {
    const auto groupStart = stages.size();
    const auto groupTile = tile;
    const auto groupChannels = channels;
    const auto groupName = "decoder.upsamples." + std::to_string(scale);
    for (int block = 0; block < 3; ++block) {
      const auto name = wan22 ? groupName + ".upsamples." + std::to_string(block)
                              : "decoder.upsamples." + std::to_string(index++);
      residual(name, channels, widths[scale]);
      channels = widths[scale];
    }
    if (scale + 1 == widths.size()) break;
    const auto name = wan22 ? groupName + ".upsamples.3"
                            : "decoder.upsamples." + std::to_string(index++);
    const auto spatial = conv(name + ".resample.1", wan22 ? channels : channels / 2, channels, 0, 3);
    const auto spatialWeight = asSpatial(spatial.weight);
    std::optional<VaeConvolutionParameters> temporal;
    if (scale < 2) temporal.emplace(conv(name + ".time_conv", 2 * channels, channels, 3, 1));
    stages.emplace_back(name, lowerVaeResample(tile, {spatialWeight, spatial.bias},
                                             temporal, firstChunk, material));
    if (!wan22) channels /= 2;
    tile.height *= 2;
    tile.width *= 2;
    if (temporal && !firstChunk) tile.frames *= 2;
    if (wan22) {
      VaeStageLayout main;
      StageComposer group(main, "wan22-vae-upsample-main");
      TensorPort current{"frames", "float32",
        {std::int64_t{1}, groupChannels, groupTile.frames, groupTile.height, groupTile.width}};
      declareFloat(group.graph().add_input(), current);
      for (auto i = groupStart; i < stages.size(); ++i)
        current = group.addStage(std::move(stages[i].second), stages[i].first + "/", current);
      declareFloat(group.graph().add_output(), current);
      stages.resize(groupStart);
      stages.emplace_back(groupName, connectVaeUpsampleResidual(std::move(main), groupTile,
        groupChannels, channels, temporal.has_value(), firstChunk));
    }
  }
  for (const auto& [name, tensor] : parameters) {
    if ((name.compare(0, 8, "decoder.") == 0 || name.compare(0, 6, "conv2.") == 0) &&
        bound.count(name) == 0) {
      throw std::invalid_argument("unused Wan2.1 decoder parameter: " + name);
    }
  }
  return connectVaeDecoder(
    latentTile, {std::move(latentMean), std::move(latentInverseStd), latentProjection,
                 inputProjection, outputGain, rgbProjection, wan22 ? 2 : 1}, std::move(stages), material);
}

VaeStageLayout
lowerWan21VaeDecoder(const VaeConvTile& tile,
                    const std::map<std::string, onnx::TensorProto>& parameters,
                    std::vector<float> mean, std::vector<float> inverseStd,
                    bool first, std::int64_t keyTile, std::uint64_t scoreBytes,
                    const WanInitializerAccess& material)
{
  return lowerCheckpointVaeDecoder(tile, parameters, std::move(mean), std::move(inverseStd),
                                  first, keyTile, scoreBytes, material, false);
}

VaeStageLayout
lowerWan22VaeDecoder(const VaeConvTile& tile,
                    const std::map<std::string, onnx::TensorProto>& parameters,
                    std::vector<float> mean, std::vector<float> inverseStd,
                    bool first, std::int64_t keyTile, std::uint64_t scoreBytes,
                    const WanInitializerAccess& material)
{
  return lowerCheckpointVaeDecoder(tile, parameters, std::move(mean), std::move(inverseStd),
                                  first, keyTile, scoreBytes, material, true);
}

static VaeStageLayout
composeVaeClip(const VaeStageLayout& first, const VaeStageLayout& later,
               std::int64_t latentFrames, std::size_t maxGraphNodes, bool sharedSidecar)
{
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (latentFrames <= 0 || latentFrames > LIMIT / 4)
    throw std::invalid_argument("invalid Wan VAE latent clip length");
  const auto findPort = [] (const auto& ports, const std::string& name) {
    const auto it = std::find_if(ports.begin(), ports.end(),
                                 [&] (const auto& port) { return port.name() == name; });
    if (it == ports.end()) throw std::invalid_argument("Wan VAE phase has no port: " + name);
    return readVideoPort(*it);
  };
  const auto& firstGraph = first.model.graph();
  const auto& laterGraph = later.model.graph();
  const auto requiredNodes = std::uint64_t{2} + firstGraph.node_size() +
    (latentFrames > 1 ? later.historySuccessorByInput.size() :
                        first.historySuccessorByInput.size());
  const auto limit = std::min(maxGraphNodes,
                             static_cast<std::size_t>(std::numeric_limits<int>::max()));
  if (requiredNodes > limit ||
      static_cast<std::uint64_t>(latentFrames - 1) >
        (limit - requiredNodes) / (1U + laterGraph.node_size())) {
    throw std::invalid_argument("Wan VAE clip graph exceeds node budget");
  }
  const auto input = findPort(firstGraph.input(), "latent");
  const auto firstRgb = findPort(firstGraph.output(), "rgb");
  auto laterRgb = firstRgb;
  if (latentFrames > 1) {
    requireCompatible(input, findPort(laterGraph.input(), "latent"));
    laterRgb = findPort(laterGraph.output(), "rgb");
    auto expected = firstRgb;
    expected.shape[2] = std::int64_t{4};
    requireCompatible(expected, laterRgb);
  }
  if (std::get<std::int64_t>(input.shape[2]) != 1 ||
      std::get<std::int64_t>(firstRgb.shape[2]) != 1)
    throw std::invalid_argument("Wan VAE initial phase must consume and produce one frame");
  struct HistoryEntry
  {
    TensorPort input;
    TensorPort output;
    std::string entryId;
  };
  const auto historyEntries = [&] (const VaeStageLayout& phase) {
    const auto& source = phase.model.graph();
    std::map<std::string, HistoryEntry> entries;
    for (const auto& [in, out] : phase.historySuccessorByInput) {
      auto input = findPort(source.input(), in);
      auto output = findPort(source.output(), out);
      requireCompatible(input, output);
      std::string owner;
      for (const auto& [id, entry] : phase.entries) {
        if (!std::any_of(entry.inputs.begin(), entry.inputs.end(),
                         [&] (const auto& port) { return port.name == in; })) {
          continue;
        }
        if (!owner.empty() || std::none_of(entry.outputs.begin(), entry.outputs.end(),
                                   [&] (const auto& port) { return port.name == out; }))
          throw std::invalid_argument("Wan VAE history needs one owning entry: " + in);
        owner = id;
      }
      if (owner.empty()) throw std::invalid_argument("Wan VAE history has no owning entry: " + in);
      entries.emplace(in, HistoryEntry{std::move(input), std::move(output), std::move(owner)});
    }
    return entries;
  };
  const auto firstHistory = historyEntries(first);
  const auto laterHistory = latentFrames > 1 ? historyEntries(later) :
                                               std::map<std::string, HistoryEntry>{};
  if (latentFrames > 1) {
    for (const auto& [port, entry] : firstHistory) {
      const auto successor = laterHistory.find(port);
      if (successor == laterHistory.end() || successor->second.output.name != entry.output.name ||
          later.rankOneEntries.count(successor->second.entryId) !=
            first.rankOneEntries.count(entry.entryId)) {
        throw std::invalid_argument("Wan VAE later phase changes history identity: " + port);
      }
      requireCompatible(entry.output, successor->second.input);
    }
  }

  VaeStageLayout result;
  StageComposer composer(result, "wan-vae-clip");
  auto& graph = composer.graph();
  auto clip = input;
  clip.name = "latent-clip";
  clip.shape[2] = latentFrames;
  declareFloat(graph.add_input(), clip);
  const auto emit = [&] (const char* op, const std::string& output,
                         const std::vector<std::string>& inputs) {
    auto* node = graph.add_node();
    node->set_name(output);
    node->set_op_type(op);
    for (const auto& port : inputs) node->add_input(port);
    node->add_output(output);
    return node;
  };
  const auto ints = [&] (const std::string& name, const std::vector<std::int64_t>& values) {
    auto* value = graph.add_initializer();
    value->set_name(name);
    value->set_data_type(onnx::TensorProto::INT64);
    value->add_dims(values.size());
    for (const auto integer : values) value->add_int64_data(integer);
  };
  ints("time-axis", {2});

  std::map<std::string, const onnx::TensorProto*> firstMaterial;
  const auto prepareMaterial = [&] (const VaeStageLayout& phase, bool initial) {
    std::map<std::string, std::string> bindings;
    for (const auto& material : phase.model.graph().initializer()) {
      if (material.name().empty() ||
          (!sharedSidecar && material.data_location() == onnx::TensorProto::EXTERNAL))
        throw std::invalid_argument("Wan VAE clip needs owned phase initializers");
      const auto firstValue = firstMaterial.find(material.name());
      const bool shared = !initial && firstValue != firstMaterial.end() &&
        material.SerializeAsString() == firstValue->second->SerializeAsString();
      const bool parameter = phase.parameterNames.count(material.name()) != 0;
      if (!initial && first.parameterNames.count(material.name()) != 0 && (!shared || !parameter))
        throw std::invalid_argument("Wan VAE phase changes a checkpoint parameter: " + material.name());
      const auto name = std::string(initial || shared ? "phase-first/" : "phase-later/") +
                        material.name();
      if (!bindings.emplace(material.name(), name).second)
        throw std::invalid_argument("Wan VAE phase repeats an initializer");
      if (parameter) result.parameterNames.insert(name);
      if (initial) firstMaterial.emplace(material.name(), &material);
      if (!shared) {
        auto* target = graph.add_initializer();
        *target = material;
        target->set_name(name);
      }
    }
    for (const auto& name : phase.parameterNames) {
      if (bindings.count(name) == 0)
        throw std::invalid_argument("Wan VAE phase has no checkpoint parameter: " + name);
    }
    return bindings;
  };
  const auto materialFirst = prepareMaterial(first, true);
  const auto materialLater = latentFrames > 1 ? prepareMaterial(later, false) :
                                              std::map<std::string, std::string>{};
  if (latentFrames > 1) {
    for (const auto& name : first.parameterNames) {
      if (materialLater.count(name) == 0)
        throw std::invalid_argument("Wan VAE later phase omits checkpoint parameter: " + name);
    }
  }
  std::map<std::string, TensorPort> history;
  std::vector<TensorPort> frames;
  const bool pipeline = first.decoderMiddleEnd && (latentFrames == 1 || later.decoderMiddleEnd);
  for (std::int64_t index = 0; index < latentFrames; ++index) {
    const bool initial = index == 0;
    const auto& phase = initial ? first : later;
    const auto& source = phase.model.graph();
    const auto prefix = "chunk-" + std::to_string(index) + "/";
    auto latent = input;
    latent.name = prefix + "latent";
    ints(prefix + "start", {index});
    ints(prefix + "end", {index + 1});
    const auto slice = static_cast<std::uint64_t>(graph.node_size());
    emit("Slice", latent.name, {clip.name, prefix + "start", prefix + "end", "time-axis"});
    if (pipeline) result.pipelineStageByNode.push_back(0);
    result.entries.emplace(prefix + "slice", OnnxEntryCut{{slice}, {clip}, {latent}});

    auto bindings = initial ? materialFirst : materialLater;
    bindings.emplace(input.name, latent.name);
    std::map<std::string, std::map<std::string, std::uint64_t>> zerosByEntry;
    std::set<std::string> unusedHistories;
    for (const auto& [in, entry] : initial ? firstHistory : laterHistory) {
      auto value = entry.input;
      auto next = entry.output;
      const auto previous = history.find(in);
      if (previous != history.end()) {
        value = previous->second;
      }
      else {
        const auto id = prefix + "zero/" + in;
        value.name = id;
        std::vector<std::int64_t> shape;
        for (const auto& dim : value.shape) shape.push_back(std::get<std::int64_t>(dim));
        ints(id + "/shape", shape);
        const auto zero = static_cast<std::uint64_t>(graph.node_size());
        emit("ConstantOfShape", id, {id + "/shape"});
        if (pipeline) {
          const auto ownerNode = phase.entries.at(entry.entryId).nodeIndices.at(0);
          result.pipelineStageByNode.push_back(ownerNode >= *phase.decoderMiddleEnd);
        }
        zerosByEntry[entry.entryId].emplace(in, zero);
      }
      bindings.emplace(in, value.name);
      next.name = prefix + next.name;
      if (index + 1 < latentFrames)
        history.insert_or_assign(in, std::move(next));
      else
        unusedHistories.insert(std::move(next.name));
    }

    const auto offset = static_cast<std::uint64_t>(graph.node_size());
    for (const auto& [id, entry] : phase.entries) {
      auto cut = rebaseCut(entry, offset, prefix, bindings);
      cut.outputs.erase(std::remove_if(cut.outputs.begin(), cut.outputs.end(),
                         [&] (const auto& port) { return unusedHistories.count(port.name) != 0; }),
                        cut.outputs.end());
      const auto zeros = zerosByEntry.find(id);
      if (zeros != zerosByEntry.end()) {
        for (const auto& [input, node] : zeros->second) {
          const auto& name = bindings.at(input);
          cut.inputs.erase(std::remove_if(cut.inputs.begin(), cut.inputs.end(),
                           [&] (const auto& port) { return port.name == name; }), cut.inputs.end());
          cut.nodeIndices.push_back(node);
        }
        std::sort(cut.nodeIndices.begin(), cut.nodeIndices.end());
      }
      result.entries.emplace(prefix + id, std::move(cut));
    }
    for (const auto& id : phase.rankOneEntries) result.rankOneEntries.insert(prefix + id);
    for (const auto& [node, reduction] : phase.reductions) {
      result.reductions.emplace(offset + node,
        OnnxReductionCut{rebaseCut(reduction.cut, offset, prefix, bindings),
                         reduction.bothRanks, reduction.firstRank});
    }
    onnx::GraphProto fragment;
    *fragment.mutable_node() = source.node();
    appendFragment(graph, std::move(fragment), prefix, bindings);
    if (pipeline) {
      for (std::size_t node = 0; node < static_cast<std::size_t>(source.node_size()); ++node)
        result.pipelineStageByNode.push_back(node >= *phase.decoderMiddleEnd);
    }
    auto rgb = initial ? firstRgb : laterRgb;
    rgb.name = prefix + rgb.name;
    frames.push_back(std::move(rgb));
  }
  auto rgb = firstRgb;
  rgb.shape[2] = 1 + 4 * (latentFrames - 1);
  std::vector<std::string> sources;
  for (const auto& frame : frames) sources.push_back(frame.name);
  const auto node = static_cast<std::uint64_t>(graph.node_size());
  auto* axis = emit("Concat", rgb.name, sources)->add_attribute();
  if (pipeline) result.pipelineStageByNode.push_back(1);
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(2);
  declareFloat(graph.add_output(), rgb);
  result.entries.emplace("rgb", OnnxEntryCut{{node}, std::move(frames), {std::move(rgb)}});
  return result;
}

VaeStageLayout
connectVaeClip(const VaeStageLayout& first, const VaeStageLayout& later,
               std::int64_t latentFrames, std::size_t maxGraphNodes)
{
  return composeVaeClip(first, later, latentFrames, maxGraphNodes, false);
}

static CheckpointVaeClip
lowerCheckpointVaeClip(const WanCheckpoint& checkpoint, const VaeConvTile& tile,
                  const std::vector<float>& latentMean, const std::vector<float>& latentInverseStd,
                  std::int64_t latentFrames, std::int64_t keyTile, std::uint64_t scoreBytes,
                  std::size_t maxGraphNodes, bool wan22)
{
  auto sidecar = std::make_shared<WanCheckpointSidecar>(nullptr);
  using SliceKey = std::tuple<std::string, std::size_t, std::uint64_t, std::uint64_t>;
  std::map<SliceKey, onnx::TensorProto> slices;
  const WanInitializerAccess material{{}, [&](const auto& tensor, auto axis, auto begin, auto end) {
    const SliceKey key{tensor.name(), axis, begin, end};
    auto it = slices.find(key);
    if (it == slices.end()) {
      it = slices.emplace(key, sidecar->addSlice(checkpoint, tensor.name(), axis, begin, end)).first;
    }
    auto result = it->second;
    // The Conv2D and RMS source bindings only add or remove singleton axes.
    *result.mutable_dims() = tensor.dims();
    result.set_dims(axis, end - begin);
    return result;
  }};
  const auto lower = [&](bool first) {
    return lowerCheckpointVaeDecoder(tile, checkpoint.parameters(), latentMean, latentInverseStd,
                                    first, keyTile, scoreBytes, material, wan22);
  };
  const auto first = lower(true);
  const auto later = latentFrames > 1 ? lower(false) : VaeStageLayout{};
  return {composeVaeClip(first, later, latentFrames, maxGraphNodes, true), std::move(sidecar)};
}

CheckpointVaeClip
lowerWan21VaeClip(const WanCheckpoint& checkpoint, const VaeConvTile& tile,
                       const std::vector<float>& mean, const std::vector<float>& inverseStd,
                       std::int64_t latentFrames, std::int64_t keyTile, std::uint64_t scoreBytes,
                       std::size_t maxGraphNodes)
{
  return lowerCheckpointVaeClip(checkpoint, tile, mean, inverseStd, latentFrames,
                               keyTile, scoreBytes, maxGraphNodes, false);
}

CheckpointVaeClip
lowerWan22VaeClip(const WanCheckpoint& checkpoint, const VaeConvTile& tile,
                       const std::vector<float>& mean, const std::vector<float>& inverseStd,
                       std::int64_t latentFrames, std::int64_t keyTile, std::uint64_t scoreBytes,
                       std::size_t maxGraphNodes)
{
  return lowerCheckpointVaeClip(checkpoint, tile, mean, inverseStd, latentFrames,
                               keyTile, scoreBytes, maxGraphNodes, true);
}

OnnxParallelPlan
makeVaePipelinePlan(const VaeStageLayout& layout, const std::array<NativeStageLayout, 2>& stages)
{
  if (layout.pipelineStageByNode.empty() || stages[0].degree() != 1 || stages[1].degree() != 1)
    throw std::invalid_argument("Wan VAE PP needs two single-unit stages and the decoder middle cut");
  const auto ranked = makeVaeTensorParallelPlan(layout, {"decoder", {}, {"rank0", "rank1"}});
  return makeStagedEntryPlan(layout.model.graph(), ranked, stages, layout.pipelineStageByNode);
}

} // namespace ndnsf::di::wan
