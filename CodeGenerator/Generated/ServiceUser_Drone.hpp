#ifndef EXAMPLE_SERVICE_USER_Drone_HPP
#define EXAMPLE_SERVICE_USER_Drone_HPP

#include <ndn-service-framework/ServiceUser.hpp>
#include <ndn-service-framework/NDNSFMessages.hpp>

#include "./ObjectDetectionServiceStub.hpp"



namespace muas
{
    class ServiceUser_Drone : public ndn_service_framework::ServiceUser
    {
    public:
        ServiceUser_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
        virtual ~ServiceUser_Drone();

        
        void YOLOv8_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback,  muas::YOLOv8_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_ObjectDetectionServiceStub.YOLOv8_Async(providers, _request, _callback, _timeout_callback, timeout_ms, strategy);
        }
        
        void YOLOv8_S_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback,  muas::YOLOv8_S_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_ObjectDetectionServiceStub.YOLOv8_S_Async(providers, _request, _callback, _timeout_callback, timeout_ms, strategy);
        }
        

      
    protected:
        
        void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription) override;
        
    private:
        
        muas::ObjectDetectionServiceStub m_ObjectDetectionServiceStub;
        
    };
}

#endif