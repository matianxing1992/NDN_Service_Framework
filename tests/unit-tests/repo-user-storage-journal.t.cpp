#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include <boost/test/unit_test.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>

namespace {
using namespace ndnsf_distributed_repo;
namespace fs = std::filesystem;

struct Fixture
{
  fs::path root;
  std::string database;

  Fixture()
  {
    auto pattern = (fs::temp_directory_path() / "spec221-user-storage-XXXXXX").string();
    const auto path = ::mkdtemp(pattern.data());
    if (!path) throw std::runtime_error("cannot create user storage fixture");
    root = path;
    database = (root / "metadata.sqlite3").string();
  }

  ~Fixture()
  {
    std::error_code error;
    fs::remove_all(root, error);
  }

  void sql(const std::string& sql) const
  {
    sqlite3* raw = nullptr;
    if (sqlite3_open(database.c_str(), &raw) != SQLITE_OK) {
      if (raw) sqlite3_close(raw);
      throw std::runtime_error("fixture sqlite open failed");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    if (sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db.get()));
  }

  uint64_t integer(const char* sql) const
  {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      if (raw) sqlite3_close(raw);
      throw std::runtime_error("fixture sqlite read failed");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db.get()));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> s(statement, sqlite3_finalize);
    if (sqlite3_step(s.get()) != SQLITE_ROW)
      throw std::runtime_error("fixture expected integer row");
    return static_cast<uint64_t>(sqlite3_column_int64(s.get(), 0));
  }

  void downgradeTo15() const
  {
    sql("DROP TABLE repo_user_storage_writes; DROP TABLE repo_user_storage_claims;"
        "DROP TABLE repo_user_storage_grants; DROP TABLE repo_user_storage_state;"
        "DROP TABLE repo_user_storage_quarantine;"
        "UPDATE repo_meta SET value='15' WHERE key='schema_generation';");
  }
};

auto hasCode(const char* code)
{
  return [code](const std::exception& e) { return std::string(e.what()).find(code) == 0; };
}

RepoUserStorageObject object(const std::string& name, uint64_t charge)
{
  return {name, std::string(64, 'a'), 1, charge - 1, 1};
}

void grant(SqliteArtifactMetadataStore& store, const RepoUserStorageObject& o,
           const std::string& grantId = "grant", const std::string& user = "/user/u",
           const std::string& node = "/repo/a", const std::string& writer = "/provider/a")
{
  (void)store.grantUserStorage(user, node, grantId, writer, o.objectName, 10000, 1000);
}

void reserve(SqliteArtifactMetadataStore& store, const RepoUserStorageObject& o,
             const std::string& operationId = "write", const std::string& grantId = "grant",
             const std::string& user = "/user/u", const std::string& node = "/repo/a")
{
  (void)store.reserveUserStorage("/provider/a", node, user, grantId, operationId, o, 1001);
}

// Fill a real metadata account without allocating model-sized payloads.
void fillCommittedBudget(SqliteArtifactMetadataStore& store, uint64_t bytes)
{
  for (unsigned index = 0; bytes != 0; ++index) {
    const auto charge = std::min(bytes, REPO_USER_STORAGE_OBJECT_MAX_BYTES);
    const auto id = "prefill-" + std::to_string(index);
    const auto item = object("/producer/prefill/" + std::to_string(index), charge);
    grant(store, item, id);
    reserve(store, item, id, id);
    (void)store.commitUserStorage("/repo/a", "/user/u", id, 1002);
    bytes -= charge;
  }
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec221RepoUserStorageJournal)

BOOST_AUTO_TEST_CASE(PersistentNodeFactoryCreatesMigratesAndReopensCurrentCatalog)
{
  Fixture fixture;
  StorageCapability capability;
  capability.repoNode = "/repo/a";
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto open = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/SERVICE/repo"), capability,
      makeSqliteRepoStore((fixture.root / "payload.sqlite3").string()),
      "/repo/a", "/provider/a", fixture.database);
  };
  {
    const auto node = open();
    BOOST_CHECK_EQUAL(fixture.integer(
      "SELECT value FROM repo_meta WHERE key='schema_generation'"),
      REPO_ARTIFACT_SCHEMA_GENERATION);
  }
  fixture.downgradeTo15();
  {
    const auto node = open();
    BOOST_CHECK_EQUAL(fixture.integer(
      "SELECT value FROM repo_meta WHERE key='schema_generation'"),
      REPO_ARTIFACT_SCHEMA_GENERATION);
  }
  BOOST_CHECK_NO_THROW(open());
}

BOOST_AUTO_TEST_CASE(InAppFactoryCreatesMigratesAndReopensCurrentCatalog)
{
  Fixture fixture;
  const auto open = [&] {
    return ArtifactRepositoryApi::makeInApp({fixture.root.string(), "spec221-owner", {}});
  };
  {
    const auto api = open();
    BOOST_CHECK_EQUAL(fixture.integer(
      "SELECT value FROM repo_meta WHERE key='schema_generation'"),
      REPO_ARTIFACT_SCHEMA_GENERATION);
  }
  fixture.downgradeTo15();
  {
    const auto api = open();
    BOOST_CHECK_EQUAL(fixture.integer(
      "SELECT value FROM repo_meta WHERE key='schema_generation'"),
      REPO_ARTIFACT_SCHEMA_GENERATION);
  }
  BOOST_CHECK_NO_THROW(open());
}

BOOST_AUTO_TEST_CASE(ExactLimitRejectsAdditionalByteWithoutMutatingRows)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_CHECK_EQUAL(REPO_USER_STORAGE_LIMIT_BYTES, 100000000000ULL);
  BOOST_CHECK(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  fillCommittedBudget(store, 99000000000ULL);
  const auto full = object("/producer/full", 1000000000ULL);
  const auto extra = object("/producer/extra", 1);
  grant(store, full); grant(store, extra, "extra");
  reserve(store, full);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 1000000000ULL);
  BOOST_CHECK_EXCEPTION(reserve(store, extra, "extra-write", "extra"),
                        std::runtime_error, hasCode("QUOTA_EXCEEDED"));
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 100U);
  BOOST_CHECK_EQUAL(f.integer("SELECT payload_wire_bytes FROM repo_user_storage_writes"), 999999999ULL);
  BOOST_CHECK_EQUAL(f.integer("SELECT manifest_bytes FROM repo_user_storage_writes"), 1U);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 100000000000ULL);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(AccountsAreIndependentAcrossUsersAndNodes)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/b", {}, 1000).ready);
  const auto full = object("/producer/full", 1000000000ULL);
  grant(store, full); reserve(store, full);
  grant(store, full, "v", "/user/v"); reserve(store, full, "v-write", "v", "/user/v");
  grant(store, full, "b", "/user/u", "/repo/b");
  reserve(store, full, "b-write", "b", "/user/u", "/repo/b");
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes WHERE reserved_bytes=1000000000"), 3U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/unused", "/repo/a").reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(DefaultHundredGigabyteAccountSurvivesRestartWithoutLargerObjects)
{
  Fixture f;
  const auto a = object("/producer/a", 600000000);
  const auto b = object("/producer/b", 600000000);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    grant(store, a, "a"); grant(store, b, "b");
    reserve(store, a, "a", "a"); reserve(store, b, "b", "b");
    reserve(store, a, "a", "a");
    BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 1200000000ULL);
    (void)store.commitUserStorage("/repo/a", "/user/u", "a", 1002);
    (void)store.commitUserStorage("/repo/a", "/user/u", "b", 1002);
  }
  SqliteArtifactMetadataStore reopened(f.database);
  BOOST_REQUIRE(reopened.reconcileUserStorageInventory("/repo/a", {{a}, {b}}, 1003).ready);
  const auto usage = reopened.userStorageUsage("/user/u", "/repo/a");
  BOOST_CHECK_EQUAL(usage.limitBytes, 100000000000ULL);
  BOOST_CHECK_EQUAL(usage.committedBytes, 1200000000ULL);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  RepoUserStorageUsageReply reply;
  reply.usage = usage;
  BOOST_CHECK_EQUAL(RepoUserStorageUsageReply::fromJson(reply.toJson()).usage.limitBytes,
                    100000000000ULL);
  BOOST_CHECK_THROW(validateRepoUserStorageObject(object("/producer/oversized", 1000000001ULL)),
                    std::invalid_argument);
  const auto oversized = object("/producer/oversized", 1000000001ULL);
  grant(reopened, oversized, "oversized");
  BOOST_CHECK_THROW(reserve(reopened, oversized, "oversized", "oversized"), std::invalid_argument);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(GrantScopeAndWriterAreNotCallerSelectedOwnership)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/b", {}, 1000).ready);
  const auto a = object("/producer/a", 25);
  grant(store, a);
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/spoof", "/repo/a", "/user/u",
    "grant", "spoof", a, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/a", "/repo/a", "/user/v",
    "grant", "other-owner", a, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/a", "/repo/b", "/user/u",
    "grant", "other-node", a, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(reserve(store, object("/producer/other", 25)),
    std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/a", "/repo/a", "/user/u",
    "grant", "expired", a, 10000), std::runtime_error, hasCode("repo-user-storage-grant-expired"));
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 0U);
}

BOOST_AUTO_TEST_CASE(ExpiredUnusedGrantCanBeExplicitlyRenewed)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1).ready);
  const auto a = object("/producer/a", 25);
  (void)store.grantUserStorage("/user/u", "/repo/a", "old", "/provider/a", a.objectName, 3, 1);
  const auto renewed = store.grantUserStorage("/user/u", "/repo/a", "new", "/provider/b", a.objectName, 20, 4);
  BOOST_CHECK_EQUAL(renewed.ownerEpoch, 1U);
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/a", "/repo/a", "/user/u",
    "old", "expired", a, 4), std::runtime_error, hasCode("repo-user-storage-grant-expired"));
  (void)store.reserveUserStorage("/provider/b", "/repo/a", "/user/u", "new", "new-write", a, 5);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 25U);
}

BOOST_AUTO_TEST_CASE(IdempotencyAndCancellationDoNotDuplicateCharge)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 70);
  grant(store, a); reserve(store, a); reserve(store, a);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 1U);
  auto altered = a; ++altered.manifestBytes;
  BOOST_CHECK_EXCEPTION(reserve(store, altered), std::runtime_error, hasCode("repo-user-storage-operation-conflict"));
  BOOST_CHECK_EXCEPTION(reserve(store, a, "parallel"), std::runtime_error, hasCode("repo-user-storage-object-conflict"));
  (void)store.cancelUserStorage("/repo/a", "/user/u", "write", 1002);
  (void)store.cancelUserStorage("/repo/a", "/user/u", "write", 1003);
  reserve(store, a);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
  reserve(store, a, "fresh");
  (void)store.commitUserStorage("/repo/a", "/user/u", "fresh", 1002);
  (void)store.commitUserStorage("/repo/a", "/user/u", "fresh", 1003);
  reserve(store, a, "reference");
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 70U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims"), 1U);
}

BOOST_AUTO_TEST_CASE(IndependentConnectionsSerializeQuotaAdmission)
{
  Fixture f;
  SqliteArtifactMetadataStore first(f.database), second(f.database);
  BOOST_REQUIRE(first.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  BOOST_REQUIRE(second.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  fillCommittedBudget(first, 99000000000ULL);
  const auto a = object("/producer/a", 600000000);
  const auto b = object("/producer/b", 600000000);
  grant(first, a, "a"); grant(first, b, "b");
  std::promise<void> start;
  const auto ready = start.get_future().share();
  const auto submit = [&](SqliteArtifactMetadataStore& store, const RepoUserStorageObject& o,
                          const std::string& id) {
    ready.wait();
    try { reserve(store, o, id, id); return std::string("admitted"); }
    catch (const std::exception& e) { return std::string(e.what()); }
  };
  auto x = std::async(std::launch::async, [&] { return submit(first, a, "a"); });
  auto y = std::async(std::launch::async, [&] { return submit(second, b, "b"); });
  start.set_value();
  const auto rx = x.get(); const auto ry = y.get();
  BOOST_CHECK((rx == "admitted" && ry.find("QUOTA_EXCEEDED") == 0) ||
              (ry == "admitted" && rx.find("QUOTA_EXCEEDED") == 0));
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 100U);
  BOOST_CHECK_EQUAL(first.userStorageUsage("/user/u", "/repo/a").reservedBytes, 600000000U);
}

BOOST_AUTO_TEST_CASE(RestartRequiresReconciliationAndPreservesPendingCharge)
{
  Fixture f;
  const auto a = object("/producer/a", 75);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    grant(store, a); reserve(store, a);
  }
  SqliteArtifactMetadataStore reopened(f.database);
  BOOST_CHECK(!reopened.userStorageReconciliation("/repo/a").ready);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").reservedBytes, 75U);
  BOOST_REQUIRE_EQUAL(reopened.pendingUserStorageWrites("/repo/a").size(), 1U);
  BOOST_CHECK_EXCEPTION(reserve(reopened, a), std::runtime_error, hasCode("repo-user-storage-reconciliation-required"));
  BOOST_CHECK(!reopened.reconcileUserStorageInventory("/repo/a", {{a}}, 1002).ready);
  (void)reopened.commitUserStorage("/repo/a", "/user/u", "write", 1003);
  BOOST_CHECK(reopened.reconcileUserStorageInventory("/repo/a", {{a}}, 1004).ready);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").committedBytes, 75U);
}

BOOST_AUTO_TEST_CASE(CleanupRetainsChargeUntilConfirmationAndFencesOldEpoch)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 90);
  grant(store, a); reserve(store, a);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  BOOST_CHECK_EXCEPTION(store.prepareUserStorageCleanup("/user/v", "/repo/a", "clear", a.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-object-missing"));
  (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", a.objectName, 1, 1003);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 90U);
  BOOST_CHECK_EXCEPTION(reserve(store, a), std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK_EXCEPTION(store.commitUserStorage("/repo/a", "/user/u", "write", 1004),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1004),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  (void)store.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1004);
  (void)store.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1005);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
  BOOST_CHECK(store.listUserStorage("/user/u", "/repo/a", "").empty());
  BOOST_CHECK_EXCEPTION(reserve(store, a), std::runtime_error, hasCode("repo-user-storage-deleted"));
  const auto next = store.grantUserStorage("/user/u", "/repo/a", "new", "/provider/a", a.objectName, 10000, 1005);
  BOOST_CHECK_EQUAL(next.ownerEpoch, 2U);
  (void)store.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "new", "next", a, 1005);
  (void)store.commitUserStorage("/repo/a", "/user/u", "next", 1006);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 90U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims WHERE state='Deleted'"), 1U);
}

BOOST_AUTO_TEST_CASE(ReservedCleanupTransfersFullChargeWithoutFreeingCapacity)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  (void)store.reconcileUserStorageInventory("/repo/a", {}, 1000);
  fillCommittedBudget(store, 99000000000ULL);
  const auto full = object("/producer/pending", 1000000000ULL);
  const auto extra = object("/producer/extra", 1);
  grant(store, full); grant(store, extra, "extra"); reserve(store, full);
  const auto prepared = store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", full.objectName, 1, 1002);
  BOOST_CHECK(prepared.state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_CHECK_EQUAL(prepared.object.objectName, full.objectName);
  BOOST_CHECK_EQUAL(prepared.object.wireSha256, full.wireSha256);
  BOOST_CHECK_EQUAL(prepared.object.payloadWireBytes, full.payloadWireBytes);
  BOOST_CHECK_EQUAL(prepared.object.manifestBytes, full.manifestBytes);
  BOOST_CHECK_EQUAL(prepared.grantId, "grant"); BOOST_CHECK_EQUAL(prepared.committedAtMs, 1002U);
  const auto usage = store.userStorageUsage("/user/u", "/repo/a");
  BOOST_CHECK_EQUAL(usage.committedBytes, 100000000000ULL);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  BOOST_CHECK_EQUAL(usage.committedBytes + usage.reservedBytes, usage.limitBytes);
  const auto cancelled = store.lookupUserStorageWrite("/repo/a", "/user/u", "write");
  BOOST_REQUIRE(cancelled); BOOST_CHECK(cancelled->state == RepoUserStorageWriteState::CANCELLED);
  BOOST_CHECK_EQUAL(cancelled->reservedBytes, 0U);
  const auto listed = store.listUserStorage("/user/u", "/repo/a", {});
  BOOST_REQUIRE_EQUAL(listed.size(), 100U);
  BOOST_CHECK(listed.front().state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_CHECK_EQUAL(listed.front().cleanupId, "clear");
  BOOST_CHECK_EQUAL(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", full.objectName, 1, 1003).committedAtMs, 1002U);
  BOOST_CHECK_EXCEPTION(reserve(store, extra, "extra-write", "extra"), std::runtime_error, hasCode("QUOTA_EXCEEDED"));
  BOOST_CHECK_EXCEPTION(store.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", full.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK_EXCEPTION(store.commitUserStorage("/repo/a", "/user/u", "write", 1003),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1003),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK_EXCEPTION(store.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "grant", "new-write", full, 1003),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  BOOST_CHECK(store.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "grant", "write", full, 1003).state == RepoUserStorageWriteState::CANCELLED);
  BOOST_CHECK_EQUAL(store.userStorageReconciliation("/repo/a").pendingCleanupCount, 1U);
}

BOOST_AUTO_TEST_CASE(ReservedCleanupSurvivesRestartAndIsolatesOwnersNodesAndEpochs)
{
  Fixture f;
  const auto pending = object("/producer/pending", 30);
  {
    SqliteArtifactMetadataStore store(f.database);
    (void)store.reconcileUserStorageInventory("/repo/a", {}, 1000);
    (void)store.reconcileUserStorageInventory("/repo/b", {}, 1000);
    grant(store, pending); reserve(store, pending);
    grant(store, pending, "v", "/user/v"); reserve(store, pending, "v-write", "v", "/user/v");
    grant(store, pending, "b", "/user/u", "/repo/b"); reserve(store, pending, "b-write", "b", "/user/u", "/repo/b");
    (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002);
  }
  SqliteArtifactMetadataStore reopened(f.database, true, 16, false);
  const auto prepared = reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1003);
  BOOST_CHECK(prepared.state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").committedBytes, 30U);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/v", "/repo/a").reservedBytes, 30U);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/b").reservedBytes, 30U);
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/x", "/repo/a", "clear", pending.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-object-missing"));
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/c", "clear", pending.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-object-missing"));
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "other", pending.objectName, 2, 1003),
    std::runtime_error, hasCode("repo-user-storage-object-missing"));
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "other", pending.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", "/producer/other", 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  const auto peers = reopened.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
  BOOST_REQUIRE_EQUAL(peers.reservedWrites.size(), 1U);
  BOOST_CHECK_EQUAL(peers.reservedWrites.front().grant.ownerIdentity, "/user/v");
  BOOST_CHECK_EXCEPTION(reopened.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", pending.objectName, 1, 1003),
    std::runtime_error, hasCode("repo-user-storage-deleted"));
  // Trusted ledger confirmation here does not stand in for a physical cleanup test.
  const auto deleted = reopened.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1004);
  BOOST_CHECK(deleted.state == RepoUserStorageClaimState::DELETED);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/v", "/repo/a").reservedBytes, 30U);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/b").reservedBytes, 30U);
  BOOST_CHECK(reopened.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1005).state == RepoUserStorageClaimState::DELETED);
  BOOST_CHECK_EQUAL(reopened.grantUserStorage("/user/u", "/repo/a", "fresh", "/provider/a", pending.objectName, 10000, 1005).ownerEpoch, 2U);
}

BOOST_AUTO_TEST_CASE(ReservedCleanupRollsBackCancellationWhenClaimInsertFails)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  (void)store.reconcileUserStorageInventory("/repo/a", {}, 1000);
  const auto pending = object("/producer/pending", 30); grant(store, pending); reserve(store, pending);
  f.sql("CREATE TRIGGER fail_reserved_cleanup BEFORE INSERT ON repo_user_storage_claims "
        "WHEN NEW.cleanup_id='clear' BEGIN SELECT RAISE(ABORT,'fixture-cleanup-insert-failure'); END;");
  BOOST_CHECK_THROW(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002), std::runtime_error);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 30U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims"), 0U);
  const auto retained = store.lookupUserStorageWrite("/repo/a", "/user/u", "write");
  BOOST_REQUIRE(retained); BOOST_CHECK(retained->state == RepoUserStorageWriteState::RESERVED);
  BOOST_CHECK_EQUAL(retained->updatedAtMs, 1001U); BOOST_CHECK_EQUAL(retained->reservedBytes, 30U);
  BOOST_CHECK_NO_THROW(store.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", pending.objectName, 1, 1002));
  f.sql("DROP TRIGGER fail_reserved_cleanup;");
  BOOST_CHECK(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002).state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 30U);
}

BOOST_AUTO_TEST_CASE(ReservedCleanupRejectsCorruptGrantChargeAndBackwardTime)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  (void)store.reconcileUserStorageInventory("/repo/a", {}, 1000);
  const auto pending = object("/producer/pending", 30); grant(store, pending); reserve(store, pending);
  f.sql("UPDATE repo_user_storage_grants SET owner_epoch=2;");
  BOOST_CHECK_EXCEPTION(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EQUAL(f.integer("SELECT reserved_bytes FROM repo_user_storage_writes"), 30U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims"), 0U);
  f.sql("UPDATE repo_user_storage_grants SET owner_epoch=1;"
        "UPDATE repo_user_storage_writes SET payload_wire_bytes=1000000000,manifest_bytes=1,reserved_bytes=1000000001;");
  BOOST_CHECK_EXCEPTION(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EQUAL(f.integer("SELECT reserved_bytes FROM repo_user_storage_writes"), 1000000001ULL);
  f.sql("UPDATE repo_user_storage_writes SET payload_wire_bytes=29,manifest_bytes=1,reserved_bytes=30;");
  BOOST_CHECK_EXCEPTION(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1000),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 30U);
  BOOST_CHECK(store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002).state == RepoUserStorageClaimState::CLEANUP_PREPARED);
}

BOOST_AUTO_TEST_CASE(SharedObjectChargesAndListsEachOwnerSeparately)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 50);
  grant(store, a); reserve(store, a);
  grant(store, a, "v", "/user/v"); reserve(store, a, "v", "v", "/user/v");
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  (void)store.commitUserStorage("/repo/a", "/user/v", "v", 1002);
  BOOST_REQUIRE_EQUAL(store.listUserStorage("/user/u", "/repo/a", "").size(), 1U);
  BOOST_CHECK_EQUAL(store.listUserStorage("/user/u", "/repo/a", "")[0].ownerIdentity, "/user/u");
  BOOST_CHECK(store.listUserStorage("/user/u", "/repo/a", a.objectName).empty());
  (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", a.objectName, 1, 1003);
  (void)store.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1004);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/v", "/repo/a").committedBytes, 50U);
  BOOST_CHECK(store.reconcileUserStorageInventory("/repo/a", {{a}}, 1005).ready);
}

BOOST_AUTO_TEST_CASE(CleanupSourcesRecoverExactCancelledOwnerNodeEpochAndTuple)
{
  Fixture f;
  const auto pending = object("/producer/pending", 30);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/b", {}, 1000).ready);
    grant(store, pending); reserve(store, pending, "old");
    (void)store.cancelUserStorage("/repo/a", "/user/u", "old", 1002);
    (void)store.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "grant", "current", pending, 1003);
    grant(store, pending, "v", "/user/v"); reserve(store, pending, "v-write", "v", "/user/v");
    (void)store.cancelUserStorage("/repo/a", "/user/v", "v-write", 1004);
    grant(store, pending, "b", "/user/u", "/repo/b"); reserve(store, pending, "b-write", "b", "/user/u", "/repo/b");
    (void)store.cancelUserStorage("/repo/b", "/user/u", "b-write", 1004);
    const auto other = object("/producer/other", 30);
    grant(store, other, "other"); reserve(store, other, "other-write", "other");
    (void)store.cancelUserStorage("/repo/a", "/user/u", "other-write", 1004);
    (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1004);
  }
  SqliteArtifactMetadataStore readonly(f.database, false, 16, false);
  const auto peers = readonly.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
  BOOST_REQUIRE_EQUAL(peers.cancelledWrites.size(), 1U);
  const auto& source = peers.cancelledWrites.front();
  BOOST_CHECK_EQUAL(source.operationId, "current");
  BOOST_CHECK(source.state == RepoUserStorageWriteState::CANCELLED);
  BOOST_CHECK_EQUAL(source.reservedBytes, 0U); BOOST_CHECK_EQUAL(source.updatedAtMs, 1004U);
  BOOST_CHECK_EQUAL(source.grant.repoNode, "/repo/a"); BOOST_CHECK_EQUAL(source.grant.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(source.grant.grantId, "grant"); BOOST_CHECK_EQUAL(source.grant.ownerEpoch, 1U);
  BOOST_CHECK_EQUAL(source.object.objectName, pending.objectName);
  BOOST_CHECK_EQUAL(source.object.wireSha256, pending.wireSha256);
  BOOST_CHECK_EQUAL(source.object.generation, pending.generation);
  BOOST_CHECK_EQUAL(source.object.payloadWireBytes, pending.payloadWireBytes);
  BOOST_CHECK_EQUAL(source.object.manifestBytes, pending.manifestBytes);
  BOOST_CHECK(peers.reservedWrites.empty()); BOOST_CHECK(peers.retainedClaims.empty());
  BOOST_CHECK(!peers.hasQuarantinedContent);
  BOOST_CHECK_EQUAL(readonly.userStorageUsage("/user/u", "/repo/a").committedBytes, 30U);
  BOOST_CHECK_EQUAL(readonly.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
  BOOST_CHECK_EQUAL(readonly.userStorageUsage("/user/v", "/repo/a").committedBytes, 0U);
  BOOST_CHECK_EQUAL(readonly.userStorageUsage("/user/u", "/repo/b").committedBytes, 0U);
  BOOST_CHECK_EXCEPTION(readonly.userStorageCleanupPeers("/repo/a", "/user/v", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-missing"));
}

BOOST_AUTO_TEST_CASE(CleanupSourcesRefuseGrantDriftAndExcludeMismatchedMeasurements)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  (void)store.reconcileUserStorageInventory("/repo/a", {}, 1000);
  const auto pending = object("/producer/pending", 30); grant(store, pending); reserve(store, pending);
  (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", pending.objectName, 1, 1002);
  f.sql("UPDATE repo_user_storage_grants SET owner_epoch=2;");
  BOOST_CHECK_EXCEPTION(store.userStorageCleanupPeers("/repo/a", "/user/u", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 30U);
  f.sql("UPDATE repo_user_storage_grants SET owner_epoch=1,object_name='/producer/other';");
  BOOST_CHECK_EXCEPTION(store.userStorageCleanupPeers("/repo/a", "/user/u", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  f.sql("UPDATE repo_user_storage_grants SET object_name='/producer/pending';"
        "UPDATE repo_user_storage_writes SET manifest_bytes=2;");
  BOOST_CHECK(store.userStorageCleanupPeers("/repo/a", "/user/u", "clear").cancelledWrites.empty());
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 30U);
  f.sql("UPDATE repo_user_storage_writes SET manifest_bytes=1;");
  const auto peers = store.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
  BOOST_REQUIRE_EQUAL(peers.cancelledWrites.size(), 1U);
  BOOST_CHECK_EQUAL(peers.cancelledWrites.front().operationId, "write");
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims WHERE state='Deleted'"), 0U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes WHERE state='Cancelled' AND reserved_bytes=0"), 1U);
}

BOOST_AUTO_TEST_CASE(CleanupPeersRetainSharedAliasesAndPendingWritersWithoutChangingUsage)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/b", {}, 1000).ready);
  const auto seed = object("/producer/a", 50);
  auto alias = object("/producer/alias", 60); alias.generation = 7;
  auto unrelated = object("/producer/unrelated", 70); unrelated.wireSha256 = std::string(64, 'b');
  const auto committed = [&](const RepoUserStorageObject& item, const std::string& id,
                             const std::string& owner, const std::string& node = "/repo/a") {
    grant(store, item, id, owner, node); reserve(store, item, id, id, owner, node);
    (void)store.commitUserStorage(node, owner, id, 1002);
  };
  committed(seed, "seed", "/user/u"); committed(seed, "shared", "/user/v");
  committed(alias, "alias", "/user/u"); committed(unrelated, "unrelated", "/user/u");
  committed(seed, "other-node", "/user/u", "/repo/b");
  auto changedName = seed; changedName.wireSha256 = std::string(64, 'c');
  auto stagedAlias = object("/producer/staged", 35); stagedAlias.generation = 9;
  auto ownStage = object("/producer/own-stage", 20);
  for (const auto& item : {changedName, stagedAlias, ownStage}) {
    const auto id = item.objectName;
    const auto owner = id == ownStage.objectName ? "/user/u" : "/user/w";
    grant(store, item, id, owner); reserve(store, item, id, id, owner);
  }
  auto unrelatedStage = object("/producer/unrelated-stage", 10);
  unrelatedStage.wireSha256 = std::string(64, 'b');
  grant(store, unrelatedStage, "unrelated-stage", "/user/w");
  reserve(store, unrelatedStage, "unrelated-stage", "unrelated-stage", "/user/w");
  const auto remoteStage = object("/producer/remote-stage", 10);
  grant(store, remoteStage, "remote-stage", "/user/w", "/repo/b");
  reserve(store, remoteStage, "remote-stage", "remote-stage", "/user/w", "/repo/b");
  const auto cancelled = object("/producer/cancelled", 10);
  grant(store, cancelled, "cancelled"); reserve(store, cancelled, "cancelled", "cancelled");
  (void)store.cancelUserStorage("/repo/a", "/user/u", "cancelled", 1002);
  (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", seed.objectName, 1, 1003);
  (void)store.prepareUserStorageCleanup("/user/v", "/repo/a", "clear-v", seed.objectName, 1, 1003);
  const auto beforeU = store.userStorageUsage("/user/u", "/repo/a");
  const auto beforeW = store.userStorageUsage("/user/w", "/repo/a");
  const auto peers = store.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
  BOOST_CHECK_EQUAL(peers.cleanup.cleanupId, "clear");
  BOOST_CHECK(peers.cleanup.state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_REQUIRE_EQUAL(peers.retainedClaims.size(), 2U);
  BOOST_CHECK_EQUAL(peers.retainedClaims[0].ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(peers.retainedClaims[0].object.objectName, "/producer/alias");
  BOOST_CHECK_EQUAL(peers.retainedClaims[0].object.generation, 7U);
  BOOST_CHECK_EQUAL(peers.retainedClaims[1].ownerIdentity, "/user/v");
  BOOST_CHECK(peers.retainedClaims[1].state == RepoUserStorageClaimState::CLEANUP_PREPARED);
  BOOST_REQUIRE_EQUAL(peers.reservedWrites.size(), 3U);
  BOOST_CHECK_EQUAL(peers.reservedWrites[0].operationId, "/producer/own-stage");
  BOOST_CHECK_EQUAL(peers.reservedWrites[0].grant.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(peers.reservedWrites[1].operationId, "/producer/a");
  BOOST_CHECK_EQUAL(peers.reservedWrites[1].object.wireSha256, std::string(64, 'c'));
  BOOST_CHECK_EQUAL(peers.reservedWrites[2].operationId, "/producer/staged");
  BOOST_CHECK_EQUAL(peers.reservedWrites[2].object.generation, 9U);
  BOOST_CHECK(!peers.hasQuarantinedContent);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, beforeU.committedBytes);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, beforeU.reservedBytes);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/w", "/repo/a").reservedBytes, beforeW.reservedBytes);
  (void)store.cancelUserStorage("/repo/a", "/user/w", "/producer/staged", 1004);
  (void)store.confirmUserStorageCleanup("/repo/a", "/user/v", "clear-v", 1004);
  const auto after = store.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
  BOOST_REQUIRE_EQUAL(after.retainedClaims.size(), 1U);
  BOOST_CHECK_EQUAL(after.retainedClaims[0].object.objectName, "/producer/alias");
  BOOST_CHECK_EQUAL(after.reservedWrites.size(), 2U);
  BOOST_CHECK_EXCEPTION(store.userStorageCleanupPeers("/repo/a", "/user/w", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-missing"));
  BOOST_CHECK_EXCEPTION(store.userStorageCleanupPeers("/repo/b", "/user/u", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-missing"));
}

BOOST_AUTO_TEST_CASE(CleanupPeersRecoverBeforeReadinessAndRefuseUnavailableOrTerminalCleanup)
{
  Fixture f;
  const auto seed = object("/producer/a", 50);
  const auto orphan = object("/producer/orphan", 30);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    grant(store, seed); reserve(store, seed);
    (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
    (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", seed.objectName, 1, 1003);
    BOOST_CHECK(!store.reconcileUserStorageInventory("/repo/a", {{seed}, {orphan}}, 1004).ready);
    BOOST_CHECK(store.userStorageCleanupPeers("/repo/a", "/user/u", "clear").hasQuarantinedContent);
  }
  {
    SqliteArtifactMetadataStore readonly(f.database, false, 16, false);
    BOOST_CHECK(!readonly.artifactWritesEnabled());
    const auto peers = readonly.userStorageCleanupPeers("/repo/a", "/user/u", "clear");
    BOOST_CHECK(peers.retainedClaims.empty()); BOOST_CHECK(peers.reservedWrites.empty());
    BOOST_CHECK(peers.hasQuarantinedContent);
    BOOST_CHECK_EQUAL(readonly.userStorageUsage("/user/u", "/repo/a").committedBytes, 50U);
    BOOST_CHECK_EXCEPTION(readonly.userStorageCleanupPeers("/repo/a", "/user/u", ""),
      std::invalid_argument, hasCode("repo-user-storage-invalid"));
  }
  f.sql("UPDATE repo_user_storage_quarantine SET wire_sha256='" + std::string(64, 'b') + "'");
  SqliteArtifactMetadataStore reopened(f.database);
  BOOST_CHECK(!reopened.userStorageCleanupPeers("/repo/a", "/user/u", "clear").hasQuarantinedContent);
  f.sql("UPDATE repo_user_storage_quarantine SET object_name='/producer/a'");
  BOOST_CHECK(reopened.userStorageCleanupPeers("/repo/a", "/user/u", "clear").hasQuarantinedContent);
  (void)reopened.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1005);
  BOOST_CHECK_EXCEPTION(reopened.userStorageCleanupPeers("/repo/a", "/user/u", "clear"),
    std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  Fixture old;
  { SqliteArtifactMetadataStore initial(old.database); }
  old.downgradeTo15();
  SqliteArtifactMetadataStore legacy(old.database, true, 15);
  BOOST_CHECK_EXCEPTION(legacy.userStorageCleanupPeers("/repo/a", "/user/u", "clear"),
    std::runtime_error, hasCode("repo-user-storage-unavailable"));
}

BOOST_AUTO_TEST_CASE(InventoryMismatchClosesLocalAdmissionAfterRollback)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 60);
  grant(store, a); reserve(store, a);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  BOOST_CHECK_EXCEPTION(store.reconcileUserStorageInventory("/repo/a", {}, 1003),
    std::runtime_error, hasCode("repo-user-storage-inventory-conflict"));
  BOOST_CHECK(!store.userStorageReconciliation("/repo/a").ready);
  BOOST_CHECK_EXCEPTION(reserve(store, a), std::runtime_error, hasCode("repo-user-storage-reconciliation-required"));
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 60U);
  BOOST_CHECK(store.reconcileUserStorageInventory("/repo/a", {{a}}, 1004).ready);
}

BOOST_AUTO_TEST_CASE(UnownedInventoryIsQuarantinedWithoutInventedUser)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  const auto legacy = object("/producer/legacy", 100);
  const auto result = store.reconcileUserStorageInventory("/repo/a", {{legacy}}, 1000);
  BOOST_CHECK_EQUAL(result.quarantinedObjectCount, 1U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims"), 0U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
  BOOST_CHECK(store.listUserStorage("/user/u", "/repo/a", "").empty());
  grant(store, legacy);
  BOOST_CHECK_EXCEPTION(reserve(store, legacy), std::runtime_error,
                        hasCode("repo-user-storage-reconciliation-required"));
}

BOOST_AUTO_TEST_CASE(Schema15MigrationPreservesHistoryAndReadonlyCeiling)
{
  Fixture f;
  {
    SqliteArtifactMetadataStore initial(f.database);
    initial.appendLifecycleEvent({"event", "operation", std::string(64, 'a'), 1,
      ArtifactLifecycleState::Absent, ArtifactLifecycleState::Reserved, 1, true, "keep"});
  }
  f.downgradeTo15();
  {
    SqliteArtifactMetadataStore readonly(f.database, true, 15);
    BOOST_CHECK(!readonly.artifactWritesEnabled());
    BOOST_CHECK_EQUAL(readonly.schemaGeneration(), 15U);
    BOOST_CHECK_EXCEPTION(readonly.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1002),
      std::runtime_error, hasCode("repo-artifact-writes-disabled"));
    BOOST_REQUIRE_EQUAL(readonly.lifecycleEvents("operation").size(), 1U);
    BOOST_CHECK_EXCEPTION(readonly.userStorageUsage("/user/u", "/repo/a"),
      std::runtime_error, hasCode("repo-user-storage-unavailable"));
    BOOST_CHECK_EXCEPTION(readonly.grantUserStorage("/user/u", "/repo/a", "grant",
      "/provider/a", "/producer/a", 10000, 1000),
      std::runtime_error, hasCode("repo-artifact-writes-disabled"));
    BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM sqlite_master WHERE name='repo_user_storage_claims'"), 0U);
  }
  {
    SqliteArtifactMetadataStore upgraded(f.database);
    BOOST_CHECK_EQUAL(upgraded.schemaGeneration(), 16U);
    BOOST_CHECK_EQUAL(upgraded.migrationDiagnostics().previousSchemaGeneration, 15U);
    BOOST_CHECK(!upgraded.migrationDiagnostics().destructiveChanges);
    BOOST_REQUIRE_EQUAL(upgraded.lifecycleEvents("operation").size(), 1U);
    BOOST_CHECK_EQUAL(upgraded.lifecycleEvents("operation")[0].detail, "keep");
  }
  BOOST_CHECK_EQUAL(f.integer("SELECT value FROM repo_meta WHERE key='schema_generation'"), 16U);
}

BOOST_AUTO_TEST_CASE(FutureReadonlyAndPartialCatalogAreFailClosed)
{
  Fixture f;
  { SqliteArtifactMetadataStore initial(f.database); }
  f.sql("UPDATE repo_meta SET value='17' WHERE key='schema_generation'");
  {
    SqliteArtifactMetadataStore future(f.database);
    BOOST_CHECK(!future.artifactWritesEnabled());
    BOOST_CHECK_EXCEPTION(future.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1002),
      std::runtime_error, hasCode("repo-artifact-writes-disabled"));
    BOOST_CHECK_EXCEPTION(future.grantUserStorage("/user/u", "/repo/a", "grant", "/provider/a",
      "/producer/a", 10000, 1000), std::runtime_error, hasCode("repo-artifact-writes-disabled"));
  }
  f.sql("UPDATE repo_meta SET value='16' WHERE key='schema_generation'; DROP TABLE repo_user_storage_quarantine");
  BOOST_CHECK_EXCEPTION(SqliteArtifactMetadataStore broken(f.database),
    std::runtime_error, hasCode("repo-schema-unsupported-profile"));
}

BOOST_AUTO_TEST_CASE(RejectsOverflowAndNoncanonicalInputs)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  auto a = object("/producer/a", 10);
  grant(store, a);
  a.payloadWireBytes = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  BOOST_CHECK_EXCEPTION(reserve(store, a), std::invalid_argument, hasCode("repo-user-storage-invalid"));
  BOOST_CHECK_EXCEPTION(store.userStorageUsage("/user/%75", "/repo/a"),
    std::invalid_argument, hasCode("repo-user-storage-invalid"));
  BOOST_CHECK_EXCEPTION(store.listUserStorage("/user/u", "/repo/a", "", 0),
    std::invalid_argument, hasCode("repo-user-storage-invalid"));
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 0U);
}

BOOST_AUTO_TEST_CASE(MissingCleanupFenceIndexIsRejectedOnReopen)
{
  Fixture f;
  { SqliteArtifactMetadataStore initial(f.database); }
  f.sql("DROP INDEX repo_user_storage_live_claim");
  BOOST_CHECK_EXCEPTION(SqliteArtifactMetadataStore broken(f.database),
    std::runtime_error, hasCode("repo-schema-unsupported-profile"));
}

BOOST_AUTO_TEST_CASE(WrongFenceKeysAndCollationAreRejectedOnReopen)
{
  for (const auto& replacement : {
    "CREATE UNIQUE INDEX repo_user_storage_cleanup_id ON repo_user_storage_claims(repo_node,cleanup_id) WHERE cleanup_id<>''",
    "CREATE UNIQUE INDEX repo_user_storage_cleanup_id ON repo_user_storage_claims(repo_node,owner_identity,cleanup_id COLLATE NOCASE) WHERE cleanup_id<>''",
    "CREATE UNIQUE INDEX repo_user_storage_cleanup_id ON repo_user_storage_claims(repo_node,owner_identity,cleanup_id) WHERE cleanup_id<>'' OR state='Deleted'"}) {
    Fixture f;
    { SqliteArtifactMetadataStore initial(f.database); }
    f.sql("DROP INDEX repo_user_storage_cleanup_id");
    f.sql(replacement);
    BOOST_CHECK_EXCEPTION(SqliteArtifactMetadataStore broken(f.database),
      std::runtime_error, hasCode("repo-schema-unsupported-profile"));
  }
}

BOOST_AUTO_TEST_CASE(DirectOwnerWritesAndLiveEpochDelegationPreserveBinding)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 100);
  grant(store, a, "direct", "/user/u", "/repo/a", "/user/u");
  (void)store.reserveUserStorage("/user/u", "/repo/a", "/user/u", "direct", "direct-write", a, 1001);
  (void)store.commitUserStorage("/repo/a", "/user/u", "direct-write", 1002);
  BOOST_CHECK_EXCEPTION(store.grantUserStorage("/user/u", "/repo/a", "direct",
    "/provider/a", a.objectName, 10000, 1003),
    std::runtime_error, hasCode("repo-user-storage-grant-conflict"));
  const auto delegated = store.grantUserStorage("/user/u", "/repo/a", "delegated",
    "/provider/a", a.objectName, 11000, 1003);
  BOOST_CHECK_EQUAL(delegated.ownerEpoch, 1U);
  (void)store.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "delegated",
    "delegate-write", a, 1004);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims"), 1U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 100U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(SameConnectionConcurrentAdmissionIsAtomic)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  fillCommittedBudget(store, 99000000000ULL);
  const auto a = object("/producer/a", 600000000);
  const auto b = object("/producer/b", 600000000);
  grant(store, a, "a"); grant(store, b, "b");
  std::promise<void> start;
  const auto ready = start.get_future().share();
  const auto submit = [&](const RepoUserStorageObject& o, const std::string& id) {
    ready.wait();
    try { reserve(store, o, id, id); return std::string("admitted"); }
    catch (const std::exception& e) { return std::string(e.what()); }
  };
  auto x = std::async(std::launch::async, [&] { return submit(a, "a"); });
  auto y = std::async(std::launch::async, [&] { return submit(b, "b"); });
  start.set_value();
  const auto rx = x.get(); const auto ry = y.get();
  BOOST_CHECK((rx == "admitted" && ry.find("QUOTA_EXCEEDED") == 0) ||
              (ry == "admitted" && rx.find("QUOTA_EXCEEDED") == 0));
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes WHERE state='Reserved'"), 1U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 600000000U);
}

BOOST_AUTO_TEST_CASE(PendingCleanupReopensChargedAndRejectsConflictingIdentity)
{
  Fixture f;
  const auto a = object("/producer/a", 90);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    grant(store, a); reserve(store, a);
    (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
    (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "clear", a.objectName, 1, 1003);
  }
  SqliteArtifactMetadataStore reopened(f.database);
  BOOST_REQUIRE_EQUAL(reopened.pendingUserStorageCleanups("/repo/a").size(), 1U);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").committedBytes, 90U);
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "clear",
    "/producer/b", 1, 1004), std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK_EXCEPTION(reopened.prepareUserStorageCleanup("/user/u", "/repo/a", "clear",
    a.objectName, 2, 1004), std::runtime_error, hasCode("repo-user-storage-cleanup-conflict"));
  BOOST_CHECK(!reopened.reconcileUserStorageInventory("/repo/a", {}, 1004).ready);
  (void)reopened.confirmUserStorageCleanup("/repo/a", "/user/u", "clear", 1005);
  BOOST_CHECK(reopened.reconcileUserStorageInventory("/repo/a", {}, 1006).ready);
  BOOST_CHECK_EQUAL(reopened.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
  BOOST_CHECK(reopened.pendingUserStorageCleanups("/repo/a").empty());
  const auto stale = reopened.reconcileUserStorageInventory("/repo/a", {{a}}, 1007);
  BOOST_CHECK(!stale.ready);
  BOOST_CHECK_EQUAL(stale.quarantinedObjectCount, 1U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_claims WHERE state='Deleted'"), 1U);
}

BOOST_AUTO_TEST_CASE(InventoryContentAndGenerationMismatchCannotReopenAdmission)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 60);
  grant(store, a); reserve(store, a);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  for (unsigned variant = 0; variant != 2; ++variant) {
    auto wrong = a;
    if (variant == 0) wrong.wireSha256 = std::string(64, 'b');
    else ++wrong.generation;
    BOOST_CHECK_EXCEPTION(store.reconcileUserStorageInventory("/repo/a", {{wrong}}, 1003),
      std::runtime_error, hasCode("repo-user-storage-inventory-conflict"));
    BOOST_CHECK(!store.userStorageReconciliation("/repo/a").ready);
    BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 60U);
    BOOST_CHECK(store.reconcileUserStorageInventory("/repo/a", {{a}}, 1004).ready);
  }
}

BOOST_AUTO_TEST_CASE(MalformedUserStorageTableConstraintsAreRejected)
{
  struct Mutation { const char* table; const char* original; const char* replacement; };
  for (const auto& mutation : {
    Mutation{"repo_user_storage_grants", "CHECK(owner_epoch>0)", ""},
    Mutation{"repo_user_storage_grants", "CHECK(owner_epoch>0)", "/* CHECK(owner_epoch>0) */"},
    Mutation{"repo_user_storage_claims", "CHECK(payload_wire_bytes>=0)", ""},
    Mutation{"repo_user_storage_writes", ",FOREIGN KEY(repo_node,owner_identity,grant_id) REFERENCES repo_user_storage_grants(repo_node,owner_identity,grant_id)", ""},
    Mutation{"repo_user_storage_state", "ready INTEGER NOT NULL", "ready TEXT NOT NULL"},
    Mutation{"repo_user_storage_quarantine", "manifest_bytes INTEGER NOT NULL", "manifest_bytes INTEGER"}}) {
    Fixture f;
    { SqliteArtifactMetadataStore initial(f.database); }
    // Mutate only this disposable database's declaration, then reopen through
    // SQLite so profile inspection observes the actual changed schema.
    f.sql(std::string("PRAGMA writable_schema=ON; UPDATE sqlite_master SET sql=replace(sql,'") +
      mutation.original + "','" + mutation.replacement + "') WHERE name='" + mutation.table +
      "'; PRAGMA writable_schema=OFF;");
    BOOST_CHECK_EXCEPTION(SqliteArtifactMetadataStore broken(f.database),
      std::runtime_error, hasCode("repo-schema-unsupported-profile"));
  }
}

BOOST_AUTO_TEST_CASE(ManifestSettlementChangesOnlyMeasuredCharge)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 100);
  grant(store, a); reserve(store, a);
  auto result = store.settleUserStorageReservation("/repo/a", "/user/u", "write", 21, 1002);
  BOOST_CHECK_EQUAL(result.reservedBytes, 120U);
  BOOST_CHECK_EQUAL(f.integer("SELECT payload_wire_bytes FROM repo_user_storage_writes"), 99U);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes WHERE object_name='/producer/a' AND generation=1 AND grant_id='grant'"), 1U);
  BOOST_CHECK_EQUAL(result.object.wireSha256, a.wireSha256);
  result = store.settleUserStorageReservation("/repo/a", "/user/u", "write", 1, 1003);
  BOOST_CHECK_EQUAL(result.reservedBytes, 100U);
  result = store.settleUserStorageReservation("/repo/a", "/user/u", "write", 1, 1004);
  BOOST_CHECK_EQUAL(result.updatedAtMs, 1003U);
  BOOST_CHECK_EQUAL(f.integer("SELECT SUM(reserved_bytes) FROM repo_user_storage_writes"), 100U);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1005);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 100U);
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 1, 1006),
    std::runtime_error, hasCode("repo-user-storage-operation-conflict"));
}

BOOST_AUTO_TEST_CASE(ManifestSettlementObjectBoundAndRejectedGrowthAreAtomic)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  auto a = object("/producer/a", 1000000000ULL);
  a.manifestBytes = 0;
  grant(store, a); reserve(store, a);
  BOOST_CHECK_EQUAL(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 1, 1002).reservedBytes, 1000000000ULL);
  BOOST_CHECK_THROW(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1003),
                    std::invalid_argument);
  BOOST_CHECK_THROW(store.settleUserStorageReservation("/repo/a", "/user/u", "write", std::numeric_limits<uint64_t>::max(), 1003), std::invalid_argument);
  BOOST_CHECK_THROW(store.settleUserStorageReservation("/repo/a", "/user/u", "write", std::numeric_limits<int64_t>::max(), 1003), std::invalid_argument);
  BOOST_CHECK_EQUAL(f.integer("SELECT manifest_bytes FROM repo_user_storage_writes"), 1U);
  BOOST_CHECK_EQUAL(f.integer("SELECT reserved_bytes FROM repo_user_storage_writes"), 1000000000ULL);
  BOOST_CHECK_EQUAL(f.integer("SELECT updated_at_ms FROM repo_user_storage_writes"), 1002U);
}

BOOST_AUTO_TEST_CASE(ManifestSettlementRejectsMissingBackwardsAndCancelledOperations)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 100);
  grant(store, a); reserve(store, a);
  (void)store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1003);
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "missing", 2, 1004),
    std::runtime_error, hasCode("repo-user-storage-operation-missing"));
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 3, 1002),
    std::runtime_error, hasCode("repo-user-storage-operation-conflict"));
  BOOST_CHECK_EQUAL(f.integer("SELECT manifest_bytes FROM repo_user_storage_writes"), 2U);
  (void)store.cancelUserStorage("/repo/a", "/user/u", "write", 1004);
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1005),
    std::runtime_error, hasCode("repo-user-storage-operation-conflict"));
}

BOOST_AUTO_TEST_CASE(ManifestSettlementDetectsCorruptReservationWithoutRepairingIt)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 100);
  grant(store, a); reserve(store, a);
  // Model externally corrupted recovery data in this disposable database only.
  f.sql("PRAGMA ignore_check_constraints=ON; UPDATE repo_user_storage_writes SET reserved_bytes=99; PRAGMA ignore_check_constraints=OFF;");
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1002),
    std::runtime_error, hasCode("repo-user-storage-inventory-conflict"));
  BOOST_CHECK_EQUAL(f.integer("SELECT reserved_bytes FROM repo_user_storage_writes"), 99U);
  BOOST_CHECK_EQUAL(f.integer("SELECT manifest_bytes FROM repo_user_storage_writes"), 1U);
}

BOOST_AUTO_TEST_CASE(ManifestSettlementCannotChangeExistingOwnerClaim)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  const auto a = object("/producer/a", 100);
  grant(store, a); reserve(store, a);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  // Inject a contradictory recovery journal without changing the live claim.
  f.sql("UPDATE repo_user_storage_writes SET state='Reserved',reserved_bytes=100");
  BOOST_CHECK_EXCEPTION(store.settleUserStorageReservation("/repo/a", "/user/u", "write", 2, 1003),
    std::runtime_error, hasCode("repo-user-storage-object-conflict"));
  BOOST_CHECK_EQUAL(f.integer("SELECT manifest_bytes FROM repo_user_storage_claims"), 1U);
  BOOST_CHECK_EQUAL(f.integer("SELECT reserved_bytes FROM repo_user_storage_writes"), 100U);
}

BOOST_AUTO_TEST_CASE(ConcurrentManifestGrowthUsesOneAccountTransaction)
{
  Fixture f;
  SqliteArtifactMetadataStore first(f.database), second(f.database);
  BOOST_REQUIRE(first.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  BOOST_REQUIRE(second.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  fillCommittedBudget(first, 99000000000ULL);
  const auto a = object("/producer/a", 400000000ULL);
  const auto b = object("/producer/b", 400000000ULL);
  grant(first, a, "a"); grant(second, b, "b");
  reserve(first, a, "a", "a"); reserve(second, b, "b", "b");
  std::promise<void> start;
  auto ready = start.get_future().share();
  auto grow = [&](SqliteArtifactMetadataStore& store, const char* op) {
    ready.wait();
    try { (void)store.settleUserStorageReservation("/repo/a", "/user/u", op, 200000001, 1002); return true; }
    catch (const std::runtime_error& e) {
      if (!hasCode("QUOTA_EXCEEDED")(e)) throw;
      return false;
    }
  };
  auto one = std::async(std::launch::async, grow, std::ref(first), "a");
  auto two = std::async(std::launch::async, grow, std::ref(second), "b");
  start.set_value();
  const bool wonOne = one.get(), wonTwo = two.get();
  BOOST_CHECK(wonOne != wonTwo);
  BOOST_CHECK_EQUAL(f.integer("SELECT SUM(reserved_bytes) FROM repo_user_storage_writes"), 1000000000ULL);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes WHERE manifest_bytes=200000001"), 1U);
}

BOOST_AUTO_TEST_CASE(RestartRecoverySettlesExistingExpiredGrantBeforeAdmissionReopens)
{
  Fixture f;
  auto a = object("/producer/a", 75);
  {
    SqliteArtifactMetadataStore store(f.database);
    BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
    grant(store, a); reserve(store, a);
  }
  SqliteArtifactMetadataStore recovered(f.database);
  BOOST_CHECK(!recovered.userStorageReconciliation("/repo/a").ready);
  BOOST_CHECK_EQUAL(recovered.settleUserStorageReservation("/repo/a", "/user/u", "write", 6, 10001).reservedBytes, 80U);
  BOOST_CHECK(!recovered.userStorageReconciliation("/repo/a").ready);
  a.manifestBytes = 6;
  BOOST_CHECK_EXCEPTION(recovered.commitUserStorage("/repo/a", "/user/u", "write", 10002),
    std::runtime_error, hasCode("repo-user-storage-grant-expired"));
  (void)recovered.commitRecoveredUserStorage("/repo/a", "/user/u", "write", 10002);
  BOOST_CHECK(recovered.reconcileUserStorageInventory("/repo/a", {{a}}, 10003).ready);
  BOOST_CHECK_EQUAL(recovered.userStorageUsage("/user/u", "/repo/a").committedBytes, 80U);
  BOOST_CHECK_EXCEPTION(recovered.reserveUserStorage("/provider/a", "/repo/a", "/user/u",
    "grant", "write", a, 10004), std::runtime_error,
    hasCode("repo-user-storage-grant-expired"));
  BOOST_CHECK_EXCEPTION(recovered.reserveUserStorage("/provider/a", "/repo/a", "/user/u", "grant", "fresh", a, 10004),
    std::runtime_error, hasCode("repo-user-storage-grant-expired"));
}

BOOST_AUTO_TEST_CASE(ExactGrantValidationDoesNotReserveOrRequireReady)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database, true, 16, true);
  const auto grant = store.grantUserStorage("/user/u", "/repo/a", "grant", "/provider/a", "/object/a", 2000, 1000);
  const auto check = [&](const std::string& writer, const std::string& node,
                         const std::string& owner, const std::string& id,
                         const std::string& objectName, uint64_t epoch, uint64_t now) {
    return store.validateUserStorageGrant(writer, node, owner, id, objectName, epoch, now);
  };
  BOOST_CHECK_EQUAL(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 1, 1000).grantId, grant.grantId);
  BOOST_CHECK(!store.userStorageReconciliation("/repo/a").ready);
  BOOST_CHECK_EQUAL(f.integer("SELECT COUNT(*) FROM repo_user_storage_writes"), 0U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").reservedBytes, 0U);
  BOOST_CHECK_EXCEPTION(check("/provider/b", "/repo/a", "/user/u", "grant", "/object/a", 1, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/b", "/user/u", "grant", "/object/a", 1, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/v", "grant", "/object/a", 1, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/u", "absent", "/object/a", 1, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/b", 1, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 2, 1001), std::runtime_error, hasCode("repo-user-storage-unauthorized"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 1, 999), std::runtime_error, hasCode("repo-user-storage-grant-expired"));
  BOOST_CHECK_EXCEPTION(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 1, 2000), std::runtime_error, hasCode("repo-user-storage-grant-expired"));
  BOOST_CHECK_THROW(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 0, 1001), std::invalid_argument);
  BOOST_CHECK_THROW(check("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 1, uint64_t{1} << 63), std::invalid_argument);
  // A live read-only connection must see WAL; immutable snapshots omit it.
  SqliteArtifactMetadataStore restarted(f.database, false, 16, false);
  BOOST_CHECK_EQUAL(restarted.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", "/object/a", 1, 1999).ownerEpoch, 1U);
}

BOOST_AUTO_TEST_CASE(ExactGrantValidationHonorsCleanupAndNewEpoch)
{
  Fixture f;
  SqliteArtifactMetadataStore store(f.database, true, 16, true);
  const auto o = object("/object/a", 100);
  grant(store, o);
  BOOST_REQUIRE(store.reconcileUserStorageInventory("/repo/a", {}, 1000).ready);
  reserve(store, o);
  (void)store.commitUserStorage("/repo/a", "/user/u", "write", 1002);
  (void)store.prepareUserStorageCleanup("/user/u", "/repo/a", "cleanup", o.objectName, 1, 1003);
  BOOST_CHECK_EXCEPTION(store.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", o.objectName, 1, 1004), std::runtime_error, hasCode("repo-user-storage-deleted"));
  (void)store.confirmUserStorageCleanup("/repo/a", "/user/u", "cleanup", 1005);
  BOOST_CHECK_EXCEPTION(store.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "grant", o.objectName, 1, 1006), std::runtime_error, hasCode("repo-user-storage-deleted"));
  const auto renewed = store.grantUserStorage("/user/u", "/repo/a", "new", "/provider/a", o.objectName, 2000, 1007);
  BOOST_CHECK_EQUAL(renewed.ownerEpoch, 2U);
  BOOST_CHECK_EQUAL(store.validateUserStorageGrant("/provider/a", "/repo/a", "/user/u", "new", o.objectName, 2, 1008).ownerEpoch, 2U);
  BOOST_CHECK_EQUAL(store.userStorageUsage("/user/u", "/repo/a").committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedListReplyBindsEmptyPagesAndRejectsMalformedClaims)
{
  RepoUserStorageClaim claim{"/repo/a", "/user/u", "grant", 1,
    {"/object/a", std::string(64, 'a'), 1, 100, 20},
    RepoUserStorageClaimState::COMMITTED, {}, 1000, 0};
  RepoUserStorageListReply reply{1, "/repo/a", "/user/u", {claim}, "/object/a"};
  const RepoArtifactTarget target{"/repo/a", "/provider/a", "/service/repo"};
  const RepoUserStorageListRequest request{1, {}, 1};
  const auto wire = reply.toJson();
  const auto decoded = RepoUserStorageListReply::fromJson(wire);
  BOOST_CHECK_EQUAL(decoded.toJson(), wire);
  BOOST_CHECK_NO_THROW(decoded.validateExpected(target, "/user/u", request));
  BOOST_CHECK_EQUAL(decoded.objects.front().object.payloadWireBytes +
    decoded.objects.front().object.manifestBytes, 120U);
  BOOST_CHECK_THROW(decoded.validateExpected(target, "/user/v", request), std::invalid_argument);
  auto wrongNode = target; wrongNode.repoNode = "/repo/b";
  BOOST_CHECK_THROW(decoded.validateExpected(wrongNode, "/user/u", request), std::invalid_argument);
  BOOST_CHECK_THROW(decoded.validateExpected(target, "/user/u", {1, "/object/a", 1}), std::invalid_argument);
  auto invalid = reply; invalid.objects.push_back(claim);
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid = reply; invalid.objects.front().ownerIdentity = "/user/v";
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid = reply; invalid.objects.front().state = RepoUserStorageClaimState::DELETED;
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid = reply; invalid.objects.front().object.payloadWireBytes = 1000000000ULL;
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid = reply; invalid.nextAfterObjectName = "/object/b";
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  auto duplicate = wire; duplicate.insert(1, "\"repoNode\":\"/repo/a\",");
  BOOST_CHECK_THROW(RepoUserStorageListReply::fromJson(duplicate), std::invalid_argument);
  auto unknown = wire; unknown.insert(1, "\"overrideOwner\":\"/user/v\",");
  BOOST_CHECK_THROW(RepoUserStorageListReply::fromJson(unknown), std::invalid_argument);
  reply.objects.front().state = RepoUserStorageClaimState::CLEANUP_PREPARED;
  reply.objects.front().cleanupId = "cleanup";
  BOOST_CHECK_EQUAL(RepoUserStorageListReply::fromJson(reply.toJson()).objects.front().cleanupId, "cleanup");
  const RepoUserStorageListReply empty{1, "/repo/a", "/user/u", {}, {}};
  const auto emptyDecoded = RepoUserStorageListReply::fromJson(empty.toJson());
  BOOST_CHECK_NO_THROW(emptyDecoded.validateExpected(target, "/user/u", request));
  BOOST_CHECK_THROW(emptyDecoded.validateExpected(target, "/user/v", request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnedListReplyBoundsEncodedBytesAndEntryCount)
{
  RepoUserStorageListReply reply{1, "/repo/a", "/user/u", {}, {}};
  for (size_t i = 0; i < 1000; ++i) {
    reply.objects.push_back({"/repo/a", "/user/u", "grant", 1,
      {"/object/" + std::to_string(10000 + i) + "/" + std::string(2000, 'a'),
        std::string(64, 'a'), 1, 1, 1}, RepoUserStorageClaimState::COMMITTED, {}, 1000, 0});
  }
  BOOST_CHECK_THROW(reply.toJson(), std::length_error);
  reply.objects.push_back(reply.objects.back());
  BOOST_CHECK_THROW(reply.toJson(), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageListReply::fromJson(std::string(1024 * 1024 + 1, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnedDeleteReplyBindsOutcomeToExactOwnerNodeAndRequest)
{
  const std::string wire =
    "{\"schemaVersion\":1,\"repoNode\":\"/repo/a\",\"ownerIdentity\":\"/user/u\","
    "\"cleanupId\":\"cleanup\",\"objectName\":\"/producer/kv\",\"ownerEpoch\":7,"
    "\"state\":\"PENDING\",\"releasedBytes\":0}";
  const RepoArtifactTarget target{"/repo/a", "/provider/a", "/service/repo"};
  const RepoUserStorageDeleteRequest request{1, "cleanup", "/producer/kv", 7};
  const auto pending = RepoUserStorageDeleteReply::fromJson(wire);
  BOOST_CHECK_EQUAL(pending.toJson(), wire);
  BOOST_CHECK_EQUAL(pending.state, "PENDING");
  BOOST_CHECK_EQUAL(pending.releasedBytes, 0U);
  BOOST_CHECK_NO_THROW(pending.validateExpected(target, "/user/u", request));
  BOOST_CHECK_THROW(pending.validateExpected(target, "/user/v", request), std::invalid_argument);
  auto otherNode = target; otherNode.repoNode = "/repo/b";
  BOOST_CHECK_THROW(pending.validateExpected(otherNode, "/user/u", request), std::invalid_argument);
  BOOST_CHECK_THROW(pending.validateExpected(target, "/user/u", {1, "other", "/producer/kv", 7}), std::invalid_argument);
  BOOST_CHECK_THROW(pending.validateExpected(target, "/user/u", {1, "cleanup", "/producer/other", 7}), std::invalid_argument);
  BOOST_CHECK_THROW(pending.validateExpected(target, "/user/u", {1, "cleanup", "/producer/kv", 8}), std::invalid_argument);
  auto deleted = pending; deleted.state = "DELETED";
  deleted.releasedBytes = 1000000000ULL;
  const auto decoded = RepoUserStorageDeleteReply::fromJson(deleted.toJson());
  BOOST_CHECK_EQUAL(decoded.releasedBytes, 1000000000ULL);
  BOOST_CHECK_NO_THROW(decoded.validateExpected(target, "/user/u", request));
  deleted.releasedBytes = 0;
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(deleted.toJson()).releasedBytes, 0U);
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteRequest::fromJson(request.toJson()).toJson(), request.toJson());
}

BOOST_AUTO_TEST_CASE(OwnedDeleteReplyRejectsMalformedAndImpossibleOutcomes)
{
  const std::string wire =
    "{\"schemaVersion\":1,\"repoNode\":\"/repo/a\",\"ownerIdentity\":\"/user/u\","
    "\"cleanupId\":\"cleanup\",\"objectName\":\"/producer/kv\",\"ownerEpoch\":7,"
    "\"state\":\"DELETED\",\"releasedBytes\":1}";
  auto changed = [&](const std::string& before, const std::string& after) {
    auto result = wire;
    const auto position = result.find(before);
    BOOST_REQUIRE(position != std::string::npos);
    result.replace(position, before.size(), after);
    return result;
  };
  for (const auto& invalid : {
      changed("\"schemaVersion\":1", "\"schemaVersion\":2"),
      changed("\"schemaVersion\":1", "\"schemaVersion\":\"1\""),
      changed("\"repoNode\":\"/repo/a\"", "\"repoNode\":\"\""),
      changed("\"ownerIdentity\":\"/user/u\"", "\"ownerIdentity\":\"user/u\""),
      changed("\"ownerIdentity\":\"/user/u\",", ""),
      changed("\"cleanupId\":\"cleanup\"", "\"cleanupId\":\"\""),
      changed("\"cleanupId\":\"cleanup\"", "\"cleanupId\":\"" + std::string(257, 'a') + "\""),
      changed("\"objectName\":\"/producer/kv\"", "\"objectName\":false"),
      changed("\"ownerEpoch\":7", "\"ownerEpoch\":0"),
      changed("\"ownerEpoch\":7", "\"ownerEpoch\":-1"),
      changed("\"ownerEpoch\":7", "\"ownerEpoch\":9223372036854775808"),
      changed("\"state\":\"DELETED\"", "\"state\":\"UNKNOWN\""),
      changed("\"state\":\"DELETED\"", "\"state\":\"PENDING\""),
      changed("\"releasedBytes\":1", "\"releasedBytes\":1000000001"),
      changed("\"releasedBytes\":1", "\"releasedBytes\":18446744073709551616"),
      changed("\"releasedBytes\":1", "\"releasedBytes\":-1"),
      changed("\"releasedBytes\":1", "\"releasedBytes\":1.0"),
      changed("\"releasedBytes\":1", "\"releasedBytes\":\"1\""),
      changed("\"releasedBytes\":1", "\"releasedBytes\":1,\"releasedBytes\":0"),
      changed("\"releasedBytes\":1", "\"releasedBytes\":1,\"success\":true")}) {
    BOOST_CHECK_THROW(RepoUserStorageDeleteReply::fromJson(invalid), std::invalid_argument);
  }
  BOOST_CHECK_THROW(RepoUserStorageDeleteReply::fromJson(std::string(64 * 1024 + 1, ' ')), std::invalid_argument);
  auto invalid = RepoUserStorageDeleteReply::fromJson(wire);
  invalid.state = "PENDING";
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid.state = "DELETED"; invalid.releasedBytes = 1000000001ULL;
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
