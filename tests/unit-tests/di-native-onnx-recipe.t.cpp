// T006-B Spec182OnnxExtraction: certified extraction and wire parity.
//
// The 11 frozen vectors (tests/fixtures/spec182/dependency-probes/
// extraction-vectors.json, schema spec182-extraction-vectors-v1, generator
// generate-extraction-vectors.py) were produced offline by the pinned python
// reference executor (onnx 1.17.0 / ort 1.19.2); expected bytes and digests
// are NEVER produced by this assembler.  This suite drives the native S3-S7
// certified pipeline (NativeOnnxRecipeAssembler.cpp
// assembleNativeCertifiedOnnxModel) over the same sources and recipes:
//
//   accept  chain-whole-inline        byte parity against the frozen wire
//           chain-external-equivalent byte parity too (byteParity True):
//             data_location is a proto3-optional field, so inlining the
//             externally sourced tensor keeps the explicit-default wire tag
//             (0x70) under both the python-upb and the C++ full-protobuf
//             loaders and the frozen python wire is reproduced exactly
//           branch-subset-rank         internal-io boundary T1
//           function-local-domain      custom-domain local function retained
//   reject  digest mismatch            -> DI_NATIVE_ONNX_RECIPE (S4)
//           ghost output io            -> DI_NATIVE_ONNX_GRAPH (S5 boundary)
//           short node cover           -> DI_NATIVE_ONNX_NODE_COVER (S6)
//           ghost input coverage       -> DI_NATIVE_ONNX_GRAPH (S5 boundary;
//             python rejects at recipe-construction IO_CONTRACT, a redundant
//             input_names cross-check that has no native analog - natively the
//             io names live only inside the contracts, so the poison surfaces
//             when the name cannot bind to the graph)
//           io dtype poison            -> DI_NATIVE_ONNX_IO_DTYPE (S6 compare)
//           io shape poison            -> DI_NATIVE_ONNX_IO_DTYPE (S6 compare)
//           layer escape               -> DI_NATIVE_ONNX_LAYER_RANGE (entry)

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

// Official ONNX 1.17 full-protobuf headers via the configured ONNX prefix.
// onnx_pb.h defines ONNX_API (visibility) before it pulls in the generated
// onnx-ml.pb.h; including the pb header directly leaves ONNX_API undefined
// and breaks the generated TableStruct declarations.
#include <onnx/onnx_pb.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

boost::property_tree::ptree loadExtractionVectors()
{
  namespace pt = boost::property_tree;
  const std::string candidates[] = {
    "tests/fixtures/spec182/dependency-probes/extraction-vectors.json",
    "../tests/fixtures/spec182/dependency-probes/extraction-vectors.json",
  };
  for (const auto& path : candidates) {
    std::ifstream in(path);
    if (in) {
      pt::ptree root;
      pt::read_json(in, root);
      return root;
    }
  }
  throw std::runtime_error("fixture not found: extraction-vectors.json");
}

// JSON arrays parse to children with empty keys, so rows are found by
// scanning one discriminating field rather than by path.
const boost::property_tree::ptree&
caseRow(const boost::property_tree::ptree& cases, const std::string& key,
        const std::string& value)
{
  for (const auto& child : cases) {
    if (child.second.get<std::string>(key, "") == value) return child.second;
  }
  throw std::runtime_error("vector row not found: " + key + "=" + value);
}

NativeAssemblyControl extractionControl()
{
  return NativeAssemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(15), [] {}, 8 * 1024 * 1024,
    8 * 1024 * 1024};
}

std::string sha256HexOf(const std::vector<std::uint8_t>& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 0x0f];
  }
  return result;
}

std::string toHex(const std::vector<std::uint8_t>& bytes)
{
  std::string result;
  for (const auto byte : bytes) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 0x0f];
  }
  return result;
}

std::vector<std::uint8_t> fromHex(const std::string& hex)
{
  BOOST_REQUIRE_EQUAL(hex.size() % 2, 0U);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

std::vector<std::string> stringChildren(const boost::property_tree::ptree& parent)
{
  std::vector<std::string> values;
  for (const auto& child : parent) values.push_back(child.second.get_value<std::string>());
  return values;
}

// Certified recipe carried by one vector row: the python
// CertifiedOnnxAssemblyRecipe to_dict fills the certificate fields below plus
// native-only identity (adapter/backend/role) that assembly requires.
NativeCertifiedRecipe recipeFromVector(const boost::property_tree::ptree& row)
{
  const auto& r = row.get_child("recipe");
  NativeCertifiedRecipe value;
  value.adapterId = "onnx";
  value.adapterVersion = "1";
  value.backend = "onnxruntime";
  value.role = "/role/0";
  value.selectedRole = value.role;
  value.backendAbi = r.get<std::string>("backendAbi");
  value.roleKind = r.get<std::string>("roleKind");
  value.modelManifestDigest = r.get<std::string>("modelManifestDigest");
  value.artifactProfileDigest = r.get<std::string>("artifactProfileDigest");
  value.graphDigest = r.get<std::string>("graphDigest");
  value.canonicalInitializerDigest = r.get<std::string>("canonicalInitializerDigest");
  value.adapterDescriptorDigest = r.get<std::string>("adapterDescriptorDigest");
  value.assemblerDescriptorDigest = r.get<std::string>("assemblerDescriptorDigest");
  value.layerBegin = r.get<std::uint64_t>("layerBegin");
  value.layerEnd = r.get<std::uint64_t>("layerEnd");
  for (const auto& child : r.get_child("nodeIndices"))
    value.nodeIndices.push_back(std::stoull(child.second.get_value<std::string>()));
  const auto readContracts = [](const boost::property_tree::ptree& parent) {
    std::vector<NativeAssemblyTensorContractV3> contracts;
    for (const auto& child : parent) {
      NativeAssemblyTensorContractV3 contract;
      contract.name = child.second.get<std::string>("name");
      contract.dtype = child.second.get<std::string>("dtype");
      contract.shape = stringChildren(child.second.get_child("shape"));
      contracts.push_back(std::move(contract));
    }
    return contracts;
  };
  value.expectedInputs = readContracts(r.get_child("expectedInputs"));
  value.expectedOutputs = readContracts(r.get_child("expectedOutputs"));
  value.precision = r.get<std::string>("precision");
  value.quantization = r.get<std::string>("quantization");
  value.layout = r.get<std::string>("layout");
  value.padding = r.get<std::string>("padding");
  value.maxSourceBytes = r.get<std::uint64_t>("maxSourceBytes");
  value.maxAssembledBytes = r.get<std::uint64_t>("maxAssembledBytes");
  value.maxNodes = r.get<std::uint64_t>("maxNodes");
  return value;
}

NativeCanonicalSource sourceFromRow(const boost::property_tree::ptree& row)
{
  NativeCanonicalSource source{fromHex(row.get<std::string>("modelHex")), std::nullopt};
  if (row.get<std::string>("initializerHex", "").empty()) return source;
  source.initializerBytes = fromHex(row.get<std::string>("initializerHex"));
  return source;
}

// Every vector rejection is asserted by its exact reason family, never by
// text produced by this assembler.
void expectNativeReject(const boost::property_tree::ptree& row, const std::string& code)
{
  try {
    (void)assembleNativeCertifiedOnnxModel(sourceFromRow(row), recipeFromVector(row),
                                           extractionControl());
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(std::string(error.what()), "DI_NATIVE_ONNX_" + code);
    return;
  }
  BOOST_FAIL("vector row expected DI_NATIVE_ONNX_" + code + " but assembled");
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182OnnxExtraction)

BOOST_AUTO_TEST_CASE(AcceptsInlineWholeModelByteParity)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "expectedModelDigest",
                            "sha256:59269bc795b453dcc15fcb34dd6c9b1625070bc094f32361fcd236c3b6600383");
  const auto result = assembleNativeCertifiedOnnxModel(
    sourceFromRow(row), recipeFromVector(row), extractionControl());
  BOOST_CHECK_EQUAL(toHex(result.modelBytes), row.get<std::string>("expectedModelHex"));
  BOOST_CHECK_EQUAL(result.modelDigest, row.get<std::string>("expectedModelDigest"));
  // Independent cross-check: the returned digest is the sha256 of the returned
  // bytes, recomputed here rather than trusted from the assembler.
  BOOST_CHECK_EQUAL(sha256HexOf(result.modelBytes), result.modelDigest);
  BOOST_CHECK_EQUAL(result.nodeCount, row.get<std::uint64_t>("expectedNodeCount"));
  BOOST_CHECK_EQUAL(result.inputNames.size(), 1U);
  BOOST_CHECK_EQUAL(result.inputNames.at(0), "X");
  BOOST_CHECK_EQUAL(result.outputNames.at(0), "Y");
  BOOST_CHECK(row.get<bool>("byteParity"));  // frozen marker: parity is expected here
}

BOOST_AUTO_TEST_CASE(AcceptsExternalSourceModelByteParity)
{
  // byteParity True: the externally sourced W1 is inlined from the side file
  // with its proto3-optional presence bit intact (onnx.proto field 14,
  // `optional DataLocation data_location = 14`), so both loaders keep the
  // explicit-default wire tag 0x70 and the frozen python bytes are reproduced
  // exactly.  On top of byte equality, the parse asserts pin the semantics:
  // node cover, io boundary, DEFAULT location, and the inlined payload bytes.
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "expectedModelDigest",
                            "sha256:f9fb15fd4403906f667de752caa70ed35182a922acc7f25fbccb28b3c55697b8");
  const auto result = assembleNativeCertifiedOnnxModel(
    sourceFromRow(row), recipeFromVector(row), extractionControl());
  BOOST_CHECK(row.get<bool>("byteParity"));  // frozen marker: parity is expected here
  BOOST_CHECK_EQUAL(toHex(result.modelBytes), row.get<std::string>("expectedModelHex"));
  BOOST_CHECK_EQUAL(result.modelDigest, row.get<std::string>("expectedModelDigest"));
  // Independent cross-check: the returned digest is the sha256 of the returned
  // bytes, recomputed here rather than trusted from the assembler.
  BOOST_CHECK_EQUAL(sha256HexOf(result.modelBytes), result.modelDigest);
  BOOST_CHECK_EQUAL(result.nodeCount, row.get<std::uint64_t>("expectedNodeCount"));
  onnx::ModelProto assembled;
  BOOST_REQUIRE(assembled.ParseFromArray(result.modelBytes.data(),
                                          static_cast<int>(result.modelBytes.size())));
  BOOST_REQUIRE_EQUAL(assembled.graph().node_size(), 2);
  BOOST_CHECK_EQUAL(assembled.graph().node(0).op_type(), "Add");
  BOOST_CHECK_EQUAL(assembled.graph().node(1).op_type(), "Relu");
  BOOST_REQUIRE_EQUAL(assembled.graph().initializer_size(), 1);
  const auto& weight = assembled.graph().initializer(0);
  BOOST_CHECK_EQUAL(weight.name(), "W1");
  BOOST_CHECK_EQUAL(weight.data_location(), onnx::TensorProto::DEFAULT);
  BOOST_CHECK_EQUAL(weight.data_type(), onnx::TensorProto::FLOAT);
  BOOST_CHECK_EQUAL(toHex(std::vector<std::uint8_t>(weight.raw_data().begin(),
                                                    weight.raw_data().end())),
                    row.get<std::string>("initializerHex"));
}

BOOST_AUTO_TEST_CASE(AcceptsRankSubsetWithInternalIoBoundary)
{
  // RANK role slice ending at the internal tensor T1: the boundary io is an
  // original value_info name, not a graph output, and the assembled wire must
  // still be byte-identical to the frozen python extractor output.
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "expectedModelDigest",
                            "sha256:354001a306647b226b3aef19a592630678eea02ea50a8ba34722e9da9d7bc12f");
  const auto result = assembleNativeCertifiedOnnxModel(
    sourceFromRow(row), recipeFromVector(row), extractionControl());
  BOOST_CHECK_EQUAL(toHex(result.modelBytes), row.get<std::string>("expectedModelHex"));
  BOOST_CHECK_EQUAL(result.modelDigest, row.get<std::string>("expectedModelDigest"));
  // Independent cross-check: the returned digest is the sha256 of the returned
  // bytes, recomputed here rather than trusted from the assembler.
  BOOST_CHECK_EQUAL(sha256HexOf(result.modelBytes), result.modelDigest);
  BOOST_CHECK_EQUAL(result.nodeCount, 1U);
  BOOST_CHECK_EQUAL(result.outputNames.size(), 1U);
  BOOST_CHECK_EQUAL(result.outputNames.at(0), "T1");
}

BOOST_AUTO_TEST_CASE(AcceptsCustomDomainFunctionModel)
{
  // A model whose certified slice refers a custom-domain local function:
  // the function body must ride along in the extracted wire and stay byte-
  // identical to the python reference (functions are copied in first-
  // reference order, like onnx.utils _collect_referred_local_functions).
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "expectedModelDigest",
                            "sha256:0d45e5672918a85e348e0c35805fd2f781a34c97e85cdb52556821ddf39abbad");
  const auto result = assembleNativeCertifiedOnnxModel(
    sourceFromRow(row), recipeFromVector(row), extractionControl());
  BOOST_CHECK_EQUAL(toHex(result.modelBytes), row.get<std::string>("expectedModelHex"));
  BOOST_CHECK_EQUAL(result.modelDigest, row.get<std::string>("expectedModelDigest"));
  // Independent cross-check: the returned digest is the sha256 of the returned
  // bytes, recomputed here rather than trusted from the assembler.
  BOOST_CHECK_EQUAL(sha256HexOf(result.modelBytes), result.modelDigest);
  BOOST_CHECK_EQUAL(result.nodeCount, row.get<std::uint64_t>("expectedNodeCount"));
  onnx::ModelProto assembled;
  BOOST_REQUIRE(assembled.ParseFromArray(result.modelBytes.data(),
                                          static_cast<int>(result.modelBytes.size())));
  BOOST_REQUIRE_EQUAL(assembled.functions_size(), 1);
  BOOST_CHECK_EQUAL(assembled.functions(0).name(), "SquareSpec182");
  BOOST_CHECK_EQUAL(assembled.functions(0).domain(), "ndnsf.spec182.extraction");
  BOOST_CHECK_EQUAL(assembled.graph().node(0).op_type(), "SquareSpec182");
  BOOST_CHECK_EQUAL(assembled.graph().node(0).domain(), "ndnsf.spec182.extraction");
}

BOOST_AUTO_TEST_CASE(RejectsRecipeDigestMismatch)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "canonical ONNX graph digest mismatch");
  expectNativeReject(row, "RECIPE");
}

BOOST_AUTO_TEST_CASE(RejectsGhostOutputBoundaryIo)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "adapter-certified ONNX extraction failed");
  expectNativeReject(row, "GRAPH");
}

BOOST_AUTO_TEST_CASE(RejectsNodeCoverShortfall)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "assembled ONNX node cover differs from the certificate");
  expectNativeReject(row, "NODE_COVER");
}

BOOST_AUTO_TEST_CASE(RejectsGhostInputByGraphResolution)
{
  // Python rejects this poison while constructing the recipe (redundant
  // input_names/contracts cross-check, text "expected_inputs does not cover
  // exact ONNX names"); natively the io names live only inside the contracts,
  // so the same poison surfaces at S5 when the certified boundary cannot
  // resolve against the graph, in the same reason family as the ghost-output
  // row (missing boundary io -> GRAPH).
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "expected_inputs does not cover exact ONNX names");
  expectNativeReject(row, "GRAPH");
}

BOOST_AUTO_TEST_CASE(RejectsIoDtypePoison)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "assembled ONNX input dtype/shape mismatch");
  const auto& recipeRow = recipeFromVector(row);
  BOOST_REQUIRE_EQUAL(recipeRow.expectedInputs.size(), 1U);
  BOOST_CHECK_EQUAL(recipeRow.expectedInputs.at(0).dtype, "int64");  // poison marker
  expectNativeReject(row, "IO_DTYPE");
}

BOOST_AUTO_TEST_CASE(RejectsIoShapePoison)
{
  const auto root = loadExtractionVectors();
  // Two rows share the frozen python text; the shape-poisoned row is the one
  // whose input contract keeps float32 but narrows the shape to [1,1].
  const auto& cases = root.get_child("cases");
  const boost::property_tree::ptree* shapeRow = nullptr;
  for (const auto& child : cases) {
    if (child.second.get<std::string>("pythonError", "") !=
        "assembled ONNX input dtype/shape mismatch")
      continue;
    const auto& inputContract = child.second.get_child("recipe.expectedInputs").begin();
    if (inputContract->second.get<std::string>("dtype") == "float32")
      shapeRow = &child.second;
  }
  BOOST_REQUIRE(shapeRow != nullptr);
  const auto contractShapes = stringChildren(
    shapeRow->get_child("recipe.expectedInputs").begin()->second.get_child("shape"));
  BOOST_REQUIRE_EQUAL(contractShapes.size(), 2U);
  BOOST_CHECK_EQUAL(contractShapes.at(0), "1");  // poison marker: [1,1] vs [2,2]
  expectNativeReject(*shapeRow, "IO_DTYPE");
}

BOOST_AUTO_TEST_CASE(RejectsLayerEscape)
{
  const auto root = loadExtractionVectors();
  const auto& row = caseRow(root.get_child("cases"), "pythonError",
                            "certified ONNX node cover escapes the graph");
  expectNativeReject(row, "LAYER_RANGE");
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// ---------------------------------------------------------------------------
// Spec182OnnxWorkerProtocol helpers (T006-C): fixed framing, metadata poisons,
// outcome revalidation JSON, and the registered test-worker binaries.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t>
craftRequestHeader(std::uint64_t metadataLength, std::uint64_t modelLength,
                   std::uint64_t initializerLength, std::uint8_t flag,
                   bool goodMagic)
{
  std::vector<std::uint8_t> header;
  const char* magic = goodMagic ? "NDI182A1" : "BAD182A1";
  header.insert(header.end(), magic, magic + 8);
  const auto pushU64 = [&header](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      header.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
  };
  pushU64(metadataLength);
  pushU64(modelLength);
  pushU64(initializerLength);
  header.push_back(flag);
  return header;
}

std::vector<std::uint8_t>
craftResponseHeader(std::uint32_t metadataLength, std::uint64_t modelLength,
                    std::uint8_t status, bool goodMagic)
{
  std::vector<std::uint8_t> header;
  const char* magic = goodMagic ? "NDI182R1" : "BAD182R1";
  header.insert(header.end(), magic, magic + 8);
  for (int i = 0; i < 4; ++i) {
    header.push_back(
      static_cast<std::uint8_t>(metadataLength >> (8 * i)));
  }
  for (int i = 0; i < 8; ++i) {
    header.push_back(static_cast<std::uint8_t>(modelLength >> (8 * i)));
  }
  header.push_back(status);
  return header;
}

// Sorted compact result metadata exactly as the worker child serializes it
// (inputNames < modelDigest < nodeCount < outputNames < schema).
std::string
resultMetadataJson(const std::vector<std::string>& inputs,
                   const std::vector<std::string>& outputs,
                   const std::string& digest, std::uint64_t nodeCount)
{
  const auto array = [](const std::vector<std::string>& names) {
    std::string text = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i != 0) text += ",";
      text += '"' + names[i] + '"';
    }
    return text + "]";
  };
  return std::string("{\"inputNames\":") + array(inputs) +
         ",\"modelDigest\":\"" + digest + "\",\"nodeCount\":" +
         std::to_string(nodeCount) + ",\"outputNames\":" + array(outputs) +
         ",\"schema\":\"" + kNativeOnnxAssemblyResultSchema + "\"}";
}

std::string
errorMetadataJson(const std::string& code, const std::string& message)
{
  return std::string("{\"code\":\"") + code + "\",\"message\":\"" + message +
         "\",\"schema\":\"" + kNativeOnnxAssemblyErrorSchema + "\"}";
}

// Run an action and return what() of the thrown std::exception, or fail the
// test when nothing is thrown.  Worker failures are single-family codes.
std::string
expectWorkerThrow(const std::function<void()>& action)
{
  try {
    action();
  }
  catch (const std::exception& error) {
    return error.what();
  }
  BOOST_FAIL("expected a worker failure to be thrown");
  return "";  // unreachable; BOOST_FAIL throws
}

NativeAssemblyControl
workerControl(std::chrono::steady_clock::time_point deadline)
{
  return NativeAssemblyControl{deadline, [] {}, 8 * 1024 * 1024,
                               8 * 1024 * 1024};
}

NativeAssemblyControl
workerControlFor(std::chrono::milliseconds window)
{
  return workerControl(std::chrono::steady_clock::now() + window);
}

// Accept fixture reused by every worker case: the frozen chain-whole-inline
// vector row whose assembled bytes are known byte-parity (Spec182OnnxExtraction
// AcceptsInlineWholeModelByteParity).  Returned by value: the vector root is
// loaded per call, so a reference must never outlive its ptree.
boost::property_tree::ptree
workerAcceptRow()
{
  const auto root = loadExtractionVectors();
  return caseRow(root.get_child("cases"), "expectedModelDigest",
                 "sha256:59269bc795b453dcc15fcb34dd6c9b1625070bc094f32361fcd236c3b6600383");
}

// Locate one built spec182 binary (the real installed worker or one of the
// misbehaving test tools) relative to the invocation directory, or via the
// NDNSF_SPEC182_BIN_DIR override.  Returns "" when the binary is absent so
// pure cases stay runnable without a build tree; subprocess cases fail loudly.
std::string
spec182BinaryPath(const std::string& basename)
{
  std::vector<std::string> candidates;
  const char* dir = std::getenv("NDNSF_SPEC182_BIN_DIR");
  if (dir != nullptr && *dir != '\0') {
    candidates.push_back(std::string(dir) + "/" + basename);
  }
  candidates.push_back("build-nac182/" + basename);
  candidates.push_back("build/" + basename);
  candidates.push_back("../" + basename);
  candidates.push_back(basename);
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (in) return path;
  }
  return "";
}

std::string
spec182RequireBinary(const std::string& basename)
{
  const std::string path = spec182BinaryPath(basename);
  BOOST_REQUIRE_MESSAGE(!path.empty(),
                        "spec182 worker binary not found: run the full waf "
                        "build (--targets=unit-tests,di-native-assembly-worker,"
                        "spec182-worker-tool-*) first (" << basename << ")");
  return path;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182OnnxWorkerProtocol)

// ---------------------------------------------------------------------------
// Pure frame parsing and decoder state.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(RequestFrameDecoderLifecycle)
{
  const std::vector<std::uint8_t> model{1, 2, 3, 4};
  const std::vector<std::uint8_t> frame =
    composeNativeOnnxWorkerRequest("{}", model, {}, false);
  BOOST_REQUIRE_EQUAL(frame.size(), 33U + 2U + model.size());

  NativeOnnxRequestDecoder decoder;
  BOOST_CHECK(decoder.feed(frame.data(), frame.size()) ==
              NativeOnnxRequestDecoder::Result::Complete);
  BOOST_CHECK(decoder.complete());
  BOOST_CHECK_EQUAL(decoder.header().metadataLength, 2U);
  BOOST_CHECK_EQUAL(decoder.header().modelLength, model.size());
  BOOST_CHECK_EQUAL(decoder.header().initializerLength, 0U);
  BOOST_CHECK(!decoder.header().hasInitializer);
  const std::vector<std::uint8_t> emptyObject{'{', '}'};
  BOOST_CHECK(decoder.metadata() == emptyObject);
  BOOST_CHECK(decoder.model() == model);
  BOOST_CHECK(decoder.initializer().empty());

  // Empty feeds are benign on a terminal state; any real byte after the
  // frame end is a duplicate/second frame.
  BOOST_CHECK(decoder.feed(nullptr, 0) ==
              NativeOnnxRequestDecoder::Result::Complete);
  const std::uint8_t stray = 'N';
  BOOST_CHECK(decoder.feed(&stray, 1) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);
  BOOST_CHECK(decoder.feed(&stray, 1) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);

  // One-byte trickles across feed calls must accumulate the 33-byte header
  // in member state (the regression that a stack header array would break)
  // and complete exactly at the frame boundary.
  NativeOnnxRequestDecoder trickle;
  for (std::size_t i = 0; i < frame.size(); ++i) {
    const auto outcome = trickle.feed(frame.data() + i, 1);
    const bool last = (i + 1 == frame.size());
    BOOST_CHECK_EQUAL(
      static_cast<int>(outcome),
      last ? static_cast<int>(NativeOnnxRequestDecoder::Result::Complete)
           : static_cast<int>(NativeOnnxRequestDecoder::Result::NeedMore));
  }
  BOOST_CHECK(trickle.model() == model);
}

BOOST_AUTO_TEST_CASE(RequestFrameTruncationNeedsMoreAtEveryPrefix)
{
  const std::vector<std::uint8_t> model(64, 0xAB);
  const std::vector<std::uint8_t> frame =
    composeNativeOnnxWorkerRequest("{}", model, {}, false);
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    NativeOnnxRequestDecoder decoder;
    BOOST_CHECK_EQUAL(
      static_cast<int>(decoder.feed(frame.data(), cut)),
      static_cast<int>(NativeOnnxRequestDecoder::Result::NeedMore));
  }
}

BOOST_AUTO_TEST_CASE(RequestFrameRejectsIncoherentHeaders)
{
  // Wrong magic, reserved flag value, and flag-0-with-initializer are all
  // protocol errors the moment the 33-byte header completes.
  std::vector<std::uint8_t> bad = craftRequestHeader(0, 0, 0, 0, false);
  NativeOnnxRequestDecoder badMagic;
  BOOST_CHECK(badMagic.feed(bad.data(), bad.size()) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);

  bad = craftRequestHeader(0, 0, 0, 2, true);
  NativeOnnxRequestDecoder reservedFlag;
  BOOST_CHECK(reservedFlag.feed(bad.data(), bad.size()) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);

  bad = craftRequestHeader(0, 0, 5, 0, true);
  NativeOnnxRequestDecoder hiddenInitializer;
  BOOST_CHECK(hiddenInitializer.feed(bad.data(), bad.size()) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);

  // flag-1 with a zero initializer length is permissive: the segment
  // completes with no payload byte and the frame finishes cleanly.
  const std::vector<std::uint8_t> emptyFlag1 =
    craftRequestHeader(0, 0, 0, 1, true);
  NativeOnnxRequestDecoder emptyInitializer;
  BOOST_CHECK(emptyInitializer.feed(emptyFlag1.data(), emptyFlag1.size()) ==
              NativeOnnxRequestDecoder::Result::Complete);
  BOOST_CHECK(emptyInitializer.header().hasInitializer);
  BOOST_CHECK(emptyInitializer.initializer().empty());
}

BOOST_AUTO_TEST_CASE(ResponseFrameDecoderLifecycle)
{
  const std::vector<std::uint8_t> model{0xAA, 0xBB};
  const std::vector<std::uint8_t> frame =
    composeNativeOnnxWorkerResponse(0, "{}", model);
  BOOST_REQUIRE_EQUAL(frame.size(), 21U + 2U + model.size());

  NativeOnnxResponseDecoder decoder;
  BOOST_CHECK(decoder.feed(frame.data(), frame.size()) ==
              NativeOnnxResponseDecoder::Result::Complete);
  BOOST_CHECK_EQUAL(decoder.header().metadataLength, 2U);
  BOOST_CHECK_EQUAL(decoder.header().modelLength, model.size());
  BOOST_CHECK_EQUAL(decoder.header().status, 0);
  const std::vector<std::uint8_t> emptyObject{'{', '}'};
  BOOST_CHECK(decoder.metadata() == emptyObject);
  BOOST_CHECK(decoder.model() == model);

  const std::uint8_t stray = 'N';
  BOOST_CHECK(decoder.feed(&stray, 1) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);

  // Error responses (status 1/2) never carry model bytes.
  const std::vector<std::uint8_t> errorFrame =
    composeNativeOnnxWorkerResponse(2, "{}", {});
  NativeOnnxResponseDecoder errorDecoder;
  BOOST_CHECK(errorDecoder.feed(errorFrame.data(), errorFrame.size()) ==
              NativeOnnxResponseDecoder::Result::Complete);
  BOOST_CHECK_EQUAL(errorDecoder.header().status, 2);
  BOOST_CHECK(errorDecoder.model().empty());

  // One-byte trickles (cross-call header accumulation).
  NativeOnnxResponseDecoder trickle;
  for (std::size_t i = 0; i < frame.size(); ++i) {
    const auto outcome = trickle.feed(frame.data() + i, 1);
    const bool last = (i + 1 == frame.size());
    BOOST_CHECK_EQUAL(
      static_cast<int>(outcome),
      last ? static_cast<int>(NativeOnnxResponseDecoder::Result::Complete)
           : static_cast<int>(NativeOnnxResponseDecoder::Result::NeedMore));
  }
}

BOOST_AUTO_TEST_CASE(ResponseFrameTruncationNeedsMoreAtEveryPrefix)
{
  const std::vector<std::uint8_t> model(64, 0xCD);
  const std::vector<std::uint8_t> frame =
    composeNativeOnnxWorkerResponse(0, "{}", model);
  for (std::size_t cut = 0; cut < frame.size(); ++cut) {
    NativeOnnxResponseDecoder decoder;
    BOOST_CHECK_EQUAL(
      static_cast<int>(decoder.feed(frame.data(), cut)),
      static_cast<int>(NativeOnnxResponseDecoder::Result::NeedMore));
  }
}

BOOST_AUTO_TEST_CASE(ResponseFrameRejectsOversizeAndIncoherentHeaders)
{
  // A declared metadata length beyond the 65536-byte worker cap is rejected
  // at header completion (overflow bound).
  std::vector<std::uint8_t> bad =
    craftResponseHeader(65537, 0, 1, true);
  NativeOnnxResponseDecoder oversize;
  BOOST_CHECK(oversize.feed(bad.data(), bad.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);

  bad = craftResponseHeader(0, 0, 1, false);  // wrong magic
  NativeOnnxResponseDecoder badMagic;
  BOOST_CHECK(badMagic.feed(bad.data(), bad.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);

  bad = craftResponseHeader(0, 0, 3, true);  // reserved status
  NativeOnnxResponseDecoder reservedStatus;
  BOOST_CHECK(reservedStatus.feed(bad.data(), bad.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);

  bad = craftResponseHeader(0, 0, 0, true);  // status 0 must carry a model
  NativeOnnxResponseDecoder emptyOk;
  BOOST_CHECK(emptyOk.feed(bad.data(), bad.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);

  bad = craftResponseHeader(0, 5, 1, true);  // error status with a model
  NativeOnnxResponseDecoder errorWithModel;
  BOOST_CHECK(errorWithModel.feed(bad.data(), bad.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);
}

BOOST_AUTO_TEST_CASE(SecondFrameIsAlwaysADuplicate)
{
  const std::vector<std::uint8_t> request =
    composeNativeOnnxWorkerRequest("{}", {1}, {}, false);
  const std::vector<std::uint8_t> duplicatedRequest(request.begin(),
                                                    request.end());
  NativeOnnxRequestDecoder requestDecoder;
  BOOST_CHECK(requestDecoder.feed(request.data(), request.size()) ==
              NativeOnnxRequestDecoder::Result::Complete);
  BOOST_CHECK(requestDecoder.feed(duplicatedRequest.data(),
                                  duplicatedRequest.size()) ==
              NativeOnnxRequestDecoder::Result::ProtocolError);

  const std::vector<std::uint8_t> response =
    composeNativeOnnxWorkerResponse(0, "{}", {1});
  NativeOnnxResponseDecoder responseDecoder;
  BOOST_CHECK(responseDecoder.feed(response.data(), response.size()) ==
              NativeOnnxResponseDecoder::Result::Complete);
  BOOST_CHECK(responseDecoder.feed(response.data(), response.size()) ==
              NativeOnnxResponseDecoder::Result::ProtocolError);
}

BOOST_AUTO_TEST_CASE(ComposeEnforcesFrameCoherence)
{
  BOOST_CHECK_THROW(composeNativeOnnxWorkerRequest("{}", {}, {1}, false),
                    std::runtime_error);  // flag 0 with a real initializer
  BOOST_CHECK_THROW(composeNativeOnnxWorkerRequest("{}", {1}, {}, true),
                    std::runtime_error);  // flag 1 without bytes
  BOOST_CHECK_THROW(composeNativeOnnxWorkerResponse(1, "{}", {1}),
                    std::runtime_error);  // error status with a model
  BOOST_CHECK_THROW(composeNativeOnnxWorkerResponse(0, "{}", {}),
                    std::runtime_error);  // ok without model bytes
  BOOST_CHECK_THROW(composeNativeOnnxWorkerResponse(
                      0, std::string(65537, 'a'), {1}),
                    std::runtime_error);  // metadata over the response cap
}

// ---------------------------------------------------------------------------
// Pure parent-side result revalidation.
// ---------------------------------------------------------------------------

namespace {
// Ok-response metadata consistent with the fixture recipe and model bytes.
std::string
okResultMetadata(const boost::property_tree::ptree& row,
                 const std::vector<std::uint8_t>& modelBytes,
                 std::uint64_t nodeCountOverride = 0)
{
  const auto& recipe = row.get_child("recipe");
  const auto names = [&recipe](const char* field) {
    std::vector<std::string> out;
    for (const auto& child : recipe.get_child(field)) {
      out.push_back(child.second.get<std::string>("name"));
    }
    return out;
  };
  const std::uint64_t nodeCount =
    nodeCountOverride != 0 ? nodeCountOverride
                           : recipe.get_child("nodeIndices").size();
  return resultMetadataJson(names("expectedInputs"), names("expectedOutputs"),
                            sha256HexOf(modelBytes), nodeCount);
}
} // namespace

BOOST_AUTO_TEST_CASE(FinalizeAcceptsValidOkResponse)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  const std::string metadata = okResultMetadata(row, model);
  const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, metadata, model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK(outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "");
  BOOST_CHECK(outcome.value.modelBytes == model);
  BOOST_CHECK_EQUAL(outcome.value.nodeCount, recipe.nodeIndices.size());
  BOOST_CHECK_EQUAL(outcome.value.modelDigest, sha256HexOf(model));
  BOOST_REQUIRE_EQUAL(outcome.value.inputNames.size(),
                      recipe.expectedInputs.size());
  BOOST_REQUIRE_EQUAL(outcome.value.outputNames.size(),
                      recipe.expectedOutputs.size());
  for (std::size_t i = 0; i < outcome.value.inputNames.size(); ++i) {
    BOOST_CHECK_EQUAL(outcome.value.inputNames[i],
                      recipe.expectedInputs[i].name);
  }
  for (std::size_t i = 0; i < outcome.value.outputNames.size(); ++i) {
    BOOST_CHECK_EQUAL(outcome.value.outputNames[i],
                      recipe.expectedOutputs[i].name);
  }
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsOkResponseFromNonzeroExit)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    false, 0, okResultMetadata(row, model), model, recipe,
    recipe.maxAssembledBytes, true);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_EXITED");
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsNonJsonResponseMetadata)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, "not json at all", model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsMalformedErrorResponse)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  const std::string longMessage(2048, 'x');

  // Wrong schema for an error response.
  NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 1, resultMetadataJson({"X"}, {"Y"}, sha256HexOf(model), 1), model,
    recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");

  // Error schema but no code member.
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 2,
    "{\"message\":\"m\",\"schema\":\"ndnsf-di-native-assembly-error-v1\"}",
    {}, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");

  // Code without the DI_NATIVE_ONNX_ prefix.
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 2, errorMetadataJson("NO_PREFIX_CODE", "m"), {}, recipe,
    recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");

  // Code beyond the 96-byte cap.
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 2, errorMetadataJson("DI_NATIVE_ONNX_" + std::string(96, 'C'), "m"),
    {}, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");

  // Message beyond the 1024-byte cap.
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 2, errorMetadataJson("DI_NATIVE_ONNX_RECIPE", longMessage), {},
    recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_PROTOCOL");
}

BOOST_AUTO_TEST_CASE(FinalizePropagatesChildErrorCode)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  // A status-1 algorithm reject carries the certified recipe failure code;
  // the parent revalidates and surfaces exactly that code.
  const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 1, errorMetadataJson("DI_NATIVE_ONNX_RECIPE", "g1"), {}, recipe,
    recipe.maxAssembledBytes, true);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_RECIPE");
  BOOST_CHECK_EQUAL(outcome.failureMessage, "g1");
}

BOOST_AUTO_TEST_CASE(FinalizeSuppressesLateSuccessAfterCancellation)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  // activeAfterResponse == false: the response is fully valid but the
  // cancellation already landed, so the late success must never publish.
  const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, okResultMetadata(row, model), model, recipe,
    recipe.maxAssembledBytes, false);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT");
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsEmptyAndOversizeModel)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  // Empty model bytes can never be an assembly result.
  NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, okResultMetadata(row, {}), {}, recipe,
    recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_RESULT");

  // Model beyond the request budget (the recipe cap itself would also trip).
  const std::vector<std::uint8_t> big(recipe.maxAssembledBytes + 1, 0x01);
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, okResultMetadata(row, big), big, recipe,
    recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_RESULT");
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsModelDigestMismatch)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);

  // Metadata digest does not match the bytes: always rejected, even though
  // the worker itself claimed the assembly succeeded.
  std::string metadata = okResultMetadata(row, model);
  const auto digestPos = metadata.find("\"modelDigest\":\"sha256:") +
                         std::string("\"modelDigest\":\"sha256:").size();
  metadata[digestPos + 63] = metadata[digestPos + 63] == '0' ? '1' : '0';
  NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, metadata, model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode,
                    "DI_NATIVE_ONNX_WORKER_RESULT_DIGEST");

  // Malformed digest text is a malformed result, not a digest mismatch.
  std::string malformed = okResultMetadata(row, model);
  const auto malformedPos =
    malformed.find("\"modelDigest\":\"") + std::string("\"modelDigest\":\"").size();
  malformed.replace(malformedPos, 7, "nope:");
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, malformed, model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK(!outcome.ok);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_RESULT");
}

BOOST_AUTO_TEST_CASE(FinalizeRejectsCountAndIoNameMismatch)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::vector<std::uint8_t> model(32, 0x7A);
  NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, okResultMetadata(row, model, recipe.nodeIndices.size() + 1),
    model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_RESULT");

  // The node count matches but the io names do not cover the certified
  // contracts in the contract order.
  const std::string renamed = resultMetadataJson(
    {"X_NOT_THE_CERTIFIED_NAME"}, {"Y"}, sha256HexOf(model),
    recipe.nodeIndices.size());
  outcome = finalizeNativeOnnxWorkerResponse(
    true, 0, renamed, model, recipe, recipe.maxAssembledBytes, true);
  BOOST_CHECK_EQUAL(outcome.failureCode, "DI_NATIVE_ONNX_WORKER_RESULT");
}

// ---------------------------------------------------------------------------
// Child-side S1 metadata validation over the worker envelope.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(MetadataAcceptsCanonicalEnvelopeRoundtrip)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::string envelope = buildNativeOnnxWorkerRequestMetadata(recipe);

  const NativeOnnxMetadataCheck check =
    validateNativeOnnxWorkerMetadata(envelope);
  BOOST_CHECK(check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "");
  BOOST_CHECK_EQUAL(check.value.schema, kNativeOnnxAssemblyRequestSchema);

  // Independent digest parity: sha256 over the canonical recipe JSON bytes
  // recomputed here equals the envelope digest and the parsed value.
  const std::string canonical = canonicalNativeOnnxRecipeJson(recipe);
  const auto canonicalBytes =
    std::vector<std::uint8_t>(canonical.begin(), canonical.end());
  BOOST_CHECK_EQUAL(check.value.recipeDigest, sha256HexOf(canonicalBytes));
  BOOST_CHECK_EQUAL(check.value.recipe.graphDigest, recipe.graphDigest);
  BOOST_CHECK_EQUAL(check.value.recipe.backend, recipe.backend);
  BOOST_REQUIRE_EQUAL(check.value.recipe.expectedInputs.size(),
                      recipe.expectedInputs.size());
  BOOST_CHECK_EQUAL(check.value.recipe.expectedInputs[0].name,
                    recipe.expectedInputs[0].name);
}

BOOST_AUTO_TEST_CASE(MetadataRejectsNonJsonAndEnvelopePoisons)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::string envelope = buildNativeOnnxWorkerRequestMetadata(recipe);

  NativeOnnxMetadataCheck check = validateNativeOnnxWorkerMetadata(
    "this is not JSON");
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_WORKER_METADATA");

  // Dropping the envelope schema member.
  const auto dropSchema = [](std::string text) {
    const std::string member = ",\"schema\":\"" +
      std::string(kNativeOnnxAssemblyRequestSchema) + "\"";
    const auto pos = text.find(member);
    BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "envelope schema member");
    return text.erase(pos, member.size());
  };
  check = validateNativeOnnxWorkerMetadata(dropSchema(envelope));
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_WORKER_METADATA");

  // Wrong envelope schema name.
  const auto swapSchema = [](std::string text) {
    const std::string from = std::string(kNativeOnnxAssemblyRequestSchema);
    const auto pos = text.find(from);
    BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "envelope schema value");
    return text.replace(pos, from.size(), kNativeOnnxAssemblyResultSchema);
  };
  check = validateNativeOnnxWorkerMetadata(swapSchema(envelope));
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_WORKER_METADATA");

  // Dropping the top-level backend binding (unique anchor: the recipe JSON
  // member is followed by backendAbi, never by "recipe").
  const auto dropBackend = [](std::string text) {
    const std::string member = ",\"backend\":\"onnxruntime\",\"recipe\"";
    const auto pos = text.find(member);
    BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "envelope backend member");
    return text.erase(pos, std::string(",\"backend\":\"onnxruntime\"").size());
  };
  check = validateNativeOnnxWorkerMetadata(dropBackend(envelope));
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_WORKER_METADATA");
}

BOOST_AUTO_TEST_CASE(MetadataRejectsDigestFormatAndPayloadMismatch)
{
  const auto& row = workerAcceptRow();
  const auto recipe = recipeFromVector(row);
  const std::string envelope = buildNativeOnnxWorkerRequestMetadata(recipe);

  // A malformed digest prefix is an envelope error; a digest that no longer
  // matches the canonical recipe payload is a certified-recipe rejection.
  const std::string marker = "\"recipeDigest\":\"sha256:";
  const auto digestHexAt = [&marker](std::string poisoned) {
    const auto pos = poisoned.find(marker);
    BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "envelope recipeDigest");
    return pos + marker.size();
  };

  std::string badFormat = envelope;
  badFormat.replace(badFormat.find(marker) + marker.size() - 7, 7, "md5:");
  NativeOnnxMetadataCheck check =
    validateNativeOnnxWorkerMetadata(badFormat);
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_WORKER_METADATA");

  std::string mismatch = envelope;
  const std::size_t hexAt = digestHexAt(mismatch);
  mismatch[hexAt + 63] = mismatch[hexAt + 63] == '0' ? '1' : '0';
  check = validateNativeOnnxWorkerMetadata(mismatch);
  BOOST_CHECK(!check.ok);
  BOOST_CHECK_EQUAL(check.failureCode, "DI_NATIVE_ONNX_RECIPE");
}

// ---------------------------------------------------------------------------
// Real-subprocess transport (OA02) against the built worker and the
// deliberately-misbehaving test tools.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(SubprocessUnregisteredThenRegisteredMatchesInProcess)
{
  const std::string worker = spec182RequireBinary("DI_NativeOnnxAssemblyWorker");
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  const auto control = extractionControl();

  // Before any registration the default entry refuses to spawn: there is no
  // fixed worker identity yet and no PATH fallback exists.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] { runNativeOnnxAssemblyWorker(source, recipe,
                                                        control); }),
    "DI_NATIVE_ONNX_WORKER_UNREGISTERED");

  const NativeCertifiedAssembly inProcess = assembleInProcess(source, recipe);

  registerNativeOnnxWorkerLocation({worker, ""});
  const NativeCertifiedAssembly viaWorker =
    runNativeOnnxAssemblyWorker(source, recipe, control);

  // Byte identity is asserted through digests: the worker's own claimed
  // digest, the independent recomputation over the returned bytes, and the
  // in-process chain must all agree.
  BOOST_CHECK_EQUAL(viaWorker.modelDigest, inProcess.modelDigest);
  BOOST_CHECK_EQUAL(viaWorker.modelBytes.size(), inProcess.modelBytes.size());
  BOOST_CHECK_EQUAL(sha256HexOf(viaWorker.modelBytes), viaWorker.modelDigest);
  BOOST_CHECK_EQUAL(viaWorker.nodeCount, inProcess.nodeCount);
  BOOST_CHECK(viaWorker.inputNames == inProcess.inputNames);
  BOOST_CHECK(viaWorker.outputNames == inProcess.outputNames);
}

BOOST_AUTO_TEST_CASE(SubprocessCrashBySignalReported)
{
  const std::string tool =
    spec182RequireBinary("spec182-worker-tool-sigkill");
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  // A child that ends its stdout by dying from SIGKILL must be classified by
  // the signal, not as an incomplete or exited worker.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({tool, ""}, source, recipe,
                                    extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_SIGNALED");
}

BOOST_AUTO_TEST_CASE(SubprocessCancelBeforeStartAndActiveGate)
{
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  // The deadline expired before the call: the entry gate refuses to spawn.
  NativeAssemblyControl expired = workerControl(
    std::chrono::steady_clock::now() - std::chrono::seconds(5));
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({"/does/not/matter", ""}, source, recipe,
                                    expired);
    }),
    "DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT");

  // The request's own requireActive callback cancels before any spawn.
  NativeAssemblyControl callbackCancelled = workerControl(
    std::chrono::steady_clock::now() + std::chrono::seconds(30));
  callbackCancelled.requireActive = [] {
    throw std::runtime_error("test-cancel-requested");
  };
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({"/does/not/matter", ""}, source, recipe,
                                    callbackCancelled);
    }),
    "test-cancel-requested");
}

BOOST_AUTO_TEST_CASE(SubprocessCancelDuringBlockingWorkerEscalatesKill)
{
  const std::string tool =
    spec182RequireBinary("spec182-worker-tool-block");
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  const auto started = std::chrono::steady_clock::now();

  // The blocking tool ignores SIGTERM, so the transport must spend its full
  // 1s TERM window and escalate to SIGKILL before returning the timeout.
  const std::string code = expectWorkerThrow([&] {
    runNativeOnnxAssemblyWorkerAt({tool, ""}, source, recipe,
                                  workerControlFor(std::chrono::milliseconds(250)));
  });
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started).count();
  BOOST_CHECK_EQUAL(code, "DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT");
  BOOST_CHECK(elapsedMs >= 1000);
  BOOST_CHECK(elapsedMs < 5000);
}

BOOST_AUTO_TEST_CASE(SubprocessSilentExitsReported)
{
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  const std::string silent0 =
    spec182RequireBinary("spec182-worker-tool-silent0");
  // Exit zero but no response frame: the stream ended before any result.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({silent0, ""}, source, recipe,
                                    extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_INCOMPLETE");

  const std::string silent7 =
    spec182RequireBinary("spec182-worker-tool-silent7");
  // Nonzero exit without a frame and without a signal.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({silent7, ""}, source, recipe,
                                    extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_EXITED");
}

BOOST_AUTO_TEST_CASE(SubprocessGarbageStdoutRejected)
{
  const std::string tool =
    spec182RequireBinary("spec182-worker-tool-garbage");
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);
  // stdout that is not the framed protocol at all must be rejected as a
  // protocol error even though the child exits cleanly.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({tool, ""}, source, recipe,
                                    extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_PROTOCOL");
}

BOOST_AUTO_TEST_CASE(PreflightRejectsMissingAndRehashedLocations)
{
  const std::string worker = spec182RequireBinary("DI_NativeOnnxAssemblyWorker");
  const auto& row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto recipe = recipeFromVector(row);

  const std::string zeroSha = "sha256:" + std::string(64, '0');

  // Registration preflight: an unreadable path and a registered identity
  // that does not match the file on disk both refuse to register.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      registerNativeOnnxWorkerLocation({"/definitely/missing/worker", ""});
    }),
    "DI_NATIVE_ONNX_WORKER_PREFLIGHT");
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      registerNativeOnnxWorkerLocation({worker, zeroSha});
    }),
    "DI_NATIVE_ONNX_WORKER_PREFLIGHT");

  // Spawn preflight over an explicit location: each run re-hashes the file
  // and refuses when the identity changed.
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({"/definitely/missing/worker", ""},
                                    source, recipe, extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_PREFLIGHT");
  BOOST_CHECK_EQUAL(
    expectWorkerThrow([&] {
      runNativeOnnxAssemblyWorkerAt({worker, zeroSha}, source, recipe,
                                    extractionControl());
    }),
    "DI_NATIVE_ONNX_WORKER_PREFLIGHT");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
