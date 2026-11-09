#include "tests/boost-test.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "ndn-service-framework/detail/nac-consumer-transport.hpp"

#include <nac-abe/attribute-authority.hpp>
#include <nac-abe/cache-producer.hpp>
#include <nac-abe/consumer.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>

namespace ndn_service_framework::test {
namespace {
using detail::NacConsumerTransport;

ndn::security::Certificate makeCertificate(ndn::KeyChain& keyChain, const ndn::Name& name)
{
  return keyChain.createIdentity(name, ndn::RsaKeyParams(2048)).getDefaultKey()
    .getDefaultCertificate();
}

struct Observation
{
  size_t success = 0;
  size_t failure = 0;
  size_t authorizedPackets = 0;
  bool wireMatches = true;
  bool current = true;
  bool authorized = true;
  ndn::Buffer plaintext;
  std::string error;
};

struct OwnedTrustDirectory
{
  std::filesystem::path path{trust_detail::makeTestTrustDirectory()};
  ~OwnedTrustDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct SdkFixture
{
  ndn::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  boost::asio::io_context io;
  ndn::DummyClientFace wire{io, keyChain, {false, true}};
  ndn::security::Certificate producerCert{makeCertificate(keyChain, ndn::Name("/test/producer"))};
  ndn::security::Certificate consumerCert{makeCertificate(keyChain, ndn::Name("/test/consumer"))};
  ndn::security::Certificate authorityCert{makeCertificate(keyChain, ndn::Name("/test/authority"))};
  OwnedTrustDirectory trustDirectory;
  ndn::security::ValidatorConfig validator{wire};
  std::shared_ptr<NacConsumerTransport> transport{std::make_shared<NacConsumerTransport>(wire)};
  ndn::Face sdkFace{transport, io, keyChain};
  ndn::signal::ScopedConnection interests;
  ndn::signal::ScopedConnection data;
  std::unique_ptr<ndn::nacabe::KpAttributeAuthority> authority;
  std::unique_ptr<ndn::nacabe::CacheProducer> producer;
  std::unique_ptr<ndn::nacabe::Consumer> consumer;
  std::vector<std::shared_ptr<const ndn::Data>> keys;
  std::shared_ptr<ndn::Data> encrypted;
  size_t networkCkInterests = 0;
  const ndn::Buffer plaintext{3, 1, 4, 1, 5};

  SdkFixture()
  {
    trust_detail::writeTestTrustSchema(trustDirectory.path,
      {producerCert, consumerCert, authorityCert});
    validator.load((trustDirectory.path / "trust.conf").string());
    interests = wire.onSendInterest.connect([this](const ndn::Interest& interest) {
      if (ndn::Name(producerCert.getIdentity()).append("CK").isPrefixOf(interest.getName()))
        ++networkCkInterests;
      wire.receive(interest);
    });
    data = wire.onSendData.connect([this](const ndn::Data& packet) { wire.receive(packet); });
    authority = std::make_unique<ndn::nacabe::KpAttributeAuthority>(
      authorityCert, wire, validator, keyChain);
    authority->addNewPolicy(consumerCert, "/SERVICE/test");
    pump([] { return false; }, std::chrono::milliseconds(30));
    producer = std::make_unique<ndn::nacabe::CacheProducer>(
      wire, keyChain, validator, producerCert, authorityCert);
    consumer = std::make_unique<ndn::nacabe::Consumer>(
      sdkFace, keyChain, validator, consumerCert, authorityCert);
    consumer->obtainDecryptionKey();
    pump([this] {
      return consumer->readyForDecryption() && !producer->getPublicParamsDataName().empty();
    });
    if (!consumer->readyForDecryption() || producer->getPublicParamsDataName().empty())
      throw std::runtime_error("SDK transport fixture bootstrap failed: consumer=" +
        std::to_string(consumer->readyForDecryption()) + " producer=" +
        producer->getPublicParamsDataName().toUri());
    auto publication = producer->produce(
      ndn::Name(producerCert.getIdentity()).append("NDNSF").append("TRANSPORT").appendVersion(1),
      std::vector<std::string>{"/SERVICE/test"}, plaintext,
      ndn::security::signingByCertificate(producerCert));
    const auto& content = std::get<0>(publication);
    if (content.size() != 1) throw std::runtime_error("fixture cipher must fit one segment");
    encrypted = content.front();
    for (const auto& packet : std::get<1>(publication)) keys.push_back(packet);
    if (keys.empty()) throw std::runtime_error("fixture original CK packets missing");
  }

  ~SdkFixture()
  {
    if (consumer) consumer->stop();
    consumer.reset();
    if (producer) producer->stop();
    producer.reset();
    authority.reset();
    sdkFace.shutdown();
    pump([] { return false; }, std::chrono::milliseconds(50));
  }

  void pump(const std::function<bool()>& done,
            std::chrono::milliseconds duration = std::chrono::seconds(5))
  {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!done() && std::chrono::steady_clock::now() < end) {
      wire.processEvents(ndn::time::milliseconds(2));
      io.restart();
    }
  }

  std::weak_ptr<const void> consume(const std::shared_ptr<Observation>& observation)
  {
    auto lease = transport->provide(keys, [observation] { return observation->current; });
    std::weak_ptr<const void> lifetime = lease;
    const auto cipher = encrypted->getContent().blockFromValue();
    consumer->consumeWithCkAuthorization(encrypted->getName(), cipher,
      producerCert.getIdentity(),
      [observation, originals = keys](const ndn::Data& packet) {
        ++observation->authorizedPackets;
        const auto found = std::find_if(originals.begin(), originals.end(), [&](const auto& original) {
          return original->getName() == packet.getName();
        });
        observation->wireMatches &= found != originals.end() &&
          (*found)->wireEncode() == packet.wireEncode();
        return observation->authorized;
      },
      [observation] { return observation->current; },
      [observation, lease](const ndn::Buffer& result) {
        ++observation->success;
        observation->plaintext = result;
      },
      [observation, lease](const std::string& error) {
        ++observation->failure;
        observation->error = error;
      });
    return lifetime;
  }
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec226NacTransport)

BOOST_AUTO_TEST_CASE(CkLocatorRejectsForeignAndNoncanonicalNamesBeforeIo)
{
  const ndn::Name producer("/producer/site");
  const auto locator = [&](const std::string& id) {
    return ndn::Name(producer).append("CK").append(id).append("ENC-BY").append("policy");
  };
  BOOST_CHECK(detail::isNacCkLocator(locator("0"), producer));
  BOOST_CHECK(detail::isNacCkLocator(locator("4294967295"), producer));
  for (const auto* id : {"", "01", "-1", "+1", "4294967296", "1x"})
    BOOST_CHECK(!detail::isNacCkLocator(locator(id), producer));
  BOOST_CHECK(!detail::isNacCkLocator(locator("1"), ndn::Name("/other")));
  BOOST_CHECK(!detail::isNacCkLocator(ndn::Name(locator("1")).appendSegment(0), producer));
  BOOST_CHECK(!detail::isNacCkLocator(locator("1").getPrefix(-1).append(""), producer));
}

BOOST_AUTO_TEST_CASE(UnchangedSdkDecryptsOriginalPacketsWithoutCkNetworkInterests)
{
  SdkFixture fixture;
  auto observed = std::make_shared<Observation>();
  auto lifetime = fixture.consume(observed);
  fixture.pump([&] { return observed->success + observed->failure != 0; });
  BOOST_CHECK_MESSAGE(observed->error.empty(), observed->error);
  BOOST_CHECK_EQUAL(observed->success, 1);
  BOOST_CHECK_EQUAL(observed->failure, 0);
  BOOST_CHECK(observed->plaintext == fixture.plaintext);
  BOOST_CHECK_EQUAL(observed->authorizedPackets, fixture.keys.size());
  BOOST_CHECK(observed->wireMatches);
  BOOST_CHECK_EQUAL(fixture.networkCkInterests, 0);
  fixture.pump([&] { return lifetime.expired(); });
  BOOST_CHECK(lifetime.expired());
}

BOOST_AUTO_TEST_CASE(SdkStillRejectsInvalidOriginalSignatureAndRevokedAuthorization)
{
  for (bool badSignature : {false, true}) {
    SdkFixture fixture;
    auto warm = std::make_shared<Observation>();
    auto warmLifetime = fixture.consume(warm);
    fixture.pump([&] { return warm->success + warm->failure != 0; });
    BOOST_REQUIRE_EQUAL(warm->success, 1);
    fixture.pump([&] { return warmLifetime.expired(); });
    BOOST_REQUIRE(warmLifetime.expired());
    auto observed = std::make_shared<Observation>();
    observed->authorized = badSignature;
    if (badSignature) {
      auto changed = std::make_shared<ndn::Data>(*fixture.keys.front());
      auto content = changed->getContent();
      ndn::Buffer bytes(content.value_begin(), content.value_end());
      bytes.back() ^= 1;
      changed->setContent(bytes);
      fixture.keys.front() = changed;
    }
    auto lifetime = fixture.consume(observed);
    fixture.pump([&] { return observed->success + observed->failure != 0; });
    BOOST_CHECK_EQUAL(observed->success, 0);
    BOOST_CHECK_EQUAL(observed->failure, 1);
    BOOST_CHECK_EQUAL(fixture.networkCkInterests, 0);
    if (badSignature) BOOST_CHECK_EQUAL(observed->authorizedPackets, 0);
    fixture.pump([&] { return lifetime.expired(); });
    BOOST_CHECK(lifetime.expired());
  }
}

BOOST_AUTO_TEST_CASE(SdkCancellationReleasesTheOriginalPacketLease)
{
  SdkFixture fixture;
  auto observed = std::make_shared<Observation>();
  auto lifetime = fixture.consume(observed);
  fixture.consumer->stop();
  BOOST_CHECK_EQUAL(observed->success + observed->failure, 0);
  fixture.pump([&] { return observed->failure != 0; });
  BOOST_CHECK_EQUAL(observed->success, 0);
  BOOST_CHECK_EQUAL(observed->failure, 1);
  BOOST_CHECK_EQUAL(fixture.networkCkInterests, 0);
  fixture.pump([&] { return lifetime.expired(); });
  BOOST_CHECK(lifetime.expired());
}

BOOST_AUTO_TEST_CASE(OriginalPacketSetsAndLeaseCountAreBounded)
{
  SdkFixture fixture;
  BOOST_CHECK_THROW(fixture.transport->provide({}, [] { return true; }), std::invalid_argument);
  auto reordered = fixture.keys;
  auto wrong = std::make_shared<ndn::Data>(*reordered.front());
  wrong->setName(ndn::Name(wrong->getName().getPrefix(-1)).appendSegment(1));
  reordered.front() = wrong;
  BOOST_CHECK_THROW(fixture.transport->provide(reordered, [] { return true; }),
                    std::invalid_argument);
  auto oversized = fixture.keys;
  auto large = std::make_shared<ndn::Data>(*oversized.front());
  large->setContent(ndn::Buffer(ndn::MAX_NDN_PACKET_SIZE));
  oversized.front() = large;
  BOOST_CHECK_THROW(fixture.transport->provide(oversized, [] { return true; }),
                    std::invalid_argument);
  std::vector<NacConsumerTransport::PacketLease> leases;
  for (size_t i = 0; i < 128; ++i)
    leases.push_back(fixture.transport->provide(fixture.keys, [] { return true; }));
  BOOST_CHECK_THROW(fixture.transport->provide(fixture.keys, [] { return true; }),
                    std::invalid_argument);
  leases.front().reset();
  BOOST_CHECK(fixture.transport->provide(fixture.keys, [] { return true; }));
}

BOOST_AUTO_TEST_CASE(PauseAndCloseFenceQueuedBytesAndRevokedLeasesNeverUseTheNetwork)
{
  SdkFixture fixture;
  auto transport = std::make_shared<NacConsumerTransport>(fixture.wire);
  auto observed = std::make_shared<Observation>();
  transport->connect(fixture.io, [observed, original = fixture.keys.front()](const ndn::Block& wire) {
    if (wire.type() == ndn::tlv::Data) {
      ++observed->success;
      observed->wireMatches &= wire == original->wireEncode();
    }
    else ++observed->failure;
  });
  auto lease = transport->provide(fixture.keys, [observed] { return observed->current; });
  ndn::Interest interest(fixture.keys.front()->getName());
  transport->send(interest.wireEncode());
  transport->pause();
  transport->resume();
  fixture.pump([] { return false; }, std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(observed->success + observed->failure, 0);
  transport->send(interest.wireEncode());
  fixture.pump([&] { return observed->success != 0; });
  BOOST_CHECK_EQUAL(observed->success, 1);
  BOOST_CHECK(observed->wireMatches);
  observed->current = false;
  transport->send(interest.wireEncode());
  fixture.pump([&] { return observed->failure != 0; });
  BOOST_CHECK_EQUAL(observed->failure, 1);
  BOOST_CHECK_EQUAL(fixture.networkCkInterests, 0);
  transport->send(interest.wireEncode());
  transport->close();
  fixture.pump([] { return false; }, std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(observed->success + observed->failure, 2);
}

BOOST_AUTO_TEST_CASE(UnboundBootstrapInterestsHaveAnOwnedOutstandingLimit)
{
  SdkFixture fixture;
  auto transport = std::make_shared<NacConsumerTransport>(fixture.wire);
  auto observed = std::make_shared<Observation>();
  transport->connect(fixture.io, [observed](const ndn::Block&) { ++observed->failure; });
  size_t outgoing = 0;
  const ndn::Name prefix("/test/unavailable");
  ndn::signal::ScopedConnection trace = fixture.wire.onSendInterest.connect(
    [&](const ndn::Interest& interest) { if (prefix.isPrefixOf(interest.getName())) ++outgoing; });
  for (size_t i = 0; i < 129; ++i) {
    ndn::Interest interest(ndn::Name(prefix).appendNumber(i));
    transport->send(interest.wireEncode());
  }
  fixture.pump([] { return false; }, std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(outgoing, 128);
  BOOST_CHECK_EQUAL(observed->failure, 1);
  transport->close();
  fixture.pump([] { return false; }, std::chrono::milliseconds(20));
  BOOST_CHECK_EQUAL(observed->failure, 1);
}

BOOST_AUTO_TEST_CASE(LocalAndUpstreamDeliveryPreserveWireAndCongestionFeedback)
{
  SdkFixture fixture;
  auto observed = std::make_shared<Observation>();
  auto marked = std::make_shared<ndn::Data>(*fixture.keys.front());
  marked->setCongestionMark(7);
  auto keys = fixture.keys;
  keys.front() = marked;
  auto lease = fixture.transport->provide(keys, [] { return true; });
  auto receive = [observed](std::shared_ptr<const ndn::Data> original, uint64_t mark) {
    return [observed, original, mark](const ndn::Interest&, const ndn::Data& packet) {
      ++observed->success;
      observed->wireMatches &= packet.wireEncode() == original->wireEncode() &&
        packet.getCongestionMark() == mark;
    };
  };
  auto fail = [observed](const auto&...) { ++observed->failure; };
  fixture.sdkFace.expressInterest(ndn::Interest(marked->getName()), receive(marked, 7), fail, fail);
  fixture.pump([&] { return observed->success + observed->failure != 0; });
  BOOST_REQUIRE_EQUAL(observed->success, 1);
  auto response = std::make_shared<ndn::Data>(ndn::Name(fixture.producerCert.getIdentity())
    .append("TRANSPORT").append("proxy"));
  response->setContent(ndn::Buffer{1});
  fixture.keyChain.sign(*response, ndn::security::signingByCertificate(fixture.producerCert));
  response->setCongestionMark(11);
  ndn::ScopedInterestFilterHandle filter = fixture.wire.setInterestFilter(response->getName(),
    [&fixture, response](const auto&, const auto&) { fixture.wire.put(*response); });
  fixture.sdkFace.expressInterest(ndn::Interest(response->getName()), receive(response, 11), fail, fail);
  fixture.pump([&] { return observed->success + observed->failure == 2; });
  BOOST_CHECK_EQUAL(observed->success, 2);
  BOOST_CHECK_EQUAL(observed->failure, 0);
  BOOST_CHECK(observed->wireMatches);
  BOOST_CHECK_EQUAL(fixture.networkCkInterests, 0);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
