#include "ControllerGenerationStore.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <openssl/crypto.h>

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
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework {
namespace {

constexpr char STATE_MAGIC[] = "NDNSFCS2";
constexpr char ABE_STATE_MAGIC[] = "NDNSFCS3";
constexpr size_t STATE_MAGIC_SIZE = sizeof(STATE_MAGIC) - 1;
constexpr char LOCK_MAGIC[] = "NDNSF-FLOCK-1";
constexpr size_t MAX_LEASE_BYTES = 16384;
constexpr size_t MAX_ABE_FIELD_BYTES = 1U << 20;

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
readStringInto(std::istream& stream, std::string& value, size_t limit)
{
  const auto length = readU64(stream);
  if (length == 0 || length > limit)
    throw std::runtime_error("invalid Controller ABE state field length");
  value.assign(static_cast<size_t>(length), '\0');
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
isValidAbeMaterial(const ControllerGenerationStore::AbeMaterial& material)
{
  return !material.owner.empty() && material.owner.size() <= MAX_LEASE_BYTES - 64 &&
    material.owner.find_first_of("\r\n\t ") == std::string::npos &&
    isDigest(material.signingKeyDigest) && isDigest(material.policyDigest) &&
    !material.publicParameters.empty() && material.publicParameters.size() <= MAX_ABE_FIELD_BYTES &&
    !material.masterSecret.empty() && material.masterSecret.size() <= MAX_ABE_FIELD_BYTES;
}

std::string
abeMaterialDigest(const ControllerGenerationStore::AbeMaterial& material)
{
  ndn::util::Sha256 hash;
  hash << std::string("NDNSF-CONTROLLER-ABE-STATE-V1");
  for (const auto* field : {&material.owner, &material.signingKeyDigest,
                            &material.policyDigest, &material.publicParameters,
                            &material.masterSecret}) {
    hash << (std::to_string(field->size()) + ":");
    hash << *field;
  }
  hash << std::string(material.rotationRequired ? "1" : "0");
  return "sha256:" + hash.toString();
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
    masterSecret = other.masterSecret;
    rotationRequired = other.rotationRequired;
  }
  return *this;
}

ControllerGenerationStore::AbeMaterial::~AbeMaterial()
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
      loadState();
    }
    catch (...) {
      ::close(fd);
      return false;
    }
  }
  m_lockFd = fd;
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
  m_lockFd = -1;
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
  struct stat info{};
  if (::lstat(m_statePath.c_str(), &info) != 0 && errno == ENOENT)
    return std::nullopt;
  return loadState().version;
}

ControllerGenerationStore::PersistedState
ControllerGenerationStore::loadState() const
{
  const int fd = ::open(m_statePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT)
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

  char magic[STATE_MAGIC_SIZE] = {};
  state.read(magic, STATE_MAGIC_SIZE);
  if (state.gcount() != static_cast<std::streamsize>(STATE_MAGIC_SIZE))
    throw std::runtime_error("truncated Controller generation state");

  PersistedState result;
  const bool hasAbeMaterial = std::equal(std::begin(magic), std::end(magic), ABE_STATE_MAGIC);
  if (hasAbeMaterial && (info.st_mode & 0077) != 0)
    throw std::runtime_error("Controller ABE state must be private");
  if (hasAbeMaterial || std::equal(std::begin(magic), std::end(magic), STATE_MAGIC)) {
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
      std::string checksum;
      readStringInto(state, checksum, 71);
      if (!isValidAbeMaterial(material) || checksum != abeMaterialDigest(material) ||
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
  return result;
}

std::vector<RevocationTarget>
ControllerGenerationStore::loadRevocations() const
{
  return loadState().revocations;
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
ControllerGenerationStore::storeAbeMaterial(const AbeMaterial& material)
{
  requireWriter();
  if (!isValidAbeMaterial(material) || material.owner != m_owner)
    throw std::invalid_argument("invalid Controller ABE material");
  auto current = loadState();
  if (!current.version.isValid() || (current.abeMaterial && current.abeMaterial->owner != m_owner))
    throw std::runtime_error("Controller ABE state identity mismatch");
  current.abeMaterial = material;
  persist(current.version, current.revocations, current.abeMaterial);
}

void
ControllerGenerationStore::persist(
    const ControllerVersion& version,
    const std::vector<RevocationTarget>& revocations,
    const std::optional<AbeMaterial>& abeMaterial) const
{
  requireWriter();
  if (!version.isValid())
    throw std::invalid_argument("invalid Controller version");
  const auto temporary = m_statePath.string() + ".tmp." + std::to_string(m_fence);
  const int fd = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create private Controller generation state");
  try {
    writeAll(fd, abeMaterial ? ABE_STATE_MAGIC : STATE_MAGIC, STATE_MAGIC_SIZE);
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
      writeString(fd, abeMaterial->masterSecret);
      writeString(fd, abeMaterialDigest(*abeMaterial));
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
  std::error_code ec;
  std::filesystem::rename(temporary, m_statePath, ec);
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot atomically replace Controller generation state");
  }
  const auto parent = m_statePath.parent_path().empty() ? std::filesystem::path(".") :
                                                        m_statePath.parent_path();
  const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirFd < 0)
    throw CommitUncertain("cannot open Controller state directory for flush");
  const int flushResult = ::fsync(dirFd);
  ::close(dirFd);
  if (flushResult != 0)
    throw CommitUncertain("cannot flush Controller state directory");
}

ControllerVersion
ControllerGenerationStore::startGeneration(
    uint64_t nowMs, const std::vector<RevocationTarget>& revocations)
{
  requireWriter();
  if (nowMs == 0)
    throw std::invalid_argument("Controller generation timestamp must be nonzero");
  const auto previous = loadState();
  if (previous.abeMaterial && previous.abeMaterial->owner != m_owner)
    throw std::runtime_error("Controller ABE state identity mismatch");
  uint64_t generation = nowMs;
  if (previous.version.isValid()) {
    if (previous.version.controllerGenerationTimestamp == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Controller generation timestamp overflow");
    generation = std::max(generation, previous.version.controllerGenerationTimestamp + 1);
  }
  const ControllerVersion version{generation, 1};
  persist(version, revocations, previous.abeMaterial);
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
  if (previous.abeMaterial &&
      (requireAbeRotationOnRestart || !sameRevocations(previous.revocations, revocations)))
    previous.abeMaterial->rotationRequired = true;
  persist(version, revocations, previous.abeMaterial);
  return version;
}

} // namespace ndn_service_framework
