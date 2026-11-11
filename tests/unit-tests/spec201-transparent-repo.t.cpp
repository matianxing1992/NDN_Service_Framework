#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoPacketServer.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "NDNSF-DistributedRepo/src/packet-store-lookup.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"
#include "ndn-service-framework/ImmutableFilePublication.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

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

struct HeldMetadataFsync
{
  static HeldMetadataFsync* current;
  std::atomic<bool> hold{true};
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  detail::FilesystemRepoStoreIoHooks previous;

  static int
  fsync(int fd)
  {
    if (current->hold.exchange(false)) {
      current->entered.set_value();
      current->released.wait();
    }
    return ::fsync(fd);
  }

  HeldMetadataFsync()
  {
    current = this;
    previous = detail::installFilesystemRepoStoreIoHooks({fsync, nullptr, nullptr});
  }

  ~HeldMetadataFsync()
  {
    detail::installFilesystemRepoStoreIoHooks(previous);
    current = nullptr;
  }
};

HeldMetadataFsync* HeldMetadataFsync::current = nullptr;

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
  mutable size_t usageScans = 0;
  mutable size_t rangeReads = 0;
  mutable size_t existenceProbes = 0;
  std::function<void(std::vector<uint8_t>&)> afterRangeRead;
  bool concurrentReads = true;
  std::optional<RepoObjectManifest> extraCatalogEntry;
  void put(const RepoObjectManifest& m, std::vector<uint8_t> p) override { delegate->put(m, std::move(p)); }
  void putManifest(const RepoObjectManifest& m) override { delegate->putManifest(m); }
  StoredObject get(const std::string& n) const override { return delegate->get(n); }
  bool has(const std::string& n) const override
  { ++existenceProbes; return delegate->has(n); }
  bool erase(const std::string& n) override { return delegate->erase(n); }
  size_t size() const override { return delegate->size(); }
  uint64_t usedBytes() const override
  {
    ++usageScans;
    return delegate->usedBytes();
  }
  std::vector<RepoObjectManifest> listManifests() const override
  {
    ++enumerations;
    auto result = delegate->listManifests();
    if (extraCatalogEntry) result.insert(result.begin(), *extraCatalogEntry);
    return result;
  }
  bool hasCommittedPayload(const RepoObjectManifest& manifest) const override
  { return delegate->hasCommittedPayload(manifest); }
  void putRange(const RepoObjectManifest& m, RepoByteRange r, const std::vector<uint8_t>& p) override
  { delegate->putRange(m, r, p); }
  void commitRanges(const RepoObjectManifest& m) override { delegate->commitRanges(m); }
  std::vector<uint8_t> getRange(const std::string& n, RepoByteRange r) const override
  {
    ++rangeReads;
    auto bytes = delegate->getRange(n, r);
    if (afterRangeRead) afterRangeRead(bytes);
    return bytes;
  }
  RepoObjectManifest getManifest(const std::string& n) const override { return delegate->getManifest(n); }
  RepoObjectMetadataProbe probeObjectMetadata(const std::string& n) const override
  { return delegate->probeObjectMetadata(n); }
  bool supportsRange() const noexcept override { return delegate->supportsRange(); }
  bool supportsManifestLookup() const noexcept override { return delegate->supportsManifestLookup(); }
  bool supportsConcurrentCommittedReads() const noexcept override
  { return concurrentReads && delegate->supportsConcurrentCommittedReads(); }
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
  explicit PacketSet(const std::string& producer = "/actual/packet-set-producer",
                     uint64_t segments = 5)
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
    manifest.plaintextBytes = segments * 4000 - 1000;
    manifest.envelopeBytes = segments * 4000;
    manifest.segmentBytes = 4000;
    manifest.segmentCount = segments;
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
      if (segment) data.setFinalBlock(ndn::name::Component::fromSegment(segments - 1));
      keys.sign(data, ndn::security::signingWithSha256());
      const auto& wire = data.wireEncode();
      packets.emplace_back(name, std::vector<uint8_t>(wire.begin(), wire.end()));
    };
    const auto payload = manifest.encode();
    append(manifest.getName(), payload, false);
    for (uint64_t i = 0; i < segments; ++i)
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

BOOST_AUTO_TEST_CASE(CommittedCacheHitAndMissReadWhileAnotherObjectWaitsForDurability)
{
  Fixture fixture;
  auto tiered = makeTieredRepoStore(fixture.store("parallel-read"), 8192, "filesystem", 8192);
  RepoNode node(ndn::Name("/SERVICE/repo"), fixture.capability(), tiered);
  const auto store = [&](const char* prefix) {
    const auto name = ndn::Name(prefix).appendSegment(0);
    const auto wire = packet(name, 42, 128);
    RepoDataReference reference;
    reference.objectName = name.toUri();
    reference.dataPrefix = prefix;
    reference.hasFinalSegment = true;
    reference.firstSegment = reference.finalSegment = 0;
    reference.expectedSize = wire.size();
    reference.expectedSha256 = sha256Hex(wire);
    return std::make_pair(reference, wire);
  };
  const auto cached = store("/producer/cached");
  const auto cold = store("/producer/cold");
  const auto incoming = store("/producer/incoming");
  BOOST_REQUIRE_EQUAL(node.insertWirePackets(cached.first, {cached.second}).state, "DONE");
  BOOST_REQUIRE_EQUAL(node.insertWirePackets(cold.first, {cold.second}).state, "DONE");
  BOOST_REQUIRE(node.getDataPacket(ndn::Interest(ndn::Name(cached.first.objectName))) ==
                cached.second);
  HeldMetadataFsync fsync;
  auto entered = fsync.entered.get_future();
  auto writing = std::async(std::launch::async, [&] {
    return node.insertWirePackets(incoming.first, {incoming.second});
  });
  const auto held = entered.wait_for(std::chrono::seconds(2));
  auto hit = std::async(std::launch::async, [&] {
    return node.getDataPacket(ndn::Interest(ndn::Name(cached.first.objectName)));
  });
  auto miss = std::async(std::launch::async, [&] {
    return node.getDataPacket(ndn::Interest(ndn::Name(cold.first.objectName)));
  });
  const auto hitStatus = hit.wait_for(std::chrono::milliseconds(250));
  const auto missStatus = miss.wait_for(std::chrono::milliseconds(250));
  const auto writeStatus = writing.wait_for(std::chrono::milliseconds(0));
  fsync.release.set_value();
  const auto written = writing.get();
  const auto hitWire = hit.get();
  const auto missWire = miss.get();
  BOOST_CHECK(held == std::future_status::ready);
  BOOST_CHECK(hitStatus == std::future_status::ready);
  BOOST_CHECK(missStatus == std::future_status::ready);
  BOOST_CHECK(writeStatus == std::future_status::timeout);
  BOOST_CHECK_EQUAL(written.state, "DONE");
  BOOST_CHECK(hitWire == cached.second);
  BOOST_CHECK(missWire == cold.second);
}

BOOST_AUTO_TEST_CASE(RelatedRangesUseOneBoundedCacheAndObserveCurrentAuthority)
{
  Fixture fixture;
  auto authority = std::make_shared<CountingStore>(fixture.store("range-cache"));
  auto tiered = makeTieredRepoStore(authority, 8192, "filesystem", 8192);
  const std::vector<uint8_t> bytes(4096, 42);
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/range-cache";
  manifest.sha256 = sha256Hex(bytes);
  manifest.size = bytes.size();
  manifest.generation = 1;
  authority->put(manifest, bytes);
  const std::vector<uint8_t> expected(16, 42);
  BOOST_CHECK(tiered->getRange(manifest.objectName, {128, 16}) == expected);
  BOOST_REQUIRE_EQUAL(authority->rangeReads, 1U);
  BOOST_CHECK(tiered->getRange(manifest.objectName, {512, 16}) == expected);
  BOOST_CHECK_EQUAL(authority->rangeReads, 1U);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().hits, 1U);
  BOOST_CHECK_LE(tiered->cacheStatus().usedBytes, 8192U);
  BOOST_CHECK_THROW(tiered->getRange(manifest.objectName, {4096, 1}), std::out_of_range);

  auto second = manifest;
  second.objectName = "/producer/second-range-cache";
  authority->put(second, bytes);
  BOOST_CHECK(tiered->getRange(second.objectName, {0, 16}) == expected);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
  BOOST_CHECK_LE(tiered->cacheStatus().usedBytes, 8192U);
  const auto afterEviction = authority->rangeReads;
  BOOST_CHECK(tiered->getRange(manifest.objectName, {0, 16}) == expected);
  BOOST_CHECK_GT(authority->rangeReads, afterEviction);

  // A changed authority generation cannot be hidden by an existing cache hit.
  const std::vector<uint8_t> replacement(4096, 7);
  manifest.sha256 = sha256Hex(replacement);
  manifest.generation = 2;
  authority->put(manifest, replacement);
  BOOST_CHECK(tiered->getRange(manifest.objectName, {0, 16}) ==
              std::vector<uint8_t>(16, 7));
  BOOST_REQUIRE(authority->erase(manifest.objectName));
  BOOST_CHECK_THROW(tiered->getRange(manifest.objectName, {0, 16}), std::out_of_range);
  tiered.reset();
  tiered = makeTieredRepoStore(authority, 8192, "filesystem", 8192);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 0U);
  BOOST_CHECK(tiered->getRange(second.objectName, {0, 16}) == expected);
}

BOOST_AUTO_TEST_CASE(RangePrefetchRespectsBudgetAndWholeObjectThreshold)
{
  Fixture fixture;
  auto authority = std::make_shared<CountingStore>(fixture.store("range-no-cache"));
  const std::vector<uint8_t> bytes(4096, 42);
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/uncached-range";
  manifest.sha256 = sha256Hex(bytes);
  manifest.size = bytes.size();
  authority->put(manifest, bytes);
  for (const auto budget : {0U, 2048U, 8192U}) {
    auto tiered = makeTieredRepoStore(authority, budget, "filesystem", 2048);
    const auto before = authority->rangeReads;
    BOOST_CHECK(tiered->getRange(manifest.objectName, {0, 16}) ==
                std::vector<uint8_t>(16, 42));
    BOOST_CHECK(tiered->getRange(manifest.objectName, {16, 16}) ==
                std::vector<uint8_t>(16, 42));
    BOOST_CHECK_EQUAL(authority->rangeReads - before, 2U);
    BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 0U);
  }
}

BOOST_AUTO_TEST_CASE(ManifestMetadataComparisonMatchesSerializedIdentity)
{
  RepoObjectManifest baseline;
  baseline.replicationFactor = 3;
  baseline.replicaNodes = {"replica-a", "replica-b"};
  baseline.packetNames = {"/producer/escaped-\"name", "/producer/second"};
  baseline.packetOffsets = {0, 12, 24};
  BOOST_CHECK(baseline.hasSameMetadata(baseline));
  const auto checkMutation = [&] (auto member, auto value) {
    auto changed = baseline;
    changed.*member = std::move(value);
    BOOST_CHECK_NE(baseline.toJson(), changed.toJson());
    BOOST_CHECK_EQUAL(baseline.hasSameMetadata(changed),
                      baseline.toJson() == changed.toJson());
    BOOST_CHECK_EQUAL(changed.hasSameMetadata(baseline),
                      changed.toJson() == baseline.toJson());
  };
  for (auto member : {&RepoObjectManifest::objectName, &RepoObjectManifest::objectType,
                     &RepoObjectManifest::sha256, &RepoObjectManifest::policyEpoch,
                     &RepoObjectManifest::publicationIdentity, &RepoObjectManifest::protectionEpoch,
                     &RepoObjectManifest::keyReferenceId, &RepoObjectManifest::keyReferenceVersion,
                     &RepoObjectManifest::ciphertextManifestDigest, &RepoObjectManifest::servingLocator,
                     &RepoObjectManifest::contentDigest, &RepoObjectManifest::operationId,
                     &RepoObjectManifest::lifecycleState, &RepoObjectManifest::dataReceiptId,
                     &RepoObjectManifest::dataReceiptRepoNode,
                     &RepoObjectManifest::dataReceiptProviderIdentity, &RepoObjectManifest::dataPrefix}) {
    checkMutation(member, std::string("different\"\\\nvalue"));
  }
  checkMutation(&RepoObjectManifest::writeConsistency, std::string("all"));
  checkMutation(&RepoObjectManifest::size, uint64_t{42});
  checkMutation(&RepoObjectManifest::segmentCount, uint32_t{2});
  checkMutation(&RepoObjectManifest::replicationFactor, uint32_t{4});
  checkMutation(&RepoObjectManifest::plaintextSize, uint64_t{42});
  checkMutation(&RepoObjectManifest::generation, uint64_t{1});
  checkMutation(&RepoObjectManifest::parentGeneration, int64_t{0});
  checkMutation(&RepoObjectManifest::requiredWriteAcks, uint32_t{2});
  checkMutation(&RepoObjectManifest::dataReceiptCommittedAtMs, uint64_t{42});
  checkMutation(&RepoObjectManifest::envelopeBytes, uint64_t{42});
  checkMutation(&RepoObjectManifest::replicaNodes, std::vector<std::string>{"replica-b", "replica-a"});
  checkMutation(&RepoObjectManifest::confirmedReplicaNodes, std::vector<std::string>{"replica-a"});
  checkMutation(&RepoObjectManifest::packetNames,
                std::vector<std::string>{"/producer/second", "/producer/escaped-\"name"});
  checkMutation(&RepoObjectManifest::packetOffsets, std::vector<uint64_t>{0, 13, 24});

  auto explicitDefaults = baseline;
  explicitDefaults.requiredWriteAcks = 3;
  explicitDefaults.confirmedReplicaNodes = baseline.replicaNodes;
  BOOST_CHECK_EQUAL(baseline.toJson(), explicitDefaults.toJson());
  BOOST_CHECK(baseline.hasSameMetadata(explicitDefaults));
  BOOST_CHECK(explicitDefaults.hasSameMetadata(baseline));
  for (const auto& consistency : {"ONE", "QUORUM", "ALL"}) {
    auto implicit = baseline;
    implicit.writeConsistency = consistency;
    auto explicitAcks = implicit;
    explicitAcks.requiredWriteAcks = requiredWriteAcks(3, parseRepoWriteConsistency(consistency));
    BOOST_CHECK_EQUAL(implicit.toJson(), explicitAcks.toJson());
    BOOST_CHECK(implicit.hasSameMetadata(explicitAcks));
    BOOST_CHECK(explicitAcks.hasSameMetadata(implicit));
  }
  auto invalid = baseline;
  invalid.writeConsistency = "invalid";
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  BOOST_CHECK_THROW(invalid.hasSameMetadata(baseline), std::invalid_argument);
  BOOST_CHECK_THROW(baseline.hasSameMetadata(invalid), std::invalid_argument);
  invalid = baseline;
  invalid.replicationFactor = 0;
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  BOOST_CHECK_THROW(invalid.hasSameMetadata(baseline), std::invalid_argument);
  BOOST_CHECK_THROW(baseline.hasSameMetadata(invalid), std::invalid_argument);
  invalid.requiredWriteAcks = 1;
  BOOST_CHECK(invalid.hasSameMetadata(invalid));
  BOOST_CHECK_EQUAL(invalid.toJson(), invalid.toJson());
}

BOOST_AUTO_TEST_CASE(ManifestMetadataComparisonLargePacketIndex)
{
  RepoObjectManifest manifest;
  for (size_t i = 0; i < 4096; ++i) {
    manifest.packetNames.push_back("/producer/" + std::string(128, 'x') + "/" + std::to_string(i));
    manifest.packetOffsets.push_back(i * 4000);
  }
  manifest.packetOffsets.push_back(4096 * 4000);
  auto same = manifest;
  constexpr size_t iterations = 16;
  size_t serializedMatches = 0;
  size_t directMatches = 0;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i)
    serializedMatches += manifest.toJson() == same.toJson();
  const auto serializedEnd = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i)
    directMatches += manifest.hasSameMetadata(same);
  const auto directEnd = std::chrono::steady_clock::now();
  BOOST_CHECK_EQUAL(serializedMatches, iterations);
  BOOST_CHECK_EQUAL(directMatches, iterations);
  same.packetNames.back() += "changed";
  BOOST_CHECK(!manifest.hasSameMetadata(same));
  same = manifest;
  ++same.packetOffsets.back();
  BOOST_CHECK(!manifest.hasSameMetadata(same));
  BOOST_TEST_MESSAGE("METADATA_COMPARISON_SAMPLE packets=4096 iterations=" << iterations
    << " serializedSeconds=" << std::chrono::duration<double>(serializedEnd - start).count()
    << " directSeconds=" << std::chrono::duration<double>(directEnd - serializedEnd).count()
    << " scope=helper-only-not-preparation");
}

BOOST_AUTO_TEST_CASE(PersistentStartupCountsAvailableBytesOnce)
{
  Fixture fixture;
  const std::vector<uint8_t> original(64, 0x31);
  {
    auto capability = fixture.capability();
    capability.freeBytes = 160;
    RepoCore initial(capability, fixture.store("capacity"));
    initial.put("/capacity/original", original);
  }
  auto store = fixture.store("capacity");
  auto capability = fixture.capability();
  capability.freeBytes = 96;
  capability.usedBytes = store->usedBytes();
  BOOST_REQUIRE_EQUAL(capability.usedBytes, 64);
  RepoCore reopened(capability, store);
  const auto snapshot = reopened.handleCapability();
  BOOST_CHECK_EQUAL(std::string(snapshot.begin(), snapshot.end()), capability.toJson());
  BOOST_CHECK(reopened.get("/capacity/original") == original);

  RepoObjectManifest oversized;
  oversized.objectName = "/capacity/oversized";
  oversized.size = 97;
  oversized.sha256 = std::string(64, '1');
  BOOST_CHECK_THROW(reopened.putRange(oversized, {0, 1}, {0x32}), std::runtime_error);
  BOOST_CHECK(!reopened.has(oversized.objectName));
  reopened.put("/capacity/exact", std::vector<uint8_t>(96, 0x32));
  capability.freeBytes = 0;
  capability.usedBytes = 160;
  const auto full = reopened.handleCapability();
  BOOST_CHECK_EQUAL(std::string(full.begin(), full.end()), capability.toJson());
  BOOST_CHECK_THROW(reopened.put("/capacity/overflow", {0x33}), std::runtime_error);
  BOOST_CHECK(reopened.get("/capacity/original") == original);
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

BOOST_AUTO_TEST_CASE(PacketBatchPreservesOriginalWiresAndReopensWithSmallRangeLimit)
{
  Fixture fixture;
  AdmissionFile incoming(fixture, "batch-read", 32, 128);
  {
    auto store = std::make_shared<CountingStore>(fixture.store("batch-read"));
    RepoCore repo(fixture.capability(), store);
    repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
    std::vector<ndn::Interest> interests;
    for (std::size_t i = 0; i < RepoCore::MAX_ORIGINAL_PACKET_READ_BATCH; ++i)
      interests.emplace_back(incoming.packets[i].first);
    const ndn::Data first(ndn::Block(ndn::span<const uint8_t>(
      incoming.packets[0].second.data(), incoming.packets[0].second.size())));
    interests[0].setName(first.getFullName());
    unsigned admissions = 0;
    store->rangeReads = 0;
    const auto result = repo.tryGetDataPacketBatch(interests,
      [&](const RepoObjectManifest&) { ++admissions; return true; }, [] {});
    BOOST_REQUIRE(result);
    BOOST_REQUIRE_EQUAL(result->size(), interests.size());
    BOOST_CHECK_EQUAL(admissions, 2U);
    BOOST_CHECK_EQUAL(store->rangeReads, interests.size());
    for (std::size_t i = 0; i < result->size(); ++i) {
      const auto& wire = result->at(i).wireEncode();
      BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == incoming.packets[i].second);
    }
    const std::vector<uint8_t> badDigest(32, 0);
    auto wrong = first.getName();
    wrong.appendImplicitSha256Digest(ndn::span<const uint8_t>(badDigest));
    interests[0].setName(wrong);
    BOOST_CHECK_THROW(repo.tryGetDataPacketBatch(interests,
      [](const RepoObjectManifest&) { return true; }, [] {}), std::runtime_error);
    interests[0].setName(first.getFullName());
    interests.push_back(interests.back());
    BOOST_CHECK_THROW(repo.tryGetDataPacketBatch(interests,
      [](const RepoObjectManifest&) { return true; }, [] {}), std::invalid_argument);
  }
  // Each individual packet fits; combining even two would exceed this backend
  // limit. Reopen with no writer-side index or memory cache available.
  const auto limit = incoming.packets.back().second.size();
  auto store = makeFilesystemRepoStore((fixture.root / "batch-read").string(), limit, 1U << 20);
  RepoCore reopened(fixture.capability(), store);
  std::vector<ndn::Interest> interests;
  for (std::size_t i = 0; i < RepoCore::MAX_ORIGINAL_PACKET_READ_BATCH; ++i)
    interests.emplace_back(incoming.packets[30 - 2 * i].first);
  const auto result = reopened.tryGetDataPacketBatch(interests,
    [](const RepoObjectManifest&) { return true; }, [] {});
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->size(), interests.size());
  for (std::size_t i = 0; i < result->size(); ++i) {
    const auto& wire = result->at(i).wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) ==
                incoming.packets[30 - 2 * i].second);
  }
}

BOOST_AUTO_TEST_CASE(PacketBatchUnsupportedShapesRetainExactReadCompatibility)
{
  Fixture fixture;
  auto store = std::make_shared<CountingStore>(fixture.store("batch-shapes"));
  RepoCore repo(fixture.capability(), store);
  AdmissionFile first(fixture, "batch-first", 4);
  AdmissionFile second(fixture, "batch-second", 4);
  repo.commitOriginalPacketFile(first.manifest, first.path);
  repo.commitOriginalPacketFile(second.manifest, second.path);
  std::vector<ndn::Interest> interests{ndn::Interest(first.packets[1].first),
                                       ndn::Interest(second.packets[1].first)};
  const auto accept = [](const RepoObjectManifest&) { return true; };
  store->rangeReads = 0;
  BOOST_CHECK(!repo.tryGetDataPacketBatch(interests, accept, [] {}));
  BOOST_CHECK_EQUAL(store->rangeReads, 0U);
  BOOST_CHECK(repo.getDataPacket(interests[0], accept) == first.packets[1].second);
  BOOST_CHECK(repo.getDataPacket(interests[1], accept) == second.packets[1].second);
  interests[0].setCanBePrefix(true);
  BOOST_CHECK(!repo.tryGetDataPacketBatch(interests, accept, [] {}));
  BOOST_CHECK(repo.getDataPacket(interests[0], accept) == first.packets[1].second);
  interests = {ndn::Interest(first.packets[1].first), ndn::Interest(first.packets[2].first)};
  store->concurrentReads = false;
  BOOST_CHECK(!repo.tryGetDataPacketBatch(interests, accept, [] {}));
  BOOST_CHECK(repo.getDataPacket(interests[1], accept) == first.packets[2].second);
  store->concurrentReads = true;
  const ndn::Name standalone("/actual/producer/standalone-batch");
  const auto wire = packet(standalone);
  repo.putDataPacket(standalone.toUri(), wire);
  interests[0].setName(standalone);
  BOOST_CHECK(!repo.tryGetDataPacketBatch(interests, accept, [] {}));
  BOOST_CHECK(repo.getDataPacket(interests[0], accept) == wire);
}

BOOST_AUTO_TEST_CASE(PacketBatchDiscardsResultsOnRevocationMutationDeletionOrStop)
{
  for (const std::string scenario : {"revoke", "metadata", "delete", "stop"}) {
    BOOST_TEST_CONTEXT(scenario) {
      Fixture fixture;
      auto store = std::make_shared<CountingStore>(fixture.store("batch-current"));
      RepoCore repo(fixture.capability(), store);
      AdmissionFile incoming(fixture, "batch-" + scenario, 4);
      repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
      std::ifstream input(incoming.path, std::ios::binary);
      const std::vector<uint8_t> payload((std::istreambuf_iterator<char>(input)), {});
      bool admitted = true;
      bool active = true;
      unsigned admissions = 0;
      store->rangeReads = 0;
      store->afterRangeRead = [&](std::vector<uint8_t>&) {
        if (store->rangeReads == 1 && scenario == "revoke") admitted = false;
        if (store->rangeReads == 1 && scenario == "stop") active = false;
        if (store->rangeReads == incoming.packets.size()) {
          if (scenario == "metadata") {
            auto changed = store->getManifest(incoming.manifest.objectName);
            ++changed.generation;
            changed.policyEpoch = "changed-during-batch";
            store->put(changed, payload);
          }
          if (scenario == "delete") (void)store->erase(incoming.manifest.objectName);
        }
      };
      std::vector<ndn::Interest> interests;
      for (const auto& item : incoming.packets) interests.emplace_back(item.first);
      std::optional<std::vector<ndn::Data>> result;
      const auto read = [&] {
        return repo.tryGetDataPacketBatch(interests,
          [&](const RepoObjectManifest&) { ++admissions; return admitted; }, [&] {
            if (!active) throw std::runtime_error("test-batch-cancelled");
          });
      };
      BOOST_CHECK_THROW(result = read(), std::exception);
      BOOST_CHECK(!result);
      BOOST_CHECK_EQUAL(store->rangeReads, scenario == "stop" ? 1U : incoming.packets.size());
      BOOST_CHECK_EQUAL(admissions, scenario == "revoke" ? 2U : 1U);
    }
  }
}

BOOST_AUTO_TEST_CASE(PacketBatchRejectsTruncatedOrWrongOriginalWire)
{
  for (const bool truncate : {false, true}) {
    Fixture fixture;
    auto store = std::make_shared<CountingStore>(fixture.store("batch-wire"));
    RepoCore repo(fixture.capability(), store);
    AdmissionFile incoming(fixture, "batch-wire", 4);
    repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
    store->rangeReads = 0;
    store->afterRangeRead = [&](std::vector<uint8_t>& wire) {
      if (store->rangeReads != 1) return;
      if (truncate) wire.pop_back();
      else wire = incoming.packets[1].second;
    };
    std::vector<ndn::Interest> interests;
    for (const auto& item : incoming.packets) interests.emplace_back(item.first);
    BOOST_CHECK_THROW(repo.tryGetDataPacketBatch(interests,
      [](const RepoObjectManifest&) { return true; }, [] {}), std::runtime_error);
    BOOST_CHECK_EQUAL(store->rangeReads, 1U);
  }
}

BOOST_AUTO_TEST_CASE(PacketFileRejectsAliasesOfTheSameTypedName)
{
  Fixture fixture;
  RepoCore repo(fixture.capability(), fixture.store("typed-alias"));
  AdmissionFile incoming(fixture, "typed-alias", 2);
  incoming.manifest.packetNames[1] =
    incoming.packets.front().first.toUri(ndn::name::UriFormat::ALTERNATE);
  BOOST_REQUIRE_NE(incoming.manifest.packetNames.front(), incoming.manifest.packetNames[1]);
  BOOST_CHECK_THROW(repo.commitOriginalPacketFile(incoming.manifest, incoming.path),
                    std::invalid_argument);
  BOOST_CHECK(repo.list().empty());
}

BOOST_AUTO_TEST_CASE(CachedRelatedPacketSetPreservesWireAndChecksAdmissionEveryRead)
{
  Fixture fixture;
  auto authority = std::make_shared<CountingStore>(fixture.store("cached-packets"));
  auto tiered = makeTieredRepoStore(authority, 1U << 20, "filesystem", 1U << 20);
  RepoCore repo(fixture.capability(), tiered);
  AdmissionFile incoming(fixture, "cached-originals", 32, 128);
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
  size_t admissionChecks = 0;
  const auto admit = [&] (const RepoObjectManifest&) { ++admissionChecks; return true; };
  const auto& first = incoming.packets.at(1);
  BOOST_CHECK(repo.getDataPacket(first.first.toUri(), admit) == first.second);
  const auto reads = authority->rangeReads;
  BOOST_REQUIRE_GT(reads, 0U);
  for (size_t i = 2; i < incoming.packets.size(); ++i) {
    const auto& item = incoming.packets[i];
    BOOST_CHECK(repo.getDataPacket(item.first.toUri(), admit) == item.second);
  }
  BOOST_CHECK_EQUAL(authority->rangeReads, reads);
  BOOST_CHECK_GE(admissionChecks, 31U);
  BOOST_CHECK_THROW(repo.getDataPacket(first.first.toUri(),
    [] (const RepoObjectManifest&) { return false; }), std::runtime_error);
  BOOST_CHECK_EQUAL(authority->rangeReads, reads);
  BOOST_REQUIRE(tiered->erase(incoming.manifest.objectName));
  BOOST_CHECK_THROW(repo.getDataPacket(first.first.toUri(), admit), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PacketFileAdmissionBoundsCatalogScans)
{
  Fixture fixture;
  auto tiered = makeTieredRepoStore(fixture.store("admission"), 0, "filesystem");
  auto store = std::make_shared<CountingStore>(tiered);
  RepoCore repo(fixture.capability(), store);
  const auto standaloneWire = packet(packetName());
  repo.putDataPacket(packetName().toUri(), standaloneWire);
  AdmissionFile incoming(fixture, "batch", 32);
  store->enumerations = 0;
  store->usageScans = 0;
  store->existenceProbes = 0;
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
  BOOST_CHECK_GT(store->enumerations, 0U);
  BOOST_CHECK_LE(store->enumerations, 8U);
  BOOST_CHECK_EQUAL(store->usageScans, 0U);
  BOOST_CHECK_LE(store->existenceProbes, 16U);
  auto expected = fixture.capability();
  expected.usedBytes = standaloneWire.size() + incoming.manifest.size;
  expected.freeBytes -= expected.usedBytes;
  const auto actualCapability = repo.handleCapability();
  BOOST_CHECK_EQUAL(std::string(actualCapability.begin(), actualCapability.end()),
                    expected.toJson());
  for (const auto& item : incoming.packets)
    BOOST_CHECK(repo.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(PacketFileAdmissionDistinguishesMetadataFromPhysicalPayload)
{
  Fixture fixture;
  auto store = fixture.store("physical-admission");
  AdmissionFile incoming(fixture, "physical", 4);
  const std::vector<uint8_t> bytes(32, 7);
  RepoObjectManifest opaque;
  opaque.objectName = incoming.manifest.packetNames.at(1);
  opaque.objectType = "opaque-payload";
  opaque.size = bytes.size();
  opaque.sha256 = sha256Hex(bytes);
  store->put(opaque, bytes);
  {
    RepoCore repo(fixture.capability(), store);
    BOOST_CHECK_EXCEPTION(repo.commitOriginalPacketFile(incoming.manifest, incoming.path),
      std::runtime_error, [](const std::runtime_error& error) {
        return std::string(error.what()) == "repo-original-packet-format-conflict";
      });
    const auto payload = fixture.root / "physical-admission/payloads/sha256" /
      opaque.sha256.substr(0, 2) / opaque.sha256;
    BOOST_REQUIRE(std::filesystem::remove(payload));
    const auto catalog = store->listManifests();
    BOOST_CHECK(std::any_of(catalog.begin(), catalog.end(), [&](const auto& row) {
      return row.objectName == opaque.objectName;
    }));
    BOOST_CHECK(!store->has(opaque.objectName));
    repo.commitOriginalPacketFile(incoming.manifest, incoming.path);
    for (const auto& item : incoming.packets)
      BOOST_CHECK(repo.getDataPacket(item.first.toUri()) == item.second);
  }
  store.reset();
  RepoCore reopened(fixture.capability(), fixture.store("physical-admission"));
  for (const auto& item : incoming.packets)
    BOOST_CHECK(reopened.getDataPacket(item.first.toUri()) == item.second);
}

BOOST_AUTO_TEST_CASE(PacketFileAdmissionPreservesDeduplicatedCapacity)
{
  Fixture fixture;
  auto tiered = makeTieredRepoStore(fixture.store("deduplicated"), 0, "filesystem");
  auto store = std::make_shared<CountingStore>(tiered);
  AdmissionFile incoming(fixture, "deduplicated", 4);
  std::ifstream input(incoming.path, std::ios::binary);
  BOOST_REQUIRE(input.good());
  std::vector<uint8_t> payload(incoming.manifest.size);
  input.read(reinterpret_cast<char*>(payload.data()),
             static_cast<std::streamsize>(payload.size()));
  BOOST_REQUIRE_EQUAL(input.gcount(), static_cast<std::streamsize>(payload.size()));

  RepoObjectManifest existing;
  existing.objectType = "opaque-payload";
  existing.objectName = "/repo/existing-payload";
  existing.size = incoming.manifest.size;
  existing.sha256 = incoming.manifest.sha256;
  store->put(existing, std::move(payload));

  RepoCore repo(fixture.capability(), store);
  store->usageScans = 0;
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);

  BOOST_CHECK_GT(store->usageScans, 0U);
  const auto actualCapability = repo.handleCapability();
  auto expected = fixture.capability();
  expected.usedBytes = incoming.manifest.size;
  expected.freeBytes -= expected.usedBytes;
  BOOST_CHECK_EQUAL(std::string(actualCapability.begin(), actualCapability.end()),
                    expected.toJson());
}

BOOST_AUTO_TEST_CASE(PacketFileAdmissionAccountsForOrphanedDigest)
{
  Fixture fixture;
  auto tiered = makeTieredRepoStore(fixture.store("orphaned"), 0, "filesystem");
  auto store = std::make_shared<CountingStore>(tiered);
  AdmissionFile incoming(fixture, "orphaned", 4);
  const auto payloadPath = fixture.root / "orphaned" / "payloads" / "sha256" /
                           incoming.manifest.sha256.substr(0, 2) /
                           incoming.manifest.sha256;
  std::filesystem::create_directories(payloadPath.parent_path());
  std::filesystem::copy_file(incoming.path, payloadPath);

  RepoCore repo(fixture.capability(), store);
  store->usageScans = 0;
  repo.commitOriginalPacketFile(incoming.manifest, incoming.path);

  BOOST_CHECK_GT(store->usageScans, 0U);
  const auto actualCapability = repo.handleCapability();
  auto expected = fixture.capability();
  expected.usedBytes = incoming.manifest.size;
  expected.freeBytes -= expected.usedBytes;
  BOOST_CHECK_EQUAL(std::string(actualCapability.begin(), actualCapability.end()),
                    expected.toJson());
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

BOOST_AUTO_TEST_CASE(ManifestSnapshotSurvivesReplacementWithoutCachingCurrentAdmission)
{
  Fixture fixture;
  auto store = fixture.store("manifest-snapshot");
  const std::vector<uint8_t> payload{1, 2, 3};
  RepoObjectManifest original;
  original.objectName = "/actual/producer/snapshot";
  original.size = payload.size();
  original.sha256 = sha256Hex(payload);
  original.generation = 1;
  original.policyEpoch = "epoch-a";
  store->put(original, payload);
  auto pinned = detail::readManifestSnapshot(*store, original.objectName);
  std::weak_ptr<const RepoObjectManifest> released = pinned;
  auto updated = original;
  ++updated.generation;
  updated.policyEpoch = "epoch-b";
  store->put(updated, payload);
  const auto current = detail::readManifestSnapshot(*store, original.objectName);
  BOOST_CHECK_EQUAL(pinned->policyEpoch, "epoch-a");
  BOOST_CHECK_EQUAL(current->policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(current->generation, updated.generation);
  auto owned = store->getManifest(original.objectName);
  owned.policyEpoch = "caller-mutation";
  BOOST_CHECK_EQUAL(detail::readManifestSnapshot(*store, original.objectName)->policyEpoch,
                    "epoch-b");
  pinned.reset();
  BOOST_CHECK(released.expired());
  const auto sidecar = std::filesystem::directory_iterator(
    fixture.root / "manifest-snapshot/manifests")->path();
  const auto stamp = std::filesystem::last_write_time(sidecar);
  {
    std::ofstream output(sidecar, std::ios::trunc);
    output << "{invalid json";
  }
  std::filesystem::last_write_time(sidecar, stamp);
  BOOST_CHECK_THROW(detail::readManifestSnapshot(*store, original.objectName), std::exception);
  BOOST_CHECK_EQUAL(current->policyEpoch, "epoch-b");
}

BOOST_AUTO_TEST_CASE(YoungManifestSnapshotsReuseParseButStillReadCurrentBytes)
{
  Fixture fixture;
  auto store = fixture.store("young-snapshot");
  const std::vector<uint8_t> payload{1, 2, 3};
  RepoObjectManifest original;
  original.objectName = "/actual/producer/young-snapshot";
  original.size = payload.size();
  original.sha256 = sha256Hex(payload);
  original.generation = 1;
  original.policyEpoch = "epoch-a";
  store->put(original, payload);
  const auto first = detail::readManifestSnapshot(*store, original.objectName);
  BOOST_CHECK(first == detail::readManifestSnapshot(*store, original.objectName));
  const auto sidecar = std::filesystem::directory_iterator(
    fixture.root / "young-snapshot/manifests")->path();
  const auto stamp = std::filesystem::last_write_time(sidecar);
  auto changed = original;
  changed.policyEpoch = "epoch-b";
  BOOST_REQUIRE_EQUAL(changed.toJson().size(), original.toJson().size());
  {
    std::ofstream output(sidecar, std::ios::trunc);
    output << changed.toJson();
  }
  std::filesystem::last_write_time(sidecar, stamp);
  const auto current = detail::readManifestSnapshot(*store, original.objectName);
  BOOST_CHECK_EQUAL(current->policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(first->policyEpoch, "epoch-a");
}

BOOST_AUTO_TEST_CASE(PhysicalMetadataSnapshotsPinIndexesButRecheckBackingFiles)
{
  Fixture fixture;
  auto backing = fixture.store("physical-snapshot");
  auto tiered = makeTieredRepoStore(backing, 1U << 20, "filesystem", 1U << 20);
  RepoCore core(fixture.capability(), tiered);
  PacketSet packets("/actual/physical-snapshot", 64);
  RepoObjectManifest original;
  original.objectName = packets.packets.front().first.toUri();
  original.objectType = "original-packet-set";
  original.generation = 1;
  original.policyEpoch = "epoch-a";
  original.packetOffsets = {0};
  std::vector<uint8_t> payload;
  for (const auto& packet : packets.packets) {
    original.packetNames.push_back(packet.first.toUri());
    payload.insert(payload.end(), packet.second.begin(), packet.second.end());
    original.packetOffsets.push_back(payload.size());
  }
  original.segmentCount = original.packetNames.size();
  original.size = payload.size();
  original.sha256 = sha256Hex(payload);
  backing->put(original, payload);
  // Recent filesystem identities intentionally require a byte read. Warm the
  // stat-valid parse only after the existing conservative timestamp boundary.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  core.probeObjectMetadataSnapshot(original.objectName);
  auto pinned = core.probeObjectMetadataSnapshot(original.objectName);
  BOOST_REQUIRE(pinned.manifest);
  BOOST_CHECK(pinned.manifest == core.probeObjectMetadataSnapshot(original.objectName).manifest);
  BOOST_CHECK(pinned.payloadPresent && pinned.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(pinned.payloadSize, payload.size());
  BOOST_CHECK(pinned.manifestMeasurementAvailable && pinned.manifestWireBytes > 0);
  auto owned = core.probeObjectMetadata(original.objectName);
  owned.manifest.packetNames.clear();
  BOOST_CHECK_EQUAL(pinned.manifest->packetNames.size(), packets.packets.size());
  auto updated = original;
  ++updated.generation;
  updated.policyEpoch = "epoch-b";
  backing->put(updated, payload);
  const auto current = core.probeObjectMetadataSnapshot(original.objectName);
  BOOST_REQUIRE(current.manifest);
  BOOST_CHECK_EQUAL(current.manifest->policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(pinned.manifest->policyEpoch, "epoch-a");
  const auto storedPayload = fixture.root / "physical-snapshot/payloads/sha256" /
    original.sha256.substr(0, 2) / original.sha256;
  std::filesystem::resize_file(storedPayload, 1);
  BOOST_CHECK_EQUAL(core.probeObjectMetadataSnapshot(original.objectName).payloadSize, 1);
  std::filesystem::remove(storedPayload);
  BOOST_CHECK(!core.probeObjectMetadataSnapshot(original.objectName).payloadPresent);
  std::filesystem::create_directory(storedPayload);
  const auto nonregular = core.probeObjectMetadataSnapshot(original.objectName);
  BOOST_CHECK(nonregular.payloadPresent && !nonregular.payloadRepresentationValid);
  const auto sidecar = std::filesystem::directory_iterator(
    fixture.root / "physical-snapshot/manifests")->path();
  {
    std::ofstream output(sidecar, std::ios::trunc);
    output << "{invalid json";
  }
  BOOST_CHECK_THROW(core.probeObjectMetadataSnapshot(original.objectName), std::exception);
  BOOST_CHECK_EQUAL(pinned.manifest->policyEpoch, "epoch-a");
  std::filesystem::remove(sidecar);
  BOOST_CHECK(!core.probeObjectMetadataSnapshot(original.objectName).manifest);
}

BOOST_AUTO_TEST_CASE(GenericPhysicalMetadataSnapshotPreservesOwnedProbeAuthority)
{
  Fixture fixture;
  const auto backing = fixture.store("generic-physical-snapshot");
  const auto generic = std::make_shared<CountingStore>(backing);
  RepoObjectManifest manifest;
  manifest.objectName = "/actual/generic-physical-snapshot";
  const std::vector<uint8_t> payload{1, 2, 3};
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.generation = 1;
  generic->put(manifest, payload);
  const auto owned = generic->probeObjectMetadata(manifest.objectName);
  const auto snapshot = detail::probeObjectMetadataSnapshot(*generic, manifest.objectName);
  BOOST_REQUIRE(snapshot.manifest);
  BOOST_CHECK(snapshot.manifest->hasSameMetadata(owned.manifest));
  BOOST_CHECK_EQUAL(snapshot.payloadMetadataAvailable, owned.payloadMetadataAvailable);
  BOOST_CHECK_EQUAL(snapshot.payloadPresent, owned.payloadPresent);
  BOOST_CHECK_EQUAL(snapshot.payloadRepresentationValid, owned.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(snapshot.payloadSize, owned.payloadSize);
  BOOST_CHECK_EQUAL(snapshot.manifestMeasurementAvailable, owned.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(snapshot.manifestWireBytes, owned.manifestWireBytes);
  const auto unknown = detail::probeObjectMetadataSnapshot(*makeSqliteRepoStore(":memory:"),
                                                          "/missing");
  BOOST_CHECK(!unknown.manifest && !unknown.payloadMetadataAvailable);
}

BOOST_AUTO_TEST_CASE(RestartPacketIndexSkipsRejectedHolder)
{
  Fixture fixture;
  const ndn::Name base("/actual/restart-index");
  const auto shared = ndn::Name(base).appendSegment(2);
  const auto sharedWire = packet(shared);
  std::string acceptedObject;
  {
    auto store = fixture.store("restart-holders");
    for (uint64_t segment = 0; segment < 2; ++segment) {
      const auto root = ndn::Name(base).appendSegment(segment);
      auto bytes = packet(root);
      const auto rootBytes = bytes.size();
      bytes.insert(bytes.end(), sharedWire.begin(), sharedWire.end());
      RepoObjectManifest manifest;
      manifest.objectName = root.toUri(ndn::name::UriFormat::CANONICAL);
      manifest.objectType = "original-packet-set";
      manifest.packetNames = {manifest.objectName,
        shared.toUri(ndn::name::UriFormat::CANONICAL)};
      manifest.packetOffsets = {0, rootBytes, bytes.size()};
      manifest.segmentCount = 2;
      manifest.size = bytes.size();
      manifest.sha256 = sha256Hex(bytes);
      manifest.generation = 1;
      store->put(manifest, bytes);
      if (segment == 1) acceptedObject = manifest.objectName;
    }
  }
  // Reopen valid durable metadata: no malformed row may force the scan fallback.
  auto store = fixture.store("restart-holders");
  BOOST_REQUIRE_EQUAL(store->listManifests().size(), 2U);
  RepoCore repo(fixture.capability(), store);
  unsigned rejectedChecks = 0;
  unsigned batchChecks = 0;
  const auto batch = repo.tryGetDataPacketBatch({ndn::Interest(shared), ndn::Interest(shared)},
    [&](const RepoObjectManifest& manifest) {
      if (manifest.objectName != acceptedObject) { ++rejectedChecks; return false; }
      ++batchChecks;
      return true;
    }, [] {});
  BOOST_REQUIRE(batch);
  BOOST_REQUIRE_EQUAL(batch->size(), 2U);
  BOOST_CHECK_GT(rejectedChecks, 0U);
  BOOST_CHECK_EQUAL(batchChecks, 2U);
  for (const auto& item : *batch) {
    const auto& wire = item.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == sharedWire);
  }
  BOOST_CHECK_THROW(repo.tryGetDataPacketBatch({ndn::Interest(shared), ndn::Interest(shared)},
    [](const RepoObjectManifest&) { return false; }, [] {}), std::runtime_error);
  unsigned selectedBatchChecks = 0;
  BOOST_CHECK_THROW(repo.tryGetDataPacketBatch({ndn::Interest(shared), ndn::Interest(shared)},
    [&](const RepoObjectManifest& manifest) {
      return manifest.objectName == acceptedObject && ++selectedBatchChecks == 1;
    }, [] {}), std::runtime_error);
  BOOST_CHECK_EQUAL(selectedBatchChecks, 2U);
  std::vector<uint8_t> received;
  BOOST_REQUIRE_NO_THROW(received = repo.getDataPacket(shared.toUri(),
    [&](const RepoObjectManifest& manifest) { return manifest.objectName == acceptedObject; }));
  BOOST_CHECK(received == sharedWire);
  BOOST_CHECK_THROW(repo.getDataPacket(shared.toUri(),
    [](const RepoObjectManifest&) { return false; }), std::runtime_error);
  unsigned selectedChecks = 0;
  BOOST_CHECK_THROW(repo.getDataPacket(shared.toUri(), [&](const RepoObjectManifest& manifest) {
    return manifest.objectName == acceptedObject && ++selectedChecks == 1;
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(selectedChecks, 2U);
}

BOOST_AUTO_TEST_CASE(PacketBatchColdLookupRetainsHeterogeneousAdmittedHolder)
{
  Fixture fixture;
  const ndn::Name base("/actual/batch-asymmetric");
  const auto shared = ndn::Name(base).appendSegment(2);
  const auto sharedWire = packet(shared);
  const std::array<ndn::Name, 2> roots{
    ndn::Name(base).appendSegment(0), ndn::Name("/actual/batch-heterogeneous/manifest")};
  std::string acceptedObject;
  {
    auto store = fixture.store("batch-asymmetric");
    for (const auto& root : roots) {
      auto bytes = packet(root);
      const auto rootBytes = bytes.size();
      bytes.insert(bytes.end(), sharedWire.begin(), sharedWire.end());
      RepoObjectManifest manifest;
      manifest.objectName = root.toUri(ndn::name::UriFormat::CANONICAL);
      manifest.objectType = "original-packet-set";
      manifest.packetNames = {manifest.objectName,
        shared.toUri(ndn::name::UriFormat::CANONICAL)};
      manifest.packetOffsets = {0, rootBytes, bytes.size()};
      manifest.segmentCount = 2;
      manifest.size = bytes.size();
      manifest.sha256 = sha256Hex(bytes);
      manifest.generation = 1;
      store->put(manifest, bytes);
      acceptedObject = manifest.objectName;
    }
  }
  RepoCore reopened(fixture.capability(), fixture.store("batch-asymmetric"));
  unsigned rejected = 0;
  unsigned accepted = 0;
  const auto result = reopened.tryGetDataPacketBatch({ndn::Interest(shared), ndn::Interest(shared)},
    [&](const RepoObjectManifest& manifest) {
      if (manifest.objectName != acceptedObject) { ++rejected; return false; }
      ++accepted;
      return true;
    }, [] {});
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->size(), 2U);
  BOOST_CHECK_GT(rejected, 0U);
  BOOST_CHECK_EQUAL(accepted, 2U);
  for (const auto& data : *result) {
    const auto& wire = data.wireEncode();
    BOOST_CHECK(std::vector<uint8_t>(wire.begin(), wire.end()) == sharedWire);
  }
}

BOOST_AUTO_TEST_CASE(PacketLocatorFiltersCatalogButRetainsEveryCurrentHolder)
{
  Fixture fixture;
  auto store = fixture.store("packet-locator");
  const ndn::Name shared("/actual/lookup/shared/seg=42");
  const auto sharedWire = packet(shared);
  const auto insert = [&](const std::string& suffix, ndn::name::UriFormat format) {
    const ndn::Name root("/actual/lookup/" + suffix);
    auto bytes = packet(root);
    const auto rootBytes = bytes.size();
    bytes.insert(bytes.end(), sharedWire.begin(), sharedWire.end());
    RepoObjectManifest manifest;
    manifest.objectName = root.toUri(format);
    manifest.objectType = "original-packet-set";
    manifest.packetNames = {root.toUri(format), shared.toUri(format)};
    manifest.packetOffsets = {0, rootBytes, bytes.size()};
    manifest.segmentCount = 2;
    manifest.size = bytes.size();
    manifest.sha256 = sha256Hex(bytes);
    manifest.generation = 1;
    store->put(manifest, bytes);
    return manifest;
  };
  const auto first = insert("holder-a", ndn::name::UriFormat::CANONICAL);
  auto second = insert("holder-b", ndn::name::UriFormat::ALTERNATE);
  const auto canonical = shared.toUri(ndn::name::UriFormat::CANONICAL);
  const auto alternate = shared.toUri(ndn::name::UriFormat::ALTERNATE);
  auto tiered = makeTieredRepoStore(store, 1U << 20, "filesystem", 1U << 20);
  const auto matches = [&] {
    return detail::findPacketManifests(*store, canonical, alternate, false);
  };
  BOOST_REQUIRE_EQUAL(matches().size(), 2U);
  BOOST_REQUIRE_EQUAL(detail::findPacketManifests(*tiered, canonical, alternate, false).size(), 2U);
  BOOST_CHECK(detail::findPacketManifests(*store, "/missing", "/missing", false).empty());
  BOOST_CHECK(detail::findPacketManifests(*store, "/8=controller/8=DKEY",
    "/controller/DKEY", true).empty());

  RepoObjectManifest unrelated;
  unrelated.objectName = ndn::Name("/unrelated/index").toUri(ndn::name::UriFormat::CANONICAL);
  unrelated.objectType = "original-packet-set";
  unrelated.packetNames = {unrelated.objectName};
  const auto unrelatedWire = packet(ndn::Name(unrelated.objectName));
  unrelated.size = unrelatedWire.size();
  unrelated.packetOffsets = {0, unrelated.size};
  unrelated.sha256 = sha256Hex(unrelatedWire);
  unrelated.generation = 1;
  store->put(unrelated, unrelatedWire);
  for (const auto& entry : std::filesystem::directory_iterator(
         fixture.root / "packet-locator" / "manifests")) {
    std::ifstream input(entry.path());
    const std::string encoded{std::istreambuf_iterator<char>(input), {}};
    if (parseManifestJson(encoded).objectName != unrelated.objectName)
      continue;
    unrelated.objectName = "/unrelated/changed-index";
    unrelated.packetOffsets = {0};
    std::ofstream output(entry.path(), std::ios::trunc);
    output << unrelated.toJson();
    break;
  }
  BOOST_REQUIRE_EQUAL(matches().size(), 2U);

  RepoCore repo(fixture.capability(), store);
  unsigned admitted = 0;
  BOOST_CHECK(repo.getDataPacket(canonical, [&](const RepoObjectManifest& manifest) {
    ++admitted;
    return manifest.objectName == second.objectName;
  }) == sharedWire);
  // Two candidate admissions, then a fresh check after copying original wire.
  BOOST_CHECK_EQUAL(admitted, 3U);
  unsigned selectedChecks = 0;
  BOOST_CHECK_THROW(repo.getDataPacket(canonical, [&](const RepoObjectManifest& manifest) {
    return manifest.objectName == second.objectName && ++selectedChecks == 1;
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(selectedChecks, 2U);

  ++second.generation;
  second.policyEpoch = "current-policy";
  std::filesystem::path changedSidecar;
  for (const auto& entry : std::filesystem::directory_iterator(
         fixture.root / "packet-locator" / "manifests")) {
    std::ifstream input(entry.path());
    const std::string encoded{std::istreambuf_iterator<char>(input), {}};
    if (parseManifestJson(encoded).objectName == second.objectName) {
      changedSidecar = entry.path();
      break;
    }
  }
  BOOST_REQUIRE(!changedSidecar.empty());
  {
    std::ofstream output(changedSidecar, std::ios::trunc);
    output << second.toJson();
    output.close();
    BOOST_REQUIRE(output.good());
  }
  const auto current = matches();
  BOOST_REQUIRE_EQUAL(current.size(), 2U);
  const auto changed = std::find_if(current.begin(), current.end(), [&](const auto& manifest) {
    return manifest.objectName == second.objectName;
  });
  BOOST_REQUIRE(changed != current.end());
  BOOST_CHECK_EQUAL(changed->generation, second.generation);
  BOOST_CHECK_EQUAL(changed->policyEpoch, second.policyEpoch);
  BOOST_CHECK(repo.getDataPacket(canonical, [&](const RepoObjectManifest& manifest) {
    return manifest.objectName == second.objectName && manifest.generation == second.generation;
  }) == sharedWire);
  BOOST_REQUIRE(store->erase(second.objectName));
  BOOST_REQUIRE_EQUAL(matches().size(), 1U);
  BOOST_REQUIRE_EQUAL(detail::findPacketManifests(*tiered, canonical, alternate, false).size(), 1U);
  BOOST_CHECK_THROW(repo.getDataPacket(canonical, [&](const RepoObjectManifest& manifest) {
    return manifest.objectName == second.objectName;
  }), std::runtime_error);

  std::filesystem::path sidecar;
  for (const auto& entry : std::filesystem::directory_iterator(
         fixture.root / "packet-locator" / "manifests")) {
    std::ifstream input(entry.path());
    const std::string encoded{std::istreambuf_iterator<char>(input), {}};
    if (parseManifestJson(encoded).objectName == first.objectName) {
      sidecar = entry.path();
      break;
    }
  }
  BOOST_REQUIRE(!sidecar.empty());
  const auto timestamp = std::filesystem::last_write_time(sidecar);
  {
    std::ofstream output(sidecar, std::ios::trunc);
    output << "{\"parentGeneration\":invalid}";
  }
  std::filesystem::last_write_time(sidecar, timestamp);
  BOOST_CHECK_THROW(matches(), std::exception);
}

BOOST_AUTO_TEST_CASE(YoungCatalogParseNeedsAReadAfterItsTimestampTickStabilizes)
{
  Fixture fixture;
  const auto directory = fixture.root / "young-catalog";
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
  const auto timestamp = std::filesystem::last_write_time(sidecar);
  const auto stamp = [&] {
    struct stat value{};
    if (::stat(sidecar.c_str(), &value) != 0)
      throw std::runtime_error("test manifest stat failed");
    return std::array<uint64_t, 7>{static_cast<uint64_t>(value.st_dev),
      static_cast<uint64_t>(value.st_ino), static_cast<uint64_t>(value.st_size),
      static_cast<uint64_t>(value.st_mtim.tv_sec), static_cast<uint64_t>(value.st_mtim.tv_nsec),
      static_cast<uint64_t>(value.st_ctim.tv_sec), static_cast<uint64_t>(value.st_ctim.tv_nsec)};
  };
  const auto replace = [&](const RepoObjectManifest& value) {
    std::ofstream output(sidecar, std::ios::trunc);
    output << value.toJson();
    output.close();
    BOOST_REQUIRE(output.good());
    std::filesystem::last_write_time(sidecar, timestamp);
  };
  auto changed = metadata;
  changed.policyEpoch = "epoch-b";
  bool collision = false;
  for (unsigned attempt = 0; attempt < 100 && !collision; ++attempt) {
    replace(metadata);
    BOOST_REQUIRE_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-a");
    const auto before = stamp();
    replace(changed);
    collision = before == stamp();
  }
  if (!collision) {
    BOOST_TEST_MESSAGE("filesystem did not expose an equal-stamp write; collision case skipped");
    return;
  }
  BOOST_TEST_MESSAGE("confirmed equal metadata stamps; checking delayed cache admission");
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const auto found = detail::findPacketManifests(*store, metadata.objectName,
    packetName().toUri(ndn::name::UriFormat::ALTERNATE), true);
  BOOST_REQUIRE_EQUAL(found.size(), 1U);
  BOOST_CHECK_EQUAL(found.front().policyEpoch, "epoch-b");
  BOOST_CHECK_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-b");
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
  BOOST_CHECK_EQUAL(store->getManifest(metadata.objectName).policyEpoch, "epoch-b");
  BOOST_CHECK(std::filesystem::last_write_time(sidecar) == originalTime);

  auto malformed = current;
  const std::string consistency = "\"writeConsistency\":\"";
  const auto consistencyAt = malformed.find(consistency);
  BOOST_REQUIRE_NE(consistencyAt, std::string::npos);
  malformed.replace(consistencyAt + consistency.size(), 3, "BAD");
  writeSidecar(malformed);
  BOOST_CHECK_THROW(store->listManifests(), std::invalid_argument);
  BOOST_CHECK_THROW(store->getManifest(metadata.objectName), std::invalid_argument);
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

BOOST_AUTO_TEST_CASE(NodeInsertEnumeratesCatalogPerGroupAndPreservesImmutablePackets)
{
  Fixture fixture;
  std::vector<size_t> scans;
  for (const auto segments : {16U, 64U}) {
    const auto suffix = std::to_string(segments);
    auto store = std::make_shared<CountingStore>(fixture.store(("node-batch-" + suffix).c_str()));
    RepoNode node(ndn::Name("/repo/batch"), fixture.capability(), store);
    const auto insert = [&](const PacketSet& set,
                            const std::vector<std::vector<uint8_t>>& wires) {
      RepoDataReference reference;
      reference.objectName = set.options.manifestName;
      reference.dataPrefix = set.manifest.producer.toUri();
      reference.expectedSize = set.options.packetWireBytes;
      return node.insertWirePackets(reference, wires);
    };
    const auto wiresFor = [](const PacketSet& set) {
      std::vector<std::vector<uint8_t>> wires;
      for (const auto& entry : set.packets) wires.push_back(entry.second);
      return wires;
    };
    PacketSet background("/actual/background", 8);
    BOOST_REQUIRE_EQUAL(insert(background, wiresFor(background)).state, "DONE");
    PacketSet incoming("/actual/incoming", segments);
    const auto wires = wiresFor(incoming);
    store->enumerations = 0;
    BOOST_REQUIRE_EQUAL(insert(incoming, wires).state, "DONE");
    scans.push_back(store->enumerations);
    BOOST_CHECK_LE(store->enumerations, 2U);

    auto duplicated = wires;
    duplicated.push_back(wires.back());
    const auto duplicate = insert(incoming, duplicated);
    BOOST_CHECK_EQUAL(duplicate.state, "FAILED");
    BOOST_CHECK_NE(duplicate.message.find("duplicate NDN Data name"), std::string::npos);

    auto changed = wires;
    ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wires.back().data(), wires.back().size())));
    data.setContent(std::vector<uint8_t>(4000, 255));
    ndn::KeyChain keys("pib-memory:", "tpm-memory:");
    keys.sign(data, ndn::security::signingWithSha256());
    const auto& encoded = data.wireEncode();
    changed.back().assign(encoded.begin(), encoded.end());
    const auto conflict = insert(incoming, changed);
    BOOST_CHECK_EQUAL(conflict.state, "FAILED");
    BOOST_CHECK_NE(conflict.message.find("repo-data-wire-conflict"), std::string::npos);
    BOOST_CHECK(node.getDataPacket(incoming.packets.back().first.toUri()) == wires.back());

    auto legacyStore = fixture.store(("node-legacy-" + suffix).c_str());
    RepoNode legacy(ndn::Name("/repo/legacy"), fixture.capability(), legacyStore);
    legacy.core().putDataPacket(incoming.packets.back().first.toUri(), changed.back());
    RepoDataReference reference;
    reference.objectName = incoming.options.manifestName;
    reference.dataPrefix = incoming.manifest.producer.toUri();
    reference.expectedSize = incoming.options.packetWireBytes;
    const auto legacyConflict = legacy.insertWirePackets(reference, wires);
    BOOST_CHECK_EQUAL(legacyConflict.state, "FAILED");
    BOOST_CHECK_NE(legacyConflict.message.find("repo-data-wire-conflict"), std::string::npos);
    BOOST_CHECK(legacy.getDataPacket(incoming.packets.back().first.toUri()) == changed.back());

    const auto& legacyName = incoming.packets.back().first;
    const auto canonical = legacyName.toUri(ndn::name::UriFormat::CANONICAL);
    const auto alternate = legacyName.toUri(ndn::name::UriFormat::ALTERNATE);
    BOOST_REQUIRE_NE(canonical, alternate);
    auto alias = legacyStore->getManifest(canonical);
    alias.objectName = alternate;
    alias.packetNames = {alternate};
    legacyStore->put(alias, changed.back());
    BOOST_REQUIRE(legacyStore->erase(canonical));
    const auto aliasConflict = legacy.insertWirePackets(reference, wires);
    BOOST_CHECK_EQUAL(aliasConflict.state, "FAILED");
    BOOST_CHECK_NE(aliasConflict.message.find("repo-data-wire-conflict"), std::string::npos);
    BOOST_CHECK(legacy.getDataPacket(alternate) == changed.back());
  }
  BOOST_REQUIRE_EQUAL(scans.size(), 2U);
  BOOST_CHECK_LE(scans.back(), scans.front() + 2);
}

BOOST_AUTO_TEST_CASE(NodeOriginalPacketInsertSurvivesFreshStoreRestart)
{
  Fixture fixture;
  PacketSet incoming("/actual/restart-node-insert", 16);
  {
    RepoNode node(ndn::Name("/repo/batch"), fixture.capability(), fixture.store("node-restart"));
    RepoDataReference reference;
    reference.objectName = incoming.options.manifestName;
    reference.dataPrefix = incoming.manifest.producer.toUri();
    reference.expectedSize = incoming.options.packetWireBytes;
    std::vector<std::vector<uint8_t>> wires;
    for (const auto& packet : incoming.packets) wires.push_back(packet.second);
    BOOST_REQUIRE_EQUAL(node.insertWirePackets(reference, wires).state, "DONE");
  }
  RepoNode reopened(ndn::Name("/repo/batch"), fixture.capability(), fixture.store("node-restart"));
  for (const auto& packet : incoming.packets)
    BOOST_CHECK(reopened.getDataPacket(packet.first.toUri()) == packet.second);
}

BOOST_AUTO_TEST_CASE(ExactOriginalRootLookupAvoidsCatalogEnumeration)
{
  Fixture fixture;
  auto store = std::make_shared<CountingStore>(fixture.store("exact-original-root"));
  auto repo = std::make_shared<RepoCore>(fixture.capability(), store);
  RepoEncryptedLargeDataStore backend(repo);
  PacketSet selected("/actual/exact-original-root");
  const auto commit = [&](PacketSet& set) {
    size_t cursor = 0;
    return backend.commitOriginalDataPackets(set.manifest.getName(), set.options,
      [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
        if (cursor == set.packets.size()) return std::nullopt;
        return set.packets[cursor++];
      });
  };
  auto source = commit(selected);
  for (unsigned i = 0; i < 3; ++i) {
    PacketSet unrelated("/actual/unrelated-root-" + std::to_string(i));
    (void)commit(unrelated);
  }
  store->enumerations = 0;
  const auto root = selected.manifest.getName();
  BOOST_CHECK(repo->hasDataPacket(root.toUri()));
  BOOST_CHECK(repo->getDataPacket(root.toUri()) == selected.packets.front().second);
  const auto found = backend.lookupDurableObject(selected.options.publicationIdentity, root);
  BOOST_REQUIRE(found);
  BOOST_CHECK(found->source->readOriginalDataPacket(root) == selected.packets.front().second);
  BOOST_CHECK_EQUAL(store->enumerations, 0U);

  unsigned admissionCalls = 0;
  BOOST_CHECK_THROW(repo->getDataPacket(root.toUri(), [&](const RepoObjectManifest&) {
    ++admissionCalls;
    return false;
  }), std::runtime_error);
  BOOST_CHECK_GT(admissionCalls, 0U);
  BOOST_CHECK_THROW(backend.lookupDurableObject("sha256:wrong-publication", root),
                    std::runtime_error);
  BOOST_CHECK(!backend.lookupDurableObject(selected.options.publicationIdentity,
    ndn::Name("/actual/missing-original-root")));
  BOOST_CHECK(source->readOriginalDataPacket(root) == selected.packets.front().second);
}

BOOST_AUTO_TEST_CASE(FilteredExactReadsUseProofHintsButRecheckCurrentMetadataAndPolicy)
{
  Fixture fixture;
  auto store = std::make_shared<CountingStore>(fixture.store("filtered-proof-hints"));
  auto repo = std::make_shared<RepoCore>(fixture.capability(), store);
  RepoEncryptedLargeDataStore backend(repo);
  PacketSet selected("/actual/filtered-proof-hints");
  size_t cursor = 0;
  auto source = backend.commitOriginalDataPackets(selected.manifest.getName(), selected.options,
    [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      if (cursor == selected.packets.size()) return std::nullopt;
      return selected.packets[cursor++];
    });
  BOOST_REQUIRE(source);
  const auto admit = [](const RepoObjectManifest&) { return true; };
  BOOST_CHECK(repo->getDataPacket(selected.packets.front().first.toUri(), admit) ==
              selected.packets.front().second);
  store->enumerations = 0;
  for (size_t i = 1; i < selected.packets.size(); ++i)
    BOOST_CHECK(repo->getDataPacket(selected.packets[i].first.toUri(), admit) ==
                selected.packets[i].second);
  BOOST_CHECK_EQUAL(store->enumerations, 0U);

  auto current = repo->getManifest(selected.options.manifestName);
  const auto directory = fixture.root / "filtered-proof-hints" / "manifests";
  const auto entry = std::filesystem::directory_iterator(directory);
  BOOST_REQUIRE(entry != std::filesystem::directory_iterator{});
  const auto sidecar = entry->path();
  const auto replace = [&](const RepoObjectManifest& metadata) {
    std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
    output << metadata.toJson();
    output.close();
    BOOST_REQUIRE(output.good());
  };
  ++current.generation;
  current.policyEpoch = "changed-current-policy";
  replace(current);
  bool observedCurrent = false;
  BOOST_CHECK_THROW(repo->getDataPacket(selected.packets.back().first.toUri(),
    [&](const RepoObjectManifest& metadata) {
      observedCurrent = metadata.generation == current.generation &&
                        metadata.policyEpoch == current.policyEpoch;
      return false;
    }), std::runtime_error);
  BOOST_CHECK(observedCurrent);
  BOOST_CHECK(repo->getDataPacket(selected.packets.back().first.toUri(),
    [&](const RepoObjectManifest& metadata) {
      return metadata.generation == current.generation && metadata.policyEpoch == current.policyEpoch;
    }) == selected.packets.back().second);

  auto invalid = current;
  ++invalid.packetOffsets.back();
  replace(invalid);
  BOOST_CHECK_THROW(repo->getDataPacket(selected.packets.back().first.toUri(), admit),
                    std::invalid_argument);
  replace(current);
  BOOST_REQUIRE(store->erase(current.objectName));
  BOOST_CHECK_THROW(repo->getDataPacket(selected.packets.back().first.toUri(), admit),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PrefixReadSkipsUnrelatedIndexesAndStillAdmitsItsHolder)
{
  Fixture fixture;
  auto store = std::make_shared<CountingStore>(fixture.store("prefix-candidates"));
  auto repo = std::make_shared<RepoCore>(fixture.capability(), store);
  RepoEncryptedLargeDataStore backend(repo);
  PacketSet selected("/actual/prefix-candidates");
  size_t cursor = 0;
  auto source = backend.commitOriginalDataPackets(selected.manifest.getName(), selected.options,
    [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
      if (cursor == selected.packets.size()) return std::nullopt;
      return selected.packets[cursor++];
    });
  BOOST_REQUIRE(source);

  RepoObjectManifest unrelated;
  unrelated.objectType = "original-packet-set";
  unrelated.objectName = ndn::Name("/unrelated/object").toUri(ndn::name::UriFormat::CANONICAL);
  unrelated.packetNames = {unrelated.objectName};
  // A corrupt noncandidate must not force index parsing or block another Producer.
  unrelated.packetOffsets = {0};
  store->extraCatalogEntry = unrelated;
  ndn::Interest missing(ndn::Name("/controller/DKEY"));
  missing.setCanBePrefix(true);
  BOOST_CHECK_EXCEPTION(repo->getDataPacket(missing), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()) == "repo-original-packet-interest-not-found";
    });
  ndn::Interest prefix(selected.manifest.dataPrefix);
  prefix.setCanBePrefix(true);
  BOOST_CHECK(repo->getDataPacket(prefix) == selected.packets[1].second);
  unsigned admissions = 0;
  BOOST_CHECK_THROW(repo->getDataPacket(prefix, [&](const RepoObjectManifest&) {
    ++admissions;
    return false;
  }), std::runtime_error);
  BOOST_CHECK_EQUAL(admissions, 1U);

  auto invalidCandidate = repo->getManifest(selected.options.manifestName);
  invalidCandidate.packetOffsets.pop_back();
  store->extraCatalogEntry = invalidCandidate;
  BOOST_CHECK_THROW(repo->getDataPacket(prefix), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RepeatedPacketSourceReadsKeepCurrentMetadataGuards)
{
  Fixture fixture;
  PacketSet set("/actual/repeated-packet-producer", 128);
  auto repo = std::make_shared<RepoCore>(fixture.capability(), fixture.store("repeated"));
  RepoEncryptedLargeDataStore backend(repo);
  size_t cursor = 0;
  const auto next = [&]() -> std::optional<std::pair<ndn::Name, std::vector<uint8_t>>> {
    if (cursor == set.packets.size()) return std::nullopt;
    return set.packets[cursor++];
  };
  auto source = backend.commitOriginalDataPackets(set.manifest.getName(), set.options, next);
  bool matches = true;
  uint64_t bytes = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto cpuStarted = std::clock();
  for (unsigned pass = 0; pass < 3; ++pass) {
    for (const auto& expected : set.packets) {
      const auto wire = source->readOriginalDataPacket(expected.first);
      matches = (wire == expected.second) && matches;
      bytes += wire.size();
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - started).count();
  const auto cpu = 1000000.0 * (std::clock() - cpuStarted) / CLOCKS_PER_SEC;
  BOOST_CHECK(matches);
  BOOST_CHECK_EQUAL(bytes, 3 * set.options.packetWireBytes);
  BOOST_TEST_MESSAGE("REPEATED_ORIGINAL_PACKET_READS packets=" << 3 * set.packets.size()
                     << " wall_us=" << elapsed << " cpu_us=" << cpu);

  const auto original = repo->list().front();
  auto changed = original;
  ++changed.generation;
  const auto sidecar = std::filesystem::directory_iterator(
    fixture.root / "repeated/manifests")->path();
  const auto originalTime = std::filesystem::last_write_time(sidecar);
  const auto rewrite = [&](const std::string& encoded) {
    std::ofstream stream(sidecar, std::ios::binary | std::ios::trunc);
    stream << encoded;
    stream.close();
    std::filesystem::last_write_time(sidecar, originalTime);
  };
  BOOST_REQUIRE_EQUAL(original.toJson().size(), changed.toJson().size());
  rewrite(changed.toJson());
  BOOST_CHECK_EXCEPTION(source->readOriginalDataPacket(set.packets.back().first),
    std::runtime_error, [](const auto& error) {
      return std::string(error.what()) == "repo-publication-identity-mismatch";
    });
  rewrite("{invalid json");
  BOOST_CHECK_THROW(source->readOriginalDataPacket(set.packets.back().first), std::exception);
  rewrite(original.toJson());
  BOOST_CHECK(source->readOriginalDataPacket(set.packets.back().first) ==
              set.packets.back().second);
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
