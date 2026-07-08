#ifndef NDNSF_EXAMPLES_UAV_GROUND_STATION_RUNTIME_STATE_HPP
#define NDNSF_EXAMPLES_UAV_GROUND_STATION_RUNTIME_STATE_HPP

#include "../shared/UavProtocol.hpp"

#include <cstdint>
#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

enum class RuntimeAvailability
{
  Unknown,
  Available,
  Unavailable
};

enum class CommandLifecycle
{
  Idle,
  Sending,
  AckWait,
  Running,
  Success,
  Timeout,
  Failed
};

inline const char*
to_string(CommandLifecycle lifecycle)
{
  switch (lifecycle) {
  case CommandLifecycle::Idle:
    return "IDLE";
  case CommandLifecycle::Sending:
    return "SENDING";
  case CommandLifecycle::AckWait:
    return "ACK_WAIT";
  case CommandLifecycle::Running:
    return "RUNNING";
  case CommandLifecycle::Success:
    return "SUCCESS";
  case CommandLifecycle::Timeout:
    return "TIMEOUT";
  case CommandLifecycle::Failed:
    return "FAILED";
  default:
    return "IDLE";
  }
}

enum class RuntimeConnectionState
{
  Unknown,
  Online,
  Stale,
  Offline
};

enum class NotReadyReason
{
  Certificate,
  FlightController,
  Camera,
  Repo
};

inline const char*
to_string(NotReadyReason reason)
{
  switch (reason) {
  case NotReadyReason::Certificate:
    return "Certificate";
  case NotReadyReason::FlightController:
    return "FlightController";
  case NotReadyReason::Camera:
    return "Camera";
  case NotReadyReason::Repo:
    return "Repo";
  default:
    return "Unknown";
  }
}

struct RuntimeCommandSnapshot
{
  std::string command = "none";
  CommandLifecycle lifecycle = CommandLifecycle::Idle;
  std::string detail = "idle";
  uint64_t updatedMs = 0;
  uint64_t rttMs = 0;
  uint64_t timeoutMs = 0;
};

struct OperatorAuthorityAlert
{
  std::string type = "unknown";
  std::string reason = "unknown";
  std::string leaseId = "none";
  std::string revokedOperator = "unknown";
  std::string revokerOperator = "unknown";
  std::string droneId = "unknown";
  std::string scope = "unknown";
  uint64_t updatedMs = 0;

  std::string
  statusLine() const
  {
    std::ostringstream os;
    os << type
       << " lease=" << leaseId
       << " revoked=" << revokedOperator
       << " revoker=" << revokerOperator
       << " drone=" << droneId
       << " scope=" << scope
       << " reason=" << reason;
    return os.str();
  }
};

struct VehicleRuntimeState
{
  std::string droneId = "unknown";
  RuntimeConnectionState connection = RuntimeConnectionState::Unknown;
  RuntimeAvailability telemetryReady = RuntimeAvailability::Unknown;
  RuntimeAvailability videoReady = RuntimeAvailability::Unknown;
  RuntimeAvailability cameraReady = RuntimeAvailability::Unknown;
  RuntimeAvailability flightControllerReady = RuntimeAvailability::Unknown;
  RuntimeAvailability missionReady = RuntimeAvailability::Unknown;
  RuntimeAvailability repoReady = RuntimeAvailability::Unknown;
  std::map<std::string, RuntimeCommandSnapshot> commandStates;
  std::vector<RuntimeCommandSnapshot> commandHistory;

  std::optional<TelemetryState> telemetry;
  std::optional<ReadinessState> readiness;
  std::optional<FlightCommandState> lastFlightCommand;
  std::optional<SafetyState> safety;
  std::optional<VideoState> video;
  std::optional<VideoAdaptiveState> videoAdaptive;
  std::optional<MissionState> mission;
  std::optional<MissionProgressState> missionProgress;
  std::vector<NotReadyReason> notReadyReasons;
  uint64_t updatedMs = 0;

  void
  clearNotReadyReasons()
  {
    notReadyReasons.clear();
  }

  void
  appendNotReadyReason(NotReadyReason reason)
  {
    if (std::find(notReadyReasons.begin(), notReadyReasons.end(), reason) == notReadyReasons.end()) {
      notReadyReasons.push_back(reason);
    }
  }

  bool
  ready() const
  {
    return notReadyReasons.empty();
  }

  void
  appendCommandHistory(RuntimeCommandSnapshot state, size_t maxEntries = 10)
  {
    commandHistory.push_back(std::move(state));
    if (commandHistory.size() > maxEntries) {
      commandHistory.erase(commandHistory.begin(),
                          commandHistory.end() - static_cast<long>(maxEntries));
    }
  }

  std::string
  notReadyReasonText() const
  {
    if (notReadyReasons.empty()) {
      return "Ready";
    }
    std::ostringstream os;
    for (size_t i = 0; i < notReadyReasons.size(); ++i) {
      if (i > 0) {
        os << ", ";
      }
      os << to_string(notReadyReasons[i]);
    }
    return os.str();
  }
};

struct GroundStationRuntimeState
{
  std::string selectedDroneId = "unknown";
  bool selectedDroneLocked = false;
  std::map<std::string, VehicleRuntimeState> drones;
  std::optional<MissionPlan> missionPlan;
  std::optional<MissionProgressState> missionProgress;
  std::vector<OperatorAuthorityAlert> operatorAuthorityAlerts;
  uint64_t updatedMs = 0;

  const VehicleRuntimeState*
  findDrone(const std::string& droneId) const
  {
    const auto it = drones.find(droneId);
    return it == drones.end() ? nullptr : &it->second;
  }

  VehicleRuntimeState&
  ensureDrone(const std::string& droneId)
  {
    auto& state = drones[droneId];
    state.droneId = droneId.empty() ? "unknown" : droneId;
    return state;
  }
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_GROUND_STATION_RUNTIME_STATE_HPP
