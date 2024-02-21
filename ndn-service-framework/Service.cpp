#include <Service.hpp>


namespace ndn_service_framework{

NDN_LOG_INIT(ndn_service_framework.Service);


Service::Service(ndn_service_framework::ServiceProvider& serviceProvider)
    :m_ServiceProvider(&serviceProvider)
    
{
    //m_ServiceProvider=std::make_shared<ServiceProvider>(serviceProvider);
}

}
