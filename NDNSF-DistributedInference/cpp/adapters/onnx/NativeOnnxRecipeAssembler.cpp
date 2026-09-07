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

// ---------------------------------------------------------------------------
// T006-A canonical source identity (OA05/OA06 seam).
//
// Python-identity compatibility target: the frozen 24 numeric + 14 extended
// reference vectors (tests/fixtures/spec182/dependency-probes) were produced
// by the pinned graph.py e5532328.. reference; this code reproduces that
// graph-facts JSON and ordered initializer digest, with the v2 normalization
// rules from initializer-normalization.md overriding the payload encoding
// (BFLOAT16 raw/typed, STRING framing, typed COMPLEX, FLOAT8/INT4/UINT4).
// Source filenames, external packing layout, protobuf field order and
// shape-inferred graphs never enter the identity.
// ---------------------------------------------------------------------------

namespace {

void failNormalization(const char* code)
{
  // Registered DI_ONNX_* reason codes (normalization contract): the adapter
  // error text is not a protocol oracle, the reason family is.
  throw std::runtime_error(std::string("DI_ONNX_") + code);
}

// elementCount of TensorProto.dims with overflow checks; every dim must be
// non-negative; a scalar (no dims) counts as one element.
std::uint64_t tensorElementCount(const onnx::TensorProto& tensor)
{
  std::uint64_t count = 1;
  for (int i = 0; i < tensor.dims_size(); ++i) {
    const auto dim = tensor.dims(i);
    if (dim < 0) failNormalization("INITIALIZER_ENCODING_INVALID");
    if (static_cast<std::uint64_t>(dim) >
        std::numeric_limits<std::uint64_t>::max() / count)
      failNormalization("INITIALIZER_ENCODING_INVALID");
    count *= static_cast<std::uint64_t>(dim);
  }
  return count;
}

std::vector<std::uint8_t> u64le(std::uint64_t value)
{
  std::vector<std::uint8_t> bytes(8);
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>(value & 0xff);
    value >>= 8;
  }
  return bytes;
}

void appendU16le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void appendU32le(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    value >>= 8;
  }
}

void appendU64le(std::vector<std::uint8_t>& out, std::uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    value >>= 8;
  }
}

template<typename T>
void appendRaw(std::vector<std::uint8_t>& out, const T& value)
{
  static_assert(sizeof(T) == 4 || sizeof(T) == 8);
  const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
  for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(raw[i]);
}

const char* dtypeLabel(int dataType)
{
  switch (dataType) {
    case onnx::TensorProto::FLOAT: return "float32";
    case onnx::TensorProto::UINT8: return "uint8";
    case onnx::TensorProto::INT8: return "int8";
    case onnx::TensorProto::UINT16: return "uint16";
    case onnx::TensorProto::INT16: return "int16";
    case onnx::TensorProto::INT32: return "int32";
    case onnx::TensorProto::INT64: return "int64";
    case onnx::TensorProto::STRING: return "string";
    case onnx::TensorProto::BOOL: return "bool";
    case onnx::TensorProto::FLOAT16: return "float16";
    case onnx::TensorProto::DOUBLE: return "float64";
    case onnx::TensorProto::UINT32: return "uint32";
    case onnx::TensorProto::UINT64: return "uint64";
    case onnx::TensorProto::COMPLEX64: return "complex64";
    case onnx::TensorProto::COMPLEX128: return "complex128";
    // Fixed literal labels from the independent reference extraction; the
    // C++ must not "improve" these names, they enter the graph digest.
    case onnx::TensorProto::BFLOAT16: return "(numpy.uint16, [('bfloat16', '<u2')])";
    case onnx::TensorProto::FLOAT8E4M3FN: return "(numpy.uint8, [('e4m3fn', 'u1')])";
    case onnx::TensorProto::FLOAT8E4M3FNUZ: return "(numpy.uint8, [('e4m3fnuz', 'u1')])";
    case onnx::TensorProto::FLOAT8E5M2: return "(numpy.uint8, [('e5m2', 'u1')])";
    case onnx::TensorProto::FLOAT8E5M2FNUZ: return "(numpy.uint8, [('e5m2fnuz', 'u1')])";
    case onnx::TensorProto::UINT4: return "(numpy.uint8, [('uint4', 'u1')])";
    case onnx::TensorProto::INT4: return "(numpy.int8, [('int4', 'i1')])";
    default: failNormalization("INITIALIZER_ENCODING_INVALID"); return "";
  }
}

std::string byteOrderOf(int dataType)
{
  switch (dataType) {
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::INT8:
    case onnx::TensorProto::BOOL:
    case onnx::TensorProto::STRING:
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
    case onnx::TensorProto::UINT4:
    case onnx::TensorProto::INT4: return "na";
    default: return "little";
  }
}

// Exact number of packed raw bytes one count-sized tensor of dataType
// requires; throws when the byte size would overflow.
std::uint64_t rawByteLength(int dataType, std::uint64_t count)
{
  const auto scaled = [&] (std::uint64_t elementBytes) {
    if (elementBytes != 0 && count > std::numeric_limits<std::uint64_t>::max() / elementBytes)
      failNormalization("INITIALIZER_ENCODING_INVALID");
    return count * elementBytes;
  };
  switch (dataType) {
    case onnx::TensorProto::FLOAT:
    case onnx::TensorProto::INT32:
    case onnx::TensorProto::UINT32: return scaled(4);
    case onnx::TensorProto::DOUBLE:
    case onnx::TensorProto::INT64:
    case onnx::TensorProto::UINT64: return scaled(8);
    case onnx::TensorProto::UINT16:
    case onnx::TensorProto::INT16:
    case onnx::TensorProto::FLOAT16:
    case onnx::TensorProto::BFLOAT16: return scaled(2);
    case onnx::TensorProto::COMPLEX64: return scaled(8);
    case onnx::TensorProto::COMPLEX128: return scaled(16);
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::INT8:
    case onnx::TensorProto::BOOL: return scaled(1);
    case onnx::TensorProto::UINT4:
    case onnx::TensorProto::INT4:
      return (count + 1) / 2;  // packed bytes, ceil(count / 2)
    default: failNormalization("INITIALIZER_ENCODING_INVALID"); return 0;
  }
}

std::vector<std::uint8_t> encodeTypedComplex(const onnx::TensorProto& tensor,
                                             int dataType, std::uint64_t count)
{
  // typed COMPLEX stores 2 * count real/imag components in float_data
  // (complex64) or double_data (complex128), one component pair per element.
  std::vector<std::uint8_t> content;
  if (dataType == onnx::TensorProto::COMPLEX64) {
    if (tensor.float_data_size() != static_cast<int>(2 * count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 2))
      failNormalization("INITIALIZER_ENCODING_INVALID");
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.float_data_size(); ++i)
      appendRaw(content, tensor.float_data(i));
  } else {
    if (tensor.double_data_size() != static_cast<int>(2 * count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 2))
      failNormalization("INITIALIZER_ENCODING_INVALID");
    content.reserve(static_cast<std::size_t>(16 * count));
    for (int i = 0; i < tensor.double_data_size(); ++i)
      appendRaw(content, tensor.double_data(i));
  }
  return content;
}

std::vector<std::uint8_t> encodeTypedInt32Based(const onnx::TensorProto& tensor,
                                                int dataType, std::uint64_t count)
{
  // Typed storage for small/8-bit types is int32_data.  INT4/UINT4 tensors
  // pack two 4-bit elements per int32 byte (low nibble first), matching the
  // raw packed layout; every other type stores one element per int32 slot.
  const auto required = [&] {
    if (dataType == onnx::TensorProto::UINT4 || dataType == onnx::TensorProto::INT4)
      return (count + 1) / 2;
    return count;
  }();
  if (tensor.int32_data_size() != static_cast<int>(required) ||
      required > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    failNormalization("INITIALIZER_ENCODING_INVALID");

  std::vector<std::uint8_t> content;
  switch (dataType) {
    case onnx::TensorProto::INT8:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(tensor.int32_data(i))));
      return content;
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(static_cast<std::uint8_t>(tensor.int32_data(i) & 0xff));
      return content;
    case onnx::TensorProto::BOOL:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(tensor.int32_data(i) != 0 ? 1 : 0);
      return content;
    case onnx::TensorProto::INT16:
      content.reserve(static_cast<std::size_t>(2 * count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        appendU16le(content, static_cast<std::uint16_t>(static_cast<std::int16_t>(tensor.int32_data(i))));
      return content;
    case onnx::TensorProto::UINT16:
    case onnx::TensorProto::FLOAT16:
    case onnx::TensorProto::BFLOAT16:
      content.reserve(static_cast<std::size_t>(2 * count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        appendU16le(content, static_cast<std::uint16_t>(tensor.int32_data(i) & 0xffff));
      return content;
    case onnx::TensorProto::INT4: {
      // packed bytes, sign-extended to int8 per nibble, low nibble first
      content.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto packed = static_cast<std::uint8_t>(
          tensor.int32_data(static_cast<int>(i / 2)) & 0xff);
        const auto nibble = (i % 2 == 0) ? (packed & 0x0f) : (packed >> 4);
        content.push_back(static_cast<std::uint8_t>(
          static_cast<std::int8_t>(nibble & 8 ? nibble - 16 : nibble)));
      }
      return content;
    }
    case onnx::TensorProto::UINT4: {
      content.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto packed = static_cast<std::uint8_t>(
          tensor.int32_data(static_cast<int>(i / 2)) & 0xff);
        content.push_back((i % 2 == 0) ? (packed & 0x0f) : (packed >> 4));
      }
      return content;
    }
    default:
      failNormalization("INITIALIZER_ENCODING_INVALID");
      return {};
  }
}

std::vector<std::uint8_t> encodeTyped64(const onnx::TensorProto& tensor,
                                        int dataType, std::uint64_t count)
{
  const auto check = [&] (int present) {
    if (present != static_cast<int>(count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
      failNormalization("INITIALIZER_ENCODING_INVALID");
  };
  std::vector<std::uint8_t> content;
  if (dataType == onnx::TensorProto::INT64) {
    check(tensor.int64_data_size());
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.int64_data_size(); ++i)
      appendU64le(content, static_cast<std::uint64_t>(tensor.int64_data(i)));
  } else if (dataType == onnx::TensorProto::UINT64) {
    check(tensor.uint64_data_size());
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.uint64_data_size(); ++i)
      appendU64le(content, tensor.uint64_data(i));
  } else {  // UINT32 typed is stored in uint64_data by the onnx helpers
    check(tensor.uint64_data_size());
    content.reserve(static_cast<std::size_t>(4 * count));
    for (int i = 0; i < tensor.uint64_data_size(); ++i)
      appendU32le(content, static_cast<std::uint32_t>(tensor.uint64_data(i) & 0xffffffffULL));
  }
  return content;
}

std::vector<std::uint8_t> encodeString(const onnx::TensorProto& tensor,
                                       std::uint64_t count)
{
  // Explicit UTF-8 framing: ASCII "NDNSF-ONNX-STRING-v2" + NUL, uint64
  // little-endian elementCount, then per item (row-major) a uint64 LE byte
  // length followed by the raw UTF-8 bytes.  No Unicode normalization, no
  // numpy object memory; string_data must hold exactly elementCount items
  // and raw_data never substitutes for STRING encoding.
  if (tensor.string_data_size() != static_cast<int>(count) ||
      count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    failNormalization("INITIALIZER_ENCODING_INVALID");
  if (!tensor.raw_data().empty()) failNormalization("INITIALIZER_ENCODING_INVALID");
  std::vector<std::uint8_t> content;
  const char header[] = "NDNSF-ONNX-STRING-v2";
  content.insert(content.end(), header, header + 20);
  content.push_back(0);
  const auto countBytes = u64le(count);
  content.insert(content.end(), countBytes.begin(), countBytes.end());
  for (int i = 0; i < tensor.string_data_size(); ++i) {
    const auto& item = tensor.string_data(i);
    // Strict UTF-8 validation of each item; embedded NUL is a content byte.
    std::size_t pos = 0;
    while (pos < item.size()) {
      const auto first = static_cast<unsigned char>(item[pos]);
      std::size_t width = 0;
      if (first < 0x80) width = 1;
      else if ((first & 0xe0) == 0xc0) width = 2;
      else if ((first & 0xf0) == 0xe0) width = 3;
      else if ((first & 0xf8) == 0xf0) width = 4;
      else failNormalization("INITIALIZER_ENCODING_INVALID");
      if (pos + width > item.size()) failNormalization("INITIALIZER_ENCODING_INVALID");
      for (std::size_t k = 1; k < width; ++k) {
        if ((static_cast<unsigned char>(item[pos + k]) & 0xc0) != 0x80)
          failNormalization("INITIALIZER_ENCODING_INVALID");
      }
      pos += width;
    }
    const auto lengthBytes = u64le(item.size());
    content.insert(content.end(), lengthBytes.begin(), lengthBytes.end());
    content.insert(content.end(), item.begin(), item.end());
  }
  return content;
}

} // namespace

NormalizedInitializerPayload
normalizedOnnxInitializerPayload(const std::vector<std::uint8_t>& serializedTensorProto)
{
  onnx::TensorProto tensor;
  if (serializedTensorProto.empty() ||
      !tensor.ParseFromArray(serializedTensorProto.data(),
                             static_cast<int>(serializedTensorProto.size())) ||
      static_cast<std::size_t>(tensor.ByteSizeLong()) != serializedTensorProto.size())
    failNormalization("INITIALIZER_ENCODING_INVALID");
  const int dataType = tensor.data_type();
  const auto count = tensorElementCount(tensor);

  NormalizedInitializerPayload payload;
  payload.shape.reserve(static_cast<std::size_t>(tensor.dims_size()));
  for (int i = 0; i < tensor.dims_size(); ++i) payload.shape.push_back(tensor.dims(i));
  payload.dtype = dtypeLabel(dataType);
  payload.byteOrder = byteOrderOf(dataType);

  if (dataType == onnx::TensorProto::STRING) {
    payload.content = encodeString(tensor, count);
  } else {
    // raw_data has priority exactly when it carries bytes (the same
    // precedence the onnx python helpers apply); an explicitly empty raw
    // field falls through to the typed representation.  The v2 rules fix
    // BFLOAT16/STRING raw handling by validating the raw bytes instead of
    // ignoring them.
    const bool typedComplex = (dataType == onnx::TensorProto::COMPLEX64 ||
                               dataType == onnx::TensorProto::COMPLEX128) &&
                              tensor.raw_data().empty();
    if (!tensor.raw_data().empty()) {
      const auto expected = rawByteLength(dataType, count);
      if (tensor.raw_data().size() != expected)
        failNormalization("INITIALIZER_ENCODING_INVALID");
      if (dataType == onnx::TensorProto::UINT4 || dataType == onnx::TensorProto::INT4) {
        // Raw INT4/UINT4 carry ceil(count/2) packed bytes; the canonical
        // payload expands every nibble to one byte (low nibble first),
        // sign-extending INT4, mirroring the typed encoders and the frozen
        // python reference bit patterns.
        payload.content.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
          const auto packed = static_cast<std::uint8_t>(
            tensor.raw_data()[static_cast<std::size_t>(i / 2)]);
          const auto nibble = (i % 2 == 0) ? (packed & 0x0f) : (packed >> 4);
          if (dataType == onnx::TensorProto::INT4)
            payload.content.push_back(static_cast<std::uint8_t>(
              static_cast<std::int8_t>(nibble & 8 ? nibble - 16 : nibble)));
          else
            payload.content.push_back(nibble);
        }
      } else {
        payload.content.assign(tensor.raw_data().begin(), tensor.raw_data().end());
      }
    } else if (typedComplex) {
      payload.content = encodeTypedComplex(tensor, dataType, count);
    } else {
      // Missing raw bytes and a non-empty tensor have no legal encoding.
      if (count > 0) {
        switch (dataType) {
          case onnx::TensorProto::FLOAT:
            if (tensor.float_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(4 * count));
            for (int i = 0; i < tensor.float_data_size(); ++i)
              appendRaw(payload.content, tensor.float_data(i));
            break;
          case onnx::TensorProto::DOUBLE:
            if (tensor.double_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(8 * count));
            for (int i = 0; i < tensor.double_data_size(); ++i)
              appendRaw(payload.content, tensor.double_data(i));
            break;
          case onnx::TensorProto::INT32:
            if (tensor.int32_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(4 * count));
            for (int i = 0; i < tensor.int32_data_size(); ++i)
              appendU32le(payload.content, static_cast<std::uint32_t>(tensor.int32_data(i)));
            break;
          case onnx::TensorProto::INT8:
          case onnx::TensorProto::UINT8:
          case onnx::TensorProto::INT16:
          case onnx::TensorProto::UINT16:
          case onnx::TensorProto::FLOAT16:
          case onnx::TensorProto::BFLOAT16:
          case onnx::TensorProto::BOOL:
          case onnx::TensorProto::FLOAT8E4M3FN:
          case onnx::TensorProto::FLOAT8E4M3FNUZ:
          case onnx::TensorProto::FLOAT8E5M2:
          case onnx::TensorProto::FLOAT8E5M2FNUZ:
          case onnx::TensorProto::UINT4:
          case onnx::TensorProto::INT4:
            payload.content = encodeTypedInt32Based(tensor, dataType, count);
            break;
          case onnx::TensorProto::INT64:
          case onnx::TensorProto::UINT64:
          case onnx::TensorProto::UINT32:
            payload.content = encodeTyped64(tensor, dataType, count);
            break;
          default:
            failNormalization("INITIALIZER_ENCODING_INVALID");
        }
      }
      // count == 0 leaves an empty payload; a non-empty typed/raw field on
      // a zero-element tensor is caught by the exact length checks above.
    }
  }
  return payload;
}

namespace {

// Deep scan of every tensor an external tensor can hide in: graph and nested
// graph initializers, node attribute tensors, and function attribute tensors
// (S2 _get_all_tensors coverage).  The identity path validates and inlines
// strictly from the bytes the caller authenticated; it never opens a path
// declared inside the model.
void collectAttributeTensors(const onnx::AttributeProto& attribute,
                             std::vector<onnx::TensorProto*>& tensors)
{
  if (attribute.has_t()) tensors.push_back(const_cast<onnx::TensorProto*>(&attribute.t()));
  for (int i = 0; i < attribute.tensors_size(); ++i)
    tensors.push_back(const_cast<onnx::TensorProto*>(&attribute.tensors(i)));
}

void collectGraphTensors(onnx::GraphProto& graph, std::vector<onnx::TensorProto*>& tensors)
{
  for (int i = 0; i < graph.initializer_size(); ++i)
    tensors.push_back(graph.mutable_initializer(i));
  for (int i = 0; i < graph.node_size(); ++i) {
    auto* node = graph.mutable_node(i);
    for (int j = 0; j < node->attribute_size(); ++j) {
      auto* attribute = node->mutable_attribute(j);
      collectAttributeTensors(*attribute, tensors);
      if (attribute->has_g()) collectGraphTensors(*attribute->mutable_g(), tensors);
      for (int k = 0; k < attribute->graphs_size(); ++k)
        collectGraphTensors(*attribute->mutable_graphs(k), tensors);
    }
  }
}

void collectModelTensors(onnx::ModelProto& model, std::vector<onnx::TensorProto*>& tensors)
{
  collectGraphTensors(*model.mutable_graph(), tensors);
  for (int i = 0; i < model.functions_size(); ++i) {
    auto* function = model.mutable_functions(i);
    // FunctionProto.attribute is a name list in this schema version; tensor
    // payloads only live in the function nodes' attributes (incl. nested
    // attribute graphs) and in node attributes of the main graph.
    for (int j = 0; j < function->node_size(); ++j) {
      auto* node = function->mutable_node(j);
      for (int k = 0; k < node->attribute_size(); ++k) {
        auto* attribute = node->mutable_attribute(k);
        collectAttributeTensors(*attribute, tensors);
        if (attribute->has_g()) collectGraphTensors(*attribute->mutable_g(), tensors);
        for (int m = 0; m < attribute->graphs_size(); ++m)
          collectGraphTensors(*attribute->mutable_graphs(m), tensors);
      }
    }
  }
}

// S2 external rules for the identity path: every external tensor has exactly
// one location, no absolute/..//empty-basename components, all locations
// identical, offset/length bounded with length-0 meaning "rest from offset"
// (the 1.17 loader behavior).  Validation of the whole model happens before
// any copy.
std::string validateAndPinLocation(const std::vector<onnx::TensorProto*>& tensors)
{
  std::string pinnedLocation;
  for (auto* tensor : tensors) {
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    std::string location;
    int locationEntries = 0;
    for (int i = 0; i < tensor->external_data_size(); ++i) {
      const auto& entry = tensor->external_data(i);
      if (entry.key() == "location") {
        location = entry.value();
        ++locationEntries;
      }
    }
    if (locationEntries != 1 || location.empty() || location.front() == '/' ||
        location.find('\\') != std::string::npos)
      fail("EXTERNAL_LOCATION");
    std::size_t component = 0;
    while (component < location.size()) {
      const auto slash = location.find('/', component);
      const auto part = location.substr(component,
                                        slash == std::string::npos ? std::string::npos
                                                                   : slash - component);
      if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
      if (slash == std::string::npos) break;
      component = slash + 1;
    }
    if (!pinnedLocation.empty() && pinnedLocation != location) fail("EXTERNAL_LOCATION");
    pinnedLocation = location;
  }
  return pinnedLocation;
}

void inlineValidatedExternals(const std::vector<onnx::TensorProto*>& tensors,
                              const std::vector<std::uint8_t>& bytes)
{
  for (auto* tensor : tensors) {
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    const auto offsetText = [&] {
      for (int i = 0; i < tensor->external_data_size(); ++i) {
        const auto& entry = tensor->external_data(i);
        if (entry.key() == "offset") return entry.value();
      }
      return std::string();
    }();
    const auto lengthText = [&] {
      for (int i = 0; i < tensor->external_data_size(); ++i) {
        const auto& entry = tensor->external_data(i);
        if (entry.key() == "length") return entry.value();
      }
      return std::string();
    }();
    const auto offset = offsetText.empty() ? 0 : parseUint(offsetText, "OFFSET");
    if (offset > bytes.size()) fail("EXTERNAL_RANGE");
    std::uint64_t length = bytes.size() - offset;
    if (!lengthText.empty()) {
      length = parseUint(lengthText, "LENGTH");
      if (length == 0) length = bytes.size() - offset;  // 1.17 loader rule
      if (length > bytes.size() - offset || length > std::numeric_limits<int>::max())
        fail("EXTERNAL_RANGE");
    }
    tensor->set_raw_data(reinterpret_cast<const char*>(bytes.data() + offset),
                         static_cast<int>(length));
    tensor->clear_external_data();
    tensor->set_data_location(onnx::TensorProto::DEFAULT);
  }
}

// Shared OA05-for-identity entry: source limits, parse, external binding and
// memory-only inlining.  It deliberately does not full-check or shape-infer.
onnx::ModelProto ownedSourceModel(const NativeCanonicalSource& source,
                                  const NativeAssemblyControl& control)
{
  checkActive(control);
  if (control.maxSourceBytes == 0 ||
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

  onnx::ModelProto model;
  if (!model.ParseFromArray(source.modelBytes.data(),
                            static_cast<int>(source.modelBytes.size())))
    fail("PARSE");
  std::vector<onnx::TensorProto*> externalTensors;
  collectModelTensors(model, externalTensors);
  const bool hasExternal = std::any_of(
    externalTensors.begin(), externalTensors.end(), [] (const onnx::TensorProto* tensor) {
      return tensor->data_location() == onnx::TensorProto::EXTERNAL;
    });
  if (hasExternal != source.initializerBytes.has_value()) fail("EXTERNAL_BINDING");
  if (source.initializerBytes) {
    validateAndPinLocation(externalTensors);
    inlineValidatedExternals(externalTensors, *source.initializerBytes);
  }
  checkActive(control);
  return model;
}

// --- python-identity-equivalent JSON composition -------------------------

std::string jsonEscape(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20) {
          const char hex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[byte >> 4]);
          out.push_back(hex[byte & 0x0f]);
        } else {
          out.push_back(ch);  // UTF-8 bytes pass through untouched
        }
    }
  }
  return out;
}

void jsonString(std::string& out, const std::string& value)
{
  out.push_back('"');
  out += jsonEscape(value);
  out.push_back('"');
}

// Deterministic protobuf wire, lowercase hex, exactly like the python
// reference proto_hex helper.  onnx-ml.pb.h is generated for the lite
// runtime, so every onnx message derives MessageLite.
std::string protoHex(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > std::numeric_limits<int>::max()) fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 * bytes.size());
  for (const auto byte : bytes) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

void jsonHexBytes(std::string& out, const google::protobuf::MessageLite& message)
{
  jsonString(out, protoHex(message));
}

std::string sha256HexOf(const std::string& utf8)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(utf8.data()), utf8.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 0x0f];
  }
  return result;
}

struct TensorIndexEntry
{
  std::string name;
  NormalizedInitializerPayload payload;
  std::string contentDigest;
  std::string sharedReference;
};

// python reference layout: sorted by tensor name; contentDigest of the
// canonical bytes; sharedReference is the first (smallest) alias name when
// more than one tensor shares the digest, else empty.
std::vector<TensorIndexEntry> buildTensorIndex(const onnx::ModelProto& model)
{
  std::vector<TensorIndexEntry> entries;
  entries.reserve(static_cast<std::size_t>(model.graph().initializer_size()));
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    const auto& initializer = model.graph().initializer(i);
    if (initializer.name().empty()) failNormalization("INITIALIZER_ENCODING_INVALID");
    TensorIndexEntry entry;
    entry.name = initializer.name();
    std::vector<std::uint8_t> serialized(
      static_cast<std::size_t>(initializer.ByteSizeLong()));
    if (!serialized.empty() &&
        !initializer.SerializeToArray(serialized.data(),
                                      static_cast<int>(serialized.size())))
      fail("SERIALIZE");
    entry.payload = normalizedOnnxInitializerPayload(serialized);
    entry.contentDigest = digest(entry.payload.content);
    entries.push_back(std::move(entry));
  }
  std::stable_sort(entries.begin(), entries.end(),
            [] (const TensorIndexEntry& left, const TensorIndexEntry& right) {
              return left.name < right.name;
            });
  for (auto& entry : entries) {
    // alias resolution: gather same-digest names (index is name-sorted)
    std::vector<std::string> aliases;
    for (const auto& other : entries) {
      if (other.contentDigest == entry.contentDigest) aliases.push_back(other.name);
    }
    entry.sharedReference = aliases.size() > 1 ? aliases.front() : "";
  }
  return entries;
}

void appendShapeJson(std::string& out, const std::vector<std::int64_t>& shape)
{
  out.push_back('[');
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::to_string(shape[i]);
  }
  out.push_back(']');
}

void appendSortedHexList(std::string& out, const std::vector<std::string>& hexes)
{
  std::vector<std::string> sorted = hexes;
  std::sort(sorted.begin(), sorted.end());
  out.push_back('[');
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) out.push_back(',');
    jsonString(out, sorted[i]);
  }
  out.push_back(']');
}

std::string graphFactsJson(const onnx::ModelProto& model,
                           const std::vector<TensorIndexEntry>& entries)
{
  // python _sha256_canonical(graph_facts): compact JSON, keys sorted at
  // every level, UTF-8 preserved, no whitespace.  Object members are emitted
  // in alphabetical order below so the wire equals the sorted-key dump.
  std::string out;
  out.reserve(1024);
  out.push_back('{');

  // "functions"
  out += "\"functions\":";
  {
    std::vector<std::string> hexes;
    hexes.reserve(static_cast<std::size_t>(model.functions_size()));
    for (int f = 0; f < model.functions_size(); ++f)
      hexes.push_back(protoHex(model.functions(f)));
    appendSortedHexList(out, hexes);
  }

  // "initializerLayout"
  out += ",\"initializerLayout\":[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& entry = entries[i];
    out += "{\"byteLength\":";
    out += std::to_string(entry.payload.content.size());
    out += ",\"byteOrder\":";
    jsonString(out, entry.payload.byteOrder);
    out += ",\"dtype\":";
    jsonString(out, entry.payload.dtype);
    out += ",\"shape\":";
    appendShapeJson(out, entry.payload.shape);
    out += ",\"sharedReference\":";
    jsonString(out, entry.sharedReference);
    out += ",\"tensorName\":";
    jsonString(out, entry.name);
    out.push_back('}');
  }
  out.push_back(']');

  // "irVersion"
  out += ",\"irVersion\":";
  out += std::to_string(static_cast<std::int64_t>(model.ir_version()));

  // "nodes"
  out += ",\"nodes\":[";
  for (int index = 0; index < model.graph().node_size(); ++index) {
    if (index != 0) out.push_back(',');
    const auto& node = model.graph().node(index);
    out += "{\"attributes\":[";
    {
      std::vector<const onnx::AttributeProto*> attributes;
      for (int a = 0; a < node.attribute_size(); ++a)
        attributes.push_back(&node.attribute(a));
      std::stable_sort(attributes.begin(), attributes.end(),
                [] (const onnx::AttributeProto* left, const onnx::AttributeProto* right) {
                  return left->name() < right->name();
                });
      for (std::size_t a = 0; a < attributes.size(); ++a) {
        if (a != 0) out.push_back(',');
        out += "{\"name\":";
        jsonString(out, attributes[a]->name());
        out += ",\"wire\":";
        jsonHexBytes(out, *attributes[a]);
        out.push_back('}');
      }
    }
    out += "],\"domain\":";
    jsonString(out, node.domain());
    out += ",\"index\":";
    out += std::to_string(static_cast<std::int64_t>(index));
    out += ",\"inputs\":[";
    for (int a = 0; a < node.input_size(); ++a) {
      if (a != 0) out.push_back(',');
      jsonString(out, node.input(a));
    }
    out += "],\"opType\":";
    jsonString(out, node.op_type());
    out += ",\"outputs\":[";
    for (int a = 0; a < node.output_size(); ++a) {
      if (a != 0) out.push_back(',');
      jsonString(out, node.output(a));
    }
    out += "]}";
  }
  out.push_back(']');

  // "opsets" sorted by (domain, version)
  out += ",\"opsets\":[";
  {
    std::vector<const onnx::OperatorSetIdProto*> opsets;
    for (int a = 0; a < model.opset_import_size(); ++a)
      opsets.push_back(&model.opset_import(a));
    std::stable_sort(opsets.begin(), opsets.end(),
              [] (const onnx::OperatorSetIdProto* left,
                  const onnx::OperatorSetIdProto* right) {
                if (left->domain() != right->domain()) return left->domain() < right->domain();
                return left->version() < right->version();
              });
    for (std::size_t a = 0; a < opsets.size(); ++a) {
      if (a != 0) out.push_back(',');
      out += "{\"domain\":";
      jsonString(out, opsets[a]->domain());
      out += ",\"version\":";
      out += std::to_string(static_cast<std::int64_t>(opsets[a]->version()));
      out.push_back('}');
    }
  }
  out.push_back(']');

  // "values"
  out += ",\"values\":{\"inputs\":[";
  for (int a = 0; a < model.graph().input_size(); ++a) {
    if (a != 0) out.push_back(',');
    jsonHexBytes(out, model.graph().input(a));
  }
  out += "],\"outputs\":[";
  for (int a = 0; a < model.graph().output_size(); ++a) {
    if (a != 0) out.push_back(',');
    jsonHexBytes(out, model.graph().output(a));
  }
  out += "],\"valueInfo\":";
  {
    std::vector<std::string> hexes;
    for (int v = 0; v < model.graph().value_info_size(); ++v)
      hexes.push_back(protoHex(model.graph().value_info(v)));
    appendSortedHexList(out, hexes);
  }
  out += "}}";
  return out;
}

std::string initializerContentJson(const std::vector<TensorIndexEntry>& entries)
{
  // ordered {tensorName, contentDigest} array; per-object keys sorted
  std::string out;
  out.push_back('[');
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += "{\"contentDigest\":";
    jsonString(out, entries[i].contentDigest);
    out += ",\"tensorName\":";
    jsonString(out, entries[i].name);
    out.push_back('}');
  }
  out.push_back(']');
  return out;
}

std::uint32_t revisionOfInlinedModel(const onnx::ModelProto& model)
{
  // Classification over participating top-level initializers of the inlined
  // model only: STRING, BFLOAT16 raw (incl. external which inlines to raw),
  // and typed COMPLEX select revision 2; all other maintained numeric
  // representations stay on revision 1.
  std::uint32_t revision = 1;
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    const auto& initializer = model.graph().initializer(i);
    if (initializer.data_type() == onnx::TensorProto::STRING) return 2;
    if (initializer.data_type() == onnx::TensorProto::BFLOAT16 &&
        !initializer.raw_data().empty())
      return 2;
    if ((initializer.data_type() == onnx::TensorProto::COMPLEX64 ||
         initializer.data_type() == onnx::TensorProto::COMPLEX128) &&
        initializer.raw_data().empty() &&
        (initializer.float_data_size() != 0 || initializer.double_data_size() != 0))
      return 2;
  }
  return revision;
}

} // namespace

NativeOnnxIdentity
canonicalOnnxSourceIdentity(const NativeCanonicalSource& source,
                            const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control);
  const auto entries = buildTensorIndex(model);
  NativeOnnxIdentity identity;
  identity.graphDigest = sha256HexOf(graphFactsJson(model, entries));
  identity.initializerDigest = sha256HexOf(initializerContentJson(entries));
  checkActive(control);
  return identity;
}

std::uint32_t
onnxInitializerNormalizationRevision(const NativeCanonicalSource& source,
                                     const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control);
  return revisionOfInlinedModel(model);
}

void
checkOnnxAssemblerDescriptorBinding(const std::string& assemblerDescriptorDigest,
                                    const NativeCanonicalSource& source,
                                    const NativeAssemblyControl& control)
{
  if (onnxInitializerNormalizationRevision(source, control) != 2) return;
  // v2-bound content must be explicitly declared by the recipe; there is no
  // silent substitution of a legacy descriptor (initializer normalization
  // compatibility matrix).
  static const std::string v2Descriptor =
    "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89";
  if (assemblerDescriptorDigest != v2Descriptor)
    failNormalization("NORMALIZATION_REVISION_REQUIRED");
}

} // namespace ndnsf::di
