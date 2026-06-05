// App-internal implementation chunk included by UavDroneApp.cpp.
// Groups flight-controller backends, video publishing, and NDNSF services.

class FlightControllerBackend
{
public:
  virtual ~FlightControllerBackend() = default;
  virtual Fields sendMavlink(const std::vector<uint8_t>& frame,
                             const std::string& commandName) = 0;
  virtual Fields latestTelemetry() = 0;
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
    uint64_t recordChunkLimit = 0;
    std::string v4l2InputFormat = "auto";
    std::string v4l2InputSize = "auto";
    uint64_t v4l2InputFps = 0;
  };

  VideoPublisher(ndn::Face& face, ndn::KeyChain& keyChain,
                 ndn_service_framework::LocalServiceRegistry& localRegistry,
                 ndn::Name localRecordingChunkServiceName,
                 UavRuntimeConfig config, std::string droneId, std::string videoPath,
                 CameraRuntimeOptions options)
    : m_face(face)
    , m_keyChain(keyChain)
    , m_localRegistry(localRegistry)
    , m_localRecordingChunkServiceName(std::move(localRecordingChunkServiceName))
    , m_config(std::move(config))
    , m_droneId(std::move(droneId))
    , m_videoPath(std::move(videoPath))
    , m_cameraOptions(std::move(options))
  {
    m_videoPrefix = droneIdentity(m_config, m_droneId).append("video");
    m_streamPrefix = ndn::Name(m_videoPrefix).append(m_streamId);
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
        m_recordingRepo = std::make_unique<ndnsf_distributed_repo::RepoCore>(
          capability);
      }
      else {
        m_recordingRepo = std::make_unique<ndnsf_distributed_repo::RepoCore>(
          capability,
          ndnsf_distributed_repo::makeSqliteRepoStore(m_cameraOptions.recordRepoPath));
      }
      m_recordingSessionId = "record-" + std::to_string(nowMilliseconds());
      m_recordingKeyId = droneIdentity(m_config, m_droneId)
        .append("repo")
        .append("camera")
        .append("recording")
        .append("hybrid-key")
        .toUri();
      m_recordingContentKey = loadOrCreateRecordingContentKey();
      m_recordingLastUpdateMs = nowMilliseconds();
      m_localRegistry.registerLocalService(
        m_localRecordingChunkServiceName,
        [this](const ndn::Name&,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage& request) {
          const auto objectName = payloadToString(request);
          const auto payload = recordingChunkData(objectName);
          return makeBinaryResponse(!payload.empty(), payload,
                                    payload.empty() ? "recording chunk not found" : "No error");
        });
      ensureRecordingFilterRegistered();
      NDN_LOG_INFO("CAMERA_RECORDING_ENCRYPTION drone=" << m_droneId
                   << " algorithm=hybrid-aes-256-gcm-at-rest"
                   << " key_id=" << m_recordingKeyId
                   << " key_scope=local-drone-runtime");
    }
    m_captureEnabled = m_cameraOptions.captureOnStart || m_cameraOptions.recordToLocalRepo;
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
  }

  Fields
  start(const Fields& requestFields)
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    ++m_streamSessionEpoch;
    m_targetFps = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "fps", "30")), 1, 60);
    m_requestedBitrateKbps = std::max<uint64_t>(
      1, std::stoull(fieldOr(requestFields, "requested_bitrate_kbps",
                             fieldOr(requestFields, "target_bitrate_kbps", "8000"))));
    m_acceptedBitrateKbps = std::clamp<uint64_t>(
      m_requestedBitrateKbps.load(), MIN_VIDEO_BITRATE_KBPS, MAX_VIDEO_BITRATE_KBPS);
    auto requestedWidth = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "requested_frame_width", "480")),
      MIN_VIDEO_FRAME_WIDTH, MAX_VIDEO_FRAME_WIDTH);
    if (requestedWidth % 2 != 0) {
      --requestedWidth;
    }
    m_requestedFrameWidth = requestedWidth;
    m_acceptedFrameWidth = requestedWidth;
    m_encoderQuality = qualityForBitrate(m_acceptedBitrateKbps);
    m_fecDataShards = defaultFecDataShardsForBitrate(m_acceptedBitrateKbps.load());
    m_fecParityShards = 1;
    m_restartEncoder = true;
    ensureFrameFilterRegistered();
    m_streamId = "stream-" + std::to_string(nowMilliseconds());
    m_streamPrefix = ndn::Name(m_videoPrefix).append(m_streamId);
    m_nextSeq = 0;
    m_nextPacketSeq = 0;
    m_nextFecFrameSeq = 0;
    m_packets.clear();
    m_order.clear();
    m_pending.clear();
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_jpegBuffer.clear();
    m_streaming = true;
    m_captureEnabled = true;
    const auto startSecond = nowMilliseconds() / 1000;
    return {
      {"status", "streaming"},
      {"drone_id", m_droneId},
      {"capture", isCapturing() ? "on" : "off"},
      {"recording", isRecording() ? "on" : "off"},
      {"recording_session_id", m_recordingSessionId},
      {"recording_object_prefix", m_cameraOptions.recordObjectPrefix},
      {"recording_chunks", std::to_string(m_recordingChunks.load())},
      {"recording_bytes", std::to_string(m_recordingBytes.load())},
      {"stream_id", m_streamId},
      {"stream_session_epoch", std::to_string(m_streamSessionEpoch)},
      {"stream_prefix", m_streamPrefix.toUri()},
      {"fps", std::to_string(m_targetFps)},
      {"requested_bitrate_kbps", std::to_string(m_requestedBitrateKbps)},
      {"accepted_bitrate_kbps", std::to_string(m_acceptedBitrateKbps)},
      {"min_bitrate_kbps", std::to_string(MIN_VIDEO_BITRATE_KBPS)},
      {"max_bitrate_kbps", std::to_string(MAX_VIDEO_BITRATE_KBPS)},
      {"encoder_quality", std::to_string(m_encoderQuality)},
      {"requested_frame_width", std::to_string(m_requestedFrameWidth)},
      {"accepted_frame_width", std::to_string(m_acceptedFrameWidth)},
      {"start_second", std::to_string(startSecond)},
      {"next_packet", "0"},
      {"encoding", "video/h264"},
      {"stream_format", "stream-start-time/packetSeq with stream-chunk metadata"},
      {"fec_data_shards", std::to_string(m_fecDataShards)},
      {"fec_parity_shards", std::to_string(m_fecParityShards)},
      {"frame_width", std::to_string(m_acceptedFrameWidth)},
      {"max_payload_bytes", std::to_string(MAX_VIDEO_PACKET_PAYLOAD)},
      {"streaming_model", "h264-low-latency-packet-stream"},
      {"prefetch_hint", "budget-from-bitrate"},
      {"source", m_videoPath},
      {"camera_available", cameraAvailable() ? "true" : "false"},
      {"camera_source", cameraSource()},
      {"camera_reason", cameraReason()},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
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
    m_streaming = false;
    if (!m_cameraOptions.captureOnStart && !m_cameraOptions.recordToLocalRepo) {
      m_captureEnabled = false;
    }
    m_pending.clear();
    m_packets.clear();
    m_order.clear();
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_nextFecFrameSeq = 0;
    m_jpegBuffer.clear();
    m_restartEncoder = true;
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
      {"stream_id", m_streamId},
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
    return m_cameraOptions.recordToLocalRepo && m_recordingRepo != nullptr;
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
    return m_cameraOptions.recordRepoPath.empty() ? std::string("memory-db")
                                                 : m_cameraOptions.recordRepoPath;
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
    const auto chunks = recordingChunks();
    Fields fields{
      {"type", "camera-recording-manifest"},
      {"drone_id", m_droneId},
      {"capture", isCapturing() ? "on" : "off"},
      {"recording", isRecording() ? "on" : "off"},
      {"recording_session_id", m_recordingSessionId},
      {"recording_object_prefix", m_cameraOptions.recordObjectPrefix},
      {"recording_object_pattern",
       m_cameraOptions.recordObjectPrefix + "/" + m_recordingSessionId + "/chunk/<index>"},
      {"recording_chunks", std::to_string(chunks)},
      {"recording_bytes", std::to_string(recordingBytes())},
      {"recording_object_type", "video/h264-chunk"},
      {"recording_encryption", isRecording() ? "hybrid-aes-256-gcm-at-rest" : "none"},
      {"recording_encryption_key_id", m_recordingKeyId},
      {"recording_encryption_content_key_hex", recordingContentKeyHex()},
      {"recording_encryption_access", "NDNSF user permission on recording manifest service"},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
    if (chunks > 0) {
      fields["first_chunk_object"] = m_cameraOptions.recordObjectPrefix + "/" +
        m_recordingSessionId + "/chunk/0";
      fields["last_chunk_object"] = m_cameraOptions.recordObjectPrefix + "/" +
        m_recordingSessionId + "/chunk/" + std::to_string(chunks - 1);
    }
    return fields;
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
      {"recording_chunk_limit", std::to_string(m_cameraOptions.recordChunkLimit)},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  std::vector<uint8_t>
  recordingChunk(const std::string& objectName) const
  {
    if (!isRecording() || objectName.empty() || !m_recordingRepo) {
      return {};
    }
    try {
      return decryptRecordingChunk(objectName, m_recordingRepo->get(objectName));
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_RECORD_CHUNK_GET_FAILED object=" << objectName
                   << " reason=" << e.what());
      return {};
    }
  }

  std::vector<uint8_t>
  recordingChunkData(const std::string& objectName) const
  {
    if (!isRecording() || objectName.empty() || !m_recordingRepo) {
      return {};
    }
    try {
      return m_recordingRepo->get(objectName);
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_RECORD_CHUNK_DATA_GET_FAILED object=" << objectName
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
  onFrameInterest(const ndn::Interest& interest)
  {
    const auto interestCount = ++m_frameInterests;
    if (interestCount <= 3 || interestCount % 30 == 0) {
      NDN_LOG_INFO("VIDEO_FRAME_INTEREST count=" << interestCount
                   << " name=" << interest.getName());
    }
    const auto& name = interest.getName();
    if (name.size() < m_streamPrefix.size() + 1) {
      return;
    }

    std::vector<uint8_t> packet;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_streaming.load() || !m_streamPrefix.isPrefixOf(name)) {
        return;
      }
      const auto uri = name.toUri();
      const auto it = m_packets.find(uri);
      if (it != m_packets.end()) {
        packet = it->second;
      }
      else {
        m_pending[uri] = name;
      }
    }

    if (!packet.empty()) {
      putPacket(name, packet);
    }
  }

  void
  ensureFrameFilterRegistered()
  {
    if (m_filterRegistered) {
      return;
    }
    m_face.setInterestFilter(
      m_videoPrefix,
      [this] (const auto&, const ndn::Interest& interest) {
        this->onFrameInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name& prefix, const std::string& reason) {
        NDN_LOG_WARN("VIDEO_PREFIX_REGISTER_FAILED prefix=" << prefix << " reason=" << reason);
      });
    m_filterRegistered = true;
  }

  void
  ensureRecordingFilterRegistered()
  {
    if (m_recordingFilterRegistered || m_cameraOptions.recordObjectPrefix.empty()) {
      return;
    }
    const ndn::Name recordingPrefix(m_cameraOptions.recordObjectPrefix);
    m_face.setInterestFilter(
      recordingPrefix,
      [this] (const auto&, const ndn::Interest& interest) {
        this->onRecordingChunkInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name& prefix, const std::string& reason) {
        NDN_LOG_WARN("CAMERA_RECORDING_PREFIX_REGISTER_FAILED prefix="
                     << prefix << " reason=" << reason);
      });
    m_recordingFilterRegistered = true;
  }

  void
  onRecordingChunkInterest(const ndn::Interest& interest)
  {
    const auto objectName = interest.getName().toUri();
    ndn_service_framework::ResponseMessage response;
    m_localRegistry.localInvokeRawInto(
      m_localRecordingChunkServiceName, makeRequest(objectName), response,
      droneIdentity(m_config, m_droneId));
    const auto responsePayload = response.getPayload();
    std::vector<uint8_t> payload(responsePayload.begin(), responsePayload.end());
    if (payload.empty()) {
      NDN_LOG_DEBUG("CAMERA_RECORDING_CHUNK_MISS object=" << objectName);
      return;
    }

    auto data = std::make_shared<ndn::Data>(interest.getName());
    data->setFreshnessPeriod(2_s);
    data->setContent(ndn::span<const uint8_t>(payload.data(), payload.size()));
    {
      std::lock_guard<std::mutex> guard(m_signMutex);
      m_keyChain.sign(*data);
    }
    boost::asio::post(m_face.getIoContext(), [this, data] {
      m_face.put(*data);
    });
  }

  void
  putPacket(const ndn::Name& name, const std::vector<uint8_t>& packet)
  {
    const auto putCount = ++m_framePuts;
    if (putCount <= 3 || putCount % 30 == 0) {
      NDN_LOG_INFO("VIDEO_PACKET_PUT count=" << putCount
                   << " name=" << name
                   << " bytes=" << packet.size());
    }
    auto data = std::make_shared<ndn::Data>(name);
    data->setFreshnessPeriod(80_ms);
    data->setContent(ndn::span<const uint8_t>(packet.data(), packet.size()));
    {
      std::lock_guard<std::mutex> guard(m_signMutex);
      m_keyChain.sign(*data);
    }
    boost::asio::post(m_face.getIoContext(), [this, data] {
      m_face.put(*data);
    });
  }

  void
  rememberPacket(const ndn::Name& name, const std::vector<uint8_t>& packet)
  {
    ndn::Name pendingName;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      const auto uri = name.toUri();
      m_packets[uri] = packet;
      m_order.push_back(uri);
      while (m_order.size() > 600) {
        m_packets.erase(m_order.front());
        m_order.pop_front();
      }
      const auto pending = m_pending.find(uri);
      if (pending != m_pending.end()) {
        pendingName = pending->second;
        m_pending.erase(pending);
      }
    }

    if (!pendingName.empty()) {
      putPacket(pendingName, packet);
    }
  }

  void
  appendStreamChunk(std::vector<uint8_t> chunk, uint64_t nowMs)
  {
    if (!m_streaming.load()) {
      return;
    }
    if (m_fecPendingChunks.empty()) {
      m_fecCurrentFrameStartMs = nowMs;
    }
    m_fecPendingChunks.push_back(std::move(chunk));

    if (m_fecPendingChunks.size() >= m_fecDataShards ||
        (m_fecCurrentFrameStartMs != 0 &&
         nowMs >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs)) {
      publishCurrentFrame(nowMs);
    }
  }

  void
  recordRawChunk(const uint8_t* data, size_t size, uint64_t captureMs)
  {
    if (!isRecording() || data == nullptr || size == 0) {
      return;
    }
    size_t offset = 0;
    while (offset < size) {
      const auto chunkSize = std::min<size_t>(MAX_VIDEO_PACKET_PAYLOAD, size - offset);
      recordSingleRawChunk(data + offset, chunkSize, captureMs);
      offset += chunkSize;
    }
  }

  void
  recordSingleRawChunk(const uint8_t* data, size_t size, uint64_t captureMs)
  {
    if (!isRecording() || data == nullptr || size == 0) {
      return;
    }
    if (m_cameraOptions.recordChunkLimit != 0 &&
        m_recordingChunks.load() >= m_cameraOptions.recordChunkLimit) {
      return;
    }
    const auto index = m_recordingChunks.fetch_add(1);
    if (m_cameraOptions.recordChunkLimit != 0 && index >= m_cameraOptions.recordChunkLimit) {
      return;
    }

    const auto objectName = m_cameraOptions.recordObjectPrefix + "/" +
      m_recordingSessionId + "/chunk/" + std::to_string(index);
    try {
      auto payload = encryptRecordingChunk(objectName, data, size);
      m_recordingRepo->put(objectName,
                           payload,
                           "video/h264-chunk+hybrid-aes-256-gcm",
                           1,
                           "capture_ms=" + std::to_string(captureMs) +
                             ";encryption=hybrid-aes-256-gcm-at-rest" +
                             ";key_id=" + m_recordingKeyId,
                           {droneIdentity(m_config, m_droneId).toUri()});
      m_recordingBytes += static_cast<uint64_t>(size);
      m_recordingLastUpdateMs = nowMilliseconds();
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("CAMERA_RECORD_CHUNK_FAILED object=" << objectName
                   << " reason=" << e.what());
    }
  }

  ndn::Buffer
  loadOrCreateRecordingContentKey() const
  {
    if (m_cameraOptions.recordRepoPath.empty()) {
      return randomRecordingContentKey();
    }

    const auto keyPath = m_cameraOptions.recordRepoPath + ".content-key";
    {
      std::ifstream input(keyPath, std::ios::binary);
      if (input) {
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        if (bytes.size() == ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE) {
          return ndn::Buffer(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        }
        NDN_LOG_WARN("CAMERA_RECORDING_KEY_INVALID path=" << keyPath
                     << " bytes=" << bytes.size()
                     << " regenerating=true");
      }
    }

    auto key = randomRecordingContentKey();
    {
      std::ofstream output(keyPath, std::ios::binary | std::ios::trunc);
      if (!output) {
        throw std::runtime_error("cannot create camera recording content key: " + keyPath);
      }
      output.write(reinterpret_cast<const char*>(key.data()), key.size());
    }
    chmod(keyPath.c_str(), S_IRUSR | S_IWUSR);
    return key;
  }

  static ndn::Buffer
  randomRecordingContentKey()
  {
    ndn::Buffer key(ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE);
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
      throw std::runtime_error("RAND_bytes failed for camera recording content key");
    }
    return key;
  }

  ndn::Buffer
  recordingAssociatedData(const std::string& objectName) const
  {
    const auto value = "ndnsf-uav-recording|" + droneIdentity(m_config, m_droneId).toUri() +
      "|" + m_recordingSessionId + "|" + objectName;
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

  std::vector<uint8_t>
  encryptRecordingChunk(const std::string& objectName, const uint8_t* data, size_t size) const
  {
    if (m_recordingContentKey.empty()) {
      throw std::runtime_error("camera recording content key is not initialized");
    }
    const auto ad = recordingAssociatedData(objectName);
    const auto encrypted = ndn_service_framework::hybridAesGcmEncrypt(
      m_recordingContentKey,
      ndn::span<const uint8_t>(data, size),
      ndn::span<const uint8_t>(ad.data(), ad.size()));

    ndn_service_framework::HybridMessageEnvelope envelope;
    envelope.setAlgorithm("AES-256-GCM");
    envelope.setKeyId(m_recordingKeyId);
    envelope.setEpochId("camera-recording-persistent-v1");
    envelope.setMessageType("uav-camera-recording-chunk");
    envelope.setNonce(encrypted.nonce);
    envelope.setCipherText(encrypted.ciphertext);
    envelope.setAuthTag(encrypted.tag);
    const auto wire = envelope.WireEncode();
    return std::vector<uint8_t>(wire.data(), wire.data() + wire.size());
  }

  std::string
  recordingContentKeyHex() const
  {
    return hexEncode(std::vector<uint8_t>(m_recordingContentKey.begin(),
                                          m_recordingContentKey.end()));
  }

  std::vector<uint8_t>
  decryptRecordingChunk(const std::string& objectName,
                        const std::vector<uint8_t>& encryptedPayload) const
  {
    if (encryptedPayload.empty()) {
      return {};
    }

    auto [ok, block] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(encryptedPayload.data(), encryptedPayload.size()));
    ndn_service_framework::HybridMessageEnvelope envelope;
    if (!ok || !envelope.WireDecode(block)) {
      NDN_LOG_WARN("CAMERA_RECORDING_LEGACY_PLAINTEXT object=" << objectName);
      return encryptedPayload;
    }
    if (envelope.getKeyId() != m_recordingKeyId ||
        envelope.getMessageType() != "uav-camera-recording-chunk") {
      NDN_LOG_WARN("CAMERA_RECORDING_DECRYPT_REJECT object=" << objectName
                   << " key_id=" << envelope.getKeyId()
                   << " message_type=" << envelope.getMessageType());
      return {};
    }

    const auto ad = recordingAssociatedData(objectName);
    ndn::Buffer plaintext;
    if (!ndn_service_framework::hybridAesGcmDecrypt(
          m_recordingContentKey,
          envelope,
          ndn::span<const uint8_t>(ad.data(), ad.size()),
          plaintext)) {
      NDN_LOG_WARN("CAMERA_RECORDING_DECRYPT_FAILED object=" << objectName);
      return {};
    }
    return std::vector<uint8_t>(plaintext.begin(), plaintext.end());
  }

  void
  publishCurrentFrame(uint64_t captureMs)
  {
    const auto dataShardCount = m_fecPendingChunks.size();
    if (dataShardCount == 0 || !m_streaming.load()) {
      return;
    }

    const auto frameSeq = m_nextFecFrameSeq++;
    const auto second = captureMs / 1000;
    auto dataChunks = std::move(m_fecPendingChunks);
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;

    std::vector<size_t> dataLengths;
    dataLengths.reserve(dataChunks.size());
    size_t maxPayloadSize = 0;
    for (const auto& payload : dataChunks) {
      dataLengths.push_back(payload.size());
      maxPayloadSize = std::max(maxPayloadSize, payload.size());
    }

    std::vector<uint8_t> parityPayload(maxPayloadSize, 0);
    for (const auto& payload : dataChunks) {
      for (size_t i = 0; i < payload.size(); ++i) {
        parityPayload[i] ^= payload[i];
      }
    }

    const auto firstPacketSeq = allocatePacketRange(dataShardCount + m_fecParityShards);
    const auto frameLastPacketSeq = firstPacketSeq + dataShardCount + m_fecParityShards - 1;
    const auto dataLengthsCsv = joinFecLengths(dataLengths);
    m_nextSeq += static_cast<uint64_t>(dataShardCount + m_fecParityShards);

    for (uint64_t i = 0; i < dataShardCount; ++i) {
      VideoPacket packet;
      packet.streamId = m_streamId;
      packet.streamSessionEpoch = m_streamSessionEpoch;
      packet.second = second;
      packet.packetSeq = firstPacketSeq + i;
      packet.frameSeq = frameSeq;
      packet.captureMs = captureMs;
      packet.frameFirstPacketSeq = firstPacketSeq;
      packet.frameLastPacketSeq = frameLastPacketSeq;
      packet.bucketPacketCount = frameLastPacketSeq + 1;
      packet.frameSegmentIndex = static_cast<uint32_t>(i);
      packet.frameSegmentCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.keyFrame = ((frameSeq % 30) == 0);
      packet.encoding = "video/h264";
      packet.fecDataShards = static_cast<uint32_t>(dataShardCount);
      packet.fecParityShards = static_cast<uint32_t>(m_fecParityShards);
      packet.fecSymbolIndex = static_cast<uint32_t>(i);
      packet.fecSymbolCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.fecDataLengths = dataLengthsCsv;
      packet.payload = std::move(dataChunks[i]);

      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packet.packetSeq));
      rememberPacket(packetName, encodeVideoPacket(packet));
    }

    for (uint64_t i = 0; i < m_fecParityShards; ++i) {
      const auto symbolIndex = dataShardCount + i;
      VideoPacket packet;
      packet.streamId = m_streamId;
      packet.streamSessionEpoch = m_streamSessionEpoch;
      packet.second = second;
      packet.packetSeq = firstPacketSeq + symbolIndex;
      packet.frameSeq = frameSeq;
      packet.captureMs = captureMs;
      packet.frameFirstPacketSeq = firstPacketSeq;
      packet.frameLastPacketSeq = frameLastPacketSeq;
      packet.bucketPacketCount = frameLastPacketSeq + 1;
      packet.frameSegmentIndex = static_cast<uint32_t>(symbolIndex);
      packet.frameSegmentCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.keyFrame = false;
      packet.encoding = "video/h264";
      packet.fecDataShards = static_cast<uint32_t>(dataShardCount);
      packet.fecParityShards = static_cast<uint32_t>(m_fecParityShards);
      packet.fecSymbolIndex = static_cast<uint32_t>(symbolIndex);
      packet.fecSymbolCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.fecDataLengths = dataLengthsCsv;
      packet.payload = parityPayload;

      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packet.packetSeq));
      rememberPacket(packetName, encodeVideoPacket(packet));
    }
  }

  uint64_t
  allocatePacketRange(uint64_t count)
  {
    const auto first = m_nextPacketSeq.fetch_add(count);
    return first;
  }

  void
  captureLoop()
  {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(nullptr, pclose);
    std::vector<uint8_t> chunkBuffer;
    while (!m_done.load()) {
      if (!m_captureEnabled.load()) {
        pipe.reset();
        {
          std::lock_guard<std::mutex> guard(m_mutex);
          m_jpegBuffer.clear();
        }
        chunkBuffer.clear();
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
      const auto n = fread(buffer.data(), 1, buffer.size(), pipe.get());
      if (n == 0) {
        pipe.reset();
        continue;
      }

      const auto captureMs = nowMilliseconds();
      recordRawChunk(buffer.data(), n, captureMs);
      if (!m_streaming.load()) {
        chunkBuffer.clear();
        continue;
      }

      chunkBuffer.insert(chunkBuffer.end(), buffer.begin(), buffer.begin() + n);
      while (chunkBuffer.size() >= MAX_VIDEO_PACKET_PAYLOAD) {
        const auto chunkSize = std::min(MAX_VIDEO_PACKET_PAYLOAD, chunkBuffer.size());
        std::vector<uint8_t> packetBytes(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        chunkBuffer.erase(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        appendStreamChunk(std::move(packetBytes), captureMs);
      }
      if (!m_fecPendingChunks.empty() &&
          m_fecCurrentFrameStartMs != 0 &&
          nowMilliseconds() >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs) {
        publishCurrentFrame(nowMilliseconds());
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

  static constexpr size_t MAX_VIDEO_PACKET_PAYLOAD = 3600;
  static constexpr uint64_t MIN_VIDEO_BITRATE_KBPS = 256;
  static constexpr uint64_t MAX_VIDEO_BITRATE_KBPS = 16000;
  static constexpr uint64_t MIN_VIDEO_FRAME_WIDTH = 160;
  static constexpr uint64_t MAX_VIDEO_FRAME_WIDTH = 1280;
  ndn::Face& m_face;
  ndn::KeyChain& m_keyChain;
  ndn_service_framework::LocalServiceRegistry& m_localRegistry;
  ndn::Name m_localRecordingChunkServiceName;
  UavRuntimeConfig m_config;
  std::string m_droneId;
  std::string m_videoPath;
  CameraRuntimeOptions m_cameraOptions;
  std::unique_ptr<ndnsf_distributed_repo::RepoCore> m_recordingRepo;
  std::string m_recordingSessionId = "record-idle";
  std::string m_recordingKeyId;
  ndn::Buffer m_recordingContentKey;
  mutable std::mutex m_mutex;
  std::mutex m_signMutex;
  ndn::Name m_videoPrefix;
  ndn::Name m_streamPrefix;
  std::string m_streamId = "idle";
  uint64_t m_streamSessionEpoch = 0;
  bool m_filterRegistered = false;
  bool m_recordingFilterRegistered = false;
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_captureEnabled{false};
  std::atomic<bool> m_cameraCaptureUsable{true};
  std::atomic<uint64_t> m_cameraProbeFailures{0};
  std::atomic<bool> m_done{false};
  std::atomic<uint64_t> m_nextSeq{0};
  std::atomic<uint64_t> m_nextPacketSeq{0};
  std::atomic<uint64_t> m_nextFecFrameSeq{0};
  std::atomic<uint64_t> m_frameInterests{0};
  std::atomic<uint64_t> m_framePuts{0};
  std::atomic<uint64_t> m_recordingChunks{0};
  std::atomic<uint64_t> m_recordingBytes{0};
  std::atomic<uint64_t> m_recordingLastUpdateMs{0};
  std::atomic<uint64_t> m_targetFps{30};
  std::atomic<uint64_t> m_requestedBitrateKbps{8000};
  std::atomic<uint64_t> m_acceptedBitrateKbps{8000};
  std::atomic<uint64_t> m_requestedFrameWidth{480};
  std::atomic<uint64_t> m_acceptedFrameWidth{480};
  std::atomic<uint64_t> m_encoderQuality{6};
  std::atomic<bool> m_restartEncoder{false};
  std::map<std::string, std::vector<uint8_t>> m_packets;
  std::deque<std::string> m_order;
  std::map<std::string, ndn::Name> m_pending;
  std::vector<uint8_t> m_jpegBuffer;
  std::vector<std::vector<uint8_t>> m_fecPendingChunks;
  uint64_t m_fecCurrentFrameStartMs = 0;
  uint64_t m_fecDataShards = 8;
  uint64_t m_fecParityShards = 1;
  static constexpr uint64_t m_fecFrameTimeoutMs = 35;
  std::thread m_captureThread;
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
    explicit RecordingManifestLocalHelper(std::function<Fields()> manifestProvider)
      : m_manifestProvider(std::move(manifestProvider))
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
          return makeResponse(true, encodeFields(m_manifestProvider()));
        });
    }

  private:
    std::function<Fields()> m_manifestProvider;
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
          m_face, m_keyChain, m_coreContainer.localRegistry(), localRecordingChunkServiceName(),
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
        {"recording_chunk_limit", std::to_string(m_cameraOptions.recordChunkLimit)},
        {"timestamp_ms", std::to_string(nowMilliseconds())},
      };
    }
    return m_videoPublisher->repoStatusFields();
  }

  std::vector<uint8_t>
  recordingChunk(const std::string& objectName) const
  {
    std::lock_guard<std::mutex> guard(m_containerMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->recordingChunk(objectName)
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
        [this] { return recordingManifestFields(); });
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
            {
              std::lock_guard<std::mutex> guard(m_containerMutex);
              if (!m_videoPublisher->isStreaming()) {
                const auto responseFields = m_videoPublisher->stopWithReason("already-stopped");
                publishStatus("video already stopped");
                return makeResponse(true, encodeFields(responseFields));
              }
              const auto responseFields = m_videoPublisher->stop();
              stopObjectDetectionLoop();
              publishStatus("video stopped");
              return makeResponse(true, encodeFields(responseFields));
            }
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
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            localRecordingManifestServiceName(), request, response, m_identity);
          return response;
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
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(telemetryHandler),
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
  localRecordingChunkServiceName() const
  {
    return ndn::Name(m_identity).append("Local").append("Recording").append("Chunk");
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
