#include "./ManualControlService.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ManualControlService);

    void ManualControlService::OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
    {
        // ask service instance to deal with the request message and return a response messsage
        // publish the request message
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: " << requesterIdentity << ServiceName << FunctionName);

        
        if (ServiceName.equals(ndn::Name("ManualControl")) & FunctionName.equals(ndn::Name("Takeoff")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} Takeoff");
            muas::ManualControl_Takeoff_Request _request;
            if (_request.ParseFromArray(buffer.data(), buffer.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::ManualControl_Takeoff_Request parse success");
                muas::ManualControl_Takeoff_Response _response;
                Takeoff(requesterIdentity, _request, _response);
                std::string buffer = "";
                _response.SerializeToString(&buffer);
                ndn::Buffer b(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
                m_ServiceProvider->PublishResponse(requesterIdentity, ServiceName, FunctionName, RequestID, b);
            }
            else
            {
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ManualControl_Takeoff_Request parse failed");
            }
        }
        
        if (ServiceName.equals(ndn::Name("ManualControl")) & FunctionName.equals(ndn::Name("Land")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} Land");
            muas::ManualControl_Land_Request _request;
            if (_request.ParseFromArray(buffer.data(), buffer.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::ManualControl_Land_Request parse success");
                muas::ManualControl_Land_Response _response;
                Land(requesterIdentity, _request, _response);
                std::string buffer = "";
                _response.SerializeToString(&buffer);
                ndn::Buffer b(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
                m_ServiceProvider->PublishResponse(requesterIdentity, ServiceName, FunctionName, RequestID, b);
            }
            else
            {
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::ManualControl_Land_Request parse failed");
            }
        }
        


    }
    
    void ManualControlService::Takeoff(const ndn::Name &requesterIdentity, const muas::ManualControl_Takeoff_Request &_request, muas::ManualControl_Takeoff_Response &_response)
    {
        NDN_LOG_INFO("Takeoff request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
    void ManualControlService::Land(const ndn::Name &requesterIdentity, const muas::ManualControl_Land_Request &_request, muas::ManualControl_Land_Response &_response)
    {
        NDN_LOG_INFO("Land request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
}