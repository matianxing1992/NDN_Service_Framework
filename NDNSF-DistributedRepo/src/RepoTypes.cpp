#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

bool
isAmbiguousCommitError(const std::exception& error)
{
  constexpr char prefix[] = "repo-file-ambiguous-commit:";
  const std::string message = error.what();
  return message.compare(0, sizeof(prefix) - 1, prefix) == 0;
}

std::string
jsonQuote(const std::string& value)
{
  std::string encoded;
  encoded.reserve(value.size() + 2);
  encoded.push_back('"');
  for (char ch : value) {
    switch (ch) {
    case '\\':
      encoded.append("\\\\");
      break;
    case '"':
      encoded.append("\\\"");
      break;
    case '\n':
      encoded.append("\\n");
      break;
    case '\r':
      encoded.append("\\r");
      break;
    case '\t':
      encoded.append("\\t");
      break;
    default:
      encoded.push_back(ch);
      break;
    }
  }
  encoded.push_back('"');
  return encoded;
}

double
scoreCandidate(const StorageCapability& candidate)
{
  double score = 0.0;
  score += static_cast<double>(candidate.freeBytes) / (1024.0 * 1024.0);
  score += 1000.0 * candidate.availabilityScore;
  score -= 1000.0 * candidate.recentLoad;
  return score;
}

std::string
normalizeModeText(const std::string& value)
{
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == '_') {
      continue;
    }
    normalized.push_back(static_cast<char>(
      std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

uint64_t
jsonUnsigned(const detail::JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, detail::JsonValue::Kind::Unsigned).number;
}

std::string
jsonString(const detail::JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, detail::JsonValue::Kind::String).string;
}

std::string
operationOwnerJson(const std::optional<RepoArtifactTarget>& operationOwner)
{
  if (!operationOwner) return "null";
  return "{\"repoNode\":" + jsonQuote(operationOwner->repoNode) +
    ",\"providerIdentity\":" + jsonQuote(operationOwner->providerIdentity) +
    ",\"servicePrefix\":" + jsonQuote(operationOwner->servicePrefix) + "}";
}

void
validateOperationOwner(const std::optional<RepoArtifactTarget>& operationOwner,
                       const ArtifactLimits& limits)
{
  if (!operationOwner) return;
  validateName(operationOwner->repoNode, limits, "operationOwner.repoNode");
  validateName(operationOwner->providerIdentity, limits,
                "operationOwner.providerIdentity");
  validateName(operationOwner->servicePrefix, limits,
                "operationOwner.servicePrefix");
}

std::optional<RepoArtifactTarget>
parseOperationOwner(const detail::JsonValue& value)
{
  if (value.kind == detail::JsonValue::Kind::Null) return std::nullopt;
  const auto& object = detail::requireObject(value);
  detail::requireExactKeys(object, {"repoNode", "providerIdentity", "servicePrefix"});
  RepoArtifactTarget target{
    jsonString(object, "repoNode"),
    jsonString(object, "providerIdentity"),
    jsonString(object, "servicePrefix")};
  validateOperationOwner(target, ArtifactLimits{});
  return target;
}

bool
hasArtifactFrameMagic(const std::vector<uint8_t>& wire, const char* magic)
{
  return wire.size() >= 8 &&
    std::equal(wire.begin(), wire.begin() + 8,
               reinterpret_cast<const uint8_t*>(magic));
}

std::vector<uint8_t>
frameArtifactRequest(const char* magic, const std::string& header,
                     const std::vector<uint8_t>& payload = {})
{
  if (header.size() > 64 * 1024 || payload.size() > 16 * 1024 * 1024 ||
      header.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("repo-artifact-request-too-large: request exceeds wire bounds");
  }
  std::vector<uint8_t> result;
  result.reserve(12 + header.size() + payload.size());
  for (size_t i = 0; i < 8; ++i) result.push_back(static_cast<uint8_t>(magic[i]));
  const auto length = static_cast<uint32_t>(header.size());
  result.push_back(static_cast<uint8_t>((length >> 24) & 0xff));
  result.push_back(static_cast<uint8_t>((length >> 16) & 0xff));
  result.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
  result.push_back(static_cast<uint8_t>(length & 0xff));
  result.insert(result.end(), header.begin(), header.end());
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::string
unframeArtifactRequest(const std::vector<uint8_t>& wire, const char* magic,
                       size_t& payloadOffset)
{
  if (wire.size() < 12 || wire.size() > 16 * 1024 * 1024 + 64 * 1024 + 12 ||
      !std::equal(wire.begin(), wire.begin() + 8,
                  reinterpret_cast<const uint8_t*>(magic))) {
    throw std::invalid_argument("repo-artifact-request-invalid: bad frame signature or length");
  }
  const uint32_t headerLength =
    (static_cast<uint32_t>(wire[8]) << 24) |
    (static_cast<uint32_t>(wire[9]) << 16) |
    (static_cast<uint32_t>(wire[10]) << 8) | static_cast<uint32_t>(wire[11]);
  if (headerLength == 0 || headerLength > 64 * 1024 ||
      wire.size() - 12 < headerLength) {
    throw std::invalid_argument("repo-artifact-request-invalid: bad header length");
  }
  payloadOffset = 12 + headerLength;
  return std::string(wire.begin() + 12, wire.begin() + payloadOffset);
}

} // namespace

std::string
toString(RepoWriteConsistency consistency)
{
  switch (consistency) {
  case RepoWriteConsistency::One:
    return "ONE";
  case RepoWriteConsistency::Quorum:
    return "QUORUM";
  case RepoWriteConsistency::All:
    return "ALL";
  }
  throw std::invalid_argument("unsupported repo write consistency");
}

RepoWriteConsistency
parseRepoWriteConsistency(const std::string& value)
{
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    normalized.push_back(static_cast<char>(
      std::toupper(static_cast<unsigned char>(ch))));
  }
  if (normalized == "ONE") {
    return RepoWriteConsistency::One;
  }
  if (normalized == "QUORUM") {
    return RepoWriteConsistency::Quorum;
  }
  if (normalized == "ALL") {
    return RepoWriteConsistency::All;
  }
  throw std::invalid_argument("unsupported repo write consistency: " + value);
}

uint32_t
requiredWriteAcks(uint32_t replicationFactor, RepoWriteConsistency consistency)
{
  if (replicationFactor == 0) {
    throw std::invalid_argument("repo replication factor must be >= 1");
  }
  switch (consistency) {
  case RepoWriteConsistency::One:
    return 1;
  case RepoWriteConsistency::Quorum:
    return replicationFactor / 2 + 1;
  case RepoWriteConsistency::All:
    return replicationFactor;
  }
  throw std::invalid_argument("unsupported repo write consistency");
}

std::string
normalizeRepoOperationState(const std::string& value)
{
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    normalized.push_back(static_cast<char>(
      std::toupper(static_cast<unsigned char>(ch))));
  }
  static const std::set<std::string> states = {
    "RECEIVED", "RUNNING", "COMMITTED", "INCOMPLETE", "FAILED",
    "CANCELLED", "EXPIRED",
  };
  if (states.count(normalized) == 0) {
    throw std::invalid_argument("unsupported repo operation state: " + value);
  }
  return normalized;
}

class SqliteRepoStore : public RepoStoreBackend
{
public:
  explicit SqliteRepoStore(std::string databasePath)
    : m_databasePath(std::move(databasePath))
    , m_ownership(m_databasePath, "cpp-sqlite-repo")
  {
    if (m_databasePath.empty()) {
      throw std::invalid_argument("sqlite repo database path must not be empty");
    }
    if (sqlite3_open(m_databasePath.c_str(), &m_db) != SQLITE_OK) {
      const std::string error = m_db != nullptr ? sqlite3_errmsg(m_db) : "unknown";
      throw std::runtime_error("failed to open sqlite repo store: " + error);
    }
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("CREATE TABLE IF NOT EXISTS objects ("
         "object_name TEXT PRIMARY KEY,"
         "manifest_json TEXT NOT NULL,"
         "payload BLOB NOT NULL,"
         "payload_size INTEGER NOT NULL,"
         "sha256 TEXT NOT NULL,"
         "object_type TEXT NOT NULL,"
         "updated_at INTEGER NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS data_receipt_history ("
         "receipt_id TEXT PRIMARY KEY,"
         "object_name TEXT NOT NULL,"
         "digest TEXT NOT NULL,"
         "committed_at_ms INTEGER NOT NULL,"
         "manifest_json TEXT NOT NULL)");
    exec("CREATE INDEX IF NOT EXISTS data_receipt_history_lookup "
         "ON data_receipt_history(object_name,digest,committed_at_ms DESC)");
  }

  ~SqliteRepoStore() override
  {
    if (m_db != nullptr) {
      sqlite3_close(m_db);
      m_db = nullptr;
    }
  }

  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) override
  {
    if (manifest.objectName.empty()) {
      throw std::invalid_argument("repo object name must not be empty");
    }

    sqlite3_stmt* stmt = nullptr;
    prepare("INSERT OR REPLACE INTO objects "
            "(object_name, manifest_json, payload, payload_size, sha256, object_type, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'))",
            &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, manifest.objectName);
    const auto manifestJson = manifest.toJson();
    bindText(stmt, 2, manifestJson);
    if (sqlite3_bind_blob(stmt, 3, payload.data(), static_cast<int>(payload.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throwSqlite("failed to bind repo payload");
    }
    if (sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(payload.size())) != SQLITE_OK) {
      throwSqlite("failed to bind repo payload size");
    }
    bindText(stmt, 5, manifest.sha256);
    bindText(stmt, 6, manifest.objectType);
    stepDone(stmt, "failed to store repo object");
  }

  void putManifest(const RepoObjectManifest& manifest) override
  {
    if (manifest.objectName.empty()) {
      throw std::invalid_argument("repo object name must not be empty");
    }

    sqlite3_stmt* stmt = nullptr;
    prepare("INSERT OR REPLACE INTO objects "
            "(object_name, manifest_json, payload, payload_size, sha256, object_type, updated_at) "
            "VALUES (?, ?, ?, 0, ?, ?, strftime('%s','now'))",
            &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, manifest.objectName);
    const auto manifestJson = manifest.toJson();
    bindText(stmt, 2, manifestJson);
    const uint8_t empty = 0;
    if (sqlite3_bind_blob(stmt, 3, &empty, 0, SQLITE_TRANSIENT) != SQLITE_OK) {
      throwSqlite("failed to bind empty repo manifest payload");
    }
    bindText(stmt, 4, manifest.sha256);
    bindText(stmt, 5, manifest.objectType);
    stepDone(stmt, "failed to store repo manifest");
  }

  StoredObject get(const std::string& objectName) const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT manifest_json, payload FROM objects WHERE object_name=?", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, objectName);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      throw std::out_of_range("repo object not found: " + objectName);
    }
    if (rc != SQLITE_ROW) {
      throwSqlite("failed to fetch repo object");
    }

    const auto manifestText = columnText(stmt, 0);
    const auto payloadPtr = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
    const auto payloadSize = sqlite3_column_bytes(stmt, 1);
    std::vector<uint8_t> payload;
    if (payloadPtr != nullptr && payloadSize > 0) {
      payload.assign(payloadPtr, payloadPtr + payloadSize);
    }
    return StoredObject{parseManifestJson(manifestText), std::move(payload)};
  }

  RepoObjectManifest getManifest(const std::string& objectName) const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT manifest_json FROM objects WHERE object_name=?", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, objectName);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      throw std::out_of_range("repo object not found: " + objectName);
    }
    if (rc != SQLITE_ROW) {
      throwSqlite("failed to fetch repo manifest");
    }
    return parseManifestJson(columnText(stmt, 0));
  }

  RepoObjectMetadataProbe
  probeObjectMetadata(const std::string& objectName) const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT manifest_json,payload_size,length(payload),typeof(payload) "
            "FROM objects WHERE object_name=?", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, objectName);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return {};
    if (rc != SQLITE_ROW) throwSqlite("failed to inspect Repo object metadata");

    RepoObjectMetadataProbe result;
    result.manifestPresent = true;
    result.manifest = parseManifestJson(columnText(stmt, 0));
    const auto recordedSize = sqlite3_column_int64(stmt, 1);
    const auto actualSize = sqlite3_column_int64(stmt, 2);
    const auto recordedSizeType = sqlite3_column_type(stmt, 1);
    const auto actualSizeType = sqlite3_column_type(stmt, 2);
    const auto type = columnText(stmt, 3);
    result.payloadMetadataAvailable = true;
    result.payloadPresent = type != "null";
    result.payloadRepresentationValid = result.payloadPresent && type == "blob" &&
      recordedSizeType == SQLITE_INTEGER && actualSizeType == SQLITE_INTEGER &&
      recordedSize >= 0 && actualSize >= 0 && recordedSize == actualSize;
    result.payloadSize = actualSizeType == SQLITE_INTEGER && actualSize >= 0
      ? static_cast<uint64_t>(actualSize) : 0;
    return result;
  }

  bool supportsManifestLookup() const noexcept override
  {
    return true;
  }

  bool has(const std::string& objectName) const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT 1 FROM objects WHERE object_name=? LIMIT 1", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, objectName);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc == SQLITE_DONE) {
      return false;
    }
    throwSqlite("failed to test repo object existence");
  }

  bool erase(const std::string& objectName) override
  {
    exec("BEGIN IMMEDIATE");
    try {
      std::optional<RepoObjectManifest> previous;
      {
        sqlite3_stmt* stmt = nullptr;
        prepare("SELECT manifest_json FROM objects WHERE object_name=?", &stmt);
        StatementGuard guard(stmt);
        bindText(stmt, 1, objectName);
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) previous = parseManifestJson(columnText(stmt, 0));
        else if (rc != SQLITE_DONE) throwSqlite("failed to inspect repo object before delete");
      }
      if (!previous) {
        exec("COMMIT");
        return false;
      }
      if (previous->objectType == "ndn-data-wire" &&
          !previous->dataReceiptId.empty()) {
        sqlite3_stmt* stmt = nullptr;
        prepare("INSERT OR IGNORE INTO data_receipt_history "
                "(receipt_id,object_name,digest,committed_at_ms,manifest_json) "
                "VALUES (?,?,?,?,?)", &stmt);
        StatementGuard guard(stmt);
        bindText(stmt, 1, previous->dataReceiptId);
        bindText(stmt, 2, previous->objectName);
        bindText(stmt, 3, previous->sha256);
        if (sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(
              previous->dataReceiptCommittedAtMs)) != SQLITE_OK) {
          throwSqlite("failed to bind data receipt commit time");
        }
        bindText(stmt, 5, previous->toJson());
        stepDone(stmt, "failed to retain data receipt");
      }
      {
        sqlite3_stmt* stmt = nullptr;
        prepare("DELETE FROM objects WHERE object_name=?", &stmt);
        StatementGuard guard(stmt);
        bindText(stmt, 1, objectName);
        stepDone(stmt, "failed to delete repo object");
      }
      const bool removed = sqlite3_changes(m_db) > 0;
      exec("COMMIT");
      return removed;
    }
    catch (...) {
      try { exec("ROLLBACK"); } catch (...) {}
      throw;
    }
  }

  std::optional<RepoObjectManifest>
  findRetainedDataReceipt(const std::string& dataName,
                          const std::string& wireSha256) const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT manifest_json FROM data_receipt_history "
            "WHERE object_name=? AND digest=? "
            "ORDER BY committed_at_ms DESC,receipt_id DESC LIMIT 1", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, dataName);
    bindText(stmt, 2, wireSha256);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) throwSqlite("failed to query retained data receipt");
    auto manifest = parseManifestJson(columnText(stmt, 0));
    if (manifest.objectName != dataName || manifest.sha256 != wireSha256 ||
        manifest.objectType != "ndn-data-wire" || manifest.dataReceiptId.empty()) {
      throw std::runtime_error("repo-sqlite-data-receipt-corrupt");
    }
    return manifest;
  }

  void attachImportedDataReceipt(const RepoObjectManifest& expected,
                                 const RepoObjectManifest& updated) override
  {
    std::string currentJson;
    {
      sqlite3_stmt* readStmt = nullptr;
      prepare("SELECT manifest_json FROM objects WHERE object_name=?", &readStmt);
      StatementGuard readGuard(readStmt);
      bindText(readStmt, 1, expected.objectName);
      const int readResult = sqlite3_step(readStmt);
      if (readResult != SQLITE_ROW) {
        if (readResult != SQLITE_DONE) throwSqlite("failed to inspect Data import");
        throw std::runtime_error("repo-data-import-manifest-conflict");
      }
      currentJson = columnText(readStmt, 0);
    }
    const auto current = parseManifestJson(currentJson);
    auto withoutReceipt = updated;
    withoutReceipt.dataReceiptId.clear();
    withoutReceipt.dataReceiptRepoNode.clear();
    withoutReceipt.dataReceiptProviderIdentity.clear();
    withoutReceipt.dataReceiptCommittedAtMs = 0;
    if (current.toJson() != expected.toJson() || !current.dataReceiptId.empty() ||
        withoutReceipt.toJson() != expected.toJson() ||
        updated.objectType != "ndn-data-wire" || updated.dataReceiptId.empty() ||
        updated.dataReceiptRepoNode.empty() ||
        updated.dataReceiptProviderIdentity.empty() ||
        updated.dataReceiptCommittedAtMs == 0) {
      throw std::runtime_error("repo-data-import-manifest-conflict");
    }
    sqlite3_stmt* updateStmt = nullptr;
    prepare("UPDATE objects SET manifest_json=? WHERE object_name=? AND manifest_json=?",
            &updateStmt);
    StatementGuard updateGuard(updateStmt);
    bindText(updateStmt, 1, updated.toJson());
    bindText(updateStmt, 2, expected.objectName);
    bindText(updateStmt, 3, currentJson);
    stepDone(updateStmt, "failed to attach imported Data receipt");
    if (sqlite3_changes(m_db) != 1) {
      throw std::runtime_error("repo-data-import-manifest-conflict");
    }
  }

  size_t size() const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT COUNT(*) FROM objects", &stmt);
    StatementGuard guard(stmt);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      throwSqlite("failed to count repo objects");
    }
    return static_cast<size_t>(sqlite3_column_int64(stmt, 0));
  }

  std::vector<RepoObjectManifest> listManifests() const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT manifest_json FROM objects ORDER BY object_name", &stmt);
    StatementGuard guard(stmt);
    std::vector<RepoObjectManifest> manifests;
    while (true) {
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) {
        break;
      }
      if (rc != SQLITE_ROW) {
        throwSqlite("failed to list repo manifests");
      }
      manifests.push_back(parseManifestJson(columnText(stmt, 0)));
    }
    return manifests;
  }

  uint64_t usedBytes() const override
  {
    sqlite3_stmt* stmt = nullptr;
    prepare("SELECT COALESCE(SUM(payload_size), 0) FROM objects", &stmt);
    StatementGuard guard(stmt);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      throwSqlite("failed to sum repo payload bytes");
    }
    return static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  }

  RepoCacheStatus cacheStatus() const override
  {
    RepoCacheStatus status;
    status.storageBackend = "sqlite";
    status.authoritativeBackend = "sqlite";
    return status;
  }

private:
  struct StatementGuard
  {
    explicit StatementGuard(sqlite3_stmt* statement)
      : stmt(statement)
    {
    }

    ~StatementGuard()
    {
      if (stmt != nullptr) {
        sqlite3_finalize(stmt);
      }
    }

    sqlite3_stmt* stmt = nullptr;
  };

  void exec(const std::string& sql)
  {
    char* error = nullptr;
    const int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
      std::string message = error != nullptr ? error : sqlite3_errmsg(m_db);
      sqlite3_free(error);
      throw std::runtime_error("sqlite repo exec failed: " + message);
    }
  }

  void prepare(const std::string& sql, sqlite3_stmt** stmt) const
  {
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, stmt, nullptr) != SQLITE_OK) {
      throwSqlite("failed to prepare sqlite repo statement");
    }
  }

  void bindText(sqlite3_stmt* stmt, int index, const std::string& value) const
  {
    if (sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throwSqlite("failed to bind sqlite repo text");
    }
  }

  void stepDone(sqlite3_stmt* stmt, const std::string& error) const
  {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throwSqlite(error);
    }
  }

  std::string columnText(sqlite3_stmt* stmt, int column) const
  {
    const auto text = sqlite3_column_text(stmt, column);
    const auto size = sqlite3_column_bytes(stmt, column);
    if (text == nullptr || size <= 0) {
      return "";
    }
    return std::string(reinterpret_cast<const char*>(text), size);
  }

  [[noreturn]] void throwSqlite(const std::string& prefix) const
  {
    throw std::runtime_error(prefix + ": " + sqlite3_errmsg(m_db));
  }

private:
  std::string m_databasePath;
  BackendOwnershipLease m_ownership;
  sqlite3* m_db = nullptr;
};

class TieredRepoStore : public RepoStoreBackend
{
public:
  TieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                  uint64_t memoryCacheBytes,
                  std::string authoritativeBackend,
                  uint64_t largeObjectThreshold)
    : m_authoritativeStore(std::move(authoritativeStore))
    , m_largeObjectThreshold(largeObjectThreshold)
  {
    if (m_authoritativeStore == nullptr) {
      throw std::invalid_argument("tiered repo authoritative store must not be null");
    }
    m_status.authoritativeBackend = authoritativeBackend.empty()
      ? "custom" : std::move(authoritativeBackend);
    m_status.storageBackend = memoryCacheBytes == 0
      ? m_status.authoritativeBackend : "tiered";
    m_status.cachePolicy = memoryCacheBytes == 0 ? "disabled" : "lru";
    m_status.budgetBytes = memoryCacheBytes;
    if (m_largeObjectThreshold == 0) {
      throw std::invalid_argument("tiered repo large-object threshold must be positive");
    }
  }

  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) override
  {
    const bool cacheable = manifest.size <= m_largeObjectThreshold;
    std::optional<StoredObject> cached;
    if (cacheable) {
      cached.emplace(StoredObject{manifest, payload});
    }
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    try {
      m_authoritativeStore->put(manifest, std::move(payload));
    }
    catch (const std::exception& e) {
      if (isAmbiguousCommitError(e)) {
        reconcileAmbiguousWrite(manifest.objectName);
      }
      throw;
    }
    auto durable = manifest;
    bool lookupFailed = false;
    try {
      durable = m_authoritativeStore->getManifest(manifest.objectName);
    }
    catch (...) {
      lookupFailed = true;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(manifest.objectName);
    try {
      ++m_epochs[manifest.objectName];
    }
    catch (...) {
      m_status.reconciliationRequired = true;
    }
    if (lookupFailed) {
      m_status.reconciliationRequired = true;
    }
    ++m_status.backingWrites;
    if (cached && !lookupFailed) {
      cached->manifest = durable;
      admitNoThrow(std::move(*cached));
    }
    else if (!cached && m_status.budgetBytes != 0) {
      ++m_status.oversizedBypasses;
    }
  }

  void putManifest(const RepoObjectManifest& manifest) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    try {
      m_authoritativeStore->putManifest(manifest);
    }
    catch (const std::exception& e) {
      if (isAmbiguousCommitError(e)) {
        reconcileAmbiguousWrite(manifest.objectName);
      }
      throw;
    }
    auto durable = manifest;
    bool lookupFailed = false;
    try {
      durable = m_authoritativeStore->getManifest(manifest.objectName);
    }
    catch (...) {
      lookupFailed = true;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(durable.objectName);
    try {
      ++m_epochs[durable.objectName];
    }
    catch (...) {
      m_status.reconciliationRequired = true;
    }
    if (lookupFailed) {
      m_status.reconciliationRequired = true;
    }
    ++m_status.backingWrites;
    if (m_status.budgetBytes != 0) {
      // A manifest-only write never supplies payload bytes.  Do not populate
      // a vector cache entry that could make a later get appear successful.
      ++m_status.oversizedBypasses;
    }
  }

  StoredObject get(const std::string& objectName) const override
  {
    uint64_t observedEpoch = 0;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto epoch = m_epochs.find(objectName);
      observedEpoch = epoch == m_epochs.end() ? 0 : epoch->second;
      const auto found = m_cache.find(objectName);
      if (found != m_cache.end()) {
        ++m_status.hits;
        m_lru.splice(m_lru.end(), m_lru, found->second.recency);
        return found->second.object;
      }
      ++m_status.misses;
      ++m_status.backingReads;
    }
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    auto object = m_authoritativeStore->get(objectName);
    auto result = object;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto epoch = m_epochs.find(objectName);
      if ((epoch == m_epochs.end() ? 0 : epoch->second) == observedEpoch &&
          result.manifest.size <= m_largeObjectThreshold) {
        admitNoThrow(std::move(object));
      }
      else if (m_status.budgetBytes != 0) {
        ++m_status.oversizedBypasses;
      }
    }
    return result;
  }

  void putRange(const RepoObjectManifest& manifest, RepoByteRange range,
                const std::vector<uint8_t>& bytes) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    m_authoritativeStore->putRange(manifest, range, bytes);
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(manifest.objectName);
    try {
      ++m_epochs[manifest.objectName];
    }
    catch (...) {
      m_status.reconciliationRequired = true;
    }
    ++m_status.backingWrites;
  }

  void commitRanges(const RepoObjectManifest& manifest) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    try {
      m_authoritativeStore->commitRanges(manifest);
    }
    catch (const std::exception& e) {
      if (isAmbiguousCommitError(e)) {
        reconcileAmbiguousWrite(manifest.objectName);
      }
      throw;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(manifest.objectName);
    try {
      ++m_epochs[manifest.objectName];
    }
    catch (...) {
      m_status.reconciliationRequired = true;
    }
    ++m_status.backingWrites;
  }

  void abortRanges(const std::string& objectName) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    m_authoritativeStore->abortRanges(objectName);
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(objectName);
    try {
      ++m_epochs[objectName];
    }
    catch (...) {
      m_status.reconciliationRequired = true;
    }
  }

  std::vector<uint8_t> getRange(const std::string& objectName,
                                RepoByteRange range) const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->getRange(objectName, range);
  }

  RepoObjectManifest getManifest(const std::string& objectName) const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->getManifest(objectName);
  }

  RepoObjectMetadataProbe
  probeObjectMetadata(const std::string& objectName) const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->probeObjectMetadata(objectName);
  }

  std::optional<RepoObjectManifest>
  findRetainedDataReceipt(const std::string& dataName,
                          const std::string& wireSha256) const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->findRetainedDataReceipt(dataName, wireSha256);
  }

  void attachImportedDataReceipt(const RepoObjectManifest& expected,
                                 const RepoObjectManifest& updated) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    m_authoritativeStore->attachImportedDataReceipt(expected, updated);
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidate(expected.objectName);
    ++m_epochs[expected.objectName];
    ++m_status.backingWrites;
  }

  bool supportsManifestLookup() const noexcept override
  {
    return m_authoritativeStore->supportsManifestLookup();
  }

  bool supportsRange() const noexcept override
  {
    return m_authoritativeStore->supportsRange();
  }

  uint64_t fullCopyFallbackCount() const noexcept override
  {
    return m_authoritativeStore->fullCopyFallbackCount();
  }

  void pin(const std::string& objectName) const override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_pins[objectName];
  }

  void unpin(const std::string& objectName) const override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_pins.find(objectName);
    if (found == m_pins.end()) {
      return;
    }
    if (found->second <= 1) {
      m_pins.erase(found);
    }
    else {
      --found->second;
    }
  }

  bool has(const std::string& objectName) const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->has(objectName);
  }

  bool erase(const std::string& objectName) override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    bool removed = false;
    try {
      removed = m_authoritativeStore->erase(objectName);
    }
    catch (const std::exception& e) {
      if (isAmbiguousCommitError(e)) {
        reconcileAmbiguousWrite(objectName);
      }
      throw;
    }
    if (removed) {
      std::lock_guard<std::mutex> lock(m_mutex);
      invalidate(objectName);
      try {
        ++m_epochs[objectName];
      }
      catch (...) {
        m_status.reconciliationRequired = true;
      }
      ++m_status.backingWrites;
    }
    return removed;
  }

  size_t size() const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->size();
  }

  std::vector<RepoObjectManifest> listManifests() const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->listManifests();
  }

  uint64_t usedBytes() const override
  {
    std::unique_lock<std::mutex> authorityLock(m_authorityMutex);
    return m_authoritativeStore->usedBytes();
  }

  RepoCacheStatus cacheStatus() const override
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto status = m_status;
    status.usedBytes = m_usedBytes;
    status.entryCount = m_cache.size();
    return status;
  }

private:
  void reconcileAmbiguousWrite(const std::string& objectName) noexcept
  {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      invalidate(objectName);
      try {
        ++m_epochs[objectName];
      }
      catch (...) {
      }
      m_status.reconciliationRequired = true;
      ++m_status.backingWrites;
    }
    catch (...) {
    }
  }

  struct CacheEntry
  {
    StoredObject object;
    uint64_t chargeBytes = 0;
    std::list<std::string>::iterator recency;
  };

  static uint64_t logicalCharge(const StoredObject& object)
  {
    const auto manifestJson = object.manifest.toJson();
    const auto payloadBytes = static_cast<uint64_t>(object.payload.size());
    const auto nameBytes = static_cast<uint64_t>(object.manifest.objectName.size());
    const auto manifestBytes = static_cast<uint64_t>(manifestJson.size());
    const auto max = std::numeric_limits<uint64_t>::max();
    if (payloadBytes > max - nameBytes || payloadBytes + nameBytes > max - manifestBytes) {
      return max;
    }
    return payloadBytes + nameBytes + manifestBytes;
  }

  void invalidate(const std::string& objectName) const
  {
    const auto found = m_cache.find(objectName);
    if (found == m_cache.end()) {
      return;
    }
    m_usedBytes -= found->second.chargeBytes;
    m_lru.erase(found->second.recency);
    m_cache.erase(found);
    ++m_status.invalidations;
  }

  void admitNoThrow(StoredObject object) const noexcept
  {
    try {
      admit(std::move(object));
    }
    catch (...) {
      // The authoritative operation has already succeeded. Cache admission is
      // optional acceleration and must not turn a durable write/read into a
      // reported failure.
    }
  }

  void admit(StoredObject object) const
  {
    if (m_status.budgetBytes == 0) {
      return;
    }

    const auto chargeBytes = logicalCharge(object);
    if (chargeBytes > m_status.budgetBytes) {
      ++m_status.oversizedBypasses;
      return;
    }

    if (m_pins.count(object.manifest.objectName) != 0) {
      return;
    }

    invalidate(object.manifest.objectName);
    size_t inspected = 0;
    while (!m_lru.empty() &&
           m_usedBytes > m_status.budgetBytes - chargeBytes &&
           inspected < m_lru.size()) {
      const auto victimName = m_lru.front();
      const auto victim = m_cache.find(victimName);
      if (m_pins.count(victimName) != 0) {
        m_lru.splice(m_lru.end(), m_lru, m_lru.begin());
        ++inspected;
        continue;
      }
      if (victim != m_cache.end()) {
        m_usedBytes -= victim->second.chargeBytes;
        m_cache.erase(victim);
      }
      m_lru.pop_front();
      ++m_status.evictions;
      inspected = 0;
    }
    if (m_usedBytes > m_status.budgetBytes - chargeBytes) {
      ++m_status.oversizedBypasses;
      return;
    }

    const auto objectName = object.manifest.objectName;
    m_lru.push_back(objectName);
    const auto recency = std::prev(m_lru.end());
    try {
      m_cache.emplace(objectName,
                      CacheEntry{std::move(object), chargeBytes, recency});
    }
    catch (...) {
      m_lru.pop_back();
      throw;
    }
    m_usedBytes += chargeBytes;
    ++m_status.admissions;
  }

private:
  std::shared_ptr<RepoStoreBackend> m_authoritativeStore;
  uint64_t m_largeObjectThreshold;
  mutable std::mutex m_authorityMutex;
  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, CacheEntry> m_cache;
  mutable std::unordered_map<std::string, uint64_t> m_epochs;
  mutable std::unordered_map<std::string, uint64_t> m_pins;
  mutable std::list<std::string> m_lru;
  mutable uint64_t m_usedBytes = 0;
  mutable RepoCacheStatus m_status;
};

RepoDeploymentMode
parseRepoDeploymentMode(const std::string& value)
{
  const auto normalized = normalizeModeText(value);
  if (normalized.empty() || normalized == "remote") {
    return RepoDeploymentMode::Remote;
  }
  if (normalized == "embedded" || normalized == "local" ||
      normalized == "inprocess" || normalized == "inapp") {
    return RepoDeploymentMode::Embedded;
  }
  if (normalized == "both" || normalized == "remoteembedded" ||
      normalized == "embeddedremote" || normalized == "remotelocal" ||
      normalized == "localremote") {
    return RepoDeploymentMode::Both;
  }
  throw std::invalid_argument("unknown repo deployment mode: " + value);
}

std::string
toString(RepoDeploymentMode mode)
{
  switch (mode) {
  case RepoDeploymentMode::Remote:
    return "remote";
  case RepoDeploymentMode::Embedded:
    return "embedded";
  case RepoDeploymentMode::Both:
    return "both";
  }
  return "remote";
}

bool
enablesRemote(RepoDeploymentMode mode)
{
  return mode == RepoDeploymentMode::Remote || mode == RepoDeploymentMode::Both;
}

bool
enablesEmbedded(RepoDeploymentMode mode)
{
  return mode == RepoDeploymentMode::Embedded || mode == RepoDeploymentMode::Both;
}

std::string
RepoObjectManifest::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"objectType\":" << jsonQuote(objectType) << ",";
  os << "\"sha256\":" << jsonQuote(sha256) << ",";
  os << "\"size\":" << size << ",";
  os << "\"segmentCount\":" << segmentCount << ",";
  os << "\"replicationFactor\":" << replicationFactor << ",";
  os << "\"generation\":" << generation << ",";
  os << "\"parentGeneration\":" << parentGeneration << ",";
  os << "\"writeConsistency\":" << jsonQuote(writeConsistency) << ",";
  os << "\"requiredWriteAcks\":"
     << (requiredWriteAcks == 0
           ? ndnsf_distributed_repo::requiredWriteAcks(
               replicationFactor, parseRepoWriteConsistency(writeConsistency))
           : requiredWriteAcks)
     << ",";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"lifecycleState\":" << jsonQuote(lifecycleState) << ",";
  os << "\"dataReceiptId\":" << jsonQuote(dataReceiptId) << ",";
  os << "\"dataReceiptRepoNode\":" << jsonQuote(dataReceiptRepoNode) << ",";
  os << "\"dataReceiptProviderIdentity\":"
     << jsonQuote(dataReceiptProviderIdentity) << ",";
  os << "\"dataReceiptCommittedAtMs\":" << dataReceiptCommittedAtMs << ",";
  os << "\"policyEpoch\":" << jsonQuote(policyEpoch) << ",";
  os << "\"publicationIdentity\":" << jsonQuote(publicationIdentity) << ",";
  os << "\"protectionEpoch\":" << jsonQuote(protectionEpoch) << ",";
  os << "\"keyReferenceId\":" << jsonQuote(keyReferenceId) << ",";
  os << "\"keyReferenceVersion\":" << jsonQuote(keyReferenceVersion) << ",";
  os << "\"ciphertextManifestDigest\":"
     << jsonQuote(ciphertextManifestDigest) << ",";
  os << "\"servingLocator\":" << jsonQuote(servingLocator) << ",";
  os << "\"contentDigest\":" << jsonQuote(contentDigest) << ",";
  os << "\"plaintextSize\":" << plaintextSize << ",";
  os << "\"replicaNodes\":[";
  for (size_t i = 0; i < replicaNodes.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(replicaNodes[i]);
  }
  os << "],";
  os << "\"confirmedReplicaNodes\":[";
  const auto& confirmed = confirmedReplicaNodes.empty()
    ? replicaNodes : confirmedReplicaNodes;
  for (size_t i = 0; i < confirmed.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(confirmed[i]);
  }
  os << "],";
  os << "\"packetNames\":[";
  for (size_t i = 0; i < packetNames.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(packetNames[i]);
  }
  os << "],\"packetOffsets\":[";
  for (size_t i = 0; i < packetOffsets.size(); ++i) {
    if (i != 0) os << ",";
    os << packetOffsets[i];
  }
  os << "],\"envelopeBytes\":" << envelopeBytes
     << ",\"dataPrefix\":" << jsonQuote(dataPrefix) << "}";
  return os.str();
}

std::string
RepoWriteIntent::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"generation\":" << generation << ",";
  os << "\"expectedGeneration\":" << expectedGeneration << ",";
  os << "\"digest\":" << jsonQuote(digest) << ",";
  os << "\"replicationFactor\":" << replicationFactor << ",";
  os << "\"requiredWriteAcks\":" << requiredAcks << ",";
  os << "\"writeConsistency\":" << jsonQuote(consistency) << ",";
  os << "\"state\":" << jsonQuote(state) << ",";
  os << "\"createdAtMs\":" << createdAtMs << ",";
  os << "\"updatedAtMs\":" << updatedAtMs << ",";
  os << "\"selectedReplicas\":[";
  for (size_t i = 0; i < selectedReplicas.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(selectedReplicas[i]);
  }
  os << "]}";
  return os.str();
}

std::string
RepoWriteReceipt::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"repoNode\":" << jsonQuote(repoNode) << ",";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"generation\":" << generation << ",";
  os << "\"digest\":" << jsonQuote(digest) << ",";
  os << "\"persistedBytes\":" << persistedBytes << ",";
  os << "\"state\":" << jsonQuote(state) << ",";
  os << "\"completedAtMs\":" << completedAtMs;
  if (!providerIdentity.empty()) {
    os << ",\"providerIdentity\":" << jsonQuote(providerIdentity);
  }
  if (artifactReference) {
    os << ",\"artifactReference\":" << artifactReference->toJson();
  }
  os << "}";
  return os.str();
}

RepoWriteReceipt
RepoWriteReceipt::dataPacketFromJson(const std::string& wire)
{
  using Kind = detail::JsonValue::Kind;
  const auto parsed = detail::parseJson(wire, 4096);
  const auto& object = detail::requireObject(parsed);
  detail::requireExactKeys(object, {
    "operationId", "repoNode", "providerIdentity", "objectName", "generation",
    "digest", "persistedBytes", "state", "completedAtMs"});
  RepoWriteReceipt receipt;
  receipt.operationId = detail::member(object, "operationId", Kind::String).string;
  receipt.repoNode = detail::member(object, "repoNode", Kind::String).string;
  receipt.providerIdentity = detail::member(object, "providerIdentity", Kind::String).string;
  receipt.objectName = detail::member(object, "objectName", Kind::String).string;
  receipt.generation = detail::member(object, "generation", Kind::Unsigned).number;
  receipt.digest = detail::member(object, "digest", Kind::String).string;
  receipt.persistedBytes = detail::member(object, "persistedBytes", Kind::Unsigned).number;
  receipt.state = detail::member(object, "state", Kind::String).string;
  receipt.completedAtMs = detail::member(object, "completedAtMs", Kind::Unsigned).number;
  if (receipt.operationId.empty() || receipt.repoNode.empty() ||
      receipt.providerIdentity.empty() || receipt.objectName.empty() ||
      receipt.generation == 0 || receipt.digest.size() != 64 ||
      receipt.persistedBytes == 0 || receipt.completedAtMs == 0 ||
      receipt.state != "COMMITTED") {
    throw std::invalid_argument("repo-data-receipt-invalid");
  }
  return receipt;
}

std::string
RepoDataPacketQuery::toJson() const
{
  std::ostringstream os;
  os << "{\"schemaVersion\":" << schemaVersion
     << ",\"dataName\":" << jsonQuote(dataName)
     << ",\"expectedWireSha256\":" << jsonQuote(expectedWireSha256) << "}";
  return os.str();
}

RepoDataPacketQuery
RepoDataPacketQuery::fromJson(const std::string& wire)
{
  using Kind = detail::JsonValue::Kind;
  const auto parsed = detail::parseJson(wire, 8192);
  const auto& object = detail::requireObject(parsed);
  detail::requireExactKeys(object, {"schemaVersion", "dataName", "expectedWireSha256"});
  RepoDataPacketQuery query;
  const auto schemaVersion = detail::member(object, "schemaVersion", Kind::Unsigned).number;
  if (schemaVersion != 1) throw std::invalid_argument("repo-data-query-invalid");
  query.schemaVersion = 1;
  query.dataName = detail::member(object, "dataName", Kind::String).string;
  query.expectedWireSha256 = detail::member(
    object, "expectedWireSha256", Kind::String).string;
  if (query.dataName.empty() ||
      query.expectedWireSha256.size() != 64 ||
      !std::all_of(query.expectedWireSha256.begin(), query.expectedWireSha256.end(),
                   [] (char ch) { return (ch >= '0' && ch <= '9') ||
                                          (ch >= 'a' && ch <= 'f'); })) {
    throw std::invalid_argument("repo-data-query-invalid");
  }
  return query;
}

std::string
RepoDataPacketLocation::toJson() const
{
  std::ostringstream os;
  os << "{\"schemaVersion\":" << schemaVersion
     << ",\"receiptId\":" << jsonQuote(receiptId)
     << ",\"repoNode\":" << jsonQuote(repoNode)
     << ",\"providerIdentity\":" << jsonQuote(providerIdentity)
     << ",\"dataName\":" << jsonQuote(dataName)
     << ",\"wireSha256\":" << jsonQuote(wireSha256)
     << ",\"state\":" << jsonQuote(state)
     << ",\"forwardingHint\":" << jsonQuote(forwardingHint)
     << ",\"observedAtMs\":" << observedAtMs
     << ",\"expiresAtMs\":" << expiresAtMs << "}";
  return os.str();
}

RepoDataPacketLocation
RepoDataPacketLocation::fromJson(const std::string& wire)
{
  using Kind = detail::JsonValue::Kind;
  const auto parsed = detail::parseJson(wire, 8192);
  const auto& object = detail::requireObject(parsed);
  detail::requireExactKeys(object, {"schemaVersion", "receiptId", "repoNode",
    "providerIdentity", "dataName", "wireSha256", "state", "forwardingHint",
    "observedAtMs", "expiresAtMs"});
  RepoDataPacketLocation location;
  const auto schemaVersion = detail::member(object, "schemaVersion", Kind::Unsigned).number;
  if (schemaVersion != 1) throw std::invalid_argument("repo-data-location-invalid");
  location.schemaVersion = 1;
  location.receiptId = detail::member(object, "receiptId", Kind::String).string;
  location.repoNode = detail::member(object, "repoNode", Kind::String).string;
  location.providerIdentity = detail::member(
    object, "providerIdentity", Kind::String).string;
  location.dataName = detail::member(object, "dataName", Kind::String).string;
  location.wireSha256 = detail::member(object, "wireSha256", Kind::String).string;
  location.state = detail::member(object, "state", Kind::String).string;
  location.forwardingHint = detail::member(object, "forwardingHint", Kind::String).string;
  location.observedAtMs = detail::member(object, "observedAtMs", Kind::Unsigned).number;
  location.expiresAtMs = detail::member(object, "expiresAtMs", Kind::Unsigned).number;
  if (location.receiptId.empty() ||
      location.repoNode.empty() || location.providerIdentity.empty() ||
      location.dataName.empty() || location.wireSha256.size() != 64 ||
      location.observedAtMs == 0 || location.expiresAtMs <= location.observedAtMs ||
      (location.state != "PRESENT" && location.state != "ABSENT" &&
       location.state != "UNKNOWN") ||
      (location.state == "PRESENT"
         ? location.forwardingHint != location.providerIdentity
         : !location.forwardingHint.empty())) {
    throw std::invalid_argument("repo-data-location-invalid");
  }
  return location;
}

std::vector<uint8_t>
ArtifactStoreRequest::toWire(const ArtifactLimits& limits) const
{
  if (schemaVersion != 2 || generation == 0 ||
      bytes.size() != reference.sizeBytes ||
      bytes.size() > std::min<uint64_t>(limits.maxChunkBytes, 16 * 1024 * 1024)) {
    throw std::invalid_argument("repo-artifact-store-request-invalid: version, size, or generation is invalid");
  }
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  validateOperationOwner(operationOwner, limits);
  const std::string header = "{\"schemaVersion\":2,\"operationId\":" +
    detail::quoteJson(operationId) + ",\"generation\":" +
    std::to_string(generation) + ",\"artifact\":" + reference.toJson() +
    ",\"operationOwner\":" + operationOwnerJson(operationOwner) +
    ",\"payloadBytes\":" + std::to_string(bytes.size()) + "}";
  return frameArtifactRequest("ARV2PUT2", header, bytes);
}

ArtifactStoreRequest
ArtifactStoreRequest::fromWire(const std::vector<uint8_t>& wire,
                               const ArtifactLimits& limits)
{
  size_t payloadOffset = 0;
  const bool version2Frame = hasArtifactFrameMagic(wire, "ARV2PUT2");
  const auto header = unframeArtifactRequest(
    wire, version2Frame ? "ARV2PUT2" : "ARV2PUT1", payloadOffset);
  const auto root = detail::parseJson(header, 64 * 1024);
  const auto& object = detail::requireObject(root);
  const auto wireVersion = jsonUnsigned(object, "schemaVersion");
  if ((wireVersion != 1 && wireVersion != 2) ||
      (wireVersion == 2) != version2Frame) {
    throw std::invalid_argument("repo-artifact-store-request-invalid: unknown schema version");
  }
  if (wireVersion == 2) {
    detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation",
      "artifact", "operationOwner", "payloadBytes"});
  }
  else {
    detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation",
      "artifact", "payloadBytes"});
  }
  ArtifactStoreRequest result;
  result.operationId = jsonString(object, "operationId");
  result.generation = jsonUnsigned(object, "generation");
  result.reference = ArtifactReference::fromJson(
    detail::renderJson(object.at("artifact")), limits);
  if (wireVersion == 2) {
    result.operationOwner = parseOperationOwner(object.at("operationOwner"));
  }
  const auto payloadBytes = jsonUnsigned(object, "payloadBytes");
  if (payloadBytes != wire.size() - payloadOffset) {
    throw std::invalid_argument("repo-artifact-store-request-invalid: payload length mismatch");
  }
  result.bytes.assign(wire.begin() + payloadOffset, wire.end());
  (void)result.toWire(limits);
  return result;
}

std::vector<uint8_t>
ArtifactRangeWriteRequest::toWire(const ArtifactLimits& limits) const
{
  if (schemaVersion != 2 || generation == 0 || lengthBytes == 0 ||
      lengthBytes != bytes.size() || offsetBytes > reference.sizeBytes ||
      lengthBytes > reference.sizeBytes - offsetBytes ||
      lengthBytes > std::min<uint64_t>(limits.maxChunkBytes, 16 * 1024 * 1024)) {
    throw std::invalid_argument("repo-artifact-range-request-invalid: range fields are inconsistent");
  }
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  validateOperationOwner(operationOwner, limits);
  const std::string header = "{\"schemaVersion\":2,\"operationId\":" +
    detail::quoteJson(operationId) + ",\"generation\":" +
    std::to_string(generation) + ",\"artifact\":" + reference.toJson() +
    ",\"operationOwner\":" + operationOwnerJson(operationOwner) +
    ",\"offsetBytes\":" + std::to_string(offsetBytes) +
    ",\"lengthBytes\":" + std::to_string(lengthBytes) +
    ",\"payloadBytes\":" + std::to_string(bytes.size()) + "}";
  return frameArtifactRequest("ARV2RNG2", header, bytes);
}

ArtifactRangeWriteRequest
ArtifactRangeWriteRequest::fromWire(const std::vector<uint8_t>& wire,
                                    const ArtifactLimits& limits)
{
  size_t payloadOffset = 0;
  const bool version2Frame = hasArtifactFrameMagic(wire, "ARV2RNG2");
  const auto header = unframeArtifactRequest(
    wire, version2Frame ? "ARV2RNG2" : "ARV2RNG1", payloadOffset);
  const auto root = detail::parseJson(header, 64 * 1024);
  const auto& object = detail::requireObject(root);
  const auto wireVersion = jsonUnsigned(object, "schemaVersion");
  if ((wireVersion != 1 && wireVersion != 2) ||
      (wireVersion == 2) != version2Frame) {
    throw std::invalid_argument("repo-artifact-range-request-invalid: unknown schema version");
  }
  if (wireVersion == 2) {
    detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation",
      "artifact", "operationOwner", "offsetBytes", "lengthBytes", "payloadBytes"});
  }
  else {
    detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation",
      "artifact", "offsetBytes", "lengthBytes", "payloadBytes"});
  }
  ArtifactRangeWriteRequest result;
  result.operationId = jsonString(object, "operationId");
  result.generation = jsonUnsigned(object, "generation");
  result.reference = ArtifactReference::fromJson(
    detail::renderJson(object.at("artifact")), limits);
  if (wireVersion == 2) {
    result.operationOwner = parseOperationOwner(object.at("operationOwner"));
  }
  result.offsetBytes = jsonUnsigned(object, "offsetBytes");
  result.lengthBytes = jsonUnsigned(object, "lengthBytes");
  const auto payloadBytes = jsonUnsigned(object, "payloadBytes");
  if (payloadBytes != wire.size() - payloadOffset) {
    throw std::invalid_argument("repo-artifact-range-request-invalid: payload length mismatch");
  }
  result.bytes.assign(wire.begin() + payloadOffset, wire.end());
  (void)result.toWire(limits);
  return result;
}

std::vector<uint8_t>
ArtifactRangeCommitRequest::toWire(const ArtifactLimits& limits) const
{
  if (schemaVersion != 2 || generation == 0) {
    throw std::invalid_argument("repo-artifact-commit-request-invalid: generation or version invalid");
  }
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  validateOperationOwner(operationOwner, limits);
  const std::string header = "{\"schemaVersion\":2,\"operationId\":" +
    detail::quoteJson(operationId) + ",\"generation\":" +
    std::to_string(generation) + ",\"artifact\":" + reference.toJson() +
    ",\"operationOwner\":" + operationOwnerJson(operationOwner) + "}";
  return frameArtifactRequest("ARV2COM2", header);
}

ArtifactRangeCommitRequest
ArtifactRangeCommitRequest::fromWire(const std::vector<uint8_t>& wire,
                                     const ArtifactLimits& limits)
{
  size_t payloadOffset = 0;
  const bool version2Frame = hasArtifactFrameMagic(wire, "ARV2COM2");
  const auto header = unframeArtifactRequest(
    wire, version2Frame ? "ARV2COM2" : "ARV2COM1", payloadOffset);
  if (payloadOffset != wire.size()) {
    throw std::invalid_argument("repo-artifact-commit-request-invalid: unexpected payload bytes");
  }
  const auto root = detail::parseJson(header, 64 * 1024);
  const auto& object = detail::requireObject(root);
  const auto wireVersion = jsonUnsigned(object, "schemaVersion");
  if ((wireVersion != 1 && wireVersion != 2) ||
      (wireVersion == 2) != version2Frame) {
    throw std::invalid_argument("repo-artifact-commit-request-invalid: unknown schema version");
  }
  if (wireVersion == 2) {
    detail::requireExactKeys(object,
      {"schemaVersion", "operationId", "generation", "artifact", "operationOwner"});
  }
  else {
    detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation", "artifact"});
  }
  ArtifactRangeCommitRequest result;
  result.operationId = jsonString(object, "operationId");
  result.generation = jsonUnsigned(object, "generation");
  result.reference = ArtifactReference::fromJson(
    detail::renderJson(object.at("artifact")), limits);
  if (wireVersion == 2) {
    result.operationOwner = parseOperationOwner(object.at("operationOwner"));
  }
  (void)result.toWire(limits);
  return result;
}

std::vector<uint8_t>
ArtifactRangeAbortRequest::toWire(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || generation == 0) {
    throw std::invalid_argument("repo-artifact-abort-request-invalid: generation or version invalid");
  }
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  const std::string header = "{\"schemaVersion\":1,\"operationId\":" +
    detail::quoteJson(operationId) + ",\"generation\":" +
    std::to_string(generation) + ",\"artifact\":" + reference.toJson() + "}";
  return frameArtifactRequest("ARV2ABT1", header);
}

ArtifactRangeAbortRequest
ArtifactRangeAbortRequest::fromWire(const std::vector<uint8_t>& wire,
                                    const ArtifactLimits& limits)
{
  size_t payloadOffset = 0;
  const auto header = unframeArtifactRequest(wire, "ARV2ABT1", payloadOffset);
  if (payloadOffset != wire.size()) {
    throw std::invalid_argument("repo-artifact-abort-request-invalid: unexpected payload bytes");
  }
  const auto root = detail::parseJson(header, 64 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operationId", "generation", "artifact"});
  if (jsonUnsigned(object, "schemaVersion") != 1) {
    throw std::invalid_argument("repo-artifact-abort-request-invalid: unknown schema version");
  }
  ArtifactRangeAbortRequest result;
  result.operationId = jsonString(object, "operationId");
  result.generation = jsonUnsigned(object, "generation");
  result.reference = ArtifactReference::fromJson(
    detail::renderJson(object.at("artifact")), limits);
  (void)result.toWire(limits);
  return result;
}

std::string
RepoCapacityReservation::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"reservationId\":" << jsonQuote(reservationId) << ",";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"reservedBytes\":" << reservedBytes << ",";
  os << "\"state\":" << jsonQuote(state) << ",";
  os << "\"expiresAtMs\":" << expiresAtMs;
  os << "}";
  return os.str();
}

std::string
RepoDataReference::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"dataPrefix\":" << jsonQuote(dataPrefix) << ",";
  os << "\"firstSegment\":" << firstSegment << ",";
  os << "\"finalSegment\":" << finalSegment << ",";
  os << "\"hasFinalSegment\":" << (hasFinalSegment ? "true" : "false") << ",";
  os << "\"forwardingHint\":" << jsonQuote(forwardingHint) << ",";
  os << "\"expectedSha256\":" << jsonQuote(expectedSha256) << ",";
  os << "\"expectedSize\":" << expectedSize << ",";
  os << "\"storeWirePackets\":" << (storeWirePackets ? "true" : "false") << ",";
  os << "\"objectType\":" << jsonQuote(objectType);
  os << "}";
  return os.str();
}

std::string
RepoOperationStatus::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"operation\":" << jsonQuote(operation) << ",";
  os << "\"state\":" << jsonQuote(state) << ",";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"message\":" << jsonQuote(message) << ",";
  os << "\"completedSegments\":" << completedSegments << ",";
  os << "\"totalSegments\":" << totalSegments;
  os << ",\"createdAtMs\":" << createdAtMs;
  os << ",\"updatedAtMs\":" << updatedAtMs;
  os << ",\"expiresAtMs\":" << expiresAtMs;
  os << "}";
  return os.str();
}

bool
RepoOperationMetrics::isCanonicalPhase(const std::string& phase)
{
  static const std::array<const char*, 11> phases = {{
    "discovery", "ackCollection", "planning", "queueWait", "sessionStart",
    "transfer", "verification", "persistence", "replication", "commit",
    "activation",
  }};
  return std::find_if(phases.begin(), phases.end(), [&phase] (const char* candidate) {
    return phase == candidate;
  }) != phases.end();
}

void
RepoOperationMetrics::validate() const
{
  if (operationId.empty() || operationId.size() > MAX_OPERATION_ID_BYTES ||
      std::any_of(operationId.begin(), operationId.end(), [] (unsigned char ch) {
        return std::iscntrl(ch) != 0;
      })) {
    throw std::invalid_argument("repo-metrics-invalid-operation-id");
  }
  if (completedAtMs != 0 && completedAtMs < startedAtMs) {
    throw std::invalid_argument("repo-metrics-invalid-time-boundary");
  }
  for (const auto& timing : phaseTimingsMs) {
    if (!isCanonicalPhase(timing.first) || !std::isfinite(timing.second) ||
        timing.second < 0.0) {
      throw std::invalid_argument("repo-metrics-invalid-phase");
    }
  }
  if (!std::isfinite(asymmetricVerificationMs) ||
      asymmetricVerificationMs < 0.0 ||
      !std::isfinite(digestVerificationMs) ||
      digestVerificationMs < 0.0) {
    throw std::invalid_argument("repo-metrics-invalid-crypto-time");
  }
  const auto checkedByteSum = [] (uint64_t left, uint64_t right) {
    if (std::numeric_limits<uint64_t>::max() - left < right) {
      throw std::invalid_argument("repo-metrics-byte-counter-overflow");
    }
    return left + right;
  };
  const auto detailedWireBytes =
    checkedByteSum(dataWireBytes, interestWireBytes);
  if (detailedWireBytes != 0 && wireBytes != detailedWireBytes) {
    throw std::invalid_argument("repo-metrics-inconsistent-wire-bytes");
  }
  const auto detailedReadBytes =
    checkedByteSum(payloadStoreBytesRead, metadataStoreBytesRead);
  if (detailedReadBytes != 0 && storageBytesRead != detailedReadBytes) {
    throw std::invalid_argument("repo-metrics-inconsistent-storage-read-bytes");
  }
  const auto detailedWrittenBytes =
    checkedByteSum(payloadStoreBytesWritten, metadataStoreBytesWritten);
  if (detailedWrittenBytes != 0 &&
      storageBytesWritten != detailedWrittenBytes) {
    throw std::invalid_argument("repo-metrics-inconsistent-storage-written-bytes");
  }
  if (selectedReplicaCount > requestedReplicaCount ||
      committedReplicaCount > selectedReplicaCount) {
    throw std::invalid_argument("repo-metrics-invalid-replica-count");
  }
}

std::string
RepoOperationMetrics::toJson() const
{
  validate();
  std::ostringstream os;
  os << "{";
  os << "\"operationId\":" << jsonQuote(operationId) << ",";
  os << "\"startedAtMs\":" << startedAtMs << ",";
  os << "\"completedAtMs\":" << completedAtMs << ",";
  os << "\"phaseTimingsMs\":{";
  size_t phaseIndex = 0;
  for (const auto& timing : phaseTimingsMs) {
    if (phaseIndex++ != 0) {
      os << ",";
    }
    os << jsonQuote(timing.first) << ":" << timing.second;
  }
  os << "},";
  os << "\"logicalPayloadBytes\":" << logicalPayloadBytes << ",";
  os << "\"dataWireBytes\":" << dataWireBytes << ",";
  os << "\"interestWireBytes\":" << interestWireBytes << ",";
  os << "\"wireBytes\":" << wireBytes << ",";
  os << "\"retransmittedBytes\":" << retransmittedBytes << ",";
  os << "\"payloadStoreBytesRead\":" << payloadStoreBytesRead << ",";
  os << "\"payloadStoreBytesWritten\":" << payloadStoreBytesWritten << ",";
  os << "\"metadataStoreBytesRead\":" << metadataStoreBytesRead << ",";
  os << "\"metadataStoreBytesWritten\":" << metadataStoreBytesWritten << ",";
  os << "\"storageBytesRead\":" << storageBytesRead << ",";
  os << "\"storageBytesWritten\":" << storageBytesWritten << ",";
  os << "\"asymmetricVerifications\":" << asymmetricVerifications << ",";
  os << "\"digestVerifications\":" << digestVerifications << ",";
  os << "\"asymmetricVerificationMs\":" << asymmetricVerificationMs << ",";
  os << "\"digestVerificationMs\":" << digestVerificationMs << ",";
  os << "\"controlOperations\":" << controlOperations << ",";
  os << "\"metadataOperations\":" << metadataOperations << ",";
  os << "\"metadataRecordCount\":" << metadataRecordCount << ",";
  os << "\"requestedReplicaCount\":" << requestedReplicaCount << ",";
  os << "\"selectedReplicaCount\":" << selectedReplicaCount << ",";
  os << "\"committedReplicaCount\":" << committedReplicaCount << ",";
  os << "\"rejectedReplicaReceiptCount\":" << rejectedReplicaReceiptCount;
  os << "}";
  return os.str();
}

std::string
StorageCapability::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"repoNode\":" << jsonQuote(repoNode) << ",";
  os << "\"repoMode\":" << jsonQuote(repoMode) << ",";
  os << "\"acceptsBackupReplica\":" << (acceptsBackupReplica ? "true" : "false") << ",";
  os << "\"freeBytes\":" << freeBytes << ",";
  os << "\"usedBytes\":" << usedBytes << ",";
  os << "\"recentLoad\":" << recentLoad << ",";
  os << "\"availabilityScore\":" << availabilityScore << ",";
  os << "\"failureDomain\":" << jsonQuote(failureDomain) << ",";
  os << "\"storageClasses\":[";
  for (size_t i = 0; i < storageClasses.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(storageClasses[i]);
  }
  os << "]}";
  return os.str();
}

std::string
RepoCatalogEntry::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"objectName\":" << jsonQuote(manifest.objectName) << ",";
  os << "\"manifestSha256\":" << jsonQuote(manifest.sha256) << ",";
  os << "\"objectType\":" << jsonQuote(manifest.objectType) << ",";
  os << "\"size\":" << manifest.size << ",";
  os << "\"segmentCount\":" << manifest.segmentCount << ",";
  os << "\"sourceRepo\":" << jsonQuote(sourceRepo) << ",";
  os << "\"repoMode\":" << jsonQuote(repoMode) << ",";
  os << "\"state\":" << jsonQuote(state) << ",";
  os << "\"catalogEpoch\":" << catalogEpoch << ",";
  os << "\"replicaNodes\":[";
  for (size_t i = 0; i < manifest.replicaNodes.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(manifest.replicaNodes[i]);
  }
  os << "],";
  os << "\"manifest\":" << manifest.toJson();
  os << "}";
  return os.str();
}

std::string
RepoCatalogStatus::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"repoNode\":" << jsonQuote(repoNode) << ",";
  os << "\"repoMode\":" << jsonQuote(repoMode) << ",";
  os << "\"catalogEpoch\":" << catalogEpoch << ",";
  os << "\"objectCount\":" << objectCount << ",";
  os << "\"acceptsBackupReplica\":" << (acceptsBackupReplica ? "true" : "false") << ",";
  os << "\"reconciliationRequired\":"
     << (reconciliationRequired ? "true" : "false");
  os << "}";
  return os.str();
}

std::string
RepoCatalogDelta::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"repoNode\":" << jsonQuote(repoNode) << ",";
  os << "\"repoMode\":" << jsonQuote(repoMode) << ",";
  os << "\"sinceEpoch\":" << sinceEpoch << ",";
  os << "\"catalogEpoch\":" << catalogEpoch << ",";
  os << "\"entries\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << entries[i].toJson();
  }
  os << "]}";
  return os.str();
}

std::string
RepoCacheStatus::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"storageBackend\":" << jsonQuote(storageBackend) << ",";
  os << "\"authoritativeBackend\":" << jsonQuote(authoritativeBackend) << ",";
  os << "\"cachePolicy\":" << jsonQuote(cachePolicy) << ",";
  os << "\"budgetBytes\":" << budgetBytes << ",";
  os << "\"usedBytes\":" << usedBytes << ",";
  os << "\"entryCount\":" << entryCount << ",";
  os << "\"hits\":" << hits << ",";
  os << "\"misses\":" << misses << ",";
  os << "\"admissions\":" << admissions << ",";
  os << "\"evictions\":" << evictions << ",";
  os << "\"invalidations\":" << invalidations << ",";
  os << "\"oversizedBypasses\":" << oversizedBypasses << ",";
  os << "\"backingReads\":" << backingReads << ",";
  os << "\"backingWrites\":" << backingWrites << ",";
  os << "\"reconciliationRequired\":"
     << (reconciliationRequired ? "true" : "false");
  os << "}";
  return os.str();
}

bool
isInAppRepo(const StorageCapability& capability)
{
  const auto normalized = normalizeModeText(capability.repoMode);
  return normalized == "inapp" || normalized == "embedded" ||
         normalized == "local" || normalized == "inprocess";
}

bool
isPersistentRepo(const StorageCapability& capability)
{
  const auto normalized = normalizeModeText(capability.repoMode);
  return normalized.empty() || normalized == "persistent" ||
         normalized == "standalone" || normalized == "remote";
}

std::string
sha256Hex(const std::vector<uint8_t>& payload)
{
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(payload.data(), payload.size(), digest);

  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (uint8_t byte : digest) {
    os << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return os.str();
}

std::vector<StorageCapability>
selectReplicas(const std::vector<StorageCapability>& candidates,
               const PlacementPolicy& policy,
               uint64_t objectSize)
{
  std::vector<StorageCapability> filtered;
  for (const auto& candidate : candidates) {
    if (!candidate.repoNode.empty() &&
        candidate.acceptsBackupReplica &&
        candidate.freeBytes >= objectSize) {
      filtered.push_back(candidate);
    }
  }

  std::sort(filtered.begin(), filtered.end(),
            [] (const StorageCapability& lhs, const StorageCapability& rhs) {
              const double lhsScore = scoreCandidate(lhs);
              const double rhsScore = scoreCandidate(rhs);
              if (lhsScore == rhsScore) {
                return lhs.repoNode < rhs.repoNode;
              }
              return lhsScore > rhsScore;
            });

  std::vector<StorageCapability> selected;
  std::set<std::string> selectedFailureDomains;
  for (const auto& candidate : filtered) {
    if (selected.size() >= policy.replicationFactor) {
      break;
    }
    if (policy.avoidSameFailureDomain && !candidate.failureDomain.empty() &&
        selectedFailureDomains.count(candidate.failureDomain) != 0) {
      continue;
    }
    selected.push_back(candidate);
    if (!candidate.failureDomain.empty()) {
      selectedFailureDomains.insert(candidate.failureDomain);
    }
  }

  if (selected.size() < policy.replicationFactor) {
    for (const auto& candidate : filtered) {
      if (selected.size() >= policy.replicationFactor) {
        break;
      }
      const auto alreadySelected = std::any_of(
        selected.begin(), selected.end(),
        [&] (const StorageCapability& item) {
          return item.repoNode == candidate.repoNode;
        });
      if (!alreadySelected) {
        selected.push_back(candidate);
      }
    }
  }

  return selected;
}

RepoCacheStatus
RepoStoreBackend::cacheStatus() const
{
  return {};
}

void
RepoStoreBackend::putRange(const RepoObjectManifest&, RepoByteRange,
                           const std::vector<uint8_t>&)
{
  throw std::runtime_error("repo-range-write-not-supported");
}

void
RepoStoreBackend::commitRanges(const RepoObjectManifest&)
{
  throw std::runtime_error("repo-range-commit-not-supported");
}

std::vector<uint8_t>
RepoStoreBackend::getRange(const std::string& objectName,
                           RepoByteRange range) const
{
  const auto object = get(objectName);
  if (range.offsetBytes > object.payload.size() ||
      range.lengthBytes > object.payload.size() - range.offsetBytes) {
    throw std::out_of_range("repo-range-read-out-of-bounds");
  }
  return std::vector<uint8_t>(
    object.payload.begin() + static_cast<std::ptrdiff_t>(range.offsetBytes),
    object.payload.begin() + static_cast<std::ptrdiff_t>(
      range.offsetBytes + range.lengthBytes));
}

RepoObjectManifest
RepoStoreBackend::getManifest(const std::string& objectName) const
{
  return get(objectName).manifest;
}

RepoObjectMetadataProbe
RepoStoreBackend::probeObjectMetadata(const std::string&) const
{
  return {};
}

std::optional<RepoObjectManifest>
RepoStoreBackend::findRetainedDataReceipt(const std::string&,
                                          const std::string&) const
{
  return std::nullopt;
}

void
RepoStoreBackend::attachImportedDataReceipt(const RepoObjectManifest&,
                                            const RepoObjectManifest&)
{
  throw std::runtime_error("repo-data-import-backend-unsupported");
}

bool
RepoStoreBackend::supportsRange() const noexcept
{
  return false;
}

uint64_t
RepoStoreBackend::fullCopyFallbackCount() const noexcept
{
  return 0;
}

void
RepoStoreBackend::pin(const std::string&) const
{
}

void
RepoStoreBackend::unpin(const std::string&) const
{
}

void
RepoStoreBackend::abortRanges(const std::string&)
{
}

bool
RepoStoreBackend::supportsManifestLookup() const noexcept
{
  return false;
}

std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath)
{
  return std::make_shared<SqliteRepoStore>(databasePath);
}

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(const std::string& databasePath, uint64_t memoryCacheBytes,
                    uint64_t largeObjectThreshold)
{
  return makeTieredRepoStore(makeSqliteRepoStore(databasePath),
                             memoryCacheBytes,
                             "sqlite", largeObjectThreshold);
}

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                    uint64_t memoryCacheBytes,
                    std::string authoritativeBackend,
                    uint64_t largeObjectThreshold)
{
  return std::make_shared<TieredRepoStore>(std::move(authoritativeStore),
                                           memoryCacheBytes,
                                           std::move(authoritativeBackend),
                                           largeObjectThreshold);
}

} // namespace ndnsf_distributed_repo
