#ifndef NDNSF_EXAMPLES_UAV_NAMES_HPP
#define NDNSF_EXAMPLES_UAV_NAMES_HPP

#include <ndn-cxx/name.hpp>

namespace ndnsf::examples::uav {

inline const ndn::Name GROUP_PREFIX("/example/uav/group");
inline const ndn::Name CONTROLLER_PREFIX("/example/uav/controller");
inline const ndn::Name GROUND_STATION_IDENTITY("/example/uav/gs");
inline const ndn::Name DRONE_IDENTITY_PREFIX("/example/uav/drone");
inline const char* TRUST_SCHEMA = "examples/trust-schema.conf";

inline const ndn::Name SERVICE_MAVLINK_EXECUTE("/UAV/MAVLink/Execute");
inline const ndn::Name SERVICE_MISSION_ASSIGN("/UAV/Mission/Assign");
inline const ndn::Name SERVICE_TELEMETRY_STATUS("/UAV/Telemetry/GetStatus");
inline const ndn::Name SERVICE_CAMERA_FRAME("/UAV/Camera/GetFrame");
inline const ndn::Name SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX("/UAV/Camera/Video");
inline const ndn::Name SERVICE_GS_OBJECT_DETECTION("/UAV/GS/ObjectDetection");

ndn::Name
droneIdentity(const std::string& droneId);

ndn::Name
droneVideoControlService(const std::string& droneId);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_NAMES_HPP
