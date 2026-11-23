#include "ControllerGenerationStore.hpp"

#include "AbeHistoricalAuthorization.hpp"
#include "AbeHistoricalKeyRequest.hpp"
#include "SealedField.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework {
namespace {

/** A context string binds a sealed secret to the slot it belongs in. */
std::string
materialSealContext()
{
  return "ndnsf/controller/abe-material";
}

std::string
historySealContext(uint64_t generation, uint64_t epoch)
{
  return "ndnsf/controller/abe-history/" + std::to_string(generation) + "/" +
         std::to_string(epoch);
}

constexpr char STATE_MAGIC[] = "NDNSFCS2";
constexpr char ABE_STATE_MAGIC[] = "NDNSFCS3";
// CS4 = CS3 plus the archive of superseded ABE generations.  It is written only when
// that archive is non-empty, so a store with no history keeps the older magic and an
// older binary can still read it.
constexpr char ABE_HISTORY_MAGIC[] = "NDNSFCS6";
// CS8 is CS6 plus the version the material was generated for, which the digest covers. A store whose material has no
// version keeps writing CS6, so an older binary still reads it and a newer one reads both.
constexpr char ABE_HISTORY_VERSIONED_MAGIC[] = "NDNSFCS8";
constexpr char ABE_PUBLIC_PACKET_MAGIC[] = "NDNSFCSA";
// CS9 authenticates the complete inner state. The inner format still seals the master secrets.
constexpr char AUTHENTICATED_STATE_MAGIC[] = "NDNSFCS9";
constexpr char WITNESS_STATE_MAGIC[] = "NDNSFCSA";
// CS5 = CS4 without a live generation: the archive outlives the current material, so a
// failed rotation that left no material must not silently drop it.
// CS7 keeps the same entry layout as CS6 but carries no live material, so the two writers stay
// distinguishable in the file itself.
constexpr char HISTORY_ONLY_MAGIC[] = "NDNSFCS7";
// CS4/CS5 recorded archive entries before the layout gained the published name and the wire
// digest.  They are read explicitly and upgraded on the next write; reusing either magic for the
// newer layout would make an old file parse as garbage.
constexpr char LEGACY_HISTORY_MAGIC[] = "NDNSFCS4";
constexpr char LEGACY_HISTORY_ONLY_MAGIC[] = "NDNSFCS5";
constexpr size_t STATE_MAGIC_SIZE = sizeof(STATE_MAGIC) - 1;
constexpr char LOCK_MAGIC[] = "NDNSF-FLOCK-1";
constexpr size_t MAX_LEASE_BYTES = 16384;
constexpr size_t MAX_ABE_FIELD_BYTES = 1U << 20;
constexpr size_t MAX_AUTHENTICATED_STATE_BYTES = 64U << 20;
constexpr size_t STATE_MAC_BYTES = 32;
constexpr std::string_view CHECKPOINT_MAGIC = "NDNSF-CONTROLLER-CHECKPOINT-v1\n";

std::optional<std::string>
readPrivateGrantFile(const std::filesystem::path& path, size_t limit);

void
writePrivateGrantFile(const std::filesystem::path& path, const std::string& bytes, uint64_t fence);

std::string
uniqueTemporaryPath(const std::filesystem::path& path, uint64_t fence)
{
  return path.string() + ".tmp." + std::to_string(fence) + "." +
         std::to_string(ndn::random::generateSecureWord64());
}

void
syncStateDirectory(const std::filesystem::path& path)
{
  const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    throw ControllerGenerationStore::CommitUncertain("cannot open Controller state directory");
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0)
    throw ControllerGenerationStore::CommitUncertain("cannot flush Controller state directory");
}

bool
isPrivateLease(int fd, const std::filesystem::path& path)
{
  struct stat opened{}, visible{};
  return fd >= 0 && ::fstat(fd, &opened) == 0 && ::lstat(path.c_str(), &visible) == 0 &&
         S_ISREG(opened.st_mode) && opened.st_nlink == 1 && opened.st_uid == ::geteuid() &&
         (opened.st_mode & 0077) == 0 && opened.st_dev == visible.st_dev &&
         opened.st_ino == visible.st_ino;
}

std::string
readStateBytes(int fd, size_t size, off_t offset)
{
  if (size > MAX_AUTHENTICATED_STATE_BYTES)
    throw std::runtime_error("Controller generation state exceeds its byte limit");
  std::string bytes(size, '\0');
  for (size_t read = 0; read < bytes.size();) {
    const auto count = ::pread(fd, bytes.data() + read, bytes.size() - read,
                               offset + static_cast<off_t>(read));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot read authenticated Controller generation state");
    read += static_cast<size_t>(count);
  }
  return bytes;
}

std::array<unsigned char, STATE_MAC_BYTES>
stateMac(const std::vector<uint8_t>& secretKey, const std::string& keyId,
         const std::string& owner, std::string_view inner)
{
  if (secretKey.size() != 32 || owner.empty())
    throw std::runtime_error("Controller generation authentication key or owner unavailable");
  std::string context = "ndnsf/controller/state-v9/";
  context += std::to_string(owner.size()) + "/" + owner + "/" + keyId;
  std::array<unsigned char, STATE_MAC_BYTES> macKey{};
  std::array<unsigned char, STATE_MAC_BYTES> mac{};
  const auto* derived = HMAC(EVP_sha256(), secretKey.data(), static_cast<int>(secretKey.size()),
                            reinterpret_cast<const unsigned char*>(context.data()),
                            context.size(), macKey.data(), nullptr);
  const auto* authenticated = derived ?
    HMAC(EVP_sha256(), macKey.data(), static_cast<int>(macKey.size()),
         reinterpret_cast<const unsigned char*>(inner.data()), inner.size(), mac.data(), nullptr) :
    nullptr;
  OPENSSL_cleanse(macKey.data(), macKey.size());
  if (authenticated == nullptr)
    throw std::runtime_error("cannot authenticate Controller generation state");
  return mac;
}

class PrivateFileInputBuffer : public std::streambuf
{
public:
  explicit PrivateFileInputBuffer(int fd)
    : m_fd(fd)
  {
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data());
  }

  ~PrivateFileInputBuffer() override
  {
    OPENSSL_cleanse(m_buffer.data(), m_buffer.size());
    ::close(m_fd);
  }

protected:
  int_type underflow() override
  {
    if (gptr() < egptr())
      return traits_type::to_int_type(*gptr());
    ssize_t count;
    do {
      count = ::read(m_fd, m_buffer.data(), m_buffer.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      throw std::runtime_error("cannot read Controller generation state");
    if (count == 0)
      return traits_type::eof();
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + count);
    return traits_type::to_int_type(*gptr());
  }

private:
  int m_fd;
  std::array<char, 8192> m_buffer{};
};

class StateSnapshotBuffer : public std::streambuf
{
public:
  explicit StateSnapshotBuffer(std::string& bytes)
  {
    setg(bytes.data(), bytes.data(), bytes.data() + bytes.size());
  }
};

struct LeaseRecord
{
  std::string owner;
  uint64_t fence = 0;
};

std::optional<LeaseRecord>
readLease(int fd)
{
  std::array<char, MAX_LEASE_BYTES + 1> bytes;
  const auto count = ::pread(fd, bytes.data(), bytes.size(), 0);
  if (count <= 0 || static_cast<size_t>(count) > MAX_LEASE_BYTES)
    return std::nullopt;
  std::istringstream input(std::string(bytes.data(), static_cast<size_t>(count)));
  std::string marker;
  LeaseRecord record;
  std::string trailing;
  if (!std::getline(input, marker) || marker != LOCK_MAGIC ||
      !(input >> record.owner >> record.fence) || record.fence == 0 || (input >> trailing))
    return std::nullopt;
  return record;
}

std::atomic<uint64_t> nextFence{1};

void
writeAll(int fd, const char* bytes, size_t size)
{
  size_t written = 0;
  while (written < size) {
    const auto count = ::write(fd, bytes + written, size - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot write Controller generation state");
    written += static_cast<size_t>(count);
  }
}

void
writeU64(int fd, uint64_t value)
{
  std::array<char, 8> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xff);
  writeAll(fd, bytes.data(), bytes.size());
}

uint64_t
readU64(std::istream& stream)
{
  uint64_t value = 0;
  for (int shift = 56; shift >= 0; shift -= 8) {
    const int byte = stream.get();
    if (byte == std::char_traits<char>::eof())
      throw std::runtime_error("truncated Controller generation state");
    value = (value << 8) | static_cast<uint8_t>(byte);
  }
  return value;
}

void
writeString(int fd, const std::string& value)
{
  writeU64(fd, value.size());
  writeAll(fd, value.data(), value.size());
}

void
readStringInto(std::istream& stream, std::string& value, size_t limit, bool allowEmpty = false)
{
  const auto length = readU64(stream);
  if ((!allowEmpty && length == 0) || length > limit)
    throw std::runtime_error("invalid Controller ABE state field length");
  value.assign(static_cast<size_t>(length), '\0');
  if (length == 0)
    return;
  stream.read(&value[0], static_cast<std::streamsize>(value.size()));
  if (stream.gcount() != static_cast<std::streamsize>(value.size()))
    throw std::runtime_error("truncated Controller ABE state");
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
             (ch >= 'A' && ch <= 'F');
    });
}

bool
isValidAbeMaterial(const ControllerGenerationStore::AbeMaterial& material, bool checkSecret = true)
{
  return !material.owner.empty() && material.owner.size() <= MAX_LEASE_BYTES - 64 &&
    material.owner.find_first_of("\r\n\t ") == std::string::npos &&
    isDigest(material.signingKeyDigest) && isDigest(material.policyDigest) &&
    !material.publicParameters.empty() && material.publicParameters.size() <= MAX_ABE_FIELD_BYTES &&
    (!checkSecret || (!material.masterSecret.empty() &&
                      material.masterSecret.size() <= MAX_ABE_FIELD_BYTES)) &&
    material.publicParametersPacket.size() <= ndn::MAX_NDN_PACKET_SIZE &&
    (material.publicParametersPacket.empty() || material.publicParametersVersion != 0);
}

std::string
abeMaterialDigest(const ControllerGenerationStore::AbeMaterial& material)
{
  ndn::util::Sha256 hash;
  // A versioned material is digested under its own prefix, so the older layout keeps validating for states written
  // before the version existed.
  hash << (material.version.isValid() ? std::string("NDNSF-CONTROLLER-ABE-STATE-V2")
                                      : std::string("NDNSF-CONTROLLER-ABE-STATE-V1"));
  for (const auto* field : {&material.owner, &material.signingKeyDigest,
                            &material.policyDigest, &material.publicParameters,
                            &material.masterSecret}) {
    hash << (std::to_string(field->size()) + ":");
    hash << *field;
  }
  hash << std::string(material.rotationRequired ? "1" : "0");
  if (material.version.isValid()) {
    hash << std::to_string(material.version.controllerGenerationTimestamp);
    hash << std::to_string(material.version.controllerEpoch);
  }
  if (material.publicParametersVersion != 0) {
    hash << ":" + std::to_string(material.publicParametersVersion);
    hash << std::to_string(material.publicParametersPacket.size()) + ":";
    hash << material.publicParametersPacket;
  }
  return "sha256:" + hash.toString();
}

std::string
abeHistoryDigest(const ControllerGenerationStore::AbeHistoryEntry& entry, bool bound = true)
{
  ndn::util::Sha256 hash;
  hash << std::string("NDNSF-CONTROLLER-ABE-HISTORY-V1");
  hash << std::to_string(entry.generationTimestamp);
  hash << std::to_string(entry.epoch);
  const auto append = [&hash] (const std::string& field) {
    hash << (std::to_string(field.size()) + ":");
    hash << field;
  };
  if (bound) {
    append(entry.publicParametersName);
    append(entry.publicParametersWireDigest);
    if (!entry.publicParametersPacket.empty())
      append(entry.publicParametersPacket);
  }
  append(entry.publicParameters);
  append(entry.masterSecret);
  return "sha256:" + hash.toString();
}

std::string
sha256Hex(const std::string& bytes)
{
  ndn::util::Sha256 hash;
  if (!bytes.empty())
    hash << bytes;
  auto hex = hash.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return hex;
}

bool
sameRevocations(const std::vector<RevocationTarget>& left,
                const std::vector<RevocationTarget>& right)
{
  if (left.size() != right.size())
    return false;
  for (size_t i = 0; i < left.size(); ++i) {
    const auto a = left[i].wireEncode();
    const auto b = right[i].wireEncode();
    if (a.size() != b.size() || !std::equal(a.data(), a.data() + a.size(), b.data()))
      return false;
  }
  return true;
}

} // namespace

ControllerGenerationStore::AbeMaterial&
ControllerGenerationStore::AbeMaterial::operator=(const AbeMaterial& other)
{
  if (this != &other) {
    if (!masterSecret.empty())
      OPENSSL_cleanse(&masterSecret[0], masterSecret.size());
    owner = other.owner;
    signingKeyDigest = other.signingKeyDigest;
    policyDigest = other.policyDigest;
    publicParameters = other.publicParameters;
    publicParametersPacket = other.publicParametersPacket;
    publicParametersVersion = other.publicParametersVersion;
    masterSecret = other.masterSecret;
    // The version is part of the material's identity: without it a reloaded state cannot tell which version the keys
    // were generated for, and the publication check that depends on it is skipped.
    version = other.version;
    rotationRequired = other.rotationRequired;
  }
  return *this;
}

ControllerGenerationStore::AbeMaterial::~AbeMaterial()
{
  if (!masterSecret.empty())
    OPENSSL_cleanse(&masterSecret[0], masterSecret.size());
}

ControllerGenerationStore::AbeHistoryEntry&
ControllerGenerationStore::AbeHistoryEntry::operator=(const AbeHistoryEntry& other)
{
  if (this != &other) {
    if (!masterSecret.empty())
      OPENSSL_cleanse(&masterSecret[0], masterSecret.size());
    generationTimestamp = other.generationTimestamp;
    epoch = other.epoch;
    publicParameters = other.publicParameters;
    publicParametersPacket = other.publicParametersPacket;
    masterSecret = other.masterSecret;
    publicParametersName = other.publicParametersName;
    publicParametersWireDigest = other.publicParametersWireDigest;
    digest = other.digest;
    reason = other.reason;
  }
  return *this;
}

ControllerGenerationStore::AbeHistoryEntry::~AbeHistoryEntry()
{
  if (!masterSecret.empty())
    OPENSSL_cleanse(&masterSecret[0], masterSecret.size());
}

ControllerGenerationStore::ControllerGenerationStore(std::filesystem::path statePath)
  : m_statePath(std::move(statePath))
  , m_lockPath(m_statePath.string() + ".lock")
{
}

ControllerGenerationStore::~ControllerGenerationStore()
{
  releaseWriter();
}

std::unique_ptr<ControllerGenerationStore>
ControllerGenerationStore::openDefault(
  const std::string& identity, const std::filesystem::path& legacyStatePath)
{
  return openDefault(identity, legacyStatePath, {}, {});
}

std::unique_ptr<ControllerGenerationStore>
ControllerGenerationStore::openDefault(
  const std::string& identity, const std::filesystem::path& legacyStatePath,
  std::vector<uint8_t> sealKey, std::string sealKeyId, bool requireAuthentication)
{
  const ndn::Name name(identity);
  if (name.empty() || !legacyStatePath.is_absolute())
    throw std::invalid_argument("Controller default store requires identity and absolute legacy path");
  const auto owner = name.toUri(ndn::name::UriFormat::CANONICAL);
  ndn::util::Sha256 hash;
  hash << owner;
  const auto currentPath = legacyStatePath.parent_path() /
    ("ndnsf-controller-generation-" + std::to_string(::geteuid()) + "-" +
     hash.toString() + ".state");
  const auto present = [] (const std::filesystem::path& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) == 0)
      return true;
    if (errno != ENOENT)
      throw std::runtime_error("cannot inspect Controller generation state: " + path.string());
    return false;
  };
  const bool legacyState = present(legacyStatePath);
  const bool legacyLock = present(legacyStatePath.string() + ".lock");
  const bool currentState = present(currentPath);
  const bool currentLock = present(currentPath.string() + ".lock");
  if (legacyState != legacyLock || currentState != currentLock ||
      ((legacyState || legacyLock) && (currentState || currentLock))) {
    throw std::runtime_error("ambiguous or incomplete Controller generation state");
  }
  const auto path = legacyState ? legacyStatePath : currentPath;
  if (legacyState || currentState) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != ::geteuid() || info.st_nlink != 1 || (info.st_mode & 0022) != 0) {
      throw std::runtime_error("unsafe Controller generation state: " + path.string());
    }
  }
  auto store = std::make_unique<ControllerGenerationStore>(path);
  store->setSecretSeal(std::move(sealKey), std::move(sealKeyId), requireAuthentication);
  if (!store->acquireWriterImpl(owner, true))
    throw std::runtime_error("Controller generation writer unavailable or identity differs: " +
                             path.string());
  return store;
}

bool
ControllerGenerationStore::acquireWriter(const std::string& owner)
{
  return acquireWriterImpl(owner, false);
}

bool
ControllerGenerationStore::acquireWriterImpl(const std::string& owner, bool requireSameOwner)
{
  if (owner.empty() || owner.size() > MAX_LEASE_BYTES - 64 || m_lockFd >= 0 ||
      owner.find_first_of("\r\n\t ") != std::string::npos)
    return false;
  int checkpointFd = -1;
  auto closeCheckpoint = ndn::make_scope_exit([&] {
    if (checkpointFd >= 0)
      ::close(checkpointFd);
  });
  if (!m_checkpointPath.empty()) {
    const auto checkpointLock = m_checkpointPath.string() + ".lock";
    checkpointFd = ::open(checkpointLock.c_str(),
      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (!isPrivateLease(checkpointFd, checkpointLock) ||
        ::flock(checkpointFd, LOCK_EX | LOCK_NB) != 0)
      return false;
  }
  int fd = ::open(m_lockPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  const bool created = fd >= 0;
  if (fd < 0 && errno == EEXIST)
    fd = ::open(m_lockPath.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;
  struct stat info{};
  struct stat pathInfo{};
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0 || ::fstat(fd, &info) != 0 ||
      ::lstat(m_lockPath.c_str(), &pathInfo) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0 || info.st_nlink != 1 ||
      info.st_dev != pathInfo.st_dev || info.st_ino != pathInfo.st_ino) {
    ::close(fd);
    return false;
  }
  const auto previous = created ? std::optional<LeaseRecord>{} : readLease(fd);
  if ((!created && !previous) ||
      (requireSameOwner && ((!created && previous->owner != owner) ||
                           (created && std::filesystem::exists(m_statePath))))) {
    // An older sentinel-only writer does not hold a kernel lock. Empty or
    // unknown existing records also require explicit closed-owner migration.
    ::close(fd);
    return false;
  }
  if (requireSameOwner) {
    try {
      // Validate under the old lease before replacing its owner/fence record.
      loadState(owner);
    }
    catch (...) {
      ::close(fd);
      return false;
    }
  }
  m_lockFd = fd;
  m_checkpointFd = std::exchange(checkpointFd, -1);
  m_owner = owner;
  m_fence = nextFence.fetch_add(1);
  const auto contents = std::string(LOCK_MAGIC) + "\n" + m_owner + "\n" +
                        std::to_string(m_fence) + "\n";
  if (::ftruncate(fd, 0) != 0 ||
      ::pwrite(fd, contents.data(), contents.size(), 0) !=
        static_cast<ssize_t>(contents.size()) || ::fsync(fd) != 0) {
    releaseWriter();
    return false;
  }
  return true;
}

bool
ControllerGenerationStore::ownsWriter() const
{
  if (m_owner.empty() || m_fence == 0 || m_lockFd < 0)
    return false;
  if (!m_checkpointPath.empty() && !isPrivateLease(m_checkpointFd,
                                                   m_checkpointPath.string() + ".lock"))
    return false;
  struct stat info{};
  struct stat pathInfo{};
  if (::fstat(m_lockFd, &info) != 0 || ::lstat(m_lockPath.c_str(), &pathInfo) != 0 ||
      !S_ISREG(pathInfo.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0077) != 0 || info.st_nlink != 1 ||
      info.st_dev != pathInfo.st_dev || info.st_ino != pathInfo.st_ino)
    return false;
  const auto record = readLease(m_lockFd);
  return record && record->owner == m_owner && record->fence == m_fence;
}

uint64_t
ControllerGenerationStore::fencingValue() const
{
  return m_fence;
}

void
ControllerGenerationStore::releaseWriter()
{
  // Keep the inode: unlinking lets another writer lock a new file while an
  // existing contender still holds the old descriptor.
  if (m_lockFd >= 0)
    ::close(m_lockFd);
  if (m_checkpointFd >= 0)
    ::close(m_checkpointFd);
  m_lockFd = -1;
  m_checkpointFd = -1;
  m_owner.clear();
  m_fence = 0;
}

void
ControllerGenerationStore::requireWriter() const
{
  if (!ownsWriter())
    throw std::runtime_error("Controller generation writer lease is not held");
}

std::optional<ControllerVersion>
ControllerGenerationStore::load() const
{
  const auto version = loadState().version;
  if (!version.isValid())
    return std::nullopt;
  return version;
}

ControllerGenerationStore::PersistedState
ControllerGenerationStore::loadState(const std::string& verifiedOwner, StateReadScope scope,
                                     ControllerVersion target) const
{
  if (m_checkpointPath.empty())
    return readState(m_statePath, verifiedOwner, {}, scope, target);
  requireWriter();
  const auto checkpoint = readPrivateGrantFile(m_checkpointPath,
                                                CHECKPOINT_MAGIC.size() + STATE_MAC_BYTES);
  if (!checkpoint)
    return readState(m_statePath, verifiedOwner, {}, scope, target);
  if (checkpoint->size() != CHECKPOINT_MAGIC.size() + STATE_MAC_BYTES ||
      checkpoint->compare(0, CHECKPOINT_MAGIC.size(), CHECKPOINT_MAGIC) != 0)
    throw std::runtime_error("invalid Controller recovery checkpoint");
  const std::string_view expected = std::string_view(*checkpoint).substr(CHECKPOINT_MAGIC.size());
  try {
    return readState(m_statePath, verifiedOwner, expected, scope, target);
  }
  catch (const std::runtime_error&) {
    // Public and per-generation reads cannot drive full-state recovery.
    if (scope != StateReadScope::ALL)
      throw;
    // A crash after the checkpoint commit can leave the matching state staged.
    const auto staged = m_statePath.string() + ".prepared";
    PersistedState recovered;
    try {
      // Promotion validates the complete recovery candidate before changing the state path.
      recovered = readState(staged, verifiedOwner, expected, StateReadScope::ALL, {});
    }
    catch (const std::runtime_error&) {
      throw std::runtime_error("Controller recovery checkpoint has no matching state");
    }
    requireWriter();
    if (::rename(staged.c_str(), m_statePath.c_str()) != 0)
      throw CommitUncertain("cannot promote the Controller recovery candidate");
    syncStateDirectory(m_statePath);
    return recovered;
  }
}

ControllerGenerationStore::PersistedState
ControllerGenerationStore::readState(const std::filesystem::path& path,
                                    const std::string& verifiedOwner,
                                    std::string_view expectedMac, StateReadScope scope,
                                    const ControllerVersion& target) const
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT && expectedMac.empty() && m_importDigest.empty())
      return {};
    throw std::runtime_error("cannot open Controller generation state");
  }
  PrivateFileInputBuffer buffer(fd);
  struct stat info{};
  if (::fstat(fd, &info) != 0)
    throw std::runtime_error("cannot inspect Controller generation state");
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
      (info.st_mode & 0022) != 0)
    throw std::runtime_error("unsafe Controller generation state");
  std::istream state(&buffer);
  state.exceptions(std::ios::badbit);

  std::string importedBytes;
  auto cleanse = ndn::make_scope_exit([&] {
    OPENSSL_cleanse(importedBytes.data(), importedBytes.size());
  });
  if (!m_checkpointPath.empty() && expectedMac.empty()) {
    if (m_importDigest.empty() || info.st_size <= 0 ||
        info.st_size > static_cast<off_t>(MAX_AUTHENTICATED_STATE_BYTES))
      throw std::runtime_error("Controller recovery checkpoint is missing for existing state");
    importedBytes = readStateBytes(fd, static_cast<size_t>(info.st_size), 0);
    if (m_importDigest != "sha256:" + sha256Hex(importedBytes))
      throw std::runtime_error("Controller recovery import digest mismatch");
  }
  StateSnapshotBuffer importedBuffer(importedBytes);
  std::istream imported(&importedBuffer);
  auto* input = importedBytes.empty() ? &state : &imported;
  char magic[STATE_MAGIC_SIZE] = {};
  input->read(magic, STATE_MAGIC_SIZE);
  if (input->gcount() != static_cast<std::streamsize>(STATE_MAGIC_SIZE))
    throw std::runtime_error("truncated Controller generation state");

  std::istringstream verified;
  std::istream* inner = input;
  const bool bindsWitness = std::equal(std::begin(magic), std::end(magic), WITNESS_STATE_MAGIC);
  std::optional<GrantWitnessRef> witness;
  if (bindsWitness || std::equal(std::begin(magic), std::end(magic), AUTHENTICATED_STATE_MAGIC)) {
    constexpr auto overhead = static_cast<off_t>(STATE_MAGIC_SIZE + STATE_MAC_BYTES);
    if ((info.st_mode & 0077) != 0 ||
        info.st_size < overhead + static_cast<off_t>(STATE_MAGIC_SIZE) ||
        info.st_size > static_cast<off_t>(MAX_AUTHENTICATED_STATE_BYTES))
      throw std::runtime_error("invalid authenticated Controller generation state size or mode");
    auto bytes = importedBytes.empty() ?
      readStateBytes(fd, static_cast<size_t>(info.st_size) - STATE_MAGIC_SIZE, STATE_MAGIC_SIZE) :
      importedBytes.substr(STATE_MAGIC_SIZE);
    const size_t length = bytes.size() - STATE_MAC_BYTES;
    const auto mac = stateMac(m_secretKey, m_secretKeyId,
                              verifiedOwner.empty() ? m_owner : verifiedOwner,
                              std::string_view(bytes.data(), length));
    if (CRYPTO_memcmp(bytes.data() + length, mac.data(), mac.size()) != 0)
      throw std::runtime_error("Controller generation state authentication failed");
    if (!expectedMac.empty() && CRYPTO_memcmp(expectedMac.data(), mac.data(), mac.size()) != 0)
      throw std::runtime_error("Controller generation state differs from the recovery checkpoint");
    bytes.resize(length);
    // Authenticate the layout discriminator along with the witness-bearing state.
    if (bindsWitness && bytes.compare(0, STATE_MAGIC_SIZE, WITNESS_STATE_MAGIC) != 0)
      throw std::runtime_error("invalid Controller grant witness state marker");
    verified.str(std::move(bytes));
    if (bindsWitness) {
      verified.ignore(STATE_MAGIC_SIZE);
      witness.emplace();
      readStringInto(verified, witness->signerDigest, 71);
      readStringInto(verified, witness->wireDigest, 71);
      if (!isDigest(witness->signerDigest) || !isDigest(witness->wireDigest))
        throw std::runtime_error("invalid Controller grant witness commitment");
    }
    verified.read(magic, STATE_MAGIC_SIZE);
    inner = &verified;
  }
  else if (scope != StateReadScope::ALL || !expectedMac.empty() ||
           (m_requireAuthentication && importedBytes.empty())) {
    throw std::runtime_error("Controller requires authenticated state or a pinned legacy import");
  }
  // Scoped readers have authenticated the whole container above. They validate the
  // plaintext-dependent entry digest only for a secret they actually open.
  auto result = decodeState(*inner, std::string_view(magic, STATE_MAGIC_SIZE),
                            info.st_mode, verifiedOwner, scope, target);
  result.grantWitness = std::move(witness);
  return result;
}

ControllerGenerationStore::PersistedState
ControllerGenerationStore::decodeState(std::istream& state, std::string_view magic, unsigned int mode,
                                      const std::string& verifiedOwner, StateReadScope scope,
                                      const ControllerVersion& target) const
{
  PersistedState result;
  const bool openAll = scope == StateReadScope::ALL;
  const bool hasPublishedPackets = magic == ABE_PUBLIC_PACKET_MAGIC;
  const bool hasMaterialVersion = hasPublishedPackets || magic == ABE_HISTORY_VERSIONED_MAGIC;
  const bool hasHistory = hasMaterialVersion || magic == ABE_HISTORY_MAGIC;
  const bool legacyHistoryOnly = magic == LEGACY_HISTORY_ONLY_MAGIC;
  const bool hasLegacyHistory = magic == LEGACY_HISTORY_MAGIC || legacyHistoryOnly;
  if (hasLegacyHistory && !openAll)
    throw std::runtime_error("Controller archive requires full legacy migration");
  // A history-only record carries no live material, so it must not be parsed as if it did.
  const auto hasAbeMaterial = hasPublishedPackets ? readU64(state) :
    static_cast<uint64_t>(hasHistory || (hasLegacyHistory && !legacyHistoryOnly) ||
                          magic == ABE_STATE_MAGIC);
  if (hasAbeMaterial > 1)
    throw std::runtime_error("invalid Controller ABE material marker");
  const bool hasHistoryOnly = magic == HISTORY_ONLY_MAGIC;
  if (hasAbeMaterial && (mode & 0077) != 0)
    throw std::runtime_error("Controller ABE state must be private");
  if (hasAbeMaterial || hasHistoryOnly || hasLegacyHistory || magic == STATE_MAGIC) {
    result.version.controllerGenerationTimestamp = readU64(state);
    result.version.controllerEpoch = readU64(state);
    const auto count = readU64(state);
    if (!result.version.isValid() || count > 100000)
      throw std::runtime_error("invalid Controller generation state");
    result.revocations.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
      const auto length = readU64(state);
      if (length == 0 || length > (1ULL << 20))
        throw std::runtime_error("invalid persisted revocation length");
      std::vector<uint8_t> bytes(static_cast<size_t>(length));
      state.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
      if (state.gcount() != static_cast<std::streamsize>(bytes.size()))
        throw std::runtime_error("truncated persisted revocation");
      ndn::Block block(ndn::Buffer(bytes.data(), bytes.size()));
      RevocationTarget target;
      if (!target.wireDecode(block))
        throw std::runtime_error("invalid persisted revocation");
      result.revocations.push_back(std::move(target));
    }
    if (hasAbeMaterial) {
      result.abeMaterial.emplace();
      auto& material = *result.abeMaterial;
      const auto rotationRequired = readU64(state);
      if (rotationRequired > 1)
        throw std::runtime_error("invalid Controller ABE rotation state");
      material.rotationRequired = rotationRequired != 0;
      readStringInto(state, material.owner, MAX_LEASE_BYTES - 64);
      readStringInto(state, material.signingKeyDigest, 71);
      readStringInto(state, material.policyDigest, 71);
      readStringInto(state, material.publicParameters, MAX_ABE_FIELD_BYTES);
      readStringInto(state, material.masterSecret, MAX_ABE_FIELD_BYTES);
      if (!openAll) {
        if (!isSealedField(material.masterSecret))
          throw std::runtime_error("unsealed Controller ABE material");
        material.masterSecret.clear();
      }
      else if (isSealedField(material.masterSecret)) {
        std::string opened;
        if (m_secretKey.empty() || !openField(m_secretKey, material.masterSecret,
                                              materialSealContext(), opened))
          throw std::runtime_error("Controller ABE state is sealed and cannot be opened");
        material.masterSecret = std::move(opened);
      }
      if (hasMaterialVersion) {
        material.version.controllerGenerationTimestamp = readU64(state);
        material.version.controllerEpoch = readU64(state);
        if (!material.version.isValid())
          throw std::runtime_error("invalid Controller ABE material version");
      }
      if (hasPublishedPackets) {
        material.publicParametersVersion = readU64(state);
        readStringInto(state, material.publicParametersPacket, ndn::MAX_NDN_PACKET_SIZE, true);
      }
      std::string checksum;
      readStringInto(state, checksum, 71);
      if (!isValidAbeMaterial(material, openAll) || !isDigest(checksum) ||
          (openAll && checksum != abeMaterialDigest(material)))
        throw std::runtime_error("invalid Controller ABE state");
      if (!hasHistory && !hasLegacyHistory &&
          state.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("invalid Controller ABE state");
    }
  }
  else {
    // Read-only compatibility with the original version-only record.  The
    // next atomic write upgrades it to the revocation-preserving format.
    uint64_t generation = 0;
    for (const char byte : magic)
      generation = (generation << 8) | static_cast<uint8_t>(byte);
    result.version.controllerGenerationTimestamp = generation;
    result.version.controllerEpoch = readU64(state);
    if (!result.version.isValid() || state.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("invalid Controller generation state");
  }
    if (hasHistory || hasHistoryOnly || hasLegacyHistory) {
      const auto archived = readU64(state);
      if (archived > 64)
        throw std::runtime_error("invalid Controller ABE history length");
      result.abeHistory.reserve(static_cast<size_t>(archived));
      for (uint64_t i = 0; i < archived; ++i) {
        AbeHistoryEntry entry;
        entry.generationTimestamp = readU64(state);
        entry.epoch = readU64(state);
        const bool selected = scope == StateReadScope::ONE_HISTORICAL_SECRET &&
          ControllerVersion{entry.generationTimestamp, entry.epoch} == target;
        if (selected && !result.abeHistory.empty())
          throw std::runtime_error("duplicate historical Controller generation");
        const bool openEntry = openAll || selected;
        readStringInto(state, entry.publicParameters, MAX_ABE_FIELD_BYTES);
        readStringInto(state, entry.masterSecret, MAX_ABE_FIELD_BYTES);
        const bool sealed = isSealedField(entry.masterSecret);
        if (!openAll && !sealed)
          throw std::runtime_error("unsealed Controller ABE archive entry");
        if (!openEntry) {
          entry.masterSecret.clear();
        }
        else if (sealed) {
          std::string opened;
          if (m_secretKey.empty() ||
              !openField(m_secretKey, entry.masterSecret,
                         historySealContext(entry.generationTimestamp, entry.epoch), opened))
            throw std::runtime_error("archived Controller ABE generation is sealed and cannot be opened");
          entry.masterSecret = std::move(opened);
        }
        if (hasLegacyHistory) {
          // Two writers used CS4/CS5: the original layout, which recorded neither the published name
          // nor the wire digest, and the intermediate one, which recorded both.  The next field
          // separates them - the original stores a `sha256:` digest, the intermediate a name.
          std::string next;
          readStringInto(state, next, MAX_LEASE_BYTES - 64);
          const bool intermediate = next.rfind("/", 0) == 0;
          if (!intermediate) {
            // Original layout: derive the missing fields, then re-seal so the next write is bound.
            entry.digest = next;
            readStringInto(state, entry.reason, MAX_LEASE_BYTES - 64);
          }
          else {
            entry.publicParametersName = next;
            readStringInto(state, entry.publicParametersWireDigest, 71);
            readStringInto(state, entry.digest, 71);
            readStringInto(state, entry.reason, MAX_LEASE_BYTES - 64);
            if (entry.digest != abeHistoryDigest(entry))
              throw std::runtime_error("invalid archived Controller ABE generation");
            result.abeHistory.push_back(std::move(entry));
            continue;
          }
          const auto& owner = verifiedOwner.empty() ? m_owner : verifiedOwner;
          if (owner.empty() || entry.generationTimestamp == 0 || entry.publicParameters.empty() ||
              entry.masterSecret.empty() || entry.digest != abeHistoryDigest(entry, false) ||
              entry.generationTimestamp >
                (std::numeric_limits<uint64_t>::max() - entry.epoch) / 1000ULL)
            throw std::runtime_error("invalid legacy archived Controller ABE generation");
          entry.publicParametersWireDigest = "sha256:" + sha256Hex(entry.publicParameters);
          ndn::Name original(owner);
          original.append("PUBPARAMS").append("KP-ABE")
            .appendVersion(entry.generationTimestamp * 1000ULL + entry.epoch);
          entry.publicParametersName = original.toUri(ndn::name::UriFormat::CANONICAL);
          entry.digest = abeHistoryDigest(entry);
        }
        else {
          readStringInto(state, entry.publicParametersName, MAX_LEASE_BYTES - 64);
          readStringInto(state, entry.publicParametersWireDigest, 71);
          if (hasPublishedPackets)
            readStringInto(state, entry.publicParametersPacket, ndn::MAX_NDN_PACKET_SIZE, true);
          readStringInto(state, entry.digest, 71);
          readStringInto(state, entry.reason, MAX_LEASE_BYTES - 64);
          if (entry.generationTimestamp == 0 || !isDigest(entry.digest) ||
              (openEntry && entry.digest != abeHistoryDigest(entry)))
            throw std::runtime_error("invalid archived Controller ABE generation");
        }
        if (scope != StateReadScope::ONE_HISTORICAL_SECRET || selected)
          result.abeHistory.push_back(std::move(entry));
      }
    }
  if (state.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("invalid Controller ABE archive state");
  return result;
}

std::vector<RevocationTarget>
ControllerGenerationStore::loadRevocations() const
{
  return loadState().revocations;
}

namespace {

constexpr char ATTRIBUTE_SNAPSHOT_SCHEMA[] = "ndnsf-controller-attribute-snapshot-v1";

/** One generation's block: a header line and one line per identity. */
struct AttributeSnapshotBlock
{
  uint64_t generation = 0;
  uint64_t epoch = 0;
  std::map<std::string, std::set<std::string>> attributes;
};

std::vector<AttributeSnapshotBlock>
parseAttributeSnapshot(const std::string& text)
{
  std::vector<AttributeSnapshotBlock> blocks;
  std::size_t begin = 0;
  bool sawSchema = false;
  while (begin <= text.size()) {
    const auto end = text.find('\n', begin);
    const std::string line = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                        : end - begin);
    if (end == std::string::npos)
      begin = text.size() + 1;
    else
      begin = end + 1;
    if (line.empty())
      continue;
    if (!sawSchema) {
      if (line != ATTRIBUTE_SNAPSHOT_SCHEMA)
        return {};
      sawSchema = true;
      continue;
    }
    if (line.rfind("generation ", 0) == 0) {
      AttributeSnapshotBlock block;
      block.generation = std::strtoull(line.c_str() + 11, nullptr, 10);
      blocks.push_back(std::move(block));
      continue;
    }
    if (line.rfind("epoch ", 0) == 0) {
      if (blocks.empty())
        return {};
      blocks.back().epoch = std::strtoull(line.c_str() + 6, nullptr, 10);
      continue;
    }
    if (line.front() != '/' || blocks.empty())
      return {};
    std::istringstream fields(line);
    std::string identity;
    fields >> identity;
    std::string attribute;
    std::set<std::string> values;
    while (fields >> attribute) {
      values.insert(attribute);
    }
    blocks.back().attributes[identity] = std::move(values);
  }
  return blocks;
}

} // namespace

void
ControllerGenerationStore::setSecretSeal(std::vector<uint8_t> key, std::string keyId,
                                          bool requireAuthentication)
{
  if (!key.empty() && key.size() != 32)
    throw std::invalid_argument("the Controller secret key must be 32 bytes");
  if ((requireAuthentication || m_requireAuthentication || !m_checkpointPath.empty()) &&
      (key.empty() || keyId.empty()))
    throw std::invalid_argument("Controller authenticated state requires a seal key and ID");
  m_requireAuthentication = m_requireAuthentication || requireAuthentication;
  m_secretKey = std::move(key);
  m_secretKeyId = std::move(keyId);
}

void
ControllerGenerationStore::setRecoveryCheckpoint(std::filesystem::path path,
                                                 std::string importDigest,
                                                 std::string witnessImportDigest)
{
  if (m_lockFd >= 0 || m_secretKey.empty() || m_secretKeyId.empty() ||
      !path.is_absolute() || !m_statePath.is_absolute() || path.filename().empty() ||
      path.filename() == "." || path.filename() == ".." ||
      (!importDigest.empty() && !isDigest(importDigest)) ||
      (!witnessImportDigest.empty() && !isDigest(witnessImportDigest)))
    throw std::invalid_argument("invalid Controller recovery checkpoint configuration");
  const auto parent = std::filesystem::canonical(path.parent_path());
  const auto stateParent = std::filesystem::canonical(m_statePath.parent_path());
  const auto isPrivateDirectory = [] (const auto& directory) {
    struct stat info{};
    return ::stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == ::geteuid() && (info.st_mode & 0022) == 0;
  };
  const auto common = std::mismatch(stateParent.begin(), stateParent.end(),
                                    parent.begin(), parent.end());
  if (common.first == stateParent.end() || common.second == parent.end() ||
      !isPrivateDirectory(parent) || !isPrivateDirectory(stateParent))
    throw std::invalid_argument("Controller recovery checkpoint needs an independent private directory");
  m_checkpointPath = parent / path.filename();
  std::transform(importDigest.begin(), importDigest.end(), importDigest.begin(),
                 [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(witnessImportDigest.begin(), witnessImportDigest.end(), witnessImportDigest.begin(),
                 [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
  m_importDigest = std::move(importDigest);
  m_witnessImportDigest = std::move(witnessImportDigest);
}

void
ControllerGenerationStore::recordAttributeSnapshot(
  uint64_t generation, uint64_t epoch,
  const std::map<std::string, std::set<std::string>>& attributes, size_t retention)
{
  std::vector<AttributeSnapshotBlock> blocks;
  {
    std::ifstream existing(attributeSnapshotPath());
    if (existing) {
      std::ostringstream buffer;
      buffer << existing.rdbuf();
      blocks = parseAttributeSnapshot(buffer.str());
    }
  }
  blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                              [generation, epoch] (const AttributeSnapshotBlock& block) {
                                return block.generation == generation && block.epoch == epoch;
                              }),
               blocks.end());
  blocks.push_back(AttributeSnapshotBlock{generation, epoch, attributes});
  while (blocks.size() > std::max<size_t>(1, retention))
    blocks.erase(blocks.begin());

  std::string text(ATTRIBUTE_SNAPSHOT_SCHEMA);
  text += "\n";
  for (const auto& block : blocks) {
    if (block.generation == 0)
      continue;
    text += "generation " + std::to_string(block.generation) + "\n";
    text += "epoch " + std::to_string(block.epoch) + "\n";
    for (const auto& item : block.attributes) {
      text += item.first;
      for (const auto& attribute : item.second) {
        text += " " + attribute;
      }
      text += "\n";
    }
  }
  const auto path = attributeSnapshotPath();
  const auto temporary = std::filesystem::path(path).concat(".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot write the Controller attribute snapshot");
    out.write(text.data(), text.size());
    if (!out)
      throw std::runtime_error("cannot write the Controller attribute snapshot");
  }
  std::filesystem::permissions(temporary, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
  std::filesystem::rename(temporary, path);
}

std::optional<std::map<std::string, std::set<std::string>>>
ControllerGenerationStore::loadAttributeSnapshot(uint64_t generation, uint64_t epoch) const
{
  std::ifstream input(attributeSnapshotPath());
  if (!input)
    return std::nullopt;
  std::ostringstream buffer;
  buffer << input.rdbuf();
  for (const auto& block : parseAttributeSnapshot(buffer.str())) {
    if (block.generation == generation && block.epoch == epoch)
      return block.attributes;
  }
  return std::nullopt;
}

std::vector<ControllerGenerationStore::AbeHistoryEntry>
ControllerGenerationStore::loadAbeHistory() const
{
  return loadState().abeHistory;
}

std::vector<ControllerGenerationStore::AbeHistoryEntry>
ControllerGenerationStore::loadAbePublicHistory() const
{
  return loadState({}, StateReadScope::PUBLIC_HISTORY).abeHistory;
}

std::optional<ControllerGenerationStore::AbeHistoryEntry>
ControllerGenerationStore::loadAbeHistoryEntry(const ControllerVersion& version) const
{
  requireWriter();
  if (!version.isValid())
    throw std::invalid_argument("invalid historical Controller generation");
  const auto entries = loadState({}, StateReadScope::ONE_HISTORICAL_SECRET, version).abeHistory;
  if (entries.empty())
    return std::nullopt;
  return entries.front();
}

namespace {

/** One file per identity and version, named so a lookup needs no index. */
std::string
grantRecordName(const std::string& identity, uint64_t generation, uint64_t epoch)
{
  ndn::util::Sha256 digest;
  digest << identity;
  return digest.toString() + "." + std::to_string(generation) + "." + std::to_string(epoch);
}

constexpr size_t MAX_GRANT_RECORD_BYTES = ndn::MAX_NDN_PACKET_SIZE;
constexpr size_t MAX_GRANT_RECORDS = 65536;
constexpr size_t MAX_HISTORICAL_REPLIES = 4096;
constexpr size_t MAX_HISTORICAL_PUBLICATION_RECEIPTS = 65536;
constexpr size_t MAX_GRANT_WITNESS_BYTES =
  (MAX_GRANT_RECORDS + MAX_HISTORICAL_PUBLICATION_RECEIPTS) * 32 + 4096;

std::optional<std::string>
readPrivateGrantFile(const std::filesystem::path& path, size_t limit)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT)
      return std::nullopt;
    throw std::runtime_error("cannot open private Controller grant file");
  }
  PrivateFileInputBuffer buffer(fd);
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
      info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0 ||
      info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > limit)
    throw std::runtime_error("invalid private Controller grant file");
  std::istream input(&buffer);
  std::string bytes(static_cast<size_t>(info.st_size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      input.peek() != std::char_traits<char>::eof() || input.bad())
    throw std::runtime_error("Controller grant file changed during read");
  return bytes;
}

void
writePrivateGrantFile(const std::filesystem::path& path, const std::string& bytes, uint64_t fence)
{
  const auto temporary = uniqueTemporaryPath(path, fence);
  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create private Controller grant file");
  try {
    writeAll(fd, bytes.data(), bytes.size());
    if (::fsync(fd) != 0)
      throw std::runtime_error("cannot flush Controller grant file");
    const int result = ::close(fd);
    fd = -1;
    if (result != 0 || ::rename(temporary.c_str(), path.c_str()) != 0)
      throw std::runtime_error("cannot publish Controller grant file");
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
      throw std::runtime_error("cannot open Controller grant directory");
    const int flushed = ::fsync(dir);
    ::close(dir);
    if (flushed != 0)
      throw std::runtime_error("cannot flush Controller grant directory");
  }
  catch (...) {
    if (fd >= 0)
      ::close(fd);
    ::unlink(temporary.c_str());
    throw;
  }
}

std::filesystem::path
grantWitnessPath(const std::filesystem::path& state, const std::string& signerDigest)
{
  if (!isDigest(signerDigest))
    throw std::invalid_argument("invalid Controller grant witness signer");
  return state.string() + ".grant-witness." + signerDigest.substr(7);
}

std::filesystem::path
historicalReplyDirectory(const std::filesystem::path& state)
{
  return state.string() + ".historical-replies";
}

std::string
historicalReplyPrefix(const ControllerVersion& version)
{
  if (!version.isValid())
    throw std::invalid_argument("invalid Controller historical reply version");
  return std::to_string(version.controllerGenerationTimestamp) + "." +
         std::to_string(version.controllerEpoch) + ".";
}

std::string
historicalReplyFilename(const std::string& prefix, const ndn::Name& requestName)
{
  if (requestName.empty())
    throw std::invalid_argument("missing Controller historical reply name");
  const auto& wire = requestName.wireEncode();
  return prefix + sha256Hex(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
}

bool
isPrivateHistoricalDirectory(const std::filesystem::path& directory)
{
  struct stat info{};
  if (::lstat(directory.c_str(), &info) != 0) {
    if (errno == ENOENT)
      return false;
    throw std::runtime_error("cannot inspect Controller historical evidence directory");
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
    throw std::runtime_error("invalid Controller historical evidence directory");
  return true;
}

bool
isHistoricalReplyFilename(std::string_view name)
{
  const auto first = name.find('.');
  if (first == std::string_view::npos || first == 0)
    return false;
  const auto second = name.find('.', first + 1);
  if (second == std::string_view::npos || second == first + 1)
    return false;
  const auto isNumber = [] (std::string_view value) {
    return std::all_of(value.begin(), value.end(), [] (char c) {
      return c >= '0' && c <= '9';
    });
  };
  return isNumber(name.substr(0, first)) &&
         isNumber(name.substr(first + 1, second - first - 1)) &&
         isDigest("sha256:" + std::string(name.substr(second + 1)));
}

std::filesystem::path
historicalPublicationReceiptPath(const std::filesystem::path& state,
                                 const ndn::Name& publicationFullName)
{
  if (publicationFullName.empty() || !publicationFullName[-1].isImplicitSha256Digest())
    throw std::invalid_argument("historical publication receipt requires a FullName");
  return std::filesystem::path(state.string() + ".historical-publications") /
         historicalReplyFilename("", publicationFullName);
}

} // namespace

void
ControllerGenerationStore::recordGrant(const std::string& identity, uint64_t generation,
                                       uint64_t epoch, const std::string& signedContent)
{
  requireWriter();
  if (identity.empty() || generation == 0 || signedContent.empty() ||
      signedContent.size() > MAX_GRANT_RECORD_BYTES)
    throw std::invalid_argument("invalid Controller grant record");
  const auto directory = grantRecordDirectory();
  std::error_code error;
  std::filesystem::create_directory(directory, error);
  if (error)
    throw std::runtime_error("cannot create Controller grant record directory");
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  writePrivateGrantFile(directory / grantRecordName(identity, generation, epoch),
                        signedContent, m_fence);
}

void
ControllerGenerationStore::recordHistoricalReply(const ControllerVersion& version,
                                                  const ndn::Name& requestName,
                                                  const std::string& signedWire)
{
  requireWriter();
  const auto prefix = historicalReplyPrefix(version);
  const auto directory = historicalReplyDirectory(m_statePath);
  const auto target = directory / historicalReplyFilename(prefix, requestName);
  if (signedWire.empty() || signedWire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("invalid Controller historical reply size");
  const ndn::Data packet{ndn::Block(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(signedWire.data()), signedWire.size()))};
  if (packet.getName() != requestName)
    throw std::invalid_argument("Controller historical reply does not match request name");
  if (!isPrivateHistoricalDirectory(directory)) {
    if (::mkdir(directory.c_str(), 0700) != 0)
      throw std::runtime_error("cannot create Controller historical reply directory");
    syncStateDirectory(directory);
  }
  if (const auto previous = readPrivateGrantFile(target, ndn::MAX_NDN_PACKET_SIZE)) {
    if (*previous != signedWire)
      throw std::runtime_error("immutable Controller historical reply conflict");
    return;
  }
  size_t current = 0;
  size_t observed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (++observed > MAX_HISTORICAL_REPLIES * 2)
      throw std::runtime_error("Controller historical reply directory bound exceeded");
    const auto name = entry.path().filename().string();
    if (!isHistoricalReplyFilename(name) || !std::filesystem::is_regular_file(entry.symlink_status()))
      continue;
    if (name.compare(0, prefix.size(), prefix) == 0)
      ++current;
    else
      std::filesystem::remove(entry.path());
  }
  if (current >= MAX_HISTORICAL_REPLIES)
    throw std::runtime_error("Controller historical reply capacity exhausted");
  try {
    writePrivateGrantFile(target, signedWire, m_fence);
  }
  catch (const std::exception&) {
    throw CommitUncertain("cannot confirm Controller historical reply durability");
  }
}

std::optional<std::string>
ControllerGenerationStore::loadHistoricalReply(const ControllerVersion& version,
                                                const ndn::Name& requestName) const
{
  requireWriter();
  const auto directory = historicalReplyDirectory(m_statePath);
  if (!isPrivateHistoricalDirectory(directory))
    return std::nullopt;
  return readPrivateGrantFile(
    directory / historicalReplyFilename(historicalReplyPrefix(version), requestName),
    ndn::MAX_NDN_PACKET_SIZE);
}

void
ControllerGenerationStore::recordHistoricalPublicationReceipt(
    const ndn::Name& publicationFullName, const std::string& signedWire)
{
  requireWriter();
  if (signedWire.empty() || signedWire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("invalid historical publication receipt size");
  const auto target = historicalPublicationReceiptPath(m_statePath, publicationFullName);
  const auto directory = target.parent_path();
  if (!isPrivateHistoricalDirectory(directory)) {
    if (::mkdir(directory.c_str(), 0700) != 0)
      throw std::runtime_error("cannot create Controller historical publication receipt directory");
    syncStateDirectory(directory);
  }
  if (const auto previous = readPrivateGrantFile(target, ndn::MAX_NDN_PACKET_SIZE)) {
    if (*previous != signedWire)
      throw std::runtime_error("immutable Controller historical publication receipt conflict");
    return;
  }
  size_t count = 0;
  for (const auto& ignored : std::filesystem::directory_iterator(directory)) {
    (void)ignored;
    if (++count >= MAX_HISTORICAL_PUBLICATION_RECEIPTS)
      throw std::runtime_error("Controller historical publication receipt capacity exhausted");
  }
  try {
    writePrivateGrantFile(target, signedWire, m_fence);
  }
  catch (const std::exception&) {
    throw CommitUncertain("cannot confirm historical publication receipt durability");
  }
}

// The derived path requires the name to end in the publication's implicit digest, so the marker goes
// in front of it: the CK receipt's own name and path stay byte-identical.
static ndn::Name
grantReceiptKey(const ndn::Name& publicationFullName)
{
  ndn::Name keyed(publicationFullName.getPrefix(-1));
  keyed.append("GRANT-RECEIPT");
  keyed.append(publicationFullName[-1]);
  return keyed;
}

void
ControllerGenerationStore::recordHistoricalGrantReceipt(
    const ndn::Name& publicationFullName, const std::string& signedWire)
{
  // Same directory and durability rules as the CK receipt, distinct derived name.
  recordHistoricalPublicationReceipt(grantReceiptKey(publicationFullName), signedWire);
}

std::optional<std::string>
ControllerGenerationStore::loadHistoricalGrantReceipt(
    const ndn::Name& publicationFullName) const
{
  return loadHistoricalPublicationReceipt(grantReceiptKey(publicationFullName));
}

std::optional<std::string>
ControllerGenerationStore::loadHistoricalPublicationReceipt(
    const ndn::Name& publicationFullName) const
{
  requireWriter();
  const auto target = historicalPublicationReceiptPath(m_statePath, publicationFullName);
  if (!isPrivateHistoricalDirectory(target.parent_path()))
    return std::nullopt;
  return readPrivateGrantFile(target, ndn::MAX_NDN_PACKET_SIZE);
}

void
ControllerGenerationStore::visitHistoricalPublicationReceipts(
    const std::function<void(const std::string&, const ControllerVersion&)>& visitor) const
{
  requireWriter();
  const auto directory = std::filesystem::path(m_statePath.string() + ".historical-publications");
  if (!isPrivateHistoricalDirectory(directory))
    return;
  const ndn::Name owner(m_owner);
  size_t count = 0;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (++count > MAX_HISTORICAL_PUBLICATION_RECEIPTS)
      throw std::runtime_error("Controller historical publication receipt scan exceeds its limit");
    const auto wire = readPrivateGrantFile(item.path(), ndn::MAX_NDN_PACKET_SIZE);
    if (!wire)
      continue;
    std::optional<HistoricalPublicationReceipt> receipt;
    std::optional<HistoricalGrantReceipt> grantReceipt;
    try {
      const ndn::Block block(ndn::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(wire->data()), wire->size()));
      if (block.size() != wire->size())
        continue;
      const ndn::Data stored(block);
      // A publication may be receipted under either protection scheme, and both are admission
      // evidence for the generation they name, so the handover witness must see both.
      receipt = decodeHistoricalPublicationReceipt(stored, owner);
      if (!receipt)
        grantReceipt = decodeHistoricalGrantReceipt(stored, owner);
    }
    catch (const std::exception&) {
      continue;
    }
    if (receipt && item.path() == historicalPublicationReceiptPath(m_statePath,
                                                                   receipt->publicationFullName))
      visitor(*wire, receipt->version);
    else if (grantReceipt &&
             item.path() == historicalPublicationReceiptPath(
                               m_statePath, grantReceiptKey(grantReceipt->publicationFullName)))
      visitor(*wire, grantReceipt->version);
  }
}

std::optional<std::string>
ControllerGenerationStore::loadGrant(const std::string& identity, uint64_t generation,
                                    uint64_t epoch) const
{
  requireWriter();
  if (identity.empty() || generation == 0)
    return std::nullopt;
  return readPrivateGrantFile(grantRecordDirectory() / grantRecordName(identity, generation, epoch),
                              MAX_GRANT_RECORD_BYTES);
}

void
ControllerGenerationStore::visitGrantRecords(
    const std::function<void(const std::string&)>& visitor) const
{
  requireWriter();
  const auto directory = grantRecordDirectory();
  if (!std::filesystem::exists(directory))
    return;
  size_t count = 0;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (++count > MAX_GRANT_RECORDS)
      throw std::runtime_error("Controller grant record scan exceeds its limit");
    const auto name = item.path().filename().string();
    if (std::count(name.begin(), name.end(), '.') != 2)
      continue;
    if (const auto wire = readPrivateGrantFile(item.path(), MAX_GRANT_RECORD_BYTES)) {
      try {
        const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(wire->data()), wire->size())));
        const auto& value = data.getContent();
        const auto record = decodeHistoricalGrantRecord(
          {reinterpret_cast<const char*>(value.value()), value.value_size()});
        if (!record || name != grantRecordName(
              record->identity.toUri(ndn::name::UriFormat::CANONICAL),
              record->version.controllerGenerationTimestamp, record->version.controllerEpoch))
          continue;
      }
      catch (const std::exception&) {
        continue;
      }
      visitor(*wire);
    }
  }
}

ControllerGenerationStore::GrantWitnessRef
ControllerGenerationStore::storeGrantWitness(const std::string& signerDigest,
                                             const std::string& wire)
{
  requireWriter();
  if (m_secretKey.empty() || wire.empty() || wire.size() > MAX_GRANT_WITNESS_BYTES)
    throw std::invalid_argument("invalid Controller grant witness size");
  const auto digest = sha256Hex(wire);
  writePrivateGrantFile(grantWitnessPath(m_statePath, signerDigest).string() + "." + digest,
                        wire, m_fence);
  return {signerDigest, "sha256:" + digest};
}

std::optional<std::string>
ControllerGenerationStore::getGrantWitnessSigner() const
{
  const auto committed = loadState().grantWitness;
  return committed ? std::optional<std::string>(committed->signerDigest) : std::nullopt;
}

std::optional<std::string>
ControllerGenerationStore::loadGrantWitness(const std::string& signerDigest) const
{
  requireWriter();
  const auto committed = loadState().grantWitness;
  const auto path = grantWitnessPath(m_statePath, signerDigest);
  if (!committed) {
    const auto legacy = readPrivateGrantFile(path, MAX_GRANT_WITNESS_BYTES);
    if (!m_witnessImportDigest.empty()) {
      if (!legacy || "sha256:" + sha256Hex(*legacy) != m_witnessImportDigest)
        throw std::runtime_error("Controller grant witness import digest mismatch");
    }
    else if (legacy && !m_checkpointPath.empty()) {
      throw std::runtime_error("unbound Controller grant witness requires trusted migration");
    }
    return legacy;
  }
  if (committed->signerDigest != signerDigest)
    return std::nullopt;
  const auto wire = readPrivateGrantFile(path.string() + "." + committed->wireDigest.substr(7),
                                         MAX_GRANT_WITNESS_BYTES);
  if (!wire || "sha256:" + sha256Hex(*wire) != committed->wireDigest)
    throw std::runtime_error("Controller grant witness differs from committed state");
  return wire;
}

void
ControllerGenerationStore::recordAbeHistory(const AbeHistoryEntry& entry, size_t retention)
{
  requireWriter();
  if (retention == 0 || retention > 64)
    throw std::invalid_argument("invalid Controller ABE history retention");
  if (entry.generationTimestamp == 0 || entry.publicParameters.empty() ||
      entry.masterSecret.empty() || entry.publicParametersName.empty() ||
      !isDigest(entry.publicParametersWireDigest) ||
      entry.publicParametersPacket.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("invalid archived Controller ABE generation");
  AbeHistoryEntry archived = entry;
  archived.digest = abeHistoryDigest(archived);
  auto state = loadState();
  // A retried rotation reaches this point again with the same material; archiving it
  // twice would push a genuine older generation out of the bounded retention.
  const auto duplicate = std::find_if(state.abeHistory.begin(), state.abeHistory.end(),
    [&archived] (const auto& existing) {
      return existing.generationTimestamp == archived.generationTimestamp &&
             existing.epoch == archived.epoch && existing.digest == archived.digest;
    });
  if (duplicate != state.abeHistory.end())
    return;
  state.abeHistory.push_back(archived);
  if (state.abeHistory.size() > retention)
    state.abeHistory.erase(state.abeHistory.begin(),
                           state.abeHistory.end() - static_cast<std::ptrdiff_t>(retention));
  persist(state.version, state.revocations, state.abeMaterial, state.abeHistory, state.grantWitness);
}

std::optional<ControllerGenerationStore::AbeMaterial>
ControllerGenerationStore::loadAbeMaterial() const
{
  requireWriter();
  auto material = loadState().abeMaterial;
  if (material && material->owner != m_owner)
    throw std::runtime_error("Controller ABE state identity mismatch");
  return material;
}

void
ControllerGenerationStore::storeAbeMaterial(const AbeMaterial& material, bool commitNextEpoch)
{
  requireWriter();
  if (!isValidAbeMaterial(material) || material.owner != m_owner)
    throw std::invalid_argument("invalid Controller ABE material");
  auto current = loadState();
  if (!current.version.isValid() || (current.abeMaterial && current.abeMaterial->owner != m_owner))
    throw std::runtime_error("Controller ABE state identity mismatch");
  if (commitNextEpoch) {
    if (!current.abeMaterial || !current.abeMaterial->rotationRequired ||
        material.rotationRequired ||
        material.version.controllerGenerationTimestamp != current.version.controllerGenerationTimestamp ||
        current.version.controllerEpoch == std::numeric_limits<uint64_t>::max() ||
        material.version.controllerEpoch != current.version.controllerEpoch + 1)
      throw std::runtime_error("Controller ABE rotation does not match the pending epoch");
    current.version = material.version;
  }
  current.abeMaterial = material;
  persist(current.version, current.revocations, current.abeMaterial, current.abeHistory,
          current.grantWitness);
}

void
ControllerGenerationStore::persist(
    const ControllerVersion& version,
    const std::vector<RevocationTarget>& revocations,
    const std::optional<AbeMaterial>& abeMaterial,
    const std::vector<AbeHistoryEntry>& abeHistory,
    const std::optional<GrantWitnessRef>& grantWitness) const
{
  requireWriter();
  if (!version.isValid())
    throw std::invalid_argument("invalid Controller version");
  const auto temporary = m_checkpointPath.empty() ?
    m_statePath.string() + ".tmp." + std::to_string(m_fence) :
    uniqueTemporaryPath(m_statePath, m_fence);
  const int fd = ::open(temporary.c_str(),
                        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create private Controller generation state");
  std::array<unsigned char, STATE_MAC_BYTES> mac{};
  try {
    const char* magic = abeMaterial
      ? (abeHistory.empty() ? ABE_STATE_MAGIC : ABE_HISTORY_MAGIC)
      : (abeHistory.empty() ? STATE_MAGIC : HISTORY_ONLY_MAGIC);
    const bool versionedMaterial = abeMaterial.has_value() && abeMaterial->version.isValid();
    const bool hasPublishedPackets = (abeMaterial && abeMaterial->publicParametersVersion != 0) ||
      std::any_of(abeHistory.begin(), abeHistory.end(),
                  [] (const auto& entry) { return !entry.publicParametersPacket.empty(); });
    if (hasPublishedPackets && abeMaterial && !versionedMaterial)
      throw std::runtime_error("published Controller ABE material requires a valid version");
    if (grantWitness) {
      if (m_secretKey.empty() ||
          !isDigest(grantWitness->signerDigest) || !isDigest(grantWitness->wireDigest))
        throw std::runtime_error("invalid Controller grant witness commitment");
      writeAll(fd, WITNESS_STATE_MAGIC, STATE_MAGIC_SIZE);
      writeAll(fd, WITNESS_STATE_MAGIC, STATE_MAGIC_SIZE);
      writeString(fd, grantWitness->signerDigest);
      writeString(fd, grantWitness->wireDigest);
    }
    else if (!m_secretKey.empty()) {
      writeAll(fd, AUTHENTICATED_STATE_MAGIC, STATE_MAGIC_SIZE);
    }
    writeAll(fd, hasPublishedPackets ? ABE_PUBLIC_PACKET_MAGIC :
                (versionedMaterial ? ABE_HISTORY_VERSIONED_MAGIC : magic), STATE_MAGIC_SIZE);
    if (hasPublishedPackets)
      writeU64(fd, abeMaterial.has_value() ? 1 : 0);
    writeU64(fd, version.controllerGenerationTimestamp);
    writeU64(fd, version.controllerEpoch);
    writeU64(fd, revocations.size());
    for (const auto& target : revocations) {
      const auto wire = target.wireEncode();
      writeU64(fd, wire.size());
      writeAll(fd, reinterpret_cast<const char*>(wire.data()), wire.size());
    }
    if (abeMaterial) {
      if (!isValidAbeMaterial(*abeMaterial) || abeMaterial->owner != m_owner)
        throw std::runtime_error("invalid Controller ABE material");
      writeU64(fd, abeMaterial->rotationRequired ? 1 : 0);
      writeString(fd, abeMaterial->owner);
      writeString(fd, abeMaterial->signingKeyDigest);
      writeString(fd, abeMaterial->policyDigest);
      writeString(fd, abeMaterial->publicParameters);
      writeString(fd, m_secretKey.empty()
        ? abeMaterial->masterSecret
        : sealField(m_secretKey, abeMaterial->masterSecret, materialSealContext()));
      if (versionedMaterial) {
        writeU64(fd, abeMaterial->version.controllerGenerationTimestamp);
        writeU64(fd, abeMaterial->version.controllerEpoch);
      }
      if (hasPublishedPackets) {
        writeU64(fd, abeMaterial->publicParametersVersion);
        writeString(fd, abeMaterial->publicParametersPacket);
      }
      writeString(fd, abeMaterialDigest(*abeMaterial));
    }
    // A CS8 record always carries the history block, so the reader has one layout to follow: the versioned material
    // and the (possibly empty) archive are read together.
    if (!abeHistory.empty() || versionedMaterial) {
      writeU64(fd, abeHistory.size());
      for (const auto& entry : abeHistory) {
        if (entry.generationTimestamp == 0 || entry.digest != abeHistoryDigest(entry))
          throw std::runtime_error("invalid archived Controller ABE generation");
        writeU64(fd, entry.generationTimestamp);
        writeU64(fd, entry.epoch);
        writeString(fd, entry.publicParameters);
        writeString(fd, m_secretKey.empty()
          ? entry.masterSecret
          : sealField(m_secretKey, entry.masterSecret,
                      historySealContext(entry.generationTimestamp, entry.epoch)));
        writeString(fd, entry.publicParametersName);
        writeString(fd, entry.publicParametersWireDigest);
        if (hasPublishedPackets)
          writeString(fd, entry.publicParametersPacket);
        writeString(fd, entry.digest);
        writeString(fd, entry.reason);
      }
    }
    if (!m_secretKey.empty()) {
      const auto end = ::lseek(fd, 0, SEEK_CUR);
      if (end < static_cast<off_t>(STATE_MAGIC_SIZE) ||
          end > static_cast<off_t>(MAX_AUTHENTICATED_STATE_BYTES - STATE_MAC_BYTES))
        throw std::runtime_error("invalid Controller generation state length");
      const auto bytes = readStateBytes(fd, static_cast<size_t>(end) - STATE_MAGIC_SIZE,
                                        STATE_MAGIC_SIZE);
      mac = stateMac(m_secretKey, m_secretKeyId, m_owner, bytes);
      writeAll(fd, reinterpret_cast<const char*>(mac.data()), mac.size());
    }
    if (::fsync(fd) != 0)
      throw std::runtime_error("cannot flush Controller generation state");
  }
  catch (...) {
    ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  if (::close(fd) != 0) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot flush Controller generation state");
  }
  const auto staged = m_checkpointPath.empty() ? m_statePath :
    std::filesystem::path(m_statePath.string() + ".prepared");
  if (::rename(temporary.c_str(), staged.c_str()) != 0) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot atomically replace Controller generation state");
  }
  if (!m_checkpointPath.empty()) {
    syncStateDirectory(staged);
    auto checkpoint = std::string(CHECKPOINT_MAGIC);
    checkpoint.append(reinterpret_cast<const char*>(mac.data()), mac.size());
    try {
      requireWriter();
      writePrivateGrantFile(m_checkpointPath, checkpoint, m_fence);
    }
    catch (const std::exception&) {
      throw CommitUncertain("cannot commit independent Controller recovery checkpoint");
    }
    // From here the old active file cannot pass the independently durable checkpoint.
    if (::rename(staged.c_str(), m_statePath.c_str()) != 0)
      throw CommitUncertain("cannot activate the Controller recovery candidate");
  }
  syncStateDirectory(m_statePath);
}

ControllerVersion
ControllerGenerationStore::startGeneration(
    uint64_t nowMs, const std::vector<RevocationTarget>& revocations,
    const std::optional<GrantWitnessRef>& witness)
{
  requireWriter();
  if (nowMs == 0)
    throw std::invalid_argument("Controller generation timestamp must be nonzero");
  auto previous = loadState();
  if (previous.abeMaterial && previous.abeMaterial->owner != m_owner)
    throw std::runtime_error("Controller ABE state identity mismatch");
  uint64_t generation = nowMs;
  if (previous.version.isValid()) {
    if (previous.version.controllerGenerationTimestamp == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Controller generation timestamp overflow");
    generation = std::max(generation, previous.version.controllerGenerationTimestamp + 1);
    // Older writers stored the live material beside the publication version.
    // Carry that version into the new layout before advancing authority.
    if (previous.abeMaterial && !previous.abeMaterial->version.isValid())
      previous.abeMaterial->version = previous.version;
  }
  const ControllerVersion version{generation, 1};
  if (witness) {
    if (!isDigest(witness->wireDigest))
      throw std::invalid_argument("invalid Controller grant witness digest");
    const auto path = grantWitnessPath(m_statePath, witness->signerDigest).string() + "." +
                      witness->wireDigest.substr(7);
    const auto wire = readPrivateGrantFile(path, MAX_GRANT_WITNESS_BYTES);
    if (!wire || "sha256:" + sha256Hex(*wire) != witness->wireDigest)
      throw std::runtime_error("Controller grant witness candidate is missing or corrupted");
    previous.grantWitness = witness;
  }
  persist(version, revocations, previous.abeMaterial, previous.abeHistory, previous.grantWitness);
  return version;
}

ControllerVersion
ControllerGenerationStore::advanceEpoch(
    const std::vector<RevocationTarget>& revocations, bool requireAbeRotationOnRestart)
{
  requireWriter();
  auto previous = loadState();
  if (previous.abeMaterial && previous.abeMaterial->owner != m_owner)
    throw std::runtime_error("Controller ABE state identity mismatch");
  if (!previous.version.isValid())
    throw std::runtime_error("Controller generation has not started");
  if (previous.version.controllerEpoch == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Controller epoch overflow");
  const ControllerVersion version{previous.version.controllerGenerationTimestamp,
                                  previous.version.controllerEpoch + 1};
  // The version names the material that backs it: refuse to publish a version whose material does not carry it.
  // Material may be older than the version being published - a grant moves the policy, not the keys - but it may never
  // be newer, because that would name a version whose material does not exist yet.
  if (previous.abeMaterial && previous.abeMaterial->version.isValid() &&
      previous.abeMaterial->version.compare(version) > 0)
    throw std::runtime_error("Controller ABE material is newer than the version being published");
  if (previous.abeMaterial &&
      (requireAbeRotationOnRestart || !sameRevocations(previous.revocations, revocations)))
    previous.abeMaterial->rotationRequired = true;
  persist(version, revocations, previous.abeMaterial, previous.abeHistory, previous.grantWitness);
  return version;
}

ControllerVersion
ControllerGenerationStore::recordRevocations(
    const std::vector<RevocationTarget>& revocations, bool requireAbeRotationOnRestart)
{
  requireWriter();
  auto state = loadState();
  if (state.abeMaterial && state.abeMaterial->owner != m_owner)
    throw std::runtime_error("Controller ABE state identity mismatch");
  if (!state.version.isValid())
    throw std::runtime_error("Controller generation has not started");
  // The version is left alone on purpose: it names key material, and the rotation that would give the next version
  // its material has not run yet. The withdrawal is durable here, and the flag tells a later start (or this process)
  // to finish the rotation before answering with anything the old material cannot back.
  if (state.abeMaterial)
    state.abeMaterial->rotationRequired = requireAbeRotationOnRestart ||
      !sameRevocations(state.revocations, revocations);
  persist(state.version, revocations, state.abeMaterial, state.abeHistory, state.grantWitness);
  return state.version;
}

} // namespace ndn_service_framework} // namespace ndn_service_framework
