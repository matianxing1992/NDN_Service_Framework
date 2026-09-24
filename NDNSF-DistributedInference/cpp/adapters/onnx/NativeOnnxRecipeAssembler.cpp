#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

// Spec 182 unifies the DI ONNX world on the official 1.17 full-protobuf
// headers (ONNX_USE_LITE_PROTO=OFF) installed by the configured ONNX prefix;
// the previous vendored lite trio no longer exists in this tree.
#include <onnx/checker.h>
#include <onnx/onnx_pb.h>
#include <onnx/shape_inference/implementation.h>

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
#include <onnxruntime_cxx_api.h>
#endif

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

[[noreturn]] void fail(const char* code);

std::string digest(const std::uint8_t* data, std::size_t size)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(data, size, hash);
  std::ostringstream out;
  out << "sha256:";
  for (unsigned char value : hash) {
    out << "0123456789abcdef"[value >> 4]
        << "0123456789abcdef"[value & 0x0f];
  }
  return out.str();
}

std::string digest(const std::vector<std::uint8_t>& bytes)
{
  return digest(bytes.data(), bytes.size());
}

std::string digestModelFile(const NativeOnnxModelFileInput& file)
{
  if (file.fd < 0 || file.bytes == 0 || file.digest.empty())
    fail("SOURCE_FILE");
  struct stat status{};
  if (fstat(file.fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != file.bytes)
    fail("SOURCE_FILE");
  if (lseek(file.fd, 0, SEEK_SET) < 0)
    fail("SOURCE_FILE");
  SHA256_CTX context;
  SHA256_Init(&context);
  std::array<std::uint8_t, 1U << 20> buffer{};
  std::uint64_t total = 0;
  for (;;) {
    const ssize_t count = read(file.fd, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      fail("SOURCE_FILE");
    }
    const auto received = static_cast<std::uint64_t>(count);
    if (received > file.bytes || total > file.bytes - received)
      fail("SOURCE_FILE");
    SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(count));
    total += received;
  }
  if (total != file.bytes || lseek(file.fd, 0, SEEK_SET) < 0)
    fail("SOURCE_FILE");
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256_Final(hash, &context);
  std::ostringstream out;
  out << "sha256:";
  for (unsigned char value : hash) {
    out << "0123456789abcdef"[value >> 4]
        << "0123456789abcdef"[value & 0x0f];
  }
  const auto computed = out.str();
  if (computed != file.digest)
    fail("SOURCE_DIGEST");
  return computed;
}

[[noreturn]] void fail(const char* code)
{
  throw std::runtime_error(std::string("DI_NATIVE_ONNX_") + code);
}

[[noreturn]] void failGraph(const char* stage, const std::exception& error)
{
  std::string detail = error.what();
  for (char& value : detail) {
    if (value == '\r' || value == '\n' || value == '\t') value = ' ';
    else if (static_cast<unsigned char>(value) < 0x20) value = ' ';
  }
  if (detail.size() > 512) detail.resize(512);
  logRuntimeEvidence(std::string("NDNSF_DI_NATIVE_ONNX_GRAPH_DETAIL stage=") + stage + " error=" + detail);
  fail("GRAPH");
}

[[noreturn]] void failGraphStage(const char* stage)
{
  logRuntimeEvidence(std::string("NDNSF_DI_NATIVE_ONNX_GRAPH_DETAIL stage=") + stage);
  fail("GRAPH");
}

bool
usesOrtLegacyDefaultDomainOp(const onnx::ModelProto& model,
                              const NativeCertifiedRecipe& recipe)
{
  if (recipe.adapterId != "onnx" || recipe.backend != "onnxruntime" ||
      recipe.backendAbi != "onnxruntime-cpu-v1" || !model.has_graph())
    return false;
  for (const auto& node : model.graph().node()) {
    if (node.domain().empty() && node.op_type() == "SimplifiedLayerNormalization")
      return true;
  }
  return false;
}

class ScopedOrtCompatibilityCheckerDomains
{
public:
  ScopedOrtCompatibilityCheckerDomains(onnx::ModelProto& model,
                                       const NativeCertifiedRecipe& recipe)
  {
    if (!usesOrtLegacyDefaultDomainOp(model, recipe)) return;
    for (auto& node : *model.mutable_graph()->mutable_node()) {
      if (node.domain().empty() && node.op_type() == "SimplifiedLayerNormalization") {
        node.set_domain("com.microsoft");
        m_normalized.push_back(&node);
      }
    }
  }

  ~ScopedOrtCompatibilityCheckerDomains() noexcept
  {
    for (auto* node : m_normalized) node->clear_domain();
  }

  bool active() const noexcept { return !m_normalized.empty(); }

private:
  std::vector<onnx::NodeProto*> m_normalized;
};

void
checkNativeOnnxModel(onnx::ModelProto& model, const NativeCertifiedRecipe& recipe)
{
  ScopedOrtCompatibilityCheckerDomains compatibility(model, recipe);
  if (compatibility.active()) {
    // The fixed ORT profile contains a legacy default-domain ORT operator that
    // the generic checker cannot resolve. Keep structural checker coverage;
    // the native shape and ORT checks remain mandatory below.
    onnx::checker::check_model(model, false, false, false);
  }
  else {
    onnx::checker::check_model(model, true);
  }
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

/**
 * Keep the certified model on disk while ONNX Runtime constructs its own
 * graph/session representation.  Loading from the in-memory vector leaves
 * the serialized model alive beside ORT's copy and is particularly expensive
 * for a cold multi-provider assembly.  The file is private, unlinked on every
 * exit path, and read back only after the ORT session has been destroyed so
 * the worker still returns the exact bytes validated by the parent protocol.
 */
class ScopedOnnxRuntimeModelFile
{
public:
  explicit ScopedOnnxRuntimeModelFile(const std::vector<std::uint8_t>& bytes)
  {
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/ndnsf-di-onnx-XXXXXX");
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_CREATE");
    }
    m_path = pattern.data();
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
      ::close(fd);
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_PERMISSIONS");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) continue;
      ::close(fd);
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_WRITE");
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_FLUSH");
    }
  }

  ~ScopedOnnxRuntimeModelFile() noexcept
  {
    cleanup();
  }

  ScopedOnnxRuntimeModelFile(const ScopedOnnxRuntimeModelFile&) = delete;
  ScopedOnnxRuntimeModelFile& operator=(const ScopedOnnxRuntimeModelFile&) = delete;

  const char* c_str() const noexcept { return m_path.c_str(); }

  std::vector<std::uint8_t> read(std::uint64_t maxBytes) const
  {
    std::error_code error;
    const auto size = std::filesystem::file_size(m_path, error);
    if (error || size == 0 || size > maxBytes) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    std::ifstream input(m_path, std::ios::binary);
    if (!input.good()) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    return bytes;
  }

private:
  void cleanup() noexcept
  {
    if (!m_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(m_path, ignored);
      m_path.clear();
    }
  }

private:
  std::string m_path;
};

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
  if (length > bytes.size() - offset || length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
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

std::vector<std::uint8_t> deterministicWire(const onnx::ModelProto& model,
                                            std::uint64_t limit)
{
  const auto size = model.ByteSizeLong();
  if (size == 0 || size > limit || size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!model.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return bytes;
}

// ---------------------------------------------------------------------------
// S5/S6 certified extraction (OA07/OA08).  This port mirrors onnx 1.17
// python utils.py::Extractor running on an InferShapes copy of the inlined
// source, plus executor.py::assemble_certified_onnx_model S6 cover and io
// comparisons.  All failures stay in the registered DI_NATIVE_ONNX_* family.
// ---------------------------------------------------------------------------

std::string deterministicMessageBytes(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> deterministicMessageVector(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  // Keep the large material-backed model in one serialization buffer.  The
  // previous string-then-vector path retained two full copies until the
  // temporary string was destroyed, which inflated the first Provider's
  // working set immediately before the worker boundary.
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return bytes;
}

// Reverse DFS from one requested output tensor, stopping at graph input and
// declared role-boundary names; node producers are exact matches on the node
// output list, exactly like utils.py _dfs_search_reachable_nodes.  Every
// recursion either stops or moves at least one index from `unreachable` into
// `reachable`, so the depth is bounded by the (recipe-capped) node count even
// on cyclic graphs.
void
dfsReachNodes(const std::string& outputName,
              const std::unordered_set<std::string>& graphInputNames,
              const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes,
              std::unordered_set<std::size_t>& unreachable,
              std::unordered_set<std::size_t>& reachable)
{
  if (graphInputNames.count(outputName) != 0) return;  // extraction boundary
  std::vector<std::size_t> producers;
  for (const std::size_t index : unreachable) {
    const auto& outs = nodes.Get(static_cast<int>(index)).output();
    if (std::find(outs.begin(), outs.end(), outputName) != outs.end())
      producers.push_back(index);
  }
  for (const std::size_t index : producers) {
    reachable.insert(index);
    unreachable.erase(index);
  }
  for (const std::size_t index : producers) {
    for (const auto& input : nodes.Get(static_cast<int>(index)).input())
      dfsReachNodes(input, graphInputNames, nodes, unreachable, reachable);
  }
}

// Local functions reachable from the selected nodes, in first-reference
// order, including functions referenced from function bodies
// (utils.py _collect_referred_local_functions).  A node refers the first
// model function whose name/domain pair matches its op_type/domain.
std::vector<const onnx::FunctionProto*>
referredLocalFunctions(const onnx::ModelProto& model,
                       const std::vector<std::size_t>& reachable,
                       const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes)
{
  std::vector<const onnx::FunctionProto*> referred;
  std::vector<const onnx::NodeProto*> frontier;
  frontier.reserve(reachable.size());
  for (const std::size_t index : reachable)
    frontier.push_back(&nodes.Get(static_cast<int>(index)));
  while (!frontier.empty()) {
    std::vector<const onnx::NodeProto*> children;
    for (const onnx::NodeProto* node : frontier) {
      const onnx::FunctionProto* match = nullptr;
      for (const auto& fn : model.functions()) {
        if (fn.name() == node->op_type() && fn.domain() == node->domain()) {
          match = &fn;
          break;
        }
      }
      if (match == nullptr ||
          std::find(referred.begin(), referred.end(), match) != referred.end())
        continue;
      referred.push_back(match);
      for (int i = 0; i < match->node_size(); ++i)
        children.push_back(&match->node(i));
    }
    frontier.swap(children);
  }
  return referred;
}

// Boundary value_infos in the requested order: a name that is an original
// graph io keeps that entry, an internal name comes from the shape-inferred
// value_info, and a name in neither is an extraction failure (the python
// reference hits a KeyError there; both reject).
std::vector<const onnx::ValueInfoProto*>
collectBoundaryIo(const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& originalIo,
                  const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& inferredValueInfo,
                  const std::vector<NativeAssemblyTensorContractV3>& requested)
{
  std::unordered_map<std::string, const onnx::ValueInfoProto*> original;
  for (const auto& value : originalIo) original[value.name()] = &value;
  std::unordered_map<std::string, const onnx::ValueInfoProto*> inferred;
  for (const auto& value : inferredValueInfo) inferred[value.name()] = &value;
  std::vector<const onnx::ValueInfoProto*> result;
  result.reserve(requested.size());
  for (const auto& contract : requested) {
    const auto* value = original.count(contract.name) != 0
      ? original[contract.name]
      : (inferred.count(contract.name) != 0 ? inferred[contract.name] : nullptr);
    if (value == nullptr) failGraphStage("boundary-io");
    result.push_back(value);
  }
  return result;
}

// Rebuild of the extractor metadata, field set exactly like the python
// 1.17 make_model(graph, ...) call: ir_version, opset_import copies,
// producer_name, then a graph holding name, selected nodes in original
// order, referenced initializers and inferred value_info in original order,
// and the boundary io; sparse initializers and quantization annotations are
// rejections (python _collect_reachable_tensors).  No other source metadata
// is copied.
onnx::ModelProto
makeExtractedModel(onnx::ModelProto& inferredSource,
                   const std::vector<std::size_t>& reachable,
                   const std::vector<const onnx::ValueInfoProto*>& inputs,
                   const std::vector<const onnx::ValueInfoProto*>& outputs,
                   const std::vector<const onnx::FunctionProto*>& functions)
{
  const auto& graph = inferredSource.graph();
  std::unordered_set<std::string> keepNames;
  for (const std::size_t index : reachable) {
    const auto& node = graph.node(static_cast<int>(index));
    for (const auto& input : node.input()) keepNames.insert(input);
    for (const auto& output : node.output()) keepNames.insert(output);
  }
  if (graph.sparse_initializer_size() != 0 ||
      graph.quantization_annotation_size() != 0)
    failGraphStage("unsupported-graph-fields");
  onnx::ModelProto result;
  result.set_ir_version(inferredSource.ir_version());
  result.set_producer_name("onnx.utils.extract_model");
  for (const auto& opset : inferredSource.opset_import())
    *result.add_opset_import() = opset;
  auto* outGraph = result.mutable_graph();
  outGraph->set_name("Extracted from {" + graph.name() + "}");
  for (const std::size_t index : reachable)
    *outGraph->add_node() = graph.node(static_cast<int>(index));
  for (int i = 0; i < graph.initializer_size(); ++i) {
    const auto& initializer = graph.initializer(i);
    if (keepNames.count(initializer.name()) == 0)
      continue;
    // The source graph is discarded immediately after extraction. Transfer
    // the selected TensorProto instead of copying its potentially GiB-sized
    // raw_data string while the source graph is still resident.
    auto* output = outGraph->add_initializer();
    output->Swap(inferredSource.mutable_graph()->mutable_initializer(i));
  }
  for (const auto& value : graph.value_info())
    if (keepNames.count(value.name()) != 0)
      *outGraph->add_value_info() = value;
  for (const auto* value : inputs) *outGraph->add_input() = *value;
  for (const auto* value : outputs) *outGraph->add_output() = *value;
  for (const auto* fn : functions) *result.add_functions() = *fn;
  return result;
}

// executor.py S6 dtype label: the pinned _ONNX_DTYPE_NAMES table; element
// types outside the table compare as their decimal string, so the label
// helper returns nullptr and the caller falls back to to_string.
const char*
onnxDtypeLabel(std::int32_t elemType)
{
  switch (elemType) {
    case 1: return "float32";
    case 2: return "uint8";
    case 3: return "int8";
    case 4: return "uint16";
    case 5: return "int16";
    case 6: return "int32";
    case 7: return "int64";
    case 9: return "bool";
    case 10: return "float16";
    case 11: return "float64";
    case 12: return "uint32";
    case 13: return "uint64";
    case 16: return "bfloat16";
    default: return nullptr;
  }
}

std::string normalizeShapeDimText(const std::string& value)
{
  // python normalize_shape_dimension: int-parseable decimal text compares
  // numerically with ints; symbolic text stays raw.
  if (!value.empty()) {
    std::size_t consumed = 0;
    try {
      const long long parsed = std::stoll(value, &consumed);
      if (consumed == value.size())
        return std::to_string(parsed);
    }
    catch (const std::exception&) {}
  }
  return value;
}

// S6 io semantic comparison against the recipe contracts, with the python
// dtype normalization (_ONNX_DTYPE_NAMES + decimal fallback) and the
// normalize_shape_dimension dimension semantics.  A missing expected name
// is an IO_CONTRACT rejection; any dtype/shape difference is IO_DTYPE.
void
compareBoundaryContracts(const std::vector<NativeAssemblyTensorContractV3>& expected,
                         const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& assembledIo)
{
  std::unordered_map<std::string, const onnx::ValueInfoProto*> byName;
  for (const auto& value : assembledIo) byName[value.name()] = &value;
  for (const auto& contract : expected) {
    const auto found = byName.find(contract.name);
    if (found == byName.end()) fail("IO_CONTRACT");
    const auto& typeProto = found->second->type();
    if (!typeProto.has_tensor_type()) fail("IO_DTYPE");
    const auto& tensorType = typeProto.tensor_type();
    std::string expectedDtype = contract.dtype;
    if (!expectedDtype.empty() &&
        std::all_of(expectedDtype.begin(), expectedDtype.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
      char* end = nullptr;
      const long parsed = std::strtol(expectedDtype.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && parsed >= 0 &&
          static_cast<std::uint64_t>(parsed) <=
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        const char* label = onnxDtypeLabel(static_cast<int>(parsed));
        if (label != nullptr) expectedDtype = label;
      }
    }
    const char* observedLabel = onnxDtypeLabel(tensorType.elem_type());
    const std::string observedDtype = observedLabel != nullptr
      ? std::string(observedLabel) : std::to_string(tensorType.elem_type());
    if (observedDtype != expectedDtype) fail("IO_DTYPE");
    const auto& dims = tensorType.shape().dim();
    if (static_cast<std::size_t>(dims.size()) != contract.shape.size()) fail("IO_DTYPE");
    for (int i = 0; i < dims.size(); ++i) {
      const std::string observedDim = dims.Get(i).has_dim_value()
        ? std::to_string(dims.Get(i).dim_value()) : dims.Get(i).dim_param();
      const auto& dimension = contract.shape[i];
      const auto expectedDim = std::holds_alternative<std::int64_t>(dimension)
        ? std::to_string(std::get<std::int64_t>(dimension)) : std::get<std::string>(dimension);
      if (normalizeShapeDimText(observedDim) != normalizeShapeDimText(expectedDim))
        fail("IO_DTYPE");
    }
  }
}

} // namespace

namespace {

// Shared certified-assembly chain (the OA04 core).  One implementation runs
// the S1-S7 gates for both entry styles: OA01 (in-process provider calls with
// control caps and a cancellation-checking onRound) and the bounded worker
// child (certified recipe caps from the metadata, no-op onRound).  onRound is
// invoked at every deadline/cancellation checkpoint exactly where the old
// in-process chain called checkActive(control).
NativeOnnxIdentity
canonicalOnnxModelIdentity(const onnx::ModelProto& model,
                           const NativeAssemblyControl& control,
                           const std::vector<std::uint8_t>* externalBytes = nullptr);

void materializeShapeInferenceInitializers(
  onnx::ModelProto& inferred,
  const onnx::ModelProto& sourceModel,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl& control,
  std::uint64_t* materializedBudget);

void materializeSelectedExternalInitializers(
  onnx::ModelProto& model,
  const std::vector<std::size_t>& selectedNodes,
  const std::vector<std::uint8_t>& externalBytes,
  const NativeAssemblyControl& control);

onnx::ModelProto
ownedSourceModel(const NativeCanonicalSource& source,
                 const NativeAssemblyControl& control,
                 bool inlineExternal = true,
                 std::uint64_t* materializedBudget = nullptr,
                 const NativeOnnxModelFileInput* modelFile = nullptr);

NativeCertifiedAssembly
assembleCertifiedOnnxChain(const NativeCanonicalSource& source,
                           const NativeCertifiedRecipe& recipe,
                           std::uint64_t maxSourceBytes,
                           std::uint64_t maxAssembledBytes,
                           const std::function<void()>& onRound,
                           NativeCanonicalSource* sourceToReleaseAfterParse,
                           bool loadRuntimeSession,
                           const NativeOnnxModelFileInput* modelFile = nullptr)
{
  onRound();
  const std::uint64_t sourceBytes = modelFile != nullptr
    ? modelFile->bytes : static_cast<std::uint64_t>(source.modelBytes.size());
  if (maxSourceBytes == 0 || maxAssembledBytes == 0 ||
      sourceBytes == 0 || sourceBytes > maxSourceBytes ||
      (modelFile == nullptr &&
       source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    fail("SOURCE_LIMIT");
  if (modelFile != nullptr && !source.modelBytes.empty())
    fail("SOURCE_OWNERSHIP");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > maxSourceBytes ||
       source.initializerBytes->size() >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
       checkedAdd(sourceBytes, source.initializerBytes->size()) >
         checkedAdd(maxSourceBytes, maxSourceBytes)))
    fail("INITIALIZER_LIMIT");
  if (recipe.adapterId.empty() || recipe.backend.empty() || recipe.roleKind.empty() ||
      recipe.nodeIndices.empty() || recipe.maxNodes == 0 ||
      recipe.nodeIndices.size() > recipe.maxNodes) fail("RECIPE");
  std::set<std::string> inputNames;
  std::set<std::string> outputNames;
  validateContracts(recipe.expectedInputs, inputNames);
  validateContracts(recipe.expectedOutputs, outputNames);
  if (inputNames.empty() || outputNames.empty()) fail("IO_CONTRACT");

  // OA04 materialized-role certificate path.  The parent has already
  // authenticated the manifest, selected node payloads, initializer payloads,
  // role boundary, deterministic wire, and role graph/initializer identity in
  // materializeNativeCanonicalModel/canonicalOnnxSourceIdentity.  It then
  // writes that exact byte vector through an atomic staging file.  Re-parsing
  // the same 752 MiB role in the child would recreate the complete protobuf
  // graph and run the second full checker without adding an independent
  // authorization decision; run-46 showed that copy as the first owned-swap
  // boundary beside another Provider's resident runner.
  //
  // Keep the child-side certificate bounded to the immutable file identity and
  // the recipe/contract shape.  The parent reads the file only after the child
  // has exited, and the Provider creates the single authoritative ORT session
  // afterwards.  Inline callers and the ORT-enabled parity entry retain the
  // full parse/check path below.
  if (modelFile != nullptr && recipe.materializedRole && !loadRuntimeSession) {
    if (source.initializerBytes)
      fail("EXTERNAL_BINDING");
    if (modelFile->bytes > maxAssembledBytes)
      fail("SERIALIZE_LIMIT");
    for (std::size_t index = 0; index < recipe.nodeIndices.size(); ++index) {
      if (recipe.nodeIndices[index] != index)
        fail("NODE_COVER");
    }
    const auto modelDigest = digestModelFile(*modelFile);
    onRound();
    NativeCertifiedAssembly result;
    result.modelDigest = modelDigest;
    result.nodeCount = recipe.nodeIndices.size();
    for (const auto& contract : recipe.expectedInputs)
      result.inputNames.push_back(contract.name);
    for (const auto& contract : recipe.expectedOutputs)
      result.outputNames.push_back(contract.name);
    return result;
  }

  NativeAssemblyControl sourceControl;
  sourceControl.deadline = std::chrono::steady_clock::time_point::max();
  sourceControl.requireActive = onRound;
  sourceControl.maxSourceBytes = maxSourceBytes;
  sourceControl.maxAssembledBytes = maxAssembledBytes;
  // The cold canonical-source path must present the same owned, in-memory
  // model to ONNX full-checking that the old Python/native preparation path
  // validated.  ONNX checker resolves EXTERNAL TensorProto locations through
  // `model.onnx.data`; Provider memory has no such filesystem object.  The
  // material-backed path is handled separately below and never enters this
  // full-source copy: it already carries a selected role model rebuilt from
  // authenticated material payloads.
  const auto* externalBytes = source.initializerBytes
    ? &source.initializerBytes->asVector() : nullptr;
  onnx::ModelProto original = ownedSourceModel(source, sourceControl, true,
                                               nullptr, modelFile);
  if (!original.has_graph() || original.graph().node_size() == 0 ||
      original.graph().node_size() > static_cast<int>(recipe.maxNodes))
    failGraphStage("source-graph");

  std::set<std::uint64_t> selected;
  for (const auto index : recipe.nodeIndices) {
    if (index >= static_cast<std::uint64_t>(original.graph().node_size()) ||
        !selected.insert(index).second) fail("NODE_COVER");
  }
  // COMPONENT_SET identifies an adapter-defined, exact node set.  It does
  // not mean that the role must own the whole canonical source graph: a
  // semantic component may be a certified subset of a larger model.  The
  // selected set is checked against the extracted graph below, where every
  // reachable node and every certified index must agree byte-for-byte.
  if (recipe.roleKind != "COMPONENT_SET" &&
      (recipe.layerEnd <= recipe.layerBegin ||
       recipe.layerEnd > static_cast<std::uint64_t>(original.graph().node_size())))
    fail("LAYER_RANGE");

  // S3: a complete canonical source must pass the full checker before any
  // certificate comparison (executor.py check_model(model, full_check=True)).
  // A post-Selection materialized role is intentionally a compact graph: its
  // selected nodes may expose an inter-role tensor and cannot retain the
  // complete source graph's final outputs.  Its authenticated manifest and
  // recipe therefore bind a separate worker path; shape inference, boundary
  // extraction, assembled checking and ORT loading below remain mandatory.
  if (!recipe.materializedRole) {
    try {
      checkNativeOnnxModel(original, recipe);
    }
    catch (const std::exception& error) {
      failGraph("canonical-check", error);
    }
  }

  // S4: compare the authenticated graph/initializer identity without making
  // a protobuf-owned copy of the external bytes.
  const auto identity = canonicalOnnxModelIdentity(original, sourceControl,
                                                    externalBytes);
  if (identity.graphDigest != recipe.graphDigest ||
      identity.initializerDigest != recipe.canonicalInitializerDigest)
    fail("RECIPE");
  onRound();

  // S5: retain the authenticated materialized role directly, or infer shapes
  // and extract the selected role from a canonical source.  The latter path
  // materializes only shape-value initializers; ordinary Qwen weight tensors
  // remain external until the selected role is extracted.  A deep
  // `inferred = original` copy here duplicates the complete source graph and
  // its external-backed tensors before extraction, which is precisely the
  // peak this bounded worker is intended to avoid.
  std::vector<std::string> certifiedNodeBytes;
  certifiedNodeBytes.reserve(recipe.nodeIndices.size());
  for (const auto index : recipe.nodeIndices) {
    certifiedNodeBytes.push_back(deterministicMessageBytes(
      original.graph().node(static_cast<int>(index))));
  }
  onnx::ModelProto assembled;
  if (recipe.materializedRole) {
    // A materialized role was rebuilt from authenticated selected node and
    // initializer payloads immediately before this worker was spawned.  The
    // parent has already bound its graph/initializer identity to that role
    // model, and the recipe is rewritten to the compact contiguous cover.
    // Re-running source shape inference and reachability extraction here
    // creates a second model-sized protobuf graph without adding a new
    // authorization or integrity check.  Keep the identity, node-byte,
    // contract, full-check and ORT checks below; only reuse this already
    // certified role graph.
    for (std::size_t index = 0; index < recipe.nodeIndices.size(); ++index) {
      if (recipe.nodeIndices[index] != index)
        fail("NODE_COVER");
    }
    if (original.graph().node_size() !=
        static_cast<int>(recipe.nodeIndices.size()))
      fail("NODE_COVER");
    assembled.Swap(&original);
  }
  else {
      materializeShapeInferenceInitializers(
        original, original, externalBytes, sourceControl, nullptr);
      try {
        onnx::shape_inference::InferShapes(original);
      }
      catch (const std::exception& error) {
        failGraph("shape-inference", error);
      }
      onRound();
      const auto& inferredGraph = original.graph();
      std::unordered_set<std::string> extractionBoundaryNames;
      for (const auto& input : inferredGraph.input()) extractionBoundaryNames.insert(input.name());
      for (const auto& contract : recipe.expectedInputs)
        extractionBoundaryNames.insert(contract.name);
      std::unordered_set<std::size_t> unreachable;
      for (int i = 0; i < inferredGraph.node_size(); ++i)
        unreachable.insert(static_cast<std::size_t>(i));
      std::unordered_set<std::size_t> reachable;
      for (const auto& contract : recipe.expectedOutputs)
        dfsReachNodes(contract.name, extractionBoundaryNames, inferredGraph.node(),
                      unreachable, reachable);
      std::vector<std::size_t> selectedNodes;
      // The certified recipe owns the complete node cover for this role.  Output
      // reachability still determines the required execution subgraph, but a
      // valid source may contain certified side-effect-free nodes that are not on
      // a path to one of the declared outputs.  Retain those explicitly selected
      // nodes so the assembled graph remains byte-identical to the authenticated
      // cover instead of silently changing the recipe identity.
      std::set<std::size_t> selectedAndReachable(reachable.begin(), reachable.end());
      selectedAndReachable.insert(selected.begin(), selected.end());
      selectedNodes.reserve(selectedAndReachable.size());
      for (const std::size_t index : selectedAndReachable) selectedNodes.push_back(index);
      std::sort(selectedNodes.begin(), selectedNodes.end());  // original order
      if (externalBytes != nullptr) {
        materializeSelectedExternalInitializers(
          original, selectedNodes, *externalBytes, sourceControl);
      }
      // The selected TensorProto values now own their authenticated bytes, so
      // the worker request buffer can be scrubbed before serialization and ORT
      // load. This is the key cache-compatible memory boundary.
      if (sourceToReleaseAfterParse != nullptr) {
        if (sourceToReleaseAfterParse != &source)
          fail("SOURCE_OWNERSHIP");
        std::fill(sourceToReleaseAfterParse->modelBytes.begin(),
                  sourceToReleaseAfterParse->modelBytes.end(), 0);
        std::vector<std::uint8_t>{}.swap(sourceToReleaseAfterParse->modelBytes);
        if (sourceToReleaseAfterParse->initializerBytes) {
          auto& initializerBytes = sourceToReleaseAfterParse->initializerBytes->asVector();
          if (!initializerBytes.empty())
            OPENSSL_cleanse(initializerBytes.data(), initializerBytes.size());
          std::vector<std::uint8_t>{}.swap(initializerBytes);
          sourceToReleaseAfterParse->initializerBytes.reset();
        }
        sourceToReleaseAfterParse->initializerRangeSource.reset();
      }
      const auto inputs = collectBoundaryIo(inferredGraph.input(),
                                            inferredGraph.value_info(),
                                            recipe.expectedInputs);
      const auto outputs = collectBoundaryIo(inferredGraph.output(),
                                             inferredGraph.value_info(),
                                             recipe.expectedOutputs);
      const auto functions = referredLocalFunctions(original, selectedNodes,
                                                    inferredGraph.node());
      assembled = makeExtractedModel(original, selectedNodes,
                                     inputs, outputs, functions);
  }
  if (sourceToReleaseAfterParse != nullptr && recipe.materializedRole) {
    if (sourceToReleaseAfterParse != &source)
      fail("SOURCE_OWNERSHIP");
    std::fill(sourceToReleaseAfterParse->modelBytes.begin(),
              sourceToReleaseAfterParse->modelBytes.end(), 0);
    std::vector<std::uint8_t>{}.swap(sourceToReleaseAfterParse->modelBytes);
    if (sourceToReleaseAfterParse->initializerBytes) {
      auto& initializerBytes = sourceToReleaseAfterParse->initializerBytes->asVector();
      if (!initializerBytes.empty())
        OPENSSL_cleanse(initializerBytes.data(), initializerBytes.size());
      std::vector<std::uint8_t>{}.swap(initializerBytes);
      sourceToReleaseAfterParse->initializerBytes.reset();
    }
    sourceToReleaseAfterParse->initializerRangeSource.reset();
  }

  // `original` still owns the complete authenticated source graph and all of
  // its initializers after extraction.  Keeping it alive through the second
  // checker, wire serialization, and ORT session load makes the worker retain
  // the full source plus the selected role simultaneously.  Extraction has
  // copied every field needed by S6/S7, so release that graph before entering
  // the final validation/runtime phase.  The scope ensures protobuf storage is
  // actually destroyed rather than merely cleared and retained by an arena.
  {
    onnx::ModelProto releasedOriginal;
    original.Swap(&releasedOriginal);
  }

  // S6: the assembled model passes the full checker again, its node cover
  // equals the certified indices with byte-identical nodes, and its io
  // matches the recipe contracts semantically (executor.py S6 block).
  try {
    checkNativeOnnxModel(assembled, recipe);
  }
  catch (const std::exception& error) {
    failGraph("assembled-check", error);
  }
  onRound();
  if (assembled.graph().node_size() != static_cast<int>(recipe.nodeIndices.size()))
    fail("NODE_COVER");
  for (std::size_t i = 0; i < recipe.nodeIndices.size(); ++i) {
    const auto& expectedBytes = certifiedNodeBytes[i];
    const auto assembledBytes = deterministicMessageBytes(
      assembled.graph().node(static_cast<int>(i)));
    if (expectedBytes != assembledBytes) fail("NODE_COVER");
  }
  compareBoundaryContracts(recipe.expectedInputs, assembled.graph().input());
  compareBoundaryContracts(recipe.expectedOutputs, assembled.graph().output());
  onRound();

  // S7: deterministic wire within the resource envelope.  The public
  // in-process parity path also loads a real CPU session here, while the
  // production worker skips that duplicate load: its parent creates and
  // warms the authoritative OnnxRuntimeModelRunner before RUNNER_READY.
  // Keeping the switch at this internal seam preserves the parity helper
  // without retaining a second model-sized ORT graph in the worker.
  // A materialized role is already an authenticated, deterministic ONNX file.
  // The file-backed worker has parsed that file and completed S1-S6 against
  // it, while the parent will read the same immutable file after the child
  // exits.  Serializing the protobuf once more here would create a second
  // model-sized buffer solely to rediscover the file digest, which is the
  // cold-worker peak this path is intended to avoid.  Inline/parity callers
  // still take the original deterministic serialization path.
  std::vector<std::uint8_t> bytes;
  std::string assembledDigest;
  if (modelFile != nullptr) {
    if (!recipe.materializedRole || modelFile->bytes == 0 ||
        modelFile->bytes > maxAssembledBytes || modelFile->digest.empty())
      fail("SERIALIZE_LIMIT");
    assembledDigest = modelFile->digest;
  }
  else {
    bytes = deterministicWire(assembled, maxAssembledBytes);
    assembledDigest = digest(bytes);
  }
  {
    onnx::ModelProto releasedAssembled;
    assembled.Swap(&releasedAssembled);
  }
  onRound();
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  if (loadRuntimeSession) {
    try {
      // Loading from a byte vector keeps the complete serialized role model
      // resident while ORT builds its graph/session representation.  Stage the
      // already-certified bytes privately, release the vector, and read it back
      // only after the validation session is destroyed.  The returned bytes and
      // digest therefore remain identical to the protocol artifact.
      ScopedOnnxRuntimeModelFile stagedModel(bytes);
      std::vector<std::uint8_t>{}.swap(bytes);
      {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-certified-assembly");
        Ort::SessionOptions options;
        // Match the production OnnxRuntimeModelRunner's bounded CPU session.
        // The default ORT intra-op pool allocates per-thread scratch state while
        // validating a model-sized role and needlessly raises the cold-worker
        // peak beside another Provider's resident runner.
        options.SetIntraOpNumThreads(1);
        // Do not retain a second prepacked weight copy while another Provider's
        // runner is resident.  The production runner uses the same setting;
        // this validation session must exercise the same bounded CPU contract.
        options.AddConfigEntry("session.disable_prepacking", "1");
        // This worker only proves that the certified bytes can create a real CPU
        // session; S6 has already performed the full graph/IO checks.  Do not
        // build an optimized execution graph here: the selected Provider creates
        // the production BASIC session after the worker exits, and performing
        // BASIC optimization twice is the cold-start peak seen with two roles.
        options.SetGraphOptimizationLevel(ORT_DISABLE_ALL);
        Ort::Session session(env, stagedModel.c_str(), options);
      }
      bytes = stagedModel.read(maxAssembledBytes);
    }
    catch (const std::exception& error) {
      failGraph("ort-session", error);
    }
  }
#else
  if (loadRuntimeSession) {
    // The ONNX graph/protobuf helpers remain linkable in a disabled build, but
    // the certified in-process parity gate requires the optional ORT runtime.
    fail("RUNTIME_UNAVAILABLE");
  }
#endif
  onRound();
  NativeCertifiedAssembly result;
  result.modelDigest = assembledDigest;
  result.modelBytes = std::move(bytes);
  result.nodeCount = recipe.nodeIndices.size();
  for (const auto& contract : recipe.expectedInputs) result.inputNames.push_back(contract.name);
  for (const auto& contract : recipe.expectedOutputs) result.outputNames.push_back(contract.name);
  return result;
}

} // namespace

NativeCertifiedAssembly
assembleNativeCertifiedOnnxModel(const NativeCanonicalSource& source,
                                 const NativeCertifiedRecipe& recipe,
                                 const NativeAssemblyControl& control)
{
  // OA01 entry: the caller keeps a valid cancellation/deadline control and
  // its own resource budget; every round of the shared chain re-checks it.
  checkActive(control);
  return assembleCertifiedOnnxChain(source, recipe, control.maxSourceBytes,
                                    control.maxAssembledBytes,
                                    [&control] { checkActive(control); }, nullptr,
                                    true);
}

NativeCertifiedAssembly
assembleInProcess(const NativeCanonicalSource& source,
                  const NativeCertifiedRecipe& recipe,
                  NativeCanonicalSource* sourceToReleaseAfterParse,
                  const NativeOnnxModelFileInput* modelFile)
{
  // OA04: the bounded worker child runs the identical chain against the
  // certified recipe budget with no cancellation callback of its own; the
  // parent transport owns cancellation by killing this process.  The worker
  // performs structural/digest/IO validation; the parent owns the single
  // authoritative ORT load and warmup before publishing RUNNER_READY.
  return assembleCertifiedOnnxChain(source, recipe, recipe.maxSourceBytes,
                                    recipe.maxAssembledBytes, [] {},
                                    sourceToReleaseAfterParse, false, modelFile);
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
    std::set<std::string> metadataKeys;
    std::string location;
    int locationEntries = 0;
    for (int i = 0; i < tensor->external_data_size(); ++i) {
      const auto& entry = tensor->external_data(i);
      if (!metadataKeys.insert(entry.key()).second) fail("EXTERNAL_METADATA");
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

struct ExternalByteRange
{
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

ExternalByteRange validatedExternalByteRange(const onnx::TensorProto& tensor,
                                             const std::vector<std::uint8_t>& bytes)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL)
    fail("EXTERNAL_BINDING");
  std::set<std::string> metadataKeys;
  std::string location;
  int locationEntries = 0;
  std::string offsetText;
  std::string lengthText;
  for (const auto& entry : tensor.external_data()) {
    if (!metadataKeys.insert(entry.key()).second) fail("EXTERNAL_METADATA");
    if (entry.key() == "location") {
      location = entry.value();
      ++locationEntries;
    } else if (entry.key() == "offset") {
      offsetText = entry.value();
    } else if (entry.key() == "length") {
      lengthText = entry.value();
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
  const auto offset = offsetText.empty() ? 0 : parseUint(offsetText, "OFFSET");
  if (offset > bytes.size()) fail("EXTERNAL_RANGE");
  std::uint64_t length = bytes.size() - offset;
  if (!lengthText.empty()) {
    length = parseUint(lengthText, "LENGTH");
    // ONNX 1.17 treats an explicit zero length as the remainder of the
    // authenticated external file. Keep this rule identical to inlining.
    if (length == 0) length = bytes.size() - offset;
  }
  if (length > bytes.size() - offset ||
      length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("EXTERNAL_RANGE");
  return {offset, length};
}

void inlineValidatedExternals(const std::vector<onnx::TensorProto*>& tensors,
                              const std::vector<std::uint8_t>& bytes,
                              const NativeAssemblyControl* control = nullptr,
                              std::uint64_t* copiedBytes = nullptr)
{
  for (auto* tensor : tensors) {
    if (control != nullptr) checkActive(*control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    const auto range = validatedExternalByteRange(*tensor, bytes);
    if (copiedBytes != nullptr) {
      *copiedBytes = checkedAdd(*copiedBytes, range.length);
      if (control != nullptr &&
          (control->maxAssembledBytes == 0 || *copiedBytes > control->maxAssembledBytes))
        fail("MATERIAL_LIMIT");
    }
    tensor->set_raw_data(reinterpret_cast<const char*>(bytes.data() + range.offset),
                         static_cast<int>(range.length));
    tensor->clear_external_data();
    tensor->set_data_location(onnx::TensorProto::DEFAULT);
    if (control != nullptr) checkActive(*control);
  }
}

onnx::TensorProto materializeExternalTensor(
  const onnx::TensorProto& tensor,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl* control = nullptr)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL)
    fail("EXTERNAL_BINDING");
  if (externalBytes == nullptr)
    fail("EXTERNAL_BINDING");
  auto result = tensor;
  std::vector<onnx::TensorProto*> tensors{&result};
  inlineValidatedExternals(tensors, *externalBytes, control);
  return result;
}

void inlineDeepExternals(onnx::ModelProto& model,
                         const std::vector<std::uint8_t>& bytes,
                         const NativeAssemblyControl* control,
                         std::uint64_t* materializedBudget = nullptr)
{
  std::vector<onnx::TensorProto*> all;
  collectModelTensors(model, all);
  std::unordered_set<onnx::TensorProto*> topLevel;
  topLevel.reserve(static_cast<std::size_t>(model.graph().initializer_size()));
  for (int i = 0; i < model.graph().initializer_size(); ++i)
    topLevel.insert(model.mutable_graph()->mutable_initializer(i));
  std::vector<onnx::TensorProto*> deep;
  std::vector<onnx::TensorProto*> externalDeep;
  deep.reserve(all.size());
  for (auto* tensor : all)
    if (topLevel.count(tensor) == 0) {
      deep.push_back(tensor);
      if (tensor->data_location() == onnx::TensorProto::EXTERNAL)
        externalDeep.push_back(tensor);
    }
  std::uint64_t localBudget = 0;
  auto* copiedBytes = materializedBudget != nullptr ? materializedBudget : &localBudget;
  inlineValidatedExternals(deep, bytes, control, copiedBytes);
  for (auto* tensor : externalDeep) {
    if (control != nullptr) checkActive(*control);
    const auto serialized = deterministicMessageVector(*tensor);
    normalizedOnnxInitializerPayload(serialized);
    if (control != nullptr) checkActive(*control);
  }
}

// Shape inference needs values only for a bounded set of operator parameters
// (for example Reshape shape, Slice bounds, and TopK K).  Keep the large
// weight initializers external and materialize only those named inputs in the
// inference copy.  This preserves external/inline shape parity without
// recreating the complete model-sized peak.
void materializeShapeInferenceInitializers(
  onnx::ModelProto& inferred,
  const onnx::ModelProto& sourceModel,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl& control,
  std::uint64_t* materializedBudget = nullptr)
{
  if (externalBytes == nullptr) return;
  struct ShapePolicyKey {
    std::string domain;
    std::string opType;
    std::int64_t opset = 0;
    bool operator<(const ShapePolicyKey& other) const
    {
      return std::tie(domain, opType, opset) <
        std::tie(other.domain, other.opType, other.opset);
    }
  };
  std::map<std::string, std::int64_t> opsets;
  for (const auto& opset : sourceModel.opset_import())
    opsets[opset.domain()] = opset.version();
  const auto opsetFor = [&] (const std::string& domain) {
    const auto found = opsets.find(domain);
    return found == opsets.end() ? std::int64_t{0} : found->second;
  };
  std::map<ShapePolicyKey, std::vector<int>> policies;
  const auto shapePolicy = [&] (const char* domain, const char* op,
                                std::initializer_list<int> indices) {
    policies[{domain, op, opsetFor(domain)}] = std::vector<int>(indices);
  };
  const auto shapeIndependent = [&] (const char* domain, const char* op) {
    shapePolicy(domain, op, {});
  };
  const auto shapeValue = [&] (const char* op, std::initializer_list<int> indices) {
    shapePolicy("", op, indices);
  };
  shapeValue("Reshape", {1}); shapeValue("Expand", {1});
  shapeValue("Gather", {1}); shapeValue("Tile", {1});
  shapeValue("OneHot", {1}); shapeValue("TopK", {1});
  shapeValue("Split", {1}); shapeValue("Squeeze", {1});
  shapeValue("Unsqueeze", {1}); shapeValue("ConstantOfShape", {0});
  shapeValue("Range", {0, 1, 2}); shapeValue("Slice", {1, 2, 3, 4});
  shapeValue("Resize", {1, 2, 3}); shapeValue("Pad", {1});
  shapeValue("CumSum", {1}); shapeValue("NonMaxSuppression", {2, 3, 4});
  shapeValue("Compress", {1}); shapeValue("If", {0}); shapeValue("Loop", {0, 1});
  for (const char* op : {"ReduceL1", "ReduceL2", "ReduceLogSum", "ReduceLogSumExp",
                         "ReduceMax", "ReduceMean", "ReduceMin", "ReduceProd",
                         "ReduceSum", "ReduceSumSquare"})
    shapeValue(op, {1});
  // Explicitly register the standard data-only operators used by Qwen and
  // common ONNX transformer exports. An unknown op/domain with an external
  // initializer input is rejected below instead of entering inference with
  // an unproven graph-only value.
  for (const char* op : {"Abs", "Add", "BatchNormalization", "Cast", "Clip", "Concat",
                         "Conv", "ConvTranspose", "DequantizeLinear", "Div", "Dropout",
                         "Einsum", "Equal", "Exp", "Flatten", "Gelu", "GemmaRotaryEmbedding",
                         "Gemm", "Greater", "GreaterOrEqual", "Identity", "LayerNormalization",
                         "Less", "LessOrEqual", "Log", "LogSoftmax", "MatMul", "MatMulInteger",
                         "Mul",
                         "Neg", "Pow", "QLinearConv", "QLinearMatMul", "QuantizeLinear",
                         "Relu", "RotaryEmbedding", "ScatterElements", "ScatterND", "Sigmoid",
                         "SimplifiedLayerNormalization", "Size", "Softmax", "Sqrt", "Sub",
                         "Tanh", "Transpose", "Where"})
    shapeIndependent("", op);
  for (const char* domain : {"com.microsoft", "ai.onnx.contrib"})
    for (const char* op : {"Attention", "FusedMatMul", "Gelu", "GroupQueryAttention",
                           "LayerNormalization", "RotaryEmbedding", "SkipLayerNormalization",
                           "SkipSimplifiedLayerNormalization"})
      shapeIndependent(domain, op);
  std::unordered_set<std::string> externalInitializers;
  for (const auto& initializer : sourceModel.graph().initializer())
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL)
      externalInitializers.insert(initializer.name());
  std::unordered_set<std::string> required;
  std::function<void(const onnx::GraphProto&, bool)> scanGraph;
  std::function<void(const google::protobuf::RepeatedPtrField<onnx::NodeProto>&,
                     const std::unordered_set<std::string>&, bool)> scanNodes;
  const auto addInput = [&] (const onnx::NodeProto& node, int index,
                             const std::unordered_set<std::string>& localNames,
                             bool root) {
    if (index >= 0 && index < node.input_size() && !node.input(index).empty() &&
        externalInitializers.count(node.input(index)) != 0 &&
        (root || localNames.count(node.input(index)) == 0))
      required.insert(node.input(index));
  };
  scanNodes = [&] (const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes,
                   const std::unordered_set<std::string>& localNames, bool root) {
    for (const auto& node : nodes) {
      const auto policy = policies.find({node.domain(), node.op_type(), opsetFor(node.domain())});
      bool hasExternalInput = false;
      for (const auto& input : node.input())
        hasExternalInput = hasExternalInput ||
          (externalInitializers.count(input) != 0 &&
           (root || localNames.count(input) == 0));
      if (policy == policies.end()) {
        if (hasExternalInput) fail("SHAPE_INPUT_UNSUPPORTED");
      }
      else {
        for (const auto index : policy->second) addInput(node, index, localNames, root);
      }
      for (const auto& attribute : node.attribute()) {
        if (attribute.has_g()) scanGraph(attribute.g(), false);
        for (const auto& graph : attribute.graphs()) scanGraph(graph, false);
      }
    }
  };
  scanGraph = [&] (const onnx::GraphProto& graph, bool root) {
    std::unordered_set<std::string> localNames;
    if (!root) {
      for (const auto& initializer : graph.initializer()) localNames.insert(initializer.name());
      for (const auto& input : graph.input()) localNames.insert(input.name());
      for (const auto& node : graph.node())
        for (const auto& output : node.output())
          if (!output.empty()) localNames.insert(output);
    }
    scanNodes(graph.node(), localNames, root);
  };
  scanGraph(sourceModel.graph(), true);
  for (const auto& function : sourceModel.functions()) {
    std::unordered_set<std::string> localNames;
    for (const auto& input : function.input()) localNames.insert(input);
    for (const auto& output : function.output()) localNames.insert(output);
    for (const auto& node : function.node())
      for (const auto& output : node.output())
        if (!output.empty()) localNames.insert(output);
    scanNodes(function.node(), localNames, false);
  }

  std::uint64_t localBudget = 0;
  auto* materializedBytes = materializedBudget != nullptr ? materializedBudget : &localBudget;
  for (int i = 0; i < sourceModel.graph().initializer_size(); ++i) {
    checkActive(control);
    const auto& sourceInitializer = sourceModel.graph().initializer(i);
    if (required.count(sourceInitializer.name()) == 0 ||
        sourceInitializer.data_location() != onnx::TensorProto::EXTERNAL)
      continue;
    auto material = materializeExternalTensor(sourceInitializer, externalBytes, &control);
    *materializedBytes = checkedAdd(*materializedBytes,
                                   static_cast<std::uint64_t>(material.ByteSizeLong()));
    if (control.maxAssembledBytes == 0 || *materializedBytes > control.maxAssembledBytes)
      fail("MATERIAL_LIMIT");
    *inferred.mutable_graph()->mutable_initializer(i) = std::move(material);
  }
  checkActive(control);
}

// Materialize only the external initializers referenced by the selected role.
// The complete source graph remains external, so the worker does not create a
// second full-size raw_data copy before extraction.  The extracted model later
// takes ownership of these TensorProto values with Swap().
void materializeSelectedExternalInitializers(
  onnx::ModelProto& model,
  const std::vector<std::size_t>& selectedNodes,
  const std::vector<std::uint8_t>& externalBytes,
  const NativeAssemblyControl& control)
{
  std::unordered_set<std::string> required;
  const auto& graph = model.graph();
  for (const auto index : selectedNodes) {
    if (index >= static_cast<std::size_t>(graph.node_size()))
      fail("NODE_COVER");
    for (const auto& input : graph.node(static_cast<int>(index)).input())
      if (!input.empty())
        required.insert(input);
  }
  std::vector<onnx::TensorProto*> tensors;
  collectModelTensors(model, tensors);
  for (auto* tensor : tensors) {
    checkActive(control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL ||
        required.count(tensor->name()) == 0)
      continue;
    auto material = materializeExternalTensor(*tensor, &externalBytes, &control);
    tensor->Swap(&material);
  }
  checkActive(control);
}

// Shared OA05-for-identity entry: source limits, parse, external binding and
// memory-only inlining.  It deliberately does not full-check or shape-infer.
onnx::ModelProto ownedSourceModel(const NativeCanonicalSource& source,
                                  const NativeAssemblyControl& control,
                                  bool inlineExternal,
                                  std::uint64_t* materializedBudget,
                                  const NativeOnnxModelFileInput* modelFile)
{
  checkActive(control);
  const std::uint64_t sourceBytes = modelFile != nullptr
    ? modelFile->bytes : static_cast<std::uint64_t>(source.modelBytes.size());
  if (control.maxSourceBytes == 0 || sourceBytes == 0 ||
      sourceBytes > control.maxSourceBytes ||
      (modelFile == nullptr &&
       source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    fail("SOURCE_LIMIT");
  if (modelFile != nullptr && !source.modelBytes.empty())
    fail("SOURCE_OWNERSHIP");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > control.maxSourceBytes ||
       source.initializerBytes->size() >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
       checkedAdd(sourceBytes, source.initializerBytes->size()) >
         checkedAdd(control.maxSourceBytes, control.maxSourceBytes)))
    fail("INITIALIZER_LIMIT");

  onnx::ModelProto model;
  if (modelFile != nullptr) {
    (void) digestModelFile(*modelFile);
    google::protobuf::io::FileInputStream input(modelFile->fd);
    input.SetCloseOnDelete(false);
    if (!model.ParseFromZeroCopyStream(&input))
      fail("PARSE");
  }
  else if (!model.ParseFromArray(source.modelBytes.data(),
                                 static_cast<int>(source.modelBytes.size()))) {
    fail("PARSE");
  }
  std::vector<onnx::TensorProto*> externalTensors;
  collectModelTensors(model, externalTensors);
  const bool hasExternal = std::any_of(
    externalTensors.begin(), externalTensors.end(), [] (const onnx::TensorProto* tensor) {
      return tensor->data_location() == onnx::TensorProto::EXTERNAL;
    });
  if (hasExternal != source.initializerBytes.has_value()) fail("EXTERNAL_BINDING");
  if (source.initializerBytes) {
    validateAndPinLocation(externalTensors);
    if (inlineExternal)
      inlineValidatedExternals(externalTensors, *source.initializerBytes, &control);
    else
      inlineDeepExternals(model, *source.initializerBytes, &control, materializedBudget);
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
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) fail("SERIALIZE_LIMIT");
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
  std::string dtype;
  std::vector<std::int64_t> shape;
  std::string byteOrder;
  std::size_t byteLength = 0;
  std::string contentDigest;
  std::string sharedReference;
};

// Python reference layout: sorted by tensor name; contentDigest of the
// canonical bytes; sharedReference is the first (smallest) alias name when
// more than one tensor shares the digest, else empty.  Top-level external
// tensors stay graph-only in the model protobuf; a single temporary
// TensorProto receives only its authenticated range, so the index never
// retains a second complete initializer object. Deep attribute tensors have
// already been materialized because their protobuf wire contributes to the
// graph identity and template payload.
std::vector<TensorIndexEntry> buildTensorIndex(
  const onnx::ModelProto& model,
  const std::vector<std::uint8_t>* externalBytes = nullptr,
  const NativeAssemblyControl* control = nullptr)
{
  std::vector<TensorIndexEntry> entries;
  entries.reserve(static_cast<std::size_t>(model.graph().initializer_size()));
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    if (control != nullptr) checkActive(*control);
    const auto& initializer = model.graph().initializer(i);
    if (initializer.name().empty()) failNormalization("INITIALIZER_ENCODING_INVALID");
    TensorIndexEntry entry;
    entry.name = initializer.name();
    const auto* material = &initializer;
    std::optional<onnx::TensorProto> externalMaterial;
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL) {
      if (externalBytes == nullptr) fail("EXTERNAL_BINDING");
      // External numeric raw_data is already the canonical little-endian
      // payload.  Hash the authenticated range directly instead of creating
      // a temporary TensorProto, serializing it, parsing it again and copying
      // the raw bytes into a second normalization vector.  Nibble-packed
      // INT4/UINT4 remain on the materialized path because normalization
      // expands each nibble to one canonical byte.
      if (initializer.data_type() != onnx::TensorProto::INT4 &&
          initializer.data_type() != onnx::TensorProto::UINT4) {
        const auto count = tensorElementCount(initializer);
        const auto expected = rawByteLength(initializer.data_type(), count);
        const auto range = validatedExternalByteRange(initializer, *externalBytes);
        if (range.length != expected)
          failNormalization("INITIALIZER_ENCODING_INVALID");
        entry.dtype = dtypeLabel(initializer.data_type());
        entry.shape.reserve(static_cast<std::size_t>(initializer.dims_size()));
        for (const auto dim : initializer.dims()) entry.shape.push_back(dim);
        entry.byteOrder = byteOrderOf(initializer.data_type());
        entry.byteLength = static_cast<std::size_t>(expected);
        entry.contentDigest = digest(externalBytes->data() + range.offset,
                                     static_cast<std::size_t>(expected));
        entries.push_back(std::move(entry));
        if (control != nullptr) checkActive(*control);
        continue;
      }
      externalMaterial.emplace(materializeExternalTensor(initializer, externalBytes, control));
      material = &*externalMaterial;
    }
    std::vector<std::uint8_t> serialized(static_cast<std::size_t>(material->ByteSizeLong()));
    if (!serialized.empty() &&
        !material->SerializeToArray(serialized.data(), static_cast<int>(serialized.size())))
      fail("SERIALIZE");
    if (control != nullptr) checkActive(*control);
    auto normalized = normalizedOnnxInitializerPayload(serialized);
    if (control != nullptr) checkActive(*control);
    entry.dtype = std::move(normalized.dtype);
    entry.shape = std::move(normalized.shape);
    entry.byteOrder = std::move(normalized.byteOrder);
    entry.byteLength = normalized.content.size();
    entry.contentDigest = digest(normalized.content);
    entries.push_back(std::move(entry));
    if (control != nullptr) checkActive(*control);
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
    out += std::to_string(entry.byteLength);
    out += ",\"byteOrder\":";
    jsonString(out, entry.byteOrder);
    out += ",\"dtype\":";
    jsonString(out, entry.dtype);
    out += ",\"shape\":";
    appendShapeJson(out, entry.shape);
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
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL &&
        initializer.data_type() == onnx::TensorProto::BFLOAT16)
      return 2;
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

NativeJson materialReferenceJson(const NativeCanonicalSource::MaterialReference& reference)
{
  NativeJson dependencies = NativeJson::array();
  for (const auto& dependency : reference.dependencies)
    dependencies.push_back(dependency);
  NativeJson result{
    {"bytes", reference.bytes},
    {"dependencies", std::move(dependencies)},
    {"digest", reference.digest},
    {"kind", reference.kind},
    {"logicalName", reference.logicalName},
    {"nodeIndex", reference.nodeIndex},
    {"payloadId", reference.payloadId},
    {"sharedDigest", reference.sharedDigest}};
  // Keep the v1 canonical JSON byte-compatible for legacy single-payload
  // references.  The optional field is emitted only for chunked external or
  // large inline initializers, so parsing an old manifest does not change its digest.
  if (!reference.chunkPayloadIds.empty()) {
    NativeJson chunks = NativeJson::array();
    for (const auto& payloadId : reference.chunkPayloadIds)
      chunks.push_back(payloadId);
    result["chunkPayloadIds"] = std::move(chunks);
  }
  return result;
}

} // namespace

std::string NativeCanonicalSource::MaterialManifest::canonicalJson() const
{
  NativeJson references = NativeJson::array();
  for (const auto& reference : this->references)
    references.push_back(materialReferenceJson(reference));
  return nativeCanonicalJson(NativeJson{
    {"graphDigest", graphDigest},
    {"initializerDigest", initializerDigest},
    {"references", std::move(references)},
    {"schema", schema},
    {"sourceDigest", sourceDigest},
    {"templatePayloadId", templatePayloadId}});
}

void NativeCanonicalSource::MaterialManifest::validate() const
{
  const auto validDigest = [] (const std::string& value) {
    return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
      std::all_of(value.begin() + 7, value.end(), [] (const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
  };
  if (schema != "ndnsf-di-canonical-material-manifest-v1" ||
      !validDigest(sourceDigest) || !validDigest(graphDigest) ||
      !validDigest(initializerDigest) || !validDigest(manifestDigest) ||
      templatePayloadId.empty() || (payloadsComplete && payloads.empty()) ||
      nativePlanningDigest(canonicalJson()) != manifestDigest || references.empty() ||
      (payloadsComplete && payloads.empty()))
    throw std::invalid_argument("native canonical material manifest is incomplete");

  std::set<std::string> payloadIds;
  std::map<std::string, const MaterialPayload*> payloadById;
  for (const auto& payload : payloads) {
    const bool invalidRange = payload.rangeSource &&
      (payload.rangeSize == 0 || payload.rangeOffset > payload.rangeSource->size() ||
       payload.rangeSize > payload.rangeSource->size() - payload.rangeOffset);
    const bool invalidString = payload.stringBacking &&
      (payload.stringSize == 0 || payload.stringOffset > payload.stringBacking->size() ||
       payload.stringSize > payload.stringBacking->size() - payload.stringOffset);
    const unsigned backingKinds = static_cast<unsigned>(payload.backing != nullptr) +
      static_cast<unsigned>(payload.stringBacking != nullptr) +
      static_cast<unsigned>(payload.rangeSource != nullptr);
    if (payload.payloadId.empty() || !validDigest(payload.digest) || payload.empty() ||
        backingKinds > 1 || invalidRange || invalidString ||
        (payload.backing &&
         (payload.backingOffset > payload.backing->size() ||
          payload.backingSize > payload.backing->size() - payload.backingOffset)) ||
        payload.byteSize() > NativeCanonicalMaterialBundleMaxBytes ||
        payload.byteSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !payloadIds.insert(payload.payloadId).second)
      throw std::invalid_argument("native canonical material payload is invalid");
    const auto payloadBytes = payload.copyBytes();
    if (nativePlanningDigest(payloadBytes.data(), payloadBytes.size()) != payload.digest)
      throw std::invalid_argument("native canonical material payload digest differs");
    payloadById.emplace(payload.payloadId, &payload);
  }
  const auto templatePayload = payloadById.find(templatePayloadId);
  if (payloadsComplete && templatePayload == payloadById.end())
    throw std::invalid_argument("native canonical material template is missing");
  std::set<std::string> referenceIds;
  std::set<std::string> referencedPayloadIds;
  std::set<std::string> directReferencePayloadIds;
  for (const auto& reference : references)
    directReferencePayloadIds.insert(reference.payloadId);
  std::set<std::string> allChunkPayloadIds;
  for (const auto& reference : references) {
    const bool hasChunks = !reference.chunkPayloadIds.empty();
    if (reference.payloadId.empty() ||
        (payloadsComplete && !payloadIds.count(reference.payloadId)) ||
        reference.kind.empty() || reference.logicalName.empty() || !validDigest(reference.digest) ||
        reference.bytes == 0 ||
        (payloadIds.count(reference.payloadId) &&
         (reference.digest != payloadById.at(reference.payloadId)->digest ||
          reference.bytes != payloadById.at(reference.payloadId)->byteSize())) ||
        !referenceIds.insert(reference.logicalName + "\x1f" + reference.kind).second)
      throw std::invalid_argument("native canonical material reference is invalid");
    referencedPayloadIds.insert(reference.payloadId);
    std::set<std::string> chunkIds;
    for (const auto& chunkId : reference.chunkPayloadIds) {
      if (chunkId.empty() || chunkId == reference.payloadId || !chunkIds.insert(chunkId).second ||
          !allChunkPayloadIds.insert(chunkId).second ||
          directReferencePayloadIds.count(chunkId) != 0 ||
          (payloadsComplete && !payloadIds.count(chunkId)))
        throw std::invalid_argument("native canonical material initializer chunk is invalid");
      referencedPayloadIds.insert(chunkId);
    }
    if (reference.kind == "graph-template") {
      if (reference.payloadId != templatePayloadId || reference.logicalName != "__template__" ||
          reference.nodeIndex != 0 || !reference.dependencies.empty() || !reference.sharedDigest.empty() ||
          hasChunks)
        throw std::invalid_argument("native canonical material template reference is invalid");
    }
    else if (reference.kind == "graph-node") {
      if (reference.logicalName != "node/" + std::to_string(reference.nodeIndex) ||
          !reference.sharedDigest.empty() || hasChunks)
        throw std::invalid_argument("native canonical material node reference is invalid");
      for (const auto& dependency : reference.dependencies)
        if (dependency.empty())
          throw std::invalid_argument("native canonical material node dependency is invalid");
    }
    else if (reference.kind == "shared-initializer") {
      if (!validDigest(reference.sharedDigest) || !reference.dependencies.empty() || reference.nodeIndex != 0)
        throw std::invalid_argument("native canonical material initializer reference is invalid");
    }
    else {
      throw std::invalid_argument("native canonical material reference kind is unsupported");
    }
  }
  if (payloadsComplete && referencedPayloadIds.size() != payloadIds.size())
    throw std::invalid_argument("native canonical material payload is unreferenced");
  if (!payloadsComplete) {
    for (const auto& payload : payloads)
      if (!referencedPayloadIds.count(payload.payloadId))
        throw std::invalid_argument("native canonical selected payload is unreferenced");
  }
}

std::shared_ptr<const NativeCanonicalSource::MaterialManifest>
deriveNativeCanonicalMaterialManifest(const NativeCanonicalSource& source,
                                      const NativeAssemblyControl& control)
{
  checkActive(control);
  auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
  auto result = std::make_shared<NativeCanonicalSource::MaterialManifest>();
  std::uint64_t materialBytes = 0;
  const auto accountMaterial = [&] (std::size_t bytes) {
    materialBytes = checkedAdd(materialBytes, static_cast<std::uint64_t>(bytes));
    if (control.maxAssembledBytes == 0 || materialBytes > control.maxAssembledBytes)
      fail("MATERIAL_LIMIT");
  };
  result->sourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  result->graphDigest = sha256HexOf(graphFactsJson(model, entries));
  result->initializerDigest = sha256HexOf(initializerContentJson(entries));

  auto templateModel = model;
  templateModel.mutable_graph()->clear_node();
  templateModel.mutable_graph()->clear_initializer();
  templateModel.mutable_graph()->clear_sparse_initializer();
  templateModel.mutable_graph()->clear_quantization_annotation();
  const auto templateBytes = deterministicMessageVector(templateModel);
  accountMaterial(templateBytes.size());
  NativeCanonicalSource::MaterialPayload templatePayload{
    "graph-template", digest(templateBytes), templateBytes};
  result->templatePayloadId = templatePayload.payloadId;
  result->payloads.push_back(std::move(templatePayload));
  result->references.push_back({
    "graph-template", {}, "graph-template", "__template__", 0,
    result->payloads.back().digest, result->payloads.back().bytes.size(), {}, {}});

  std::set<std::string> initializerNames;
  std::map<std::string, std::string> initializerPayloadByDigest;
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    checkActive(control);
    auto& initializer = *model.mutable_graph()->mutable_initializer(i);
    initializerNames.insert(initializer.name());
    const auto normalized = std::find_if(entries.begin(), entries.end(),
      [&initializer] (const auto& item) { return item.name == initializer.name(); });
    if (normalized == entries.end())
      failNormalization("INITIALIZER_ENCODING_INVALID");
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL) {
      // Do not serialize a 300+ MiB TensorProto just to publish one weight.
      // Keep a small metadata header and address the authenticated raw range
      // as bounded chunks.  Providers concatenate only the chunks selected
      // by their role before constructing the final TensorProto.
      if (!source.initializerBytes)
        fail("EXTERNAL_BINDING");
      auto header = initializer;
      header.clear_raw_data();
      header.clear_float_data();
      header.clear_int32_data();
      header.clear_string_data();
      header.clear_int64_data();
      header.clear_double_data();
      header.clear_uint64_data();
      header.clear_external_data();
      header.set_data_location(onnx::TensorProto::DEFAULT);
      const auto headerBytes = deterministicMessageVector(header);
      const auto headerDigest = digest(headerBytes);
      const auto headerPayloadId = std::string("initializer-header-") + headerDigest.substr(7);
      accountMaterial(headerBytes.size());
      result->payloads.push_back({headerPayloadId, headerDigest, headerBytes});

      const auto offsetText = externalValue(initializer, "offset");
      const auto lengthText = externalValue(initializer, "length");
      const auto offset = offsetText.empty() ? std::uint64_t{0} : parseUint(offsetText, "OFFSET");
      if (offset > source.initializerBytes->size())
        fail("EXTERNAL_RANGE");
      auto length = lengthText.empty()
        ? static_cast<std::uint64_t>(source.initializerBytes->size()) - offset
        : parseUint(lengthText, "LENGTH");
      if (length == 0)
        length = static_cast<std::uint64_t>(source.initializerBytes->size()) - offset;
      if (length > source.initializerBytes->size() - offset)
        fail("EXTERNAL_RANGE");

      std::vector<std::string> chunkPayloadIds;
      std::uint64_t consumed = 0;
      std::uint64_t chunkIndex = 0;
      if (source.initializerRangeSource &&
          source.initializerRangeSource->size() != source.initializerBytes->size())
        fail("INITIALIZER_RANGE");
      while (consumed < length) {
        checkActive(control);
        const auto chunkBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
          NativeCanonicalMaterialBundleMaxBytes, length - consumed));
        const auto chunkOffset = static_cast<std::size_t>(offset + consumed);
        const auto chunkDigest = digest(source.initializerBytes->data() + chunkOffset,
                                        chunkBytes);
        const auto chunkPayloadId = headerPayloadId + "-chunk-" + std::to_string(chunkIndex++);
        accountMaterial(chunkBytes);
        NativeCanonicalSource::MaterialPayload chunk;
        chunk.payloadId = chunkPayloadId;
        chunk.digest = chunkDigest;
        if (source.initializerRangeSource) {
          chunk.rangeSource = source.initializerRangeSource;
          chunk.rangeOffset = static_cast<std::uint64_t>(chunkOffset);
          chunk.rangeSize = chunkBytes;
        }
        else {
          chunk.backing = source.initializerBytes->shared();
          chunk.backingOffset = chunkOffset;
          chunk.backingSize = chunkBytes;
        }
        result->payloads.push_back(std::move(chunk));
        chunkPayloadIds.push_back(chunkPayloadId);
        consumed += chunkBytes;
      }
      result->references.push_back({
        headerPayloadId, std::move(chunkPayloadIds), "shared-initializer", initializer.name(), 0,
        headerDigest, headerBytes.size(), {}, normalized->contentDigest});
    }
    else if (initializer.raw_data().size() > NativeCanonicalMaterialBundleMaxBytes ||
             static_cast<std::uint64_t>(initializer.ByteSizeLong()) >
               NativeCanonicalMaterialBundleMaxBytes) {
      // Inline quantized ONNX commonly stores a large raw_data field directly
      // in the model protobuf. Use the serialized TensorProto size as well as
      // raw_data: a raw field exactly at the bundle limit can still exceed the
      // limit after protobuf metadata is included. Move that allocation into
      // shared string backing, emit a small authenticated TensorProto header,
      // and publish only bounded views. This keeps the inline source contract
      // equivalent to external initializer chunking without creating a second
      // full byte vector.
      auto rawBacking = std::make_shared<std::string>(std::move(*initializer.mutable_raw_data()));
      auto header = initializer;
      header.clear_raw_data();
      header.clear_float_data();
      header.clear_int32_data();
      header.clear_string_data();
      header.clear_int64_data();
      header.clear_double_data();
      header.clear_uint64_data();
      header.clear_external_data();
      header.set_data_location(onnx::TensorProto::DEFAULT);
      const auto headerBytes = deterministicMessageVector(header);
      const auto headerDigest = digest(headerBytes);
      const auto headerPayloadId = std::string("initializer-header-") + headerDigest.substr(7);
      accountMaterial(headerBytes.size());
      result->payloads.push_back({headerPayloadId, headerDigest, headerBytes});

      std::vector<std::string> chunkPayloadIds;
      std::uint64_t consumed = 0;
      std::uint64_t chunkIndex = 0;
      while (consumed < rawBacking->size()) {
        checkActive(control);
        const auto chunkBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
          NativeCanonicalMaterialBundleMaxBytes,
          static_cast<std::uint64_t>(rawBacking->size()) - consumed));
        const auto chunkOffset = static_cast<std::size_t>(consumed);
        const auto chunkDigest = digest(
          reinterpret_cast<const std::uint8_t*>(rawBacking->data()) + chunkOffset, chunkBytes);
        const auto chunkPayloadId = headerPayloadId + "-chunk-" + std::to_string(chunkIndex++);
        accountMaterial(chunkBytes);
        NativeCanonicalSource::MaterialPayload chunk;
        chunk.payloadId = chunkPayloadId;
        chunk.digest = chunkDigest;
        chunk.stringBacking = rawBacking;
        chunk.stringOffset = chunkOffset;
        chunk.stringSize = chunkBytes;
        result->payloads.push_back(std::move(chunk));
        chunkPayloadIds.push_back(chunkPayloadId);
        consumed += chunkBytes;
      }
      result->references.push_back({
        headerPayloadId, std::move(chunkPayloadIds), "shared-initializer", initializer.name(), 0,
        headerDigest, headerBytes.size(), {}, normalized->contentDigest});
    }
    else {
      const auto bytes = deterministicMessageVector(initializer);
      const auto payloadDigest = digest(bytes);
      auto payloadId = std::string("initializer-") + payloadDigest.substr(7);
      const auto existing = initializerPayloadByDigest.find(payloadDigest);
      if (existing == initializerPayloadByDigest.end()) {
        initializerPayloadByDigest.emplace(payloadDigest, payloadId);
        accountMaterial(bytes.size());
        result->payloads.push_back({payloadId, payloadDigest, bytes});
      }
      else {
        payloadId = existing->second;
      }
      result->references.push_back({
        payloadId, {}, "shared-initializer", initializer.name(), 0, payloadDigest,
        bytes.size(), {}, normalized->contentDigest});
    }
  }

  for (int i = 0; i < model.graph().node_size(); ++i) {
    checkActive(control);
    const auto& node = model.graph().node(i);
    const auto bytes = deterministicMessageVector(node);
    const auto payloadDigest = digest(bytes);
    accountMaterial(bytes.size());
    const auto payloadId = "node-" + std::to_string(i);
    result->payloads.push_back({payloadId, payloadDigest, bytes});
    std::vector<std::string> dependencies;
    for (const auto& input : node.input()) {
      if (initializerNames.count(input) != 0)
        dependencies.push_back(input);
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    result->references.push_back({
      payloadId, {}, "graph-node", "node/" + std::to_string(i),
      static_cast<std::uint64_t>(i), payloadDigest, bytes.size(), dependencies, {}});
    if ((i & 0x3f) == 0) control.requireActive();
  }
  control.requireActive();
  const auto materialManifestJson = result->canonicalJson();
  accountMaterial(materialManifestJson.size());
  result->manifestDigest = nativePlanningDigest(result->canonicalJson());
  result->validate();
  return result;
}

std::shared_ptr<NativeCanonicalSource::MaterialManifest>
parseNativeCanonicalMaterialManifest(const std::vector<std::uint8_t>& bytes)
{
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("native canonical material manifest bytes are invalid");
  const auto root = nativeParseJson(std::string(bytes.begin(), bytes.end()));
  if (!root.is_object())
    throw std::invalid_argument("native canonical material manifest is not an object");
  auto result = std::make_shared<NativeCanonicalSource::MaterialManifest>();
  result->schema = root.value("schema", std::string{});
  result->sourceDigest = root.value("sourceDigest", std::string{});
  result->graphDigest = root.value("graphDigest", std::string{});
  result->initializerDigest = root.value("initializerDigest", std::string{});
  result->manifestDigest = digest(bytes);
  if (!root.contains("templatePayloadId") || !root.at("templatePayloadId").is_string())
    throw std::invalid_argument("native canonical material template is missing");
  result->templatePayloadId = root.at("templatePayloadId").get<std::string>();
  const auto references = root.value("references", NativeJson::array());
  if (!references.is_array())
    throw std::invalid_argument("native canonical material references are invalid");
  for (const auto& item : references) {
    if (!item.is_object())
      throw std::invalid_argument("native canonical material reference is invalid");
    NativeCanonicalSource::MaterialReference reference;
    reference.payloadId = item.value("payloadId", std::string{});
    const auto chunks = item.value("chunkPayloadIds", NativeJson::array());
    if (!chunks.is_array())
      throw std::invalid_argument("native canonical material initializer chunks are invalid");
    for (const auto& chunk : chunks) {
      if (!chunk.is_string())
        throw std::invalid_argument("native canonical material initializer chunk is invalid");
      reference.chunkPayloadIds.push_back(chunk.get<std::string>());
    }
    reference.kind = item.value("kind", std::string{});
    reference.logicalName = item.value("logicalName", std::string{});
    reference.nodeIndex = item.value("nodeIndex", std::uint64_t{0});
    reference.digest = item.value("digest", std::string{});
    reference.bytes = item.value("bytes", std::uint64_t{0});
    const auto dependencies = item.value("dependencies", NativeJson::array());
    if (!dependencies.is_array())
      throw std::invalid_argument("native canonical material dependencies are invalid");
    for (const auto& dependency : dependencies)
      if (!dependency.is_string())
        throw std::invalid_argument("native canonical material dependency is invalid");
      else
        reference.dependencies.push_back(dependency.get<std::string>());
    reference.sharedDigest = item.value("sharedDigest", std::string{});
    result->references.push_back(std::move(reference));
  }
  // The reference index is authenticated by manifestDigest.  Payload bytes
  // are deliberately supplied separately by the post-Selection reader.
  result->payloadsComplete = false;
  result->validate();
  return result;
}

std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedInputs,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedOutputs,
                                const NativeAssemblyControl& control)
{
  checkActive(control);
  if (!source.materialManifest || nodeIndices.empty())
    fail("MATERIAL_SELECTION");
  source.materialManifest->validate();
  std::map<std::string, NativeCanonicalSource::MaterialPayload*> payloads;
  for (auto& payload : source.materialPayloads) {
    const auto payloadBytes = payload.copyBytes();
    if (payload.payloadId.empty() || payload.empty() ||
        digest(payloadBytes.data(), payloadBytes.size()) != payload.digest ||
        payloads.count(payload.payloadId) != 0)
      fail("MATERIAL_PAYLOAD");
    payloads.emplace(payload.payloadId, &payload);
  }
  const auto referencesForPayload = [&] (const std::string& payloadId) {
    std::vector<const NativeCanonicalSource::MaterialReference*> matches;
    for (const auto& reference : source.materialManifest->references) {
      if (reference.payloadId == payloadId ||
          std::find(reference.chunkPayloadIds.begin(), reference.chunkPayloadIds.end(),
                    payloadId) != reference.chunkPayloadIds.end())
        matches.push_back(&reference);
    }
    return matches;
  };
  for (const auto& item : payloads) {
    const auto matches = referencesForPayload(item.first);
    if (matches.empty())
      fail("MATERIAL_PAYLOAD");
    for (const auto* match : matches) {
      const bool chunkPayload = match->payloadId != item.first;
      if ((!chunkPayload && (match->digest != item.second->digest ||
                             match->bytes != item.second->byteSize())) ||
          (chunkPayload && item.second->byteSize() > NativeCanonicalMaterialBundleMaxBytes))
        fail("MATERIAL_PAYLOAD");
    }
  }
  std::uint64_t retainedMaterialBytes = source.materialManifest->canonicalJson().size();
  for (const auto& payload : source.materialPayloads)
    retainedMaterialBytes = checkedAdd(retainedMaterialBytes, payload.byteSize());
  const auto findReference = [&] (const std::string& kind,
                                  const std::string& logicalName)
    -> const NativeCanonicalSource::MaterialReference* {
    for (const auto& reference : source.materialManifest->references)
      if (reference.kind == kind && reference.logicalName == logicalName)
        return &reference;
    return nullptr;
  };
  const auto templateReference = findReference("graph-template", "__template__");
  if (templateReference == nullptr)
    fail("MATERIAL_TEMPLATE");
  const auto templatePayload = payloads.find(templateReference->payloadId);
  if (templatePayload == payloads.end())
    fail("MATERIAL_TEMPLATE");
  const auto templateBytes = templatePayload->second->copyBytes();
  onnx::ModelProto model;
  if (!model.ParseFromArray(templateBytes.data(),
                            static_cast<int>(templateBytes.size())) ||
      !model.has_graph() || model.graph().node_size() != 0 ||
      model.graph().initializer_size() != 0)
    fail("MATERIAL_TEMPLATE");

  // The legacy Qwen exporter created each stage with explicit role I/O.  A
  // canonical graph only contains the full-model boundary, so merely copying
  // its template leaves an internal handoff (for example
  // `hidden_states_out`) absent from graph.output and makes the subsequent
  // native extractor reject an otherwise valid selected node set.  Rebuild
  // the role boundary from the authenticated preparation contracts before
  // adding nodes.  The contracts carry the same dtype/shape information that
  // the old exporter wrote into its stage ONNX files.
  if (!expectedInputs.empty() || !expectedOutputs.empty()) {
    if (expectedInputs.empty() || expectedOutputs.empty())
      fail("MATERIAL_BOUNDARY");
    std::set<std::string> inputNames;
    std::set<std::string> outputNames;
    validateContracts(expectedInputs, inputNames);
    validateContracts(expectedOutputs, outputNames);
    const auto dtypeCode = [] (const std::string& value) -> std::int32_t {
      static const std::map<std::string, std::int32_t> labels{
        {"float32", onnx::TensorProto::FLOAT},
        {"uint8", onnx::TensorProto::UINT8},
        {"int8", onnx::TensorProto::INT8},
        {"uint16", onnx::TensorProto::UINT16},
        {"int16", onnx::TensorProto::INT16},
        {"int32", onnx::TensorProto::INT32},
        {"int64", onnx::TensorProto::INT64},
        {"string", onnx::TensorProto::STRING},
        {"bool", onnx::TensorProto::BOOL},
        {"float16", onnx::TensorProto::FLOAT16},
        {"float64", onnx::TensorProto::DOUBLE},
        {"uint32", onnx::TensorProto::UINT32},
        {"uint64", onnx::TensorProto::UINT64},
        {"complex64", onnx::TensorProto::COMPLEX64},
        {"complex128", onnx::TensorProto::COMPLEX128},
        {"bfloat16", onnx::TensorProto::BFLOAT16}};
      const auto named = labels.find(value);
      if (named != labels.end()) return named->second;
      if (value.empty() || !std::all_of(value.begin(), value.end(), [] (unsigned char c) {
            return std::isdigit(c) != 0;
          }))
        fail("MATERIAL_BOUNDARY");
      try {
        const auto parsed = std::stoll(value);
        if (parsed < 0 || parsed > std::numeric_limits<std::int32_t>::max())
          fail("MATERIAL_BOUNDARY");
        return static_cast<std::int32_t>(parsed);
      }
      catch (const std::exception&) {
        fail("MATERIAL_BOUNDARY");
      }
    };
    const auto valueInfo = [&] (const NativeAssemblyTensorContractV3& contract) {
      onnx::ValueInfoProto value;
      value.set_name(contract.name);
      auto* tensor = value.mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(dtypeCode(contract.dtype));
      auto* shape = tensor->mutable_shape();
      for (const auto& dimension : contract.shape) {
        auto* dim = shape->add_dim();
        if (std::holds_alternative<std::int64_t>(dimension)) {
          const auto number = std::get<std::int64_t>(dimension);
          if (number < 0) fail("MATERIAL_BOUNDARY");
          dim->set_dim_value(number);
        }
        else {
          const auto& symbol = std::get<std::string>(dimension);
          if (symbol.empty()) fail("MATERIAL_BOUNDARY");
          dim->set_dim_param(symbol);
        }
      }
      return value;
    };
    model.mutable_graph()->clear_input();
    model.mutable_graph()->clear_output();
    for (const auto& contract : expectedInputs)
      *model.mutable_graph()->add_input() = valueInfo(contract);
    for (const auto& contract : expectedOutputs)
      *model.mutable_graph()->add_output() = valueInfo(contract);
  }

  std::set<std::uint64_t> seenNodes;
  std::set<std::string> dependencies;
  std::optional<std::uint64_t> previousNodeIndex;
  for (const auto nodeIndex : nodeIndices) {
    checkActive(control);
    if (previousNodeIndex && nodeIndex <= *previousNodeIndex)
      fail("MATERIAL_SELECTION");
    previousNodeIndex = nodeIndex;
    if (!seenNodes.insert(nodeIndex).second)
      fail("MATERIAL_SELECTION");
    const auto* reference = findReference("graph-node", "node/" + std::to_string(nodeIndex));
    if (reference == nullptr)
      fail("MATERIAL_NODE");
    const auto payload = payloads.find(reference->payloadId);
    if (payload == payloads.end())
      fail("MATERIAL_NODE");
    const auto nodeBytes = payload->second->copyBytes();
    onnx::NodeProto node;
    if (!node.ParseFromArray(nodeBytes.data(), static_cast<int>(nodeBytes.size())))
      fail("MATERIAL_NODE");
    *model.mutable_graph()->add_node() = std::move(node);
    dependencies.insert(reference->dependencies.begin(), reference->dependencies.end());
  }
  for (const auto& dependency : dependencies) {
    const auto* match = findReference("shared-initializer", dependency);
    if (match == nullptr || payloads.find(match->payloadId) == payloads.end())
      fail("MATERIAL_INITIALIZER");
  }
  // A selected initializer may be backed by many authenticated bundle
  // objects.  Keep only the objects that still have a reference consumer:
  // once the final header/chunk has been copied into TensorProto, release its
  // backing immediately instead of retaining the whole bundle set until the
  // role model is complete.  The count also preserves correctness if a
  // manifest legitimately reuses one payload in more than one initializer.
  std::map<std::string, std::size_t> remainingPayloadUses;
  for (const auto& reference : source.materialManifest->references) {
    if (reference.kind != "shared-initializer" ||
        dependencies.count(reference.logicalName) == 0)
      continue;
    ++remainingPayloadUses[reference.payloadId];
    for (const auto& chunkPayloadId : reference.chunkPayloadIds)
      ++remainingPayloadUses[chunkPayloadId];
  }
  const auto releaseMaterialPayload = [&] (const std::string& payloadId) {
    const auto remaining = remainingPayloadUses.find(payloadId);
    if (remaining == remainingPayloadUses.end() || remaining->second == 0)
      return;
    if (--remaining->second == 0) {
      const auto payload = payloads.find(payloadId);
      if (payload != payloads.end())
        payload->second->release();
    }
  };
  const auto materializeInitializer = [&] (const auto& reference) {
    const auto header = payloads.find(reference.payloadId);
    if (header == payloads.end())
      fail("MATERIAL_INITIALIZER");
    const auto headerBytes = header->second->copyBytes();
    if (reference.chunkPayloadIds.empty()) {
      onnx::TensorProto initializer;
      if (!initializer.ParseFromArray(headerBytes.data(),
                                     static_cast<int>(headerBytes.size())))
        fail("MATERIAL_INITIALIZER");
      if (reference.digest != header->second->digest || reference.bytes != headerBytes.size())
        fail("MATERIAL_INITIALIZER");
      releaseMaterialPayload(reference.payloadId);
      return initializer;
    }
    onnx::TensorProto initializer;
    if (!initializer.ParseFromArray(headerBytes.data(),
                                   static_cast<int>(headerBytes.size())) ||
        !initializer.raw_data().empty() || initializer.data_location() == onnx::TensorProto::EXTERNAL)
      fail("MATERIAL_INITIALIZER");
    std::uint64_t rawBytes = 0;
    std::vector<std::uint8_t> raw;
    for (const auto& chunkId : reference.chunkPayloadIds) {
      checkActive(control);
      const auto chunk = payloads.find(chunkId);
      if (chunk == payloads.end() ||
          chunk->second->byteSize() > NativeCanonicalMaterialBundleMaxBytes) {
        fail("MATERIAL_INITIALIZER");
      }
      rawBytes = checkedAdd(rawBytes, chunk->second->byteSize());
    }
    const auto modelBytesBeforeInitializer = static_cast<std::uint64_t>(model.ByteSizeLong());
    const auto initializerHeaderBytes = static_cast<std::uint64_t>(initializer.ByteSizeLong());
    const auto finalModelWithoutRaw = checkedAdd(modelBytesBeforeInitializer,
                                                 initializerHeaderBytes);
    // maxAssembledBytes is the material-backed working-set ceiling here:
    // selected payloads remain owned by the source while the initializer is
    // concatenated, copied into TensorProto, and serialized into the result.
    // Include a small protobuf framing allowance before allocating raw.
    constexpr std::uint64_t protobufFramingAllowance = 128;
    const auto finalModelUpper = checkedAdd(
      checkedAdd(finalModelWithoutRaw, rawBytes), protobufFramingAllowance);
    const auto peakWithRawCopy = checkedAdd(
      retainedMaterialBytes, checkedAdd(rawBytes, checkedAdd(finalModelUpper, finalModelUpper)));
    if (rawBytes == 0 || rawBytes > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        control.maxAssembledBytes == 0 || peakWithRawCopy > control.maxAssembledBytes)
      fail("MATERIAL_INITIALIZER");
    raw.reserve(static_cast<std::size_t>(rawBytes));
    for (const auto& chunkId : reference.chunkPayloadIds) {
      checkActive(control);
      const auto chunk = payloads.find(chunkId);
      if (chunk == payloads.end())
        fail("MATERIAL_INITIALIZER");
      const auto chunkBytes = chunk->second->copyBytes();
      raw.insert(raw.end(), chunkBytes.begin(), chunkBytes.end());
      releaseMaterialPayload(chunkId);
    }
    releaseMaterialPayload(reference.payloadId);
    initializer.set_raw_data(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()));
    raw.clear();
    raw.shrink_to_fit();
    initializer.set_data_location(onnx::TensorProto::DEFAULT);
    const auto normalized = normalizedOnnxInitializerPayload(
      deterministicMessageVector(initializer));
    if (digest(normalized.content) != reference.sharedDigest)
      fail("MATERIAL_INITIALIZER");
    return initializer;
  };
  // Initializer references are emitted in canonical source order.  Preserve
  // that order while fetching only dependencies of this selected role.
  for (const auto& reference : source.materialManifest->references) {
    checkActive(control);
    if (reference.kind != "shared-initializer" ||
        dependencies.count(reference.logicalName) == 0)
      continue;
    const auto payload = payloads.find(reference.payloadId);
    if (payload == payloads.end())
      fail("MATERIAL_INITIALIZER");
    *model.mutable_graph()->add_initializer() = materializeInitializer(reference);
  }
  // Every selected node and initializer has now been parsed, authenticated,
  // and copied into `model`.  Release the fetched material before serializing
  // the final model: keeping the shared bundle backing alive here duplicates
  // the largest initializer while protobuf constructs the result vector.
  // `retainedMaterialBytes` above remains the conservative budget accounting;
  // this release only lowers the actual working set after all reads finish.
  for (auto& payload : source.materialPayloads)
    payload.scrub();
  std::vector<NativeCanonicalSource::MaterialPayload>{}.swap(source.materialPayloads);
  if (model.graph().node_size() != static_cast<int>(nodeIndices.size()) ||
      model.graph().node_size() > static_cast<int>(std::numeric_limits<int>::max()))
    fail("MATERIAL_SELECTION");
  const auto result = deterministicMessageVector(model);
  const auto finalWorkingSet = checkedAdd(
    retainedMaterialBytes, checkedAdd(result.size(), model.ByteSizeLong()));
  if (control.maxAssembledBytes == 0 || result.size() > control.maxAssembledBytes ||
      finalWorkingSet > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  return result;
}

std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const NativeAssemblyControl& control)
{
  return materializeNativeCanonicalModel(source, nodeIndices, {}, {}, control);
}

void validateNativeCanonicalMaterialManifest(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control)
{
  manifest.validate();
  std::uint64_t materialBudget = 0;
  for (const auto& payload : manifest.payloads)
    materialBudget = checkedAdd(materialBudget, payload.byteSize());
  materialBudget = checkedAdd(materialBudget, manifest.canonicalJson().size());
  if (control.maxAssembledBytes == 0 || materialBudget > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  checkActive(control);
  const auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
  if (manifest.sourceDigest != nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) ||
      manifest.graphDigest != sha256HexOf(graphFactsJson(model, entries)) ||
      manifest.initializerDigest != sha256HexOf(initializerContentJson(entries)))
    throw std::invalid_argument("native canonical material manifest source identity differs");

  const auto templateIt = std::find_if(manifest.references.begin(), manifest.references.end(),
    [] (const auto& reference) { return reference.kind == "graph-template"; });
  if (templateIt == manifest.references.end())
    throw std::invalid_argument("native canonical material template reference is missing");
  const auto templatePayloadIt = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
    [&templateIt] (const auto& payload) { return payload.payloadId == templateIt->payloadId; });
  if (templatePayloadIt == manifest.payloads.end())
    throw std::invalid_argument("native canonical material template payload is missing");
  const auto templatePayloadBytes = templatePayloadIt->copyBytes();
  onnx::ModelProto templateModel;
  if (!templateModel.ParseFromArray(templatePayloadBytes.data(),
                                    static_cast<int>(templatePayloadBytes.size())) ||
      templateModel.graph().node_size() != 0 || templateModel.graph().initializer_size() != 0 ||
      templateModel.graph().sparse_initializer_size() != 0 ||
      templateModel.graph().quantization_annotation_size() != 0)
    throw std::invalid_argument("native canonical material template does not match source");
  auto expectedTemplate = model;
  expectedTemplate.mutable_graph()->clear_node();
  expectedTemplate.mutable_graph()->clear_initializer();
  expectedTemplate.mutable_graph()->clear_sparse_initializer();
  expectedTemplate.mutable_graph()->clear_quantization_annotation();
  if (deterministicMessageVector(expectedTemplate) != templatePayloadBytes)
    throw std::invalid_argument("native canonical material template identity is invalid");

  std::map<std::string, std::string> initializerDigestByName;
  for (const auto& entry : entries)
    initializerDigestByName.emplace(entry.name, entry.contentDigest);
  std::set<std::string> coveredInitializers;
  std::set<std::uint64_t> coveredNodes;
  for (const auto& reference : manifest.references) {
    checkActive(control);
    const auto payload = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
      [&reference] (const auto& item) { return item.payloadId == reference.payloadId; });
    if (payload == manifest.payloads.end())
      throw std::invalid_argument("native canonical material reference payload is missing");
    if (reference.kind == "shared-initializer") {
      const auto expected = initializerDigestByName.find(reference.logicalName);
      if (expected == initializerDigestByName.end() || expected->second != reference.sharedDigest ||
          !coveredInitializers.insert(reference.logicalName).second)
        throw std::invalid_argument("native canonical material initializer binding is invalid");
      const auto sourceInitializer = std::find_if(model.graph().initializer().begin(),
        model.graph().initializer().end(), [&reference] (const auto& initializer) {
          return initializer.name() == reference.logicalName;
        });
      if (sourceInitializer == model.graph().initializer().end())
        throw std::invalid_argument("native canonical material initializer payload is invalid");
      const bool hasChunks = !reference.chunkPayloadIds.empty();
      auto expectedHeader = *sourceInitializer;
      if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL || hasChunks) {
        expectedHeader.clear_raw_data();
        expectedHeader.clear_float_data();
        expectedHeader.clear_int32_data();
        expectedHeader.clear_string_data();
        expectedHeader.clear_int64_data();
        expectedHeader.clear_double_data();
        expectedHeader.clear_uint64_data();
        expectedHeader.clear_external_data();
        expectedHeader.set_data_location(onnx::TensorProto::DEFAULT);
      }
      if (deterministicMessageVector(expectedHeader) != payload->copyBytes())
        throw std::invalid_argument("native canonical material initializer payload is invalid");
      if (reference.chunkPayloadIds.empty()) {
        if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL) {
          const auto materialized = materializeExternalTensor(
            *sourceInitializer,
            source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
          if (deterministicMessageVector(materialized) != payload->copyBytes())
            throw std::invalid_argument("native canonical material initializer payload is invalid");
        }
        else if (deterministicMessageVector(*sourceInitializer) != payload->copyBytes()) {
          throw std::invalid_argument("native canonical material initializer payload is invalid");
        }
      }
      else {
        if (sourceInitializer->data_location() != onnx::TensorProto::EXTERNAL &&
            sourceInitializer->raw_data().size() <= NativeCanonicalMaterialBundleMaxBytes &&
            static_cast<std::uint64_t>(sourceInitializer->ByteSizeLong()) <=
              NativeCanonicalMaterialBundleMaxBytes)
          throw std::invalid_argument("native canonical material initializer chunks are invalid");
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        const std::uint8_t* expectedBytes = nullptr;
        if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL) {
          if (!source.initializerBytes)
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          const auto offsetText = externalValue(*sourceInitializer, "offset");
          const auto lengthText = externalValue(*sourceInitializer, "length");
          offset = offsetText.empty() ? std::uint64_t{0} : parseUint(offsetText, "OFFSET");
          if (offset > source.initializerBytes->size())
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          length = lengthText.empty()
            ? static_cast<std::uint64_t>(source.initializerBytes->size()) - offset
            : parseUint(lengthText, "LENGTH");
          if (length == 0)
            length = static_cast<std::uint64_t>(source.initializerBytes->size()) - offset;
          if (length > source.initializerBytes->size() - offset)
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          expectedBytes = source.initializerBytes->data() + offset;
        }
        else {
          length = sourceInitializer->raw_data().size();
          expectedBytes = reinterpret_cast<const std::uint8_t*>(sourceInitializer->raw_data().data());
        }
        std::uint64_t total = 0;
        for (const auto& chunkId : reference.chunkPayloadIds) {
          const auto chunk = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
            [&chunkId] (const auto& item) { return item.payloadId == chunkId; });
          if (chunk == manifest.payloads.end() ||
              chunk->byteSize() > NativeCanonicalMaterialBundleMaxBytes)
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          const auto chunkBytes = chunk->copyBytes();
          if (total > length || chunk->byteSize() > length - total ||
              !std::equal(chunkBytes.begin(), chunkBytes.end(), expectedBytes + total))
            throw std::invalid_argument("native canonical material initializer chunks differ");
          total = checkedAdd(total, chunk->byteSize());
        }
        if (total != length)
          throw std::invalid_argument("native canonical material initializer chunks are incomplete");
      }
    }
    else if (reference.kind == "graph-node") {
      if (reference.nodeIndex >= static_cast<std::uint64_t>(model.graph().node_size()) ||
          !coveredNodes.insert(reference.nodeIndex).second)
        throw std::invalid_argument("native canonical material node coverage is invalid");
      const auto payloadBytes = payload->copyBytes();
      onnx::NodeProto node;
      if (!node.ParseFromArray(payloadBytes.data(), static_cast<int>(payloadBytes.size())) ||
          deterministicMessageVector(node) != payloadBytes ||
          deterministicMessageVector(model.graph().node(static_cast<int>(reference.nodeIndex))) != payloadBytes)
        throw std::invalid_argument("native canonical material node payload is invalid");
      std::set<std::string> expectedDependencies;
      for (const auto& input : model.graph().node(static_cast<int>(reference.nodeIndex)).input())
        if (initializerDigestByName.count(input) != 0) expectedDependencies.insert(input);
      std::set<std::string> actualDependencies(reference.dependencies.begin(), reference.dependencies.end());
      if (actualDependencies != expectedDependencies || actualDependencies.size() != reference.dependencies.size())
        throw std::invalid_argument("native canonical material node dependencies are invalid");
    }
  }
  if (coveredInitializers.size() != initializerDigestByName.size() ||
      coveredNodes.size() != static_cast<std::size_t>(model.graph().node_size()))
    throw std::invalid_argument("native canonical material coverage is incomplete");
}

void validateNativeCanonicalMaterialReferenceIndex(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control)
{
  checkActive(control);
  if (manifest.payloadsComplete)
    throw std::invalid_argument("native canonical material index is complete");
  manifest.validate();
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  if (manifest.sourceDigest != nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) ||
      manifest.graphDigest != identity.graphDigest || manifest.initializerDigest != identity.initializerDigest)
    throw std::invalid_argument("native canonical material reference identity differs");
  checkActive(control);
}

namespace {
NativeOnnxIdentity
canonicalOnnxModelIdentity(const onnx::ModelProto& model,
                           const NativeAssemblyControl& control,
                           const std::vector<std::uint8_t>* externalBytes)
{
  checkActive(control);
  const auto entries = buildTensorIndex(model, externalBytes, &control);
  NativeOnnxIdentity identity;
  identity.graphDigest = sha256HexOf(graphFactsJson(model, entries));
  identity.initializerDigest = sha256HexOf(initializerContentJson(entries));
  checkActive(control);
  return identity;
}
} // namespace

NativeOnnxIdentity
canonicalOnnxSourceIdentity(const NativeCanonicalSource& source,
                            const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
  checkActive(control);
  NativeOnnxIdentity identity;
  identity.graphDigest = sha256HexOf(graphFactsJson(model, entries));
  identity.initializerDigest = sha256HexOf(initializerContentJson(entries));
  checkActive(control);
  return identity;
}

NativeOnnxGraphInspection
inspectNativeOnnxSourceGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control)
{
  expectedModel.validate();
  if (expectedModel.modelFormat != "onnx") fail("GRAPH_MODEL_FORMAT");
  std::uint64_t materializedBudget = 0;
  const auto original = ownedSourceModel(source, control, false, &materializedBudget);
  const auto entries = buildTensorIndex(
    original, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
  NativeOnnxGraphInspection result;
  result.canonicalIdentity = {sha256HexOf(graphFactsJson(original, entries)),
                              sha256HexOf(initializerContentJson(entries))};
  auto inferred = original;
  materializeShapeInferenceInitializers(
    inferred, original, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, control,
    &materializedBudget);
  try { onnx::shape_inference::InferShapes(inferred); }
  catch (const std::exception&) {
    // Python infer_shapes returns a copy. A failed C++ inference may have
    // partially mutated its argument, so restore the original before fallback.
    inferred = original;
  }
  checkActive(control);
  const auto& graph = inferred.graph();
  if (graph.node_size() != original.graph().node_size()) fail("GRAPH_NODE_ORDER");

  static const std::map<int, std::pair<std::string, std::uint64_t>> types = {
    {1, {"float32", 4}}, {2, {"uint8", 1}}, {3, {"int8", 1}}, {4, {"uint16", 2}},
    {5, {"int16", 2}}, {6, {"int32", 4}}, {7, {"int64", 8}}, {9, {"bool", 1}},
    {10, {"float16", 2}}, {11, {"float64", 8}}, {12, {"uint32", 4}},
    {13, {"uint64", 8}}, {16, {"bfloat16", 2}}};
  const auto tensorInfo = [&](const std::string& name, int type, const NativeJson& shape) {
    const auto known = types.find(type);
    NativeJson bytes = nullptr;
    if (known != types.end()) {
      bool fixed = true, zero = false;
      for (const auto& dim : shape) {
        if (!dim.is_number_integer() || dim.get<std::int64_t>() < 0) fixed = false;
        else if (dim.get<std::int64_t>() == 0) zero = true;
      }
      if (fixed) {
        std::uint64_t count = zero ? 0 : known->second.second;
        if (!zero) for (const auto& dim : shape) {
          const auto n = dim.get<std::uint64_t>();
          if (n && count > std::numeric_limits<std::uint64_t>::max() / n) fail("GRAPH_TENSOR_SIZE");
          count *= n;
        }
        bytes = count;
      }
    }
    return NativeJson{{"name", name}, {"dtype", known == types.end() ? "onnx_type_" + std::to_string(type) : known->second.first},
                      {"shape", shape}, {"sizeBytes", bytes}};
  };
  auto tensors = NativeJson::object();
  const auto addValues = [&](const auto& values) {
    for (const auto& value : values) {
      if (value.name().empty()) continue;
      const auto& type = value.type().tensor_type();
      auto shape = NativeJson::array();
      for (const auto& dim : type.shape().dim()) {
        if (dim.has_dim_value()) shape.push_back(dim.dim_value());
        else if (dim.has_dim_param()) shape.push_back(dim.dim_param());
        else shape.push_back("?");
      }
      tensors[value.name()] = tensorInfo(value.name(), type.elem_type(), shape);
    }
  };
  addValues(graph.input()); addValues(graph.output()); addValues(graph.value_info());
  std::set<std::string> initializers;
  for (const auto& tensor : graph.initializer()) {
    auto shape = NativeJson::array();
    for (const auto dim : tensor.dims()) shape.push_back(dim);
    tensors[tensor.name()] = tensorInfo(tensor.name(), tensor.data_type(), shape);
    initializers.insert(tensor.name());
  }
  std::vector<std::string> inputs, outputs;
  for (const auto& value : graph.input()) if (!initializers.count(value.name())) inputs.push_back(value.name());
  for (const auto& value : graph.output()) outputs.push_back(value.name());
  auto nodes = NativeJson::array();
  std::map<std::string, std::uint64_t> producers;
  std::map<std::string, std::vector<std::uint64_t>> consumers;
  for (int index = 0; index < graph.node_size(); ++index) {
    checkActive(control);
    const auto& node = graph.node(index);
    const auto& before = original.graph().node(index);
    if (node.name() != before.name() || node.op_type() != before.op_type() ||
        !std::equal(node.input().begin(), node.input().end(), before.input().begin(), before.input().end()) ||
        !std::equal(node.output().begin(), node.output().end(), before.output().begin(), before.output().end()))
      fail("GRAPH_NODE_ORDER");
    const auto id = "onnx-node-" + std::to_string(index);
    const auto name = node.name().empty() ? std::to_string(index) + ":" + node.op_type() : node.name();
    std::vector<std::string> in, out;
    for (const auto& value : node.input()) if (!value.empty() && !initializers.count(value)) {
      in.push_back(value); consumers[value].push_back(index);
    }
    for (const auto& value : node.output()) if (!value.empty()) {
      out.push_back(value); producers[value] = index;
    }
    nodes.push_back(NativeJson{{"index", index}, {"name", name}, {"opType", node.op_type()},
                               {"inputs", in}, {"outputs", out}});
    result.graph.nodes.push_back({id, node.op_type(), std::uint64_t(index)});
    result.graph.topologicalOrder.push_back(id);
    result.nodeNames.push_back(name);
    result.canonicalNodeIndices[id] = index;
  }
  const auto contract = [&](const std::string& name) {
    NativeTensorContract tensor;
    tensor.name = name; tensor.dtype = "unknown";
    const auto found = tensors.find(name);
    if (found == tensors.end()) return tensor;
    tensor.dtype = found->at("dtype").get<std::string>();
    for (const auto& dim : found->at("shape")) {
      if (dim.is_string()) tensor.shape.emplace_back(dim.get<std::string>());
      else tensor.shape.emplace_back(dim.get<std::int64_t>());
    }
    if (!found->at("sizeBytes").is_null()) tensor.estimatedBytes = found->at("sizeBytes").get<std::uint64_t>();
    return tensor;
  };
  for (const auto& producer : producers) {
    const auto users = consumers.find(producer.first);
    if (users == consumers.end() || users->second.empty()) continue;
    NativeGraphEdge edge;
    edge.id = producer.first; edge.producer = "onnx-node-" + std::to_string(producer.second);
    edge.tensor = contract(edge.id);
    // An ONNX node may consume one tensor in several operand slots. Keep
    // that multiplicity in metadata/identity, but a dependency consumer is
    // a node identity and must occur only once in the planning edge.
    std::set<std::uint64_t> uniqueUsers;
    for (const auto user : users->second)
      if (uniqueUsers.insert(user).second) edge.consumers.push_back("onnx-node-" + std::to_string(user));
    result.graph.edges.push_back(std::move(edge));
    // Every forward edge crosses the sequential cut after its producer.
    // The union of all maintained cuts is therefore exactly this edge set;
    // validate() below rejects backward or otherwise invalid dependencies.
    result.graph.legalCutEdges.push_back(producer.first);
  }
  for (const auto& name : inputs) result.graph.modelInputs.push_back(contract(name));
  for (const auto& name : outputs) result.graph.modelOutputs.push_back(contract(name));
  const std::vector<std::string> initializerNames(initializers.begin(), initializers.end());
  const NativeJson identity{{"adapter_descriptor_digest", expectedModel.adapter.descriptorDigest()},
    {"inputs", inputs}, {"outputs", outputs}, {"initializers", initializerNames}, {"tensors", tensors},
    {"nodes", nodes}, {"tensor_producers", producers}, {"tensor_consumers", consumers}};
  result.graph.graphDigest = nativePlanningDigest(nativeCanonicalJson(identity));
  auto sourceModel = expectedModel;
  sourceModel.graphDigest = result.graph.graphDigest;
  result.graph.validate(sourceModel);
  result.graphMetadataJson = nativeCanonicalJson(NativeJson{
    {"inputs", inputs}, {"outputs", outputs}, {"initializers", initializerNames}, {"tensors", tensors},
    {"nodes", nodes}, {"tensorProducers", producers}, {"tensorConsumers", consumers}});
  checkActive(control);
  return result;
}

NativeOnnxGraphInspection
inspectNativeOnnxPlanningGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control)
{
  auto result = inspectNativeOnnxSourceGraph(source, expectedModel, control);
  result.graph.validate(expectedModel);
  return result;
}

std::uint32_t
onnxInitializerNormalizationRevision(const NativeCanonicalSource& source,
                                     const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control, false);
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
