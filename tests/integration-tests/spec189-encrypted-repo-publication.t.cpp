#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <atomic>
#include <future>
#include <thread>
#include <unistd.h>

namespace {
using namespace ndn_service_framework;
using namespace ndn_service_framework::test;
using namespace ndnsf_distributed_repo;

struct InspectingUser : LocalServiceUser
{
  using LocalServiceUser::LocalServiceUser;
  void releaseSeed(const ndn::Name& service, const std::string& id)
  { m_hybridMessageCrypto.eraseWrappedSendKey(service, id); }
  bool hasWrapped(const std::string& id) const
  {
    ndn::Buffer bytes;
    return m_hybridMessageCrypto.getWrappedSendKey(id, bytes);
  }
};

struct RepoFixture
{
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    ("spec189-encrypted-repo-" + std::to_string(::getpid()));
  std::shared_ptr<RepoCore> repo;
  RepoFixture()
  {
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
    StorageCapability capability;
    capability.repoNode = "/spec189/encrypted-repo";
    capability.freeBytes = 16U << 20;
    repo = std::make_shared<RepoCore>(capability,
      makeFilesystemRepoStore((root / "objects").string(), 1U << 20, 1U << 20));
  }
  ~RepoFixture()
  {
    repo.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};

struct SlowStore final : EncryptedLargeDataRangeStore
{
  std::shared_ptr<RepoEncryptedLargeDataStore> delegate;
  std::atomic<bool> entered{false}, release{false};
  explicit SlowStore(std::shared_ptr<RepoCore> repo)
    : delegate(std::make_shared<RepoEncryptedLargeDataStore>(std::move(repo))) {}
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string& name, const std::filesystem::path& path, std::uint64_t size,
    const std::function<void()>& active) override
  {
    entered = true;
    while (!release.load()) {
      if (active) active();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return delegate->commitFile(name, path, size, active);
  }
};

struct WorkerDrain
{
  ndn::Face& face;
  std::future<LargeDataPublishResult>& result;
  std::atomic<bool>& cancelled;
  bool wait(std::chrono::seconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (result.valid() && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      face.processEvents(ndn::time::milliseconds(1));
      if (std::chrono::steady_clock::now() >= deadline) return false;
    }
    return true;
  }
  ~WorkerDrain()
  {
    cancelled = true;
    // A broken drain must fail this fixture, never destroy a live worker's
    // Face. The target timeout is an additional external failure boundary.
    if (!wait(std::chrono::seconds(3))) std::terminate();
  }
};

std::shared_ptr<ndn::svs::SVSPubSub> pubsub(ndn::Face& face, ndn::KeyChain& keys)
{
  ndn::svs::SecurityOptions security(keys);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo = ndn::security::signingWithSha256();
  security.pubSigner->signingInfo = ndn::security::signingWithSha256();
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions options;
  options.useTimestamp = false;
  return std::make_shared<ndn::svs::SVSPubSub>(
    ndn::Name("/spec189/repo/sync"), ndn::Name("/spec189/repo/user"), face,
    [] (const std::vector<ndn::svs::MissingDataInfo>&) {}, options, security);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec189EncryptedRepo)

BOOST_AUTO_TEST_CASE(WorkerCommitKeepsIoResponsiveAndCancellationDrains)
{
  RepoFixture fixture;
  auto store = std::make_shared<SlowStore>(fixture.repo);
  const auto spool = (fixture.root / "spool").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ndn::security::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto certificate = makeRsaIdentity(keys, ndn::Name("/spec189/worker-user"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec189/worker-aa"));
  InspectingUser user(face, ndn::Name("/spec189/worker"), certificate, authority,
                        "examples/trust-any.conf");
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.setEncryptedLargeDataRangeStore(store);
  user.init();
  const auto key = user.prepareHybridSendKeyForTest(ndn::Name("/spec189/model"), "REQUEST-LARGE");
  const auto request = user.prepareServiceRequest("/spec189/model");
  const std::vector<std::uint8_t> bytes(40000, 9);
  std::atomic<bool> cancelled{false};
  auto result = std::async(std::launch::async, [&] {
    return user.publishEncryptedLargeDataFromWorker(request, bytes, "slow-store",
      ndn::time::milliseconds(1), [&] {
        if (cancelled.load()) throw std::runtime_error("fixture-cancelled");
      });
  });
  WorkerDrain drain{face, result, cancelled};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!store->entered && std::chrono::steady_clock::now() < deadline)
    face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(store->entered);
  user.releaseSeed(ndn::Name("/spec189/model"), key.keyId);
  BOOST_CHECK(user.hasWrapped(key.keyId));
  bool heartbeat = false;
  user.postToIo([&] { heartbeat = true; });
  face.processEvents(ndn::time::milliseconds(5));
  BOOST_CHECK(heartbeat);
  BOOST_CHECK(result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
  cancelled = true;
  BOOST_REQUIRE(drain.wait(std::chrono::seconds(3)));
  const auto failed = result.get();
  BOOST_CHECK(!failed.success);
  BOOST_CHECK_NE(failed.errorMessage.find("fixture-cancelled"), std::string::npos);
  BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 0U);
  BOOST_CHECK(fixture.repo->list().empty());
  BOOST_CHECK(std::filesystem::is_empty(spool));
  BOOST_CHECK(!user.hasWrapped(key.keyId));
}

BOOST_AUTO_TEST_CASE(ProtectedSegmentsSurviveTtlAndReleaseWithLease)
{
  RepoFixture fixture;
  auto store = std::make_shared<RepoEncryptedLargeDataStore>(fixture.repo);
  const auto spool = (fixture.root / "spool").string();
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ScopedEnvironmentValue retention("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "5");
  ndn::security::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto certificate = makeRsaIdentity(keys, ndn::Name("/spec189/user"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/spec189/authority"));
  InspectingUser user(face, ndn::Name("/spec189/group"), certificate,
                        authority, "examples/trust-any.conf");
  user.useSigningKeyChainForSigningOnlyForTest(keys);
  user.attachLocalMockPubSubForTest(pubsub(face, keys));
  user.setEncryptedLargeDataRangeStore(store);
  user.init();
  face.processEvents(ndn::time::milliseconds(1));
  const ndn::Name service("/spec189/model");
  const auto key = user.prepareHybridSendKeyForTest(service, "REQUEST-LARGE");
  const std::vector<std::uint8_t> plaintext(40000, 0x5a);
  auto publication = user.publishEncryptedLargeData(user.prepareServiceRequest(service.toUri()),
    plaintext, "model-material", ndn::time::milliseconds(1), true);
  BOOST_REQUIRE_MESSAGE(publication.success, publication.errorMessage);
  BOOST_REQUIRE(publication.servingLease);
  user.releaseSeed(service, key.keyId);
  BOOST_REQUIRE(user.hasWrapped(key.keyId));
  BOOST_REQUIRE(fixture.repo->has(publication.encryptedDataName.toUri()));
  face.processEvents(ndn::time::milliseconds(30));
  BOOST_REQUIRE_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 1U);
  BOOST_CHECK_THROW(user.setEncryptedLargeDataRangeStore({}), std::logic_error);

  // Fetch every segment through the production Interest callback after TTL.
  ndn::Interest discovery(publication.encryptedDataName);
  discovery.setCanBePrefix(true);
  face.receive(discovery);
  face.processEvents(ndn::time::milliseconds(1));
  std::map<std::uint64_t, ndn::Buffer> segments;
  std::uint64_t last = 0;
  for (const auto& data : face.sentData) {
    if (publication.encryptedDataName.isPrefixOf(data.getName())) {
      BOOST_REQUIRE(data.getFinalBlock());
      last = data.getFinalBlock()->toSegment();
    }
  }
  BOOST_REQUIRE_GT(last, 0U);
  for (std::uint64_t i = 1; i <= last; ++i)
    face.receive(ndn::Interest(ndn::Name(publication.encryptedDataName).appendSegment(i)));
  face.processEvents(ndn::time::milliseconds(1));
  for (const auto& data : face.sentData) {
    if (!publication.encryptedDataName.isPrefixOf(data.getName()))
      continue;
    BOOST_REQUIRE(ndn::security::verifySignature(data, certificate));
    BOOST_CHECK_LE(data.wireEncode().size(), 8800U);
    const auto content = data.getContent();
    segments[data.getName().get(-1).toSegment()] =
      ndn::Buffer(content.value(), content.value() + content.value_size());
  }
  BOOST_REQUIRE_EQUAL(segments.size(), last + 1);
  ndn::Buffer ciphertext;
  for (const auto& item : segments)
    ciphertext.insert(ciphertext.end(), item.second.begin(), item.second.end());
  HybridMessageEnvelope envelope;
  BOOST_REQUIRE(envelope.WireDecode(ndn::Block(ciphertext)));
  const auto associated = publication.encryptedDataName.toUri() + "|REQUEST-LARGE|" + service.toUri();
  ndn::Buffer decoded;
  BOOST_REQUIRE(hybridAesGcmDecrypt(key.key, envelope,
    ndn::Buffer(reinterpret_cast<const std::uint8_t*>(associated.data()), associated.size()), decoded));
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(), plaintext.begin(), plaintext.end());
  const auto name = publication.encryptedDataName.toUri();
  publication.servingLease.reset();
  face.processEvents(ndn::time::milliseconds(1100));
  BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 0U);
  BOOST_CHECK(!fixture.repo->has(name));
  BOOST_CHECK(!user.hasWrapped(key.keyId));
}

BOOST_AUTO_TEST_CASE(DuplicateAndInvalidInputDoNotRemoveCommittedData)
{
  RepoFixture fixture;
  RepoEncryptedLargeDataStore store(fixture.repo);
  const auto input = fixture.root / "encrypted.bin";
  { std::ofstream stream(input, std::ios::binary); stream << "ciphertext"; }
  auto lease = store.commitFile("/ciphertext/object", input, 10);
  BOOST_REQUIRE(fixture.repo->has("/ciphertext/object"));
  BOOST_CHECK_THROW(store.commitFile("/ciphertext/object", input, 10), std::runtime_error);
  BOOST_CHECK_THROW(store.commitFile("/ciphertext/invalid", input, 11), std::invalid_argument);
  BOOST_CHECK(fixture.repo->has("/ciphertext/object"));
  BOOST_CHECK_THROW(lease->read(9, 2), std::out_of_range);
  BOOST_CHECK_EQUAL(lease->read(0, 10).size(), 10U);
  lease.reset();
  BOOST_CHECK(!fixture.repo->has("/ciphertext/object"));
}

BOOST_AUTO_TEST_CASE(OldLeaseCannotReadOrDeleteSameNameReplacement)
{
  for (bool sameBytes : {false, true}) {
    RepoFixture fixture;
    RepoEncryptedLargeDataStore store(fixture.repo);
    const auto input = fixture.root / "replace.bin";
    { std::ofstream stream(input, std::ios::binary); stream << "ciphertext"; }
    auto old = store.commitFile("/replace/object", input, 10);
    const auto original = fixture.repo->getManifest("/replace/object");
    BOOST_REQUIRE(fixture.repo->remove("/replace/object"));
    if (!sameBytes) {
      std::ofstream stream(input, std::ios::binary); stream << "different!";
    }
    auto replacement = store.commitFile("/replace/object", input, 10);
    const auto current = fixture.repo->getManifest("/replace/object");
    BOOST_REQUIRE_NE(original.operationId, current.operationId);
    BOOST_CHECK_THROW(old->read(0, 10), std::runtime_error);
    old.reset();
    BOOST_CHECK(fixture.repo->has("/replace/object"));
    BOOST_CHECK_EQUAL(replacement->read(0, 10).size(), 10U);
    replacement.reset();
    BOOST_CHECK(!fixture.repo->has("/replace/object"));
  }
}

BOOST_AUTO_TEST_CASE(StaleTransactionCannotAbortOrOverwriteAnotherReservation)
{
  RepoFixture fixture;
  const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
  RepoObjectManifest old;
  old.objectName = "/reservation/object";
  old.operationId = "old-operation";
  old.size = bytes.size();
  old.sha256 = sha256Hex(bytes);
  fixture.repo->putRangeIfAbsent(old, {0, bytes.size()}, bytes);
  BOOST_REQUIRE(fixture.repo->abortRangesIfOwned(old));
  auto current = old;
  current.operationId = "replacement-operation";
  fixture.repo->putRangeIfAbsent(current, {0, bytes.size()}, bytes);
  BOOST_CHECK(!fixture.repo->abortRangesIfOwned(old));
  BOOST_CHECK_THROW(fixture.repo->commitRangesIfOwned(old), std::runtime_error);
  const auto committed = fixture.repo->commitRangesIfOwned(current);
  BOOST_CHECK_THROW(fixture.repo->putRangeIfAbsent(old, {0, bytes.size()}, bytes), std::runtime_error);
  BOOST_CHECK(!fixture.repo->removeIfCurrent(old));
  BOOST_CHECK(fixture.repo->getRangeIfCurrent(committed, {0, bytes.size()}) == bytes);
  BOOST_CHECK(fixture.repo->removeIfCurrent(committed));
}

BOOST_AUTO_TEST_CASE(CancellationRollsBackOnlyItsStagingOrCommittedObject)
{
  // Three windows: checks 1=start, 2..4=hash, 5..7=write,
  // 8=before durable commit, 9=after durable commit.
  for (unsigned cancelAt : {1U, 3U, 6U, 9U}) {
    RepoFixture fixture;
    RepoEncryptedLargeDataStore store(fixture.repo);
    const auto input = fixture.root / "cancel.bin";
    const std::vector<char> bytes((2U << 20) + 1, 'c');
    { std::ofstream stream(input, std::ios::binary); stream.write(bytes.data(), bytes.size()); }
    unsigned checks = 0;
    BOOST_CHECK_THROW(store.commitFile("/cancel/object", input, bytes.size(), [&] {
      if (++checks == cancelAt) throw std::runtime_error("cancelled");
    }), std::runtime_error);
    BOOST_CHECK_EQUAL(checks, cancelAt);
    BOOST_CHECK(!fixture.repo->has("/cancel/object"));
    BOOST_CHECK(std::filesystem::exists(input));
    // A successful retry proves that Core/backend reservations were released.
    auto retry = store.commitFile("/cancel/object", input, bytes.size());
    BOOST_CHECK_EQUAL(retry->read(0, 8).size(), 8U);
    retry.reset();
    BOOST_CHECK(!fixture.repo->has("/cancel/object"));
  }
}

BOOST_AUTO_TEST_SUITE_END()
