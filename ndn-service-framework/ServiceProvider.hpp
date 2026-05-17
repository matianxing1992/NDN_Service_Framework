#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP

#include "common.hpp"

#include "Service.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"
#include "ConfigManager.hpp"

#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>



namespace ndn_service_framework{

    class ServiceProvider
    {
        public:
            using ServiceKey = ndn::Name;

            struct AckDecision
            {
                bool status = false;
                std::string message;
                ndn::Buffer payload;
            };

            using AckStrategyHandler =
                std::function<AckDecision(const RequestMessage&)>;

            using LegacyAckStrategyHandler =
                std::function<std::pair<bool, ndn::Block>(const RequestAckMessage&)>;

            using SimpleAckStrategyHandler =
                std::function<bool(const RequestMessage&)>;

            using RequestHandler =
                std::function<ResponseMessage(const ndn::Name& requesterIdentity,
                                              const ndn::Name& providerName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId,
                                              const RequestMessage& requestMessage)>;

            using SimpleRequestHandler =
                std::function<ResponseMessage(const RequestMessage& requestMessage)>;

            struct LocalMockTag
            {
            };

            ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            ServiceProvider(LocalMockTag,
                            ndn::Face& face,
                            ndn::Name group_prefix,
                            ndn::security::Certificate identityCert,
                            ndn::security::Certificate attrAuthorityCertificate,
                            std::string trustSchemaPath);
            virtual ~ServiceProvider() {}

            void init();

            ndn::Name getName();

            void fetchPermissionsFromController(const ndn::Name& controllerPrefix);
            void applyPermissionResponse(const PermissionResponse& response);
            static bool handlePermissionResponseData(const ndn::Data& data,
                                                     const ndn::Name& identity,
                                                     ndn::KeyChain& keyChain,
                                                     UserPermissionTable& permissionTable);

            void UpdateUPTWithServiceMetaInfo(ndnsd::discovery::Details serviceDetails);
            
            void OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // After receiving service coordination message, this function is called to consumeRequest.
            // Generic dynamic providers can rely on this safe default; legacy generated providers
            // may still override it for service-specific dispatch.
            virtual void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, RequestMessage& requestMessage);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            LegacyAckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            LegacyAckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            SimpleAckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            SimpleAckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            const ndn::Name& functionName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            const ndn::Name& functionName,
                            LegacyAckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            const ndn::Name& functionName,
                            RequestHandler requestHandler);

            void setAckStrategyHandler(const ndn::Name& serviceName,
                                       AckStrategyHandler ackHandler);

            void setAckStrategyHandler(const ndn::Name& serviceName,
                                       const ndn::Name& functionName,
                                       AckStrategyHandler ackHandler);

            void setLegacyAckStrategyHandler(const ndn::Name& serviceName,
                                             LegacyAckStrategyHandler ackHandler);

            void setLegacyAckStrategyHandler(const ndn::Name& serviceName,
                                             const ndn::Name& functionName,
                                             LegacyAckStrategyHandler ackHandler);

            template<typename RequestT, typename ResponseT>
            void addHandler(const ndn::Name& serviceName,
                            std::function<void(const ndn::Name& requesterIdentity,
                                               const RequestT& request,
                                               ResponseT& response)> handler)
            {
                addService(serviceName,
                           [handler = std::move(handler)](
                               const ndn::Name& requesterIdentity,
                               const ndn::Name&,
                               const ndn::Name& serviceName,
                               const ndn::Name&,
                               const RequestMessage& requestMessage) {
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

                               ResponseMessage responseMessage;
                               responseMessage.setStatus(true);
                               responseMessage.setErrorInfo("No error");
                               responseMessage.setPayload(responsePayload, responsePayload.size());
                               return responseMessage;
                           });
            }

            template<typename RequestT, typename ResponseT>
            void addHandler(const ndn::Name& serviceName,
                            const ndn::Name& functionName,
                            std::function<void(const ndn::Name& requesterIdentity,
                                               const RequestT& request,
                                               ResponseT& response)> handler)
            {
                addHandler<RequestT, ResponseT>(makeUnifiedServiceName(serviceName, functionName),
                                                std::move(handler));
            }

            bool hasService(const ndn::Name& serviceName) const;

            bool hasService(const ndn::Name& serviceName,
                            const ndn::Name& functionName) const;

            ResponseMessage dispatchRequest(const ndn::Name& requesterIdentity,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId,
                                            const RequestMessage& requestMessage) const;

            ResponseMessage dispatchRequest(const ndn::Name& requesterIdentity,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& functionName,
                                            const ndn::Name& requestId,
                                            const RequestMessage& requestMessage) const;

            ResponseMessage handleDecryptedRequestByName(const ndn::Name& requestName,
                                                         const RequestMessage& requestMessage) const;

            ResponseMessage handleDecryptedRequestByName(const ndn::Name& requestName,
                                                         const ndn::Block& requestBlock) const;

            void OnRequestDecryptionSuccessCallbackV2(const ndn::Name& requesterIdentity,
                                                       const ndn::Name& serviceName,
                                                       const ndn::Name& bloomFilterName,
                                                       const ndn::Name& requestId,
                                                       const ndn::Buffer& buffer);

            void OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilterName,  const ndn::Name &RequestID, const ndn::Buffer & buffer);
    
            // virtual void OnRequestDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &);
            void OnRequestDecryptionErrorCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &);
            
            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void onPermissionResponseData(const ndn::Interest& interest,
                                          const ndn::Data& data);
            void onPermissionResponseTimeout(const ndn::Interest& interest);

            // void PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer& buffer);

            bool replyFromIMS(const ndn::Interest &interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishRequestAckMessage(const ndn::Name & requesterIdentity, const ndn::Name & ServiceName, const ndn::Name & FunctionName, const ndn::Name & RequestID, bool status, const std::string& msg, const ndn::Buffer& payload = ndn::Buffer(), const std::string& userToken = "", const std::string& providerToken = "");
            void PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId,
                                            bool status,
                                            const std::string& msg,
                                            const ndn::Buffer& payload = ndn::Buffer(),
                                            const std::string& userToken = "",
                                            const std::string& providerToken = "");
    
            void onServiceCoordinationMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void PublishMessage(const ndn::Name& messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage& message);

            void OnServiceCoordinationMessageDecryptionSuccessCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const ndn::Buffer & buffer);
            void OnServiceCoordinationMessageDecryptionSuccessCallbackV2(const ndn::Name& requesterName,
                                                                          const ndn::Name& providerName,
                                                                          const ndn::Name& serviceName,
                                                                          const ndn::Name& msgId,
                                                                          const ndn::Buffer& buffer);

            void OnServiceCoordinationMessageDecryptionErrorCallback(const ndn::Name& requesterName,const ndn::Name &providerName, const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& msgID,const std::string & reason);
            
            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();

            // Register service info using ndnsd(). Generic dynamic providers may use the no-op
            // default; legacy generated providers may still override it.
            virtual void registerServiceInfo();

            bool isFresh(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
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

            static ResponseMessage makeErrorResponse(const std::string& errorInfo);

            static AckDecision makeDefaultAckDecision();

            static ndn::Name makeUnifiedServiceName(const ndn::Name& serviceName,
                                                    const ndn::Name& functionName);

            static std::optional<ParsedRequestName>
            parseRequestNameForUnifiedService(const ndn::Name& requestName);

            ndn::Face& m_face;
            ndn::Scheduler m_scheduler;
            ndn::Name identity;
            ndn::KeyChain m_keyChain;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            std::shared_ptr<MessageValidator> validator;
            std::vector<std::string> m_serviceNames;

            //ndn::security::Validator nac_validator;
            ndn::ValidatorConfig nac_validator{m_face};
            ndn::security::Certificate identityCert;
            ndn::security::Certificate attrAuthorityCertificate;
            ndn::nacabe::Consumer nacConsumer;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            ndn::security::SigningInfo m_signingInfo;

            // ChanllengeID->(Token->RequestNameWithoutRequestID)
            std::map<ndn::Name,std::pair<std::string, ndn::Name>> chanllengeRecords;
            // RequestPrefix is a Request Name Without RequestID
            std::set<ndn::Name> authorizedRequestPrefixSet;
            // Requests that are authorized request -> requestPrefix
            std::map<ndn::Name,ndn::Name> unauthorizedRequestMap;

            /*
                pending requests waiting for Service Coordination Message;
                (/<requesterName>/<ServiceName>/<FunctionName>/<RequestID> -> RequestMessage)
            */
            std::map<ndn::Name,std::shared_ptr<RequestMessage>> pendingRequests;
            std::map<ndn::Name,std::string> pendingProviderTokens;

            ndn::random::RandomNumberEngine random;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            OptionalServiceDiscovery m_ServiceDiscovery;

            UserPermissionTable UPT;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ServiceKey, RegisteredService> m_services;
    };
}

#endif
