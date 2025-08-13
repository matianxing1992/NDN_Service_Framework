#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>

// #include <ndn-cxx/name.hpp>
// #include <ndn-cxx/face.hpp>

// #include "ndn-rpc-framework/ServiceProvider.hpp"
// #include "ndn-rpc-framework/ServiceStub.hpp"
// #include "ndn-rpc-framework/Service.hpp"

//#include "ObjectDetectionService.hpp"

#include "CodeGenerator/Generated/messages.pb.h"
#include <ndn-service-framework/common.hpp>
#include "CodeGenerator/Generated/ServiceProvider_Drone.hpp"
#include "CodeGenerator/Generated/ServiceUser_GS.hpp"

#include <boost/asio/io_context.hpp>

NDN_LOG_INIT(muas.main_gs);

int
main(int argc, char **argv)
{
    if (argc < 5) {
        std::cerr << "Usage: gs-example <strategy> <user_identity> <requests_per_second> <total_running_seconds> <provider_identity1> [<identity2> ... <identityN>]" << std::endl;
        return 1;
    }

    std::string strategy_str = argv[1];
    size_t strategy = 0;
    if (strategy_str == "NoCoordination") {
        strategy = ndn_service_framework::tlv::NoCoordination;
    }
    else if (strategy_str == "FirstResponding") {
        strategy = ndn_service_framework::tlv::FirstResponding;
    }
    else if (strategy_str == "RandomSelection") {
        strategy = ndn_service_framework::tlv::LoadBalancing;
    }
    else {
        std::cerr << "Invalid strategy: " << strategy_str << std::endl;
        return 1;
    }

    std::string user_identity = argv[2];
    int requests_per_second = std::stoi(argv[3]);
    int total_seconds = std::stoi(argv[4]);
    int interval_in_ms = 1000 / requests_per_second;
    int count = requests_per_second * total_seconds;

    std::vector<ndn::Name> provider_identities;
    for (int i = 5; i < argc; ++i) {
        provider_identities.push_back(ndn::Name(argv[i]));
    }

    ndn::Face m_face;
    ndn::Scheduler m_scheduler(m_face.getIoContext());
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate gs_certificate(m_keyChain.getPib().getIdentity(user_identity).getDefaultKey().getDefaultCertificate());

    float totalLatency = 0.0;
    int receivedRes = 0;
    int requestSent = 0;
    std::mutex mtx;

    muas::ServiceUser_GS m_serviceUser(m_face, "/muas", gs_certificate,
        m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),
        "/usr/local/bin/trust-any.conf");

    

    m_face.processEvents(ndn::time::milliseconds(2000));

    muas::FlightControl_ManualControl_Request _request;
    _request.set_x(0.5f);
    _request.set_y(0.5f);
    _request.set_z(0.5f);
    _request.set_r(0.5f);
    _request.set_s(0.5f);
    _request.set_t(0.5f);

    auto call = [&]() {
        auto start_time = ndn::time::system_clock::now();
        {
            std::lock_guard<std::mutex> lock(mtx);
            requestSent += 1;
        }

        m_serviceUser.ManualControl_Async(provider_identities, _request,
            [&, start_time](const muas::FlightControl_ManualControl_Response& _response) {
                NDN_LOG_INFO(_response.DebugString());
                auto end_time = ndn::time::system_clock::now();
                auto latency = ndn::time::duration_cast<ndn::time::milliseconds>(end_time - start_time).count();
                NDN_LOG_INFO("Latency: " << latency << " ms");

                std::lock_guard<std::mutex> lock(mtx);
                totalLatency += latency;
                receivedRes += 1;
            },
            [&](const muas::FlightControl_ManualControl_Request& _request) {
                NDN_LOG_INFO("Timeout " << _request.DebugString());
            },
            1000, // timeout time in ms
            strategy);
    };

    auto CalculateLatency = [&]() {
        std::ostringstream oss;
        oss << (receivedRes > 0 ? totalLatency / receivedRes : 0.0);
        double successRate = requestSent > 0 ? (double)receivedRes / requestSent : 0.0;

        std::cout << "Success Rate: " << successRate << std::endl;
        std::cout << "Received Responses: " << receivedRes << std::endl;
        std::cout << "Average Latency: " << oss.str() << " ms" << std::endl;

        // m_face.getIoContext().stop();  // 停止事件循环，结束程序
    };

    m_face.processEvents(ndn::time::milliseconds(30000), true);

    for (int i = 0; i < count; ++i) {
        m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms * i), call);
    }

    m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms * count + 20000), CalculateLatency);

    NDN_LOG_INFO("Drone Running");

    try {
        m_face.processEvents(ndn::time::milliseconds(0), true);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
