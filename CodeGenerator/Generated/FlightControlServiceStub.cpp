#include "./FlightControlServiceStub.hpp"

NDN_LOG_INIT(muas.FlightControlServiceStub);

muas::FlightControlServiceStub::FlightControlServiceStub(ndn::Face& face, ndn_service_framework::ServiceUser &user)
    : ndn_service_framework::ServiceStub(face, user),
      serviceName("FlightControl")
{
}

muas::FlightControlServiceStub::~FlightControlServiceStub(){}


void muas::FlightControlServiceStub::Takeoff_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback, muas::Takeoff_Timeout_Callback _timeout_callback, int timeout_ms, const size_t strategy)
{
    NDN_LOG_INFO("Takeoff_Async "<<"provider:"<<providers.size()<<" request:"<<_request.DebugString());
    muas::FlightControl_Takeoff_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer payload(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(providers, ndn::Name("FlightControl"), ndn::Name("Takeoff"), requestId, payload, strategy);
    Takeoff_Callbacks.emplace(requestId, _callback);
    Takeoff_Timeout_Callbacks.emplace(requestId, _timeout_callback);
    strategyMap.emplace(requestId, strategy);
    
    m_scheduler.schedule(ndn::time::milliseconds(timeout_ms), [this, requestId, _request] { 
        // time out
        this->Takeoff_Callbacks.erase(requestId);
        // check if timeout_callback is still valid
        auto it = Takeoff_Timeout_Callbacks.find(requestId);
        if (it != Takeoff_Timeout_Callbacks.end()) {
            it->second(_request);
        }
    });
}

void muas::FlightControlServiceStub::Land_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_Land_Request &_request, muas::Land_Callback _callback, muas::Land_Timeout_Callback _timeout_callback, int timeout_ms, const size_t strategy)
{
    NDN_LOG_INFO("Land_Async "<<"provider:"<<providers.size()<<" request:"<<_request.DebugString());
    muas::FlightControl_Land_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer payload(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(providers, ndn::Name("FlightControl"), ndn::Name("Land"), requestId, payload, strategy);
    Land_Callbacks.emplace(requestId, _callback);
    Land_Timeout_Callbacks.emplace(requestId, _timeout_callback);
    strategyMap.emplace(requestId, strategy);
    
    m_scheduler.schedule(ndn::time::milliseconds(timeout_ms), [this, requestId, _request] { 
        // time out
        this->Land_Callbacks.erase(requestId);
        // check if timeout_callback is still valid
        auto it = Land_Timeout_Callbacks.find(requestId);
        if (it != Land_Timeout_Callbacks.end()) {
            it->second(_request);
        }
    });
}

void muas::FlightControlServiceStub::ManualControl_Async(const std::vector<ndn::Name>& providers, const muas::FlightControl_ManualControl_Request &_request, muas::ManualControl_Callback _callback, muas::ManualControl_Timeout_Callback _timeout_callback, int timeout_ms, const size_t strategy)
{
    NDN_LOG_INFO("ManualControl_Async "<<"provider:"<<providers.size()<<" request:"<<_request.DebugString());
    muas::FlightControl_ManualControl_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer payload(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(providers, ndn::Name("FlightControl"), ndn::Name("ManualControl"), requestId, payload, strategy);
    ManualControl_Callbacks.emplace(requestId, _callback);
    ManualControl_Timeout_Callbacks.emplace(requestId, _timeout_callback);
    strategyMap.emplace(requestId, strategy);
    
    m_scheduler.schedule(ndn::time::milliseconds(timeout_ms), [this, requestId, _request] { 
        // time out
        this->ManualControl_Callbacks.erase(requestId);
        // check if timeout_callback is still valid
        auto it = ManualControl_Timeout_Callbacks.find(requestId);
        if (it != ManualControl_Timeout_Callbacks.end()) {
            it->second(_request);
        }
    });
}


void muas::FlightControlServiceStub::OnResponseDecryptionSuccessCallback(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID);

    // parse Response Message from buffer
    ndn_service_framework::ResponseMessage responseMessage;
    responseMessage.WireDecode(ndn::Block(buffer));
    responseMessage.getErrorInfo();

    ndn::Buffer payload = responseMessage.getPayload();

    
    if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("Takeoff")))
    {
        
        // FlightControlService.Takeoff()
        muas::FlightControl_Takeoff_Response _response;
        if (_response.ParseFromArray(payload.data(), payload.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::FlightControl_Takeoff_Response parse success");
            auto it = Takeoff_Callbacks.find(RequestID);
            if (it != Takeoff_Callbacks.end())
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
                        Takeoff_Callbacks.erase(it);
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
                    }else{
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Keep callback for ndn_service_framework::tlv::NoCoordination");
                    }
                    // remove timeout callback if receive any response
                    Takeoff_Timeout_Callbacks.erase(RequestID);
                }
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::FlightControl_Takeoff_Response parse failed");
        }
    }
    
    if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("Land")))
    {
        
        // FlightControlService.Land()
        muas::FlightControl_Land_Response _response;
        if (_response.ParseFromArray(payload.data(), payload.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::FlightControl_Land_Response parse success");
            auto it = Land_Callbacks.find(RequestID);
            if (it != Land_Callbacks.end())
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
                        Land_Callbacks.erase(it);
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
                    }else{
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Keep callback for ndn_service_framework::tlv::NoCoordination");
                    }
                    // remove timeout callback if receive any response
                    Land_Timeout_Callbacks.erase(RequestID);
                }
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::FlightControl_Land_Response parse failed");
        }
    }
    
    if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("ManualControl")))
    {
        
        // FlightControlService.ManualControl()
        muas::FlightControl_ManualControl_Response _response;
        if (_response.ParseFromArray(payload.data(), payload.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::FlightControl_ManualControl_Response parse success");
            auto it = ManualControl_Callbacks.find(RequestID);
            if (it != ManualControl_Callbacks.end())
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
                        ManualControl_Callbacks.erase(it);
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
                    }else{
                        NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Keep callback for ndn_service_framework::tlv::NoCoordination");
                    }
                    // remove timeout callback if receive any response
                    ManualControl_Timeout_Callbacks.erase(RequestID);
                }
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::FlightControl_ManualControl_Response parse failed");
        }
    }
    
}