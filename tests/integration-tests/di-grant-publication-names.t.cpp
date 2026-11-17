#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include "tests/fixtures/naming-wire-oracle.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <algorithm>

namespace ndn_service_framework::test {
using namespace naming_oracle;

BOOST_AUTO_TEST_SUITE(Spec201ControlAuthorization)

BOOST_AUTO_TEST_CASE(GrantNamesMatchIndependentPaperComponentWire)
{
  const ndn::Name producer("/org/NDNSF/GRANT/user"), recipient("/org/KEY/provider");
  const ndn::Name request = ndn::Name("/request/tail").appendVersion(257).appendSegment(0);
  const Bytes plan(32, 0xaa), model(32, 0xbb);
  auto expected = producer;
  expected.append("NDNSF").append("GRANT").append(ndn::name::Component("kind=KEY-GRANT"))
    .append("RECIPIENT").append(commitment("ndnsf-naming/opaque/recipient", {{1, 4, wire(recipient)}}))
    .append("SCOPE").append("MODEL")
    .append(commitment("ndnsf-naming/grant-scope", {
      {1, 1, wire(producer)}, {2, 1, wire(request)}, {3, 3, uint64(7)},
      {4, 4, plan}, {5, 4, model}}))
    .append("EPOCH").append(commitment("ndnsf-naming/opaque/protection-epoch", {{1, 4, utf8("epoch/typed=1")}}))
    .append("OBJECT").append(repeatedDigest(0xcc));
  const ndn::Name actual(ndnsf::di::canonicalNativeGrantName(
      producer.toUri(ndn::name::UriFormat::CANONICAL), recipient.toUri(), request.toUri(),
      7, digestText('a'), digestText('b'), "epoch/typed=1", digestText('c')));
  BOOST_CHECK(actual.wireEncode() == expected.wireEncode());
  BOOST_CHECK(ndn::Name(ndnsf::di::canonicalNativeGrantName(producer.toUri(ndn::name::UriFormat::ALTERNATE),
      recipient.toUri(), request.toUri(), 7, digestText('a'), digestText('b'),
      "epoch/typed=1", digestText('c'))) == actual);

}

BOOST_AUTO_TEST_CASE(ActualUserGrantPublisherKeepsOriginalWireAndRejectsConflict)
{
  IsolatedTestTrust trustOwner;
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keys);
  const auto owner = trustOwner.createTestIdentityCertificate(
      keys, ndn::Name("/spec201/owner/NDNSF/GRANT"));
  const auto authority = trustOwner.createTestIdentityCertificate(keys, ndn::Name("/spec201/authority"));
  LocalServiceUser user(face, ndn::Name("/spec201/group"), owner, authority,
                        trustOwner.schemaPath());
  user.useSigningKeyChainForTest(keys);
  const ndn::Name name(ndnsf::di::canonicalNativeGrantName(owner.getIdentity().toUri(), "/recipient", "/request", 1,
       digestText('a'), digestText('b'), "epoch", digestText('c')));
  ndn::Buffer payload(5); std::fill(payload.begin(), payload.end(), 0x71);
  user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(!face.sentData.empty());
  const auto original = face.sentData.back().wireEncode();
  user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
  payload[0] ^= 1;
  BOOST_CHECK_THROW(user.publishSignedAppData(name, payload, ndn::time::milliseconds(1000)), std::invalid_argument);
  BOOST_CHECK(face.sentData.back().wireEncode() == original);
  BOOST_CHECK_THROW(user.publishSignedAppData(ndn::Name("/foreign/NDNSF/GRANT/object"), payload,
                                            ndn::time::milliseconds(1000)), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
