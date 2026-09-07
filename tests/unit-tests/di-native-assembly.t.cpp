#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx/onnx-ml.pb.h"

#include <boost/test/unit_test.hpp>

namespace ndnsf::di {
namespace {

NativeCertifiedRecipe recipe()
{
  NativeCertifiedRecipe value;
  value.adapterId = "onnx";
  value.adapterVersion = "1";
  value.backend = "onnxruntime";
  value.backendAbi = "onnxruntime-cpu-v1";
  value.roleKind = "COMPONENT_SET";
  value.role = "/role/0";
  value.selectedRole = value.role;
  value.nodeIndices = {0};
  value.maxNodes = 8;
  value.maxSourceBytes = 64 * 1024;
  value.maxAssembledBytes = 64 * 1024;
  value.expectedInputs = {{"x", "float32", {"1", "1"}}};
  value.expectedOutputs = {{"y", "float32", {"1", "1"}}};
  return value;
}

std::vector<std::uint8_t> modelBytes()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("spec182-native-assembly");
  auto* input = graph->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  input->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* output = graph->add_output();
  output->set_name("y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* node = graph->add_node();
  node->set_op_type("Identity");
  node->add_input("x");
  node->add_output("y");
  const auto size = model.ByteSizeLong();
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  BOOST_REQUIRE(model.SerializeToArray(bytes.data(), static_cast<int>(bytes.size())));
  return bytes;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativeAssembly)

BOOST_AUTO_TEST_CASE(AssemblesComponentSetWithoutInterpreter)
{
  NativeCanonicalSource source{modelBytes(), std::nullopt};
  const auto control = NativeAssemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, 64 * 1024, 64 * 1024};
  const auto result = assembleNativeCertifiedOnnxModel(source, recipe(), control);
  BOOST_CHECK_EQUAL(result.nodeCount, 1);
  BOOST_CHECK_EQUAL(result.inputNames.at(0), "x");
  BOOST_CHECK_EQUAL(result.outputNames.at(0), "y");
  BOOST_CHECK(!result.modelBytes.empty());
  BOOST_CHECK_EQUAL(result.modelDigest.rfind("sha256:", 0), 0);
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateNodeCover)
{
  auto invalid = recipe();
  invalid.nodeIndices = {0, 0};
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, 64 * 1024, 64 * 1024};
  BOOST_CHECK_THROW(assembleNativeCertifiedOnnxModel(
                     NativeCanonicalSource{modelBytes(), std::nullopt}, invalid, control),
                   std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
