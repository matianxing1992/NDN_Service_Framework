#ifndef SPEC182_NATIVE_QWEN_CONFIG_HPP
#define SPEC182_NATIVE_QWEN_CONFIG_HPP

#include <onnx/onnx_pb.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di::fixture {

/** Build the small source-bound Qwen contract fixture without generated files. */
inline std::string
makeQwenNativeConfigOnnxSource()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(13);

  auto* graph = model.mutable_graph();
  graph->set_name("spec182-qwen-native-config");
  const auto addTensor = [] (onnx::ValueInfoProto* value,
                             const std::string& name,
                             onnx::TensorProto_DataType elementType,
                             const std::vector<std::string>& dimensions) {
    value->set_name(name);
    auto* tensorType = value->mutable_type()->mutable_tensor_type();
    tensorType->set_elem_type(elementType);
    for (const auto& dimension : dimensions)
      tensorType->mutable_shape()->add_dim()->set_dim_param(dimension);
  };

  addTensor(graph->add_input(), "input_ids", onnx::TensorProto::INT64,
            {"batch", "sequence"});
  addTensor(graph->add_output(), "logits", onnx::TensorProto::FLOAT,
            {"batch", "sequence"});
  auto* logits = graph->add_node();
  logits->set_name("embedding");
  logits->set_op_type("Cast");
  logits->add_input("input_ids");
  logits->add_output("embedding_out");
  auto* castType = logits->add_attribute();
  castType->set_name("to");
  castType->set_type(onnx::AttributeProto::INT);
  castType->set_i(onnx::TensorProto::FLOAT);

  std::string previousHidden = "embedding_out";
  const std::vector<std::pair<std::string, std::string>> hiddenStages{
    {"layer-00", "layer_00_out"}, {"layer-01", "layer_01_out"},
    {"final-norm", "final_norm_out"}, {"head-hidden", "head_hidden_out"},
    {"head-projection", "head_projection_out"}, {"logits", "logits"},
  };
  for (const auto& [stage, outputName] : hiddenStages) {
    auto* node = graph->add_node();
    node->set_name(stage);
    node->set_op_type("Identity");
    node->add_input(previousHidden);
    node->add_output(outputName);
    previousHidden = outputName;
  }

  const std::vector<std::pair<std::string, std::vector<std::string>>> stateShapes{
    {"attention_kv", {"batch", "2", "past_sequence", "heads", "head_dim"}},
    {"recurrent_state", {"batch", "heads", "head_dim"}},
    {"convolution_state", {"batch", "heads", "kernel"}},
  };
  for (const auto& [family, shape] : stateShapes) {
    const auto inputName = family + "_in";
    const auto outputName = family + "_out";
    addTensor(graph->add_input(), inputName, onnx::TensorProto::FLOAT, shape);
    addTensor(graph->add_output(), outputName, onnx::TensorProto::FLOAT, shape);
    auto* state = graph->add_node();
    state->set_name(family + "_state_passthrough");
    state->set_op_type("Identity");
    state->add_input(inputName);
    state->add_output(outputName);
  }

  std::string sourceBytes;
  if (!model.SerializeToString(&sourceBytes))
    throw std::runtime_error("failed to serialize Spec182 Qwen source fixture");
  return sourceBytes;
}

} // namespace ndnsf::di::fixture

#endif // SPEC182_NATIVE_QWEN_CONFIG_HPP
