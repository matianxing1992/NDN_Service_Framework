#ifndef FlightControlServiceStub_HPP
#define FlightControlServiceStub_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>


#include "ndn-service-framework/ServiceUser.hpp"
#include "messages.pb.h"

#include <iostream>
#include <string>
#include <regex>

namespace muas
{
    
    using Takeoff_Callback = std::function<void(const muas::FlightControl_Takeoff_Response &)>;
    using Takeoff_Timeout_Callback = std::function<void(const muas::FlightControl_Takeoff_Request &)>;
    
    using Land_Callback = std::function<void(const muas::FlightControl_Land_Response &)>;
    using Land_Timeout_Callback = std::function<void(const muas::FlightControl_Land_Request &)>;
    
    using ManualControl_Callback = std::function<void(const muas::FlightControl_ManualControl_Response &)>;
    using ManualControl_Timeout_Callback = std::function<void(const muas::FlightControl_ManualControl_Request &)>;
    

    class FlightControlServiceStub : public ndn_service_framework::ServiceStub
    {
    public:
        FlightControlServiceStub(ndn_service_framework::ServiceUser& user);
        virtual ~FlightControlServiceStub();

        
        void Takeoff_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback, muas::Takeoff_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy);
        
        void Land_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Land_Request &_request, muas::Land_Callback _callback, muas::Land_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy);
        
        void ManualControl_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_ManualControl_Request &_request, muas::ManualControl_Callback _callback, muas::ManualControl_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy);
              

        void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;


    public:
        std::map<ndn::Name,size_t> strategyMap;
        
        std::map<ndn::Name,Takeoff_Callback> Takeoff_Callbacks;
        std::map<ndn::Name,Takeoff_Timeout_Callback> Takeoff_Timeout_Callbacks;
        
        std::map<ndn::Name,Land_Callback> Land_Callbacks;
        std::map<ndn::Name,Land_Timeout_Callback> Land_Timeout_Callbacks;
        
        std::map<ndn::Name,ManualControl_Callback> ManualControl_Callbacks;
        std::map<ndn::Name,ManualControl_Timeout_Callback> ManualControl_Timeout_Callbacks;
        
        ndn::Name serviceName;
    };
}

#endif