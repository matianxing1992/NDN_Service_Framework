#include "wan-video-block.hpp"

#include "wan-checkpoint.hpp"
#include "wan-feed-forward.hpp"
#include "wan-patch-embedding.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

std::uint64_t
offsetOf(const onnx::TensorProto& tensor)
{
  for (const auto& attribute : tensor.external_data()) {
    if (attribute.key() == "offset") return std::stoull(attribute.value());
  }
  throw std::logic_error("Wan checkpoint slice has no sidecar offset");
}

void
declarePort(onnx::ValueInfoProto& port, const char* name, std::int64_t tokens, std::int64_t width,
            bool dynamicTokens)
{
  port.set_name(name);
  auto& tensor = *port.mutable_type()->mutable_tensor_type();
  tensor.set_elem_type(onnx::TensorProto::FLOAT);
  auto& shape = *tensor.mutable_shape();
  shape.add_dim()->set_dim_value(1);
  if (dynamicTokens) shape.add_dim()->set_dim_param("valid_tokens");
  else shape.add_dim()->set_dim_value(tokens);
  shape.add_dim()->set_dim_value(width);
}

onnx::NodeProto&
emit(onnx::GraphProto& graph, const char* op, const char* output,
     std::initializer_list<const char*> inputs)
{
  auto& node = *graph.add_node();
  node.set_name(output);
  node.set_op_type(op);
  for (const auto* input : inputs) node.add_input(input);
  node.add_output(output);
  return node;
}

using Matrix = std::pair<onnx::TensorProto, onnx::TensorProto>;

onnx::ModelProto
linearGraph(const Matrix& first, std::int64_t tokens, const Matrix* second = nullptr,
            const onnx::TensorProto* gain = nullptr, bool dynamicTokens = false)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-checkpoint-projection");
  declarePort(*graph.add_input(), "input", tokens, first.first.dims(0), dynamicTokens);
  declarePort(*graph.add_output(), "output", tokens,
              second ? second->first.dims(1) : first.first.dims(1), dynamicTokens);
  const auto add = [&](const onnx::TensorProto& parameter, const char* label) {
    auto& tensor = *graph.add_initializer();
    tensor = parameter;
    tensor.set_name(label);
  };
  add(first.first, "weight");
  add(first.second, "bias");
  emit(graph, "MatMul", "linear", {"input", "weight"});
  emit(graph, "Add", second || gain ? "biased" : "output", {"linear", "bias"});
  if (second) {
    add(second->first, "down-weight");
    add(second->second, "down-bias");
    auto& approximate = *emit(graph, "Gelu", "activated", {"biased"}).add_attribute();
    approximate.set_name("approximate");
    approximate.set_type(onnx::AttributeProto::STRING);
    approximate.set_s("tanh");
    emit(graph, "MatMul", "contracted", {"activated", "down-weight"});
    emit(graph, "Add", "output", {"contracted", "down-bias"});
  }
  if (gain) {
    auto& axes = *graph.add_initializer();
    axes.set_name("axes");
    axes.set_data_type(onnx::TensorProto::INT64);
    axes.add_dims(1);
    axes.add_int64_data(-1);
    auto& epsilon = *graph.add_initializer();
    epsilon.set_name("epsilon");
    epsilon.set_data_type(onnx::TensorProto::FLOAT);
    epsilon.add_float_data(1e-6F);
    add(*gain, "gain");
    emit(graph, "Mul", "squares", {"biased", "biased"});
    emit(graph, "ReduceMean", "mean", {"squares", "axes"});
    emit(graph, "Add", "stabilized", {"mean", "epsilon"});
    emit(graph, "Sqrt", "rms", {"stabilized"});
    emit(graph, "Div", "normalized", {"biased", "rms"});
    emit(graph, "Mul", "output", {"normalized", "gain"});
  }
  return model;
}

void
lowerCheckpointMatrixOperands(VideoDenoiserLayout& layout,
                              const std::set<std::uint64_t>& physicalMatrices)
{
  auto& graph = *layout.model.mutable_graph();
  std::set<std::string> remaining;
  for (auto& tensor : *graph.mutable_initializer()) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL ||
        physicalMatrices.count(offsetOf(tensor)) == 0) continue;
    if (tensor.dims_size() != 2 || !remaining.insert(tensor.name()).second)
      throw std::logic_error("invalid Wan DiT checkpoint matrix operand");
    tensor.mutable_dims()->SwapElements(0, 1);
  }
  if (remaining.size() != physicalMatrices.size())
    throw std::logic_error("Wan DiT checkpoint matrix is not present in the lowered graph");
  const auto matrixNames = remaining;

  google::protobuf::RepeatedPtrField<onnx::NodeProto> nodes;
  nodes.Swap(graph.mutable_node());
  std::vector<std::uint64_t> starts;
  for (auto& node : nodes) {
    starts.push_back(graph.node_size());
    for (int port = 0; port < node.input_size(); ++port) {
      const auto& name = node.input(port);
      if (matrixNames.count(name) == 0) continue;
      if (node.op_type() != "MatMul" || port != 1 || remaining.erase(name) != 1)
        throw std::logic_error("Wan DiT checkpoint matrix requires one MatMul consumer");
      const auto transpose = name + "/checkpoint-transpose";
      auto& permutation =
        *emit(graph, "Transpose", transpose.c_str(), {name.c_str()}).add_attribute();
      permutation.set_name("perm");
      permutation.set_type(onnx::AttributeProto::INTS);
      permutation.add_ints(1);
      permutation.add_ints(0);
      node.set_input(port, transpose);
    }
    *graph.add_node() = std::move(node);
  }
  starts.push_back(graph.node_size());
  if (!remaining.empty())
    throw std::logic_error("Wan DiT checkpoint matrix has no consumer");

  for (auto& entry : layout.tensorParallel.entries) {
    std::vector<std::uint64_t> translated;
    for (const auto old : entry.second.nodeIndices) {
      for (auto i = starts.at(old); i < starts.at(old + 1); ++i) translated.push_back(i);
    }
    entry.second.nodeIndices = std::move(translated);
  }
  for (auto& end : layout.blockEnds) end = starts.at(end);
}

} // namespace

CheckpointDenoiser
lowerWan21Denoiser(const WanCheckpoint& checkpoint, const WanDenoiserGeometry& shape,
                   const NativeStageLayout& stage)
{
  const auto& parameters = checkpoint.parameters();
  constexpr std::size_t FIXED_PARAMETERS = 15;
  constexpr std::size_t PARAMETERS_PER_BLOCK = 27;
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (parameters.size() < FIXED_PARAMETERS + PARAMETERS_PER_BLOCK ||
      (parameters.size() - FIXED_PARAMETERS) % PARAMETERS_PER_BLOCK != 0 ||
      shape.heads < 2 || shape.textTokens <= 0 || shape.grid.height > LIMIT / 2 ||
      shape.grid.width > LIMIT / 2 || shape.grid.height <= 0 || shape.grid.width <= 0)
    throw std::invalid_argument("invalid Wan DiT checkpoint or geometry");

  auto sidecar = std::make_shared<WanCheckpointSidecar>(nullptr);
  std::set<std::string> bound;
  std::set<std::string> linearNames;
  std::set<std::uint64_t> physicalMatrices;
  const auto get = [&](const std::string& name) -> const onnx::TensorProto& {
    const auto found = parameters.find(name);
    if (found == parameters.end() || !bound.insert(name).second)
      throw std::invalid_argument("missing or repeated Wan DiT checkpoint key: " + name);
    return found->second;
  };
  // Shards may reuse physical offsets. A logical TensorProto's location also
  // participates in the lookup, independently of its fragment-local name.
  std::map<std::vector<std::string>, std::string> sourceNames;
  const auto coordinate = [](const onnx::TensorProto& tensor) {
    std::vector<std::string> values;
    for (const auto& field : tensor.external_data()) values.push_back(field.value());
    return values;
  };
  const auto bind = [&](const std::string& name) -> const onnx::TensorProto& {
    const auto& tensor = get(name);
    sourceNames.emplace(coordinate(tensor), name);
    return tensor;
  };
  const auto matrix = [&](const std::string& prefix) {
    auto weight = bind(prefix + ".weight");
    if (weight.dims_size() != 2)
      throw std::invalid_argument("Wan DiT linear weight must be a matrix");
    weight.mutable_dims()->SwapElements(0, 1);
    linearNames.insert(weight.name());
    return Matrix{std::move(weight), bind(prefix + ".bias")};
  };
  const WanInitializerAccess material{checkpoint.reader(),
    [&](const auto& tensor, auto axis, auto begin, auto end) {
      const auto& name = sourceNames.at(coordinate(tensor));
      const bool linear = linearNames.count(name) != 0;
      auto result = sidecar->addSlice(checkpoint, name, linear ? 1 - axis : axis, begin, end);
      if (linear) {
        physicalMatrices.insert(offsetOf(result));
        // The fragments still use logical MatMul dimensions until all cuts are connected.
        result.mutable_dims()->SwapElements(0, 1);
      }
      return result;
    }};

  const auto& patchWeight = bind("patch_embedding.weight");
  (void)bind("patch_embedding.bias");
  if (patchWeight.dims_size() != 5 || patchWeight.dims(0) % shape.heads != 0)
    throw std::invalid_argument("Wan DiT patch width does not match its attention heads");
  auto patch = lowerWan21PatchEmbedding(
    checkpoint, {patchWeight.dims(1), shape.grid.frames, shape.grid.height * 2, shape.grid.width * 2},
    material.slicer);
  const auto textUp = matrix("text_embedding.0");
  const auto textDown = matrix("text_embedding.2");
  auto text = lowerTextEmbedding(linearGraph(textUp, shape.textTokens, &textDown, nullptr, true),
                                  shape.textTokens, material);
  const auto timeUp = matrix("time_embedding.0");
  const auto timeDown = matrix("time_embedding.2");
  const auto timeModulation = matrix("time_projection.1");
  auto time = lowerTimeEmbedding(
    {{{timeUp.first, timeUp.second}, {timeDown.first, timeDown.second},
      {timeModulation.first, timeModulation.second}}}, material);
  const auto prediction = matrix("head.head");
  auto head = lowerVideoHead(shape.grid, bind("head.modulation"), prediction.first,
                             prediction.second, 1e-6F, material);

  const auto width = patchWeight.dims(0) / shape.heads;
  const VideoRotaryTile rotary{shape.grid.frames, shape.grid.height, shape.grid.width,
                               shape.heads, width, 0, shape.grid.paddedTokens};
  const auto attention = [&](const std::string& prefix, bool video) {
    const auto q = matrix(prefix + "q");
    const auto k = matrix(prefix + "k");
    const auto v = matrix(prefix + "v");
    const auto o = matrix(prefix + "o");
    const auto& gainQ = bind(prefix + "norm_q.weight");
    const auto& gainK = bind(prefix + "norm_k.weight");
    const auto tokens = video ? shape.grid.paddedTokens : shape.textTokens;
    const auto query = linearGraph(q, shape.grid.paddedTokens, nullptr, &gainQ);
    const auto out = linearGraph(o, shape.grid.paddedTokens);
    const auto key = linearGraph(k, tokens, nullptr, &gainK);
    const auto value = linearGraph(v, tokens);
    if (video) {
      return VideoBlockAttention{
        lowerVideoAttentionContext(key, value, rotary, shape.rotaryBytes, material),
        lowerVideoAttentionQuery(query, out, rotary, tokens, shape.videoKeyTile,
                                 shape.scoreBytes, shape.rotaryBytes, material)};
    }
    return VideoBlockAttention{
      lowerTextAttentionContext(key, value, shape.heads, width, material),
      lowerTextAttentionQuery(query, out, shape.heads, width, tokens, shape.textKeyTile,
                              shape.scoreBytes, material)};
  };
  std::vector<ConnectedVideoBlock> blocks;
  const auto count = (parameters.size() - FIXED_PARAMETERS) / PARAMETERS_PER_BLOCK;
  blocks.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto prefix = "blocks." + std::to_string(index) + ".";
    const auto expansion = matrix(prefix + "ffn.0");
    const auto contraction = matrix(prefix + "ffn.2");
    auto ffn = lowerFeedForward(linearGraph(expansion, shape.grid.paddedTokens, &contraction),
                                material);
    auto block = lowerVideoBlock(shape.grid.paddedTokens, bind(prefix + "modulation"),
                                 bind(prefix + "norm3.weight"), bind(prefix + "norm3.bias"),
                                 1e-6F, material);
    auto self = attention(prefix + "self_attn.", true);
    auto cross = attention(prefix + "cross_attn.", false);
    blocks.push_back(connectVideoBlock(std::move(block), std::move(self), std::move(cross),
                                       std::move(ffn), stage));
  }
  if (bound.size() != parameters.size())
    throw std::invalid_argument("Wan DiT checkpoint has unused parameters");
  auto layout = connectVideoDenoiser(std::move(patch), std::move(text), std::move(time),
                                    std::move(blocks), std::move(head), shape.grid, stage);
  lowerCheckpointMatrixOperands(layout, physicalMatrices);
  return {std::move(layout), std::move(sidecar)};
}

} // namespace ndnsf::di::wan
