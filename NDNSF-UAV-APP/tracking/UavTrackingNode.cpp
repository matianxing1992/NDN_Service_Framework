#include "uav-tracking-provider.hpp"

#include <exception>
#include <iostream>

int
main(int argc, char** argv)
{
  try {
    return ndnsf::examples::uav::runTrackingNodeCli(argc, argv);
  }
  catch (const std::exception& error) {
    std::cerr << "UavTrackingNode error: " << error.what() << std::endl;
    return 2;
  }
}
