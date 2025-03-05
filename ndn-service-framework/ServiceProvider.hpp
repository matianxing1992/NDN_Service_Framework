#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP

#include "common.hpp"

#include "Service.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"



namespace ndn_service_framework{

    class ServiceProvider
    {
        public:
            ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            virtual ~ServiceProvider() {}

            void init();

            ndn::Name getName();

            void UpdateUPTWithServiceMetaInfo(ndnsd::discovery::Details serviceDetails);
            
            void OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // After receving service coordination message, this function is called to consumeRequest;
            virtual void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, RequestMessage& requestMessage) = 0;

            void OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilterName,  const ndn::Name &RequestID, const ndn::Buffer & buffer);
    
            // virtual void OnRequestDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &);
            void OnRequestDecryptionErrorCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &);
            
            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            // void PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer& buffer);

            bool replyFromIMS(const ndn::Interest &interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishRequestAckMessage(const ndn::Name & requesterIdentity, const ndn::Name & ServiceName, const ndn::Name & FunctionName, const ndn::Name & RequestID, bool status, std::string& msg);
    
            void onServiceCoordinationMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void PublishMessage(const ndn::Name& messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage& message);

            void OnServiceCoordinationMessageDecryptionSuccessCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const ndn::Buffer & buffer);

            void OnServiceCoordinationMessageDecryptionErrorCallback(const ndn::Name& requesterName,const ndn::Name &providerName, const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& msgID,const std::string & reason);
            
            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();

            // Register service info using ndnsd();
            virtual void registerServiceInfo() = 0;

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            ndn::Face& m_face;
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

            ndn::random::RandomNumberEngine random;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            ndnsd::discovery::ServiceDiscovery m_ServiceDiscovery;

            UserPermissionTable UPT;
    };
}

#endif