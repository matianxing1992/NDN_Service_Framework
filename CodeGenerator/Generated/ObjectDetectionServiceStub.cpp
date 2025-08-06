#include "./ObjectDetectionServiceStub.hpp"

NDN_LOG_INIT(muas.ObjectDetectionServiceStub);

muas::ObjectDetectionServiceStub::ObjectDetectionServiceStub(ndn_service_framework::ServiceUser &user)
    : ndn_service_framework::ServiceStub(user),
      serviceName("ObjectDetection")
{
}

muas::ObjectDetectionServiceStub::~ObjectDetectionServiceStub(){}


void muas::ObjectDetectionServiceStub::YOLOv8_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_Callback _callback, muas::YOLOv8_Timeout_Callback _timeout_callback, int timeout_ms, const size_t strategy)
{
    NDN_LOG_INFO("YOLOv8_Async "<<"provider:"<<providers.size()<<" request:"<<_request.DebugString());
    muas::ObjectDetection_YOLOv8_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer payload(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(providers, ndn::Name("ObjectDetection"), ndn::Name("YOLOv8"), requestId, payload, strategy);
    YOLOv8_Callbacks.emplace(requestId, _callback);
    YOLOv8_Timeout_Callbacks.emplace(requestId, _timeout_callback);
    strategyMap.emplace(requestId, strategy);
    
    m_scheduler.schedule(ndn::time::milliseconds(timeout_ms), [this, requestId, _request, _timeout_callback] { 
        // time out
        this->YOLOv8_Callbacks.erase(requestId);
        _timeout_callback(_request);
    });
}

void muas::ObjectDetectionServiceStub::YOLOv8_S_Async(const std::vector<ndn::Name>& providers, const muas::ObjectDetection_YOLOv8_Request &_request, muas::YOLOv8_S_Callback _callback, muas::YOLOv8_S_Timeout_Callback _timeout_callback, int timeout_ms, const size_t strategy)
{
    NDN_LOG_INFO("YOLOv8_S_Async "<<"provider:"<<providers.size()<<" request:"<<_request.DebugString());
    muas::ObjectDetection_YOLOv8_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer payload(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(providers, ndn::Name("ObjectDetection"), ndn::Name("YOLOv8_S"), requestId, payload, strategy);
    YOLOv8_S_Callbacks.emplace(requestId, _callback);
    YOLOv8_S_Timeout_Callbacks.emplace(requestId, _timeout_callback);
    strategyMap.emplace(requestId, strategy);
    
    m_scheduler.schedule(ndn::time::milliseconds(timeout_ms), [this, requestId, _request, _timeout_callback] { 
        // time out
        this->YOLOv8_S_Callbacks.erase(requestId);
        _timeout_callback(_request);
    });
}


void muas::ObjectDetectionServiceStub::OnResponseDecryptionSuccessCallback(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID);

    // parse Response Message from buffer
    ndn_service_framework::ResponseMessage responseMessage;
    responseMessage.WireDecode(ndn::Block(buffer));
    responseMessage.getErrorInfo();

    ndn::Buffer payload = responseMessage.getPayload();

    
    if (ServiceName.equals(ndn::Name("ObjectDetection")) & FunctionName.equals(ndn::Name("YOLOv8")))
    {
        
        // ObjectDetectionService.YOLOv8()
        muas::ObjectDetection_YOLOv8_Response _response;
        if (_response.ParseFromArray(payload.data(), payload.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse success");
            auto it = YOLOv8_Callbacks.find(RequestID);
            if (it != YOLOv8_Callbacks.end())
            {
                it->second(_response);
                // find strategy in the strategyMap using RequestID, and check whether it's ndn_service_framework::tlv:NoCoordination
                // if yes, then remove the callback from the map, otherwise, do nothing.
                auto strategyIt = strategyMap.find(RequestID);
                if (strategyIt != strategyMap.end())
                {
                    if (strategyIt->second != ndn_service_framework::tlv::NoCoordination)
                    {
                        strategyMap.erase(strategyIt);
                        YOLOv8_Callbacks.erase(it);
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
                    }else{
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Keep callback for ndn_service_framework::tlv::NoCoordination");
                    }
                    // remove timeout callback if receive any response
                    YOLOv8_Timeout_Callbacks.erase(RequestID);
                }
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
        if (_response.ParseFromArray(payload.data(), payload.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse success");
            auto it = YOLOv8_S_Callbacks.find(RequestID);
            if (it != YOLOv8_S_Callbacks.end())
            {
                it->second(_response);
                // find strategy in the strategyMap using RequestID, and check whether it's ndn_service_framework::tlv:NoCoordination
                // if yes, then remove the callback from the map, otherwise, do nothing.
                auto strategyIt = strategyMap.find(RequestID);
                if (strategyIt != strategyMap.end())
                {
                    if (strategyIt->second != ndn_service_framework::tlv::NoCoordination)
                    {
                        strategyMap.erase(strategyIt);
                        YOLOv8_S_Callbacks.erase(it);
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
                    }else{
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Keep callback for ndn_service_framework::tlv::NoCoordination");
                    }
                    // remove timeout callback if receive any response
                    YOLOv8_S_Timeout_Callbacks.erase(RequestID);
                }
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::ObjectDetection_YOLOv8_Response parse failed");
        }
    }
    
}