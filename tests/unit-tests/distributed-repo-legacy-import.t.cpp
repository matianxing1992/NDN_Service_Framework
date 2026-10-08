#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/LegacyRepoImporter.hpp"
#include "tests/boost-test.hpp"
#include "tests/fixtures/spec202-syscall-fault.hpp"
#include "tests/fixtures/spec202-import-lock.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

namespace ndnsf_distributed_repo::test {
namespace {

namespace fs = std::filesystem;

constexpr char ARTIFACT_BYTES[] = "spec192 durable artifact v1";
constexpr char ARTIFACT_SHA256[] =
  "24756d38ad4c7488d9d32776fd8533d2ceb790658d024a46f00ae0f945554d73";

class TemporaryDirectory
{
public:
  TemporaryDirectory()
  {
    static std::atomic<unsigned> sequence{0};
    m_path = fs::temp_directory_path() /
      ("ndnsf-spec192-import-unit-" + std::to_string(::getpid()) + "-" +
       std::to_string(sequence.fetch_add(1)));
    fs::create_directories(m_path);
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(m_path, error);
  }

  const fs::path& path() const noexcept { return m_path; }

private:
  fs::path m_path;
};

class Database
{
public:
  explicit Database(const fs::path& path)
  {
    const int status = sqlite3_open_v2(path.c_str(), &m_db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (status != SQLITE_OK) {
      const std::string message = m_db == nullptr ? "sqlite-open-failed" :
        sqlite3_errmsg(m_db);
      if (m_db != nullptr) sqlite3_close(m_db);
      m_db = nullptr;
      throw std::runtime_error(message);
    }
  }
  ~Database() { if (m_db != nullptr) sqlite3_close(m_db); }
  sqlite3* get() const noexcept { return m_db; }

private:
  sqlite3* m_db = nullptr;
};

fs::path
repositoryRoot()
{
  auto current = fs::current_path();
  for (;;) {
    if (fs::exists(current / "tests/fixtures/spec192")) return current;
    const auto parent = current.parent_path();
    if (parent == current) throw std::runtime_error("spec192-fixture-root-not-found");
    current = parent;
  }
}

std::string
readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("fixture-read-failed: " + path.string());
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void
execute(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  const int status = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (status != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(db) : error;
    sqlite3_free(error);
    throw std::runtime_error("fixture-sql-failed: " + message);
  }
}

void
bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("fixture-bind-text-failed");
  }
}

void
stepDone(sqlite3_stmt* statement)
{
  if (sqlite3_step(statement) != SQLITE_DONE) {
    throw std::runtime_error("fixture-insert-failed");
  }
}

ArtifactReference
artifactReference()
{
  return ArtifactReference{
    "/app/model/v1", "sha256", ARTIFACT_SHA256,
    sizeof(ARTIFACT_BYTES) - 1, "artifact-manifest-v2",
    "/app/manifest/v1", "/app/provider", "epoch-1"};
}

void
initializeArtifactFixture(sqlite3* db, const fs::path& payloadRoot)
{
  execute(db, readFile(repositoryRoot() /
                       "tests/fixtures/spec192/legacy-python-artifact-v12.sql"));
  execute(db, "PRAGMA journal_mode=WAL");

  const auto reference = artifactReference();
  constexpr char operationId[] = "legacy-artifact-operation-1";
  sqlite3_stmt* catalog = nullptr;
  const char* catalogSql =
    "INSERT INTO artifact_active_catalog(logical_name,policy_epoch,format_version,"
    "digest_algorithm,artifact_digest,generation,operation_id,artifact_json,activated_at_ms)"
    " VALUES(?,?,'artifact-manifest-v2','sha256',?,5,?,?,100)";
  if (sqlite3_prepare_v2(db, catalogSql, -1, &catalog, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-catalog-prepare-failed");
  }
  bindText(catalog, 1, reference.logicalName);
  bindText(catalog, 2, reference.policyEpoch);
  bindText(catalog, 3, reference.contentDigest);
  bindText(catalog, 4, operationId);
  bindText(catalog, 5, reference.toJson());
  stepDone(catalog);
  sqlite3_finalize(catalog);

  sqlite3_stmt* lifecycle = nullptr;
  const char* lifecycleSql =
    "INSERT INTO artifact_lifecycle_journal(event_id,operation_id,artifact_digest,"
    "generation,from_state,to_state,event_time_ms,accepted,detail_json,error)"
    " VALUES(?,?,?,5,?,?,?,1,'{}','')";
  if (sqlite3_prepare_v2(db, lifecycleSql, -1, &lifecycle, nullptr) != SQLITE_OK) {
    throw std::runtime_error("fixture-lifecycle-prepare-failed");
  }
  const std::pair<std::string, std::string> states[] = {
    {"FINALIZING", "COMMITTED"}, {"COMMITTED", "ACTIVE"}};
  for (size_t i = 0; i < 2; ++i) {
    sqlite3_reset(lifecycle);
    sqlite3_clear_bindings(lifecycle);
    bindText(lifecycle, 1, "legacy-event-" + std::to_string(i));
    bindText(lifecycle, 2, operationId);
    bindText(lifecycle, 3, reference.contentDigest);
    bindText(lifecycle, 4, states[i].first);
    bindText(lifecycle, 5, states[i].second);
    sqlite3_bind_int64(lifecycle, 6, static_cast<sqlite3_int64>(101 + i));
    stepDone(lifecycle);
  }
  sqlite3_finalize(lifecycle);

  const auto payloadPath = payloadRoot / "payloads/sha256" /
    std::string(ARTIFACT_SHA256).substr(0, 2) / ARTIFACT_SHA256;
  fs::create_directories(payloadPath.parent_path());
  std::ofstream payload(payloadPath, std::ios::binary | std::ios::trunc);
  payload.write(ARTIFACT_BYTES, sizeof(ARTIFACT_BYTES) - 1);
  if (!payload) throw std::runtime_error("fixture-payload-write-failed");
}

std::string
sha256Bytes(const std::string& bytes)
{
  std::array<unsigned char, 32> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1 || length != digest.size()) {
    throw std::runtime_error("source-hash-failed");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(HEX[byte >> 4]);
    result.push_back(HEX[byte & 0x0f]);
  }
  return result;
}

std::string
sha256File(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("source-hash-open-failed");
  return sha256Bytes(std::string(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()));
}

std::string
jsonQuote(const std::string& value)
{
  std::string result = "\"";
  for (const auto c : value) {
    if (c == '\\' || c == '"') result.push_back('\\');
    result.push_back(c);
  }
  result.push_back('"');
  return result;
}

std::string
sourceBundleDigest(const fs::path& database)
{
  std::string json = "[";
  bool first = true;
  for (const auto* suffix : {"", "-shm", "-wal"}) {
    const auto path = fs::path(database.string() + suffix);
    if (!fs::exists(path)) continue;
    if (!first) json += ',';
    first = false;
    const auto name = std::string(suffix).empty() ? "database" : suffix;
    json += "{\"name\":" + jsonQuote(name) + ",\"sha256\":" +
      jsonQuote(sha256File(path)) + ",\"size\":" +
      std::to_string(fs::file_size(path)) + "}";
  }
  json += ']';
  return sha256Bytes(json);
}

std::string
sourcePayloadDigest(const fs::path& root, const fs::path& payload)
{
  const auto name = fs::relative(payload, root).generic_string();
  const std::string json = "[{\"name\":" + jsonQuote(name) +
    ",\"sha256\":" + jsonQuote(sha256File(payload)) +
    ",\"size\":" + std::to_string(fs::file_size(payload)) + "}]";
  return sha256Bytes(json);
}

std::string
artifactDestinationDigest(const ArtifactReference& reference)
{
  const std::string item = "{\"kind\":\"artifact\",\"reference\":" +
    jsonQuote(reference.toJson()) + ",\"sha256\":" +
    jsonQuote(reference.contentDigest) + ",\"size\":" +
    std::to_string(reference.sizeBytes) + "}";
  return sha256Bytes("[" + item + "]");
}

std::vector<std::string>
databaseBundleHashes(const fs::path& database)
{
  std::vector<std::string> result;
  for (const auto* suffix : {"", "-wal", "-shm"}) {
    const auto path = fs::path(database.string() + suffix);
    result.push_back(fs::exists(path) ? sha256File(path) : "MISSING");
  }
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec192LegacyRepoImportUnit)

BOOST_AUTO_TEST_CASE(importsArtifactProfileWithoutMutatingWalSourceAndResumes)
{
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native";
  Database database(source);
  initializeArtifactFixture(database.get(), payloadRoot);
  const auto sourceHashes = databaseBundleHashes(source);
  const auto sourcePayload = payloadRoot / "payloads/sha256" /
    std::string(ARTIFACT_SHA256).substr(0, 2) / ARTIFACT_SHA256;
  const auto payloadHash = sha256File(sourcePayload);
  const auto snapshotDigest = sourceBundleDigest(source);
  const auto payloadDigest = sourcePayloadDigest(payloadRoot, sourcePayload);

  LegacyRepoImportConfig config;
  config.sourceDatabase = source;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "unit-artifact-import";

  const auto result = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(result.state, "COMMITTED");
  BOOST_CHECK_EQUAL(result.sourceSchemaId, "python-artifact-v12");
  BOOST_CHECK_EQUAL(result.sourceSchemaFingerprint,
    "73c858bbe42890f8e66957bb159c5ff3d62e3203bbe5bd104dc328204c7f8017");
  BOOST_CHECK_EQUAL(result.sourceSnapshotDigest, snapshotDigest);
  BOOST_CHECK_EQUAL(result.sourcePayloadInventoryDigest, payloadDigest);
  BOOST_CHECK_EQUAL(result.destinationInventoryDigest,
                    artifactDestinationDigest(artifactReference()));
  BOOST_REQUIRE_EQUAL(result.items.size(), 1);
  BOOST_CHECK_EQUAL(result.items.front().state, "COMMITTED");
  BOOST_CHECK_EQUAL(result.items.front().sourceGeneration, 5);
  BOOST_CHECK_EQUAL(result.items.front().nativeGeneration, 1);
  bool retainedCatalogClaim = false;
  bool retainedLifecycleClaim = false;
  for (const auto& claim : result.items.front().legacyClaims) {
    retainedCatalogClaim = retainedCatalogClaim ||
      claim.table == "artifact_active_catalog";
    retainedLifecycleClaim = retainedLifecycleClaim ||
      claim.table == "artifact_lifecycle_journal";
  }
  BOOST_CHECK(retainedCatalogClaim);
  BOOST_CHECK(retainedLifecycleClaim);
  BOOST_CHECK(fs::exists(destination / "LegacyImportManifest.json"));
  BOOST_CHECK(databaseBundleHashes(source) == sourceHashes);
  BOOST_CHECK_EQUAL(sha256File(sourcePayload), payloadHash);

  const auto repeat = LegacyRepoImporter::import(config);
  BOOST_CHECK_EQUAL(repeat.state, "COMMITTED");
  BOOST_CHECK_EQUAL(repeat.sourceSnapshotDigest, result.sourceSnapshotDigest);

  auto native = ArtifactRepositoryApi::makeInApp(
    InAppRepoConfig{destination / "artifacts", "legacy-import-unit-artifact", {}});
  const auto fetched = temporary.path() / "artifact.out";
  native.fetchFile(artifactReference(), fetched.string());
  BOOST_CHECK_EQUAL(readFile(fetched),
                    std::string(ARTIFACT_BYTES, sizeof(ARTIFACT_BYTES) - 1));
  BOOST_CHECK(databaseBundleHashes(source) == sourceHashes);
  BOOST_CHECK_EQUAL(sha256File(sourcePayload), payloadHash);
}

BOOST_AUTO_TEST_CASE(retriesPublishedDestinationParentBarrierBeforeAcknowledgment)
{
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native";
  Database database(source);
  initializeArtifactFixture(database.get(), payloadRoot);
  const auto sourceHashes = databaseBundleHashes(source);
  const auto sourcePayload = payloadRoot / "payloads/sha256" /
    std::string(ARTIFACT_SHA256).substr(0, 2) / ARTIFACT_SHA256;
  const auto payloadHash = sha256File(sourcePayload);
  LegacyRepoImportConfig config;
  config.sourceDatabase = source;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "spec202-publish-barrier";
  unsigned checkpoints = 0;
  config.onItemCommitted = [&] (size_t, const LegacyImportItem&) { ++checkpoints; };
  const auto isBarrierError = [] (const LegacyRepoImportError& error) {
    return std::string(error.what()) == "legacy-import-fsync-directory-failed";
  };
  {
    spec202_test::ScopedFault fault(destination, spec202_test::Fault::SyncError, 0, true);
    BOOST_CHECK_EXCEPTION(LegacyRepoImporter::import(config), LegacyRepoImportError, isBarrierError);
    BOOST_CHECK_EQUAL(fault.hits(), 1U);
  }
  BOOST_CHECK_EQUAL(checkpoints, 1U);
  BOOST_REQUIRE(fs::is_directory(destination));
  const auto manifestPath = destination / "LegacyImportManifest.json";
  const auto manifestBytes = readFile(manifestPath);
  BOOST_CHECK(manifestBytes.find("COMMITTED") != std::string::npos);
  BOOST_CHECK(databaseBundleHashes(source) == sourceHashes);
  BOOST_CHECK_EQUAL(sha256File(sourcePayload), payloadHash);
  {
    spec202_test::ScopedFault fault(destination, spec202_test::Fault::SyncError, 0, true);
    BOOST_CHECK_EXCEPTION(LegacyRepoImporter::import(config), LegacyRepoImportError, isBarrierError);
    BOOST_CHECK_EQUAL(fault.hits(), 1U);
  }
  BOOST_CHECK_EQUAL(readFile(manifestPath), manifestBytes);
  {
    spec202_test::ScopedFault barrier(destination, spec202_test::Fault::SyncDelay, 1, true);
    const auto replay = LegacyRepoImporter::import(config);
    BOOST_CHECK_EQUAL(replay.state, "COMMITTED");
    BOOST_CHECK_EQUAL(replay.destinationInventoryDigest,
                      artifactDestinationDigest(artifactReference()));
    BOOST_CHECK_EQUAL(barrier.hits(), 1U);
  }
  BOOST_CHECK_EQUAL(checkpoints, 1U);

  // First caller has passed the absent-destination check but has not acquired its lock.
  // A second real importer publishes; first caller must validate and sync the locked replay.
  const auto lockedDestination = temporary.path() / "locked-native";
  auto lockedConfig = config;
  lockedConfig.destinationRoot = lockedDestination;
  lockedConfig.importId = "spec202-locked-barrier";
  std::atomic<unsigned> lockedCheckpoints{0};
  lockedConfig.onItemCommitted = [&] (size_t, const LegacyImportItem&) {
    lockedCheckpoints.fetch_add(1);
  };
  const auto lockPath = temporary.path() /
    ".locked-native.ndnsf-import-spec202-locked-barrier.staging.lock";
  spec202_test::ScopedImportLockPause pause(lockPath);
  auto first = std::async(std::launch::async, [&] { return LegacyRepoImporter::import(lockedConfig); });
  BOOST_REQUIRE(pause.waitForEntry());
  const auto second = LegacyRepoImporter::import(lockedConfig);
  BOOST_CHECK_EQUAL(second.state, "COMMITTED");
  {
    spec202_test::ScopedFault fault(lockedDestination, spec202_test::Fault::SyncError, 0, true);
    pause.release();
    BOOST_CHECK_EXCEPTION(first.get(), LegacyRepoImportError, isBarrierError);
    BOOST_CHECK_EQUAL(fault.hits(), 1U);
  }
  BOOST_CHECK_EQUAL(lockedCheckpoints.load(), 1U);
  BOOST_CHECK_EQUAL(LegacyRepoImporter::import(lockedConfig).destinationInventoryDigest,
                    second.destinationInventoryDigest);
  BOOST_CHECK(databaseBundleHashes(source) == sourceHashes);
  BOOST_CHECK_EQUAL(sha256File(sourcePayload), payloadHash);
  auto native = ArtifactRepositoryApi::makeInApp(
    InAppRepoConfig{destination / "artifacts", "spec202-reopened-import", {}});
  const auto fetched = temporary.path() / "reopened-artifact";
  native.fetchFile(artifactReference(), fetched.string());
  BOOST_CHECK_EQUAL(readFile(fetched), std::string(ARTIFACT_BYTES, sizeof(ARTIFACT_BYTES) - 1));
}

BOOST_AUTO_TEST_CASE(rejectsUnknownMarkerAndExistingDestinationBeforeWrites)
{
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native";
  Database database(source);
  execute(database.get(), readFile(repositoryRoot() /
    "tests/fixtures/spec192/legacy-python-artifact-v12.sql"));
  execute(database.get(), "UPDATE artifact_schema_state SET schema_generation=11");

  LegacyRepoImportConfig config;
  config.sourceDatabase = source;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "unit-unknown-schema";
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK(!fs::exists(destination));

  const auto conflict = temporary.path() / "conflict";
  const auto validSource = temporary.path() / "valid.sqlite3";
  Database validDatabase(validSource);
  execute(validDatabase.get(), readFile(repositoryRoot() /
    "tests/fixtures/spec192/legacy-python-artifact-v12.sql"));
  fs::create_directories(payloadRoot);
  fs::create_directories(conflict);
  std::ofstream marker(conflict / "keep.txt");
  marker << "preserve";
  marker.close();
  config.sourceDatabase = validSource;
  config.destinationRoot = conflict;
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK_EQUAL(readFile(conflict / "keep.txt"), "preserve");
}

BOOST_AUTO_TEST_CASE(rejectsArtifactCatalogWhoseLatestAcceptedLifecycleIsNotActive)
{
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "legacy.sqlite3";
  const auto payloadRoot = temporary.path() / "source-payloads";
  const auto destination = temporary.path() / "native";
  Database database(source);
  initializeArtifactFixture(database.get(), payloadRoot);
  const auto reference = artifactReference();
  execute(database.get(),
    "INSERT INTO artifact_lifecycle_journal(event_id,operation_id,artifact_digest,"
    "generation,from_state,to_state,event_time_ms,accepted,detail_json,error) VALUES("
    "'legacy-event-deleted','legacy-artifact-operation-1','" +
    reference.contentDigest + "',5,'ACTIVE','DELETED',103,1,'{}','')");

  LegacyRepoImportConfig config;
  config.sourceDatabase = source;
  config.sourcePayloadRoot = payloadRoot;
  config.destinationRoot = destination;
  config.importId = "unit-stale-artifact-catalog";
  BOOST_CHECK_THROW(LegacyRepoImporter::import(config), LegacyRepoImportError);
  BOOST_CHECK(!fs::exists(destination));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
