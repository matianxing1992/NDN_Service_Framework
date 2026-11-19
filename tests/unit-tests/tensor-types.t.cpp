#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <onnx/onnx_pb.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(Spec233TensorTypes)

namespace {

struct IdentityModelFixture
{
  IdentityModelFixture()
    : directory(std::filesystem::temp_directory_path() /
                ("ndnsf-spec233-types-" + std::to_string(::getpid())))
  {
    if (!std::filesystem::create_directory(directory)) {
      throw std::runtime_error("identity fixture directory already exists");
    }
  }

  ~IdentityModelFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  std::filesystem::path
  write(std::int32_t elementType, std::int64_t count) const
  {
    onnx::ModelProto model;
    model.set_ir_version(8);
    auto* opset = model.add_opset_import();
    opset->set_version(13);
    auto* graph = model.mutable_graph();
    graph->set_name("typed-identity");
    auto* operation = graph->add_node();
    operation->set_op_type("Identity");
    operation->add_input("x");
    operation->add_output("y");
    for (const auto& port : std::array<std::string, 2>{"x", "y"}) {
      auto* value = port == "x" ? graph->add_input() : graph->add_output();
      value->set_name(port);
      auto* type = value->mutable_type()->mutable_tensor_type();
      type->set_elem_type(elementType);
      type->mutable_shape()->add_dim()->set_dim_value(count);
    }
    const auto path = directory / (std::to_string(elementType) + ".onnx");
    std::ofstream output(path, std::ios::binary);
    if (!model.SerializeToOstream(&output)) {
      throw std::runtime_error("identity fixture serialization failed");
    }
    return path;
  }

  const std::filesystem::path directory;
};

} // namespace

BOOST_AUTO_TEST_CASE(PreservesLegacyUnsignedScalarWire)
{
  const NamedTensor tensor{"x", TensorElementType::UInt8, {}, {0xa5}};
  const std::vector<std::uint8_t> expected{
    'N', 'D', 'I', 'T', 'B', '0', '0', '1',
    1, 0, 0, 0,
    1, 0, 0, 0, 'x',
    5, 0, 0, 0,
    0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0,
    0xa5};
  const auto encoded = encodeTensorBundle({tensor});
  BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), expected.begin(), expected.end());
  const auto decoded = decodeTensorBundle(expected);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1);
  BOOST_CHECK(decoded.front().shape.empty());
  BOOST_CHECK(decoded.front().elementType == TensorElementType::UInt8);
  BOOST_REQUIRE_EQUAL(decoded.front().payload.size(), 1);
  BOOST_CHECK_EQUAL(decoded.front().payload.front(), 0xa5);
}

BOOST_AUTO_TEST_CASE(PreservesAllSixLegacyScalarFrames)
{
  struct Case
  {
    TensorElementType type;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> suffix;
  };
  // Literal frames from the original six-type contract, independent of codec helpers.
  const std::vector<Case> cases{
    {TensorElementType::Float32, {0, 0, 0, 0xc0},
      {1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0}},
    {TensorElementType::Float16, {0, 0xc0},
      {2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0}},
    {TensorElementType::Int64, {255, 255, 255, 255, 255, 255, 255, 255},
      {3, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0,
       255, 255, 255, 255, 255, 255, 255, 255}},
    {TensorElementType::Bool, {1},
      {4, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}},
    {TensorElementType::UInt8, {255},
      {5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 255}},
    {TensorElementType::Int32, {255, 255, 255, 255},
      {6, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255}}};
  for (const auto& item : cases) {
    std::vector<std::uint8_t> expected{
      'N', 'D', 'I', 'T', 'B', '0', '0', '1', 1, 0, 0, 0, 1, 0, 0, 0, 'x'};
    expected.insert(expected.end(), item.suffix.begin(), item.suffix.end());
    const auto actual = encodeTensorBundle({NamedTensor{"x", item.type, {}, item.payload}});
    BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
    const auto decoded = decodeTensorBundle(expected);
    BOOST_REQUIRE_EQUAL(decoded.size(), 1);
    BOOST_CHECK(decoded.front().elementType == item.type);
    BOOST_CHECK(decoded.front().shape.empty());
    BOOST_CHECK(decoded.front().payload == item.payload);
  }
}

BOOST_AUTO_TEST_CASE(PreservesSignedAndBfloatBitsInOwnedAndBorrowedBundles)
{
  const std::vector<NamedTensor> source{
    {"signed", TensorElementType::INT8, {5}, {0x80, 0xff, 0, 1, 0x7f}},
    {"bf16", TensorElementType::BFLOAT16, {2}, {0x80, 0x3f, 0x00, 0xc0}},
    {"past", TensorElementType::BFLOAT16, {1, 8, 0, 128}, {}}};
  const auto wire = encodeTensorBundle(source);
  const auto decoded = decodeTensorBundle(wire);
  const auto views = decodeTensorBundleViews(wire);
  BOOST_REQUIRE_EQUAL(decoded.size(), source.size());
  BOOST_REQUIRE_EQUAL(views.size(), source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    BOOST_CHECK(decoded[i].elementType == source[i].elementType);
    BOOST_CHECK(views[i].elementType == source[i].elementType);
    BOOST_CHECK(decoded[i].shape == source[i].shape);
    BOOST_CHECK(views[i].shape == source[i].shape);
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded[i].payload.begin(), decoded[i].payload.end(),
                                  source[i].payload.begin(), source[i].payload.end());
    BOOST_CHECK_EQUAL_COLLECTIONS(views[i].payload.begin(), views[i].payload.end(),
                                  source[i].payload.begin(), source[i].payload.end());
  }
  BOOST_CHECK(encodeTensorBundleViews(views) == wire);
}

BOOST_AUTO_TEST_CASE(ResolvesAllCanonicalTypesWithoutReassigningWireIds)
{
  const std::array<std::string, 8> names{
    "float32", "float16", "int64", "bool", "uint8", "int32", "int8", "bfloat16"};
  const std::array<std::size_t, 8> widths{4, 2, 8, 1, 1, 4, 1, 2};
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto type = tensorElementTypeFromName(names[i]);
    BOOST_CHECK_EQUAL(static_cast<std::uint32_t>(type), i + 1);
    BOOST_CHECK_EQUAL(tensorElementTypeName(type), names[i]);
    BOOST_CHECK_EQUAL(tensorElementByteSize(type), widths[i]);
  }
  BOOST_CHECK_THROW(tensorElementTypeFromName("int4"), std::invalid_argument);
  BOOST_CHECK_THROW(tensorElementTypeFromName("float"), std::invalid_argument);
  BOOST_CHECK_THROW(tensorElementByteSize(static_cast<TensorElementType>(255)),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidPayloadsOnEveryCodecAdmissionPath)
{
  const NamedTensor invalidBool{"b", TensorElementType::Bool, {1}, {2}};
  BOOST_CHECK_THROW(encodeTensorBundle({invalidBool}), std::invalid_argument);
  const NamedTensorView invalidView{
    invalidBool.name, invalidBool.elementType, invalidBool.shape,
    ndn::span<const std::uint8_t>(invalidBool.payload.data(), invalidBool.payload.size())};
  BOOST_CHECK_THROW(encodeTensorBundleViews({invalidView}), std::invalid_argument);

  auto wire = encodeTensorBundle({NamedTensor{"b", TensorElementType::Bool, {1}, {1}}});
  wire.back() = 2;
  BOOST_CHECK_THROW(decodeTensorBundle(wire), std::invalid_argument);
  BOOST_CHECK_THROW(decodeTensorBundleViews(wire), std::invalid_argument);

  const auto validWire = encodeTensorBundle({NamedTensor{"b", TensorElementType::Bool, {1}, {1}}});
  for (std::size_t length = 0; length < validWire.size(); ++length) {
    const std::vector<std::uint8_t> truncated(validWire.begin(), validWire.begin() + length);
    BOOST_CHECK_THROW(decodeTensorBundle(truncated), std::invalid_argument);
    BOOST_CHECK_THROW(decodeTensorBundleViews(truncated), std::invalid_argument);
  }
  auto unknownType = validWire;
  unknownType[17] = 255; // Literal offset of the type field in the one-character-name frame.
  BOOST_CHECK_THROW(decodeTensorBundle(unknownType), std::invalid_argument);
  BOOST_CHECK_THROW(decodeTensorBundleViews(unknownType), std::invalid_argument);

  // Bounded deterministic parser mutations complement the independent golden frames.
  for (std::size_t position = 0; position < validWire.size(); ++position) {
    for (const auto value : std::array<std::uint8_t, 5>{0, 1, 127, 128, 255}) {
      auto mutated = validWire;
      mutated[position] = value;
      std::optional<std::vector<NamedTensor>> owned;
      try {
        owned = decodeTensorBundle(mutated);
      }
      catch (const std::invalid_argument&) {
        // Rejection is expected for malformed headers, metadata and payloads.
      }
      if (owned) {
        BOOST_CHECK(encodeTensorBundle(*owned) == mutated);
      }
      std::optional<std::vector<NamedTensorView>> views;
      try {
        views = decodeTensorBundleViews(mutated);
      }
      catch (const std::invalid_argument&) {
      }
      if (views) {
        BOOST_CHECK(encodeTensorBundleViews(*views) == mutated);
      }
    }
  }

  BOOST_CHECK_NO_THROW(encodeTensorBundle({NamedTensor{"empty", TensorElementType::INT8,
                                                      {2147483648LL, 0}, {}}}));
  BOOST_CHECK_THROW(encodeTensorBundle({NamedTensor{"bad", TensorElementType::INT8,
                                                    {2147483649LL, 0}, {}}}),
                    std::invalid_argument);

  BOOST_CHECK_THROW(encodeTensorBundle({NamedTensor{"bad", TensorElementType::BFLOAT16,
                                                    {2}, {0x80, 0x3f}}}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(encodeTensorBundle({NamedTensor{"bad", TensorElementType::INT8,
                                                    {-1}, {}}}), std::invalid_argument);
  BOOST_CHECK_THROW(encodeTensorBundle({NamedTensor{"bad", TensorElementType::BFLOAT16,
                                                    {2147483648LL, 2147483648LL, 8}, {}}}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxRunnerPreservesActualPortTypesAndBits)
{
  IdentityModelFixture models;
  struct Case
  {
    TensorElementType type;
    std::int32_t onnxType;
    std::int64_t count;
    std::vector<std::uint8_t> bytes;
  };
  // ONNX scalar IDs are independent of NDITB wire IDs.
  const std::vector<Case> cases{
    {TensorElementType::UInt8, 2, 3, {0, 128, 255}},
    {TensorElementType::INT8, 3, 3, {128, 255, 127}},
    {TensorElementType::Int32, 6, 1, {0xff, 0xff, 0xff, 0xff}},
    {TensorElementType::Float16, 10, 2, {0, 0x3c, 0, 0xc0}},
    {TensorElementType::BFLOAT16, 16, 2, {0x80, 0x3f, 0, 0xc0}}};
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  for (const auto& item : cases) {
    const auto path = models.write(item.onnxType, item.count);
    NativeModelRunnerSpec spec;
    spec.role = "/TypedIdentity";
    spec.kind = "onnx-model";
    spec.backend = "onnxruntime";
    spec.path = path.string();
    spec.metadata = {{"inputNames", "x"}, {"outputNames", "y"},
                     {"outputBundleScope", "typed-output"}, {"forceOutputBundle", "true"}};
    auto runner = factory.create(spec);
    RoleExecutionContext context;
    context.sessionId = "typed-identity";
    context.role = spec.role;
    context.inputsByScope.emplace("activation", makeEncodedTensorBundle(
      "activation", {{"x", item.type, {item.count}, item.bytes}}));
    const auto outputs = runner->run(context);
    BOOST_REQUIRE_EQUAL(outputs.count("typed-output"), 1);
    const auto tensors = decodeTensorBundle(outputs.at("typed-output").payload);
    const auto& actual = findTensor(tensors, "y");
    BOOST_CHECK(actual.elementType == item.type);
    BOOST_CHECK(actual.shape == std::vector<std::int64_t>{item.count});
    BOOST_CHECK_EQUAL_COLLECTIONS(actual.payload.begin(), actual.payload.end(),
                                  item.bytes.begin(), item.bytes.end());
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
