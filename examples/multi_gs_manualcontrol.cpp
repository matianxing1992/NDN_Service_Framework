#include <iostream>
#include <sstream>  // std::ostringstream
#include <string>

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

#include <mutex>

NDN_LOG_INIT(muas.main_gs);

int
main(int argc, char **argv)
{
    // Ensure there are at least two arguments: the program name and one identity
    if (argc < 3)
    {
        std::cerr << "Usage: gs-example <strategy> <user_identity> <interval_in_ms> <count> <provider_identity1> [<identity2> ... <identityN>]" << std::endl;
        exit(1);
    }

    std::string strategy_str = argv[1];
    size_t strategy = 0;
    // "NoCoordination" -> ndn_service_framework::tlv::NoCoordination
    // "FirstResponding" ->  ndn_service_framework::tlv::FirstResponding
    // "LoadBalancing" -> ndn_service_framework::tlv::LoadBalancing
    if (strategy_str == "NoCoordination") {
      strategy = ndn_service_framework::tlv::NoCoordination;
    }
    else if (strategy_str == "FirstResponding") {
      strategy = ndn_service_framework::tlv::FirstResponding;
    }
    else if (strategy_str == "LoadBalancing") {
      strategy = ndn_service_framework::tlv::LoadBalancing;
    }
    else {
      std::cerr << "Invalid strategy: " << strategy_str << std::endl;
      exit(1);
    }

    std::string user_identity = argv[2];
    int interval_in_ms = std::stoi(argv[3]);
    int count = std::stoi(argv[4]);

    // Store identities in a vector
    std::vector<ndn::Name> provider_identities;
    for (int i = 5; i < argc; ++i) {
      provider_identities.push_back(ndn::Name(argv[i]));
    }

    ndn::Face m_face;
    ndn::Scheduler m_scheduler(m_face.getIoContext());
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate gs_certificate(m_keyChain.getPib().getIdentity(user_identity).getDefaultKey().getDefaultCertificate());
    // a map to record the starting time of each RPC Call
    std::map<int, ndn::time::system_clock::time_point> rpcStartTimeMap;
    // a map to record the end time of each RPC Call
    std::map<int, ndn::time::system_clock::time_point> rpcEndTimeMap;
    float totalLatency = 0.0;
    int receivedRes = 0;
    int requestSent = 0;
    std::mutex mtx;



    // muas::ServiceProvider_Drone m_serviceProvider(m_face,"/nsn/svs/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    muas::ServiceUser_GS m_serviceUser(m_face, "/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"/usr/local/bin/trust-any.conf");
    
    m_face.processEvents(ndn::time::milliseconds(2000));

    muas::FlightControl_ManualControl_Request _request;
    _request.set_x(0.5f);
    _request.set_y(0.5f);
    _request.set_z(0.5f);
    _request.set_r(0.5f);
    _request.set_s(0.5f);
    _request.set_t(0.5f);
  

    auto call = [&](){
      // add RPC Call start time to map
      auto start_time = ndn::time::system_clock::now();
      std::lock_guard<std::mutex> lock(mtx);
      requestSent += 1;
      m_serviceUser.ManualControl_Async(provider_identities, _request, [&,start_time](const muas::FlightControl_ManualControl_Response& _response){
            NDN_LOG_INFO(_response.DebugString());
            auto end_time = ndn::time::system_clock::now();
            auto latency = ndn::time::duration_cast<ndn::time::milliseconds>(end_time - start_time).count();
            // log latency
            NDN_LOG_INFO("Latency: " << latency << " ms");
            // add sync lock to totalLatency

            std::lock_guard<std::mutex> lock(mtx);
            totalLatency += latency;
            receivedRes += 1;
            // // log total latency
            // NDN_LOG_INFO("Total Latency: " << totalLatency << " ms");
            // // log receivedRes
            // NDN_LOG_INFO("Received Res: " << receivedRes);
        },strategy); 
    };

    auto CalculateLatency = [&]() {
      //totalLatency / count to string
      std::ostringstream oss;
      oss << totalLatency / receivedRes;
      std::string latency = oss.str();
      // cout success rate = receivedRes / requestSent
      double successRate = (double)receivedRes / (double)requestSent;
      std::cout << "Success Rate: " << successRate << std::endl;
      // cout receivedRes
      std::cout << "Received Res: " << receivedRes << std::endl;
      // cout average latency
      std::cout << "Average Latency: " << latency << " ms" << std::endl;
    };


    for (int i = 0; i < count; i++)
    {
        m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms*i), call);
    }

    m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms*count+20000), CalculateLatency);
 
    // m_serviceUser.YOLOv8_Async(providers, _request, [&](const muas::ObjectDetection_YOLOv8_Response& _response){
    //     NDN_LOG_INFO(_response.DebugString());
    // }); 

    NDN_LOG_INFO("Drone Running");
    try{
        m_face.processEvents(ndn::time::milliseconds(0),true);
  }catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
    
}