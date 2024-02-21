
#include "ServiceStub.hpp"

namespace ndn_service_framework{

NDN_LOG_INIT(ndn_service_framework.ServiceStub);

ServiceStub::ServiceStub(ServiceUser& user)
    :m_user(&user)
{
}

}