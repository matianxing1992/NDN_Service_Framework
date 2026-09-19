#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

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
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  const auto wire = deterministicMessageBytes(message);
  return std::vector<std::uint8_t>(wire.begin(), wire.end());
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
    if (value == nullptr) fail("GRAPH");
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
makeExtractedModel(const onnx::ModelProto& inferredSource,
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
    fail("GRAPH");
  onnx::ModelProto result;
  result.set_ir_version(inferredSource.ir_version());
  result.set_producer_name("onnx.utils.extract_model");
  for (const auto& opset : inferredSource.opset_import())
    *result.add_opset_import() = opset;
  auto* outGraph = result.mutable_graph();
  outGraph->set_name("Extracted from {" + graph.name() + "}");
  for (const std::size_t index : reachable)
    *outGraph->add_node() = graph.node(static_cast<int>(index));
  for (const auto& initializer : graph.initializer())
    if (keepNames.count(initializer.name()) != 0)
      *outGraph->add_initializer() = initializer;
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
                           const NativeAssemblyControl& control);

onnx::ModelProto
ownedSourceModel(const NativeCanonicalSource& source,
                 const NativeAssemblyControl& control);

NativeCertifiedAssembly
assembleCertifiedOnnxChain(const NativeCanonicalSource& source,
                           const NativeCertifiedRecipe& recipe,
                           std::uint64_t maxSourceBytes,
                           std::uint64_t maxAssembledBytes,
                           const std::function<void()>& onRound)
{
  onRound();
  if (maxSourceBytes == 0 || maxAssembledBytes == 0 ||
      source.modelBytes.empty() || source.modelBytes.size() > maxSourceBytes ||
      source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SOURCE_LIMIT");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > maxSourceBytes ||
       source.initializerBytes->size() >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
       checkedAdd(source.modelBytes.size(), source.initializerBytes->size()) >
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

  NativeAssemblyControl sourceControl;
  sourceControl.deadline = std::chrono::steady_clock::time_point::max();
  sourceControl.requireActive = onRound;
  sourceControl.maxSourceBytes = maxSourceBytes;
  sourceControl.maxAssembledBytes = maxAssembledBytes;
  // Reuse the strict owned-source path here.  It validates duplicate and
  // consistent external metadata, traverses function/nested tensors, and
  // applies the ONNX 1.17 length=0-to-EOF rule before returning the one model
  // object that the checker, identity and extractor all share.
  onnx::ModelProto original = ownedSourceModel(source, sourceControl);
  if (!original.has_graph() || original.graph().node_size() == 0 ||
      original.graph().node_size() > static_cast<int>(recipe.maxNodes)) fail("GRAPH");

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

  // S3: the inlined original must pass the full checker before any
  // certificate comparison (executor.py check_model(model, full_check=True));
  // checker rejections map to the semantic-model GRAPH family.
  try {
    onnx::checker::check_model(original, true);
  }
  catch (const std::exception&) {
    fail("GRAPH");
  }

  // The shared source control keeps the strict source, identity, and
  // cancellation checks on one bounded path for the worker and in-process
  // entries; no second source parse is needed here.
  // S4: the certified identity of the post-inline original must equal the
  // recipe digests; a mismatch means the recipe was sealed for different
  // source bytes (executor.py canonical ONNX digest mismatch errors).
  // ``original`` already owns the authenticated, external-data-inlined model
  // used by the checker and extractor.  Re-parsing ``source`` here would
  // allocate a second protobuf graph plus another copy of the 1.5 GiB
  // initializer before identity verification.  Derive the identity from the
  // existing model so the worker's peak remains within the request envelope.
  const auto identity = canonicalOnnxModelIdentity(original, sourceControl);
  if (identity.graphDigest != recipe.graphDigest ||
      identity.initializerDigest != recipe.canonicalInitializerDigest)
    fail("RECIPE");
  onRound();

  // S5: infer shapes on a copy (never mutating the parsed original), walk
  // the certified output boundary backwards, and rebuild the extractor
  // metadata with referred local functions and referenced tensors.
  onnx::ModelProto inferred = original;
  try {
    onnx::shape_inference::InferShapes(inferred);
  }
  catch (const std::exception&) {
    fail("GRAPH");
  }
  onRound();
  const auto& inferredGraph = inferred.graph();
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
  const auto inputs = collectBoundaryIo(inferredGraph.input(),
                                        inferredGraph.value_info(),
                                        recipe.expectedInputs);
  const auto outputs = collectBoundaryIo(inferredGraph.output(),
                                         inferredGraph.value_info(),
                                         recipe.expectedOutputs);
  const auto functions = referredLocalFunctions(inferred, selectedNodes,
                                                inferredGraph.node());
  onnx::ModelProto assembled = makeExtractedModel(inferred, selectedNodes,
                                                  inputs, outputs, functions);

  // S6: the assembled model passes the full checker again, its node cover
  // equals the certified indices with byte-identical nodes, and its io
  // matches the recipe contracts semantically (executor.py S6 block).
  try {
    onnx::checker::check_model(assembled, true);
  }
  catch (const std::exception&) {
    fail("GRAPH");
  }
  onRound();
  if (assembled.graph().node_size() != static_cast<int>(recipe.nodeIndices.size()))
    fail("NODE_COVER");
  for (std::size_t i = 0; i < recipe.nodeIndices.size(); ++i) {
    const auto expectedBytes = deterministicMessageBytes(
      original.graph().node(static_cast<int>(recipe.nodeIndices[i])));
    const auto assembledBytes = deterministicMessageBytes(
      assembled.graph().node(static_cast<int>(i)));
    if (expectedBytes != assembledBytes) fail("NODE_COVER");
  }
  compareBoundaryContracts(recipe.expectedInputs, assembled.graph().input());
  compareBoundaryContracts(recipe.expectedOutputs, assembled.graph().output());
  onRound();

  // S7: deterministic wire within the resource envelope, then a real CPU
  // session load of exactly these bytes; the session is destroyed before the
  // result leaves this function.
  auto bytes = deterministicWire(assembled, maxAssembledBytes);
  onRound();
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  try {
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-certified-assembly");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    Ort::Session session(env, bytes.data(), static_cast<int>(bytes.size()), options);
  }
  catch (const std::exception&) {
    fail("GRAPH");
  }
#else
  // The ONNX graph/protobuf helpers remain linkable in a disabled build, but
  // the certified assembly gate requires the optional ORT C++ runtime.
  fail("RUNTIME_UNAVAILABLE");
#endif
  onRound();
  NativeCertifiedAssembly result;
  result.modelDigest = digest(bytes);
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
                                    [&control] { checkActive(control); });
}

NativeCertifiedAssembly
assembleInProcess(const NativeCanonicalSource& source,
                  const NativeCertifiedRecipe& recipe)
{
  // OA04: the bounded worker child runs the identical chain against the
  // certified recipe budget with no cancellation callback of its own; the
  // parent transport owns cancellation by killing this process.
  return assembleCertifiedOnnxChain(source, recipe, recipe.maxSourceBytes,
                                    recipe.maxAssembledBytes, [] {});
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
      if (length > bytes.size() - offset || length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
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
      source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SOURCE_LIMIT");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > control.maxSourceBytes ||
       source.initializerBytes->size() >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
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

NativeJson materialReferenceJson(const NativeCanonicalSource::MaterialReference& reference)
{
  NativeJson dependencies = NativeJson::array();
  for (const auto& dependency : reference.dependencies)
    dependencies.push_back(dependency);
  return NativeJson{
    {"bytes", reference.bytes},
    {"dependencies", std::move(dependencies)},
    {"digest", reference.digest},
    {"kind", reference.kind},
    {"logicalName", reference.logicalName},
    {"nodeIndex", reference.nodeIndex},
    {"payloadId", reference.payloadId},
    {"sharedDigest", reference.sharedDigest}};
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
    if (payload.payloadId.empty() || !validDigest(payload.digest) || payload.bytes.empty() ||
        payload.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        nativePlanningDigest(payload.bytes.data(), payload.bytes.size()) != payload.digest ||
        !payloadIds.insert(payload.payloadId).second)
      throw std::invalid_argument("native canonical material payload is invalid");
    payloadById.emplace(payload.payloadId, &payload);
  }
  const auto templatePayload = payloadById.find(templatePayloadId);
  if (payloadsComplete && templatePayload == payloadById.end())
    throw std::invalid_argument("native canonical material template is missing");
  std::set<std::string> referenceIds;
  std::set<std::string> referencedPayloadIds;
  for (const auto& reference : references) {
    if (reference.payloadId.empty() ||
        (payloadsComplete && !payloadIds.count(reference.payloadId)) ||
        reference.kind.empty() || reference.logicalName.empty() || !validDigest(reference.digest) ||
        reference.bytes == 0 ||
        (payloadIds.count(reference.payloadId) &&
         (reference.digest != payloadById.at(reference.payloadId)->digest ||
          reference.bytes != payloadById.at(reference.payloadId)->bytes.size())) ||
        !referenceIds.insert(reference.logicalName + "\x1f" + reference.kind).second)
      throw std::invalid_argument("native canonical material reference is invalid");
    referencedPayloadIds.insert(reference.payloadId);
    if (reference.kind == "graph-template") {
      if (reference.payloadId != templatePayloadId || reference.logicalName != "__template__" ||
          reference.nodeIndex != 0 || !reference.dependencies.empty() || !reference.sharedDigest.empty())
        throw std::invalid_argument("native canonical material template reference is invalid");
    }
    else if (reference.kind == "graph-node") {
      if (reference.logicalName != "node/" + std::to_string(reference.nodeIndex) ||
          !reference.sharedDigest.empty())
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
  const auto model = ownedSourceModel(source, control);
  const auto entries = buildTensorIndex(model);
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
    "graph-template", "graph-template", "__template__", 0,
    result->payloads.back().digest, result->payloads.back().bytes.size(), {}, {}});

  std::set<std::string> initializerNames;
  std::map<std::string, std::string> initializerPayloadByDigest;
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    checkActive(control);
    const auto& initializer = model.graph().initializer(i);
    initializerNames.insert(initializer.name());
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
    const auto normalized = std::find_if(entries.begin(), entries.end(),
      [&initializer] (const auto& item) { return item.name == initializer.name(); });
    if (normalized == entries.end())
      failNormalization("INITIALIZER_ENCODING_INVALID");
    result->references.push_back({
      payloadId, "shared-initializer", initializer.name(), 0, payloadDigest,
      bytes.size(), {}, normalized->contentDigest});
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
      payloadId, "graph-node", "node/" + std::to_string(i),
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
materializeNativeCanonicalModel(const NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const NativeAssemblyControl& control)
{
  checkActive(control);
  if (!source.materialManifest || nodeIndices.empty())
    fail("MATERIAL_SELECTION");
  source.materialManifest->validate();
  std::map<std::string, const NativeCanonicalSource::MaterialPayload*> payloads;
  for (const auto& payload : source.materialPayloads) {
    if (payload.payloadId.empty() || payload.bytes.empty() ||
        digest(payload.bytes) != payload.digest || payloads.count(payload.payloadId) != 0)
      fail("MATERIAL_PAYLOAD");
    payloads.emplace(payload.payloadId, &payload);
  }
  const auto referencesForPayload = [&] (const std::string& payloadId) {
    std::vector<const NativeCanonicalSource::MaterialReference*> matches;
    for (const auto& reference : source.materialManifest->references)
      if (reference.payloadId == payloadId) matches.push_back(&reference);
    return matches;
  };
  for (const auto& item : payloads) {
    const auto matches = referencesForPayload(item.first);
    if (matches.empty())
      fail("MATERIAL_PAYLOAD");
    for (const auto* match : matches)
      if (match->digest != item.second->digest || match->bytes != item.second->bytes.size())
        fail("MATERIAL_PAYLOAD");
  }
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
  onnx::ModelProto model;
  if (!model.ParseFromArray(templatePayload->second->bytes.data(),
                            static_cast<int>(templatePayload->second->bytes.size())) ||
      !model.has_graph() || model.graph().node_size() != 0 ||
      model.graph().initializer_size() != 0)
    fail("MATERIAL_TEMPLATE");

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
    onnx::NodeProto node;
    if (!node.ParseFromArray(payload->second->bytes.data(),
                             static_cast<int>(payload->second->bytes.size())))
      fail("MATERIAL_NODE");
    *model.mutable_graph()->add_node() = std::move(node);
    dependencies.insert(reference->dependencies.begin(), reference->dependencies.end());
  }
  for (const auto& dependency : dependencies) {
    const auto* match = findReference("shared-initializer", dependency);
    if (match == nullptr || payloads.find(match->payloadId) == payloads.end())
      fail("MATERIAL_INITIALIZER");
  }
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
    onnx::TensorProto initializer;
    if (!initializer.ParseFromArray(payload->second->bytes.data(),
                                    static_cast<int>(payload->second->bytes.size())))
      fail("MATERIAL_INITIALIZER");
    *model.mutable_graph()->add_initializer() = std::move(initializer);
  }
  if (model.graph().node_size() != static_cast<int>(nodeIndices.size()) ||
      model.graph().node_size() > static_cast<int>(std::numeric_limits<int>::max()))
    fail("MATERIAL_SELECTION");
  const auto result = deterministicMessageVector(model);
  if (control.maxAssembledBytes == 0 || result.size() > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  return result;
}

void validateNativeCanonicalMaterialManifest(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control)
{
  manifest.validate();
  std::uint64_t materialBudget = 0;
  for (const auto& payload : manifest.payloads)
    materialBudget = checkedAdd(materialBudget, payload.bytes.size());
  materialBudget = checkedAdd(materialBudget, manifest.canonicalJson().size());
  if (control.maxAssembledBytes == 0 || materialBudget > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  checkActive(control);
  const auto model = ownedSourceModel(source, control);
  const auto entries = buildTensorIndex(model);
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
  onnx::ModelProto templateModel;
  if (!templateModel.ParseFromArray(templatePayloadIt->bytes.data(),
                                    static_cast<int>(templatePayloadIt->bytes.size())) ||
      templateModel.graph().node_size() != 0 || templateModel.graph().initializer_size() != 0 ||
      templateModel.graph().sparse_initializer_size() != 0 ||
      templateModel.graph().quantization_annotation_size() != 0)
    throw std::invalid_argument("native canonical material template does not match source");
  auto expectedTemplate = model;
  expectedTemplate.mutable_graph()->clear_node();
  expectedTemplate.mutable_graph()->clear_initializer();
  expectedTemplate.mutable_graph()->clear_sparse_initializer();
  expectedTemplate.mutable_graph()->clear_quantization_annotation();
  if (deterministicMessageVector(expectedTemplate) != templatePayloadIt->bytes)
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
      onnx::TensorProto tensor;
      if (!tensor.ParseFromArray(payload->bytes.data(), static_cast<int>(payload->bytes.size())) ||
          sourceInitializer == model.graph().initializer().end() ||
          deterministicMessageVector(tensor) != payload->bytes ||
          deterministicMessageVector(*sourceInitializer) != payload->bytes)
        throw std::invalid_argument("native canonical material initializer payload is invalid");
    }
    else if (reference.kind == "graph-node") {
      if (reference.nodeIndex >= static_cast<std::uint64_t>(model.graph().node_size()) ||
          !coveredNodes.insert(reference.nodeIndex).second)
        throw std::invalid_argument("native canonical material node coverage is invalid");
      onnx::NodeProto node;
      if (!node.ParseFromArray(payload->bytes.data(), static_cast<int>(payload->bytes.size())) ||
          deterministicMessageVector(node) != payload->bytes ||
          deterministicMessageVector(model.graph().node(static_cast<int>(reference.nodeIndex))) != payload->bytes)
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

namespace {
NativeOnnxIdentity
canonicalOnnxModelIdentity(const onnx::ModelProto& model,
                           const NativeAssemblyControl& control)
{
  checkActive(control);
  const auto entries = buildTensorIndex(model);
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
  const auto model = ownedSourceModel(source, control);
  return canonicalOnnxModelIdentity(model, control);
}

NativeOnnxGraphInspection
inspectNativeOnnxSourceGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control)
{
  expectedModel.validate();
  if (expectedModel.modelFormat != "onnx") fail("GRAPH_MODEL_FORMAT");
  const auto original = ownedSourceModel(source, control);
  const auto entries = buildTensorIndex(original);
  NativeOnnxGraphInspection result;
  result.canonicalIdentity = {sha256HexOf(graphFactsJson(original, entries)),
                              sha256HexOf(initializerContentJson(entries))};
  auto inferred = original;
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
