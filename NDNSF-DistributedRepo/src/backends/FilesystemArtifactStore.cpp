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
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
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
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
      if (errno == EINTR) {
        continue;
      }
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
ensurePrivateDirectory(const fs::path& path)
{
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (!error && fs::exists(status)) {
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
      throw std::runtime_error("repo-artifact-directory-invalid: " + path.string());
    }
  }
  else if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-directory-stat-failed: " +
                             error.message());
  }
  else if (!fs::exists(status)) {
    fs::create_directories(path, error);
    if (error) {
      throw std::runtime_error("repo-artifact-directory-create-failed: " +
                               error.message());
    }
  }
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
  if (error) {
    throw std::runtime_error("repo-artifact-directory-permissions-failed: " +
                             error.message());
  }
}

void
writeAll(int fd, const uint8_t* bytes, size_t size, const std::string& path)
{
  size_t written = 0;
  while (written < size) {
    const auto count = ::write(fd, bytes + written, size - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throwSystem("repo-artifact-write-failed", "write", path);
    }
    if (count == 0) {
      throw std::runtime_error("repo-artifact-write-failed: short write " +
                               path);
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
      if (errno == EINTR) {
        continue;
      }
      throwSystem("repo-artifact-write-failed", "pwrite", path);
    }
    if (count == 0) {
      throw std::runtime_error("repo-artifact-write-failed: short write " +
                               path);
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
      if (errno == EINTR) {
        continue;
      }
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
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) {
    return {};
  }
  if (fd < 0) {
    throwSystem("repo-artifact-range-map-read-failed", "open", path);
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-artifact-range-map-read-failed", "stat", path);
  }
  const auto size = static_cast<uint64_t>(status.st_size);
  const uint64_t maximum = 4 + 8 + MAX_VERIFIED_RANGES * 16;
  if (size > maximum) {
    ::close(fd);
    throw std::runtime_error(
      "repo-artifact-range-map-invalid: range map exceeds parser bound");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  try {
    if (!bytes.empty()) {
      preadAll(fd, bytes.data(), bytes.size(), 0, path);
    }
  }
  catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) {
    throwSystem("repo-artifact-range-map-read-failed", "close", path);
  }
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
                  O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
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

std::string
columnText(sqlite3_stmt* statement, int index)
{
  const auto* value = sqlite3_column_text(statement, index);
  return value == nullptr
           ? std::string{}
           : reinterpret_cast<const char*>(value);
}

void
bindInt64(sqlite3* database, sqlite3_stmt* statement, int index, int64_t value)
{
  if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: bind integer: ") +
      sqlite3_errmsg(database));
  }
}

void
bindReference(sqlite3* database, sqlite3_stmt* statement, int index,
              const ArtifactReference& reference)
{
  bindText(database, statement, index++, reference.logicalName);
  bindText(database, statement, index++, reference.digestAlgorithm);
  bindText(database, statement, index++, reference.contentDigest);
  bindInt64(database, statement, index++, static_cast<int64_t>(reference.sizeBytes));
  bindText(database, statement, index++, reference.formatVersion);
  bindText(database, statement, index++, reference.rootManifestName);
  bindText(database, statement, index++, reference.publisherIdentity);
  bindText(database, statement, index++, reference.policyEpoch);
}

ArtifactReference
referenceFromColumns(sqlite3_stmt* statement, int index)
{
  const auto size = sqlite3_column_int64(statement, index + 3);
  if (size < 0) {
    throw std::runtime_error("repo-schema-reference-invalid: negative artifact size");
  }
  return ArtifactReference{
    columnText(statement, index), columnText(statement, index + 1),
    columnText(statement, index + 2), static_cast<uint64_t>(size),
    columnText(statement, index + 4), columnText(statement, index + 5),
    columnText(statement, index + 6), columnText(statement, index + 7),
  };
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
  std::string rootPath, uint64_t maxRangeBytes, bool createDirectories)
  : m_rootPath(fs::absolute(std::move(rootPath)).lexically_normal().string())
  , m_maxRangeBytes(maxRangeBytes)
  , m_createDirectories(createDirectories)
{
  if (m_maxRangeBytes == 0) {
    throw std::invalid_argument(
      "repo-artifact-range-limit-invalid: maxRangeBytes must be positive");
  }
  if (m_createDirectories) {
    ensurePrivateDirectory(fs::path(m_rootPath));
    ensurePrivateDirectory(fs::path(m_rootPath) / "staging");
    ensurePrivateDirectory(fs::path(m_rootPath) / "payloads");
    ensurePrivateDirectory(fs::path(m_rootPath) / "payloads" / "sha256");
  }
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
  const auto shard = fs::path(m_rootPath) / "payloads" / artifact.digestAlgorithm /
                     artifact.contentDigest.substr(0, 2);
  return (shard / artifact.contentDigest).string();
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
  const auto committed = committedPath(artifact);
  std::error_code statusError;
  const auto committedStatus = fs::symlink_status(committed, statusError);
  if (statusError && statusError != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-committed-stat-failed: " +
                             statusError.message());
  }
  if (fs::is_symlink(committedStatus)) {
    throw std::runtime_error("repo-artifact-committed-symlink-forbidden: " +
                             committed);
  }
  if (fs::exists(committedStatus)) {
    return;
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
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
  const int fd = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
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
  std::error_code committedError;
  const auto committedStatus = fs::symlink_status(committed, committedError);
  if (committedError && committedError != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-committed-stat-failed: " +
                             committedError.message());
  }
  if (fs::is_symlink(committedStatus)) {
    throw std::runtime_error("repo-artifact-committed-symlink-forbidden: " +
                             committed);
  }
  const auto path = fs::exists(committedStatus) ? committed : stagingPath(artifact, generation);
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
  const auto committed = committedPath(artifact);
  std::error_code committedError;
  const auto committedStatus = fs::symlink_status(committed, committedError);
  if (committedError && committedError != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-committed-stat-failed: " +
                             committedError.message());
  }
  if (fs::is_symlink(committedStatus)) {
    throw std::runtime_error("repo-artifact-committed-symlink-forbidden: " +
                             committed);
  }
  if (fs::exists(committedStatus)) {
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
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
  std::error_code committedStatusError;
  const auto committedStatus = fs::symlink_status(committed, committedStatusError);
  if (committedStatusError &&
      committedStatusError != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-committed-stat-failed: " +
                             committedStatusError.message());
  }
  if (fs::is_symlink(committedStatus)) {
    throw std::runtime_error("repo-artifact-committed-symlink-forbidden: " +
                             committed);
  }
  if (fs::exists(committedStatus)) {
    if (sha256File(committed) != artifact.contentDigest) {
      throw std::runtime_error(
        "repo-artifact-committed-corrupt: committed payload digest mismatch");
    }
    return;
  }
  const auto staging = stagingPath(artifact, generation);
  const auto ranges = artifact.sizeBytes == 0
    ? std::vector<ArtifactByteRange>{}
    : decodeRanges(rangePath(staging), artifact.sizeBytes);
  const bool complete =
    artifact.sizeBytes == 0 ? ranges.empty()
                            : ranges.size() == 1 && ranges[0].offsetBytes == 0 &&
                                ranges[0].lengthBytes == artifact.sizeBytes;
  if (!complete) {
    throw std::runtime_error(
      "repo-artifact-incomplete: verified ranges do not cover the artifact");
  }
  const int fd = ::open(staging.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
  ensurePrivateDirectory(committedDirectory);
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
  const auto committed = committedPath(artifact);
  std::error_code statusError;
  const auto status = fs::symlink_status(committed, statusError);
  if (statusError && statusError != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-artifact-committed-stat-failed: " +
                             statusError.message());
  }
  if (fs::is_symlink(status)) {
    throw std::runtime_error("repo-artifact-committed-symlink-forbidden: " +
                             committed);
  }
  return fs::exists(status);
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
  static constexpr uint64_t RUNTIME_SCHEMA_GENERATION = 14;

  struct Profile
  {
    uint64_t generation = 0;
    bool hasLocalCommitCatalog = false;
    bool hasPersistentCatalog = false;
  };

  explicit Impl(std::string path, bool requestedWrites,
                uint64_t maxWriteSchemaGeneration,
                bool immutableReadOnlyProfile)
    : databasePath(fs::absolute(std::move(path)).lexically_normal().string())
  {
    migration.runtimeSchemaGeneration = RUNTIME_SCHEMA_GENERATION;
    migration.maxWriteSchemaGeneration = maxWriteSchemaGeneration;
    migration.destructiveChanges = false;

    std::error_code statusError;
    const auto status = fs::symlink_status(databasePath, statusError);
    if (statusError && statusError != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("repo-schema-file-stat-failed: " +
                               statusError.message());
    }
    const bool databaseExists = !statusError && fs::exists(status);
    if (databaseExists &&
        (fs::is_symlink(status) || !fs::is_regular_file(status))) {
      throw std::runtime_error(
        "repo-schema-file-invalid: existing metadata path must be a regular file");
    }
    if (!databaseExists) {
      if (!requestedWrites || maxWriteSchemaGeneration < RUNTIME_SCHEMA_GENERATION) {
        throw std::runtime_error(
          "repo-schema-read-only-missing: cannot create metadata in read-only mode");
      }
      initializeNewDatabase();
      return;
    }

    const int preflightFlags = SQLITE_OPEN_READONLY |
      (immutableReadOnlyProfile ? SQLITE_OPEN_URI : 0);
    const auto preflightPath = immutableReadOnlyProfile
      ? immutableReadOnlyUri(databasePath) : databasePath;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> preflight(
      openDatabase(preflightPath, preflightFlags), sqlite3_close);
    const Profile observed = inspectProfile(preflight.get());
    migration.previousSchemaGeneration = observed.generation;
    migration.databaseSchemaGeneration = observed.generation;
    catalogAvailable = observed.hasLocalCommitCatalog;

    const bool futureReadOnly = observed.generation > RUNTIME_SCHEMA_GENERATION;
    const bool belowWriteCeiling =
      observed.generation > maxWriteSchemaGeneration ||
      (observed.generation < RUNTIME_SCHEMA_GENERATION &&
       maxWriteSchemaGeneration < RUNTIME_SCHEMA_GENERATION);
    const bool schema12 = observed.generation == 12 &&
                          !observed.hasLocalCommitCatalog;
    const bool schema13 = observed.generation == 13 &&
                          observed.hasLocalCommitCatalog;
    const bool schema14 = observed.generation == 14 &&
                          observed.hasLocalCommitCatalog &&
                          observed.hasPersistentCatalog;
    const bool markerOnlyNine = observed.generation == 9 &&
                                observed.hasLocalCommitCatalog;

    if (futureReadOnly || !requestedWrites || belowWriteCeiling) {
      if ((!observed.hasLocalCommitCatalog && observed.generation >= 13) ||
          (observed.generation >= 14 && !observed.hasPersistentCatalog)) {
        throw std::runtime_error(
          "repo-schema-unsupported-profile: future schema lacks compatible local catalog");
      }
      if (immutableReadOnlyProfile) {
        database = preflight.release();
      }
      else {
        preflight.reset();
        openExistingDatabase(SQLITE_OPEN_READONLY);
      }
      migration.writesEnabled = false;
      migration.action = "read-only-rollback";
      migration.reason = futureReadOnly
                           ? "database-schema-newer-than-write-runtime"
                           : !requestedWrites
                               ? "operator-disabled"
                               : "write-ceiling-below-database-schema";
      return;
    }

    if (!schema12 && !schema13 && !schema14 && !markerOnlyNine) {
      throw std::runtime_error(
        "repo-schema-unsupported-profile: generation marker and table shape do not match");
    }

    preflight.reset();
    openExistingDatabase(SQLITE_OPEN_READWRITE);
    try {
      sqlExec(database, "BEGIN IMMEDIATE");
      const Profile rechecked = inspectProfile(database);
      if (rechecked.generation != observed.generation ||
          rechecked.hasLocalCommitCatalog != observed.hasLocalCommitCatalog) {
        throw std::runtime_error(
          "repo-schema-changed-during-open: metadata profile changed after preflight");
      }
      if (schema12) {
        createLocalCommitCatalog(database);
        setGeneration(database, 13);
        migration.action = "roll-forward";
      }
      else if (markerOnlyNine) {
        setGeneration(database, 13);
        migration.action = "marker-repaired";
      }
      else if (schema13) {
        migration.action = "roll-forward";
      }
      if (!schema14) {
        if (!rechecked.hasPersistentCatalog) {
          createPersistentCatalog(database);
        }
        setGeneration(database, RUNTIME_SCHEMA_GENERATION);
        if (migration.action == "marker-repaired") {
          migration.action = "marker-repaired+roll-forward";
        }
      }
      else {
        migration.action = "none";
      }
      sqlExec(database, "COMMIT");
    }
    catch (...) {
      try {
        sqlExec(database, "ROLLBACK");
      }
      catch (...) {
      }
      throw;
    }
    sqlExec(database, "PRAGMA journal_mode=WAL");
    sqlExec(database, "PRAGMA synchronous=FULL");
    migration.databaseSchemaGeneration = RUNTIME_SCHEMA_GENERATION;
    migration.writesEnabled = true;
    catalogAvailable = true;
  }

  static sqlite3*
  openDatabase(const std::string& path, int flags)
  {
    sqlite3* result = nullptr;
    if (sqlite3_open_v2(path.c_str(), &result,
                        flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      const std::string detail =
        result == nullptr ? "unknown" : sqlite3_errmsg(result);
      if (result != nullptr) {
        sqlite3_close(result);
      }
      throw std::runtime_error("repo-metadata-open-failed: " + detail);
    }
    sqlite3_busy_timeout(result, 5000);
    return result;
  }

  static std::string
  immutableReadOnlyUri(const std::string& path)
  {
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string uri = "file:";
    for (const unsigned char value : path) {
      const bool safe = (value >= 'a' && value <= 'z') ||
                        (value >= 'A' && value <= 'Z') ||
                        (value >= '0' && value <= '9') || value == '/' ||
                        value == '-' || value == '_' || value == '.' ||
                        value == '~';
      if (safe) {
        uri.push_back(static_cast<char>(value));
      }
      else {
        uri.push_back('%');
        uri.push_back(HEX[value >> 4]);
        uri.push_back(HEX[value & 0x0f]);
      }
    }
    uri += "?immutable=1";
    return uri;
  }

  void
  openExistingDatabase(int flags)
  {
    database = openDatabase(databasePath, flags);
  }

  static bool
  tableExists(sqlite3* db, const char* table)
  {
    Statement statement(
      db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    bindText(db, statement.get(), 1, table);
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_ROW) {
      return true;
    }
    if (status == SQLITE_DONE) {
      return false;
    }
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: inspect tables: ") + sqlite3_errmsg(db));
  }

  static std::set<std::string>
  tableColumns(sqlite3* db, const char* table)
  {
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    Statement statement(db, sql.c_str());
    std::set<std::string> columns;
    for (;;) {
      const int status = sqlite3_step(statement.get());
      if (status == SQLITE_DONE) {
        return columns;
      }
      if (status != SQLITE_ROW) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: inspect columns: ") +
          sqlite3_errmsg(db));
      }
      const auto* name = sqlite3_column_text(statement.get(), 1);
      if (name != nullptr) {
        columns.emplace(reinterpret_cast<const char*>(name));
      }
    }
  }

  static bool
  containsColumns(const std::set<std::string>& actual,
                  const std::set<std::string>& required)
  {
    return std::includes(actual.begin(), actual.end(),
                         required.begin(), required.end());
  }

  static bool
  hasUniqueIndex(sqlite3* db, const char* table,
                 const std::vector<std::string>& expected)
  {
    Statement indexes(db,
      "SELECT name,partial FROM pragma_index_list(?) WHERE \"unique\"=1");
    bindText(db, indexes.get(), 1, table);
    for (;;) {
      const int status = sqlite3_step(indexes.get());
      if (status == SQLITE_DONE) {
        return false;
      }
      if (status != SQLITE_ROW) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: inspect catalog indexes: ") +
          sqlite3_errmsg(db));
      }
      const auto* indexName = sqlite3_column_text(indexes.get(), 0);
      if (indexName == nullptr || sqlite3_column_int(indexes.get(), 1) != 0) {
        continue;
      }
      Statement columns(db,
        "SELECT name,coll,key FROM pragma_index_xinfo(?) ORDER BY seqno");
      const std::string name(reinterpret_cast<const char*>(indexName));
      bindText(db, columns.get(), 1, name);
      std::vector<std::string> actual;
      bool hasExpectedCollation = true;
      for (;;) {
        const int columnStatus = sqlite3_step(columns.get());
        if (columnStatus == SQLITE_DONE) {
          break;
        }
        if (columnStatus != SQLITE_ROW) {
          throw std::runtime_error(
          std::string("repo-metadata-sql-failed: inspect catalog index columns: ") +
          sqlite3_errmsg(db));
        }
        if (sqlite3_column_int(columns.get(), 2) == 0) {
          continue;
        }
        const auto* columnName = sqlite3_column_text(columns.get(), 0);
        actual.emplace_back(columnName == nullptr
                              ? ""
                              : reinterpret_cast<const char*>(columnName));
        const auto* collation = sqlite3_column_text(columns.get(), 1);
        const std::string collationName = collation == nullptr
                                            ? ""
                                            : reinterpret_cast<const char*>(collation);
        if (collationName != "BINARY") {
          hasExpectedCollation = false;
        }
      }
      if (actual == expected && hasExpectedCollation) {
        return true;
      }
    }
  }

  static bool
  hasUniqueReferenceIndex(sqlite3* db)
  {
    return hasUniqueIndex(db, "artifact_local_commits", {
      "logical_name", "digest_algorithm", "content_digest", "size_bytes",
      "format_version", "root_manifest_name", "publisher_identity", "policy_epoch",
    });
  }

  static bool
  hasForeignKey(sqlite3* db, const char* table, const char* referencedTable,
                const char* column, const char* referencedColumn)
  {
    Statement foreignKeys(db,
      "SELECT \"table\",\"from\",\"to\" FROM pragma_foreign_key_list(?)");
    bindText(db, foreignKeys.get(), 1, table);
    for (;;) {
      const int status = sqlite3_step(foreignKeys.get());
      if (status == SQLITE_DONE) {
        return false;
      }
      if (status != SQLITE_ROW) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: inspect catalog foreign keys: ") +
          sqlite3_errmsg(db));
      }
      if (columnText(foreignKeys.get(), 0) == referencedTable &&
          columnText(foreignKeys.get(), 1) == column &&
          columnText(foreignKeys.get(), 2) == referencedColumn) {
        return true;
      }
    }
  }

  static bool
  hasPersistentCatalogConstraints(sqlite3* db)
  {
    const std::vector<std::string> referenceColumns{
      "repo_node", "provider_identity", "logical_name", "digest_algorithm",
      "content_digest", "size_bytes", "format_version", "root_manifest_name",
      "publisher_identity", "policy_epoch",
    };
    const std::vector<std::string> operationColumns{
      "repo_node", "provider_identity", "operation_id",
    };
    return hasUniqueIndex(db, "artifact_persistent_receipts", {"receipt_id"}) &&
           hasUniqueIndex(db, "artifact_persistent_receipts", operationColumns) &&
           hasUniqueIndex(db, "artifact_persistent_receipts", referenceColumns) &&
           hasUniqueIndex(db, "artifact_locations", referenceColumns) &&
           hasForeignKey(db, "artifact_locations", "artifact_persistent_receipts",
                         "receipt_id", "receipt_id");
  }

  static uint64_t
  readGeneration(sqlite3* db)
  {
    Statement statement(
      db, "SELECT value FROM repo_meta WHERE key='schema_generation'");
    const int status = sqlite3_step(statement.get());
    if (status != SQLITE_ROW) {
      throw std::runtime_error(
        "repo-schema-invalid-generation: schema generation row is missing");
    }
    const auto* value = sqlite3_column_text(statement.get(), 0);
    if (value == nullptr) {
      throw std::runtime_error(
        "repo-schema-invalid-generation: schema generation is null");
    }
    const std::string text(reinterpret_cast<const char*>(value));
    try {
      if (text.empty() ||
          !std::all_of(text.begin(), text.end(), [] (char digit) {
            return digit >= '0' && digit <= '9';
          })) {
        throw std::invalid_argument("non-decimal schema generation");
      }
      size_t parsed = 0;
      const auto generation = std::stoull(text, &parsed);
      if (parsed != text.size()) {
        throw std::invalid_argument("trailing data");
      }
      return generation;
    }
    catch (const std::exception&) {
      throw std::runtime_error(
        "repo-schema-invalid-generation: stored generation is not an integer");
    }
  }

  static Profile
  inspectProfile(sqlite3* db)
  {
    const auto metaColumns = tableColumns(db, "repo_meta");
    const auto journalColumns = tableColumns(db, "artifact_lifecycle_journal");
    const std::set<std::string> requiredMeta{"key", "value"};
    const std::set<std::string> requiredJournal{
      "sequence", "event_id", "operation_id", "artifact_digest", "generation",
      "from_state", "to_state", "event_time_ms", "accepted", "detail",
    };
    if (!containsColumns(metaColumns, requiredMeta) ||
        !containsColumns(journalColumns, requiredJournal)) {
      throw std::runtime_error(
        "repo-schema-unsupported-profile: required metadata or lifecycle columns differ");
    }
    Profile profile;
    profile.generation = readGeneration(db);
    profile.hasLocalCommitCatalog = tableExists(db, "artifact_local_commits");
    if (profile.hasLocalCommitCatalog) {
      const auto catalogColumns = tableColumns(db, "artifact_local_commits");
      const std::set<std::string> requiredCatalog{
        "local_commit_id", "logical_name", "digest_algorithm", "content_digest",
        "size_bytes", "format_version", "root_manifest_name", "publisher_identity",
        "policy_epoch", "committed_at_ms",
      };
      if (!containsColumns(catalogColumns, requiredCatalog) ||
          !hasUniqueReferenceIndex(db)) {
        throw std::runtime_error(
          "repo-schema-unsupported-profile: local-commit catalog shape is incompatible");
      }
    }
    const bool hasReceipts = tableExists(db, "artifact_persistent_receipts");
    const bool hasLocations = tableExists(db, "artifact_locations");
    if (hasReceipts != hasLocations) {
      throw std::runtime_error(
        "repo-schema-unsupported-profile: Persistent receipt/location tables are incomplete");
    }
    profile.hasPersistentCatalog = hasReceipts && hasLocations;
    if (profile.generation >= 14 && !profile.hasPersistentCatalog) {
      throw std::runtime_error(
        "repo-schema-unsupported-profile: generation 14 requires Persistent catalogs");
    }
    if (profile.hasPersistentCatalog) {
      const std::set<std::string> requiredReceipt{
        "receipt_id", "operation_id", "repo_node", "provider_identity",
        "logical_name", "digest_algorithm", "content_digest", "size_bytes",
        "format_version", "root_manifest_name", "publisher_identity", "policy_epoch",
        "generation", "persisted_bytes", "committed_at_ms", "state",
      };
      const std::set<std::string> requiredLocation{
        "repo_node", "provider_identity", "logical_name", "digest_algorithm",
        "content_digest", "size_bytes", "format_version", "root_manifest_name",
        "publisher_identity", "policy_epoch", "receipt_id", "operation_id",
        "generation", "location_version", "committed_at_ms", "expires_at_ms",
        "route_state", "forwarding_hint",
      };
      if (!containsColumns(tableColumns(db, "artifact_persistent_receipts"),
                           requiredReceipt) ||
          !containsColumns(tableColumns(db, "artifact_locations"),
                           requiredLocation)) {
        throw std::runtime_error(
          "repo-schema-unsupported-profile: Persistent catalog columns are incompatible");
      }
      if (!hasPersistentCatalogConstraints(db)) {
        throw std::runtime_error(
          "repo-schema-unsupported-profile: Persistent catalog constraints are incompatible");
      }
    }
    return profile;
  }

  static void
  createLocalCommitCatalog(sqlite3* db)
  {
    sqlExec(db,
      "CREATE TABLE artifact_local_commits("
      "local_commit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "logical_name TEXT NOT NULL,digest_algorithm TEXT NOT NULL,"
      "content_digest TEXT NOT NULL,size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),"
      "format_version TEXT NOT NULL,root_manifest_name TEXT NOT NULL,"
      "publisher_identity TEXT NOT NULL,policy_epoch TEXT NOT NULL,"
      "committed_at_ms INTEGER NOT NULL CHECK(committed_at_ms>0),"
      "UNIQUE(logical_name,digest_algorithm,content_digest,size_bytes,format_version,"
      "root_manifest_name,publisher_identity,policy_epoch))");
  }

  static void
  createLifecycleJournal(sqlite3* db)
  {
    sqlExec(db,
      "CREATE TABLE artifact_lifecycle_journal("
      "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
      "event_id TEXT NOT NULL UNIQUE,operation_id TEXT NOT NULL,"
      "artifact_digest TEXT NOT NULL,generation INTEGER NOT NULL,"
      "from_state TEXT NOT NULL,to_state TEXT NOT NULL,event_time_ms INTEGER NOT NULL,"
      "accepted INTEGER NOT NULL,detail TEXT NOT NULL)");
    sqlExec(db,
      "CREATE INDEX idx_artifact_lifecycle_operation "
      "ON artifact_lifecycle_journal(operation_id,sequence)");
  }

  static void
  createPersistentCatalog(sqlite3* db)
  {
    sqlExec(db,
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
      "size_bytes,format_version,root_manifest_name,publisher_identity,policy_epoch))");
    sqlExec(db,
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
      "FOREIGN KEY(receipt_id) REFERENCES artifact_persistent_receipts(receipt_id))");
  }

  static void
  setGeneration(sqlite3* db, uint64_t generation)
  {
    Statement statement(
      db, "UPDATE repo_meta SET value=? WHERE key='schema_generation'");
    bindText(db, statement.get(), 1, std::to_string(generation));
    if (sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes(db) != 1) {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: update schema generation: ") +
        sqlite3_errmsg(db));
    }
  }

  void
  initializeNewDatabase()
  {
    fs::create_directories(fs::path(databasePath).parent_path());
    openExistingDatabase(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    try {
      sqlExec(database, "BEGIN IMMEDIATE");
      sqlExec(database,
        "CREATE TABLE repo_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
      sqlExec(database,
        "INSERT INTO repo_meta(key,value) VALUES('schema_generation','14')");
      createLifecycleJournal(database);
      createLocalCommitCatalog(database);
      createPersistentCatalog(database);
      sqlExec(database, "COMMIT");
    }
    catch (...) {
      try {
        sqlExec(database, "ROLLBACK");
      }
      catch (...) {
      }
      throw;
    }
    sqlExec(database, "PRAGMA journal_mode=WAL");
    sqlExec(database, "PRAGMA synchronous=FULL");
    migration.databaseSchemaGeneration = RUNTIME_SCHEMA_GENERATION;
    migration.previousSchemaGeneration = 0;
    migration.writesEnabled = true;
    migration.action = "initialized";
    catalogAvailable = true;
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
  bool catalogAvailable = false;
};

SqliteArtifactMetadataStore::SqliteArtifactMetadataStore(
  std::string databasePath, bool artifactWritesEnabled,
  uint64_t maxWriteSchemaGeneration, bool immutableReadOnlyProfile)
  : m_impl(std::make_unique<Impl>(
      std::move(databasePath), artifactWritesEnabled,
      maxWriteSchemaGeneration, immutableReadOnlyProfile))
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

ArtifactLocalCommitRecord
SqliteArtifactMetadataStore::recordLocalCommit(
  const ArtifactReference& reference, uint64_t committedAtMs)
{
  reference.validate();
  if (committedAtMs == 0 ||
      committedAtMs > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw std::invalid_argument(
      "repo-artifact-local-commit-invalid-time: commit time must fit a positive SQLite integer");
  }

  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (!m_impl->migration.writesEnabled) {
    throw std::runtime_error(
      "repo-artifact-writes-disabled: " + m_impl->migration.reason);
  }
  if (!m_impl->catalogAvailable) {
    throw std::runtime_error(
      "repo-schema-local-catalog-unavailable: this database profile has no local commit catalog");
  }

  sqlExec(m_impl->database, "BEGIN IMMEDIATE");
  try {
    {
      Statement insert(
        m_impl->database,
        "INSERT INTO artifact_local_commits(logical_name,digest_algorithm,"
        "content_digest,size_bytes,format_version,root_manifest_name,"
        "publisher_identity,policy_epoch,committed_at_ms) VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(logical_name,digest_algorithm,content_digest,size_bytes,"
        "format_version,root_manifest_name,publisher_identity,policy_epoch) DO NOTHING");
      bindText(m_impl->database, insert.get(), 1, reference.logicalName);
      bindText(m_impl->database, insert.get(), 2, reference.digestAlgorithm);
      bindText(m_impl->database, insert.get(), 3, reference.contentDigest);
      bindInt64(m_impl->database, insert.get(), 4,
                static_cast<int64_t>(reference.sizeBytes));
      bindText(m_impl->database, insert.get(), 5, reference.formatVersion);
      bindText(m_impl->database, insert.get(), 6, reference.rootManifestName);
      bindText(m_impl->database, insert.get(), 7, reference.publisherIdentity);
      bindText(m_impl->database, insert.get(), 8, reference.policyEpoch);
      bindInt64(m_impl->database, insert.get(), 9,
                static_cast<int64_t>(committedAtMs));
      if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: local commit insert: ") +
          sqlite3_errmsg(m_impl->database));
      }
    }

    ArtifactLocalCommitRecord record;
    {
      Statement lookup(
        m_impl->database,
        "SELECT local_commit_id,logical_name,digest_algorithm,content_digest,"
        "size_bytes,format_version,root_manifest_name,publisher_identity,"
        "policy_epoch,committed_at_ms FROM artifact_local_commits WHERE "
        "logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? "
        "AND format_version=? AND root_manifest_name=? AND publisher_identity=? "
        "AND policy_epoch=?");
      bindText(m_impl->database, lookup.get(), 1, reference.logicalName);
      bindText(m_impl->database, lookup.get(), 2, reference.digestAlgorithm);
      bindText(m_impl->database, lookup.get(), 3, reference.contentDigest);
      bindInt64(m_impl->database, lookup.get(), 4,
                static_cast<int64_t>(reference.sizeBytes));
      bindText(m_impl->database, lookup.get(), 5, reference.formatVersion);
      bindText(m_impl->database, lookup.get(), 6, reference.rootManifestName);
      bindText(m_impl->database, lookup.get(), 7, reference.publisherIdentity);
      bindText(m_impl->database, lookup.get(), 8, reference.policyEpoch);
      if (sqlite3_step(lookup.get()) != SQLITE_ROW) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: local commit lookup: ") +
          sqlite3_errmsg(m_impl->database));
      }
      const auto id = sqlite3_column_int64(lookup.get(), 0);
      const auto size = sqlite3_column_int64(lookup.get(), 4);
      const auto timestamp = sqlite3_column_int64(lookup.get(), 9);
      if (id <= 0 || size < 0 || timestamp <= 0) {
        throw std::runtime_error(
          "repo-schema-local-commit-invalid: stored local commit fields are out of range");
      }
      record.localCommitId = static_cast<uint64_t>(id);
      record.reference = ArtifactReference{
        columnText(lookup.get(), 1), columnText(lookup.get(), 2),
        columnText(lookup.get(), 3), static_cast<uint64_t>(size),
        columnText(lookup.get(), 5), columnText(lookup.get(), 6),
        columnText(lookup.get(), 7), columnText(lookup.get(), 8),
      };
      record.committedAtMs = static_cast<uint64_t>(timestamp);
    }
    sqlExec(m_impl->database, "COMMIT");
    return record;
  }
  catch (...) {
    sqlite3_exec(m_impl->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::optional<ArtifactLocalCommitRecord>
SqliteArtifactMetadataStore::lookupLocalCommit(
  const ArtifactReference& reference) const
{
  reference.validate();
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (!m_impl->catalogAvailable) {
    return std::nullopt;
  }

  Statement lookup(
    m_impl->database,
    "SELECT local_commit_id,logical_name,digest_algorithm,content_digest,"
    "size_bytes,format_version,root_manifest_name,publisher_identity,"
    "policy_epoch,committed_at_ms FROM artifact_local_commits WHERE "
    "logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? "
    "AND format_version=? AND root_manifest_name=? AND publisher_identity=? "
    "AND policy_epoch=?");
  bindText(m_impl->database, lookup.get(), 1, reference.logicalName);
  bindText(m_impl->database, lookup.get(), 2, reference.digestAlgorithm);
  bindText(m_impl->database, lookup.get(), 3, reference.contentDigest);
  bindInt64(m_impl->database, lookup.get(), 4,
            static_cast<int64_t>(reference.sizeBytes));
  bindText(m_impl->database, lookup.get(), 5, reference.formatVersion);
  bindText(m_impl->database, lookup.get(), 6, reference.rootManifestName);
  bindText(m_impl->database, lookup.get(), 7, reference.publisherIdentity);
  bindText(m_impl->database, lookup.get(), 8, reference.policyEpoch);
  const int status = sqlite3_step(lookup.get());
  if (status == SQLITE_DONE) {
    return std::nullopt;
  }
  if (status != SQLITE_ROW) {
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: local commit lookup: ") +
      sqlite3_errmsg(m_impl->database));
  }

  const auto id = sqlite3_column_int64(lookup.get(), 0);
  const auto size = sqlite3_column_int64(lookup.get(), 4);
  const auto timestamp = sqlite3_column_int64(lookup.get(), 9);
  if (id <= 0 || size < 0 || timestamp <= 0) {
    throw std::runtime_error(
      "repo-schema-local-commit-invalid: stored local commit fields are out of range");
  }
  ArtifactLocalCommitRecord record;
  record.localCommitId = static_cast<uint64_t>(id);
  record.reference = ArtifactReference{
    columnText(lookup.get(), 1), columnText(lookup.get(), 2),
    columnText(lookup.get(), 3), static_cast<uint64_t>(size),
    columnText(lookup.get(), 5), columnText(lookup.get(), 6),
    columnText(lookup.get(), 7), columnText(lookup.get(), 8),
  };
  record.committedAtMs = static_cast<uint64_t>(timestamp);
  return record;
}

void
SqliteArtifactMetadataStore::recordPersistentCommit(
  const ArtifactLocationRecord& location)
{
  location.validate();
  if (location.reference.sizeBytes > static_cast<uint64_t>(INT64_MAX) ||
      location.generation > static_cast<uint64_t>(INT64_MAX) ||
      location.locationVersion > static_cast<uint64_t>(INT64_MAX) ||
      location.committedAtMs > static_cast<uint64_t>(INT64_MAX) ||
      location.expiresAtMs > static_cast<uint64_t>(INT64_MAX)) {
    throw std::invalid_argument(
      "repo-artifact-persistent-receipt-invalid: value exceeds SQLite integer range");
  }
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (!m_impl->migration.writesEnabled ||
      m_impl->migration.databaseSchemaGeneration < 14) {
    throw std::runtime_error(
      "repo-artifact-writes-disabled: Persistent catalog is unavailable");
  }

  const auto& reference = location.reference;
  sqlExec(m_impl->database, "BEGIN IMMEDIATE");
  try {
    Statement receipt(m_impl->database,
      "INSERT INTO artifact_persistent_receipts(receipt_id,operation_id,repo_node,"
      "provider_identity,logical_name,digest_algorithm,content_digest,size_bytes,"
      "format_version,root_manifest_name,publisher_identity,policy_epoch,generation,"
      "persisted_bytes,committed_at_ms,state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(receipt_id) DO NOTHING");
    bindText(m_impl->database, receipt.get(), 1, location.receiptId);
    bindText(m_impl->database, receipt.get(), 2, location.operationId);
    bindText(m_impl->database, receipt.get(), 3, location.repoNode);
    bindText(m_impl->database, receipt.get(), 4, location.providerIdentity);
    bindReference(m_impl->database, receipt.get(), 5, reference);
    bindInt64(m_impl->database, receipt.get(), 13, static_cast<int64_t>(location.generation));
    bindInt64(m_impl->database, receipt.get(), 14, static_cast<int64_t>(reference.sizeBytes));
    bindInt64(m_impl->database, receipt.get(), 15, static_cast<int64_t>(location.committedAtMs));
    bindText(m_impl->database, receipt.get(), 16, "COMMITTED");
    if (sqlite3_step(receipt.get()) != SQLITE_DONE) {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: Persistent receipt insert: ") +
        sqlite3_errmsg(m_impl->database));
    }

    Statement checkReceipt(m_impl->database,
      "SELECT operation_id,repo_node,provider_identity,logical_name,digest_algorithm,"
      "content_digest,size_bytes,format_version,root_manifest_name,publisher_identity,"
      "policy_epoch,generation,persisted_bytes,committed_at_ms,state "
      "FROM artifact_persistent_receipts WHERE receipt_id=?");
    bindText(m_impl->database, checkReceipt.get(), 1, location.receiptId);
    if (sqlite3_step(checkReceipt.get()) != SQLITE_ROW ||
        columnText(checkReceipt.get(), 0) != location.operationId ||
        columnText(checkReceipt.get(), 1) != location.repoNode ||
        columnText(checkReceipt.get(), 2) != location.providerIdentity ||
        referenceFromColumns(checkReceipt.get(), 3) != reference ||
        sqlite3_column_int64(checkReceipt.get(), 11) != static_cast<int64_t>(location.generation) ||
        sqlite3_column_int64(checkReceipt.get(), 12) != static_cast<int64_t>(reference.sizeBytes) ||
        sqlite3_column_int64(checkReceipt.get(), 13) != static_cast<int64_t>(location.committedAtMs) ||
        columnText(checkReceipt.get(), 14) != "COMMITTED") {
      throw std::runtime_error(
        "repo-artifact-receipt-conflict: receipt ID has another durable binding");
    }

    Statement current(m_impl->database,
      "SELECT receipt_id,operation_id,generation,location_version,committed_at_ms,"
      "expires_at_ms,route_state,forwarding_hint FROM artifact_locations WHERE "
      "repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND "
      "content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND "
      "publisher_identity=? AND policy_epoch=?");
    bindText(m_impl->database, current.get(), 1, location.repoNode);
    bindText(m_impl->database, current.get(), 2, location.providerIdentity);
    bindReference(m_impl->database, current.get(), 3, reference);
    const int found = sqlite3_step(current.get());
    if (found == SQLITE_DONE) {
      Statement insert(m_impl->database,
        "INSERT INTO artifact_locations(repo_node,provider_identity,logical_name,"
        "digest_algorithm,content_digest,size_bytes,format_version,root_manifest_name,"
        "publisher_identity,policy_epoch,receipt_id,operation_id,generation,location_version,"
        "committed_at_ms,expires_at_ms,route_state,forwarding_hint) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
      bindText(m_impl->database, insert.get(), 1, location.repoNode);
      bindText(m_impl->database, insert.get(), 2, location.providerIdentity);
      bindReference(m_impl->database, insert.get(), 3, reference);
      bindText(m_impl->database, insert.get(), 11, location.receiptId);
      bindText(m_impl->database, insert.get(), 12, location.operationId);
      bindInt64(m_impl->database, insert.get(), 13, static_cast<int64_t>(location.generation));
      bindInt64(m_impl->database, insert.get(), 14, static_cast<int64_t>(location.locationVersion));
      bindInt64(m_impl->database, insert.get(), 15, static_cast<int64_t>(location.committedAtMs));
      bindInt64(m_impl->database, insert.get(), 16, static_cast<int64_t>(location.expiresAtMs));
      bindText(m_impl->database, insert.get(), 17, location.routeState);
      bindText(m_impl->database, insert.get(), 18, location.forwardingHint);
      if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        throw std::runtime_error(
          std::string("repo-metadata-sql-failed: Persistent location insert: ") +
          sqlite3_errmsg(m_impl->database));
      }
    }
    else if (found == SQLITE_ROW) {
      const auto oldVersion = sqlite3_column_int64(current.get(), 3);
      if (oldVersion <= 0 || columnText(current.get(), 0) != location.receiptId ||
          columnText(current.get(), 1) != location.operationId ||
          sqlite3_column_int64(current.get(), 2) != static_cast<int64_t>(location.generation) ||
          location.locationVersion < static_cast<uint64_t>(oldVersion)) {
        throw std::runtime_error(
          "repo-artifact-location-conflict: receipt binding changed or version rolled back");
      }
      const bool sameVersionSameValue =
        location.locationVersion == static_cast<uint64_t>(oldVersion) &&
        sqlite3_column_int64(current.get(), 4) == static_cast<int64_t>(location.committedAtMs) &&
        sqlite3_column_int64(current.get(), 5) == static_cast<int64_t>(location.expiresAtMs) &&
        columnText(current.get(), 6) == location.routeState &&
        columnText(current.get(), 7) == location.forwardingHint;
      if (location.locationVersion == static_cast<uint64_t>(oldVersion) &&
          !sameVersionSameValue) {
        throw std::runtime_error(
          "repo-artifact-location-conflict: same version carries different location data");
      }
      if (location.locationVersion > static_cast<uint64_t>(oldVersion)) {
        Statement update(m_impl->database,
          "UPDATE artifact_locations SET location_version=?,expires_at_ms=?,"
          "route_state=?,forwarding_hint=? WHERE repo_node=? AND provider_identity=? AND "
          "logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? AND "
          "format_version=? AND root_manifest_name=? AND publisher_identity=? AND policy_epoch=?");
        bindInt64(m_impl->database, update.get(), 1, static_cast<int64_t>(location.locationVersion));
        bindInt64(m_impl->database, update.get(), 2, static_cast<int64_t>(location.expiresAtMs));
        bindText(m_impl->database, update.get(), 3, location.routeState);
        bindText(m_impl->database, update.get(), 4, location.forwardingHint);
        bindText(m_impl->database, update.get(), 5, location.repoNode);
        bindText(m_impl->database, update.get(), 6, location.providerIdentity);
        bindReference(m_impl->database, update.get(), 7, reference);
        if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(m_impl->database) != 1) {
          throw std::runtime_error(
            std::string("repo-metadata-sql-failed: Persistent location update: ") +
            sqlite3_errmsg(m_impl->database));
        }
      }
    }
    else {
      throw std::runtime_error(
        std::string("repo-metadata-sql-failed: Persistent location lookup: ") +
        sqlite3_errmsg(m_impl->database));
    }
    sqlExec(m_impl->database, "COMMIT");
  }
  catch (...) {
    sqlite3_exec(m_impl->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::optional<ArtifactLocationRecord>
SqliteArtifactMetadataStore::lookupPersistentLocation(
  const ArtifactReference& reference, const std::string& repoNode,
  const std::string& providerIdentity, uint64_t nowMs) const
{
  reference.validate();
  validateName(repoNode, ArtifactLimits{}, "repoNode");
  validateName(providerIdentity, ArtifactLimits{}, "providerIdentity");
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (m_impl->migration.databaseSchemaGeneration < 14) return std::nullopt;

  Statement lookup(m_impl->database,
    "SELECT receipt_id,operation_id,generation,location_version,committed_at_ms,"
    "expires_at_ms,route_state,forwarding_hint FROM artifact_locations WHERE "
    "repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND "
    "content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND "
    "publisher_identity=? AND policy_epoch=?");
  bindText(m_impl->database, lookup.get(), 1, repoNode);
  bindText(m_impl->database, lookup.get(), 2, providerIdentity);
  bindReference(m_impl->database, lookup.get(), 3, reference);
  const int status = sqlite3_step(lookup.get());
  if (status == SQLITE_DONE) return std::nullopt;
  if (status != SQLITE_ROW) {
    throw std::runtime_error(
      std::string("repo-metadata-sql-failed: Persistent location lookup: ") +
      sqlite3_errmsg(m_impl->database));
  }
  ArtifactLocationRecord result;
  result.repoNode = repoNode;
  result.providerIdentity = providerIdentity;
  result.reference = reference;
  result.receiptId = columnText(lookup.get(), 0);
  result.operationId = columnText(lookup.get(), 1);
  const auto generation = sqlite3_column_int64(lookup.get(), 2);
  const auto version = sqlite3_column_int64(lookup.get(), 3);
  const auto committedAt = sqlite3_column_int64(lookup.get(), 4);
  const auto expiresAt = sqlite3_column_int64(lookup.get(), 5);
  if (generation <= 0 || version <= 0 || committedAt <= 0 || expiresAt <= 0) {
    throw std::runtime_error(
      "repo-schema-location-invalid: stored location fields are out of range");
  }
  result.generation = static_cast<uint64_t>(generation);
  result.locationVersion = static_cast<uint64_t>(version);
  result.committedAtMs = static_cast<uint64_t>(committedAt);
  result.expiresAtMs = static_cast<uint64_t>(expiresAt);
  result.routeState = columnText(lookup.get(), 6);
  result.forwardingHint = columnText(lookup.get(), 7);
  if (nowMs != 0 && nowMs >= result.expiresAtMs) return std::nullopt;
  result.validate(nowMs);
  return result;
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
