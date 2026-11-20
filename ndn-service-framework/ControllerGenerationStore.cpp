#include "ControllerGenerationStore.hpp"

#include "SealedField.hpp"

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

std::string
abeHistoryDigest(const ControllerGenerationStore::AbeHistoryEntry& entry)
{
  ndn::util::Sha256 hash;
  hash << std::string("NDNSF-CONTROLLER-ABE-HISTORY-V1");
  hash << std::to_string(entry.generationTimestamp);
  hash << std::to_string(entry.epoch);
  for (const auto* field : {&entry.publicParametersName, &entry.publicParametersWireDigest,
                            &entry.publicParameters, &entry.masterSecret}) {
    hash << (std::to_string(field->size()) + ":");
    hash << *field;
  }
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

ControllerGenerationStore::AbeHistoryEntry&
ControllerGenerationStore::AbeHistoryEntry::operator=(const AbeHistoryEntry& other)
{
  if (this != &other) {
    if (!masterSecret.empty())
      OPENSSL_cleanse(&masterSecret[0], masterSecret.size());
    generationTimestamp = other.generationTimestamp;
    epoch = other.epoch;
    publicParameters = other.publicParameters;
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
  const bool hasHistory = std::equal(std::begin(magic), std::end(magic), ABE_HISTORY_MAGIC);
  const bool hasLegacyHistory = std::equal(std::begin(magic), std::end(magic),
                                           LEGACY_HISTORY_MAGIC) ||
    std::equal(std::begin(magic), std::end(magic), LEGACY_HISTORY_ONLY_MAGIC);
  const bool legacyHistoryOnly = std::equal(std::begin(magic), std::end(magic),
                                            LEGACY_HISTORY_ONLY_MAGIC);
  // A history-only record carries no live material, so it must not be parsed as if it did.
  const bool hasAbeMaterial = hasHistory || (hasLegacyHistory && !legacyHistoryOnly) ||
    std::equal(std::begin(magic), std::end(magic), ABE_STATE_MAGIC);
  const bool hasHistoryOnly = std::equal(std::begin(magic), std::end(magic), HISTORY_ONLY_MAGIC);
  if (hasAbeMaterial && (info.st_mode & 0077) != 0)
    throw std::runtime_error("Controller ABE state must be private");
  if (hasAbeMaterial || hasHistoryOnly || hasLegacyHistory ||
      std::equal(std::begin(magic), std::end(magic), STATE_MAGIC)) {
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
      if (isSealedField(material.masterSecret)) {
        std::string opened;
        if (m_secretKey.empty() || !openField(m_secretKey, material.masterSecret,
                                              materialSealContext(), opened))
          throw std::runtime_error("Controller ABE state is sealed and cannot be opened");
        material.masterSecret = std::move(opened);
      }
      std::string checksum;
      readStringInto(state, checksum, 71);
      if (!isValidAbeMaterial(material) || checksum != abeMaterialDigest(material))
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
        readStringInto(state, entry.publicParameters, MAX_ABE_FIELD_BYTES);
        readStringInto(state, entry.masterSecret, MAX_ABE_FIELD_BYTES);
        if (isSealedField(entry.masterSecret)) {
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
          if (entry.generationTimestamp == 0 || entry.publicParameters.empty())
            throw std::runtime_error("invalid legacy archived Controller ABE generation");
          entry.publicParametersWireDigest = "sha256:" + sha256Hex(entry.publicParameters);
          ndn::Name original(m_owner);
          original.append("PUBPARAMS").append("KP-ABE")
            .appendVersion(entry.generationTimestamp * 1000ULL + entry.epoch);
          entry.publicParametersName = original.toUri(ndn::name::UriFormat::CANONICAL);
          entry.digest = abeHistoryDigest(entry);
        }
        else {
          readStringInto(state, entry.publicParametersName, MAX_LEASE_BYTES - 64);
          readStringInto(state, entry.publicParametersWireDigest, 71);
          readStringInto(state, entry.digest, 71);
          readStringInto(state, entry.reason, MAX_LEASE_BYTES - 64);
          if (entry.generationTimestamp == 0 || entry.digest != abeHistoryDigest(entry))
            throw std::runtime_error("invalid archived Controller ABE generation");
        }
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
ControllerGenerationStore::setSecretSeal(std::vector<uint8_t> key, std::string keyId)
{
  if (!key.empty() && key.size() != 32)
    throw std::invalid_argument("the Controller secret key must be 32 bytes");
  m_secretKey = std::move(key);
  m_secretKeyId = std::move(keyId);
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

namespace {

/** One file per identity and version, named so a lookup needs no index. */
std::string
grantRecordName(const std::string& identity, uint64_t generation, uint64_t epoch)
{
  ndn::util::Sha256 digest;
  digest << identity;
  return digest.toString() + "." + std::to_string(generation) + "." + std::to_string(epoch);
}

} // namespace

void
ControllerGenerationStore::recordGrant(const std::string& identity, uint64_t generation,
                                       uint64_t epoch, const std::string& signedContent)
{
  requireWriter();
  if (identity.empty() || generation == 0 || signedContent.empty())
    throw std::invalid_argument("invalid Controller grant record");
  const auto directory = grantRecordDirectory();
  std::error_code error;
  std::filesystem::create_directory(directory, error);
  if (error)
    throw std::runtime_error("cannot create Controller grant record directory");
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                              std::filesystem::perm_options::add, error);
  const auto path = directory / grantRecordName(identity, generation, epoch);
  const auto temporary = path.string() + ".tmp." + std::to_string(m_fence);
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create private Controller grant record");
  try {
    writeAll(fd, signedContent.data(), signedContent.size());
    if (::fsync(fd) != 0)
      throw std::runtime_error("cannot flush Controller grant record");
    if (::close(fd) != 0)
      throw std::runtime_error("cannot close Controller grant record");
    if (::rename(temporary.c_str(), path.c_str()) != 0)
      throw std::runtime_error("cannot publish Controller grant record");
  }
  catch (...) {
    ::close(fd);
    ::unlink(temporary.c_str());
    throw;
  }
}

std::optional<std::string>
ControllerGenerationStore::loadGrant(const std::string& identity, uint64_t generation,
                                     uint64_t epoch) const
{
  requireWriter();
  if (identity.empty() || generation == 0)
    return std::nullopt;
  const auto path = grantRecordDirectory() / grantRecordName(identity, generation, epoch);
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (buffer.str().empty())
    return std::nullopt;
  return buffer.str();
}

void
ControllerGenerationStore::recordAbeHistory(const AbeHistoryEntry& entry, size_t retention)
{
  requireWriter();
  if (retention == 0 || retention > 64)
    throw std::invalid_argument("invalid Controller ABE history retention");
  if (entry.generationTimestamp == 0 || entry.publicParameters.empty() ||
      entry.masterSecret.empty() || entry.publicParametersName.empty() ||
      !isDigest(entry.publicParametersWireDigest))
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
  persist(state.version, state.revocations, state.abeMaterial, state.abeHistory);
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
  persist(current.version, current.revocations, current.abeMaterial, current.abeHistory);
}

void
ControllerGenerationStore::persist(
    const ControllerVersion& version,
    const std::vector<RevocationTarget>& revocations,
    const std::optional<AbeMaterial>& abeMaterial,
    const std::vector<AbeHistoryEntry>& abeHistory) const
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
    const char* magic = abeMaterial
      ? (abeHistory.empty() ? ABE_STATE_MAGIC : ABE_HISTORY_MAGIC)
      : (abeHistory.empty() ? STATE_MAGIC : HISTORY_ONLY_MAGIC);
    writeAll(fd, magic, STATE_MAGIC_SIZE);
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
      writeString(fd, abeMaterialDigest(*abeMaterial));
    }
    if (!abeHistory.empty()) {
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
        writeString(fd, entry.digest);
        writeString(fd, entry.reason);
      }
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
  persist(version, revocations, previous.abeMaterial, previous.abeHistory);
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
  persist(version, revocations, previous.abeMaterial, previous.abeHistory);
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
  persist(state.version, revocations, state.abeMaterial, state.abeHistory);
  return state.version;
}

} // namespace ndn_service_framework} // namespace ndn_service_framework
