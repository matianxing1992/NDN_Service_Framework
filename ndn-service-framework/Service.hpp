#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_HPP
#include "common.hpp"
#include "NDNSFMessages.hpp"



namespace ndn_service_framework{

    class ServiceProvider;
    class Service
    {
        public:
            Service(ndn_service_framework::ServiceProvider& serviceProvider);
            virtual ~Service() {}

        virtual void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, RequestMessage& requestMessage) = 0;

        protected:
            ndn_service_framework::ServiceProvider* m_ServiceProvider;

    };
}
#endif