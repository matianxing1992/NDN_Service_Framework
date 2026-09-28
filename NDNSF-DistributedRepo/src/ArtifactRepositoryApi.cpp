#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"

#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf_distributed_repo {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t LOCAL_GENERATION = 1;
constexpr size_t COPY_CHUNK_BYTES = 1024 * 1024;
constexpr uint64_t MAX_STORE_RANGE_BYTES = 16 * 1024 * 1024;

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
      config.limits.maxNameBytes > hardLimits.maxNameBytes) {
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

std::string
makeReceiptId(uint64_t localCommitId)
{
  return "in-app-local-" + std::to_string(localCommitId);
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
  std::string error;
  std::vector<uint8_t> payload;
};

void
completeWithError(const std::shared_ptr<CallbackResult>& result,
                  const std::string& error)
{
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->done) return;
    result->done = true;
    result->error = error;
  }
  result->condition.notify_all();
}

template<typename StartRequest>
std::vector<uint8_t>
waitForSignedResponse(StartRequest&& start)
{
  auto result = std::make_shared<CallbackResult>();
  const auto requestId = start(
    [result] (const ndn::Name&) {
      completeWithError(result, "repo-request-timeout: Provider request timed out");
    },
    [result] (const ndn_service_framework::ResponseMessage& response) {
      const auto payload = response.getPayload();
      {
        std::lock_guard<std::mutex> guard(result->mutex);
        if (result->done) return;
        if (!response.getStatus()) result->error = response.getErrorInfo();
        else result->payload.assign(payload.begin(), payload.end());
        result->done = true;
      }
      result->condition.notify_all();
    });
  if (requestId.empty()) {
    throw std::runtime_error("repo-request-start-failed: Core rejected Provider request");
  }
  std::unique_lock<std::mutex> lock(result->mutex);
  result->condition.wait(lock, [&] { return result->done; });
  if (!result->error.empty()) throw std::runtime_error(result->error);
  return std::move(result->payload);
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
    throw std::invalid_argument("repo-request-timeout-invalid: selected request timeout must exceed ACK window");
  }
  return RepoClient::requestSelectedService(
    serviceUser, target.providerIdentity, serviceName, requestBytes,
    config.ackCollectionTimeMs, timeoutMs);
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
  auto result = std::make_shared<CallbackResult>();
  auto candidates = std::make_shared<RepoCandidateSet>();
  const auto service = makeRepoServiceName(config.servicePrefix, "CAPABILITY");
  ndn::Buffer emptyPayload;
  const std::optional<ArtifactCapabilityRequirements> requiredCopy = requirements == nullptr
      ? std::nullopt : std::optional<ArtifactCapabilityRequirements>(*requirements);
  const std::optional<ArtifactReference> storageCopy = storageReference == nullptr
      ? std::nullopt : std::optional<ArtifactReference>(*storageReference);
  const auto requestId = serviceUser.BeginCollaborationWithProviders(
    service, emptyPayload, config.ackCollectionTimeMs, timeoutMs,
    [&serviceUser, result, candidates, service, config, requiredCopy, storageCopy]
    (const ndn_service_framework::CollaborationAckClosure& closure) {
      try {
        candidates->observedAtMs = nowMilliseconds();
        std::set<std::string> seenProviders;
        for (const auto& candidate : closure.candidates) {
          const auto provider = candidate.providerName.toUri();
          if (!candidate.ack.getStatus() ||
              candidate.serviceName != service ||
              !candidate.authenticationEvidence.trustSchemaValidated ||
              candidate.authenticationEvidence.signerIdentity.empty() ||
              candidate.authenticationEvidence.signerIdentity != provider ||
              !seenProviders.insert(provider).second) {
            continue;
          }
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
                continue;
              }
            }
            candidates->candidates.push_back(RepoCandidate{
              capability.repoNode, candidate.providerName, config.servicePrefix,
              std::move(capability), candidates->observedAtMs});
          }
          catch (const std::exception&) {
            continue;
          }
        }
        serviceUser.CancelCollaboration(closure.requestId);
        {
          std::lock_guard<std::mutex> guard(result->mutex);
          result->done = true;
        }
        result->condition.notify_all();
      }
      catch (const std::exception& error) {
        serviceUser.CancelCollaboration(closure.requestId);
        completeWithError(result, error.what());
      }
    },
    [result] (const ndn_service_framework::ResponseMessage&) {
      completeWithError(result, "repo-discovery-invalid: capability discovery unexpectedly received a final response");
    },
    [result] (const ndn::Name&) {
      completeWithError(result, "repo-discovery-timeout: capability ACK window timed out");
    },
    ndn::Name(), {}, ndn_service_framework::RequestCapabilities(),
    std::nullopt, {}, {}, {}, config.providerScope);
  if (requestId.empty()) throw std::runtime_error("repo-discovery-start-failed: Core rejected discovery request");
  std::unique_lock<std::mutex> lock(result->mutex);
  result->condition.wait(lock, [&] { return result->done; });
  if (!result->error.empty()) throw std::runtime_error(result->error);
  return std::move(*candidates);
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
    if (placementState != expectedPlacement ||
        (placementState == "SATISFIED" && state != "COMMITTED") ||
        (placementState == "PARTIAL" && state != "PARTIAL") ||
        (placementState == "UNSATISFIED" && state != "FAILED" && state != "ABORTED")) {
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
    if (!resultRepos.insert(replica.repoNode).second ||
        !resultProviders.insert(replica.providerIdentity).second ||
        (replica.state != "COMMITTED" && replica.state != "FAILED" &&
         replica.state != "SKIPPED") ||
        (replica.state == "FAILED" && replica.errorCode.empty()) ||
        (replica.state != "FAILED" && !replica.errorCode.empty())) {
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
      , ownership((rootPath / "metadata.sqlite3").string(), ownerId)
      , metadataStore((rootPath / "metadata.sqlite3").string(), true, 14, true)
      , payloadStore(rootPath.string(), MAX_STORE_RANGE_BYTES,
                     metadataStore.artifactWritesEnabled())
    {
    }

    fs::path rootPath;
    std::string ownerId;
    ArtifactLimits limits;
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
        persistentConfig->requestTimeoutMs <= persistentConfig->ackCollectionTimeMs) {
      throw std::invalid_argument("repo-persistent-client-config-invalid: deadlines or service prefix invalid");
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
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  const auto& config = *m_impl->persistentConfig;
  if (timeoutMs <= config.ackCollectionTimeMs) {
    throw std::invalid_argument("repo-request-timeout-invalid: publish timeout must exceed ACK window");
  }
  selected.validate(config.servicePrefix, config.limits);
  reference.validate(config.limits);
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

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::milliseconds(timeoutMs);
  const auto rangeCleanupReserveMs = std::min(250, std::max(50, timeoutMs / 10));
  const auto rangeWorkDeadline = deadline -
    std::chrono::milliseconds(rangeCleanupReserveMs);
  const auto orderedTargets = canonicalSelectedTargets(selected);
  const auto operationId = makePersistentOperationId(reference, selected, orderedTargets);
  const auto candidates = discoverCandidates(
    *m_impl->serviceUser, config, nullptr, &reference,
    remainingTimeoutMs(deadline));

  for (const auto& target : selected.targets) {
    const auto found = std::find_if(candidates.candidates.begin(), candidates.candidates.end(),
      [&target] (const auto& candidate) {
        return candidate.repoNode == target.repoNode &&
               candidate.providerIdentity == target.providerIdentity &&
               candidate.servicePrefix == target.servicePrefix;
      });
    if (found == candidates.candidates.end()) {
      throw std::runtime_error(
        "repo-selected-target-stale: a selected Repo/Provider tuple is absent from fresh authenticated discovery");
    }
  }

  const auto required = requiredWriteAcks(
    static_cast<uint32_t>(orderedTargets.size()), selected.consistency);
  const auto seed = std::find_if(orderedTargets.begin(), orderedTargets.end(),
    [&selected] (const auto& target) { return target.repoNode == selected.seedRepoId; });
  if (seed == orderedTargets.end()) {
    throw std::logic_error("repo-selected-seed-invalid: validated seed was not found");
  }
  const size_t seedIndex = static_cast<size_t>(std::distance(orderedTargets.begin(), seed));
  std::vector<size_t> attemptOrder;
  attemptOrder.reserve(orderedTargets.size());
  attemptOrder.push_back(seedIndex);
  for (size_t index = 0; index < orderedTargets.size(); ++index) {
    if (index != seedIndex) attemptOrder.push_back(index);
  }

  std::vector<std::optional<ArtifactReplicaCommit>> commits(orderedTargets.size());
  std::vector<std::optional<ArtifactReplicaResult>> replicaResults(orderedTargets.size());
  uint32_t achieved = 0;
  bool seedCommitted = false;
  const auto chunkBytes = std::min<uint64_t>(
    {COPY_CHUNK_BYTES, MAX_STORE_RANGE_BYTES, config.limits.maxChunkBytes});
  if (chunkBytes == 0) {
    throw std::invalid_argument("repo-artifact-chunk-limit-invalid: range size must be positive");
  }

  for (const size_t index : attemptOrder) {
    const auto& target = orderedTargets[index];
    ArtifactReplicaCommit replica;
    replica.repoNode = target.repoNode;
    replica.providerIdentity = target.providerIdentity.toUri();
    replica.operationId = operationId;
    replica.reference = reference;
    replica.completedAtMs = nowMilliseconds();
    replica.routeState = "UNKNOWN";

    if (achieved >= required) {
      replica.state = "SKIPPED";
      ArtifactReplicaResult result;
      result.repoNode = target.repoNode;
      result.providerIdentity = target.providerIdentity.toUri();
      result.state = replica.state;
      commits[index] = std::move(replica);
      replicaResults[index] = std::move(result);
      continue;
    }

    bool rangeTransferStarted = false;
    try {
      std::vector<uint8_t> response;
      if (reference.sizeBytes <= chunkBytes) {
        ArtifactStoreRequest write;
        write.operationId = operationId;
        write.generation = 1;
        write.reference = reference;
        write.bytes = readFileRange(sourceFd.get(), sourcePath, 0, reference.sizeBytes);
        response = runSelectedRepoRequest(
          *m_impl->serviceUser, target,
          makeRepoServiceName(target.servicePrefix, "STORE"), write.toWire(config.limits),
          config, remainingTimeoutMs(deadline));
      }
      else {
        uint64_t offset = 0;
        while (offset < reference.sizeBytes) {
          const auto length = std::min<uint64_t>(chunkBytes, reference.sizeBytes - offset);
          ArtifactRangeWriteRequest write;
          write.operationId = operationId;
          write.generation = 1;
          write.reference = reference;
          write.offsetBytes = offset;
          write.lengthBytes = length;
          write.bytes = readFileRange(sourceFd.get(), sourcePath, offset, length);
          const int rangeTimeoutMs = remainingTimeoutMs(rangeWorkDeadline);
          if (rangeTimeoutMs <= config.ackCollectionTimeMs) {
            throw std::runtime_error("repo-request-timeout-invalid: range work deadline exhausted");
          }
          rangeTransferStarted = true;
          (void)runSelectedRepoRequest(
            *m_impl->serviceUser, target,
            makeRepoServiceName(target.servicePrefix, "STORE_RANGE"), write.toWire(config.limits),
            config, rangeTimeoutMs);
          offset += length;
        }
        ArtifactRangeCommitRequest commitRequest;
        commitRequest.operationId = operationId;
        commitRequest.generation = 1;
        commitRequest.reference = reference;
        response = runSelectedRepoRequest(
          *m_impl->serviceUser, target,
          makeRepoServiceName(target.servicePrefix, "COMMIT_RANGES"),
          commitRequest.toWire(config.limits), config,
          remainingTimeoutMs(rangeWorkDeadline));
      }
      const std::string wire(response.begin(), response.end());
      const auto location = ArtifactLocationRecord::fromJson(wire, config.limits);
      location.validate(nowMilliseconds(), config.limits);
      if (location.repoNode != target.repoNode ||
          location.providerIdentity != target.providerIdentity.toUri() ||
          location.operationId != operationId || location.reference != reference ||
          location.generation != 1 || location.receiptId.empty() ||
          location.locationVersion == 0 || location.routeState != "ROUTABLE" ||
          location.forwardingHint != target.servicePrefix.toUri()) {
        throw std::runtime_error(
          "repo-artifact-receipt-binding-failed: commit response does not match selected target and reference");
      }
      replica.receiptId = location.receiptId;
      replica.generation = location.generation;
      replica.contentDigest = reference.contentDigest;
      replica.persistedBytes = reference.sizeBytes;
      replica.completedAtMs = location.committedAtMs;
      replica.locationVersion = location.locationVersion;
      replica.routeState = location.routeState;
      replica.state = "COMMITTED";
      replica.forwardingHint = location.forwardingHint;
      ++achieved;
      if (index == seedIndex) seedCommitted = true;

      ArtifactReplicaResult result;
      result.repoNode = target.repoNode;
      result.providerIdentity = target.providerIdentity.toUri();
      result.state = replica.state;
      result.receiptId = replica.receiptId;
      commits[index] = std::move(replica);
      replicaResults[index] = std::move(result);
    }
    catch (const std::exception&) {
      if (rangeTransferStarted) {
        try {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
          if (remaining > 1) {
            ArtifactRangeAbortRequest abort;
            abort.operationId = operationId;
            abort.generation = 1;
            abort.reference = reference;
            const auto abortBytes = abort.toWire(config.limits);
            const int timeout = static_cast<int>(std::min<int64_t>(
              remaining, std::numeric_limits<int>::max()));
            const int ackWindow = std::max(1, std::min(
              config.ackCollectionTimeMs, std::max(1, timeout / 4)));
            if (timeout > ackWindow) {
              const auto abortResponse = RepoClient::requestSelectedService(
                *m_impl->serviceUser, target.providerIdentity,
                makeRepoServiceName(target.servicePrefix, "ABORT_RANGES"),
                abortBytes, ackWindow, timeout);
              if (std::string(abortResponse.begin(), abortResponse.end()) != "aborted") {
                throw std::runtime_error("repo-artifact-range-abort-failed: unexpected response");
              }
            }
          }
        }
        catch (const std::exception&) {
          // The selected Repo also removes unreferenced range staging at its
          // next startup. Keep the publish failure bounded by the original
          // operation deadline if an owner-checked abort cannot be delivered.
        }
      }
      replica.state = "FAILED";
      replica.failureReason = "repo-replica-store-failed";
      ArtifactReplicaResult result;
      result.repoNode = target.repoNode;
      result.providerIdentity = target.providerIdentity.toUri();
      result.state = replica.state;
      result.errorCode = replica.failureReason;
      commits[index] = std::move(replica);
      replicaResults[index] = std::move(result);
      if (index == seedIndex) {
        for (const auto rest : attemptOrder) {
          if (commits[rest]) continue;
          const auto& skippedTarget = orderedTargets[rest];
          ArtifactReplicaCommit skipped;
          skipped.repoNode = skippedTarget.repoNode;
          skipped.providerIdentity = skippedTarget.providerIdentity.toUri();
          skipped.operationId = operationId;
          skipped.reference = reference;
          skipped.completedAtMs = nowMilliseconds();
          skipped.routeState = "UNKNOWN";
          skipped.state = "SKIPPED";
          commits[rest] = std::move(skipped);
          ArtifactReplicaResult skippedResult;
          skippedResult.repoNode = skippedTarget.repoNode;
          skippedResult.providerIdentity = skippedTarget.providerIdentity.toUri();
          skippedResult.state = "SKIPPED";
          replicaResults[rest] = std::move(skippedResult);
        }
        ArtifactPublishResult failed;
        failed.reference = reference;
        failed.operationId = operationId;
        failed.requestedReplicas = static_cast<uint32_t>(orderedTargets.size());
        failed.achievedReplicas = 0;
        for (auto& item : replicaResults) failed.replicas.push_back(std::move(*item));
        const auto ended = std::chrono::steady_clock::now();
        failed.totalDurationMs = std::chrono::duration<double, std::milli>(ended - started).count();
        failed.validate(config.limits);
        return failed;
      }
    }
  }

  if (!seedCommitted) {
    throw std::runtime_error("repo-seed-commit-missing: selected seed did not commit");
  }
  uint64_t receiptCompletedAtMs = nowMilliseconds();
  if (achieved >= required) {
    // A satisfied replay returns the same immutable result from Repo-local
    // commit facts, so use their stable commit time for skipped/failed target
    // rows and the aggregate receipt timestamp.
    receiptCompletedAtMs = 0;
    for (const auto& item : commits) {
      if (item->state == "COMMITTED") {
        receiptCompletedAtMs = std::max(receiptCompletedAtMs,
                                        item->completedAtMs);
      }
    }
    if (receiptCompletedAtMs == 0) {
      throw std::logic_error("repo-committed-time-missing: satisfied result has no commit time");
    }
    for (auto& item : commits) {
      if (item->state != "COMMITTED") {
        item->completedAtMs = receiptCompletedAtMs;
      }
    }
  }
  ArtifactCommitReceipt receipt;
  receipt.operationId = operationId;
  receipt.reference = reference;
  receipt.generation = 1;
  receipt.lifecycleMode = "PERSISTENT";
  receipt.requestedReplicas = static_cast<uint32_t>(orderedTargets.size());
  receipt.requiredAcks = required;
  receipt.achievedReplicas = achieved;
  receipt.writeConsistency = toString(selected.consistency);
  receipt.placementState = achieved >= required ? "SATISFIED" : "PARTIAL";
  receipt.state = achieved >= required ? "COMMITTED" : "PARTIAL";
  receipt.completedAtMs = receiptCompletedAtMs;

  ArtifactHandle handle;
  handle.reference = reference;
  handle.lifecycleMode = "PERSISTENT";
  handle.seedRepoId = selected.seedRepoId;
  handle.writeConsistency = receipt.writeConsistency;
  handle.selectedTargets = orderedTargets;
  for (size_t index = 0; index < commits.size(); ++index) {
    receipt.replicaCommits.push_back(*commits[index]);
  }
  receipt.receiptId = makePersistentReceiptId(receipt);
  handle.commitReceiptIds.push_back(receipt.receiptId);
  for (size_t index = 0; index < commits.size(); ++index) {
    handle.replicaCommits.push_back(*commits[index]);
    if (commits[index]->state == "COMMITTED") {
      handle.commitReceiptIds.push_back(commits[index]->receiptId);
    }
  }
  handle.validate(config.limits);
  receipt.validate(config.limits);

  ArtifactPublishResult published;
  published.reference = reference;
  published.operationId = operationId;
  published.requestedReplicas = receipt.requestedReplicas;
  published.achievedReplicas = achieved;
  published.deduplicated = false;
  published.resumed = false;
  const auto ended = std::chrono::steady_clock::now();
  published.totalDurationMs = std::chrono::duration<double, std::milli>(ended - started).count();
  for (auto& item : replicaResults) published.replicas.push_back(std::move(*item));
  published.handle = std::move(handle);
  published.receipt = std::move(receipt);
  published.validate(config.limits);
  return published;
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
  if (!m_impl) {
    throw std::logic_error("repo-inapp-api-moved-from: API has no active owner");
  }
  if (!m_impl->inApp) {
    throw std::logic_error("repo-artifact-lifecycle-mismatch: fetchFile(reference) is In-App only");
  }
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
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
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
      throwSystem("repo-artifact-destination-commit-failed", "rename",
                  destination.string());
    }
    renamed = true;
    fsyncDirectory(parent);
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

void
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
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  const auto& config = *m_impl->persistentConfig;
  handle.validate(config.limits);
  if (handle.lifecycleMode != "PERSISTENT") {
    throw std::invalid_argument("repo-artifact-lifecycle-mismatch: Persistent handle is required");
  }
  if (destinationPath.empty() || timeoutMs <= 0) {
    throw std::invalid_argument("repo-artifact-fetch-arguments-invalid: destination and timeout are required");
  }
  const auto destination = fs::absolute(destinationPath).lexically_normal();
  if (destination.filename().empty()) {
    throw std::invalid_argument("repo-artifact-destination-invalid: destination must name a file");
  }
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  std::error_code directoryError;
  fs::create_directories(parent, directoryError);
  if (directoryError || !fs::is_directory(parent)) {
    throw std::runtime_error("repo-artifact-destination-directory-failed: " +
      (directoryError ? directoryError.message() : parent.string()));
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  struct FetchCandidate
  {
    const SelectedRepoTarget* target = nullptr;
    const ArtifactReplicaCommit* replica = nullptr;
  };
  std::vector<FetchCandidate> locations;
  for (const auto& target : handle.selectedTargets) {
    const auto replica = std::find_if(handle.replicaCommits.begin(), handle.replicaCommits.end(),
      [&target] (const auto& item) {
        return item.repoNode == target.repoNode &&
               item.providerIdentity == target.providerIdentity.toUri();
      });
    if (replica != handle.replicaCommits.end() && replica->state == "COMMITTED") {
      locations.push_back(FetchCandidate{&target, &*replica});
    }
  }
  std::stable_partition(locations.begin(), locations.end(), [&handle] (const auto& candidate) {
    return candidate.target->repoNode == handle.seedRepoId;
  });
  std::string lastError = "repo-artifact-location-unavailable: no committed selected replica";
  static std::atomic<uint64_t> sequence{0};
  const auto chunkBytes = std::min<uint64_t>(
    {COPY_CHUNK_BYTES, MAX_STORE_RANGE_BYTES, config.limits.maxChunkBytes});
  if (chunkBytes == 0) {
    throw std::invalid_argument("repo-artifact-chunk-limit-invalid: range size must be positive");
  }

  for (const auto& candidate : locations) {
    if (std::chrono::steady_clock::now() >= deadline) break;
    const auto& target = *candidate.target;
    const auto& replica = *candidate.replica;
    try {
      ArtifactLocationRequest locationRequest;
      locationRequest.reference = handle.reference;
      const auto locationWire = waitForSignedResponse(
        [&] (auto onTimeout, auto onResponse) {
          return RepoClient::requestLocation(
            *m_impl->serviceUser, target.providerIdentity, target.servicePrefix,
            locationRequest, remainingTimeoutMs(deadline),
            std::move(onTimeout), std::move(onResponse));
        });
      const std::string locationJson(locationWire.begin(), locationWire.end());
      const auto location = ArtifactLocationRecord::fromJson(locationJson, config.limits);
      location.validate(nowMilliseconds(), config.limits);
      // Core validates the signed Response KeyLocator against its Provider name
      // before this callback; this owner binds the signed Repo record to the handle.
      if (location.repoNode != target.repoNode ||
          location.providerIdentity != target.providerIdentity.toUri() ||
          location.operationId != replica.operationId ||
          location.receiptId != replica.receiptId ||
          location.reference != handle.reference ||
          location.generation != replica.generation ||
          location.committedAtMs != replica.completedAtMs ||
          location.locationVersion != replica.locationVersion ||
          location.routeState != "ROUTABLE" ||
          location.routeState != replica.routeState ||
          location.forwardingHint != replica.forwardingHint ||
          location.forwardingHint != target.servicePrefix.toUri()) {
        throw std::runtime_error(
          "repo-artifact-location-binding-failed: current signed location differs from the selected commit receipt");
      }

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
          const auto bytes = waitForSignedResponse(
            [&] (auto onTimeout, auto onResponse) {
              return RepoClient::requestFetchRange(
                *m_impl->serviceUser, target.providerIdentity, target.servicePrefix,
                artifactStorageObjectName(handle.reference), RepoByteRange{offset, length},
                remainingTimeoutMs(deadline), std::move(onTimeout), std::move(onResponse));
            });
          if (bytes.size() != length ||
              EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error(
              "repo-artifact-range-invalid: received range size or digest update is invalid");
          }
          writeFileRange(temporaryFd.get(), temporary.string(), offset,
                         bytes.data(), bytes.size());
          offset += length;
        }
        const auto payloadRef = payloadReference(handle.reference);
        if (finishDigest(digest.get()) != payloadRef.contentDigest) {
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
        if (::rename(temporary.c_str(), destination.c_str()) != 0) {
          throwSystem("repo-artifact-destination-commit-failed", "rename", destination.string());
        }
        renamed = true;
        fsyncDirectory(parent);
      }
      catch (...) {
        if (temporaryFd.get() >= 0) {
          const int rawFd = temporaryFd.release();
          ::close(rawFd);
        }
        if (!renamed) ::unlink(temporary.c_str());
        throw;
      }
      return;
    }
    catch (const std::exception& error) {
      lastError = error.what();
    }
  }
  throw std::runtime_error(lastError);
}

std::optional<ArtifactLocalCommitRecord>
ArtifactRepositoryApi::lookupLocalCommit(const ArtifactReference& reference) const
{
  if (!m_impl) {
    throw std::logic_error("repo-inapp-api-moved-from: API has no active owner");
  }
  if (!m_impl->inApp) return std::nullopt;
  std::lock_guard<std::mutex> operationGuard(m_impl->operationMutex);
  reference.validate(m_impl->inApp->limits);
  return m_impl->inApp->metadataStore.lookupLocalCommit(reference);
}

} // namespace ndnsf_distributed_repo
