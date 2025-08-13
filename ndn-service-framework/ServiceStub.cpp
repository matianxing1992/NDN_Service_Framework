
#include "ServiceStub.hpp"

namespace ndn_service_framework{

NDN_LOG_INIT(ndn_service_framework.ServiceStub);

ServiceStub::ServiceStub(ndn::Face& face, ServiceUser& user)
    :m_user(&user),
    m_face(face),
    m_scheduler(m_face.getIoContext())
{
}

}