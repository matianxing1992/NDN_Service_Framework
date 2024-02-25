#include "./ServiceProvider_Drone.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ServiceProvider_Drone);
    ServiceProvider_Drone::ServiceProvider_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
        : ndn_service_framework::ServiceProvider(face, group_prefix, identityCert, attrAuthorityCertificate,  trustSchemaPath),
        
            
        m_ObjectDetectionService(*this)
            
        

    {
        init();
    }

    void ServiceProvider_Drone::init()
    {
        
        for (auto regex : m_ObjectDetectionService.regexSet)
        {
            m_svsps->subscribeWithRegex(*regex,
                                        std::bind(&ServiceProvider_Drone::OnRequest, this, _1),
                                        false);
        }
        
    }

    void ServiceProvider_Drone::OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription)

    {

        ndn::Name RequesterName, ServiceProviderIdentity, ServiceName, FunctionName, RequestId;
        std::tie(RequesterName, ServiceProviderIdentity, ServiceName, FunctionName, RequestId) =
            ndn_service_framework::parseRequestName(subscription.name);
        NDN_LOG_INFO("OnRequest: " << RequesterName << ServiceProviderIdentity << ServiceName << FunctionName << RequestId);

        // check whether you should process it by the ServiceProviderIdentity if it's "/" or "/your-identity"
        if (!ServiceProviderIdentity.equals(identity) & !ServiceProviderIdentity.equals(ndn::Name()))
        {
            return;
        }

        // check the permission of user identityndn::N
        PermissionCheck(RequesterName,ServiceProviderIdentity,ServiceName,FunctionName, RequestId, [=](bool isAuthorized){
            if(isAuthorized)
                ConsumeRequest(RequesterName,ServiceName,ServiceProviderIdentity,FunctionName,RequestId);
        });
        // decrypt the request message with nac-abe; if cannot be decrypted

        

    }

    void ServiceProvider_Drone::ConsumeRequest(const ndn::Name& requesterName,const ndn::Name& ServiceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID)
    {
        
        if (ServiceName.equals(m_ObjectDetectionService.serviceName))
        {
            ndn::Name request=ndn_service_framework::makeRequestName(requesterName,ServiceProviderName,ServiceName,FunctionName, RequestID);
            requestConsumer.consume(request,
                                    std::bind(&muas::ObjectDetectionService::OnRequestDecryptionSuccessCallback, m_ObjectDetectionService, requesterName, ServiceName, FunctionName, RequestID, _1),
                                    std::bind(&ServiceProvider_Drone::OnRequestDecryptionErrorCallback, this, requesterName, ServiceName, FunctionName, RequestID, _1));
        }
        
    }


    void ServiceProvider_Drone::OnRequestDecryptionErrorCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string &)
    {
        NDN_LOG_ERROR("OnRequestDecryptionErrorCallback: " << requesterIdentity << ServiceName << FunctionName);
    }

}