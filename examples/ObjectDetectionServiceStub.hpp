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
    using YOLOv8_M_Callback = std::function<void(const muas::ObjectDetection_YOLOv8_Response &)>;

    class ObjectDetectionServiceStub : public ndn_service_framework::ServiceStub
    {
    public:
        ObjectDetectionServiceStub(ndn_service_framework::ServiceUser& user);
        ~ObjectDetectionServiceStub() {}

        void YOLOv8_Async(const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback, const ndn::Name& provider);
        
        void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer) override;

        void YOLOv8_M_ASync(const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_M_Callback _callback);

    public:

        std::map<ndn::Name,YOLOv8_Callback> YOLOv8_Callbacks;
        std::set<std::shared_ptr<ndn::Regex>> regexSet{
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ObjectDetection><Yolov8>(<>)"),
            std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>*)<ObjectDetection><Yolov8_M>(<>)")};

        ndn::Name serviceName;
    };
}

#endif