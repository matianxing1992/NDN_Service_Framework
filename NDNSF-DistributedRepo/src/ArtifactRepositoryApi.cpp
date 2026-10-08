#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"

#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf_distributed_repo {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t LOCAL_GENERATION = 1;
constexpr size_t COPY_CHUNK_BYTES = 1024 * 1024;
constexpr uint64_t MAX_STORE_RANGE_BYTES = 16 * 1024 * 1024;

bool
spec192DiscoveryTraceEnabled()
{
  const char* value = std::getenv("NDNSF_SPEC192_SELECTED_REQUEST_TRACE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[noreturn]] void
throwSystem(const std::string& code, const std::string& action,
            const std::string& path)
{
  throw std::runtime_error(
    code + ": " + action + " " + path + ": " + std::strerror(errno));
}

class FileDescriptor
{
public:
  explicit FileDescriptor(int fd = -1) noexcept
    : m_fd(fd)
  {
  }

  ~FileDescriptor()
  {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept
    : m_fd(other.release())
  {
  }

  FileDescriptor& operator=(FileDescriptor&& other) noexcept
  {
    if (this != &other) {
      if (m_fd >= 0) {
        ::close(m_fd);
      }
      m_fd = other.release();
    }
    return *this;
  }

  int get() const noexcept
  {
    return m_fd;
  }

  int release() noexcept
  {
    const int result = m_fd;
    m_fd = -1;
    return result;
  }

private:
  int m_fd;
};

void
validateConfig(const InAppRepoConfig& config)
{
  const ArtifactLimits hardLimits;
  if (config.durableRoot.empty()) {
    throw std::invalid_argument(
      "repo-inapp-invalid-root: durable root is required");
  }
  if (config.ownerId.empty()) {
    throw std::invalid_argument(
      "repo-persistence-invalid-owner: owner ID is required");
  }
  if (config.limits.maxArtifactBytes == 0 ||
      config.limits.maxArtifactBytes > hardLimits.maxArtifactBytes ||
      config.limits.maxChunkBytes == 0 ||
      config.limits.maxChunkBytes > hardLimits.maxChunkBytes ||
      config.limits.maxNameBytes == 0 ||
      config.limits.maxNameBytes > hardLimits.maxNameBytes ||
      config.availabilityFreshnessMs == 0) {
    throw std::invalid_argument(
      "repo-inapp-invalid-limits: configured artifact, chunk, and name limits must fit hard limits");
  }
}

fs::path
prepareRoot(const InAppRepoConfig& config)
{
  validateConfig(config);
  const auto root = fs::absolute(config.durableRoot).lexically_normal();
  std::error_code error;
  const auto status = fs::symlink_status(root, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("repo-inapp-root-stat-failed: " + error.message());
  }
  if (!error && fs::exists(status)) {
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
      throw std::runtime_error(
        "repo-inapp-root-invalid: root must be a non-symlink directory");
    }
    const auto sharedPermissions = fs::perms::group_all | fs::perms::others_all;
    if ((status.permissions() & sharedPermissions) != fs::perms::none) {
      throw std::runtime_error(
        "repo-inapp-root-permissions-invalid: existing root must not be accessible by group or others");
    }
    return root;
  }

  fs::create_directories(root, error);
  if (error) {
    throw std::runtime_error("repo-inapp-root-create-failed: " + error.message());
  }
  fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, error);
  if (error) {
    throw std::runtime_error(
      "repo-inapp-root-permissions-failed: " + error.message());
  }
  return root;
}

ArtifactReference
payloadReference(const ArtifactReference& reference)
{
  auto result = reference;
  std::transform(result.contentDigest.begin(), result.contentDigest.end(),
                 result.contentDigest.begin(), [] (unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return result;
}

std::string
hexDigest(const uint8_t* bytes, size_t size)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return output.str();
}

std::string
finishDigest(EVP_MD_CTX* context)
{
  std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned digestSize = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &digestSize) != 1) {
    throw std::runtime_error(
      "repo-artifact-digest-failed: SHA-256 finalization failed");
  }
  return hexDigest(digest.data(), digestSize);
}

EVP_MD_CTX*
makeSha256Context()
{
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error(
      "repo-artifact-digest-failed: cannot initialize SHA-256");
  }
  return context;
}

bool
sameSnapshot(const struct stat& before, const struct stat& after)
{
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

struct stat
statRegularFile(int fd, const std::string& path, uint64_t expectedSize,
                const char* errorCode)
{
  struct stat status {};
  if (::fstat(fd, &status) != 0) {
    throwSystem(errorCode, "stat", path);
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<uint64_t>(status.st_size) != expectedSize) {
    throw std::runtime_error(
      std::string(errorCode) + ": expected a regular file of the declared size: " + path);
  }
  return status;
}

std::string
hashFileDescriptor(int fd, const std::string& path, uint64_t expectedSize)
{
  const auto before = statRegularFile(
    fd, path, expectedSize, "repo-artifact-file-invalid");
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    makeSha256Context(), EVP_MD_CTX_free);
  std::array<uint8_t, COPY_CHUNK_BYTES> buffer{};
  uint64_t offset = 0;
  while (offset < expectedSize) {
    const auto amount = static_cast<size_t>(
      std::min<uint64_t>(buffer.size(), expectedSize - offset));
    const auto count = ::pread(fd, buffer.data(), amount,
                               static_cast<off_t>(offset));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throwSystem("repo-artifact-read-failed", "read", path);
    }
    if (count == 0) {
      throw std::runtime_error(
        "repo-artifact-source-size-mismatch: source was truncated while reading");
    }
    if (EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<size_t>(count)) != 1) {
      throw std::runtime_error(
        "repo-artifact-digest-failed: SHA-256 update failed");
    }
    offset += static_cast<uint64_t>(count);
  }
  struct stat after {};
  if (::fstat(fd, &after) != 0) {
    throwSystem("repo-artifact-read-failed", "restat", path);
  }
  if (!sameSnapshot(before, after)) {
    throw std::runtime_error(
      "repo-artifact-source-changed: source changed while being verified");
  }
  return finishDigest(context.get());
}

uint64_t
nowMilliseconds()
{
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (value <= 0) {
    throw std::runtime_error("repo-artifact-clock-invalid: system clock is not positive");
  }
  return static_cast<uint64_t>(value);
}

void
copySourceToStore(int sourceFd, const std::string& sourcePath,
                  const ArtifactReference& reference,
                  FilesystemArtifactPayloadStore& store)
{
  const auto payloadRef = payloadReference(reference);
  const auto before = statRegularFile(
    sourceFd, sourcePath, reference.sizeBytes,
    "repo-artifact-source-size-mismatch");
  try {
    store.begin(payloadRef, LOCAL_GENERATION);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      makeSha256Context(), EVP_MD_CTX_free);
    std::array<uint8_t, COPY_CHUNK_BYTES> buffer{};
    uint64_t offset = 0;
    while (offset < reference.sizeBytes) {
      const auto amount = static_cast<size_t>(
        std::min<uint64_t>(buffer.size(), reference.sizeBytes - offset));
      const auto count = ::pread(sourceFd, buffer.data(), amount,
                                 static_cast<off_t>(offset));
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        throwSystem("repo-artifact-read-failed", "read", sourcePath);
      }
      if (count == 0) {
        throw std::runtime_error(
          "repo-artifact-source-size-mismatch: source was truncated while copying");
      }
      const auto countBytes = static_cast<size_t>(count);
      if (EVP_DigestUpdate(context.get(), buffer.data(), countBytes) != 1) {
        throw std::runtime_error(
          "repo-artifact-digest-failed: SHA-256 update failed");
      }
      std::vector<uint8_t> bytes(buffer.begin(), buffer.begin() + countBytes);
      const ArtifactByteRange range{offset, static_cast<uint64_t>(countBytes)};
      store.writeRange(payloadRef, LOCAL_GENERATION, range, bytes);
      store.markVerified(payloadRef, LOCAL_GENERATION, range);
      offset += static_cast<uint64_t>(countBytes);
    }

    struct stat after {};
    if (::fstat(sourceFd, &after) != 0) {
      throwSystem("repo-artifact-read-failed", "restat", sourcePath);
    }
    if (!sameSnapshot(before, after)) {
      throw std::runtime_error(
        "repo-artifact-source-changed: source changed while being copied");
    }
    if (finishDigest(context.get()) != payloadRef.contentDigest) {
      throw std::runtime_error(
        "repo-artifact-source-digest-mismatch: source does not match ArtifactReference");
    }
    store.flush(payloadRef, LOCAL_GENERATION);
    store.finalize(payloadRef, LOCAL_GENERATION);
  }
  catch (...) {
    try {
      store.abort(payloadRef, LOCAL_GENERATION);
    }
    catch (...) {
    }
    throw;
  }
}

void
verifyCommittedPayload(const ArtifactReference& reference,
                       FilesystemArtifactPayloadStore& store)
{
  const auto payloadRef = payloadReference(reference);
  const auto path = store.committedPath(payloadRef);
  const int rawFd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (rawFd < 0) {
    throwSystem("repo-artifact-committed-read-failed", "open", path);
  }
  FileDescriptor fd(rawFd);
  if (hashFileDescriptor(fd.get(), path, reference.sizeBytes) !=
      payloadRef.contentDigest) {
    throw std::runtime_error(
      "repo-artifact-committed-corrupt: committed payload size or digest mismatch");
  }
}

bool
sameReference(const ArtifactReference& lhs, const ArtifactReference& rhs)
{
  return lhs == rhs;
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
      throwSystem("repo-artifact-fetch-write-failed", "write", path);
    }
    if (count == 0) {
      throw std::runtime_error(
        "repo-artifact-fetch-write-failed: short write " + path);
    }
    written += static_cast<size_t>(count);
  }
}

void
fsyncDirectory(const fs::path& directory)
{
  const int rawFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY |
                           O_NOFOLLOW | O_CLOEXEC);
  if (rawFd < 0) {
    throwSystem("repo-artifact-fetch-sync-failed", "open directory",
                directory.string());
  }
  FileDescriptor fd(rawFd);
  if (::fsync(fd.get()) != 0) {
    throwSystem("repo-artifact-fetch-sync-failed", "fsync directory",
                directory.string());
  }
}

class RepoRequestTimeoutError final : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class DestinationRecoveryError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

void
commitFetchedFile(const fs::path& temporary, const fs::path& destination,
                  std::chrono::steady_clock::time_point deadline)
{
  const auto parent = destination.parent_path();
  const fs::path backup = temporary.string() + ".previous";
  bool preserved = false;
  bool installed = false;
  const auto checkDeadline = [&] {
    if (deadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= deadline) {
      throw RepoRequestTimeoutError(
        "repo-artifact-fetch-deadline-exceeded: destination transaction exceeded operation deadline");
    }
  };
  try {
    struct stat previous {};
    if (::lstat(destination.c_str(), &previous) == 0) {
      if (!S_ISREG(previous.st_mode) && !S_ISLNK(previous.st_mode)) {
        throw std::invalid_argument(
          "repo-artifact-destination-invalid: destination must be a file or leaf symlink");
      }
      // link preserves the leaf inode, including a symlink itself, without overwriting a backup.
      if (::link(destination.c_str(), backup.c_str()) != 0) {
        throwSystem("repo-artifact-destination-preserve-failed", "link", destination.string());
      }
      preserved = true;
    }
    else if (errno != ENOENT) {
      throwSystem("repo-artifact-destination-inspect-failed", "lstat", destination.string());
    }
    fsyncDirectory(parent);
    checkDeadline();
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
      throwSystem("repo-artifact-destination-commit-failed", "rename", destination.string());
    }
    installed = true;
    fsyncDirectory(parent);
    checkDeadline();
  }
  catch (...) {
    const auto original = std::current_exception();
    std::string initialFailure;
    try { std::rethrow_exception(original); }
    catch (const std::exception& error) { initialFailure = error.what(); }
    catch (...) { initialFailure = "unknown destination failure"; }
    if (installed) {
      try {
        if (preserved) {
          if (::rename(backup.c_str(), destination.c_str()) != 0) {
            throwSystem("repo-artifact-destination-restore-failed", "rename", backup.string());
          }
          preserved = false;
        }
        else if (::unlink(destination.c_str()) != 0) {
          throwSystem("repo-artifact-destination-restore-failed", "unlink", destination.string());
        }
        fsyncDirectory(parent);
      }
      catch (const std::exception& recovery) {
        throw DestinationRecoveryError(
          "repo-artifact-destination-recovery-required: destination=" + destination.string() +
          "; backup=" + backup.string() + "; " + recovery.what() +
          "; initial=" + initialFailure);
      }
    }
    if (preserved && ::unlink(backup.c_str()) != 0) {
      throw DestinationRecoveryError(
        "repo-artifact-destination-recovery-required: destination=" + destination.string() +
        "; backup=" + backup.string() + "; backup cleanup: " + std::strerror(errno) +
        "; initial=" + initialFailure);
    }
    std::rethrow_exception(original);
  }
  // The verified destination has already been accepted. Cleanup cannot revoke success.
  if (preserved && ::unlink(backup.c_str()) == 0) {
    try { fsyncDirectory(parent); }
    catch (...) {}
  }
}

std::string
makeReceiptId(uint64_t localCommitId)
{
  return "in-app-local-" + std::to_string(localCommitId);
}

std::optional<uint64_t>
parseLocalReceiptId(const std::string& receiptId)
{
  constexpr char PREFIX[] = "in-app-local-";
  if (receiptId.compare(0, sizeof(PREFIX) - 1, PREFIX) != 0) {
    return std::nullopt;
  }
  const auto digits = receiptId.substr(sizeof(PREFIX) - 1);
  if (digits.empty() || digits.front() == '0') return std::nullopt;
  uint64_t id = 0;
  const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || id == 0) {
    return std::nullopt;
  }
  return id;
}

std::vector<SelectedRepoTarget>
canonicalSelectedTargets(const SelectedRepoSet& selected)
{
  auto targets = selected.targets;
  std::sort(targets.begin(), targets.end(), [] (const auto& lhs, const auto& rhs) {
    if (lhs.repoNode != rhs.repoNode) return lhs.repoNode < rhs.repoNode;
    const auto lhsProvider = lhs.providerIdentity.toUri();
    const auto rhsProvider = rhs.providerIdentity.toUri();
    if (lhsProvider != rhsProvider) return lhsProvider < rhsProvider;
    return lhs.servicePrefix.toUri() < rhs.servicePrefix.toUri();
  });
  return targets;
}

std::string
makePersistentOperationId(const ArtifactReference& reference,
                          const SelectedRepoSet& selected,
                          const std::vector<SelectedRepoTarget>& targets)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    makeSha256Context(), EVP_MD_CTX_free);
  const auto update = [&] (const void* data, size_t size) {
    if (EVP_DigestUpdate(context.get(), data, size) != 1) {
      throw std::runtime_error(
        "repo-artifact-operation-id-failed: SHA-256 update failed");
    }
  };
  const auto updateString = [&] (const std::string& value) {
    std::array<uint8_t, 8> length{};
    auto remaining = static_cast<uint64_t>(value.size());
    for (size_t index = length.size(); index > 0; --index) {
      length[index - 1] = static_cast<uint8_t>(remaining & 0xff);
      remaining >>= 8;
    }
    update(length.data(), length.size());
    update(value.data(), value.size());
  };
  const auto updateCount = [&] (uint64_t count) {
    std::array<uint8_t, 8> encoded{};
    for (size_t index = encoded.size(); index > 0; --index) {
      encoded[index - 1] = static_cast<uint8_t>(count & 0xff);
      count >>= 8;
    }
    update(encoded.data(), encoded.size());
  };

  static constexpr char DOMAIN[] =
    "ndnsf-distributed-repo:persistent-operation:v1";
  update(DOMAIN, sizeof(DOMAIN)); // include the NUL domain separator
  updateString(reference.toJson());
  updateString(selected.seedRepoId);
  updateString(toString(selected.consistency));

  updateCount(static_cast<uint64_t>(targets.size()));
  for (const auto& target : targets) {
    updateString(target.repoNode);
    updateString(target.providerIdentity.toUri());
    updateString(target.servicePrefix.toUri());
  }
  return "artifact-v2-" + finishDigest(context.get());
}

int
remainingTimeoutMs(std::chrono::steady_clock::time_point deadline)
{
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now()).count();
  if (remaining <= 0 || remaining > std::numeric_limits<int>::max()) {
    throw std::runtime_error("repo-operation-timeout: operation deadline expired");
  }
  return static_cast<int>(remaining);
}

int
boundedRequestTimeoutMs(std::chrono::steady_clock::time_point deadline,
                         int configuredTimeoutMs)
{
  return std::min(configuredTimeoutMs, remainingTimeoutMs(deadline));
}

std::unique_lock<std::mutex>
lockOperationUntil(std::mutex& operationMutex,
                   std::chrono::steady_clock::time_point deadline,
                   const char* operation)
{
  std::unique_lock<std::mutex> operationGuard(
    operationMutex, std::defer_lock);
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    operationGuard.lock();
    return operationGuard;
  }
  while (!operationGuard.try_lock()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error(
        std::string("repo-operation-timeout: deadline expired waiting for ") +
        operation);
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
      deadline - now, std::chrono::milliseconds(1)));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      std::string("repo-operation-timeout: deadline expired acquiring ") +
      operation);
  }
  return operationGuard;
}

int64_t
statusRecoveryReserveTimeoutMs(const PersistentRepoClientConfig& config)
{
  const auto requestAndAckWindowMs =
    static_cast<int64_t>(config.requestTimeoutMs) +
    static_cast<int64_t>(config.ackCollectionTimeMs);
  const auto twoAckWindowsMs =
    2LL * static_cast<int64_t>(config.ackCollectionTimeMs);
  return std::max<int64_t>(
    {60'000, requestAndAckWindowMs, twoAckWindowsMs});
}

uint64_t
elapsedMilliseconds(std::chrono::steady_clock::time_point startedAt)
{
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - startedAt).count();
  return elapsed <= 0 ? 0 : static_cast<uint64_t>(elapsed);
}

std::string
fetchFailureClass(const std::exception& error)
{
  if (dynamic_cast<const DestinationRecoveryError*>(&error) != nullptr) return "local-io";
  const std::string message(error.what());
  if (message.find("timeout") != std::string::npos ||
      message.find("deadline") != std::string::npos) {
    return "timeout";
  }
  if (message.find("authorization") != std::string::npos ||
      message.find("authenticat") != std::string::npos ||
      message.find("unauthor") != std::string::npos ||
      message.find("permission") != std::string::npos ||
      message.find("not-authorized") != std::string::npos ||
      message.find("denied") != std::string::npos ||
      message.find("forbidden") != std::string::npos) {
    return "authorization";
  }
  if (message.find("provider-mismatch") != std::string::npos ||
      message.find("signer") != std::string::npos) {
    return "identity";
  }
  if (message.find("location") != std::string::npos) {
    return "location";
  }
  if (message.find("digest") != std::string::npos ||
      message.find("range") != std::string::npos ||
      message.find("manifest") != std::string::npos ||
      message.find("corrupt") != std::string::npos) {
    return "integrity";
  }
  if (message.find("destination") != std::string::npos ||
      message.find("write") != std::string::npos ||
      message.find("sync") != std::string::npos) {
    return "local-io";
  }
  return "request-failed";
}

std::vector<uint8_t>
readFileRange(int fd, const std::string& path, uint64_t offset, uint64_t length)
{
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  size_t completed = 0;
  while (completed < bytes.size()) {
    const auto count = ::pread(fd, bytes.data() + completed, bytes.size() - completed,
                               static_cast<off_t>(offset + completed));
    if (count < 0) {
      if (errno == EINTR) continue;
      throwSystem("repo-artifact-source-read-failed", "read", path);
    }
    if (count == 0) {
      throw std::runtime_error("repo-artifact-source-truncated: source changed during publish");
    }
    completed += static_cast<size_t>(count);
  }
  return bytes;
}

void
writeFileRange(int fd, const std::string& path, uint64_t offset,
               const uint8_t* bytes, size_t length)
{
  size_t completed = 0;
  while (completed < length) {
    const auto count = ::pwrite(fd, bytes + completed, length - completed,
                                static_cast<off_t>(offset + completed));
    if (count < 0) {
      if (errno == EINTR) continue;
      throwSystem("repo-artifact-destination-write-failed", "write", path);
    }
    if (count == 0) {
      throw std::runtime_error("repo-artifact-destination-write-failed: short write");
    }
    completed += static_cast<size_t>(count);
  }
}

class PrivateTemporaryDirectory
{
public:
  PrivateTemporaryDirectory()
  {
    auto pattern = (fs::temp_directory_path() / "ndnsf-artifact-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto* path = ::mkdtemp(writable.data());
    if (path == nullptr) {
      throwSystem("repo-artifact-temp-create-failed", "mkdtemp", pattern);
    }
    m_path = path;
  }

  ~PrivateTemporaryDirectory()
  {
    std::error_code ignored;
    fs::remove_all(m_path, ignored);
  }

  const fs::path& path() const noexcept { return m_path; }

private:
  fs::path m_path;
};

ArtifactPublishResult
makePublishResult(const ArtifactLocalCommitRecord& localCommit,
                  bool deduplicated, bool resumed,
                  double totalDurationMs)
{
  const std::string receiptId = makeReceiptId(localCommit.localCommitId);
  const std::string operationId = receiptId;

  ArtifactCommitReceipt receipt;
  receipt.receiptId = receiptId;
  receipt.operationId = operationId;
  receipt.reference = localCommit.reference;
  receipt.generation = LOCAL_GENERATION;
  receipt.lifecycleMode = "IN_APP";
  receipt.placementState = "NOT_APPLICABLE";
  receipt.localCommit = localCommit;
  receipt.writeConsistency = "NOT_APPLICABLE";
  receipt.state = "COMMITTED";
  receipt.completedAtMs = localCommit.committedAtMs;
  receipt.validate();

  ArtifactHandle handle;
  handle.reference = localCommit.reference;
  handle.commitReceiptIds.push_back(receiptId);
  handle.lifecycleMode = "IN_APP";
  handle.validate();

  ArtifactPublishResult result;
  result.reference = localCommit.reference;
  result.operationId = operationId;
  result.deduplicated = deduplicated;
  result.resumed = resumed;
  result.totalDurationMs = totalDurationMs;
  result.handle = std::move(handle);
  result.receipt = std::move(receipt);
  result.validate();
  return result;
}

using detail::JsonValue;

const JsonValue::Object&
objectOf(const JsonValue& value)
{
  return detail::requireObject(value);
}

std::string
jsonString(const JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, JsonValue::Kind::String).string;
}

uint64_t
jsonUnsigned(const JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, JsonValue::Kind::Unsigned).number;
}

uint32_t
jsonU32(const JsonValue::Object& object, const char* key)
{
  const auto value = jsonUnsigned(object, key);
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument(std::string("artifact-json-invalid: integer overflow in ") + key);
  }
  return static_cast<uint32_t>(value);
}

ArtifactReference
parseArtifactReference(const JsonValue& value, const ArtifactLimits& limits)
{
  const auto& object = objectOf(value);
  detail::requireExactKeys(object, {"logicalName", "digestAlgorithm", "contentDigest",
    "sizeBytes", "formatVersion", "rootManifestName", "publisherIdentity", "policyEpoch"});
  ArtifactReference result;
  result.logicalName = jsonString(object, "logicalName");
  result.digestAlgorithm = jsonString(object, "digestAlgorithm");
  result.contentDigest = jsonString(object, "contentDigest");
  result.sizeBytes = jsonUnsigned(object, "sizeBytes");
  result.formatVersion = jsonString(object, "formatVersion");
  result.rootManifestName = jsonString(object, "rootManifestName");
  result.publisherIdentity = jsonString(object, "publisherIdentity");
  result.policyEpoch = jsonString(object, "policyEpoch");
  result.validate(limits);
  return result;
}

std::string
targetJson(const SelectedRepoTarget& target)
{
  return "{\"repoNode\":" + detail::quoteJson(target.repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(target.providerIdentity.toUri()) +
    ",\"servicePrefix\":" + detail::quoteJson(target.servicePrefix.toUri()) + "}";
}

SelectedRepoTarget
parseTarget(const JsonValue& value)
{
  const auto& object = objectOf(value);
  detail::requireExactKeys(object, {"repoNode", "providerIdentity", "servicePrefix"});
  return SelectedRepoTarget{
    jsonString(object, "repoNode"), ndn::Name(jsonString(object, "providerIdentity")),
    ndn::Name(jsonString(object, "servicePrefix")),
  };
}

std::string
replicaJson(const ArtifactReplicaCommit& replica)
{
  return "{\"repoNode\":" + detail::quoteJson(replica.repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(replica.providerIdentity) +
    ",\"receiptId\":" + detail::quoteJson(replica.receiptId) +
    ",\"operationId\":" + detail::quoteJson(replica.operationId) +
    ",\"reference\":" + replica.reference.toJson() +
    ",\"generation\":" + std::to_string(replica.generation) +
    ",\"contentDigest\":" + detail::quoteJson(replica.contentDigest) +
    ",\"persistedBytes\":" + std::to_string(replica.persistedBytes) +
    ",\"completedAtMs\":" + std::to_string(replica.completedAtMs) +
    ",\"locationVersion\":" + std::to_string(replica.locationVersion) +
    ",\"routeState\":" + detail::quoteJson(replica.routeState) +
    ",\"state\":" + detail::quoteJson(replica.state) +
    ",\"failureReason\":" + detail::quoteJson(replica.failureReason) +
    ",\"forwardingHint\":" + detail::quoteJson(replica.forwardingHint) + "}";
}

std::string
makePersistentReceiptId(const ArtifactCommitReceipt& receipt)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    makeSha256Context(), EVP_MD_CTX_free);
  const auto update = [&] (const void* data, size_t size) {
    if (EVP_DigestUpdate(context.get(), data, size) != 1) {
      throw std::runtime_error(
        "repo-artifact-receipt-id-failed: SHA-256 update failed");
    }
  };
  const auto updateString = [&] (const std::string& value) {
    std::array<uint8_t, 8> length{};
    auto remaining = static_cast<uint64_t>(value.size());
    for (size_t index = length.size(); index > 0; --index) {
      length[index - 1] = static_cast<uint8_t>(remaining & 0xff);
      remaining >>= 8;
    }
    update(length.data(), length.size());
    update(value.data(), value.size());
  };
  const auto updateNumber = [&] (uint64_t number) {
    std::array<uint8_t, 8> encoded{};
    for (size_t index = encoded.size(); index > 0; --index) {
      encoded[index - 1] = static_cast<uint8_t>(number & 0xff);
      number >>= 8;
    }
    update(encoded.data(), encoded.size());
  };

  static constexpr char DOMAIN[] =
    "ndnsf-distributed-repo:persistent-receipt:v1";
  update(DOMAIN, sizeof(DOMAIN)); // include the NUL domain separator
  updateNumber(receipt.schemaVersion);
  updateString(receipt.operationId);
  updateString(receipt.reference.toJson());
  updateNumber(receipt.generation);
  updateString(receipt.lifecycleMode);
  updateString(receipt.placementState);
  updateNumber(receipt.requestedReplicas);
  updateNumber(receipt.requiredAcks);
  updateNumber(receipt.achievedReplicas);
  updateString(receipt.writeConsistency);
  updateString(receipt.state);
  updateNumber(receipt.completedAtMs);
  updateNumber(static_cast<uint64_t>(receipt.replicaCommits.size()));
  for (const auto& replica : receipt.replicaCommits) {
    updateString(replica.repoNode);
    updateString(replica.providerIdentity);
    updateString(replica.receiptId);
    updateString(replica.operationId);
    updateString(replica.reference.toJson());
    updateNumber(replica.generation);
    updateString(replica.contentDigest);
    updateNumber(replica.persistedBytes);
    updateNumber(replica.completedAtMs);
    updateNumber(replica.locationVersion);
    updateString(replica.routeState);
    updateString(replica.state);
    updateString(replica.failureReason);
    updateString(replica.forwardingHint);
  }
  return "artifact-commit-" + finishDigest(context.get());
}

ArtifactReplicaCommit
parseReplica(const JsonValue& value, const ArtifactLimits& limits)
{
  const auto& object = objectOf(value);
  detail::requireExactKeys(object, {"repoNode", "providerIdentity", "receiptId", "operationId",
    "reference", "generation", "contentDigest", "persistedBytes", "completedAtMs",
    "locationVersion", "routeState", "state", "failureReason", "forwardingHint"});
  ArtifactReplicaCommit result;
  result.repoNode = jsonString(object, "repoNode");
  result.providerIdentity = jsonString(object, "providerIdentity");
  result.receiptId = jsonString(object, "receiptId");
  result.operationId = jsonString(object, "operationId");
  result.reference = parseArtifactReference(object.at("reference"), limits);
  result.generation = jsonUnsigned(object, "generation");
  result.contentDigest = jsonString(object, "contentDigest");
  result.persistedBytes = jsonUnsigned(object, "persistedBytes");
  result.completedAtMs = jsonUnsigned(object, "completedAtMs");
  result.locationVersion = jsonUnsigned(object, "locationVersion");
  result.routeState = jsonString(object, "routeState");
  result.state = jsonString(object, "state");
  result.failureReason = jsonString(object, "failureReason");
  result.forwardingHint = jsonString(object, "forwardingHint");
  return result;
}

std::string
stringVectorJson(const std::vector<std::string>& values)
{
  std::string result = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) result.push_back(',');
    result += detail::quoteJson(values[i]);
  }
  result.push_back(']');
  return result;
}

std::vector<std::string>
parseStringVector(const JsonValue& value)
{
  if (value.kind != JsonValue::Kind::Array) {
    throw std::invalid_argument("artifact-json-invalid: expected string array");
  }
  std::vector<std::string> result;
  result.reserve(value.array.size());
  for (const auto& item : value.array) {
    if (item.kind != JsonValue::Kind::String) {
      throw std::invalid_argument("artifact-json-invalid: non-string array member");
    }
    result.push_back(item.string);
  }
  return result;
}

struct CallbackResult
{
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  bool timedOut = false;
  bool providerMismatch = false;
  std::string error;
  std::vector<uint8_t> payload;
};

struct FaceRequestStartResult
{
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  bool starting = false;
  bool abandoned = false;
  ndn::Name requestId;
  std::exception_ptr error;
};

class RepoRequestStartError final : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class RepoResponseProviderMismatchError final : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

void
completeWithError(const std::shared_ptr<CallbackResult>& result,
                  const std::string& error,
                  bool timedOut = false,
                  bool providerMismatch = false,
                  std::chrono::steady_clock::time_point deadline =
                    std::chrono::steady_clock::time_point::max(),
                  const std::string& deadlineError = {})
{
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->done) return;
    result->done = true;
    const bool expired = deadline != std::chrono::steady_clock::time_point::max() &&
                         std::chrono::steady_clock::now() >= deadline;
    result->timedOut = timedOut || (expired && !deadlineError.empty());
    result->providerMismatch = providerMismatch && !result->timedOut;
    result->error = expired && !deadlineError.empty() ? deadlineError : error;
  }
  result->condition.notify_all();
}

template<typename StartRequest>
std::vector<uint8_t>
waitForSignedResponse(ndn_service_framework::ServiceUser& serviceUser,
                      const ndn::Name& expectedProvider,
                      StartRequest&& start,
                      std::chrono::steady_clock::time_point waitDeadline =
                        std::chrono::steady_clock::time_point::max())
{
  if (serviceUser.isOnIoThread()) {
    throw RepoRequestStartError(
      "repo-request-on-io-thread: synchronous Persistent requests cannot run on the Face I/O thread");
  }
  auto result = std::make_shared<CallbackResult>();
  auto startState = std::make_shared<FaceRequestStartResult>();
  auto startRequest = std::make_shared<std::decay_t<StartRequest>>(
    std::forward<StartRequest>(start));
  const auto expectedProviderIdentity = expectedProvider.toUri();
  const auto startOnFace = [&serviceUser, result, startState, startRequest,
                            expectedProviderIdentity, waitDeadline] {
    bool expiredBeforeStart = false;
    {
      std::lock_guard<std::mutex> guard(startState->mutex);
      if (startState->abandoned || startState->done) return;
      if (waitDeadline != std::chrono::steady_clock::time_point::max() &&
          std::chrono::steady_clock::now() >= waitDeadline) {
        startState->error = std::make_exception_ptr(RepoRequestTimeoutError(
          "repo-request-timeout: Provider request deadline expired before Face startup"));
        startState->abandoned = true;
        startState->done = true;
        expiredBeforeStart = true;
      }
      else {
        startState->starting = true;
      }
    }
    if (expiredBeforeStart) {
      startState->condition.notify_all();
      return;
    }
    try {
      const auto requestId = (*startRequest)(
        [result] (const ndn::Name&) {
          completeWithError(result,
            "repo-request-timeout: Provider request timed out", true);
        },
        [result, expectedProviderIdentity, waitDeadline] (
            const ndn_service_framework::ResponseMessage& response) {
          const bool providerMatches =
            response.getProviderIdentity() == expectedProviderIdentity;
          const auto responseError = response.getStatus()
            ? std::string() : response.getErrorInfo();
          std::vector<uint8_t> payload;
          if (providerMatches && response.getStatus()) {
            const auto responsePayload = response.getPayload();
            payload.assign(responsePayload.begin(), responsePayload.end());
          }
          {
            std::lock_guard<std::mutex> guard(result->mutex);
            if (result->done) return;
            if (waitDeadline != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= waitDeadline) {
              result->timedOut = true;
              result->error =
                "repo-request-timeout: Provider response callback was not delivered before the request deadline";
            }
            else if (!providerMatches) {
              result->providerMismatch = true;
              result->error =
                "repo-response-provider-mismatch: authenticated response Provider differs from requested Provider";
            }
            else if (!response.getStatus()) {
              result->error = responseError;
            }
            else {
              result->payload = std::move(payload);
            }
            result->done = true;
          }
          result->condition.notify_all();
        });
      bool cancelLateRequest = false;
      {
        std::lock_guard<std::mutex> guard(startState->mutex);
        startState->requestId = requestId;
        if (requestId.empty()) {
          const bool deadlineExpired =
            waitDeadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= waitDeadline;
          if (startState->abandoned || deadlineExpired) {
            startState->abandoned = true;
            startState->error = std::make_exception_ptr(RepoRequestTimeoutError(
              "repo-request-timeout: Provider request deadline expired during Face startup"));
          }
          else {
            startState->error = std::make_exception_ptr(RepoRequestStartError(
              "repo-request-start-failed: Core rejected Provider request"));
          }
        }
        else if (startState->abandoned ||
                 (waitDeadline != std::chrono::steady_clock::time_point::max() &&
                  std::chrono::steady_clock::now() >= waitDeadline)) {
          startState->abandoned = true;
          cancelLateRequest = true;
        }
        startState->starting = false;
        startState->done = true;
      }
      if (cancelLateRequest) {
        try {
          serviceUser.CancelCollaboration(requestId);
        }
        catch (...) {
          // The caller has already reached its absolute deadline.
        }
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> guard(startState->mutex);
      const bool deadlineExpired =
        waitDeadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= waitDeadline;
      if (startState->abandoned || deadlineExpired) {
        startState->abandoned = true;
        startState->error = std::make_exception_ptr(RepoRequestTimeoutError(
          "repo-request-timeout: Provider request deadline expired during Face startup"));
      }
      else {
        startState->error = std::current_exception();
      }
      startState->starting = false;
      startState->done = true;
    }
    startState->condition.notify_all();
  };
  try {
    serviceUser.postToIo(startOnFace);
  }
  catch (const std::exception& error) {
    if (waitDeadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= waitDeadline) {
      throw RepoRequestTimeoutError(
        "repo-request-timeout: Provider request deadline expired while queuing Face startup");
    }
    throw RepoRequestStartError(
      "repo-request-start-failed: could not queue Provider request on Face: " +
      std::string(error.what()));
  }
  catch (...) {
    if (waitDeadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= waitDeadline) {
      throw RepoRequestTimeoutError(
        "repo-request-timeout: Provider request deadline expired while queuing Face startup");
    }
    throw RepoRequestStartError(
      "repo-request-start-failed: Provider request could not be queued on Face");
  }

  ndn::Name requestId;
  std::exception_ptr startError;
  bool startTimedOut = false;
  {
    std::unique_lock<std::mutex> lock(startState->mutex);
    const auto startIsDone = [&] { return startState->done; };
    const bool startCompleted =
      waitDeadline == std::chrono::steady_clock::time_point::max()
        ? (startState->condition.wait(lock, startIsDone), true)
        : startState->condition.wait_until(lock, waitDeadline, startIsDone);
    if (!startCompleted) {
      startState->abandoned = true;
      startTimedOut = true;
    }
    requestId = startState->requestId;
    startError = startState->error;
  }
  const auto cancelRequest = [&serviceUser] (const ndn::Name& id) {
    if (id.empty()) return;
    try {
      serviceUser.postToIo([&serviceUser, id] {
        try {
          serviceUser.CancelCollaboration(id);
        }
        catch (...) {
          // Cancellation is best-effort after the caller's absolute deadline.
        }
      });
    }
    catch (...) {
      // Preserve the bounded caller result if Face shutdown already began.
    }
  };
  if (startTimedOut) {
    cancelRequest(requestId);
    throw RepoRequestTimeoutError(
      "repo-request-timeout: Provider request did not start before its deadline");
  }
  if (startError) {
    try {
      std::rethrow_exception(startError);
    }
    catch (const RepoRequestStartError&) {
      throw;
    }
    catch (const RepoRequestTimeoutError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw RepoRequestStartError(
        "repo-request-start-failed: " + std::string(error.what()));
    }
    catch (...) {
      throw RepoRequestStartError(
        "repo-request-start-failed: Provider request could not be started");
    }
  }
  if (requestId.empty()) {
    throw RepoRequestStartError(
      "repo-request-start-failed: Core rejected Provider request");
  }
  std::unique_lock<std::mutex> lock(result->mutex);
  const auto isDone = [&] { return result->done; };
  const bool completed = waitDeadline == std::chrono::steady_clock::time_point::max()
    ? (result->condition.wait(lock, isDone), true)
    : result->condition.wait_until(lock, waitDeadline, isDone);
  if (!completed) {
    // Core's timeout callback is normally delivered by the caller's Face
    // event loop. Settle the wrapper independently so a stalled or lost
    // callback cannot outlive the operation/request deadline.
    result->done = true;
    result->timedOut = true;
    result->error = "repo-request-timeout: Provider response callback was not delivered before the request deadline";
  }
  const bool timedOut = result->timedOut;
  const bool providerMismatch = result->providerMismatch;
  const std::string error = result->error;
  auto payload = std::move(result->payload);
  lock.unlock();
  if (timedOut) cancelRequest(requestId);
  if (!error.empty()) {
    if (providerMismatch) {
      throw RepoResponseProviderMismatchError(error);
    }
    if (timedOut) throw RepoRequestTimeoutError(error);
    throw std::runtime_error(error);
  }
  return payload;
}

std::vector<uint8_t>
runSelectedRepoRequest(ndn_service_framework::ServiceUser& serviceUser,
                       const SelectedRepoTarget& target,
                       const ndn::Name& serviceName,
                       const std::vector<uint8_t>& requestBytes,
                       const PersistentRepoClientConfig& config,
                       int timeoutMs)
{
  if (timeoutMs <= config.ackCollectionTimeMs) {
    throw RepoRequestTimeoutError(
      "repo-request-timeout: remaining deadline cannot exceed the selected request ACK window");
  }
  return RepoClient::requestSelectedService(
    serviceUser, target.providerIdentity, serviceName, requestBytes,
    config.ackCollectionTimeMs, timeoutMs);
}

SelectedRepoTarget
fromArtifactTarget(const RepoArtifactTarget& target);

PublishOperationStatus
queryPublishOperationStatus(
  ndn_service_framework::ServiceUser& serviceUser,
  const PersistentRepoClientConfig& config,
  const PublishOperationHandle& handle, int timeoutMs)
{
  handle.validate(config.limits);
  if (timeoutMs <= config.ackCollectionTimeMs) {
    throw std::invalid_argument(
      "repo-request-timeout-invalid: status timeout must exceed ACK window");
  }
  const auto seedTarget = fromArtifactTarget(handle.seed);
  const PublishOperationStatusRequest query{1, handle.operationId};
  const auto response = runSelectedRepoRequest(
    serviceUser, seedTarget,
    makeRepoServiceName(seedTarget.servicePrefix, "STATUS"),
    toBytes(query.toJson()), config, timeoutMs);
  const std::string wire(response.begin(), response.end());
  auto status = PublishOperationStatus::fromJson(wire, config.limits);
  if (status.operationId != handle.operationId ||
      status.seed != handle.seed) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "publish status response does not bind the requested operation handle");
  }
  return status;
}

bool
isPublishOperationNotFound(const std::exception& error)
{
  return std::string(error.what()).find(
    "repo-publish-operation-not-found:") != std::string::npos;
}

void
validateAcceptedPublishStatus(const PublishOperationStatus& status,
                              const PublishOperationIntent& intent,
                              const std::string& operationId,
                              const ArtifactLimits& limits)
{
  status.validate(limits);
  bool matchesIntent = status.operationId == operationId &&
    status.reference == intent.reference &&
    status.generation == intent.generation &&
    status.seed == intent.seed &&
    status.consistency == intent.consistency &&
    status.targets.size() == intent.targets.size();
  for (size_t index = 0; matchesIntent && index < intent.targets.size(); ++index) {
    const auto& actual = status.targets[index].target;
    const auto& expected = intent.targets[index];
    matchesIntent = actual.repoNode == expected.repoNode &&
      actual.providerIdentity == expected.providerIdentity &&
      actual.servicePrefix == expected.servicePrefix;
  }
  if (!matchesIntent) {
    throw std::runtime_error(
      "repo-publish-idempotency-conflict: existing operation uses another canonical request");
  }

  const auto seedStatus = std::find_if(
    status.targets.begin(), status.targets.end(), [&intent] (const auto& target) {
      return target.target.repoNode == intent.seed.repoNode &&
        target.target.providerIdentity == intent.seed.providerIdentity &&
        target.target.servicePrefix == intent.seed.servicePrefix;
    });
  if (seedStatus == status.targets.end() || seedStatus->state != "COMMITTED" ||
      !seedStatus->receipt) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "accepted publish status is missing its durable seed receipt");
  }
  const auto& receipt = *seedStatus->receipt;
  receipt.validate(limits);
  if (receipt.repoNode != intent.seed.repoNode ||
      receipt.providerIdentity != intent.seed.providerIdentity ||
      receipt.operationId != operationId ||
      receipt.reference != intent.reference ||
      receipt.generation != intent.generation ||
      receipt.persistedBytes != intent.reference.sizeBytes ||
      receipt.routeState != "ROUTABLE" ||
      receipt.forwardingHint != intent.seed.servicePrefix) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "accepted publish status seed receipt differs from the canonical request");
  }
}

RepoCandidateSet
discoverCandidates(ndn_service_framework::ServiceUser& serviceUser,
                   const PersistentRepoClientConfig& config,
                   const ArtifactCapabilityRequirements* requirements,
                   const ArtifactReference* storageReference,
                   int timeoutMs)
{
  if (timeoutMs <= config.ackCollectionTimeMs) {
    throw std::invalid_argument("repo-request-timeout-invalid: discovery timeout must exceed ACK window");
  }
  if (serviceUser.isOnIoThread()) {
    throw std::logic_error(
      "repo-discovery-on-io-thread: blocking discovery cannot run on the Face I/O thread");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  auto result = std::make_shared<CallbackResult>();
  auto start = std::make_shared<FaceRequestStartResult>();
  auto candidates = std::make_shared<RepoCandidateSet>();
  const auto service = makeRepoServiceName(config.servicePrefix, "CAPABILITY");
  ndn::Buffer emptyPayload;
  const std::optional<ArtifactCapabilityRequirements> requiredCopy = requirements == nullptr
      ? std::nullopt : std::optional<ArtifactCapabilityRequirements>(*requirements);
  const std::optional<ArtifactReference> storageCopy = storageReference == nullptr
      ? std::nullopt : std::optional<ArtifactReference>(*storageReference);
  const auto ackCollectionTimeMs = config.ackCollectionTimeMs;
  const auto providerScope = config.providerScope;
  const auto traceDiscovery = spec192DiscoveryTraceEnabled();
  const auto deadlineError =
    std::string("repo-discovery-timeout: capability discovery deadline expired before ACK closure");
  const auto startOnFace = [&serviceUser, start, result, candidates, service,
                            config, requiredCopy, storageCopy, emptyPayload,
                            ackCollectionTimeMs, providerScope, deadline,
                            deadlineError, traceDiscovery] {
    int invocationTimeoutMs = 0;
    {
      std::lock_guard<std::mutex> guard(start->mutex);
      if (start->abandoned || start->done) return;
      const auto remainingTimeoutMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remainingTimeoutMs <= ackCollectionTimeMs) {
        start->error = std::make_exception_ptr(std::runtime_error(
          "repo-discovery-timeout: capability discovery deadline expired before request start"));
        start->done = true;
      }
      else {
        invocationTimeoutMs = static_cast<int>(remainingTimeoutMs);
        start->starting = true;
      }
    }
    if (invocationTimeoutMs == 0) {
      start->condition.notify_all();
      return;
    }
    try {
      const auto requestId = serviceUser.BeginCollaborationWithProviders(
        service, emptyPayload, ackCollectionTimeMs, invocationTimeoutMs,
        [&serviceUser, result, candidates, service, config, requiredCopy, storageCopy,
         deadline, deadlineError, traceDiscovery]
        (const ndn_service_framework::CollaborationAckClosure& closure) {
          try {
            candidates->observedAtMs = nowMilliseconds();
            candidates->requestId = closure.requestId;
            if (traceDiscovery) {
              const auto request = closure.requestId.toUri();
              std::fprintf(stderr,
                "[SPEC192_CAPABILITY] closure request=%s raw-candidates=%zu\n",
                request.c_str(), closure.candidates.size());
            }
            std::set<std::string> seenProviders;
            for (const auto& candidate : closure.candidates) {
              const auto provider = candidate.providerName.toUri();
              const bool ackStatus = candidate.ack.getStatus();
              const bool serviceMatches = candidate.serviceName == service;
              const bool trustValidated =
                candidate.authenticationEvidence.trustSchemaValidated;
              const auto& signer = candidate.authenticationEvidence.signerIdentity;
              const bool signerMatches = !signer.empty() && signer == provider;
              const auto& ackWireDigest = candidate.authenticationEvidence.wireDigest;
              const auto& signerKeyLocator = candidate.authenticationEvidence.signerKeyLocator;
              const bool hasPacketProvenance =
                !ackWireDigest.empty() && !signerKeyLocator.empty();
              const bool duplicateProvider = seenProviders.find(provider) != seenProviders.end();
              if (!ackStatus || !serviceMatches || !trustValidated ||
                  !signerMatches || !hasPacketProvenance || duplicateProvider) {
                if (traceDiscovery) {
                  const auto candidateService = candidate.serviceName.toUri();
                  std::fprintf(stderr,
                    "[SPEC192_CAPABILITY] provider=%s reject=ack-filter status=%d service=%s service-match=%d trust=%d signer=%s signer-match=%d duplicate=%d\n",
                    provider.c_str(), ackStatus ? 1 : 0, candidateService.c_str(),
                    serviceMatches ? 1 : 0, trustValidated ? 1 : 0,
                    signer.c_str(), signerMatches ? 1 : 0,
                    duplicateProvider ? 1 : 0);
                }
                continue;
              }
              seenProviders.insert(provider);
              try {
                const auto payload = candidate.ack.getPayload();
                const std::string wire(payload.begin(), payload.end());
                auto capability = ArtifactCapabilityAck::fromJson(wire, config.limits).capability;
                if (requiredCopy) {
                  capability.requireSupport(*requiredCopy, config.limits);
                }
                else {
                  if (!storageCopy || capability.signatureAlgorithms.empty() ||
                      !capability.supports(*storageCopy, capability.signatureAlgorithms.front()) ||
                      !capability.supportsResume || !capability.supportsReplicaReceipts) {
                    if (traceDiscovery) {
                      std::fprintf(stderr,
                        "[SPEC192_CAPABILITY] provider=%s reject=storage-capability repo=%s signatures=%zu resume=%d receipts=%d has-reference=%d\n",
                        provider.c_str(), capability.repoNode.c_str(),
                        capability.signatureAlgorithms.size(),
                        capability.supportsResume ? 1 : 0,
                        capability.supportsReplicaReceipts ? 1 : 0,
                        storageCopy ? 1 : 0);
                    }
                    continue;
                  }
                }
                if (traceDiscovery) {
                  std::fprintf(stderr,
                    "[SPEC192_CAPABILITY] provider=%s accept repo=%s\n",
                    provider.c_str(), capability.repoNode.c_str());
                }
                candidates->candidates.push_back(RepoCandidate{
                  capability.repoNode, candidate.providerName, config.servicePrefix,
                  std::move(capability), candidates->observedAtMs,
                  ackWireDigest, signerKeyLocator});
              }
              catch (const std::exception& error) {
                if (traceDiscovery) {
                  std::fprintf(stderr,
                    "[SPEC192_CAPABILITY] provider=%s reject=capability-parse detail=%s\n",
                    provider.c_str(), error.what());
                }
                continue;
              }
            }
            serviceUser.CancelCollaboration(closure.requestId);
            {
              std::lock_guard<std::mutex> guard(result->mutex);
              if (std::chrono::steady_clock::now() >= deadline) {
                result->timedOut = true;
                result->error = deadlineError;
              }
              result->done = true;
            }
            result->condition.notify_all();
          }
          catch (const std::exception& error) {
            serviceUser.CancelCollaboration(closure.requestId);
            completeWithError(result, error.what(), false, false, deadline,
                              deadlineError);
          }
        },
        [result, deadline, deadlineError] (
            const ndn_service_framework::ResponseMessage&) {
          completeWithError(result,
            "repo-discovery-invalid: capability discovery unexpectedly received a final response",
            false, false, deadline, deadlineError);
        },
        [result] (const ndn::Name&) {
          completeWithError(result, "repo-discovery-timeout: capability ACK window timed out");
        },
        ndn::Name(), {}, ndn_service_framework::RequestCapabilities(),
        std::nullopt, {}, {}, {}, providerScope);
      bool cancelLateRequest = false;
      {
        std::lock_guard<std::mutex> guard(start->mutex);
        start->requestId = requestId;
        if (!requestId.empty() &&
            (start->abandoned ||
             std::chrono::steady_clock::now() >= deadline)) {
          start->abandoned = true;
          cancelLateRequest = true;
        }
        if (requestId.empty()) {
          const bool deadlineExpired =
            std::chrono::steady_clock::now() >= deadline;
          if (start->abandoned || deadlineExpired) {
            start->abandoned = true;
            start->error = std::make_exception_ptr(std::runtime_error(
              "repo-discovery-timeout: capability discovery deadline expired during Face startup"));
          }
          else {
            start->error = std::make_exception_ptr(std::runtime_error(
              "repo-discovery-start-failed: Core rejected discovery request"));
          }
        }
        start->starting = false;
        start->done = true;
      }
      if (cancelLateRequest) {
        try {
          serviceUser.CancelCollaboration(requestId);
        }
        catch (...) {
          // The caller has already reached its deadline.
        }
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> guard(start->mutex);
      if (start->abandoned || std::chrono::steady_clock::now() >= deadline) {
        start->abandoned = true;
        start->error = std::make_exception_ptr(std::runtime_error(
          "repo-discovery-timeout: capability discovery deadline expired during Face startup"));
      }
      else {
        start->error = std::current_exception();
      }
      start->starting = false;
      start->done = true;
    }
    start->condition.notify_all();
  };
  try {
    serviceUser.postToIo(startOnFace);
  }
  catch (const std::exception& error) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(
        "repo-discovery-timeout: capability discovery deadline expired while queuing Face startup");
    }
    throw std::runtime_error(
      "repo-discovery-start-failed: could not queue discovery on Face: " +
      std::string(error.what()));
  }
  catch (...) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(
        "repo-discovery-timeout: capability discovery deadline expired while queuing Face startup");
    }
    throw std::runtime_error(
      "repo-discovery-start-failed: capability discovery could not be queued on Face");
  }

  ndn::Name requestId;
  std::exception_ptr startError;
  bool startTimedOut = false;
  {
    std::unique_lock<std::mutex> lock(start->mutex);
    if (!start->condition.wait_until(lock, deadline, [&] { return start->done; })) {
      start->abandoned = true;
      startTimedOut = true;
    }
    requestId = start->requestId;
    startError = start->error;
  }
  const auto cancelRequest = [&serviceUser] (const ndn::Name& id) {
    if (id.empty()) return;
    try {
      serviceUser.postToIo([&serviceUser, id] {
        try {
          serviceUser.CancelCollaboration(id);
        }
        catch (...) {
          // Cancellation is best-effort after the caller's absolute deadline.
        }
      });
    }
    catch (...) {
      // The caller still receives its deadline even if cancellation cannot queue.
    }
  };
  if (startTimedOut) {
    cancelRequest(requestId);
    throw std::runtime_error(
      "repo-discovery-timeout: capability discovery deadline expired before request start");
  }
  if (startError) std::rethrow_exception(startError);
  if (requestId.empty()) {
    throw std::runtime_error("repo-discovery-start-failed: Core rejected discovery request");
  }
  std::unique_lock<std::mutex> lock(result->mutex);
  if (!result->condition.wait_until(lock, deadline, [&] { return result->done; })) {
    lock.unlock();
    cancelRequest(requestId);
    throw std::runtime_error(
      "repo-discovery-timeout: capability discovery deadline expired before ACK closure");
  }
  if (!result->error.empty()) throw std::runtime_error(result->error);
  return std::move(*candidates);
}

std::string
makeFreshPublishIdempotencyKey()
{
  std::array<uint8_t, 32> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error(
      "repo-publish-key-generation-failed: cryptographic random source failed");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string key;
  key.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    key.push_back(HEX[byte >> 4]);
    key.push_back(HEX[byte & 0x0f]);
  }
  return key;
}

RepoArtifactTarget
toArtifactTarget(const SelectedRepoTarget& target)
{
  return RepoArtifactTarget{target.repoNode, target.providerIdentity.toUri(),
                            target.servicePrefix.toUri()};
}

SelectedRepoTarget
fromArtifactTarget(const RepoArtifactTarget& target)
{
  return SelectedRepoTarget{target.repoNode, ndn::Name(target.providerIdentity),
                            ndn::Name(target.servicePrefix)};
}

bool
isTerminalPublishStatus(const PublishOperationStatus& status)
{
  return status.state == "COMMITTED" || status.state == "PARTIAL" ||
         status.state == "FAILED";
}

ArtifactPublishResult
makePublishResult(const PublishOperationHandle& operationHandle,
                  const PublishOperationStatus& status,
                  const ArtifactLimits& limits)
{
  operationHandle.validate(limits);
  status.validate(limits);
  if (operationHandle.operationId != status.operationId ||
      operationHandle.seed != status.seed) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "publish status does not bind the supplied operation handle");
  }

  ArtifactPublishResult result;
  result.reference = status.reference;
  result.operationId = status.operationId;
  result.requestedReplicas = static_cast<uint32_t>(status.targets.size());
  result.operationHandle = operationHandle;
  result.operationStatus = status;

  ArtifactCommitReceipt receipt;
  receipt.operationId = status.operationId;
  receipt.reference = status.reference;
  receipt.generation = status.generation;
  receipt.lifecycleMode = "PERSISTENT";
  receipt.requestedReplicas = static_cast<uint32_t>(status.targets.size());
  receipt.requiredAcks = requiredWriteAcks(receipt.requestedReplicas,
    parseRepoWriteConsistency(status.consistency));
  receipt.writeConsistency = status.consistency;
  receipt.state = status.state;
  receipt.completedAtMs = status.updatedAtMs;

  ArtifactHandle artifactHandle;
  artifactHandle.reference = status.reference;
  artifactHandle.seedRepoId = status.seed.repoNode;
  artifactHandle.writeConsistency = status.consistency;
  artifactHandle.lifecycleMode = "PERSISTENT";
  for (const auto& target : status.targets) {
    artifactHandle.selectedTargets.push_back(fromArtifactTarget(target.target));

    ArtifactReplicaResult replicaResult;
    replicaResult.repoNode = target.target.repoNode;
    replicaResult.providerIdentity = target.target.providerIdentity;

    ArtifactReplicaCommit replica;
    replica.repoNode = target.target.repoNode;
    replica.providerIdentity = target.target.providerIdentity;
    replica.operationId = status.operationId;
    replica.reference = status.reference;
    replica.completedAtMs = status.updatedAtMs;
    if (target.receipt) {
      const auto& commit = *target.receipt;
      replica.state = "COMMITTED";
      replica.receiptId = commit.receiptId;
      replica.generation = commit.generation;
      replica.contentDigest = status.reference.contentDigest;
      replica.persistedBytes = commit.persistedBytes;
      replica.completedAtMs = commit.completedAtMs;
      replica.locationVersion = commit.locationVersion;
      replica.routeState = commit.routeState;
      replica.forwardingHint = commit.forwardingHint;
      replicaResult.state = "COMMITTED";
      replicaResult.receiptId = commit.receiptId;
      ++receipt.achievedReplicas;
      ++result.achievedReplicas;
    }
    else if (target.state == "NOT_ATTEMPTED") {
      replica.state = "SKIPPED";
      replica.routeState = "UNKNOWN";
      replicaResult.state = "SKIPPED";
    }
    else if (target.state == "FAILED" || target.state == "UNKNOWN") {
      replica.state = "FAILED";
      replica.routeState = "UNKNOWN";
      replica.failureReason = target.failureCode.empty()
        ? "repo-target-outcome-unknown" : target.failureCode;
      replicaResult.state = "FAILED";
      replicaResult.errorCode = replica.failureReason;
    }
    else {
      replica.state = target.state;
      replica.routeState = "UNKNOWN";
      replicaResult.state = target.state;
    }
    receipt.replicaCommits.push_back(replica);
    result.replicas.push_back(std::move(replicaResult));
    artifactHandle.replicaCommits.push_back(std::move(replica));
  }

  if (isTerminalPublishStatus(status)) {
    receipt.placementState = receipt.achievedReplicas >= receipt.requiredAcks
      ? "SATISFIED" : receipt.achievedReplicas > 0 ? "PARTIAL" : "UNSATISFIED";
    receipt.receiptId = makeArtifactCommitReceiptId(receipt);
    if (receipt.receiptId != status.receiptId) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "terminal publish status does not bind the reconstructed aggregate receipt");
    }
    artifactHandle.commitReceiptIds.push_back(receipt.receiptId);
    for (const auto& replica : receipt.replicaCommits) {
      if (replica.state == "COMMITTED") {
        artifactHandle.commitReceiptIds.push_back(replica.receiptId);
      }
    }
    artifactHandle.validate(limits);
    receipt.validate(limits);
    result.handle = std::move(artifactHandle);
    result.receipt = std::move(receipt);
  }
  else if (!status.receiptId.empty()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "nonterminal publish status cannot carry a receipt ID");
  }
  result.validate(limits);
  return result;
}

} // namespace

void
SelectedRepoSet::validate(const ndn::Name& expectedServicePrefix,
                          const ArtifactLimits& limits) const
{
  if (targets.empty() || targets.size() > 1024 || expectedServicePrefix.empty()) {
    throw std::invalid_argument("repo-selected-targets-invalid: target set must be non-empty and bounded");
  }
  const auto threshold = requiredWriteAcks(
    static_cast<uint32_t>(targets.size()), consistency);
  (void)threshold;
  std::set<std::string> repos;
  std::set<std::string> providers;
  bool seedFound = false;
  for (const auto& target : targets) {
    validateName(target.repoNode, limits, "selected.repoNode");
    if (target.providerIdentity.empty() || target.servicePrefix != expectedServicePrefix) {
      throw std::invalid_argument("repo-selected-target-invalid: Provider or service prefix mismatch");
    }
    validateName(target.providerIdentity.toUri(), limits, "selected.providerIdentity");
    validateName(target.servicePrefix.toUri(), limits, "selected.servicePrefix");
    if (!repos.insert(target.repoNode).second ||
        !providers.insert(target.providerIdentity.toUri()).second) {
      throw std::invalid_argument("repo-selected-target-invalid: duplicate Repo or Provider identity");
    }
    seedFound = seedFound || target.repoNode == seedRepoId;
  }
  validateName(seedRepoId, limits, "seedRepoId");
  if (!seedFound) {
    throw std::invalid_argument("repo-selected-target-invalid: seed is outside selected set");
  }
}

void
ArtifactHandle::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported artifact handle schema version");
  }
  reference.validate(limits);
  if (lifecycleMode != "IN_APP" && lifecycleMode != "PERSISTENT") {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unknown handle lifecycle mode");
  }
  validateUniqueStrings(commitReceiptIds, 1024, "commitReceiptIds",
                        artifact_error::InvalidReceipt);
  if (lifecycleMode == "IN_APP") {
    if (!selectedTargets.empty() || !replicaCommits.empty() || !seedRepoId.empty() ||
        !writeConsistency.empty()) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "In-App handle cannot claim Persistent placement");
    }
    return;
  }
  if (selectedTargets.empty() || selectedTargets.size() > 1024 ||
      seedRepoId.empty() || writeConsistency.empty()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "Persistent handle is missing its selected placement");
  }
  std::set<std::string> repos;
  std::set<std::string> providers;
  bool hasSeed = false;
  for (const auto& target : selectedTargets) {
    validateName(target.repoNode, limits, "selected.repoNode");
    if (target.providerIdentity.empty()) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "selected Provider identity is required");
    }
    validateName(target.providerIdentity.toUri(), limits, "selected.providerIdentity");
    validateName(target.servicePrefix.toUri(), limits, "selected.servicePrefix");
    if (!repos.insert(target.repoNode).second ||
        !providers.insert(target.providerIdentity.toUri()).second) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent handle has duplicate selected identities");
    }
    hasSeed = hasSeed || target.repoNode == seedRepoId;
  }
  if (!hasSeed || (writeConsistency != "ONE" && writeConsistency != "QUORUM" &&
                   writeConsistency != "ALL")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "Persistent handle has invalid seed or consistency");
  }
  if (replicaCommits.size() != selectedTargets.size()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "Persistent handle receipt entries do not cover selected targets");
  }
  std::set<std::string> replicaRepos;
  std::set<std::string> replicaProviders;
  for (const auto& replica : replicaCommits) {
    validateName(replica.repoNode, limits, "replica.repoNode");
    validateName(replica.providerIdentity, limits, "replica.providerIdentity");
    validateIdentifier(replica.operationId, 256, "replica.operationId",
                       artifact_error::InvalidReceipt);
    replica.reference.validate(limits);
    const bool selectedPair = std::any_of(selectedTargets.begin(), selectedTargets.end(),
      [&replica] (const auto& target) {
        return target.repoNode == replica.repoNode &&
               target.providerIdentity.toUri() == replica.providerIdentity;
      });
    if (!selectedPair || !replicaRepos.insert(replica.repoNode).second ||
        !replicaProviders.insert(replica.providerIdentity).second ||
        replica.reference != reference || replica.completedAtMs == 0 ||
        (replica.state != "COMMITTED" && replica.state != "FAILED" &&
         replica.state != "SKIPPED")) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent handle contains an invalid replica receipt");
    }
    if (replica.state == "COMMITTED") {
      validateIdentifier(replica.receiptId, 256, "replica.receiptId",
                         artifact_error::InvalidReceipt);
      if (replica.generation == 0 || replica.contentDigest != reference.contentDigest ||
          replica.persistedBytes != reference.sizeBytes || !replica.failureReason.empty() ||
          std::find(commitReceiptIds.begin(), commitReceiptIds.end(), replica.receiptId) ==
            commitReceiptIds.end() ||
          (replica.routeState != "ROUTABLE" && replica.routeState != "UNROUTABLE") ||
          (replica.routeState == "ROUTABLE" &&
           (replica.locationVersion == 0 || replica.forwardingHint.empty())) ||
          (replica.routeState == "UNROUTABLE" &&
           (replica.locationVersion != 0 || !replica.forwardingHint.empty()))) {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "Persistent handle contains an invalid committed receipt");
      }
      if (replica.routeState == "ROUTABLE") {
        validateName(replica.forwardingHint, limits, "replica.forwardingHint");
      }
    }
    else if (!replica.receiptId.empty() || replica.generation != 0 ||
             !replica.contentDigest.empty() || replica.persistedBytes != 0 ||
             replica.routeState != "UNKNOWN" || replica.locationVersion != 0 ||
             !replica.forwardingHint.empty() ||
             (replica.state == "FAILED" && replica.failureReason.empty()) ||
             (replica.state == "SKIPPED" && !replica.failureReason.empty())) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "failed handle entry claims commit or route data");
    }
  }
}

std::string
ArtifactHandle::toJson() const
{
  validate();
  std::string selected = "[";
  for (size_t i = 0; i < selectedTargets.size(); ++i) {
    if (i != 0) selected.push_back(',');
    selected += targetJson(selectedTargets[i]);
  }
  selected.push_back(']');
  std::string replicas = "[";
  for (size_t i = 0; i < replicaCommits.size(); ++i) {
    if (i != 0) replicas.push_back(',');
    replicas += replicaJson(replicaCommits[i]);
  }
  replicas.push_back(']');
  return "{\"schemaVersion\":1,\"reference\":" + reference.toJson() +
    ",\"commitReceiptIds\":" + stringVectorJson(commitReceiptIds) +
    ",\"selectedTargets\":" + selected + ",\"replicaCommits\":" + replicas +
    ",\"seedRepoId\":" + detail::quoteJson(seedRepoId) +
    ",\"writeConsistency\":" + detail::quoteJson(writeConsistency) +
    ",\"lifecycleMode\":" + detail::quoteJson(lifecycleMode) + "}";
}

ArtifactHandle
ArtifactHandle::fromJson(const std::string& wire, const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 1024 * 1024);
  const auto& object = objectOf(root);
  detail::requireExactKeys(object, {"schemaVersion", "reference", "commitReceiptIds",
    "selectedTargets", "replicaCommits", "seedRepoId", "writeConsistency", "lifecycleMode"});
  if (jsonUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported artifact handle schema version");
  }
  ArtifactHandle result;
  result.reference = parseArtifactReference(object.at("reference"), limits);
  result.commitReceiptIds = parseStringVector(object.at("commitReceiptIds"));
  const auto& targets = detail::member(object, "selectedTargets", JsonValue::Kind::Array).array;
  for (const auto& target : targets) result.selectedTargets.push_back(parseTarget(target));
  const auto& replicas = detail::member(object, "replicaCommits", JsonValue::Kind::Array).array;
  for (const auto& replica : replicas) result.replicaCommits.push_back(parseReplica(replica, limits));
  result.seedRepoId = jsonString(object, "seedRepoId");
  result.writeConsistency = jsonString(object, "writeConsistency");
  result.lifecycleMode = jsonString(object, "lifecycleMode");
  result.validate(limits);
  return result;
}

void
ArtifactCommitReceipt::validate(const ArtifactLimits& limits) const
{
  validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  if (schemaVersion != 1 || generation == 0 || completedAtMs == 0 ||
      (state != "COMMITTED" && state != "PARTIAL" && state != "FAILED" &&
       state != "ABORTED")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "receipt must be a committed version-1 record with a positive generation and time");
  }
  if (lifecycleMode == "IN_APP") {
    if (placementState != "NOT_APPLICABLE" || requestedReplicas != 0 ||
        requiredAcks != 0 || achievedReplicas != 0 || !replicaCommits.empty() ||
        writeConsistency != "NOT_APPLICABLE" || !localCommit ||
        state != "COMMITTED" ||
        localCommit->localCommitId == 0 || localCommit->committedAtMs == 0 ||
        generation != 1 || localCommit->committedAtMs != completedAtMs ||
        !sameReference(localCommit->reference, reference)) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "In-App receipt must describe only its durable local commit");
    }
  }
  else if (lifecycleMode != "PERSISTENT" || localCommit) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "invalid lifecycle mode or local commit binding");
  }
  else {
    if (requestedReplicas == 0 || replicaCommits.size() != requestedReplicas ||
        requiredAcks == 0 || requiredAcks > requestedReplicas ||
        achievedReplicas > requestedReplicas ||
        (placementState != "SATISFIED" && placementState != "PARTIAL" &&
         placementState != "UNSATISFIED") ||
        (writeConsistency != "ONE" && writeConsistency != "QUORUM" &&
         writeConsistency != "ALL")) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent receipt placement is invalid");
    }
    const auto consistency = parseRepoWriteConsistency(writeConsistency);
    if (requiredWriteAcks(requestedReplicas, consistency) != requiredAcks) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent receipt ACK threshold is inconsistent");
    }
    std::set<std::string> repos;
    std::set<std::string> providers;
    std::set<std::string> receiptIds;
    uint32_t committed = 0;
    for (const auto& replica : replicaCommits) {
      validateName(replica.repoNode, limits, "replica.repoNode");
      validateName(replica.providerIdentity, limits, "replica.providerIdentity");
      if (!repos.insert(replica.repoNode).second ||
          !providers.insert(replica.providerIdentity).second ||
          replica.reference != reference || replica.operationId != operationId ||
          replica.completedAtMs == 0) {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "Persistent receipt identity or reference binding is invalid");
      }
      if (replica.state == "COMMITTED") {
        validateIdentifier(replica.receiptId, 256, "replica.receiptId",
                           artifact_error::InvalidReceipt);
        validateDigest("sha256", replica.contentDigest, "replica digest");
        if (replica.generation != generation ||
            replica.contentDigest != reference.contentDigest ||
            replica.persistedBytes != reference.sizeBytes ||
            !receiptIds.insert(replica.receiptId).second ||
            (replica.routeState != "ROUTABLE" && replica.routeState != "UNROUTABLE") ||
            (replica.routeState == "ROUTABLE" &&
             (replica.locationVersion == 0 || replica.forwardingHint.empty())) ||
            (replica.routeState == "UNROUTABLE" &&
             (replica.locationVersion != 0 || !replica.forwardingHint.empty())) ||
            !replica.failureReason.empty()) {
          throw ArtifactValidationError(
            artifact_error::InvalidReceipt, "committed replica receipt is inconsistent");
        }
        if (replica.routeState == "ROUTABLE") {
          validateName(replica.forwardingHint, limits, "replica.forwardingHint");
        }
        ++committed;
      }
      else if (replica.state == "FAILED" || replica.state == "SKIPPED") {
        if (!replica.receiptId.empty() || replica.generation != 0 ||
            !replica.contentDigest.empty() || replica.persistedBytes != 0 ||
            replica.routeState != "UNKNOWN" || replica.locationVersion != 0 ||
            !replica.forwardingHint.empty() ||
            (replica.state == "FAILED" && replica.failureReason.empty()) ||
            (replica.state == "SKIPPED" && !replica.failureReason.empty())) {
          throw ArtifactValidationError(
            artifact_error::InvalidReceipt, "failed replica cannot claim a commit or route");
        }
      }
      else {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "unknown Persistent replica state");
      }
    }
    if (committed != achievedReplicas) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "achieved replica count differs from committed receipts");
    }
    const std::string expectedPlacement = achievedReplicas >= requiredAcks
      ? "SATISFIED" : achievedReplicas > 0 ? "PARTIAL" : "UNSATISFIED";
    const bool terminalStateMatchesPlacement =
      (state == "COMMITTED" && placementState == "SATISFIED") ||
      (state == "PARTIAL" && placementState == "PARTIAL") ||
      state == "FAILED" || state == "ABORTED";
    if (placementState != expectedPlacement || !terminalStateMatchesPlacement ||
        (state == "PARTIAL" && achievedReplicas == 0)) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent placement and terminal state disagree");
    }
    if (receiptId != makePersistentReceiptId(*this)) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "Persistent receipt ID does not bind its terminal content");
    }
  }
}

std::string
ArtifactCommitReceipt::toJson() const
{
  validate();
  std::string replicas = "[";
  for (size_t i = 0; i < replicaCommits.size(); ++i) {
    if (i != 0) replicas.push_back(',');
    replicas += replicaJson(replicaCommits[i]);
  }
  replicas.push_back(']');
  std::string local = "null";
  if (localCommit) {
    local = "{\"localCommitId\":" + std::to_string(localCommit->localCommitId) +
      ",\"reference\":" + localCommit->reference.toJson() +
      ",\"committedAtMs\":" + std::to_string(localCommit->committedAtMs) + "}";
  }
  return "{\"schemaVersion\":1,\"receiptId\":" + detail::quoteJson(receiptId) +
    ",\"operationId\":" + detail::quoteJson(operationId) +
    ",\"reference\":" + reference.toJson() +
    ",\"generation\":" + std::to_string(generation) +
    ",\"lifecycleMode\":" + detail::quoteJson(lifecycleMode) +
    ",\"placementState\":" + detail::quoteJson(placementState) +
    ",\"requestedReplicas\":" + std::to_string(requestedReplicas) +
    ",\"requiredAcks\":" + std::to_string(requiredAcks) +
    ",\"achievedReplicas\":" + std::to_string(achievedReplicas) +
    ",\"replicaCommits\":" + replicas + ",\"localCommit\":" + local +
    ",\"writeConsistency\":" + detail::quoteJson(writeConsistency) +
    ",\"state\":" + detail::quoteJson(state) +
    ",\"completedAtMs\":" + std::to_string(completedAtMs) + "}";
}

ArtifactCommitReceipt
ArtifactCommitReceipt::fromJson(const std::string& wire,
                                const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 4 * 1024 * 1024);
  const auto& object = objectOf(root);
  detail::requireExactKeys(object, {"schemaVersion", "receiptId", "operationId", "reference",
    "generation", "lifecycleMode", "placementState", "requestedReplicas", "requiredAcks",
    "achievedReplicas", "replicaCommits", "localCommit", "writeConsistency", "state",
    "completedAtMs"});
  if (jsonUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported artifact receipt schema version");
  }
  ArtifactCommitReceipt result;
  result.receiptId = jsonString(object, "receiptId");
  result.operationId = jsonString(object, "operationId");
  result.reference = parseArtifactReference(object.at("reference"), limits);
  result.generation = jsonUnsigned(object, "generation");
  result.lifecycleMode = jsonString(object, "lifecycleMode");
  result.placementState = jsonString(object, "placementState");
  result.requestedReplicas = jsonU32(object, "requestedReplicas");
  result.requiredAcks = jsonU32(object, "requiredAcks");
  result.achievedReplicas = jsonU32(object, "achievedReplicas");
  const auto& replicas = detail::member(object, "replicaCommits", JsonValue::Kind::Array).array;
  for (const auto& replica : replicas) result.replicaCommits.push_back(parseReplica(replica, limits));
  const auto& local = object.at("localCommit");
  if (local.kind != JsonValue::Kind::Null) {
    const auto& commit = objectOf(local);
    detail::requireExactKeys(commit, {"localCommitId", "reference", "committedAtMs"});
    ArtifactLocalCommitRecord record;
    record.localCommitId = jsonUnsigned(commit, "localCommitId");
    record.reference = parseArtifactReference(commit.at("reference"), limits);
    record.committedAtMs = jsonUnsigned(commit, "committedAtMs");
    result.localCommit = std::move(record);
  }
  result.writeConsistency = jsonString(object, "writeConsistency");
  result.state = jsonString(object, "state");
  result.completedAtMs = jsonUnsigned(object, "completedAtMs");
  result.validate(limits);
  return result;
}

void
ArtifactPublishResult::validate(const ArtifactLimits& limits) const
{
  reference.validate(limits);
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  if (achievedReplicas > requestedReplicas ||
      (handle.has_value() != receipt.has_value()) ||
      (requestedReplicas != 0 && replicas.size() != requestedReplicas) ||
      (requestedReplicas == 0 && !replicas.empty())) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "publish result counts or receipt pairing are invalid");
  }
  std::set<std::string> resultRepos;
  std::set<std::string> resultProviders;
  uint32_t reportedCommitted = 0;
  for (const auto& replica : replicas) {
    validateName(replica.repoNode, limits, "result.replica.repoNode");
    validateName(replica.providerIdentity, limits, "result.replica.providerIdentity");
    const bool pendingReplica = replica.state == "QUEUED" ||
      replica.state == "IN_FLIGHT" || replica.state == "STAGED";
    if (!resultRepos.insert(replica.repoNode).second ||
        !resultProviders.insert(replica.providerIdentity).second ||
        (replica.state != "COMMITTED" && replica.state != "FAILED" &&
         replica.state != "SKIPPED" && replica.state != "UNKNOWN" &&
         !pendingReplica) ||
        ((replica.state == "FAILED" || replica.state == "UNKNOWN") &&
         replica.errorCode.empty()) ||
        ((replica.state != "FAILED" && replica.state != "UNKNOWN") &&
         !replica.errorCode.empty()) || (pendingReplica && !operationStatus)) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "publish result contains a duplicate or invalid replica state");
    }
    if (replica.state == "COMMITTED") {
      validateIdentifier(replica.receiptId, 256, "result.replica.receiptId",
                         artifact_error::InvalidReceipt);
      ++reportedCommitted;
    }
    else if (!replica.receiptId.empty()) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "non-committed result entry cannot claim a receipt");
    }
  }
  if (reportedCommitted != achievedReplicas) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "publish result achieved count differs from committed entries");
  }
  if (operationHandle.has_value() != operationStatus.has_value()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "publish result must carry both operation handle and operation status");
  }
  if (operationHandle) {
    operationHandle->validate(limits);
    operationStatus->validate(limits);
    const bool terminal = isTerminalPublishStatus(*operationStatus);
    if (operationHandle->operationId != operationId ||
        operationStatus->operationId != operationId ||
        operationStatus->reference != reference ||
        operationHandle->seed != operationStatus->seed ||
        operationStatus->targets.size() != requestedReplicas ||
        terminal != handle.has_value() ||
        (terminal && operationStatus->receiptId != receipt->receiptId)) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "publish result operation binding or terminal projection is invalid");
    }
    for (size_t index = 0; index < operationStatus->targets.size(); ++index) {
      const auto& target = operationStatus->targets[index];
      const auto& resultReplica = replicas[index];
      const std::string expectedState = target.receipt ? "COMMITTED" :
        target.state == "NOT_ATTEMPTED" ? "SKIPPED" :
        (target.state == "FAILED" || target.state == "UNKNOWN") ? "FAILED" :
        target.state;
      const auto expectedReceiptId = target.receipt
        ? target.receipt->receiptId : std::string();
      const auto expectedError =
        (target.state == "FAILED" || target.state == "UNKNOWN")
          ? (target.failureCode.empty()
               ? std::string("repo-target-outcome-unknown")
               : target.failureCode)
          : std::string();
      if (resultReplica.repoNode != target.target.repoNode ||
          resultReplica.providerIdentity != target.target.providerIdentity ||
          resultReplica.state != expectedState ||
          resultReplica.receiptId != expectedReceiptId ||
          resultReplica.errorCode != expectedError) {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt,
          "publish result target rows differ from authenticated operation status");
      }
    }
  }
  if (handle) {
    handle->validate(limits);
    receipt->validate(limits);
    if (!sameReference(handle->reference, reference) ||
        !sameReference(receipt->reference, reference) ||
        receipt->operationId != operationId ||
        handle->lifecycleMode != receipt->lifecycleMode ||
        std::find(handle->commitReceiptIds.begin(), handle->commitReceiptIds.end(),
                  receipt->receiptId) == handle->commitReceiptIds.end()) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "publish result handle and receipt do not bind the same operation");
    }
    if (receipt->lifecycleMode == "PERSISTENT" &&
        (receipt->replicaCommits.size() != receipt->requestedReplicas ||
         handle->replicaCommits.size() != receipt->replicaCommits.size() ||
         replicas.size() != receipt->requestedReplicas ||
         requestedReplicas != receipt->requestedReplicas ||
         achievedReplicas != receipt->achievedReplicas ||
         handle->seedRepoId.empty() ||
         handle->writeConsistency != receipt->writeConsistency)) {
      throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "Persistent result handle does not preserve its receipt placement");
    }
    if (receipt->lifecycleMode == "PERSISTENT") {
      for (size_t i = 0; i < receipt->replicaCommits.size(); ++i) {
        const auto& persisted = receipt->replicaCommits[i];
        const auto& inHandle = handle->replicaCommits[i];
        const auto& reported = replicas[i];
        if (persisted.repoNode != inHandle.repoNode ||
            persisted.providerIdentity != inHandle.providerIdentity ||
            persisted.operationId != inHandle.operationId ||
            persisted.reference != inHandle.reference ||
            persisted.receiptId != inHandle.receiptId ||
            persisted.generation != inHandle.generation ||
            persisted.contentDigest != inHandle.contentDigest ||
            persisted.persistedBytes != inHandle.persistedBytes ||
            persisted.completedAtMs != inHandle.completedAtMs ||
            persisted.locationVersion != inHandle.locationVersion ||
            persisted.routeState != inHandle.routeState ||
            persisted.state != inHandle.state ||
            persisted.failureReason != inHandle.failureReason ||
            persisted.forwardingHint != inHandle.forwardingHint ||
            reported.repoNode != persisted.repoNode ||
            reported.providerIdentity != persisted.providerIdentity ||
            reported.state != persisted.state || reported.receiptId != persisted.receiptId ||
            ((reported.state == "FAILED") != !reported.errorCode.empty())) {
          throw ArtifactValidationError(
            artifact_error::InvalidReceipt,
            "Persistent publish result entries do not match the durable receipt");
        }
      }
    }
  }
}

struct ArtifactRepositoryApi::Impl
{
  struct InAppOwner
  {
    explicit InAppOwner(InAppRepoConfig config)
      : rootPath(prepareRoot(config))
      , ownerId(std::move(config.ownerId))
      , limits(std::move(config.limits))
      , availabilityFreshnessMs(config.availabilityFreshnessMs)
      , ownership((rootPath / "metadata.sqlite3").string(), ownerId)
      , metadataStore((rootPath / "metadata.sqlite3").string(), true, 15, true)
      , payloadStore(rootPath.string(), MAX_STORE_RANGE_BYTES,
                     metadataStore.artifactWritesEnabled())
    {
    }

    fs::path rootPath;
    std::string ownerId;
    ArtifactLimits limits;
    uint64_t availabilityFreshnessMs;
    BackendOwnershipLease ownership;
    SqliteArtifactMetadataStore metadataStore;
    FilesystemArtifactPayloadStore payloadStore;
  };

  explicit Impl(InAppRepoConfig config)
    : inApp(std::make_unique<InAppOwner>(std::move(config)))
  {
  }

  Impl(ndn_service_framework::ServiceUser& user,
       PersistentRepoClientConfig config)
    : serviceUser(&user)
    , persistentConfig(std::move(config))
  {
    if (persistentConfig->servicePrefix.empty() ||
        persistentConfig->ackCollectionTimeMs <= 0 ||
        persistentConfig->requestTimeoutMs <= persistentConfig->ackCollectionTimeMs ||
        persistentConfig->operationWaitTimeoutMs <= 0 ||
        persistentConfig->fetchFailureCooldownMs < 0 ||
        persistentConfig->fetchFailureCooldownMs > 60000 ||
        persistentConfig->maxFetchHealthEntries == 0 ||
        persistentConfig->maxFetchHealthEntries > 65536) {
      throw std::invalid_argument(
        "repo-persistent-client-config-invalid: deadlines, health limits, or service prefix invalid");
    }
    const ArtifactLimits hardLimits;
    const auto& limits = persistentConfig->limits;
    if (limits.maxArtifactBytes == 0 || limits.maxArtifactBytes > hardLimits.maxArtifactBytes ||
        limits.maxChunkBytes == 0 || limits.maxChunkBytes > hardLimits.maxChunkBytes ||
        limits.maxRootEncodedBytes == 0 || limits.maxRootEncodedBytes > hardLimits.maxRootEncodedBytes ||
        limits.maxPageEncodedBytes == 0 || limits.maxPageEncodedBytes > hardLimits.maxPageEncodedBytes ||
        limits.maxPageEntries == 0 || limits.maxPageEntries > hardLimits.maxPageEntries ||
        limits.maxManifestDepth == 0 || limits.maxManifestDepth > hardLimits.maxManifestDepth ||
        limits.maxCriticalExtensions > hardLimits.maxCriticalExtensions ||
        limits.maxNameBytes == 0 || limits.maxNameBytes > hardLimits.maxNameBytes ||
        limits.maxPacketPayloadBytes == 0 || limits.maxPacketPayloadBytes > hardLimits.maxPacketPayloadBytes ||
        limits.maxSignatureBytes == 0 || limits.maxSignatureBytes > hardLimits.maxSignatureBytes ||
        limits.maxManifestPages == 0 || limits.maxManifestPages > hardLimits.maxManifestPages ||
        limits.maxManifestChunks == 0 || limits.maxManifestChunks > hardLimits.maxManifestChunks ||
        limits.maxCryptographicOperations == 0 ||
        limits.maxCryptographicOperations > hardLimits.maxCryptographicOperations) {
      throw std::invalid_argument(
        "repo-persistent-client-config-invalid: artifact limits must be positive and fit hard limits");
    }
    validateName(persistentConfig->servicePrefix.toUri(),
                 persistentConfig->limits, "servicePrefix");
    std::set<std::string> providers;
    for (const auto& provider : persistentConfig->providerScope) {
      if (provider.empty()) {
        throw std::invalid_argument("repo-persistent-client-config-invalid: provider scope contains an empty name");
      }
      validateName(provider.toUri(), persistentConfig->limits, "providerScope");
      if (!providers.insert(provider.toUri()).second) {
        throw std::invalid_argument("repo-persistent-client-config-invalid: provider scope contains duplicates");
      }
    }
  }

  std::unique_ptr<InAppOwner> inApp;
  ndn_service_framework::ServiceUser* serviceUser = nullptr;
  std::optional<PersistentRepoClientConfig> persistentConfig;

  struct FetchHealthKey
  {
    std::string artifactObjectName;
    std::string providerIdentity;

    bool operator<(const FetchHealthKey& other) const noexcept
    {
      return std::tie(artifactObjectName, providerIdentity) <
             std::tie(other.artifactObjectName, other.providerIdentity);
    }
  };

  struct FetchHealth
  {
    uint64_t locationVersion = 0;
    std::string operationId;
    std::string receiptId;
    double latencyEwmaMs = 0.0;
    uint64_t latencyLocationVersion = 0;
    uint32_t consecutiveFailures = 0;
    uint64_t failureLocationVersion = 0;
    std::chrono::steady_clock::time_point cooldownUntil{};
    std::chrono::steady_clock::time_point lastUpdated{};
  };

  std::map<FetchHealthKey, FetchHealth> fetchHealth;
  mutable std::mutex operationMutex;
};

ArtifactRepositoryApi::ArtifactRepositoryApi(std::unique_ptr<Impl> impl)
  : m_impl(std::move(impl))
{
}

ArtifactRepositoryApi
ArtifactRepositoryApi::makeInApp(InAppRepoConfig config)
{
  return ArtifactRepositoryApi(std::make_unique<Impl>(std::move(config)));
}

ArtifactRepositoryApi
ArtifactRepositoryApi::makePersistent(
  ndn_service_framework::ServiceUser& serviceUser,
  PersistentRepoClientConfig config)
{
  return ArtifactRepositoryApi(
    std::make_unique<Impl>(serviceUser, std::move(config)));
}

ArtifactRepositoryApi::~ArtifactRepositoryApi() = default;
ArtifactRepositoryApi::ArtifactRepositoryApi(ArtifactRepositoryApi&&) noexcept = default;
ArtifactRepositoryApi&
ArtifactRepositoryApi::operator=(ArtifactRepositoryApi&&) noexcept = default;

ArtifactFetchError::ArtifactFetchError(std::string message,
                                       ArtifactFetchResult result)
  : std::runtime_error(std::move(message))
  , m_result(std::move(result))
{
}

const ArtifactFetchResult&
ArtifactFetchError::result() const noexcept
{
  return m_result;
}

RepoCandidateSet
ArtifactRepositoryApi::discoverRepoCandidates(
  const ArtifactCapabilityRequirements& requirements, int timeoutMs) const
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: discovery is Persistent only");
  }
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  requirements.validate(m_impl->persistentConfig->limits);
  return discoverCandidates(*m_impl->serviceUser, *m_impl->persistentConfig,
                            &requirements, nullptr, timeoutMs);
}

ArtifactPublishResult
ArtifactRepositoryApi::publishFile(const std::string& sourcePath,
                                   const ArtifactReference& reference)
{
  if (!m_impl) {
    throw std::logic_error("repo-inapp-api-moved-from: API has no active owner");
  }
  if (!m_impl->inApp) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: publishFile(path, reference) is In-App only");
  }
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  auto& owner = *m_impl->inApp;
  if (!owner.metadataStore.artifactWritesEnabled()) {
    const auto migration = owner.metadataStore.migrationDiagnostics();
    throw std::runtime_error(
      "repo-artifact-writes-disabled: " + migration.reason);
  }
  const auto started = std::chrono::steady_clock::now();
  reference.validate(owner.limits);
  const auto payloadRef = payloadReference(reference);
  const int rawSourceFd = ::open(sourcePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (rawSourceFd < 0) {
    throwSystem("repo-artifact-source-open-failed", "open", sourcePath);
  }
  FileDescriptor sourceFd(rawSourceFd);

  auto existing = owner.metadataStore.lookupLocalCommit(reference);
  const bool payloadExists =
    owner.payloadStore.isCommitted(payloadRef, LOCAL_GENERATION);
  bool resumed = false;
  ArtifactLocalCommitRecord localCommit;

  if (existing) {
    if (!payloadExists) {
      throw std::runtime_error(
        "repo-artifact-commit-payload-missing: historical local commit has no payload");
    }
    if (hashFileDescriptor(sourceFd.get(), sourcePath, reference.sizeBytes) !=
        payloadRef.contentDigest) {
      throw std::runtime_error(
        "repo-artifact-source-digest-mismatch: source does not match ArtifactReference");
    }
    verifyCommittedPayload(reference, owner.payloadStore);
    localCommit = *existing;
  }
  else {
    if (payloadExists) {
      if (hashFileDescriptor(sourceFd.get(), sourcePath, reference.sizeBytes) !=
          payloadRef.contentDigest) {
        throw std::runtime_error(
          "repo-artifact-source-digest-mismatch: source does not match ArtifactReference");
      }
      verifyCommittedPayload(reference, owner.payloadStore);
      resumed = true;
    }
    else {
      copySourceToStore(sourceFd.get(), sourcePath, reference,
                        owner.payloadStore);
    }
    localCommit = owner.metadataStore.recordLocalCommit(reference, nowMilliseconds());
  }

  const auto ended = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double, std::milli>(ended - started).count();
  return makePublishResult(localCommit, existing.has_value(), resumed, elapsed);
}

PublishOperationHandle
ArtifactRepositoryApi::beginPublishFileImpl(
  const std::string& sourcePath, const ArtifactReference& reference,
  const SelectedRepoSet& selected, const std::string& idempotencyKey,
  std::chrono::steady_clock::time_point deadline,
  PublishOperationStatus* acceptedStatus)
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: Persistent publish overload is Persistent only");
  }
  const auto& config = *m_impl->persistentConfig;
  std::unique_lock<std::mutex> operationGuard(
    m_impl->operationMutex, std::defer_lock);
  while (!operationGuard.try_lock()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error(
        "repo-operation-timeout: begin deadline expired waiting for API operation");
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
      deadline - now, std::chrono::milliseconds(1)));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      "repo-operation-timeout: begin deadline expired acquiring API operation");
  }
  selected.validate(config.servicePrefix, config.limits);
  reference.validate(config.limits);
  const auto remaining = [&deadline] { return remainingTimeoutMs(deadline); };
  const auto remainingRequestTimeout = [&] {
    return boundedRequestTimeoutMs(deadline, config.requestTimeoutMs);
  };
  // A seed STORE/COMMIT response can be lost after the seed crosses the
  // acceptance barrier. Keep enough begin budget for one configured STATUS
  // request plus an ACK margin; never extend the deadline for recovery.
  const auto statusRecoveryReserveMs = statusRecoveryReserveTimeoutMs(config);
  const auto remainingForSeedWrite = [&] {
    const int timeout = remaining();
    if (timeout <= statusRecoveryReserveMs + config.ackCollectionTimeMs) {
      throw std::runtime_error(
        "repo-operation-timeout: insufficient begin budget for seed write and status recovery");
    }
    return static_cast<int>(std::min<int64_t>(
      config.requestTimeoutMs,
      static_cast<int64_t>(timeout) - statusRecoveryReserveMs));
  };
  const auto orderedTargets = canonicalSelectedTargets(selected);
  const auto seedTarget = std::find_if(
    orderedTargets.begin(), orderedTargets.end(),
    [&selected] (const auto& target) {
      return target.repoNode == selected.seedRepoId;
    });
  if (seedTarget == orderedTargets.end()) {
    throw std::logic_error("repo-selected-seed-invalid: validated seed was not found");
  }

  PublishOperationIntent intent;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey(idempotencyKey);
  intent.reference = reference;
  intent.generation = 1;
  intent.seed = toArtifactTarget(*seedTarget);
  intent.consistency = toString(selected.consistency);
  for (const auto& target : orderedTargets) {
    intent.targets.push_back(toArtifactTarget(target));
  }
  intent.requestDigest = makePublishRequestDigest(
    intent.reference, intent.generation, intent.seed,
    intent.consistency, intent.targets);
  intent.validate(config.limits);
  const auto operationId = makePublishOperationId(
    m_impl->serviceUser->getName().toUri(), intent.seed,
    intent.idempotencyKeyDigest);
  PublishOperationHandle handle;
  handle.operationId = operationId;
  handle.seed = intent.seed;
  handle.validate(config.limits);

  // Resolve an already accepted operation before requiring every selected
  // replica to appear in current discovery. A retry after a lost return value
  // must be able to recover the seed-owned operation even when a replica is
  // now unavailable.
  try {
    auto previous = queryPublishOperationStatus(
      *m_impl->serviceUser, config, handle, remainingRequestTimeout());
    validateAcceptedPublishStatus(previous, intent, operationId, config.limits);
    if (acceptedStatus != nullptr) {
      *acceptedStatus = std::move(previous);
    }
    return handle;
  }
  catch (const std::exception& error) {
    if (!isPublishOperationNotFound(error)) {
      throw;
    }
  }

  if (sourcePath.empty()) {
    throw std::invalid_argument("repo-artifact-source-invalid: source path is required");
  }
  const int rawSourceFd = ::open(sourcePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (rawSourceFd < 0) {
    throwSystem("repo-artifact-source-open-failed", "open", sourcePath);
  }
  FileDescriptor sourceFd(rawSourceFd);
  (void)statRegularFile(sourceFd.get(), sourcePath, reference.sizeBytes,
                        "repo-artifact-source-size-mismatch");
  const auto payloadRef = payloadReference(reference);
  if (hashFileDescriptor(sourceFd.get(), sourcePath, reference.sizeBytes) !=
      payloadRef.contentDigest) {
    throw std::runtime_error(
      "repo-artifact-source-digest-mismatch: source does not match ArtifactReference");
  }

  const auto candidates = discoverCandidates(
    *m_impl->serviceUser, config, nullptr, &reference,
    remainingRequestTimeout());
  for (const auto& target : orderedTargets) {
    const auto found = std::find_if(
      candidates.candidates.begin(), candidates.candidates.end(),
      [&target] (const auto& candidate) {
        return candidate.repoNode == target.repoNode &&
               candidate.providerIdentity == target.providerIdentity &&
               candidate.servicePrefix == target.servicePrefix;
      });
    if (found == candidates.candidates.end()) {
      if (spec192DiscoveryTraceEnabled()) {
        const auto provider = target.providerIdentity.toUri();
        const auto servicePrefix = target.servicePrefix.toUri();
        std::fprintf(stderr,
          "[SPEC192_CAPABILITY] selected-missing repo=%s provider=%s service=%s discovered=%zu\n",
          target.repoNode.c_str(), provider.c_str(), servicePrefix.c_str(),
          candidates.candidates.size());
        for (const auto& candidate : candidates.candidates) {
          const auto observedProvider = candidate.providerIdentity.toUri();
          std::fprintf(stderr,
            "[SPEC192_CAPABILITY] selected-observed repo=%s provider=%s service=%s\n",
            candidate.repoNode.c_str(), observedProvider.c_str(),
            candidate.servicePrefix.toUri().c_str());
        }
      }
      throw std::runtime_error(
        "repo-selected-target-stale: a selected Repo/Provider tuple is absent from fresh authenticated discovery");
    }
  }

  const auto intentResponse = runSelectedRepoRequest(
    *m_impl->serviceUser, *seedTarget,
    makeRepoServiceName(seedTarget->servicePrefix, "STORE"),
    toBytes(intent.toJson()), config, remainingRequestTimeout());
  const std::string acceptedOperationId(intentResponse.begin(),
                                        intentResponse.end());
  if (acceptedOperationId != operationId) {
    throw std::runtime_error(
      "repo-publish-intent-binding-failed: selected seed returned another operation ID");
  }

  const auto chunkBytes = std::min<uint64_t>(
    {COPY_CHUNK_BYTES, MAX_STORE_RANGE_BYTES,
     config.limits.maxChunkBytes,
     std::max<uint64_t>(1, config.limits.maxPacketPayloadBytes / 2)});
  if (chunkBytes == 0) {
    throw std::invalid_argument(
      "repo-artifact-chunk-limit-invalid: range size must be positive");
  }

  bool rangeTransferStarted = false;
  std::vector<uint8_t> response;
  RepoArtifactCommitReceipt seedReceipt;
  try {
    if (reference.sizeBytes <= chunkBytes) {
      ArtifactStoreRequest write;
      write.operationId = operationId;
      write.generation = intent.generation;
      write.reference = reference;
      write.bytes = readFileRange(sourceFd.get(), sourcePath, 0,
                                  reference.sizeBytes);
      response = runSelectedRepoRequest(
        *m_impl->serviceUser, *seedTarget,
        makeRepoServiceName(seedTarget->servicePrefix, "STORE"),
        write.toWire(config.limits), config, remainingForSeedWrite());
    }
    else {
      uint64_t offset = 0;
      while (offset < reference.sizeBytes) {
        const auto length = std::min<uint64_t>(
          chunkBytes, reference.sizeBytes - offset);
        ArtifactRangeWriteRequest write;
        write.operationId = operationId;
        write.generation = intent.generation;
        write.reference = reference;
        write.offsetBytes = offset;
        write.lengthBytes = length;
        write.bytes = readFileRange(sourceFd.get(), sourcePath, offset, length);
        rangeTransferStarted = true;
        const auto stageResponse = runSelectedRepoRequest(
          *m_impl->serviceUser, *seedTarget,
          makeRepoServiceName(seedTarget->servicePrefix, "STORE_RANGE"),
          write.toWire(config.limits), config, remainingForSeedWrite());
        if (std::string(stageResponse.begin(), stageResponse.end()) != "staged") {
          throw std::runtime_error(
            "repo-artifact-range-stage-invalid: selected seed returned an unexpected stage response");
        }
        offset += length;
      }
      ArtifactRangeCommitRequest commit;
      commit.operationId = operationId;
      commit.generation = intent.generation;
      commit.reference = reference;
      response = runSelectedRepoRequest(
        *m_impl->serviceUser, *seedTarget,
        makeRepoServiceName(seedTarget->servicePrefix, "COMMIT_RANGES"),
        commit.toWire(config.limits), config, remainingForSeedWrite());
    }

    const std::string receiptWire(response.begin(), response.end());
    seedReceipt = RepoArtifactCommitReceipt::fromJson(receiptWire, config.limits);
    seedReceipt.validate(config.limits);
    if (seedReceipt.repoNode != seedTarget->repoNode ||
        seedReceipt.providerIdentity != seedTarget->providerIdentity.toUri() ||
        seedReceipt.operationId != operationId ||
        seedReceipt.reference != reference ||
        seedReceipt.generation != intent.generation ||
        seedReceipt.persistedBytes != reference.sizeBytes ||
        seedReceipt.routeState != "ROUTABLE" ||
        seedReceipt.forwardingHint != seedTarget->servicePrefix.toUri()) {
      throw std::runtime_error(
        "repo-artifact-seed-receipt-binding-failed: seed receipt differs from the accepted intent");
    }
  }
  catch (...) {
    const auto seedWriteFailure = std::current_exception();
    // A failed/lost response is ambiguous: the seed may have committed its
    // receipt and ACCEPTED transition before the response was lost. Confirm
    // that durable barrier through the authenticated seed status service
    // before deciding whether the operation has no accepted handle.
    try {
      auto accepted = queryPublishOperationStatus(
        *m_impl->serviceUser, config, handle, remainingRequestTimeout());
      validateAcceptedPublishStatus(accepted, intent, operationId, config.limits);
      if (acceptedStatus != nullptr) {
        *acceptedStatus = std::move(accepted);
      }
      return handle;
    }
    catch (const std::exception& statusError) {
      const std::string message = statusError.what();
      if (message.find("repo-publish-idempotency-conflict:") != std::string::npos) {
        throw;
      }
      // A missing status means the seed did not cross the acceptance barrier.
      // Other status-query failures cannot prove acceptance, so preserve the
      // original STORE/COMMIT failure for the caller.
    }
    catch (...) {
      // Preserve the original seed-write failure when status cannot be read.
    }

    if (rangeTransferStarted) {
      try {
        ArtifactRangeAbortRequest abort;
        abort.operationId = operationId;
        abort.generation = intent.generation;
        abort.reference = reference;
        (void)runSelectedRepoRequest(
          *m_impl->serviceUser, *seedTarget,
          makeRepoServiceName(seedTarget->servicePrefix, "ABORT_RANGES"),
          abort.toWire(config.limits), config, remainingRequestTimeout());
      }
      catch (...) {
        // The Repo's range reservation reaper remains the bounded cleanup path.
      }
    }
    std::rethrow_exception(seedWriteFailure);
  }

  PublishOperationStatus accepted;
  accepted.operationId = operationId;
  accepted.reference = reference;
  accepted.generation = intent.generation;
  accepted.seed = intent.seed;
  accepted.consistency = intent.consistency;
  accepted.state = "ACCEPTED";
  accepted.updatedAtMs = seedReceipt.completedAtMs;
  for (const auto& target : intent.targets) {
    PublishOperationTargetStatus targetStatus;
    targetStatus.target = target;
    targetStatus.state = target == intent.seed ? "COMMITTED" : "QUEUED";
    targetStatus.attemptCount = target == intent.seed ? 1 : 0;
    targetStatus.updatedAtMs = seedReceipt.completedAtMs;
    if (target == intent.seed) {
      targetStatus.receipt = seedReceipt;
    }
    accepted.targets.push_back(std::move(targetStatus));
  }
  accepted.validate(config.limits);
  if (acceptedStatus != nullptr) {
    *acceptedStatus = std::move(accepted);
  }
  return handle;
}

PublishOperationHandle
ArtifactRepositoryApi::beginPublishFile(
  const std::string& sourcePath, const ArtifactReference& reference,
  const SelectedRepoSet& selected, const std::string& idempotencyKey,
  int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: Persistent publish overload is Persistent only");
  }
  if (timeoutMs <= m_impl->persistentConfig->ackCollectionTimeMs) {
    throw std::invalid_argument("repo-request-timeout-invalid: begin timeout must exceed ACK window");
  }
  return beginPublishFileImpl(sourcePath, reference, selected, idempotencyKey,
                             deadline, nullptr);
}

PublishOperationHandle
ArtifactRepositoryApi::beginArchive(
  const ArtifactRepositoryApi& inAppSource, const ArtifactHandle& handle,
  const SelectedRepoSet& selected, const std::string& idempotencyKey,
  int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  if (!m_impl || m_impl->serviceUser == nullptr ||
      !m_impl->persistentConfig) {
    throw std::logic_error(
      "repo-artifact-lifecycle-mismatch: archive destination must be Persistent");
  }
  if (timeoutMs <= m_impl->persistentConfig->ackCollectionTimeMs) {
    throw std::invalid_argument("repo-request-timeout-invalid: begin timeout must exceed ACK window");
  }
  if (!inAppSource.m_impl || !inAppSource.m_impl->inApp) {
    throw std::logic_error(
      "repo-artifact-lifecycle-mismatch: archive source must be In-App");
  }
  handle.validate(inAppSource.m_impl->inApp->limits);
  if (handle.lifecycleMode != "IN_APP" || handle.commitReceiptIds.size() != 1) {
    throw std::invalid_argument(
      "repo-artifact-archive-source-invalid: archive requires one In-App receipt");
  }
  const auto localCommit = inAppSource.lookupLocalCommitUntil(
    handle.reference, deadline);
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      "repo-operation-timeout: begin deadline expired verifying In-App archive source");
  }
  if (!localCommit ||
      handle.commitReceiptIds.front() != makeReceiptId(localCommit->localCommitId)) {
    throw std::runtime_error(
      "repo-artifact-archive-source-stale: saved handle does not match a durable In-App commit");
  }

  PrivateTemporaryDirectory temporaryDirectory;
  const auto temporaryPayload = temporaryDirectory.path() / "verified-source.bin";
  inAppSource.fetchFileUntil(handle.reference, temporaryPayload.string(), deadline);
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      "repo-operation-timeout: begin deadline expired materializing In-App archive source");
  }
  return beginPublishFileImpl(temporaryPayload.string(), handle.reference,
                              selected, idempotencyKey, deadline, nullptr);
}

PublishOperationStatus
ArtifactRepositoryApi::getPublishStatus(
  const PublishOperationHandle& handle, int timeoutMs) const
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error(
      "repo-artifact-lifecycle-mismatch: publish status is Persistent only");
  }
  const auto& config = *m_impl->persistentConfig;
  if (timeoutMs < 0) {
    throw std::invalid_argument(
      "repo-request-timeout-invalid: status timeout cannot be negative");
  }
  const int requestTimeout = timeoutMs == 0 ? config.requestTimeoutMs : timeoutMs;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(requestTimeout);
  return getPublishStatusUntil(handle, deadline, requestTimeout);
}

PublishOperationStatus
ArtifactRepositoryApi::getPublishStatusUntil(
  const PublishOperationHandle& handle,
  std::chrono::steady_clock::time_point deadline,
  int requestTimeoutMs) const
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error(
      "repo-artifact-lifecycle-mismatch: publish status is Persistent only");
  }
  const auto& config = *m_impl->persistentConfig;
  handle.validate(config.limits);
  std::unique_lock<std::mutex> operationGuard(
    m_impl->operationMutex, std::defer_lock);
  while (!operationGuard.try_lock()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error(
        "repo-operation-timeout: status deadline expired waiting for API operation");
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
      deadline - now, std::chrono::milliseconds(1)));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      "repo-operation-timeout: status deadline expired acquiring API operation");
  }
  const int statusTimeoutMs = boundedRequestTimeoutMs(deadline, requestTimeoutMs);
  if (requestTimeoutMs > config.ackCollectionTimeMs &&
      statusTimeoutMs <= config.ackCollectionTimeMs) {
    throw std::runtime_error(
      "repo-operation-timeout: status deadline leaves insufficient ACK window after API lock wait");
  }
  return queryPublishOperationStatus(
    *m_impl->serviceUser, config, handle, statusTimeoutMs);
}

ArtifactPublishResult
ArtifactRepositoryApi::publishFile(const std::string& sourcePath,
                                   const ArtifactReference& reference,
                                   const SelectedRepoSet& selected,
                                   int timeoutMs)
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: Persistent publish overload is Persistent only");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto& config = *m_impl->persistentConfig;
  if (timeoutMs < 0) {
    throw std::invalid_argument(
      "repo-request-timeout-invalid: publish wait budget cannot be negative");
  }
  const int waitBudget = timeoutMs == 0
    ? config.operationWaitTimeoutMs : timeoutMs;
  reference.validate(config.limits);
  const int64_t ackWindowMs = config.ackCollectionTimeMs;
  const int64_t requestWindowMs =
    static_cast<int64_t>(config.requestTimeoutMs) + ackWindowMs;
  const int64_t statusRecoveryReserveMs =
    statusRecoveryReserveTimeoutMs(config);
  const auto chunkBytes = std::min<uint64_t>(
    {COPY_CHUNK_BYTES, MAX_STORE_RANGE_BYTES,
     config.limits.maxChunkBytes,
     std::max<uint64_t>(1, config.limits.maxPacketPayloadBytes / 2)});
  uint64_t seedWriteRequests = 1;
  if (reference.sizeBytes > chunkBytes) {
    const uint64_t rangeChunks = reference.sizeBytes / chunkBytes +
      (reference.sizeBytes % chunkBytes != 0 ? 1 : 0);
    seedWriteRequests = rangeChunks + 1; // STORE_RANGE chunks plus COMMIT_RANGES
  }
  // The synchronous timeout is only the post-accept status-wait budget. Give
  // the shared begin path its own bounded window for STATUS, discovery,
  // PUBLISH_INTENT, the seed write(s), and status recovery. One extra request
  // window covers local source verification and orchestration overhead.
  constexpr uint64_t PRE_SEED_REQUESTS = 3; // STATUS, discovery, PUBLISH_INTENT
  constexpr uint64_t LOCAL_WORK_MARGIN_WINDOWS = 1;
  const uint64_t requestWindows =
    PRE_SEED_REQUESTS + seedWriteRequests + LOCAL_WORK_MARGIN_WINDOWS;
  const int64_t maxBeginTimeoutMs = std::numeric_limits<int>::max();
  if (requestWindows == 0 || requestWindowMs <= 0 ||
      statusRecoveryReserveMs >= maxBeginTimeoutMs ||
      requestWindows > static_cast<uint64_t>(
        (maxBeginTimeoutMs - statusRecoveryReserveMs) / requestWindowMs)) {
    throw std::invalid_argument(
      "repo-request-timeout-invalid: synchronous begin budget exceeds supported range");
  }
  const int beginTimeout = static_cast<int>(
    static_cast<int64_t>(requestWindows) * requestWindowMs +
    statusRecoveryReserveMs);
  PublishOperationStatus status;
  const auto beginDeadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(beginTimeout);
  const auto handle = beginPublishFileImpl(
    sourcePath, reference, selected, makeFreshPublishIdempotencyKey(),
    beginDeadline, &status);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(waitBudget);
  while (!isTerminalPublishStatus(status)) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= config.ackCollectionTimeMs) break;
    try {
      status = getPublishStatusUntil(handle, deadline, config.requestTimeoutMs);
    }
    catch (const std::exception& error) {
      const std::string message = error.what();
      const bool unavailable =
        message.find("repo-request-timeout:") == 0 ||
        message.find("repo-request-start-failed:") == 0 ||
        message.find("repo-operation-timeout:") == 0;
      if (!unavailable) throw;
      // Seed acceptance is durable. A status timeout leaves a reconnectable
      // handle and the last authenticated nonterminal status available.
      break;
    }
    if (!isTerminalPublishStatus(status)) {
      const auto afterQuery = std::chrono::steady_clock::now();
      if (afterQuery < deadline) {
        std::this_thread::sleep_for(std::min(
          std::chrono::milliseconds(50),
          std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - afterQuery)));
      }
    }
  }
  auto result = makePublishResult(handle, status, config.limits);
  result.totalDurationMs = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - started).count();
  result.validate(config.limits);
  return result;
}

ArtifactPublishResult
ArtifactRepositoryApi::archive(const ArtifactRepositoryApi& inAppSource,
                               const ArtifactHandle& handle,
                               const SelectedRepoSet& selected, int timeoutMs)
{
  if (!m_impl || m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: archive destination must be Persistent");
  }
  if (!inAppSource.m_impl || !inAppSource.m_impl->inApp) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: archive source must be In-App");
  }
  handle.validate(inAppSource.m_impl->inApp->limits);
  if (handle.lifecycleMode != "IN_APP" || handle.commitReceiptIds.size() != 1) {
    throw std::invalid_argument("repo-artifact-archive-source-invalid: archive requires one In-App receipt");
  }
  const auto localCommit = inAppSource.lookupLocalCommit(handle.reference);
  if (!localCommit || handle.commitReceiptIds.front() != makeReceiptId(localCommit->localCommitId)) {
    throw std::runtime_error(
      "repo-artifact-archive-source-stale: saved handle does not match a durable In-App commit");
  }

  PrivateTemporaryDirectory temporaryDirectory;
  const auto temporaryPayload = temporaryDirectory.path() / "verified-source.bin";
  inAppSource.fetchFile(handle.reference, temporaryPayload.string());
  return publishFile(temporaryPayload.string(), handle.reference, selected, timeoutMs);
}

void
ArtifactRepositoryApi::fetchFile(const ArtifactReference& reference,
                                 const std::string& destinationPath) const
{
  fetchFileUntil(reference, destinationPath,
                 std::chrono::steady_clock::time_point::max());
}

void
ArtifactRepositoryApi::fetchFileUntil(
  const ArtifactReference& reference, const std::string& destinationPath,
  std::chrono::steady_clock::time_point deadline) const
{
  if (!m_impl) {
    throw std::logic_error("repo-inapp-api-moved-from: API has no active owner");
  }
  if (!m_impl->inApp) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: fetchFile(reference) is In-App only");
  }
  auto operationGuard = lockOperationUntil(
    m_impl->operationMutex, deadline, "In-App fetch operation");
  auto& owner = *m_impl->inApp;
  reference.validate(owner.limits);
  if (destinationPath.empty()) {
    throw std::invalid_argument(
      "repo-artifact-destination-invalid: destination path is required");
  }
  const auto localCommit = owner.metadataStore.lookupLocalCommit(reference);
  if (!localCommit) {
    throw std::runtime_error(
      "repo-artifact-not-found: no durable local commit matches the full reference");
  }
  const auto payloadRef = payloadReference(reference);
  if (!owner.payloadStore.isCommitted(payloadRef, LOCAL_GENERATION)) {
    throw std::runtime_error(
      "repo-artifact-payload-missing: committed local payload is unavailable");
  }
  if (deadline != std::chrono::steady_clock::time_point::max() &&
      std::chrono::steady_clock::now() >= deadline) {
    throw std::runtime_error(
      "repo-operation-timeout: fetch deadline expired before In-App materialization");
  }

  const fs::path destination = fs::absolute(destinationPath).lexically_normal();
  if (destination.filename().empty()) {
    throw std::invalid_argument(
      "repo-artifact-destination-invalid: destination must name a file");
  }
  auto parent = destination.parent_path();
  if (parent.empty()) {
    parent = fs::current_path();
  }
  std::error_code directoryError;
  fs::create_directories(parent, directoryError);
  if (directoryError || !fs::is_directory(parent)) {
    throw std::runtime_error(
      "repo-artifact-destination-directory-failed: " +
      (directoryError ? directoryError.message() : parent.string()));
  }

  static std::atomic<uint64_t> sequence{0};
  fs::path temporary;
  FileDescriptor temporaryFd;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    temporary = parent /
      ("." + destination.filename().string() + ".ndnsf-" +
       std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
       std::to_string(sequence.fetch_add(1)) + ".tmp");
    const int rawFd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                             O_NOFOLLOW | O_CLOEXEC, 0600);
    if (rawFd >= 0) {
      temporaryFd = FileDescriptor(rawFd);
      break;
    }
    if (errno != EEXIST) {
      throwSystem("repo-artifact-destination-open-failed", "open", temporary.string());
    }
  }
  if (temporaryFd.get() < 0) {
    throw std::runtime_error(
      "repo-artifact-destination-open-failed: could not allocate a unique temporary file");
  }

  bool renamed = false;
  try {
    const auto payloadPath = owner.payloadStore.committedPath(payloadRef);
    const int rawPayloadFd = ::open(payloadPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (rawPayloadFd < 0) {
      throwSystem("repo-artifact-payload-open-failed", "open", payloadPath);
    }
    FileDescriptor payloadFd(rawPayloadFd);
    const auto before = statRegularFile(
      payloadFd.get(), payloadPath, reference.sizeBytes,
      "repo-artifact-payload-size-mismatch");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      makeSha256Context(), EVP_MD_CTX_free);
    std::array<uint8_t, COPY_CHUNK_BYTES> buffer{};
    uint64_t offset = 0;
    while (offset < reference.sizeBytes) {
      const auto amount = static_cast<size_t>(std::min<uint64_t>(
        buffer.size(), reference.sizeBytes - offset));
      const auto count = ::pread(payloadFd.get(), buffer.data(), amount,
                                 static_cast<off_t>(offset));
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        throwSystem("repo-artifact-payload-read-failed", "read", payloadPath);
      }
      if (count == 0) {
        throw std::runtime_error(
          "repo-artifact-truncated: committed payload is shorter than declared");
      }
      const auto countBytes = static_cast<size_t>(count);
      if (EVP_DigestUpdate(context.get(), buffer.data(), countBytes) != 1) {
        throw std::runtime_error(
          "repo-artifact-digest-failed: SHA-256 update failed");
      }
      writeAll(temporaryFd.get(), buffer.data(), countBytes, temporary.string());
      offset += static_cast<uint64_t>(countBytes);
    }
    struct stat after {};
    if (::fstat(payloadFd.get(), &after) != 0) {
      throwSystem("repo-artifact-payload-read-failed", "restat", payloadPath);
    }
    if (!sameSnapshot(before, after)) {
      throw std::runtime_error(
        "repo-artifact-payload-changed: committed payload changed while fetching");
    }
    if (finishDigest(context.get()) != payloadRef.contentDigest) {
      throw std::runtime_error(
        "repo-artifact-digest-mismatch: fetched bytes failed full verification");
    }
    if (::fsync(temporaryFd.get()) != 0) {
      throwSystem("repo-artifact-destination-sync-failed", "fsync", temporary.string());
    }
    const int rawFd = temporaryFd.release();
    if (::close(rawFd) != 0) {
      throwSystem("repo-artifact-destination-close-failed", "close", temporary.string());
    }
    commitFetchedFile(temporary, destination, deadline);
    renamed = true;
  }
  catch (...) {
    if (temporaryFd.get() >= 0) {
      const int rawFd = temporaryFd.release();
      ::close(rawFd);
    }
    if (!renamed) {
      ::unlink(temporary.c_str());
    }
    throw;
  }
}

ArtifactFetchResult
ArtifactRepositoryApi::fetchFile(const ArtifactHandle& handle,
                                 const std::string& destinationPath,
                                 int timeoutMs) const
{
  if (!m_impl) {
    throw std::logic_error("repo-persistent-api-moved-from: API has no active owner");
  }
  if (m_impl->serviceUser == nullptr || !m_impl->persistentConfig) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: handle fetch is Persistent only");
  }
  if (destinationPath.empty() || timeoutMs <= 0) {
    throw std::invalid_argument("repo-artifact-fetch-arguments-invalid: destination and timeout are required");
  }
  const auto operationStartedAt = std::chrono::steady_clock::now();
  const auto deadline = operationStartedAt + std::chrono::milliseconds(timeoutMs);
  ArtifactFetchResult result;
  result.reference = handle.reference;
  std::unique_lock<std::mutex> operationGuard(
    m_impl->operationMutex, std::defer_lock);
  while (!operationGuard.try_lock()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
      throw ArtifactFetchError(
        "repo-artifact-fetch-deadline-exceeded: deadline expired waiting for API operation",
        std::move(result));
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
      deadline - now, std::chrono::milliseconds(1)));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
    throw ArtifactFetchError(
      "repo-artifact-fetch-deadline-exceeded: deadline expired acquiring API operation",
      std::move(result));
  }
  const auto& config = *m_impl->persistentConfig;
  handle.validate(config.limits);
  if (handle.lifecycleMode != "PERSISTENT") {
    throw std::invalid_argument("repo-artifact-lifecycle-mismatch: Persistent handle is required");
  }
  fs::path destination;
  try {
    destination = fs::absolute(destinationPath).lexically_normal();
  }
  catch (const fs::filesystem_error& error) {
    result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
    throw ArtifactFetchError(
      "repo-artifact-destination-prepare-failed: " + std::string(error.what()),
      std::move(result));
  }
  if (destination.filename().empty()) {
    throw std::invalid_argument("repo-artifact-destination-invalid: destination must name a file");
  }
  fs::path parent;
  try {
    parent = destination.parent_path();
    if (parent.empty()) parent = fs::current_path();
    std::error_code directoryError;
    fs::create_directories(parent, directoryError);
    std::error_code typeError;
    const bool parentIsDirectory = fs::is_directory(parent, typeError);
    if (directoryError || typeError || !parentIsDirectory) {
      result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
      const auto detail = directoryError ? directoryError.message()
        : (typeError ? typeError.message() : parent.string());
      throw ArtifactFetchError(
        "repo-artifact-destination-directory-failed: " + detail,
        std::move(result));
    }
  }
  catch (const fs::filesystem_error& error) {
    result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
    throw ArtifactFetchError(
      "repo-artifact-destination-prepare-failed: " + std::string(error.what()),
      std::move(result));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
    throw ArtifactFetchError(
      "repo-artifact-fetch-deadline-exceeded: deadline expired preparing destination",
      std::move(result));
  }

  struct FetchCandidate
  {
    const SelectedRepoTarget* target = nullptr;
    const ArtifactReplicaCommit* replica = nullptr;
    size_t originalOrder = 0;
    bool wasCoolingDown = false;
  };
  std::vector<FetchCandidate> locations;
  for (size_t order = 0; order < handle.selectedTargets.size(); ++order) {
    const auto& target = handle.selectedTargets[order];
    const auto replica = std::find_if(handle.replicaCommits.begin(), handle.replicaCommits.end(),
      [&target] (const auto& item) {
        return item.repoNode == target.repoNode &&
               item.providerIdentity == target.providerIdentity.toUri();
      });
    if (replica != handle.replicaCommits.end() && replica->state == "COMMITTED") {
      locations.push_back(FetchCandidate{&target, &*replica, order});
    }
  }

  const auto artifactObjectName = artifactStorageObjectName(handle.reference);
  auto healthKeyFor = [&artifactObjectName] (const SelectedRepoTarget& target) {
    return Impl::FetchHealthKey{artifactObjectName, target.providerIdentity.toUri()};
  };
  const auto rankingTime = std::chrono::steady_clock::now();
  for (auto& candidate : locations) {
    const auto health = m_impl->fetchHealth.find(healthKeyFor(*candidate.target));
    const auto relevantVersion = health == m_impl->fetchHealth.end()
      ? candidate.replica->locationVersion
      : std::max(candidate.replica->locationVersion,
                 health->second.locationVersion);
    candidate.wasCoolingDown = health != m_impl->fetchHealth.end() &&
      health->second.failureLocationVersion == relevantVersion &&
      health->second.cooldownUntil > rankingTime;
  }
  std::stable_sort(locations.begin(), locations.end(), [&] (const auto& lhs, const auto& rhs) {
    const auto lhsHealth = m_impl->fetchHealth.find(healthKeyFor(*lhs.target));
    const auto rhsHealth = m_impl->fetchHealth.find(healthKeyFor(*rhs.target));
    if (lhs.wasCoolingDown != rhs.wasCoolingDown) return !lhs.wasCoolingDown;
    const auto lhsVersion = lhsHealth == m_impl->fetchHealth.end()
      ? lhs.replica->locationVersion
      : std::max(lhs.replica->locationVersion, lhsHealth->second.locationVersion);
    const auto rhsVersion = rhsHealth == m_impl->fetchHealth.end()
      ? rhs.replica->locationVersion
      : std::max(rhs.replica->locationVersion, rhsHealth->second.locationVersion);
    const uint32_t lhsFailures = lhsHealth != m_impl->fetchHealth.end() &&
      lhsHealth->second.failureLocationVersion == lhsVersion
      ? lhsHealth->second.consecutiveFailures : 0;
    const uint32_t rhsFailures = rhsHealth != m_impl->fetchHealth.end() &&
      rhsHealth->second.failureLocationVersion == rhsVersion
      ? rhsHealth->second.consecutiveFailures : 0;
    if (lhsFailures != rhsFailures) return lhsFailures < rhsFailures;
    const double lhsLatency = lhsHealth != m_impl->fetchHealth.end() &&
      lhsHealth->second.latencyLocationVersion == lhsVersion
      ? lhsHealth->second.latencyEwmaMs : 0.0;
    const double rhsLatency = rhsHealth != m_impl->fetchHealth.end() &&
      rhsHealth->second.latencyLocationVersion == rhsVersion
      ? rhsHealth->second.latencyEwmaMs : 0.0;
    if (lhsLatency != rhsLatency) return lhsLatency < rhsLatency;
    const bool lhsSeed = lhs.target->repoNode == handle.seedRepoId;
    const bool rhsSeed = rhs.target->repoNode == handle.seedRepoId;
    if (lhsSeed != rhsSeed) return lhsSeed;
    return lhs.originalOrder < rhs.originalOrder;
  });

  auto getOrCreateHealth = [&] (const Impl::FetchHealthKey& key) -> Impl::FetchHealth& {
    auto found = m_impl->fetchHealth.find(key);
    if (found != m_impl->fetchHealth.end()) return found->second;
    if (m_impl->fetchHealth.size() >= config.maxFetchHealthEntries) {
      const auto oldest = std::min_element(m_impl->fetchHealth.begin(),
        m_impl->fetchHealth.end(), [] (const auto& lhs, const auto& rhs) {
          return lhs.second.lastUpdated < rhs.second.lastUpdated;
        });
      if (oldest != m_impl->fetchHealth.end()) m_impl->fetchHealth.erase(oldest);
    }
    return m_impl->fetchHealth.emplace(key, Impl::FetchHealth{}).first->second;
  };

  std::string lastError = "repo-artifact-location-unavailable: no committed selected replica";
  static std::atomic<uint64_t> sequence{0};
  std::set<std::string> visitedProviderIdentities;
  const auto chunkBytes = std::min<uint64_t>(
    {COPY_CHUNK_BYTES, MAX_STORE_RANGE_BYTES, config.limits.maxChunkBytes});
  if (chunkBytes == 0) {
    throw std::invalid_argument("repo-artifact-chunk-limit-invalid: range size must be positive");
  }

  for (const auto& candidate : locations) {
    if (std::chrono::steady_clock::now() >= deadline) break;
    const auto& target = *candidate.target;
    const auto& replica = *candidate.replica;
    const auto providerIdentity = target.providerIdentity.toUri();
    if (!visitedProviderIdentities.insert(providerIdentity).second) continue;
    ArtifactFetchAttempt attempt;
    attempt.repoNode = target.repoNode;
    attempt.providerIdentity = providerIdentity;
    attempt.wasCoolingDownAtSelection = candidate.wasCoolingDown;
    const auto attemptStartedAt = std::chrono::steady_clock::now();
    const auto healthKey = healthKeyFor(target);
    try {
      attempt.requestTimeoutBudgetMs = static_cast<uint64_t>(
        boundedRequestTimeoutMs(deadline, config.requestTimeoutMs));
      ArtifactLocationRequest locationRequest;
      locationRequest.reference = handle.reference;
      const auto locationWaitDeadline = std::min(deadline,
        std::chrono::steady_clock::now() +
          std::chrono::milliseconds(config.requestTimeoutMs));
      const auto locationWire = waitForSignedResponse(*m_impl->serviceUser,
        target.providerIdentity,
        [serviceUser = m_impl->serviceUser,
         providerIdentity = target.providerIdentity,
         servicePrefix = target.servicePrefix, locationRequest, deadline,
         requestTimeoutMs = config.requestTimeoutMs]
        (auto onTimeout, auto onResponse) {
          const auto requestTimeout = boundedRequestTimeoutMs(
            deadline, requestTimeoutMs);
          return RepoClient::requestLocation(
            *serviceUser, providerIdentity, servicePrefix,
            locationRequest, requestTimeout,
            std::move(onTimeout), std::move(onResponse));
        }, locationWaitDeadline);
      if (std::chrono::steady_clock::now() >= deadline) {
        throw RepoRequestTimeoutError(
          "repo-artifact-fetch-deadline-exceeded: LOCATION response arrived after the operation deadline");
      }
      const std::string locationJson(locationWire.begin(), locationWire.end());
      const auto location = ArtifactLocationRecord::fromJson(locationJson, config.limits);
      location.validate(nowMilliseconds(), config.limits);
      // Core validates the signed Response KeyLocator against its Provider name
      // before this callback; this owner binds the signed Repo record to the handle.
      // The handle keeps its historical commit receipt. LOCATION is mutable
      // current routing evidence, so only an equal-version record must match
      // that receipt snapshot; a strictly newer signed record may name a later
      // operation's receipt for the same immutable artifact.
      const bool matchesHistoricalLocation =
        location.locationVersion == replica.locationVersion &&
        location.operationId == replica.operationId &&
        location.receiptId == replica.receiptId &&
        location.committedAtMs == replica.completedAtMs &&
        location.routeState == replica.routeState &&
        location.forwardingHint == replica.forwardingHint;
      const bool isNewerCurrentLocation =
        location.locationVersion > replica.locationVersion;
      if (location.repoNode != target.repoNode ||
          location.providerIdentity != target.providerIdentity.toUri() ||
          location.reference != handle.reference ||
          location.generation != replica.generation ||
          (!matchesHistoricalLocation && !isNewerCurrentLocation) ||
          location.routeState != "ROUTABLE" ||
          location.forwardingHint != target.servicePrefix.toUri()) {
        throw std::runtime_error(
          "repo-artifact-location-binding-failed: current signed location differs from the selected commit receipt");
      }

      auto& health = getOrCreateHealth(healthKey);
      if (health.locationVersion != 0 &&
          location.locationVersion < health.locationVersion) {
        throw std::runtime_error(
          "repo-artifact-location-rollback: signed location version moved backwards");
      }
      if (health.locationVersion == location.locationVersion &&
          health.locationVersion != 0 &&
          (health.operationId != location.operationId ||
           health.receiptId != location.receiptId)) {
        throw std::runtime_error(
          "repo-artifact-location-conflict: equal location version changed receipt tuple");
      }
      if (location.locationVersion > health.locationVersion) {
        health.locationVersion = location.locationVersion;
        health.operationId = location.operationId;
        health.receiptId = location.receiptId;
        health.consecutiveFailures = 0;
        health.failureLocationVersion = 0;
        health.latencyEwmaMs = 0.0;
        health.latencyLocationVersion = 0;
        health.cooldownUntil = {};
      }
      attempt.locationVersion = location.locationVersion;
      health.lastUpdated = std::chrono::steady_clock::now();

      fs::path temporary;
      FileDescriptor temporaryFd;
      for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary = parent /
          ("." + destination.filename().string() + ".ndnsf-" +
           std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
           std::to_string(sequence.fetch_add(1)) + ".tmp");
        const int rawFd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                 O_NOFOLLOW | O_CLOEXEC, 0600);
        if (rawFd >= 0) {
          temporaryFd = FileDescriptor(rawFd);
          break;
        }
        if (errno != EEXIST) {
          throwSystem("repo-artifact-destination-open-failed", "open", temporary.string());
        }
      }
      if (temporaryFd.get() < 0) {
        throw std::runtime_error(
          "repo-artifact-destination-open-failed: could not allocate a unique temporary file");
      }

      bool renamed = false;
      try {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
          makeSha256Context(), EVP_MD_CTX_free);
        uint64_t offset = 0;
        while (offset < handle.reference.sizeBytes) {
          const auto length = std::min<uint64_t>(chunkBytes,
            handle.reference.sizeBytes - offset);
          const auto rangeWaitDeadline = std::min(deadline,
            std::chrono::steady_clock::now() +
              std::chrono::milliseconds(config.requestTimeoutMs));
          const auto bytes = waitForSignedResponse(*m_impl->serviceUser,
            target.providerIdentity,
            [serviceUser = m_impl->serviceUser,
             providerIdentity = target.providerIdentity,
             servicePrefix = target.servicePrefix, artifactObjectName,
             offset, length, deadline,
             requestTimeoutMs = config.requestTimeoutMs]
            (auto onTimeout, auto onResponse) {
              const auto requestTimeout = boundedRequestTimeoutMs(
                deadline, requestTimeoutMs);
              return RepoClient::requestFetchRange(
                *serviceUser, providerIdentity, servicePrefix,
                artifactObjectName, RepoByteRange{offset, length}, requestTimeout,
                std::move(onTimeout), std::move(onResponse));
            }, rangeWaitDeadline);
          if (std::chrono::steady_clock::now() >= deadline) {
            throw RepoRequestTimeoutError(
              "repo-artifact-fetch-deadline-exceeded: range response arrived after the operation deadline");
          }
          attempt.bytesTransferred += bytes.size();
          result.bytesTransferred += bytes.size();
          if (bytes.size() != length ||
              EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error(
              "repo-artifact-range-invalid: received range size or digest update is invalid");
          }
          writeFileRange(temporaryFd.get(), temporary.string(), offset,
                         bytes.data(), bytes.size());
          offset += length;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          throw RepoRequestTimeoutError(
            "repo-artifact-fetch-deadline-exceeded: deadline expired before digest verification");
        }
        const auto payloadRef = payloadReference(handle.reference);
        if (finishDigest(digest.get()) != payloadRef.contentDigest) {
          throw std::runtime_error(
            "repo-artifact-digest-mismatch: fetched bytes failed full verification");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          throw RepoRequestTimeoutError(
            "repo-artifact-fetch-deadline-exceeded: deadline expired before destination sync");
        }
        if (::fsync(temporaryFd.get()) != 0) {
          throwSystem("repo-artifact-destination-sync-failed", "fsync", temporary.string());
        }
        const int rawFd = temporaryFd.release();
        if (::close(rawFd) != 0) {
          throwSystem("repo-artifact-destination-close-failed", "close", temporary.string());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          throw RepoRequestTimeoutError(
            "repo-artifact-fetch-deadline-exceeded: deadline expired before destination commit");
        }
        commitFetchedFile(temporary, destination, deadline);
        renamed = true;
      }
      catch (...) {
        if (temporaryFd.get() >= 0) {
          const int rawFd = temporaryFd.release();
          ::close(rawFd);
        }
        if (!renamed) ::unlink(temporary.c_str());
        throw;
      }
      const auto completedAt = std::chrono::steady_clock::now();
      auto& completionHealth = getOrCreateHealth(healthKey);
      const double latencyMs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
          completedAt - attemptStartedAt).count()) / 1000.0;
      completionHealth.latencyEwmaMs =
        completionHealth.latencyLocationVersion != attempt.locationVersion
        ? latencyMs : (completionHealth.latencyEwmaMs <= 0.0
          ? latencyMs : 0.8 * completionHealth.latencyEwmaMs + 0.2 * latencyMs);
      completionHealth.latencyLocationVersion = attempt.locationVersion;
      if (completionHealth.failureLocationVersion == attempt.locationVersion &&
          completionHealth.consecutiveFailures > 0) {
        --completionHealth.consecutiveFailures;
      }
      else if (completionHealth.failureLocationVersion != attempt.locationVersion) {
        completionHealth.consecutiveFailures = 0;
        completionHealth.failureLocationVersion = 0;
      }
      completionHealth.cooldownUntil = {};
      completionHealth.lastUpdated = completedAt;
      attempt.bytesVerified = handle.reference.sizeBytes;
      result.bytesVerified = handle.reference.sizeBytes;
      attempt.outcome = "SUCCEEDED";
      attempt.elapsedMs = elapsedMilliseconds(attemptStartedAt);
      result.repoNode = target.repoNode;
      result.providerIdentity = providerIdentity;
      result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
      result.attempts.push_back(std::move(attempt));
      return result;
    }
    catch (const std::exception& error) {
      lastError = error.what();
      attempt.outcome = "FAILED";
      attempt.failureClass = fetchFailureClass(error);
      attempt.elapsedMs = elapsedMilliseconds(attemptStartedAt);
      if (attempt.failureClass != "local-io") {
        auto& health = getOrCreateHealth(healthKey);
        const auto failureVersion = attempt.locationVersion != 0
          ? attempt.locationVersion : replica.locationVersion;
        const bool staleUnvalidatedFailure = attempt.locationVersion == 0 &&
          health.locationVersion > failureVersion;
        if (!staleUnvalidatedFailure) {
          if (health.failureLocationVersion != failureVersion) {
            health.consecutiveFailures = 0;
            health.failureLocationVersion = failureVersion;
          }
          health.consecutiveFailures = std::min<uint32_t>(
            health.consecutiveFailures + 1, 16);
          health.cooldownUntil = config.fetchFailureCooldownMs == 0
            ? std::chrono::steady_clock::time_point{}
            : std::chrono::steady_clock::now() +
                std::chrono::milliseconds(config.fetchFailureCooldownMs);
          health.lastUpdated = std::chrono::steady_clock::now();
        }
      }
      result.attempts.push_back(std::move(attempt));
      if (dynamic_cast<const DestinationRecoveryError*>(&error) != nullptr) break;
    }
  }
  result.totalElapsedMs = elapsedMilliseconds(operationStartedAt);
  throw ArtifactFetchError(lastError, std::move(result));
}

std::optional<ArtifactLocalCommitRecord>
ArtifactRepositoryApi::lookupLocalCommit(const ArtifactReference& reference) const
{
  return lookupLocalCommitUntil(
    reference, std::chrono::steady_clock::time_point::max());
}

std::optional<ArtifactLocalCommitRecord>
ArtifactRepositoryApi::lookupLocalCommitUntil(
  const ArtifactReference& reference,
  std::chrono::steady_clock::time_point deadline) const
{
  if (!m_impl) {
    throw std::logic_error("repo-inapp-api-moved-from: API has no active owner");
  }
  if (!m_impl->inApp) return std::nullopt;
  auto operationGuard = lockOperationUntil(
    m_impl->operationMutex, deadline, "In-App lookup operation");
  reference.validate(m_impl->inApp->limits);
  return m_impl->inApp->metadataStore.lookupLocalCommit(reference);
}

ArtifactCommitReceipt
ArtifactRepositoryApi::getReceipt(
  const std::string& receiptId, std::optional<ArtifactHandle> handle,
  int timeoutMs) const
{
  if (!m_impl) {
    throw std::logic_error("repo-artifact-api-moved-from: API has no active owner");
  }
  if (timeoutMs < 0) {
    throw std::invalid_argument("repo-receipt-timeout-invalid: timeout must be nonnegative");
  }
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);

  if (m_impl->inApp) {
    const auto id = parseLocalReceiptId(receiptId);
    if (!id) {
      throw std::out_of_range("repo-receipt-not-found: invalid In-App receipt ID");
    }
    if (handle) {
      handle->validate(m_impl->inApp->limits);
      if (handle->lifecycleMode != "IN_APP" ||
          handle->commitReceiptIds.empty() ||
          handle->commitReceiptIds.front() != receiptId) {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "In-App receipt ID is not present in the supplied handle");
      }
    }
    const auto record = m_impl->inApp->metadataStore.lookupLocalCommit(*id);
    if (!record || makeReceiptId(record->localCommitId) != receiptId ||
        (handle && handle->reference != record->reference)) {
      throw std::out_of_range("repo-receipt-not-found: no matching immutable local receipt");
    }
    const auto result = makePublishResult(*record, false, false, 0);
    if (!result.receipt) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "local commit did not reconstruct its immutable receipt");
    }
    return *result.receipt;
  }

  if (!m_impl->serviceUser || !m_impl->persistentConfig || !handle) {
    throw std::invalid_argument(
      "repo-receipt-handle-required: Persistent receipt lookup requires its ArtifactHandle");
  }
  const auto& config = *m_impl->persistentConfig;
  handle->validate(config.limits);
  if (handle->lifecycleMode != "PERSISTENT" ||
      handle->commitReceiptIds.empty() ||
      handle->commitReceiptIds.front() != receiptId ||
      handle->seedRepoId.empty()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "Persistent receipt ID or seed does not match the supplied handle");
  }
  const auto seed = std::find_if(handle->selectedTargets.begin(),
    handle->selectedTargets.end(), [&handle] (const auto& target) {
      return target.repoNode == handle->seedRepoId;
    });
  if (seed == handle->selectedTargets.end()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "Persistent handle does not select its declared seed Repo");
  }
  const int effectiveTimeout = timeoutMs == 0 ? config.requestTimeoutMs : timeoutMs;
  if (effectiveTimeout <= config.ackCollectionTimeMs) {
    throw std::invalid_argument(
      "repo-receipt-timeout-invalid: timeout must exceed the ACK collection window");
  }
  RepoReceiptLookupRequest query;
  query.receiptId = receiptId;
  query.repoNode = seed->repoNode;
  query.providerIdentity = seed->providerIdentity.toUri();
  query.reference = handle->reference;
  const auto receiptWaitDeadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(effectiveTimeout);
  const auto payload = waitForSignedResponse(*m_impl->serviceUser,
    seed->providerIdentity,
    [serviceUser = m_impl->serviceUser,
     providerIdentity = seed->providerIdentity,
     servicePrefix = seed->servicePrefix, query, effectiveTimeout]
    (auto onTimeout, auto onResponse) {
      return RepoClient::requestReceipt(*serviceUser,
        providerIdentity, servicePrefix, query, effectiveTimeout,
        std::move(onTimeout), std::move(onResponse));
    }, receiptWaitDeadline);
  const std::string wire(payload.begin(), payload.end());
  auto receipt = ArtifactCommitReceipt::fromJson(wire, config.limits);
  const auto seedReplica = std::find_if(receipt.replicaCommits.begin(),
    receipt.replicaCommits.end(), [&seed] (const auto& replica) {
      return replica.repoNode == seed->repoNode &&
             replica.providerIdentity == seed->providerIdentity.toUri();
    });
  if (receipt.receiptId != receiptId || receipt.lifecycleMode != "PERSISTENT" ||
      receipt.reference != handle->reference || seedReplica == receipt.replicaCommits.end() ||
      seedReplica->state != "COMMITTED" ||
      seedReplica->operationId != receipt.operationId) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "authenticated seed response does not bind the requested receipt");
  }
  return receipt;
}

AvailabilityReport
ArtifactRepositoryApi::checkAvailability(const ArtifactHandle& handle,
                                        const std::string& probeKind,
                                        int timeoutMs,
                                        uint64_t maxAgeMs) const
{
  (void)maxAgeMs; // T005 intentionally performs a fresh probe on every call.
  if (!m_impl) {
    throw std::logic_error("repo-artifact-api-moved-from: API has no active owner");
  }
  if (timeoutMs < 0 ||
      (probeKind != "metadata" && probeKind != "verify_content")) {
    throw std::invalid_argument(
      "repo-availability-arguments-invalid: probe kind or timeout is invalid");
  }
  const auto& limits = m_impl->inApp ? m_impl->inApp->limits
                                     : m_impl->persistentConfig->limits;
  if (handle.lifecycleMode == "IN_APP") {
    if (!m_impl->inApp) {
      throw std::logic_error("repo-artifact-lifecycle-mismatch: In-App handle requires In-App API");
    }
    handle.validate(limits);
    std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
    auto& owner = *m_impl->inApp;
    const auto now = nowMilliseconds();
    AvailabilityLocationReport location;
    location.locationKind = "IN_APP";
    location.generation = LOCAL_GENERATION;
    location.probeKind = probeKind;
    location.observedAtMs = now;
    location.validUntilMs = now > std::numeric_limits<uint64_t>::max() -
        owner.availabilityFreshnessMs
      ? std::numeric_limits<uint64_t>::max()
      : now + owner.availabilityFreshnessMs;
    std::error_code rootError;
    const auto rootStatus = fs::symlink_status(owner.rootPath, rootError);
    const bool rootUsable = !rootError && !fs::is_symlink(rootStatus) &&
      fs::is_directory(rootStatus) &&
      (rootStatus.permissions() & (fs::perms::owner_read | fs::perms::owner_exec)) ==
        (fs::perms::owner_read | fs::perms::owner_exec) &&
      ::access(owner.rootPath.c_str(), R_OK | X_OK) == 0;
    if (!rootUsable) {
      location.reachability = "UNREACHABLE";
      location.objectPresence = "UNKNOWN";
      location.validUntilMs = now;
      location.reason = "In-App durable root is missing or inaccessible";
    }
    else {
      location.reachability = "REACHABLE";
      try {
        const auto id = handle.commitReceiptIds.empty()
          ? std::optional<uint64_t>{} : parseLocalReceiptId(handle.commitReceiptIds.front());
        const auto record = id ? owner.metadataStore.lookupLocalCommit(*id)
                               : std::optional<ArtifactLocalCommitRecord>{};
        if (!record || record->reference != handle.reference ||
            makeReceiptId(record->localCommitId) != handle.commitReceiptIds.front()) {
          location.objectPresence = "ABSENT";
        }
        else {
          const auto payloadRef = payloadReference(handle.reference);
          const auto payload = owner.payloadStore.inspectCommitted(payloadRef);
          if (!payload.exists || !payload.regularFile ||
              payload.sizeBytes != handle.reference.sizeBytes) {
            location.objectPresence = "PARTIAL";
          }
          else {
            location.objectPresence = "PRESENT";
            if (probeKind == "verify_content") {
              try {
                verifyCommittedPayload(handle.reference, owner.payloadStore);
                location.integrity = "VERIFIED";
              }
              catch (const std::exception& error) {
                if (std::string(error.what()).find("repo-artifact-committed-corrupt:") == 0) {
                  location.integrity = "FAILED";
                  location.reason = "full payload digest does not match the immutable reference";
                }
                else {
                  location.reason = "full payload verifier could not complete reliably";
                }
              }
              catch (...) {
                location.reason = "full payload verifier could not complete reliably";
              }
            }
          }
        }
      }
      catch (...) {
        location.objectPresence = "UNKNOWN";
        location.integrity = "NOT_CHECKED";
        location.reason = "In-App durable metadata or payload could not be observed reliably";
      }
    }
    AvailabilityReport report;
    report.reference = handle.reference;
    report.probeKind = probeKind;
    report.locations.push_back(std::move(location));
    report.validate(limits);
    return report;
  }

  if (!m_impl->serviceUser || !m_impl->persistentConfig ||
      handle.lifecycleMode != "PERSISTENT") {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: Persistent handle requires Persistent API");
  }
  const auto& config = *m_impl->persistentConfig;
  const int effectiveTimeout = timeoutMs == 0 ? config.requestTimeoutMs : timeoutMs;
  if (effectiveTimeout <= config.ackCollectionTimeMs) {
    throw std::invalid_argument(
      "repo-availability-timeout-invalid: timeout must exceed the ACK collection window");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(effectiveTimeout);
  handle.validate(config.limits);
  AvailabilityReport report;
  report.reference = handle.reference;
  report.probeKind = probeKind;
  report.locations.reserve(handle.replicaCommits.size());

  const auto appendOperationLockTimeoutLocations = [&] {
    for (const auto& replica : handle.replicaCommits) {
      AvailabilityLocationReport result;
      result.locationKind = "PERSISTENT";
      result.repoNode = replica.repoNode;
      result.providerIdentity = replica.providerIdentity;
      result.generation = replica.generation;
      result.reachability = "UNKNOWN";
      result.objectPresence = "UNKNOWN";
      result.integrity = "NOT_CHECKED";
      result.observedAtMs = nowMilliseconds();
      result.validUntilMs = result.observedAtMs;
      result.probeKind = probeKind;
      result.reason = "operation deadline expired waiting for API operation lock";
      report.locations.push_back(std::move(result));
    }
  };

  std::unique_lock<std::mutex> operationGuard(
    m_impl->operationMutex, std::defer_lock);
  while (!operationGuard.try_lock()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      appendOperationLockTimeoutLocations();
      report.validate(config.limits);
      return report;
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
      deadline - now, std::chrono::milliseconds(1)));
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    appendOperationLockTimeoutLocations();
    report.validate(config.limits);
    return report;
  }

  for (const auto& replicaValue : handle.replicaCommits) {
    const auto targetIt = std::find_if(handle.selectedTargets.begin(),
      handle.selectedTargets.end(), [&replicaValue] (const auto& target) {
        return replicaValue.repoNode == target.repoNode &&
               replicaValue.providerIdentity == target.providerIdentity.toUri();
      });
    if (targetIt == handle.selectedTargets.end()) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "handle replica has no selected Repo target");
    }
    const auto& target = *targetIt;
    const auto* replica = &replicaValue;
    AvailabilityLocationReport result;
    result.locationKind = "PERSISTENT";
    result.repoNode = target.repoNode;
    result.providerIdentity = target.providerIdentity.toUri();
    result.probeKind = probeKind;
    result.observedAtMs = nowMilliseconds();
    result.validUntilMs = result.observedAtMs;
    result.reason = "no committed location receipt is recorded in the handle";
    if (replica != nullptr) {
      result.generation = replica->generation;
    }
    if (replica == nullptr || replica->state != "COMMITTED" ||
        replica->receiptId.empty() || replica->operationId.empty() ||
        replica->generation == 0 || replica->locationVersion == 0) {
      report.locations.push_back(std::move(result));
      continue;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.reason = "operation deadline expired before this location probe began";
      report.locations.push_back(std::move(result));
      continue;
    }

    ArtifactLocationRequest locationRequest;
    locationRequest.reference = handle.reference;
    std::vector<uint8_t> locationWire;
    int locationTimeoutMs = 0;
    try {
      locationTimeoutMs = remainingTimeoutMs(deadline);
    }
    catch (const std::exception&) {
      result.reason = "operation deadline expired before LOCATION request began";
      report.locations.push_back(std::move(result));
      continue;
    }
    if (locationTimeoutMs <= config.ackCollectionTimeMs) {
      result.reason = "operation deadline leaves no valid LOCATION request window";
      report.locations.push_back(std::move(result));
      continue;
    }
    try {
      const auto locationWaitDeadline = std::min(deadline,
        std::chrono::steady_clock::now() +
          std::chrono::milliseconds(locationTimeoutMs));
      locationWire = waitForSignedResponse(*m_impl->serviceUser,
        target.providerIdentity,
        [serviceUser = m_impl->serviceUser,
         providerIdentity = target.providerIdentity,
         servicePrefix = target.servicePrefix, locationRequest,
         locationTimeoutMs]
        (auto onTimeout, auto onResponse) {
          return RepoClient::requestLocation(*serviceUser,
            providerIdentity, servicePrefix, locationRequest,
            locationTimeoutMs, std::move(onTimeout), std::move(onResponse));
        }, locationWaitDeadline);
    }
    catch (const RepoRequestTimeoutError&) {
      result.reachability = "UNREACHABLE";
      result.objectPresence = "UNKNOWN";
      result.reason = "expected Provider did not return LOCATION before the deadline";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const RepoRequestStartError&) {
      result.reachability = "UNKNOWN";
      result.objectPresence = "UNKNOWN";
      result.reason = "LOCATION request could not be started";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const RepoResponseProviderMismatchError&) {
      result.reachability = "UNKNOWN";
      result.objectPresence = "UNKNOWN";
      result.integrity = "NOT_CHECKED";
      result.reason = "authenticated LOCATION response Provider differs from selected target";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const std::exception&) {
      result.reachability = "REACHABLE";
      result.objectPresence = "UNKNOWN";
      result.reason = "authenticated Provider rejected the LOCATION request";
      report.locations.push_back(std::move(result));
      continue;
    }

    result.reachability = "REACHABLE";
    result.reason = "signed current LOCATION could not be validated";
    ArtifactLocationRecord location;
    try {
      const std::string locationJson(locationWire.begin(), locationWire.end());
      location = ArtifactLocationRecord::fromJson(locationJson, config.limits);
      location.validate(nowMilliseconds(), config.limits);
    }
    catch (const std::exception&) {
      report.locations.push_back(std::move(result));
      continue;
    }
    const bool historicalMatches =
      location.locationVersion == replica->locationVersion &&
      location.operationId == replica->operationId &&
      location.receiptId == replica->receiptId &&
      location.committedAtMs == replica->completedAtMs &&
      location.routeState == replica->routeState &&
      location.forwardingHint == replica->forwardingHint;
    const bool newerCurrent = location.locationVersion > replica->locationVersion;
    if (location.repoNode != target.repoNode ||
        location.providerIdentity != target.providerIdentity.toUri() ||
        location.reference != handle.reference ||
        location.generation != replica->generation ||
        (!historicalMatches && !newerCurrent) ||
        location.routeState != "ROUTABLE" ||
        location.forwardingHint != target.servicePrefix.toUri()) {
      result.reason = "signed LOCATION is stale or does not bind this handle replica";
      report.locations.push_back(std::move(result));
      continue;
    }
    const Impl::FetchHealthKey healthKey{
      artifactStorageObjectName(handle.reference), target.providerIdentity.toUri()};
    auto healthIt = m_impl->fetchHealth.find(healthKey);
    if (healthIt != m_impl->fetchHealth.end()) {
      auto& health = healthIt->second;
      if (health.locationVersion != 0 &&
          location.locationVersion < health.locationVersion) {
        result.reason = "signed LOCATION version moved backwards from the latest validated probe";
        report.locations.push_back(std::move(result));
        continue;
      }
      if (health.locationVersion == location.locationVersion &&
          health.locationVersion != 0 &&
          (health.operationId != location.operationId ||
           health.receiptId != location.receiptId)) {
        result.reason = "equal signed LOCATION version conflicts with the latest receipt tuple";
        report.locations.push_back(std::move(result));
        continue;
      }
      if (location.locationVersion > health.locationVersion) {
        health.locationVersion = location.locationVersion;
        health.operationId = location.operationId;
        health.receiptId = location.receiptId;
        health.consecutiveFailures = 0;
        health.failureLocationVersion = 0;
        health.latencyEwmaMs = 0.0;
        health.latencyLocationVersion = 0;
        health.cooldownUntil = {};
        health.lastUpdated = std::chrono::steady_clock::now();
      }
    }
    result.operationId = location.operationId;
    result.receiptId = location.receiptId;
    result.locationVersion = location.locationVersion;

    int availabilityTimeoutMs = 0;
    try {
      availabilityTimeoutMs = remainingTimeoutMs(deadline);
    }
    catch (const std::exception&) {
      result.reason = "operation deadline expired before AVAILABILITY request began";
      report.locations.push_back(std::move(result));
      continue;
    }
    if (availabilityTimeoutMs <= config.ackCollectionTimeMs) {
      result.reason = "operation deadline leaves no valid AVAILABILITY request window";
      report.locations.push_back(std::move(result));
      continue;
    }
    RepoAvailabilityProbeRequest probe;
    probe.reference = handle.reference;
    probe.probeKind = probeKind;
    probe.expectedLocation = ArtifactExpectedLocation{
      location.repoNode, location.providerIdentity, location.operationId,
      location.receiptId, location.generation, location.locationVersion};
    std::vector<uint8_t> probeWire;
    try {
      const auto probeWaitDeadline = std::min(deadline,
        std::chrono::steady_clock::now() +
          std::chrono::milliseconds(availabilityTimeoutMs));
      probeWire = waitForSignedResponse(*m_impl->serviceUser,
        target.providerIdentity,
        [serviceUser = m_impl->serviceUser,
         providerIdentity = target.providerIdentity,
         servicePrefix = target.servicePrefix, probe,
         availabilityTimeoutMs]
        (auto onTimeout, auto onResponse) {
          return RepoClient::requestAvailability(*serviceUser,
            providerIdentity, servicePrefix, probe,
            availabilityTimeoutMs, std::move(onTimeout), std::move(onResponse));
        }, probeWaitDeadline);
    }
    catch (const RepoRequestTimeoutError&) {
      result.reachability = "REACHABLE";
      result.reason = "expected Provider did not return AVAILABILITY before the deadline";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const RepoRequestStartError&) {
      result.reachability = "REACHABLE";
      result.objectPresence = "UNKNOWN";
      result.integrity = "NOT_CHECKED";
      result.reason = "AVAILABILITY request could not start after signed LOCATION";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const RepoResponseProviderMismatchError&) {
      result.reachability = "REACHABLE";
      result.objectPresence = "UNKNOWN";
      result.integrity = "NOT_CHECKED";
      result.reason = "authenticated AVAILABILITY response Provider differs from selected target";
      report.locations.push_back(std::move(result));
      continue;
    }
    catch (const std::exception&) {
      result.reachability = "REACHABLE";
      result.reason = "authenticated Provider rejected the AVAILABILITY request";
      report.locations.push_back(std::move(result));
      continue;
    }
    try {
      const std::string responseJson(probeWire.begin(), probeWire.end());
      const auto response = RepoAvailabilityProbeResponse::fromJson(
        responseJson, config.limits);
      auto observed = response.report;
      const bool echoedTuple =
        response.expectedLocation.repoNode == location.repoNode &&
        response.expectedLocation.providerIdentity == location.providerIdentity &&
        response.expectedLocation.operationId == location.operationId &&
        response.expectedLocation.receiptId == location.receiptId &&
        response.expectedLocation.generation == location.generation &&
        response.expectedLocation.locationVersion == location.locationVersion;
      const bool noCurrentTuple = observed.operationId.empty() &&
        observed.receiptId.empty() && observed.locationVersion == 0;
      const bool currentTupleMatches = noCurrentTuple ||
        (observed.operationId == location.operationId &&
         observed.receiptId == location.receiptId &&
         observed.locationVersion == location.locationVersion);
      if (response.reference != handle.reference || !echoedTuple ||
          observed.locationKind != "PERSISTENT" ||
          observed.repoNode != target.repoNode ||
          observed.providerIdentity != target.providerIdentity.toUri() ||
          observed.generation != location.generation ||
          !currentTupleMatches ||
          (noCurrentTuple && observed.objectPresence != "UNKNOWN") ||
          observed.probeKind != probeKind || observed.reachability != "REACHABLE" ||
          observed.validUntilMs > location.expiresAtMs) {
        result.reason = "signed AVAILABILITY response does not bind the current location tuple";
        report.locations.push_back(std::move(result));
      }
      else {
        if (observed.reachability == "REACHABLE" &&
            observed.objectPresence == "PRESENT") {
          healthIt = m_impl->fetchHealth.find(healthKey);
          if (healthIt != m_impl->fetchHealth.end() &&
              healthIt->second.locationVersion == location.locationVersion &&
              healthIt->second.operationId == location.operationId &&
              healthIt->second.receiptId == location.receiptId) {
            healthIt->second.cooldownUntil = {};
            if (healthIt->second.failureLocationVersion == location.locationVersion &&
                healthIt->second.consecutiveFailures > 0) {
              --healthIt->second.consecutiveFailures;
            }
            else if (healthIt->second.failureLocationVersion != location.locationVersion) {
              healthIt->second.consecutiveFailures = 0;
              healthIt->second.failureLocationVersion = 0;
            }
            healthIt->second.lastUpdated = std::chrono::steady_clock::now();
          }
        }
        report.locations.push_back(std::move(observed));
      }
    }
    catch (const std::exception&) {
      result.reason = "signed AVAILABILITY response is malformed or inconsistent";
      report.locations.push_back(std::move(result));
    }
  }
  report.validate(config.limits);
  return report;
}

std::string
makeArtifactCommitReceiptId(const ArtifactCommitReceipt& receipt)
{
  return makePersistentReceiptId(receipt);
}

} // namespace ndnsf_distributed_repo
