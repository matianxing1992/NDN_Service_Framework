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
#include <optional>
#include <set>
#include <stdexcept>
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

} // namespace

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
  const int raw = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  require(raw >= 0);
  const FileDescriptor fd{raw};
  struct stat status{};
  require(::fstat(raw, &status) == 0 && S_ISREG(status.st_mode) &&
          status.st_uid == ::geteuid() && status.st_nlink == 1 &&
          (status.st_mode & 07777) == 0600 && status.st_size > 0 && status.st_size <= 32768);
  Bytes bytes(static_cast<std::size_t>(status.st_size));
  Cleanse guard{bytes};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::read(raw, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    require(count > 0);
    offset += count;
  }
  std::uint8_t extra;
  require(::read(raw, &extra, 1) == 0);
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
constexpr std::uint64_t MAX_PUBLICATION_BYTES = 100000000000ULL;

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

ndn::Data
decodeOriginal(const Bytes& wire)
{
  require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
  const ndn::Block block{ndn::span<const std::uint8_t>(wire.data(), wire.size())};
  require(block.size() == wire.size());
  return ndn::Data(block);
}

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
    objects[id] = NativeJson{
      {"name", object.name.toUri(ndn::name::UriFormat::CANONICAL)},
      {"plaintextDigest", object.plaintextDigest}, {"ciphertextDigest", object.ciphertextDigest},
      {"plaintextBytes", object.plaintextBytes}, {"ciphertextBytes", object.ciphertextBytes},
      {"segmentBytes", entry.segmentBytes}};
  }
  return NativeJson{{"schema", "ndnsf-di-stored-model-index-v1"},
    {"modelDigest", catalog.modelDigest()}, {"catalogId", catalog.catalogId()},
    {"manifestId", catalog.manifestId()}, {"objects", std::move(objects)}};
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
               const NativeStoredModelPacketIo& io, NativeStoredModelPublication& publication)
{
  NativeStoredModelPacketIo::PacketWires group;
  std::size_t nameBytes = 0;
  Bytes digestInput;
  const auto flush = [&] {
    if (group.empty())
      return;
    io.requireCurrent();
    const auto expected = digest(digestInput.data(), digestInput.size());
    publication.wireBytes += digestInput.size();
    io.commit(std::move(group), expected);
    io.requireCurrent();
    ++publication.packetGroups;
    group.clear();
    digestInput.clear();
    nameBytes = 0;
  };
  for (const auto& packet : packets) {
    io.requireCurrent();
    const auto& wire = packet->wireEncode();
    const auto name = packet->getName().toUri(ndn::name::UriFormat::CANONICAL);
    require(wire.size() <= ndn::MAX_NDN_PACKET_SIZE && name.size() <= MAX_GROUP_NAMES);
    if (!group.empty() && (group.size() == MAX_GROUP_PACKETS ||
        wire.size() > MAX_GROUP_WIRE - digestInput.size() ||
        name.size() > MAX_GROUP_NAMES - nameBytes))
      flush();
    nameBytes += name.size();
    digestInput.insert(digestInput.end(), wire.begin(), wire.end());
    group.emplace_back(wire.begin(), wire.end());
  }
  flush();
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

NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io)
{
  require(io.commit && io.requireCurrent && nextObject && !repository.empty() &&
          repository.wireEncode().size() <= 4096 &&
          isDigest(modelDigest) && validObjectId(catalogId) && validObjectId(manifestId) &&
          catalogId != manifestId);
  requireObjectName(producer, publicationPrefix);
  // Bound both the binary root Name and the textual index Name before any source read or write.
  require(publicationPrefix.toUri(ndn::name::UriFormat::CANONICAL).size() <=
          (ndn::MAX_NDN_PACKET_SIZE - 1024) / 2);
  io.requireCurrent();
  NativeStoredModelPublication publication;
  auto& handle = publication.handle;
  handle.producer = producer;
  handle.repository = repository;
  handle.rootName = ndn::Name(publicationPrefix).append("root");
  handle.masterKey.resize(32);
  require(RAND_bytes(handle.masterKey.data(), handle.masterKey.size()) == 1);
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
    const auto name = nativeStoredModelObjectName(publicationPrefix, input->id);
    NativeStoredModelIndexedObject entry;
    entry.segmentBytes = segmentPayload(name);
    const auto ciphertext = nativeSealStoredModelObject(handle.masterKey, producer, name,
                                                        input->plaintext, entry.object);
    publication.sourceBytes += input->plaintext.size();
    commitPackets(segmenter.segment(ciphertext, name, entry.segmentBytes,
                                     ndn::time::milliseconds::zero()), io, publication);
    catalog.m_objects.emplace(input->id, std::move(entry));
    ++publication.objectCount;
  }
  io.requireCurrent();
  const auto index = nativeCanonicalJson(catalogJson(catalog, producer));
  require(index.size() <= MAX_INDEX_BYTES);
  const auto indexName = ndn::Name(publicationPrefix).append("index").appendVersion(0);
  const auto indexSegmentBytes = segmentPayload(indexName);
  const Bytes indexBytes(index.begin(), index.end());
  commitPackets(segmenter.segment(indexBytes, indexName, indexSegmentBytes,
                                   ndn::time::milliseconds::zero()), io, publication);
  auto root = std::make_shared<ndn::Data>(handle.rootName);
  const auto rootJson = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-stored-model-root-v1"}, {"modelDigest", modelDigest},
    {"indexName", indexName.toUri(ndn::name::UriFormat::CANONICAL)},
    {"indexDigest", digest(indexBytes.data(), indexBytes.size())}, {"indexBytes", index.size()},
    {"segmentBytes", indexSegmentBytes},
    {"segmentCount", (index.size() + indexSegmentBytes - 1) / indexSegmentBytes}});
  root->setContent(std::string_view(rootJson));
  keyChain.sign(*root, ndn::security::signingWithSha256());
  const auto& rootWire = root->wireEncode();
  handle.rootWireDigest = digest(rootWire.data(), rootWire.size());
  handle.validate();
  (void)nativeAdmitStoredModelRoot(handle, *root);
  commitPackets({root}, io, publication);
  io.requireCurrent();
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
    const auto& entry = item.value();
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
    catalog.m_objects.emplace(item.key(), std::move(object));
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
