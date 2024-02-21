#ifndef EXAMPLE_SERVICE_USER_GS_HPP
#define EXAMPLE_SERVICE_USER_GS_HPP

#include <ndn-service-framework/ServiceUser.hpp>
#include "ObjectDetectionServiceStub.hpp"

namespace muas
{
    class ServiceUser_GS : public ndn_service_framework::ServiceUser
    {
    public:
        ServiceUser_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
        ~ServiceUser_GS() {}

        void init();

        void PublishRequest(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;

        void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription) override;

        //void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, const ndn::Buffer&) override;

        void OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, const std::string&) override;

    public:
        
        
        muas::ObjectDetectionServiceStub m_ObjectDetectionServiceStub;
    };
}

#endif