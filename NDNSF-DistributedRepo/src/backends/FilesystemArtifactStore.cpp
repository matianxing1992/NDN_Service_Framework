#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf_distributed_repo {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t MAX_VERIFIED_RANGES = 1U << 20;
constexpr std::array<uint8_t, 4> RANGE_MAGIC{{'R', 'N', 'G', '2'}};

[[noreturn]] void
throwSystem(const std::string& code, const std::string& action,
            const std::string& path)
{
  throw std::runtime_error(
    code + ": " + action + " " + path + ": " + std::strerror(errno));
}

void
validateGeneration(uint64_t generation)
{
  if (generation == 0) {
    throw std::invalid_argument(
      "repo-artifact-invalid-generation: generation must be positive");
  }
}

void
validateRange(const ArtifactReference& artifact, ArtifactByteRange range,
              size_t suppliedBytes, bool requireSuppliedLength,
              uint64_t maxRangeBytes)
{
  if (range.offsetBytes > artifact.sizeBytes ||
      range.lengthBytes > artifact.sizeBytes - range.offsetBytes) {
    throw std::out_of_range(
      "repo-artifact-range-out-of-bounds: range exceeds artifact size");
  }
  if (requireSuppliedLength && range.lengthBytes != suppliedBytes) {
    throw std::invalid_argument(
      "repo-artifact-range-length-mismatch: byte count differs from range length");
  }
  if (range.lengthBytes > maxRangeBytes) {
    throw std::invalid_argument(
      "repo-artifact-range-too-large: range exceeds bounded I/O limit");
  }
  if (range.lengthBytes > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())) {
    throw std::invalid_argument(
      "repo-artifact-range-too-large: one operation exceeds platform I/O limit");
  }
}

std::string
hexDigest(const std::vector<uint8_t>& value)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : value) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

std::string
sha256File(const std::string& path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-read-failed", "open", path);
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    ::close(fd);
    throw std::runtime_error(
      "repo-artifact-digest-failed: cannot initialize SHA-256");
  }
  std::array<uint8_t, 256 * 1024> buffer{};
  for (;;) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      const int saved = errno;
      ::close(fd);
      errno = saved;
      throwSystem("repo-artifact-read-failed", "read", path);
    }
    if (count == 0) {
      break;
    }
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<size_t>(count)) != 1) {
      ::close(fd);
      throw std::runtime_error(
        "repo-artifact-digest-failed: SHA-256 update failed");
    }
  }
  if (::close(fd) != 0) {
    throwSystem("repo-artifact-read-failed", "close", path);
  }
  std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
  unsigned digestSize = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1) {
    throw std::runtime_error(
      "repo-artifact-digest-failed: SHA-256 finalization failed");
  }
  digest.resize(digestSize);
  return hexDigest(digest);
}

void
writeAll(int fd, const uint8_t* bytes, size_t size, const std::string& path)
{
  size_t written = 0;
  while (written < size) {
    const auto count = ::write(fd, bytes + written, size - written);
    if (count < 0) {
      throwSystem("repo-artifact-write-failed", "write", path);
    }
    written += static_cast<size_t>(count);
  }
}

void
pwriteAll(int fd, const uint8_t* bytes, size_t size, uint64_t offset,
          const std::string& path)
{
  size_t written = 0;
  while (written < size) {
    const auto count = ::pwrite(
      fd, bytes + written, size - written,
      static_cast<off_t>(offset + written));
    if (count < 0) {
      throwSystem("repo-artifact-write-failed", "pwrite", path);
    }
    written += static_cast<size_t>(count);
  }
}

void
preadAll(int fd, uint8_t* bytes, size_t size, uint64_t offset,
         const std::string& path)
{
  size_t readBytes = 0;
  while (readBytes < size) {
    const auto count = ::pread(
      fd, bytes + readBytes, size - readBytes,
      static_cast<off_t>(offset + readBytes));
    if (count < 0) {
      throwSystem("repo-artifact-read-failed", "pread", path);
    }
    if (count == 0) {
      throw std::runtime_error(
        "repo-artifact-truncated: staged payload is shorter than declared");
    }
    readBytes += static_cast<size_t>(count);
  }
}

void
fsyncDirectory(const fs::path& directory)
{
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-sync-failed", "open directory", directory.string());
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-sync-failed", "fsync directory", directory.string());
  }
  ::close(fd);
}

void
putU64(std::vector<uint8_t>& output, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

uint64_t
getU64(const std::vector<uint8_t>& input, size_t& cursor)
{
  if (cursor > input.size() || input.size() - cursor < 8) {
    throw std::runtime_error(
      "repo-artifact-range-map-invalid: truncated integer");
  }
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) {
    value = (value << 8) | input[cursor++];
  }
  return value;
}

std::vector<uint8_t>
readSmallFile(const std::string& path)
{
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return {};
    }
    throw std::runtime_error(
      "repo-artifact-range-map-read-failed: " + path + ": " + error.message());
  }
  const uint64_t maximum = 4 + 8 + MAX_VERIFIED_RANGES * 16;
  if (size > maximum) {
    throw std::runtime_error(
      "repo-artifact-range-map-invalid: range map exceeds parser bound");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-range-map-read-failed", "open", path);
  }
  if (!bytes.empty()) {
    preadAll(fd, bytes.data(), bytes.size(), 0, path);
  }
  ::close(fd);
  return bytes;
}

std::vector<ArtifactByteRange>
decodeRanges(const std::string& path, uint64_t artifactSize)
{
  const auto bytes = readSmallFile(path);
  if (bytes.empty()) {
    return {};
  }
  if (bytes.size() < 12 ||
      !std::equal(RANGE_MAGIC.begin(), RANGE_MAGIC.end(), bytes.begin())) {
    throw std::runtime_error(
      "repo-artifact-range-map-invalid: bad range map header");
  }
  size_t cursor = 4;
  const auto count = getU64(bytes, cursor);
  if (count > MAX_VERIFIED_RANGES ||
      count > (bytes.size() - cursor) / 16 ||
      bytes.size() != 12 + count * 16) {
    throw std::runtime_error(
      "repo-artifact-range-map-invalid: invalid range count");
  }
  std::vector<ArtifactByteRange> ranges;
  ranges.reserve(static_cast<size_t>(count));
  uint64_t priorEnd = 0;
  for (uint64_t index = 0; index < count; ++index) {
    ArtifactByteRange range{getU64(bytes, cursor), getU64(bytes, cursor)};
    if (range.lengthBytes == 0 || range.offsetBytes > artifactSize ||
        range.lengthBytes > artifactSize - range.offsetBytes ||
        (!ranges.empty() && range.offsetBytes <= priorEnd)) {
      throw std::runtime_error(
        "repo-artifact-range-map-invalid: ranges are not canonical");
    }
    priorEnd = range.offsetBytes + range.lengthBytes;
    ranges.push_back(range);
  }
  return ranges;
}

std::vector<ArtifactByteRange>
mergeRange(std::vector<ArtifactByteRange> ranges, ArtifactByteRange added)
{
  if (added.lengthBytes == 0) {
    return ranges;
  }
  ranges.push_back(added);
  std::sort(ranges.begin(), ranges.end(), [] (const auto& left, const auto& right) {
    return left.offsetBytes < right.offsetBytes;
  });
  std::vector<ArtifactByteRange> merged;
  for (const auto& range : ranges) {
    if (merged.empty()) {
      merged.push_back(range);
      continue;
    }
    auto& prior = merged.back();
    const uint64_t priorEnd = prior.offsetBytes + prior.lengthBytes;
    const uint64_t rangeEnd = range.offsetBytes + range.lengthBytes;
    if (range.offsetBytes <= priorEnd) {
      prior.lengthBytes = std::max(priorEnd, rangeEnd) - prior.offsetBytes;
    }
    else {
      merged.push_back(range);
    }
  }
  return merged;
}

void
atomicWrite(const std::string& path, const std::vector<uint8_t>& bytes)
{
  const std::string temporary = path + ".tmp";
  int fd = ::open(temporary.c_str(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    throwSystem("repo-artifact-metadata-write-failed", "open", temporary);
  }
  try {
    if (!bytes.empty()) {
      writeAll(fd, bytes.data(), bytes.size(), temporary);
    }
    if (::fsync(fd) != 0) {
      throwSystem("repo-artifact-metadata-write-failed", "fsync", temporary);
    }
    if (::close(fd) != 0) {
      fd = -1;
      throwSystem("repo-artifact-metadata-write-failed", "close", temporary);
    }
    fd = -1;
  }
  catch (...) {
    if (fd >= 0) {
      ::close(fd);
    }
    ::unlink(temporary.c_str());
    throw;
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throwSystem("repo-artifact-metadata-write-failed", "rename", path);
  }
  fsyncDirectory(fs::path(path).parent_path());
}

std::vector<uint8_t>
encodeRanges(const std::vector<ArtifactByteRange>& ranges)
{
  std::vector<uint8_t> bytes(RANGE_MAGIC.begin(), RANGE_MAGIC.end());
  putU64(bytes, ranges.size());
  for (const auto& range : ranges) {
    putU64(bytes, range.offsetBytes);
    putU64(bytes, range.lengthBytes);
  }
  return bytes;
}

std::string
rangePath(const std::string& staging)
{
  return staging + ".ranges";
}

std::string
intentPath(const std::string& staging)
{
  return staging + ".finalize-intent";
}

void
removeIfExists(const std::string& path)
{
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    throwSystem("repo-artifact-remove-failed", "unlink", path);
  }
}

class Statement
{
public:
  Statement(sqlite3* database, const char* sql)
  {
    if (sqlite3_prepare_v2(database, sql, -1, &m_value, nullptr) != SQLITE_OK) {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: prepare: ") +
        sqlite3_errmsg(database));
    }
  }

  ~Statement()
  {
    sqlite3_finalize(m_value);
  }

  sqlite3_stmt* get() const
  {
    return m_value;
  }

private:
  sqlite3_stmt* m_value = nullptr;
};

void
sqlExec(sqlite3* database, const char* sql)
{
  char* message = nullptr;
  const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (status != SQLITE_OK) {
    const std::string detail = message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    throw std::runtime_error("repo-metadata-sql-failed: " + detail);
  }
}

void
bindText(sqlite3* database, sqlite3_stmt* statement, int index,
         const std::string& value)
{
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: bind: ") + sqlite3_errmsg(database));
  }
}

ArtifactLifecycleEvent
eventFromStatement(sqlite3_stmt* statement)
{
  ArtifactLifecycleEvent event;
  event.eventId = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  event.operationId = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
  event.artifactDigest =
    reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
  event.generation = static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
  event.fromState = parseArtifactLifecycleState(
    reinterpret_cast<const char*>(sqlite3_column_text(statement, 4)));
  event.toState = parseArtifactLifecycleState(
    reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)));
  event.eventTimeMs = static_cast<uint64_t>(sqlite3_column_int64(statement, 6));
  event.accepted = sqlite3_column_int(statement, 7) != 0;
  event.detail = reinterpret_cast<const char*>(sqlite3_column_text(statement, 8));
  return event;
}

} // namespace

FilesystemArtifactPayloadStore::FilesystemArtifactPayloadStore(
  std::string rootPath, uint64_t maxRangeBytes)
  : m_rootPath(fs::absolute(std::move(rootPath)).lexically_normal().string())
  , m_maxRangeBytes(maxRangeBytes)
{
  if (m_maxRangeBytes == 0) {
    throw std::invalid_argument(
      "repo-artifact-range-limit-invalid: maxRangeBytes must be positive");
  }
  fs::create_directories(fs::path(m_rootPath) / "staging");
  fs::create_directories(fs::path(m_rootPath) / "payloads" / "sha256");
}

const std::string&
FilesystemArtifactPayloadStore::rootPath() const noexcept
{
  return m_rootPath;
}

std::string
FilesystemArtifactPayloadStore::committedPath(const ArtifactReference& artifact) const
{
  artifact.validate();
  return (fs::path(m_rootPath) / "payloads" / artifact.digestAlgorithm /
          artifact.contentDigest.substr(0, 2) / artifact.contentDigest).string();
}

std::string
FilesystemArtifactPayloadStore::stagingPath(const ArtifactReference& artifact,
                                            uint64_t generation) const
{
  artifact.validate();
  validateGeneration(generation);
  return (fs::path(m_rootPath) / "staging" /
          (artifact.contentDigest + "." + std::to_string(generation) + ".part")).string();
}

void
FilesystemArtifactPayloadStore::begin(const ArtifactReference& artifact,
                                      uint64_t generation)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  const auto path = stagingPath(artifact, generation);
  if (fs::exists(committedPath(artifact))) {
    return;
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    throwSystem("repo-artifact-begin-failed", "open", path);
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0 ||
      (status.st_size != 0 &&
       static_cast<uint64_t>(status.st_size) != artifact.sizeBytes) ||
      ::ftruncate(fd, static_cast<off_t>(artifact.sizeBytes)) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-begin-failed", "size staging file", path);
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-begin-failed", "fsync", path);
  }
  ::close(fd);
  fsyncDirectory(fs::path(path).parent_path());
}

void
FilesystemArtifactPayloadStore::writeRange(const ArtifactReference& artifact,
                                           uint64_t generation,
                                           ArtifactByteRange range,
                                           const std::vector<uint8_t>& bytes)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  validateRange(artifact, range, bytes.size(), true, m_maxRangeBytes);
  const auto path = stagingPath(artifact, generation);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-write-failed", "open", path);
  }
  try {
    if (!bytes.empty()) {
      pwriteAll(fd, bytes.data(), bytes.size(), range.offsetBytes, path);
    }
  }
  catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
}

std::vector<uint8_t>
FilesystemArtifactPayloadStore::readRange(const ArtifactReference& artifact,
                                          uint64_t generation,
                                          ArtifactByteRange range) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  validateRange(artifact, range, 0, false, m_maxRangeBytes);
  const auto committed = committedPath(artifact);
  const auto path = fs::exists(committed) ? committed : stagingPath(artifact, generation);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-read-failed", "open", path);
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(range.lengthBytes));
  try {
    if (!bytes.empty()) {
      preadAll(fd, bytes.data(), bytes.size(), range.offsetBytes, path);
    }
  }
  catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  return bytes;
}

void
FilesystemArtifactPayloadStore::markVerified(const ArtifactReference& artifact,
                                             uint64_t generation,
                                             ArtifactByteRange range)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  validateRange(artifact, range, 0, false, m_maxRangeBytes);
  const auto path = rangePath(stagingPath(artifact, generation));
  const auto merged = mergeRange(decodeRanges(path, artifact.sizeBytes), range);
  atomicWrite(path, encodeRanges(merged));
}

std::vector<ArtifactByteRange>
FilesystemArtifactPayloadStore::verifiedRanges(const ArtifactReference& artifact,
                                               uint64_t generation) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  if (fs::exists(committedPath(artifact))) {
    return artifact.sizeBytes == 0
             ? std::vector<ArtifactByteRange>{}
             : std::vector<ArtifactByteRange>{{0, artifact.sizeBytes}};
  }
  return decodeRanges(rangePath(stagingPath(artifact, generation)),
                      artifact.sizeBytes);
}

void
FilesystemArtifactPayloadStore::flush(const ArtifactReference& artifact,
                                      uint64_t generation)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto path = stagingPath(artifact, generation);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-sync-failed", "open", path);
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-sync-failed", "fsync", path);
  }
  ::close(fd);
}

void
FilesystemArtifactPayloadStore::finalize(const ArtifactReference& artifact,
                                         uint64_t generation)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  artifact.validate();
  const auto committed = committedPath(artifact);
  if (fs::exists(committed)) {
    if (sha256File(committed) != artifact.contentDigest) {
      throw std::runtime_error(
        "repo-artifact-committed-corrupt: committed payload digest mismatch");
    }
    return;
  }
  const auto staging = stagingPath(artifact, generation);
  const auto ranges = decodeRanges(rangePath(staging), artifact.sizeBytes);
  const bool complete =
    artifact.sizeBytes == 0 ? ranges.empty()
                            : ranges.size() == 1 && ranges[0].offsetBytes == 0 &&
                                ranges[0].lengthBytes == artifact.sizeBytes;
  if (!complete) {
    throw std::runtime_error(
      "repo-artifact-incomplete: verified ranges do not cover the artifact");
  }
  const int fd = ::open(staging.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-artifact-finalize-failed", "open", staging);
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-finalize-failed", "fsync", staging);
  }
  ::close(fd);
  if (sha256File(staging) != artifact.contentDigest) {
    throw std::runtime_error(
      "repo-artifact-digest-mismatch: staged payload failed full verification");
  }

  std::vector<uint8_t> intent;
  intent.insert(intent.end(), artifact.contentDigest.begin(),
                artifact.contentDigest.end());
  intent.push_back('\n');
  const auto generationText = std::to_string(generation) + "\n";
  intent.insert(intent.end(), generationText.begin(), generationText.end());
  atomicWrite(intentPath(staging), intent);

  const fs::path committedDirectory = fs::path(committed).parent_path();
  fs::create_directories(committedDirectory);
  fsyncDirectory(committedDirectory.parent_path());
  if (::rename(staging.c_str(), committed.c_str()) != 0) {
    throwSystem("repo-artifact-finalize-failed", "rename", committed);
  }
  fsyncDirectory(committedDirectory);
  removeIfExists(rangePath(staging));
  removeIfExists(intentPath(staging));
  fsyncDirectory(fs::path(staging).parent_path());
}

bool
FilesystemArtifactPayloadStore::isCommitted(const ArtifactReference& artifact,
                                            uint64_t generation) const
{
  std::lock_guard<std::mutex> guard(m_mutex);
  validateGeneration(generation);
  return fs::exists(committedPath(artifact));
}

void
FilesystemArtifactPayloadStore::abort(const ArtifactReference& artifact,
                                      uint64_t generation)
{
  std::lock_guard<std::mutex> guard(m_mutex);
  const auto staging = stagingPath(artifact, generation);
  removeIfExists(staging);
  removeIfExists(rangePath(staging));
  removeIfExists(intentPath(staging));
  fsyncDirectory(fs::path(staging).parent_path());
}

struct SqliteArtifactMetadataStore::Impl
{
  static constexpr uint64_t RUNTIME_SCHEMA_GENERATION = 12;

  explicit Impl(std::string path, bool artifactWritesEnabled,
                uint64_t maxWriteSchemaGeneration)
    : databasePath(fs::absolute(std::move(path)).lexically_normal().string())
  {
    fs::create_directories(fs::path(databasePath).parent_path());
    if (sqlite3_open_v2(databasePath.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      const std::string detail =
        database == nullptr ? "unknown" : sqlite3_errmsg(database);
      if (database != nullptr) {
        sqlite3_close(database);
        database = nullptr;
      }
      throw std::runtime_error("repo-metadata-open-failed: " + detail);
    }
    sqlExec(database, "PRAGMA journal_mode=WAL");
    sqlExec(database, "PRAGMA synchronous=FULL");
    sqlExec(database,
            "CREATE TABLE IF NOT EXISTS repo_meta("
            "key TEXT PRIMARY KEY,value TEXT NOT NULL)");
    uint64_t storedGeneration = 0;
    {
      Statement statement(
        database,
        "SELECT value FROM repo_meta WHERE key='schema_generation'");
      const int status = sqlite3_step(statement.get());
      if (status == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement.get(), 0);
        try {
          storedGeneration =
            text == nullptr ? 0 : std::stoull(reinterpret_cast<const char*>(text));
        }
        catch (const std::exception&) {
          throw std::runtime_error(
            "repo-schema-invalid-generation: stored generation is not numeric");
        }
      }
      else if (status != SQLITE_DONE) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: schema generation: ") +
          sqlite3_errmsg(database));
      }
    }
    migration.runtimeSchemaGeneration = RUNTIME_SCHEMA_GENERATION;
    migration.databaseSchemaGeneration = storedGeneration;
    migration.previousSchemaGeneration = storedGeneration;
    migration.maxWriteSchemaGeneration = maxWriteSchemaGeneration;
    migration.destructiveChanges = false;
    if (!artifactWritesEnabled ||
        storedGeneration > maxWriteSchemaGeneration ||
        storedGeneration > RUNTIME_SCHEMA_GENERATION) {
      migration.writesEnabled = false;
      migration.action = "read-only-rollback";
      migration.reason = artifactWritesEnabled
                           ? "database-schema-newer-than-write-runtime"
                           : "operator-disabled";
      return;
    }
    sqlExec(database,
            "INSERT OR REPLACE INTO repo_meta(key,value)"
            "VALUES('schema_generation','12')");
    sqlExec(database,
            "CREATE TABLE IF NOT EXISTS artifact_lifecycle_journal("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "event_id TEXT NOT NULL UNIQUE,"
            "operation_id TEXT NOT NULL,"
            "artifact_digest TEXT NOT NULL,"
            "generation INTEGER NOT NULL,"
            "from_state TEXT NOT NULL,"
            "to_state TEXT NOT NULL,"
            "event_time_ms INTEGER NOT NULL,"
            "accepted INTEGER NOT NULL,"
            "detail TEXT NOT NULL)");
    sqlExec(database,
            "CREATE INDEX IF NOT EXISTS idx_artifact_lifecycle_operation "
            "ON artifact_lifecycle_journal(operation_id,sequence)");
    migration.databaseSchemaGeneration = RUNTIME_SCHEMA_GENERATION;
    migration.writesEnabled = true;
    migration.action = storedGeneration == 0
                         ? "initialized"
                         : storedGeneration < RUNTIME_SCHEMA_GENERATION
                             ? "roll-forward"
                             : "none";
  }

  ~Impl()
  {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }

  std::string databasePath;
  sqlite3* database = nullptr;
  mutable std::mutex mutex;
  ArtifactBackendMigrationDiagnostics migration;
};

SqliteArtifactMetadataStore::SqliteArtifactMetadataStore(
  std::string databasePath, bool artifactWritesEnabled,
  uint64_t maxWriteSchemaGeneration)
  : m_impl(std::make_unique<Impl>(
      std::move(databasePath), artifactWritesEnabled,
      maxWriteSchemaGeneration))
{
}

SqliteArtifactMetadataStore::~SqliteArtifactMetadataStore() = default;

uint64_t
SqliteArtifactMetadataStore::schemaGeneration() const
{
  return m_impl->migration.databaseSchemaGeneration;
}

bool
SqliteArtifactMetadataStore::artifactWritesEnabled() const noexcept
{
  return m_impl->migration.writesEnabled;
}

ArtifactBackendMigrationDiagnostics
SqliteArtifactMetadataStore::migrationDiagnostics() const
{
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  return m_impl->migration;
}

void
SqliteArtifactMetadataStore::appendLifecycleEvent(
  const ArtifactLifecycleEvent& event)
{
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (!m_impl->migration.writesEnabled) {
    throw std::runtime_error(
      "repo-artifact-writes-disabled: " + m_impl->migration.reason);
  }
  sqlExec(m_impl->database, "BEGIN IMMEDIATE");
  try {
    Statement statement(
      m_impl->database,
      "INSERT INTO artifact_lifecycle_journal("
      "event_id,operation_id,artifact_digest,generation,from_state,to_state,"
      "event_time_ms,accepted,detail) VALUES(?,?,?,?,?,?,?,?,?)");
    bindText(m_impl->database, statement.get(), 1, event.eventId);
    bindText(m_impl->database, statement.get(), 2, event.operationId);
    bindText(m_impl->database, statement.get(), 3, event.artifactDigest);
    sqlite3_bind_int64(statement.get(), 4,
                       static_cast<sqlite3_int64>(event.generation));
    bindText(m_impl->database, statement.get(), 5, toString(event.fromState));
    bindText(m_impl->database, statement.get(), 6, toString(event.toState));
    sqlite3_bind_int64(statement.get(), 7,
                       static_cast<sqlite3_int64>(event.eventTimeMs));
    sqlite3_bind_int(statement.get(), 8, event.accepted ? 1 : 0);
    bindText(m_impl->database, statement.get(), 9, event.detail);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: insert: ") +
        sqlite3_errmsg(m_impl->database));
    }
    sqlExec(m_impl->database, "COMMIT");
  }
  catch (...) {
    sqlite3_exec(m_impl->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::vector<ArtifactLifecycleEvent>
SqliteArtifactMetadataStore::lifecycleEvents(
  const std::string& operationId) const
{
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  Statement statement(
    m_impl->database,
    "SELECT event_id,operation_id,artifact_digest,generation,from_state,to_state,"
    "event_time_ms,accepted,detail FROM artifact_lifecycle_journal "
    "WHERE operation_id=? ORDER BY sequence");
  bindText(m_impl->database, statement.get(), 1, operationId);
  std::vector<ArtifactLifecycleEvent> events;
  for (;;) {
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_DONE) {
      return events;
    }
    if (status != SQLITE_ROW) {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: select: ") +
        sqlite3_errmsg(m_impl->database));
    }
    events.push_back(eventFromStatement(statement.get()));
  }
}

ArtifactLifecycleState
SqliteArtifactMetadataStore::currentLifecycleState(
  const std::string& operationId) const
{
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  Statement statement(
    m_impl->database,
    "SELECT to_state FROM artifact_lifecycle_journal "
    "WHERE operation_id=? AND accepted=1 ORDER BY sequence DESC LIMIT 1");
  bindText(m_impl->database, statement.get(), 1, operationId);
  const int status = sqlite3_step(statement.get());
  if (status == SQLITE_DONE) {
    return ArtifactLifecycleState::Absent;
  }
  if (status != SQLITE_ROW) {
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: state: ") +
      sqlite3_errmsg(m_impl->database));
  }
  return parseArtifactLifecycleState(
    reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)));
}

const std::string&
SqliteArtifactMetadataStore::databasePath() const noexcept
{
  return m_impl->databasePath;
}

std::unique_ptr<RepositoryStoreFacade>
makeFilesystemArtifactRepositoryStore(const std::string& rootPath,
                                      const std::string& ownerId)
{
  const auto absoluteRoot = fs::absolute(rootPath).lexically_normal();
  fs::create_directories(absoluteRoot);
  auto payload = std::make_shared<FilesystemArtifactPayloadStore>(
    (absoluteRoot / "artifact-v2").string());
  auto metadata = std::make_shared<SqliteArtifactMetadataStore>(
    (absoluteRoot / "artifact-v2-metadata.sqlite3").string());
  return std::make_unique<RepositoryStoreFacade>(
    (absoluteRoot / "artifact-v2").string(), ownerId,
    std::move(payload), std::move(metadata));
}

} // namespace ndnsf_distributed_repo
