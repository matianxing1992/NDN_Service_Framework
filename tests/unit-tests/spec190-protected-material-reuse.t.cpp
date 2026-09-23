#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {

using namespace ndn_service_framework;
using namespace ndnsf_distributed_repo;

struct Fixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec190-protected-material-reuse-" + std::to_string(::getpid()));
  std::shared_ptr<RepoCore> repo;

  Fixture()
  {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
    StorageCapability capability;
    capability.repoNode = "/spec190/protected-material";
    capability.freeBytes = 16U << 20;
    repo = std::make_shared<RepoCore>(capability,
      makeFilesystemRepoStore((root / "objects").string(), 1U << 20, 1U << 20));
  }

  ~Fixture()
  {
    repo.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path input(const std::string& name, const std::string& bytes = "ciphertext")
  {
    const auto path = root / name;
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
  }
};

EncryptedLargeDataCommitOptions
durableOptions()
{
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "/protected/publication/qwen/v1";
  options.protectionEpoch = "grant-epoch-7";
  options.keyReferenceId = "key-reference-digest-7";
  options.keyReferenceVersion = "v1";
  options.ciphertextManifestDigest = "sha256:ciphertext-manifest-7";
  options.servingLocator = "/protected/durable";
  options.contentDigest = "sha256:plaintext-content-7";
  options.plaintextSize = 9;
  return options;
}

class LegacyTransientOnlyStore final : public EncryptedLargeDataRangeStore
{
public:
  using EncryptedLargeDataRangeStore::commitFile;
  std::shared_ptr<const EncryptedLargeDataRangeSource>
  commitFile(const std::string&, const std::filesystem::path&, std::uint64_t,
             const std::function<void()>&) override
  {
    return {};
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190ProtectedMaterialReuse)

BOOST_AUTO_TEST_CASE(LegacyCommitRemainsTransient)
{
  Fixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto path = fixture.input("transient.bin");
  auto source = store.commitFile("/protected/transient", path, 10);
  BOOST_REQUIRE(source);
  BOOST_CHECK(!source->isDurable());
  BOOST_CHECK(fixture.repo->has("/protected/transient"));
  source.reset();
  BOOST_CHECK(!fixture.repo->has("/protected/transient"));
}

BOOST_AUTO_TEST_CASE(DurableSourceSurvivesRequestLeaseDestruction)
{
  Fixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto path = fixture.input("durable.bin");
  const auto options = durableOptions();
  auto source = store.commitFile("/protected/durable", path, 10, options);
  BOOST_REQUIRE(source);
  BOOST_CHECK(source->isDurable());
  BOOST_CHECK_EQUAL(source->read(0, 10).size(), 10U);
  const auto manifest = fixture.repo->getManifest("/protected/durable");
  source.reset();
  BOOST_CHECK(fixture.repo->has("/protected/durable"));

  // Core/Repo owner performs explicit invalidation after the request lease is gone.
  BOOST_CHECK(fixture.repo->removeIfCurrent(manifest));
  BOOST_CHECK(!fixture.repo->has("/protected/durable"));
}

BOOST_AUTO_TEST_CASE(ExplicitReleaseIsIdempotentAndGenerationFenced)
{
  Fixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto path = fixture.input("replace.bin");
  const auto options = durableOptions();
  auto old = store.commitFile("/protected/replace", path, 10, options);
  const auto oldManifest = fixture.repo->getManifest("/protected/replace");
  BOOST_REQUIRE(fixture.repo->removeIfCurrent(oldManifest));
  auto replacement = store.commitFile("/protected/replace", path, 10, options);
  const auto replacementManifest = fixture.repo->getManifest("/protected/replace");
  BOOST_REQUIRE_NE(oldManifest.operationId, replacementManifest.operationId);

  old->release();
  old->release();
  BOOST_CHECK(fixture.repo->has("/protected/replace"));
  BOOST_CHECK_EQUAL(replacement->read(0, 10).size(), 10U);
  replacement->release();
  BOOST_CHECK(!fixture.repo->has("/protected/replace"));
}

BOOST_AUTO_TEST_CASE(DurableCancellationLeavesNoCommittedObject)
{
  Fixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto path = fixture.input("cancel.bin", std::string(2U << 20, 'c'));
  const auto options = durableOptions();
  unsigned checks = 0;
  BOOST_CHECK_THROW(store.commitFile("/protected/cancel", path, 2U << 20, options, [&] {
    if (++checks == 5)
      throw std::runtime_error("fixture-cancelled");
  }), std::runtime_error);
  BOOST_CHECK(!fixture.repo->has("/protected/cancel"));
  BOOST_CHECK_GT(checks, 0U);
}

BOOST_AUTO_TEST_CASE(UnsupportedDurableAdapterFailsClosed)
{
  LegacyTransientOnlyStore store;
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  BOOST_CHECK_THROW(store.commitFile("/protected/unsupported", {}, 1, options),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableIdentityMetadataSurvivesRepoOwnerRestart)
{
  Fixture fixture;
  const auto path = fixture.input("restart.bin");
  const auto options = durableOptions();
  RepoObjectManifest committed;
  {
    RepoEncryptedLargeDataStore store(fixture.repo);
    auto source = store.commitFile("/protected/restart", path, 10, options);
    BOOST_REQUIRE(source);
    committed = fixture.repo->getManifest("/protected/restart");
    BOOST_CHECK_EQUAL(committed.publicationIdentity, options.publicationIdentity);
    BOOST_CHECK_EQUAL(committed.protectionEpoch, options.protectionEpoch);
    BOOST_CHECK_EQUAL(committed.keyReferenceId, options.keyReferenceId);
    BOOST_CHECK_EQUAL(committed.keyReferenceVersion, options.keyReferenceVersion);
    BOOST_CHECK_EQUAL(committed.ciphertextManifestDigest,
                      options.ciphertextManifestDigest);
    BOOST_CHECK_EQUAL(committed.servingLocator, options.servingLocator);
    BOOST_CHECK_EQUAL(committed.contentDigest, options.contentDigest);
    BOOST_CHECK_EQUAL(committed.plaintextSize, options.plaintextSize);
    source.reset();
  }

  fixture.repo.reset();
  StorageCapability capability;
  capability.repoNode = "/spec190/protected-material";
  capability.freeBytes = 16U << 20;
  fixture.repo = std::make_shared<RepoCore>(capability,
    makeFilesystemRepoStore((fixture.root / "objects").string(), 1U << 20, 1U << 20));

  BOOST_CHECK(fixture.repo->has("/protected/restart"));
  const auto restored = fixture.repo->getManifest("/protected/restart");
  BOOST_CHECK_EQUAL(restored.publicationIdentity, committed.publicationIdentity);
  BOOST_CHECK_EQUAL(restored.protectionEpoch, committed.protectionEpoch);
  BOOST_CHECK_EQUAL(restored.keyReferenceId, committed.keyReferenceId);
  BOOST_CHECK_EQUAL(restored.keyReferenceVersion, committed.keyReferenceVersion);
  BOOST_CHECK_EQUAL(restored.ciphertextManifestDigest,
                    committed.ciphertextManifestDigest);
  BOOST_CHECK_EQUAL(restored.servingLocator, committed.servingLocator);
  BOOST_CHECK_EQUAL(restored.contentDigest, committed.contentDigest);
  BOOST_CHECK_EQUAL(restored.plaintextSize, committed.plaintextSize);
  BOOST_CHECK_EQUAL(fixture.repo->getRangeIfCurrent(restored, {0, 10}).size(), 10U);

  // The Core-facing adapter must recover a durable range source by the
  // stable publication identity, not by the request-scoped object name.
  RepoEncryptedLargeDataStore restoredStore(fixture.repo);
  const auto hit = restoredStore.lookupDurable(options.publicationIdentity);
  BOOST_REQUIRE(hit);
  BOOST_CHECK_EQUAL(hit->encryptedName, committed.objectName);
  BOOST_CHECK_EQUAL(hit->publicationIdentity, options.publicationIdentity);
  BOOST_CHECK_EQUAL(hit->protectionEpoch, options.protectionEpoch);
  BOOST_CHECK_EQUAL(hit->keyReferenceId, options.keyReferenceId);
  BOOST_CHECK_EQUAL(hit->keyReferenceVersion, options.keyReferenceVersion);
  BOOST_CHECK_EQUAL(hit->ciphertextManifestDigest,
                    options.ciphertextManifestDigest);
  BOOST_CHECK_EQUAL(hit->servingLocator, options.servingLocator);
  BOOST_CHECK_EQUAL(hit->contentDigest, options.contentDigest);
  BOOST_CHECK_EQUAL(hit->plaintextSize, options.plaintextSize);
  BOOST_REQUIRE(hit->source);
  BOOST_CHECK(hit->source->isDurable());
  BOOST_CHECK_EQUAL(hit->source->read(0, 10).size(), 10U);
}

BOOST_AUTO_TEST_CASE(DurableIdentityMetadataIsRequired)
{
  Fixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto path = fixture.input("invalid-metadata.bin");
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  BOOST_CHECK_THROW(store.commitFile("/protected/invalid-metadata", path, 10, options),
                    std::invalid_argument);
  BOOST_CHECK(!fixture.repo->has("/protected/invalid-metadata"));
}

BOOST_AUTO_TEST_SUITE_END()
