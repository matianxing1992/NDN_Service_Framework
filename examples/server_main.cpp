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
int
main(int argc, char **argv)
{
    // ndn_rpc_framework::ServiceProvider sp(ndn::Name("/service_provider_group"),ndn::Name("/node1"));
    // ObjectDetectionService service_1;
    // sp.bind(service_1);
    // sp.start();
    muas::ObjectDetection_YOLOv8_Request _request;
    _request.set_image_str("image_str");
    std::string buffer="";
    _request.SerializeToString(&buffer);
    std::cout << "Serialized to String: " << buffer;
    muas::ObjectDetection_YOLOv8_Request _request2;
    _request2.ParseFromString(buffer);
    std::cout << "image_str String: " << _request.image_str();

}