#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "tests/boost-test.hpp"

#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace ndnsf_distributed_repo::test {

namespace {

namespace fs = std::filesystem;

constexpr char FIXTURE_BYTES[] = "spec192 durable artifact v1";
constexpr char FIXTURE_SHA256[] =
  "24756d38ad4c7488d9d32776fd8533d2ceb790658d024a46f00ae0f945554d73";

class TemporaryDirectory
{
public:
  explicit TemporaryDirectory(const std::string& label)
  {
    static std::atomic<unsigned> sequence{0};
    m_path = fs::temp_directory_path() /
      ("ndnsf-inapp-artifact-api-" +
       std::to_string(static_cast<unsigned long>(::getpid())) + "-" + label +
       "-" + std::to_string(sequence.fetch_add(1)));
    std::error_code error;
    fs::remove_all(m_path, error);
    fs::create_directories(m_path);
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(m_path, error);
  }

  const fs::path&
  path() const noexcept
  {
    return m_path;
  }

private:
  fs::path m_path;
};

std::string
readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  BOOST_REQUIRE(input.good());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void
writeFile(const fs::path& path, const std::string& bytes)
{
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  BOOST_REQUIRE(output.good());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  BOOST_REQUIRE(output.good());
}

ArtifactReference
fixtureReference()
{
  return ArtifactReference{
    "/app/model/v1",
    "sha256",
    FIXTURE_SHA256,
    sizeof(FIXTURE_BYTES) - 1,
    "artifact-manifest-v2",
    "/app/manifest/v1",
    "/app/provider",
    "epoch-1",
  };
}

InAppRepoConfig
makeConfig(const fs::path& root)
{
  return InAppRepoConfig{root.string(), "spec192-test-owner", ArtifactLimits{}};
}

bool
sameReference(const ArtifactReference& lhs, const ArtifactReference& rhs)
{
  return lhs.logicalName == rhs.logicalName &&
         lhs.digestAlgorithm == rhs.digestAlgorithm &&
         lhs.contentDigest == rhs.contentDigest &&
         lhs.sizeBytes == rhs.sizeBytes &&
         lhs.formatVersion == rhs.formatVersion &&
         lhs.rootManifestName == rhs.rootManifestName &&
         lhs.publisherIdentity == rhs.publisherIdentity &&
         lhs.policyEpoch == rhs.policyEpoch;
}

std::string
columnText(sqlite3_stmt* statement, int column)
{
  const auto* value = sqlite3_column_text(statement, column);
  return value == nullptr
    ? std::string{}
    : reinterpret_cast<const char*>(value);
}

std::optional<ArtifactLocalCommitRecord>
directCatalogLookup(const fs::path& databasePath,
                    const ArtifactReference& reference)
{
  sqlite3* database = nullptr;
  const int openStatus = sqlite3_open_v2(
    databasePath.c_str(), &database, SQLITE_OPEN_READONLY, nullptr);
  BOOST_REQUIRE_EQUAL(openStatus, SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  const char* sql =
    "SELECT local_commit_id,logical_name,digest_algorithm,content_digest,"
    "size_bytes,format_version,root_manifest_name,publisher_identity,"
    "policy_epoch,committed_at_ms FROM artifact_local_commits WHERE "
    "logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? "
    "AND format_version=? AND root_manifest_name=? AND publisher_identity=? "
    "AND policy_epoch=?";
  int status = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
  BOOST_REQUIRE_EQUAL(status, SQLITE_OK);
  const std::string fields[] = {
    reference.logicalName,
    reference.digestAlgorithm,
    reference.contentDigest,
  };
  for (int index = 0; index < 3; ++index) {
    status = sqlite3_bind_text(statement, index + 1, fields[index].data(),
                               static_cast<int>(fields[index].size()),
                               SQLITE_TRANSIENT);
    BOOST_REQUIRE_EQUAL(status, SQLITE_OK);
  }
  status = sqlite3_bind_int64(statement, 4,
                              static_cast<sqlite3_int64>(reference.sizeBytes));
  BOOST_REQUIRE_EQUAL(status, SQLITE_OK);
  const std::string tailFields[] = {
    reference.formatVersion,
    reference.rootManifestName,
    reference.publisherIdentity,
    reference.policyEpoch,
  };
  for (int index = 0; index < 4; ++index) {
    status = sqlite3_bind_text(statement, index + 5, tailFields[index].data(),
                               static_cast<int>(tailFields[index].size()),
                               SQLITE_TRANSIENT);
    BOOST_REQUIRE_EQUAL(status, SQLITE_OK);
  }

  std::optional<ArtifactLocalCommitRecord> record;
  status = sqlite3_step(statement);
  if (status == SQLITE_ROW) {
    ArtifactReference stored{
      columnText(statement, 1),
      columnText(statement, 2),
      columnText(statement, 3),
      static_cast<uint64_t>(sqlite3_column_int64(statement, 4)),
      columnText(statement, 5),
      columnText(statement, 6),
      columnText(statement, 7),
      columnText(statement, 8),
    };
    record = ArtifactLocalCommitRecord{
      static_cast<uint64_t>(sqlite3_column_int64(statement, 0)),
      std::move(stored),
      static_cast<uint64_t>(sqlite3_column_int64(statement, 9)),
    };
  }
  BOOST_REQUIRE(status == SQLITE_ROW || status == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return record;
}

int
queryCount(const fs::path& databasePath, const char* table)
{
  sqlite3* database = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(
    databasePath.c_str(), &database, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
  BOOST_REQUIRE_EQUAL(
    sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), SQLITE_OK);
  BOOST_REQUIRE_EQUAL(sqlite3_step(statement), SQLITE_ROW);
  const int count = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return count;
}

void
executeSql(const fs::path& databasePath, const char* sql)
{
  sqlite3* database = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open(databasePath.c_str(), &database), SQLITE_OK);
  char* error = nullptr;
  const int status = sqlite3_exec(database, sql, nullptr, nullptr, &error);
  const std::string detail = error == nullptr ? "" : error;
  sqlite3_free(error);
  sqlite3_close(database);
  BOOST_REQUIRE_MESSAGE(status == SQLITE_OK, detail);
}

std::map<std::string, std::optional<std::string>>
snapshotDatabaseAndSidecars(const fs::path& databasePath)
{
  std::map<std::string, std::optional<std::string>> snapshot;
  for (const auto& suffix : {"", "-wal", "-shm"}) {
    const fs::path path(databasePath.string() + suffix);
    if (fs::exists(path)) {
      snapshot.emplace(suffix, readFile(path));
    }
    else {
      snapshot.emplace(suffix, std::nullopt);
    }
  }
  return snapshot;
}

fs::path
payloadPath(const fs::path& root, const ArtifactReference& reference)
{
  return root / "payloads" / reference.digestAlgorithm /
         reference.contentDigest.substr(0, 2) / reference.contentDigest;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoInAppArtifactRepositoryApi)

BOOST_AUTO_TEST_CASE(PublishesReopensFetchesAndMatchesIndependentDiskOracle)
{
  TemporaryDirectory temporary("reopen");
  const fs::path root = temporary.path() / "repo";
  const fs::path source = temporary.path() / "source.bin";
  const fs::path destination = temporary.path() / "download" / "artifact.bin";
  writeFile(source, FIXTURE_BYTES);
  const auto reference = fixtureReference();

  std::optional<ArtifactPublishResult> original;
  {
    auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    BOOST_CHECK(!repository.lookupLocalCommit(reference));
    original = repository.publishFile(source.string(), reference);
    BOOST_REQUIRE(original->handle);
    BOOST_REQUIRE(original->receipt);
    BOOST_REQUIRE(original->receipt->localCommit);
    BOOST_CHECK_EQUAL(original->handle->lifecycleMode, "IN_APP");
    BOOST_CHECK_EQUAL(original->receipt->placementState, "NOT_APPLICABLE");
    BOOST_CHECK_EQUAL(original->requestedReplicas, 0);
    BOOST_CHECK_EQUAL(original->achievedReplicas, 0);
    BOOST_CHECK(original->replicas.empty());
    BOOST_CHECK(original->receipt->localCommit->localCommitId > 0);
    BOOST_CHECK(original->receipt->localCommit->committedAtMs > 0);
    BOOST_CHECK(sameReference(original->receipt->localCommit->reference, reference));
    const auto repeated = repository.publishFile(source.string(), reference);
    BOOST_REQUIRE(repeated.receipt);
    BOOST_REQUIRE(repeated.receipt->localCommit);
    BOOST_CHECK(repeated.deduplicated);
    BOOST_CHECK_EQUAL(repeated.receipt->localCommit->localCommitId,
                      original->receipt->localCommit->localCommitId);
    BOOST_CHECK_EQUAL(repeated.receipt->localCommit->committedAtMs,
                      original->receipt->localCommit->committedAtMs);
  }

  {
    auto reopened = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    const auto recovered = reopened.lookupLocalCommit(reference);
    BOOST_REQUIRE(recovered);
    BOOST_REQUIRE(original->receipt);
    BOOST_REQUIRE(original->receipt->localCommit);
    BOOST_CHECK_EQUAL(recovered->localCommitId,
                      original->receipt->localCommit->localCommitId);
    BOOST_CHECK_EQUAL(recovered->committedAtMs,
                      original->receipt->localCommit->committedAtMs);
    BOOST_CHECK(sameReference(recovered->reference, reference));
    reopened.fetchFile(reference, destination.string());
  }

  BOOST_CHECK_EQUAL(readFile(destination), FIXTURE_BYTES);
  const auto onDisk = directCatalogLookup(root / "metadata.sqlite3", reference);
  BOOST_REQUIRE(onDisk);
  BOOST_REQUIRE(original->receipt);
  BOOST_REQUIRE(original->receipt->localCommit);
  BOOST_CHECK_EQUAL(onDisk->localCommitId,
                    original->receipt->localCommit->localCommitId);
  BOOST_CHECK_EQUAL(onDisk->committedAtMs,
                    original->receipt->localCommit->committedAtMs);
  BOOST_CHECK(sameReference(onDisk->reference, reference));
  BOOST_CHECK_EQUAL(
    readFile(payloadPath(root, reference)), FIXTURE_BYTES);
}

BOOST_AUTO_TEST_CASE(FullReferenceIsTheCatalogKeyNotDigestAlone)
{
  TemporaryDirectory temporary("full-reference");
  const fs::path root = temporary.path() / "repo";
  const fs::path source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_BYTES);
  auto first = fixtureReference();
  auto second = first;
  second.logicalName = "/app/model/v2";
  second.formatVersion = "exact-packet-v1";
  second.rootManifestName = "/app/manifest/v2";
  second.publisherIdentity = "/app/provider-2";
  second.policyEpoch = "epoch-2";
  BOOST_REQUIRE(first.sameBytes(second));
  BOOST_REQUIRE(!sameReference(first, second));

  auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
  const auto firstResult = repository.publishFile(source.string(), first);
  const auto secondResult = repository.publishFile(source.string(), second);
  BOOST_REQUIRE(firstResult.receipt);
  BOOST_REQUIRE(firstResult.receipt->localCommit);
  BOOST_REQUIRE(secondResult.receipt);
  BOOST_REQUIRE(secondResult.receipt->localCommit);
  BOOST_CHECK_NE(firstResult.receipt->localCommit->localCommitId,
                 secondResult.receipt->localCommit->localCommitId);
  BOOST_CHECK_EQUAL(queryCount(root / "metadata.sqlite3", "artifact_local_commits"), 2);
  const auto firstLookup = repository.lookupLocalCommit(first);
  const auto secondLookup = repository.lookupLocalCommit(second);
  BOOST_REQUIRE(firstLookup);
  BOOST_REQUIRE(secondLookup);
  BOOST_CHECK_EQUAL(firstLookup->localCommitId,
                    firstResult.receipt->localCommit->localCommitId);
  BOOST_CHECK_EQUAL(secondLookup->localCommitId,
                    secondResult.receipt->localCommit->localCommitId);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidConfigurationBeforeCreatingDurableState)
{
  TemporaryDirectory temporary("invalid-config");
  const auto missingRoot = temporary.path() / "must-not-be-created";
  auto emptyOwner = makeConfig(missingRoot);
  emptyOwner.ownerId.clear();
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeInApp(emptyOwner),
                    std::invalid_argument);
  BOOST_CHECK(!fs::exists(missingRoot));

  auto zeroLimit = makeConfig(missingRoot);
  zeroLimit.limits.maxArtifactBytes = 0;
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeInApp(zeroLimit),
                    std::invalid_argument);
  BOOST_CHECK(!fs::exists(missingRoot));
}

BOOST_AUTO_TEST_CASE(ExclusiveRootOwnershipEndsWithTheApiLifetime)
{
  TemporaryDirectory temporary("lease");
  const auto root = temporary.path() / "repo";
  {
    auto owner = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    BOOST_CHECK_EXCEPTION(
      ArtifactRepositoryApi::makeInApp(makeConfig(root)),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("repo-persistence-owned:") == 0;
      });
  }
  BOOST_CHECK_NO_THROW(ArtifactRepositoryApi::makeInApp(makeConfig(root)));
}

BOOST_AUTO_TEST_CASE(RecoversFinalizedPayloadWhenCatalogCommitWasInterrupted)
{
  TemporaryDirectory temporary("catalog-recovery");
  const auto root = temporary.path() / "repo";
  const auto source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_BYTES);
  const auto reference = fixtureReference();
  {
    FilesystemArtifactPayloadStore payloadStore(root.string());
    payloadStore.begin(reference, 1);
    const std::vector<uint8_t> bytes(FIXTURE_BYTES, FIXTURE_BYTES + sizeof(FIXTURE_BYTES) - 1);
    payloadStore.writeRange(reference, 1, {0, bytes.size()}, bytes);
    payloadStore.markVerified(reference, 1, {0, bytes.size()});
    payloadStore.flush(reference, 1);
    payloadStore.finalize(reference, 1);
  }
  {
    auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    BOOST_CHECK(!repository.lookupLocalCommit(reference));
    const auto recovered = repository.publishFile(source.string(), reference);
    BOOST_REQUIRE(recovered.receipt);
    BOOST_REQUIRE(recovered.receipt->localCommit);
    BOOST_CHECK(recovered.resumed);
    BOOST_CHECK(recovered.receipt->localCommit->localCommitId > 0);
    BOOST_CHECK(sameReference(recovered.reference, reference));
  }
  BOOST_CHECK_EQUAL(queryCount(root / "metadata.sqlite3", "artifact_local_commits"), 1);
}

BOOST_AUTO_TEST_CASE(CorruptPreexistingPayloadCannotCreateACommit)
{
  TemporaryDirectory temporary("precommit-corrupt");
  const auto root = temporary.path() / "repo";
  const auto source = temporary.path() / "source.bin";
  writeFile(source, FIXTURE_BYTES);
  const auto reference = fixtureReference();
  {
    auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
  }
  writeFile(payloadPath(root, reference), "corrupted existing payload");

  auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
  BOOST_CHECK_THROW(repository.publishFile(source.string(), reference),
                    std::runtime_error);
  BOOST_CHECK(!repository.lookupLocalCommit(reference));
  BOOST_CHECK_EQUAL(queryCount(root / "metadata.sqlite3", "artifact_local_commits"), 0);
}

BOOST_AUTO_TEST_CASE(PostCommitDamagePreservesHistoryAndNeverReplacesDestination)
{
  TemporaryDirectory temporary("postcommit-damage");
  const auto root = temporary.path() / "repo";
  const auto source = temporary.path() / "source.bin";
  const auto destination = temporary.path() / "destination.bin";
  writeFile(source, FIXTURE_BYTES);
  writeFile(destination, "keep-existing-destination");
  const auto reference = fixtureReference();
  auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
  const auto record = repository.publishFile(source.string(), reference);
  BOOST_REQUIRE(record.receipt);
  BOOST_REQUIRE(record.receipt->localCommit);
  const auto diskRecordBefore = directCatalogLookup(root / "metadata.sqlite3", reference);
  BOOST_REQUIRE(diskRecordBefore);

  writeFile(payloadPath(root, reference), "corrupt after commit");
  BOOST_CHECK_THROW(repository.fetchFile(reference, destination.string()),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(readFile(destination), "keep-existing-destination");
  const auto afterCorruption = directCatalogLookup(root / "metadata.sqlite3", reference);
  BOOST_REQUIRE(afterCorruption);
  BOOST_CHECK_EQUAL(afterCorruption->localCommitId,
                    record.receipt->localCommit->localCommitId);
  BOOST_CHECK_EQUAL(afterCorruption->committedAtMs,
                    record.receipt->localCommit->committedAtMs);
  BOOST_CHECK(sameReference(afterCorruption->reference, reference));

  fs::remove(payloadPath(root, reference));
  BOOST_CHECK_THROW(repository.fetchFile(reference, destination.string()),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(readFile(destination), "keep-existing-destination");
  const auto afterDeletion = directCatalogLookup(root / "metadata.sqlite3", reference);
  BOOST_REQUIRE(afterDeletion);
  BOOST_CHECK_EQUAL(afterDeletion->localCommitId,
                    record.receipt->localCommit->localCommitId);
  BOOST_CHECK_EQUAL(afterDeletion->committedAtMs,
                    record.receipt->localCommit->committedAtMs);
  BOOST_CHECK(sameReference(afterDeletion->reference, reference));
}

BOOST_AUTO_TEST_CASE(MismatchedSourceBytesDoNotCreateACommit)
{
  TemporaryDirectory temporary("source-mismatch");
  const auto root = temporary.path() / "repo";
  const auto source = temporary.path() / "source.bin";
  writeFile(source, "different bytes");
  const auto reference = fixtureReference();
  auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
  BOOST_CHECK_THROW(repository.publishFile(source.string(), reference),
                    std::runtime_error);
  BOOST_CHECK(!repository.lookupLocalCommit(reference));
  BOOST_CHECK_EQUAL(queryCount(root / "metadata.sqlite3", "artifact_local_commits"), 0);
}

BOOST_AUTO_TEST_CASE(FutureSchemaAllowsReadOnlyReuseWithoutPublishingOrMutation)
{
  TemporaryDirectory temporary("future-schema-api");
  const auto root = temporary.path() / "repo";
  const auto source = temporary.path() / "source.bin";
  const auto destination = temporary.path() / "reused" / "artifact.bin";
  const auto reference = fixtureReference();
  writeFile(source, FIXTURE_BYTES);
  {
    auto repository = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    BOOST_REQUIRE(repository.publishFile(source.string(), reference).receipt);
  }
  executeSql(root / "metadata.sqlite3",
             "UPDATE repo_meta SET value='15' WHERE key='schema_generation'");
  const auto before = snapshotDatabaseAndSidecars(root / "metadata.sqlite3");

  {
    auto readOnly = ArtifactRepositoryApi::makeInApp(makeConfig(root));
    BOOST_REQUIRE(readOnly.lookupLocalCommit(reference));
    readOnly.fetchFile(reference, destination.string());
    BOOST_CHECK_EXCEPTION(
      readOnly.publishFile(source.string(), reference),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("repo-artifact-writes-disabled:") == 0;
      });
  }

  BOOST_CHECK_EQUAL(readFile(destination), FIXTURE_BYTES);
  BOOST_CHECK(snapshotDatabaseAndSidecars(root / "metadata.sqlite3") == before);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
