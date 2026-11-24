#include "tests/fixtures/wan-video/wan-video-fixture.hpp"
#include "tests/fixtures/wan-video/wan-video-reference.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/initializer-slice.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-entry-extraction.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-activation-tiles.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-attention-context.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-attention-tile.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-entry-plan.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-feed-forward.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-generation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-head-projection.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-normalized-projection.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-patch-embedding.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-source-layout.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-time-embedding.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-umt5.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-unipc.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-vae.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-video-block.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-video-head.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-video-rotary.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-step-template-json.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/model-object-keys.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-preparation.hpp"
#include "ndn-service-framework/RequestConfidentiality.hpp"
#include "tests/fixtures/native-dependency-bus.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/test-resource-paths.hpp"

#include <boost/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ndnsf::di::wan::tests {

namespace videoFixture = ndnsf::di::wan::fixture;

namespace {

using videoFixture::makeCatalogTemplate;

template<typename MatchInitializer>
NativeCanonicalSource
selectCanonicalEntry(const NativeCanonicalSource::MaterialManifest& producer,
                     std::shared_ptr<const NativeCanonicalSource::MaterialManifest> index,
                     const OnnxEntryCut& entry, MatchInitializer matchInitializer)
{
  NativeCanonicalSource selected;
  selected.materialManifest = std::move(index);
  std::set<std::string> ids{selected.materialManifest->templatePayloadId};
  for (const auto& reference : selected.materialManifest->references) {
    if ((reference.kind == "graph-node" &&
         std::find(entry.nodeIndices.begin(), entry.nodeIndices.end(), reference.nodeIndex) !=
           entry.nodeIndices.end()) ||
        (reference.kind == "shared-initializer" && matchInitializer(reference.logicalName))) {
      ids.insert(reference.payloadId);
      ids.insert(reference.chunkPayloadIds.begin(), reference.chunkPayloadIds.end());
    }
  }
  for (const auto& payload : producer.payloads) {
    if (ids.count(payload.payloadId) != 0) selected.materialPayloads.push_back(payload);
  }
  return selected;
}

void
runCompiledEntriesFixture(const OnnxParallelPlan& pipeline, const NativeCanonicalSource& source,
                          std::map<std::string, Ort::Value>& values, Ort::Env& env,
                          const Ort::SessionOptions& options, const Ort::MemoryInfo& memory,
                          const NativeAssemblyControl& control, bool checkReplicatedSums = false)
{
  const auto json = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  for (const auto& step : pipeline.plan.steps) {
    if (step.operation == "SUM") {
      const auto& name = step.outputSlots.front().slotId;
      std::vector<std::vector<NamedTensorView>> contributions;
      for (const auto& slot : step.inputSlots) {
        const auto& value = values.at(slot.producerSlotId);
        const auto info = value.GetTensorTypeAndShapeInfo();
        BOOST_REQUIRE(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        const auto* data = reinterpret_cast<const std::uint8_t*>(value.GetTensorRawData());
        contributions.push_back({{name, TensorElementType::Float32, info.GetShape(),
                                   {data, value.GetTensorSizeInBytes()}}});
      }
      const auto result = sumTensorBundleViews(name, contributions);
      const auto reduced = decodeTensorBundleViews(result);
      BOOST_REQUIRE_EQUAL(reduced.size(), 1);
      const auto& tensor = reduced.front();
      Ort::AllocatorWithDefaultOptions allocator;
      auto value = Ort::Value::CreateTensor<float>(allocator, tensor.shape.data(),
                                                   tensor.shape.size());
      std::memcpy(value.GetTensorMutableData<float>(), tensor.payload.data(),
                  tensor.payload.size());
      if (const auto previous = values.find(name);
          checkReplicatedSums && previous != values.end()) {
        // Replicated reductions have two role-local producers for one logical port.
        // The single-process fixture runs both and checks that their values agree.
        BOOST_CHECK(previous->second.GetTensorTypeAndShapeInfo().GetShape() == tensor.shape);
        BOOST_REQUIRE_EQUAL(previous->second.GetTensorSizeInBytes(), tensor.payload.size());
        BOOST_CHECK_EQUAL(std::memcmp(previous->second.GetTensorRawData(), tensor.payload.data(),
                                     tensor.payload.size()), 0);
        continue;
      }
      BOOST_CHECK(values.emplace(name, std::move(value)).second);
      continue;
    }
    BOOST_REQUIRE_EQUAL(step.operation, "CALL_SUBGRAPH");
    const auto& cut = pipeline.entries.at(step.artifactEntry);
    auto selected = selectCanonicalEntry(*source.materialManifest, index, cut,
                                         [] (const auto&) { return true; });
    const auto bytes = materializeNativeCanonicalModel(
      selected, cut.nodeIndices, cut.inputs, cut.outputs, control);
    Ort::Session session(env, bytes.data(), bytes.size(), options);
    std::vector<const char*> inputs, outputs;
    std::vector<Ort::Value> arguments;
    for (const auto& slot : step.inputSlots) {
      auto& value = values.at(slot.slotId);
      const auto info = value.GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      inputs.push_back(slot.slotId.c_str());
      arguments.push_back(Ort::Value::CreateTensor(
        memory, value.GetTensorMutableRawData(), value.GetTensorSizeInBytes(),
        shape.data(), shape.size(), info.GetElementType()));
    }
    for (const auto& slot : step.outputSlots) outputs.push_back(slot.slotId.c_str());
    auto result = session.Run(Ort::RunOptions{}, inputs.data(), arguments.data(), inputs.size(),
                              outputs.data(), outputs.size());
    for (std::size_t i = 0; i < result.size(); ++i)
      BOOST_CHECK(values.emplace(outputs[i], std::move(result[i])).second);
  }
}

class HeaderSource final : public NativeCanonicalByteRangeSource
{
public:
  HeaderSource(std::string header, std::uint64_t fullSize)
    : m_header(std::move(header)), m_fullSize(fullSize)
  {
  }
  std::uint64_t size() const noexcept override { return m_fullSize; }
  std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_header.size() || length > m_header.size() - offset)
      throw std::out_of_range("metadata fixture may not fetch weights");
    return {m_header.begin() + offset, m_header.begin() + offset + length};
  }
private:
  std::string m_header;
  std::uint64_t m_fullSize;
};

class ZeroInitializerSource final : public NativeCanonicalByteRangeSource
{
public:
  explicit ZeroInitializerSource(std::uint64_t bytes, const char* label = "Official DiT")
    : m_bytes(bytes)
    , m_label(label)
  {
  }

  std::uint64_t size() const noexcept override { return m_bytes; }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_bytes || length > m_bytes - offset ||
        length > NativeCanonicalMaterialBundleMaxBytes)
      throw std::out_of_range("synthetic initializer read exceeds its window");
    const auto previous = m_bytesRead.fetch_add(length, std::memory_order_relaxed);
    if (m_bytes != 0 && previous / m_bytes != (previous + length) / m_bytes)
      std::clog << m_label << " synthetic range bytes read=" << previous + length
                << std::endl;
    return std::vector<std::uint8_t>(static_cast<std::size_t>(length));
  }

private:
  std::uint64_t m_bytes;
  std::string m_label;
  mutable std::atomic<std::uint64_t> m_bytesRead{0};
};

void
checkOfficialDitStoredPublication(const onnx::ModelProto& sourceModel,
  const OnnxParallelPlan& layout, std::uint64_t sidecarBytes, std::uint64_t tileBytes)
{
  constexpr std::uint64_t LIMIT = 64ULL << 30;
  // The 15-minute probes established the CPU-bound catalog failure on this
  // host. Give this opt-in diagnostic time to reach the Repo wire preflight.
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::minutes(30);
  const NativeAssemblyControl control{
    deadline, [&] {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("official DiT material preflight deadline expired");
    }, LIMIT, LIMIT};
  const auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  const auto digest = [](const std::string& value) { return nativePlanningDigest(value); };
  const auto descriptor = ndnsf::di::fixture::completeModel({
    "official-dit-geometry", digest("shape-fixture"), digest("dit-model"),
    digest("dit-graph"), "onnx", "fp32", "wan", "1"});
  const NativeRoleRecipeProfile profile{digest("dit-role"), digest("dit-assembler"),
    "fixture-ort", "fp32", "none", "native", "right", "dit", LIMIT, LIMIT, 65536};
  std::map<std::string, NativeRoleResourceRequirement> resources;
  for (const auto& role : layout.plan.roles)
    resources.emplace(role, NativeRoleResourceRequirement{
      descriptor.adapter.backends, 32ULL << 30, 1U << 30, 0, 1U << 30, 1U << 30, 1.0});
  const ndn::Name producer("/ndnsf-test/wan/requester");
  auto config = makeCatalogTemplate(descriptor, profile, "/fixture/dit/source",
    digest("dit-manifest"), "/fixture/dit/package", "/fixture/dit", resources);
  NativeCanonicalSource source;
  source.initializerRangeSource = std::make_shared<ZeroInitializerSource>(sidecarBytes);
  auto compiled = compileWanComponent(sourceModel, layout, tileBytes, std::move(source),
                                      std::move(config), control);
  std::clog << "Official DiT synthesized compiled catalog bytes="
            << compiled.configurationJson.size() << " elapsed_s=" << elapsed() << std::endl;
  const auto catalog = NativeRequestCatalog::load(compiled.configurationJson,
    std::move(compiled.source), control);
  const auto stored = catalog.preparation->sourceRefFor(catalog.model.descriptor);
  BOOST_REQUIRE(stored && stored->materialManifest && !stored->preparedMetadataJson.empty());
  BOOST_REQUIRE_EQUAL(catalog.planningCache->candidates.at(0).roles.size(), 2);
  std::clog << "Official DiT synthesized prepared metadata bytes="
            << stored->preparedMetadataJson.size() << " material payloads="
            << stored->materialManifest->payloads.size() << " material manifest bytes="
            << stored->materialManifest->canonicalJson().size()
            << " elapsed_s=" << elapsed() << std::endl;
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  struct FirstCommit {};
  NativeStoredModelPacketIo io;
  io.requireCurrent = control.requireActive;
  io.commit = [&] (auto wires, const std::string&) {
    BOOST_REQUIRE(!wires.empty());
    std::clog << "Official DiT first bounded publication batch packets=" << wires.size()
              << " elapsed_s=" << elapsed() << std::endl;
    throw FirstCommit{};
  };
  // The first commit is reached only after role audiences, encrypted objects,
  // receipt bounds, and full publication wire budget have been checked.
  BOOST_CHECK_THROW(nativePublishStoredPreparedModel(keys, producer, ndn::Name("/fixture/repo"),
    ndn::Name(producer).append("dit-preflight").appendVersion(1), catalog,
    compiled.configurationJson, io), FirstCommit);
}

struct OfficialRoleWeights
{
  std::uint64_t entryBytes = 0;
  std::uint64_t maxEntryBytes = 0;
  std::uint64_t uniqueBytes = 0;
  const NativeExecutionStepSpec* largestEntry = nullptr;
};

std::map<std::string, OfficialRoleWeights>
checkOfficialTpRoleWeights(const char* component, const onnx::GraphProto& graph,
                           const NativeExecutionPlan& plan, std::uint64_t sidecarBytes)
{
  std::map<std::string, std::uint64_t> lengths;
  std::map<std::uint64_t, std::uint64_t> ranges;
  for (const auto& tensor : graph.initializer()) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL) continue;
    std::map<std::string, std::string> fields;
    for (const auto& field : tensor.external_data())
      BOOST_REQUIRE(fields.emplace(field.key(), field.value()).second);
    const auto offset = std::stoull(fields.at("offset"));
    const auto length = std::stoull(fields.at("length"));
    BOOST_REQUIRE_GT(length, 0);
    BOOST_REQUIRE_LE(offset, sidecarBytes);
    BOOST_REQUIRE_LE(length, sidecarBytes - offset);
    BOOST_REQUIRE(ranges.emplace(offset, length).second);
    BOOST_REQUIRE(lengths.emplace(tensor.name(), length).second);
  }
  std::uint64_t covered = 0;
  for (const auto& [offset, length] : ranges) {
    BOOST_REQUIRE_EQUAL(offset, covered);
    covered += length;
  }
  BOOST_REQUIRE_EQUAL(covered, sidecarBytes);

  std::map<std::string, OfficialRoleWeights> usage;
  std::map<std::string, std::string> owners;
  for (const auto& step : plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    std::set<std::string> weights;
    const auto collect = [&](const auto& recurse, const onnx::NodeProto& node) -> void {
      for (const auto& name : node.input())
        if (lengths.count(name) != 0) weights.insert(name);
      for (const auto& attr : node.attribute()) {
        if (attr.has_g())
          for (const auto& child : attr.g().node()) recurse(recurse, child);
        for (const auto& subgraph : attr.graphs())
          for (const auto& child : subgraph.node()) recurse(recurse, child);
      }
    };
    for (const auto index : step.onnxNodeIndices)
      collect(collect, graph.node(static_cast<int>(index)));
    auto& role = usage[step.unitId];
    std::uint64_t entryBytes = 0;
    for (const auto& name : weights) {
      const auto length = lengths.at(name);
      const auto [owner, first] = owners.emplace(name, step.unitId);
      BOOST_REQUIRE_EQUAL(owner->second, step.unitId);
      if (first) role.uniqueBytes += length;
      entryBytes += length;
    }
    role.entryBytes += entryBytes;
    if (entryBytes > role.maxEntryBytes) {
      role.maxEntryBytes = entryBytes;
      role.largestEntry = &step;
    }
  }
  BOOST_REQUIRE_EQUAL(owners.size(), lengths.size());
  BOOST_REQUIRE_EQUAL(usage.size(), 2);
  std::uint64_t total = 0;
  for (const auto& [role, weights] : usage) {
    BOOST_REQUIRE(weights.largestEntry);
    BOOST_CHECK_GT(weights.uniqueBytes, 0);
    BOOST_CHECK_GE(weights.entryBytes, weights.uniqueBytes);
    total += weights.uniqueBytes;
    BOOST_TEST_MESSAGE("Official " << component << " role=" << role << " unique_external_bytes="
      << weights.uniqueBytes << " sum_entry_external_bytes=" << weights.entryBytes
      << " max_entry_external_bytes=" << weights.maxEntryBytes
      << " largest_entry=" << weights.largestEntry->stepId);
  }
  BOOST_CHECK_EQUAL(total, sidecarBytes);
  return usage;
}

void
probeOfficialVaeConv(const onnx::ModelProto& model, std::uint64_t initializerBytes,
                     const NativeExecutionStepSpec& step, const OnnxEntryCut& cut,
                     std::string_view variant)
{
  BOOST_REQUIRE(variant == "1" || variant == "2" || variant == "default-arena");
  const auto entryCount = variant == "1" ? 1U : 2U;
  NativeCanonicalSource source;
  const auto wire = model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  source.initializerRangeSource =
    std::make_shared<ZeroInitializerSource>(initializerBytes, "Official VAE");
  constexpr std::uint64_t LIMIT = 512U << 20;
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(60), [] {}, LIMIT, LIMIT};
  const auto producer = deriveNativeCanonicalMaterialManifest(source, control);
  const auto manifest = producer->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(manifest.begin(), manifest.end()));
  std::set<std::string> initializers;
  for (const auto& reference : index->references) {
    if (reference.kind == "graph-node" &&
        std::find(cut.nodeIndices.begin(), cut.nodeIndices.end(), reference.nodeIndex) !=
          cut.nodeIndices.end())
      initializers.insert(reference.dependencies.begin(), reference.dependencies.end());
  }
  auto selected = selectCanonicalEntry(*producer, index, cut,
    [&](const auto& name) { return initializers.count(name) != 0; });
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-vae-conv-memory");
  BOOST_REQUIRE(::mkdtemp(scratch.data()));
  const std::filesystem::path directory(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&directory) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  };
  std::ofstream sidecar(directory / "model.onnx.data", std::ios::binary);
  const auto files = materializeNativeCanonicalModelFiles(
    selected, cut.nodeIndices, cut.inputs, cut.outputs, control, [&](const auto& window) {
      sidecar.write(reinterpret_cast<const char*>(window.data()),
                    static_cast<std::streamsize>(window.size()));
      BOOST_REQUIRE(sidecar.good());
    });
  sidecar.close();
  BOOST_REQUIRE(sidecar.good());
  BOOST_CHECK_EQUAL(files.initializerBytes, 7962624U);
  const auto digest = nativePlanningDigest(files.modelBytes.data(), files.modelBytes.size());
  const auto entryFile = "entry-" + digest.substr(7) + ".onnx";
  {
    std::ofstream file(directory / entryFile, std::ios::binary);
    file.write(reinterpret_cast<const char*>(files.modelBytes.data()),
               static_cast<std::streamsize>(files.modelBytes.size()));
    file.close();
    BOOST_REQUIRE(file.good());
  }
  const auto names = [](const auto& slots) {
    std::vector<std::string> result;
    for (const auto& slot : slots) result.push_back(slot.slotId);
    return result;
  };
  NativeModelRunnerSpec spec;
  spec.role = step.unitId;
  spec.backend = "onnxruntime";
  spec.path = (directory / "model.onnx").string();
  const NativeJson entry{{"id", step.artifactEntry}, {"file", entryFile}, {"digest", digest},
                         {"inputs", names(step.inputSlots)}, {"outputs", names(step.outputSlots)}};
  spec.metadata[NATIVE_ONNX_ENTRY_TABLE] = nativeCanonicalJson(NativeJson::array({entry}));
  const auto sampleMemory = [](const char* phase) {
    std::ifstream status("/proc/self/status");
    BOOST_REQUIRE(status.good());
    unsigned fields = 0;
    for (std::string line; std::getline(status, line);) {
      if (line.rfind("VmRSS:", 0) == 0 || line.rfind("RssAnon:", 0) == 0 ||
          line.rfind("VmHWM:", 0) == 0) {
        BOOST_TEST_MESSAGE("VAE Conv " << phase << ' ' << line);
        ++fields;
      }
    }
    BOOST_REQUIRE_EQUAL(fields, 3U);
  };
  const auto shapeOf = [](const NativeStepSlotSpec& slot) {
    std::vector<std::int64_t> result;
    for (const auto& dim : slot.shape) result.push_back(std::get<std::int64_t>(dim));
    return result;
  };
  const auto bytesOf = [](const auto& shape) {
    return std::accumulate(shape.begin(), shape.end(), sizeof(float),
                           std::multiplies<std::size_t>());
  };
  RoleExecutionContext ctx;
  ctx.role = step.unitId;
  ctx.sessionId = "wan-official-vae-conv-probe";
  ctx.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(
    std::vector<NativeExecutionStepSpec>{step});
  ctx.activeStep = std::shared_ptr<const NativeExecutionStepSpec>(ctx.roleSteps,
                                                                 &ctx.roleSteps->front());
  for (const auto& slot : step.inputSlots) {
    BOOST_REQUIRE_EQUAL(slot.dtype, "float32");
    auto shape = shapeOf(slot);
    const auto owner = std::make_shared<std::vector<std::uint8_t>>(bytesOf(shape));
    auto local = std::make_shared<LocalTensorBundle>();
    local->owner = owner;
    local->tensors.push_back({slot.slotId, TensorElementType::Float32, std::move(shape),
                              {owner->data(), owner->size()}});
    TensorBundle bundle;
    bundle.name = slot.slotId;
    bundle.expectedBytes = tensorBundleWireSize(*local);
    bundle.localTensors = std::move(local);
    ctx.stepInputsBySlot.emplace(slot.slotId, std::move(bundle));
  }
  RegistryNativeModelRunnerFactory factory;
  if (variant == "default-arena") {
    // Run the identical synthetic entries with only the CPU allocator policy changed.
    factory.registerBackend("onnxruntime", [](NativeModelRunnerSpec child) {
      child.metadata.erase("disableCpuMemArena");
      return std::make_shared<OnnxRuntimeModelRunner>(std::move(child));
    });
  }
  else {
    registerOnnxRuntimeBackend(factory);
  }
  sampleMemory("before-create");
  std::vector<std::shared_ptr<NativeModelRunner>> runners;
  for (unsigned i = 0; i < entryCount; ++i) {
    runners.push_back(factory.create(spec));
    sampleMemory(i == 0 ? "after-create" : "after-create-second");
  }
  for (const auto& runner : runners) {
    const auto started = std::chrono::steady_clock::now();
    const auto results = runner->run(ctx);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    BOOST_TEST_MESSAGE("VAE Conv entry " << step.stepId << " execution_s=" << seconds.count());
    sampleMemory("after-execute");
    BOOST_REQUIRE_EQUAL(results.size(), step.outputSlots.size());
    for (const auto& slot : step.outputSlots) {
      const auto values = decodeTensorBundleViews(results.at(slot.slotId));
      BOOST_REQUIRE_EQUAL(values.size(), 1U);
      const auto shape = shapeOf(slot);
      BOOST_CHECK(values.front().elementType == TensorElementType::Float32);
      BOOST_CHECK(values.front().shape == shape);
      BOOST_REQUIRE_EQUAL(values.front().payload.size(), bytesOf(shape));
      BOOST_CHECK(std::all_of(values.front().payload.begin(), values.front().payload.end(),
                              [](std::uint8_t byte) { return byte == 0; }));
    }
  }
  runners.clear();
  sampleMemory("after-release");
}

std::shared_ptr<const NativeCanonicalByteRangeSource>
writeFixtureSafeTensors(const std::filesystem::path& file, const std::string& header,
                        const std::vector<float>& payload, const NativeAssemblyControl& control)
{
  std::string wire(8, '\0');
  for (std::size_t i = 0; i < wire.size(); ++i)
    wire[i] = static_cast<char>((static_cast<std::uint64_t>(header.size()) >> (8 * i)) & 0xff);
  wire += header;
  wire.append(reinterpret_cast<const char*>(payload.data()), payload.size() * sizeof(float));
  std::ofstream out(file, std::ios::binary);
  out << wire;
  out.close();
  BOOST_REQUIRE(out.good());
  return openNativeCanonicalInitializerFile(file.string(), wire.size(),
                                            nativePlanningDigest(wire), control);
}

NativeJson
describeCheckpointFile(const std::filesystem::path& file)
{
  std::ifstream input(file, std::ios::binary);
  BOOST_REQUIRE(input.good());
  const std::string bytes(std::istreambuf_iterator<char>(input), {});
  return {{"file", file.filename().string()}, {"bytes", bytes.size()},
          {"digest", nativePlanningDigest(bytes)}};
}

NativeJson
offlineCheckpointRequest(const std::string& component, const std::filesystem::path& shard,
                         const std::vector<NativeStageLayout>& stages, NativeJson geometry,
                         const NativeAssemblyControl& control)
{
  const auto model = ndnsf::di::fixture::completeModel({
    component, nativePlanningDigest("fixture/content"), nativePlanningDigest("fixture/model"),
    nativePlanningDigest("fixture/graph"), "onnx", "fp32", "wan", "1"});
  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("fixture/profile"), nativePlanningDigest("fixture/assembler"),
    "fixture-ort", "fp32", "none", "native", "right", "wan",
    control.maxSourceBytes, control.maxAssembledBytes, 65536};
  auto stageRows = NativeJson::array();
  std::map<std::string, NativeRoleResourceRequirement> roles;
  for (const auto& stage : stages) {
    stageRows.push_back({{"id", stage.stageId}, {"roles", stage.unitRoleIds}});
    for (const auto& role : stage.unitRoleIds)
      roles.emplace(role, NativeRoleResourceRequirement{
        model.adapter.backends, control.maxSourceBytes, control.maxAssembledBytes, 0,
        4096, 4096, 1.0});
  }
  auto catalog = makeCatalogTemplate(model, profile, "/fixture/wan/source",
    nativePlanningDigest("fixture/source"), "/fixture/wan/package", "/fixture/wan", roles);
  return {{"schema", "ndnsf-di-wan-checkpoint-preparation-v1"}, {"component", component},
          {"shards", {{shard.filename().string(), describeCheckpointFile(shard)}}},
          {"stages", std::move(stageRows)}, {"geometry", std::move(geometry)},
          {"max_exact_bundle_bytes", 32U << 20}, {"catalog", std::move(catalog)}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(WanSourceLayout)

BOOST_AUTO_TEST_CASE(SharedVideoFixtureUsesTheSameSourcesAndCompatiblePortsInAllLayouts)
{
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-video-fixture");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  const std::filesystem::path directory(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&directory) {
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);
  };
  const auto root = directory / "workload";
  const auto manifest = videoFixture::writeWanVideoFixture(
    root, ndn::Name("/fixture/wan/requester"), ndn::Name("/fixture/wan/infer"), "fixture-epoch");
  const auto& modes = manifest.at("modes");
  BOOST_REQUIRE_EQUAL(modes.size(), 4);
  const auto read = [] (const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    BOOST_REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  for (const auto& [name, mode] : modes.items()) {
    for (const auto& [component, row] : mode.items()) {
      const auto& baseline = modes.at("tp").at(component);
      const auto catalog = root / row.at("catalog").get<std::string>();
      const auto source = nativeParseJson(read(catalog)).at("source");
      const auto graph = read(catalog.parent_path() / source.at("file").get<std::string>());
      BOOST_CHECK_EQUAL(nativePlanningDigest(graph), baseline.at("graph_digest").get<std::string>());
      if (source.contains("initializer_file")) {
        const auto sidecar = read(catalog.parent_path() /
                                  source.at("initializer_file").get<std::string>());
        BOOST_CHECK_EQUAL(nativePlanningDigest(sidecar),
                          baseline.at("initializer_digest").get<std::string>());
      }
      BOOST_CHECK(row.at("package_digest") == baseline.at("package_digest"));
      const auto& stages = row.at("stages");
      if (name == "tp") {
        BOOST_REQUIRE_EQUAL(stages.size(), 1);
        BOOST_CHECK_EQUAL(stages.at(0).size(), 2);
      }
      else {
        BOOST_REQUIRE_EQUAL(stages.size(), 2);
        BOOST_CHECK_EQUAL(stages.at(0).size(), component == "dit" && name == "tp-pp" ? 2 : 1);
        BOOST_CHECK_EQUAL(stages.at(1).size(), component == "dit" && name == "pp-tp" ? 2 : 1);
      }
    }
  }

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-video-fixture");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetInterOpNumThreads(1);
  const auto open = [&] (const std::string& component) {
    const auto location = root / modes.at("tp").at(component).at("catalog").get<std::string>();
    const auto source = nativeParseJson(read(location)).at("source");
    return Ort::Session(env, (location.parent_path() / source.at("file").get<std::string>()).c_str(),
                        options);
  };
  auto encoder = open("umt5"), dit = open("dit"), decoder = open("vae");
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<std::int64_t, 5> latentShape{1, 2, 3, 4, 4};
  const auto reference = videoFixture::referenceWanVideo();
  BOOST_REQUIRE_EQUAL(manifest.at("seed").get<int>(), 271);
  BOOST_REQUIRE_EQUAL(manifest.at("shift").get<double>(), 3.0);
  BOOST_REQUIRE_EQUAL(manifest.at("guidance").get<double>(), 2.0);
  BOOST_REQUIRE_EQUAL(manifest.at("steps").get<std::size_t>(), reference.steps.size());
  std::map<std::string, double> maximumErrors;
  const auto compare = [&] (const std::string& phase, ndn::span<const float> actual,
                             const std::vector<double>& expected,
                             double absolute, double relative) {
    BOOST_TEST_CONTEXT(phase) {
      BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
      for (std::size_t i = 0; i < expected.size(); ++i) {
        const double difference = std::abs(actual[i] - expected[i]);
        maximumErrors[phase] = std::max(maximumErrors[phase], difference);
        BOOST_CHECK_MESSAGE(std::isfinite(actual[i]) &&
                            difference <= absolute + relative * std::abs(expected[i]),
                            "element " << i << ": " << actual[i] << " vs " << expected[i]
                            << ", absolute error " << difference);
      }
    }
  };

  const std::array<std::string, 2> branches{"positive", "negative"};
  const std::array<std::vector<std::int64_t>, 2> referenceIds{{{2, 0, 1}, {1, 0, 0}}};
  const std::array<std::int64_t, 2> referenceLengths{3, 1};
  std::array<std::vector<float>, 2> contexts;
  for (std::size_t branch = 0; branch < branches.size(); ++branch) {
    const auto& prompt = manifest.at(branches[branch] + "_tokens");
    auto ids = prompt.at("ids").get<std::vector<std::int64_t>>();
    const auto valid = prompt.at("valid_tokens").get<std::int64_t>();
    BOOST_REQUIRE(ids == referenceIds[branch]);
    BOOST_REQUIRE_EQUAL(valid, referenceLengths[branch]);
    std::array<bool, 3> keyMask{};
    std::fill_n(keyMask.begin(), valid, true);
    const std::array<std::int64_t, 2> textShape{1, 3};
    const std::array<Ort::Value, 2> textInput{
      Ort::Value::CreateTensor<std::int64_t>(memory, ids.data(), ids.size(), textShape.data(), 2),
      Ort::Value::CreateTensor<bool>(memory, keyMask.data(), keyMask.size(), textShape.data(), 2)};
    const std::array<const char*, 2> textNames{"token-ids", "key-mask"};
    const char* contextName = "text-context";
    const auto context = encoder.Run(Ort::RunOptions{}, textNames.data(), textInput.data(),
                                     textInput.size(), &contextName, 1);
    const auto info = context.front().GetTensorTypeAndShapeInfo();
    BOOST_REQUIRE((info.GetShape() == std::vector<std::int64_t>{1, valid, 2}));
    BOOST_REQUIRE_EQUAL(info.GetElementType(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    const auto* data = context.front().GetTensorData<float>();
    contexts[branch].assign(data, data + info.GetElementCount());
    compare("text/" + branches[branch], contexts[branch], reference.textContexts[branch],
            2e-6, 0);
  }

  WanUniPc sampler(reference.steps.size(), 3.0, reference.initialLatent);
  const std::array<unsigned, 3> predictorOrders{1, 2, 1}, correctorOrders{0, 1, 2};
  const std::array<const char*, 4> names{"latent", "context", "timestep", "video-valid"};
  const std::array<std::int64_t, 1> timeShape{1};
  const std::array<std::int64_t, 4> maskShape{1, 1, 1, 12};
  std::array<bool, 12> videoMask;
  videoMask.fill(true);
  for (std::size_t step = 0; step < reference.steps.size(); ++step) {
    auto timestep = sampler.timestep();
    BOOST_REQUIRE_EQUAL(timestep, reference.steps[step].timestep);
    const auto sampled = sampler.latent();
    std::vector<float> latent(sampled.begin(), sampled.end());
    std::array<std::vector<float>, 2> predictions;
    for (std::size_t branch = 0; branch < branches.size(); ++branch) {
      auto& context = contexts[branch];
      const std::array<std::int64_t, 3> contextShape{1, referenceLengths[branch], 2};
      const std::array<Ort::Value, 4> inputs{
        Ort::Value::CreateTensor<float>(memory, latent.data(), latent.size(),
                                       latentShape.data(), latentShape.size()),
        Ort::Value::CreateTensor<float>(memory, context.data(), context.size(),
                                       contextShape.data(), contextShape.size()),
        Ort::Value::CreateTensor<std::int64_t>(memory, &timestep, 1, timeShape.data(), 1),
        Ort::Value::CreateTensor<bool>(memory, videoMask.data(), videoMask.size(),
                                      maskShape.data(), maskShape.size())};
      const char* predictionName = "prediction";
      const auto result = dit.Run(Ort::RunOptions{}, names.data(), inputs.data(), inputs.size(),
                                  &predictionName, 1);
      const auto info = result.front().GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      BOOST_REQUIRE(std::equal(shape.begin(), shape.end(), latentShape.begin(), latentShape.end()));
      BOOST_REQUIRE_EQUAL(info.GetElementType(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
      const auto* data = result.front().GetTensorData<float>();
      predictions[branch].assign(data, data + info.GetElementCount());
      compare("denoise/" + std::to_string(step) + "/" + branches[branch], predictions[branch],
              reference.steps[step].predictions[branch], 2e-6, 2e-6);
    }
    BOOST_CHECK(predictions[0] != predictions[1]);
    const auto info = sampler.advance(step, predictions[0], predictions[1], 2.0F, [] {});
    BOOST_CHECK_EQUAL(info.predictorOrder, predictorOrders[step]);
    BOOST_CHECK_EQUAL(info.correctorOrder, correctorOrders[step]);
    compare("latent/" + std::to_string(step), sampler.latent(), reference.steps[step].latent,
            2e-6, 2e-6);
  }
  BOOST_REQUIRE(sampler.finished());

  const auto sampled = sampler.latent();
  std::vector<float> finalLatent(sampled.begin(), sampled.end());
  const auto decoderInput = Ort::Value::CreateTensor<float>(
    memory, finalLatent.data(), finalLatent.size(), latentShape.data(), latentShape.size());
  const char* latentName = "latent-clip";
  const char* rgbName = "rgb";
  const auto decoded = decoder.Run(Ort::RunOptions{}, &latentName, &decoderInput, 1, &rgbName, 1);
  const auto rgb = decoded.front().GetTensorTypeAndShapeInfo();
  BOOST_REQUIRE((rgb.GetShape() == std::vector<std::int64_t>{1, 3, 9, 32, 32}));
  const auto encodedBytes = tensorBundleEncodedSize(
    {{std::string_view(rgbName).size(), rgb.GetShape().size(),
      rgb.GetElementCount() * sizeof(float)}});
  BOOST_CHECK_GT(encodedBytes, 4096U);
  for (const auto& mode : modes) {
    const auto& row = mode.at("vae");
    const auto catalog = nativeParseJson(read(root / row.at("catalog").get<std::string>()));
    BOOST_CHECK_LE(encodedBytes, catalog.at("max_payload_bytes").get<std::size_t>());
  }
  BOOST_REQUIRE_EQUAL(rgb.GetElementType(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
  compare("decode", {decoded.front().GetTensorData<float>(), rgb.GetElementCount()}, reference.rgb,
          1e-6, 0);
  for (const auto& [phase, error] : maximumErrors)
    BOOST_TEST_MESSAGE(phase << " max absolute error " << error);
}

BOOST_AUTO_TEST_CASE(Wan21DitCheckpointBindsAllBlocksAndRunsHybridEntries)
{
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-dit-checkpoint");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&scratch) {
    std::error_code cleanup;
    std::filesystem::remove_all(scratch, cleanup);
  };
  const std::filesystem::path directory(scratch.data());
  constexpr auto assemblyTimeout = std::chrono::seconds(90);
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + assemblyTimeout, [] {}, 32U << 20, 32U << 20};
  NativeJson header = NativeJson::object();
  std::vector<float> payload;
  for (const auto& [name, tensor] : videoFixture::makeDitCheckpointWeights()) {
    const auto offset = payload.size() * sizeof(float);
    payload.insert(payload.end(), tensor.float_data().begin(), tensor.float_data().end());
    header[name] = {{"dtype", "F32"},
                    {"shape", std::vector<std::int64_t>(tensor.dims().begin(),
                                                        tensor.dims().end())},
                    {"data_offsets", {offset, payload.size() * sizeof(float)}}};
  }
  const auto load = [&](const std::string& name, const NativeJson& manifest) {
    return WanCheckpoint({{name, writeFixtureSafeTensors(
      directory / name, nativeCanonicalJson(manifest), payload, control)}});
  };
  const NativeStageLayout tp{"dit", nativePlanningDigest("fixture/dit/tp"),
                             {"/dit/first", "/dit/second"}};
  const NativeStageLayout serial{"prefix", nativePlanningDigest("fixture/dit/pp"), {"/dit/serial"}};
  const WanDenoiserGeometry shape{{3, 2, 2, 12}, 4, 3, 3, 1, 64U << 10, 64U << 10};
  auto wrong = header;
  wrong["blocks.3.norm3.bias"] = wrong.at("blocks.2.norm3.bias");
  wrong.erase("blocks.2.norm3.bias");
  BOOST_CHECK_THROW(lowerWan21Denoiser(load("wrong.safetensors", wrong), shape, tp),
                    std::invalid_argument);
  auto checkpoint = lowerWan21Denoiser(load("denoiser.safetensors", header), shape, tp);
  BOOST_REQUIRE_EQUAL(checkpoint.layout.blockEnds.size(), 3);
  BOOST_REQUIRE_EQUAL(checkpoint.initializerRangeSource->size(), payload.size() * sizeof(float));
  const auto hybrid = makeVideoDenoiserStagedPlan(checkpoint.layout, 1, {serial, tp});
  const auto reverse = makeVideoDenoiserStagedPlan(checkpoint.layout, 1, {tp, serial});
  const NativeStageLayout suffix{"suffix", nativePlanningDigest("fixture/dit/pp2"), {"/dit/last"}};
  const auto pp = makeVideoDenoiserStagedPlan(checkpoint.layout, 1, {serial, suffix});
  const auto shard = directory / "denoiser.safetensors";
  auto request = offlineCheckpointRequest("dit", shard, {serial, tp},
    {{"grid", {{"frames", 3}, {"height", 2}, {"width", 2}, {"padded_tokens", 12}}},
     {"heads", 4}, {"text_tokens", 3}, {"video_key_tile", 3}, {"text_key_tile", 1},
     {"score_bytes", 64U << 10}, {"rotary_bytes", 64U << 10}, {"prefix_blocks", 1}}, control);
  NativeJson weightMap = NativeJson::object();
  for (const auto& [name, row] : header.items())
    weightMap[name] = shard.filename().string();
  const auto indexFile = directory / "index.json";
  std::ofstream(indexFile) << nativeCanonicalJson(NativeJson{
    {"metadata", {{"total_size", payload.size() * sizeof(float)}}},
    {"weight_map", std::move(weightMap)}});
  request["index"] = describeCheckpointFile(indexFile);
  auto tooSmall = request;
  tooSmall["catalog"]["splitter"]["resources_by_role"][serial.unitRoleIds[0]]["weight_bytes"] = 1;
  BOOST_CHECK_EXCEPTION(prepareWanCheckpointComponent(
    tooSmall, directory, directory / "underbudget", control), std::invalid_argument,
    [](const auto& error) {
      return std::string_view(error.what()) == "native role weight budget is below its consumed initializers";
    });
  BOOST_CHECK(!std::filesystem::exists(directory / "underbudget"));
  const auto offline = prepareWanCheckpointComponent(request, directory, directory / "offline",
                                                     control);
  BOOST_CHECK(std::filesystem::exists(offline));
  auto baselineControl = control;
  baselineControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  const auto baselineRequest = offlineCheckpointRequest("dit", shard,
    {{"single", {}, {"provider"}}}, request.at("geometry"), baselineControl);
  BOOST_CHECK(std::filesystem::is_regular_file(prepareWanCheckpointComponent(
    baselineRequest, directory, directory / "single", baselineControl)));
  NativeCanonicalSource source;
  const auto model = checkpoint.layout.model.SerializeAsString();
  source.modelBytes.assign(model.begin(), model.end());
  source.initializerRangeSource = std::move(checkpoint.initializerRangeSource);
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);

  const auto reference = videoFixture::referenceWanVideo(1e-6);
  const auto& step = reference.steps.front();
  auto latent = reference.initialLatent;
  const auto& context = reference.textContexts.front();
  std::vector<float> contextFp32(context.begin(), context.end());
  std::int64_t timestep = step.timestep;
  std::array<bool, 12> mask;
  mask.fill(true);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-checkpoint-dit");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const auto makeTensor = [&](auto* data, std::size_t cells, std::vector<std::int64_t> dims) {
    using Scalar = std::remove_pointer_t<decltype(data)>;
    return Ort::Value::CreateTensor<Scalar>(memory, data, cells, dims.data(), dims.size());
  };
  for (const auto* candidate : {&std::as_const(checkpoint.layout.tensorParallel), &pp,
                                &hybrid, &reverse}) {
    std::map<std::string, Ort::Value> values;
    values.emplace("latent", makeTensor(latent.data(), latent.size(), {1, 2, 3, 4, 4}));
    values.emplace("timestep", makeTensor(&timestep, 1, {1}));
    values.emplace("context", makeTensor(contextFp32.data(), contextFp32.size(), {1, 3, 2}));
    values.emplace("video-valid", makeTensor(mask.data(), mask.size(), {1, 1, 1, 12}));
    auto layoutControl = control;
    layoutControl.deadline = std::chrono::steady_clock::now() + assemblyTimeout;
    runCompiledEntriesFixture(*candidate, source, values, env, options, memory, layoutControl, true);
    const auto& prediction = values.at("prediction");
    BOOST_REQUIRE_EQUAL(prediction.GetTensorTypeAndShapeInfo().GetElementCount(), latent.size());
    const auto* actual = prediction.GetTensorData<float>();
    for (std::size_t i = 0; i < step.predictions[0].size(); ++i)
      BOOST_CHECK_SMALL(actual[i] - step.predictions[0][i], 5e-6);
  }

  if (const auto* root = std::getenv("NDNSF_DI_WAN_CHECKPOINT_METADATA_ROOT")) {
    const std::filesystem::path metadataRoot(root);
    const auto read = [](const std::filesystem::path& path) {
      std::ifstream stream(path, std::ios::binary);
      BOOST_REQUIRE(stream.good());
      return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto recipe = nativeParseJson(read(metadataRoot / "recipe.json"));
    const auto index = read(metadataRoot / "dit-index.json");
    BOOST_REQUIRE_EQUAL(nativePlanningDigest(index),
                        "sha256:" + recipe.at("sources").at(0).at("sha256").get<std::string>());
    WanCheckpointShards sources;
    for (const auto& row : recipe.at("safetensors_header_ranges")) {
      const auto name = row.at("file").get<std::string>();
      auto data = read(metadataRoot / "headers" / (name + ".header"));
      BOOST_REQUIRE_EQUAL(nativePlanningDigest(data), "sha256:" + row.at("sha256").get<std::string>());
      sources.emplace(name, std::make_shared<HeaderSource>(
        std::move(data), row.at("full_bytes").get<std::uint64_t>()));
    }
    const auto actual = lowerWan21Denoiser(WanCheckpoint(std::move(sources), index),
      {{21, 30, 52, 32760}, 40, 512, 4, 4, 32U << 20, 128U << 20}, tp);
    BOOST_REQUIRE_EQUAL(actual.layout.blockEnds.size(), 40);
    BOOST_CHECK_EQUAL(actual.initializerRangeSource->size(), 57153966336ULL);
    BOOST_CHECK_THROW(actual.initializerRangeSource->read(0, 4), std::out_of_range);
    const auto stage = makeVideoDenoiserStagedPlan(actual.layout, 20, {serial, tp});
    BOOST_CHECK_EQUAL(stage.plan.roles.size(), 3);
    if (std::getenv("NDNSF_DI_WAN_MATERIAL_PREFLIGHT"))
      checkOfficialDitStoredPublication(actual.layout.model, actual.layout.tensorParallel,
                                        actual.initializerRangeSource->size(), 56U << 20);
    for (const auto budget : {32U << 20, 56U << 20}) {
      const auto tiled = tileWanExactActivations(actual.layout.model,
                                                actual.layout.tensorParallel, budget);
      auto plan = tiled.execution.plan;
      for (auto& step : plan.steps) {
        if (step.operation != "CALL_SUBGRAPH") continue;
        step.onnxNodeIndices = tiled.execution.entries.at(step.artifactEntry).nodeIndices;
        std::sort(step.onnxNodeIndices.begin(), step.onnxNodeIndices.end());
      }
      const auto steps = nativeCanonicalJson(nativeV2StepTemplateJson(plan));
      const auto edges = nativeCanonicalJson(nativeDependenciesJson(plan.dependencies));
      const auto decoded = nativeCanonicalOnnxStepPlanFromJson(
        nativeParseJson(steps), nativeParseJson(edges));
      BOOST_CHECK_EQUAL(decoded.steps.size(), plan.steps.size());
      BOOST_CHECK_EQUAL(decoded.dependencies.size(), plan.dependencies.size());
      if (budget == (56U << 20)) {
        auto materialTemplate = tiled.model;
        auto& templateGraph = *materialTemplate.mutable_graph();
        templateGraph.clear_node();
        templateGraph.clear_initializer();
        templateGraph.clear_sparse_initializer();
        templateGraph.clear_quantization_annotation();
        const auto templateBytes = materialTemplate.ByteSizeLong();
        BOOST_TEST_MESSAGE("Official DiT canonical template bytes " << templateBytes);
        BOOST_CHECK_LE(templateBytes, NativeCanonicalMaterialBundleMaxBytes);
        const auto weights = checkOfficialTpRoleWeights(
          "DiT", tiled.model.graph(), decoded, actual.initializerRangeSource->size());
        for (const auto& [role, usage] : weights)
          BOOST_CHECK_EQUAL(usage.entryBytes, usage.uniqueBytes);
        const NativeProviderAssignment assignment{{
          {"/dit/first", "/fixture/provider-a"}, {"/dit/second", "/fixture/provider-b"}}};
        auto firstRole = roleSpecFor(decoded, "/dit/first", "/fixture/wan/width", assignment);
        auto secondRole = roleSpecFor(decoded, "/dit/second", "/fixture/wan/width", assignment);
        BOOST_CHECK_EQUAL(firstRole.dependencyReadWidth(), 2335U);
        BOOST_CHECK_EQUAL(secondRole.dependencyReadWidth(), 2374U);
        firstRole.stepScopedDependencyReads = true;
        secondRole.stepScopedDependencyReads = true;
        BOOST_CHECK_EQUAL(firstRole.dependencyReadWidth(), 6U);
        BOOST_CHECK_EQUAL(secondRole.dependencyReadWidth(), 12U);
        auto catalog = request.at("catalog");
        auto& splitter = catalog.at("splitter");
        splitter["step_template_v2"] = nativeParseJson(steps);
        splitter["dependencies"] = nativeParseJson(edges);
        const auto configuration = nativeCanonicalJson(catalog);
        BOOST_TEST_MESSAGE("Official DiT TP catalog before NodeMap bytes " << configuration.size());
        BOOST_CHECK_GT(configuration.size(), 4U << 20);
        BOOST_CHECK_LT(configuration.size(), NATIVE_REQUEST_CATALOG_MAX_BYTES);
        BOOST_CHECK_NO_THROW(nativeStoredModelConfigurationIdentity(configuration));
        if (const auto* path = std::getenv("NDNSF_DI_WAN_PROJECTION_PAYLOAD_OUT")) {
          std::ofstream output(path);
          output << R"({"steps":)" << steps << R"(,"dependencies":)" << edges << "}\n";
          BOOST_REQUIRE(output.good());
        }
      }
      std::map<std::pair<std::string, std::string>, const NativeStepSlotSpec*> outputs;
      for (const auto& step : plan.steps) {
        for (const auto& slot : step.outputSlots)
          outputs.emplace(std::make_pair(step.stepId, slot.slotId), &slot);
      }
      std::map<std::string, std::uint64_t> outboundBytes;
      std::set<std::tuple<std::string, std::string, std::string>> transfers;
      std::uint64_t repeatedBytes = 0;
      std::size_t repeatedTransfers = 0;
      for (const auto& edge : plan.dependencies) {
        const auto& slot = *outputs.at({edge.producerStepId, edge.producerSlotId});
        auto bytes = slot.dtype == "float32" ? sizeof(float) :
                     slot.dtype == "int64" ? sizeof(std::int64_t) : 1;
        for (const auto& dim : slot.shape) bytes *= std::get<std::int64_t>(dim);
        const auto payload = tensorBundleEncodedSize({{slot.slotId.size(), slot.shape.size(), bytes}});
        BOOST_CHECK_LE(payload, budget);
        outboundBytes[edge.producers.front()] += payload;
        if (!transfers.emplace(edge.producerStepId, edge.producerSlotId,
                               edge.consumers.front()).second) {
          repeatedBytes += payload;
          ++repeatedTransfers;
        }
      }
      BOOST_TEST_MESSAGE("Official DiT bytes " << actual.layout.model.ByteSizeLong()
                         << ", TP steps " << actual.layout.tensorParallel.plan.steps.size()
                         << ", TP dependencies " << actual.layout.tensorParallel.plan.dependencies.size());
      BOOST_TEST_MESSAGE("Official tiled DiT tile budget " << budget
                         << ", V2 steps " << plan.steps.size()
                         << ", dependencies " << plan.dependencies.size()
                         << ", step bytes " << steps.size() << ", dependency bytes " << edges.size());
      for (const auto& [role, bytes] : outboundBytes)
        BOOST_TEST_MESSAGE("Official tiled DiT " << role << " bundle bytes " << bytes);
      BOOST_TEST_MESSAGE("Official tiled DiT repeated same-role transfers " << repeatedTransfers
                         << ", bundle bytes " << repeatedBytes);
    }
  }
}

BOOST_AUTO_TEST_CASE(WanTokenizerPadsBothPromptsAndKeepsEosWhenTruncating)
{
  const std::string json = R"({
    "version":"1.0","truncation":null,"padding":null,"added_tokens":[],
    "normalizer":null,"pre_tokenizer":{"type":"WhitespaceSplit"},"decoder":null,
    "post_processor":{
      "type":"TemplateProcessing",
      "single":[{"Sequence":{"id":"A","type_id":0}},{"SpecialToken":{"id":"</s>","type_id":0}}],
      "pair":[{"Sequence":{"id":"A","type_id":0}},{"Sequence":{"id":"B","type_id":1}}],
      "special_tokens":{"</s>":{"id":"</s>","ids":[1],"tokens":["</s>"]}}
    },
    "model":{"type":"WordLevel","vocab":{"<pad>":0,"</s>":1,"<unk>":2,"云":3,"lake":4},
             "unk_token":"<unk>"}
  })";
  auto scratch = (std::filesystem::temp_directory_path() / "wan-tokenizer-XXXXXX").string();
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&) { std::filesystem::remove_all(scratch); };
  const auto file = std::filesystem::path(scratch) / "tokenizer.json";
  std::ofstream(file, std::ios::binary) << json;
  const auto digest = nativePlanningDigest(json);
  const auto prompts = tokenizeWanPrompts(file.string(), digest, 4,
    u8"  云\u00a0\t\u3000lake\n ", "lake lake lake lake lake");
  BOOST_CHECK(prompts[0].ids == (std::vector<std::int64_t>{3, 4, 1, 0}));
  BOOST_CHECK_EQUAL(prompts[0].validTokens, 3);
  BOOST_CHECK(prompts[1].ids == (std::vector<std::int64_t>{4, 4, 4, 1}));
  BOOST_CHECK_EQUAL(prompts[1].validTokens, 4);
  const auto minimum = tokenizeWanPrompts(file.string(), digest, 1, "lake", "");
  for (const auto& prompt : minimum) {
    BOOST_CHECK(prompt.ids == std::vector<std::int64_t>{1});
    BOOST_CHECK_EQUAL(prompt.validTokens, 1);
  }
  BOOST_CHECK_THROW(tokenizeWanPrompts(file.string(), digest, 513, "", ""),
                    std::invalid_argument);
  BOOST_CHECK_THROW(tokenizeWanPrompts(file.string(), digest, 4, "\xff", ""),
                    std::runtime_error);
  std::ofstream(file, std::ios::binary | std::ios::app) << ' ';
  BOOST_CHECK_EXCEPTION(tokenizeWanPrompts(file.string(), digest, 4, "lake", ""),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "tokenizer digest mismatch";
    });

  const auto tokenizerPath = std::getenv("NDNSF_DI_WAN_TOKENIZER_FILE");
  const auto referencePath = std::getenv("NDNSF_DI_WAN_TOKEN_REFERENCE");
  if (tokenizerPath || referencePath) {
    BOOST_REQUIRE(tokenizerPath && referencePath);
    std::ifstream referenceFile(referencePath);
    BOOST_REQUIRE(referenceFile.good());
    const auto reference = nativeParseJson(
      std::string(std::istreambuf_iterator<char>(referenceFile), {}));
    BOOST_REQUIRE_EQUAL(reference.at("cases").size(), 2);
    const auto pinnedDigest = reference.at("tokenizer_digest").get<std::string>();
    for (const auto& row : reference.at("cases")) {
      const auto encoded = tokenizeWanPrompts(tokenizerPath, pinnedDigest,
        row.at("slots").get<std::size_t>(),
        row.at("positive").at("text").get<std::string>(),
        row.at("negative").at("text").get<std::string>());
      const std::array<std::string, 2> branches{"positive", "negative"};
      for (std::size_t i = 0; i < branches.size(); ++i) {
        const auto& expected = row.at(branches[i]);
        BOOST_CHECK_EQUAL(encoded[i].validTokens, expected.at("valid").get<std::size_t>());
        BOOST_CHECK(encoded[i].ids == expected.at("ids").get<std::vector<std::int64_t>>());
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(UniPcMatchesShiftedFlowTrajectoryAndRejectsPartialAdvances)
{
  const std::vector<float> initial{0.625f, -1.25f, 3};
  const std::array<std::array<float, 3>, 4> positive{{
    {1, -0.25f, 1.5f}, {-0.75f, 0.5f, -3}, {0.5f, 1, -0.25f}, {-0.75f, 0.25f, 1.5f},
  }};
  const std::array<std::array<float, 3>, 4> negative{{
    {0.5f, 0.25f, 1}, {0.25f, -1, -1}, {0.125f, -1.5f, 0.5f}, {0.5f, -0.5f, -1.5f},
  }};
  constexpr float GUIDANCE = 3.5f;
  const auto active = [] {};
  const auto check = [&] (std::size_t steps, const auto& timesteps, const auto& expected) {
    WanUniPc solver(steps, 5, initial);
    for (std::size_t step = 0; step < steps; ++step) {
      BOOST_CHECK_EQUAL(solver.stepIndex(), step);
      BOOST_CHECK_EQUAL(solver.timestep(), timesteps[step]);
      const auto before = solver.latent();
      BOOST_CHECK_THROW(solver.advance(step + 1, positive[step], negative[step], GUIDANCE, active),
                        std::invalid_argument);
      BOOST_CHECK_THROW(solver.advance(step, positive[step], {}, GUIDANCE, active),
                        std::invalid_argument);
      auto invalid = negative[step];
      invalid.back() = std::numeric_limits<float>::infinity();
      BOOST_CHECK_THROW(solver.advance(step, positive[step], invalid, GUIDANCE, active),
                        std::invalid_argument);
      int fenceChecks = 0;
      BOOST_CHECK_THROW(solver.advance(step, positive[step], negative[step], GUIDANCE, [&] {
        if (++fenceChecks == 3) throw std::runtime_error("cancelled before latent commit");
      }), std::runtime_error);
      BOOST_CHECK_EQUAL(fenceChecks, 3);
      BOOST_CHECK(solver.latent() == before);
      BOOST_CHECK_EQUAL(solver.stepIndex(), step);

      const auto info = solver.advance(step, positive[step], negative[step], GUIDANCE, active);
      BOOST_CHECK_EQUAL(info.predictorOrder, step == 0 || step + 1 == steps ? 1 : 2);
      BOOST_CHECK_EQUAL(info.correctorOrder, std::min<std::size_t>(step, 2));
      for (std::size_t i = 0; i < initial.size(); ++i)
        BOOST_CHECK_SMALL(solver.latent()[i] - expected[step][i], 3e-6);
    }
    BOOST_CHECK(solver.finished());
    BOOST_CHECK_THROW(solver.timestep(), std::logic_error);
    BOOST_CHECK_THROW(solver.advance(steps, positive[0], negative[0], GUIDANCE, active),
                      std::invalid_argument);
  };
  // FP64 reference uses numerical quadrature of the two BH2 moments, then
  // solves the two interpolation weights independently of the native recurrence.
  const std::array<std::array<double, 3>, 3> threeSteps{{
    {0.4203466475, -1.1135644317, 2.7498681247},
    {1.3516467276, -2.2532730771, 4.8912181672},
    {0.3253013070, -7.4296238942, 6.4084244412},
  }};
  const std::array<std::array<double, 3>, 4> fourSteps{{
    {0.4842976183, -1.1561984122, 2.8280304223},
    {1.0203394753, -1.8054212162, 4.0474583863},
    {0.0438852873, -3.7418121322, 3.6387033785},
    {2.4645488602, -5.0692728012, -1.9834829842},
  }};
  check(3, std::array<int, 3>{999, 908, 713}, threeSteps);
  check(4, std::array<int, 4>{999, 937, 833, 624}, fourSteps);
  BOOST_CHECK_THROW(WanUniPc(0, 1, initial), std::invalid_argument);
  BOOST_CHECK_THROW(WanUniPc(3, 1e30, initial), std::invalid_argument);
  WanUniPc single(1, 1, initial);
  BOOST_CHECK_EQUAL(single.timestep(), 999);
  single.advance(0, positive[0], negative[0], GUIDANCE, active);
  BOOST_CHECK_SMALL(single.latent()[0] - -1.6227500290, 3e-6);
  BOOST_CHECK(single.finished());
}

BOOST_AUTO_TEST_CASE(UniPcMatchesOfficialWan22FiftyStepReferenceWithoutModelWeights)
{
  const auto path = ndnsf::test::testFixturePath("wan-video/wan22-sampler-reference.json");
  BOOST_REQUIRE(std::filesystem::file_size(path) <= (64U << 10));
  std::ifstream file(path);
  BOOST_REQUIRE(file.good());
  const auto reference = nativeParseJson(std::string(std::istreambuf_iterator<char>(file), {}));
  BOOST_REQUIRE_EQUAL(reference.at("schema"), "ndnsf-di-wan22-sampler-reference-v1");
  const auto& rows = reference.at("steps");
  BOOST_REQUIRE_EQUAL(rows.size(), 50);
  const auto initial = reference.at("initial").get<std::vector<float>>();
  BOOST_REQUIRE_EQUAL(initial.size(), 3);
  WanUniPc solver(rows.size(), reference.at("shift").get<double>(), initial);
  const auto guidance = reference.at("guidance").get<float>();
  for (std::size_t step = 0; step < rows.size(); ++step) {
    const auto& row = rows.at(step);
    const auto positive = row.at("positive").get<std::vector<float>>();
    const auto negative = row.at("negative").get<std::vector<float>>();
    const auto expected = row.at("latent").get<std::vector<double>>();
    BOOST_REQUIRE_EQUAL(positive.size(), initial.size());
    BOOST_REQUIRE_EQUAL(negative.size(), initial.size());
    BOOST_REQUIRE_EQUAL(expected.size(), initial.size());
    BOOST_CHECK_EQUAL(solver.timestep(), row.at("timestep").get<std::int64_t>());
    const auto info = solver.advance(step, positive, negative, guidance, [] {});
    BOOST_CHECK_EQUAL(info.predictorOrder, step == 0 || step + 1 == rows.size() ? 1 : 2);
    BOOST_CHECK_EQUAL(info.correctorOrder, std::min<std::size_t>(step, 2));
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto actual = solver.latent()[i];
      BOOST_CHECK(std::isfinite(actual));
      BOOST_CHECK_LE(std::abs(actual - expected[i]), 2e-4 + 2e-4 * std::abs(expected[i]));
    }
  }
  BOOST_CHECK(solver.finished());
  BOOST_CHECK_EQUAL(solver.stepIndex(), 50);
}

BOOST_AUTO_TEST_CASE(ExactActivationTilesPreservePipelineTensorAndHybridNumerics)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(17);
  auto& graph = *source.mutable_graph();
  graph.set_name("activation-tile-fixture");
  const std::vector<std::int64_t> dims{1, 15, 4};
  const auto declare = [] (auto& value, const char* name, const auto& extents) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto extent : extents) tensor->mutable_shape()->add_dim()->set_dim_value(extent);
  };
  declare(*graph.add_input(), "X", dims);
  declare(*graph.add_output(), "Z", std::array<std::int64_t, 3>{1, 1, 1});
  const auto node = [&] (const char* op, const char* output,
                         std::initializer_list<const char*> inputs) {
    auto& value = *graph.add_node();
    value.set_op_type(op);
    value.add_output(output);
    for (const auto* name : inputs) value.add_input(name);
  };
  node("Identity", "H", {"X"});
  node("Add", "L", {"H", "H"});
  node("Neg", "Rneg", {"H"});
  node("Sub", "R", {"H", "Rneg"});
  node("Add", "Y", {"L", "R"});
  node("Identity", "W", {"Y"});
  node("ReduceSum", "Z", {"W"});
  const auto port = [] (const char* name, bool scalar = false) {
    return NativeAssemblyTensorContractV3{name, "float32",
      {std::int64_t{1}, std::int64_t{scalar ? 1 : 15}, std::int64_t{scalar ? 1 : 4}}};
  };
  const auto x = port("X"), h = port("H"), l = port("L"), r = port("R"), y = port("Y");
  const auto rneg = port("Rneg");
  const auto w = port("W");
  const auto z = port("Z", true);
  const NativeStageLayout tp{"tp", nativePlanningDigest("tp"), {"rank0", "rank1"}};
  const auto parallel = compileParallelEntries(graph, tp,
    {{"head", {{0}, {x}, {h}}}, {"left", {{1}, {h}, {l}}},
     {"right-neg", {{2}, {h}, {rneg}}}, {"right", {{3}, {h, rneg}, {r}}},
     {"publish", {{5}, {y}, {w}}}, {"tail", {{6}, {w}, {z}}}}, {"right-neg", "right"},
    {{4, {{{}, {l, r}, {y}}, false}}});
  const NativeStageLayout pp0{"prefix", nativePlanningDigest("prefix"), {"prefix/pp"}};
  const NativeStageLayout pp1{"suffix", nativePlanningDigest("suffix"), {"suffix/pp"}};
  const auto pp = makeContiguousEntryPlan(graph, parallel, {pp0, pp1}, 6);
  const auto ppTp = makeContiguousEntryPlan(graph, parallel, {pp0, tp}, 1);
  const auto tpPp = makeContiguousEntryPlan(graph, parallel, {tp, pp1}, 6);

  constexpr std::uint64_t BUDGET = 150;
  const auto noChange = tileWanExactActivations(source, parallel, 1U << 20);
  BOOST_CHECK_EQUAL(noChange.model.SerializeAsString(), source.SerializeAsString());
  BOOST_CHECK_THROW(tileWanExactActivations(source, parallel, 48), std::invalid_argument);

  std::vector<float> input(60);
  std::iota(input.begin(), input.end(), 1.0f);
  const auto expected = 4 * std::accumulate(input.begin(), input.end(), 0.0f);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-activation-tiles");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetInterOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  for (const auto* layout : {&parallel, &pp, &ppTp, &tpPp}) {
    const auto tiled = tileWanExactActivations(source, *layout, BUDGET);
    BOOST_CHECK(tiled.execution.plan.roles == layout->plan.roles);
    BOOST_CHECK_GT(tiled.execution.plan.dependencies.size(), layout->plan.dependencies.size());
    BOOST_CHECK_NO_THROW(tiled.execution.plan.validateSteps());
    std::set<std::tuple<std::string, std::string, std::string>> transfers;
    for (const auto& edge : tiled.execution.plan.dependencies) {
      BOOST_CHECK(transfers.emplace(edge.producerStepId, edge.producerSlotId,
                                    edge.consumers.front()).second);
      const auto producer = std::find_if(tiled.execution.plan.steps.begin(),
        tiled.execution.plan.steps.end(), [&](const auto& step) {
          return step.stepId == edge.producerStepId;
        });
      BOOST_REQUIRE(producer != tiled.execution.plan.steps.end());
      const auto output = std::find_if(producer->outputSlots.begin(), producer->outputSlots.end(),
        [&](const auto& slot) { return slot.slotId == edge.producerSlotId; });
      BOOST_REQUIRE(output != producer->outputSlots.end());
      BOOST_CHECK_EQUAL(output->slotId, edge.consumerSlotId);
      std::uint64_t bytes = sizeof(float);
      for (const auto& dim : output->shape) bytes *= std::get<std::int64_t>(dim);
      BOOST_CHECK_LE(tensorBundleEncodedSize({{output->slotId.size(), output->shape.size(), bytes}}),
                     BUDGET);
      BOOST_CHECK(output->slotId.find("__ndnsf_wan_wire/") == 0);
    }
    const auto sums = [](const auto& steps) {
      return std::count_if(steps.begin(), steps.end(),
                           [](const auto& step) { return step.operation == "SUM"; });
    };
    if (sums(layout->plan.steps) != 0)
      BOOST_CHECK_GT(sums(tiled.execution.plan.steps), sums(layout->plan.steps));
    auto wire = tiled.model.SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    auto value = Ort::Value::CreateTensor<float>(
      memory, input.data(), input.size(), dims.data(), dims.size());
    const std::array<const char*, 1> names{"X"}, results{"Z"};
    const auto result = session.Run(Ort::RunOptions{}, names.data(), &value, 1,
                                    results.data(), 1);
    BOOST_REQUIRE_EQUAL(result.size(), 1);
    BOOST_CHECK_EQUAL(result.front().GetTensorData<float>()[0], expected);

    NativeCanonicalSource canonical;
    canonical.modelBytes.assign(wire.begin(), wire.end());
    const NativeAssemblyControl control{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
    NativeInspectedModel model;
    auto descriptor = ndnsf::di::fixture::completeModel({
      "activation-tiles", nativePlanningDigest("tile-source"), nativePlanningDigest("tile-model"),
      nativePlanningDigest("graph-probe"), "onnx", "fp32", "wan", "1"});
    const auto inspection = inspectNativeOnnxSourceGraph(canonical, descriptor, control);
    descriptor.graphDigest = inspection.graph.graphDigest;
    model.descriptor = descriptor;
    model.graph = inspection.graph;
    model.canonicalSourceName = "/fixture/activation-tile-source";
    model.canonicalSourceBytes = canonical.modelBytes.size();
    model.canonicalSourceDigest = nativePlanningDigest(canonical.modelBytes.data(),
                                                       canonical.modelBytes.size());
    model.modelManifestDigest = nativePlanningDigest("tile-manifest");
    model.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
    model.canonicalInitializerDigest = inspection.canonicalIdentity.initializerDigest;
    std::map<std::string, NativeRoleResourceRequirement> resources;
    for (const auto& role : tiled.execution.plan.roles)
      resources.emplace(role, NativeRoleResourceRequirement{
        {"onnxruntime"}, 1024, 1024, 0, 1024, 1024, 1.0});
    const NativeRequestControl request{"/fixture/tiled-exact", 1, control.deadline, {}};
    const auto candidate = makeWanSplitCandidate(model, inspection, tiled.execution,
                                                "/fixture/wan-tiles", resources, request);
    BOOST_CHECK_EQUAL(candidate.resultEgressRole, layout->plan.steps.back().unitId);
    BOOST_CHECK_EQUAL(candidate.nodeRoles.size(), tiled.model.graph().node_size());
    const NativeRoleRecipeProfile profile{
      nativePlanningDigest("tiles-profile"), nativePlanningDigest("tiles-assembler"),
      "fixture-ort", "fp32", "none", "native", "none", "tiles", 4U << 20, 4U << 20, 1024};
    NativeCanonicalRolePreparer preparer(model, inspection, profile, control,
                                         makeWanMaterialNodeMap(inspection));
    BOOST_CHECK_EQUAL(preparer.prepare(model, candidate, request).size(),
                      tiled.execution.plan.roles.size());
    const auto catalogTemplate = makeCatalogTemplate(descriptor, profile,
      model.canonicalSourceName, model.modelManifestDigest, "/fixture/activation-tiles/package",
      "/fixture/wan-tiles", resources);
    auto package = compileWanComponent(source, *layout, BUDGET, {}, catalogTemplate, control);
    BOOST_CHECK(package.source.modelBytes == canonical.modelBytes);
    const auto catalog = NativeRequestCatalog::load(package.configurationJson,
                                                     std::move(package.source), control);
    BOOST_CHECK_EQUAL(catalog.planningCache->candidates.at(0).candidate.candidateDigest,
                      candidate.candidateDigest);
    if (layout == &parallel) {
      auto extraOutput = source;
      declare(*extraOutput.mutable_graph()->add_output(), "L", dims);
      auto invalid = compileWanComponent(std::move(extraOutput), *layout, 1U << 20, {},
                                         catalogTemplate, control);
      BOOST_CHECK_EXCEPTION(
        NativeRequestCatalog::load(invalid.configurationJson, std::move(invalid.source), control),
        std::invalid_argument, [] (const auto& error) {
          return std::string(error.what()) == "Wan component must declare exactly one graph output";
        });
      BOOST_CHECK_THROW(compileWanComponent(source, *layout, 48, {}, catalogTemplate, control),
                        std::invalid_argument);
      auto shortControl = control;
      shortControl.maxSourceBytes = 1;
      BOOST_CHECK_THROW(
        compileWanComponent(source, *layout, BUDGET, {}, catalogTemplate, shortControl),
        std::invalid_argument);
    }
    canonical.materialManifest = deriveNativeCanonicalMaterialManifest(canonical, control);
    std::map<std::string, Ort::Value> values;
    values.emplace("X", Ort::Value::CreateTensor<float>(
      memory, input.data(), input.size(), dims.data(), dims.size()));
    runCompiledEntriesFixture(tiled.execution, canonical, values, env, options, memory, control);
    BOOST_CHECK_EQUAL(values.at("Z").GetTensorData<float>()[0], expected);
  }

  // Keep the tiny operator topology but use real Wan 14B hidden geometry in
  // the sealed ports. No activation buffer or ORT session is created here.
  const auto geometry = planVideoGeometry(81, 480, 832, 5120);
  auto realSource = source;
  auto* inputShape = realSource.mutable_graph()->mutable_input(0)
    ->mutable_type()->mutable_tensor_type()->mutable_shape();
  inputShape->mutable_dim(1)->set_dim_value(geometry.videoTokens);
  inputShape->mutable_dim(2)->set_dim_value(5120);
  auto realLayout = parallel;
  const auto useRealShape = [&](auto& ports) {
    for (auto& tensor : ports) {
      if (tensor.shape != x.shape) continue;
      tensor.shape[1] = static_cast<std::int64_t>(geometry.videoTokens);
      tensor.shape[2] = std::int64_t{5120};
    }
  };
  for (auto& step : realLayout.plan.steps) {
    useRealShape(step.inputSlots);
    useRealShape(step.outputSlots);
  }
  for (auto& entry : realLayout.entries) {
    useRealShape(entry.second.inputs);
    useRealShape(entry.second.outputs);
  }
  auto metadata = tileWanExactActivations(std::move(realSource), std::move(realLayout), 32U << 20);
  BOOST_CHECK_EQUAL(metadata.execution.plan.dependencies.size(), 40U);
  for (auto& step : metadata.execution.plan.steps)
    if (step.operation == "CALL_SUBGRAPH")
      step.onnxNodeIndices = metadata.execution.entries.at(step.artifactEntry).nodeIndices;
  const auto steps = nativeV2StepTemplateJson(metadata.execution.plan);
  const auto dependencies = nativeDependenciesJson(metadata.execution.plan.dependencies);
  const auto parsed = nativeCanonicalOnnxStepPlanFromJson(steps, dependencies);
  BOOST_CHECK_EQUAL(parsed.steps.size(), metadata.execution.plan.steps.size());
  BOOST_CHECK_EQUAL(parsed.dependencies.size(), metadata.execution.plan.dependencies.size());
  BOOST_CHECK_LT(nativeCanonicalJson(steps).size() + nativeCanonicalJson(dependencies).size(),
                 4U << 20);
}

BOOST_AUTO_TEST_CASE(FirstTensorReductionWaitsForBothRoleContributions)
{
  using ndnsf::di::tests::CancellableBusDependencyIo;
  using ndnsf::di::tests::PartialBus;
  onnx::GraphProto graph;
  const auto port = [](std::string name) {
    return NativeAssemblyTensorContractV3{std::move(name), "float32", {std::int64_t{2}}};
  };
  auto* input = graph.add_input();
  input->set_name("X");
  auto* type = input->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  type->mutable_shape()->add_dim()->set_dim_value(2);
  const auto node = [&](const char* op, const char* output,
                        std::initializer_list<const char*> inputs) {
    auto* value = graph.add_node();
    value->set_op_type(op);
    value->add_output(output);
    for (const auto* name : inputs) value->add_input(name);
  };
  node("Identity", "A", {"X"});
  node("Identity", "B", {"X"});
  node("Add", "sum", {"A", "B"});
  node("Neg", "Z", {"sum"});
  const auto layout = compileParallelEntries(
    graph, {"first-reduction", nativePlanningDigest("fixture-region"), {"left", "right"}},
    {{"left", {{0}, {port("X")}, {port("A")}}},
     {"right", {{1}, {port("X")}, {port("B")}}},
     {"next", {{3}, {port("sum")}, {port("Z")}}}},
    {"right"}, {{2, {{{}, {port("A"), port("B")}, {port("sum")}}, true}}});
  const NativeProviderAssignment assignment{{{"left", "/fixture/provider-a"},
                                              {"right", "/fixture/provider-b"}}};
  ExecutionAttemptKey attempt;
  attempt.requestId = "/fixture/wan/first-reduction";
  attempt.attemptEpoch = 1;
  const auto session = attempt.scopedSessionId();
  auto left = roleSpecFor(layout.plan, "left", attempt, assignment, "/fixture/provider-a");
  auto right = roleSpecFor(layout.plan, "right", attempt, assignment, "/fixture/provider-b");
  BOOST_REQUIRE_EQUAL(left.outputs.size(), 1);
  const auto delivery = left.outputs.front().plannedDataName;

  auto bus = std::make_shared<PartialBus>();
  std::promise<std::pair<std::string, TensorBundle>> withholding;
  auto held = withholding.get_future();
  auto leftIo = std::make_shared<CancellableBusDependencyIo>(bus);
  auto rightIo = std::make_shared<CancellableBusDependencyIo>(bus,
    [&](const auto& edge, auto& value) {
      withholding.set_value({edge.plannedDataName, value});
      return false;
    });
  const auto bundle = [](const std::string& name, std::initializer_list<float> values) {
    TensorBundle result;
    result.name = name;
    result.payload = encodeTensorBundle({{name, TensorElementType::Float32, {2},
                                         float32Payload(values)}});
    result.expectedBytes = result.payload.size();
    return result;
  };
  std::atomic<unsigned> downstream{0};
  const auto execute = [&](const RoleExecutionContext& context) {
    const auto& output = context.activeStep->outputSlots.front().slotId;
    if (output == "A") return std::map<std::string, TensorBundle>{{output, bundle(output, {2, 3})}};
    if (output == "B") return std::map<std::string, TensorBundle>{{output, bundle(output, {5, 7})}};
    const auto merged = decodeTensorBundleViews(context.stepInputsBySlot.at("sum"));
    std::array<float, 2> data{};
    if (merged.size() != 1 || merged.front().payload.size() != sizeof(data))
      throw std::runtime_error("unexpected first reduction result shape");
    std::memcpy(data.data(), merged.front().payload.data(), sizeof(data));
    ++downstream;
    return std::map<std::string, TensorBundle>{{output, bundle(output, {-data[0], -data[1]})}};
  };
  ProviderRoleWorker leftWorker(1, 1, 8, std::chrono::seconds(5));
  ProviderRoleWorker rightWorker(1, 1, 8, std::chrono::seconds(5));
  BOOST_SCOPE_EXIT_ALL(&bus) {
    bus->cancel(std::make_exception_ptr(std::runtime_error("first reduction fixture stopped")));
  };
  const std::map<std::string, TensorBundle> inputs{{"X", bundle("X", {1, 1})}};
  auto leftDone = leftWorker.executeAsync(session, std::move(left), leftIo, execute, inputs);
  auto rightDone = rightWorker.executeAsync(session, std::move(right), rightIo, execute, inputs);
  BOOST_REQUIRE(held.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while ((!bus->published(delivery) || !leftIo->prefetchCalls.load()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  BOOST_REQUIRE(bus->published(delivery));
  BOOST_REQUIRE_GT(leftIo->prefetchCalls.load(), 0);
  BOOST_CHECK_EQUAL(downstream.load(), 0);
  BOOST_CHECK(leftDone.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
  const auto [name, contribution] = held.get();
  bus->publish(name, contribution);
  BOOST_REQUIRE(leftDone.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  BOOST_REQUIRE(rightDone.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  const auto result = leftDone.get();
  (void)rightDone.get();
  BOOST_CHECK_EQUAL(downstream.load(), 1);
  const auto output = decodeTensorBundleViews(result.outputsByScope.at("Z")).front().payload;
  const auto expected = float32Payload({-7, -10});
  BOOST_CHECK(std::equal(output.begin(), output.end(), expected.begin(), expected.end()));
}

BOOST_AUTO_TEST_CASE(CanonicalCallOwnershipSurvivesSelectionAndFixedInvocation)
{
  NativeSelectionProjectionV3 projection;
  const auto digest = [] (const char* value) { return nativePlanningDigest(value); };
  projection.provider = "/fixture/provider";
  projection.requestId = "/fixture/request";
  projection.attempt = 1;
  projection.deadlineMs = 120000;
  projection.planCoreDigest = digest("core");
  projection.planDigest = digest("plan");
  projection.ackClosedDigest = digest("ack");
  projection.offerDigest = digest("offer");
  projection.securityPolicySnapshotDigest = digest("policy");
  auto& role = projection.assembly;
  role.role = role.selectedRole = "decoder";
  role.roleKind = "COMPONENT_SET";
  role.backend = "onnxruntime-cpu";
  role.adapterId = "wan";
  role.adapterVersion = "1";
  role.artifactDigest = digest("artifact");
  role.recipeDigest = digest("recipe");
  role.modelManifestDigest = digest("manifest");
  role.artifactProfileDigest = digest("profile");
  role.graphDigest = digest("graph");
  role.canonicalInitializerDigest = digest("initializers");
  role.adapterDescriptorDigest = digest("adapter");
  role.assemblerDescriptorDigest = digest("assembler");
  role.backendAbi = "fixture-ort";
  role.nodeIndices = {1, 2, 5};
  role.precision = "fp32";
  role.quantization = "none";
  role.layout = "ncdhw";
  role.padding = "none";
  role.maxSourceBytes = role.maxAssembledBytes = 1U << 20;
  role.maxNodes = 8;
  const std::vector<std::variant<std::int64_t, std::string>> shape{std::int64_t{2}};
  role.expectedInputs = {{"latent", "float32", shape}};
  role.expectedOutputs = {{"rgb", "float32", shape}};
  projection.selectedRole = role;
  projection.executionRole = {role.selectedRole, role.role, 0, 0, 0,
                               role.backend, role.adapterId, role.adapterVersion};
  auto& flow = projection.dataflow;
  flow.requestId = projection.requestId;
  flow.attempt = projection.attempt;
  flow.planDigest = projection.planDigest;
  flow.role = role.selectedRole;
  flow.terminalResponseOwner = true;
  auto flowJson = nativeDataflowJson(flow);
  flowJson.erase("dataflow_digest");
  flow.dataflowDigest = nativePlanningDigest(nativeCanonicalJson(flowJson));
  projection.deviceBinding = {"CPU", projection.provider, role.selectedRole,
    projection.offerDigest, digest("topology"), digest("resources"), 1, "", "EXCLUSIVE_ROLE"};
  auto& plan = projection.plan;
  plan.roles = {role.selectedRole};
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"decoder", digest("region"), plan.roles}};
  NativeExecutionStepSpec first;
  first.stepId = "first";
  first.operation = "CALL_SUBGRAPH";
  first.unitId = role.selectedRole;
  first.artifactEntry = "latent-entry";
  first.onnxNodeIndices = {1, 2};
  first.inputSlots = {{"latent", "float32", shape, digest("layout"), "EPOCH_INPUT", {}, {}}};
  first.outputSlots = {{"hidden", "float32", shape, digest("layout"), "LOCAL_STEP", {}, {}}};
  auto second = first;
  second.stepId = "second";
  second.artifactEntry = "rgb-entry";
  second.onnxNodeIndices = {5};
  second.inputSlots = {{"hidden", "float32", shape, digest("layout"),
                        "LOCAL_STEP", "first", "hidden"}};
  second.outputSlots = {{"rgb", "float32", shape, digest("layout"), "LOCAL_STEP", {}, {}}};
  plan.steps = {first, second};

  const auto wire = nativeSelectionProjectionV3ToJson(projection);
  std::istringstream explicitInput(wire);
  const auto decoded = nativeSelectionProjectionV3FromJson(explicitInput, role.selectedRole);
  BOOST_REQUIRE_EQUAL(decoded.plan.steps.size(), 2);
  BOOST_CHECK(decoded.plan.steps[0].onnxNodeIndices == first.onnxNodeIndices);
  BOOST_CHECK(decoded.plan.steps[1].onnxNodeIndices == second.onnxNodeIndices);
  const auto supported = nativePlanRequiredCapabilities(decoded.plan);
  BOOST_REQUIRE_EQUAL(supported.size(), 2);
  BOOST_CHECK_EQUAL(supported[0], NATIVE_STEP_PLAN_CAPABILITY_V2);
  BOOST_CHECK_EQUAL(supported[1], NATIVE_ONNX_ENTRY_CAPABILITY_V1);
  const auto fixed = nativeFixedRoleAssignmentV1FromProjection(decoded);
  const auto rawFixed = nativeFixedRoleAssignmentV1ToJson(*fixed);
  std::istringstream fixedInput(rawFixed);
  const auto restored = nativeFixedRoleAssignmentV1FromJson(fixedInput);
  const auto packed = nativeFixedRoleAssignmentV1ToWire(*fixed);
  BOOST_CHECK_LT(packed.size(), rawFixed.size());
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1FromWire(packed)->digest(), restored->digest());
  BOOST_CHECK_EQUAL(nativeFixedRoleAssignmentV1FromWire(rawFixed)->digest(), restored->digest());
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromWire(packed.substr(0, packed.size() - 1)),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromWire(packed + "trailing"),
                    std::invalid_argument);
  auto invalidSize = packed;
  const std::size_t sizeOffset = std::string_view("NDNSF-DI-FIXED-Z1").size();
  invalidSize.replace(sizeOffset, 4, 4, '\xff');
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromWire(invalidSize), std::invalid_argument);
  const auto compact = nativeSelectionInvocationV2ToJson(decoded, *restored);
  std::istringstream invocationInput(compact);
  const auto invocation = nativeSelectionInvocationV1FromJson(
    invocationInput, role.selectedRole, *restored);
  BOOST_CHECK(invocation.plan.steps[1].onnxNodeIndices == second.onnxNodeIndices);

  const auto reject = [&](NativeJson nodes) {
    auto tampered = nativeParseJson(wire);
    tampered.at("step_template_v2").at("steps").at(1)["onnx_node_indices"] = std::move(nodes);
    std::istringstream input(nativeCanonicalJson(tampered));
    BOOST_CHECK_THROW(nativeSelectionProjectionV3FromJson(input, role.selectedRole),
                      std::invalid_argument);
  };
  reject(NativeJson::array({2}));
  reject(NativeJson::array({6}));
  reject(NativeJson::array({"5"}));
  reject(NativeJson::array());
  auto changed = decoded;
  changed.plan.steps[0].onnxNodeIndices = {1};
  changed.plan.steps[1].onnxNodeIndices = {2, 5};
  BOOST_CHECK_THROW(nativeSelectionInvocationV2ToJson(changed, *restored), std::invalid_argument);

  // A real fixed role codec round trip beyond the transport ceiling, without a model payload.
  auto large = projection;
  auto& largeRole = large.assembly;
  constexpr std::size_t NODE_COUNT = 8000;
  largeRole.maxNodes = NODE_COUNT;
  largeRole.nodeIndices.clear();
  large.plan.steps.clear();
  largeRole.nodeIndices.reserve(NODE_COUNT);
  large.plan.steps.reserve(NODE_COUNT);
  for (std::size_t index = 0; index < NODE_COUNT; ++index) {
    auto step = first;
    step.stepId = "decoder-stage-step-" + std::to_string(index);
    step.artifactEntry = step.stepId;
    step.onnxNodeIndices = {index};
    step.outputSlots[0].slotId = index + 1 == NODE_COUNT ? "rgb" :
      "intermediate-activation-" + std::to_string(index);
    if (index != 0) {
      const auto& previous = large.plan.steps.back();
      const auto& name = previous.outputSlots[0].slotId;
      step.inputSlots = {{name, "float32", shape, first.inputSlots[0].layoutDigest,
                          "LOCAL_STEP", previous.stepId, name}};
    }
    largeRole.nodeIndices.push_back(index);
    large.plan.steps.push_back(std::move(step));
  }
  large.selectedRole = largeRole;
  const auto fixedLarge = nativeFixedRoleAssignmentV1FromProjection(std::move(large));
  const auto largeJson = nativeFixedRoleAssignmentV1ToJson(*fixedLarge);
  BOOST_REQUIRE_GT(largeJson.size(), 4U << 20);
  BOOST_CHECK_THROW(nativeFixedRoleAssignmentV1FromWire(largeJson), std::invalid_argument);
  const auto largeWire = nativeFixedRoleAssignmentV1ToWire(*fixedLarge);
  const auto recovered = nativeFixedRoleAssignmentV1FromWire(largeWire);
  BOOST_CHECK_EQUAL(recovered->digest(), fixedLarge->digest());
  BOOST_CHECK_EQUAL(recovered->executionTemplate()->steps.size(), NODE_COUNT);
  BOOST_CHECK(nativeFixedRoleAssignmentV1ToJson(*recovered) == largeJson);
  BOOST_TEST_MESSAGE("Large fixed source JSON " << largeJson.size() << " wire " << largeWire.size());

  // The compact rank graph omits the other ranks' nodes. Its entry graphs both
  // borrow the same external initializer, including the nonzero byte offset.
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(17);
  auto& graph = *model.mutable_graph();
  graph.set_name("entry-fixture");
  const auto port = [] (onnx::ValueInfoProto& value, const char* name) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  port(*graph.add_input(), "latent");
  port(*graph.add_output(), "rgb");
  auto& weight = *graph.add_initializer();
  weight.set_name("weight");
  weight.set_data_type(onnx::TensorProto::FLOAT);
  weight.add_dims(2);
  weight.set_data_location(onnx::TensorProto::EXTERNAL);
  const auto external = [&] (const char* key, const char* value) {
    auto& item = *weight.add_external_data();
    item.set_key(key);
    item.set_value(value);
  };
  external("location", "model.onnx.data");
  external("offset", "4");
  external("length", "8");
  const auto node = [&] (const char* op, const char* lhs, const char* rhs, const char* output) {
    auto& instance = *graph.add_node();
    instance.set_op_type(op);
    instance.add_input(lhs);
    if (rhs != nullptr) instance.add_input(rhs);
    instance.add_output(output);
  };
  node("Add", "latent", "weight", "intermediate");
  node("Identity", "intermediate", nullptr, "hidden");
  node("Mul", "hidden", "weight", "rgb");
  const auto serialized = model.SerializeAsString();
  const std::vector<std::uint8_t> roleBytes(serialized.begin(), serialized.end());
  const std::array<float, 3> sidecar{100, 2, 3};
  auto scratch = ndnsf::test::makeTestScratchTemplate("ndnsf-onnx-entries");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  const std::filesystem::path dir(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  };
  {
    std::ofstream output(dir / "model.onnx.data", std::ios::binary);
    output.write(reinterpret_cast<const char*>(sidecar.data()), sizeof(sidecar));
    BOOST_REQUIRE(output.good());
  }
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(10),
                                      [] {}, role.maxSourceBytes, role.maxAssembledBytes};
  auto table = NativeJson::array();
  extractNativeOnnxEntryGraphs(decoded, roleBytes, sizeof(sidecar), control,
    [&] (const NativeExecutionStepSpec& step, std::vector<std::uint8_t> entry) {
      const auto hash = nativePlanningDigest(entry.data(), entry.size());
      const auto file = "entry-" + hash.substr(7) + ".onnx";
      std::ofstream output(dir / file, std::ios::binary);
      output.write(reinterpret_cast<const char*>(entry.data()), entry.size());
      BOOST_REQUIRE(output.good());
      table.push_back({{"id", step.artifactEntry}, {"file", file}, {"digest", hash},
                       {"inputs", {step.inputSlots.front().slotId}},
                       {"outputs", {step.outputSlots.front().slotId}}});
    });
  BOOST_REQUIRE_EQUAL(table.size(), 2);
  auto tight = control;
  tight.maxAssembledBytes = roleBytes.size() + sizeof(sidecar);
  const auto rejectEntry = [] (const auto&, auto) { BOOST_FAIL("unexpected entry graph"); };
  BOOST_CHECK_THROW(extractNativeOnnxEntryGraphs(decoded, roleBytes, sizeof(sidecar), tight,
                                                rejectEntry), std::runtime_error);
  BOOST_CHECK_THROW(extractNativeOnnxEntryGraphs(decoded, roleBytes, sizeof(float), control,
                                                rejectEntry), std::runtime_error);
  auto badModel = model;
  badModel.mutable_graph()->mutable_node(0)->set_input(0, "unbound");
  const auto badBytes = badModel.SerializeAsString();
  BOOST_CHECK_EXCEPTION(extractNativeOnnxEntryGraphs(
    decoded, {badBytes.begin(), badBytes.end()}, sizeof(sidecar), control, rejectEntry),
    std::runtime_error, [](const auto& error) {
      return std::string(error.what()) == "DI_NATIVE_ONNX_ENTRY_BOUNDARY";
    });

  NativeModelRunnerSpec runnerSpec;
  runnerSpec.backend = role.backend;
  runnerSpec.role = role.selectedRole;
  runnerSpec.path = (dir / "model.onnx").string();
  runnerSpec.metadata[NATIVE_ONNX_ENTRY_TABLE] = nativeCanonicalJson(table);
  runnerSpec.metadata["canonicalOnnxEntryBinding"] =
    detail::nativeRoleStepContractDigest(decoded.plan, role.selectedRole);
  bindNativeRunnerPreparationContext(runnerSpec, decoded,
                                     {decoded.provider, "fixture-boot", 1, dir.string()});
  validateNativeOnnxEntryTable(decoded, runnerSpec);
  BOOST_REQUIRE_EQUAL(runnerSpec.dependencyInputWarmupEntries.size(), 1);
  BOOST_CHECK_EQUAL(runnerSpec.dependencyInputWarmupEntries.front(), second.artifactEntry);
  auto current = decoded;
  current.plan.steps[0].artifactEntry = "renamed-latent-entry";
  current.plan.steps[1].artifactEntry = "renamed-hidden-entry";
  auto reboundSpec = runnerSpec;
  BOOST_CHECK_THROW(validateNativeOnnxEntryTable(current, reboundSpec), std::invalid_argument);
  bindNativeRunnerPreparationContext(reboundSpec, current,
                                     {decoded.provider, "fixture-boot", 1, dir.string()});
  validateNativeOnnxEntryTable(current, reboundSpec);
  BOOST_CHECK_EQUAL(readNativeOnnxEntryTable(reboundSpec).front().entryId,
                    "renamed-latent-entry");
  BOOST_REQUIRE_EQUAL(reboundSpec.dependencyInputWarmupEntries.size(), 1);
  BOOST_CHECK_EQUAL(reboundSpec.dependencyInputWarmupEntries.front(), "renamed-hidden-entry");
  table.at(1)["inputs"] = {"latent"};
  auto wrongSpec = runnerSpec;
  wrongSpec.metadata[NATIVE_ONNX_ENTRY_TABLE] = nativeCanonicalJson(table);
  BOOST_CHECK_THROW(validateNativeOnnxEntryTable(decoded, wrongSpec), std::invalid_argument);
  RegistryNativeModelRunnerFactory factory;
  registerMaintainedNativeProviderBackends(factory, nullptr);
  const auto runner = factory.create(runnerSpec);
  const auto evidence = runner->executionEvidenceSnapshot();
  BOOST_REQUIRE(evidence);
  BOOST_CHECK(evidence->loadCompleted);
  BOOST_CHECK(!evidence->warmupCompleted);
  BOOST_CHECK(evidence->warmupDeferredDependencyInput);
  BOOST_CHECK(evidence->runnerKind == RunnerKind::OnnxRuntimeCpu);
  BOOST_CHECK_EQUAL(evidence->artifactDigests.at(role.selectedRole), role.artifactDigest);
  RoleExecutionContext ctx;
  ctx.role = role.selectedRole;
  ctx.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(decoded.plan.steps);
  const std::array<float, 2> input{1, 2};
  NamedTensor latent{"latent", TensorElementType::Float32, {2},
                     std::vector<std::uint8_t>(sizeof(input))};
  std::memcpy(latent.payload.data(), input.data(), sizeof(input));
  TensorBundle initial;
  initial.name = latent.name;
  initial.payload = encodeTensorBundle({std::move(latent)});
  initial.expectedBytes = initial.payload.size();
  ctx.activeStep = std::make_shared<const NativeExecutionStepSpec>(first);
  ctx.stepInputsBySlot.emplace("latent", std::move(initial));
  ctx.stepInputsBySlot = runner->run(ctx);
  ctx.activeStep = std::make_shared<const NativeExecutionStepSpec>(second);
  const auto result = runner->run(ctx);
  const auto tensor = decodeTensorBundleViews(result.at("rgb"));
  BOOST_REQUIRE_EQUAL(tensor.size(), 1);
  std::array<float, 2> rgb{};
  BOOST_REQUIRE_EQUAL(tensor.front().payload.size(), sizeof(rgb));
  std::memcpy(rgb.data(), tensor.front().payload.data(), sizeof(rgb));
  BOOST_CHECK_EQUAL(rgb[0], 6.0f);
  BOOST_CHECK_EQUAL(rgb[1], 15.0f);
}

BOOST_AUTO_TEST_CASE(SafeTensorsCheckpointBindsDisjointWanPatchRanks)
{
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-checkpoint");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&scratch) {
    std::error_code cleanup;
    std::filesystem::remove_all(scratch, cleanup);
  };
  const std::filesystem::path directory(scratch.data());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1U << 20, 1U << 20};
  const auto write = [&] (const std::string& shard, const std::string& header,
                          const std::vector<float>& payload) {
    return writeFixtureSafeTensors(directory / shard, header, payload, control);
  };
  const auto weight = write("weights.safetensors", R"({
    "patch_embedding.weight":{"dtype":"F32","shape":[2,2,1,2,2],"data_offsets":[0,64]}})",
    {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, -1});
  const auto bias = write("bias.safetensors", R"({
    "patch_embedding.bias":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})", {5, -2});
  const WanCheckpointShards shards{{"weights.safetensors", weight}, {"bias.safetensors", bias}};
  const std::string index = R"({"metadata":{"total_size":72},"weight_map":{
    "patch_embedding.weight":"weights.safetensors","patch_embedding.bias":"bias.safetensors"}})";
  const WanCheckpoint checkpoint(shards, index);
  BOOST_REQUIRE_EQUAL(checkpoint.parameters().size(), 2);
  const auto payloadStart = weight->size() - 64;
  BOOST_CHECK_THROW(checkpoint.reader()("weights.safetensors", payloadStart - 1, 4),
                    std::out_of_range);
  BOOST_CHECK_THROW(checkpoint.reader()("weights.safetensors", payloadStart + 63, 2),
                    std::out_of_range);
  BOOST_CHECK_THROW(checkpoint.reader()("../weights.safetensors", payloadStart, 4),
                    std::invalid_argument);
  BOOST_CHECK_THROW((void)WanCheckpoint(shards), std::invalid_argument);
  BOOST_CHECK_THROW(WanCheckpoint(shards, R"({"metadata":{"total_size":72},"weight_map":{
    "patch_embedding.weight":"bias.safetensors","patch_embedding.bias":"weights.safetensors"}})"),
    std::invalid_argument);
  BOOST_CHECK_THROW(WanCheckpoint(shards, R"({"metadata":{"total_size":71},"weight_map":{
    "patch_embedding.weight":"weights.safetensors","patch_embedding.bias":"bias.safetensors"}})"),
    std::invalid_argument);
  const auto bfloat = write("bfloat.safetensors", R"({
    "bad":{"dtype":"BF16","shape":[1],"data_offsets":[0,4]}})", {1});
  const auto overlap = write("overlap.safetensors", R"({
    "a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},
    "b":{"dtype":"F32","shape":[1],"data_offsets":[4,8]}})", {1, 2});
  const auto duplicate = write("duplicate.safetensors", R"({
    "x":{"dtype":"F32","shape":[1],"data_offsets":[0,4]},
    "x":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", {0});
  BOOST_CHECK_THROW(WanCheckpoint({{"bfloat.safetensors", bfloat}}), std::invalid_argument);
  BOOST_CHECK_THROW(WanCheckpoint({{"overlap.safetensors", overlap}}), std::invalid_argument);
  BOOST_CHECK_THROW(WanCheckpoint({{"duplicate.safetensors", duplicate}}), std::invalid_argument);
  BOOST_CHECK_THROW(lowerWan21PatchEmbedding(checkpoint, {3, 1, 2, 2}), std::invalid_argument);
  BOOST_CHECK_THROW(lowerWan21PatchEmbedding(WanCheckpoint({{"bias.safetensors", bias}}),
                                              {2, 1, 2, 2}), std::invalid_argument);

  const auto checkPatch = [&] (const WanCheckpoint& archive) {
    const auto layout = lowerWan21PatchEmbedding(archive, {2, 1, 2, 2});
    std::array<std::array<float, 8>, 2> expectedWeights{{
      {{1, 0, 0, 0, 0, 0, 3, 0}}, {{0, 2, 0, 0, 0, 0, 0, -1}}
    }};
    for (std::size_t rank = 0; rank < expectedWeights.size(); ++rank) {
      const auto name = "patch_embedding/ndnsf-rank-" + std::to_string(rank) +
                        "/patch_embedding.weight";
      const auto& initializers = layout.model.graph().initializer();
      const auto found = std::find_if(initializers.begin(), initializers.end(),
        [&](const auto& item) { return item.name() == name; });
      BOOST_REQUIRE(found != initializers.end());
      BOOST_REQUIRE_EQUAL(found->raw_data().size(), sizeof(expectedWeights[rank]));
      BOOST_CHECK(std::memcmp(found->raw_data().data(), expectedWeights[rank].data(),
                              sizeof(expectedWeights[rank])) == 0);
    }

    const auto graph = layout.model.SerializeAsString();
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-checkpoint-patch");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session runner(env, graph.data(), graph.size(), options);
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<float, 8> input{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::int64_t, 5> shape{1, 2, 1, 2, 2};
    const auto value = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                       shape.data(), shape.size());
    const char* inputName = "latent";
    const char* outputName = "projection";
    const auto outputs = runner.Run(Ort::RunOptions{}, &inputName, &value, 1, &outputName, 1);
    const auto* result = outputs.front().GetTensorData<float>();
    BOOST_CHECK_EQUAL(result[0], 18);
    BOOST_CHECK_EQUAL(result[1], -1);
  };
  checkPatch(checkpoint);
  if (const auto* path = std::getenv("NDNSF_DI_WAN_CONVERTED_CHECKPOINT")) {
    const auto length = std::filesystem::file_size(path);
    BOOST_REQUIRE_LT(length, 2U << 20);
    std::ifstream file(path, std::ios::binary);
    BOOST_REQUIRE(file.good());
    const std::string generated(std::istreambuf_iterator<char>(file), {});
    auto archiveControl = control;
    archiveControl.maxSourceBytes = 2U << 20;
    const WanCheckpoint converted({
      {"fixture.safetensors", openNativeCanonicalInitializerFile(
        path, length, nativePlanningDigest(generated), archiveControl)}});
    BOOST_REQUIRE_EQUAL(converted.parameters().size(), 3);
    checkPatch(converted);
  }

  // The same reader parses official shard headers backed by metadata-only owners.
  if (const auto* root = std::getenv("NDNSF_DI_WAN_CHECKPOINT_METADATA_ROOT")) {
    const auto read = [] (const std::filesystem::path& path) {
      std::ifstream stream(path, std::ios::binary);
      BOOST_REQUIRE(stream.good());
      return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto recipe = nativeParseJson(read(std::filesystem::path(root) / "recipe.json"));
    const auto wire = read(std::filesystem::path(root) / "dit-index.json");
    BOOST_REQUIRE_EQUAL(nativePlanningDigest(wire),
                        "sha256:" + recipe.at("sources").at(0).at("sha256").get<std::string>());
    WanCheckpointShards metadata;
    for (const auto& row : recipe.at("safetensors_header_ranges")) {
      const auto label = row.at("file").get<std::string>();
      auto data = read(std::filesystem::path(root) / "headers" / (label + ".header"));
      BOOST_CHECK_EQUAL(nativePlanningDigest(data), "sha256:" + row.at("sha256").get<std::string>());
      metadata.emplace(label, std::make_shared<HeaderSource>(
        std::move(data), row.at("full_bytes").get<std::uint64_t>()));
    }
    BOOST_REQUIRE_EQUAL(metadata.size(), 6);
    const WanCheckpoint actual(std::move(metadata), wire);
    BOOST_REQUIRE_EQUAL(actual.parameters().size(), 1095);
    for (std::size_t block = 0; block < 40; ++block) {
      const auto prefix = "blocks." + std::to_string(block);
      const auto& ffn = actual.parameters().at(prefix + ".ffn.0.weight");
      BOOST_REQUIRE_EQUAL(ffn.dims_size(), 2);
      BOOST_CHECK_EQUAL(ffn.dims(0), 13824);
      BOOST_CHECK_EQUAL(ffn.dims(1), 5120);
    }
    BOOST_CHECK_THROW(lowerWan21PatchEmbedding(actual, {16, 21, 60, 104}), std::out_of_range);
  }
}

BOOST_AUTO_TEST_CASE(PatchConv3dLowersIntoDisjointRanksAndMatchesUnsplitOrt)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(17);
  auto* graph = source.mutable_graph();
  graph->set_name("wan-patch-fixture");
  constexpr std::array<std::int64_t, 5> inputShape{1, 4, 2, 2, 2};
  const auto declare = [] (onnx::ValueInfoProto& value, const char* name,
                           const std::array<std::int64_t, 5>& dims) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto extent : dims) tensor->mutable_shape()->add_dim()->set_dim_value(extent);
  };
  declare(*graph->add_input(), "latent", inputShape);
  declare(*graph->add_output(), "patches", {1, 1, 2, 1, 1});
  auto* weight = graph->add_initializer();
  weight->set_name("weight");
  weight->set_data_type(onnx::TensorProto::FLOAT);
  for (const auto extent : {1, 4, 1, 2, 2}) weight->add_dims(extent);
  for (int channel = 1; channel <= 4; ++channel) {
    for (int pixel = 0; pixel < 4; ++pixel) weight->add_float_data(channel);
  }
  auto* bias = graph->add_initializer();
  bias->set_name("bias");
  bias->set_data_type(onnx::TensorProto::FLOAT);
  bias->add_dims(1);
  bias->add_float_data(7);
  auto* conv = graph->add_node();
  conv->set_name("patch_embedding");
  conv->set_op_type("Conv");
  for (const auto* port : {"latent", "weight", "bias"}) conv->add_input(port);
  conv->add_output("patches");
  auto* stride = conv->add_attribute();
  stride->set_name("strides");
  stride->set_type(onnx::AttributeProto::INTS);
  for (const auto amount : {1, 2, 2}) stride->add_ints(amount);

  const auto layout = lowerPatchEmbedding(source);
  const auto& lowered = layout.model;
  BOOST_REQUIRE_EQUAL(lowered.graph().node_size(), 6);
  BOOST_CHECK_EQUAL(lowered.graph().node(1).input_size(), 2);
  BOOST_CHECK_EQUAL(lowered.graph().node(3).input_size(), 2);
  BOOST_CHECK_EQUAL(lowered.graph().node(5).input(1), "patch_embedding/ndnsf-bias");
  for (const auto index : {0, 4}) {
    const auto& rankWeight = lowered.graph().initializer(index);
    BOOST_CHECK_EQUAL(rankWeight.dims(1), 2);
    BOOST_CHECK_EQUAL(rankWeight.raw_data().size(), 8 * sizeof(float));
  }
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-patch-fixture");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 32> input{};
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = i / 8 + 1 + i % 8 / 4;
  const auto value = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                     inputShape.data(), inputShape.size());
  const char* inputName = "latent";
  const char* outputName = "patches";
  for (const auto* model : std::array<const onnx::ModelProto*, 2>{&source, &lowered}) {
    const auto wire = model->SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const auto output = session.Run(Ort::RunOptions{}, &inputName, &value, 1, &outputName, 1);
    BOOST_REQUIRE_EQUAL(output.front().GetTensorTypeAndShapeInfo().GetElementCount(), 2);
    const auto* values = output.front().GetTensorData<float>();
    BOOST_CHECK_EQUAL(values[0], 127);
    BOOST_CHECK_EQUAL(values[1], 167);
  }

  const auto wire = lowered.SerializeAsString();
  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto manifestJson = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(manifestJson.begin(), manifestJson.end()));
  BOOST_REQUIRE(index);
  BOOST_CHECK(index->payloads.empty());

  // The fixture supplies only the payloads a selection grant would make available.
  const auto select = [&] (const OnnxEntryCut& entry, const std::string& initializerPrefix) {
    return selectCanonicalEntry(*manifest, index, entry, [&] (const std::string& name) {
      return name.rfind(initializerPrefix, 0) == 0;
    });
  };
  const auto runEntry = [&] (NativeCanonicalSource selected, const OnnxEntryCut& entry,
                             const Ort::Value& argument, int expectedInitializers) {
    const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices,
                                                        entry.inputs, entry.outputs, control);
    onnx::ModelProto assembled;
    BOOST_REQUIRE(assembled.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    BOOST_REQUIRE_EQUAL(assembled.graph().initializer_size(), expectedInitializers);
    Ort::Session session(env, bytes.data(), bytes.size(), options);
    const auto* nameIn = entry.inputs.front().name.c_str();
    const auto* nameOut = entry.outputs.front().name.c_str();
    const auto result = session.Run(Ort::RunOptions{}, &nameIn, &argument, 1, &nameOut, 1);
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), 2);
    const auto* data = result.front().GetTensorData<float>();
    return std::array<float, 2>{data[0], data[1]};
  };
  const auto selected0 = select(layout.partials[0], "patch_embedding/ndnsf-rank-0/");
  const auto selected1 = select(layout.partials[1], "patch_embedding/ndnsf-rank-1/");
  auto wrongGrant = selected0;
  BOOST_CHECK_EXCEPTION(materializeNativeCanonicalModel(
    wrongGrant, layout.partials[1].nodeIndices, layout.partials[1].inputs,
    layout.partials[1].outputs, control), std::runtime_error,
    [] (const auto& error) { return std::string(error.what()) == "DI_NATIVE_ONNX_MATERIAL_NODE"; });
  wrongGrant = selected0;
  const auto missingWeight = std::find_if(index->references.begin(), index->references.end(),
    [] (const auto& reference) {
      return reference.logicalName == "patch_embedding/ndnsf-rank-0/weight";
    });
  BOOST_REQUIRE(missingWeight != index->references.end());
  auto& payloads = wrongGrant.materialPayloads;
  payloads.erase(std::remove_if(payloads.begin(), payloads.end(), [&] (const auto& payload) {
    return payload.payloadId == missingWeight->payloadId;
  }), payloads.end());
  BOOST_CHECK_EXCEPTION(materializeNativeCanonicalModel(
    wrongGrant, layout.partials[0].nodeIndices, layout.partials[0].inputs,
    layout.partials[0].outputs, control), std::runtime_error,
    [] (const auto& error) {
      return std::string(error.what()) == "DI_NATIVE_ONNX_MATERIAL_INITIALIZER";
    });
  const auto rank0 = runEntry(selected0, layout.partials[0], value, 4);
  const auto rank1 = runEntry(selected1, layout.partials[1], value, 4);
  BOOST_CHECK_EQUAL(rank0[0], 20);
  BOOST_CHECK_EQUAL(rank0[1], 32);
  BOOST_CHECK_EQUAL(rank1[0], 100);
  BOOST_CHECK_EQUAL(rank1[1], 128);
  std::array<float, 2> summed{rank0[0] + rank1[0], rank0[1] + rank1[1]};
  constexpr std::array<std::int64_t, 5> outputShape{1, 1, 2, 1, 1};
  const auto reduced = Ort::Value::CreateTensor<float>(memory, summed.data(), summed.size(),
                                                       outputShape.data(), outputShape.size());
  const auto joined = runEntry(select(layout.addBias, "patch_embedding/ndnsf-bias"),
                               layout.addBias, reduced, 1);
  BOOST_CHECK_EQUAL(joined[0], 127);
  BOOST_CHECK_EQUAL(joined[1], 167);

  auto badShape = source;
  badShape.mutable_graph()->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(1);
  BOOST_CHECK_THROW(lowerPatchEmbedding(badShape), std::invalid_argument);
  stride->set_ints(1, 1);
  BOOST_CHECK_THROW(lowerPatchEmbedding(source), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FeedForwardPreservesTanhGeluAndUnevenIntermediateSlices)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-ffn-fixture");
  const auto port = [] (onnx::ValueInfoProto& value, const char* name) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(1);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  port(*graph->add_input(), "hidden");
  port(*graph->add_output(), "result");
  const std::vector<float> up{1, -2, 3, 4, -5, 2, 1, -2, 5, 3};
  const std::vector<float> upBias{1, 0, 2, 1, -1};
  const std::vector<float> down{1, -1, 2, 1, -1, 2, 1, 2, -2, 1};
  const std::vector<float> downBias{7, -3};
  const auto parameter = [&] (const char* name, std::initializer_list<int> dimensions,
                              const std::vector<float>& values) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    for (auto extent : dimensions) tensor->add_dims(extent);
    for (auto value : values) tensor->add_float_data(value);
  };
  parameter("up", {2, 5}, up);
  parameter("up-bias", {5}, upBias);
  parameter("down", {5, 2}, down);
  parameter("down-bias", {2}, downBias);
  const auto node = [&] (const char* op, const char* input, const char* output,
                         const char* weight = nullptr) {
    auto* value = graph->add_node();
    value->set_op_type(op);
    value->add_input(input);
    if (weight != nullptr) value->add_input(weight);
    value->add_output(output);
    return value;
  };
  node("MatMul", "hidden", "expanded", "up");
  node("Add", "expanded", "biased", "up-bias");
  auto* approximation = node("Gelu", "biased", "activated")->add_attribute();
  approximation->set_name("approximate");
  approximation->set_type(onnx::AttributeProto::STRING);
  approximation->set_s("tanh");
  node("MatMul", "activated", "unbiased", "down");
  node("Add", "unbiased", "result", "down-bias");

  const auto layout = lowerFeedForward(model);
  const auto& lowered = layout.model;
  BOOST_REQUIRE_EQUAL(lowered.graph().initializer_size(), 7);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(0).dims(1), 3);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(2).dims(0), 3);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(3).dims(1), 2);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(5).dims(0), 2);
  std::array<float, 2> input{0.5F, -0.75F};
  std::array<double, 2> expected{downBias[0], downBias[1]};
  for (std::size_t j = 0; j < upBias.size(); ++j) {
    const double x = static_cast<double>(input[0]) * up[j] +
                     static_cast<double>(input[1]) * up[j + 5] + upBias[j];
    const double activation = 0.5 * x *
      (1.0 + std::tanh(std::sqrt(2.0 / std::acos(-1.0)) * (x + 0.044715 * x * x * x)));
    for (std::size_t k = 0; k < expected.size(); ++k) expected[k] += activation * down[2 * j + k];
  }
  const std::array<std::int64_t, 2> shape{1, 2};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                               shape.data(), shape.size());
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-ffn-fixture");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const char* inputName = "hidden";
  const char* outputName = "result";
  for (const auto* candidate : std::array<const onnx::ModelProto*, 2>{&model, &lowered}) {
    const auto wire = candidate->SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const auto outputs = session.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &outputName, 1);
    BOOST_REQUIRE_EQUAL(outputs.front().GetTensorTypeAndShapeInfo().GetElementCount(), 2);
    const auto* values = outputs.front().GetTensorData<float>();
    for (std::size_t k = 0; k < expected.size(); ++k) {
      BOOST_CHECK_SMALL(static_cast<double>(values[k]) - expected[k], 1e-5);
    }
  }
  auto text = model;
  auto* textGraph = text.mutable_graph();
  const auto textPort = [] (onnx::ValueInfoProto& value, std::int64_t width) {
    auto* shape = value.mutable_type()->mutable_tensor_type()->mutable_shape();
    shape->clear_dim();
    for (auto dim : {std::int64_t{1}, std::int64_t{1}, width}) {
      shape->add_dim()->set_dim_value(dim);
    }
  };
  textPort(*textGraph->mutable_input(0), 2);
  textPort(*textGraph->mutable_output(0), 5);
  auto* textDown = textGraph->mutable_initializer(2);
  textDown->set_dims(1, 5);
  textDown->clear_float_data();
  for (int row = 0; row < 5; ++row) {
    for (int column = 0; column < 5; ++column) textDown->add_float_data(row == column ? 1 : 0);
  }
  auto* textBias = textGraph->mutable_initializer(3);
  textBias->set_dims(0, 5);
  textBias->clear_float_data();
  for (int channel = 0; channel < 5; ++channel) textBias->add_float_data(channel + 2);
  const auto paddedLayout = lowerTextEmbedding(text, 3);
  const auto& padded = paddedLayout.model;
  BOOST_REQUIRE_EQUAL(padded.graph().node(0).op_type(), "Pad");
  BOOST_REQUIRE_EQUAL(padded.graph().node(1).input(0), padded.graph().node(0).output(0));
  BOOST_REQUIRE_EQUAL(padded.graph().node(5).op_type(), "Pad");
  BOOST_REQUIRE_EQUAL(padded.graph().node(6).input(0), padded.graph().node(5).output(0));
  BOOST_REQUIRE_LT(paddedLayout.partials[0].nodeIndices.back(),
                   paddedLayout.partials[1].nodeIndices.front());
  BOOST_CHECK_THROW(lowerTextEmbedding(text, 0), std::invalid_argument);
  BOOST_CHECK_THROW(lowerFeedForward(text), std::invalid_argument);
  const auto paddedWire = padded.SerializeAsString();
  Ort::Session paddedSession(env, paddedWire.data(), paddedWire.size(), options);
  const std::array<std::int64_t, 3> textShape{1, 1, 2};
  auto inputText = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                  textShape.data(), textShape.size());
  const auto textOutputs = paddedSession.Run(Ort::RunOptions{}, &inputName, &inputText, 1,
                                              &outputName, 1);
  BOOST_REQUIRE(textOutputs.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 3, 5}));
  const auto* data = textOutputs.front().GetTensorData<float>();
  for (std::size_t token = 0; token < 3; ++token) {
    for (std::size_t channel = 0; channel < upBias.size(); ++channel) {
      double x = upBias[channel];
      if (token == 0) x += input[0] * up[channel] + input[1] * up[channel + 5];
      const double reference = 0.5 * x *
        (1.0 + std::tanh(std::sqrt(2.0 / std::acos(-1.0)) * (x + 0.044715 * x * x * x))) +
        channel + 2;
      BOOST_CHECK_SMALL(data[token * 5 + channel] - reference, 1e-6);
    }
  }

  auto dynamic = text;
  for (auto* port : {dynamic.mutable_graph()->mutable_input(0),
                     dynamic.mutable_graph()->mutable_output(0)}) {
    port->mutable_type()->mutable_tensor_type()->mutable_shape()->mutable_dim(1)
      ->set_dim_param("valid_tokens");
  }
  const auto dynamicLayout = lowerTextEmbedding(dynamic, 3);
  BOOST_CHECK_EQUAL(std::get<std::string>(dynamicLayout.partials[0].inputs[0].shape[1]),
                    "valid_tokens");
  const auto dynamicWire = dynamicLayout.model.SerializeAsString();
  Ort::Session dynamicSession(env, dynamicWire.data(), dynamicWire.size(), options);
  std::array<float, 8> tokenValues{input[0], input[1], 0.125F, -0.25F, -0.125F, 0.25F, 0, 0};
  const auto runDynamic = [&] (std::int64_t tokens) {
    const std::array<std::int64_t, 3> shape{1, tokens, 2};
    auto context = Ort::Value::CreateTensor<float>(memory, tokenValues.data(), tokens * 2,
                                                  shape.data(), shape.size());
    return dynamicSession.Run(Ort::RunOptions{}, &inputName, &context, 1, &outputName, 1);
  };
  for (const auto tokens : {1, 2, 3}) {
    const auto output = runDynamic(tokens);
    BOOST_REQUIRE(output.front().GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, 3, 5}));
    const auto* values = output.front().GetTensorData<float>();
    for (int token = 0; token < 3; ++token) {
      for (std::size_t channel = 0; channel < upBias.size(); ++channel) {
        double x = upBias[channel];
        if (token < tokens) {
          x += static_cast<double>(tokenValues[2 * token]) * up[channel] +
               static_cast<double>(tokenValues[2 * token + 1]) * up[channel + 5];
        }
        const double reference = 0.5 * x *
          (1.0 + std::tanh(std::sqrt(2.0 / std::acos(-1.0)) * (x + 0.044715 * x * x * x))) +
          channel + 2;
        BOOST_CHECK_SMALL(values[token * 5 + channel] - reference, 1e-6);
      }
    }
  }
  BOOST_CHECK_THROW(runDynamic(4), Ort::Exception);

  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(paddedWire.begin(), paddedWire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  canonical.materialManifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(canonical.materialManifest);
  for (std::size_t rank = 0; rank < paddedLayout.partials.size(); ++rank) {
    const auto& entry = paddedLayout.partials[rank];
    auto selected = canonical;
    selected.materialPayloads = canonical.materialManifest->payloads;
    const auto assembled = materializeNativeCanonicalModel(selected, entry.nodeIndices,
                                                           entry.inputs, entry.outputs, control);
    onnx::ModelProto rankModel;
    BOOST_REQUIRE(rankModel.ParseFromArray(assembled.data(), static_cast<int>(assembled.size())));
    BOOST_REQUIRE_EQUAL(rankModel.graph().node_size(), 5);
    BOOST_CHECK_EQUAL(rankModel.graph().node(0).op_type(), "Pad");
    BOOST_REQUIRE_EQUAL(rankModel.graph().initializer_size(), 4);
    const auto prefix = "ndnsf-wan-text-embedding/rank-" + std::to_string(rank) + "/";
    for (const auto& parameter : rankModel.graph().initializer()) {
      BOOST_CHECK(parameter.name() == "ndnsf-wan-text-embedding/pads" ||
                  parameter.name().rfind(prefix, 0) == 0);
    }
    Ort::Session rankSession(env, assembled.data(), assembled.size(), options);
    const char* rankOutput = entry.outputs.front().name.c_str();
    const auto outputs = rankSession.Run(Ort::RunOptions{}, &inputName, &inputText, 1,
                                         &rankOutput, 1);
    BOOST_REQUIRE(outputs.front().GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, 3, 5}));
    const auto* partial = outputs.front().GetTensorData<float>();
    for (std::size_t token = 0; token < 3; ++token) {
      for (std::size_t channel = 0; channel < 5; ++channel) {
        const bool owns = (channel < 3) == (rank == 0);
        const auto cell = token * 5 + channel;
        const double expectedPartial = owns ? data[cell] - (channel + 2.0) : 0.0;
        BOOST_CHECK_SMALL(static_cast<double>(partial[cell]) - expectedPartial, 1e-6);
      }
    }
  }
  approximation->set_s("none");
  BOOST_CHECK_THROW(lowerFeedForward(model), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NormalizedProjectionReducesAcrossUnevenHeadsBeforeScaling)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("wan-qk-fixture");
  const auto port = [] (onnx::ValueInfoProto& value, const char* name, int width) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (auto extent : {1, 2, width}) tensor->mutable_shape()->add_dim()->set_dim_value(extent);
  };
  port(*graph->add_input(), "hidden", 1);
  port(*graph->add_output(), "projected", 6);
  const auto initializer = [&] (const char* name, std::initializer_list<int> dims,
                                std::initializer_list<float> values) {
    auto* value = graph->add_initializer();
    value->set_name(name);
    value->set_data_type(onnx::TensorProto::FLOAT);
    for (int dim : dims) value->add_dims(dim);
    for (float element : values) value->add_float_data(element);
  };
  initializer("w", {1, 6}, {1, -2, 1, 3, 11, -8});
  initializer("b", {6}, {1, 0, 2, -1, 6, -5});
  initializer("gamma", {6}, {1, 2, 3, 4, 1, 0.5});
  initializer("eps", {}, {0.125});
  auto* axes = graph->add_initializer();
  axes->set_name("axes");
  axes->set_data_type(onnx::TensorProto::INT64);
  axes->add_dims(1);
  axes->add_int64_data(-1);

  const auto node = [&] (const char* op, const char* output,
                         std::initializer_list<const char*> inputs) {
    auto* value = graph->add_node();
    value->set_op_type(op);
    for (const auto* input : inputs) value->add_input(input);
    value->add_output(output);
  };
  node("MatMul", "linear", {"hidden", "w"});
  node("Add", "biased", {"linear", "b"});
  node("Mul", "squared", {"biased", "biased"});
  node("ReduceMean", "mean", {"squared", "axes"});
  node("Add", "stabilized", {"mean", "eps"});
  node("Sqrt", "rms", {"stabilized"});
  node("Div", "norm", {"biased", "rms"});
  node("Mul", "projected", {"norm", "gamma"});

  const auto layout = lowerNormalizedProjection(model, 2);
  const auto& lowered = layout.model;
  BOOST_REQUIRE_EQUAL(lowered.graph().initializer_size(), 9);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(3).dims(1), 4);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(6).dims(1), 2);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(5).dims(0), 4);
  BOOST_CHECK_EQUAL(lowered.graph().initializer(8).dims(0), 2);
  BOOST_CHECK_EQUAL(lowered.graph().node(8).input(0), "ndnsf-wan-qk/rank-0/statistics");
  BOOST_CHECK_EQUAL(lowered.graph().node(8).input(1), "ndnsf-wan-qk/rank-1/statistics");

  const std::array<double, 6> weights{1, -2, 1, 3, 11, -8};
  const std::array<double, 6> bias{1, 0, 2, -1, 6, -5};
  const std::array<double, 6> gain{1, 2, 3, 4, 1, 0.5};
  std::array<float, 2> input{1.0F, -0.25F};
  const std::array<std::int64_t, 3> shape{1, 2, 1};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                               shape.data(), shape.size());
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-qk-fixture");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const char* inputName = "hidden";
  const char* outputName = "projected";
  for (const auto* candidate : std::array<const onnx::ModelProto*, 2>{&model, &lowered}) {
    const auto wire = candidate->SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const auto outputs = session.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &outputName, 1);
    BOOST_REQUIRE_EQUAL(outputs.front().GetTensorTypeAndShapeInfo().GetElementCount(), 12);
    const auto* data = outputs.front().GetTensorData<float>();
    for (std::size_t token = 0; token < input.size(); ++token) {
      double squares = 0;
      std::array<double, 6> projected{};
      for (std::size_t channel = 0; channel < weights.size(); ++channel) {
        projected[channel] = input[token] * weights[channel] + bias[channel];
        squares += projected[channel] * projected[channel];
      }
      const auto rms = std::sqrt(squares / weights.size() + 0.125);
      for (std::size_t channel = 0; channel < weights.size(); ++channel) {
        const auto expected = projected[channel] * gain[channel] / rms;
        BOOST_CHECK_SMALL(data[token * weights.size() + channel] - expected, 1e-6);
      }
    }
  }
  NativeCanonicalSource canonical;
  const auto wire = lowered.SerializeAsString();
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto manifestIndex = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  const auto assemble = [&] (const OnnxEntryCut& entry, auto allowed) {
    auto selected = selectCanonicalEntry(*manifest, manifestIndex, entry, allowed);
    return materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                           entry.outputs, control);
  };
  std::array<Ort::Value, 2> projected{Ort::Value{nullptr}, Ort::Value{nullptr}};
  std::array<std::array<float, 2>, 2> sums{};
  constexpr std::array<std::size_t, 3> bounds{0, 4, 6};
  const std::string prefix = "ndnsf-wan-qk/";
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& project = layout.ranks[rank].project;
    const auto own = prefix + "rank-" + std::to_string(rank) + "/";
    const auto bytes = assemble(project, [&] (const std::string& name) {
      return name == own + "weights" || name == own + "bias" || name == prefix + "axes";
    });
    Ort::Session session(env, bytes.data(), bytes.size(), options);
    const std::array<const char*, 2> ports{
      project.outputs[0].name.c_str(), project.outputs[1].name.c_str()};
    auto results = session.Run(Ort::RunOptions{}, &inputName, &tensor, 1,
                                ports.data(), ports.size());
    BOOST_REQUIRE(results[1].GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, 2, 1}));
    for (std::size_t token = 0; token < input.size(); ++token) {
      double expected = 0;
      for (std::size_t ch = bounds[rank]; ch < bounds[rank + 1]; ++ch) {
        const auto local = input[token] * weights[ch] + bias[ch];
        expected += local * local;
      }
      sums[rank][token] = results[1].GetTensorData<float>()[token];
      BOOST_CHECK_SMALL(sums[rank][token] - expected, 1e-6);
    }
    projected[rank] = std::move(results[0]);
  }
  const auto& statsName = layout.ranks[0].normalize.inputs[1].name;
  std::vector<std::vector<NamedTensorView>> operands;
  for (const auto& rank : sums) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(rank.data());
    operands.push_back({{statsName, TensorElementType::Float32, {1, 2, 1},
                         {raw, rank.size() * sizeof(float)}}});
  }
  const auto global = sumTensorBundleViews(statsName, operands);
  const auto view = decodeTensorBundleViews(global);
  BOOST_REQUIRE_EQUAL(view.size(), 1);
  std::array<float, 2> globalStats{};
  BOOST_REQUIRE_EQUAL(view.front().payload.size(), sizeof(globalStats));
  std::memcpy(globalStats.data(), view.front().payload.data(), sizeof(globalStats));
  constexpr std::array<std::int64_t, 3> statisticsShape{1, 2, 1};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& normalize = layout.ranks[rank].normalize;
    const auto own = prefix + "rank-" + std::to_string(rank) + "/";
    const auto bytes = assemble(normalize, [&] (const std::string& name) {
      return name == own + "gain" || name == prefix + "width" || name == prefix + "epsilon";
    });
    Ort::Session session(env, bytes.data(), bytes.size(), options);
    const std::array<const char*, 2> ports{
      normalize.inputs[0].name.c_str(), normalize.inputs[1].name.c_str()};
    const std::array<Ort::Value, 2> operands{
      std::move(projected[rank]),
      Ort::Value::CreateTensor<float>(memory, globalStats.data(), globalStats.size(),
                                      statisticsShape.data(), statisticsShape.size())};
    const auto* port = normalize.outputs.front().name.c_str();
    const auto result = session.Run(Ort::RunOptions{}, ports.data(), operands.data(),
                                     operands.size(), &port, 1);
    const auto count = bounds[rank + 1] - bounds[rank];
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(),
                        input.size() * count);
    for (std::size_t token = 0; token < input.size(); ++token) {
      double squares = 0;
      for (std::size_t ch = 0; ch < weights.size(); ++ch) {
        const auto value = input[token] * weights[ch] + bias[ch];
        squares += value * value;
      }
      const auto rms = std::sqrt(squares / weights.size() + 0.125);
      for (std::size_t local = 0; local < count; ++local) {
        const auto ch = bounds[rank] + local;
        const auto expected = (input[token] * weights[ch] + bias[ch]) * gain[ch] / rms;
        BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[token * count + local] - expected,
                          1e-6);
      }
    }
  }
  BOOST_CHECK_THROW(lowerNormalizedProjection(model, 4), std::invalid_argument);
  axes->set_int64_data(0, -2);
  BOOST_CHECK_THROW(lowerNormalizedProjection(model, 2), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ValueAndOutputProjectionsUseTheSameUnevenHeadPartition)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(18);
  auto* graph = source.mutable_graph();
  graph->set_name("wan-head-projection");
  const auto port = [] (onnx::ValueInfoProto& value, const char* name) {
    value.set_name(name);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (int dim : {1, 2, 6}) tensor->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  port(*graph->add_input(), "input");
  port(*graph->add_output(), "output");
  auto* matrix = graph->add_initializer();
  matrix->set_name("weights");
  matrix->set_data_type(onnx::TensorProto::FLOAT);
  matrix->add_dims(6);
  matrix->add_dims(6);
  for (int row = 0; row < 6; ++row) {
    for (int column = 0; column < 6; ++column) {
      matrix->add_float_data((3 * row + column) % 11 - 5);
    }
  }
  auto* bias = graph->add_initializer();
  bias->set_name("bias");
  bias->set_data_type(onnx::TensorProto::FLOAT);
  bias->add_dims(6);
  for (int channel = 0; channel < 6; ++channel) bias->add_float_data(channel - 2);
  const auto emit = [&] (const char* op, const char* lhs, const char* rhs, const char* output) {
    auto* node = graph->add_node();
    node->set_op_type(op);
    node->add_input(lhs);
    node->add_input(rhs);
    node->add_output(output);
  };
  emit("MatMul", "input", "weights", "projected");
  emit("Add", "projected", "bias", "output");

  const auto values = lowerHeadProjection(source, 2, ProjectionParallelism::OUTPUT_CHANNELS);
  const auto outputs = lowerHeadProjection(source, 2, ProjectionParallelism::INPUT_CHANNELS);
  const auto& value = values.model;
  const auto& output = outputs.model;
  BOOST_REQUIRE_EQUAL(value.graph().initializer_size(), 4);
  BOOST_REQUIRE_EQUAL(output.graph().initializer_size(), 4);
  BOOST_CHECK_EQUAL(value.graph().initializer(0).dims(1), 4);
  BOOST_CHECK_EQUAL(value.graph().initializer(2).dims(1), 2);
  BOOST_CHECK_EQUAL(output.graph().initializer(1).dims(0), 4);
  BOOST_CHECK_EQUAL(output.graph().initializer(2).dims(0), 2);
  BOOST_CHECK_EQUAL(value.graph().node(4).op_type(), "Concat");
  BOOST_CHECK_EQUAL(output.graph().node(4).input(1), "ndnsf-wan-head/bias");

  std::array<float, 12> input{1, 2, 3, 4, 5, 6, -2, -3, 8, 0, -6, 7};
  const std::array<std::int64_t, 3> shape{1, 2, 6};
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                               shape.data(), shape.size());
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-head-projection");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const char* inputName = "input";
  const char* outputName = "output";
  for (const auto* model : std::array<const onnx::ModelProto*, 3>{&source, &value, &output}) {
    const auto wire = model->SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const auto outputs = session.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &outputName, 1);
    BOOST_REQUIRE_EQUAL(outputs.front().GetTensorTypeAndShapeInfo().GetElementCount(), input.size());
    const auto* actual = outputs.front().GetTensorData<float>();
    for (std::size_t row = 0; row < 2; ++row) {
      for (int channel = 0; channel < 6; ++channel) {
        int expected = channel - 2;
        for (int k = 0; k < 6; ++k) {
          expected += static_cast<int>(input[6 * row + k]) * ((3 * k + channel) % 11 - 5);
        }
        BOOST_CHECK_EQUAL(actual[6 * row + channel], expected);
      }
    }
  }
  for (const auto* layout : {&values, &outputs}) {
    const bool columns = layout == &values;
    NativeCanonicalSource canonical;
    const auto wire = layout->model.SerializeAsString();
    canonical.modelBytes.assign(wire.begin(), wire.end());
    const NativeAssemblyControl control{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
    const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
    BOOST_REQUIRE(manifest);
    const auto json = manifest->canonicalJson();
    const auto index = parseNativeCanonicalMaterialManifest(
      std::vector<std::uint8_t>(json.begin(), json.end()));
    const std::array<int, 3> bounds{0, 4, 6};
    for (int rank = 0; rank < 2; ++rank) {
      const auto& entry = layout->ranks[rank];
      const auto own = "ndnsf-wan-head/rank-" + std::to_string(rank) + "/";
      auto selected = selectCanonicalEntry(*manifest, index, entry, [&] (const std::string& name) {
        return name == own + "weight" || (columns && name == own + "bias");
      });
      const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                                          entry.outputs, control);
      Ort::Session session(env, bytes.data(), bytes.size(), options);
      const auto width = bounds[rank + 1] - bounds[rank];
      std::vector<float> localInput;
      const auto firstInput = columns ? 0 : bounds[rank];
      const auto lastInput = columns ? 6 : bounds[rank + 1];
      const auto firstOutput = columns ? bounds[rank] : 0;
      const auto lastOutput = columns ? bounds[rank + 1] : 6;
      for (int token = 0; token < 2; ++token) {
        localInput.insert(localInput.end(), input.begin() + token * 6 + firstInput,
                          input.begin() + token * 6 + lastInput);
      }
      const std::array<std::int64_t, 3> localShape{1, 2, columns ? 6 : width};
      const auto arg = Ort::Value::CreateTensor<float>(
        memory, localInput.data(), localInput.size(), localShape.data(), localShape.size());
      const auto* inName = entry.inputs.front().name.c_str();
      const auto* outName = entry.outputs.front().name.c_str();
      const auto produced = session.Run(Ort::RunOptions{}, &inName, &arg, 1, &outName, 1);
      BOOST_REQUIRE(produced.front().GetTensorTypeAndShapeInfo().GetShape() ==
                    (std::vector<std::int64_t>{1, 2, lastOutput - firstOutput}));
      const auto* actual = produced.front().GetTensorData<float>();
      for (int token = 0; token < 2; ++token) {
        for (int ch = firstOutput; ch < lastOutput; ++ch) {
          int expected = columns ? ch - 2 : 0;
          for (int k = firstInput; k < lastInput; ++k) {
            expected += static_cast<int>(input[token * 6 + k]) * ((3 * k + ch) % 11 - 5);
          }
          BOOST_CHECK_EQUAL(actual[token * (lastOutput - firstOutput) + ch - firstOutput],
                            expected);
        }
      }
    }
    const auto& finish = layout->finish;
    auto selected = selectCanonicalEntry(*manifest, index, finish,
      [&] (const auto& name) { return !columns && name == "ndnsf-wan-head/bias"; });
    const auto bytes = materializeNativeCanonicalModel(selected, finish.nodeIndices, finish.inputs,
                                                        finish.outputs, control);
    onnx::ModelProto join;
    BOOST_REQUIRE(join.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    BOOST_REQUIRE_EQUAL(join.graph().node_size(), 1);
    BOOST_CHECK_EQUAL(join.graph().node(0).op_type(), columns ? "Concat" : "Add");
    BOOST_CHECK_EQUAL(join.graph().initializer_size(), columns ? 0 : 1);
  }
  BOOST_CHECK_THROW(lowerHeadProjection(source, 4, ProjectionParallelism::INPUT_CHANNELS),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AttentionTilesMatchDenseMaskedReferenceWithLargeScores)
{
  const auto model = makeAttentionTileUpdate({1, 1, 2, 2, 2}, 16);
  BOOST_CHECK_THROW(makeAttentionTileUpdate({1, 1, 2, 2, 2}, 15), std::invalid_argument);
  BOOST_CHECK_THROW(makeAttentionTileUpdate({1, 20, 32760, 32760, 128}, 1U << 20),
                    std::invalid_argument);
  const auto wire = model.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-attention-tile");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  Ort::Session session(env, wire.data(), wire.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<std::int64_t, 4> valueShape{1, 1, 2, 2};
  const std::array<std::int64_t, 4> statisticShape{1, 1, 2, 1};
  const std::array<const char*, 7> inputPorts{
    "query", "key", "value", "valid", "prior-max", "prior-denominator", "prior-weighted"
  };
  const std::array<const char*, 3> outputPorts{"next-max", "next-denominator", "next-weighted"};
  std::array<float, 4> query{100, 0, 3, 2};
  std::array<std::array<float, 4>, 3> keys{{{2, 1, 3, 3}, {3.01F, 0, -3, 1}, {4, 2, 0, 0}}};
  std::array<std::array<float, 4>, 3> values{{{2, 3, 7, -4}, {-2, 9, 4, 2}, {99, 99, 99, 99}}};
  std::array<std::array<bool, 4>, 3> mask{{{true, true, false, false},
                                         {true, false, true, true},
                                         {false, false, false, false}}};
  std::array<float, 2> maximum{std::numeric_limits<float>::lowest(),
                               std::numeric_limits<float>::lowest()};
  std::array<float, 2> denom{};
  std::array<float, 4> weighted{};
  const auto valueTensor = [&] (std::array<float, 4>& data) {
    return Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                          valueShape.data(), valueShape.size());
  };
  const auto statTensor = [&] (std::array<float, 2>& data) {
    return Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                          statisticShape.data(), statisticShape.size());
  };
  for (std::size_t tile = 0; tile < keys.size(); ++tile) {
    auto valid = Ort::Value::CreateTensor<bool>(memory, mask[tile].data(), mask[tile].size(),
                                               valueShape.data(), valueShape.size());
    const std::array<Ort::Value, 7> inputs{
      valueTensor(query), valueTensor(keys[tile]), valueTensor(values[tile]), std::move(valid),
      statTensor(maximum), statTensor(denom), valueTensor(weighted)
    };
    const auto output = session.Run(Ort::RunOptions{}, inputPorts.data(), inputs.data(),
                                    inputs.size(), outputPorts.data(), outputPorts.size());
    std::copy_n(output[0].GetTensorData<float>(), maximum.size(), maximum.begin());
    std::copy_n(output[1].GetTensorData<float>(), denom.size(), denom.begin());
    std::copy_n(output[2].GetTensorData<float>(), weighted.size(), weighted.begin());
    if (tile == 0) {
      BOOST_CHECK_EQUAL(maximum[1], std::numeric_limits<float>::lowest());
      BOOST_CHECK_EQUAL(denom[1], 0);
      BOOST_CHECK_EQUAL(weighted[2], 0);
      BOOST_CHECK_EQUAL(weighted[3], 0);
    }
  }
  const auto queryBlock = makeAttentionQueryBlock({1, 1, 2, 2, 2}, 6, 16);
  BOOST_CHECK_THROW(makeAttentionQueryBlock({1, 1, 2, 2, 2}, 5, 16), std::invalid_argument);
  const auto blockWire = queryBlock.SerializeAsString();
  Ort::Session blockSession(env, blockWire.data(), blockWire.size(), options);
  std::array<float, 12> contextKeys{}, contextValues{};
  std::array<bool, 12> contextMask{};
  for (std::size_t tile = 0; tile < keys.size(); ++tile) {
    std::copy(keys[tile].begin(), keys[tile].end(), contextKeys.begin() + 4 * tile);
    std::copy(values[tile].begin(), values[tile].end(), contextValues.begin() + 4 * tile);
    for (std::size_t row = 0; row < 2; ++row) {
      std::copy_n(mask[tile].begin() + 2 * row, 2, contextMask.begin() + 6 * row + 2 * tile);
    }
  }
  const std::array<std::int64_t, 4> contextShape{1, 1, 6, 2};
  const std::array<std::int64_t, 4> maskShape{1, 1, 2, 6};
  const std::array<const char*, 4> blockPorts{"query", "context-key", "context-value",
                                             "context-valid"};
  const char* attentionPort = "attention";
  const std::array<Ort::Value, 4> blockInputs{
    valueTensor(query),
    Ort::Value::CreateTensor<float>(memory, contextKeys.data(), contextKeys.size(),
                                    contextShape.data(), contextShape.size()),
    Ort::Value::CreateTensor<float>(memory, contextValues.data(), contextValues.size(),
                                    contextShape.data(), contextShape.size()),
    Ort::Value::CreateTensor<bool>(memory, contextMask.data(), contextMask.size(),
                                   maskShape.data(), maskShape.size())
  };
  const auto attended = blockSession.Run(Ort::RunOptions{}, blockPorts.data(), blockInputs.data(),
                                         blockInputs.size(), &attentionPort, 1);
  BOOST_REQUIRE_EQUAL(attended.front().GetTensorTypeAndShapeInfo().GetElementCount(), 4);
  const auto* actual = attended.front().GetTensorData<float>();
  for (std::size_t row = 0; row < 2; ++row) {
    std::vector<double> scores;
    std::vector<std::array<double, 2>> context;
    for (std::size_t tile = 0; tile < keys.size(); ++tile) {
      for (std::size_t key = 0; key < 2; ++key) {
        if (!mask[tile][2 * row + key]) continue;
        scores.push_back((static_cast<double>(query[2 * row]) * keys[tile][2 * key] +
                          static_cast<double>(query[2 * row + 1]) * keys[tile][2 * key + 1]) /
                         std::sqrt(2.0));
        context.push_back({values[tile][2 * key], values[tile][2 * key + 1]});
      }
    }
    const double peak = *std::max_element(scores.begin(), scores.end());
    std::array<double, 2> expected{};
    double total = 0;
    for (std::size_t key = 0; key < scores.size(); ++key) {
      const auto weight = std::exp(scores[key] - peak);
      total += weight;
      for (std::size_t d = 0; d < expected.size(); ++d) expected[d] += weight * context[key][d];
    }
    BOOST_CHECK_SMALL(maximum[row] - peak, 3e-5);
    BOOST_REQUIRE_GT(denom[row], 0);
    for (std::size_t d = 0; d < expected.size(); ++d) {
      BOOST_CHECK_SMALL(weighted[2 * row + d] / denom[row] - expected[d] / total, 1e-4);
      BOOST_CHECK_SMALL(actual[2 * row + d] - expected[d] / total, 1e-4);
    }
  }
  std::fill(contextMask.begin() + 6, contextMask.end(), false);
  const auto masked = blockSession.Run(Ort::RunOptions{}, blockPorts.data(), blockInputs.data(),
                                       blockInputs.size(), &attentionPort, 1);
  const auto* maskedValues = masked.front().GetTensorData<float>();
  BOOST_CHECK_EQUAL(maskedValues[2], 0);
  BOOST_CHECK_EQUAL(maskedValues[3], 0);

  // The unscaled dot product overflows FP32, but the scaled score stays finite.
  query[0] = query[1] = 1.6e19F;
  contextKeys[0] = contextKeys[1] = 1.2e19F;
  contextMask.fill(false);
  contextMask[0] = true;
  const auto highScore = blockSession.Run(Ort::RunOptions{}, blockPorts.data(), blockInputs.data(),
                                          blockInputs.size(), &attentionPort, 1);
  const auto* highScoreValues = highScore.front().GetTensorData<float>();
  BOOST_CHECK_EQUAL(highScoreValues[0], contextValues[0]);
  BOOST_CHECK_EQUAL(highScoreValues[1], contextValues[1]);
  BOOST_CHECK_EQUAL(highScoreValues[2], 0);
  BOOST_CHECK_EQUAL(highScoreValues[3], 0);
}

BOOST_AUTO_TEST_CASE(ProductionContextUsesSingleBoundedLoopBody)
{
  constexpr AttentionTileShape tile{1, 20, 64, 64, 128};
  constexpr auto planeBytes = 20 * 64 * 64 * sizeof(float);
  const auto graph = makeAttentionQueryBlock(tile, 32768, planeBytes);
  const auto& body = graph.graph().node(3);
  BOOST_REQUIRE_EQUAL(body.op_type(), "Loop");
  BOOST_REQUIRE_EQUAL(body.attribute_size(), 1);
  const auto singleTile = makeAttentionQueryBlock(tile, 64, planeBytes);
  BOOST_CHECK_EQUAL(body.attribute(0).g().node_size(),
                    singleTile.graph().node(3).attribute(0).g().node_size());
  BOOST_CHECK_EQUAL(graph.graph().initializer(0).int64_data(0), 512);
  BOOST_CHECK_LT(graph.ByteSizeLong(), 8192);
  BOOST_CHECK_THROW(makeAttentionQueryBlock(tile, 32768, planeBytes - 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(VideoRotaryTileUsesAbsoluteCoordinatesAndPreservesPadding)
{
  constexpr VideoRotaryTile tile{2, 2, 2, 2, 8, 3, 6};
  constexpr std::uint64_t budget = 2 * sizeof(double) * 6 * 4;
  const auto lowered = makeVideoRotaryEmbedding(tile, budget);
  BOOST_CHECK_THROW(makeVideoRotaryEmbedding(tile, budget - 1), std::invalid_argument);
  const VideoRotaryTile badGrid{2, std::numeric_limits<std::int64_t>::max(), 2, 2, 8, 0, 6};
  BOOST_CHECK_THROW(makeVideoRotaryEmbedding(badGrid, budget), std::invalid_argument);
  const VideoRotaryTile badOffset{2, 2, 2, 2, 8, std::numeric_limits<std::int64_t>::max(), 6};
  BOOST_CHECK_THROW(makeVideoRotaryEmbedding(badOffset, budget), std::invalid_argument);
  const auto wire = lowered.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-video-rotary");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, wire.data(), wire.size(), options);
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 96> input{};
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = (static_cast<int>(index % 19) - 9) / 4.0F;
  }
  constexpr std::array<std::int64_t, 4> shape{1, 2, 6, 8};
  const auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                      shape.data(), shape.size());
  const char* inputName = "input";
  const char* outputName = "rotated";
  const auto output = session.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &outputName, 1);
  BOOST_REQUIRE_EQUAL(output.front().GetTensorTypeAndShapeInfo().GetElementCount(), input.size());
  const auto* result = output.front().GetTensorData<float>();
  // One tile starts at position (0, 1, 1) and crosses the frame boundary.
  // With four complex pairs, Wan uses time frequencies 1 and 1/100.
  const std::array<std::array<double, 4>, 6> phase{{
    {0, 0, 1, 1}, {1, 0.01, 0, 0}, {1, 0.01, 0, 1},
    {1, 0.01, 1, 0}, {1, 0.01, 1, 1}, {0, 0, 0, 0}
  }};
  for (std::size_t head = 0; head < 2; ++head) {
    for (std::size_t row = 0; row < phase.size(); ++row) {
      for (std::size_t pair = 0; pair < phase[row].size(); ++pair) {
        const auto index = (head * 6 + row) * 8 + pair * 2;
        const std::complex<double> value(input[index], input[index + 1]);
        const auto rotated = value * std::polar(1.0, phase[row][pair]);
        BOOST_CHECK_SMALL(result[index] - rotated.real(), 2e-7);
        BOOST_CHECK_SMALL(result[index + 1] - rotated.imag(), 2e-7);
        if (row == phase.size() - 1) {
          BOOST_CHECK_EQUAL(result[index], input[index]);
          BOOST_CHECK_EQUAL(result[index + 1], input[index + 1]);
        }
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(SelfAttentionRotatesQueriesAgainstTheSamePreparedContext)
{
  constexpr VideoRotaryTile context{1, 1, 2, 1, 6, 0, 2};
  const auto keyModel = makeVideoRotaryEmbedding(context, 96);
  const auto keyWire = keyModel.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-attention-rotary");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session keySession(env, keyWire.data(), keyWire.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 12> key{};
  key[4] = key[10] = 1;
  const std::array<std::int64_t, 4> contextShape{1, 1, 2, 6};
  const auto rawKey = Ort::Value::CreateTensor<float>(memory, key.data(), key.size(),
                                                      contextShape.data(), contextShape.size());
  const char* keyInput = "input";
  const char* keyOutput = "rotated";
  auto contextResult = keySession.Run(Ort::RunOptions{}, &keyInput, &rawKey, 1, &keyOutput, 1);
  std::array<float, 6> query{0, 0, 0, 0, 1, 0};
  std::array<float, 12> values{2, 2, 2, 2, 2, 2, 7, 7, 7, 7, 7, 7};
  std::array<bool, 2> mask{true, true};
  const std::array<std::int64_t, 4> queryShape{1, 1, 1, 6};
  const std::array<std::int64_t, 4> maskShape{1, 1, 1, 2};
  const std::array<Ort::Value, 4> inputs{
    Ort::Value::CreateTensor<float>(memory, query.data(), query.size(),
                                    queryShape.data(), queryShape.size()),
    std::move(contextResult.front()),
    Ort::Value::CreateTensor<float>(memory, values.data(), values.size(),
                                    contextShape.data(), contextShape.size()),
    Ort::Value::CreateTensor<bool>(memory, mask.data(), mask.size(),
                                   maskShape.data(), maskShape.size())
  };
  const std::array<const char*, 4> ports{
    "query-unrotated", "context-key", "context-value", "context-valid"
  };
  const char* output = "attention";
  const auto matching = std::exp(1.0 / std::sqrt(6.0));
  const auto other = std::exp(std::cos(1.0) / std::sqrt(6.0));
  for (std::int64_t position = 0; position < 2; ++position) {
    const VideoRotaryTile tile{1, 1, 2, 1, 6, position, 1};
    const auto graph = makeVideoAttentionQueryBlock(tile, 2, 1, sizeof(float), 48);
    const auto wire = graph.SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const auto result = session.Run(Ort::RunOptions{}, ports.data(), inputs.data(),
                                    inputs.size(), &output, 1);
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), 6);
    const auto expected = position == 0 ? (2 * matching + 7 * other) / (matching + other)
                                        : (7 * matching + 2 * other) / (matching + other);
    const auto* actual = result.front().GetTensorData<float>();
    for (std::size_t channel = 0; channel < query.size(); ++channel) {
      BOOST_CHECK_SMALL(actual[channel] - expected, 1e-6);
    }
  }
  const VideoRotaryTile outside{1, 1, 2, 1, 6, 2, 1};
  BOOST_CHECK_THROW(makeVideoAttentionQueryBlock(outside, 2, 1, 4, 48), std::invalid_argument);
  const VideoRotaryTile overlongGrid{1, 1, 3, 1, 6, 0, 1};
  BOOST_CHECK_THROW(makeVideoAttentionQueryBlock(overlongGrid, 2, 1, 4, 48), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(VideoContextEmitsRankLocalNormalizedRotatedKeysAndValues)
{
  const auto fragment = [] (bool key) {
    onnx::ModelProto model;
    model.set_ir_version(9);
    model.add_opset_import()->set_version(18);
    auto* graph = model.mutable_graph();
    graph->set_name("projection");
    const auto port = [] (onnx::ValueInfoProto& value, const char* name, int width) {
      value.set_name(name);
      auto* tensor = value.mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto::FLOAT);
      for (const auto extent : {1, 2, width}) tensor->mutable_shape()->add_dim()->set_dim_value(extent);
    };
    port(*graph->add_input(), "input", 1);
    port(*graph->add_output(), "output", 12);
    const auto parameter = [&] (const char* name, std::initializer_list<int> dims) {
      auto* value = graph->add_initializer();
      value->set_name(name);
      value->set_data_type(onnx::TensorProto::FLOAT);
      for (auto dim : dims) value->add_dims(dim);
      return value;
    };
    auto* weights = parameter("weights", {1, 12});
    auto* bias = parameter("bias", {12});
    for (int i = 1; i <= 12; ++i) {
      weights->add_float_data(key ? i : 2 * i);
      bias->add_float_data(key ? 0.25F : -1.0F);
    }
    const auto emit = [&] (const char* op, const char* output,
                           std::initializer_list<const char*> inputs) {
      auto* node = graph->add_node();
      node->set_op_type(op);
      for (const auto* name : inputs) node->add_input(name);
      node->add_output(output);
    };
    emit("MatMul", "linear", {"input", "weights"});
    emit("Add", key ? "biased" : "output", {"linear", "bias"});
    if (key) {
      auto* axis = graph->add_initializer();
      axis->set_name("axes");
      axis->set_data_type(onnx::TensorProto::INT64);
      axis->add_dims(1);
      axis->add_int64_data(-1);
      parameter("epsilon", {})->add_float_data(0.125F);
      auto* gain = parameter("gain", {12});
      for (int i = 0; i < 12; ++i) gain->add_float_data(i + 1);
      emit("Mul", "squares", {"biased", "biased"});
      emit("ReduceMean", "mean", {"squares", "axes"});
      emit("Add", "stabilized", {"mean", "epsilon"});
      emit("Sqrt", "rms", {"stabilized"});
      emit("Div", "normalized", {"biased", "rms"});
      emit("Mul", "output", {"normalized", "gain"});
    }
    return model;
  };
  const auto key = fragment(true);
  const auto value = fragment(false);
  const VideoRotaryTile context{1, 1, 2, 2, 6, 0, 2};
  const auto compiledLayout = lowerVideoAttentionContext(key, value, context, 192);
  const auto& compiled = compiledLayout.model;
  BOOST_REQUIRE_EQUAL(compiled.graph().output_size(), 4);
  for (const auto& node : compiled.graph().node()) {
    if (node.op_type() == "Concat") {
      BOOST_CHECK(node.name().find("/rotary/") != std::string::npos);
    }
  }
  BOOST_CHECK_THROW(lowerVideoAttentionContext(key, value, context, 191), std::invalid_argument);
  auto wrong = context;
  wrong.heads = 3;
  BOOST_CHECK_THROW(lowerVideoAttentionContext(key, value, wrong, 288), std::invalid_argument);
  auto wrongKey = key;
  auto wrongValue = value;
  for (auto* source : {&wrongKey, &wrongValue}) {
    source->mutable_graph()->mutable_input(0)->mutable_type()->mutable_tensor_type()
      ->mutable_shape()->mutable_dim(2)->set_dim_value(2);
  }
  BOOST_CHECK_THROW(lowerVideoAttentionContext(wrongKey, wrongValue, context, 192),
                    std::invalid_argument);

  const auto bytes = compiled.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-video-context");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, bytes.data(), bytes.size(), options);
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 2> hidden{1.0F, -0.5F};
  const std::array<std::int64_t, 3> shape{1, 2, 1};
  const auto tensor = Ort::Value::CreateTensor<float>(memory, hidden.data(), hidden.size(),
                                                      shape.data(), shape.size());
  const char* input = "hidden";
  const std::array<const char*, 4> outputs{
    "key-rank-0", "value-rank-0", "key-rank-1", "value-rank-1"
  };
  auto result = session.Run(Ort::RunOptions{}, &input, &tensor, 1,
                                  outputs.data(), outputs.size());
  for (const auto& tensor : result) {
    BOOST_CHECK(tensor.GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 1, 2, 6}));
  }
  for (std::size_t token = 0; token < hidden.size(); ++token) {
    std::array<double, 12> keyVector{};
    double squareSum = 0;
    for (std::size_t channel = 0; channel < keyVector.size(); ++channel) {
      keyVector[channel] = static_cast<double>(hidden[token]) * (channel + 1) + 0.25;
      squareSum += keyVector[channel] * keyVector[channel];
    }
    const auto rms = std::sqrt(squareSum / keyVector.size() + 0.125);
    for (std::size_t rank = 0; rank < 2; ++rank) {
      const auto* actualKey = result[2 * rank].GetTensorData<float>() + token * 6;
      const auto* actualValue = result[2 * rank + 1].GetTensorData<float>() + token * 6;
      for (std::size_t pair = 0; pair < 3; ++pair) {
        const auto channel = rank * 6 + pair * 2;
        const std::complex<double> norm{keyVector[channel] * (channel + 1) / rms,
                                        keyVector[channel + 1] * (channel + 2) / rms};
        const auto expected = norm * std::polar(1.0, pair == 2 ? static_cast<double>(token) : 0.0);
        BOOST_CHECK_SMALL(actualKey[pair * 2] - expected.real(), 2e-6);
        BOOST_CHECK_SMALL(actualKey[pair * 2 + 1] - expected.imag(), 2e-6);
        for (std::size_t part = 0; part < 2; ++part) {
          BOOST_CHECK_EQUAL(actualValue[pair * 2 + part],
                            hidden[token] * (2 * (channel + part + 1)) - 1);
        }
      }
    }
  }

  auto outputProjection = fragment(false);
  auto* outputGraph = outputProjection.mutable_graph();
  outputGraph->mutable_input(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(12);
  outputGraph->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(1);
  outputGraph->mutable_initializer(0)->set_dims(0, 12);
  outputGraph->mutable_initializer(0)->set_dims(1, 1);
  auto* outputBias = outputGraph->mutable_initializer(1);
  outputBias->set_dims(0, 1);
  outputBias->clear_float_data();
  outputBias->add_float_data(-1);

  const auto queryLayout = lowerVideoAttentionQuery(key, outputProjection, context, 2, 1, 8, 192);
  const auto& queryGraph = queryLayout.model;
  const auto queryWire = queryGraph.SerializeAsString();
  Ort::Session querySession(env, queryWire.data(), queryWire.size(), options);
  std::array<bool, 2> mask{true, true};
  const std::array<std::int64_t, 4> maskShape{1, 1, 1, 2};
  for (const auto& rank : queryLayout.ranks) {
    const auto& input = rank.attend.inputs.back();
    BOOST_CHECK_EQUAL(input.name, "valid");
    BOOST_REQUIRE_EQUAL(input.shape.size(), maskShape.size());
    for (std::size_t i = 0; i < maskShape.size(); ++i)
      BOOST_CHECK_EQUAL(std::get<std::int64_t>(input.shape[i]), maskShape[i]);
  }
  const std::array<const char*, 6> queryPorts{
    "hidden", "key-rank-0", "value-rank-0", "key-rank-1", "value-rank-1", "valid"
  };
  const std::array<Ort::Value, 6> queryInputs{
    Ort::Value::CreateTensor<float>(memory, hidden.data(), hidden.size(),
                                    shape.data(), shape.size()),
    std::move(result[0]), std::move(result[1]), std::move(result[2]), std::move(result[3]),
    Ort::Value::CreateTensor<bool>(memory, mask.data(), mask.size(),
                                   maskShape.data(), maskShape.size())
  };
  const char* outputName = "output";
  const auto projected = querySession.Run(Ort::RunOptions{}, queryPorts.data(), queryInputs.data(),
                                          queryInputs.size(), &outputName, 1);
  BOOST_REQUIRE_EQUAL(projected.front().GetTensorTypeAndShapeInfo().GetElementCount(), 2);
  const auto* predictions = projected.front().GetTensorData<float>();

  std::array<std::array<std::complex<double>, 6>, 2> qk{};
  for (std::size_t token = 0; token < hidden.size(); ++token) {
    std::array<double, 12> projection{};
    double sum = 0;
    for (std::size_t channel = 0; channel < projection.size(); ++channel) {
      projection[channel] = hidden[token] * (channel + 1) + 0.25;
      sum += projection[channel] * projection[channel];
    }
    const double divisor = std::sqrt(sum / 12 + 0.125);
    for (std::size_t pair = 0; pair < qk[token].size(); ++pair) {
      const auto channel = 2 * pair;
      qk[token][pair] = std::complex<double>{projection[channel] * (channel + 1),
                                            projection[channel + 1] * (channel + 2)} /
                        divisor * std::polar(1.0, pair % 3 == 2 ? double(token) : 0.0);
    }
  }
  for (std::size_t query = 0; query < hidden.size(); ++query) {
    double expected = -1;
    for (std::size_t rank = 0; rank < 2; ++rank) {
      std::array<double, 2> score{};
      for (std::size_t token = 0; token < hidden.size(); ++token) {
        for (std::size_t pair = 3 * rank; pair < 3 * (rank + 1); ++pair) {
          score[token] += std::real(qk[query][pair] * std::conj(qk[token][pair])) /
                          std::sqrt(6.0);
        }
      }
      const auto max = std::max(score[0], score[1]);
      const auto first = std::exp(score[0] - max);
      const auto second = std::exp(score[1] - max);
      for (std::size_t channel = 6 * rank; channel < 6 * (rank + 1); ++channel) {
        const double coefficient = 2 * (channel + 1);
        const auto value = (first * (hidden[0] * coefficient - 1) +
                            second * (hidden[1] * coefficient - 1)) / (first + second);
        expected += coefficient * value;
      }
    }
    BOOST_CHECK_SMALL(predictions[query] - expected, 0.001);
  }


  // Wan text padding is projected before K/V. Its bias produces real K/V
  // values, so passing an UMT5 mask through would change the result.
  auto textKey = key;
  auto* textGain = textKey.mutable_graph()->mutable_initializer(4);
  for (int i = 0; i < 12; ++i) textGain->set_float_data(i, 1);
  const auto textLayout = lowerTextAttentionContext(textKey, value, 2, 6);
  const auto& textContext = textLayout.model;
  const auto textWire = textContext.SerializeAsString();
  Ort::Session textSession(env, textWire.data(), textWire.size(), options);
  std::array<float, 2> textHidden{1, 0};
  const auto textTensor = Ort::Value::CreateTensor<float>(memory, textHidden.data(), textHidden.size(),
                                                          shape.data(), shape.size());
  auto textResult = textSession.Run(Ort::RunOptions{}, &input, &textTensor, 1,
                                    outputs.data(), outputs.size());

  auto textQuery = textKey;
  const auto setSingleToken = [] (onnx::ValueInfoProto& port) {
    port.mutable_type()->mutable_tensor_type()->mutable_shape()->mutable_dim(1)->set_dim_value(1);
  };
  for (auto* projection : {&textQuery, &outputProjection}) {
    setSingleToken(*projection->mutable_graph()->mutable_input(0));
    setSingleToken(*projection->mutable_graph()->mutable_output(0));
  }
  const auto crossLayout = lowerTextAttentionQuery(textQuery, outputProjection, 2, 6, 2, 1, 4);
  const auto& cross = crossLayout.model;
  BOOST_REQUIRE_EQUAL(cross.graph().input_size(), 5);
  const auto crossWire = cross.SerializeAsString();
  Ort::Session crossSession(env, crossWire.data(), crossWire.size(), options);
  std::array<float, 1> singleQuery{1};
  const std::array<std::int64_t, 3> singleShape{1, 1, 1};
  const std::array<Ort::Value, 5> crossInputs{
    Ort::Value::CreateTensor<float>(memory, singleQuery.data(), singleQuery.size(),
                                    singleShape.data(), singleShape.size()),
    std::move(textResult[0]), std::move(textResult[1]),
    std::move(textResult[2]), std::move(textResult[3])
  };
  const auto crossResult = crossSession.Run(Ort::RunOptions{}, queryPorts.data(), crossInputs.data(),
                                            crossInputs.size(), &outputName, 1);
  BOOST_REQUIRE_EQUAL(crossResult.front().GetTensorTypeAndShapeInfo().GetElementCount(), 1);
  const auto referenceKey = [] (double hidden) {
    std::array<double, 12> data{};
    double square = 0;
    for (std::size_t channel = 0; channel < data.size(); ++channel) {
      data[channel] = hidden * (channel + 1) + 0.25;
      square += data[channel] * data[channel];
    }
    for (auto& value : data) value /= std::sqrt(square / 12 + 0.125);
    return data;
  };
  const auto firstKey = referenceKey(1);
  const auto paddedKey = referenceKey(0);
  double expectedCross = -1;
  double withoutPadding = -1;
  for (std::size_t rank = 0; rank < 2; ++rank) {
    double tokenScore = 0;
    double paddedScore = 0;
    for (std::size_t c = 6 * rank; c < 6 * (rank + 1); ++c) {
      tokenScore += firstKey[c] * firstKey[c] / std::sqrt(6.0);
      paddedScore += firstKey[c] * paddedKey[c] / std::sqrt(6.0);
    }
    const auto nonPaddingWeight = 1.0 / (1 + std::exp(paddedScore - tokenScore));
    for (std::size_t c = 6 * rank; c < 6 * (rank + 1); ++c) {
      const double coefficient = 2 * (c + 1);
      expectedCross += coefficient * (nonPaddingWeight * coefficient - 1);
      withoutPadding += coefficient * (coefficient - 1);
    }
  }
  BOOST_CHECK_SMALL(crossResult.front().GetTensorData<float>()[0] - expectedCross, 0.001);
  BOOST_CHECK_GT(std::abs(expectedCross - withoutPadding), 1);

  const auto splitAttention = [&] (const AttentionContextLayout& contextLayout,
                                   const AttentionQueryLayout& queryPlan,
                                   std::vector<float> contextInput,
                                   std::vector<float> queryInput) {
    const NativeAssemblyControl control{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
    const auto prepare = [&] (const onnx::ModelProto& model) {
      NativeCanonicalSource source;
      const auto bytes = model.SerializeAsString();
      source.modelBytes.assign(bytes.begin(), bytes.end());
      const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
      BOOST_REQUIRE(manifest);
      const auto json = manifest->canonicalJson();
      return std::make_pair(manifest, parseNativeCanonicalMaterialManifest(
        std::vector<std::uint8_t>(json.begin(), json.end())));
    };
    const auto contextPackage = prepare(contextLayout.model);
    const auto queryPackage = prepare(queryPlan.model);
    std::map<std::string, std::vector<float>> ctx{{"hidden", std::move(contextInput)}};
    std::map<std::string, std::vector<float>> qry{{"hidden", std::move(queryInput)}};
    const auto execute = [&] (const auto& pack, const OnnxEntryCut& entry, int rank,
                               auto& data) {
      const auto opposite = "rank-" + std::to_string(1 - rank);
      auto selected = selectCanonicalEntry(*pack.first, pack.second, entry,
        [&] (const std::string& name) {
          if (rank == 2) return name == "output/ndnsf-wan-head/bias";
          return name.find(opposite) == std::string::npos &&
            name != "output/ndnsf-wan-head/bias";
        });
      const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices,
                                                          entry.inputs, entry.outputs, control);
      onnx::ModelProto subgraph;
      BOOST_REQUIRE(subgraph.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
      for (const auto& initializer : subgraph.graph().initializer()) {
        if (rank == 2) {
          BOOST_CHECK_EQUAL(initializer.name(), "output/ndnsf-wan-head/bias");
        }
        else {
          BOOST_CHECK(initializer.name().find(opposite) == std::string::npos);
          BOOST_CHECK_NE(initializer.name(), "output/ndnsf-wan-head/bias");
        }
      }
      Ort::Session runner(env, bytes.data(), bytes.size(), options);
      std::vector<Ort::Value> args;
      std::vector<const char*> ins, outs;
      for (const auto& port : entry.inputs) {
        std::vector<std::int64_t> dims;
        for (const auto& extent : port.shape) dims.push_back(std::get<std::int64_t>(extent));
        if (port.dtype == "bool") {
          BOOST_REQUIRE_EQUAL(port.name, "valid");
          args.push_back(Ort::Value::CreateTensor<bool>(memory, mask.data(), mask.size(),
                                                        dims.data(), dims.size()));
        }
        else {
          auto& values = data.at(port.name);
          args.push_back(Ort::Value::CreateTensor<float>(memory, values.data(), values.size(),
                                                         dims.data(), dims.size()));
        }
        ins.push_back(port.name.c_str());
      }
      for (const auto& port : entry.outputs) outs.push_back(port.name.c_str());
      const auto result = runner.Run(Ort::RunOptions{}, ins.data(), args.data(), args.size(),
                                      outs.data(), outs.size());
      for (std::size_t i = 0; i < result.size(); ++i) {
        const auto* values = result[i].template GetTensorData<float>();
        const auto count = result[i].GetTensorTypeAndShapeInfo().GetElementCount();
        data[entry.outputs[i].name].assign(values, values + count);
      }
    };
    const auto reduce = [] (auto& data, const NativeAssemblyTensorContractV3& output,
                             const NativeAssemblyTensorContractV3& first,
                             const NativeAssemblyTensorContractV3& second) {
      std::vector<std::int64_t> dims;
      for (const auto& extent : output.shape) dims.push_back(std::get<std::int64_t>(extent));
      std::vector<std::vector<NamedTensorView>> operands;
      for (const auto* port : {&first, &second}) {
        BOOST_REQUIRE(port->shape == output.shape);
        const auto& values = data.at(port->name);
        const auto* raw = reinterpret_cast<const std::uint8_t*>(values.data());
        operands.push_back({{output.name, TensorElementType::Float32, dims,
                             {raw, values.size() * sizeof(float)}}});
      }
      const auto sum = sumTensorBundleViews(output.name, operands);
      const auto views = decodeTensorBundleViews(sum);
      BOOST_REQUIRE_EQUAL(views.size(), 1);
      const auto& bytes = views.front().payload;
      auto& merged = data[output.name];
      merged.resize(bytes.size() / sizeof(float));
      std::memcpy(merged.data(), bytes.data(), bytes.size());
    };
    for (int rank = 0; rank < 2; ++rank) {
      execute(contextPackage, contextLayout.ranks[rank].project, rank, ctx);
      execute(queryPackage, queryPlan.ranks[rank].project, rank, qry);
    }
    reduce(ctx, contextLayout.ranks[0].prepare.inputs[1],
           contextLayout.ranks[0].project.outputs[1], contextLayout.ranks[1].project.outputs[1]);
    reduce(qry, queryPlan.ranks[0].attend.inputs[1],
           queryPlan.ranks[0].project.outputs[1], queryPlan.ranks[1].project.outputs[1]);
    for (int rank = 0; rank < 2; ++rank) {
      const auto& entry = contextLayout.ranks[rank].prepare;
      execute(contextPackage, entry, rank, ctx);
      for (const auto& port : entry.outputs) qry.emplace(port.name, std::move(ctx.at(port.name)));
      execute(queryPackage, queryPlan.ranks[rank].attend, rank, qry);
    }
    reduce(qry, queryPlan.finish.inputs.front(), queryPlan.ranks[0].attend.outputs.front(),
           queryPlan.ranks[1].attend.outputs.front());
    execute(queryPackage, queryPlan.finish, 2, qry);
    return qry.at("output");
  };
  const auto splitSelf = splitAttention(compiledLayout, queryLayout,
    {hidden.begin(), hidden.end()}, {hidden.begin(), hidden.end()});
  BOOST_REQUIRE_EQUAL(splitSelf.size(), hidden.size());
  for (std::size_t i = 0; i < splitSelf.size(); ++i) {
    BOOST_CHECK_SMALL(splitSelf[i] - predictions[i], 0.001F);
  }
  const auto splitCross = splitAttention(textLayout, crossLayout,
    {textHidden.begin(), textHidden.end()}, {singleQuery.begin(), singleQuery.end()});
  BOOST_REQUIRE_EQUAL(splitCross.size(), 1);
  BOOST_CHECK_SMALL(splitCross[0] - expectedCross, 0.001);
}

BOOST_AUTO_TEST_CASE(TimeEmbeddingKeepsSixModulationGroupsInRankOrder)
{
  const auto initializer = [] (int seed, std::initializer_list<int> dims) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const int extent : dims) {
      tensor.add_dims(extent);
      count *= extent;
    }
    for (int i = 0; i < count; ++i) {
      tensor.add_float_data(((i + seed) % 9 - 4) / 8.0F);
    }
    return tensor;
  };
  const auto up = initializer(0, {4, 3});
  const auto upBias = initializer(1, {3});
  const auto down = initializer(2, {3, 3});
  const auto downBias = initializer(3, {3});
  const auto modulation = initializer(4, {3, 18});
  const auto modulationBias = initializer(5, {18});
  const auto layout = lowerTimeEmbedding(
    {{{up, upBias}, {down, downBias}, {modulation, modulationBias}}});
  auto oddUp = initializer(0, {3, 3});
  BOOST_CHECK_THROW(lowerTimeEmbedding(
    {{{oddUp, upBias}, {down, downBias}, {modulation, modulationBias}}}), std::invalid_argument);

  const auto wire = layout.embedding.model.SerializeAsString();
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-time-embedding");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, wire.data(), wire.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<std::int64_t, 1> timestep{7};
  const std::array<std::int64_t, 1> shape{1};
  const auto input = Ort::Value::CreateTensor<std::int64_t>(memory, timestep.data(), timestep.size(),
                                                           shape.data(), shape.size());
  const char* inputName = "timestep";
  const std::array<const char*, 3> outputs{"embedding", "modulation-rank-0", "modulation-rank-1"};
  const auto result = session.Run(Ort::RunOptions{}, &inputName, &input, 1,
                                  outputs.data(), outputs.size());
  const std::array<std::vector<std::int64_t>, 3> expectedShape{{{1, 3}, {1, 6, 2}, {1, 6, 1}}};
  for (std::size_t i = 0; i < result.size(); ++i) {
    BOOST_CHECK(result[i].GetTensorTypeAndShapeInfo().GetShape() == expectedShape[i]);
  }
  const std::vector<double> frequencies{std::cos(7.0), std::cos(0.07),
                                       std::sin(7.0), std::sin(0.07)};
  const auto linear = [] (const std::vector<double>& input, const auto& weight, const auto& bias) {
    const int channels = bias.float_data_size();
    std::vector<double> result(bias.float_data().begin(), bias.float_data().end());
    for (std::size_t row = 0; row < input.size(); ++row) {
      for (int col = 0; col < channels; ++col) {
        result[col] += input[row] * weight.float_data(static_cast<int>(row) * channels + col);
      }
    }
    return result;
  };
  const auto activate = [] (std::vector<double> data) {
    for (auto& x : data) x /= 1.0 + std::exp(-x);
    return data;
  };
  const auto activation = activate(linear(frequencies, up, upBias));
  const auto embedded = linear(activation, down, downBias);
  const auto expected = linear(activate(embedded), modulation, modulationBias);
  for (std::size_t channel = 0; channel < embedded.size(); ++channel) {
    BOOST_CHECK_SMALL(result[0].GetTensorData<float>()[channel] - embedded[channel], 1e-6);
  }
  for (std::size_t group = 0; group < 6; ++group) {
    const auto* first = result[1].GetTensorData<float>() + group * 2;
    const auto second = result[2].GetTensorData<float>()[group];
    BOOST_CHECK_SMALL(first[0] - expected[group * 3], 1e-6);
    BOOST_CHECK_SMALL(first[1] - expected[group * 3 + 1], 1e-6);
    BOOST_CHECK_SMALL(second - expected[group * 3 + 2], 1e-6);
  }

  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto manifestJson = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(manifestJson.begin(), manifestJson.end()));
  std::set<std::uint64_t> assignedNodes;
  const auto assemble = [&] (const OnnxEntryCut& entry, auto matchInitializer) {
    for (const auto node : entry.nodeIndices) {
      BOOST_CHECK(assignedNodes.insert(node).second);
    }
    auto source = selectCanonicalEntry(*manifest, index, entry, matchInitializer);
    return materializeNativeCanonicalModel(source, entry.nodeIndices, entry.inputs,
                                           entry.outputs, control);
  };
  const auto run = [] (Ort::Session& runner, const OnnxEntryCut& entry,
                       const Ort::Value& argument) {
    const auto* inName = entry.inputs.front().name.c_str();
    const auto* outName = entry.outputs.front().name.c_str();
    return runner.Run(Ort::RunOptions{}, &inName, &argument, 1, &outName, 1);
  };

  std::array<float, 3> reduced{};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& entry = layout.embedding.partials[rank];
    const auto prefix = "rank-" + std::to_string(rank) + "/";
    const auto bytes = assemble(entry, [&] (const std::string& name) {
      return name == "sinusoidal-table" || name == "first-step" || name == "past-last-step" ||
        name == prefix + "up-weight" || name == prefix + "up-bias" ||
        name == prefix + "down-weight";
    });
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    const auto values = run(runner, entry, input);
    BOOST_REQUIRE(values.front().GetTensorTypeAndShapeInfo().GetShape() == expectedShape[0]);
    for (std::size_t col = 0; col < reduced.size(); ++col) {
      double partial = 0;
      for (std::size_t row = rank == 0 ? 0 : 2; row < (rank == 0 ? 2 : 3); ++row) {
        partial += activation[row] * down.float_data(row * 3 + col);
      }
      const auto actual = values.front().GetTensorData<float>()[col];
      BOOST_CHECK_SMALL(actual - partial, 1e-6);
      reduced[col] += actual;
    }
    timestep[0] = -1;
    BOOST_CHECK_THROW(run(runner, entry, input), Ort::Exception);
    timestep[0] = 7;
  }

  const auto& biasEntry = layout.embedding.addBias;
  const auto biasBytes = assemble(biasEntry,
    [] (const std::string& name) { return name == "embedding-bias"; });
  Ort::Session biasRunner(env, biasBytes.data(), biasBytes.size(), options);
  const auto sumInput = Ort::Value::CreateTensor<float>(
    memory, reduced.data(), reduced.size(), expectedShape[0].data(), expectedShape[0].size());
  const auto head = run(biasRunner, biasEntry, sumInput);
  BOOST_REQUIRE(head.front().GetTensorTypeAndShapeInfo().GetShape() == expectedShape[0]);
  for (std::size_t col = 0; col < reduced.size(); ++col) {
    BOOST_CHECK_SMALL(head.front().GetTensorData<float>()[col] - embedded[col], 1e-6);
  }
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& entry = layout.modulation[rank];
    const auto prefix = "rank-" + std::to_string(rank) + "/group-";
    const auto bytes = assemble(entry, [&] (const std::string& name) {
      return name == "axis-1" || name.rfind(prefix, 0) == 0;
    });
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    const auto values = run(runner, entry, head.front());
    const auto dims = values.front().GetTensorTypeAndShapeInfo().GetShape();
    BOOST_REQUIRE(dims == expectedShape[rank + 1]);
    const auto width = static_cast<std::size_t>(dims.back());
    for (std::size_t group = 0; group < 6; ++group) {
      for (std::size_t col = 0; col < width; ++col) {
        const auto actual = values.front().GetTensorData<float>()[group * width + col];
        BOOST_CHECK_SMALL(actual - expected[group * 3 + 2 * rank + col], 1e-6);
      }
    }
  }
  // Only the whole-model Add, replaced by the local native SUM, is outside the five cuts.
  BOOST_CHECK_EQUAL(assignedNodes.size() + 1, layout.embedding.model.graph().node_size());

  for (const auto invalid : {-1, 1000}) {
    timestep[0] = invalid;
    BOOST_CHECK_THROW(session.Run(Ort::RunOptions{}, &inputName, &input, 1,
                                  outputs.data(), outputs.size()), Ort::Exception);
  }
}

BOOST_AUTO_TEST_CASE(VideoBlockAppliesSixGroupsAndThreeResidualsInSourceOrder)
{
  constexpr float epsilon = 1e-6F;
  constexpr int tokens = 2, hidden = 3;
  const auto tensor = [] (std::initializer_list<int> dims, const std::vector<float>& values) {
    onnx::TensorProto result;
    result.set_data_type(onnx::TensorProto::FLOAT);
    for (const auto dim : dims) result.add_dims(dim);
    for (const auto value : values) result.add_float_data(value);
    return result;
  };
  const auto modulation = tensor({1, 6, hidden}, {
    -1, 0.5, 0.25, 0.5, -0.5, 1, 0.25, 0, -1, 1, 0, -0.25, -1, 1, 0, 0, 0.5, 0.25});
  const auto crossScale = tensor({hidden}, {1.5, -2, 0.25});
  const auto crossBias = tensor({hidden}, {0.5, 0.125, -0.75});
  const auto layout = lowerVideoBlock(tokens, modulation, crossScale, crossBias, epsilon);
  BOOST_CHECK_THROW(lowerVideoBlock(0, modulation, crossScale, crossBias, epsilon),
                    std::invalid_argument);
  BOOST_CHECK_THROW(lowerVideoBlock(tokens, modulation, crossScale, tensor({1}, {0}), epsilon),
                    std::invalid_argument);

  std::map<std::string, std::vector<float>> values{
    {"hidden", {3, -2, 0.5, 1.5, 1.5, 1.5}},
    {"self-result", {2, 5, 1, -4, -0.125, 0.5}},
    {"cross-result", {0.25, -0.5, 1.75, 3, -2, 0.125}},
    {"ffn-result", {-1, 2, 4, 5, -4, 1}},
    {"modulation-rank-0", {0, 1, 1, 0.5, 0.5, 0, 0.25, 1, 0, -0.5, 1, -1}},
    {"modulation-rank-1", {0, 0.25, 0, 0.125, 1, -0.5}}};
  std::map<std::string, std::vector<double>> expected;
  for (const auto* name : {"self-input", "after-self", "cross-input", "after-cross",
                           "ffn-input", "block-output"}) {
    expected[name].resize(tokens * hidden);
  }
  const auto scale = [&] (int group, int col) {
    const auto time = col < 2 ? values.at("modulation-rank-0")[group * 2 + col]
                              : values.at("modulation-rank-1")[group];
    return static_cast<double>(time) + modulation.float_data(group * hidden + col);
  };
  const auto normRow = [] (const auto& row) {
    const auto mean = (row[0] + row[1] + row[2]) / hidden;
    std::array<double, hidden> norm{};
    double variance = 0;
    for (int col = 0; col < hidden; ++col) {
      norm[col] = row[col] - mean;
      variance += norm[col] * norm[col];
    }
    for (auto& val : norm) val /= std::sqrt(variance / hidden + epsilon);
    return norm;
  };
  for (int token = 0; token < tokens; ++token) {
    const auto base = token * hidden;
    std::array<double, hidden> row{};
    for (int col = 0; col < hidden; ++col) row[col] = values.at("hidden")[base + col];
    const auto norm1 = normRow(row);
    for (int col = 0; col < hidden; ++col) {
      const auto offset = base + col;
      expected["self-input"][offset] = norm1[col] * (1 + scale(1, col)) + scale(0, col);
      row[col] += values.at("self-result")[offset] * scale(2, col);
      expected["after-self"][offset] = row[col];
    }
    const auto norm3 = normRow(row);
    for (int col = 0; col < hidden; ++col) {
      const auto offset = base + col;
      expected["cross-input"][offset] = norm3[col] * crossScale.float_data(col) +
        crossBias.float_data(col);
      row[col] += values.at("cross-result")[offset];
      expected["after-cross"][offset] = row[col];
    }
    const auto norm2 = normRow(row);
    for (int col = 0; col < hidden; ++col) {
      const auto offset = base + col;
      expected["ffn-input"][offset] = norm2[col] * (1 + scale(4, col)) + scale(3, col);
      expected["block-output"][offset] = row[col] +
        values.at("ffn-result")[offset] * scale(5, col);
    }
  }

  const auto wire = layout.model.SerializeAsString();
  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto manifestIndex = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-video-block");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::set<std::uint64_t> covered;
  const auto execute = [&] (const OnnxEntryCut& entry, const std::string& prefix) {
    for (const auto node : entry.nodeIndices) BOOST_CHECK(covered.insert(node).second);
    auto selected = selectCanonicalEntry(*manifest, manifestIndex, entry,
      [&] (const std::string& name) {
        return !prefix.empty() && (name.rfind(prefix, 0) == 0 ||
          name.rfind("group-", 0) == 0 || name.rfind("channels-", 0) == 0 ||
          name == "hidden-axis" ||
          name == "norm-scale" || name == "one");
      });
    const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                                        entry.outputs, control);
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    std::vector<Ort::Value> arguments;
    std::vector<const char*> names;
    for (const auto& port : entry.inputs) {
      std::vector<std::int64_t> shape;
      for (const auto& dim : port.shape) shape.push_back(std::get<std::int64_t>(dim));
      auto& input = values.at(port.name);
      arguments.push_back(Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                          shape.data(), shape.size()));
      names.push_back(port.name.c_str());
    }
    const auto* outputName = entry.outputs.front().name.c_str();
    const auto output = runner.Run(Ort::RunOptions{}, names.data(), arguments.data(),
                                    arguments.size(), &outputName, 1);
    const auto* data = output.front().GetTensorData<float>();
    const auto count = output.front().GetTensorTypeAndShapeInfo().GetElementCount();
    values[entry.outputs.front().name].assign(data, data + count);
  };
  const auto stage = [&] (const VideoBlockStage& stage) {
    execute(stage.ranks[0], "rank-0/");
    execute(stage.ranks[1], "rank-1/");
    execute(stage.join, "");
  };
  execute(layout.modulation[0], "rank-0/");
  execute(layout.modulation[1], "rank-1/");
  stage(layout.selfInput);
  stage(layout.selfResidual);
  stage(layout.crossInput);
  execute(layout.crossResidual, "");
  stage(layout.ffnInput);
  stage(layout.ffnResidual);
  BOOST_CHECK_EQUAL(covered.size(), layout.model.graph().node_size());
  for (const auto& [name, reference] : expected) {
    const auto& actual = values.at(name);
    BOOST_REQUIRE_EQUAL(actual.size(), reference.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      BOOST_CHECK_SMALL(actual[i] - reference[i], 2e-6);
    }
  }
}

BOOST_AUTO_TEST_CASE(ConnectedVideoBlockRunsAttentionAndFeedForwardBeforeTheirResiduals)
{
  const auto projection = videoFixture::makeLinearSource;

  const auto qk = projection(2, 3, 12, {}, true);
  const auto o = projection(2, 12, 3, {{0, 0}, {1, 1}, {6, 2}}, false);
  const auto selfV = projection(2, 3, 12, {{0, 0}, {1, 1}, {2, 6}}, false);
  const VideoRotaryTile tile{1, 1, 2, 2, 6, 0, 2};
  const VideoBlockAttention self{
    lowerVideoAttentionContext(qk, selfV, tile, 192),
    lowerVideoAttentionQuery(qk, o, tile, 2, 1, 8, 192)};
  const VideoBlockAttention cross{
    lowerTextAttentionContext(projection(3, 3, 12, {}, true),
                              projection(3, 3, 12, {{0, 0}, {1, 1}, {2, 6}}, false), 2, 6),
    lowerTextAttentionQuery(qk, o, 2, 6, 3, 1, 8)};

  auto ffnSource = projection(2, 3, 4, {{0, 0}, {1, 1}, {2, 2}}, false);
  ffnSource.mutable_opset_import(0)->set_version(20);
  auto* middle = ffnSource.mutable_graph();
  middle->mutable_node(1)->set_output(0, "biased");
  auto* gelu = middle->add_node();
  gelu->set_op_type("Gelu");
  gelu->add_input("biased");
  gelu->add_output("gelu");
  auto* approximation = gelu->add_attribute();
  approximation->set_name("approximate");
  approximation->set_type(onnx::AttributeProto::STRING);
  approximation->set_s("tanh");
  const auto down = projection(2, 4, 3, {{0, 0}, {1, 1}, {2, 2}}, false);
  for (auto initializer : down.graph().initializer()) {
    initializer.set_name("down-" + initializer.name());
    *middle->add_initializer() = std::move(initializer);
  }
  auto* matmul = middle->add_node();
  *matmul = down.graph().node(0);
  matmul->set_input(0, "gelu");
  matmul->set_input(1, "down-weight");
  matmul->set_output(0, "contracted");
  auto* add = middle->add_node();
  *add = down.graph().node(1);
  add->set_input(0, "contracted");
  add->set_input(1, "down-bias");
  middle->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(3);
  const auto ffn = lowerFeedForward(ffnSource);

  onnx::TensorProto modulation, scale, bias;
  for (auto* param : {&modulation, &scale, &bias}) param->set_data_type(onnx::TensorProto::FLOAT);
  for (int extent : {1, 6, 3}) modulation.add_dims(extent);
  modulation.mutable_float_data()->Resize(18, 0);
  for (int channel = 0; channel < 3; ++channel) {
    modulation.set_float_data(6 + channel, 0.5F);
    modulation.set_float_data(15 + channel, 1.0F);
  }
  scale.add_dims(3);
  scale.mutable_float_data()->Resize(3, 1);
  bias.add_dims(3);
  bias.mutable_float_data()->Resize(3, 0);
  const auto conditioning = lowerVideoBlock(2, modulation, scale, bias, 1e-6F);
  const NativeStageLayout stage{"dit/block0", nativePlanningDigest("fixture/source-region"),
                                {"dit/rank0", "dit/rank1"}};
  const auto& roles = stage.unitRoleIds;
  const auto compiled = connectVideoBlock(conditioning, self, cross, ffn, stage);
  const auto& block = compiled.model;
  auto wrong = cross;
  wrong.context.model.mutable_graph()->mutable_output(0)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(2)->set_dim_value(2);
  BOOST_CHECK_THROW(connectVideoBlock(conditioning, self, wrong, ffn, stage),
                    std::invalid_argument);
  auto repeatedRole = stage;
  repeatedRole.unitRoleIds[1] = roles[0];
  BOOST_CHECK_THROW(connectVideoBlock(conditioning, self, cross, ffn, repeatedRole),
                    std::invalid_argument);
  BOOST_REQUIRE_EQUAL(block.graph().input_size(), 5);
  BOOST_REQUIRE_EQUAL(block.graph().output_size(), 1);
  BOOST_CHECK_EQUAL(block.graph().output(0).name(), "block-output");

  const auto wire = block.SerializeAsString();
  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
  canonical.materialManifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(canonical.materialManifest);
  const auto& plan = compiled.tensorParallel;
  BOOST_CHECK_NO_THROW(plan.validateSteps());
  BOOST_CHECK_EQUAL(plan.stepPlanVersion, NATIVE_STEP_PLAN_VERSION_V2);
  BOOST_REQUIRE_EQUAL(plan.stageLayout.size(), 1);
  BOOST_CHECK_EQUAL(plan.stageLayout.front().unitRoleIds.size(), 2);
  BOOST_CHECK_EQUAL(plan.stageLayout.front().sourceRegionDigest, stage.sourceRegionDigest);
  BOOST_CHECK_EQUAL(compiled.entries.size(), 42);
  BOOST_REQUIRE(!plan.dependencies.empty());
  const std::set<std::string> fullJoins{
    "self-input", "after-self", "cross-input", "ffn-input", "block-output"};
  const std::set<std::string> gatedSums{"self", "ffn"};
  std::map<std::string, std::set<std::string>> joinProducers, gatedConsumers;
  for (const auto& edge : plan.dependencies) {
    BOOST_CHECK_EQUAL(fullJoins.count(edge.producerSlotId), 0U);
    BOOST_CHECK(edge.producerSlotId.find("self-result") == std::string::npos);
    BOOST_CHECK(edge.producerSlotId.find("ffn-result") == std::string::npos);
    const auto join = edge.consumerStepId.find("/join/");
    if (join != std::string::npos) {
      joinProducers[edge.consumerStepId.substr(0, join)].insert(edge.producers.front());
    }
    for (const auto& projection : gatedSums) {
      if (edge.consumerStepId.find("/" + projection + "/sum/") == std::string::npos) continue;
      const std::size_t receiver = edge.consumers.front() == roles[0] ? 0 : 1;
      BOOST_CHECK_EQUAL(edge.producers.front(), roles[1 - receiver]);
      BOOST_CHECK_EQUAL(edge.operationKind, "TENSOR_PARTIAL");
      BOOST_REQUIRE(gatedConsumers[projection].insert(edge.consumers.front()).second);
      const auto producer = std::find_if(plan.steps.begin(), plan.steps.end(),
        [&](const auto& step) { return step.stepId == edge.producerStepId; });
      BOOST_REQUIRE(producer != plan.steps.end());
      const auto& entry = compiled.entries.at(producer->artifactEntry);
      BOOST_REQUIRE_EQUAL(entry.outputs.size(), 2);
      BOOST_CHECK_EQUAL(entry.outputs[receiver].name, edge.producerSlotId);
      BOOST_CHECK_EQUAL(std::get<std::int64_t>(entry.outputs[receiver].shape.back()), 2 - receiver);
    }
  }
  BOOST_CHECK_EQUAL(joinProducers.size(), 5);
  for (const auto& [name, producers] : joinProducers) {
    BOOST_CHECK_EQUAL(producers.size(), 2);
  }
  BOOST_CHECK_EQUAL(gatedConsumers.size(), 2);
  for (const auto& [name, consumers] : gatedConsumers) {
    BOOST_CHECK_EQUAL(consumers.size(), 2);
  }
  auto missingEdge = plan;
  missingEdge.dependencies.pop_back();
  BOOST_CHECK_THROW(missingEdge.validateSteps(), std::invalid_argument);
  const auto json = canonical.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  std::set<std::uint64_t> owned;
  std::size_t reductions = 0;
  for (const auto& step : plan.steps) {
    if (step.operation == "SUM") {
      ++reductions;
      BOOST_REQUIRE_EQUAL(step.inputSlots.size(), 2);
      BOOST_REQUIRE_EQUAL(step.outputSlots.size(), 1);
      BOOST_CHECK(step.inputSlots[0].shape == step.outputSlots[0].shape);
      BOOST_CHECK(step.inputSlots[1].shape == step.outputSlots[0].shape);
      continue;
    }
    const auto& entry = compiled.entries.at(step.artifactEntry);
    for (const auto node : entry.nodeIndices) BOOST_CHECK(owned.insert(node).second);
    const auto otherRank = step.unitId == roles[0] ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*canonical.materialManifest, index, entry,
      [&] (const std::string& initializer) {
        return initializer.find(otherRank) == std::string::npos;
      });
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(selected, entry.nodeIndices,
                          entry.inputs, entry.outputs, control));
  }
  BOOST_CHECK_EQUAL(reductions, 13);
  // Four K/Q Adds are each executed on both ranks.
  BOOST_CHECK_EQUAL(owned.size() + 9, static_cast<std::size_t>(block.graph().node_size()));
  canonical.materialPayloads = canonical.materialManifest->payloads;
  std::vector<std::uint64_t> nodes;
  for (int i = 0; i < block.graph().node_size(); ++i) nodes.push_back(i);
  const auto bytes = materializeNativeCanonicalModel(canonical, nodes, control);

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-connected-block");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, bytes.data(), bytes.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 6> hidden{3, -2, 0.5, -2, 2, 5};
  std::array<float, 9> text{3, -1, 2, 1, 4, -1, 2, 0, 2};
  std::array<float, 12> time0{};
  std::array<float, 6> time1{};
  std::array<bool, 2> mask{true, false};
  const auto tensor = [&] (auto& values, std::initializer_list<std::int64_t> shape) {
    return Ort::Value::CreateTensor(memory, values.data(), values.size(), shape.begin(),
                                    shape.size());
  };
  const std::array<const char*, 5> inputNames{
    "hidden", "text-context", "modulation-rank-0", "modulation-rank-1", "video-valid"};
  const std::array<Ort::Value, 5> inputs{
    tensor(hidden, {1, 2, 3}), tensor(text, {1, 3, 3}), tensor(time0, {1, 6, 2}),
    tensor(time1, {1, 6, 1}), tensor(mask, {1, 1, 1, 2})};
  const char* out = "block-output";
  const auto result = session.Run(Ort::RunOptions{}, inputNames.data(), inputs.data(),
                                  inputs.size(), &out, 1);
  BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 2, 3}));
  const auto normalize = [] (const auto* row) {
    const double mean = (double(row[0]) + row[1] + row[2]) / 3;
    std::array<double, 3> centered{row[0] - mean, row[1] - mean, row[2] - mean};
    const auto variance = (centered[0] * centered[0] + centered[1] * centered[1] +
                           centered[2] * centered[2]) / 3;
    for (auto& v : centered) v /= std::sqrt(variance + 1e-6);
    return centered;
  };
  const auto norm0 = normalize(hidden.data());
  for (int token = 0; token < 2; ++token) {
    std::array<double, 3> residual{};
    for (int ch = 0; ch < 3; ++ch) {
      const auto selfTerm = norm0[ch];
      const auto textMean = (text[ch] + text[ch + 3] + text[ch + 6]) / 3.0;
      residual[ch] = hidden[token * 3 + ch] + 0.5 * selfTerm + textMean;
    }
    const auto ffnInput = normalize(residual.data());
    for (int ch = 0; ch < 3; ++ch) {
      const auto x = ffnInput[ch];
      const auto geluValue = 0.5 * x *
        (1 + std::tanh(std::sqrt(2 / std::acos(-1.0)) * (x + 0.044715 * x * x * x)));
      BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[token * 3 + ch] -
                        (residual[ch] + geluValue), 2e-6);
    }
  }
  const auto zeros = [] (std::initializer_list<int> shape) {
    onnx::TensorProto parameter;
    parameter.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto size : shape) {
      parameter.add_dims(size);
      count *= size;
    }
    parameter.mutable_float_data()->Resize(count, 0);
    return parameter;
  };
  onnx::ModelProto patchSource;
  patchSource.set_ir_version(9);
  patchSource.add_opset_import()->set_version(17);
  auto* patchGraph = patchSource.mutable_graph();
  patchGraph->set_name("wan-denoiser-patch");
  const auto declarePatch = [] (onnx::ValueInfoProto* port, const char* name,
                                std::initializer_list<int> shape) {
    port->set_name(name);
    auto* dtype = port->mutable_type()->mutable_tensor_type();
    dtype->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dim : shape) dtype->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  declarePatch(patchGraph->add_input(), "latent", {1, 2, 1, 2, 4});
  declarePatch(patchGraph->add_output(), "projection", {1, 3, 1, 1, 2});
  auto* patchWeight = patchGraph->add_initializer();
  *patchWeight = zeros({3, 2, 1, 2, 2});
  patchWeight->set_name("weight");
  patchWeight->set_float_data(0, 1);
  patchWeight->set_float_data(9, 1);
  patchWeight->set_float_data(20, 1);
  auto* patchBias = patchGraph->add_initializer();
  *patchBias = zeros({3});
  patchBias->set_name("bias");
  auto* patchConv = patchGraph->add_node();
  patchConv->set_name("patch_embedding");
  patchConv->set_op_type("Conv");
  for (const auto* input : {"latent", "weight", "bias"}) patchConv->add_input(input);
  patchConv->add_output("projection");
  auto* strides = patchConv->add_attribute();
  strides->set_name("strides");
  strides->set_type(onnx::AttributeProto::INTS);
  for (const auto size : {1, 2, 2}) strides->add_ints(size);
  const auto patch = lowerPatchEmbedding(patchSource);
  auto textSource = ffnSource;
  for (auto* port : {textSource.mutable_graph()->mutable_input(0),
                     textSource.mutable_graph()->mutable_output(0)}) {
    port->mutable_type()->mutable_tensor_type()->mutable_shape()->mutable_dim(1)
      ->set_dim_param("valid_tokens");
  }
  for (auto& parameter : *textSource.mutable_graph()->mutable_initializer()) {
    if (parameter.name() == "weight" || parameter.name() == "down-weight") {
      parameter.set_dims(0, 3);
      parameter.set_dims(1, 3);
      parameter.clear_float_data();
      for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) parameter.add_float_data(row == column ? 1 : 0);
      }
    }
    else if (parameter.name() == "bias") {
      parameter.set_dims(0, 3);
      parameter.mutable_float_data()->Truncate(3);
    }
  }
  const auto textProjection = lowerTextEmbedding(textSource, 3);

  const auto timeUp = zeros({2, 3}), timeUpBias = zeros({3}), timeDown = zeros({3, 3});
  auto timeDownBias = zeros({3});
  timeDownBias.set_float_data(0, 0.25F);
  timeDownBias.set_float_data(1, -0.5F);
  timeDownBias.set_float_data(2, 0.125F);
  const auto timeMod = zeros({3, 18}), timeModBias = zeros({18});
  const auto time = lowerTimeEmbedding({{
    {timeUp, timeUpBias}, {timeDown, timeDownBias}, {timeMod, timeModBias}}});
  const auto headMod = zeros({1, 2, 3});
  auto headWeight = zeros({3, 8});
  for (int c = 0; c < 3; ++c) headWeight.set_float_data(10 * c, 1);
  auto headBias = zeros({8});
  headBias.set_float_data(6, 0.375F);
  for (int position = 1; position < 8; position += 2) headBias.set_float_data(position, 0.25F);
  const VideoHeadGrid grid{1, 1, 2, 2};
  const auto head = lowerVideoHead(grid, headMod, headWeight, headBias, 1e-6F);

  std::vector<ConnectedVideoBlock> blocks;
  for (const float gate : {0.25F, 0.5F, 0.75F}) {
    auto parameter = modulation;
    for (int c = 0; c < 3; ++c) parameter.set_float_data(6 + c, gate);
    blocks.push_back(connectVideoBlock(lowerVideoBlock(2, parameter, scale, bias, 1e-6F),
                                       self, cross, ffn, stage));
  }
  const auto denoiser = connectVideoDenoiser(patch, textProjection, time, blocks, head, grid, stage);
  const NativeStageLayout prefix{"dit-prefix", nativePlanningDigest("fixture/dit-prefix"),
                                  {"dit-prefix/pp"}};
  const NativeStageLayout suffix{"dit-suffix", nativePlanningDigest("fixture/dit-suffix"),
                                  {"dit-suffix/pp"}};
  auto prefixTp = prefix, suffixTp = suffix;
  prefixTp.unitRoleIds = {"dit-prefix/rank0", "dit-prefix/rank1"};
  suffixTp.unitRoleIds = {"dit-suffix/rank0", "dit-suffix/rank1"};
  const auto pp = makeVideoDenoiserStagedPlan(denoiser, 1, {prefix, suffix});
  const auto ppTp = makeVideoDenoiserStagedPlan(denoiser, 1, {prefix, suffixTp});
  const auto tpPp = makeVideoDenoiserStagedPlan(denoiser, 2, {prefixTp, suffix});
  BOOST_CHECK_THROW(makeVideoDenoiserStagedPlan(denoiser, 0, {prefix, suffix}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(makeVideoDenoiserStagedPlan(denoiser, 1, {prefixTp, suffixTp}),
                    std::invalid_argument);
  auto duplicate = suffixTp;
  duplicate.unitRoleIds[0] = prefix.unitRoleIds[0];
  BOOST_CHECK_THROW(makeVideoDenoiserStagedPlan(denoiser, 1, {prefix, duplicate}),
                    std::invalid_argument);
  BOOST_REQUIRE_EQUAL(pp.entries.size(), 2);
  BOOST_CHECK_EQUAL(pp.plan.tensorDegree, 1);
  BOOST_CHECK_EQUAL(ppTp.plan.roles.size(), 3);
  BOOST_CHECK_EQUAL(tpPp.plan.roles.size(), 3);
  std::set<std::string> hiddenConsumers;
  for (const auto& dependency : ppTp.plan.dependencies) {
    if (dependency.tensors != std::vector<std::string>{"block-0/block-output"} &&
        dependency.tensors != std::vector<std::string>{"block-0/block-output-rank-1-join"})
      continue;
    BOOST_CHECK_EQUAL(dependency.operationKind, "ACTIVATION");
    BOOST_CHECK(dependency.useNdnsfDataV1);
    hiddenConsumers.insert(dependency.consumers.at(0));
  }
  BOOST_CHECK_EQUAL(hiddenConsumers.size(), 2);
  BOOST_CHECK(std::any_of(tpPp.plan.dependencies.begin(), tpPp.plan.dependencies.end(),
    [&] (const auto& dependency) {
      return dependency.tensors == std::vector<std::string>{"block-1/block-output"} &&
             dependency.producers == std::vector<std::string>{prefixTp.unitRoleIds[0]} &&
             dependency.consumers == suffix.unitRoleIds;
    }));
  BOOST_CHECK_THROW(connectVideoDenoiser(patch, textProjection, time, {}, head, grid, stage),
                    std::invalid_argument);
  BOOST_CHECK_THROW(connectVideoDenoiser(patch, textProjection, time, blocks, head,
                                         {1, 2, 1, 2}, stage), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(denoiser.tensorParallel.entries.size(), 141);
  BOOST_REQUIRE_EQUAL(denoiser.tensorParallel.plan.steps.size(), 184);
  NativeCanonicalSource image;
  const auto serialized = denoiser.model.SerializeAsString();
  image.modelBytes.assign(serialized.begin(), serialized.end());
  const NativeAssemblyControl preparationControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 32U << 20};
  NativeInspectedModel inspected;
  const auto digest = [] (const char* value) { return nativePlanningDigest(value); };
  inspected.descriptor = ndnsf::di::fixture::completeModel({
    "tiny-wan-denoiser", digest("content"), digest("dit-semantics"), digest("planning"),
    "onnx", "fp32", "wan", "1"});
  inspected.descriptor.adapter.backends = {"onnxruntime-cpu"};
  const auto inspection = inspectNativeOnnxSourceGraph(image, inspected.descriptor,
                                                       preparationControl);
  inspected.graph = inspection.graph;
  inspected.descriptor.graphDigest = inspection.graph.graphDigest;
  inspected.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
  inspected.canonicalInitializerDigest = inspection.canonicalIdentity.initializerDigest;
  inspected.canonicalSourceName = "/fixture/wan/denoiser";
  inspected.canonicalSourceBytes = image.modelBytes.size();
  inspected.canonicalSourceDigest = nativePlanningDigest(image.modelBytes.data(),
                                                         image.modelBytes.size());
  inspected.modelManifestDigest = digest("dit-manifest");
  const auto mapping = makeWanMaterialNodeMap(inspection);
  BOOST_CHECK_EQUAL(mapping.size(), denoiser.model.graph().node_size());
  const NativeRoleRecipeProfile profile{digest("profile"), digest("assembler"), "fixture-ort",
    "fp32", "none", "ncdhw", "right", "fixture-epoch", 4U << 20, 32U << 20, 4096};
  NativeCanonicalRolePreparer preparer(inspected, inspection, profile, preparationControl,
                                        mapping);
  for (const auto* layout : {&denoiser.tensorParallel, &pp, &ppTp, &tpPp}) {
    const auto requestDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const NativeRequestControl requestControl{
      "/fixture/wan-preparation", 1, requestDeadline, {}};
    std::map<std::string, NativeRoleResourceRequirement> resources;
    for (const auto& role : layout->plan.roles)
      resources[role] = {{"onnxruntime-cpu"}, 1U << 20, 1U << 20, 0, 1U << 20,
                         1U << 20, 1.0};
    const auto candidate = makeWanSplitCandidate(inspected, inspection, *layout, "/fixture/di",
                                                 resources, requestControl);
    NativeWanSplitStrategy splitter(candidate);
    const auto candidates = splitter.enumerate(inspected.descriptor, inspected.graph, {},
                                               {requestDeadline, {}});
    BOOST_REQUIRE_EQUAL(candidates.size(), 1);
    BOOST_CHECK_EQUAL(candidates[0].candidateDigest, candidate.candidateDigest);
    BOOST_CHECK_EQUAL(candidate.nodeRoles.size(), mapping.size());
    BOOST_CHECK_EQUAL(candidate.resultEgressRole, layout->plan.roles[layout->plan.roles.size() -
      layout->plan.stageLayout.back().degree()]);
    const auto ready = preparer.prepare(inspected, candidate, requestControl);
    BOOST_CHECK_EQUAL(ready.size(), layout->plan.roles.size());
    for (const auto& recipe : ready)
      BOOST_CHECK_EQUAL(recipe.nodeIndices.size(), candidate.roleNodeIdsByRole.at(recipe.role).size());
    auto bad = *layout;
    bad.entries.begin()->second.inputs.front().name = "foreign-input";
    BOOST_CHECK_THROW(makeWanSplitCandidate(inspected, inspection, bad, "/fixture/di",
                                           resources, requestControl), std::invalid_argument);
    if (layout == &denoiser.tensorParallel) {
      auto rebound = *layout;
      bool reboundInput = false;
      for (auto& step : rebound.plan.steps) {
        const auto input = std::find_if(step.inputSlots.begin(), step.inputSlots.end(),
          [](const auto& slot) { return slot.source == "LOCAL_STEP"; });
        if (input == step.inputSlots.end()) continue;
        input->source = "EPOCH_INPUT";
        input->producerStepId.clear();
        input->producerSlotId.clear();
        reboundInput = true;
        break;
      }
      BOOST_REQUIRE(reboundInput);
      BOOST_CHECK_EXCEPTION(makeWanSplitCandidate(inspected, inspection, rebound, "/fixture/di",
        resources, requestControl), std::invalid_argument, [](const auto& error) {
          return std::string(error.what()) == "Wan step input does not bind its canonical producer";
        });
    }
    const auto config = makeCatalogTemplate(inspected.descriptor, profile,
      inspected.canonicalSourceName, inspected.modelManifestDigest, "/fixture/wan/package",
      "/fixture/di", resources);
    const NativeAssemblyControl catalogControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(15), [] {}, 4U << 20, 32U << 20};
    auto compiled = compileWanComponent(denoiser.model, *layout, 32U << 20, {}, config,
                                        catalogControl);
    auto encodedConfig = std::move(compiled.configurationJson);
    if (layout == &denoiser.tensorParallel) {
      auto wide = nativeParseJson(encodedConfig);
      // This ignored metadata exercises the encrypted catalog boundary while
      // retaining the tiny numerical fixture and its bound physical plan.
      wide["description"] = std::string(4U << 20, 'w');
      encodedConfig = nativeCanonicalJson(wide);
      BOOST_REQUIRE_GT(encodedConfig.size(), 4U << 20);
    }
    BOOST_CHECK(compiled.source.modelBytes == image.modelBytes);
    const auto cold = NativeRequestCatalog::load(encodedConfig, std::move(compiled.source),
                                                 catalogControl);
    const auto& frozen = cold.planningCache->candidates.at(0);
    BOOST_CHECK_EQUAL(frozen.candidate.candidateDigest, candidate.candidateDigest);
    const auto modelOwner = cold.preparation->modelRefFor(cold.model.descriptor);
    const auto cachedPlan = cold.planningCache->validatedTemplate(*modelOwner,
                                                                  cold.cooperativeSplitter.get());
    BOOST_REQUIRE(cachedPlan);
    BOOST_CHECK_NO_THROW(cachedPlan->execution(0).validateSteps());
    BOOST_CHECK(nativeV2StepTemplateJson(cachedPlan->execution(0)) ==
                nativeV2StepTemplateJson(frozen.candidate.executionPlan));
    BOOST_CHECK(nativeDependenciesJson(cachedPlan->execution(0).dependencies) ==
                nativeDependenciesJson(frozen.candidate.executionPlan.dependencies));
    BOOST_REQUIRE_EQUAL(frozen.roles.size(), ready.size());
    for (std::size_t i = 0; i < ready.size(); ++i)
      BOOST_CHECK_EQUAL(frozen.roles[i].recipeDigest, ready[i].recipeDigest);

    if (layout == &denoiser.tensorParallel) {
      ndn::KeyChain keys("pib-memory:", "tpm-memory:");
      std::map<ndn::Name, std::vector<std::uint8_t>> packets;
      std::map<ndn::Name, std::pair<std::string, std::uint64_t>> groups;
      std::size_t materialReads = 0;
      NativeStoredModelPacketIo io;
      io.requireCurrent = [] {};
      io.commit = [&](auto wires, const std::string& groupDigest) {
        BOOST_REQUIRE(!wires.empty());
        const ndn::Data firstPacket{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
        std::uint64_t size = 0;
        for (auto& wire : wires) {
          const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
          size += wire.size();
          BOOST_REQUIRE(packets.emplace(packet.getName(), std::move(wire)).second);
        }
        groups.emplace(firstPacket.getName(), std::make_pair(groupDigest, size));
      };
      io.requireCommitted = [&](const ndn::Name& name, const std::string& hash,
                                 std::uint64_t bytes) {
        BOOST_CHECK_EQUAL(groups.at(name).first, hash);
        BOOST_CHECK_EQUAL(groups.at(name).second, bytes);
      };
      io.fetch = [&](const std::vector<ndn::Name>& names) {
        NativeStoredModelPacketIo::PacketWires result;
        for (const auto& name : names) {
          if (name.toUri().find("material-bundle-") != std::string::npos) ++materialReads;
          result.push_back(packets.at(name));
        }
        return result;
      };
      const ndn::Name producer("/fixture/wan");
      auto bounded = nativeParseJson(encodedConfig);
      bounded.at("recipe")["max_assembled_bytes"] = 4U << 20;
      const auto boundedConfig = nativeCanonicalJson(bounded);
      const auto boundedCatalog = NativeRequestCatalog::load(boundedConfig, image, catalogControl);
      BOOST_CHECK_EXCEPTION(
        nativePublishStoredPreparedModel(keys, producer, ndn::Name("/fixture/repo"),
          ndn::Name(producer).append("bounded").appendVersion(1), boundedCatalog, boundedConfig, io),
        std::invalid_argument, [] (const auto& error) {
          return std::string(error.what()) == "DI_STORED_PREPARATION_MATERIAL_METADATA_BUDGET";
        });
      BOOST_CHECK(packets.empty());
      const auto publication = nativePublishStoredPreparedModel(keys, producer,
        ndn::Name("/fixture/repo"), ndn::Name(producer).append("package").appendVersion(1),
        cold, encodedConfig, io);
      BOOST_CHECK_LE(publication.objectCount, 10);
      auto readControl = catalogControl;
      readControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      BOOST_CHECK_THROW(nativeOpenStoredPreparedModel(publication.handle, io, readControl,
                                                      nativeStoredCatalogReadLimit(0)),
                        std::invalid_argument);
      const auto reopened = nativeOpenStoredPreparedModel(publication.handle, io, readControl,
                                  nativeStoredCatalogReadLimit(encodedConfig.size()));
      BOOST_CHECK(reopened.configurationJson ==
                  nativeStoredModelConfigurationIdentity(encodedConfig));
      BOOST_CHECK_EQUAL(reopened.catalog.planningCache->candidates.at(0).candidate.candidateDigest,
                        frozen.candidate.candidateDigest);
      const auto reopenedModel = reopened.catalog.preparation->modelRefFor(
        reopened.catalog.model.descriptor);
      const auto reopenedPlan = reopened.catalog.planningCache->validatedTemplate(
        *reopenedModel, reopened.catalog.cooperativeSplitter.get());
      BOOST_REQUIRE(reopenedPlan);
      BOOST_CHECK(nativeV2StepTemplateJson(reopenedPlan->execution(0)) ==
                  nativeV2StepTemplateJson(frozen.candidate.executionPlan));
      const auto source = reopened.catalog.preparation->sourceRefFor(inspected.descriptor);
      BOOST_CHECK(source->modelBytes.empty());
      BOOST_CHECK_EQUAL(materialReads, 0);
      const auto keyIndex = nativeBuildStoredModelObjectKeyIndex(
        reopened.objects.publication(), reopened.objects, reopened.publication);
      BOOST_REQUIRE_EQUAL(frozen.roles.size(), 2);
      const auto firstKeys = nativeSelectModelObjectKeyDataNames(*keyIndex,
                                                                 frozen.roles[0].nodeIndices);
      const auto secondKeys = nativeSelectModelObjectKeyDataNames(*keyIndex,
                                                                  frozen.roles[1].nodeIndices);
      BOOST_CHECK(std::any_of(firstKeys.begin(), firstKeys.end(),
                              [&](const auto& name) { return secondKeys.count(name) == 0; }));
      BOOST_CHECK(std::any_of(secondKeys.begin(), secondKeys.end(),
                              [&](const auto& name) { return firstKeys.count(name) == 0; }));
      BOOST_REQUIRE_GT(frozen.roles[0].nodeIndices.size(), 1);
      BOOST_CHECK_EXCEPTION(
        nativeSelectModelObjectKeyDataNames(*keyIndex, {frozen.roles[0].nodeIndices.front()}),
        std::invalid_argument, [] (const auto& error) {
          return std::string(error.what()) == "DI_MODEL_OBJECT_BUNDLE_CROSSES_PROVIDER_ASSIGNMENT";
        });

      auto legacy = nativeParseJson(encodedConfig);
      legacy.at("publication").erase("material_bundle_policy");
      const auto legacyConfig = nativeCanonicalJson(legacy);
      auto legacySource = image;
      auto legacyControl = catalogControl;
      legacyControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      const auto legacyCatalog = NativeRequestCatalog::load(legacyConfig, std::move(legacySource),
                                                             legacyControl);
      const auto legacyPublication = nativePublishStoredPreparedModel(keys, producer,
        ndn::Name("/fixture/repo"), ndn::Name(producer).append("package").appendVersion(2),
        legacyCatalog, legacyConfig, io);
      BOOST_CHECK_GT(legacyPublication.objectCount, 10 * publication.objectCount);
      const auto legacyReopened = nativeOpenStoredPreparedModel(legacyPublication.handle, io,
                                                               legacyControl);
      BOOST_CHECK_EQUAL(legacyReopened.catalog.planningCache->candidates.at(0).candidate.candidateDigest,
                        frozen.candidate.candidateDigest);
      BOOST_CHECK_EQUAL(materialReads, 0);
    }
  }
  // Each materialization below is an independent assembly with its own owner fence.
  const auto denoiserControl = [] {
    return NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
  };
  image.materialManifest = deriveNativeCanonicalMaterialManifest(image, denoiserControl());
  BOOST_REQUIRE(image.materialManifest);
  const auto denoiserJson = image.materialManifest->canonicalJson();
  const auto denoiserIndex = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(denoiserJson.begin(), denoiserJson.end()));
  for (const auto& step : denoiser.tensorParallel.plan.steps) {
    if (step.operation == "SUM") continue;
    const auto& cut = denoiser.tensorParallel.entries.at(step.artifactEntry);
    const auto other = step.unitId == roles[0] ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*image.materialManifest, denoiserIndex, cut,
      [&] (const auto& initializer) { return initializer.find(other) == std::string::npos; });
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(
      selected, cut.nodeIndices, cut.inputs, cut.outputs, denoiserControl()));
  }
  for (const auto* hybrid : {&ppTp, &tpPp}) {
    for (const auto& step : hybrid->plan.steps) {
      if (hybrid->plan.degreeForRole(step.unitId) != 1) continue;
      const auto& cut = hybrid->entries.at(step.artifactEntry);
      auto selected = selectCanonicalEntry(*image.materialManifest, denoiserIndex, cut,
                                            [] (const auto&) { return true; });
      BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(
        selected, cut.nodeIndices, cut.inputs, cut.outputs, denoiserControl()));
    }
  }
  image.materialPayloads = image.materialManifest->payloads;
  std::vector<std::uint64_t> all(denoiser.model.graph().node_size());
  std::iota(all.begin(), all.end(), 0);
  const auto denoiserBytes = materializeNativeCanonicalModel(image, all, denoiserControl());
  Ort::Session denoiserSession(env, denoiserBytes.data(), denoiserBytes.size(), options);
  std::array<float, 16> latent{3, -2, -2, 2, 0, 0, 0, 0, 0.5, 0, 5, 0, 0, 0, 0, 0};
  std::array<float, 6> trimmed{3, -1, 2, 1, 4, -1};
  std::array<std::int64_t, 1> timestep{17};
  const std::array<const char*, 4> denoiserInputs{"latent", "context", "timestep", "video-valid"};
  const std::array<Ort::Value, 4> arguments{
    tensor(latent, {1, 2, 1, 2, 4}), tensor(trimmed, {1, 2, 3}),
    tensor(timestep, {1}), tensor(mask, {1, 1, 1, 2})};
  const char* prediction = "prediction";
  const auto predicted = denoiserSession.Run(Ort::RunOptions{}, denoiserInputs.data(),
                                              arguments.data(), arguments.size(), &prediction, 1);
  BOOST_REQUIRE(predicted.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 2, 1, 2, 4}));
  std::map<std::string, Ort::Value> ppValues;
  ppValues.emplace("latent", tensor(latent, {1, 2, 1, 2, 4}));
  ppValues.emplace("context", tensor(trimmed, {1, 2, 3}));
  ppValues.emplace("timestep", tensor(timestep, {1}));
  ppValues.emplace("video-valid", tensor(mask, {1, 1, 1, 2}));
  const NativeAssemblyControl ppControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
  runCompiledEntriesFixture(pp, image, ppValues, env, options, memory, ppControl);
  const auto* ppPrediction = ppValues.at("prediction").GetTensorData<float>();
  for (int pixel = 0; pixel < 16; ++pixel)
    BOOST_CHECK_SMALL(double(ppPrediction[pixel]) - predicted.front().GetTensorData<float>()[pixel],
                      1e-6);
  const auto geluTanh = [] (double x) {
    return 0.5 * x *
      (1 + std::tanh(std::sqrt(2 / std::acos(-1.0)) * (x + 0.044715 * x * x * x)));
  };
  std::array<double, 3> mean{};
  for (int channel = 0; channel < 3; ++channel)
    mean[channel] = (geluTanh(trimmed[channel]) + geluTanh(trimmed[3 + channel])) / 3;
  std::array<std::array<double, 3>, 2> reference{{{3, -2, 0.5}, {-2, 2, 5}}};
  for (const double gate : {0.25, 0.5, 0.75}) {
    const auto norm0 = normalize(reference[0].data());
    for (int token = 0; token < 2; ++token) {
      for (int c = 0; c < 3; ++c)
        reference[token][c] += gate * norm0[c] + mean[c];
      const auto beforeFfn = normalize(reference[token].data());
      for (int c = 0; c < 3; ++c) reference[token][c] += geluTanh(beforeFfn[c]);
    }
  }
  const auto* actual = predicted.front().GetTensorData<float>();
  for (int token = 0; token < 2; ++token) {
    const auto normalized = normalize(reference[token].data());
    for (int col = 0; col < 3; ++col) {
      const auto timeValue = timeDownBias.float_data(col);
      const int coordinate = (col / 2) * 4 + 2 * token + col % 2;
      BOOST_CHECK_SMALL(double(actual[coordinate]) -
                        (normalized[col] * (1 + timeValue) + timeValue), 5e-6);
    }
    BOOST_CHECK_EQUAL(actual[5 + 2 * token], 0.375F);
  }
  for (int pixel = 8; pixel < 16; ++pixel) BOOST_CHECK_EQUAL(actual[pixel], 0.25F);

}

BOOST_AUTO_TEST_CASE(VideoHeadProjectsDisjointChannelsAndRestoresVideoCoordinates)
{
  constexpr std::int64_t frames = 2, height = 2, width = 3, padded = 14;
  constexpr std::int64_t hidden = 3, channels = 2, patchWidth = 4 * channels;
  constexpr float epsilon = 1e-6F;
  const auto parameter = [] (std::initializer_list<int> dimensions, int seed) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto dim : dimensions) {
      tensor.add_dims(dim);
      count *= dim;
    }
    for (int i = 0; i < count; ++i) tensor.add_float_data(((i * 3 + seed) % 17 - 8) / 8.0F);
    return tensor;
  };
  const auto modulation = parameter({1, 2, hidden}, 2);
  const auto weight = parameter({hidden, patchWidth}, 4);
  const auto bias = parameter({patchWidth}, 5);
  const VideoHeadGrid grid{frames, height, width, padded};
  const auto layout = lowerVideoHead(grid, modulation, weight, bias, epsilon);
  BOOST_CHECK_THROW(lowerVideoHead({frames, height, width, 11}, modulation, weight, bias, epsilon),
                    std::invalid_argument);
  BOOST_CHECK_THROW(lowerVideoHead(grid, modulation, weight, bias, 0.0F), std::invalid_argument);

  std::array<float, padded * hidden> input{};
  constexpr std::array<float, hidden> timeValues{1.0F, -0.75F, 0.25F};
  auto time = timeValues;
  for (std::size_t token = 1; token < padded; ++token) {
    input[token * hidden] = static_cast<float>(token) / 3.0F;
    input[token * hidden + 1] = -static_cast<float>(token % 5);
    input[token * hidden + 2] = static_cast<float>(token % 3) / 2.0F;
  }
  // The two padding rows have values, so dropping them cannot be explained by zero padding.
  std::array<std::array<double, padded * patchWidth>, 2> expectedPartial{};
  for (std::size_t token = 0; token < padded; ++token) {
    const auto* values = input.data() + token * hidden;
    double mean = 0;
    for (std::size_t col = 0; col < hidden; ++col) mean += values[col];
    mean /= hidden;
    double variance = 0;
    for (std::size_t col = 0; col < hidden; ++col) {
      const auto delta = values[col] - mean;
      variance += delta * delta;
    }
    const auto divisor = std::sqrt(variance / hidden + epsilon);
    for (std::size_t col = 0; col < hidden; ++col) {
      const double shift = time[col] + modulation.float_data(col);
      const double gain = 1 + time[col] + modulation.float_data(hidden + col);
      const double activation = ((values[col] - mean) / divisor) * gain + shift;
      const auto rank = col < 2 ? 0 : 1;
      for (std::size_t column = 0; column < patchWidth; ++column) {
        expectedPartial[rank][token * patchWidth + column] +=
          activation * weight.float_data(col * patchWidth + column);
      }
    }
  }

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-video-head");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  constexpr std::array<std::int64_t, 3> inputShape{1, padded, hidden};
  constexpr std::array<std::int64_t, 2> timeShape{1, hidden};
  const std::array<Ort::Value, 2> values{
    Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                    inputShape.data(), inputShape.size()),
    Ort::Value::CreateTensor<float>(memory, time.data(), time.size(),
                                    timeShape.data(), timeShape.size())};
  const std::array<const char*, 2> names{"hidden", "embedding"};
  const auto wire = layout.model.SerializeAsString();
  Ort::Session full(env, wire.data(), wire.size(), options);
  const char* finalName = "latent";
  const auto fullOutput = full.Run(Ort::RunOptions{}, names.data(), values.data(), values.size(),
                                   &finalName, 1);

  NativeCanonicalSource canonical;
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto manifestJson = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(manifestJson.begin(), manifestJson.end()));
  const auto assemble = [&] (const OnnxEntryCut& cut, auto allowed) {
    auto selected = selectCanonicalEntry(*manifest, index, cut, allowed);
    return materializeNativeCanonicalModel(selected, cut.nodeIndices, cut.inputs,
                                           cut.outputs, control);
  };
  constexpr std::array<std::int64_t, 3> reducedShape{1, padded, patchWidth};
  std::array<std::array<float, padded * patchWidth>, 2> partials{};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto prefix = "head/rank-" + std::to_string(rank) + "/";
    const auto& cut = layout.partials[rank];
    const auto bytes = assemble(cut, [&] (const std::string& name) {
      return name.rfind(prefix, 0) == 0 || name == "axis-1" || name == "axis-2" ||
        name == "shift-index" || name == "scale-index" || name == "one" || name == "norm-scale";
    });
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    const char* partialName = cut.outputs.front().name.c_str();
    const auto result = runner.Run(Ort::RunOptions{}, names.data(), values.data(), values.size(),
                                     &partialName, 1);
    BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>(reducedShape.begin(), reducedShape.end())));
    const auto* actual = result.front().GetTensorData<float>();
    for (std::size_t cell = 0; cell < partials[rank].size(); ++cell) {
      BOOST_CHECK_SMALL(actual[cell] - expectedPartial[rank][cell], 2e-6);
      partials[rank][cell] = actual[cell];
    }
  }
  const auto joinBytes = assemble(layout.addBias, [] (const std::string& name) {
    return name == "axis-1" || name == "head/bias" || name == "head/first-token" ||
      name == "head/token-count" || name == "head/patch-shape" || name == "head/latent-shape";
  });
  Ort::Session join(env, joinBytes.data(), joinBytes.size(), options);
  const auto& sumPort = layout.addBias.inputs.front().name;
  std::vector<std::vector<NamedTensorView>> operands;
  for (const auto& partial : partials) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(partial.data());
    operands.push_back({{sumPort, TensorElementType::Float32,
                         {reducedShape.begin(), reducedShape.end()},
                         {raw, partial.size() * sizeof(float)}}});
  }
  const auto sum = sumTensorBundleViews(sumPort, operands);
  const auto sumViews = decodeTensorBundleViews(sum);
  BOOST_REQUIRE_EQUAL(sumViews.size(), 1);
  std::array<float, padded * patchWidth> reduced{};
  BOOST_REQUIRE_EQUAL(sumViews.front().payload.size(), reduced.size() * sizeof(float));
  std::memcpy(reduced.data(), sumViews.front().payload.data(), sumViews.front().payload.size());
  const auto inputSum = Ort::Value::CreateTensor<float>(memory, reduced.data(), reduced.size(),
                                                        reducedShape.data(), reducedShape.size());
  const auto* sumName = sumPort.c_str();
  const auto joinedOutput = join.Run(Ort::RunOptions{}, &sumName, &inputSum, 1, &finalName, 1);

  const std::vector<std::int64_t> outShape{1, channels, frames, 2 * height, 2 * width};
  for (const auto* output : {&fullOutput.front(), &joinedOutput.front()}) {
    BOOST_REQUIRE(output->GetTensorTypeAndShapeInfo().GetShape() == outShape);
    const auto* data = output->GetTensorData<float>();
    for (int c = 0; c < channels; ++c) {
      for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < height * 2; ++y) {
          for (int x = 0; x < width * 2; ++x) {
            const auto token = (f * height + y / 2) * width + x / 2;
            const auto coord = ((y % 2) * 2 + x % 2) * channels + c;
            const auto cell = token * patchWidth + coord;
            const auto expected = expectedPartial[0][cell] + expectedPartial[1][cell] +
              bias.float_data(coord);
            const auto destination = (((c * frames + f) * height * 2 + y) * width * 2) + x;
            BOOST_CHECK_SMALL(data[destination] - expected, 2e-6);
          }
        }
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(Umt5EmbeddingStreamsColumnRangesAndRejectsInvalidIds)
{
  class Table final : public NativeCanonicalByteRangeSource
  {
  public:
    std::uint64_t total = 0;
    std::vector<std::uint8_t> bytes;
    mutable std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::uint64_t size() const noexcept override { return total; }
    std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
    {
      reads.emplace_back(offset, length);
      if (offset > bytes.size() || length > bytes.size() - offset)
        throw std::out_of_range("fixture read exceeds resident bytes");
      return {bytes.begin() + offset, bytes.begin() + offset + length};
    }
  };
  constexpr std::uint64_t base = 17;
  const std::array<float, 15> weights{
    -5, -4, -3, -2, -1, 1, 2, 3, 4, 5, 11, 12, 13, 14, 15};
  auto table = std::make_shared<Table>();
  table->bytes.resize(base + sizeof(weights));
  std::memcpy(table->bytes.data() + base, weights.data(), sizeof(weights));
  table->total = table->bytes.size();
  const auto layout = lowerUmt5Embedding(2, 3, 5, table, base);
  BOOST_CHECK(table->reads.empty());
  const auto& source = layout.source;
  constexpr std::uint64_t split = 3 * 3 * sizeof(float);
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> expectedReads{
    {base + 48, 4}, {base + 12, 4}};
  auto window = source.initializerRangeSource->read(split - 4, 8);
  BOOST_CHECK(table->reads == expectedReads);
  std::array<float, 2> crossing{};
  std::memcpy(crossing.data(), window.data(), window.size());
  BOOST_CHECK_EQUAL(crossing[0], 13);
  BOOST_CHECK_EQUAL(crossing[1], -2);
  BOOST_CHECK_THROW(source.initializerRangeSource->read(sizeof(weights) - 1, 2),
                    std::out_of_range);
  BOOST_CHECK_THROW(lowerUmt5Embedding(2, 3, 5, table, base + 1), std::invalid_argument);

  auto metadataOnly = std::make_shared<Table>();
  metadataOnly->total = 256384ULL * 4096 * sizeof(float);
  const auto production = lowerUmt5Embedding(512, 256384, 4096, metadataOnly);
  BOOST_CHECK(metadataOnly->reads.empty());
  BOOST_CHECK_EQUAL(production.source.initializerRangeSource->size(), 4200595456ULL);
  BOOST_CHECK_LT(production.source.modelBytes.size(), 4096);
  BOOST_CHECK_EQUAL(std::get<std::int64_t>(production.ranks[1].outputs.front().shape.back()), 2048);

  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-umt5-embedding");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<std::int64_t, 2> ids{2, 0};
  constexpr std::array<std::int64_t, 2> shape{1, 2};
  const auto tensor = Ort::Value::CreateTensor<std::int64_t>(
    memory, ids.data(), ids.size(), shape.data(), shape.size());
  const char* inputName = "token-ids";
  std::array<Ort::Value, 2> local{Ort::Value{nullptr}, Ort::Value{nullptr}};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& entry = layout.ranks[rank];
    const auto ownedTable = "rank-" + std::to_string(rank) + "/table";
    auto selection = selectCanonicalEntry(*manifest, index, entry, [&] (const auto& name) {
      return name == ownedTable || name == "zero" || name == "vocab";
    });
    const auto bytes = materializeNativeCanonicalModel(selection, entry.nodeIndices, entry.inputs,
                                                        entry.outputs, control);
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    auto* name = entry.outputs.front().name.c_str();
    auto values = runner.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &name, 1);
    local[rank] = std::move(values.front());
    const auto width = rank == 0 ? 3U : 2U;
    const auto first = rank == 0 ? 0U : 3U;
    BOOST_REQUIRE_EQUAL(local[rank].GetTensorTypeAndShapeInfo().GetElementCount(), width * 2);
    for (std::size_t token = 0; token < ids.size(); ++token) {
      for (std::size_t col = 0; col < width; ++col) {
        BOOST_CHECK_EQUAL(local[rank].GetTensorData<float>()[token * width + col],
                          weights[ids[token] * 5 + first + col]);
      }
    }
    for (const auto invalid : {-1, 3}) {
      ids[0] = invalid;
      BOOST_CHECK_THROW(runner.Run(Ort::RunOptions{}, &inputName, &tensor, 1, &name, 1),
                        Ort::Exception);
    }
    ids[0] = 2;
  }
  const auto& join = layout.join;
  auto selection = selectCanonicalEntry(*manifest, index, join, [] (const auto&) { return false; });
  const auto joined = materializeNativeCanonicalModel(selection, join.nodeIndices,
                                                       join.inputs, join.outputs, control);
  Ort::Session runner(env, joined.data(), joined.size(), options);
  const std::array<const char*, 2> names{join.inputs[0].name.c_str(), join.inputs[1].name.c_str()};
  const char* output = "token-embedding";
  const auto result = runner.Run(Ort::RunOptions{}, names.data(), local.data(), local.size(),
                                   &output, 1);
  BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), 10);
  for (std::size_t token = 0; token < ids.size(); ++token) {
    for (std::size_t col = 0; col < 5; ++col) {
      BOOST_CHECK_EQUAL(result.front().GetTensorData<float>()[token * 5 + col],
                        weights[ids[token] * 5 + col]);
    }
  }
}

BOOST_AUTO_TEST_CASE(Umt5GatedFeedForwardMatchesRankAndFullReference)
{
  const auto matrix = [] (int rows, int cols, int seed) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(rows);
    tensor.add_dims(cols);
    for (int i = 0; i < rows * cols; ++i) tensor.add_float_data(((i + seed) % 9 - 4) / 4.0F);
    return tensor;
  };
  const auto gate = matrix(2, 3, 3);
  const auto expansion = matrix(2, 3, 5);
  const auto contraction = matrix(3, 2, 9);
  const auto layout = lowerUmt5FeedForward(2, gate, expansion, contraction);
  BOOST_CHECK_THROW(lowerUmt5FeedForward(2, gate, expansion, gate), std::invalid_argument);
  const auto wire = layout.model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-umt5-ffn");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 4> inputs{-1, 0.5F, 0.375F, 1.25F};
  constexpr std::array<std::int64_t, 3> dims{1, 2, 2};
  const auto x = Ort::Value::CreateTensor<float>(memory, inputs.data(), inputs.size(),
                                                  dims.data(), dims.size());
  const char* inputName = "hidden";
  std::array<std::array<double, 4>, 2> expected{};
  for (int token = 0; token < 2; ++token) {
    for (int channel = 0; channel < 3; ++channel) {
      double g = 0, u = 0;
      for (int col = 0; col < 2; ++col) {
        g += inputs[token * 2 + col] * gate.float_data(col * 3 + channel);
        u += inputs[token * 2 + col] * expansion.float_data(col * 3 + channel);
      }
      const auto activated = 0.5 * g * (1 + std::tanh(std::sqrt(2 / std::acos(-1.0)) *
                                                      (g + 0.044715 * g * g * g)));
      for (int col = 0; col < 2; ++col) {
        expected[channel < 2 ? 0 : 1][token * 2 + col] +=
          u * activated * contraction.float_data(channel * 2 + col);
      }
    }
  }
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto& entry = layout.partials[rank];
    const auto prefix = "rank-" + std::to_string(rank) + "/";
    auto selected = selectCanonicalEntry(*manifest, index, entry, [&] (const auto& name) {
      return name.rfind(prefix, 0) == 0;
    });
    const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                                        entry.outputs, control);
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    const auto* output = entry.outputs.front().name.c_str();
    const auto result = runner.Run(Ort::RunOptions{}, &inputName, &x, 1, &output, 1);
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[i] - expected[rank][i], 1e-6);
    }
  }
  Ort::Session full(env, wire.data(), wire.size(), options);
  const auto* output = layout.output.name.c_str();
  const auto result = full.Run(Ort::RunOptions{}, &inputName, &x, 1, &output, 1);
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[i] -
                      expected[0][i] - expected[1][i], 1e-6);
  }
}

BOOST_AUTO_TEST_CASE(Umt5AttentionUsesUnscaledDotsAndPerHeadRelativeBias)
{
  constexpr int tokens = 3, hidden = 2, heads = 3, width = 2, buckets = 8;
  const auto matrix = [] (int rows, int cols, int seed) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(rows);
    tensor.add_dims(cols);
    for (int i = 0; i < rows * cols; ++i)
      tensor.add_float_data(((i * seed + seed) % 17 - 8) / 8.0F);
    return tensor;
  };
  const auto q = matrix(hidden, heads * width, 3);
  const auto k = matrix(hidden, heads * width, 5);
  const auto v = matrix(hidden, heads * width, 7);
  const auto o = matrix(heads * width, hidden, 11);
  const auto relative = matrix(buckets, heads, 13);
  const Umt5AttentionParameters parameters{q, k, v, o, relative};
  const auto layout = lowerUmt5Attention(tokens, parameters, 72);
  BOOST_CHECK_THROW(lowerUmt5Attention(tokens, parameters, 71), std::invalid_argument);
  BOOST_CHECK_THROW(lowerUmt5Attention(513, parameters, 1U << 20), std::invalid_argument);

  std::array<float, 6> state{-1.75F, 0.125F, 2.0F, -2.5F, 0.75F, 1.5F};
  std::array<bool, 3> mask{true, false, true};
  // Distances 0 and 1 have exact buckets. Distance 2 is the beginning of the log range.
  const std::array<int, 9> bucket{0, 5, 6, 1, 0, 5, 2, 1, 0};
  const auto project = [&] (const auto& weight, int token, int channel) {
    double value = 0;
    for (int col = 0; col < hidden; ++col) {
      value += state[token * hidden + col] * weight.float_data(col * heads * width + channel);
    }
    return value;
  };
  std::array<std::array<double, 6>, 2> expected{};
  for (int token = 0; token < tokens; ++token) {
    for (int head = 0; head < heads; ++head) {
      const auto rank = head < 2 ? 0 : 1;
      std::array<double, tokens> probability{};
      double total = 0;
      for (int key = 0; key < tokens; ++key) {
        if (!mask[key]) continue;
        double logit = relative.float_data(bucket[token * tokens + key] * heads + head);
        for (int dim = 0; dim < width; ++dim) {
          logit += project(q, token, head * width + dim) * project(k, key, head * width + dim);
        }
        probability[key] = std::exp(logit);
        total += probability[key];
      }
      for (int dim = 0; dim < width; ++dim) {
        const auto feature = head * width + dim;
        double value = 0;
        for (int key = 0; key < tokens; ++key)
          value += probability[key] * project(v, key, feature) / total;
        for (int col = 0; col < hidden; ++col) {
          expected[rank][token * hidden + col] += value * o.float_data(feature * hidden + col);
        }
      }
    }
  }

  const auto wire = layout.model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-umt5-attention");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  constexpr std::array<std::int64_t, 3> stateShape{1, tokens, hidden};
  constexpr std::array<std::int64_t, 2> maskShape{1, tokens};
  const std::array<Ort::Value, 2> values{
    Ort::Value::CreateTensor<float>(memory, state.data(), state.size(),
                                    stateShape.data(), stateShape.size()),
    Ort::Value::CreateTensor<bool>(memory, mask.data(), mask.size(),
                                   maskShape.data(), maskShape.size())};
  const std::array<const char*, 2> inputs{"hidden", "key-mask"};
  for (int rank = 0; rank < 2; ++rank) {
    const auto& entry = layout.partials[rank];
    const auto prefix = "rank-" + std::to_string(rank) + "/";
    auto selected = selectCanonicalEntry(*manifest, index, entry, [&] (const auto& name) {
      return name.rfind(prefix, 0) == 0 || name == "relative-buckets" ||
        name == "masked-bias" || name == "mask-axes";
    });
    const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                                        entry.outputs, control);
    Ort::Session runner(env, bytes.data(), bytes.size(), options);
    const auto* output = entry.outputs.front().name.c_str();
    const auto result = runner.Run(Ort::RunOptions{}, inputs.data(), values.data(),
                                     values.size(), &output, 1);
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), state.size());
    for (std::size_t i = 0; i < state.size(); ++i) {
      BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[i] - expected[rank][i], 1e-6);
    }
  }
  Ort::Session full(env, wire.data(), wire.size(), options);
  const auto* output = layout.output.name.c_str();
  const auto result = full.Run(Ort::RunOptions{}, inputs.data(), values.data(),
                                values.size(), &output, 1);
  for (std::size_t i = 0; i < state.size(); ++i) {
    BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[i] -
                      expected[0][i] - expected[1][i], 1e-6);
  }
}

BOOST_AUTO_TEST_CASE(Umt5RmsSharesOnlyStatisticsAndRestoresChannelOrder)
{
  onnx::TensorProto gamma;
  gamma.set_data_type(onnx::TensorProto::FLOAT);
  gamma.add_dims(3);
  for (const auto gain : {1.5F, -0.5F, 2.25F}) gamma.add_float_data(gain);
  constexpr float epsilon = 0.125F;
  const auto layout = lowerUmt5Rms(2, gamma, epsilon);
  BOOST_CHECK_THROW(lowerUmt5Rms(2, gamma, 0), std::invalid_argument);

  NativeCanonicalSource canonical;
  const auto wire = layout.model.SerializeAsString();
  canonical.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(canonical, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-umt5-rms");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 6> hidden{-1, 2, 4, 0, 0, 0};
  constexpr std::array<std::int64_t, 3> hiddenDims{1, 2, 3};
  const auto input = Ort::Value::CreateTensor<float>(memory, hidden.data(), hidden.size(),
                                                     hiddenDims.data(), hiddenDims.size());
  const auto execute = [&] (const OnnxEntryCut& step, auto allowed,
                             const Ort::Value* args, std::size_t count) {
    auto selected = selectCanonicalEntry(*manifest, index, step, allowed);
    const auto model = materializeNativeCanonicalModel(selected, step.nodeIndices, step.inputs,
                                                        step.outputs, control);
    Ort::Session runner(env, model.data(), model.size(), options);
    std::vector<const char*> in, out;
    for (const auto& p : step.inputs) in.push_back(p.name.c_str());
    for (const auto& p : step.outputs) out.push_back(p.name.c_str());
    BOOST_REQUIRE_EQUAL(in.size(), count);
    return runner.Run(Ort::RunOptions{}, in.data(), args, count, out.data(), out.size());
  };
  std::array<Ort::Value, 2> shards{Ort::Value{nullptr}, Ort::Value{nullptr}};
  std::array<std::array<float, 2>, 2> localSquares{};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto prefix = "rank-" + std::to_string(rank) + "/";
    auto values = execute(layout.statistics[rank], [&] (const auto& name) {
      return name == "axis" || name == prefix + "start" || name == prefix + "end";
    }, &input, 1);
    shards[rank] = std::move(values.at(0));
    const auto* local = values.at(1).GetTensorData<float>();
    std::copy_n(local, 2, localSquares[rank].begin());
    BOOST_CHECK_EQUAL(localSquares[rank][0], rank == 0 ? 5 : 16);
    BOOST_CHECK_EQUAL(localSquares[rank][1], 0);
  }
  const auto& sumPort = layout.normalize[0].inputs[1];
  std::vector<std::vector<NamedTensorView>> operands;
  for (const auto& partial : localSquares) {
    operands.push_back({{sumPort.name, TensorElementType::Float32, {1, 2, 1},
      {reinterpret_cast<const std::uint8_t*>(partial.data()), sizeof(partial)}}});
  }
  const auto sum = sumTensorBundleViews(sumPort.name, operands);
  const auto views = decodeTensorBundleViews(sum);
  BOOST_REQUIRE_EQUAL(views.size(), 1);
  std::array<float, 2> merged{};
  BOOST_REQUIRE_EQUAL(views.front().payload.size(), sizeof(merged));
  std::memcpy(merged.data(), views.front().payload.data(), sizeof(merged));
  constexpr std::array<std::int64_t, 3> sumShape{1, 2, 1};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto owner = "rank-" + std::to_string(rank) + "/gain";
    const std::array<Ort::Value, 2> args{
      std::move(shards[rank]),
      Ort::Value::CreateTensor<float>(memory, merged.data(), merged.size(),
                                      sumShape.data(), sumShape.size())};
    auto result = execute(layout.normalize[rank], [&] (const auto& name) {
      return name == owner || name == "width" || name == "epsilon";
    }, args.data(), args.size());
    shards[rank] = std::move(result.front());
  }
  const auto result = execute(layout.join, [] (const auto&) { return false; },
                               shards.data(), shards.size());
  BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), 6);
  const auto* data = result.front().GetTensorData<float>();
  for (int i = 0; i < 3; ++i) {
    const auto expected = double(hidden[i]) * gamma.float_data(i) / std::sqrt(7.0 + epsilon);
    BOOST_CHECK_SMALL(data[i] - expected, 1e-6);
    BOOST_CHECK_EQUAL(data[i + 3], 0);
  }
}

BOOST_AUTO_TEST_CASE(Wan21Umt5CheckpointBindsTwoLayersAndRunsSplitEntries)
{
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-umt5-checkpoint");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  const std::filesystem::path directory(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&directory) {
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);
  };
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(90), [] {}, 16U << 20, 16U << 20};

  NativeJson header = NativeJson::object();
  std::vector<float> data;
  const auto add = [&](const std::string& name, std::vector<int> shape,
                       std::initializer_list<float> values) {
    const auto offset = data.size() * sizeof(float);
    data.insert(data.end(), values);
    header[name] = {{"dtype", "F32"}, {"shape", std::move(shape)},
                    {"data_offsets", {offset, data.size() * sizeof(float)}}};
  };
  using Pair = std::array<double, 2>;
  const std::array<Pair, 3> embedding{{{1, -1}, {2, 3}, {100, -50}}};
  const std::array<Pair, 2> query{{{0.5, 1}, {-0.25, 0.5}}};
  const std::array<Pair, 2> key{{{-1, 0.25}, {0.5, 1}}};
  const std::array<Pair, 2> value{{{0.75, -0.5}, {0.25, 1}}};
  const std::array<Pair, 2> output{{{1, 0.25}, {-0.5, 0.75}}};
  const std::array<Pair, 4> bias{{{0, 0.125}, {0.25, -0.25}, {0, 0}, {-0.5, 0.5}}};
  const std::array<Pair, 3> gate{{{1, 0.5}, {-0.25, 1}, {0.5, -1}}};
  const std::array<Pair, 3> expand{{{0.75, 1}, {1, -0.5}, {-0.5, 0.25}}};
  const std::array<std::array<double, 3>, 2> contract{{{0.5, -0.25, 0.125}, {0.5, 1, -0.5}}};
  const std::array<Pair, 2> gains{{{1, 2}, {0.5, 1.5}}};
  add("token_embedding.weight", {3, 2}, {1, -1, 2, 3, 100, -50});
  add("norm.weight", {2}, {0.75, 1.25});
  for (std::size_t layer = 0; layer < gains.size(); ++layer) {
    const auto prefix = "blocks." + std::to_string(layer) + ".";
    add(prefix + "norm1.weight", {2},
        {static_cast<float>(gains[layer][0]), static_cast<float>(gains[layer][1])});
    add(prefix + "norm2.weight", {2}, {1.25, 0.75});
    add(prefix + "attn.q.weight", {2, 2}, {0.5, 1, -0.25, 0.5});
    add(prefix + "attn.k.weight", {2, 2}, {-1, 0.25, 0.5, 1});
    add(prefix + "attn.v.weight", {2, 2}, {0.75, -0.5, 0.25, 1});
    add(prefix + "attn.o.weight", {2, 2}, {1, 0.25, -0.5, 0.75});
    add(prefix + "pos_embedding.embedding.weight", {4, 2},
        {0, 0.125, 0.25, -0.25, 0, 0, -0.5, 0.5});
    add(prefix + "ffn.gate.0.weight", {3, 2}, {1, 0.5, -0.25, 1, 0.5, -1});
    add(prefix + "ffn.fc1.weight", {3, 2}, {0.75, 1, 1, -0.5, -0.5, 0.25});
    add(prefix + "ffn.fc2.weight", {2, 3}, {0.5, -0.25, 0.125, 0.5, 1, -0.5});
  }

  const auto archive = [&](const std::string& name, const NativeJson& index) {
    const auto owner = writeFixtureSafeTensors(directory / name, nativeCanonicalJson(index),
                                               data, control);
    return WanCheckpoint({{name, owner}});
  };
  auto bad = header;
  bad["blocks.2.norm1.weight"] = bad.at("blocks.1.norm1.weight");
  bad.erase("blocks.1.norm1.weight");
  BOOST_CHECK_THROW(lowerWan21Umt5Encoder(archive("missing.safetensors", bad), 3, 36),
                    std::invalid_argument);
  const auto layout = lowerWan21Umt5Encoder(archive("encoder.safetensors", header), 3, 36);
  BOOST_REQUIRE_EQUAL(layout.layerEnds.size(), 2);
  BOOST_REQUIRE(layout.source.initializerRangeSource);
  BOOST_CHECK_EQUAL(layout.source.initializerRangeSource->size(), data.size() * sizeof(float));
  BOOST_CHECK_THROW(layout.source.initializerRangeSource->read(0, 2U << 20), std::out_of_range);
  onnx::ModelProto graph;
  BOOST_REQUIRE(graph.ParseFromArray(layout.source.modelBytes.data(), layout.source.modelBytes.size()));
  std::map<std::uint64_t, std::uint64_t> ranges;
  for (const auto& weight : graph.graph().initializer()) {
    if (weight.data_type() != onnx::TensorProto::FLOAT ||
        weight.name().find("epsilon") != std::string::npos ||
        weight.name().find("width") != std::string::npos ||
        weight.name().find("masked-bias") != std::string::npos)
      continue;
    BOOST_CHECK_EQUAL(weight.data_location(), onnx::TensorProto::EXTERNAL);
    BOOST_CHECK(weight.raw_data().empty() && weight.float_data().empty());
    std::map<std::string, std::string> fields;
    for (const auto& field : weight.external_data())
      BOOST_CHECK(fields.emplace(field.key(), field.value()).second);
    BOOST_CHECK_EQUAL(fields.at("location"), "model.onnx.data");
    BOOST_CHECK(ranges.emplace(std::stoull(fields.at("offset")),
                               std::stoull(fields.at("length"))).second);
  }
  std::uint64_t covered = 0;
  for (const auto& [offset, length] : ranges) {
    BOOST_CHECK_EQUAL(offset, covered);
    covered += length;
  }
  BOOST_CHECK_EQUAL(covered, data.size() * sizeof(float));

  const NativeStageLayout ranks{"checkpoint", nativePlanningDigest("fixture/umt5/checkpoint"),
                                 {"/umt5/rank0", "/umt5/rank1"}};
  const std::array<NativeStageLayout, 2> stages{{
    {"prefix", nativePlanningDigest("fixture/umt5/prefix"), {"/umt5/first"}},
    {"suffix", nativePlanningDigest("fixture/umt5/suffix"), {"/umt5/last"}},
  }};
  const auto tp = makeUmt5TensorParallelPlan(layout, ranks);
  const auto pp = makeUmt5PipelinePlan(layout, 1, stages);

  const auto descriptor = ndnsf::di::fixture::completeModel({
    "umt5-checkpoint", nativePlanningDigest("umt5/content"), nativePlanningDigest("umt5/model"),
    nativePlanningDigest("umt5/graph"), "onnx", "fp32", "wan", "1"});
  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("umt5/profile"), nativePlanningDigest("umt5/assembler"),
    "fixture-ort", "fp32", "none", "native", "right", "umt5", 16U << 20, 16U << 20, 4096};
  std::map<std::string, NativeRoleResourceRequirement> requirements;
  for (const auto& role : tp.plan.roles)
    requirements[role] = {descriptor.adapter.backends, 4096, 4096, 0, 4096, 4096, 1.0};
  const auto config = makeCatalogTemplate(descriptor, profile, "/fixture/umt5/source",
    nativePlanningDigest("umt5/source"), "/fixture/umt5/package", "/fixture/umt5", requirements);
  auto request = offlineCheckpointRequest("umt5", directory / "encoder.safetensors", {ranks},
    {{"text_tokens", 3}, {"score_bytes", 36}}, control);
  request["catalog"] = config;
  auto invalid = request;
  invalid["shards"]["encoder.safetensors"]["digest"] = nativePlanningDigest("wrong");
  BOOST_CHECK_EXCEPTION(prepareWanCheckpointComponent(
    invalid, directory, directory / "bad-digest", control), std::runtime_error,
    [](const auto& error) {
      return std::string_view(error.what()) == "DI_NATIVE_ONNX_INITIALIZER_DIGEST";
    });
  BOOST_CHECK(!std::filesystem::exists(directory / "bad-digest"));
  const auto catalogPath = prepareWanCheckpointComponent(request, directory, directory / "prepared",
                                                        control);
  BOOST_CHECK_THROW(prepareWanCheckpointComponent(request, directory, directory / "prepared",
                                                  control), std::invalid_argument);
  auto baselineControl = control;
  baselineControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  const auto baselineRequest = offlineCheckpointRequest("umt5", directory / "encoder.safetensors",
    {{"single", {}, {"provider"}}}, request.at("geometry"), baselineControl);
  BOOST_CHECK(std::filesystem::is_regular_file(prepareWanCheckpointComponent(
    baselineRequest, directory, directory / "single", baselineControl)));
  const auto readWire = [](const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    BOOST_REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), {});
  };
  const auto catalog = readWire(catalogPath);
  const auto parsed = nativeParseJson(catalog).at("source");
  const auto graphWire = readWire(catalogPath.parent_path() / parsed.at("file").get<std::string>());
  NativeCanonicalSource reopened;
  reopened.modelBytes.assign(graphWire.begin(), graphWire.end());
  reopened.initializerRangeSource = openNativeCanonicalInitializerFile(
    (catalogPath.parent_path() / parsed.at("initializer_file").get<std::string>()).string(),
    parsed.at("initializer_bytes").get<std::uint64_t>(),
    parsed.at("initializer_digest").get<std::string>(), control);
  const auto admitted = NativeRequestCatalog::load(catalog, std::move(reopened), control);
  BOOST_REQUIRE_EQUAL(admitted.planningCache->candidates.at(0).roles.size(), 2);

  auto material = layout.source;
  material.materialManifest = deriveNativeCanonicalMaterialManifest(material, control);
  BOOST_REQUIRE(material.materialManifest);

  const auto rms = [](Pair row, Pair gain) {
    const auto factor = 1 / std::sqrt((row[0] * row[0] + row[1] * row[1]) / 2 + 1e-6);
    for (std::size_t i = 0; i < row.size(); ++i) row[i] *= factor * gain[i];
    return row;
  };
  const auto linear = [](const auto& weights, const auto& input) {
    std::array<double, std::tuple_size_v<std::decay_t<decltype(weights)>>> result{};
    for (std::size_t row = 0; row < weights.size(); ++row)
      result[row] = std::inner_product(weights[row].begin(), weights[row].end(),
                                      input.begin(), 0.0);
    return result;
  };
  std::array<Pair, 2> expected{embedding[0], embedding[1]};
  for (const auto& gain : gains) {
    std::array<Pair, 2> q, k, v;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto normalized = rms(expected[i], gain);
      q[i] = linear(query, normalized);
      k[i] = linear(key, normalized);
      v[i] = linear(value, normalized);
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
      Pair context{};
      for (std::size_t head = 0; head < context.size(); ++head) {
        Pair probabilities{};
        for (std::size_t j = 0; j < expected.size(); ++j) {
          const auto bucket = i == j ? 0 : (i < j ? 3 : 1);
          probabilities[j] = std::exp(q[i][head] * k[j][head] + bias[bucket][head]);
        }
        for (std::size_t j = 0; j < expected.size(); ++j)
          context[head] += v[j][head] * probabilities[j] /
                           (probabilities[0] + probabilities[1]);
      }
      const auto projected = linear(output, context);
      for (std::size_t c = 0; c < projected.size(); ++c) expected[i][c] += projected[c];
      const auto normalized = rms(expected[i], {1.25, 0.75});
      const auto gated = linear(gate, normalized);
      auto expanded = linear(expand, normalized);
      for (std::size_t c = 0; c < expanded.size(); ++c) {
        const auto g = gated[c];
        expanded[c] *= 0.5 * g *
          (1 + std::tanh(std::sqrt(2 / std::acos(-1.0)) * (g + 0.044715 * g * g * g)));
      }
      const auto reduced = linear(contract, expanded);
      for (std::size_t c = 0; c < reduced.size(); ++c) expected[i][c] += reduced[c];
    }
  }
  for (auto& row : expected) row = rms(row, {0.75, 1.25});

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-checkpoint-umt5");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<std::int64_t, 3> ids{0, 1, 2};
  std::array<bool, 3> mask{true, true, false};
  const std::array<std::int64_t, 2> dims{1, 3};
  for (const auto* candidate : {&tp, &pp}) {
    std::map<std::string, Ort::Value> inputs;
    inputs.emplace("token-ids", Ort::Value::CreateTensor<std::int64_t>(
      memory, ids.data(), ids.size(), dims.data(), dims.size()));
    inputs.emplace("key-mask", Ort::Value::CreateTensor<bool>(
      memory, mask.data(), mask.size(), dims.data(), dims.size()));
    runCompiledEntriesFixture(*candidate, material, inputs, env, options, memory, control,
                              candidate == &tp);
    const auto& context = inputs.at("text-context");
    BOOST_REQUIRE((context.GetTensorTypeAndShapeInfo().GetShape() ==
                   std::vector<std::int64_t>{1, 2, 2}));
    const auto* actual = context.GetTensorData<float>();
    for (std::size_t i = 0; i < expected.size(); ++i) {
      for (std::size_t c = 0; c < expected[i].size(); ++c)
        BOOST_CHECK_SMALL(actual[2 * i + c] - expected[i][c], 2e-6);
    }
  }

  if (const auto* root = std::getenv("NDNSF_DI_WAN_CHECKPOINT_METADATA_ROOT")) {
    const std::filesystem::path metadataRoot(root);
    const auto recipe = nativeParseJson(readWire(metadataRoot / "recipe.json"));
    const std::string model = "models_t5_umt5-xxl-enc-bf16.pth";
    const auto metadata = readWire(metadataRoot / (model + ".metadata.json"));
    const auto& descriptor = recipe.at("pytorch_mapped_metadata").at(model);
    BOOST_REQUIRE_EQUAL(nativePlanningDigest(metadata),
                        "sha256:" + descriptor.at("metadata_sha256").get<std::string>());
    const auto actual = nativeParseJson(metadata);
    BOOST_REQUIRE_EQUAL(actual.size(), 242);
    NativeJson archiveHeader = NativeJson::object();
    std::uint64_t offset = 0;
    for (const auto& [name, weight] : actual.items()) {
      BOOST_CHECK_EQUAL(weight.at("dtype").get<std::string>(), "torch.bfloat16");
      const auto shape = weight.at("shape").get<std::vector<std::uint64_t>>();
      std::uint64_t bytes = sizeof(float);
      for (const auto axis : shape) {
        BOOST_REQUIRE(axis > 0 && axis <= std::numeric_limits<std::uint64_t>::max() / bytes);
        bytes *= axis;
      }
      BOOST_REQUIRE(bytes <= std::numeric_limits<std::uint64_t>::max() - offset);
      archiveHeader[name] = {{"dtype", "F32"}, {"shape", shape},
                             {"data_offsets", {offset, offset + bytes}}};
      offset += bytes;
    }
    const auto archive = nativeCanonicalJson(archiveHeader);
    std::string wire(8, '\0');
    for (std::size_t i = 0; i < wire.size(); ++i)
      wire[i] = static_cast<char>((static_cast<std::uint64_t>(archive.size()) >> (8 * i)) & 0xff);
    wire += archive;
    const auto size = wire.size() + offset;
    const WanCheckpoint checkpoint({
      {"umt5-xxl.safetensors", std::make_shared<HeaderSource>(std::move(wire), size)}});
    const auto official = lowerWan21Umt5Encoder(checkpoint, 512, 32U << 20);
    BOOST_REQUIRE_EQUAL(official.layerEnds.size(), 24);
    BOOST_CHECK_EQUAL(official.source.initializerRangeSource->size(), offset);
    BOOST_CHECK_THROW(official.source.initializerRangeSource->read(0, 4), std::out_of_range);
    const auto productionTp = makeUmt5TensorParallelPlan(official, ranks);
    const auto productionPp = makeUmt5PipelinePlan(official, 12, stages);
    BOOST_CHECK_EQUAL(productionTp.plan.roles.size(), 2);
    BOOST_CHECK_EQUAL(productionPp.plan.roles.size(), 2);
    for (const auto* candidate : {&productionTp, &productionPp}) {
      onnx::ModelProto fullModel;
      BOOST_REQUIRE(fullModel.ParseFromArray(official.source.modelBytes.data(),
                                            official.source.modelBytes.size()));
      auto tiled = tileWanExactActivations(std::move(fullModel), *candidate, 32U << 20);
      for (auto& step : tiled.execution.plan.steps) {
        if (step.operation != "CALL_SUBGRAPH") continue;
        step.onnxNodeIndices = tiled.execution.entries.at(step.artifactEntry).nodeIndices;
        std::sort(step.onnxNodeIndices.begin(), step.onnxNodeIndices.end());
      }
      if (candidate == &productionTp) {
        const auto roleWeights = checkOfficialTpRoleWeights(
          "UMT5", tiled.model.graph(), tiled.execution.plan, offset);
        for (const auto& role : tiled.execution.plan.roles) {
          const auto& usage = roleWeights.at(role);
          BOOST_CHECK_EQUAL(usage.uniqueBytes, offset / 2);
          BOOST_CHECK_EQUAL(usage.entryBytes, usage.uniqueBytes);
        }
      }
      const auto steps = nativeCanonicalJson(nativeV2StepTemplateJson(tiled.execution.plan));
      const auto dependencies = nativeCanonicalJson(
        nativeDependenciesJson(tiled.execution.plan.dependencies));
      const auto plan = nativeCanonicalOnnxStepPlanFromJson(
        nativeParseJson(steps), nativeParseJson(dependencies));
      BOOST_CHECK_EQUAL(plan.steps.size(), tiled.execution.plan.steps.size());
      BOOST_CHECK_EQUAL(plan.dependencies.size(), tiled.execution.plan.dependencies.size());
      BOOST_TEST_MESSAGE((candidate == &productionTp ? "TP" : "PP") <<
                         " V2 template bytes " << steps.size() <<
                         ", dependencies bytes " << dependencies.size());
    }
    BOOST_TEST_MESSAGE("Official UMT5: " << offset << " FP32 bytes, "
                       << official.source.modelBytes.size() << " ONNX bytes, "
                       << productionTp.plan.steps.size() << " TP steps, "
                       << productionTp.plan.dependencies.size() << " TP dependencies");
  }
}

BOOST_AUTO_TEST_CASE(Umt5EncoderConnectsTwoLayersAndTrimsUsingTokenizerMask)
{
  const auto sourceFixture = videoFixture::makeUmt5Source();
  const auto& [table, embedding, layers, final] = sourceFixture;
  const auto encoder = connectUmt5Encoder(embedding, layers, final);
  BOOST_CHECK_THROW(connectUmt5Encoder(embedding, {}, final), std::invalid_argument);
  BOOST_CHECK_THROW(connectUmt5Encoder(embedding, {layers.front()},
    lowerUmt5Rms(2, videoFixture::floatTensor({2}, {1, 1}), videoFixture::UMT5_EPSILON)), std::invalid_argument);
  BOOST_CHECK(encoder.source.initializerRangeSource == embedding.source.initializerRangeSource);
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
  auto source = encoder.source;
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  const auto json = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  onnx::ModelProto proto;
  BOOST_REQUIRE(proto.ParseFromArray(source.modelBytes.data(), source.modelBytes.size()));
  const NativeStageLayout stage{"umt5/encoder", nativePlanningDigest("fixture/umt5"),
                                {"umt5/rank0", "umt5/rank1"}};
  const auto compiled = makeUmt5TensorParallelPlan(encoder, stage);
  BOOST_REQUIRE_EQUAL(compiled.entries.size(), encoder.entries.size());
  BOOST_CHECK_EQUAL(compiled.plan.steps.size(), compiled.entries.size() + 14);
  BOOST_CHECK(compiled.plan.roles == stage.unitRoleIds);
  auto incomplete = encoder;
  incomplete.reductions.erase(incomplete.reductions.begin());
  BOOST_CHECK_THROW(makeUmt5TensorParallelPlan(incomplete, stage), std::invalid_argument);
  std::set<std::uint64_t> covered;
  std::set<std::string> tokenConsumers, maskConsumers;
  for (const auto& step : compiled.plan.steps) {
    if (step.operation == "SUM") continue;
    for (const auto& input : step.inputSlots) {
      if (input.source != "EPOCH_INPUT") continue;
      if (input.slotId == "token-ids") {
        BOOST_CHECK_EQUAL(input.dtype, "int64");
        tokenConsumers.insert(step.unitId);
      }
      else if (input.slotId == "key-mask") {
        BOOST_CHECK_EQUAL(input.dtype, "bool");
        maskConsumers.insert(step.unitId);
      }
    }
    const auto& cut = compiled.entries.at(step.artifactEntry);
    const auto other = step.unitId == stage.unitRoleIds[0] ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*source.materialManifest, index, cut,
      [&] (const auto& weight) { return weight.find(other) == std::string::npos; });
    for (const auto node : cut.nodeIndices) BOOST_CHECK(covered.insert(node).second);
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(selected, cut.nodeIndices,
                          cut.inputs, cut.outputs, control));
  }
  BOOST_CHECK_EQUAL(tokenConsumers.size(), 2);
  BOOST_CHECK_EQUAL(maskConsumers.size(), 2);
  const auto& finalStep = compiled.plan.steps.back();
  BOOST_CHECK_EQUAL(finalStep.unitId, stage.unitRoleIds[0]);
  BOOST_REQUIRE_EQUAL(finalStep.outputSlots.size(), 1);
  const auto& context = finalStep.outputSlots[0];
  BOOST_CHECK_EQUAL(context.slotId, "text-context");
  BOOST_REQUIRE_EQUAL(context.shape.size(), 3);
  BOOST_CHECK_EQUAL(std::get<std::string>(context.shape[1]), "valid_tokens");
  const std::array<NativeStageLayout, 2> stages{{
    {"encoder-first", nativePlanningDigest("fixture/umt5-first"), {"umt5/first"}},
    {"encoder-later", nativePlanningDigest("fixture/umt5-later"), {"umt5/later"}},
  }};
  const auto pipeline = makeUmt5PipelinePlan(encoder, 1, stages);
  BOOST_CHECK_THROW(makeUmt5PipelinePlan(encoder, 2, stages), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(pipeline.plan.dependencies.size(), 1);
  BOOST_REQUIRE_EQUAL(pipeline.plan.steps.size(), 2);
  const auto descriptor = ndnsf::di::fixture::completeModel({
    "tiny-wan-umt5", nativePlanningDigest("umt5-content"), nativePlanningDigest("umt5-model"),
    nativePlanningDigest("umt5-graph"), "onnx", "fp32", "wan", "1"});
  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("umt5-profile"), nativePlanningDigest("umt5-assembler"),
    "fixture-ort", "fp32", "none", "native", "right", "umt5", 4U << 20, 4U << 20, 4096};
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-component-output");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  const std::filesystem::path directory(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&directory) {
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);
  };
  for (const auto* layout : {&compiled, &pipeline}) {
    std::map<std::string, NativeRoleResourceRequirement> resources;
    for (const auto& role : layout->plan.roles)
      resources[role] = {descriptor.adapter.backends, 1024, 1024, 0, 1024, 1024, 1.0};
    const auto config = makeCatalogTemplate(descriptor, profile, "/fixture/umt5/source",
      nativePlanningDigest("umt5-manifest"), "/fixture/umt5/package", "/fixture/umt5", resources);
    const NativeAssemblyControl prepare{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 4U << 20};
    auto package = compileWanComponent(encoder.source, *layout, 32U << 20, config, prepare);
    BOOST_CHECK(package.source.initializerRangeSource == encoder.source.initializerRangeSource);
    BOOST_CHECK(package.source.modelBytes == encoder.source.modelBytes);
    BOOST_CHECK(!package.source.initializerBytes);
    const auto expected = nativeCanonicalInitializerDigest(encoder.source, prepare);
    const auto pinned = nativeParseJson(package.configurationJson).at("source");
    BOOST_CHECK_EQUAL(pinned.at("initializer_digest").get<std::string>(), expected);
    if (layout == &compiled) {
      const auto destination = directory / "tp";
      const auto catalogFile = writeWanComponent(package, destination, prepare);
      BOOST_CHECK(catalogFile == destination / "catalog.json");
      BOOST_CHECK_THROW(writeWanComponent(package, destination, prepare), std::invalid_argument);
      std::ifstream configStream(catalogFile, std::ios::binary);
      const std::string catalogText((std::istreambuf_iterator<char>(configStream)), {});
      const auto saved = nativeParseJson(catalogText).at("source");
      BOOST_CHECK_EQUAL(saved.at("file").get<std::string>(), "model.onnx");
      BOOST_CHECK_EQUAL(saved.at("initializer_file").get<std::string>(), "model.onnx.data");
      const auto modelPath = destination / saved.at("file").get<std::string>();
      std::ifstream modelStream(modelPath, std::ios::binary);
      NativeCanonicalSource persisted;
      persisted.modelBytes.assign(std::istreambuf_iterator<char>(modelStream), {});
      BOOST_CHECK_EQUAL(persisted.modelBytes.size(), saved.at("bytes").get<std::uint64_t>());
      persisted.initializerRangeSource = openNativeCanonicalInitializerFile(
        (destination / saved.at("initializer_file").get<std::string>()).string(),
        saved.at("initializer_bytes").get<std::uint64_t>(),
        saved.at("initializer_digest").get<std::string>(), prepare);
      const auto fromDisk = NativeRequestCatalog::load(catalogText, std::move(persisted), prepare);
      BOOST_CHECK_EQUAL(fromDisk.model.canonicalInitializerObjectDigest, expected);

      auto missing = package.source;
      missing.initializerRangeSource.reset();
      BOOST_CHECK_EXCEPTION(
        NativeRequestCatalog::load(package.configurationJson, std::move(missing), prepare),
        std::invalid_argument, [](const auto& error) {
          return std::string(error.what()) == "pinned initializer object is missing";
        });
      auto changed = package.source;
      auto wrong = std::make_shared<videoFixture::EmbeddingTable>();
      wrong->bytes = table->bytes;
      wrong->bytes.front() ^= 1;
      changed.initializerRangeSource = std::move(wrong);
      auto corrupted = package;
      corrupted.source = changed;
      BOOST_CHECK_THROW(writeWanComponent(corrupted, directory / "invalid", prepare),
                        std::runtime_error);
      BOOST_REQUIRE_EQUAL(std::distance(std::filesystem::directory_iterator(directory),
                                        std::filesystem::directory_iterator()), 1);
      BOOST_CHECK_EXCEPTION(
        NativeRequestCatalog::load(package.configurationJson, std::move(changed), prepare),
        std::invalid_argument, [](const auto& error) {
          return std::string(error.what()) == "request initializer bytes differ from pinned digest";
        });
    }
    const auto loaded = NativeRequestCatalog::load(package.configurationJson,
                                                    std::move(package.source), prepare);
    BOOST_CHECK_EQUAL(loaded.model.canonicalInitializerBytes, sizeof(videoFixture::UMT5_WORDS));
    BOOST_CHECK_EQUAL(loaded.model.canonicalInitializerObjectDigest, expected);
    const auto& frozen = loaded.planningCache->candidates.at(0);
    BOOST_CHECK(frozen.candidate.executionPlan.roles == layout->plan.roles);
    BOOST_CHECK_EQUAL(frozen.roles.size(), layout->plan.roles.size());
  }
  // Two RMS and two reduction Adds for each layer, plus the final RMS Add.
  BOOST_CHECK_EQUAL(covered.size() + 9, static_cast<std::size_t>(proto.graph().node_size()));
  std::vector<std::uint64_t> nodes;
  for (int i = 0; i < proto.graph().node_size(); ++i) nodes.push_back(i);
  source.materialPayloads = source.materialManifest->payloads;
  const auto assembled = materializeNativeCanonicalModel(source, nodes, control);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-umt5-encoder");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session runner(env, assembled.data(), assembled.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<std::int64_t, 3> ids{0, 1, 2};
  std::array<bool, 3> mask{true, true, false};
  constexpr std::array<std::int64_t, 2> dims{1, 3};
  const std::array<Ort::Value, 2> values{
    Ort::Value::CreateTensor<std::int64_t>(memory, ids.data(), ids.size(), dims.data(), dims.size()),
    Ort::Value::CreateTensor<bool>(memory, mask.data(), mask.size(), dims.data(), dims.size())};
  const std::array<const char*, 2> inputs{"token-ids", "key-mask"};
  const char* output = "text-context";
  for (int valid = 2; valid >= 1; --valid) {
    mask[1] = valid == 2;
    auto result = runner.Run(Ort::RunOptions{}, inputs.data(), values.data(),
                              values.size(), &output, 1);
    BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, valid, 2}));
    const auto expected = videoFixture::referenceUmt5(ids, valid);
    const auto* data = result.front().GetTensorData<float>();
    std::map<std::string, Ort::Value> ppValues;
    ppValues.emplace("token-ids", Ort::Value::CreateTensor<std::int64_t>(
      memory, ids.data(), ids.size(), dims.data(), dims.size()));
    ppValues.emplace("key-mask", Ort::Value::CreateTensor<bool>(
      memory, mask.data(), mask.size(), dims.data(), dims.size()));
    runCompiledEntriesFixture(pipeline, source, ppValues, env, options, memory, control);
    const auto& ppContext = ppValues.at("text-context");
    BOOST_REQUIRE(ppContext.GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, valid, 2}));
    for (std::size_t element = 0; element < expected.size(); ++element) {
      BOOST_CHECK_SMALL(data[element] - expected[element], 2e-6);
      BOOST_CHECK_SMALL(ppContext.GetTensorData<float>()[element] - expected[element], 2e-6);
    }
  }
}

BOOST_AUTO_TEST_CASE(VaeCausalConvFeedsOnlyRankLocalHistoryAcrossFrames)
{
  onnx::TensorProto weight, bias;
  weight.set_data_type(onnx::TensorProto::FLOAT);
  bias.set_data_type(onnx::TensorProto::FLOAT);
  for (const auto dim : {3, 3, 3, 1, 3}) weight.add_dims(dim);
  bias.add_dims(3);
  for (int i = 0; i < 81; ++i) weight.add_float_data((i * 3 + 5) % 13 - 6);
  for (const float value : {-1, 2, 5}) bias.add_float_data(value);
  const auto layout = lowerVaeCausalConv({1, 1, 2}, weight, bias);
  BOOST_CHECK_THROW(lowerVaeCausalConv({1, 0, 2}, weight, bias), std::invalid_argument);
  auto even = weight;
  even.set_dims(2, 2);
  BOOST_CHECK_THROW(lowerVaeCausalConv({1, 1, 2}, even, bias), std::invalid_argument);

  const std::array<std::array<float, 6>, 3> frames{{
    {1, -2, 3, 0, 4, -1}, {4, 2, -3, 1, 0, 8}, {5, -1, 1, -4, 6, 2}}};
  NativeCanonicalSource source;
  const auto wire = layout.model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto manifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(manifest);
  const auto json = manifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  const auto assemble = [&] (const OnnxEntryCut& entry, auto allowed) {
    auto selected = selectCanonicalEntry(*manifest, index, entry, allowed);
    return materializeNativeCanonicalModel(selected, entry.nodeIndices, entry.inputs,
                                           entry.outputs, control);
  };
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-causal");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<Ort::Session, 2> runners{Ort::Session{nullptr}, Ort::Session{nullptr}};
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto prefix = "rank-" + std::to_string(rank) + "/";
    const auto bytes = assemble(layout.partials[rank], [&] (const auto& name) {
      return name.rfind(prefix, 0) == 0 || name == "channel-axis" || name == "time-axis" ||
        name == "history-begin" || name == "history-end";
    });
    runners[rank] = Ort::Session(env, bytes.data(), bytes.size(), options);
  }
  const auto biasBytes = assemble(layout.addBias, [] (const auto& name) { return name == "bias"; });
  Ort::Session finalize(env, biasBytes.data(), biasBytes.size(), options);
  std::array<std::vector<float>, 2> histories{std::vector<float>(8), std::vector<float>(4)};
  const std::array<std::int64_t, 5> frameShape{1, 3, 1, 1, 2};

  for (int frame = 0; frame < 3; ++frame) {
    auto current = frames[frame];
    std::array<std::array<float, 6>, 2> partials{};
    for (int rank = 0; rank < 2; ++rank) {
      const auto& entry = layout.partials[rank];
      const auto channels = rank == 0 ? 2 : 1;
      const std::array<std::int64_t, 5> historyShape{1, channels, 2, 1, 2};
      const std::array<Ort::Value, 2> arguments{
        Ort::Value::CreateTensor<float>(memory, current.data(), current.size(),
                                        frameShape.data(), frameShape.size()),
        Ort::Value::CreateTensor<float>(memory, histories[rank].data(), histories[rank].size(),
                                        historyShape.data(), historyShape.size())};
      const std::array<const char*, 2> input{
        entry.inputs[0].name.c_str(), entry.inputs[1].name.c_str()};
      const std::array<const char*, 2> output{
        entry.outputs[0].name.c_str(), entry.outputs[1].name.c_str()};
      const auto values = runners[rank].Run(Ort::RunOptions{}, input.data(), arguments.data(),
                                            arguments.size(), output.data(), output.size());
      BOOST_REQUIRE(values[1].GetTensorTypeAndShapeInfo().GetShape() ==
        (std::vector<std::int64_t>(historyShape.begin(), historyShape.end())));
      const auto* next = values[1].GetTensorData<float>();
      std::copy_n(next, histories[rank].size(), histories[rank].begin());
      const auto begin = rank == 0 ? 0 : 2;
      for (int c = 0; c < channels; ++c) {
        for (int x = 0; x < 2; ++x) {
          BOOST_CHECK_EQUAL(next[c * 4 + x], frame == 0 ? 0 : frames[frame - 1][2 * (c + begin) + x]);
          BOOST_CHECK_EQUAL(next[c * 4 + 2 + x], current[2 * (c + begin) + x]);
        }
      }
      const auto* data = values[0].GetTensorData<float>();
      for (int out = 0; out < 3; ++out) {
        for (int x = 0; x < 2; ++x) {
          int expected = 0;
          for (int in = begin; in < begin + channels; ++in) {
            for (int lag = 0; lag < 3; ++lag) {
              const auto inputFrame = frame + lag - 2;
              if (inputFrame < 0) continue;
              for (int dx = 0; dx < 3; ++dx) {
                const auto inputX = x + dx - 1;
                if (inputX < 0 || inputX > 1) continue;
                const auto index = ((out * 3 + in) * 3 + lag) * 3 + dx;
                expected += weight.float_data(index) * frames[inputFrame][in * 2 + inputX];
              }
            }
          }
          BOOST_CHECK_EQUAL(data[out * 2 + x], expected);
          partials[rank][out * 2 + x] = data[out * 2 + x];
        }
      }
    }
    const auto& port = layout.addBias.inputs.front();
    std::vector<std::vector<NamedTensorView>> operands;
    for (const auto& partial : partials) {
      operands.push_back({{port.name, TensorElementType::Float32,
        {frameShape.begin(), frameShape.end()},
        {reinterpret_cast<const std::uint8_t*>(partial.data()), sizeof(partial)}}});
    }
    const auto sum = sumTensorBundleViews(port.name, operands);
    const auto decoded = decodeTensorBundleViews(sum);
    std::array<float, 6> combined{};
    BOOST_REQUIRE_EQUAL(decoded.front().payload.size(), sizeof(combined));
    std::memcpy(combined.data(), decoded.front().payload.data(), sizeof(combined));
    const auto input = Ort::Value::CreateTensor<float>(memory, combined.data(), combined.size(),
                                                       frameShape.data(), frameShape.size());
    const auto* name = port.name.c_str();
    const char* output = "decoded";
    const auto result = finalize.Run(Ort::RunOptions{}, &name, &input, 1, &output, 1);
    for (std::size_t voxel = 0; voxel < combined.size(); ++voxel) {
      BOOST_CHECK_EQUAL(result.front().GetTensorData<float>()[voxel],
                        partials[0][voxel] + partials[1][voxel] + bias.float_data(voxel / 2));
    }
  }
}

BOOST_AUTO_TEST_CASE(VaeUpsamplePreservesFirstFrameAndInterleavesLaterGroups)
{
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-upsample");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  BOOST_CHECK_THROW(makeVaeUpsample({1, 0, 2}, 2, false), std::invalid_argument);
  for (const bool subsequent : {false, true}) {
    const std::int64_t frames = subsequent ? 2 : 1;
    const std::int64_t channels = subsequent ? 4 : 2;
    const auto model = makeVaeUpsample({frames, 2, 2}, 2, subsequent);
    const auto wire = model.SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    const std::array<std::int64_t, 5> dims{1, channels, frames, 2, 2};
    std::vector<float> data(channels * frames * 4);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<float>(i + 1);
    const auto value = Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                                       dims.data(), dims.size());
    const char* in = "frames";
    const char* out = "upsampled";
    const auto result = session.Run(Ort::RunOptions{}, &in, &value, 1, &out, 1);
    const auto outputFrames = subsequent ? 4 : 1;
    BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, 2, outputFrames, 4, 4}));
    const auto* actual = result.front().GetTensorData<float>();
    for (int c = 0; c < 2; ++c) {
      for (int t = 0; t < outputFrames; ++t) {
        const auto sourceChannel = subsequent ? c + (t % 2) * 2 : c;
        const auto sourceFrame = subsequent ? t / 2 : t;
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const auto index = (sourceChannel * frames + sourceFrame) * 4 + (y / 2) * 2 + x / 2;
            BOOST_CHECK_EQUAL(actual[(c * outputFrames + t) * 16 + y * 4 + x], data[index]);
          }
        }
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(VaeResampleSkipsFirstTimeProjectionAndCarriesSubsequentHistory)
{
  const auto tensor = [] (std::initializer_list<int> dims, const std::vector<float>& values) {
    onnx::TensorProto result;
    result.set_data_type(onnx::TensorProto::FLOAT);
    for (const auto dim : dims) result.add_dims(dim);
    for (const auto value : values) result.add_float_data(value);
    return result;
  };
  std::vector<float> spatialValues(18);
  spatialValues[4] = 2;
  spatialValues[12] = 3;
  const auto ws = tensor({1, 2, 1, 3, 3}, spatialValues);
  const auto bs = tensor({1}, {5});
  const auto wt = tensor({4, 2, 3, 1, 1}, {1, 2, 3, 4, 5, 6, 2, 3, 4, 5, 6, 7,
                                          3, 4, 5, 6, 7, 8, 4, 5, 6, 7, 8, 9});
  const auto bt = tensor({4}, {1, 2, 3, 4});
  const VaeConvolutionParameters spatial{ws, bs};
  const VaeConvolutionParameters temporal{wt, bt};
  const auto first = lowerVaeResample({1, 1, 1}, spatial, temporal, true);
  const auto later = lowerVaeResample({1, 1, 1}, spatial, temporal, false);
  BOOST_CHECK(first.historySuccessorByInput.empty());
  BOOST_CHECK_EQUAL(first.entries.size(), 5);
  BOOST_CHECK_EQUAL(first.entries.count("spatial/rank-1/input-slice"), 1);
  BOOST_CHECK_EQUAL(first.entries.count("temporal/rank-1/input-slice"), 0);
  BOOST_CHECK_EQUAL(later.historySuccessorByInput.size(), 2);
  BOOST_CHECK_EQUAL(later.entries.size(), 9);
  BOOST_CHECK_EQUAL(later.entries.count("spatial/rank-1/input-slice"), 1);
  BOOST_CHECK_EQUAL(later.entries.count("temporal/rank-1/input-slice"), 1);
  auto wrong = wt;
  wrong.set_dims(2, 1);
  BOOST_CHECK_THROW(lowerVaeResample({1, 1, 1}, spatial,
                    VaeConvolutionParameters{wrong, bt}, true), std::invalid_argument);
  BOOST_CHECK_THROW(lowerVaeResample({2, 1, 1}, spatial, temporal, true), std::invalid_argument);

  using History = std::array<float, 2>;
  std::map<std::string, History> histories;
  for (const auto& [input, output] : later.historySuccessorByInput)
    histories[input] = {};
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-resample");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const auto makeSession = [&] (const VaeStageLayout& layout) {
    NativeCanonicalSource source;
    const auto wire = layout.model.SerializeAsString();
    source.modelBytes.assign(wire.begin(), wire.end());
    const NativeAssemblyControl control{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 2U << 20, 2U << 20};
    source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
    BOOST_REQUIRE(source.materialManifest);
    const auto json = source.materialManifest->canonicalJson();
    const auto index = parseNativeCanonicalMaterialManifest(
      std::vector<std::uint8_t>(json.begin(), json.end()));
    for (const auto& [id, entry] : layout.entries) {
      const auto other = id.find("rank-0") != std::string::npos ? "rank-1/" : "rank-0/";
      auto selected = selectCanonicalEntry(*source.materialManifest, index, entry,
        [&] (const auto& name) {
          return (id.find("rank-") == std::string::npos || name.find(other) == std::string::npos) &&
                 (!layout.historySuccessorByInput.empty() || name.find("temporal/") != 0);
        });
      BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(selected, entry.nodeIndices,
                            entry.inputs, entry.outputs, control));
    }
    source.materialPayloads = source.materialManifest->payloads;
    std::vector<std::uint64_t> all(layout.model.graph().node_size());
    std::iota(all.begin(), all.end(), 0);
    const auto bytes = materializeNativeCanonicalModel(source, all, control);
    return Ort::Session(env, bytes.data(), bytes.size(), options);
  };
  auto firstSession = makeSession(first);
  auto laterSession = makeSession(later);
  std::array<std::array<float, 2>, 4> frames{{{20, 30}, {2, 3}, {5, 7}, {11, 13}}};
  const std::array<std::int64_t, 5> frameShape{1, 2, 1, 1, 1};
  const std::array<std::int64_t, 5> historyShape{1, 1, 2, 1, 1};
  for (std::size_t t = 0; t < frames.size(); ++t) {
    const auto& layout = t == 0 ? first : later;
    auto& session = t == 0 ? firstSession : laterSession;
    std::vector<const char*> inputs{"frames"}, outputs{"spatial/decoded"};
    std::vector<Ort::Value> values;
    values.push_back(Ort::Value::CreateTensor<float>(memory, frames[t].data(), 2,
                                                     frameShape.data(), frameShape.size()));
    for (const auto& [input, output] : layout.historySuccessorByInput) {
      auto& history = histories.at(input);
      inputs.push_back(input.c_str());
      outputs.push_back(output.c_str());
      values.push_back(Ort::Value::CreateTensor<float>(memory, history.data(), history.size(),
                                                       historyShape.data(), historyShape.size()));
    }
    const auto result = session.Run(Ort::RunOptions{}, inputs.data(), values.data(), values.size(),
                                    outputs.data(), outputs.size());
    const int count = t == 0 ? 1 : 2;
    BOOST_REQUIRE(result[0].GetTensorTypeAndShapeInfo().GetShape() ==
                  (std::vector<std::int64_t>{1, 1, count, 2, 2}));
    const auto group = [&] (int channel, int phase) -> double {
      if (t == 0) return frames[t][channel];
      const int out = phase * 2 + channel;
      double value = bt.float_data(out);
      for (int c = 0; c < 2; ++c) {
        for (int lag = 0; lag < 3 && t > static_cast<std::size_t>(lag); ++lag)
          value += wt.float_data((out * 2 + c) * 3 + 2 - lag) * frames[t - lag][c];
      }
      return value;
    };
    const auto* decoded = result[0].GetTensorData<float>();
    for (int phase = 0; phase < count; ++phase) {
      for (int voxel = 0; voxel < 4; ++voxel) {
        const auto expected = 2 * group(0, phase) + (voxel % 2 ? 3 * group(1, phase) : 0) + 5;
        BOOST_CHECK_EQUAL(decoded[phase * 4 + voxel], expected);
      }
    }
    std::size_t rank = 0;
    for (const auto& [input, output] : layout.historySuccessorByInput) {
      const auto* next = result[rank + 1].GetTensorData<float>();
      BOOST_CHECK_EQUAL(next[0], t == 1 ? 0 : frames[t - 1][rank]);
      BOOST_CHECK_EQUAL(next[1], frames[t][rank]);
      histories.at(input) = {next[0], next[1]};
      ++rank;
    }
  }
}

BOOST_AUTO_TEST_CASE(VaeDecoderConnectsLatentScalingAndRgbAcrossCanonicalStages)
{
  const auto zeros = [] (std::initializer_list<int> dims) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto dim : dims) {
      tensor.add_dims(dim);
      count *= dim;
    }
    for (int i = 0; i < count; ++i) tensor.add_float_data(0);
    return tensor;
  };
  auto latent = zeros({2, 2, 1, 1, 1});
  latent.set_float_data(0, 1);
  latent.set_float_data(3, 1);
  const auto latentBias = zeros({2});
  auto input = zeros({4, 2, 3, 3, 3});
  input.set_float_data(22, 1);
  input.set_float_data(3 * 27 + 22, 1);
  auto inputBias = zeros({4});
  inputBias.set_float_data(2, 1);
  inputBias.set_float_data(3, -1);
  const auto attnGain = zeros({4}), qkv = zeros({12, 4, 1, 1, 1}), qkvBias = zeros({12});
  const auto attnOut = zeros({4, 4, 1, 1, 1}), attnBias = zeros({4});
  auto spatial = zeros({2, 4, 1, 3, 3});
  spatial.set_float_data(4, 1);
  spatial.set_float_data(2 * 9 + 4, 1);
  spatial.set_float_data(5 * 9 + 4, 1);
  spatial.set_float_data(7 * 9 + 4, 1);
  const auto spatialBias = zeros({2});
  auto outputGain = zeros({2});
  outputGain.set_float_data(0, 1);
  outputGain.set_float_data(1, 2);
  auto rgb = zeros({3, 2, 3, 3, 3});
  rgb.set_float_data(22, 1);
  rgb.set_float_data(3 * 27 + 22, 2);
  rgb.set_float_data(4 * 27 + 22, 2);
  const auto rgbBias = zeros({3});
  const std::vector<float> mean{1, -1}, inverse{0.5F, 2};
  const VaeConvTile tile{1, 1, 1};
  const VaeDecoderParameters params{
    mean, inverse, {latent, latentBias}, {input, inputBias}, outputGain, {rgb, rgbBias}};
  const std::vector<std::pair<std::string, VaeStageLayout>> stages{
    {"middle", lowerVaeAttention(tile, {attnGain, qkv, qkvBias, attnOut, attnBias}, 1, 4)},
    {"up", lowerVaeResample(tile, {spatial, spatialBias}, {}, true)}};
  const auto decoder = connectVaeDecoder(tile, params, stages);
  BOOST_REQUIRE_EQUAL(decoder.historySuccessorByInput.size(), 4);
  auto bad = params;
  bad.latentInverseStd = {1, 0};
  BOOST_CHECK_THROW(connectVaeDecoder(tile, bad, stages), std::invalid_argument);
  auto duplicates = stages;
  duplicates[1].first = duplicates[0].first;
  BOOST_CHECK_THROW(connectVaeDecoder(tile, params, duplicates), std::invalid_argument);

  NativeCanonicalSource source;
  const auto serialized = decoder.model.SerializeAsString();
  source.modelBytes.assign(serialized.begin(), serialized.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 2U << 20, 2U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  const auto json = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  const NativeStageLayout stage{"vae/phase0", nativePlanningDigest("fixture/vae"),
                                {"vae/rank0", "vae/rank1"}};
  const auto compiled = makeVaeTensorParallelPlan(decoder, stage);
  BOOST_CHECK_EQUAL(compiled.entries.size(), decoder.entries.size());
  BOOST_REQUIRE(!compiled.plan.dependencies.empty());
  auto missingCut = decoder;
  missingCut.reductions.erase(missingCut.reductions.begin());
  BOOST_CHECK_THROW(makeVaeTensorParallelPlan(missingCut, stage), std::invalid_argument);
  auto missingTransfer = compiled.plan;
  missingTransfer.dependencies.pop_back();
  BOOST_CHECK_THROW(missingTransfer.validateSteps(), std::invalid_argument);

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-decoder");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  struct FloatTensor
  {
    std::vector<std::int64_t> shape;
    std::vector<float> data;
  };
  std::map<std::pair<std::string, std::string>, FloatTensor> tensors;
  tensors.emplace(std::make_pair("", "latent"), FloatTensor{{1, 2, 1, 1, 1}, {0.5F, 2}});
  for (const auto& port : decoder.model.graph().input()) {
    if (port.name() == "latent") continue;
    auto& value = tensors[{"", port.name()}];
    std::size_t count = 1;
    for (const auto& dim : port.type().tensor_type().shape().dim()) {
      value.shape.push_back(dim.dim_value());
      count *= static_cast<std::size_t>(dim.dim_value());
    }
    value.data.resize(count);
  }
  const FloatTensor* rgbTensor = nullptr;
  for (const auto& step : compiled.plan.steps) {
    std::vector<const char*> inputs, outputs;
    std::vector<Ort::Value> arguments;
    std::vector<std::vector<NamedTensorView>> contributions;
    for (const auto& slot : step.inputSlots) {
      const auto& producerSlot = slot.producerStepId.empty() ? slot.slotId : slot.producerSlotId;
      auto& supplied = tensors.at({slot.producerStepId, producerSlot});
      if (step.operation == "SUM") {
        contributions.push_back({{"partial", TensorElementType::Float32, supplied.shape,
          {reinterpret_cast<const std::uint8_t*>(supplied.data.data()),
           supplied.data.size() * sizeof(float)}}});
      }
      else {
        inputs.push_back(slot.slotId.c_str());
        arguments.push_back(Ort::Value::CreateTensor<float>(
          memory, supplied.data.data(), supplied.data.size(), supplied.shape.data(),
          supplied.shape.size()));
      }
    }
    if (step.operation == "SUM") {
      const auto& slot = step.outputSlots.front();
      const auto sum = sumTensorBundleViews(slot.slotId, contributions);
      const auto decoded = decodeTensorBundleViews(sum);
      BOOST_REQUIRE_EQUAL(decoded.size(), 1);
      auto& total = tensors[{step.stepId, slot.slotId}];
      total.shape = decoded.front().shape;
      total.data.resize(decoded.front().payload.size() / sizeof(float));
      std::memcpy(total.data.data(), decoded.front().payload.data(), decoded.front().payload.size());
      continue;
    }
    const auto& entry = compiled.entries.at(step.artifactEntry);
    const auto other = step.unitId == stage.unitRoleIds[0] ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*source.materialManifest, index, entry,
      [&] (const auto& name) { return name.find(other) == std::string::npos; });
    const auto bytes = materializeNativeCanonicalModel(selected, entry.nodeIndices,
                                                        entry.inputs, entry.outputs, control);
    Ort::Session session(env, bytes.data(), bytes.size(), options);
    for (const auto& slot : step.outputSlots) outputs.push_back(slot.slotId.c_str());
    const auto result = session.Run(Ort::RunOptions{}, inputs.data(), arguments.data(),
                                    arguments.size(), outputs.data(), outputs.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
      const auto& slot = step.outputSlots[i];
      const auto info = result[i].GetTensorTypeAndShapeInfo();
      const auto* data = result[i].GetTensorData<float>();
      auto& produced = tensors[{step.stepId, slot.slotId}];
      produced.shape = info.GetShape();
      produced.data.assign(data, data + info.GetElementCount());
      if (slot.slotId == "rgb") rgbTensor = &produced;
    }
  }
  BOOST_REQUIRE(rgbTensor);
  BOOST_REQUIRE(rgbTensor->shape == (std::vector<std::int64_t>{1, 3, 1, 2, 2}));
  // The affine latent values are {2,0}. Spatial output is {3,-1} everywhere.
  const auto silu = [] (double x) { return x / (1 + std::exp(-x)); };
  const auto n0 = silu(3 * std::sqrt(2.0 / 10));
  const auto n1 = silu(-2 * std::sqrt(2.0 / 10));
  const std::array<double, 3> expected{1, 2 * n1, std::clamp(2 * n0, -1.0, 1.0)};
  const auto* pixels = rgbTensor->data.data();
  for (std::size_t c = 0; c < expected.size(); ++c) {
    for (std::size_t p = 0; p < 4; ++p)
      BOOST_CHECK_SMALL(double(pixels[c * 4 + p]) - expected[c], 1e-6);
  }
  auto packedWeight = zeros({12, 2, 3, 3, 3});
  auto packedBias = zeros({12});
  for (int i = 0; i < 12; ++i) packedBias.set_float_data(i, (i - 6) / 8.0F);
  VaeDecoderParameters packedParams{mean, inverse, {latent, latentBias},
    {input, inputBias}, outputGain, {packedWeight, packedBias}, 2};
  const auto packed = connectVaeDecoder(tile, packedParams, stages);
  const auto packedWire = packed.model.SerializeAsString();
  Ort::Session packedSession(env, packedWire.data(), packedWire.size(), options);
  std::vector<std::vector<float>> packedInputs;
  std::vector<Ort::Value> packedValues;
  std::vector<std::string> packedNames;
  packedInputs.reserve(packedSession.GetInputCount());
  for (std::size_t i = 0; i < packedSession.GetInputCount(); ++i) {
    const auto type = packedSession.GetInputTypeInfo(i);
    const auto info = type.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    packedInputs.emplace_back(info.GetElementCount(), 0.0F);
    packedValues.push_back(Ort::Value::CreateTensor<float>(memory, packedInputs.back().data(),
      packedInputs.back().size(), shape.data(), shape.size()));
    packedNames.push_back(packed.model.graph().input(i).name());
  }
  std::vector<const char*> packedNamePointers;
  for (const auto& name : packedNames) packedNamePointers.push_back(name.c_str());
  const char* packedOutput = "rgb";
  const auto unpacked = packedSession.Run(Ort::RunOptions{}, packedNamePointers.data(),
    packedValues.data(), packedValues.size(), &packedOutput, 1);
  BOOST_REQUIRE(unpacked.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 3, 1, 4, 4}));
  const auto* unpackedData = unpacked.front().GetTensorData<float>();
  for (int c = 0; c < 3; ++c) {
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        const int sourceChannel = 4 * c + 2 * (x % 2) + y % 2;
        BOOST_CHECK_EQUAL(unpackedData[c * 16 + y * 4 + x],
                          packedBias.float_data(sourceChannel));
      }
    }
  }
  const auto packedPlan = makeVaeTensorParallelPlan(packed, stage);
  BOOST_CHECK(!packedPlan.plan.steps.empty());
  packedParams.outputPatchSize = 3;
  BOOST_CHECK_THROW(connectVaeDecoder(tile, packedParams, stages), std::invalid_argument);

}

BOOST_AUTO_TEST_CASE(Wan21CheckpointDecoderRunsFirstAndLaterFrames)
{
  auto weights = videoFixture::makeVaeWeights();

  const VaeConvTile tile{1, 1, 1};
  const auto lower = [&] (bool first) {
    return lowerWan21VaeDecoder(tile, weights, {0, 0}, {1, 1}, first, 1, 4);
  };
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan-vae-checkpoint");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&scratch) {
    std::error_code cleanup;
    std::filesystem::remove_all(scratch, cleanup);
  };
  const auto file = std::filesystem::path(scratch) / "decoder.safetensors";
  const NativeAssemblyControl checkpointControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 8U << 20, 16U << 20};
  NativeJson header = NativeJson::object();
  std::vector<float> payload;
  for (const auto& [name, weight] : weights) {
    const auto start = payload.size() * sizeof(float);
    payload.insert(payload.end(), weight.float_data().begin(), weight.float_data().end());
    header[name] = {{"dtype", "F32"},
                    {"shape", std::vector<std::int64_t>(weight.dims().begin(), weight.dims().end())},
                    {"data_offsets", {start, payload.size() * sizeof(float)}}};
  }
  const WanCheckpoint checkpoint({
    {file.filename().string(),
     writeFixtureSafeTensors(file, nativeCanonicalJson(header), payload, checkpointControl)}});
  BOOST_REQUIRE_EQUAL(checkpoint.parameters().size(), weights.size());
  const std::array<NativeStageLayout, 2> stages{{
    {"vae-first", nativePlanningDigest("fixture/vae-first"), {"vae/first"}},
    {"vae-later", nativePlanningDigest("fixture/vae-later"), {"vae/later"}},
  }};
  const auto offline = prepareWanCheckpointComponent(
    offlineCheckpointRequest("vae", file, {stages[0], stages[1]},
      {{"height", 1}, {"width", 1}, {"latent_frames", 3}, {"key_tile", 1}, {"score_bytes", 4},
       {"latent_mean", {0, 0}}, {"latent_inverse_std", {1, 1}}}, checkpointControl),
    file.parent_path(), file.parent_path() / "offline", checkpointControl);
  BOOST_CHECK(std::filesystem::exists(offline));
  const auto first =
    lowerWan21VaeDecoder(tile, checkpoint.parameters(), {0, 0}, {1, 1}, true, 1, 4,
                         {checkpoint.reader(), {}});
  const auto next =
    lowerWan21VaeDecoder(tile, checkpoint.parameters(), {0, 0}, {1, 1}, false, 1, 4,
                         {checkpoint.reader(), {}});
  BOOST_CHECK(first.model.SerializeAsString() == lower(true).model.SerializeAsString());
  BOOST_CHECK(next.model.SerializeAsString() == lower(false).model.SerializeAsString());
  BOOST_REQUIRE_EQUAL(first.historySuccessorByInput.size(), 60);
  BOOST_REQUIRE_EQUAL(next.historySuccessorByInput.size(), 64);
  for (const auto& [name, successor] : first.historySuccessorByInput)
    BOOST_CHECK_EQUAL(next.historySuccessorByInput.at(name), successor);
  auto bad = weights.extract("decoder.upsamples.7.time_conv.bias");
  BOOST_CHECK_THROW(lower(true), std::invalid_argument);
  weights.insert(std::move(bad));
  weights.emplace("decoder.unsupported.weight", videoFixture::floatTensor({1}, {0}));
  BOOST_CHECK_THROW(lower(false), std::invalid_argument);

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan21-source-decoder");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const auto prepare = [&] (const auto& layout, const OnnxParallelPlan& compiled,
                             const OnnxParallelPlan& pipeline,
                             std::map<std::string, Ort::Value>& ppValues) {
    NativeCanonicalSource source;
    const auto wire = layout.model.SerializeAsString();
    source.modelBytes.assign(wire.begin(), wire.end());
    const NativeAssemblyControl control{
      std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 4U << 20, 16U << 20};
    source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
    BOOST_REQUIRE(source.materialManifest);
    runCompiledEntriesFixture(pipeline, source, ppValues, env, options, memory, control);
    const auto json = source.materialManifest->canonicalJson();
    const auto index = parseNativeCanonicalMaterialManifest(
      std::vector<std::uint8_t>(json.begin(), json.end()));
    std::set<std::string> checkedRanks;
    for (const auto& step : compiled.plan.steps) {
      if (step.operation != "CALL_SUBGRAPH" || checkedRanks.count(step.unitId) != 0) continue;
      const auto& cut = compiled.entries.at(step.artifactEntry);
      if (std::none_of(cut.nodeIndices.begin(), cut.nodeIndices.end(), [&] (auto node) {
        return layout.model.graph().node(node).op_type() == "ConstantOfShape";
      })) continue;
      const auto other = step.unitId == compiled.plan.roles[0] ? "rank-1/" : "rank-0/";
      auto selected = selectCanonicalEntry(*source.materialManifest, index, cut,
        [&] (const auto& initializer) { return initializer.find(other) == std::string::npos; });
      BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(
        selected, cut.nodeIndices, cut.inputs, cut.outputs, control));
      checkedRanks.insert(step.unitId);
    }
    BOOST_CHECK_EQUAL(checkedRanks.size(), 2);
    source.materialPayloads = source.materialManifest->payloads;
    std::vector<std::uint64_t> all(layout.model.graph().node_size());
    std::iota(all.begin(), all.end(), 0);
    auto lowSource = source;
    auto fullGraphControl = control;
    fullGraphControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    BOOST_TEST_CHECKPOINT("materialize the uncut VAE fixture");
    const auto bytes = materializeNativeCanonicalModel(source, all, fullGraphControl);
    auto lowBudget = fullGraphControl;
    lowBudget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Fitting the assembled result alone is insufficient during serialization.
    lowBudget.maxAssembledBytes = bytes.size();
    BOOST_TEST_CHECKPOINT("reject the VAE fixture under the lower assembly budget");
    BOOST_CHECK_EXCEPTION(materializeNativeCanonicalModel(lowSource, all, lowBudget),
      std::runtime_error, [] (const auto& error) {
        return std::string(error.what()) == "DI_NATIVE_ONNX_MATERIAL_LIMIT";
      });
    return Ort::Session(env, bytes.data(), bytes.size(), options);
  };
  const auto clip = connectVaeClip(first, next, 3, 4096);
  auto different = next;
  for (auto& material : *different.model.mutable_graph()->mutable_initializer()) {
    if (material.name() == "latent-mean") material.set_float_data(0, 0.5F);
  }
  BOOST_CHECK_THROW(connectVaeClip(first, different, 3, 4096), std::invalid_argument);
  BOOST_CHECK_THROW(connectVaeClip(first, next, 3, 1), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(clip.model.graph().input_size(), 1);
  BOOST_REQUIRE_EQUAL(clip.model.graph().output_size(), 1);
  BOOST_CHECK(clip.historySuccessorByInput.empty());
  const NativeStageLayout stage{"vae/clip", nativePlanningDigest("fixture/vae-clip"),
                                {"vae/rank0", "vae/rank1"}};
  const auto compiled = makeVaeTensorParallelPlan(clip, stage);
  const auto pipeline = makeVaePipelinePlan(clip, stages);
  BOOST_REQUIRE_EQUAL(pipeline.plan.dependencies.size(), 3);
  for (const auto& edge : pipeline.plan.dependencies) {
    BOOST_REQUIRE_EQUAL(edge.tensors.size(), 1);
    BOOST_CHECK(edge.tensors.front().find("decoder.middle.2/decoded") != std::string::npos);
  }
  const auto descriptor = ndnsf::di::fixture::completeModel({
    "tiny-wan-vae", nativePlanningDigest("vae-content"), nativePlanningDigest("vae-model"),
    nativePlanningDigest("vae-graph"), "onnx", "fp32", "wan", "1"});
  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("vae-profile"), nativePlanningDigest("vae-assembler"),
    "fixture-ort", "fp32", "none", "ncdhw", "none", "vae", 8U << 20, 16U << 20, 4096};
  for (const auto* layout : {&compiled, &pipeline}) {
    std::map<std::string, NativeRoleResourceRequirement> resources;
    for (const auto& role : layout->plan.roles)
      resources[role] = {descriptor.adapter.backends, 4U << 20, 1U << 20, 0,
                         1U << 20, 1U << 20, 1.0};
    const auto config = makeCatalogTemplate(descriptor, profile, "/fixture/vae/source",
      nativePlanningDigest("vae-manifest"), "/fixture/vae/package", "/fixture/vae", resources);
    const NativeAssemblyControl compileControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(15), [] {}, 8U << 20, 16U << 20};
    BOOST_TEST_CHECKPOINT("compile the VAE fixture catalog");
    auto package = compileWanComponent(clip.model, *layout, 32U << 20, {}, config, compileControl);
    auto loadControl = compileControl;
    // Catalog load validates the entire graph and freezes both role recipes.
    // Use the same test allowance as the split checkpoint entry checks.
    loadControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    BOOST_TEST_CHECKPOINT("load the VAE fixture catalog");
    const auto loaded = NativeRequestCatalog::load(package.configurationJson,
                                                    std::move(package.source), loadControl);
    const auto& frozen = loaded.planningCache->candidates.at(0);
    BOOST_CHECK_EQUAL(frozen.roles.size(), 2);
    BOOST_CHECK_EQUAL(frozen.candidate.executionPlan.dependencies.size(),
                      layout->plan.dependencies.size());
    BOOST_CHECK_EQUAL(frozen.candidate.resultEgressRole, layout->plan.steps.back().unitId);
    BOOST_REQUIRE_EQUAL(loaded.model.graph.modelOutputs.size(), 1);
    BOOST_CHECK_EQUAL(loaded.model.graph.modelOutputs.front().name, "rgb");
  }
  int zeros = 0;
  int historyInputs = 0;
  for (const auto& step : compiled.plan.steps) {
    if (step.operation == "CALL_SUBGRAPH") {
      const auto& cut = compiled.entries.at(step.artifactEntry);
      BOOST_CHECK(!cut.inputs.empty());
      for (const auto node : cut.nodeIndices) {
        if (clip.model.graph().node(node).name().find("/zero/") != std::string::npos) ++zeros;
      }
    }
    for (const auto& slot : step.inputSlots) {
      if (slot.slotId.find("/zero/") == std::string::npos &&
          slot.slotId.find("/next-history") == std::string::npos) {
        continue;
      }
      BOOST_CHECK_EQUAL(slot.source, "LOCAL_STEP");
      ++historyInputs;
    }
  }
  BOOST_CHECK_EQUAL(zeros, 64);
  BOOST_CHECK_EQUAL(historyInputs, 124);
  int weightCount = 0;
  for (const auto& initializer : clip.model.graph().initializer()) {
    if (initializer.name().find("/weight") == std::string::npos) continue;
    ++weightCount;
    if (initializer.name().compare(0, 12, "phase-later/") == 0)
      BOOST_CHECK(initializer.name().find("/temporal/") != std::string::npos);
  }
  BOOST_CHECK_GT(weightCount, 0);
  std::array<float, 6> latents{0, 1, 2, 0, 0, 0};
  const std::array<std::int64_t, 5> latentShape{1, 2, 3, 1, 1};
  std::map<std::string, Ort::Value> ppValues;
  ppValues.emplace("latent-clip", Ort::Value::CreateTensor<float>(
    memory, latents.data(), latents.size(), latentShape.data(), latentShape.size()));
  auto runner = prepare(clip, compiled, pipeline, ppValues);
  const auto value = Ort::Value::CreateTensor<float>(
    memory, latents.data(), latents.size(), latentShape.data(), latentShape.size());
  const char* input = "latent-clip";
  const char* output = "rgb";
  const auto out = runner.Run(Ort::RunOptions{}, &input, &value, 1, &output, 1);
  constexpr int RGB_FRAMES = 9;
  BOOST_REQUIRE(out.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 3, RGB_FRAMES, 8, 8}));
  const auto* actual = out.front().GetTensorData<float>();
  const auto& ppRgb = ppValues.at("rgb");
  BOOST_REQUIRE(ppRgb.GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 3, RGB_FRAMES, 8, 8}));
  for (int pixel = 0; pixel < 3 * RGB_FRAMES * 64; ++pixel)
    BOOST_CHECK_EQUAL(ppRgb.GetTensorData<float>()[pixel], actual[pixel]);
  const std::vector<double> referenceLatents(latents.begin(), latents.end());
  const auto expected = videoFixture::referenceVaeRgb(referenceLatents.data(), 3, 1, 1, {0, 0});
  BOOST_REQUIRE_EQUAL(expected.size(), 3U * RGB_FRAMES * 64);
  for (std::size_t pixel = 0; pixel < expected.size(); ++pixel)
    BOOST_CHECK_SMALL(double(actual[pixel]) - expected[pixel], 1e-6);

  const auto external = lowerWan21VaeClip(checkpoint, tile, {0, 0}, {1, 1}, 3, 1, 4, 4096);
  std::uint64_t decoderBytes = 0;
  for (const auto& [name, weight] : checkpoint.parameters()) {
    if (name.rfind("conv2.", 0) == 0 || name.rfind("decoder.", 0) == 0)
      decoderBytes += checkpoint.tensorSource(name)->size();
  }
  BOOST_CHECK_EQUAL(external.sidecar->size(), decoderBytes);
  const auto externalTp = makeVaeTensorParallelPlan(external.layout, stage);
  const auto externalPp = makeVaePipelinePlan(external.layout, stages);
  NativeCanonicalSource source;
  const auto wire = external.layout.model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  source.initializerRangeSource = external.sidecar;
  auto externalControl = checkpointControl;
  externalControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, externalControl);
  const auto manifestJson = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(manifestJson.begin(), manifestJson.end()));
  std::set<std::string> checkedRoles;
  for (const auto& step : externalTp.plan.steps) {
    if (step.operation != "CALL_SUBGRAPH" || checkedRoles.count(step.unitId) != 0) continue;
    const auto& entry = externalTp.entries.at(step.artifactEntry);
    if (std::none_of(entry.nodeIndices.begin(), entry.nodeIndices.end(), [&](auto node) {
          return external.layout.model.graph().node(node).op_type() == "Conv";
        })) continue;
    const auto other = step.unitId == stage.unitRoleIds.front() ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*source.materialManifest, index, entry,
      [&](const auto& initializer) { return initializer.find(other) == std::string::npos; });
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(
      selected, entry.nodeIndices, entry.inputs, entry.outputs, externalControl));
    checkedRoles.insert(step.unitId);
  }
  BOOST_CHECK_EQUAL(checkedRoles.size(), 2);
  std::map<std::string, Ort::Value> externalValues;
  externalValues.emplace("latent-clip", Ort::Value::CreateTensor<float>(
    memory, latents.data(), latents.size(), latentShape.data(), latentShape.size()));
  runCompiledEntriesFixture(externalPp, source, externalValues, env, options, memory,
                            externalControl);
  const auto& externalRgb = externalValues.at("rgb");
  BOOST_REQUIRE(externalRgb.GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>{1, 3, RGB_FRAMES, 8, 8}));
  for (std::size_t pixel = 0; pixel < expected.size(); ++pixel)
    BOOST_CHECK_SMALL(double(externalRgb.GetTensorData<float>()[pixel]) - expected[pixel], 1e-6);

  if (const auto* root = std::getenv("NDNSF_DI_WAN_CHECKPOINT_METADATA_ROOT")) {
    const std::filesystem::path directory(root);
    const auto read = [](const std::filesystem::path& path) {
      std::ifstream stream(path, std::ios::binary);
      BOOST_REQUIRE(stream.good());
      return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const std::string name = "Wan2.1_VAE.pth";
    const auto recipe = nativeParseJson(read(directory / "recipe.json"));
    const auto metadata = read(directory / (name + ".metadata.json"));
    BOOST_REQUIRE_EQUAL(nativePlanningDigest(metadata),
      "sha256:" + recipe.at("pytorch_mapped_metadata").at(name).at("metadata_sha256")
                   .get<std::string>());
    const auto weights = nativeParseJson(metadata);
    BOOST_REQUIRE_EQUAL(weights.size(), 194);
    NativeJson header = NativeJson::object();
    std::uint64_t offset = 0;
    for (const auto& [key, weight] : weights.items()) {
      BOOST_CHECK_EQUAL(weight.at("dtype").get<std::string>(), "torch.float32");
      const auto shape = weight.at("shape").get<std::vector<std::uint64_t>>();
      auto bytes = std::uint64_t{sizeof(float)};
      for (const auto dim : shape) {
        BOOST_REQUIRE(dim != 0 && dim <= std::numeric_limits<std::uint64_t>::max() / bytes);
        bytes *= dim;
      }
      BOOST_REQUIRE_LE(bytes, std::numeric_limits<std::uint64_t>::max() - offset);
      header[key] = {{"dtype", "F32"}, {"shape", shape}, {"data_offsets", {offset, offset + bytes}}};
      offset += bytes;
    }
    const auto json = nativeCanonicalJson(header);
    std::string archive(8, '\0');
    for (std::size_t i = 0; i < archive.size(); ++i)
      archive[i] = static_cast<char>((static_cast<std::uint64_t>(json.size()) >> (8 * i)) & 0xff);
    archive += json;
    const auto archiveBytes = archive.size() + offset;
    const WanCheckpoint official({
      {"vae.safetensors", std::make_shared<HeaderSource>(std::move(archive), archiveBytes)}});
    // Constant values are immaterial to the official metadata-only shape check.
    const auto clip = lowerWan21VaeClip(official, {1, 60, 104}, std::vector<float>(16, 0),
                                      std::vector<float>(16, 1), 21, 16, 32U << 20, 32768);
    BOOST_CHECK_EQUAL(clip.sidecar->size(), 293182412U);
    BOOST_CHECK_THROW(clip.sidecar->read(0, 4), std::out_of_range);
    const auto& rgb = clip.layout.model.graph().output(0).type().tensor_type().shape();
    const std::array<std::int64_t, 5> expectedRgb{1, 3, 81, 480, 832};
    BOOST_REQUIRE_EQUAL(rgb.dim_size(), expectedRgb.size());
    for (int axis = 0; axis < rgb.dim_size(); ++axis)
      BOOST_CHECK_EQUAL(rgb.dim(axis).dim_value(), expectedRgb[axis]);
    const auto tp = makeVaeTensorParallelPlan(clip.layout, stage);
    const auto pp = makeVaePipelinePlan(clip.layout, stages);
    BOOST_CHECK_EQUAL(pp.plan.dependencies.size(), 21);
    auto tpPlan = tp.plan;
    for (auto& step : tpPlan.steps) {
      if (step.operation == "CALL_SUBGRAPH")
        step.onnxNodeIndices = tp.entries.at(step.artifactEntry).nodeIndices;
    }
    const auto roleWeights = checkOfficialTpRoleWeights(
      "VAE", clip.layout.model.graph(), tpPlan, clip.sidecar->size());
    for (const auto& role : tpPlan.roles) {
      const auto& usage = roleWeights.at(role);
      BOOST_CHECK_GT(usage.entryBytes, usage.uniqueBytes);
      const auto& step = *usage.largestEntry;
      for (const auto& slot : step.inputSlots) {
        const auto dims = [&] {
          std::ostringstream encoded;
          for (const auto& dim : slot.shape) encoded << ' ' << std::get<std::int64_t>(dim);
          return encoded.str();
        }();
        BOOST_TEST_MESSAGE("Largest VAE entry input " << slot.slotId << " shape=" << dims);
      }
      for (auto index : step.onnxNodeIndices) {
        const auto& node = clip.layout.model.graph().node(static_cast<int>(index));
        if (node.op_type() != "Conv") continue;
        const auto& initializers = clip.layout.model.graph().initializer();
        const auto weight = std::find_if(initializers.begin(), initializers.end(),
          [&](const auto& tensor) { return tensor.name() == node.input(1); });
        BOOST_REQUIRE(weight != initializers.end());
        std::ostringstream dims;
        for (auto extent : weight->dims()) dims << ' ' << extent;
        BOOST_TEST_MESSAGE("Largest VAE Conv " << node.name() << " kernel=" << dims.str());
      }
    }
    if (const auto* probe = std::getenv("NDNSF_DI_WAN_VAE_CONV_PROBE")) {
      auto stepId = roleWeights.at(tpPlan.roles.front()).largestEntry->stepId;
      const auto firstChunk = stepId.find("chunk-0/");
      BOOST_REQUIRE(firstChunk != std::string::npos);
      stepId.replace(firstChunk, std::string_view("chunk-0/").size(), "chunk-20/");
      const auto step = std::find_if(tpPlan.steps.begin(), tpPlan.steps.end(),
        [&](const auto& candidate) { return candidate.stepId == stepId; });
      BOOST_REQUIRE(step != tpPlan.steps.end());
      probeOfficialVaeConv(clip.layout.model, clip.sidecar->size(), *step,
                           tp.entries.at(step->artifactEntry), probe);
    }
    const auto* vaeProjectionPath = std::getenv("NDNSF_DI_WAN_VAE_PROJECTION_PAYLOAD_OUT");
    if (vaeProjectionPath || std::getenv("NDNSF_DI_WAN_VAE_TILED_META")) {
      constexpr std::uint64_t TILE_BYTES = 56U << 20;
      constexpr std::uint64_t LIMIT = 512U << 20;
      const auto compileStarted = std::chrono::steady_clock::now();
      const NativeAssemblyControl control{
        compileStarted + std::chrono::seconds(120), [] {}, LIMIT, LIMIT};
      const auto hash = [](const std::string& value) { return nativePlanningDigest(value); };
      const auto descriptor = ndnsf::di::fixture::completeModel({
        "official-vae-geometry", hash("shape-fixture"), hash("vae-model"),
        hash("vae-graph"), "onnx", "fp32", "wan", "1"});
      const NativeRoleRecipeProfile profile{hash("vae-role"), hash("vae-assembler"),
        "fixture-ort", "fp32", "none", "native", "right", "vae", LIMIT, LIMIT, 65536};
      std::map<std::string, NativeRoleResourceRequirement> resources;
      for (const auto& role : tp.plan.roles)
        resources.emplace(role, NativeRoleResourceRequirement{
          descriptor.adapter.backends, 256U << 20, LIMIT, 0, LIMIT, LIMIT, 1.0});
      auto catalog = makeCatalogTemplate(descriptor, profile, "/fixture/vae/source",
        hash("vae-manifest"), "/fixture/vae/package", "/fixture/vae", resources);
      NativeCanonicalSource source;
      source.initializerRangeSource =
        std::make_shared<ZeroInitializerSource>(clip.sidecar->size(), "Official VAE");
      auto compiled = compileWanComponent(clip.layout.model, tp, TILE_BYTES, std::move(source),
                                          std::move(catalog), control);
      const auto compileSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - compileStarted).count();
      const auto configuration = nativeParseJson(compiled.configurationJson);
      const auto& split = configuration.at("splitter");
      const auto plan = nativeCanonicalOnnxStepPlanFromJson(
        split.at("step_template_v2"), split.at("dependencies"));
      const auto steps = nativeCanonicalJson(split.at("step_template_v2"));
      const auto dependencies = nativeCanonicalJson(split.at("dependencies"));
      if (vaeProjectionPath) {
        std::ofstream output(vaeProjectionPath);
        output << R"({"steps":)" << steps << R"(,"dependencies":)" << dependencies << "}\n";
        output.close();
        BOOST_REQUIRE(output.good());
      }
      std::map<std::pair<std::string, std::string>, const NativeStepSlotSpec*> outputs;
      for (const auto& step : plan.steps) {
        for (const auto& slot : step.outputSlots)
          outputs.emplace(std::make_pair(step.stepId, slot.slotId), &slot);
      }
      std::map<std::string, std::uint64_t> sent;
      std::map<std::pair<std::string, std::string>, std::uint64_t> sentByOperation;
      std::set<std::tuple<std::string, std::string, std::string>> transfers;
      std::uint64_t repeatedBytes = 0;
      std::size_t repeatedTransfers = 0;
      for (const auto& edge : plan.dependencies) {
        const auto& slot = *outputs.at({edge.producerStepId, edge.producerSlotId});
        BOOST_REQUIRE_EQUAL(slot.dtype, "float32");
        std::uint64_t bytes = sizeof(float);
        for (const auto& dim : slot.shape) bytes *= std::get<std::int64_t>(dim);
        const auto bundle = tensorBundleEncodedSize({{slot.slotId.size(), slot.shape.size(), bytes}});
        BOOST_CHECK_LE(bundle, TILE_BYTES);
        sent[edge.producers.front()] += bundle;
        sentByOperation[{edge.producers.front(), edge.operationKind}] += bundle;
        if (!transfers.emplace(edge.producerStepId, edge.producerSlotId,
                               edge.consumers.front()).second) {
          repeatedBytes += bundle;
          ++repeatedTransfers;
        }
      }
      BOOST_TEST_MESSAGE("Official tiled VAE steps=" << plan.steps.size()
        << " dependencies=" << plan.dependencies.size() << " steps_bytes=" << steps.size()
        << " dependencies_bytes=" << dependencies.size()
        << " catalog_bytes=" << compiled.configurationJson.size()
        << " compile_s=" << compileSeconds);
      for (const auto& [role, bytes] : sent)
        BOOST_TEST_MESSAGE("Official tiled VAE role=" << role << " sent_bytes=" << bytes);
      for (const auto& [coordinate, bytes] : sentByOperation)
        BOOST_TEST_MESSAGE("Official tiled VAE role=" << coordinate.first
          << " operation=" << coordinate.second << " sent_bytes=" << bytes);
      BOOST_TEST_MESSAGE("Official tiled VAE repeated_transfers=" << repeatedTransfers
        << " repeated_bytes=" << repeatedBytes);
      if (!std::getenv("NDNSF_DI_WAN_VAE_TILED_META"))
        return;

      // The two-minute probe reached the native preparation deadline. Time the
      // two stages independently on this metadata-only qualification.
      const auto loadStarted = std::chrono::steady_clock::now();
      const auto loadDeadline = loadStarted + std::chrono::minutes(10);
      const NativeAssemblyControl loadControl{loadDeadline, [&] {
        if (std::chrono::steady_clock::now() >= loadDeadline)
          throw std::runtime_error("official VAE preparation diagnostic deadline expired");
      }, LIMIT, LIMIT};
      const auto loaded = NativeRequestCatalog::load(compiled.configurationJson,
        std::move(compiled.source), loadControl);
      BOOST_REQUIRE(loaded.preparation && loaded.planningCache);
      BOOST_REQUIRE_EQUAL(loaded.planningCache->candidates.at(0).roles.size(), 2);
      const auto stored = loaded.preparation->sourceRefFor(loaded.model.descriptor);
      BOOST_REQUIRE(stored && stored->materialManifest);
      BOOST_TEST_MESSAGE("Official VAE prepared_metadata_bytes=" << stored->preparedMetadataJson.size()
        << " material_manifest_bytes=" << stored->materialManifest->canonicalJson().size()
        << " load_s=" << std::chrono::duration<double>(
          std::chrono::steady_clock::now() - loadStarted).count());
      ndn::KeyChain keys("pib-memory:", "tpm-memory:");
      const ndn::Name producer("/ndnsf-test/wan/requester");
      struct FirstCommit {};
      NativeStoredModelPacketIo io;
      io.requireCurrent = loadControl.requireActive;
      io.commit = [](auto wires, const std::string&) {
        BOOST_REQUIRE(!wires.empty());
        throw FirstCommit{};
      };
      BOOST_CHECK_THROW(nativePublishStoredPreparedModel(keys, producer, ndn::Name("/fixture/repo"),
        ndn::Name(producer).append("vae-preflight").appendVersion(1), loaded,
        compiled.configurationJson, io), FirstCommit);
    }
    BOOST_TEST_MESSAGE("Official VAE graph bytes " << clip.layout.model.ByteSizeLong()
                       << ", TP steps " << tp.plan.steps.size()
                       << ", TP dependencies " << tp.plan.dependencies.size());
  }
}

BOOST_AUTO_TEST_CASE(VaeResidualSeparatesBothHistoriesAndProjectsShortcut)
{
  const auto tensor = [] (std::initializer_list<int> dims, int seed) {
    onnx::TensorProto result;
    result.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto dim : dims) {
      result.add_dims(dim);
      count *= dim;
    }
    for (int i = 0; i < count; ++i) result.add_float_data(((i + seed) % 9 - 4) / 8.0F);
    return result;
  };
  const VaeConvTile tile{1, 1, 1};
  const auto g1 = tensor({2}, 1), g2 = tensor({3}, 5);
  const auto w1 = tensor({3, 2, 3, 1, 1}, 2), w2 = tensor({3, 3, 3, 1, 1}, 3);
  const auto b1 = tensor({3}, 3), b2 = tensor({3}, 8);
  const auto ws = tensor({3, 2, 1, 1, 1}, 4), bs = tensor({3}, 5);
  const auto n1 = lowerVaeRmsSilu(tile, g1), n2 = lowerVaeRmsSilu(tile, g2);
  const auto c1 = lowerVaeCausalConv(tile, w1, b1), c2 = lowerVaeCausalConv(tile, w2, b2);
  const auto shortcut = lowerVaeCausalConv(tile, ws, bs);
  BOOST_CHECK_THROW(connectVaeResidual(n1, c1, n2, c2), std::invalid_argument);
  const auto residual = connectVaeResidual(n1, c1, n2, c2, shortcut);
  BOOST_REQUIRE_EQUAL(residual.historySuccessorByInput.size(), 4);
  for (std::size_t layer = 1; layer <= 2; ++layer) {
    const auto norm = "norm" + std::to_string(layer) + "/";
    const auto conv = "conv" + std::to_string(layer) + "/";
    BOOST_CHECK_EQUAL(residual.entries.count(norm + "join"), 0);
    const auto sliceId = norm + "rank-1/input-slice";
    const auto& slice = residual.entries.at(sliceId);
    const auto& peerStats = residual.entries.at(norm + "rank-1/statistics");
    BOOST_CHECK_EQUAL(residual.rankOneEntries.count(sliceId), 0);
    BOOST_CHECK_EQUAL(peerStats.inputs.at(0).name, slice.outputs.at(0).name);
    BOOST_CHECK_EQUAL(std::get<std::int64_t>(slice.outputs.at(0).shape.at(1)), 1);
    for (std::size_t rank = 0; rank < 2; ++rank) {
      const auto id = "rank-" + std::to_string(rank);
      const auto& activated = residual.entries.at(norm + id + "/normalize").outputs.at(0);
      const auto& current = residual.entries.at(conv + id).inputs.at(0);
      BOOST_CHECK_EQUAL(current.name, activated.name);
      BOOST_CHECK(current.shape == activated.shape);
    }
  }

  const auto wire = residual.model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 2U << 20, 2U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  const auto json = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  std::set<std::uint64_t> covered;
  std::map<std::string, std::vector<float>> historyValues;
  std::map<std::string, std::vector<std::int64_t>> historyShapes;
  for (const auto& [id, entry] : residual.entries) {
    for (const auto n : entry.nodeIndices) BOOST_CHECK(covered.insert(n).second);
    const auto rank = id.find("rank-");
    const std::string other = id.find("rank-0") != std::string::npos ? "rank-1/" : "rank-0/";
    auto selection = selectCanonicalEntry(*source.materialManifest, index, entry,
      [&] (const auto& name) { return rank == std::string::npos || name.find(other) == std::string::npos; });
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(selection, entry.nodeIndices, entry.inputs,
                          entry.outputs, control));
    for (const auto& port : entry.inputs) {
      if (residual.historySuccessorByInput.count(port.name) == 0) continue;
      auto& shape = historyShapes[port.name];
      std::size_t count = 1;
      for (const auto& dim : port.shape) {
        shape.push_back(std::get<std::int64_t>(dim));
        count *= static_cast<std::size_t>(shape.back());
      }
      historyValues[port.name].resize(count);
    }
  }
  BOOST_CHECK_EQUAL(covered.size() + 5, static_cast<std::size_t>(residual.model.graph().node_size()));
  BOOST_REQUIRE_EQUAL(historyValues.size(), 4);
  source.materialPayloads = source.materialManifest->payloads;
  std::vector<std::uint64_t> nodes;
  for (int i = 0; i < residual.model.graph().node_size(); ++i) nodes.push_back(i);
  const auto bytes = materializeNativeCanonicalModel(source, nodes, control);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-residual");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session runner(env, bytes.data(), bytes.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::array<std::array<float, 2>, 3> input{{{1e-14F, -2e-14F}, {0, 0}, {2, -1}}};
  const auto rms = [] (const auto& x, const auto& gain) {
    std::vector<double> out;
    double squares = 0;
    for (const auto value : x) squares += double(value) * value;
    for (std::size_t channel = 0; channel < x.size(); ++channel) {
      const auto v = x[channel] / std::max(std::sqrt(squares), 1e-12) *
        std::sqrt(x.size()) * gain.float_data(channel);
      out.push_back(v / (1 + std::exp(-v)));
    }
    return out;
  };
  const auto convolve = [] (const auto& history, int frame, const auto& w, const auto& b) {
    std::vector<double> out(b.float_data().begin(), b.float_data().end());
    const int width = w.dims(1), kernel = w.dims(2);
    for (int to = 0; to < b.dims(0); ++to) {
      for (int channel = 0; channel < width; ++channel) {
        for (int k = 0; k < kernel; ++k) {
          const int from = frame + k - kernel + 1;
          if (from >= 0) out[to] += history[from][channel] *
            w.float_data((to * width + channel) * kernel + k);
        }
      }
    }
    return out;
  };
  std::vector<std::vector<double>> normalized1, normalized2;
  constexpr std::array<std::int64_t, 5> inputShape{1, 2, 1, 1, 1};
  for (std::size_t frame = 0; frame < input.size(); ++frame) {
    normalized1.push_back(rms(input[frame], g1));
    normalized2.push_back(rms(convolve(normalized1, frame, w1, b1), g2));
    const auto main = convolve(normalized2, frame, w2, b2);
    const auto bypass = convolve(input, frame, ws, bs);
    std::vector<const char*> inputs{"frames"}, outputs{"decoded"};
    std::vector<Ort::Value> args;
    args.push_back(Ort::Value::CreateTensor<float>(memory, input[frame].data(), input[frame].size(),
                                                   inputShape.data(), inputShape.size()));
    for (auto& [name, history] : historyValues) {
      const auto& dims = historyShapes.at(name);
      args.push_back(Ort::Value::CreateTensor<float>(memory, history.data(), history.size(),
                                                     dims.data(), dims.size()));
      inputs.push_back(name.c_str());
      outputs.push_back(residual.historySuccessorByInput.at(name).c_str());
    }
    const auto result = runner.Run(Ort::RunOptions{}, inputs.data(), args.data(),
                                    args.size(), outputs.data(), outputs.size());
    BOOST_REQUIRE_EQUAL(result.front().GetTensorTypeAndShapeInfo().GetElementCount(), 3);
    for (std::size_t c = 0; c < main.size(); ++c)
      BOOST_CHECK_SMALL(result.front().GetTensorData<float>()[c] - main[c] - bypass[c], 2e-6);
    std::size_t i = 1;
    for (auto& [name, history] : historyValues) {
      const auto* next = result[i++].GetTensorData<float>();
      const auto& reference = name.rfind("conv1/", 0) == 0 ? normalized1 : normalized2;
      const auto channelStart = name.find("rank-0") != std::string::npos ? 0U :
                                (reference.front().size() + 1) / 2;
      for (std::size_t c = 0; c < history.size() / 2; ++c) {
        const auto old = frame == 0 ? 0 : reference[frame - 1][channelStart + c];
        BOOST_CHECK_SMALL(next[c * 2] - old, 2e-6);
        BOOST_CHECK_SMALL(next[c * 2 + 1] - reference[frame][channelStart + c], 2e-6);
      }
      std::copy_n(next, history.size(), history.begin());
    }
  }
}

BOOST_AUTO_TEST_CASE(VaeSpatialAttentionMasksPaddingAndKeepsFramesIndependent)
{
  const auto tensor = [] (std::initializer_list<int> dims, int seed) {
    onnx::TensorProto result;
    result.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto dim : dims) {
      result.add_dims(dim);
      count *= dim;
    }
    for (int i = 0; i < count; ++i)
      result.add_float_data(((i * seed + seed) % 17 - 8) / 8.0F);
    return result;
  };
  const auto gain = tensor({3}, 7);
  const auto wqkv = tensor({9, 3, 1, 1, 1}, 3), bqkv = tensor({9}, 5);
  const auto wo = tensor({3, 3, 1, 1, 1}, 11), bo = tensor({3}, 13);
  const VaeAttentionParameters parameters{gain, wqkv, bqkv, wo, bo};
  const VaeConvTile tile{2, 1, 3};
  const auto layout = lowerVaeAttention(tile, parameters, 2, 48);
  BOOST_CHECK_THROW(lowerVaeAttention(tile, parameters, 2, 47), std::invalid_argument);
  BOOST_CHECK(layout.historySuccessorByInput.empty());

  const auto wire = layout.model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 2U << 20, 2U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  BOOST_REQUIRE(source.materialManifest);
  const auto json = source.materialManifest->canonicalJson();
  const auto index = parseNativeCanonicalMaterialManifest(
    std::vector<std::uint8_t>(json.begin(), json.end()));
  std::set<std::uint64_t> covered;
  for (const auto& [id, entry] : layout.entries) {
    const auto other = id.find("rank-0") != std::string::npos ? "rank-1/" : "rank-0/";
    auto selected = selectCanonicalEntry(*source.materialManifest, index, entry,
      [&] (const auto& name) {
        return id.find("rank-") == std::string::npos || name.find(other) == std::string::npos;
      });
    for (const auto node : entry.nodeIndices) BOOST_CHECK(covered.insert(node).second);
    BOOST_CHECK_NO_THROW(materializeNativeCanonicalModel(selected, entry.nodeIndices,
                          entry.inputs, entry.outputs, control));
  }
  BOOST_CHECK_EQUAL(covered.size() + 3, static_cast<std::size_t>(layout.model.graph().node_size()));
  std::vector<std::uint64_t> all;
  for (int i = 0; i < layout.model.graph().node_size(); ++i) all.push_back(i);
  source.materialPayloads = source.materialManifest->payloads;
  const auto bytes = materializeNativeCanonicalModel(source, all, control);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan-vae-spatial-attention");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  Ort::Session session(env, bytes.data(), bytes.size(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<float, 18> data{1, 2, -3, 0.5, 4, 5, 4, 0, 2, 1, -2, -4, -2, 1, 3, 2, 7, -1};
  constexpr std::array<std::int64_t, 5> shape{1, 3, 2, 1, 3};
  const auto arg = Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                                    shape.data(), shape.size());
  const char* input = "frames";
  const char* output = "decoded";
  const auto result = session.Run(Ort::RunOptions{}, &input, &arg, 1, &output, 1);
  BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
                (std::vector<std::int64_t>(shape.begin(), shape.end())));
  const auto* actual = result.front().GetTensorData<float>();
  for (int frame = 0; frame < 2; ++frame) {
    std::array<std::array<double, 9>, 3> qkv{};
    for (int pixel = 0; pixel < 3; ++pixel) {
      std::array<double, 3> normalized{};
      double square = 0;
      for (int c = 0; c < 3; ++c) {
        normalized[c] = data[c * 6 + frame * 3 + pixel];
        square += normalized[c] * normalized[c];
      }
      for (int c = 0; c < 3; ++c)
        normalized[c] *= gain.float_data(c) * std::sqrt(3.0) / std::sqrt(square);
      for (int to = 0; to < 9; ++to) {
        qkv[pixel][to] = bqkv.float_data(to);
        for (int c = 0; c < 3; ++c) qkv[pixel][to] += normalized[c] * wqkv.float_data(to * 3 + c);
      }
    }
    for (int query = 0; query < 3; ++query) {
      std::array<double, 3> probabilities{};
      double total = 0;
      for (int key = 0; key < 3; ++key) {
        double dot = 0;
        for (int c = 0; c < 3; ++c) dot += qkv[query][c] * qkv[key][3 + c];
        probabilities[key] = std::exp(dot / std::sqrt(3.0));
        total += probabilities[key];
      }
      for (int out = 0; out < 3; ++out) {
        const auto i = out * 6 + frame * 3 + query;
        double expected = data[i] + bo.float_data(out);
        for (int key = 0; key < 3; ++key) {
          for (int c = 0; c < 3; ++c) {
            expected += probabilities[key] * qkv[key][6 + c] * wo.float_data(out * 3 + c) /
                        total;
          }
        }
        BOOST_CHECK_SMALL(actual[i] - expected, 2e-6);
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(Wan22CheckpointDecoderBindsGroupedUpsamplesAndPackedHead)
{
  std::map<std::string, onnx::TensorProto> weights;
  const auto tensor = [&](const std::string& name, std::initializer_list<std::int64_t> dims) {
    auto& value = weights[name];
    value.set_name(name);
    value.set_data_type(onnx::TensorProto::FLOAT);
    std::size_t count = 1;
    for (auto dim : dims) {
      value.add_dims(dim);
      count *= dim;
    }
    for (std::size_t i = 0; i < count; ++i) value.add_float_data(0);
  };
  const auto conv = [&](const std::string& name, auto out, auto in, int t, int space) {
    if (t) tensor(name + ".weight", {out, in, t, space, space});
    else tensor(name + ".weight", {out, in, space, space});
    tensor(name + ".bias", {out});
  };
  const auto residual = [&](const std::string& name, std::int64_t in, std::int64_t out) {
    tensor(name + ".residual.0.gamma", {in, 1, 1, 1});
    tensor(name + ".residual.3.gamma", {out, 1, 1, 1});
    conv(name + ".residual.2", out, in, 3, 3);
    conv(name + ".residual.6", out, out, 3, 3);
    if (in != out) conv(name + ".shortcut", out, in, 1, 1);
  };
  conv("conv2", 2, 2, 1, 1);
  conv("decoder.conv1", 8, 2, 3, 3);
  residual("decoder.middle.0", 8, 8);
  tensor("decoder.middle.1.norm.gamma", {8, 1, 1});
  conv("decoder.middle.1.to_qkv", 24, 8, 0, 1);
  conv("decoder.middle.1.proj", 8, 8, 0, 1);
  residual("decoder.middle.2", 8, 8);
  std::int64_t channels = 8;
  const std::array<std::int64_t, 4> widths{8, 8, 4, 2};
  for (std::size_t scale = 0; scale < widths.size(); ++scale) {
    const auto prefix = "decoder.upsamples." + std::to_string(scale) + ".upsamples.";
    for (int block = 0; block < 3; ++block) {
      residual(prefix + std::to_string(block), channels, widths[scale]);
      channels = widths[scale];
    }
    if (scale == 3) break;
    conv(prefix + "3.resample.1", channels, channels, 0, 3);
    if (scale < 2) conv(prefix + "3.time_conv", 2 * channels, channels, 3, 1);
  }
  tensor("decoder.head.0.gamma", {2, 1, 1, 1});
  conv("decoder.head.2", 12, 2, 3, 3);
  for (int i = 0; i < 12; ++i)
    weights.at("decoder.head.2.bias").set_float_data(i, (i - 6) / 16.0F);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan22-checkpoint-decoder");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  for (bool first : {true, false}) {
    const auto model = lowerWan22VaeDecoder({1, 1, 1}, weights, {0, 0}, {1, 1}, first, 1, 4);
    const auto wire = model.model.SerializeAsString();
    Ort::Session session(env, wire.data(), wire.size(), options);
    std::vector<std::vector<float>> data;
    std::vector<Ort::Value> inputs;
    std::vector<const char*> names;
    data.reserve(session.GetInputCount());
    for (int i = 0; i < model.model.graph().input_size(); ++i) {
      const auto type = session.GetInputTypeInfo(i);
      const auto info = type.GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      data.emplace_back(info.GetElementCount(), 0);
      inputs.push_back(Ort::Value::CreateTensor<float>(memory, data.back().data(),
        data.back().size(), shape.data(), shape.size()));
      names.push_back(model.model.graph().input(i).name().c_str());
    }
    const char* output = "rgb";
    const auto result = session.Run(Ort::RunOptions{}, names.data(), inputs.data(),
                                    inputs.size(), &output, 1);
    const std::int64_t frames = first ? 1 : 4;
    BOOST_REQUIRE(result.front().GetTensorTypeAndShapeInfo().GetShape() ==
      (std::vector<std::int64_t>{1, 3, frames, 16, 16}));
    const auto* rgb = result.front().GetTensorData<float>();
    for (int c = 0; c < 3; ++c) {
      for (int t = 0; t < frames; ++t) {
        for (int y = 0; y < 16; ++y) {
          for (int x = 0; x < 16; ++x)
            BOOST_CHECK_EQUAL(rgb[((c * frames + t) * 16 + y) * 16 + x],
              weights.at("decoder.head.2.bias").float_data(c * 4 + x % 2 * 2 + y % 2));
        }
      }
    }
    BOOST_CHECK(!makeVaeTensorParallelPlan(model,
      {"vae22", {}, {"rank0", "rank1"}}).plan.steps.empty());
  }
  auto scratch = ndnsf::test::makeTestScratchTemplate("wan22-vae-checkpoint");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&scratch) {
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
  };
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(90), [] {}, 64U << 20, 64U << 20};
  NativeJson header = NativeJson::object();
  std::vector<float> payload;
  for (const auto& [name, weight] : weights) {
    const auto begin = payload.size() * sizeof(float);
    payload.insert(payload.end(), weight.float_data().begin(), weight.float_data().end());
    header[name] = {{"dtype", "F32"},
      {"shape", std::vector<std::int64_t>(weight.dims().begin(), weight.dims().end())},
      {"data_offsets", {begin, payload.size() * sizeof(float)}}};
  }
  const auto path = std::filesystem::path(scratch) / "decoder.safetensors";
  const WanCheckpoint checkpoint({{"decoder", writeFixtureSafeTensors(
    path, nativeCanonicalJson(header), payload, control)}});
  const auto clip = lowerWan22VaeClip(checkpoint, {1, 1, 1}, {0, 0}, {1, 1}, 3, 1, 4, 32768);
  const std::array<NativeStageLayout, 2> stages{{
    {"front", {}, {"rank0"}}, {"back", {}, {"rank1"}}}};
  const auto pipeline = makeVaePipelinePlan(clip.layout, stages);
  NativeCanonicalSource source;
  const auto wire = clip.layout.model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  source.initializerRangeSource = clip.sidecar;
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  std::vector<float> latent(6, 0);
  const std::array<std::int64_t, 5> shape{1, 2, 3, 1, 1};
  std::map<std::string, Ort::Value> values;
  values.emplace("latent-clip", Ort::Value::CreateTensor<float>(memory,
    latent.data(), latent.size(), shape.data(), shape.size()));
  runCompiledEntriesFixture(pipeline, source, values, env, options, memory, control);
  const auto& rgb = values.at("rgb");
  BOOST_REQUIRE(rgb.GetTensorTypeAndShapeInfo().GetShape() ==
    (std::vector<std::int64_t>{1, 3, 9, 16, 16}));
  for (int c = 0; c < 3; ++c) {
    for (int t = 0; t < 9; ++t) {
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x)
          BOOST_CHECK_EQUAL(rgb.GetTensorData<float>()[((c * 9 + t) * 16 + y) * 16 + x],
            weights.at("decoder.head.2.bias").float_data(c * 4 + x % 2 * 2 + y % 2));
      }
    }
  }

  const std::vector<std::vector<NativeStageLayout>> layouts{
    {{"vae22", {}, {"rank0", "rank1"}}}, {stages[0], stages[1]},
    {{"baseline", {}, {"provider"}}}};
  for (std::size_t i = 0; i < layouts.size(); ++i) {
    auto prepareControl = control;
    prepareControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    const auto output = path.parent_path() / ("prepared-" + std::to_string(i));
    const auto catalog = prepareWanCheckpointComponent(
      offlineCheckpointRequest("vae", path, layouts[i],
        {{"height", 1}, {"width", 1}, {"latent_frames", 3}, {"key_tile", 1},
         {"score_bytes", 4}, {"latent_mean", {0, 0}}, {"latent_inverse_std", {1, 1}}},
        prepareControl), path.parent_path(), output, prepareControl);
    BOOST_CHECK(std::filesystem::is_regular_file(catalog));
    onnx::ModelProto graph;
    std::ifstream graphFile(output / "model.onnx", std::ios::binary);
    BOOST_REQUIRE(graph.ParseFromIstream(&graphFile));
    BOOST_REQUIRE_EQUAL(graph.graph().output_size(), 1);
    const auto& shape = graph.graph().output(0).type().tensor_type().shape();
    BOOST_REQUIRE_EQUAL(shape.dim_size(), 5);
    BOOST_CHECK_EQUAL(shape.dim(2).dim_value(), 9);
    BOOST_CHECK_EQUAL(shape.dim(3).dim_value(), 16);
    BOOST_CHECK_EQUAL(shape.dim(4).dim_value(), 16);
    std::ifstream catalogFile(catalog, std::ios::binary);
    const std::string catalogBytes(std::istreambuf_iterator<char>(catalogFile), {});
    const auto preparedCatalog = nativeParseJson(catalogBytes);
    const auto& facts = preparedCatalog.at("preparation_facts");
    BOOST_REQUIRE(facts.at("schema") == "ndnsf-di-wan-preparation-facts-v1");
    BOOST_CHECK(facts.at("source_digest") == preparedCatalog.at("source").at("digest"));
    BOOST_CHECK(facts.at("initializer_digest") ==
                preparedCatalog.at("source").at("initializer_digest"));
    NativeCanonicalSource reopened;
    const auto graphBytes = graph.SerializeAsString();
    reopened.modelBytes.assign(graphBytes.begin(), graphBytes.end());
    const auto& initializer = preparedCatalog.at("source");
    reopened.initializerRangeSource = openNativeCanonicalInitializerFile(
      (output / initializer.at("initializer_file").get<std::string>()).string(),
      initializer.at("initializer_bytes").get<std::uint64_t>(),
      initializer.at("initializer_digest").get<std::string>(), prepareControl);
    const auto admitted = NativeRequestCatalog::load(catalogBytes, reopened, prepareControl);
    BOOST_REQUIRE(admitted.planningCache);
    BOOST_REQUIRE_EQUAL(admitted.planningCache->candidates.size(), 1);
    const auto& candidate = admitted.planningCache->candidates.front().candidate;
    BOOST_CHECK(facts.at("candidate_digest") == candidate.candidateDigest);
    BOOST_REQUIRE_EQUAL(candidate.executionPlan.stageLayout.size(), layouts[i].size());
    for (std::size_t stage = 0; stage < layouts[i].size(); ++stage) {
      BOOST_CHECK(candidate.executionPlan.stageLayout[stage].unitRoleIds ==
                  layouts[i][stage].unitRoleIds);
    }
    if (layouts[i].size() == 1 && layouts[i].front().degree() == 1) {
      Ort::Session baseline(env, (output / "model.onnx").c_str(), options);
      BOOST_REQUIRE_EQUAL(baseline.GetInputCount(), 1);
      const char* inputName = "latent-clip";
      const char* outputName = "rgb";
      const std::array<std::int64_t, 5> latentDims{1, 2, 3, 1, 1};
      auto inputValue = Ort::Value::CreateTensor<float>(memory, latent.data(), latent.size(),
                                                       latentDims.data(), latentDims.size());
      const auto singleResult = baseline.Run(Ort::RunOptions{}, &inputName, &inputValue,
                                             1, &outputName, 1);
      BOOST_REQUIRE_EQUAL(singleResult.front().GetTensorSizeInBytes(), rgb.GetTensorSizeInBytes());
      const auto count = singleResult.front().GetTensorTypeAndShapeInfo().GetElementCount();
      BOOST_CHECK_EQUAL_COLLECTIONS(singleResult.front().GetTensorData<float>(),
        singleResult.front().GetTensorData<float>() + count,
        rgb.GetTensorData<float>(), rgb.GetTensorData<float>() + count);
    }
  }

}

BOOST_AUTO_TEST_CASE(Wan22UpsampleShortcutPreservesChannelAndFrameOrder)
{
  const auto zeros = [](std::initializer_list<std::int64_t> dims) {
    onnx::TensorProto tensor;
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    std::size_t count = 1;
    for (auto dim : dims) {
      tensor.add_dims(dim);
      count *= dim;
    }
    for (std::size_t i = 0; i < count; ++i) tensor.add_float_data(0);
    return tensor;
  };
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "wan22-vae-shortcut");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  for (bool temporal : {false, true}) {
    for (bool first : {false, true}) {
      for (std::int64_t outputChannels : {1, 2}) {
        const VaeConvTile tile{first ? 1 : 2, 2, 3};
        const auto spatial = zeros({outputChannels, 2, 1, 3, 3});
        const auto spatialBias = zeros({outputChannels});
        const auto time = zeros({4, 2, 3, 1, 1});
        const auto timeBias = zeros({4});
        std::optional<VaeConvolutionParameters> timeParams;
        if (temporal) timeParams.emplace(VaeConvolutionParameters{time, timeBias});
        auto main = lowerVaeResample(tile, {spatial, spatialBias}, timeParams, first);
        auto layout = connectVaeUpsampleResidual(std::move(main), tile, 2,
                                                outputChannels, temporal, first);
        const auto bytes = layout.model.SerializeAsString();
        Ort::Session session(env, bytes.data(), bytes.size(), options);
        std::vector<std::vector<float>> buffers;
        std::vector<Ort::Value> values;
        std::vector<const char*> names;
        buffers.reserve(session.GetInputCount());
        for (int i = 0; i < layout.model.graph().input_size(); ++i) {
          const auto type = session.GetInputTypeInfo(i);
          const auto info = type.GetTensorTypeAndShapeInfo();
          const auto shape = info.GetShape();
          buffers.emplace_back(info.GetElementCount(), 0);
          const auto& name = layout.model.graph().input(i).name();
          if (name == "frames") {
            for (std::size_t j = 0; j < buffers.back().size(); ++j)
              buffers.back()[j] = static_cast<float>(j + 1);
          }
          names.push_back(name.c_str());
          values.push_back(Ort::Value::CreateTensor<float>(memory, buffers.back().data(),
            buffers.back().size(), shape.data(), shape.size()));
        }
        const char* output = "decoded";
        const auto actual = session.Run(Ort::RunOptions{}, names.data(), values.data(),
                                        values.size(), &output, 1);
        const std::int64_t factor = temporal ? 2 : 1;
        const auto frames = factor * tile.frames - (first ? factor - 1 : 0);
        BOOST_REQUIRE(actual.front().GetTensorTypeAndShapeInfo().GetShape() ==
          (std::vector<std::int64_t>{1, outputChannels, frames, 4, 6}));
        const auto* data = actual.front().GetTensorData<float>();
        const auto repeats = outputChannels * factor * 4 / 2;
        for (std::int64_t c = 0; c < outputChannels; ++c) {
          for (std::int64_t t = 0; t < frames; ++t) {
            const auto expanded = t + (first ? factor - 1 : 0);
            for (std::int64_t y = 0; y < 4; ++y) {
              for (std::int64_t x = 0; x < 6; ++x) {
                const auto channel = (c * factor * 4 + expanded % factor * 4 +
                                      y % 2 * 2 + x % 2) / repeats;
                const auto expected = ((channel * tile.frames + expanded / factor) * 2 +
                                       y / 2) * 3 + x / 2 + 1;
                BOOST_CHECK_EQUAL(data[((c * frames + t) * 4 + y) * 6 + x], expected);
              }
            }
          }
        }
        BOOST_CHECK(!makeVaeTensorParallelPlan(layout,
          {"upsample", {}, {"rank0", "rank1"}}).plan.steps.empty());
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(GenerationTraceBudgetUsesEncodedBoundsWithoutModelWeights)
{
  WanGenerationOptions options;
  options.latentShape = {2, 3, 4, 4};
  options.textWidth = 2;
  options.steps = 3;
  const auto encoded = [] (const std::string& name, const std::vector<std::int64_t>& shape) {
    std::size_t cells = 1;
    for (const auto extent : shape) cells *= extent;
    const std::vector<std::uint8_t> bytes(cells * sizeof(float));
    return encodeTensorBundleViews({{name, TensorElementType::Float32, shape, bytes}}).size();
  };
  const auto rgb = encoded("rgb", {1, 3, 9, 32, 32});
  const auto prediction = encoded("prediction", {1, 2, 3, 4, 4});
  const auto expected = rgb + encoded("text-context", {1, 3, 2}) +
    encoded("text-context", {1, 2, 2}) + 9 * prediction;
  BOOST_CHECK_EQUAL(planWanGenerationTraceBytes(options, 3, 2), expected);
  options.traceMaxBytes = expected;
  BOOST_CHECK_EQUAL(planWanGenerationTraceBytes(options, 3, 2), expected);
  --options.traceMaxBytes;
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 3, 2), std::invalid_argument);
  options.traceMaxBytes = 0;
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 3, 2), std::invalid_argument);
  options.traceMaxBytes = WAN_TRACE_HARD_MAX_BYTES + 1;
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 3, 2), std::invalid_argument);
  options.traceMaxBytes = WAN_TRACE_HARD_MAX_BYTES;
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 0, 2), std::invalid_argument);
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 513, 2), std::invalid_argument);
  options.steps = std::numeric_limits<std::size_t>::max();
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 3, 2), std::invalid_argument);

  // Only counts the final workload; this case never loads or allocates the model.
  options.latentShape = {48, 91, 20, 20};
  options.vaeSpatialStride = 16;
  options.textWidth = 4096;
  options.steps = 50;
  const auto fullBound = planWanGenerationTraceBytes(options, 512, 512);
  BOOST_CHECK_GT(fullBound, 1ULL << 30);
  BOOST_CHECK_LE(fullBound, WAN_TRACE_HARD_MAX_BYTES);
  options.traceMaxBytes = WAN_FIXTURE_TRACE_MAX_BYTES;
  BOOST_CHECK_THROW(planWanGenerationTraceBytes(options, 512, 512), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Wan22LongClipGeometryUsesItsVaeStride)
{
  const auto geometry = planVideoGeometry(361, 704, 1280, 3072, 16);
  BOOST_CHECK_EQUAL(geometry.latentFrames, 91);
  BOOST_CHECK_EQUAL(geometry.latentHeight, 44);
  BOOST_CHECK_EQUAL(geometry.latentWidth, 80);
  BOOST_CHECK_EQUAL(geometry.videoTokens, 80080);
  BOOST_CHECK_EQUAL(geometry.hiddenBytes, 984023040ULL);
  BOOST_CHECK_EQUAL(geometry.rgbBytes, 975912960ULL);
  if (const auto* root = std::getenv("NDNSF_DI_WAN22_METADATA_ROOT")) {
    const auto read = [](const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      BOOST_REQUIRE(input.good());
      return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const std::filesystem::path directory(root);
    const auto recipe = nativeParseJson(read(directory / "metadata.json"));
    BOOST_REQUIRE_EQUAL(recipe.at("revision").get<std::string>(),
                        "921dbaf3f1674a56f47e83fb80a34bac8a8f203e");
    const auto index = read(directory / "diffusion_pytorch_model.safetensors.index.json");
    const auto& files = recipe.at("sources");
    const auto pinnedIndex = std::find_if(files.begin(), files.end(), [](const auto& row) {
      return row.at("file") == "diffusion_pytorch_model.safetensors.index.json";
    });
    BOOST_REQUIRE(pinnedIndex != files.end());
    BOOST_REQUIRE_EQUAL(nativePlanningDigest(index),
                        "sha256:" + pinnedIndex->at("sha256").template get<std::string>());
    WanCheckpointShards shards;
    for (const auto& row : recipe.at("safetensors_headers")) {
      const auto name = row.at("file").get<std::string>();
      const auto header = read(directory / (name + ".header.json"));
      BOOST_REQUIRE_EQUAL(header.size(), row.at("header_bytes").get<std::uint64_t>());
      std::string wire;
      for (unsigned byte = 0; byte < 8; ++byte)
        wire.push_back(static_cast<char>((static_cast<std::uint64_t>(header.size()) >>
                                         (8 * byte)) & 255));
      wire += header;
      BOOST_REQUIRE_EQUAL(nativePlanningDigest(wire),
                          "sha256:" + row.at("sha256").get<std::string>());
      const auto file = std::find_if(files.begin(), files.end(), [&](const auto& item) {
        return item.at("file") == name;
      });
      BOOST_REQUIRE(file != files.end());
      shards.emplace(name, std::make_shared<HeaderSource>(
        std::move(wire), file->at("bytes").template get<std::uint64_t>()));
    }
    const WanCheckpoint checkpoint(std::move(shards), index);
    const auto lower = [&](std::uint64_t scoreBytes, std::uint64_t rotaryBytes) {
      return lowerWan21Denoiser(checkpoint,
        {{91, 22, 40, 80128}, 24, 512, 128, 128, scoreBytes, rotaryBytes},
        {"dit22", {}, {"rank0", "rank1"}});
    };
    BOOST_CHECK_EXCEPTION(lower(64U << 20, 64U << 20), std::invalid_argument, [](const auto& error) {
      return std::string_view(error.what()) == "Wan video rotary tile exceeds its size budget";
    });
    // Context preparation gives each of the two rank rotary graphs half the allowance.
    BOOST_CHECK_EXCEPTION(lower(64U << 20, 256U << 20),
      std::invalid_argument, [](const auto& error) {
        return std::string_view(error.what()) ==
          "Wan attention score tile exceeds its byte budget";
      });
    const auto actual = lower(512U << 20, 256U << 20);
    BOOST_REQUIRE_EQUAL(actual.layout.blockEnds.size(), 30);
    BOOST_CHECK_EQUAL(actual.initializerRangeSource->size(), 19999150848ULL);
    BOOST_CHECK_THROW(actual.initializerRangeSource->read(0, 4), std::out_of_range);
  }

  const auto output = planWanClipOutput({48, 3, 2, 4}, 16);
  BOOST_REQUIRE(output.framesHeightWidth == (std::array<std::int64_t, 3>{9, 32, 64}));
  BOOST_CHECK_EQUAL(output.rgb24Bytes, 9U * 32 * 64 * 3);
  BOOST_CHECK_THROW(planWanClipOutput({48, 91, 44, 80}, 16), std::invalid_argument);
  BOOST_CHECK_THROW(planWanClipOutput({48, 3, 2, 4}, 0), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(360, 704, 1280, 3072, 16), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(361, 720, 1280, 3072, 16), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(361, 704, 1280, 3072, 0), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Wan21ProductionShapeHasCheckedByteCounts)
{
  const auto geometry = planVideoGeometry(81, 480, 832, 5120);
  BOOST_CHECK_EQUAL(geometry.latentFrames, 21);
  BOOST_CHECK_EQUAL(geometry.latentHeight, 60);
  BOOST_CHECK_EQUAL(geometry.latentWidth, 104);
  BOOST_CHECK_EQUAL(geometry.videoTokens, 32760);
  BOOST_CHECK_EQUAL(geometry.hiddenBytes, 670924800);
  BOOST_CHECK_EQUAL(geometry.rgbBytes, 97044480);
  const auto clipBytes = tensorBundleEncodedSize(
    {{sizeof("rgb") - 1, 5, geometry.rgbBytes * sizeof(float)}});
  BOOST_CHECK_EQUAL(clipBytes, 388177995ULL);
  const auto clip = planWanClipOutput({16, 21, 60, 104});
  BOOST_CHECK_EQUAL(clip.rgb24Bytes, geometry.rgbBytes);
  BOOST_CHECK_EQUAL(clip.responseBytes, clipBytes);
  BOOST_CHECK_THROW(planWanClipOutput({16, 33, 68, 104}), std::invalid_argument);
  constexpr std::uint64_t responseChunkBytes = 4096;
  const auto responseSegments = (clipBytes + responseChunkBytes - 1) / responseChunkBytes;
  BOOST_CHECK_EQUAL(responseSegments, 94771U);
  BOOST_CHECK_LE(clipBytes, ndn_service_framework::MAX_REQUEST_RESPONSE_BYTES);
  BOOST_CHECK_LE(responseSegments, ndn_service_framework::MAX_REQUEST_RESPONSE_SEGMENTS);
  const auto tokens = geometry.videoTokens;
  const TensorBundleEncodingMember videoMask{sizeof("video-valid") - 1, 4, tokens};
  BOOST_CHECK_LT(tensorBundleEncodedSize({videoMask}), 33U << 10);
  BOOST_CHECK_THROW(tensorBundleEncodedSize({{videoMask.nameBytes, videoMask.rank,
                                             tokens * tokens}}), std::invalid_argument);
  constexpr std::uint64_t groupBytes = 64U << 20;
  constexpr std::uint64_t rowBytes = 5120 * sizeof(float);
  const TensorBundleEncodingMember hidden{sizeof("hidden") - 1, 3, geometry.hiddenBytes};
  BOOST_CHECK_THROW(tensorBundleEncodedSize({hidden}), std::invalid_argument);
  const auto oneRow = tensorBundleEncodedSize({{hidden.nameBytes, hidden.rank, rowBytes}});
  const auto rowsPerObject = (groupBytes - (oneRow - rowBytes)) / rowBytes;
  BOOST_REQUIRE_EQUAL(rowsPerObject, 3276);
  BOOST_CHECK_LE(tensorBundleEncodedSize({{hidden.nameBytes, hidden.rank, rowsPerObject * rowBytes}}),
                 groupBytes);
  BOOST_CHECK_GT(tensorBundleEncodedSize(
    {{hidden.nameBytes, hidden.rank, (rowsPerObject + 1) * rowBytes}}), groupBytes);
  BOOST_CHECK_EQUAL((geometry.videoTokens + rowsPerObject - 1) / rowsPerObject, 10);
  BOOST_CHECK_THROW(planVideoGeometry(80, 480, 832, 5120), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(81, 488, 832, 5120), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(81, 480, 832, 0), std::invalid_argument);
  BOOST_CHECK_THROW(planVideoGeometry(81, 480, 832, std::numeric_limits<std::uint64_t>::max()),
                    std::overflow_error);
}

BOOST_AUTO_TEST_CASE(WanRgb24ConversionDeliversBoundedOrderedChunks)
{
  constexpr std::size_t pixels = 65537;
  std::vector<float> planes(3 * pixels);
  std::fill_n(planes.begin(), pixels, -2.0F);
  std::fill_n(planes.begin() + pixels, pixels, 0.0F);
  std::fill_n(planes.begin() + 2 * pixels, pixels, 2.0F);
  planes[pixels - 1] = 1.0F;
  planes[2 * pixels - 1] = -1.0F;
  planes.back() = 0.0F;
  const auto bytes = ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(planes.data()), planes.size() * sizeof(float));
  const std::array<std::uint8_t, 3> pixel{0, 127, 255};
  const std::array<std::uint8_t, 3> lastPixel{255, 0, 127};
  std::vector<std::size_t> chunks;
  writeWanRgb24(bytes, [&](auto chunk) {
    chunks.push_back(chunk.size());
    BOOST_CHECK_EQUAL(chunk.size() % pixel.size(), 0);
    const auto& expected = chunks.size() == 1 ? pixel : lastPixel;
    for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
      BOOST_CHECK_EQUAL(chunk[channel], expected[channel]);
      BOOST_CHECK_EQUAL(chunk[chunk.size() - pixel.size() + channel], expected[channel]);
    }
  }, [] {});
  BOOST_CHECK((chunks == std::vector<std::size_t>{3 * 65536, 3}));

  planes.back() = std::numeric_limits<float>::quiet_NaN();
  std::size_t emitted = 0;
  BOOST_CHECK_THROW(writeWanRgb24(bytes, [&](auto chunk) { emitted += chunk.size(); }, [] {}),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(emitted, 3 * 65536);
  emitted = 0;
  BOOST_CHECK_THROW(writeWanRgb24(bytes, [&](auto chunk) { emitted += chunk.size(); },
    [&] {
      if (emitted) throw std::runtime_error("cancelled");
    }), std::runtime_error);
  BOOST_CHECK_EQUAL(emitted, 3 * 65536);
}

BOOST_AUTO_TEST_CASE(InputChannelShardsCoverEveryConvolutionKernelExactlyOnce)
{
  // Four output filters with five input channels, each storing a 2x2 kernel.
  const InitializerSlice rank0({4, 5, 2, 2}, 1, 0, 2, 4);
  const InitializerSlice rank1({4, 5, 2, 2}, 1, 2, 5, 4);
  BOOST_REQUIRE_EQUAL(rank0.blockCount(), 4);
  BOOST_REQUIRE_EQUAL(rank1.blockCount(), 4);
  BOOST_CHECK_EQUAL(rank0.sourceBytes(), 320);
  BOOST_CHECK_EQUAL(rank0.sliceBytes(), 128);
  BOOST_CHECK_EQUAL(rank1.sliceBytes(), 192);
  for (std::uint64_t filter = 0; filter < 4; ++filter) {
    BOOST_CHECK_EQUAL(rank0.sourceOffset(filter), filter * 80);
    BOOST_CHECK_EQUAL(rank1.sourceOffset(filter), filter * 80 + 32);
    BOOST_CHECK_EQUAL(rank1.sourceOffset(filter) + rank1.blockBytes(), (filter + 1) * 80);
  }
  BOOST_CHECK_THROW(rank1.sourceOffset(4), std::out_of_range);
  BOOST_CHECK_THROW(InitializerSlice({4, 5, 2, 2}, 1, 2, 6, 4), std::invalid_argument);
  BOOST_CHECK_THROW(InitializerSlice({0, 5, 2, 2}, 1, 2, 5, 4), std::invalid_argument);
  BOOST_CHECK_THROW(InitializerSlice({4, 5, 0, 2}, 1, 2, 5, 4), std::invalid_argument);
  BOOST_CHECK_THROW(InitializerSlice({std::numeric_limits<std::uint64_t>::max(), 2}, 1, 0, 1, 4),
                    std::overflow_error);
}

BOOST_AUTO_TEST_CASE(RangeWindowsDoNotReadUnassignedColumns)
{
  const InitializerSlice rank({3, 5}, 1, 1, 4, 4);
  std::vector<std::uint64_t> starts;
  std::vector<std::uint64_t> sizes;
  rank.forEachRange(8, [&] (auto start, auto size) {
    starts.push_back(start);
    sizes.push_back(size);
  });
  const std::vector<std::uint64_t> expectedStarts{4, 12, 24, 32, 44, 52};
  const std::vector<std::uint64_t> expectedSizes{8, 4, 8, 4, 8, 4};
  BOOST_CHECK_EQUAL_COLLECTIONS(starts.begin(), starts.end(),
                                expectedStarts.begin(), expectedStarts.end());
  BOOST_CHECK_EQUAL_COLLECTIONS(sizes.begin(), sizes.end(),
                                expectedSizes.begin(), expectedSizes.end());
  BOOST_CHECK_THROW(rank.forEachRange(0, [] (auto, auto) {}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di::wan::tests
