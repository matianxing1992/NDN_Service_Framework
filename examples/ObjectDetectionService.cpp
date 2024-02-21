#include "ObjectDetectionService.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ObjectDetectionService);


    void ObjectDetectionService::OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer& buffer)
   {
        // ask service instance to deal with the request message and return a response messsage
        // publish the request message
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: "<<requesterIdentity<<ServiceName<<FunctionName);
        if(ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: ObjectDetection YOLOv8");
            //objectDetectionService.Yolov8()
            muas::ObjectDetection_YOLOv8_Request _request;
            if(_request.ParseFromArray(buffer.data(),buffer.size()))
            {
                NDN_LOG_INFO("nRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Request parse success");
                muas::ObjectDetection_YOLOv8_Response _response;
                YOLOv8(requesterIdentity,_request,_response);
                std::string buffer="";
                _response.SerializeToString(&buffer);
                ndn::Buffer b(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
                //m_ServiceProvider
                //_response.SerializeToArray()
                m_ServiceProvider->PublishResponse(requesterIdentity,ServiceName,FunctionName,RequestID,b);
                }else{
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ObjectDetection_Yolov8_Request parse failed");
            }
            
        }
        
    }

    void ObjectDetectionService::YOLOv8(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response)
    {
        NDN_LOG_INFO("YOLOv8 request: " << _request.DebugString());
        muas::YOLOv8_DetectionResult *result = _response.add_results();
        result->set_classification(8);
        result->set_x_1(1);
        result->set_y_1(2);
        result->set_x_2(100);
        result->set_y_2(200);
    }

    void ObjectDetectionService::YOLOv8_M(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response)
    {
        muas::YOLOv8_DetectionResult *result = _response.add_results();
        result->set_classification(8);
        result->set_x_1(1);
        result->set_y_1(2);
        result->set_x_2(100);
        result->set_y_2(200);
    }
}