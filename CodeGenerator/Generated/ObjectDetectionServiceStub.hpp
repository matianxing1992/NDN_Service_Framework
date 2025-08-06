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
    using YOLOv8_Timeout_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Request &)>;
    
    using YOLOv8_S_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Response &)>;
    using YOLOv8_S_Timeout_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Request &)>;
    

    class ObjectDetectionServiceStub : public ndn_service_framework::ServiceStub
    {
    public:
        ObjectDetectionServiceStub(ndn_service_framework::ServiceUser& user);
        virtual ~ObjectDetectionServiceStub();

        
        void YOLOv8_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback, muas::YOLOv8_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy);
        
        void YOLOv8_S_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback, muas::YOLOv8_S_Timeout_Callback _timeout_callback, int timeout_ms,  const size_t strategy);
              

        void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;


    public:
        std::map<ndn::Name,size_t> strategyMap;
        
        std::map<ndn::Name,YOLOv8_Callback> YOLOv8_Callbacks;
        std::map<ndn::Name,YOLOv8_Timeout_Callback> YOLOv8_Timeout_Callbacks;
        
        std::map<ndn::Name,YOLOv8_S_Callback> YOLOv8_S_Callbacks;
        std::map<ndn::Name,YOLOv8_S_Timeout_Callback> YOLOv8_S_Timeout_Callbacks;
        
        ndn::Name serviceName;
    };
}

#endif