#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(TargetedInvocation)

BOOST_AUTO_TEST_CASE(SelectionGatedInputFailsBeforeTargetedPublication)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-gated",
                                   "tpm-memory:targeted-gated");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/aa"));
  LocalServiceUser user(face, ndn::Name("/group"), userCert, aaCert,
                        "examples/trust-any.conf");
  RequestCapabilities capabilities;
  capabilities.setField("SelectionGatedInputV1", "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  const auto requestId = user.RequestServiceTargeted(
    ndn::Name("/provider/a"), ndn::Name("/service/a"), request, 100,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});
  BOOST_CHECK(requestId.empty());
  BOOST_CHECK(face.sentData.empty());
}

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

BOOST_AUTO_TEST_CASE(TargetedPoolProactivelyRefillsOnceAtLowWatermark)
{
  // This case explicitly exercises the opt-in adaptive controller.  The
  // production default is fixed-size refill (256); keep the adaptive
  // contract isolated from the default-path tests below.
  ::setenv("NDNSF_TARGETED_TOKEN_ADAPTIVE", "1", 1);
  ::setenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE", "16", 1);
  struct RestoreEnv {
    ~RestoreEnv()
    {
      ::unsetenv("NDNSF_TARGETED_TOKEN_ADAPTIVE");
      ::unsetenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE");
    }
  } restoreEnv;

  ndn::security::KeyChain keyChain("pib-memory:targeted-user-refill",
                                   "tpm-memory:targeted-user-refill");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/refill"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa/refill"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group/refill"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  const ndn::Name providerName("/test/provider/refill");
  const ndn::Name serviceName("/Repo/Adaptive");
  user.applyPermissionResponse(
    makePermissionResponse(ndn::Name("/test/user/refill"),
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  ResponseMessage bootstrapResponse;
  bootstrapResponse.setStatus(true);
  bootstrapResponse.setUserToken("bootstrap-user-token");
  std::map<std::string, std::string> tokens;
  for (size_t i = 0; i < 16; ++i) {
    tokens["targeted." + std::to_string(i) + ".provider"] =
      "provider-token-" + std::to_string(i);
    tokens["targeted." + std::to_string(i) + ".user"] =
      "user-token-" + std::to_string(i);
  }
  tokens["targeted.count"] = "16";
  bootstrapResponse.setTokens(tokens);
  user.addTargetedPendingCallForTokenTest(ndn::Name("/bootstrap-refill"),
                                          serviceName,
                                          providerName,
                                          "bootstrap-user-token");
  BOOST_REQUIRE(user.handleDecryptedResponse(ndn::Name("/bootstrap-refill"),
                                             providerName,
                                             bootstrapResponse));
  BOOST_CHECK_EQUAL(user.getTargetedTokenPoolSizeForTest(providerName, serviceName), 16);

  struct PublishedCall {
    ndn::Name requestId;
    RequestMessage request;
  };
  std::vector<PublishedCall> published;
  user.setRequestPublisher(
    [&published] (const ndn::Name& requestId,
                  const ndn::Name&,
                  const std::vector<ndn::Name>&,
                  const ndn::Name&,
                  const RequestMessage& requestMessage,
                  size_t) {
      published.push_back(PublishedCall{requestId, requestMessage});
    });

  // Consuming down to the 25% low-watermark must trigger exactly one
  // bootstrap refill, while all application calls continue using Targeted.
  for (size_t i = 0; i < 12; ++i) {
    BOOST_REQUIRE(!user.RequestServiceTargeted(
      providerName,
      serviceName,
      RequestMessage(),
      5000,
      [] (const ndn::Name&) {},
      [] (const ResponseMessage&) {}).empty());
  }

  size_t bootstrapCount = 0;
  ndn::Name refillRequestId;
  std::string refillUserToken;
  for (const auto& call : published) {
    if (call.request.getRequestMode() == tlv::TargetedBootstrapRequest) {
      ++bootstrapCount;
      refillRequestId = call.requestId;
      refillUserToken = call.request.getUserToken();
      BOOST_CHECK_EQUAL(call.request.getTokens().at("targeted.refill"), "1");
      BOOST_CHECK_EQUAL(call.request.getTokens().at("targeted.batch_hint"), "16");
    }
    else {
      BOOST_CHECK_EQUAL(call.request.getRequestMode(), tlv::TargetedRequest);
    }
  }
  BOOST_CHECK_EQUAL(bootstrapCount, 1);
  BOOST_REQUIRE(!refillRequestId.empty());

  // Give the adaptive controller a measurable demand interval.  A fast
  // refill must use the observed consumption rate rather than staying fixed
  // at the initial batch size.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  ResponseMessage refillResponse;
  refillResponse.setStatus(true);
  refillResponse.setUserToken(refillUserToken);
  refillResponse.setTokens(tokens);
  BOOST_REQUIRE(user.handleDecryptedResponse(refillRequestId,
                                             providerName,
                                             refillResponse));
  BOOST_CHECK_EQUAL(user.getTargetedTokenPoolSizeForTest(providerName, serviceName), 20);

  const auto publishedBeforeSecondRefill = published.size();
  for (size_t i = 0; i < 16; ++i) {
    BOOST_REQUIRE(!user.RequestServiceTargeted(
      providerName,
      serviceName,
      RequestMessage(),
      5000,
      [] (const ndn::Name&) {},
      [] (const ResponseMessage&) {}).empty());
  }
  size_t secondBootstrapCount = 0;
  size_t secondBatchHint = 0;
  for (size_t i = publishedBeforeSecondRefill; i < published.size(); ++i) {
    const auto& call = published[i];
    if (call.request.getRequestMode() == tlv::TargetedBootstrapRequest) {
      ++secondBootstrapCount;
      secondBatchHint = static_cast<size_t>(std::stoul(
        call.request.getTokens().at("targeted.batch_hint")));
    }
  }
  BOOST_CHECK_EQUAL(secondBootstrapCount, 1);
  BOOST_CHECK_GT(secondBatchHint, 16);
  BOOST_CHECK_LE(secondBatchHint, 256);
}

BOOST_AUTO_TEST_CASE(TargetedEmptyPoolCoalescesBootstrapAndUsesNormalFallback)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-user-coalesce",
                                   "tpm-memory:targeted-user-coalesce");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name userName("/test/user/coalesce");
  const ndn::Name providerName("/test/provider/coalesce");
  const ndn::Name serviceName("/Repo/Coalesce");
  auto userCert = makeRsaIdentity(keyChain, userName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa/coalesce"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group/coalesce"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(userName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  std::vector<size_t> publishedModes;
  user.setRequestPublisher(
    [&publishedModes] (const ndn::Name&,
                       const ndn::Name&,
                       const std::vector<ndn::Name>&,
                       const ndn::Name&,
                       const RequestMessage& requestMessage,
                       size_t) {
      publishedModes.push_back(requestMessage.getRequestMode());
    });

  const auto first = user.RequestServiceTargeted(
    providerName, serviceName, RequestMessage(), 5000,
    [] (const ndn::Name&) {}, [] (const ResponseMessage&) {});
  const auto second = user.RequestServiceTargeted(
    providerName, serviceName, RequestMessage(), 5000,
    [] (const ndn::Name&) {}, [] (const ResponseMessage&) {});
  BOOST_REQUIRE(!first.empty());
  BOOST_REQUIRE(!second.empty());
  BOOST_REQUIRE_EQUAL(publishedModes.size(), 2);
  BOOST_CHECK_EQUAL(publishedModes[0], tlv::TargetedBootstrapRequest);
  BOOST_CHECK_EQUAL(publishedModes[1], tlv::NormalRequest);
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
  BOOST_CHECK_EQUAL(response.getTokens().at("targeted.count"), "256");
  BOOST_CHECK(response.getTokens().find("targeted.0.provider") != response.getTokens().end());
  BOOST_CHECK(response.getTokens().find("targeted.0.user") != response.getTokens().end());
}

BOOST_AUTO_TEST_CASE(TargetedBootstrapUsesConfiguredBoundedTokenBatch)
{
  ::setenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE", "3", 1);
  struct RestoreEnv {
    ~RestoreEnv()
    {
      ::unsetenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE");
    }
  } restoreEnv;

  ndn::security::KeyChain keyChain("pib-memory:targeted-provider-config",
                                   "tpm-memory:targeted-provider-config");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/configured");
  const ndn::Name requesterName("/test/user/configured");
  const ndn::Name serviceName("/Repo/ObjectStore");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-configured"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group-configured"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
  provider.addTargetedService(
    serviceName,
    [](const RequestMessage&) {
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("store", "user-token");
  request.setRequestMode(tlv::TargetedBootstrapRequest);
  request.setTargetProvider(providerName);
  const auto response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/configured-batch")),
    request);

  BOOST_REQUIRE(response.getStatus());
  BOOST_CHECK_EQUAL(response.getTokens().at("targeted.count"), "3");
  BOOST_CHECK(response.getTokens().find("targeted.2.provider") != response.getTokens().end());
  BOOST_CHECK(response.getTokens().find("targeted.3.provider") == response.getTokens().end());
}

BOOST_AUTO_TEST_CASE(TargetedBootstrapHonorsAdaptiveBatchHintWithinProviderBound)
{
  ::setenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE", "32", 1);
  struct RestoreEnv {
    ~RestoreEnv()
    {
      ::unsetenv("NDNSF_TARGETED_TOKEN_BATCH_SIZE");
    }
  } restoreEnv;

  ndn::security::KeyChain keyChain("pib-memory:targeted-provider-hint",
                                   "tpm-memory:targeted-provider-hint");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/hint");
  const ndn::Name requesterName("/test/user/hint");
  const ndn::Name serviceName("/Repo/Adaptive");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-hint"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group-hint"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
  provider.addTargetedService(
    serviceName,
    [](const RequestMessage&) {
      ResponseMessage response;
      response.setStatus(true);
      return response;
    });

  auto request = makeRequestMessageWithUserToken("store", "user-token");
  request.setRequestMode(tlv::TargetedBootstrapRequest);
  request.setTargetProvider(providerName);
  request.setTokens({{"targeted.batch_hint", "12"}});
  auto response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/hint-12")),
    request);
  BOOST_REQUIRE(response.getStatus());
  BOOST_CHECK_EQUAL(response.getTokens().at("targeted.count"), "12");
  BOOST_CHECK(response.getTokens().find("targeted.11.provider") != response.getTokens().end());
  BOOST_CHECK(response.getTokens().find("targeted.12.provider") == response.getTokens().end());

  request.setTokens({{"targeted.batch_hint", "9999"}});
  response = provider.handleDecryptedRequestByName(
    makeRequestNameV2(requesterName, serviceName, ndn::Name("/hint-cap")),
    request);
  BOOST_REQUIRE(response.getStatus());
  BOOST_CHECK_EQUAL(response.getTokens().at("targeted.count"), "256");
  BOOST_CHECK(response.getTokens().find("targeted.255.provider") != response.getTokens().end());
  BOOST_CHECK(response.getTokens().find("targeted.256.provider") == response.getTokens().end());
}

BOOST_AUTO_TEST_CASE(TargetedUserStoresFullAdvertised256PairBatch)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-user-256",
                                   "tpm-memory:targeted-user-256");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name userName("/test/user/256");
  const ndn::Name providerName("/test/provider/256");
  const ndn::Name serviceName("/Repo/Batch256");
  auto userCert = makeRsaIdentity(keyChain, userName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa/256"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group/256"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(userName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token-256");
  std::map<std::string, std::string> tokens;
  for (size_t i = 0; i < 256; ++i) {
    tokens["targeted." + std::to_string(i) + ".provider"] =
      "provider-token-" + std::to_string(i);
    tokens["targeted." + std::to_string(i) + ".user"] =
      "user-token-" + std::to_string(i);
  }
  tokens["targeted.count"] = "256";
  response.setTokens(tokens);
  user.addTargetedPendingCallForTokenTest(ndn::Name("/bootstrap-256"),
                                          serviceName,
                                          providerName,
                                          "user-token-256");
  BOOST_REQUIRE(user.handleDecryptedResponse(ndn::Name("/bootstrap-256"),
                                             providerName,
                                             response));
  BOOST_CHECK_EQUAL(user.getTargetedTokenPoolSizeForTest(providerName, serviceName), 256);
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
  BOOST_CHECK_EQUAL(bootstrapResponse.getTokens().at("targeted.count"), "256");

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
  BOOST_CHECK_EQUAL(bootstrapResponse.getTokens().at("targeted.count"), "256");
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
                                                requestId,
                                                encoded);
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                requestId,
                                                encoded);

  BOOST_CHECK_EQUAL(handlerCalls, 1);
}

BOOST_AUTO_TEST_CASE(TargetedTokenFailuresNeverInvokeRegisteredHandler)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-token-negative",
                                   "tpm-memory:targeted-token-negative");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/token-negative");
  const ndn::Name requesterName("/test/user/token-negative");
  const ndn::Name serviceName("/Token/Negative");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-negative"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group-token-negative"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  size_t handlerCalls = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler{},
    ServiceProvider::SimpleRequestHandler([&](const RequestMessage&) {
      ++handlerCalls;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    }),
    ServiceProvider::ServiceInvocationMode::NormalAndTargeted);

  auto invoke = [&](const std::string& requestSuffix,
                    const std::string& userToken,
                    const std::string& providerToken) {
    auto request = makeRequestMessageWithUserToken("payload", userToken);
    request.setRequestMode(tlv::TargetedRequest);
    request.setTargetProvider(providerName);
    request.setProviderToken(providerToken);
    return provider.handleDecryptedRequestByName(
      makeRequestNameV2(requesterName, serviceName, ndn::Name(requestSuffix)), request);
  };

  BOOST_CHECK(!invoke("/missing-user", "", "provider-token").getStatus());
  BOOST_CHECK(!invoke("/missing-provider", "user-token", "").getStatus());
  BOOST_CHECK(!invoke("/unknown-provider", "user-token", "unknown-token").getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 0);

  provider.addTargetedProviderTokenForTest(requesterName,
                                           serviceName,
                                           "provider-token",
                                           "expected-user-token");
  BOOST_CHECK(!invoke("/mismatched-user", "wrong-user-token", "provider-token").getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 0);

  const auto accepted = invoke("/accepted", "expected-user-token", "provider-token");
  BOOST_CHECK(accepted.getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
  BOOST_CHECK(!invoke("/consumed-replay", "expected-user-token", "provider-token").getStatus());
  BOOST_CHECK_EQUAL(handlerCalls, 1);
}

BOOST_AUTO_TEST_CASE(TargetedDeadlineStartsBeforePublicationReturns)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-deadline-publish",
                                   "tpm-memory:targeted-deadline-publish");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/deadline-publish");
  const ndn::Name providerName("/test/provider/deadline-publish");
  const ndn::Name serviceName("/Inference/TargetedDeadline");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-deadline-publish"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group-deadline-publish"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));
  user.addTargetedTokenPairForTest(providerName,
                                   serviceName,
                                   "provider-token-deadline",
                                   "user-token-deadline");

  user.setRequestPublisher(
    [] (const ndn::Name&,
        const ndn::Name&,
        const std::vector<ndn::Name>&,
        const ndn::Name&,
        const RequestMessage&,
        size_t) {
      throw std::runtime_error("injected targeted publication failure");
    });

  size_t timeoutCallbacks = 0;
  size_t responseCallbacks = 0;
  RequestMessage request;
  BOOST_CHECK_THROW(
    user.RequestServiceTargeted(
      providerName,
      serviceName,
      std::move(request),
      20,
      [&] (const ndn::Name&) { ++timeoutCallbacks; },
      [&] (const ResponseMessage&) { ++responseCallbacks; }),
    std::runtime_error);

  face.processEvents(ndn::time::milliseconds(40));
  BOOST_CHECK_EQUAL(timeoutCallbacks, 1);
  BOOST_CHECK_EQUAL(responseCallbacks, 0);
}

BOOST_AUTO_TEST_CASE(TargetedDeadlineIncludesAdmissionQueueDelay)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-deadline-admission",
                                   "tpm-memory:targeted-deadline-admission");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/deadline-admission");
  const ndn::Name providerName("/test/provider/deadline-admission");
  const ndn::Name serviceName("/Inference/TargetedAdmissionDeadline");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-deadline-admission"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group-deadline-admission"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  ServiceUser::AdaptiveAdmissionOptions options;
  options.enabled = true;
  options.minWindow = 1;
  options.maxWindow = 1;
  options.initialWindow = 1;
  options.hardInflightLimit = 1;
  options.softQueueLimit = 2;
  options.hardQueueLimit = 2;
  user.setAdaptiveAdmissionControl(options);

  size_t published = 0;
  user.setRequestPublisher(
    [&] (const ndn::Name&,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage&,
         size_t) {
      ++published;
    });

  user.addTargetedTokenPairForTest(providerName, serviceName,
                                   "provider-token-admission-1",
                                   "user-token-admission-1");
  user.addTargetedTokenPairForTest(providerName, serviceName,
                                   "provider-token-admission-2",
                                   "user-token-admission-2");
  size_t firstTimeouts = 0;
  size_t queuedTimeouts = 0;
  const auto first = user.RequestServiceTargeted(
    providerName, serviceName, RequestMessage(), 1000,
    [&] (const ndn::Name&) { ++firstTimeouts; },
    [] (const ResponseMessage&) {});
  const auto queued = user.RequestServiceTargeted(
    providerName, serviceName, RequestMessage(), 20,
    [&] (const ndn::Name&) { ++queuedTimeouts; },
    [] (const ResponseMessage&) {});

  BOOST_REQUIRE(!first.empty());
  BOOST_REQUIRE(!queued.empty());
  BOOST_CHECK_EQUAL(published, 1);
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionQueueDepth(), 1);
  face.processEvents(ndn::time::milliseconds(40));
  BOOST_CHECK_EQUAL(queuedTimeouts, 1);
  BOOST_CHECK_EQUAL(firstTimeouts, 0);
  BOOST_CHECK(!user.hasPendingCall(queued));
  BOOST_CHECK(user.hasPendingCall(first));
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionQueueDepth(), 0);
  BOOST_CHECK_EQUAL(published, 1);
}

BOOST_AUTO_TEST_CASE(TargetedResponseAndTimeoutHaveExactlyOneTerminalCallback)
{
  ndn::security::KeyChain keyChain("pib-memory:targeted-deadline-race",
                                   "tpm-memory:targeted-deadline-race");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/deadline-race");
  const ndn::Name providerName("/test/provider/deadline-race");
  const ndn::Name serviceName("/Inference/TargetedRace");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-deadline-race"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group-deadline-race"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));
  user.setRequestPublisher(
    [] (const ndn::Name&,
        const ndn::Name&,
        const std::vector<ndn::Name>&,
        const ndn::Name&,
        const RequestMessage&,
        size_t) {});

  size_t timeoutCallbacks = 0;
  size_t responseCallbacks = 0;
  user.addTargetedTokenPairForTest(providerName,
                                   serviceName,
                                   "provider-token-response-first",
                                   "user-token-response-first");
  const auto responseFirstId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    RequestMessage(),
    30,
    [&] (const ndn::Name&) { ++timeoutCallbacks; },
    [&] (const ResponseMessage&) { ++responseCallbacks; });
  BOOST_REQUIRE(!responseFirstId.empty());

  ResponseMessage responseFirst;
  responseFirst.setStatus(true);
  responseFirst.setUserToken("user-token-response-first");
  BOOST_CHECK(user.handleDecryptedResponse(responseFirstId,
                                           providerName,
                                           responseFirst));
  BOOST_CHECK(!user.handleDecryptedResponse(responseFirstId,
                                            providerName,
                                            responseFirst));
  face.processEvents(ndn::time::milliseconds(50));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK_EQUAL(timeoutCallbacks, 0);
  BOOST_CHECK(!user.hasPendingCall(responseFirstId));

  user.addTargetedTokenPairForTest(providerName,
                                   serviceName,
                                   "provider-token-timeout-first",
                                   "user-token-timeout-first");
  const auto timeoutFirstId = user.RequestServiceTargeted(
    providerName,
    serviceName,
    RequestMessage(),
    20,
    [&] (const ndn::Name&) { ++timeoutCallbacks; },
    [&] (const ResponseMessage&) { ++responseCallbacks; });
  BOOST_REQUIRE(!timeoutFirstId.empty());
  face.processEvents(ndn::time::milliseconds(40));
  BOOST_CHECK_EQUAL(timeoutCallbacks, 1);
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK(!user.hasPendingCall(timeoutFirstId));

  ResponseMessage timeoutFirst;
  timeoutFirst.setStatus(true);
  timeoutFirst.setUserToken("user-token-timeout-first");
  BOOST_CHECK(!user.handleDecryptedResponse(timeoutFirstId,
                                            providerName,
                                            timeoutFirst));
  BOOST_CHECK_EQUAL(timeoutCallbacks, 1);
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
