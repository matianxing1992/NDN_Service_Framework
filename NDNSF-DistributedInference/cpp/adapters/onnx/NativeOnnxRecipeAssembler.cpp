#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx/onnx-ml.pb.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

std::string digest(const std::vector<std::uint8_t>& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), hash);
  std::ostringstream out;
  out << "sha256:";
  for (unsigned char value : hash) {
    out << "0123456789abcdef"[value >> 4]
        << "0123456789abcdef"[value & 0x0f];
  }
  return out.str();
}

void fail(const char* code)
{
  throw std::runtime_error(std::string("DI_NATIVE_ONNX_") + code);
}

void checkActive(const NativeAssemblyControl& control)
{
  if (!control.requireActive ||
      std::chrono::steady_clock::now() >= control.deadline) {
    fail("ASSEMBLY_TIMEOUT");
  }
  control.requireActive();
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right)
{
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    fail("SIZE_OVERFLOW");
  }
  return left + right;
}

std::uint64_t parseUint(const std::string& value, const char* field)
{
  if (value.empty() || (value.size() > 1 && value.front() == '0') ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return std::isdigit(ch) != 0;
      })) {
    throw std::runtime_error(std::string("DI_NATIVE_ONNX_EXTERNAL_") + field);
  }
  try {
    return std::stoull(value);
  }
  catch (...) {
    throw std::runtime_error(std::string("DI_NATIVE_ONNX_EXTERNAL_") + field);
  }
}

std::string externalValue(const onnx::TensorProto& tensor, const std::string& key)
{
  for (int i = 0; i < tensor.external_data_size(); ++i) {
    const auto& entry = tensor.external_data(i);
    if (entry.key() == key) return entry.value();
  }
  return {};
}

void inlineExternal(onnx::TensorProto& tensor,
                    const std::vector<std::uint8_t>& bytes)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL) return;
  const auto location = externalValue(tensor, "location");
  if (location.empty() || location.front() == '/' || location.find('\\') != std::string::npos) {
    fail("EXTERNAL_LOCATION");
  }
  std::size_t component = 0;
  while (component < location.size()) {
    const auto slash = location.find('/', component);
    const auto part = location.substr(component, slash == std::string::npos ?
                                      std::string::npos : slash - component);
    if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
    if (slash == std::string::npos) break;
    component = slash + 1;
  }
  const auto offsetText = externalValue(tensor, "offset");
  const auto lengthText = externalValue(tensor, "length");
  const auto offset = offsetText.empty() ? 0 : parseUint(offsetText, "OFFSET");
  if (offset > bytes.size()) fail("EXTERNAL_RANGE");
  const auto length = lengthText.empty() ? bytes.size() - offset : parseUint(lengthText, "LENGTH");
  if (length > bytes.size() - offset || length > std::numeric_limits<int>::max())
    fail("EXTERNAL_RANGE");
  tensor.set_raw_data(bytes.data() + offset, static_cast<int>(length));
  tensor.clear_external_data();
  tensor.set_data_location(onnx::TensorProto::DEFAULT);
}

bool graphHasExternal(const onnx::GraphProto& graph);

bool tensorHasExternal(const onnx::TensorProto& tensor)
{
  return tensor.data_location() == onnx::TensorProto::EXTERNAL;
}

bool graphHasExternal(const onnx::GraphProto& graph)
{
  for (const auto& tensor : graph.initializer()) {
    if (tensorHasExternal(tensor)) return true;
  }
  for (const auto& node : graph.node()) {
    for (const auto& attribute : node.attribute()) {
      if (attribute.has_t() && tensorHasExternal(attribute.t())) return true;
      for (const auto& tensor : attribute.tensors()) {
        if (tensorHasExternal(tensor)) return true;
      }
      if (attribute.has_g() && graphHasExternal(attribute.g())) return true;
      for (const auto& nested : attribute.graphs()) {
        if (graphHasExternal(nested)) return true;
      }
    }
  }
  return false;
}

void inlineGraph(onnx::GraphProto& graph,
                 const std::vector<std::uint8_t>& initializerBytes)
{
  for (int i = 0; i < graph.initializer_size(); ++i) {
    inlineExternal(*graph.mutable_initializer(i), initializerBytes);
  }
  for (int i = 0; i < graph.node_size(); ++i) {
    auto* node = graph.mutable_node(i);
    for (int j = 0; j < node->attribute_size(); ++j) {
      auto* attribute = node->mutable_attribute(j);
      if (attribute->has_t()) inlineExternal(*attribute->mutable_t(), initializerBytes);
      for (int k = 0; k < attribute->tensors_size(); ++k)
        inlineExternal(*attribute->mutable_tensors(k), initializerBytes);
      if (attribute->has_g()) inlineGraph(*attribute->mutable_g(), initializerBytes);
      for (int k = 0; k < attribute->graphs_size(); ++k)
        inlineGraph(*attribute->mutable_graphs(k), initializerBytes);
    }
  }
}

void validateContracts(const std::vector<NativeAssemblyTensorContractV3>& contracts,
                       std::set<std::string>& names)
{
  for (const auto& contract : contracts) {
    if (contract.name.empty() || contract.dtype.empty() ||
        !names.insert(contract.name).second) fail("IO_CONTRACT");
  }
}

int
onnxElementType(const std::string& dtype)
{
  std::string value;
  value.reserve(dtype.size());
  for (const auto ch : dtype) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      value.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }
  if (value == "FLOAT" || value == "FLOAT32" || value == "F32" ||
      value == "TENSORFLOAT") return onnx::TensorProto::FLOAT;
  if (value == "FLOAT16" || value == "F16" || value == "HALF")
    return onnx::TensorProto::FLOAT16;
  if (value == "BFLOAT16" || value == "BF16")
    return onnx::TensorProto::BFLOAT16;
  if (value == "DOUBLE" || value == "FLOAT64" || value == "F64")
    return onnx::TensorProto::DOUBLE;
  if (value == "INT64" || value == "I64") return onnx::TensorProto::INT64;
  if (value == "INT32" || value == "I32") return onnx::TensorProto::INT32;
  if (value == "INT16" || value == "I16") return onnx::TensorProto::INT16;
  if (value == "INT8" || value == "I8") return onnx::TensorProto::INT8;
  if (value == "UINT8" || value == "U8") return onnx::TensorProto::UINT8;
  if (value == "BOOL") return onnx::TensorProto::BOOL;
  fail("IO_DTYPE");
  return onnx::TensorProto::UNDEFINED;
}

std::vector<std::uint8_t> deterministicWire(const onnx::ModelProto& model,
                                            std::uint64_t limit)
{
  const auto size = model.ByteSizeLong();
  if (size == 0 || size > limit || size > std::numeric_limits<int>::max())
    fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!model.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return bytes;
}

} // namespace

NativeCertifiedAssembly
assembleNativeCertifiedOnnxModel(const NativeCanonicalSource& source,
                                 const NativeCertifiedRecipe& recipe,
                                 const NativeAssemblyControl& control)
{
  checkActive(control);
  if (control.maxSourceBytes == 0 || control.maxAssembledBytes == 0 ||
      source.modelBytes.empty() || source.modelBytes.size() > control.maxSourceBytes ||
      source.modelBytes.size() > std::numeric_limits<int>::max())
    fail("SOURCE_LIMIT");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > control.maxSourceBytes ||
       source.initializerBytes->size() > std::numeric_limits<int>::max() ||
       checkedAdd(source.modelBytes.size(), source.initializerBytes->size()) >
         checkedAdd(control.maxSourceBytes, control.maxSourceBytes)))
    fail("INITIALIZER_LIMIT");
  if (recipe.adapterId.empty() || recipe.backend.empty() || recipe.roleKind.empty() ||
      recipe.nodeIndices.empty() || recipe.maxNodes == 0 ||
      recipe.nodeIndices.size() > recipe.maxNodes) fail("RECIPE");
  std::set<std::string> inputNames;
  std::set<std::string> outputNames;
  validateContracts(recipe.expectedInputs, inputNames);
  validateContracts(recipe.expectedOutputs, outputNames);
  if (inputNames.empty() || outputNames.empty()) fail("IO_CONTRACT");

  onnx::ModelProto original;
  if (!original.ParseFromArray(source.modelBytes.data(),
                              static_cast<int>(source.modelBytes.size())))
    fail("PARSE");
  if (!original.has_graph() || original.graph().node_size() == 0 ||
      original.graph().node_size() > static_cast<int>(recipe.maxNodes)) fail("GRAPH");
  const bool hasExternal = graphHasExternal(original.graph());
  if (hasExternal != source.initializerBytes.has_value()) fail("EXTERNAL_BINDING");
  if (source.initializerBytes) inlineGraph(*original.mutable_graph(), *source.initializerBytes);

  std::set<std::uint64_t> selected;
  for (const auto index : recipe.nodeIndices) {
    if (index >= static_cast<std::uint64_t>(original.graph().node_size()) ||
        !selected.insert(index).second) fail("NODE_COVER");
  }
  if (recipe.roleKind == "COMPONENT_SET" && selected.size() !=
      static_cast<std::size_t>(original.graph().node_size())) fail("NODE_COVER");
  if (recipe.roleKind != "COMPONENT_SET" && recipe.layerEnd <= recipe.layerBegin)
    fail("LAYER_RANGE");

  onnx::ModelProto assembled = original;
  auto* graph = assembled.mutable_graph();
  std::vector<onnx::NodeProto> selectedNodes;
  selectedNodes.reserve(selected.size());
  for (int i = 0; i < graph->node_size(); ++i) {
    if (selected.count(static_cast<std::uint64_t>(i)) != 0) selectedNodes.push_back(graph->node(i));
  }
  graph->clear_node();
  for (const auto& node : selectedNodes) *graph->add_node() = node;
  graph->clear_input();
  for (const auto& contract : recipe.expectedInputs) {
    auto* value = graph->add_input();
    value->set_name(contract.name);
    value->mutable_type()->mutable_tensor_type()->set_elem_type(
      onnxElementType(contract.dtype));
    for (const auto& dimension : contract.shape) {
      auto* dim = value->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim();
      try { dim->set_dim_value(std::stoll(dimension)); }
      catch (...) { dim->set_dim_param(dimension); }
    }
  }
  graph->clear_output();
  for (const auto& contract : recipe.expectedOutputs) {
    auto* value = graph->add_output();
    value->set_name(contract.name);
    value->mutable_type()->mutable_tensor_type()->set_elem_type(
      onnxElementType(contract.dtype));
    for (const auto& dimension : contract.shape) {
      auto* dim = value->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim();
      try { dim->set_dim_value(std::stoll(dimension)); }
      catch (...) { dim->set_dim_param(dimension); }
    }
  }
  checkActive(control);
  auto bytes = deterministicWire(assembled, control.maxAssembledBytes);
  checkActive(control);
  NativeCertifiedAssembly result;
  result.modelDigest = digest(bytes);
  result.modelBytes = std::move(bytes);
  result.nodeCount = selected.size();
  for (const auto& contract : recipe.expectedInputs) result.inputNames.push_back(contract.name);
  for (const auto& contract : recipe.expectedOutputs) result.outputNames.push_back(contract.name);
  return result;
}

} // namespace ndnsf::di
