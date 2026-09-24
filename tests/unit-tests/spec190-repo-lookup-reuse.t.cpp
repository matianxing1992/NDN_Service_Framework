#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace ndnsf::di;
using namespace ndnsf_distributed_repo;

std::atomic<int> g_fsyncCalls{0};
std::atomic<int> g_failFsyncAt{0};

int
faultFsync(int fd)
{
  const auto call = ++g_fsyncCalls;
  if (g_failFsyncAt.load(std::memory_order_acquire) == call) {
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}

int
passthroughClose(int fd)
{
  return ::close(fd);
}

struct RepoIoHookScope
{
  ndnsf_distributed_repo::detail::FilesystemRepoStoreIoHooks previous;

  RepoIoHookScope()
    : previous(ndnsf_distributed_repo::detail::installFilesystemRepoStoreIoHooks(
                 {faultFsync, passthroughClose}))
  {
    g_fsyncCalls = 0;
    g_failFsyncAt = 0;
  }

  ~RepoIoHookScope()
  {
    ndnsf_distributed_repo::detail::installFilesystemRepoStoreIoHooks(previous);
  }
};

std::string digest(const std::string& value)
{
  return nativePlanningDigest(value);
}

struct Fixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec190-repo-lookup-" + std::to_string(::getpid()));
  std::shared_ptr<RepoCore> repo;

  std::shared_ptr<RepoCore> openRepo() const
  {
    StorageCapability capability;
    capability.repoNode = "/spec190/local-repo";
    capability.freeBytes = 16U * 1024U * 1024U;
    capability.repoMode = "persistent";
    return std::make_shared<RepoCore>(
      std::move(capability), makeFilesystemRepoStore(root.string(), 4U * 1024U * 1024U,
                                                     1U * 1024U * 1024U, "spec190-test"));
  }

  Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error)
      throw std::runtime_error("unable to make Repo fixture root private: " + error.message());
    repo = openRepo();
  }

  ~Fixture()
  {
    repo.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove(root.string() + ".authority.lock", error);
  }
};

NativeModelDescriptor modelDescriptor()
{
  NativeModelDescriptor model;
  model.modelName = "qwen-lookup-fixture";
  model.contentDigest = digest("model-content");
  model.semanticsDigest = digest("model-semantics");
  model.graphDigest = digest("planning-graph");
  model.modelFormat = "onnx";
  model.precision = "float32";
  model.sourceRevision = "r1";
  model.adapter.name = "qwen";
  model.adapter.version = "1";
  model.adapter.stateDigest = digest("adapter-state");
  model.adapter.abi = "qwen-abi";
  model.adapter.modelFormats = {"onnx"};
  model.adapter.tasks = {"generation"};
  model.adapter.backends = {"onnxruntime-cpu"};
  model.adapter.precisions = {"float32"};
  model.adapter.inputSchemaDigest = digest("input");
  model.adapter.optionsSchemaDigest = digest("options");
  model.adapter.resultSchemaDigest = digest("result");
  model.adapter.graphSchemaDigest = digest("graph-schema");
  model.adapter.splitSchemaDigest = digest("split-schema");
  model.adapter.stateSchemaDigest = digest("state-schema");
  model.adapter.graphInspectable = true;
  model.adapter.splittable = true;
  model.adapter.deterministicAnalysis = true;
  model.adapterId = model.adapter.name;
  model.adapterVersion = model.adapter.version;
  return model;
}

NativeJson catalogFor(const NativeModelDescriptor& model, const std::string& profile)
{
  return NativeJson{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", NativeJson::parse(model.canonicalJson())},
    {"source", {{"data_name", "/spec190/source/qwen"},
                 {"digest", digest("source-bytes")},
                 {"model_manifest_digest", digest("package")},
                 {"canonical_graph_digest", digest("graph")}}},
    {"recipe", {{"artifact_profile_digest", profile},
                 {"assembler_descriptor_digest", digest("assembler")},
                 {"backend_abi", "onnxruntime-cpu-v1"}, {"precision", "float32"},
                 {"quantization", "none"}, {"layout", "native"}, {"padding", "none"},
                 {"protection_epoch", "epoch-1"}, {"max_source_bytes", 1U << 20},
                 {"max_assembled_bytes", 1U << 20}, {"max_nodes", 64}}},
    {"publication", {{"artifact_root", "/spec190/di/artifacts"},
                      {"package_manifest_digest", digest("package")}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
    {"splitter", {{"kind", "QWEN"}, {"layer_ranges", {{0, 1}}},
                   {"artifact_digests_by_role", {{"stage-0", digest("stage")}}},
                   {"weight_bytes_by_role", {{"stage-0", 1}}},
                   {"roles", {"stage-0"}}, {"tensor_degrees", {1}},
                   {"input_ingress_role", "stage-0"}, {"result_egress_role", "stage-0"}}},
    {"node_mapping", NativeJson::object()}, {"state_inputs", NativeJson::object()},
    {"state_outputs", NativeJson::object()}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190RepoLookupReuse)

BOOST_AUTO_TEST_CASE(CommittedReceiptIsFoundBeforeAnotherPublication)
{
  Fixture fixture;
  NativeInspectedModel model;
  model.descriptor = modelDescriptor();
  NativeCanonicalSource source;
  source.modelBytes = {0x01, 0x02, 0x03};
  source.preparedMetadataJson = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-prepared-metadata-v1"}, {"fixture", "external-root-last"}});
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = digest("graph");
  model.modelManifestDigest = digest("package");

  const auto profile = digest("profile");
  auto catalog = catalogFor(model.descriptor, profile);
  catalog["source"]["digest"] = model.canonicalSourceDigest;
  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec190/di/artifacts";
  options.packageManifestDigest = digest("package");
  options.artifactProfileDigest = profile;
  options.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(catalog));
  NativeRequestControl control{
    "/spec190/lookup", 1, std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
  auto provider = std::make_unique<RepoSourceProvider>(fixture.repo);
  const auto committed = provider->publish("qwen", "/service", model, source, options, control);
  BOOST_REQUIRE_EQUAL(provider->stats().publicationCalls, 1U);

  const auto hit = provider->lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(hit);
  BOOST_CHECK_EQUAL(hit->rootDataName, committed.rootDataName);
  BOOST_CHECK_EQUAL(hit->sourceDataName, committed.sourceDataName);
  BOOST_CHECK_EQUAL(hit->preparedMetadataJson, source.preparedMetadataJson);
  BOOST_CHECK_EQUAL(hit->preparedMetadataDataName, committed.preparedMetadataDataName);
  BOOST_CHECK(fixture.repo->has(committed.preparedMetadataDataName));
  BOOST_CHECK_EQUAL(provider->stats().publicationCalls, 1U);

  auto changed = catalog;
  changed["recipe"]["artifact_profile_digest"] = digest("different-profile");
  BOOST_CHECK(!provider->lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(changed), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)}));

  auto changedInitializer = catalog;
  changedInitializer["source"]["initializer_digest"] = digest("different-initializer");
  BOOST_CHECK(!provider->lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(changedInitializer), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)}));

  auto changedLayer = catalog;
  changedLayer["publication"]["layer_manifest_digests"] = {digest("different-layer")};
  BOOST_CHECK(!provider->lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(changedLayer), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)}));

  // Reopen the same fixed root through a fresh RepoCore/provider owner. This
  // is the close/reopen analogue of the process boundary and must not publish.
  provider.reset();
  fixture.repo.reset();
  fixture.repo = fixture.openRepo();
  RepoSourceProvider restarted(fixture.repo);
  const auto reopened = restarted.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(reopened);
  BOOST_CHECK_EQUAL(reopened->rootDataName, committed.rootDataName);
  BOOST_CHECK_EQUAL(restarted.stats().publicationCalls, 0U);

  // A receipt recovered by a later run is a durable reference, not an owned
  // staging transaction.  Stale cleanup must therefore be a no-op and leave
  // every committed object readable.
  const auto committedObjects = fixture.repo->list();
  restarted.rollback(*reopened);
  for (const auto& object : committedObjects)
    BOOST_CHECK_MESSAGE(fixture.repo->has(object.objectName),
                        "stale cleanup removed committed object " + object.objectName);

  std::atomic<bool> cancelled{true};
  BOOST_CHECK_THROW(restarted.publish(
                      "qwen", "/service", model, source,
                      options,
                      NativeRequestControl{
                        "/spec190/cancelled", 1,
                        std::chrono::steady_clock::now() + std::chrono::seconds(10),
                        [&cancelled] { return cancelled.load(std::memory_order_acquire); }}),
                    std::exception);
  BOOST_CHECK(restarted.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)}));
}

BOOST_AUTO_TEST_CASE(MissingDependencyFallsBackToRootLastRepair)
{
  Fixture fixture;
  NativeInspectedModel model;
  model.descriptor = modelDescriptor();
  NativeCanonicalSource source;
  source.modelBytes = {0x21, 0x22, 0x23, 0x24};
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = digest("graph-missing");
  model.modelManifestDigest = digest("package-missing");

  const auto profile = digest("profile-missing");
  auto catalog = catalogFor(model.descriptor, profile);
  catalog["source"]["digest"] = model.canonicalSourceDigest;
  catalog["source"]["canonical_graph_digest"] = model.canonicalGraphDigest;
  catalog["source"]["model_manifest_digest"] = model.modelManifestDigest;
  catalog["publication"]["package_manifest_digest"] = model.modelManifestDigest;
  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec190/di/artifacts";
  options.packageManifestDigest = model.modelManifestDigest;
  options.artifactProfileDigest = profile;
  options.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(catalog));
  NativeRequestControl control{
    "/spec190/missing", 1, std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
  RepoSourceProvider provider(fixture.repo);
  const auto committed = provider.publish("qwen", "/service", model, source, options, control);
  const auto committedObjectCount = fixture.repo->list().size();
  BOOST_REQUIRE(fixture.repo->remove(committed.sourceDataName));

  const auto miss = provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(miss);
  BOOST_REQUIRE_EQUAL(miss->missingDataNames.size(), 1U);
  BOOST_CHECK_EQUAL(miss->missingDataNames.front(), committed.sourceDataName);

  const auto repaired = provider.publish("qwen", "/service", model, source, options,
                                         NativeRequestControl{
                                           "/spec190/repair", 1,
                                           std::chrono::steady_clock::now() + std::chrono::seconds(10), {}});
  BOOST_CHECK_EQUAL(repaired.rootDataName, committed.rootDataName);
  BOOST_CHECK(fixture.repo->has(committed.sourceDataName));
  BOOST_CHECK(fixture.repo->has(committed.rootDataName));
  BOOST_CHECK_EQUAL(fixture.repo->list().size(), committedObjectCount);
  BOOST_CHECK_EQUAL(provider.stats().publicationHits, 0U);
  BOOST_CHECK_EQUAL(provider.stats().publicationCalls, 2U);

  const auto hit = provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(hit);
  BOOST_CHECK_EQUAL(hit->rootDataName, committed.rootDataName);

  BOOST_REQUIRE(fixture.repo->remove(committed.sourceDataName));
  fixture.repo->put(committed.sourceDataName, {0xee}, "corrupt-source");
  BOOST_CHECK_THROW(provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)}),
                    RepositorySourceError);
  BOOST_CHECK_EQUAL(provider.stats().publicationCalls, 2U);
}

BOOST_AUTO_TEST_CASE(LayerDependencyRepairReportsOnlyMissingLayer)
{
  Fixture fixture;
  NativeInspectedModel model;
  model.descriptor = modelDescriptor();
  NativeCanonicalSource source;
  source.modelBytes = {0x41, 0x42, 0x43, 0x44};
  NativeCanonicalSource::LayerPayload firstLayer;
  firstLayer.stageIndex = 0;
  firstLayer.layerBegin = 0;
  firstLayer.layerEnd = 1;
  firstLayer.bytes = {0xa1, 0xa2};
  firstLayer.digest = nativePlanningDigest(firstLayer.bytes.data(), firstLayer.bytes.size());
  source.layerPayloads.push_back(firstLayer);
  NativeCanonicalSource::LayerPayload secondLayer;
  secondLayer.stageIndex = 1;
  secondLayer.layerBegin = 1;
  secondLayer.layerEnd = 2;
  secondLayer.bytes = {0xb1, 0xb2, 0xb3};
  secondLayer.digest = nativePlanningDigest(secondLayer.bytes.data(), secondLayer.bytes.size());
  source.layerPayloads.push_back(secondLayer);
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = digest("graph-layer-missing");
  model.modelManifestDigest = digest("package-layer-missing");

  const auto profile = digest("profile-layer-missing");
  auto catalog = catalogFor(model.descriptor, profile);
  catalog["source"]["digest"] = model.canonicalSourceDigest;
  catalog["source"]["canonical_graph_digest"] = model.canonicalGraphDigest;
  catalog["source"]["model_manifest_digest"] = model.modelManifestDigest;
  catalog["publication"]["package_manifest_digest"] = model.modelManifestDigest;
  catalog["publication"]["layer_manifest_digests"] =
    {firstLayer.digest, secondLayer.digest};
  catalog["splitter"]["layer_ranges"] = {{0, 1}, {1, 2}};
  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec190/di/artifacts";
  options.packageManifestDigest = model.modelManifestDigest;
  options.artifactProfileDigest = profile;
  options.layerManifestDigests = {firstLayer.digest, secondLayer.digest};
  options.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(catalog));
  RepoSourceProvider provider(fixture.repo);
  const auto committed = provider.publish(
    "qwen", "/service", model, source, options,
    NativeRequestControl{
      "/spec190/layer-missing", 1,
      std::chrono::steady_clock::now() + std::chrono::seconds(10), {}});
  BOOST_REQUIRE_EQUAL(committed.layerDataNames.size(), 2U);
  BOOST_REQUIRE(fixture.repo->has(committed.layerDataNames.at(0)));
  BOOST_REQUIRE(fixture.repo->has(committed.layerDataNames.at(1)));

  BOOST_REQUIRE(fixture.repo->remove(committed.layerDataNames.at(1)));
  const auto partial = provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(partial);
  BOOST_REQUIRE_EQUAL(partial->missingDataNames.size(), 1U);
  BOOST_CHECK_EQUAL(partial->missingDataNames.front(), committed.layerDataNames.at(1));
  BOOST_CHECK(fixture.repo->has(committed.layerDataNames.at(0)));
  BOOST_CHECK(fixture.repo->has(committed.sourceDataName));
  BOOST_CHECK(fixture.repo->has(committed.rootDataName));

  const auto repaired = provider.publish(
    "qwen", "/service", model, source, options,
    NativeRequestControl{
      "/spec190/layer-repair", 1,
      std::chrono::steady_clock::now() + std::chrono::seconds(10), {}});
  BOOST_CHECK_EQUAL(repaired.rootDataName, committed.rootDataName);
  BOOST_CHECK(fixture.repo->has(committed.layerDataNames.at(0)));
  BOOST_CHECK(fixture.repo->has(committed.layerDataNames.at(1)));
  BOOST_CHECK_EQUAL(provider.stats().publicationCalls, 2U);

  const auto hit = provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(hit);
  BOOST_CHECK(hit->missingDataNames.empty());
  BOOST_CHECK_EQUAL(hit->layerDataNames.size(), 2U);
}

BOOST_AUTO_TEST_CASE(PublicationFailureDoesNotRemoveAnOlderCommittedReceipt)
{
  Fixture fixture;
  NativeInspectedModel model;
  model.descriptor = modelDescriptor();
  NativeCanonicalSource source;
  source.modelBytes = {0x31, 0x32, 0x33};
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = digest("graph-fault");
  model.modelManifestDigest = digest("package-fault");

  const auto profile = digest("profile-fault-old");
  auto catalog = catalogFor(model.descriptor, profile);
  catalog["source"]["digest"] = model.canonicalSourceDigest;
  catalog["source"]["canonical_graph_digest"] = model.canonicalGraphDigest;
  catalog["source"]["model_manifest_digest"] = model.modelManifestDigest;
  catalog["publication"]["package_manifest_digest"] = model.modelManifestDigest;
  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec190/di/artifacts";
  options.packageManifestDigest = model.modelManifestDigest;
  options.artifactProfileDigest = profile;
  options.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(catalog));
  RepoSourceProvider provider(fixture.repo);
  provider.publish("qwen", "/service", model, source, options,
                   NativeRequestControl{
                     "/spec190/old", 1,
                     std::chrono::steady_clock::now() + std::chrono::seconds(10), {}});

  auto changedCatalog = catalog;
  changedCatalog["recipe"]["artifact_profile_digest"] = digest("profile-fault-new");
  NativeCanonicalPublicationOptions changedOptions = options;
  changedOptions.artifactProfileDigest = changedCatalog["recipe"]["artifact_profile_digest"];
  changedOptions.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(changedCatalog));
  const auto changedRoot = std::string("/spec190/di/artifacts/prepared/") +
    changedOptions.publicationIdentityDigest.substr(7) + "/manifest";

  {
    RepoIoHookScope hooks;
    g_failFsyncAt = 1;
    BOOST_CHECK_THROW(provider.publish(
                        "qwen", "/service", model, source, changedOptions,
                        NativeRequestControl{
                          "/spec190/fault", 1,
                          std::chrono::steady_clock::now() + std::chrono::seconds(10), {}}),
                      std::exception);
  }

  const auto oldHit = provider.lookupPrepared({
    "qwen", "/service", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(oldHit);
  BOOST_CHECK(!fixture.repo->has(changedRoot));
  BOOST_CHECK(fixture.repo->has(oldHit->rootDataName));
}

BOOST_AUTO_TEST_CASE(ConcurrentLookupAndPublishObserveOneCommitBoundary)
{
  Fixture fixture;
  NativeInspectedModel model;
  model.descriptor = modelDescriptor();
  NativeCanonicalSource source;
  source.modelBytes = {0x11, 0x12, 0x13, 0x14};
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = digest("graph-race");
  model.modelManifestDigest = digest("package-race");

  const auto profile = digest("profile-race");
  auto catalog = catalogFor(model.descriptor, profile);
  catalog["source"]["digest"] = model.canonicalSourceDigest;
  catalog["source"]["canonical_graph_digest"] = model.canonicalGraphDigest;
  catalog["source"]["model_manifest_digest"] = model.modelManifestDigest;
  catalog["publication"]["package_manifest_digest"] = model.modelManifestDigest;
  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec190/di/artifacts";
  options.packageManifestDigest = model.modelManifestDigest;
  options.artifactProfileDigest = profile;
  options.publicationIdentityDigest = nativePlanningDigest(nativeCanonicalJson(catalog));
  const auto catalogJson = nativeCanonicalJson(catalog);
  auto provider = std::make_shared<RepoSourceProvider>(fixture.repo);

  std::atomic<bool> start{false};
  std::exception_ptr publisherError;
  std::exception_ptr lookupError;
  std::thread publisher([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    try {
      provider->publish("qwen", "/service", model, source, options,
                        NativeRequestControl{
                          "/spec190/race-publish", 1,
                          std::chrono::steady_clock::now() + std::chrono::seconds(10), {}});
    }
    catch (...) {
      publisherError = std::current_exception();
    }
  });
  std::thread lookup([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    try {
      (void)provider->lookupPrepared({
        "qwen", "/service", catalogJson, 1U << 20,
        std::chrono::steady_clock::now() + std::chrono::seconds(10)});
    }
    catch (...) {
      lookupError = std::current_exception();
    }
  });
  start.store(true, std::memory_order_release);
  publisher.join();
  lookup.join();
  BOOST_REQUIRE_MESSAGE(!publisherError, "concurrent publication unexpectedly failed");
  BOOST_REQUIRE_MESSAGE(!lookupError,
                        "concurrent lookup observed a partial publication");

  const auto committed = provider->lookupPrepared({
    "qwen", "/service", catalogJson, 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)});
  BOOST_REQUIRE(committed);
  BOOST_CHECK_EQUAL(provider->stats().publicationCalls, 1U);
}

BOOST_AUTO_TEST_SUITE_END()
