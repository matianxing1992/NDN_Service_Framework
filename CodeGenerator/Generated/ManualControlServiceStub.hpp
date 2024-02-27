#ifndef ManualControlServiceStub_HPP
#define ManualControlServiceStub_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>


#include "ndn-service-framework/ServiceUser.hpp"
#include "messages.pb.h"

#include <iostream>
#include <string>
#include <regex>

namespace muas
{
    
    using Takeoff_Callback = std::function<void(const muas::ManualControl_Takeoff_Response &)>;
    
    using Land_Callback = std::function<void(const muas::ManualControl_Land_Response &)>;
    

    class ManualControlServiceStub : public ndn_service_framework::ServiceStub
    {
    public:
        ManualControlServiceStub(ndn_service_framework::ServiceUser& user);
        ~ManualControlServiceStub() {}

        
        void Takeoff_Async(const ndn::Name& provider, const muas::ManualControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback);
        
        void Land_Async(const ndn::Name& provider, const muas::ManualControl_Land_Request &_request, muas::Land_Callback _callback);
              

        void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;


    public:
        
        std::map<ndn::Name,Takeoff_Callback> Takeoff_Callbacks;
        
        std::map<ndn::Name,Land_Callback> Land_Callbacks;
        
        
        std::set<std::shared_ptr<ndn::Regex>> regexSet{
            
                
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ManualControl><ManualControl>(<>)"),
                
            
                
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ManualControl><ManualControl>(<>)")
                
            
        };

        ndn::Name serviceName;
    };
}

#endif