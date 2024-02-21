#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP

#include "common.hpp"

#include "Service.hpp"
#include "utils.hpp"

namespace ndn_service_framework{

    using PermissionCheckCallback = std::function<void(bool isAuthorized)>;
  
    class ServiceProvider
    {
        public:
            ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            virtual ~ServiceProvider() {}


            void PermissionCheck(const ndn::Name& requesterIdentity,const ndn::Name &ServiceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName,const ndn::Name& RequestID, PermissionCheckCallback AfterPermissionCheck);

            
            
            virtual void OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription) = 0;

            virtual void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& ServiceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID) = 0;

            void OnPermissionChallengeResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // virtual void OnRequestDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &);
            virtual void OnRequestDecryptionErrorCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &) = 0;
            
            void PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer& buffer);

            bool replyFromIMS(const ndn::Interest &interest);

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            ndn::Face& m_face;
            ndn::Name identity;
            ndn::KeyChain m_keyChain;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            std::shared_ptr<MessageValidator> validator;

            //ndn::security::Validator nac_validator;
            ndn::ValidatorConfig nac_validator{m_face};
            ndn::security::Certificate identityCert;
            ndn::security::Certificate attrAuthorityCertificate;
            ndn::nacabe::Consumer requestConsumer;
            ndn::nacabe::Producer responseProduer;
            ndn::security::SigningInfo m_signingInfo;

            // ChanllengeID->(Token->RequestNameWithoutRequestID)
            std::map<ndn::Name,std::pair<std::string, ndn::Name>> chanllengeRecords;
            // RequestPrefix is a Request Name Without RequestID
            std::set<ndn::Name> authorizedRequestPrefixSet;
            // Requests that are authorized request -> requestPrefix
            std::map<ndn::Name,ndn::Name> unauthorizedRequestMap;

            ndn::random::RandomNumberEngine random;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;
    };
}

#endif