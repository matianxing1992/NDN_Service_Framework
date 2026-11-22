#include "wan-umt5.hpp"

#include "wan-checkpoint.hpp"
#include "wan-entry-plan.hpp"
#include "wan-graph-fragment.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/initializer-slice.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

class EmbeddingSidecar final : public NativeCanonicalByteRangeSource
{
public:
  EmbeddingSidecar(std::shared_ptr<const NativeCanonicalByteRangeSource> table,
                   std::uint64_t base, std::array<InitializerSlice, 2> slices)
    : m_table(std::move(table))
    , m_base(base)
    , m_slices(std::move(slices))
  {
    if (!m_table || m_base > m_table->size() ||
        m_slices[0].sourceBytes() > m_table->size() - m_base) {
      throw std::invalid_argument("UMT5 table exceeds its source range");
    }
  }

  std::uint64_t
  size() const noexcept override
  {
    return m_slices[0].sourceBytes();
  }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    constexpr auto WINDOW = NativeCanonicalMaterialBundleMaxBytes;
    if (offset > size() || length > size() - offset || length > WINDOW)
      throw std::out_of_range("UMT5 sidecar read exceeds its window");
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(length));
    for (const auto& slice : m_slices) {
      if (offset >= slice.sliceBytes()) {
        offset -= slice.sliceBytes();
        continue;
      }
      const auto bytes = std::min(length, slice.sliceBytes() - offset);
      slice.forEachRange(offset, bytes, WINDOW, [&] (auto location, auto count) {
        const auto part = m_table->read(m_base + location, count);
        if (part.size() != count) throw std::runtime_error("UMT5 table read was incomplete");
        result.insert(result.end(), part.begin(), part.end());
      });
      length -= bytes;
      if (length == 0) break;
      offset = 0;
    }
    return result;
  }

private:
  std::shared_ptr<const NativeCanonicalByteRangeSource> m_table;
  std::uint64_t m_base;
  std::array<InitializerSlice, 2> m_slices;
};

void
declare(onnx::ValueInfoProto& port, const NativeAssemblyTensorContractV3& contract,
        onnx::TensorProto::DataType type)
{
  port.set_name(contract.name);
  auto* tensor = port.mutable_type()->mutable_tensor_type();
  tensor->set_elem_type(type);
  for (const auto& dim : contract.shape) {
    auto* declared = tensor->mutable_shape()->add_dim();
    if (const auto* count = std::get_if<std::int64_t>(&dim)) {
      declared->set_dim_value(*count);
    }
    else {
      declared->set_dim_param(std::get<std::string>(dim));
    }
  }
}

onnx::NodeProto&
emit(onnx::GraphProto& graph, const char* op, const std::string& output,
     std::initializer_list<std::string> inputs)
{
  auto* node = graph.add_node();
  node->set_op_type(op);
  node->set_name(output);
  node->add_output(output);
  for (const auto& input : inputs) node->add_input(input);
  return *node;
}

struct ParameterAccess
{
  const NativeOnnxExternalReader& external;
  const WanCheckpoint* checkpoint = nullptr;
  WanCheckpointSidecar* sidecar = nullptr;

  onnx::TensorProto
  slice(const onnx::TensorProto& tensor, int axis, std::uint64_t begin, std::uint64_t end) const
  {
    if (checkpoint)
      return sidecar->addSlice(*checkpoint, tensor.name(), axis, begin, end);
    return sliceNativeOnnxFloatInitializer(tensor, axis, begin, end, external);
  }
};

std::string
matMulWeight(onnx::GraphProto& graph, const std::string& stored, const ParameterAccess& weights)
{
  if (!weights.checkpoint) return stored;
  const auto value = stored + "/transpose";
  auto* perm = emit(graph, "Transpose", value, {stored}).add_attribute();
  perm->set_name("perm");
  perm->set_type(onnx::AttributeProto::INTS);
  perm->add_ints(1);
  perm->add_ints(0);
  return value;
}

std::vector<std::uint64_t>
nodesFrom(const onnx::GraphProto& graph, int first)
{
  std::vector<std::uint64_t> indices;
  for (int i = first; i < graph.node_size(); ++i) indices.push_back(i);
  return indices;
}

} // namespace

Umt5EmbeddingLayout
lowerUmt5Embedding(std::int64_t tokens, std::int64_t vocab, std::int64_t hidden,
                   std::shared_ptr<const NativeCanonicalByteRangeSource> table,
                   std::uint64_t tableOffset)
{
  if (tokens <= 0 || vocab <= 0 || hidden < 2 ||
      tokens > std::numeric_limits<std::int64_t>::max() / hidden /
               static_cast<std::int64_t>(sizeof(float))) {
    throw std::invalid_argument("invalid UMT5 embedding shape");
  }
  const auto range = nativeConvInputChannelRanges(hidden, 2);
  const std::vector<std::uint64_t> dims{static_cast<std::uint64_t>(vocab),
                                        static_cast<std::uint64_t>(hidden)};
  const std::array<InitializerSlice, 2> slices{
    InitializerSlice(dims, 1, range[0].first, range[0].second, sizeof(float)),
    InitializerSlice(dims, 1, range[1].first, range[1].second, sizeof(float))};
  Umt5EmbeddingLayout result;
  result.source.initializerRangeSource =
    std::make_shared<EmbeddingSidecar>(std::move(table), tableOffset, slices);

  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-umt5-token-embedding");
  const NativeAssemblyTensorContractV3 input{"token-ids", "int64", {std::int64_t{1}, tokens}};
  const NativeAssemblyTensorContractV3 output{
    "token-embedding", "float32", {std::int64_t{1}, tokens, hidden}};
  declare(*graph.add_input(), input, onnx::TensorProto::INT64);
  declare(*graph.add_output(), output, onnx::TensorProto::FLOAT);
  for (const auto& [name, value] :
       std::initializer_list<std::pair<const char*, std::int64_t>>{{"zero", 0}, {"vocab", vocab}}) {
    auto* boundary = graph.add_initializer();
    boundary->set_name(name);
    boundary->set_data_type(onnx::TensorProto::INT64);
    boundary->add_int64_data(value);
  }
  std::uint64_t offset = 0;
  for (std::size_t rank = 0; rank < range.size(); ++rank) {
    const auto name = "rank-" + std::to_string(rank) + "/";
    const auto rankWidth = static_cast<std::int64_t>(range[rank].second - range[rank].first);
    auto* weight = graph.add_initializer();
    weight->set_name(name + "table");
    weight->set_data_type(onnx::TensorProto::FLOAT);
    weight->set_data_location(onnx::TensorProto::EXTERNAL);
    weight->add_dims(vocab);
    weight->add_dims(rankWidth);
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, std::string>>{
           {"location", "model.onnx.data"}, {"offset", std::to_string(offset)},
           {"length", std::to_string(slices[rank].sliceBytes())}}) {
      auto* item = weight->add_external_data();
      item->set_key(key);
      item->set_value(value);
    }
    offset += slices[rank].sliceBytes();
    const auto first = graph.node_size();
    emit(graph, "GreaterOrEqual", name + "nonnegative", {input.name, "zero"});
    emit(graph, "Less", name + "below-vocab", {input.name, "vocab"});
    emit(graph, "And", name + "valid", {name + "nonnegative", name + "below-vocab"});
    emit(graph, "Where", name + "ids", {name + "valid", input.name, "vocab"});
    auto local = output;
    local.name = name + "embedded";
    local.shape.back() = rankWidth;
    emit(graph, "Gather", local.name, {weight->name(), name + "ids"});
    result.ranks[rank] = {nodesFrom(graph, first), {input}, {local}};
  }
  const auto first = graph.node_size();
  const auto& left = result.ranks[0].outputs.front();
  const auto& right = result.ranks[1].outputs.front();
  auto* axis = emit(graph, "Concat", output.name, {left.name, right.name}).add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(-1);
  result.join = {nodesFrom(graph, first), {left, right}, {output}};
  const auto wire = model.SerializeAsString();
  result.source.modelBytes.assign(wire.begin(), wire.end());
  return result;
}

static Umt5ReductionLayout
lowerUmt5FeedForwardImpl(std::int64_t tokens, const onnx::TensorProto& gate,
                     const onnx::TensorProto& expansion, const onnx::TensorProto& contraction,
                     const ParameterAccess& parameters)
{
  for (const auto* weight : {&gate, &expansion, &contraction}) {
    if (weight->data_type() != onnx::TensorProto::FLOAT || weight->dims_size() != 2 ||
        weight->dims(0) <= 0 || weight->dims(1) <= 0)
      throw std::invalid_argument("invalid UMT5 feed-forward matrix");
  }
  const int inputAxis = parameters.checkpoint ? 1 : 0;
  const int outputAxis = 1 - inputAxis;
  const auto hidden = gate.dims(inputAxis);
  const auto inner = gate.dims(outputAxis);
  if (tokens <= 0 || hidden < 2 || inner < 2 ||
      expansion.dims(inputAxis) != hidden || expansion.dims(outputAxis) != inner ||
      contraction.dims(inputAxis) != inner || contraction.dims(outputAxis) != hidden) {
    throw std::invalid_argument("UMT5 feed-forward dimensions disagree");
  }
  Umt5ReductionLayout layout;
  layout.model.set_ir_version(9);
  layout.model.add_opset_import()->set_version(20);
  auto& graph = *layout.model.mutable_graph();
  graph.set_name("wan-umt5-gated-ffn");
  const NativeAssemblyTensorContractV3 input{
    "hidden", "float32", {std::int64_t{1}, tokens, hidden}};
  layout.output = {"ffn-output", input.dtype, input.shape};
  declare(*graph.add_input(), input, onnx::TensorProto::FLOAT);
  declare(*graph.add_output(), layout.output, onnx::TensorProto::FLOAT);
  const auto ranges = nativeConvInputChannelRanges(inner, 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto name = "rank-" + std::to_string(rank) + "/";
    const auto [begin, end] = ranges[rank];
    const auto sliced = [&] (const onnx::TensorProto& source, int axis, const char* suffix) {
      auto* tensor = graph.add_initializer();
      *tensor = parameters.slice(source, axis, begin, end);
      tensor->set_name(name + suffix);
      return tensor->name();
    };
    const auto first = graph.node_size();
    const auto gating = matMulWeight(graph, sliced(gate, outputAxis, "gate"), parameters);
    const auto up = matMulWeight(graph, sliced(expansion, outputAxis, "expansion"), parameters);
    const auto down = matMulWeight(graph, sliced(contraction, inputAxis, "contraction"), parameters);
    emit(graph, "MatMul", name + "gated", {input.name, gating});
    auto* gelu = emit(graph, "Gelu", name + "activated", {name + "gated"}).add_attribute();
    gelu->set_name("approximate");
    gelu->set_type(onnx::AttributeProto::STRING);
    gelu->set_s("tanh");
    emit(graph, "MatMul", name + "expanded", {input.name, up});
    emit(graph, "Mul", name + "product", {name + "expanded", name + "activated"});
    auto partial = layout.output;
    partial.name = name + "partial";
    emit(graph, "MatMul", partial.name, {name + "product", down});
    layout.partials[rank] = {nodesFrom(graph, first), {input}, {partial}};
  }
  emit(graph, "Add", layout.output.name, {layout.partials[0].outputs.front().name,
                                          layout.partials[1].outputs.front().name});
  return layout;
}

static Umt5ReductionLayout
lowerUmt5AttentionImpl(std::int64_t tokens, const Umt5AttentionParameters& parameters,
                   std::uint64_t scoreBytes, const ParameterAccess& weights)
{
  const auto& [query, key, value, output, relativeBias] = parameters;
  for (const auto* weight : {&query, &key, &value, &output, &relativeBias}) {
    if (weight->data_type() != onnx::TensorProto::FLOAT || weight->dims_size() != 2 ||
        weight->dims(0) <= 0 || weight->dims(1) <= 0) {
      throw std::invalid_argument("invalid UMT5 attention initializer");
    }
  }
  const int inputAxis = weights.checkpoint ? 1 : 0;
  const int outputAxis = 1 - inputAxis;
  const auto hidden = query.dims(inputAxis);
  const auto inner = query.dims(outputAxis);
  const auto heads = relativeBias.dims(1);
  const auto buckets = relativeBias.dims(0);
  constexpr std::int64_t MAX_DISTANCE = 128;
  const auto exact = buckets / 4;
  if (tokens <= 0 || tokens > 512 || heads < 2 || inner % heads != 0 ||
      buckets % 2 != 0 || exact < 1 || exact >= MAX_DISTANCE ||
      key.dims(inputAxis) != hidden || key.dims(outputAxis) != inner ||
      value.dims(inputAxis) != hidden || value.dims(outputAxis) != inner ||
      output.dims(inputAxis) != inner || output.dims(outputAxis) != hidden) {
    throw std::invalid_argument("UMT5 attention dimensions disagree");
  }
  const auto width = inner / heads;
  const auto ranks = nativeConvInputChannelRanges(heads, 2);
  const auto largestRank = std::max(ranks[0].second - ranks[0].first,
                                    ranks[1].second - ranks[1].first);
  if (largestRank > scoreBytes / sizeof(float) / tokens / tokens) {
    throw std::invalid_argument("UMT5 attention score plane exceeds its budget");
  }

  Umt5ReductionLayout layout;
  layout.model.set_ir_version(9);
  layout.model.add_opset_import()->set_version(20);
  auto& graph = *layout.model.mutable_graph();
  graph.set_name("wan-umt5-attention");
  const NativeAssemblyTensorContractV3 input{
    "hidden", "float32", {std::int64_t{1}, tokens, hidden}};
  const NativeAssemblyTensorContractV3 mask{"key-mask", "bool", {std::int64_t{1}, tokens}};
  layout.output = {"attention-output", input.dtype, input.shape};
  declare(*graph.add_input(), input, onnx::TensorProto::FLOAT);
  declare(*graph.add_input(), mask, onnx::TensorProto::BOOL);
  declare(*graph.add_output(), layout.output, onnx::TensorProto::FLOAT);

  const auto ints = [&] (const std::string& name, std::initializer_list<std::int64_t> values) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(values.size());
    for (const auto value : values) tensor->add_int64_data(value);
  };
  const auto transpose = [&] (const std::string& name, const std::string& source,
                               std::initializer_list<std::int64_t> permutation) {
    auto* perm = emit(graph, "Transpose", name, {source}).add_attribute();
    perm->set_name("perm");
    perm->set_type(onnx::AttributeProto::INTS);
    for (const auto axis : permutation) perm->add_ints(axis);
  };
  ints("mask-axes", {1, 2});
  auto* excluded = graph.add_initializer();
  excluded->set_name("masked-bias");
  excluded->set_data_type(onnx::TensorProto::FLOAT);
  excluded->add_float_data(std::numeric_limits<float>::lowest());
  auto* positions = graph.add_initializer();
  positions->set_name("relative-buckets");
  positions->set_data_type(onnx::TensorProto::INT64);
  positions->add_dims(tokens);
  positions->add_dims(tokens);
  const auto half = buckets / 2;
  const auto logRange = std::log(static_cast<float>(MAX_DISTANCE) / exact);
  for (std::int64_t q = 0; q < tokens; ++q) {
    for (std::int64_t k = 0; k < tokens; ++k) {
      const auto distance = std::abs(k - q);
      const auto bucket = distance < exact ? distance :
        std::min(half - 1, exact + static_cast<std::int64_t>(
          std::log(static_cast<float>(distance) / exact) / logRange * (half - exact)));
      positions->add_int64_data((k > q ? half : 0) + bucket);
    }
  }

  for (std::size_t rank = 0; rank < ranks.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto [begin, end] = ranks[rank];
    const auto rankHeads = static_cast<std::int64_t>(end - begin);
    const auto parameter = [&] (const char* name, const onnx::TensorProto& source, int axis,
                                 std::uint64_t first, std::uint64_t last) {
      auto* tensor = graph.add_initializer();
      *tensor = weights.slice(source, axis, first, last);
      tensor->set_name(p + name);
      return tensor->name();
    };
    const auto qWeight = parameter("q-weight", query, outputAxis, begin * width, end * width);
    const auto kWeight = parameter("k-weight", key, outputAxis, begin * width, end * width);
    const auto vWeight = parameter("v-weight", value, outputAxis, begin * width, end * width);
    const auto oWeight = parameter("o-weight", output, inputAxis, begin * width, end * width);
    const auto bias = parameter("relative-bias", relativeBias, 1, begin, end);
    ints(p + "head-shape", {1, tokens, rankHeads, width});
    ints(p + "joined-shape", {1, tokens, rankHeads * width});

    const auto first = graph.node_size();
    for (const auto& [name, weight] :
         std::initializer_list<std::pair<const char*, std::string>>{
           {"q", qWeight}, {"k", kWeight}, {"v", vWeight}}) {
      const auto n = p + name;
      emit(graph, "MatMul", n + "/linear", {input.name, matMulWeight(graph, weight, weights)});
      emit(graph, "Reshape", n + "/head-grid", {n + "/linear", p + "head-shape"});
      transpose(n + "/heads", n + "/head-grid",
                name == std::string_view("k") ? std::initializer_list<std::int64_t>{0, 2, 3, 1} :
                                               std::initializer_list<std::int64_t>{0, 2, 1, 3});
    }
    emit(graph, "Gather", p + "bias-grid", {bias, "relative-buckets"});
    transpose(p + "head-bias", p + "bias-grid", {2, 0, 1});
    emit(graph, "Unsqueeze", p + "mask", {mask.name, "mask-axes"});
    emit(graph, "Where", p + "valid-bias", {p + "mask", p + "head-bias", "masked-bias"});
    emit(graph, "MatMul", p + "dot", {p + "q/heads", p + "k/heads"});
    emit(graph, "Add", p + "logits", {p + "dot", p + "valid-bias"});
    emit(graph, "Softmax", p + "probability", {p + "logits"});
    emit(graph, "MatMul", p + "weighted", {p + "probability", p + "v/heads"});
    transpose(p + "token-heads", p + "weighted", {0, 2, 1, 3});
    emit(graph, "Reshape", p + "flattened", {p + "token-heads", p + "joined-shape"});
    auto partial = layout.output;
    partial.name = p + "partial";
    emit(graph, "MatMul", partial.name, {p + "flattened", matMulWeight(graph, oWeight, weights)});
    layout.partials[rank] = {nodesFrom(graph, first), {input, mask}, {partial}};
  }
  emit(graph, "Add", layout.output.name, {layout.partials[0].outputs.front().name,
                                          layout.partials[1].outputs.front().name});
  return layout;
}

static Umt5RmsLayout
lowerUmt5RmsImpl(std::int64_t tokens, const onnx::TensorProto& gain, float epsilon,
             const ParameterAccess& weights)
{
  if (tokens <= 0 || tokens > 512 || gain.data_type() != onnx::TensorProto::FLOAT ||
      gain.dims_size() != 1 || gain.dims(0) < 2 || !std::isfinite(epsilon) || epsilon <= 0) {
    throw std::invalid_argument("invalid UMT5 RMS normalization parameters");
  }
  const auto width = gain.dims(0);
  const auto ranges = nativeConvInputChannelRanges(width, 2);
  Umt5RmsLayout layout;
  layout.model.set_ir_version(9);
  layout.model.add_opset_import()->set_version(20);
  auto& graph = *layout.model.mutable_graph();
  graph.set_name("wan-umt5-rms");
  const NativeAssemblyTensorContractV3 input{
    "hidden", "float32", {std::int64_t{1}, tokens, width}};
  const NativeAssemblyTensorContractV3 output{"normalized", input.dtype, input.shape};
  const NativeAssemblyTensorContractV3 total{
    "square-sum", "float32", {std::int64_t{1}, tokens, std::int64_t{1}}};
  declare(*graph.add_input(), input, onnx::TensorProto::FLOAT);
  declare(*graph.add_output(), output, onnx::TensorProto::FLOAT);
  const auto index = [&] (const std::string& name, std::int64_t n) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(1);
    tensor->add_int64_data(n);
  };
  const auto constant = [&] (const std::string& name, float value) {
    auto* tensor = graph.add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    tensor->add_float_data(value);
  };
  index("axis", -1);
  constant("width", static_cast<float>(width));
  constant("epsilon", epsilon);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto& [begin, end] = ranges[rank];
    index(p + "start", begin);
    index(p + "end", end);
    const auto first = graph.node_size();
    auto local = input;
    local.name = p + "local";
    local.shape.back() = static_cast<std::int64_t>(end - begin);
    auto squares = total;
    squares.name = p + "local-square-sum";
    emit(graph, "Slice", local.name, {input.name, p + "start", p + "end", "axis"});
    emit(graph, "Mul", p + "squares", {local.name, local.name});
    emit(graph, "ReduceSum", squares.name, {p + "squares", "axis"});
    layout.statistics[rank] = {nodesFrom(graph, first), {input}, {local, squares}};
  }
  emit(graph, "Add", total.name, {layout.statistics[0].outputs.at(1).name,
                                   layout.statistics[1].outputs.at(1).name});
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto p = "rank-" + std::to_string(rank) + "/";
    const auto& [begin, end] = ranges[rank];
    const auto& local = layout.statistics[rank].outputs.front();
    auto* slice = graph.add_initializer();
    *slice = weights.slice(gain, 0, begin, end);
    slice->set_name(p + "gain");
    const auto first = graph.node_size();
    emit(graph, "Div", p + "mean", {total.name, "width"});
    emit(graph, "Add", p + "stable", {p + "mean", "epsilon"});
    emit(graph, "Sqrt", p + "rms", {p + "stable"});
    emit(graph, "Div", p + "unit", {local.name, p + "rms"});
    auto normalized = local;
    normalized.name = p + "normalized";
    emit(graph, "Mul", normalized.name, {p + "unit", slice->name()});
    layout.normalize[rank] = {nodesFrom(graph, first), {local, total}, {normalized}};
  }
  const auto first = graph.node_size();
  const auto& left = layout.normalize[0].outputs.front();
  const auto& right = layout.normalize[1].outputs.front();
  auto* axis = emit(graph, "Concat", output.name, {left.name, right.name}).add_attribute();
  axis->set_name("axis");
  axis->set_type(onnx::AttributeProto::INT);
  axis->set_i(-1);
  layout.join = {nodesFrom(graph, first), {left, right}, {output}};
  return layout;
}

Umt5ReductionLayout
lowerUmt5FeedForward(std::int64_t tokens, const onnx::TensorProto& gate,
                     const onnx::TensorProto& expansion, const onnx::TensorProto& contraction,
                     const NativeOnnxExternalReader& external)
{
  return lowerUmt5FeedForwardImpl(tokens, gate, expansion, contraction, {external});
}

Umt5ReductionLayout
lowerUmt5Attention(std::int64_t tokens, const Umt5AttentionParameters& parameters,
                   std::uint64_t scoreBytes, const NativeOnnxExternalReader& external)
{
  return lowerUmt5AttentionImpl(tokens, parameters, scoreBytes, {external});
}

Umt5RmsLayout
lowerUmt5Rms(std::int64_t tokens, const onnx::TensorProto& gain, float epsilon,
             const NativeOnnxExternalReader& external)
{
  return lowerUmt5RmsImpl(tokens, gain, epsilon, {external});
}

Umt5EncoderLayout
lowerWan21Umt5Encoder(const WanCheckpoint& checkpoint, std::int64_t tokens,
                      std::uint64_t scoreBytes)
{
  const auto& parameters = checkpoint.parameters();
  constexpr std::size_t PARAMETERS_PER_LAYER = 10;
  constexpr float EPSILON = 1e-6F;
  if (parameters.size() < 2 + PARAMETERS_PER_LAYER ||
      (parameters.size() - 2) % PARAMETERS_PER_LAYER != 0 || tokens <= 0 || tokens > 512)
    throw std::invalid_argument("invalid Wan UMT5 encoder checkpoint or token slots");
  const auto get = [&] (const std::string& name) -> const onnx::TensorProto& {
    const auto found = parameters.find(name);
    if (found == parameters.end())
      throw std::invalid_argument("Wan UMT5 checkpoint parameter is missing: " + name);
    return found->second;
  };
  const auto& table = get("token_embedding.weight");
  if (table.dims_size() != 2)
    throw std::invalid_argument("Wan UMT5 checkpoint embedding must be a matrix");
  auto embedding = lowerUmt5Embedding(tokens, table.dims(0), table.dims(1),
                                      checkpoint.tensorSource(table.name()));
  const auto sidecar = std::make_shared<WanCheckpointSidecar>(embedding.source.initializerRangeSource);
  const auto external = checkpoint.reader();
  const ParameterAccess access{external, &checkpoint, sidecar.get()};
  const auto makeRms = [&](const std::string& name) {
    return lowerUmt5RmsImpl(tokens, get(name), EPSILON, access);
  };
  std::vector<Umt5EncoderLayer> layers;
  const auto layerCount = (parameters.size() - 2) / PARAMETERS_PER_LAYER;
  layers.reserve(layerCount);
  for (std::size_t layer = 0; layer < layerCount; ++layer) {
    const auto prefix = "blocks." + std::to_string(layer) + ".";
    const Umt5AttentionParameters attention{
      get(prefix + "attn.q.weight"), get(prefix + "attn.k.weight"),
      get(prefix + "attn.v.weight"), get(prefix + "attn.o.weight"),
      get(prefix + "pos_embedding.embedding.weight")};
    layers.push_back({
      makeRms(prefix + "norm1.weight"),
      lowerUmt5AttentionImpl(tokens, attention, scoreBytes, access),
      makeRms(prefix + "norm2.weight"),
      lowerUmt5FeedForwardImpl(tokens, get(prefix + "ffn.gate.0.weight"),
                              get(prefix + "ffn.fc1.weight"), get(prefix + "ffn.fc2.weight"),
                              access)});
  }
  auto finalRms = makeRms("norm.weight");
  embedding.source.initializerRangeSource = sidecar;
  return connectUmt5Encoder(std::move(embedding), std::move(layers), std::move(finalRms));
}

Umt5EncoderLayout
connectUmt5Encoder(Umt5EmbeddingLayout embedding, std::vector<Umt5EncoderLayer> layers,
                   Umt5RmsLayout finalRms)
{
  if (layers.empty()) throw std::invalid_argument("UMT5 encoder expects at least one layer");
  Umt5EncoderLayout encoder;
  onnx::ModelProto model;
  const auto& embeddingBytes = embedding.source.modelBytes;
  if (embeddingBytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !model.ParseFromArray(embeddingBytes.data(), static_cast<int>(embeddingBytes.size())) ||
      !embedding.source.initializerRangeSource || model.graph().input_size() != 1 ||
      model.graph().output_size() != 1) {
    throw std::invalid_argument("invalid UMT5 embedding source");
  }
  encoder.source.initializerRangeSource = std::move(embedding.source.initializerRangeSource);
  encoder.source.externalFileRanges = std::move(embedding.source.externalFileRanges);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-umt5-encoder");
  auto hidden = embedding.join.outputs.at(0);
  if (hidden.shape.size() != 3)
    throw std::invalid_argument("invalid UMT5 embedding output rank");
  const NativeAssemblyTensorContractV3 mask{"key-mask", "bool", {hidden.shape[0], hidden.shape[1]}};
  graph.clear_output();
  declare(*graph.add_input(), mask, onnx::TensorProto::BOOL);
  for (std::size_t rank = 0; rank < 2; ++rank) {
    encoder.entries.emplace("embedding/rank-" + std::to_string(rank),
                            std::move(embedding.ranks[rank]));
  }
  encoder.entries.emplace("embedding/join", std::move(embedding.join));
  encoder.rankOneEntries.insert("embedding/rank-1");

  const auto requireCompatible = [] (const auto& input, const auto& upstream) {
    if (input.dtype != upstream.dtype || input.shape != upstream.shape)
      throw std::invalid_argument("UMT5 encoder tensor contract disagrees: " + input.name);
  };
  const auto remember = [&] (const std::string& id, std::size_t rank,
                             OnnxEntryCut cut, std::uint64_t offset,
                             const std::string& prefix, const auto& bindings) {
    encoder.entries.emplace(id, rebaseCut(std::move(cut), offset, prefix, bindings));
    if (rank == 1) encoder.rankOneEntries.insert(id);
  };
  const auto appendRms = [&] (Umt5RmsLayout rms, const std::string& prefix,
                              const NativeAssemblyTensorContractV3& input) {
    const auto offset = static_cast<std::uint64_t>(graph.node_size());
    const std::map<std::string, std::string> bindings{{"hidden", input.name}};
    const auto sum = rms.statistics[1].nodeIndices.back() + 1;
    encoder.reductions.emplace(offset + sum, OnnxReductionCut{rebaseCut(
      {{}, {rms.statistics[0].outputs.at(1), rms.statistics[1].outputs.at(1)},
       {rms.normalize[0].inputs.at(1)}}, offset, prefix, bindings), true});
    for (std::size_t rank = 0; rank < 2; ++rank) {
      requireCompatible(rms.statistics[rank].inputs.at(0), input);
      const auto id = prefix + "rank-" + std::to_string(rank);
      remember(id + "/statistics", rank, std::move(rms.statistics[rank]), offset, prefix, bindings);
      remember(id + "/normalize", rank, std::move(rms.normalize[rank]), offset, prefix, bindings);
    }
    const auto id = prefix + "join";
    remember(id, 0, std::move(rms.join), offset, prefix, bindings);
    appendFragment(graph, std::move(*rms.model.mutable_graph()), prefix, bindings);
    return encoder.entries.at(id).outputs.at(0);
  };
  const auto appendResidual = [&] (Umt5ReductionLayout part, const std::string& prefix,
                                   const NativeAssemblyTensorContractV3& input,
                                   const NativeAssemblyTensorContractV3& residual) {
    requireCompatible(part.output, residual);
    const auto offset = static_cast<std::uint64_t>(graph.node_size());
    const std::map<std::string, std::string> bindings{
      {"hidden", input.name}, {mask.name, mask.name}};
    const auto sum = part.partials[1].nodeIndices.back() + 1;
    encoder.reductions.emplace(offset + sum, OnnxReductionCut{rebaseCut(
      {{}, {part.partials[0].outputs.at(0), part.partials[1].outputs.at(0)}, {part.output}},
      offset, prefix, bindings), false});
    for (std::size_t rank = 0; rank < 2; ++rank) {
      const auto& inputs = part.partials[rank].inputs;
      requireCompatible(inputs.at(0), input);
      if (inputs.size() == 2) requireCompatible(inputs[1], mask);
      remember(prefix + "rank-" + std::to_string(rank), rank, std::move(part.partials[rank]),
                offset, prefix, bindings);
    }
    appendFragment(graph, std::move(*part.model.mutable_graph()), prefix, bindings);
    auto reduced = part.output;
    reduced.name = prefix + reduced.name;
    auto output = residual;
    output.name = prefix + "residual";
    const auto start = graph.node_size();
    emit(graph, "Add", output.name, {residual.name, reduced.name});
    encoder.entries.emplace(output.name, OnnxEntryCut{
      nodesFrom(graph, start), {residual, reduced}, {output}});
    return output;
  };
  for (std::size_t layer = 0; layer < layers.size(); ++layer) {
    const auto p = "layer-" + std::to_string(layer) + "/";
    auto& parts = layers[layer];
    const auto attentionInput = appendRms(std::move(parts.preAttention), p + "norm1/", hidden);
    hidden = appendResidual(std::move(parts.attention), p + "attention/", attentionInput, hidden);
    const auto ffnInput = appendRms(std::move(parts.preFfn), p + "norm2/", hidden);
    hidden = appendResidual(std::move(parts.ffn), p + "ffn/", ffnInput, hidden);
    encoder.layerEnds.push_back(static_cast<std::size_t>(graph.node_size()));
  }
  hidden = appendRms(std::move(finalRms), "final-norm/", hidden);

  const auto trimStart = graph.node_size();
  for (const auto& [name, value] :
       std::initializer_list<std::pair<const char*, std::int64_t>>{
         {"trim-axis", 1}, {"trim-start", 0}}) {
    auto* index = graph.add_initializer();
    index->set_name(name);
    index->set_data_type(onnx::TensorProto::INT64);
    index->add_dims(1);
    index->add_int64_data(value);
  }
  auto* cast = emit(graph, "Cast", "mask-integer", {mask.name}).add_attribute();
  cast->set_name("to");
  cast->set_type(onnx::AttributeProto::INT);
  cast->set_i(onnx::TensorProto::INT64);
  auto* keep = emit(graph, "ReduceSum", "valid-tokens", {"mask-integer", "trim-axis"})
                 .add_attribute();
  keep->set_name("keepdims");
  keep->set_type(onnx::AttributeProto::INT);
  keep->set_i(0);
  const NativeAssemblyTensorContractV3 context{
    "text-context", "float32", {hidden.shape[0], std::string("valid_tokens"), hidden.shape[2]}};
  emit(graph, "Slice", context.name, {hidden.name, "trim-start", "valid-tokens", "trim-axis"});
  declare(*graph.add_output(), context, onnx::TensorProto::FLOAT);
  encoder.entries.emplace("trim", OnnxEntryCut{
    nodesFrom(graph, trimStart), {hidden, mask}, {context}});

  const auto wire = model.SerializeAsString();
  encoder.source.modelBytes.assign(wire.begin(), wire.end());
  return encoder;
}

OnnxParallelPlan
makeUmt5TensorParallelPlan(const Umt5EncoderLayout& layout, const NativeStageLayout& stage)
{
  onnx::ModelProto model;
  const auto& bytes = layout.source.modelBytes;
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) || !model.has_graph())
    throw std::invalid_argument("invalid UMT5 encoder canonical source");
  return compileParallelEntries(model.graph(), stage, layout.entries,
                                layout.rankOneEntries, layout.reductions);
}

OnnxParallelPlan
makeUmt5PipelinePlan(const Umt5EncoderLayout& layout, std::size_t prefixLayers,
                     const std::array<NativeStageLayout, 2>& stages)
{
  if (prefixLayers == 0 || prefixLayers >= layout.layerEnds.size() ||
      stages[0].degree() != 1 || stages[1].degree() != 1)
    throw std::invalid_argument("Wan UMT5 PP needs two single-unit stages and a layer boundary");
  onnx::ModelProto model;
  const auto& bytes = layout.source.modelBytes;
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !model.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) || !model.has_graph())
    throw std::invalid_argument("invalid UMT5 encoder canonical source");
  const NativeStageLayout unplaced{"encoder", {}, {"rank0", "rank1"}};
  const auto ranks = compileParallelEntries(model.graph(), unplaced, layout.entries,
                                            layout.rankOneEntries, layout.reductions);
  return makeContiguousEntryPlan(model.graph(), ranks, stages, layout.layerEnds[prefixLayers - 1]);
}

} // namespace ndnsf::di::wan
