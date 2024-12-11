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

    
    using YOLOv8_Function = std::function<void(const ndn::Name &, const muas::ObjectDetection_YOLOv8_Request &, muas::ObjectDetection_YOLOv8_Response &)>;
    
    using YOLOv8_S_Function = std::function<void(const ndn::Name &, const muas::ObjectDetection_YOLOv8_Request &, muas::ObjectDetection_YOLOv8_Response &)>;
    

    class ObjectDetectionService : public ndn_service_framework::Service
    {
    public:
        ObjectDetectionService(ndn_service_framework::ServiceProvider &serviceProvider)
            : ndn_service_framework::Service(serviceProvider),
              serviceName("ObjectDetection")
        {
        }

        virtual ~ObjectDetectionService();

        void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage) override;
        
        void YOLOv8(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response);
        
        void YOLOv8_S(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response);
        


    public:
        ndn::Name serviceName;
    };
}
#endif