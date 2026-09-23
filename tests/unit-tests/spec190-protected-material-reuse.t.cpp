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
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
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
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
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
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
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

BOOST_AUTO_TEST_SUITE_END()
