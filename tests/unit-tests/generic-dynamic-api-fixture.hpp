#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>
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

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

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

std::string
makeProviderTokenHashForTest(const ndn::Name& requesterName,
                             const ndn::Name& serviceName,
                             const std::string& providerToken)
{
  ndn::util::Sha256 digest;
  digest << "TARGETED";
  digest << requesterName.toUri();
  digest << serviceName.toUri();
  digest << providerToken;
  return digest.toString();
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
  addTargetedPendingCallForTokenTest(const ndn::Name& requestId,
                                     const ndn::Name& serviceName,
                                     const ndn::Name& providerName,
                                     const std::string& userToken)
  {
    PendingCall pendingCall;
    pendingCall.providers = {providerName};
    pendingCall.serviceName = serviceName;
    pendingCall.strategy = tlv::FirstResponding;
    pendingCall.requestMessage.setUserToken(userToken);
    pendingCall.requestMessage.setRequestMode(tlv::TargetedRequest);
    pendingCall.requestMessage.setTargetProvider(providerName);
    pendingCall.directMode = true;
    pendingCall.expectedResponseProviders.push_back(providerName);
    m_pendingCalls[requestId] = pendingCall;
  }

  void
  addTargetedTokenPairForTest(const ndn::Name& providerName,
                              const ndn::Name& serviceName,
                              const std::string& providerToken,
                              const std::string& userToken)
  {
    m_targetedTokenPools[
      makeTargetedTokenPoolKey(providerName, serviceName)].push_back(
        TargetedTokenPair{providerToken, userToken});
  }

  size_t
  getTargetedTokenPoolSizeForTest(const ndn::Name& providerName,
                                  const ndn::Name& serviceName) const
  {
    const auto poolIt =
      m_targetedTokenPools.find(makeTargetedTokenPoolKey(providerName, serviceName));
    if (poolIt == m_targetedTokenPools.end()) {
      return 0;
    }
    return poolIt->second.size();
  }

  void
  setPendingResponseHandlerForTest(const ndn::Name& requestId,
                                   ResponseHandler responseHandler)
  {
    m_pendingCalls[requestId].responseHandler = std::move(responseHandler);
  }

  void
  setPendingAckCandidatesHandlerForTest(const ndn::Name& requestId,
                                        AckCandidatesHandler ackCandidatesHandler)
  {
    m_pendingCalls[requestId].ackCandidatesHandler = std::move(ackCandidatesHandler);
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
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
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
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
    return pendingRequests.find(key) != pendingRequests.end();
  }

  bool
  hasPendingProviderTokenForTest(const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId) const
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
    return pendingProviderTokens.find(key) != pendingProviderTokens.end();
  }

  bool
  replySelectionStatusForTest(const ndn::Interest& interest)
  {
    return replySelectionExecutionStatus(interest);
  }

  void
  addTargetedProviderTokenForTest(const ndn::Name& requesterName,
                                  const ndn::Name& serviceName,
                                  const std::string& providerToken,
                                  const std::string& userToken)
  {
    const auto tokenHash =
      makeProviderTokenHashForTest(requesterName, serviceName, providerToken);
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
    m_targetedProviderTokens[tokenHash] =
      TargetedProviderTokenState{requesterName, serviceName, userToken};
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

} // namespace ndn_service_framework::test

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
