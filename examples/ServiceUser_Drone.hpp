#ifndef EXAMPLE_SERVICE_USER_Drone_HPP
#define EXAMPLE_SERVICE_USER_Drone_HPP

#include <ndn-service-framework/ServiceUser.hpp>

#include "./ObjectDetectionServiceStub.hpp"



namespace muas
{
    class ServiceUser_Drone : public ndn_service_framework::ServiceUser
    {
    public:
        ServiceUser_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
        ~ServiceUser_Drone() {}

        
        void YOLOv8_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback)
        {
            m_ObjectDetectionServiceStub.YOLOv8_Async(provider, _request, _callback);
        }
        
        void YOLOv8_S_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback)
        {
            m_ObjectDetectionServiceStub.YOLOv8_S_Async(provider, _request, _callback);
        }
        

        void PublishRequest(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;

    protected:

        void init();
        
        void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription) override;

        void OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, const std::string&) override;

    private:
        
        muas::ObjectDetectionServiceStub m_ObjectDetectionServiceStub;
        
    };
}

#endif