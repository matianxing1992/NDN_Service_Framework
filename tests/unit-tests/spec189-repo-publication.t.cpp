#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ndnsf::di;
using namespace ndnsf_distributed_repo;

std::vector<std::uint8_t> unhex(const std::string& text)
{
  std::vector<std::uint8_t> result;
  result.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return result;
}

struct RepoFixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec189-repo-publication-" + std::to_string(::getpid()));
  std::shared_ptr<RepoCore> repo;

  RepoFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::permissions(
      root, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, error);
    if (error)
      throw std::runtime_error("unable to make Repo fixture root private: " + error.message());
    StorageCapability capability;
    capability.repoNode = "/spec189/local-repo";
    capability.freeBytes = 16U * 1024U * 1024U;
    capability.repoMode = "persistent";
    repo = std::make_shared<RepoCore>(
      std::move(capability),
      makeFilesystemRepoStore(root.string(), 4U * 1024U * 1024U, 1U * 1024U * 1024U,
                              "spec189-test"));
  }

  ~RepoFixture()
  {
    repo.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove(root.string() + ".authority.lock", error);
  }
};

struct PublicationInput
{
  NativeInspectedModel model;
  NativeCanonicalSource source;
  NativeCanonicalPublicationOptions options;
  NativeRequestControl control{
    "/spec189/request", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};

  PublicationInput()
  {
    source.modelBytes = {0x01, 0x02, 0x03, 0x04};
    source.layerPayloads = {
      {0, 0, 14, {""}, {0x11, 0x12, 0x13}},
      {1, 14, 28, {""}, {0x21, 0x22, 0x23, 0x24}}};
    source.layerPayloads[0].digest = nativePlanningDigest(
      source.layerPayloads[0].bytes.data(), source.layerPayloads[0].bytes.size());
    source.layerPayloads[1].digest = nativePlanningDigest(
      source.layerPayloads[1].bytes.data(), source.layerPayloads[1].bytes.size());
    model.descriptor.modelName = "qwen-fixture";
    model.descriptor.contentDigest = nativePlanningDigest("model-identity");
    model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(),
                                                        source.modelBytes.size());
    model.canonicalSourceBytes = source.modelBytes.size();
    model.canonicalGraphDigest = nativePlanningDigest("graph");
    model.canonicalInitializerBytes = 0;
    model.canonicalInitializerObjectDigest.clear();
    options.artifactRoot = "/spec189/di/artifacts";
    options.layerManifestDigests = {source.layerPayloads[0].digest,
                                    source.layerPayloads[1].digest};
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec189RepoPublication)

BOOST_AUTO_TEST_CASE(PreparePublicationCommitsAndReusesCanonicalReceipt)
{
  RepoFixture fixture;
  PublicationInput input;
  auto provider = std::make_shared<RepoSourceProvider>(fixture.repo);

  const auto first = provider->publish(
    "qwen-key", "/service", input.model, input.source, input.options, input.control);
  const auto firstStats = provider->stats();
  BOOST_REQUIRE_EQUAL(firstStats.publicationCalls, 1U);
  BOOST_REQUIRE_EQUAL(firstStats.publicationHits, 0U);
  BOOST_REQUIRE(fixture.repo->has(first.rootDataName));
  BOOST_REQUIRE(fixture.repo->has(first.sourceDataName));
  BOOST_REQUIRE_EQUAL(first.layerDataNames.size(), 2U);
  BOOST_REQUIRE_EQUAL_COLLECTIONS(first.layerManifestDigests.begin(), first.layerManifestDigests.end(),
                                  input.options.layerManifestDigests.begin(),
                                  input.options.layerManifestDigests.end());
  BOOST_REQUIRE(fixture.repo->has(first.layerDataNames.at(0)));
  BOOST_REQUIRE(fixture.repo->has(first.layerDataNames.at(1)));
  BOOST_CHECK_EQUAL(first.manifestDigest, nativePlanningDigest(first.canonicalManifestJson));

  const auto second = provider->publish(
    "qwen-key", "/service", input.model, input.source, input.options, input.control);
  const auto secondStats = provider->stats();
  BOOST_CHECK_EQUAL(secondStats.publicationCalls, 2U);
  BOOST_CHECK_EQUAL(secondStats.publicationHits, 1U);
  BOOST_CHECK_EQUAL(second.rootDataName, first.rootDataName);
  BOOST_CHECK_EQUAL(second.sourceDataName, first.sourceDataName);
  BOOST_CHECK(second.layerDataNames == first.layerDataNames);
  BOOST_CHECK_EQUAL(fixture.repo->getManifest(first.rootDataName).size,
                    first.canonicalManifestJson.size());

  fixture.repo->put(first.sourceDataName, {0xaa, 0xbb}, "foreign-source");
  BOOST_CHECK_THROW(provider->publish(
    "qwen-key", "/service", input.model, input.source, input.options, input.control),
    RepositorySourceError);

  BOOST_CHECK_THROW(provider->publish(
    "foreign-key", "/service", input.model, input.source, input.options, input.control),
    RepositorySourceError);

  // A committed Repo receipt is durable and reusable; a later cache/package
  // failure must not remove objects that another prepare can already use.
  provider->rollback(first);
  BOOST_CHECK(fixture.repo->has(first.rootDataName));
  BOOST_CHECK(fixture.repo->has(first.sourceDataName));
  BOOST_CHECK(fixture.repo->has(first.layerDataNames.at(0)));
  BOOST_CHECK(fixture.repo->has(first.layerDataNames.at(1)));

  auto corruptedLayer = input;
  corruptedLayer.source.layerPayloads[1].bytes[0] ^= 0xff;
  BOOST_CHECK_THROW(provider->publish(
    "qwen-key", "/service", corruptedLayer.model, corruptedLayer.source,
    corruptedLayer.options, corruptedLayer.control), RepositorySourceError);

  auto mismatchedLayer = input;
  mismatchedLayer.options.layerManifestDigests.pop_back();
  BOOST_CHECK_THROW(provider->publish(
    "qwen-key", "/service", mismatchedLayer.model, mismatchedLayer.source,
    mismatchedLayer.options, mismatchedLayer.control), RepositorySourceError);
}

BOOST_AUTO_TEST_CASE(CancelledPrepareDoesNotLeaveRepoObjects)
{
  RepoFixture fixture;
  PublicationInput input;
  input.control.cancelled = [] { return true; };
  RepoSourceProvider provider(fixture.repo);

  BOOST_CHECK_THROW(provider.publish(
    "qwen-key", "/service", input.model, input.source, input.options, input.control),
    std::runtime_error);
  BOOST_CHECK(fixture.repo->list().empty());
}

BOOST_AUTO_TEST_CASE(RepoSourceMissReusesValidatedInitializer)
{
  RepoFixture fixture;
  const std::vector<std::uint8_t> modelBytes{0x01, 0x02, 0x03, 0x04};
  const std::vector<std::uint8_t> initializerBytes{0xa1, 0xa2, 0xa3};
  const auto sourceName = std::string{"/spec189/source/qwen"};
  const auto initializerName = sourceName + "/initializer";
  const auto sourceDigest = nativePlanningDigest(modelBytes.data(), modelBytes.size());
  const auto initializerDigest = nativePlanningDigest(
    initializerBytes.data(), initializerBytes.size());
  const NativeJson catalog = {
    {"source", {
      {"data_name", sourceName},
      {"digest", sourceDigest},
      {"bytes", modelBytes.size()},
      {"initializer_data_name", initializerName},
      {"initializer_digest", initializerDigest},
    }},
  };
  RepositorySourceRequest request{
    "default", nativeCanonicalJson(catalog), 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)};
  std::size_t fallbackCalls = 0;
  const auto fallback = [&] (const RepositorySourceRequest&) {
    ++fallbackCalls;
    NativeCanonicalSource source;
    source.modelBytes = modelBytes;
    source.initializerBytes = initializerBytes;
    return source;
  };
  RepoSourceProvider provider(fixture.repo);

  const auto first = provider.load(request, fallback);
  BOOST_CHECK_EQUAL(fallbackCalls, 1U);
  BOOST_REQUIRE(first.initializerBytes.has_value());
  BOOST_CHECK_EQUAL_COLLECTIONS(first.initializerBytes->begin(), first.initializerBytes->end(),
                               initializerBytes.begin(), initializerBytes.end());
  BOOST_REQUIRE(fixture.repo->has(sourceName));
  BOOST_REQUIRE(fixture.repo->has(initializerName));

  const auto second = provider.load(request, [&] (const RepositorySourceRequest&) {
    ++fallbackCalls;
    throw std::runtime_error("fallback must not run after Repo ingest");
    return NativeCanonicalSource{};
  });
  BOOST_CHECK_EQUAL(fallbackCalls, 1U);
  BOOST_REQUIRE(second.initializerBytes.has_value());
  BOOST_CHECK_EQUAL_COLLECTIONS(second.initializerBytes->begin(), second.initializerBytes->end(),
                               initializerBytes.begin(), initializerBytes.end());
}

BOOST_AUTO_TEST_CASE(LegacyCanonicalReceiptWithoutLayersStillReuses)
{
  RepoFixture fixture;
  PublicationInput input;
  input.source.layerPayloads.clear();
  input.options.layerManifestDigests.clear();
  auto provider = std::make_shared<RepoSourceProvider>(fixture.repo);

  const auto first = provider->publish(
    "legacy-key", "/service", input.model, input.source, input.options, input.control);
  const auto second = provider->publish(
    "legacy-key", "/service", input.model, input.source, input.options, input.control);
  BOOST_CHECK(second.layerDataNames.empty());
  BOOST_CHECK(second.layerManifestDigests.empty());
  BOOST_CHECK_EQUAL(provider->stats().publicationHits, 1U);
  BOOST_CHECK(fixture.repo->has(first.rootDataName));
}

BOOST_AUTO_TEST_CASE(MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption)
{
  RepoFixture fixture;
  std::ifstream stream("tests/fixtures/spec182/dependency-probes/extraction-vectors.json");
  BOOST_REQUIRE(stream.good());
  const auto vectors = NativeJson::parse(stream);
  const auto& row = vectors.at("cases").at(1);
  NativeCanonicalSource source;
  source.modelBytes = unhex(row.at("modelHex").get<std::string>());
  source.initializerBytes = unhex(row.at("initializerHex").get<std::string>());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  NativeAssemblyControl assemblyControl{
    deadline, [] {}, 1U << 20, 1U << 20};
  source.materialManifest = deriveNativeCanonicalMaterialManifest(source, assemblyControl);
  const auto identity = canonicalOnnxSourceIdentity(source, assemblyControl);

  NativeInspectedModel model;
  model.descriptor.modelName = "qwen-material-fixture";
  model.descriptor.contentDigest = nativePlanningDigest("qwen-material-model");
  model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  model.canonicalSourceBytes = source.modelBytes.size();
  model.canonicalGraphDigest = identity.graphDigest;
  model.canonicalInitializerBytes = source.initializerBytes->size();
  model.canonicalInitializerObjectDigest = nativePlanningDigest(
    source.initializerBytes->data(), source.initializerBytes->size());
  model.canonicalInitializerDigest = identity.initializerDigest;

  NativeCanonicalPublicationOptions options;
  options.artifactRoot = "/spec189/di/material-artifacts";
  options.packageManifestDigest = nativePlanningDigest("qwen-material-package");
  options.artifactProfileDigest = nativePlanningDigest("qwen-material-profile");
  options.maxPublicationBytes = 1U << 20;
  NativeRequestControl control{
    "/spec189/material-request", 1, deadline, {}};
  RepoSourceProvider provider(fixture.repo);
  const auto first = provider.publish(
    "qwen-material-key", "/service", model, source, options, control);
  BOOST_REQUIRE(!first.materialManifestDataName.empty());
  BOOST_REQUIRE(!first.materialPayloadIds.empty());
  BOOST_REQUIRE_EQUAL(first.materialPayloadIds.size(), first.materialDataNames.size());
  BOOST_REQUIRE_EQUAL(first.materialPayloadIds.size(), first.materialDigests.size());
  BOOST_REQUIRE(fixture.repo->has(first.materialManifestDataName));
  for (const auto& name : first.materialDataNames)
    BOOST_REQUIRE(fixture.repo->has(name));
  BOOST_REQUIRE(fixture.repo->has(first.rootDataName));

  const auto second = provider.publish(
    "qwen-material-key", "/service", model, source, options, control);
  BOOST_CHECK_EQUAL(provider.stats().publicationHits, 1U);
  BOOST_CHECK_EQUAL(second.materialManifestDataName, first.materialManifestDataName);
  BOOST_CHECK(second.materialDataNames == first.materialDataNames);

  // The post-ACK consumer reads the authenticated index and one selected node
  // without falling back to the complete canonical source/initializer.
  const auto selected = provider.loadMaterialSelection(
    first.rootDataName, first.manifestDigest, {0}, 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10));
  BOOST_REQUIRE(selected.manifest);
  BOOST_REQUIRE(selected.manifest->payloads.empty());
  BOOST_REQUIRE(!selected.payloads.empty());
  BOOST_CHECK_LT(selected.payloads.size(), first.materialPayloadIds.size());
  NativeCanonicalSource selectedSource;
  selectedSource.materialManifest = selected.manifest;
  selectedSource.materialPayloads = selected.payloads;
  NativeAssemblyControl selectedControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1U << 20, 1U << 20};
  const auto selectedModel = materializeNativeCanonicalModel(
    selectedSource, {0}, selectedControl);
  BOOST_CHECK(!selectedModel.empty());
  BOOST_CHECK_THROW(provider.loadMaterialSelection(
    first.rootDataName, first.manifestDigest, {1, 0}, 1U << 20,
    std::chrono::steady_clock::now() + std::chrono::seconds(10)), RepositorySourceError);

  const auto original = fixture.repo->get(first.materialDataNames.front());
  fixture.repo->put(first.materialDataNames.front(), {0xaa, 0xbb}, "foreign-material");
  BOOST_CHECK_THROW(provider.publish(
    "qwen-material-key", "/service", model, source, options, control),
    RepositorySourceError);
  BOOST_CHECK(fixture.repo->has(first.rootDataName));
  BOOST_CHECK(fixture.repo->get(first.materialDataNames.front()) != original);
}

BOOST_AUTO_TEST_SUITE_END()
