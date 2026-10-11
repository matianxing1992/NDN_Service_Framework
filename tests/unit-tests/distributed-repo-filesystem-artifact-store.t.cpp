#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "tests/boost-test.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace ndnsf_distributed_repo::test {

namespace {

namespace fs = std::filesystem;

std::string
temporaryRoot(const std::string& suffix)
{
  return "/tmp/ndnsf-artifact-store-" +
         std::to_string(static_cast<unsigned long>(::getpid())) + "-" + suffix;
}

std::string
sha256(const std::vector<uint8_t>& bytes)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  BOOST_REQUIRE(context != nullptr);
  BOOST_REQUIRE_EQUAL(
    EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr), 1);
  BOOST_REQUIRE_EQUAL(
    EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()), 1);
  std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
  unsigned size = 0;
  BOOST_REQUIRE_EQUAL(EVP_DigestFinal_ex(context.get(), digest.data(), &size), 1);
  digest.resize(size);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

ArtifactReference
makeReference(const std::vector<uint8_t>& bytes)
{
  return ArtifactReference{
    "/artifact/spec164/t006",
    "sha256",
    sha256(bytes),
    bytes.size(),
    "artifact-manifest-v2",
    "/publisher/manifests/spec164/t006",
    "/publisher",
    "policy-epoch-1",
  };
}

std::vector<uint8_t>
slice(const std::vector<uint8_t>& bytes, size_t begin, size_t end)
{
  return std::vector<uint8_t>(bytes.begin() + begin, bytes.begin() + end);
}

int
queryCount(const std::string& databasePath, const char* table)
{
  sqlite3* database = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open(databasePath.c_str(), &database), SQLITE_OK);
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
executeSql(const std::string& databasePath, const char* sql)
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

void
downgradePersistentCatalogToSchema14(const std::string& database)
{
  executeSql(
    database,
    "ALTER TABLE artifact_locations RENAME TO artifact_locations_v15;"
    "ALTER TABLE artifact_persistent_receipts "
      "RENAME TO artifact_persistent_receipts_v15;"
    "CREATE TABLE artifact_persistent_receipts("
      "receipt_id TEXT PRIMARY KEY,operation_id TEXT NOT NULL,repo_node TEXT NOT NULL,"
      "provider_identity TEXT NOT NULL,logical_name TEXT NOT NULL,digest_algorithm TEXT NOT NULL,"
      "content_digest TEXT NOT NULL,size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),"
      "format_version TEXT NOT NULL,root_manifest_name TEXT NOT NULL,"
      "publisher_identity TEXT NOT NULL,policy_epoch TEXT NOT NULL,"
      "generation INTEGER NOT NULL CHECK(generation>0),"
      "persisted_bytes INTEGER NOT NULL CHECK(persisted_bytes>=0),"
      "committed_at_ms INTEGER NOT NULL CHECK(committed_at_ms>0),"
      "state TEXT NOT NULL CHECK(state='COMMITTED'),"
      "UNIQUE(repo_node,provider_identity,operation_id),"
      "UNIQUE(repo_node,provider_identity,logical_name,digest_algorithm,content_digest,"
        "size_bytes,format_version,root_manifest_name,publisher_identity,policy_epoch));"
    "CREATE TABLE artifact_locations("
      "repo_node TEXT NOT NULL,provider_identity TEXT NOT NULL,logical_name TEXT NOT NULL,"
      "digest_algorithm TEXT NOT NULL,content_digest TEXT NOT NULL,"
      "size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),format_version TEXT NOT NULL,"
      "root_manifest_name TEXT NOT NULL,publisher_identity TEXT NOT NULL,"
      "policy_epoch TEXT NOT NULL,receipt_id TEXT NOT NULL,operation_id TEXT NOT NULL,"
      "generation INTEGER NOT NULL CHECK(generation>0),"
      "location_version INTEGER NOT NULL CHECK(location_version>0),"
      "committed_at_ms INTEGER NOT NULL CHECK(committed_at_ms>0),"
      "expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>committed_at_ms),"
      "route_state TEXT NOT NULL CHECK(route_state IN ('ROUTABLE','UNROUTABLE')),"
      "forwarding_hint TEXT NOT NULL,"
      "PRIMARY KEY(repo_node,provider_identity,logical_name,digest_algorithm,content_digest,"
        "size_bytes,format_version,root_manifest_name,publisher_identity,policy_epoch),"
      "FOREIGN KEY(receipt_id) REFERENCES artifact_persistent_receipts(receipt_id));"
    "INSERT INTO artifact_persistent_receipts(receipt_id,operation_id,repo_node,"
      "provider_identity,logical_name,digest_algorithm,content_digest,size_bytes,"
      "format_version,root_manifest_name,publisher_identity,policy_epoch,generation,"
      "persisted_bytes,committed_at_ms,state) "
      "SELECT receipt_id,operation_id,repo_node,provider_identity,logical_name,"
      "digest_algorithm,content_digest,size_bytes,format_version,root_manifest_name,"
      "publisher_identity,policy_epoch,generation,persisted_bytes,committed_at_ms,state "
      "FROM artifact_persistent_receipts_v15;"
    "INSERT INTO artifact_locations(repo_node,provider_identity,logical_name,"
      "digest_algorithm,content_digest,size_bytes,format_version,root_manifest_name,"
      "publisher_identity,policy_epoch,receipt_id,operation_id,generation,location_version,"
      "committed_at_ms,expires_at_ms,route_state,forwarding_hint) "
      "SELECT repo_node,provider_identity,logical_name,digest_algorithm,content_digest,"
      "size_bytes,format_version,root_manifest_name,publisher_identity,policy_epoch,"
      "receipt_id,operation_id,generation,location_version,committed_at_ms,expires_at_ms,"
      "route_state,forwarding_hint FROM artifact_locations_v15;"
    "DROP TABLE artifact_locations_v15;"
    "DROP TABLE artifact_persistent_receipts_v15;"
    "DROP TABLE artifact_publish_receipts;"
    "DROP TABLE artifact_publish_targets;"
    "DROP TABLE artifact_publish_operations;"
    "UPDATE repo_meta SET value='14' WHERE key='schema_generation'");
}

std::map<std::string, std::optional<std::string>>
snapshotDatabaseAndSidecars(const std::string& databasePath)
{
  std::map<std::string, std::optional<std::string>> snapshot;
  for (const auto* suffix : {"", "-wal", "-shm"}) {
    const std::string key(suffix);
    const std::string path = databasePath + key;
    if (!fs::exists(path)) {
      snapshot.emplace(key, std::nullopt);
      continue;
    }
    std::ifstream input(path, std::ios::binary);
    BOOST_REQUIRE(input.good());
    snapshot.emplace(key, std::string(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>()));
  }
  return snapshot;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoFilesystemArtifactStore)

BOOST_AUTO_TEST_CASE(StreamsOutOfOrderRangesAndAtomicallyFinalizesByDigest)
{
  const auto root = temporaryRoot("stream");
  fs::remove_all(root);
  std::vector<uint8_t> payload(1024 * 1024 + 37);
  for (size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<uint8_t>((index * 29 + 7) & 0xff);
  }
  const auto reference = makeReference(payload);
  {
    FilesystemArtifactPayloadStore store(root);
    store.begin(reference, 1);
    const uint64_t split = 700000;
    store.writeRange(
      reference, 1, {split, payload.size() - split},
      slice(payload, split, payload.size()));
    store.markVerified(reference, 1, {split, payload.size() - split});
    BOOST_CHECK_EXCEPTION(
      store.finalize(reference, 1),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("repo-artifact-incomplete:") == 0;
      });

    store.writeRange(reference, 1, {0, split}, slice(payload, 0, split));
    store.markVerified(reference, 1, {0, split});
    const auto ranges = store.verifiedRanges(reference, 1);
    BOOST_REQUIRE_EQUAL(ranges.size(), 1);
    BOOST_CHECK_EQUAL(ranges[0].offsetBytes, 0);
    BOOST_CHECK_EQUAL(ranges[0].lengthBytes, payload.size());
    const auto middle = store.readRange(reference, 1, {1234, 8192});
    BOOST_CHECK_EQUAL_COLLECTIONS(
      middle.begin(), middle.end(),
      payload.begin() + 1234, payload.begin() + 1234 + 8192);

    store.flush(reference, 1);
    store.finalize(reference, 1);
    BOOST_CHECK(store.isCommitted(reference, 1));
    BOOST_CHECK(fs::exists(store.committedPath(reference)));
    BOOST_CHECK(!fs::exists(store.stagingPath(reference, 1)));
  }
  {
    FilesystemArtifactPayloadStore reopened(root);
    BOOST_CHECK(reopened.isCommitted(reference, 9));
    const auto loaded = reopened.readRange(reference, 9, {0, payload.size()});
    BOOST_CHECK_EQUAL_COLLECTIONS(
      loaded.begin(), loaded.end(), payload.begin(), payload.end());
  }
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(CommittedPayloadMetadataDistinguishesExactPartialAndMissingFiles)
{
  const auto root = temporaryRoot("metadata-probe");
  fs::remove_all(root);
  const std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04};
  const auto reference = makeReference(payload);
  FilesystemArtifactPayloadStore store(root);
  store.begin(reference, 1);
  store.writeRange(reference, 1, {0, payload.size()}, payload);
  store.markVerified(reference, 1, {0, payload.size()});
  store.flush(reference, 1);
  store.finalize(reference, 1);

  const auto path = store.committedPath(reference);
  auto metadata = store.inspectCommitted(reference);
  BOOST_CHECK(metadata.exists);
  BOOST_CHECK(metadata.regularFile);
  BOOST_CHECK_EQUAL(metadata.sizeBytes, payload.size());

  fs::resize_file(path, 2);
  metadata = store.inspectCommitted(reference);
  BOOST_CHECK(metadata.exists);
  BOOST_CHECK(metadata.regularFile);
  BOOST_CHECK_EQUAL(metadata.sizeBytes, 2U);

  fs::remove(path);
  metadata = store.inspectCommitted(reference);
  BOOST_CHECK(!metadata.exists);
  BOOST_CHECK(!metadata.regularFile);
  BOOST_CHECK_EQUAL(metadata.sizeBytes, 0U);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsDigestMismatchBeforeCommittedVisibility)
{
  const auto root = temporaryRoot("corruption");
  fs::remove_all(root);
  const std::vector<uint8_t> expected(4096, 0x11);
  const std::vector<uint8_t> corrupt(4096, 0x22);
  const auto reference = makeReference(expected);
  FilesystemArtifactPayloadStore store(root);
  store.begin(reference, 1);
  store.writeRange(reference, 1, {0, corrupt.size()}, corrupt);
  store.markVerified(reference, 1, {0, corrupt.size()});
  BOOST_CHECK_EXCEPTION(
    store.finalize(reference, 1),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("repo-artifact-digest-mismatch:") == 0;
    });
  BOOST_CHECK(!store.isCommitted(reference, 1));
  BOOST_CHECK(fs::exists(store.stagingPath(reference, 1)));
  store.abort(reference, 1);
  BOOST_CHECK(!fs::exists(store.stagingPath(reference, 1)));
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(EnforcesConfiguredPerOperationMemoryBound)
{
  const auto root = temporaryRoot("bounded-range");
  fs::remove_all(root);
  const std::vector<uint8_t> payload(8192, 0x33);
  const auto reference = makeReference(payload);
  FilesystemArtifactPayloadStore store(root, 4096);
  store.begin(reference, 1);
  BOOST_CHECK_EXCEPTION(
    store.writeRange(reference, 1, {0, payload.size()}, payload),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("repo-artifact-range-too-large:") == 0;
    });
  BOOST_CHECK_EXCEPTION(
    store.readRange(reference, 1, {0, payload.size()}),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("repo-artifact-range-too-large:") == 0;
    });
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(UsesTransactionalLifecycleMetadataNotPacketRows)
{
  const auto root = temporaryRoot("metadata");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore metadata(database);
    BOOST_CHECK_EQUAL(metadata.schemaGeneration(), 15);
    BOOST_CHECK(metadata.artifactWritesEnabled());
    ArtifactLifecycleEvent event{
      "event-1", "operation-1", std::string(64, 'a'), 1,
      ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved,
      1000, true, "one artifact-level event",
    };
    metadata.appendLifecycleEvent(event);
    BOOST_CHECK_EQUAL(
      toString(metadata.currentLifecycleState("operation-1")), "RESERVED");
  }
  {
    SqliteArtifactMetadataStore reopened(database);
    const auto events = reopened.lifecycleEvents("operation-1");
    BOOST_REQUIRE_EQUAL(events.size(), 1);
    BOOST_CHECK_EQUAL(events[0].eventId, "event-1");
  }
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_lifecycle_journal"), 1);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(MigrationIsAdditiveAndRollbackIsReadOnly)
{
  const auto root = temporaryRoot("migration");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  const std::vector<uint8_t> payload{0x53, 0x70, 0x65, 0x63, 0x31, 0x39, 0x32};
  const auto reference = makeReference(payload);
  const ArtifactLifecycleEvent event{
    "event-1", "operation-1", reference.contentDigest, 1,
    ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved,
    1000, true, "retained across schema migrations",
  };
  {
    SqliteArtifactMetadataStore metadata(database);
    metadata.appendLifecycleEvent(event);
    BOOST_CHECK_EQUAL(metadata.schemaGeneration(), 15);
  }

  executeSql(
    database,
    "DROP TABLE artifact_publish_receipts;"
    "DROP TABLE artifact_publish_targets;"
    "DROP TABLE artifact_publish_operations;"
    "DROP TABLE artifact_locations;"
    "DROP TABLE artifact_persistent_receipts;"
    "DROP TABLE artifact_local_commits;"
    "UPDATE repo_meta SET value='12' WHERE key='schema_generation'");
  {
    SqliteArtifactMetadataStore migrated(database);
    const auto diagnostics = migrated.migrationDiagnostics();
    BOOST_CHECK_EQUAL(diagnostics.previousSchemaGeneration, 12);
    BOOST_CHECK_EQUAL(diagnostics.databaseSchemaGeneration, 15);
    BOOST_CHECK_EQUAL(diagnostics.action, "roll-forward+schema-15");
    BOOST_CHECK(diagnostics.writesEnabled);
    BOOST_CHECK(!diagnostics.destructiveChanges);
    const auto events = migrated.lifecycleEvents("operation-1");
    BOOST_REQUIRE_EQUAL(events.size(), 1);
    BOOST_CHECK_EQUAL(events[0].eventId, event.eventId);
    BOOST_CHECK_EQUAL(events[0].detail, event.detail);
    BOOST_CHECK(!migrated.lookupLocalCommit(reference));
    const auto localCommit = migrated.recordLocalCommit(reference, 2345);
    BOOST_CHECK_EQUAL(localCommit.committedAtMs, 2345);
    BOOST_CHECK_EQUAL(migrated.schemaGeneration(), 15);
  }

  const auto schema13Database =
    (fs::path(root) / "metadata-schema13.sqlite3").string();
  {
    SqliteArtifactMetadataStore schema14(schema13Database);
    schema14.appendLifecycleEvent(event);
    schema14.recordLocalCommit(reference, 3456);
  }
  executeSql(
    schema13Database,
    "DROP TABLE artifact_publish_receipts;"
    "DROP TABLE artifact_publish_targets;"
    "DROP TABLE artifact_publish_operations;"
    "DROP TABLE artifact_locations;"
    "DROP TABLE artifact_persistent_receipts;"
    "UPDATE repo_meta SET value='13' WHERE key='schema_generation'");
  {
    SqliteArtifactMetadataStore migrated(schema13Database);
    const auto diagnostics = migrated.migrationDiagnostics();
    BOOST_CHECK_EQUAL(diagnostics.previousSchemaGeneration, 13);
    BOOST_CHECK_EQUAL(diagnostics.databaseSchemaGeneration, 15);
    BOOST_CHECK_EQUAL(diagnostics.action, "roll-forward+schema-15");
    BOOST_CHECK(migrated.artifactWritesEnabled());
    const auto events = migrated.lifecycleEvents("operation-1");
    BOOST_REQUIRE_EQUAL(events.size(), 1);
    BOOST_CHECK_EQUAL(events[0].eventId, event.eventId);
    BOOST_CHECK_EQUAL(events[0].detail, event.detail);
    const auto localCommit = migrated.lookupLocalCommit(reference);
    BOOST_REQUIRE(localCommit);
    BOOST_CHECK_EQUAL(localCommit->committedAtMs, 3456);
  }

  executeSql(
    schema13Database,
    "DROP TABLE artifact_publish_receipts;"
    "DROP TABLE artifact_publish_targets;"
    "DROP TABLE artifact_publish_operations;"
    "DROP TABLE artifact_locations;"
    "DROP TABLE artifact_persistent_receipts;"
    "UPDATE repo_meta SET value='9' WHERE key='schema_generation'");
  {
    SqliteArtifactMetadataStore repaired(schema13Database);
    const auto diagnostics = repaired.migrationDiagnostics();
    BOOST_CHECK_EQUAL(diagnostics.previousSchemaGeneration, 9);
    BOOST_CHECK_EQUAL(diagnostics.databaseSchemaGeneration, 15);
    BOOST_CHECK_EQUAL(diagnostics.action, "marker-repaired+schema-15");
    BOOST_CHECK(repaired.artifactWritesEnabled());
    const auto events = repaired.lifecycleEvents("operation-1");
    BOOST_REQUIRE_EQUAL(events.size(), 1);
    BOOST_CHECK_EQUAL(events[0].eventId, event.eventId);
    BOOST_CHECK_EQUAL(events[0].detail, event.detail);
    const auto localCommit = repaired.lookupLocalCommit(reference);
    BOOST_REQUIRE(localCommit);
    BOOST_CHECK_EQUAL(localCommit->committedAtMs, 3456);
  }

  executeSql(
    database,
    "UPDATE repo_meta SET value='16' WHERE key='schema_generation'");
  const auto before = snapshotDatabaseAndSidecars(database);
  {
    SqliteArtifactMetadataStore future(database, true, 15, true);
    const auto diagnostics = future.migrationDiagnostics();
    BOOST_CHECK_EQUAL(diagnostics.databaseSchemaGeneration, 16);
    BOOST_CHECK_EQUAL(diagnostics.action, "read-only-rollback");
    BOOST_CHECK_EQUAL(diagnostics.reason,
                      "database-schema-newer-than-write-runtime");
    BOOST_CHECK(!future.artifactWritesEnabled());
    BOOST_REQUIRE_EQUAL(future.lifecycleEvents("operation-1").size(), 1);
    BOOST_REQUIRE(future.lookupLocalCommit(reference));
    BOOST_CHECK_EXCEPTION(
      future.appendLifecycleEvent(ArtifactLifecycleEvent{
        "event-2", "operation-2", std::string(64, 'b'), 1,
        ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved,
        2000, true, "must fail",
      }),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find(
          "repo-artifact-writes-disabled:") == 0;
      });
    BOOST_CHECK_EXCEPTION(
      future.recordLocalCommit(reference, 5678),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find(
          "repo-artifact-writes-disabled:") == 0;
      });
  }
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_lifecycle_journal"), 1);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_local_commits"), 1);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 0);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(MigratesGeneration14ReceiptsAndLocationsTransactionallyTo15)
{
  const auto root = temporaryRoot("schema14-to15");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  const std::vector<uint8_t> payload{0x54, 0x30, 0x30, 0x34};
  const auto reference = makeReference(payload);
  const ArtifactLifecycleEvent event{
    "event-schema14", "operation-schema14", reference.contentDigest, 1,
    ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved,
    1000, true, "preserved by generation 14 to 15",
  };
  ArtifactLocationRecord legacyLocation;
  legacyLocation.repoNode = "/repo/schema14";
  legacyLocation.providerIdentity = "/provider/schema14";
  legacyLocation.operationId = std::string(64, 'b');
  legacyLocation.receiptId = "receipt-schema14";
  legacyLocation.reference = reference;
  legacyLocation.generation = 1;
  legacyLocation.locationVersion = 1;
  legacyLocation.committedAtMs = 2000;
  legacyLocation.expiresAtMs = 9000;
  legacyLocation.routeState = "ROUTABLE";
  legacyLocation.forwardingHint = "/NDNSF/DistributedRepo/Object/v1";
  {
    SqliteArtifactMetadataStore metadata(database);
    metadata.appendLifecycleEvent(event);
    metadata.recordLocalCommit(reference, 1500);
    metadata.recordPersistentCommit(legacyLocation);
    BOOST_CHECK_EQUAL(metadata.schemaGeneration(), 15U);
  }

  downgradePersistentCatalogToSchema14(database);

  {
    SqliteArtifactMetadataStore migrated(database);
    const auto diagnostics = migrated.migrationDiagnostics();
    BOOST_CHECK_EQUAL(diagnostics.previousSchemaGeneration, 14U);
    BOOST_CHECK_EQUAL(diagnostics.databaseSchemaGeneration, 15U);
    BOOST_CHECK_EQUAL(diagnostics.action, "roll-forward+schema-15");
    BOOST_CHECK(migrated.artifactWritesEnabled());
    BOOST_CHECK_EQUAL(migrated.schemaGeneration(), 15U);
    const auto events = migrated.lifecycleEvents("operation-schema14");
    BOOST_REQUIRE_EQUAL(events.size(), 1);
    BOOST_CHECK_EQUAL(events.front().eventId, event.eventId);
    const auto localCommit = migrated.lookupLocalCommit(reference);
    BOOST_REQUIRE(localCommit);
    BOOST_CHECK_EQUAL(localCommit->committedAtMs, 1500U);
    const auto currentLocation = migrated.lookupPersistentLocation(
      reference, legacyLocation.repoNode, legacyLocation.providerIdentity, 0);
    BOOST_REQUIRE(currentLocation);
    BOOST_CHECK_EQUAL(currentLocation->receiptId, legacyLocation.receiptId);
    BOOST_CHECK_EQUAL(currentLocation->locationVersion, 1U);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 1);
    BOOST_CHECK_EQUAL(queryCount(database,
      "artifact_persistent_receipts WHERE location_version IS NULL"), 0);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_locations"), 1);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_operations"), 0);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_targets"), 0);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_receipts"), 0);

    RepoArtifactCommitReceipt newerReceipt;
    newerReceipt.repoNode = legacyLocation.repoNode;
    newerReceipt.providerIdentity = legacyLocation.providerIdentity;
    newerReceipt.operationId = std::string(64, 'c');
    newerReceipt.receiptId = "receipt-schema14-repair";
    newerReceipt.reference = reference;
    newerReceipt.generation = 2;
    newerReceipt.persistedBytes = reference.sizeBytes;
    newerReceipt.completedAtMs = 3000;
    newerReceipt.locationVersion = 1;
    newerReceipt.routeState = "ROUTABLE";
    newerReceipt.forwardingHint = "/NDNSF/DistributedRepo/Object/v2";
    const auto repaired = migrated.commitPersistentArtifact(newerReceipt, 10000);
    BOOST_CHECK_EQUAL(repaired.receipt.locationVersion, 2U);
    BOOST_CHECK_EQUAL(repaired.currentLocation.receiptId, newerReceipt.receiptId);
    BOOST_CHECK_EQUAL(repaired.currentLocation.locationVersion, 2U);

    RepoArtifactCommitReceipt migratedLegacyReceipt;
    migratedLegacyReceipt.repoNode = legacyLocation.repoNode;
    migratedLegacyReceipt.providerIdentity = legacyLocation.providerIdentity;
    migratedLegacyReceipt.operationId = legacyLocation.operationId;
    migratedLegacyReceipt.receiptId = legacyLocation.receiptId;
    migratedLegacyReceipt.reference = reference;
    migratedLegacyReceipt.generation = legacyLocation.generation;
    migratedLegacyReceipt.persistedBytes = reference.sizeBytes;
    migratedLegacyReceipt.completedAtMs = legacyLocation.committedAtMs;
    migratedLegacyReceipt.locationVersion = legacyLocation.locationVersion;
    migratedLegacyReceipt.routeState = legacyLocation.routeState;
    migratedLegacyReceipt.forwardingHint = legacyLocation.forwardingHint;
    const auto replayedLegacy = migrated.commitPersistentArtifact(
      migratedLegacyReceipt, 11000);
    BOOST_CHECK_EQUAL(replayedLegacy.receipt.receiptId, legacyLocation.receiptId);
    BOOST_CHECK_EQUAL(replayedLegacy.receipt.operationId, legacyLocation.operationId);
    BOOST_CHECK_EQUAL(replayedLegacy.receipt.locationVersion,
                      legacyLocation.locationVersion);
    BOOST_CHECK_EQUAL(replayedLegacy.receipt.routeState, legacyLocation.routeState);
    BOOST_CHECK_EQUAL(replayedLegacy.receipt.forwardingHint,
                      legacyLocation.forwardingHint);
    BOOST_CHECK_EQUAL(replayedLegacy.currentLocation.receiptId,
                      newerReceipt.receiptId);
    BOOST_CHECK_EQUAL(replayedLegacy.currentLocation.locationVersion, 2U);

    sqlite3* rawDatabase = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open_v2(database.c_str(), &rawDatabase,
                                        SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* check = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(
      rawDatabase, "PRAGMA foreign_key_check", -1, &check, nullptr), SQLITE_OK);
    BOOST_CHECK_EQUAL(sqlite3_step(check), SQLITE_DONE);
    sqlite3_finalize(check);
    sqlite3_close(rawDatabase);
  }
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsSchema14ReceiptWithoutExactLocationTransactionally)
{
  const auto root = temporaryRoot("schema14-receipt-without-location");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  const std::vector<uint8_t> payload{0x14, 0x15};
  const auto reference = makeReference(payload);
  ArtifactLocationRecord location;
  location.repoNode = "/repo/schema14-missing-location";
  location.providerIdentity = "/provider/schema14-missing-location";
  location.operationId = std::string(64, 'd');
  location.receiptId = "receipt-schema14-missing-location";
  location.reference = reference;
  location.generation = 1;
  location.locationVersion = 1;
  location.committedAtMs = 2000;
  location.expiresAtMs = 9000;
  location.routeState = "ROUTABLE";
  location.forwardingHint = "/NDNSF/DistributedRepo/Object/v1";
  {
    SqliteArtifactMetadataStore metadata(database);
    metadata.recordPersistentCommit(location);
  }
  downgradePersistentCatalogToSchema14(database);
  executeSql(database, "DELETE FROM artifact_locations");

  const auto openMalformed = [&database] {
    SqliteArtifactMetadataStore unsupported(database);
  };
  BOOST_CHECK_EXCEPTION(
    openMalformed(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-migration-location-snapshot-missing:") == 0;
    });
  BOOST_CHECK_EQUAL(queryCount(
    database, "repo_meta WHERE key='schema_generation' AND value='14'"), 1);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 1);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_locations"), 0);
  BOOST_CHECK_EQUAL(queryCount(
    database,
    "sqlite_master WHERE type='table' AND name='artifact_publish_operations'"), 0);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(UnknownLowerSchemaShapeFailsReadOnlyWithoutMutation)
{
  const auto root = temporaryRoot("unknown-schema");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  executeSql(
    database,
    "CREATE TABLE repo_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
    "INSERT INTO repo_meta(key,value) VALUES('schema_generation','11');"
    "CREATE TABLE unrelated(value TEXT)");
  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 14, true);
  };
  BOOST_CHECK_THROW(openUnsupported(), std::runtime_error);
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsPersistentCatalogWithoutConstraintsBeforePromotion)
{
  const auto root = temporaryRoot("persistent-catalog-missing-constraints");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK_EQUAL(initialized.schemaGeneration(), 15);
  }
  executeSql(
    database,
    "ALTER TABLE artifact_locations RENAME TO valid_artifact_locations;"
    "ALTER TABLE artifact_persistent_receipts "
      "RENAME TO valid_artifact_persistent_receipts;"
    "CREATE TABLE artifact_persistent_receipts AS SELECT * "
      "FROM valid_artifact_persistent_receipts WHERE 0;"
    "CREATE TABLE artifact_locations AS SELECT * FROM valid_artifact_locations WHERE 0;"
    "DROP TABLE valid_artifact_locations;"
    "DROP TABLE valid_artifact_persistent_receipts;"
    "UPDATE repo_meta SET value='13' WHERE key='schema_generation'");

  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 14, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile: Persistent catalog constraints") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsPersistentCatalogWithWrongIndexCollationBeforePromotion)
{
  const auto root = temporaryRoot("persistent-catalog-wrong-collation");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK_EQUAL(initialized.schemaGeneration(), 15);
  }
  executeSql(
    database,
    "ALTER TABLE artifact_locations RENAME TO valid_artifact_locations;"
    "CREATE TABLE artifact_locations("
    "repo_node TEXT NOT NULL,provider_identity TEXT NOT NULL,"
    "logical_name TEXT COLLATE NOCASE NOT NULL,digest_algorithm TEXT NOT NULL,"
    "content_digest TEXT NOT NULL,size_bytes INTEGER NOT NULL,"
    "format_version TEXT NOT NULL,root_manifest_name TEXT NOT NULL,"
    "publisher_identity TEXT NOT NULL,policy_epoch TEXT NOT NULL,"
    "receipt_id TEXT NOT NULL,operation_id TEXT NOT NULL,generation INTEGER NOT NULL,"
    "location_version INTEGER NOT NULL,committed_at_ms INTEGER NOT NULL,"
    "expires_at_ms INTEGER NOT NULL,route_state TEXT NOT NULL,forwarding_hint TEXT NOT NULL,"
    "PRIMARY KEY(repo_node,provider_identity,logical_name,digest_algorithm,content_digest,"
    "size_bytes,format_version,root_manifest_name,publisher_identity,policy_epoch),"
    "FOREIGN KEY(receipt_id) REFERENCES artifact_persistent_receipts(receipt_id));"
    "DROP TABLE valid_artifact_locations;"
    "UPDATE repo_meta SET value='13' WHERE key='schema_generation'");

  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 14, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile: Persistent catalog constraints") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsSchema15PublishJournalWithoutConstraintsReadOnly)
{
  const auto root = temporaryRoot("schema15-journal-missing-constraints");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK_EQUAL(initialized.schemaGeneration(), 15U);
  }
  executeSql(
    database,
    "ALTER TABLE artifact_publish_receipts RENAME TO valid_artifact_publish_receipts;"
    "CREATE TABLE artifact_publish_receipts("
      "receipt_id TEXT NOT NULL,operation_id TEXT NOT NULL,"
      "receipt_json TEXT NOT NULL,terminal_at_ms INTEGER NOT NULL);"
    "DROP TABLE valid_artifact_publish_receipts;");

  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 15, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile: Persistent catalog constraints") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsSchema15OperationUniqueIndexWithoutPrimaryKeyReadOnly)
{
  const auto root = temporaryRoot("schema15-operation-unique-no-primary-key");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK_EQUAL(initialized.schemaGeneration(), 15U);
  }
  executeSql(
    database,
    "DROP TABLE artifact_publish_receipts;"
    "DROP TABLE artifact_publish_targets;"
    "ALTER TABLE artifact_publish_operations RENAME TO valid_artifact_publish_operations;"
    "DROP TABLE valid_artifact_publish_operations;"
    "CREATE TABLE artifact_publish_operations("
      "operation_id TEXT NOT NULL,submitting_principal TEXT NOT NULL,"
      "idempotency_key_digest TEXT NOT NULL,request_digest TEXT NOT NULL,"
      "intent_json TEXT NOT NULL,state TEXT NOT NULL,created_at_ms INTEGER NOT NULL,"
      "updated_at_ms INTEGER NOT NULL,accepted_at_ms INTEGER,terminal_at_ms INTEGER,"
      "status_json TEXT NOT NULL DEFAULT '',"
      "UNIQUE(submitting_principal,idempotency_key_digest));"
    "CREATE UNIQUE INDEX schema15_operation_id_unique_without_primary_key "
      "ON artifact_publish_operations(operation_id);"
    "CREATE INDEX idx_artifact_publish_operations_state "
      "ON artifact_publish_operations(state,updated_at_ms);"
    "CREATE TABLE artifact_publish_targets("
      "operation_id TEXT NOT NULL,repo_node TEXT NOT NULL,"
      "provider_identity TEXT NOT NULL,service_prefix TEXT NOT NULL,state TEXT NOT NULL,"
      "attempt_count INTEGER NOT NULL CHECK(attempt_count>=0 AND attempt_count<=65535),"
      "updated_at_ms INTEGER NOT NULL,receipt_json TEXT NOT NULL DEFAULT '',"
      "failure_code TEXT NOT NULL DEFAULT '',"
      "PRIMARY KEY(operation_id,repo_node,provider_identity,service_prefix),"
      "FOREIGN KEY(operation_id) REFERENCES artifact_publish_operations(operation_id) "
        "ON DELETE CASCADE);"
    "CREATE INDEX idx_artifact_publish_targets_state "
      "ON artifact_publish_targets(operation_id,state);"
    "CREATE TABLE artifact_publish_receipts("
      "receipt_id TEXT PRIMARY KEY,operation_id TEXT NOT NULL UNIQUE,"
      "receipt_json TEXT NOT NULL,terminal_at_ms INTEGER NOT NULL,"
      "FOREIGN KEY(operation_id) REFERENCES artifact_publish_operations(operation_id));");

  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 15, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile: Persistent catalog constraints") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsSchema15TargetUniqueIndexWithoutPrimaryKeyReadOnly)
{
  const auto root = temporaryRoot("schema15-target-unique-no-primary-key");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK_EQUAL(initialized.schemaGeneration(), 15U);
  }
  executeSql(
    database,
    "ALTER TABLE artifact_publish_targets RENAME TO valid_artifact_publish_targets;"
    "DROP TABLE valid_artifact_publish_targets;"
    "CREATE TABLE artifact_publish_targets("
      "operation_id TEXT NOT NULL,repo_node TEXT NOT NULL,"
      "provider_identity TEXT NOT NULL,service_prefix TEXT NOT NULL,state TEXT NOT NULL,"
      "attempt_count INTEGER NOT NULL CHECK(attempt_count>=0 AND attempt_count<=65535),"
      "updated_at_ms INTEGER NOT NULL,receipt_json TEXT NOT NULL DEFAULT '',"
      "failure_code TEXT NOT NULL DEFAULT '',"
      "FOREIGN KEY(operation_id) REFERENCES artifact_publish_operations(operation_id) "
        "ON DELETE CASCADE);"
    "CREATE UNIQUE INDEX schema15_target_tuple_unique_without_primary_key "
      "ON artifact_publish_targets(operation_id,repo_node,provider_identity,service_prefix);"
    "CREATE INDEX idx_artifact_publish_targets_state "
      "ON artifact_publish_targets(operation_id,state)");

  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 15, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile: Persistent catalog constraints") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsPartialLocalCommitUniqueIndexWithoutMutation)
{
  const auto root = temporaryRoot("partial-local-commit-index");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK(initialized.artifactWritesEnabled());
  }
  executeSql(
    database,
    "DROP TABLE artifact_local_commits;"
    "CREATE TABLE artifact_local_commits("
    "local_commit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "logical_name TEXT NOT NULL,digest_algorithm TEXT NOT NULL,"
    "content_digest TEXT NOT NULL,size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),"
    "format_version TEXT NOT NULL,root_manifest_name TEXT NOT NULL,"
    "publisher_identity TEXT NOT NULL,policy_epoch TEXT NOT NULL,"
    "committed_at_ms INTEGER NOT NULL CHECK(committed_at_ms>0));"
    "CREATE UNIQUE INDEX partial_artifact_reference ON artifact_local_commits("
    "logical_name,digest_algorithm,content_digest,size_bytes,format_version,"
    "root_manifest_name,publisher_identity,policy_epoch) WHERE size_bytes>=0");
  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openUnsupported = [&database] {
    SqliteArtifactMetadataStore unsupported(database, true, 13, true);
  };
  BOOST_CHECK_EXCEPTION(
    openUnsupported(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-unsupported-profile:") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(RejectsNegativeSchemaGenerationWithoutMutation)
{
  const auto root = temporaryRoot("negative-schema-generation");
  fs::remove_all(root);
  fs::create_directories(root);
  const auto database = (fs::path(root) / "metadata.sqlite3").string();
  {
    SqliteArtifactMetadataStore initialized(database);
    BOOST_CHECK(initialized.artifactWritesEnabled());
  }
  executeSql(
    database,
    "UPDATE repo_meta SET value='-1' WHERE key='schema_generation'");
  const auto before = snapshotDatabaseAndSidecars(database);
  const auto openMalformed = [&database] {
    SqliteArtifactMetadataStore malformed(database, true, 13, true);
  };
  BOOST_CHECK_EXCEPTION(
    openMalformed(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-schema-invalid-generation:") == 0;
    });
  BOOST_CHECK(snapshotDatabaseAndSidecars(database) == before);
  fs::remove_all(root);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
