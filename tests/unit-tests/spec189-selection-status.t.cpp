#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(Spec189SelectionStatus)

BOOST_AUTO_TEST_CASE(UnknownSelectionStatusIsNotPublishedAsUnboundData)
{
  ndn::security::KeyChain keyChain("pib-memory:spec189-selection-status",
                                   "tpm-memory:spec189-selection-status");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/status-not-found");
  const ndn::Name serviceName("/AI/LLM/Pipeline/QwenNative");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto aaCert = makeRsaIdentity(
    keyChain, ndn::Name("/test/aa-spec189-selection-status"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), providerCert,
                                aaCert, "examples/trust-any.conf");
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
