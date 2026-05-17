#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <map>
#include <string>

namespace ndn_service_framework::test {
namespace {

class DynamicRequest
{
public:
  void
  setPayload(std::string value)
  {
    payload = std::move(value);
  }

  const std::string&
  getPayload() const
  {
    return payload;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = payload;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    payload.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string payload;
};

class DynamicResponse
{
public:
  void
  setClassification(int value)
  {
    classification = value;
  }

  int
  getClassification() const
  {
    return classification;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = std::to_string(classification);
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    classification = std::stoi(std::string(static_cast<const char*>(data), size));
    return true;
  }

private:
  int classification = 0;
};

ndn::security::Certificate
makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return id.getDefaultKey().getDefaultCertificate();
}

class LocalServiceUser : public ServiceUser
{
public:
  LocalServiceUser(ndn::Face& face,
                   const ndn::Name& groupPrefix,
                   const ndn::security::Certificate& identityCert,
                   const ndn::security::Certificate& attrAuthorityCertificate,
                   const std::string& trustSchemaPath)
    : ServiceUser(LocalMockTag{},
                  face,
                  groupPrefix,
                  identityCert,
                  attrAuthorityCertificate,
                  trustSchemaPath)
  {
  }

  size_t
  getPendingRequestAckCount(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return 0;
    }
    return pending->second.requestAcks.size();
  }

  ndn::Name
  getSelectedProvider(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return ndn::Name();
    }
    return pending->second.selectedProvider;
  }

  void
  addPendingCallForTokenTest(const ndn::Name& requestId,
                             const ndn::Name& serviceName,
                             const std::string& userToken,
                             size_t strategy = tlv::FirstResponding)
  {
    PendingCall pendingCall;
    pendingCall.serviceName = serviceName;
    pendingCall.strategy = strategy;
    pendingCall.requestMessage.setUserToken(userToken);
    m_pendingCalls[requestId] = pendingCall;
  }
};

class LocalServiceProvider : public ServiceProvider
{
public:
  LocalServiceProvider(ndn::Face& face,
                       const ndn::Name& groupPrefix,
                       const ndn::security::Certificate& identityCert,
                       const ndn::security::Certificate& attrAuthorityCertificate,
                       const std::string& trustSchemaPath)
    : ServiceProvider(LocalMockTag{},
                      face,
                      groupPrefix,
                      identityCert,
                      attrAuthorityCertificate,
                      trustSchemaPath)
  {
  }

  void
  addPendingRequestForTokenTest(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId,
                                const RequestMessage& requestMessage,
                                const std::string& providerToken)
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    pendingRequests[key] = std::make_shared<RequestMessage>(requestMessage);
    pendingProviderTokens[key] = providerToken;
  }
};

RequestAckMessage
makeSuccessAck()
{
  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("Permission Granted");
  return ack;
}

PermissionResponse
makePermissionResponse(const ndn::Name& targetIdentity,
                       size_t permissionKind,
                       const ndn::Name& providerName,
                       const ndn::Name& serviceName)
{
  PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(1);

  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.addEntry(entry);
  return response;
}

void
installPermissions(LocalServiceUser& user,
                   ServiceProvider& provider,
                   const ndn::Name& requesterName,
                   const ndn::Name& serviceName)
{
  const ndn::Name providerName = provider.getName();
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
}

void
runLocalFlow(LocalServiceUser& user,
             ServiceProvider& provider,
             const ndn::Name& serviceName,
             const std::string& requestPayload,
             int classification)
{
  const ndn::Name providerName = provider.getName();
  installPermissions(user,
                     provider,
                     ndn::Name("/test/user/alice"),
                     serviceName);
  bool providerHandlerCalled = false;

  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&](const ndn::Name& requester, const DynamicRequest& request, DynamicResponse& response) {
        BOOST_CHECK_EQUAL(requester, ndn::Name("/test/user/alice"));
        BOOST_CHECK_EQUAL(request.getPayload(), requestPayload);
        providerHandlerCalled = true;
        response.setClassification(classification);
      }));
  BOOST_CHECK(provider.hasService(serviceName));

  user.setRequestPublisher(
    [&](const ndn::Name& requestId,
        const ndn::Name& requestName,
        const std::vector<ndn::Name>& providers,
        const ndn::Name& publishedServiceName,
        const RequestMessage& requestMessage,
        size_t strategy) {
      BOOST_CHECK(!requestId.empty());
      BOOST_REQUIRE_EQUAL(providers.size(), 1);
      BOOST_CHECK_EQUAL(providers.front(), providerName);
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);

      const auto parsedRequest = parseRequestNameV2(requestName);
      BOOST_REQUIRE(parsedRequest);
      BOOST_CHECK_EQUAL(parsedRequest->requesterName, ndn::Name("/test/user/alice"));
      BOOST_CHECK_EQUAL(parsedRequest->serviceName, serviceName);
      BOOST_CHECK_EQUAL(parsedRequest->requestId, requestId);

      const auto ackName = makeRequestAckNameV2(providerName,
                                                parsedRequest->requesterName,
                                                serviceName,
                                                requestId);
      auto ack = makeSuccessAck();
      ack.setUserToken(requestMessage.getUserToken());
      ack.setProviderToken("provider-token");
      BOOST_CHECK(user.handleRequestAckByName(ackName, ack));
      BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
      BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);

      const auto response = provider.handleDecryptedRequestByName(requestName, requestMessage);
      BOOST_CHECK(response.getStatus());

      const auto responseName = makeResponseNameV2(providerName,
                                                   parsedRequest->requesterName,
                                                   serviceName,
                                                   requestId);
      const auto parsedResponse = parseResponseNameV2(responseName);
      BOOST_REQUIRE(parsedResponse);
      BOOST_CHECK_EQUAL(parsedResponse->providerName, providerName);
      BOOST_CHECK_EQUAL(parsedResponse->requesterName, ndn::Name("/test/user/alice"));
      BOOST_CHECK_EQUAL(parsedResponse->serviceName, serviceName);
      BOOST_CHECK_EQUAL(parsedResponse->requestId, requestId);

      BOOST_CHECK(user.handleDecryptedResponseByName(responseName, response));
    });

  bool callbackCalled = false;
  DynamicRequest request;
  request.setPayload(requestPayload);

  const auto requestId = user.asyncCall<DynamicRequest, DynamicResponse>(
    {providerName},
    serviceName,
    request,
    std::function<void(const DynamicResponse&)>(
      [&](const DynamicResponse& response) {
        BOOST_CHECK_EQUAL(response.getClassification(), classification);
        callbackCalled = true;
      }),
    std::function<void()>([] {
      BOOST_FAIL("local dynamic API test should not time out");
    }),
    1000,
    tlv::FirstResponding);

  BOOST_CHECK(!requestId.empty());
  BOOST_CHECK(providerHandlerCalled);
  BOOST_CHECK(callbackCalled);
}

RequestMessage
makeRequestMessageWithUserToken(const std::string& payload,
                                const std::string& userToken = "user-token")
{
  RequestMessage request;
  request.setUserToken(userToken);
  ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
  request.setPayload(payloadBuffer, payloadBuffer.size());
  request.setStrategy(tlv::FirstResponding);
  return request;
}

} // namespace

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)

BOOST_AUTO_TEST_CASE(V2RequestAndResponseNames)
{
  const ndn::Name requester("/test/user/alice");
  const ndn::Name provider("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name bloomFilter("/ff00");
  const ndn::Name requestId("/request-1");

  const auto requestName = makeRequestNameV2(requester, serviceName, bloomFilter, requestId);
  const auto parsedRequest = parseRequestNameV2(requestName);
  BOOST_REQUIRE(parsedRequest);
  BOOST_CHECK_EQUAL(parsedRequest->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedRequest->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedRequest->bloomFilter, bloomFilter);
  BOOST_CHECK_EQUAL(parsedRequest->requestId, requestId);

  const auto responseName = makeResponseNameV2(provider, requester, serviceName, requestId);
  const auto parsedResponse = parseResponseNameV2(responseName);
  BOOST_REQUIRE(parsedResponse);
  BOOST_CHECK_EQUAL(parsedResponse->providerName, provider);
  BOOST_CHECK_EQUAL(parsedResponse->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedResponse->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedResponse->requestId, requestId);
}

BOOST_AUTO_TEST_CASE(AddHandlerAsyncCallDispatchResponseAndAck)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-dynamic-api", "tpm-memory:generic-dynamic-api");
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/alice"));
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/camera"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  runLocalFlow(user, provider, ndn::Name("/ObjectDetection/YOLOv8"), "local-image-bytes", 42);
  runLocalFlow(user, provider, ndn::Name("/LLM/Llama3/Prefill"), "prompt-tokens", 7);
}

BOOST_AUTO_TEST_CASE(MessageTokenFieldsRoundTrip)
{
  RequestMessage request;
  request.setUserToken("user-token");
  RequestMessage decodedRequest;
  BOOST_CHECK(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decodedRequest.getUserToken(), "user-token");

  RequestAckMessage ack;
  ack.setUserToken("user-token");
  ack.setProviderToken("provider-token");
  RequestAckMessage decodedAck;
  BOOST_CHECK(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_CHECK_EQUAL(decodedAck.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedAck.getProviderToken(), "provider-token");

  ServiceCoordinationMessage coordination;
  coordination.setProviderToken("provider-token");
  ServiceCoordinationMessage decodedCoordination;
  BOOST_CHECK(decodedCoordination.WireDecode(coordination.WireEncode()));
  BOOST_CHECK_EQUAL(decodedCoordination.getProviderToken(), "provider-token");

  ResponseMessage response;
  response.setUserToken("user-token");
  ResponseMessage decodedResponse;
  BOOST_CHECK(decodedResponse.WireDecode(response.WireEncode()));
  BOOST_CHECK_EQUAL(decodedResponse.getUserToken(), "user-token");
}

BOOST_AUTO_TEST_CASE(ProviderRequiresPermissionAndUserToken)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-auth-negative",
                                   "tpm-memory:generic-auth-negative");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name requestId("/request-auth-negative");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-auth-negative"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  bool handlerCalled = false;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerCalled = true;
        response.setClassification(1);
      }));

  installPermissions(user, provider, requesterName, serviceName);

  const auto requestName = makeRequestNameV2(requesterName,
                                            serviceName,
                                            ndn::Name("/bf"),
                                            requestId);
  auto goodRequest = makeRequestMessageWithUserToken("payload");
  auto goodResponse = provider.handleDecryptedRequestByName(requestName, goodRequest);
  BOOST_CHECK(goodResponse.getStatus());
  BOOST_CHECK_EQUAL(goodResponse.getUserToken(), goodRequest.getUserToken());
  BOOST_CHECK(handlerCalled);

  handlerCalled = false;
  auto missingUserTokenRequest = makeRequestMessageWithUserToken("payload", "");
  auto missingUserTokenResponse =
    provider.handleDecryptedRequestByName(requestName, missingUserTokenRequest);
  BOOST_CHECK(!missingUserTokenResponse.getStatus());
  BOOST_CHECK(!handlerCalled);

  ServiceProvider providerWithoutPermission(ServiceProvider::LocalMockTag{},
                                            face,
                                            ndn::Name("/test/group"),
                                            providerCert,
                                            aaCert,
                                            "examples/trust-any.conf");
  providerWithoutPermission.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerCalled = true;
        response.setClassification(2);
      }));

  auto missingProviderPermissionResponse =
    providerWithoutPermission.handleDecryptedRequestByName(requestName, goodRequest);
  BOOST_CHECK(!missingProviderPermissionResponse.getStatus());
  BOOST_CHECK(!handlerCalled);
}

BOOST_AUTO_TEST_CASE(TokenHandshakeNegativeRegression)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-handshake-negative",
                                   "tpm-memory:token-handshake-negative");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-negative");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-negative"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");

  auto ackName = makeRequestAckNameV2(providerName, requesterName, serviceName, requestId);
  auto wrongUserAck = makeSuccessAck();
  wrongUserAck.setUserToken("wrong-user-token");
  wrongUserAck.setProviderToken("provider-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, wrongUserAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0);

  auto missingProviderTokenAck = makeSuccessAck();
  missingProviderTokenAck.setUserToken("user-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, missingProviderTokenAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0);

  auto goodAck = makeSuccessAck();
  goodAck.setUserToken("user-token");
  goodAck.setProviderToken("provider-token");
  BOOST_CHECK(user.handleRequestAckByName(ackName, goodAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  ResponseMessage wrongUserResponse;
  wrongUserResponse.setStatus(true);
  wrongUserResponse.setUserToken("wrong-user-token");
  BOOST_CHECK(!user.handleDecryptedResponse(requestId, wrongUserResponse));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  installPermissions(user, provider, requesterName, serviceName);

  bool providerHandlerCalled = false;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        providerHandlerCalled = true;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         "provider-token");

  ServiceCoordinationMessage wrongCoordination;
  wrongCoordination.setRequestIDs({requestId.toUri()});
  wrongCoordination.setProviderToken("wrong-provider-token");
  auto wrongCoordinationBlock = wrongCoordination.WireEncode();
  ndn::Buffer wrongCoordinationBuffer(wrongCoordinationBlock.data(),
                                      wrongCoordinationBlock.size());
  provider.OnServiceCoordinationMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    wrongCoordinationBuffer);
  BOOST_CHECK(!providerHandlerCalled);
}

BOOST_AUTO_TEST_CASE(BaseServiceProviderDefaultsAreSafe)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-provider-defaults",
                                   "tpm-memory:generic-provider-defaults");
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/default"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-default"));

  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  provider.registerServiceInfo();

  RequestMessage requestMessage;
  provider.ConsumeRequest(ndn::Name("/test/user/alice"),
                          provider.getName(),
                          ndn::Name("/Unregistered"),
                          ndn::Name("/Endpoint"),
                          ndn::Name("/request-default"),
                          requestMessage);

  BOOST_CHECK(!provider.getName().empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
