#include "./ServiceProvider_Drone.hpp"

namespace muas
{
    NDN_LOG_INIT(muas.ServiceProvider_Drone);
    ServiceProvider_Drone::ServiceProvider_Drone(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath)
        : ndn_service_framework::ServiceProvider(face, group_prefix, identityCert, attrAuthorityCertificate,  trustSchemaPath),
        m_ObjectDetectionService(*this),m_FlightControlService(*this)
    {
        
        this->m_serviceNames.push_back("ObjectDetection");
        
        this->m_serviceNames.push_back("FlightControl");
        
        init();
    }

    ServiceProvider_Drone::~ServiceProvider_Drone(){}

    void ServiceProvider_Drone::registerServiceInfo()
    {
        NDN_LOG_INFO("Registering services using NDNSD");
        ndnsd::discovery::Details details;
        
        

        details = {ndn::Name("/ObjectDetection/YOLOv8"),
            identity,
            3600,
            time(NULL),
            { {"type", "ObjectDetection"}, {"version", "1.0.0"}, {"tokenName", identity.toUri()+"/NDNSF/TOKEN/ObjectDetection/YOLOv8/0"} }};
        m_ServiceDiscovery.publishServiceDetail(details);
        UpdateUPTWithServiceMetaInfo(details);
        
        

        details = {ndn::Name("/ObjectDetection/YOLOv8_S"),
            identity,
            3600,
            time(NULL),
            { {"type", "ObjectDetection"}, {"version", "1.0.0"}, {"tokenName", identity.toUri()+"/NDNSF/TOKEN/ObjectDetection/YOLOv8_S/0"} }};
        m_ServiceDiscovery.publishServiceDetail(details);
        UpdateUPTWithServiceMetaInfo(details);
        
        

        details = {ndn::Name("/FlightControl/Takeoff"),
            identity,
            3600,
            time(NULL),
            { {"type", "FlightControl"}, {"version", "1.0.0"}, {"tokenName", identity.toUri()+"/NDNSF/TOKEN/FlightControl/Takeoff/0"} }};
        m_ServiceDiscovery.publishServiceDetail(details);
        UpdateUPTWithServiceMetaInfo(details);
        
        

        details = {ndn::Name("/FlightControl/Land"),
            identity,
            3600,
            time(NULL),
            { {"type", "FlightControl"}, {"version", "1.0.0"}, {"tokenName", identity.toUri()+"/NDNSF/TOKEN/FlightControl/Land/0"} }};
        m_ServiceDiscovery.publishServiceDetail(details);
        UpdateUPTWithServiceMetaInfo(details);
        
        

        details = {ndn::Name("/FlightControl/ManualControl"),
            identity,
            3600,
            time(NULL),
            { {"type", "FlightControl"}, {"version", "1.0.0"}, {"tokenName", identity.toUri()+"/NDNSF/TOKEN/FlightControl/ManualControl/0"} }};
        m_ServiceDiscovery.publishServiceDetail(details);
        UpdateUPTWithServiceMetaInfo(details);
        
    }

    void ServiceProvider_Drone::ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, ndn_service_framework::RequestMessage& requestMessage)
    {
        // log the request
        NDN_LOG_TRACE("Received request from " << RequesterName << " for service " << ServiceName << " function " << FunctionName << " with request id " << RequestID);

        
        if (ServiceName.equals(m_ObjectDetectionService.serviceName))
        {
            m_ObjectDetectionService.ConsumeRequest(RequesterName, providerName, ServiceName, FunctionName, RequestID, requestMessage);                                  
        }
        
        if (ServiceName.equals(m_FlightControlService.serviceName))
        {
            m_FlightControlService.ConsumeRequest(RequesterName, providerName, ServiceName, FunctionName, RequestID, requestMessage);                                  
        }
        
    }


}