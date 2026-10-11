#include "tests/boost-test.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"

#include <string>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(ServiceUserResponseSignerIdentity)

BOOST_AUTO_TEST_CASE(key_locator_identity_must_match_response_provider)
{
  const ndn::Name provider("/repo/provider-a");
  const ndn::Name certificate = ndn::Name(provider)
                                  .append("KEY")
                                  .append("key-1")
                                  .append("ID-CERT")
                                  .appendVersion(1);

  const auto match = detail::matchResponseProviderIdentity(provider, certificate);
  BOOST_REQUIRE(match);
  BOOST_CHECK_EQUAL(match->toUri(), provider.toUri());

  BOOST_CHECK(!detail::matchResponseProviderIdentity(
      ndn::Name("/repo/provider-b"), certificate));
  BOOST_CHECK(!detail::matchResponseProviderIdentity(
      provider, ndn::Name("/repo/provider-a/KEY/key-1")));
  BOOST_CHECK(!detail::matchResponseProviderIdentity(ndn::Name(), certificate));
  BOOST_CHECK(!detail::matchResponseProviderIdentity(provider, ndn::Name()));
}

BOOST_AUTO_TEST_CASE(provider_identity_is_local_transport_metadata)
{
  ResponseMessage response;
  response.setAuthenticatedTransportEvidence(
      "/repo/provider-a/NDNSF/RESPONSE/user/STORE/request",
      "/repo/provider-a/KEY/key-1/ID-CERT/v=1",
      "wire-digest",
      "/repo/provider-a");

  ResponseMessage copied(response);
  BOOST_CHECK_EQUAL(copied.getProviderIdentity(), "/repo/provider-a");

  ResponseMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(response.WireEncode()));
  BOOST_CHECK(decoded.getProviderIdentity().empty());
  BOOST_CHECK(decoded.getSignerCertificate().empty());
  BOOST_CHECK(decoded.getDataName().empty());

  ResponseMessage assigned;
  assigned = copied;
  BOOST_CHECK_EQUAL(assigned.getProviderIdentity(), "/repo/provider-a");

  assigned.Clear();
  BOOST_CHECK(assigned.getProviderIdentity().empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
