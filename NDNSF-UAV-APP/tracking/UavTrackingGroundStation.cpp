#include "uav-tracking-job.hpp"

#include <exception>
#include <iostream>

int
main(int argc, char** argv)
{
  try {
    return ndnsf::examples::uav::runTrackingGroundStationCli(argc, argv);
  }
  catch (const std::exception& error) {
    std::cerr << "UavTrackingGroundStation error: " << error.what() << std::endl;
    return 2;
  }
}
