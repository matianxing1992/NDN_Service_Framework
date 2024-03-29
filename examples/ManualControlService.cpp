#include "./ManualControlService.hpp"

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
using namespace mavsdk;
using std::chrono::seconds;
using std::this_thread::sleep_for;

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
        
        _response.set_latitude(100);
        _response.set_longitude(200);


        
        NDN_LOG_INFO("Takeoff request: " << _request.DebugString());
        // RPC logic starts here


        Mavsdk mavsdk{Mavsdk::Configuration{Mavsdk::ComponentType::GroundStation}};
        ConnectionResult connection_result = mavsdk.add_any_connection("udp://:14540");

        if (connection_result != ConnectionResult::Success) {
            NDN_LOG_INFO("Connection failed: " << connection_result );
        }

        auto system = mavsdk.first_autopilot(-1);
        if (!system) {
            NDN_LOG_INFO("Timed out waiting for system");
        }

        // Instantiate plugins.
        auto telemetry = Telemetry{system.value()};
        auto action = Action{system.value()};

        // We want to listen to the altitude of the drone at 1 Hz.
        const auto set_rate_result = telemetry.set_rate_position(1.0);
        if (set_rate_result != Telemetry::Result::Success) {
            NDN_LOG_ERROR("Setting rate failed: " << set_rate_result );
        }

        // Set up callback to monitor altitude while the vehicle is in flight
        telemetry.subscribe_position([](Telemetry::Position position) {
            NDN_LOG_INFO("Altitude: " << position.relative_altitude_m << " m");
        });

        // Check until vehicle is ready to arm
        while (telemetry.health_all_ok() != true) {
            NDN_LOG_INFO("Vehicle is getting ready to arm");
            sleep_for(seconds(1));
        }

        // Arm vehicle
        NDN_LOG_INFO("Arming...");
        const Action::Result arm_result = action.arm();

        if (arm_result != Action::Result::Success) {
            NDN_LOG_ERROR("Arming failed: " << arm_result);
        }

        // Take off
        NDN_LOG_INFO("Taking off...");
        const Action::Result takeoff_result = action.takeoff();
        if (takeoff_result != Action::Result::Success) {
            NDN_LOG_ERROR("Takeoff failed: " << takeoff_result);
        }

        // Let it hover for a bit before landing again.
        sleep_for(seconds(30));

        NDN_LOG_INFO("Landing...");
        const Action::Result land_result = action.land();
        if (land_result != Action::Result::Success) {
            NDN_LOG_ERROR("Land failed: " << land_result );
        }

        // Check if vehicle is still in air
        while (telemetry.in_air()) {
            NDN_LOG_INFO("Vehicle is landing...");
            sleep_for(seconds(1));
        }
        NDN_LOG_INFO("Landed!");

        // We are relying on auto-disarming but let's keep watching the telemetry for a bit longer.
        sleep_for(seconds(3));
        NDN_LOG_INFO("Finished...");
        // RPC logic ends here
    }
    
    void ManualControlService::Land(const ndn::Name &requesterIdentity, const muas::ManualControl_Land_Request &_request, muas::ManualControl_Land_Response &_response)
    {
        NDN_LOG_INFO("Land request: " << _request.DebugString());
        // RPC logic starts here

        // RPC logic ends here
    }
    
}