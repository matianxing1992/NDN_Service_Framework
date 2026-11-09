#include "tests/boost-test.hpp"
#include "ndn-service-framework/ImmutableProtectedPublicationStore.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <future>
#include <limits>

namespace ndn_service_framework::test {
namespace {
using Store = ImmutableProtectedPublicationStore;
using Status = Store::Status;
const ndn::Name producer("/test/source");
const ndn::Name service("/UAV/Tracking");
const ndn::Name semantic("/test/source/UAV/CLOCK/1");
const ControllerVersion generation{100, 1};
const std::string digest = "sha256:" + std::string(64, 'a');

struct Fixture
{
  ndn::KeyChain keyChain{"pib-memory:", "tpm-memory:"};

  std::shared_ptr<const ndn::Data>
  packet(const ndn::Name& name, uint64_t final = 0, size_t bytes = 32, uint8_t value = 7)
  {
    auto data = std::make_shared<ndn::Data>(name);
    data->setFreshnessPeriod(ndn::time::seconds(1));
    data->setFinalBlock(ndn::name::Component::fromSegment(final));
    ndn::Buffer content;
    content.assign(bytes, value);
    data->setContent(content);
    keyChain.sign(*data, ndn::security::signingWithSha256());
    return data;
  }

  Store::Packets complete(const ndn::Name& name)
  {
    return {packet(ndn::Name(name).appendSegment(0))};
  }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(ImmutableProtectedPublicationStoreTests, Fixture)

BOOST_AUTO_TEST_CASE(ReusesOriginalNameAndWireAndSeparatesServiceAndProtectionVersions)
{
  Store store(producer, 50);
  auto first = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(first.status == Status::Reserved);
  auto packets = complete(first.dataName);
  const auto wire = packets.front()->wireEncode();
  BOOST_REQUIRE(store.commit(first.ticket, packets));
  for (int request = 0; request < 4; ++request) {
    const auto reused = store.begin(service, generation, semantic, digest, 32);
    BOOST_REQUIRE(reused.status == Status::Reused);
    BOOST_CHECK_EQUAL(reused.dataName, first.dataName);
    BOOST_CHECK(reused.publication->packets.front()->wireEncode() == wire);
    BOOST_CHECK(reused.publication->packets.front() == packets.front());
  }
  auto changedPolicy = store.begin(service, ControllerVersion{100, 2}, semantic, digest, 32);
  BOOST_REQUIRE(changedPolicy.status == Status::Reserved);
  BOOST_REQUIRE(store.commit(changedPolicy.ticket, complete(changedPolicy.dataName)));
  auto changedService = store.begin(ndn::Name("/Other"), generation,
                                   semantic, digest, 32);
  BOOST_REQUIRE(changedService.status == Status::Reserved);
  BOOST_CHECK_NE(changedPolicy.dataName, first.dataName);
  BOOST_CHECK_NE(changedPolicy.dataName.at(-1), changedService.dataName.at(-1));
  store.rollback(changedPolicy.ticket);
  store.rollback(changedService.ticket);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(RevisionConflictAndFailedPreparationNeverExposePartialPackets)
{
  Store store(producer, 70);
  auto first = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(first.status == Status::Reserved);
  BOOST_CHECK(store.begin(service, generation, semantic, digest, 32).status == Status::Busy);
  auto incomplete = Store::Packets{packet(ndn::Name(first.dataName).appendSegment(0), 1)};
  BOOST_CHECK(!store.commit(first.ticket, incomplete));
  BOOST_CHECK(!store.find(ndn::Interest(first.dataName).setCanBePrefix(true)));
  BOOST_CHECK_EQUAL(store.size(), 0);
  store.rollback(first.ticket);
  auto retry = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(retry.status == Status::Reserved);
  BOOST_REQUIRE(store.commit(retry.ticket, complete(retry.dataName)));
  const std::string changed = "sha256:" + std::string(64, 'b');
  BOOST_CHECK(store.begin(service, generation, semantic, changed, 32).status ==
              Status::IdentityConflict);
  BOOST_CHECK(store.begin(ndn::Name("/Other"), generation, semantic, changed, 32).status ==
              Status::IdentityConflict);
  BOOST_CHECK(store.begin(service, generation, ndn::Name("/test/source/UAV/CLOCK/2"),
                          changed, 32).status == Status::Reserved);
}

BOOST_AUTO_TEST_CASE(ChangingTransportAllocatesANewVersionWithoutChangingOriginalWire)
{
  Store store(producer, 700);
  const auto plain = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(plain.status == Status::Reserved);
  const auto originals = complete(plain.dataName);
  BOOST_REQUIRE(store.commit(plain.ticket, originals));
  const auto oldWire = originals.front()->wireEncode();
  const auto fec = store.begin(service, generation, semantic, digest, 32, true);
  BOOST_REQUIRE(fec.status == Status::Reserved);
  BOOST_CHECK(fec.dataName != plain.dataName);
  const auto reused = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(reused.status == Status::Reused);
  BOOST_CHECK(reused.dataName == plain.dataName);
  BOOST_CHECK(reused.publication->packets.front()->wireEncode() == oldWire);
  store.rollback(fec.ticket);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(CompleteMultiSegmentPublicationKeepsEveryOriginalAndAuxiliaryKey)
{
  Store store(producer, 80);
  auto item = store.begin(service, generation, semantic, digest, 64);
  Store::Packets packets{packet(ndn::Name(item.dataName).appendSegment(0), 1),
                        packet(ndn::Name(item.dataName).appendSegment(1), 1),
                        packet(ndn::Name("/test/source/NDNSF/key"))};
  BOOST_REQUIRE(store.commit(item.ticket, packets));
  for (const auto& original : packets) {
    const auto fetched = store.find(ndn::Interest(original->getFullName()));
    BOOST_REQUIRE(fetched);
    BOOST_CHECK(fetched->wireEncode() == original->wireEncode());
  }
}

BOOST_AUTO_TEST_CASE(UndeclaredDuplicateOrNonSegmentChildrenAreNotExposed)
{
  for (unsigned kind = 0; kind < 4; ++kind) {
    Store store(producer, 90);
    auto item = store.begin(service, generation, semantic, digest, 64);
    auto packets = complete(item.dataName);
    if (kind == 0) packets.push_back(packet(ndn::Name(item.dataName).appendSegment(1)));
    if (kind == 1) packets.push_back(packets.front());
    if (kind == 2) packets.push_back(packet(ndn::Name(item.dataName).append("child")));
    if (kind == 3) packets.push_back(packet(ndn::Name(item.dataName).appendSegment(1).append("child")));
    BOOST_CHECK(!store.commit(item.ticket, packets));
    BOOST_CHECK_EQUAL(store.size(), 0);
    for (const auto& original : packets) BOOST_CHECK(!store.find(ndn::Interest(original->getFullName())));
    store.rollback(item.ticket);
    BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
  }
}

BOOST_AUTO_TEST_CASE(OriginalLookupSurvivesCallerReleaseAndSupportsImplicitDigest)
{
  Store store(producer, 100);
  auto first = store.begin(service, generation, semantic, digest, 32);
  auto packets = complete(first.dataName);
  const auto fullName = packets.front()->getFullName();
  const auto wire = packets.front()->wireEncode();
  BOOST_REQUIRE(store.commit(first.ticket, packets));
  packets.clear();
  const auto discovered = store.find(ndn::Interest(first.dataName).setCanBePrefix(true));
  BOOST_REQUIRE(discovered);
  BOOST_CHECK(discovered->wireEncode() == wire);
  const auto exact = store.find(ndn::Interest(fullName));
  BOOST_REQUIRE(exact);
  BOOST_CHECK(exact->wireEncode() == wire);
  BOOST_CHECK(!store.find(ndn::Interest(ndn::Name(first.dataName).appendSegment(1))));
}

BOOST_AUTO_TEST_CASE(PacketNameConflictRollsBackWithoutChangingOriginal)
{
  Store store(producer, 200);
  const ndn::Name keyName("/test/source/NDNSF/key");
  auto first = store.begin(service, generation, semantic, digest, 32);
  auto packets = complete(first.dataName);
  packets.push_back(packet(keyName));
  BOOST_REQUIRE(store.commit(first.ticket, packets));
  const auto original = store.find(ndn::Interest(keyName));
  auto next = store.begin(service, generation, ndn::Name("/test/source/UAV/GROUND/1"), digest, 32);
  auto conflicting = complete(next.dataName);
  conflicting.push_back(packet(keyName, 0, 32, 8));
  BOOST_CHECK(!store.commit(next.ticket, conflicting));
  store.rollback(next.ticket);
  BOOST_CHECK_EQUAL(store.size(), 1);
  BOOST_CHECK(store.find(ndn::Interest(keyName)) == original);
  BOOST_CHECK(!store.find(ndn::Interest(next.dataName).setCanBePrefix(true)));
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(AdmissionAndWireBudgetsFailBeforeExposure)
{
  Store store(producer, 300, Store::Limits{2, 1024, 64, 512});
  BOOST_CHECK(store.begin(service, generation, semantic, digest, 65).status == Status::Invalid);
  BOOST_CHECK(store.begin(service, generation, ndn::Name("/wrong/UAV/CLOCK/1"), digest, 32)
                .status == Status::Invalid);
  auto first = store.begin(service, generation, semantic, digest, 64);
  auto next = store.begin(service, generation, ndn::Name("/test/source/UAV/GROUND/1"), digest, 64);
  BOOST_REQUIRE(first.status == Status::Reserved);
  BOOST_REQUIRE(next.status == Status::Reserved);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 1024);
  BOOST_CHECK(store.begin(service, generation, ndn::Name("/test/source/UAV/GROUND/2"), digest, 32)
                .status == Status::Capacity);
  BOOST_CHECK(!store.commit(first.ticket,
    {packet(ndn::Name(first.dataName).appendSegment(0), 0, 600)}));
  store.rollback(first.ticket);
  store.rollback(next.ticket);
  BOOST_CHECK_EQUAL(store.retainedWireBytes(), 0);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(DefaultEntryLimitAndVersionExhaustionAreBounded)
{
  Store store(producer, 400);
  for (uint64_t revision = 1; revision <= 128; ++revision) {
    const auto prefix = ndn::Name("/test/source/UAV/CLOCK").append(std::to_string(revision));
    auto item = store.begin(service, generation, prefix, digest, 32);
    BOOST_REQUIRE(item.status == Status::Reserved);
    BOOST_REQUIRE(store.commit(item.ticket, complete(item.dataName)));
  }
  BOOST_CHECK_EQUAL(store.size(), 128);
  BOOST_CHECK(store.begin(service, generation, ndn::Name("/test/source/UAV/GROUND/1"), digest, 32)
                .status == Status::Capacity);
  Store last(producer, std::numeric_limits<uint64_t>::max());
  auto item = last.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(item.status == Status::Reserved);
  BOOST_CHECK_EQUAL(item.dataName.at(-1).toVersion(), std::numeric_limits<uint64_t>::max());
  last.rollback(item.ticket);
  BOOST_CHECK(last.begin(service, generation, semantic, digest, 32).status == Status::Capacity);
}

BOOST_AUTO_TEST_CASE(ExactNamesBindContentAndProtectionAndReuseOriginalPackets)
{
  Store store(producer, 1);
  const auto name = ndn::Name(semantic).appendVersion(7);
  const auto protection = "sha256:" + std::string(64, 'b');
  const auto changed = "sha256:" + std::string(64, 'c');
  const auto first = store.beginExact(name, digest, protection, 32, 1024);
  BOOST_REQUIRE(first.status == Status::Reserved);
  BOOST_CHECK_EQUAL(first.dataName, name);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 1024);
  BOOST_CHECK(store.beginExact(name, digest, protection, 32, 1024).status == Status::Busy);
  BOOST_CHECK(store.beginExact(name, digest, changed, 32, 1024).status ==
              Status::IdentityConflict);
  auto packets = complete(name);
  const auto original = packets.front();
  const auto wire = original->wireEncode();
  BOOST_REQUIRE(store.commit(first.ticket, packets));
  packets.clear();
  const auto replay = store.beginExact(name, digest, protection, 32, 1024);
  BOOST_REQUIRE(replay.status == Status::Reused);
  BOOST_CHECK(replay.publication->packets.front() == original);
  BOOST_CHECK(replay.publication->packets.front()->wireEncode() == wire);
  BOOST_CHECK_EQUAL(replay.publication->protectionSha256, protection);
  BOOST_CHECK(store.beginExact(name, changed, protection, 32, 1024).status ==
              Status::IdentityConflict);
  BOOST_CHECK(store.beginExact(name, digest, changed, 32, 1024).status ==
              Status::IdentityConflict);
  BOOST_CHECK(store.find(ndn::Interest(original->getFullName())) == original);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(ExactPreparationIsAtomicAndReleasesItsVariableReservation)
{
  Store store(producer, 1, Store::Limits{4, 2048, 256, 1024});
  const auto name = ndn::Name(semantic).appendVersion(9);
  const auto protection = "sha256:" + std::string(64, 'd');
  auto item = store.beginExact(name, digest, protection, 32, 512);
  BOOST_REQUIRE(item.status == Status::Reserved);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 512);
  BOOST_CHECK(!store.commit(item.ticket, {packet(ndn::Name(name).appendSegment(0), 1)}));
  BOOST_CHECK(!store.find(ndn::Interest(name).setCanBePrefix(true)));
  store.rollback(item.ticket);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
  item = store.beginExact(name, digest, protection, 32, 1024);
  auto mixed = complete(name);
  mixed.push_back(packet(ndn::Name("/test/source/NDNSF/key")));
  BOOST_CHECK(!store.commit(item.ticket, mixed));
  BOOST_CHECK_EQUAL(store.size(), 0);
  store.rollback(item.ticket);
  item = store.beginExact(name, digest, protection, 32, 512);
  BOOST_REQUIRE(store.commit(item.ticket, complete(name)));
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
  BOOST_CHECK(store.beginExact(name, digest, "not-a-digest", 32, 512).status == Status::Invalid);
  BOOST_CHECK(store.beginExact(name, digest, protection, 257, 512).status == Status::Invalid);
  BOOST_CHECK(store.beginExact(name, digest, protection, 32, 1025).status == Status::Invalid);
}

BOOST_AUTO_TEST_CASE(ExactCapacityDenialNeverEvictsIdentityOrOriginalWire)
{
  const auto name = ndn::Name(semantic).appendVersion(10);
  const auto protection = "sha256:" + std::string(64, 'e');
  const auto packets = complete(name);
  const auto bytes = packets.front()->wireEncode().size();
  Store store(producer, 1, Store::Limits{1, bytes, 64, bytes});
  const auto item = store.beginExact(name, digest, protection, 32, bytes);
  BOOST_REQUIRE(item.status == Status::Reserved);
  BOOST_REQUIRE(store.commit(item.ticket, packets));
  BOOST_CHECK_EQUAL(store.retainedWireBytes(), bytes);
  BOOST_CHECK(store.beginExact(ndn::Name(semantic).appendVersion(11), digest,
                              protection, 32, bytes).status == Status::Capacity);
  const auto replay = store.beginExact(name, digest, protection, 32, bytes);
  BOOST_REQUIRE(replay.status == Status::Reused);
  BOOST_CHECK(replay.publication->packets.front() == packets.front());
  BOOST_CHECK(store.beginExact(name, digest, "sha256:" + std::string(64, 'f'),
                              32, bytes).status == Status::IdentityConflict);
  BOOST_CHECK_EQUAL(store.size(), 1);
  BOOST_CHECK_EQUAL(store.reservedWireBytes(), 0);
}

BOOST_AUTO_TEST_CASE(ExactDomainPreservesParentPolicyAndRejectsCrossDomainConflicts)
{
  Store store(producer, 1);
  const auto name = ndn::Name(semantic).appendVersion(30);
  const auto protection = "sha256:" + std::string(64, 'b');
  const Store::Limits limits{2, 2048, 64, 1024};
  const auto originals = complete(name);
  const auto item = store.beginExact(limits, name, digest, protection, 32, 1024);
  BOOST_REQUIRE(item.status == Status::Reserved);
  BOOST_REQUIRE(store.commitExact(item.ticket, originals));
  BOOST_CHECK(store.find(ndn::Interest(originals.front()->getFullName())) == originals.front());
  BOOST_CHECK_EQUAL(store.size(), 0);
  BOOST_CHECK_EQUAL(store.retainedWireBytes(), 0);
  BOOST_CHECK_THROW(store.beginExact(Store::Limits{3, 2048, 64, 1024}, name,
    digest, protection, 32, 1024), std::invalid_argument);

  const auto legacy = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(legacy.status == Status::Reserved);
  auto mixed = complete(legacy.dataName);
  mixed.push_back(packet(ndn::Name(name).appendSegment(0), 0, 32, 99));
  BOOST_CHECK(!store.commit(legacy.ticket, mixed));
  store.rollback(legacy.ticket);
  BOOST_CHECK(store.find(ndn::Interest(originals.front()->getFullName())) == originals.front());

  const auto valid = store.begin(service, generation, semantic, digest, 32);
  BOOST_REQUIRE(valid.status == Status::Reserved);
  BOOST_REQUIRE(store.commit(valid.ticket, complete(valid.dataName)));
  BOOST_CHECK_THROW(store.beginExact(limits, valid.dataName, digest, protection, 32, 1024),
                    std::invalid_argument);
  BOOST_CHECK_EQUAL(store.size(), 1);
}

BOOST_AUTO_TEST_CASE(ConcurrentExactHitsKeepOneOriginalOwner)
{
  Store store(producer, 1);
  const auto name = ndn::Name(semantic).appendVersion(40);
  const auto protection = "sha256:" + std::string(64, 'c');
  const Store::Limits limits{2, 2048, 64, 1024};
  const auto originals = complete(name);
  const auto item = store.beginExact(limits, name, digest, protection, 32, 1024);
  BOOST_REQUIRE(item.status == Status::Reserved);
  BOOST_REQUIRE(store.commitExact(item.ticket, originals));
  std::vector<std::future<bool>> readers;
  for (unsigned worker = 0; worker < 4; ++worker) {
    readers.push_back(std::async(std::launch::async, [&] {
      for (unsigned retry = 0; retry < 32; ++retry) {
        const auto hit = store.beginExact(limits, name, digest, protection, 32, 1024);
        if (hit.status != Status::Reused || hit.publication->packets.front() != originals.front() ||
            store.find(ndn::Interest(originals.front()->getFullName())) != originals.front()) {
          return false;
        }
      }
      return true;
    }));
  }
  for (auto& reader : readers) BOOST_CHECK(reader.get());
  BOOST_CHECK(store.find(ndn::Interest(originals.front()->getFullName())) == originals.front());
}

BOOST_AUTO_TEST_CASE(ExactObjectKeyCommitsAtomicallyWithOriginalPackets)
{
  Store store(producer, 1);
  const auto name = ndn::Name(producer).append("object").appendVersion(1);
  const auto protection = "sha256:" + std::string(64, 'c');
  const Store::Limits limits{2, 2048, 64, 1024};
  const auto originals = complete(name);
  const auto item = store.beginExact(limits, name, digest, protection, 32, 1024);
  BOOST_REQUIRE(item.status == Status::Reserved);
  for (size_t length : {31U, 33U}) {
    ndn::Buffer malformed;
    malformed.assign(length, 0x57);
    BOOST_CHECK(!store.commitExact(item.ticket, originals, malformed));
    BOOST_CHECK(!store.find(ndn::Interest(originals.front()->getName())));
  }
  const ndn::Buffer key(32, 0x57);
  BOOST_REQUIRE(store.commitExact(item.ticket, originals, key));
  const auto verify = [&] {
    for (unsigned i = 0; i < 32; ++i) {
      const auto hit = store.beginExact(limits, name, digest, protection, 32, 1024);
      if (hit.status != Status::Reused || hit.publication->objectKey != key ||
          hit.publication->packets.front() != originals.front()) return false;
    }
    return true;
  };
  auto first = std::async(std::launch::async, verify);
  auto second = std::async(std::launch::async, verify);
  BOOST_CHECK(first.get());
  BOOST_CHECK(second.get());
  BOOST_CHECK(store.find(ndn::Interest(originals.front()->getName())) == originals.front());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
