#ifndef EXAMPLE_SERVICE_PROVIDER_Drone_HPP
#define EXAMPLE_SERVICE_PROVIDER_Drone_HPP

#include <ndn-service-framework/ServiceProvider.hpp>

#include "./ObjectDetectionService.hpp"

#include "./FlightControlService.hpp"



namespace muas
{
    class ServiceProvider_Drone : public ndn_service_framework::ServiceProvider
    {
    public:
        ServiceProvider_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath);
        virtual ~ServiceProvider_Drone();

    protected:
        virtual void registerServiceInfo() override;

        void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage) override;


    public:
        
        muas::ObjectDetectionService m_ObjectDetectionService;
        
        muas::FlightControlService m_FlightControlService;
        
    };
}

#endif