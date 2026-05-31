#ifndef NDNSF_EXAMPLES_UAV_NAMES_HPP
#define NDNSF_EXAMPLES_UAV_NAMES_HPP

#include <ndn-cxx/name.hpp>

#include <string>

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

struct UavRuntimeConfig
{
  ndn::Name groupPrefix = GROUP_PREFIX;
  ndn::Name controllerPrefix = CONTROLLER_PREFIX;
  ndn::Name groundStationIdentity = GROUND_STATION_IDENTITY;
  ndn::Name droneIdentityPrefix = DRONE_IDENTITY_PREFIX;
  std::string trustSchema = TRUST_SCHEMA;
  ndn::Name serviceMavlinkExecute = SERVICE_MAVLINK_EXECUTE;
  ndn::Name serviceMissionAssign = SERVICE_MISSION_ASSIGN;
  ndn::Name serviceTelemetryStatus = SERVICE_TELEMETRY_STATUS;
  ndn::Name serviceCameraFrame = SERVICE_CAMERA_FRAME;
  ndn::Name serviceCameraVideoControlSuffix = SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX;
  ndn::Name serviceGsObjectDetection = SERVICE_GS_OBJECT_DETECTION;
};

UavRuntimeConfig
loadUavRuntimeConfig(const std::string& path);

ndn::Name
droneIdentity(const std::string& droneId);

ndn::Name
droneIdentity(const UavRuntimeConfig& config, const std::string& droneId);

ndn::Name
droneVideoControlService(const std::string& droneId);

ndn::Name
droneVideoControlService(const UavRuntimeConfig& config, const std::string& droneId);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_NAMES_HPP
