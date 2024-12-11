#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_STUB_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_STUB_HPP
#include "common.hpp"

#include "utils.hpp"
#include <vector>

namespace ndn_service_framework{

    class ServiceUser;
    class ServiceStub
    {
        public:
            ServiceStub(ServiceUser &user);
            virtual ~ServiceStub() {}

            virtual void OnResponseDecryptionSuccessCallback(const ndn::Name& serviceProviderName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &buffer)= 0;

        protected:
            ndn::Face m_face;
            ndn::Name m_serviceGroupName;
            ndn::Name m_serviceStubID;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            ndn::KeyChain m_keyChain;
            ServiceUser* m_user;
    };
}
#endif