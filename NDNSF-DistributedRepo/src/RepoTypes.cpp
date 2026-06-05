#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
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
  os << "\"policyEpoch\":" << jsonQuote(policyEpoch) << ",";
  os << "\"replicaNodes\":[";
  for (size_t i = 0; i < replicaNodes.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(replicaNodes[i]);
  }
  os << "]}";
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

} // namespace ndnsf_distributed_repo
