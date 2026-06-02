// App-internal implementation chunk included by UavGroundStationApp.cpp.
// Keeps the NDNSF service container separate from the GTK window for review.

class GroundStationServiceContainer
{
public:
  GroundStationServiceContainer(bool serveCertificates, int ackTimeoutMs, int timeoutMs,
                       UavRuntimeConfig config,
                       std::string targetDroneId, uint64_t videoBitrateKbps,
                       uint64_t videoFrameWidth,
                       std::vector<std::string> patrolDroneIds = {},
                       std::string yoloModel = "yolo26n.pt",
                       std::string yoloScript = "NDNSF-UAV-APP/tools/yolo_detect_once.py",
                       std::string yoloWorkerScript = "NDNSF-UAV-APP/tools/yolo_detect_worker.py",
                       uint64_t linkStaleMs = 3500,
                       uint64_t linkLostMs = 8000,
                       std::string lostLinkAction = "notify",
                       std::string videoBitratePolicy = "manual",
                       uint64_t videoBitrateAutoPressureMs = 2500)
    : m_serveCertificates(serveCertificates)
    , m_config(std::move(config))
    , m_ackTimeoutMs(ackTimeoutMs)
    , m_timeoutMs(timeoutMs)
    , m_targetDroneId(std::move(targetDroneId))
    , m_videoBitrateKbps(videoBitrateKbps)
    , m_videoFrameWidth(videoFrameWidth)
    , m_patrolDroneIds(std::move(patrolDroneIds))
    , m_yoloModel(std::move(yoloModel))
    , m_yoloScript(std::move(yoloScript))
    , m_yoloWorkerScript(std::move(yoloWorkerScript))
    , m_linkStaleMs(linkStaleMs)
    , m_linkLostMs(std::max(linkLostMs, linkStaleMs))
    , m_lostLinkAction(std::move(lostLinkAction))
    , m_videoBitratePolicy(std::move(videoBitratePolicy))
    , m_videoBitrateAutoPressureMs(videoBitrateAutoPressureMs == 0 ?
                                   0 : std::max<uint64_t>(500, videoBitrateAutoPressureMs))
    , m_videoPumpTimer(m_face.getIoContext())
  {
    if (m_patrolDroneIds.empty()) {
      m_patrolDroneIds.push_back(m_targetDroneId);
    }
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_gsCert = getOrCreateIdentity(m_keyChain, m_config.groundStationIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_config.controllerPrefix);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_config.groundStationIdentity));
    m_videoRequestedBitrateKbps = std::max<uint64_t>(128, m_videoBitrateKbps.load());
    m_videoAcceptedBitrateKbps = std::max<uint64_t>(128, m_videoBitrateKbps.load());
  }

  ~GroundStationServiceContainer()
  {
    shutdownRuntime();
  }

  void
  shutdownRuntime()
  {
    if (m_done.exchange(true)) {
      return;
    }
    m_streaming = false;
    m_recordingPlaybackActive = false;
    if (m_yoloPrewarmThread.joinable()) {
      m_yoloPrewarmThread.join();
    }
    stopYoloWorker();
    stopDecoder();
    if (m_recordingPlaybackDecodeThread.joinable()) {
      m_recordingPlaybackDecodeThread.join();
    }
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
            m_face, m_keyChain, m_gsCert.getName());
        }
        m_user = std::make_unique<ndn_service_framework::ServiceUser>(
          m_face, m_config.groupPrefix, m_gsCert, m_controllerCert, m_config.trustSchema);
        m_user->setHandlerThreads(2);
        m_user->init();
        m_user->fetchPermissionsFromController(m_config.controllerPrefix);
        installServiceInstances();
        m_objectDetectionProvider->init();
        m_objectDetectionProvider->fetchPermissionsFromController(m_config.controllerPrefix);
        m_containerReady = true;
        publishStatus("NDNSF runtime ready");
        m_yoloPrewarmThread = std::thread([this] {
          std::lock_guard<std::mutex> guard(m_yoloMutex);
          startYoloWorkerLocked();
        });

        while (!m_done.load()) {
          m_face.getIoContext().run_for(std::chrono::milliseconds(10));
          m_face.getIoContext().restart();
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

  void
  setStatusCallback(std::function<void(std::string)> callback)
  {
    m_statusCallback = std::move(callback);
  }

  void
  setFrameCallback(std::function<void(std::vector<uint8_t>, uint64_t, uint64_t)> callback)
  {
    m_frameCallback = std::move(callback);
  }

  std::string
  targetDroneId() const
  {
    std::lock_guard<std::mutex> guard(m_targetMutex);
    return m_targetDroneId;
  }

  void
  setTargetDroneId(std::string droneId)
  {
    if (droneId.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> guard(m_targetMutex);
      if (m_targetDroneId == droneId) {
        return;
      }
      m_targetDroneId = std::move(droneId);
    }
    publishStatus("Selected drone " + targetDroneId());
  }

  std::vector<std::string>
  missionReadyDrones() const
  {
    std::lock_guard<std::mutex> guard(m_missionReadyMutex);
    return m_missionReadyDrones;
  }

  std::vector<std::string>
  missionStartableDrones() const
  {
    std::vector<std::string> candidates;
    {
      std::lock_guard<std::mutex> guard(m_missionReadyMutex);
      candidates = m_missionReadyDrones;
    }

    std::vector<std::string> out;
    std::lock_guard<std::mutex> telemetryGuard(m_telemetryMutex);
    for (const auto& droneId : candidates) {
      const auto found = m_missionByDrone.find(droneId);
      if (found != m_missionByDrone.end() && found->second.isStartable()) {
        out.push_back(droneId);
      }
    }
    return out;
  }

  void
  injectMissionStateForTest(MissionState mission)
  {
    if (mission.updatedMs == 0) {
      mission.updatedMs = nowMilliseconds();
    }
    updateMissionState(mission);

    std::lock_guard<std::mutex> readyGuard(m_missionReadyMutex);
    auto found = std::find(m_missionReadyDrones.begin(), m_missionReadyDrones.end(),
                           mission.droneId);
    if (mission.isStartable()) {
      if (found == m_missionReadyDrones.end()) {
        m_missionReadyDrones.push_back(mission.droneId);
      }
    }
    else if (found != m_missionReadyDrones.end()) {
      m_missionReadyDrones.erase(found);
    }
  }

  void
  injectMissionProgressForTest(MissionProgressState progress)
  {
    if (progress.taskId.empty() || progress.taskId == "none") {
      progress.taskId = "mission-progress-test";
    }
    updateMissionProgress(std::move(progress));
  }

  std::string
  serviceCatalogForDrone(const std::string& droneId) const
  {
    std::ostringstream os;
    os << "Services for Drone " << droneId << ":\n";
    os << "video-control normal-only "
       << droneVideoControlService(m_config, droneId).toUri() << "\n";
    os << "recording-manifest normal-only "
       << droneCameraRecordingManifestService(m_config, droneId).toUri() << "\n";
    os << "recording-chunk helper encrypted-ndn-data "
       << droneIdentity(m_config, droneId).append("repo").append("camera").append("recording").toUri()
       << "\n";
    os << "mavlink-execute targeted-only "
       << m_config.serviceMavlinkExecute.toUri() << "\n";
    os << "telemetry normal-and-targeted "
       << m_config.serviceTelemetryStatus.toUri() << "\n";
    os << "camera-frame normal-only "
       << m_config.serviceCameraFrame.toUri() << "\n";
    os << "mission normal-only "
       << m_config.serviceMissionAssign.toUri() << "\n";
    os << "gs-object-detection normal-only "
       << m_config.serviceGsObjectDetection.toUri();
    return os.str();
  }

  void
  logServiceCatalogForDrone(const std::string& droneId) const
  {
    std::istringstream lines(serviceCatalogForDrone(droneId));
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty()) {
        NDN_LOG_INFO("GS_SERVICE_CATALOG " << line);
      }
    }
  }

  void
  requestTelemetryStatus()
  {
    requestTelemetryStatusForDrone(targetDroneId());
  }

  void
  requestTelemetryStatusForDrone(const std::string& droneId)
  {
    {
      std::lock_guard<std::mutex> guard(m_telemetryMutex);
      if (m_telemetryInFlightDrones.find(droneId) != m_telemetryInFlightDrones.end()) {
        return;
      }
      m_telemetryInFlightDrones.insert(droneId);
    }
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceTelemetryStatus,
      encodeFields({{"type", "telemetry-status"}, {"target_drone", droneId}}),
      [this, droneId](const std::string& payload) {
        clearTelemetryInFlight(droneId);
        const auto fields = decodeFields(payload);
        auto telemetry = TelemetryState::fromFields(fields);
        if (telemetry.droneId == "unknown") {
          telemetry.droneId = droneId;
        }
        const auto mission = MissionState::fromFields(fields);
        const auto readiness = ReadinessState::fromTelemetry(telemetry);
        const auto video = VideoState::fromFields(fields);
        updateDroneState(telemetry, mission);
        publishStatus(telemetry.statusLine() +
                      " " + readiness.statusLine() +
                      " mission=" + mission.phase +
                      " mission_detail=" + mission.detail +
                      " " + video.statusLine());
      },
      [this, droneId] {
        clearTelemetryInFlight(droneId);
        publishStatus("Telemetry timeout for drone " + droneId);
      });
  }

  std::optional<TelemetryState>
  telemetryForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_telemetryByDrone.find(droneId);
    if (found == m_telemetryByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<MissionState>
  missionForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_missionByDrone.find(droneId);
    if (found == m_missionByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<MissionProgressState>
  missionProgressSnapshot() const
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    if (m_latestMissionProgress.taskId == "none") {
      return std::nullopt;
    }
    return m_latestMissionProgress;
  }

  std::optional<MissionPlan>
  missionPlanSnapshot() const
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    if (m_latestMissionPlan.taskId.empty()) {
      return std::nullopt;
    }
    return m_latestMissionPlan;
  }

  std::optional<MissionPart>
  missionPartForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    if (m_latestMissionPlan.taskId.empty()) {
      return std::nullopt;
    }
    for (const auto& part : m_latestMissionPlan.parts) {
      if (part.assignedDrone == droneId) {
        return part;
      }
    }
    return std::nullopt;
  }

  std::optional<ReadinessState>
  readinessForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_readinessByDrone.find(droneId);
    if (found == m_readinessByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void
  injectReadinessStateForTest(ReadinessState readiness)
  {
    if (readiness.timestampMs == 0) {
      readiness.timestampMs = nowMilliseconds();
    }
    if (readiness.droneId.empty() || readiness.droneId == "unknown") {
      return;
    }
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    m_readinessByDrone[readiness.droneId] = std::move(readiness);
  }

  std::optional<VideoState>
  videoForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_videoByDrone.find(droneId);
    if (found == m_videoByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<VideoAdaptiveState>
  videoAdaptiveForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_videoAdaptiveByDrone.find(droneId);
    if (found == m_videoAdaptiveByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void
  injectVideoAdaptivePressureForTest(const std::string& profile,
                                     uint64_t timeoutPressure,
                                     uint64_t probePressure,
                                     uint64_t duplicatePressure,
                                     uint64_t decoderPendingChunks,
                                     uint64_t receivedChunks,
                                     uint64_t timeouts,
                                     uint64_t nacks)
  {
    m_videoTimeoutPressurePercent = std::clamp<uint64_t>(timeoutPressure, 0, 100);
    m_videoProbePressurePercent = std::clamp<uint64_t>(probePressure, 0, 100);
    m_videoDuplicatePressurePercent = std::clamp<uint64_t>(duplicatePressure, 0, 100);
    m_decoderPendingChunkCount = decoderPendingChunks;
    m_receivedChunks = receivedChunks;
    m_frameTimeouts = timeouts;
    m_frameNacks = nacks;
    publishVideoAdaptiveState("pressure-profile-" + profile, true);
  }

  std::optional<FlightCommandState>
  commandForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    const auto found = m_commandByDrone.find(droneId);
    if (found == m_commandByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<SafetyState>
  safetyForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    auto found = m_safetyByDrone.find(droneId);
    SafetyState state;
    if (found != m_safetyByDrone.end()) {
      state = found->second;
    }
    else {
      const auto telemetry = m_telemetryByDrone.find(droneId);
      if (telemetry == m_telemetryByDrone.end()) {
        return std::nullopt;
      }
      state = SafetyState::fromTelemetry(telemetry->second);
      state.droneId = droneId;
    }
    if (!ageSafetyStateLocked(droneId, state)) {
      return std::nullopt;
    }
    return state;
  }

  std::vector<TelemetryState>
  telemetrySnapshots() const
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    std::vector<TelemetryState> out;
    out.reserve(m_telemetryByDrone.size());
    for (const auto& item : m_telemetryByDrone) {
      out.push_back(item.second);
    }
    return out;
  }

  void
  startVideo()
  {
    const auto droneId = targetDroneId();
    if (m_streaming.load()) {
      const auto activeDrone = activeVideoDroneId();
      publishStatus("Video already streaming drone=" +
                    (activeDrone.empty() ? std::string("unknown") : activeDrone));
      return;
    }
    if (m_videoStartInFlight.exchange(true)) {
      publishStatus("Video start already pending");
      return;
    }
    m_seenVideoStart = false;
    m_videoStartRetries = 0;
    m_videoStopDelayInjected = false;
    startVideoAttempt(droneId);
  }

  bool
  applySuggestedVideoBitrate()
  {
    const auto droneId = targetDroneId();
    const auto adaptive = videoAdaptiveForDrone(droneId);
    if (!adaptive) {
      publishStatus("No video adaptive state for selected drone " + droneId);
      return false;
    }
    if (!isStreamingForDrone(droneId)) {
      m_videoBitrateKbps = std::max<uint64_t>(128, adaptive->suggestedBitrateKbps);
      publishStatus("Next video start bitrate drone=" + droneId +
                    " requested_kbps=" + std::to_string(m_videoBitrateKbps.load()));
      return false;
    }
    if (adaptive->bitrateAction == "hold" ||
        adaptive->suggestedBitrateKbps == 0 ||
        adaptive->suggestedBitrateKbps == adaptive->acceptedBitrateKbps) {
      publishStatus("Video bitrate hold drone=" + droneId +
                    " accepted_kbps=" + std::to_string(adaptive->acceptedBitrateKbps) +
                    " reason=" + adaptive->bitrateReason);
      return false;
    }
    return restartVideoWithBitrate(droneId,
                                   adaptive->suggestedBitrateKbps,
                                   adaptive->acceptedBitrateKbps,
                                   adaptive->bitrateAction,
                                   adaptive->bitrateReason);
  }

  std::string
  videoBitratePolicy() const
  {
    return m_videoBitratePolicy;
  }

  void
  stopVideo()
  {
    const auto droneId = targetDroneId();
    if (m_recordingPlaybackActive.load() && activeRecordingPlaybackDroneId() == droneId) {
      m_recordingPlaybackActive = false;
      stopDecoder();
      {
        std::lock_guard<std::mutex> guard(m_videoStateMutex);
        m_recordingPlaybackDroneId.clear();
      }
      publishStatus("Recording playback stopped drone=" + droneId);
      return;
    }
    const auto activeDrone = activeVideoDroneId();
    if (activeDrone != droneId) {
      publishStatus("No video streaming for selected drone " + droneId);
      return;
    }
    if (m_videoStopInFlight.exchange(true)) {
      publishStatus("Video stop already pending");
      return;
    }
    m_videoStartInFlight = false;
    m_streaming = false;
    m_videoPumpScheduled = false;
    boost::system::error_code ec;
    m_videoPumpTimer.cancel(ec);
    publishVideoAdaptiveState("stop-requested", true);
    stopDecoder();
    stopVideoAttempt(droneId);
  }

  bool
  isStreaming() const
  {
    return m_streaming.load();
  }

  bool
  isStreamingForDrone(const std::string& droneId) const
  {
    return m_streaming.load() && activeVideoDroneId() == droneId;
  }

  bool
  isVideoDisplayActiveForDrone(const std::string& droneId) const
  {
    return isStreamingForDrone(droneId) ||
           (m_recordingPlaybackActive.load() && activeRecordingPlaybackDroneId() == droneId);
  }

  std::string
  activeVideoDroneId() const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    return m_activeVideoDroneId;
  }

  std::string
  activeRecordingPlaybackDroneId() const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    return m_recordingPlaybackDroneId;
  }

  void
  requestRecordingManifest()
  {
    requestRecordingManifestForDrone(targetDroneId(), false);
  }

  void
  playLatestRecording()
  {
    requestRecordingManifestForDrone(targetDroneId(), true);
  }

  bool
  sendMavlinkCommand(const std::string& commandName, Fields params = {})
  {
    return sendMavlinkCommandToDrone(targetDroneId(), commandName, std::move(params));
  }

  bool
  sendMavlinkCommandToDrone(const std::string& droneId, const std::string& commandName, Fields params = {})
  {
    const bool isManualControl = commandName == "manual_control";
    const bool isEmergencyStop = commandName == "emergency_stop";
    if (commandName == "arm") {
      std::string reason;
      if (!validateArmReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Arm blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    if (isManualControl) {
      std::string reason;
      if (!validateManualControlReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        const auto now = nowMilliseconds();
        if (now > m_lastManualControlBlockedLogMs.load() + 1000) {
          m_lastManualControlBlockedLogMs = now;
          publishStatus("Manual control blocked drone=" + droneId + " reason=" + reason);
        }
        return false;
      }
    }
    if (commandName == "land") {
      std::string reason;
      if (!validateLandReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Land blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    if (commandName == "takeoff") {
      std::string reason;
      if (!validateTakeoffReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Takeoff blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    auto& inFlight = mavlinkInFlightFlag(isManualControl, isEmergencyStop);
    if (inFlight.exchange(true)) {
      recordBlockedCommand(droneId, commandName, "command-in-flight");
      if (!isManualControl) {
        publishStatus("MAVLink command busy; dropped " + commandName);
      }
      return false;
    }
    params["target_drone"] = droneId;
    params.emplace("target_system", mavlinkTargetSystemForDrone(droneId));
    params.emplace("target_component", "1");
    const auto missionId = "manual-" + commandName + "-" + std::to_string(nowMilliseconds());
    const auto payload = makeMavlinkCommandPayload(commandName, missionId, params);
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceMavlinkExecute,
      payload,
      [this, commandName, isManualControl, isEmergencyStop, droneId](const std::string& responsePayload) {
        mavlinkInFlightFlag(isManualControl, isEmergencyStop) = false;
        const auto fields = decodeFields(responsePayload);
        auto commandState = FlightCommandState::fromFields(fields);
        commandState.droneId = droneId;
        commandState.command = commandName;
        commandState.updatedMs = nowMilliseconds();
        if (commandState.detail == "idle") {
          commandState.detail = commandState.isAccepted() ? "response-accepted" : "response-rejected";
        }
        updateCommandState(commandState);
        const auto accepted = fieldOr(fields, "accepted", "false");
        const auto bytes = fieldOr(fields, "forwarded_bytes", "0");
        const auto ackResult = fieldOr(fields, "ack_result", "unknown");
        const auto fcState = fieldOr(fields, "fc_state", "");
        const auto altitude = fieldOr(fields, "altitude_m", "");
        const auto speed = fieldOr(fields, "groundspeed_mps", "");
        const auto battery = fieldOr(fields, "battery_percent", "");
        publishStatus("MAVLink " + commandName +
                      " drone=" + droneId +
                      " accepted=" + accepted +
                      " ack=" + ackResult +
                      " forwarded_bytes=" + bytes +
                      (fcState.empty() ? "" : " state=" + fcState) +
                      (altitude.empty() ? "" : " alt=" + altitude + "m") +
                      (speed.empty() ? "" : " speed=" + speed + "m/s") +
                      (battery.empty() ? "" : " battery=" + battery + "%"));
      },
      [this, commandName, isManualControl, isEmergencyStop, droneId] {
        mavlinkInFlightFlag(isManualControl, isEmergencyStop) = false;
        updateCommandState(FlightCommandState{
          droneId,
          commandName,
          "false",
          "timeout",
          "unknown",
          "unknown",
          "unknown",
          "unknown",
          "0",
          "targeted-request-timeout",
          nowMilliseconds(),
        });
        publishStatus("MAVLink " + commandName +
                      " drone=" + droneId +
                      " accepted=false ack=timeout forwarded_bytes=0 detail=targeted-request-timeout");
      });
    return true;
  }

  bool
  sendMavlinkCommandToDroneSync(const std::string& droneId, const std::string& commandName,
                                Fields params, std::chrono::milliseconds timeout)
  {
    if (commandName == "arm") {
      std::string reason;
      if (!validateArmReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        NDN_LOG_INFO("SINGLE_MISSION_COMMAND command=" << commandName
                     << " ok=false ack=arm-blocked reason=" << reason);
        return false;
      }
    }
    if (commandName == "takeoff") {
      std::string reason;
      if (!validateTakeoffReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        NDN_LOG_INFO("SINGLE_MISSION_COMMAND command=" << commandName
                     << " ok=false ack=takeoff-blocked reason=" << reason);
        return false;
      }
    }
    if (commandName == "land") {
      std::string reason;
      if (!validateLandReadiness(droneId, reason)) {
        recordBlockedCommand(droneId, commandName, reason);
        NDN_LOG_INFO("SINGLE_MISSION_COMMAND command=" << commandName
                     << " ok=false ack=land-blocked reason=" << reason);
        return false;
      }
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string ackResult = "unknown";
    params["target_drone"] = droneId;
    params.emplace("target_system", mavlinkTargetSystemForDrone(droneId));
    params.emplace("target_component", "1");
    const auto missionId = "auto-" + commandName + "-" + std::to_string(nowMilliseconds());
    const auto payload = makeMavlinkCommandPayload(commandName, missionId, params);
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceMavlinkExecute,
      payload,
      [&](const std::string& responsePayload) {
        const auto fields = decodeFields(responsePayload);
        ackResult = fieldOr(fields, "ack_result", "unknown");
        const auto accepted = fieldOr(fields, "accepted", "false");
        auto commandState = FlightCommandState::fromFields(fields);
        commandState.droneId = droneId;
        commandState.command = commandName;
        commandState.updatedMs = nowMilliseconds();
        if (commandState.detail == "idle") {
          commandState.detail = accepted == "true" ? "sync-response-accepted" : "sync-response-rejected";
        }
        updateCommandState(commandState);
        {
          std::lock_guard<std::mutex> guard(mutex);
          ok = accepted == "true" &&
               (ackResult == "accepted" || ackResult.rfind("mock", 0) == 0);
          done = true;
        }
        cv.notify_all();
      },
      [&] {
        updateCommandState(FlightCommandState{
          droneId,
          commandName,
          "false",
          "timeout",
          "unknown",
          "unknown",
          "unknown",
          "unknown",
          "0",
          "sync-targeted-request-timeout",
          nowMilliseconds(),
        });
        std::lock_guard<std::mutex> guard(mutex);
        ackResult = "timeout";
        done = true;
        ok = false;
        cv.notify_all();
      });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [&] { return done; });
    NDN_LOG_INFO("SINGLE_MISSION_COMMAND command=" << commandName
                 << " ok=" << (done && ok ? "true" : "false")
                 << " ack=" << ackResult);
    return done && ok;
  }

  Fields
  requestTelemetryStatusForDroneSync(const std::string& droneId,
                                     std::chrono::milliseconds timeout)
  {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    Fields out;
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceTelemetryStatus,
      encodeFields({{"type", "telemetry-status"}, {"target_drone", droneId}}),
      [&](const std::string& payload) {
        {
          std::lock_guard<std::mutex> guard(mutex);
          out = decodeFields(payload);
          done = true;
        }
        cv.notify_all();
      },
      [&] {
        std::lock_guard<std::mutex> guard(mutex);
        done = true;
        cv.notify_all();
      });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [&] { return done; });
    if (!out.empty()) {
      auto telemetry = TelemetryState::fromFields(out);
      if (telemetry.droneId == "unknown") {
        telemetry.droneId = droneId;
      }
      updateDroneState(telemetry, MissionState::fromFields(out));
    }
    return out;
  }

  bool
  runTelemetryLiveTest(std::chrono::seconds timeout, bool requireSensorDetails)
  {
    const auto droneId = targetDroneId();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto commandTimeout = std::chrono::milliseconds(m_timeoutMs);

    struct TelemetryCheck
    {
      bool gpsFix = false;
      bool ekfReady = false;
      bool landedKnown = false;
      bool landedChanged = false;
      bool batteryVoltage = false;
      bool armedTrue = false;
      bool latLon = false;
      std::string firstLanded;
    } check;

    auto sample = [&](const std::string& phase, int index) {
      const auto fields = requestTelemetryStatusForDroneSync(droneId, commandTimeout);
      auto telemetry = TelemetryState::fromFields(fields);
      if (telemetry.droneId == "unknown") {
        telemetry.droneId = droneId;
      }
      const auto mission = MissionState::fromFields(fields);
      updateDroneState(telemetry, mission);
      const auto readiness = readinessForDrone(droneId);
      const auto safety = safetyForDrone(droneId);
      const auto video = videoForDrone(droneId);
      const auto videoAdaptive = videoAdaptiveForDrone(droneId);
      const auto progress = missionProgressSnapshot();
      const auto flight = FlightActionControlState::fromGate(
        FlightSafetyGateState::fromStates(droneId, readiness, safety));
      const auto missionControl = MissionControlState::fromStates({}, progress,
                                                                  false, false, false);
      const auto selectedAction = SelectedActionState::fromStates(droneId, flight,
                                                                  missionControl,
                                                                  false, false);
      const auto summary = SelectedDroneSummaryState::fromStates(droneId, telemetry,
                                                                 readiness, mission,
                                                                 std::nullopt, std::nullopt,
                                                                 progress, video,
                                                                 videoAdaptive, safety);
      const auto row = DroneListRowState::fromStates(droneId, true, telemetry,
                                                     readiness, mission, video,
                                                     videoAdaptive, std::nullopt,
                                                     safety, progress);
      const auto known = [](const std::string& value) {
        return !value.empty() && value != "unknown";
      };
      check.gpsFix = check.gpsFix || known(telemetry.gpsFixName);
      check.ekfReady = check.ekfReady || telemetry.ekfReady == "true";
      check.landedKnown = check.landedKnown || known(telemetry.landedStateName);
      check.batteryVoltage = check.batteryVoltage || known(telemetry.batteryVoltageV);
      check.armedTrue = check.armedTrue || telemetry.armed == "true";
      check.latLon = check.latLon || (known(telemetry.lat) && known(telemetry.lon));
      if (known(telemetry.landedStateName)) {
        if (check.firstLanded.empty()) {
          check.firstLanded = telemetry.landedStateName;
        }
        else if (check.firstLanded != telemetry.landedStateName) {
          check.landedChanged = true;
        }
      }
      NDN_LOG_INFO("TELEMETRY_LIVE sample=" << index
                   << " phase=" << phase
                   << " drone=" << telemetry.droneId
                   << " gps_fix_name=" << telemetry.gpsFixName
                   << " ekf_ready=" << telemetry.ekfReady
                   << " landed_state_name=" << telemetry.landedStateName
                   << " battery_voltage_v=" << telemetry.batteryVoltageV
                   << " armed=" << telemetry.armed
                   << " lat=" << telemetry.lat
                   << " lon=" << telemetry.lon
                   << " readiness=" << telemetry.readiness
                   << " reason=" << telemetry.readinessReason);
      NDN_LOG_INFO("TELEMETRY_STATE_MODEL sample=" << index
                   << " phase=" << phase
                   << " " << flight.statusLine()
                   << " " << selectedAction.statusLine()
                   << " " << summary.statusLine()
                   << " row=" << row.rowText);
    };

    int sampleIndex = 0;
    sample("initial", sampleIndex++);
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    const bool armOk = sendMavlinkCommandToDroneSync(
      droneId, "arm", {{"arm", "true"}}, commandTimeout);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    sample("armed", sampleIndex++);

    const bool takeoffOk = sendMavlinkCommandToDroneSync(
      droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}}, commandTimeout);
    for (int i = 0; i < 4 && std::chrono::steady_clock::now() < deadline; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      sample("takeoff", sampleIndex++);
    }

    const bool landOk = sendMavlinkCommandToDroneSync(droneId, "land", {}, commandTimeout);
    for (int i = 0; i < 4 && std::chrono::steady_clock::now() < deadline; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      sample("land", sampleIndex++);
    }

    const bool sensorDetailsOk = !requireSensorDetails ||
                                 (check.gpsFix && check.batteryVoltage);
    const bool ok = armOk && takeoffOk && landOk &&
                    sensorDetailsOk && check.ekfReady && check.landedKnown &&
                    check.landedChanged &&
                    check.armedTrue && check.latLon;
    NDN_LOG_INFO("TELEMETRY_LIVE_RESULT ok=" << (ok ? "true" : "false")
                 << " require_sensor_details=" << (requireSensorDetails ? "true" : "false")
                 << " arm_ok=" << (armOk ? "true" : "false")
                 << " takeoff_ok=" << (takeoffOk ? "true" : "false")
                 << " land_ok=" << (landOk ? "true" : "false")
                 << " gps_fix=" << (check.gpsFix ? "true" : "false")
                 << " ekf_ready=" << (check.ekfReady ? "true" : "false")
                 << " landed_known=" << (check.landedKnown ? "true" : "false")
                 << " landed_changed=" << (check.landedChanged ? "true" : "false")
                 << " battery_voltage=" << (check.batteryVoltage ? "true" : "false")
                 << " armed_true=" << (check.armedTrue ? "true" : "false")
                 << " lat_lon=" << (check.latLon ? "true" : "false"));
    return ok;
  }

  bool
  runLinkStateAgingTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto fields = requestTelemetryStatusForDroneSync(
      droneId, std::chrono::milliseconds(std::min(m_timeoutMs, 3000)));
    if (fields.empty()) {
      NDN_LOG_INFO("LINK_STATE_AGING_RESULT ok=false reason=no-initial-telemetry");
      return false;
    }

    auto logSample = [this, &droneId](const std::string& phase) {
      const auto safety = safetyForDrone(droneId);
      if (!safety) {
        NDN_LOG_INFO("LINK_STATE_AGING sample=" << phase
                     << " drone=" << droneId
                     << " state=missing");
        return SafetyState{};
      }
      NDN_LOG_INFO("LINK_STATE_AGING sample=" << phase
                   << " drone=" << droneId
                   << " state=" << safety->linkState
                   << " age_ms=" << safety->linkAgeMs
                   << " action=" << safety->lostLinkAction
                   << " attention=" << (safety->needsOperatorAttention() ? "true" : "false")
                   << " detail=" << safety->detail);
      const auto flight = FlightActionControlState::fromGate(
        FlightSafetyGateState::fromStates(droneId, readinessForDrone(droneId), safety));
      NDN_LOG_INFO("LINK_STATE_GATE sample=" << phase
                   << " " << flight.statusLine());
      return *safety;
    };

    const auto initial = logSample("initial");
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(m_linkStaleMs + 150));
    const auto stale = logSample("stale");
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }

    if (m_linkLostMs > stale.linkAgeMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(m_linkLostMs - stale.linkAgeMs + 150));
    }
    const auto lost = logSample("lost");

    const bool ok = !initial.droneId.empty() &&
                    stale.linkState == "stale" &&
                    lost.linkState == "lost" &&
                    lost.lostLinkAction == m_lostLinkAction;
    NDN_LOG_INFO("LINK_STATE_AGING_RESULT ok=" << (ok ? "true" : "false")
                 << " initial=" << initial.linkState
                 << " stale=" << stale.linkState
                 << " lost=" << lost.linkState
                 << " lost_link_action=" << lost.lostLinkAction);
    return ok;
  }

  bool
  runSingleDroneMissionUploadTest(std::chrono::seconds timeout, bool startMission)
  {
    const std::string droneId = targetDroneId();
    const std::string taskId = "mission-upload-" + std::to_string(nowMilliseconds());
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;

    auto makeWaypoints = [] {
      std::ostringstream os;
      os << std::fixed << std::setprecision(5)
         << "single-drone:"
         << 35.11860 << "," << -89.93750 << ">"
         << 35.11920 << "," << -89.93750 << ">"
         << 35.11920 << "," << -89.93680 << ">"
         << 35.11860 << "," << -89.93680;
      return os.str();
    };
    const std::string payload = encodeFields({
      {"type", "patrol-task"},
      {"patrol_task_id", taskId},
      {"mission_id", taskId},
      {"attempt_id", "1"},
      {"part_id", "single"},
      {"role", "single-drone-survey"},
      {"area", "single-drone-demo-area"},
      {"waypoints", makeWaypoints()},
      {"altitude_m", "12"},
      {"capture_required", "true"},
    });
    NDN_LOG_INFO("SINGLE_MISSION_START task=" << taskId
                 << " provider=" << droneId);

    auto requestMessage = makeRequest(payload);
    std::vector<ndn::Name> providerNames{droneIdentity(m_config, droneId)};
    boost::asio::post(m_face.getIoContext(), [this, requestMessage = std::move(requestMessage),
                                providerNames, taskId, &mutex, &cv, &done, &ok] () mutable {
      if (!m_containerReady.load() || !m_user) {
        std::lock_guard<std::mutex> guard(mutex);
        done = true;
        ok = false;
        cv.notify_all();
        return;
      }
      auto selectIdleCandidate =
        [providerNames](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
          std::vector<ndn_service_framework::AckSelectionCandidate> selected;
          for (const auto& candidate : candidates) {
            if (!candidate.ack.getStatus() || !candidate.providerName.equals(providerNames.front())) {
              continue;
            }
            const auto payload = candidate.ack.getPayload();
            const auto fields = decodeFields(
              std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
            if (fieldOr(fields, "mission_busy", "false") == "true") {
              continue;
            }
            selected.push_back(candidate);
            break;
          }
          return selected;
        };
      m_user->RequestService(
        providerNames,
        m_config.serviceMissionAssign,
        std::move(requestMessage),
        m_ackTimeoutMs,
        std::move(selectIdleCandidate),
        m_timeoutMs,
        [&mutex, &cv, &done, &ok, taskId](const ndn::Name&) {
          NDN_LOG_INFO("SINGLE_MISSION_TIMEOUT task=" << taskId);
          std::lock_guard<std::mutex> guard(mutex);
          done = true;
          ok = false;
          cv.notify_all();
        },
        [this, &mutex, &cv, &done, &ok, taskId](const ndn_service_framework::ResponseMessage& response) {
          const auto fields = decodeFields(responsePayload(response));
          const auto mission = MissionState::fromFields(fields);
          const bool responseOk = response.getStatus() && fieldOr(fields, "accepted", "false") == "true";
          updateMissionState(mission);
          NDN_LOG_INFO("SINGLE_MISSION_DONE task=" << taskId
                       << " ok=" << (responseOk ? "true" : "false")
                       << " provider=" << mission.droneId
                       << " phase=" << mission.phase
                       << " detail=" << mission.detail
                       << " mission_transport=" << mission.transport
                       << " mission_ack=" << mission.ack
                       << " waypoints_forwarded=" << mission.waypointsForwarded);
          std::lock_guard<std::mutex> guard(mutex);
          ok = responseOk;
          done = true;
          cv.notify_all();
        });
    });

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [&] { return done; });
    if (!(done && ok) || !startMission) {
      return done && ok;
    }
    lock.unlock();

    if (!sendMavlinkCommandToDroneSync(droneId, "arm", {{"arm", "true"}},
                                       std::chrono::milliseconds(m_timeoutMs))) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    if (!sendMavlinkCommandToDroneSync(droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}},
                                       std::chrono::milliseconds(m_timeoutMs))) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(6500));
    if (!sendMavlinkCommandToDroneSync(droneId, "start_mission", {},
                                       std::chrono::milliseconds(m_timeoutMs))) {
      return false;
    }
    for (int i = 0; i < 6; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      const auto telemetry = requestTelemetryStatusForDroneSync(
        droneId, std::chrono::milliseconds(m_timeoutMs));
      NDN_LOG_INFO("SINGLE_MISSION_TELEMETRY sample=" << i
                   << " drone=" << fieldOr(telemetry, "drone_id", droneId)
                   << " lat=" << fieldOr(telemetry, "lat", "unknown")
                   << " lon=" << fieldOr(telemetry, "lon", "unknown")
                   << " local_north_m=" << fieldOr(telemetry, "local_north_m", "unknown")
                   << " local_east_m=" << fieldOr(telemetry, "local_east_m", "unknown")
                   << " alt=" << fieldOr(telemetry, "altitude_m", "unknown")
                   << " speed=" << fieldOr(telemetry, "groundspeed_mps", "unknown"));
    }
    return true;
  }

  bool
  runAutoPatrolCompensationDemo(std::chrono::seconds timeout)
  {
    return runPatrolCompensationTask(timeout, 35.1186, -89.9375, 140.0, true);
  }

  bool
  runPatrolCompensationTask(std::chrono::seconds timeout, double centerLat, double centerLon,
                            double sideMeters, bool simulateFirstPartMissing,
                            const std::vector<std::pair<double, double>>& routeWaypoints = {})
  {
    struct PatrolDemoState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::map<std::string, MissionPart> parts;
      std::set<std::string> timedOut;
    };

    if (m_patrolDroneIds.size() < 2) {
      publishStatus("Patrol demo needs at least two drones");
      return false;
    }

    const std::string taskId = "patrol-" + std::to_string(nowMilliseconds());
    {
      std::lock_guard<std::mutex> guard(m_missionReadyMutex);
      m_missionReadyDrones.clear();
    }
    auto state = std::make_shared<PatrolDemoState>();
    std::vector<MissionWaypoint> missionRouteWaypoints;
    missionRouteWaypoints.reserve(routeWaypoints.size());
    for (const auto& waypoint : routeWaypoints) {
      missionRouteWaypoints.push_back({waypoint.first, waypoint.second});
    }

    auto departurePointForDrone = [this] (const std::string& droneId, MissionWaypoint fallback) {
      const auto telemetry = requestTelemetryStatusForDroneSync(droneId, std::chrono::milliseconds(900));
      try {
        const auto lat = std::stod(fieldOr(telemetry, "lat", ""));
        const auto lon = std::stod(fieldOr(telemetry, "lon", ""));
        if (std::isfinite(lat) && std::isfinite(lon)) {
          return MissionWaypoint{lat, lon};
        }
      }
      catch (const std::exception&) {
      }
      return fallback;
    };

    const auto initialPlan = buildPatrolMissionPlan(taskId, centerLat, centerLon, sideMeters,
                                                    m_patrolDroneIds, missionRouteWaypoints);
    std::map<std::string, MissionWaypoint> departurePoints;
    for (const auto& part : initialPlan.parts) {
      if (part.assignedDrone.empty() || departurePoints.count(part.assignedDrone) > 0) {
        continue;
      }
      const auto routeStart = part.firstWaypointOr(MissionWaypoint{centerLat, centerLon});
      departurePoints.emplace(part.assignedDrone, departurePointForDrone(part.assignedDrone, routeStart));
    }
    const auto plan = buildPatrolMissionPlan(taskId, centerLat, centerLon, sideMeters,
                                             m_patrolDroneIds, missionRouteWaypoints,
                                             departurePoints);
    updateMissionPlan(plan);
    for (const auto& part : plan.parts) {
      state->parts.emplace(part.id, part);
    }

    auto logLedger = [] (const std::string& line) {
      NDN_LOG_INFO(line);
    };
    logLedger("PATROL_PLAN " + plan.statusLine());
    for (const auto& part : plan.parts) {
      logLedger("PATROL_PART " + part.statusLine() + " waypoints=" + part.waypointText());
    }

    auto joinDroneIds = [] (const std::vector<std::string>& droneIds) {
      std::string out;
      for (size_t i = 0; i < droneIds.size(); ++i) {
        if (i > 0) {
          out += ",";
        }
        out += droneIds[i];
      }
      return out;
    };
    const std::string assignmentMode = "clustered-waypoints-return-to-start";
    const std::string patrolDroneText = joinDroneIds(m_patrolDroneIds);

    auto joinPartIds = [state] {
      std::string out;
      for (const auto& item : state->parts) {
        if (!out.empty()) {
          out += ",";
        }
        out += item.first;
      }
      return out;
    };

    auto emitProgress = [this, state, taskId, assignmentMode, patrolDroneText, logLedger](
                          std::string phase, uint64_t attempts) {
      auto appendId = [] (std::string& list, const std::string& id) {
        if (!list.empty() && list != "none") {
          list += ",";
        }
        if (list == "none") {
          list.clear();
        }
        list += id;
      };

      MissionProgressState progress;
      progress.taskId = taskId;
      progress.phase = std::move(phase);
      progress.assignment = assignmentMode;
      progress.drones = patrolDroneText;
      progress.attempts = attempts;
      progress.returnHomePlanned = true;
      progress.completedPartIds = "none";
      progress.missingPartIds = "none";
      progress.compensatedPartIds = "none";
      progress.pendingPartIds = "none";

      {
        std::lock_guard<std::mutex> guard(state->mutex);
        progress.totalParts = state->parts.size();
        for (const auto& item : state->parts) {
          const auto& part = item.second;
          if (part.done) {
            ++progress.completedParts;
            appendId(progress.completedPartIds, item.first);
          }
          else if (state->timedOut.find(item.first) != state->timedOut.end()) {
            ++progress.missingParts;
            appendId(progress.missingPartIds, item.first);
          }
          else {
            appendId(progress.pendingPartIds, item.first);
          }
          if (part.attempt > 1) {
            ++progress.compensatedParts;
            appendId(progress.compensatedPartIds, item.first);
          }
        }
      }

      updateMissionProgress(progress);
      logLedger("PATROL_PROGRESS " + progress.statusLine());
    };

    auto allDone = [state] {
      for (const auto& item : state->parts) {
        if (!item.second.done) {
          return false;
        }
      }
      return true;
    };

    auto dispatchPart = [&] (const std::string& partId, std::vector<std::string> droneIds,
                             int attempt, bool simulateNoResponse) {
      const std::string candidateText = joinDroneIds(droneIds);
      MissionPart part;
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        auto& storedPart = state->parts[partId];
        storedPart.assignedDrone = candidateText;
        storedPart.attempt = attempt;
        part = storedPart;
      }
      Fields payloadFields{
        {"type", "patrol-task"},
        {"patrol_task_id", taskId},
        {"mission_id", taskId},
        {"attempt_id", std::to_string(attempt)},
        {"part_id", part.id},
        {"role", part.role},
        {"area", "demo-area"},
        {"waypoints", part.waypointText()},
        {"capture_required", "true"},
        {"simulate_no_response", simulateNoResponse ? "true" : "false"},
        {"simulate_delay_ms", "6500"},
      };
      if (droneIds.size() == 1) {
        payloadFields.emplace("target_system", mavlinkTargetSystemForDrone(droneIds.front()));
        payloadFields.emplace("target_component", "1");
      }
      const std::string payload = encodeFields(payloadFields);
      logLedger("PATROL_ASSIGN task=" + taskId +
                " attempt=" + std::to_string(attempt) +
                " part=" + part.id +
                " candidates=" + candidateText +
                " simulate_no_response=" + (simulateNoResponse ? "true" : "false") +
                " waypoints=" + part.waypointText());

      auto requestMessage = makeRequest(payload);
      std::vector<ndn::Name> providerNames;
      providerNames.reserve(droneIds.size());
      for (const auto& droneId : droneIds) {
        providerNames.push_back(droneIdentity(m_config, droneId));
      }
      boost::asio::post(m_face.getIoContext(), [this, requestMessage = std::move(requestMessage),
                                  providerNames = std::move(providerNames),
                                  taskId, partId, candidateText,
                                  attempt, state,
                                  logLedger, emitProgress] () mutable {
        if (!m_containerReady.load() || !m_user) {
          logLedger("PATROL_RUNTIME_NOT_READY task=" + taskId +
                    " part=" + partId);
          {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->timedOut.insert(partId);
          }
          state->cv.notify_all();
          emitProgress("waiting-compensation", attempt);
          return;
        }
        auto selectIdleCandidate =
          [providerNames, taskId, partId, attempt, logLedger](
            const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
            std::vector<ndn_service_framework::AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              bool inCandidateSet = false;
              for (const auto& providerName : providerNames) {
                if (candidate.providerName.equals(providerName)) {
                  inCandidateSet = true;
                  break;
                }
              }
              if (!inCandidateSet || !candidate.ack.getStatus()) {
                if (!inCandidateSet) {
                  continue;
                }
              }

              const auto payload = candidate.ack.getPayload();
              const auto fields = decodeFields(
                std::string(reinterpret_cast<const char*>(payload.data()),
                            payload.size()));
              if (fieldOr(fields, "mission_busy", "false") == "true") {
                logLedger("PATROL_ACK_BUSY task=" + taskId +
                          " attempt=" + std::to_string(attempt) +
                          " part=" + partId +
                          " provider=" + candidate.providerName.toUri());
                continue;
              }
              if (!candidate.ack.getStatus()) {
                continue;
              }

              logLedger("PATROL_ACK_SELECTED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + candidate.providerName.toUri());
              selected.push_back(candidate);
              break;
            }
            return selected;
          };
        m_user->RequestService(
          providerNames,
          m_config.serviceMissionAssign,
          std::move(requestMessage),
          m_ackTimeoutMs,
          std::move(selectIdleCandidate),
          m_timeoutMs,
          [taskId, partId, candidateText, attempt, state, logLedger, emitProgress](const ndn::Name&) {
            logLedger("PATROL_PART_MISSING task=" + taskId +
                      " attempt=" + std::to_string(attempt) +
                      " part=" + partId +
                      " candidates=" + candidateText);
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              if (!state->parts[partId].done) {
                state->timedOut.insert(partId);
              }
            }
            state->cv.notify_all();
            emitProgress("waiting-compensation", attempt);
          },
          [this, taskId, partId, candidateText, attempt, state, logLedger, emitProgress](
            const ndn_service_framework::ResponseMessage& response) {
            const auto fields = decodeFields(responsePayload(response));
            auto mission = MissionState::fromFields(fields);
            const auto responder = mission.droneId == "unknown" ? candidateText : mission.droneId;
            if (mission.droneId == "unknown") {
              mission.droneId = responder;
            }
            bool accepted = false;
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              auto& part = state->parts[partId];
              if (!part.done && response.getStatus()) {
                part.done = true;
                part.completedBy = responder;
                accepted = true;
              }
            }
            if (accepted) {
              updateMissionState(mission);
              {
                std::lock_guard<std::mutex> readyGuard(m_missionReadyMutex);
                if (std::find(m_missionReadyDrones.begin(), m_missionReadyDrones.end(),
                              responder) == m_missionReadyDrones.end()) {
                  m_missionReadyDrones.push_back(responder);
                }
              }
              logLedger("PATROL_PART_DONE task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + responder +
                        " status=true" +
                        " phase=" + mission.phase +
                        " detail=" + mission.detail +
                        " mission_transport=" + mission.transport +
                        " waypoints_forwarded=" + mission.waypointsForwarded +
                        " waypoint_acks_accepted=" + mission.waypointAcksAccepted +
                        " mission_ack=" + mission.ack);
              emitProgress(attempt > 1 ? "compensating" : "assigning", attempt);
            }
            else {
              logLedger("PATROL_LATE_RESPONSE_IGNORED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + responder +
                        " status=" + (response.getStatus() ? "true" : "false"));
            }
            state->cv.notify_all();
          });
      });
    };

    const auto allPartIds = joinPartIds();
    logLedger("PATROL_TASK_START task=" + taskId +
              " parts=" + allPartIds +
              " drones=" + patrolDroneText +
              " assignment=" + assignmentMode +
              " center_lat=" + std::to_string(centerLat) +
              " center_lon=" + std::to_string(centerLon) +
              " side_m=" + std::to_string(sideMeters));
    logLedger("PATROL_ATTEMPT task=" + taskId + " attempt=1 parts=" + allPartIds);
    emitProgress("assigning", 1);
    size_t dispatchIndex = 0;
    for (const auto& item : state->parts) {
      const auto droneId = m_patrolDroneIds[dispatchIndex % m_patrolDroneIds.size()];
      dispatchPart(item.first, {droneId}, 1,
                   simulateFirstPartMissing && dispatchIndex == 0);
      ++dispatchIndex;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    state->cv.notify_all();

    std::vector<std::string> missingParts;
    bool completedInFirstAttempt = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, [&] {
        if (allDone()) {
          return true;
        }
        for (const auto& item : state->parts) {
          if (state->timedOut.find(item.first) != state->timedOut.end()) {
            return true;
          }
        }
        return false;
      });
      if (allDone()) {
        completedInFirstAttempt = true;
      }
      else {
        for (const auto& item : state->parts) {
          if (!item.second.done &&
              state->timedOut.find(item.first) != state->timedOut.end()) {
            missingParts.push_back(item.first);
          }
        }
        if (missingParts.empty()) {
          for (const auto& item : state->parts) {
            if (!item.second.done) {
              missingParts.push_back(item.first);
            }
          }
        }
      }
    }
    if (completedInFirstAttempt) {
      emitProgress("completed", 1);
      logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=1");
      return true;
    }
    emitProgress("waiting-compensation", 1);

    for (const auto& partId : missingParts) {
      logLedger("PATROL_COMPENSATION task=" + taskId +
                " attempt=2 parts=" + partId +
                " candidates=" + patrolDroneText);
      emitProgress("compensating", 2);
      dispatchPart(partId, m_patrolDroneIds, 2, false);
    }

    bool failed = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, allDone);
      if (!allDone()) {
        failed = true;
      }
    }
    if (failed) {
      emitProgress("failed", 2);
      logLedger("PATROL_TASK_FAILED task=" + taskId);
      return false;
    }
    emitProgress("completed", 2);
    logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=2");
    return true;
  }

private:
  void
  clearTelemetryInFlight(const std::string& droneId)
  {
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    m_telemetryInFlightDrones.erase(droneId);
  }

  void
  updateDroneState(const TelemetryState& telemetry, const MissionState& mission)
  {
    const auto droneId = telemetry.droneId == "unknown" ? mission.droneId : telemetry.droneId;
    if (droneId.empty() || droneId == "unknown") {
      return;
    }
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    auto storedTelemetry = telemetry;
    storedTelemetry.droneId = droneId;
    m_telemetryByDrone[droneId] = storedTelemetry;
    auto storedReadiness = ReadinessState::fromTelemetry(storedTelemetry);
    storedReadiness.droneId = droneId;
    m_readinessByDrone[droneId] = storedReadiness;
    auto storedSafety = SafetyState::fromTelemetry(storedTelemetry);
    storedSafety.droneId = droneId;
    storedSafety.linkAgeMs = 0;
    storedSafety.lostLinkAction = m_lostLinkAction;
    m_safetyByDrone[droneId] = storedSafety;
    auto storedVideo = VideoState::fromFields(storedTelemetry.toFields());
    storedVideo.droneId = droneId;
    const auto previousVideo = m_videoByDrone.find(droneId);
    if (storedVideo.status == "unknown" && previousVideo != m_videoByDrone.end()) {
      storedVideo.status = previousVideo->second.status;
    }
    if (storedVideo.streamId == "unknown" && previousVideo != m_videoByDrone.end()) {
      storedVideo.streamId = previousVideo->second.streamId;
    }
    if (storedVideo.updatedMs == 0) {
      storedVideo.updatedMs = storedTelemetry.timestampMs;
    }
    m_videoByDrone[droneId] = storedVideo;
    auto storedMission = mission;
    if (storedMission.droneId == "unknown") {
      storedMission.droneId = droneId;
    }
    const auto previousMission = m_missionByDrone.find(droneId);
    const bool emptyIdleTelemetryMission =
      storedMission.isIdle() &&
      (storedMission.missionId.empty() || storedMission.missionId == "none" ||
       storedMission.missionId == "unknown") &&
      (storedMission.partId.empty() || storedMission.partId == "none" ||
       storedMission.partId == "unknown") &&
      storedMission.updatedMs == 0;
    if (previousMission != m_missionByDrone.end() &&
        !previousMission->second.isIdle() &&
        emptyIdleTelemetryMission) {
      return;
    }
    m_missionByDrone[droneId] = storedMission;
  }

  bool
  ageSafetyStateLocked(const std::string& droneId, SafetyState& state) const
  {
    const auto telemetry = m_telemetryByDrone.find(droneId);
    if (telemetry == m_telemetryByDrone.end()) {
      return false;
    }
    state.droneId = droneId;
    state.lostLinkAction = m_lostLinkAction;
    if (telemetry->second.timestampMs == 0) {
      return true;
    }

    const auto now = nowMilliseconds();
    state.linkAgeMs = now > telemetry->second.timestampMs ?
      now - telemetry->second.timestampMs : 0;
    if (state.linkAgeMs >= m_linkLostMs) {
      state.linkState = "lost";
      state.detail = "telemetry-lost";
      return true;
    }
    if (state.linkAgeMs >= m_linkStaleMs) {
      state.linkState = "stale";
      state.detail = "telemetry-stale";
      return true;
    }
    if (state.linkState == "unknown" || state.linkState == "lost" ||
        state.linkState == "stale") {
      state.linkState = "connected";
    }
    if (state.detail == "telemetry-lost" || state.detail == "telemetry-stale") {
      state.detail = "telemetry-fresh";
    }
    return true;
  }

  void
  updateMissionState(const MissionState& mission)
  {
    if (mission.droneId.empty() || mission.droneId == "unknown") {
      return;
    }
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    m_missionByDrone[mission.droneId] = mission;
  }

  void
  updateMissionProgress(MissionProgressState progress)
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    m_latestMissionProgress = std::move(progress);
  }

  void
  updateMissionPlan(MissionPlan plan)
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    m_latestMissionPlan = std::move(plan);
  }

  void
  updateVideoState(const std::string& droneId, const Fields& fields)
  {
    if (droneId.empty() || droneId == "unknown") {
      return;
    }
    auto video = VideoState::fromFields(fields);
    video.droneId = droneId;
    if (video.updatedMs == 0) {
      video.updatedMs = nowMilliseconds();
    }
    {
      std::lock_guard<std::mutex> guard(m_telemetryMutex);
      const auto previous = m_videoByDrone.find(droneId);
      if (video.streamId == "unknown" && previous != m_videoByDrone.end()) {
        video.streamId = previous->second.streamId;
      }
      m_videoByDrone[droneId] = video;
    }
  }

  std::atomic<bool>&
  mavlinkInFlightFlag(bool isManualControl, bool isEmergencyStop)
  {
    if (isManualControl) {
      return m_manualControlInFlight;
    }
    if (isEmergencyStop) {
      return m_emergencyStopInFlight;
    }
    return m_mavlinkCommandInFlight;
  }

  void
  updateCommandState(const FlightCommandState& command)
  {
    if (command.droneId.empty() || command.droneId == "unknown") {
      return;
    }
    std::lock_guard<std::mutex> guard(m_telemetryMutex);
    m_commandByDrone[command.droneId] = command;
    NDN_LOG_INFO("GS_COMMAND_STATE " << command.statusLine());
  }

  void
  recordBlockedCommand(const std::string& droneId, const std::string& commandName,
                       const std::string& reason)
  {
    updateCommandState(FlightCommandState{
      droneId,
      commandName,
      "false",
      "blocked",
      "unknown",
      "unknown",
      "unknown",
      "unknown",
      "0",
      reason,
      nowMilliseconds(),
    });
  }

  bool
  validateFlightSafetyGate(const std::string& droneId, const std::string& action,
                           uint64_t maxAgeMs, std::string& reason)
  {
    const auto telemetry = freshTelemetryForSafetyCheck(droneId, maxAgeMs);
    if (!telemetry) {
      reason = "no-telemetry";
      return false;
    }
    const auto readiness = ReadinessState::fromTelemetry(*telemetry);
    const auto safety = SafetyState::fromTelemetry(*telemetry);
    return FlightSafetyGateState::fromStates(droneId, readiness, safety)
      .actionAllowed(action, reason);
  }

  bool
  validateArmReadiness(const std::string& droneId, std::string& reason)
  {
    return validateFlightSafetyGate(droneId, "arm", 2500, reason);
  }

  bool
  validateTakeoffReadiness(const std::string& droneId, std::string& reason)
  {
    return validateFlightSafetyGate(droneId, "takeoff", 2500, reason);
  }

  bool
  validateLandReadiness(const std::string& droneId, std::string& reason)
  {
    return validateFlightSafetyGate(droneId, "land", 2500, reason);
  }

  bool
  validateManualControlReadiness(const std::string& droneId, std::string& reason)
  {
    return validateFlightSafetyGate(droneId, "manual_control", 1200, reason);
  }

  std::optional<TelemetryState>
  freshTelemetryForSafetyCheck(const std::string& droneId, uint64_t maxAgeMs)
  {
    auto telemetry = telemetryForDrone(droneId);
    if (!telemetry || telemetry->timestampMs == 0 ||
        nowMilliseconds() > telemetry->timestampMs + maxAgeMs) {
      const auto fields = requestTelemetryStatusForDroneSync(
        droneId, std::chrono::milliseconds(std::min(m_timeoutMs, 2500)));
      if (!fields.empty()) {
        auto fresh = TelemetryState::fromFields(fields);
        if (fresh.droneId == "unknown") {
          fresh.droneId = droneId;
        }
        updateDroneState(fresh, MissionState::fromFields(fields));
        telemetry = fresh;
      }
    }
    return telemetry;
  }

  void
  installServiceInstances()
  {
    m_objectDetectionProvider = std::make_unique<ndn_service_framework::ServiceProvider>(
      m_face, m_config.groupPrefix, m_gsCert, m_controllerCert, m_config.trustSchema);
    m_objectDetectionProvider->setHandlerThreads(2);
    m_objectDetectionProvider->setAckThreads(1);
    installObjectDetectionService();
  }

  void
  installObjectDetectionService()
  {
    using ServiceInvocationMode = ndn_service_framework::ServiceProvider::ServiceInvocationMode;

    m_objectDetectionProvider->addService(
      m_config.serviceGsObjectDetection,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = m_streaming.load();
          decision.message = decision.status ? "object detection ready" : "video not streaming";
          decision.payload = bufferFromString(encodeFields(Fields{
            {"gs", m_config.groundStationIdentity.toUri()},
            {"ready", decision.status ? "true" : "false"},
          }));
          return decision;
        }),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          const auto payload = request.getPayload();
          const auto fields = decodeFields(std::string(
            reinterpret_cast<const char*>(payload.data()), payload.size()));
          const auto frameId = fieldOr(fields, "frame_id", "live-frame");
          const auto frameSeq = std::stoull(fieldOr(fields, "frame_seq", "0"));
          auto detection = runYoloDetection(frameId);
          const bool ok = fieldOr(detection, "ok", "false") == "true";
          const bool car = fieldOr(detection, "car", "false") == "true";
          const bool truck = fieldOr(detection, "truck", "false") == "true";
          const auto objects = fieldOr(detection, "objects", "none");
          return makeResponse(true, encodeFields({
            {"frame_id", frameId},
            {"frame_seq", std::to_string(frameSeq)},
            {"objects", objects},
            {"car", car ? "true" : "false"},
            {"truck", truck ? "true" : "false"},
            {"detector_ok", ok ? "true" : "false"},
            {"car_count", fieldOr(detection, "car_count", "0")},
            {"truck_count", fieldOr(detection, "truck_count", "0")},
            {"car_conf", fieldOr(detection, "car_conf", "0")},
            {"truck_conf", fieldOr(detection, "truck_conf", "0")},
            {"model", fieldOr(detection, "model", m_yoloModel)},
            {"summary", ok ? (objects == "none" ? "no target vehicle" : "detected " + objects)
                           : fieldOr(detection, "error", "detector failed")},
          }));
        }),
      ServiceInvocationMode::NormalOnly);
  }

  bool
  readYoloWorkerLineLocked(std::string& line, int timeoutMs)
  {
    line.clear();
    if (m_yoloWorkerOutFd < 0) {
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(m_yoloWorkerOutFd, &readSet);

      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      const auto ready = select(m_yoloWorkerOutFd + 1, &readSet, nullptr, nullptr, &tv);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (ready == 0) {
        continue;
      }

      char ch = 0;
      const auto n = read(m_yoloWorkerOutFd, &ch, 1);
      if (n <= 0) {
        return false;
      }
      if (ch == '\n') {
        while (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (!line.empty()) {
          return true;
        }
        continue;
      }
      line.push_back(ch);
    }
    return false;
  }

  bool
  startYoloWorkerLocked()
  {
    if (m_yoloWorkerPid > 0 && m_yoloWorkerInFd >= 0 && m_yoloWorkerOutFd >= 0) {
      return true;
    }
    stopYoloWorkerLocked();

    int toChild[2] = {-1, -1};
    int fromChild[2] = {-1, -1};
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
      return false;
    }

    const auto pid = fork();
    if (pid < 0) {
      close(toChild[0]);
      close(toChild[1]);
      close(fromChild[0]);
      close(fromChild[1]);
      return false;
    }

    if (pid == 0) {
      dup2(toChild[0], STDIN_FILENO);
      dup2(fromChild[1], STDOUT_FILENO);
      close(toChild[0]);
      close(toChild[1]);
      close(fromChild[0]);
      close(fromChild[1]);
      const auto command =
        pythonUserEnvironmentPrefix() +
        "python3 " + shellQuote(m_yoloWorkerScript) +
        " --model " + shellQuote(m_yoloModel) +
        " --conf 0.25 --classes car,truck";
      execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }

    close(toChild[0]);
    close(fromChild[1]);
    m_yoloWorkerPid = pid;
    m_yoloWorkerInFd = toChild[1];
    m_yoloWorkerOutFd = fromChild[0];

    std::string line;
    while (readYoloWorkerLineLocked(line, 30000)) {
      const auto fields = decodeFields(line);
      if (fieldOr(fields, "ready", "false") == "true") {
        NDN_LOG_INFO("GS_OBJECT_DETECTION worker ready model=" << fieldOr(fields, "model", m_yoloModel));
        return true;
      }
      if (fieldOr(fields, "ready", "") == "false") {
        NDN_LOG_WARN("GS_OBJECT_DETECTION worker unavailable: " << fieldOr(fields, "error", "unknown"));
        stopYoloWorkerLocked();
        return false;
      }
    }

    NDN_LOG_WARN("GS_OBJECT_DETECTION worker did not become ready");
    stopYoloWorkerLocked();
    return false;
  }

  void
  stopYoloWorker()
  {
    std::lock_guard<std::mutex> guard(m_yoloMutex);
    stopYoloWorkerLocked();
  }

  void
  stopYoloWorkerLocked()
  {
    if (m_yoloWorkerInFd >= 0) {
      const std::string quit = "__quit__\n";
      const auto ignored = write(m_yoloWorkerInFd, quit.data(), quit.size());
      (void)ignored;
      close(m_yoloWorkerInFd);
      m_yoloWorkerInFd = -1;
    }
    if (m_yoloWorkerOutFd >= 0) {
      close(m_yoloWorkerOutFd);
      m_yoloWorkerOutFd = -1;
    }
    if (m_yoloWorkerPid > 0) {
      int status = 0;
      if (waitpid(m_yoloWorkerPid, &status, WNOHANG) == 0) {
        kill(m_yoloWorkerPid, SIGTERM);
        waitpid(m_yoloWorkerPid, nullptr, 0);
      }
      m_yoloWorkerPid = -1;
    }
  }

  Fields
  runYoloDetectionOnceLocked(const std::string& imagePath)
  {
    const auto command =
      pythonUserEnvironmentPrefix() +
      "python3 " + shellQuote(m_yoloScript) +
      " --model " + shellQuote(m_yoloModel) +
      " --image " + shellQuote(imagePath) +
      " --conf 0.25 --classes car,truck";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    std::string resultText;
    if (pipe) {
      std::array<char, 512> buffer{};
      while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        resultText += buffer.data();
      }
    }
    if (resultText.empty()) {
      return {
        {"ok", "false"},
        {"error", "YOLO helper produced no output"},
        {"objects", "none"},
        {"car", "false"},
        {"truck", "false"},
      };
    }

    std::string resultLine;
    std::istringstream lines(resultText);
    for (std::string line; std::getline(lines, line);) {
      while (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty()) {
        resultLine = line;
      }
    }
    if (resultLine.empty()) {
      return {
        {"ok", "false"},
        {"error", "YOLO helper produced no parseable output"},
        {"objects", "none"},
        {"car", "false"},
        {"truck", "false"},
      };
    }
    return decodeFields(resultLine);
  }

  Fields
  runYoloDetectionWorkerLocked(const std::string& imagePath)
  {
    if (!startYoloWorkerLocked()) {
      return runYoloDetectionOnceLocked(imagePath);
    }

    const auto request = imagePath + "\n";
    if (write(m_yoloWorkerInFd, request.data(), request.size()) !=
        static_cast<ssize_t>(request.size())) {
      stopYoloWorkerLocked();
      return runYoloDetectionOnceLocked(imagePath);
    }

    std::string line;
    while (readYoloWorkerLineLocked(line, 15000)) {
      const auto fields = decodeFields(line);
      if (fields.find("ok") != fields.end()) {
        return fields;
      }
    }

    NDN_LOG_WARN("GS_OBJECT_DETECTION worker timed out; falling back to one-shot helper");
    stopYoloWorkerLocked();
    return runYoloDetectionOnceLocked(imagePath);
  }

  Fields
  runYoloDetection(const std::string& frameId)
  {
    std::vector<uint8_t> image;
    {
      std::lock_guard<std::mutex> frameGuard(m_latestDecodedFrameMutex);
      image = m_latestDecodedFrame;
    }
    if (image.empty()) {
      return {
        {"ok", "false"},
        {"error", "no decoded live frame available at ground station"},
        {"objects", "none"},
        {"car", "false"},
        {"truck", "false"},
      };
    }

    std::lock_guard<std::mutex> guard(m_yoloMutex);
    const auto imagePath = "/tmp/ndnsf-uav-yolo-" + std::to_string(getuid()) +
                           "-" + std::to_string(nowMilliseconds()) + ".jpg";
    {
      std::ofstream output(imagePath, std::ios::binary);
      output.write(reinterpret_cast<const char*>(image.data()),
                   static_cast<std::streamsize>(image.size()));
    }

    auto fields = runYoloDetectionWorkerLocked(imagePath);
    std::remove(imagePath.c_str());
    fields["frame_id"] = frameId;
    NDN_LOG_INFO("GS_OBJECT_DETECTION frame=" << frameId
                 << " ok=" << fieldOr(fields, "ok", "false")
                 << " objects=" << fieldOr(fields, "objects", "none")
                 << " error=" << fieldOr(fields, "error", ""));
    return fields;
  }

  void
  startVideoAttempt(std::string droneId, uint64_t requestedBitrateKbps = 0)
  {
    if (requestedBitrateKbps == 0) {
      requestedBitrateKbps = m_videoBitrateKbps.load();
    }
    postRequestForDrone(droneId, droneVideoControlService(m_config, droneId),
                encodeFields({
                  {"type", "video-control"},
                  {"action", "start"},
                  {"fps", std::to_string(VIDEO_FPS)},
                  {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
                  {"requested_frame_width", std::to_string(m_videoFrameWidth)},
                }),
                [this, droneId, requestedBitrateKbps](const std::string& payload) {
                  const auto fields = decodeFields(payload);
                  const auto prefix = fieldOr(fields, "stream_prefix", "");
                  const auto seqText = fieldOr(fields, "next_seq", "0");
                  if (prefix.empty()) {
                    publishStatus("Video control response missing stream prefix");
                    return;
                  }

                  updateVideoState(droneId, fields);
                  m_videoBitrateKbps = requestedBitrateKbps;
                  m_streamPrefix = ndn::Name(prefix);
                  {
                    std::lock_guard<std::mutex> guard(m_videoStateMutex);
                    m_activeVideoDroneId = droneId;
                  }
                  configurePrefetch(fields);
                  m_keyLane = PacketLane{};
                  m_deltaLane = PacketLane{"packet", 0, 0, 0, 0};
	                  m_videoPumpScheduled = false;
                  m_streaming = true;
                  m_seenVideoStart = true;
                  m_videoStartInFlight = false;
                  m_firstFrameMs = 0;
                  m_receivedChunks = 0;
                  m_frameNacks = 0;
                  m_frameTimeouts = 0;
                  m_duplicateVideoPackets = 0;
                  m_decodedVideoFrames = 0;
                  m_lastVideoAdaptiveLogMs = 0;
                  resetVideoAdaptiveState();
                  m_highestReceivedVideoPacketSeq = UINT64_MAX;
                  m_nextChunkSeqToDecode = 0;
                  resetVideoPacketTracking();
                  {
                    std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
                    m_chunkQueue.clear();
                    m_pendingChunks.clear();
                    m_decoderPendingChunkCount = 0;
                    m_decoderOutBuffer.clear();
                  }
                  stopDecoder();
                  startDecoder();
                  publishVideoAdaptiveState("configured", true);
                  publishStatus("Video packet stream drone=" + droneId + " from " + prefix);
                  requestVideoPackets();
                },
                [this, droneId, requestedBitrateKbps] {
                  if (m_seenVideoStart.load()) {
                    return true;
                  }
                  const uint64_t retry = m_videoStartRetries.fetch_add(1);
                  if (retry < MAX_VIDEO_START_RETRIES) {
                    publishStatus("Video start retry " + std::to_string(retry + 1));
                    boost::asio::post(m_face.getIoContext(), [this, droneId, requestedBitrateKbps] {
                      startVideoAttempt(droneId, requestedBitrateKbps);
                    });
                    return true;
                  }
                  return false;
                },
                [this] {
                  if (!m_seenVideoStart.load()) {
                    m_videoStartInFlight = false;
                  }
                });
  }

  void
  restartVideoWithBitrateAfterStop(std::string droneId, uint64_t requestedBitrateKbps)
  {
    m_videoStopDelayInjected = false;
    m_seenVideoStart = false;
    m_videoStartRetries = 0;
    startVideoAttempt(std::move(droneId), requestedBitrateKbps);
  }

  bool
  restartVideoWithBitrate(const std::string& droneId,
                          uint64_t requestedBitrateKbps,
                          uint64_t previousBitrateKbps,
                          const std::string& action,
                          const std::string& reason)
  {
    requestedBitrateKbps = std::max<uint64_t>(128, requestedBitrateKbps);
    if (m_videoStopInFlight.exchange(true)) {
      publishStatus("Video bitrate change already pending");
      return false;
    }
    if (m_videoStartInFlight.exchange(true)) {
      m_videoStopInFlight = false;
      publishStatus("Video bitrate change already pending");
      return false;
    }
    m_videoBitrateKbps = requestedBitrateKbps;
    m_videoBitrateAdviceSinceMs = 0;
    m_lastVideoBitrateApplyMs = nowMilliseconds();
    m_streaming = false;
    m_videoPumpScheduled = false;
    boost::system::error_code ec;
    m_videoPumpTimer.cancel(ec);
    publishVideoAdaptiveState("bitrate-change-requested", true);
    stopDecoder();
    NDN_LOG_INFO("GS_VIDEO_BITRATE_CHANGE_APPLY drone=" << droneId
                 << " from_kbps=" << previousBitrateKbps
                 << " to_kbps=" << requestedBitrateKbps
                 << " action=" << action
                 << " reason=" << reason);
    publishStatus("Applying video bitrate drone=" + droneId +
                  " from=" + std::to_string(previousBitrateKbps) +
                  "kbps to=" + std::to_string(requestedBitrateKbps) +
                  "kbps reason=" + reason);
    stopVideoAttempt(
      droneId,
      [this, droneId, requestedBitrateKbps] {
        restartVideoWithBitrateAfterStop(droneId, requestedBitrateKbps);
      },
      [this] {
        m_videoStartInFlight = false;
      });
    return true;
  }

  void
  maybeApplyVideoBitratePolicy(const VideoAdaptiveState& state, const std::string& reason)
  {
    if (m_videoBitratePolicy != "auto-after-pressure") {
      m_videoBitrateAdviceSinceMs = 0;
      return;
    }
    if (reason == "configured" ||
        reason == "stop-ack" ||
        reason == "bitrate-change-requested") {
      return;
    }
    if (!isStreamingForDrone(state.droneId) ||
        state.bitrateAction != "decrease" ||
        state.suggestedBitrateKbps == 0 ||
        state.suggestedBitrateKbps >= state.acceptedBitrateKbps) {
      m_videoBitrateAdviceSinceMs = 0;
      return;
    }

    const auto nowMs = nowMilliseconds();
    auto sinceMs = m_videoBitrateAdviceSinceMs.load();
    if (sinceMs == 0) {
      m_videoBitrateAdviceSinceMs = nowMs;
      NDN_LOG_INFO("GS_VIDEO_BITRATE_POLICY_ARMED drone=" << state.droneId
                   << " policy=" << m_videoBitratePolicy
                   << " suggested_kbps=" << state.suggestedBitrateKbps
                   << " accepted_kbps=" << state.acceptedBitrateKbps
                   << " reason=" << state.bitrateReason);
      sinceMs = nowMs;
    }
    if (nowMs < sinceMs + m_videoBitrateAutoPressureMs) {
      return;
    }
    const auto lastApplyMs = m_lastVideoBitrateApplyMs.load();
    if (lastApplyMs != 0 && nowMs < lastApplyMs + VIDEO_BITRATE_APPLY_COOLDOWN_MS) {
      return;
    }

    NDN_LOG_INFO("GS_VIDEO_BITRATE_POLICY_APPLY drone=" << state.droneId
                 << " policy=" << m_videoBitratePolicy
                 << " pressure_ms=" << (nowMs - sinceMs)
                 << " from_kbps=" << state.acceptedBitrateKbps
                 << " to_kbps=" << state.suggestedBitrateKbps
                 << " reason=" << state.bitrateReason);
    restartVideoWithBitrate(state.droneId,
                            state.suggestedBitrateKbps,
                            state.acceptedBitrateKbps,
                            "auto-" + state.bitrateAction,
                            state.bitrateReason);
  }

  void
  stopVideoAttempt(std::string droneId,
                   std::function<void()> onStopped = {},
                   std::function<void()> onStopTimeout = {})
  {
    Fields stopFields{{"type", "video-control"}, {"action", "stop"}};
    if (const auto* delayMs = std::getenv("NDNSF_UAV_SIMULATE_STOP_DELAY_MS")) {
      if (!m_videoStopDelayInjected.exchange(true)) {
        stopFields["simulate_delay_ms"] = delayMs;
      }
    }
    postRequestForDrone(droneId, droneVideoControlService(m_config, droneId),
                encodeFields(stopFields),
                [this, droneId, onStopped = std::move(onStopped)](const std::string& payload) {
                  m_videoStopInFlight = false;
                  const auto fields = decodeFields(payload);
                  updateVideoState(droneId, fields);
                  publishVideoAdaptiveState("stop-ack", true);
                  {
                    std::lock_guard<std::mutex> guard(m_videoStateMutex);
                    if (m_activeVideoDroneId == droneId) {
                      m_activeVideoDroneId.clear();
                    }
                  }
                  publishStatus("Video stopped drone=" + droneId + ", packets=" +
                                fieldOr(fields, "stream_packets_published",
                                        fieldOr(fields, "frames_published", "0")) +
                                ", fec_groups=" +
                                fieldOr(fields, "fec_groups_published", "0"));
                  if (onStopped) {
                    onStopped();
                  }
                },
                [this, droneId] {
                  return false;
                },
                [this, droneId, onStopTimeout = std::move(onStopTimeout)] {
                  m_videoStopInFlight = false;
                  publishStatus("Video stop timed out for drone " + droneId +
                                "; NDNSF status diagnostics were queried. "
                                "If the drone still shows video streaming, "
                                "click Stop Video again.");
                  if (onStopTimeout) {
                    onStopTimeout();
                  }
                });
  }

  void
  requestRecordingManifestForDrone(const std::string& droneId, bool playAfterRefresh)
  {
    postRequestForDrone(
      droneId,
      droneCameraRecordingManifestService(m_config, droneId),
      encodeFields({{"type", "camera-recording-manifest-request"}}),
      [this, droneId, playAfterRefresh](const std::string& payload) {
        const auto fields = decodeFields(payload);
        auto manifest = RecordingDataProductState::fromFields(fields, droneId);
        {
          std::lock_guard<std::mutex> guard(m_recordingManifestMutex);
          m_recordingManifests[droneId] = manifest;
        }
        publishStatus(manifest.statusLine());
        publishStatus("Recording manifest drone=" + droneId +
                      " chunks=" + std::to_string(manifest.chunks) +
                      " bytes=" + std::to_string(manifest.bytes) +
                      " session=" + manifest.sessionId +
                      " encryption=" + manifest.encryption +
                      " playable=" + std::string(manifest.isPlayable() ? "true" : "false"));
        if (playAfterRefresh) {
          startRecordingPlayback(manifest);
        }
      });
  }

  void
  startRecordingPlayback(const RecordingDataProductState& manifest)
  {
    if (!manifest.isAvailable()) {
      publishStatus("No recorded video chunks for drone " + manifest.droneId);
      return;
    }
    if (!manifest.isPlayable()) {
      publishStatus("Recording data product is not playable drone=" + manifest.droneId +
                    " encryption=" + manifest.encryption +
                    " key_bytes=" + std::to_string(manifest.contentKey.size()));
      return;
    }

    m_streaming = false;
    m_videoPumpScheduled = false;
    boost::system::error_code ec;
    m_videoPumpTimer.cancel(ec);
    stopDecoder();
    m_firstFrameMs = nowMilliseconds();
    m_receivedChunks = 0;
    m_nextChunkSeqToDecode = 0;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_pendingChunks.clear();
      m_recordingPlaybackChunks.clear();
      m_decoderOutBuffer.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_videoStateMutex);
      m_activeVideoDroneId.clear();
      m_recordingPlaybackDroneId = manifest.droneId;
    }
    m_recordingPlaybackActive = true;
    startDecoder();
    publishStatus("Recording playback drone=" + manifest.droneId +
                  " chunks=" + std::to_string(manifest.chunks));
    constexpr uint64_t recordingFetchWindow = 16;
    const auto initialFetches = std::min<uint64_t>(recordingFetchWindow, manifest.chunks);
    for (uint64_t index = 0; index < initialFetches; ++index) {
      fetchRecordingChunk(manifest, index, recordingFetchWindow);
    }
  }

  void
  fetchRecordingChunk(RecordingDataProductState manifest, uint64_t index, uint64_t stride)
  {
    if (!m_recordingPlaybackActive.load() ||
        activeRecordingPlaybackDroneId() != manifest.droneId ||
        index >= manifest.chunks) {
      if (index >= manifest.chunks) {
        publishStatus("Recording playback completed drone=" + manifest.droneId +
                      " chunks=" + std::to_string(manifest.chunks));
      }
      return;
    }

    const auto objectName = manifest.chunkObjectName(index);
    if (index < 3 || index + 1 == manifest.chunks) {
      publishStatus("Recording chunk request drone=" + manifest.droneId +
                    " index=" + std::to_string(index));
    }
    fetchRecordingChunkData(
      manifest,
      objectName,
      [this, manifest, index, stride](std::vector<uint8_t> payload) mutable {
        if (!payload.empty()) {
          {
            std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
            m_recordingPlaybackChunks[index] = payload;
          }
          insertChunkForDecode(index, payload, nowMilliseconds() - m_firstFrameMs.load());
          const auto receivedCount = ++m_receivedChunks;
          if (receivedCount <= 3 || receivedCount % 30 == 0) {
            publishStatus("Recording playback chunk drone=" + manifest.droneId +
                          " count=" + std::to_string(receivedCount) +
                          " index=" + std::to_string(index));
          }
        }
        if (index + stride < manifest.chunks) {
          fetchRecordingChunk(std::move(manifest), index + stride, stride);
        }
        else if (m_receivedChunks.load() >= manifest.chunks) {
          publishStatus("Recording playback fetched drone=" + manifest.droneId +
                        " chunks=" + std::to_string(m_receivedChunks.load()));
          m_closeDecoderInputWhenQueueDrained = true;
          m_decoderQueueCv.notify_one();
          decodeRecordingFromFetchedChunksAsync(manifest);
        }
      },
      [this, manifest, index] {
        publishStatus("Recording chunk timeout drone=" + manifest.droneId +
                      " index=" + std::to_string(index));
      });
  }

  void
  fetchRecordingChunkData(const RecordingDataProductState& manifest,
                          const std::string& objectName,
                          std::function<void(std::vector<uint8_t>)> onData,
                          std::function<void()> onTimeout)
  {
    boost::asio::post(m_face.getIoContext(), [this, manifest, objectName,
                                onData = std::move(onData),
                                onTimeout = std::move(onTimeout)]() mutable {
      ndn::Interest interest{ndn::Name(objectName)};
      interest.setCanBePrefix(false);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(2500_ms);
      m_face.expressInterest(
        interest,
        [this, manifest, objectName, onData = std::move(onData)](
          const ndn::Interest&, const ndn::Data& data) mutable {
          std::vector<uint8_t> encrypted(data.getContent().value(),
                                         data.getContent().value() + data.getContent().value_size());
          auto plaintext = decryptRecordingChunkData(manifest, objectName, encrypted);
          onData(std::move(plaintext));
        },
        [this, objectName](const ndn::Interest&, const ndn::lp::Nack&) {
          publishStatus("Recording chunk Nack object=" + objectName);
        },
        [onTimeout = std::move(onTimeout)](const ndn::Interest&) mutable {
          onTimeout();
        });
    });
  }

  std::vector<uint8_t>
  decryptRecordingChunkData(const RecordingDataProductState& manifest,
                            const std::string& objectName,
                            const std::vector<uint8_t>& encryptedPayload) const
  {
    if (encryptedPayload.empty()) {
      return {};
    }
    if (manifest.encryption != "hybrid-aes-256-gcm-at-rest") {
      return encryptedPayload;
    }
    if (manifest.contentKey.size() != ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE) {
      NDN_LOG_WARN("GS_RECORDING_DECRYPT_NO_KEY object=" << objectName
                   << " key_bytes=" << manifest.contentKey.size());
      return {};
    }

    auto [ok, block] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(encryptedPayload.data(), encryptedPayload.size()));
    ndn_service_framework::HybridMessageEnvelope envelope;
    if (!ok || !envelope.WireDecode(block)) {
      NDN_LOG_WARN("GS_RECORDING_DECRYPT_BAD_ENVELOPE object=" << objectName);
      return {};
    }
    if (envelope.getKeyId() != manifest.keyId ||
        envelope.getMessageType() != "uav-camera-recording-chunk") {
      NDN_LOG_WARN("GS_RECORDING_DECRYPT_REJECT object=" << objectName
                   << " key_id=" << envelope.getKeyId()
                   << " expected_key_id=" << manifest.keyId
                   << " message_type=" << envelope.getMessageType());
      return {};
    }

    const auto adString = "ndnsf-uav-recording|" +
      droneIdentity(m_config, manifest.droneId).toUri() + "|" +
      manifest.sessionId + "|" + objectName;
    const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adString.data()), adString.size());
    ndn::Buffer key(manifest.contentKey.data(), manifest.contentKey.size());
    ndn::Buffer plaintext;
    if (!ndn_service_framework::hybridAesGcmDecrypt(
          key,
          envelope,
          ndn::span<const uint8_t>(ad.data(), ad.size()),
          plaintext)) {
      NDN_LOG_WARN("GS_RECORDING_DECRYPT_FAILED object=" << objectName);
      return {};
    }
    return std::vector<uint8_t>(plaintext.begin(), plaintext.end());
  }

  void
  publishStatus(const std::string& value)
  {
    NDN_LOG_INFO("GS_STATUS " << value);
    if (m_statusCallback) {
      m_statusCallback(value);
    }
  }

  void
  publishUiOnlyStatus(const std::string& value)
  {
    if (m_statusCallback) {
      m_statusCallback(value);
    }
  }

  void
  postRequest(const ndn::Name& service, const std::string& payload,
              std::function<void(std::string)> onSuccess,
              std::function<bool()> ignoreTimeout = {},
              std::function<void()> onTimeout = {})
  {
    postRequestForDrone(targetDroneId(), service, payload, std::move(onSuccess),
                        std::move(ignoreTimeout), std::move(onTimeout));
  }

  void
  postRequestForDrone(const std::string& droneId,
              const ndn::Name& service, const std::string& payload,
              std::function<void(std::string)> onSuccess,
              std::function<bool()> ignoreTimeout = {},
              std::function<void()> onTimeout = {})
  {
    boost::asio::post(m_face.getIoContext(), [this, service, payload,
                                droneId,
                                onSuccess = std::move(onSuccess),
                                ignoreTimeout = std::move(ignoreTimeout),
                                onTimeout = std::move(onTimeout)] {
      if (!m_containerReady.load() || !m_user) {
        publishStatus("NDNSF runtime not ready for " + service.toUri());
        if (onTimeout) {
          onTimeout();
        }
        return;
      }
      auto requestMessage = makeRequest(payload);
      const auto requestStartMs = nowMilliseconds();
      m_user->RequestService(
        std::vector<ndn::Name>{droneIdentity(m_config, droneId)},
        service,
        std::move(requestMessage),
        m_ackTimeoutMs,
        ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
        m_timeoutMs,
        [this, service,
         ignoreTimeout = std::move(ignoreTimeout),
         onTimeout = std::move(onTimeout)](const ndn::Name&) {
          if (ignoreTimeout && ignoreTimeout()) {
            return;
          }
          if (onTimeout) {
            onTimeout();
          }
          publishStatus("Timeout waiting for " + service.toUri());
        },
        [this, onSuccess, service, droneId, requestStartMs](
          const ndn_service_framework::ResponseMessage& response) {
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_RESPONSE service=" << service << " payload=" << payloadText);
          publishUiOnlyStatus("Link drone=" + droneId +
                              " service=" + service.toUri() +
                              " rtt_ms=" +
                              std::to_string(nowMilliseconds() - requestStartMs));
          onSuccess(payloadText);
        });
    });
  }

  void
  postTargetedRequestBytes(const ndn::Name& provider,
                           const std::string& droneId,
                           const ndn::Name& service,
                           const std::string& payload,
                           std::function<void(std::vector<uint8_t>)> onSuccess,
                           std::function<void()> onTimeout = {})
  {
    boost::asio::post(m_face.getIoContext(), [this, provider, droneId, service, payload,
                                onSuccess = std::move(onSuccess),
                                onTimeout = std::move(onTimeout)] {
      if (!m_containerReady.load() || !m_user) {
        publishStatus("NDNSF runtime not ready for targeted " + service.toUri());
        if (onTimeout) {
          onTimeout();
        }
        return;
      }
      auto requestMessage = makeRequest(payload);
      const auto requestStartMs = nowMilliseconds();
      m_user->RequestServiceTargeted(
        provider,
        service,
        std::move(requestMessage),
        m_timeoutMs,
        [this, service, onTimeout = std::move(onTimeout)](const ndn::Name&) {
          if (onTimeout) {
            onTimeout();
          }
          publishStatus("Timeout waiting for targeted " + service.toUri());
        },
        [this, onSuccess, service, droneId, requestStartMs](
          const ndn_service_framework::ResponseMessage& response) {
          if (!response.getStatus()) {
            publishStatus("Targeted chunk fetch failed for " + service.toUri() +
                          ": " + response.getErrorInfo());
            return;
          }
          const auto payloadBuffer = response.getPayload();
          std::vector<uint8_t> payloadBytes(payloadBuffer.data(),
                                            payloadBuffer.data() + payloadBuffer.size());
          publishUiOnlyStatus("Link drone=" + droneId +
                              " service=" + service.toUri() +
                              " rtt_ms=" +
                              std::to_string(nowMilliseconds() - requestStartMs));
          onSuccess(std::move(payloadBytes));
        });
    });
  }

  void
  postTargetedRequest(const ndn::Name& provider, const ndn::Name& service,
                      const std::string& payload,
                      std::function<void(std::string)> onSuccess,
                      std::function<void()> onTimeout = {})
  {
    boost::asio::post(m_face.getIoContext(), [this, provider, service, payload,
                                onSuccess = std::move(onSuccess),
                                onTimeout = std::move(onTimeout)] {
      if (!m_containerReady.load() || !m_user) {
        publishStatus("NDNSF runtime not ready for targeted " + service.toUri());
        if (onTimeout) {
          onTimeout();
        }
        return;
      }
      auto requestMessage = makeRequest(payload);
      const auto requestStartMs = nowMilliseconds();
      m_user->RequestServiceTargeted(
        provider,
        service,
        std::move(requestMessage),
        m_timeoutMs,
        [this, service, onTimeout = std::move(onTimeout)](const ndn::Name&) {
          if (onTimeout) {
            onTimeout();
          }
          else {
            publishStatus("Timeout waiting for targeted " + service.toUri());
          }
        },
        [this, onSuccess, service, provider, requestStartMs](
          const ndn_service_framework::ResponseMessage& response) {
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_TARGETED_RESPONSE service=" << service
                       << " payload=" << payloadText);
          publishUiOnlyStatus("Link provider=" + provider.toUri() +
                              " service=" + service.toUri() +
                              " rtt_ms=" +
                              std::to_string(nowMilliseconds() - requestStartMs));
          onSuccess(payloadText);
        });
    });
  }

  struct PacketLane
  {
    std::string kind;
    uint64_t second = 0;
    uint64_t nextSeq = 0;
    uint64_t inFlight = 0;
    uint64_t futureInFlight = 0;
    uint64_t maxPacketsPerSecond = 0;
    uint64_t prefetchLimit = 0;
    uint64_t advertisedPackets = 0;
    uint64_t probeNotBeforeMs = 0;
  };

  struct StreamChunk
  {
    uint64_t packetSeq = 0;
    uint64_t arrivalMs = 0;
    uint64_t elapsedMs = 0;
    std::vector<uint8_t> payload;
  };

  struct FecFrameState
  {
    bool initialized = false;
    uint64_t frameSeq = 0;
    uint64_t frameFirstPacketSeq = 0;
    uint64_t frameLastPacketSeq = 0;
    uint32_t dataShards = 0;
    uint32_t parityShards = 0;
    uint32_t symbolCount = 0;
    uint64_t firstArrivalMs = 0;
    std::vector<size_t> fecDataLengths;
    std::map<uint32_t, std::vector<uint8_t>> shards;
    bool complete = false;
  };

  struct VideoBitrateAdvice
  {
    uint64_t requestedKbps = 0;
    uint64_t acceptedKbps = 0;
    uint64_t suggestedKbps = 0;
    std::string action = "hold";
    std::string reason = "stable";
  };

  void
  requestVideoPackets()
  {
    if (!m_streaming.load()) {
      return;
    }
    requestVideoLane(m_deltaLane, dynamicVideoWindow());
  }

  void
  scheduleVideoPump(uint64_t delayMs)
  {
    if (!m_streaming.load() || m_videoPumpScheduled.exchange(true)) {
      return;
    }
    m_videoPumpTimer.expires_after(std::chrono::milliseconds(delayMs));
    m_videoPumpTimer.async_wait([this] (const boost::system::error_code& ec) {
      m_videoPumpScheduled = false;
      if (!ec && m_streaming.load()) {
        requestVideoPackets();
      }
    });
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

  void
  configurePrefetch(const Fields& fields)
  {
    const auto bitrateKbps = std::max<uint64_t>(
      128, fieldAsUint64(fields, "accepted_bitrate_kbps",
                         fieldAsUint64(fields, "target_bitrate_kbps", m_videoBitrateKbps.load())));
    const auto requestedBitrateKbps = std::max<uint64_t>(
      128, fieldAsUint64(fields, "requested_bitrate_kbps", m_videoBitrateKbps.load()));
    const auto payloadBytes = std::max<uint64_t>(
      512, fieldAsUint64(fields, "max_payload_bytes", 3600));
    const auto fps = std::max<uint64_t>(1, fieldAsUint64(fields, "fps", VIDEO_FPS));
    const auto frameWidth = std::max<uint64_t>(
      1, fieldAsUint64(fields, "accepted_frame_width",
                       fieldAsUint64(fields, "frame_width", m_videoFrameWidth)));
    const auto bytesPerSecond = (bitrateKbps * 1000 + 7) / 8;
    const auto estimatedPacketsPerSecond =
      std::max<uint64_t>(fps, (bytesPerSecond + payloadBytes - 1) / payloadBytes);

    m_videoPayloadBytes = payloadBytes;
    m_videoFps = fps;
    m_videoRequestedBitrateKbps = requestedBitrateKbps;
    m_videoAcceptedBitrateKbps = bitrateKbps;
    m_keyPacketsPerSecond = std::clamp<uint64_t>(
      (estimatedPacketsPerSecond + 7) / 8, 4, 16);
    m_deltaPacketsPerSecond = std::clamp<uint64_t>(
      estimatedPacketsPerSecond + m_keyPacketsPerSecond + fps / 2, 24, 512);
    m_keyWindow = std::clamp<uint64_t>(m_keyPacketsPerSecond, 4, 16);
    m_videoTimeoutBudgetMs = std::clamp<uint64_t>(
      static_cast<uint64_t>(std::max(m_timeoutMs, 1)), 800, 6000);
    const auto frameMs = frameDurationMs();
    m_dynamicWindowMax = packetsForDurationMs(
      std::clamp<uint64_t>(m_videoTimeoutBudgetMs / 3 + 300, 650, 1400), 32, 640);
    m_dynamicLookaheadMax = packetsForDurationMs(
      std::clamp<uint64_t>(m_videoTimeoutBudgetMs / 8 + frameMs * 4, 180, 600), 8, 256);
    m_decoderReorderWindow = packetsForDurationMs(
      std::clamp<uint64_t>(frameMs * 4 + DEFAULT_VIDEO_RTT_MS / 2, 100, 280), 6, 160);
    m_decoderBacklogLimit = std::clamp<uint64_t>(m_decoderReorderWindow * 4, 32, 512);
    m_videoRttEwmaMs = DEFAULT_VIDEO_RTT_MS;
    resetVideoAdaptiveState();
    m_deltaWindow = dynamicVideoWindow();

    NDN_LOG_INFO("GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
                 << " frameWidth=" << frameWidth
                 << " payloadBytes=" << payloadBytes
                 << " fps=" << fps
                 << " keyBudget=" << m_keyPacketsPerSecond
                 << " deltaBudget=" << m_deltaPacketsPerSecond
                 << " keyWindow=" << m_keyWindow
                 << " deltaWindow=" << m_deltaWindow
                 << " lookaheadMax=" << m_dynamicLookaheadMax
                 << " reorderWindow=" << m_decoderReorderWindow
                 << " backlogLimit=" << m_decoderBacklogLimit
                 << " interestLifetimeMs=" << dynamicInterestLifetimeMs()
                 << " timeoutBudgetMs=" << m_videoTimeoutBudgetMs
                 << " missingTimeoutMs=" << dynamicDecoderMissingTimeoutMs()
                 << " rttMs=" << videoRttMs()
                 << " congestionPressure=" << videoCongestionPressurePercent()
                 << " probePressure=" << videoProbePressurePercent());
  }

  uint64_t
  videoRttMs() const
  {
    return std::clamp<uint64_t>(m_videoRttEwmaMs.load(), 20, 2000);
  }

  void
  recordVideoRtt(uint64_t sentMs, uint64_t receivedMs)
  {
    if (sentMs == 0 || receivedMs <= sentMs) {
      return;
    }
    const auto sample = std::clamp<uint64_t>(receivedMs - sentMs, 1, 3000);
    auto previous = m_videoRttEwmaMs.load();
    if (previous == 0) {
      previous = sample;
    }
    const auto updated = (previous * 7 + sample) / 8;
    m_videoRttEwmaMs = std::clamp<uint64_t>(updated, 20, 2000);
  }

  static void
  recordVideoPressure(std::atomic<uint64_t>& pressure, uint64_t sample)
  {
    const auto clamped = std::clamp<uint64_t>(sample, 0, 100);
    const auto previous = pressure.load();
    pressure = std::clamp<uint64_t>((previous * 7 + clamped) / 8, 0, 100);
  }

  void
  recordVideoDataReceived()
  {
    recordVideoPressure(m_videoTimeoutPressurePercent, 0);
    recordVideoPressure(m_videoProbePressurePercent, 0);
    recordVideoPressure(m_videoDuplicatePressurePercent, 0);
  }

  void
  recordVideoFutureProbeTimeout()
  {
    recordVideoPressure(m_videoProbePressurePercent, 100);
    recordVideoPressure(m_videoTimeoutPressurePercent, 20);
  }

  void
  recordVideoFetchTimeout()
  {
    recordVideoPressure(m_videoTimeoutPressurePercent, 100);
  }

  void
  recordVideoDuplicatePacket()
  {
    recordVideoPressure(m_videoDuplicatePressurePercent, 100);
    recordVideoPressure(m_videoProbePressurePercent, 60);
  }

  void
  resetVideoAdaptiveState()
  {
    m_videoTimeoutPressurePercent = 0;
    m_videoProbePressurePercent = 0;
    m_videoDuplicatePressurePercent = 0;
  }

  uint64_t
  packetsForDurationMs(uint64_t durationMs, uint64_t minValue, uint64_t maxValue) const
  {
    const auto packets = (m_deltaPacketsPerSecond * durationMs + 999) / 1000;
    return std::clamp<uint64_t>(packets, minValue, maxValue);
  }

  uint64_t
  frameDurationMs() const
  {
    return std::max<uint64_t>(1, 1000 / std::max<uint64_t>(1, m_videoFps));
  }

  uint64_t
  dynamicVideoWindow() const
  {
    return currentVideoAdaptivePolicyDecision().window;
  }

  uint64_t
  dynamicVideoLookahead() const
  {
    return currentVideoAdaptivePolicyDecision().lookahead;
  }

  uint64_t
  dynamicFutureProbeInFlightLimit() const
  {
    return currentVideoAdaptivePolicyDecision().futureProbeLimit;
  }

  uint64_t
  dynamicProbeBackoffMs() const
  {
    return currentVideoAdaptivePolicyDecision().probeBackoffMs;
  }

  uint64_t
  dynamicInterestLifetimeMs() const
  {
    return currentVideoAdaptivePolicyDecision().interestLifetimeMs;
  }

  uint64_t
  dynamicDecoderMissingTimeoutMs() const
  {
    return currentVideoAdaptivePolicyDecision().missingTimeoutMs;
  }

  uint64_t
  decoderBacklogPressurePercent() const
  {
    return currentVideoAdaptivePolicyDecision().backlogPressure;
  }

  uint64_t
  videoLossPressurePercent() const
  {
    return currentVideoAdaptivePolicyDecision().lossPressure;
  }

  uint64_t
  videoCongestionPressurePercent() const
  {
    return currentVideoAdaptivePolicyDecision().congestionPressure;
  }

  VideoBitrateAdvice
  videoBitrateAdvice() const
  {
    VideoBitrateAdvice advice;
    advice.requestedKbps = std::max<uint64_t>(128, m_videoRequestedBitrateKbps.load());
    advice.acceptedKbps = std::max<uint64_t>(128, m_videoAcceptedBitrateKbps.load());
    const auto decision = currentVideoAdaptivePolicyDecision();
    advice.suggestedKbps = decision.suggestedBitrateKbps;
    advice.action = decision.bitrateAction;
    advice.reason = decision.bitrateReason;
    return advice;
  }

  uint64_t
  videoProbePressurePercent() const
  {
    return currentVideoAdaptivePolicyDecision().probePressure;
  }

  VideoAdaptivePolicyInput
  currentVideoAdaptivePolicyInput() const
  {
    VideoAdaptivePolicyInput input;
    input.rttMs = videoRttMs();
    input.fps = m_videoFps;
    input.deltaPacketsPerSecond = m_deltaPacketsPerSecond;
    input.timeoutBudgetMs = m_videoTimeoutBudgetMs;
    input.dynamicWindowMax = m_dynamicWindowMax;
    input.dynamicLookaheadMax = m_dynamicLookaheadMax;
    input.decoderBacklogLimit = m_decoderBacklogLimit;
    input.decoderPendingChunks = m_decoderPendingChunkCount.load();
    input.receivedChunks = m_receivedChunks.load();
    input.timeouts = m_frameTimeouts.load();
    input.nacks = m_frameNacks.load();
    input.timeoutPressure = m_videoTimeoutPressurePercent.load();
    input.probePressure = m_videoProbePressurePercent.load();
    input.duplicatePressure = m_videoDuplicatePressurePercent.load();
    input.requestedBitrateKbps = m_videoRequestedBitrateKbps.load();
    input.acceptedBitrateKbps = m_videoAcceptedBitrateKbps.load();
    return input;
  }

  VideoAdaptivePolicyDecision
  currentVideoAdaptivePolicyDecision() const
  {
    return computeVideoAdaptivePolicy(currentVideoAdaptivePolicyInput());
  }

  VideoAdaptiveState
  currentVideoAdaptiveState(const std::string& droneId) const
  {
    VideoAdaptiveState state;
    const auto decision = currentVideoAdaptivePolicyDecision();
    state.droneId = droneId.empty() ? "unknown" : droneId;
    state.state = m_streaming.load() ? "streaming" : "stopped";
    state.rttMs = videoRttMs();
    state.requestedBitrateKbps = std::max<uint64_t>(128, m_videoRequestedBitrateKbps.load());
    state.acceptedBitrateKbps = std::max<uint64_t>(128, m_videoAcceptedBitrateKbps.load());
    state.suggestedBitrateKbps = decision.suggestedBitrateKbps;
    state.bitrateAction = decision.bitrateAction;
    state.bitrateReason = decision.bitrateReason;
    state.window = decision.window;
    state.lookahead = decision.lookahead;
    state.futureProbeLimit = decision.futureProbeLimit;
    state.interestLifetimeMs = decision.interestLifetimeMs;
    state.missingTimeoutMs = decision.missingTimeoutMs;
    state.timeoutPressure = m_videoTimeoutPressurePercent.load();
    state.probePressure = decision.probePressure;
    state.duplicatePressure = m_videoDuplicatePressurePercent.load();
    state.lossPressure = decision.lossPressure;
    state.backlogPressure = decision.backlogPressure;
    state.primaryPressure = decision.primaryPressure;
    state.policyReason = decision.policyReason;
    state.pendingChunks = m_decoderPendingChunkCount.load();
    state.receivedChunks = m_receivedChunks.load();
    state.timeouts = m_frameTimeouts.load();
    state.nacks = m_frameNacks.load();
    state.duplicates = m_duplicateVideoPackets.load();
    state.decodedFrames = m_decodedVideoFrames.load();
    state.updatedMs = nowMilliseconds();
    return state;
  }

  void
  publishVideoAdaptiveState(const std::string& reason, bool force = false)
  {
    const auto droneId = activeVideoDroneId();
    if (droneId.empty()) {
      return;
    }
    const auto nowMs = nowMilliseconds();
    const auto lastLogMs = m_lastVideoAdaptiveLogMs.load();
    if (!force && lastLogMs != 0 && nowMs < lastLogMs + 500) {
      const auto state = currentVideoAdaptiveState(droneId);
      std::lock_guard<std::mutex> guard(m_telemetryMutex);
      m_videoAdaptiveByDrone[droneId] = state;
      return;
    }
    m_lastVideoAdaptiveLogMs = nowMs;
    const auto state = currentVideoAdaptiveState(droneId);
    {
      std::lock_guard<std::mutex> guard(m_telemetryMutex);
      m_videoAdaptiveByDrone[droneId] = state;
    }
    NDN_LOG_INFO("GS_VIDEO_ADAPTIVE_STATE reason=" << reason << " " << state.statusLine());
    publishStatus(state.statusLine());
    maybeApplyVideoBitratePolicy(state, reason);
  }

  void
  requestVideoLane(PacketLane& lane, uint64_t window)
  {
    advanceLaneIfStale(lane);
    while (m_streaming.load() && lane.inFlight < window) {
      advanceLaneIfStale(lane);
      const auto highWaterLimit = lane.advertisedPackets == 0 ?
        INITIAL_PACKET_PROBE :
        lane.advertisedPackets + dynamicVideoLookahead();
      if (lane.prefetchLimit == 0 &&
          lane.nextSeq >= highWaterLimit) {
        if (lane.inFlight == 0 && lane.advertisedPackets > 0) {
          lane.nextSeq = lane.advertisedPackets;
        }
        scheduleVideoPump(STREAM_PUMP_INTERVAL_MS);
        break;
      }
      if (lane.nextSeq >= lane.advertisedPackets &&
          lane.advertisedPackets > 0 &&
          lane.futureInFlight >= dynamicFutureProbeInFlightLimit()) {
        scheduleVideoPump(dynamicProbeBackoffMs());
        break;
      }
      if (lane.probeNotBeforeMs > 0 &&
          nowMilliseconds() < lane.probeNotBeforeMs &&
          lane.nextSeq >= lane.advertisedPackets) {
        scheduleVideoPump(dynamicProbeBackoffMs());
        break;
      }
      const auto packetSeq = lane.nextSeq++;
      if (!reserveVideoPacketFetch(packetSeq)) {
        continue;
      }
      ++lane.inFlight;
      const auto sentMs = nowMilliseconds();
      const auto advertisedAtSend = lane.advertisedPackets;
      const auto futureProbeAtSend =
        advertisedAtSend == 0 ||
        packetSeq >= advertisedAtSend ||
        isBeyondHighestReceivedVideoPacket(packetSeq);
      if (futureProbeAtSend) {
        ++lane.futureInFlight;
      }
      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packetSeq));
      auto interest = ndn::Interest(packetName);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(ndn::time::milliseconds(dynamicInterestLifetimeMs()));

      m_face.expressInterest(
        interest,
        [this, &lane, packetSeq, sentMs, advertisedAtSend, futureProbeAtSend](const ndn::Interest&, const ndn::Data& data) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          if (futureProbeAtSend && lane.futureInFlight > 0) {
            --lane.futureInFlight;
          }
          releaseVideoPacketFetch(packetSeq);
          lane.probeNotBeforeMs = 0;
          advanceLaneIfStale(lane);
        const auto receivedMs = nowMilliseconds();
        if (advertisedAtSend > packetSeq) {
          recordVideoRtt(sentMs, receivedMs);
        }
        if (m_firstFrameMs == 0) {
          m_firstFrameMs = receivedMs;
        }
        const auto content = data.getContent();
        std::vector<uint8_t> bytes(content.value(), content.value() + content.value_size());
          try {
            const auto packet = decodeVideoPacket(bytes);
            if (!markVideoPacketCompleted(packet.packetSeq)) {
              recordVideoDuplicatePacket();
              const auto duplicateCount = ++m_duplicateVideoPackets;
              if (duplicateCount <= 3 || duplicateCount % 30 == 0) {
                NDN_LOG_INFO("GS_VIDEO_DUPLICATE_PACKET count=" << duplicateCount
                             << " packetSeq=" << packet.packetSeq
                             << " requestedSeq=" << packetSeq);
              }
              requestVideoPackets();
              return;
            }
            recordVideoDataReceived();
            const auto receivedCount = ++m_receivedChunks;
            if (receivedCount <= 3 || receivedCount % 30 == 0) {
              NDN_LOG_INFO("GS_VIDEO_CHUNK count=" << receivedCount
                           << " packetSeq=" << packet.packetSeq
                           << " requestedSeq=" << packetSeq
                           << " name=" << data.getName()
                           << " bytes=" << data.getContent().value_size()
                           << " rttMs=" << videoRttMs()
                           << " lossPressure=" << videoLossPressurePercent()
                           << " congestionPressure=" << videoCongestionPressurePercent()
                           << " probePressure=" << videoProbePressurePercent()
                           << " futureInFlight=" << lane.futureInFlight
                           << " futureLimit=" << dynamicFutureProbeInFlightLimit()
                           << " backlogPressure=" << decoderBacklogPressurePercent()
                           << " interestLifetimeMs=" << dynamicInterestLifetimeMs()
                           << " missingTimeoutMs=" << dynamicDecoderMissingTimeoutMs()
                           << " window=" << dynamicVideoWindow());
            }
            publishVideoAdaptiveState("chunk");
            updateHighestReceivedVideoPacket(packet.packetSeq);
            updateLaneHighWatermark(lane, packet);
            queueStreamChunk(packet, receivedMs);
          }
          catch (const std::exception& e) {
            NDN_LOG_WARN("GS_VIDEO_PACKET_DECODE_FAILED " << e.what());
          }
          requestVideoPackets();
      },
        [this, &lane, packetSeq, futureProbeAtSend](const ndn::Interest&, const ndn::lp::Nack&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          if (futureProbeAtSend && lane.futureInFlight > 0) {
            --lane.futureInFlight;
          }
          releaseVideoPacketFetch(packetSeq);
          recordVideoFetchTimeout();
          const auto nackCount = ++m_frameNacks;
          if (nackCount <= 3 || nackCount % 30 == 0) {
            NDN_LOG_INFO("GS_VIDEO_NACK count=" << nackCount
                         << " packetSeq=" << packetSeq
                         << " congestionPressure=" << videoCongestionPressurePercent()
                         << " probePressure=" << videoProbePressurePercent());
          }
          publishVideoAdaptiveState("nack");
          advanceLaneIfStale(lane);
          requestVideoPackets();
      },
        [this, &lane, packetSeq, futureProbeAtSend](const ndn::Interest&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          if (futureProbeAtSend && lane.futureInFlight > 0) {
            --lane.futureInFlight;
          }
          releaseVideoPacketFetch(packetSeq);
          const bool isFutureProbe =
            futureProbeAtSend ||
            packetSeq >= lane.advertisedPackets ||
            isBeyondHighestReceivedVideoPacket(packetSeq);
          if (isFutureProbe && lane.nextSeq > packetSeq) {
            recordVideoFutureProbeTimeout();
            NDN_LOG_DEBUG("GS_VIDEO_FUTURE_PROBE_TIMEOUT packetSeq=" << packetSeq
                          << " advertisedPackets=" << lane.advertisedPackets
                          << " highestReceived=" << m_highestReceivedVideoPacketSeq.load()
                          << " probePressure=" << videoProbePressurePercent()
                          << " futureLimit=" << dynamicFutureProbeInFlightLimit()
                          << " backoffMs=" << dynamicProbeBackoffMs());
            lane.nextSeq = packetSeq;
            lane.probeNotBeforeMs = nowMilliseconds() + dynamicProbeBackoffMs();
            publishVideoAdaptiveState("future-probe-timeout");
          }
          else {
            recordVideoFetchTimeout();
            const auto timeoutCount = ++m_frameTimeouts;
            if (timeoutCount <= 3 || timeoutCount % 30 == 0) {
              NDN_LOG_INFO("GS_VIDEO_TIMEOUT count=" << timeoutCount
                           << " packetSeq=" << packetSeq
                           << " congestionPressure=" << videoCongestionPressurePercent()
                           << " probePressure=" << videoProbePressurePercent());
            }
            publishVideoAdaptiveState("timeout");
          }
          advanceLaneIfStale(lane);
          scheduleVideoPump(dynamicProbeBackoffMs());
          requestVideoPackets();
      });
    }
  }

  void
  advanceLaneIfStale(PacketLane& lane)
  {
    const auto currentSecond = nowMilliseconds() / 1000;
    if (lane.second == 0) {
      return;
    }
    if (lane.prefetchLimit > 0 && currentSecond >= lane.second) {
      lane.prefetchLimit = 0;
    }
    if (currentSecond > lane.second + 1 ||
        (currentSecond > lane.second &&
         lane.maxPacketsPerSecond > 0 &&
         lane.nextSeq >= lane.maxPacketsPerSecond &&
         lane.inFlight == 0)) {
      lane.second = currentSecond;
      lane.nextSeq = 0;
      lane.inFlight = 0;
      lane.futureInFlight = 0;
      lane.prefetchLimit = 0;
      lane.advertisedPackets = 0;
      lane.probeNotBeforeMs = 0;
    }
  }

  void
  updateLaneHighWatermark(PacketLane& lane, const VideoPacket& packet)
  {
    lane.nextSeq = std::max(lane.nextSeq, packet.packetSeq + 1);
    lane.advertisedPackets = std::max(lane.advertisedPackets, packet.bucketPacketCount);
  }

  void
  resetVideoPacketTracking()
  {
    std::lock_guard<std::mutex> guard(m_videoPacketTrackingMutex);
    m_videoInFlightPacketSeqs.clear();
    m_videoCompletedPacketSeqs.clear();
    m_videoCompletedPacketSeqOrder.clear();
  }

  bool
  reserveVideoPacketFetch(uint64_t packetSeq)
  {
    if (packetSeq == UINT64_MAX) {
      return false;
    }

    std::lock_guard<std::mutex> guard(m_videoPacketTrackingMutex);
    if (m_videoCompletedPacketSeqs.find(packetSeq) != m_videoCompletedPacketSeqs.end() ||
        m_videoInFlightPacketSeqs.find(packetSeq) != m_videoInFlightPacketSeqs.end()) {
      return false;
    }
    m_videoInFlightPacketSeqs.insert(packetSeq);
    return true;
  }

  void
  releaseVideoPacketFetch(uint64_t packetSeq)
  {
    if (packetSeq == UINT64_MAX) {
      return;
    }

    std::lock_guard<std::mutex> guard(m_videoPacketTrackingMutex);
    m_videoInFlightPacketSeqs.erase(packetSeq);
  }

  bool
  markVideoPacketCompleted(uint64_t packetSeq)
  {
    if (packetSeq == UINT64_MAX) {
      return false;
    }

    std::lock_guard<std::mutex> guard(m_videoPacketTrackingMutex);
    m_videoInFlightPacketSeqs.erase(packetSeq);
    if (m_videoCompletedPacketSeqs.find(packetSeq) != m_videoCompletedPacketSeqs.end()) {
      return false;
    }

    m_videoCompletedPacketSeqs.insert(packetSeq);
    m_videoCompletedPacketSeqOrder.push_back(packetSeq);
    while (m_videoCompletedPacketSeqOrder.size() > MAX_VIDEO_PACKET_HISTORY) {
      m_videoCompletedPacketSeqs.erase(m_videoCompletedPacketSeqOrder.front());
      m_videoCompletedPacketSeqOrder.pop_front();
    }
    return true;
  }

  void
  updateHighestReceivedVideoPacket(uint64_t packetSeq)
  {
    if (packetSeq == UINT64_MAX) {
      return;
    }
    auto current = m_highestReceivedVideoPacketSeq.load();
    while ((current == UINT64_MAX || packetSeq > current) &&
           !m_highestReceivedVideoPacketSeq.compare_exchange_weak(current, packetSeq)) {
    }
  }

  bool
  isBeyondHighestReceivedVideoPacket(uint64_t packetSeq) const
  {
    const auto highest = m_highestReceivedVideoPacketSeq.load();
    return highest == UINT64_MAX || packetSeq > highest;
  }

  void
  queueStreamChunk(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (!m_decoderRunning.load() || packet.payload.empty()) {
      return;
    }

    if (packet.fecDataShards > 0 || packet.fecParityShards > 0 || packet.fecSymbolCount > 0) {
      processFecChunk(packet, receivedMs);
      return;
    }

    if (packet.packetSeq == UINT64_MAX) {
      return;
    }

    const auto elapsedMs = (m_firstFrameMs == 0 ? 0 : receivedMs - m_firstFrameMs);
    insertChunkForDecode(packet.packetSeq, packet.payload, elapsedMs);
  }

  void
  processFecChunk(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (!m_decoderRunning.load() || packet.payload.empty() ||
        packet.fecSymbolCount == 0 || packet.fecDataShards == 0) {
      return;
    }

    const auto frameSeq = packet.frameSeq;
    auto& state = m_fecFrames[frameSeq];

    if (!state.initialized) {
      state.frameSeq = frameSeq;
      state.frameFirstPacketSeq = packet.frameFirstPacketSeq;
      state.frameLastPacketSeq = packet.frameLastPacketSeq;
      state.dataShards = packet.fecDataShards;
      state.parityShards = packet.fecParityShards;
      state.symbolCount = packet.fecSymbolCount;
      state.fecDataLengths = parseFecDataLengths(packet.fecDataLengths);
      state.firstArrivalMs = receivedMs;
      state.initialized = true;
    }

    state.dataShards = std::max<uint32_t>(state.dataShards, packet.fecDataShards);
    state.parityShards = std::max<uint32_t>(state.parityShards, packet.fecParityShards);
    state.symbolCount = std::max<uint32_t>(state.symbolCount, packet.fecSymbolCount);
    state.frameLastPacketSeq =
      packet.frameLastPacketSeq != 0 ?
      packet.frameLastPacketSeq :
      (packet.frameFirstPacketSeq + state.symbolCount - 1);
    if (state.fecDataLengths.empty() && !packet.fecDataLengths.empty()) {
      state.fecDataLengths = parseFecDataLengths(packet.fecDataLengths);
    }

    if (packet.fecSymbolIndex < packet.fecSymbolCount) {
      state.shards.try_emplace(packet.fecSymbolIndex, packet.payload);
    }

    const auto elapsedMs = (m_firstFrameMs == 0 ? 0 : receivedMs - m_firstFrameMs);
    attemptAndRecoverFrame(state);
    if (packet.fecSymbolIndex < state.dataShards) {
      insertChunkForDecode(packet.packetSeq, packet.payload, elapsedMs);
    }
    if (state.complete) {
      cleanupFecFrames();
    }
  }

  void
  attemptAndRecoverFrame(FecFrameState& state)
  {
    if (state.complete || state.dataShards == 0 || state.symbolCount == 0) {
      return;
    }

    uint32_t receivedDataShards = 0;
    for (uint32_t i = 0; i < state.dataShards; ++i) {
      if (state.shards.find(i) != state.shards.end()) {
        ++receivedDataShards;
      }
    }

    if (receivedDataShards == state.dataShards) {
      state.complete = true;
      return;
    }

    if (receivedDataShards + state.parityShards < state.dataShards) {
      return;
    }

    if (state.dataShards - receivedDataShards != 1) {
      return;
    }

    for (uint32_t missingIdx = 0; missingIdx < state.dataShards; ++missingIdx) {
      if (state.shards.find(missingIdx) != state.shards.end()) {
        continue;
      }
      const auto recovered = recoverFecDataSymbol(state, missingIdx);
      if (recovered.empty()) {
        return;
      }
      const auto recoveredSeq = state.frameFirstPacketSeq + missingIdx;
      if (!markVideoPacketCompleted(recoveredSeq)) {
        return;
      }
      const auto recoveredElapsed = (m_firstFrameMs == 0 ? 0 : state.firstArrivalMs - m_firstFrameMs);
      insertChunkForDecode(recoveredSeq, recovered, recoveredElapsed);
      state.shards[missingIdx] = recovered;
      state.complete = true;
      break;
    }
  }

  std::vector<size_t>
  parseFecDataLengths(const std::string& value)
  {
    std::vector<size_t> lengths;
    if (value.empty()) {
      return lengths;
    }

    std::stringstream parser(value);
    std::string token;
    while (std::getline(parser, token, ',')) {
      if (token.empty()) {
        continue;
      }
      try {
        lengths.push_back(std::stoull(token));
      }
      catch (const std::exception&) {
      }
    }
    return lengths;
  }

  std::vector<uint8_t>
  recoverFecDataSymbol(const FecFrameState& state, uint32_t missingIdx)
  {
    if (missingIdx >= state.dataShards || state.fecDataLengths.empty()) {
      return {};
    }
    if (missingIdx >= state.fecDataLengths.size()) {
      return {};
    }

    const auto targetLen = state.fecDataLengths[missingIdx];
    if (targetLen == 0) {
      return {};
    }

    std::vector<uint8_t> recovered(targetLen, 0);
    bool usedParity = false;
    for (uint32_t i = 0; i < state.symbolCount; ++i) {
      if (i == missingIdx) {
        continue;
      }
      const auto it = state.shards.find(i);
      if (it == state.shards.end()) {
        continue;
      }
      const auto& payload = it->second;
      for (size_t j = 0; j < targetLen; ++j) {
        const auto byte = (j < payload.size()) ? payload[j] : 0;
        recovered[j] ^= byte;
      }
      if (i >= state.dataShards) {
        usedParity = true;
      }
    }

    if (!usedParity) {
      return {};
    }
    return recovered;
  }

  void
  cleanupFecFrames()
  {
    for (auto it = m_fecFrames.begin(); it != m_fecFrames.end();) {
      if (it->second.complete ||
          (it->second.frameLastPacketSeq != 0 &&
           it->second.frameLastPacketSeq < m_nextChunkSeqToDecode)) {
        it = m_fecFrames.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void
  insertChunkForDecode(uint64_t packetSeq, const std::vector<uint8_t>& payload,
                      uint64_t elapsedMs)
  {
    if (packetSeq == UINT64_MAX) {
      return;
    }
    if (packetSeq < m_nextChunkSeqToDecode) {
      ++m_decoderDroppedChunks;
      return;
    }

    bool notifyWriter = false;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      const auto inserted = m_pendingChunks.emplace(packetSeq, StreamChunk{});
      if (inserted.second) {
        StreamChunk chunk;
        chunk.packetSeq = packetSeq;
        chunk.arrivalMs = (m_firstFrameMs == 0 ? 0 : m_firstFrameMs + elapsedMs);
        chunk.elapsedMs = elapsedMs;
        chunk.payload = payload;
        inserted.first->second = std::move(chunk);
      }

      while (!m_pendingChunks.empty()) {
        auto it = m_pendingChunks.find(m_nextChunkSeqToDecode);
        if (it == m_pendingChunks.end()) {
          break;
        }
        m_chunkQueue.push_back(std::move(it->second));
        m_pendingChunks.erase(it);
        ++m_nextChunkSeqToDecode;
        notifyWriter = true;
      }

      if (m_pendingChunks.size() > m_decoderBacklogLimit &&
          !m_pendingChunks.empty()) {
        auto first = m_pendingChunks.begin();
        if (first->first > m_nextChunkSeqToDecode) {
          NDN_LOG_DEBUG("GS_VIDEO_SKIP_MISSING_CHUNKS start="
                       << m_nextChunkSeqToDecode << " to=" << first->first - 1);
          m_decoderDroppedChunks += (first->first - m_nextChunkSeqToDecode);
          m_nextChunkSeqToDecode = first->first;
        }
      }

      if (m_pendingChunks.empty() || m_pendingChunks.begin()->first == m_nextChunkSeqToDecode) {
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }
      else if (m_decoderMissingChunkSeq != m_nextChunkSeqToDecode) {
        m_decoderMissingChunkSeq = m_nextChunkSeqToDecode;
        m_decoderMissingChunkStartMs = nowMilliseconds();
      }
      m_decoderPendingChunkCount = m_pendingChunks.size();
    }

    if (notifyWriter) {
      m_decoderQueueCv.notify_one();
    }
  }

  void
  startDecoder()
  {
    if (m_decoderRunning.load()) {
      return;
    }
    std::string command =
      "ffmpeg -hide_banner -loglevel error -fflags nobuffer -flags low_delay "
      "-analyzeduration 100000 -probesize 32768 -f h264 -i pipe:0 -f image2pipe -vcodec mjpeg -";

    if (!startDecoderProcess(command)) {
      publishStatus("Failed to start video decoder");
      return;
    }

    m_decoderRunning = true;
    m_lastOutputChunkSeq = 0;
    m_lastOutputChunkElapsedMs = 0;
    m_decoderDroppedChunks = 0;
    m_decoderMissingChunkSeq = UINT64_MAX;
    m_decoderMissingChunkStartMs = 0;
    m_closeDecoderInputWhenQueueDrained = false;
    m_decoderOutBuffer.clear();
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_pendingChunks.clear();
      m_decoderPendingChunkCount = 0;
      m_nextChunkSeqToDecode = 0;
    }

    m_decoderWriterThread = std::thread([this] { decoderWriterLoop(); });
    m_decoderReaderThread = std::thread([this] { decoderReaderLoop(); });
    publishStatus("Video decoder started");
  }

  void
  decodeRecordingFromFetchedChunksAsync(RecordingDataProductState manifest)
  {
    std::map<uint64_t, std::vector<uint8_t>> chunks;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      chunks = m_recordingPlaybackChunks;
    }
    if (chunks.empty()) {
      return;
    }

    if (m_recordingPlaybackDecodeThread.joinable()) {
      m_recordingPlaybackDecodeThread.join();
    }
    m_recordingPlaybackDecodeThread =
      std::thread([this, manifest = std::move(manifest), chunks = std::move(chunks)]() mutable {
      const auto tempPath = "/tmp/ndnsf-uav-recording-playback-" +
        std::to_string(getpid()) + "-" + std::to_string(nowMilliseconds()) + ".h264";
      {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
          publishStatus("Recording playback temp file failed drone=" + manifest.droneId);
          return;
        }
        for (const auto& [index, payload] : chunks) {
          (void)index;
          out.write(reinterpret_cast<const char*>(payload.data()),
                    static_cast<std::streamsize>(payload.size()));
        }
      }

      const auto command =
        "ffmpeg -hide_banner -loglevel error -f h264 -i " + shellQuote(tempPath) +
        " -f image2pipe -vcodec mjpeg -";
      std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
      if (!pipe) {
        publishStatus("Recording playback decoder failed drone=" + manifest.droneId);
        unlink(tempPath.c_str());
        return;
      }

      std::vector<uint8_t> outBuffer;
      std::array<uint8_t, 8192> buffer{};
      uint64_t frameCount = 0;
      while (m_recordingPlaybackActive.load() &&
             activeRecordingPlaybackDroneId() == manifest.droneId) {
        const auto n = fread(buffer.data(), 1, buffer.size(), pipe.get());
        if (n == 0) {
          break;
        }
        outBuffer.insert(outBuffer.end(), buffer.begin(), buffer.begin() + n);

        static constexpr uint8_t kJpegStart[2] = {0xff, 0xd8};
        static constexpr uint8_t kJpegEnd[2] = {0xff, 0xd9};
        while (outBuffer.size() >= 4) {
          const auto start = std::search(outBuffer.begin(), outBuffer.end(),
                                         std::begin(kJpegStart), std::end(kJpegStart));
          if (start == outBuffer.end()) {
            outBuffer.clear();
            break;
          }
          if (start != outBuffer.begin()) {
            outBuffer.erase(outBuffer.begin(), start);
            if (outBuffer.size() < 4) {
              break;
            }
          }
          const auto end = std::search(outBuffer.begin() + 2, outBuffer.end(),
                                       std::begin(kJpegEnd), std::end(kJpegEnd));
          if (end == outBuffer.end()) {
            break;
          }
          const auto endIt = end + 2;
          std::vector<uint8_t> frame(outBuffer.begin(), endIt);
          outBuffer.erase(outBuffer.begin(), endIt);
          {
            std::lock_guard<std::mutex> guard(m_latestDecodedFrameMutex);
            m_latestDecodedFrame = frame;
          }
          ++frameCount;
          if (m_frameCallback) {
            m_frameCallback(std::move(frame), frameCount, frameCount * 33);
          }
          std::this_thread::sleep_for(33ms);
        }
      }
      unlink(tempPath.c_str());
    });
  }

  void
  closeDecoderInput()
  {
    if (m_decoderInFd >= 0) {
      shutdown(m_decoderInFd, SHUT_WR);
      close(m_decoderInFd);
      m_decoderInFd = -1;
    }
  }

  void
  stopDecoder()
  {
    m_decoderRunning = false;
    m_decoderQueueCv.notify_all();

    closeDecoderInput();
    if (m_decoderOutFd >= 0) {
      close(m_decoderOutFd);
      m_decoderOutFd = -1;
    }
    if (m_decoderWriterThread.joinable()) {
      m_decoderWriterThread.join();
    }
    if (m_decoderReaderThread.joinable()) {
      m_decoderReaderThread.join();
    }

    if (m_decoderPid > 0) {
      kill(m_decoderPid, SIGTERM);
      waitpid(m_decoderPid, nullptr, 0);
      m_decoderPid = -1;
    }

    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_pendingChunks.clear();
      m_decoderPendingChunkCount = 0;
      m_decoderOutBuffer.clear();
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
    }
    m_decoderDroppedChunks = 0;
  }

  void
  decoderWriterLoop()
  {
    while (m_decoderRunning.load()) {
      StreamChunk chunk;
      {
        std::unique_lock<std::mutex> guard(m_decoderQueueMutex);
        m_decoderQueueCv.wait_for(guard, std::chrono::milliseconds(10), [this] {
          return !m_decoderRunning.load() ||
                 !m_chunkQueue.empty() ||
                 shouldAdvanceMissingChunk();
        });

        if (!m_decoderRunning.load()) {
          return;
        }

        const auto nowMs = nowMilliseconds();
        advanceMissingChunkUnderTimeout(nowMs);
        if (m_chunkQueue.empty()) {
          if (m_closeDecoderInputWhenQueueDrained.exchange(false)) {
            closeDecoderInput();
          }
          continue;
        }
        chunk = std::move(m_chunkQueue.front());
        m_chunkQueue.pop_front();
      }

      if (m_decoderInFd < 0) {
        continue;
      }

      if (chunk.payload.empty()) {
        continue;
      }
      m_lastOutputChunkSeq = chunk.packetSeq;
      m_lastOutputChunkElapsedMs = chunk.elapsedMs;

      const auto* data = chunk.payload.data();
      auto remaining = chunk.payload.size();
      while (remaining > 0 && m_decoderRunning.load()) {
        const auto n = write(m_decoderInFd, data, remaining);
        if (n > 0) {
          remaining -= static_cast<size_t>(n);
          data += n;
          continue;
        }
        if (errno == EINTR) {
          continue;
        }
        m_decoderRunning = false;
        return;
      }
    }
  }

  void
  decoderReaderLoop()
  {
    std::vector<uint8_t> buffer(8192);
    while (m_decoderRunning.load()) {
      const auto n = read(m_decoderOutFd, buffer.data(), buffer.size());
      if (n <= 0) {
        if (errno == EINTR) {
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      {
        std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
        m_decoderOutBuffer.insert(m_decoderOutBuffer.end(), buffer.data(), buffer.data() + n);
      }
      emitDecodedFramesFromBuffer();
    }
  }

  void
  emitDecodedFramesFromBuffer()
  {
    std::vector<std::vector<uint8_t>> frameCandidates;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      static constexpr uint8_t kJpegStart[2] = {0xff, 0xd8};
      static constexpr uint8_t kJpegEnd[2] = {0xff, 0xd9};

      while (m_decoderOutBuffer.size() >= 4) {
        const auto start = std::search(
          m_decoderOutBuffer.begin(), m_decoderOutBuffer.end(), std::begin(kJpegStart), std::end(kJpegStart));
        if (start == m_decoderOutBuffer.end()) {
          m_decoderOutBuffer.clear();
          break;
        }
        if (start != m_decoderOutBuffer.begin()) {
          m_decoderOutBuffer.erase(m_decoderOutBuffer.begin(), start);
          if (m_decoderOutBuffer.size() < 4) {
            break;
          }
        }

        const auto end = std::search(
          m_decoderOutBuffer.begin() + 2, m_decoderOutBuffer.end(),
          std::begin(kJpegEnd), std::end(kJpegEnd));
        if (end == m_decoderOutBuffer.end()) {
          break;
        }
        const auto endIt = end + 2;
        if (endIt > m_decoderOutBuffer.end()) {
          break;
        }
        frameCandidates.emplace_back(m_decoderOutBuffer.begin(), endIt);
        m_decoderOutBuffer.erase(m_decoderOutBuffer.begin(), endIt);
      }
    }

    for (auto& frame : frameCandidates) {
      {
        std::lock_guard<std::mutex> guard(m_latestDecodedFrameMutex);
        m_latestDecodedFrame = frame;
      }
      const auto decoded = ++m_decodedVideoFrames;
      if (decoded <= 3 || decoded % 30 == 0) {
        publishVideoAdaptiveState("decoded");
      }
      if (m_frameCallback) {
        m_frameCallback(std::move(frame), m_lastOutputChunkSeq, m_lastOutputChunkElapsedMs);
      }
    }
  }

  bool
  shouldAdvanceMissingChunk()
  {
    if (m_decoderMissingChunkSeq == UINT64_MAX || !m_decoderRunning.load()) {
      return false;
    }
    const auto nowMs = nowMilliseconds();
    return nowMs >= m_decoderMissingChunkStartMs + dynamicDecoderMissingTimeoutMs();
  }

  void
  advanceMissingChunkUnderTimeout(uint64_t nowMs)
  {
    if (m_decoderMissingChunkSeq == UINT64_MAX || m_pendingChunks.empty() ||
        m_decoderRunning.load() == false) {
      return;
    }

    const auto now = nowMs;
    const auto first = m_pendingChunks.begin();
    if (first->first <= m_nextChunkSeqToDecode) {
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
      return;
    }

    if (first->first > m_nextChunkSeqToDecode &&
        now >= m_decoderMissingChunkStartMs + dynamicDecoderMissingTimeoutMs()) {
      NDN_LOG_DEBUG("GS_VIDEO_SKIP_MISSING_CHUNKS_TIMEOUT start=" << m_decoderMissingChunkSeq
                     << " to=" << first->first - 1
                     << " timeoutMs=" << dynamicDecoderMissingTimeoutMs()
                     << " nowMs=" << now);
      m_decoderDroppedChunks += (first->first - m_nextChunkSeqToDecode);
      m_nextChunkSeqToDecode = first->first;
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;

      while (!m_pendingChunks.empty()) {
        auto it = m_pendingChunks.find(m_nextChunkSeqToDecode);
        if (it == m_pendingChunks.end()) {
          m_decoderMissingChunkSeq = m_nextChunkSeqToDecode;
          m_decoderMissingChunkStartMs = nowMs;
          break;
        }
        m_chunkQueue.push_back(std::move(it->second));
        m_pendingChunks.erase(it);
        ++m_nextChunkSeqToDecode;
      }
      if (m_pendingChunks.empty() ||
          (!m_pendingChunks.empty() && m_pendingChunks.begin()->first == m_nextChunkSeqToDecode)) {
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }
      m_decoderPendingChunkCount = m_pendingChunks.size();
      m_decoderQueueCv.notify_one();
    }
  }

  bool
  startDecoderProcess(const std::string& command)
  {
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
      NDN_LOG_WARN("GS_VIDEO_PIPE_ERROR errno=" << errno);
      return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
      NDN_LOG_WARN("GS_VIDEO_DECODER_FORK_FAILED errno=" << errno);
      return false;
    }

    if (pid == 0) {
      dup2(inPipe[0], STDIN_FILENO);
      dup2(outPipe[1], STDOUT_FILENO);
      dup2(outPipe[1], STDERR_FILENO);

      close(inPipe[0]);
      close(inPipe[1]);
      close(outPipe[0]);
      close(outPipe[1]);
      execl("/bin/sh", "/bin/sh", "-c", command.c_str(), (char*)nullptr);
      _exit(1);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    m_decoderPid = pid;
    m_decoderInFd = inPipe[1];
    m_decoderOutFd = outPipe[0];
    return true;
  }

private:
  bool m_serveCertificates;
  UavRuntimeConfig m_config;
  int m_ackTimeoutMs;
  int m_timeoutMs;
  std::string m_targetDroneId;
  mutable std::mutex m_targetMutex;
  mutable std::mutex m_missionReadyMutex;
  mutable std::mutex m_missionProgressMutex;
  mutable std::mutex m_videoStateMutex;
  mutable std::mutex m_recordingManifestMutex;
  std::vector<std::string> m_missionReadyDrones;
  MissionPlan m_latestMissionPlan;
  MissionProgressState m_latestMissionProgress;
  std::string m_activeVideoDroneId;
  std::string m_recordingPlaybackDroneId;
  std::map<std::string, RecordingDataProductState> m_recordingManifests;
  std::atomic<uint64_t> m_videoBitrateKbps{8000};
  uint64_t m_videoFrameWidth = 480;
  std::vector<std::string> m_patrolDroneIds;
  std::string m_yoloModel;
  std::string m_yoloScript;
  std::string m_yoloWorkerScript;
  uint64_t m_linkStaleMs = 3500;
  uint64_t m_linkLostMs = 8000;
  std::string m_lostLinkAction = "notify";
  std::string m_videoBitratePolicy = "manual";
  uint64_t m_videoBitrateAutoPressureMs = 2500;
  std::mutex m_yoloMutex;
  std::thread m_yoloPrewarmThread;
  pid_t m_yoloWorkerPid = -1;
  int m_yoloWorkerInFd = -1;
  int m_yoloWorkerOutFd = -1;
  std::mutex m_latestDecodedFrameMutex;
  std::vector<uint8_t> m_latestDecodedFrame;
  ndn::Face m_face;
  boost::asio::steady_timer m_videoPumpTimer;
  ndn::KeyChain m_keyChain;
  ndn::security::Certificate m_gsCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<ndn_service_framework::CertificatePublisher> m_certPublisher;
  std::unique_ptr<ndn_service_framework::ServiceUser> m_user;
  std::unique_ptr<ndn_service_framework::ServiceProvider> m_objectDetectionProvider;
  std::thread m_faceThread;
  std::function<void(std::string)> m_statusCallback;
  std::function<void(std::vector<uint8_t>, uint64_t, uint64_t)> m_frameCallback;
  std::atomic<bool> m_containerReady{false};
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_seenVideoStart{false};
  std::atomic<bool> m_videoStartInFlight{false};
  std::atomic<bool> m_videoStopInFlight{false};
  std::atomic<bool> m_videoStopDelayInjected{false};
  std::atomic<bool> m_recordingPlaybackActive{false};
  std::atomic<uint64_t> m_videoStartRetries{0};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
  std::atomic<uint64_t> m_highestReceivedVideoPacketSeq{UINT64_MAX};
  std::atomic<uint64_t> m_frameNacks{0};
  std::atomic<uint64_t> m_frameTimeouts{0};
  std::atomic<uint64_t> m_duplicateVideoPackets{0};
  std::atomic<uint64_t> m_decodedVideoFrames{0};
  std::atomic<uint64_t> m_lastVideoAdaptiveLogMs{0};
  std::atomic<uint64_t> m_videoBitrateAdviceSinceMs{0};
  std::atomic<uint64_t> m_lastVideoBitrateApplyMs{0};
  std::atomic<bool> m_mavlinkCommandInFlight{false};
  std::atomic<bool> m_manualControlInFlight{false};
  std::atomic<bool> m_emergencyStopInFlight{false};
  std::atomic<uint64_t> m_lastManualControlBlockedLogMs{0};
  mutable std::mutex m_telemetryMutex;
  std::set<std::string> m_telemetryInFlightDrones;
  std::map<std::string, TelemetryState> m_telemetryByDrone;
  std::map<std::string, ReadinessState> m_readinessByDrone;
  std::map<std::string, MissionState> m_missionByDrone;
  std::map<std::string, VideoState> m_videoByDrone;
  std::map<std::string, VideoAdaptiveState> m_videoAdaptiveByDrone;
  std::map<std::string, FlightCommandState> m_commandByDrone;
  std::map<std::string, SafetyState> m_safetyByDrone;
  ndn::Name m_streamPrefix;
  PacketLane m_keyLane;
  PacketLane m_deltaLane;
  uint64_t m_keyPacketsPerSecond = 16;
  uint64_t m_deltaPacketsPerSecond = 160;
  uint64_t m_keyWindow = 16;
  uint64_t m_deltaWindow = 108;
  uint64_t m_videoPayloadBytes = 3600;
  uint64_t m_videoFps = 30;
  std::atomic<uint64_t> m_videoRequestedBitrateKbps{8000};
  std::atomic<uint64_t> m_videoAcceptedBitrateKbps{8000};
  uint64_t m_videoTimeoutBudgetMs = 2500;
  uint64_t m_dynamicWindowMax = 128;
  uint64_t m_dynamicLookaheadMax = 64;
  uint64_t m_decoderReorderWindow = 12;
  uint64_t m_decoderBacklogLimit = 48;
  uint64_t m_nextChunkSeqToDecode = 0;
  uint64_t m_decoderDroppedChunks = 0;
  uint64_t m_decoderMissingChunkSeq = UINT64_MAX;
  uint64_t m_decoderMissingChunkStartMs = 0;
  uint64_t m_lastOutputChunkSeq = 0;
  uint64_t m_lastOutputChunkElapsedMs = 0;
  static constexpr uint64_t VIDEO_FPS = 30;
  static constexpr uint64_t INITIAL_PACKET_PROBE = 4;
  static constexpr uint64_t DEFAULT_VIDEO_RTT_MS = 120;
  static constexpr uint64_t STREAM_PUMP_INTERVAL_MS = 25;
  static constexpr uint64_t MAX_VIDEO_START_RETRIES = 2;
  static constexpr uint64_t VIDEO_BITRATE_APPLY_COOLDOWN_MS = 8000;
  std::atomic<uint64_t> m_videoRttEwmaMs{DEFAULT_VIDEO_RTT_MS};
  std::atomic<uint64_t> m_videoTimeoutPressurePercent{0};
  std::atomic<uint64_t> m_videoProbePressurePercent{0};
  std::atomic<uint64_t> m_videoDuplicatePressurePercent{0};
  std::atomic<uint64_t> m_decoderPendingChunkCount{0};
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_videoPumpScheduled{false};
  std::mutex m_videoPacketTrackingMutex;
  std::set<uint64_t> m_videoInFlightPacketSeqs;
  std::set<uint64_t> m_videoCompletedPacketSeqs;
  std::deque<uint64_t> m_videoCompletedPacketSeqOrder;
  static constexpr size_t MAX_VIDEO_PACKET_HISTORY = 4096;
  std::mutex m_decoderQueueMutex;
  std::condition_variable m_decoderQueueCv;
  std::deque<StreamChunk> m_chunkQueue;
  std::map<uint64_t, StreamChunk> m_pendingChunks;
  std::map<uint64_t, std::vector<uint8_t>> m_recordingPlaybackChunks;
  std::map<uint64_t, FecFrameState> m_fecFrames;
  std::vector<uint8_t> m_decoderOutBuffer;
  std::thread m_decoderWriterThread;
  std::thread m_decoderReaderThread;
  std::thread m_recordingPlaybackDecodeThread;
  std::atomic<bool> m_decoderRunning{false};
  std::atomic<bool> m_closeDecoderInputWhenQueueDrained{false};
  int m_decoderInFd = -1;
  int m_decoderOutFd = -1;
  pid_t m_decoderPid = -1;
};

int
serveObjectDetection(ndn::Face& face, ndn::KeyChain& keyChain,
                     const ndn::security::Certificate& gsCert,
                     const ndn::security::Certificate& controllerCert,
                     const UavRuntimeConfig& config,
                     bool serveCertificates)
{
  using ServiceInvocationMode = ndn_service_framework::ServiceProvider::ServiceInvocationMode;

  std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
  if (serveCertificates) {
    certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
      face, keyChain, gsCert.getName());
  }

  ndn_service_framework::ServiceProvider provider(
    face, config.groupPrefix, gsCert, controllerCert, config.trustSchema);
  provider.setHandlerThreads(2);
  provider.setAckThreads(2);
  provider.addService(
    config.serviceGsObjectDetection,
    ndn_service_framework::ServiceProvider::SimpleAckStrategyHandler(
      [](const ndn_service_framework::RequestMessage&) { return true; }),
    ndn_service_framework::ServiceProvider::SimpleRequestHandler(
      [](const ndn_service_framework::RequestMessage& request) {
        const auto payload = request.getPayload();
        const auto fields = decodeFields(std::string(
          reinterpret_cast<const char*>(payload.data()), payload.size()));
        const auto frameId = fieldOr(fields, "frame_id", "frame-unknown");
        return makeResponse(true, encodeFields({
          {"frame_id", frameId},
          {"model", "mock-yolo-gs"},
          {"objects", "road,vehicle,person"},
          {"summary", "mock detection generated at ground station"},
        }));
      }),
    ServiceInvocationMode::NormalOnly);
  provider.init();
  provider.fetchPermissionsFromController(config.controllerPrefix);
  NDN_LOG_INFO("UavGroundStationApp object detection service ready");
  face.processEvents();
  return 0;
}
