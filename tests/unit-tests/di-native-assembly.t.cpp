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

std::vector<std::uint8_t> modelBytesWithNestedExternal()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* graph = model.mutable_graph();
  graph->set_name("spec182-native-nested-external");
  auto* input = graph->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* output = graph->add_output();
  output->set_name("y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* node = graph->add_node();
  node->set_op_type("If");
  node->add_input("x");
  node->add_output("y");
  auto* nested = node->add_attribute()->mutable_g();
  nested->set_name("then_branch");
  auto* initializer = nested->add_initializer();
  initializer->set_name("nested-weight");
  initializer->set_data_location(onnx::TensorProto::EXTERNAL);
  auto* location = initializer->add_external_data();
  location->set_key("location");
  location->set_value("weights.bin");
  auto* offset = initializer->add_external_data();
  offset->set_key("offset");
  offset->set_value("0");
  auto* length = initializer->add_external_data();
  length->set_key("length");
  length->set_value("4");
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

BOOST_AUTO_TEST_CASE(InlinesNestedGraphExternalInitializers)
{
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, 64 * 1024, 64 * 1024};
  const auto result = assembleNativeCertifiedOnnxModel(
    NativeCanonicalSource{modelBytesWithNestedExternal(), std::vector<std::uint8_t>{1, 2, 3, 4}},
    recipe(), control);
  onnx::ModelProto assembled;
  BOOST_REQUIRE(assembled.ParseFromArray(result.modelBytes.data(),
                                          static_cast<int>(result.modelBytes.size())));
  BOOST_REQUIRE_EQUAL(assembled.graph().node_size(), 1);
  const auto& attributes = assembled.graph().node(0).attribute();
  BOOST_REQUIRE_EQUAL(attributes.size(), 1);
  BOOST_REQUIRE(attributes.Get(0).has_g());
  BOOST_REQUIRE_EQUAL(attributes.Get(0).g().initializer_size(), 1);
  const auto& initializer = attributes.Get(0).g().initializer(0);
  BOOST_CHECK_EQUAL(initializer.data_location(), onnx::TensorProto::DEFAULT);
  BOOST_CHECK_EQUAL(initializer.raw_data().size(), 4U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
