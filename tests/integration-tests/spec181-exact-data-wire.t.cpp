#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>

#include <chrono>
#include <future>

namespace ndn_service_framework::test {
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(Spec181ExactDataWire)

BOOST_AUTO_TEST_CASE(SignedPacketLimitAndRejectedBatchVisibility)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();

  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  auto data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });

  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "producer";
  assignment.service = profile.serviceName;
  const ndn::Name requestId("/spec181/exact-wire");
  ServiceProvider::CollaborationContext publisher(
    producer, profile.userIdentity, requestId, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), assignment);

  auto makeName = [&] (const std::string& suffix) {
    ndn::Name name(producer.getName());
    return name.append("NDNSF-DI").append("TENSOR").append(std::string(500, 'x'))
      .append(suffix);
  };
  auto signedSize = [&] (const ndn::Name& name, const ndn::Buffer& content) {
    ndn::Data packet(name);
    packet.setFreshnessPeriod(ndn::time::milliseconds(60000));
    packet.setContent(content);
    environment.keyChain().sign(packet, ndn::security::signingByCertificate(
      producer.getSigningCertificateName()));
    return packet.wireEncode().size();
  };
  auto payloadForSize = [&] (const ndn::Name& name, std::size_t wanted) {
    // RSA signatures have fixed size. At these lengths the TLV header width
    // remains fixed, so adjust only content and verify the actual signed wire.
    ndn::Buffer content(7000, 0x5a);
    const auto measured = signedSize(name, content);
    BOOST_REQUIRE_LT(measured, wanted);
    content.resize(content.size() + wanted - measured, 0x5a);
    BOOST_REQUIRE_EQUAL(signedSize(name, content), wanted);
    return content;
  };
  auto fetch = [&] (const ndn::Name& name) {
    auto pending = std::async(std::launch::async, [&] {
      return receiver.fetchSignedExactData(
        "wire-limit", name, producer.getName(), 250);
    });
    environment.pumpUntil([&] {
      return pending.wait_for(0ms) == std::future_status::ready;
    });
    return pending.get();
  };

  for (const auto wanted : {ndn::MAX_NDN_PACKET_SIZE - 1,
                            ndn::MAX_NDN_PACKET_SIZE}) {
    const auto name = makeName(std::to_string(wanted));
    const auto content = payloadForSize(name, wanted);
    std::size_t observedSize = 0;
    auto observe = producerFace.onSendData.connect([&] (const ndn::Data& packet) {
      if (packet.getName() == name) observedSize = packet.wireEncode().size();
    });
    BOOST_REQUIRE(publisher.publishSignedExactData(
      "wire-limit", {{name, content}}, 60000));
    const auto result = fetch(name);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL_COLLECTIONS(result->begin(), result->end(),
                                  content.begin(), content.end());
    BOOST_CHECK_EQUAL(observedSize, wanted);
  }

  const auto tooLargeName = makeName("too-large");
  const auto tooLarge = payloadForSize(tooLargeName, ndn::MAX_NDN_PACKET_SIZE + 1);
  BOOST_CHECK(!publisher.publishSignedExactData(
    "wire-limit", {{tooLargeName, tooLarge}}, 60000));

  const auto firstName = makeName("batch-first");
  const ndn::Buffer first{'o', 'k'};
  BOOST_CHECK(!publisher.publishSignedExactData(
    "wire-limit", {{firstName, first}, {tooLargeName, tooLarge}}, 60000));
  // A late size failure must not leave an apparently ready manifest in IMS.
  BOOST_CHECK(!fetch(firstName));
  BOOST_REQUIRE(publisher.publishSignedExactData(
    "wire-limit", {{firstName, first}}, 60000));
  const auto recovered = fetch(firstName);
  BOOST_REQUIRE(recovered);
  BOOST_CHECK_EQUAL_COLLECTIONS(recovered->begin(), recovered->end(),
                                first.begin(), first.end());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
