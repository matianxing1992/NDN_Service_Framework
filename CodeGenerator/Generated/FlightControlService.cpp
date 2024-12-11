#include "./FlightControlService.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.FlightControlService);

    FlightControlService::~FlightControlService() {}

    void FlightControlService::ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage)
    {
        // log the parameters
        NDN_LOG_INFO("ConsumeRequest: RequesterName: " << RequesterName << " providerName: " << providerName << " ServiceName: " << ServiceName << " FunctionName: " << FunctionName << " RequestID: " << RequestID);
        
        //the payload of the request message is a protobuf message, which is deserialized by the following code:
        ndn::Buffer payload = requestMessage.getPayload();

        
        if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("Takeoff")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} Takeoff");
            muas::FlightControl_Takeoff_Request _request;
            if (_request.ParseFromArray(payload.data(), payload.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::FlightControl_Takeoff_Request parse success");
                muas::FlightControl_Takeoff_Response _response;
                Takeoff(RequesterName, _request, _response);
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
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::FlightControl_Takeoff_Request parse failed");
            }
        }
        
        if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("Land")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} Land");
            muas::FlightControl_Land_Request _request;
            if (_request.ParseFromArray(payload.data(), payload.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::FlightControl_Land_Request parse success");
                muas::FlightControl_Land_Response _response;
                Land(RequesterName, _request, _response);
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
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::FlightControl_Land_Request parse failed");
            }
        }
        
        if (ServiceName.equals(ndn::Name("FlightControl")) & FunctionName.equals(ndn::Name("ManualControl")))
        {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: {ServiceName} ManualControl");
            muas::FlightControl_ManualControl_Request _request;
            if (_request.ParseFromArray(payload.data(), payload.size()))
            {
                NDN_LOG_INFO("onRequestDecryptionSuccessCallback muas::FlightControl_ManualControl_Request parse success");
                muas::FlightControl_ManualControl_Response _response;
                ManualControl(RequesterName, _request, _response);
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
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback muas::FlightControl_ManualControl_Request parse failed");
            }
        }
        

    }

    
    void FlightControlService::Takeoff(const ndn::Name &requesterIdentity, const muas::FlightControl_Takeoff_Request &_request, muas::FlightControl_Takeoff_Response &_response)
    {
        NDN_LOG_INFO("Takeoff request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
    void FlightControlService::Land(const ndn::Name &requesterIdentity, const muas::FlightControl_Land_Request &_request, muas::FlightControl_Land_Response &_response)
    {
        NDN_LOG_INFO("Land request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
    void FlightControlService::ManualControl(const ndn::Name &requesterIdentity, const muas::FlightControl_ManualControl_Request &_request, muas::FlightControl_ManualControl_Response &_response)
    {
        NDN_LOG_INFO("ManualControl request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
}