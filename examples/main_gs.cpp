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
#include "ServiceUser_GS.hpp"
#include "ServiceProvider_GS.hpp"
NDN_LOG_INIT(muas.main_gs);
int
main(int argc, char **argv)
{
    ndn::Face m_face("127.0.0.1");
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate gs_certificate(m_keyChain.getPib().getIdentity("/muas/gs1").getDefaultKey().getDefaultCertificate());
    muas::ServiceUser_GS m_serviceUser(m_face, "/nsn/svs/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    muas::ServiceProvider_GS m_serviceProvider(m_face,"/nsn/svs/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    
    m_face.processEvents(ndn::time::milliseconds(2000));



    muas::ManualControl_Takeoff_Request _request2;
    _request2.set_latitude(100);
    _request2.set_longitude(200);
    m_serviceUser.Takeoff_Async(ndn::Name("/muas/drone2"), _request2, [](const muas::ManualControl_Takeoff_Response& _response){
        NDN_LOG_INFO(_response.DebugString());
    }); 

    NDN_LOG_INFO("GS Running");
    try{
        m_face.processEvents(ndn::time::milliseconds(0),true);
    } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
    


}