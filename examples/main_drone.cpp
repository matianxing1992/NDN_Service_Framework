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

#include <gtkmm.h>

NDN_LOG_INIT(muas.main_gs);

int
main(int argc, char **argv)
{
    ndn::Face m_face("127.0.0.1");
    ndn::security::KeyChain m_keyChain;
    ndn::security::Certificate drone_certificate(m_keyChain.getPib().getIdentity("/muas/drone1").getDefaultKey().getDefaultCertificate());
    
    // muas::ServiceProvider_Drone m_serviceProvider(m_face,"/nsn/svs/muas",drone_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    muas::ServiceUser_Drone m_serviceUser(m_face, "/nsn/svs/muas",drone_certificate,m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate(),"trust-schema.conf");
    
    m_face.processEvents(ndn::time::milliseconds(2000));

    muas::ObjectDetection_YOLOv8_Request _request;
    _request.set_image_str("image_str");
    m_serviceUser.YOLOv8_Async(ndn::Name("/muas/gs1"), _request, [&](const muas::ObjectDetection_YOLOv8_Response& _response){
        NDN_LOG_INFO(_response.DebugString());

        auto app = Gtk::Application::create(argc, argv, "so.question.q65011763");
        
        Gtk::Window window;
        Gtk::Image m_image("/home/tianxing/NDN/drone-car.png");
        window.add(m_image);
        window.show_all();

        app->run(window);
    }); 

    NDN_LOG_INFO("Drone Running");
    try{
        m_face.processEvents(ndn::time::milliseconds(0),true);
  }catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
    
}