#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(DiSelectionStatus)

BOOST_AUTO_TEST_CASE(UnknownSelectionStatusIsNotPublishedAsUnboundData)
{
  ndn::security::KeyChain keyChain("pib-memory:di-selection-status",
                                   "tpm-memory:di-selection-status");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/status-not-found");
  const ndn::Name serviceName("/AI/LLM/Pipeline/QwenNative");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/aa-di-selection-status"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), providerCert,
                                aaCert);
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
  provider.setSelectionStatusQueryable(serviceName, true);

  const auto query = ndn::Interest(makeSelectionStatusQueryName(
    providerName, serviceName, "missing-selection-digest"));
  BOOST_CHECK(!provider.replySelectionStatusForTest(query));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
