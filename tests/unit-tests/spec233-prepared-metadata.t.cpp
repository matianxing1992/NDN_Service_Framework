#include "tests/boost-test.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"
#include "tests/fixtures/spec233/automatic-source-projection.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-admission.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-io.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-filesystem-artifacts.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-object-keys.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeRequestOptionsTransport.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <onnx/onnx_pb.h>
#include <boost/process.hpp>
#include <boost/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace ndnsf::di::tests {
namespace {

NativeAssemblyControl
metadataControl()
{
  return {std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
          8U << 20, 8U << 20};
}

class MetadataSourceFixture
{
public:
  explicit MetadataSourceFixture(bool external, bool annotated = false,
                                 std::uint32_t transformSeed = 0)
  {
    static std::uint64_t sequence = 0;
    root = std::filesystem::temp_directory_path() /
      ("spec233-metadata-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence));
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture exists");
    try {
      std::filesystem::permissions(root, std::filesystem::perms::owner_all);
      onnx::ModelProto model; model.set_ir_version(8);
      model.add_opset_import()->set_version(13);
      auto* graph = model.mutable_graph(); graph->set_name("independent-metadata-source");
      const auto port = [](onnx::ValueInfoProto* value, const char* name, std::int64_t width) {
        value->set_name(name);
        auto* type = value->mutable_type()->mutable_tensor_type();
        type->set_elem_type(onnx::TensorProto::FLOAT);
        type->mutable_shape()->add_dim()->set_dim_param("batch");
        type->mutable_shape()->add_dim()->set_dim_value(width);
      };
      port(graph->add_input(), "x", 3); port(graph->add_output(), "y", 4);
      const auto initializer = [&](const char* name, const char* location,
                                    std::initializer_list<std::int64_t> dimensions) {
        auto* tensor = graph->add_initializer(); tensor->set_name(name);
        tensor->set_data_type(onnx::TensorProto::FLOAT);
        std::uint64_t count = 1;
        for (const auto dimension : dimensions) { tensor->add_dims(dimension); count *= dimension; }
        if (!external) {
          for (std::uint64_t i = 0; i < count; ++i) tensor->add_float_data(1.0F);
          return;
        }
        tensor->set_data_location(onnx::TensorProto::EXTERNAL);
        for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
               {"location", location}, {"offset", "0"}, {"length", std::to_string(count * 4)}}) {
          auto* field = tensor->add_external_data(); field->set_key(key); field->set_value(value);
        }
        const auto path = root / location;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        const char one[4]{0, 0, static_cast<char>(0x80), static_cast<char>(0x3f)};
        for (std::uint64_t i = 0; i < count; ++i) output.write(one, 4);
        output.close(); if (!output) throw std::runtime_error("fixture weight write");
      };
      initializer("weights", "a.bin", {3, 4}); initializer("bias", "nested/b.bin", {4});
      auto* product = graph->add_node(); product->set_op_type("MatMul");
      product->add_input("x"); product->add_input("weights"); product->add_output("hidden");
      auto* sum = graph->add_node(); sum->set_op_type("Add");
      sum->add_input("hidden"); sum->add_input("bias"); sum->add_output("y");
      if (annotated) port(graph->add_value_info(), "hidden", 4);
      if (transformSeed != 0) {
        std::mt19937 names(transformSeed);
        const auto nextName = [&] {
          return std::string("opaque-\xce\xbb\t,=") + std::to_string(names());
        };
        const std::map<std::string, std::string> renamed{
          {"x", nextName()}, {"y", nextName()}, {"weights", nextName()},
          {"bias", nextName()}, {"hidden", nextName()}};
        inputName = renamed.at("x");
        outputName = renamed.at("y");
        graph->set_name(nextName());
        for (auto& value : *graph->mutable_input()) {
          value.set_name(renamed.at(value.name()));
          for (auto& dimension : *value.mutable_type()->mutable_tensor_type()
                                    ->mutable_shape()->mutable_dim())
            dimension.clear_dim_param();
        }
        for (auto& value : *graph->mutable_output()) {
          value.set_name(renamed.at(value.name()));
          for (auto& dimension : *value.mutable_type()->mutable_tensor_type()
                                    ->mutable_shape()->mutable_dim())
            dimension.clear_dim_param();
        }
        for (auto& tensor : *graph->mutable_initializer())
          tensor.set_name(renamed.at(tensor.name()));
        // Copy before clearing repeated protobuf storage; no node pointer escapes.
        auto multiply = graph->node(0);
        auto add = graph->node(1);
        for (auto* node : {&multiply, &add}) {
          for (auto& name : *node->mutable_input()) name = renamed.at(name);
          for (auto& name : *node->mutable_output()) name = renamed.at(name);
          node->set_name(nextName());
        }
        graph->clear_value_info();
        graph->clear_node();
        const auto identityOutput = nextName();
        const auto castOutput = nextName();
        const auto addOutput = nextName();
        const auto transposeOutput = nextName();
        const auto unary = [&] (const char* type, const std::string& input,
                                const std::string& output) {
          auto* node = graph->add_node();
          node->set_name(nextName()); node->set_op_type(type);
          node->add_input(input); node->add_output(output);
          return node;
        };
        unary("Identity", inputName, identityOutput);
        auto* cast = unary("Cast", identityOutput, castOutput);
        auto* target = cast->add_attribute();
        target->set_name("to"); target->set_type(onnx::AttributeProto::INT);
        target->set_i(onnx::TensorProto::FLOAT);
        multiply.set_input(0, castOutput);
        *graph->add_node() = multiply;
        add.set_output(0, addOutput);
        *graph->add_node() = add;
        for (const auto& endpoints :
             {std::make_pair(addOutput, transposeOutput),
              std::make_pair(transposeOutput, outputName)}) {
          auto* transpose = unary("Transpose", endpoints.first, endpoints.second);
          auto* permutation = transpose->add_attribute();
          permutation->set_name("perm"); permutation->set_type(onnx::AttributeProto::INTS);
          permutation->add_ints(1); permutation->add_ints(0);
        }
      }
      path = root / "model.onnx";
      std::ofstream output(path, std::ios::binary);
      if (!model.SerializeToOstream(&output)) throw std::runtime_error("fixture model write");
      output.close(); if (!output) throw std::runtime_error("fixture model close");
    }
    catch (...) {
      std::filesystem::remove_all(root);
      throw;
    }
  }

  MetadataSourceFixture(const MetadataSourceFixture&) = delete;
  MetadataSourceFixture& operator=(const MetadataSourceFixture&) = delete;

  ~MetadataSourceFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path root;
  std::filesystem::path path;
  std::string inputName = "x";
  std::string outputName = "y";
};

NativeInspectedModel
metadataModel(const AutomaticOnnxModel& automatic)
{
  NativeInspectedModel model;
  model.descriptor = automatic.descriptor; model.graph = automatic.inspection.graph;
  model.canonicalSourceName = ndn::Name("/metadata-producer/di-model/source").toUri(
    ndn::name::UriFormat::CANONICAL);
  model.canonicalSourceDigest = automatic.opened.modelDigest;
  model.canonicalSourceBytes = automatic.opened.source.modelBytes.size();
  model.modelManifestDigest = nativePlanningDigest("independent-metadata-fixture-manifest");
  model.canonicalGraphDigest = automatic.inspection.canonicalIdentity.graphDigest;
  model.canonicalInitializerDigest = automatic.inspection.canonicalIdentity.initializerDigest;
  if (automatic.opened.source.initializerRangeSource) {
    model.canonicalInitializerBytes = nativeCanonicalInitializerByteSize(automatic.opened.source);
    model.canonicalInitializerObjectDigest = nativeCanonicalInitializerDigest(
      automatic.opened.source, metadataControl());
  }
  return model;
}

NativeJson
encodedMetadata(const AutomaticOnnxModel& model)
{
  return nativeParseJson(encodeNativePreparedOnnxMetadata(
    metadataModel(model), model.inspection, &model, metadataControl()));
}

AutomaticOnnxSourceBinding
sourceBinding(const AutomaticOnnxModel& automatic)
{
  const auto model = metadataModel(automatic);
  return AutomaticOnnxSourceBinding::fromSourceMetadata(
    encodedMetadata(automatic).at("automatic_onnx"), model.canonicalSourceDigest,
    model.canonicalInitializerBytes, model.canonicalInitializerObjectDigest, metadataControl());
}

NativeSelectionProjectionV3
sourceProjection(const AutomaticOnnxModel& model, std::uint64_t batch = 2)
{
  const auto& scopedName = model.contract.modelInputs.at(0);
  const std::vector<NamedTensor> inputs{{scopedName.substr(6), TensorElementType::Float32,
    {static_cast<std::int64_t>(batch), 3}, std::vector<std::uint8_t>(batch * 12, 0)}};
  return fixture::automaticSourceProjection(model, automaticOnnxShapeSeed(
    bindAutomaticOnnxShapes(model.contract, inputs, 0, 16, metadataControl())));
}

NativeModelRunnerSpec
sourceRunnerSpec(const MetadataSourceFixture& file, const AutomaticOnnxModel& model,
                 const NativeSelectionProjectionV3& projection)
{
  return fixture::automaticSourceRunnerSpec(file.path.string(), file.root.string(), projection,
    sourceBinding(model).canonicalJson(metadataControl()));
}

NativeCanonicalCatalogEntry
sourceCatalogEntry(const AutomaticOnnxModel& automatic)
{
  NativeCanonicalCatalogEntry entry;
  entry.model = metadataModel(automatic);
  entry.source = automatic.opened.source;
  entry.source.preparedMetadataJson = nativeCanonicalJson(encodedMetadata(automatic));
  entry.sourceGraphInspection = automatic.inspection;
  const auto role = sourceProjection(automatic).assembly;
  entry.recipe = {role.artifactProfileDigest, role.assemblerDescriptorDigest, role.backendAbi,
    role.precision, role.quantization, role.layout, role.padding, role.protectionEpoch,
    role.maxSourceBytes, role.maxAssembledBytes, role.maxNodes};
  entry.maxPayloadBytes = 4U << 20;
  return entry;
}

std::string
readCacheFixtureFile(const std::filesystem::path& path, std::uint64_t maximum = 1U << 20)
{
  const auto size = std::filesystem::file_size(path);
  if (size == 0 || size > maximum) throw std::runtime_error("cache fixture file size");
  std::ifstream input(path, std::ios::binary);
  std::string value(static_cast<std::size_t>(size), '\0');
  if (!input.read(value.data(), value.size()) || input.peek() != EOF)
    throw std::runtime_error("cache fixture file read");
  return value;
}

void
writeCacheFixtureFile(const std::filesystem::path& path, const std::string& value)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), value.size());
  output.close();
  if (!output) throw std::runtime_error("cache fixture file write");
}

NativeSelectionProjectionV3
completeFilesystemSelection(NativeSelectionProjectionV3 projection, const std::string& request)
{
  // Local fixture policy commitments are not evidence of network ACKs/grants.
  const auto& role = projection.assembly;
  projection.requestId = request;
  const auto identity = request + projection.automaticShapeSeed->envelopeDigest;
  projection.planCoreDigest = nativePlanningDigest("fs-core:" + identity);
  projection.planDigest = nativePlanningDigest("fs-plan:" + identity);
  projection.requestContractDigest = nativePlanningDigest("fs-request:" + identity);
  projection.ackClosedDigest = nativePlanningDigest("fs-local-ack-policy");
  projection.offerDigest = nativePlanningDigest("fs-local-cpu-offer");
  projection.securityPolicySnapshotDigest = nativePlanningDigest("fs-local-policy");
  projection.deadlineMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
    std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + 120000;
  projection.executionRole.roleId = role.selectedRole;
  projection.executionRole.stageId = role.role;
  projection.executionRole.rank = role.rank;
  projection.executionRole.layerBegin = role.layerBegin;
  projection.executionRole.layerEnd = role.layerEnd;
  projection.executionRole.backend = role.backend;
  projection.dataflow.requestId = request;
  projection.dataflow.planDigest = projection.planDigest;
  auto flow = nativeDataflowJson(projection.dataflow);
  flow.erase("dataflow_digest");
  projection.dataflow.dataflowDigest = nativePlanningDigest(nativeCanonicalJson(flow));
  auto& device = projection.deviceBinding;
  device.mode = "CPU"; device.provider = projection.provider; device.role = role.selectedRole;
  device.offerDigest = projection.offerDigest;
  device.topologyProfileDigest = nativePlanningDigest("fs-local-cpu-topology");
  device.resourceSnapshotDigest = nativePlanningDigest("fs-local-cpu-resource");
  device.resourceSequence = 1; device.sharingPolicy = "EXCLUSIVE_ROLE";
  // This is the actual production writer/parser, not a test projection codec.
  const auto wire = nativeSelectionProjectionV3ToJson(projection);
  std::istringstream input(wire);
  auto decoded = nativeSelectionProjectionV3FromJson(input, role.selectedRole);
  decoded.canonicalArtifactName = projection.canonicalArtifactName;
  decoded.plan.serviceName = projection.plan.serviceName;
  decoded.plan.modelName = projection.plan.modelName;
  (void)roleSpecFromSelectionProjectionV3(decoded, decoded.provider);
  return decoded;
}

std::map<std::string, TensorBundle>
sourceInputs(std::uint64_t batch, const std::string& inputName = "x")
{
  std::vector<float> values(batch * 3);
  for (std::size_t index = 0; index < values.size(); ++index)
    values[index] = static_cast<float>(index + 1);
  std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return {{"input", makeEncodedTensorBundle("input",
    {{inputName, TensorElementType::Float32,
      {static_cast<std::int64_t>(batch), 3}, std::move(bytes)}})}};
}

void
checkSourceResult(const std::map<std::string, TensorBundle>& result, std::uint64_t batch,
                  const std::string& outputName = "y")
{
  BOOST_REQUIRE_EQUAL(result.size(), 1U);
  const auto tensors = decodeTensorBundle(result.begin()->second.payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors[0].name, outputName);
  BOOST_CHECK(tensors[0].shape == std::vector<std::int64_t>({static_cast<std::int64_t>(batch), 4}));
  BOOST_REQUIRE_EQUAL(tensors[0].payload.size(), batch * 4 * sizeof(float));
  for (std::size_t row = 0; row < batch; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      float value;
      std::memcpy(&value, tensors[0].payload.data() + (row * 4 + column) * sizeof(float), sizeof(float));
      // Source is MatMul(x, ones) + one, independent of runtime serializers.
      BOOST_CHECK_EQUAL(value, static_cast<float>(9 * row + 7));
    }
  }
}

// No transport edges are declared by these worker-only fixtures.
class SourceWorkerIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge&) override
  {
    throw std::logic_error("unexpected source worker dependency fetch");
  }

  void
  publishOutput(const std::string&, const DependencyEdge&, const TensorBundle&) override
  {
    throw std::logic_error("unexpected source worker dependency publication");
  }
};

// Bounded original-wire facility only, not a Repo durability qualification.
// Production publication/index/key-lease/AEAD code performs the model work.
struct SourcePacketFacility
{
  using Bytes = std::vector<std::uint8_t>;
  std::map<ndn::Name, Bytes> packets;
  std::map<ndn::Name, std::pair<std::string, std::uint64_t>> groups;
  std::map<ndn::Name, unsigned> opened;

  NativeStoredModelPacketIo
  port()
  {
    NativeStoredModelPacketIo io;
    io.requireCurrent = [] {};
    io.commit = [this] (NativeStoredModelPacketIo::PacketWires wires, const std::string& expected) {
      BOOST_REQUIRE(!wires.empty());
      BOOST_REQUIRE_LE(wires.size(), 128U);
      Bytes retained;
      ndn::Name first;
      for (const auto& wire : wires) {
        const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
        if (first.empty()) first = packet.getName();
        const auto existing = packets.find(packet.getName());
        if (existing != packets.end()) BOOST_REQUIRE(existing->second == wire);
        packets[packet.getName()] = wire;
        const auto& stored = packets.at(packet.getName());
        retained.insert(retained.end(), stored.begin(), stored.end());
      }
      BOOST_REQUIRE_LE(retained.size(), 1U << 20);
      BOOST_REQUIRE_EQUAL(nativePlanningDigest(retained.data(), retained.size()), expected);
      groups[first] = {expected, retained.size()};
    };
    io.requireCommitted = [this] (const ndn::Name& name, const std::string& digest,
                                  std::uint64_t bytes) {
      BOOST_REQUIRE(groups.count(name) == 1);
      BOOST_REQUIRE_EQUAL(groups.at(name).first, digest);
      BOOST_REQUIRE_EQUAL(groups.at(name).second, bytes);
    };
    io.fetch = [this] (const std::vector<ndn::Name>& names) {
      NativeStoredModelPacketIo::PacketWires wires;
      for (const auto& name : names) wires.push_back(packets.at(name));
      return wires;
    };
    return io;
  }

  std::optional<ndn::Buffer>
  read(const NativeStoredModelObjectKeyLease& lease, const ndn::Name& name)
  {
    // No master key enters the Provider read port. Missing assignment keys
    // fail in the production lease before any ciphertext is opened.
    const auto& object = lease.object(name);
    BOOST_REQUIRE_LE(object.ciphertextBytes, 1U << 20);
    const auto segmentBytes = lease.segmentBytes(name);
    BOOST_REQUIRE_GT(segmentBytes, 0U);
    Bytes ciphertext;
    for (std::uint64_t offset = 0, segment = 0; offset < object.ciphertextBytes; ++segment) {
      auto packetName = name; packetName.appendSegment(segment);
      const auto& wire = packets.at(packetName);
      const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
      BOOST_REQUIRE(packet.getName() == packetName);
      const auto& content = packet.getContent();
      ciphertext.insert(ciphertext.end(), content.value_begin(), content.value_end());
      offset += content.value_size();
      BOOST_REQUIRE_GT(content.value_size(), 0U);
    }
    auto plaintext = lease.open(name, ciphertext);
    ++opened[name];
    return ndn::Buffer(plaintext.begin(), plaintext.end());
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233PreparedMetadata)

BOOST_AUTO_TEST_CASE(InventoryHashRetainsSixActualSourceGoldens)
{
  std::ifstream input("tests/fixtures/spec233/source-inventory-v1.json");
  BOOST_REQUIRE(input.good());
  const auto fixture = nativeParseJson(std::string(std::istreambuf_iterator<char>(input), {}));
  BOOST_REQUIRE_EQUAL(fixture.at("rows").size(), 6);
  for (const auto& row : fixture.at("rows")) {
    std::vector<AutomaticOnnxSourceFile> files;
    for (const auto& file : row.at("externalFiles"))
      files.push_back({file.at("location"), file.at("bytes"), file.at("digest")});
    BOOST_CHECK_EQUAL(automaticOnnxSourceDigest(row.at("modelDigest"), files, metadataControl()),
                      row.at("sourceDigest").get<std::string>());
  }
}

BOOST_AUTO_TEST_CASE(ActualInlineAndMultiFileModelsRestoreCompleteContracts)
{
  for (const bool external : {false, true}) {
    const MetadataSourceFixture sourceFixture(external);
    const auto model = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
    auto metadata = encodedMetadata(model);
    BOOST_CHECK_EQUAL(metadata.at("schema"), "ndnsf-di-prepared-metadata-v2");
    BOOST_CHECK_EQUAL(metadata.at("automatic_onnx").at("externalFiles").size(), external ? 2 : 0);
    for (const bool publishedExtension : {false, true}) {
      if (publishedExtension) metadata["protectedObjects"] = NativeJson::array();
      const auto restored = decodeNativePreparedOnnxMetadata(metadata, metadataControl());
      BOOST_REQUIRE(restored.automaticContract);
      BOOST_CHECK_EQUAL(restored.automaticContract->canonicalJson(metadataControl()),
                        model.contract.canonicalJson(metadataControl()));
      BOOST_CHECK_EQUAL(restored.graph.graphMetadataJson, model.inspection.graphMetadataJson);
      BOOST_CHECK_EQUAL(restored.graph.canonicalIdentity.graphDigest,
                        model.inspection.canonicalIdentity.graphDigest);
      BOOST_CHECK(restored.graph.initializerConsumers == model.inspection.initializerConsumers);
      BOOST_CHECK_NO_THROW(validateNativePreparedMetadataVersion(metadata, metadataControl()));
      NativePreparedCanonicalPublication receipt;
      receipt.sourceDataName = metadata.at("source").at("name").get<std::string>();
      receipt.rootDataName = ndn::Name("/metadata-producer/di-model/root").toUri(
        ndn::name::UriFormat::CANONICAL);
      receipt.canonicalManifestJson = "{}";
      receipt.manifestDigest = nativePlanningDigest(receipt.canonicalManifestJson);
      receipt.preparedMetadataJson = nativeCanonicalJson(metadata);
      try {
        receipt.validate();
      }
      catch (const std::exception& error) {
        BOOST_FAIL("valid metadata receipt rejected: " << error.what());
      }
      auto broken = metadata; broken.erase("automatic_onnx");
      receipt.preparedMetadataJson = nativeCanonicalJson(broken);
      BOOST_CHECK_THROW(receipt.validate(), std::invalid_argument);
    }
  }
}

BOOST_AUTO_TEST_CASE(IdentityInventoryVersionAndUnknownFieldsCannotBeSubstituted)
{
  const MetadataSourceFixture sourceFixture(true);
  const auto model = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
  const auto metadata = encodedMetadata(model);
  for (int failure = 0; failure != 37; ++failure) {
    auto broken = metadata;
    auto& files = broken["automatic_onnx"]["externalFiles"];
    const auto other = nativePlanningDigest("unrelated-source");
    if (failure == 0) broken["source"]["digest"] = other;
    if (failure == 1) broken["source"]["bytes"] = 0;
    if (failure == 2) broken["source"]["bytes"] = -1;
    if (failure == 3) broken["source"]["initializer_bytes"] = 1;
    if (failure == 4) broken["source"]["graph_digest"] = other;
    if (failure == 5) broken["source"]["initializer_digest"] = other;
    if (failure == 6) broken["descriptor"]["content_digest"] = other;
    if (failure == 7) broken["automatic_onnx"]["contract"]["facts"]["sourceDigest"] = other;
    if (failure == 8) broken["automatic_onnx"]["contract"]["unknown"] = true;
    if (failure == 9) broken["unknown"] = true;
    if (failure == 10) broken["schema"] = "ndnsf-di-prepared-metadata-v1";
    if (failure == 11) {
      broken["schema"] = "ndnsf-di-prepared-metadata-v1";
      broken.erase("automatic_onnx");
    }
    if (failure == 12) broken["schema"] = "ndnsf-di-prepared-metadata-v99";
    if (failure == 13) files[0]["location"] = "../escape";
    if (failure == 14) files[0]["bytes"] = 0;
    if (failure == 15) files[0]["digest"] = other;
    if (failure == 16) files.push_back(files.at(0));
    if (failure == 17) std::swap(files[0], files[1]);
    if (failure == 18) broken.erase("automatic_onnx");
    if (failure == 19) broken["protectedObjects"] = NativeJson::object();
    auto& graph = broken["sourceGraph"];
    auto& indices = graph["canonicalNodeIndices"];
    if (failure == 20) graph["graphDigest"] = other;
    if (failure == 21) indices.begin().value() = -1;
    if (failure == 22) indices.begin().value() = 0.5;
    if (failure == 23) indices.begin().value() = true;
    if (failure == 24) indices.begin().value() = std::numeric_limits<std::uint64_t>::max();
    if (failure == 25) indices.begin().value() = graph["nodes"].size();
    if (failure == 26) graph["nodes"][0]["id"] = "foreign-node";
    if (failure == 27) graph["modelOutputs"][0]["shape"][1] = 4.5;
    if (failure == 28) graph["modelOutputs"][0]["shape"][0] = false;
    if (failure == 29)
      graph["modelOutputs"][0]["shape"][1] = std::numeric_limits<std::uint64_t>::max();
    if (failure == 30) graph["modelOutputs"][0]["shape"][1] = -1;
    if (failure == 31) graph["modelOutputs"][0]["estimatedBytes"] = -1;
    if (failure == 32) graph["modelOutputs"][0]["estimatedBytes"] = 0.5;
    if (failure == 33) graph["nodes"][0]["ordinal"] = 0.5;
    if (failure == 34) graph["nodes"][0]["ordinal"] = 1;
    if (failure == 35) indices.erase(indices.begin());
    if (failure == 36) graph["modelOutputs"][0]["shape"] = NativeJson::object();
    BOOST_CHECK_THROW(decodeNativePreparedOnnxMetadata(broken, metadataControl()), std::exception);
    BOOST_CHECK_THROW(validateNativePreparedMetadataVersion(broken, metadataControl()),
                      std::exception);
  }
  auto unrelated = metadataModel(model);
  unrelated.canonicalSourceDigest = nativePlanningDigest("other");
  BOOST_CHECK_THROW(encodeNativePreparedOnnxMetadata(unrelated, model.inspection, &model,
                                                    metadataControl()), std::invalid_argument);
  BOOST_CHECK_THROW(encodeNativePreparedOnnxMetadata(
    metadataModel(model), model.inspection, nullptr, metadataControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(MetadataCapsCancellationAndDeadlinesDoNotGrantReadiness)
{
  const MetadataSourceFixture sourceFixture(false);
  const auto model = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
  const auto metadata = encodedMetadata(model);
  auto small = metadataControl(); small.maxAssembledBytes = 1;
  BOOST_CHECK_THROW(decodeNativePreparedOnnxMetadata(metadata, small), std::invalid_argument);
  auto cancelled = metadataControl();
  cancelled.requireActive = [] { throw std::logic_error("owner stopped"); };
  BOOST_CHECK_THROW(decodeNativePreparedOnnxMetadata(metadata, cancelled), std::logic_error);
  auto expired = metadataControl(); expired.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(decodeNativePreparedOnnxMetadata(metadata, expired), std::runtime_error);
  auto absent = metadataControl(); absent.requireActive = {};
  BOOST_CHECK_THROW(decodeNativePreparedOnnxMetadata(metadata, absent), std::invalid_argument);
  auto other = metadataControl(); other.maxAssembledBytes /= 2;
  const auto restored = decodeNativePreparedOnnxMetadata(metadata, other);
  BOOST_CHECK_EQUAL(restored.automaticContract->digest(other), model.contract.digest(metadataControl()));
}

BOOST_AUTO_TEST_CASE(LegacyGraphAndOpaquePublicationMetadataKeepTheirContract)
{
  const MetadataSourceFixture sourceFixture(false);
  const auto automatic = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
  auto model = metadataModel(automatic);
  model.descriptor = fixture::completeModel({"legacy-metadata", model.canonicalSourceDigest,
    nativePlanningDigest("legacy-semantics"), model.canonicalGraphDigest,
    "onnx", "float32", "onnx", "1"});
  const auto graph = inspectNativeOnnxSourceGraph(automatic.opened.source, model.descriptor,
                                                metadataControl());
  const auto encoded = encodeNativePreparedOnnxMetadata(model, graph, nullptr, metadataControl());
  const auto metadata = nativeParseJson(encoded);
  BOOST_CHECK_EQUAL(metadata.at("schema"), "ndnsf-di-prepared-metadata-v1");
  BOOST_CHECK(!metadata.contains("automatic_onnx"));
  const auto restored = decodeNativePreparedOnnxMetadata(metadata, metadataControl());
  BOOST_CHECK(!restored.automaticContract);
  BOOST_CHECK_EQUAL(restored.graph.graphMetadataJson, graph.graphMetadataJson);
  BOOST_CHECK(restored.graph.initializerConsumers == graph.initializerConsumers);
  BOOST_CHECK_NO_THROW(validateNativePreparedMetadataVersion(
    NativeJson{{"schema", "ndnsf-di-prepared-metadata-v1"}, {"fixtureGraphFacts", "retained"}},
    metadataControl()));
}

BOOST_AUTO_TEST_CASE(CompactShapeSeedKeepsScalarZeroAndLiteralNames)
{
  NativeAutomaticShapeSeed seed{nativePlanningDigest("contract"), nativePlanningDigest("binding"),
    nativePlanningDigest("budget"), nativePlanningDigest("envelope"), 16, 0,
    {{"graph/scalar", {}}, {"graph/old,key=0", {2, 0, 128}}}};
  const auto encoded = nativeCanonicalJson(nativeAutomaticShapeSeedToJson(seed));
  const auto restored = nativeAutomaticShapeSeedFromJson(nativeParseJson(encoded));
  BOOST_CHECK(restored.inputShapes == seed.inputShapes);
  BOOST_CHECK_EQUAL(nativeCanonicalJson(nativeAutomaticShapeSeedToJson(restored)), encoded);
  seed.inputShapes.clear();
  BOOST_CHECK(nativeAutomaticShapeSeedFromJson(nativeAutomaticShapeSeedToJson(seed)).inputShapes.empty());
  seed.inputShapes["graph/rank16"] = std::vector<std::int64_t>(16, 1);
  BOOST_CHECK_NO_THROW(nativeAutomaticShapeSeedToJson(seed));
  seed.inputShapes = {{"graph/" + std::string(1024, 'x'), {0, 1LL << 31}}};
  BOOST_CHECK_NO_THROW(nativeAutomaticShapeSeedToJson(seed));
  seed.inputShapes = {{"graph/" + std::string(1025, 'x'), {}}};
  BOOST_CHECK_THROW(nativeAutomaticShapeSeedToJson(seed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CompactShapeSeedRejectsMalformedFieldsAndBoundedRepresentations)
{
  const NativeAutomaticShapeSeed seed{nativePlanningDigest("contract"), nativePlanningDigest("binding"),
    nativePlanningDigest("budget"), nativePlanningDigest("envelope"), 16, 0, {{"graph/x", {2, 3}}}};
  const auto original = nativeAutomaticShapeSeedToJson(seed);
  for (int failure = 0; failure != 26; ++failure) {
    auto value = original;
    if (failure == 0) value["schema"] = "ndnsf-di-automatic-shape-seed-v99";
    if (failure == 1) value.erase("contractDigest");
    if (failure == 2) value["unknown"] = 1;
    if (failure == 3) value["contractDigest"] = 1;
    if (failure == 4) value["budgetDigest"] = "sha256:bad";
    if (failure == 5) value["maxContext"] = 0;
    if (failure == 6) value["maxContext"] = -1;
    if (failure == 7) value["maxContext"] = 1.5;
    if (failure == 8) value["maxContext"] = true;
    if (failure == 9) value["maxNewTokens"] = -1;
    if (failure == 10) value["maxNewTokens"] = 1.0;
    if (failure == 11) value["maxNewTokens"] = false;
    if (failure == 12) value["inputShapes"] = NativeJson::array();
    if (failure == 13) value["inputShapes"]["graph/x"] = 1;
    if (failure == 14) value["inputShapes"]["graph/x"] = {2, 3.5};
    if (failure == 15) value["inputShapes"]["graph/x"] = {true, 3};
    if (failure == 16) value["inputShapes"]["graph/x"] = {-1, 3};
    if (failure == 17)
      value["inputShapes"]["graph/x"] = {std::numeric_limits<std::uint64_t>::max(), 3};
    if (failure == 18) value["inputShapes"]["graph/x"] = std::vector<std::int64_t>(17, 1);
    if (failure >= 19 && failure <= 23) {
      value["inputShapes"] = NativeJson::object();
      const auto name = failure == 19 ? "function/x" : failure == 20 ? "graph/" :
        failure == 21 ? std::string("graph/x\0y", 9) : failure == 22 ?
        "graph/" + std::string(4091, 'x') : "x";
      value["inputShapes"][name] = {2, 3};
    }
    if (failure == 24) value["inputShapes"]["graph/x"] = {0, (1ULL << 31) + 1};
    if (failure == 25) value["inputShapes"]["graph/x"] = {1LL << 31, 1LL << 31, 1LL << 31};
    BOOST_CHECK_THROW(nativeAutomaticShapeSeedFromJson(value), std::exception);
  }
  BOOST_CHECK_THROW(nativeAutomaticShapeSeedToJson(seed, 1), std::invalid_argument);
  BOOST_CHECK_THROW(nativeAutomaticShapeSeedFromJson(original, 1), std::invalid_argument);
  auto oversized = original;
  oversized["contractDigest"] = std::string(NATIVE_AUTOMATIC_SHAPE_SEED_MAX_BYTES, 'x');
  BOOST_CHECK_EXCEPTION(nativeAutomaticShapeSeedFromJson(oversized,
    std::numeric_limits<std::uint64_t>::max()), std::invalid_argument,
    [](const auto& error) { return std::string(error.what()) == "AUTOMATIC_SHAPE_SEED_LIMIT"; });
  auto tooMany = seed; tooMany.inputShapes.clear();
  for (int index = 0; index != 257; ++index)
    tooMany.inputShapes.emplace("graph/value-" + std::to_string(index),
                               std::vector<std::int64_t>{});
  BOOST_CHECK_THROW(nativeAutomaticShapeSeedToJson(tooMany), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ActualTensorForwardShapeSeedReconstructsTheSameEnvelope)
{
  const MetadataSourceFixture sourceFixture(true);
  const auto model = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
  const std::vector<NamedTensor> inputs{{"x", TensorElementType::Float32, {2, 3},
    std::vector<std::uint8_t>(24, 0)}};
  const auto envelope = bindAutomaticOnnxShapes(model.contract, inputs, 0, 16, metadataControl());
  const auto seed = automaticOnnxShapeSeed(envelope);
  const auto wire = nativeCanonicalJson(nativeAutomaticShapeSeedToJson(seed));
  const auto decoded = nativeAutomaticShapeSeedFromJson(nativeParseJson(wire));
  const auto rebound = rebindAutomaticOnnxShapeSeed(model.contract, decoded, metadataControl());
  BOOST_CHECK_EQUAL(rebound.contractDigest, envelope.contractDigest);
  BOOST_CHECK_EQUAL(rebound.bindingDigest, envelope.bindingDigest);
  BOOST_CHECK_EQUAL(rebound.budgetDigest, envelope.budgetDigest);
  BOOST_CHECK_EQUAL(rebound.envelopeDigest, envelope.envelopeDigest);
  BOOST_CHECK(rebound.inputShapes == envelope.inputShapes);
  BOOST_REQUIRE(rebound.tensors.at("graph/y").maxBytes);
  BOOST_CHECK_EQUAL(*rebound.tensors.at("graph/y").maxBytes, 32);
  auto other = metadataControl(); other.maxAssembledBytes /= 2;
  BOOST_CHECK_EQUAL(rebindAutomaticOnnxShapeSeed(model.contract, decoded, other).envelopeDigest,
                    envelope.envelopeDigest);
}

BOOST_AUTO_TEST_CASE(ShapeSeedRejectsTamperingForeignSourcesOverflowAndStoppedOwners)
{
  const MetadataSourceFixture sourceFixture(false);
  const auto model = inspectAutomaticOnnxModel({sourceFixture.path}, metadataControl());
  const std::vector<NamedTensor> inputs{{"x", TensorElementType::Float32, {2, 3},
    std::vector<std::uint8_t>(24, 0)}};
  const auto seed = automaticOnnxShapeSeed(bindAutomaticOnnxShapes(
    model.contract, inputs, 0, 16, metadataControl()));
  for (int failure = 0; failure != 10; ++failure) {
    auto changed = seed;
    const auto other = nativePlanningDigest("unrelated");
    if (failure == 0) changed.contractDigest = other;
    if (failure == 1) changed.bindingDigest = other;
    if (failure == 2) changed.budgetDigest = other;
    if (failure == 3) changed.envelopeDigest = other;
    if (failure == 4) changed.maxContext = 15;
    if (failure == 5) changed.maxNewTokens = 1;
    if (failure == 6) changed.inputShapes["graph/x"] = {3, 3};
    if (failure == 7) changed.inputShapes["graph/x"] = {2};
    if (failure == 8) changed.inputShapes = {{"graph/foreign", {2, 3}}};
    if (failure == 9)
      changed.inputShapes["graph/x"] = {std::numeric_limits<std::int64_t>::max(), 3};
    BOOST_CHECK_THROW(rebindAutomaticOnnxShapeSeed(model.contract, changed, metadataControl()),
                      std::exception);
  }
  const MetadataSourceFixture otherFixture(true);
  const auto foreign = inspectAutomaticOnnxModel({otherFixture.path}, metadataControl());
  BOOST_CHECK_THROW(rebindAutomaticOnnxShapeSeed(foreign.contract, seed, metadataControl()),
                    std::invalid_argument);
  auto stopped = metadataControl(); stopped.requireActive = [] { throw std::logic_error("stopped"); };
  BOOST_CHECK_THROW(rebindAutomaticOnnxShapeSeed(model.contract, seed, stopped), std::logic_error);
  auto expired = metadataControl(); expired.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(rebindAutomaticOnnxShapeSeed(model.contract, seed, expired), std::runtime_error);
  auto small = metadataControl(); small.maxAssembledBytes = 1;
  BOOST_CHECK_THROW(rebindAutomaticOnnxShapeSeed(model.contract, seed, small), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AutomaticRequestBindsActualSeedAndRejectsMutableDto)
{
  const MetadataSourceFixture fixture(false);
  const auto source = inspectAutomaticOnnxModel({fixture.path}, metadataControl());
  const auto& model = source.descriptor;
  const std::vector<NamedTensor> tensors{{"x", TensorElementType::Float32, {2, 3},
    std::vector<std::uint8_t>(24, 0)}};
  const auto seed = automaticOnnxShapeSeed(bindAutomaticOnnxShapes(
    source.contract, tensors, 0, 1, metadataControl()));
  NativeRequestContract contract{"/di/infer", model.adapter.tasks.at(0), model.adapterId,
    model.adapter.descriptorDigest(), nativePlanningDigest("composition"),
    nativePlanningDigest("task")};
  NativeApplicationInput input;
  input.taskName = contract.taskName;
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  input.payload = encodeTensorBundle(tensors); input.options = {'{', '}'};
  const auto encode = [&](const std::optional<NativeAutomaticShapeSeed>& value) {
    return encodeNativeRequestEnvelope(model, input, contract, "/request/automatic", 1,
      10000, std::nullopt, {}, std::nullopt, std::nullopt, {}, value);
  };
  BOOST_CHECK_THROW(encode(std::nullopt), std::invalid_argument);
  const auto request = encode(seed);
  const auto root = parseNativeRequestEnvelope(std::string(request.wire.begin(), request.wire.end()));
  BOOST_REQUIRE(root);
  BOOST_CHECK_EQUAL(root->at("schema").get<std::string>(), "ndnsf-di-request-envelope-v3");
  BOOST_CHECK(root->at("schema_version") == 3);
  BOOST_REQUIRE(request.automaticShapeSeed);
  BOOST_CHECK(root->at("automatic_shape_seed") == nativeAutomaticShapeSeedToJson(seed));
  BOOST_CHECK_NO_THROW(validateNativeAutomaticRequestBinding(request, model, contract,
    "/request/automatic", 1, std::nullopt));
  for (int failure = 0; failure != 9; ++failure) {
    auto changed = request;
    auto changedContract = contract;
    auto changedModel = model;
    auto requestId = std::string("/request/automatic");
    std::uint64_t attempt = 1;
    std::optional<NativeGenerationExecutionContractV1> generation;
    if (failure == 0) changed.automaticShapeSeed->maxContext += 1;
    if (failure == 1) changed.automaticShapeSeed.reset();
    if (failure == 2) changed.requestContractDigest = nativePlanningDigest("wrong");
    if (failure == 3) requestId += "/other";
    if (failure == 4) attempt = 2;
    if (failure == 5) changedContract.serviceName += "/other";
    if (failure == 6) changedModel.contentDigest = nativePlanningDigest("other-model");
    if (failure == 7) changed.modelIntentDigest = nativePlanningDigest("other-intent");
    if (failure == 8) {
      generation.emplace(); generation->enabled = true; generation->maxGeneratedTokens = 9;
    }
    BOOST_CHECK_THROW(validateNativeAutomaticRequestBinding(changed, changedModel,
      changedContract, requestId, attempt, generation), std::invalid_argument);
  }
  NativeProviderOfferV3Config provider;
  provider.provider = "/provider/automatic"; provider.service = contract.serviceName;
  provider.bootEpoch = "boot-1"; provider.signerKeyId = nativePlanningDigest("key");
  provider.acceptedRoles = {"worker"}; provider.backends = {"onnxruntime"};
  provider.devices = {"cpu"};
  unsigned signedOffers = 0;
  provider.signDigest = [&](const auto&) { ++signedOffers; return std::string(128, 'a'); };
  BOOST_REQUIRE(request.discoveryPayload);
  const std::string discovery(request.discoveryPayload->begin(), request.discoveryPayload->end());
  const auto discoveryRoot = parseNativeRequestOffer(discovery);
  BOOST_REQUIRE(discoveryRoot);
  BOOST_CHECK(discoveryRoot->at("schema") == "ndnsf-di-request-discovery-v1");
  BOOST_CHECK(discoveryRoot->at("request_contract_digest") == request.requestContractDigest);
  BOOST_CHECK(!discoveryRoot->contains("input_payload_b64"));
  BOOST_CHECK(!discoveryRoot->contains("automatic_shape_seed"));
  BOOST_CHECK(!parseNativeRequestEnvelope(discovery));
  for (int failure = 0; failure != 10; ++failure) {
    auto changed = *discoveryRoot;
    if (failure == 0) changed["schema"] = "ndnsf-di-request-discovery-v99";
    if (failure == 1) changed["schema_version"] = 1.0;
    if (failure == 2) changed["automatic_contract_digest"] = "invalid";
    if (failure == 3) changed["input_payload_b64"] = "private";
    if (failure == 4) changed["attempt"] = "1";
    if (failure == 5) changed["plan_deadline_ms"] = -1;
    if (failure == 6) changed["task"]["adapter"] = "legacy";
    if (failure == 7) changed.erase("request_contract_digest");
    if (failure == 8) changed["task"]["placement_profile"] = "DI_PLACEMENT_V4";
    if (failure == 9) changed["request_id"] = std::string(4096, 'x');
    BOOST_CHECK_THROW(parseNativeRequestOffer(nativeCanonicalJson(changed)), std::exception);
  }
  const auto offer = issueNativeProviderOfferV3(*request.discoveryPayload, provider, 1000);
  BOOST_REQUIRE(offer); BOOST_CHECK(offer->status); BOOST_CHECK_EQUAL(signedOffers, 1U);
  auto downgraded = *root; downgraded["schema"] = "ndnsf-di-request-envelope-v2";
  const auto wire = nativeCanonicalJson(downgraded);
  const auto rejected = issueNativeProviderOfferV3({wire.begin(), wire.end()}, provider, 1000);
  BOOST_REQUIRE(rejected); BOOST_CHECK(!rejected->status); BOOST_CHECK_EQUAL(signedOffers, 1U);
}

BOOST_AUTO_TEST_CASE(AutomaticRequestVersionsCannotFallBackToLegacy)
{
  const MetadataSourceFixture fixture(false);
  const auto source = inspectAutomaticOnnxModel({fixture.path}, metadataControl());
  const auto seed = automaticOnnxShapeSeed(bindAutomaticOnnxShapes(source.contract,
    {{"x", TensorElementType::Float32, {2, 3}, std::vector<std::uint8_t>(24, 0)}},
    0, 1, metadataControl()));
  const NativeJson valid{{"schema", "ndnsf-di-request-envelope-v3"}, {"schema_version", 3},
    {"task", {{"adapter", "automatic-onnx"}}},
    {"automatic_shape_seed", nativeAutomaticShapeSeedToJson(seed)}};
  BOOST_REQUIRE(parseNativeRequestEnvelope(nativeCanonicalJson(valid)));
  for (int failure = 0; failure != 10; ++failure) {
    auto value = valid;
    if (failure == 0) value["schema"] = "ndnsf-di-request-envelope-v2";
    if (failure == 1) value["schema"] = "ndnsf-di-request-envelope-v99";
    if (failure == 2) value["schema_version"] = 2;
    if (failure == 3) value["schema_version"] = 3.0;
    if (failure == 4) value.erase("automatic_shape_seed");
    if (failure == 5) value["automatic_shape_seed"]["unexpected"] = true;
    if (failure == 6) value["automatic_shape_seed"]["maxContext"] = -1;
    if (failure == 7) value["task"]["adapter"] = "legacy";
    if (failure == 8) value.erase("schema");
    if (failure == 9) value["schema"] = 3;
    const auto wire = nativeCanonicalJson(value);
    BOOST_CHECK_THROW(parseNativeRequestEnvelope(wire), std::invalid_argument);
    BOOST_CHECK_THROW(detail::requiresSelectionGenerationOptions(wire, true), std::invalid_argument);
  }
  for (const auto& wire : {
      std::string(R"({"schema":"legacy","sch\u0065ma":"ndnsf-di-request-envelope-v3"})"),
      std::string(R"({"schema":"ndnsf-di-request-envelope-v99","schema":"legacy"})"),
      std::string(R"({"schema":"ndnsf-di-request-envelope-v3",)"),
      std::string(R"({"schema":"ndnsf-di-request-envelope-v2","automatic_shape_seed":null})")}) {
    BOOST_CHECK_THROW(parseNativeRequestEnvelope(wire), std::invalid_argument);
  }
  BOOST_CHECK(!parseNativeRequestEnvelope("raw legacy bytes"));
  BOOST_CHECK(!parseNativeRequestEnvelope(R"({"schema":"legacy","x":1,"x":2})"));
  BOOST_CHECK(!parseNativeRequestEnvelope(R"({"schema":"legacy","message":"ndnsf-di-request-envelope-v3"})"));
  BOOST_CHECK(!parseNativeRequestEnvelope(std::string((16U << 20) + 1, 'x')));
  BOOST_CHECK_THROW(parseNativeRequestEnvelope("{" + std::string(16U << 20, ' ')),
                    std::invalid_argument);
  const auto deep = std::string("{\"schema\":\"ndnsf-di-request-envelope-v3\",\"x\":") +
    std::string(256, '[') + "0" + std::string(256, ']') + "}";
  BOOST_CHECK_THROW(parseNativeRequestEnvelope(deep), std::invalid_argument);
  BOOST_CHECK_THROW(parseNativeRequestOffer(
    R"({"schema":"ndnsf-di-request-discovery-v1","sch\u0065ma":"legacy"})"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AutomaticRequestChecksBudgetBeforeOmittingOptions)
{
  const MetadataSourceFixture fixture(false);
  const auto source = inspectAutomaticOnnxModel({fixture.path}, metadataControl());
  const auto& model = source.descriptor;
  auto seed = automaticOnnxShapeSeed(bindAutomaticOnnxShapes(source.contract,
    {{"x", TensorElementType::Float32, {2, 3}, std::vector<std::uint8_t>(24, 0)}},
    0, 1, metadataControl()));
  NativeRequestContract contract{"/di/infer", model.adapter.tasks.at(0), model.adapterId,
    model.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  NativeApplicationInput input;
  input.taskName = contract.taskName;
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  input.payload = {1};
  seed.maxNewTokens = 8;
  const auto diagnostic = [&] {
    return encodeNativeRequestEnvelope(model, input, contract, "/request/automatic", 1,
      10000, std::nullopt, {}, std::nullopt, std::nullopt, {}, seed);
  };
  BOOST_CHECK_THROW(diagnostic(), std::invalid_argument);
  contract.generationMode = "TOKEN_STREAMING";
  contract.tokenizerDigest = nativePlanningDigest("tokenizer");
  for (const auto& options : {std::string("{}"), std::string(R"({"maxNewTokens":9})"),
      std::string(R"({"maxNewTokens":8.0})"), std::string(R"({"maxNewTokens":-1})"),
      std::string(R"({"maxNewTokens":0})")}) {
    input.options.assign(options.begin(), options.end());
    BOOST_CHECK_THROW(detail::encodeNativeRequestEnvelopeForRuntime(model, input, contract,
      "/request/automatic", 1, 10000, std::nullopt, {}, std::nullopt, std::nullopt,
      {}, "SELECTION_GENERATION_V1", {}, seed), std::invalid_argument);
  }
  // This checks transport syntax only: the unchanged source contract will
  // reject the altered budget on semantic rebind at the adapter boundary.
  const std::string options = R"({"maxNewTokens":8})";
  input.options.assign(options.begin(), options.end());
  const auto encoded = detail::encodeNativeRequestEnvelopeForRuntime(model, input, contract,
    "/request/automatic", 1, 10000, std::nullopt, {}, std::nullopt, std::nullopt,
    {}, "SELECTION_GENERATION_V1", {}, seed);
  const auto root = parseNativeRequestEnvelope(std::string(encoded.wire.begin(), encoded.wire.end()));
  BOOST_REQUIRE(root);
  BOOST_CHECK(root->at("options_payload_b64") == "");
  BOOST_CHECK(detail::requiresSelectionGenerationOptions(
    std::string(encoded.wire.begin(), encoded.wire.end()), true));
  const auto inlineRequest = diagnostic();
  const auto inlineRoot = parseNativeRequestEnvelope(
    std::string(inlineRequest.wire.begin(), inlineRequest.wire.end()));
  BOOST_REQUIRE(inlineRoot);
  BOOST_CHECK(inlineRoot->at("automatic_shape_seed") == root->at("automatic_shape_seed"));
  BOOST_CHECK(!inlineRoot->at("options_payload_b64").get<std::string>().empty());
  NativeGenerationRecovery recovery;
  recovery.generationId = std::string(32, 'a');
  recovery.originalRequestId = "/request/automatic";
  recovery.recoveryRequestId = "/request/automatic/recovery";
  recovery.originalInputManifestDigest = encoded.inputManifestDigest;
  recovery.priorPlanDigest = nativePlanningDigest("prior-plan");
  recovery.failedProvider = "/provider/failed"; recovery.committedTokenIds = {1, 2};
  const auto recovered = detail::encodeNativeRequestEnvelopeForRuntime(model, input, contract,
    recovery.recoveryRequestId, 2, 10000, recovery, {}, std::nullopt, std::nullopt,
    {}, "SELECTION_GENERATION_V1", {}, seed);
  const auto recoveredRoot = parseNativeRequestEnvelope(
    std::string(recovered.wire.begin(), recovered.wire.end()));
  BOOST_REQUIRE(recoveredRoot);
  BOOST_CHECK(recoveredRoot->at("automatic_shape_seed") == root->at("automatic_shape_seed"));
  BOOST_CHECK(recovered.requestContractDigest != encoded.requestContractDigest);
}

BOOST_AUTO_TEST_CASE(AutomaticDiscoveryOmitsLargeShapesAndFallsBackWithoutPartialV4Scope)
{
  const MetadataSourceFixture fixture(false);
  const auto source = inspectAutomaticOnnxModel({fixture.path}, metadataControl());
  const auto& model = source.descriptor;
  auto seed = automaticOnnxShapeSeed(bindAutomaticOnnxShapes(source.contract,
    {{"x", TensorElementType::Float32, {2, 3}, std::vector<std::uint8_t>(24, 0)}},
    0, 1, metadataControl()));
  // Structural pressure fixture only, not a source-valid input-set proof.
  // Discovery must not copy these bounded shape names into the ACK view.
  for (int index = 0; index != 60; ++index)
    seed.inputShapes.emplace("graph/" + std::string(80, 'x') + std::to_string(index),
                             std::vector<std::int64_t>{2, 3});
  NativeRequestContract contract{"/di/infer", model.adapter.tasks.at(0), model.adapterId,
    model.adapter.descriptorDigest(), nativePlanningDigest("composition"), nativePlanningDigest("task")};
  NativeApplicationInput input;
  input.taskName = contract.taskName; input.payload = {1};
  input.inputSchemaDigest = model.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  const auto plain = encodeNativeRequestEnvelope(model, input, contract, "/request/discovery", 1,
    10000, std::nullopt, {}, std::nullopt, std::nullopt, {}, seed);
  BOOST_REQUIRE(plain.discoveryPayload);
  BOOST_CHECK_GT(plain.wire.size(), 4096);
  BOOST_CHECK_LE(plain.discoveryPayload->size(), 4096);
  const auto summary = parseNativeRequestOffer(std::string(
    plain.discoveryPayload->begin(), plain.discoveryPayload->end()));
  BOOST_REQUIRE(summary);
  BOOST_CHECK(!summary->contains("input_payload_b64"));
  BOOST_CHECK(!summary->contains("automatic_shape_seed"));
  NativeCandidateArtifactInventoryV4 inventory;
  for (int index = 0; index != 24; ++index) {
    inventory.identities.push_back({{"artifact_digest", nativePlanningDigest(std::to_string(index))},
      {"model_manifest_digest", nativePlanningDigest("manifest")},
      {"graph_digest", model.graphDigest}, {"role", "role-" + std::to_string(index)},
      {"rank", 0}, {"backend", "onnxruntime"}, {"protection_epoch", "epoch"}});
  }
  std::sort(inventory.identities.begin(), inventory.identities.end(), [](const auto& a, const auto& b) {
    return nativeCanonicalJson(a) < nativeCanonicalJson(b);
  });
  inventory.digest = nativePlanningDigest(nativeCanonicalJson(NativeJson(inventory.identities)));
  auto statistics = makeNativeWorkStatistics();
  const auto encoded = detail::encodeNativeRequestEnvelopeForRuntime(model, input, contract,
    "/request/discovery", 1, 10000, std::nullopt, {"/provider/a", "/provider/b"},
    inventory, NativePlacementProbeAllowlistV4{}, "/requester", "INLINE", statistics, seed);
  BOOST_REQUIRE(encoded.discoveryPayload);
  const auto root = parseNativeRequestEnvelope(std::string(encoded.wire.begin(), encoded.wire.end()));
  BOOST_REQUIRE(root);
  BOOST_CHECK(root->at("task").at("placement_profile") == "DI_PLACEMENT_V3");
  for (const auto* key : {"candidate_providers", "candidate_set_digest", "candidate_artifacts",
                          "candidate_artifact_set_digest", "rtt_pairs", "goodput_edges",
                          "probe_scope_digest", "requester_identity"})
    BOOST_CHECK(!root->contains(key));
  BOOST_CHECK(encoded.candidateArtifacts.empty()); BOOST_CHECK(encoded.probeScopeDigest.empty());
  BOOST_CHECK_LE(encoded.discoveryPayload->size(), 4096);
  BOOST_CHECK_EQUAL(statistics->snapshot().counts.at("request.automaticDiscoveryV4BudgetFallbacks"), 1);
  auto oversized = contract; oversized.serviceName = "/" + std::string(4096, 's');
  BOOST_CHECK_THROW(encodeNativeRequestEnvelope(model, input, oversized, "/request/discovery", 1,
    10000, std::nullopt, {}, std::nullopt, std::nullopt, {}, seed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(MaterialManifestVersionsKeepIndependentCanonicalBytes)
{
  // Structural reference-index golden only: an empty contract is not an
  // executable source proof. Full source admission is exercised separately.
  const std::string v1 =
    R"({"graphDigest":"sha256:2222222222222222222222222222222222222222222222222222222222222222")"
    R"(,"initializerDigest":"sha256:33333333333333333333333333333333333333333333333333333333333)"
    R"(33333","references":[{"bytes":1,"dependencies":[],"digest":"sha256:444444444444444444444)"
    R"(4444444444444444444444444444444444444444444","kind":"graph-template","logicalName":"__te)"
    R"(mplate__","nodeIndex":0,"payloadId":"t","sharedDigest":""}],"schema":"ndnsf-di-canonical)"
    R"(-material-manifest-v1","sourceDigest":"sha256:111111111111111111111111111111111111111111)"
    R"(1111111111111111111111","templatePayloadId":"t"})";
  const std::string v2 =
    R"({"automaticSource":{"contract":{},"externalFiles":[]},"graphDigest":"sha256:222222222222)"
    R"(2222222222222222222222222222222222222222222222222222","initializerDigest":"sha256:333333)"
    R"(3333333333333333333333333333333333333333333333333333333333","references":[{"bytes":1,"de)"
    R"(pendencies":[],"digest":"sha256:44444444444444444444444444444444444444444444444444444444)"
    R"(44444444","kind":"graph-template","logicalName":"__template__","nodeIndex":0,"payloadId")"
    R"(:"t","sharedDigest":""}],"schema":"ndnsf-di-canonical-material-manifest-v2","sourceDiges)"
    R"(t":"sha256:1111111111111111111111111111111111111111111111111111111111111111","templatePa)"
    R"(yloadId":"t"})";

  NativeCanonicalSource::MaterialManifest manifest;
  manifest.payloadsComplete = false;
  manifest.sourceDigest = "sha256:" + std::string(64, '1');
  manifest.graphDigest = "sha256:" + std::string(64, '2');
  manifest.initializerDigest = "sha256:" + std::string(64, '3');
  manifest.templatePayloadId = "t";
  NativeCanonicalSource::MaterialReference reference;
  reference.payloadId = "t"; reference.kind = "graph-template";
  reference.logicalName = "__template__"; reference.bytes = 1;
  reference.digest = "sha256:" + std::string(64, '4');
  manifest.references = {reference};
  BOOST_CHECK_EQUAL(manifest.canonicalJson(), v1);
  BOOST_CHECK_EQUAL(nativePlanningDigest(v1), "sha256:a51e739238da759c71162ed37b75f71d2f0ca54df91364ec8c66a3d433911dbd");
  manifest.manifestDigest = nativePlanningDigest(v1);
  BOOST_CHECK_NO_THROW(manifest.validate());
  manifest.schema = "ndnsf-di-canonical-material-manifest-v2";
  manifest.automaticSourceJson = R"({"contract":{},"externalFiles":[]})";
  BOOST_CHECK_EQUAL(manifest.canonicalJson(), v2);
  BOOST_CHECK_EQUAL(nativePlanningDigest(v2), "sha256:4674b695caa4c49e930d75b2a01b591496aaaebf9d0dc40e5d0e94392c7c41ac");
  manifest.manifestDigest = nativePlanningDigest(v2);
  BOOST_CHECK_NO_THROW(manifest.validate());
  const auto parsed = parseNativeCanonicalMaterialManifest({v2.begin(), v2.end()});
  BOOST_CHECK_EQUAL(parsed->canonicalJson(), v2);
  BOOST_CHECK_EQUAL(parsed->automaticSourceJson, manifest.automaticSourceJson);
  for (unsigned failure = 0; failure < 6; ++failure) {
    auto changed = nativeParseJson(v2);
    if (failure == 0) changed["schema"] = "ndnsf-di-canonical-material-manifest-v1";
    if (failure == 1) changed["schema"] = "ndnsf-di-canonical-material-manifest-v3";
    if (failure == 2) changed.erase("automaticSource");
    if (failure == 3) changed["automaticSource"]["extra"] = 1;
    if (failure == 4) changed["automaticSource"]["externalFiles"] = NativeJson::object();
    if (failure == 5) changed["extra"] = 1;
    const auto wire = nativeCanonicalJson(changed);
    BOOST_TEST_CONTEXT("material mutation=" << failure) {
      BOOST_CHECK_THROW(parseNativeCanonicalMaterialManifest({wire.begin(), wire.end()}),
                        std::exception);
    }
  }
  auto oversized = manifest;
  oversized.automaticSourceJson = std::string(NativeCanonicalMaterialPublicationMaxBytes + 1, ' ');
  BOOST_CHECK_THROW(oversized.canonicalJson(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SharedAutomaticSourceDecoderPreservesPhysicalInventoryBinding)
{
  for (const bool external : {false, true}) {
    const MetadataSourceFixture file(external);
    const auto source = inspectAutomaticOnnxModel({file.path}, metadataControl());
    const auto model = metadataModel(source);
    const auto automatic = encodedMetadata(source).at("automatic_onnx");
    const auto decoded = decodeNativeAutomaticOnnxSourceMetadata(automatic,
      model.canonicalSourceDigest, model.canonicalInitializerBytes,
      model.canonicalInitializerObjectDigest, metadataControl());
    BOOST_REQUIRE(decoded);
    BOOST_CHECK_EQUAL(decoded->digest(metadataControl()), source.contract.digest(metadataControl()));
    BOOST_CHECK_EQUAL(decoded->sourceDigest, source.opened.sourceDigest);
    BOOST_CHECK(decoded->sourceDigest != model.canonicalSourceDigest);
    BOOST_CHECK_THROW(decodeNativeAutomaticOnnxSourceMetadata(automatic, decoded->sourceDigest,
      model.canonicalInitializerBytes, model.canonicalInitializerObjectDigest, metadataControl()),
      std::exception);
    const auto binding = sourceBinding(source);
    const auto wire = binding.canonicalJson(metadataControl());
    const auto value = nativeParseJson(wire);
    BOOST_REQUIRE_EQUAL(value.size(), 5U);
    BOOST_CHECK_EQUAL(value.at("schema"), "ndnsf-di-automatic-source-binding-v1");
    BOOST_CHECK_EQUAL(value.at("modelDigest"), model.canonicalSourceDigest);
    BOOST_CHECK_EQUAL(value.at("initializerBytes"), model.canonicalInitializerBytes);
    BOOST_CHECK_EQUAL(value.at("initializerObjectDigest"), model.canonicalInitializerObjectDigest);
    BOOST_CHECK(value.at("automaticSource") == automatic);
    BOOST_CHECK_EQUAL(AutomaticOnnxSourceBinding::fromCanonicalJson(wire, metadataControl())
      .canonicalJson(metadataControl()), wire);
    auto changed = value; changed["requestId"] = "/must-not-be-retained";
    BOOST_CHECK_THROW(AutomaticOnnxSourceBinding::fromCanonicalJson(
      nativeCanonicalJson(changed), metadataControl()), std::exception);
    changed = value; changed["initializerBytes"] = -1;
    BOOST_CHECK_THROW(AutomaticOnnxSourceBinding::fromCanonicalJson(
      nativeCanonicalJson(changed), metadataControl()), std::exception);
    BOOST_CHECK_THROW(AutomaticOnnxSourceBinding::fromCanonicalJson(value.dump(2), metadataControl()),
                      std::exception);
  }
}

BOOST_AUTO_TEST_CASE(SharedAutomaticSourceDecoderRetainsStrictInventoryFailures)
{
  const MetadataSourceFixture file(true);
  const auto source = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto model = metadataModel(source);
  const auto automatic = encodedMetadata(source).at("automatic_onnx");
  for (unsigned failure = 0; failure < 9; ++failure) {
    auto changed = automatic;
    if (failure == 0) changed["extra"] = 1;
    if (failure == 1) changed["externalFiles"][0]["location"] = "../escape";
    if (failure == 2) changed["externalFiles"][0]["bytes"] = 0;
    if (failure == 3) changed["externalFiles"][0]["bytes"] = -1;
    if (failure == 4) changed["externalFiles"][0]["digest"] = "unknown";
    if (failure == 5) changed["externalFiles"][1] = changed["externalFiles"][0];
    if (failure == 6) std::swap(changed["externalFiles"][0], changed["externalFiles"][1]);
    if (failure == 7) changed["externalFiles"] = NativeJson::array();
    if (failure == 8) changed["externalFiles"][0]["bytes"] =
      std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST_CONTEXT("inventory mutation=" << failure) {
      BOOST_CHECK_THROW(decodeNativeAutomaticOnnxSourceMetadata(changed,
        model.canonicalSourceDigest, model.canonicalInitializerBytes,
        model.canonicalInitializerObjectDigest, metadataControl()), std::exception);
    }
  }
  BOOST_CHECK_THROW(decodeNativeAutomaticOnnxSourceMetadata(automatic, model.canonicalSourceDigest,
    model.canonicalInitializerBytes, "", metadataControl()), std::exception);
  auto stopped = metadataControl(); stopped.requireActive = [] { throw std::runtime_error("stopped"); };
  BOOST_CHECK_THROW(decodeNativeAutomaticOnnxSourceMetadata(automatic, model.canonicalSourceDigest,
    model.canonicalInitializerBytes, model.canonicalInitializerObjectDigest, stopped),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(AutomaticMaterialCatalogRetainsFactsOnFreshAndRestoredIndexes)
{
  const MetadataSourceFixture file(true);
  const auto automatic = inspectAutomaticOnnxModel({file.path}, metadataControl());
  auto entry = sourceCatalogEntry(automatic);
  const NativeCanonicalPreparationCatalog catalog({entry}, metadataControl());
  const auto fresh = catalog.sourceRefFor(entry.model.descriptor);
  BOOST_REQUIRE(fresh->materialManifest);
  BOOST_CHECK_EQUAL(fresh->materialManifest->schema, "ndnsf-di-canonical-material-manifest-v2");
  BOOST_CHECK_EQUAL(fresh->materialManifest->automaticSourceJson,
                    nativeCanonicalJson(encodedMetadata(automatic).at("automatic_onnx")));
  const auto wire = fresh->materialManifest->canonicalJson();
  entry.source = *fresh;
  entry.source.modelBytes.clear();
  entry.source.initializerBytes.reset();
  entry.source.initializerRangeSource.reset();
  entry.source.materialManifest = parseNativeCanonicalMaterialManifest({wire.begin(), wire.end()});
  const NativeCanonicalPreparationCatalog restored({entry}, metadataControl());
  BOOST_CHECK_EQUAL(restored.sourceRefFor(entry.model.descriptor)->materialManifest->canonicalJson(),
                    wire);
  auto wrong = entry;
  auto poisoned = std::make_shared<NativeCanonicalSource::MaterialManifest>(
    *wrong.source.materialManifest);
  poisoned->automaticSourceJson.clear();
  poisoned->schema = "ndnsf-di-canonical-material-manifest-v1";
  poisoned->manifestDigest = nativePlanningDigest(poisoned->canonicalJson());
  wrong.source.materialManifest = std::move(poisoned);
  BOOST_CHECK_THROW(NativeCanonicalPreparationCatalog({wrong}, metadataControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourceAdmissionUsesCurrentSeedAndSourceBoundaryFacts)
{
  const MetadataSourceFixture file(false);
  const auto source = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto binding = sourceBinding(source);
  auto projection = sourceProjection(source);
  BOOST_REQUIRE_NO_THROW(validateNativeAutomaticSelectionBinding(projection));
  const auto admitted = admitAutomaticOnnxProjection(binding, projection, metadataControl());
  BOOST_REQUIRE(admitted.tensors.at("graph/y").maxBytes);
  BOOST_CHECK_EQUAL(*admitted.tensors.at("graph/y").maxBytes, 32U);
  const auto other = sourceProjection(source, 1);
  BOOST_CHECK_EQUAL(*admitAutomaticOnnxProjection(binding, other,
    metadataControl()).tensors.at("graph/y").maxBytes, 16U);
  for (unsigned failure = 0; failure < 9; ++failure) {
    auto changed = projection;
    const auto foreign = nativePlanningDigest("foreign-binding");
    if (failure == 0) changed.automaticShapeSeed->bindingDigest = foreign;
    if (failure == 1) changed.automaticShapeSeed->budgetDigest = foreign;
    if (failure == 2) changed.automaticShapeSeed->envelopeDigest = foreign;
    if (failure == 3) changed.assembly.expectedInputs[0].name = "missing";
    if (failure == 4) changed.assembly.expectedInputs[0].dtype = "float16";
    if (failure == 5) changed.assembly.expectedInputs[0].shape.clear();
    if (failure == 6) changed.assembly.expectedInputs[0].shape[0] = std::int64_t(2);
    if (failure == 7) changed.assembly.expectedInputs[0].shape[1] = std::int64_t(4);
    if (failure == 8) changed.assembly.expectedOutputs[0].name = "missing";
    changed.assembly.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(changed.assembly));
    changed.selectedRole = changed.assembly;
    BOOST_REQUIRE_NO_THROW(validateNativeAutomaticSelectionBinding(changed));
    BOOST_TEST_CONTEXT("admission mutation=" << failure) {
      BOOST_CHECK_THROW(admitAutomaticOnnxProjection(binding, changed, metadataControl()),
                        std::exception);
    }
  }
}

BOOST_AUTO_TEST_CASE(CachedTemplateChecksCurrentSourceAndCoreWorkAuthorityWithoutPlaintext)
{
  using namespace ndn_service_framework;
  using namespace ndn_service_framework::test;
  const MetadataSourceFixture file(false);
  const auto source = inspectAutomaticOnnxModel({file.path}, metadataControl());
  auto projection = sourceProjection(source);
  auto spec = sourceRunnerSpec(file, source, projection);
  spec.path.clear();
  ndn::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/source-provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/source-authority"));
  IsolatedTestTrust trust{providerCert, authorityCert};
  LocalServiceProvider provider(face, ndn::Name("/source"), providerCert,
                                authorityCert, trust.schemaPath());
  bool current = true;
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = projection.assembly.selectedRole;
  assignment.service = ndn::Name("/service/model");
  assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  assignment.workCurrent = [&] { return current; };
  ServiceProvider::CollaborationContext context(provider, ndn::Name("/source-requester"),
    ndn::Name(projection.requestId), RequestMessage{}, std::move(assignment));
  NativeCanonicalOnnxAssemblerOptions options;
  BOOST_REQUIRE(context.hasCurrentWorkAuthority());
  BOOST_CHECK_NO_THROW(validateNativeAutomaticCachedRunnerSource(context, projection, spec, options));
  const auto other = sourceProjection(source, 1);
  BOOST_CHECK_NO_THROW(validateNativeAutomaticCachedRunnerSource(context, other, spec, options));
  auto invalid = projection;
  invalid.automaticShapeSeed->bindingDigest = nativePlanningDigest("foreign-current-binding");
  BOOST_CHECK_THROW(validateNativeAutomaticCachedRunnerSource(context, invalid, spec, options),
                    std::invalid_argument);
  BOOST_CHECK(spec.path.empty());
  current = false;
  BOOST_CHECK_THROW(validateNativeAutomaticCachedRunnerSource(context, projection, spec, options),
                    std::runtime_error);
  current = true;
  options.shouldCancel = [] { return true; };
  BOOST_CHECK_THROW(validateNativeAutomaticCachedRunnerSource(context, projection, spec, options),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(FrozenEngineExecutesRenamedEquivalentGraphsWithoutShapeAnnotations)
{
  std::set<std::string> sourceIdentities;
  std::set<std::string> contractIdentities;
  for (const auto seed : {0U, 0x233A91U, 0x233B72U}) {
    BOOST_TEST_CONTEXT("transformation seed=" << seed) {
      const MetadataSourceFixture file(false, true, seed);
      onnx::ModelProto serialized;
      std::ifstream encoded(file.path, std::ios::binary);
      BOOST_REQUIRE(serialized.ParseFromIstream(&encoded));
      BOOST_CHECK_EQUAL(serialized.graph().value_info_size(), seed == 0 ? 1 : 0);
      BOOST_CHECK_EQUAL(serialized.metadata_props_size(), 0);
      if (seed != 0) {
        BOOST_CHECK_EQUAL(serialized.graph().input(0).type().tensor_type()
                            .shape().dim_size(), 2);
        BOOST_CHECK(!serialized.graph().input(0).type().tensor_type()
                       .shape().dim(0).has_dim_param());
        BOOST_CHECK(!serialized.graph().output(0).type().tensor_type()
                       .shape().dim(0).has_dim_param());
      }
      const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
      sourceIdentities.insert(model.opened.modelDigest);
      contractIdentities.insert(model.contract.digest(metadataControl()));
      BOOST_REQUIRE_EQUAL(model.contract.modelInputs.size(), 1U);
      BOOST_REQUIRE_EQUAL(model.contract.modelOutputs.size(), 1U);
      BOOST_CHECK_EQUAL(model.contract.modelInputs.front(), "graph/" + file.inputName);
      BOOST_CHECK_EQUAL(model.contract.modelOutputs.front(), "graph/" + file.outputName);
      for (const std::uint64_t batch : {1U, 2U}) {
        const auto inputs = sourceInputs(batch, file.inputName);
        const auto tensors = decodeTensorBundle(inputs.at("input").payload);
        const auto envelope = bindAutomaticOnnxShapes(
          model.contract, tensors, 0, 16, metadataControl());
        for (const auto& port : {std::make_pair(file.inputName, 3U),
                                std::make_pair(file.outputName, 4U)}) {
          const auto& bounds = envelope.tensors.at("graph/" + port.first);
          BOOST_REQUIRE(bounds.maximumDimensions);
          BOOST_REQUIRE_EQUAL(bounds.maximumDimensions->size(), 2U);
          BOOST_REQUIRE(bounds.maximumDimensions->at(0));
          BOOST_REQUIRE(bounds.maximumDimensions->at(1));
          BOOST_CHECK_EQUAL(*bounds.maximumDimensions->at(0), batch);
          BOOST_CHECK_EQUAL(*bounds.maximumDimensions->at(1), port.second);
          BOOST_REQUIRE(bounds.maxBytes);
          BOOST_CHECK_EQUAL(*bounds.maxBytes, batch * port.second * sizeof(float));
        }
        const auto projection = sourceProjection(model, batch);
        auto spec = sourceRunnerSpec(file, model, projection);
        OnnxRuntimeModelRunner runner(spec);
        NativeRequestControl control;
        control.requestId = projection.requestId; control.attempt = projection.attempt;
        control.deadline = metadataControl().deadline;
        RoleExecutionContext context;
        context.role = spec.role;
        context.requestId = context.sessionId = projection.requestId;
        context.attemptEpoch = projection.attempt;
        context.automaticBinding = runner.validateAutomaticRequestBinding(projection, control);
        context.inputsByScope = inputs;
        BOOST_REQUIRE(context.automaticBinding);
        checkSourceResult(runner.run(context), batch, file.outputName);
      }
    }
  }
  BOOST_CHECK_EQUAL(sourceIdentities.size(), 3U);
  BOOST_CHECK_EQUAL(contractIdentities.size(), 3U);
}

BOOST_AUTO_TEST_CASE(ActualCpuRunnerAdmitsCurrentSourceBeforeNamedForwardExecution)
{
  const MetadataSourceFixture file(true);
  const auto source = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto projection = sourceProjection(source);
  auto spec = sourceRunnerSpec(file, source, projection);
  OnnxRuntimeModelRunner runner(spec);
  NativeRequestControl control;
  control.requestId = projection.requestId; control.attempt = projection.attempt;
  control.deadline = metadataControl().deadline;
  const auto binding = runner.validateAutomaticRequestBinding(projection, control);
  BOOST_REQUIRE(binding);
  const auto different = sourceProjection(source, 1);
  BOOST_CHECK_NO_THROW(runner.validateAutomaticRequestBinding(different, control));
  auto invalid = projection;
  invalid.automaticShapeSeed->envelopeDigest = nativePlanningDigest("wrong-current-envelope");
  BOOST_CHECK_THROW(runner.validateAutomaticRequestBinding(invalid, control), std::exception);
  RoleExecutionContext context;
  context.role = spec.role; context.requestId = context.sessionId = projection.requestId;
  context.attemptEpoch = 1;
  context.automaticBinding = binding;
  const float input[6]{1, 2, 3, 4, 5, 6};
  std::vector<std::uint8_t> bytes(sizeof(input));
  std::memcpy(bytes.data(), input, bytes.size());
  context.inputsByScope.emplace("input", makeEncodedTensorBundle("input",
    {{"x", TensorElementType::Float32, {2, 3}, std::move(bytes)}}));
  const auto outputs = runner.run(context);
  BOOST_REQUIRE_EQUAL(outputs.size(), 1U);
  const auto tensors = decodeTensorBundle(outputs.begin()->second.payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors[0].name, "y");
  BOOST_CHECK(tensors[0].shape == std::vector<std::int64_t>({2, 4}));
  BOOST_REQUIRE_EQUAL(tensors[0].payload.size(), 8 * sizeof(float));
  float actual[8]; std::memcpy(actual, tensors[0].payload.data(), sizeof(actual));
  for (unsigned index = 0; index < 8; ++index)
    BOOST_CHECK_EQUAL(actual[index], index < 4 ? 7.0F : 16.0F);
  auto missing = spec; missing.metadata.erase("automaticSourceBinding");
  OnnxRuntimeModelRunner namingOnly(missing);
  BOOST_CHECK_THROW(namingOnly.validateAutomaticRequestBinding(projection, control),
                    std::invalid_argument);
  control.cancelled = [] { return true; };
  BOOST_CHECK_THROW(runner.validateAutomaticRequestBinding(projection, control), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RequestBindingFencesActualInputsAndSharesOnlyImmutableSession)
{
  const MetadataSourceFixture file(false);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  auto first = sourceProjection(model, 2);
  auto second = sourceProjection(model, 1);
  second.requestId = second.dataflow.requestId = "/source-request/second";
  auto cache = std::make_shared<OnnxRuntimeSessionCache>(OnnxRuntimeSessionCache::Config{});
  const auto spec = sourceRunnerSpec(file, model, first);
  OnnxRuntimeModelRunner runner(spec, cache);
  OnnxRuntimeModelRunner other(spec, cache);
  NativeRequestControl control;
  control.requestId = first.requestId; control.attempt = first.attempt;
  control.deadline = metadataControl().deadline;
  const auto firstBinding = runner.validateAutomaticRequestBinding(first, control);
  control.requestId = second.requestId;
  const auto secondBinding = other.validateAutomaticRequestBinding(second, control);
  BOOST_REQUIRE(firstBinding && secondBinding);
  BOOST_CHECK(firstBinding != secondBinding);
  BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
  BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
  RoleExecutionContext context;
  context.requestId = context.sessionId = first.requestId;
  context.attemptEpoch = first.attempt; context.role = first.executionRole.roleId;
  context.automaticBinding = firstBinding; context.inputsByScope = sourceInputs(2);
  checkSourceResult(runner.run(context), 2);
  context.requestId = context.sessionId = second.requestId;
  context.inputsByScope = sourceInputs(1);
  BOOST_CHECK_THROW(other.run(context), std::invalid_argument);
  context.automaticBinding = secondBinding;
  checkSourceResult(other.run(context), 1);
  context.inputsByScope = sourceInputs(2);
  BOOST_CHECK_THROW(other.run(context), std::invalid_argument);
  context.inputsByScope = sourceInputs(1);
  context.inferenceEpoch = 1;
  BOOST_CHECK_THROW(other.run(context), std::invalid_argument);
  context.inferenceEpoch = 0;
  context.automaticBinding.reset();
  BOOST_CHECK_THROW(other.run(context), std::invalid_argument);
  context.automaticBinding = secondBinding;
  context.executionGuard = [] { throw std::runtime_error("current worker cancelled"); };
  BOOST_CHECK_THROW(other.run(context), std::runtime_error);
  context.executionGuard = {};
  // A separately loaded source owner cannot consume another runner's binding.
  OnnxRuntimeModelRunner foreign(spec);
  BOOST_CHECK_THROW(foreign.run(context), std::invalid_argument);
  checkSourceResult(other.run(context), 1);
  const std::vector<AutomaticOnnxTensorObservation> wrongOutput{
    {"y", TensorElementType::Float32, {2, 4}, 32}};
  BOOST_CHECK_THROW(secondBinding->validateOutputs(wrongOutput, context), std::invalid_argument);
  BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
}

BOOST_AUTO_TEST_CASE(AdmittedRequestDeadlineStillFencesActualExecution)
{
  const MetadataSourceFixture file(false);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto projection = sourceProjection(model);
  auto runner = std::make_shared<OnnxRuntimeModelRunner>(sourceRunnerSpec(file, model, projection));
  auto io = std::make_shared<SourceWorkerIo>();
  ProviderRoleWorker worker(1, 1, 4, std::chrono::seconds(2), 4);
  NativeRequestControl control;
  control.requestId = projection.requestId; control.attempt = projection.attempt;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  const auto binding = runner->validateAutomaticRequestBinding(projection, control);
  BOOST_REQUIRE(binding);
  RoleSpec role{projection.executionRole.roleId, {}, {}};
  role.requestId = projection.requestId; role.attemptEpoch = projection.attempt;
  role.automaticBinding = binding;
  const auto first = worker.executeAsync(projection.requestId, role, io, runner,
                                          sourceInputs(2)).get();
  BOOST_CHECK(!first.exactForwardCacheHit);
  BOOST_CHECK(first.exactForwardCacheKey.empty());
  checkSourceResult(first.outputsByScope, 2);
  std::this_thread::sleep_until(control.deadline);
  BOOST_CHECK_THROW(worker.executeAsync(projection.requestId, role, io, runner,
    sourceInputs(2)).get(), std::runtime_error);
  RoleExecutionContext context;
  context.requestId = context.sessionId = projection.requestId;
  context.attemptEpoch = projection.attempt; context.role = projection.executionRole.roleId;
  context.automaticBinding = binding; context.inputsByScope = sourceInputs(2);
  BOOST_CHECK_THROW(runner->run(context), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ActualBoundedPreparationCarriesBindingAndKeepsLegacyCallbackResults)
{
  const MetadataSourceFixture file(false);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto projection = sourceProjection(model);
  auto runner = std::make_shared<OnnxRuntimeModelRunner>(sourceRunnerSpec(file, model, projection));
  NativeRequestControl control;
  control.requestId = projection.requestId; control.attempt = projection.attempt;
  control.deadline = metadataControl().deadline;
  auto io = std::make_shared<SourceWorkerIo>();
  std::atomic<unsigned> preparations{0};
  bool called = false;
  ProviderRoleWorker worker(1, 1, 4, std::chrono::seconds(2), 4);
  const auto prepare = [&] {
    ++preparations;
    return NativePreparedRunner{runner, runner->validateAutomaticRequestBinding(projection, control)};
  };
  auto prepared = worker.prepareRunnerAsync(prepare).get();
  BOOST_REQUIRE(prepared.runner && prepared.automaticBinding);
  RoleSpec role{projection.executionRole.roleId, {}, {}};
  role.requestId = projection.requestId; role.attemptEpoch = projection.attempt;
  role.automaticBinding = prepared.automaticBinding;
  // The cached-runner branch has no preparation callback; RoleSpec owns the binding.
  const auto cached = worker.executeAsync(projection.requestId, role, io, prepared.runner,
                                           sourceInputs(2)).get();
  checkSourceResult(cached.outputsByScope, 2);
  BOOST_CHECK(!cached.exactForwardCacheHit);
  BOOST_CHECK(cached.exactForwardCacheKey.empty());
  role.automaticBinding.reset();
  const auto cold = worker.executePreparedAsync(projection.requestId, role, io, prepare,
                                                 sourceInputs(2)).get();
  checkSourceResult(cold.outputsByScope, 2);
  BOOST_CHECK(!cold.exactForwardCacheHit);
  BOOST_CHECK(cold.exactForwardCacheKey.empty());
  auto stale = role;
  stale.automaticBinding = prepared.automaticBinding;
  stale.requestId = "/source-request/stale";
  BOOST_CHECK_THROW(worker.executeAsync(stale.requestId, stale, io, prepared.runner,
    sourceInputs(2)).get(), std::invalid_argument);
  BOOST_CHECK_EQUAL(preparations.load(), 2U);
  BOOST_CHECK_THROW(worker.executePreparedAsync(projection.requestId, role, io,
    [runner] { return runner; }, sourceInputs(2)).get(), std::invalid_argument);
  const auto legacy = worker.prepareRunnerAsync([] {
    return std::make_shared<LambdaModelRunner>([] (const RoleExecutionContext&) {
      return std::map<std::string, TensorBundle>{};
    });
  }).get();
  BOOST_REQUIRE(legacy.runner);
  BOOST_CHECK(!legacy.automaticBinding);
  BOOST_CHECK_THROW(worker.prepareRunnerAsync([] { return nullptr; }).get(), std::runtime_error);
  BOOST_CHECK_EXCEPTION(worker.prepareRunnerAsync([] () -> std::shared_ptr<NativeModelRunner> {
    throw std::runtime_error("factory boundary");
  }).get(), std::runtime_error, [] (const auto& error) {
    return std::string(error.what()) == "factory boundary";
  });
  BOOST_CHECK_THROW(worker.prepareRunnerAsync([&] { called = true; return runner; },
    [] { throw std::runtime_error("preparation cancelled"); }).get(), std::runtime_error);
  BOOST_CHECK(!called);
}

BOOST_AUTO_TEST_CASE(AutomaticBindingsCannotFallThroughLegacyGenerationCodecs)
{
  const MetadataSourceFixture file(false);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto projection = sourceProjection(model);
  auto spec = sourceRunnerSpec(file, model, projection);
  spec.metadata["streamingGeneration"] = "true";
  OnnxRuntimeModelRunner runner(spec);
  NativeRequestControl control;
  control.requestId = projection.requestId; control.attempt = projection.attempt;
  control.deadline = metadataControl().deadline;
  const auto binding = runner.validateAutomaticRequestBinding(projection, control);
  BOOST_REQUIRE(binding);
  std::size_t events = 0;
  // This valid automatic forward binding cannot authorize the old generation
  // scaffold. Its real x input must never reach the legacy input_ids reader.
  BOOST_CHECK_EXCEPTION(ndnsf::di::test::runSamplingEpochs({{0.0F, 1.0F}},
    [&] (NativeEpochCoordinatorConfig& config) {
      config.automaticBinding = binding;
      config.initialInputs = sourceInputs(2);
      config.eventSink = [&] (const auto&) { ++events; return true; };
    }), std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "AUTOMATIC_GENERATION_CONTRACT_REQUIRED";
    });
  BOOST_CHECK_EQUAL(events, 0U);
  RoleExecutionContext context;
  context.automaticBinding = binding;
  BOOST_CHECK(!runner.runStreamed(context));
  context.streamEventSink = [] (const auto&) { return true; };
  BOOST_CHECK_EXCEPTION(runner.runStreamed(context), std::invalid_argument,
    [] (const auto& error) {
      return std::string(error.what()) == "AUTOMATIC_GENERATION_RUNTIME_UNSUPPORTED";
    });
}

BOOST_AUTO_TEST_CASE(AutomaticSourceFactsReachColdAndDiskCacheThroughAssignedEncryptedObjects)
{
  const MetadataSourceFixture file(true);
  const auto automatic = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto entry = sourceCatalogEntry(automatic);
  const NativeCanonicalPreparationCatalog preparation({entry}, metadataControl());
  const auto source = preparation.sourceRefFor(entry.model.descriptor);
  const auto& material = *source->materialManifest;
  const auto manifestWire = material.canonicalJson();
  ndn::Name prefix("/source-publisher/models/test-" + std::to_string(::getpid()));
  prefix.appendVersion();
  const auto name = [&] (const std::string& id) {
    return nativeStoredModelObjectName(prefix, id).toUri(ndn::name::UriFormat::CANONICAL);
  };
  const auto bytes = [] (const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
  };
  std::vector<NativeStoredModelInput> inputs;
  NativePreparedCanonicalPublication prepared;
  prepared.materialManifest = source->materialManifest;
  auto records = NativeJson::array();
  for (std::size_t index = 0; index < material.payloads.size(); ++index) {
    const auto& payload = material.payloads[index];
    const auto id = "part-" + std::to_string(index);
    auto value = payload.copyBytes(metadataControl().requireActive);
    records.push_back({{"payloadId", payload.payloadId}, {"dataName", name(id)},
      {"digest", payload.digest}, {"bytes", value.size()}, {"bundleDigest", payload.digest},
      {"bundleBytes", value.size()}, {"bundleOffset", 0}});
    prepared.materialPayloadIds.push_back(payload.payloadId);
    prepared.materialDataNames.push_back(name(id));
    prepared.materialDigests.push_back(payload.digest);
    inputs.push_back({id, std::move(value)});
  }
  const auto receipt = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-material-receipt-v1"}, {"sourceDigest", material.sourceDigest},
    {"graphDigest", material.graphDigest}, {"materialIdentityDigest", material.manifestDigest},
    {"materialManifestDigest", nativePlanningDigest(manifestWire)},
    {"materialObjects", std::move(records)}});
  NativeJson metadata{{"canonicalSourceDigest", material.sourceDigest},
    {"canonicalSourceBytes", entry.model.canonicalSourceBytes}, {"materialBacked", true},
    {"canonicalInitializerBytes", entry.model.canonicalInitializerBytes},
    {"canonicalInitializerObjectDigest", entry.model.canonicalInitializerObjectDigest},
    {"materialManifestDataName", name("material")},
    {"materialManifestDigest", nativePlanningDigest(manifestWire)},
    {"materialManifestBytes", manifestWire.size()}, {"materialIdentityDigest", material.manifestDigest},
    {"materialReceiptDataName", name("receipt")}, {"materialReceiptDigest", nativePlanningDigest(receipt)},
    {"materialReceiptBytes", receipt.size()}};
  auto projection = sourceProjection(automatic);
  const auto root = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", projection.assembly.artifactProfileDigest},
    {"modelName", automatic.descriptor.modelName},
    {"modelIdentityDigest", automatic.descriptor.contentDigest}, {"metadata", metadata}});
  inputs.push_back({"business-root", bytes(root)});
  inputs.push_back({"material", bytes(manifestWire)});
  inputs.push_back({"receipt", bytes(receipt)});
  inputs.push_back({"owner-metadata", bytes(source->preparedMetadataJson)});
  inputs.push_back({"catalog", {'o', 'w', 'n', 'e', 'r'}});
  SourcePacketFacility facility;
  auto io = facility.port();
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  std::size_t next = 0;
  const auto publication = nativePublishStoredModel(keys, ndn::Name("/source-publisher"),
    ndn::Name("/source-repo"), prefix, automatic.descriptor.contentDigest,
    "catalog", "business-root", [&] () -> std::optional<NativeStoredModelInput> {
      if (next == inputs.size()) return std::nullopt;
      return std::move(inputs[next++]);
    }, io);
  const auto objects = nativeLoadStoredModelCatalog(publication.handle, io);
  prepared.rootDataName = name("business-root");
  prepared.materialManifestDataName = name("material");
  prepared.materialReceiptDataName = name("receipt");
  prepared.preparedMetadataDataName = name("owner-metadata");
  const auto index = nativeBuildStoredModelObjectKeyIndex(publication.handle, objects, prepared);
  BOOST_REQUIRE_EQUAL(index->providerAssemblyMetadataNames.size(), 3U);
  const auto permitted = nativeSelectModelObjectKeyDataNames(*index, {0});
  BOOST_CHECK(permitted.count(name("owner-metadata")) == 0);
  BOOST_CHECK(permitted.count(name("catalog")) == 0);
  // Exercise the real assignment selector and object-key lease with real
  // encrypted original packets. This is not the network grant exchange.
  auto lease = std::make_shared<NativeStoredModelObjectKeyLease>(publication.handle,
                                                                ndn::Name("/source-repo"));
  for (const auto& permittedName : permitted) {
    const auto& object = index->storedObjectsByName.at(permittedName);
    lease->add(object, nativeStoredModelObjectKey(publication.handle.masterKey,
      publication.handle.producer, object.object.name));
  }
  lease->finalize();
  BOOST_CHECK(!lease->contains(ndn::Name(name("owner-metadata"))));
  BOOST_CHECK_THROW(facility.read(*lease, ndn::Name(name("owner-metadata"))), std::exception);
  projection.canonicalArtifactName = prepared.rootDataName;
  projection.storedModelObjectKeyLease = lease;
  projection.storedModelCacheIdentity = lease->cacheIdentity();
  projection.assembly.modelManifestDigest = nativePlanningDigest(root);
  projection.assembly.nodeIndices = {0};
  projection.assembly.expectedOutputs = {{"hidden", "float32",
    {std::string("batch"), std::int64_t(4)}}};
  projection.assembly.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(projection.assembly));
  projection.selectedRole = projection.assembly;
  BOOST_REQUIRE_NO_THROW(validateNativeAutomaticSelectionBinding(projection));
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&] (const ndn::Name& object) { return facility.read(*lease, object); };
  fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& object, const ndn::Name& service) {
    BOOST_REQUIRE(service == ndn::Name("/service"));
    return facility.read(*lease, object);
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = (file.root / "assembled-cache").string();
  options.providerIdentity = projection.provider;
  const auto* bin = std::getenv("NDNSF_SPEC182_BIN_DIR");
  options.workerLocation = {((bin ? std::filesystem::path(bin) :
    std::filesystem::path("/usr/local/libexec/ndnsf-di")) / "DI_NativeOnnxAssemblyWorker").string(), ""};
  options.signManifest = [] (const std::string&) { return "source-fixture-signature"; };
  nativeBindStoredModelAssemblyCache(projection, options);
  // Current-root primitive checks must run before any encrypted material read.
  // This injected root port is a negative parser fixture, not a valid publication.
  const auto beforeMalformedRoots = facility.opened;
  for (const auto& bad : std::vector<NativeJson>{"bad", "0", -1, false, nullptr, 1.5}) {
    for (const auto* field : {"canonicalInitializerBytes", "materialManifestBytes",
                              "canonicalSourceBytes", "materialReceiptBytes"}) {
      auto malformedRoot = nativeParseJson(root);
      malformedRoot["metadata"][field] = bad;
      const auto wire = nativeCanonicalJson(malformedRoot);
      auto changed = projection;
      changed.assembly.modelManifestDigest = nativePlanningDigest(wire);
      changed.assembly.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(changed.assembly));
      changed.selectedRole = changed.assembly;
      auto negativePorts = fetchers;
      negativePorts.getArtifact = [&] (const ndn::Name& requested) -> std::optional<ndn::Buffer> {
        BOOST_REQUIRE(requested == ndn::Name(prepared.rootDataName));
        return ndn::Buffer(wire.begin(), wire.end());
      };
      BOOST_TEST_CONTEXT("root field=" << field << " value=" << bad.dump()) {
        BOOST_CHECK_EXCEPTION(prepareNativeCanonicalOnnxRole(negativePorts, changed, options),
          std::runtime_error, [] (const auto& error) {
            return std::string(error.what()).find("DI_AUTOMATIC_SOURCE_ROOT_METADATA_INVALID") !=
              std::string::npos;
          });
      }
      BOOST_CHECK(facility.opened == beforeMalformedRoots);
    }
  }
  const auto cold = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_CHECK(cold.metadata.count("automaticSourceBinding") == 1);
  const auto counts = facility.opened;
  const auto cached = tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options);
  BOOST_REQUIRE(cached);
  BOOST_CHECK_EQUAL(cached->path, cold.path);
  BOOST_CHECK_EQUAL(cached->metadata.at("automaticSourceBinding"),
                    cold.metadata.at("automaticSourceBinding"));
  for (const auto& objectName : permitted) {
    if (index->providerAssemblyMetadataNames.count(objectName)) continue;
    BOOST_CHECK_EQUAL(facility.opened[ndn::Name(objectName)], counts.count(ndn::Name(objectName))
      ? counts.at(ndn::Name(objectName)) : 0U);
  }
  auto invalid = projection;
  invalid.automaticShapeSeed->envelopeDigest = nativePlanningDigest("invalid-current-request");
  BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, invalid, options),
                    std::exception);
  BOOST_CHECK(std::filesystem::exists(cold.path));
  BOOST_CHECK(tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options).has_value());
  BOOST_CHECK(facility.opened.count(ndn::Name(name("owner-metadata"))) == 0);
}

BOOST_AUTO_TEST_CASE(AutomaticFilesystemCacheReopensInFreshProcess)
{
  const char* childPath = std::getenv("NDNSF_SPEC233_FS_REOPEN_CHILD");
  if (childPath) {
    // Exec starts here with no parent catalog, publisher, source FD or cache owner.
    const std::filesystem::path sidecarPath(childPath);
    const auto data = nativeParseJson(readCacheFixtureFile(sidecarPath));
    BOOST_REQUIRE_EQUAL(data.at("schema").get<std::string>(), "spec233-fs-reopen-v1");
    const auto root = sidecarPath.parent_path();
    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = (root / "assembled-cache").string();
    options.cacheCompatibilitySourceDir = root / "local-materials";
    options.providerIdentity = data.at("provider").get<std::string>();
    options.verifyCachedArtifactDigest = true;
    options.assemblyTimeoutMs = 30000;
    const auto decode = [&](const NativeJson& row) {
      std::istringstream input(row.at("wire").get<std::string>());
      auto projection = nativeSelectionProjectionV3FromJson(input,
        data.at("role").get<std::string>());
      // These are deliberately assignment-local, not Selection wire fields.
      projection.canonicalArtifactName = data.at("rootName").get<std::string>();
      projection.plan.serviceName = data.at("service").get<std::string>();
      projection.plan.modelName = data.at("model").get<std::string>();
      (void)roleSpecFromSelectionProjectionV3(projection, projection.provider);
      return projection;
    };
    std::size_t networkCalls = 0;
    NativeCanonicalOnnxFetchers forbidden;
    forbidden.getArtifact = [&](const ndn::Name&) -> std::optional<ndn::Buffer> {
      ++networkCalls; throw std::runtime_error("FS recovery attempted a network root read");
    };
    forbidden.fetchEncryptedLargeData = [&](const ndn::Name&, const ndn::Name&)
        -> std::optional<ndn::Buffer> {
      ++networkCalls; throw std::runtime_error("FS recovery attempted a network material read");
    };
    const auto path = data.at("artifactPath").get<std::string>();
    const auto artifactDigest = data.at("artifactDigest").get<std::string>();
    const auto unchanged = [&] {
      BOOST_CHECK(std::filesystem::exists(path));
      BOOST_CHECK_EQUAL(nativePlanningDigest(readCacheFixtureFile(path)), artifactDigest);
    };
    BOOST_REQUIRE_EQUAL(data.at("requests").size(), 2U);
    for (const auto& row : data.at("requests")) {
      const auto projection = decode(row);
      for (const auto empty : {false, true}) {
        const auto cached = tryLoadNativeCanonicalOnnxRoleFromCache(
          empty ? NativeCanonicalOnnxFetchers{} : forbidden, projection, options);
        BOOST_REQUIRE(cached); // A miss must never fall back to cold preparation.
        BOOST_CHECK_EQUAL(cached->path, path);
        const auto binding = AutomaticOnnxSourceBinding::fromCanonicalJson(
          cached->metadata.at("automaticSourceBinding"), metadataControl());
        const auto envelope = admitAutomaticOnnxProjection(binding, projection, metadataControl());
        BOOST_CHECK_EQUAL(automaticOnnxShapeSeed(envelope).envelopeDigest,
                          row.at("envelopeDigest").get<std::string>());
        BOOST_REQUIRE(envelope.tensors.at("graph/y").maxBytes);
        BOOST_CHECK_EQUAL(*envelope.tensors.at("graph/y").maxBytes,
                          row.at("batch").get<std::uint64_t>() * 4 * sizeof(float));
        unchanged();
      }
    }
    const auto projection = decode(data.at("requests").at(0));
    auto invalid = projection;
    invalid.automaticShapeSeed->envelopeDigest = nativePlanningDigest("invalid-FS-current-seed");
    BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, invalid, options),
                      std::exception);
    unchanged();
    auto expired = projection; expired.deadlineMs = 1;
    BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, expired, options),
                      std::exception);
    auto cancelled = options; cancelled.shouldCancel = [] { return true; };
    BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, projection, cancelled),
                      std::exception);
    unchanged();
    const auto checkDamagedObject = [&](const std::string& digest) {
      const auto hex = digest.substr(7);
      const auto object = options.cacheCompatibilitySourceDir / "objects" / "sha256" /
        hex.substr(0, 2) / hex;
      const auto original = readCacheFixtureFile(object);
      const auto backup = std::filesystem::path(object.string() + ".test-backup");
      std::filesystem::rename(object, backup);
      {
        BOOST_SCOPE_EXIT_ALL(&) { std::filesystem::rename(backup, object); };
        BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, projection, options),
                          std::exception);
        unchanged();
      }
      {
        BOOST_SCOPE_EXIT_ALL(&) { writeCacheFixtureFile(object, original); };
        auto corrupt = original; corrupt[0] ^= 1;
        writeCacheFixtureFile(object, corrupt);
        BOOST_CHECK_THROW(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, projection, options),
                          std::exception);
        unchanged();
      }
    };
    checkDamagedObject(projection.assemblyContract().modelManifestDigest);
    checkDamagedObject(data.at("materialManifestDigest").get<std::string>());
    BOOST_CHECK(tryLoadNativeCanonicalOnnxRoleFromCache(forbidden, projection, options).has_value());
    unchanged();
    BOOST_CHECK_EQUAL(networkCalls, 0U);
    return;
  }

  const MetadataSourceFixture file(false);
  NativeJson data;
  {
    const auto automatic = inspectAutomaticOnnxModel({file.path}, metadataControl());
    const auto entry = sourceCatalogEntry(automatic);
    const NativeCanonicalPreparationCatalog catalog({entry}, metadataControl());
    const auto source = catalog.sourceRefFor(entry.model.descriptor);
    std::filesystem::create_directory(file.root / "local-materials");
    NativeFilesystemArtifactPublisher publisher(file.root / "local-materials",
      ndn::Name("/fs-source"), automatic.contract.sourceDigest);
    NativeCanonicalPublicationOptions publicationOptions;
    publicationOptions.artifactProfileDigest = entry.recipe.artifactProfileDigest;
    NativeRequestControl control{"/fs-cold", 1, metadataControl().deadline, {}};
    const auto publication = publisher.publish("automatic-fixture", "/service", entry.model,
      *source, publicationOptions, control);
    const auto projectionFor = [&](std::uint64_t batch) {
      auto projection = sourceProjection(automatic, batch);
      projection.canonicalArtifactName = publication.rootDataName;
      projection.assembly.modelManifestDigest = publication.manifestDigest;
      projection.assembly.recipeDigest = nativePlanningDigest(
        canonicalNativeOnnxRecipeJson(projection.assembly));
      projection.selectedRole = projection.assembly;
      return completeFilesystemSelection(std::move(projection), "/fs-request/" +
        std::to_string(batch));
    };
    const auto first = projectionFor(2);
    const auto other = projectionFor(1);
    BOOST_CHECK_EQUAL(first.assembly.recipeDigest, other.assembly.recipeDigest);
    BOOST_CHECK_NE(first.automaticShapeSeed->envelopeDigest, other.automaticShapeSeed->envelopeDigest);
    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = (file.root / "assembled-cache").string();
    options.cacheCompatibilitySourceDir = file.root / "local-materials";
    options.providerIdentity = first.provider;
    options.verifyCachedArtifactDigest = true;
    const auto* bin = std::getenv("NDNSF_SPEC182_BIN_DIR");
    options.workerLocation = {((bin ? std::filesystem::path(bin) :
      std::filesystem::path("/usr/local/libexec/ndnsf-di")) /
        "DI_NativeOnnxAssemblyWorker").string(), ""};
    options.signManifest = [] (const std::string&) { return "fs-fixture-signature"; };
    std::size_t networkCalls = 0;
    NativeCanonicalOnnxFetchers forbidden;
    forbidden.getArtifact = [&](const ndn::Name&) -> std::optional<ndn::Buffer> {
      ++networkCalls; throw std::runtime_error("FS cold attempted a network root read");
    };
    forbidden.fetchEncryptedLargeData = [&](const ndn::Name&, const ndn::Name&)
        -> std::optional<ndn::Buffer> {
      ++networkCalls; throw std::runtime_error("FS cold attempted a network material read");
    };
    const auto cold = prepareNativeCanonicalOnnxRole(forbidden, first, options);
    BOOST_CHECK_EQUAL(networkCalls, 0U);
    data = NativeJson{{"schema", "spec233-fs-reopen-v1"}, {"role", first.assembly.selectedRole},
      {"rootName", publication.rootDataName}, {"service", first.plan.serviceName},
      {"model", first.plan.modelName}, {"provider", first.provider}, {"artifactPath", cold.path},
      {"artifactDigest", nativePlanningDigest(readCacheFixtureFile(cold.path))},
      {"materialManifestDigest", publication.materialManifestDigest}, {"requests", NativeJson::array()}};
    const std::pair<const NativeSelectionProjectionV3*, std::uint64_t> requests[] = {
      {&first, 2}, {&other, 1}};
    for (const auto& item : requests) {
      data["requests"].push_back({{"wire", nativeSelectionProjectionV3ToJson(*item.first)},
        {"envelopeDigest", item.first->automaticShapeSeed->envelopeDigest}, {"batch", item.second}});
    }
  } // Source/catalog/publication/cold-preparation owners end before exec.
  const auto sidecar = file.root / "reopen.json";
  writeCacheFixtureFile(sidecar, nativeCanonicalJson(data));
  const auto executable = std::filesystem::read_symlink("/proc/self/exe");
  BOOST_REQUIRE(executable.is_absolute());
  BOOST_REQUIRE(std::filesystem::is_regular_file(executable));
  const auto log = file.root / "reopen-child.log";
  const auto status = boost::process::system("/usr/bin/timeout",
    boost::process::args(std::vector<std::string>{"--signal=TERM", "--kill-after=5", "15",
      "/usr/bin/env", "NDNSF_SPEC233_FS_REOPEN_CHILD=" + sidecar.string(), executable.string(),
      "--run_test=Spec233PreparedMetadata/AutomaticFilesystemCacheReopensInFreshProcess",
      "--log_level=test_suite"}),
    (boost::process::std_out & boost::process::std_err) > log.string());
  const auto childLog = readCacheFixtureFile(log);
  if (status != 0) BOOST_TEST_MESSAGE(childLog);
  BOOST_REQUIRE_EQUAL(status, 0);
  BOOST_CHECK(childLog.find("No errors detected") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(SourceMetadataSeparatesNormalAndProtectedSessionKeys)
{
  const MetadataSourceFixture file(false);
  const MetadataSourceFixture otherFile(true);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto otherModel = inspectAutomaticOnnxModel({otherFile.path}, metadataControl());
  const auto projection = sourceProjection(model);
  for (const bool protectedIdentity : {false, true}) {
    OnnxRuntimeSessionCache::Config configuration;
    configuration.maxEntries = 4;
    auto cache = std::make_shared<OnnxRuntimeSessionCache>(configuration);
    auto spec = sourceRunnerSpec(file, model, projection);
    if (protectedIdentity) spec.metadata["protectedResidentIdentity"] = "source-key-fixture";
    {
      OnnxRuntimeModelRunner first(spec, cache);
      OnnxRuntimeModelRunner repeated(spec, cache);
      BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
      BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
      for (const auto* key : {"outputAlias.y", "output_alias.y"}) {
        auto aliased = spec;
        aliased.metadata[key] = "changed-name";
        BOOST_CHECK_EXCEPTION(OnnxRuntimeModelRunner(aliased, cache), std::invalid_argument,
          [] (const auto& error) {
            return std::string(error.what()) == "AUTOMATIC_OUTPUT_ALIAS_UNSUPPORTED";
          });
        BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
        BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
      }
      // Hold every other declared cache field fixed to isolate the source
      // metadata key. This deliberately foreign spec is not request-qualified.
      auto different = spec;
      different.metadata["automaticSourceBinding"] = sourceBinding(otherModel).canonicalJson(metadataControl());
      OnnxRuntimeModelRunner separate(different, cache);
      BOOST_CHECK_EQUAL(cache->counters().loads, 2U);
      auto noIo = spec; noIo.metadata.erase("automaticIoContract");
      BOOST_CHECK_THROW(OnnxRuntimeModelRunner(noIo, cache), std::invalid_argument);
      BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
      BOOST_CHECK_EQUAL(cache->counters().loads, 2U);
      auto malformed = spec; malformed.metadata["automaticSourceBinding"] = "{}";
      BOOST_CHECK_THROW(OnnxRuntimeModelRunner(malformed, cache), std::exception);
    }
    cache->close();
    BOOST_CHECK(cache->drain(std::chrono::milliseconds(50)));
  }
  // A warm legacy protected entry must not admit a source marker without I/O.
  OnnxRuntimeSessionCache::Config configuration;
  auto cache = std::make_shared<OnnxRuntimeSessionCache>(configuration);
  auto legacy = sourceRunnerSpec(file, model, projection);
  legacy.metadata.erase("automaticIoContract");
  legacy.metadata.erase("automaticSourceBinding");
  legacy.metadata["protectedResidentIdentity"] = "legacy-source-marker-fixture";
  {
    OnnxRuntimeModelRunner first(legacy, cache);
    OnnxRuntimeModelRunner repeated(legacy, cache);
    auto invalid = legacy;
    invalid.metadata["automaticSourceBinding"] = sourceBinding(model).canonicalJson(metadataControl());
    BOOST_CHECK_THROW(OnnxRuntimeModelRunner(invalid, cache), std::invalid_argument);
    BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
    BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
  }
  cache->close();
  BOOST_CHECK(cache->drain(std::chrono::milliseconds(50)));
}

BOOST_AUTO_TEST_CASE(CurrentAdmissionControlIsNotRetainedAndDefaultRunnerRejectsAutomaticUse)
{
  const MetadataSourceFixture file(false);
  const auto model = inspectAutomaticOnnxModel({file.path}, metadataControl());
  const auto projection = sourceProjection(model);
  OnnxRuntimeModelRunner runner(sourceRunnerSpec(file, model, projection));
  std::shared_ptr<const AutomaticOnnxRequestBinding> retained;
  auto token = std::make_shared<int>(0);
  const std::weak_ptr<int> observer = token;
  {
    NativeRequestControl control;
    control.requestId = projection.requestId; control.attempt = projection.attempt;
    control.deadline = metadataControl().deadline;
    control.cancelled = [token] { ++*token; return false; };
    retained = runner.validateAutomaticRequestBinding(projection, control);
    BOOST_CHECK_GT(*token, 0);
    unsigned fences = 0;
    control.cancelled = [&] { return ++fences >= 3; };
    BOOST_CHECK_THROW(runner.validateAutomaticRequestBinding(projection, control), std::runtime_error);
  }
  token.reset();
  BOOST_REQUIRE(retained);
  BOOST_CHECK(observer.expired());
  unsigned calls = 0;
  auto unsupported = makeNativeModelRunner([&] (const RoleExecutionContext&) {
    ++calls; return std::map<std::string, TensorBundle>{};
  });
  NativeRequestControl control;
  control.requestId = projection.requestId; control.attempt = projection.attempt;
  control.deadline = metadataControl().deadline;
  BOOST_CHECK_THROW(unsupported->validateAutomaticRequestBinding(projection, control),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
