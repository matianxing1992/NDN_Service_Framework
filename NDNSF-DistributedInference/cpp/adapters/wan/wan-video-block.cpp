#include "wan-video-block.hpp"

#include "wan-entry-plan.hpp"
#include "wan-graph-fragment.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/initializer-slice.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

std::string
rankOneJoinName(const std::string& name)
{
  return name + "-rank-1-join";
}

void
bindEntryInputs(onnx::GraphProto& graph, OnnxEntryCut& cut,
                const std::map<std::string, std::string>& bindings)
{
  for (auto& input : cut.inputs) {
    const auto replacement = bindings.find(input.name);
    if (replacement == bindings.end()) continue;
    bool used = false;
    for (const auto index : cut.nodeIndices) {
      for (auto& value : *graph.mutable_node(static_cast<int>(index))->mutable_input()) {
        if (value == input.name) {
          value = replacement->second;
          used = true;
        }
      }
    }
    if (!used)
      throw std::invalid_argument("Wan join has no explicit consumer: " + input.name);
    input.name = replacement->second;
  }
}

onnx::TensorProto
sliceOutputBias(const onnx::TensorProto& bias, std::uint64_t begin, std::uint64_t end)
{
  if (bias.dims_size() != 1 || bias.dims(0) <= 0 ||
      bias.data_type() != onnx::TensorProto::FLOAT)
    throw std::invalid_argument("invalid Wan gated projection bias");
  if (bias.data_location() != onnx::TensorProto::EXTERNAL)
    return sliceNativeOnnxFloatInitializer(bias, 0, begin, end, {});
  const InitializerSlice slice({static_cast<std::uint64_t>(bias.dims(0))}, 0,
                               begin, end, sizeof(float));
  auto result = bias;
  result.set_dims(0, static_cast<std::int64_t>(end - begin));
  bool offsetSeen = false;
  bool lengthSeen = false;
  for (auto& field : *result.mutable_external_data()) {
    if (field.key() != "offset" && field.key() != "length") continue;
    const auto& value = field.value();
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      throw std::invalid_argument("invalid Wan bias sidecar coordinate");
    if (field.key() == "offset") {
      if (offsetSeen || number > std::numeric_limits<std::uint64_t>::max() - slice.sourceBytes())
        throw std::invalid_argument("invalid Wan bias sidecar offset");
      offsetSeen = true;
      field.set_value(std::to_string(number + slice.sourceOffset(0)));
    }
    else {
      if (lengthSeen || number != slice.sourceBytes())
        throw std::invalid_argument("invalid Wan bias sidecar length");
      lengthSeen = true;
      field.set_value(std::to_string(slice.sliceBytes()));
    }
  }
  if (!offsetSeen || !lengthSeen)
    throw std::invalid_argument("Wan bias requires a bounded sidecar range");
  return result;
}

/** Keep partial addition and bias in their original order for each output slice. */
std::array<NativeAssemblyTensorContractV3, 2>
splitGatedProjection(onnx::GraphProto& graph, const std::array<OnnxEntryCut*, 2>& partials,
                    const OnnxEntryCut& finish, VideoBlockStage& residual,
                    google::protobuf::RepeatedPtrField<onnx::NodeProto>& conditioningNodes)
{
  using Port = NativeAssemblyTensorContractV3;
  const auto& full = finish.outputs.at(0);
  const auto biasIndex = finish.nodeIndices.at(0);
  if (full.dtype != "float32" || full.shape.size() != 3 || finish.nodeIndices.size() != 1 ||
      biasIndex == 0 || biasIndex + 1 != static_cast<std::uint64_t>(graph.node_size()))
    throw std::invalid_argument("invalid Wan gated projection boundary");
  const auto& biased = graph.node(static_cast<int>(biasIndex));
  const auto& sum = graph.node(static_cast<int>(biasIndex - 1));
  const auto isBinaryAdd = [](const auto& node) {
    return node.domain().empty() && node.op_type() == "Add" &&
           node.input_size() == 2 && node.output_size() == 1;
  };
  if (!isBinaryAdd(biased) || !isBinaryAdd(sum) || biased.input(0) != sum.output(0) ||
      biased.output(0) != full.name || sum.input(0) != partials[0]->outputs.at(0).name ||
      sum.input(1) != partials[1]->outputs.at(0).name)
    throw std::invalid_argument("Wan gated projection needs its canonical SUM and bias");
  auto biases = graph.mutable_initializer();
  const auto bias = std::find_if(biases->begin(), biases->end(),
    [&](const auto& tensor) { return tensor.name() == biased.input(1); });
  if (bias == biases->end() || bias->dims_size() != 1 ||
      bias->dims(0) != std::get<std::int64_t>(full.shape.back()))
    throw std::invalid_argument("Wan gated projection has no matching bias");
  const auto fullBias = *bias;
  biases->erase(bias);
  graph.mutable_node()->DeleteSubrange(static_cast<int>(biasIndex - 1), 2);

  const std::array widths{
    std::get<std::int64_t>(residual.ranks[0].outputs.at(0).shape.back()),
    std::get<std::int64_t>(residual.ranks[1].outputs.at(0).shape.back())};
  if (widths[0] <= 0 || widths[1] <= 0 || widths[0] >= fullBias.dims(0) ||
      widths[1] != fullBias.dims(0) - widths[0])
    throw std::invalid_argument("Wan gated projection channels must cover the original bias");
  const auto widthsName = full.name + "/channel-widths";
  auto* widthsTensor = graph.add_initializer();
  widthsTensor->set_name(widthsName);
  widthsTensor->set_data_type(onnx::TensorProto::INT64);
  widthsTensor->add_dims(2);
  for (const auto width : widths) widthsTensor->add_int64_data(width);
  const auto emit = [&] (const char* op, const std::string& output,
                         const std::string& lhs, const std::string& rhs) {
    auto* node = graph.add_node();
    node->set_name(output);
    node->set_op_type(op);
    node->add_input(lhs);
    node->add_input(rhs);
    node->add_output(output);
    return node;
  };
  for (auto* entry : partials) {
    const auto unsplit = entry->outputs.at(0);
    if (entry->outputs.size() != 1 || unsplit.shape != full.shape || unsplit.dtype != full.dtype)
      throw std::invalid_argument("Wan gated projection partials must have the original layout");
    entry->outputs.clear();
    for (std::size_t rank = 0; rank < 2; ++rank) {
      auto port = unsplit;
      port.name += "/channel-" + std::to_string(rank);
      port.shape.back() = widths[rank];
      entry->outputs.push_back(std::move(port));
    }
    entry->nodeIndices.push_back(static_cast<std::uint64_t>(graph.node_size()));
    auto* split = emit("Split", entry->outputs[0].name, unsplit.name, widthsName);
    split->add_output(entry->outputs[1].name);
    auto* axis = split->add_attribute();
    axis->set_name("axis");
    axis->set_type(onnx::AttributeProto::INT);
    axis->set_i(2);
  }
  std::array<Port, 2> sums;
  std::uint64_t begin = 0;
  for (std::size_t rank = 0; rank < 2; ++rank) {
    auto& gate = residual.ranks[rank];
    auto input = std::find_if(gate.inputs.begin(), gate.inputs.end(),
      [&](const auto& port) { return port.name == full.name; });
    if (input == gate.inputs.end())
      throw std::invalid_argument("Wan residual is missing its gated projection");
    onnx::NodeProto* slice = nullptr;
    for (const auto index : gate.nodeIndices) {
      auto* node = conditioningNodes.Mutable(static_cast<int>(index));
      if (std::find(node->input().begin(), node->input().end(), full.name) == node->input().end())
        continue;
      if (slice != nullptr || !node->domain().empty() || node->op_type() != "Slice" ||
          node->input(0) != full.name || node->output_size() != 1)
        throw std::invalid_argument("Wan residual must exclusively slice its projected branch");
      slice = node;
    }
    if (slice == nullptr)
      throw std::invalid_argument("Wan residual branch has no Slice");
    const auto end = begin + static_cast<std::uint64_t>(widths[rank]);
    const auto checkIndex = [&](int inputIndex, std::int64_t expected) {
      const auto found = std::find_if(biases->begin(), biases->end(), [&](const auto& tensor) {
        return tensor.name() == slice->input(inputIndex);
      });
      return found != biases->end() && found->data_type() == onnx::TensorProto::INT64 &&
             found->dims_size() == 1 && found->dims(0) == 1 &&
             found->int64_data_size() == 1 && found->int64_data(0) == expected;
    };
    if (slice->input_size() != 4 || !checkIndex(1, begin) ||
        !checkIndex(2, end) || !checkIndex(3, 2))
      throw std::invalid_argument("Wan residual uses different projection channel bounds");
    // The gate consumes its own unbiased SUM and adds only its own bias.
    const auto oldOutput = slice->output(0);
    auto& unbiased = sums[rank];
    unbiased = gate.outputs.at(0);
    unbiased.name = full.name + "/channel-" + std::to_string(rank) + "/sum";
    emit("Add", unbiased.name, partials[0]->outputs[rank].name, partials[1]->outputs[rank].name);
    auto biasSlice = sliceOutputBias(fullBias, begin, end);
    biasSlice.set_name("rank-" + std::to_string(rank) + "/" + fullBias.name());
    const auto biasName = biasSlice.name();
    *graph.add_initializer() = std::move(biasSlice);
    slice->Clear();
    slice->set_name(oldOutput);
    slice->set_op_type("Add");
    slice->add_input(unbiased.name);
    slice->add_input(biasName);
    slice->add_output(oldOutput);
    *input = unbiased;
    begin = end;
  }
  return sums;
}

} // namespace

VideoBlockLayout
lowerVideoBlock(std::int64_t tokens, const onnx::TensorProto& modulation,
                const onnx::TensorProto& crossScale, const onnx::TensorProto& crossBias,
                float epsilon, const WanInitializerAccess& material)
{
  if (tokens <= 0 || !std::isfinite(epsilon) || epsilon <= 0 ||
      modulation.data_type() != onnx::TensorProto::FLOAT || modulation.dims_size() != 3 ||
      modulation.dims(0) != 1 || modulation.dims(1) != 6 || modulation.dims(2) < 2) {
    throw std::invalid_argument("invalid Wan block modulation or tile");
  }
  const auto hidden = modulation.dims(2);
  for (const auto* parameter : {&crossScale, &crossBias}) {
    if (parameter->data_type() != onnx::TensorProto::FLOAT ||
        parameter->dims_size() != 1 || parameter->dims(0) != hidden) {
      throw std::invalid_argument("Wan cross-attention affine width differs from modulation");
    }
  }
  if (static_cast<std::uint64_t>(hidden) > NativeCanonicalMaterialBundleMaxBytes / sizeof(float) ||
      tokens > std::numeric_limits<std::int64_t>::max() / hidden /
               static_cast<std::int64_t>(sizeof(float))) {
    throw std::invalid_argument("Wan conditioning exceeds tensor arithmetic bounds");
  }

  VideoBlockLayout layout;
  layout.model.set_ir_version(9);
  layout.model.add_opset_import()->set_version(18);
  auto* graph = layout.model.mutable_graph();
  graph->set_name("wan-video-block");
  const auto activation = [&] (std::string name, std::int64_t width) {
    return NativeAssemblyTensorContractV3{std::move(name), "float32",
                                          {std::int64_t{1}, tokens, width}};
  };
  const auto declare = [] (onnx::ValueInfoProto& value, const NativeAssemblyTensorContractV3& port) {
    value.set_name(port.name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto& dim : port.shape) {
      tensor->mutable_shape()->add_dim()->set_dim_value(std::get<std::int64_t>(dim));
    }
  };
  const auto emit = [&] (const char* op, const std::string& output,
                         std::initializer_list<std::string> inputs) {
    auto* node = graph->add_node();
    node->set_op_type(op);
    node->set_name(output);
    node->add_output(output);
    for (const auto& input : inputs) node->add_input(input);
    return node;
  };
  const auto axis = [] (onnx::NodeProto& node, std::int64_t dim) {
    auto* attr = node.add_attribute();
    attr->set_name("axis");
    attr->set_type(onnx::AttributeProto::INT);
    attr->set_i(dim);
  };
  const auto index = [&] (const std::string& name, std::int64_t value) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::INT64);
    tensor->add_dims(1);
    tensor->add_int64_data(value);
  };
  const auto ones = [&] (const char* name, std::int64_t count) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    tensor->add_dims(count);
    tensor->mutable_float_data()->Resize(static_cast<int>(count), 1.0F);
  };
  const auto cut = [&] (int begin, std::vector<NativeAssemblyTensorContractV3> inputs,
                         NativeAssemblyTensorContractV3 output) {
    OnnxEntryCut entry{{}, std::move(inputs), {std::move(output)}};
    for (int n = begin; n < graph->node_size(); ++n) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(n));
    }
    return entry;
  };

  index("hidden-axis", 2);
  for (int group = 0; group < 6; ++group) index("group-" + std::to_string(group), group);
  ones("one", 1);
  ones("norm-scale", hidden);
  const auto ranges = nativeConvInputChannelRanges(hidden, 2);
  const std::array<std::string, 2> rankPrefix{"rank-0/", "rank-1/"};
  const std::array<std::string, 2> channelPrefix{"channels-0/", "channels-1/"};
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto& prefix = rankPrefix[rank];
    const auto [begin, end] = ranges[rank];
    index(channelPrefix[rank] + "begin", begin);
    index(channelPrefix[rank] + "end", end);
    const auto addParameter = [&] (const char* suffix, const onnx::TensorProto& source,
                                   std::size_t axisIndex) {
      auto* tensor = graph->add_initializer();
      *tensor = material.slice(source, axisIndex, begin, end);
      tensor->set_name(prefix + suffix);
    };
    addParameter("learned-modulation", modulation, 2);
    addParameter("cross-scale", crossScale, 0);
    addParameter("cross-bias", crossBias, 0);
    NativeAssemblyTensorContractV3 time{"modulation-rank-" + std::to_string(rank), "float32",
      {std::int64_t{1}, std::int64_t{6}, static_cast<std::int64_t>(end - begin)}};
    declare(*graph->add_input(), time);
    auto conditioned = time;
    conditioned.name = prefix + "conditioning";
    const auto node = graph->node_size();
    emit("Add", conditioned.name, {time.name, prefix + "learned-modulation"});
    layout.modulation[rank] = cut(node, {time}, conditioned);
  }
  const auto slice = [&] (const std::string& name, const std::string& input, std::size_t rank) {
    const auto& prefix = channelPrefix[rank];
    emit("Slice", name, {input, prefix + "begin", prefix + "end", "hidden-axis"});
  };
  const auto normalized = [&] (const std::string& name,
                                const std::string& input, std::size_t rank) {
    auto* norm = emit("LayerNormalization", name + "/norm", {input, "norm-scale"});
    auto* eps = norm->add_attribute();
    eps->set_name("epsilon");
    eps->set_type(onnx::AttributeProto::FLOAT);
    eps->set_f(epsilon);
    slice(name + "/norm-local", name + "/norm", rank);
  };
  const auto condition = [&] (const std::string& name, std::size_t rank, int group) {
    const auto& conditioned = layout.modulation[rank].outputs.front();
    axis(*emit("Gather", name, {conditioned.name, "group-" + std::to_string(group)}), 1);
  };
  const auto sharded = [&] (const std::string& name, auto write) {
    VideoBlockStage stage;
    for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
      const auto [begin, end] = ranges[rank];
      const auto out = activation(rankPrefix[rank] + name, end - begin);
      const auto first = graph->node_size();
      stage.ranks[rank] = cut(first, write(out.name, rank), out);
    }
    const auto first = graph->node_size();
    const auto out = activation(name, hidden);
    const auto& left = stage.ranks[0].outputs.front();
    const auto& right = stage.ranks[1].outputs.front();
    axis(*emit("Concat", out.name, {left.name, right.name}), 2);
    stage.join = cut(first, {left, right}, out);
    declare(*graph->add_output(), out);
    return stage;
  };
  const auto modulatedInput = [&] (const char* name, const auto& input, int shift, int scale) {
    return sharded(name, [&] (const std::string& out, std::size_t rank) {
      normalized(out, input.name, rank);
      condition(out + "/scale", rank, scale);
      condition(out + "/shift", rank, shift);
      emit("Add", out + "/gain", {out + "/scale", "one"});
      emit("Mul", out + "/scaled", {out + "/norm-local", out + "/gain"});
      emit("Add", out, {out + "/scaled", out + "/shift"});
      return std::vector<NativeAssemblyTensorContractV3>{
        input, layout.modulation[rank].outputs.front()};
    });
  };
  const auto gatedResidual = [&] (const char* name, const auto& input, const auto& result, int gate) {
    return sharded(name, [&] (const std::string& out, std::size_t rank) {
      slice(out + "/residual", input.name, rank);
      slice(out + "/branch", result.name, rank);
      condition(out + "/gate", rank, gate);
      emit("Mul", out + "/scaled", {out + "/branch", out + "/gate"});
      emit("Add", out, {out + "/residual", out + "/scaled"});
      return std::vector<NativeAssemblyTensorContractV3>{
        input, result, layout.modulation[rank].outputs.front()};
    });
  };

  const auto hiddenInput = activation("hidden", hidden);
  const auto selfResult = activation("self-result", hidden);
  const auto crossResult = activation("cross-result", hidden);
  const auto ffnResult = activation("ffn-result", hidden);
  for (const auto* input : {&hiddenInput, &selfResult, &crossResult, &ffnResult}) {
    declare(*graph->add_input(), *input);
  }
  layout.selfInput = modulatedInput("self-input", hiddenInput, 0, 1);
  layout.selfResidual = gatedResidual("after-self", hiddenInput, selfResult, 2);
  const auto& afterSelf = layout.selfResidual.join.outputs.front();
  layout.crossInput = sharded("cross-input", [&] (const std::string& out, std::size_t rank) {
    normalized(out, afterSelf.name, rank);
    emit("Mul", out + "/scaled", {out + "/norm-local", rankPrefix[rank] + "cross-scale"});
    emit("Add", out, {out + "/scaled", rankPrefix[rank] + "cross-bias"});
    return std::vector<NativeAssemblyTensorContractV3>{afterSelf};
  });
  const auto afterCross = activation("after-cross", hidden);
  const auto node = graph->node_size();
  emit("Add", afterCross.name, {afterSelf.name, crossResult.name});
  layout.crossResidual = cut(node, {afterSelf, crossResult}, afterCross);
  declare(*graph->add_output(), afterCross);
  layout.ffnInput = modulatedInput("ffn-input", afterCross, 3, 4);
  layout.ffnResidual = gatedResidual("block-output", afterCross, ffnResult, 5);
  return layout;
}

ConnectedVideoBlock
connectVideoBlock(VideoBlockLayout conditioning, VideoBlockAttention self,
                  VideoBlockAttention cross, SummedProjection ffn,
                  const NativeStageLayout& stage)
{
  stage.validate();
  if (stage.degree() != 2)
    throw std::invalid_argument("Wan block needs two distinct rank role identities");
  ConnectedVideoBlock connected;
  auto& model = connected.model;
  model = std::move(conditioning.model);
  auto* graph = model.mutable_graph();
  const auto requirePort = [] (const auto& ports, const std::string& name)
      -> const onnx::ValueInfoProto& {
    const auto found = std::find_if(ports.begin(), ports.end(),
                                    [&] (const auto& port) { return port.name() == name; });
    if (found == ports.end())
      throw std::invalid_argument("Wan block fragment has no port " + name);
    return *found;
  };
  const auto requireSameType = [] (const auto& input, const auto& output) {
    if (input.type().SerializeAsString() != output.type().SerializeAsString())
      throw std::invalid_argument("Wan block port type mismatch: " + input.name());
  };
  const auto addExternalInput = [&] (const onnx::ValueInfoProto& port, const std::string& name) {
    auto* added = graph->add_input();
    *added = port;
    added->set_name(name);
  };
  for (const auto* fragment : {&model, &self.context.model, &self.query.model,
                                &cross.context.model, &cross.query.model, &ffn.model}) {
    if (fragment->opset_import_size() != 1 ||
        !fragment->opset_import(0).domain().empty() ||
        (fragment->opset_import(0).version() != 18 &&
         fragment->opset_import(0).version() != 20)) {
      throw std::invalid_argument("Wan block requires compatible canonical opsets 18 or 20");
    }
    model.set_ir_version(std::max(model.ir_version(), fragment->ir_version()));
    model.mutable_opset_import(0)->set_version(
      std::max(model.opset_import(0).version(), fragment->opset_import(0).version()));
  }
  const auto& conditionerOut = graph->output();
  const auto appendAttention = [&] (VideoBlockAttention& attention, const std::string& prefix,
                                    const std::string& input, const std::string& output,
                                    bool video) {
    auto* ctx = attention.context.model.mutable_graph();
    auto* query = attention.query.model.mutable_graph();
    if (ctx->input_size() != 1 || ctx->output_size() != 4 ||
        query->input_size() != (video ? 6 : 5) || query->output_size() != 1) {
      throw std::invalid_argument("invalid Wan block attention fragment boundary");
    }
    const auto& hidden = requirePort(conditionerOut, input);
    requireSameType(requirePort(query->input(), "hidden"), hidden);
    requireSameType(requirePort(graph->input(), output), requirePort(query->output(), "output"));
    if (video) {
      requireSameType(ctx->input(0), hidden);
      addExternalInput(requirePort(query->input(), "valid"), "video-valid");
    }
    else {
      addExternalInput(ctx->input(0), "text-context");
    }
    std::map<std::string, std::string> contextBinding{
      {ctx->input(0).name(), video ? input : "text-context"}};
    std::map<std::string, std::string> queryBinding{{"hidden", input}, {"output", output}};
    if (video) queryBinding.emplace("valid", "video-valid");
    for (const auto* name : {"key-rank-0", "value-rank-0", "key-rank-1", "value-rank-1"}) {
      const auto& provided = requirePort(ctx->output(), name);
      requireSameType(requirePort(query->input(), name), provided);
      contextBinding.emplace(name, prefix + name);
      queryBinding.emplace(name, prefix + name);
    }
    const auto contextPrefix = prefix + "context/";
    const auto contextOffset = static_cast<std::uint64_t>(graph->node_size());
    for (auto& rank : attention.context.ranks) {
      rank.project = rebaseCut(std::move(rank.project), contextOffset,
                                contextPrefix, contextBinding);
      rank.prepare = rebaseCut(std::move(rank.prepare), contextOffset,
                                contextPrefix, contextBinding);
    }
    appendFragment(*graph, std::move(*ctx), contextPrefix, contextBinding);
    const auto queryPrefix = prefix + "query/";
    const auto queryOffset = static_cast<std::uint64_t>(graph->node_size());
    for (auto& rank : attention.query.ranks) {
      rank.project = rebaseCut(std::move(rank.project), queryOffset, queryPrefix, queryBinding);
      rank.attend = rebaseCut(std::move(rank.attend), queryOffset, queryPrefix, queryBinding);
    }
    attention.query.finish = rebaseCut(std::move(attention.query.finish), queryOffset,
                                       queryPrefix, queryBinding);
    appendFragment(*graph, std::move(*query), queryPrefix, queryBinding);
  };

  const auto& ffnGraph = ffn.model.graph();
  if (ffnGraph.input_size() != 1 || ffnGraph.output_size() != 1)
    throw std::invalid_argument("invalid Wan block FFN fragment boundary");
  requireSameType(ffnGraph.input(0), requirePort(conditionerOut, "ffn-input"));
  requireSameType(ffnGraph.output(0), requirePort(graph->input(), "ffn-result"));
  const std::map<std::string, std::string> ffnBindings{
    {ffnGraph.input(0).name(), "ffn-input"}, {ffnGraph.output(0).name(), "ffn-result"}};

  const auto selfAt = conditioning.selfResidual.ranks[0].nodeIndices.at(0);
  const auto crossAt = conditioning.crossResidual.nodeIndices.at(0);
  const auto ffnAt = conditioning.ffnResidual.ranks[0].nodeIndices.at(0);
  std::map<std::uint64_t, OnnxEntryCut> rankOneJoins;
  std::map<std::string, std::string> rankOneBindings;
  for (const auto* region : {&conditioning.selfInput, &conditioning.selfResidual,
                             &conditioning.crossInput, &conditioning.ffnInput,
                             &conditioning.ffnResidual}) {
    auto cut = region->join;
    const auto oldIndex = cut.nodeIndices.at(0);
    auto& port = cut.outputs.at(0);
    auto localName = rankOneJoinName(port.name);
    rankOneBindings.emplace(port.name, localName);
    port.name = std::move(localName);
    rankOneJoins.emplace(oldIndex, std::move(cut));
  }
  auto originalNodes = std::move(*graph->mutable_node());
  graph->clear_node();
  std::vector<std::uint64_t> remap(originalNodes.size());
  std::array<NativeAssemblyTensorContractV3, 2> selfSums, ffnSums;
  for (int i = 0; i < originalNodes.size(); ++i) {
    const auto index = static_cast<std::uint64_t>(i);
    if (index == selfAt) {
      appendAttention(self, "self/", "self-input", "self-result", true);
      selfSums = splitGatedProjection(*graph,
        {&self.query.ranks[0].attend, &self.query.ranks[1].attend},
        self.query.finish, conditioning.selfResidual, originalNodes);
    }
    if (index == crossAt)
      appendAttention(cross, "cross/", "cross-input", "cross-result", false);
    if (index == ffnAt) {
      const auto offset = static_cast<std::uint64_t>(graph->node_size());
      for (auto& rank : ffn.partials)
        rank = rebaseCut(std::move(rank), offset, "ffn/", ffnBindings);
      ffn.addBias = rebaseCut(std::move(ffn.addBias), offset, "ffn/", ffnBindings);
      appendFragment(*graph, std::move(*ffn.model.mutable_graph()), "ffn/", ffnBindings);
      ffnSums = splitGatedProjection(*graph, {&ffn.partials[0], &ffn.partials[1]},
                                    ffn.addBias, conditioning.ffnResidual, originalNodes);
    }
    remap[i] = static_cast<std::uint64_t>(graph->node_size());
    *graph->add_node() = std::move(*originalNodes.Mutable(i));
    const auto join = rankOneJoins.find(index);
    if (join != rankOneJoins.end()) {
      auto local = graph->node(static_cast<int>(remap[i]));
      const auto& output = join->second.outputs.front().name;
      local.set_name(output);
      local.set_output(0, output);
      join->second.nodeIndices.front() = static_cast<std::uint64_t>(graph->node_size());
      *graph->add_node() = std::move(local);
    }
  }
  for (int i = graph->input_size() - 1; i >= 0; --i) {
    const auto& name = graph->input(i).name();
    if (name == "self-result" || name == "cross-result" || name == "ffn-result")
      graph->mutable_input()->DeleteSubrange(i, 1);
  }
  onnx::ValueInfoProto output = requirePort(conditionerOut, "block-output");
  graph->clear_output();
  *graph->add_output() = std::move(output);

  EntryPlanBuilder planner(*graph, stage, connected.tensorParallel, connected.entries);
  const auto call = [&] (const std::string& label, std::size_t rank, OnnxEntryCut cut) {
    if (rank == 1) bindEntryInputs(*graph, cut, rankOneBindings);
    planner.call(label, rank, cut);
  };
  const auto conditionStep = [&] (const std::string& label, std::size_t rank, OnnxEntryCut cut) {
    for (auto& node : cut.nodeIndices) node = remap.at(node);
    call(label, rank, std::move(cut));
  };
  const auto conditionStage = [&] (const std::string& label, const VideoBlockStage& stage) {
    for (std::size_t rank = 0; rank < 2; ++rank) conditionStep(label, rank, stage.ranks[rank]);
    conditionStep(label + "/join", 0, stage.join);
    planner.call(label + "/join", 1, rankOneJoins.at(stage.join.nodeIndices.at(0)));
  };
  const auto attention = [&] (const std::string& label, const VideoBlockAttention& parts) {
    const auto& ctx = parts.context.ranks;
    const auto& qry = parts.query.ranks;
    for (std::size_t rank = 0; rank < 2; ++rank) {
      call(label + "/kv-project", rank, ctx[rank].project);
      call(label + "/q-project", rank, qry[rank].project);
    }
    for (std::size_t rank = 0; rank < 2; ++rank) {
      planner.sum(label + "/k-sum", rank, ctx[0].project.outputs.at(1), ctx[1].project.outputs.at(1),
          ctx[rank].prepare.inputs.at(1));
      planner.sum(label + "/q-sum", rank, qry[0].project.outputs.at(1), qry[1].project.outputs.at(1),
          qry[rank].attend.inputs.at(1));
      call(label + "/kv-prepare", rank, ctx[rank].prepare);
      call(label + "/attend", rank, qry[rank].attend);
    }
  };
  const auto sumGated = [&](const std::string& label, const OnnxEntryCut& left,
                            const OnnxEntryCut& right,
                            const std::array<NativeAssemblyTensorContractV3, 2>& sums) {
    for (std::size_t rank = 0; rank < 2; ++rank) {
      planner.sum(label + "/sum", rank, left.outputs.at(rank), right.outputs.at(rank), sums[rank]);
    }
  };

  for (std::size_t rank = 0; rank < 2; ++rank)
    conditionStep("modulation", rank, conditioning.modulation[rank]);
  conditionStage("self/input", conditioning.selfInput);
  attention("self", self);
  sumGated("self", self.query.ranks[0].attend, self.query.ranks[1].attend, selfSums);
  conditionStage("self/residual", conditioning.selfResidual);
  conditionStage("cross/input", conditioning.crossInput);
  attention("cross", cross);
  planner.sum("cross/output-sum", 0, cross.query.ranks[0].attend.outputs.front(),
              cross.query.ranks[1].attend.outputs.front(), cross.query.finish.inputs.front());
  planner.call("cross/output", 0, cross.query.finish);
  conditionStep("cross/residual", 0, conditioning.crossResidual);
  conditionStage("ffn/input", conditioning.ffnInput);
  for (std::size_t rank = 0; rank < 2; ++rank)
    call("ffn/project", rank, ffn.partials[rank]);
  sumGated("ffn", ffn.partials[0], ffn.partials[1], ffnSums);
  conditionStage("ffn/residual", conditioning.ffnResidual);
  connected.tensorParallel.validateSteps();
  return connected;
}

VideoDenoiserLayout
connectVideoDenoiser(SummedProjection patch, SummedProjection text, TimeEmbeddingLayout time,
                     std::vector<ConnectedVideoBlock> blocks, SummedProjection head,
                     const VideoHeadGrid& grid, const NativeStageLayout& stage)
{
  using Port = NativeAssemblyTensorContractV3;
  using Bindings = std::map<std::string, std::string>;
  constexpr auto LIMIT = std::numeric_limits<std::int64_t>::max();
  if (blocks.empty() || grid.frames <= 0 || grid.height <= 0 || grid.width <= 0 ||
      grid.frames > LIMIT / grid.height / grid.width) {
    throw std::invalid_argument("invalid Wan denoiser grid or missing blocks");
  }
  const auto validTokens = grid.frames * grid.height * grid.width;
  if (grid.paddedTokens < validTokens)
    throw std::invalid_argument("Wan denoiser padded tokens omit video patches");

  VideoDenoiserLayout result;
  auto& model = result.model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("wan-video-denoiser");
  EntryPlanBuilder plan(graph, stage, result.tensorParallel.plan, result.tensorParallel.entries);
  const auto compatible = [] (const Port& source, const Port& target) {
    if (source.dtype != target.dtype || source.shape != target.shape)
      throw std::invalid_argument("Wan denoiser port contract disagrees: " + target.name);
  };
  const auto declare = [] (onnx::ValueInfoProto* value, const Port& port) {
    value->set_name(port.name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    if (port.dtype == "float32") tensor->set_elem_type(onnx::TensorProto::FLOAT);
    else if (port.dtype == "int64") tensor->set_elem_type(onnx::TensorProto::INT64);
    else if (port.dtype == "bool") tensor->set_elem_type(onnx::TensorProto::BOOL);
    else throw std::invalid_argument("unsupported Wan denoiser port dtype: " + port.dtype);
    for (const auto& dim : port.shape) {
      auto* extent = tensor->mutable_shape()->add_dim();
      if (const auto* fixed = std::get_if<std::int64_t>(&dim))
        extent->set_dim_value(*fixed);
      else
        extent->set_dim_param(std::get<std::string>(dim));
    }
  };
  const auto graphPort = [] (const auto& ports, const std::string& name) {
    const auto found = std::find_if(ports.begin(), ports.end(),
                                   [&] (const auto& value) { return value.name() == name; });
    if (found == ports.end()) throw std::invalid_argument("missing Wan denoiser port: " + name);
    const auto& type = found->type().tensor_type();
    Port result{name, type.elem_type() == onnx::TensorProto::FLOAT ? "float32" :
                      type.elem_type() == onnx::TensorProto::BOOL ? "bool" : "", {}};
    for (const auto& dim : type.shape().dim()) {
      if (!dim.has_dim_value() || dim.dim_value() <= 0)
        throw std::invalid_argument("Wan denoiser expects fixed positive dimensions");
      result.shape.emplace_back(dim.dim_value());
    }
    return result;
  };
  const auto fragment = [&] (onnx::ModelProto& source, const std::string& prefix,
                             const Bindings& bindings) {
    if (source.opset_import_size() != 1 || !source.opset_import(0).domain().empty() ||
        source.opset_import(0).version() < 17 || source.opset_import(0).version() > 20)
      throw std::invalid_argument("unsupported Wan denoiser fragment opset");
    model.set_ir_version(std::max(model.ir_version(), source.ir_version()));
    const auto offset = static_cast<std::uint64_t>(graph.node_size());
    appendFragment(graph, std::move(*source.mutable_graph()), prefix, bindings);
    return offset;
  };
  const auto ingress = [&] (const OnnxEntryCut& cut, const std::string& name) {
    auto input = cut.inputs.at(0);
    input.name = name;
    declare(graph.add_input(), input);
    return input;
  };
  const auto addProjection = [&] (SummedProjection& projection, const std::string& prefix,
                                  const Bindings& bindings,
                                  const Bindings& rankOneBindings = {}) {
    const auto offset = fragment(projection.model, prefix, bindings);
    for (std::size_t rank = 0; rank < 2; ++rank) {
      projection.partials[rank] =
        rebaseCut(std::move(projection.partials[rank]), offset, prefix, bindings);
      if (rank == 1) bindEntryInputs(graph, projection.partials[rank], rankOneBindings);
      plan.call(prefix + "project", rank, projection.partials[rank]);
    }
    auto bias = rebaseCut(std::move(projection.addBias), offset, prefix, bindings);
    plan.sum(prefix + "sum", 0, projection.partials[0].outputs.at(0),
             projection.partials[1].outputs.at(0), bias.inputs.at(0));
    plan.call(prefix + "bias", 0, bias);
    return std::make_pair(bias.outputs.at(0), offset);
  };
  const auto latent = ingress(patch.partials[0], "latent");
  const auto context = ingress(text.partials[0], "context");
  const auto timestep = ingress(time.embedding.partials[0], "timestep");
  const auto patches = addProjection(patch, "patch/", {{patch.partials[0].inputs.at(0).name,
                                                       latent.name}}).first;
  if (patches.shape.size() != 5)
    throw std::invalid_argument("Wan denoiser patch projection requires NCDHW");
  const auto hiddenWidth = std::get<std::int64_t>(patches.shape.at(1));
  compatible(patches, {"patch-grid", "float32",
              {std::int64_t{1}, hiddenWidth, grid.frames, grid.height, grid.width}});
  auto hidden = Port{"patch-tokens", "float32",
                     {std::int64_t{1}, grid.paddedTokens, hiddenWidth}};
  const auto index = [&] (const std::string& name, std::initializer_list<std::int64_t> dims) {
    auto* value = graph.add_initializer();
    value->set_name(name);
    value->set_data_type(onnx::TensorProto::INT64);
    value->add_dims(dims.size());
    for (const auto dim : dims) value->add_int64_data(dim);
  };
  const auto emit = [&] (const char* op, const std::string& output,
                         std::initializer_list<std::string> inputs) {
    auto* value = graph.add_node();
    value->set_name(output);
    value->set_op_type(op);
    for (const auto& input : inputs) value->add_input(input);
    value->add_output(output);
    return value;
  };
  const auto first = static_cast<std::uint64_t>(graph.node_size());
  auto* perm = emit("Transpose", "patch-fhwc", {patches.name})->add_attribute();
  perm->set_name("perm");
  perm->set_type(onnx::AttributeProto::INTS);
  for (const auto dim : {0, 2, 3, 4, 1}) perm->add_ints(dim);
  index("patch-token-shape", {1, validTokens, hiddenWidth});
  emit("Reshape", validTokens == grid.paddedTokens ? hidden.name : "valid-patch-tokens",
       {"patch-fhwc", "patch-token-shape"});
  if (validTokens != grid.paddedTokens) {
    index("patch-token-pads", {0, 0, 0, 0, grid.paddedTokens - validTokens, 0});
    emit("Pad", hidden.name, {"valid-patch-tokens", "patch-token-pads"});
  }
  OnnxEntryCut flatten{{}, {patches}, {hidden}};
  for (auto i = first; i < static_cast<std::uint64_t>(graph.node_size()); ++i)
    flatten.nodeIndices.push_back(i);
  plan.call("patch/flatten", 0, flatten);

  const auto embeddedText =
    addProjection(text, "text/", {{text.partials[0].inputs.at(0).name, context.name}}).first;
  const auto [embedding, timeOffset] =
    addProjection(time.embedding, "time/", {{time.embedding.partials[0].inputs.at(0).name,
                                             timestep.name}});
  std::array<Port, 2> modulation;
  for (std::size_t rank = 0; rank < modulation.size(); ++rank) {
    const auto cut = rebaseCut(std::move(time.modulation[rank]), timeOffset, "time/", {});
    compatible(embedding, cut.inputs.at(0));
    plan.call("time/modulation", rank, cut);
    modulation[rank] = cut.outputs.at(0);
  }
  auto valid = graphPort(blocks.front().model.graph().input(), "video-valid");
  declare(graph.add_input(), valid);
  std::string hiddenRankOne = hidden.name;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    auto& block = blocks[i];
    const auto prefix = "block-" + std::to_string(i) + "/";
    const std::map<std::string, Port> inputs{
      {"hidden", hidden}, {"text-context", embeddedText}, {"video-valid", valid},
      {"modulation-rank-0", modulation[0]}, {"modulation-rank-1", modulation[1]}};
    Bindings bindings;
    for (const auto& [name, port] : inputs) {
      compatible(port, graphPort(block.model.graph().input(), name));
      bindings.emplace(name, port.name);
    }
    auto output = graphPort(block.model.graph().output(), "block-output");
    const auto offset = fragment(block.model, prefix, bindings);
    const auto rebound = [&] (const NativeStepSlotSpec& slot) {
      const auto found = bindings.find(slot.slotId);
      return Port{found == bindings.end() ? prefix + slot.slotId : found->second,
                  slot.dtype, slot.shape};
    };
    block.tensorParallel.validateSteps();
    if (block.tensorParallel.roles.size() != 2)
      throw std::invalid_argument("Wan denoiser block requires two ranked entries");
    for (const auto& step : block.tensorParallel.steps) {
      const std::size_t rank = step.unitId == block.tensorParallel.roles[0] ? 0 : 1;
      if (step.operation == "CALL_SUBGRAPH") {
        auto cut = rebaseCut(std::move(block.entries.at(step.artifactEntry)), offset,
                             prefix, bindings);
        if (rank == 1 && hiddenRankOne != hidden.name)
          bindEntryInputs(graph, cut, {{hidden.name, hiddenRankOne}});
        plan.call(prefix + step.stepId, rank, cut);
      }
      else {
        plan.sum(prefix + step.stepId, rank, rebound(step.inputSlots.at(0)),
                 rebound(step.inputSlots.at(1)), rebound(step.outputSlots.at(0)));
      }
    }
    output.name = prefix + output.name;
    hidden = std::move(output);
    hiddenRankOne = rankOneJoinName(hidden.name);
    result.blockEnds.push_back(static_cast<std::size_t>(graph.node_size()));
  }
  compatible(hidden, head.partials[0].inputs.at(0));
  compatible(embedding, head.partials[0].inputs.at(1));
  const auto output = addProjection(
    head, "head/", {{head.partials[0].inputs.at(0).name, hidden.name},
                    {head.partials[0].inputs.at(1).name, embedding.name},
                    {head.addBias.outputs.at(0).name, "prediction"}},
    {{hidden.name, hiddenRankOne}}).first;
  if (output.shape.size() != 5 || grid.height > LIMIT / 2 || grid.width > LIMIT / 2) {
    throw std::invalid_argument("Wan denoiser output requires NCDHW");
  }
  compatible(output, {"prediction", "float32",
              {std::int64_t{1}, output.shape.at(1), grid.frames, 2 * grid.height, 2 * grid.width}});
  compatible(latent, output);
  declare(graph.add_output(), output);
  result.tensorParallel.plan.validateSteps();
  return result;
}

OnnxParallelPlan
makeVideoDenoiserStagedPlan(const VideoDenoiserLayout& layout, std::size_t prefixBlocks,
                           const std::array<NativeStageLayout, 2>& stages)
{
  if (prefixBlocks == 0 || prefixBlocks >= layout.blockEnds.size())
    throw std::invalid_argument("Wan DiT cut must be between complete blocks");
  return makeContiguousEntryPlan(layout.model.graph(), layout.tensorParallel, stages,
                                 layout.blockEnds[prefixBlocks - 1]);
}

} // namespace ndnsf::di::wan
