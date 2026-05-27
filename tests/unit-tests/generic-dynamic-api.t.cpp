#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

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

  std::vector<ndn::Name>
  getSuccessfulAckProviders(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return {};
    }
    return pending->second.successfulAckProviders;
  }

  std::vector<ndn::Name>
  getExpectedResponseProviders(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return {};
    }
    return pending->second.expectedResponseProviders;
  }

  std::vector<ndn::Name>
  getSelectionPublishedProviders(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return {};
    }
    return pending->second.selectionPublishedProviders;
  }

  bool
  hasPendingCall(const ndn::Name& requestId) const
  {
    return m_pendingCalls.find(requestId) != m_pendingCalls.end();
  }

  bool
  isAckWindowExpired(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending != m_pendingCalls.end() && pending->second.ackWindowExpired;
  }

  bool
  hasLegacyStrategyState(const ndn::Name& requestId) const
  {
    return m_strategyMap.find(requestId) != m_strategyMap.end();
  }

  bool
  hasCachedDataForTest(const ndn::Name& dataName)
  {
    return m_IMS.find(dataName) != nullptr;
  }

  ndn::Buffer
  getCachedDataContentForTest(const ndn::Name& dataName)
  {
    auto data = m_IMS.find(dataName);
    if (data == nullptr) {
      return ndn::Buffer();
    }
    const auto& content = data->getContent();
    return ndn::Buffer(content.value(), content.value_size());
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

  void
  setPendingResponseHandlerForTest(const ndn::Name& requestId,
                                   ResponseHandler responseHandler)
  {
    m_pendingCalls[requestId].responseHandler = std::move(responseHandler);
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

  void
  cleanupPendingRequestStateForTest(const ndn::Name& requesterName,
                                    const ndn::Name& serviceName,
                                    const ndn::Name& requestId)
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    cleanupPendingRequestState(key);
  }

  bool
  expirePendingRequestStateForTest(const ndn::Name& requesterName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId)
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    return expirePendingRequestState(key);
  }

  bool
  hasPendingRequestForTokenTest(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId) const
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    return pendingRequests.find(key) != pendingRequests.end();
  }

  bool
  hasPendingProviderTokenForTest(const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId) const
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    return pendingProviderTokens.find(key) != pendingProviderTokens.end();
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

  const auto requestId = user.RequestService<DynamicRequest, DynamicResponse>(
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

ndn::Buffer
makeSelectionBuffer(const ndn::Name& requestId, const std::string& providerToken)
{
  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setProviderToken(providerToken);
  auto block = selection.WireEncode();
  return ndn::Buffer(block.data(), block.size());
}

RequestAckMessage
makeSuccessAckForRequest(const RequestMessage& requestMessage,
                         const std::string& providerToken = "provider-token")
{
  auto ack = makeSuccessAck();
  ack.setUserToken(requestMessage.getUserToken());
  ack.setProviderToken(providerToken);
  return ack;
}

void
pumpFace(ndn::Face& face, ndn::time::milliseconds duration)
{
  face.getIoContext().restart();
  face.getIoContext().run_for(std::chrono::milliseconds(duration.count()));
}

bool
namesContain(const std::vector<ndn::Name>& names, const ndn::Name& name)
{
  return std::any_of(names.begin(), names.end(), [&] (const ndn::Name& item) {
    return item.equals(name);
  });
}

} // namespace

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)

BOOST_AUTO_TEST_CASE(PreparedServiceRequestOnlyCreatesContext)
{
  ndn::security::KeyChain keyChain("pib-memory:prepared-context",
                                   "tpm-memory:prepared-context");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-prepared-context"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  bool published = false;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage&, size_t) {
      published = true;
    });

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  BOOST_CHECK(!ctx.requestId.empty());
  BOOST_CHECK_EQUAL(ctx.serviceName, serviceName);
  BOOST_CHECK(!published);
  BOOST_CHECK_EQUAL(user.getPendingCallCount(), 0);
  BOOST_CHECK(!user.hasPendingCall(ctx.requestId));
}

BOOST_AUTO_TEST_CASE(PreparedRequestServicePreservesOpaquePayloadAndRequestId)
{
  ndn::security::KeyChain keyChain("pib-memory:prepared-opaque",
                                   "tpm-memory:prepared-opaque");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-prepared-opaque"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::string payloadText =
    "{imageDataName:\"/encrypted/image\",configDataName:\"/encrypted/config\"}";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                      payloadText.size());
  RequestMessage request;
  request.setPayload(payload, payload.size());

  bool published = false;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedServiceName,
         const RequestMessage& requestMessage,
         size_t) {
      published = true;
      BOOST_CHECK_EQUAL(requestId, ctx.requestId);
      BOOST_REQUIRE_EQUAL(providers.size(), 1);
      BOOST_CHECK_EQUAL(providers.front(), providerName);
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      const auto parsed = parseRequestNameV2(requestName);
      BOOST_REQUIRE(parsed);
      BOOST_CHECK_EQUAL(parsed->requestId, ctx.requestId);
      const auto publishedPayload = requestMessage.getPayload();
      BOOST_REQUIRE_EQUAL(publishedPayload.size(), payloadText.size());
      BOOST_CHECK(std::equal(publishedPayload.begin(),
                             publishedPayload.end(),
                             payloadText.begin()));
    });

  const auto requestId = user.RequestService(
    ctx,
    {providerName},
    request,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(requestId, ctx.requestId);
  BOOST_CHECK(published);
  BOOST_CHECK(user.hasPendingCall(ctx.requestId));
  BOOST_CHECK(user.RequestService(ctx,
                              RequestMessage(),
                              100,
                              ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
                              ServiceUser::ResponseHandler([] (const ResponseMessage&) {})).empty());
}

BOOST_AUTO_TEST_CASE(AdaptiveAdmissionControlWarnsAtSoftLimitAndRejectsAtHardLimit)
{
  ndn::security::KeyChain keyChain("pib-memory:adaptive-admission-queue",
                                   "tpm-memory:adaptive-admission-queue");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/admission");
  const ndn::Name providerName("/test/provider/admission");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-admission"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ServiceUser::AdaptiveAdmissionOptions options;
  options.enabled = true;
  options.minWindow = 1;
  options.maxWindow = 1;
  options.initialWindow = 1;
  options.hardInflightLimit = 1;
  options.softQueueLimit = 1;
  options.hardQueueLimit = 1;
  user.setAdaptiveAdmissionControl(options);

  size_t published = 0;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage&, size_t) {
      ++published;
    });

  size_t admissionWarnings = 0;
  size_t admissionRejects = 0;
  size_t lastRemainingHardSlots = 99;
  user.setAdmissionControlWarningHandler(
    [&] (const ServiceUser::AdmissionControlStatus& status) {
      lastRemainingHardSlots = status.remainingHardSlots;
      ++admissionWarnings;
    });
  user.setAdmissionControlRejectHandler(
    [&] (const ServiceUser::AdmissionControlStatus& status) {
      lastRemainingHardSlots = status.remainingHardSlots;
      ++admissionRejects;
    });

  RequestMessage request;
  const std::string payloadText = "HELLO";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                      payloadText.size());
  request.setPayload(payload, payload.size());

  size_t timeoutCallbacks = 0;
  auto timeout = [&] (const ndn::Name&) { ++timeoutCallbacks; };
  auto response = [] (const ResponseMessage&) {};

  const auto first = user.RequestService({providerName}, serviceName, request, 100, timeout, response);
  const auto second = user.RequestService({providerName}, serviceName, request, 100, timeout, response);
  const auto third = user.RequestService({providerName}, serviceName, request, 100, timeout, response);

  BOOST_CHECK(!first.empty());
  BOOST_CHECK(!second.empty());
  BOOST_CHECK(!third.empty());
  BOOST_CHECK_EQUAL(published, 1);
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionInflight(), 1);
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionQueueDepth(), 1);
  BOOST_CHECK_EQUAL(admissionWarnings, 1);
  BOOST_CHECK_EQUAL(admissionRejects, 1);
  BOOST_CHECK_EQUAL(lastRemainingHardSlots, 0);
  BOOST_CHECK_EQUAL(timeoutCallbacks, 0);

  const auto rejectedStatus = user.getRequestStatus(third);
  BOOST_REQUIRE(rejectedStatus);
  BOOST_CHECK_EQUAL(ServiceUser::requestLifecycleStateToString(rejectedStatus->state),
                    std::string("ADMISSION_REJECTED"));
  BOOST_CHECK_EQUAL(rejectedStatus->finalCleanupReason, "admission_queue_full");
}

BOOST_AUTO_TEST_CASE(LargeDataNamePayloadRemainsOpaqueAcrossPreparedRequestService)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-name-opaque",
                                   "tpm-memory:large-data-name-opaque");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-name-opaque"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::string encryptedDataNameUri =
    "/test/user/alice/NDNSF/LARGE-DATA/1/HELLO/" + ctx.requestId.toUri() + "/image";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(encryptedDataNameUri.data()),
                      encryptedDataNameUri.size());
  RequestMessage request;
  request.setPayload(payload, payload.size());

  bool preparedPublished = false;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t) {
      if (requestId != ctx.requestId) {
        return;
      }
      preparedPublished = true;
      const auto publishedPayload = requestMessage.getPayload();
      BOOST_REQUIRE_EQUAL(publishedPayload.size(), encryptedDataNameUri.size());
      BOOST_CHECK(std::equal(publishedPayload.begin(),
                             publishedPayload.end(),
                             encryptedDataNameUri.begin()));
    });

  const auto preparedRequestId = user.RequestService(
    ctx,
    {providerName},
    request,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(preparedRequestId, ctx.requestId);
  BOOST_CHECK(preparedPublished);

  RequestMessage legacyRequest;
  legacyRequest.setPayload(payload, payload.size());
  const auto legacyRequestId = user.RequestService(
    {providerName},
    serviceName,
    legacyRequest,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK(!legacyRequestId.empty());
}

BOOST_AUTO_TEST_CASE(MultipleLargeDataObjectsUseOnePreparedRequestScope)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-multiple",
                                   "tpm-memory:large-data-multiple");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-multiple"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::vector<uint8_t> imageBytes = {'i', 'm', 'a', 'g', 'e'};
  const std::vector<uint8_t> configBytes = {'c', 'o', 'n', 'f', 'i', 'g'};

  const auto image = user.publishEncryptedLargeData(ctx, imageBytes, "image");
  const auto config = user.publishEncryptedLargeData(ctx, configBytes, "config");
  if (!image.success || !config.success) {
    BOOST_TEST_MESSAGE("NAC-ABE large-data production unavailable in local mock: "
                       << image.errorMessage << " " << config.errorMessage);
    BOOST_CHECK(!image.errorMessage.empty());
    BOOST_CHECK(!config.errorMessage.empty());
    return;
  }

  BOOST_CHECK(image.success);
  BOOST_CHECK(config.success);
  BOOST_CHECK(!image.encryptedDataName.get(-1).isSegment());
  BOOST_CHECK(!config.encryptedDataName.get(-1).isSegment());
  BOOST_CHECK_NE(image.encryptedDataName, config.encryptedDataName);
  BOOST_CHECK(image.encryptedDataName.toUri().find(ctx.requestId.toUri()) != std::string::npos);
  BOOST_CHECK(config.encryptedDataName.toUri().find(ctx.requestId.toUri()) != std::string::npos);
  BOOST_CHECK_NE(image.objectId, config.objectId);
  BOOST_CHECK(user.hasCachedDataForTest(image.encryptedDataName));
  BOOST_CHECK(user.hasCachedDataForTest(config.encryptedDataName));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(image.encryptedDataName),
                 ndn::Buffer(imageBytes.begin(), imageBytes.end()));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(config.encryptedDataName),
                 ndn::Buffer(configBytes.begin(), configBytes.end()));
}

BOOST_AUTO_TEST_CASE(MissingLargeDataFetchFailsCleanly)
{
  ndn::security::KeyChain keyChain("pib-memory:missing-large-data",
                                   "tpm-memory:missing-large-data");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/camera");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-missing-large-data"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  const auto result =
    provider.fetchAndDecryptLargeData(ndn::Name("/missing/large/data"), "/HELLO");
  BOOST_CHECK(!result.success);
  BOOST_CHECK(!result.errorMessage.empty());
}

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

BOOST_AUTO_TEST_CASE(AddHandlerRequestServiceDispatchResponseAndAck)
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
  request.setPolicyEpoch(42);
  RequestMessage decodedRequest;
  BOOST_CHECK(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decodedRequest.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedRequest.getPolicyEpoch(), 42);

  RequestAckMessage ack;
  ack.setUserToken("user-token");
  ack.setProviderToken("provider-token");
  ack.setPolicyEpoch(42);
  RequestAckMessage decodedAck;
  BOOST_CHECK(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_CHECK_EQUAL(decodedAck.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedAck.getProviderToken(), "provider-token");
  BOOST_CHECK_EQUAL(decodedAck.getPolicyEpoch(), 42);

  ServiceSelectionMessage selection;
  selection.setProviderToken("provider-token");
  selection.setPolicyEpoch(42);
  ServiceSelectionMessage decodedSelection;
  BOOST_CHECK(decodedSelection.WireDecode(selection.WireEncode()));
  BOOST_CHECK_EQUAL(decodedSelection.getProviderToken(), "provider-token");
  BOOST_CHECK_EQUAL(decodedSelection.getPolicyEpoch(), 42);

  ResponseMessage response;
  response.setUserToken("user-token");
  response.setPolicyEpoch(42);
  ResponseMessage decodedResponse;
  BOOST_CHECK(decodedResponse.WireDecode(response.WireEncode()));
  BOOST_CHECK_EQUAL(decodedResponse.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedResponse.getPolicyEpoch(), 42);

  PolicyManifest manifest;
  manifest.setPolicyEpoch(42);
  manifest.setValidFromMs(1234);
  manifest.setGracePeriodMs(5000);
  manifest.setRequiredKeyEpoch(43);
  PolicyManifest decodedManifest;
  BOOST_CHECK(decodedManifest.WireDecode(manifest.WireEncode()));
  BOOST_CHECK_EQUAL(decodedManifest.getPolicyEpoch(), 42);
  BOOST_CHECK_EQUAL(decodedManifest.getValidFromMs(), 1234);
  BOOST_CHECK_EQUAL(decodedManifest.getGracePeriodMs(), 5000);
  BOOST_CHECK_EQUAL(decodedManifest.getRequiredKeyEpoch(), 43);
}

BOOST_AUTO_TEST_CASE(HybridMessageEnvelopeProtectsRequestPayloadAndUserToken)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/user/alice");
  auto key = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                       "REQUEST", counters);

  RequestMessage request;
  request.setUserToken("user-token-secret");
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload-secret"), 14);
  request.setPayload(payload, payload.size());
  const auto plaintext = request.WireEncode();
  const auto ad = hybridAssociatedData(
    ndn::Name("/test/user/alice/NDNSF/REQUEST/1/HELLO/bloom/rid"),
    "REQUEST", ndn::Name("/rid"), serviceName, sender, key.keyId, key.epochId);

  auto encrypted = hybridAesGcmEncrypt(
    key.key,
    ndn::span<const uint8_t>(&*plaintext.begin(), plaintext.size()),
    ndn::span<const uint8_t>(ad.data(), ad.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType("REQUEST");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);

  const auto envelopeWire = envelope.WireEncode();
  const std::string envelopeBytes(
    reinterpret_cast<const char*>(&*envelopeWire.begin()),
    envelopeWire.size());
  BOOST_CHECK_EQUAL(envelopeBytes.find("user-token-secret"), std::string::npos);
  BOOST_CHECK_EQUAL(envelopeBytes.find("payload-secret"), std::string::npos);

  ndn::Buffer decrypted;
  BOOST_REQUIRE(hybridAesGcmDecrypt(key.key, envelope,
                                    ndn::span<const uint8_t>(ad.data(), ad.size()),
                                    decrypted));
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(ndn::Block(decrypted)));
  BOOST_CHECK_EQUAL(decoded.getUserToken(), "user-token-secret");
  BOOST_CHECK_EQUAL(decoded.getPayloadSize(), 14);
}

BOOST_AUTO_TEST_CASE(HybridMessageEnvelopeProtectsAckProviderTokenAndDetectsTamper)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/provider/a");
  auto key = crypto.getOrCreateSendKey(serviceName, sender, "/PERMISSION/HELLO",
                                       "ACK", counters);

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("ready");
  ack.setUserToken("user-token-secret");
  ack.setProviderToken("provider-token-secret");
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("queue=0"), 7);
  ack.setPayload(payload, payload.size());
  const auto plaintext = ack.WireEncode();
  const auto ad = hybridAssociatedData(
    ndn::Name("/test/provider/a/NDNSF/ACK/3/test/user/alice/1/HELLO/rid"),
    "ACK", ndn::Name("/rid"), serviceName, sender, key.keyId, key.epochId);

  auto encrypted = hybridAesGcmEncrypt(
    key.key,
    ndn::span<const uint8_t>(&*plaintext.begin(), plaintext.size()),
    ndn::span<const uint8_t>(ad.data(), ad.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType("ACK");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);

  const auto envelopeWire = envelope.WireEncode();
  const std::string envelopeBytes(
    reinterpret_cast<const char*>(&*envelopeWire.begin()),
    envelopeWire.size());
  BOOST_CHECK_EQUAL(envelopeBytes.find("provider-token-secret"), std::string::npos);

  ndn::Buffer decrypted;
  BOOST_REQUIRE(hybridAesGcmDecrypt(key.key, envelope,
                                    ndn::span<const uint8_t>(ad.data(), ad.size()),
                                    decrypted));
  RequestAckMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(ndn::Block(decrypted)));
  BOOST_CHECK_EQUAL(decoded.getProviderToken(), "provider-token-secret");

  auto tamperedCiphertext = envelope.getCipherText();
  tamperedCiphertext[0] ^= 0x01;
  envelope.setCipherText(tamperedCiphertext);
  BOOST_CHECK(!hybridAesGcmDecrypt(key.key, envelope,
                                   ndn::span<const uint8_t>(ad.data(), ad.size()),
                                   decrypted));
}

BOOST_AUTO_TEST_CASE(HybridKeyEpochRotatesByUsesAndNonceIsUnique)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/user/alice");

  std::set<std::string> nonces;
  std::string firstKeyId;
  for (size_t i = 0; i < HybridMessageCrypto::MAX_EPOCH_USES; ++i) {
    auto key = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                         "REQUEST", counters);
    if (i == 0) {
      firstKeyId = key.keyId;
    }
    BOOST_CHECK_EQUAL(key.keyId, firstKeyId);
    ndn::Buffer plaintext(reinterpret_cast<const uint8_t*>("x"), 1);
    ndn::Buffer ad(reinterpret_cast<const uint8_t*>("ad"), 2);
    auto encrypted = hybridAesGcmEncrypt(
      key.key,
      ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(ad.data(), ad.size()));
    nonces.insert(std::string(reinterpret_cast<const char*>(encrypted.nonce.data()),
                              encrypted.nonce.size()));
  }
  BOOST_CHECK_EQUAL(nonces.size(), HybridMessageCrypto::MAX_EPOCH_USES);

  auto rotated = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                           "REQUEST", counters);
  BOOST_CHECK_NE(rotated.keyId, firstKeyId);
  BOOST_CHECK_EQUAL(counters.hybrid_key_rotation_uses.load(), 1);
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

BOOST_AUTO_TEST_CASE(LateAckAfterAckTimeoutSelectsProviderBeforeRequestTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-selects",
                                   "tpm-memory:late-ack-selects");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-selects"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckBeforeAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-before-ack-timeout",
                                   "tpm-memory:first-responding-before-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-before-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  pumpFace(face, ndn::time::milliseconds(150));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckAfterNominalAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-ack",
                                   "tpm-memory:first-responding-late-ack");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-ack"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingIgnoresAckTimeoutCompletely)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ignores-ack-timeout",
                                   "tpm-memory:first-responding-ignores-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ignore-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    200,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(!timeoutCalled);

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingLateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-after-timeout",
                                   "tpm-memory:first-responding-late-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(FirstRespondingAckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ack-after-selected",
                                   "tpm-memory:first-responding-ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(FirstRespondingV2AckCallbackDoesNotFallThroughToLegacySelection)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-v2-no-legacy-fallback",
                                   "tpm-memory:first-responding-v2-no-legacy-fallback");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-v2-no-legacy"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  auto firstAckBlock = firstAck.WireEncode();
  ndn::Buffer firstAckBuffer(firstAckBlock.data(), firstAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerA,
                                             serviceName,
                                             ndn::Name(),
                                             requestId,
                                             firstAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  auto secondAckBlock = secondAck.WireEncode();
  ndn::Buffer secondAckBuffer(secondAckBlock.data(), secondAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerB,
                                             serviceName,
                                             ndn::Name(),
                                             requestId,
                                             secondAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));
}

BOOST_AUTO_TEST_CASE(LateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-after-timeout",
                                   "tpm-memory:late-ack-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(AckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:ack-after-selected",
                                   "tpm-memory:ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(RandomSelectionMultipleAcksWithinWindowSelectsOneCandidate)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-normal",
                                   "tpm-memory:random-selection-normal");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-normal"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("normal ACK selection should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto ackA = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    ackA));
  auto ackB = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    ackB));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  pumpFace(face, ndn::time::milliseconds(50));
  const auto selectedProvider = user.getSelectedProvider(requestId);
  BOOST_CHECK(selectedProvider.equals(providerA) || selectedProvider.equals(providerB));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 2);
}

BOOST_AUTO_TEST_CASE(RandomSelectionIgnoresFailedAcksAndKeepsPendingForLateSuccess)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-valid-only",
                                   "tpm-memory:random-selection-valid-only");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-valid-only"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto failedAck = makeSuccessAckForRequest(publishedRequest, "");
  failedAck.setStatus(false);
  failedAck.setMessage("busy");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    failedAck));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto successAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    successAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerB);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(RandomSelectionDistributionSanity)
{
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/random-distribution");
  std::vector<AckSelectionCandidate> candidates;
  for (const auto& provider : {ndn::Name("/test/provider/A"),
                               ndn::Name("/test/provider/B"),
                               ndn::Name("/test/provider/C")}) {
    AckSelectionCandidate candidate;
    candidate.providerName = provider;
    candidate.serviceName = serviceName;
    candidate.requestId = requestId;
    candidate.ack = makeSuccessAck();
    candidate.ack.setUserToken("user-token");
    candidate.ack.setProviderToken("provider-token");
    candidates.push_back(candidate);
  }

  std::map<std::string, int> selectedCounts;
  for (int i = 0; i < 200; ++i) {
    const auto selected = ServiceUser::selectRandomAck(candidates);
    BOOST_REQUIRE_EQUAL(selected.size(), 1);
    ++selectedCounts[selected.front().providerName.toUri()];
  }

  BOOST_CHECK_GE(selectedCounts.size(), 2);
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

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
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

  ServiceSelectionMessage wrongSelection;
  wrongSelection.setRequestIDs({requestId.toUri()});
  wrongSelection.setProviderToken("wrong-provider-token");
  auto wrongSelectionBlock = wrongSelection.WireEncode();
  ndn::Buffer wrongSelectionBuffer(wrongSelectionBlock.data(),
                                      wrongSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    wrongSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);

  ServiceSelectionMessage goodSelection;
  goodSelection.setRequestIDs({requestId.toUri()});
  goodSelection.setProviderToken("provider-token");
  auto goodSelectionBlock = goodSelection.WireEncode();
  ndn::Buffer goodSelectionBuffer(goodSelectionBlock.data(),
                                     goodSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  const ndn::Name replayedRequestId("/request-token-negative-new");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         replayedRequestId,
                                         requestMessage,
                                         "new-provider-token");
  ServiceSelectionMessage replayedOldTokenSelection;
  replayedOldTokenSelection.setRequestIDs({replayedRequestId.toUri()});
  replayedOldTokenSelection.setProviderToken("provider-token");
  auto replayedOldTokenBlock = replayedOldTokenSelection.WireEncode();
  ndn::Buffer replayedOldTokenBuffer(replayedOldTokenBlock.data(),
                                     replayedOldTokenBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    replayedRequestId,
    replayedOldTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
}

BOOST_AUTO_TEST_CASE(TokenModeDefaultsToEnabledAndPreservesChecks)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-default",
                                   "tpm-memory:token-mode-default");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-mode-default");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-default"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  BOOST_CHECK(user.getUseTokens());
  BOOST_CHECK(provider.getUseTokens());

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  auto ackName = makeRequestAckNameV2(providerName, requesterName, serviceName, requestId);
  auto missingProviderTokenAck = makeSuccessAck();
  missingProviderTokenAck.setUserToken("user-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, missingProviderTokenAck));
}

BOOST_AUTO_TEST_CASE(TokenModeDisabledKeepsFirstRespondingAckSelectionResponsePath)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-disabled",
                                   "tpm-memory:token-mode-disabled");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-disabled"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  user.setUseTokens(false);
  provider.setUseTokens(false);
  BOOST_CHECK(!user.getUseTokens());
  BOOST_CHECK(!provider.getUseTokens());

  installPermissions(user, provider, requesterName, serviceName);

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  ndn::Name publishedRequestId;
  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t strategy) {
      publishedRequestId = requestId;
      publishedRequest = requestMessage;
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
      BOOST_CHECK(requestMessage.getUserToken().empty());

      auto requestBlock = requestMessage.WireEncode();
      ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());
      provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                    serviceName,
                                                    ndn::Name("/bf"),
                                                    requestId,
                                                    requestBuffer);
      BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
      BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
      BOOST_CHECK(!provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

      auto ack = makeSuccessAck();
      BOOST_CHECK(ack.getUserToken().empty());
      BOOST_CHECK(ack.getProviderToken().empty());
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providerName, requesterName, serviceName, requestId), ack));
      BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
      BOOST_CHECK(!requestName.empty());
    });

  int responseCallbacks = 0;
  RequestMessage request;
  const auto requestId = user.RequestService(
    {providerName},
    serviceName,
    request,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("token-disabled FirstResponding request should not time out");
    }),
    ServiceUser::ResponseHandler([&] (const ResponseMessage& response) {
      BOOST_CHECK(response.getUserToken().empty());
      ++responseCallbacks;
    }),
    tlv::FirstResponding);

  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  auto selectionBuffer = makeSelectionBuffer(requestId, "");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  ResponseMessage response;
  response.setStatus(true);
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providerName, requesterName, serviceName, requestId),
    response));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK(!user.hasPendingCall(requestId));
  BOOST_CHECK(publishedRequest.getUserToken().empty());
}

BOOST_AUTO_TEST_CASE(TokenModeMismatchFailsClearly)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-mismatch",
                                   "tpm-memory:token-mode-mismatch");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-mode-mismatch");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-mismatch"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  user.setUseTokens(false);
  installPermissions(user, provider, requesterName, serviceName);

  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [] (const ndn::Name&,
          const ndn::Name&,
          const ndn::Name&,
          const ndn::Name&,
          const RequestMessage&) {
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage tokenlessRequest = makeRequestMessageWithUserToken("payload", "");
  auto requestBlock = tokenlessRequest.WireEncode();
  ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                ndn::Name("/bf"),
                                                requestId,
                                                requestBuffer);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));

  LocalServiceUser secureUser(face,
                              ndn::Name("/test/group"),
                              userCert,
                              aaCert,
                              "examples/trust-any.conf");
  secureUser.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  auto tokenlessAck = makeSuccessAck();
  BOOST_CHECK(!secureUser.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    tokenlessAck));
}

BOOST_AUTO_TEST_CASE(ProviderTokenCleanupExpiresUnselectedAckState)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-token-cleanup",
                                   "tpm-memory:provider-token-cleanup");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-cleanup");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-token-cleanup"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
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

  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
  BOOST_CHECK(provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  provider.cleanupPendingRequestStateForTest(requesterName, serviceName, requestId);

  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
  BOOST_CHECK(!provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  ServiceSelectionMessage staleSelection;
  staleSelection.setRequestIDs({requestId.toUri()});
  staleSelection.setProviderToken("provider-token");
  auto staleSelectionBlock = staleSelection.WireEncode();
  ndn::Buffer staleSelectionBuffer(staleSelectionBlock.data(),
                                      staleSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    staleSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);

  const ndn::Name consumedRequestId("/request-token-cleanup-consumed");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         consumedRequestId,
                                         requestMessage,
                                         "fresh-provider-token");
  ServiceSelectionMessage goodSelection;
  goodSelection.setRequestIDs({consumedRequestId.toUri()});
  goodSelection.setProviderToken("fresh-provider-token");
  auto goodSelectionBlock = goodSelection.WireEncode();
  ndn::Buffer goodSelectionBuffer(goodSelectionBlock.data(),
                                     goodSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    consumedRequestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    consumedRequestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
}

BOOST_AUTO_TEST_CASE(ProviderTokenAdversarialRejectionAndRestartBehavior)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-token-adversarial",
                                   "tpm-memory:provider-token-adversarial");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-adversarial");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-token-adversarial"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
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
                                         "expected-token");

  auto randomTokenBuffer = makeSelectionBuffer(requestId, "random-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    randomTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 0);
  BOOST_CHECK(provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  BOOST_CHECK(provider.expirePendingRequestStateForTest(requesterName, serviceName, requestId));
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 1);

  auto expiredTokenBuffer = makeSelectionBuffer(requestId, "expected-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    expiredTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 0);

  LocalServiceProvider restartedProvider(face,
                                         ndn::Name("/test/group"),
                                         providerCert,
                                         aaCert,
                                         "examples/trust-any.conf");
  restartedProvider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));
  restartedProvider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    expiredTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(restartedProvider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(restartedProvider.getPendingProviderTokenCountForTesting(), 0);
}

BOOST_AUTO_TEST_CASE(ProviderStateCleanupEdgeCases)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-cleanup-edge",
                                   "tpm-memory:provider-cleanup-edge");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-cleanup-edge"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");

  for (int i = 0; i < 25; ++i) {
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           ndn::Name("/expire-" + std::to_string(i)),
                                           requestMessage,
                                           "token-" + std::to_string(i));
  }
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 25);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 25);

  for (int i = 0; i < 25; ++i) {
    BOOST_CHECK(provider.expirePendingRequestStateForTest(
      requesterName,
      serviceName,
      ndn::Name("/expire-" + std::to_string(i))));
  }
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 25);

  const ndn::Name selectedRequestId("/cleanup-during-success");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         selectedRequestId,
                                         requestMessage,
                                         "live-token");
  auto liveSelectionBuffer = makeSelectionBuffer(selectedRequestId, "live-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    selectedRequestId,
    liveSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 1);
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  BOOST_CHECK(!provider.expirePendingRequestStateForTest(
    requesterName,
    serviceName,
    selectedRequestId));
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 26);

  for (int cycle = 0; cycle < 5; ++cycle) {
    for (int i = 0; i < 20; ++i) {
      provider.addPendingRequestForTokenTest(requesterName,
                                             serviceName,
                                             ndn::Name("/cycle-" + std::to_string(cycle) +
                                                       "-" + std::to_string(i)),
                                             requestMessage,
                                             "cycle-token");
    }
    for (int i = 0; i < 20; ++i) {
      provider.expirePendingRequestStateForTest(
        requesterName,
        serviceName,
        ndn::Name("/cycle-" + std::to_string(cycle) + "-" + std::to_string(i)));
    }
    BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
    BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  }
}

BOOST_AUTO_TEST_CASE(ProviderStateLightweightStressIsDeterministic)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-state-stress",
                                   "tpm-memory:provider-state-stress");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-state-stress"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");

  constexpr int requestCount = 250;
  std::vector<int> order;
  for (int i = 0; i < requestCount; ++i) {
    order.push_back(i);
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           ndn::Name("/stress-" + std::to_string(i)),
                                           requestMessage,
                                           "stress-token-" + std::to_string(i));
  }

  std::mt19937 rng(1337);
  std::shuffle(order.begin(), order.end(), rng);

  int expectedCompletions = 0;
  int expectedExpirations = 0;
  for (int i : order) {
    const ndn::Name requestId("/stress-" + std::to_string(i));
    if (i % 5 == 0) {
      auto wrongBuffer = makeSelectionBuffer(requestId, "wrong-token");
      provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        requesterName,
        providerName,
        serviceName,
        requestId,
        wrongBuffer);
    }

    if (i % 2 == 0) {
      BOOST_CHECK(provider.expirePendingRequestStateForTest(requesterName,
                                                            serviceName,
                                                            requestId));
      ++expectedExpirations;
      auto expiredBuffer = makeSelectionBuffer(requestId,
                                                  "stress-token-" + std::to_string(i));
      provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        requesterName,
        providerName,
        serviceName,
        requestId,
        expiredBuffer);
      continue;
    }

    auto buffer = makeSelectionBuffer(requestId, "stress-token-" + std::to_string(i));
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requesterName,
      providerName,
      serviceName,
      requestId,
      buffer);
    ++expectedCompletions;
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requesterName,
      providerName,
      serviceName,
      requestId,
      buffer);
  }

  BOOST_CHECK_EQUAL(providerHandlerCallCount, expectedCompletions);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), expectedCompletions);
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(),
                    expectedCompletions + expectedExpirations);
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
}

BOOST_AUTO_TEST_CASE(AllSelectedPublishesSelectionForEveryValidAckResponder)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-selection",
                                   "tpm-memory:all-selected-selection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  const std::vector<ndn::Name> providers{
    ndn::Name("/test/provider/a"),
    ndn::Name("/test/provider/b"),
    ndn::Name("/test/provider/c")
  };
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-selection"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ndn::Name publishedRequestId;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t strategy) {
      publishedRequestId = requestId;
      BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);

      auto ackA = makeSuccessAckForRequest(requestMessage, "provider-token-a");
      auto ackB = makeSuccessAckForRequest(requestMessage, "provider-token-b");
      RequestAckMessage rejectedAck;
      rejectedAck.setStatus(false);
      rejectedAck.setMessage("busy");
      rejectedAck.setUserToken(requestMessage.getUserToken());

      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[0], requesterName, serviceName, requestId), ackA));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[1], requesterName, serviceName, requestId), ackB));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[2], requesterName, serviceName, requestId), rejectedAck));
    });

  RequestMessage request;
  auto requestId = user.RequestService(
    providers,
    serviceName,
    request,
    1,
    ServiceUser::AckSelectionStrategy::AllSelected,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("AllSelected selection test should not time out");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  pumpFace(face, ndn::time::milliseconds(5));

  const auto selected = user.getExpectedResponseProviders(requestId);
  BOOST_REQUIRE_EQUAL(selected.size(), 2);
  BOOST_CHECK(namesContain(selected, providers[0]));
  BOOST_CHECK(namesContain(selected, providers[1]));
  BOOST_CHECK(!namesContain(selected, providers[2]));

  const auto selectedForPublication = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(selectedForPublication.size(), 2);
  BOOST_CHECK(namesContain(selectedForPublication, providers[0]));
  BOOST_CHECK(namesContain(selectedForPublication, providers[1]));
  BOOST_CHECK(!namesContain(selectedForPublication, providers[2]));
}

BOOST_AUTO_TEST_CASE(AllSelectedProvidersExecuteOnlyAfterSelection)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-provider-selection",
                                   "tpm-memory:all-selected-provider-selection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/a");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-all-selected-provider");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-provider"));
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

  int executions = 0;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        ++executions;
        response.setClassification(7);
      }));

  RequestMessage request = makeRequestMessageWithUserToken("hello");
  request.setStrategy(tlv::AllSelected);
  auto requestBlock = request.WireEncode();
  ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());

  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                ndn::Name("/bf"),
                                                requestId,
                                                requestBuffer);
  BOOST_CHECK_EQUAL(executions, 0);
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));

  auto selectionBuffer = makeSelectionBuffer(requestId, "provider-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(executions, 0);

  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         request,
                                         "provider-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(executions, 1);
}

BOOST_AUTO_TEST_CASE(AllSelectedHandlesMultipleSelectedProviderResponses)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-responses",
                                   "tpm-memory:all-selected-responses");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  const std::vector<ndn::Name> providers{
    ndn::Name("/test/provider/a"),
    ndn::Name("/test/provider/b")
  };
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-responses"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ndn::Name publishedRequestId;
  std::string userToken;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t) {
      publishedRequestId = requestId;
      userToken = requestMessage.getUserToken();
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[0], requesterName, serviceName, requestId),
        makeSuccessAckForRequest(requestMessage, "provider-token-a")));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[1], requesterName, serviceName, requestId),
        makeSuccessAckForRequest(requestMessage, "provider-token-b")));
    });

  int responseCallbacks = 0;
  RequestMessage request;
  auto requestId = user.RequestService(
    providers,
    serviceName,
    request,
    1,
    ServiceUser::AckSelectionStrategy::AllSelected,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("AllSelected response test should not time out");
    }),
    ServiceUser::ResponseHandler([&] (const ResponseMessage&) {
      ++responseCallbacks;
    }));
  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  pumpFace(face, ndn::time::milliseconds(5));

  ResponseMessage responseA;
  responseA.setStatus(true);
  responseA.setUserToken(userToken);
  ResponseMessage responseB;
  responseB.setStatus(true);
  responseB.setUserToken(userToken);

  const auto selected = user.getExpectedResponseProviders(requestId);
  BOOST_REQUIRE_EQUAL(selected.size(), 2);
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providers[0], requesterName, serviceName, requestId),
    responseA));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK(user.hasPendingCall(requestId));

  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providers[1], requesterName, serviceName, requestId),
    responseB));
  BOOST_CHECK_EQUAL(responseCallbacks, 2);
  BOOST_CHECK(!user.hasPendingCall(requestId));
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

BOOST_AUTO_TEST_CASE(SerializedWorkerQueueRunsTasksOffCallerThreadAndSerializes)
{
  SerializedWorkerQueue queue("unit-test serialized queue", 16);
  const auto callerThread = std::this_thread::get_id();
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::thread::id> workerThreads;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int completed = 0;

  for (int i = 0; i < 8; ++i) {
    BOOST_REQUIRE(queue.post([&] {
      const int nowActive = ++active;
      int observedMax = maxActive.load();
      while (nowActive > observedMax &&
             !maxActive.compare_exchange_weak(observedMax, nowActive)) {
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        workerThreads.push_back(std::this_thread::get_id());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      --active;

      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
      }
      cv.notify_one();
    }));
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return completed == 8;
    }));
  }

  BOOST_CHECK_EQUAL(maxActive.load(), 1);
  BOOST_REQUIRE(!workerThreads.empty());
  BOOST_CHECK(std::all_of(workerThreads.begin(), workerThreads.end(),
                          [&] (const std::thread::id& id) {
                            return id != callerThread;
                          }));
}

BOOST_AUTO_TEST_CASE(SerializedWorkerQueueShutdownDrainsPendingWorkAndRejectsNewWork)
{
  SerializedWorkerQueue queue("unit-test serialized shutdown", 8);
  std::atomic<int> completed{0};

  for (int i = 0; i < 5; ++i) {
    BOOST_REQUIRE(queue.post([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ++completed;
    }));
  }

  queue.shutdown();

  BOOST_CHECK_EQUAL(completed.load(), 5);
  BOOST_CHECK(!queue.post([] {}));
}

BOOST_AUTO_TEST_CASE(BoundedWorkerPoolRunsConcurrentlyAndRejectsWhenFull)
{
  BoundedWorkerPool pool("unit-test bounded pool", 8);
  pool.setThreadCount(2);
  const auto callerThread = std::this_thread::get_id();
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int started = 0;
  int completed = 0;

  for (int i = 0; i < 2; ++i) {
    BOOST_REQUIRE(pool.post([&] {
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++started;
      }
      cv.notify_one();

      const int nowActive = ++active;
      int observedMax = maxActive.load();
      while (nowActive > observedMax &&
             !maxActive.compare_exchange_weak(observedMax, nowActive)) {
      }
      BOOST_CHECK(std::this_thread::get_id() != callerThread);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      --active;

      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
      }
      cv.notify_one();
    }));
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
      return completed == 2;
    }));
  }
  BOOST_CHECK_GE(maxActive.load(), 2);
  pool.shutdown();

  BOOST_CHECK(!pool.post([] {}));

  BoundedWorkerPool fullPool("unit-test bounded full", 1);
  fullPool.setThreadCount(1);
  std::mutex gateMutex;
  std::condition_variable gateCv;
  bool firstStarted = false;
  bool releaseFirst = false;
  BOOST_REQUIRE(fullPool.post([&] {
    {
      std::lock_guard<std::mutex> lock(gateMutex);
      firstStarted = true;
    }
    gateCv.notify_one();
    std::unique_lock<std::mutex> lock(gateMutex);
    gateCv.wait(lock, [&] { return releaseFirst; });
  }));
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    BOOST_REQUIRE(gateCv.wait_for(lock, std::chrono::seconds(1), [&] {
      return firstStarted;
    }));
  }
  BOOST_CHECK(fullPool.post([] {}));
  BOOST_CHECK(!fullPool.post([] {}));
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseFirst = true;
  }
  gateCv.notify_one();
  fullPool.shutdown();
}

BOOST_AUTO_TEST_CASE(ProviderHandlerExecutionCanRunOffEventLoopAndInParallel)
{
  ndn::security::KeyChain keyChain("pib-memory:provider-handler-parallel",
                                   "tpm-memory:provider-handler-parallel");
  ndn::DummyClientFace face(keyChain);
  const auto eventLoopThread = std::this_thread::get_id();
  const ndn::Name requesterName("/test/user/parallel");
  const ndn::Name providerName("/test/provider/parallel");
  const ndn::Name serviceName("/HELLO");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-parallel"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.setHandlerThreads(2);
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int executed = 0;
  bool handlerOffEventLoop = false;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerOffEventLoop =
          handlerOffEventLoop || std::this_thread::get_id() != eventLoopThread;
        const int nowActive = ++active;
        int observedMax = maxActive.load();
        while (nowActive > observedMax &&
               !maxActive.compare_exchange_weak(observedMax, nowActive)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        response.setClassification(7);
        --active;
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++executed;
        }
        cv.notify_one();
      }));

  RequestMessage request = makeRequestMessageWithUserToken("hello");
  const std::vector<ndn::Name> requestIds{ndn::Name("/request-parallel-a"),
                                          ndn::Name("/request-parallel-b")};
  for (const auto& requestId : requestIds) {
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           requestId,
                                           request,
                                           "provider-token");
    auto selectionBuffer = makeSelectionBuffer(requestId, "provider-token");
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                    providerName,
                                                                    serviceName,
                                                                    requestId,
                                                                    selectionBuffer);
  }

  for (int i = 0; i < 20 && executed < 2; ++i) {
    pumpFace(face, ndn::time::milliseconds(10));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return executed == 2;
    }));
  }
  pumpFace(face, ndn::time::milliseconds(50));

  BOOST_CHECK(handlerOffEventLoop);
  BOOST_CHECK_GE(maxActive.load(), 2);
  for (const auto& requestId : requestIds) {
    auto status = provider.getProviderRequestStatus(requestId);
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(status->state ==
                ServiceProvider::ProviderRequestLifecycleState::RESPONSE_PUBLISHED);
  }
}

BOOST_AUTO_TEST_CASE(UserResponseCallbackCanRunOffEventLoopAfterStateUpdate)
{
  ndn::security::KeyChain keyChain("pib-memory:user-callback-parallel",
                                   "tpm-memory:user-callback-parallel");
  ndn::DummyClientFace face(keyChain);
  const auto eventLoopThread = std::this_thread::get_id();
  const ndn::Name requesterName("/test/user/callback");
  const ndn::Name providerName("/test/provider/callback");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-callback");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-user-callback"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  user.setHandlerThreads(1);

  bool callbackOffEventLoop = false;
  bool pendingGoneBeforeCallback = false;
  std::mutex mutex;
  std::condition_variable cv;
  bool callbackDone = false;

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  user.setPendingResponseHandlerForTest(requestId, [&] (const ResponseMessage&) {
    callbackOffEventLoop = std::this_thread::get_id() != eventLoopThread;
    pendingGoneBeforeCallback = !user.hasPendingCall(requestId);
    {
      std::lock_guard<std::mutex> lock(mutex);
      callbackDone = true;
    }
    cv.notify_one();
  });

  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token");
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providerName, requesterName, serviceName, requestId),
    response));

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return callbackDone;
    }));
  }

  BOOST_CHECK(callbackOffEventLoop);
  BOOST_CHECK(pendingGoneBeforeCallback);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
