#include "./ManualControlServiceStub.hpp"

NDN_LOG_INIT(muas.ManualControlServiceStub);

muas::ManualControlServiceStub::ManualControlServiceStub(ndn_service_framework::ServiceUser &user)
    : ndn_service_framework::ServiceStub(user),
      serviceName("ManualControl")
{
}


void muas::ManualControlServiceStub::Takeoff_Async(const ndn::Name& provider, const muas::ManualControl_Takeoff_Request &_request, muas::Takeoff_Callback _callback)
{
    NDN_LOG_INFO("Takeoff_Async "<<"provider:"<<provider<<" request:"<<_request.DebugString());
    muas::ManualControl_Takeoff_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer b(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(provider, ndn::Name("ManualControl"), ndn::Name("Takeoff"), requestId, b);
    Takeoff_Callbacks.emplace(requestId, _callback);
}

void muas::ManualControlServiceStub::Land_Async(const ndn::Name& provider, const muas::ManualControl_Land_Request &_request, muas::Land_Callback _callback)
{
    NDN_LOG_INFO("Land_Async "<<"provider:"<<provider<<" request:"<<_request.DebugString());
    muas::ManualControl_Land_Response response;
    std::string buffer = "";
    _request.SerializeToString(&buffer);
    ndn::Buffer b(buffer.begin(),buffer.end());
    ndn::Name requestId(ndn::time::toIsoString(ndn::time::system_clock::now()));
    m_user->PublishRequest(provider, ndn::Name("ManualControl"), ndn::Name("Land"), requestId, b);
    Land_Callbacks.emplace(requestId, _callback);
}


void muas::ManualControlServiceStub::OnResponseDecryptionSuccessCallback(const ndn::Name &serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
{
    NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID);
    
    if (ServiceName.equals(ndn::Name("ManualControl")) & FunctionName.equals(ndn::Name("Takeoff")))
    {
        
        // ManualControlService.Takeoff()
        muas::ManualControl_Takeoff_Response _response;
        if (_response.ParseFromArray(buffer.data(), buffer.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ManualControl_Takeoff_Response parse success");
            auto it = Takeoff_Callbacks.find(RequestID);
            if (it != Takeoff_Callbacks.end())
            {
                it->second(_response);
                Takeoff_Callbacks.erase(it);
                NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::ManualControl_Takeoff_Response parse failed");
        }
    }
    
    if (ServiceName.equals(ndn::Name("ManualControl")) & FunctionName.equals(ndn::Name("Land")))
    {
        
        // ManualControlService.Land()
        muas::ManualControl_Land_Response _response;
        if (_response.ParseFromArray(buffer.data(), buffer.size()))
        {
            NDN_LOG_INFO("OnResponseDecryptionSuccessCallback muas::ManualControl_Land_Response parse success");
            auto it = Land_Callbacks.find(RequestID);
            if (it != Land_Callbacks.end())
            {
                it->second(_response);
                Land_Callbacks.erase(it);
                NDN_LOG_INFO("OnResponseDecryptionSuccessCallback: Remove used callback");
            }
        }
        else
        {
            NDN_LOG_ERROR("OnResponseDecryptionSuccessCallback muas::ManualControl_Land_Response parse failed");
        }
    }
    
}