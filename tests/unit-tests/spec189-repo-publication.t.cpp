#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ndnsf::di;
using namespace ndnsf_distributed_repo;

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
    model.descriptor.modelName = "qwen-fixture";
    model.descriptor.contentDigest = nativePlanningDigest("model-identity");
    model.canonicalSourceDigest = nativePlanningDigest(source.modelBytes.data(),
                                                        source.modelBytes.size());
    model.canonicalSourceBytes = source.modelBytes.size();
    model.canonicalGraphDigest = nativePlanningDigest("graph");
    model.canonicalInitializerBytes = 0;
    model.canonicalInitializerObjectDigest.clear();
    options.artifactRoot = "/spec189/di/artifacts";
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
  BOOST_CHECK_EQUAL(first.manifestDigest, nativePlanningDigest(first.canonicalManifestJson));

  const auto second = provider->publish(
    "qwen-key", "/service", input.model, input.source, input.options, input.control);
  const auto secondStats = provider->stats();
  BOOST_CHECK_EQUAL(secondStats.publicationCalls, 2U);
  BOOST_CHECK_EQUAL(secondStats.publicationHits, 1U);
  BOOST_CHECK_EQUAL(second.rootDataName, first.rootDataName);
  BOOST_CHECK_EQUAL(second.sourceDataName, first.sourceDataName);
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

  auto layered = input.options;
  layered.layerManifestDigests = {nativePlanningDigest("layer-0")};
  BOOST_CHECK_THROW(provider->publish(
    "qwen-key", "/service", input.model, input.source, layered, input.control),
    RepositorySourceError);
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

BOOST_AUTO_TEST_SUITE_END()
