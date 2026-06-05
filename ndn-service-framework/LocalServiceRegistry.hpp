#ifndef NDN_SERVICE_FRAMEWORK_LOCAL_SERVICE_REGISTRY_HPP
#define NDN_SERVICE_FRAMEWORK_LOCAL_SERVICE_REGISTRY_HPP

#include "NDNSFMessages.hpp"

#include <ndn-cxx/name.hpp>

#include <future>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace ndn_service_framework {

/**
 * In-process service registry for trusted local service composition.
 *
 * Local invocation is not a wire protocol mode. It does not publish NDNSF
 * messages, does not consume tokens, and does not perform NAC-ABE checks. Only
 * code holding a reference to this registry can call registered handlers.
 */
class LocalServiceRegistry
{
public:
  using LocalRequestHandler =
    std::function<void(const ndn::Name& requesterIdentity,
                       const ndn::Name& serviceName,
                       const RequestMessage& requestMessage,
                       ResponseMessage& responseMessage)>;

  using LegacyLocalRequestHandler =
    std::function<ResponseMessage(const ndn::Name& requesterIdentity,
                                  const ndn::Name& serviceName,
                                  const RequestMessage& requestMessage)>;

  template<typename ResponseT>
  struct InvocationResult
  {
    bool success = false;
    ResponseT response;
    std::string error;
  };

  void
  registerLocalService(const ndn::Name& serviceName, LocalRequestHandler handler)
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_handlers[serviceName] = std::move(handler);
  }

  void
  registerLocalService(const ndn::Name& serviceName, LegacyLocalRequestHandler handler)
  {
    registerLocalService(serviceName,
                         [handler = std::move(handler)](
                           const ndn::Name& requesterIdentity,
                           const ndn::Name& localServiceName,
                           const RequestMessage& requestMessage,
                           ResponseMessage& responseMessage) {
                           responseMessage = handler(requesterIdentity,
                                                     localServiceName,
                                                     requestMessage);
                         });
  }

  template<typename RequestT, typename ResponseT>
  void
  registerLocalService(const ndn::Name& serviceName,
                       std::function<void(const ndn::Name& requesterIdentity,
                                          const RequestT& request,
                                          ResponseT& response)> handler)
  {
    registerLocalService(serviceName,
                         [handler = std::move(handler)](
                           const ndn::Name& requesterIdentity,
                           const ndn::Name& localServiceName,
                           const RequestMessage& requestMessage,
                           ResponseMessage& responseMessage) {
                           const auto payload = requestMessage.getPayload();
                           RequestT typedRequest;
                           if (!typedRequest.ParseFromArray(payload.data(), payload.size())) {
                             responseMessage = makeErrorResponse(
                               "Failed to parse local request payload for " +
                               localServiceName.toUri());
                             return;
                           }

                           ResponseT typedResponse;
                           handler(requesterIdentity, typedRequest, typedResponse);

                           std::string responseBytes;
                           if (!typedResponse.SerializeToString(&responseBytes)) {
                             responseMessage = makeErrorResponse(
                               "Failed to serialize local response payload for " +
                               localServiceName.toUri());
                             return;
                           }

                           ndn::Buffer responsePayload(
                             reinterpret_cast<const uint8_t*>(responseBytes.data()),
                             responseBytes.size());
                           responseMessage.setStatus(true);
                           responseMessage.setErrorInfo("No error");
                           responseMessage.setPayload(responsePayload, responsePayload.size());
                         });
  }

  bool
  hasService(const ndn::Name& serviceName) const
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_handlers.find(serviceName) != m_handlers.end();
  }

  bool
  localInvokeRawInto(const ndn::Name& serviceName,
                     const RequestMessage& requestMessage,
                     ResponseMessage& responseMessage,
                     const ndn::Name& requesterIdentity = ndn::Name("/local"))
  {
    LocalRequestHandler handler;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      const auto it = m_handlers.find(serviceName);
      if (it == m_handlers.end()) {
        responseMessage = makeErrorResponse("Local service is not registered: " +
                                            serviceName.toUri());
        return false;
      }
      handler = it->second;
    }
    handler(requesterIdentity, serviceName, requestMessage, responseMessage);
    return responseMessage.getStatus();
  }

  template<typename RequestT, typename ResponseT>
  InvocationResult<ResponseT>
  localInvoke(const ndn::Name& serviceName,
              const RequestT& request,
              const ndn::Name& requesterIdentity = ndn::Name("/local"))
  {
    std::string requestBytes;
    if (!request.SerializeToString(&requestBytes)) {
      return {false, ResponseT{}, "Failed to serialize local request payload"};
    }

    ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(requestBytes.data()), requestBytes.size());
    RequestMessage requestMessage;
    requestMessage.setPayload(requestPayload, requestPayload.size());

    ResponseMessage responseMessage;
    localInvokeRawInto(serviceName, requestMessage, responseMessage, requesterIdentity);
    if (!responseMessage.getStatus()) {
      return {false, ResponseT{}, responseMessage.getErrorInfo()};
    }

    const auto responsePayload = responseMessage.getPayload();
    ResponseT typedResponse;
    if (!typedResponse.ParseFromArray(responsePayload.data(), responsePayload.size())) {
      return {false, ResponseT{}, "Failed to parse local response payload"};
    }
    return {true, typedResponse, ""};
  }

  template<typename RequestT, typename ResponseT>
  std::future<InvocationResult<ResponseT>>
  localInvokeAsync(const ndn::Name& serviceName,
                   RequestT request,
                   const ndn::Name& requesterIdentity = ndn::Name("/local"))
  {
    return std::async(std::launch::async,
                      [this, serviceName, request = std::move(request),
                       requesterIdentity] {
                        return localInvoke<RequestT, ResponseT>(
                          serviceName, request, requesterIdentity);
                      });
  }

  template<typename RequestT, typename ResponseT>
  void
  localInvokeAsync(const ndn::Name& serviceName,
                   RequestT request,
                   std::function<void(const ResponseT&)> onResponse,
                   std::function<void(const std::string&)> onError,
                   const ndn::Name& requesterIdentity = ndn::Name("/local"))
  {
    std::thread([this, serviceName, request = std::move(request),
                 onResponse = std::move(onResponse),
                 onError = std::move(onError),
                 requesterIdentity] {
      auto result = localInvoke<RequestT, ResponseT>(serviceName, request, requesterIdentity);
      if (result.success) {
        if (onResponse) {
          onResponse(result.response);
        }
      }
      else if (onError) {
        onError(result.error);
      }
    }).detach();
  }

private:
  static ResponseMessage
  makeErrorResponse(const std::string& error)
  {
    ResponseMessage responseMessage;
    responseMessage.setStatus(false);
    responseMessage.setErrorInfo(error);
    return responseMessage;
  }

private:
  mutable std::mutex m_mutex;
  std::map<ndn::Name, LocalRequestHandler> m_handlers;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_LOCAL_SERVICE_REGISTRY_HPP
