#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-private-checkpoint-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-symmetric-crypto.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace ndnsf::di {
namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t MAX_OBJECT_BYTES = 1ULL << 30;
constexpr std::uint64_t MAX_INDEX_BYTES = 16ULL << 20;
constexpr char HANDLE_MAGIC[] = "NDNSF-DI-STORED-MODEL-HANDLE-v1\n";

void
require(bool condition)
{
  if (!condition)
    throw std::runtime_error("DI_STORED_MODEL_REJECTED: invalid capability or object");
}

struct Cleanse
{
  Bytes& bytes;
  ~Cleanse() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

struct FileDescriptor
{
  int value;
  ~FileDescriptor() { ::close(value); }
};

std::string
digest(const std::uint8_t* bytes, std::size_t size)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(bytes, size, hash);
  std::string result = "sha256:";
  for (const auto value : hash) {
    result += "0123456789abcdef"[value >> 4];
    result += "0123456789abcdef"[value & 15];
  }
  return result;
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void
requireObjectName(const ndn::Name& producer, const ndn::Name& name)
{
  require(!producer.empty() && producer.size() < name.size() && producer.isPrefixOf(name) &&
          name.wireEncode().size() <= 4096 && !name[-1].isSegment() &&
          !name[-1].isImplicitSha256Digest());
}

std::string
objectContext(const ndn::Name& producer, const ndn::Name& name)
{
  requireObjectName(producer, name);
  const auto& wire = name.wireEncode();
  std::string result = "NDNSF-DI/stored-model/object/v1:";
  result.append(reinterpret_cast<const char*>(wire.data()), wire.size());
  return result;
}

void
writeAll(int fd, const std::uint8_t* bytes, std::size_t size)
{
  while (size > 0) {
    const auto count = ::write(fd, bytes, size);
    if (count < 0 && errno == EINTR)
      continue;
    require(count > 0);
    bytes += count;
    size -= count;
  }
}

Bytes
readPrivateFile(const std::filesystem::path& source, std::uint64_t maximum,
                const std::function<void()>& current = {})
{
  if (current) current();
  const int raw = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  require(raw >= 0);
  const FileDescriptor fd{raw};
  struct stat status{};
  require(::fstat(raw, &status) == 0 && S_ISREG(status.st_mode) &&
          status.st_uid == ::geteuid() && status.st_nlink == 1 &&
          (status.st_mode & 07777) == 0600 && status.st_size > 0 &&
          static_cast<std::uint64_t>(status.st_size) <= maximum);
  Bytes bytes(static_cast<std::size_t>(status.st_size));
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      if (current) current();
      const auto count = ::read(raw, bytes.data() + offset,
        std::min<std::size_t>(1U << 20, bytes.size() - offset));
      if (count < 0 && errno == EINTR) continue;
      require(count > 0);
      offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra;
    require(::read(raw, &extra, 1) == 0);
    if (current) current();
  }
  catch (...) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
    throw;
  }
  return bytes;
}

} // namespace

NativeModelCommitPipeline::NativeModelCommitPipeline(CurrentCheck requireCurrent)
  : m_requireCurrent(std::move(requireCurrent))
  , m_owner(std::this_thread::get_id())
  , m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
  if (!m_requireCurrent) throw std::invalid_argument("MODEL_REPO_PIPELINE_CURRENT_MISSING");
  m_requireCurrent();
}

NativeModelCommitPipeline::~NativeModelCommitPipeline() noexcept
{
  cancelAndDrain();
}

void
NativeModelCommitPipeline::requireOwner() const
{
  if (std::this_thread::get_id() != m_owner)
    throw std::logic_error("MODEL_REPO_PIPELINE_WRONG_OWNER_THREAD");
}

void
NativeModelCommitPipeline::cancelAndDrain() noexcept
{
  m_cancelled->store(true, std::memory_order_release);
  m_closed = true;
  while (!m_pending.empty()) {
    auto pending = std::move(m_pending.front());
    m_pending.pop_front();
    try {
      pending.completion.get();
      if (pending.observe) pending.observe();
    }
    catch (...) {
      // A failed scope cannot roll back a later independently durable commit.
      // Observation is best-effort here; the original failure stays primary.
    }
  }
}

void
NativeModelCommitPipeline::fail(std::exception_ptr error)
{
  cancelAndDrain();
  std::rethrow_exception(error);
}

void
NativeModelCommitPipeline::completeFirst()
{
  auto pending = std::move(m_pending.front());
  m_pending.pop_front();
  try {
    pending.completion.get();
    if (pending.observe) pending.observe();
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
NativeModelCommitPipeline::submit(std::uint64_t wireBytes, Commit commit,
                                  std::function<void()> observe)
{
  requireOwner();
  try {
    if (m_closed || !commit || wireBytes == 0 || wireBytes > NATIVE_STORED_MODEL_UPLOAD_WIRE)
      throw std::invalid_argument("MODEL_REPO_PIPELINE_SUBMISSION_INVALID");
    m_requireCurrent();
    while (m_pending.size() >= MAX_ACTIVE_COMMITS) completeFirst();
    m_requireCurrent();
    const auto cancelled = m_cancelled;
    // Reserve the owner record before starting a task: allocation failure must
    // not leave an unowned asynchronous commit whose cancellation cannot drain.
    m_pending.emplace_back();
    auto& pending = m_pending.back();
    try {
      pending.observe = std::move(observe);
      pending.completion = std::async(std::launch::async,
        [commit = std::move(commit), cancelled] {
          const CurrentCheck current = [cancelled] {
            if (cancelled->load(std::memory_order_acquire))
              throw std::runtime_error("MODEL_REPO_PIPELINE_CANCELLED");
          };
          current();
          commit(current);
        });
    }
    catch (...) {
      m_pending.pop_back();
      throw;
    }
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
NativeModelCommitPipeline::drain()
{
  requireOwner();
  if (m_closed) throw std::logic_error("MODEL_REPO_PIPELINE_CLOSED");
  try {
    m_requireCurrent();
    while (!m_pending.empty()) completeFirst();
    m_requireCurrent();
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
NativeModelCommitPipeline::finish()
{
  drain();
  m_closed = true;
}

NativeStoredModelHandle::~NativeStoredModelHandle()
{
  OPENSSL_cleanse(masterKey.data(), masterKey.size());
}

NativeStoredModelHandle&
NativeStoredModelHandle::operator=(const NativeStoredModelHandle& other)
{
  if (this != &other) {
    NativeStoredModelHandle copy(other);
    *this = std::move(copy);
  }
  return *this;
}

NativeStoredModelHandle&
NativeStoredModelHandle::operator=(NativeStoredModelHandle&& other) noexcept
{
  if (this != &other) {
    OPENSSL_cleanse(masterKey.data(), masterKey.size());
    producer = std::move(other.producer);
    repository = std::move(other.repository);
    rootName = std::move(other.rootName);
    rootWireDigest = std::move(other.rootWireDigest);
    masterKey = std::move(other.masterKey);
  }
  return *this;
}

void
NativeStoredModelReference::validate() const
{
  requireObjectName(producer, rootName);
  require(!repository.empty() && repository.wireEncode().size() <= 4096 &&
          isDigest(rootWireDigest));
}

void
NativeStoredModelHandle::validate() const
{
  NativeStoredModelReference::validate();
  require(masterKey.size() == 32);
}

void
nativeExportStoredModelHandle(const NativeStoredModelHandle& handle,
                              const std::filesystem::path& destination)
{
  handle.validate();
  // Only nonsecret metadata passes through JSON. Key bytes are written directly.
  const auto metadata = nativeCanonicalJson(NativeJson{
    {"producer", handle.producer.toUri(ndn::name::UriFormat::CANONICAL)},
    {"repository", handle.repository.toUri(ndn::name::UriFormat::CANONICAL)},
    {"rootName", handle.rootName.toUri(ndn::name::UriFormat::CANONICAL)},
    {"rootWireDigest", handle.rootWireDigest}});
  const auto prefix = std::string(HANDLE_MAGIC) + metadata + '\n';
  require(prefix.size() + handle.masterKey.size() <= 32768);
  detail::writePrivateCheckpointFile(destination, {}, [&] (int fd) {
    writeAll(fd, reinterpret_cast<const std::uint8_t*>(prefix.data()), prefix.size());
    writeAll(fd, handle.masterKey.data(), handle.masterKey.size());
  });
}

NativeStoredModelHandle
nativeLoadStoredModelHandle(const std::filesystem::path& source)
{
  auto bytes = readPrivateFile(source, 32768);
  Cleanse guard{bytes};
  const std::string magic(HANDLE_MAGIC);
  require(bytes.size() > magic.size() &&
          std::equal(magic.begin(), magic.end(), bytes.begin()));
  const auto end = std::find(bytes.begin() + magic.size(), bytes.end(), '\n');
  require(end != bytes.end() && static_cast<std::size_t>(bytes.end() - end - 1) == 32);
  NativeStoredModelHandle handle;
  try {
    const auto metadata = nativeParseJson(std::string(bytes.begin() + magic.size(), end));
    require(metadata.is_object() && metadata.size() == 4);
    handle.producer = ndn::Name(metadata.at("producer").get<std::string>());
    handle.repository = ndn::Name(metadata.at("repository").get<std::string>());
    handle.rootName = ndn::Name(metadata.at("rootName").get<std::string>());
    handle.rootWireDigest = metadata.at("rootWireDigest").get<std::string>();
    handle.masterKey.assign(end + 1, bytes.end());
    handle.validate();
  }
  catch (...) {
    // Parser diagnostics must never echo a private capability file.
    require(false);
  }
  return handle;
}

NativeStoredModelRoot
nativeAdmitStoredModelRoot(const NativeStoredModelReference& handle, const ndn::Data& root)
{
  handle.validate();
  const auto& wire = root.wireEncode();
  require(wire.size() <= 8800 && root.getName() == handle.rootName &&
          digest(wire.data(), wire.size()) == handle.rootWireDigest &&
          root.getContentType() == ndn::tlv::ContentType_Blob && !root.getFinalBlock() &&
          root.getSignatureType() == ndn::tlv::DigestSha256 &&
          ndn::security::verifySignature(root, std::optional<ndn::security::Certificate>{}));
  NativeStoredModelRoot result;
  try {
    const auto value = nativeParseJson(std::string(
      reinterpret_cast<const char*>(root.getContent().value()), root.getContent().value_size()));
    require(value.is_object() && value.size() == 7 &&
            value.at("schema") == "ndnsf-di-stored-model-root-v1");
    result.modelDigest = value.at("modelDigest").get<std::string>();
    result.indexName = ndn::Name(value.at("indexName").get<std::string>());
    result.indexDigest = value.at("indexDigest").get<std::string>();
    for (const auto* field : {"indexBytes", "segmentBytes", "segmentCount"})
      require(value.at(field).is_number_unsigned());
    result.indexBytes = value.at("indexBytes").get<std::uint64_t>();
    const auto segmentBytes = value.at("segmentBytes").get<std::uint64_t>();
    const auto segmentCount = value.at("segmentCount").get<std::uint64_t>();
    requireObjectName(handle.producer, result.indexName);
    require(isDigest(result.modelDigest) && isDigest(result.indexDigest) &&
            result.indexName != handle.rootName && result.indexBytes > 0 &&
            result.indexBytes <= MAX_INDEX_BYTES && segmentBytes >= 512 &&
            segmentBytes <= 6144 && segmentCount ==
              (result.indexBytes + segmentBytes - 1) / segmentBytes);
    result.segmentBytes = static_cast<std::uint32_t>(segmentBytes);
    result.segmentCount = static_cast<std::uint32_t>(segmentCount);
  }
  catch (...) {
    require(false);
  }
  return result;
}

Bytes
nativeStoredModelObjectKey(const Bytes& masterKey, const ndn::Name& producer,
                           const ndn::Name& objectName)
{
  require(masterKey.size() == 32);
  return detail::deriveSymmetricKey(masterKey, objectContext(producer, objectName));
}

Bytes
nativeSealStoredModelObject(const Bytes& masterKey, const ndn::Name& producer,
                            const ndn::Name& objectName, const Bytes& plaintext,
                            NativeStoredModelObject& reference)
{
  require(plaintext.size() <= MAX_OBJECT_BYTES);
  auto key = nativeStoredModelObjectKey(masterKey, producer, objectName);
  Cleanse keyGuard{key};
  Bytes nonce(12);
  require(RAND_bytes(nonce.data(), nonce.size()) == 1);
  auto ciphertext = detail::cryptSymmetric(true, key, nonce, plaintext,
                                           objectContext(producer, objectName));
  ciphertext.insert(ciphertext.begin(), nonce.begin(), nonce.end());
  NativeStoredModelObject result;
  result.name = objectName;
  result.plaintextBytes = plaintext.size();
  result.ciphertextBytes = ciphertext.size();
  result.plaintextDigest = digest(plaintext.data(), plaintext.size());
  result.ciphertextDigest = digest(ciphertext.data(), ciphertext.size());
  reference = std::move(result);
  return ciphertext;
}

Bytes
nativeOpenStoredModelObject(const NativeStoredModelHandle& handle,
                            const NativeStoredModelObject& reference, const Bytes& wire)
{
  handle.validate();
  auto key = nativeStoredModelObjectKey(handle.masterKey, handle.producer, reference.name);
  Cleanse keyGuard{key};
  return nativeOpenStoredModelObjectWithKey(handle, reference, key, wire);
}

Bytes
nativeOpenStoredModelObjectWithKey(const NativeStoredModelReference& publication,
                                  const NativeStoredModelObject& reference,
                                  const Bytes& objectKey, const Bytes& wire)
{
  publication.validate();
  requireObjectName(publication.producer, reference.name);
  require(reference.plaintextBytes <= MAX_OBJECT_BYTES &&
          reference.ciphertextBytes == reference.plaintextBytes + 28 &&
          wire.size() == reference.ciphertextBytes &&
          isDigest(reference.plaintextDigest) && isDigest(reference.ciphertextDigest) &&
          digest(wire.data(), wire.size()) == reference.ciphertextDigest);
  auto plaintext = detail::cryptSymmetric(false, objectKey, Bytes(wire.begin(), wire.begin() + 12),
    Bytes(wire.begin() + 12, wire.end()), objectContext(publication.producer, reference.name));
  if (plaintext.size() != reference.plaintextBytes ||
      digest(plaintext.data(), plaintext.size()) != reference.plaintextDigest) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    require(false);
  }
  return plaintext;
}

namespace {

constexpr std::size_t MAX_GROUP_PACKETS = 1024;
constexpr std::size_t MAX_GROUP_NAMES = 128U << 10;
constexpr std::size_t MAX_GROUP_WIRE = 8U << 20;
constexpr std::uint64_t MAX_PUBLICATION_BYTES = NATIVE_STORED_MODEL_MAX_PUBLICATION_BYTES;

bool
validObjectId(const std::string& id)
{
  return !id.empty() && id.size() <= 128 && id != "." && id != ".." &&
    std::all_of(id.begin(), id.end(), [] (unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

std::uint32_t
segmentPayload(const ndn::Name& name)
{
  // Reserve room for segment/final-block components, TLV lengths and DigestSha256.
  require(name.wireEncode().size() + 128 + 512 <= ndn::MAX_NDN_PACKET_SIZE);
  return static_cast<std::uint32_t>(std::min<std::size_t>(6144,
    ndn::MAX_NDN_PACKET_SIZE - name.wireEncode().size() - 128));
}

void
requirePublicationPrefix(const ndn::Name& producer, const ndn::Name& prefix)
{
  requireObjectName(producer, prefix);
  require(prefix.toUri(ndn::name::UriFormat::CANONICAL).size() <=
          (ndn::MAX_NDN_PACKET_SIZE - 1024) / 2);
}

ndn::Data
decodeOriginal(const Bytes& wire)
{
  require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
  const ndn::Block block{ndn::span<const std::uint8_t>(wire.data(), wire.size())};
  require(block.size() == wire.size());
  return ndn::Data(block);
}

NativeJson
indexedObjectJson(const NativeStoredModelIndexedObject& entry)
{
  const auto& object = entry.object;
  return NativeJson{
    {"name", object.name.toUri(ndn::name::UriFormat::CANONICAL)},
    {"plaintextDigest", object.plaintextDigest}, {"ciphertextDigest", object.ciphertextDigest},
    {"plaintextBytes", object.plaintextBytes}, {"ciphertextBytes", object.ciphertextBytes},
    {"segmentBytes", entry.segmentBytes}};
}

NativeStoredModelIndexedObject
decodeIndexedObject(const NativeJson& entry)
{
  require(entry.is_object() && entry.size() == 6);
  for (const auto* field : {"plaintextBytes", "ciphertextBytes", "segmentBytes"})
    require(entry.at(field).is_number_unsigned());
  require(entry.at("segmentBytes").get<std::uint64_t>() <= 6144);
  NativeStoredModelIndexedObject object;
  object.object.name = ndn::Name(entry.at("name").get<std::string>());
  object.object.plaintextDigest = entry.at("plaintextDigest").get<std::string>();
  object.object.ciphertextDigest = entry.at("ciphertextDigest").get<std::string>();
  object.object.plaintextBytes = entry.at("plaintextBytes").get<std::uint64_t>();
  object.object.ciphertextBytes = entry.at("ciphertextBytes").get<std::uint64_t>();
  object.segmentBytes = entry.at("segmentBytes").get<std::uint32_t>();
  require(isDigest(object.object.plaintextDigest) && isDigest(object.object.ciphertextDigest) &&
    object.object.plaintextBytes <= MAX_OBJECT_BYTES &&
    object.object.ciphertextBytes == object.object.plaintextBytes + 28 &&
    object.segmentBytes >= 512 && object.segmentBytes <= segmentPayload(object.object.name));
  return object;
}

// Missing tags are the original grouping, including an ambiguously committed tail.
unsigned
uploadGroupingVersion(const NativeJson& value, const char* field = "uploadGroupingVersion")
{
  const auto found = value.find(field);
  if (found == value.end()) return 1;
  require(found->is_number_integer() && (*found == 1 || *found == 2));
  return found->get<unsigned>();
}

/** One private preparation owner; no worker, public key registry or model-sized history. */
class PreparationJournal
{
public:
  PreparationJournal(const std::filesystem::path& path, NativeJson context,
                     const NativeStoredModelPacketIo& io)
    : m_path(path)
    , m_stage(path.string() + ".object")
    , m_io(io)
  {
    require(m_io.requireCommitted && !path.empty() && !path.filename().empty() &&
            path.filename() != "." && path.filename() != "..");
    m_io.requireCurrent();
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") :
                                                    path.parent_path();
    m_parent.value = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat status{};
    require(m_parent.value >= 0 && ::fstat(m_parent.value, &status) == 0 &&
            status.st_uid == ::geteuid() && (status.st_mode & 0022) == 0);
    m_lock.value = ::open((path.string() + ".lock").c_str(),
      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    require(m_lock.value >= 0 && ::fstat(m_lock.value, &status) == 0 &&
      S_ISREG(status.st_mode) && status.st_uid == ::geteuid() && status.st_nlink == 1 &&
      (status.st_mode & 07777) == 0600 && ::flock(m_lock.value, LOCK_EX | LOCK_NB) == 0);
    m_handle.producer = ndn::Name(context.at("producer").get<std::string>());
    m_handle.repository = ndn::Name(context.at("repository").get<std::string>());
    const bool resolvePrefix = context.at("publicationPrefix").get<std::string>().empty();
    const auto bindPrefix = [&](const std::string& text) {
      m_prefix = ndn::Name(text);
      requirePublicationPrefix(m_handle.producer, m_prefix);
      context["publicationPrefix"] = m_prefix.toUri(ndn::name::UriFormat::CANONICAL);
      m_handle.rootName = ndn::Name(m_prefix).append("root");
    };
    if (!resolvePrefix) bindPrefix(context.at("publicationPrefix").get<std::string>());
    if (::lstat(path.c_str(), &status) == 0) {
      auto bytes = readPrivateFile(path, MAX_JOURNAL_BYTES, m_io.requireCurrent);
      Cleanse guard{bytes};
      try {
        const std::string magic = MAGIC;
        require(bytes.size() > magic.size() + 103 &&
          std::equal(magic.begin(), magic.end(), bytes.begin()));
        const auto bodySize = bytes.size() - 71;
        require(digest(bytes.data(), bodySize) ==
                std::string(bytes.begin() + bodySize, bytes.end()));
        const auto metadataEnd = bodySize - 32;
        const auto text = std::string(bytes.begin() + magic.size(), bytes.begin() + metadataEnd);
        m_metadata = nativeParseJson(text);
        if (resolvePrefix)
          bindPrefix(m_metadata.at("context").at("publicationPrefix").get<std::string>());
        auto savedContext = m_metadata.at("context");
        (void)uploadGroupingVersion(savedContext, "tailUploadGroupingVersion");
        savedContext.erase("tailUploadGroupingVersion");
        require(nativeCanonicalJson(m_metadata) == text && m_metadata.is_object() &&
          m_metadata.size() == 7 && m_metadata.at("schema") == "ndnsf-di-model-preparation-v1" &&
          savedContext == context);
        m_handle.masterKey.assign(bytes.begin() + metadataEnd, bytes.begin() + bodySize);
        validate();
      }
      catch (...) {
        // Neither parser text nor private key bytes may escape through diagnostics.
        require(false);
      }
    }
    else {
      require(errno == ENOENT && ::lstat(m_stage.c_str(), &status) != 0 && errno == ENOENT);
      if (resolvePrefix) {
        std::uint64_t version = 0;
        require(RAND_bytes(reinterpret_cast<unsigned char*>(&version), sizeof(version)) == 1);
        const auto prefix = ndn::Name(m_handle.producer).append("NDNSF").append("DI")
          .append("STORED-MODEL").appendVersion(version);
        bindPrefix(prefix.toUri(ndn::name::UriFormat::CANONICAL));
      }
      m_handle.masterKey.resize(32);
      require(RAND_bytes(m_handle.masterKey.data(), m_handle.masterKey.size()) == 1);
      context["tailUploadGroupingVersion"] = 2;
      m_metadata = NativeJson{{"schema", "ndnsf-di-model-preparation-v1"}, {"context", context},
        {"objects", NativeJson::object()}, {"current", nullptr}, {"rootDigest", ""},
        {"tail", NativeJson::array()}, {"ready", false}};
      save();
    }
    if (m_metadata.at("current").is_null()) removeCompletedStage();
    for (const auto& object : m_metadata.at("objects").items())
      confirm(object.value().at("groups"));
    if (ready()) confirm(m_metadata.at("tail"));
    m_io.requireCurrent();
  }

  PreparationJournal(const PreparationJournal&) = delete;
  PreparationJournal& operator=(const PreparationJournal&) = delete;

  const NativeStoredModelHandle& handle() const noexcept { return m_handle; }
  const ndn::Name& prefix() const noexcept { return m_prefix; }
  bool ready() const { return m_metadata.at("ready").get<bool>(); }
  bool hasCurrent() const { return !m_metadata.at("current").is_null(); }
  std::size_t objectCount() const { return m_metadata.at("objects").size(); }
  unsigned currentGroupingVersion() const
  { return uploadGroupingVersion(m_metadata.at("current")); }
  unsigned tailGroupingVersion() const
  { return uploadGroupingVersion(m_metadata.at("context"), "tailUploadGroupingVersion"); }

  std::optional<NativeStoredModelIndexedObject>
  completed(const std::string& id) const
  {
    const auto found = m_metadata.at("objects").find(id);
    if (found == m_metadata.at("objects").end()) return std::nullopt;
    return decodeIndexedObject(found->at("entry"));
  }

  std::optional<NativeStoredModelIndexedObject>
  current(const std::string& id) const
  {
    const auto& value = m_metadata.at("current");
    if (value.is_null()) return std::nullopt;
    require(value.at("id") == id);
    return decodeIndexedObject(value.at("entry"));
  }

  Bytes
  currentCiphertext(const NativeStoredModelIndexedObject& entry) const
  {
    auto bytes = readPrivateFile(m_stage, MAX_OBJECT_BYTES + 28, m_io.requireCurrent);
    require(bytes.size() == entry.object.ciphertextBytes &&
            digest(bytes.data(), bytes.size()) == entry.object.ciphertextDigest);
    return bytes;
  }

  void
  begin(const std::string& id, const NativeStoredModelIndexedObject& entry, const Bytes& cipher)
  {
    require(m_metadata.at("current").is_null());
    detail::writePrivateCheckpointFile(m_stage, {}, [&](int fd) {
      for (std::size_t offset = 0; offset < cipher.size();) {
        m_io.requireCurrent();
        const auto count = std::min<std::size_t>(1U << 20, cipher.size() - offset);
        writeAll(fd, cipher.data() + offset, count);
        offset += count;
      }
    }, [&] { m_io.requireCurrent(); return false; });
    m_metadata["current"] = NativeJson{{"id", id}, {"entry", indexedObjectJson(entry)},
                                     {"uploadGroupingVersion", 2}};
    save();
  }

  void
  complete(const std::string& id, const NativeStoredModelIndexedObject& entry,
           const NativeJson& groups)
  {
    require(m_metadata.at("current").at("id") == id);
    m_metadata["objects"][id] = NativeJson{{"entry", indexedObjectJson(entry)}, {"groups", groups}};
    if (m_metadata.at("current").contains("uploadGroupingVersion"))
      m_metadata["objects"][id]["uploadGroupingVersion"] = currentGroupingVersion();
    m_metadata["current"] = nullptr;
    save();
    removeCompletedStage();
  }

  void
  finish(const std::string& rootDigest, const NativeJson& groups)
  {
    require(m_metadata.at("current").is_null() && isDigest(rootDigest) && !groups.empty());
    const auto previous = m_metadata.at("rootDigest").get<std::string>();
    require(previous.empty() || previous == rootDigest);
    m_handle.rootWireDigest = rootDigest;
    m_handle.validate();
    m_metadata["rootDigest"] = rootDigest;
    m_metadata["tail"] = groups;
    m_metadata["ready"] = true;
    validate();
    save();
  }

private:
  void
  validate()
  {
    require(m_metadata.at("objects").is_object() && m_metadata.at("objects").size() <= 4096 &&
      m_metadata.at("ready").is_boolean() && m_metadata.at("rootDigest").is_string());
    std::uint64_t total = 0;
    std::size_t groupCount = 0;
    std::set<ndn::Name> groupNames;
    const auto checkGroups = [&](const NativeJson& groups, const ndn::Name& prefix,
                                 unsigned version) {
      require(groups.is_array() && !groups.empty());
      for (const auto& group : groups) {
        require(++groupCount <= 65536 && group.is_object() && group.size() == 3 &&
          group.at("bytes").is_number_unsigned() && group.at("bytes").get<std::uint64_t>() > 0 &&
          group.at("bytes").get<std::uint64_t>() <=
            (version == 1 ? MAX_GROUP_WIRE : NATIVE_STORED_MODEL_UPLOAD_WIRE) &&
          isDigest(group.at("digest").get<std::string>()));
        const auto text = group.at("name").get<std::string>();
        const ndn::Name name(text);
        require(text.size() <= 4096 && name.toUri(ndn::name::UriFormat::CANONICAL) == text &&
                prefix.isPrefixOf(name) && groupNames.insert(name).second);
      }
    };
    for (const auto& object : m_metadata.at("objects").items()) {
      require(validObjectId(object.key()) && object.value().is_object() &&
              object.value().size() ==
                (object.value().contains("uploadGroupingVersion") ? 3U : 2U));
      const auto entry = decodeIndexedObject(object.value().at("entry"));
      require(entry.object.name == nativeStoredModelObjectName(m_prefix, object.key()) &&
              entry.object.plaintextBytes <= MAX_PUBLICATION_BYTES - total);
      total += entry.object.plaintextBytes;
      checkGroups(object.value().at("groups"), entry.object.name,
                  uploadGroupingVersion(object.value()));
    }
    const auto& current = m_metadata.at("current");
    require(current.is_null() || (current.is_object() && current.size() ==
            (current.contains("uploadGroupingVersion") ? 3U : 2U)));
    if (!current.is_null()) {
      (void)currentGroupingVersion();
      const auto id = current.at("id").get<std::string>();
      const auto entry = decodeIndexedObject(current.at("entry"));
      require(validObjectId(id) && !m_metadata.at("objects").contains(id) &&
        entry.object.name == nativeStoredModelObjectName(m_prefix, id) &&
        entry.object.plaintextBytes <= MAX_PUBLICATION_BYTES - total);
    }
    const auto rootDigest = m_metadata.at("rootDigest").get<std::string>();
    require(rootDigest.empty() || isDigest(rootDigest));
    require(m_metadata.at("tail").is_array());
    if (!m_metadata.at("tail").empty())
      checkGroups(m_metadata.at("tail"), m_prefix, tailGroupingVersion());
    if (ready()) {
      require(current.is_null() && !m_metadata.at("tail").empty() && isDigest(rootDigest) &&
        m_metadata.at("objects").contains(
          m_metadata.at("context").at("catalogId").get<std::string>()) &&
        m_metadata.at("objects").contains(
          m_metadata.at("context").at("manifestId").get<std::string>()) &&
        m_metadata.at("tail").back().at("name") ==
          m_handle.rootName.toUri(ndn::name::UriFormat::CANONICAL) &&
        m_metadata.at("tail").back().at("digest") == rootDigest);
      m_handle.rootWireDigest = rootDigest;
      m_handle.validate();
    }
  }

  void
  confirm(const NativeJson& groups) const
  {
    for (const auto& group : groups) {
      m_io.requireCurrent();
      m_io.requireCommitted(ndn::Name(group.at("name").get<std::string>()),
        group.at("digest").get<std::string>(), group.at("bytes").get<std::uint64_t>());
    }
  }

  void
  save()
  {
    m_io.requireCurrent();
    const auto prefix = std::string(MAGIC) + nativeCanonicalJson(m_metadata);
    require(prefix.size() + 103 <= MAX_JOURNAL_BYTES);
    Bytes bytes(prefix.begin(), prefix.end());
    bytes.reserve(prefix.size() + 103);
    Cleanse guard{bytes};
    bytes.insert(bytes.end(), m_handle.masterKey.begin(), m_handle.masterKey.end());
    const auto checksum = digest(bytes.data(), bytes.size());
    bytes.insert(bytes.end(), checksum.begin(), checksum.end());
    detail::writePrivateCheckpointFile(m_path, {}, [&](int fd) {
      writeAll(fd, bytes.data(), bytes.size());
    }, [&] { m_io.requireCurrent(); return false; });
  }

  void
  removeCompletedStage()
  {
    struct stat status{};
    if (::lstat(m_stage.c_str(), &status) != 0) {
      require(errno == ENOENT);
      return;
    }
    // With a valid locked journal and no current entry, this file is either a completed spool
    // or a write interrupted before begin() committed its entry; neither can be published anew.
    require(S_ISREG(status.st_mode) && status.st_uid == ::geteuid() && status.st_nlink == 1 &&
      (status.st_mode & 07777) == 0600 && status.st_size > 0 &&
      static_cast<std::uint64_t>(status.st_size) <= MAX_OBJECT_BYTES + 28);
    m_io.requireCurrent();
    require(::unlink(m_stage.c_str()) == 0 && ::fsync(m_parent.value) == 0);
  }

  static constexpr char MAGIC[] = "NDNSF-DI-MODEL-PREPARATION-v1\n";
  static constexpr std::uint64_t MAX_JOURNAL_BYTES = 64U << 20;
  std::filesystem::path m_path, m_stage;
  NativeStoredModelPacketIo m_io;
  FileDescriptor m_parent{-1}, m_lock{-1};
  NativeStoredModelHandle m_handle;
  ndn::Name m_prefix;
  NativeJson m_metadata;
};

NativeJson
catalogJson(const NativeStoredModelCatalog& catalog, const ndn::Name& producer)
{
  require(isDigest(catalog.modelDigest()) && validObjectId(catalog.catalogId()) &&
          validObjectId(catalog.manifestId()) && catalog.catalogId() != catalog.manifestId() &&
          !catalog.objects().empty() && catalog.objects().size() <= 4096 &&
          catalog.objects().count(catalog.catalogId()) == 1 &&
          catalog.objects().count(catalog.manifestId()) == 1);
  auto objects = NativeJson::object();
  std::set<ndn::Name> names;
  std::uint64_t totalBytes = 0;
  for (const auto& [id, entry] : catalog.objects()) {
    const auto& object = entry.object;
    requireObjectName(producer, object.name);
    require(validObjectId(id) && names.insert(object.name).second &&
            isDigest(object.plaintextDigest) && isDigest(object.ciphertextDigest) &&
            object.plaintextBytes <= MAX_OBJECT_BYTES &&
            object.ciphertextBytes == object.plaintextBytes + 28 &&
            entry.segmentBytes >= 512 && entry.segmentBytes <= segmentPayload(object.name) &&
            object.plaintextBytes <= MAX_PUBLICATION_BYTES - totalBytes);
    totalBytes += object.plaintextBytes;
    objects[id] = indexedObjectJson(entry);
  }
  return NativeJson{{"schema", "ndnsf-di-stored-model-index-v1"},
    {"modelDigest", catalog.modelDigest()}, {"catalogId", catalog.catalogId()},
    {"manifestId", catalog.manifestId()}, {"objects", std::move(objects)}};
}

std::string
storedRootJson(const std::string& modelDigest, const ndn::Name& indexName,
               const std::string& indexDigest, std::uint64_t indexBytes,
               std::uint32_t segmentBytes)
{
  return nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-stored-model-root-v1"}, {"modelDigest", modelDigest},
    {"indexName", indexName.toUri(ndn::name::UriFormat::CANONICAL)},
    {"indexDigest", indexDigest}, {"indexBytes", indexBytes}, {"segmentBytes", segmentBytes},
    {"segmentCount", (indexBytes + segmentBytes - 1) / segmentBytes}});
}

Bytes
readSegments(const ndn::Name& prefix, std::uint64_t size, std::uint32_t segmentBytes,
             const NativeStoredModelPacketIo& io)
{
  require(io.fetch && io.requireCurrent && size > 0 && size <= MAX_OBJECT_BYTES + 28 &&
          segmentBytes >= 512 && segmentBytes <= segmentPayload(prefix));
  const auto count = (size + segmentBytes - 1) / segmentBytes;
  const auto finalBlock = ndn::name::Component::fromSegment(count - 1);
  Bytes content;
  content.reserve(static_cast<std::size_t>(size));
  for (std::uint64_t first = 0; first < count;) {
    io.requireCurrent();
    std::vector<ndn::Name> names;
    std::size_t nameBytes = 0;
    // Bound request vectors as well as returned wire batches, including long Names.
    while (first + names.size() < count && names.size() < MAX_GROUP_PACKETS &&
           (names.size() + 1) * ndn::MAX_NDN_PACKET_SIZE <= MAX_GROUP_WIRE) {
      auto name = prefix;
      name.appendSegment(first + names.size());
      const auto bytes = name.toUri(ndn::name::UriFormat::CANONICAL).size();
      if (!names.empty() && bytes > MAX_GROUP_NAMES - nameBytes)
        break;
      require(bytes <= MAX_GROUP_NAMES - nameBytes);
      nameBytes += bytes;
      names.push_back(std::move(name));
    }
    const auto packets = io.fetch(names);
    io.requireCurrent();
    require(packets.size() == names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
      io.requireCurrent();
      const auto packet = decodeOriginal(packets[i]);
      const auto expectedBytes = std::min<std::uint64_t>(segmentBytes, size - content.size());
      require(packet.getName() == names[i] && packet.getFinalBlock() &&
              *packet.getFinalBlock() == finalBlock &&
              packet.getContentType() == ndn::tlv::ContentType_Blob &&
              packet.getContent().value_size() == expectedBytes &&
              packet.getSignatureType() == ndn::tlv::DigestSha256 &&
              ndn::security::verifySignature(packet, std::optional<ndn::security::Certificate>{}));
      content.insert(content.end(), packet.getContent().value_begin(),
                     packet.getContent().value_end());
    }
    first += names.size();
  }
  require(content.size() == size);
  return content;
}

void
commitPackets(const std::vector<std::shared_ptr<ndn::Data>>& packets,
               const NativeStoredModelPacketIo& io, NativeStoredModelPublication& publication,
               unsigned version = 2,
               std::function<void(const ndn::Name&, const std::string&, std::uint64_t)>
                 observe = {})
{
  require(version == 1 || version == 2);
  const auto maxPackets = version == 1 ? MAX_GROUP_PACKETS : NATIVE_STORED_MODEL_UPLOAD_PACKETS;
  const auto maxNames = version == 1 ? MAX_GROUP_NAMES : NATIVE_STORED_MODEL_UPLOAD_NAMES;
  const auto maxWire = version == 1 ? MAX_GROUP_WIRE : NATIVE_STORED_MODEL_UPLOAD_WIRE;
  NativeStoredModelPacketIo::PacketWires group;
  std::size_t nameBytes = 0;
  Bytes digestInput;
  NativeModelCommitPipeline pipeline(io.requireCurrent);
  const auto flush = [&] {
    if (group.empty())
      return;
    io.requireCurrent();
    const auto expected = digest(digestInput.data(), digestInput.size());
    const auto bytes = static_cast<std::uint64_t>(digestInput.size());
    const auto name = decodeOriginal(group.front()).getName();
    const auto committed = [&, observe, bytes, name, expected] {
      publication.wireBytes += bytes;
      ++publication.packetGroups;
      if (observe) observe(name, expected, bytes);
    };
    if (io.commitConcurrent) {
      pipeline.submit(bytes, [commit = io.commitConcurrent, wires = std::move(group), expected]
        (const NativeModelCommitPipeline::CurrentCheck& current) mutable {
          commit(std::move(wires), expected, current);
        }, committed);
    }
    else {
      io.commit(std::move(group), expected);
      io.requireCurrent();
      committed();
    }
    group.clear();
    digestInput.clear();
    nameBytes = 0;
  };
  for (const auto& packet : packets) {
    io.requireCurrent();
    const auto& wire = packet->wireEncode();
    const auto name = packet->getName().toUri(ndn::name::UriFormat::CANONICAL);
    require(wire.size() <= ndn::MAX_NDN_PACKET_SIZE && name.size() <= maxNames);
    if (!group.empty() && (group.size() == maxPackets ||
        wire.size() > maxWire - digestInput.size() ||
        name.size() > maxNames - nameBytes))
      flush();
    nameBytes += name.size();
    digestInput.insert(digestInput.end(), wire.begin(), wire.end());
    group.emplace_back(wire.begin(), wire.end());
  }
  flush();
  pipeline.finish();
}

} // namespace

ndn::Name
nativeStoredModelObjectName(const ndn::Name& publicationPrefix, const std::string& id)
{
  require(!publicationPrefix.empty() && validObjectId(id));
  auto name = publicationPrefix;
  name.append("objects").append(id).appendVersion(0);
  return name;
}

std::uint64_t
nativePreflightStoredModelPublication(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& publicationPrefix, const std::string& modelDigest,
  const std::string& catalogId, const std::string& manifestId,
  const std::vector<std::pair<std::string, std::uint64_t>>& objects)
{
  requirePublicationPrefix(producer, publicationPrefix);
  require(!objects.empty() && objects.size() <= 4096);
  NativeStoredModelCatalog catalog;
  catalog.m_modelDigest = modelDigest;
  catalog.m_catalogId = catalogId;
  catalog.m_manifestId = manifestId;
  const auto placeholder = digest(nullptr, 0);
  std::uint64_t wireBytes = 0;
  const auto addWireBound = [&](const ndn::Name& name, std::uint64_t size,
                                std::uint32_t segmentBytes) {
    // segmentPayload reserves 128 bytes for typed segment/final-block, TLVs and DigestSha256.
    const auto packets = (size + segmentBytes - 1) / segmentBytes;
    const auto bound = size + packets * (name.wireEncode().size() + 128);
    require(bound <= MAX_PUBLICATION_BYTES - wireBytes);
    wireBytes += bound;
  };
  for (const auto& item : objects) {
    require(validObjectId(item.first) && item.second <= MAX_OBJECT_BYTES);
    const auto name = nativeStoredModelObjectName(publicationPrefix, item.first);
    const auto segmentBytes = segmentPayload(name);
    NativeStoredModelIndexedObject entry{
      {name, placeholder, placeholder, item.second, item.second + 28}, segmentBytes};
    require(catalog.m_objects.emplace(item.first, std::move(entry)).second);
    addWireBound(name, item.second + 28, segmentBytes);
  }
  const auto index = nativeCanonicalJson(catalogJson(catalog, producer));
  require(index.size() <= MAX_INDEX_BYTES);
  const auto indexName = ndn::Name(publicationPrefix).append("index").appendVersion(0);
  const auto segmentBytes = segmentPayload(indexName);
  addWireBound(indexName, index.size(), segmentBytes);
  ndn::Data root(ndn::Name(publicationPrefix).append("root"));
  root.setContent(std::string_view(storedRootJson(
    modelDigest, indexName, placeholder, index.size(), segmentBytes)));
  keyChain.sign(root, ndn::security::signingWithSha256());
  require(root.wireEncode().size() <= ndn::MAX_NDN_PACKET_SIZE &&
          root.wireEncode().size() <= MAX_PUBLICATION_BYTES - wireBytes);
  return wireBytes + root.wireEncode().size();
}

NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io)
{
  return nativePublishStoredModel(keyChain, producer, repository, publicationPrefix, modelDigest,
                                  catalogId, manifestId, nextObject, io, {});
}

NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState)
{
  return nativePublishStoredModel(keyChain, producer, repository, publicationPrefix, modelDigest,
    catalogId, manifestId, nextObject, io, preparationState, {});
}

NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState,
  const std::function<void(const ndn::Name&)>& bindPrefix)
{
  require(io.commit && io.requireCurrent && nextObject && !repository.empty() &&
          repository.wireEncode().size() <= 4096 &&
          isDigest(modelDigest) && validObjectId(catalogId) && validObjectId(manifestId) &&
          catalogId != manifestId);
  require(!producer.empty() && (!publicationPrefix.empty() || !preparationState.empty()));
  auto prefix = publicationPrefix;
  if (!prefix.empty()) requirePublicationPrefix(producer, prefix);
  io.requireCurrent();
  NativeStoredModelPublication publication;
  auto& handle = publication.handle;
  handle.producer = producer;
  handle.repository = repository;
  if (!prefix.empty()) handle.rootName = ndn::Name(prefix).append("root");
  std::unique_ptr<PreparationJournal> journal;
  if (!preparationState.empty()) {
    journal = std::make_unique<PreparationJournal>(preparationState, NativeJson{
      {"producer", producer.toUri(ndn::name::UriFormat::CANONICAL)},
      {"repository", repository.toUri(ndn::name::UriFormat::CANONICAL)},
      {"publicationPrefix", prefix.empty() ? std::string{} :
        prefix.toUri(ndn::name::UriFormat::CANONICAL)},
      {"modelDigest", modelDigest}, {"catalogId", catalogId}, {"manifestId", manifestId}}, io);
    handle = journal->handle();
    prefix = journal->prefix();
    if (journal->ready()) {
      publication.objectCount = journal->objectCount();
      io.requireCurrent();
      return publication;
    }
  }
  else {
    handle.masterKey.resize(32);
    require(RAND_bytes(handle.masterKey.data(), handle.masterKey.size()) == 1);
  }
  if (bindPrefix) bindPrefix(prefix);
  io.requireCurrent();
  const auto recording = [&](NativeJson& groups) {
    return [&groups](const ndn::Name& name, const std::string& expected, std::uint64_t bytes) {
      groups.push_back(NativeJson{{"name", name.toUri(ndn::name::UriFormat::CANONICAL)},
                                 {"digest", expected}, {"bytes", bytes}});
    };
  };
  NativeStoredModelCatalog catalog;
  catalog.m_modelDigest = modelDigest;
  catalog.m_catalogId = catalogId;
  catalog.m_manifestId = manifestId;
  ndn::Segmenter segmenter(keyChain, ndn::security::signingWithSha256());
  while (true) {
    io.requireCurrent();
    auto input = nextObject();
    if (!input) {
      io.requireCurrent();
      break;
    }
    Cleanse plaintextGuard{input->plaintext};
    io.requireCurrent();
    require(validObjectId(input->id) && catalog.m_objects.size() < 4096 &&
            catalog.m_objects.count(input->id) == 0 &&
            input->plaintext.size() <= MAX_PUBLICATION_BYTES - publication.sourceBytes);
    const auto name = nativeStoredModelObjectName(prefix, input->id);
    NativeStoredModelIndexedObject entry;
    publication.sourceBytes += input->plaintext.size();
    if (journal) {
      if (const auto completed = journal->completed(input->id)) {
        require(completed->object.plaintextBytes == input->plaintext.size() &&
          completed->object.plaintextDigest ==
            digest(input->plaintext.data(), input->plaintext.size()));
        catalog.m_objects.emplace(input->id, *completed);
        ++publication.objectCount;
        continue;
      }
    }
    Bytes ciphertext;
    const auto pending = journal ? journal->current(input->id) : std::nullopt;
    if (pending) {
      entry = *pending;
      require(entry.object.plaintextBytes == input->plaintext.size() &&
        entry.object.plaintextDigest == digest(input->plaintext.data(), input->plaintext.size()));
      ciphertext = journal->currentCiphertext(entry);
    }
    else {
      entry.segmentBytes = segmentPayload(name);
      ciphertext = nativeSealStoredModelObject(handle.masterKey, producer, name,
                                               input->plaintext, entry.object);
      if (journal) journal->begin(input->id, entry, ciphertext);
    }
    const auto packets = segmenter.segment(ciphertext, name, entry.segmentBytes,
                                           ndn::time::milliseconds::zero());
    if (journal) {
      auto groups = NativeJson::array();
      commitPackets(packets, io, publication, journal->currentGroupingVersion(), recording(groups));
      journal->complete(input->id, entry, groups);
    }
    else {
      commitPackets(packets, io, publication);
    }
    catalog.m_objects.emplace(input->id, std::move(entry));
    ++publication.objectCount;
  }
  io.requireCurrent();
  require(!journal ||
          (!journal->hasCurrent() && journal->objectCount() == publication.objectCount));
  const auto index = nativeCanonicalJson(catalogJson(catalog, producer));
  require(index.size() <= MAX_INDEX_BYTES);
  const auto indexName = ndn::Name(prefix).append("index").appendVersion(0);
  const auto indexSegmentBytes = segmentPayload(indexName);
  const Bytes indexBytes(index.begin(), index.end());
  auto tailGroups = NativeJson::array();
  std::function<void(const ndn::Name&, const std::string&, std::uint64_t)> tailObserver;
  if (journal) tailObserver = recording(tailGroups);
  const auto tailVersion = journal ? journal->tailGroupingVersion() : 2;
  commitPackets(segmenter.segment(indexBytes, indexName, indexSegmentBytes,
                                   ndn::time::milliseconds::zero()), io, publication, tailVersion,
                tailObserver);
  auto root = std::make_shared<ndn::Data>(handle.rootName);
  const auto rootJson = storedRootJson(modelDigest, indexName,
    digest(indexBytes.data(), indexBytes.size()), index.size(), indexSegmentBytes);
  root->setContent(std::string_view(rootJson));
  keyChain.sign(*root, ndn::security::signingWithSha256());
  const auto& rootWire = root->wireEncode();
  handle.rootWireDigest = digest(rootWire.data(), rootWire.size());
  handle.validate();
  (void)nativeAdmitStoredModelRoot(handle, *root);
  commitPackets({root}, io, publication, tailVersion, tailObserver);
  io.requireCurrent();
  if (journal) journal->finish(handle.rootWireDigest, tailGroups);
  return publication;
}

NativeStoredModelCatalog
nativeLoadStoredModelCatalog(const NativeStoredModelReference& publication,
                             const NativeStoredModelPacketIo& io)
{
  publication.validate();
  require(io.fetch && io.requireCurrent);
  io.requireCurrent();
  const auto roots = io.fetch({publication.rootName});
  io.requireCurrent();
  require(roots.size() == 1);
  const auto root = nativeAdmitStoredModelRoot(publication, decodeOriginal(roots.front()));
  const auto bytes = readSegments(root.indexName, root.indexBytes, root.segmentBytes, io);
  require(digest(bytes.data(), bytes.size()) == root.indexDigest);
  const auto value = nativeParseJson(std::string(bytes.begin(), bytes.end()));
  require(value.is_object() && value.size() == 5 &&
          value.at("schema") == "ndnsf-di-stored-model-index-v1" &&
          value.at("modelDigest") == root.modelDigest && value.at("objects").is_object() &&
          value.at("objects").size() <= 4096);
  NativeStoredModelCatalog catalog;
  catalog.m_publication = publication;
  catalog.m_modelDigest = root.modelDigest;
  catalog.m_catalogId = value.at("catalogId").get<std::string>();
  catalog.m_manifestId = value.at("manifestId").get<std::string>();
  for (const auto& item : value.at("objects").items()) {
    io.requireCurrent();
    catalog.m_objects.emplace(item.key(), decodeIndexedObject(item.value()));
  }
  (void)catalogJson(catalog, publication.producer);
  io.requireCurrent();
  return catalog;
}

Bytes
nativeReadStoredModelObject(const NativeStoredModelHandle& handle,
                            const NativeStoredModelCatalog& catalog, const std::string& id,
                            const NativeStoredModelPacketIo& io)
{
  handle.validate();
  const auto& admitted = catalog.publication();
  require(admitted.producer == handle.producer && admitted.rootName == handle.rootName &&
          admitted.rootWireDigest == handle.rootWireDigest);
  const auto object = catalog.objects().find(id);
  require(object != catalog.objects().end());
  const auto wire = readSegments(object->second.object.name,
    object->second.object.ciphertextBytes, object->second.segmentBytes, io);
  auto plaintext = nativeOpenStoredModelObject(handle, object->second.object, wire);
  try {
    io.requireCurrent();
  }
  catch (...) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    throw;
  }
  return plaintext;
}

} // namespace ndnsf::di
