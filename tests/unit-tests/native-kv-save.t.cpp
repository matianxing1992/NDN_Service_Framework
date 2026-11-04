#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/fixtures/fixed-keychain-trust.hpp"
#include "tests/unit-tests/native-protected-kv-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-owner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"

#include "ndn-service-framework/ServiceUser.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework::test {
namespace {

namespace di = ndnsf::di;

struct TransportFixture
{
  std::filesystem::path directory;
  ndn::KeyChain keys{"pib-memory:spec221-save-route", "tpm-memory:spec221-save-route"};
  ndn::DummyClientFace face{keys};
  ndn::security::Certificate producer = makeRsaIdentity(keys, ndn::Name("/provider/save"));
  ndn::security::Certificate authority = makeRsaIdentity(keys, ndn::Name("/authority/save"));
  di::NativeProviderHandoffConfig config;

  TransportFixture()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "spec221-save-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::runtime_error("save fixture workspace allocation failed");
    directory = pattern;
    const auto schema = ndnsf_distributed_repo::tests::writeFixedKeychainTrust(directory, keys);
    config.storageWriter = std::make_shared<ServiceUser>(ServiceUser::LocalMockTag{},
      face, ndn::Name("/group/save"), producer, authority, schema);
    config.signingCertificate = producer;
    config.workspace = directory;
    config.repoNode = "/repo/logical";
    config.repoProvider = "/repo/provider";
    config.repoServicePrefix = "/Repo/Service";
  }

  ~TransportFixture()
  {
    config.storageWriter.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec221KvSave)

BOOST_AUTO_TEST_CASE(KvOriginalPacketReferenceSelectsTheExactRepoPacketPath)
{
  di::test::protectedKv::Fixture fixture;
  const auto protectedReference = fixture.protect();
  const auto certificate = fixture.ownerKeys.createIdentity(protectedReference.producerPrefix,
    ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const di::NativeProtectedKvPacketLimits limits{fixture.limits, 1000000, 128};
  const auto packets = di::nativePrepareProtectedKvPackets(fixture.cipher, protectedReference,
    certificate, limits, [&](ndn::Data& data) {
      fixture.ownerKeys.sign(data, ndn::security::signingByCertificate(certificate));
    }, [] {});
  const auto reference = di::nativeKvPacketReference(packets, certificate.getIdentity());
  std::vector<std::vector<std::uint8_t>> wires;
  for (const auto& wire : packets.packetWires)
    wires.emplace_back(wire.begin(), wire.end());
  BOOST_REQUIRE(reference.packetSetMetadata);
  BOOST_CHECK(reference.packetSetMetadata->dataPrefix.empty());
  BOOST_CHECK_EQUAL(reference.packetSetMetadata->envelopeBytes, 0U);
  BOOST_CHECK(ndn::Name(reference.dataPrefix) == certificate.getIdentity());

  ndnsf_distributed_repo::StorageCapability capability;
  capability.repoNode = "/repo/kv";
  capability.freeBytes = 1U << 20;
  const auto makeNode = [&](const char* label) {
    return std::make_unique<ndnsf_distributed_repo::RepoNode>(ndn::Name("/repo/service"),
      capability, ndnsf_distributed_repo::makeFilesystemRepoStore(
        (fixture.directory / label).string(), 1U << 20, 1U << 20));
  };
  auto oldReference = reference;
  oldReference.packetSetMetadata->dataPrefix = reference.dataPrefix;
  const auto rejected = makeNode("old-marking");
  const auto oldStatus = rejected->insertWirePackets(oldReference, wires);
  BOOST_CHECK_EQUAL(oldStatus.state, "FAILED");
  BOOST_CHECK_EQUAL(oldStatus.message, "BLOB_MANIFEST_ENVELOPE_INVALID");
  BOOST_CHECK(rejected->core().list().empty());

  const auto node = makeNode("exact-kv");
  BOOST_CHECK_EQUAL(node->insertWirePackets(reference, wires).state, "DONE");
  for (std::size_t i = 0; i < packets.packetNames.size(); ++i)
    BOOST_CHECK(node->getDataPacket(packets.packetNames[i].toUri()) == wires[i]);
  auto corrupted = wires;
  corrupted.back().back() ^= 1;
  BOOST_CHECK_EQUAL(node->insertWirePackets(reference, corrupted).state, "FAILED");
  for (std::size_t i = 0; i < packets.packetNames.size(); ++i)
    BOOST_CHECK(node->getDataPacket(packets.packetNames[i].toUri()) == wires[i]);
  BOOST_CHECK_THROW(di::nativeKvPacketReference(packets, ndn::Name("/other/producer")),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnerReadBudgetBoundsTheCompleteRoleSetBeforeTransport)
{
  using Limits = ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits;
  const auto validate = [](std::uint64_t roles, const Limits& limits) {
    di::NativeKvHandoffOwner::validateReadBudget(roles, limits);
  };
  const Limits defaults;
  BOOST_CHECK_NO_THROW(validate(4, defaults));
  BOOST_CHECK_THROW(validate(5, defaults), std::runtime_error);
  BOOST_CHECK_THROW(validate(0, defaults), std::runtime_error);
  BOOST_CHECK_THROW(validate(std::numeric_limits<std::uint64_t>::max(), defaults),
                    std::runtime_error);

  Limits small;
  small.maxListingPagesPerScan = 1;
  small.maxListedObjectsPerScan = 1;
  small.maxListingWireBytes = 1;
  small.maxPacketCount = 1;
  small.maxPacketWireBytes = 1;
  small.maxManifestWireBytes = 1;
  BOOST_CHECK_NO_THROW(validate(16, small));
  const std::vector<std::pair<std::function<void(Limits&, std::uint64_t)>, std::uint64_t>>
    boundaries{
      {[](Limits& l, std::uint64_t n) {
        l.maxListingPagesPerScan = static_cast<std::uint32_t>(n); }, 2048 / 3},
      {[](Limits& l, std::uint64_t n) { l.maxListedObjectsPerScan = n; }, 204800 / 3},
      {[](Limits& l, std::uint64_t n) { l.maxListingWireBytes = n; }, 96ULL << 20},
      {[](Limits& l, std::uint64_t n) {
        l.maxPacketCount = static_cast<std::uint32_t>(n); }, 16384},
      {[](Limits& l, std::uint64_t n) { l.maxPacketWireBytes = n; }, 256ULL << 20},
      {[](Limits& l, std::uint64_t n) { l.maxManifestWireBytes = n; }, 16ULL << 20}};
  for (const auto& boundary : boundaries) {
    auto limits = small;
    boundary.first(limits, boundary.second);
    BOOST_CHECK_NO_THROW(validate(1, limits));
    boundary.first(limits, boundary.second + 1);
    BOOST_CHECK_THROW(validate(1, limits), std::runtime_error);
    boundary.first(limits, 0);
    BOOST_CHECK_THROW(validate(1, limits), std::runtime_error);
  }
  small.maxListingWireBytes = std::numeric_limits<std::uint64_t>::max();
  BOOST_CHECK_THROW(validate(1, small), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RejectsForeignWriterCertificateAndUnsafeWorkspaceBeforeRegistration)
{
  TransportFixture fixture;
  auto& config = fixture.config;
  const auto producer = fixture.producer.getIdentity();
  BOOST_CHECK_NO_THROW(di::nativeValidateKvSaveConfig(config, producer));
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, ndn::Name("/foreign")),
    std::invalid_argument);
  const auto certificate = config.signingCertificate;
  config.signingCertificate = fixture.authority;
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, producer), std::invalid_argument);
  config.signingCertificate = certificate;
  config.repoProvider = producer.toUri();
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, producer), std::invalid_argument);
  config.repoProvider = "/repo/provider";
  BOOST_REQUIRE_EQUAL(::chmod(fixture.directory.c_str(), 0755), 0);
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, producer), std::invalid_argument);
  BOOST_REQUIRE_EQUAL(::chmod(fixture.directory.c_str(), 0700), 0);
  const auto alias = fixture.directory / "alias";
  std::filesystem::create_directory_symlink(fixture.directory, alias);
  config.workspace = alias;
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, producer), std::invalid_argument);
  config.workspace = fixture.directory;
  config.maxStateBytes = 0;
  BOOST_CHECK_THROW(di::nativeValidateKvSaveConfig(config, producer), std::invalid_argument);
}

using di::NativeDurableStorageGrantBarrierV1;
using di::NativeDurableStorageRoleGrantChallengeV1;
using di::NativeDurableStorageRoleGrantV1;

BOOST_AUTO_TEST_CASE(DurableGrantBarrierReleasesAllRolesTogether)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto deadline = Barrier::Clock::now() + std::chrono::seconds(5);
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}}, deadline);

  auto encoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });
  auto decoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });

  const auto challenges = barrier->waitForAllChallenges();
  BOOST_REQUIRE_EQUAL(challenges.size(), 2U);
  BOOST_CHECK_EQUAL(challenges[0].roleName, "decoder");
  BOOST_CHECK_EQUAL(challenges[0].manifestName.toUri(), "/provider/b/kv/manifest/1");
  BOOST_CHECK_EQUAL(challenges[1].roleName, "encoder");
  BOOST_CHECK_EQUAL(challenges[1].manifestName.toUri(), "/provider/a/kv/manifest/1");
  BOOST_CHECK(encoder.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  BOOST_CHECK(decoder.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

  Barrier::GrantSet grants;
  for (const auto& challenge : challenges) {
    const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
    grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
      "/repo", "/owner", grantId, challenge.writerIdentity,
      challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
  }
  barrier->releaseAll(std::move(grants));

  BOOST_REQUIRE(encoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_REQUIRE(decoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(encoder.get().grantId, "grant-encoder");
  BOOST_CHECK_EQUAL(decoder.get().grantId, "grant-decoder");
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsPartialAndReboundGrantSets)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto makeBarrier = [] {
    return std::make_shared<Barrier>("/owner",
      Barrier::ProducerPrefixesByRole{
        {"encoder", ndn::Name("/provider/a")},
        {"decoder", ndn::Name("/provider/b")}},
      Barrier::Clock::now() + std::chrono::seconds(5));
  };
  const auto startPublishers = [](const std::shared_ptr<Barrier>& barrier) {
    auto encoder = std::async(std::launch::async, [barrier] {
      return barrier->resolverFor("encoder")(
        ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
    });
    auto decoder = std::async(std::launch::async, [barrier] {
      return barrier->resolverFor("decoder")(
        ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
    });
    return std::make_pair(std::move(encoder), std::move(decoder));
  };
  const auto completeGrants = [](const std::vector<NativeDurableStorageRoleGrantChallengeV1>& challenges) {
    Barrier::GrantSet grants;
    for (const auto& challenge : challenges) {
      const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
      grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
        "/repo", "/owner", grantId, challenge.writerIdentity,
        challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
    }
    return grants;
  };

  for (int mode = 0; mode < 2; ++mode) {
    auto barrier = makeBarrier();
    auto publishers = startPublishers(barrier);
    const auto challenges = barrier->waitForAllChallenges();
    auto grants = completeGrants(challenges);
    if (mode == 0) {
      grants.erase("decoder");
    }
    else {
      grants.at("encoder").objectName = "/provider/a/kv/manifest/rebound";
    }
    BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
    BOOST_REQUIRE(publishers.first.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    BOOST_REQUIRE(publishers.second.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    BOOST_CHECK_THROW(publishers.first.get(), std::runtime_error);
    BOOST_CHECK_THROW(publishers.second.get(), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsWrongProducerAndAbortsOtherRoles)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::seconds(5));
  auto wrong = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  BOOST_CHECK_THROW(wrong.get(), std::runtime_error);
  auto other = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  BOOST_CHECK_THROW(other.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierDeadlineAbortsIncompleteRoleSet)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::milliseconds(100));
  auto publisher = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });

  BOOST_CHECK_THROW(barrier->waitForAllChallenges(), std::runtime_error);
  BOOST_REQUIRE(publisher.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(publisher.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsSwappedRoleGrants)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::seconds(5));
  auto encoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });
  auto decoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  const auto challenges = barrier->waitForAllChallenges();
  Barrier::GrantSet grants;
  for (const auto& challenge : challenges) {
    const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
    grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
      "/repo", "/owner", grantId, challenge.writerIdentity,
      challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
  }
  std::swap(grants.at("encoder"), grants.at("decoder"));
  BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
  BOOST_REQUIRE(encoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_REQUIRE(decoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(encoder.get(), std::runtime_error);
  BOOST_CHECK_THROW(decoder.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsGrantSetReleasedAfterDeadline)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto deadline = Barrier::Clock::now() + std::chrono::seconds(1);
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{{"encoder", ndn::Name("/provider/a")}}, deadline);
  std::promise<void> callbackEntered;
  std::promise<void> resumeCallback;
  auto resume = resumeCallback.get_future().share();
  auto callbackCount = std::make_shared<unsigned>(0);
  auto current = [callbackCount, &callbackEntered, resume] {
    if ((*callbackCount)++ == 1) {
      callbackEntered.set_value();
      resume.wait();
    }
  };
  auto publisher = std::async(std::launch::async, [barrier, current] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", current);
  });
  const auto challenges = barrier->waitForAllChallenges();
  BOOST_REQUIRE_EQUAL(challenges.size(), 1U);
  auto callbackEnteredFuture = callbackEntered.get_future();
  BOOST_REQUIRE(callbackEnteredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  std::this_thread::sleep_until(deadline + std::chrono::milliseconds(5));
  Barrier::GrantSet grants{{"encoder", NativeDurableStorageRoleGrantV1{
    "/repo", "/owner", "grant-encoder", "/provider/a",
    "/provider/a/kv/manifest/1", 1}}};
  BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
  resumeCallback.set_value();
  BOOST_REQUIRE(publisher.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(publisher.get(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
