#include "ndnsf-distributed-repo/LegacyRepoImporter.hpp"

#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/util/span.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <variant>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>

namespace ndnsf_distributed_repo {
namespace {

namespace fs = std::filesystem;
using boost::property_tree::ptree;

constexpr uint64_t MAX_IMPORT_RANGE_BYTES = 16 * 1024 * 1024;
constexpr uint64_t MAX_IMPORT_DATA_WIRE_BYTES = 64 * 1024 * 1024;

struct JsonValue
{
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;
  using Value = std::variant<std::nullptr_t, bool, int64_t, uint64_t,
                             std::string, Array, Object>;

  JsonValue() : value(nullptr) {}
  JsonValue(std::nullptr_t) : value(nullptr) {}
  JsonValue(bool v) : value(v) {}
  JsonValue(int v) : value(static_cast<int64_t>(v)) {}
  JsonValue(unsigned v) : value(static_cast<uint64_t>(v)) {}
  JsonValue(int64_t v) : value(v) {}
  JsonValue(uint64_t v) : value(v) {}
  JsonValue(std::string v) : value(std::move(v)) {}
  JsonValue(const char* v) : value(std::string(v)) {}
  JsonValue(Array v) : value(std::move(v)) {}
  JsonValue(Object v) : value(std::move(v)) {}

  Value value;
};

std::string
quoteJson(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for (const unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned>(c) << std::dec;
        }
        else {
          out << static_cast<char>(c);
        }
    }
  }
  out << '"';
  return out.str();
}

void
appendJson(std::ostringstream& out, const JsonValue& value)
{
  if (std::holds_alternative<std::nullptr_t>(value.value)) {
    out << "null";
  }
  else if (const auto* v = std::get_if<bool>(&value.value)) {
    out << (*v ? "true" : "false");
  }
  else if (const auto* v = std::get_if<int64_t>(&value.value)) {
    out << *v;
  }
  else if (const auto* v = std::get_if<uint64_t>(&value.value)) {
    out << *v;
  }
  else if (const auto* v = std::get_if<std::string>(&value.value)) {
    out << quoteJson(*v);
  }
  else if (const auto* v = std::get_if<JsonValue::Array>(&value.value)) {
    out << '[';
    bool first = true;
    for (const auto& item : *v) {
      if (!first) out << ',';
      first = false;
      appendJson(out, item);
    }
    out << ']';
  }
  else {
    const auto& object = std::get<JsonValue::Object>(value.value);
    out << '{';
    bool first = true;
    for (const auto& entry : object) {
      if (!first) out << ',';
      first = false;
      out << quoteJson(entry.first) << ':';
      appendJson(out, entry.second);
    }
    out << '}';
  }
}

std::string
dumpJson(const JsonValue& value)
{
  std::ostringstream out;
  appendJson(out, value);
  return out.str();
}

class Digest
{
public:
  Digest()
    : m_context(EVP_MD_CTX_new())
  {
    if (!m_context || EVP_DigestInit_ex(m_context, EVP_sha256(), nullptr) != 1) {
      if (m_context) EVP_MD_CTX_free(m_context);
      throw LegacyRepoImportError("legacy-import-digest-init-failed");
    }
  }

  ~Digest() { EVP_MD_CTX_free(m_context); }

  void update(const void* data, size_t size)
  {
    if (size != 0 && EVP_DigestUpdate(m_context, data, size) != 1) {
      throw LegacyRepoImportError("legacy-import-digest-update-failed");
    }
  }

  std::string finish()
  {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned size = 0;
    if (EVP_DigestFinal_ex(m_context, bytes.data(), &size) != 1) {
      throw LegacyRepoImportError("legacy-import-digest-final-failed");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned i = 0; i < size; ++i) {
      out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
  }

private:
  EVP_MD_CTX* m_context;
};

std::string
digestString(const std::string& value)
{
  Digest digest;
  digest.update(value.data(), value.size());
  return digest.finish();
}

std::string
digestFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw LegacyRepoImportError("legacy-import-source-read-failed");
  Digest digest;
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) digest.update(buffer.data(), static_cast<size_t>(count));
  }
  if (!input.eof()) throw LegacyRepoImportError("legacy-import-source-read-failed");
  return digest.finish();
}

std::string
isoTimestamp()
{
  const auto now = std::chrono::system_clock::to_time_t(
    std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

class Statement
{
public:
  Statement(sqlite3* db, const std::string& sql)
    : m_db(db)
  {
    const int result = sqlite3_prepare_v2(db, sql.c_str(), -1, &m_statement, nullptr);
    if (result != SQLITE_OK) {
      throw LegacyRepoImportError(std::string("legacy-import-sqlite-read-failed: ") +
                                  sqlite3_errmsg(db));
    }
  }

  ~Statement() { sqlite3_finalize(m_statement); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  bool step()
  {
    const int result = sqlite3_step(m_statement);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    throw LegacyRepoImportError(std::string("legacy-import-sqlite-read-failed: ") +
                                sqlite3_errmsg(m_db));
  }

  void bind(int index, const std::string& value)
  {
    if (sqlite3_bind_text(m_statement, index, value.c_str(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
      throw LegacyRepoImportError("legacy-import-sqlite-bind-failed");
    }
  }

  sqlite3_stmt* get() const noexcept { return m_statement; }

private:
  sqlite3* m_db;
  sqlite3_stmt* m_statement = nullptr;
};

class Database
{
public:
  explicit Database(const fs::path& path)
  {
    if (sqlite3_open_v2(path.c_str(), &m_db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      const std::string message = m_db ? sqlite3_errmsg(m_db) : "allocation failed";
      if (m_db) sqlite3_close(m_db);
      m_db = nullptr;
      throw LegacyRepoImportError("legacy-import-source-open-failed: " + message);
    }
    sqlite3_busy_timeout(m_db, 5000);
  }

  ~Database() { if (m_db) sqlite3_close(m_db); }
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  sqlite3* get() const noexcept { return m_db; }

private:
  sqlite3* m_db = nullptr;
};

std::string
columnText(sqlite3_stmt* statement, int column)
{
  const auto* value = sqlite3_column_text(statement, column);
  const int length = sqlite3_column_bytes(statement, column);
  return value ? std::string(reinterpret_cast<const char*>(value),
                             static_cast<size_t>(length)) : std::string();
}

JsonValue
sqliteValue(sqlite3_stmt* statement, int column)
{
  switch (sqlite3_column_type(statement, column)) {
    case SQLITE_NULL: return JsonValue(nullptr);
    case SQLITE_INTEGER: return JsonValue(static_cast<int64_t>(
      sqlite3_column_int64(statement, column)));
    case SQLITE_FLOAT: {
      std::ostringstream value;
      value << std::setprecision(17) << sqlite3_column_double(statement, column);
      return JsonValue(value.str());
    }
    case SQLITE_TEXT: return JsonValue(columnText(statement, column));
    case SQLITE_BLOB: {
      const auto* bytes = static_cast<const uint8_t*>(
        sqlite3_column_blob(statement, column));
      const int size = sqlite3_column_bytes(statement, column);
      static constexpr char HEX[] = "0123456789abcdef";
      std::string hex;
      hex.reserve(static_cast<size_t>(size) * 2);
      for (int i = 0; i < size; ++i) {
        hex.push_back(HEX[bytes[i] >> 4]);
        hex.push_back(HEX[bytes[i] & 0x0f]);
      }
      return JsonValue(JsonValue::Object{{"blobHex", JsonValue(std::move(hex))},
                                         {"type", JsonValue("blob")}});
    }
    default: throw LegacyRepoImportError("legacy-import-sqlite-value-invalid");
  }
}

std::string
quoteIdentifier(const std::string& identifier)
{
  std::string result = "\"";
  for (const char c : identifier) {
    if (c == '"') result += "\"\"";
    else result.push_back(c);
  }
  result.push_back('"');
  return result;
}

std::vector<std::string>
getTableNames(sqlite3* db)
{
  Statement query(db, "SELECT name FROM sqlite_master WHERE type='table' "
                      "AND name NOT LIKE 'sqlite_%' ORDER BY name");
  std::vector<std::string> names;
  while (query.step()) names.push_back(columnText(query.get(), 0));
  return names;
}

uint64_t
countRows(sqlite3* db, const std::string& table)
{
  Statement query(db, "SELECT COUNT(*) FROM " + quoteIdentifier(table));
  if (!query.step()) throw LegacyRepoImportError("legacy-import-count-failed");
  return static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0));
}

JsonValue
pragmaRows(sqlite3* db, const std::string& table, const std::string& pragma)
{
  Statement query(db, "PRAGMA " + pragma + "(" + quoteIdentifier(table) + ")");
  JsonValue::Array rows;
  while (query.step()) {
    JsonValue::Array row;
    const int columns = sqlite3_column_count(query.get());
    for (int i = 0; i < columns; ++i) row.push_back(sqliteValue(query.get(), i));
    rows.emplace_back(std::move(row));
  }
  return JsonValue(std::move(rows));
}

JsonValue
tableProjection(sqlite3* db, const std::string& table)
{
  Statement ddl(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?");
  ddl.bind(1, table);
  if (!ddl.step() || sqlite3_column_type(ddl.get(), 0) == SQLITE_NULL) {
    throw LegacyRepoImportError("legacy-import-schema-table-ddl-missing");
  }
  const std::string tableSql = columnText(ddl.get(), 0);
  JsonValue::Array indexes;
  Statement indexList(db, "PRAGMA index_list(" + quoteIdentifier(table) + ")");
  while (indexList.step()) {
    const std::string name = columnText(indexList.get(), 1);
    JsonValue::Array record{
      JsonValue(name), sqliteValue(indexList.get(), 2),
      sqliteValue(indexList.get(), 3), sqliteValue(indexList.get(), 4)};
    record.push_back(pragmaRows(db, name, "index_info"));
    Statement indexDdl(db, "SELECT sql FROM sqlite_master WHERE type='index' AND name=?");
    indexDdl.bind(1, name);
    record.push_back(indexDdl.step() ? sqliteValue(indexDdl.get(), 0) : JsonValue(nullptr));
    indexes.emplace_back(std::move(record));
  }
  std::sort(indexes.begin(), indexes.end(), [](const JsonValue& left,
                                                const JsonValue& right) {
    return std::get<std::string>(std::get<JsonValue::Array>(left.value)[0].value) <
           std::get<std::string>(std::get<JsonValue::Array>(right.value)[0].value);
  });
  return JsonValue(JsonValue::Object{
    {"columns", pragmaRows(db, table, "table_info")},
    {"foreignKeys", pragmaRows(db, table, "foreign_key_list")},
    {"indexes", JsonValue(std::move(indexes))},
    {"name", JsonValue(table)},
    {"tableSql", JsonValue(tableSql)}});
}

struct SchemaProfile
{
  std::string id;
  std::string fingerprint;
  std::vector<std::string> tables;
  bool hasRepoTables = false;
};

const std::vector<SchemaProfile>&
schemaProfiles()
{
  static const std::vector<SchemaProfile> profiles{
    {"python-artifact-v12",
     "73c858bbe42890f8e66957bb159c5ff3d62e3203bbe5bd104dc328204c7f8017",
     {"artifact_active_catalog", "artifact_capacity_reservations",
      "artifact_finalization_journal", "artifact_gc_claims",
      "artifact_lifecycle_journal", "artifact_replica_receipts",
      "artifact_schema_state", "artifact_transfer_sessions"}, false},
    {"python-repo-v12-exact-packets",
     "56dc41b33bd3b67c4c0cd863482b4355beda11585b141122946e28093c974682",
     {"artifact_active_catalog", "artifact_capacity_reservations",
      "artifact_finalization_journal", "artifact_gc_claims",
      "artifact_lifecycle_journal", "artifact_replica_receipts",
      "artifact_schema_state", "artifact_transfer_sessions",
      "capacity_reservations", "catalog_journal", "catalog_tombstones",
      "data_packets", "data_segments", "object_packet_refs", "objects",
      "peer_watermarks", "repair_jobs", "repo_membership", "repo_meta",
      "serving_packets", "serving_prefixes", "write_operations",
      "write_receipts"}, true}
  };
  return profiles;
}

std::string
schemaFingerprint(sqlite3* db, const SchemaProfile& profile)
{
  JsonValue::Array tables;
  for (const auto& table : profile.tables) tables.push_back(tableProjection(db, table));
  JsonValue::Object markers{
    {"artifact_schema_state", JsonValue(JsonValue::Object{
      {"format_version", JsonValue("artifact-manifest-v2")},
      {"schema_generation", JsonValue(12)},
      {"singleton", JsonValue(1)}})}};
  if (profile.hasRepoTables) {
    markers.emplace("repo_meta.schema_version", JsonValue("12"));
  }
  const auto canonical = dumpJson(JsonValue(JsonValue::Object{
    {"markers", JsonValue(std::move(markers))},
    {"profileId", JsonValue(profile.id)},
    {"tables", JsonValue(std::move(tables))}}));
  return digestString(canonical);
}

class TemporaryDirectory
{
public:
  TemporaryDirectory()
  {
    static std::atomic<uint64_t> serial{0};
    const auto root = fs::temp_directory_path();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      m_path = root / ("ndnsf-spec192-import-" + std::to_string(::getpid()) + "-" +
                       std::to_string(serial.fetch_add(1)));
      if (::mkdir(m_path.c_str(), S_IRWXU) == 0) return;
      if (errno != EEXIST) {
        throw LegacyRepoImportError("legacy-import-private-snapshot-create-failed");
      }
    }
    throw LegacyRepoImportError("legacy-import-private-snapshot-create-failed");
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

struct FileIdentity
{
  std::string name;
  uint64_t size = 0;
  std::string sha256;
};

JsonValue
identityJson(const FileIdentity& identity)
{
  return JsonValue(JsonValue::Object{
    {"name", JsonValue(identity.name)},
    {"sha256", JsonValue(identity.sha256)},
    {"size", JsonValue(identity.size)}});
}

std::vector<FileIdentity>
databaseBundle(const fs::path& database)
{
  std::vector<FileIdentity> files;
  for (const std::string suffix : {"", "-shm", "-wal"}) {
    const fs::path path(database.string() + suffix);
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found) continue;
    if (error || status.type() != fs::file_type::regular) {
      throw LegacyRepoImportError("legacy-import-source-sidecar-not-regular");
    }
    files.push_back({suffix.empty() ? "database" : suffix,
                     static_cast<uint64_t>(fs::file_size(path)), digestFile(path)});
  }
  if (files.empty() || files.front().name != "database") {
    throw LegacyRepoImportError("legacy-import-source-database-missing");
  }
  return files;
}

std::string
bundleDigest(const std::vector<FileIdentity>& files)
{
  JsonValue::Array values;
  for (const auto& file : files) values.push_back(identityJson(file));
  return digestString(dumpJson(JsonValue(std::move(values))));
}

fs::path
copyReadOnlySnapshot(const fs::path& sourceDatabase,
                     const std::vector<FileIdentity>& before,
                     const fs::path& scratchRoot)
{
  const auto destination = scratchRoot / "source.sqlite3";
  for (const auto& identity : before) {
    const std::string suffix = identity.name == "database" ? "" : identity.name;
    std::error_code error;
    fs::copy_file(sourceDatabase.string() + suffix,
                  destination.string() + suffix,
                  fs::copy_options::none, error);
    if (error) throw LegacyRepoImportError("legacy-import-source-snapshot-copy-failed");
  }
  const auto after = databaseBundle(sourceDatabase);
  if (bundleDigest(before) != bundleDigest(after)) {
    throw LegacyRepoImportError("legacy-import-source-changed-during-snapshot");
  }
  return destination;
}

std::string
payloadTreeDigest(const fs::path& root)
{
  std::vector<FileIdentity> files;
  std::error_code error;
  const auto rootStatus = fs::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory ||
      rootStatus.type() == fs::file_type::not_found) {
    return bundleDigest(files);
  }
  if (error || rootStatus.type() != fs::file_type::directory) {
    throw LegacyRepoImportError("legacy-import-payload-root-invalid");
  }
  for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
    const auto status = it->symlink_status(error);
    if (error) throw LegacyRepoImportError("legacy-import-payload-inventory-failed");
    if (status.type() == fs::file_type::directory) continue;
    if (status.type() != fs::file_type::regular) {
      throw LegacyRepoImportError("legacy-import-payload-entry-not-regular");
    }
    const auto relative = fs::relative(it->path(), root, error);
    if (error) throw LegacyRepoImportError("legacy-import-payload-inventory-failed");
    files.push_back({relative.generic_string(),
                     static_cast<uint64_t>(it->file_size()), digestFile(it->path())});
  }
  std::sort(files.begin(), files.end(), [](const FileIdentity& a, const FileIdentity& b) {
    return a.name < b.name;
  });
  return bundleDigest(files);
}

std::string
schemaProfileId(sqlite3* db, const std::vector<std::string>& names,
                std::string& fingerprint, bool& hasRepoTables)
{
  Statement extras(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type IN ('view','trigger') AND name NOT LIKE 'sqlite_%'");
  if (!extras.step() || sqlite3_column_int64(extras.get(), 0) != 0) {
    throw LegacyRepoImportError("legacy-import-unsupported-schema-objects");
  }
  for (const auto& profile : schemaProfiles()) {
    if (names != profile.tables) continue;
    Statement marker(db, "SELECT singleton,schema_generation,format_version "
                         "FROM artifact_schema_state");
    if (!marker.step() || sqlite3_column_int64(marker.get(), 0) != 1 ||
        sqlite3_column_int64(marker.get(), 1) != 12 ||
        columnText(marker.get(), 2) != "artifact-manifest-v2" || marker.step()) {
      throw LegacyRepoImportError("legacy-import-unsupported-schema-marker");
    }
    if (profile.hasRepoTables) {
      Statement repoMarker(db, "SELECT value FROM repo_meta WHERE key='schema_version'");
      if (!repoMarker.step() || columnText(repoMarker.get(), 0) != "12" ||
          repoMarker.step() || countRows(db, "data_segments") != 0) {
        throw LegacyRepoImportError("legacy-import-unsupported-repo-profile");
      }
    }
    fingerprint = schemaFingerprint(db, profile);
    if (fingerprint != profile.fingerprint) {
      throw LegacyRepoImportError("legacy-import-unsupported-schema-fingerprint");
    }
    hasRepoTables = profile.hasRepoTables;
    return profile.id;
  }
  throw LegacyRepoImportError("legacy-import-unsupported-schema");
}

std::string
canonicalDestination(const fs::path& destination)
{
  if (destination.empty() || destination.filename().empty()) {
    throw LegacyRepoImportError("legacy-import-destination-invalid");
  }
  const auto parent = fs::weakly_canonical(destination.parent_path().empty()
                                            ? fs::current_path()
                                            : destination.parent_path());
  return (parent / destination.filename()).lexically_normal().string();
}

void
validateImportId(const std::string& id)
{
  if (id.empty() || id.size() > 128 || id == "." || id == ".." ||
      !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
      })) {
    throw LegacyRepoImportError("legacy-import-id-invalid");
  }
}

struct BlobInfo
{
  uint64_t size = 0;
  std::string sha256;
};

BlobInfo
copyBlobToFile(sqlite3* db, const std::string& table, const std::string& column,
               sqlite3_int64 rowid, const fs::path& destination)
{
  sqlite3_blob* blob = nullptr;
  if (sqlite3_blob_open(db, "main", table.c_str(), column.c_str(), rowid,
                        0, &blob) != SQLITE_OK) {
    throw LegacyRepoImportError("legacy-import-source-blob-open-failed");
  }
  std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> owner(
    blob, sqlite3_blob_close);
  const int blobSize = sqlite3_blob_bytes(blob);
  if (blobSize < 0) throw LegacyRepoImportError("legacy-import-source-blob-size-invalid");
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
  Digest digest;
  std::array<uint8_t, 1024 * 1024> buffer{};
  int offset = 0;
  while (offset < blobSize) {
    const int count = std::min<int>(static_cast<int>(buffer.size()), blobSize - offset);
    if (sqlite3_blob_read(blob, buffer.data(), count, offset) != SQLITE_OK) {
      throw LegacyRepoImportError("legacy-import-source-blob-read-failed");
    }
    output.write(reinterpret_cast<const char*>(buffer.data()), count);
    if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
    digest.update(buffer.data(), static_cast<size_t>(count));
    offset += count;
  }
  output.close();
  if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
  return {static_cast<uint64_t>(blobSize), digest.finish()};
}

std::vector<uint8_t>
readFileVector(const fs::path& path, uint64_t limit)
{
  const auto size = fs::file_size(path);
  if (size > limit || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    throw LegacyRepoImportError("legacy-import-data-wire-too-large");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  std::ifstream input(path, std::ios::binary);
  if (!input || (!bytes.empty() &&
      !input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size())))) {
    throw LegacyRepoImportError("legacy-import-scratch-read-failed");
  }
  return bytes;
}

BlobInfo
validateDataWire(const fs::path& wirePath, const std::string& declaredName,
                 uint64_t maxWireBytes, const fs::path& contentPath = {})
{
  const auto bytes = readFileVector(wirePath, maxWireBytes);
  if (bytes.empty()) throw LegacyRepoImportError("legacy-import-data-wire-malformed");
  std::string encodedName;
  ndn::Block content;
  try {
    ndn::Block block(ndn::span<const uint8_t>(bytes.data(), bytes.size()));
    block.parse();
    const ndn::Data data(block);
    encodedName = data.getName().toUri();
    content = data.getContent();
  }
  catch (const std::exception&) {
    throw LegacyRepoImportError("legacy-import-data-wire-malformed");
  }
  if (encodedName != declaredName) {
    throw LegacyRepoImportError("legacy-import-data-wire-name-mismatch");
  }
  if (!contentPath.empty()) {
    std::ofstream output(contentPath, std::ios::binary | std::ios::trunc);
    if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
    if (content.isValid() && content.value_size() > 0) {
      output.write(reinterpret_cast<const char*>(content.value()),
                   static_cast<std::streamsize>(content.value_size()));
    }
    output.close();
    if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
  }
  return {static_cast<uint64_t>(bytes.size()), digestString(std::string(
    reinterpret_cast<const char*>(bytes.data()), bytes.size()))};
}

BlobInfo
digestAndSizeFile(const fs::path& path)
{
  return {static_cast<uint64_t>(fs::file_size(path)), digestFile(path)};
}

bool
sameFiles(const fs::path& left, const fs::path& right)
{
  if (fs::file_size(left) != fs::file_size(right)) return false;
  std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
  if (!a || !b) throw LegacyRepoImportError("legacy-import-scratch-read-failed");
  std::array<char, 1024 * 1024> ab{}, bb{};
  while (a && b) {
    a.read(ab.data(), static_cast<std::streamsize>(ab.size()));
    b.read(bb.data(), static_cast<std::streamsize>(bb.size()));
    const auto ac = a.gcount(), bc = b.gcount();
    if (ac != bc || !std::equal(ab.begin(), ab.begin() + ac, bb.begin())) return false;
  }
  if (!a.eof() || !b.eof()) throw LegacyRepoImportError("legacy-import-scratch-read-failed");
  return true;
}

std::vector<std::string>
primaryKeyColumns(sqlite3* db, const std::string& table)
{
  Statement query(db, "PRAGMA table_info(" + quoteIdentifier(table) + ")");
  std::map<int, std::string> ordered;
  while (query.step()) {
    const int ordinal = sqlite3_column_int(query.get(), 5);
    if (ordinal > 0) ordered.emplace(ordinal, columnText(query.get(), 1));
  }
  std::vector<std::string> columns;
  for (const auto& entry : ordered) columns.push_back(entry.second);
  return columns;
}

JsonValue
canonicalRow(sqlite3_stmt* row)
{
  JsonValue::Array names, values;
  const int columns = sqlite3_column_count(row);
  for (int i = 0; i < columns; ++i) {
    names.emplace_back(sqlite3_column_name(row, i));
    values.push_back(sqliteValue(row, i));
  }
  return JsonValue(JsonValue::Object{
    {"columns", JsonValue(std::move(names))},
    {"values", JsonValue(std::move(values))}});
}

std::string
sourceRowKey(sqlite3_stmt* row, sqlite3* db, const std::string& table)
{
  const auto keys = primaryKeyColumns(db, table);
  JsonValue::Array values;
  if (keys.empty()) {
    values.emplace_back(static_cast<int64_t>(sqlite3_column_int64(row, 0)));
  }
  else {
    for (const auto& key : keys) {
      int column = -1;
      for (int i = 0; i < sqlite3_column_count(row); ++i) {
        if (key == sqlite3_column_name(row, i)) { column = i; break; }
      }
      if (column < 0) throw LegacyRepoImportError("legacy-import-primary-key-invalid");
      values.push_back(sqliteValue(row, column));
    }
  }
  return table + ":" + dumpJson(JsonValue(std::move(values)));
}

void
collectLegacyClaims(sqlite3* db, const std::vector<std::string>& tables,
                    std::vector<LegacyImportClaim>& claims)
{
  static const std::set<std::string> CLAIM_TABLES{
    "artifact_active_catalog", "artifact_capacity_reservations",
    "artifact_finalization_journal", "artifact_gc_claims",
    "artifact_lifecycle_journal", "artifact_replica_receipts",
    "artifact_transfer_sessions", "capacity_reservations", "catalog_journal",
    "catalog_tombstones", "peer_watermarks", "repair_jobs", "repo_membership",
    "repo_meta", "serving_prefixes", "write_operations", "write_receipts"};
  for (const auto& table : tables) {
    if (CLAIM_TABLES.count(table) == 0) continue;
    const auto keys = primaryKeyColumns(db, table);
    std::string order;
    for (const auto& key : keys) {
      if (!order.empty()) order += ",";
      order += quoteIdentifier(key);
    }
    const auto sql = "SELECT * FROM " + quoteIdentifier(table) +
      (order.empty() ? " ORDER BY rowid" : " ORDER BY " + order);
    Statement query(db, sql);
    while (query.step()) {
      claims.push_back({table, sourceRowKey(query.get(), db, table),
                        digestString(dumpJson(canonicalRow(query.get())))});
    }
  }
}

struct PacketPlan
{
  std::string name;
  fs::path wirePath;
  fs::path contentPath;
  uint64_t sizeBytes = 0;
  std::string sha256;
};

struct ImportAction
{
  enum class Kind { Artifact, RepoObject, RepoPacket } kind;
  LegacyImportItem item;
  fs::path payloadPath;
  ArtifactReference artifactReference;
  RepoObjectManifest repoManifest;
};

struct ImportPlan
{
  std::vector<ImportAction> actions;
  std::map<std::string, PacketPlan> packets;
  std::vector<LegacyImportClaim> legacyClaims;
};

void
writeEmptyFile(const fs::path& path)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
}

void
appendFile(const fs::path& source, const fs::path& destination,
           Digest* digest = nullptr, uint64_t* byteCount = nullptr)
{
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::app);
  if (!input || !output) throw LegacyRepoImportError("legacy-import-scratch-io-failed");
  std::array<char, 1024 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      output.write(buffer.data(), count);
      if (!output) throw LegacyRepoImportError("legacy-import-scratch-write-failed");
      if (digest) digest->update(buffer.data(), static_cast<size_t>(count));
      if (byteCount) *byteCount += static_cast<uint64_t>(count);
    }
  }
  if (!input.eof()) throw LegacyRepoImportError("legacy-import-scratch-read-failed");
}

PacketPlan
loadPacket(sqlite3* db, const std::string& table, const std::string& name,
           const fs::path& scratch, uint64_t maxWireBytes, uint64_t serial)
{
  const std::string querySql = table == "data_packets"
    ? "SELECT rowid,wire_sha256,wire_size FROM data_packets WHERE data_name=?"
    : "SELECT rowid,wire_sha256,wire_size FROM serving_packets WHERE data_name=?";
  Statement query(db, querySql);
  query.bind(1, name);
  if (!query.step()) throw LegacyRepoImportError("legacy-import-data-wire-missing");
  const auto rowid = sqlite3_column_int64(query.get(), 0);
  const auto declaredDigest = columnText(query.get(), 1);
  const auto declaredSize = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 2));
  if (declaredSize > maxWireBytes) {
    throw LegacyRepoImportError("legacy-import-data-wire-too-large");
  }
  if (query.step()) throw LegacyRepoImportError("legacy-import-data-wire-ambiguous");
  const auto wirePath = scratch / ("wire-" + std::to_string(serial));
  const auto contentPath = scratch / ("content-" + std::to_string(serial));
  const auto blob = copyBlobToFile(db, table, "wire", rowid, wirePath);
  if (blob.size != declaredSize || blob.sha256 != declaredDigest ||
      blob.size > maxWireBytes) {
    throw LegacyRepoImportError("legacy-import-data-wire-digest-mismatch");
  }
  const auto wire = validateDataWire(wirePath, name, maxWireBytes, contentPath);
  if (wire.size != blob.size || wire.sha256 != blob.sha256) {
    throw LegacyRepoImportError("legacy-import-data-wire-digest-mismatch");
  }
  return {name, wirePath, contentPath, wire.size, wire.sha256};
}

void
addPacket(ImportPlan& plan, sqlite3* db, const std::string& table,
          const std::string& name, const fs::path& scratch,
          uint64_t maxWireBytes, uint64_t serial)
{
  auto packet = loadPacket(db, table, name, scratch, maxWireBytes, serial);
  auto existing = plan.packets.find(name);
  if (existing != plan.packets.end()) {
    if (existing->second.sha256 != packet.sha256 ||
        existing->second.sizeBytes != packet.sizeBytes ||
        !sameFiles(existing->second.wirePath, packet.wirePath)) {
      throw LegacyRepoImportError("legacy-import-data-wire-name-conflict");
    }
    return;
  }
  plan.packets.emplace(name, std::move(packet));
}

std::vector<LegacyImportClaim>
acceptedArtifactLifecycle(sqlite3* db, const std::string& operationId,
                          const std::string& digest, uint64_t generation)
{
  Statement query(db, "SELECT sequence,event_id,operation_id,artifact_digest,generation,"
                      "from_state,to_state,event_time_ms,accepted,detail_json,error "
                      "FROM artifact_lifecycle_journal WHERE operation_id=? "
                      "ORDER BY sequence");
  query.bind(1, operationId);
  bool committed = false;
  bool active = false;
  std::string lastAcceptedState;
  std::vector<LegacyImportClaim> claims;
  while (query.step()) {
    const auto sequence = sqlite3_column_int64(query.get(), 0);
    const auto fromState = columnText(query.get(), 5);
    const auto state = columnText(query.get(), 6);
    const auto eventDigest = columnText(query.get(), 3);
    const auto eventGeneration = static_cast<uint64_t>(sqlite3_column_int64(query.get(), 4));
    const bool accepted = sqlite3_column_int(query.get(), 8) == 1;
    if (!accepted || eventDigest != digest || eventGeneration != generation) continue;
    if (state == "COMMITTED") {
      committed = true;
      active = false;
    }
    else if (state == "ACTIVE" && committed &&
             lastAcceptedState == "COMMITTED" && fromState == "COMMITTED") {
      active = true;
    }
    else {
      active = false;
    }
    lastAcceptedState = state;
    claims.push_back({"artifact_lifecycle_journal",
      "artifact_lifecycle_journal:[" + std::to_string(sequence) + "]",
      digestString(dumpJson(canonicalRow(query.get())))});
  }
  if (!committed || !active) {
    throw LegacyRepoImportError("legacy-import-artifact-not-terminal-active");
  }
  return claims;
}

ImportPlan
buildImportPlan(sqlite3* db, const std::vector<std::string>& tables,
                const LegacyRepoImportConfig& config,
                const fs::path& scratch, LegacyImportManifest& manifest)
{
  ImportPlan plan;
  collectLegacyClaims(db, tables, plan.legacyClaims);
  uint64_t serial = 0;
  Statement catalog(db, "SELECT logical_name,policy_epoch,format_version,digest_algorithm,"
                       "artifact_digest,generation,operation_id,artifact_json "
                       "FROM artifact_active_catalog ORDER BY logical_name,policy_epoch");
  while (catalog.step()) {
    const auto logicalName = columnText(catalog.get(), 0);
    const auto policyEpoch = columnText(catalog.get(), 1);
    const auto formatVersion = columnText(catalog.get(), 2);
    const auto digestAlgorithm = columnText(catalog.get(), 3);
    const auto digest = columnText(catalog.get(), 4);
    const auto storedGeneration = sqlite3_column_int64(catalog.get(), 5);
    if (storedGeneration <= 0) {
      throw LegacyRepoImportError("legacy-import-artifact-generation-invalid");
    }
    const auto generation = static_cast<uint64_t>(storedGeneration);
    const auto operationId = columnText(catalog.get(), 6);
    const auto artifactJson = columnText(catalog.get(), 7);
    if (formatVersion != "artifact-manifest-v2" || digestAlgorithm != "sha256" ||
        operationId.empty()) {
      throw LegacyRepoImportError("legacy-import-artifact-catalog-invalid");
    }
    ArtifactReference reference;
    try {
      reference = ArtifactReference::fromJson(artifactJson);
    }
    catch (const std::exception&) {
      throw LegacyRepoImportError("legacy-import-artifact-reference-invalid");
    }
    if (reference.logicalName != logicalName || reference.policyEpoch != policyEpoch ||
        reference.formatVersion != formatVersion ||
        reference.digestAlgorithm != digestAlgorithm || reference.contentDigest != digest) {
      throw LegacyRepoImportError("legacy-import-artifact-catalog-mismatch");
    }
    const auto lifecycleKeys = acceptedArtifactLifecycle(
      db, operationId, digest, generation);
    const auto payloadPath = config.sourcePayloadRoot / "payloads" / "sha256" /
      digest.substr(0, 2) / digest;
    const auto payloadStatus = fs::symlink_status(payloadPath);
    if (payloadStatus.type() != fs::file_type::regular) {
      throw LegacyRepoImportError("legacy-import-artifact-payload-missing");
    }
    const auto payload = digestAndSizeFile(payloadPath);
    if (payload.sha256 != digest || payload.size != reference.sizeBytes) {
      throw LegacyRepoImportError("legacy-import-artifact-digest-mismatch");
    }
    ImportAction action;
    action.kind = ImportAction::Kind::Artifact;
    action.item.sourceKind = "python-artifact-catalog";
    action.item.sourceKey = logicalName + ":" + policyEpoch;
    action.item.sourceManifestSha256 = digestString(artifactJson);
    action.item.sourcePayloadSha256 = payload.sha256;
    action.item.nativeKey = logicalName + ":" + policyEpoch;
    action.item.sizeBytes = payload.size;
    action.item.sourceGeneration = generation;
    action.item.nativeGeneration = 1;
    action.item.legacyClaims.push_back({
      "artifact_active_catalog", action.item.sourceKey,
      digestString(dumpJson(canonicalRow(catalog.get())))});
    action.item.legacyClaims.insert(action.item.legacyClaims.end(),
                                   lifecycleKeys.begin(), lifecycleKeys.end());
    action.payloadPath = payloadPath;
    action.artifactReference = std::move(reference);
    plan.actions.push_back(std::move(action));
  }

  if (!tables.empty() && std::find(tables.begin(), tables.end(), "objects") != tables.end()) {
    Statement orphanRefs(db, "SELECT COUNT(*) FROM object_packet_refs AS r "
                            "LEFT JOIN objects AS o ON o.object_name=r.object_name "
                            "WHERE o.object_name IS NULL");
    if (!orphanRefs.step() || sqlite3_column_int64(orphanRefs.get(), 0) != 0) {
      throw LegacyRepoImportError("legacy-import-orphan-packet-reference");
    }
    Statement objects(db, "SELECT rowid,object_name,manifest_json,payload,payload_size,"
                          "sha256,object_type FROM objects ORDER BY object_name");
    while (objects.step()) {
      const auto rowid = sqlite3_column_int64(objects.get(), 0);
      const auto objectName = columnText(objects.get(), 1);
      const auto manifestJson = columnText(objects.get(), 2);
      const bool hasInlinePayload = sqlite3_column_type(objects.get(), 3) != SQLITE_NULL;
      const auto payloadSize = static_cast<uint64_t>(sqlite3_column_int64(objects.get(), 4));
      const auto digest = columnText(objects.get(), 5);
      const auto objectType = columnText(objects.get(), 6);
      RepoObjectManifest sourceManifest;
      try {
        sourceManifest = parseManifestJson(manifestJson);
      }
      catch (const std::exception&) {
        throw LegacyRepoImportError("legacy-import-repo-manifest-invalid");
      }
      if (sourceManifest.objectName != objectName || sourceManifest.objectType != objectType ||
        sourceManifest.sha256 != digest || sourceManifest.size != payloadSize ||
        (!sourceManifest.packetNames.empty() &&
         sourceManifest.segmentCount != sourceManifest.packetNames.size())) {
        throw LegacyRepoImportError("legacy-import-repo-manifest-mismatch");
      }

      Statement refs(db, "SELECT object_name,ordinal,segment_no,data_name "
                         "FROM object_packet_refs "
                         "WHERE object_name=? ORDER BY ordinal");
      refs.bind(1, objectName);
      std::vector<std::string> packetNames;
      std::vector<LegacyImportClaim> packetRefClaims;
      int64_t expectedOrdinal = 0;
      while (refs.step()) {
        const auto refObjectName = columnText(refs.get(), 0);
        const auto ordinal = sqlite3_column_int64(refs.get(), 1);
        const auto segmentNo = sqlite3_column_int64(refs.get(), 2);
        const auto name = columnText(refs.get(), 3);
        if (refObjectName != objectName || ordinal != expectedOrdinal++) {
          throw LegacyRepoImportError("legacy-import-packet-order-invalid");
        }
        if (segmentNo < 0) {
          throw LegacyRepoImportError("legacy-import-packet-segment-invalid");
        }
        packetNames.push_back(name);
        packetRefClaims.push_back({
          "object_packet_refs",
          sourceRowKey(refs.get(), db, "object_packet_refs"),
          digestString(dumpJson(canonicalRow(refs.get()))) });
      }
      if (packetNames != sourceManifest.packetNames) {
        throw LegacyRepoImportError("legacy-import-packet-reference-mismatch");
      }

      const auto objectPath = scratch / ("object-" + std::to_string(serial++));
      fs::path reconstructedPath;
      if (hasInlinePayload) {
        const auto inlinePayload = copyBlobToFile(db, "objects", "payload", rowid, objectPath);
        if (inlinePayload.size != payloadSize || inlinePayload.sha256 != digest) {
          throw LegacyRepoImportError("legacy-import-repo-payload-digest-mismatch");
        }
      }
      else {
        writeEmptyFile(objectPath);
      }
      if (!packetNames.empty()) {
        reconstructedPath = scratch / ("reconstructed-" + std::to_string(serial++));
        writeEmptyFile(reconstructedPath);
        Digest reconstructedDigest;
        uint64_t reconstructedSize = 0;
        for (const auto& name : packetNames) {
          auto packet = plan.packets.find(name);
          if (packet == plan.packets.end()) {
            addPacket(plan, db, "data_packets", name, scratch,
                      config.maxDataWireBytes, serial++);
            packet = plan.packets.find(name);
          }
          if (packet == plan.packets.end()) {
            throw LegacyRepoImportError("legacy-import-data-wire-missing");
          }
          const auto part = scratch / ("part-" + std::to_string(serial++));
          validateDataWire(packet->second.wirePath, name,
                           config.maxDataWireBytes, part);
          appendFile(part, reconstructedPath, &reconstructedDigest, &reconstructedSize);
        }
        const auto reconstructedSha = reconstructedDigest.finish();
        if (reconstructedSize != payloadSize || reconstructedSha != digest) {
          throw LegacyRepoImportError("legacy-import-repo-payload-digest-mismatch");
        }
        if (hasInlinePayload && !sameFiles(objectPath, reconstructedPath)) {
          throw LegacyRepoImportError("legacy-import-repo-payload-packet-conflict");
        }
        if (!hasInlinePayload) {
          fs::copy_file(reconstructedPath, objectPath, fs::copy_options::overwrite_existing);
        }
      }
      else if (!hasInlinePayload && payloadSize != 0) {
        throw LegacyRepoImportError("legacy-import-repo-payload-missing");
      }
      if (digestAndSizeFile(objectPath).sha256 != digest ||
          fs::file_size(objectPath) != payloadSize) {
        throw LegacyRepoImportError("legacy-import-repo-payload-digest-mismatch");
      }

      RepoObjectManifest nativeManifest = sourceManifest;
      // Packet provenance remains in the import item; this object contains verified bytes.
      nativeManifest.packetNames.clear();
      nativeManifest.segmentCount = 1;
      if (nativeManifest.objectType == "ndn-segmented-data") {
        nativeManifest.objectType = "object";
      }
      nativeManifest.replicationFactor = 1;
      nativeManifest.replicaNodes.clear();
      nativeManifest.confirmedReplicaNodes.clear();
      nativeManifest.policyEpoch.clear();
      nativeManifest.publicationIdentity.clear();
      nativeManifest.protectionEpoch.clear();
      nativeManifest.keyReferenceId.clear();
      nativeManifest.keyReferenceVersion.clear();
      nativeManifest.ciphertextManifestDigest.clear();
      nativeManifest.servingLocator.clear();
      nativeManifest.contentDigest.clear();
      nativeManifest.plaintextSize = 0;
      nativeManifest.writeConsistency = "ALL";
      nativeManifest.requiredWriteAcks = 1;
      nativeManifest.operationId.clear();
      nativeManifest.lifecycleState = "COMMITTED";
      nativeManifest.generation = sourceManifest.generation == 0
        ? 1 : sourceManifest.generation;
      nativeManifest.parentGeneration = -1;

      ImportAction action;
      action.kind = ImportAction::Kind::RepoObject;
      action.item.sourceKind = "python-repo-object";
      action.item.sourceKey = objectName;
      action.item.sourceManifestSha256 = digestString(manifestJson);
      action.item.sourcePayloadSha256 = digest;
      action.item.nativeKey = objectName;
      action.item.sizeBytes = payloadSize;
      action.item.sourceGeneration = sourceManifest.generation;
      action.item.nativeGeneration = nativeManifest.generation;
      action.item.packetNames = packetNames;
      for (const auto& name : packetNames) {
        action.item.packetSha256.push_back(plan.packets.at(name).sha256);
      }
      action.item.legacyClaims.push_back({
        "objects", "objects:[" + quoteJson(objectName) + "]",
        digestString(manifestJson)});
      action.item.legacyClaims.insert(action.item.legacyClaims.end(),
        packetRefClaims.begin(), packetRefClaims.end());
      action.payloadPath = objectPath;
      action.repoManifest = std::move(nativeManifest);
      plan.actions.push_back(std::move(action));
    }

    Statement allPackets(db, "SELECT data_name FROM data_packets ORDER BY data_name");
    while (allPackets.step()) {
      addPacket(plan, db, "data_packets", columnText(allPackets.get(), 0),
                scratch, config.maxDataWireBytes, serial++);
    }

    Statement serving(db, "SELECT data_name FROM serving_packets ORDER BY data_name");
    while (serving.step()) {
      const auto name = columnText(serving.get(), 0);
      addPacket(plan, db, "serving_packets", name, scratch,
                config.maxDataWireBytes, serial++);
    }

    for (const auto& entry : plan.packets) {
      const auto& packet = entry.second;
      RepoObjectManifest manifest;
      manifest.objectName = packet.name;
      manifest.objectType = "ndn-data-wire";
      manifest.sha256 = packet.sha256;
      manifest.size = packet.sizeBytes;
      manifest.segmentCount = 1;
      manifest.packetNames = {packet.name};
      manifest.generation = 1;
      manifest.replicationFactor = 1;
      manifest.writeConsistency = "ALL";
      manifest.requiredWriteAcks = 1;
      manifest.lifecycleState = "COMMITTED";
      ImportAction action;
      action.kind = ImportAction::Kind::RepoPacket;
      action.item.sourceKind = "python-repo-packet-wire";
      action.item.sourceKey = packet.name;
      action.item.sourcePayloadSha256 = packet.sha256;
      action.item.nativeKey = packet.name;
      action.item.sizeBytes = packet.sizeBytes;
      action.item.sourceGeneration = 0;
      action.item.nativeGeneration = 1;
      action.item.packetNames = {packet.name};
      action.item.packetSha256 = {packet.sha256};
      action.payloadPath = packet.wirePath;
      action.repoManifest = std::move(manifest);
      plan.actions.push_back(std::move(action));
    }
  }
  manifest.legacyClaims = plan.legacyClaims;
  return plan;
}

JsonValue
claimJson(const LegacyImportClaim& claim)
{
  return JsonValue(JsonValue::Object{
    {"rowSha256", JsonValue(claim.rowSha256)},
    {"sourceKey", JsonValue(claim.sourceKey)},
    {"table", JsonValue(claim.table)}});
}

JsonValue
itemJson(const LegacyImportItem& item)
{
  JsonValue::Array packetNames, packetDigests, claims;
  for (const auto& name : item.packetNames) packetNames.emplace_back(name);
  for (const auto& digest : item.packetSha256) packetDigests.emplace_back(digest);
  for (const auto& claim : item.legacyClaims) claims.push_back(claimJson(claim));
  return JsonValue(JsonValue::Object{
    {"legacyClaims", JsonValue(std::move(claims))},
    {"nativeGeneration", JsonValue(item.nativeGeneration)},
    {"nativeKey", JsonValue(item.nativeKey)},
    {"packetNames", JsonValue(std::move(packetNames))},
    {"packetSha256", JsonValue(std::move(packetDigests))},
    {"sizeBytes", JsonValue(item.sizeBytes)},
    {"sourceGeneration", JsonValue(item.sourceGeneration)},
    {"sourceKey", JsonValue(item.sourceKey)},
    {"sourceKind", JsonValue(item.sourceKind)},
    {"sourceManifestSha256", JsonValue(item.sourceManifestSha256)},
    {"sourcePayloadSha256", JsonValue(item.sourcePayloadSha256)},
    {"state", JsonValue(item.state)}});
}

JsonValue
manifestJsonValue(const LegacyImportManifest& manifest)
{
  JsonValue::Array items, claims;
  for (const auto& item : manifest.items) items.push_back(itemJson(item));
  for (const auto& claim : manifest.legacyClaims) claims.push_back(claimJson(claim));
  return JsonValue(JsonValue::Object{
    {"byteCount", JsonValue(manifest.byteCount)},
    {"completedAt", JsonValue(manifest.completedAt)},
    {"destinationId", JsonValue(manifest.destinationId)},
    {"destinationInventoryDigest", JsonValue(manifest.destinationInventoryDigest)},
    {"failureCode", JsonValue(manifest.failureCode)},
    {"formatVersion", JsonValue(manifest.formatVersion)},
    {"importId", JsonValue(manifest.importId)},
    {"itemCount", JsonValue(manifest.itemCount)},
    {"items", JsonValue(std::move(items))},
    {"legacyClaims", JsonValue(std::move(claims))},
    {"sourcePayloadInventoryDigest", JsonValue(manifest.sourcePayloadInventoryDigest)},
    {"sourceSchemaFingerprint", JsonValue(manifest.sourceSchemaFingerprint)},
    {"sourceSchemaId", JsonValue(manifest.sourceSchemaId)},
    {"sourceSnapshotDigest", JsonValue(manifest.sourceSnapshotDigest)},
    {"startedAt", JsonValue(manifest.startedAt)},
    {"state", JsonValue(manifest.state)}});
}

LegacyImportClaim
parseClaim(const ptree& tree)
{
  return {tree.get<std::string>("table"), tree.get<std::string>("sourceKey"),
          tree.get<std::string>("rowSha256")};
}

std::vector<std::string>
parseStringArray(const ptree& tree)
{
  std::vector<std::string> values;
  for (const auto& entry : tree) values.push_back(entry.second.get_value<std::string>());
  return values;
}

LegacyImportItem
parseItem(const ptree& tree)
{
  LegacyImportItem item;
  item.sourceKind = tree.get<std::string>("sourceKind");
  item.sourceKey = tree.get<std::string>("sourceKey");
  item.sourceManifestSha256 = tree.get<std::string>("sourceManifestSha256");
  item.sourcePayloadSha256 = tree.get<std::string>("sourcePayloadSha256");
  item.nativeKey = tree.get<std::string>("nativeKey");
  item.sizeBytes = tree.get<uint64_t>("sizeBytes");
  item.sourceGeneration = tree.get<uint64_t>("sourceGeneration");
  item.nativeGeneration = tree.get<uint64_t>("nativeGeneration");
  item.packetNames = parseStringArray(tree.get_child("packetNames"));
  item.packetSha256 = parseStringArray(tree.get_child("packetSha256"));
  item.state = tree.get<std::string>("state");
  for (const auto& entry : tree.get_child("legacyClaims")) {
    item.legacyClaims.push_back(parseClaim(entry.second));
  }
  return item;
}

LegacyImportManifest
parseManifest(const std::string& json)
{
  std::stringstream input(json);
  ptree root;
  try {
    boost::property_tree::read_json(input, root);
    LegacyImportManifest manifest;
    manifest.formatVersion = root.get<uint32_t>("formatVersion");
    manifest.importId = root.get<std::string>("importId");
    manifest.sourceSchemaId = root.get<std::string>("sourceSchemaId");
    manifest.sourceSchemaFingerprint = root.get<std::string>("sourceSchemaFingerprint");
    manifest.sourceSnapshotDigest = root.get<std::string>("sourceSnapshotDigest");
    manifest.sourcePayloadInventoryDigest = root.get<std::string>("sourcePayloadInventoryDigest");
    manifest.destinationId = root.get<std::string>("destinationId");
    manifest.destinationInventoryDigest = root.get<std::string>("destinationInventoryDigest");
    manifest.state = root.get<std::string>("state");
    manifest.itemCount = root.get<uint64_t>("itemCount");
    manifest.byteCount = root.get<uint64_t>("byteCount");
    manifest.failureCode = root.get<std::string>("failureCode");
    manifest.startedAt = root.get<std::string>("startedAt");
    manifest.completedAt = root.get<std::string>("completedAt");
    for (const auto& entry : root.get_child("items")) {
      manifest.items.push_back(parseItem(entry.second));
    }
    for (const auto& entry : root.get_child("legacyClaims")) {
      manifest.legacyClaims.push_back(parseClaim(entry.second));
    }
    return manifest;
  }
  catch (const std::exception&) {
    throw LegacyRepoImportError("legacy-import-manifest-invalid");
  }
}

struct DestinationInventory
{
  uint64_t itemCount = 0;
  uint64_t byteCount = 0;
  std::string digest;
};

DestinationInventory
makeDestinationInventory(const std::vector<ImportAction>& actions,
                         const std::map<std::string, RepoObjectManifest>& repoObjects)
{
  JsonValue::Array values;
  uint64_t itemCount = 0;
  uint64_t byteCount = 0;
  std::set<std::string> artifactKeys;
  for (const auto& action : actions) {
    if (action.kind != ImportAction::Kind::Artifact) continue;
    const auto key = action.artifactReference.logicalName + "\n" +
                     action.artifactReference.policyEpoch;
    if (!artifactKeys.insert(key).second) continue;
    ++itemCount;
    byteCount += action.artifactReference.sizeBytes;
    values.emplace_back(JsonValue::Object{
      {"kind", JsonValue("artifact")},
      {"reference", JsonValue(action.artifactReference.toJson())},
      {"sha256", JsonValue(action.artifactReference.contentDigest)},
      {"size", JsonValue(action.artifactReference.sizeBytes)}});
  }
  for (const auto& entry : repoObjects) {
    ++itemCount;
    byteCount += entry.second.size;
    values.emplace_back(JsonValue::Object{
      {"kind", JsonValue("repo-object")},
      {"manifest", JsonValue(entry.second.toJson())},
      {"name", JsonValue(entry.first)},
      {"sha256", JsonValue(entry.second.sha256)},
      {"size", JsonValue(entry.second.size)}});
  }
  std::sort(values.begin(), values.end(), [](const JsonValue& a, const JsonValue& b) {
    return dumpJson(a) < dumpJson(b);
  });
  return {itemCount, byteCount,
          digestString(dumpJson(JsonValue(std::move(values))))};
}

bool
sameItemIdentity(const LegacyImportItem& left, const LegacyImportItem& right)
{
  auto a = left;
  auto b = right;
  a.state.clear();
  b.state.clear();
  return dumpJson(itemJson(a)) == dumpJson(itemJson(b));
}

void
validateManifestIdentity(const LegacyImportManifest& existing,
                         const LegacyImportManifest& expected)
{
  if (existing.formatVersion != expected.formatVersion ||
      existing.importId != expected.importId ||
      existing.sourceSchemaId != expected.sourceSchemaId ||
      existing.sourceSchemaFingerprint != expected.sourceSchemaFingerprint ||
      existing.sourceSnapshotDigest != expected.sourceSnapshotDigest ||
      existing.sourcePayloadInventoryDigest != expected.sourcePayloadInventoryDigest ||
      existing.destinationId != expected.destinationId ||
      existing.destinationInventoryDigest != expected.destinationInventoryDigest ||
      existing.itemCount != expected.itemCount ||
      existing.byteCount != expected.byteCount ||
      existing.items.size() != expected.items.size()) {
    throw LegacyRepoImportError("legacy-import-existing-state-conflict");
  }
  for (size_t i = 0; i < expected.items.size(); ++i) {
    if (!sameItemIdentity(existing.items[i], expected.items[i])) {
      throw LegacyRepoImportError("legacy-import-existing-item-conflict");
    }
  }
  if (existing.state != "PREPARING" && existing.state != "INCOMPLETE" &&
      existing.state != "COMMITTED") {
    throw LegacyRepoImportError("legacy-import-existing-state-invalid");
  }
}

void
streamToRepoStore(FilesystemRepoStoreBackend& store,
                  const RepoObjectManifest& manifest,
                  const fs::path& payload, uint64_t maxRangeBytes)
{
  const auto size = static_cast<uint64_t>(fs::file_size(payload));
  if (size != manifest.size || digestFile(payload) != manifest.sha256) {
    throw LegacyRepoImportError("legacy-import-staged-payload-mismatch");
  }
  if (size == 0) {
    store.put(manifest, {});
    return;
  }
  std::ifstream input(payload, std::ios::binary);
  if (!input) throw LegacyRepoImportError("legacy-import-staged-payload-read-failed");
  uint64_t offset = 0;
  while (offset < size) {
    const auto chunkSize = static_cast<size_t>(
      std::min<uint64_t>(maxRangeBytes, size - offset));
    std::vector<uint8_t> bytes(chunkSize);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
      throw LegacyRepoImportError("legacy-import-staged-payload-read-failed");
    }
    store.putRange(manifest, RepoByteRange{offset, chunkSize}, bytes);
    offset += chunkSize;
  }
  store.commitRanges(manifest);
}

void
verifyRepoPayload(FilesystemRepoStoreBackend& store,
                  const RepoObjectManifest& manifest,
                  const fs::path& expectedPath, uint64_t maxRangeBytes)
{
  const auto durable = store.getManifest(manifest.objectName);
  if (durable.toJson() != manifest.toJson()) {
    throw LegacyRepoImportError("legacy-import-native-manifest-mismatch");
  }
  const auto size = static_cast<uint64_t>(fs::file_size(expectedPath));
  if (size != manifest.size) {
    throw LegacyRepoImportError("legacy-import-native-payload-size-mismatch");
  }
  std::ifstream expected(expectedPath, std::ios::binary);
  if (!expected) throw LegacyRepoImportError("legacy-import-staged-payload-read-failed");
  uint64_t offset = 0;
  while (offset < size) {
    const auto count = static_cast<size_t>(std::min<uint64_t>(maxRangeBytes, size - offset));
    const auto actual = store.getRange(manifest.objectName, RepoByteRange{offset, count});
    std::vector<uint8_t> bytes(count);
    if (!expected.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size())) || actual != bytes) {
      throw LegacyRepoImportError("legacy-import-native-payload-mismatch");
    }
    offset += count;
  }
  if (digestFile(expectedPath) != manifest.sha256) {
    throw LegacyRepoImportError("legacy-import-native-payload-digest-mismatch");
  }
}

struct ImportLock
{
  explicit ImportLock(const fs::path& path)
    : path(path)
  {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                S_IRUSR | S_IWUSR);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      if (fd >= 0) ::close(fd);
      fd = -1;
      throw LegacyRepoImportError("legacy-import-already-running");
    }
  }
  ~ImportLock() { if (fd >= 0) ::close(fd); }
  void unlinkAfterPublish() noexcept
  {
    if (path.empty()) return;
    ::unlink(path.c_str());
    path.clear();
  }
  fs::path path;
  int fd = -1;
};

void fsyncDirectory(const fs::path& path);

void
publishNoReplace(const fs::path& staging, const fs::path& destination)
{
  const long result = ::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(),
                                AT_FDCWD, destination.c_str(), RENAME_NOREPLACE);
  if (result != 0) {
    if (errno == EEXIST) throw LegacyRepoImportError("legacy-import-destination-conflict");
    throw LegacyRepoImportError("legacy-import-atomic-publish-failed");
  }
  fsyncDirectory(destination.parent_path());
}


std::string
readWholeFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw LegacyRepoImportError("legacy-import-manifest-read-failed");
  std::ostringstream output;
  output << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw LegacyRepoImportError("legacy-import-manifest-read-failed");
  }
  return output.str();
}

void
fsyncDirectory(const fs::path& path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw LegacyRepoImportError("legacy-import-fsync-directory-failed");
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) throw LegacyRepoImportError("legacy-import-fsync-directory-failed");
}

void
writeManifest(const fs::path& path, const LegacyImportManifest& manifest)
{
  const auto temporary = path.string() + ".tmp";
  const auto json = dumpJson(manifestJsonValue(manifest));
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
  if (fd < 0) throw LegacyRepoImportError("legacy-import-manifest-write-failed");
  size_t offset = 0;
  while (offset < json.size()) {
    const auto written = ::write(fd, json.data() + offset, json.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(fd);
      throw LegacyRepoImportError("legacy-import-manifest-write-failed");
    }
    offset += static_cast<size_t>(written);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw LegacyRepoImportError("legacy-import-manifest-fsync-failed");
  }
  ::close(fd);
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    throw LegacyRepoImportError("legacy-import-manifest-publish-failed");
  }
  fsyncDirectory(path.parent_path());
}

std::map<std::string, const ImportAction*>
uniqueRepoActions(const std::vector<ImportAction>& actions)
{
  std::map<std::string, const ImportAction*> result;
  for (const auto& action : actions) {
    if (action.kind == ImportAction::Kind::Artifact) continue;
    const auto inserted = result.emplace(action.repoManifest.objectName, &action);
    if (!inserted.second) {
      const auto& prior = *inserted.first->second;
      if (prior.repoManifest.toJson() != action.repoManifest.toJson() ||
          prior.item.sourcePayloadSha256 != action.item.sourcePayloadSha256 ||
          !sameFiles(prior.payloadPath, action.payloadPath)) {
        throw LegacyRepoImportError("legacy-import-native-name-conflict");
      }
    }
  }
  return result;
}

std::map<std::string, RepoObjectManifest>
repoManifests(const std::map<std::string, const ImportAction*>& actions)
{
  std::map<std::string, RepoObjectManifest> result;
  for (const auto& entry : actions) {
    result.emplace(entry.first, entry.second->repoManifest);
  }
  return result;
}

void
verifyExistingDestination(const fs::path& root,
                          const LegacyRepoImportConfig& config,
                          const std::vector<ImportAction>& actions,
                          const std::map<std::string, const ImportAction*>& repoActions,
                          const DestinationInventory& inventory,
                          bool hasRepoTables,
                          const fs::path& scratch,
                          const LegacyImportManifest& expected)
{
  const auto manifestPath = root / "LegacyImportManifest.json";
  const auto existing = parseManifest(readWholeFile(manifestPath));
  validateManifestIdentity(existing, expected);
  const bool allItemsCommitted = std::all_of(
    existing.items.begin(), existing.items.end(), [](const auto& item) {
      return item.state == "COMMITTED";
    });
  if (existing.state != "COMMITTED" || !allItemsCommitted ||
      !existing.failureCode.empty() || existing.completedAt.empty() ||
      existing.destinationInventoryDigest != inventory.digest ||
      existing.itemCount != inventory.itemCount ||
      existing.byteCount != inventory.byteCount) {
    throw LegacyRepoImportError("legacy-import-destination-inventory-conflict");
  }

  const bool hasArtifacts = std::any_of(actions.begin(), actions.end(), [](const auto& action) {
    return action.kind == ImportAction::Kind::Artifact;
  });
  if (hasArtifacts) {
    InAppRepoConfig appConfig;
    appConfig.durableRoot = root / "artifacts";
    appConfig.ownerId = "legacy-import-" + config.importId;
    auto api = ArtifactRepositoryApi::makeInApp(std::move(appConfig));
    size_t ordinal = 0;
    for (const auto& action : actions) {
      if (action.kind != ImportAction::Kind::Artifact) continue;
      const auto verifyPath = scratch / ("verify-artifact-" + std::to_string(ordinal++));
      api.fetchFile(action.artifactReference, verifyPath.string());
      const auto observed = digestAndSizeFile(verifyPath);
      if (observed.sha256 != action.item.sourcePayloadSha256 ||
          observed.size != action.item.sizeBytes) {
        throw LegacyRepoImportError("legacy-import-destination-artifact-mismatch");
      }
    }
  }
  if (hasRepoTables) {
    FilesystemRepoStoreBackend store((root / "repo-core").string(),
                                     config.maxRangeBytes,
                                     std::min<uint64_t>(config.maxRangeBytes,
                                                        1024 * 1024),
                                     "legacy-import-" + config.importId);
    if (store.size() != repoActions.size()) {
      throw LegacyRepoImportError("legacy-import-destination-repo-count-mismatch");
    }
    for (const auto& entry : repoActions) {
      verifyRepoPayload(store, entry.second->repoManifest,
                        entry.second->payloadPath, config.maxRangeBytes);
    }
  }
}

void
verifySourceUnchanged(const fs::path& sourceDatabase,
                      const std::vector<FileIdentity>& beforeDatabase,
                      const std::string& beforePayloadDigest,
                      const fs::path& payloadRoot)
{
  if (bundleDigest(databaseBundle(sourceDatabase)) != bundleDigest(beforeDatabase)) {
    throw LegacyRepoImportError("legacy-import-source-database-mutated");
  }
  if (payloadTreeDigest(payloadRoot) != beforePayloadDigest) {
    throw LegacyRepoImportError("legacy-import-source-payload-mutated");
  }
}

bool
pathIsWithin(const fs::path& parent, const fs::path& candidate)
{
  const auto a = parent.lexically_normal();
  const auto b = candidate.lexically_normal();
  auto ai = a.begin();
  auto bi = b.begin();
  for (; ai != a.end(); ++ai, ++bi) {
    if (bi == b.end() || *ai != *bi) return false;
  }
  return true;
}

} // namespace

std::string
LegacyImportManifest::toJson() const
{
  return dumpJson(manifestJsonValue(*this));
}

LegacyImportManifest
LegacyRepoImporter::import(const LegacyRepoImportConfig& config)
{
  validateImportId(config.importId);
  if (config.maxRangeBytes == 0 || config.maxRangeBytes > MAX_IMPORT_RANGE_BYTES ||
      config.maxDataWireBytes == 0 ||
      config.maxDataWireBytes > MAX_IMPORT_DATA_WIRE_BYTES ||
      config.sourceDatabase.empty() || config.sourcePayloadRoot.empty()) {
    throw LegacyRepoImportError("legacy-import-config-invalid");
  }
  const fs::path sourceDatabase = fs::absolute(config.sourceDatabase).lexically_normal();
  const fs::path payloadRoot = fs::absolute(config.sourcePayloadRoot).lexically_normal();
  const fs::path destination(canonicalDestination(config.destinationRoot));
  if (sourceDatabase == destination || payloadRoot == destination ||
      pathIsWithin(payloadRoot, destination) || pathIsWithin(destination, payloadRoot)) {
    throw LegacyRepoImportError("legacy-import-path-overlap");
  }
  const auto parentStatus = fs::symlink_status(destination.parent_path());
  if (parentStatus.type() != fs::file_type::directory) {
    throw LegacyRepoImportError("legacy-import-destination-parent-invalid");
  }

  TemporaryDirectory scratchOwner;
  const auto scratch = scratchOwner.path();
  const auto sourceBefore = databaseBundle(sourceDatabase);
  const auto payloadBefore = payloadTreeDigest(payloadRoot);
  const auto snapshot = copyReadOnlySnapshot(sourceDatabase, sourceBefore, scratch);
  if (bundleDigest(databaseBundle(snapshot)) != bundleDigest(sourceBefore)) {
    throw LegacyRepoImportError("legacy-import-source-snapshot-mismatch");
  }

  Database db(snapshot);
  Statement integrity(db.get(), "PRAGMA integrity_check");
  if (!integrity.step() || columnText(integrity.get(), 0) != "ok") {
    throw LegacyRepoImportError("legacy-import-source-integrity-failed");
  }
  const auto tables = getTableNames(db.get());
  std::string fingerprint;
  bool hasRepoTables = false;
  const auto profileId = schemaProfileId(db.get(), tables, fingerprint, hasRepoTables);

  LegacyImportManifest manifest;
  manifest.importId = config.importId;
  manifest.sourceSchemaId = profileId;
  manifest.sourceSchemaFingerprint = fingerprint;
  manifest.sourceSnapshotDigest = bundleDigest(sourceBefore);
  manifest.sourcePayloadInventoryDigest = payloadBefore;
  manifest.destinationId = "ndnsf-native-store:" +
    digestString(destination.string() + "\n" + config.importId);
  manifest.startedAt = isoTimestamp();

  auto plan = buildImportPlan(db.get(), tables, config, scratch, manifest);
  manifest.items.reserve(plan.actions.size());
  for (const auto& action : plan.actions) manifest.items.push_back(action.item);
  const auto repoActions = uniqueRepoActions(plan.actions);
  const auto repoManifestMap = repoManifests(repoActions);
  const auto inventory = makeDestinationInventory(plan.actions, repoManifestMap);
  manifest.itemCount = inventory.itemCount;
  manifest.byteCount = inventory.byteCount;
  manifest.destinationInventoryDigest = inventory.digest;
  manifest.legacyClaims = plan.legacyClaims;
  verifySourceUnchanged(sourceDatabase, sourceBefore, payloadBefore, payloadRoot);

  const auto staging = destination.parent_path() /
    ("." + destination.filename().string() + ".ndnsf-import-" +
     config.importId + ".staging");
  std::error_code error;
  const auto destinationStatus = fs::symlink_status(destination, error);
  const bool destinationExists = !error && destinationStatus.type() != fs::file_type::not_found;
  if (error && error != std::errc::no_such_file_or_directory) {
    throw LegacyRepoImportError("legacy-import-destination-inspect-failed");
  }
  const auto stagingStatus = fs::symlink_status(staging, error);
  const bool stagingExists = !error && stagingStatus.type() != fs::file_type::not_found;
  if (error && error != std::errc::no_such_file_or_directory) {
    throw LegacyRepoImportError("legacy-import-staging-inspect-failed");
  }
  if (destinationExists && stagingExists) {
    throw LegacyRepoImportError("legacy-import-destination-staging-conflict");
  }
  if (destinationExists) {
    if (destinationStatus.type() != fs::file_type::directory) {
      throw LegacyRepoImportError("legacy-import-destination-conflict");
    }
    verifyExistingDestination(destination, config, plan.actions, repoActions,
                              inventory, hasRepoTables, scratch, manifest);
    verifySourceUnchanged(sourceDatabase, sourceBefore, payloadBefore, payloadRoot);
    fsyncDirectory(destination.parent_path());
    return parseManifest(readWholeFile(destination / "LegacyImportManifest.json"));
  }

  ImportLock lock(staging.string() + ".lock");
  const auto lockedDestinationStatus = fs::symlink_status(destination, error);
  const bool lockedDestinationExists = !error &&
    lockedDestinationStatus.type() != fs::file_type::not_found;
  if (error && error != std::errc::no_such_file_or_directory) {
    throw LegacyRepoImportError("legacy-import-destination-inspect-failed");
  }
  if (lockedDestinationExists) {
    if (lockedDestinationStatus.type() != fs::file_type::directory) {
      throw LegacyRepoImportError("legacy-import-destination-conflict");
    }
    const auto lockedStageStatus = fs::symlink_status(staging, error);
    const bool lockedStageExists = !error &&
      lockedStageStatus.type() != fs::file_type::not_found;
    if (error && error != std::errc::no_such_file_or_directory) {
      throw LegacyRepoImportError("legacy-import-staging-inspect-failed");
    }
    if (lockedStageExists) {
      throw LegacyRepoImportError("legacy-import-destination-staging-conflict");
    }
    verifyExistingDestination(destination, config, plan.actions, repoActions,
                              inventory, hasRepoTables, scratch, manifest);
    verifySourceUnchanged(sourceDatabase, sourceBefore, payloadBefore, payloadRoot);
    fsyncDirectory(destination.parent_path());
    return parseManifest(readWholeFile(destination / "LegacyImportManifest.json"));
  }

  const auto currentStageStatus = fs::symlink_status(staging, error);
  const bool currentStageExists = !error &&
    currentStageStatus.type() != fs::file_type::not_found;
  if (error && error != std::errc::no_such_file_or_directory) {
    throw LegacyRepoImportError("legacy-import-staging-inspect-failed");
  }
  if (!currentStageExists) {
    if (::mkdir(staging.c_str(), S_IRWXU) != 0) {
      throw LegacyRepoImportError("legacy-import-staging-create-failed");
    }
  }
  else if (currentStageStatus.type() != fs::file_type::directory) {
    throw LegacyRepoImportError("legacy-import-staging-not-directory");
  }

  const auto stagedManifestPath = staging / "LegacyImportManifest.json";
  if (fs::exists(stagedManifestPath)) {
    const auto existing = parseManifest(readWholeFile(stagedManifestPath));
    validateManifestIdentity(existing, manifest);
    manifest.startedAt = existing.startedAt;
    manifest.state = existing.state;
    manifest.completedAt = existing.completedAt;
    for (size_t i = 0; i < manifest.items.size(); ++i) {
      if (existing.items[i].state != "PREPARING" &&
          existing.items[i].state != "COMMITTED") {
        throw LegacyRepoImportError("legacy-import-existing-item-state-invalid");
      }
      manifest.items[i].state = existing.items[i].state;
    }
  }
  else {
    if (!fs::is_empty(staging)) {
      throw LegacyRepoImportError("legacy-import-staging-manifest-missing");
    }
    writeManifest(stagedManifestPath, manifest);
  }

  try {
    const bool hasArtifacts = std::any_of(plan.actions.begin(), plan.actions.end(),
      [](const auto& action) { return action.kind == ImportAction::Kind::Artifact; });
    {
      std::unique_ptr<ArtifactRepositoryApi> inApp;
      if (hasArtifacts) {
        InAppRepoConfig appConfig;
        appConfig.durableRoot = staging / "artifacts";
        appConfig.ownerId = "legacy-import-" + config.importId;
        inApp = std::make_unique<ArtifactRepositoryApi>(
          ArtifactRepositoryApi::makeInApp(std::move(appConfig)));
      }
      std::unique_ptr<FilesystemRepoStoreBackend> repoStore;
      if (hasRepoTables) {
        repoStore = std::make_unique<FilesystemRepoStoreBackend>(
          (staging / "repo-core").string(), config.maxRangeBytes,
          std::min<uint64_t>(config.maxRangeBytes, 1024 * 1024),
          "legacy-import-" + config.importId);
      }

      for (size_t i = 0; i < plan.actions.size(); ++i) {
        const auto& action = plan.actions[i];
        if (action.kind == ImportAction::Kind::Artifact) {
          if (!inApp) throw LegacyRepoImportError("legacy-import-artifact-owner-missing");
          inApp->publishFile(action.payloadPath.string(), action.artifactReference);
          const auto verifyPath = scratch / ("verify-staged-artifact-" +
                                              std::to_string(i));
          inApp->fetchFile(action.artifactReference, verifyPath.string());
          const auto observed = digestAndSizeFile(verifyPath);
          if (observed.sha256 != action.item.sourcePayloadSha256 ||
              observed.size != action.item.sizeBytes) {
            throw LegacyRepoImportError("legacy-import-staged-artifact-mismatch");
          }
        }
        else {
          if (!repoStore) throw LegacyRepoImportError("legacy-import-repo-owner-missing");
          streamToRepoStore(*repoStore, action.repoManifest, action.payloadPath,
                            config.maxRangeBytes);
          verifyRepoPayload(*repoStore, action.repoManifest, action.payloadPath,
                            config.maxRangeBytes);
        }
        manifest.items[i].state = "COMMITTED";
        manifest.state = "PREPARING";
        manifest.failureCode.clear();
        writeManifest(stagedManifestPath, manifest);
        if (config.onItemCommitted) {
          config.onItemCommitted(i, manifest.items[i]);
        }
      }

      if (repoStore) {
        if (repoStore->size() != repoActions.size()) {
          throw LegacyRepoImportError("legacy-import-native-repo-count-mismatch");
        }
        for (const auto& entry : repoActions) {
          verifyRepoPayload(*repoStore, entry.second->repoManifest,
                            entry.second->payloadPath, config.maxRangeBytes);
        }
      }
    }

    verifySourceUnchanged(sourceDatabase, sourceBefore, payloadBefore, payloadRoot);
    manifest.state = "COMMITTED";
    manifest.failureCode.clear();
    manifest.completedAt = isoTimestamp();
    writeManifest(stagedManifestPath, manifest);
    publishNoReplace(staging, destination);
    lock.unlinkAfterPublish();
    return manifest;
  }
  catch (const std::exception& errorValue) {
    manifest.state = "INCOMPLETE";
    const std::string message = errorValue.what();
    const auto separator = message.find(':');
    manifest.failureCode = message.substr(0, separator);
    try {
      if (fs::exists(staging)) writeManifest(stagedManifestPath, manifest);
    }
    catch (...) {
    }
    throw;
  }
  catch (...) {
    manifest.state = "INCOMPLETE";
    manifest.failureCode = "legacy-import-unhandled-exception";
    try {
      if (fs::exists(staging)) writeManifest(stagedManifestPath, manifest);
    }
    catch (...) {
    }
    throw;
  }
}

} // namespace ndnsf_distributed_repo
