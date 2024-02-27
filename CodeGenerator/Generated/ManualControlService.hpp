#ifndef ManualControlService_HPP
#define ManualControlService_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/face.hpp>

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceStub.hpp"
#include "ndn-service-framework/Service.hpp"

#include "messages.pb.h"

#include <vector>

namespace muas
{

	using Takeoff_Function = std::function<void(const ndn::Name &, const muas::ManualControl_Takeoff_Request &, muas::ManualControl_Takeoff_Response &)>;

	using Land_Function = std::function<void(const ndn::Name &, const muas::ManualControl_Land_Request &, muas::ManualControl_Land_Response &)>;



    class ManualControlService : public ndn_service_framework::Service
    {
    public:
        ManualControlService(ndn_service_framework::ServiceProvider &serviceProvider)
            : ndn_service_framework::Service(serviceProvider),
              serviceName("ManualControl")
        {
        }

        ~ManualControlService() {}

        void OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &) override;

		void Takeoff(const ndn::Name &requesterIdentity, const muas::ManualControl_Takeoff_Request &_request, muas::ManualControl_Takeoff_Response &_response);

		void Land(const ndn::Name &requesterIdentity, const muas::ManualControl_Land_Request &_request, muas::ManualControl_Land_Response &_response);



    public:
        std::set<std::shared_ptr<ndn::Regex>> regexSet{
			std::make_shared<ndn::Regex>("^(<>*)<NDNSF><REQUEST>(<>*)<ManualControl><Takeoff>(<>)"),
			std::make_shared<ndn::Regex>("^(<>*)<NDNSF><REQUEST>(<>*)<ManualControl><Land>(<>)")

        };
        ndn::Name serviceName;
    };
}
#endif