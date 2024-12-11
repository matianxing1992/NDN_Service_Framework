#include "./ServiceProvider_GS.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ServiceProvider_GS);
    ServiceProvider_GS::ServiceProvider_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
        : ndn_service_framework::ServiceProvider(face, group_prefix, identityCert, attrAuthorityCertificate,  trustSchemaPath),
        m_ObjectDetectionService(*this)
    {
        
        this->m_serviceNames.push_back("ObjectDetection");
        
        init();
    }

    ServiceProvider_GS::~ServiceProvider_GS(){}

    void ServiceProvider_GS::ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage)
    {
        // log the request
        NDN_LOG_TRACE("Received request from " << RequesterName << " for service " << ServiceName << " function " << FunctionName << " with request id " << RequestID);

        
        if (ServiceName.equals(m_ObjectDetectionService.serviceName))
        {
            m_ObjectDetectionService.ConsumeRequest(RequesterName, providerName, ServiceName, FunctionName, RequestID, requestMessage);                                  
        }
        
    }


}