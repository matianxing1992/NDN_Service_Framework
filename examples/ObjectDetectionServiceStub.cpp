#include "ObjectDetectionServiceStub.hpp"

NDN_LOG_INIT(muas.ObjectDetectionServiceStub);

muas::ObjectDetectionServiceStub::ObjectDetectionServiceStub(ndn_service_framework::ServiceUser &user)
    : ndn_service_framework::ServiceStub(user),
      serviceName("ObjectDetection")
{
}

void muas::ObjectDetectionServiceStub::YOLOv8_Async(const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback,const ndn::Name& provider)
{
    muas::ObjectDetection_YOLOv8_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer b(buffer.begin(),buffer.end());
    // ndn::time::nanoseconds timestamp = ndn::time::toUnixTimestamp<ndn::time::nanoseconds>(ndn::time::system_clock::now());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(provider, ndn::Name("ObjectDetection"), ndn::Name("YOLOv8"), requestId, b);
    YOLOv8_Callbacks.emplace(requestId, _callback);
    //_callback(response);
}

void muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID);
    if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8")))
    {
        
        // objectDetectionService.Yolov8()
        muas::ObjectDetection_YOLOv8_Response _response;
        if (_response.ParseFromArray(buffer.data(), buffer.size()))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse success");
            auto it = YOLOv8_Callbacks.find(RequestID);
            if (it != YOLOv8_Callbacks.end())
            {
                it->second(_response);
                YOLOv8_Callbacks.erase(it);
                NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: Remove used callback");
            }

            // m_ServiceProvider
            //_response.SerializeToArray()
            // m_ServiceProvider->PublishResponse(requesterIdentity,ServiceName,FunctionName,RequestID,b);
        }
        else
        {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse failed");
        }
    }
}

void muas::ObjectDetectionServiceStub::YOLOv8_M_ASync(const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_M_Callback _callback)
{
    // muas::ObjectDetection_YOLOv8_Response response;
    // _callback(response);
}