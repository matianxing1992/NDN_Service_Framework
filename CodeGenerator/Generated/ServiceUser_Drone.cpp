#include "./ServiceUser_Drone.hpp"

NDN_LOG_INIT(muas.ServiceUser_Drone);

muas::ServiceUser_Drone::ServiceUser_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
    :ndn_service_framework::ServiceUser(face, group_prefix, identityCert, attrAuthorityCertificate, trustSchemaPath),
    
        
    m_ObjectDetectionServiceStub(*this)
        
    
{
    init();
}

void muas::ServiceUser_Drone::init()
{
    std::string regex_str = "^(<>*)<NDNSF><RESPONSE>" + ndn_service_framework::NameToRegexString(identity) + "(<>)(<>)(<>)$";
    NDN_LOG_INFO(regex_str);

    m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                std::bind(&ServiceUser_Drone::OnResponse, this, _1),
                                false);
}

void muas::ServiceUser_Drone::PublishRequest(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    ndn::Name requestName = ndn_service_framework::makeRequestName(identity, serviceProviderName, ServiceName, FunctionName, RequestID);
    ndn::Name requestNameWithoutPrefix = ndn_service_framework::makeRequestNameWithoutPrefix(serviceProviderName, ServiceName, FunctionName, RequestID);
   
    NDN_LOG_INFO(requestName);

    ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
    ndn::Name serviceAttributeName("/SERVICE");
    serviceAttributeName.append(serviceProviderName).append(ServiceName).append(FunctionName);
    std::vector<std::string> attributes;
    attributes = {serviceAttributeName.toUri()};

    std::tie(contentData, ckData) =
        responseProduer.produce(requestNameWithoutPrefix, attributes, ndn::span<const uint8_t>{buffer.begin(),buffer.end()}, m_signingInfo);
    // serve data
    for (auto data : contentData){
        NDN_LOG_INFO(data->getName());
        m_IMS.insert(*data);
    }
        
    for (auto data : ckData)
        m_IMS.insert(*data);
    m_svsps->publish(requestName);
    //m_svsps->publish(ckData.at(0)->getName().getPrefix(-1));
}

void muas::ServiceUser_Drone::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
{
        ndn::Name ServiceProviderName, RequesterName, ServiceName, FunctionName, RequestId;
        std::tie(ServiceProviderName, RequesterName, ServiceName, FunctionName, RequestId) =
            ndn_service_framework::parseResponseName(subscription.name);
        NDN_LOG_INFO("OnResponse: " << ServiceProviderName << RequesterName << ServiceName << FunctionName << RequestId);

        // decrypt the request message with nac-abe; if cannot be decrypted
        
        if (ServiceName.equals(m_ObjectDetectionServiceStub.serviceName))
        {
            requestConsumer.consume(subscription.name,
                                    std::bind(&muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback, &m_ObjectDetectionServiceStub, ServiceProviderName, ServiceName, FunctionName, RequestId, _1),
                                    std::bind(&ServiceUser_Drone::OnResponseDecryptionErrorCallback, this, ServiceProviderName, ServiceName, FunctionName, RequestId, _1));
        }
        
}

void muas::ServiceUser_Drone::OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string &)
{
        NDN_LOG_ERROR("OnResponseDecryptionErrorCallback: " << serviceProviderName << ServiceName << FunctionName);
}