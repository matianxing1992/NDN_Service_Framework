#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_HPP
#include "common.hpp"



namespace ndn_service_framework{

    class ServiceProvider;
    class Service
    {
        public:
            Service(ndn_service_framework::ServiceProvider& serviceProvider);
            virtual ~Service() {}

        virtual void OnRequestDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, const ndn::Buffer&)= 0;


        protected:
            ndn_service_framework::ServiceProvider* m_ServiceProvider;

    };
}
#endif