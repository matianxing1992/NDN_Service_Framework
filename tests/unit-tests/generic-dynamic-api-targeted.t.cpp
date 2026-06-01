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
  request.setProviderToken("provider-token");

  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decoded.getRequestMode(), tlv::TargetedRequest);
  BOOST_CHECK_EQUAL(decoded.getTargetProvider(), ndn::Name("/test/provider/drone-a"));
  BOOST_CHECK_EQUAL(decoded.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decoded.getProviderToken(), "provider-token");
}

BOOST_AUTO_TEST_CASE(RequestServiceTargetedBootstrapsBeforeFastPath)
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
  user.applyPermissionResponse(
    makePermissionResponse(ndn::Name("/test/user/gs"),
                           tlv::UserPermission,
                           providerName,
                           serviceName));
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
      BOOST_CHECK_EQUAL(requestMessage.getRequestMode(), tlv::TargetedBootstrapRequest);
      BOOST_CHECK_EQUAL(requestMessage.getTargetProvider(), providerName);
      BOOST_CHECK(!requestMessage.getUserToken().empty());
      BOOST_CHECK(requestMessage.getProviderToken().empty());
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

  ResponseMessage bootstrapResponse;
  bootstrapResponse.setStatus(true);
  bootstrapResponse.setUserToken("bootstrap-user-token");
  bootstrapResponse.setTokens({
    {"targeted.0.provider", "provider-token-0"},
    {"targeted.0.user", "user-token-0"},
    {"targeted.count", "1"},
  });
  user.addTargetedPendingCallForTokenTest(ndn::Name("/bootstrap-response"),
                                          serviceName,
                                          providerName,
                                          "bootstrap-user-token");
  BOOST_CHECK(user.handleDecryptedResponse(ndn::Name("/bootstrap-response"),
                                           providerName,
                                           bootstrapResponse));
  BOOST_CHECK_EQUAL(user.getTargetedTokenPoolSizeForTest(providerName, serviceName), 1);

  bool fastPathPublished = false;
  user.setRequestPublisher(
    [&](const ndn::Name&,
        const ndn::Name&,
        const std::vector<ndn::Name>&,
        const ndn::Name&,
        const RequestMessage& requestMessage,
        size_t) {
      fastPathPublished = true;
      BOOST_CHECK_EQUAL(requestMessage.getRequestMode(), tlv::TargetedRequest);
      BOOST_CHECK_EQUAL(requestMessage.getTargetProvider(), providerName);
      BOOST_CHECK_EQUAL(requestMessage.getProviderToken(), "provider-token-0");
      BOOST_CHECK_EQUAL(requestMessage.getUserToken(), "user-token-0");
    });
  RequestMessage secondRequest;
  const auto fastRequestId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    std::move(secondRequest),
    1000,
    [] (const ndn::Name&) {},
    [] (const ResponseMessage&) {});
  BOOST_CHECK(!fastRequestId.empty());
  BOOST_CHECK(fastPathPublished);
  BOOST_CHECK_EQUAL(user.getTargetedTokenPoolSizeForTest(providerName, serviceName), 0);
}

BOOST_AUTO_TEST_CASE(RequestServiceTargetedRequiresUserPermission)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-user-permission",
                                   "tpm-memory:targeted-user-permission");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name serviceName("/UAV/MAVLink/Execute");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");

  bool published = false;
  user.setRequestPublisher(
    [&] (const ndn::Name&,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage&,
         size_t) {
      published = true;
    });

  RequestMessage request;
  auto requestId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    request,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));
  BOOST_CHECK(requestId.empty());
  BOOST_CHECK(!published);

  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  requestId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    std::move(request),
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));
  BOOST_CHECK(!requestId.empty());
  BOOST_CHECK(published);
}

BOOST_AUTO_TEST_CASE(TargetedBootstrapExecutesAndReturnsTokenBatch)
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
  request.setRequestMode(tlv::TargetedBootstrapRequest);
  request.setTargetProvider(providerName);

  const ndn::Name requestId("/targeted-bootstrap-1");
  const auto requestName = makeRequestNameV2(requesterName, serviceName, requestId);
  const auto response = provider.handleDecryptedRequestByName(requestName, request);
  BOOST_CHECK(response.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
  BOOST_CHECK_EQUAL(response.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(response.getTokens().at("targeted.count"), "8");
  BOOST_CHECK(response.getTokens().find("targeted.0.provider") != response.getTokens().end());
  BOOST_CHECK(response.getTokens().find("targeted.0.user") != response.getTokens().end());
}

BOOST_AUTO_TEST_CASE(TargetedServiceConsumesCachedTokenForFastPath)
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
  provider.addTargetedProviderTokenForTest(requesterName,
                                           serviceName,
                                           "provider-token",
                                           "user-token");

  auto request = makeRequestMessageWithUserToken("arm", "user-token");
  request.setRequestMode(tlv::TargetedRequest);
  request.setTargetProvider(providerName);
  request.setProviderToken("provider-token");

  const ndn::Name requestId("/targeted-1");
  const auto requestName = makeRequestNameV2(requesterName, serviceName, requestId);
  const auto response = provider.handleDecryptedRequestByName(requestName, request);
  BOOST_CHECK(response.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
  BOOST_CHECK_EQUAL(response.getUserToken(), "user-token");

  const auto replayResponse =
    provider.handleDecryptedRequestByName(
      makeRequestNameV2(requesterName, serviceName, ndn::Name("/targeted-replay-token")),
      request);
  BOOST_CHECK(!replayResponse.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
}

BOOST_AUTO_TEST_CASE(NormalAndTargetedRegistrationsCoexistForSameService)
{
  auto setResponsePayload = [] (ResponseMessage& response, const std::string& value) {
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    response.setPayload(payload, payload.size());
  };
  auto responsePayloadToString = [] (const ResponseMessage& response) {
    const auto payload = response.getPayload();
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
  };

  ndn::security::KeyChain keyChain("pib-memory:targeted-provider-coexist",
                                   "tpm-memory:targeted-provider-coexist");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/Targeted/Telemetry/GetStatus");
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

  size_t normalCalls = 0;
  size_t targetedCalls = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler{},
    [&](const RequestMessage&) {
      ++normalCalls;
      ResponseMessage response;
      response.setStatus(true);
      setResponsePayload(response, "normal");
      return response;
    });
  provider.addTargetedService(
    serviceName,
    [&](const RequestMessage&) {
      ++targetedCalls;
      ResponseMessage response;
      response.setStatus(true);
      setResponsePayload(response, "targeted");
      return response;
    });

  auto normalRequest = makeRequestMessageWithUserToken("get", "normal-user-token");
  const auto normalResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/normal-coexist")),
    normalRequest);
  BOOST_REQUIRE(normalResponse.getStatus());
  BOOST_CHECK_EQUAL(normalCalls, 1);
  BOOST_CHECK_EQUAL(targetedCalls, 0);
  BOOST_CHECK_EQUAL(responsePayloadToString(normalResponse), "normal");

  auto bootstrapRequest = makeRequestMessageWithUserToken("get", "bootstrap-user-token");
  bootstrapRequest.setRequestMode(tlv::TargetedBootstrapRequest);
  bootstrapRequest.setTargetProvider(providerName);
  const auto bootstrapResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/bootstrap-coexist")),
    bootstrapRequest);
  BOOST_REQUIRE(bootstrapResponse.getStatus());
  BOOST_CHECK_EQUAL(normalCalls, 1);
  BOOST_CHECK_EQUAL(targetedCalls, 1);
  BOOST_CHECK_EQUAL(responsePayloadToString(bootstrapResponse), "targeted");
  BOOST_CHECK_EQUAL(bootstrapResponse.getTokens().at("targeted.count"), "8");

  auto targetedRequest = makeRequestMessageWithUserToken("get", "fast-user-token");
  targetedRequest.setRequestMode(tlv::TargetedRequest);
  targetedRequest.setTargetProvider(providerName);
  targetedRequest.setProviderToken("provider-token");
  provider.addTargetedProviderTokenForTest(requesterName,
                                           serviceName,
                                           "provider-token",
                                           "fast-user-token");
  const auto targetedResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/targeted-coexist")),
    targetedRequest);
  BOOST_REQUIRE(targetedResponse.getStatus());
  BOOST_CHECK_EQUAL(normalCalls, 1);
  BOOST_CHECK_EQUAL(targetedCalls, 2);
  BOOST_CHECK_EQUAL(responsePayloadToString(targetedResponse), "targeted");
}

BOOST_AUTO_TEST_CASE(ExplicitNormalAndTargetedInvocationModeRegistersBothPaths)
{
  auto setResponsePayload = [] (ResponseMessage& response, const std::string& value) {
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    response.setPayload(payload, payload.size());
  };
  auto responsePayloadToString = [] (const ResponseMessage& response) {
    const auto payload = response.getPayload();
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
  };

  ndn::security::KeyChain keyChain("pib-memory:targeted-provider-explicit-mode",
                                   "tpm-memory:targeted-provider-explicit-mode");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/drone-a");
  const ndn::Name requesterName("/test/user/gs");
  const ndn::Name serviceName("/UAV/Telemetry/GetStatus");
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

  size_t calls = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler{},
    ServiceProvider::SimpleRequestHandler(
      [&](const RequestMessage&) {
        ++calls;
        ResponseMessage response;
        response.setStatus(true);
        setResponsePayload(response, "telemetry");
        return response;
      }),
    ServiceProvider::ServiceInvocationMode::NormalAndTargeted);

  auto normalRequest = makeRequestMessageWithUserToken("get", "normal-user-token");
  const auto normalResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/normal-explicit-mode")),
    normalRequest);
  BOOST_REQUIRE(normalResponse.getStatus());
  BOOST_CHECK_EQUAL(responsePayloadToString(normalResponse), "telemetry");
  BOOST_CHECK_EQUAL(calls, 1);

  auto bootstrapRequest = makeRequestMessageWithUserToken("get", "bootstrap-user-token");
  bootstrapRequest.setRequestMode(tlv::TargetedBootstrapRequest);
  bootstrapRequest.setTargetProvider(providerName);
  const auto bootstrapResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/bootstrap-explicit-mode")),
    bootstrapRequest);
  BOOST_REQUIRE(bootstrapResponse.getStatus());
  BOOST_CHECK_EQUAL(responsePayloadToString(bootstrapResponse), "telemetry");
  BOOST_CHECK_EQUAL(bootstrapResponse.getTokens().at("targeted.count"), "8");
  BOOST_CHECK_EQUAL(calls, 2);

  auto targetedRequest = makeRequestMessageWithUserToken("get", "fast-user-token");
  targetedRequest.setRequestMode(tlv::TargetedRequest);
  targetedRequest.setTargetProvider(providerName);
  targetedRequest.setProviderToken("provider-token");
  provider.addTargetedProviderTokenForTest(requesterName,
                                           serviceName,
                                           "provider-token",
                                           "fast-user-token");
  const auto targetedResponse = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/targeted-explicit-mode")),
    targetedRequest);
  BOOST_REQUIRE(targetedResponse.getStatus());
  BOOST_CHECK_EQUAL(responsePayloadToString(targetedResponse), "telemetry");
  BOOST_CHECK_EQUAL(calls, 3);
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
  request.setProviderToken("provider-token");
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

  auto missingProviderTokenRequest =
    makeRequestMessageWithUserToken("arm", "user-token");
  missingProviderTokenRequest.setRequestMode(tlv::TargetedRequest);
  missingProviderTokenRequest.setTargetProvider(providerName);
  response = provider.handleDecryptedRequestByName(requestName,
                                                   missingProviderTokenRequest);
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
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));
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
  request.setProviderToken("provider-token");

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
  request.setProviderToken("provider-token");

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
  request.setProviderToken("provider-token");
  provider.addTargetedProviderTokenForTest(requesterName,
                                           serviceName,
                                           "provider-token",
                                           "replay-user-token");
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
