#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoPacketServer.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"
#include "ndn-service-framework/ImmutableFilePublication.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <atomic>

namespace {
using namespace ndnsf_distributed_repo;

std::atomic<size_t> catalogParses{0};
void countCatalogParse() { ++catalogParses; }
struct CatalogParseScope
{
  detail::FilesystemRepoStoreIoHooks previous;
  CatalogParseScope()
    : previous(detail::installFilesystemRepoStoreIoHooks({nullptr, nullptr, countCatalogParse}))
  { catalogParses = 0; }
  ~CatalogParseScope() { detail::installFilesystemRepoStoreIoHooks(previous); }
};

struct Fixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec201-packet-index-" + std::to_string(::getpid()));
  Fixture() { std::filesystem::create_directories(root); }
  ~Fixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove(root.string() + ".authority.lock", error);
  }
  StorageCapability capability() const
  {
    StorageCapability value;
    value.repoNode = "/spec201/repo";
    value.freeBytes = 1U << 20;
    return value;
  }
  std::shared_ptr<RepoStoreBackend> store(const char* replica) const
  {
    return makeFilesystemRepoStore((root / replica).string(), 1U << 20, 1U << 20);
  }
};

// Count the actual filesystem catalog enumerations without changing its I/O.
struct CountingStore : RepoStoreBackend
{
  explicit CountingStore(std::shared_ptr<RepoStoreBackend> store) : delegate(std::move(store)) {}
  std::shared_ptr<RepoStoreBackend> delegate;
  mutable size_t enumerations = 0;
  void put(const RepoObjectManifest& m, std::vector<uint8_t> p) override { delegate->put(m, std::move(p)); }
  void putManifest(const RepoObjectManifest& m) override { delegate->putManifest(m); }
  StoredObject get(const std::string& n) const override { return delegate->get(n); }
  bool has(const std::string& n) const override { return delegate->has(n); }
  bool erase(const std::string& n) override { return delegate->erase(n); }
  size_t size() const override { return delegate->size(); }
  uint64_t usedBytes() const override { return delegate->usedBytes(); }
  std::vector<RepoObjectManifest> listManifests() const override
  {
    ++enumerations;
    return delegate->listManifests();
  }
  void putRange(const RepoObjectManifest& m, RepoByteRange r, const std::vector<uint8_t>& p) override
  { delegate->putRange(m, r, p); }
  void commitRanges(const RepoObjectManifest& m) override { delegate->commitRanges(m); }
  std::vector<uint8_t> getRange(const std::string& n, RepoByteRange r) const override
  { return delegate->getRange(n, r); }
  RepoObjectManifest getManifest(const std::string& n) const override { return delegate->getManifest(n); }
  bool supportsRange() const noexcept override { return delegate->supportsRange(); }
  bool supportsManifestLookup() const noexcept override { return delegate->supportsManifestLookup(); }
  void abortRanges(const std::string& n) override { delegate->abortRanges(n); }
};

std::vector<uint8_t>
packet(const ndn::Name& name, uint8_t content = 42, size_t contentBytes = 1)
{
  ndn::Data data(name);
  data.setContent(std::vector<uint8_t>(contentBytes, content));
  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  keyChain.sign(data, ndn::security::signingWithSha256());
  const auto& wire = data.wireEncode();
  return {wire.begin(), wire.end()};
}

ndn::Name
packetName()
{
  ndn::Name name("/actual/producer/NDNSF/BLOB");
  const std::vector<uint8_t> identity(32, 0x81);
  name.append(ndn::name::Component(ndn::span<const uint8_t>(identity.data(), identity.size())));
  return name.append("DATA").appendSegment(0);
}

struct PacketSet
{
  ndn_service_framework::ProtectedBlobManifest manifest;
  ndn_service_framework::EncryptedLargeDataCommitOptions options;
  std::vector<std::pair<ndn::Name, std::vector<uint8_t>>> packets;
  explicit PacketSet(const std::string& producer = "/actual/packet-set-producer")
  {
    using namespace ndn_service_framework;
    manifest.producer = ndn::Name(producer);
    PublishedContentName semantics;
    semantics.kind = ContentDataKind::ModelFile;
    semantics.label = "canonical-source";
    semantics.modelName = "model/full";
    semantics.modelIdentity = std::string(64, 'b');
    manifest.contentName = semantics;
    manifest.plaintextDigest.assign(32, 0x2a);
    manifest.plaintextBytes = 19000;
    manifest.envelopeBytes = 20000;
    manifest.segmentBytes = 4000;
    manifest.segmentCount = 5;
    manifest.representationSpec = "{\"encoding\":\"test\",\"segmentBytes\":4000}";
    manifest.protectionSpec = "{\"epoch\":\"1\",\"keyReference\":\"test\"}";
    manifest.authorizationScope = "/SERVICE/test";
    manifest.protectionEpoch = "1";
    manifest.keyReferenceId = "test";
    manifest.keyIdentifier = "test-key";
    const auto protection = computeNamingDigest("ndnsf-naming/protection",
      {{1, 5, std::vector<uint8_t>(manifest.representationSpec.begin(), manifest.representationSpec.end())},
       {2, 5, std::vector<uint8_t>(manifest.protectionSpec.begin(), manifest.protectionSpec.end())}});
    std::string plaintextHash;
    for (unsigned i = 0; i < 32; ++i) plaintextHash += "2a";
    manifest.dataPrefix = semantics.contentPrefix(manifest.producer, plaintextHash)
      .append("PROTECTED").append(makeNamingDigestComponent(protection)).append("DATA");
    ndn::KeyChain keys("pib-memory:", "tpm-memory:");
    const auto append = [&](const ndn::Name& name, const std::vector<uint8_t>& payload, bool segment) {
      ndn::Data data(name);
      data.setContent(payload);
      if (segment) data.setFinalBlock(ndn::name::Component::fromSegment(4));
      keys.sign(data, ndn::security::signingWithSha256());
      const auto& wire = data.wireEncode();
      packets.emplace_back(name, std::vector<uint8_t>(wire.begin(), wire.end()));
    };
    const auto payload = manifest.encode();
    append(manifest.getName(), payload, false);
    for (uint64_t i = 0; i < 5; ++i)
      append(ndn::Name(manifest.dataPrefix).appendSegment(i), std::vector<uint8_t>(4000, uint8_t(i)), true);
    options.retention = EncryptedLargeDataRetention::Durable;
    options.packetStorageFormat = "original-packet-set";
    options.manifestName = manifest.getName().toUri(ndn::name::UriFormat::CANONICAL);
    options.dataPrefix = manifest.dataPrefix.toUri(ndn::name::UriFormat::CANONICAL);
    options.publicationIdentity = "sha256:" + sha256Hex(toBytes(producer));
    options.protectionEpoch = "1";
    options.keyReferenceId = "test";
    options.keyReferenceVersion = "1";
    options.ciphertextManifestDigest = "sha256:" + sha256Hex(payload);
    options.servingLocator = producer;
    options.contentDigest = "sha256:";
    for (unsigned i = 0; i < 32; ++i) options.contentDigest += "2a";
    options.plaintextSize = manifest.plaintextBytes;
    for (const auto& item : packets) options.packetWireBytes += item.second.size();
  }
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec201TransparentRepo)

BOOST_AUTO_TEST_CASE(PacketIdentitySurvivesUriAliasesTwoReplicasAndRestart)
{
  Fixture fixture;
  const auto name = packetName();
  const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
  BOOST_REQUIRE_NE(canonical, alternate);
  const auto wire = packet(name);
  {
    RepoNode first(ndn::Name("/repo/one"), fixture.capability(), fixture.store("one"));
    RepoNode second(ndn::Name("/repo/two"), fixture.capability(), fixture.store("two"));
    const auto firstCommit = first.putDataPacket(alternate, wire);
    BOOST_CHECK_EQUAL(firstCommit.objectName, canonical);
    BOOST_CHECK(firstCommit.dataReceiptId.empty());
    BOOST_CHECK_EQUAL(RepoClient::putDataPacket(second, wire).objectName, canonical);
    BOOST_CHECK(first.hasDataPacket(canonical));
    BOOST_CHECK(first.hasDataPacket(alternate));
    BOOST_CHECK(first.getDataPacket(alternate) == wire);
    BOOST_CHECK(second.getDataPacket(canonical) == wire);
    BOOST_CHECK_EQUAL(first.putDataPacket(canonical, wire).objectName, canonical);
    BOOST_CHECK_EQUAL(first.core().list().size(), 1U);
    BOOST_CHECK(first.core().get(alternate) == wire);
    BOOST_CHECK(first.core().has(alternate));
    BOOST_CHECK_EQUAL(first.core().getManifest(alternate).objectName, canonical);
    BOOST_CHECK_EQUAL(first.core().catalogLookup(alternate).manifest.objectName, canonical);
  }
  {
    RepoCore reopened(fixture.capability(), fixture.store("one"));
    BOOST_CHECK(reopened.getDataPacket(alternate) == wire);
    BOOST_CHECK(reopened.remove(alternate));
    BOOST_CHECK(!reopened.has(canonical));
    BOOST_CHECK(!reopened.hasDataPacket(alternate));
    RepoCore second(fixture.capability(), fixture.store("two"));
    BOOST_CHECK(second.getDataPacket(alternate) == wire);
  }
}

BOOST_AUTO_TEST_CASE(ConfiguredPacketStoreKeepsCompleteReceiptOwner)
{
  Fixture fixture;
  const auto name = packetName();
  const auto wire = packet(name);
  RepoNode repo(ndn::Name("/repo/service"), fixture.capability(), fixture.store("owned"),
                fixture.capability().repoNode, "/repo/provider");
  const auto commit = repo.putDataPacket(name.toUri(ndn::name::UriFormat::ALTERNATE), wire);
  BOOST_CHECK(!commit.dataReceiptId.empty());
  BOOST_CHECK_EQUAL(commit.dataReceiptRepoNode, fixture.capability().repoNode);
  BOOST_CHECK_EQUAL(commit.dataReceiptProviderIdentity, "/repo/provider");
  BOOST_CHECK(repo.getDataPacket(name.toUri(ndn::name::UriFormat::CANONICAL)) == wire);
}

BOOST_AUTO_TEST_CASE(ConflictsAndForeignNamesPreserveCommittedWire)
{
  Fixture fixture;
  RepoCore repo(fixture.capability(), fixture.store("conflict"));
  const auto name = packetName();
  const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
  const auto wire = packet(name);
  repo.putDataPacket(alternate, wire);
  BOOST_CHECK_THROW(repo.putDataPacket(alternate, packet(name, 43)), std::runtime_error);
  BOOST_CHECK_THROW(repo.putDataPacket("/different/producer/DATA", wire), std::invalid_argument);
  BOOST_CHECK(repo.getDataPacket(canonical) == wire);
  BOOST_CHECK_EQUAL(repo.list().size(), 1U);
}

BOOST_AUTO_TEST_CASE(LegacyEnvelopeIsNotAnOriginalPacketOrAnAlias)
{
  Fixture fixture;
  RepoCore repo(fixture.capability(), fixture.store("legacy"));
  const auto name = packetName();
  const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
  repo.put(canonical, {1, 2, 3}, "encrypted-large-data-envelope");
  BOOST_CHECK(!repo.hasDataPacket(canonical));
  BOOST_CHECK(!repo.has(alternate));
  BOOST_CHECK_THROW(repo.getDataPacket(canonical), std::runtime_error);
  BOOST_CHECK_THROW(repo.putDataPacket(alternate, packet(name)), std::runtime_error);
  BOOST_CHECK(repo.get(canonical) == std::vector<uint8_t>({1, 2, 3}));
}

BOOST_AUTO_TEST_CASE(ServingCallbackPreservesOriginalWireAndChecksImplicitDigest)
{
  Fixture fixture;
  auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("serving"));
  PacketSet set;
  const auto name = set.packets.front().first;
  const auto wire = set.packets.front().second;
  repo->putDataPacket(name.toUri(), wire);
  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain, {true, true});
  RepoPacketServer server(face, repo);
  bool ready = false;
  bool failed = false;
  server.servePrefix(set.manifest.producer, [&] { ready = true; },
                     [&](const std::string&) { failed = true; });
  BOOST_CHECK(!ready);
  face.processEvents(ndn::time::milliseconds(10));
  BOOST_REQUIRE(ready);
  BOOST_CHECK(!failed);
  face.receive(ndn::Interest(name));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  const auto& served = face.sentData.back().wireEncode();
  BOOST_CHECK(std::vector<uint8_t>(served.begin(), served.end()) == wire);
  const ndn::Data original(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
  face.receive(ndn::Interest(original.getFullName()));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), 2U);
  auto wrong = original.getFullName();
  wrong = wrong.getPrefix(-1);
  const std::vector<uint8_t> digest(32, 0);
  wrong.appendImplicitSha256Digest(ndn::span<const uint8_t>(digest.data(), digest.size()));
  face.receive(ndn::Interest(wrong));
  face.receive(ndn::Interest(ndn::Name(set.manifest.producer).append("unknown")));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), 2U);
  server.close();
  face.receive(ndn::Interest(name));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), 2U);
}

BOOST_AUTO_TEST_CASE(PrefixDiscoveryReturnsOriginalSegmentWithoutResigning)
{
  Fixture fixture;
  auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("prefix-serving"));
  PacketSet set;
  const auto& name = set.packets[1].first;
  const auto& wire = set.packets[1].second;
  repo->putDataPacket(name.toUri(), wire);
  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain, {true, true});
  RepoPacketServer server(face, repo);
  bool ready = false;
  server.servePrefix(name.getPrefix(-1), [&] { ready = true; },
                     [](const std::string&) { BOOST_FAIL("prefix registration failed"); });
  face.processEvents(ndn::time::milliseconds(10));
  BOOST_REQUIRE(ready);
  ndn::Interest discovery(name.getPrefix(-1));
  discovery.setCanBePrefix(true);
  face.receive(discovery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  const auto& original = face.sentData.back().wireEncode();
  BOOST_CHECK(std::vector<uint8_t>(original.begin(), original.end()) == wire);
  ndn::Interest exactOnly(name.getPrefix(-1));
  exactOnly.setCanBePrefix(false);
  face.receive(exactOnly);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), 1U);
}

BOOST_AUTO_TEST_CASE(ClosedServerAndUncommittedPrefixesCannotBecomeReady)
{
  Fixture fixture;
  auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("closing"));
  repo->putDataPacket(packetName().toUri(), packet(packetName()));
  ndn::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain, {true, true});
  bool ready = false;
  {
    RepoPacketServer server(face, repo);
    BOOST_CHECK_THROW(server.servePrefix(ndn::Name(), {}, {}), std::invalid_argument);
    BOOST_CHECK_THROW(server.servePrefix(ndn::Name("/uncommitted"), {}, {}),
                      std::invalid_argument);
    server.servePrefix(ndn::Name("/actual/producer"), [&] { ready = true; }, {});
    server.close();
    BOOST_CHECK_THROW(server.servePrefix(ndn::Name("/actual/producer"), {}, {}),
                      std::invalid_argument);
  }
  face.processEvents(ndn::time::milliseconds(10));
  BOOST_CHECK(!ready);
  face.receive(ndn::Interest(packetName()));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());
}

struct AdmissionFile
{
  RepoObjectManifest manifest;
  std::filesystem::path path;
  std::vector<std::pair<ndn::Name, std::vector<uint8_t>>> packets;
  AdmissionFile(const Fixture& fixture, const std::string& label, size_t count,
                size_t contentBytes = 1, size_t firstSegment = 0)
    : path(fixture.root / (label + ".bin"))
  {
    manifest.objectType = "original-packet-set";
    manifest.operationId = label;
    manifest.packetOffsets = {0};
    std::vector<uint8_t> archive;
    for (size_t i = 0; i < count; ++i) {
      auto name = ndn::Name("/actual/producer/admission").append(label).appendSegment(i + firstSegment);
      auto wire = packet(name, 42, contentBytes);
      manifest.packetNames.push_back(name.toUri(ndn::name::UriFormat::CANONICAL));
      archive.insert(archive.end(), wire.begin(), wire.end());
      manifest.packetOffsets.push_back(archive.size());
      packets.emplace_back(std::move(name), std::move(wire));
    }
    manifest.objectName = manifest.packetNames.front();
    manifest.size = archive.size();
    manifest.sha256 = sha256Hex(archive);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(archive.data()), archive.size());
  }
};

BOOST_AUTO_TEST_CASE(PacketFileAdmissionBoundsCatalogScans)
{
  Fixture fixture;
  auto store = std::make_shared<CountingStore>(fixture.store("admission"));
  RepoCore repo(fixture.capability(), store);
  repo.putDataPacket(packetName().toUri(), packet(packetName()));
  AdmissionFile incoming(fixture, "batch", 32);
  store->enumerations = 0;
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
  BOOST_CHECK_GT(store->enumerations, 0U);
  BOOST_CHECK_LE(store->enumerations, 8U);
  for (const auto& item : incoming.packets)
    BOOST_CHECK(repo.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(PacketFileAdmissionRejectsExternalCatalogChange)
{
  Fixture fixture;
  auto store = fixture.store("external-admission");
  RepoCore repo(fixture.capability(), store);
  const auto original = packet(packetName());
  repo.putDataPacket(packetName().toUri(), original);
  AdmissionFile incoming(fixture, "external", 32);
  size_t callbacks = 0;
  bool changed = false;
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path, [&] {
    if (++callbacks == 2) {
      auto metadata = store->getManifest(packetName().toUri(ndn::name::UriFormat::CANONICAL));
      metadata.policyEpoch = "external-metadata-change";
      ++metadata.generation;
      store->put(metadata, original);
      changed = true;
    }
  }), std::runtime_error, [](const std::runtime_error& error) {
    return std::string(error.what()) == "repo-original-packet-catalog-changed";
  });
  BOOST_CHECK(changed);
  BOOST_CHECK(!store->has(incoming.manifest.objectName));
  BOOST_CHECK(repo.getDataPacket(packetName().toUri()) == original);
}

BOOST_AUTO_TEST_CASE(PacketFileCatalogScansDoNotGrowWithRangeWindows)
{
  Fixture fixture;
  auto capability = fixture.capability();
  capability.freeBytes = 8U << 20;
  auto store = std::make_shared<CountingStore>(fixture.store("multi-window"));
  RepoCore repo(capability, store);
  AdmissionFile small(fixture, "one-window", 32);
  store->enumerations = 0;
  repo.commitOriginalPacketFile(small.manifest, small.path);
  const auto oneWindowScans = store->enumerations;
  AdmissionFile large(fixture, "multi-window", 800, 4000);
  BOOST_REQUIRE_GT(large.manifest.size, 3U << 20);
  store->enumerations = 0;
  repo.commitOriginalPacketFile(large.manifest, large.path);
  BOOST_CHECK_EQUAL(store->enumerations, oneWindowScans);
  BOOST_CHECK_LE(store->enumerations, 8U);
  for (const auto& item : large.packets)
    BOOST_CHECK(repo.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(PacketFileAdmittedRangesKeepCollisionAndCatalogGuards)
{
  Fixture fixture;
  auto capability = fixture.capability();
  capability.freeBytes = 8U << 20;
  auto store = fixture.store("guarded-windows");
  RepoCore repo(capability, store);
  AdmissionFile existing(fixture, "indexed-collision", 2);
  repo.commitOriginalPacketFile(existing.manifest, existing.path);
  AdmissionFile collision(fixture, "indexed-collision", 1, 1, 1);
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(collision.manifest, collision.path),
    std::runtime_error, [](const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-is-immutable";
    });
  BOOST_CHECK(!store->has(collision.manifest.objectName));
  BOOST_CHECK_THROW(repo.putRange(collision.manifest, {0, 0}, {}), std::invalid_argument);
  auto genericRange = collision.manifest;
  genericRange.objectType = "blob";
  BOOST_CHECK_EXCEPTION(repo.putRange(genericRange, {0, 0}, {}),
    std::runtime_error, [](const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-is-immutable";
    });

  const auto original = packet(packetName());
  repo.putDataPacket(packetName().toUri(), original);
  AdmissionFile incoming(fixture, "changed-during-ranges", 800, 4000);
  size_t callbacks = 0;
  bool changed = false;
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path, [&] {
    // Initial check, all packet validations, first window, then second window.
    if (++callbacks == incoming.packets.size() + 3) {
      auto metadata = store->getManifest(packetName().toUri(ndn::name::UriFormat::CANONICAL));
      metadata.policyEpoch = "changed-after-first-range";
      ++metadata.generation;
      store->put(metadata, original);
      changed = true;
    }
  }), std::runtime_error, [](const std::runtime_error& error) {
    return std::string(error.what()) == "repo-original-packet-catalog-changed";
  });
  BOOST_CHECK(changed);
  BOOST_CHECK(!store->has(incoming.manifest.objectName));
  BOOST_CHECK(repo.getDataPacket(packetName().toUri()) == original);
  // A failed transaction must release its range reservation and staging.
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
  BOOST_CHECK(repo.getDataPacket(incoming.packets.back().first.toUri()) ==
              incoming.packets.back().second);
}

BOOST_AUTO_TEST_CASE(PacketFileCannotShadowLegacyAlternateStandalonePacket)
{
  Fixture fixture;
  auto store = fixture.store("legacy-admission");
  RepoCore repo(fixture.capability(), store);
  AdmissionFile incoming(fixture, "legacy-packet", 1);
  const auto& name = incoming.packets.front().first;
  const auto canonical = name.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = name.toUri(ndn::name::UriFormat::ALTERNATE);
  BOOST_REQUIRE_NE(canonical, alternate);
  const auto& original = incoming.packets.front().second;
  repo.putDataPacket(canonical, original);
  auto legacy = store->getManifest(canonical);
  legacy.objectName = alternate;
  legacy.packetNames = {alternate};
  store->put(legacy, original);
  BOOST_REQUIRE(store->erase(canonical));
  BOOST_REQUIRE(!store->has(canonical));
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path),
    std::runtime_error, [](const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-is-immutable";
    });
  BOOST_CHECK(!store->has(canonical));
  BOOST_CHECK(repo.getDataPacket(canonical) == original);
}

BOOST_AUTO_TEST_CASE(CatalogParseReuseRequiresExactCurrentBytes)
{
  Fixture fixture;
  const auto directory = fixture.root / "parse-cache";
  auto store = makeFilesystemRepoStore(directory.string());
  const auto wire = packet(packetName());
  RepoObjectManifest metadata;
  metadata.objectName = packetName().toUri(ndn::name::UriFormat::CANONICAL);
  metadata.objectType = "ndn-data-wire";
  metadata.packetNames = {metadata.objectName};
  metadata.sha256 = sha256Hex(wire);
  metadata.size = wire.size();
  metadata.generation = 1;
  metadata.policyEpoch = "epoch-a";
  store->put(metadata, wire);
  const auto sidecar = std::filesystem::directory_iterator(directory / "manifests")->path();
  const auto originalTime = std::filesystem::last_write_time(sidecar);
  const auto writeSidecar = [&](const std::string& bytes) {
    std::ofstream stream(sidecar, std::ios::binary | std::ios::trunc);
    stream << bytes;
    stream.close();
    std::filesystem::last_write_time(sidecar, originalTime);
  };
  CatalogParseScope counts;
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 1U);
  BOOST_CHECK_EQUAL(catalogParses.load(), 1U);
  BOOST_CHECK_EQUAL(store->listManifests().front().policyEpoch, "epoch-a");
  BOOST_CHECK_EQUAL(catalogParses.load(), 1U);
  metadata.policyEpoch = "epoch-b";
  const auto current = metadata.toJson();
  BOOST_REQUIRE_EQUAL(current.size(), std::filesystem::file_size(sidecar));
  writeSidecar(current);
  BOOST_CHECK_EQUAL(store->listManifests().front().policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(catalogParses.load(), 2U);
  BOOST_CHECK(std::filesystem::last_write_time(sidecar) == originalTime);

  auto malformed = current;
  const std::string consistency = "\"writeConsistency\":\"";
  const auto consistencyAt = malformed.find(consistency);
  BOOST_REQUIRE_NE(consistencyAt, std::string::npos);
  malformed.replace(consistencyAt + consistency.size(), 3, "BAD");
  writeSidecar(malformed);
  BOOST_CHECK_THROW(store->listManifests(), std::invalid_argument);
  writeSidecar(current);
  BOOST_CHECK_EQUAL(store->listManifests().front().policyEpoch, "epoch-b");
  const auto backup = directory / "sidecar-backup";
  std::filesystem::rename(sidecar, backup);
  std::filesystem::create_symlink(backup, sidecar);
  BOOST_CHECK_THROW(store->listManifests(), std::exception);
  std::filesystem::remove(sidecar);
  BOOST_CHECK(store->listManifests().empty());
  std::filesystem::rename(backup, sidecar);
  const auto parsesBeforeRestore = catalogParses.load();
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 1U);
  BOOST_CHECK_EQUAL(catalogParses.load(), parsesBeforeRestore + 1);
  store.reset();
  store = makeFilesystemRepoStore(directory.string());
  const auto parsesBeforeReopen = catalogParses.load();
  BOOST_CHECK_EQUAL(store->listManifests().front().policyEpoch, "epoch-b");
  BOOST_CHECK_GT(catalogParses.load(), parsesBeforeReopen);
}

BOOST_AUTO_TEST_CASE(CatalogParseCacheEvictsBeyondEntryBound)
{
  Fixture fixture;
  const auto directory = fixture.root / "bounded-parse-cache";
  auto store = makeFilesystemRepoStore(directory.string());
  RepoObjectManifest metadata;
  for (size_t i = 0; i < 4097; ++i) {
    metadata.objectName = "/actual/producer/catalog/" + std::to_string(i);
    std::ofstream output(directory / "manifests" / (std::to_string(i) + ".json"));
    output << metadata.toJson();
  }
  CatalogParseScope counts;
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 4097U);
  BOOST_CHECK_EQUAL(catalogParses.load(), 4097U);
  catalogParses = 0;
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 4097U);
  BOOST_CHECK_GT(catalogParses.load(), 0U);
}

BOOST_AUTO_TEST_CASE(AtomicPacketSetBackendPreservesWireAcrossReuseAndRestart)
{
  if (const char* path = std::getenv("NDNSF_SPEC201_PACKET_SET_REOPEN")) {
    StorageCapability capability;
    capability.freeBytes = 1U << 20;
    capability.repoNode = "/spec201/repo";
    auto repo = std::make_shared<RepoCore>(capability, makeFilesystemRepoStore(path, 1U << 20, 1U << 20));
    RepoEncryptedLargeDataStore backend(repo);
    PacketSet expected;
    const auto found = backend.lookupDurable(expected.options.publicationIdentity);
    BOOST_REQUIRE(found);
    for (const auto& item : expected.packets)
      BOOST_CHECK(found->source->readOriginalDataPacket(item.first) == item.second);
    return;
  }
  Fixture fixture;
  PacketSet set;
  {
    auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("set"));
    RepoEncryptedLargeDataStore backend(repo);
    size_t cursor = 0;
    const auto next = [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      if (cursor == set.packets.size()) return std::nullopt;
      return set.packets[cursor++];
    };
    auto source = backend.commitOriginalDataPackets(set.manifest.getName(), set.options, next);
    BOOST_REQUIRE(source->isDurable());
    BOOST_CHECK_EQUAL(source->size(), set.manifest.envelopeBytes);
    BOOST_CHECK_THROW(source->read(0, 1), std::runtime_error);
    for (const auto& item : set.packets) {
      BOOST_CHECK(source->readOriginalDataPacket(item.first) == item.second);
      BOOST_CHECK(repo->getDataPacket(item.first.toUri(ndn::name::UriFormat::ALTERNATE)) == item.second);
      BOOST_CHECK(repo->hasDataPacket(item.first.toUri()));
      BOOST_CHECK(repo->has(item.first.toUri(ndn::name::UriFormat::ALTERNATE)));
      BOOST_CHECK(repo->get(item.first.toUri()) == item.second);
      const auto metadata = repo->getManifest(item.first.toUri(ndn::name::UriFormat::ALTERNATE));
      BOOST_CHECK_EQUAL(metadata.objectName, item.first.toUri(ndn::name::UriFormat::CANONICAL));
      BOOST_CHECK_EQUAL(metadata.size, item.first == set.manifest.getName()
                        ? set.options.packetWireBytes : item.second.size());
      BOOST_CHECK_EQUAL(repo->catalogLookup(item.first.toUri()).manifest.objectName, metadata.objectName);
    }
    const auto stored = repo->list().front();
    BOOST_CHECK_THROW(repo->put(set.options.manifestName, {1, 2, 3}), std::runtime_error);
    BOOST_CHECK_THROW(repo->put(set.packets.back().first.toUri(), {1, 2, 3}), std::runtime_error);
    BOOST_CHECK_THROW(repo->putManifest(stored), std::invalid_argument);
    BOOST_CHECK_EQUAL(repo->putDataPacket(set.packets.back().first.toUri(), set.packets.back().second).size,
                      set.packets.back().second.size());
    BOOST_CHECK_THROW(repo->putDataPacket(set.packets.back().first.toUri(),
                      packet(set.packets.back().first)), std::runtime_error);
    const auto decoded = parseManifestJson(stored.toJson());
    BOOST_CHECK(decoded.packetOffsets == stored.packetOffsets);
    BOOST_CHECK_EQUAL(decoded.envelopeBytes, stored.envelopeBytes);
    BOOST_CHECK_EQUAL(decoded.dataPrefix, stored.dataPrefix);
    BOOST_CHECK_THROW(repo->remove(set.packets.back().first.toUri()), std::invalid_argument);
    cursor = 0;
    auto reused = backend.commitOriginalDataPackets(set.manifest.getName(), set.options, next);
    BOOST_CHECK_EQUAL(repo->list().size(), 1U);
    BOOST_CHECK_EQUAL(repo->list().front().operationId, stored.operationId);
    // Generic packet import must preserve the existing protected metadata.
    RepoObjectManifest imported = stored;
    imported.publicationIdentity.clear();
    imported.operationId = "generic-copy";
    const auto archive = fixture.root / "generic-copy.bin";
    {
      std::ofstream file(archive, std::ios::binary);
      for (const auto& item : set.packets)
        file.write(reinterpret_cast<const char*>(item.second.data()), item.second.size());
    }
    BOOST_CHECK_EQUAL(repo->commitOriginalPacketFile(imported, archive).publicationIdentity,
                      stored.publicationIdentity);
    for (unsigned field = 0; field < 3; ++field) {
      auto conflicting = imported;
      if (field == 0) conflicting.dataPrefix = "/foreign/producer/DATA";
      if (field == 1) ++conflicting.envelopeBytes;
      if (field == 2) conflicting.protectionEpoch = "different";
      BOOST_CHECK_THROW(repo->commitOriginalPacketFile(conflicting, archive), std::runtime_error);
      BOOST_CHECK_EQUAL(repo->list().front().toJson(), stored.toJson());
      BOOST_CHECK(repo->getDataPacket(set.packets.back().first.toUri()) == set.packets.back().second);
    }
    auto shared = source;
    source->release();
    BOOST_CHECK(shared->readOriginalDataPacket(set.packets.back().first) == set.packets.back().second);
    BOOST_CHECK(reused->readOriginalDataPacket(set.packets.back().first) == set.packets.back().second);
  }
  {
    auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("set"));
    RepoEncryptedLargeDataStore backend(repo);
    const auto found = backend.lookupDurable(set.options.publicationIdentity);
    BOOST_REQUIRE(found);
    BOOST_CHECK_EQUAL(found->packetStorageFormat, "original-packet-set");
    BOOST_CHECK_EQUAL(found->manifestName, set.options.manifestName);
    BOOST_CHECK_EQUAL(found->packetWireBytes, set.options.packetWireBytes);
    BOOST_CHECK_EQUAL(found->source->size(), set.manifest.envelopeBytes);
    for (const auto& item : set.packets)
      BOOST_CHECK(found->source->readOriginalDataPacket(item.first) == item.second);
    ndn::KeyChain keys("pib-memory:", "tpm-memory:");
    ndn::DummyClientFace face(keys, {true, true});
    RepoPacketServer server(face, repo);
    bool ready = false;
    server.servePrefix(set.manifest.dataPrefix, [&] { ready = true; }, {});
    face.processEvents(ndn::time::milliseconds(10));
    BOOST_REQUIRE(ready);
    face.receive(ndn::Interest(set.packets.back().first));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    const auto& wire = face.sentData.back().wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == set.packets.back().second);
  }
  const pid_t child = ::fork();
  BOOST_REQUIRE_GE(child, 0);
  if (child == 0) {
    ::setenv("NDNSF_SPEC201_PACKET_SET_REOPEN", (fixture.root / "set").c_str(), 1);
    ::execl("/proc/self/exe", "spec201-transparent-repo",
      "--run_test=Spec201TransparentRepo/AtomicPacketSetBackendPreservesWireAcrossReuseAndRestart",
      static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  BOOST_REQUIRE_EQUAL(::waitpid(child, &status, 0), child);
  BOOST_REQUIRE(WIFEXITED(status));
  BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
}

BOOST_AUTO_TEST_CASE(PacketSetCancellationQuotaAndIncompleteInputLeaveNoRoot)
{
  Fixture fixture;
  PacketSet set;
  auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("cancel-set"));
  RepoEncryptedLargeDataStore backend(repo);
  const auto prior = packet(packetName());
  repo->putDataPacket(packetName().toUri(), prior);
  size_t cursor = 0;
  bool drained = false;
  unsigned checks = 0;
  const auto next = [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
    if (cursor == set.packets.size()) { drained = true; return std::nullopt; }
    return set.packets[cursor++];
  };
  BOOST_CHECK_THROW(backend.commitOriginalDataPackets(set.manifest.getName(), set.options, next,
    [&] { if (drained && ++checks == set.packets.size() + 3) throw std::runtime_error("cancelled"); }),
    std::runtime_error);
  BOOST_CHECK(!repo->hasDataPacket(set.options.manifestName));
  BOOST_CHECK_EQUAL(repo->list().size(), 1U);
  BOOST_CHECK(repo->getDataPacket(packetName().toUri()) == prior);
  cursor = 0;
  auto source = backend.commitOriginalDataPackets(set.manifest.getName(), set.options, next);
  BOOST_CHECK(source->readOriginalDataPacket(set.packets.front().first) == set.packets.front().second);
  auto capability = fixture.capability();
  capability.freeBytes = set.options.packetWireBytes - 1;
  auto small = std::make_shared<RepoCore>(capability, fixture.store("small-set"));
  RepoEncryptedLargeDataStore limited(small);
  cursor = 0;
  BOOST_CHECK_THROW(limited.commitOriginalDataPackets(set.manifest.getName(), set.options, next),
                    std::runtime_error);
  BOOST_CHECK(small->list().empty());
  cursor = set.packets.size();
  BOOST_CHECK_THROW(limited.commitOriginalDataPackets(set.manifest.getName(), set.options, next),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UnsupportedBackendRejectsBeforeRequestingPackets)
{
  Fixture fixture;
  PacketSet set;
  auto repo = std::make_shared<RepoCore>(fixture.capability(),
    makeSqliteRepoStore((fixture.root / "legacy.sqlite").string()));
  RepoEncryptedLargeDataStore backend(repo);
  BOOST_CHECK(!backend.supportsOriginalDataPackets());
  unsigned calls = 0;
  BOOST_CHECK_THROW(backend.commitOriginalDataPackets(set.manifest.getName(), set.options,
    [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      ++calls;
      return std::nullopt;
    }), std::runtime_error);
  BOOST_CHECK_EQUAL(calls, 0U);
  BOOST_CHECK(repo->list().empty());
  BOOST_CHECK_THROW(backend.commitFile("/wrong/format", fixture.root / "absent", 1, set.options),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NodeInsertCommitsCompleteOriginalSetOrNothing)
{
  Fixture fixture;
  PacketSet set;
  RepoDataReference reference;
  reference.objectName = set.options.manifestName;
  reference.dataPrefix = set.manifest.producer.toUri();
  reference.expectedSize = set.options.packetWireBytes;
  std::vector<std::vector<uint8_t>> wires;
  for (const auto& item : set.packets) wires.push_back(item.second);
  auto capacity = fixture.capability();
  capacity.freeBytes = set.options.packetWireBytes - 1;
  RepoNode limited(ndn::Name("/repo/limited"), capacity, fixture.store("node-limited"));
  BOOST_CHECK_EQUAL(limited.insertWirePackets(reference, wires).state, "FAILED");
  for (const auto& item : set.packets) BOOST_CHECK(!limited.hasDataPacket(item.first.toUri()));
  RepoNode node(ndn::Name("/repo/full"), fixture.capability(), fixture.store("node-full"));
  BOOST_CHECK_EQUAL(node.insertWirePackets(reference, wires).state, "DONE");
  BOOST_CHECK_EQUAL(node.insertWirePackets(reference, wires).state, "DONE");
  BOOST_CHECK_EQUAL(node.core().list().size(), 1U);
  for (const auto& item : set.packets) BOOST_CHECK(node.getDataPacket(item.first.toUri()) == item.second);
  auto wrongRoot = reference;
  wrongRoot.objectName = "/repo/renamed/object";
  BOOST_CHECK_EQUAL(node.insertWirePackets(wrongRoot, wires).state, "FAILED");
  auto badWire = wires;
  badWire.back().back() ^= 1;
  BOOST_CHECK_EQUAL(node.insertWirePackets(reference, badWire).state, "FAILED");
  for (const auto& item : set.packets) BOOST_CHECK(node.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(FileSourceReplaysOriginalWireAcrossMutationAndRestart)
{
  Fixture fixture;
  const auto source = fixture.root / "source.bin";
  { std::ofstream out(source, std::ios::binary); out << std::string(9000, 'a'); }
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const ndn::Name identity("/spec201/file-owner");
  keys.createIdentity(identity);
  ndn::Name base = identity;
  base.append("NDNSF-DI").append("blob")
    .append(ndn_service_framework::makeNamingLabelComponent("fixture"));
  const auto cache = fixture.root / "source-cache";
  std::vector<uint8_t> first;
  ndn::Name original;
  {
    ndn_service_framework::ImmutableFilePublication publication(base, source.string(), keys, identity, 4000, 60000, cache);
    BOOST_CHECK_EQUAL(publication.fileSize(), 9000U);
    BOOST_CHECK_EQUAL(publication.segmentCount(), 3U);
    auto data = publication.packet(0);
    original = data.getName();
    const auto& wire = data.wireEncode();
    first.assign(wire.begin(), wire.end());
    BOOST_CHECK(ndn::security::verifySignature(data, keys.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate()));
    { std::ofstream out(source, std::ios::binary | std::ios::trunc); out << "mutated"; }
    const auto again = publication.packet(0).wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(again.begin(), again.end()) == first);
    BOOST_CHECK_EQUAL(publication.packet(2).getContent().value_size(), 1000U);
    BOOST_CHECK_THROW(publication.packet(3), std::out_of_range);
  }
  { std::ofstream out(source, std::ios::binary | std::ios::trunc); out << std::string(9000, 'a'); }
  {
    ndn_service_framework::ImmutableFilePublication reopened(base, source.string(), keys, identity, 4000, 60000, cache);
    const auto data = reopened.packet(0);
    BOOST_CHECK(data.getName() == original);
    const auto& wire = data.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == first);
  }
  BOOST_CHECK_THROW(ndn_service_framework::ImmutableFilePublication(ndn::Name("/foreign/data"), source.string(), keys, identity, 4000, 60000, cache), std::invalid_argument);
  BOOST_CHECK_THROW(ndn_service_framework::ImmutableFilePublication(base, source.string(), keys, ndn::Name("/unresolved"), 4000, 60000, cache), std::exception);
  ndn::Name falseContent = base;
  falseContent.append(ndn_service_framework::makeNamingDigestComponent(std::string(64, '0')))
    .append("REPRESENTATION").append(ndn_service_framework::makeNamingDigestComponent(std::string(64, '1'))).append("DATA");
  BOOST_CHECK_THROW(ndn_service_framework::ImmutableFilePublication(falseContent, source.string(), keys, identity, 4000, 60000, cache), std::runtime_error);
  for (const auto& entry : std::filesystem::directory_iterator(cache)) {
    if (entry.path().extension() != ".packets") continue;
    std::fstream file(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(24 + 8 + first.size() - 1); file.put(static_cast<char>(first.back() ^ 1)); file.close();
  }
  ndn_service_framework::ImmutableFilePublication corrupt(base, source.string(), keys, identity, 4000, 60000, cache);
  BOOST_CHECK_THROW(corrupt.packet(0), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(FileSourceFullWireSlotAndCapacityFailureAreSafe)
{
  Fixture fixture;
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const ndn::Name identity("/spec201/file-boundary");
  keys.createIdentity(identity, ndn::RsaKeyParams(2048));
  std::vector<uint8_t> content(8000, 42);
  const auto source = fixture.root / "full.bin";
  { std::ofstream out(source, std::ios::binary); out.write(reinterpret_cast<const char*>(content.data()), content.size()); }
  ndn::Name base;
  int filler = 400;
  size_t length = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    base = identity;
    base.append("NDNSF-DI").append("blob").append(std::string(filler, 'x'));
    ndn::Name name = base; name.appendVersion(1780000000000ULL).appendSegment(0);
    ndn::Data data(name); data.setContent(content);
    data.setFreshnessPeriod(ndn::time::milliseconds(60000));
    data.setFinalBlock(ndn::name::Component::fromSegment(0));
    keys.sign(data, ndn::security::signingByIdentity(identity));
    length = data.wireEncode().size();
    if (length == 8800) break;
    filler += 8800 - static_cast<int>(length);
    BOOST_REQUIRE(filler > 0 && filler < 900);
  }
  BOOST_REQUIRE_EQUAL(length, 8800U);
  const auto cache = fixture.root / "full-cache";
  ndn_service_framework::ImmutableFilePublication publication(base, source.string(), keys, identity, 8000, 60000, cache);
  BOOST_CHECK_EQUAL(publication.packet(0).wireEncode().size(), 8800U);
  BOOST_CHECK_EQUAL(publication.packet(0).getContent().value_size(), content.size());
  const char* previous = std::getenv("NDNSF_FILE_PUBLICATION_CACHE_MAX_BYTES");
  const std::string saved = previous ? previous : "";
  ::setenv("NDNSF_FILE_PUBLICATION_CACHE_MAX_BYTES", "1", 1);
  const auto limited = fixture.root / "limited-cache";
  BOOST_CHECK_THROW(ndn_service_framework::ImmutableFilePublication(base, source.string(), keys, identity, 8000, 60000, limited), std::runtime_error);
  if (previous) ::setenv("NDNSF_FILE_PUBLICATION_CACHE_MAX_BYTES", saved.c_str(), 1);
  else ::unsetenv("NDNSF_FILE_PUBLICATION_CACHE_MAX_BYTES");
  for (const auto& item : std::filesystem::directory_iterator(limited))
    BOOST_CHECK(item.path().extension() != ".packets" && item.path().extension() != ".partial");
}

BOOST_AUTO_TEST_SUITE_END()
