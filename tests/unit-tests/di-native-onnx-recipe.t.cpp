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
#include <fstream>
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

} // namespace ndnsf::di
