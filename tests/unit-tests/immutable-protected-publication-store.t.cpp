#include "tests/boost-test.hpp"
#include "ndn-service-framework/ImmutableProtectedPublicationStore.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
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

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
