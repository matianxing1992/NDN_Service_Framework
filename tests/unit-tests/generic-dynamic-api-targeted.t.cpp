#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(TargetedInvocation)

BOOST_AUTO_TEST_CASE(TargetedRequestFieldsRoundTrip)
{
  RequestMessage request;
  request.setUserToken("user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(ndn::Name("/test/provider/drone-a"));

  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decoded.getRequestMode(), tlv::TargetedRequest);
  BOOST_CHECK_EQUAL(decoded.getTargetProvider(), ndn::Name("/test/provider/drone-a"));
  BOOST_CHECK_EQUAL(decoded.getUserToken(), "user-token");
}

BOOST_AUTO_TEST_CASE(RequestServiceTargetedPublishesTargetedRequest)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-user",
                                   "tpm-memory:targeted-user");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/gs"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");

  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  bool published = false;
  ndn::Name publishedRequestId;

  user.setRequestPublisher(
    [&](const ndn::Name& requestId,
        const ndn::Name&,
        const std::vector<ndn::Name>& providers,
        const ndn::Name& publishedServiceName,
        const RequestMessage& requestMessage,
        size_t strategy) {
      published = true;
      publishedRequestId = requestId;
      BOOST_REQUIRE_EQUAL(providers.size(), 1);
      BOOST_CHECK_EQUAL(providers.front(), providerName);
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
      BOOST_CHECK_EQUAL(requestMessage.getRequestMode(), tlv::TargetedRequest);
      BOOST_CHECK_EQUAL(requestMessage.getTargetProvider(), providerName);
      BOOST_CHECK(!requestMessage.getUserToken().empty());
    });

  RequestMessage request;
  const auto requestId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    std::move(request),
    1000,
    [] (const ndn::Name&) {
      BOOST_FAIL("targeted request should not time out during publish test");
    },
    [] (const ResponseMessage&) {
      BOOST_FAIL("targeted request publish test should not receive a response");
    });

  BOOST_CHECK(!requestId.empty());
  BOOST_CHECK(published);
  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(namesContain(user.getExpectedResponseProviders(requestId), providerName));
}

BOOST_AUTO_TEST_CASE(TargetedServiceAcceptsOnlyTargetedRequestsForThisProvider)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-provider",
                                   "tpm-memory:targeted-provider");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  size_t handlerCalls = 0;
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      ++handlerCalls;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("arm", "user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(providerName);

  const ndn::Name requestId("/targeted-1");
  const auto requestName = makeRequestNameV2(requesterName, serviceName, requestId);
  const auto response = provider.handleDecryptedRequestByName(requestName, request);
  BOOST_CHECK(response.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
  BOOST_CHECK_EQUAL(response.getUserToken(), "user-token");
}

BOOST_AUTO_TEST_CASE(TargetedProviderRequiresPermissionAndUserToken)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-provider-permission",
                                   "tpm-memory:targeted-provider-permission");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  size_t handlerCalls = 0;
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      ++handlerCalls;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("arm", "user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(providerName);
  const auto requestName =
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/targeted-no-provider-permission"));

  auto response = provider.handleDecryptedRequestByName(requestName, request);
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 0);

  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  auto tokenlessRequest = makeRequestMessageWithUserToken("arm", "");
  tokenlessRequest.setRequestMode(tlv::TargetedRequest);
  tokenlessRequest.setTargetProvider(providerName);
  response = provider.handleDecryptedRequestByName(requestName, tokenlessRequest);
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 0);
}

BOOST_AUTO_TEST_CASE(TargetedUserAcceptsOnlyExpectedProviderAndUserToken)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-user-response",
                                   "tpm-memory:targeted-user-response");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name otherProviderName("/test/provider/drone-b");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  const ndn::Name requestId("/targeted-user-response");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.addTargetedPendingCallForTokenTest(requestId,
                                          serviceName,
                                          providerName,
                                          "user-token");

  ResponseMessage wrongTokenResponse;
  wrongTokenResponse.setStatus(true);
  wrongTokenResponse.setUserToken("wrong-user-token");
  BOOST_CHECK(!user.handleDecryptedResponse(requestId, providerName, wrongTokenResponse));

  ResponseMessage wrongProviderResponse;
  wrongProviderResponse.setStatus(true);
  wrongProviderResponse.setUserToken("user-token");
  BOOST_CHECK(!user.handleDecryptedResponse(requestId, otherProviderName, wrongProviderResponse));

  bool callbackCalled = false;
  user.setPendingResponseHandlerForTest(
    requestId,
    [&](const ResponseMessage& response) {
      callbackCalled = true;
      BOOST_CHECK(response.getStatus());
      BOOST_CHECK_EQUAL(response.getUserToken(), "user-token");
    });

  ResponseMessage goodResponse;
  goodResponse.setStatus(true);
  goodResponse.setUserToken("user-token");
  BOOST_CHECK(user.handleDecryptedResponse(requestId, providerName, goodResponse));
  BOOST_CHECK(callbackCalled);
}

BOOST_AUTO_TEST_CASE(TargetedRequestRequiresTargetedProviderService)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-reject-normal-service",
                                   "tpm-memory:targeted-reject-normal-service");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  bool handlerCalled = false;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler{},
    [&](const RequestMessage&) {
      handlerCalled = true;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("arm", "user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(providerName);

  const auto response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/targeted-normal-service")),
    request);
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!handlerCalled);
}

BOOST_AUTO_TEST_CASE(NormalRequestDoesNotExecuteTargetedOnlyService)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-reject-normal-request",
                                   "tpm-memory:targeted-reject-normal-request");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  bool handlerCalled = false;
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      handlerCalled = true;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  const auto request = makeRequestMessageWithUserToken("arm", "user-token");
  const auto response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/normal-targeted-service")),
    request);
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!handlerCalled);
}

BOOST_AUTO_TEST_CASE(TargetedRequestForOtherProviderDoesNotExecute)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-wrong-provider",
                                   "tpm-memory:targeted-wrong-provider");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  bool handlerCalled = false;
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      handlerCalled = true;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("arm", "user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(ndn::Name("/test/provider/drone-b"));

  const auto response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/wrong-provider")),
    request);
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!handlerCalled);
}

BOOST_AUTO_TEST_CASE(ReplayedTargetedRuntimeRequestExecutesOnce)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-replay",
                                   "tpm-memory:targeted-replay");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  size_t handlerCalls = 0;
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      ++handlerCalls;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("arm", "replay-user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(providerName);
  const auto block = request.WireEncode();
  const ndn::Buffer encoded(block.data(), block.size());
  const ndn::Name requestId("/targeted-replay");

  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                ndn::Name(),
                                                requestId,
                                                encoded);
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                ndn::Name(),
                                                requestId,
                                                encoded);

  BOOST_CHECK_EQUAL(handlerCalls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
