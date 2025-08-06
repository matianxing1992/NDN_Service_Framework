#include "./ServiceUser_GS.hpp"

NDN_LOG_INIT(muas.ServiceUser_GS);

muas::ServiceUser_GS::ServiceUser_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
    :ndn_service_framework::ServiceUser(face, group_prefix, identityCert, attrAuthorityCertificate, trustSchemaPath),
    
        
    m_ObjectDetectionServiceStub(*this),
        
    
        
    m_FlightControlServiceStub(*this)
        
    
{
    
    this->m_serviceNames.push_back("ObjectDetection");
    
    this->m_serviceNames.push_back("FlightControl");
    
    init();
}

muas::ServiceUser_GS::~ServiceUser_GS() {}

void muas::ServiceUser_GS::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
{
        if(!isFresh(subscription)) return;

        ndn::Name RequesterName, providerName,ServiceName, FunctionName, RequestId;
        //std::tie(ServiceProviderName, RequesterName, ServiceName, FunctionName, RequestId) =
        auto results=ndn_service_framework::parseResponseName(subscription.name);
        if(!results){
            NDN_LOG_ERROR("parseResponseName failed: " << subscription.name);
            return;
        }
        std::tie(RequesterName, providerName, ServiceName, FunctionName, RequestId) = results.value();
        NDN_LOG_INFO("OnResponse: " << RequesterName << providerName << ServiceName << FunctionName << RequestId);

        // decrypt the request message with nac-abe; if cannot be decrypted
        
        if (ServiceName.equals(m_ObjectDetectionServiceStub.serviceName))
        {
            NDN_LOG_INFO("Response for : "  << m_ObjectDetectionServiceStub.serviceName);
            if(subscription.data.size() > 0){
                nacConsumer.consume(subscription.name,
                                    ndn::Block(subscription.data),
                                    std::bind(&muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback, &m_ObjectDetectionServiceStub, providerName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_GS::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback, &m_ObjectDetectionServiceStub, providerName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_GS::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }
        }
        
        if (ServiceName.equals(m_FlightControlServiceStub.serviceName))
        {
            NDN_LOG_INFO("Response for : "  << m_FlightControlServiceStub.serviceName);
            if(subscription.data.size() > 0){
                nacConsumer.consume(subscription.name,
                                    ndn::Block(subscription.data),
                                    std::bind(&muas::FlightControlServiceStub::OnResponseDecryptionSuccessCallback, &m_FlightControlServiceStub, providerName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_GS::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&muas::FlightControlServiceStub::OnResponseDecryptionSuccessCallback, &m_FlightControlServiceStub, providerName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_GS::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }
        }
        
}


