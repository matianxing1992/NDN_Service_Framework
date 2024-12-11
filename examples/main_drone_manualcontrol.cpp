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
#include "CodeGenerator/Generated/ServiceUser_GS.hpp"
#include "CodeGenerator/Generated/ServiceProvider_Drone.hpp"
NDN_LOG_INIT(muas.main_drone);
int
main(int argc, char **argv)
{
    ndn::Face m_face;
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate gs_certificate(m_keyChain.getPib().getIdentity("/muas/drone1").getDefaultKey().getDefaultCertificate());
    muas::ServiceProvider_Drone m_serviceProvider(m_face,"/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    
    //m_face.processEvents(ndn::time::milliseconds(2000));

    NDN_LOG_INFO("Drone Running");
    try{
        m_face.processEvents(ndn::time::milliseconds(0),true);
    } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
    


}