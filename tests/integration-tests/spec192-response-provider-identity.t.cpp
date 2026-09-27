#include "tests/boost-test.hpp"

#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/utils.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework::integration_test {
namespace {

class ScopedEnvironmentValue
{
public:
  ScopedEnvironmentValue(const char* name, const char* value)
    : m_name(name)
  {
    if (const char* previous = std::getenv(name)) {
      m_previous = previous;
    }
    ::setenv(name, value, 1);
  }

  ~ScopedEnvironmentValue()
  {
    if (m_previous) {
      ::setenv(m_name.c_str(), m_previous->c_str(), 1);
    }
    else {
      ::unsetenv(m_name.c_str());
    }
  }

private:
  std::string m_name;
  std::optional<std::string> m_previous;
};

PolicyStatusData
currentStatusFor(const ndn::Name& serviceName)
{
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(ControllerVersion{now, 1});
  status.setValidity(now - 1000, now + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ndn::Name("/controller/spec192/response"));
  return status;
}

void
publish(ndn::svs::SVSPubSub& pubSub, const ndn::Name& name,
        const ndn::Buffer& payload)
{
  pubSub.publish(name, ndn::span<const uint8_t>(payload.data(), payload.size()));
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec192ResponseProviderIdentity)

BOOST_AUTO_TEST_CASE(ProductionOnResponseRejectsValidDataSignedByWrongProvider)
{
  // Exercise the production ResponseMessage decode/delivery path with a
  // plaintext diagnostic payload; the packet identity gate remains the
  // production check under test.
  ScopedEnvironmentValue cryptoDiagnostics("NDNSF_CRYPTO_DIAG", "1");
  ScopedEnvironmentValue plaintextResponses(
      "NDNSF_DIAG_PLAINTEXT_RESPONSE", "1");

  test::BootstrapProfile profile;
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("spec192-response-provider-identity");

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  std::vector<ndn::Name> providers;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providers.push_back(environment.provider(index).getName());
  }

  auto status = currentStatusFor(serviceName);
  status.setAbePublicParametersName(
      environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(
      environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    BOOST_REQUIRE(environment.provider(index).installControllerStatus(status));
    environment.provider(index).setUseTokens(false);
  }
  environment.user().setUseTokens(false);
  environment.pumpUntilWithAttributeAuthority([] { return false; });
  environment.enableProductionIngressForTest();

  environment.user().setRequestPublisher(
      [] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
          const ndn::Name&, const RequestMessage&, size_t) {});

  bool responseCallbackInvoked = false;
  bool timeoutCallbackInvoked = false;
  std::string receivedPayload;
  std::string receivedProviderIdentity;
  RequestMessage request;
  const auto requestId = environment.user().RequestService(
      providers, serviceName, request, 5000,
      [&] (const ndn::Name&) { timeoutCallbackInvoked = true; },
      [&] (const ResponseMessage& response) {
        responseCallbackInvoked = true;
        const auto payload = response.getPayload();
        receivedPayload.assign(payload.begin(), payload.end());
        receivedProviderIdentity = response.getProviderIdentity();
      },
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE_EQUAL(environment.user().getPendingCallCount(), 1U);

  // The fixture uses an accept-all SVS validator. Publish a syntactically
  // valid Response name for Provider 0 using Provider 1's real KeyChainSigner;
  // this reaches the production OnResponse identity check after validation.
  const auto mismatchedResponseName = makeResponseNameV2(
      providers[0], requesterName, serviceName, requestId);
  ResponseMessage response;
  response.setStatus(true);
  ndn::Buffer responsePayload{'o', 'k'};
  response.setPayload(responsePayload, responsePayload.size());
  const auto responseWire = response.WireEncode();
  const ndn::Buffer responseContent(
      responseWire.data(), responseWire.data() + responseWire.size());
  publish(environment.providerPubSub(1), mismatchedResponseName, responseContent);

  uint64_t rejectedIdentityCount = 0;
  environment.pumpUntil([&] {
    rejectedIdentityCount += environment.user().consumeRuntimeDiagnostics()
                                 .responseRejectedProviderIdentity;
    return rejectedIdentityCount > 0;
  });

  BOOST_CHECK_EQUAL(rejectedIdentityCount, 1U);
  BOOST_CHECK(!responseCallbackInvoked);
  BOOST_CHECK(!timeoutCallbackInvoked);
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 1U);

  // A rejected mismatched signer must not consume the pending request. A
  // correctly signed Response from the named Provider must still complete it.
  const auto validResponseName = makeResponseNameV2(
      providers[0], requesterName, serviceName, requestId);
  publish(environment.providerPubSub(0), validResponseName, responseContent);
  environment.pumpUntil([&] { return responseCallbackInvoked; });

  BOOST_CHECK(responseCallbackInvoked);
  BOOST_CHECK(!timeoutCallbackInvoked);
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0U);
  BOOST_CHECK_EQUAL(receivedPayload, "ok");
  BOOST_CHECK_EQUAL(receivedProviderIdentity, providers[0].toUri());

  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::integration_test
