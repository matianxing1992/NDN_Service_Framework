#ifndef FlightControlService_HPP
#define FlightControlService_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceStub.hpp"
#include "ndn-service-framework/Service.hpp"

#include "messages.pb.h"

#include <vector>

namespace muas
{

    
    using Takeoff_Function = std::function<void(const ndn::Name &, const muas::FlightControl_Takeoff_Request &, muas::FlightControl_Takeoff_Response &)>;
    
    using Land_Function = std::function<void(const ndn::Name &, const muas::FlightControl_Land_Request &, muas::FlightControl_Land_Response &)>;
    
    using ManualControl_Function = std::function<void(const ndn::Name &, const muas::FlightControl_ManualControl_Request &, muas::FlightControl_ManualControl_Response &)>;
    

    class FlightControlService : public ndn_service_framework::Service
    {
    public:
        FlightControlService(ndn_service_framework::ServiceProvider &serviceProvider)
            : ndn_service_framework::Service(serviceProvider),
              serviceName("FlightControl")
        {
        }

        virtual ~FlightControlService();

        void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage) override;
        
        void Takeoff(const ndn::Name &requesterIdentity, const muas::FlightControl_Takeoff_Request &_request, muas::FlightControl_Takeoff_Response &_response);
        
        void Land(const ndn::Name &requesterIdentity, const muas::FlightControl_Land_Request &_request, muas::FlightControl_Land_Response &_response);
        
        void ManualControl(const ndn::Name &requesterIdentity, const muas::FlightControl_ManualControl_Request &_request, muas::FlightControl_ManualControl_Response &_response);
        


    public:
        ndn::Name serviceName;
        
        Takeoff_Function Takeoff_Handler;
        
        Land_Function Land_Handler;
        
        ManualControl_Function ManualControl_Handler;
        
    };
}
#endif