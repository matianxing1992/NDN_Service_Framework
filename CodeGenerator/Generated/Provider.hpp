// Provider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath);

// std::vector<ndn::Name> getProviderPermissions();

// using AckStrategyHandler = std::function<std::pair<bool, ndn::Block>(const ndnsf::RequestAckMessage&)>;
// using RequestHandler = std::function<ndnsf::ResponseMessage(const ndnsf::RequestMessage&)>;}
// addService(ndn::Name serviceName, AckStrategyHandler handler, RequestHandler)


#ifndef NDNSF_PROVIDER_HPP
#define NDNSF_PROVIDER_HPP

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/security/certificate.hpp>

#include <ndn-service-framework/NDNSFMessages.hpp>
#include <ndn-service-framework/utils.hpp>

#include "PublishMessageBridge.hpp"

namespace muas {

class Provider
{
public:
  using ServiceKey = ndn::Name;

  using AckStrategyHandler =
      std::function<std::pair<bool, ndn::Block>(const ndn_service_framework::RequestAckMessage&)>;

  using RequestHandler =
      std::function<ndn_service_framework::ResponseMessage(
          const ndn::Name& requesterIdentity,
          const ndn::Name& providerName,
          const ndn::Name& serviceName,
          const ndn::Name& requestId,
          const ndn_service_framework::RequestMessage& requestMessage)>;

  using SimpleRequestHandler =
      std::function<ndn_service_framework::ResponseMessage(
          const ndn_service_framework::RequestMessage& requestMessage)>;

  using ResponsePublisher =
      std::function<void(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& requestId,
                         const ndn::Name& responseName,
                         const ndn_service_framework::ResponseMessage& response)>;

  using RequestAckPublisher =
      std::function<void(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& requestId,
                         const ndn::Name& ackName,
                         const ndn_service_framework::RequestAckMessage& ack)>;

private:
  struct RegisteredService
  {
    AckStrategyHandler ackHandler;
    RequestHandler requestHandler;
  };

  struct ParsedRequestName
  {
    ndn::Name requesterIdentity;
    ndn::Name serviceName;
    ndn::Name requestId;
  };

public:
  Provider(ndn::Face& face,
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
  getProviderPermissions() const
  {
    return {};
  }

  void
  setResponsePublisher(ResponsePublisher publisher)
  {
    m_responsePublisher = std::move(publisher);
  }

  void
  setRequestAckPublisher(RequestAckPublisher publisher)
  {
    m_requestAckPublisher = std::move(publisher);
  }

  void
  setPublishMessageBridgeForRequestAcks(PublishMessageBridge& bridge)
  {
    setRequestAckPublisher([&bridge](const ndn::Name& requesterIdentity,
                                     const ndn::Name&,
                                     const ndn::Name& serviceName,
                                     const ndn::Name& requestId,
                                     const ndn::Name& ackName,
                                     const ndn_service_framework::RequestAckMessage& ack) {
      bridge.publish(ackName,
                     makeRequestAckNameWithoutPrefix(requesterIdentity, serviceName, requestId),
                     ack);
    });
  }

  void
  setPublishMessageBridgeForResponses(PublishMessageBridge& bridge)
  {
    setResponsePublisher([&bridge](const ndn::Name& requesterIdentity,
                                   const ndn::Name&,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId,
                                   const ndn::Name& responseName,
                                   const ndn_service_framework::ResponseMessage& response) {
      bridge.publish(responseName,
                     makeResponseNameWithoutPrefix(requesterIdentity, serviceName, requestId),
                     response);
    });
  }

  void
  addService(const ndn::Name& serviceName,
             AckStrategyHandler ackHandler,
             RequestHandler requestHandler)
  {
    m_services[serviceName] = {std::move(ackHandler), std::move(requestHandler)};
  }

  void
  addService(const ndn::Name& serviceName,
             RequestHandler requestHandler)
  {
    addService(serviceName, AckStrategyHandler{}, std::move(requestHandler));
  }

  void
  addService(const ndn::Name& serviceName,
             AckStrategyHandler ackHandler,
             SimpleRequestHandler requestHandler)
  {
    addService(serviceName,
               ndn::Name(),
               std::move(ackHandler),
               [handler = std::move(requestHandler)](const ndn::Name&,
                                                     const ndn::Name&,
                                                     const ndn::Name&,
                                                     const ndn::Name&,
                                                     const ndn_service_framework::RequestMessage& requestMessage) {
                 return handler(requestMessage);
               });
  }

  void
  addService(const ndn::Name& serviceName,
             const ndn::Name& functionName,
             AckStrategyHandler ackHandler,
             RequestHandler requestHandler)
  {
    addService(makeUnifiedServiceName(serviceName, functionName),
               std::move(ackHandler),
               std::move(requestHandler));
  }

  void
  addService(const ndn::Name& serviceName,
             const ndn::Name& functionName,
             RequestHandler requestHandler)
  {
    addService(makeUnifiedServiceName(serviceName, functionName), std::move(requestHandler));
  }

  template<typename RequestT, typename ResponseT>
  void
  addHandler(const ndn::Name& serviceName,
             std::function<void(const ndn::Name& requesterIdentity,
                                const RequestT& request,
                                ResponseT& response)> handler)
  {
    addService(serviceName,
               [handler = std::move(handler)](const ndn::Name& requesterIdentity,
                                              const ndn::Name&,
                                              const ndn::Name& serviceName,
                                              const ndn::Name&,
                                              const ndn_service_framework::RequestMessage& requestMessage) {
                 const auto payload = requestMessage.getPayload();

                 RequestT typedRequest;
                 if (!typedRequest.ParseFromArray(payload.data(), payload.size())) {
                   return makeErrorResponse("Failed to parse request payload for " +
                                            serviceName.toUri());
                 }

                 ResponseT typedResponse;
                 handler(requesterIdentity, typedRequest, typedResponse);

                 std::string responseBytes;
                 if (!typedResponse.SerializeToString(&responseBytes)) {
                   return makeErrorResponse("Failed to serialize response payload for " +
                                            serviceName.toUri());
                 }

                 ndn::Buffer responsePayload(
                     reinterpret_cast<const uint8_t*>(responseBytes.data()),
                     responseBytes.size());

                 ndn_service_framework::ResponseMessage responseMessage;
                 responseMessage.setStatus(true);
                 responseMessage.setErrorInfo("No error");
                 responseMessage.setPayload(responsePayload, responsePayload.size());
                 return responseMessage;
               });
  }

  template<typename RequestT, typename ResponseT>
  void
  addHandler(const ndn::Name& serviceName,
             const ndn::Name& functionName,
             std::function<void(const ndn::Name& requesterIdentity,
                                const RequestT& request,
                                ResponseT& response)> handler)
  {
    addHandler<RequestT, ResponseT>(makeUnifiedServiceName(serviceName, functionName),
                                    std::move(handler));
  }

  bool
  hasService(const ndn::Name& serviceName) const
  {
    return m_services.find(serviceName) != m_services.end();
  }

  bool
  hasService(const ndn::Name& serviceName,
             const ndn::Name& functionName) const
  {
    return hasService(makeUnifiedServiceName(serviceName, functionName));
  }

  ndn_service_framework::ResponseMessage
  dispatchRequest(const ndn::Name& requesterIdentity,
                  const ndn::Name& providerName,
                  const ndn::Name& serviceName,
                  const ndn::Name& requestId,
                  const ndn_service_framework::RequestMessage& requestMessage) const
  {
    auto service = m_services.find(serviceName);
    if (service == m_services.end()) {
      return makeErrorResponse("No handler registered for " + serviceName.toUri());
    }

    if (!service->second.requestHandler) {
      return makeErrorResponse("Registered service has no request handler for " +
                               serviceName.toUri());
    }

    return service->second.requestHandler(requesterIdentity,
                                          providerName,
                                          serviceName,
                                          requestId,
                                          requestMessage);
  }

  ndn_service_framework::ResponseMessage
  dispatchRequest(const ndn::Name& requesterIdentity,
                  const ndn::Name& providerName,
                  const ndn::Name& serviceName,
                  const ndn::Name& functionName,
                  const ndn::Name& requestId,
                  const ndn_service_framework::RequestMessage& requestMessage) const
  {
    return dispatchRequest(requesterIdentity,
                           providerName,
                           makeUnifiedServiceName(serviceName, functionName),
                           requestId,
                           requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequest(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& requestId,
                         const ndn_service_framework::RequestMessage& requestMessage) const
  {
    return dispatchRequest(requesterIdentity,
                           providerName,
                           serviceName,
                           requestId,
                           requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestAndPublish(
      const ndn::Name& requesterIdentity,
      const ndn::Name& providerName,
      const ndn::Name& serviceName,
      const ndn::Name& requestId,
      const ndn_service_framework::RequestMessage& requestMessage) const
  {
    auto response = handleDecryptedRequest(requesterIdentity,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           requestMessage);

    publishResponseIfConfigured(requesterIdentity,
                                providerName,
                                serviceName,
                                requestId,
                                response);
    return response;
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequest(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& requestId,
                         const ndn::Block& requestBlock) const
  {
    ndn_service_framework::RequestMessage requestMessage;
    if (!requestMessage.WireDecode(requestBlock)) {
      return makeErrorResponse("Failed to decode RequestMessage for " + serviceName.toUri());
    }

    return handleDecryptedRequest(requesterIdentity,
                                  providerName,
                                  serviceName,
                                  requestId,
                                  requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestAndPublish(const ndn::Name& requesterIdentity,
                                   const ndn::Name& providerName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId,
                                   const ndn::Block& requestBlock) const
  {
    auto response = handleDecryptedRequest(requesterIdentity,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           requestBlock);

    publishResponseIfConfigured(requesterIdentity,
                                providerName,
                                serviceName,
                                requestId,
                                response);
    return response;
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestByName(
      const ndn::Name& requestName,
      const ndn_service_framework::RequestMessage& requestMessage) const
  {
    auto parsed = parseRequestNameForUnifiedService(requestName);
    if (!parsed) {
      return makeErrorResponse("Failed to parse request name " + requestName.toUri());
    }

    return handleDecryptedRequest(parsed->requesterIdentity,
                                  m_identityCert.getIdentity(),
                                  parsed->serviceName,
                                  parsed->requestId,
                                  requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestByNameAndPublish(
      const ndn::Name& requestName,
      const ndn_service_framework::RequestMessage& requestMessage) const
  {
    auto parsed = parseRequestNameForUnifiedService(requestName);
    if (!parsed) {
      return makeErrorResponse("Failed to parse request name " + requestName.toUri());
    }

    auto response = handleDecryptedRequest(parsed->requesterIdentity,
                                           m_identityCert.getIdentity(),
                                           parsed->serviceName,
                                           parsed->requestId,
                                           requestMessage);

    publishResponseIfConfigured(parsed->requesterIdentity,
                                m_identityCert.getIdentity(),
                                parsed->serviceName,
                                parsed->requestId,
                                response);
    return response;
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestByName(const ndn::Name& requestName,
                               const ndn::Block& requestBlock) const
  {
    ndn_service_framework::RequestMessage requestMessage;
    if (!requestMessage.WireDecode(requestBlock)) {
      return makeErrorResponse("Failed to decode RequestMessage for " + requestName.toUri());
    }

    return handleDecryptedRequestByName(requestName, requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestByNameAndPublish(const ndn::Name& requestName,
                                         const ndn::Block& requestBlock) const
  {
    ndn_service_framework::RequestMessage requestMessage;
    if (!requestMessage.WireDecode(requestBlock)) {
      return makeErrorResponse("Failed to decode RequestMessage for " + requestName.toUri());
    }

    return handleDecryptedRequestByNameAndPublish(requestName, requestMessage);
  }

  ndn_service_framework::RequestAckMessage
  makeRequestAckForName(const ndn::Name& requestName) const
  {
    auto parsed = parseRequestNameForUnifiedService(requestName);
    if (!parsed) {
      return makeRequestAck(false, "Failed to parse request name");
    }

    auto service = m_services.find(parsed->serviceName);
    if (service == m_services.end()) {
      return makeRequestAck(false, "No handler registered for " + parsed->serviceName.toUri());
    }

    auto ack = makeRequestAck(true, "Permission Granted");
    if (service->second.ackHandler) {
      const auto decision = service->second.ackHandler(ack);
      ack.setStatus(decision.first);
      ack.setMessage(decision.first ? "ACK accepted by strategy" : "ACK rejected by strategy");
    }

    return ack;
  }

  ndn_service_framework::RequestAckMessage
  publishRequestAckForName(const ndn::Name& requestName) const
  {
    auto parsed = parseRequestNameForUnifiedService(requestName);
    auto ack = makeRequestAckForName(requestName);
    if (!parsed) {
      return ack;
    }

    publishRequestAckIfConfigured(parsed->requesterIdentity,
                                  m_identityCert.getIdentity(),
                                  parsed->serviceName,
                                  parsed->requestId,
                                  ack);
    return ack;
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequest(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& functionName,
                         const ndn::Name& requestId,
                         const ndn_service_framework::RequestMessage& requestMessage) const
  {
    return handleDecryptedRequest(requesterIdentity,
                                  providerName,
                                  makeUnifiedServiceName(serviceName, functionName),
                                  requestId,
                                  requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestAndPublish(
      const ndn::Name& requesterIdentity,
      const ndn::Name& providerName,
      const ndn::Name& serviceName,
      const ndn::Name& functionName,
      const ndn::Name& requestId,
      const ndn_service_framework::RequestMessage& requestMessage) const
  {
    return handleDecryptedRequestAndPublish(requesterIdentity,
                                            providerName,
                                            makeUnifiedServiceName(serviceName, functionName),
                                            requestId,
                                            requestMessage);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequest(const ndn::Name& requesterIdentity,
                         const ndn::Name& providerName,
                         const ndn::Name& serviceName,
                         const ndn::Name& functionName,
                         const ndn::Name& requestId,
                         const ndn::Block& requestBlock) const
  {
    return handleDecryptedRequest(requesterIdentity,
                                  providerName,
                                  makeUnifiedServiceName(serviceName, functionName),
                                  requestId,
                                  requestBlock);
  }

  ndn_service_framework::ResponseMessage
  handleDecryptedRequestAndPublish(const ndn::Name& requesterIdentity,
                                   const ndn::Name& providerName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& functionName,
                                   const ndn::Name& requestId,
                                   const ndn::Block& requestBlock) const
  {
    return handleDecryptedRequestAndPublish(requesterIdentity,
                                            providerName,
                                            makeUnifiedServiceName(serviceName, functionName),
                                            requestId,
                                            requestBlock);
  }

private:
  static ndn_service_framework::ResponseMessage
  makeErrorResponse(const std::string& errorInfo)
  {
    ndn_service_framework::ResponseMessage response;
    response.setStatus(false);
    response.setErrorInfo(errorInfo);
    return response;
  }

  static ndn_service_framework::RequestAckMessage
  makeRequestAck(bool status,
                 const std::string& message)
  {
    ndn_service_framework::RequestAckMessage ack;
    ack.setStatus(status);
    ack.setMessage(message);
    return ack;
  }

  void
  publishResponseIfConfigured(const ndn::Name& requesterIdentity,
                              const ndn::Name& providerName,
                              const ndn::Name& serviceName,
                              const ndn::Name& requestId,
                              const ndn_service_framework::ResponseMessage& response) const
  {
    if (m_responsePublisher) {
      m_responsePublisher(requesterIdentity,
                          providerName,
                          serviceName,
                          requestId,
                          makeResponseName(requesterIdentity,
                                           providerName,
                                           serviceName,
                                           requestId),
                          response);
    }
  }

  void
  publishRequestAckIfConfigured(const ndn::Name& requesterIdentity,
                                const ndn::Name& providerName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId,
                                const ndn_service_framework::RequestAckMessage& ack) const
  {
    if (m_requestAckPublisher) {
      m_requestAckPublisher(requesterIdentity,
                            providerName,
                            serviceName,
                            requestId,
                            makeRequestAckName(requesterIdentity,
                                               providerName,
                                               serviceName,
                                               requestId),
                            ack);
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

  static std::optional<ParsedRequestName>
  parseRequestNameForUnifiedService(const ndn::Name& requestName)
  {
    for (size_t i = 0; i + 1 < requestName.size(); ++i) {
      if (requestName[i].toUri() != "NDNSF" || requestName[i + 1].toUri() != "REQUEST") {
        continue;
      }

      if (requestName.size() < i + 5) {
        return std::nullopt;
      }

      ndn::Name fullServiceName;
      for (size_t j = i + 2; j + 2 < requestName.size(); ++j) {
        fullServiceName.append(requestName[j]);
      }

      if (fullServiceName.empty()) {
        return std::nullopt;
      }

      ndn::Name parsedServiceName;
      parsedServiceName.append(fullServiceName[0]);

      ndn::Name parsedFunctionName;
      for (size_t j = 1; j < fullServiceName.size(); ++j) {
        parsedFunctionName.append(fullServiceName[j]);
      }

      return ParsedRequestName{
          requestName.getPrefix(i),
          makeUnifiedServiceName(parsedServiceName, parsedFunctionName),
          ndn::Name().append(requestName[-1])};
    }

    auto parsed = ndn_service_framework::parseRequestName(requestName);
    if (!parsed) {
      return std::nullopt;
    }

    ndn::Name requesterIdentity;
    ndn::Name parsedServiceName;
    ndn::Name parsedFunctionName;
    ndn::Name bloomFilterName;
    ndn::Name requestId;
    std::tie(requesterIdentity, parsedServiceName, parsedFunctionName, bloomFilterName, requestId) =
        parsed.value();

    return ParsedRequestName{
        requesterIdentity,
        makeUnifiedServiceName(parsedServiceName, parsedFunctionName),
        requestId};
  }

  static ndn::Name
  makeResponseName(const ndn::Name& requesterIdentity,
                   const ndn::Name& providerName,
                   const ndn::Name& serviceName,
                   const ndn::Name& requestId)
  {
    return ndn_service_framework::makeResponseName(providerName,
                                                   requesterIdentity,
                                                   serviceName,
                                                   ndn::Name(),
                                                   requestId);
  }

  static ndn::Name
  makeResponseNameWithoutPrefix(const ndn::Name& requesterIdentity,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId)
  {
    return ndn_service_framework::makeResponseNameWithoutPrefix(requesterIdentity,
                                                                serviceName,
                                                                ndn::Name(),
                                                                requestId);
  }

  static ndn::Name
  makeRequestAckName(const ndn::Name& requesterIdentity,
                     const ndn::Name& providerName,
                     const ndn::Name& serviceName,
                     const ndn::Name& requestId)
  {
    return ndn_service_framework::makeRequestAckName(providerName,
                                                     requesterIdentity,
                                                     serviceName,
                                                     ndn::Name(),
                                                     requestId);
  }

  static ndn::Name
  makeRequestAckNameWithoutPrefix(const ndn::Name& requesterIdentity,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId)
  {
    return ndn_service_framework::makeRequestAckNameWithoutPrefix(requesterIdentity,
                                                                  serviceName,
                                                                  ndn::Name(),
                                                                  requestId);
  }

private:
  ndn::Face& m_face;

  ndn::Name m_groupPrefix;

  ndn::security::Certificate m_identityCert;
  ndn::security::Certificate m_attrAuthorityCertificate;

  std::string m_trustSchemaPath;

  std::map<ServiceKey, RegisteredService> m_services;
  ResponsePublisher m_responsePublisher;
  RequestAckPublisher m_requestAckPublisher;
};

} // namespace muas

#endif // NDNSF_PROVIDER_HPP







