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
#include <iterator>
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

// Test-owned private envelopes only: preserve the key and recompute the file checksum.
void
editJournal(const std::filesystem::path& path, const std::function<void(NativeJson&)>& edit)
{
  std::ifstream input(path, std::ios::binary);
  const Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const std::string magic = "NDNSF-DI-MODEL-PREPARATION-v1\n";
  BOOST_REQUIRE_GT(bytes.size(), magic.size() + 103);
  auto metadata = nativeParseJson(std::string(bytes.begin() + magic.size(), bytes.end() - 103));
  edit(metadata);
  const auto text = magic + nativeCanonicalJson(metadata);
  Bytes changed(text.begin(), text.end());
  changed.insert(changed.end(), bytes.end() - 103, bytes.end() - 71);
  const auto checksum = hash(changed.data(), changed.size());
  changed.insert(changed.end(), checksum.begin(), checksum.end());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(changed.data()), changed.size());
  BOOST_REQUIRE(output.good());
}

struct PacketRepository
{
  struct Group
  {
    std::string digest;
    std::uint64_t bytes = 0;
    std::vector<ndn::Name> names;
  };
  std::map<ndn::Name, Bytes> packets;
  std::map<ndn::Name, Group> groups;
  std::vector<ndn::Name> writes;
  std::vector<ndn::Name> reads;
  std::size_t sourceReads = 0;
  std::size_t confirmations = 0;
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
        BOOST_CHECK_LE(wires.size(), 4096);
        Bytes all;
        std::vector<ndn::Name> packetNames;
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
          packetNames.push_back(data.getName());
          writes.push_back(data.getName());
          all.insert(all.end(), wire.begin(), wire.end());
        }
        BOOST_CHECK_LE(names, 512U << 10);
        BOOST_CHECK_LE(all.size(), 32U << 20);
        BOOST_REQUIRE_EQUAL(hash(all.data(), all.size()), expected);
        // Re-read the retained original wires independently, as required by the commit port.
        Bytes retained;
        for (const auto& wire : wires) {
          const ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire))};
          const auto& stored = packets.at(data.getName());
          retained.insert(retained.end(), stored.begin(), stored.end());
        }
        BOOST_REQUIRE_EQUAL(hash(retained.data(), retained.size()), expected);
        const auto first = packetNames.front();
        const auto previous = groups.find(first);
        if (previous != groups.end() && (previous->second.digest != expected ||
            previous->second.bytes != all.size() || previous->second.names != packetNames))
          throw std::runtime_error("immutable group conflict");
        groups[first] = Group{expected, all.size(), std::move(packetNames)};
      };
      port.requireCommitted = [this](const ndn::Name& name, const std::string& expected,
                                     std::uint64_t bytes) {
        ++confirmations;
        const auto& group = groups.at(name);
        std::uint64_t measured = 0;
        for (const auto& member : group.names) measured += packets.at(member).size();
        if (group.digest != expected || group.bytes != bytes || measured != bytes)
          throw std::runtime_error("committed metadata mismatch");
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
  publish(const NativeStoredModelPacketIo* custom = nullptr,
          const std::filesystem::path& state = {},
          const ndn::Name& prefix = ndn::Name("/preparer/models/v=1"))
  {
    ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
    auto port = custom ? *custom : io();
    std::size_t next = 0;
    return nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
      prefix, "sha256:" + std::string(64, 'd'),
      "catalog", "manifest", [&] () -> std::optional<NativeStoredModelInput> {
        if (next == source.size())
          return std::nullopt;
        ++sourceReads;
        return source[next++];
      }, port, state);
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

BOOST_AUTO_TEST_CASE(LegacyPendingUploadKeepsItsCommittedGroupBoundaries)
{
  PrivateFile file;
  PacketRepository repository;
  repository.source.back().plaintext = Bytes(16U << 20, 0x73);
  const ndn::Name layer("/preparer/models/v=1/objects/layer0");
  auto port = repository.io();
  const auto commit = port.commit;
  port.commit = [&](auto wires, const auto& expected) {
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    if (layer.isPrefixOf(first.getName())) throw std::runtime_error("before layer commit");
    commit(std::move(wires), expected);
  };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  editJournal(file.path, [](NativeJson& value) {
    value["context"].erase("tailUploadGroupingVersion");
    value["current"].erase("uploadGroupingVersion");
    for (auto& row : value["objects"]) row.erase("uploadGroupingVersion");
  });
  port.commit = [&](auto wires, const auto& expected) {
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    commit(std::move(wires), expected);
    if (layer.isPrefixOf(first.getName())) throw std::runtime_error("lost legacy commit reply");
  };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  const auto original = repository.groups;
  const auto resumed = repository.publish(nullptr, file.path);
  std::size_t layerGroups = 0;
  for (const auto& [name, group] : repository.groups) {
    if (layer.isPrefixOf(name)) {
      ++layerGroups;
      BOOST_CHECK_LE(group.names.size(), 1024U);
      BOOST_CHECK_LE(group.bytes, 8U << 20);
    }
  }
  BOOST_CHECK_EQUAL(layerGroups, 3U);
  for (const auto& [name, group] : original)
    BOOST_CHECK(repository.groups.at(name).digest == group.digest);
  const auto catalog = nativeLoadStoredModelCatalog(resumed.handle, repository.io(false));
  BOOST_CHECK(nativeReadStoredModelObject(resumed.handle, catalog, "layer0", repository.io(false))
              == repository.source.back().plaintext);
}

BOOST_AUTO_TEST_CASE(LegacyTailAndNewPendingObjectResumeWithoutRegrouping)
{
  PrivateFile file;
  PacketRepository repository;
  const auto prefix = ndn::Name("/preparer").append(std::string(800, 'p'));
  for (unsigned i = 0; i < 1024; ++i)
    repository.source.push_back({"layer" + std::to_string(i + 1), Bytes{0x71}});
  auto port = repository.io();
  port.commit = [](auto, const auto&) { throw std::runtime_error("before first upload"); };
  BOOST_CHECK_EXCEPTION(repository.publish(&port, file.path, prefix), std::runtime_error,
    [](const auto& error) { return std::string(error.what()) == "before first upload"; });
  editJournal(file.path, [](NativeJson& value) {
    BOOST_REQUIRE(value.at("current").is_object());
    value["context"].erase("tailUploadGroupingVersion");
    value["current"].erase("uploadGroupingVersion");
  });
  const auto commit = repository.io().commit;
  const auto layer = ndn::Name(prefix).append("objects").append("layer0");
  repository.source[2].plaintext = Bytes(16U << 20, 0x73);
  port.commit = [&](auto wires, const auto& expected) {
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    commit(std::move(wires), expected);
    if (layer.isPrefixOf(first.getName())) throw std::runtime_error("lost new object reply");
  };
  BOOST_CHECK_EXCEPTION(repository.publish(&port, file.path, prefix), std::runtime_error,
    [](const auto& error) { return std::string(error.what()) == "lost new object reply"; });
  // The old journal must persist the new current object's policy across another restart.
  const auto index = ndn::Name(prefix).append("index");
  port.commit = [&](auto wires, const auto& expected) {
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    commit(std::move(wires), expected);
    if (index.isPrefixOf(first.getName())) throw std::runtime_error("lost old tail reply");
  };
  BOOST_CHECK_EXCEPTION(repository.publish(&port, file.path, prefix), std::runtime_error,
    [](const auto& error) { return std::string(error.what()) == "lost old tail reply"; });
  const auto writes = repository.writes.size();
  const auto resumed = repository.publish(nullptr, file.path, prefix);
  std::size_t indexGroups = 0;
  for (const auto& [name, group] : repository.groups) {
    if (index.isPrefixOf(name)) {
      ++indexGroups;
      std::size_t nameBytes = 0;
      for (const auto& member : group.names)
        nameBytes += member.toUri(ndn::name::UriFormat::CANONICAL).size();
      BOOST_CHECK_LE(nameBytes, 128U << 10);
    }
  }
  BOOST_CHECK_GT(indexGroups, 1U);
  for (std::size_t i = writes; i < repository.writes.size(); ++i)
    BOOST_CHECK(!ndn::Name(prefix).append("objects").isPrefixOf(repository.writes[i]));
  BOOST_CHECK_NO_THROW(nativeLoadStoredModelCatalog(resumed.handle, repository.io(false)));
}

BOOST_AUTO_TEST_CASE(PreparationRejectsUnknownGroupingPoliciesAndFieldsBeforeSourceRead)
{
  for (unsigned mode = 0; mode < 6; ++mode) {
    PrivateFile file;
    PacketRepository repository;
    auto port = repository.io();
    port.commit = [](auto, const auto&) { throw std::runtime_error("before first upload"); };
    BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
    editJournal(file.path, [mode](NativeJson& value) {
      if (mode == 0) value["context"]["tailUploadGroupingVersion"] = 3;
      if (mode == 1) value["current"]["uploadGroupingVersion"] = 3;
      if (mode == 2) value["context"]["tailUploadGroupingVersion"] = 2.0;
      if (mode == 3) value["current"]["uploadGroupingVersion"] = "2";
      if (mode == 4) value["context"]["unknown"] = 1;
      if (mode == 5) value["current"]["unknown"] = 1;
    });
    const auto reads = repository.sourceReads;
    BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
    BOOST_CHECK_EQUAL(repository.sourceReads, reads);
    BOOST_CHECK(repository.writes.empty());
  }
}

BOOST_AUTO_TEST_CASE(ReadyPreparationRejectsChangedCompletedGroupingPolicy)
{
  PrivateFile file;
  PacketRepository repository;
  repository.publish(nullptr, file.path);
  editJournal(file.path, [](NativeJson& value) {
    value["objects"]["catalog"]["uploadGroupingVersion"] = 3;
  });
  const auto reads = repository.sourceReads;
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK_EQUAL(repository.sourceReads, reads);
}

BOOST_AUTO_TEST_CASE(LargeObjectUsesOneBoundedUploadButKeepsSmallFetchWindows)
{
  PacketRepository repository;
  repository.source.back().plaintext = Bytes(16U << 20, 0x73);
  const auto publication = repository.publish();
  const ndn::Name objectName("/preparer/models/v=1/objects/layer0");
  std::size_t uploads = 0;
  for (const auto& [name, group] : repository.groups) {
    if (objectName.isPrefixOf(name)) {
      ++uploads;
      BOOST_CHECK_GT(group.names.size(), 1024U);
    }
  }
  BOOST_CHECK_EQUAL(uploads, 1U);
  auto port = repository.io(false);
  const auto fetch = port.fetch;
  port.fetch = [fetch] (const std::vector<ndn::Name>& names) {
    BOOST_CHECK_LE(names.size(), 1024U);
    auto wires = fetch(names);
    std::size_t bytes = 0;
    for (const auto& wire : wires) bytes += wire.size();
    BOOST_CHECK_LE(bytes, 8U << 20);
    return wires;
  };
  const auto catalog = nativeLoadStoredModelCatalog(publication.handle, port);
  const auto recovered = nativeReadStoredModelObject(publication.handle, catalog, "layer0", port);
  BOOST_CHECK(recovered == repository.source.back().plaintext);
}

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
  ndn::KeyChain planKeys{"pib-memory:", "tpm-memory:"};
  std::vector<std::pair<std::string, std::uint64_t>> plan;
  for (const auto& input : repository.source) plan.emplace_back(input.id, input.plaintext.size());
  const auto wireBound = nativePreflightStoredModelPublication(planKeys, ndn::Name("/preparer"),
    ndn::Name("/preparer/models/v=1"), "sha256:" + std::string(64, 'd'),
    "catalog", "manifest", plan);
  const auto published = repository.publish();
  BOOST_CHECK_EQUAL(published.objectCount, 3);
  BOOST_CHECK_EQUAL(published.sourceBytes, 20007);
  BOOST_CHECK_EQUAL(published.packetGroups, 5);
  std::uint64_t retainedBytes = 0;
  for (const auto& [name, wire] : repository.packets)
    retainedBytes += wire.size();
  BOOST_CHECK_EQUAL(published.wireBytes, retainedBytes);
  BOOST_CHECK_LE(published.wireBytes, wireBound);
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

BOOST_AUTO_TEST_CASE(PreflightRejectsAllowanceAndIdentityBeforePayloadAccess)
{
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  const auto check = [&](const std::vector<std::pair<std::string, std::uint64_t>>& plan) {
    return nativePreflightStoredModelPublication(keys, ndn::Name("/preparer"),
      ndn::Name("/preparer/models/v=1"), "sha256:" + std::string(64, 'd'),
      "catalog", "manifest", plan);
  };
  BOOST_CHECK_THROW(check({{"catalog", 3}, {"manifest", 4}, {"catalog", 1}}), std::exception);
  BOOST_CHECK_THROW(check({{"catalog", 3}}), std::exception);
  std::vector<std::pair<std::string, std::uint64_t>> oversized{{"catalog", 3}, {"manifest", 4}};
  for (std::size_t i = 0; i < 100; ++i)
    oversized.emplace_back("weight-" + std::to_string(i), 1ULL << 30);
  BOOST_CHECK_THROW(check(oversized), std::exception);
  std::vector<std::pair<std::string, std::uint64_t>> many{{"catalog", 1}, {"manifest", 1}};
  for (std::size_t i = 0; i < 4095; ++i)
    many.emplace_back("weight-" + std::to_string(i), 1);
  BOOST_CHECK_THROW(check(many), std::exception);
  many.pop_back();
  BOOST_CHECK_NO_THROW(check(many));
  const auto longPrefix = ndn::Name("/preparer").append(std::string(3850, 'x'));
  BOOST_REQUIRE_LE(longPrefix.toUri(ndn::name::UriFormat::CANONICAL).size(),
                   (ndn::MAX_NDN_PACKET_SIZE - 1024) / 2);
  BOOST_CHECK_THROW(nativePreflightStoredModelPublication(keys, ndn::Name("/preparer"),
    longPrefix, "sha256:" + std::string(64, 'd'), "catalog", "manifest", many), std::exception);
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

BOOST_AUTO_TEST_CASE(PreparationResumesAmbiguousObjectWithIdenticalCiphertextAndKey)
{
  PrivateFile file;
  PacketRepository repository;
  auto port = repository.io();
  const auto commit = port.commit;
  port.commit = [&](NativeStoredModelPacketIo::PacketWires wires, const std::string& expected) {
    const auto first = ndn::Data(
      ndn::Block(ndn::span<const std::uint8_t>(wires.front()))).getName();
    commit(std::move(wires), expected);
    if (ndn::Name("/preparer/models/v=1/objects/layer0").isPrefixOf(first))
      throw std::runtime_error("lost commit reply");
  };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  BOOST_CHECK(std::filesystem::exists(file.path.string() + ".object"));
  BOOST_CHECK(repository.packets.count(ndn::Name("/preparer/models/v=1/root")) == 0);
  const auto original = repository.packets;
  std::ifstream state(file.path, std::ios::binary);
  const Bytes privateState{std::istreambuf_iterator<char>(state), std::istreambuf_iterator<char>()};
  BOOST_REQUIRE_GT(privateState.size(), 103U);
  const Bytes originalKey(privateState.end() - 103, privateState.end() - 71);
  const auto resumed = repository.publish(nullptr, file.path);
  const bool sameMaster = resumed.handle.masterKey == originalKey;
  BOOST_CHECK(sameMaster);
  for (const auto& [name, wire] : original) BOOST_CHECK(repository.packets.at(name) == wire);
  BOOST_CHECK(!std::filesystem::exists(file.path.string() + ".object"));
  BOOST_CHECK_GT(repository.confirmations, 0U);
  const auto catalog = nativeLoadStoredModelCatalog(resumed.handle, repository.io(false));
  const auto recovered = nativeReadStoredModelObject(resumed.handle, catalog, "layer0",
                                                      repository.io(false));
  BOOST_CHECK(recovered == repository.source.back().plaintext);
  const auto writes = repository.writes.size();
  repository.source.clear();
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  std::size_t sourceCalls = 0;
  auto readyIo = repository.io();
  readyIo.commit = [](auto, const auto&) { throw std::runtime_error("unexpected publication"); };
  const auto ready = nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
    ndn::Name("/preparer/models/v=1"), "sha256:" + std::string(64, 'd'), "catalog", "manifest",
    [&]() -> std::optional<NativeStoredModelInput> {
      ++sourceCalls;
      throw std::runtime_error("source is offline");
    }, readyIo, file.path);
  BOOST_CHECK_EQUAL(sourceCalls, 0U);
  BOOST_CHECK_EQUAL(ready.sourceBytes, 0U);
  BOOST_CHECK_EQUAL(ready.wireBytes, 0U);
  BOOST_CHECK_EQUAL(ready.objectCount, 3U);
  BOOST_CHECK(ready.handle.rootWireDigest == resumed.handle.rootWireDigest);
  BOOST_CHECK_EQUAL(repository.writes.size(), writes);
}

BOOST_AUTO_TEST_CASE(PreparationResolvesPrefixUnderItsExistingOwner)
{
  PrivateFile state;
  PacketRepository repository;
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  ndn::Name resolved;
  std::size_t bindings = 0;
  std::size_t sourceCalls = 0;
  const auto publish = [&](const ndn::Name& prefix, const std::string& catalogId,
                           const std::function<void(const ndn::Name&)>& bind) {
    std::size_t next = 0;
    return nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"), prefix,
      "sha256:" + std::string(64, 'd'), catalogId, "manifest",
      [&]() -> std::optional<NativeStoredModelInput> {
        BOOST_REQUIRE(!resolved.empty());
        ++sourceCalls;
        if (next == repository.source.size()) return std::nullopt;
        return repository.source[next++];
      }, repository.io(), state.path, bind);
  };
  const auto bind = [&](const ndn::Name& prefix) {
    resolved = prefix;
    ++bindings;
    BOOST_CHECK(ndn::Name("/preparer").isPrefixOf(prefix));
    BOOST_CHECK_THROW(publish({}, "catalog", {}), std::runtime_error); // same owner is locked
  };
  const auto first = publish({}, "catalog", bind);
  BOOST_REQUIRE_EQUAL(bindings, 1);
  BOOST_CHECK(first.handle.rootName == ndn::Name(resolved).append("root"));
  const auto oldCalls = sourceCalls;
  const auto writes = repository.writes.size();
  const auto ready = publish({}, "catalog", bind);
  BOOST_CHECK(ready.handle.rootName == first.handle.rootName);
  BOOST_CHECK(ready.handle.masterKey == first.handle.masterKey);
  BOOST_CHECK_EQUAL(ready.handle.rootWireDigest, first.handle.rootWireDigest);
  BOOST_CHECK_EQUAL(bindings, 1);
  BOOST_CHECK_EQUAL(sourceCalls, oldCalls);
  BOOST_CHECK_EQUAL(repository.writes.size(), writes);
  BOOST_CHECK_THROW(publish({}, "changed-catalog", bind), std::runtime_error);
  BOOST_CHECK_EQUAL(bindings, 1);
  BOOST_CHECK_EQUAL(sourceCalls, oldCalls);
  const auto explicitReady = publish(resolved, "catalog", bind);
  BOOST_CHECK(explicitReady.handle.masterKey == first.handle.masterKey);
}

BOOST_AUTO_TEST_CASE(PreparationKeepsResolvedPrefixAfterSourceBindingFailure)
{
  PrivateFile state;
  PacketRepository repository;
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  ndn::Name allocated;
  const auto publish = [&](bool failBinding) {
    std::size_t next = 0;
    return nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"), {},
      "sha256:" + std::string(64, 'd'), "catalog", "manifest",
      [&]() -> std::optional<NativeStoredModelInput> {
        ++repository.sourceReads;
        if (next == repository.source.size()) return std::nullopt;
        return repository.source[next++];
      }, repository.io(), state.path, [&](const ndn::Name& prefix) {
        if (allocated.empty()) allocated = prefix;
        BOOST_CHECK(prefix == allocated);
        if (failBinding) throw std::runtime_error("source preflight failed");
      });
  };
  BOOST_CHECK_THROW(publish(true), std::runtime_error);
  BOOST_CHECK(repository.writes.empty());
  BOOST_CHECK_EQUAL(repository.sourceReads, 0);
  const auto resumed = publish(false);
  BOOST_CHECK(resumed.handle.rootName == ndn::Name(allocated).append("root"));
  BOOST_CHECK(!repository.writes.empty());
}

BOOST_AUTO_TEST_CASE(PreparationResumesRootReplyLossWithoutRepublishingCompletedObjects)
{
  PrivateFile file;
  PacketRepository repository;
  auto port = repository.io();
  const auto commit = port.commit;
  port.commit = [&](NativeStoredModelPacketIo::PacketWires wires, const std::string& expected) {
    const auto first = ndn::Data(
      ndn::Block(ndn::span<const std::uint8_t>(wires.front()))).getName();
    commit(std::move(wires), expected);
    if (first == ndn::Name("/preparer/models/v=1/root"))
      throw std::runtime_error("lost root reply");
  };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  const auto root = repository.packets.at(ndn::Name("/preparer/models/v=1/root"));
  const auto previousWrites = repository.writes.size();
  const auto resumed = repository.publish(nullptr, file.path);
  BOOST_CHECK(repository.packets.at(resumed.handle.rootName) == root);
  for (std::size_t i = previousWrites; i < repository.writes.size(); ++i)
    BOOST_CHECK(!ndn::Name("/preparer/models/v=1/objects").isPrefixOf(repository.writes[i]));
  const auto sourceReads = repository.sourceReads;
  repository.groups.erase(resumed.handle.rootName);
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::exception);
  BOOST_CHECK_EQUAL(repository.sourceReads, sourceReads);
}

BOOST_AUTO_TEST_CASE(PreparationRejectsChangedSourcePrivateStateAndMissingSpool)
{
  PrivateFile file;
  PacketRepository repository;
  auto port = repository.io();
  port.commit = [](auto, const auto&) { throw std::runtime_error("interrupted storage"); };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  BOOST_CHECK(repository.writes.empty());
  const auto sourceReads = repository.sourceReads;
  BOOST_REQUIRE_EQUAL(::chmod(file.path.c_str(), 0644), 0);
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK_EQUAL(repository.sourceReads, sourceReads);
  BOOST_REQUIRE_EQUAL(::chmod(file.path.c_str(), 0600), 0);
  repository.source.front().plaintext.push_back(1);
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  repository.source.front().plaintext.pop_back();
  BOOST_CHECK(repository.writes.empty());
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  BOOST_CHECK_THROW(nativePublishStoredModel(keys, ndn::Name("/preparer"), ndn::Name("/repo"),
    ndn::Name("/preparer/models/v=1"), "sha256:" + std::string(64, 'e'), "catalog", "manifest",
    []() -> std::optional<NativeStoredModelInput> {
      throw std::runtime_error("wrong context must not read source");
    }, repository.io(), file.path), std::runtime_error);
  BOOST_CHECK(repository.writes.empty());
  const auto stage = file.path.string() + ".object";
  {
    std::fstream cipher(stage, std::ios::in | std::ios::out | std::ios::binary);
    BOOST_REQUIRE(cipher);
    char byte = 0;
    cipher.read(&byte, 1);
    byte ^= 1;
    cipher.seekp(0);
    cipher.write(&byte, 1);
  }
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK(repository.writes.empty());
  BOOST_REQUIRE(std::filesystem::remove(stage));
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK(repository.writes.empty());
  {
    std::fstream state(file.path, std::ios::in | std::ios::out | std::ios::binary);
    BOOST_REQUIRE(state);
    state.seekp(-1, std::ios::end);
    state.put('!');
  }
  const auto before = repository.sourceReads;
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK_EQUAL(repository.sourceReads, before);
}

BOOST_AUTO_TEST_CASE(PreparationOwnsItsWriterLockAndRecoversBeforeJournalCommit)
{
  {
    PrivateFile file;
    PacketRepository repository;
    auto port = repository.io();
    bool attempted = false;
    port.requireCurrent = [&] {
      if (!attempted && std::filesystem::exists(file.path)) {
        attempted = true;
        BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
      }
    };
    (void)repository.publish(&port, file.path);
    BOOST_CHECK(attempted);
    BOOST_CHECK_EQUAL(repository.sourceReads, 3U);
  }
  {
    PrivateFile file;
    PacketRepository repository;
    auto port = repository.io();
    unsigned stageChecks = 0;
    port.requireCurrent = [&] {
      if (std::filesystem::exists(file.path.string() + ".object") && ++stageChecks == 2)
        throw std::runtime_error("interrupted before journal current commit");
    };
    BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
    BOOST_CHECK(repository.packets.empty());
    BOOST_CHECK(std::filesystem::exists(file.path.string() + ".object"));
    const auto resumed = repository.publish(nullptr, file.path);
    BOOST_CHECK_EQUAL(resumed.objectCount, 3U);
    BOOST_CHECK(!std::filesystem::exists(file.path.string() + ".object"));
  }
}

BOOST_AUTO_TEST_CASE(PreparationRejectsSymlinkStateAndLockBeforeSourceReads)
{
  PrivateFile file;
  PacketRepository repository;
  auto port = repository.io();
  port.commit = [](auto, const auto&) { throw std::runtime_error("interrupted storage"); };
  BOOST_CHECK_THROW(repository.publish(&port, file.path), std::runtime_error);
  const auto sourceReads = repository.sourceReads;
  const auto backup = file.directory / "original.state";
  std::filesystem::rename(file.path, backup);
  std::filesystem::create_symlink(backup, file.path);
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK_EQUAL(repository.sourceReads, sourceReads);
  BOOST_REQUIRE(std::filesystem::remove(file.path));
  std::filesystem::rename(backup, file.path);
  const auto lock = file.path.string() + ".lock";
  const auto oldLock = file.directory / "original.lock";
  std::filesystem::rename(lock, oldLock);
  std::filesystem::create_symlink(oldLock, lock);
  BOOST_CHECK_THROW(repository.publish(nullptr, file.path), std::runtime_error);
  BOOST_CHECK_EQUAL(repository.sourceReads, sourceReads);
  BOOST_CHECK(repository.writes.empty());
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
