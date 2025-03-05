#ifndef EXAMPLE_SERVICE_PROVIDER_GS_HPP
#define EXAMPLE_SERVICE_PROVIDER_GS_HPP

#include <ndn-service-framework/ServiceProvider.hpp>

#include "./ObjectDetectionService.hpp"



namespace muas
{
    class ServiceProvider_GS : public ndn_service_framework::ServiceProvider
    {
    public:
        ServiceProvider_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath);
        virtual ~ServiceProvider_GS();

    protected:
        virtual void registerServiceInfo() override;

        void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage) override;


    public:
        
        muas::ObjectDetectionService m_ObjectDetectionService;
        
    };
}

#endif