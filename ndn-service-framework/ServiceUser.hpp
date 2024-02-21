#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "ServiceStub.hpp"
#include "utils.hpp"

namespace ndn_service_framework{

    class ServiceUser
    {
        public:
            ServiceUser(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);

            virtual ~ServiceUser() {}
            // void addServiceStub(ServiceStub serviceStub);

            virtual void PublishRequest(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer)= 0;

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)= 0;

            void OnPermissionChallenge(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            //virtual void OnResponseDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &)= 0;

            virtual void OnResponseDecryptionErrorCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &)= 0;


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
            
            ndn::nacabe::Consumer requestConsumer;
            ndn::nacabe::Producer responseProduer;
            ndn::security::SigningInfo m_signingInfo;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;
    };
}

#endif