#include "UavProtocol.hpp"
#include "UavNames.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

std::string
trimConfigText(std::string text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

void
assignConfigValue(UavRuntimeConfig& config, const std::string& key, const std::string& value)
{
  if (key == "group-prefix") {
    config.groupPrefix = ndn::Name(value);
  }
  else if (key == "controller-prefix") {
    config.controllerPrefix = ndn::Name(value);
  }
  else if (key == "ground-station-identity") {
    config.groundStationIdentity = ndn::Name(value);
  }
  else if (key == "drone-prefix") {
    config.droneIdentityPrefix = ndn::Name(value);
  }
  else if (key == "trust-schema") {
    config.trustSchema = value;
  }
  else if (key == "service-mavlink-execute") {
    config.serviceMavlinkExecute = ndn::Name(value);
  }
  else if (key == "service-mission-assign") {
    config.serviceMissionAssign = ndn::Name(value);
  }
  else if (key == "service-telemetry-status") {
    config.serviceTelemetryStatus = ndn::Name(value);
  }
  else if (key == "service-camera-frame") {
    config.serviceCameraFrame = ndn::Name(value);
  }
  else if (key == "service-camera-video-control-suffix") {
    config.serviceCameraVideoControlSuffix = ndn::Name(value);
  }
  else if (key == "service-camera-recording-manifest-suffix") {
    config.serviceCameraRecordingManifestSuffix = ndn::Name(value);
  }
  else if (key == "service-gs-object-detection") {
    config.serviceGsObjectDetection = ndn::Name(value);
  }
}

uint64_t
uint64FieldOr(const Fields& fields, const std::string& key, uint64_t fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  try {
    return std::stoull(it->second);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

} // namespace

Fields
loadKeyValueConfig(const std::string& path)
{
  Fields fields;
  if (path.empty()) {
    return fields;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open UAV config: " + path);
  }

  std::string line;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    line = trimConfigText(line);
    if (line.empty()) {
      continue;
    }

    std::string key;
    std::string value;
    const auto equal = line.find('=');
    if (equal != std::string::npos) {
      key = trimConfigText(line.substr(0, equal));
      value = trimConfigText(line.substr(equal + 1));
    }
    else {
      std::istringstream is(line);
      is >> key;
      std::getline(is, value);
      value = trimConfigText(value);
    }
    if (!key.empty() && !value.empty()) {
      fields[key] = value;
    }
  }
  return fields;
}

UavRuntimeConfig
loadUavRuntimeConfig(const std::string& path)
{
  UavRuntimeConfig config;
  if (path.empty()) {
    return config;
  }

  for (const auto& [key, value] : loadKeyValueConfig(path)) {
    assignConfigValue(config, key, value);
  }

  return config;
}

ndn::Name
droneIdentity(const std::string& droneId)
{
  return droneIdentity(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneIdentity(const UavRuntimeConfig& config, const std::string& droneId)
{
  if (droneId.empty()) {
    return config.droneIdentityPrefix;
  }
  return ndn::Name(config.droneIdentityPrefix).append(droneId);
}

ndn::Name
droneVideoControlService(const std::string& droneId)
{
  return droneVideoControlService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneVideoControlService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraVideoControlSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneCameraRecordingManifestService(const std::string& droneId)
{
  return droneCameraRecordingManifestService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneCameraRecordingManifestService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraRecordingManifestSuffix) {
    service.append(component);
  }
  return service;
}

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
encodeFields(const Fields& fields)
{
  std::ostringstream os;
  bool first = true;
  for (const auto& [key, value] : fields) {
    if (!first) {
      os << ';';
    }
    first = false;
    os << key << '=';
    for (const char ch : value) {
      if (ch == '%' || ch == ';' || ch == '=') {
        os << '%' << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch))
           << std::dec << std::nouppercase;
      }
      else {
        os << ch;
      }
    }
  }
  return os.str();
}

Fields
decodeFields(const std::string& payload)
{
  Fields fields;
  size_t start = 0;
  while (start <= payload.size()) {
    const auto end = payload.find(';', start);
    const auto part = payload.substr(start, end == std::string::npos ? end : end - start);
    if (!part.empty()) {
      const auto equal = part.find('=');
      if (equal != std::string::npos) {
        std::string value;
        for (size_t i = equal + 1; i < part.size(); ++i) {
          if (part[i] == '%' && i + 2 < part.size()) {
            const auto byte = std::stoi(part.substr(i + 1, 2), nullptr, 16);
            value.push_back(static_cast<char>(byte));
            i += 2;
          }
          else {
            value.push_back(part[i]);
          }
        }
        fields[part.substr(0, equal)] = value;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return fields;
}

TelemetryState
TelemetryState::fromFields(const Fields& fields)
{
  TelemetryState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.lat = fieldOr(fields, "lat", state.lat);
  state.lon = fieldOr(fields, "lon", state.lon);
  state.altitudeM = fieldOr(fields, "altitude_m", state.altitudeM);
  state.groundspeedMps = fieldOr(fields, "groundspeed_mps", state.groundspeedMps);
  state.batteryPercent = fieldOr(fields, "battery_percent", state.batteryPercent);
  state.heartbeatSeen = fieldOr(fields, "heartbeat_seen", state.heartbeatSeen);
  state.flightControllerReady = fieldOr(fields, "flight_controller_ready", state.flightControllerReady);
  state.gpsReady = fieldOr(fields, "gps_ready", state.gpsReady);
  state.ekfReady = fieldOr(fields, "ekf_ready", state.ekfReady);
  state.batteryReady = fieldOr(fields, "battery_ready", state.batteryReady);
  state.armed = fieldOr(fields, "armed", state.armed);
  state.gpsFixType = fieldOr(fields, "gps_fix_type", state.gpsFixType);
  state.gpsFixName = fieldOr(fields, "gps_fix_name", state.gpsFixName);
  state.gpsSatellitesVisible = fieldOr(fields, "gps_satellites_visible", state.gpsSatellitesVisible);
  state.flightControllerState = fieldOr(fields, "fc_state", state.flightControllerState);
  state.systemStatus = fieldOr(fields, "system_status", state.systemStatus);
  state.systemStatusName = fieldOr(fields, "system_status_name", state.systemStatusName);
  state.landedState = fieldOr(fields, "landed_state", state.landedState);
  state.landedStateName = fieldOr(fields, "landed_state_name", state.landedStateName);
  state.vtolStateName = fieldOr(fields, "vtol_state_name", state.vtolStateName);
  state.batteryVoltageV = fieldOr(fields, "battery_voltage_v", state.batteryVoltageV);
  state.batteryCurrentA = fieldOr(fields, "battery_current_a", state.batteryCurrentA);
  state.video = fieldOr(fields, "video", state.video);
  state.capture = fieldOr(fields, "capture", state.capture);
  state.recording = fieldOr(fields, "recording", state.recording);
  state.linkState = fieldOr(fields, "link_state", state.linkState);
  state.manualControlState = fieldOr(fields, "manual_control_state", state.manualControlState);
  state.manualReplayActive = fieldOr(fields, "manual_replay_active", state.manualReplayActive);
  state.manualNeutralSent = fieldOr(fields, "manual_neutral_sent", state.manualNeutralSent);
  state.manualFreshForMs = fieldOr(fields, "manual_fresh_for_ms", state.manualFreshForMs);
  state.manualReplayCount = fieldOr(fields, "manual_replay_count", state.manualReplayCount);
  state.safetyDetail = fieldOr(fields, "safety_detail", state.safetyDetail);

  const auto timestamp = fieldOr(fields, "timestamp_ms", "");
  if (!timestamp.empty()) {
    try {
      state.timestampMs = std::stoull(timestamp);
    }
    catch (const std::exception&) {
      state.timestampMs = 0;
    }
  }

  std::string reason = fieldOr(fields, "readiness_reason", "");
  if (reason.empty()) {
    if (state.heartbeatSeen != "true") {
      reason = "waiting-heartbeat";
    }
    else if (state.flightControllerReady == "false") {
      reason = "fc-not-ready";
    }
    else if (state.gpsReady == "false") {
      reason = "gps-not-ready";
    }
    else if (state.ekfReady == "false") {
      reason = "ekf-not-ready";
    }
    else if (state.batteryReady == "false") {
      reason = "battery-low";
    }
    else if (state.flightControllerReady == "unknown" ||
             state.gpsReady == "unknown" ||
             state.ekfReady == "unknown" ||
             state.batteryReady == "unknown") {
      reason = "readiness-unknown";
    }
    else {
      reason = "ok";
    }
  }
  state.readinessReason = reason;
  state.readiness = fieldOr(fields, "readiness", reason == "ok" ? "ready" : "not-ready");
  return state;
}

Fields
TelemetryState::toFields() const
{
  const bool readyForTakeoff = heartbeatSeen == "true" &&
                               flightControllerReady == "true" &&
                               gpsReady == "true" &&
                               ekfReady == "true" &&
                               batteryReady == "true" &&
                               armed == "true" &&
                               landedStateName == "on-ground";
  return {
    {"drone_id", droneId},
    {"lat", lat},
    {"lon", lon},
    {"altitude_m", altitudeM},
    {"groundspeed_mps", groundspeedMps},
    {"battery_percent", batteryPercent},
    {"heartbeat_seen", heartbeatSeen},
    {"flight_controller_ready", flightControllerReady},
    {"gps_ready", gpsReady},
    {"ekf_ready", ekfReady},
    {"battery_ready", batteryReady},
    {"armed", armed},
    {"gps_fix_type", gpsFixType},
    {"gps_fix_name", gpsFixName},
    {"gps_satellites_visible", gpsSatellitesVisible},
    {"fc_state", flightControllerState},
    {"system_status", systemStatus},
    {"system_status_name", systemStatusName},
    {"landed_state", landedState},
    {"landed_state_name", landedStateName},
    {"vtol_state_name", vtolStateName},
    {"battery_voltage_v", batteryVoltageV},
    {"battery_current_a", batteryCurrentA},
    {"readiness", readiness},
    {"readiness_reason", readinessReason},
    {"ready_for_takeoff", readyForTakeoff ? "true" : "false"},
    {"video", video},
    {"capture", capture},
    {"recording", recording},
    {"link_state", linkState},
    {"manual_control_state", manualControlState},
    {"manual_replay_active", manualReplayActive},
    {"manual_neutral_sent", manualNeutralSent},
    {"manual_fresh_for_ms", manualFreshForMs},
    {"manual_replay_count", manualReplayCount},
    {"safety_detail", safetyDetail},
    {"timestamp_ms", std::to_string(timestampMs)},
  };
}

std::string
TelemetryState::statusLine() const
{
  return "Telemetry drone=" + droneId +
         " alt=" + altitudeM + "m" +
         " lat=" + lat +
         " lon=" + lon +
         " battery=" + batteryPercent + "%" +
         " ready=" + readiness +
         " reason=" + readinessReason +
         " armed=" + armed +
         " gps=" + gpsReady +
         " ekf=" + ekfReady +
         " landed=" + landedStateName +
         " speed=" + groundspeedMps + "m/s" +
         " video=" + video +
         " link=" + linkState +
         " manual=" + manualControlState;
}

std::string
TelemetryState::mapSummary(const std::string& selectedDrone) const
{
  return "Map / mission workspace\n\n"
         "Selected drone: " + selectedDrone + "\n"
         "Telemetry source: Drone " + droneId + "\n"
         "Position: lat " + lat + "  lon " + lon + "\n"
         "Altitude: " + altitudeM + "\n"
         "Readiness: " + readiness + " (" + readinessReason + ")  Armed: " + armed + "\n"
         "GPS: " + gpsReady + " fix=" + gpsFixName + " (" + gpsFixType + ")" +
         " sats=" + gpsSatellitesVisible + " EKF=" + ekfReady + "\n"
         "Flight controller: " + systemStatusName + " landed=" + landedStateName +
         " vtol=" + vtolStateName + "\n"
         "Battery: " + batteryPercent + "% " + batteryVoltageV + "V " +
         batteryCurrentA + "A  Speed: " + groundspeedMps + " m/s\n"
         "Video: " + video + "  Capture: " + capture + "  Recording: " + recording + "\n\n"
         "Safety: link=" + linkState + " manual=" + manualControlState +
         " neutral=" + manualNeutralSent + " fresh_for=" + manualFreshForMs + "ms\n\n"
         "Map tile: OpenStreetMap, centered on the ground station.\n"
         "Click the map to append mission waypoints.";
}

ReadinessState
ReadinessState::fromFields(const Fields& fields)
{
  ReadinessState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.heartbeatSeen = fieldOr(fields, "heartbeat_seen", state.heartbeatSeen);
  state.flightControllerReady = fieldOr(fields, "flight_controller_ready", state.flightControllerReady);
  state.gpsReady = fieldOr(fields, "gps_ready", state.gpsReady);
  state.ekfReady = fieldOr(fields, "ekf_ready", state.ekfReady);
  state.batteryReady = fieldOr(fields, "battery_ready", state.batteryReady);
  state.armed = fieldOr(fields, "armed", state.armed);
  state.mode = fieldOr(fields, "mode", fieldOr(fields, "system_status_name", state.mode));
  state.landedStateName = fieldOr(fields, "landed_state_name", state.landedStateName);
  state.readiness = fieldOr(fields, "readiness", state.readiness);
  auto readinessReason = fieldOr(fields, "readiness_reason", "");
  state.timestampMs = uint64FieldOr(fields, "timestamp_ms", state.timestampMs);

  if (readinessReason.empty() || readinessReason == "unknown") {
    if (state.heartbeatSeen != "true") {
      readinessReason = "waiting-heartbeat";
    }
    else if (state.flightControllerReady == "false") {
      readinessReason = "flight-controller-not-ready";
    }
    else if (state.gpsReady == "false") {
      readinessReason = "gps-not-ready";
    }
    else if (state.ekfReady == "false") {
      readinessReason = "ekf-not-ready";
    }
    else if (state.batteryReady == "false") {
      readinessReason = "battery-not-ready";
    }
    else if (state.flightControllerReady == "true" &&
             state.gpsReady == "true" &&
             state.ekfReady == "true" &&
             state.batteryReady == "true") {
      readinessReason = "ok";
    }
    else {
      readinessReason = "readiness-unknown";
    }
  }
  state.readinessReason = readinessReason;
  if (fieldOr(fields, "readiness", "").empty()) {
    state.readiness = state.readinessReason == "ok" ? "ready" : "not-ready";
  }
  return state;
}

ReadinessState
ReadinessState::fromTelemetry(const TelemetryState& telemetry)
{
  return fromFields(telemetry.toFields());
}

Fields
ReadinessState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"heartbeat_seen", heartbeatSeen},
    {"flight_controller_ready", flightControllerReady},
    {"gps_ready", gpsReady},
    {"ekf_ready", ekfReady},
    {"battery_ready", batteryReady},
    {"armed", armed},
    {"mode", mode},
    {"landed_state_name", landedStateName},
    {"readiness", readiness},
    {"readiness_reason", readinessReason},
    {"ready_for_arm", readyForArm() ? "true" : "false"},
    {"ready_for_takeoff", readyForTakeoff() ? "true" : "false"},
    {"ready_for_land", readyForLand() ? "true" : "false"},
    {"ready_for_manual_control", readyForManualControl() ? "true" : "false"},
    {"timestamp_ms", std::to_string(timestampMs)},
  };
}

bool
ReadinessState::readyForArm() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         gpsReady == "true" &&
         ekfReady == "true" &&
         batteryReady == "true";
}

bool
ReadinessState::landedForTakeoff() const
{
  return landedStateName == "on-ground";
}

bool
ReadinessState::readyForTakeoff() const
{
  return readyForArm() && armed == "true" && landedForTakeoff();
}

bool
ReadinessState::readyForLand() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         armed == "true";
}

bool
ReadinessState::readyForManualControl() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         armed == "true";
}

std::string
ReadinessState::statusLine() const
{
  return "Readiness drone=" + droneId +
         " state=" + readiness +
         " reason=" + readinessReason +
         " heartbeat=" + heartbeatSeen +
         " fc=" + flightControllerReady +
         " gps=" + gpsReady +
         " ekf=" + ekfReady +
         " battery=" + batteryReady +
         " armed=" + armed +
         " landed=" + landedStateName;
}

FlightCommandState
FlightCommandState::fromFields(const Fields& fields)
{
  FlightCommandState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.command = fieldOr(fields, "command", state.command);
  state.accepted = fieldOr(fields, "accepted", state.accepted);
  state.ackResult = fieldOr(fields, "ack_result", fieldOr(fields, "ack", state.ackResult));
  state.flightControllerState = fieldOr(fields, "fc_state", state.flightControllerState);
  state.altitudeM = fieldOr(fields, "altitude_m", state.altitudeM);
  state.groundspeedMps = fieldOr(fields, "groundspeed_mps", state.groundspeedMps);
  state.batteryPercent = fieldOr(fields, "battery_percent", state.batteryPercent);
  state.forwardedBytes = fieldOr(fields, "forwarded_bytes", state.forwardedBytes);
  state.detail = fieldOr(fields, "detail", fieldOr(fields, "reason", state.detail));
  state.updatedMs = uint64FieldOr(fields, "updated_ms", state.updatedMs);
  if (state.updatedMs == 0) {
    state.updatedMs = uint64FieldOr(fields, "timestamp_ms", state.updatedMs);
  }
  return state;
}

Fields
FlightCommandState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"command", command},
    {"accepted", accepted},
    {"ack_result", ackResult},
    {"fc_state", flightControllerState},
    {"altitude_m", altitudeM},
    {"groundspeed_mps", groundspeedMps},
    {"battery_percent", batteryPercent},
    {"forwarded_bytes", forwardedBytes},
    {"detail", detail},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
FlightCommandState::isAccepted() const
{
  return accepted == "true";
}

bool
FlightCommandState::isTimeout() const
{
  return ackResult == "timeout";
}

bool
FlightCommandState::isSafetyCritical() const
{
  return command == "arm" ||
         command == "takeoff" ||
         command == "land" ||
         command == "emergency_stop" ||
         command == "manual_control";
}

std::string
FlightCommandState::statusLine() const
{
  return "Command drone=" + droneId +
         " command=" + command +
         " accepted=" + accepted +
         " ack=" + ackResult +
         " state=" + flightControllerState +
         " alt=" + altitudeM + "m" +
         " speed=" + groundspeedMps + "m/s" +
         " battery=" + batteryPercent + "%" +
         " bytes=" + forwardedBytes +
         " detail=" + detail;
}

SafetyState
SafetyState::fromFields(const Fields& fields)
{
  SafetyState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.linkState = fieldOr(fields, "link_state", state.linkState);
  state.manualControlState = fieldOr(fields, "manual_control_state", state.manualControlState);
  state.manualReplayActive = fieldOr(fields, "manual_replay_active", state.manualReplayActive);
  state.manualNeutralSent = fieldOr(fields, "manual_neutral_sent", state.manualNeutralSent);
  state.manualFreshForMs = uint64FieldOr(fields, "manual_fresh_for_ms", state.manualFreshForMs);
  state.manualReplayCount = uint64FieldOr(fields, "manual_replay_count", state.manualReplayCount);
  state.linkAgeMs = uint64FieldOr(fields, "link_age_ms", state.linkAgeMs);
  state.lostLinkAction = fieldOr(fields, "lost_link_action", state.lostLinkAction);
  state.detail = fieldOr(fields, "safety_detail", state.detail);
  state.updatedMs = uint64FieldOr(fields, "timestamp_ms", uint64FieldOr(fields, "updated_ms", state.updatedMs));
  return state;
}

SafetyState
SafetyState::fromTelemetry(const TelemetryState& telemetry)
{
  return fromFields(telemetry.toFields());
}

Fields
SafetyState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"link_state", linkState},
    {"manual_control_state", manualControlState},
    {"manual_replay_active", manualReplayActive},
    {"manual_neutral_sent", manualNeutralSent},
    {"manual_fresh_for_ms", std::to_string(manualFreshForMs)},
    {"manual_replay_count", std::to_string(manualReplayCount)},
    {"link_age_ms", std::to_string(linkAgeMs)},
    {"lost_link_action", lostLinkAction},
    {"safety_detail", detail},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
SafetyState::manualControlFresh() const
{
  return manualReplayActive == "true" &&
         manualControlState == "fresh" &&
         manualFreshForMs > 0;
}

bool
SafetyState::needsOperatorAttention() const
{
  return linkState == "lost" ||
         linkState == "stale" ||
         linkState == "waiting-heartbeat" ||
         manualControlState == "stale-waiting-neutral" ||
         manualControlState == "send-failed";
}

std::string
SafetyState::statusLine() const
{
  return "Safety drone=" + droneId +
         " link=" + linkState +
         " manual=" + manualControlState +
         " replay_active=" + manualReplayActive +
         " neutral_sent=" + manualNeutralSent +
         " fresh_for_ms=" + std::to_string(manualFreshForMs) +
         " replay_count=" + std::to_string(manualReplayCount) +
         " link_age_ms=" + std::to_string(linkAgeMs) +
         " lost_link_action=" + lostLinkAction +
         " attention=" + std::string(needsOperatorAttention() ? "yes" : "no") +
         " detail=" + detail;
}

namespace {

std::string
safetyBlockReason(const SafetyState& safety)
{
  if (safety.linkState == "lost" ||
      safety.linkState == "stale" ||
      safety.linkState == "waiting-heartbeat") {
    return "link-" + safety.linkState;
  }
  if (safety.manualControlState == "stale-waiting-neutral" ||
      safety.manualControlState == "send-failed") {
    return "manual-" + safety.manualControlState;
  }
  return "safety-attention";
}

void
setNoReadinessReasons(FlightSafetyGateState& state)
{
  state.armReason = "no-telemetry";
  state.takeoffReason = "no-telemetry";
  state.landReason = "no-telemetry";
  state.manualControlReason = "no-telemetry";
  state.controlPanelReason = "no-telemetry";
}

} // namespace

FlightSafetyGateState
FlightSafetyGateState::fromStates(const std::string& droneId,
                                  const std::optional<ReadinessState>& readiness,
                                  const std::optional<SafetyState>& safety)
{
  FlightSafetyGateState state;
  state.droneId = droneId.empty() ? "unknown" : droneId;
  state.hasReadiness = readiness.has_value();
  state.hasSafety = safety.has_value();
  state.canEmergencyStop = !droneId.empty();
  state.emergencyStopReason = state.canEmergencyStop ? "ok" : "no-drone";
  if (safety) {
    state.operatorAttention = safety->needsOperatorAttention();
    state.linkState = safety->linkState;
    state.manualControlState = safety->manualControlState;
  }
  if (!readiness) {
    setNoReadinessReasons(state);
    return state;
  }

  state.readiness = readiness->readiness;
  state.readinessReason = readiness->readinessReason;
  state.armed = readiness->armed;

  const auto safetyReason = safety && state.operatorAttention ?
                            safetyBlockReason(*safety) : std::string();
  auto blockIfAttention = [&safetyReason] (bool allowed, std::string& reason) {
    if (!allowed) {
      return false;
    }
    if (!safetyReason.empty()) {
      reason = safetyReason;
      return false;
    }
    reason = "ok";
    return true;
  };

  if (readiness->armed == "true") {
    state.armReason = "already-armed";
  }
  else if (!readiness->readyForArm()) {
    state.armReason = readiness->readinessReason;
  }
  else {
    state.canArm = blockIfAttention(true, state.armReason);
  }

  if (!readiness->readyForTakeoff()) {
    if (!readiness->readyForArm()) {
      state.takeoffReason = readiness->readinessReason;
    }
    else if (readiness->armed != "true") {
      state.takeoffReason = "not-armed";
    }
    else if (!readiness->landedForTakeoff()) {
      state.takeoffReason = "not-on-ground";
    }
    else {
      state.takeoffReason = "not-ready";
    }
  }
  else {
    state.canTakeoff = blockIfAttention(true, state.takeoffReason);
  }

  if (!readiness->readyForLand()) {
    state.landReason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
  }
  else {
    state.canLand = true;
    state.landReason = "ok";
  }

  if (!readiness->readyForManualControl()) {
    state.manualControlReason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
    state.controlPanelReason = state.manualControlReason;
  }
  else {
    state.canManualControl = blockIfAttention(true, state.manualControlReason);
    state.canControlPanel = blockIfAttention(true, state.controlPanelReason);
  }

  return state;
}

bool
FlightSafetyGateState::actionAllowed(const std::string& action, std::string& reason) const
{
  if (action == "arm") {
    reason = armReason;
    return canArm;
  }
  if (action == "takeoff") {
    reason = takeoffReason;
    return canTakeoff;
  }
  if (action == "land") {
    reason = landReason;
    return canLand;
  }
  if (action == "manual_control") {
    reason = manualControlReason;
    return canManualControl;
  }
  if (action == "control_panel") {
    reason = controlPanelReason;
    return canControlPanel;
  }
  if (action == "emergency_stop") {
    reason = emergencyStopReason;
    return canEmergencyStop;
  }
  reason = "ok";
  return true;
}

std::string
FlightSafetyGateState::statusLine() const
{
  return "FlightSafetyGate drone=" + droneId +
         " has_readiness=" + std::string(hasReadiness ? "true" : "false") +
         " has_safety=" + std::string(hasSafety ? "true" : "false") +
         " readiness=" + readiness +
         " reason=" + readinessReason +
         " armed=" + armed +
         " link=" + linkState +
         " manual=" + manualControlState +
         " attention=" + std::string(operatorAttention ? "true" : "false") +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " can_emergency_stop=" + std::string(canEmergencyStop ? "true" : "false");
}

FlightActionControlState
FlightActionControlState::fromGate(const FlightSafetyGateState& gate)
{
  FlightActionControlState state;
  state.selectedDrone = gate.droneId;
  state.hasReadiness = gate.hasReadiness;
  state.hasSafety = gate.hasSafety;
  state.operatorAttention = gate.operatorAttention;
  state.canArm = gate.canArm;
  state.canTakeoff = gate.canTakeoff;
  state.canLand = gate.canLand;
  state.canManualControl = gate.canManualControl;
  state.canControlPanel = gate.canControlPanel;
  state.canEmergencyStop = gate.canEmergencyStop;
  state.armReason = gate.armReason;
  state.takeoffReason = gate.takeoffReason;
  state.landReason = gate.landReason;
  state.manualControlReason = gate.manualControlReason;
  state.controlPanelReason = gate.controlPanelReason;
  state.emergencyStopReason = gate.emergencyStopReason;
  state.linkState = gate.linkState;
  state.manualControlState = gate.manualControlState;
  return state;
}

std::string
FlightActionControlState::statusLine() const
{
  return "FlightAction selected=" + selectedDrone +
         " has_readiness=" + std::string(hasReadiness ? "true" : "false") +
         " has_safety=" + std::string(hasSafety ? "true" : "false") +
         " safety_attention=" + std::string(operatorAttention ? "true" : "false") +
         " link=" + linkState +
         " manual_state=" + manualControlState +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " arm_reason=" + armReason +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " takeoff_reason=" + takeoffReason +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " land_reason=" + landReason +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " manual_reason=" + manualControlReason +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " panel_reason=" + controlPanelReason +
         " emergency_stop=" + std::string(canEmergencyStop ? "true" : "false") +
         " emergency_reason=" + emergencyStopReason;
}

VideoState
VideoState::fromFields(const Fields& fields)
{
  VideoState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.status = fieldOr(fields, "video", fieldOr(fields, "status", state.status));
  state.capture = fieldOr(fields, "capture", state.capture);
  state.recording = fieldOr(fields, "recording", state.recording);
  state.streamId = fieldOr(fields, "stream_id", state.streamId);
  state.encoding = fieldOr(fields, "encoding", state.encoding);
  state.source = fieldOr(fields, "source", state.source);
  state.requestedBitrateKbps = uint64FieldOr(fields, "requested_bitrate_kbps", state.requestedBitrateKbps);
  state.acceptedBitrateKbps = uint64FieldOr(fields, "accepted_bitrate_kbps", state.acceptedBitrateKbps);
  state.requestedFrameWidth = uint64FieldOr(fields, "requested_frame_width", state.requestedFrameWidth);
  state.acceptedFrameWidth = uint64FieldOr(fields, "accepted_frame_width",
                                           uint64FieldOr(fields, "frame_width", state.acceptedFrameWidth));
  state.fps = uint64FieldOr(fields, "fps", state.fps);
  state.streamPacketsPublished = uint64FieldOr(fields, "stream_packets_published",
                                               uint64FieldOr(fields, "packets", state.streamPacketsPublished));
  state.framesPublished = uint64FieldOr(fields, "frames_published", state.framesPublished);
  state.fecGroupsPublished = uint64FieldOr(fields, "fec_groups_published", state.fecGroupsPublished);
  state.recordingChunks = uint64FieldOr(fields, "recording_chunks", state.recordingChunks);
  state.recordingBytes = uint64FieldOr(fields, "recording_bytes", state.recordingBytes);
  state.rttMs = uint64FieldOr(fields, "rtt_ms", state.rttMs);
  state.timeoutPressure = uint64FieldOr(fields, "timeout_pressure", state.timeoutPressure);
  state.probePressure = uint64FieldOr(fields, "probe_pressure", state.probePressure);
  state.backlogPressure = uint64FieldOr(fields, "backlog_pressure", state.backlogPressure);
  state.decodedFrames = uint64FieldOr(fields, "decoded_frames", state.decodedFrames);
  state.updatedMs = uint64FieldOr(fields, "timestamp_ms",
                                  uint64FieldOr(fields, "video_updated_ms", state.updatedMs));
  return state;
}

Fields
VideoState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"video", status},
    {"capture", capture},
    {"recording", recording},
    {"stream_id", streamId},
    {"encoding", encoding},
    {"source", source},
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"accepted_bitrate_kbps", std::to_string(acceptedBitrateKbps)},
    {"requested_frame_width", std::to_string(requestedFrameWidth)},
    {"accepted_frame_width", std::to_string(acceptedFrameWidth)},
    {"fps", std::to_string(fps)},
    {"stream_packets_published", std::to_string(streamPacketsPublished)},
    {"frames_published", std::to_string(framesPublished)},
    {"fec_groups_published", std::to_string(fecGroupsPublished)},
    {"recording_chunks", std::to_string(recordingChunks)},
    {"recording_bytes", std::to_string(recordingBytes)},
    {"rtt_ms", std::to_string(rttMs)},
    {"timeout_pressure", std::to_string(timeoutPressure)},
    {"probe_pressure", std::to_string(probePressure)},
    {"backlog_pressure", std::to_string(backlogPressure)},
    {"decoded_frames", std::to_string(decodedFrames)},
    {"video_updated_ms", std::to_string(updatedMs)},
  };
}

bool
VideoState::isStreaming() const
{
  return status == "streaming";
}

std::string
VideoState::statusLine() const
{
  return "Video drone=" + droneId +
         " state=" + status +
         " capture=" + capture +
         " recording=" + recording +
         " stream=" + streamId +
         " bitrate=" + std::to_string(acceptedBitrateKbps) + "kbps" +
         " width=" + std::to_string(acceptedFrameWidth) +
         " packets=" + std::to_string(streamPacketsPublished) +
         " fec_groups=" + std::to_string(fecGroupsPublished) +
         " decoded=" + std::to_string(decodedFrames);
}

VideoControlState
VideoControlState::fromStates(const std::string& selectedDrone,
                              const std::optional<VideoState>& video,
                              bool displayActive)
{
  VideoControlState state;
  state.selectedDrone = selectedDrone.empty() ? "unknown" : selectedDrone;
  state.remoteStreaming = video && video->isStreaming();
  state.displayActive = displayActive;
  state.canStart = !state.remoteStreaming && !state.displayActive;
  state.canStop = state.remoteStreaming || state.displayActive;
  return state;
}

std::string
VideoControlState::statusLine() const
{
  return "VideoControl selected=" + selectedDrone +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " remote_streaming=" + std::string(remoteStreaming ? "true" : "false") +
         " display_active=" + std::string(displayActive ? "true" : "false");
}

VideoAdaptiveState
VideoAdaptiveState::fromFields(const Fields& fields)
{
  VideoAdaptiveState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.state = fieldOr(fields, "adaptive_state", fieldOr(fields, "state", state.state));
  state.rttMs = uint64FieldOr(fields, "rtt_ms", state.rttMs);
  state.requestedBitrateKbps = uint64FieldOr(fields, "requested_bitrate_kbps", state.requestedBitrateKbps);
  state.acceptedBitrateKbps = uint64FieldOr(fields, "accepted_bitrate_kbps", state.acceptedBitrateKbps);
  state.suggestedBitrateKbps = uint64FieldOr(fields, "suggested_bitrate_kbps", state.suggestedBitrateKbps);
  state.bitrateAction = fieldOr(fields, "bitrate_action", state.bitrateAction);
  state.bitrateReason = fieldOr(fields, "bitrate_reason", state.bitrateReason);
  state.window = uint64FieldOr(fields, "window", state.window);
  state.lookahead = uint64FieldOr(fields, "lookahead", state.lookahead);
  state.futureProbeLimit = uint64FieldOr(fields, "future_probe_limit", state.futureProbeLimit);
  state.interestLifetimeMs = uint64FieldOr(fields, "interest_lifetime_ms", state.interestLifetimeMs);
  state.missingTimeoutMs = uint64FieldOr(fields, "missing_timeout_ms", state.missingTimeoutMs);
  state.timeoutPressure = uint64FieldOr(fields, "timeout_pressure", state.timeoutPressure);
  state.probePressure = uint64FieldOr(fields, "probe_pressure", state.probePressure);
  state.duplicatePressure = uint64FieldOr(fields, "duplicate_pressure", state.duplicatePressure);
  state.lossPressure = uint64FieldOr(fields, "loss_pressure", state.lossPressure);
  state.backlogPressure = uint64FieldOr(fields, "backlog_pressure", state.backlogPressure);
  state.primaryPressure = fieldOr(fields, "primary_pressure", state.primaryPressure);
  state.policyReason = fieldOr(fields, "policy_reason", state.policyReason);
  state.pendingChunks = uint64FieldOr(fields, "pending_chunks", state.pendingChunks);
  state.receivedChunks = uint64FieldOr(fields, "received_chunks", state.receivedChunks);
  state.timeouts = uint64FieldOr(fields, "timeouts", state.timeouts);
  state.nacks = uint64FieldOr(fields, "nacks", state.nacks);
  state.duplicates = uint64FieldOr(fields, "duplicates", state.duplicates);
  state.decodedFrames = uint64FieldOr(fields, "decoded_frames", state.decodedFrames);
  state.updatedMs = uint64FieldOr(fields, "updated_ms", state.updatedMs);
  return state;
}

Fields
VideoAdaptiveState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"adaptive_state", state},
    {"rtt_ms", std::to_string(rttMs)},
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"accepted_bitrate_kbps", std::to_string(acceptedBitrateKbps)},
    {"suggested_bitrate_kbps", std::to_string(suggestedBitrateKbps)},
    {"bitrate_action", bitrateAction},
    {"bitrate_reason", bitrateReason},
    {"window", std::to_string(window)},
    {"lookahead", std::to_string(lookahead)},
    {"future_probe_limit", std::to_string(futureProbeLimit)},
    {"interest_lifetime_ms", std::to_string(interestLifetimeMs)},
    {"missing_timeout_ms", std::to_string(missingTimeoutMs)},
    {"timeout_pressure", std::to_string(timeoutPressure)},
    {"probe_pressure", std::to_string(probePressure)},
    {"duplicate_pressure", std::to_string(duplicatePressure)},
    {"loss_pressure", std::to_string(lossPressure)},
    {"backlog_pressure", std::to_string(backlogPressure)},
    {"primary_pressure", primaryPressure},
    {"policy_reason", policyReason},
    {"pending_chunks", std::to_string(pendingChunks)},
    {"received_chunks", std::to_string(receivedChunks)},
    {"timeouts", std::to_string(timeouts)},
    {"nacks", std::to_string(nacks)},
    {"duplicates", std::to_string(duplicates)},
    {"decoded_frames", std::to_string(decodedFrames)},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
VideoAdaptiveState::underPressure() const
{
  return maxPressure() >= 50;
}

uint64_t
VideoAdaptiveState::maxPressure() const
{
  return std::max({timeoutPressure, probePressure, duplicatePressure,
                   lossPressure, backlogPressure});
}

std::string
VideoAdaptiveState::compactSummary() const
{
  return "rtt=" + std::to_string(rttMs) +
         "ms,win=" + std::to_string(window) +
         ",pressure=" + std::to_string(maxPressure()) +
         "/" + primaryPressure +
         ",bitrate=" + std::to_string(acceptedBitrateKbps) +
         "->" + std::to_string(suggestedBitrateKbps) +
         "kbps/" + bitrateAction +
         ",reason=" + policyReason;
}

std::string
VideoAdaptiveState::statusLine() const
{
  return "VideoAdaptive drone=" + droneId +
         " state=" + state +
         " rtt_ms=" + std::to_string(rttMs) +
         " requested_bitrate_kbps=" + std::to_string(requestedBitrateKbps) +
         " accepted_bitrate_kbps=" + std::to_string(acceptedBitrateKbps) +
         " suggested_bitrate_kbps=" + std::to_string(suggestedBitrateKbps) +
         " bitrate_action=" + bitrateAction +
         " bitrate_reason=" + bitrateReason +
         " window=" + std::to_string(window) +
         " lookahead=" + std::to_string(lookahead) +
         " future_probe_limit=" + std::to_string(futureProbeLimit) +
         " interest_lifetime_ms=" + std::to_string(interestLifetimeMs) +
         " missing_timeout_ms=" + std::to_string(missingTimeoutMs) +
         " timeout_pressure=" + std::to_string(timeoutPressure) +
         " probe_pressure=" + std::to_string(probePressure) +
         " duplicate_pressure=" + std::to_string(duplicatePressure) +
         " loss_pressure=" + std::to_string(lossPressure) +
         " backlog_pressure=" + std::to_string(backlogPressure) +
         " primary_pressure=" + primaryPressure +
         " policy_reason=" + policyReason +
         " pending_chunks=" + std::to_string(pendingChunks) +
         " received_chunks=" + std::to_string(receivedChunks) +
         " timeouts=" + std::to_string(timeouts) +
         " nacks=" + std::to_string(nacks) +
         " duplicates=" + std::to_string(duplicates) +
         " decoded_frames=" + std::to_string(decodedFrames);
}

namespace {

uint64_t
policyRttMs(const VideoAdaptivePolicyInput& input)
{
  return std::clamp<uint64_t>(input.rttMs, 20, 2000);
}

uint64_t
policyFrameDurationMs(const VideoAdaptivePolicyInput& input)
{
  return std::max<uint64_t>(1, 1000 / std::max<uint64_t>(1, input.fps));
}

uint64_t
policyPacketsForDurationMs(const VideoAdaptivePolicyInput& input,
                           uint64_t durationMs, uint64_t minValue,
                           uint64_t maxValue)
{
  const auto packets = (std::max<uint64_t>(1, input.deltaPacketsPerSecond) *
                        durationMs + 999) / 1000;
  return std::clamp<uint64_t>(packets, minValue, maxValue);
}

uint64_t
policyLossPressurePercent(const VideoAdaptivePolicyInput& input)
{
  const auto received = input.receivedChunks;
  const auto losses = input.nacks + input.timeouts;
  if (received + losses < 20) {
    return 0;
  }
  return std::clamp<uint64_t>((losses * 100) /
                              std::max<uint64_t>(1, received + losses),
                              0, 80);
}

uint64_t
policyBacklogPressurePercent(const VideoAdaptivePolicyInput& input)
{
  if (input.decoderBacklogLimit == 0) {
    return 0;
  }
  return std::clamp<uint64_t>((input.decoderPendingChunks * 100) /
                              input.decoderBacklogLimit, 0, 100);
}

uint64_t
lowerVideoBitrateStep(uint64_t currentKbps)
{
  if (currentKbps > 6000) {
    return 6000;
  }
  if (currentKbps > 4000) {
    return 4000;
  }
  if (currentKbps > 2500) {
    return 2500;
  }
  if (currentKbps > 1500) {
    return 1500;
  }
  if (currentKbps > 800) {
    return 800;
  }
  return currentKbps;
}

uint64_t
higherVideoBitrateStep(uint64_t currentKbps, uint64_t requestedKbps)
{
  if (currentKbps < 800) {
    return std::min<uint64_t>(800, requestedKbps);
  }
  if (currentKbps < 1500) {
    return std::min<uint64_t>(1500, requestedKbps);
  }
  if (currentKbps < 2500) {
    return std::min<uint64_t>(2500, requestedKbps);
  }
  if (currentKbps < 4000) {
    return std::min<uint64_t>(4000, requestedKbps);
  }
  if (currentKbps < 6000) {
    return std::min<uint64_t>(6000, requestedKbps);
  }
  if (currentKbps < 8000) {
    return std::min<uint64_t>(8000, requestedKbps);
  }
  return std::min(currentKbps, requestedKbps);
}

std::string
primaryVideoPressure(uint64_t lossPressure, uint64_t timeoutPressure,
                     uint64_t duplicatePressure, uint64_t backlogPressure,
                     uint64_t probePressure)
{
  const auto primary = std::max({lossPressure, timeoutPressure, duplicatePressure,
                                 backlogPressure, probePressure});
  if (primary == 0) {
    return "none";
  }
  if (primary == timeoutPressure) {
    return "timeout";
  }
  if (primary == lossPressure) {
    return "loss";
  }
  if (primary == duplicatePressure) {
    return "duplicate";
  }
  if (primary == backlogPressure) {
    return "backlog";
  }
  return "probe";
}

} // namespace

VideoAdaptivePolicyDecision
computeVideoAdaptivePolicy(const VideoAdaptivePolicyInput& input)
{
  VideoAdaptivePolicyDecision decision;
  const auto rtt = policyRttMs(input);
  const auto frameMs = policyFrameDurationMs(input);
  const auto timeoutBudgetMs = std::clamp<uint64_t>(input.timeoutBudgetMs, 800, 6000);
  const auto dynamicWindowMax = std::max<uint64_t>(1, input.dynamicWindowMax);
  const auto dynamicLookaheadMax = std::max<uint64_t>(1, input.dynamicLookaheadMax);

  decision.lossPressure = policyLossPressurePercent(input);
  decision.backlogPressure = policyBacklogPressurePercent(input);
  decision.probePressure = std::max(input.probePressure,
                                    input.duplicatePressure / 2);
  decision.congestionPressure = std::max({
    decision.lossPressure,
    input.timeoutPressure,
    input.duplicatePressure / 2
  });
  decision.primaryPressure = primaryVideoPressure(decision.lossPressure,
                                                  input.timeoutPressure,
                                                  input.duplicatePressure / 2,
                                                  decision.backlogPressure,
                                                  decision.probePressure);

  const auto windowPressure = std::max(decision.congestionPressure,
                                       decision.backlogPressure);
  const auto windowTimeoutCapMs = std::clamp<uint64_t>(timeoutBudgetMs / 2, 350, 1000);
  const auto targetBufferMs = std::clamp<uint64_t>(rtt * 2 + frameMs * 2,
                                                  180, windowTimeoutCapMs);
  const auto windowPressureCap = windowPressure > 0 ?
    std::max<uint64_t>(16, dynamicWindowMax *
                           (100 - std::min<uint64_t>(windowPressure, 75)) / 100) :
    dynamicWindowMax;
  const auto minWindow = policyPacketsForDurationMs(
    input, std::clamp<uint64_t>(rtt / 2 + frameMs, 80, 300), 8, 128);
  decision.window = policyPacketsForDurationMs(input, targetBufferMs,
                                               std::min(minWindow, windowPressureCap),
                                               windowPressureCap);

  const auto lookaheadPressure = std::max({
    decision.congestionPressure,
    decision.probePressure,
    decision.backlogPressure
  });
  const auto lookaheadTimeoutCapMs = std::clamp<uint64_t>(timeoutBudgetMs / 5, 160, 500);
  const auto futureMs = std::clamp<uint64_t>(rtt + frameMs * 2, 100, lookaheadTimeoutCapMs);
  const auto lookaheadPressureCap = lookaheadPressure > 0 ?
    std::max<uint64_t>(4, dynamicLookaheadMax *
                          (100 - std::min<uint64_t>(lookaheadPressure, 85)) / 100) :
    dynamicLookaheadMax;
  decision.lookahead = policyPacketsForDurationMs(input, futureMs, 2,
                                                  lookaheadPressureCap);

  const auto rttProbeMs = std::clamp<uint64_t>(rtt / 3 + frameMs,
                                              frameMs, 180);
  auto probeLimit = policyPacketsForDurationMs(input, rttProbeMs, 1, 24);
  const auto probeLimitPressure = std::max(decision.probePressure,
                                           decision.congestionPressure);
  if (probeLimitPressure > 0) {
    probeLimit = std::max<uint64_t>(1, probeLimit *
                                      (100 - std::min<uint64_t>(probeLimitPressure, 90)) / 100);
  }
  decision.futureProbeLimit = std::clamp<uint64_t>(probeLimit, 1, 24);

  const auto pressureDelay =
    decision.probePressure * 8 + decision.congestionPressure * 4;
  decision.probeBackoffMs = std::clamp<uint64_t>(rtt / 2 + pressureDelay, 60, 1200);

  const auto interestLower = std::clamp<uint64_t>(rtt + frameMs * 2, 350, 1000);
  const auto interestUpper = std::clamp<uint64_t>(timeoutBudgetMs - 100,
                                                 interestLower, 3500);
  const auto lossSlackMs = std::min<uint64_t>(decision.congestionPressure * 8, 600);
  decision.interestLifetimeMs = std::clamp<uint64_t>(rtt * 2 + frameMs * 4 + 200 +
                                                    lossSlackMs,
                                                    interestLower, interestUpper);

  const auto minWaitMs = std::clamp<uint64_t>(std::max<uint64_t>(frameMs * 2, rtt / 2),
                                             100, 350);
  const auto maxWaitMs = std::clamp<uint64_t>(
    std::min<uint64_t>(timeoutBudgetMs / 2, rtt * 2 + frameMs * 4),
    300, 1800);
  const auto baseWaitMs = std::clamp<uint64_t>(rtt + frameMs * 3, minWaitMs, maxWaitMs);
  const auto waitPressureReductionMs =
    std::min<uint64_t>(decision.lossPressure * 3 + decision.backlogPressure * 2,
                       baseWaitMs / 2);
  decision.missingTimeoutMs = std::clamp<uint64_t>(baseWaitMs - waitPressureReductionMs,
                                                  minWaitMs, maxWaitMs);

  const auto requestedKbps = std::max<uint64_t>(128, input.requestedBitrateKbps);
  const auto acceptedKbps = std::max<uint64_t>(128, input.acceptedBitrateKbps);
  decision.suggestedBitrateKbps = acceptedKbps;
  const auto bitratePressure = std::max({
    decision.congestionPressure,
    decision.backlogPressure,
    decision.probePressure
  });
  const auto highRttThreshold = std::clamp<uint64_t>(
    timeoutBudgetMs / 3 + frameMs * 4, 350, 900);
  if ((bitratePressure >= 65 || rtt >= highRttThreshold) && acceptedKbps > 800) {
    decision.suggestedBitrateKbps = lowerVideoBitrateStep(acceptedKbps);
    decision.bitrateAction =
      decision.suggestedBitrateKbps < acceptedKbps ? "decrease" : "hold";
    decision.bitrateReason = bitratePressure >= 65 ? "pressure" : "high-rtt";
  }
  else if (bitratePressure <= 15 && rtt < highRttThreshold / 2 &&
           acceptedKbps < requestedKbps) {
    decision.suggestedBitrateKbps = higherVideoBitrateStep(acceptedKbps,
                                                           requestedKbps);
    decision.bitrateAction =
      decision.suggestedBitrateKbps > acceptedKbps ? "increase" : "hold";
    decision.bitrateReason = "recovery";
  }
  else {
    decision.bitrateReason = "stable";
  }

  if (decision.bitrateReason == "pressure") {
    decision.policyReason = "pressure-" + decision.primaryPressure;
  }
  else if (decision.bitrateReason == "high-rtt" ||
           decision.bitrateReason == "recovery") {
    decision.policyReason = decision.bitrateReason;
  }
  else if (bitratePressure > 0) {
    decision.policyReason = "pressure-" + decision.primaryPressure;
  }
  else {
    decision.policyReason = "stable";
  }

  return decision;
}

RecordingDataProductState
RecordingDataProductState::fromFields(const Fields& fields,
                                      const std::string& fallbackDroneId)
{
  RecordingDataProductState state;
  state.droneId = fieldOr(fields, "drone_id", fallbackDroneId.empty() ? state.droneId : fallbackDroneId);
  state.productType = fieldOr(fields, "product_type",
                              fieldOr(fields, "type", state.productType));
  if (state.productType == "camera-recording-manifest") {
    state.productType = "camera-recording";
  }
  state.sessionId = fieldOr(fields, "recording_session_id",
                            fieldOr(fields, "session_id", state.sessionId));
  state.objectPrefix = fieldOr(fields, "recording_object_prefix",
                               fieldOr(fields, "object_prefix", state.objectPrefix));
  state.encryption = fieldOr(fields, "recording_encryption",
                             fieldOr(fields, "encryption", state.encryption));
  state.keyId = fieldOr(fields, "recording_encryption_key_id",
                        fieldOr(fields, "key_id", state.keyId));
  state.contentKey = hexDecode(fieldOr(fields, "recording_encryption_content_key_hex",
                                       fieldOr(fields, "content_key_hex", "")));
  state.chunks = uint64FieldOr(fields, "recording_chunks",
                               uint64FieldOr(fields, "chunks", state.chunks));
  state.bytes = uint64FieldOr(fields, "recording_bytes",
                              uint64FieldOr(fields, "bytes", state.bytes));
  state.updatedMs = uint64FieldOr(fields, "updated_ms",
                                  uint64FieldOr(fields, "timestamp_ms", state.updatedMs));
  return state;
}

Fields
RecordingDataProductState::toFields(bool includeContentKey) const
{
  Fields fields{
    {"type", productType + "-manifest"},
    {"product_type", productType},
    {"drone_id", droneId},
    {"recording_session_id", sessionId},
    {"recording_object_prefix", objectPrefix},
    {"recording_encryption", encryption},
    {"recording_encryption_key_id", keyId},
    {"recording_chunks", std::to_string(chunks)},
    {"recording_bytes", std::to_string(bytes)},
    {"updated_ms", std::to_string(updatedMs)},
  };
  if (includeContentKey) {
    fields["recording_encryption_content_key_hex"] = hexEncode(contentKey);
  }
  return fields;
}

bool
RecordingDataProductState::isAvailable() const
{
  return !sessionId.empty() && !objectPrefix.empty() && chunks > 0;
}

bool
RecordingDataProductState::isEncrypted() const
{
  return encryption != "none" && !encryption.empty();
}

bool
RecordingDataProductState::isPlayable() const
{
  return isAvailable() && (!isEncrypted() || (!keyId.empty() && !contentKey.empty()));
}

std::string
RecordingDataProductState::chunkObjectName(uint64_t index) const
{
  if (objectPrefix.empty() || sessionId.empty()) {
    return "";
  }
  return objectPrefix + "/" + sessionId + "/chunk/" + std::to_string(index);
}

std::string
RecordingDataProductState::statusLine() const
{
  return "RecordingDataProduct drone=" + droneId +
         " type=" + productType +
         " session=" + sessionId +
         " prefix=" + objectPrefix +
         " chunks=" + std::to_string(chunks) +
         " bytes=" + std::to_string(bytes) +
         " encryption=" + encryption +
         " key_id=" + keyId +
         " key_bytes=" + std::to_string(contentKey.size()) +
         " available=" + std::string(isAvailable() ? "true" : "false") +
         " playable=" + std::string(isPlayable() ? "true" : "false");
}

MissionState
MissionState::fromFields(const Fields& fields)
{
  MissionState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.missionId = fieldOr(fields, "mission_id", fieldOr(fields, "active_mission_id", state.missionId));
  state.partId = fieldOr(fields, "part_id", fieldOr(fields, "active_mission_part", state.partId));
  state.phase = fieldOr(fields, "mission_phase", fieldOr(fields, "mission_status", state.phase));
  state.detail = fieldOr(fields, "mission_detail", fieldOr(fields, "status", state.detail));
  state.ack = fieldOr(fields, "mission_ack", state.ack);
  state.transport = fieldOr(fields, "mission_transport", state.transport);
  state.waypointsForwarded = fieldOr(fields, "waypoints_forwarded", state.waypointsForwarded);
  state.waypointAcksAccepted = fieldOr(fields, "waypoint_acks_accepted", state.waypointAcksAccepted);
  const auto updated = fieldOr(fields, "mission_updated_ms", "");
  if (!updated.empty()) {
    try {
      state.updatedMs = std::stoull(updated);
    }
    catch (const std::exception&) {
      state.updatedMs = 0;
    }
  }
  return state;
}

Fields
MissionState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"mission_id", missionId},
    {"part_id", partId},
    {"mission_phase", phase},
    {"mission_detail", detail},
    {"mission_ack", ack},
    {"mission_transport", transport},
    {"waypoints_forwarded", waypointsForwarded},
    {"waypoint_acks_accepted", waypointAcksAccepted},
    {"mission_updated_ms", std::to_string(updatedMs)},
  };
}

bool
MissionState::isIdle() const
{
  return phase == "idle" || phase == "none";
}

bool
MissionState::isUploading() const
{
  return phase == "uploading";
}

bool
MissionState::isUploaded() const
{
  return phase == "uploaded";
}

bool
MissionState::isExecuting() const
{
  return phase == "executing";
}

bool
MissionState::isStopping() const
{
  return phase == "stopping";
}

bool
MissionState::isCompleted() const
{
  return phase == "completed";
}

bool
MissionState::isFailed() const
{
  return phase == "failed";
}

bool
MissionState::isCancelled() const
{
  return phase == "cancelled";
}

bool
MissionState::isTerminal() const
{
  return isCompleted() || isFailed() || isCancelled();
}

bool
MissionState::isAssigned() const
{
  return isUploading() || isUploaded() || isExecuting() || isStopping();
}

bool
MissionState::isBusyForAssignment() const
{
  return isUploading() || isExecuting() || isStopping();
}

bool
MissionState::isStartable() const
{
  return isUploaded();
}

bool
MissionState::isStoppable() const
{
  return isUploaded() || isExecuting() || isStopping();
}

std::string
MissionState::statusLine() const
{
  return "Mission drone=" + droneId +
         " mission=" + missionId +
         " part=" + partId +
         " phase=" + phase +
         " detail=" + detail +
         " ack=" + ack +
         " waypoints=" + waypointsForwarded +
         " accepted=" + waypointAcksAccepted;
}

MissionStartGateState
MissionStartGateState::fromStates(const std::string& droneId,
                                  const std::optional<MissionState>& mission,
                                  const std::optional<FlightSafetyGateState>& flightGate)
{
  MissionStartGateState state;
  state.droneId = droneId.empty() ? "unknown" : droneId;
  state.hasMission = mission.has_value();
  state.hasFlightGate = flightGate.has_value();
  if (!mission) {
    return state;
  }

  state.missionPhase = mission->phase;
  state.missionUploaded = mission->isStartable();
  state.canStop = mission->isStoppable();
  state.stopReason = state.canStop ? "ok" : "mission-" + mission->phase;
  if (!mission->isStartable()) {
    state.startReason = "mission-" + mission->phase;
    return state;
  }
  if (!flightGate) {
    state.startReason = "no-flight-gate";
    return state;
  }
  if (flightGate->operatorAttention) {
    state.startReason = !flightGate->takeoffReason.empty() ? flightGate->takeoffReason :
                        !flightGate->armReason.empty() ? flightGate->armReason : "safety-attention";
    return state;
  }
  if (flightGate->canArm || flightGate->canTakeoff) {
    state.canStart = true;
    state.startReason = "ok";
    return state;
  }
  state.startReason = !flightGate->takeoffReason.empty() ? flightGate->takeoffReason :
                      !flightGate->armReason.empty() ? flightGate->armReason : "not-ready";
  return state;
}

std::string
MissionStartGateState::statusLine() const
{
  return "MissionStartGate drone=" + droneId +
         " has_mission=" + std::string(hasMission ? "true" : "false") +
         " has_flight_gate=" + std::string(hasFlightGate ? "true" : "false") +
         " mission_uploaded=" + std::string(missionUploaded ? "true" : "false") +
         " phase=" + missionPhase +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " start_reason=" + startReason +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " stop_reason=" + stopReason;
}

bool
MissionProgressState::isActive() const
{
  return phase == "assigning" ||
         phase == "waiting-compensation" ||
         phase == "compensating" ||
         phase == "executing";
}

bool
MissionProgressState::needsCompensation() const
{
  return missingParts > 0 && !isComplete() && !isFailed();
}

bool
MissionProgressState::isComplete() const
{
  return phase == "completed" ||
         (totalParts > 0 && completedParts >= totalParts && missingParts == 0);
}

bool
MissionProgressState::isFailed() const
{
  return phase == "failed";
}

namespace {

bool
commaSeparatedContains(const std::string& list, const std::string& value)
{
  if (value.empty()) {
    return false;
  }
  std::stringstream input(list);
  std::string token;
  while (std::getline(input, token, ',')) {
    if (token == value) {
      return true;
    }
  }
  return false;
}

} // namespace

bool
MissionProgressState::appliesToDrone(const std::string& droneId) const
{
  return drones == "all" || drones == droneId || commaSeparatedContains(drones, droneId);
}

std::string
MissionProgressState::statusLine() const
{
  return "MissionProgress task=" + taskId +
         " phase=" + phase +
         " assignment=" + assignment +
         " drones=" + drones +
         " attempts=" + std::to_string(attempts) +
         " total_parts=" + std::to_string(totalParts) +
         " completed_parts=" + std::to_string(completedParts) +
         " missing_parts=" + std::to_string(missingParts) +
         " compensated_parts=" + std::to_string(compensatedParts) +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false") +
         " completed=" + completedPartIds +
         " missing=" + missingPartIds +
         " compensated=" + compensatedPartIds +
         " pending=" + pendingPartIds;
}

namespace {

bool
missionPhaseTerminal(const std::string& phase)
{
  return phase == "completed" || phase == "failed" || phase == "cancelled";
}

void
appendCommaList(std::string& list, const std::string& value)
{
  if (!list.empty() && list != "none") {
    list += ",";
  }
  if (list == "none") {
    list.clear();
  }
  list += value;
}

} // namespace

MissionControlState
MissionControlState::fromStates(const std::vector<MissionStartGateState>& missionGates,
                                const std::optional<MissionProgressState>& progress,
                                bool uploadPending, bool startPending, bool stopPending)
{
  MissionControlState state;
  state.uploadPending = uploadPending;
  state.startPending = startPending;
  state.stopPending = stopPending;
  if (progress) {
    state.hasProgress = true;
    state.progressPhase = progress->phase;
    state.progressActive = progress->isActive();
    state.progressNeedsCompensation = progress->needsCompensation();
    state.progressComplete = progress->isComplete();
    state.progressFailed = progress->isFailed();
  }

  state.phases.clear();
  state.startEligible.clear();
  state.startBlocked.clear();
  for (const auto& gate : missionGates) {
    if (!gate.hasMission) {
      continue;
    }
    appendCommaList(state.phases, gate.droneId + ":" + gate.missionPhase);
    state.hasUploaded = state.hasUploaded || gate.missionUploaded;
    state.hasExecuting = state.hasExecuting || gate.missionPhase == "executing";
    state.hasStopping = state.hasStopping || gate.missionPhase == "stopping";
    state.hasTerminal = state.hasTerminal || missionPhaseTerminal(gate.missionPhase);
    if (gate.missionUploaded) {
      ++state.startableCount;
      if (gate.canStart) {
        appendCommaList(state.startEligible, gate.droneId);
        ++state.startEligibleCount;
      }
      else {
        appendCommaList(state.startBlocked, gate.droneId + ":" + gate.startReason);
        ++state.startBlockedCount;
      }
    }
  }
  if (state.phases.empty()) {
    state.phases = "none";
  }
  if (state.startEligible.empty()) {
    state.startEligible = "none";
  }
  if (state.startBlocked.empty()) {
    state.startBlocked = "none";
  }

  state.canUpload = !state.uploadPending && !state.startPending && !state.stopPending &&
                    !state.hasExecuting && !state.hasStopping && !state.progressActive;
  if (state.uploadPending) {
    state.uploadReason = "upload-pending";
  }
  else if (state.startPending) {
    state.uploadReason = "start-pending";
  }
  else if (state.stopPending) {
    state.uploadReason = "stop-pending";
  }
  else if (state.hasExecuting) {
    state.uploadReason = "mission-executing";
  }
  else if (state.hasStopping) {
    state.uploadReason = "mission-stopping";
  }
  else if (state.progressActive) {
    state.uploadReason = "progress-active";
  }
  else {
    state.uploadReason = "ok";
  }

  state.canStart = state.hasUploaded &&
                   state.startableCount > 0 &&
                   state.startEligibleCount == state.startableCount &&
                   state.startBlockedCount == 0 &&
                   !state.uploadPending && !state.startPending && !state.stopPending &&
                   !state.hasExecuting && !state.hasStopping &&
                   !state.progressActive &&
                   !state.progressNeedsCompensation &&
                   !state.progressFailed;
  state.canStop = !state.stopPending &&
                  (state.startPending || state.hasUploaded || state.hasExecuting ||
                   state.hasStopping || state.progressActive);
  if (!state.hasUploaded || state.startableCount == 0) {
    state.startReason = "no-uploaded-mission";
  }
  else if (state.startBlockedCount > 0) {
    state.startReason = "blocked-" + state.startBlocked;
  }
  else if (state.uploadPending) {
    state.startReason = "upload-pending";
  }
  else if (state.startPending) {
    state.startReason = "start-pending";
  }
  else if (state.stopPending) {
    state.startReason = "stop-pending";
  }
  else if (state.hasExecuting) {
    state.startReason = "mission-executing";
  }
  else if (state.hasStopping) {
    state.startReason = "mission-stopping";
  }
  else if (state.progressActive) {
    state.startReason = "progress-active";
  }
  else if (state.progressNeedsCompensation) {
    state.startReason = "progress-needs-compensation";
  }
  else if (state.progressFailed) {
    state.startReason = "progress-failed";
  }
  else {
    state.startReason = "ok";
  }

  if (state.stopPending) {
    state.stopReason = "stop-pending";
  }
  else {
    state.stopReason = state.canStop ? "ok" : "no-active-mission";
  }
  return state;
}

std::string
MissionControlState::statusLine() const
{
  return "MissionControl can_upload=" + std::string(canUpload ? "true" : "false") +
         " upload_reason=" + uploadReason +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " start_reason=" + startReason +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " stop_reason=" + stopReason +
         " upload_pending=" + std::string(uploadPending ? "true" : "false") +
         " start_pending=" + std::string(startPending ? "true" : "false") +
         " stop_pending=" + std::string(stopPending ? "true" : "false") +
         " startable_count=" + std::to_string(startableCount) +
         " start_eligible_count=" + std::to_string(startEligibleCount) +
         " start_blocked_count=" + std::to_string(startBlockedCount) +
         " phases=" + phases +
         " progress_phase=" + progressPhase;
}

SelectedActionState
SelectedActionState::fromStates(const std::string& selectedDrone,
                                const FlightActionControlState& flight,
                                const MissionControlState& mission,
                                bool manualMode,
                                bool manualInputActive)
{
  SelectedActionState state;
  state.selectedDrone = selectedDrone.empty() ? flight.selectedDrone : selectedDrone;
  state.flight = flight;
  state.mission = mission;
  state.manualMode = manualMode;
  state.manualInputActive = manualInputActive;
  state.emergencyStopAvailable = flight.canEmergencyStop;
  return state;
}

std::string
SelectedActionState::statusLine() const
{
  return "SelectedAction selected=" + selectedDrone +
         " can_arm=" + std::string(flight.canArm ? "true" : "false") +
         " can_takeoff=" + std::string(flight.canTakeoff ? "true" : "false") +
         " can_land=" + std::string(flight.canLand ? "true" : "false") +
         " can_manual=" + std::string(flight.canManualControl ? "true" : "false") +
         " can_panel=" + std::string(flight.canControlPanel ? "true" : "false") +
         " mission_can_start=" + std::string(mission.canStart ? "true" : "false") +
         " mission_start_reason=" + mission.startReason +
         " mission_can_stop=" + std::string(mission.canStop ? "true" : "false") +
         " mission_stop_reason=" + mission.stopReason +
         " mission_phases=" + mission.phases +
         " mission_progress=" + mission.progressPhase +
         " manual_mode=" + std::string(manualMode ? "true" : "false") +
         " manual_active=" + std::string(manualInputActive ? "true" : "false") +
         " emergency_stop=" + std::string(emergencyStopAvailable ? "true" : "false");
}

DroneListRowState
DroneListRowState::fromStates(const std::string& droneId,
                              bool selected,
                              const std::optional<TelemetryState>& telemetry,
                              const std::optional<ReadinessState>& readiness,
                              const std::optional<MissionState>& mission,
                              const std::optional<VideoState>& video,
                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                              const std::optional<FlightCommandState>& command,
                              const std::optional<SafetyState>& safety,
                              const std::optional<MissionProgressState>& progress)
{
  DroneListRowState state;
  state.droneId = droneId;
  state.selected = selected;
  state.hasTelemetry = telemetry.has_value();
  state.hasReadiness = readiness.has_value();
  state.hasMission = mission.has_value();
  state.hasVideo = video.has_value();
  state.hasCommand = command.has_value() && command->command != "none";
  state.hasSafety = safety.has_value();
  state.hasMissionProgress = progress && progress->appliesToDrone(droneId);
  state.hasVideoAdaptive = videoAdaptive.has_value();

  state.readiness = readiness ? readiness->readiness :
                    telemetry ? telemetry->readiness : "unknown";
  state.armed = readiness ? readiness->armed :
                telemetry ? telemetry->armed : "unknown";
  state.gps = readiness ? readiness->gpsReady :
              telemetry ? telemetry->gpsFixName : "unknown";
  state.battery = telemetry ? telemetry->batteryPercent + "%" : "unknown";
  state.mission = mission ? mission->phase : "idle";
  state.missionProgress = state.hasMissionProgress ? progress->phase : "idle";
  state.video = video ? video->status :
                telemetry ? telemetry->video : "unknown";
  state.videoAdaptive = videoAdaptive ? videoAdaptive->compactSummary() : "unknown";
  state.command = state.hasCommand ? command->command + ":" + command->ackResult : "none";
  state.safety = safety ? safety->manualControlState + "/" + safety->linkState : "unknown";

  state.rowText = std::string(selected ? "● " : "○ ") + "Drone " + droneId +
                  (selected ? " active" : " standby");
  if (state.hasReadiness || state.hasTelemetry) {
    state.rowText += " " + state.readiness +
                     " armed=" + state.armed +
                     " gps=" + state.gps;
  }
  if (state.hasTelemetry) {
    state.rowText += " bat=" + state.battery;
  }
  if (state.hasMission && state.mission != "idle") {
    state.rowText += " mission=" + state.mission;
  }
  if (state.hasMissionProgress && state.missionProgress != "idle") {
    state.rowText += " progress=" + state.missionProgress;
  }
  if ((state.hasVideo || state.hasTelemetry) && state.video != "unknown") {
    state.rowText += " video=" + state.video;
  }
  if (state.hasVideoAdaptive) {
    state.rowText += " adaptive=" + state.videoAdaptive;
  }
  if (state.hasCommand) {
    state.rowText += " cmd=" + state.command;
  }
  if (state.hasSafety) {
    state.rowText += " safe=" + state.safety;
  }
  return state;
}

std::string
MissionWaypoint::str() const
{
  std::ostringstream os;
  os << std::fixed << std::setprecision(6) << lat << "," << lon;
  return os.str();
}

MissionWaypoint
MissionPart::firstWaypointOr(MissionWaypoint fallback) const
{
  if (waypoints.empty()) {
    return fallback;
  }
  return waypoints.front();
}

std::vector<std::string>
MissionPart::waypointStrings() const
{
  std::vector<std::string> out;
  out.reserve(waypoints.size());
  for (const auto& waypoint : waypoints) {
    out.push_back(waypoint.str());
  }
  return out;
}

std::string
MissionPart::waypointText() const
{
  std::ostringstream os;
  os << (role.empty() ? "route" : role) << ":";
  for (size_t i = 0; i < waypoints.size(); ++i) {
    if (i > 0) {
      os << ">";
    }
    os << waypoints[i].str();
  }
  return os.str();
}

std::string
MissionPart::statusLine() const
{
  return "MissionPart id=" + id +
         " role=" + role +
         " drone=" + assignedDrone +
         " waypoints=" + std::to_string(waypoints.size()) +
         " attempt=" + std::to_string(attempt) +
         " done=" + std::string(done ? "true" : "false") +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false");
}

std::string
MissionPlan::droneList() const
{
  std::string out;
  for (const auto& part : parts) {
    if (part.assignedDrone.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += ",";
    }
    out += part.assignedDrone;
  }
  return out.empty() ? "none" : out;
}

std::string
MissionPlan::statusLine() const
{
  return "MissionPlan task=" + taskId +
         " assignment=" + assignment +
         " drones=" + droneList() +
         " parts=" + std::to_string(parts.size()) +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false");
}

SelectedDroneSummaryState
SelectedDroneSummaryState::fromStates(const std::string& selectedDrone,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<ReadinessState>& readiness,
                                      const std::optional<MissionState>& mission,
                                      const std::optional<MissionPlan>& missionPlan,
                                      const std::optional<MissionPart>& missionPart,
                                      const std::optional<MissionProgressState>& missionProgress,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<SafetyState>& safety)
{
  SelectedDroneSummaryState state;
  state.selectedDrone = selectedDrone.empty() ? "unknown" : selectedDrone;
  state.hasTelemetry = telemetry.has_value();
  state.readiness = readiness ? readiness->readiness :
                    telemetry ? telemetry->readiness : "unknown";
  state.missionPhase = mission ? mission->phase : "idle";
  state.missionProgressPhase = missionProgress ? missionProgress->phase : "idle";
  state.missionPlanTask = missionPlan ? missionPlan->taskId : "none";
  state.missionPartId = missionPart ? missionPart->id : "none";
  state.missionPartWaypoints = missionPart ? missionPart->waypoints.size() : 0;
  state.videoStatus = video ? video->status :
                      telemetry ? telemetry->video : "unknown";
  state.videoAdaptive = videoAdaptive ? videoAdaptive->compactSummary() : "unknown";
  state.linkState = safety ? safety->linkState :
                    telemetry ? telemetry->linkState : "unknown";

  const auto flight = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates(state.selectedDrone, readiness, safety));
  state.safetyAttention = flight.operatorAttention;
  state.canArm = flight.canArm;
  state.canTakeoff = flight.canTakeoff;
  state.canLand = flight.canLand;
  state.canManualControl = flight.canManualControl;
  state.canControlPanel = flight.canControlPanel;
  state.armReason = flight.armReason;
  state.takeoffReason = flight.takeoffReason;
  state.landReason = flight.landReason;
  state.manualControlReason = flight.manualControlReason;
  state.controlPanelReason = flight.controlPanelReason;
  return state;
}

std::string
SelectedDroneSummaryState::statusLine() const
{
  return "SelectedDroneSummary selected=" + selectedDrone +
         " has_telemetry=" + std::string(hasTelemetry ? "true" : "false") +
         " readiness=" + readiness +
         " mission=" + missionPhase +
         " mission_progress=" + missionProgressPhase +
         " mission_plan=" + missionPlanTask +
         " mission_part=" + missionPartId +
         " mission_part_waypoints=" + std::to_string(missionPartWaypoints) +
         " video=" + videoStatus +
         " video_adaptive=" + videoAdaptive +
         " link=" + linkState +
         " safety_attention=" + std::string(safetyAttention ? "true" : "false") +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " arm_reason=" + armReason +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " takeoff_reason=" + takeoffReason +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " land_reason=" + landReason +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " manual_reason=" + manualControlReason +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " panel_reason=" + controlPanelReason;
}

namespace {

double
missionDistanceSq(const MissionWaypoint& a, const MissionWaypoint& b, double referenceLat)
{
  const auto latScale = 111320.0;
  const auto lonScale = 111320.0 * std::max(0.2, std::cos(referenceLat * M_PI / 180.0));
  const auto dLat = (a.lat - b.lat) * latScale;
  const auto dLon = (a.lon - b.lon) * lonScale;
  return dLat * dLat + dLon * dLon;
}

std::vector<MissionWaypoint>
nearestNeighborMissionRoute(std::vector<MissionWaypoint> points, double referenceLat)
{
  std::vector<MissionWaypoint> route;
  if (points.empty()) {
    return route;
  }
  auto startIt = std::min_element(points.begin(), points.end(),
    [] (const MissionWaypoint& a, const MissionWaypoint& b) {
      if (a.lat == b.lat) {
        return a.lon < b.lon;
      }
      return a.lat < b.lat;
    });
  route.push_back(*startIt);
  points.erase(startIt);
  while (!points.empty()) {
    const auto current = route.back();
    auto nextIt = std::min_element(points.begin(), points.end(),
      [current, referenceLat] (const MissionWaypoint& a, const MissionWaypoint& b) {
        return missionDistanceSq(current, a, referenceLat) <
               missionDistanceSq(current, b, referenceLat);
      });
    route.push_back(*nextIt);
    points.erase(nextIt);
  }
  return route;
}

} // namespace

MissionPlan
buildPatrolMissionPlan(const std::string& taskId,
                       double centerLat,
                       double centerLon,
                       double sideMeters,
                       const std::vector<std::string>& droneIds,
                       const std::vector<MissionWaypoint>& routeWaypoints,
                       const std::map<std::string, MissionWaypoint>& departurePoints)
{
  MissionPlan plan;
  plan.taskId = taskId;
  plan.returnHomePlanned = true;
  if (droneIds.empty()) {
    return plan;
  }

  sideMeters = std::clamp(sideMeters, 40.0, 1000.0);
  const auto latStep = sideMeters / 111320.0;
  const auto lonStep = sideMeters / (111320.0 * std::max(0.2, std::cos(centerLat * M_PI / 180.0)));

  if (routeWaypoints.size() >= 2) {
    const size_t clusterCount = std::min(droneIds.size(), routeWaypoints.size());
    std::vector<MissionWaypoint> centers;
    centers.reserve(clusterCount);
    auto sorted = routeWaypoints;
    std::sort(sorted.begin(), sorted.end(), [] (const MissionWaypoint& a, const MissionWaypoint& b) {
      if (a.lon == b.lon) {
        return a.lat < b.lat;
      }
      return a.lon < b.lon;
    });
    for (size_t i = 0; i < clusterCount; ++i) {
      const size_t index = std::min(sorted.size() - 1, i * sorted.size() / clusterCount);
      centers.push_back(sorted[index]);
    }

    std::vector<size_t> assignments(routeWaypoints.size(), 0);
    for (int iteration = 0; iteration < 8; ++iteration) {
      std::vector<std::vector<MissionWaypoint>> groups(clusterCount);
      for (size_t pointIndex = 0; pointIndex < routeWaypoints.size(); ++pointIndex) {
        size_t best = 0;
        double bestDistance = missionDistanceSq(routeWaypoints[pointIndex], centers.front(), centerLat);
        for (size_t centerIndex = 1; centerIndex < centers.size(); ++centerIndex) {
          const auto candidateDistance =
            missionDistanceSq(routeWaypoints[pointIndex], centers[centerIndex], centerLat);
          if (candidateDistance < bestDistance) {
            best = centerIndex;
            bestDistance = candidateDistance;
          }
        }
        assignments[pointIndex] = best;
        groups[best].push_back(routeWaypoints[pointIndex]);
      }
      for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        if (groups[groupIndex].empty()) {
          continue;
        }
        MissionWaypoint nextCenter{};
        for (const auto& point : groups[groupIndex]) {
          nextCenter.lat += point.lat;
          nextCenter.lon += point.lon;
        }
        nextCenter.lat /= static_cast<double>(groups[groupIndex].size());
        nextCenter.lon /= static_cast<double>(groups[groupIndex].size());
        centers[groupIndex] = nextCenter;
      }
    }

    std::vector<std::vector<MissionWaypoint>> groups(clusterCount);
    for (size_t pointIndex = 0; pointIndex < routeWaypoints.size(); ++pointIndex) {
      groups[assignments[pointIndex]].push_back(routeWaypoints[pointIndex]);
    }
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
      if (groups[groupIndex].empty()) {
        continue;
      }
      MissionPart part;
      part.id = "part" + std::to_string(plan.parts.size());
      part.role = "waypoint-cluster-" + std::to_string(groupIndex);
      part.waypoints = nearestNeighborMissionRoute(groups[groupIndex], centerLat);
      part.returnHomePlanned = true;
      plan.parts.push_back(std::move(part));
    }
  }

  if (plan.parts.empty()) {
    const auto spacing = lonStep * 1.20;
    const auto startLon = centerLon - spacing * (static_cast<double>(droneIds.size()) - 1.0) / 2.0;
    plan.parts.reserve(droneIds.size());
    for (size_t i = 0; i < droneIds.size(); ++i) {
      const auto sectorLon = startLon + spacing * static_cast<double>(i);
      const auto sectorLat = centerLat - latStep / 2.0;
      MissionPart part;
      part.id = "part" + std::to_string(plan.parts.size());
      part.role = "patrol-cluster-" + std::to_string(i);
      part.waypoints = {
        {sectorLat, sectorLon - lonStep / 2.0},
        {sectorLat + latStep, sectorLon - lonStep / 2.0},
        {sectorLat + latStep, sectorLon + lonStep / 2.0},
        {sectorLat, sectorLon + lonStep / 2.0},
      };
      part.returnHomePlanned = true;
      plan.parts.push_back(std::move(part));
    }
  }

  for (size_t i = 0; i < plan.parts.size(); ++i) {
    auto& part = plan.parts[i];
    part.assignedDrone = droneIds[i % droneIds.size()];
    const auto fallback = part.firstWaypointOr(MissionWaypoint{centerLat, centerLon});
    const auto it = departurePoints.find(part.assignedDrone);
    part.waypoints.push_back(it == departurePoints.end() ? fallback : it->second);
  }

  return plan;
}

std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet)
{
  const auto header = encodeFields({
    {"capture_ms", std::to_string(packet.captureMs)},
    {"bucket_packet_count", std::to_string(packet.bucketPacketCount)},
    {"encoding", packet.encoding},
    {"frame_first_packet_seq", std::to_string(packet.frameFirstPacketSeq)},
    {"frame_last_packet_seq", std::to_string(packet.frameLastPacketSeq)},
    {"frame_segment_count", std::to_string(packet.frameSegmentCount)},
    {"frame_segment_index", std::to_string(packet.frameSegmentIndex)},
    {"frame_seq", std::to_string(packet.frameSeq)},
    {"key_frame", packet.keyFrame ? "true" : "false"},
    {"fec_data_shards", std::to_string(packet.fecDataShards)},
    {"fec_parity_shards", std::to_string(packet.fecParityShards)},
    {"fec_symbol_index", std::to_string(packet.fecSymbolIndex)},
    {"fec_symbol_count", std::to_string(packet.fecSymbolCount)},
    {"fec_data_lengths", packet.fecDataLengths},
    {"packet_seq", std::to_string(packet.packetSeq)},
    {"second", std::to_string(packet.second)},
  });
  if (header.size() > 0xffffffffULL) {
    throw std::runtime_error("video packet header too large");
  }

  std::vector<uint8_t> output;
  output.reserve(4 + header.size() + packet.payload.size());
  const auto headerSize = static_cast<uint32_t>(header.size());
  output.push_back(static_cast<uint8_t>((headerSize >> 24) & 0xff));
  output.push_back(static_cast<uint8_t>((headerSize >> 16) & 0xff));
  output.push_back(static_cast<uint8_t>((headerSize >> 8) & 0xff));
  output.push_back(static_cast<uint8_t>(headerSize & 0xff));
  output.insert(output.end(), header.begin(), header.end());
  output.insert(output.end(), packet.payload.begin(), packet.payload.end());
  return output;
}

VideoPacket
decodeVideoPacket(const std::vector<uint8_t>& payload)
{
  if (payload.size() < 4) {
    throw std::runtime_error("video packet too short");
  }
  const uint32_t headerSize =
    (static_cast<uint32_t>(payload[0]) << 24) |
    (static_cast<uint32_t>(payload[1]) << 16) |
    (static_cast<uint32_t>(payload[2]) << 8) |
    static_cast<uint32_t>(payload[3]);
  if (payload.size() < 4 + headerSize) {
    throw std::runtime_error("video packet header exceeds payload");
  }

  const auto header = decodeFields(std::string(
    reinterpret_cast<const char*>(payload.data() + 4), headerSize));
  VideoPacket packet;
  packet.second = std::stoull(fieldOr(header, "second", "0"));
  packet.packetSeq = std::stoull(fieldOr(header, "packet_seq", "0"));
  packet.frameSeq = std::stoull(fieldOr(header, "frame_seq", "0"));
  packet.captureMs = std::stoull(fieldOr(header, "capture_ms", "0"));
  packet.frameFirstPacketSeq = std::stoull(fieldOr(header, "frame_first_packet_seq",
                                                   std::to_string(packet.packetSeq)));
  packet.frameLastPacketSeq = std::stoull(fieldOr(header, "frame_last_packet_seq",
                                                  std::to_string(packet.packetSeq)));
  packet.bucketPacketCount = std::stoull(fieldOr(header, "bucket_packet_count",
                                                 std::to_string(packet.packetSeq + 1)));
  packet.frameSegmentIndex = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "frame_segment_index", "0")));
  packet.frameSegmentCount = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "frame_segment_count", "0")));
  packet.fecDataShards = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_data_shards", "0")));
  packet.fecParityShards = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_parity_shards", "0")));
  packet.fecSymbolIndex = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_symbol_index", "0")));
  packet.fecSymbolCount = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_symbol_count", "0")));
  packet.fecDataLengths = fieldOr(header, "fec_data_lengths", "");
  packet.keyFrame = fieldOr(header, "key_frame", "false") == "true";
  packet.encoding = fieldOr(header, "encoding", "");
  packet.payload.assign(payload.begin() + 4 + headerSize, payload.end());
  return packet;
}

std::vector<uint8_t>
buildMockMavlinkFrame(const std::string& commandName, const Fields& params)
{
  auto body = encodeFields(params);
  body = "magic=MAVLINK-MOCK-v1;command=" + commandName + ";" + body;
  std::vector<uint8_t> frame;
  frame.push_back(0xfe);
  frame.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xff));
  frame.push_back(static_cast<uint8_t>(body.size() & 0xff));
  frame.insert(frame.end(), body.begin(), body.end());
  uint8_t checksum = 0;
  for (const auto byte : frame) {
    checksum ^= byte;
  }
  frame.push_back(checksum);
  return frame;
}

namespace {

uint16_t
mavlinkCrcAccumulate(uint8_t data, uint16_t crc)
{
  data ^= static_cast<uint8_t>(crc & 0xff);
  data ^= static_cast<uint8_t>(data << 4);
  return static_cast<uint16_t>(
    (crc >> 8) ^
    (static_cast<uint16_t>(data) << 8) ^
    (static_cast<uint16_t>(data) << 3) ^
    (static_cast<uint16_t>(data) >> 4));
}

uint16_t
mavlinkCrcX25(const std::vector<uint8_t>& bytes, uint8_t extra)
{
  uint16_t crc = 0xffff;
  for (const auto byte : bytes) {
    crc = mavlinkCrcAccumulate(byte, crc);
  }
  return mavlinkCrcAccumulate(extra, crc);
}

void
appendFloatLe(std::vector<uint8_t>& out, float value)
{
  static_assert(sizeof(float) == 4, "MAVLink float must be 32 bits");
  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  out.push_back(static_cast<uint8_t>(raw & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 24) & 0xff));
}

void
appendUint16Le(std::vector<uint8_t>& out, uint16_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void
appendInt16Le(std::vector<uint8_t>& out, int16_t value)
{
  appendUint16Le(out, static_cast<uint16_t>(value));
}

void
appendUint32Le(std::vector<uint8_t>& out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void
appendInt32Le(std::vector<uint8_t>& out, int32_t value)
{
  appendUint32Le(out, static_cast<uint32_t>(value));
}

float
fieldFloatOr(const Fields& fields, const std::string& key, float fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  if (it->second == "true") {
    return 1.0F;
  }
  if (it->second == "false") {
    return 0.0F;
  }
  return std::stof(it->second);
}

uint8_t
fieldUint8Or(const Fields& fields, const std::string& key, uint8_t fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  return static_cast<uint8_t>(std::stoul(it->second));
}

std::vector<uint8_t>
buildMavlinkV1Frame(uint8_t msgId, uint8_t crcExtra, uint8_t sourceSystem,
                    uint8_t sourceComponent, std::vector<uint8_t> payload)
{
  constexpr uint8_t mavlinkStx = 0xfe;
  static uint8_t sequence = 0;

  std::vector<uint8_t> checksumInput;
  checksumInput.reserve(5 + payload.size());
  checksumInput.push_back(static_cast<uint8_t>(payload.size()));
  checksumInput.push_back(sequence);
  checksumInput.push_back(sourceSystem);
  checksumInput.push_back(sourceComponent);
  checksumInput.push_back(msgId);
  checksumInput.insert(checksumInput.end(), payload.begin(), payload.end());
  const auto crc = mavlinkCrcX25(checksumInput, crcExtra);

  std::vector<uint8_t> frame;
  frame.reserve(8 + payload.size());
  frame.push_back(mavlinkStx);
  frame.insert(frame.end(), checksumInput.begin(), checksumInput.end());
  frame.push_back(static_cast<uint8_t>(crc & 0xff));
  frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xff));
  ++sequence;
  return frame;
}

int16_t
fieldInt16ClampedOr(const Fields& fields, const std::string& key,
                    int16_t fallback, int16_t minValue, int16_t maxValue)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  const auto value = std::stoi(it->second);
  return static_cast<int16_t>(std::clamp(value, static_cast<int>(minValue),
                                        static_cast<int>(maxValue)));
}

uint16_t
fieldUint16ClampedOr(const Fields& fields, const std::string& key,
                     uint16_t fallback, uint16_t maxValue)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  const auto value = std::stoul(it->second);
  return static_cast<uint16_t>(std::min<unsigned long>(value, maxValue));
}

std::vector<uint8_t>
buildMavlinkManualControlFrame(const Fields& params)
{
  constexpr uint8_t manualControlMsgId = 69;
  constexpr uint8_t manualControlCrcExtra = 243;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  const auto x = fieldInt16ClampedOr(params, "x", 0, -1000, 1000);
  const auto y = fieldInt16ClampedOr(params, "y", 0, -1000, 1000);
  const auto z = fieldInt16ClampedOr(params, "z", 500, 0, 1000);
  const auto r = fieldInt16ClampedOr(params, "r", 0, -1000, 1000);
  const auto buttons = fieldUint16ClampedOr(params, "buttons", 0, 0xffff);

  std::vector<uint8_t> payload;
  payload.reserve(11);
  appendInt16Le(payload, x);
  appendInt16Le(payload, y);
  appendInt16Le(payload, z);
  appendInt16Le(payload, r);
  appendUint16Le(payload, buttons);
  payload.push_back(targetSystem);
  return buildMavlinkV1Frame(manualControlMsgId, manualControlCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkCommandLongFrame(const std::string& commandName, const Fields& params)
{
  constexpr uint8_t commandLongMsgId = 76;
  constexpr uint8_t commandLongCrcExtra = 152;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  uint16_t command = 0;
  std::array<float, 7> p = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  if (commandName == "arm") {
    command = 400; // MAV_CMD_COMPONENT_ARM_DISARM
    p[0] = fieldFloatOr(params, "arm", 1.0F);
  }
  else if (commandName == "disarm") {
    command = 400;
    p[0] = 0.0F;
  }
  else if (commandName == "emergency_stop") {
    command = 400; // MAV_CMD_COMPONENT_ARM_DISARM, force disarm magic.
    p[0] = 0.0F;
    p[1] = fieldFloatOr(params, "force_code", 21196.0F);
  }
  else if (commandName == "takeoff") {
    command = 22; // MAV_CMD_NAV_TAKEOFF
    p[6] = fieldFloatOr(params, "altitude_m", 15.0F);
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
  }
  else if (commandName == "land") {
    command = 21; // MAV_CMD_NAV_LAND
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
  }
  else if (commandName == "start_mission") {
    command = 300; // MAV_CMD_MISSION_START
    p[0] = fieldFloatOr(params, "first_item", 0.0F);
    p[1] = fieldFloatOr(params, "last_item", 0.0F);
  }
  else if (commandName == "goto" || commandName == "waypoint") {
    command = 16; // MAV_CMD_NAV_WAYPOINT
    p[0] = fieldFloatOr(params, "hold_time_s", 0.0F);
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
    p[6] = fieldFloatOr(params, "altitude_m", 15.0F);
  }
  else {
    return buildMockMavlinkFrame(commandName, params);
  }

  for (size_t i = 0; i < p.size(); ++i) {
    p[i] = fieldFloatOr(params, "param" + std::to_string(i + 1), p[i]);
  }

  std::vector<uint8_t> payload;
  payload.reserve(33);
  for (const auto value : p) {
    appendFloatLe(payload, value);
  }
  appendUint16Le(payload, command);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  payload.push_back(0); // confirmation
  return buildMavlinkV1Frame(commandLongMsgId, commandLongCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

} // namespace

std::vector<uint8_t>
buildMavlinkParamSetFrame(const std::string& paramName, float value,
                          uint8_t paramType, const Fields& params)
{
  constexpr uint8_t paramSetMsgId = 23;
  constexpr uint8_t paramSetCrcExtra = 168;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  std::vector<uint8_t> payload;
  payload.reserve(23);
  appendFloatLe(payload, value);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  for (size_t i = 0; i < 16; ++i) {
    payload.push_back(i < paramName.size() ? static_cast<uint8_t>(paramName[i]) : 0);
  }
  payload.push_back(paramType);
  return buildMavlinkV1Frame(paramSetMsgId, paramSetCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkHeartbeatFrame(const Fields& params)
{
  constexpr uint8_t heartbeatMsgId = 0;
  constexpr uint8_t heartbeatCrcExtra = 50;
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  const auto mavTypeGcs = fieldUint8Or(params, "type", 6);
  const auto autopilotInvalid = fieldUint8Or(params, "autopilot", 8);

  std::vector<uint8_t> payload;
  payload.reserve(9);
  appendUint32Le(payload, 0); // custom_mode
  payload.push_back(mavTypeGcs);
  payload.push_back(autopilotInvalid);
  payload.push_back(0); // base_mode
  payload.push_back(0); // system_status
  payload.push_back(3); // mavlink_version
  return buildMavlinkV1Frame(heartbeatMsgId, heartbeatCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkMissionCountFrame(uint16_t count, const Fields& params)
{
  constexpr uint8_t missionCountMsgId = 44;
  constexpr uint8_t missionCountCrcExtra = 221;
  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  std::vector<uint8_t> payload;
  payload.reserve(4);
  appendUint16Le(payload, count);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  return buildMavlinkV1Frame(missionCountMsgId, missionCountCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkMissionItemIntFrame(uint16_t seq, double latitude, double longitude,
                                float altitudeM, bool current,
                                const Fields& params)
{
  constexpr uint8_t missionItemIntMsgId = 73;
  constexpr uint8_t missionItemIntCrcExtra = 38;
  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  constexpr uint16_t mavCmdNavWaypoint = 16;
  constexpr uint8_t mavFrameGlobalRelativeAltInt = 6;

  std::vector<uint8_t> payload;
  payload.reserve(37);
  appendFloatLe(payload, fieldFloatOr(params, "hold_time_s", 0.0F));
  appendFloatLe(payload, fieldFloatOr(params, "acceptance_radius_m", 2.0F));
  appendFloatLe(payload, fieldFloatOr(params, "pass_radius_m", 0.0F));
  appendFloatLe(payload, fieldFloatOr(params, "yaw_deg", std::numeric_limits<float>::quiet_NaN()));
  appendInt32Le(payload, static_cast<int32_t>(std::llround(latitude * 10000000.0)));
  appendInt32Le(payload, static_cast<int32_t>(std::llround(longitude * 10000000.0)));
  appendFloatLe(payload, altitudeM);
  appendUint16Le(payload, seq);
  appendUint16Le(payload, mavCmdNavWaypoint);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  payload.push_back(mavFrameGlobalRelativeAltInt);
  payload.push_back(current ? 1 : 0);
  payload.push_back(1); // autocontinue
  return buildMavlinkV1Frame(missionItemIntMsgId, missionItemIntCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMockJpeg(const std::string& droneId, const std::string& frameId)
{
  const auto body = "mock-jpeg drone=" + droneId + " frame=" + frameId +
                    " timestamp_ms=" + std::to_string(nowMilliseconds());
  std::vector<uint8_t> image{0xff, 0xd8};
  image.insert(image.end(), body.begin(), body.end());
  image.push_back(0xff);
  image.push_back(0xd9);
  return image;
}

std::string
hexEncode(const std::vector<uint8_t>& value)
{
  std::ostringstream os;
  for (const auto byte : value) {
    os << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(byte);
  }
  return os.str();
}

std::vector<uint8_t>
hexDecode(const std::string& value)
{
  if (value.size() % 2 != 0) {
    throw std::runtime_error("invalid hex payload length");
  }
  std::vector<uint8_t> output;
  output.reserve(value.size() / 2);
  for (size_t i = 0; i < value.size(); i += 2) {
    output.push_back(static_cast<uint8_t>(
      std::stoi(value.substr(i, 2), nullptr, 16)));
  }
  return output;
}

std::string
makeMavlinkCommandPayload(const std::string& commandName,
                          const std::string& missionId,
                          const Fields& params)
{
  auto frame = buildMavlinkCommandLongFrame(commandName, params);
  if (commandName == "manual_control") {
    frame = buildMavlinkManualControlFrame(params);
  }
  Fields fields = params;
  fields["type"] = "mavlink-command";
  fields["command"] = commandName;
  fields["mavlink_encoding"] = "mavlink-mock";
  if (frame.size() > 5 && frame[0] == 0xfe) {
    if (frame[5] == 76) {
      fields["mavlink_encoding"] = "mavlink-v1-command-long";
    }
    else if (frame[5] == 69) {
      fields["mavlink_encoding"] = "mavlink-v1-manual-control";
    }
  }
  fields["mission_id"] = missionId;
  fields["timestamp_ms"] = std::to_string(nowMilliseconds());
  fields["mavlink_hex"] = hexEncode(frame);
  return encodeFields(fields);
}

std::string
makeMissionPayload(const std::string& missionId,
                   const std::string& role,
                   const std::string& area,
                   const std::vector<std::string>& waypoints,
                   bool captureRequired,
                   const std::string& objectDetectionService)
{
  std::ostringstream wp;
  for (size_t i = 0; i < waypoints.size(); ++i) {
    if (i > 0) {
      wp << '|';
    }
    wp << waypoints[i];
  }
  return encodeFields({
    {"type", "mission-plan"},
    {"mission_id", missionId},
    {"role", role},
    {"area", area},
    {"waypoints", wp.str()},
    {"capture_required", captureRequired ? "true" : "false"},
    {"object_detection_service", objectDetectionService},
  });
}

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback)
{
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

} // namespace ndnsf::examples::uav
