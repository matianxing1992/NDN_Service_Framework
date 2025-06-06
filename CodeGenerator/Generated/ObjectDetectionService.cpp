#include "./ObjectDetectionService.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ObjectDetectionService);

    ObjectDetectionService::~ObjectDetectionService() {}

    void ObjectDetectionService::ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage)
    {
        // log the parameters
        NDN_LOG_INFO("ConsumeRequest: RequesterName: " << RequesterName << " providerName: " << providerName << " ServiceName: " << ServiceName << " FunctionName: " << FunctionName << " RequestID: " << RequestID);
        
        //the payload of the request message is a protobuf message, which is deserialized by the following code:
        ndn::Buffer payload = requestMessage.getPayload();

        
        if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} YOLOv8");
            muas::ObjectDetection_YOLOv8_Request _request;
            if (_request.ParseFromArray(payload.data(), payload.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Request parse success");
                muas::ObjectDetection_YOLOv8_Response _response;
                YOLOv8(RequesterName, _request, _response);
                std::string buffer = "";
                _response.SerializeToString(&buffer);
                ndn::Buffer resPayload(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
                // make ResponseMessage and publish it
                ndn_service_framework::ResponseMessage responseMessage;
                responseMessage.setStatus(true);
                responseMessage.setErrorInfo("No error");
                responseMessage.setPayload(const_cast<ndn::Buffer&>(resPayload), resPayload.size());

                // make response name and response name without prefix
                ndn::Name responseName = ndn_service_framework::makeResponseName(providerName, RequesterName, ServiceName, FunctionName, RequestID);
                ndn::Name responseNameWithoutPrefix = ndn_service_framework::makeResponseNameWithoutPrefix(RequesterName, ServiceName, FunctionName, RequestID);
                m_ServiceProvider->PublishMessage(responseName, responseNameWithoutPrefix, responseMessage);
            }
            else
            {
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Request parse failed");
            }
        }
        
        if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8_S")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} YOLOv8_S");
            muas::ObjectDetection_YOLOv8_Request _request;
            if (_request.ParseFromArray(payload.data(), payload.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Request parse success");
                muas::ObjectDetection_YOLOv8_Response _response;
                YOLOv8_S(RequesterName, _request, _response);
                std::string buffer = "";
                _response.SerializeToString(&buffer);
                ndn::Buffer resPayload(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
                // make ResponseMessage and publish it
                ndn_service_framework::ResponseMessage responseMessage;
                responseMessage.setStatus(true);
                responseMessage.setErrorInfo("No error");
                responseMessage.setPayload(const_cast<ndn::Buffer&>(resPayload), resPayload.size());

                // make response name and response name without prefix
                ndn::Name responseName = ndn_service_framework::makeResponseName(providerName, RequesterName, ServiceName, FunctionName, RequestID);
                ndn::Name responseNameWithoutPrefix = ndn_service_framework::makeResponseNameWithoutPrefix(RequesterName, ServiceName, FunctionName, RequestID);
                m_ServiceProvider->PublishMessage(responseName, responseNameWithoutPrefix, responseMessage);
            }
            else
            {
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Request parse failed");
            }
        }
        

    }

    
    void ObjectDetectionService::YOLOv8(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response)
    {
        NDN_LOG_INFO("YOLOv8 request: " << _request.DebugString());
        // RPC logic starts here
        if (YOLOv8_Handler) {
            YOLOv8_Handler(requesterIdentity, _request, _response);
        } else {
            NDN_LOG_ERROR("No YOLOv8 handler set.");
        }

        // RPC logic ends here
    }
    
    void ObjectDetectionService::YOLOv8_S(const ndn::Name &requesterIdentity, const muas::ObjectDetection_YOLOv8_Request &_request, muas::ObjectDetection_YOLOv8_Response &_response)
    {
        NDN_LOG_INFO("YOLOv8_S request: " << _request.DebugString());
        // RPC logic starts here
        if (YOLOv8_S_Handler) {
            YOLOv8_S_Handler(requesterIdentity, _request, _response);
        } else {
            NDN_LOG_ERROR("No YOLOv8_S handler set.");
        }

        // RPC logic ends here
    }
    
}