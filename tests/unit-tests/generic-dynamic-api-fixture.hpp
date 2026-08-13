#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
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

inline SelectionInputKeyOffer
makeSelectionInputKeyOffer(const ndn::security::Certificate& certificate,
                           const std::string& bootEpoch = "boot-1")
{
  const auto publicKey = certificate.getPublicKey();
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(publicKey.data()),
                        publicKey.size());
  SelectionInputKeyOffer offer;
  offer.setField("recipient", certificate.getIdentity().toUri());
  offer.setField("recipientCertName", certificate.getName().toUri());
  offer.setField("recipientPublicKey", selectionGatedHex(publicKey));
  offer.setField("recipientCertDigest", "sha256:" + digest.toString());
  offer.setField("providerBootEpoch", bootEpoch);
  return offer;
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

  static SelectionExecutionStatus
  parseSelectionStatusForTest(const ndn::Data& data,
                              const ndn::Name& provider,
                              const std::string& digest)
  {
    return parseSelectionExecutionStatusPayload(data, provider, digest);
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

  void
  deliverPlaintextAckPublicationForTest(
      const ndn::Name& providerName,
      const ndn::Name& serviceName,
      const ndn::Name& requestId,
      const RequestAckMessage& ackMessage)
  {
    const auto ackName = makeRequestAckNameV2(providerName, identity,
                                               serviceName, requestId);
    const auto ackBlock = ackMessage.WireEncode();
    const ndn::Buffer ackBuffer(ackBlock.data(), ackBlock.size());
    const std::optional<ndn::Data> packet;
    const ndn::Name producerPrefix = ndn::Name(providerName).append("1");
    const ndn::svs::SVSPubSub::SubscriptionData publication{
      ackName,
      ndn::span<const uint8_t>(ackBuffer.data(), ackBuffer.size()),
      producerPrefix,
      1,
      packet};
    OnRequestAck(publication);
  }

  std::map<std::string, std::string>
  getSelectionDigestsByProvider(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return {};
    }
    return pending->second.selectionDigestsByProvider;
  }

  ndn::Buffer
  getCollaborationAssignmentForTest(const ndn::Name& requestId,
                                    const ndn::Name& providerName) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return {};
    }
    const auto assignment = pending->second.collaborationAssignments.find(
      providerName.toUri());
    if (assignment == pending->second.collaborationAssignments.end()) {
      return {};
    }
    return assignment->second;
  }

  bool
  hasPendingCall(const ndn::Name& requestId) const
  {
    return m_pendingCalls.find(requestId) != m_pendingCalls.end();
  }

  void
  prepareDeferredCollaborationForTest(
      const ndn::Name& requestId,
      CollaborationAckClosedHandler onAckClosed,
      int ackTimeoutMs = 100,
      int timeoutMs = 1000)
  {
    PendingCall call;
    call.serviceName = ndn::Name("/generic/work");
    call.requestMessage.setUserToken("user-token");
    call.timeoutMs = timeoutMs;
    call.ackTimeoutMs = ackTimeoutMs;
    call.createdAtUs = 1;
    call.requestDeadlineUs = std::numeric_limits<uint64_t>::max();
    call.ackWindowExpired = true;
    call.isCollaboration = true;
    call.collaborationDeferred = true;
    call.collaborationPlan.ackCollectionTimeMs = ackTimeoutMs;
    call.collaborationPlan.timeoutMs = timeoutMs;
    call.collaborationAckClosedHandler = std::move(onAckClosed);
    m_pendingCalls[requestId] = std::move(call);
  }

  void
  addDeferredAckForTest(const ndn::Name& requestId,
                        const ndn::Name& provider,
                        const std::string& role)
  {
    auto pending = m_pendingCalls.find(requestId);
    BOOST_REQUIRE(pending != m_pendingCalls.end());
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("willing");
    ack.setUserToken("user-token");
    ack.setProviderToken("provider-token");
    std::string text = "role=" + role + ";";
    ndn::Buffer payload(
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
    ack.setPayload(payload, payload.size());
    pending->second.requestAcks.push_back({
      provider, pending->second.serviceName, requestId, std::move(ack)});
    pending->second.providerTokens[provider.toUri()] = "provider-token";
  }

  bool
  closeDeferredAcksForTest(const ndn::Name& requestId)
  {
    auto pending = m_pendingCalls.find(requestId);
    BOOST_REQUIRE(pending != m_pendingCalls.end());
    return closeDeferredCollaborationAcks(requestId, pending->second);
  }

  bool
  tracksAckDecryptForTest(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    BOOST_REQUIRE(pending != m_pendingCalls.end());
    return shouldTrackAckDecrypt(pending->second);
  }

  ndn::Buffer
  getSelectionAssignmentPayloadForTest(const ndn::Name& requestId,
                                       const ndn::Name& providerName) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return ndn::Buffer();
    }
    const auto assignment =
      pending->second.selectionAssignmentPayloads.find(providerName.toUri());
    if (assignment == pending->second.selectionAssignmentPayloads.end()) {
      return ndn::Buffer();
    }
    return assignment->second;
  }

  bool
  isAckWindowExpired(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending != m_pendingCalls.end() && pending->second.ackWindowExpired;
  }

  size_t
  getR1DecisionTransmissionCount(const ndn::Name& requestId,
                                 const std::string& reservationId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) return 0;
    const auto delivery = pending->second.r1DecisionDeliveries.find(reservationId);
    return delivery == pending->second.r1DecisionDeliveries.end() ? 0 :
                                                                    delivery->second.transmissions;
  }

  std::string
  getR1DecisionDigestForTest(const ndn::Name& requestId,
                             const std::string& reservationId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) return {};
    const auto delivery = pending->second.r1DecisionDeliveries.find(reservationId);
    return delivery == pending->second.r1DecisionDeliveries.end() ? std::string() :
                                                                    delivery->second.decisionDigest;
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
  addPendingCollaborationCallForTest(
    const ndn::Name& requestId,
    const std::vector<ndn::Name>& participants,
    ResponseHandler responseHandler)
  {
    PendingCall pendingCall;
    pendingCall.isCollaboration = true;
    pendingCall.expectedResponseProviders = participants;
    pendingCall.responseHandler = std::move(responseHandler);
    m_pendingCalls[requestId] = std::move(pendingCall);
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
    pendingCall.targetedMode = true;
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

  static std::string
  encodeSelectionStatusForTest(const SelectionExecutionStatus& status)
  {
    return encodeSelectionExecutionStatus(status);
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
  addPendingR1RequestForTokenTest(const ndn::Name& requesterName,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId,
                                  const RequestMessage& requestMessage,
                                  const std::string& providerToken,
                                  const ReservationLease& lease)
  {
    addPendingRequestForTokenTest(requesterName, serviceName, requestId,
                                  requestMessage, providerToken);
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
    pendingReservationLeases[key] = lease;
  }

  bool
  hasAcceptedR1DecisionForTest(const std::string& reservationId) const
  {
    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
    return m_r1AcceptedSelectionDecisions.find(reservationId) !=
           m_r1AcceptedSelectionDecisions.end();
  }

  ndn::Buffer
  getDecisionReceiptForTest(const std::string& selectionDigest) const
  {
    const auto found = m_selectionExecutionStatuses.find(selectionDigest);
    return found == m_selectionExecutionStatuses.end() ? ndn::Buffer() :
                                                         found->second.decisionReceipt;
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

  void
  schedulePendingRequestCleanupForTest(
      const ndn::Name& requesterName,
      const ndn::Name& serviceName,
      const ndn::Name& requestId,
      ndn::time::milliseconds ttl = ndn::time::seconds(30),
      bool authoritative = false)
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    schedulePendingRequestCleanup(key, ttl, authoritative);
  }

  uint64_t
  pendingCleanupExpiryUnixMsForTest(
      const ndn::Name& requesterName,
      const ndn::Name& serviceName,
      const ndn::Name& requestId) const
  {
    ndn::Name key(requesterName);
    key.append(serviceName).append(requestId);
    std::lock_guard<std::mutex> lock(m_pendingCleanupDeadlineMutex);
    const auto found = m_pendingCleanupExpiryUnixMs.find(key);
    return found == m_pendingCleanupExpiryUnixMs.end() ? 0 : found->second;
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
  seedSelectionStatusForTest(const std::string& selectionDigest,
                             const ndn::Name& serviceName,
                             const ndn::Name& requestId)
  {
    updateSelectionExecutionStatus(selectionDigest,
                                   SelectionExecutionState::Received,
                                   identity,
                                   serviceName,
                                   requestId,
                                   "selection received");
  }

  void
  failCollaborationForTest(const std::string& selectionDigest,
                           const ndn::Name& serviceName,
                           const ndn::Name& requestId,
                           const std::string& reason)
  {
    CollaborationAssignment assignment;
    assignment.role = "worker";
    assignment.service = serviceName;
    assignment.selectionDigest = selectionDigest;
    RequestMessage request;
    CollaborationContext ctx(*this,
                             ndn::Name("/user/test"),
                             requestId,
                             std::move(request),
                             std::move(assignment));
    ctx.fail(reason);
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
                       const ndn::Name& serviceName,
                       size_t policyEpoch = 1)
{
  PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(policyEpoch);

  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.setPolicyEpoch(policyEpoch);
  response.addEntry(entry);
  return response;
}

void
installUserPermissions(LocalServiceUser& user,
                       const ndn::Name& requesterName,
                       const ndn::Name& serviceName,
                       const std::vector<ndn::Name>& providerNames,
                       size_t policyEpoch = 1)
{
  PermissionResponse response;
  response.setTargetIdentity(requesterName.toUri());
  response.setPermissionKind(tlv::UserPermission);
  response.setPolicyEpoch(policyEpoch);
  for (const auto& providerName : providerNames) {
    PermissionEntry entry;
    entry.setProviderName(providerName.toUri());
    entry.setServiceName(serviceName.toUri());
    entry.setToken("");
    entry.setTtl(0);
    entry.setVersion(policyEpoch);
    response.addEntry(entry);
  }
  user.applyPermissionResponse(response);
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
makeSelectionBuffer(const ndn::Name& requestId,
                    const std::string& providerToken,
                    const ndn::Buffer& assignmentPayload = ndn::Buffer())
{
  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setProviderToken(providerToken);
  if (!assignmentPayload.empty()) {
    selection.setAssignmentPayload(assignmentPayload);
  }
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
