#include <iostream>
#include <sstream>  // std::ostringstream
#include <string>

// #include <ndn-cxx/name.hpp>
// #include <ndn-cxx/face.hpp>

// #include "ndn-rpc-framework/ServiceProvider.hpp"
// #include "ndn-rpc-framework/ServiceStub.hpp"
// #include "ndn-rpc-framework/Service.hpp"

//#include "ObjectDetectionService.hpp"

#include "examples/messages.pb.h"
#include <ndn-service-framework/common.hpp>
#include "examples/ServiceProvider_Drone.hpp"
#include "examples/ServiceUser_Drone.hpp"
NDN_LOG_INIT(muas.main_gs);
int
main(int argc, char **argv)
{
    ndn::Face m_face("127.0.0.1");
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate drone_certificate(m_keyChain.getPib().getIdentity("/muas/drone2").getDefaultKey().getDefaultCertificate());
    
    muas::ServiceProvider_Drone m_serviceProvider(m_face,"/nsn/svs/muas",drone_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    // muas::ServiceUser_Drone m_serviceUser(m_face, "/nsn/svs/muas",drone_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    m_face.processEvents(ndn::time::milliseconds(2000));
    NDN_LOG_INFO("Drone Running");
    try{
        m_face.processEvents(ndn::time::milliseconds(0),true);
  }catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
    
}