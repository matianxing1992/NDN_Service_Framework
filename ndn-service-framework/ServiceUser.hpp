#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "ServiceStub.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"


namespace ndn_service_framework{

    struct AckInfo{
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestID;
    };

    class ServiceUser
    {
        public:
            ServiceUser(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);

            virtual ~ServiceUser() {}
            // void addServiceStub(ServiceStub serviceStub);

            void init();

            ndn::Name getName();

            void PublishRequest(const std::vector<ndn::Name>& serviceProviderNames,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &payload, const size_t& strategy=ndn_service_framework::tlv::FirstResponding);

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)= 0;

            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void OnRequestAckDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const ndn::Buffer &buffer) ;

            void OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error) ;

            void PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID) ;

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

            

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
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
    };
}

#endif