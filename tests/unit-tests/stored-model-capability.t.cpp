#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-symmetric-crypto.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <openssl/sha.h>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace ndnsf::di::tests {
namespace {

using Bytes = std::vector<std::uint8_t>;

std::string
hex(const Bytes& bytes)
{
  std::string value;
  for (const auto c : bytes) {
    value += "0123456789abcdef"[c >> 4];
    value += "0123456789abcdef"[c & 15];
  }
  return value;
}

std::string
hash(const std::uint8_t* bytes, std::size_t size)
{
  Bytes result(SHA256_DIGEST_LENGTH);
  SHA256(bytes, size, result.data());
  return "sha256:" + hex(result);
}

struct Fixture
{
  ndn::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  NativeStoredModelHandle handle;
  ndn::Data root{ndn::Name("/publisher/models/version/root")};

  Fixture()
  {
    handle.producer = ndn::Name("/publisher");
    handle.repository = ndn::Name("/repo/service");
    handle.rootName = root.getName();
    handle.masterKey = Bytes(32, 0x71);
    const auto value = nativeCanonicalJson(NativeJson{
      {"schema", "ndnsf-di-stored-model-root-v1"},
      {"modelDigest", "sha256:" + std::string(64, 'a')},
      {"indexName", "/publisher/models/version/index"},
      {"indexDigest", "sha256:" + std::string(64, 'b')},
      {"indexBytes", 7000}, {"segmentBytes", 6144}, {"segmentCount", 2}});
    root.setContent(std::string_view(value));
    signRoot();
  }

  void
  signRoot()
  {
    keyChain.sign(root, ndn::security::signingWithSha256());
    const auto& wire = root.wireEncode();
    handle.rootWireDigest = hash(wire.data(), wire.size());
  }
};

struct PrivateFile
{
  std::filesystem::path directory;
  std::filesystem::path path;

  PrivateFile()
  {
    char name[] = "/tmp/ndnsf-stored-capability-XXXXXX";
    const auto result = ::mkdtemp(name);
    if (!result)
      throw std::runtime_error("private fixture directory unavailable");
    directory = result;
    path = directory / "model.handle";
  }

  ~PrivateFile() { std::filesystem::remove_all(directory); }
};

struct PacketRepository
{
  std::map<ndn::Name, Bytes> packets;
  std::vector<ndn::Name> writes;
  std::vector<ndn::Name> reads;
  std::size_t sourceReads = 0;
  std::vector<NativeStoredModelInput> source{
    {"catalog", Bytes{'c', 'a', 't'}}, {"manifest", Bytes{'r', 'o', 'o', 't'}},
    {"layer0", Bytes(20000, 0x73)}};

  NativeStoredModelPacketIo
  io(bool writable = true)
  {
    NativeStoredModelPacketIo port;
    port.requireCurrent = [] {};
    if (writable) {
      port.commit = [this] (NativeStoredModelPacketIo::PacketWires wires,
                            const std::string& expected) {
        BOOST_REQUIRE(!wires.empty());
        BOOST_CHECK_LE(wires.size(), 1024);
        Bytes all;
        std::size_t names = 0;
        for (const auto& wire : wires) {
          const ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire))};
          names += data.getName().toUri(ndn::name::UriFormat::CANONICAL).size();
          BOOST_CHECK_EQUAL(data.getSignatureType(), ndn::tlv::DigestSha256);
          BOOST_CHECK_LE(wire.size(), ndn::MAX_NDN_PACKET_SIZE);
          const auto existing = packets.find(data.getName());
          if (existing != packets.end() && existing->second != wire)
            throw std::runtime_error("immutable packet conflict");
          packets[data.getName()] = wire;
          writes.push_back(data.getName());
          all.insert(all.end(), wire.begin(), wire.end());
        }
        BOOST_CHECK_LE(names, 128U << 10);
        BOOST_CHECK_LE(all.size(), 8U << 20);
        BOOST_REQUIRE_EQUAL(hash(all.data(), all.size()), expected);
        // Re-read the retained original wires independently, as required by the commit port.
        Bytes retained;
        for (const auto& wire : wires) {
          const ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire))};
          const auto& stored = packets.at(data.getName());
          retained.insert(retained.end(), stored.begin(), stored.end());
        }
        BOOST_REQUIRE_EQUAL(hash(retained.data(), retained.size()), expected);
      };
    }
    port.fetch = [this] (const std::vector<ndn::Name>& names) {
      NativeStoredModelPacketIo::PacketWires result;
      for (const auto& name : names) {
        reads.push_back(name);
        result.push_back(packets.at(name));
      }
      return result;
    };
    return port;
  }

  NativeStoredModelPublication
  publish(const NativeStoredModelPacketIo* custom = nullptr)
  {
    ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
    auto port = custom ? *custom : io();
    std::size_t next = 0;
    return nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
      ndn::Name("/preparer/models/v=1"), "sha256:" + std::string(64, 'd'),
      "catalog", "manifest", [&] () -> std::optional<NativeStoredModelInput> {
        if (next == source.size())
          return std::nullopt;
        ++sourceReads;
        return source[next++];
      }, port);
  }

  void
  corruptContent(const ndn::Name& name)
  {
    auto& wire = packets.at(name);
    ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire))};
    auto content = Bytes(data.getContent().value_begin(), data.getContent().value_end());
    BOOST_REQUIRE(!content.empty());
    content.front() ^= 1;
    data.setContent(content);
    ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
    keys.sign(data, ndn::security::signingWithSha256());
    const auto& changed = data.wireEncode();
    wire.assign(changed.begin(), changed.end());
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(StoredModelCapability)

BOOST_AUTO_TEST_CASE(ExistingPrimitivesMatchIndependentStandardVectors)
{
  // RFC 5869 test case 3: absent salt and empty info; first 32 OKM bytes.
  BOOST_CHECK_EQUAL(hex(detail::deriveSymmetricKey(Bytes(22, 0x0b), "")),
    "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d");
  // NIST AES-256-GCM: all-zero key, nonce and one-block plaintext; empty AAD.
  const auto cipher = detail::cryptSymmetric(true, Bytes(32), Bytes(12), Bytes(16), "");
  BOOST_CHECK_EQUAL(hex(cipher),
    "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");
  BOOST_CHECK(detail::cryptSymmetric(false, Bytes(32), Bytes(12), cipher, "") == Bytes(16));
}

BOOST_FIXTURE_TEST_CASE(TrustedRootAdmitsOnlyBoundOriginalData, Fixture)
{
  const auto admitted = nativeAdmitStoredModelRoot(handle, root);
  BOOST_CHECK_EQUAL(admitted.segmentCount, 2);
  BOOST_CHECK_EQUAL(admitted.indexBytes, 7000);
  auto foreign = handle;
  foreign.producer = ndn::Name("/other");
  BOOST_CHECK_THROW(nativeAdmitStoredModelRoot(foreign, root), std::runtime_error);
  foreign = handle;
  foreign.rootWireDigest.back() = foreign.rootWireDigest.back() == '0' ? '1' : '0';
  BOOST_CHECK_THROW(nativeAdmitStoredModelRoot(foreign, root), std::runtime_error);
  root.setContent(std::string_view("{}"));
  BOOST_CHECK_THROW(nativeAdmitStoredModelRoot(handle, root), std::runtime_error);
  signRoot();
  BOOST_CHECK_THROW(nativeAdmitStoredModelRoot(handle, root), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(IdentitySignatureDoesNotSelectTheNewFormat, Fixture)
{
  const auto identity = keyChain.createIdentity(ndn::Name("/publisher"));
  keyChain.sign(root, ndn::security::signingByIdentity(identity.getName()));
  const auto& wire = root.wireEncode();
  handle.rootWireDigest = hash(wire.data(), wire.size());
  BOOST_CHECK_THROW(nativeAdmitStoredModelRoot(handle, root), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(ObjectCiphertextIsReusableAcrossUsersAndBoundToItsName, Fixture)
{
  NativeStoredModelObject reference;
  const Bytes input{0, 1, 7, 0, 255};
  const auto wire = nativeSealStoredModelObject(handle.masterKey, handle.producer,
    ndn::Name("/publisher/models/version/layer0"), input, reference);
  BOOST_CHECK_EQUAL(wire.size(), input.size() + 28);
  const auto userB = handle;
  const auto userC = handle;
  BOOST_CHECK(nativeOpenStoredModelObject(userB, reference, wire) == input);
  BOOST_CHECK(nativeOpenStoredModelObject(userC, reference, wire) == input);
  auto bad = reference;
  bad.name = ndn::Name("/publisher/models/version/layer1");
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, bad, wire), std::runtime_error);
  bad = reference;
  bad.name = ndn::Name("/other/models/version/layer0");
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, bad, wire), std::runtime_error);
  auto wrongKey = handle;
  wrongKey.masterKey[0] ^= 1;
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(wrongKey, reference, wire), std::runtime_error);
  auto corrupt = wire;
  corrupt.back() ^= 1;
  bad = reference;
  bad.ciphertextDigest = hash(corrupt.data(), corrupt.size());
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, bad, corrupt), std::runtime_error);
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, reference, corrupt), std::runtime_error);
  bad = reference;
  ++bad.plaintextBytes;
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, bad, wire), std::runtime_error);
  bad = reference;
  bad.plaintextDigest.back() = bad.plaintextDigest.back() == '0' ? '1' : '0';
  BOOST_CHECK_THROW(nativeOpenStoredModelObject(handle, bad, wire), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(IndependentObjectsHaveDifferentKeysAndNewEncryptionHasFreshNonce, Fixture)
{
  const ndn::Name first("/publisher/models/version/layer0");
  const ndn::Name second("/publisher/models/version/layer1");
  BOOST_CHECK(nativeStoredModelObjectKey(handle.masterKey, handle.producer, first) !=
              nativeStoredModelObjectKey(handle.masterKey, handle.producer, second));
  NativeStoredModelObject reference;
  const auto one = nativeSealStoredModelObject(handle.masterKey, handle.producer, first, {}, reference);
  const auto two = nativeSealStoredModelObject(handle.masterKey, handle.producer, second, {}, reference);
  BOOST_CHECK(!std::equal(one.begin(), one.begin() + 12, two.begin()));
  BOOST_CHECK(nativeOpenStoredModelObject(handle, reference, two).empty());
}

BOOST_FIXTURE_TEST_CASE(PrivateHandleRoundtripRejectsPublicPermissionsAndSymlink, Fixture)
{
  PrivateFile file;
  nativeExportStoredModelHandle(handle, file.path);
  struct stat status{};
  BOOST_REQUIRE_EQUAL(::stat(file.path.c_str(), &status), 0);
  BOOST_CHECK_EQUAL(status.st_mode & 07777, 0600);
  const auto loaded = nativeLoadStoredModelHandle(file.path);
  BOOST_CHECK(loaded.masterKey == handle.masterKey);
  BOOST_CHECK(loaded.rootName == handle.rootName);
  BOOST_CHECK_EQUAL(loaded.rootWireDigest, handle.rootWireDigest);
  BOOST_REQUIRE_EQUAL(::chmod(file.path.c_str(), 0644), 0);
  BOOST_CHECK_THROW(nativeLoadStoredModelHandle(file.path), std::runtime_error);
  BOOST_REQUIRE_EQUAL(::chmod(file.path.c_str(), 0600), 0);
  const auto link = file.directory / "link.handle";
  std::filesystem::create_symlink(file.path, link);
  BOOST_CHECK_THROW(nativeLoadStoredModelHandle(link), std::runtime_error);
  BOOST_CHECK_THROW(nativeExportStoredModelHandle(handle, link), std::runtime_error);
  std::ofstream(file.path, std::ios::binary | std::ios::trunc) << "invalid private capability";
  BOOST_CHECK_THROW(nativeLoadStoredModelHandle(file.path), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PublishThenReadWithoutPreparationOrWriteCapability)
{
  PacketRepository repository;
  const auto published = repository.publish();
  BOOST_CHECK_EQUAL(published.objectCount, 3);
  BOOST_CHECK_EQUAL(published.sourceBytes, 20007);
  BOOST_CHECK_EQUAL(published.packetGroups, 5);
  std::uint64_t retainedBytes = 0;
  for (const auto& [name, wire] : repository.packets)
    retainedBytes += wire.size();
  BOOST_CHECK_EQUAL(published.wireBytes, retainedBytes);
  BOOST_REQUIRE(!repository.writes.empty());
  BOOST_CHECK(repository.writes.back() == published.handle.rootName);
  const auto writes = repository.writes.size();
  const auto sourceReads = repository.sourceReads;
  const auto expected = repository.source.back().plaintext;
  repository.source.clear();
  auto readOnly = repository.io(false);
  const NativeStoredModelReference publicReference = published.handle;
  const auto catalog = nativeLoadStoredModelCatalog(publicReference, readOnly);
  static_assert(!std::is_default_constructible_v<NativeStoredModelCatalog>);
  BOOST_CHECK_EQUAL(catalog.objects().size(), 3);
  BOOST_CHECK_EQUAL(catalog.catalogId(), "catalog");
  const auto readsBeforeLayer = repository.reads.size();
  for (const auto& name : repository.reads)
    BOOST_CHECK(!catalog.objects().at("layer0").object.name.isPrefixOf(name));
  BOOST_CHECK(nativeReadStoredModelObject(published.handle, catalog, "layer0", readOnly) == expected);
  BOOST_CHECK_GT(repository.reads.size(), readsBeforeLayer);
  BOOST_CHECK_EQUAL(repository.writes.size(), writes);
  BOOST_CHECK_EQUAL(repository.sourceReads, sourceReads);
  const auto& object = catalog.objects().at("layer0").object;
  const auto key = nativeStoredModelObjectKey(published.handle.masterKey,
                                             publicReference.producer, object.name);
  Bytes cipher;
  const auto count = (object.ciphertextBytes + 6143) / 6144;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto& wire = repository.packets.at(ndn::Name(object.name).appendSegment(i));
    const ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire))};
    cipher.insert(cipher.end(), data.getContent().value_begin(), data.getContent().value_end());
  }
  BOOST_CHECK(nativeOpenStoredModelObjectWithKey(publicReference, object, key, cipher) == expected);
  const auto otherKey = nativeStoredModelObjectKey(published.handle.masterKey,
    publicReference.producer, catalog.objects().at("manifest").object.name);
  BOOST_CHECK_THROW(nativeOpenStoredModelObjectWithKey(publicReference, object, otherKey, cipher),
                    std::runtime_error);
  auto wrongPublication = published.handle;
  wrongPublication.rootWireDigest.back() = wrongPublication.rootWireDigest.back() == '0' ? '1' : '0';
  const auto reads = repository.reads.size();
  BOOST_CHECK_THROW(nativeReadStoredModelObject(wrongPublication, catalog, "layer0", readOnly),
                    std::runtime_error);
  BOOST_CHECK_EQUAL(repository.reads.size(), reads);
}

BOOST_AUTO_TEST_CASE(MissingOrResignedCorruptObjectsFailReadOnly)
{
  PacketRepository repository;
  const auto published = repository.publish();
  const auto readOnly = repository.io(false);
  const auto catalog = nativeLoadStoredModelCatalog(published.handle, readOnly);
  const auto segment = ndn::Name(catalog.objects().at("layer0").object.name).appendSegment(0);
  const auto original = repository.packets.at(segment);
  repository.packets.erase(segment);
  BOOST_CHECK_THROW(nativeReadStoredModelObject(published.handle, catalog, "layer0", readOnly),
                    std::out_of_range);
  repository.packets[segment] = original;
  repository.corruptContent(segment);
  BOOST_CHECK_THROW(nativeReadStoredModelObject(published.handle, catalog, "layer0", readOnly),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ResignedCorruptIndexIsRejectedBeforeObjectFetch)
{
  PacketRepository repository;
  const auto published = repository.publish();
  const auto index = ndn::Name("/preparer/models/v=1/index/v=0").appendSegment(0);
  repository.corruptContent(index);
  BOOST_CHECK_THROW(nativeLoadStoredModelCatalog(published.handle, repository.io(false)),
                    std::runtime_error);
  for (const auto& name : repository.reads)
    BOOST_CHECK(!ndn::Name("/preparer/models/v=1/objects").isPrefixOf(name));
}

BOOST_AUTO_TEST_CASE(FailedCommitNeverReturnsReadyHandle)
{
  PacketRepository repository;
  auto port = repository.io();
  const auto commit = port.commit;
  port.commit = [&] (NativeStoredModelPacketIo::PacketWires wires, const std::string& expected) {
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    if (first.getName() == ndn::Name("/preparer/models/v=1/root"))
      throw std::runtime_error("injected durable commit failure");
    commit(std::move(wires), expected);
  };
  BOOST_CHECK_THROW(repository.publish(&port), std::runtime_error);
  BOOST_CHECK(repository.packets.count(ndn::Name("/preparer/models/v=1/root")) == 0);
}

BOOST_AUTO_TEST_CASE(InvalidPrefixFailsBeforeSourceOrStorageSideEffects)
{
  PacketRepository repository;
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  std::size_t sources = 0;
  BOOST_CHECK_THROW(nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
    ndn::Name("/preparer/" + std::string(4000, 'a')), "sha256:" + std::string(64, 'd'),
    "catalog", "manifest", [&] () -> std::optional<NativeStoredModelInput> {
      ++sources;
      return std::nullopt;
    }, repository.io()), std::runtime_error);
  BOOST_CHECK_EQUAL(sources, 0);
  BOOST_CHECK(repository.writes.empty());
}

BOOST_AUTO_TEST_CASE(CloseAfterFetchOrSourceReadDoesNotAdmitWork)
{
  PacketRepository repository;
  const auto published = repository.publish();
  auto readOnly = repository.io(false);
  const auto catalog = nativeLoadStoredModelCatalog(published.handle, readOnly);
  bool closed = false;
  const auto fetch = readOnly.fetch;
  readOnly.fetch = [&] (const std::vector<ndn::Name>& names) {
    auto result = fetch(names);
    closed = true;
    return result;
  };
  readOnly.requireCurrent = [&] {
    if (closed)
      throw std::runtime_error("owner closed");
  };
  BOOST_CHECK_THROW(nativeReadStoredModelObject(published.handle, catalog, "layer0", readOnly),
                    std::runtime_error);
  PacketRepository interrupted;
  auto write = interrupted.io();
  write.requireCurrent = readOnly.requireCurrent;
  closed = false;
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  BOOST_CHECK_THROW(nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
    ndn::Name("/preparer/models/v=2"), "sha256:" + std::string(64, 'd'), "catalog", "manifest",
    [&] () -> std::optional<NativeStoredModelInput> {
      closed = true;
      return NativeStoredModelInput{"catalog", Bytes(100, 0x13)};
    }, write), std::runtime_error);
  BOOST_CHECK(interrupted.writes.empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
