#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoPacketServer.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"
#include "ndn-service-framework/ImmutableFilePublication.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/mgmt/nfd/control-parameters.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <future>
#include <condition_variable>
#include <boost/asio/post.hpp>
#include <chrono>
#include <sys/stat.h>
#include <cerrno>
#include <sqlite3.h>

namespace ndnsf_distributed_repo::detail {
struct RepoNodeTestAccess
{
  static size_t pending(const RepoNode& node) { return node.m_originalDataPending.load(); }
  static void enqueue(RepoNode& node, const ndn::Interest& interest)
  { node.enqueueOriginalDataInterest(interest); }
  static bool queuedOwnsWire(RepoNode& node, const ndn::Interest& interest)
  {
    std::lock_guard<std::mutex> guard(node.m_originalDataMutex);
    if (node.m_originalDataQueue.empty()) return false;
    const auto& queued = node.m_originalDataQueue.back();
    const auto& sourceWire = interest.wireEncode();
    const auto& queuedWire = queued.wireEncode();
    return queuedWire.size() == sourceWire.size() &&
      std::equal(queuedWire.begin(), queuedWire.end(), sourceWire.begin()) &&
      queuedWire.data() != sourceWire.data() &&
      queued.getName().wireEncode().data() != interest.getName().wireEncode().data();
  }
  static bool stopped(RepoNode& node)
  {
    std::lock_guard<std::mutex> guard(node.m_originalDataMutex);
    return node.m_stopOriginalDataWorker;
  }
};
}

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

void waitForOriginalData(ndn::DummyClientFace& face, const RepoNode* node)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (node && detail::RepoNodeTestAccess::pending(*node) != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    face.getIoContext().restart();
    face.processEvents(ndn::time::milliseconds(1));
    std::this_thread::yield();
  }
  if (node) BOOST_REQUIRE_EQUAL(detail::RepoNodeTestAccess::pending(*node), 0U);
  face.getIoContext().restart();
  face.processEvents(ndn::time::milliseconds(1));
}

struct SlowReadStore : CountingStore
{
  using CountingStore::CountingStore;
  mutable std::mutex mutex;
  mutable std::condition_variable wake;
  mutable bool armed = false, entered = false, released = false, timedOut = false;
  mutable std::atomic<bool> failOnce{false};
  void arm()
  {
    std::lock_guard<std::mutex> guard(mutex);
    armed = true; entered = released = timedOut = false;
  }
  void release()
  {
    std::lock_guard<std::mutex> guard(mutex);
    released = true; wake.notify_all();
  }
  bool waitEntered()
  {
    std::unique_lock<std::mutex> guard(mutex);
    return wake.wait_for(guard, std::chrono::seconds(2), [&] { return entered; });
  }
  bool timeout() const { std::lock_guard<std::mutex> guard(mutex); return timedOut; }
  StoredObject get(const std::string& name) const override
  {
    if (failOnce.exchange(false)) throw 37;
    {
      std::unique_lock<std::mutex> guard(mutex);
      if (armed && !entered) {
        entered = true; wake.notify_all();
        timedOut = !wake.wait_for(guard, std::chrono::seconds(2), [&] { return released; });
      }
    }
    return delegate->get(name);
  }
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
  explicit PacketSet(const std::string& producer = "/actual/packet-set-producer",
                     const std::string& label = "canonical-source")
  {
    using namespace ndn_service_framework;
    manifest.producer = ndn::Name(producer);
    PublishedContentName semantics;
    semantics.kind = ContentDataKind::ModelFile;
    semantics.label = label;
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
    if (label != "canonical-source")
      options.publicationIdentity = "sha256:" + sha256Hex(toBytes(producer + "\n" + label));
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
void signPacketSetAsProducer(PacketSet& set, ndn::KeyChain& keys)
{
  keys.createIdentity(set.manifest.producer, ndn::RsaKeyParams(2048));
  set.options.packetWireBytes = 0;
  for (auto& packet : set.packets) {
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(packet.second.data(), packet.second.size())));
    keys.sign(data, ndn::security::signingByIdentity(set.manifest.producer));
    const auto& wire = data.wireEncode();
    packet.second.assign(wire.begin(), wire.end());
    set.options.packetWireBytes += packet.second.size();
  }
}

struct OwnedAdapterFixture
{
  Fixture files;
  ndn::KeyChain keys{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keys, {true, true}};
  StorageCapability capability = files.capability();
  std::unique_ptr<ndn_service_framework::ServiceProvider> provider;
  std::shared_ptr<RepoNode> node;
  std::unique_ptr<SqliteArtifactMetadataStore> journal;
  std::string database = (files.root / "owned.sqlite3").string();
  OwnedAdapterFixture()
  {
    capability.repoMode = "persistent";
    (void)keys.createIdentity(ndn::Name("/owned/provider"), ndn::RsaKeyParams(2048));
    (void)keys.createIdentity(ndn::Name("/owned/authority"), ndn::RsaKeyParams(2048));
    reopen();
  }
  void close()
  {
    provider.reset(); // Release registered handlers before inspecting Node lifetime.
    node.reset(); journal.reset();
  }
  void reopen()
  {
    close();
    node = std::make_shared<RepoNode>(ndn::Name("/owned/repo-service"), capability,
      files.store("owned"), capability.repoNode, "/owned/provider", database);
    provider = std::make_unique<ndn_service_framework::ServiceProvider>(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/owned/group"),
      keys.getPib().getIdentity(ndn::Name("/owned/provider")).getDefaultKey().getDefaultCertificate(),
      keys.getPib().getIdentity(ndn::Name("/owned/authority")).getDefaultKey().getDefaultCertificate(),
      std::filesystem::absolute("examples/trust-any.conf").string());
    node->registerServices(*provider);
    journal = std::make_unique<SqliteArtifactMetadataStore>(database, true, 16, false);
  }
  static uint64_t now()
  {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  }
  ndn_service_framework::EncryptedLargeDataCommitOptions authorize(
    ndn_service_framework::EncryptedLargeDataCommitOptions options, const ndn::Name& name,
    const std::string& owner, const std::string& id)
  {
    const auto grant = journal->grantUserStorage(owner, capability.repoNode, id, "/owned/writer",
      name.toUri(ndn::name::UriFormat::ALTERNATE), now() + 60000, now());
    options.storageActor = ndn_service_framework::DurableStorageActorContext{
      "/owned/writer", {grant.repoNode, grant.ownerIdentity, grant.grantId, grant.ownerEpoch}};
    return options;
  }
  static std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commit(ndn_service_framework::EncryptedLargeDataRangeStore& adapter, const PacketSet& set,
         const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
         const std::function<void()>& active = {})
  {
    size_t cursor = 0;
    return adapter.commitOriginalDataPackets(set.manifest.getName(), options,
      [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
        if (cursor == set.packets.size()) return std::nullopt;
        return set.packets[cursor++];
      }, active);
  }
  RepoUserStorageDeleteReply remove(const std::string& owner, const ndn::Name& name,
                                   uint64_t epoch, const std::string& id)
  {
    const auto payload = toBytes(RepoUserStorageDeleteRequest{1, id,
      name.toUri(ndn::name::UriFormat::ALTERNATE), epoch}.toJson());
    ndn_service_framework::RequestMessage request;
    ndn::Buffer buffer(payload.data(), payload.size());
    request.setPayload(buffer, buffer.size());
    const auto response = provider->dispatchRequest(ndn::Name(owner), provider->getName(),
      makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_DELETE"), ndn::Name("/owned/delete").append(id), request);
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto result = response.getPayload();
    return RepoUserStorageDeleteReply::fromJson(std::string(result.begin(), result.end()));
  }
};

int rejectDirectoryFsync(int fd)
{
  struct stat value{};
  if (::fstat(fd, &value) == 0 && S_ISDIR(value.st_mode)) {
    errno = EIO; return -1;
  }
  return ::fsync(fd);
}
struct RejectDirectorySync
{
  detail::FilesystemRepoStoreIoHooks previous =
    detail::installFilesystemRepoStoreIoHooks({rejectDirectoryFsync, nullptr, nullptr});
  ~RejectDirectorySync() { detail::installFilesystemRepoStoreIoHooks(previous); }
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec201TransparentRepo)

BOOST_AUTO_TEST_CASE(OriginalWireInterestMatchingSupportsPrefixAndImplicitDigest)
{
  Fixture fixture;
  RepoCore repo(fixture.capability(), fixture.store("interest-matching"));
  const auto name = packetName();
  const auto wire = packet(name);
  repo.putDataPacket(name.toUri(), wire);
  ndn::Interest prefix(name.getPrefix(-1)); prefix.setCanBePrefix(true);
  BOOST_CHECK(repo.getDataPacket(prefix) == wire);
  size_t admissions = 0;
  BOOST_CHECK_THROW(repo.getDataPacket(prefix, [&](const auto&) { ++admissions; return false; }), std::runtime_error);
  BOOST_CHECK_GT(admissions, 0U);
  prefix.setCanBePrefix(false);
  BOOST_CHECK_THROW(repo.getDataPacket(prefix), std::runtime_error);
  const ndn::Data original(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
  BOOST_CHECK(repo.getDataPacket(ndn::Interest(original.getFullName())) == wire);
  auto bad = name; const std::vector<uint8_t> zeros(32, 0);
  bad.appendImplicitSha256Digest(ndn::span<const uint8_t>(zeros.data(), zeros.size()));
  BOOST_CHECK_THROW(repo.getDataPacket(ndn::Interest(bad)), std::runtime_error);
}

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

BOOST_AUTO_TEST_CASE(PacketFileFailedCleanupRetainsCapacityUntilOwnedAbort)
{
  Fixture fixture;
  AdmissionFile incoming(fixture, "owned-failed-cleanup", 2);
  auto store = fixture.store("owned-failed-cleanup");
  const auto original = packet(packetName());
  auto capability = fixture.capability();
  capability.freeBytes = original.size() + 2 * incoming.manifest.size;
  RepoCore repo(capability, store);
  repo.putDataPacket(packetName().toUri(), original);
  const auto staging = fixture.root / "owned-failed-cleanup" / "staging";
  struct RestorePermissions {
    std::filesystem::path path;
    ~RestorePermissions() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } restore{staging};
  BOOST_REQUIRE_NE(::geteuid(), 0U); // This oracle requires actual unprivileged unlink denial.
  bool staged = false;
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path, [&] {
    if (!staged && !std::filesystem::is_empty(staging)) {
      staged = !std::filesystem::is_empty(staging);
      std::filesystem::permissions(staging,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
      throw std::runtime_error("test-owner-cancelled");
    }
  }), std::runtime_error, [](const std::runtime_error& error) {
    return std::string(error.what()) == "test-owner-cancelled";
  });
  BOOST_REQUIRE(staged);
  BOOST_CHECK(!std::filesystem::is_empty(staging));
  BOOST_CHECK(!repo.has(incoming.manifest.objectName));
  BOOST_CHECK_EQUAL(repo.reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 0U);
  BOOST_CHECK(!std::filesystem::is_empty(staging));
  auto other = incoming.manifest;
  other.objectName = "/actual/producer/other-capacity";
  other.operationId = "other-capacity";
  other.generation = 1;
  other.size = incoming.manifest.size + 1;
  std::vector<uint8_t> otherBytes(other.size, 7);
  other.sha256 = sha256Hex(otherBytes);
  other.objectType = "object";
  other.packetNames.clear();
  other.packetOffsets.clear();
  BOOST_CHECK_EXCEPTION(repo.putRange(other, {0, other.size}, otherBytes), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()).find("insufficient") != std::string::npos;
    });
  BOOST_CHECK(repo.getDataPacket(packetName().toUri()) == original);
  std::filesystem::permissions(staging, std::filesystem::perms::owner_all);
  auto expected = incoming.manifest;
  BOOST_CHECK_EQUAL(expected.generation, 0U);
  expected.generation = 1; // Core assigned this actual staging generation.
  BOOST_CHECK(repo.abortUserStorageRangesIfOwned(expected));
  BOOST_CHECK(!repo.abortUserStorageRangesIfOwned(expected));
  BOOST_CHECK(std::filesystem::is_empty(staging));
  BOOST_CHECK_NO_THROW(repo.putRange(other, {0, other.size}, otherBytes));
  BOOST_CHECK(repo.abortUserStorageRangesIfOwned(other));
  BOOST_CHECK(repo.getDataPacket(packetName().toUri()) == original);
}

BOOST_AUTO_TEST_CASE(PacketFileCommittedResidueIsNotRolledBackByFailureOrReaper)
{
  Fixture fixture;
  AdmissionFile incoming(fixture, "owned-committed-residue", 2);
  auto store = fixture.store("owned-committed-residue");
  auto capability = fixture.capability();
  capability.freeBytes = 2 * incoming.manifest.size;
  RepoCore repo(capability, store);
  const auto staging = fixture.root / "owned-committed-residue" / "staging";
  bool injected = false;
  auto committed = incoming.manifest;
  committed.generation = 1;
  BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path, [&] {
    if (!injected && !std::filesystem::is_empty(staging)) {
      injected = true;
      store->commitRanges(committed); // Real durable residue before the Core sees failure.
      throw std::runtime_error("test-post-commit-cancelled");
    }
  }), std::runtime_error, [](const std::runtime_error& error) {
    return std::string(error.what()) == "test-post-commit-cancelled";
  });
  BOOST_CHECK(injected);
  BOOST_REQUIRE(repo.has(committed.objectName));
  BOOST_CHECK_EQUAL(repo.reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 0U);
  auto other = committed;
  other.objectName = "/actual/producer/committed-residue-capacity";
  other.operationId = "residue-capacity";
  other.size = incoming.manifest.size + 1;
  std::vector<uint8_t> bytes(other.size, 9);
  other.sha256 = sha256Hex(bytes);
  other.objectType = "object";
  other.packetNames.clear();
  other.packetOffsets.clear();
  BOOST_CHECK_EXCEPTION(repo.putRange(other, {0, other.size}, bytes), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()).find("insufficient") != std::string::npos;
    });
  for (const auto& item : incoming.packets)
    BOOST_CHECK(repo.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(OwnedReplacementReaperPreservesOlderCommittedBytes)
{
  Fixture fixture;
  auto store = fixture.store("owned-replacement-reaper");
  RepoCore repo(fixture.capability(), store);
  const std::vector<uint8_t> oldBytes{1, 2, 3};
  const auto old = repo.put("/actual/producer/owned-replacement", oldBytes);
  auto replacement = old;
  replacement.operationId = "owned-replacement";
  replacement.generation = old.generation + 1;
  const std::vector<uint8_t> newBytes{4, 5, 6, 7};
  replacement.size = newBytes.size();
  replacement.sha256 = sha256Hex(newBytes);
  repo.putRange(replacement, {0, newBytes.size()}, newBytes);
  BOOST_CHECK_EQUAL(repo.reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 1U);
  BOOST_CHECK_EQUAL(repo.reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 0U);
  BOOST_CHECK(repo.get(old.objectName) == oldBytes);
  BOOST_CHECK_EQUAL(repo.getManifest(old.objectName).generation, old.generation);
  BOOST_CHECK(std::filesystem::is_empty(fixture.root / "owned-replacement-reaper" / "staging"));
}

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

BOOST_AUTO_TEST_CASE(OriginalServingKeepsControlResponsiveAndBoundsShutdown)
{
  Fixture fixture;
  auto store = std::make_shared<SlowReadStore>(fixture.store("slow-serving"));
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys, {true, true});
  auto node = std::make_shared<RepoNode>(ndn::Name("/serving/service"),
    fixture.capability(), store, "/spec201/repo", "/serving/provider");
  struct ReleaseRead { std::shared_ptr<SlowReadStore> store; ~ReleaseRead() { store->release(); } } release{store};
  const auto name = packetName(); const auto wire = packet(name);
  node->core().putDataPacket(name.toUri(), wire);
  node->registerOriginalDataServing(face);
  bool controlHandled = false;
  auto control = face.setInterestFilter(ndn::InterestFilter(ndn::Name("/control")),
    [&](const ndn::InterestFilter&, const ndn::Interest&) { controlHandled = true; });
  face.processEvents(ndn::time::milliseconds(10));
  store->arm();
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(store->waitEntered());
  ndn::Interest callerOwned(name);
  detail::RepoNodeTestAccess::enqueue(*node, callerOwned);
  BOOST_CHECK(detail::RepoNodeTestAccess::queuedOwnsWire(*node, callerOwned));
  ndn::Interest oversized(ndn::Name("/oversized").append(std::string(ndn::MAX_NDN_PACKET_SIZE, 'x')));
  BOOST_REQUIRE_GT(oversized.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
  detail::RepoNodeTestAccess::enqueue(*node, oversized);
  BOOST_CHECK_EQUAL(detail::RepoNodeTestAccess::pending(*node), 2U);
  for (size_t i = 0; i < 300; ++i) face.receive(ndn::Interest(name));
  face.receive(ndn::Interest(ndn::Name("/control/check")));
  face.processEvents(ndn::time::milliseconds(10));
  BOOST_CHECK(controlHandled);
  BOOST_CHECK(!store->timeout());
  BOOST_REQUIRE_EQUAL(detail::RepoNodeTestAccess::pending(*node), 256U);
  BOOST_CHECK(face.sentData.empty());
  store->release(); waitForOriginalData(face, node.get());
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 256U);
  for (const auto& data : face.sentData) {
    const auto& encoded = data.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(encoded.begin(), encoded.end()) == wire);
  }
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  waitForOriginalData(face, node.get());
  BOOST_CHECK_EQUAL(face.sentData.size(), 257U); // an overflowed Interest may retry

  store->failOnce.store(true);
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  waitForOriginalData(face, node.get());
  BOOST_CHECK_EQUAL(face.sentData.size(), 257U);
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  waitForOriginalData(face, node.get());
  BOOST_CHECK_EQUAL(face.sentData.size(), 258U); // non-std backend failure is contained

  store->arm();
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(store->waitEntered());
  for (size_t i = 0; i < 30; ++i) face.receive(ndn::Interest(name));
  face.processEvents(ndn::time::milliseconds(1));
  auto stopping = std::async(std::launch::async, [&] { node->beginShutdown(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!detail::RepoNodeTestAccess::stopped(*node) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  BOOST_REQUIRE(detail::RepoNodeTestAccess::stopped(*node));
  BOOST_CHECK(stopping.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  BOOST_CHECK_EQUAL(detail::RepoNodeTestAccess::pending(*node), 1U);
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(detail::RepoNodeTestAccess::pending(*node), 1U);
  store->release();
  BOOST_REQUIRE(stopping.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  stopping.get(); waitForOriginalData(face, node.get());
  const auto afterClose = face.sentData.size();
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), afterClose);
  BOOST_CHECK_THROW(node->registerOriginalDataServing(face), std::logic_error);
  const std::weak_ptr<RepoNode> weak = node;
  node.reset(); BOOST_CHECK(weak.expired());
  face.receive(ndn::Interest(name)); face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK_EQUAL(face.sentData.size(), afterClose);
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

BOOST_AUTO_TEST_CASE(ExactReadsReuseOnlyCurrentCatalogBytes)
{
  Fixture fixture;
  const auto directory = fixture.root / "exact-read-parse-cache";
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
    std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    std::filesystem::last_write_time(sidecar, originalTime);
  };
  CatalogParseScope counts;
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 1U);
  BOOST_REQUIRE_EQUAL(catalogParses.load(), 1U);
  for (size_t i = 0; i < 12; ++i) {
    BOOST_CHECK_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-a");
    const auto probe = store->probeObjectMetadata(metadata.objectName);
    BOOST_CHECK(probe.payloadPresent);
    BOOST_CHECK_EQUAL(probe.payloadSize, wire.size());
    BOOST_CHECK(store->getRange(metadata.objectName, {0, wire.size()}) == wire);
  }
  BOOST_CHECK_EQUAL(catalogParses.load(), 1U);

  metadata.policyEpoch = "epoch-b";
  const auto current = metadata.toJson();
  BOOST_REQUIRE_EQUAL(current.size(), std::filesystem::file_size(sidecar));
  writeSidecar(current);
  BOOST_CHECK_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(catalogParses.load(), 2U);
  BOOST_CHECK_EQUAL(store->probeObjectMetadata(metadata.objectName).manifest.policyEpoch, "epoch-b");
  BOOST_CHECK(store->getRange(metadata.objectName, {0, wire.size()}) == wire);
  BOOST_CHECK_EQUAL(catalogParses.load(), 4U);
  BOOST_CHECK(std::filesystem::last_write_time(sidecar) == originalTime);
  BOOST_REQUIRE_EQUAL(store->listManifests().front().policyEpoch, "epoch-b");
  const auto warmParses = catalogParses.load();
  BOOST_CHECK(store->getRange(metadata.objectName, {0, wire.size()}) == wire);
  BOOST_CHECK_EQUAL(catalogParses.load(), warmParses);

  writeSidecar(std::string(current.size(), 'x'));
  BOOST_CHECK_THROW(store->getManifest(metadata.objectName), std::exception);
  BOOST_CHECK_THROW(store->probeObjectMetadata(metadata.objectName), std::exception);
  BOOST_CHECK_THROW(store->getRange(metadata.objectName, {0, wire.size()}), std::exception);
  writeSidecar(current);
  const auto backup = directory / "sidecar-backup";
  std::filesystem::rename(sidecar, backup);
  std::filesystem::create_symlink(backup, sidecar);
  BOOST_CHECK_THROW(store->getManifest(metadata.objectName), std::exception);
  BOOST_CHECK_THROW(store->probeObjectMetadata(metadata.objectName), std::exception);
  BOOST_CHECK_THROW(store->getRange(metadata.objectName, {0, wire.size()}), std::exception);
  std::filesystem::remove(sidecar);
  BOOST_CHECK_THROW(store->getManifest(metadata.objectName), std::out_of_range);
  BOOST_CHECK(!store->probeObjectMetadata(metadata.objectName).manifestPresent);
  BOOST_CHECK_THROW(store->getRange(metadata.objectName, {0, wire.size()}), std::out_of_range);
  std::filesystem::rename(backup, sidecar);
  BOOST_CHECK_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-b");
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

BOOST_AUTO_TEST_CASE(CatalogParseCacheHonorsEncodedByteBound)
{
  Fixture fixture;
  constexpr size_t budget = 64U * 1024U * 1024U;
  for (const size_t encodedBytes : {budget - 1, budget, budget + 1}) {
    const auto directory = fixture.root / ("byte-bound-" + std::to_string(encodedBytes));
    auto store = makeFilesystemRepoStore(directory.string());
    RepoObjectManifest metadata;
    size_t totalBytes = 0, epochBytes = 0;
    for (size_t i = 0; i < 5; ++i) {
      // Each sidecar must respect readText's separate 16MiB input limit.
      const auto bytes = i < 4 ? 13U * 1024U * 1024U : encodedBytes - totalBytes;
      BOOST_REQUIRE_LE(bytes, 16U * 1024U * 1024U);
      metadata.objectName = "/actual/producer/catalog/byte-bound/" + std::to_string(i);
      const auto overhead = metadata.toJson().size();
      BOOST_REQUIRE_LT(overhead, bytes);
      // Synthetic opaque metadata exercises the cache budget, not a
      // network packet limit or an authorization policy's epoch semantics.
      metadata.policyEpoch.assign(bytes - overhead, 'x');
      epochBytes += metadata.policyEpoch.size();
      const auto sidecar = directory / "manifests" / (std::to_string(i) + ".json");
      {
        std::ofstream output(sidecar, std::ios::binary);
        output << metadata.toJson();
        BOOST_REQUIRE(output.good());
      }
      BOOST_REQUIRE_EQUAL(std::filesystem::file_size(sidecar), bytes);
      totalBytes += bytes;
      std::string{}.swap(metadata.policyEpoch);
    }
    BOOST_REQUIRE_EQUAL(totalBytes, encodedBytes);
    CatalogParseScope counts;
    {
      const auto first = store->listManifests();
      BOOST_REQUIRE_EQUAL(first.size(), 5U);
      size_t decodedEpochBytes = 0;
      for (const auto& item : first) decodedEpochBytes += item.policyEpoch.size();
      BOOST_CHECK_EQUAL(decodedEpochBytes, epochBytes);
    }
    BOOST_CHECK_EQUAL(catalogParses.load(), 5U);
    BOOST_REQUIRE_EQUAL(store->listManifests().size(), 5U);
    if (encodedBytes <= budget) BOOST_CHECK_EQUAL(catalogParses.load(), 5U);
    else BOOST_CHECK_GT(catalogParses.load(), 5U);

    metadata.objectName = "/actual/producer/catalog/small";
    {
      std::ofstream output(directory / "manifests" / "small.json", std::ios::binary);
      output << metadata.toJson();
      BOOST_REQUIRE(output.good());
    }
    BOOST_REQUIRE_EQUAL(store->listManifests().size(), 6U);
    catalogParses = 0;
    BOOST_REQUIRE_EQUAL(store->listManifests().size(), 6U);
    // All files together exceed the budget in every row. Their enumeration
    // order may differ; an eviction must force a reparse.
    BOOST_CHECK_GT(catalogParses.load(), 0U);
  }
}

BOOST_AUTO_TEST_CASE(CatalogParseRejectsOversizedMetadataWithSizeDiagnostic)
{
  Fixture fixture;
  const auto directory = fixture.root / "oversized-sidecar";
  auto store = makeFilesystemRepoStore(directory.string());
  RepoObjectManifest metadata;
  metadata.objectName = "/actual/producer/catalog/oversized";
  constexpr size_t bytes = 16U * 1024U * 1024U + 1;
  metadata.policyEpoch.assign(bytes - metadata.toJson().size(), 'x');
  const auto sidecar = directory / "manifests" / "oversized.json";
  {
    std::ofstream output(sidecar, std::ios::binary);
    output << metadata.toJson();
    BOOST_REQUIRE(output.good());
  }
  BOOST_REQUIRE_EQUAL(std::filesystem::file_size(sidecar), bytes);
  std::string{}.swap(metadata.policyEpoch);
  CatalogParseScope counts;
  // Size admission fails before parsing. Do not depend on strerror's locale.
  BOOST_CHECK_EXCEPTION(store->listManifests(), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()).find(
        "repo-file-metadata-read-failed: size-limit ") == 0;
    });
  BOOST_CHECK_EQUAL(catalogParses.load(), 0U);
}

BOOST_AUTO_TEST_CASE(JsonQuotingPreservesLegacyBytesAndLargeMetadata)
{
  RepoObjectManifest metadata;
  metadata.objectName = "/actual/producer/serialization";
  metadata.policyEpoch = "a\\b\"c\n\r\t";
  metadata.keyReferenceId = std::string("\0\x1b\xff", 3);
  metadata.operationId = "utf8-\xc3\xa9";
  const auto encoded = metadata.toJson();
  BOOST_CHECK_NE(encoded.find("\"policyEpoch\":\"a\\\\b\\\"c\\n\\r\\t\""), std::string::npos);
  const auto opaqueExpected = std::string("\"keyReferenceId\":\"") +
    metadata.keyReferenceId + "\"";
  BOOST_CHECK_NE(encoded.find(opaqueExpected), std::string::npos);
  BOOST_CHECK_NE(encoded.find("\"operationId\":\"utf8-\xc3\xa9\""), std::string::npos);
  metadata.policyEpoch = "epoch";
  metadata.keyReferenceId = "key-reference";
  for (size_t i = 0; i < 128; ++i) {
    auto name = ndn::Name("/actual/producer/serialization")
      .append(std::string(256, 'x')).appendSegment(i);
    metadata.packetNames.push_back(name.toUri(ndn::name::UriFormat::CANONICAL));
  }
  const auto expected = metadata.toJson();
  BOOST_REQUIRE_GT(expected.size(), 32U * 1024U);
  // Frozen from the old production serializer in both URI profiles.
  BOOST_CHECK_EQUAL(sha256Hex(std::vector<uint8_t>(expected.begin(), expected.end())),
                    "0c3776d01c4ca861a013c664ca09ddafd57fbdad31e16ce775b9d031235932cb");
  BOOST_TEST_MESSAGE("REPO_JSON_QUOTE_BASELINE_SHA256 " <<
    sha256Hex(std::vector<uint8_t>(expected.begin(), expected.end())));
  constexpr size_t repetitions = 1000;
  size_t encodedBytes = 0;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < repetitions; ++i) {
    const auto current = metadata.toJson();
    BOOST_REQUIRE(current == expected);
    encodedBytes += current.size();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - start).count();
  BOOST_TEST_MESSAGE("REPO_JSON_QUOTE_BENCH repetitions=" << repetitions <<
    " encodedBytes=" << encodedBytes << " elapsed_us=" << elapsed);
}

BOOST_AUTO_TEST_CASE(DurableEnvelopeReleasePreservesIndependentSourcesAndRestart)
{
  using namespace ndn_service_framework;
  Fixture fixture;
  const std::vector<uint8_t> bytes{11, 22, 33, 44};
  const auto inputPath = fixture.root / "envelope-input.bin";
  {
    std::ofstream input(inputPath, std::ios::binary);
    input.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    BOOST_REQUIRE(input.good());
  }
  EncryptedLargeDataCommitOptions options;
  options.retention = EncryptedLargeDataRetention::Durable;
  options.publicationIdentity = "spec204-durable-envelope";
  options.protectionEpoch = "epoch1";
  options.keyReferenceId = "key1";
  options.keyReferenceVersion = "v1";
  options.ciphertextManifestDigest = std::string(64, 'c');
  options.servingLocator = "/actual/envelope-producer";
  options.contentDigest = std::string(64, 'd');
  options.plaintextSize = bytes.size();
  const std::string name = "/actual/envelope-producer/durable";
  std::string persistedManifest;
  {
    auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("envelope"));
    RepoEncryptedLargeDataStore backend(repo);
    auto original = backend.commitFile(name, inputPath, bytes.size(), options);
    BOOST_REQUIRE(original);
    BOOST_REQUIRE(original->isDurable());
    BOOST_CHECK_THROW(original->requireCurrentDurableStorageOwner(ndn::Name(name), "/owner/u"), std::runtime_error);
    auto shared = original;
    auto first = backend.lookupDurable(options.publicationIdentity);
    auto second = backend.lookupDurable(options.publicationIdentity);
    BOOST_REQUIRE(first);
    BOOST_REQUIRE(second);
    BOOST_CHECK(first->source != second->source);
    persistedManifest = repo->getManifest(name).toJson();
    original->release();
    original->release();
    original.reset();
    shared.reset();
    BOOST_CHECK(repo->has(name));
    BOOST_CHECK(first->source->read(0, bytes.size()) == bytes);
    first->source->release();
    first->source->release();
    first.reset();
    BOOST_CHECK(second->source->read(0, bytes.size()) == bytes);
    BOOST_CHECK_EQUAL(repo->getManifest(name).toJson(), persistedManifest);

    const std::string transientName = "/actual/envelope-producer/transient";
    auto transient = backend.commitFile(transientName, inputPath, bytes.size());
    BOOST_REQUIRE(transient);
    BOOST_CHECK(!transient->isDurable());
    BOOST_CHECK(repo->has(transientName));
    transient->release();
    transient->release();
    BOOST_CHECK(!repo->has(transientName));
    BOOST_CHECK(second->source->read(0, bytes.size()) == bytes);
    BOOST_CHECK_EQUAL(repo->getManifest(name).toJson(), persistedManifest);
  }
  {
    auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("envelope"));
    RepoEncryptedLargeDataStore backend(repo);
    auto restored = backend.lookupDurable(options.publicationIdentity);
    BOOST_REQUIRE(restored);
    BOOST_CHECK(restored->source->isDurable());
    BOOST_CHECK(restored->source->read(0, bytes.size()) == bytes);
    restored->source->release();
    BOOST_CHECK(restored->source->read(0, bytes.size()) == bytes);
    BOOST_CHECK_EQUAL(repo->getManifest(name).toJson(), persistedManifest);
  }
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

BOOST_AUTO_TEST_SUITE(Spec204OwnedLiveStore)

BOOST_AUTO_TEST_CASE(PersistentOriginalServingUsesPhysicalSharedClaimsAndPreservesWire)
{
  OwnedAdapterFixture f; PacketSet packets; signPacketSetAsProducer(packets, f.keys);
  const auto root = packets.manifest.getName();
  const auto objectName = root.toUri(ndn::name::UriFormat::ALTERNATE);
  auto adapter = f.node->createEncryptedLargeDataStore();
  const auto u = f.authorize(packets.options, root, "/owner/u", "u1");
  auto uSource = OwnedAdapterFixture::commit(*adapter, packets, u);
  auto hit = adapter->lookupDurable(packets.options.publicationIdentity);
  BOOST_REQUIRE(hit);
  const auto v = f.authorize(packets.options, root, "/owner/v", "v1");
  auto vSource = adapter->retainDurableSource(*hit, v);
  for (const auto& packet : packets.packets) {
    const auto physical = f.node->core().lookupDataPacketObject(packet.first.toUri());
    BOOST_REQUIRE(physical);
    BOOST_CHECK_EQUAL(physical->objectName, objectName);
    BOOST_CHECK_EQUAL(physical->objectType, "original-packet-set");
    BOOST_CHECK_EQUAL(physical->size, packets.options.packetWireBytes);
  }
  BOOST_CHECK(!f.node->core().lookupDataPacketObject("/unknown/original"));
  BOOST_CHECK_EQUAL(f.node->core().catalogLookup(packets.packets.back().first.toUri()).manifest.objectName,
                    packets.packets.back().first.toUri(ndn::name::UriFormat::CANONICAL));
  f.face.processEvents(ndn::time::milliseconds(10));
  const auto registrationsBegin = f.face.sentInterests.size();
  BOOST_CHECK_NO_THROW(f.node->registerOriginalDataServing(f.face));
  BOOST_CHECK_THROW(f.node->registerOriginalDataServing(f.face), std::logic_error);
  f.face.processEvents(ndn::time::milliseconds(10));
  size_t routes = 0;
  const ndn::Name registrationCommand("/localhost/nfd/rib/register");
  for (auto i = registrationsBegin; i < f.face.sentInterests.size(); ++i) {
    const auto& name = f.face.sentInterests[i].getName();
    if (!registrationCommand.isPrefixOf(name) || name.size() <= 4) continue;
    ndn::nfd::ControlParameters parameters;
    parameters.wireDecode(name[4].blockFromValue());
    BOOST_CHECK(parameters.getName() == ndn::Name("/owned/provider"));
    ++routes;
  }
  BOOST_CHECK_EQUAL(routes, 1U);
  const auto request = [&](const ndn::Interest& interest, size_t count,
                           const std::vector<uint8_t>& wire) {
    f.face.getIoContext().restart(); const auto before = f.face.sentData.size();
    f.face.receive(interest); f.face.processEvents(ndn::time::milliseconds(1));
    waitForOriginalData(f.face, f.node.get());
    BOOST_REQUIRE_EQUAL(f.face.sentData.size(), before + count);
    if (count == 0) return;
    const auto& actual = f.face.sentData.back(); const auto& encoded = actual.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(encoded.begin(), encoded.end()) == wire);
    BOOST_CHECK(interest.matchesData(actual));
    BOOST_CHECK(ndn::security::verifySignature(actual,
      f.keys.getPib().getIdentity(packets.manifest.producer).getDefaultKey().getDefaultCertificate()));
  };
  for (const auto& packet : packets.packets) {
    request(ndn::Interest(packet.first), 1, packet.second);
    const ndn::Data original(ndn::Block(ndn::span<const uint8_t>(packet.second.data(), packet.second.size())));
    request(ndn::Interest(original.getFullName()), 1, packet.second);
  }
  request(ndn::Interest(root).setCanBePrefix(true), 1, packets.packets.front().second);
  auto wrongDigest = root;
  const std::vector<uint8_t> zeros(32, 0);
  wrongDigest.appendImplicitSha256Digest(ndn::span<const uint8_t>(zeros.data(), zeros.size()));
  request(ndn::Interest(wrongDigest), 0, {});
  request(ndn::Interest(packets.manifest.producer).setCanBePrefix(true), 0, {});
  const auto ownerlessName = packetName();
  f.node->core().putDataPacket(ownerlessName.toUri(), packet(ownerlessName));
  request(ndn::Interest(ownerlessName), 0, {});
  const auto charge = f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes;
  f.journal->prepareUserStorageCleanup("/owner/u", f.capability.repoNode, "serving-u",
    objectName, u.storageActor->grant.ownerEpoch, f.now());
  for (const auto& packet : packets.packets) request(ndn::Interest(packet.first), 1, packet.second);
  f.journal->prepareUserStorageCleanup("/owner/v", f.capability.repoNode, "serving-v",
    objectName, v.storageActor->grant.ownerEpoch, f.now());
  BOOST_CHECK(f.node->core().hasDataPacket(root.toUri()));
  for (const auto& packet : packets.packets) request(ndn::Interest(packet.first), 0, {});
  BOOST_CHECK_EQUAL(f.remove("/owner/u", root, u.storageActor->grant.ownerEpoch, "serving-u").state, "DELETED");
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, charge);
  BOOST_CHECK_EQUAL(f.remove("/owner/v", root, v.storageActor->grant.ownerEpoch, "serving-v").state, "DELETED");
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, 0U);
  request(ndn::Interest(root), 0, {});
  const auto fresh = f.authorize(packets.options, root, "/owner/u", "u2");
  BOOST_CHECK_GT(fresh.storageActor->grant.ownerEpoch, u.storageActor->grant.ownerEpoch);
  auto freshSource = OwnedAdapterFixture::commit(*adapter, packets, fresh);
  request(ndn::Interest(root), 1, packets.packets.front().second);
  BOOST_CHECK_THROW(uSource->readOriginalDataPacket(root), std::runtime_error);
  uSource.reset(); vSource.reset(); freshSource.reset(); hit.reset(); adapter.reset();
  const std::weak_ptr<RepoNode> oldNode = f.node;
  f.close(); BOOST_CHECK(oldNode.expired());
  f.face.processEvents(ndn::time::milliseconds(10));
  request(ndn::Interest(root), 0, {});
  f.reopen(); f.node->registerOriginalDataServing(f.face);
  f.face.processEvents(ndn::time::milliseconds(10));
  request(ndn::Interest(root), 1, packets.packets.front().second);
  BOOST_CHECK_EQUAL(f.remove("/owner/u", root, fresh.storageActor->grant.ownerEpoch, "serving-reopen").state,
                    "DELETED");
  request(ndn::Interest(root), 0, {});
}

BOOST_AUTO_TEST_CASE(SharedOriginalPacketServingSelectsCurrentHolderBeforeReading)
{
  OwnedAdapterFixture f;
  const ndn::Name producer("/actual/shared-key-producer");
  f.keys.createIdentity(producer, ndn::RsaKeyParams(2048));
  ndn::Data key(ndn::Name(producer).append("KEY").append("shared-original").appendVersion(1));
  key.setContent(toBytes("opaque encrypted key packet"));
  f.keys.sign(key, ndn::security::signingByIdentity(producer));
  const auto& encoded = key.wireEncode();
  const std::vector<uint8_t> keyWire(encoded.begin(), encoded.end());
  auto adapter = f.node->createEncryptedLargeDataStore();
  f.node->registerOriginalDataServing(f.face);
  f.face.processEvents(ndn::time::milliseconds(10));
  const auto request = [&](const ndn::Interest& interest, size_t count) {
    f.face.getIoContext().restart(); const auto before = f.face.sentData.size();
    f.face.receive(interest); f.face.processEvents(ndn::time::milliseconds(1));
    waitForOriginalData(f.face, f.node.get());
    BOOST_REQUIRE_EQUAL(f.face.sentData.size(), before + count);
    if (count == 0) return;
    const auto& actual = f.face.sentData.back(); const auto& wire = actual.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == keyWire);
    BOOST_CHECK(interest.matchesData(actual));
    BOOST_CHECK(ndn::security::verifySignature(actual,
      f.keys.getPib().getIdentity(producer).getDefaultKey().getDefaultCertificate()));
  };
  for (const bool bothFenced : {false, true}) {
    const std::string prefix = bothFenced ? "both" : "one";
    PacketSet a(producer.toUri(), prefix + "-a"), b(producer.toUri(), prefix + "-b");
    signPacketSetAsProducer(a, f.keys); signPacketSetAsProducer(b, f.keys);
    for (auto* packets : {&a, &b}) {
      packets->packets.emplace_back(key.getName(), keyWire);
      packets->options.auxiliaryPacketNames = {key.getName()};
      packets->options.packetWireBytes += keyWire.size();
    }
    const auto ao = f.authorize(a.options, a.manifest.getName(), "/owner/u", prefix + "-u");
    const auto bo = f.authorize(b.options, b.manifest.getName(), "/owner/v", prefix + "-v");
    auto as = OwnedAdapterFixture::commit(*adapter, a, ao);
    auto bs = OwnedAdapterFixture::commit(*adapter, b, bo);
    const auto holder = f.node->core().lookupDataPacketObject(key.getName().toUri());
    BOOST_REQUIRE(holder);
    const bool firstIsA = ndn::Name(holder->objectName) == a.manifest.getName();
    BOOST_REQUIRE(firstIsA || ndn::Name(holder->objectName) == b.manifest.getName());
    const auto firstRoot = firstIsA ? a.manifest.getName() : b.manifest.getName();
    const auto secondRoot = firstIsA ? b.manifest.getName() : a.manifest.getName();
    const auto firstOwner = firstIsA ? "/owner/u" : "/owner/v";
    const auto secondOwner = firstIsA ? "/owner/v" : "/owner/u";
    const auto firstEpoch = (firstIsA ? ao : bo).storageActor->grant.ownerEpoch;
    const auto secondEpoch = (firstIsA ? bo : ao).storageActor->grant.ownerEpoch;
    const auto firstCharge = f.journal->userStorageUsage(firstOwner, f.capability.repoNode).committedBytes;
    const auto secondCharge = f.journal->userStorageUsage(secondOwner, f.capability.repoNode).committedBytes;
    request(ndn::Interest(key.getName()), 1);
    f.journal->prepareUserStorageCleanup(firstOwner, f.capability.repoNode, prefix + "-first",
      firstRoot.toUri(), firstEpoch, f.now());
    request(ndn::Interest(key.getName()), 1);
    request(ndn::Interest(key.getFullName()), 1);
    ndn::Interest bootstrap(key.getName().getPrefix(-1));
    bootstrap.setCanBePrefix(true);
    request(bootstrap, 1); // select the live shared holder, preserving original wire
    bootstrap.setCanBePrefix(false);
    request(bootstrap, 0);
    auto wrongDigest = key.getName(); const std::vector<uint8_t> zeros(32, 0);
    wrongDigest.appendImplicitSha256Digest(ndn::span<const uint8_t>(zeros.data(), zeros.size()));
    request(ndn::Interest(wrongDigest), 0);
    if (bothFenced) {
      f.journal->prepareUserStorageCleanup(secondOwner, f.capability.repoNode, prefix + "-second",
        secondRoot.toUri(), secondEpoch, f.now());
      request(ndn::Interest(key.getName()), 0);
      bootstrap.setCanBePrefix(true); request(bootstrap, 0);
      BOOST_CHECK_EQUAL(f.journal->userStorageUsage(firstOwner, f.capability.repoNode).committedBytes, firstCharge);
      BOOST_CHECK_EQUAL(f.journal->userStorageUsage(secondOwner, f.capability.repoNode).committedBytes, secondCharge);
    }
    BOOST_CHECK_EQUAL(f.remove(firstOwner, firstRoot, firstEpoch, prefix + "-first").state, "DELETED");
    request(ndn::Interest(key.getName()), bothFenced ? 0 : 1);
    if (!bothFenced) {
      f.journal->prepareUserStorageCleanup(secondOwner, f.capability.repoNode, prefix + "-second",
        secondRoot.toUri(), secondEpoch, f.now());
      request(ndn::Interest(key.getName()), 0);
      BOOST_CHECK_EQUAL(f.journal->userStorageUsage(secondOwner, f.capability.repoNode).committedBytes, secondCharge);
    }
    BOOST_CHECK_EQUAL(f.remove(secondOwner, secondRoot, secondEpoch, prefix + "-second").state, "DELETED");
    request(ndn::Interest(key.getName()), 0);
    BOOST_CHECK_EQUAL(f.journal->userStorageUsage(firstOwner, f.capability.repoNode).committedBytes, 0U);
    BOOST_CHECK_EQUAL(f.journal->userStorageUsage(secondOwner, f.capability.repoNode).committedBytes, 0U);
  }
}

BOOST_AUTO_TEST_CASE(PersistentOriginalServingRejectsIncompletePhysicalMetadata)
{
  OwnedAdapterFixture f; PacketSet packets; signPacketSetAsProducer(packets, f.keys);
  const auto root = packets.manifest.getName();
  auto adapter = f.node->createEncryptedLargeDataStore();
  auto source = OwnedAdapterFixture::commit(*adapter, packets,
    f.authorize(packets.options, root, "/owner/u", "u1"));
  const auto charge = f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes;
  const auto physical = f.node->core().lookupDataPacketObject(root.toUri());
  BOOST_REQUIRE(physical); BOOST_REQUIRE_EQUAL(physical->generation, 1U);
  BOOST_REQUIRE(!physical->operationId.empty());
  const auto metadata = f.files.root / "owned" / "manifests" /
    (sha256Hex(toBytes(root.toUri(ndn::name::UriFormat::ALTERNATE))) + ".json");
  std::ifstream input(metadata, std::ios::binary);
  const std::string original(std::istreambuf_iterator<char>(input), {});
  input.close();
  const auto write = [&](const std::string& bytes) {
    std::ofstream output(metadata, std::ios::binary | std::ios::trunc); output << bytes;
    BOOST_REQUIRE(output.good());
  };
  const auto request = [&](size_t count) {
    f.face.getIoContext().restart(); const auto before = f.face.sentData.size();
    f.face.receive(ndn::Interest(root)); f.face.processEvents(ndn::time::milliseconds(1));
    waitForOriginalData(f.face, f.node.get());
    BOOST_REQUIRE_EQUAL(f.face.sentData.size(), before + count);
    if (count == 1) {
      const auto& wire = f.face.sentData.back().wireEncode();
      BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == packets.packets.front().second);
    }
    BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, charge);
  };
  f.node->registerOriginalDataServing(f.face); f.face.processEvents(ndn::time::milliseconds(10));
  request(1);
  for (const bool zeroGeneration : {true, false}) {
    auto changed = *physical;
    if (zeroGeneration) changed.generation = 0;
    else changed.operationId.clear();
    auto encoded = changed.toJson(); BOOST_REQUIRE_LE(encoded.size(), original.size());
    encoded.append(original.size() - encoded.size(), ' '); write(encoded);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(metadata), original.size());
    const auto probe = f.node->core().probeObjectMetadata(physical->objectName);
    BOOST_REQUIRE(probe.manifestPresent);
    if (zeroGeneration) BOOST_CHECK_EQUAL(probe.manifest.generation, 0U);
    else BOOST_CHECK(probe.manifest.operationId.empty());
    request(0); write(original); request(1);
  }
}

BOOST_AUTO_TEST_CASE(ExactActorAdmissionPreservesSignedPacketWireAndMeasuredCharge)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto adapter = f.node->createEncryptedLargeDataStore();
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, packets.options), std::runtime_error);
  auto options = f.authorize(packets.options, root, "/owner/u", "u1");
  auto wrong = options;
  wrong.storageActor->authenticatedWriterIdentity = "/alien/writer";
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, wrong), std::runtime_error);
  wrong = options; ++wrong.storageActor->grant.ownerEpoch;
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, wrong), std::runtime_error);
  wrong = options; wrong.storageActor->grant.repoNode = "/other/node";
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, wrong), std::runtime_error);
  const auto expired = f.journal->grantUserStorage("/owner/u", f.capability.repoNode,
    "expired", "/owned/writer", root.toUri(ndn::name::UriFormat::ALTERNATE),
    OwnedAdapterFixture::now() - 1000, OwnedAdapterFixture::now() - 2000);
  wrong = options;
  wrong.storageActor->grant.grantId = expired.grantId;
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, wrong), std::runtime_error);
  BOOST_CHECK(f.node->core().list().empty());
  BOOST_CHECK(std::filesystem::is_empty(f.files.root / "owned" / "staging"));
  auto source = OwnedAdapterFixture::commit(*adapter, packets, options);
  BOOST_CHECK_NO_THROW(source->requireCurrentDurableStorageOwner(root, "/owner/u"));
  BOOST_CHECK_NO_THROW(source->requireCurrentDurableStorageOwner(
    ndn::Name(root.toUri(ndn::name::UriFormat::CANONICAL)), "/owner/u"));
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(root, "/owner/v"), std::runtime_error);
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(ndn::Name("/wrong/manifest"), "/owner/u"), std::runtime_error);
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(ndn::Name(), "/owner/u"), std::runtime_error);
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(root, ""), std::runtime_error);
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(root, "/"), std::runtime_error);
  BOOST_CHECK_THROW(source->requireCurrentDurableStorageOwner(root, "/8=owner/8=u"), std::runtime_error);
  const auto key = root.toUri(ndn::name::UriFormat::ALTERNATE);
  const auto probe = f.node->core().probeObjectMetadata(key);
  BOOST_REQUIRE(probe.manifestPresent);
  BOOST_CHECK_EQUAL(probe.manifest.objectName, key);
  BOOST_CHECK_EQUAL(probe.manifest.generation, 1U);
  BOOST_CHECK_EQUAL(probe.manifest.packetNames.front(), root.toUri(ndn::name::UriFormat::CANONICAL));
  BOOST_CHECK_EQUAL(probe.payloadSize, packets.options.packetWireBytes);
  const auto usage = f.journal->userStorageUsage("/owner/u", f.capability.repoNode);
  BOOST_CHECK_EQUAL(usage.committedBytes, probe.payloadSize + probe.manifestWireBytes);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  for (const auto& packet : packets.packets) {
    BOOST_CHECK(source->readOriginalDataPacket(packet.first) == packet.second);
    BOOST_CHECK(f.node->core().getDataPacket(packet.first.toUri()) == packet.second);
  }
  auto again = OwnedAdapterFixture::commit(*adapter, packets, options);
  BOOST_CHECK(again->readOriginalDataPacket(root) == packets.packets.front().second);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes,
                    usage.committedBytes);
}

BOOST_AUTO_TEST_CASE(ProvisionalSharedSourcesHonorOwnerDeleteAndNewEpoch)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto adapter = f.node->createEncryptedLargeDataStore();
  const auto u = f.authorize(packets.options, root, "/owner/u", "u1");
  auto uSource = OwnedAdapterFixture::commit(*adapter, packets, u);
  auto candidate = adapter->lookupDurable(packets.options.publicationIdentity);
  BOOST_REQUIRE(candidate);
  BOOST_CHECK(candidate->source->isDurable());
  BOOST_CHECK_THROW(candidate->source->requireCurrentDurableStorageOwner(root, "/owner/u"), std::runtime_error);
  BOOST_CHECK_THROW(candidate->source->readOriginalDataPacket(root), std::runtime_error);
  const auto v = f.authorize(packets.options, root, "/owner/v", "v1");
  auto vSource = adapter->retainDurableSource(*candidate, v);
  BOOST_CHECK_NO_THROW(vSource->requireCurrentDurableStorageOwner(root, "/owner/v"));
  const auto charge = f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes;
  BOOST_CHECK_EQUAL(charge, f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes);
  auto vAgain = adapter->retainDurableSource(*candidate, v);
  BOOST_CHECK(vAgain->readOriginalDataPacket(root) == packets.packets.front().second);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, charge);
  (void)f.journal->prepareUserStorageCleanup("/owner/u", f.capability.repoNode, "delete-u",
    root.toUri(ndn::name::UriFormat::ALTERNATE), u.storageActor->grant.ownerEpoch, OwnedAdapterFixture::now());
  BOOST_CHECK_THROW(uSource->readOriginalDataPacket(root), std::runtime_error);
  BOOST_CHECK_THROW(uSource->requireCurrentDurableStorageOwner(root, "/owner/u"), std::runtime_error);
  BOOST_CHECK_NO_THROW(vSource->requireCurrentDurableStorageOwner(root, "/owner/v"));
  BOOST_CHECK(vSource->readOriginalDataPacket(root) == packets.packets.front().second);
  BOOST_CHECK_EQUAL(f.journal->liveUserStorageClaimsForObject(f.capability.repoNode,
    root.toUri(ndn::name::UriFormat::ALTERNATE)).size(), 2U);
  BOOST_CHECK_EQUAL(f.remove("/owner/u", root, u.storageActor->grant.ownerEpoch, "delete-u").state, "DELETED");
  BOOST_CHECK_THROW(uSource->readOriginalDataPacket(root), std::runtime_error);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, charge);
  const auto freshU = f.authorize(packets.options, root, "/owner/u", "u2");
  BOOST_CHECK_GT(freshU.storageActor->grant.ownerEpoch, u.storageActor->grant.ownerEpoch);
  auto newUSource = adapter->retainDurableSource(*candidate, freshU);
  BOOST_CHECK_NO_THROW(newUSource->requireCurrentDurableStorageOwner(root, "/owner/u"));
  BOOST_CHECK_THROW(uSource->requireCurrentDurableStorageOwner(root, "/owner/u"), std::runtime_error);
  BOOST_CHECK(newUSource->readOriginalDataPacket(root) == packets.packets.front().second);
  BOOST_CHECK_THROW(uSource->readOriginalDataPacket(root), std::runtime_error);
  BOOST_CHECK(vSource->readOriginalDataPacket(root) == packets.packets.front().second);
}

BOOST_AUTO_TEST_CASE(RetentionFsyncFailureRetainsReservationUntilActualRetry)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto adapter = f.node->createEncryptedLargeDataStore();
  auto uSource = OwnedAdapterFixture::commit(*adapter, packets,
    f.authorize(packets.options, root, "/owner/u", "u1"));
  const auto candidate = adapter->lookupDurable(packets.options.publicationIdentity);
  BOOST_REQUIRE(candidate);
  const auto v = f.authorize(packets.options, root, "/owner/v", "v1");
  const auto charge = f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes;
  {
    RejectDirectorySync fault;
    BOOST_CHECK_THROW(adapter->retainDurableSource(*candidate, v), std::runtime_error);
  }
  const auto pending = f.journal->userStorageUsage("/owner/v", f.capability.repoNode);
  BOOST_CHECK_EQUAL(pending.committedBytes, 0U);
  BOOST_CHECK_EQUAL(pending.reservedBytes, charge);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, charge);
  BOOST_CHECK(uSource->readOriginalDataPacket(root) == packets.packets.front().second);
  auto recovered = adapter->retainDurableSource(*candidate, v);
  BOOST_CHECK(recovered->readOriginalDataPacket(root) == packets.packets.front().second);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, charge);
}

BOOST_AUTO_TEST_CASE(SharedHolderDriftRejectsNewOwnerBeforeMetadataRewrite)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto adapter = f.node->createEncryptedLargeDataStore();
  auto source = OwnedAdapterFixture::commit(*adapter, packets,
    f.authorize(packets.options, root, "/owner/u", "u1"));
  const auto candidate = adapter->lookupDurable(packets.options.publicationIdentity);
  BOOST_REQUIRE(candidate);
  const auto v = f.authorize(packets.options, root, "/owner/v", "v1");
  const auto metadata = f.files.root / "owned" / "manifests" /
    (sha256Hex(toBytes(root.toUri(ndn::name::UriFormat::ALTERNATE))) + ".json");
  const auto read = [&] {
    std::ifstream input(metadata, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto original = read();
  {
    std::ofstream output(metadata, std::ios::binary);
    output << " \n" << original;
    BOOST_REQUIRE(output.good());
  }
  BOOST_CHECK_THROW(adapter->retainDurableSource(*candidate, v), std::runtime_error);
  BOOST_CHECK_EQUAL(read(), " \n" + original);
  BOOST_CHECK_THROW(source->readOriginalDataPacket(root), std::runtime_error);
  {
    std::ofstream output(metadata, std::ios::binary); output << original;
    BOOST_REQUIRE(output.good());
  }
  sqlite3* raw = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open(f.database.c_str(), &raw), SQLITE_OK);
  const auto result = sqlite3_exec(raw,
    "UPDATE repo_user_storage_claims SET manifest_bytes=manifest_bytes+1 WHERE owner_identity='/owner/u'",
    nullptr, nullptr, nullptr);
  sqlite3_close(raw);
  BOOST_REQUIRE_EQUAL(result, SQLITE_OK);
  BOOST_CHECK_THROW(adapter->retainDurableSource(*candidate, v), std::runtime_error);
  BOOST_CHECK_EQUAL(read(), original);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/v", f.capability.repoNode).committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedEnvelopeSurvivesNodeDestructionAndReconciledReopen)
{
  OwnedAdapterFixture f;
  {
    RepoNode transient(ndn::Name("/trusted/service"), f.capability, f.files.store("trusted"));
    BOOST_CHECK_THROW(transient.createEncryptedLargeDataStore(), std::logic_error);
  }
  PacketSet profile;
  auto options = profile.options;
  options.packetStorageFormat = "encrypted-envelope-bytes";
  const ndn::Name name("/owned/producer/envelope");
  const std::vector<uint8_t> bytes{4, 5, 6, 7};
  const auto input = f.files.root / "input.bin";
  {
    std::ofstream file(input, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    BOOST_REQUIRE(file.good());
  }
  options = f.authorize(options, name, "/owner/u", "u1");
  auto adapter = f.node->createEncryptedLargeDataStore();
  auto source = adapter->commitFile(name.toUri(), input, bytes.size(), options);
  const auto probe = f.node->core().probeObjectMetadata(name.toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes,
                    bytes.size() + probe.manifestWireBytes);
  std::weak_ptr<RepoNode> node = f.node;
  f.close(); adapter.reset();
  BOOST_CHECK(node.expired());
  BOOST_CHECK(source->read(0, bytes.size()) == bytes);
  source->release(); source->release();
  BOOST_CHECK(source->read(0, bytes.size()) == bytes);
  source.reset();
  f.reopen();
  adapter = f.node->createEncryptedLargeDataStore();
  const auto provisional = adapter->lookupDurable(options.publicationIdentity);
  BOOST_REQUIRE(provisional);
  BOOST_CHECK_THROW(provisional->source->read(0, 1), std::runtime_error);
  auto restored = adapter->retainDurableSource(*provisional, options);
  BOOST_CHECK(restored->read(0, bytes.size()) == bytes);
  BOOST_CHECK_EQUAL(f.remove("/owner/u", name, options.storageActor->grant.ownerEpoch, "cleanup").state, "DELETED");
  BOOST_CHECK_THROW(restored->read(0, bytes.size()), std::runtime_error);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(ReservedQuotaRejectsRealPacketSpoolBeforeAnyPersistentRange)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  BOOST_REQUIRE(f.node->core().list().empty());
  BOOST_REQUIRE(f.journal->reconcileUserStorageInventory(f.capability.repoNode, {},
    OwnedAdapterFixture::now()).ready);
  // An independently admitted pending write occupies quota before any payload
  // arrives. This is a ledger reservation fixture, not a measured 1 GB upload.
  const auto grant = f.journal->grantUserStorage("/owner/u", f.capability.repoNode,
    "pending", "/owned/writer", "/owned/pending", OwnedAdapterFixture::now() + 60000,
    OwnedAdapterFixture::now());
  (void)f.journal->reserveUserStorage("/owned/writer", f.capability.repoNode, "/owner/u",
    grant.grantId, "pending-op", {"/owned/pending", std::string(64, 'b'), 1,
      REPO_USER_STORAGE_LIMIT_BYTES - 1, 1}, OwnedAdapterFixture::now());
  auto options = f.authorize(packets.options, packets.manifest.getName(), "/owner/u", "u1");
  auto adapter = f.node->createEncryptedLargeDataStore();
  BOOST_CHECK_EXCEPTION(OwnedAdapterFixture::commit(*adapter, packets, options), std::runtime_error,
    [](const auto& error) { return std::string(error.what()).find("QUOTA_EXCEEDED") == 0; });
  BOOST_CHECK(f.node->core().list().empty());
  BOOST_CHECK(std::filesystem::is_empty(f.files.root / "owned" / "staging"));
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).reservedBytes,
                    REPO_USER_STORAGE_LIMIT_BYTES);
  BOOST_CHECK_EQUAL(f.journal->pendingUserStorageWrites(f.capability.repoNode).size(), 1U);
}

BOOST_AUTO_TEST_CASE(CancelledPacketStagingRefundsOnlyAfterPhysicalAbsence)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto options = f.authorize(packets.options, root, "/owner/u", "u1");
  auto adapter = f.node->createEncryptedLargeDataStore();
  std::vector<uint8_t> spool;
  for (const auto& packet : packets.packets) spool.insert(spool.end(), packet.second.begin(), packet.second.end());
  const auto stage = f.files.root / "owned" / "staging" / (sha256Hex(spool) + ".1.part");
  bool reachedStage = false;
  const auto cancel = [&] {
    if (std::filesystem::exists(stage)) {
      reachedStage = true;
      throw std::runtime_error("test cancellation after actual staging");
    }
  };
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, options, cancel), std::runtime_error);
  BOOST_CHECK(reachedStage);
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK(f.node->core().list().empty());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, options), std::runtime_error);
  BOOST_CHECK(f.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(FailedPacketStageRemovalKeepsQuotaUntilOwnerCleanup)
{
  OwnedAdapterFixture f;
  PacketSet packets;
  const auto root = packets.manifest.getName();
  auto options = f.authorize(packets.options, root, "/owner/u", "u1");
  auto adapter = f.node->createEncryptedLargeDataStore();
  std::vector<uint8_t> spool;
  for (const auto& packet : packets.packets) spool.insert(spool.end(), packet.second.begin(), packet.second.end());
  const auto directory = f.files.root / "owned" / "staging";
  const auto stage = directory / (sha256Hex(spool) + ".1.part");
  struct RestoreMode
  {
    std::filesystem::path path;
    mode_t original;
    ~RestoreMode() { ::chmod(path.c_str(), original); }
  };
  struct stat value{};
  BOOST_REQUIRE_EQUAL(::stat(directory.c_str(), &value), 0);
  RestoreMode restore{directory, value.st_mode & 07777};
  bool reachedStage = false;
  const auto failRemoval = [&] {
    if (std::filesystem::exists(stage)) {
      reachedStage = true;
      if (::chmod(directory.c_str(), 0500) != 0) throw std::runtime_error("test chmod failed");
      throw std::runtime_error("test cancellation with actual unlink failure");
    }
  };
  BOOST_REQUIRE_NE(::getuid(), 0U);
  BOOST_CHECK_THROW(OwnedAdapterFixture::commit(*adapter, packets, options, failRemoval), std::runtime_error);
  BOOST_CHECK(reachedStage);
  BOOST_CHECK(std::filesystem::exists(stage));
  BOOST_CHECK_GT(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_REQUIRE_EQUAL(::chmod(directory.c_str(), restore.original), 0);
  BOOST_CHECK_EQUAL(f.remove("/owner/u", root, options.storageActor->grant.ownerEpoch, "failed-cleanup").state,
                    "DELETED");
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/owner/u", f.capability.repoNode).committedBytes, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
