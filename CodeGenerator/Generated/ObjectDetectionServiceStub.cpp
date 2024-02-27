#include "./ObjectDetectionServiceStub.hpp"

NDN_LOG_INIT(muas.ObjectDetectionServiceStub);

muas::ObjectDetectionServiceStub::ObjectDetectionServiceStub(ndn_service_framework::ServiceUser &user)
    : ndn_service_framework::ServiceStub(user),
      serviceName("ObjectDetection")
{
}


void muas::ObjectDetectionServiceStub::YOLOv8_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback)
{
    NDN_LOG_INFO("YOLOv8_Async "<<"provider:"<<provider<<" request:"<<_request.DebugString());
    muas::ObjectDetection_YOLOv8_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer b(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(provider, ndn::Name("ObjectDetection"), ndn::Name("YOLOv8"), requestId, b);
    YOLOv8_Callbacks.emplace(requestId, _callback);
}

void muas::ObjectDetectionServiceStub::YOLOv8_S_Async(const ndn::Name& provider, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback)
{
    NDN_LOG_INFO("YOLOv8_S_Async "<<"provider:"<<provider<<" request:"<<_request.DebugString());
    muas::ObjectDetection_YOLOv8_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer b(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(provider, ndn::Name("ObjectDetection"), ndn::Name("YOLOv8_S"), requestId, b);
    YOLOv8_S_Callbacks.emplace(requestId, _callback);
}


void muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID);
    
    if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8")))
    {
        
        // ObjectDetectionService.YOLOv8()
        muas::ObjectDetection_YOLOv8_Response _response;
        if (_response.ParseFromArray(buffer.data(), buffer.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse success");
            auto it = YOLOv8_Callbacks.find(RequestID);
            if (it != YOLOv8_Callbacks.end())
            {
                it->second(_response);
                YOLOv8_Callbacks.erase(it);
                NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse failed");
        }
    }
    
    if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8_S")))
    {
        
        // ObjectDetectionService.YOLOv8_S()
        muas::ObjectDetection_YOLOv8_Response _response;
        if (_response.ParseFromArray(buffer.data(), buffer.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse success");
            auto it = YOLOv8_S_Callbacks.find(RequestID);
            if (it != YOLOv8_S_Callbacks.end())
            {
                it->second(_response);
                YOLOv8_S_Callbacks.erase(it);
                NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse failed");
        }
    }
    
}