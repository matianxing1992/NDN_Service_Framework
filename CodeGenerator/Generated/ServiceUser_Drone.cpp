#include "./ServiceUser_Drone.hpp"

NDN_LOG_INIT(muas.ServiceUser_Drone);

muas::ServiceUser_Drone::ServiceUser_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
    :ndn_service_framework::ServiceUser(face, group_prefix, identityCert, attrAuthorityCertificate, trustSchemaPath),
    
        
    m_ObjectDetectionServiceStub(*this)
        
    
{
    
    this->m_serviceNames.push_back("ObjectDetection");
    
    init();
}

muas::ServiceUser_Drone::~ServiceUser_Drone() {}

void muas::ServiceUser_Drone::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
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
                                    std::bind(&ServiceUser_Drone::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback, &m_ObjectDetectionServiceStub, providerName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_Drone::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
            }
        }
        
}


