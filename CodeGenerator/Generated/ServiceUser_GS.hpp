#ifndef EXAMPLE_SERVICE_USER_GS_HPP
#define EXAMPLE_SERVICE_USER_GS_HPP

#include <ndn-service-framework/ServiceUser.hpp>

#include "./ObjectDetectionServiceStub.hpp"

#include "./ManualControlServiceStub.hpp"



namespace muas
{
    class ServiceUser_GS : public ndn_service_framework::ServiceUser
    {
    public:
        ServiceUser_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
        ~ServiceUser_GS() {}

        
        void YOLOv8_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback)
        {
            m_ObjectDetectionServiceStub.YOLOv8_Async(provider, _request, _callback);
        }
        
        void YOLOv8_S_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback)
        {
            m_ObjectDetectionServiceStub.YOLOv8_S_Async(provider, _request, _callback);
        }
        
        void Takeoff_Async(const ndn::Name& provider, const muas::ManualControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback)
        {
            m_ManualControlServiceStub.Takeoff_Async(provider, _request, _callback);
        }
        
        void Land_Async(const ndn::Name& provider, const muas::ManualControl_Land_Request &_request, muas::Land_Callback _callback)
        {
            m_ManualControlServiceStub.Land_Async(provider, _request, _callback);
        }
        

        void PublishRequest(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;

    protected:

        void init();
        
        void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription) override;

        void OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, const std::string&) override;

    private:
        
        muas::ObjectDetectionServiceStub m_ObjectDetectionServiceStub;
        
        muas::ManualControlServiceStub m_ManualControlServiceStub;
        
    };
}

#endif