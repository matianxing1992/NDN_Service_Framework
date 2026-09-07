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

#include <fstream>
#include <functional>
#include <stdexcept>

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

// One branch graph carries an EXTERNAL initializer consumed by an Identity
// node; the other carries the same structure inline.  The model is full-
// checker valid (bool scalar condition, both If branches, typed io and
// branch outputs), so the certified S3 full check, S5 InferShapes and the S7
// ORT CPU session load all accept it once external tensors are inlined.
std::vector<std::uint8_t> modelBytesWithNestedExternal()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("spec182-native-nested-external");
  auto* input = graph->add_input();
  input->set_name("cond");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::BOOL);
  // Scalar (zero-dim) shape: the full checker requires the type shape field on
  // io values even when the value is a rank-0 scalar condition.
  input->mutable_type()->mutable_tensor_type()->mutable_shape();
  auto* output = graph->add_output();
  output->set_name("y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  output->mutable_type()->mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  auto* node = graph->add_node();
  node->set_op_type("If");
  node->add_input("cond");
  node->add_output("y");

  const auto addBranch = [](onnx::NodeProto* ifNode, const char* name,
                            bool external) {
    auto* attribute = ifNode->add_attribute();
    attribute->set_name(name);
    attribute->set_type(onnx::AttributeProto::GRAPH);
    auto* nested = attribute->mutable_g();
    nested->set_name(name);
    auto* weight = nested->add_initializer();
    weight->set_name(external ? "nested-weight" : "else-weight");
    weight->set_data_type(onnx::TensorProto::FLOAT);
    weight->add_dims(1);
    weight->add_dims(1);
    if (external) {
      weight->set_data_location(onnx::TensorProto::EXTERNAL);
      auto* location = weight->add_external_data();
      location->set_key("location");
      location->set_value("weights.bin");
      auto* offset = weight->add_external_data();
      offset->set_key("offset");
      offset->set_value("0");
      auto* length = weight->add_external_data();
      length->set_key("length");
      length->set_value("4");
    }
    else {
      weight->set_raw_data("\x00\x00\x00\x40", 4);
    }
    auto* identity = nested->add_node();
    identity->set_op_type("Identity");
    identity->set_name(std::string(name) + "_id");
    identity->add_input(weight->name());
    identity->add_output("y");
    auto* branchOutput = nested->add_output();
    branchOutput->set_name("y");
    branchOutput->mutable_type()->mutable_tensor_type()->set_elem_type(
      onnx::TensorProto::FLOAT);
    branchOutput->mutable_type()->mutable_tensor_type()->mutable_shape()
      ->add_dim()->set_dim_value(1);
    branchOutput->mutable_type()->mutable_tensor_type()->mutable_shape()
      ->add_dim()->set_dim_value(1);
  };
  addBranch(node, "then_branch", true);
  addBranch(node, "else_branch", false);
  const auto size = model.ByteSizeLong();
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  BOOST_REQUIRE(model.SerializeToArray(bytes.data(), static_cast<int>(bytes.size())));
  return bytes;
}

NativeAssemblyControl identityControl(std::uint64_t maxSourceBytes = 8 * 1024 * 1024)
{
  return NativeAssemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(5), [] {}, maxSourceBytes,
    maxSourceBytes};
}

// These fixtures predate the certified S3-S7 gates, which reject any recipe
// whose digests do not match the canonical identity of the exact source
// bytes (S4).  Fill the digests from the runtime identity of the source the
// case assembles so the case exercises the certified pipeline instead of
// tripping the certificate gate.
void fillCertifiedIdentity(NativeCertifiedRecipe& recipe,
                           const NativeCanonicalSource& source)
{
  const auto identity = canonicalOnnxSourceIdentity(source, identityControl());
  recipe.graphDigest = identity.graphDigest;
  recipe.canonicalInitializerDigest = identity.initializerDigest;
}

// Assert the exact registered DI_NATIVE_ONNX_* reason family, never a
// textual expectation produced by this implementation.
void expectReason(const std::function<void()>& call, const std::string& code)
{
  try {
    call();
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(std::string(error.what()), "DI_NATIVE_ONNX_" + code);
    return;
  }
  BOOST_FAIL("expected DI_NATIVE_ONNX_" + code + " but the call succeeded");
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativeAssembly)

BOOST_AUTO_TEST_CASE(AssemblesComponentSetWithoutInterpreter)
{
  NativeCanonicalSource source{modelBytes(), std::nullopt};
  auto certified = recipe();
  fillCertifiedIdentity(certified, source);
  const auto control = NativeAssemblyControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, 64 * 1024, 64 * 1024};
  const auto result = assembleNativeCertifiedOnnxModel(source, certified, control);
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
  expectReason([&] {
    assembleNativeCertifiedOnnxModel(NativeCanonicalSource{modelBytes(), std::nullopt},
                                     invalid, control);
  }, "NODE_COVER");
}

BOOST_AUTO_TEST_CASE(InlinesNestedGraphExternalInitializers)
{
  NativeCanonicalSource source{modelBytesWithNestedExternal(),
                               std::vector<std::uint8_t>{1, 2, 3, 4}};
  auto certified = recipe();
  certified.expectedInputs = {{"cond", "bool", {}}};
  certified.expectedOutputs = {{"y", "float32", {"1", "1"}}};
  fillCertifiedIdentity(certified, source);
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, 64 * 1024, 64 * 1024};
  const auto result = assembleNativeCertifiedOnnxModel(source, certified, control);
  onnx::ModelProto assembled;
  BOOST_REQUIRE(assembled.ParseFromArray(result.modelBytes.data(),
                                          static_cast<int>(result.modelBytes.size())));
  BOOST_REQUIRE_EQUAL(assembled.graph().node_size(), 1);
  const auto& attributes = assembled.graph().node(0).attribute();
  // The If node keeps both branch graphs (node cover is per-node and its
  // deterministic bytes must equal the original node), so locate the branch
  // that carried the EXTERNAL tensor by name instead of by position.
  BOOST_REQUIRE_EQUAL(attributes.size(), 2);
  const onnx::AttributeProto* thenBranch = nullptr;
  for (const auto& attribute : attributes) {
    if (attribute.name() == "then_branch") thenBranch = &attribute;
  }
  BOOST_REQUIRE(thenBranch != nullptr);
  BOOST_REQUIRE(thenBranch->has_g());
  BOOST_REQUIRE_EQUAL(thenBranch->g().initializer_size(), 1);
  const auto& initializer = thenBranch->g().initializer(0);
  BOOST_CHECK_EQUAL(initializer.name(), "nested-weight");
  BOOST_CHECK_EQUAL(initializer.data_location(), onnx::TensorProto::DEFAULT);
  BOOST_CHECK_EQUAL(initializer.raw_data(),
                    std::string("\x01\x02\x03\x04", 4));  // side payload {1,2,3,4}
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// ---------------------------------------------------------------------------
// Spec182OnnxIdentity fixture plumbing (boost::property_tree JSON vectors).
// ---------------------------------------------------------------------------

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

boost::property_tree::ptree loadFixture(const std::string& fileName)
{
  namespace pt = boost::property_tree;
  const std::string candidates[] = {
    "tests/fixtures/spec182/dependency-probes/" + fileName,
    "../tests/fixtures/spec182/dependency-probes/" + fileName,
  };
  for (const auto& path : candidates) {
    std::ifstream in(path);
    if (in) {
      pt::ptree root;
      pt::read_json(in, root);
      return root;
    }
  }
  throw std::runtime_error("fixture not found: " + fileName);
}

// JSON arrays parse to children with empty keys, so rows are found by
// scanning their "name" field rather than by path.
const boost::property_tree::ptree&
fixtureRow(const boost::property_tree::ptree& cases, const std::string& name)
{
  for (const auto& child : cases) {
    if (child.second.get<std::string>("name") == name) return child.second;
  }
  throw std::runtime_error("fixture row not found: " + name);
}

// Whole-model golden runner shared by the 24 numeric and 14 extended cases.
void checkWholeModelGolden(const boost::property_tree::ptree& row,
                           const std::string& description)
{
  const auto modelBytes = fromHex(row.get<std::string>("modelHex"));
  BOOST_CHECK_MESSAGE(sha256HexOf(modelBytes) == row.get<std::string>("modelDigest"),
                      description + " modelHex decodes to the pinned model bytes");
  NativeCanonicalSource source{modelBytes, std::nullopt};
  const auto identity = canonicalOnnxSourceIdentity(source, identityControl());
  BOOST_CHECK_MESSAGE(identity.graphDigest == row.get<std::string>("graphDigest"),
                      description + " graphDigest");
  BOOST_CHECK_MESSAGE(identity.initializerDigest == row.get<std::string>("initializerDigest"),
                      description + " initializerDigest");
  // The single top-level initializer's payload must reproduce the frozen
  // per-tensor content digest stored in the vector row.
  onnx::ModelProto model;
  BOOST_REQUIRE_MESSAGE(
    model.ParseFromArray(modelBytes.data(), static_cast<int>(modelBytes.size())),
    description + " model parses");
  BOOST_REQUIRE_MESSAGE(model.graph().initializer_size() == 1,
                        description + " has exactly one initializer");
  const auto& initializer = model.graph().initializer(0);
  std::vector<std::uint8_t> serialized(static_cast<std::size_t>(initializer.ByteSizeLong()));
  BOOST_REQUIRE(initializer.SerializeToArray(serialized.data(),
                                             static_cast<int>(serialized.size())));
  const auto payload = normalizedOnnxInitializerPayload(serialized);
  BOOST_CHECK_MESSAGE(sha256HexOf(payload.content) == row.get<std::string>("contentDigest"),
                      description + " initializer contentDigest");
  const auto& tensorIndex = row.get_child("tensorIndex");
  BOOST_REQUIRE_EQUAL(tensorIndex.size(), 1U);
  for (const auto& entry : tensorIndex) {
    BOOST_CHECK_MESSAGE(payload.content.size() ==
                          static_cast<std::size_t>(std::stoull(entry.second.get<std::string>("byteLength"))),
                        description + " tensorIndex byteLength");
    BOOST_CHECK_MESSAGE(payload.dtype == entry.second.get<std::string>("dtype"),
                        description + " tensorIndex dtype");
  }
}

onnx::TensorProto makeTensorFromSpec(const boost::property_tree::ptree& spec)
{
  onnx::TensorProto tensor;
  const auto dataType = spec.get<std::string>("dataType");
  tensor.set_data_type(dataType == "BFLOAT16" ? onnx::TensorProto::BFLOAT16
                     : dataType == "COMPLEX64" ? onnx::TensorProto::COMPLEX64
                     : dataType == "COMPLEX128" ? onnx::TensorProto::COMPLEX128
                     : dataType == "STRING" ? onnx::TensorProto::STRING
                     : dataType == "INT4" ? onnx::TensorProto::INT4
                     : dataType == "UINT4" ? onnx::TensorProto::UINT4
                     : onnx::TensorProto::UNDEFINED);
  for (const auto& dim : spec.get_child("dims"))
    tensor.add_dims(std::stoll(dim.second.data()));
  if (auto raw = spec.get_child_optional("rawDataHex")) {
    const auto bytes = fromHex(raw->data());
    tensor.set_raw_data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  if (auto typed = spec.get_child_optional("int32Data")) {
    for (const auto& value : *typed) tensor.add_int32_data(std::stoi(value.second.data()));
  }
  if (auto typed = spec.get_child_optional("floatData")) {
    for (const auto& value : *typed) tensor.add_float_data(std::stof(value.second.data()));
  }
  if (auto typed = spec.get_child_optional("doubleData")) {
    for (const auto& value : *typed) tensor.add_double_data(std::stod(value.second.data()));
  }
  if (auto typed = spec.get_child_optional("stringDataHex")) {
    for (const auto& value : *typed) {
      const auto bytes = fromHex(value.second.data());
      tensor.add_string_data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
  }
  return tensor;
}

// Minimal deterministic single-initializer model: ir_version 8, one
// ""-domain opset 13, Identity node x -> y over the weight initializer.
onnx::ModelProto singleInitializerModel(onnx::TensorProto initializer)
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("spec182-identity");
  auto* input = graph->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* output = graph->add_output();
  output->set_name("y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* node = graph->add_node();
  node->set_op_type("Identity");
  node->add_input("x");
  node->add_output("y");
  initializer.set_name("weight");
  *graph->add_initializer() = std::move(initializer);
  return model;
}

std::vector<std::uint8_t> serialize(const google::protobuf::MessageLite& message)
{
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(message.ByteSizeLong()));
  if (!bytes.empty())
    BOOST_REQUIRE(message.SerializeToArray(bytes.data(), static_cast<int>(bytes.size())));
  return bytes;
}

void expectPrefix(const std::function<void()>& fn, const std::string& prefix,
                  const std::string& description)
{
  bool threw = false;
  try {
    fn();
  } catch (const std::runtime_error& error) {
    threw = true;
    BOOST_CHECK_MESSAGE(
      prefix.compare(0, std::string::npos, error.what(), prefix.size()) == 0,
      description + " expected prefix " + prefix + " got " + error.what());
  }
  BOOST_CHECK_MESSAGE(threw, description + " expected an error");
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182OnnxIdentity)

// The 24 numeric full-model vectors: frozen by the pinned python reference
// e5532328.. (referenceSourceSha256 in the file), reproduced byte-exact here.
BOOST_AUTO_TEST_CASE(WholeModelNumericVectorsMatchFrozenGoldens)
{
  const auto root = loadFixture("identity-vectors.json");
  int caseIndex = 0;
  for (const auto& row : root.get_child("cases")) {
    checkWholeModelGolden(row.second, "v1 " + row.second.get<std::string>("name"));
    ++caseIndex;
  }
  BOOST_CHECK_EQUAL(caseIndex, 24);
}

// The 14 extended full-model vectors: complex raw + FLOAT8/INT4/UINT4 raw and
// typed encodings under the same oracle chain.
BOOST_AUTO_TEST_CASE(WholeModelExtendedVectorsMatchFrozenGoldens)
{
  const auto root = loadFixture("identity-extended-vectors.json");
  int acceptedCount = 0;
  int rejectedCount = 0;
  for (const auto& row : root.get_child("cases")) {
    const bool accepted = row.second.get<bool>("accepted");
    const auto name = row.second.get<std::string>("name");
    if (accepted) {
      checkWholeModelGolden(row.second, "extended " + name);
      ++acceptedCount;
    } else {
      // The two legacy-typed complex cases are rejected by the old numpy
      // conversion but remain v2-normalizable; they are covered by the v2
      // hand vectors and the revision gate, not by a stored digest row.
      ++rejectedCount;
    }
  }
  BOOST_CHECK_EQUAL(acceptedCount, 14);
  BOOST_CHECK_EQUAL(rejectedCount, 2);
}

// typed/raw same-dtype pairs must normalize to identical canonical bytes,
// which the layout and initializer digests inherit; this also arbitrates the
// float16-typed representation (bit-copy, not numeric conversion).
BOOST_AUTO_TEST_CASE(RawAndTypedPairsProduceIdenticalDigests)
{
  const auto root = loadFixture("identity-vectors.json");
  const auto& cases = root.get_child("cases");
  for (const auto& row : cases) {
    const auto name = row.second.get<std::string>("name");
    const bool typed = row.second.get<std::string>("encoding") == "typed";
    if (!typed) continue;
    const auto& raw = fixtureRow(cases, name.substr(0, name.size() - 6) + "-raw");
    BOOST_CHECK_MESSAGE(row.second.get<std::string>("contentDigest") ==
                          raw.get<std::string>("contentDigest"),
                        name + " typed/raw content bytes identical");
    BOOST_CHECK_MESSAGE(row.second.get<std::string>("initializerDigest") ==
                          raw.get<std::string>("initializerDigest"),
                        name + " typed/raw initializerDigest identical");
    BOOST_CHECK_MESSAGE(row.second.get<std::string>("graphDigest") ==
                          raw.get<std::string>("graphDigest"),
                        name + " typed/raw graphDigest identical");
  }
}

// The 17 per-tensor v2 hand vectors (payloadHex/byteLength/dtype/shape/
// byteOrder/contentDigest are the spec-defined ground truth for the
// revision-2 representations: BFLOAT16 raw/typed, STRING framing, typed
// COMPLEX).
BOOST_AUTO_TEST_CASE(V2PerTensorVectorsMatchHandEncodedPayloads)
{
  const auto root = loadFixture("initializer-stable-v2-vectors.json");
  int acceptedCount = 0;
  for (const auto& row : root.get_child("cases")) {
    const bool accepted = row.second.get<bool>("accepted");
    const auto name = row.second.get<std::string>("name");
    if (!accepted) {
      expectPrefix([&] {
        normalizedOnnxInitializerPayload(serialize(makeTensorFromSpec(row.second.get_child("tensor"))));
      }, "DI_ONNX_INITIALIZER_ENCODING_INVALID", name);
      continue;
    }
    ++acceptedCount;
    const auto payload = normalizedOnnxInitializerPayload(
      serialize(makeTensorFromSpec(row.second.get_child("tensor"))));
    const auto& expected = row.second.get_child("expected");
    BOOST_CHECK_MESSAGE(payload.dtype == expected.get<std::string>("dtype"), name + " dtype");
    BOOST_CHECK_MESSAGE(payload.byteOrder == expected.get<std::string>("byteOrder"),
                        name + " byteOrder");
    BOOST_CHECK_MESSAGE(payload.shape.size() == expected.get_child("shape").size(),
                        name + " rank");
    const auto& shape = expected.get_child("shape");
    std::size_t dimIndex = 0;
    for (const auto& dim : shape) {
      BOOST_CHECK_MESSAGE(payload.shape.at(dimIndex) == std::stoll(dim.second.data()),
                          name + " dim " + std::to_string(dimIndex));
      ++dimIndex;
    }
    BOOST_CHECK_MESSAGE(payload.content.size() ==
                          static_cast<std::size_t>(std::stoull(expected.get<std::string>("byteLength"))),
                        name + " byteLength");
    BOOST_CHECK_MESSAGE(toHex(payload.content) == expected.get<std::string>("payloadHex"),
                        name + " payload bytes");
    BOOST_CHECK_MESSAGE(sha256HexOf(payload.content) == expected.get<std::string>("contentDigest"),
                        name + " contentDigest");
  }
  BOOST_CHECK_EQUAL(acceptedCount, 12);
}

// v2 per-tensor cross-consistency: the bfloat16-raw and bfloat16-typed rows
// carry the same hand-encoded bit patterns and must land on one payload.
BOOST_AUTO_TEST_CASE(V2Bfloat16RawAndTypedShareOnePayload)
{
  const auto root = loadFixture("initializer-stable-v2-vectors.json");
  const auto& cases = root.get_child("cases");
  const auto& raw = fixtureRow(cases, "bfloat16-raw");
  const auto& typed = fixtureRow(cases, "bfloat16-typed");
  BOOST_REQUIRE(raw.get<bool>("accepted") && typed.get<bool>("accepted"));
  BOOST_CHECK_EQUAL(raw.get_child("expected").get<std::string>("payloadHex"),
                    typed.get_child("expected").get<std::string>("payloadHex"));
  BOOST_CHECK_EQUAL(raw.get_child("expected").get<std::string>("contentDigest"),
                    typed.get_child("expected").get<std::string>("contentDigest"));
}

// Revision classification on inlined sources: STRING, BFLOAT16 raw (incl.
// external inlined to raw) and typed COMPLEX select revision 2.
BOOST_AUTO_TEST_CASE(RevisionClassificationFollowsNormalizationContract)
{
  NativeAssemblyControl control = identityControl();
  auto makeSource = [&] (onnx::TensorProto initializer) {
    onnx::ModelProto model = singleInitializerModel(std::move(initializer));
    return NativeCanonicalSource{serialize(model), std::nullopt};
  };
  {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::BFLOAT16);
    tensor.add_dims(2);
    tensor.set_raw_data(std::string("\x80\x3f\x00\x40", 4));
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(makeSource(tensor), control), 2);
  }
  {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::BFLOAT16);
    tensor.add_dims(2);
    tensor.add_int32_data(16256);
    tensor.add_int32_data(16384);
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(makeSource(tensor), control), 1);
  }
  {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::STRING);
    tensor.add_dims(1);
    tensor.add_string_data("a");
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(makeSource(tensor), control), 2);
  }
  {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::COMPLEX64);
    tensor.add_dims(2);
    tensor.add_float_data(1.25f);
    tensor.add_float_data(-2.5f);
    tensor.add_float_data(-3.0f);
    tensor.add_float_data(4.0f);
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(makeSource(tensor), control), 2);
  }
  {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(2);
    tensor.set_raw_data(std::string("\x00\x00\x80\x3f\x00\x00\x00\x40", 8));
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(makeSource(tensor), control), 1);
  }
  // External BFLOAT16 inlines to raw -> revision 2; external COMPLEX inlines
  // to a legal raw representation -> revision 1.
  {
    onnx::ModelProto model = singleInitializerModel([] {
      onnx::TensorProto tensor;
      tensor.set_data_type(onnx::TensorProto::BFLOAT16);
      tensor.add_dims(2);
      tensor.set_data_location(onnx::TensorProto::EXTERNAL);
      auto* location = tensor.add_external_data();
      location->set_key("location");
      location->set_value("weights.bin");
      auto* offset = tensor.add_external_data();
      offset->set_key("offset");
      offset->set_value("0");
      auto* length = tensor.add_external_data();
      length->set_key("length");
      length->set_value("4");
      return tensor;
    }());
    NativeCanonicalSource source{serialize(model),
                                 std::vector<std::uint8_t>{0x80, 0x3f, 0x00, 0x40}};
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(source, control), 2);
  }
  {
    onnx::ModelProto model = singleInitializerModel([] {
      onnx::TensorProto tensor;
      tensor.set_data_type(onnx::TensorProto::COMPLEX64);
      tensor.add_dims(2);
      tensor.set_data_location(onnx::TensorProto::EXTERNAL);
      auto* location = tensor.add_external_data();
      location->set_key("location");
      location->set_value("weights.bin");
      auto* offset = tensor.add_external_data();
      offset->set_key("offset");
      offset->set_value("0");
      auto* length = tensor.add_external_data();
      length->set_key("length");
      length->set_value("16");
      return tensor;
    }());
    NativeCanonicalSource source{serialize(model),
                                 fromHex("0000a03f000020c0000040c000008040")};
    BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(source, control), 1);
  }
}

// A revision-2 source must be bound to the exact v2 assembler descriptor;
// revision-1 sources keep the legacy descriptor rules.
BOOST_AUTO_TEST_CASE(AssemblerDescriptorBindingGate)
{
  const auto v2Descriptor = "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89";
  NativeAssemblyControl control = identityControl();
  auto makeSource = [&] (onnx::TensorProto initializer) {
    onnx::ModelProto model = singleInitializerModel(std::move(initializer));
    return NativeCanonicalSource{serialize(model), std::nullopt};
  };
  onnx::TensorProto stringTensor;
  stringTensor.set_data_type(onnx::TensorProto::STRING);
  stringTensor.add_dims(1);
  stringTensor.add_string_data("a");
  const auto stringSource = makeSource(stringTensor);
  onnx::TensorProto floatTensor;
  floatTensor.set_data_type(onnx::TensorProto::FLOAT);
  floatTensor.add_dims(2);
  floatTensor.set_raw_data(std::string("\x00\x00\x80\x3f\x00\x00\x00\x40", 8));
  const auto floatSource = makeSource(floatTensor);

  // Revision 2 without the exact descriptor -> NORMALIZATION_REVISION_REQUIRED.
  expectPrefix([&] { checkOnnxAssemblerDescriptorBinding("", stringSource, control); },
               "DI_ONNX_NORMALIZATION_REVISION_REQUIRED", "rev2 empty descriptor");
  expectPrefix(
    [&] { checkOnnxAssemblerDescriptorBinding(
            "sha256:f0210ce34f62d5fdabcd8129a0dfbafcf8fca8d99852443518b5fcda7434841b",
            stringSource, control); },
    "DI_ONNX_NORMALIZATION_REVISION_REQUIRED", "rev2 legacy descriptor");
  // Revision 2 with the exact v2 descriptor passes.
  checkOnnxAssemblerDescriptorBinding(v2Descriptor, stringSource, control);
  // Revision 1 keeps legacy rules: empty or legacy descriptors are legal.
  checkOnnxAssemblerDescriptorBinding("", floatSource, control);
  checkOnnxAssemblerDescriptorBinding(
    "sha256:f0210ce34f62d5fdabcd8129a0dfbafcf8fca8d99852443518b5fcda7434841b",
    floatSource, control);
  checkOnnxAssemblerDescriptorBinding(v2Descriptor, floatSource, control);
}

// Typed COMPLEX is a revision-2 representation on the identity seam: the v2
// hand vector proves payload parity with raw COMPLEX, and the descriptor gate
// rejects a typed-complex model bound to a legacy recipe.
BOOST_AUTO_TEST_CASE(TypedComplexIsV2BoundNotLegacyConvertible)
{
  onnx::TensorProto tensor;
  tensor.set_data_type(onnx::TensorProto::COMPLEX64);
  tensor.add_dims(2);
  tensor.add_float_data(1.25f);
  tensor.add_float_data(-2.5f);
  tensor.add_float_data(-3.0f);
  tensor.add_float_data(4.0f);
  const auto source = NativeCanonicalSource{serialize(singleInitializerModel(tensor)), std::nullopt};
  const auto control = identityControl();
  expectPrefix([&] {
    checkOnnxAssemblerDescriptorBinding("", source, control);
  }, "DI_ONNX_NORMALIZATION_REVISION_REQUIRED", "typed complex legacy binding");
  checkOnnxAssemblerDescriptorBinding(
    "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89",
    source, control);
  // revision is 2 on the identity seam (full-checker success is not the
  // legacy numpy conversion's success).
  BOOST_CHECK_EQUAL(onnxInitializerNormalizationRevision(source, control), 2);
}

// External tensors: memory-only inlining with S2 rules (identical relative
// location, bounded offsets, length 0 == rest-from-offset), identity
// equivalence between external and inlined models, and the full boundary
// family.
BOOST_AUTO_TEST_CASE(ExternalInliningRulesAndInlineEquivalence)
{
  NativeAssemblyControl control = identityControl();
  onnx::TensorProto external = [] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(4);
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    auto* location = tensor.add_external_data();
    location->set_key("location");
    location->set_value("weights.bin");
    auto* offset = tensor.add_external_data();
    offset->set_key("offset");
    offset->set_value("2");
    return tensor;
  }();  // no length entry -> rest from offset
  onnx::ModelProto model = singleInitializerModel(external);
  // offset 2 + 16 content bytes: any shorter/longer binding must not match
  // the inlined raw payload below.
  const std::vector<std::uint8_t> weights{0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
                                          0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
                                          0x80, 0x3f, 0x00, 0x00, 0x00, 0x40};
  NativeCanonicalSource externalSource{serialize(model), weights};
  onnx::TensorProto inlined = [] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(4);
    tensor.set_raw_data(std::string("\x00\x00\x80\x3f\x00\x00\x00\x40"
                                    "\x00\x00\x80\x3f\x00\x00\x00\x40",
                                    16));
    return tensor;
  }();
  NativeCanonicalSource inlinedSource{serialize(singleInitializerModel(inlined)), std::nullopt};
  const auto externalIdentity = canonicalOnnxSourceIdentity(externalSource, control);
  const auto inlinedIdentity = canonicalOnnxSourceIdentity(inlinedSource, control);
  BOOST_CHECK_EQUAL(externalIdentity.graphDigest, inlinedIdentity.graphDigest);
  BOOST_CHECK_EQUAL(externalIdentity.initializerDigest, inlinedIdentity.initializerDigest);

  // Explicit length "0" is the 1.17 loader rule for rest-from-offset as well.
  onnx::ModelProto withZeroLength = singleInitializerModel([] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(4);
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    auto* location = tensor.add_external_data();
    location->set_key("location");
    location->set_value("weights.bin");
    auto* offset = tensor.add_external_data();
    offset->set_key("offset");
    offset->set_value("2");
    auto* length = tensor.add_external_data();
    length->set_key("length");
    length->set_value("0");
    return tensor;
  }());
  const auto zeroLengthIdentity =
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(withZeroLength), weights}, control);
  BOOST_CHECK_EQUAL(zeroLengthIdentity.initializerDigest, inlinedIdentity.initializerDigest);

  // Absolute and parent-relative locations are rejected.
  onnx::TensorProto absolute = external;
  absolute.mutable_external_data(0)->set_value("/etc/weights.bin");
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(
      NativeCanonicalSource{serialize(singleInitializerModel(absolute)), weights}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_LOCATION", "absolute location");
  onnx::TensorProto parentTraversal = external;
  parentTraversal.mutable_external_data(0)->set_value("sub/../weights.bin");
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(
      NativeCanonicalSource{serialize(singleInitializerModel(parentTraversal)), weights}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_LOCATION", "parent traversal location");
  // Offset beyond the provided bytes -> range error.
  onnx::TensorProto oversized = external;
  oversized.mutable_external_data(1)->set_value("100");
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(
      NativeCanonicalSource{serialize(singleInitializerModel(oversized)), weights}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_RANGE", "offset beyond bytes");
  // External declared without a binding side file -> binding error.
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(model), std::nullopt}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_BINDING", "missing binding");
  // Binding declared for a fully inlined model -> binding error.
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(singleInitializerModel(inlined)), weights},
                                control);
  }, "DI_NATIVE_ONNX_EXTERNAL_BINDING", "unneeded binding");
  // Two external tensors at different locations -> location error.
  onnx::ModelProto twoLocations = singleInitializerModel(external);
  {
    auto* second = twoLocations.mutable_graph()->add_initializer();
    *second = external;
    second->set_name("weight2");
    second->mutable_external_data(0)->set_value("other.bin");
  }
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(twoLocations), weights}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_LOCATION", "differing locations");
}

// Function-attribute external tensors are scanned by the identity path
// (S2 _get_all_tensors coverage the pre-worker assembler does not have;
// that unification lands with the T006-B/C extractor work).
BOOST_AUTO_TEST_CASE(FunctionAttributeExternalsAreValidatedAndInlined)
{
  NativeAssemblyControl control = identityControl();
  // Build: model with a local function whose node carries the external tensor
  // as an attribute constant, plus x/y I/O so the model stays structural.
  onnx::ModelProto model;
  model.set_ir_version(8);
  auto* opset = model.add_opset_import();
  opset->set_domain("");
  opset->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("spec182-function-external");
  auto* input = graph->add_input();
  input->set_name("x");
  input->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* output = graph->add_output();
  output->set_name("y");
  output->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
  auto* function = model.add_functions();
  function->set_name("fn_with_constant");
  function->set_domain("spec182");
  auto* fnode = function->add_node();
  fnode->set_op_type("Constant");
  fnode->add_output("c");
  auto* attribute = fnode->add_attribute();
  attribute->set_name("value");
  attribute->set_type(onnx::AttributeProto::TENSOR);
  attribute->mutable_t()->set_data_type(onnx::TensorProto::FLOAT);
  attribute->mutable_t()->add_dims(1);
  attribute->mutable_t()->set_data_location(onnx::TensorProto::EXTERNAL);
  auto* location = attribute->mutable_t()->add_external_data();
  location->set_key("location");
  location->set_value("weights.bin");
  auto* offset = attribute->mutable_t()->add_external_data();
  offset->set_key("offset");
  offset->set_value("0");
  auto* length = attribute->mutable_t()->add_external_data();
  length->set_key("length");
  length->set_value("4");

  const std::vector<std::uint8_t> weights{0x00, 0x00, 0x80, 0x3f};
  // The deep scan must see the function-attribute tensor: without a binding
  // the source is rejected even though the main graph has no externals.
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(model), std::nullopt}, control);
  }, "DI_NATIVE_ONNX_EXTERNAL_BINDING", "function attribute without binding");
  // With the binding the model inlines and its identity equals the same
  // model carrying the inlined bytes directly.
  const auto externalIdentity =
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(model), weights}, control);
  attribute->mutable_t()->clear_external_data();
  attribute->mutable_t()->set_data_location(onnx::TensorProto::DEFAULT);
  attribute->mutable_t()->set_raw_data(std::string("\x00\x00\x80\x3f", 4));
  const auto inlinedIdentity =
    canonicalOnnxSourceIdentity(NativeCanonicalSource{serialize(model), std::nullopt}, control);
  BOOST_CHECK_EQUAL(externalIdentity.graphDigest, inlinedIdentity.graphDigest);
  BOOST_CHECK_EQUAL(externalIdentity.initializerDigest, inlinedIdentity.initializerDigest);
}

// Source-boundary and shape failures of the identity seam.
BOOST_AUTO_TEST_CASE(IdentityRejectsBoundaryViolations)
{
  // Source limit: zero-limit control, and bytes above the declared maximum.
  NativeCanonicalSource empty{std::vector<std::uint8_t>{}, std::nullopt};
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(empty, NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(5), [] {}, 0, 0});
  }, "DI_NATIVE_ONNX_SOURCE_LIMIT", "zero source limit");
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(NativeCanonicalSource{std::vector<std::uint8_t>{1, 2, 3, 4},
                                                      std::nullopt},
                                identityControl(2));
  }, "DI_NATIVE_ONNX_SOURCE_LIMIT", "bytes over limit");
  // Initializer side file above the limit.
  onnx::TensorProto external = [] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(1);
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    auto* location = tensor.add_external_data();
    location->set_key("location");
    location->set_value("weights.bin");
    auto* offset = tensor.add_external_data();
    offset->set_key("offset");
    offset->set_value("0");
    auto* length = tensor.add_external_data();
    length->set_key("length");
    length->set_value("4");
    return tensor;
  }();
  // Side file above the limit: the model passes the source gate at exactly
  // its own size, so the initializer gate (not the source gate) must fire.
  const std::vector<std::uint8_t> modelBytes = serialize(singleInitializerModel(external));
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(
      NativeCanonicalSource{modelBytes, std::vector<std::uint8_t>(1024 * 1024, 0x01)},
      identityControl(modelBytes.size()));
  }, "DI_NATIVE_ONNX_INITIALIZER_LIMIT", "initializer bytes over limit");
  // Parse failure.
  expectPrefix([&] {
    canonicalOnnxSourceIdentity(
      NativeCanonicalSource{std::vector<std::uint8_t>{0xde, 0xad, 0xbe, 0xef}, std::nullopt},
      identityControl());
  }, "DI_NATIVE_ONNX_PARSE", "garbage model bytes");
  // Dimension count overflow and negative dims are normalization-invalid.
  expectPrefix([&] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(4611686018427387904LL);  // 2^62
    tensor.add_dims(4611686018427387904LL);
    normalizedOnnxInitializerPayload(serialize(tensor));
  }, "DI_ONNX_INITIALIZER_ENCODING_INVALID", "dim product overflow");
  expectPrefix([&] {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(-1);
    normalizedOnnxInitializerPayload(serialize(tensor));
  }, "DI_ONNX_INITIALIZER_ENCODING_INVALID", "negative dim");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
