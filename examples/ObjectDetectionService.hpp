#ifndef ObjectDetectionService_HPP
#define ObjectDetectionService_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceStub.hpp"
#include "ndn-service-framework/Service.hpp"

#include "messages.pb.h"

#include <vector>

namespace muas
{

    using Yolov8_Function = std::function<void(const ndn::Name &, const muas::ObjectDetection_YOLOv8_Request &, muas::ObjectDetection_YOLOv8_Response &)>;
    using Yolov8_M_Function = std::function<void(const ndn::Name &, const muas::ObjectDetection_YOLOv8_Request &, muas::ObjectDetection_YOLOv8_Response &)>;

    class ObjectDetectionService : public ndn_service_framework::Service
    {
    public:
        ObjectDetectionService(ndn_service_framework::ServiceProvider &serviceProvider)
            : ndn_service_framework::Service(serviceProvider),
              serviceName("ObjectDetection")
        {
        }

        ~ObjectDetectionService() {}

        virtual void OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &) override;

        void YOLOv8(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response);

        void YOLOv8_M(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response);

    public:
        std::set<std::shared_ptr<ndn::Regex>> regexSet{
            std::make_shared<ndn::Regex>("^(<>*)<NDNSF><REQUEST>(<>*)<ObjectDetection><YOLOv8>(<>)"),
            std::make_shared<ndn::Regex>("^(<>*)<NDNSF><REQUEST>(<>*)<ObjectDetection><YOLOv8_M>(<>)")};
        ndn::Name serviceName;
    };
}
#endif