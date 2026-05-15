#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "ServiceStub.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"
#include "ConfigManager.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>


namespace ndn_service_framework{

    struct AckInfo{
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestID;
    };

    using Timeout_Callback = std::function<void(const std::string & reason)>;

    class ServiceUser
    {
        public:
            using AcksHandler =
                std::function<std::vector<ndn_service_framework::RequestAckMessage>(
                    const std::vector<ndn_service_framework::RequestAckMessage>&)>;

            using ResponseHandler =
                std::function<void(const ndn_service_framework::ResponseMessage&)>;

            using TimeoutHandler =
                std::function<void(const ndn::Name&)>;

            ServiceUser(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);

            virtual ~ServiceUser() {}
            // void addServiceStub(ServiceStub serviceStub);

            void init();

            ndn::Name getName();

            void PublishRequest(const std::vector<ndn::Name>& serviceProviderNames,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &payload, const size_t& strategy=ndn_service_framework::tlv::FirstResponding);
            void PublishRequestV2(const std::vector<ndn::Name>& serviceProviderNames,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId,
                                  const ndn::Buffer& payload,
                                  const size_t& strategy=ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& functionName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AcksHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            template<typename RequestT, typename ResponseT>
            ndn::Name asyncCall(const std::vector<ndn::Name>& providers,
                                const ndn::Name& serviceName,
                                const RequestT& request,
                                std::function<void(const ResponseT&)> onResponse,
                                std::function<void()> onTimeout,
                                int timeoutMs,
                                size_t strategy = ndn_service_framework::tlv::FirstResponding)
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
            ndn::Name asyncCall(const std::vector<ndn::Name>& providers,
                                const ndn::Name& serviceName,
                                const ndn::Name& functionName,
                                const RequestT& request,
                                std::function<void(const ResponseT&)> onResponse,
                                std::function<void()> onTimeout,
                                int timeoutMs,
                                size_t strategy = ndn_service_framework::tlv::FirstResponding)
            {
                return asyncCall<RequestT, ResponseT>(providers,
                                                      makeUnifiedServiceName(serviceName, functionName),
                                                      request,
                                                      std::move(onResponse),
                                                      std::move(onTimeout),
                                                      timeoutMs,
                                                      strategy);
            }

            void handleResponse(const ndn::Name& requestId,
                                const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn::Block& responseBlock);

            bool handleDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn::Block& responseBlock);

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn_service_framework::RequestAckMessage& ackMessage);

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn::Block& ackBlock);

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void OnRequestAckDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const ndn::Buffer &buffer) ;

            void OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error) ;

            void PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID) ;
            void PublishServiceCoordinationMessageV2(const ndn::Name& providerName,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId);

            void OnResponseDecryptionErrorCallback(const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &);

            void OnPermissionTokenDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const ndn::Buffer &buffer) ;

            void OnPermissionTokenDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const std::string &error) ;

            bool replyFromIMS(const ndn::Interest &interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishMessage(const ndn::Name& messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage& message);

            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();

            // search for service info using ndnsd();
            void requestForServiceInfo();

            bool isFresh(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
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

            static ndn::Name makeRequestId();

            static ndn::Name makeUnifiedServiceName(const ndn::Name& serviceName,
                                                    const ndn::Name& functionName);

            bool evaluateAckSelection(const ndn::Name& requestId);

            bool evaluateCustomAckSelection(PendingCall& pendingCall);

            bool evaluateBuiltInAckSelection(PendingCall& pendingCall);

            static bool containsName(const std::vector<ndn::Name>& names,
                                     const ndn::Name& name);

            static void addUniqueName(std::vector<ndn::Name>& names,
                                      const ndn::Name& name);

            static ndn::Name selectLoadBalancingProvider(const std::vector<ndn::Name>& providers);

            static const StoredAck* findStoredAck(
                const PendingCall& pendingCall,
                const ndn_service_framework::RequestAckMessage& ackMessage);

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
            
            ndn::nacabe::Consumer nacConsumer;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            ndn::security::SigningInfo m_signingInfo;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            ndnsd::discovery::ServiceDiscovery m_ServiceDiscovery;
            UserPermissionTable UPT;

            std::map<ndn::Name, size_t> m_strategyMap;

            // a map used for load balancing requestID 
            std::map<ndn::Name, std::vector<AckInfo>> m_AckInfoMap;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ndn::Name, PendingCall> m_pendingCalls;
    };
}

#endif
