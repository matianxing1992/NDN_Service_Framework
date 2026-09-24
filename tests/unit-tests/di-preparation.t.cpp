#include "NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ndnsf::di {

class NativeCanonicalPublisherTestAccess
{
public:
  using Transport = NativeCanonicalArtifactPublisher::Transport;
  static NativeCanonicalArtifactPublisher create(
    Transport transport, NativeCanonicalPublicationOptions options,
    NativeCanonicalArtifactPublisher::SourcePort source)
  {
    return NativeCanonicalArtifactPublisher(std::move(transport), "/service",
                                            std::move(options), std::move(source));
  }
};

// Test-only access keeps the production cache owner private while allowing
// deterministic C++ fixtures to hold a job at its source barrier.
struct Spec185PreparationTestAccess
{
  static PreparationHandle prepareAsync(ModelPreparationCache& cache,
                                        const PreparationSpec& spec,
                                        CachePolicy policy = CachePolicy::UseOrFetch,
                                        std::chrono::milliseconds timeout = {})
  {
    return PreparationHandle(cache.prepareAsync(spec, policy, timeout));
  }
};

} // namespace ndnsf::di

namespace {

using namespace ndnsf::di;
using namespace ndn_service_framework;

struct Fixture
{
  NativeJson oracle;
  NativeCanonicalSource source;
  NativeModelDescriptor descriptor;
  NativeJson catalog;
  PreparationSpec spec;
  std::string canonicalGraphDigest;
  std::string planningGraphDigest;
  std::size_t fetches = 0;

  Fixture()
  {
    std::ifstream file("tests/fixtures/spec182/yolo-semantic-oracle.json");
    BOOST_REQUIRE(file.good());
    file >> oracle;
    const auto hex = oracle.at("model_hex").get<std::string>();
    for (std::size_t i = 0; i < hex.size(); i += 2)
      source.modelBytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    std::ifstream graphOracleFile("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
    BOOST_REQUIRE(graphOracleFile.good());
    NativeJson graphOracles;
    graphOracleFile >> graphOracles;
    NativeJson graphOracle;
    for (const auto& item : graphOracles) {
      if (item.at("model_hex") == hex) {
        graphOracle = item;
        break;
      }
    }
    BOOST_REQUIRE(!graphOracle.is_null());
    canonicalGraphDigest = graphOracle.at("canonical_graph_digest").get<std::string>();
    const auto sourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
    const auto control = NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {}, 1 << 20, 1 << 20};
    const auto identity = canonicalOnnxSourceIdentity(source, control);
    descriptor = fixture::completeModel({"yolo26n", nativePlanningDigest("YOLOFixture-content"),
      nativePlanningDigest("YOLOFixture-semantics"), oracle.at("graph_digest"), "onnx", "float32", "YOLOFixture", "1"});
    // The descriptor carries the planning graph identity, while the catalog
    // source field below carries the canonical ONNX source graph identity.
    // Derive the former from the native inspector instead of conflating the
    // two digests in the fixture.
    const auto planning = inspectNativeOnnxSourceGraph(source, descriptor, control);
    descriptor.graphDigest = planning.graph.graphDigest;
    planningGraphDigest = planning.graph.graphDigest;
    BOOST_REQUIRE_EQUAL(identity.graphDigest, canonicalGraphDigest);
    catalog = NativeJson::object();
    catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
    catalog["model"] = nativeParseJson(descriptor.canonicalJson());
    catalog["source"] = NativeJson::object();
    catalog["source"]["data_name"] = "/fixture/source";
    catalog["source"]["digest"] = sourceDigest;
    catalog["source"]["model_manifest_digest"] = nativePlanningDigest("fixture-manifest");
    catalog["source"]["canonical_graph_digest"] = identity.graphDigest;
    catalog["recipe"] = NativeJson::object();
    catalog["recipe"]["artifact_profile_digest"] = nativePlanningDigest("fixture-profile");
    catalog["recipe"]["assembler_descriptor_digest"] = nativePlanningDigest("fixture-assembler-v1");
    catalog["recipe"]["backend_abi"] = "fixture-abi";
    catalog["recipe"]["precision"] = descriptor.precision;
    catalog["recipe"]["quantization"] = "none";
    catalog["recipe"]["layout"] = "NCHW";
    catalog["recipe"]["padding"] = "none";
    catalog["recipe"]["protection_epoch"] = "fixture-epoch";
    catalog["recipe"]["max_source_bytes"] = 1 << 20;
    catalog["recipe"]["max_assembled_bytes"] = 1 << 20;
    catalog["recipe"]["max_nodes"] = 100;
    catalog["publication"] = NativeJson::object();
    catalog["publication"]["artifact_root"] = "/fixture/artifacts";
    catalog["input_format"] = "JSON";
    catalog["max_payload_bytes"] = 32;
    NativeJson component = NativeJson::object();
    component["candidate_id"] = "semantic-v1";
    component["priority"] = 1;
    component["roles"] = {"Front", "Branch", "Merge"};
    component["node_names_by_role"] = NativeJson::object();
    component["input_ingress_role"] = "Front";
    component["result_egress_role"] = "Merge";
    component["merge_kind"] = "NATIVE_POSTPROCESS";
    component["candidate_digest"] = oracle.at("registered_digest");
    component["semantic_partition"] = oracle.at("partition");
    catalog["splitter"] = NativeJson::object();
    catalog["splitter"]["kind"] = "YOLO";
    catalog["splitter"]["components"] = NativeJson::array({component});
    spec.key = "default";
    spec.baseDirectory = ".";
    spec.catalogConfigurationJson = nativeCanonicalJson(catalog);
    spec.taskName = "task";
    spec.taskContractDigest = nativePlanningDigest("task-contract");
    spec.inputLayoutDigest = nativePlanningDigest("input-layout");
    spec.maxSourceBytes = 1 << 20;
    spec.maxAssembledBytes = 1 << 20;
    const NativeJson runtime{
      {"schema", "ndnsf-di-native-requester-v1"},
      {"request", {{"task", spec.taskName}, {"task_descriptor_digest", spec.taskContractDigest},
        {"input_layout_digest", spec.inputLayoutDigest}, {"generation_mode", "TOKEN_DIAGNOSTIC"}}},
      {"catalog", catalog},
      {"limits", {{"max_source_bytes", spec.maxSourceBytes}, {"max_assembled_bytes", spec.maxAssembledBytes}}}};
    spec.configurationJson = nativeCanonicalJson(runtime);
    spec.configurationDigest = nativePlanningDigest(spec.configurationJson);
    spec.loadSource = [this] (const PreparationSpec&, std::chrono::steady_clock::time_point deadline) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("fixture deadline");
      ++fetches;
      return source;
    };
  }

  PreparationSpec withCatalog(NativeJson value) const
  {
    auto result = spec;
    result.catalogConfigurationJson = nativeCanonicalJson(value);
    auto runtime = nativeParseJson(result.configurationJson);
    runtime["catalog"] = value;
    result.configurationJson = nativeCanonicalJson(runtime);
    result.configurationDigest = nativePlanningDigest(result.configurationJson);
    return result;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185Preparation)

BOOST_AUTO_TEST_CASE(ColdPreparationPublishesOnlyVerifiedImmutablePackage)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto prepared = cache.prepare(fixture.spec);
  BOOST_CHECK_EQUAL(prepared.manifest().modelName, "yolo26n");
  BOOST_CHECK_EQUAL(prepared.manifest().taskName, "task");
  BOOST_CHECK_EQUAL(prepared.manifest().canonicalGraphDigest, fixture.canonicalGraphDigest);
  BOOST_CHECK_EQUAL(prepared.manifest().planningGraphDigest, fixture.planningGraphDigest);
  BOOST_CHECK(prepared.receipt().origin == PreparationReceipt::Origin::Fetched);
  BOOST_CHECK_EQUAL(prepared.receipt().manifestDigest,
                    fixture.catalog.at("source").at("model_manifest_digest"));
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
  BOOST_CHECK(prepared.capabilities().inputKinds == std::vector<std::string>{"BYTES"});
  BOOST_CHECK(!prepared.capabilities().conversations);

  // A hit returns the same verified package without reading or parsing source.
  auto hit = cache.prepare(fixture.spec);
  BOOST_CHECK(hit.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(hit.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
}

BOOST_AUTO_TEST_CASE(CompletePreparedHitRestoresQwenCatalogWithoutSourceLoader)
{
  const auto digest = [] (const std::string& value) { return nativePlanningDigest(value); };
  const std::string role = "/Qwen/Stage/0";
  auto descriptor = fixture::completeModel({
    "QwenReferenceFixture", digest("qwen-content"), digest("qwen-semantics"),
    digest("placeholder-graph"), "onnx", "float32", "qwen", "1"});
  descriptor.sourceRevision = "revision";
  const NativeJson ranges = NativeJson::array({NativeJson::array({0, 1})});
  const NativeJson nodes = NativeJson{"embedding", "layer-00", "final-norm-head"};
  const NativeJson edges = NativeJson{"hidden-embedding-to-layer-00", "hidden-layer-0-to-final"};
  descriptor.graphDigest = digest(nativeCanonicalJson(NativeJson{
    {"model", descriptor.modelName}, {"revision", descriptor.sourceRevision},
    {"precision", descriptor.precision}, {"decode_mode", "single-token-autoregressive"},
    {"modality", "text-only"}, {"mtp_enabled", false}, {"thinking_mode", "disabled"},
    {"layer_ranges", ranges}, {"nodes", nodes}, {"edges", edges}, {"legal_cuts", edges}}));

  const auto sourceDigest = digest("reference-source");
  const auto manifestDigest = digest("reference-manifest");
  const auto canonicalGraphDigest = digest("reference-canonical-graph");
  const auto profileDigest = digest("reference-profile");
  const auto assemblerDigest = digest("reference-assembler");
  const auto catalog = NativeJson{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/qwen/reference/source"}, {"digest", sourceDigest},
                 {"model_manifest_digest", manifestDigest}, {"canonical_graph_digest", canonicalGraphDigest}}},
    {"recipe", {{"artifact_profile_digest", profileDigest},
                 {"assembler_descriptor_digest", assemblerDigest}, {"backend_abi", "qwen-reference-abi"},
                 {"precision", "float32"}, {"quantization", "none"}, {"layout", "NCHW"},
                 {"padding", "none"}, {"protection_epoch", "reference-epoch"},
                 {"max_source_bytes", 1U << 20}, {"max_assembled_bytes", 1U << 20}, {"max_nodes", 16}}},
    {"publication", {{"artifact_root", "/qwen/reference/artifacts"}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 1024},
    {"splitter", {{"kind", "QWEN"}, {"layer_ranges", {{0, 1}}},
                   {"artifact_digests_by_role", {{role, digest("reference-artifact")}}},
                   {"weight_bytes_by_role", {{role, 1}}}, {"roles", {role}},
                   {"tensor_degrees", {1}}, {"input_ingress_role", role},
                   {"result_egress_role", role}}},
    {"node_mapping", NativeJson::object()}, {"state_inputs", NativeJson::object()},
    {"state_outputs", NativeJson::object()}};

  NativeJson encodedNodes = NativeJson::array();
  NativeJson canonicalNodeIndices = NativeJson::object();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    encodedNodes.push_back({{"id", nodes.at(i)}, {"opType", nodes.at(i)}, {"ordinal", i}});
    canonicalNodeIndices[nodes.at(i).get<std::string>()] = i;
  }
  const auto preparedMetadata = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-metadata-v1"},
    {"descriptor", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"name", "/qwen/reference/source"}, {"digest", sourceDigest}, {"bytes", 777},
                 {"manifest_digest", manifestDigest}, {"graph_digest", canonicalGraphDigest},
                 {"initializer_object_digest", ""}, {"initializer_bytes", 0}, {"initializer_digest", ""}}},
    {"sourceGraph", {{"graphDigest", descriptor.graphDigest}, {"nodes", encodedNodes},
                      {"modelOutputs", NativeJson::array()}, {"graphMetadataJson", "{}"},
                      {"canonicalNodeIndices", canonicalNodeIndices},
                      {"canonicalIdentity", {{"graphDigest", canonicalGraphDigest},
                                              {"initializerDigest", ""}}}}}});

  NativePreparedCanonicalPublication publication;
  publication.sourceDataName = "/qwen/reference/source";
  publication.rootDataName = "/qwen/reference/artifacts/manifest";
  publication.canonicalManifestJson = "{}";
  publication.manifestDigest = digest("{}");
  publication.preparedMetadataJson = preparedMetadata;
  auto materialManifest = std::make_shared<NativeCanonicalSource::MaterialManifest>();
  materialManifest->sourceDigest = sourceDigest;
  materialManifest->graphDigest = canonicalGraphDigest;
  materialManifest->initializerDigest = digest("reference-no-initializer");
  materialManifest->templatePayloadId = "reference-template";
  materialManifest->payloadsComplete = false;
  materialManifest->references.push_back({"reference-template", {}, "graph-template", "__template__", 0,
                                          digest("reference-template-bytes"), 1, {}, ""});
  materialManifest->manifestDigest = digest(materialManifest->canonicalJson());
  materialManifest->validate();
  publication.materialManifest = materialManifest;

  PreparationSpec spec;
  spec.key = "qwen-reference-hit";
  spec.baseDirectory = ".";
  spec.catalogConfigurationJson = nativeCanonicalJson(catalog);
  const NativeJson runtime{
    {"schema", "ndnsf-di-native-requester-v1"},
    {"request", {{"task", "task"}, {"task_descriptor_digest", digest("task")},
                  {"input_layout_digest", digest("input")}, {"generation_mode", "TOKEN_DIAGNOSTIC"}}},
    {"catalog", catalog},
    {"limits", {{"max_source_bytes", 1U << 20}, {"max_assembled_bytes", 1U << 20}}}};
  spec.configurationJson = nativeCanonicalJson(runtime);
  spec.configurationDigest = digest(spec.configurationJson);
  spec.taskName = "task";
  spec.taskContractDigest = digest("task");
  spec.inputLayoutDigest = digest("input");
  spec.maxSourceBytes = 1U << 20;
  spec.maxAssembledBytes = 1U << 20;
  std::atomic<unsigned> sourceLoads{0};
  spec.loadSource = [&sourceLoads] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    ++sourceLoads;
    throw std::runtime_error("complete prepared hit loaded canonical source");
    return NativeCanonicalSource{};
  };
  spec.lookupPrepared = [publication] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    return std::optional<NativePreparedCanonicalPublication>{publication};
  };
  std::vector<PreparationSpec::MemorySnapshot> snapshots;
  spec.memoryObserver = [&snapshots] (const auto& snapshot) { snapshots.push_back(snapshot); };

  ModelPreparationCache cache(8 << 20, 1, std::chrono::seconds(5));
  const auto prepared = cache.prepare(spec);
  BOOST_CHECK(prepared.receipt().origin == PreparationReceipt::Origin::Fetched);
  BOOST_CHECK_EQUAL(sourceLoads.load(), 0U);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(prepared.manifest().modelName, "QwenReferenceFixture");
  BOOST_REQUIRE_EQUAL(snapshots.size(), 1U);
  BOOST_CHECK_EQUAL(snapshots.back().sourceBytes, 0U);
  BOOST_CHECK_EQUAL(snapshots.back().initializerBytes, 0U);
}

BOOST_AUTO_TEST_CASE(Spec189PreparationMemorySnapshotCoversOwnersAndCancellation)
{
  Fixture fixture;
  std::vector<PreparationSpec::MemorySnapshot> samples;
  fixture.spec.memoryObserver = [&samples] (const auto& sample) {
    samples.push_back(sample);
  };
  fixture.spec.preparePublication = [] (const NativeCanonicalPreparationCatalog&,
                                        const NativeInspectedModel&,
                                        const NativeRequestControl&) {
    NativePreparedCanonicalPublication publication;
    publication.sourceDataName = "/fixture/source";
    publication.rootDataName = "/fixture/root";
    publication.canonicalManifestJson = "{}";
    publication.manifestDigest = nativePlanningDigest("{}");
    publication.publishedBytes = 123;
    return publication;
  };
  fixture.spec.rollbackPublication = [] (const NativePreparedCanonicalPublication&) {};

  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  (void) cache.prepare(fixture.spec);
  BOOST_REQUIRE_EQUAL(samples.size(), 1U);
  const auto& sample = samples.back();
  BOOST_CHECK_EQUAL(sample.sourceBytes, fixture.source.modelBytes.size());
  BOOST_CHECK_EQUAL(sample.initializerBytes, 0U);
  BOOST_CHECK_GT(sample.materialBytes, 0U);
  BOOST_CHECK_EQUAL(sample.encryptedPublicationBytes, 123U);
  BOOST_CHECK_EQUAL(sample.ortPreparationBudgetBytes, fixture.spec.maxAssembledBytes);
  BOOST_CHECK_GE(sample.peakBytes,
                 sample.sourceBytes + sample.materialBytes +
                 sample.encryptedPublicationBytes + sample.ortPreparationBudgetBytes);
  BOOST_CHECK(sample.sourceOwnerReleased);
  BOOST_CHECK(sample.cacheEntryCommitted);
  BOOST_CHECK(!sample.publicationRollbackAttempted);
  BOOST_CHECK(sample.terminalCleanup);

  auto cancelled = fixture.spec;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  auto commitCalls = std::make_shared<std::atomic<unsigned>>(0);
  auto rollbacks = std::make_shared<std::atomic<unsigned>>(0);
  cancelled.cancelled = [cancel] { return cancel->load(std::memory_order_acquire); };
  cancelled.acquireCommit = [cancel, commitCalls] (std::chrono::steady_clock::time_point) {
    if (commitCalls->fetch_add(1, std::memory_order_acq_rel) == 0)
      return std::shared_ptr<void>(std::make_shared<int>(0));
    cancel->store(true, std::memory_order_release);
    return std::shared_ptr<void>{};
  };
  cancelled.rollbackPublication = [rollbacks] (const NativePreparedCanonicalPublication&) {
    rollbacks->fetch_add(1, std::memory_order_acq_rel);
  };
  std::vector<PreparationSpec::MemorySnapshot> cancelledSamples;
  cancelled.memoryObserver = [&cancelledSamples] (const auto& sample) {
    cancelledSamples.push_back(sample);
  };
  ModelPreparationCache cancelledCache(8 << 20, 2, std::chrono::seconds(5));
  BOOST_CHECK_THROW(cancelledCache.prepare(cancelled), std::runtime_error);
  BOOST_REQUIRE_EQUAL(cancelledSamples.size(), 1U);
  BOOST_CHECK_EQUAL(rollbacks->load(std::memory_order_acquire), 1U);
  BOOST_CHECK_EQUAL(cancelledCache.entryCount(), 0U);
  BOOST_CHECK_EQUAL(cancelledCache.chargedBytes(), 0U);
  BOOST_CHECK(!cancelledSamples.back().cacheEntryCommitted);
  BOOST_CHECK(cancelledSamples.back().publicationRollbackAttempted);
  BOOST_CHECK(cancelledSamples.back().terminalCleanup);
  BOOST_CHECK(cancelledSamples.back().sourceOwnerReleased);
}

BOOST_AUTO_TEST_CASE(PreparationKeyIgnoresLocalSourceLocator)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto firstCatalog = fixture.catalog;
  firstCatalog["source"]["file"] = "models/first/model.onnx";
  auto secondCatalog = fixture.catalog;
  secondCatalog["source"]["file"] = "models/second/model.onnx";
  auto first = fixture.withCatalog(firstCatalog);
  auto second = fixture.withCatalog(secondCatalog);

  auto prepared = cache.prepare(first);
  auto hit = cache.prepare(second);
  BOOST_CHECK(hit.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(hit.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
}

BOOST_AUTO_TEST_CASE(PreparationRejectsDetachedCatalogAndHonoursCancellation)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));

  auto detached = fixture.spec;
  auto foreignCatalog = fixture.catalog;
  foreignCatalog["source"]["data_name"] = "/foreign/source";
  detached.catalogConfigurationJson = nativeCanonicalJson(foreignCatalog);
  BOOST_CHECK_THROW(cache.prepare(detached), std::exception);
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);

  auto cancelled = fixture.spec;
  cancelled.cancelled = [] { return true; };
  BOOST_CHECK_EXCEPTION(cache.prepare(cancelled), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("CANCELLED") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
}

BOOST_AUTO_TEST_CASE(PreparationCancellationRollsBackFetchReservation)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 2, std::chrono::seconds(5));
  auto lateCancelled = fixture.spec;
  lateCancelled.cancelled = [&fixture] { return fixture.fetches != 0; };
  BOOST_CHECK_EXCEPTION(cache.prepare(lateCancelled), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("CANCELLED") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
  BOOST_CHECK_EQUAL(cache.chargedBytes(), 0U);

  auto prepared = cache.prepare(fixture.spec);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK(!prepared.manifest().modelName.empty());
}

BOOST_AUTO_TEST_CASE(PreparationRejectsWrongIdentityAndNonOnnxSource)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 4, std::chrono::seconds(5));

  auto wrongGraph = fixture.catalog;
  wrongGraph["source"]["canonical_graph_digest"] = nativePlanningDigest("foreign-graph");
  BOOST_CHECK_THROW(cache.prepare(fixture.withCatalog(wrongGraph)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto wrongTask = fixture.spec;
  wrongTask.taskName = "foreign-task";
  BOOST_CHECK_THROW(cache.prepare(wrongTask), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto nonOnnx = fixture;
  nonOnnx.source.modelBytes = {'{', '}', '\n'};
  nonOnnx.spec.loadSource = [&nonOnnx] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    return nonOnnx.source;
  };
  auto nonOnnxCatalog = nonOnnx.catalog;
  nonOnnxCatalog["source"]["digest"] = nativePlanningDigest(nonOnnx.source.modelBytes.data(),
                                                               nonOnnx.source.modelBytes.size());
  BOOST_CHECK_THROW(cache.prepare(nonOnnx.withCatalog(nonOnnxCatalog)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  auto unsupported = fixture.spec;
  unsupported.taskName = "other-task";
  auto unsupportedRuntime = nativeParseJson(unsupported.configurationJson);
  unsupportedRuntime["request"]["task"] = unsupported.taskName;
  unsupported.configurationJson = nativeCanonicalJson(unsupportedRuntime);
  unsupported.configurationDigest = nativePlanningDigest(unsupported.configurationJson);
  BOOST_CHECK_EXCEPTION(cache.prepare(unsupported), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("UNSUPPORTED_CAPABILITY") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
}

BOOST_AUTO_TEST_CASE(PreparationRejectsMissingInitializerAndBudgetBeforePublication)
{
  Fixture fixture;
  ModelPreparationCache cache(64, 1, std::chrono::seconds(5));
  BOOST_CHECK_THROW(cache.prepare(fixture.spec), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);

  Fixture missingInitializer;
  auto catalog = missingInitializer.catalog;
  catalog["source"]["initializer_digest"] = nativePlanningDigest("missing-initializer");
  BOOST_CHECK_THROW(cache.prepare(missingInitializer.withCatalog(catalog)), std::exception);
  BOOST_CHECK_EQUAL(cache.entryCount(), 0U);
}

BOOST_AUTO_TEST_CASE(RequireReadyAndUseOrWaitNeverPerformAnImplicitFetch)
{
  Fixture fixture;
  ModelPreparationCache cache(4 << 20, 1, std::chrono::seconds(5));
  BOOST_CHECK_THROW(cache.prepare(fixture.spec, CachePolicy::RequireReady), std::exception);
  BOOST_CHECK_EXCEPTION(cache.prepare(fixture.spec, CachePolicy::UseOrWait), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("PREPARATION_NOT_IN_FLIGHT") != std::string::npos;
                        });
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
  BOOST_CHECK_EQUAL(cache.parseCount(), 0U);
}

BOOST_AUTO_TEST_CASE(RefreshKeepsOldLeaseBytesUntilTheOldObjectIsReleased)
{
  Fixture fixture;
  // Refresh reserves model and optional initializer staging while retaining
  // the old leased package until commit; allow both generations in this
  // focused budget test.
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::size_t refreshedCharge = 0;
  {
    auto prepared = cache.prepare(fixture.spec);
    auto refreshed = cache.prepare(fixture.spec, CachePolicy::Refresh);
    BOOST_CHECK(refreshed.receipt().origin == PreparationReceipt::Origin::Refreshed);
    refreshedCharge = cache.chargedBytes();
    BOOST_CHECK(refreshedCharge > refreshed.manifest().modelName.size());
    BOOST_CHECK_EQUAL(prepared.manifest().modelName, "yolo26n");
  }
  BOOST_CHECK(cache.chargedBytes() < refreshedCharge);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
}

BOOST_AUTO_TEST_CASE(EightConcurrentPreparationsUseOneFetchAndInspection)
{
  Fixture fixture;
  ModelPreparationCache cache(16 << 20, 2, std::chrono::seconds(5));
  std::atomic<unsigned> loads{0};
  std::mutex gateMutex;
  std::condition_variable gate;
  bool release = false;
  auto makeSpec = [&] {
    auto spec = fixture.spec;
    spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
      const auto count = loads.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count == 1)
        gate.notify_all();
      std::unique_lock<std::mutex> lock(gateMutex);
      gate.wait(lock, [&] { return release; });
      return fixture.source;
    };
    return spec;
  };
  std::vector<std::thread> threads;
  std::atomic<unsigned> successes{0};
  std::vector<std::exception_ptr> errors(8);
  for (unsigned i = 0; i < 8; ++i) {
    threads.emplace_back([&, i, spec = makeSpec()] {
      try {
        (void)cache.prepare(spec);
        successes.fetch_add(1, std::memory_order_relaxed);
      }
      catch (...) { errors[i] = std::current_exception(); }
    });
  }
  bool bothLoaded = false;
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    bothLoaded = gate.wait_for(lock, std::chrono::seconds(2), [&] {
      return loads.load(std::memory_order_acquire) >= 1;
    });
    release = true;
  }
  gate.notify_all();
  for (auto& thread : threads)
    thread.join();
  BOOST_REQUIRE(bothLoaded);
  BOOST_CHECK_EQUAL(successes.load(std::memory_order_relaxed), 8U);
  for (const auto& error : errors)
    BOOST_CHECK(!error);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK_EQUAL(cache.parseCount(), 1U);
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
}

BOOST_AUTO_TEST_SUITE(Spec185PreparationT004)

BOOST_AUTO_TEST_CASE(RefreshFailureRetainsThePreviouslyPublishedReadyPackage)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  auto prepared = cache.prepare(fixture.spec);
  auto failing = fixture.spec;
  failing.loadSource = [] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    throw std::runtime_error("refresh source failed");
    return NativeCanonicalSource{};
  };
  BOOST_CHECK_THROW(cache.prepare(failing, CachePolicy::Refresh), std::exception);
  auto ready = cache.prepare(fixture.spec, CachePolicy::RequireReady);
  BOOST_CHECK(ready.receipt().origin == PreparationReceipt::Origin::CacheHit);
  BOOST_CHECK_EQUAL(ready.manifest().preparationKeyDigest,
                    prepared.manifest().preparationKeyDigest);
}

BOOST_AUTO_TEST_CASE(PinnedEntryBlocksEvictionUntilItsLeaseIsReleased)
{
  Fixture fixture;
  auto otherCatalog = fixture.catalog;
  otherCatalog["source"]["data_name"] = "/fixture/other-source";
  auto other = fixture.withCatalog(otherCatalog);
  ModelPreparationCache cache(8 << 20, 1, std::chrono::seconds(5));
  {
    auto pinned = cache.prepare(fixture.spec);
    BOOST_CHECK_EXCEPTION(cache.prepare(other), std::runtime_error,
                          [] (const std::runtime_error& error) {
                            return std::string(error.what()).find("BUDGET_EXCEEDED") != std::string::npos;
                          });
  }
  auto replacement = cache.prepare(other);
  BOOST_CHECK_EQUAL(cache.entryCount(), 1U);
  BOOST_CHECK(!replacement.manifest().preparationKeyDigest.empty());
}

BOOST_AUTO_TEST_CASE(PreparedPackageOwnsPublicationLeaseUntilCacheEviction)
{
  Fixture fixture;
  auto publicationLease = std::make_shared<int>(189);
  const std::weak_ptr<int> weakPublicationLease = publicationLease;
  std::atomic<unsigned> publicationCalls{0};
  NativeCanonicalPublisherTestAccess::Transport transport{
    [] (std::function<void()> work) { work(); },
    [] { return false; },
    [] {
      PreparedServiceRequest request;
      request.requestId = "spec189-cache-owner";
      request.serviceName = ndn::Name("/service");
      return request;
    },
    [&publicationCalls, weakPublicationLease]
      (const PreparedServiceRequest&, const std::vector<std::uint8_t>& bytes,
       const std::string& label, const NativeRequestControl& control) {
      control.requireActive();
      publicationCalls.fetch_add(1, std::memory_order_relaxed);
      ndn_service_framework::LargeDataPublishResult result;
      result.success = true;
      result.encrypted = true;
      result.objectId = label;
      result.encryptedDataName = ndn::Name("/fixture/encrypted").append(label).appendVersion(1);
      result.plaintextSize = bytes.size();
      result.contentDigest = nativePlanningDigest(bytes.data(), bytes.size());
      result.manifestDigest = nativePlanningDigest("spec189-cache-owner-transport");
      result.authorizationScope = "/SERVICE/service";
      result.protectionEpoch = "spec189-cache-owner-epoch";
      result.servingLease = weakPublicationLease.lock();
      if (!result.servingLease) {
        result.success = false;
        result.errorMessage = "fixture publication lease expired";
      }
      return result;
    },
    [] (const std::vector<ndn_service_framework::LargeDataPublishResult>&) {},
    true};
  const NativeCanonicalPublicationOptions options{
    "/fixture/NDNSF/DI/ARTIFACT", nativePlanningDigest("spec189-cache-owner-manifest"),
    {}, nativePlanningDigest("spec189-cache-owner-profile")};
  const auto source = std::make_shared<const NativeCanonicalSource>(fixture.source);
  auto publisher = NativeCanonicalPublisherTestAccess::create(
    std::move(transport), options,
    [source] (const NativeInspectedModel&, const NativeRequestControl&) { return source; });
  auto published = fixture.spec;
  published.preparePublication = [publisher = std::move(publisher)]
    (const NativeCanonicalPreparationCatalog&, const NativeInspectedModel& model,
     const NativeRequestControl& control) { return publisher.prepare(model, control); };

  ModelPreparationCache cache(8 << 20, 1, std::chrono::seconds(5));
  std::optional<PreparedModel> prepared{cache.prepare(published)};
  BOOST_REQUIRE_EQUAL(publicationCalls.load(std::memory_order_relaxed), 2U);
  BOOST_REQUIRE(!weakPublicationLease.expired());
  publicationLease.reset();

  // The application handle is the active owner.  A second package cannot evict
  // it while that lease is live, even though the cache has one entry capacity.
  auto replacement = fixture.catalog;
  replacement["source"]["data_name"] = "/fixture/replacement-source";
  auto replacementSpec = fixture.withCatalog(replacement);
  BOOST_CHECK_EXCEPTION(cache.prepare(replacementSpec), std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("BUDGET_EXCEEDED") != std::string::npos;
                        });
  BOOST_CHECK(!weakPublicationLease.expired());

  // Releasing the last PreparedModel view permits the cache to evict the
  // package.  The replacement has no publication receipt, so the old serving
  // lease must disappear with that package instead of being retained by the
  // publisher or a hidden cache index.
  prepared.reset();
  auto evicted = cache.prepare(replacementSpec);
  BOOST_CHECK(!evicted.manifest().preparationKeyDigest.empty());
  BOOST_CHECK_EQUAL(publicationCalls.load(std::memory_order_relaxed), 2U);
  BOOST_CHECK(weakPublicationLease.expired());
}

BOOST_AUTO_TEST_CASE(AsyncWaiterTimeoutUsesASeparateGateAndAllowsReentrantCancel)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  std::function<void()> fireTimer;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  spec.schedule = [&fireTimer] (std::chrono::steady_clock::time_point,
                                std::function<void()> task) {
    fireTimer = std::move(task);
    return [&fireTimer] { fireTimer = {}; };
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::promise<std::string> timeoutCode;
  auto future = timeoutCode.get_future();
  auto subscription = handle.resultAsync(std::chrono::milliseconds(1),
    [&] (std::exception_ptr error, std::optional<PreparedModel>) {
      std::string message = "NONE";
      try {
        if (error)
          std::rethrow_exception(error);
      }
      catch (const std::exception& value) { message = value.what(); }
      timeoutCode.set_value(message);
      // This callback re-enters the owning handle; the timer must not hold
      // the job commit mutex while invoking it.
      handle.cancel();
    });
  BOOST_REQUIRE(static_cast<bool>(fireTimer));
  auto trigger = fireTimer;
  trigger();
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(future.get(), "DI_NATIVE_PREPARATION_RESULT_TIMEOUT");
  subscription.cancel();
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
}

BOOST_AUTO_TEST_CASE(AsyncCompletionCapacityIsExactlyBoundedAtSixtyFour)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::vector<Subscription> subscriptions;
  subscriptions.reserve(64);
  for (unsigned i = 0; i < 64; ++i)
    subscriptions.emplace_back(handle.onCompletion(
      [] (std::exception_ptr, std::optional<PreparedModel>) {}));
  BOOST_CHECK_EXCEPTION(handle.onCompletion(
      [] (std::exception_ptr, std::optional<PreparedModel>) {}), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("SUBSCRIPTION_LIMIT") != std::string::npos;
    });
  for (auto& subscription : subscriptions)
    subscription.cancel();
  handle.cancel();
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
}

BOOST_AUTO_TEST_CASE(DispatchQueueThenThrowStillDeliversExactlyOnce)
{
  Fixture fixture;
  ModelPreparationCache cache(8 << 20, 2, std::chrono::seconds(5));
  std::mutex sourceMutex;
  std::condition_variable sourceCondition;
  bool releaseSource = false;
  std::mutex dispatchMutex;
  std::condition_variable dispatchCondition;
  std::vector<std::function<void()>> queued;
  auto spec = fixture.spec;
  spec.loadSource = [&] (const PreparationSpec&, std::chrono::steady_clock::time_point) {
    std::unique_lock<std::mutex> lock(sourceMutex);
    sourceCondition.wait(lock, [&] { return releaseSource; });
    return fixture.source;
  };
  spec.dispatch = [&] (std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(dispatchMutex);
      queued.push_back(std::move(task));
    }
    dispatchCondition.notify_one();
    throw std::runtime_error("dispatch failure after queue");
  };
  auto handle = Spec185PreparationTestAccess::prepareAsync(cache, spec);
  std::atomic<unsigned> callbackCount{0};
  std::promise<std::string> callbackError;
  auto subscription = handle.onCompletion(
    [&] (std::exception_ptr error, std::optional<PreparedModel>) {
      callbackCount.fetch_add(1, std::memory_order_acq_rel);
      std::string message = "NONE";
      try {
        if (error)
          std::rethrow_exception(error);
      }
      catch (const std::exception& value) {
        message = value.what();
      }
      callbackError.set_value(std::move(message));
    });
  {
    std::lock_guard<std::mutex> lock(sourceMutex);
    releaseSource = true;
  }
  sourceCondition.notify_all();
  auto future = callbackError.get_future();
  BOOST_REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(future.get(), "dispatch failure after queue");
  std::function<void()> queuedTask;
  {
    std::unique_lock<std::mutex> lock(dispatchMutex);
    BOOST_REQUIRE(dispatchCondition.wait_for(lock, std::chrono::seconds(2),
      [&] { return !queued.empty(); }));
    queuedTask = std::move(queued.front());
  }
  queuedTask();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(callbackCount.load(std::memory_order_acquire), 1U);
  subscription.cancel();
  handle.cancel();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
