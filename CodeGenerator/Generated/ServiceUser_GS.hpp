#ifndef EXAMPLE_SERVICE_USER_GS_HPP
#define EXAMPLE_SERVICE_USER_GS_HPP

#include <ndn-service-framework/ServiceUser.hpp>
#include <ndn-service-framework/NDNSFMessages.hpp>

#include "./ObjectDetectionServiceStub.hpp"

#include "./FlightControlServiceStub.hpp"



namespace muas
{
    class ServiceUser_GS : public ndn_service_framework::ServiceUser
    {
    public:
        ServiceUser_GS(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
        virtual ~ServiceUser_GS();

        
        void YOLOv8_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_ObjectDetectionServiceStub.YOLOv8_Async(providers, _request, _callback, strategy);
        }
        
        void YOLOv8_S_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_ObjectDetectionServiceStub.YOLOv8_S_Async(providers, _request, _callback, strategy);
        }
        
        void Takeoff_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_FlightControlServiceStub.Takeoff_Async(providers, _request, _callback, strategy);
        }
        
        void Land_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Land_Request &_request, muas::Land_Callback _callback,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_FlightControlServiceStub.Land_Async(providers, _request, _callback, strategy);
        }
        
        void ManualControl_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_ManualControl_Request &_request, muas::ManualControl_Callback _callback,  const size_t strategy = ndn_service_framework::tlv::FirstResponding)
        {
            m_FlightControlServiceStub.ManualControl_Async(providers, _request, _callback, strategy);
        }
        

      
    protected:
        
        void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription) override;
        
    private:
        
        muas::ObjectDetectionServiceStub m_ObjectDetectionServiceStub;
        
        muas::FlightControlServiceStub m_FlightControlServiceStub;
        
    };
}

#endif