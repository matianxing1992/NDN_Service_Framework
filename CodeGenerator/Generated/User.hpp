// User(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);

// std::vector<ndn::Name> getUserPermissions();

//using AcksHandler = std::function<std::vector<ndn_service_framework::AckMessage>(const std::vector<ndn_service_framework::AckMessage>&)>;
//using ResponseHandler = std::function<void(const ndn_service_framework::ResponseMessage&)>;
//using TimeoutHandler = std::function<void(const ndn::Name&)>;

// default
// std::Name async_call(ndn::Name serviceName, ndn_service_framework::RequestMessage requestMessage, ndn_service_framework::SelectionStrategy selectionStrategy, int timeout_ms, TimeoutHandler onTimeout, ResponseHandler onResponseHandler);

// custom
// std::Name async_call(ndn::Name serviceName,ndn_service_framework::RequestMessage requestMessage, int ack_timeout_ms, AcksHandler onAcksHandler, int timeout_ms, TimeoutHandler onTimeout, ResponseHandler onResponseHandler);


#ifndef NDNSF_USER_HPP
#define NDNSF_USER_HPP

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/util/time.hpp>

#include <ndn-service-framework/BloomFilter.hpp>
#include <ndn-service-framework/NDNSFMessages.hpp>
#include <ndn-service-framework/utils.hpp>

#include "PublishMessageBridge.hpp"

namespace muas {

class User
{
public:
  using AcksHandler =
      std::function<std::vector<ndn_service_framework::RequestAckMessage>(
          const std::vector<ndn_service_framework::RequestAckMessage>&)>;

  using ResponseHandler =
      std::function<void(const ndn_service_framework::ResponseMessage&)>;

  using TimeoutHandler =
      std::function<void(const ndn::Name&)>;

  using RequestPublisher =
      std::function<void(const ndn::Name& requestId,
                         const ndn::Name& requestName,
                         const std::vector<ndn::Name>& providers,
                         const ndn::Name& serviceName,
                         const ndn_service_framework::RequestMessage& requestMessage,
                         size_t strategy)>;

private:
  struct StoredAck
  {
    ndn::Name providerName;
    ndn::Name serviceName;
    ndn::Name requestId;
    ndn_service_framework::RequestAckMessage message;
  };

  struct PendingCall
  {
    std::vector<ndn::Name> providers;
    ndn::Name serviceName;
    ndn::Name requestName;
    ndn::Name requestNameWithoutPrefix;
    ndn_service_framework::RequestMessage requestMessage;
    size_t strategy = ndn_service_framework::tlv::FirstResponding;
    int timeoutMs = 0;
    int ackTimeoutMs = 0;
    AcksHandler acksHandler;
    TimeoutHandler timeoutHandler;
    ResponseHandler responseHandler;
    std::vector<StoredAck> requestAcks;
    std::vector<StoredAck> customSelectedAcks;
    std::vector<ndn::Name> successfulAckProviders;
    ndn::Name selectedProvider;
  };

public:
  User(ndn::Face& face,
       ndn::Name groupPrefix,
       ndn::security::Certificate identityCert,
       ndn::security::Certificate attrAuthorityCertificate,
       std::string trustSchemaPath)
    : m_face(face)
    , m_groupPrefix(std::move(groupPrefix))
    , m_identityCert(std::move(identityCert))
    , m_attrAuthorityCertificate(std::move(attrAuthorityCertificate))
    , m_trustSchemaPath(std::move(trustSchemaPath))
  {
  }

  std::vector<ndn::Name>
  getUserPermissions() const
  {
    return {};
  }

  void
  setRequestPublisher(RequestPublisher publisher)
  {
    m_requestPublisher = std::move(publisher);
  }

  void
  setPublishMessageBridgeForRequests(PublishMessageBridge& bridge)
  {
    setRequestPublisher([this, &bridge](const ndn::Name& requestId,
                                        const ndn::Name& requestName,
                                        const std::vector<ndn::Name>&,
                                        const ndn::Name&,
                                        const ndn_service_framework::RequestMessage& requestMessage,
                                        size_t) {
      const auto* requestNameWithoutPrefix = getPendingRequestNameWithoutPrefix(requestId);
      if (requestNameWithoutPrefix == nullptr) {
        return;
      }

      bridge.publish(requestName, *requestNameWithoutPrefix, requestMessage);
    });
  }

  ndn::Name
  async_call(const std::vector<ndn::Name>& providers,
             const ndn::Name& serviceName,
             ndn_service_framework::RequestMessage requestMessage,
             int timeoutMs,
             TimeoutHandler onTimeout,
             ResponseHandler onResponseHandler,
             size_t strategy = ndn_service_framework::tlv::FirstResponding)
  {
    const ndn::Name requestId = makeRequestId();
    PendingCall pendingCall;
    pendingCall.providers = providers;
    pendingCall.serviceName = serviceName;
    pendingCall.requestName = makeRequestName(serviceName, providers, requestId);
    pendingCall.requestNameWithoutPrefix = makeRequestNameWithoutPrefix(serviceName,
                                                                        providers,
                                                                        requestId);
    pendingCall.requestMessage = std::move(requestMessage);
    pendingCall.strategy = strategy;
    pendingCall.timeoutMs = timeoutMs;
    pendingCall.timeoutHandler = std::move(onTimeout);
    pendingCall.responseHandler = std::move(onResponseHandler);
    m_pendingCalls[requestId] = std::move(pendingCall);

    publishRequestIfConfigured(requestId, m_pendingCalls.at(requestId));
    return requestId;
  }

  ndn::Name
  async_call(const std::vector<ndn::Name>& providers,
             const ndn::Name& serviceName,
             const ndn::Name& functionName,
             ndn_service_framework::RequestMessage requestMessage,
             int timeoutMs,
             TimeoutHandler onTimeout,
             ResponseHandler onResponseHandler,
             size_t strategy = ndn_service_framework::tlv::FirstResponding)
  {
    return async_call(providers,
                      makeUnifiedServiceName(serviceName, functionName),
                      std::move(requestMessage),
                      timeoutMs,
                      std::move(onTimeout),
                      std::move(onResponseHandler),
                      strategy);
  }

  ndn::Name
  async_call(ndn::Name serviceName,
             ndn_service_framework::RequestMessage requestMessage,
             int timeout_ms,
             TimeoutHandler onTimeout,
             ResponseHandler onResponseHandler,
             size_t strategy = ndn_service_framework::tlv::FirstResponding)
  {
    return async_call({},
                      serviceName,
                      std::move(requestMessage),
                      timeout_ms,
                      std::move(onTimeout),
                      std::move(onResponseHandler),
                      strategy);
  }

  ndn::Name
  async_call(ndn::Name serviceName,
             ndn_service_framework::RequestMessage requestMessage,
             int ack_timeout_ms,
             AcksHandler onAcksHandler,
             int timeout_ms,
             TimeoutHandler onTimeout,
             ResponseHandler onResponseHandler)
  {
    const ndn::Name requestId = makeRequestId();
    PendingCall pendingCall;
    pendingCall.serviceName = serviceName;
    pendingCall.requestName = makeRequestName(serviceName, pendingCall.providers, requestId);
    pendingCall.requestNameWithoutPrefix = makeRequestNameWithoutPrefix(serviceName,
                                                                        pendingCall.providers,
                                                                        requestId);
    pendingCall.requestMessage = std::move(requestMessage);
    pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
    pendingCall.timeoutMs = timeout_ms;
    pendingCall.ackTimeoutMs = ack_timeout_ms;
    pendingCall.acksHandler = std::move(onAcksHandler);
    pendingCall.timeoutHandler = std::move(onTimeout);
    pendingCall.responseHandler = std::move(onResponseHandler);
    m_pendingCalls[requestId] = std::move(pendingCall);

    publishRequestIfConfigured(requestId, m_pendingCalls.at(requestId));
    return requestId;
  }

  template<typename RequestT, typename ResponseT>
  ndn::Name
  asyncCall(const std::vector<ndn::Name>& providers,
            const ndn::Name& serviceName,
            const RequestT& request,
            std::function<void(const ResponseT&)> onResponse,
            std::function<void()> onTimeout,
            int timeoutMs,
            size_t strategy)
  {
    std::string requestBytes;
    if (!request.SerializeToString(&requestBytes)) {
      return ndn::Name();
    }

    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(requestBytes.data()),
                        requestBytes.size());

    ndn_service_framework::RequestMessage requestMessage;
    requestMessage.setPayload(payload, payload.size());
    requestMessage.setStrategy(strategy);

    return async_call(providers,
                      serviceName,
                      std::move(requestMessage),
                      timeoutMs,
                      [timeout = std::move(onTimeout)](const ndn::Name&) {
                        if (timeout) {
                          timeout();
                        }
                      },
                      [response = std::move(onResponse)](
                          const ndn_service_framework::ResponseMessage& responseMessage) {
                        const auto payload = responseMessage.getPayload();

                        ResponseT typedResponse;
                        if (!typedResponse.ParseFromArray(payload.data(), payload.size())) {
                          return;
                        }

                        if (response) {
                          response(typedResponse);
                        }
                      },
                      strategy);
  }

  template<typename RequestT, typename ResponseT>
  ndn::Name
  asyncCall(const std::vector<ndn::Name>& providers,
            const ndn::Name& serviceName,
            const ndn::Name& functionName,
            const RequestT& request,
            std::function<void(const ResponseT&)> onResponse,
            std::function<void()> onTimeout,
            int timeoutMs,
            size_t strategy)
  {
    return asyncCall<RequestT, ResponseT>(providers,
                                          makeUnifiedServiceName(serviceName, functionName),
                                          request,
                                          std::move(onResponse),
                                          std::move(onTimeout),
                                          timeoutMs,
                                          strategy);
  }

  void
  handleResponse(const ndn::Name& requestId,
                 const ndn_service_framework::ResponseMessage& responseMessage)
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return;
    }

    if (pendingCall->second.responseHandler) {
      pendingCall->second.responseHandler(responseMessage);
    }

    m_pendingCalls.erase(pendingCall);
  }

  bool
  handleDecryptedResponse(const ndn::Name& requestId,
                          const ndn_service_framework::ResponseMessage& responseMessage)
  {
    if (m_pendingCalls.find(requestId) == m_pendingCalls.end()) {
      return false;
    }

    handleResponse(requestId, responseMessage);
    return true;
  }

  bool
  handleDecryptedResponse(const ndn::Name& requestId,
                          const ndn::Block& responseBlock)
  {
    ndn_service_framework::ResponseMessage responseMessage;
    if (!responseMessage.WireDecode(responseBlock)) {
      return false;
    }

    return handleDecryptedResponse(requestId, responseMessage);
  }

  bool
  handleDecryptedResponseByName(
      const ndn::Name& responseName,
      const ndn_service_framework::ResponseMessage& responseMessage)
  {
    auto parsed = ndn_service_framework::parseResponseName(responseName);
    if (!parsed) {
      return false;
    }

    ndn::Name requesterIdentity;
    ndn::Name providerName;
    ndn::Name serviceName;
    ndn::Name functionName;
    ndn::Name requestId;
    std::tie(requesterIdentity, providerName, serviceName, functionName, requestId) =
        parsed.value();

    return handleDecryptedResponse(requestId, responseMessage);
  }

  bool
  handleDecryptedResponseByName(const ndn::Name& responseName,
                                const ndn::Block& responseBlock)
  {
    ndn_service_framework::ResponseMessage responseMessage;
    if (!responseMessage.WireDecode(responseBlock)) {
      return false;
    }

    return handleDecryptedResponseByName(responseName, responseMessage);
  }

  bool
  handleRequestAckByName(const ndn::Name& ackName,
                         const ndn_service_framework::RequestAckMessage& ackMessage)
  {
    auto parsed = ndn_service_framework::parseRequestAckName(ackName);
    if (!parsed) {
      return false;
    }

    ndn::Name providerName;
    ndn::Name requesterName;
    ndn::Name serviceName;
    ndn::Name functionName;
    ndn::Name requestId;
    std::tie(providerName, requesterName, serviceName, functionName, requestId) =
        parsed.value();

    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return false;
    }

    pendingCall->second.requestAcks.push_back(
        StoredAck{providerName,
                  makeUnifiedServiceName(serviceName, functionName),
                  requestId,
                  ackMessage});
    evaluateAckSelection(requestId);

    return true;
  }

  bool
  handleRequestAckByName(const ndn::Name& ackName,
                         const ndn::Block& ackBlock)
  {
    ndn_service_framework::RequestAckMessage ackMessage;
    if (!ackMessage.WireDecode(ackBlock)) {
      return false;
    }

    return handleRequestAckByName(ackName, ackMessage);
  }

  const ndn_service_framework::RequestMessage*
  getPendingRequestMessage(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return nullptr;
    }

    return &pendingCall->second.requestMessage;
  }

  const ndn::Name*
  getPendingRequestName(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return nullptr;
    }

    return &pendingCall->second.requestName;
  }

  const ndn::Name*
  getPendingRequestNameWithoutPrefix(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return nullptr;
    }

    return &pendingCall->second.requestNameWithoutPrefix;
  }

  size_t
  getPendingRequestAckCount(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return 0;
    }

    return pendingCall->second.requestAcks.size();
  }

  std::vector<ndn::Name>
  getSuccessfulAckProviders(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return {};
    }

    return pendingCall->second.successfulAckProviders;
  }

  const ndn::Name*
  getSelectedProvider(const ndn::Name& requestId) const
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end() || pendingCall->second.selectedProvider.empty()) {
      return nullptr;
    }

    return &pendingCall->second.selectedProvider;
  }

  bool
  evaluateAckSelection(const ndn::Name& requestId)
  {
    auto pendingCall = m_pendingCalls.find(requestId);
    if (pendingCall == m_pendingCalls.end()) {
      return false;
    }

    if (pendingCall->second.acksHandler) {
      return evaluateCustomAckSelection(pendingCall->second);
    }

    return evaluateBuiltInAckSelection(pendingCall->second);
  }

private:
  static ndn::Name
  makeRequestId()
  {
    return ndn::Name(ndn::time::toIsoString(ndn::time::system_clock::now()));
  }

  void
  publishRequestIfConfigured(const ndn::Name& requestId,
                             const PendingCall& pendingCall) const
  {
    if (m_requestPublisher) {
      m_requestPublisher(requestId,
                         pendingCall.requestName,
                         pendingCall.providers,
                         pendingCall.serviceName,
                         pendingCall.requestMessage,
                         pendingCall.strategy);
    }
  }

  static ndn::Name
  makeUnifiedServiceName(const ndn::Name& serviceName,
                         const ndn::Name& functionName)
  {
    if (functionName.empty()) {
      return serviceName;
    }

    ndn::Name unified(serviceName);
    for (const auto& component : functionName) {
      unified.append(component);
    }
    return unified;
  }

  static bool
  containsName(const std::vector<ndn::Name>& names,
               const ndn::Name& name)
  {
    for (const auto& item : names) {
      if (item.equals(name)) {
        return true;
      }
    }
    return false;
  }

  static void
  addUniqueName(std::vector<ndn::Name>& names,
                const ndn::Name& name)
  {
    if (!name.empty() && !containsName(names, name)) {
      names.push_back(name);
    }
  }

  bool
  evaluateCustomAckSelection(PendingCall& pendingCall)
  {
    std::vector<ndn_service_framework::RequestAckMessage> ackMessages;
    for (const auto& storedAck : pendingCall.requestAcks) {
      ackMessages.push_back(storedAck.message);
    }

    const auto selectedMessages = pendingCall.acksHandler(ackMessages);
    pendingCall.customSelectedAcks.clear();
    pendingCall.successfulAckProviders.clear();
    pendingCall.selectedProvider = ndn::Name();

    for (const auto& selectedMessage : selectedMessages) {
      const auto* storedAck = findStoredAck(pendingCall, selectedMessage);
      if (storedAck == nullptr || !storedAck->message.getStatus()) {
        continue;
      }

      pendingCall.customSelectedAcks.push_back(*storedAck);
      addUniqueName(pendingCall.successfulAckProviders, storedAck->providerName);
      if (pendingCall.selectedProvider.empty()) {
        pendingCall.selectedProvider = storedAck->providerName;
      }
    }

    return true;
  }

  bool
  evaluateBuiltInAckSelection(PendingCall& pendingCall)
  {
    pendingCall.successfulAckProviders.clear();
    for (const auto& storedAck : pendingCall.requestAcks) {
      if (storedAck.message.getStatus()) {
        addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
      }
    }

    if (pendingCall.strategy == ndn_service_framework::tlv::FirstResponding) {
      if (pendingCall.selectedProvider.empty() && !pendingCall.successfulAckProviders.empty()) {
        pendingCall.selectedProvider = pendingCall.successfulAckProviders.front();
      }
      return true;
    }

    if (pendingCall.strategy == ndn_service_framework::tlv::LoadBalancing) {
      pendingCall.selectedProvider = selectLoadBalancingProvider(pendingCall.successfulAckProviders);
      return true;
    }

    if (pendingCall.strategy == ndn_service_framework::tlv::NoCoordination) {
      pendingCall.selectedProvider = ndn::Name();
      return true;
    }

    return false;
  }

  static ndn::Name
  selectLoadBalancingProvider(const std::vector<ndn::Name>& providers)
  {
    if (providers.empty()) {
      return ndn::Name();
    }

    ndn::Name selected = providers.front();
    for (const auto& provider : providers) {
      if (provider.toUri() < selected.toUri()) {
        selected = provider;
      }
    }
    return selected;
  }

  static const StoredAck*
  findStoredAck(const PendingCall& pendingCall,
                const ndn_service_framework::RequestAckMessage& ackMessage)
  {
    for (const auto& storedAck : pendingCall.requestAcks) {
      if (storedAck.message.getStatus() == ackMessage.getStatus() &&
          storedAck.message.getMessage() == ackMessage.getMessage()) {
        return &storedAck;
      }
    }
    return nullptr;
  }

  ndn::Name
  makeRequestName(const ndn::Name& serviceName,
                  const std::vector<ndn::Name>& providers,
                  const ndn::Name& requestId) const
  {
    return ndn_service_framework::makeRequestName(m_identityCert.getIdentity(),
                                                  serviceName,
                                                  ndn::Name(),
                                                  makeBloomFilterName(providers),
                                                  requestId);
  }

  ndn::Name
  makeRequestNameWithoutPrefix(const ndn::Name& serviceName,
                               const std::vector<ndn::Name>& providers,
                               const ndn::Name& requestId) const
  {
    return ndn_service_framework::makeRequestNameWithoutPrefix(serviceName,
                                                               ndn::Name(),
                                                               makeBloomFilterName(providers),
                                                               requestId);
  }

  static ndn::Name
  makeBloomFilterName(const std::vector<ndn::Name>& providers)
  {
    ndn_service_framework::BloomFilter bloomFilter;
    for (const auto& provider : providers) {
      bloomFilter.insert(provider.toUri());
    }
    return ndn::Name(bloomFilter.toHexString());
  }

private:
  ndn::Face& m_face;

  ndn::Name m_groupPrefix;

  ndn::security::Certificate m_identityCert;
  ndn::security::Certificate m_attrAuthorityCertificate;

  std::string m_trustSchemaPath;

  std::map<ndn::Name, PendingCall> m_pendingCalls;
  RequestPublisher m_requestPublisher;
};

} // namespace muas

#endif // NDNSF_USER_HPP
