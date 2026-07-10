#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

std::string
jsonQuote(const std::string& value)
{
  std::ostringstream os;
  os << '"';
  for (char ch : value) {
    switch (ch) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\r':
      os << "\\r";
      break;
    case '\t':
      os << "\\t";
      break;
    default:
      os << ch;
      break;
    }
  }
  os << '"';
  return os.str();
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
    sqlite3_stmt* stmt = nullptr;
    prepare("DELETE FROM objects WHERE object_name=?", &stmt);
    StatementGuard guard(stmt);
    bindText(stmt, 1, objectName);
    stepDone(stmt, "failed to delete repo object");
    return sqlite3_changes(m_db) > 0;
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
  sqlite3* m_db = nullptr;
};

class TieredRepoStore : public RepoStoreBackend
{
public:
  TieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                  uint64_t memoryCacheBytes,
                  std::string authoritativeBackend)
    : m_authoritativeStore(std::move(authoritativeStore))
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
  }

  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) override
  {
    StoredObject cached{manifest, payload};
    m_authoritativeStore->put(manifest, std::move(payload));
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_status.backingWrites;
    invalidate(manifest.objectName);
    admitNoThrow(std::move(cached));
  }

  void putManifest(const RepoObjectManifest& manifest) override
  {
    m_authoritativeStore->putManifest(manifest);
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_status.backingWrites;
    invalidate(manifest.objectName);
    admitNoThrow(StoredObject{manifest, {}});
  }

  StoredObject get(const std::string& objectName) const override
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_cache.find(objectName);
      if (found != m_cache.end()) {
        ++m_status.hits;
        m_lru.splice(m_lru.end(), m_lru, found->second.recency);
        return found->second.object;
      }
      ++m_status.misses;
      ++m_status.backingReads;
    }
    auto object = m_authoritativeStore->get(objectName);
    auto result = object;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      admitNoThrow(std::move(object));
    }
    return result;
  }

  bool has(const std::string& objectName) const override
  {
    return m_authoritativeStore->has(objectName);
  }

  bool erase(const std::string& objectName) override
  {
    const bool removed = m_authoritativeStore->erase(objectName);
    if (removed) {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_status.backingWrites;
      invalidate(objectName);
    }
    return removed;
  }

  size_t size() const override
  {
    return m_authoritativeStore->size();
  }

  std::vector<RepoObjectManifest> listManifests() const override
  {
    return m_authoritativeStore->listManifests();
  }

  uint64_t usedBytes() const override
  {
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

    invalidate(object.manifest.objectName);
    while (!m_lru.empty() &&
           m_usedBytes > m_status.budgetBytes - chargeBytes) {
      const auto victimName = m_lru.front();
      const auto victim = m_cache.find(victimName);
      if (victim != m_cache.end()) {
        m_usedBytes -= victim->second.chargeBytes;
        m_cache.erase(victim);
      }
      m_lru.pop_front();
      ++m_status.evictions;
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
  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, CacheEntry> m_cache;
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
  os << "\"policyEpoch\":" << jsonQuote(policyEpoch) << ",";
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
  os << "]}";
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
  os << "}";
  return os.str();
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
  os << "\"acceptsBackupReplica\":" << (acceptsBackupReplica ? "true" : "false");
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
  os << "\"backingWrites\":" << backingWrites;
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

void
InMemoryRepoStore::put(const RepoObjectManifest& manifest,
                       std::vector<uint8_t> payload)
{
  if (manifest.objectName.empty()) {
    throw std::invalid_argument("repo object name must not be empty");
  }
  m_objects[manifest.objectName] = StoredObject{manifest, std::move(payload)};
}

void
InMemoryRepoStore::putManifest(const RepoObjectManifest& manifest)
{
  if (manifest.objectName.empty()) {
    throw std::invalid_argument("repo object name must not be empty");
  }
  m_objects[manifest.objectName] = StoredObject{manifest, {}};
}

StoredObject
InMemoryRepoStore::get(const std::string& objectName) const
{
  auto it = m_objects.find(objectName);
  if (it == m_objects.end()) {
    throw std::out_of_range("repo object not found: " + objectName);
  }
  return it->second;
}

bool
InMemoryRepoStore::has(const std::string& objectName) const
{
  return m_objects.count(objectName) != 0;
}

bool
InMemoryRepoStore::erase(const std::string& objectName)
{
  return m_objects.erase(objectName) != 0;
}

size_t
InMemoryRepoStore::size() const
{
  return m_objects.size();
}

std::vector<RepoObjectManifest>
InMemoryRepoStore::listManifests() const
{
  std::vector<RepoObjectManifest> manifests;
  manifests.reserve(m_objects.size());
  for (const auto& item : m_objects) {
    manifests.push_back(item.second.manifest);
  }
  return manifests;
}

uint64_t
InMemoryRepoStore::usedBytes() const
{
  uint64_t used = 0;
  for (const auto& item : m_objects) {
    used += item.second.payload.size();
  }
  return used;
}

RepoCacheStatus
RepoStoreBackend::cacheStatus() const
{
  return {};
}

RepoCacheStatus
InMemoryRepoStore::cacheStatus() const
{
  RepoCacheStatus status;
  status.storageBackend = "memory";
  status.authoritativeBackend = "memory";
  return status;
}

std::shared_ptr<RepoStoreBackend>
makeMemoryRepoStore()
{
  return std::make_shared<InMemoryRepoStore>();
}

std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath)
{
  return std::make_shared<SqliteRepoStore>(databasePath);
}

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(const std::string& databasePath, uint64_t memoryCacheBytes)
{
  return makeTieredRepoStore(makeSqliteRepoStore(databasePath),
                             memoryCacheBytes,
                             "sqlite");
}

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                    uint64_t memoryCacheBytes,
                    std::string authoritativeBackend)
{
  return std::make_shared<TieredRepoStore>(std::move(authoritativeStore),
                                           memoryCacheBytes,
                                           std::move(authoritativeBackend));
}

} // namespace ndnsf_distributed_repo
