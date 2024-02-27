#ifndef ObjectDetectionServiceStub_HPP
#define ObjectDetectionServiceStub_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>


#include "ndn-service-framework/ServiceUser.hpp"
#include "messages.pb.h"

#include <iostream>
#include <string>
#include <regex>

namespace muas
{
    
    using YOLOv8_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Response &)>;
    
    using YOLOv8_S_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Response &)>;
    

    class ObjectDetectionServiceStub : public ndn_service_framework::ServiceStub
    {
    public:
        ObjectDetectionServiceStub(ndn_service_framework::ServiceUser& user);
        ~ObjectDetectionServiceStub() {}

        
        void YOLOv8_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback);
        
        void YOLOv8_S_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback);
              

        void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;


    public:
        
        std::map<ndn::Name,YOLOv8_Callback> YOLOv8_Callbacks;
        
        std::map<ndn::Name,YOLOv8_S_Callback> YOLOv8_S_Callbacks;
        
        
        std::set<std::shared_ptr<ndn::Regex>> regexSet{
            
                
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ObjectDetection><ObjectDetection>(<>)"),
                
            
                
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ObjectDetection><ObjectDetection>(<>)")
                
            
        };

        ndn::Name serviceName;
    };
}

#endif