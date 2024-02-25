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
NDN_LOG_INIT(muas.main_gs);
int
main(int argc, char **argv)
{
    ndn::Face m_face("127.0.0.1");
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate gs_certificate(m_keyChain.getPib().getIdentity("/muas/gs1").getDefaultKey().getDefaultCertificate());
    muas::ServiceUser_GS m_serviceUser(m_face, "/nsn/svs/muas",gs_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");

    m_face.processEvents(ndn::time::milliseconds(1000));

    muas::ObjectDetection_YOLOv8_Request _request;
    _request.set_image_str("image_str");
    m_serviceUser.YOLOv8_Async(ndn::Name("/muas/drone1"), _request, [](const muas::ObjectDetection_YOLOv8_Response& _response){
        //auto result0=_response.results().Get(0);
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