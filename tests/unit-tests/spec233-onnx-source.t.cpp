#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-generation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-tensor-type.hpp"

#include <onnx/onnx_pb.h>
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
#include <onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ndnsf::di::tests {
namespace {

class MemoryRanges final : public NativeCanonicalByteRangeSource
{
public:
  explicit MemoryRanges(std::vector<std::uint8_t> bytes)
    : m_bytes(std::move(bytes))
  {
  }

  std::uint64_t size() const noexcept override { return m_bytes.size(); }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_bytes.size() || length > m_bytes.size() - offset ||
        length > NativeCanonicalMaterialBundleMaxBytes) {
      throw std::out_of_range("fixture range outside source");
    }
    return {m_bytes.begin() + offset, m_bytes.begin() + offset + length};
  }

private:
  const std::vector<std::uint8_t> m_bytes;
};

NativeAssemblyControl
control()
{
  return {std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
          1U << 20, 1U << 20};
}

NativeCanonicalSource
twoFileSource()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("two-file-source");
  for (const auto& name : {std::string("a"), std::string("b")}) {
    auto* value = graph->add_initializer();
    value->set_name(name);
    value->set_data_type(onnx::TensorProto::FLOAT);
    value->add_dims(2);
    value->set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
           {"location", "weights/" + name + ".bin"}, {"offset", "0"}, {"length", "8"}}) {
      auto* entry = value->add_external_data();
      entry->set_key(pair.first);
      entry->set_value(pair.second);
    }
  }
  auto* node = graph->add_node();
  node->set_op_type("Add");
  node->add_input("a");
  node->add_input("b");
  node->add_output("y");
  auto* output = graph->add_output();
  output->set_name("y");
  auto* type = output->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  type->mutable_shape()->add_dim()->set_dim_value(2);
  std::string bytes;
  if (!model.SerializeToString(&bytes)) throw std::runtime_error("fixture serialization");
  NativeCanonicalSource source;
  source.modelBytes.assign(bytes.begin(), bytes.end());
  source.initializerRangeSource = std::make_shared<MemoryRanges>(std::vector<std::uint8_t>{
    0, 0, 128, 63, 0, 0, 0, 64, 0, 0, 64, 64, 0, 0, 128, 64});
  source.externalFileRanges = {{"weights/a.bin", {0, 8}}, {"weights/b.bin", {8, 8}}};
  return source;
}

struct FilesFixture
{
  FilesFixture()
    : root(std::filesystem::temp_directory_path() /
           ("ndnsf-spec233-source-" + std::to_string(::getpid())))
  {
    if (!std::filesystem::create_directory(root)) {
      throw std::runtime_error("source fixture directory already exists");
    }
    try {
      std::filesystem::create_directory(root / "weights");
      const auto source = twoFileSource();
      write(root / "model.onnx", source.modelBytes);
      write(root / "weights/a.bin", {0, 0, 128, 63, 0, 0, 0, 64});
      write(root / "weights/b.bin", {0, 0, 64, 64, 0, 0, 128, 64});
    }
    catch (...) {
      std::filesystem::remove_all(root);
      throw;
    }
  }

  ~FilesFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  static void
  write(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes)
  {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    if (!output) throw std::runtime_error("source fixture write failed");
  }

  const std::filesystem::path root;
};

onnx::ModelProto
nBitsModel()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  auto* custom = model.add_opset_import();
  custom->set_domain("com.microsoft");
  custom->set_version(1);
  auto* graph = model.mutable_graph();
  graph->set_name("tail-block");
  auto* weight = graph->add_initializer();
  weight->set_name("packed");
  weight->set_data_type(onnx::TensorProto::UINT8);
  for (const auto dim : {3, 2, 16}) weight->add_dims(dim);
  weight->set_raw_data(std::string(96, static_cast<char>(0x88)));
  auto* scales = graph->add_initializer();
  scales->set_name("scales");
  scales->set_data_type(onnx::TensorProto::FLOAT);
  scales->add_dims(6); // A valid flattened contiguous per-block scale representation.
  for (int i = 0; i < 6; ++i) scales->add_float_data(1.0f);
  auto* node = graph->add_node();
  node->set_op_type("MatMulNBits");
  node->set_domain("com.microsoft");
  for (const auto* name : {"x", "packed", "scales"}) node->add_input(name);
  node->add_output("y");
  for (const auto& item : std::vector<std::pair<std::string, int>>{
         {"K", 33}, {"N", 3}, {"bits", 4}, {"block_size", 32}}) {
    auto* attr = node->add_attribute();
    attr->set_name(item.first);
    attr->set_type(onnx::AttributeProto::INT);
    attr->set_i(item.second);
  }
  for (const auto& name : {std::string("x"), std::string("y")}) {
    auto* value = name == "x" ? graph->add_input() : graph->add_output();
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dim : {1, 2, name == "x" ? 33 : 3})
      type->mutable_shape()->add_dim()->set_dim_value(dim);
  }
  return model;
}

void
writeModel(const std::filesystem::path& path, const onnx::ModelProto& model)
{
  std::string bytes;
  if (!model.SerializeToString(&bytes)) throw std::runtime_error("fixture model serialization");
  FilesFixture::write(path, {bytes.begin(), bytes.end()});
}

onnx::ModelProto
integerLiteralModel()
{
  const auto source = twoFileSource();
  onnx::ModelProto model;
  if (!model.ParseFromArray(source.modelBytes.data(), source.modelBytes.size()))
    throw std::runtime_error("fixture parse failed");
  return model;
}

onnx::ModelProto
quantizeModel()
{
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  auto* graph = model.mutable_graph();
  graph->set_name("per-channel");
  auto* scales = graph->add_initializer();
  scales->set_name("s");
  scales->set_data_type(onnx::TensorProto::FLOAT);
  scales->add_dims(3);
  for (int i = 0; i < 3; ++i) scales->add_float_data(0.25f);
  auto* zero = graph->add_initializer();
  zero->set_name("z");
  zero->set_data_type(onnx::TensorProto::INT8);
  zero->add_dims(3);
  zero->set_raw_data(std::string(3, static_cast<char>(0xff)));
  auto* node = graph->add_node();
  node->set_op_type("QuantizeLinear");
  for (const auto* name : {"x", "s", "z"}) node->add_input(name);
  node->add_output("y");
  for (const auto& name : {std::string("x"), std::string("y")}) {
    auto* value = name == "x" ? graph->add_input() : graph->add_output();
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(name == "x" ? onnx::TensorProto::FLOAT : onnx::TensorProto::INT8);
    type->mutable_shape()->add_dim()->set_dim_value(2);
    type->mutable_shape()->add_dim()->set_dim_value(3);
  }
  return model;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233OnnxSource)

BOOST_AUTO_TEST_CASE(PreservesDistinctFilesAtTheSameDeclaredOffset)
{
  const auto source = twoFileSource();
  const auto original = source.modelBytes;
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control());
  BOOST_CHECK(source.modelBytes == original);
  BOOST_REQUIRE_EQUAL(manifest->references.size(), 4);
  for (const auto& name : {std::string("a"), std::string("b")}) {
    const auto found = std::find_if(manifest->references.begin(), manifest->references.end(),
      [&name](const auto& item) { return item.logicalName == name; });
    BOOST_REQUIRE(found != manifest->references.end());
    const auto expectedDigest = name == "a"
      ? "sha256:b9c80b5adeca450753a16950c3cc655d271f7bef7a485bc83f112b72fef21d37"
      : "sha256:986c627ee6ef1bcc3d746256a7045624ceeb44c4ed557ca055b2d43933de3489";
    BOOST_CHECK_EQUAL(found->sharedDigest, expectedDigest);
    BOOST_REQUIRE_EQUAL(found->chunkPayloadIds.size(), 1);
    const auto chunk = std::find_if(manifest->payloads.begin(), manifest->payloads.end(),
      [&found](const auto& item) { return item.payloadId == found->chunkPayloadIds.front(); });
    BOOST_REQUIRE(chunk != manifest->payloads.end());
    const std::vector<std::uint8_t> expected = name == "a"
      ? std::vector<std::uint8_t>{0, 0, 128, 63, 0, 0, 0, 64}
      : std::vector<std::uint8_t>{0, 0, 64, 64, 0, 0, 128, 64};
    BOOST_CHECK(chunk->copyBytes() == expected);
  }
}

BOOST_AUTO_TEST_CASE(RejectsAnIndividualFileOverflowDespiteSufficientAggregateBytes)
{
  auto source = twoFileSource();
  source.externalFileRanges = {{"weights/a.bin", {0, 4}}, {"weights/b.bin", {4, 12}}};
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, control()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidLayoutsAndMissingOwners)
{
  auto source = twoFileSource();
  source.externalFileRanges.at("weights/b.bin").offset = 0;
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, control()), std::runtime_error);
  source = twoFileSource();
  source.initializerRangeSource.reset();
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, control()), std::runtime_error);
  source = twoFileSource();
  source.externalFileRanges.clear();
  // Legacy callers must still provide one unambiguous location.
  BOOST_CHECK_THROW(deriveNativeCanonicalMaterialManifest(source, control()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(OpensPhysicalFilesWithoutRewritingOrCopyingExternalWeights)
{
  FilesFixture files;
  const auto opened = openAutomaticOnnxSource(files.root / "model.onnx", control());
  BOOST_CHECK(opened.source.modelBytes == twoFileSource().modelBytes);
  BOOST_CHECK(!opened.source.initializerBytes);
  BOOST_REQUIRE_EQUAL(opened.externalFiles.size(), 2);
  BOOST_CHECK_EQUAL(opened.externalFiles[0].location, "weights/a.bin");
  BOOST_CHECK_EQUAL(opened.externalFiles[0].digest,
    "sha256:b9c80b5adeca450753a16950c3cc655d271f7bef7a485bc83f112b72fef21d37");
  BOOST_CHECK_EQUAL(opened.externalFiles[1].digest,
    "sha256:986c627ee6ef1bcc3d746256a7045624ceeb44c4ed557ca055b2d43933de3489");
  const auto manifest = deriveNativeCanonicalMaterialManifest(opened.source, control());
  BOOST_CHECK_EQUAL(manifest->references.size(), 4);
  const auto again = openAutomaticOnnxSource(files.root / "model.onnx", control());
  BOOST_CHECK_EQUAL(opened.sourceDigest, again.sourceDigest);
  const auto relative = std::filesystem::path(".") /
    std::filesystem::relative(files.root / "model.onnx", std::filesystem::current_path());
  const auto fromRelative = openAutomaticOnnxSource(relative, control());
  BOOST_CHECK_EQUAL(opened.sourceDigest, fromRelative.sourceDigest);
  FilesFixture::write(files.root / "weights/a.bin", {0, 0, 128, 191, 0, 0, 0, 64});
  BOOST_CHECK_THROW(opened.source.initializerRangeSource->read(0, 8), std::runtime_error);
  const auto changed = openAutomaticOnnxSource(files.root / "model.onnx", control());
  BOOST_CHECK(changed.sourceDigest != opened.sourceDigest);
}

BOOST_AUTO_TEST_CASE(RejectsSymlinksMissingFilesBudgetsAndCancellation)
{
  FilesFixture files;
  std::filesystem::create_symlink(files.root / "model.onnx", files.root / "alias.onnx");
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "alias.onnx", control()),
                    std::runtime_error);
  std::filesystem::create_directory_symlink(files.root, files.root / "alias-directory");
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "alias-directory/model.onnx", control()),
                    std::runtime_error);
  std::filesystem::rename(files.root / "weights/a.bin", files.root / "original.bin");
  std::filesystem::create_symlink(files.root / "original.bin", files.root / "weights/a.bin");
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", control()),
                    std::runtime_error);
  std::filesystem::remove(files.root / "weights/a.bin");
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", control()),
                    std::runtime_error);
  BOOST_REQUIRE_EQUAL(::mkfifo((files.root / "weights/a.bin").c_str(), 0600), 0);
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", control()),
                    std::runtime_error);
  std::filesystem::remove(files.root / "weights/a.bin");
  std::filesystem::rename(files.root / "original.bin", files.root / "weights/a.bin");
  auto limited = control();
  limited.maxSourceBytes = std::filesystem::file_size(files.root / "model.onnx") + 15;
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", limited),
                    std::runtime_error);
  auto cancelled = control();
  cancelled.requireActive = [] { throw std::runtime_error("cancelled"); };
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", cancelled),
                    std::runtime_error);
  auto expired = control();
  expired.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", expired),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PathReplacementCannotSupplyDifferentBytesToAnOpenedSource)
{
  FilesFixture files;
  const auto opened = openAutomaticOnnxSource(files.root / "model.onnx", control());
  std::filesystem::rename(files.root / "weights/a.bin", files.root / "old-a.bin");
  FilesFixture::write(files.root / "weights/a.bin", {255, 255, 255, 255, 255, 255, 255, 255});
  try {
    const std::vector<std::uint8_t> original{0, 0, 128, 63, 0, 0, 0, 64};
    BOOST_CHECK(opened.source.initializerRangeSource->read(0, 8) == original);
  }
  catch (const std::runtime_error& error) {
    // A rename can change ctime; rejecting that original fd is also safe.
    BOOST_CHECK_EQUAL(error.what(), "DI_AUTOMATIC_ONNX_SOURCE_CHANGED");
  }
}

BOOST_AUTO_TEST_CASE(VerifiesReadsAcrossHashBlocksAndPhysicalFiles)
{
  FilesFixture files;
  std::vector<std::uint8_t> large(NativeCanonicalMaterialBundleMaxBytes + 16, 0x5a);
  FilesFixture::write(files.root / "weights/a.bin", large);
  auto budget = control();
  budget.maxSourceBytes = 4U << 20;
  const auto opened = openAutomaticOnnxSource(files.root / "model.onnx", budget);
  const auto reader = opened.source.initializerRangeSource;
  BOOST_CHECK(reader->read(NativeCanonicalMaterialBundleMaxBytes - 4, 16) ==
              std::vector<std::uint8_t>(16, 0x5a));
  const std::vector<std::uint8_t> crossing{
    0x5a, 0x5a, 0x5a, 0x5a, 0, 0, 64, 64, 0, 0, 128, 64};
  BOOST_CHECK(reader->read(large.size() - 4, crossing.size()) == crossing);
  BOOST_CHECK_THROW(reader->read(0, NativeCanonicalMaterialBundleMaxBytes + 1),
                    std::runtime_error);
  BOOST_CHECK_THROW(reader->read(reader->size() - 1, 2), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ReleasesDescriptorsAtEveryCancellationFence)
{
  FilesFixture files;
  const auto descriptorCount = [] {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      (void)entry;
      ++count;
    }
    return count;
  };
  const auto baseline = descriptorCount();
  std::size_t fences = 0;
  {
    auto measured = control();
    measured.requireActive = [&] { ++fences; };
    const auto opened = openAutomaticOnnxSource(files.root / "model.onnx", measured);
    BOOST_CHECK_EQUAL(opened.externalFiles.size(), 2);
  }
  BOOST_CHECK_EQUAL(descriptorCount(), baseline);
  BOOST_REQUIRE(fences > 0);
  for (std::size_t stop = 1; stop <= fences; ++stop) {
    auto cancelled = control();
    std::size_t calls = 0;
    cancelled.requireActive = [&] {
      if (++calls == stop) throw std::runtime_error("cancelled");
    };
    BOOST_CHECK_THROW(openAutomaticOnnxSource(files.root / "model.onnx", cancelled),
                      std::runtime_error);
    BOOST_CHECK_EQUAL(calls, stop);
    BOOST_CHECK_EQUAL(descriptorCount(), baseline);
  }
}

BOOST_AUTO_TEST_CASE(DerivesDescriptorAndInspectionWithoutCatalogOrProducerPlaceholders)
{
  FilesFixture files;
  const auto model = inspectAutomaticOnnxModel({files.root / "model.onnx"}, control());
  model.descriptor.validate();
  BOOST_CHECK_EQUAL(model.descriptor.contentDigest, model.opened.sourceDigest);
  BOOST_CHECK_EQUAL(model.descriptor.precision, "fp32");
  BOOST_CHECK_EQUAL(model.descriptor.graphDigest, model.inspection.graph.graphDigest);
  BOOST_CHECK(!model.descriptor.artifactReference);
  BOOST_CHECK(!model.descriptor.adapter.splittable);
  BOOST_REQUIRE_EQUAL(model.contract.operators.size(), 1);
  BOOST_CHECK_EQUAL(model.contract.operators[0].opType, "Add");
  BOOST_CHECK_EQUAL(model.contract.operators[0].opset, 13);
  BOOST_CHECK(model.contract.quantization.empty());
  const auto legacy = inspectNativeOnnxSourceGraph(
    model.opened.source, model.descriptor, control());
  BOOST_CHECK_EQUAL(legacy.graph.graphDigest, model.inspection.graph.graphDigest);
  BOOST_CHECK_EQUAL(legacy.canonicalIdentity.initializerDigest,
                    model.inspection.canonicalIdentity.initializerDigest);
}

BOOST_AUTO_TEST_CASE(PreservesUnknownRankScalarUnknownAxisAndOriginalInternalTypes)
{
  FilesFixture files;
  auto source = nBitsModel();
  auto* unknown = source.mutable_graph()->add_input();
  unknown->set_name("unknown-rank");
  unknown->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::DOUBLE);
  auto* scalar = source.mutable_graph()->add_input();
  scalar->set_name("scalar");
  auto* scalarType = scalar->mutable_type()->mutable_tensor_type();
  scalarType->set_elem_type(onnx::TensorProto::INT16);
  scalarType->mutable_shape();
  auto* axes = source.mutable_graph()->add_input();
  axes->set_name("axes");
  auto* axesType = axes->mutable_type()->mutable_tensor_type();
  axesType->set_elem_type(onnx::TensorProto::FLOAT);
  auto* shape = axesType->mutable_shape();
  shape->add_dim()->set_dim_param("B");
  shape->add_dim();
  shape->add_dim()->set_dim_value(0);
  shape->add_dim()->set_dim_value(128);
  writeModel(files.root / "model.onnx", source);
  const auto model = inspectAutomaticOnnxModel({files.root / "model.onnx"}, control());
  const auto find = [&](const std::string& name) -> const AutomaticTensorContract& {
    const auto it = std::find_if(model.contract.tensors.begin(), model.contract.tensors.end(),
      [&](const auto& contract) { return contract.name == "graph/" + name; });
    if (it == model.contract.tensors.end()) throw std::runtime_error("missing tensor fact");
    return *it;
  };
  const auto& unknownContract = find("unknown-rank");
  BOOST_CHECK_EQUAL(unknownContract.onnxElementType, onnx::TensorProto::DOUBLE);
  BOOST_CHECK(!unknownContract.wireElementType);
  BOOST_CHECK(!unknownContract.dimensions);
  const auto& scalarContract = find("scalar");
  BOOST_CHECK_EQUAL(scalarContract.onnxElementType, onnx::TensorProto::INT16);
  BOOST_CHECK(!scalarContract.wireElementType);
  BOOST_REQUIRE(scalarContract.dimensions);
  BOOST_CHECK(scalarContract.dimensions->empty());
  const auto& axesContract = find("axes");
  BOOST_REQUIRE(axesContract.wireElementType);
  BOOST_CHECK(*axesContract.wireElementType == TensorElementType::Float32);
  BOOST_REQUIRE(axesContract.dimensions);
  const auto& dimensions = *axesContract.dimensions;
  BOOST_REQUIRE_EQUAL(dimensions.size(), 4);
  BOOST_CHECK(dimensions[0].kind == DimensionConstraint::Kind::SYMBOL);
  BOOST_CHECK_EQUAL(dimensions[0].symbol, "graph/B");
  BOOST_CHECK(!dimensions[0].upperBound);
  BOOST_CHECK(dimensions[1].kind == DimensionConstraint::Kind::UNKNOWN);
  BOOST_CHECK(!dimensions[1].upperBound);
  BOOST_CHECK(dimensions[2].kind == DimensionConstraint::Kind::FIXED);
  BOOST_REQUIRE(dimensions[2].upperBound);
  BOOST_CHECK_EQUAL(*dimensions[2].upperBound, 0);
  BOOST_CHECK_EQUAL(dimensions[3].fixed, 128);
  BOOST_CHECK_EQUAL(axesContract.provenance.front(), "graph-input-v1");
}

BOOST_AUTO_TEST_CASE(SharesExactOnnxScalarMappingWithTheRuntimeBoundary)
{
  const std::vector<std::pair<int, TensorElementType>> expected = {
    {1, TensorElementType::Float32}, {10, TensorElementType::Float16},
    {7, TensorElementType::Int64}, {6, TensorElementType::Int32},
    {9, TensorElementType::Bool}, {2, TensorElementType::UInt8},
    {3, TensorElementType::INT8}, {16, TensorElementType::BFLOAT16}};
  for (const auto& [scalar, wire] : expected) {
    BOOST_CHECK_EQUAL(onnxTensorScalarType(wire), scalar);
    const auto actual = onnxTensorWireType(scalar);
    BOOST_REQUIRE(actual);
    BOOST_CHECK(*actual == wire);
  }
  for (const auto internal : {0, 4, 5, 8, 11, 17, 21, 22, 999})
    BOOST_CHECK(!onnxTensorWireType(internal));
  BOOST_CHECK_THROW(onnxTensorScalarType(static_cast<TensorElementType>(999)),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConstantHeaderOverridesStaleScalarAnnotationWithoutChangingLegacyInspection)
{
  FilesFixture files;
  onnx::ModelProto source;
  source.set_ir_version(8);
  source.add_opset_import()->set_version(13);
  auto* graph = source.mutable_graph();
  graph->set_name("literal-header-authority");
  const auto port = [](onnx::ValueInfoProto* value, const char* name, std::int64_t extent) {
    value->set_name(name);
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::INT64);
    type->mutable_shape()->add_dim()->set_dim_value(extent);
  };
  port(graph->add_input(), "dynamic", 1);
  port(graph->add_output(), "joined", 2);
  auto* stale = graph->add_value_info();
  stale->set_name("literal");
  stale->mutable_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto::INT64);
  stale->mutable_type()->mutable_tensor_type()->mutable_shape();
  auto* constant = graph->add_node();
  constant->set_op_type("Constant"); constant->add_output("literal");
  auto* attr = constant->add_attribute();
  attr->set_name("value"); attr->set_type(onnx::AttributeProto::TENSOR);
  auto* literal = attr->mutable_t();
  literal->set_data_type(onnx::TensorProto::INT64);
  literal->add_dims(1); literal->add_int64_data(-1);
  auto* concat = graph->add_node();
  concat->set_op_type("Concat"); concat->add_input("literal"); concat->add_input("dynamic");
  concat->add_output("joined");
  auto* axis = concat->add_attribute();
  axis->set_name("axis"); axis->set_type(onnx::AttributeProto::INT); axis->set_i(0);
  const auto bytes = source.SerializeAsString();
  writeModel(files.root / "model.onnx", source);
  const auto model = inspectAutomaticOnnxModel({files.root / "model.onnx"}, control());
  const auto found = std::find_if(model.contract.tensors.begin(), model.contract.tensors.end(),
    [](const auto& tensor) { return tensor.name == "graph/literal"; });
  BOOST_REQUIRE(found != model.contract.tensors.end());
  BOOST_REQUIRE(found->dimensions); BOOST_REQUIRE_EQUAL(found->dimensions->size(), 1);
  BOOST_CHECK_EQUAL(found->dimensions->front().fixed, 1);
  BOOST_CHECK_EQUAL(found->provenance.front(), "constant-header-v1");
  const auto legacy = inspectNativeOnnxSourceGraph(model.opened.source, model.descriptor, control());
  BOOST_CHECK_EQUAL(legacy.graphMetadataJson, model.inspection.graphMetadataJson);
  BOOST_CHECK_EQUAL(std::string(model.opened.source.modelBytes.begin(),
                               model.opened.source.modelBytes.end()), bytes);
  NamedTensor input{"dynamic", TensorElementType::Int64, {1}, std::vector<std::uint8_t>(8)};
  const auto envelope = bindAutomaticOnnxShapes(model.contract, {input}, 0, 32, control());
  BOOST_REQUIRE(envelope.tensors.at("graph/joined").maxBytes);
  BOOST_CHECK_EQUAL(*envelope.tensors.at("graph/joined").maxBytes, 16);
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  // The original graph is executable: ORT resolves the literal header correctly.
  Ort::Env environment(ORT_LOGGING_LEVEL_FATAL, "spec233-literal");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(environment, bytes.data(), bytes.size(), options);
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<std::int64_t, 1> data{7}, shape{1};
  auto actual = Ort::Value::CreateTensor<std::int64_t>(memory, data.data(), data.size(),
                                                     shape.data(), shape.size());
  const char* inputs[] = {"dynamic"}; const char* outputs[] = {"joined"};
  auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &actual, 1, outputs, 1);
  const auto actualShape = result.at(0).GetTensorTypeAndShapeInfo().GetShape();
  BOOST_REQUIRE_EQUAL(actualShape.size(), 1); BOOST_CHECK_EQUAL(actualShape[0], 2);
  const auto* values = result.at(0).GetTensorData<std::int64_t>();
  BOOST_CHECK_EQUAL(values[0], -1); BOOST_CHECK_EQUAL(values[1], 7);
#endif
}

BOOST_AUTO_TEST_CASE(ConstantQuantizationInputReplacesExistingOutputShapeInsteadOfAppendingAxes)
{
  FilesFixture files;
  auto source = quantizeModel();
  auto* graph = source.mutable_graph();
  const auto originalInput = graph->input(0);
  graph->mutable_input()->Clear();
  graph->add_output()->CopyFrom(originalInput);
  auto* constant = graph->add_node();
  constant->set_op_type("Constant"); constant->add_output(originalInput.name());
  auto* attr = constant->add_attribute();
  attr->set_name("value"); attr->set_type(onnx::AttributeProto::TENSOR);
  auto* literal = attr->mutable_t();
  literal->set_data_type(onnx::TensorProto::FLOAT); literal->add_dims(2); literal->add_dims(3);
  for (int i = 0; i < 6; ++i) literal->add_float_data(1.0F);
  graph->mutable_node()->SwapElements(0, graph->node_size() - 1);
  writeModel(files.root / "model.onnx", source);
  const auto valid = inspectAutomaticOnnxModel({files.root / "model.onnx"}, control());
  BOOST_REQUIRE_EQUAL(valid.contract.quantization.size(), 1);
  BOOST_CHECK(valid.contract.quantization.front().staticParametersValidated);
  auto* axis = graph->mutable_node(1)->add_attribute();
  axis->set_name("axis"); axis->set_type(onnx::AttributeProto::INT); axis->set_i(3);
  writeModel(files.root / "model.onnx", source);
  BOOST_CHECK_THROW(inspectAutomaticOnnxModel({files.root / "model.onnx"}, control()),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ReadsNBitsTailPackingAndImplicitZeroPointFromActualAttributes)
{
  FilesFixture files;
  const auto path = files.root / "model.onnx";
  writeModel(path, nBitsModel());
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_CHECK_EQUAL(model.descriptor.precision, "int4");
  BOOST_CHECK_EQUAL(model.descriptor.quantizationSubtype, "weight_only_int4");
  BOOST_REQUIRE_EQUAL(model.contract.quantization.size(), 1);
  const auto& quant = model.contract.quantization.front();
  BOOST_REQUIRE(quant.bits && quant.blockSize && quant.implicitZeroPoint);
  BOOST_CHECK_EQUAL(*quant.bits, 4);
  BOOST_CHECK_EQUAL(*quant.blockSize, 32);
  BOOST_CHECK_EQUAL(*quant.implicitZeroPoint, 8);
  BOOST_CHECK(quant.staticParametersValidated);
  BOOST_REQUIRE_EQUAL(quant.constantInputs.size(), 2);
  BOOST_CHECK_EQUAL(quant.constantInputs.front().storageBytes, 96);
  BOOST_CHECK_EQUAL(quant.constantInputs.front().onnxElementType, onnx::TensorProto::UINT8);
  auto renamed = nBitsModel();
  renamed.mutable_graph()->mutable_node(0)->set_name("unregistered-exporter-name");
  writeModel(path, renamed);
  const auto other = inspectAutomaticOnnxModel({path}, control());
  BOOST_CHECK_EQUAL(other.descriptor.precision, "int4");
  BOOST_CHECK(other.descriptor.modelDigest() != model.descriptor.modelDigest());
}

BOOST_AUTO_TEST_CASE(RejectsBadPackedGeometryAndWrongScaleTypes)
{
  FilesFixture files;
  const auto path = files.root / "model.onnx";
  auto bad = nBitsModel();
  bad.mutable_graph()->mutable_initializer(0)->set_dims(2, 15);
  bad.mutable_graph()->mutable_initializer(0)->set_raw_data(std::string(90, 'x'));
  writeModel(path, bad);
  BOOST_CHECK_THROW(inspectAutomaticOnnxModel({path}, control()), std::runtime_error);
  bad = nBitsModel();
  auto* scales = bad.mutable_graph()->mutable_initializer(1);
  scales->set_data_type(onnx::TensorProto::INT32);
  scales->clear_float_data();
  for (int i = 0; i < 6; ++i) scales->add_int32_data(1);
  writeModel(path, bad);
  BOOST_CHECK_THROW(inspectAutomaticOnnxModel({path}, control()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PreservesOneInitializerConsumedByMultipleOperators)
{
  FilesFixture files;
  auto source = nBitsModel();
  auto* graph = source.mutable_graph();
  graph->mutable_initializer(0)->set_dims(0, 33);
  graph->mutable_initializer(0)->set_raw_data(std::string(1056, static_cast<char>(0x88)));
  auto* scale = graph->mutable_initializer(1);
  scale->set_dims(0, 66);
  scale->clear_float_data();
  for (int i = 0; i < 66; ++i) scale->add_float_data(1.0f);
  for (auto& attr : *graph->mutable_node(0)->mutable_attribute())
    if (attr.name() == "N") attr.set_i(33);
  auto* next = graph->add_node();
  next->CopyFrom(graph->node(0));
  next->set_input(0, "y");
  next->set_output(0, "z");
  graph->mutable_output(0)->set_name("z");
  graph->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(33);
  const auto path = files.root / "model.onnx";
  writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_REQUIRE_EQUAL(model.contract.quantization.size(), 2);
  BOOST_REQUIRE(!model.contract.quantization[0].constantInputs.empty());
  BOOST_REQUIRE(!model.contract.quantization[1].constantInputs.empty());
  const std::vector<std::uint64_t> expected{0, 1};
  BOOST_CHECK(model.inspection.initializerConsumers.at("packed") == expected);
  BOOST_CHECK_EQUAL(model.contract.quantization[0].constantInputs[0].normalizedContentDigest,
                    model.contract.quantization[1].constantInputs[0].normalizedContentDigest);
}

BOOST_AUTO_TEST_CASE(PreservesBfloatScalesAndActivationTypesInNBitsInspection)
{
  FilesFixture files;
  auto source = nBitsModel();
  auto* graph = source.mutable_graph();
  auto* scale = graph->mutable_initializer(1);
  scale->set_data_type(onnx::TensorProto::BFLOAT16);
  scale->clear_float_data();
  std::string bits;
  for (int i = 0; i < 6; ++i) bits.append("\x80\x3f", 2);
  scale->set_raw_data(bits);
  graph->mutable_input(0)->mutable_type()->mutable_tensor_type()
    ->set_elem_type(onnx::TensorProto::BFLOAT16);
  graph->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->set_elem_type(onnx::TensorProto::BFLOAT16);
  const auto path = files.root / "model.onnx";
  writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_REQUIRE_EQUAL(model.contract.quantization.size(), 1);
  BOOST_CHECK(model.contract.quantization[0].staticParametersValidated);
  BOOST_REQUIRE_EQUAL(model.inspection.graph.modelOutputs.size(), 1);
  BOOST_CHECK_EQUAL(model.inspection.graph.modelOutputs[0].dtype, "bfloat16");
}

BOOST_AUTO_TEST_CASE(NormalizesZeroElementInitializerWithoutDivisionByZero)
{
  onnx::TensorProto tensor;
  tensor.set_name("empty-state");
  tensor.set_data_type(onnx::TensorProto::FLOAT16);
  for (const auto dim : {1, 8, 0, 128}) tensor.add_dims(dim);
  tensor.set_raw_data("");
  std::string bytes;
  BOOST_REQUIRE(tensor.SerializeToString(&bytes));
  const auto result = normalizedOnnxInitializerPayload({bytes.begin(), bytes.end()});
  BOOST_CHECK(result.content.empty());
  const std::vector<std::int64_t> expected{1, 8, 0, 128};
  BOOST_CHECK(result.shape == expected);
}

BOOST_AUTO_TEST_CASE(UsesInstalledStandardSchemaDefaultsAndRejectsBadChannelScales)
{
  FilesFixture files;
  const auto path = files.root / "model.onnx";
  writeModel(path, quantizeModel());
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_REQUIRE_EQUAL(model.contract.quantization.size(), 1);
  const auto& quant = model.contract.quantization.front();
  BOOST_REQUIRE(quant.axis && quant.bits);
  BOOST_CHECK_EQUAL(*quant.axis, 1);
  BOOST_CHECK_EQUAL(*quant.bits, 8);
  BOOST_CHECK(quant.staticParametersValidated);
  BOOST_CHECK_EQUAL(model.descriptor.quantizationSubtype, "none");
  auto bad = quantizeModel();
  auto* scale = bad.mutable_graph()->mutable_initializer(0);
  scale->set_dims(0, 2);
  scale->mutable_float_data()->RemoveLast();
  writeModel(path, bad);
  BOOST_CHECK_THROW(inspectAutomaticOnnxModel({path}, control()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RejectsMalformedArityEvenWhenAttributesAreUnresolved)
{
  FilesFixture files;
  auto source = quantizeModel();
  auto* node = source.mutable_graph()->mutable_node(0);
  node->clear_input();
  auto* attr = node->add_attribute();
  attr->set_name("axis");
  attr->set_type(onnx::AttributeProto::INT);
  attr->set_ref_attr_name("caller-axis");
  const auto path = files.root / "model.onnx";
  writeModel(path, source);
  BOOST_CHECK_THROW(inspectAutomaticOnnxModel({path}, control()), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RecordsReferencedFunctionScopesWithoutInventingDynamicParameterValues)
{
  FilesFixture files;
  auto source = quantizeModel();
  auto* custom = source.add_opset_import();
  custom->set_domain("org.ndnsf.fixture");
  custom->set_version(1);
  auto* function = source.add_functions();
  function->set_name("UnregisteredFunction");
  function->set_domain("org.ndnsf.fixture");
  function->add_opset_import()->set_version(13);
  for (const auto* name : {"fx", "fs", "fz"}) function->add_input(name);
  function->add_output("fy");
  auto* body = function->add_node();
  body->set_op_type("QuantizeLinear");
  for (const auto* name : {"fx", "fs", "fz"}) body->add_input(name);
  body->add_output("fy");
  auto* call = source.mutable_graph()->mutable_node(0);
  call->set_domain("org.ndnsf.fixture");
  call->set_op_type("UnregisteredFunction");
  const auto path = files.root / "model.onnx";
  writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_REQUIRE_EQUAL(model.contract.operators.size(), 2);
  BOOST_CHECK(model.contract.operators[0].atomicBody);
  BOOST_CHECK_EQUAL(model.contract.operators[1].scope, "function/0");
  BOOST_REQUIRE_EQUAL(model.contract.quantization.size(), 1);
  BOOST_CHECK(!model.contract.quantization[0].staticParametersValidated);
  BOOST_CHECK(model.contract.quantization[0].constantInputs.empty());
  BOOST_CHECK(!model.contract.quantization[0].bits);
}

BOOST_AUTO_TEST_CASE(ReadsTypedAndRawIntegerLiteralsWithoutSignednessOrEndianLoss)
{
  FilesFixture files;
  auto source = integerLiteralModel();
  auto* typed = source.mutable_graph()->add_initializer();
  typed->set_name("small,I32=values"); typed->set_data_type(onnx::TensorProto::INT32);
  typed->add_dims(4);
  for (const auto v : {std::numeric_limits<std::int32_t>::min(), -1, 0,
                        std::numeric_limits<std::int32_t>::max()}) typed->add_int32_data(v);
  auto* raw = source.mutable_graph()->add_initializer();
  raw->set_name("raw-I64"); raw->set_data_type(onnx::TensorProto::INT64); raw->add_dims(2);
  raw->set_raw_data(std::string(7, '\0') + std::string(1, static_cast<char>(0x80)) +
                    std::string(7, static_cast<char>(0xff)) +
                    std::string(1, static_cast<char>(0x7f)));
  auto* node = source.mutable_graph()->add_node();
  node->set_op_type("Constant"); node->add_output("tensor-scalar");
  auto* attr = node->add_attribute(); attr->set_name("value");
  attr->set_type(onnx::AttributeProto::TENSOR);
  attr->mutable_t()->set_data_type(onnx::TensorProto::INT64);
  attr->mutable_t()->add_int64_data(-7);
  const auto path = files.root / "model.onnx"; writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  const std::vector<std::int64_t> expected32{std::numeric_limits<std::int32_t>::min(), -1, 0,
                                           std::numeric_limits<std::int32_t>::max()};
  const std::vector<std::int64_t> expected64{std::numeric_limits<std::int64_t>::min(),
                                           std::numeric_limits<std::int64_t>::max()};
  BOOST_CHECK(model.contract.integerLiterals.at("graph/small,I32=values") == expected32);
  BOOST_CHECK(model.contract.integerLiterals.at("graph/raw-I64") == expected64);
  BOOST_CHECK(model.contract.integerLiterals.at("graph/tensor-scalar") ==
              std::vector<std::int64_t>{-7});
  BOOST_CHECK_EQUAL(model.contract.integerLiterals.size(), 3);
}

BOOST_AUTO_TEST_CASE(BoundsScalarEmptyListAndLiteralElementCountWithoutInventingValues)
{
  FilesFixture files;
  auto source = integerLiteralModel();
  for (const auto count : {0, 64, 65}) {
    auto* node = source.mutable_graph()->add_node(); node->set_op_type("Constant");
    node->add_output("list-" + std::to_string(count));
    auto* attr = node->add_attribute(); attr->set_name("value_ints");
    attr->set_type(onnx::AttributeProto::INTS);
    for (int i = 0; i < count; ++i) attr->add_ints(i - 32);
    auto* tensor = source.mutable_graph()->add_initializer();
    tensor->set_name("tensor-" + std::to_string(count));
    tensor->set_data_type(onnx::TensorProto::INT64); tensor->add_dims(count);
    for (int i = 0; i < count; ++i) tensor->add_int64_data(i - 32);
  }
  auto* scalar = source.mutable_graph()->add_node(); scalar->set_op_type("Constant");
  scalar->add_output("integer-scalar");
  auto* attr = scalar->add_attribute(); attr->set_name("value_int");
  attr->set_type(onnx::AttributeProto::INT); attr->set_i(-99);
  const auto path = files.root / "model.onnx"; writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  const auto& literals = model.contract.integerLiterals;
  BOOST_CHECK(literals.at("graph/list-0").empty());
  BOOST_CHECK(literals.at("graph/tensor-0").empty());
  BOOST_REQUIRE_EQUAL(literals.at("graph/list-64").size(), 64);
  BOOST_CHECK_EQUAL(literals.at("graph/list-64").front(), -32);
  BOOST_CHECK_EQUAL(literals.at("graph/list-64").back(), 31);
  BOOST_CHECK(literals.at("graph/list-64") == literals.at("graph/tensor-64"));
  BOOST_CHECK_EQUAL(literals.count("graph/list-65"), 0);
  BOOST_CHECK_EQUAL(literals.count("graph/tensor-65"), 0);
  BOOST_CHECK(literals.at("graph/integer-scalar") == std::vector<std::int64_t>{-99});
}

BOOST_AUTO_TEST_CASE(ReadsSmallExternalIntegerRangeWithoutChangingPhysicalSource)
{
  FilesFixture files;
  auto source = integerLiteralModel();
  auto* tensor = source.mutable_graph()->add_initializer();
  tensor->set_name("range"); tensor->set_data_type(onnx::TensorProto::INT32); tensor->add_dims(2);
  tensor->set_data_location(onnx::TensorProto::EXTERNAL);
  for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
         {"location", "weights/integers.bin"}, {"offset", "4"}, {"length", "8"}}) {
    auto* item = tensor->add_external_data(); item->set_key(pair.first); item->set_value(pair.second);
  }
  const std::vector<std::uint8_t> bytes{42, 42, 42, 42, 0, 0, 0, 128, 255, 255, 255, 127};
  FilesFixture::write(files.root / "weights/integers.bin", bytes);
  const auto path = files.root / "model.onnx"; writeModel(path, source);
  std::string original; BOOST_REQUIRE(source.SerializeToString(&original));
  const auto model = inspectAutomaticOnnxModel({path}, control());
  const std::vector<std::int64_t> expected{std::numeric_limits<std::int32_t>::min(),
                                         std::numeric_limits<std::int32_t>::max()};
  BOOST_CHECK(model.contract.integerLiterals.at("graph/range") == expected);
  BOOST_CHECK(model.opened.source.modelBytes ==
              std::vector<std::uint8_t>(original.begin(), original.end()));
  std::ifstream input(files.root / "weights/integers.bin", std::ios::binary);
  const std::vector<std::uint8_t> after{std::istreambuf_iterator<char>(input),
                                       std::istreambuf_iterator<char>()};
  BOOST_CHECK(after == bytes);
}

BOOST_AUTO_TEST_CASE(LocalFunctionShadowCannotInventABuiltinConstantLiteral)
{
  FilesFixture files;
  auto source = integerLiteralModel();
  auto* fn = source.add_functions();
  fn->set_name("Constant"); fn->add_input("arg"); fn->add_output("result");
  fn->add_attribute("value_int");
  fn->add_opset_import()->set_version(13);
  auto* body = fn->add_node(); body->set_op_type("Identity"); body->add_input("arg");
  body->add_output("result");
  auto* input = source.mutable_graph()->add_input(); input->set_name("function-argument");
  auto* type = input->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  type->mutable_shape()->add_dim()->set_dim_value(2);
  auto* call = source.mutable_graph()->add_node();
  call->set_op_type("Constant"); call->add_input("function-argument"); call->add_output("opaque");
  auto* attr = call->add_attribute(); attr->set_name("value_int");
  attr->set_type(onnx::AttributeProto::INT); attr->set_i(777);
  const auto path = files.root / "model.onnx"; writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_CHECK(model.contract.integerLiterals.empty());
}

BOOST_AUTO_TEST_CASE(ReferencedAttributeDoesNotSupplyItsUnresolvedFallbackValue)
{
  FilesFixture files;
  auto source = integerLiteralModel();
  auto* node = source.mutable_graph()->add_node(); node->set_op_type("Constant");
  node->add_output("unresolved");
  auto* attr = node->add_attribute(); attr->set_name("value_int");
  attr->set_type(onnx::AttributeProto::INT); attr->set_ref_attr_name("unbound");
  attr->set_i(555);
  const auto path = files.root / "model.onnx"; writeModel(path, source);
  const auto model = inspectAutomaticOnnxModel({path}, control());
  BOOST_CHECK(model.contract.integerLiterals.empty());
  BOOST_CHECK_EQUAL(model.contract.operators.back().attributeReferences.at("value_int"), "unbound");
}

BOOST_AUTO_TEST_CASE(ZeroElementTensorCannotDiscardNonemptyTypedOrRawSourceValues)
{
  for (const auto field : {0, 1, 2, 3, 4, 5, 6}) {
    onnx::TensorProto tensor;
    tensor.set_name("malformed-empty"); tensor.add_dims(0);
    tensor.set_data_type(onnx::TensorProto::INT32);
    if (field == 0) tensor.add_int32_data(1);
    if (field == 1) tensor.add_int64_data(1);
    if (field == 2) tensor.add_float_data(1);
    if (field == 3) tensor.add_double_data(1);
    if (field == 4) tensor.add_uint64_data(1);
    if (field == 5) tensor.add_string_data("unexpected");
    if (field == 6) tensor.set_raw_data(std::string(4, '\0'));
    std::string encoded; BOOST_REQUIRE(tensor.SerializeToString(&encoded));
    BOOST_CHECK_THROW(normalizedOnnxInitializerPayload({encoded.begin(), encoded.end()}),
                      std::runtime_error);
  }
  FilesFixture files;
  for (const auto type : {onnx::TensorProto::INT32, onnx::TensorProto::INT64}) {
    auto source = integerLiteralModel();
    auto* tensor = source.mutable_graph()->add_initializer();
    tensor->set_name("bad-zero"); tensor->set_data_type(type); tensor->add_dims(0);
    if (type == onnx::TensorProto::INT32) tensor->add_int32_data(1);
    else tensor->add_int64_data(1);
    const auto path = files.root / "model.onnx"; writeModel(path, source);
    BOOST_CHECK_THROW(inspectAutomaticOnnxModel({path}, control()), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(OneShotOwnerExceptionsInsideGenerationCannotBecomeUnsupportedDiagnostics)
{
  FilesFixture files;
  const auto path = files.root / "model.onnx";
  writeModel(path, integerLiteralModel());
  std::uint64_t allFences = 0;
  auto counting = control(); counting.requireActive = [&] { ++allFences; };
  const auto baseline = inspectAutomaticOnnxModel({path}, counting);
  BOOST_REQUIRE(!baseline.contract.generation);
  std::uint64_t finalValidation = 0;
  counting = control(); counting.requireActive = [&] { ++finalValidation; };
  baseline.contract.validate(counting);
  // The inspected source path validates the contract and checks one final
  // owner fence after derivation. Select the last fence inside that derivation.
  BOOST_REQUIRE_GT(allFences, finalValidation + 1);
  const auto insideDerivation = allFences - finalValidation - 1;
  const auto descriptors = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
  };
  const auto before = descriptors();
  for (const bool semanticType : {false, true}) {
    std::uint64_t calls = 0;
    bool fired = false;
    auto cancelled = control();
    cancelled.requireActive = [&] {
      if (++calls != insideDerivation) return;
      fired = true;
      if (semanticType) throw AutomaticGenerationUnproven("one-shot owner cancellation");
      throw std::invalid_argument("one-shot owner cancellation");
    };
    BOOST_CHECK_EXCEPTION(inspectAutomaticOnnxModel({path}, cancelled), std::invalid_argument,
      [](const auto& error) { return std::string(error.what()) == "one-shot owner cancellation"; });
    BOOST_CHECK(fired);
    BOOST_CHECK_EQUAL(descriptors(), before);
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
