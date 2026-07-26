// App-internal implementation chunk included by UavDroneApp.cpp.
// Groups flight-controller backends, video publishing, and NDNSF services.

class FlightControllerBackend
{
public:
  virtual ~FlightControllerBackend() = default;
  virtual Fields sendMavlink(const std::vector<uint8_t>& frame,
                             const std::string& commandName) = 0;
  virtual Fields latestTelemetry() = 0;
  virtual VehicleParameterSnapshot parameterSnapshot() = 0;
  virtual VehicleParameterEditResult editParameter(const VehicleParameterEditRequest& request)
  {
    VehicleParameterEditResult result;
    result.requestId = request.requestId;
    result.droneId = request.droneId;
    result.parameterName = request.parameterName;
    result.valueType = request.valueType;
    result.requestedValue = request.requestedValue;
    result.accepted = false;
    result.applied = false;
    result.verified = false;
    result.reason = "parameter-edit-unsupported";
    result.updatedMs = nowMilliseconds();
    return result;
  }
  virtual Fields executeMissionWaypoints(const std::vector<std::pair<std::string, std::string>>& waypoints,
                                         const Fields& missionFields)
  {
    size_t forwardedWaypoints = 0;
    size_t acceptedWaypointAcks = 0;
    std::string lastWaypointAck = "none";
    for (const auto& [lat, lon] : waypoints) {
      auto params = missionFields;
      params["latitude"] = lat;
      params["longitude"] = lon;
      params.emplace("altitude_m", "15");
      const auto result = sendMavlink(
        hexDecode(fieldOr(decodeFields(makeMavlinkCommandPayload(
          "goto",
          fieldOr(missionFields, "mission_id", "mission") + "-" +
            fieldOr(missionFields, "part_id", "part") + "-" +
            std::to_string(forwardedWaypoints),
          params)), "mavlink_hex", "")),
        "mission-waypoint-goto");
      lastWaypointAck = fieldOr(result, "ack_result", "unknown");
      if (fieldOr(result, "accepted", "false") == "true" &&
          (lastWaypointAck == "accepted" || lastWaypointAck == "forwarded" ||
           lastWaypointAck == "mock-accepted")) {
        ++acceptedWaypointAcks;
      }
      ++forwardedWaypoints;
    }
    return {
      {"accepted", forwardedWaypoints == 0 || acceptedWaypointAcks > 0 ? "true" : "false"},
      {"mission_transport", forwardedWaypoints > 0 ? "mavlink-command-long-waypoints" : "mock"},
      {"waypoints_forwarded", std::to_string(forwardedWaypoints)},
      {"waypoint_acks_accepted", std::to_string(acceptedWaypointAcks)},
      {"last_waypoint_ack", lastWaypointAck},
    };
  }
  virtual std::string description() const = 0;
};

class MockFlightControllerBackend : public FlightControllerBackend
{
public:
  explicit MockFlightControllerBackend(std::string droneId)
    : m_droneId(std::move(droneId))
  {
    m_parameters = {
      {"COM_FAIL_ACT_T", "25"},
      {"COM_RC_LOSS_T", "30"},
      {"MPC_XY_VEL_MAX", "12"},
      {"NAV_RCL_ACT", "1"},
    };
  }

  Fields
  sendMavlink(const std::vector<uint8_t>& frame,
              const std::string& commandName) override
  {
    ++m_forwardedCount;
    bool accepted = true;
    if (commandName == "arm") {
      m_armed = true;
      m_airborne = false;
      m_altitudeTenths = 0;
    }
    else if (commandName == "disarm" || commandName == "emergency_stop") {
      m_armed = false;
      m_airborne = false;
      m_altitudeTenths = 0;
    }
    else if (commandName == "takeoff") {
      accepted = m_armed.load();
      if (accepted) {
        m_airborne = true;
        m_altitudeTenths = 150;
      }
    }
    else if (commandName == "land") {
      m_airborne = false;
      m_altitudeTenths = 0;
      m_armed = false;
    }
    else if (commandName == "manual_control" || commandName == "start_mission") {
      accepted = m_armed.load();
    }
    if (commandName == "manual_control") {
      if (accepted) {
        m_lastManualControlMs = nowMilliseconds();
        m_manualNeutralSent = false;
        m_manualControlRejected = false;
        ++m_manualReplayCount;
      }
      else {
        m_manualControlRejected = true;
      }
    }
    NDN_LOG_INFO("MOCK_FC_FORWARD drone=" << m_droneId
                 << " bytes=" << frame.size()
                 << " count=" << m_forwardedCount.load()
                 << " accepted=" << accepted);
    return {
      {"accepted", accepted ? "true" : "false"},
      {"ack_source", "mock"},
      {"ack_result", accepted ? "mock-accepted" : "mock-rejected"},
      {"command", commandName},
      {"fc_state", m_armed.load() ? "mock-armed" : "mock-disarmed"},
      {"altitude_m", std::to_string(m_altitudeTenths.load() / 10.0)},
      {"groundspeed_mps", "0.0"},
      {"battery_percent", "87.5"},
      {"armed", m_armed.load() ? "true" : "false"},
      {"landed_state_name", m_airborne.load() ? "in-air" : "on-ground"},
      {"forwarded_bytes", std::to_string(frame.size())},
    };
  }

  std::string
  description() const override
  {
    return "mock-flight-controller";
  }

  Fields
  latestTelemetry() override
  {
    const auto now = nowMilliseconds();
    const auto lastManual = m_lastManualControlMs.load();
    std::string manualState = "idle";
    std::string manualActive = "false";
    std::string manualNeutral = m_manualNeutralSent.load() ? "true" : "false";
    uint64_t manualFreshForMs = 0;
    std::string safetyDetail = "no-manual-input";
    if (m_manualControlRejected.load()) {
      manualState = "send-failed";
      safetyDetail = "manual-control-rejected";
    }
    else if (lastManual > 0 && now <= lastManual + 1500) {
      manualState = "fresh";
      manualActive = "true";
      manualNeutral = "false";
      manualFreshForMs = lastManual + 1500 - now;
      safetyDetail = "manual-control-fresh";
    }
    else if (lastManual > 0) {
      m_manualNeutralSent = true;
      manualNeutral = "true";
      manualState = "neutral-sent";
      safetyDetail = "neutral-after-timeout";
    }
    const bool armed = m_armed.load();
    const bool airborne = m_airborne.load();
    return {
      {"fc_state", "mock-ready"},
      {"lat", "35.1186"},
      {"lon", "-89.9375"},
      {"altitude_m", "42.0"},
      {"groundspeed_mps", "0.0"},
      {"battery_percent", "87.5"},
      {"heartbeat_seen", "true"},
      {"flight_controller_ready", "true"},
      {"gps_ready", "true"},
      {"ekf_ready", "true"},
      {"battery_ready", "true"},
      {"armed", armed ? "true" : "false"},
      {"landed_state_name", airborne ? "in-air" : "on-ground"},
      {"ready_for_takeoff", armed && !airborne ? "true" : "false"},
      {"readiness", "ready"},
      {"readiness_reason", "ok"},
      {"link_state", "connected"},
      {"manual_control_state", manualState},
      {"manual_replay_active", manualActive},
      {"manual_neutral_sent", manualNeutral},
      {"manual_fresh_for_ms", std::to_string(manualFreshForMs)},
      {"manual_replay_count", std::to_string(m_manualReplayCount.load())},
      {"safety_detail", safetyDetail},
    };
  }

  VehicleParameterSnapshot
  parameterSnapshot() override
  {
    std::lock_guard<std::mutex> guard(m_parameterMutex);
    VehicleParameterSnapshot snapshot;
    snapshot.droneId = m_droneId;
    snapshot.source = "mock-flight-controller-cache";
    snapshot.firmware = "MockPX4-1.14";
    snapshot.vehicleType = "quadrotor";
    snapshot.flightModes = "MANUAL,POSCTL,AUTO.MISSION";
    snapshot.parameters = m_parameters;
    snapshot.parameterCount = snapshot.parameters.size();
    snapshot.completePercent = 100;
    snapshot.updatedMs = nowMilliseconds();
    return snapshot;
  }

  VehicleParameterEditResult
  editParameter(const VehicleParameterEditRequest& request) override
  {
    VehicleParameterEditResult result;
    result.requestId = request.requestId;
    result.droneId = m_droneId;
    result.parameterName = request.parameterName;
    result.valueType = request.valueType;
    result.requestedValue = request.requestedValue;
    result.updatedMs = nowMilliseconds();

    std::string reason;
    if (!request.isValid(reason)) {
      result.reason = reason;
      return result;
    }

    std::lock_guard<std::mutex> guard(m_parameterMutex);
    const auto found = m_parameters.find(request.parameterName);
    if (found == m_parameters.end()) {
      result.reason = "parameter-not-found";
      return result;
    }
    result.previousValue = found->second;
    if (!request.expectedValue.empty() && request.expectedValue != found->second) {
      result.reason = "parameter-value-conflict";
      result.verifiedValue = found->second;
      return result;
    }
    result.accepted = true;
    if (request.dryRun) {
      result.reason = "dry-run";
      result.verifiedValue = found->second;
      result.verified = true;
      return result;
    }
    found->second = request.requestedValue;
    result.applied = true;
    result.verifiedValue = found->second;
    result.verified = result.verifiedValue == request.requestedValue;
    result.reason = result.verified ? "ok" : "verify-mismatch";
    return result;
  }

private:
  std::string m_droneId;
  std::atomic<size_t> m_forwardedCount{0};
  std::atomic<bool> m_armed{false};
  std::atomic<bool> m_airborne{false};
  std::atomic<int> m_altitudeTenths{0};
  std::atomic<uint64_t> m_lastManualControlMs{0};
  std::atomic<bool> m_manualNeutralSent{true};
  std::atomic<bool> m_manualControlRejected{false};
  std::atomic<size_t> m_manualReplayCount{0};
  std::mutex m_parameterMutex;
  Fields m_parameters;
};

class UdpFlightControllerBackend : public FlightControllerBackend
{
public:
  UdpFlightControllerBackend(std::string droneId, std::string host, std::string port,
                             std::string listenPort, bool configurePx4SitlDemoParams)
    : m_droneId(std::move(droneId))
    , m_transport("udp")
    , m_host(std::move(host))
    , m_port(std::move(port))
    , m_listenPort(std::move(listenPort))
    , m_configurePx4SitlDemoParams(configurePx4SitlDemoParams)
  {
  }

  UdpFlightControllerBackend(std::string droneId, std::string serialDevice,
                             std::string serialBaud)
    : m_droneId(std::move(droneId))
    , m_transport("serial")
    , m_host(std::move(serialDevice))
    , m_port(std::move(serialBaud))
  {
  }

  ~UdpFlightControllerBackend()
  {
    m_manualReplayDone = true;
    if (m_manualReplayThread.joinable()) {
      m_manualReplayThread.join();
    }
    if (m_socket >= 0) {
      close(m_socket);
    }
    if (m_listenSocket >= 0) {
      close(m_listenSocket);
    }
  }

  Fields
  sendMavlink(const std::vector<uint8_t>& frame,
              const std::string& commandName) override
  {
    std::lock_guard<std::mutex> guard(m_socketMutex);
    if (!ensureConnected()) {
      return {
        {"accepted", "false"},
        {"ack_source", m_transport},
        {"ack_result", "connect-failed"},
        {"command", commandName},
        {"forwarded_bytes", "0"},
      };
    }
    sendGcsHeartbeatIfNeededLocked();
    const auto n = sendFrameLocked(frame);
    if (n < 0 || static_cast<size_t>(n) != frame.size()) {
      NDN_LOG_WARN("UDP_FC_FORWARD_FAILED drone=" << m_droneId
                   << " host=" << m_host
                   << " port=" << m_port
                   << " bytes=" << frame.size());
      return {
        {"accepted", "false"},
        {"ack_source", m_transport},
        {"ack_result", "send-failed"},
        {"command", commandName},
        {"forwarded_bytes", std::to_string(frame.size())},
      };
    }
    ++m_forwardedCount;
    NDN_LOG_INFO("MAVLINK_FC_FORWARD drone=" << m_droneId
                 << " transport=" << m_transport
                 << " endpoint=" << m_host
                 << " port_or_baud=" << m_port
                 << " bytes=" << frame.size()
                 << " count=" << m_forwardedCount.load());
    if (commandName == "manual_control") {
      updateManualReplayLocked(frame);
    }
    auto result = commandName == "manual_control" ?
                  drainMavlinkTelemetry(std::chrono::milliseconds(5)) :
                  waitForCommandAck(commandName, std::chrono::milliseconds(700));
    if (commandName == "manual_control") {
      result["ack_result"] = "manual-control-forwarded";
    }
    const auto ackResult = fieldOr(result, "ack_result", "");
    const bool accepted = commandName == "manual_control" ||
                          ackResult == "accepted" || ackResult == "in-progress";
    result["accepted"] = accepted ? "true" : "false";
    result["ack_source"] = m_transport;
    result["command"] = commandName;
    result["forwarded_bytes"] = std::to_string(frame.size());
    result["fc_state"] = fieldOr(result, "ack_result", "forwarded");
    appendLatestTelemetry(result);
    return result;
  }

  std::string
  description() const override
  {
    if (m_transport == "serial") {
      return "serial://" + m_host + "@" + m_port;
    }
    return "udp://" + m_host + ":" + m_port;
  }

  Fields
  executeMissionWaypoints(const std::vector<std::pair<std::string, std::string>>& waypoints,
                          const Fields& missionFields) override
  {
    std::lock_guard<std::mutex> guard(m_socketMutex);
    if (!ensureConnected()) {
      return {
        {"accepted", "false"},
        {"mission_transport", "mavlink-mission-upload"},
        {"mission_ack", "connect-failed"},
        {"waypoints_forwarded", "0"},
        {"waypoint_acks_accepted", "0"},
        {"last_waypoint_ack", "connect-failed"},
      };
    }

    const auto altitudeM = std::stof(fieldOr(missionFields, "altitude_m", "15"));
    const auto count = static_cast<uint16_t>(std::min<size_t>(waypoints.size(), 65535));
    const auto countFrame = buildMavlinkMissionCountFrame(count, missionFields);
    sendFrameLocked(countFrame);
    NDN_LOG_INFO("UDP_FC_MISSION_COUNT drone=" << m_droneId
                 << " count=" << count);

    size_t itemRequests = 0;
    size_t sentItems = 0;
    std::string missionAck = "no-mission-ack";
    bool accepted = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
      std::array<pollfd, 2> pfds{};
      nfds_t fdCount = 0;
      pfds[fdCount].fd = m_socket;
      pfds[fdCount].events = POLLIN;
      ++fdCount;
      if (m_listenSocket >= 0) {
        pfds[fdCount].fd = m_listenSocket;
        pfds[fdCount].events = POLLIN;
        ++fdCount;
      }
      const int pollRc = poll(pfds.data(), fdCount, 200);
      if (pollRc <= 0) {
        continue;
      }
      auto event = drainMissionUploadPackets(pfds.data(), fdCount);
      if (event.type == MissionUploadEvent::Type::RequestItem && event.seq < count) {
        ++itemRequests;
        NDN_LOG_INFO("UDP_FC_MISSION_REQUEST drone=" << m_droneId
                     << " seq=" << event.seq);
        const auto& [latText, lonText] = waypoints[event.seq];
        const auto itemFrame = buildMavlinkMissionItemIntFrame(
          event.seq, std::stod(latText), std::stod(lonText), altitudeM, event.seq == 0,
          missionFields);
        if (sendFrameLocked(itemFrame) == static_cast<ssize_t>(itemFrame.size())) {
          ++sentItems;
          NDN_LOG_INFO("UDP_FC_MISSION_ITEM_SENT drone=" << m_droneId
                       << " seq=" << event.seq);
        }
      }
      else if (event.type == MissionUploadEvent::Type::Ack) {
        missionAck = event.ackResult;
        accepted = event.ackResult == "accepted";
        NDN_LOG_INFO("UDP_FC_MISSION_ACK drone=" << m_droneId
                     << " result=" << missionAck);
        break;
      }
    }
    if (!accepted) {
      NDN_LOG_WARN("UDP_FC_MISSION_UPLOAD_INCOMPLETE drone=" << m_droneId
                   << " count=" << count
                   << " item_requests=" << itemRequests
                   << " sent_items=" << sentItems
                   << " ack=" << missionAck);
    }

    Fields result{
      {"accepted", accepted ? "true" : "false"},
      {"mission_transport", "mavlink-mission-upload"},
      {"mission_ack", missionAck},
      {"waypoints_forwarded", std::to_string(sentItems)},
      {"waypoint_acks_accepted", accepted ? std::to_string(count) : "0"},
      {"last_waypoint_ack", missionAck},
      {"mission_item_requests", std::to_string(itemRequests)},
    };
    appendLatestTelemetry(result);
    return result;
  }

  Fields
  latestTelemetry() override
  {
    std::lock_guard<std::mutex> guard(m_socketMutex);
    if (m_socket < 0) {
      (void)ensureConnected();
    }
    if (m_socket >= 0) {
      sendGcsHeartbeatIfNeededLocked();
      (void)drainMavlinkTelemetry(std::chrono::milliseconds(250));
    }
    Fields result;
    appendLatestTelemetry(result);
    return result;
  }

  VehicleParameterSnapshot
  parameterSnapshot() override
  {
    std::lock_guard<std::mutex> guard(m_socketMutex);
    if (m_socket < 0) {
      (void)ensureConnected();
    }
    if (m_socket >= 0) {
      sendGcsHeartbeatIfNeededLocked();
      (void)drainMavlinkTelemetry(std::chrono::milliseconds(100));
    }
    VehicleParameterSnapshot snapshot;
    snapshot.droneId = m_droneId;
    snapshot.source = m_transport + "-flight-controller-cache";
    snapshot.firmware = "PX4-compatible";
    snapshot.vehicleType = "quadrotor";
    snapshot.flightModes = "MANUAL,POSCTL,AUTO.MISSION";
    snapshot.parameters = {
      {"COM_FAIL_ACT_T", "25"},
      {"COM_RC_LOSS_T", "30"},
      {"NAV_RCL_ACT", "1"},
    };
    snapshot.parameterCount = snapshot.parameters.size();
    snapshot.completePercent = m_configurePx4SitlDemoParams ? 100 : 25;
    snapshot.updatedMs = nowMilliseconds();
    return snapshot;
  }

private:
  ssize_t
  sendFrameLocked(const std::vector<uint8_t>& frame)
  {
    if (m_transport == "serial") {
      return write(m_socket, frame.data(), frame.size());
    }
    if (!m_udpRemoteReady) {
      errno = ENOTCONN;
      return -1;
    }
    return sendto(m_socket, frame.data(), frame.size(), 0,
                  reinterpret_cast<const sockaddr*>(&m_udpRemoteAddr),
                  m_udpRemoteAddrLen);
  }

  ssize_t
  receiveFrameLocked(int fd, uint8_t* buffer, size_t size)
  {
    if (m_transport == "serial") {
      return read(fd, buffer, size);
    }
    sockaddr_storage src{};
    socklen_t srcLen = sizeof(src);
    return recvfrom(fd, buffer, size, MSG_DONTWAIT,
                    reinterpret_cast<sockaddr*>(&src), &srcLen);
  }

  void
  updateManualReplayLocked(const std::vector<uint8_t>& frame)
  {
    m_latestManualFrame = frame;
    Fields neutralFields{{"x", "0"}, {"y", "0"}, {"z", "500"}, {"r", "0"}, {"buttons", "0"}};
    if (frame.size() > 16 && frame[0] == 0xfe && frame[5] == 69) {
      neutralFields["target_system"] = std::to_string(frame[16]);
    }
    m_neutralManualFrame = hexDecode(fieldOr(
      decodeFields(makeMavlinkCommandPayload("manual_control", "manual-neutral", neutralFields)),
      "mavlink_hex", ""));
    m_manualNeutralSent = false;
    m_manualReplayDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    if (!m_manualReplayThread.joinable()) {
      m_manualReplayThread = std::thread([this] {
        manualReplayLoop();
      });
    }
  }

  void
  sendGcsHeartbeatIfNeededLocked()
  {
    if (m_socket < 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextGcsHeartbeat) {
      return;
    }
    const auto heartbeat = buildMavlinkHeartbeatFrame();
    const auto n = sendFrameLocked(heartbeat);
    if (n == static_cast<ssize_t>(heartbeat.size())) {
      m_latestTelemetry["gcs_heartbeat_sent"] = "true";
      m_latestTelemetry["last_gcs_heartbeat_ms"] = std::to_string(nowMilliseconds());
    }
    m_nextGcsHeartbeat = now + std::chrono::seconds(1);
  }

  void
  manualReplayLoop()
  {
    while (!m_manualReplayDone.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::lock_guard<std::mutex> guard(m_socketMutex);
      if (m_socket < 0 || m_latestManualFrame.empty()) {
        continue;
      }
      if (std::chrono::steady_clock::now() > m_manualReplayDeadline) {
        if (!m_manualNeutralSent && !m_neutralManualFrame.empty()) {
          const auto n = sendFrameLocked(m_neutralManualFrame);
          if (n == static_cast<ssize_t>(m_neutralManualFrame.size())) {
            m_manualNeutralSent = true;
            NDN_LOG_INFO("MAVLINK_MANUAL_SAFETY_NEUTRAL drone=" << m_droneId);
          }
        }
        continue;
      }
      const auto n = sendFrameLocked(m_latestManualFrame);
      if (n == static_cast<ssize_t>(m_latestManualFrame.size())) {
        ++m_manualReplayCount;
      }
    }
  }

  static std::string
  mavlinkAckResultName(uint8_t result)
  {
    switch (result) {
      case 0:
        return "accepted";
      case 1:
        return "temporarily-rejected";
      case 2:
        return "denied";
      case 3:
        return "unsupported";
      case 4:
        return "failed";
      case 5:
        return "in-progress";
      case 6:
        return "cancelled";
      default:
        return "unknown-" + std::to_string(result);
    }
  }

  static std::string
  mavlinkSystemStatusName(uint8_t status)
  {
    switch (status) {
      case 0: return "uninitialized";
      case 1: return "boot";
      case 2: return "calibrating";
      case 3: return "standby";
      case 4: return "active";
      case 5: return "critical";
      case 6: return "emergency";
      case 7: return "poweroff";
      case 8: return "flight-termination";
      default: return "unknown-" + std::to_string(status);
    }
  }

  static std::string
  mavlinkGpsFixName(uint8_t fixType)
  {
    switch (fixType) {
      case 0: return "no-gps";
      case 1: return "no-fix";
      case 2: return "2d-fix";
      case 3: return "3d-fix";
      case 4: return "dgps";
      case 5: return "rtk-float";
      case 6: return "rtk-fixed";
      case 7: return "static";
      case 8: return "ppp";
      default: return "unknown-" + std::to_string(fixType);
    }
  }

  static std::string
  mavlinkLandedStateName(uint8_t state)
  {
    switch (state) {
      case 0: return "undefined";
      case 1: return "on-ground";
      case 2: return "in-air";
      case 3: return "takeoff";
      case 4: return "landing";
      default: return "unknown-" + std::to_string(state);
    }
  }

  static std::string
  mavlinkVtolStateName(uint8_t state)
  {
    switch (state) {
      case 0: return "undefined";
      case 1: return "transition-to-fw";
      case 2: return "transition-to-mc";
      case 3: return "mc";
      case 4: return "fw";
      default: return "unknown-" + std::to_string(state);
    }
  }

  static uint16_t
  readLe16(const uint8_t* value)
  {
    return static_cast<uint16_t>(value[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(value[1]) << 8);
  }

  static int16_t
  readI16(const uint8_t* value)
  {
    return static_cast<int16_t>(readLe16(value));
  }

  static uint32_t
  readLe32(const uint8_t* value)
  {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) |
           (static_cast<uint32_t>(value[3]) << 24);
  }

  static int32_t
  readI32(const uint8_t* value)
  {
    return static_cast<int32_t>(readLe32(value));
  }

  static float
  readFloatLe(const uint8_t* value)
  {
    float out = 0.0F;
    static_assert(sizeof(out) == 4, "float must be 32 bits");
    std::memcpy(&out, value, sizeof(out));
    return out;
  }

  static std::string
  formatDouble(double value, int precision = 2)
  {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(precision);
    os << value;
    return os.str();
  }

  static uint16_t
  commandIdForName(const std::string& commandName)
  {
    if (commandName == "arm" || commandName == "disarm" ||
        commandName == "emergency_stop") {
      return 400;
    }
    if (commandName == "takeoff") {
      return 22;
    }
    if (commandName == "land") {
      return 21;
    }
    if (commandName == "start_mission") {
      return 300;
    }
    if (commandName == "goto" || commandName == "waypoint" ||
        commandName == "mission-waypoint-goto") {
      return 16;
    }
    return 0;
  }

  Fields
  waitForCommandAck(const std::string& commandName, std::chrono::milliseconds timeout)
  {
    const auto wantedCommand = commandIdForName(commandName);
    if (wantedCommand == 0) {
      return {{"ack_result", "not-command-long"}};
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      std::array<pollfd, 2> pfds{};
      nfds_t fdCount = 0;
      pfds[fdCount].fd = m_socket;
      pfds[fdCount].events = POLLIN;
      ++fdCount;
      if (m_listenSocket >= 0) {
        pfds[fdCount].fd = m_listenSocket;
        pfds[fdCount].events = POLLIN;
        ++fdCount;
      }
      const int pollRc = poll(pfds.data(), fdCount,
                              static_cast<int>(std::max<int64_t>(1, remaining.count())));
      if (pollRc <= 0) {
        break;
      }
      auto ack = drainReadyMavlinkPackets(pfds.data(), fdCount, wantedCommand, commandName);
      if (!ack.empty()) {
        appendLatestTelemetry(ack);
        return ack;
      }
    }
    Fields result{{"ack_result", "no-command-ack"}};
    appendLatestTelemetry(result);
    return result;
  }

  Fields
  drainMavlinkTelemetry(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::array<pollfd, 2> pfds{};
      nfds_t fdCount = 0;
      pfds[fdCount].fd = m_socket;
      pfds[fdCount].events = POLLIN;
      ++fdCount;
      if (m_listenSocket >= 0) {
        pfds[fdCount].fd = m_listenSocket;
        pfds[fdCount].events = POLLIN;
        ++fdCount;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      const int pollRc = poll(pfds.data(), fdCount,
                              static_cast<int>(std::max<int64_t>(1, remaining.count())));
      if (pollRc <= 0) {
        break;
      }
      drainReadyMavlinkPackets(pfds.data(), fdCount, 0, "");
    }
    Fields result;
    appendLatestTelemetry(result);
    return result;
  }

  Fields
  drainReadyMavlinkPackets(pollfd* pfds, nfds_t fdCount,
                           uint16_t wantedCommand, const std::string& commandName)
  {
    std::array<uint8_t, 4096> buffer{};
    for (nfds_t fdIndex = 0; fdIndex < fdCount; ++fdIndex) {
      if ((pfds[fdIndex].revents & POLLIN) == 0) {
        continue;
      }
      while (true) {
        const auto n = receiveFrameLocked(pfds[fdIndex].fd, buffer.data(), buffer.size());
        if (n <= 0) {
          break;
        }
        auto ack = parseMavlinkFrames(buffer.data(), static_cast<size_t>(n),
                                      wantedCommand, commandName);
        if (!ack.empty()) {
          return ack;
        }
      }
    }
    return {};
  }

  Fields
  parseMavlinkFrames(const uint8_t* buffer, size_t size,
                     uint16_t wantedCommand, const std::string& commandName)
  {
    for (size_t i = 0; i + 8 <= size; ++i) {
      uint32_t msgId = 0;
      const uint8_t* payload = nullptr;
      size_t payloadLen = 0;
      size_t frameLen = 0;
      if (buffer[i] == 0xfe) {
        payloadLen = buffer[i + 1];
        frameLen = payloadLen + 8;
        if (i + frameLen > size) {
          break;
        }
        msgId = buffer[i + 5];
        payload = &buffer[i + 6];
      }
      else if (buffer[i] == 0xfd && i + 12 <= size) {
        payloadLen = buffer[i + 1];
        const bool signedFrame = (buffer[i + 2] & 0x01) != 0;
        frameLen = 10 + payloadLen + 2 + (signedFrame ? 13 : 0);
        if (i + frameLen > size) {
          break;
        }
        msgId = static_cast<uint32_t>(buffer[i + 7]) |
                (static_cast<uint32_t>(buffer[i + 8]) << 8) |
                (static_cast<uint32_t>(buffer[i + 9]) << 16);
        payload = &buffer[i + 10];
      }
      else {
        continue;
      }

      auto ack = parseMavlinkPayload(msgId, payload, payloadLen, wantedCommand, commandName);
      if (!ack.empty()) {
        return ack;
      }
      i += frameLen - 1;
    }
    return {};
  }

  struct MissionUploadEvent
  {
    enum class Type {
      None,
      RequestItem,
      Ack,
    };
    Type type = Type::None;
    uint16_t seq = 0;
    std::string ackResult;
  };

  MissionUploadEvent
  drainMissionUploadPackets(pollfd* pfds, nfds_t fdCount)
  {
    std::array<uint8_t, 4096> buffer{};
    for (nfds_t fdIndex = 0; fdIndex < fdCount; ++fdIndex) {
      if ((pfds[fdIndex].revents & POLLIN) == 0) {
        continue;
      }
      while (true) {
        const auto n = receiveFrameLocked(pfds[fdIndex].fd, buffer.data(), buffer.size());
        if (n <= 0) {
          break;
        }
        auto event = parseMissionUploadFrames(buffer.data(), static_cast<size_t>(n));
        if (event.type != MissionUploadEvent::Type::None) {
          return event;
        }
      }
    }
    return {};
  }

  MissionUploadEvent
  parseMissionUploadFrames(const uint8_t* buffer, size_t size)
  {
    for (size_t i = 0; i + 8 <= size; ++i) {
      uint32_t msgId = 0;
      const uint8_t* payload = nullptr;
      size_t payloadLen = 0;
      size_t frameLen = 0;
      if (buffer[i] == 0xfe) {
        payloadLen = buffer[i + 1];
        frameLen = payloadLen + 8;
        if (i + frameLen > size) {
          break;
        }
        msgId = buffer[i + 5];
        payload = &buffer[i + 6];
      }
      else if (buffer[i] == 0xfd && i + 12 <= size) {
        payloadLen = buffer[i + 1];
        const bool signedFrame = (buffer[i + 2] & 0x01) != 0;
        frameLen = 10 + payloadLen + 2 + (signedFrame ? 13 : 0);
        if (i + frameLen > size) {
          break;
        }
        msgId = static_cast<uint32_t>(buffer[i + 7]) |
                (static_cast<uint32_t>(buffer[i + 8]) << 8) |
                (static_cast<uint32_t>(buffer[i + 9]) << 16);
        payload = &buffer[i + 10];
      }
      else {
        continue;
      }

      if ((msgId == 40 || msgId == 51) && payloadLen >= 2) {
        return {MissionUploadEvent::Type::RequestItem, readLe16(payload), ""};
      }
      if (msgId == 47 && payloadLen >= 3) {
        return {MissionUploadEvent::Type::Ack, 0, mavlinkAckResultName(payload[2])};
      }
      auto ignored = parseMavlinkPayload(msgId, payload, payloadLen, 0, "");
      (void)ignored;
      i += frameLen - 1;
    }
    return {};
  }

  Fields
  parseMavlinkPayload(uint32_t msgId, const uint8_t* payload, size_t payloadLen,
                      uint16_t wantedCommand, const std::string& commandName)
  {
    if (msgId == 0 && payloadLen >= 9) {
      const auto baseMode = payload[6];
      const auto systemStatus = payload[7];
      m_latestTelemetry["heartbeat_seen"] = "true";
      m_latestTelemetry["last_heartbeat_ms"] = std::to_string(nowMilliseconds());
      m_latestTelemetry["armed"] = (baseMode & 0x80) != 0 ? "true" : "false";
      m_latestTelemetry["base_mode"] = std::to_string(baseMode);
      m_latestTelemetry["system_status"] = std::to_string(systemStatus);
      m_latestTelemetry["system_status_name"] = mavlinkSystemStatusName(systemStatus);
      m_latestTelemetry["fc_state"] = m_latestTelemetry["armed"] == "true" ? "armed" : "disarmed";
      m_latestTelemetry["flight_controller_ready"] =
        systemStatus >= 3 && systemStatus <= 4 ? "true" : "false";
    }
    else if (msgId == 1 && payloadLen >= 31) {
      const auto voltageMv = readLe16(payload + 14);
      const auto currentCa = readI16(payload + 20);
      const auto battery = static_cast<int8_t>(payload[30]);
      if (voltageMv != UINT16_MAX && voltageMv > 0) {
        m_latestTelemetry["battery_voltage_v"] = formatDouble(voltageMv / 1000.0, 2);
      }
      if (currentCa != -1) {
        m_latestTelemetry["battery_current_a"] = formatDouble(currentCa / 100.0, 2);
      }
      if (battery >= 0) {
        m_latestTelemetry["battery_percent"] = std::to_string(static_cast<int>(battery));
        m_latestTelemetry["battery_ready"] = battery > 15 ? "true" : "false";
      }
    }
    else if (msgId == 24 && payloadLen >= 30) {
      const auto fixType = payload[28];
      const auto satellites = payload[29];
      m_latestTelemetry["gps_fix_type"] = std::to_string(fixType);
      m_latestTelemetry["gps_fix_name"] = mavlinkGpsFixName(fixType);
      m_latestTelemetry["gps_satellites_visible"] = std::to_string(satellites);
      m_latestTelemetry["gps_ready"] = fixType >= 3 ? "true" : "false";
      m_latestTelemetry["ekf_ready"] = fixType >= 3 && satellites >= 6 ? "true" : "false";
      const auto latE7 = readI32(payload + 8);
      const auto lonE7 = readI32(payload + 12);
      const auto altMm = readI32(payload + 16);
      if (latE7 != 0 || lonE7 != 0) {
        m_latestTelemetry["lat"] = formatDouble(latE7 / 10000000.0, 7);
        m_latestTelemetry["lon"] = formatDouble(lonE7 / 10000000.0, 7);
      }
      m_latestTelemetry["gps_altitude_m"] = formatDouble(altMm / 1000.0);
    }
    else if (msgId == 32 && payloadLen >= 28) {
      const auto x = readFloatLe(payload + 4);
      const auto y = readFloatLe(payload + 8);
      const auto z = readFloatLe(payload + 12);
      const auto vx = readFloatLe(payload + 16);
      const auto vy = readFloatLe(payload + 20);
      const auto vz = readFloatLe(payload + 24);
      updateMapPositionFromLocalNed(x, y);
      m_latestTelemetry["altitude_m"] = formatDouble(-z);
      m_latestTelemetry["groundspeed_mps"] = formatDouble(std::sqrt(vx * vx + vy * vy + vz * vz));
    }
    else if (msgId == 33 && payloadLen >= 28) {
      const auto latE7 = readI32(payload + 4);
      const auto lonE7 = readI32(payload + 8);
      const auto relativeAltMm = readI32(payload + 16);
      const auto vx = readI16(payload + 20) / 100.0;
      const auto vy = readI16(payload + 22) / 100.0;
      m_latestTelemetry["lat"] = formatDouble(latE7 / 10000000.0, 7);
      m_latestTelemetry["lon"] = formatDouble(lonE7 / 10000000.0, 7);
      m_latestTelemetry["altitude_m"] = formatDouble(relativeAltMm / 1000.0);
      m_latestTelemetry["groundspeed_mps"] = formatDouble(std::sqrt(vx * vx + vy * vy));
    }
    else if (msgId == 245 && payloadLen >= 2) {
      const auto vtolState = payload[0];
      const auto landedState = payload[1];
      m_latestTelemetry["vtol_state"] = std::to_string(vtolState);
      m_latestTelemetry["vtol_state_name"] = mavlinkVtolStateName(vtolState);
      m_latestTelemetry["landed_state"] = std::to_string(landedState);
      m_latestTelemetry["landed_state_name"] = mavlinkLandedStateName(landedState);
    }
    else if (msgId == 77 && payloadLen >= 3 && wantedCommand != 0) {
      const auto command = readLe16(payload);
      const auto ackResult = payload[2];
      if (command == wantedCommand) {
        const auto resultName = mavlinkAckResultName(ackResult);
        NDN_LOG_INFO("UDP_FC_COMMAND_ACK drone=" << m_droneId
                     << " command=" << commandName
                     << " result=" << resultName);
        return {
          {"ack_result", resultName},
          {"ack_command_id", std::to_string(command)},
          {"ack_raw_result", std::to_string(ackResult)},
        };
      }
    }
    return {};
  }

  void
  appendLatestTelemetry(Fields& result) const
  {
    for (const auto& [key, value] : m_latestTelemetry) {
      result.emplace(key, value);
    }
    result.emplace("heartbeat_seen", "false");
    result.emplace("flight_controller_ready", "unknown");
    result.emplace("gps_ready", "unknown");
    result.emplace("ekf_ready", "unknown");
    result.emplace("battery_ready", "unknown");
    result.emplace("armed", "unknown");
    result.emplace("gps_fix_type", "unknown");
    result.emplace("gps_fix_name", "unknown");
    result.emplace("gps_satellites_visible", "unknown");
    result.emplace("system_status", "unknown");
    result.emplace("system_status_name", "unknown");
    result.emplace("landed_state", "unknown");
    result.emplace("landed_state_name", "unknown");
    result.emplace("vtol_state_name", "unknown");
    result.emplace("altitude_m", "unknown");
    result.emplace("groundspeed_mps", "unknown");
    result.emplace("battery_percent", "unknown");
    result.emplace("battery_voltage_v", "unknown");
    result.emplace("battery_current_a", "unknown");
    result["link_state"] = fieldOr(result, "heartbeat_seen", "false") == "true" ?
                           "connected" : "waiting-heartbeat";
    result.emplace("manual_replay_count", std::to_string(m_manualReplayCount.load()));
    if (m_latestManualFrame.empty()) {
      result["manual_control_state"] = "idle";
      result["manual_replay_active"] = "false";
      result["manual_neutral_sent"] = "true";
      result["manual_fresh_for_ms"] = "0";
      result["safety_detail"] = "no-manual-input";
    }
    else {
      const auto now = std::chrono::steady_clock::now();
      if (now <= m_manualReplayDeadline) {
        const auto freshForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          m_manualReplayDeadline - now).count();
        result["manual_control_state"] = "fresh";
        result["manual_replay_active"] = "true";
        result["manual_neutral_sent"] = "false";
        result["manual_fresh_for_ms"] = std::to_string(std::max<int64_t>(0, freshForMs));
        result["safety_detail"] = "manual-control-fresh";
      }
      else if (m_manualNeutralSent) {
        result["manual_control_state"] = "neutral-sent";
        result["manual_replay_active"] = "false";
        result["manual_neutral_sent"] = "true";
        result["manual_fresh_for_ms"] = "0";
        result["safety_detail"] = "neutral-after-timeout";
      }
      else {
        result["manual_control_state"] = "stale-waiting-neutral";
        result["manual_replay_active"] = "false";
        result["manual_neutral_sent"] = "false";
        result["manual_fresh_for_ms"] = "0";
        result["safety_detail"] = "manual-timeout-neutral-pending";
      }
    }
    auto state = TelemetryState::fromFields(result);
    if (state.timestampMs == 0) {
      state.timestampMs = nowMilliseconds();
    }
    for (const auto& [key, value] : state.toFields()) {
      result[key] = value;
    }
  }

  void
  updateMapPositionFromLocalNed(float northM, float eastM)
  {
    constexpr double metersPerDegreeLat = 111111.0;
    const double cosLat = std::max(0.01, std::cos(kDefaultHomeLat * M_PI / 180.0));
    const double lat = kDefaultHomeLat + static_cast<double>(northM) / metersPerDegreeLat;
    const double lon = kDefaultHomeLon + static_cast<double>(eastM) / (metersPerDegreeLat * cosLat);
    m_latestTelemetry["local_north_m"] = formatDouble(northM);
    m_latestTelemetry["local_east_m"] = formatDouble(eastM);
    m_latestTelemetry["lat"] = formatDouble(lat, 7);
    m_latestTelemetry["lon"] = formatDouble(lon, 7);
  }

  void
  ensureListenSocket()
  {
    if (m_transport == "serial") {
      return;
    }
    if (m_sendSocketBoundToListenPort) {
      return;
    }
    if (m_listenSocket >= 0 || m_listenPort.empty() || m_listenPort == "0") {
      return;
    }
    const auto portValue = static_cast<uint16_t>(std::stoul(m_listenPort));
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      NDN_LOG_WARN("UDP_FC_LISTEN_SOCKET_FAILED port=" << m_listenPort);
      return;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(portValue);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      NDN_LOG_WARN("UDP_FC_LISTEN_BIND_FAILED port=" << m_listenPort
                   << " errno=" << errno);
      close(fd);
      return;
    }
    m_listenSocket = fd;
    NDN_LOG_INFO("UDP_FC_LISTENING drone=" << m_droneId
                 << " port=" << m_listenPort);
  }

  bool
  bindSendSocketToListenPort(int fd)
  {
    if (m_listenPort.empty() || m_listenPort == "0") {
      return false;
    }
    const auto portValue = static_cast<uint16_t>(std::stoul(m_listenPort));
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(portValue);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      NDN_LOG_WARN("UDP_FC_SEND_BIND_FAILED port=" << m_listenPort
                   << " errno=" << errno);
      return false;
    }
    m_sendSocketBoundToListenPort = true;
    NDN_LOG_INFO("UDP_FC_SEND_BOUND drone=" << m_droneId
                 << " local_port=" << m_listenPort);
    return true;
  }

  bool
  ensureConnected()
  {
    if (m_socket >= 0) {
      ensureListenSocket();
      configurePx4SitlDemoParamsLocked();
      return true;
    }

    if (m_transport == "serial") {
      return ensureSerialConnected();
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const int rc = getaddrinfo(m_host.c_str(), m_port.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
      NDN_LOG_WARN("UDP_FC_RESOLVE_FAILED host=" << m_host
                   << " port=" << m_port
                   << " error=" << gai_strerror(rc));
      return false;
    }

    int fd = -1;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
      fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd < 0) {
        continue;
      }
      m_sendSocketBoundToListenPort = false;
      if (!m_listenPort.empty() && m_listenPort != "0" &&
          !bindSendSocketToListenPort(fd)) {
        close(fd);
        fd = -1;
        continue;
      }
      std::memset(&m_udpRemoteAddr, 0, sizeof(m_udpRemoteAddr));
      std::memcpy(&m_udpRemoteAddr, rp->ai_addr, rp->ai_addrlen);
      m_udpRemoteAddrLen = static_cast<socklen_t>(rp->ai_addrlen);
      m_udpRemoteReady = true;
      break;
    }
    freeaddrinfo(result);
    if (fd < 0) {
      m_udpRemoteReady = false;
      NDN_LOG_WARN("UDP_FC_SOCKET_FAILED host=" << m_host
                   << " port=" << m_port);
      return false;
    }
    m_socket = fd;
    ensureListenSocket();
    NDN_LOG_INFO("UDP_FC_READY drone=" << m_droneId
                 << " host=" << m_host
                 << " port=" << m_port
                 << " local_port=" << (m_listenPort.empty() ? "ephemeral" : m_listenPort));
    configurePx4SitlDemoParamsLocked();
    return true;
  }

  static speed_t
  baudToTermios(const std::string& baud)
  {
    const auto value = std::stoul(baud.empty() ? "57600" : baud);
    switch (value) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:
      throw std::runtime_error("unsupported serial baud " + baud);
    }
  }

  bool
  ensureSerialConnected()
  {
    const int fd = open(m_host.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
      NDN_LOG_WARN("SERIAL_FC_OPEN_FAILED device=" << m_host
                   << " errno=" << errno);
      return false;
    }

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
      NDN_LOG_WARN("SERIAL_FC_TCGETATTR_FAILED device=" << m_host
                   << " errno=" << errno);
      close(fd);
      return false;
    }
    cfmakeraw(&tty);
    const auto baud = baudToTermios(m_port);
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);
    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    tty.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
      NDN_LOG_WARN("SERIAL_FC_TCSETATTR_FAILED device=" << m_host
                   << " errno=" << errno);
      close(fd);
      return false;
    }
    tcflush(fd, TCIOFLUSH);
    m_socket = fd;
    NDN_LOG_INFO("SERIAL_FC_CONNECTED drone=" << m_droneId
                 << " device=" << m_host
                 << " baud=" << m_port);
    return true;
  }

  void
  configurePx4SitlDemoParamsLocked()
  {
    if (m_transport == "serial" ||
        !m_configurePx4SitlDemoParams || m_px4SitlDemoParamsConfigured || m_socket < 0) {
      return;
    }
    struct ParamSet
    {
      const char* name;
      float value;
      uint8_t type;
    };
    constexpr uint8_t mavParamTypeInt32 = 6;
    constexpr uint8_t mavParamTypeReal32 = 9;
    const std::array<ParamSet, 3> params{{
      {"COM_RC_LOSS_T", 30.0F, mavParamTypeReal32},
      {"COM_FAIL_ACT_T", 25.0F, mavParamTypeReal32},
      {"NAV_RCL_ACT", 1.0F, mavParamTypeInt32},
    }};
    for (const auto& param : params) {
      const auto frame = buildMavlinkParamSetFrame(param.name, param.value, param.type);
      const auto n = sendFrameLocked(frame);
      NDN_LOG_INFO("UDP_FC_DEMO_PARAM_SET drone=" << m_droneId
                   << " param=" << param.name
                   << " value=" << param.value
                   << " sent=" << (n == static_cast<ssize_t>(frame.size()) ? "true" : "false"));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    m_px4SitlDemoParamsConfigured = true;
  }

private:
  std::string m_droneId;
  std::string m_transport;
  std::string m_host;
  std::string m_port;
  std::string m_listenPort;
  bool m_configurePx4SitlDemoParams = false;
  bool m_px4SitlDemoParamsConfigured = false;
  int m_socket = -1;
  int m_listenSocket = -1;
  bool m_sendSocketBoundToListenPort = false;
  sockaddr_storage m_udpRemoteAddr{};
  socklen_t m_udpRemoteAddrLen = 0;
  bool m_udpRemoteReady = false;
  mutable std::mutex m_socketMutex;
  Fields m_latestTelemetry;
  static constexpr double kDefaultHomeLat = 35.1186;
  static constexpr double kDefaultHomeLon = -89.9375;
  std::vector<uint8_t> m_latestManualFrame;
  std::vector<uint8_t> m_neutralManualFrame;
  std::chrono::steady_clock::time_point m_manualReplayDeadline{};
  std::chrono::steady_clock::time_point m_nextGcsHeartbeat{};
  std::thread m_manualReplayThread;
  std::atomic<bool> m_manualReplayDone{false};
  bool m_manualNeutralSent = true;
  std::atomic<size_t> m_forwardedCount{0};
  std::atomic<size_t> m_manualReplayCount{0};
};

class VideoPublisher
{
public:
  struct CameraRuntimeOptions
  {
    bool captureOnStart = false;
    bool recordToLocalRepo = false;
    std::string recordRepoPath;
    std::string recordObjectPrefix;
    uint64_t recordPacketLimit = 0;
    std::string v4l2InputFormat = "auto";
    std::string v4l2InputSize = "auto";
    uint64_t v4l2InputFps = 0;
  };

  VideoPublisher(ndn_service_framework::ServiceProvider& serviceProvider,
                 ndn::Face& face, ndn::KeyChain& keyChain,
                 ndn_service_framework::LocalServiceRegistry& localRegistry,
                 ndn::Name localArchivedPacketServiceName,
                 UavRuntimeConfig config, std::string droneId, std::string videoPath,
                 CameraRuntimeOptions options)
    : m_serviceProvider(serviceProvider)
    , m_face(face)
    , m_keyChain(keyChain)
    , m_localRegistry(localRegistry)
    , m_localArchivedPacketServiceName(std::move(localArchivedPacketServiceName))
    , m_config(std::move(config))
    , m_droneId(std::move(droneId))
    , m_videoPath(std::move(videoPath))
    , m_cameraOptions(std::move(options))
  {
    if (const auto* mode = std::getenv("NDNSF_UAV_ENCODER_PIPE_READ_MODE")) {
      m_useLegacyBatchedEncoderRead = std::string(mode) == "stdio-batched";
    }
    if (const auto* backend = std::getenv("NDNSF_UAV_VIDEO_PIPELINE")) {
      m_useGStreamerPipeline = std::string(backend) == "gstreamer";
    }
    m_signingInfo = ndn_service_framework::makeEcdsaPreferredSigningInfo(
      m_keyChain, droneIdentity(m_config, m_droneId));
    m_streamPrefix = droneIdentity(m_config, m_droneId).append("video").append(m_streamId);
    if (m_cameraOptions.recordObjectPrefix.empty()) {
      m_cameraOptions.recordObjectPrefix = droneIdentity(m_config, m_droneId)
        .append("repo")
        .append("camera")
        .append("recording")
        .toUri();
    }
    if (m_cameraOptions.recordToLocalRepo) {
      ndnsf_distributed_repo::StorageCapability capability;
      capability.repoNode = droneIdentity(m_config, m_droneId).append("local-repo").toUri();
      capability.freeBytes = 4'000'000'000ULL;
      capability.storageClasses = {"video", "camera-recording"};
      if (m_cameraOptions.recordRepoPath.empty()) {
        auto repoId = m_droneId;
        std::replace(repoId.begin(), repoId.end(), '/', '_');
        m_cameraOptions.recordRepoPath =
          "/tmp/ndnsf-uav-" + repoId + "-camera.sqlite3";
      }
      m_recordingRepo = std::make_unique<ndnsf_distributed_repo::RepoCore>(
        capability,
        ndnsf_distributed_repo::makeTieredRepoStore(
          m_cameraOptions.recordRepoPath, 64 * 1024 * 1024));
      for (const auto& object : m_recordingRepo->list()) {
        if (object.objectType == "video/h264-chunk" ||
            object.objectName.find("/chunk/") != std::string::npos) {
          m_legacyRecordingDetected = true;
          NDN_LOG_WARN("CAMERA_RECORDING_LEGACY_DB_UNSUPPORTED path="
                       << m_cameraOptions.recordRepoPath
                       << " action=export-with-pre-spec120-or-delete-old-repo");
          break;
        }
      }
      if (!m_legacyRecordingDetected) {
        try {
          recoverLatestCompletedRecording();
        }
        catch (const std::exception& e) {
          // Historical material is never guessed or silently downgraded. A
          // new live session may still run, but replay remains unavailable.
          NDN_LOG_WARN("CAMERA_RECORDING_RECOVERY_REJECTED path="
                       << m_cameraOptions.recordRepoPath << " reason=" << e.what());
        }
      }
      m_recordingSessionId = "record-" + std::to_string(nowMilliseconds());
      m_recordingLastUpdateMs = nowMilliseconds();
      m_retentionActive = true;
      m_localRegistry.registerLocalService(
        m_localArchivedPacketServiceName,
        [this](const ndn::Name&,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage& request) {
          const auto objectName = payloadToString(request);
          const auto payload = archivedPacketWire(objectName);
          return makeBinaryResponse(!payload.empty(), payload,
                                    payload.empty() ? "archived signed packet not found" : "No error");
        });
      ensureRecordingFilterRegistered();
      NDN_LOG_INFO("CAMERA_RECORDING_CANONICAL drone=" << m_droneId
                   << " object_type=application/ndn-data"
                   << " media_encryption=live-stream-epoch-key"
                   << " storage_mode=exact-signed-wire");
    }
    m_captureEnabled = m_cameraOptions.captureOnStart || m_cameraOptions.recordToLocalRepo;
    if (m_recordingRepo) {
      m_retentionThread = std::thread([this] { this->retentionLoop(); });
    }
    m_captureThread = std::thread([this] { this->captureLoop(); });
  }

  ~VideoPublisher()
  {
    shutdown();
  }

  void
  shutdown()
  {
    m_done = true;
    m_streaming = false;
    m_captureEnabled = false;
    if (m_captureThread.joinable()) {
      m_captureThread.join();
    }
    if (m_retentionThread.joinable()) {
      m_retentionThread.join();
    }
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (m_recordingPacketFeed) m_recordingPacketFeed->close();
    }
    try {
      persistCanonicalManifest(true);
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_CANONICAL_FINAL_MANIFEST_FAILED reason=" << e.what());
    }
  }

  static ndn::Buffer
  randomBytes(size_t size)
  {
    ndn::Buffer value(size);
    if (size == 0 || RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) {
      throw std::runtime_error("RAND_bytes failed for live video session");
    }
    return value;
  }

  static std::string
  lowerHex(const ndn::Buffer& value)
  {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto byte : value) {
      result.push_back(DIGITS[byte >> 4]);
      result.push_back(DIGITS[byte & 0x0f]);
    }
    return result;
  }

  static uint64_t
  randomNonZeroUint64()
  {
    const auto bytes = randomBytes(sizeof(uint64_t));
    uint64_t value = 0;
    for (const auto byte : bytes) {
      value = (value << 8) | byte;
    }
    return value == 0 ? 1 : value;
  }

  void
  ensureFutureSampleAnnouncementsLocked(uint64_t throughSampleId)
  {
    if (!m_livePublisher) {
      throw std::logic_error("Core LiveStream publisher is missing");
    }
    const auto period = std::max<uint64_t>(1, m_streamDescriptor.samplePeriodMs);
    const auto lead = std::max<uint64_t>(
      4, static_cast<uint64_t>(std::ceil(120.0 / static_cast<double>(period))) + 2);
    const auto required = throughSampleId + lead;
    while (m_nextAnnouncedSampleId <= required) {
      const auto sampleId = m_nextAnnouncedSampleId++;
      if (!m_videoSampleClassSchedule) {
        throw std::logic_error("UAV video sample-class schedule is missing");
      }
      const auto sampleClass = m_videoSampleClassSchedule->classFor(sampleId);
      auto reservation = m_livePublisher->announceSample(
        sampleId, sampleClass,
        [this, sampleId] (size_t index,
                          ndn_service_framework::LiveStreamItemKind kind) {
          ndn::Name name = m_streamDescriptor.dataPrefix;
          name.append("fec-group").appendSequenceNumber(sampleId)
              .append(kind == ndn_service_framework::LiveStreamItemKind::Source ?
                        "data" : "parity")
              .appendSegment(index);
          return name;
        });
      m_announcedVideoSamples.emplace(sampleId, std::move(reservation));
    }
  }

  void
  initializeProtectedSessionLocked()
  {
    if (m_legacyRecordingDetected) {
      throw std::runtime_error(
        "legacy recording-only Repo is unsupported; export it with the pre-Spec-120 "
        "application or delete the old Repo database");
    }
    m_readiness.reset();
    m_mediaSequenceByCursor.clear();
    m_coreStreamDescriptor.reset();
    m_streamId = "stream-" + lowerHex(randomBytes(16));
    ++m_streamSessionEpoch;
    if (m_streamSessionEpoch == 0) {
      ++m_streamSessionEpoch;
    }
    m_videoSampleClassSchedule = m_useGStreamerPipeline ?
      UavVideoSampleClassSchedule::exactKeyDelta(
        static_cast<uint32_t>(m_targetFps.load()),
        static_cast<size_t>(m_fecDataShards), m_streamSessionEpoch) :
      UavVideoSampleClassSchedule::boundedOpaque(
        static_cast<uint32_t>(m_targetFps.load()),
        static_cast<size_t>(m_fecDataShards), m_streamSessionEpoch);

    m_streamDescriptor = VideoStreamDescriptor{};
    m_streamDescriptor.streamId = m_streamId;
    m_streamDescriptor.sessionEpoch = m_streamSessionEpoch;
    m_streamDescriptor.providerIdentity = droneIdentity(m_config, m_droneId);
    m_streamDescriptor.serviceName = droneVideoControlService(m_config, m_droneId);
    m_streamDescriptor.mappingVersion = randomNonZeroUint64();
    m_streamDescriptor.dataPrefix = m_streamDescriptor.providerIdentity;
    m_streamDescriptor.dataPrefix.append("video").append("front").append(m_streamId);
    m_streamDescriptor.mappingRoot = ndn_service_framework::makeStreamNameMapRoot(
      m_streamDescriptor.providerIdentity, m_streamId);
    // Thirty-two semantic names remain safely below one signed NDN Data packet
    // for this namespace while halving Mapping fetch/validation work versus
    // the former 16-entry block at the original 12+1 video load.
    m_streamDescriptor.mappingBlockCapacity = 32;
    m_streamDescriptor.maxNameReservations = UAV_VIDEO_MAX_NAME_RESERVATIONS;
    m_streamDescriptor.sampleUnit = "fec-group";
    m_streamDescriptor.samplePeriodMs = std::max<uint64_t>(1, 1000 / m_targetFps.load());
    m_streamDescriptor.prefetchEligibility = "ahead-mapped";
    m_streamDescriptor.cipher = "aes-256-gcm";
    m_streamDescriptor.keyEpoch = 1;
    m_streamDescriptor.streamKey = randomBytes(32);
    m_streamDescriptor.nonceSalt = randomBytes(4);
    // Decoder/FEC parameters are part of the durable stream contract.  They
    // must not exist only as convenience fields in the live start response,
    // otherwise canonical replay cannot interpret the same encrypted Data.
    m_streamDescriptor.extensions = {
      {"encoding", "video/h264"},
      {"stream_format", "semantic-name-map+aead-video-packet-v1"},
      {"fec_data_shards", std::to_string(m_fecDataShards)},
      {"fec_parity_shards", std::to_string(m_fecParityShards)},
      {"frame_width", std::to_string(m_acceptedFrameWidth.load())},
      {"max_payload_bytes", std::to_string(MAX_VIDEO_PACKET_PAYLOAD)},
      {"streaming_model", "h264-low-latency-packet-stream"},
      {"prefetch_hint", "ahead-mapped"},
      {"sample_class_mode",
       m_videoSampleClassSchedule->hasExactFrameClass() ?
         "exact-key-delta" : "bounded-opaque"},
      {"sample_class_key_seed", std::to_string(m_fecDataShards)},
      {"sample_class_delta_seed", std::to_string(
        std::min<uint64_t>(3, m_fecDataShards))},
      {"sample_class_opaque_seed", std::to_string(m_fecDataShards)},
    };
    // A descriptor is encoded before the first reservation is materialized.
    // Use the valid empty-session checkpoint shape; Core status replaces it
    // immediately after the first announceSample() reservation.
    m_streamDescriptor.frontiers = {
      0, 0, 0, m_streamDescriptor.mappingBlockCapacity - 1,
      m_streamDescriptor.mappingBlockCapacity};
    m_streamPrefix = m_streamDescriptor.dataPrefix;
    m_recordingSessionId = "record-" + m_streamId;

    if (m_recordingRepo) {
      CanonicalVideoRecordingManifest manifest;
      manifest.recordingId = m_recordingSessionId;
      manifest.streamId = m_streamId;
      manifest.sessionEpoch = m_streamSessionEpoch;
      manifest.mappingVersion = m_streamDescriptor.mappingVersion;
      manifest.keyEpoch = m_streamDescriptor.keyEpoch;
      manifest.providerIdentity = m_streamDescriptor.providerIdentity;
      manifest.serviceName = m_streamDescriptor.serviceName;
      manifest.redactedStreamDescriptor = decodeFields(
        encodeVideoStreamDescriptor(m_streamDescriptor));
      manifest.redactedStreamDescriptor.erase("stream_key_hex");
      manifest.redactedStreamDescriptor.erase("nonce_salt_hex");
      manifest.startedMs = nowMilliseconds();
      manifest.signerCertificateName = m_signingInfo.getSignerName().toUri();
      manifest.trustPolicyVersion = "unavailable";
      {
        std::ifstream trustInput(m_config.trustSchema, std::ios::binary);
        if (trustInput) {
          const std::vector<uint8_t> trustBytes(
            (std::istreambuf_iterator<char>(trustInput)),
            std::istreambuf_iterator<char>());
          const auto trustDigest = ndn_service_framework::computeStreamContentDigest(
            ndn::span<const uint8_t>(trustBytes.data(), trustBytes.size()));
          manifest.trustPolicyVersion = "sha256:" + hexEncode(
            std::vector<uint8_t>(trustDigest.begin(), trustDigest.end()));
        }
      }
      manifest.packetCatalogPrefix = ndn::Name(m_cameraOptions.recordObjectPrefix)
        .append(m_recordingSessionId).append("CATALOG");
      try {
        const auto signingCertificateName = m_signingInfo.getSignerName();
        const auto signingKeyName =
          ndn::security::extractKeyNameFromCertName(signingCertificateName);
        const auto certificate = m_keyChain.getPib()
          .getIdentity(m_streamDescriptor.providerIdentity)
          .getKey(signingKeyName).getCertificate(signingCertificateName);
        manifest.signerCertificateName = certificate.getName().toUri();
        const auto certificateWire = certificate.wireEncode();
        manifest.signerCertificateDigest =
          ndn_service_framework::computeStreamContentDigest(
            ndn::span<const uint8_t>(certificateWire.begin(), certificateWire.size()));
        manifest.archivedCertificateObjects.push_back(certificate.getName());
        m_recordingRepo->put(
          certificate.getName().toUri(),
          std::vector<uint8_t>(certificateWire.begin(), certificateWire.end()),
          "application/ndn-cert", 1,
          "recording_id=" + m_recordingSessionId,
          {m_streamDescriptor.providerIdentity.toUri()});

        // Durable replay must survive a Provider process restart without ever
        // writing a plaintext epoch key. Wrap the small key record to the
        // Provider's persistent RSA encryption certificate and bind every
        // field again inside the signed authorization object.
        const auto encryptionCertificate =
          ndn_service_framework::getRsaEncryptionCertificateOrThrow(
            m_keyChain, certificate);
        if (encryptionCertificate.getName() != certificate.getName()) {
          const auto encryptionCertificateWire = encryptionCertificate.wireEncode();
          m_recordingRepo->put(
            encryptionCertificate.getName().toUri(),
            std::vector<uint8_t>(encryptionCertificateWire.begin(),
                                 encryptionCertificateWire.end()),
            "application/ndn-cert", 1,
            "recording_id=" + m_recordingSessionId + ";role=key-encryption",
            {m_streamDescriptor.providerIdentity.toUri()});
          manifest.archivedCertificateObjects.push_back(
            encryptionCertificate.getName());
        }

        ndn::Buffer protectedPlaintext;
        protectedPlaintext.reserve(m_streamDescriptor.streamKey.size() +
                                   m_streamDescriptor.nonceSalt.size());
        protectedPlaintext.insert(protectedPlaintext.end(),
                                  m_streamDescriptor.streamKey.begin(),
                                  m_streamDescriptor.streamKey.end());
        protectedPlaintext.insert(protectedPlaintext.end(),
                                  m_streamDescriptor.nonceSalt.begin(),
                                  m_streamDescriptor.nonceSalt.end());
        ndn::security::transform::PublicKey publicKey;
        publicKey.loadPkcs8(encryptionCertificate.getPublicKey());
        auto wrapped = publicKey.encrypt(ndn::span<const uint8_t>(
          protectedPlaintext.data(), protectedPlaintext.size()));
        if (!wrapped) {
          throw std::runtime_error("RSA epoch-key wrap returned no ciphertext");
        }
        const Fields archiveFields{
          {"type", "canonical-video-epoch-key-archive-v1"},
          {"provider_identity", m_streamDescriptor.providerIdentity.toUri()},
          {"service_name", m_streamDescriptor.serviceName.toUri()},
          {"stream_id", m_streamDescriptor.streamId},
          {"session_epoch", std::to_string(m_streamDescriptor.sessionEpoch)},
          {"key_epoch", std::to_string(m_streamDescriptor.keyEpoch)},
          {"recipient_certificate", encryptionCertificate.getName().toUri()},
          {"algorithm", "RSA-OAEP"},
          {"wrapped_key_record_hex", hexEncode(std::vector<uint8_t>(
            wrapped->begin(), wrapped->end()))},
        };
        manifest.keyAuthorizationObject = ndn::Name(m_cameraOptions.recordObjectPrefix)
          .append(m_recordingSessionId).append("KEY-AUTH")
          .appendVersion(m_streamDescriptor.keyEpoch);
        ndn::Data keyAuthorizationData(manifest.keyAuthorizationObject);
        const auto archiveText = encodeFields(archiveFields);
        keyAuthorizationData.setContent(ndn::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(archiveText.data()), archiveText.size()));
        {
          std::lock_guard<std::mutex> signGuard(m_signMutex);
          m_keyChain.sign(keyAuthorizationData, m_signingInfo);
        }
        const auto keyAuthorizationWire = keyAuthorizationData.wireEncode();
        m_recordingRepo->put(
          manifest.keyAuthorizationObject.toUri(),
          std::vector<uint8_t>(keyAuthorizationWire.begin(), keyAuthorizationWire.end()),
          "application/ndn-data-key-authorization", 1,
          "recording_id=" + m_recordingSessionId,
          {m_streamDescriptor.providerIdentity.toUri()});
      }
      catch (const std::exception& e) {
        throw std::runtime_error(std::string("cannot archive Provider certificate: ") + e.what());
      }
      std::lock_guard<std::mutex> retentionGuard(m_retentionMutex);
      m_retentionManifest = std::move(manifest);
      m_retentionCatalogHead.fill(0);
      m_retentionHasSource = false;
    }

    ndn_service_framework::LiveStreamDefinition definition;
    definition.contractVersion =
      ndn_service_framework::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
    definition.streamId = m_streamDescriptor.streamId;
    definition.provider = m_streamDescriptor.providerIdentity;
    definition.semanticDataPrefix = m_streamDescriptor.dataPrefix;
    definition.sessionEpoch = m_streamDescriptor.sessionEpoch;
    definition.mappingVersion = m_streamDescriptor.mappingVersion;
    definition.mappingBlockCapacity = m_streamDescriptor.mappingBlockCapacity;
    definition.mappingAheadBlocks = 4;
    definition.retainedItems = computeLiveVideoRetentionItems(
      m_targetFps.load(), m_fecDataShards, m_fecParityShards);
    definition.maxNameReservations = m_streamDescriptor.maxNameReservations;
    definition.maxPendingInterests = 256;
    definition.samplePeriodMs = static_cast<double>(m_streamDescriptor.samplePeriodMs);
    if (m_videoSampleClassSchedule->hasExactFrameClass()) {
      definition.sampleClasses = {
        ndn_service_framework::SampleClassProfile::bounded(
          "key", static_cast<size_t>(m_fecDataShards),
          static_cast<size_t>(m_fecDataShards), 32, 0),
        ndn_service_framework::SampleClassProfile::bounded(
          "delta", static_cast<size_t>(std::min<uint64_t>(3, m_fecDataShards)),
          static_cast<size_t>(m_fecDataShards), 32, 0),
      };
    }
    else {
      definition.sampleClasses = {
        ndn_service_framework::SampleClassProfile::bounded(
          "opaque", static_cast<size_t>(m_fecDataShards),
          static_cast<size_t>(m_fecDataShards), 32, 0),
      };
    }
    if (m_fecParityShards == 1) {
      // FEC protects the already-encrypted UAV item, so this bound includes
      // the VideoPacket and AES-GCM envelope overhead above the 3600-byte
      // plaintext media payload. It does not change packetization or rate.
      definition.fec = ndn_service_framework::LiveStreamFecOptions::xorOneRepair(
        static_cast<size_t>(m_fecDataShards), 7000, 500);
    }
    {
      ndn_service_framework::StreamConfig sc;
      sc.streamId = m_streamId;
      sc.dataPrefix = definition.semanticDataPrefix;
      sc.samplePeriodMs = static_cast<double>(m_streamDescriptor.samplePeriodMs);
      sc.sampleClasses = definition.sampleClasses;
      sc.fec = definition.fec;
      sc.sessionEpoch = m_streamSessionEpoch;
      sc.advanced.mappingBlockCapacity = definition.mappingBlockCapacity;
      sc.advanced.mappingAheadBlocks = definition.mappingAheadBlocks;
      sc.advanced.retainedItems = definition.retainedItems;
      sc.advanced.maxNameReservations = definition.maxNameReservations;
      sc.advanced.maxPendingInterests = definition.maxPendingInterests;
      m_streamPublisher = m_serviceProvider.createStream(sc);
    }
    m_coreStreamDescriptor = m_streamPublisher->start();
    applyCorePredictiveStreamDescriptor(
      m_streamDescriptor, *m_coreStreamDescriptor);
    NDN_LOG_INFO("STREAM_API_ACTIVE role=provider mode=predictive"
                 << " stream=" << m_streamId
                 << " epoch=" << m_streamSessionEpoch);
    m_retentionStorageCircuitOpen = false;
    m_nonceGuard = std::make_unique<UavVideoNonceUseGuard>(m_streamDescriptor);
  }

  void
  clearProtectedSessionLocked()
  {
    m_streaming = false;
    std::fill(m_streamDescriptor.streamKey.begin(), m_streamDescriptor.streamKey.end(), 0);
    std::fill(m_streamDescriptor.nonceSalt.begin(), m_streamDescriptor.nonceSalt.end(), 0);
    if (m_nonceGuard) {
      m_nonceGuard->closeForUncertainUse();
    }
    m_nonceGuard.reset();
    if (m_streamPublisher) {
      m_streamPublisher->stop();
    }
    if (m_recordingPacketFeed) m_recordingPacketFeed->close();
    m_recordingPacketFeed.reset();
    m_streamPublisher.reset();
    m_coreStreamDescriptor.reset();
    m_videoSampleClassSchedule.reset();
    m_readiness.reset();
    m_streamDescriptor = VideoStreamDescriptor{};
  }

  static uint64_t
  fieldAsUint64(const Fields& fields, const std::string& key, uint64_t fallback)
  {
    try {
      return std::stoull(fieldOr(fields, key, std::to_string(fallback)));
    }
    catch (const std::exception&) {
      return fallback;
    }
  }

  std::string
  startFingerprintLocked() const
  {
    return std::to_string(m_targetFps.load()) + "|" +
      std::to_string(m_requestedBitrateKbps.load()) + "|" +
      std::to_string(m_requestedFrameWidth.load()) + "|" +
      std::to_string(m_fecParityShards);
  }

  void
  refreshDescriptorSnapshotLocked()
  {
    if (!m_readiness.ready() || !m_streamPublisher ||
        !m_coreStreamDescriptor) {
      throw std::logic_error("live video descriptor requested before readiness");
    }
    const auto status = m_streamPublisher->status();
    if (status.state !=
        ndn_service_framework::LiveStreamLifecycleState::Active) {
      throw std::logic_error(
        "predictive stream is not active: " + status.reason);
    }
    m_coreStreamDescriptor->checkpoint.initialSampleId =
      m_readiness.latestJoinCursor();
    m_coreStreamDescriptor->checkpoint.oldestRetainedSampleId =
      status.frontiers.oldestRetained;
    m_coreStreamDescriptor->checkpoint.latestProducedSampleId =
      status.frontiers.latestProduced;
    m_coreStreamDescriptor->checkpoint.nextExpectedSampleId =
      std::max(status.frontiers.nextReserved,
               status.frontiers.latestProduced + 1);
    m_coreStreamDescriptor->measuredSamplePeriodMs =
      static_cast<double>(m_readiness.samplePeriodMs());
    applyCorePredictiveStreamDescriptor(
      m_streamDescriptor, *m_coreStreamDescriptor);
    const auto joinMedia = m_mediaSequenceByCursor.find(
      m_coreStreamDescriptor->checkpoint.initialSampleId);
    if (joinMedia == m_mediaSequenceByCursor.end()) {
      throw std::logic_error("live video join cursor lacks media sequence binding");
    }
    m_streamDescriptor.extensions["latest_join_media_sequence"] =
      std::to_string(joinMedia->second);
    if (m_recordingRepo) {
      auto redacted = decodeFields(encodeVideoStreamDescriptor(m_streamDescriptor));
      redacted.erase("stream_key_hex");
      redacted.erase("nonce_salt_hex");
      std::lock_guard<std::mutex> retentionGuard(m_retentionMutex);
      m_retentionManifest.redactedStreamDescriptor = std::move(redacted);
    }
    // Canonical encode is also the final invariant check before key disclosure.
    (void)encodeVideoStreamDescriptor(m_streamDescriptor);
  }

  Fields
  makeStartFailureFieldsLocked(const std::string& reason) const
  {
    return {
      {"status", "failed"},
      {"drone_id", m_droneId},
      {"reason", reason},
      {"camera_available", cameraAvailable() ? "true" : "false"},
      {"camera_source", cameraSource()},
      {"camera_reason", cameraReason()},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  Fields
  makeStartResponseFieldsLocked() const
  {
    auto fields = decodeFields(encodeVideoStreamDescriptor(m_streamDescriptor));
    fields.insert({
      {"status", "streaming"},
      {"drone_id", m_droneId},
      {"capture", isCapturing() ? "on" : "off"},
      {"recording", isRecording() ? "on" : "off"},
      {"recording_session_id", m_recordingSessionId},
      {"recording_object_prefix", m_cameraOptions.recordObjectPrefix},
      {"recording_chunks", std::to_string(m_recordingChunks.load())},
      {"recording_bytes", std::to_string(m_recordingBytes.load())},
      {"fps", std::to_string(m_targetFps)},
      {"requested_bitrate_kbps", std::to_string(m_requestedBitrateKbps)},
      {"accepted_bitrate_kbps", std::to_string(m_acceptedBitrateKbps)},
      {"min_bitrate_kbps", std::to_string(MIN_VIDEO_BITRATE_KBPS)},
      {"max_bitrate_kbps", std::to_string(MAX_VIDEO_BITRATE_KBPS)},
      {"encoder_quality", std::to_string(m_encoderQuality)},
      {"requested_frame_width", std::to_string(m_requestedFrameWidth)},
      {"accepted_frame_width", std::to_string(m_acceptedFrameWidth)},
      {"encoding", "video/h264"},
      {"stream_format", "semantic-name-map+aead-video-packet-v1"},
      {"fec_data_shards", std::to_string(m_fecDataShards)},
      {"fec_parity_shards", std::to_string(m_fecParityShards)},
      {"frame_width", std::to_string(m_acceptedFrameWidth)},
      {"max_payload_bytes", std::to_string(MAX_VIDEO_PACKET_PAYLOAD)},
      {"streaming_model", "h264-low-latency-packet-stream"},
      {"prefetch_hint", "ahead-mapped"},
      {"source", m_videoPath},
      {"camera_available", cameraAvailable() ? "true" : "false"},
      {"camera_source", cameraSource()},
      {"camera_reason", cameraReason()},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    });
    return fields;
  }

  Fields
  start(const Fields& requestFields)
  {
    std::unique_lock<std::mutex> guard(m_mutex);
    const auto requestedFps = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "fps", "30")), 1, 60);
    const auto requestedBitrate = std::max<uint64_t>(
      1, std::stoull(fieldOr(requestFields, "requested_bitrate_kbps",
                             fieldOr(requestFields, "target_bitrate_kbps", "8000"))));
    const auto acceptedBitrate = std::clamp<uint64_t>(
      requestedBitrate, MIN_VIDEO_BITRATE_KBPS, MAX_VIDEO_BITRATE_KBPS);
    auto requestedWidth = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "requested_frame_width", "480")),
      MIN_VIDEO_FRAME_WIDTH, MAX_VIDEO_FRAME_WIDTH);
    if (requestedWidth % 2 != 0) {
      --requestedWidth;
    }
    const auto requestedParity = parseVideoFecParityShards(requestFields);
    auto refreshForLateViewer = [this, &guard, &requestFields]() -> Fields {
      m_readiness.reset();
      m_restartEncoder = true;
      NDN_LOG_INFO("CAMERA_VIEWER_SAFE_JOIN_REQUESTED stream_id=" << m_streamId
                   << " join=next-encoder-idr");
      const auto timeoutMs = std::clamp<uint64_t>(
        fieldAsUint64(requestFields, "readiness_timeout_ms", 4000), 500, 10000);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs);
      const auto ready = m_readinessCv.wait_until(
        guard, deadline,
        [this] { return !m_streaming.load() || m_readiness.ready(); });
      if (!ready || !m_streaming.load() || !m_readiness.ready()) {
        return makeStartFailureFieldsLocked("viewer-safe-join-timeout");
      }
      refreshDescriptorSnapshotLocked();
      NDN_LOG_INFO("CAMERA_VIEWER_SAFE_JOIN_READY stream_id=" << m_streamId
                   << " safe_join_cursor=" << m_readiness.latestJoinCursor());
      return makeStartResponseFieldsLocked();
    };
    if (m_streaming.load() && m_recordingRepo) {
      if (requestedFps == m_targetFps.load() &&
          requestedBitrate == m_requestedBitrateKbps.load() &&
          requestedWidth == m_requestedFrameWidth.load() &&
          requestedParity == m_fecParityShards && m_readiness.ready()) {
        // Keep the canonical session, but do not return an old delta-frame
        // frontier. Wait until the restarted encoder publishes a Mapping-
        // covered SPS/PPS/IDR boundary and return that cursor.
        return refreshForLateViewer();
      }
      return makeStartFailureFieldsLocked(
        "retention-session-active-reconfiguration-rejected");
    }
    m_targetFps = requestedFps;
    m_requestedBitrateKbps = requestedBitrate;
    m_acceptedBitrateKbps = acceptedBitrate;
    m_requestedFrameWidth = requestedWidth;
    m_acceptedFrameWidth = requestedWidth;
    m_encoderQuality = qualityForBitrate(m_acceptedBitrateKbps);
    m_fecDataShards = defaultFecDataShardsForBitrate(m_acceptedBitrateKbps.load());
    m_fecParityShards = requestedParity;
    const auto fingerprint = startFingerprintLocked();
    if (m_streaming.load() && m_readiness.ready() &&
        fingerprint == m_activeStartFingerprint) {
      return refreshForLateViewer();
    }
    clearProtectedSessionLocked();
    m_restartEncoder = true;
    m_nextSeq = 0;
    m_nextFecFrameSeq = 0;
    m_nextMediaSequence = 0;
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_jpegBuffer.clear();
    m_readiness.reset();
    m_activeStartFingerprint = fingerprint;
    try {
      initializeProtectedSessionLocked();
    }
    catch (const std::exception& e) {
      NDN_LOG_ERROR("VIDEO_SESSION_INITIALIZATION_FAILED reason=" << e.what());
      clearProtectedSessionLocked();
      return makeStartFailureFieldsLocked(std::string("session-initialization-failed:") + e.what());
    }
    m_streaming = true;
    m_captureEnabled = true;

    const auto readinessTimeoutMs = std::clamp<uint64_t>(
      fieldAsUint64(requestFields, "readiness_timeout_ms", 4000), 500, 10000);
    const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(readinessTimeoutMs);
    const auto ready = m_readinessCv.wait_until(
      guard, deadline,
      [this] {
        return !m_streaming.load() || m_readiness.ready() ||
               (m_streamPublisher &&
                m_streamPublisher->status().state ==
                  ndn_service_framework::LiveStreamLifecycleState::Failed);
      });
    if (!ready || !m_streaming.load() || !m_readiness.ready() ||
        (m_streamPublisher &&
         m_streamPublisher->status().state ==
           ndn_service_framework::LiveStreamLifecycleState::Failed)) {
      const auto reason = m_streamPublisher &&
                          m_streamPublisher->status().state ==
                            ndn_service_framework::LiveStreamLifecycleState::Failed ?
        m_streamPublisher->status().reason : m_readiness.reason();
      clearProtectedSessionLocked();
      return makeStartFailureFieldsLocked("readiness-timeout:" + reason);
    }
    while (!m_coreStreamDescriptor && std::chrono::steady_clock::now() < deadline) {
      try {
        refreshDescriptorSnapshotLocked();
      }
      catch (const std::logic_error&) {
        m_readinessCv.wait_for(guard, 10ms);
      }
    }
    if (!m_coreStreamDescriptor) {
      clearProtectedSessionLocked();
      return makeStartFailureFieldsLocked("readiness-timeout:core-routes-not-ready");
    }
    return makeStartResponseFieldsLocked();
  }

  Fields
  stop()
  {
    return stopWithReason("stopped");
  }

  Fields
  stopWithReason(const std::string& reason)
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    const auto streamPacketsPublished = m_nextSeq.load();
    const auto fecGroupsPublished = m_nextFecFrameSeq.load();
    const auto stoppedStreamId = m_streamId;
    if (isRecording() && m_streaming.load() && !m_done.load()) {
      return {
        {"status", "viewer-detached-retention-active"},
        {"drone_id", m_droneId}, {"reason", reason},
        {"publication_state", "active"}, {"live_consumption_state", "detached"},
        {"retention_state", m_retentionFailures.load() == 0 ? "active" : "degraded"},
        {"stream_id", stoppedStreamId},
        {"recording_session_id", m_recordingSessionId},
        {"timestamp_ms", std::to_string(nowMilliseconds())},
      };
    }
    if (m_streamPublisher) {
      const auto coreStatus = m_streamPublisher->status();
      NDN_LOG_INFO("VIDEO_LIVE_STREAM_CORE_FINAL"
                   << " stream_id=" << stoppedStreamId
                   << " pending_interests=" << coreStatus.pendingInterests
                   << " provider_future_interests="
                   << coreStatus.providerFutureInterests
                   << " provider_future_hits=" << coreStatus.providerFutureHits
                   << " future_hit_ratio="
                   << (coreStatus.providerFutureInterests == 0 ? 0.0 :
                     static_cast<double>(coreStatus.providerFutureHits) /
                     static_cast<double>(coreStatus.providerFutureInterests)));
    }
    m_streaming = false;
    if (!m_cameraOptions.captureOnStart && !m_cameraOptions.recordToLocalRepo) {
      m_captureEnabled = false;
    }
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_nextFecFrameSeq = 0;
    m_nextMediaSequence = 0;
    m_mediaSequenceByCursor.clear();
    m_jpegBuffer.clear();
    m_restartEncoder = true;
    clearProtectedSessionLocked();
    m_activeStartFingerprint.clear();
    return {
      {"status", "stopped"},
      {"drone_id", m_droneId},
      {"reason", reason},
      {"capture", isCapturing() ? "on" : "off"},
      {"recording", isRecording() ? "on" : "off"},
      {"recording_session_id", m_recordingSessionId},
      {"recording_object_prefix", m_cameraOptions.recordObjectPrefix},
      {"recording_chunks", std::to_string(m_recordingChunks.load())},
      {"recording_bytes", std::to_string(m_recordingBytes.load())},
      {"stream_id", stoppedStreamId},
      {"stream_packets_published", std::to_string(streamPacketsPublished)},
      {"fec_groups_published", std::to_string(fecGroupsPublished)},
      {"frames_published", std::to_string(fecGroupsPublished)},
      {"camera_available", cameraAvailable() ? "true" : "false"},
      {"camera_source", cameraSource()},
      {"camera_reason", cameraReason()},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  bool
  isStreaming() const
  {
    return m_streaming.load();
  }

  bool
  isCapturing() const
  {
    return m_captureEnabled.load();
  }

  bool
  isRecording() const
  {
    return m_cameraOptions.recordToLocalRepo && m_recordingRepo != nullptr &&
      m_retentionActive.load();
  }

  bool
  isRecordingEnabled() const
  {
    return m_cameraOptions.recordToLocalRepo;
  }

  bool
  isRepoOpen() const
  {
    return m_recordingRepo != nullptr;
  }

  std::string
  recordingRepoPath() const
  {
    return m_cameraOptions.recordRepoPath;
  }

  std::string
  recordingObjectPrefix() const
  {
    return m_cameraOptions.recordObjectPrefix;
  }

  bool
  cameraAvailable() const
  {
    if (m_videoPath.empty() || access(m_videoPath.c_str(), F_OK) != 0) {
      return false;
    }
    return !isVideoDevice(m_videoPath) || m_cameraCaptureUsable.load();
  }

  std::string
  cameraSource() const
  {
    return m_videoPath.empty() ? std::string("none") : m_videoPath;
  }

  std::string
  cameraReason() const
  {
    if (m_videoPath.empty()) {
      return "no-source-configured";
    }
    if (access(m_videoPath.c_str(), F_OK) != 0) {
      return "source-unavailable";
    }
    if (isVideoDevice(m_videoPath) && !m_cameraCaptureUsable.load()) {
      return "capture-probe-failed";
    }
    if (!isCapturing()) {
      return "capture-off";
    }
    return "ok";
  }

  uint64_t
  streamPacketsPublished() const
  {
    return m_nextSeq.load();
  }

  uint64_t
  fecGroupsPublished() const
  {
    return m_nextFecFrameSeq.load();
  }

  uint64_t
  recordingChunks() const
  {
    return m_recordingChunks.load();
  }

  uint64_t
  recordingBytes() const
  {
    return m_recordingBytes.load();
  }

  uint64_t
  recordingLastUpdateMs() const
  {
    return m_recordingLastUpdateMs.load();
  }

  std::string
  recordingPrefix() const
  {
    return m_cameraOptions.recordObjectPrefix;
  }

  Fields
  recordingManifestFields() const
  {
    if (m_legacyRecordingDetected) {
      return {{"type", "unsupported-legacy-recording"},
              {"drone_id", m_droneId},
              {"recording_repo_path", recordingRepoPath()},
              {"action", "export-with-pre-spec120-or-delete-old-repo"}};
    }
    Fields fields;
    {
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      const auto* manifest = m_retentionManifest.complete ? &m_retentionManifest :
        (m_latestCompletedManifest ? &*m_latestCompletedManifest : &m_retentionManifest);
      if (!manifest->recordingId.empty() &&
          !manifest->signerCertificateName.empty()) {
        try {
          fields = manifest->toFields();
        }
        catch (const std::exception& e) {
          // An empty recording has no committed cursor range yet. Report its
          // lifecycle without advertising an invalid durable manifest.
          NDN_LOG_WARN("CAMERA_CANONICAL_MANIFEST_SNAPSHOT_PENDING recording_id="
                       << manifest->recordingId << " reason=" << e.what());
        }
      }
    }
    fields.insert({
      {"type", fields.empty() ? "canonical-video-recording-pending" :
                                "canonical-video-recording-manifest"},
      {"drone_id", m_droneId},
      {"capture", isCapturing() ? "on" : "off"},
      {"recording", isRecording() ? "on" : "off"},
      {"recording_session_id", m_recordingSessionId},
      {"recording_object_prefix", m_cameraOptions.recordObjectPrefix},
      {"recording_object_type", "application/ndn-data"},
      {"recording_packets", std::to_string(recordingChunks())},
      {"recording_bytes", std::to_string(recordingBytes())},
      {"retention_queue_packets", std::to_string(m_retentionQueuePackets.load())},
      {"retention_queue_bytes", std::to_string(m_retentionQueueBytes.load())},
      {"retention_gap_packets", std::to_string(m_retentionGapPackets.load())},
      {"retention_failures", std::to_string(m_retentionFailures.load())},
      {"retention_storage_circuit",
       m_retentionStorageCircuitOpen.load() ? "open" : "closed"},
      {"retention_checkpoint_cursor", std::to_string(m_retentionCheckpointCursor.load())},
      {"publication_state", m_streaming.load() ? "active" : "stopped"},
      {"live_consumption_state", "externally-observed"},
      {"retention_state", !m_recordingRepo ? "disabled" :
        (!m_retentionActive.load() ? "stopped" :
         (m_retentionFailures.load() == 0 ? "active" : "degraded"))},
      {"media_encryption", "canonical-live-stream-epoch-key"},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    });
    return fields;
  }

  Fields
  recordingManifestFieldsFor(const ndn::Name& requesterIdentity) const
  {
    auto fields = recordingManifestFields();
    if (requesterIdentity.empty()) return fields;
    VideoStreamDescriptor descriptor;
    bool useCurrent = false;
    {
      std::lock_guard<std::mutex> retentionGuard(m_retentionMutex);
      if (m_retentionManifest.complete) {
        useCurrent = true;
      }
      else if (m_latestCompletedManifest && m_latestCompletedDescriptor) {
        descriptor = *m_latestCompletedDescriptor;
      }
      else {
        return fields;
      }
    }
    if (useCurrent) {
      std::lock_guard<std::mutex> stateGuard(m_mutex);
      descriptor = m_streamDescriptor;
    }
    if (descriptor.streamKey.size() != 32 || descriptor.nonceSalt.size() != 4)
      return fields;
    UavVideoContentKeyGrant grant;
    grant.recipientIdentity = requesterIdentity.toUri();
    grant.providerIdentity = descriptor.providerIdentity;
    grant.serviceName = descriptor.serviceName;
    grant.permission = ndn::Name("/PERMISSION")
      .append(descriptor.serviceName).append("history").toUri();
    grant.streamId = descriptor.streamId;
    grant.sessionEpoch = descriptor.sessionEpoch;
    grant.keyEpoch = descriptor.keyEpoch;
    grant.protectedKeyMaterial = descriptor.streamKey;
    grant.protectedNonceSalt = descriptor.nonceSalt;
    grant.issuedMs = nowMilliseconds();
    grant.expiresMs = grant.issuedMs + 3600 * 1000;
    const auto grantFields = grant.toProtectedFields();
    for (const auto& [key, value] : grantFields) fields[key] = value;
    fields["grant_transport"] = "protected-ndnsf-response-only";
    return fields;
  }

  Fields
  startRetention()
  {
    std::lock_guard<std::mutex> lifecycleGuard(m_retentionLifecycleMutex);
    if (!m_recordingRepo) return {{"status", "retention-disabled"}};
    if (m_retentionActive.load()) {
      return {{"status", "retention-already-active"},
              {"recording_session_id", m_recordingSessionId}};
    }

    std::lock_guard<std::mutex> stateGuard(m_mutex);
    if (m_retentionActive.load()) {
      return {{"status", "retention-already-active"},
              {"recording_session_id", m_recordingSessionId}};
    }
    if (!m_livePublisher || !m_streaming.load()) {
      return {{"status", "retention-start-rejected"},
              {"reason", "canonical-publication-not-active"}};
    }

    CanonicalVideoRecordingManifest evidence;
    {
      std::lock_guard<std::mutex> retentionGuard(m_retentionMutex);
      if (m_latestCompletedManifest) evidence = *m_latestCompletedManifest;
      else evidence = m_retentionManifest;
    }
    if (evidence.signerCertificateName.empty() ||
        evidence.keyAuthorizationObject.empty() ||
        evidence.archivedCertificateObjects.empty()) {
      return {{"status", "retention-start-rejected"},
              {"reason", "archived-trust-evidence-unavailable"}};
    }

    const auto fromCursor = m_nextSeq.load();
    const auto startedMs = nowMilliseconds();
    m_recordingSessionId = "record-" + m_streamId + "-" +
                           std::to_string(startedMs);
    CanonicalVideoRecordingManifest manifest;
    manifest.recordingId = m_recordingSessionId;
    manifest.streamId = m_streamId;
    manifest.sessionEpoch = m_streamSessionEpoch;
    manifest.mappingVersion = m_streamDescriptor.mappingVersion;
    manifest.keyEpoch = m_streamDescriptor.keyEpoch;
    manifest.providerIdentity = m_streamDescriptor.providerIdentity;
    manifest.serviceName = m_streamDescriptor.serviceName;
    manifest.redactedStreamDescriptor = decodeFields(
      encodeVideoStreamDescriptor(m_streamDescriptor));
    manifest.redactedStreamDescriptor.erase("stream_key_hex");
    manifest.redactedStreamDescriptor.erase("nonce_salt_hex");
    manifest.startedMs = startedMs;
    manifest.signerCertificateName = evidence.signerCertificateName;
    manifest.signerCertificateDigest = evidence.signerCertificateDigest;
    manifest.archivedCertificateObjects = evidence.archivedCertificateObjects;
    manifest.trustPolicyVersion = evidence.trustPolicyVersion;
    manifest.keyAuthorizationObject = evidence.keyAuthorizationObject;
    manifest.packetCatalogPrefix = ndn::Name(m_cameraOptions.recordObjectPrefix)
      .append(m_recordingSessionId).append("CATALOG");

    ndn_service_framework::PublishedPacketFeedOptions feedOptions;
    feedOptions.fromCursor = fromCursor;
    feedOptions.maxQueuedPackets = 512;
    feedOptions.maxQueuedBytes = 16 * 1024 * 1024;
    auto feed = m_livePublisher->openPublishedPacketFeed(feedOptions);
    {
      std::lock_guard<std::mutex> retentionGuard(m_retentionMutex);
      m_retentionManifest = std::move(manifest);
      m_retentionCatalogHead.fill(0);
      m_retainedMappingContentDigests.clear();
      m_retentionHasSource = false;
      m_lastObservedFeedDrops = 0;
    }
    m_recordingChunks = 0;
    m_recordingBytes = 0;
    m_retentionQueuePackets = 0;
    m_retentionQueueBytes = 0;
    m_retentionGapPackets = 0;
    m_retentionFailures = 0;
    m_retentionStorageCircuitOpen = false;
    m_retentionCheckpointCursor = fromCursor;
    m_recordingPacketFeed = std::move(feed);
    m_retentionActive = true;

    // The feed is attached before the restart fence. Therefore the first
    // retained source cursor is Mapping-covered and begins with fresh
    // SPS/PPS/IDR bytes, while the canonical publisher and live consumers
    // remain the same session.
    m_restartEncoder = true;
    NDN_LOG_INFO("CAMERA_CANONICAL_RETENTION_STARTED recording_id="
                 << m_recordingSessionId << " from_cursor=" << fromCursor
                 << " join=next-encoder-idr");
    return {{"status", "retention-started"},
            {"recording_session_id", m_recordingSessionId},
            {"retention_start_cursor", std::to_string(fromCursor)}};
  }

  Fields
  finalizeRetention()
  {
    std::lock_guard<std::mutex> lifecycleGuard(m_retentionLifecycleMutex);
    if (!m_recordingRepo) return {{"status", "retention-disabled"}};
    if (!m_retentionActive.exchange(false)) {
      return {{"status", "retention-already-stopped"},
              {"recording_session_id", m_recordingSessionId}};
    }
    std::shared_ptr<ndn_service_framework::PublishedPacketFeed> feed;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      feed = m_recordingPacketFeed;
    }
    if (feed) feed->close();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (feed &&
           (feed->status().queuedPackets != 0 || m_retentionWorkerBusy.load()) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(5ms);
    }
    if (feed &&
        (feed->status().queuedPackets != 0 || m_retentionWorkerBusy.load())) {
      recordRetentionGap(m_retentionCheckpointCursor.load(),
                         m_retentionCheckpointCursor.load(),
                         "retention-finalize-drain-timeout");
      return {{"status", "retention-finalize-drain-timeout"},
              {"recording_session_id", m_recordingSessionId},
              {"retention_checkpoint_cursor",
               std::to_string(m_retentionCheckpointCursor.load())}};
    }
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (m_recordingPacketFeed == feed) m_recordingPacketFeed.reset();
    }
    {
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      if (!m_retentionHasSource || m_retentionManifest.packetCatalogEntries == 0) {
        NDN_LOG_INFO("CAMERA_CANONICAL_RETENTION_FINALIZED_EMPTY recording_id="
                     << m_recordingSessionId);
        return {{"status", "retention-finalized-empty"},
                {"recording_session_id", m_recordingSessionId},
                {"retention_checkpoint_cursor",
                 std::to_string(m_retentionCheckpointCursor.load())}};
      }
    }
    const auto descriptor = freezeArchivedDescriptorSnapshot();
    persistCanonicalManifest(true);
    if (const auto* rotate = std::getenv("NDNSF_UAV_ARCHIVED_TRUST_REPLAY_TEST");
        rotate != nullptr && std::string(rotate) == "1") {
      const auto identityName = droneIdentity(m_config, m_droneId);
      const auto oldCertificate = m_signingInfo.getSignerName();
      auto identity = m_keyChain.getPib().getIdentity(identityName);
      auto rotatedKey = m_keyChain.createKey(identity, ndn::EcKeyParams());
      const auto rotatedCertificate = rotatedKey.getDefaultCertificate();
      {
        std::lock_guard<std::mutex> signGuard(m_signMutex);
        m_signingInfo = ndn::security::signingByCertificate(rotatedCertificate);
      }
      NDN_LOG_INFO("CAMERA_ARCHIVED_TRUST_CERTIFICATE_ROTATED old_certificate="
                   << oldCertificate << " current_certificate="
                   << rotatedCertificate.getName()
                   << " archived_packets=unchanged");
    }
    {
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      if (m_retentionManifest.complete) {
        m_latestCompletedManifest = m_retentionManifest;
        m_latestCompletedDescriptor = descriptor;
      }
      NDN_LOG_INFO("CAMERA_CANONICAL_RETENTION_FINALIZED recording_id="
                   << m_retentionManifest.recordingId
                   << " complete=" << (m_retentionManifest.complete ? "true" : "false")
                   << " packets=" << m_retentionManifest.packetCatalogEntries
                   << " gaps=" << m_retentionManifest.gaps.size());
    }
    return {{"status", "retention-finalized"},
            {"recording_session_id", m_recordingSessionId},
            {"retention_checkpoint_cursor",
             std::to_string(m_retentionCheckpointCursor.load())}};
  }

  Fields
  repoStatusFields() const
  {
    return {
      {"type", "camera-repo-status"},
      {"drone_id", m_droneId},
      {"repo_open", isRepoOpen() ? "true" : "false"},
      {"recording_enabled", isRecordingEnabled() ? "true" : "false"},
      {"recording_repo_path", recordingRepoPath()},
      {"recording_object_prefix", recordingObjectPrefix()},
      {"recording_session_id", m_recordingSessionId},
      {"recording_chunks", std::to_string(recordingChunks())},
      {"recording_bytes", std::to_string(recordingBytes())},
      {"recording_last_update_ms", std::to_string(recordingLastUpdateMs())},
      {"retention_packet_limit", std::to_string(m_cameraOptions.recordPacketLimit)},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  Fields
  recordingCatalogFields() const
  {
    std::vector<Fields> products;
    if (m_recordingRepo) {
      for (const auto& manifest : m_recordingRepo->list()) {
        products.push_back({
          {"object_name", manifest.objectName},
          {"object_type", manifest.objectType},
          {"size", std::to_string(manifest.size)},
          {"segments", std::to_string(manifest.segmentCount)},
          {"updated_ms", std::to_string(m_recordingLastUpdateMs.load())},
        });
      }
    }

    auto catalog = UavDataProductCatalogState::fromCatalogProductFields(
      products, droneIdentity(m_config, m_droneId).append("local-repo").toUri(),
      nowMilliseconds());
    if (!catalog.hasQueryableProducts() && recordingChunks() > 0) {
      const auto recording = RecordingDataProductState::fromFields(recordingManifestFields(), m_droneId);
      catalog = UavDataProductCatalogState::fromRecording(recording);
      catalog.repoObjects = products.size();
      catalog.sourceRepo = droneIdentity(m_config, m_droneId).append("local-repo").toUri();
      catalog.updatedMs = nowMilliseconds();
    }
    auto fields = catalog.toFields();
    fields["type"] = "uav-data-product-catalog";
    fields["drone_id"] = m_droneId;
    fields["repo_open"] = isRepoOpen() ? "true" : "false";
    fields["recording_repo_path"] = recordingRepoPath();
    fields["recording_session_id"] = m_recordingSessionId;
    fields["recording_object_prefix"] = m_cameraOptions.recordObjectPrefix;
    fields["timestamp_ms"] = std::to_string(nowMilliseconds());
    return fields;
  }

  std::vector<uint8_t>
  archivedPacketWire(const std::string& objectName) const
  {
    // Historical replay is expected after retention has been finalized.
    // Authorization comes from the manifest/key grant and integrity from the
    // committed catalog digest, not from the current recording lifecycle.
    if (objectName.empty() || !m_recordingRepo) {
      return {};
    }
    {
      // The archive producer is registered below the provider prefix so it
      // sees unrelated Interests too.  Reject names outside the committed
      // recording catalog before touching SQLite or producing warning noise.
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      if (m_committedPacketDigests.find(ndn::Name(objectName)) ==
          m_committedPacketDigests.end()) {
        return {};
      }
    }
    try {
      const auto wire = m_recordingRepo->get(objectName);
      const auto digest = ndn_service_framework::computeStreamContentDigest(
        ndn::span<const uint8_t>(wire.data(), wire.size()));
      {
        std::lock_guard<std::mutex> guard(m_retentionMutex);
        const auto expected = m_committedPacketDigests.find(ndn::Name(objectName));
        if (expected == m_committedPacketDigests.end() || expected->second != digest) {
          NDN_LOG_WARN("CAMERA_ARCHIVE_CATALOG_BINDING_REJECT name=" << objectName);
          return {};
        }
      }
      return wire;
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_ARCHIVE_DATA_GET_FAILED object=" << objectName
                   << " reason=" << e.what());
      return {};
    }
  }

  ndn::Name
  streamPrefix() const
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_streamPrefix;
  }

private:
  static std::string
  shellQuote(const std::string& value)
  {
    std::string output = "'";
    for (const auto ch : value) {
      if (ch == '\'') {
        output += "'\\''";
      }
      else {
        output.push_back(ch);
      }
    }
    output.push_back('\'');
    return output;
  }

  static uint64_t
  qualityForBitrate(uint64_t bitrateKbps)
  {
    if (bitrateKbps >= 8000) {
      return 20;
    }
    if (bitrateKbps >= 6000) {
      return 22;
    }
    if (bitrateKbps >= 4000) {
      return 24;
    }
    if (bitrateKbps >= 2500) {
      return 27;
    }
    if (bitrateKbps >= 1500) {
      return 30;
    }
    if (bitrateKbps >= 800) {
      return 33;
    }
    return 36;
  }

  static uint64_t
  defaultFecDataShardsForBitrate(uint64_t bitrateKbps)
  {
    if (bitrateKbps >= 8000) {
      return 12;
    }
    if (bitrateKbps >= 4000) {
      return 8;
    }
    if (bitrateKbps >= 2000) {
      return 6;
    }
    if (bitrateKbps >= 1200) {
      return 4;
    }
    return 3;
  }

  static std::string
  joinFecLengths(const std::vector<size_t>& lengths)
  {
    std::string out;
    for (size_t i = 0; i < lengths.size(); ++i) {
      if (i > 0) {
        out += ",";
      }
      out += std::to_string(lengths[i]);
    }
    return out;
  }

  void
  ensureRecordingFilterRegistered()
  {
    if (m_recordingFilterRegistered || m_cameraOptions.recordObjectPrefix.empty()) {
      return;
    }
    // One APP-owned fallback serves exact archived wires under their original
    // Provider namespace. LiveStream remains authoritative for active/future
    // packets; this path only answers objects already durable in Repo.
    const ndn::Name recordingPrefix = droneIdentity(m_config, m_droneId);
    m_face.setInterestFilter(
      recordingPrefix,
      [this] (const auto&, const ndn::Interest& interest) {
        this->onArchivedPacketInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name& prefix, const std::string& reason) {
        NDN_LOG_WARN("CAMERA_RECORDING_PREFIX_REGISTER_FAILED prefix="
                     << prefix << " reason=" << reason);
      });
    m_recordingFilterRegistered = true;
  }

  static ndn::Data
  decodeStoredData(const std::vector<uint8_t>& wire, const std::string& label)
  {
    auto [ok, block] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(wire.data(), wire.size()));
    if (!ok) throw std::invalid_argument(label + " is not a complete TLV block");
    try {
      return ndn::Data(block);
    }
    catch (const std::exception& e) {
      throw std::invalid_argument(label + " is not Data: " + e.what());
    }
  }

  static Fields
  decodeDataFields(const ndn::Data& data)
  {
    return decodeFields(std::string(
      reinterpret_cast<const char*>(data.getContent().value()),
      data.getContent().value_size()));
  }

  void
  recoverLatestCompletedRecording()
  {
    std::optional<CanonicalVideoRecordingManifest> selected;
    std::optional<ndn::Data> selectedData;
    for (const auto& object : m_recordingRepo->list()) {
      if (object.objectType != "application/ndn-data-manifest") continue;
      try {
        auto data = decodeStoredData(m_recordingRepo->get(object.objectName),
                                     "recording manifest");
        auto candidate = CanonicalVideoRecordingManifest::fromFields(
          decodeDataFields(data));
        if (!candidate.complete || !candidate.gaps.empty()) continue;
        if (!selected || candidate.endedMs > selected->endedMs ||
            (candidate.endedMs == selected->endedMs &&
             candidate.manifestVersion > selected->manifestVersion)) {
          selected = std::move(candidate);
          selectedData = std::move(data);
        }
      }
      catch (const std::exception&) {
        // A torn/old manifest is not a candidate; the selected object below
        // must independently pass all integrity checks.
      }
    }
    if (!selected || !selectedData) return;

    const auto certificateData = decodeStoredData(
      m_recordingRepo->get(selected->archivedCertificateObjects.front().toUri()),
      "archived signing certificate");
    ndn::security::Certificate signingCertificate(certificateData);
    const auto certificateWire = signingCertificate.wireEncode();
    const auto certificateDigest = ndn_service_framework::computeStreamContentDigest(
      ndn::span<const uint8_t>(certificateWire.begin(), certificateWire.size()));
    if (signingCertificate.getName().toUri() != selected->signerCertificateName)
      throw std::invalid_argument("archived manifest certificate name mismatch");
    if (certificateDigest != selected->signerCertificateDigest)
      throw std::invalid_argument("archived manifest certificate digest mismatch");
    if (signingCertificate.getIdentity() != selected->providerIdentity)
      throw std::invalid_argument("archived manifest certificate identity mismatch");
    if (!ndn::security::verifySignature(
          *selectedData,
          std::optional<ndn::security::Certificate>{signingCertificate}))
      throw std::invalid_argument("archived manifest signature mismatch");

    const auto keyData = decodeStoredData(
      m_recordingRepo->get(selected->keyAuthorizationObject.toUri()),
      "epoch-key authorization");
    if (keyData.getName() != selected->keyAuthorizationObject ||
        !ndn::security::verifySignature(
          keyData, std::optional<ndn::security::Certificate>{signingCertificate})) {
      throw std::invalid_argument("epoch-key authorization signature mismatch");
    }
    const auto archive = decodeDataFields(keyData);
    const auto recipientCertificateName = ndn::Name(
      fieldOr(archive, "recipient_certificate", ""));
    if (fieldOr(archive, "type", "") != "canonical-video-epoch-key-archive-v1" ||
        fieldOr(archive, "algorithm", "") != "RSA-OAEP" ||
        fieldOr(archive, "provider_identity", "") != selected->providerIdentity.toUri() ||
        fieldOr(archive, "service_name", "") != selected->serviceName.toUri() ||
        fieldOr(archive, "stream_id", "") != selected->streamId ||
        fieldOr(archive, "session_epoch", "") != std::to_string(selected->sessionEpoch) ||
        fieldOr(archive, "key_epoch", "") != std::to_string(selected->keyEpoch)) {
      throw std::invalid_argument("epoch-key authorization binding mismatch");
    }
    const auto wrapped = hexDecode(fieldOr(archive, "wrapped_key_record_hex", ""));
    auto plaintext = m_keyChain.getTpm().decrypt(
      ndn::span<const uint8_t>(wrapped.data(), wrapped.size()),
      ndn::security::extractKeyNameFromCertName(recipientCertificateName));
    if (!plaintext) throw std::runtime_error("Provider cannot unwrap archived epoch key");
    if (plaintext->size() != 36)
      throw std::invalid_argument("unwrapped epoch-key record size mismatch");
    auto descriptorFields = selected->redactedStreamDescriptor;
    descriptorFields["stream_key_hex"] = hexEncode(std::vector<uint8_t>(
      plaintext->begin(), plaintext->begin() + 32));
    descriptorFields["nonce_salt_hex"] = hexEncode(std::vector<uint8_t>(
      plaintext->begin() + 32, plaintext->end()));
    auto descriptor = decodeVideoStreamDescriptorStrict(
      encodeFields(descriptorFields), selected->providerIdentity,
      selected->providerIdentity, selected->serviceName, selected->serviceName);
    if (descriptor.streamKey.size() != 32 || descriptor.nonceSalt.size() != 4)
      throw std::invalid_argument("unwrapped epoch-key size mismatch");

    std::map<ndn::Name, ndn_service_framework::StreamContentDigest> recoveredDigests;
    ndn_service_framework::StreamContentDigest previousCatalogDigest{};
    for (uint64_t index = 0; index < selected->packetCatalogEntries; ++index) {
      const auto catalogName = ndn::Name(selected->packetCatalogPrefix)
        .appendSequenceNumber(index);
      const auto catalogData = decodeStoredData(
        m_recordingRepo->get(catalogName.toUri()), "packet catalog entry");
      if (catalogData.getName() != catalogName ||
          !ndn::security::verifySignature(
            catalogData,
            std::optional<ndn::security::Certificate>{signingCertificate}))
        throw std::invalid_argument("packet catalog signature/name mismatch");
      const auto catalogFields = decodeDataFields(catalogData);
      const auto previous = hexDecode(
        fieldOr(catalogFields, "previous_entry_digest", ""));
      const auto packetDigestBytes = hexDecode(
        fieldOr(catalogFields, "wire_digest", ""));
      if (fieldOr(catalogFields, "type", "") !=
            "canonical-video-packet-catalog-entry" ||
          fieldOr(catalogFields, "recording_id", "") != selected->recordingId ||
          fieldOr(catalogFields, "stream_id", "") != selected->streamId ||
          fieldOr(catalogFields, "session_epoch", "") !=
            std::to_string(selected->sessionEpoch) ||
          fieldOr(catalogFields, "mapping_version", "") !=
            std::to_string(selected->mappingVersion) ||
          fieldOr(catalogFields, "entry_index", "") != std::to_string(index) ||
          previous.size() != previousCatalogDigest.size() ||
          !std::equal(previous.begin(), previous.end(), previousCatalogDigest.begin()) ||
          packetDigestBytes.size() != previousCatalogDigest.size())
        throw std::invalid_argument("packet catalog chain/binding mismatch");
      const auto packetName = ndn::Name(fieldOr(catalogFields, "data_name", ""));
      const auto packetWire = m_recordingRepo->get(packetName.toUri());
      const auto packetData = decodeStoredData(packetWire, "canonical archived packet");
      const auto packetDigest = ndn_service_framework::computeStreamContentDigest(
        ndn::span<const uint8_t>(packetWire.data(), packetWire.size()));
      ndn_service_framework::StreamContentDigest expectedPacketDigest{};
      std::copy(packetDigestBytes.begin(), packetDigestBytes.end(),
                expectedPacketDigest.begin());
      if (packetData.getName() != packetName || packetDigest != expectedPacketDigest ||
          !recoveredDigests.emplace(packetName, packetDigest).second)
        throw std::invalid_argument("canonical archived packet digest/name mismatch");
      const auto catalogWire = catalogData.wireEncode();
      previousCatalogDigest = ndn_service_framework::computeStreamContentDigest(
        ndn::span<const uint8_t>(catalogWire.begin(), catalogWire.size()));
    }
    if (previousCatalogDigest != selected->packetCatalogHeadDigest)
      throw std::invalid_argument("packet catalog head digest mismatch");

    std::lock_guard<std::mutex> guard(m_retentionMutex);
    m_latestCompletedManifest = std::move(selected);
    m_latestCompletedDescriptor = std::move(descriptor);
    m_committedPacketDigests = std::move(recoveredDigests);
    NDN_LOG_INFO("CAMERA_RECORDING_RECOVERED recording_id="
                 << m_latestCompletedManifest->recordingId
                 << " packets=" << m_latestCompletedManifest->packetCatalogEntries);
  }

  void
  onArchivedPacketInterest(const ndn::Interest& interest)
  {
    const auto objectName = interest.getName().toUri();
    ndn_service_framework::ResponseMessage response;
    m_localRegistry.localInvokeRawInto(
      m_localArchivedPacketServiceName, makeRequest(objectName), response,
      droneIdentity(m_config, m_droneId));
    const auto responsePayload = response.getPayload();
    std::vector<uint8_t> payload(responsePayload.begin(), responsePayload.end());
    if (payload.empty()) {
      NDN_LOG_DEBUG("CAMERA_ARCHIVED_PACKET_MISS object=" << objectName);
      return;
    }

    try {
      ndn::Block wire(ndn::span<const uint8_t>(payload.data(), payload.size()));
      wire.parse();
      ndn::Data data(wire);
      if (data.getName() != interest.getName()) {
        NDN_LOG_WARN("CAMERA_ARCHIVE_NAME_MISMATCH requested=" << interest.getName()
                     << " stored=" << data.getName());
        return;
      }
      m_face.put(data); // exact archived wire; no re-signing or re-encryption
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_ARCHIVE_WIRE_REJECT name=" << interest.getName()
                   << " reason=" << e.what());
    }
  }

  void
  appendStreamChunk(std::vector<uint8_t> chunk, uint64_t nowMs)
  {
    if (!m_streaming.load()) {
      return;
    }
    bool publish = false;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_streaming.load()) {
        return;
      }
      if (m_fecPendingChunks.empty()) {
        m_fecCurrentFrameStartMs = nowMs;
      }
      m_fecPendingChunks.push_back(std::move(chunk));
      publish = m_fecPendingChunks.size() >= m_fecDataShards ||
        (m_fecCurrentFrameStartMs != 0 &&
         nowMs >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs);
    }
    if (publish) {
      publishCurrentFrame(nowMs);
    }
  }

  void
  ensureCanonicalRecordingSession()
  {
    if (m_done.load() || !isRecording() || m_streaming.load()) return;
    const auto now = nowMilliseconds();
    if (now < m_nextRecordingSessionRetryMs.load()) return;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_done.load() || m_streaming.load()) return;
    try {
      if (const auto* bitrate = std::getenv("NDNSF_UAV_RECORDING_BITRATE_KBPS")) {
        try {
          m_requestedBitrateKbps = std::stoull(bitrate);
        }
        catch (const std::exception&) {
          NDN_LOG_WARN("CAMERA_RECORDING_BITRATE_INVALID value=" << bitrate);
        }
      }
      if (const auto* width = std::getenv("NDNSF_UAV_RECORDING_FRAME_WIDTH")) {
        try {
          auto parsed = std::clamp<uint64_t>(
            std::stoull(width), MIN_VIDEO_FRAME_WIDTH, MAX_VIDEO_FRAME_WIDTH);
          if (parsed % 2 != 0) --parsed;
          m_requestedFrameWidth = parsed;
          m_acceptedFrameWidth = parsed;
        }
        catch (const std::exception&) {
          NDN_LOG_WARN("CAMERA_RECORDING_FRAME_WIDTH_INVALID value=" << width);
        }
      }
      m_acceptedBitrateKbps = std::clamp<uint64_t>(
        m_requestedBitrateKbps.load(), MIN_VIDEO_BITRATE_KBPS, MAX_VIDEO_BITRATE_KBPS);
      m_encoderQuality = qualityForBitrate(m_acceptedBitrateKbps.load());
      m_fecDataShards = defaultFecDataShardsForBitrate(m_acceptedBitrateKbps.load());
      m_activeStartFingerprint = startFingerprintLocked();
      initializeProtectedSessionLocked();
      m_streaming = true;
      NDN_LOG_INFO("CAMERA_RECORDING_SESSION_STARTED stream_id=" << m_streamId
                   << " session_epoch=" << m_streamSessionEpoch
                   << " publication=canonical-live-stream");
    }
    catch (const std::exception& e) {
      clearProtectedSessionLocked();
      m_nextRecordingSessionRetryMs = now + 500;
      NDN_LOG_WARN("CAMERA_RECORDING_SESSION_START_FAILED reason=" << e.what());
    }
  }

  void
  retentionLoop()
  {
    while (true) {
      std::shared_ptr<ndn_service_framework::PublishedPacketFeed> feed;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        feed = m_recordingPacketFeed;
      }
      if (!feed) {
        if (m_done.load()) break;
        std::this_thread::sleep_for(10ms);
        continue;
      }
      m_retentionWorkerBusy = true;
      const auto packets = feed->takeAvailable(64);
      if (packets.empty()) {
        const auto status = feed->status();
        m_retentionQueuePackets = status.queuedPackets;
        m_retentionQueueBytes = status.queuedBytes;
        m_retentionGapPackets = std::max<uint64_t>(m_retentionGapPackets.load(),
                                                   status.droppedPackets);
        observeFeedDrops(status);
        if (m_done.load()) break;
        if (status.closed) {
          std::lock_guard<std::mutex> guard(m_mutex);
          if (m_recordingPacketFeed == feed) m_recordingPacketFeed.reset();
        }
        m_retentionWorkerBusy = false;
        std::this_thread::sleep_for(5ms);
        continue;
      }
      bool retentionLimitReached = false;
      bool retentionStorageCircuitOpened = false;
      for (const auto& packet : packets) {
        if (m_cameraOptions.recordPacketLimit != 0 &&
            m_recordingChunks.load() >= m_cameraOptions.recordPacketLimit) {
          // A configured retention bound is a clean interval boundary, not a
          // storage failure. Stop observing future packets and commit exactly
          // the prefix already made durable.
          m_retentionActive = false;
          feed->close();
          retentionLimitReached = true;
          break;
        }
        const auto cursor = packet.cursor.value_or(0);
        ndn_service_framework::logStreamTimelineTrace(
          "provider", "retention-dequeued", packet.streamId,
          packet.sessionEpoch, cursor,
          {{"packet_kind", packet.kind ==
             ndn_service_framework::PublishedLiveStreamPacketKind::Mapping ?
             "mapping" : packet.kind ==
             ndn_service_framework::PublishedLiveStreamPacketKind::Repair ?
             "repair" : "source"}});
        try {
          if (const auto* failAfterText =
                std::getenv("NDNSF_UAV_SIMULATE_STORAGE_FAILURE_AFTER_PACKETS")) {
            try {
              const auto failAfter = std::stoull(failAfterText);
              if (m_recordingChunks.load() >= failAfter) {
                throw std::runtime_error("simulated canonical retention storage failure");
              }
            }
            catch (const std::invalid_argument&) {
              NDN_LOG_WARN("CAMERA_CANONICAL_STORAGE_FAILURE_INJECTION_INVALID value="
                           << failAfterText);
            }
          }
          const auto objectName = packet.dataName.toUri();
          m_recordingRepo->put(
            objectName, packet.signedDataWire, "application/ndn-data", 1,
            "stream_id=" + packet.streamId +
              ";session_epoch=" + std::to_string(packet.sessionEpoch) +
              ";mapping_version=" + std::to_string(packet.mappingVersion) +
              ";wire_sha256=" + hexEncode(std::vector<uint8_t>(
                packet.wireDigest.begin(), packet.wireDigest.end())),
            {packet.provider.toUri()});
          persistCanonicalCatalogEntry(packet);
          ++m_recordingChunks;
          m_recordingBytes += packet.signedDataWire.size();
          m_recordingLastUpdateMs = nowMilliseconds();
          if (packet.cursor) m_retentionCheckpointCursor = *packet.cursor;
          ndn_service_framework::logStreamTimelineTrace(
            "provider", "retention-durable", packet.streamId,
            packet.sessionEpoch, cursor,
            {{"wire_bytes", std::to_string(packet.signedDataWire.size())}});
        }
        catch (const std::exception& e) {
          ++m_retentionFailures;
          const auto firstFailed = packet.cursor.value_or(m_retentionCheckpointCursor.load());
          auto lastFailed = firstFailed;
          for (const auto& remaining : packets) {
            if (remaining.cursor && *remaining.cursor >= firstFailed) {
              lastFailed = std::max(lastFailed, *remaining.cursor);
            }
          }
          m_retentionGapPackets += lastFailed - firstFailed + 1;
          recordRetentionGap(firstFailed, lastFailed,
                             "storage-or-integrity-failure");
          m_retentionStorageCircuitOpen = true;
          retentionStorageCircuitOpened = true;
          feed->close();
          NDN_LOG_WARN("CAMERA_CANONICAL_RETENTION_FAILED first_cursor="
                       << firstFailed << " last_cursor=" << lastFailed
                       << " action=storage-circuit-open reason=" << e.what());
          break;
        }
      }
      std::optional<VideoStreamDescriptor> completedDescriptor;
      try {
        if (retentionLimitReached) {
          completedDescriptor = freezeArchivedDescriptorSnapshot();
        }
        persistCanonicalManifest(retentionLimitReached);
        if (retentionLimitReached && completedDescriptor) {
          std::lock_guard<std::mutex> guard(m_retentionMutex);
          if (m_retentionManifest.complete) {
            m_latestCompletedManifest = m_retentionManifest;
            m_latestCompletedDescriptor = *completedDescriptor;
          }
        }
      }
      catch (const std::exception& e) {
        ++m_retentionFailures;
        ++m_retentionGapPackets;
        NDN_LOG_WARN("CAMERA_CANONICAL_MANIFEST_COMMIT_FAILED reason=" << e.what());
      }
      const auto status = feed->status();
      m_retentionQueuePackets = status.queuedPackets;
      m_retentionQueueBytes = status.queuedBytes;
      m_retentionGapPackets = std::max<uint64_t>(m_retentionGapPackets.load(),
                                                 status.droppedPackets);
      observeFeedDrops(status);
      if (retentionLimitReached || retentionStorageCircuitOpened) {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_recordingPacketFeed == feed) m_recordingPacketFeed.reset();
      }
      m_retentionWorkerBusy = false;
    }
  }

  void
  observeFeedDrops(const ndn_service_framework::PublishedPacketFeedStatus& status)
  {
    if (status.droppedPackets <= m_lastObservedFeedDrops) return;
    if (status.firstDroppedCursor && status.lastDroppedCursor) {
      recordRetentionGap(*status.firstDroppedCursor, *status.lastDroppedCursor,
                         "bounded-feed-overflow");
    }
    m_lastObservedFeedDrops = status.droppedPackets;
  }

  void
  recordRetentionGap(uint64_t firstCursor, uint64_t lastCursor,
                     const std::string& reason)
  {
    std::lock_guard<std::mutex> guard(m_retentionMutex);
    if (!m_retentionManifest.gaps.empty() &&
        firstCursor <= m_retentionManifest.gaps.back().lastCursor + 1 &&
        m_retentionManifest.gaps.back().reason == reason) {
      m_retentionManifest.gaps.back().lastCursor =
        std::max(m_retentionManifest.gaps.back().lastCursor, lastCursor);
      return;
    }
    if (!m_retentionManifest.gaps.empty() &&
        firstCursor <= m_retentionManifest.gaps.back().lastCursor) {
      firstCursor = m_retentionManifest.gaps.back().lastCursor + 1;
      lastCursor = std::max(firstCursor, lastCursor);
    }
    m_retentionManifest.gaps.push_back({firstCursor, lastCursor, reason});
  }

  void
  persistCanonicalCatalogEntry(
    const ndn_service_framework::PublishedLiveStreamPacket& packet)
  {
    CanonicalVideoRecordingManifest snapshot;
    RetainedVideoPacketReference reference;
    reference.kind = packet.kind ==
      ndn_service_framework::PublishedLiveStreamPacketKind::Mapping ? "mapping" :
      packet.kind == ndn_service_framework::PublishedLiveStreamPacketKind::Repair ? "repair" :
      "source";
    reference.cursor = packet.cursor;
    reference.dataName = packet.dataName;
    reference.wireDigest = packet.wireDigest;

    std::optional<std::pair<uint64_t,
      ndn_service_framework::StreamContentDigest>> mappingCheckpoint;
    if (packet.kind == ndn_service_framework::PublishedLiveStreamPacketKind::Mapping) {
      const auto mappingData = decodeStoredData(
        std::vector<uint8_t>(packet.signedDataWire.begin(), packet.signedDataWire.end()),
        "retained Mapping Data");
      auto content = mappingData.getContent();
      content.parse();
      ndn_service_framework::StreamNameMapBlock block;
      if (content.elements().size() != 1 ||
          !block.wireDecode(content.elements().front()) ||
          block.streamId != packet.streamId ||
          block.sessionEpoch != packet.sessionEpoch ||
          block.mappingVersion != packet.mappingVersion ||
          !packet.cursor || block.firstCursor != *packet.cursor) {
        throw std::invalid_argument("retained Mapping checkpoint binding mismatch");
      }
      mappingCheckpoint = std::make_pair(block.blockNumber, block.contentDigest());
    }

    uint64_t catalogIndex = 0;
    ndn_service_framework::StreamContentDigest previousDigest{};
    {
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      if (packet.streamId != m_retentionManifest.streamId ||
          packet.sessionEpoch != m_retentionManifest.sessionEpoch) {
        throw std::invalid_argument("mixed-session packet rejected by retention adapter");
      }
      catalogIndex = m_retentionManifest.packetCatalogEntries;
      previousDigest = m_retentionCatalogHead;
    }

    Fields catalogFields{
      {"type", "canonical-video-packet-catalog-entry"},
      {"recording_id", m_recordingSessionId},
      {"stream_id", packet.streamId},
      {"session_epoch", std::to_string(packet.sessionEpoch)},
      {"mapping_version", std::to_string(packet.mappingVersion)},
      {"entry_index", std::to_string(catalogIndex)},
      {"packet_kind", reference.kind},
      {"cursor", packet.cursor ? std::to_string(*packet.cursor) : "mapping"},
      {"data_name", packet.dataName.toUri()},
      {"wire_digest", hexEncode(std::vector<uint8_t>(
        packet.wireDigest.begin(), packet.wireDigest.end()))},
      {"previous_entry_digest", hexEncode(std::vector<uint8_t>(
        previousDigest.begin(), previousDigest.end()))},
    };
    const auto catalogName = ndn::Name(m_cameraOptions.recordObjectPrefix)
      .append(m_recordingSessionId).append("CATALOG").appendSequenceNumber(catalogIndex);
    auto catalogData = ndn::Data(catalogName);
    const auto catalogText = encodeFields(catalogFields);
    catalogData.setContent(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(catalogText.data()), catalogText.size()));
    {
      std::lock_guard<std::mutex> signGuard(m_signMutex);
      m_keyChain.sign(catalogData, m_signingInfo);
    }
    const auto catalogWire = catalogData.wireEncode();
    const auto catalogDigest = ndn_service_framework::computeStreamContentDigest(
      ndn::span<const uint8_t>(catalogWire.begin(), catalogWire.size()));
    m_recordingRepo->put(catalogName.toUri(),
                         std::vector<uint8_t>(catalogWire.begin(), catalogWire.end()),
                         "application/ndn-data-catalog", 1,
                         "recording_id=" + m_recordingSessionId,
                         {packet.provider.toUri()});

    std::lock_guard<std::mutex> guard(m_retentionMutex);
    if (catalogIndex != m_retentionManifest.packetCatalogEntries ||
        previousDigest != m_retentionCatalogHead) {
      throw std::logic_error("retention catalog changed during durable write");
    }
    ++m_retentionManifest.packetCatalogEntries;
    m_retentionManifest.packetCatalogHeadDigest = catalogDigest;
    m_retentionCatalogHead = catalogDigest;
    m_committedPacketDigests[packet.dataName] = packet.wireDigest;
    if (mappingCheckpoint) {
      const auto [it, inserted] = m_retainedMappingContentDigests.emplace(
        mappingCheckpoint->first, mappingCheckpoint->second);
      if (!inserted && it->second != mappingCheckpoint->second) {
        throw std::logic_error("retained Mapping checkpoint equivocation");
      }
    }
    if (reference.cursor && reference.kind == "source") {
      if (!m_retentionHasSource) {
        m_retentionManifest.firstCommittedCursor = *reference.cursor;
        m_retentionManifest.safeJoinCursor = *reference.cursor;
        m_retentionHasSource = true;
      }
      m_retentionManifest.lastCommittedCursor = *reference.cursor;
    }
    // Keep only a small audit tail inline; the signed catalog is authoritative.
    m_retentionManifest.packets.push_back(std::move(reference));
    if (m_retentionManifest.packets.size() > 8)
      m_retentionManifest.packets.erase(m_retentionManifest.packets.begin());
  }

  void
  persistCanonicalManifest(bool complete)
  {
    CanonicalVideoRecordingManifest manifest;
    {
      std::lock_guard<std::mutex> guard(m_retentionMutex);
      if (m_retentionManifest.recordingId.empty() ||
          m_retentionManifest.packetCatalogEntries == 0) return;
      ++m_retentionManifest.manifestVersion;
      m_retentionManifest.complete = complete && m_retentionManifest.gaps.empty();
      if (complete) m_retentionManifest.endedMs = nowMilliseconds();
      manifest = m_retentionManifest;
    }
    const auto manifestFields = manifest.toFields();
    const auto manifestText = encodeFields(manifestFields);
    auto manifestName = ndn::Name(m_cameraOptions.recordObjectPrefix)
      .append(m_recordingSessionId).append("MANIFEST")
      .appendVersion(manifest.manifestVersion);
    ndn::Data manifestData(manifestName);
    manifestData.setFreshnessPeriod(2_s);
    manifestData.setContent(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(manifestText.data()), manifestText.size()));
    {
      std::lock_guard<std::mutex> signGuard(m_signMutex);
      m_keyChain.sign(manifestData, m_signingInfo);
    }
    const auto wire = manifestData.wireEncode();
    m_recordingRepo->put(manifestName.toUri(),
                         std::vector<uint8_t>(wire.begin(), wire.end()),
                         "application/ndn-data-manifest", 1,
                         "recording_id=" + manifest.recordingId,
                         {manifest.providerIdentity.toUri()});
  }

  VideoStreamDescriptor
  freezeArchivedDescriptorSnapshot()
  {
    VideoStreamDescriptor descriptor;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      descriptor = m_streamDescriptor;
    }
    std::lock_guard<std::mutex> guard(m_retentionMutex);
    if (!m_retentionHasSource || m_retainedMappingContentDigests.empty()) {
      throw std::logic_error("canonical retention has no replay checkpoint");
    }
    const auto firstCursor = m_retentionManifest.firstCommittedCursor;
    const auto lastCursor = m_retentionManifest.lastCommittedCursor;
    const auto anchorBlock = firstCursor / descriptor.mappingBlockCapacity;
    const auto anchor = m_retainedMappingContentDigests.find(anchorBlock);
    if (anchor == m_retainedMappingContentDigests.end()) {
      throw std::logic_error("canonical retention is missing its anchor Mapping");
    }
    const auto lastMappingBlock = m_retainedMappingContentDigests.rbegin()->first;
    const auto mappingThrough =
      (lastMappingBlock + 1) * descriptor.mappingBlockCapacity - 1;
    if (lastCursor > mappingThrough) {
      throw std::logic_error("canonical retention source exceeds Mapping frontier");
    }
    descriptor.frontiers.oldestRetained = firstCursor;
    descriptor.frontiers.latestJoin = firstCursor;
    descriptor.frontiers.latestProduced = lastCursor;
    descriptor.frontiers.mappingCommittedThrough = mappingThrough;
    descriptor.frontiers.nextReserved = mappingThrough + 1;
    descriptor.mappingAnchorBlock = anchorBlock;
    descriptor.mappingAnchorContentDigest = anchor->second;
    auto redacted = decodeFields(encodeVideoStreamDescriptor(descriptor));
    redacted.erase("stream_key_hex");
    redacted.erase("nonce_salt_hex");
    m_retentionManifest.safeJoinCursor = firstCursor;
    m_retentionManifest.redactedStreamDescriptor = std::move(redacted);
    return descriptor;
  }

  void
  publishCurrentFrame(uint64_t encodedOutputReadyMs,
                      const UavVideoFrame* exactFrame = nullptr)
  {
    std::unique_lock<std::mutex> stateGuard(m_mutex);
    const auto actualDataShardCount = m_fecPendingChunks.size();
    if (actualDataShardCount == 0 || !m_streaming.load() || !m_nonceGuard ||
        !m_streamPublisher || !m_coreStreamDescriptor) {
      return;
    }

    const auto frameSeq = m_nextFecFrameSeq++;
    const auto second = encodedOutputReadyMs / 1000;
    auto dataChunks = std::move(m_fecPendingChunks);
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    std::vector<uint8_t> readinessBytes;
    for (const auto& payload : dataChunks) {
      readinessBytes.insert(readinessBytes.end(), payload.begin(), payload.end());
    }
    const auto dataShardCount = dataChunks.size();
    if (!m_videoSampleClassSchedule) {
      throw std::logic_error("UAV video sample-class schedule is missing");
    }
    const auto sampleClass = m_videoSampleClassSchedule->classFor(frameSeq);
    if (exactFrame != nullptr &&
        !m_videoSampleClassSchedule->matchesActual(frameSeq,
                                                   exactFrame->keyFrame)) {
      throw std::logic_error(
        "encoded access-unit class does not match its future class announcement");
    }
    const bool keyFrame = exactFrame != nullptr && exactFrame->keyFrame;
    const auto firstPacketSeq = m_nextSeq.load();
    const auto frameLastPacketSeq =
      firstPacketSeq + static_cast<uint64_t>(dataShardCount) - 1;
    m_nextSeq = frameLastPacketSeq + 1;
    const auto firstMediaSequence = m_nextMediaSequence.fetch_add(dataShardCount);
    m_mediaSequenceByCursor[firstPacketSeq] = firstMediaSequence;
    auto makeStreamChunk = [&] (uint64_t symbolIndex, uint64_t cursor,
                                std::vector<uint8_t> payload,
                                bool keyChunk) {
      ndn_service_framework::StreamChunk chunk;
      chunk.streamId = m_streamId;
      chunk.sessionEpoch = m_streamSessionEpoch;
      chunk.seq = cursor;
      chunk.payload = std::move(payload);
      chunk.contentType = "video/h264";
      // StreamChunk keeps the wire-compatible field name. Its actual origin is
      // the first post-FFmpeg encoded byte, not camera acquisition.
      chunk.captureMs = encodedOutputReadyMs;
      chunk.keyChunk = keyChunk;
      chunk.frameId = frameSeq;
      chunk.frameFirstSeq = firstPacketSeq;
      chunk.frameLastSeq = frameLastPacketSeq;
      chunk.segmentIndex = symbolIndex;
      chunk.segmentCount = dataShardCount + m_fecParityShards;
      chunk.metadata["uav.second"] = std::to_string(second);
      chunk.metadata["uav.bucket_packet_count"] = std::to_string(frameLastPacketSeq + 1);
      chunk.metadata["uav.media_sequence"] =
        std::to_string(firstMediaSequence + symbolIndex);
      if (exactFrame != nullptr) {
        chunk.metadata["uav.frame_binding_version"] = "1";
        chunk.metadata["uav.source_frame_id"] = std::to_string(exactFrame->sourceFrameId);
        chunk.metadata["uav.capture_origin_ns"] = std::to_string(exactFrame->captureOriginNs);
        chunk.metadata["uav.capture_clock_id"] = "provider-steady-v1";
        chunk.metadata["uav.codec_pts"] = std::to_string(exactFrame->codecPts);
        chunk.metadata["uav.codec_time_base_num"] = "1";
        chunk.metadata["uav.codec_time_base_den"] = "1000000000";
        chunk.metadata["uav.codec_config_epoch"] =
          std::to_string(exactFrame->codecConfigEpoch);
      }

      // FEC is a Core transport concern over the protected bytes. The UAV
      // payload records only the source grouping needed by its decoder.
      ndn_service_framework::StreamFecInfo group;
      group.scheme = m_fecParityShards == 1 ? "core-xor-one-repair" : "none";
      group.dataShards = dataShardCount;
      group.parityShards = m_fecParityShards;
      group.symbolIndex = symbolIndex;
      group.symbolCount = dataShardCount + m_fecParityShards;
      group.sourceBlockId = std::to_string(frameSeq);
      group.repairSymbol = false;
      chunk.fec = std::move(group);
      return chunk;
    };

    auto protectStreamChunk = [&] (
      const ndn_service_framework::StreamChunk& chunk, uint64_t cursor) {
      const auto packet = streamChunkToVideoPacket(chunk);
      UavVideoDataName binding;
      binding.cursor = cursor;
      binding.parity = false;
      binding.name = ndn_service_framework::makePredictiveDataName(
        m_coreStreamDescriptor->definition, cursor);
      return protectUavVideoPacket(
        m_streamDescriptor, binding, packet, *m_nonceGuard);
    };

    try {
      if (exactFrame != nullptr) {
        ndn::Name frameTrace("/NDNSF/UAV/VIDEO/FRAME");
        frameTrace.append(m_streamId).appendNumber(m_streamSessionEpoch)
          .appendNumber(exactFrame->sourceFrameId);
        ndn_service_framework::logTimelineTrace(
          "provider", "source-acquired", frameTrace,
          {{"clock_domain", "host-steady"},
           {"frame_correlation", "exact"},
           {"source_id", std::to_string(exactFrame->sourceFrameId)},
           {"capture_origin_ns", std::to_string(exactFrame->captureOriginNs)},
           {"codec_pts", std::to_string(exactFrame->codecPts)}});
        ndn_service_framework::logTimelineTrace(
          "provider", "encoded-output-ready", frameTrace,
          {{"clock_domain", "host-steady"},
           {"frame_correlation", "exact"},
           {"source_id", std::to_string(exactFrame->sourceFrameId)},
           {"output_ordinal", "0"}});
      }
      std::vector<uint64_t> producedCursors;
      producedCursors.reserve(dataShardCount);
      for (size_t index = 0; index < dataShardCount; ++index) {
        producedCursors.push_back(
          firstPacketSeq + static_cast<uint64_t>(index));
      }
      for (const auto cursor : producedCursors) {
        if (exactFrame != nullptr) {
          ndn_service_framework::logStreamTimelineTrace(
            "provider", "source-acquired", m_streamId,
            m_streamSessionEpoch, cursor,
            {{"clock_domain", "provider-steady-v1"},
             {"source_frame_id", std::to_string(exactFrame->sourceFrameId)},
             {"capture_origin_ns", std::to_string(exactFrame->captureOriginNs)},
             {"codec_pts", std::to_string(exactFrame->codecPts)}});
        }
        ndn_service_framework::logStreamTimelineTrace(
          "provider", "encoded-output-ready", m_streamId,
          m_streamSessionEpoch, cursor,
          {{"origin", exactFrame == nullptr ? "post-ffmpeg-h264-output" :
                                              "gstreamer-access-unit"}});
        ndn_service_framework::logStreamTimelineTrace(
          "provider", "group-ready", m_streamId,
          m_streamSessionEpoch, cursor);
      }
      std::vector<std::vector<uint8_t>> protectedSources;
      protectedSources.reserve(dataShardCount);
      for (uint64_t i = 0; i < dataShardCount; ++i) {
        const auto cursor = firstPacketSeq + i;
        auto chunk = makeStreamChunk(i, cursor,
                                     std::move(dataChunks[i]), keyFrame);
        protectedSources.push_back(
          protectStreamChunk(chunk, cursor));
        ndn_service_framework::logStreamTimelineTrace(
          "provider", "protection-complete", m_streamId,
          m_streamSessionEpoch, cursor);
      }
      size_t pendingCoreGroup = 0;
      const auto coreGroupLimit =
        std::max<size_t>(1, static_cast<size_t>(m_fecDataShards));
      for (size_t i = 0; i < protectedSources.size(); ++i) {
        const auto cursor = firstPacketSeq + static_cast<uint64_t>(i);
        auto data = std::make_shared<ndn::Data>(
          ndn_service_framework::makePredictiveDataName(
            m_coreStreamDescriptor->definition, cursor));
        data->setContent(ndn::span<const uint8_t>(
          protectedSources[i].data(), protectedSources[i].size()));
        data->setFreshnessPeriod(ndn::time::milliseconds(1000));
        m_keyChain.sign(*data, m_signingInfo);
        m_streamPublisher->push(data);
        if (++pendingCoreGroup == coreGroupLimit) {
          m_streamPublisher->flush();
          pendingCoreGroup = 0;
        }
      }
      if (pendingCoreGroup != 0) {
        m_streamPublisher->flush();
      }
      m_readiness.observePublicationGroup(
        firstPacketSeq, frameLastPacketSeq,
        std::max<uint64_t>(1, nowMilliseconds()), readinessBytes);
      if (m_readiness.ready()) {
        m_streamDescriptor.samplePeriodMs = m_readiness.samplePeriodMs();
      }
      if (frameSeq % 30 == 0) {
        const auto coreStatus = m_streamPublisher->status();
        NDN_LOG_INFO("VIDEO_LIVE_STREAM_CORE_STATUS"
                     << " stream_id=" << m_streamId
                     << " frame_seq=" << frameSeq
                     << " retained_items=" << coreStatus.retainedItems
                     << " pending_interests=" << coreStatus.pendingInterests
                     << " provider_future_interests="
                     << coreStatus.providerFutureInterests
                     << " provider_future_hits=" << coreStatus.providerFutureHits
                     << " latest_produced_cursor="
                     << coreStatus.frontiers.latestProduced
                     << " mapping_committed_through_cursor="
                     << coreStatus.frontiers.mappingCommittedThrough);
        for (const auto& [classId, prediction] : coreStatus.sampleClassPredictions) {
          NDN_LOG_INFO("VIDEO_SAMPLE_CLASS_PREDICTION"
                       << " stream_id=" << m_streamId
                       << " class=" << classId
                       << " predicted_sources=" << prediction.prediction
                       << " observations=" << prediction.observations
                       << " underpredictions=" << prediction.underpredictions
                       << " underpredicted_items=" << prediction.underpredictedItems
                       << " overpredictions=" << prediction.overpredictions
                       << " overpredicted_items=" << prediction.overpredictedItems);
        }
      }
    }
    catch (const std::exception& e) {
      NDN_LOG_ERROR("VIDEO_PROTECTED_PUBLICATION_FAILED stream=" << m_streamId
                    << " frame=" << frameSeq << " reason=" << e.what());
      m_nonceGuard->closeForUncertainUse();
      m_streaming = false;
      m_readinessCv.notify_all();
      return;
    }
    stateGuard.unlock();
    m_readinessCv.notify_all();
  }

  void
  publishGStreamerAccessUnit(const UavVideoFrame& frame)
  {
    if (!m_streaming.load() || frame.bytes.empty()) {
      return;
    }
    ensureCanonicalRecordingSession();
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_fecPendingChunks.clear();
      m_fecCurrentFrameStartMs = nowMilliseconds();
      for (size_t offset = 0; offset < frame.bytes.size();
           offset += MAX_VIDEO_PACKET_PAYLOAD) {
        const auto size = std::min(MAX_VIDEO_PACKET_PAYLOAD,
                                   frame.bytes.size() - offset);
        m_fecPendingChunks.emplace_back(frame.bytes.begin() + offset,
                                        frame.bytes.begin() + offset + size);
      }
    }
    publishCurrentFrame(nowMilliseconds(), &frame);
  }

  void
  captureLoopGStreamer()
  {
    GStreamerVideoPipeline pipeline;
    bool running = false;
    while (!m_done.load()) {
      if (running && pipeline.state() == UavVideoPipelineState::Failed) {
        const auto failure = pipeline.failure();
        NDN_LOG_ERROR("UAV_VIDEO_PIPELINE backend=gstreamer state=failed"
                      << " direction="
                      << (failure ? failure->direction : "unknown")
                      << " code=" << (failure ? failure->code : "unknown")
                      << " reason=" << (failure ? failure->reason : "unknown"));
        pipeline.stop();
        running = false;
        m_streaming = false;
        m_captureEnabled = false;
        m_readinessCv.notify_all();
        continue;
      }
      if (!m_captureEnabled.load()) {
        if (running) {
          pipeline.stop();
          running = false;
        }
        std::this_thread::sleep_for(50ms);
        continue;
      }
      if (!running || m_restartEncoder.exchange(false)) {
        if (running) pipeline.stop();
        UavVideoCaptureConfig config;
        const auto* sourceOverride = std::getenv("NDNSF_UAV_GSTREAMER_SOURCE");
        config.source = sourceOverride == nullptr ? m_videoPath : sourceOverride;
        config.width = static_cast<uint32_t>(m_acceptedFrameWidth.load());
        config.height = std::max<uint32_t>(2, config.width * 3 / 4);
        config.fps = static_cast<uint32_t>(m_targetFps.load());
        config.bitrateKbps = static_cast<uint32_t>(m_acceptedBitrateKbps.load());
        config.keyFrameInterval = config.fps;
        try {
          pipeline.startCapture(config, [this] (const UavVideoFrame& frame) {
            publishGStreamerAccessUnit(frame);
          });
          running = true;
          NDN_LOG_INFO("UAV_VIDEO_PIPELINE backend=gstreamer state=running"
                       << " exact_frame_binding=true");
        }
        catch (const std::exception& e) {
          NDN_LOG_ERROR("UAV_VIDEO_PIPELINE backend=gstreamer state=failed reason="
                        << e.what());
          std::this_thread::sleep_for(1s);
        }
      }
      std::this_thread::sleep_for(20ms);
    }
    pipeline.stop();
  }

  void
  captureLoop()
  {
    if (m_useGStreamerPipeline) {
      captureLoopGStreamer();
      return;
    }
    std::unique_ptr<FILE, decltype(&pclose)> pipe(nullptr, pclose);
    std::vector<uint8_t> chunkBuffer;
    uint64_t chunkBufferStartMs = 0;
    uint64_t packetizationFlushes = 0;
    while (!m_done.load()) {
      if (!m_captureEnabled.load()) {
        pipe.reset();
        {
          std::lock_guard<std::mutex> guard(m_mutex);
          m_jpegBuffer.clear();
        }
        chunkBuffer.clear();
        chunkBufferStartMs = 0;
        std::this_thread::sleep_for(50ms);
        continue;
      }

      if (m_restartEncoder.exchange(false)) {
        pipe.reset();
      }

      if (!pipe) {
        const auto fps = m_targetFps.load();
        const auto width = m_acceptedFrameWidth.load();
        const auto crf = m_encoderQuality.load();
        std::string inputArgs;
        if (isVideoDevice(m_videoPath)) {
          const auto v4l2Input = resolveV4l2Input(m_videoPath, m_cameraOptions);
          if (!v4l2Input.usable) {
            m_cameraCaptureUsable = false;
            NDN_LOG_WARN("V4L2_CAMERA_UNUSABLE path=" << m_videoPath
                         << " failures=" << m_cameraProbeFailures.fetch_add(1) + 1);
            std::this_thread::sleep_for(1s);
            continue;
          }
          m_cameraCaptureUsable = true;
          inputArgs = " -thread_queue_size 512 -f v4l2";
          if (!v4l2Input.format.empty()) {
            inputArgs += " -input_format " + shellQuote(v4l2Input.format);
          }
          if (!v4l2Input.size.empty()) {
            inputArgs += " -video_size " + shellQuote(v4l2Input.size);
          }
          if (v4l2Input.fps > 0) {
            inputArgs += " -framerate " + std::to_string(v4l2Input.fps);
          }
          inputArgs += " -i " + shellQuote(m_videoPath);
        }
        else {
          inputArgs = " -re -stream_loop -1 -i " + shellQuote(m_videoPath);
        }
        const auto command =
          "ffmpeg -loglevel error" + inputArgs +
          " -vf fps=" + std::to_string(fps) +
          ",scale=" + std::to_string(width) + ":-2 -an "
          " -c:v libx264 -preset veryfast -tune zerolatency "
          "-crf " + std::to_string(crf) + " "
          "-x264-params keyint=60:min-keyint=60:scenecut=0 "
          "-f h264 pipe:1";
        pipe.reset(popen(command.c_str(), "r"));
        if (!pipe) {
          NDN_LOG_WARN("VIDEO_ENCODER_START_FAILED path=" << m_videoPath);
          std::this_thread::sleep_for(1s);
          continue;
        }
      }

      std::array<uint8_t, 8192> buffer{};
      const auto n = m_useLegacyBatchedEncoderRead ?
        static_cast<ssize_t>(fread(buffer.data(), 1, buffer.size(), pipe.get())) :
        ::read(fileno(pipe.get()), buffer.data(), buffer.size());
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n == 0) {
        pipe.reset();
        chunkBufferStartMs = 0;
        continue;
      }
      if (n < 0) {
        NDN_LOG_WARN("VIDEO_ENCODER_PIPE_READ_FAILED errno=" << errno);
        pipe.reset();
        chunkBufferStartMs = 0;
        continue;
      }

      // start() may fence the encoder while this thread is blocked in fread().
      // Never publish the final bytes returned by the old process: they can
      // begin in the middle of a GOP and make a bounded recording undecodable
      // until a later key frame.  Reopen libx264 so cursor zero begins with the
      // new stream's SPS/PPS and IDR boundary.
      if (m_restartEncoder.exchange(false)) {
        pipe.reset();
        chunkBuffer.clear();
        chunkBufferStartMs = 0;
        continue;
      }

      const auto encodedOutputReadyMs = nowMilliseconds();
      ensureCanonicalRecordingSession();
      if (!m_streaming.load()) {
        chunkBuffer.clear();
        chunkBufferStartMs = 0;
        continue;
      }

      if (chunkBuffer.empty()) {
        chunkBufferStartMs = encodedOutputReadyMs;
      }
      chunkBuffer.insert(chunkBuffer.end(), buffer.begin(), buffer.begin() + n);
      while (chunkBuffer.size() >= MAX_VIDEO_PACKET_PAYLOAD) {
        const auto chunkSize = std::min(MAX_VIDEO_PACKET_PAYLOAD, chunkBuffer.size());
        std::vector<uint8_t> packetBytes(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        chunkBuffer.erase(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        appendStreamChunk(std::move(packetBytes), encodedOutputReadyMs);
        chunkBufferStartMs = chunkBuffer.empty() ? 0 : encodedOutputReadyMs;
      }
      if (!m_useLegacyBatchedEncoderRead && !chunkBuffer.empty() &&
          chunkBufferStartMs != 0 &&
          encodedOutputReadyMs >= chunkBufferStartMs + m_encoderPacketizationTimeoutMs) {
        std::vector<uint8_t> packetBytes(chunkBuffer.begin(), chunkBuffer.end());
        chunkBuffer.clear();
        appendStreamChunk(std::move(packetBytes), encodedOutputReadyMs);
        chunkBufferStartMs = 0;
        ++packetizationFlushes;
        if (packetizationFlushes <= 3 || packetizationFlushes % 30 == 0) {
          NDN_LOG_DEBUG("VIDEO_ENCODER_PACKETIZATION_FLUSH"
                        << " flushes=" << packetizationFlushes
                        << " mode=posix-time-bounded"
                        << " timeout_ms=" << m_encoderPacketizationTimeoutMs);
        }
      }
      const auto timeoutNowMs = nowMilliseconds();
      bool publishTimedOutGroup = false;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        publishTimedOutGroup = !m_fecPendingChunks.empty() &&
          m_fecCurrentFrameStartMs != 0 &&
          timeoutNowMs >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs;
      }
      if (publishTimedOutGroup) {
        publishCurrentFrame(timeoutNowMs);
      }
    }
  }

private:
  static bool
  isVideoDevice(const std::string& path)
  {
    return path.rfind("/dev/video", 0) == 0;
  }

  struct V4l2InputSelection
  {
    std::string format;
    std::string size;
    uint64_t fps = 0;
    bool usable = true;
  };

  static std::string
  fourccToFfmpeg(uint32_t fourcc)
  {
    switch (fourcc) {
    case V4L2_PIX_FMT_YUYV:
      return "yuyv422";
    case V4L2_PIX_FMT_MJPEG:
      return "mjpeg";
    case V4L2_PIX_FMT_H264:
      return "h264";
    case V4L2_PIX_FMT_NV12:
      return "nv12";
    case V4L2_PIX_FMT_RGB24:
      return "rgb24";
    default:
      return "";
    }
  }

  static bool
  isAutoValue(const std::string& value)
  {
    return value.empty() || value == "auto";
  }

  static std::string
  frameSizeToString(uint32_t width, uint32_t height)
  {
    if (width == 0 || height == 0) {
      return "";
    }
    return std::to_string(width) + "x" + std::to_string(height);
  }

  static std::string
  chooseSizeForFormat(int fd, uint32_t pixelFormat)
  {
    std::string first;
    std::string firstReasonable;
    for (uint32_t i = 0; ; ++i) {
      v4l2_frmsizeenum size {};
      size.index = i;
      size.pixel_format = pixelFormat;
      if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) {
        break;
      }
      if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        const auto value = frameSizeToString(size.discrete.width, size.discrete.height);
        if (first.empty()) {
          first = value;
        }
        if (size.discrete.width == 640 && size.discrete.height == 480) {
          return value;
        }
        if (firstReasonable.empty() &&
            size.discrete.width <= 1280 && size.discrete.height <= 720) {
          firstReasonable = value;
        }
      }
      else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
               size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        const auto minWidth = size.stepwise.min_width;
        const auto minHeight = size.stepwise.min_height;
        const auto maxWidth = size.stepwise.max_width;
        const auto maxHeight = size.stepwise.max_height;
        if (minWidth <= 640 && minHeight <= 480 &&
            maxWidth >= 640 && maxHeight >= 480) {
          return "640x480";
        }
        return frameSizeToString(minWidth, minHeight);
      }
    }
    if (!firstReasonable.empty()) {
      return firstReasonable;
    }
    return first;
  }

  static V4l2InputSelection
  resolveV4l2Input(const std::string& path, const CameraRuntimeOptions& options)
  {
    V4l2InputSelection selection;
    if (!isAutoValue(options.v4l2InputFormat)) {
      selection.format = options.v4l2InputFormat;
    }
    if (!isAutoValue(options.v4l2InputSize)) {
      selection.size = options.v4l2InputSize;
    }
    selection.fps = options.v4l2InputFps;

    if (!isAutoValue(options.v4l2InputFormat) &&
        !isAutoValue(options.v4l2InputSize)) {
      return selection;
    }

    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      NDN_LOG_WARN("V4L2_CAMERA_PROBE_FAILED path=" << path
                   << " reason=open errno=" << errno);
      selection.usable = false;
      return selection;
    }

    struct ScopedFd
    {
      explicit ScopedFd(int value)
        : fd(value)
      {
      }
      ~ScopedFd()
      {
        if (fd >= 0) {
          close(fd);
        }
      }
      int fd = -1;
    } scoped(fd);

    struct Candidate
    {
      uint32_t fourcc = 0;
      std::string format;
      std::string size;
      int priority = 100;
    };

    std::vector<Candidate> candidates;
    for (uint32_t i = 0; ; ++i) {
      v4l2_fmtdesc fmt {};
      fmt.index = i;
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0) {
        break;
      }
      auto ffmpegFormat = fourccToFfmpeg(fmt.pixelformat);
      if (ffmpegFormat.empty()) {
        continue;
      }
      int priority = 50;
      if (ffmpegFormat == "yuyv422") {
        priority = 0;
      }
      else if (ffmpegFormat == "mjpeg") {
        priority = 10;
      }
      else if (ffmpegFormat == "h264") {
        priority = 20;
      }
      candidates.push_back(Candidate{
        fmt.pixelformat,
        std::move(ffmpegFormat),
        chooseSizeForFormat(fd, fmt.pixelformat),
        priority
      });
    }

    if (candidates.empty()) {
      NDN_LOG_WARN("V4L2_CAMERA_PROBE_EMPTY path=" << path);
      selection.usable = false;
      return selection;
    }

    std::sort(candidates.begin(), candidates.end(),
              [] (const Candidate& lhs, const Candidate& rhs) {
                return lhs.priority < rhs.priority;
              });

    const auto& chosen = candidates.front();
    if (isAutoValue(options.v4l2InputFormat)) {
      selection.format = chosen.format;
    }
    if (isAutoValue(options.v4l2InputSize)) {
      selection.size = chosen.size;
    }
    NDN_LOG_INFO("V4L2_CAMERA_AUTO path=" << path
                 << " format=" << selection.format
                 << " size=" << selection.size
                 << " fps=" << selection.fps);
    return selection;
  }

  // Preserve the original high-packet-rate UAV load as the prefetch gate.
  // Packet sizing is an APP capacity decision, not a Core pipeline repair.
  static constexpr size_t MAX_VIDEO_PACKET_PAYLOAD = 3600;
  static constexpr uint64_t MIN_VIDEO_BITRATE_KBPS = 256;
  static constexpr uint64_t MAX_VIDEO_BITRATE_KBPS = 16000;
  static constexpr uint64_t MIN_VIDEO_FRAME_WIDTH = 160;
  static constexpr uint64_t MAX_VIDEO_FRAME_WIDTH = 1280;
  ndn_service_framework::ServiceProvider& m_serviceProvider;
  ndn::Face& m_face;
  ndn::KeyChain& m_keyChain;
  ndn::security::SigningInfo m_signingInfo;
  ndn_service_framework::LocalServiceRegistry& m_localRegistry;
  ndn::Name m_localArchivedPacketServiceName;
  UavRuntimeConfig m_config;
  std::string m_droneId;
  std::string m_videoPath;
  CameraRuntimeOptions m_cameraOptions;
  std::unique_ptr<ndnsf_distributed_repo::RepoCore> m_recordingRepo;
  std::string m_recordingSessionId = "record-idle";
  mutable std::mutex m_mutex;
  std::mutex m_signMutex;
  ndn::Name m_streamPrefix;
  std::string m_streamId = "idle";
  uint64_t m_streamSessionEpoch = 0;
  bool m_recordingFilterRegistered = false;
  std::condition_variable m_readinessCv;
  UavH264ReadinessTracker m_readiness{3};
  VideoStreamDescriptor m_streamDescriptor;
  std::shared_ptr<ndn_service_framework::LiveStreamPublisher> m_livePublisher;
  std::shared_ptr<ndn_service_framework::StreamPublisher> m_streamPublisher;
  std::shared_ptr<ndn_service_framework::PublishedPacketFeed> m_recordingPacketFeed;
  std::mutex m_retentionLifecycleMutex;
  std::optional<ndn_service_framework::PredictiveStreamDescriptor>
    m_coreStreamDescriptor;
  std::optional<UavVideoSampleClassSchedule> m_videoSampleClassSchedule;
  std::unique_ptr<UavVideoNonceUseGuard> m_nonceGuard;
  std::string m_activeStartFingerprint;
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_captureEnabled{false};
  std::atomic<bool> m_cameraCaptureUsable{true};
  std::atomic<uint64_t> m_cameraProbeFailures{0};
  std::atomic<bool> m_done{false};
  std::atomic<uint64_t> m_nextSeq{0};
  std::atomic<uint64_t> m_nextFecFrameSeq{0};
  std::atomic<uint64_t> m_nextMediaSequence{0};
  std::map<uint64_t, uint64_t> m_mediaSequenceByCursor;
  uint64_t m_nextAnnouncedSampleId = 0;
  std::map<uint64_t, ndn_service_framework::LiveStreamSampleReservation>
    m_announcedVideoSamples;
  std::atomic<uint64_t> m_recordingChunks{0};
  std::atomic<uint64_t> m_recordingBytes{0};
  std::atomic<uint64_t> m_recordingLastUpdateMs{0};
  std::atomic<uint64_t> m_retentionQueuePackets{0};
  std::atomic<uint64_t> m_retentionQueueBytes{0};
  std::atomic<uint64_t> m_retentionGapPackets{0};
  std::atomic<uint64_t> m_retentionFailures{0};
  std::atomic<bool> m_retentionStorageCircuitOpen{false};
  std::atomic<bool> m_retentionWorkerBusy{false};
  std::atomic<uint64_t> m_retentionCheckpointCursor{0};
  std::atomic<uint64_t> m_nextRecordingSessionRetryMs{0};
  std::atomic<bool> m_retentionActive{false};
  mutable std::mutex m_retentionMutex;
  CanonicalVideoRecordingManifest m_retentionManifest;
  std::optional<CanonicalVideoRecordingManifest> m_latestCompletedManifest;
  std::optional<VideoStreamDescriptor> m_latestCompletedDescriptor;
  std::map<ndn::Name, ndn_service_framework::StreamContentDigest>
    m_committedPacketDigests;
  std::map<uint64_t, ndn_service_framework::StreamContentDigest>
    m_retainedMappingContentDigests;
  ndn_service_framework::StreamContentDigest m_retentionCatalogHead{};
  bool m_retentionHasSource = false;
  uint64_t m_lastObservedFeedDrops = 0;
  bool m_legacyRecordingDetected = false;
  std::atomic<uint64_t> m_targetFps{30};
  std::atomic<uint64_t> m_requestedBitrateKbps{8000};
  std::atomic<uint64_t> m_acceptedBitrateKbps{8000};
  std::atomic<uint64_t> m_requestedFrameWidth{480};
  std::atomic<uint64_t> m_acceptedFrameWidth{480};
  std::atomic<uint64_t> m_encoderQuality{6};
  std::atomic<bool> m_restartEncoder{false};
  bool m_useLegacyBatchedEncoderRead = false;
  bool m_useGStreamerPipeline = false;
  static constexpr uint64_t m_encoderPacketizationTimeoutMs = 20;
  std::vector<uint8_t> m_jpegBuffer;
  std::vector<std::vector<uint8_t>> m_fecPendingChunks;
  uint64_t m_fecCurrentFrameStartMs = 0;
  uint64_t m_fecDataShards = 8;
  uint64_t m_fecParityShards = 1;
  static constexpr uint64_t m_fecFrameTimeoutMs = 35;
  std::thread m_captureThread;
  std::thread m_retentionThread;
};

class DroneServiceContainer
{
private:
  class RepoStatusLocalHelper
  {
  public:
    explicit RepoStatusLocalHelper(std::function<Fields()> repoStatusProvider)
      : m_repoStatusProvider(std::move(repoStatusProvider))
    {
    }

    void
    registerService(ndn_service_framework::LocalServiceRegistry& localRegistry,
                   const ndn::Name& serviceName)
    {
      localRegistry.registerLocalService(
        serviceName,
        [this](const ndn::Name&,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage&) {
          return makeResponse(true, encodeFields(m_repoStatusProvider()));
        });
    }

  private:
    std::function<Fields()> m_repoStatusProvider;
  };

  class RecordingManifestLocalHelper
  {
  public:
    explicit RecordingManifestLocalHelper(
      std::function<Fields(const ndn::Name&)> manifestProvider)
      : m_manifestProvider(std::move(manifestProvider))
    {
    }

    void
    registerService(ndn_service_framework::LocalServiceRegistry& localRegistry,
                   const ndn::Name& serviceName)
    {
      localRegistry.registerLocalService(
        serviceName,
        [this](const ndn::Name& requesterIdentity,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage&) {
          return makeResponse(true, encodeFields(m_manifestProvider(requesterIdentity)));
        });
    }

  private:
    std::function<Fields(const ndn::Name&)> m_manifestProvider;
  };

public:
  DroneServiceContainer(std::string droneId, bool available, bool serveCertificates,
               UavRuntimeConfig config,
               std::string videoPath, std::string flightControllerBackend,
               std::string mavlinkUdpHost, std::string mavlinkUdpPort,
               std::string mavlinkUdpListenPort, std::string mavlinkSerialDevice,
               std::string mavlinkSerialBaud,
               bool configurePx4SitlDemoParams,
               VideoPublisher::CameraRuntimeOptions cameraOptions)
    : m_serveCertificates(serveCertificates)
    , m_config(std::move(config))
    , m_droneId(std::move(droneId))
    , m_available(available)
    , m_identity(droneIdentity(m_config, m_droneId))
    , m_coreContainer({
        m_identity,
        m_config.groupPrefix,
        m_config.controllerPrefix,
        m_config.trustSchema
      })
    , m_flightControllerBackend(std::move(flightControllerBackend))
    , m_mavlinkUdpHost(std::move(mavlinkUdpHost))
    , m_mavlinkUdpPort(std::move(mavlinkUdpPort))
    , m_mavlinkUdpListenPort(std::move(mavlinkUdpListenPort))
    , m_mavlinkSerialDevice(std::move(mavlinkSerialDevice))
    , m_mavlinkSerialBaud(std::move(mavlinkSerialBaud))
    , m_configurePx4SitlDemoParams(configurePx4SitlDemoParams)
    , m_cameraOptions(std::move(cameraOptions))
  {
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_providerCert = getOrCreateIdentity(m_keyChain, m_identity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_config.controllerPrefix);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_identity));
    m_videoPath = std::move(videoPath);
    m_coreContainer.addLifecycleHook("drone-runtime", {
      [this] { publishStatus("NDNSF service container started"); },
      [this] { publishStatus("NDNSF service container stopped"); }
    });
  }

  ~DroneServiceContainer()
  {
    m_coreContainer.stop();
    m_statusCallback = nullptr;
    stopObjectDetectionLoop();
    m_done = true;
    m_face.getIoContext().stop();
    if (m_faceThread.joinable()) {
      m_faceThread.join();
    }
  }

  void
  start()
  {
    m_faceThread = std::thread([this] {
      try {
        if (m_serveCertificates) {
          m_certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
            m_face, m_keyChain, m_providerCert.getName());
        }
        auto provider = std::make_unique<ndn_service_framework::ServiceProvider>(
          m_face, m_config.groupPrefix, m_providerCert, m_controllerCert, m_config.trustSchema);
        auto videoPublisher = std::make_unique<VideoPublisher>(
          *provider, m_face, m_keyChain, m_coreContainer.localRegistry(), localArchivedPacketServiceName(),
          m_config, m_droneId, m_videoPath, m_cameraOptions);
        {
          std::lock_guard<std::mutex> guard(m_containerMutex);
          m_provider = std::move(provider);
          m_videoPublisher = std::move(videoPublisher);
        }
        m_coreContainer.useProvider("drone-services", *m_provider);
        m_user = std::make_unique<ndn_service_framework::ServiceUser>(
          m_face, m_config.groupPrefix, m_providerCert, m_controllerCert, m_config.trustSchema);
        m_user->setHandlerThreads(1);
        m_coreContainer.useUser("drone-user", *m_user);
        installServiceInstances();
        m_provider->init();
        m_provider->fetchPermissionsFromController(m_config.controllerPrefix);
        m_user->init();
        m_user->fetchPermissionsFromController(m_config.controllerPrefix);
        m_coreContainer.start();
        m_containerReady = true;
        publishStatus("NDNSF runtime ready");

        auto nextServiceAdvertisement = std::chrono::steady_clock::now();
        while (!m_done.load()) {
          m_face.getIoContext().run_for(std::chrono::milliseconds(10));
          m_face.getIoContext().restart();
          const auto now = std::chrono::steady_clock::now();
          if (now >= nextServiceAdvertisement) {
            publishServiceAdvertisements();
            nextServiceAdvertisement = now + std::chrono::seconds(15);
          }
        }
      }
      catch (const std::exception& e) {
        publishStatus(std::string("NDNSF runtime error: ") + e.what());
        m_done = true;
      }
    });
  }

  bool
  waitUntilReady(std::chrono::seconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (m_containerReady.load()) {
        return true;
      }
      if (m_done.load()) {
        return false;
      }
      std::this_thread::sleep_for(50ms);
    }
    return m_containerReady.load();
  }

  ndn_service_framework::ServiceContainer&
  ndnsfContainer()
  {
    return m_coreContainer;
  }

  ndn_service_framework::LocalServiceRegistry&
  localRegistry()
  {
    return m_coreContainer.localRegistry();
  }

  void
  setStatusCallback(std::function<void(std::string)> callback)
  {
    m_statusCallback = std::move(callback);
  }

  bool
  isStreaming() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr && m_videoPublisher->isStreaming();
  }

  bool
  isCapturing() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr && m_videoPublisher->isCapturing();
  }

  bool
  isRecording() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr && m_videoPublisher->isRecording();
  }

  Fields
  cameraStatusFields() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    const bool available = m_videoPublisher != nullptr && m_videoPublisher->cameraAvailable();
    return {
      {"camera_available", available ? "true" : "false"},
      {"camera_source", m_videoPublisher != nullptr ? m_videoPublisher->cameraSource() : m_videoPath},
      {"camera_reason", m_videoPublisher != nullptr ? m_videoPublisher->cameraReason() :
        (m_videoPath.empty() ? std::string("no-source-configured") : std::string("publisher-not-ready"))},
    };
  }

  uint64_t
  streamPacketsPublished() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->streamPacketsPublished() : 0;
  }

  uint64_t
  fecGroupsPublished() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->fecGroupsPublished() : 0;
  }

  uint64_t
  recordingChunks() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->recordingChunks() : 0;
  }

  uint64_t
  recordingBytes() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->recordingBytes() : 0;
  }

  Fields
  recordingManifestFields() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    if (m_videoPublisher == nullptr) {
      return Fields{
        {"type", "camera-recording-manifest"},
        {"drone_id", m_droneId},
        {"recording", "off"},
        {"recording_chunks", "0"},
        {"recording_bytes", "0"},
      };
    }
    return m_videoPublisher->recordingManifestFields();
  }

  Fields
  recordingManifestFieldsFor(const ndn::Name& requesterIdentity) const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    if (m_videoPublisher == nullptr) {
      return {{"type", "canonical-video-recording-pending"},
              {"drone_id", m_droneId}, {"recording", "off"},
              {"recording_packets", "0"}, {"recording_bytes", "0"}};
    }
    return m_videoPublisher->recordingManifestFieldsFor(requesterIdentity);
  }

  Fields
  repoStatusFields() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    if (m_videoPublisher == nullptr) {
      return Fields{
        {"type", "camera-repo-status"},
        {"drone_id", m_droneId},
        {"repo_open", "false"},
        {"recording_enabled", "false"},
        {"recording_repo_path", "none"},
        {"recording_object_prefix",
         m_cameraOptions.recordObjectPrefix.empty() ? "none" : m_cameraOptions.recordObjectPrefix},
        {"recording_session_id", "record-idle"},
        {"recording_chunks", "0"},
        {"recording_bytes", "0"},
        {"recording_last_update_ms", "0"},
        {"retention_packet_limit", std::to_string(m_cameraOptions.recordPacketLimit)},
        {"timestamp_ms", std::to_string(nowMilliseconds())},
      };
    }
    return m_videoPublisher->repoStatusFields();
  }

  Fields
  recordingCatalogFields() const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    if (m_videoPublisher == nullptr) {
      UavDataProductCatalogState catalog;
      catalog.sourceRepo = droneIdentity(m_config, m_droneId).append("local-repo").toUri();
      auto fields = catalog.toFields();
      fields["type"] = "uav-data-product-catalog";
      fields["drone_id"] = m_droneId;
      fields["repo_open"] = "false";
      fields["recording_repo_path"] = "none";
      fields["timestamp_ms"] = std::to_string(nowMilliseconds());
      return fields;
    }
    return m_videoPublisher->recordingCatalogFields();
  }

  std::vector<uint8_t>
  archivedPacketWire(const std::string& objectName) const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->archivedPacketWire(objectName)
                                       : std::vector<uint8_t>{};
  }

  TelemetryState
  latestTelemetryState() const
  {
    Fields telemetry;
    if (m_backend) {
      telemetry = m_backend->latestTelemetry();
    }
    telemetry["drone_id"] = m_droneId;
    telemetry["video"] = isStreaming() ? "streaming" : "stopped";
    telemetry["capture"] = isCapturing() ? "on" : "off";
    telemetry["recording"] = isRecording() ? "on" : "off";
    telemetry["flight_controller_backend"] = m_flightControllerBackend;
    telemetry["flight_controller_available"] = m_backend ? "true" : "false";
    telemetry["flight_controller_reason"] = m_backend ? "ok" : "backend-not-created";
    const auto cameraFields = cameraStatusFields();
    telemetry.insert(cameraFields.begin(), cameraFields.end());
    telemetry["stream_packets_published"] = std::to_string(streamPacketsPublished());
    telemetry["fec_groups_published"] = std::to_string(fecGroupsPublished());
    telemetry["frames_published"] = std::to_string(fecGroupsPublished());
    telemetry["recording_chunks"] = std::to_string(recordingChunks());
    telemetry["recording_bytes"] = std::to_string(recordingBytes());
    telemetry["timestamp_ms"] = std::to_string(nowMilliseconds());
    return TelemetryState::fromFields(telemetry);
  }

  ReadinessState
  latestReadinessState() const
  {
    return ReadinessState::fromTelemetry(latestTelemetryState());
  }

  VideoState
  latestVideoState() const
  {
    return VideoState::fromFields(latestTelemetryState().toFields());
  }

  Fields
  preflightChecklistFields()
  {
    const auto telemetry = latestTelemetryState();
    const auto readiness = ReadinessState::fromTelemetry(telemetry);
    const auto camera = cameraStatusFields();
    const auto now = nowMilliseconds();
    std::vector<PreflightCheckItem> items;
    auto addItem = [&](std::string id, std::string label, std::string category,
                       bool pass, std::string reason, bool blocking) {
      PreflightCheckItem item;
      item.checkId = std::move(id);
      item.droneId = m_droneId;
      item.label = std::move(label);
      item.category = std::move(category);
      item.status = pass ? "pass" : "fail";
      item.reason = std::move(reason);
      item.blocking = blocking;
      item.order = static_cast<uint64_t>(items.size() + 1);
      item.updatedMs = now;
      items.push_back(std::move(item));
    };

    addItem("heartbeat", "Heartbeat", "Link",
            telemetry.heartbeatSeen == "true",
            telemetry.heartbeatSeen == "true" ? "ok" : "waiting-heartbeat",
            true);
    addItem("flight-controller", "Flight controller", "Vehicle",
            telemetry.flightControllerAvailable != "false" &&
              telemetry.flightControllerReady == "true",
            telemetry.flightControllerReady == "true" ? "ok" : telemetry.flightControllerReason,
            true);
    addItem("gps", "GPS", "Sensors",
            readiness.gpsReady == "true",
            readiness.gpsReady == "true" ? "ok" : "gps-not-ready",
            true);
    addItem("battery", "Battery", "Power",
            readiness.batteryReady == "true",
            readiness.batteryReady == "true" ? "ok" : "battery-not-ready",
            true);
    addItem("camera", "Camera", "Payload",
            fieldOr(camera, "camera_available", "unknown") == "true",
            fieldOr(camera, "camera_reason", "camera-status-unknown"),
            false);

    Fields fields{
      {"type", "preflight-checklist"},
      {"preflight_drone", m_droneId},
      {"preflight_count", std::to_string(items.size())},
      {"preflight_updated_ms", std::to_string(now)},
    };
    for (size_t i = 0; i < items.size(); ++i) {
      const auto itemFields = items[i].toFields();
      const auto prefix = "check." + std::to_string(i) + ".";
      for (const auto& [key, value] : itemFields) {
        fields[prefix + key] = value;
      }
    }
    return fields;
  }

  Fields
  analyzeSnapshotFields(const MissionState& mission)
  {
    const auto telemetry = latestTelemetryState();
    const auto readiness = ReadinessState::fromTelemetry(telemetry);
    const auto video = VideoState::fromFields(telemetry.toFields());
    const auto now = nowMilliseconds();
    UavAnalyzeSnapshot snapshot;
    snapshot.droneId = m_droneId;
    snapshot.linkState = telemetry.linkState;
    snapshot.flightMode = readiness.mode;
    snapshot.missionPhase = mission.phase;
    snapshot.videoState = video.status;
    snapshot.parameterCacheStatus = m_backend ? "available" : "unavailable";
    snapshot.updatedMs = now;

    auto addMessage = [&](std::string name, uint64_t id, uint64_t count,
                          std::string rateHz, bool active) {
      MavlinkMessageSummary summary;
      summary.messageName = std::move(name);
      summary.messageId = id;
      summary.systemId = 1;
      summary.componentId = 1;
      summary.count = count;
      summary.rateHz = std::move(rateHz);
      summary.lastSeenMs = active ? now : 0;
      snapshot.messages.push_back(std::move(summary));
    };

    addMessage("HEARTBEAT", 0, telemetry.heartbeatSeen == "true" ? 1 : 0,
               "1.0", telemetry.heartbeatSeen == "true");
    addMessage("LOCAL_POSITION_NED", 32,
               telemetry.altitudeM != "unknown" ? 1 : 0,
               "2.0", telemetry.altitudeM != "unknown");
    addMessage("GLOBAL_POSITION_INT", 33,
               telemetry.lat != "unknown" && telemetry.lon != "unknown" ? 1 : 0,
               "2.0", telemetry.lat != "unknown" && telemetry.lon != "unknown");
    addMessage("BATTERY_STATUS", 147,
               telemetry.batteryPercent != "unknown" ? 1 : 0,
               "0.5", telemetry.batteryPercent != "unknown");
    return snapshot.toFields();
  }

  std::string
  identityUri() const
  {
    return m_identity.toUri();
  }

private:
  void
  publishStatus(const std::string& value)
  {
    NDN_LOG_INFO("DRONE_STATUS drone=" << m_droneId << " " << value);
    if (m_statusCallback) {
      m_statusCallback(value);
    }
  }

  void
  installServiceInstances()
  {
    using ServiceInvocationMode = ndn_service_framework::ServiceProvider::ServiceInvocationMode;

    m_provider->setHandlerThreads(2);
    m_provider->setAckThreads(2);
    m_provider->setPerformanceMode(false);

    std::shared_ptr<FlightControllerBackend> backend;
    if (m_flightControllerBackend == "udp" || m_flightControllerBackend == "mavlink-router") {
      backend = std::make_shared<UdpFlightControllerBackend>(
        m_droneId, m_mavlinkUdpHost, m_mavlinkUdpPort, m_mavlinkUdpListenPort,
        m_configurePx4SitlDemoParams);
    }
    else if (m_flightControllerBackend == "serial") {
      backend = std::make_shared<UdpFlightControllerBackend>(
        m_droneId, m_mavlinkSerialDevice, m_mavlinkSerialBaud);
    }
    else {
      backend = std::make_shared<MockFlightControllerBackend>(m_droneId);
    }
    m_backend = backend;
    auto missionState = std::make_shared<MissionState>();
    missionState->droneId = m_droneId;
    auto missionMutex = std::make_shared<std::mutex>();
    auto missionBusy = std::make_shared<std::atomic<bool>>(false);

    auto ackHandler = [this, backend](
                        const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = m_available;
      decision.message = m_available ? "drone ready" : "drone unavailable";
      decision.payload = bufferFromString(encodeFields({
        {"drone_id", m_droneId},
        {"backend", backend->description()},
        {"queue", "0"},
        {"capture", isCapturing() ? "true" : "false"},
        {"recording", isRecording() ? "true" : "false"},
        {"recording_chunks", std::to_string(recordingChunks())},
        {"streaming", isStreaming() ? "true" : "false"},
      }));
      return decision;
    };

    auto missionAckHandler = [this, missionState, missionMutex, missionBusy, backend](
                               const ndn_service_framework::RequestMessage&) {
      MissionState mission;
      {
        std::lock_guard<std::mutex> guard(*missionMutex);
        mission = *missionState;
      }
      const bool busyForAssignment = mission.isBusyForAssignment();
      const bool busy = missionBusy->load() || busyForAssignment;
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = m_available && !busy;
      decision.message = decision.status ? "mission slot available" :
                         (busy ? "mission slot busy" : "drone unavailable");
      decision.payload = bufferFromString(encodeFields({
        {"drone_id", m_droneId},
        {"backend", backend->description()},
        {"capture", isCapturing() ? "true" : "false"},
        {"recording", isRecording() ? "true" : "false"},
        {"mission_busy", busy ? "true" : "false"},
        {"mission_phase", mission.phase},
        {"mission_detail", busy ? mission.detail : "mission-slot-available"},
        {"queue", busy ? "1" : "0"},
        {"streaming", isStreaming() ? "true" : "false"},
      }));
      return decision;
    };

    m_coreContainer.localRegistry().registerLocalService(
      localCameraStatusServiceName(),
      [this](const ndn::Name&,
             const ndn::Name&,
             const ndn_service_framework::RequestMessage&) {
        return makeResponse(true, encodeFields(cameraStatusFields()));
      });

    if (!m_repoStatusLocalHelper) {
      m_repoStatusLocalHelper = std::make_unique<RepoStatusLocalHelper>(
        [this] { return repoStatusFields(); });
    }
    m_repoStatusLocalHelper->registerService(m_coreContainer.localRegistry(),
                                            localRepoStatusServiceName());

    if (!m_recordingManifestLocalHelper) {
      m_recordingManifestLocalHelper = std::make_unique<RecordingManifestLocalHelper>(
        [this](const ndn::Name& requesterIdentity) {
          return recordingManifestFieldsFor(requesterIdentity);
        });
    }
    m_recordingManifestLocalHelper->registerService(m_coreContainer.localRegistry(),
                                                   localRecordingManifestServiceName());

    m_provider->addService(
      droneVideoControlService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto action = fieldOr(fields, "action", "start");
          if (action == "start") {
            std::lock_guard<std::mutex> guard(m_containerMutex);
            const auto responseFields = m_videoPublisher->start(fields);
            publishStatus("video streaming");
            startObjectDetectionLoop();
            return makeResponse(true, encodeFields(responseFields));
          }
          if (action == "stop") {
            const auto delayText = fieldOr(fields, "simulate_delay_ms", "0");
            if (delayText != "0") {
              try {
                const auto delayMs = std::min<uint64_t>(std::stoull(delayText), 10000);
                if (delayMs > 0) {
                  NDN_LOG_INFO("DRONE_VIDEO_STOP_SIMULATED_DELAY_MS drone=" << m_droneId
                               << " delay_ms=" << delayMs);
                  std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
              }
              catch (const std::exception& e) {
                NDN_LOG_WARN("DRONE_VIDEO_STOP_SIMULATED_DELAY_INVALID drone=" << m_droneId
                             << " value=" << delayText << " error=" << e.what());
              }
            }
            Fields responseFields;
            bool alreadyStopped = false;
            {
              std::lock_guard<std::mutex> guard(m_containerMutex);
              alreadyStopped = !m_videoPublisher->isStreaming();
              if (alreadyStopped) {
                responseFields =
                  m_videoPublisher->stopWithReason("already-stopped");
              }
              else {
                responseFields = m_videoPublisher->stop();
              }
            }
            // Joining while holding m_containerMutex can deadlock when the
            // detection loop is already entering isStreaming(), which takes
            // the same mutex. Complete the APP-owned thread lifecycle only
            // after releasing the container state lock.
            if (!alreadyStopped) {
              stopObjectDetectionLoop();
            }
            publishStatus(alreadyStopped ? "video already stopped" :
                                           "video stopped");
            return makeResponse(true, encodeFields(responseFields));
          }
          return makeResponse(false, encodeFields({
            {"status", "rejected"},
            {"reason", "unknown video control action"},
            {"action", action},
          }), "unknown video control action");
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      droneCameraRecordingManifestService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [this](const ndn::Name& requesterIdentity,
               const ndn::Name&,
               const ndn::Name&,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto retentionAction = fieldOr(fields, "retention_action", "status");
          if (retentionAction == "start") {
            (void)m_videoPublisher->startRetention();
          }
          else if (retentionAction == "stop" ||
                   fieldOr(fields, "finalize_retention", "false") == "true") {
            (void)m_videoPublisher->finalizeRetention();
          }
          else if (retentionAction == "restart") {
            (void)m_videoPublisher->finalizeRetention();
            (void)m_videoPublisher->startRetention();
          }
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            localRecordingManifestServiceName(), request, response, requesterIdentity);
          const auto responseFields = decodeFields(responsePayload(response));
          NDN_LOG_INFO("CAMERA_CANONICAL_MANIFEST_RESPONSE type="
                       << fieldOr(responseFields, "type", "missing")
                       << " fields=" << responseFields.size()
                       << " bytes=" << response.getPayload().size());
          return response;
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      droneCameraRepoCatalogService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          return makeResponse(true, encodeFields(recordingCatalogFields()));
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      droneMavlinkParametersService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [backend, this](const ndn_service_framework::RequestMessage&) {
          auto snapshot = backend->parameterSnapshot();
          snapshot.droneId = m_droneId;
          return makeResponse(true, encodeFields(snapshot.toFields(true)));
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      droneMavlinkParameterEditService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [backend, this](const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          auto editRequest = VehicleParameterEditRequest::fromFields(fields);
          if (editRequest.droneId == "unknown") {
            editRequest.droneId = m_droneId;
          }
          auto result = backend->editParameter(editRequest);
          result.droneId = m_droneId;
          const bool ok = result.successful() || (result.accepted && result.verified && result.reason == "dry-run");
          NDN_LOG_INFO("DRONE_PARAMETER_EDIT_RESULT drone=" << m_droneId
                       << " ok=" << (ok ? "true" : "false")
                       << " param=" << result.parameterName
                       << " reason=" << result.reason
                       << " verified_value=" << result.verifiedValue);
          return makeResponse(ok, encodeFields(result.toFields()),
                              ok ? "No error" : result.reason);
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      dronePreflightChecklistService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          return makeResponse(true, encodeFields(preflightChecklistFields()));
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      droneMavlinkAnalyzeSnapshotService(m_config, m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this, missionState, missionMutex](const ndn_service_framework::RequestMessage&) {
          MissionState mission;
          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            mission = *missionState;
          }
          return makeResponse(true, encodeFields(analyzeSnapshotFields(mission)));
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      m_config.serviceMavlinkExecute,
      ndn_service_framework::ServiceProvider::AckStrategyHandler{},
      ndn_service_framework::ServiceProvider::RequestHandler(
        [backend, this, missionState, missionMutex](
          const ndn::Name&, const ndn::Name&, const ndn::Name&,
          const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto command = fieldOr(fields, "command", "unknown");
          const auto frame = hexDecode(fieldOr(fields, "mavlink_hex", ""));
          auto result = backend->sendMavlink(frame, command);
          const bool ok = fieldOr(result, "accepted", "false") == "true";
          if (ok && (command == "start_mission" || command == "land" ||
                     command == "emergency_stop" || command == "disarm")) {
            std::lock_guard<std::mutex> guard(*missionMutex);
            missionState->droneId = m_droneId;
            missionState->updatedMs = nowMilliseconds();
            if (command == "start_mission") {
              missionState->phase = "executing";
              missionState->detail = "flight-controller-mission-started";
            }
            else {
              missionState->phase = "stopping";
              missionState->detail = command + "-sent-to-flight-controller";
            }
          }
          result["backend"] = backend->description();
          result["drone_id"] = m_droneId;
          return makeResponse(ok, encodeFields(result),
                              ok ? "No error" : "flight-controller rejected frame");
        }),
      ServiceInvocationMode::TargetedOnly);

    auto telemetryHandler =
      [backend, this, missionState, missionMutex](const ndn_service_framework::RequestMessage&) {
          MissionState mission;
          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            mission = *missionState;
          }
          auto telemetry = backend->latestTelemetry();
          auto missionFields = mission.toFields();
          telemetry.insert(missionFields.begin(), missionFields.end());
          telemetry["drone_id"] = m_droneId;
          telemetry.emplace("lat", "35.1186");
          telemetry.emplace("lon", "-89.9375");
          telemetry["mission_status"] = mission.phase;
          telemetry["video"] = isStreaming() ? "streaming" : "stopped";
          telemetry["capture"] = isCapturing() ? "on" : "off";
          telemetry["recording"] = isRecording() ? "on" : "off";
          telemetry["flight_controller_backend"] = m_flightControllerBackend;
          telemetry["flight_controller_available"] = m_backend ? "true" : "false";
          telemetry["flight_controller_reason"] = m_backend ? "ok" : "backend-not-created";
          ndn_service_framework::ResponseMessage cameraResponse;
          m_coreContainer.localRegistry().localInvokeRawInto(
            localCameraStatusServiceName(), ndn_service_framework::RequestMessage{},
            cameraResponse, m_identity);
          if (cameraResponse.getStatus()) {
            const auto cameraPayload = cameraResponse.getPayload();
            const auto cameraFields = decodeFields(std::string(
              reinterpret_cast<const char*>(cameraPayload.data()), cameraPayload.size()));
            telemetry.insert(cameraFields.begin(), cameraFields.end());
          }
          telemetry["stream_packets_published"] = std::to_string(streamPacketsPublished());
          telemetry["fec_groups_published"] = std::to_string(fecGroupsPublished());
          telemetry["frames_published"] = std::to_string(fecGroupsPublished());
          telemetry["recording_chunks"] = std::to_string(recordingChunks());
          telemetry["recording_bytes"] = std::to_string(recordingBytes());
          telemetry["timestamp_ms"] = std::to_string(nowMilliseconds());
          return makeResponse(true, encodeFields(telemetry));
        };

    m_provider->addService(
      m_config.serviceTelemetryStatus,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [telemetryHandler](const ndn::Name&, const ndn::Name&,
                           const ndn::Name& serviceName, const ndn::Name& requestId,
                           const ndn_service_framework::RequestMessage& request) {
          NDN_LOG_INFO("UAV_TELEMETRY_PROVIDER_PHASE phase=handler-enter"
                       << " request_id=" << requestId.toUri()
                       << " service=" << serviceName.toUri()
                       << " timestamp_ms=" << nowMilliseconds()
                       << " status=running");
          auto response = telemetryHandler(request);
          NDN_LOG_INFO("UAV_TELEMETRY_PROVIDER_PHASE phase=handler-return"
                       << " request_id=" << requestId.toUri()
                       << " service=" << serviceName.toUri()
                       << " timestamp_ms=" << nowMilliseconds()
                       << " status=" << (response.getStatus() ? "success" : "failure"));
          return response;
        }),
      ServiceInvocationMode::NormalAndTargeted);

    m_provider->addService(
      m_config.serviceCameraFrame,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          const auto frameId = "frame-" + std::to_string(nowMilliseconds());
          const auto image = buildMockJpeg(m_droneId, frameId);
          return makeResponse(true, encodeFields({
            {"drone_id", m_droneId},
            {"frame_id", frameId},
            {"mime", "image/jpeg"},
            {"image_hex", hexEncode(image)},
            {"timestamp_ms", std::to_string(nowMilliseconds())},
          }));
        }),
      ServiceInvocationMode::NormalOnly);

    m_provider->addService(
      m_config.serviceMissionAssign,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(missionAckHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [backend, this, missionState, missionMutex, missionBusy](
          const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto missionId = fieldOr(fields, "mission_id", "mission-unknown");
          const auto role = fieldOr(fields, "role", "survey");
          const auto partId = fieldOr(fields, "part_id", role);
          const auto attemptId = fieldOr(fields, "attempt_id", "1");
          const auto waypoints = fieldOr(fields, "waypoints", "");
          bool expectedIdle = false;
          if (!missionBusy->compare_exchange_strong(expectedIdle, true)) {
            return makeResponse(false, encodeFields({
              {"accepted", "false"},
              {"reason", "mission-slot-busy"},
              {"drone_id", m_droneId},
              {"part_id", partId},
              {"attempt_id", attemptId},
            }), "mission slot busy");
          }
          struct BusyGuard
          {
            std::shared_ptr<std::atomic<bool>> flag;
            ~BusyGuard()
            {
              flag->store(false);
            }
          } clearBusy{missionBusy};
          if (fieldOr(fields, "simulate_no_response", "false") == "true") {
            const auto delayMs = std::stoul(fieldOr(fields, "simulate_delay_ms", "6000"));
            publishStatus("mission response delayed part=" + partId +
                          " attempt=" + attemptId);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
          }

          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            missionState->droneId = m_droneId;
            missionState->missionId = missionId;
            missionState->partId = partId;
            missionState->phase = "uploading";
            missionState->detail = "forwarding-waypoints-to-flight-controller";
            missionState->updatedMs = nowMilliseconds();
          }

          const auto waypointPairs = parseWaypointPairs(waypoints);
          auto missionResult = backend->executeMissionWaypoints(waypointPairs, {
            {"mission_id", missionId},
            {"role", role},
            {"part_id", partId},
            {"attempt_id", attemptId},
            {"altitude_m", fieldOr(fields, "altitude_m", "15")},
            {"target_system", fieldOr(fields, "target_system", "1")},
            {"target_component", fieldOr(fields, "target_component", "1")},
          });
          if (waypointPairs.empty()) {
            backend->sendMavlink(buildMockMavlinkFrame("mission-waypoints", {
              {"mission_id", missionId},
              {"role", role},
              {"part_id", partId},
              {"attempt_id", attemptId},
              {"waypoints", waypoints},
            }), "mission-waypoints");
          }
          const auto frameId = "mission-" + missionId + "-" + partId + "-capture";
          const auto image = buildMockJpeg(m_droneId, frameId);
          const auto forwardedWaypoints = fieldOr(missionResult, "waypoints_forwarded", "0");
          const bool missionAccepted = fieldOr(missionResult, "accepted", "true") == "true";
          const auto updatedMissionState = MissionState{
            m_droneId,
            missionId,
            partId,
            missionAccepted ? "uploaded" : "failed",
            forwardedWaypoints != "0" ? "mission-waypoints-forwarded-to-fc"
                                      : "mission-executed-with-mock-fc",
            fieldOr(missionResult, "mission_ack", "unknown"),
            fieldOr(missionResult, "mission_transport", "unknown"),
            forwardedWaypoints,
            fieldOr(missionResult, "waypoint_acks_accepted", "0"),
            nowMilliseconds(),
          };
          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            *missionState = updatedMissionState;
          }
          auto responseFields = updatedMissionState.toFields();
          responseFields.insert({
            {"accepted", missionAccepted ? "true" : "false"},
            {"role", role},
            {"attempt_id", attemptId},
            {"status", updatedMissionState.detail},
            {"mission_backend", backend->description()},
            {"last_waypoint_ack", fieldOr(missionResult, "last_waypoint_ack", "unknown")},
            {"mission_item_requests", fieldOr(missionResult, "mission_item_requests", "0")},
            {"captured_frame_id", frameId},
            {"captured_image_bytes", std::to_string(image.size())},
            {"object_detection_service", m_config.serviceGsObjectDetection.toUri()},
            {"detection_summary", "mock-detected=road,vehicle;confidence=0.91"},
          });

          return makeResponse(missionAccepted, encodeFields(responseFields),
                              missionAccepted ? "No error" : "flight controller did not accept mission");
        }),
      ServiceInvocationMode::NormalOnly);
  }

  ndn::Name
  localCameraStatusServiceName() const
  {
    return ndn::Name(m_identity).append("Local").append("Camera").append("Status");
  }

  ndn::Name
  localRecordingManifestServiceName() const
  {
    return ndn::Name(m_identity).append("Local").append("Recording").append("Manifest");
  }

  ndn::Name
  localRepoStatusServiceName() const
  {
    return ndn::Name(m_identity).append("Local").append("Repo").append("Status");
  }

  ndn::Name
  localArchivedPacketServiceName() const
  {
    return ndn::Name(m_identity).append("Local").append("Recording").append("ArchivedPacket");
  }

  void
  publishServiceAdvertisements()
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    if (!m_provider) {
      return;
    }
    const auto common = Fields{
      {"drone_id", m_droneId},
      {"identity", m_identity.toUri()},
      {"backend", m_flightControllerBackend},
      {"available", m_available ? "true" : "false"},
    };
    auto publish = [this, &common](const ndn::Name& serviceName,
                                   const std::string& invocationMode,
                                   const std::string& category) {
      auto meta = common;
      meta["invocation_mode"] = invocationMode;
      meta["category"] = category;
      meta["published_by"] = "NDNSF-UAV-APP";
      m_provider->publishServiceInfo(serviceName, 45, std::move(meta));
    };
    publish(droneVideoControlService(m_config, m_droneId), "normal-only", "video-control");
    publish(droneCameraRecordingManifestService(m_config, m_droneId), "normal-only",
            "camera-recording-manifest");
    publish(m_config.serviceMavlinkExecute, "targeted-only", "flight-control");
    publish(m_config.serviceTelemetryStatus, "normal-and-targeted", "telemetry");
    publish(m_config.serviceCameraFrame, "normal-only", "camera");
    publish(m_config.serviceMissionAssign, "normal-only", "mission");
  }

  void
  startObjectDetectionLoop()
  {
    if (!m_objectDetectionDone.exchange(false)) {
      return;
    }
    if (m_objectDetectionThread.joinable()) {
      m_objectDetectionThread.join();
    }
    m_objectDetectionThread = std::thread([this] {
      uint64_t frameSeq = 0;
      while (!m_objectDetectionDone.load() && !m_done.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (m_objectDetectionDone.load() || m_done.load() || !isStreaming()) {
          continue;
        }
        const auto payload = encodeFields({
          {"type", "live-object-detection"},
          {"drone_id", m_droneId},
          {"frame_id", "live-" + std::to_string(frameSeq)},
          {"frame_seq", std::to_string(frameSeq)},
          {"target_objects", "Car,Truck"},
          {"image_source", "ground-station-latest-decoded-video-frame"},
        });
        boost::asio::post(m_face.getIoContext(), [this, payload, frameSeq] {
          if (!m_user || !m_containerReady.load()) {
            return;
          }
          auto request = makeRequest(payload);
          m_user->RequestService(
            std::vector<ndn::Name>{m_config.groundStationIdentity},
            m_config.serviceGsObjectDetection,
            std::move(request),
            300,
            ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
            2000,
            [](const ndn::Name&) {},
            [this, frameSeq](const ndn_service_framework::ResponseMessage& response) {
              if (!response.getStatus()) {
                return;
              }
              const auto fields = decodeFields(responsePayload(response));
              const bool car = fieldOr(fields, "car", "false") == "true";
              const bool truck = fieldOr(fields, "truck", "false") == "true";
              if (car || truck) {
                publishStatus("object detection frame=" + std::to_string(frameSeq) +
                              " objects=" + fieldOr(fields, "objects", "unknown"));
              }
            });
        });
        ++frameSeq;
      }
    });
  }

  void
  stopObjectDetectionLoop()
  {
    m_objectDetectionDone = true;
    if (m_objectDetectionThread.joinable()) {
      m_objectDetectionThread.join();
    }
  }

private:
  bool m_serveCertificates;
  UavRuntimeConfig m_config;
  std::string m_droneId;
  bool m_available;
  ndn::Name m_identity;
  ndn_service_framework::ServiceContainer m_coreContainer;
  std::unique_ptr<RecordingManifestLocalHelper> m_recordingManifestLocalHelper;
  std::unique_ptr<RepoStatusLocalHelper> m_repoStatusLocalHelper;
  std::string m_videoPath;
  std::string m_flightControllerBackend;
  std::string m_mavlinkUdpHost;
  std::string m_mavlinkUdpPort;
  std::string m_mavlinkUdpListenPort;
  std::string m_mavlinkSerialDevice;
  std::string m_mavlinkSerialBaud;
  bool m_configurePx4SitlDemoParams = false;
  VideoPublisher::CameraRuntimeOptions m_cameraOptions;
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::security::Certificate m_providerCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<ndn_service_framework::CertificatePublisher> m_certPublisher;
  std::unique_ptr<ndn_service_framework::ServiceProvider> m_provider;
  std::unique_ptr<ndn_service_framework::ServiceUser> m_user;
  std::unique_ptr<VideoPublisher> m_videoPublisher;
  std::shared_ptr<FlightControllerBackend> m_backend;
  mutable std::mutex m_containerMutex;
  std::thread m_faceThread;
  std::thread m_objectDetectionThread;
  std::function<void(std::string)> m_statusCallback;
  std::atomic<bool> m_containerReady{false};
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_objectDetectionDone{true};
};
