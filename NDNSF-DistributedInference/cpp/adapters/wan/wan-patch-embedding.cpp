#include "wan-patch-embedding.hpp"

#include "wan-checkpoint.hpp"

#include <initializer_list>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

const onnx::TensorProto&
requireInitializer(const onnx::GraphProto& graph, const std::string& name)
{
  const onnx::TensorProto* found = nullptr;
  for (const auto& tensor : graph.initializer()) {
    if (tensor.name() == name) {
      if (found != nullptr) {
        throw std::invalid_argument("duplicate Wan patch initializer");
      }
      found = &tensor;
    }
  }
  if (found == nullptr || found->data_type() != onnx::TensorProto::FLOAT) {
    throw std::invalid_argument("Wan patch initializer must be float32");
  }
  return *found;
}

} // namespace

SummedProjection
lowerPatchEmbedding(const onnx::ModelProto& source, const WanInitializerAccess& material)
{
  const auto& patch = source.graph();
  if (patch.node_size() != 1 || patch.input_size() != 1 || patch.output_size() != 1) {
    throw std::invalid_argument("Wan patch source must isolate one Conv3D");
  }
  const auto& conv = patch.node(0);
  if (conv.op_type() != "Conv" || !conv.domain().empty() || conv.name().empty() ||
      conv.input_size() != 3 || conv.output_size() != 1 || patch.initializer_size() != 2 ||
      conv.input(0) != patch.input(0).name() || conv.output(0) != patch.output(0).name() ||
      conv.input(0) == conv.input(1) || conv.input(0) == conv.input(2) ||
      conv.input(0) == conv.output(0)) {
    throw std::invalid_argument("invalid Wan patch Conv3D bindings");
  }
  const std::string prefix = conv.name() + "/ndnsf-";
  if (conv.input(0).rfind(prefix, 0) == 0 || conv.output(0).rfind(prefix, 0) == 0 ||
      conv.input(0).rfind(conv.name() + "/source-transform/", 0) == 0 ||
      conv.output(0).rfind(conv.name() + "/source-transform/", 0) == 0) {
    throw std::invalid_argument("Wan patch port conflicts with reserved generated names");
  }
  const auto& weight = requireInitializer(patch, conv.input(1));
  const auto& bias = requireInitializer(patch, conv.input(2));
  if (conv.input(1) == conv.input(2) || weight.dims_size() != 5 ||
      weight.dims(0) <= 0 || weight.dims(1) < 2 ||
      weight.dims(2) != 1 || weight.dims(3) != 2 || weight.dims(4) != 2 ||
      bias.dims_size() != 1 || bias.dims(0) != weight.dims(0)) {
    throw std::invalid_argument("invalid Wan patch weight geometry");
  }
  for (const auto& attribute : conv.attribute()) {
    if (attribute.name() == "auto_pad" && attribute.s() != "NOTSET") {
      throw std::invalid_argument("Wan patch requires explicit unpadded geometry");
    }
  }

  const auto tensorPort = [] (const onnx::ValueInfoProto& port) {
    const auto& tensor = port.type().tensor_type();
    if (tensor.elem_type() != onnx::TensorProto::FLOAT || tensor.shape().dim_size() != 5)
      throw std::invalid_argument("Wan patch requires fixed NCDHW float32 ports");
    NativeAssemblyTensorContractV3 contract{port.name(), "float32", {}};
    for (const auto& extent : tensor.shape().dim()) {
      if (!extent.has_dim_value() || extent.dim_value() <= 0)
        throw std::invalid_argument("Wan patch requires fixed positive dimensions");
      contract.shape.emplace_back(extent.dim_value());
    }
    return contract;
  };
  const auto input = tensorPort(patch.input(0));
  const auto output = tensorPort(patch.output(0));
  const auto dim = [&] (std::size_t axis) { return std::get<std::int64_t>(input.shape[axis]); };
  const decltype(output.shape) expectedShape{dim(0), weight.dims(0), dim(2),
                                            dim(3) / 2, dim(4) / 2};
  if (dim(0) != 1 || dim(1) != weight.dims(1) || dim(3) % 2 != 0 || dim(4) % 2 != 0 ||
      output.shape != expectedShape) {
    throw std::invalid_argument("Wan patch ports differ from the Conv3D geometry");
  }

  NativeOnnxSourceTransformV1 transform;
  transform.kind = "conv-input-channel-partial";
  transform.node = conv.name();
  transform.axis = "input-channel";
  transform.input = conv.input(0);
  transform.weight = conv.input(1);
  transform.bias = conv.input(2);
  transform.output = conv.output(0);
  transform.extent = static_cast<std::uint64_t>(weight.dims(1));
  transform.pads = {0, 0, 0, 0, 0, 0};
  transform.strides = {1, 2, 2};
  transform.dilations = {1, 1, 1};

  SummedProjection result;
  result.model = source;
  auto* graph = result.model.mutable_graph();
  graph->clear_node();
  graph->clear_initializer();
  const auto ranges = nativeConvInputChannelRanges(transform.extent, 2);
  for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
    const auto rankPrefix = prefix + "rank-" + std::to_string(rank) + "/";
    transform.begin = ranges[rank].first;
    transform.end = ranges[rank].second;
    const auto fragment = applyNativeOnnxSourceTransform(source, transform, material.reader, {},
                                                         material.slicer);
    const auto rename = [&] (const std::string& name) -> std::string {
      return name == transform.input ? name : rankPrefix + name;
    };
    auto& entry = result.partials[rank];
    entry.inputs = {input};
    entry.outputs = {output};
    entry.outputs.front().name = rename(transform.output);
    for (const auto& tensor : fragment.graph().initializer()) {
      if (tensor.name() == transform.bias) {
        continue;
      }
      auto* emitted = graph->add_initializer();
      *emitted = tensor;
      emitted->set_name(rename(tensor.name()));
    }
    for (const auto& fragmentNode : fragment.graph().node()) {
      entry.nodeIndices.push_back(static_cast<std::uint64_t>(graph->node_size()));
      auto* emitted = graph->add_node();
      *emitted = fragmentNode;
      emitted->set_name(rankPrefix + fragmentNode.name());
      for (auto& input : *emitted->mutable_input()) input = rename(input);
      for (auto& output : *emitted->mutable_output()) output = rename(output);
    }
  }
  auto* sum = graph->add_node();
  sum->set_name(prefix + "sum");
  sum->set_op_type("Add");
  for (const auto& entry : result.partials) sum->add_input(entry.outputs.front().name);
  sum->add_output(prefix + "unbiased");

  auto* broadcastBias = graph->add_initializer();
  *broadcastBias = material.slice(bias, 0, 0, bias.dims(0));
  broadcastBias->set_name(prefix + "bias");
  broadcastBias->clear_dims();
  for (const auto extent : std::initializer_list<std::int64_t>{1, bias.dims(0), 1, 1, 1})
    broadcastBias->add_dims(extent);

  result.addBias = {{static_cast<std::uint64_t>(graph->node_size())}, {output}, {output}};
  result.addBias.inputs.front().name = sum->output(0);
  auto* addBias = graph->add_node();
  addBias->set_name(prefix + "add-bias");
  addBias->set_op_type("Add");
  addBias->add_input(result.addBias.inputs.front().name);
  addBias->add_input(broadcastBias->name());
  addBias->add_output(transform.output);
  return result;
}

SummedProjection
lowerWan21PatchEmbedding(const WanCheckpoint& checkpoint,
                        const std::array<std::int64_t, 4>& latentShape,
                        const NativeOnnxInitializerSlicer& slicer)
{
  const auto& parameters = checkpoint.parameters();
  const auto weight = parameters.find("patch_embedding.weight");
  const auto bias = parameters.find("patch_embedding.bias");
  if (weight == parameters.end() || bias == parameters.end() || weight->second.dims_size() != 5)
    throw std::invalid_argument("Wan checkpoint patch weights are missing or invalid");
  const auto [channels, frames, height, width] = latentShape;
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(18);
  auto& graph = *source.mutable_graph();
  graph.set_name("wan-patch-embedding");
  const auto declare = [] (onnx::ValueInfoProto& port, const char* name,
                            std::initializer_list<std::int64_t> shape) {
    port.set_name(name);
    auto& type = *port.mutable_type()->mutable_tensor_type();
    type.set_elem_type(onnx::TensorProto::FLOAT);
    for (auto extent : shape) type.mutable_shape()->add_dim()->set_dim_value(extent);
  };
  declare(*graph.add_input(), "latent", {1, channels, frames, height, width});
  declare(*graph.add_output(), "projection",
          {1, weight->second.dims(0), frames, height / 2, width / 2});
  *graph.add_initializer() = weight->second;
  *graph.add_initializer() = bias->second;
  auto& conv = *graph.add_node();
  conv.set_name("patch_embedding");
  conv.set_op_type("Conv");
  for (const auto* input : {"latent", "patch_embedding.weight", "patch_embedding.bias"})
    conv.add_input(input);
  conv.add_output("projection");
  auto& strides = *conv.add_attribute();
  strides.set_name("strides");
  strides.set_type(onnx::AttributeProto::INTS);
  for (const auto extent : {1, 2, 2}) strides.add_ints(extent);
  return lowerPatchEmbedding(source, {checkpoint.reader(), slicer});
}

} // namespace ndnsf::di::wan
