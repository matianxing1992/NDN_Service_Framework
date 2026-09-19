#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"

#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

namespace fs = std::filesystem;

std::atomic<uint64_t> g_metadataTempSequence{0};
std::mutex g_ioHooksMutex;
detail::FilesystemRepoStoreIoHooks g_ioHooks;

int
callFsync(int fd)
{
  std::lock_guard<std::mutex> lock(g_ioHooksMutex);
  return g_ioHooks.fsync == nullptr ? ::fsync(fd) : g_ioHooks.fsync(fd);
}

int
callClose(int fd)
{
  std::lock_guard<std::mutex> lock(g_ioHooksMutex);
  return g_ioHooks.close == nullptr ? ::close(fd) : g_ioHooks.close(fd);
}

[[noreturn]] void
throwSystem(const std::string& code, const std::string& action,
            const fs::path& path)
{
  throw std::runtime_error(code + ": " + action + " " + path.string() + ": " +
                           std::strerror(errno));
}

void
fsyncDirectory(const fs::path& path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-file-metadata-fsync-failed", "open directory", path);
  }
  if (callFsync(fd) != 0) {
    const int saved = errno;
    (void) callClose(fd);
    errno = saved;
    throwSystem("repo-file-metadata-fsync-failed", "fsync directory", path);
  }
  if (callClose(fd) != 0) {
    throwSystem("repo-file-metadata-fsync-failed", "close directory", path);
  }
}

void
writeAll(int fd, const uint8_t* bytes, size_t size, const fs::path& path)
{
  size_t offset = 0;
  while (offset < size) {
    const auto count = ::write(fd, bytes + offset, size - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throwSystem("repo-file-metadata-write-failed", "write", path);
    }
    if (count == 0) {
      throw std::runtime_error("repo-file-metadata-write-failed: short write " +
                               path.string());
    }
    offset += static_cast<size_t>(count);
  }
}

void
atomicWrite(const fs::path& path, const std::string& value)
{
  fs::path temporary;
  int fd = -1;
  for (size_t attempt = 0; attempt != 32; ++attempt) {
    temporary = path.string() + ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(g_metadataTempSequence.fetch_add(1));
    fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                 O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (fd < 0) {
    throwSystem("repo-file-metadata-write-failed", "open", temporary);
  }
  try {
    writeAll(fd, reinterpret_cast<const uint8_t*>(value.data()), value.size(),
             temporary);
    if (callFsync(fd) != 0) {
      const int saved = errno;
      (void) callClose(fd);
      fd = -1;
      ::unlink(temporary.c_str());
      errno = saved;
      throwSystem("repo-file-metadata-fsync-failed", "fsync", temporary);
    }
    if (callClose(fd) != 0) {
      const int saved = errno;
      // A failed close has an unspecified descriptor state.  Ownership ends
      // at this call; retrying in the catch block could close a reused fd.
      fd = -1;
      errno = saved;
      throwSystem("repo-file-metadata-write-failed", "close", temporary);
    }
    fd = -1;
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
      throwSystem("repo-file-metadata-write-failed", "rename", path);
    }
    try {
      fsyncDirectory(path.parent_path());
    }
    catch (const std::exception& e) {
      // rename() has made the new sidecar visible.  A directory fsync failure
      // is therefore an ambiguous durability boundary, not an ordinary
      // pre-commit write failure; callers must reconcile the visible row.
      throw std::runtime_error(
        std::string("repo-file-ambiguous-commit: metadata rename succeeded; ") +
        e.what());
    }
  }
  catch (...) {
    if (fd >= 0) {
      const int saved = errno;
      (void) callClose(fd);
      fd = -1;
      errno = saved;
    }
    ::unlink(temporary.c_str());
    throw;
  }
}

std::string
readText(const fs::path& path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    throwSystem("repo-file-metadata-read-failed", "open", path);
  }
  struct stat status {};
  if (::fstat(fd, &status) != 0 || status.st_size < 0 ||
      static_cast<uint64_t>(status.st_size) > 16 * 1024 * 1024) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    throwSystem("repo-file-metadata-read-failed", "stat", path);
  }
  std::string value(static_cast<size_t>(status.st_size), '\0');
  size_t offset = 0;
  while (offset < value.size()) {
    const auto count = ::read(fd, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      const int saved = errno;
      ::close(fd);
      errno = saved;
      throwSystem("repo-file-metadata-read-failed", "read", path);
    }
    offset += static_cast<size_t>(count);
  }
  if (::close(fd) != 0) {
    throwSystem("repo-file-metadata-read-failed", "close", path);
  }
  return value;
}

std::string
objectKey(const std::string& objectName)
{
  const std::vector<uint8_t> bytes(objectName.begin(), objectName.end());
  return sha256Hex(bytes);
}

uint64_t
effectiveGeneration(const RepoObjectManifest& manifest)
{
  return manifest.generation == 0 ? 1 : manifest.generation;
}

bool
sameManifestIdentity(const RepoObjectManifest& lhs,
                     const RepoObjectManifest& rhs)
{
  const auto lhsRequiredAcks = lhs.requiredWriteAcks == 0
    ? ndnsf_distributed_repo::requiredWriteAcks(
        lhs.replicationFactor, parseRepoWriteConsistency(lhs.writeConsistency))
    : lhs.requiredWriteAcks;
  const auto rhsRequiredAcks = rhs.requiredWriteAcks == 0
    ? ndnsf_distributed_repo::requiredWriteAcks(
        rhs.replicationFactor, parseRepoWriteConsistency(rhs.writeConsistency))
    : rhs.requiredWriteAcks;
  const auto& lhsConfirmed = lhs.confirmedReplicaNodes.empty()
    ? lhs.replicaNodes : lhs.confirmedReplicaNodes;
  const auto& rhsConfirmed = rhs.confirmedReplicaNodes.empty()
    ? rhs.replicaNodes : rhs.confirmedReplicaNodes;
  return lhs.objectName == rhs.objectName &&
         lhs.objectType == rhs.objectType &&
         lhs.sha256 == rhs.sha256 &&
         lhs.size == rhs.size &&
         lhs.segmentCount == rhs.segmentCount &&
         lhs.replicationFactor == rhs.replicationFactor &&
         effectiveGeneration(lhs) == effectiveGeneration(rhs) &&
         lhs.parentGeneration == rhs.parentGeneration &&
         lhs.writeConsistency == rhs.writeConsistency &&
         lhsRequiredAcks == rhsRequiredAcks &&
         lhs.operationId == rhs.operationId &&
         lhs.lifecycleState == rhs.lifecycleState &&
         lhs.policyEpoch == rhs.policyEpoch &&
         lhs.replicaNodes == rhs.replicaNodes &&
         lhsConfirmed == rhsConfirmed &&
         lhs.packetNames == rhs.packetNames;
}

void
validateManifestForPayload(const RepoObjectManifest& manifest)
{
  if (manifest.objectName.empty() || manifest.objectName.front() != '/') {
    throw std::invalid_argument("repo-file-invalid-object-name");
  }
  if (manifest.sha256.size() != 64 ||
      !std::all_of(manifest.sha256.begin(), manifest.sha256.end(), [](char ch) {
        return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
      })) {
    throw std::invalid_argument("repo-file-invalid-object-digest");
  }
}

void
ensurePrivateDirectory(const fs::path& path)
{
  std::error_code error;
  if (fs::exists(path, error)) {
    if (error || !fs::is_directory(path, error) || fs::is_symlink(path, error)) {
      throw std::runtime_error("repo-file-root-invalid: " + path.string());
    }
    const auto mode = fs::status(path, error).permissions();
    if (error || (mode & fs::perms::others_write) != fs::perms::none ||
        (mode & fs::perms::group_write) != fs::perms::none) {
      throw std::runtime_error("repo-file-root-not-private: " + path.string());
    }
    return;
  }
  fs::create_directories(path, error);
  if (error) {
    throw std::runtime_error("repo-file-root-create-failed: " + error.message());
  }
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
  if (error) {
    throw std::runtime_error("repo-file-root-permissions-failed: " + error.message());
  }
}

std::string
prepareBackendRoot(std::string rootPath)
{
  const auto path = fs::absolute(std::move(rootPath)).lexically_normal();
  ensurePrivateDirectory(path);
  ensurePrivateDirectory(path / "manifests");
  return path.string();
}

} // namespace

detail::FilesystemRepoStoreIoHooks
detail::installFilesystemRepoStoreIoHooks(detail::FilesystemRepoStoreIoHooks hooks) noexcept
{
  std::lock_guard<std::mutex> lock(g_ioHooksMutex);
  const auto previous = g_ioHooks;
  g_ioHooks = hooks;
  return previous;
}

FilesystemRepoStoreBackend::FilesystemRepoStoreBackend(
  std::string rootPath, uint64_t maxRangeBytes,
  uint64_t vectorCompatibilityThreshold, std::string ownerId)
  : m_rootPath(prepareBackendRoot(std::move(rootPath)))
  , m_maxRangeBytes(maxRangeBytes)
  , m_vectorCompatibilityThreshold(vectorCompatibilityThreshold)
  , m_ownership(m_rootPath, std::move(ownerId))
  , m_payloadStore(m_rootPath, maxRangeBytes)
{
  if (m_vectorCompatibilityThreshold == 0) {
    throw std::invalid_argument(
      "repo-file-vector-threshold-invalid: threshold must be positive");
  }
  ensurePrivateDirectory(fs::path(m_rootPath));
  ensurePrivateDirectory(fs::path(m_rootPath) / "manifests");
  recoverOrphans();
}

void
FilesystemRepoStoreBackend::put(const RepoObjectManifest& manifest,
                                 std::vector<uint8_t> payload)
{
  RepoObjectManifest durable = manifest;
  if (durable.size != payload.size() ||
      durable.sha256 != sha256Hex(payload)) {
    throw std::invalid_argument("repo-file-vector-manifest-mismatch");
  }
  if (payload.size() > m_vectorCompatibilityThreshold) {
    ++m_fullCopyFallbacks;
    throw std::runtime_error("repo-large-object-vector-path-disabled");
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto path = manifestPath(durable.objectName);
    if (durable.generation == 0 && fs::exists(path)) {
      durable.generation = effectiveGeneration(
        parseManifestJson(readText(path))) + 1;
    }
    else if (durable.generation == 0) {
      durable.generation = 1;
    }
  }
  putRange(durable, RepoByteRange{0, payload.size()}, payload);
  commitRanges(durable);
}

void
FilesystemRepoStoreBackend::putManifest(const RepoObjectManifest& manifest)
{
  if (manifest.objectName.empty() || manifest.objectName.front() != '/') {
    throw std::invalid_argument("repo-file-invalid-object-name");
  }
  if ((!isMetadataOnly(manifest) && manifest.size != 0) ||
      !manifest.sha256.empty()) {
    if (!isMetadataOnly(manifest)) {
      throw std::invalid_argument(
        "repo-file-manifest-payload-required: ordinary manifest must use put or ranges");
    }
    validateManifestForPayload(manifest);
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  RepoObjectManifest durable = manifest;
  if (durable.generation == 0) {
    durable.generation = effectiveGeneration(durable);
  }
  const auto previous = [&]() -> std::optional<RepoObjectManifest> {
    const auto path = manifestPath(manifest.objectName);
    if (!fs::exists(path)) {
      return std::nullopt;
    }
    return parseManifestJson(readText(path));
  }();
  const auto generation = effectiveGeneration(durable);
  if (previous) {
    const auto previousGeneration = effectiveGeneration(*previous);
    if (previousGeneration > generation ||
        (previousGeneration == generation &&
         !sameManifestIdentity(*previous, durable))) {
      throw std::runtime_error("repo-generation-conflict: stale manifest update");
    }
  }
  writeManifest(durable);
  if (previous && previous->sha256 != durable.sha256 &&
      !previous->sha256.empty() && !isMetadataOnly(*previous)) {
    // Metadata publication is the commit point.  If old-payload cleanup is
    // interrupted, leave the unreferenced bytes for recoverOrphans() rather
    // than reporting a failure after callers can already observe the new
    // manifest.
    try {
      if (!isDigestReferencedUnlocked(previous->sha256, durable.objectName)) {
        std::error_code error;
        fs::remove(m_payloadStore.committedPath(artifactReference(*previous)),
                   error);
      }
    }
    catch (...) {
    }
  }
}

StoredObject
FilesystemRepoStoreBackend::get(const std::string& objectName) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto manifest = readManifestUnlocked(objectName);
  if (isMetadataOnly(manifest)) {
    throw std::runtime_error("repo-metadata-only-payload-not-materialized");
  }
  if (manifest.size > m_vectorCompatibilityThreshold) {
    ++m_fullCopyFallbacks;
    throw std::runtime_error("repo-large-object-vector-path-disabled");
  }
  if (manifest.size == 0) {
    return StoredObject{manifest, {}};
  }
  const auto reference = artifactReference(manifest);
  return StoredObject{manifest,
                      m_payloadStore.readRange(reference, manifest.generation,
                                                ArtifactByteRange{0, manifest.size})};
}

bool
FilesystemRepoStoreBackend::has(const std::string& objectName) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  try {
    const auto manifest = readManifestUnlocked(objectName);
    if (isMetadataOnly(manifest) || manifest.size == 0) {
      return true;
    }
    return m_payloadStore.isCommitted(artifactReference(manifest),
                                      manifest.generation);
  }
  catch (const std::out_of_range&) {
    return false;
  }
}

bool
FilesystemRepoStoreBackend::erase(const std::string& objectName)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  RepoObjectManifest manifest;
  try {
    manifest = readManifestUnlocked(objectName);
  }
  catch (const std::out_of_range&) {
    return false;
  }

  // Metadata is the visibility boundary.  Remove and fsync the manifest
  // before touching its payload so a cleanup failure cannot leave a visible
  // manifest whose payload has already disappeared.  Any payload left after
  // this point is an unreferenced orphan and can be reclaimed by recovery.
  std::error_code error;
  fs::remove(manifestPath(objectName), error);
  if (error) {
    throw std::runtime_error("repo-file-metadata-erase-failed: " +
                             error.message());
  }
  try {
    fsyncDirectory(fs::path(m_rootPath) / "manifests");
  }
  catch (const std::exception& e) {
    // The sidecar has already been removed and is no longer observable.  The
    // directory fsync result is nevertheless ambiguous across a crash, so
    // let the Core reconcile the durable catalog instead of claiming a clean
    // failure.
    throw std::runtime_error(
      std::string("repo-file-ambiguous-commit: metadata erase succeeded; ") +
      e.what());
  }

  if (!isMetadataOnly(manifest) && manifest.size != 0) {
    try {
      if (!isDigestReferencedUnlocked(manifest.sha256, objectName)) {
        const auto reference = artifactReference(manifest);
        m_payloadStore.abort(reference, manifest.generation);
        std::error_code payloadError;
        fs::remove(m_payloadStore.committedPath(reference), payloadError);
        if (payloadError) {
          throw std::runtime_error("payload cleanup failed: " +
                                   payloadError.message());
        }
        fsyncDirectory(fs::path(m_payloadStore.committedPath(reference)).parent_path());
      }
    }
    catch (const std::exception& e) {
      // The manifest is already absent.  Report an ambiguous cleanup result
      // so RepoCore refreshes physical usage and recovery can reclaim the
      // unreferenced payload without exposing a broken manifest.
      throw std::runtime_error(
        std::string("repo-file-ambiguous-commit: metadata erase succeeded; ") +
        e.what());
    }
  }
  return true;
}

size_t
FilesystemRepoStoreBackend::size() const
{
  return listManifests().size();
}

std::vector<RepoObjectManifest>
FilesystemRepoStoreBackend::listManifests() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<RepoObjectManifest> manifests;
  const fs::path directory = fs::path(m_rootPath) / "manifests";
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    manifests.push_back(parseManifestJson(readText(entry.path())));
  }
  std::sort(manifests.begin(), manifests.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.objectName < rhs.objectName;
            });
  return manifests;
}

uint64_t
FilesystemRepoStoreBackend::usedBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  uint64_t total = 0;
  const fs::path payloadRoot = fs::path(m_rootPath) / "payloads";
  std::error_code rootError;
  const auto rootStatus = fs::symlink_status(payloadRoot, rootError);
  if (rootError == std::errc::no_such_file_or_directory) {
    return 0;
  }
  if (rootError || fs::is_symlink(rootStatus) || !fs::is_directory(rootStatus)) {
    throw std::runtime_error("repo-file-payload-root-invalid: " + payloadRoot.string());
  }
  for (const auto& entry : fs::recursive_directory_iterator(payloadRoot)) {
    std::error_code statusError;
    const auto status = fs::symlink_status(entry.path(), statusError);
    if (statusError) {
      throw std::runtime_error("repo-file-payload-stat-failed: " +
                               statusError.message());
    }
    if (fs::is_symlink(status)) {
      throw std::runtime_error("repo-file-payload-symlink-forbidden: " +
                               entry.path().string());
    }
    if (!fs::is_regular_file(status)) {
      continue;
    }
    const int fd = ::open(entry.path().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      throwSystem("repo-file-size-query-failed", "open", entry.path());
    }
    struct stat fileStatus {};
    if (::fstat(fd, &fileStatus) != 0 || !S_ISREG(fileStatus.st_mode) ||
        fileStatus.st_size < 0) {
      const int saved = errno;
      ::close(fd);
      errno = saved;
      throwSystem("repo-file-size-query-failed", "stat", entry.path());
    }
    const auto bytes = static_cast<uint64_t>(fileStatus.st_size);
    ::close(fd);
    if (bytes > std::numeric_limits<uint64_t>::max() - total) {
      return std::numeric_limits<uint64_t>::max();
    }
    total += bytes;
  }
  return total;
}

void
FilesystemRepoStoreBackend::putRange(const RepoObjectManifest& manifest,
                                      RepoByteRange range,
                                      const std::vector<uint8_t>& bytes)
{
  validateManifestForPayload(manifest);
  if (range.lengthBytes != bytes.size()) {
    throw std::invalid_argument("repo-file-range-length-mismatch");
  }
  if (range.offsetBytes > manifest.size ||
      range.lengthBytes > manifest.size - range.offsetBytes) {
    throw std::out_of_range("repo-file-range-write-out-of-bounds");
  }
  if (range.lengthBytes > m_maxRangeBytes) {
    throw std::invalid_argument("repo-file-range-too-large");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto reference = artifactReference(manifest);
  const auto generation = effectiveGeneration(manifest);
  const auto previous = [&]() -> std::optional<RepoObjectManifest> {
    const auto path = manifestPath(manifest.objectName);
    if (!fs::exists(path)) {
      return std::nullopt;
    }
    return parseManifestJson(readText(path));
  }();
  if (previous) {
    const auto previousGeneration = effectiveGeneration(*previous);
    if (previousGeneration > generation ||
        (previousGeneration == generation &&
         !sameManifestIdentity(*previous, manifest))) {
      throw std::runtime_error("repo-generation-conflict: stale range write");
    }
  }
  if (m_payloadStore.isCommitted(reference, generation)) {
    const auto existing = m_payloadStore.readRange(
      reference, generation,
      ArtifactByteRange{range.offsetBytes, range.lengthBytes});
    if (existing != bytes) {
      throw std::runtime_error("repo-file-immutable-digest-conflict");
    }
    return;
  }
  const auto reservation = m_reservations.find(manifest.objectName);
  auto canonicalManifest = manifest;
  canonicalManifest.generation = generation;
  if (reservation != m_reservations.end() &&
      (reservation->second.digest != manifest.sha256 ||
       reservation->second.size != manifest.size ||
       reservation->second.generation != generation ||
       reservation->second.canonicalIdentity != canonicalManifest.toJson())) {
    throw std::runtime_error("repo-generation-conflict: range reservation identity mismatch");
  }
  reserveDiskUnlocked(manifest);
  try {
    m_payloadStore.begin(reference, generation);
    m_payloadStore.writeRange(
      reference, generation,
      ArtifactByteRange{range.offsetBytes, range.lengthBytes}, bytes);
    if (range.lengthBytes != 0) {
      m_payloadStore.markVerified(
        reference, generation,
        ArtifactByteRange{range.offsetBytes, range.lengthBytes});
    }
  }
  catch (...) {
    // A failed range closes this staging attempt.  Do not leave a reservation
    // or an apparently resumable partial object owned by the failed caller.
    // The cleanup is best effort; recoverOrphans() remains the restart barrier
    // if the filesystem itself refuses the cleanup operation.
    try {
      m_payloadStore.abort(reference, generation);
    }
    catch (...) {
    }
    releaseReservationUnlocked(manifest.objectName);
    throw;
  }
}

void
FilesystemRepoStoreBackend::commitRanges(const RepoObjectManifest& manifest)
{
  validateManifestForPayload(manifest);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto reference = artifactReference(manifest);
  const auto generation = effectiveGeneration(manifest);
  auto canonicalManifest = manifest;
  canonicalManifest.generation = generation;
  const auto reservation = m_reservations.find(manifest.objectName);
  if (reservation != m_reservations.end() &&
      reservation->second.canonicalIdentity != canonicalManifest.toJson()) {
    throw std::runtime_error("repo-generation-conflict: commit reservation identity mismatch");
  }
  const auto previous = [&]() -> std::optional<RepoObjectManifest> {
    const auto path = manifestPath(manifest.objectName);
    if (!fs::exists(path)) {
      return std::nullopt;
    }
    return parseManifestJson(readText(path));
  }();
  if (previous) {
    const auto previousGeneration = effectiveGeneration(*previous);
    if (previousGeneration > generation ||
        (previousGeneration == generation &&
         !sameManifestIdentity(*previous, manifest))) {
      throw std::runtime_error("repo-generation-conflict: stale range commit");
    }
  }
  const bool hadReservation = reservation != m_reservations.end();
  const auto rollback = [&]() noexcept {
    if (!hadReservation) {
      return;
    }
    try {
      m_payloadStore.abort(reference, generation);
    }
    catch (...) {
    }
    // finalize() may have renamed the payload before metadata publication.
    // Remove that payload only when the current manifest does not reference
    // this failed generation; a previously durable object remains intact.
    try {
      bool currentReferencesPayload = false;
      const auto path = manifestPath(manifest.objectName);
      if (fs::exists(path)) {
        const auto current = parseManifestJson(readText(path));
        currentReferencesPayload = current.sha256 == manifest.sha256 &&
                                   effectiveGeneration(current) == generation;
      }
      if (!currentReferencesPayload &&
          !isDigestReferencedUnlocked(manifest.sha256, manifest.objectName)) {
        std::error_code error;
        const auto payloadPath = m_payloadStore.committedPath(reference);
        fs::remove(payloadPath, error);
        if (!error && fs::exists(fs::path(payloadPath).parent_path())) {
          fsyncDirectory(fs::path(payloadPath).parent_path());
        }
      }
    }
    catch (...) {
    }
    releaseReservationUnlocked(manifest.objectName);
  };
  try {
    if (!m_payloadStore.isCommitted(reference, generation)) {
      m_payloadStore.flush(reference, generation);
      m_payloadStore.finalize(reference, generation);
    }
    RepoObjectManifest durable = manifest;
    if (durable.generation == 0) {
      durable.generation = generation;
    }
    writeManifest(durable);
    if (previous && previous->sha256 != durable.sha256 &&
        !previous->sha256.empty() && !isMetadataOnly(*previous)) {
      // The new manifest is already durable at this point.  Retaining an old
      // unreferenced payload on cleanup failure is safe and recoverable, while
      // reporting the commit as failed would split Core accounting from the
      // manifest that callers can already observe.
      try {
        if (!isDigestReferencedUnlocked(previous->sha256, durable.objectName)) {
          std::error_code error;
          fs::remove(m_payloadStore.committedPath(artifactReference(*previous)),
                     error);
        }
      }
      catch (...) {
      }
    }
    releaseReservationUnlocked(manifest.objectName);
  }
  catch (...) {
    rollback();
    throw;
  }
}

std::vector<uint8_t>
FilesystemRepoStoreBackend::getRange(const std::string& objectName,
                                     RepoByteRange range) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto manifest = readManifestUnlocked(objectName);
  if (isMetadataOnly(manifest)) {
    throw std::runtime_error("repo-metadata-only-payload-not-materialized");
  }
  if (range.offsetBytes > manifest.size ||
      range.lengthBytes > manifest.size - range.offsetBytes) {
    throw std::out_of_range("repo-file-range-read-out-of-bounds");
  }
  if (range.lengthBytes == 0) {
    return {};
  }
  return m_payloadStore.readRange(artifactReference(manifest),
                                  manifest.generation,
                                  ArtifactByteRange{range.offsetBytes,
                                                    range.lengthBytes});
}

RepoObjectManifest
FilesystemRepoStoreBackend::getManifest(const std::string& objectName) const
{
  return readManifest(objectName);
}

bool
FilesystemRepoStoreBackend::supportsRange() const noexcept
{
  return true;
}

uint64_t
FilesystemRepoStoreBackend::fullCopyFallbackCount() const noexcept
{
  return m_fullCopyFallbacks.load();
}

bool
FilesystemRepoStoreBackend::supportsManifestLookup() const noexcept
{
  return true;
}

void
FilesystemRepoStoreBackend::abortRanges(const std::string& objectName)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_reservations.find(objectName);
  if (found == m_reservations.end()) {
    return;
  }
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.sha256 = found->second.digest;
  manifest.size = found->second.size;
  manifest.generation = found->second.generation;
  m_payloadStore.abort(artifactReference(manifest), found->second.generation);
  releaseReservationUnlocked(objectName);
}

uint64_t
FilesystemRepoStoreBackend::recoverOrphans()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto requireDirectory = [](const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) {
      throw std::runtime_error("repo-file-recovery-directory-invalid: " +
                               path.string());
    }
  };
  std::set<std::string> durableDigests;
  bool uncertain = false;
  const fs::path manifests = fs::path(m_rootPath) / "manifests";
  requireDirectory(manifests);
  for (const auto& entry : fs::directory_iterator(manifests)) {
    std::error_code entryError;
    const auto entryStatus = fs::symlink_status(entry.path(), entryError);
    if (entryError || fs::is_symlink(entryStatus)) {
      uncertain = true;
      continue;
    }
    if (!fs::is_regular_file(entryStatus) || entry.path().extension() != ".json") {
      continue;
    }
    try {
      durableDigests.insert(parseManifestJson(readText(entry.path())).sha256);
    }
    catch (const std::exception&) {
      // A malformed sidecar is retained for operator diagnosis; it is not
      // safe to infer ownership from it during recovery.
      uncertain = true;
    }
  }
  if (uncertain) {
    return 0;
  }
  uint64_t removed = 0;
  const fs::path staging = fs::path(m_rootPath) / "staging";
  requireDirectory(staging);
  for (const auto& entry : fs::directory_iterator(staging)) {
    std::error_code entryError;
    const auto entryStatus = fs::symlink_status(entry.path(), entryError);
    if (entryError || fs::is_symlink(entryStatus)) {
      throw std::runtime_error("repo-file-recovery-staging-entry-invalid: " +
                               entry.path().string());
    }
    if (fs::is_regular_file(entryStatus)) {
      std::error_code error;
      if (fs::remove(entry.path(), error) && !error) {
        ++removed;
      }
    }
  }
  const fs::path payloadRoot = fs::path(m_rootPath) / "payloads" / "sha256";
  std::error_code payloadError;
  const auto payloadStatus = fs::symlink_status(payloadRoot, payloadError);
  if (payloadError == std::errc::no_such_file_or_directory) {
    m_reservations.clear();
    m_reservedBytes = 0;
    return removed;
  }
  requireDirectory(payloadRoot);
  if (fs::exists(payloadStatus)) {
    for (const auto& entry : fs::recursive_directory_iterator(payloadRoot)) {
      std::error_code entryError;
      const auto entryStatus = fs::symlink_status(entry.path(), entryError);
      if (entryError || fs::is_symlink(entryStatus)) {
        throw std::runtime_error("repo-file-recovery-payload-entry-invalid: " +
                                 entry.path().string());
      }
      if (!fs::is_regular_file(entryStatus)) {
        continue;
      }
      if (durableDigests.count(entry.path().filename().string()) == 0) {
        std::error_code error;
        if (fs::remove(entry.path(), error) && !error) {
          ++removed;
        }
      }
    }
  }
  m_reservations.clear();
  m_reservedBytes = 0;
  return removed;
}

const std::string&
FilesystemRepoStoreBackend::rootPath() const noexcept
{
  return m_rootPath;
}

uint64_t
FilesystemRepoStoreBackend::vectorCompatibilityThreshold() const noexcept
{
  return m_vectorCompatibilityThreshold;
}

ArtifactReference
FilesystemRepoStoreBackend::artifactReference(
  const RepoObjectManifest& manifest) const
{
  validateManifestForPayload(manifest);
  ArtifactReference reference;
  reference.logicalName = manifest.objectName;
  reference.contentDigest = manifest.sha256;
  reference.sizeBytes = manifest.size;
  reference.formatVersion = "artifact-manifest-v2";
  reference.rootManifestName = manifest.objectName;
  reference.publisherIdentity = "/repo";
  reference.policyEpoch = manifest.policyEpoch.empty() ? "repo" : manifest.policyEpoch;
  return reference;
}

fs::path
FilesystemRepoStoreBackend::manifestPath(const std::string& objectName) const
{
  if (objectName.empty() || objectName.front() != '/') {
    throw std::invalid_argument("repo-file-invalid-object-name");
  }
  return fs::path(m_rootPath) / "manifests" / (objectKey(objectName) + ".json");
}

RepoObjectManifest
FilesystemRepoStoreBackend::readManifest(const std::string& objectName) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return readManifestUnlocked(objectName);
}

RepoObjectManifest
FilesystemRepoStoreBackend::readManifestUnlocked(const std::string& objectName) const
{
  const auto path = manifestPath(objectName);
  if (!fs::exists(path)) {
    throw std::out_of_range("repo-object-not-found: " + objectName);
  }
  auto manifest = parseManifestJson(readText(path));
  if (manifest.objectName != objectName) {
    throw std::runtime_error("repo-file-metadata-key-mismatch");
  }
  return manifest;
}

void
FilesystemRepoStoreBackend::writeManifest(const RepoObjectManifest& manifest) const
{
  const auto path = manifestPath(manifest.objectName);
  fs::create_directories(path.parent_path());
  atomicWrite(path, manifest.toJson());
}

bool
FilesystemRepoStoreBackend::isDigestReferencedUnlocked(
  const std::string& digest, const std::string& exceptObjectName) const
{
  const fs::path directory = fs::path(m_rootPath) / "manifests";
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    try {
      const auto manifest = parseManifestJson(readText(entry.path()));
      if (manifest.objectName != exceptObjectName && manifest.sha256 == digest &&
          !isMetadataOnly(manifest)) {
        return true;
      }
    }
    catch (const std::exception&) {
      // Keep malformed sidecars for recovery/diagnosis; do not delete a
      // payload when its ownership cannot be established.
      return true;
    }
  }
  return false;
}

bool
FilesystemRepoStoreBackend::isMetadataOnly(
  const RepoObjectManifest& manifest) noexcept
{
  return manifest.objectType == "ndn-segmented-data" ||
         manifest.segmentCount > 1 || manifest.packetNames.size() > 1;
}

void
FilesystemRepoStoreBackend::reserveDiskUnlocked(
  const RepoObjectManifest& manifest)
{
  const auto generation = effectiveGeneration(manifest);
  const auto found = m_reservations.find(manifest.objectName);
  if (found != m_reservations.end()) {
    auto canonicalManifest = manifest;
    canonicalManifest.generation = generation;
    if (found->second.digest != manifest.sha256 ||
        found->second.size != manifest.size ||
        found->second.generation != generation ||
        found->second.canonicalIdentity != canonicalManifest.toJson()) {
      throw std::runtime_error("repo-generation-conflict: reservation identity mismatch");
    }
    return;
  }
  std::error_code error;
  const auto available = fs::space(m_rootPath, error).available;
  if (error) {
    throw std::runtime_error("repo-file-disk-space-query-failed: " +
                             error.message());
  }
  if (manifest.size > std::numeric_limits<uint64_t>::max() - m_reservedBytes ||
      m_reservedBytes + manifest.size > available) {
    throw std::runtime_error("repo-file-disk-reservation-failed: insufficient space");
  }
  auto canonicalManifest = manifest;
  canonicalManifest.generation = generation;
  m_reservations.emplace(manifest.objectName,
                         Reservation{canonicalManifest.toJson(), manifest.sha256,
                                     manifest.size, generation});
  m_reservedBytes += manifest.size;
}

void
FilesystemRepoStoreBackend::releaseReservationUnlocked(
  const std::string& objectName)
{
  const auto found = m_reservations.find(objectName);
  if (found == m_reservations.end()) {
    return;
  }
  m_reservedBytes -= std::min(m_reservedBytes, found->second.size);
  m_reservations.erase(found);
}

std::shared_ptr<RepoStoreBackend>
makeFilesystemRepoStore(const std::string& rootPath, uint64_t maxRangeBytes,
                        uint64_t vectorCompatibilityThreshold,
                        std::string ownerId)
{
  return std::make_shared<FilesystemRepoStoreBackend>(
    rootPath, maxRangeBytes, vectorCompatibilityThreshold, std::move(ownerId));
}

} // namespace ndnsf_distributed_repo
