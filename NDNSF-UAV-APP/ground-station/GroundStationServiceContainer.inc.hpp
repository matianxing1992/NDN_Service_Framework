// App-internal implementation chunk included by UavGroundStationApp.cpp.
// Keeps the NDNSF service container separate from the GTK window for review.

class GroundStationServiceContainer
{
public:
  GroundStationServiceContainer(bool serveCertificates, int ackTimeoutMs, int timeoutMs,
                       UavRuntimeConfig config,
                       std::string targetDroneId, uint64_t videoBitrateKbps,
                       uint64_t videoFrameWidth, uint64_t videoFecParityShards,
                       std::vector<std::string> patrolDroneIds = {},
                       std::string yoloModel = "yolo26n.pt",
                       std::string yoloScript = "NDNSF-UAV-APP/tools/yolo_detect_once.py",
                       std::string yoloWorkerScript = "NDNSF-UAV-APP/tools/yolo_detect_worker.py",
                       uint64_t linkStaleMs = 3500,
                       uint64_t linkLostMs = 8000,
                       std::string lostLinkAction = "notify",
                       std::string videoBitratePolicy = "manual",
                       uint64_t videoBitrateAutoPressureMs = 2500,
                       std::string missionPlanFilePath = "",
                       std::string operatorId = "",
                       std::string operatorLeaseDrone = "all",
                       std::string operatorLeaseScope = "control",
                       uint64_t operatorLeaseTtlMs = 0,
                       std::string operatorAuthorityStateFile = "",
                       std::string operatorAdminIds = "",
                       uint64_t operatorAuthorityRefreshIntervalMs = 0)
    : m_serveCertificates(serveCertificates)
    , m_config(std::move(config))
    , m_coreContainer({
        m_config.groundStationIdentity,
        m_config.groupPrefix,
        m_config.controllerPrefix,
        m_config.trustSchema
      })
    , m_ackTimeoutMs(ackTimeoutMs)
    , m_timeoutMs(timeoutMs)
    , m_targetDroneId(std::move(targetDroneId))
    , m_videoBitrateKbps(videoBitrateKbps)
    , m_videoFrameWidth(videoFrameWidth)
    , m_videoFecParityShards(videoFecParityShards)
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
    , m_missionPlanFilePath(std::move(missionPlanFilePath))
    , m_operatorId(std::move(operatorId))
    , m_defaultOperatorLeaseDrone(std::move(operatorLeaseDrone))
    , m_defaultOperatorLeaseScope(std::move(operatorLeaseScope))
    , m_defaultOperatorLeaseTtlMs(operatorLeaseTtlMs)
    , m_operatorAuthorityStateFile(std::move(operatorAuthorityStateFile))
    , m_operatorAdminIds(parseOperatorIdList(operatorAdminIds))
    , m_operatorAuthorityRefreshIntervalMs(operatorAuthorityRefreshIntervalMs)
    , m_videoPumpTimer(m_face.getIoContext())
  {
    if (m_patrolDroneIds.empty()) {
      m_patrolDroneIds.push_back(m_targetDroneId);
    }
    if (m_operatorId.empty()) {
      m_operatorId = m_config.groundStationIdentity.toUri();
    }
    if (m_defaultOperatorLeaseDrone.empty()) {
      m_defaultOperatorLeaseDrone = "all";
    }
    if (m_defaultOperatorLeaseScope.empty()) {
      m_defaultOperatorLeaseScope = "control";
    }
    loadIssuedOperatorLeasesFromStateFile();
    m_targetDroneLocked = !m_targetDroneId.empty();
    issueDefaultOperatorLease();
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_gsCert = getOrCreateIdentity(m_keyChain, m_config.groundStationIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_config.controllerPrefix);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_config.groundStationIdentity));
    m_videoRequestedBitrateKbps = std::max<uint64_t>(128, m_videoBitrateKbps.load());
    m_videoAcceptedBitrateKbps = std::max<uint64_t>(128, m_videoBitrateKbps.load());
    m_coreContainer.addLifecycleHook("ground-station-runtime", {
      [this] { publishStatus("NDNSF service container started"); },
      [this] { publishStatus("NDNSF service container stopped"); }
    });
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
    NDN_LOG_INFO("GS_RUNTIME_SHUTDOWN phase=begin");
    m_streaming = false;
    m_recordingPlaybackActive = false;
    if (m_operatorAuthorityRefreshThread.joinable()) {
      m_operatorAuthorityRefreshThread.join();
    }
    NDN_LOG_INFO("GS_RUNTIME_SHUTDOWN phase=refresh-joined");
    m_face.getIoContext().stop();
    if (m_faceThread.joinable()) {
      m_faceThread.join();
    }
    NDN_LOG_INFO("GS_RUNTIME_SHUTDOWN phase=face-joined");
    m_coreContainer.stop();
    NDN_LOG_INFO("GS_RUNTIME_SHUTDOWN phase=container-stopped");
    // The face thread can create these workers during runtime initialization or
    // callbacks. Quiesce it first so no joinable worker appears after its join check.
    if (m_yoloPrewarmThread.joinable()) {
      m_yoloPrewarmThread.join();
    }
    stopYoloWorker();
    stopDecoder();
    if (m_recordingPlaybackDecodeThread.joinable()) {
      m_recordingPlaybackDecodeThread.join();
    }
    NDN_LOG_INFO("GS_RUNTIME_SHUTDOWN phase=workers-joined");
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
        // Ground-station control responses are small and share runtime/UI state.
        // Keep their callbacks on the Face thread so telemetry timeouts and
        // command responses cannot mutate that state concurrently.
        m_user->setHandlerThreads(0);
        m_user->init();
        m_coreContainer.useUser("ground-station", *m_user);
        m_user->fetchPermissionsFromController(m_config.controllerPrefix);
        installServiceInstances();
        m_objectDetectionProvider->init();
        m_coreContainer.useProvider("object-detection", *m_objectDetectionProvider);
        m_objectDetectionProvider->fetchPermissionsFromController(m_config.controllerPrefix);
        m_coreContainer.start();
        m_containerReady = true;
        publishStatus("NDNSF runtime ready");
        startOperatorAuthorityRefreshThread();
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

  const UavRuntimeConfig&
  config() const
  {
    return m_config;
  }

  void
  setOperatorAuthorityLease(OperatorAuthorityLease lease)
  {
    std::string status;
    {
      std::lock_guard<std::mutex> guard(m_operatorLeaseMutex);
      m_operatorLease = std::move(lease);
      status = m_operatorLease.statusLine();
    }
    publishStatus(status);
  }

  OperatorAuthorityLease
  operatorAuthorityLease() const
  {
    std::lock_guard<std::mutex> guard(m_operatorLeaseMutex);
    return m_operatorLease;
  }

  bool
  refreshOperatorAuthorityLeaseFromIssuer(const ndn::Name& issuerIdentity,
                                          std::chrono::seconds timeout,
                                          std::string& reason,
                                          Fields* revocationFields = nullptr)
  {
    bool expected = false;
    if (!m_operatorAuthorityRefreshInFlight.compare_exchange_strong(expected, true)) {
      reason = "refresh-in-flight";
      NDN_LOG_INFO("AUTHORITY_LEASE_REFRESH revoked=false reason=" << reason);
      return false;
    }
    struct RefreshGuard
    {
      std::atomic<bool>& flag;
      ~RefreshGuard() { flag = false; }
    } guard{m_operatorAuthorityRefreshInFlight};

    const auto current = operatorAuthorityLease();
    if (current.leaseId.empty() || current.leaseId == "none") {
      reason = "no-active-lease";
      NDN_LOG_INFO("AUTHORITY_LEASE_REFRESH revoked=false reason=" << reason);
      return false;
    }

    Fields fields;
    const bool revoked = requestOperatorRevocationRecordFromIssuerSync(
      issuerIdentity, current.leaseId, timeout, fields, reason);
    if (revocationFields != nullptr) {
      *revocationFields = fields;
    }
    if (revoked) {
      auto updated = current;
      updated.revoked = true;
      setOperatorAuthorityLease(updated);
      appendOperatorAuthorityAlert({
        "lease-revoked-detected",
        fieldOr(fields, "reason", reason),
        current.leaseId,
        fieldOr(fields, "revoked_operator", current.operatorId),
        fieldOr(fields, "revoker_operator", "unknown"),
        fieldOr(fields, "revoked_drone", current.droneId),
        fieldOr(fields, "revoked_scope", current.scope),
        nowMilliseconds()
      });
      {
        std::lock_guard<std::mutex> stateGuard(m_issuedOperatorLeaseMutex);
        persistIssuedOperatorLeasesLocked();
      }
      reason = fieldOr(fields, "reason", reason);
    }
    NDN_LOG_INFO("AUTHORITY_LEASE_REFRESH revoked=" << (revoked ? "true" : "false")
                 << " reason=" << reason
                 << " lease_id=" << current.leaseId
                 << " operator=" << current.operatorId
                 << " revoker_operator=" << fieldOr(fields, "revoker_operator", "none"));
    return revoked;
  }

  uint64_t
  operatorAuthorityRefreshIntervalMs() const
  {
    return m_operatorAuthorityRefreshIntervalMs;
  }

  std::vector<OperatorAuthorityAlert>
  operatorAuthorityAlertsSnapshot() const
  {
    std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
    return m_operatorAuthorityAlerts;
  }

  void
  setStatusCallback(std::function<void(std::string)> callback)
  {
    m_statusCallback = std::move(callback);
  }

  void
  setFrameCallback(std::function<void(std::vector<uint8_t>, uint64_t, uint64_t, std::string, uint64_t)> callback)
  {
    m_frameCallback = std::move(callback);
  }

  std::string
  activeVideoStreamId() const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    return m_activeStreamId;
  }

  uint64_t
  videoStreamSessionEpoch() const
  {
    return m_videoStreamSessionEpoch.load();
  }

  bool
  isCurrentStreamSession(uint64_t streamSessionEpoch) const
  {
    return streamSessionEpoch != 0 && streamSessionEpoch == videoStreamSessionEpoch();
  }

  std::string
  makeVideoSessionId(const std::string& tag, const std::string& droneId)
  {
    return tag + "|" + droneId + "|" + std::to_string(nowMilliseconds()) + "|" +
      std::to_string(++m_videoSessionCounter);
  }

  uint64_t
  allocateStreamSessionEpoch(std::string streamId)
  {
    const auto epoch = ++m_videoSessionCounter;
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    m_activeStreamId = std::move(streamId);
    m_videoStreamSessionEpoch = epoch;
    m_streamEpochByStreamId[m_activeStreamId] = epoch;
    return epoch;
  }

  uint64_t
  allocateStreamSessionEpoch(std::string streamId, uint64_t streamSessionEpoch)
  {
    if (streamSessionEpoch == 0) {
      return allocateStreamSessionEpoch(std::move(streamId));
    }

    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    if (streamSessionEpoch <= m_videoStreamSessionEpoch.load()) {
      NDN_LOG_WARN("GS_VIDEO_STREAM_SESSION stale epoch ignored streamId="
                   << streamId << " epoch=" << streamSessionEpoch
                   << " current=" << m_videoStreamSessionEpoch.load());
      m_streamEpochByStreamId[streamId] = m_videoStreamSessionEpoch.load();
      m_videoStreamSessionEpoch = m_videoStreamSessionEpoch.load();
      if (m_activeStreamId.empty()) {
        m_activeStreamId = std::move(streamId);
      }
      return m_videoStreamSessionEpoch.load();
    }

    m_videoSessionCounter = streamSessionEpoch;
    m_activeStreamId = std::move(streamId);
    m_videoStreamSessionEpoch = streamSessionEpoch;
    m_streamEpochByStreamId[m_activeStreamId] = streamSessionEpoch;
    return streamSessionEpoch;
  }

  void
  syncStreamSessionFromActiveId()
  {
    if (m_activeStreamId.empty()) {
      return;
    }
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    m_streamEpochByStreamId[m_activeStreamId] = m_videoStreamSessionEpoch.load();
  }

  uint64_t
  streamSessionEpochForStreamId(const std::string& streamId) const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    const auto it = m_streamEpochByStreamId.find(streamId);
    return it == m_streamEpochByStreamId.end() ? 0 : it->second;
  }

  bool
  isCurrentVideoSessionPacket(const std::string& streamId, uint64_t streamSessionEpoch) const
  {
    if (streamSessionEpoch == 0 || m_videoStreamSessionEpoch.load() == 0) {
      return false;
    }
    return isCurrentLiveVideoStream(streamId) && isCurrentStreamSession(streamSessionEpoch);
  }

  bool
  isCurrentSessionStreamId(const std::string& streamId) const
  {
    const auto activeStreamId = activeVideoStreamId();
    return activeStreamId.empty() || streamId == activeStreamId;
  }

  void
  setActiveStreamSession(std::string streamId)
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    m_activeStreamId = std::move(streamId);
    m_streamEpochByStreamId[m_activeStreamId] = m_videoStreamSessionEpoch.load();
  }

  bool
  isCurrentLiveVideoStream(const std::string& streamId) const
  {
    const auto activeStreamId = activeVideoStreamId();
    return activeStreamId.empty() || streamId == activeStreamId;
  }

  std::string
  targetDroneId() const
  {
    std::lock_guard<std::mutex> guard(m_targetMutex);
    return m_targetDroneId;
  }

  const ndn::Name&
  groundStationIdentity() const
  {
    return m_config.groundStationIdentity;
  }

  void
  setTargetDroneId(std::string droneId)
  {
    setTargetDroneId(std::move(droneId), TargetSelectionSource::User);
  }

  enum class TargetSelectionSource
  {
    User,
    Auto,
    Internal
  };

  void
  setTargetDroneId(std::string droneId, TargetSelectionSource source)
  {
    if (droneId.empty()) {
      return;
    }
    if (source != TargetSelectionSource::User) {
      std::lock_guard<std::mutex> guard(m_targetMutex);
      if (m_targetDroneLocked) {
        return;
      }
    }
    {
      std::lock_guard<std::mutex> guard(m_targetMutex);
      if (m_targetDroneId == droneId) {
        return;
      }
      m_targetDroneId = std::move(droneId);
      if (source == TargetSelectionSource::User) {
        m_targetDroneLocked = true;
      }
    }
    publishStatus("Selected drone " + targetDroneId());
  }

  bool
  isTargetDroneLocked() const
  {
    std::lock_guard<std::mutex> guard(m_targetMutex);
    return m_targetDroneLocked;
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
    os << "repo-catalog normal-only "
       << droneCameraRepoCatalogService(m_config, droneId).toUri() << "\n";
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
        NDN_LOG_INFO("GS_SUBSYSTEM_STATE drone=" << telemetry.droneId
                     << " camera_available=" << telemetry.cameraAvailable
                     << " camera_source=" << telemetry.cameraSource
                     << " camera_reason=" << telemetry.cameraReason
                     << " fc_backend=" << telemetry.flightControllerBackend
                     << " fc_available=" << telemetry.flightControllerAvailable
                     << " fc_ready=" << telemetry.flightControllerReady
                     << " fc_state=" << telemetry.flightControllerState
                     << " fc_reason=" << telemetry.flightControllerReason);
        publishStatus(telemetry.statusLine() +
                      " " + readiness.statusLine() +
                      " mission=" + mission.phase +
                      " mission_detail=" + mission.detail +
                      " " + video.statusLine());
      },
      [this, droneId] {
        clearTelemetryInFlight(droneId);
        publishStatus("Telemetry timeout for drone " + droneId);
      }, std::min(m_timeoutMs, 5000));
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

  std::string
  missionPlanFilePath() const
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    return m_missionPlanFilePath;
  }

  void
  setMissionPlanFilePath(std::string path)
  {
    std::lock_guard<std::mutex> guard(m_missionProgressMutex);
    m_missionPlanFilePath = std::move(path);
  }

  bool
  saveMissionPlanToFile(const MissionPlan& plan, const std::string& path,
                        std::string* detail = nullptr) const
  {
    if (path.empty()) {
      if (detail != nullptr) {
        *detail = "mission plan file path is empty";
      }
      return false;
    }
    try {
      auto document = MissionPlanDocument::fromPlan(
        plan,
        plan.taskId.empty() ? ("mission-" + std::to_string(nowMilliseconds())) : plan.taskId,
        plan.taskId.empty() ? "Ground station mission" : ("Ground station mission " + plan.taskId),
        m_config.groundStationIdentity.toUri(),
        nowMilliseconds());
      document.metadata["source"] = "ground-station";
      saveMissionPlanDocument(document, path);
      if (detail != nullptr) {
        *detail = document.statusLine();
      }
      return true;
    }
    catch (const std::exception& e) {
      if (detail != nullptr) {
        *detail = e.what();
      }
      return false;
    }
  }

  bool
  saveCurrentMissionPlanToFile(const std::string& path, std::string* detail = nullptr) const
  {
    const auto plan = missionPlanSnapshot();
    if (!plan) {
      if (detail != nullptr) {
        *detail = "no current mission plan";
      }
      return false;
    }
    return saveMissionPlanToFile(*plan, path, detail);
  }

  bool
  loadMissionPlanFromFile(const std::string& path, std::string* detail = nullptr)
  {
    if (path.empty()) {
      if (detail != nullptr) {
        *detail = "mission plan file path is empty";
      }
      return false;
    }
    try {
      const auto document = loadMissionPlanDocument(path);
      if (!document.isSaveable()) {
        if (detail != nullptr) {
          *detail = "loaded mission plan is not saveable: " + document.statusLine();
        }
        return false;
      }
      updateMissionPlan(document.plan);
      setMissionPlanFilePath(path);
      if (detail != nullptr) {
        *detail = document.statusLine();
      }
      return true;
    }
    catch (const std::exception& e) {
      if (detail != nullptr) {
        *detail = e.what();
      }
      return false;
    }
  }

  GroundStationRuntimeState
  runtimeSnapshot() const
  {
    GroundStationRuntimeState snapshot;
    snapshot.selectedDroneId = targetDroneId();
    snapshot.selectedDroneLocked = isTargetDroneLocked();
    snapshot.updatedMs = nowMilliseconds();
    snapshot.missionPlan = missionPlanSnapshot();
    snapshot.missionProgress = missionProgressSnapshot();
    {
      std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
      snapshot.operatorAuthorityAlerts = m_operatorAuthorityAlerts;
    }

    auto toAvailability = [] (const std::string& value) {
      if (value == "true" || value == "online" || value == "connected" || value == "ready") {
        return RuntimeAvailability::Available;
      }
      if (value == "false" || value == "offline" || value == "unavailable" || value == "lost") {
        return RuntimeAvailability::Unavailable;
      }
      return RuntimeAvailability::Unknown;
    };

    auto toConnection = [] (const std::string& linkState) {
      if (linkState == "connected" || linkState == "fresh") {
        return RuntimeConnectionState::Online;
      }
      if (linkState == "stale") {
        return RuntimeConnectionState::Stale;
      }
      if (linkState == "lost" || linkState == "offline") {
        return RuntimeConnectionState::Offline;
      }
      return RuntimeConnectionState::Unknown;
    };
    const auto classifyTelemetryFreshness = [this, now = nowMilliseconds()] (uint64_t timestampMs) {
      if (timestampMs == 0) {
        return "unknown";
      }
      const auto ageMs = now > timestampMs ? now - timestampMs : 0;
      if (ageMs >= m_linkLostMs) {
        return "missing";
      }
      if (ageMs >= m_linkStaleMs) {
        return "stale";
      }
      return "fresh";
    };

    std::set<std::string> droneIds;
    {
      std::lock_guard<std::mutex> guard(m_telemetryMutex);
      for (const auto& item : m_telemetryByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_readinessByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_missionByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_videoByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_videoAdaptiveByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_commandByDrone) {
        droneIds.insert(item.first);
      }
      for (const auto& item : m_safetyByDrone) {
        droneIds.insert(item.first);
      }

      for (const auto& droneId : droneIds) {
        auto& droneState = snapshot.ensureDrone(droneId);
        droneState.clearNotReadyReasons();
        uint64_t lastUpdatedMs = 0;
        if (const auto telemetryIt = m_telemetryByDrone.find(droneId);
            telemetryIt != m_telemetryByDrone.end()) {
          auto telemetryCopy = telemetryIt->second;
          telemetryCopy.telemetryFreshness = classifyTelemetryFreshness(telemetryCopy.timestampMs);
          droneState.telemetry = telemetryCopy;
          lastUpdatedMs = std::max(lastUpdatedMs, telemetryIt->second.timestampMs);
          droneState.telemetryReady = RuntimeAvailability::Available;
          droneState.cameraReady = toAvailability(telemetryIt->second.cameraAvailable);
          droneState.videoReady = telemetryIt->second.recording == "true" || telemetryIt->second.video == "true" ?
                                 RuntimeAvailability::Available : RuntimeAvailability::Unknown;
          droneState.flightControllerReady = toAvailability(telemetryIt->second.flightControllerReady);
        }

        if (const auto readinessIt = m_readinessByDrone.find(droneId);
            readinessIt != m_readinessByDrone.end()) {
          droneState.readiness = readinessIt->second;
          lastUpdatedMs = std::max(lastUpdatedMs, readinessIt->second.timestampMs);
          droneState.missionReady = readinessIt->second.readiness == "ready" ?
                                    RuntimeAvailability::Available : RuntimeAvailability::Unavailable;
        }

        if (const auto missionIt = m_missionByDrone.find(droneId);
            missionIt != m_missionByDrone.end()) {
          droneState.mission = missionIt->second;
          lastUpdatedMs = std::max(lastUpdatedMs, missionIt->second.updatedMs);
        }

        if (const auto videoIt = m_videoByDrone.find(droneId);
            videoIt != m_videoByDrone.end()) {
          droneState.video = videoIt->second;
          lastUpdatedMs = std::max(lastUpdatedMs, videoIt->second.updatedMs);
          droneState.videoReady = toAvailability(videoIt->second.status == "streaming" ||
                                                 videoIt->second.status == "active" ? "true" : videoIt->second.status);
          droneState.repoReady = toAvailability(videoIt->second.recording == "true" ? "true" : videoIt->second.recording);
        }

        if (const auto adaptiveIt = m_videoAdaptiveByDrone.find(droneId);
            adaptiveIt != m_videoAdaptiveByDrone.end()) {
          droneState.videoAdaptive = adaptiveIt->second;
          lastUpdatedMs = std::max(lastUpdatedMs, adaptiveIt->second.updatedMs);
        }

        if (const auto commandIt = m_commandByDrone.find(droneId);
            commandIt != m_commandByDrone.end()) {
          const auto& command = commandIt->second;
          RuntimeCommandSnapshot runtimeCommand;
          runtimeCommand.command = command.command;
          runtimeCommand.lifecycle = commandLifecycle(command);
          runtimeCommand.detail = command.detail;
          runtimeCommand.updatedMs = command.updatedMs;
          lastUpdatedMs = std::max(lastUpdatedMs, command.updatedMs);
          droneState.commandStates[command.command] = std::move(runtimeCommand);
        }
        if (const auto historyIt = m_commandHistoryByDrone.find(droneId);
            historyIt != m_commandHistoryByDrone.end()) {
          droneState.commandHistory = historyIt->second;
        }

        if (const auto safetyIt = m_safetyByDrone.find(droneId);
            safetyIt != m_safetyByDrone.end()) {
          const auto& safety = safetyIt->second;
          droneState.safety = safety;
          droneState.connection = toConnection(safety.linkState);
          lastUpdatedMs = std::max(lastUpdatedMs, safety.updatedMs);
          droneState.cameraReady = toAvailability(safety.detail == "idle" ? "unknown" : safety.manualControlState);
          droneState.repoReady = toAvailability(safety.lostLinkAction.empty() ? "false" : safety.lostLinkAction);
        }
        else if (droneState.safety.has_value()) {
          droneState.connection = RuntimeConnectionState::Offline;
        }

        if (lastUpdatedMs == 0 && droneState.telemetry.has_value()) {
          lastUpdatedMs = droneState.telemetry->timestampMs;
        }
        if (!m_containerReady.load()) {
          droneState.appendNotReadyReason(NotReadyReason::Certificate);
        }
        if (droneState.flightControllerReady == RuntimeAvailability::Unavailable) {
          droneState.appendNotReadyReason(NotReadyReason::FlightController);
        }
        if (droneState.cameraReady == RuntimeAvailability::Unavailable) {
          droneState.appendNotReadyReason(NotReadyReason::Camera);
        }
        if (droneState.repoReady == RuntimeAvailability::Unavailable) {
          droneState.appendNotReadyReason(NotReadyReason::Repo);
        }
        droneState.updatedMs = lastUpdatedMs;
      }
    }

    return snapshot;
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

  UavFunctionalityState
  functionalitySnapshotForSelectedDrone() const
  {
    const auto selectedDrone = targetDroneId();
    std::optional<RecordingDataProductState> recording;
    {
      std::lock_guard<std::mutex> guard(m_recordingManifestMutex);
      const auto found = m_recordingManifests.find(selectedDrone);
      if (found != m_recordingManifests.end()) {
        recording = found->second;
      }
    }

    const auto missionPlan = missionPlanSnapshot();
    const auto missionPart = missionPartForDrone(selectedDrone);
    const auto telemetry = telemetryForDrone(selectedDrone);
    const auto droneCount = std::max<size_t>(m_patrolDroneIds.size(), runtimeSnapshot().drones.size());
    auto functionality = UavFunctionalityState::fromStates(missionPlan, missionPart, recording,
                                                           telemetry,
                                                           !m_config.serviceGsObjectDetection.empty(),
                                                           droneCount);
    const auto parameters = parameterSnapshotForDrone(selectedDrone);
    if (parameters && parameters->isUsable()) {
      functionality.parameterStatusInspection = "available";
    }
    return functionality;
  }

  UavPracticalityState
  practicalitySnapshotForSelectedDrone() const
  {
    const auto selectedDrone = targetDroneId();
    return UavPracticalityState::fromStates(telemetryForDrone(selectedDrone),
                                            readinessForDrone(selectedDrone),
                                            true,
                                            true,
                                            true);
  }

  UavStabilityState
  stabilitySnapshotForSelectedDrone() const
  {
    const auto selectedDrone = targetDroneId();
    return UavStabilityState::fromStates(commandForDrone(selectedDrone),
                                         videoForDrone(selectedDrone),
                                         videoAdaptiveForDrone(selectedDrone),
                                         telemetryForDrone(selectedDrone),
                                         safetyForDrone(selectedDrone),
                                         true,
                                         true);
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
    if (profile == "frame-gap" || profile == "decode-gap") {
      const auto fps = std::max<uint64_t>(1, m_videoFps);
      m_videoFramesPublished = std::max<uint64_t>(fps * 4, 120);
      m_decodedVideoFrames = fps / 2;
    }
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
    const auto nowMs = nowMilliseconds();
    const auto stopSuppressUntil = m_videoStopSuppressUntilMs.load();
    if (stopSuppressUntil > nowMs) {
      publishStatus("Video stop for drone " + droneId + " is rate-limited; please avoid repeated clicks.");
      return;
    }
    if (m_recordingPlaybackActive.load() && activeRecordingPlaybackDroneId() == droneId) {
      m_recordingPlaybackActive = false;
      stopDecoder();
      {
        std::lock_guard<std::mutex> guard(m_videoStateMutex);
        m_recordingPlaybackDroneId.clear();
        m_recordingPlaybackStreamId.clear();
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
      m_videoStopSuppressUntilMs = nowMs + VIDEO_STOP_CLICK_SUPPRESS_MS;
      return;
    }
    m_videoStartInFlight = false;
    m_streaming = false;
    m_activeStreamId = makeVideoSessionId("live-stop", droneId);
    m_videoPumpScheduled = false;
    boost::system::error_code ec;
    m_videoPumpTimer.cancel(ec);
    publishVideoAdaptiveState("stop-requested", true);
    stopDecoder();
    stopVideoAttempt(droneId);
    m_videoStopSuppressUntilMs = nowMs + VIDEO_STOP_CLICK_SUPPRESS_MS;
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

  std::string
  activeRecordingPlaybackStreamId() const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    return m_recordingPlaybackStreamId;
  }

  void
  requestRecordingManifest()
  {
    requestRecordingManifestForDrone(targetDroneId(), false);
  }

  void
  requestRepoCatalog()
  {
    requestRepoCatalogForDrone(targetDroneId());
  }

  void
  requestVehicleParameters(std::function<void(std::optional<VehicleParameterSnapshot>)> onDone = {})
  {
    requestVehicleParametersForDrone(targetDroneId(), std::move(onDone));
  }

  void
  requestVehicleParameterEdit(VehicleParameterEditRequest request,
                              std::function<void(std::optional<VehicleParameterEditResult>)> onDone = {})
  {
    requestVehicleParameterEditForDrone(targetDroneId(), std::move(request), std::move(onDone));
  }

  void
  requestPreflightChecklist(std::function<void(std::vector<PreflightCheckItem>)> onDone = {})
  {
    requestPreflightChecklistForDrone(targetDroneId(), std::move(onDone));
  }

  void
  requestAnalyzeSnapshot(std::function<void(std::optional<UavAnalyzeSnapshot>)> onDone = {})
  {
    requestAnalyzeSnapshotForDrone(targetDroneId(), std::move(onDone));
  }

  std::optional<UavDataProductCatalogState>
  catalogForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_catalogMutex);
    const auto found = m_catalogByDrone.find(droneId);
    if (found == m_catalogByDrone.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<VehicleParameterSnapshot>
  parameterSnapshotForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_parameterMutex);
    const auto found = m_parameterSnapshots.find(droneId);
    if (found == m_parameterSnapshots.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<PreflightCheckItem>
  preflightChecklistForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_preflightMutex);
    const auto found = m_preflightByDrone.find(droneId);
    if (found == m_preflightByDrone.end()) {
      return {};
    }
    return found->second;
  }

  std::optional<UavAnalyzeSnapshot>
  analyzeSnapshotForDrone(const std::string& droneId) const
  {
    std::lock_guard<std::mutex> guard(m_analyzeMutex);
    const auto found = m_analyzeSnapshots.find(droneId);
    if (found == m_analyzeSnapshots.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  UavOperatorDashboardSnapshot
  operatorDashboardSnapshotForDrone(const std::string& droneId) const
  {
    UavOperatorDashboardSnapshot snapshot;
    snapshot.droneId = droneId;
    snapshot.updatedMs = nowMilliseconds();

    const auto telemetry = telemetryForDrone(droneId);
    if (telemetry) {
      snapshot.telemetryFreshness = telemetry->telemetryFreshnessLabel();
      snapshot.linkState = telemetry->linkState;
      if (snapshot.videoState == "unknown") {
        snapshot.videoState = telemetry->video;
      }
    }

    const auto readiness = readinessForDrone(droneId);
    if (readiness) {
      snapshot.readiness = readiness->readiness;
      snapshot.readinessReason = readiness->readinessReason;
      snapshot.flightMode = readiness->mode;
    }

    const auto mission = missionForDrone(droneId);
    if (mission) {
      snapshot.missionPhase = mission->phase;
    }

    const auto video = videoForDrone(droneId);
    if (video) {
      snapshot.videoState = video->status;
    }

    const auto parameters = parameterSnapshotForDrone(droneId);
    if (parameters) {
      snapshot.parameterCacheStatus = parameters->isUsable() ? "available" : "empty";
      snapshot.parameterCount = parameters->parameterCount == 0 ?
                                parameters->parameters.size() : parameters->parameterCount;
    }

    const auto preflight = preflightChecklistForDrone(droneId);
    snapshot.preflightTotal = preflight.size();
    snapshot.preflightBlockingFailures = static_cast<uint64_t>(std::count_if(
      preflight.begin(), preflight.end(), [] (const PreflightCheckItem& item) {
        return item.isBlockingFailure();
      }));

    const auto analyze = analyzeSnapshotForDrone(droneId);
    if (analyze) {
      snapshot.linkState = analyze->linkState;
      snapshot.flightMode = analyze->flightMode;
      snapshot.missionPhase = analyze->missionPhase;
      snapshot.videoState = analyze->videoState;
      if (snapshot.parameterCacheStatus == "unknown") {
        snapshot.parameterCacheStatus = analyze->parameterCacheStatus;
      }
      snapshot.mavlinkMessageCount = analyze->messages.size();
      snapshot.activeMavlinkMessageCount = analyze->activeMessageCount(snapshot.updatedMs, 3000);
    }

    const auto safety = safetyForDrone(droneId);
    const auto gate = FlightSafetyGateState::fromStates(droneId, readiness, safety);
    snapshot.canArm = gate.canArm;
    snapshot.canTakeoff = gate.canTakeoff;
    snapshot.canLand = gate.canLand;
    snapshot.canManualControl = gate.canManualControl;
    snapshot.canEmergencyStop = gate.canEmergencyStop;
    return snapshot;
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
    const auto commandAttemptMs = nowMilliseconds();
    NDN_LOG_INFO("UAV_CONTROL_COMMAND phase=attempt drone=" << droneId
                 << " command=" << commandName
                 << " timestamp_ms=" << commandAttemptMs
                 << " elapsed_ms=0 accepted=unknown reason=attempt");
    auto logLocalFailure = [&] (const std::string& phase, const std::string& reason) {
      NDN_LOG_INFO("UAV_CONTROL_COMMAND phase=" << phase
                   << " drone=" << droneId
                   << " command=" << commandName
                   << " timestamp_ms=" << nowMilliseconds()
                   << " elapsed_ms=" << (nowMilliseconds() - commandAttemptMs)
                   << " accepted=false reason=" << reason);
    };
    std::string leaseReason;
    if (!validateOperatorLease(droneId, commandName, leaseReason)) {
      logLocalFailure("blocked", leaseReason);
      recordBlockedCommand(droneId, commandName, leaseReason);
      publishStatus("MAVLink " + commandName + " blocked by operator lease drone=" +
                    droneId + " reason=" + leaseReason);
      return false;
    }
    const bool isManualControl = commandName == "manual_control";
    const bool isEmergencyStop = commandName == "emergency_stop";
    if (commandName == "arm") {
      std::string reason;
      if (!validateArmReadiness(droneId, reason)) {
        logLocalFailure("blocked", reason);
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Arm blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    if (isManualControl) {
      std::string reason;
      if (!validateManualControlReadiness(droneId, reason)) {
        logLocalFailure("blocked", reason);
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
        logLocalFailure("blocked", reason);
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Land blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    if (commandName == "takeoff") {
      std::string reason;
      if (!validateTakeoffReadiness(droneId, reason)) {
        logLocalFailure("blocked", reason);
        recordBlockedCommand(droneId, commandName, reason);
        publishStatus("Takeoff blocked drone=" + droneId + " reason=" + reason);
        return false;
      }
    }
    auto& inFlight = mavlinkInFlightFlag(isManualControl, isEmergencyStop);
    if (inFlight.exchange(true)) {
      logLocalFailure("busy", "command-in-flight");
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
    const auto requestStartMs = nowMilliseconds();
    updateCommandState(FlightCommandState::makePending(
      droneId, commandName, requestStartMs, m_timeoutMs));
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceMavlinkExecute,
      payload,
      [this, commandName, isManualControl, isEmergencyStop, droneId, requestStartMs](
        const std::string& responsePayload) {
        mavlinkInFlightFlag(isManualControl, isEmergencyStop) = false;
        const auto fields = decodeFields(responsePayload);
        auto commandState = FlightCommandState::fromFields(fields);
        commandState.droneId = droneId;
        commandState.command = commandName;
        const auto nowMs = nowMilliseconds();
        commandState.updatedMs = nowMs;
        commandState.rttMs = nowMs - requestStartMs;
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
        NDN_LOG_INFO("UAV_CONTROL_COMMAND phase=response drone=" << droneId
                     << " command=" << commandName
                     << " timestamp_ms=" << nowMs
                     << " elapsed_ms=" << (nowMs - requestStartMs)
                     << " accepted=" << accepted
                     << " reason=" << ackResult);
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
      [this, commandName, isManualControl, isEmergencyStop, droneId, requestStartMs] {
        mavlinkInFlightFlag(isManualControl, isEmergencyStop) = false;
        const auto timeoutMs = nowMilliseconds();
        NDN_LOG_INFO("UAV_CONTROL_COMMAND phase=timeout drone=" << droneId
                     << " command=" << commandName
                     << " timestamp_ms=" << timeoutMs
                     << " elapsed_ms=" << (timeoutMs - requestStartMs)
                     << " accepted=false reason=timeout");
        auto timeoutState = FlightCommandState::makeTimeout(
          droneId, commandName, requestStartMs, timeoutMs, m_timeoutMs);
        updateCommandState(timeoutState);
        publishStatus("MAVLink " + commandName +
                      " timed out for drone=" + droneId +
                      " accepted=false ack=timeout forwarded_bytes=0" +
                      " detail=operator-timeout-decision");
      });
    return true;
  }

  bool
  sendMavlinkCommandToDroneSync(const std::string& droneId, const std::string& commandName,
                                Fields params, std::chrono::milliseconds timeout)
  {
    std::string leaseReason;
    if (!validateOperatorLease(droneId, commandName, leaseReason)) {
      recordBlockedCommand(droneId, commandName, leaseReason);
      NDN_LOG_INFO("SINGLE_MISSION_COMMAND command=" << commandName
                   << " ok=false ack=lease-blocked reason=" << leaseReason);
      return false;
    }
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
    const auto requestStartMs = nowMilliseconds();
    auto pendingState = FlightCommandState::makePending(
      droneId, commandName, requestStartMs, m_timeoutMs);
    pendingState.detail = "sync-targeted-request-sent";
    updateCommandState(pendingState);
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceMavlinkExecute,
      payload,
      [&, requestStartMs](const std::string& responsePayload) {
        const auto fields = decodeFields(responsePayload);
        ackResult = fieldOr(fields, "ack_result", "unknown");
        const auto accepted = fieldOr(fields, "accepted", "false");
        auto commandState = FlightCommandState::fromFields(fields);
        commandState.droneId = droneId;
        commandState.command = commandName;
        const auto nowMs = nowMilliseconds();
        commandState.updatedMs = nowMs;
        commandState.rttMs = nowMs - requestStartMs;
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
        const auto timeoutMs = nowMilliseconds();
        auto timeoutState = FlightCommandState::makeTimeout(
          droneId, commandName, requestStartMs, timeoutMs, m_timeoutMs);
        updateCommandState(timeoutState);
        publishStatus("MAVLink " + commandName +
                      " timed out for drone=" + droneId +
                      " accepted=false ack=timeout forwarded_bytes=0" +
                      " detail=operator-timeout-decision");
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
      const auto missionPart = missionPartForDrone(droneId);
      const auto row = DroneListRowState::fromStates(droneId, true, telemetry,
                                                     readiness, mission, video,
                                                     videoAdaptive, std::nullopt,
                                                     safety, progress, missionPart);
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
                   << " camera_available=" << telemetry.cameraAvailable
                   << " camera_source=" << telemetry.cameraSource
                   << " camera_reason=" << telemetry.cameraReason
                   << " fc_backend=" << telemetry.flightControllerBackend
                   << " fc_available=" << telemetry.flightControllerAvailable
                   << " fc_ready=" << telemetry.flightControllerReady
                   << " fc_state=" << telemetry.flightControllerState
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
      return readiness.value_or(ReadinessState{});
    };

    int sampleIndex = 0;
    auto sampledReadiness = sample("initial", sampleIndex++);
    bool readyForArm = sampledReadiness.readyForArm();
    while (!readyForArm && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      sampledReadiness = sample("wait-ready", sampleIndex++);
      readyForArm = sampledReadiness.readyForArm();
    }
    if (!readyForArm || std::chrono::steady_clock::now() >= deadline) {
      NDN_LOG_INFO("TELEMETRY_LIVE_RESULT ok=false reason=not-ready-for-arm");
      return false;
    }
    const bool armOk = sendMavlinkCommandToDroneSync(
      droneId, "arm", {{"arm", "true"}}, commandTimeout);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    sampledReadiness = sample("armed", sampleIndex++);
    bool readyForTakeoff = sampledReadiness.readyForTakeoff();
    while (armOk && !readyForTakeoff && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      sampledReadiness = sample("wait-armed", sampleIndex++);
      readyForTakeoff = sampledReadiness.readyForTakeoff();
    }

    bool takeoffOk = false;
    if (readyForTakeoff) {
      takeoffOk = sendMavlinkCommandToDroneSync(
        droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}}, commandTimeout);
    }
    else {
      NDN_LOG_INFO("TELEMETRY_LIVE_RESULT ok=false reason=not-ready-for-takeoff");
    }
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
    std::string leaseReason;
    if (!validateOperatorLease(droneId, "mission_assign", leaseReason)) {
      NDN_LOG_INFO("SINGLE_MISSION_RESULT ok=false reason=" << leaseReason
                   << " phase=lease-blocked");
      return false;
    }
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
  runLoadedMissionPlanUploadTest(std::chrono::seconds timeout, std::string path)
  {
    if (path.empty()) {
      path = "/tmp/ndnsf-uav-loaded-mission-plan-test.conf";
    }
    const auto taskId = "loaded-mission-" + std::to_string(nowMilliseconds());
    std::vector<MissionWaypoint> route{
      {35.11860, -89.93750},
      {35.11920, -89.93750},
      {35.11920, -89.93680},
      {35.11860, -89.93680},
    };
    MissionPlan plan = buildPatrolMissionPlan(taskId, 35.1186, -89.9375, 140.0,
                                              m_patrolDroneIds, route);
    plan.assignment = "loaded-mission-plan";
    plan.completionObjective = "return-to-start";
    plan.returnHomePlanned = true;
    auto document = MissionPlanDocument::fromPlan(
      plan, taskId, "Loaded mission smoke", m_config.groundStationIdentity.toUri(),
      nowMilliseconds());
    document.metadata["source"] = "auto-loaded-mission-plan-test";
    saveMissionPlanDocument(document, path);
    NDN_LOG_INFO("LOADED_MISSION_PLAN_FILE_SAVED path=" << path
                 << " " << document.statusLine());
    std::string detail;
    if (!loadMissionPlanFromFile(path, &detail)) {
      NDN_LOG_INFO("LOADED_MISSION_PLAN_LOAD_FAILED path=" << path
                   << " detail=" << detail);
      return false;
    }
    NDN_LOG_INFO("LOADED_MISSION_PLAN_FILE_LOADED path=" << path
                 << " detail=" << detail);
    const auto loaded = missionPlanSnapshot();
    if (!loaded) {
      NDN_LOG_INFO("LOADED_MISSION_PLAN_UPLOAD_FAILED reason=no-loaded-plan");
      return false;
    }
    const bool ok = uploadMissionPlan(*loaded, timeout);
    NDN_LOG_INFO("LOADED_MISSION_PLAN_UPLOAD_RESULT ok=" << (ok ? "true" : "false")
                 << " task=" << loaded->taskId
                 << " parts=" << loaded->parts.size());
    return ok;
  }

  bool
  runRepoCatalogBrowseTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto catalog = requestRepoCatalogForDroneSync(
      droneId, std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    const bool ok = catalog && catalog->repoObjects > 0 && catalog->recordingProducts > 0;
    if (catalog) {
      NDN_LOG_INFO("REPO_CATALOG_BROWSE_RESULT ok=" << (ok ? "true" : "false")
                   << " drone=" << droneId
                   << " products=" << catalog->totalProducts()
                   << " repo_objects=" << catalog->repoObjects
                   << " latest=" << catalog->latestProductType
                   << " source=" << catalog->sourceRepo);
    }
    else {
      NDN_LOG_INFO("REPO_CATALOG_BROWSE_RESULT ok=false drone=" << droneId
                   << " reason=no-catalog-response");
    }
    return ok;
  }

  bool
  runParameterCacheTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto snapshot = requestVehicleParametersForDroneSync(
      droneId, std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    const bool ok = snapshot && snapshot->isUsable() &&
                    snapshot->parameters.find("NAV_RCL_ACT") != snapshot->parameters.end();
    if (snapshot) {
      NDN_LOG_INFO("VEHICLE_PARAMETER_CACHE_RESULT ok=" << (ok ? "true" : "false")
                   << " drone=" << droneId
                   << " source=" << snapshot->source
                   << " firmware=" << snapshot->firmware
                   << " vehicle=" << snapshot->vehicleType
                   << " parameters=" << snapshot->parameterCount
                   << " complete=" << snapshot->completePercent);
    }
    else {
      NDN_LOG_INFO("VEHICLE_PARAMETER_CACHE_RESULT ok=false drone=" << droneId
                   << " reason=no-parameter-response");
    }
    return ok;
  }

  bool
  runParameterEditTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const auto before = requestVehicleParametersForDroneSync(droneId, wait);
    if (!before || before->parameters.find("NAV_RCL_ACT") == before->parameters.end()) {
      NDN_LOG_INFO("VEHICLE_PARAMETER_EDIT_RESULT ok=false drone=" << droneId
                   << " reason=no-before-snapshot");
      return false;
    }
    const auto previousValue = before->parameters.at("NAV_RCL_ACT");
    const auto requestedValue = previousValue == "1" ? std::string("2") : std::string("1");

    VehicleParameterEditRequest request;
    request.requestId = "auto-param-edit-" + std::to_string(nowMilliseconds());
    request.operatorId = m_config.groundStationIdentity.toUri();
    request.droneId = droneId;
    request.parameterName = "NAV_RCL_ACT";
    request.expectedValue = previousValue;
    request.requestedValue = requestedValue;
    request.valueType = "MAV_PARAM_TYPE_INT32";
    request.requestedMs = nowMilliseconds();

    const auto edit = requestVehicleParameterEditForDroneSync(droneId, request, wait);
    if (!edit || !edit->successful()) {
      NDN_LOG_INFO("VEHICLE_PARAMETER_EDIT_RESULT ok=false drone=" << droneId
                   << " param=NAV_RCL_ACT previous=" << previousValue
                   << " requested=" << requestedValue
                   << " reason=" << (edit ? edit->reason : "no-edit-response"));
      return false;
    }

    const auto after = requestVehicleParametersForDroneSync(droneId, wait);
    const bool verified = after &&
                          after->parameters.find("NAV_RCL_ACT") != after->parameters.end() &&
                          after->parameters.at("NAV_RCL_ACT") == requestedValue;
    NDN_LOG_INFO("VEHICLE_PARAMETER_EDIT_RESULT ok=" << (verified ? "true" : "false")
                 << " drone=" << droneId
                 << " param=NAV_RCL_ACT previous=" << previousValue
                 << " requested=" << requestedValue
                 << " edit_reason=" << edit->reason
                 << " verified_value="
                 << (after && after->parameters.find("NAV_RCL_ACT") != after->parameters.end() ?
                     after->parameters.at("NAV_RCL_ACT") : std::string("missing")));
    return verified;
  }

  bool
  runPreflightChecklistTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto items = requestPreflightChecklistForDroneSync(
      droneId, std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    const auto blockingFailures = static_cast<size_t>(std::count_if(
      items.begin(), items.end(), [] (const PreflightCheckItem& item) {
        return item.isBlockingFailure();
      }));
    const bool hasHeartbeat = std::any_of(
      items.begin(), items.end(), [] (const PreflightCheckItem& item) {
        return item.checkId == "heartbeat" && item.isPass();
      });
    const bool hasGps = std::any_of(
      items.begin(), items.end(), [] (const PreflightCheckItem& item) {
        return item.checkId == "gps" && item.isPass();
      });
    const bool ok = items.size() >= 5 && blockingFailures == 0 && hasHeartbeat && hasGps;
    NDN_LOG_INFO("PREFLIGHT_CHECKLIST_RESULT ok=" << (ok ? "true" : "false")
                 << " drone=" << droneId
                 << " items=" << items.size()
                 << " blocking_failures=" << blockingFailures
                 << " heartbeat_pass=" << (hasHeartbeat ? "true" : "false")
                 << " gps_pass=" << (hasGps ? "true" : "false"));
    return ok;
  }

  bool
  runAnalyzeSnapshotTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto snapshot = requestAnalyzeSnapshotForDroneSync(
      droneId, std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    const auto now = nowMilliseconds();
    const bool hasHeartbeat = snapshot && std::any_of(
      snapshot->messages.begin(), snapshot->messages.end(), [] (const MavlinkMessageSummary& message) {
        return message.messageName == "HEARTBEAT" && message.count > 0;
      });
    const bool hasPosition = snapshot && std::any_of(
      snapshot->messages.begin(), snapshot->messages.end(), [] (const MavlinkMessageSummary& message) {
        return message.messageName == "GLOBAL_POSITION_INT" && message.count > 0;
      });
    const auto active = snapshot ? snapshot->activeMessageCount(now, 3000) : 0;
    const bool ok = snapshot && snapshot->messages.size() >= 4 &&
                    active >= 2 && hasHeartbeat && hasPosition;
    NDN_LOG_INFO("ANALYZE_SNAPSHOT_RESULT ok=" << (ok ? "true" : "false")
                 << " drone=" << droneId
                 << " messages=" << (snapshot ? snapshot->messages.size() : 0)
                 << " active_messages=" << active
                 << " heartbeat=" << (hasHeartbeat ? "true" : "false")
                 << " global_position=" << (hasPosition ? "true" : "false"));
    return ok;
  }

  bool
  runOperatorDashboardSnapshotTest(std::chrono::seconds timeout)
  {
    const auto droneId = targetDroneId();
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const auto telemetryFields = requestTelemetryStatusForDroneSync(droneId, wait);
    if (!telemetryFields.empty()) {
      auto telemetry = TelemetryState::fromFields(telemetryFields);
      if (telemetry.droneId == "unknown") {
        telemetry.droneId = droneId;
      }
      updateDroneState(telemetry, MissionState::fromFields(telemetryFields));
    }
    const auto parameters = requestVehicleParametersForDroneSync(droneId, wait);
    const auto preflight = requestPreflightChecklistForDroneSync(droneId, wait);
    const auto analyze = requestAnalyzeSnapshotForDroneSync(droneId, wait);
    const auto dashboard = operatorDashboardSnapshotForDrone(droneId);

    const bool ok = !telemetryFields.empty() &&
                    parameters && parameters->isUsable() &&
                    !preflight.empty() &&
                    analyze && !analyze->messages.empty() &&
                    dashboard.droneId == droneId &&
                    dashboard.parameterCount > 0 &&
                    dashboard.preflightTotal >= 5 &&
                    dashboard.preflightBlockingFailures == 0 &&
                    dashboard.mavlinkMessageCount >= 4 &&
                    dashboard.activeMavlinkMessageCount >= 2 &&
                    dashboard.canEmergencyStop;
    NDN_LOG_INFO("OPERATOR_DASHBOARD_SNAPSHOT_RESULT ok=" << (ok ? "true" : "false")
                 << " drone=" << droneId
                 << " telemetry=" << dashboard.telemetryFreshness
                 << " readiness=" << dashboard.readiness
                 << " reason=" << dashboard.readinessReason
                 << " parameters=" << dashboard.parameterCount
                 << " preflight=" << dashboard.preflightTotal
                 << " blocking_failures=" << dashboard.preflightBlockingFailures
                 << " mavlink_messages=" << dashboard.mavlinkMessageCount
                 << " active_mavlink_messages=" << dashboard.activeMavlinkMessageCount
                 << " can_takeoff=" << (dashboard.canTakeoff ? "true" : "false"));
    publishStatus(dashboard.statusLine());
    publishStatus("Operator dashboard snapshot drone=" + droneId +
                  " readiness=" + dashboard.readiness +
                  " preflight=" + std::to_string(dashboard.preflightTotal) +
                  " active_mavlink=" + std::to_string(dashboard.activeMavlinkMessageCount) +
                  " parameter_count=" + std::to_string(dashboard.parameterCount));
    return ok;
  }

  bool
  runAuthorityLeaseGateTest(std::chrono::seconds)
  {
    OperatorAuthorityLease monitorLease;
    monitorLease.leaseId = "monitor-only-test";
    monitorLease.operatorId = m_config.groundStationIdentity.toUri();
    monitorLease.droneId = targetDroneId();
    monitorLease.scope = "monitor";
    monitorLease.issuedMs = nowMilliseconds();
    monitorLease.expiresMs = monitorLease.issuedMs + 60000;
    setOperatorAuthorityLease(monitorLease);

    std::string telemetryReason;
    const bool telemetryAllowed = validateOperatorLease(targetDroneId(), "telemetry", telemetryReason);
    const bool landBlocked = !sendMavlinkCommandToDrone(targetDroneId(), "land");
    std::string missionReason;
    const bool missionBlocked = !validateOperatorLease(targetDroneId(), "mission_assign", missionReason);

    OperatorAuthorityLease expiredControl;
    expiredControl.leaseId = "expired-control-test";
    expiredControl.operatorId = m_config.groundStationIdentity.toUri();
    expiredControl.droneId = targetDroneId();
    expiredControl.scope = "control";
    expiredControl.issuedMs = 1;
    expiredControl.expiresMs = 2;
    setOperatorAuthorityLease(expiredControl);
    std::string expiredReason;
    const bool expiredBlocked = !validateOperatorLease(targetDroneId(), "arm", expiredReason);

    issueDefaultOperatorLease();
    const bool ok = telemetryAllowed && telemetryReason == "ok" &&
                    landBlocked && missionBlocked && missionReason == "monitor-scope" &&
                    expiredBlocked && expiredReason == "lease-expired";
    NDN_LOG_INFO("AUTHORITY_LEASE_GATE_RESULT ok=" << (ok ? "true" : "false")
                 << " telemetry_allowed=" << (telemetryAllowed ? "true" : "false")
                 << " telemetry_reason=" << telemetryReason
                 << " land_blocked=" << (landBlocked ? "true" : "false")
                 << " mission_blocked=" << (missionBlocked ? "true" : "false")
                 << " mission_reason=" << missionReason
                 << " expired_blocked=" << (expiredBlocked ? "true" : "false")
                 << " expired_reason=" << expiredReason);
    return ok;
  }

  bool
  runConfiguredAuthorityLeaseTest(std::chrono::seconds)
  {
    const auto lease = operatorAuthorityLease();
    std::string telemetryReason;
    const bool telemetryAllowed = validateOperatorLease(targetDroneId(), "telemetry", telemetryReason);
    const bool landBlocked = !sendMavlinkCommandToDrone(targetDroneId(), "land");
    std::string missionReason;
    const bool missionBlocked = !validateOperatorLease(targetDroneId(), "mission_assign", missionReason);
    const bool expectedMonitorLease = lease.scope == "monitor" && lease.droneId == targetDroneId();
    const bool ok = expectedMonitorLease &&
                    telemetryAllowed && telemetryReason == "ok" &&
                    landBlocked &&
                    missionBlocked && missionReason == "monitor-scope";
    NDN_LOG_INFO("AUTHORITY_CONFIG_RESULT ok=" << (ok ? "true" : "false")
                 << " lease_id=" << lease.leaseId
                 << " lease_operator=" << lease.operatorId
                 << " lease_drone=" << lease.droneId
                 << " lease_scope=" << lease.scope
                 << " lease_expires_ms=" << lease.expiresMs
                 << " telemetry_allowed=" << (telemetryAllowed ? "true" : "false")
                 << " telemetry_reason=" << telemetryReason
                 << " land_blocked=" << (landBlocked ? "true" : "false")
                 << " mission_blocked=" << (missionBlocked ? "true" : "false")
                 << " mission_reason=" << missionReason);
    return ok;
  }

  bool
  runAuthorityLeaseIssuerTest(std::chrono::seconds timeout)
  {
    OperatorAuthorityLease monitorLease;
    monitorLease.leaseId = "issuer-smoke-monitor";
    monitorLease.operatorId = m_operatorId;
    monitorLease.droneId = targetDroneId();
    monitorLease.scope = "monitor";
    monitorLease.issuedMs = nowMilliseconds();
    monitorLease.expiresMs = monitorLease.issuedMs + 60000;
    setOperatorAuthorityLease(monitorLease);

    std::string beforeReason;
    const bool beforeBlocked = !validateOperatorLease(targetDroneId(), "mission_assign", beforeReason);

    OperatorAuthorityLeaseRequest request;
    request.requestId = "issuer-smoke-" + std::to_string(nowMilliseconds());
    request.operatorId = m_operatorId;
    request.droneId = targetDroneId();
    request.scope = "control";
    request.ttlMs = 60000;
    request.requestedMs = nowMilliseconds();

    std::string responseReason;
    OperatorAuthorityLease issuedLease;
    const bool issued = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, request, timeout, issuedLease, responseReason);
    if (issued) {
      setOperatorAuthorityLease(issuedLease);
    }

    std::string afterReason;
    const bool afterAllowed = validateOperatorLease(targetDroneId(), "mission_assign", afterReason);
    const bool ok = beforeBlocked && beforeReason == "monitor-scope" &&
                    issued && responseReason == "ok" &&
                    issuedLease.scope == "control" &&
                    issuedLease.droneId == targetDroneId() &&
                    afterAllowed && afterReason == "ok";
    NDN_LOG_INFO("AUTHORITY_ISSUER_RESULT ok=" << (ok ? "true" : "false")
                 << " before_blocked=" << (beforeBlocked ? "true" : "false")
                 << " before_reason=" << beforeReason
                 << " issued=" << (issued ? "true" : "false")
                 << " response_reason=" << responseReason
                 << " issued_scope=" << issuedLease.scope
                 << " issued_drone=" << issuedLease.droneId
                 << " after_allowed=" << (afterAllowed ? "true" : "false")
                 << " after_reason=" << afterReason);
    return ok;
  }

  bool
  runAuthorityLeaseArbitrationTest(std::chrono::seconds timeout)
  {
    OperatorAuthorityLeaseRequest first;
    first.requestId = "arbitration-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);

    auto monitor = first;
    monitor.requestId = "arbitration-monitor-" + std::to_string(nowMilliseconds());
    monitor.operatorId = "/example/uav/operator/two";
    monitor.scope = "monitor";
    OperatorAuthorityLease monitorLease;
    std::string monitorReason;
    const bool monitorAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, monitor, timeout, monitorLease, monitorReason);

    auto conflict = first;
    conflict.requestId = "arbitration-conflict-" + std::to_string(nowMilliseconds());
    conflict.operatorId = "/example/uav/operator/two";
    OperatorAuthorityLease conflictLease;
    std::string conflictReason;
    const bool conflictAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, conflict, timeout, conflictLease, conflictReason);

    auto renewal = first;
    renewal.requestId = "arbitration-renew-" + std::to_string(nowMilliseconds());
    OperatorAuthorityLease renewalLease;
    std::string renewalReason;
    const bool renewalAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, renewal, timeout, renewalLease, renewalReason);

    auto admin = conflict;
    admin.requestId = "arbitration-admin-" + std::to_string(nowMilliseconds());
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason);

    auto postAdmin = first;
    postAdmin.requestId = "arbitration-post-admin-" + std::to_string(nowMilliseconds());
    OperatorAuthorityLease postAdminLease;
    std::string postAdminReason;
    const bool postAdminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, postAdmin, timeout, postAdminLease, postAdminReason);

    const bool ok = firstAccepted && firstReason == "ok" &&
                    monitorAccepted && monitorReason == "ok" &&
                    !conflictAccepted && conflictReason == "lease-conflict" &&
                    renewalAccepted && renewalReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    !postAdminAccepted && postAdminReason == "lease-conflict";
    NDN_LOG_INFO("AUTHORITY_ARBITRATION_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " monitor=" << (monitorAccepted ? "accepted" : monitorReason)
                 << " conflict=" << (conflictAccepted ? "accepted" : conflictReason)
                 << " renewal=" << (renewalAccepted ? "accepted" : renewalReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " post_admin=" << (postAdminAccepted ? "accepted" : postAdminReason)
                 << " first_operator=" << first.operatorId
                 << " conflict_operator=" << conflict.operatorId
                 << " drone=" << first.droneId);
    return ok;
  }

  bool
  runAuthorityLeasePersistenceTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_PERSISTENCE_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "persistence-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    Fields firstFields;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason, &firstFields);

    auto unauthAdmin = first;
    unauthAdmin.requestId = "persistence-unauth-admin-" + std::to_string(nowMilliseconds());
    unauthAdmin.operatorId = "/example/uav/operator/three";
    unauthAdmin.scope = "admin";
    OperatorAuthorityLease unauthLease;
    std::string unauthReason;
    Fields unauthFields;
    const bool unauthAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, unauthAdmin, timeout, unauthLease, unauthReason,
      &unauthFields);

    auto admin = first;
    admin.requestId = "persistence-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    Fields adminFields;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason, &adminFields);

    bool persistedOk = false;
    std::string persistedOperator = "none";
    std::string persistedScope = "none";
    try {
      const auto persisted = loadKeyValueConfig(m_operatorAuthorityStateFile);
      persistedOperator = fieldOr(persisted, "lease.0.lease_operator", persistedOperator);
      persistedScope = fieldOr(persisted, "lease.0.lease_scope", persistedScope);
      persistedOk = fieldOr(persisted, "lease_count", "0") == "1" &&
                    persistedOperator == admin.operatorId &&
                    persistedScope == "admin";
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("AUTHORITY_PERSISTENCE_LOAD_CHECK_FAILED path="
                   << m_operatorAuthorityStateFile << " error=" << e.what());
    }

    const auto revokedIds = fieldOr(adminFields, "revoked_lease_ids", "");
    const bool revokedOk = !revokedIds.empty() &&
                           revokedIds.find(firstLease.leaseId) != std::string::npos;
    const bool ok = firstAccepted && firstReason == "ok" &&
                    !unauthAccepted && unauthReason == "admin-unauthorized" &&
                    adminAccepted && adminReason == "ok" &&
                    fieldOr(adminFields, "overridden_leases", "0") == "1" &&
                    revokedOk && persistedOk;
    NDN_LOG_INFO("AUTHORITY_PERSISTENCE_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " unauth_admin=" << (unauthAccepted ? "accepted" : unauthReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " revoked_lease_ids=" << revokedIds
                 << " persisted_operator=" << persistedOperator
                 << " persisted_scope=" << persistedScope
                 << " state_file=" << m_operatorAuthorityStateFile);
    return ok;
  }

  bool
  runAuthorityRevocationLookupTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_REVOCATION_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "revocation-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);

    auto admin = first;
    admin.requestId = "revocation-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    Fields adminFields;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason, &adminFields);

    const auto revokedLeaseId = fieldOr(adminFields, "revoked_lease_ids", "");
    Fields recordFields;
    std::string lookupReason;
    const bool lookupFound = requestOperatorRevocationRecordFromIssuerSync(
      m_config.groundStationIdentity, revokedLeaseId, timeout, recordFields, lookupReason);

    Fields missingFields;
    std::string missingReason;
    const bool missingFound = requestOperatorRevocationRecordFromIssuerSync(
      m_config.groundStationIdentity, "missing-lease-id", timeout, missingFields, missingReason);

    const bool recordOk = lookupFound &&
                          lookupReason == "ok" &&
                          fieldOr(recordFields, "revoked_lease_id", "") == firstLease.leaseId &&
                          fieldOr(recordFields, "revoked_operator", "") == first.operatorId &&
                          fieldOr(recordFields, "revoker_operator", "") == admin.operatorId &&
                          fieldOr(recordFields, "revoked_scope", "") == first.scope &&
                          fieldOr(recordFields, "revoked_drone", "") == first.droneId;
    const bool missingOk = !missingFound && missingReason == "not-found";
    const bool ok = firstAccepted && firstReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    !revokedLeaseId.empty() &&
                    recordOk && missingOk;
    NDN_LOG_INFO("AUTHORITY_REVOCATION_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " revoked_lease_id=" << revokedLeaseId
                 << " lookup=" << (lookupFound ? "found" : lookupReason)
                 << " missing=" << (missingFound ? "found" : missingReason)
                 << " record_operator=" << fieldOr(recordFields, "revoked_operator", "none")
                 << " revoker_operator=" << fieldOr(recordFields, "revoker_operator", "none"));
    return ok;
  }

  bool
  runAuthorityLeaseRefreshTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_REFRESH_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "refresh-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);
    if (firstAccepted) {
      setOperatorAuthorityLease(firstLease);
    }

    std::string beforeReason;
    const bool beforeAllowed = validateOperatorLease(targetDroneId(), "mission_assign",
                                                     beforeReason);

    auto admin = first;
    admin.requestId = "refresh-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    Fields adminFields;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason, &adminFields);

    std::string refreshReason;
    Fields refreshFields;
    const bool refreshRevoked = refreshOperatorAuthorityLeaseFromIssuer(
      m_config.groundStationIdentity, timeout, refreshReason, &refreshFields);

    const auto refreshedLease = operatorAuthorityLease();
    std::string afterReason;
    const bool afterAllowed = validateOperatorLease(targetDroneId(), "mission_assign",
                                                    afterReason);
    const auto revokedIds = fieldOr(adminFields, "revoked_lease_ids", "");
    const bool ok = firstAccepted && firstReason == "ok" &&
                    beforeAllowed && beforeReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    revokedIds.find(firstLease.leaseId) != std::string::npos &&
                    refreshRevoked && refreshReason == "ok" &&
                    refreshedLease.revoked &&
                    fieldOr(refreshFields, "revoked_operator", "") == first.operatorId &&
                    fieldOr(refreshFields, "revoker_operator", "") == admin.operatorId &&
                    !afterAllowed && afterReason == "lease-revoked";
    NDN_LOG_INFO("AUTHORITY_REFRESH_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " before_reason=" << beforeReason
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " revoked_lease_ids=" << revokedIds
                 << " refresh=" << (refreshRevoked ? "revoked" : refreshReason)
                 << " refreshed_revoked=" << (refreshedLease.revoked ? "true" : "false")
                 << " after_allowed=" << (afterAllowed ? "true" : "false")
                 << " after_reason=" << afterReason
                 << " revoked_operator=" << fieldOr(refreshFields, "revoked_operator", "none")
                 << " revoker_operator=" << fieldOr(refreshFields, "revoker_operator", "none"));
    return ok;
  }

  bool
  runAuthorityLeaseRefreshTimerTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityRefreshIntervalMs == 0) {
      NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER_RESULT ok=false reason=timer-disabled");
      return false;
    }
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "refresh-timer-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);
    if (firstAccepted) {
      setOperatorAuthorityLease(firstLease);
    }

    auto admin = first;
    admin.requestId = "refresh-timer-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    Fields adminFields;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason, &adminFields);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string afterReason = "not-checked";
    bool afterAllowed = true;
    bool timerRevoked = false;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto lease = operatorAuthorityLease();
      std::string currentReason;
      const bool currentAllowed = validateOperatorLease(targetDroneId(), "mission_assign",
                                                        currentReason);
      if (lease.revoked && !currentAllowed && currentReason == "lease-revoked") {
        timerRevoked = true;
        afterAllowed = currentAllowed;
        afterReason = currentReason;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!timerRevoked) {
      afterAllowed = validateOperatorLease(targetDroneId(), "mission_assign", afterReason);
    }
    const auto refreshedLease = operatorAuthorityLease();
    const auto revokedIds = fieldOr(adminFields, "revoked_lease_ids", "");
    const bool ok = firstAccepted && firstReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    revokedIds.find(firstLease.leaseId) != std::string::npos &&
                    timerRevoked && refreshedLease.revoked &&
                    !afterAllowed && afterReason == "lease-revoked";
    NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " interval_ms=" << m_operatorAuthorityRefreshIntervalMs
                 << " timer_revoked=" << (timerRevoked ? "true" : "false")
                 << " refreshed_revoked=" << (refreshedLease.revoked ? "true" : "false")
                 << " after_allowed=" << (afterAllowed ? "true" : "false")
                 << " after_reason=" << afterReason
                 << " revoked_lease_ids=" << revokedIds);
    return ok;
  }

  bool
  runAuthorityAlertHistoryTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_ALERT_HISTORY_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
      m_operatorAuthorityAlerts.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "alert-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);
    if (firstAccepted) {
      setOperatorAuthorityLease(firstLease);
    }

    auto admin = first;
    admin.requestId = "alert-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    Fields adminFields;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason, &adminFields);

    std::string refreshReason;
    Fields refreshFields;
    const bool refreshRevoked = refreshOperatorAuthorityLeaseFromIssuer(
      m_config.groundStationIdentity, timeout, refreshReason, &refreshFields);

    const auto alerts = operatorAuthorityAlertsSnapshot();
    bool sawOverride = false;
    bool sawDetected = false;
    for (const auto& alert : alerts) {
      if (alert.type == "admin-override" &&
          alert.leaseId == firstLease.leaseId &&
          alert.revokedOperator == first.operatorId &&
          alert.revokerOperator == admin.operatorId) {
        sawOverride = true;
      }
      if (alert.type == "lease-revoked-detected" &&
          alert.leaseId == firstLease.leaseId &&
          alert.revokedOperator == first.operatorId &&
          alert.revokerOperator == admin.operatorId) {
        sawDetected = true;
      }
    }
    {
      std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
      m_operatorAuthorityAlerts.clear();
    }
    loadIssuedOperatorLeasesFromStateFile();
    const auto reloadedAlerts = operatorAuthorityAlertsSnapshot();
    bool reloadedOverride = false;
    bool reloadedDetected = false;
    for (const auto& alert : reloadedAlerts) {
      if (alert.type == "admin-override" &&
          alert.leaseId == firstLease.leaseId &&
          alert.revokedOperator == first.operatorId &&
          alert.revokerOperator == admin.operatorId) {
        reloadedOverride = true;
      }
      if (alert.type == "lease-revoked-detected" &&
          alert.leaseId == firstLease.leaseId &&
          alert.revokedOperator == first.operatorId &&
          alert.revokerOperator == admin.operatorId) {
        reloadedDetected = true;
      }
    }
    const bool ok = firstAccepted && firstReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    refreshRevoked && refreshReason == "ok" &&
                    sawOverride && sawDetected && alerts.size() >= 2 &&
                    reloadedOverride && reloadedDetected && reloadedAlerts.size() >= 2;
    NDN_LOG_INFO("AUTHORITY_ALERT_HISTORY_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " refresh=" << (refreshRevoked ? "revoked" : refreshReason)
                 << " alert_count=" << alerts.size()
                 << " saw_override=" << (sawOverride ? "true" : "false")
                 << " saw_detected=" << (sawDetected ? "true" : "false")
                 << " reloaded_alert_count=" << reloadedAlerts.size()
                 << " reloaded_override=" << (reloadedOverride ? "true" : "false")
                 << " reloaded_detected=" << (reloadedDetected ? "true" : "false")
                 << " lease_id=" << firstLease.leaseId);
    return ok;
  }

  bool
  runAuthorityAuditQueryTest(std::chrono::seconds timeout)
  {
    if (m_operatorAuthorityStateFile.empty()) {
      NDN_LOG_INFO("AUTHORITY_AUDIT_QUERY_RESULT ok=false reason=missing-state-file");
      return false;
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      m_issuedOperatorLeases.clear();
      m_operatorRevocationRecords.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
      m_operatorAuthorityAlerts.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      persistIssuedOperatorLeasesLocked();
    }

    OperatorAuthorityLeaseRequest first;
    first.requestId = "audit-first-" + std::to_string(nowMilliseconds());
    first.operatorId = "/example/uav/operator/one";
    first.droneId = targetDroneId();
    first.scope = "control";
    first.ttlMs = 60000;
    first.requestedMs = nowMilliseconds();

    OperatorAuthorityLease firstLease;
    std::string firstReason;
    const bool firstAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, first, timeout, firstLease, firstReason);
    if (firstAccepted) {
      setOperatorAuthorityLease(firstLease);
    }

    auto admin = first;
    admin.requestId = "audit-admin-" + std::to_string(nowMilliseconds());
    admin.operatorId = "/example/uav/operator/two";
    admin.scope = "admin";
    OperatorAuthorityLease adminLease;
    std::string adminReason;
    const bool adminAccepted = requestOperatorAuthorityLeaseFromIssuerSync(
      m_config.groundStationIdentity, admin, timeout, adminLease, adminReason);

    std::string refreshReason;
    const bool refreshRevoked = refreshOperatorAuthorityLeaseFromIssuer(
      m_config.groundStationIdentity, timeout, refreshReason);

    Fields auditFields;
    std::string auditReason;
    const bool auditOk = requestOperatorAuthorityAuditFromIssuerSync(
      m_config.groundStationIdentity, Fields{
        {"limit", "20"},
        {"redaction", "self"},
      }, timeout, auditFields, auditReason);

    const auto returnedCount = unsignedFieldOr(auditFields, "returned_count", 0);
    bool sawOverride = false;
    bool sawDetected = false;
    for (uint64_t i = 0; i < returnedCount; ++i) {
      const auto prefix = "alert." + std::to_string(i) + ".";
      const auto type = fieldOr(auditFields, prefix + "type", "");
      const auto leaseId = fieldOr(auditFields, prefix + "lease_id", "");
      const auto revokedOperator = fieldOr(auditFields, prefix + "revoked_operator", "");
      const auto revokerOperator = fieldOr(auditFields, prefix + "revoker_operator", "");
      if (type == "admin-override" &&
          leaseId == firstLease.leaseId &&
          revokedOperator == first.operatorId &&
          revokerOperator == admin.operatorId) {
        sawOverride = true;
      }
      if (type == "lease-revoked-detected" &&
          leaseId == firstLease.leaseId &&
          revokedOperator == first.operatorId &&
          revokerOperator == admin.operatorId) {
        sawDetected = true;
      }
    }

    const bool ok = firstAccepted && firstReason == "ok" &&
                    adminAccepted && adminReason == "ok" &&
                    refreshRevoked && refreshReason == "ok" &&
                    auditOk && auditReason == "ok" &&
                    returnedCount >= 2 && sawOverride && sawDetected;
    NDN_LOG_INFO("AUTHORITY_AUDIT_QUERY_RESULT ok=" << (ok ? "true" : "false")
                 << " first=" << (firstAccepted ? "accepted" : firstReason)
                 << " admin=" << (adminAccepted ? "accepted" : adminReason)
                 << " refresh=" << (refreshRevoked ? "revoked" : refreshReason)
                 << " audit=" << (auditOk ? "ok" : auditReason)
                 << " alert_count=" << fieldOr(auditFields, "alert_count", "0")
                 << " returned_count=" << returnedCount
                 << " saw_override=" << (sawOverride ? "true" : "false")
                 << " saw_detected=" << (sawDetected ? "true" : "false")
                 << " lease_id=" << firstLease.leaseId);
    Fields pageFields;
    std::string pageReason;
    const bool pageOk = requestOperatorAuthorityAuditFromIssuerSync(
      m_config.groundStationIdentity, Fields{
        {"offset", "1"},
        {"limit", "1"},
        {"from_ms", std::to_string(firstLease.issuedMs)},
        {"redaction", "self"},
      }, timeout, pageFields, pageReason);
    const bool pageMatchesDetected =
      pageOk &&
      pageReason == "ok" &&
      fieldOr(pageFields, "returned_count", "0") == "1" &&
      fieldOr(pageFields, "offset", "0") == "1" &&
      fieldOr(pageFields, "limit", "0") == "1" &&
      fieldOr(pageFields, "alert.0.type", "") == "lease-revoked-detected" &&
      fieldOr(pageFields, "alert.0.lease_id", "") == firstLease.leaseId;
    NDN_LOG_INFO("AUTHORITY_AUDIT_PAGE_RESULT ok=" << (pageMatchesDetected ? "true" : "false")
                 << " query=" << (pageOk ? "ok" : pageReason)
                 << " returned_count=" << fieldOr(pageFields, "returned_count", "0")
                 << " offset=" << fieldOr(pageFields, "offset", "0")
                 << " limit=" << fieldOr(pageFields, "limit", "0")
                 << " first_type=" << fieldOr(pageFields, "alert.0.type", "none")
                 << " lease_id=" << fieldOr(pageFields, "alert.0.lease_id", "none"));

    Fields redactedFields;
    std::string redactedReason;
    const bool redactedOk = requestOperatorAuthorityAuditFromIssuerSync(
      m_config.groundStationIdentity, Fields{
        {"offset", "0"},
        {"limit", "1"},
        {"redaction", "summary"},
      }, timeout, redactedFields, redactedReason);
    const bool summaryRedacted =
      redactedOk &&
      redactedReason == "ok" &&
      fieldOr(redactedFields, "redaction", "") == "summary" &&
      fieldOr(redactedFields, "returned_count", "0") == "1" &&
      fieldOr(redactedFields, "alert.0.type", "") == "admin-override" &&
      fieldOr(redactedFields, "alert.0.lease_id", "") == firstLease.leaseId &&
      fieldOr(redactedFields, "alert.0.drone", "") == first.droneId &&
      fieldOr(redactedFields, "alert.0.scope", "") == first.scope &&
      fieldOr(redactedFields, "alert.0.revoked_operator", "") == "redacted" &&
      fieldOr(redactedFields, "alert.0.revoker_operator", "") == "redacted" &&
      fieldOr(redactedFields, "alert.0.redacted", "") == "true";
    NDN_LOG_INFO("AUTHORITY_AUDIT_REDACTION_RESULT ok=" << (summaryRedacted ? "true" : "false")
                 << " query=" << (redactedOk ? "ok" : redactedReason)
                 << " redaction=" << fieldOr(redactedFields, "redaction", "none")
                 << " returned_count=" << fieldOr(redactedFields, "returned_count", "0")
                 << " first_type=" << fieldOr(redactedFields, "alert.0.type", "none")
                 << " revoked_operator=" << fieldOr(redactedFields, "alert.0.revoked_operator", "none")
                 << " revoker_operator=" << fieldOr(redactedFields, "alert.0.revoker_operator", "none")
                 << " redacted=" << fieldOr(redactedFields, "alert.0.redacted", "false"));

    Fields selfFields;
    std::string selfReason;
    const bool selfOk = requestOperatorAuthorityAuditFromIssuerSync(
      m_config.groundStationIdentity, Fields{
        {"offset", "0"},
        {"limit", "1"},
        {"redaction", "self"},
        {"requester_operator", "/example/uav/operator/not-involved"},
      }, timeout, selfFields, selfReason);
    const bool identityPreferred =
      selfOk &&
      selfReason == "ok" &&
      fieldOr(selfFields, "redaction", "") == "self" &&
      fieldOr(selfFields, "requester_operator_source", "") == "requester-identity" &&
      fieldOr(selfFields, "effective_requester_operator", "") == first.operatorId &&
      fieldOr(selfFields, "alert.0.type", "") == "admin-override" &&
      fieldOr(selfFields, "alert.0.lease_id", "") == firstLease.leaseId &&
      fieldOr(selfFields, "alert.0.revoked_operator", "") == first.operatorId &&
      fieldOr(selfFields, "alert.0.revoker_operator", "") == admin.operatorId &&
      fieldOr(selfFields, "alert.0.redacted", "") == "false";
    NDN_LOG_INFO("AUTHORITY_AUDIT_IDENTITY_RESULT ok=" << (identityPreferred ? "true" : "false")
                 << " query=" << (selfOk ? "ok" : selfReason)
                 << " redaction=" << fieldOr(selfFields, "redaction", "none")
                 << " source=" << fieldOr(selfFields, "requester_operator_source", "none")
                 << " requester_identity=" << fieldOr(selfFields, "requester_identity", "none")
                 << " effective_requester_operator="
                 << fieldOr(selfFields, "effective_requester_operator", "none")
                 << " payload_requester_operator=/example/uav/operator/not-involved"
                 << " revoked_operator=" << fieldOr(selfFields, "alert.0.revoked_operator", "none")
                 << " revoker_operator=" << fieldOr(selfFields, "alert.0.revoker_operator", "none")
                 << " redacted=" << fieldOr(selfFields, "alert.0.redacted", "false"));

    Fields fullDeniedFields;
    std::string fullDeniedReason;
    const bool fullDeniedOk = requestOperatorAuthorityAuditFromIssuerSync(
      m_config.groundStationIdentity, Fields{
        {"offset", "0"},
        {"limit", "1"},
        {"redaction", "full"},
      }, timeout, fullDeniedFields, fullDeniedReason);
    const bool fullDenied =
      !fullDeniedOk &&
      fullDeniedReason == "full-redaction-requires-admin" &&
      fieldOr(fullDeniedFields, "redaction", "") == "full" &&
      fieldOr(fullDeniedFields, "requester_operator_source", "") == "requester-identity" &&
      fieldOr(fullDeniedFields, "effective_requester_operator", "") == first.operatorId;
    NDN_LOG_INFO("AUTHORITY_AUDIT_FULL_GATE_RESULT ok=" << (fullDenied ? "true" : "false")
                 << " query=" << (fullDeniedOk ? "unexpected-ok" : fullDeniedReason)
                 << " redaction=" << fieldOr(fullDeniedFields, "redaction", "none")
                 << " source=" << fieldOr(fullDeniedFields, "requester_operator_source", "none")
                 << " effective_requester_operator="
                 << fieldOr(fullDeniedFields, "effective_requester_operator", "none")
                 << " admin=" << (operatorHasAdminAuthority(first.operatorId) ? "true" : "false"));
    return ok && pageMatchesDetected && summaryRedacted && identityPreferred && fullDenied;
  }

  bool
  uploadMissionPlan(MissionPlan plan, std::chrono::seconds timeout)
  {
    struct UploadState
    {
      std::mutex mutex;
      std::condition_variable cv;
      size_t completed = 0;
      size_t failed = 0;
      std::map<std::string, MissionPart> parts;
    };

    if (plan.parts.empty()) {
      publishStatus("Mission plan upload failed: no parts");
      return false;
    }
    if (plan.taskId.empty()) {
      plan.taskId = "loaded-plan-" + std::to_string(nowMilliseconds());
    }
    if (plan.completionObjective.empty()) {
      plan.completionObjective = "return-to-start";
    }
    plan.returnHomePlanned = plan.returnHomePlanned || plan.completionObjective == "return-to-start";
    updateMissionPlan(plan);

    const auto state = std::make_shared<UploadState>();
    {
      std::lock_guard<std::mutex> guard(m_missionReadyMutex);
      m_missionReadyDrones.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      m_activePatrolTaskId = plan.taskId;
      m_patrolCancelRequested = false;
    }
    for (size_t i = 0; i < plan.parts.size(); ++i) {
      auto part = plan.parts[i];
      if (part.id.empty()) {
        part.id = "part-" + std::to_string(i + 1);
      }
      if (part.assignedDrone.empty() && !m_patrolDroneIds.empty()) {
        part.assignedDrone = m_patrolDroneIds[i % m_patrolDroneIds.size()];
      }
      state->parts.emplace(part.id, std::move(part));
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
    auto parseDroneIds = [this] (const std::string& droneIds, size_t fallbackIndex) {
      std::vector<std::string> parsed;
      std::string token;
      std::istringstream stream(droneIds);
      while (std::getline(stream, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t\r\n"));
        const auto last = token.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) {
          continue;
        }
        token.erase(last + 1);
        if (!token.empty() && token != "unknown" && token != "none") {
          parsed.push_back(token);
        }
      }
      if (parsed.empty() && !m_patrolDroneIds.empty()) {
        parsed.push_back(m_patrolDroneIds[fallbackIndex % m_patrolDroneIds.size()]);
      }
      return parsed;
    };
    for (const auto& item : state->parts) {
      const auto candidateDrones = parseDroneIds(item.second.assignedDrone, 0);
      std::string leaseReason;
      if (!validateMissionLeaseForDrones(candidateDrones, leaseReason)) {
        publishStatus("Mission upload blocked by operator lease part=" + item.first +
                      " reason=" + leaseReason);
        NDN_LOG_INFO("MISSION_PLAN_UPLOAD_LEASE_BLOCKED task=" << plan.taskId
                     << " part=" << item.first
                     << " reason=" << leaseReason);
        return false;
      }
    }
    auto emitProgress = [this, state, taskId = plan.taskId, plan] (std::string phase) {
      MissionProgressState progress;
      progress.taskId = taskId;
      progress.phase = std::move(phase);
      progress.assignment = plan.assignment.empty() ? "loaded-mission-plan" : plan.assignment;
      progress.completionObjective = plan.completionObjective;
      progress.returnHomePlanned = plan.returnHomePlanned;
      progress.attempts = 1;
      progress.completedPartIds = "none";
      progress.pendingPartIds = "none";
      progress.missingPartIds = "none";
      progress.compensatedPartIds = "none";
      auto appendId = [] (std::string& list, const std::string& id) {
        if (list == "none") {
          list.clear();
        }
        if (!list.empty()) {
          list += ",";
        }
        list += id;
      };
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        progress.totalParts = state->parts.size();
        for (const auto& item : state->parts) {
          if (item.second.done) {
            ++progress.completedParts;
            appendId(progress.completedPartIds, item.first);
          }
          else {
            appendId(progress.pendingPartIds, item.first);
          }
        }
      }
      updateMissionProgress(progress);
      NDN_LOG_INFO("MISSION_PLAN_UPLOAD_PROGRESS " << progress.statusLine());
    };
    auto isCancelled = [this, taskId = plan.taskId] {
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      return m_patrolCancelRequested && m_activePatrolTaskId == taskId;
    };

    NDN_LOG_INFO("MISSION_PLAN_UPLOAD_START task=" << plan.taskId
                 << " parts=" << state->parts.size()
                 << " assignment=" << plan.assignment);
    emitProgress("assigning");

    size_t dispatchIndex = 0;
    for (const auto& item : state->parts) {
      const auto partId = item.first;
      const auto part = item.second;
      const auto candidateDrones = parseDroneIds(part.assignedDrone, dispatchIndex++);
      if (candidateDrones.empty()) {
        std::lock_guard<std::mutex> guard(state->mutex);
        ++state->failed;
        state->cv.notify_all();
        continue;
      }
      const auto candidateText = joinDroneIds(candidateDrones);
      Fields payloadFields{
        {"type", "patrol-task"},
        {"patrol_task_id", plan.taskId},
        {"mission_id", plan.taskId},
        {"mission_completion_objective", plan.completionObjective},
        {"attempt_id", std::to_string(std::max(1, part.attempt))},
        {"part_id", part.id},
        {"role", part.role},
        {"area", "loaded-mission-plan"},
        {"waypoints", part.waypointText()},
        {"capture_required", "true"},
        {"simulate_no_response", "false"},
        {"simulate_delay_ms", "0"},
      };
      if (candidateDrones.size() == 1) {
        payloadFields.emplace("target_system", mavlinkTargetSystemForDrone(candidateDrones.front()));
        payloadFields.emplace("target_component", "1");
      }
      auto requestMessage = makeRequest(encodeFields(payloadFields));
      std::vector<ndn::Name> providerNames;
      providerNames.reserve(candidateDrones.size());
      for (const auto& droneId : candidateDrones) {
        providerNames.push_back(droneIdentity(m_config, droneId));
      }

      boost::asio::post(m_face.getIoContext(), [this, requestMessage = std::move(requestMessage),
                                  providerNames = std::move(providerNames),
                                  partId, candidateText, state, emitProgress,
                                  isCancelled, taskId = plan.taskId] () mutable {
        if (isCancelled() || !m_containerReady.load() || !m_user) {
          std::lock_guard<std::mutex> guard(state->mutex);
          ++state->failed;
          state->cv.notify_all();
          return;
        }
        auto selectIdleCandidate =
          [providerNames, taskId, partId](
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
                continue;
              }
              const auto payload = candidate.ack.getPayload();
              const auto fields = decodeFields(
                std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
              if (fieldOr(fields, "mission_busy", "false") == "true") {
                NDN_LOG_INFO("MISSION_PLAN_UPLOAD_ACK_BUSY task=" << taskId
                             << " part=" << partId
                             << " provider=" << candidate.providerName);
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
          [partId, state, emitProgress, taskId](const ndn::Name&) {
            NDN_LOG_INFO("MISSION_PLAN_UPLOAD_PART_TIMEOUT task=" << taskId
                         << " part=" << partId);
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              ++state->failed;
            }
            emitProgress("assigning");
            state->cv.notify_all();
          },
          [this, partId, candidateText, state, emitProgress, taskId](
            const ndn_service_framework::ResponseMessage& response) {
            const auto fields = decodeFields(responsePayload(response));
            auto mission = MissionState::fromFields(fields);
            const auto responder = mission.droneId == "unknown" ? candidateText : mission.droneId;
            if (mission.droneId == "unknown") {
              mission.droneId = responder;
            }
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              if (response.getStatus()) {
                auto& part = state->parts[partId];
                if (!part.done) {
                  part.done = true;
                  part.completedBy = responder;
                  ++state->completed;
                }
              }
              else {
                ++state->failed;
              }
            }
            if (response.getStatus()) {
              updateMissionState(mission);
              std::lock_guard<std::mutex> readyGuard(m_missionReadyMutex);
              if (std::find(m_missionReadyDrones.begin(), m_missionReadyDrones.end(),
                            responder) == m_missionReadyDrones.end()) {
                m_missionReadyDrones.push_back(responder);
              }
              NDN_LOG_INFO("MISSION_PLAN_UPLOAD_PART_DONE task=" << taskId
                           << " part=" << partId
                           << " provider=" << responder
                           << " phase=" << mission.phase);
            }
            emitProgress("assigning");
            state->cv.notify_all();
          });
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool ok = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, [&] {
        return isCancelled() || state->completed + state->failed >= state->parts.size();
      });
      ok = !isCancelled() && state->failed == 0 && state->completed == state->parts.size();
    }
    emitProgress(ok ? "completed" : (isCancelled() ? "cancelled" : "failed"));
    NDN_LOG_INFO("MISSION_PLAN_UPLOAD_DONE task=" << plan.taskId
                 << " ok=" << (ok ? "true" : "false"));
    {
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      if (m_activePatrolTaskId == plan.taskId) {
        m_activePatrolTaskId.clear();
      }
      m_patrolCancelRequested = false;
    }
    return ok;
  }

  bool
  cancelCurrentPatrolMission()
  {
    std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
    if (m_activePatrolTaskId.empty()) {
      return false;
    }
    m_patrolCancelRequested = true;
    NDN_LOG_INFO("PATROL_CANCEL_REQUEST task=" << m_activePatrolTaskId);
    return true;
  }

  bool
  cancelPatrolMission(const std::string& taskId)
  {
    std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
    if (taskId.empty() || taskId != m_activePatrolTaskId) {
      return false;
    }
    m_patrolCancelRequested = true;
    NDN_LOG_INFO("PATROL_CANCEL_REQUEST task=" << taskId);
    return true;
  }

  std::optional<std::string>
  activePatrolMissionId() const
  {
    std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
    if (m_activePatrolTaskId.empty()) {
      return std::nullopt;
    }
    return m_activePatrolTaskId;
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
      bool cancelled = false;
    };

    if (m_patrolDroneIds.size() < 2) {
      publishStatus("Patrol demo needs at least two drones");
      return false;
    }
    std::string leaseReason;
    if (!validateMissionLeaseForDrones(m_patrolDroneIds, leaseReason)) {
      publishStatus("Patrol mission blocked by operator lease reason=" + leaseReason);
      NDN_LOG_INFO("PATROL_LEASE_BLOCKED reason=" << leaseReason);
      return false;
    }

    const std::string taskId = "patrol-" + std::to_string(nowMilliseconds());
    {
      std::lock_guard<std::mutex> guard(m_missionReadyMutex);
      m_missionReadyDrones.clear();
    }

    {
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      m_activePatrolTaskId = taskId;
      m_patrolCancelRequested = false;
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
    const std::string completionObjective = "return-to-start";
    std::map<std::string, MissionWaypoint> departurePoints;
    for (const auto& part : initialPlan.parts) {
      if (part.assignedDrone.empty() || departurePoints.count(part.assignedDrone) > 0) {
        continue;
      }
      const auto routeStart = part.firstWaypointOr(MissionWaypoint{centerLat, centerLon});
      departurePoints.emplace(part.assignedDrone, departurePointForDrone(part.assignedDrone, routeStart));
    }
    auto plan = buildPatrolMissionPlan(taskId, centerLat, centerLon, sideMeters,
                                             m_patrolDroneIds, missionRouteWaypoints,
                                             departurePoints);
    // Ensure every generated mission plan carries an explicit completion objective.
    // Return-to-start is the default behavior for patrol demos.
    plan.completionObjective = completionObjective;
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

    struct CompensationTaskObject
    {
      std::string taskId;
      uint64_t attempt = 1;
      std::vector<std::string> partIds;
      std::vector<std::string> candidateDrones;
    };

    auto buildCompensationTaskObjects = [&] (const std::string& baseTaskId,
                                            const std::vector<std::string>& partIds,
                                            uint64_t attempt,
                                            const std::vector<std::string>& candidateDrones) {
      std::vector<CompensationTaskObject> tasks;
      tasks.reserve(partIds.size());
      for (const auto& partId : partIds) {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->parts.find(partId) == state->parts.end()) {
          continue;
        }
        CompensationTaskObject task;
        task.taskId = baseTaskId + "/comp/attempt-" + std::to_string(attempt) + "/part-" + partId;
        task.attempt = attempt;
        task.partIds.push_back(partId);
        task.candidateDrones = candidateDrones;
        tasks.push_back(std::move(task));
      }
      return tasks;
    };

    auto buildCompensationPlanView = [&state, completionObjective] (
                                      const std::string& baseTaskId,
                                      uint64_t attempt,
                                      const std::vector<std::string>& missingPartIds) {
      MissionPlan view;
      view.taskId = baseTaskId + "/comp-plan/" + std::to_string(attempt);
      view.assignment = "compensation";
      view.completionObjective = completionObjective;
      view.returnHomePlanned = true;

      std::lock_guard<std::mutex> guard(state->mutex);
      for (const auto& partId : missingPartIds) {
        const auto it = state->parts.find(partId);
        if (it == state->parts.end()) {
          continue;
        }
        auto compensationPart = it->second;
        compensationPart.id = partId + "/retry/" + std::to_string(attempt);
        compensationPart.attempt = attempt;
        view.parts.push_back(std::move(compensationPart));
      }
      return view;
    };

    auto emitProgress = [this, state, taskId, assignmentMode, patrolDroneText, completionObjective, logLedger](
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
      progress.completionObjective = completionObjective;
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

    auto isTaskCancelled = [this, state, taskId] {
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->cancelled) {
          return true;
        }
      }
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      return m_patrolCancelRequested && m_activePatrolTaskId == taskId;
    };

    auto parseDroneIds = [] (const std::string& droneIds) {
      std::vector<std::string> parsed;
      if (droneIds.empty() || droneIds == "none") {
        return parsed;
      }
      std::string token;
      std::istringstream stream(droneIds);
      while (std::getline(stream, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t\r\n"));
        token.erase(token.find_last_not_of(" \t\r\n") + 1);
        if (!token.empty()) {
          parsed.push_back(token);
        }
      }
      return parsed;
    };

    auto applyPatrolCancel = [this, state, taskId, completionObjective, patrolDroneText,
                             logLedger, parseDroneIds] (uint64_t attempts) {
      bool firstFinalize = false;
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->cancelled) {
          return;
        }
        state->cancelled = true;
        firstFinalize = true;
      }
      if (!firstFinalize) {
        return;
      }

      logLedger("PATROL_TASK_CANCEL_FINALIZE task=" + taskId + " attempts=" + std::to_string(attempts));

      {
        std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
        if (m_activePatrolTaskId == taskId) {
          m_patrolCancelRequested = false;
        }
      }

      std::vector<std::string> candidateDrones = parseDroneIds(patrolDroneText);
      if (candidateDrones.empty()) {
        candidateDrones = m_patrolDroneIds;
      }
      std::set<std::string> updated;
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        for (const auto& item : state->parts) {
          const auto& part = item.second;
          const auto fromPart = parseDroneIds(part.assignedDrone);
          if (!fromPart.empty()) {
            for (const auto& droneId : fromPart) {
              updated.insert(droneId);
            }
          }
        }
      }
      for (const auto& droneId : candidateDrones) {
        updated.insert(droneId);
      }

      const auto now = nowMilliseconds();
      for (const auto& droneId : updated) {
        MissionState cancelState;
        cancelState.droneId = droneId;
        cancelState.missionId = taskId;
        cancelState.partId = "cancel";
        cancelState.phase = "cancelled";
        cancelState.detail = "user-cancel";
        cancelState.ack = "user_requested";
        cancelState.transport = completionObjective;
        cancelState.updatedMs = now;
        updateMissionState(cancelState);
      }
    };

    auto markCancel = [this, state, taskId, logLedger] {
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->cancelled = true;
      }
      {
        std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
        if (m_activePatrolTaskId == taskId) {
          m_patrolCancelRequested = false;
        }
      }
      logLedger("PATROL_CANCELLED task=" + taskId);
    };

    auto dispatchPart = [&] (const std::string& partId, std::vector<std::string> droneIds,
                             int attempt, bool simulateNoResponse) {
      if (isTaskCancelled()) {
        logLedger("PATROL_DISPATCH_SKIPPED task=" + taskId +
                  " attempt=" + std::to_string(attempt) +
                  " part=" + partId + " reason=cancelled");
        return;
      }
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
        {"mission_completion_objective", completionObjective},
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
                                  attempt, state, isTaskCancelled,
                                  logLedger, emitProgress] () mutable {
        if (isTaskCancelled()) {
          logLedger("PATROL_DISPATCH_SKIPPED task=" + taskId +
                    " attempt=" + std::to_string(attempt) +
                    " part=" + partId + " reason=cancelled");
          return;
        }
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
          [taskId, partId, candidateText, attempt, state, isTaskCancelled,
           logLedger, emitProgress](const ndn::Name&) {
            if (isTaskCancelled()) {
              logLedger("PATROL_PART_TIMEOUT_IGNORED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId + " reason=cancelled");
              return;
            }
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
          [this, taskId, partId, candidateText, attempt, state, isTaskCancelled,
           logLedger, emitProgress](
            const ndn_service_framework::ResponseMessage& response) {
            if (isTaskCancelled()) {
              logLedger("PATROL_PART_DONE_IGNORED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId + " reason=cancelled");
              state->cv.notify_all();
              return;
            }
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
        if (state->cancelled || isTaskCancelled()) {
          return true;
        }
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
      markCancel();
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      if (m_activePatrolTaskId == taskId) {
        m_activePatrolTaskId.clear();
      }
      return true;
    }
    if (isTaskCancelled()) {
      emitProgress("cancelled", 1);
      logLedger("PATROL_TASK_CANCELLED task=" + taskId);
      applyPatrolCancel(1);
      {
        std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
        if (m_activePatrolTaskId == taskId) {
          m_activePatrolTaskId.clear();
        }
      }
      return false;
    }

    const auto compensationTasks = buildCompensationTaskObjects(taskId, missingParts, 2,
                                                              m_patrolDroneIds);
    emitProgress("waiting-compensation", 1);
    if (!compensationTasks.empty()) {
      const auto compensationPlan = buildCompensationPlanView(taskId, 2, missingParts);
      updateMissionPlan(compensationPlan);
      for (const auto& compensationTask : compensationTasks) {
        std::string partList;
        for (const auto& partId : compensationTask.partIds) {
          if (!partList.empty()) {
            partList += ",";
          }
          partList += partId;
        }
        logLedger("PATROL_COMP_TASK task=" + compensationTask.taskId +
                  " attempt=" + std::to_string(compensationTask.attempt) +
                  " parts=" + partList +
                  " candidates=" + joinDroneIds(compensationTask.candidateDrones) +
                  " plan=" + compensationPlan.statusLine());
      }
    }

    for (const auto& compensationTask : compensationTasks) {
      for (const auto& partId : compensationTask.partIds) {
        if (isTaskCancelled()) {
          emitProgress("cancelled", 2);
          logLedger("PATROL_TASK_CANCELLED task=" + taskId +
                    " attempt=" + std::to_string(compensationTask.attempt));
          applyPatrolCancel(compensationTask.attempt);
          std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
          if (m_activePatrolTaskId == taskId) {
            m_activePatrolTaskId.clear();
          }
          return false;
        }
        logLedger("PATROL_COMPENSATION task=" + compensationTask.taskId +
                  " attempt=" + std::to_string(compensationTask.attempt) +
                  " parts=" + partId +
                  " candidates=" + joinDroneIds(compensationTask.candidateDrones));
        emitProgress("compensating", 2);
        dispatchPart(partId, compensationTask.candidateDrones, 2, false);
      }
    }
    if (compensationTasks.empty()) {
      for (const auto& partId : missingParts) {
        logLedger("PATROL_COMPENSATION task=" + taskId +
                  " attempt=2 parts=" + partId +
                  " candidates=" + patrolDroneText +
                  " fallback=all-drones");
        emitProgress("compensating", 2);
        dispatchPart(partId, m_patrolDroneIds, 2, false);
      }
    }

    bool failed = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, [&] {
        if (state->cancelled || isTaskCancelled()) {
          return true;
        }
        return allDone();
      });
      if (!allDone()) {
        failed = true;
      }
    }
    if (failed) {
      emitProgress("failed", 2);
      logLedger("PATROL_TASK_FAILED task=" + taskId);
      markCancel();
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      if (m_activePatrolTaskId == taskId) {
        m_activePatrolTaskId.clear();
      }
      return false;
    }
    if (isTaskCancelled()) {
      emitProgress("cancelled", 2);
      logLedger("PATROL_TASK_CANCELLED task=" + taskId);
      applyPatrolCancel(2);
      std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
      if (m_activePatrolTaskId == taskId) {
        m_activePatrolTaskId.clear();
      }
      return false;
    }
    emitProgress("completed", 2);
    logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=2");
    markCancel();
    std::lock_guard<std::mutex> guard(m_patrolTaskMutex);
    if (m_activePatrolTaskId == taskId) {
      m_activePatrolTaskId.clear();
    }
    return true;
  }

private:
  void
  issueDefaultOperatorLease()
  {
    OperatorAuthorityLease lease;
    lease.leaseId = "default-gs-" + m_defaultOperatorLeaseScope + "-" +
                    m_defaultOperatorLeaseDrone;
    lease.operatorId = m_operatorId.empty() ? m_config.groundStationIdentity.toUri() : m_operatorId;
    lease.droneId = m_defaultOperatorLeaseDrone;
    lease.scope = m_defaultOperatorLeaseScope;
    lease.issuedMs = nowMilliseconds();
    lease.expiresMs = m_defaultOperatorLeaseTtlMs == 0 ?
                      0 : lease.issuedMs + m_defaultOperatorLeaseTtlMs;
    {
      std::lock_guard<std::mutex> guard(m_operatorLeaseMutex);
      m_operatorLease = std::move(lease);
    }
  }

  bool
  requestOperatorAuthorityLeaseFromIssuerSync(const ndn::Name& issuerIdentity,
                                              const OperatorAuthorityLeaseRequest& leaseRequest,
                                              std::chrono::seconds timeout,
                                              OperatorAuthorityLease& out,
                                              std::string& reason,
                                              Fields* responseFields = nullptr)
  {
    struct RequestState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      bool ok = false;
      std::string reason = "timeout";
      OperatorAuthorityLease lease;
      Fields fields;
    };

    auto state = std::make_shared<RequestState>();
    boost::asio::post(m_face.getIoContext(), [this, issuerIdentity, leaseRequest, state] {
      if (!m_containerReady.load() || !m_user) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->done = true;
        state->ok = false;
        state->reason = "runtime-not-ready";
        state->cv.notify_one();
        return;
      }

      auto requestMessage = makeRequest(encodeFields(leaseRequest.toFields()));
      m_user->RequestService(
        std::vector<ndn::Name>{issuerIdentity},
        m_config.serviceGsOperatorAuthorityLease,
        std::move(requestMessage),
        m_ackTimeoutMs,
        ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
        m_timeoutMs,
        [state](const ndn::Name&) {
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->ok = false;
          state->reason = "request-timeout";
          state->cv.notify_one();
        },
        [state](const ndn_service_framework::ResponseMessage& response) {
          const auto fields = decodeFields(responsePayload(response));
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->reason = fieldOr(fields, "reason", "unknown");
          state->ok = fieldOr(fields, "accepted", "false") == "true";
          state->fields = fields;
          if (state->ok) {
            state->lease = OperatorAuthorityLease::fromFields(fields);
          }
          state->cv.notify_one();
        });
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&state] { return state->done; });
    out = state->lease;
    reason = state->reason;
    if (responseFields != nullptr) {
      *responseFields = state->fields;
    }
    return state->done && state->ok;
  }

  bool
  requestOperatorRevocationRecordFromIssuerSync(const ndn::Name& issuerIdentity,
                                                const std::string& revokedLeaseId,
                                                std::chrono::seconds timeout,
                                                Fields& out,
                                                std::string& reason)
  {
    struct RequestState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      bool found = false;
      std::string reason = "timeout";
      Fields fields;
    };

    auto state = std::make_shared<RequestState>();
    boost::asio::post(m_face.getIoContext(), [this, issuerIdentity, revokedLeaseId, state] {
      if (!m_containerReady.load() || !m_user) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->done = true;
        state->found = false;
        state->reason = "runtime-not-ready";
        state->cv.notify_one();
        return;
      }

      auto requestMessage = makeRequest(encodeFields(Fields{
        {"type", "operator-authority-revocation-query"},
        {"revoked_lease_id", revokedLeaseId},
      }));
      m_user->RequestService(
        std::vector<ndn::Name>{issuerIdentity},
        m_config.serviceGsOperatorAuthorityRevocation,
        std::move(requestMessage),
        m_ackTimeoutMs,
        ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
        m_timeoutMs,
        [state](const ndn::Name&) {
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->found = false;
          state->reason = "request-timeout";
          state->cv.notify_one();
        },
        [state](const ndn_service_framework::ResponseMessage& response) {
          const auto fields = decodeFields(responsePayload(response));
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->fields = fields;
          state->reason = fieldOr(fields, "reason", "unknown");
          state->found = fieldOr(fields, "found", "false") == "true";
          state->cv.notify_one();
        });
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&state] { return state->done; });
    out = state->fields;
    reason = state->reason;
    return state->done && state->found;
  }

  bool
  requestOperatorAuthorityAuditFromIssuerSync(const ndn::Name& issuerIdentity,
                                              Fields query,
                                              std::chrono::seconds timeout,
                                              Fields& out,
                                              std::string& reason)
  {
    struct RequestState
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      bool ok = false;
      std::string reason = "timeout";
      Fields fields;
    };

    auto state = std::make_shared<RequestState>();
    boost::asio::post(m_face.getIoContext(), [this, issuerIdentity, query = std::move(query), state] {
      if (!m_containerReady.load() || !m_user) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->done = true;
        state->ok = false;
        state->reason = "runtime-not-ready";
        state->cv.notify_one();
        return;
      }

      auto requestFields = query;
      requestFields["type"] = "operator-authority-audit-query";
      auto requestMessage = makeRequest(encodeFields(requestFields));
      m_user->RequestService(
        std::vector<ndn::Name>{issuerIdentity},
        m_config.serviceGsOperatorAuthorityAudit,
        std::move(requestMessage),
        m_ackTimeoutMs,
        ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
        m_timeoutMs,
        [state](const ndn::Name&) {
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->ok = false;
          state->reason = "request-timeout";
          state->cv.notify_one();
        },
        [state](const ndn_service_framework::ResponseMessage& response) {
          const auto fields = decodeFields(responsePayload(response));
          std::lock_guard<std::mutex> guard(state->mutex);
          state->done = true;
          state->fields = fields;
          state->reason = fieldOr(fields, "reason", "unknown");
          state->ok = fieldOr(fields, "ok", "false") == "true";
          state->cv.notify_one();
        });
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&state] { return state->done; });
    out = state->fields;
    reason = state->reason;
    return state->done && state->ok;
  }

  static bool
  isExclusiveAuthorityScope(const std::string& scope)
  {
    return scope == "control" || scope == "mission" || scope == "admin";
  }

  static bool
  leaseTargetsOverlap(const std::string& left, const std::string& right)
  {
    return left == "all" || right == "all" || left == right;
  }

  static std::set<std::string>
  parseOperatorIdList(const std::string& text)
  {
    auto trimText = [] (std::string value) {
      const auto begin = value.find_first_not_of(" \t\r\n");
      if (begin == std::string::npos) {
        return std::string();
      }
      const auto end = value.find_last_not_of(" \t\r\n");
      return value.substr(begin, end - begin + 1);
    };
    std::set<std::string> output;
    std::string current;
    auto flush = [&] {
      current = trimText(current);
      if (!current.empty()) {
        output.insert(current);
      }
      current.clear();
    };
    for (const auto ch : text) {
      if (ch == ',' || ch == ';' || ch == ' ') {
        flush();
      }
      else {
        current.push_back(ch);
      }
    }
    flush();
    return output;
  }

  static std::string
  joinTextList(const std::vector<std::string>& values)
  {
    std::ostringstream os;
    for (size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        os << ",";
      }
      os << values[i];
    }
    return os.str();
  }

  static uint64_t
  unsignedFieldOr(const Fields& fields, const std::string& key, uint64_t fallback)
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

  bool
  operatorHasAdminAuthority(const std::string& operatorId) const
  {
    return m_operatorAdminIds.empty() || m_operatorAdminIds.count(operatorId) != 0;
  }

  std::string
  operatorIdForRequesterIdentity(const ndn::Name& requesterIdentity) const
  {
    const auto requester = requesterIdentity.toUri();
    if (requester == m_config.groundStationIdentity.toUri()) {
      return m_operatorId;
    }
    if (requester.find("/operator/") != std::string::npos) {
      return requester;
    }
    if (requester.find("/Operator/") != std::string::npos) {
      return requester;
    }
    return "";
  }

  void
  appendOperatorAuthorityAlert(OperatorAuthorityAlert alert)
  {
    if (alert.updatedMs == 0) {
      alert.updatedMs = nowMilliseconds();
    }
    std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
    m_operatorAuthorityAlerts.push_back(std::move(alert));
    while (m_operatorAuthorityAlerts.size() > 20) {
      m_operatorAuthorityAlerts.erase(m_operatorAuthorityAlerts.begin());
    }
    const auto& latest = m_operatorAuthorityAlerts.back();
    NDN_LOG_INFO("AUTHORITY_ALERT type=" << latest.type
                 << " lease_id=" << latest.leaseId
                 << " revoked_operator=" << latest.revokedOperator
                 << " revoker_operator=" << latest.revokerOperator
                 << " drone=" << latest.droneId
                 << " scope=" << latest.scope
                 << " reason=" << latest.reason
                 << " count=" << m_operatorAuthorityAlerts.size());
  }

  void
  persistIssuedOperatorLeasesLocked() const
  {
    if (m_operatorAuthorityStateFile.empty()) {
      return;
    }
    const auto tmp = m_operatorAuthorityStateFile + ".tmp";
    std::ofstream output(tmp, std::ios::trunc);
    if (!output) {
      NDN_LOG_WARN("AUTHORITY_STATE_SAVE_FAILED path=" << m_operatorAuthorityStateFile);
      return;
    }
    output << "# NDNSF-UAV operator authority active leases\n";
    output << "lease_count=" << m_issuedOperatorLeases.size() << "\n";
    for (size_t i = 0; i < m_issuedOperatorLeases.size(); ++i) {
      const auto fields = m_issuedOperatorLeases[i].toFields();
      const auto prefix = "lease." + std::to_string(i) + ".";
      for (const auto& [key, value] : fields) {
        output << prefix << key << "=" << value << "\n";
      }
    }
    output << "revocation_count=" << m_operatorRevocationRecords.size() << "\n";
    size_t revocationIndex = 0;
    for (const auto& [leaseId, record] : m_operatorRevocationRecords) {
      const auto prefix = "revocation." + std::to_string(revocationIndex++) + ".";
      output << prefix << "key=" << leaseId << "\n";
      for (const auto& [key, value] : record) {
        output << prefix << key << "=" << value << "\n";
      }
    }
    std::vector<OperatorAuthorityAlert> alerts;
    {
      std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
      alerts = m_operatorAuthorityAlerts;
    }
    output << "alert_count=" << alerts.size() << "\n";
    for (size_t i = 0; i < alerts.size(); ++i) {
      const auto prefix = "alert." + std::to_string(i) + ".";
      output << prefix << "type=" << alerts[i].type << "\n";
      output << prefix << "reason=" << alerts[i].reason << "\n";
      output << prefix << "lease_id=" << alerts[i].leaseId << "\n";
      output << prefix << "revoked_operator=" << alerts[i].revokedOperator << "\n";
      output << prefix << "revoker_operator=" << alerts[i].revokerOperator << "\n";
      output << prefix << "drone=" << alerts[i].droneId << "\n";
      output << prefix << "scope=" << alerts[i].scope << "\n";
      output << prefix << "updated_ms=" << alerts[i].updatedMs << "\n";
    }
    output.close();
    if (std::rename(tmp.c_str(), m_operatorAuthorityStateFile.c_str()) != 0) {
      NDN_LOG_WARN("AUTHORITY_STATE_RENAME_FAILED path=" << m_operatorAuthorityStateFile
                   << " errno=" << errno);
      std::remove(tmp.c_str());
    }
    else {
      NDN_LOG_INFO("AUTHORITY_STATE_SAVED path=" << m_operatorAuthorityStateFile
                   << " active_lease_count=" << m_issuedOperatorLeases.size()
                   << " revocation_count=" << m_operatorRevocationRecords.size()
                   << " alert_count=" << alerts.size());
    }
  }

  void
  loadIssuedOperatorLeasesFromStateFile()
  {
    if (m_operatorAuthorityStateFile.empty()) {
      return;
    }
    std::ifstream probe(m_operatorAuthorityStateFile);
    if (!probe) {
      NDN_LOG_INFO("AUTHORITY_STATE_LOAD_EMPTY path=" << m_operatorAuthorityStateFile);
      return;
    }
    probe.close();

    try {
      const auto fields = loadKeyValueConfig(m_operatorAuthorityStateFile);
      const auto count = unsignedFieldOr(fields, "lease_count", 0);
      const auto revocationCount = unsignedFieldOr(fields, "revocation_count", 0);
      const auto alertCount = unsignedFieldOr(fields, "alert_count", 0);
      const auto nowMs = nowMilliseconds();
      std::vector<OperatorAuthorityLease> loaded;
      std::map<std::string, Fields> loadedRevocations;
      std::vector<OperatorAuthorityAlert> loadedAlerts;
      for (uint64_t i = 0; i < count; ++i) {
        Fields leaseFields;
        const auto prefix = "lease." + std::to_string(i) + ".";
        for (const auto& [key, value] : fields) {
          if (key.rfind(prefix, 0) == 0) {
            leaseFields[key.substr(prefix.size())] = value;
          }
        }
        auto lease = OperatorAuthorityLease::fromFields(leaseFields);
        if (lease.isFresh(nowMs)) {
          loaded.push_back(std::move(lease));
        }
      }
      for (uint64_t i = 0; i < revocationCount; ++i) {
        Fields record;
        const auto prefix = "revocation." + std::to_string(i) + ".";
        for (const auto& [key, value] : fields) {
          if (key.rfind(prefix, 0) == 0) {
            record[key.substr(prefix.size())] = value;
          }
        }
        const auto leaseId = fieldOr(record, "revoked_lease_id", fieldOr(record, "key", ""));
        if (!leaseId.empty()) {
          record.erase("key");
          loadedRevocations[leaseId] = std::move(record);
        }
      }
      for (uint64_t i = 0; i < alertCount; ++i) {
        Fields alertFields;
        const auto prefix = "alert." + std::to_string(i) + ".";
        for (const auto& [key, value] : fields) {
          if (key.rfind(prefix, 0) == 0) {
            alertFields[key.substr(prefix.size())] = value;
          }
        }
        OperatorAuthorityAlert alert;
        alert.type = fieldOr(alertFields, "type", "unknown");
        alert.reason = fieldOr(alertFields, "reason", "unknown");
        alert.leaseId = fieldOr(alertFields, "lease_id", "none");
        alert.revokedOperator = fieldOr(alertFields, "revoked_operator", "unknown");
        alert.revokerOperator = fieldOr(alertFields, "revoker_operator", "unknown");
        alert.droneId = fieldOr(alertFields, "drone", "unknown");
        alert.scope = fieldOr(alertFields, "scope", "unknown");
        alert.updatedMs = unsignedFieldOr(alertFields, "updated_ms", 0);
        loadedAlerts.push_back(std::move(alert));
        while (loadedAlerts.size() > 20) {
          loadedAlerts.erase(loadedAlerts.begin());
        }
      }
      {
        std::lock_guard<std::mutex> guard(m_operatorAuthorityAlertMutex);
        m_operatorAuthorityAlerts = std::move(loadedAlerts);
      }
      {
        std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
        m_issuedOperatorLeases = std::move(loaded);
        m_operatorRevocationRecords = std::move(loadedRevocations);
        persistIssuedOperatorLeasesLocked();
      }
      NDN_LOG_INFO("AUTHORITY_STATE_LOADED path=" << m_operatorAuthorityStateFile
                   << " active_lease_count=" << m_issuedOperatorLeases.size()
                   << " revocation_count=" << m_operatorRevocationRecords.size()
                   << " alert_count=" << operatorAuthorityAlertsSnapshot().size());
    }
    catch (const std::exception& e) {
      NDN_LOG_WARN("AUTHORITY_STATE_LOAD_FAILED path=" << m_operatorAuthorityStateFile
                   << " error=" << e.what());
    }
  }

  bool
  issueOperatorAuthorityLease(const OperatorAuthorityLeaseRequest& leaseRequest,
                              OperatorAuthorityLease& lease,
                              std::string& reason,
                              Fields& details)
  {
    const auto nowMs = nowMilliseconds();
    const auto ttlMs = leaseRequest.ttlMs == 0 ? uint64_t{60000} : leaseRequest.ttlMs;
    std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
    m_issuedOperatorLeases.erase(
      std::remove_if(m_issuedOperatorLeases.begin(), m_issuedOperatorLeases.end(),
                     [nowMs](const OperatorAuthorityLease& active) {
                       return !active.isFresh(nowMs);
                     }),
      m_issuedOperatorLeases.end());

    const bool requestedExclusive = isExclusiveAuthorityScope(leaseRequest.scope);
    if (leaseRequest.scope == "admin" && !operatorHasAdminAuthority(leaseRequest.operatorId)) {
      reason = "admin-unauthorized";
      details["admin_operator"] = leaseRequest.operatorId;
      details["active_lease_count"] = std::to_string(m_issuedOperatorLeases.size());
      persistIssuedOperatorLeasesLocked();
      return false;
    }
    if (requestedExclusive && leaseRequest.scope != "admin") {
      for (const auto& active : m_issuedOperatorLeases) {
        if (!isExclusiveAuthorityScope(active.scope) ||
            !leaseTargetsOverlap(active.droneId, leaseRequest.droneId) ||
            active.operatorId == leaseRequest.operatorId) {
          continue;
        }
        reason = "lease-conflict";
        details["conflicting_lease_id"] = active.leaseId;
        details["conflicting_operator"] = active.operatorId;
        details["conflicting_scope"] = active.scope;
        details["conflicting_drone"] = active.droneId;
        details["active_lease_count"] = std::to_string(m_issuedOperatorLeases.size());
        persistIssuedOperatorLeasesLocked();
        return false;
      }
    }

    size_t overridden = 0;
    std::vector<std::string> revokedLeaseIds;
    std::vector<std::string> revokedOperators;
    std::vector<OperatorAuthorityAlert> overrideAlerts;
    m_issuedOperatorLeases.erase(
      std::remove_if(m_issuedOperatorLeases.begin(), m_issuedOperatorLeases.end(),
                     [&](const OperatorAuthorityLease& active) {
                       if (!leaseTargetsOverlap(active.droneId, leaseRequest.droneId)) {
                         return false;
                       }
                       if (leaseRequest.scope == "admin" && isExclusiveAuthorityScope(active.scope)) {
                         ++overridden;
                         revokedLeaseIds.push_back(active.leaseId);
                         revokedOperators.push_back(active.operatorId);
                         m_operatorRevocationRecords[active.leaseId] = Fields{
                           {"type", "operator-authority-revocation-record"},
                           {"found", "true"},
                           {"reason", "ok"},
                           {"revoked_lease_id", active.leaseId},
                           {"revoked_operator", active.operatorId},
                           {"revoked_drone", active.droneId},
                           {"revoked_scope", active.scope},
                           {"revoked_ms", std::to_string(nowMs)},
                           {"revoker_operator", leaseRequest.operatorId},
                           {"revoker_request_id", leaseRequest.requestId},
                         };
                         overrideAlerts.push_back({
                           "admin-override",
                           "ok",
                           active.leaseId,
                           active.operatorId,
                           leaseRequest.operatorId,
                           active.droneId,
                           active.scope,
                           nowMs
                         });
                         return true;
                       }
                       return active.operatorId == leaseRequest.operatorId &&
                              active.scope == leaseRequest.scope;
                     }),
      m_issuedOperatorLeases.end());

    lease.leaseId = "issued-" + leaseRequest.requestId;
    lease.operatorId = leaseRequest.operatorId;
    lease.droneId = leaseRequest.droneId;
    lease.scope = leaseRequest.scope;
    lease.issuedMs = nowMs;
    lease.expiresMs = nowMs + ttlMs;
    m_issuedOperatorLeases.push_back(lease);

    reason = "ok";
    details["overridden_leases"] = std::to_string(overridden);
    details["revoked_lease_ids"] = joinTextList(revokedLeaseIds);
    details["revoked_operators"] = joinTextList(revokedOperators);
    details["active_lease_count"] = std::to_string(m_issuedOperatorLeases.size());
    for (auto& alert : overrideAlerts) {
      appendOperatorAuthorityAlert(std::move(alert));
    }
    persistIssuedOperatorLeasesLocked();
    return true;
  }

  void
  startOperatorAuthorityRefreshThread()
  {
    if (m_operatorAuthorityRefreshIntervalMs == 0 ||
        m_operatorAuthorityRefreshThread.joinable()) {
      NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER state="
                   << (m_operatorAuthorityRefreshIntervalMs == 0 ? "disabled" : "already-running")
                   << " interval_ms=" << m_operatorAuthorityRefreshIntervalMs);
      return;
    }
    m_operatorAuthorityRefreshThread = std::thread([this] {
      NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER state=started interval_ms="
                   << m_operatorAuthorityRefreshIntervalMs);
      const auto interval = std::chrono::milliseconds(m_operatorAuthorityRefreshIntervalMs);
      while (!m_done.load()) {
        const auto sleepUntil = std::chrono::steady_clock::now() + interval;
        while (!m_done.load() && std::chrono::steady_clock::now() < sleepUntil) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (m_done.load()) {
          break;
        }
        std::string reason;
        Fields fields;
        const bool revoked = refreshOperatorAuthorityLeaseFromIssuer(
          m_config.groundStationIdentity, std::chrono::seconds(5), reason, &fields);
        NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER_TICK revoked=" << (revoked ? "true" : "false")
                     << " reason=" << reason
                     << " lease_id=" << operatorAuthorityLease().leaseId
                     << " revoker_operator=" << fieldOr(fields, "revoker_operator", "none"));
      }
      NDN_LOG_INFO("AUTHORITY_REFRESH_TIMER state=stopped");
    });
  }

  bool
  validateOperatorLease(const std::string& droneId,
                        const std::string& commandName,
                        std::string& reason) const
  {
    std::lock_guard<std::mutex> guard(m_operatorLeaseMutex);
    return m_operatorLease.allowsCommand(droneId, commandName, nowMilliseconds(), reason);
  }

  bool
  validateMissionLeaseForDrones(const std::vector<std::string>& droneIds,
                                std::string& reason) const
  {
    if (droneIds.empty()) {
      reason = "no-candidate-drone";
      return false;
    }
    for (const auto& droneId : droneIds) {
      std::string itemReason;
      if (!validateOperatorLease(droneId, "mission_assign", itemReason)) {
        reason = droneId + ":" + itemReason;
        return false;
      }
    }
    reason = "ok";
    return true;
  }

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
    if (plan.completionObjective.empty()) {
      plan.completionObjective = "return-to-start";
    }
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
    if (droneId == activeVideoDroneId() && video.isStreaming()) {
      m_videoFramesPublished = video.framesPublished;
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
    auto runtimeCommand = RuntimeCommandSnapshot{
      command.command,
      commandLifecycle(command),
      command.detail,
      command.updatedMs,
      command.rttMs,
      command.timeoutMs
    };
    auto& commandHistory = m_commandHistoryByDrone[command.droneId];
    commandHistory.push_back(std::move(runtimeCommand));
    if (commandHistory.size() > 10) {
      commandHistory.erase(commandHistory.begin());
    }
    NDN_LOG_INFO("GS_COMMAND_STATE " << command.statusLine());
  }

  CommandLifecycle
  commandLifecycle(const FlightCommandState& command) const
  {
    if (command.isTimeout()) {
      return CommandLifecycle::Timeout;
    }
    if (command.accepted == "false") {
      return CommandLifecycle::Failed;
    }
    if (command.accepted == "true") {
      return command.ackResult == "success" ? CommandLifecycle::Success : CommandLifecycle::Running;
    }
    if (!command.ackResult.empty() && command.ackResult != "unknown") {
      return CommandLifecycle::AckWait;
    }
    return CommandLifecycle::Sending;
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
    installOperatorAuthorityLeaseService();
  }

  void
  installObjectDetectionService()
  {
    using ServiceInvocationMode = ndn_service_framework::ServiceProvider::ServiceInvocationMode;

    m_coreContainer.localRegistry().registerLocalService(
      m_config.serviceGsObjectDetection,
      [this](const ndn::Name&,
             const ndn::Name&,
             const ndn_service_framework::RequestMessage& request) {
        return runObjectDetectionLocal(request);
      });

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
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            m_config.serviceGsObjectDetection, request, response,
            m_config.groundStationIdentity);
          return response;
        }),
      ServiceInvocationMode::NormalOnly);
  }

  ndn_service_framework::ResponseMessage
  runObjectDetectionLocal(const ndn_service_framework::RequestMessage& request)
  {
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
    m_activeStreamId = makeVideoSessionId("live-start", droneId);
    postRequestForDrone(droneId, droneVideoControlService(m_config, droneId),
                encodeFields(makeVideoStartFields(
                  VIDEO_FPS, requestedBitrateKbps, m_videoFrameWidth,
                  m_videoFecParityShards)),
                [this, droneId, requestedBitrateKbps](const std::string& payload) {
                  const auto fields = decodeFields(payload);
                  const auto prefix = fieldOr(fields, "stream_prefix", "");
                  const auto seqText = fieldOr(fields, "next_seq", "0");
                  uint64_t streamSessionEpoch = 0;
                  try {
                    streamSessionEpoch = std::stoull(fieldOr(fields, "stream_session_epoch", "0"));
                  }
                  catch (const std::exception&) {
                    streamSessionEpoch = 0;
                  }
                  if (prefix.empty()) {
                    publishStatus("Video control response missing stream prefix");
                    return;
                  }

                  updateVideoState(droneId, fields);
                  m_videoBitrateKbps = requestedBitrateKbps;
                  m_streamPrefix = ndn::Name(prefix);
                  m_activeStreamId = fieldOr(fields, "stream_id", "");
                  if (m_activeStreamId.empty()) {
                    m_activeStreamId = "live|" + droneId + "|" +
                      std::to_string(nowMilliseconds()) + "|" +
                      std::to_string(++m_videoSessionCounter);
                  }
                  if (streamSessionEpoch > 0) {
                    allocateStreamSessionEpoch(m_activeStreamId, streamSessionEpoch);
                  }
                  else {
                    allocateStreamSessionEpoch(m_activeStreamId);
                  }
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
                  m_videoFramesPublished = 0;
                  m_lastVideoAdaptiveLogMs = 0;
                  resetVideoAdaptiveState();
                  m_highestReceivedVideoPacketSeq = UINT64_MAX;
                  m_nextChunkSeqToDecode = 0;
                  m_fecFrames.clear();
                  resetVideoPacketTracking();
                  {
                    std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
                    m_chunkQueue.clear();
                    m_decoderReorderBuffer.reset();
                    m_decoderPendingChunkCount = 0;
                    m_decoderOutBuffer.clear();
                  }
                  stopDecoder();
                  startDecoder();
                  publishVideoAdaptiveState("configured", true);
                  if (m_videoBitrateChangePending.exchange(false)) {
                    const auto acceptedBitrateKbps = m_videoAcceptedBitrateKbps.load();
                    const auto expectedBitrateKbps = m_videoBitrateChangeToKbps.load();
                    NDN_LOG_INFO("GS_VIDEO_BITRATE_CHANGE_COMPLETE drone=" << droneId
                                 << " from_kbps=" << m_videoBitrateChangeFromKbps.load()
                                 << " requested_kbps=" << requestedBitrateKbps
                                 << " expected_kbps=" << expectedBitrateKbps
                                 << " accepted_kbps=" << acceptedBitrateKbps
                                 << " matched="
                                 << (acceptedBitrateKbps == expectedBitrateKbps ? "true" : "false")
                                 << " stream_prefix=" << prefix);
                  }
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
                    m_videoBitrateChangePending = false;
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
    m_videoBitrateChangeFromKbps = previousBitrateKbps;
    m_videoBitrateChangeToKbps = requestedBitrateKbps;
    m_videoBitrateChangePending = true;
    m_streaming = false;
    m_activeStreamId = makeVideoSessionId("live-restart", droneId);
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
        m_videoBitrateChangePending = false;
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
                  if (activeVideoDroneId().empty()) {
                    m_activeStreamId = makeVideoSessionId("live-stop-ack", droneId);
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
                                "retry after a short delay.");
                  m_videoStopSuppressUntilMs = nowMilliseconds() + VIDEO_STOP_TIMEOUT_RETRY_GUARD_MS;
                  if (onStopTimeout) {
                    onStopTimeout();
                  }
                  m_activeStreamId = makeVideoSessionId("live-stop-timeout", droneId);
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
  requestRepoCatalogForDrone(const std::string& droneId,
                             std::function<void(std::optional<UavDataProductCatalogState>)> onDone = {})
  {
    auto completion = std::make_shared<
      std::function<void(std::optional<UavDataProductCatalogState>)>>(std::move(onDone));
    postRequestForDrone(
      droneId,
      droneCameraRepoCatalogService(m_config, droneId),
      encodeFields({{"type", "uav-data-product-catalog-request"}}),
      [this, droneId, completion](const std::string& payload) mutable {
        const auto fields = decodeFields(payload);
        auto catalog = UavDataProductCatalogState::fromFields(fields);
        {
          std::lock_guard<std::mutex> guard(m_catalogMutex);
          m_catalogByDrone[droneId] = catalog;
        }
        publishStatus(catalog.statusLine());
        publishStatus("Repo catalog drone=" + droneId +
                      " products=" + std::to_string(catalog.totalProducts()) +
                      " repo_objects=" + std::to_string(catalog.repoObjects) +
                      " source=" + catalog.sourceRepo +
                      " latest=" + catalog.latestProductType + ":" +
                      catalog.latestObjectPrefix);
        if (*completion) {
          (*completion)(catalog);
        }
      },
      {},
      [completion]() mutable {
        if (*completion) {
          (*completion)(std::nullopt);
        }
      });
  }

  std::optional<UavDataProductCatalogState>
  requestRepoCatalogForDroneSync(const std::string& droneId, std::chrono::milliseconds timeout)
  {
    struct CatalogWaitState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::optional<UavDataProductCatalogState> catalog;
      bool done = false;
    };
    auto state = std::make_shared<CatalogWaitState>();
    requestRepoCatalogForDrone(
      droneId,
      [state](std::optional<UavDataProductCatalogState> catalog) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->catalog = std::move(catalog);
        state->done = true;
        state->cv.notify_all();
      });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&] { return state->done; });
    return state->catalog;
  }

  void
  requestVehicleParametersForDrone(const std::string& droneId,
                                   std::function<void(std::optional<VehicleParameterSnapshot>)> onDone = {})
  {
    auto completion = std::make_shared<
      std::function<void(std::optional<VehicleParameterSnapshot>)>>(std::move(onDone));
    postRequestForDrone(
      droneId,
      droneMavlinkParametersService(m_config, droneId),
      encodeFields({{"type", "vehicle-parameter-snapshot-request"}}),
      [this, droneId, completion](const std::string& payload) mutable {
        const auto fields = decodeFields(payload);
        auto snapshot = VehicleParameterSnapshot::fromFields(fields);
        if (snapshot.droneId == "unknown") {
          snapshot.droneId = droneId;
        }
        {
          std::lock_guard<std::mutex> guard(m_parameterMutex);
          m_parameterSnapshots[droneId] = snapshot;
        }
        publishStatus(snapshot.statusLine());
        publishStatus("Vehicle parameters drone=" + droneId +
                      " source=" + snapshot.source +
                      " firmware=" + snapshot.firmware +
                      " vehicle=" + snapshot.vehicleType +
                      " parameters=" + std::to_string(snapshot.parameterCount) +
                      " complete=" + std::to_string(snapshot.completePercent));
        if (*completion) {
          (*completion)(snapshot);
        }
      },
      {},
      [completion]() mutable {
        if (*completion) {
          (*completion)(std::nullopt);
        }
      });
  }

  std::optional<VehicleParameterSnapshot>
  requestVehicleParametersForDroneSync(const std::string& droneId, std::chrono::milliseconds timeout)
  {
    struct ParameterWaitState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::optional<VehicleParameterSnapshot> snapshot;
      bool done = false;
    };
    auto state = std::make_shared<ParameterWaitState>();
    requestVehicleParametersForDrone(
      droneId,
      [state](std::optional<VehicleParameterSnapshot> snapshot) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->snapshot = std::move(snapshot);
        state->done = true;
        state->cv.notify_all();
      });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&] { return state->done; });
    return state->snapshot;
  }

  void
  requestVehicleParameterEditForDrone(const std::string& droneId,
                                      VehicleParameterEditRequest request,
                                      std::function<void(std::optional<VehicleParameterEditResult>)> onDone = {})
  {
    auto completion = std::make_shared<
      std::function<void(std::optional<VehicleParameterEditResult>)>>(std::move(onDone));
    if (request.droneId == "unknown") {
      request.droneId = droneId;
    }
    if (request.operatorId == "unknown") {
      request.operatorId = m_config.groundStationIdentity.toUri();
    }
    postRequestForDrone(
      droneId,
      droneMavlinkParameterEditService(m_config, droneId),
      encodeFields(request.toFields()),
      [this, droneId, completion](const std::string& payload) mutable {
        const auto fields = decodeFields(payload);
        auto result = VehicleParameterEditResult::fromFields(fields);
        if (result.droneId == "unknown") {
          result.droneId = droneId;
        }
        publishStatus(result.statusLine());
        publishStatus("Vehicle parameter edit drone=" + droneId +
                      " param=" + result.parameterName +
                      " accepted=" + std::string(result.accepted ? "true" : "false") +
                      " applied=" + std::string(result.applied ? "true" : "false") +
                      " verified=" + std::string(result.verified ? "true" : "false") +
                      " reason=" + result.reason +
                      " value=" + result.verifiedValue);
        if (*completion) {
          (*completion)(result);
        }
      },
      {},
      [completion]() mutable {
        if (*completion) {
          (*completion)(std::nullopt);
        }
      });
  }

  std::optional<VehicleParameterEditResult>
  requestVehicleParameterEditForDroneSync(const std::string& droneId,
                                          VehicleParameterEditRequest request,
                                          std::chrono::milliseconds timeout)
  {
    struct ParameterEditWaitState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::optional<VehicleParameterEditResult> result;
      bool done = false;
    };
    auto state = std::make_shared<ParameterEditWaitState>();
    requestVehicleParameterEditForDrone(
      droneId,
      std::move(request),
      [state](std::optional<VehicleParameterEditResult> result) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->result = std::move(result);
        state->done = true;
        state->cv.notify_all();
      });

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&] { return state->done; });
    return state->result;
  }

  void
  requestPreflightChecklistForDrone(const std::string& droneId,
                                    std::function<void(std::vector<PreflightCheckItem>)> onDone = {})
  {
    auto completion = std::make_shared<
      std::function<void(std::vector<PreflightCheckItem>)>>(std::move(onDone));
    postRequestForDrone(
      droneId,
      dronePreflightChecklistService(m_config, droneId),
      encodeFields({{"type", "preflight-checklist-request"}}),
      [this, droneId, completion](const std::string& payload) mutable {
        const auto fields = decodeFields(payload);
        std::vector<PreflightCheckItem> items;
        const auto count = static_cast<size_t>(std::stoull(fieldOr(fields, "preflight_count", "0")));
        items.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          Fields itemFields;
          const auto prefix = "check." + std::to_string(i) + ".";
          for (const auto& [key, value] : fields) {
            if (key.rfind(prefix, 0) == 0) {
              itemFields[key.substr(prefix.size())] = value;
            }
          }
          auto item = PreflightCheckItem::fromFields(itemFields);
          if (item.droneId == "unknown") {
            item.droneId = droneId;
          }
          items.push_back(std::move(item));
        }
        {
          std::lock_guard<std::mutex> guard(m_preflightMutex);
          m_preflightByDrone[droneId] = items;
        }
        const auto blockingFailures = static_cast<size_t>(std::count_if(
          items.begin(), items.end(), [] (const PreflightCheckItem& item) {
            return item.isBlockingFailure();
          }));
        publishStatus("Preflight checklist drone=" + droneId +
                      " items=" + std::to_string(items.size()) +
                      " blocking_failures=" + std::to_string(blockingFailures));
        if (*completion) {
          (*completion)(items);
        }
      },
      {},
      [completion]() mutable {
        if (*completion) {
          (*completion)({});
        }
      });
  }

  std::vector<PreflightCheckItem>
  requestPreflightChecklistForDroneSync(const std::string& droneId, std::chrono::milliseconds timeout)
  {
    struct PreflightWaitState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::vector<PreflightCheckItem> items;
      bool done = false;
    };
    auto state = std::make_shared<PreflightWaitState>();
    requestPreflightChecklistForDrone(
      droneId,
      [state](std::vector<PreflightCheckItem> items) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->items = std::move(items);
        state->done = true;
        state->cv.notify_all();
      });
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&] { return state->done; });
    return state->items;
  }

  void
  requestAnalyzeSnapshotForDrone(const std::string& droneId,
                                 std::function<void(std::optional<UavAnalyzeSnapshot>)> onDone = {})
  {
    auto completion = std::make_shared<
      std::function<void(std::optional<UavAnalyzeSnapshot>)>>(std::move(onDone));
    postRequestForDrone(
      droneId,
      droneMavlinkAnalyzeSnapshotService(m_config, droneId),
      encodeFields({{"type", "uav-analyze-snapshot-request"}}),
      [this, droneId, completion](const std::string& payload) mutable {
        const auto fields = decodeFields(payload);
        auto snapshot = UavAnalyzeSnapshot::fromFields(fields);
        if (snapshot.droneId == "unknown") {
          snapshot.droneId = droneId;
        }
        {
          std::lock_guard<std::mutex> guard(m_analyzeMutex);
          m_analyzeSnapshots[droneId] = snapshot;
        }
        publishStatus(snapshot.statusLine());
        publishStatus("Analyze snapshot drone=" + droneId +
                      " messages=" + std::to_string(snapshot.messages.size()) +
                      " active_messages=" +
                      std::to_string(snapshot.activeMessageCount(nowMilliseconds(), 3000)) +
                      " link=" + snapshot.linkState +
                      " mode=" + snapshot.flightMode);
        if (*completion) {
          (*completion)(snapshot);
        }
      },
      {},
      [completion]() mutable {
        if (*completion) {
          (*completion)(std::nullopt);
        }
      });
  }

  std::optional<UavAnalyzeSnapshot>
  requestAnalyzeSnapshotForDroneSync(const std::string& droneId, std::chrono::milliseconds timeout)
  {
    struct AnalyzeWaitState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::optional<UavAnalyzeSnapshot> snapshot;
      bool done = false;
    };
    auto state = std::make_shared<AnalyzeWaitState>();
    requestAnalyzeSnapshotForDrone(
      droneId,
      [state](std::optional<UavAnalyzeSnapshot> snapshot) {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->snapshot = std::move(snapshot);
        state->done = true;
        state->cv.notify_all();
      });
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, timeout, [&] { return state->done; });
    return state->snapshot;
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
    const auto recordingStreamId = "recording|" + manifest.droneId + "|" +
      manifest.sessionId + "|" + std::to_string(nowMilliseconds());
    allocateStreamSessionEpoch(recordingStreamId);
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
      m_decoderReorderBuffer.reset();
      m_recordingPlaybackChunks.clear();
      m_decoderOutBuffer.clear();
    }
    {
      std::lock_guard<std::mutex> guard(m_videoStateMutex);
      m_activeVideoDroneId.clear();
      m_recordingPlaybackDroneId = manifest.droneId;
      m_recordingPlaybackStreamId = recordingStreamId;
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
          const auto sessionId = activeRecordingPlaybackStreamId();
          insertChunkForDecode(index,
                               payload,
                               sessionId,
                               streamSessionEpochForStreamId(sessionId),
                               nowMilliseconds() - m_firstFrameMs.load());
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

  void
  installOperatorAuthorityLeaseService()
  {
    using ServiceInvocationMode = ndn_service_framework::ServiceProvider::ServiceInvocationMode;

    m_coreContainer.localRegistry().registerLocalService(
      m_config.serviceGsOperatorAuthorityLease,
      [this](const ndn::Name&,
             const ndn::Name&,
             const ndn_service_framework::RequestMessage& request) {
        return runOperatorAuthorityLeaseLocal(request);
      });

    m_objectDetectionProvider->addService(
      m_config.serviceGsOperatorAuthorityLease,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "operator authority lease issuer ready";
          decision.payload = bufferFromString(encodeFields(Fields{
            {"gs", m_config.groundStationIdentity.toUri()},
            {"ready", "true"},
            {"service", m_config.serviceGsOperatorAuthorityLease.toUri()},
          }));
          return decision;
        }),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            m_config.serviceGsOperatorAuthorityLease, request, response,
            m_config.groundStationIdentity);
          return response;
        }),
      ServiceInvocationMode::NormalOnly);

    m_coreContainer.localRegistry().registerLocalService(
      m_config.serviceGsOperatorAuthorityRevocation,
      [this](const ndn::Name&,
             const ndn::Name&,
             const ndn_service_framework::RequestMessage& request) {
        return runOperatorAuthorityRevocationLocal(request);
      });

    m_objectDetectionProvider->addService(
      m_config.serviceGsOperatorAuthorityRevocation,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "operator authority revocation lookup ready";
          decision.payload = bufferFromString(encodeFields(Fields{
            {"gs", m_config.groundStationIdentity.toUri()},
            {"ready", "true"},
            {"service", m_config.serviceGsOperatorAuthorityRevocation.toUri()},
          }));
          return decision;
        }),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            m_config.serviceGsOperatorAuthorityRevocation, request, response,
            m_config.groundStationIdentity);
          return response;
        }),
      ServiceInvocationMode::NormalOnly);

    m_coreContainer.localRegistry().registerLocalService(
      m_config.serviceGsOperatorAuthorityAudit,
      [this](const ndn::Name& requesterIdentity,
             const ndn::Name&,
             const ndn_service_framework::RequestMessage& request) {
        return runOperatorAuthorityAuditLocal(request, requesterIdentity);
      });

    m_objectDetectionProvider->addService(
      m_config.serviceGsOperatorAuthorityAudit,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "operator authority audit lookup ready";
          decision.payload = bufferFromString(encodeFields(Fields{
            {"gs", m_config.groundStationIdentity.toUri()},
            {"ready", "true"},
            {"service", m_config.serviceGsOperatorAuthorityAudit.toUri()},
          }));
          return decision;
        }),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [this](const ndn::Name& requesterIdentity,
               const ndn::Name&,
               const ndn::Name&,
               const ndn::Name&,
               const ndn_service_framework::RequestMessage& request) {
          ndn_service_framework::ResponseMessage response;
          m_coreContainer.localRegistry().localInvokeRawInto(
            m_config.serviceGsOperatorAuthorityAudit, request, response,
            requesterIdentity);
          return response;
        }),
      ServiceInvocationMode::NormalOnly);
  }

  ndn_service_framework::ResponseMessage
  runOperatorAuthorityLeaseLocal(const ndn_service_framework::RequestMessage& request)
  {
    const auto payload = request.getPayload();
    const auto fields = decodeFields(std::string(
      reinterpret_cast<const char*>(payload.data()), payload.size()));
    const auto leaseRequest = OperatorAuthorityLeaseRequest::fromFields(fields);
    std::string reason;
    if (!leaseRequest.isValid(reason)) {
      return makeResponse(false, encodeFields({
        {"type", "operator-authority-lease-response"},
        {"accepted", "false"},
        {"reason", reason},
      }), reason);
    }
    if (leaseRequest.droneId != "all" &&
        std::find(m_patrolDroneIds.begin(), m_patrolDroneIds.end(), leaseRequest.droneId) ==
          m_patrolDroneIds.end()) {
      return makeResponse(false, encodeFields({
        {"type", "operator-authority-lease-response"},
        {"accepted", "false"},
        {"reason", "unknown-drone"},
        {"lease_drone", leaseRequest.droneId},
      }), "unknown-drone");
    }

    OperatorAuthorityLease lease;
    Fields arbitrationDetails;
    const bool accepted = issueOperatorAuthorityLease(leaseRequest, lease, reason,
                                                      arbitrationDetails);
    if (!accepted) {
      Fields response{
        {"type", "operator-authority-lease-response"},
        {"accepted", "false"},
        {"reason", reason},
        {"lease_request_id", leaseRequest.requestId},
      };
      response.insert(arbitrationDetails.begin(), arbitrationDetails.end());
      NDN_LOG_INFO("AUTHORITY_LEASE_REJECTED request_id=" << leaseRequest.requestId
                   << " operator=" << leaseRequest.operatorId
                   << " drone=" << leaseRequest.droneId
                   << " scope=" << leaseRequest.scope
                   << " reason=" << reason
                   << " conflicting_operator=" << fieldOr(response, "conflicting_operator", "none"));
      return makeResponse(true, encodeFields(response), reason);
    }
    auto response = lease.toFields();
    response["type"] = "operator-authority-lease-response";
    response["accepted"] = "true";
    response["reason"] = "ok";
    response["lease_request_id"] = leaseRequest.requestId;
    response["lease_ttl_ms"] = std::to_string(leaseRequest.ttlMs == 0 ? uint64_t{60000} :
                                              leaseRequest.ttlMs);
    response.insert(arbitrationDetails.begin(), arbitrationDetails.end());
    NDN_LOG_INFO("AUTHORITY_LEASE_ISSUED request_id=" << leaseRequest.requestId
                 << " operator=" << lease.operatorId
                 << " drone=" << lease.droneId
                 << " scope=" << lease.scope
                 << " expires_ms=" << lease.expiresMs
                 << " active_lease_count=" << fieldOr(response, "active_lease_count", "0")
                 << " overridden_leases=" << fieldOr(response, "overridden_leases", "0"));
    return makeResponse(true, encodeFields(response));
  }

  ndn_service_framework::ResponseMessage
  runOperatorAuthorityRevocationLocal(const ndn_service_framework::RequestMessage& request)
  {
    const auto payload = request.getPayload();
    const auto fields = decodeFields(std::string(
      reinterpret_cast<const char*>(payload.data()), payload.size()));
    const auto leaseId = fieldOr(fields, "revoked_lease_id", fieldOr(fields, "lease_id", ""));
    if (leaseId.empty()) {
      return makeResponse(true, encodeFields(Fields{
        {"type", "operator-authority-revocation-response"},
        {"found", "false"},
        {"reason", "missing-lease-id"},
      }), "missing-lease-id");
    }

    Fields response;
    {
      std::lock_guard<std::mutex> guard(m_issuedOperatorLeaseMutex);
      const auto it = m_operatorRevocationRecords.find(leaseId);
      if (it == m_operatorRevocationRecords.end()) {
        response = Fields{
          {"type", "operator-authority-revocation-response"},
          {"found", "false"},
          {"reason", "not-found"},
          {"revoked_lease_id", leaseId},
        };
      }
      else {
        response = it->second;
        response["type"] = "operator-authority-revocation-response";
        response["found"] = "true";
        response["reason"] = "ok";
      }
    }

    NDN_LOG_INFO("AUTHORITY_REVOCATION_LOOKUP lease_id=" << leaseId
                 << " found=" << fieldOr(response, "found", "false")
                 << " reason=" << fieldOr(response, "reason", "unknown")
                 << " revoked_operator=" << fieldOr(response, "revoked_operator", "none")
                 << " revoker_operator=" << fieldOr(response, "revoker_operator", "none"));
    return makeResponse(true, encodeFields(response), fieldOr(response, "reason", "ok"));
  }

  ndn_service_framework::ResponseMessage
  runOperatorAuthorityAuditLocal(const ndn_service_framework::RequestMessage& request,
                                 const ndn::Name& requesterIdentity = ndn::Name("/local"))
  {
    const auto payload = request.getPayload();
    const auto fields = decodeFields(std::string(
      reinterpret_cast<const char*>(payload.data()), payload.size()));
    const auto limit = std::min<uint64_t>(
      20, std::max<uint64_t>(1, unsignedFieldOr(fields, "limit",
                                                unsignedFieldOr(fields, "max_records", 20))));
    const bool explicitWindow = fields.find("offset") != fields.end() ||
                                fields.find("limit") != fields.end() ||
                                fields.find("from_ms") != fields.end() ||
                                fields.find("to_ms") != fields.end();
    const auto requestedOffset = unsignedFieldOr(fields, "offset", 0);
    const auto fromMs = unsignedFieldOr(fields, "from_ms", 0);
    const auto toMs = unsignedFieldOr(fields, "to_ms", 0);
    const auto redaction = fieldOr(fields, "redaction", "full");
    const auto payloadRequesterOperator = fieldOr(fields, "requester_operator", "");
    std::string requesterOperatorSource = "payload";
    auto requesterOperator = payloadRequesterOperator;
    const auto authenticatedRequesterOperator = operatorIdForRequesterIdentity(requesterIdentity);
    if (!authenticatedRequesterOperator.empty()) {
      requesterOperator = authenticatedRequesterOperator;
      requesterOperatorSource = "requester-identity";
    }
    else if (requesterOperator.empty()) {
      requesterOperatorSource = "none";
    }
    const bool fullRedactionAllowed =
      redaction != "full" ||
      requesterOperator.empty() ||
      operatorHasAdminAuthority(requesterOperator);
    if (!fullRedactionAllowed) {
      Fields response{
        {"type", "operator-authority-audit-response"},
        {"ok", "false"},
        {"reason", "full-redaction-requires-admin"},
        {"redaction", redaction},
        {"requester_identity", requesterIdentity.toUri()},
        {"effective_requester_operator", requesterOperator},
        {"requester_operator_source", requesterOperatorSource},
      };
      NDN_LOG_INFO("AUTHORITY_AUDIT_LOOKUP alert_count=0 matched_count=0 returned_count=0"
                   << " offset=0 limit=0 from_ms=" << fromMs
                   << " to_ms=" << toMs
                   << " redaction=" << redaction
                   << " requester_identity=" << requesterIdentity.toUri()
                   << " effective_requester_operator=" << requesterOperator
                   << " requester_operator_source=" << requesterOperatorSource
                   << " reason=full-redaction-requires-admin");
      return makeResponse(false, encodeFields(response), "full-redaction-requires-admin");
    }
    const auto alerts = operatorAuthorityAlertsSnapshot();
    std::vector<OperatorAuthorityAlert> matched;
    for (const auto& alert : alerts) {
      if (fromMs != 0 && alert.updatedMs < fromMs) {
        continue;
      }
      if (toMs != 0 && alert.updatedMs > toMs) {
        continue;
      }
      matched.push_back(alert);
    }
    const auto totalMatched = static_cast<uint64_t>(matched.size());
    const auto start = explicitWindow ?
                       std::min<uint64_t>(requestedOffset, totalMatched) :
                       totalMatched - std::min<uint64_t>(limit, totalMatched);
    const auto returnedCount = std::min<uint64_t>(limit, totalMatched - start);

    Fields response{
      {"type", "operator-authority-audit-response"},
      {"ok", "true"},
      {"reason", "ok"},
      {"alert_count", std::to_string(alerts.size())},
      {"matched_count", std::to_string(totalMatched)},
      {"returned_count", std::to_string(returnedCount)},
      {"offset", std::to_string(start)},
      {"limit", std::to_string(limit)},
      {"from_ms", std::to_string(fromMs)},
      {"to_ms", std::to_string(toMs)},
      {"redaction", redaction},
      {"requester_identity", requesterIdentity.toUri()},
      {"effective_requester_operator", requesterOperator},
      {"requester_operator_source", requesterOperatorSource},
    };
    for (uint64_t i = 0; i < returnedCount; ++i) {
      const auto& alert = matched[static_cast<size_t>(start + i)];
      const auto prefix = "alert." + std::to_string(i) + ".";
      const bool revealOperators =
        redaction == "full" ||
        (redaction == "self" &&
         !requesterOperator.empty() &&
         (requesterOperator == alert.revokedOperator ||
          requesterOperator == alert.revokerOperator));
      response[prefix + "type"] = alert.type;
      response[prefix + "reason"] = alert.reason;
      response[prefix + "lease_id"] = alert.leaseId;
      response[prefix + "revoked_operator"] = revealOperators ? alert.revokedOperator : "redacted";
      response[prefix + "revoker_operator"] = revealOperators ? alert.revokerOperator : "redacted";
      response[prefix + "drone"] = alert.droneId;
      response[prefix + "scope"] = alert.scope;
      response[prefix + "updated_ms"] = std::to_string(alert.updatedMs);
      response[prefix + "redacted"] = revealOperators ? "false" : "true";
    }

    NDN_LOG_INFO("AUTHORITY_AUDIT_LOOKUP alert_count=" << alerts.size()
                 << " matched_count=" << totalMatched
                 << " returned_count=" << returnedCount
                 << " offset=" << start
                 << " limit=" << limit
                 << " from_ms=" << fromMs
                 << " to_ms=" << toMs
                 << " redaction=" << redaction
                 << " requester_identity=" << requesterIdentity.toUri()
                 << " effective_requester_operator=" << requesterOperator
                 << " requester_operator_source=" << requesterOperatorSource
                 << " reason=ok");
    return makeResponse(true, encodeFields(response));
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
                      std::function<void()> onTimeout = {},
                      int timeoutOverrideMs = -1)
  {
    const int requestTimeoutMs = timeoutOverrideMs > 0 ? timeoutOverrideMs : m_timeoutMs;
    const auto queuedMs = nowMilliseconds();
    NDN_LOG_INFO("GS_TARGETED_PHASE phase=queued provider=" << provider.toUri()
                 << " service=" << service.toUri()
                 << " request_id=none timestamp_ms=" << queuedMs
                 << " elapsed_ms=0 status=queued");
    boost::asio::post(m_face.getIoContext(), [this, provider, service, payload,
                                onSuccess = std::move(onSuccess),
                                onTimeout = std::move(onTimeout), queuedMs,
                                requestTimeoutMs] {
      if (!m_containerReady.load() || !m_user) {
        NDN_LOG_INFO("GS_TARGETED_PHASE phase=dispatch-rejected provider="
                     << provider.toUri() << " service=" << service.toUri()
                     << " request_id=none timestamp_ms=" << nowMilliseconds()
                     << " elapsed_ms=" << (nowMilliseconds() - queuedMs)
                     << " status=runtime-not-ready");
        publishStatus("NDNSF runtime not ready for targeted " + service.toUri());
        if (onTimeout) {
          onTimeout();
        }
        return;
      }
      auto requestMessage = makeRequest(payload);
      const auto requestStartMs = nowMilliseconds();
      auto requestId = std::make_shared<ndn::Name>();
      *requestId = m_user->RequestServiceTargeted(
        provider,
        service,
        std::move(requestMessage),
        requestTimeoutMs,
        [this, service, provider, requestId, requestStartMs,
         onTimeout = std::move(onTimeout)](const ndn::Name&) {
          const auto timeoutMs = nowMilliseconds();
          NDN_LOG_INFO("GS_TARGETED_PHASE phase=timeout provider=" << provider.toUri()
                       << " service=" << service.toUri()
                       << " request_id=" << (requestId->empty() ? "none" : requestId->toUri())
                       << " timestamp_ms=" << timeoutMs
                       << " elapsed_ms=" << (timeoutMs - requestStartMs)
                       << " status=timeout");
          if (onTimeout) {
            onTimeout();
          }
          else {
            publishStatus("Timeout waiting for targeted " + service.toUri());
          }
        },
        [this, onSuccess, service, provider, requestStartMs, requestId](
          const ndn_service_framework::ResponseMessage& response) {
          const auto responseMs = nowMilliseconds();
          NDN_LOG_INFO("GS_TARGETED_PHASE phase=response provider=" << provider.toUri()
                       << " service=" << service.toUri()
                       << " request_id=" << (requestId->empty() ? "none" : requestId->toUri())
                       << " timestamp_ms=" << responseMs
                       << " elapsed_ms=" << (responseMs - requestStartMs)
                       << " status=" << (response.getStatus() ? "success" : "failure"));
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_TARGETED_RESPONSE service=" << service
                       << " payload=" << payloadText);
          publishUiOnlyStatus("Link provider=" + provider.toUri() +
                              " service=" + service.toUri() +
                              " rtt_ms=" +
                              std::to_string(nowMilliseconds() - requestStartMs));
          onSuccess(payloadText);
        });
      NDN_LOG_INFO("GS_TARGETED_PHASE phase="
                   << (requestId->empty() ? "dispatch-rejected" : "dispatched")
                   << " provider=" << provider.toUri()
                   << " service=" << service.toUri()
                   << " request_id=" << (requestId->empty() ? "none" : requestId->toUri())
                   << " timestamp_ms=" << nowMilliseconds()
                   << " elapsed_ms=" << (nowMilliseconds() - queuedMs)
                   << " status=" << (requestId->empty() ? "empty-request-id" : "pending"));
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

  struct DecoderStreamChunk
  {
    uint64_t packetSeq = 0;
    uint64_t arrivalMs = 0;
    uint64_t elapsedMs = 0;
    uint64_t streamSessionEpoch = 0;
    std::string streamId;
    std::vector<uint8_t> payload;
  };

  struct FecFrameState
  {
    uint64_t streamSessionEpoch = 0;
    std::string streamId;
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
    input.publishedFrames = m_videoFramesPublished.load();
    input.decodedFrames = m_decodedVideoFrames.load();
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
    state.pendingBytes = m_decoderPendingBytes.load();
    state.receivedChunks = m_receivedChunks.load();
    state.fecRecoveredChunks = m_fecRecoveredChunks.load();
    state.timeouts = m_frameTimeouts.load();
    state.nacks = m_frameNacks.load();
    state.duplicates = m_duplicateVideoPackets.load();
    state.publishedFrames = m_videoFramesPublished.load();
    state.decodedFrames = m_decodedVideoFrames.load();
    state.decodedFrameGap = state.publishedFrames > state.decodedFrames ?
      state.publishedFrames - state.decodedFrames : 0;
    state.frameGapPressure = decision.frameGapPressure;
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
            if (!isCurrentVideoSessionPacket(packet.streamId, packet.streamSessionEpoch)) {
              NDN_LOG_WARN("GS_VIDEO_STALE_SESSION_PACKET expected="
                           << activeVideoStreamId() << "@" << m_videoStreamSessionEpoch.load()
                           << " got=" << packet.streamId << "@" << packet.streamSessionEpoch
                           << " packetSeq=" << packet.packetSeq
                           << " requestedSeq=" << packetSeq);
              requestVideoPackets();
              return;
            }
            const auto activeStreamId = activeVideoStreamId();
            if (!activeStreamId.empty() && packet.streamId != activeStreamId) {
              NDN_LOG_WARN("GS_VIDEO_STALE_STREAM_PACKET expected=" << activeStreamId
                           << " got=" << packet.streamId
                           << " packetSeq=" << packet.packetSeq
                           << " requestedSeq=" << packetSeq);
              requestVideoPackets();
              return;
            }
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

    if (!isCurrentVideoSessionPacket(packet.streamId, packet.streamSessionEpoch)) {
      NDN_LOG_WARN("GS_VIDEO_STALE_SESSION_QUEUE dropped stream="
                   << packet.streamId << " session=" << packet.streamSessionEpoch
                   << " active_session=" << m_videoStreamSessionEpoch.load()
                   << " seq=" << packet.packetSeq);
      return;
    }

    if (!isCurrentLiveVideoStream(packet.streamId)) {
      NDN_LOG_WARN("GS_VIDEO_STALE_STREAM_QUEUE dropped=" << packet.streamId
                   << " active=" << activeVideoStreamId()
                   << " seq=" << packet.packetSeq);
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
    auto streamChunk = videoPacketToStreamChunk(packet);
    streamChunk.arrivalMs = receivedMs;
    insertStreamChunkForDecode(streamChunk, elapsedMs);
  }

  void
  processFecChunk(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (!m_decoderRunning.load() || packet.payload.empty() ||
        packet.fecSymbolCount == 0 || packet.fecDataShards == 0) {
      return;
    }

    if (!isCurrentVideoSessionPacket(packet.streamId, packet.streamSessionEpoch)) {
      NDN_LOG_WARN("GS_VIDEO_STALE_SESSION_FEC dropped stream="
                   << packet.streamId << " session=" << packet.streamSessionEpoch
                   << " active_session=" << m_videoStreamSessionEpoch.load()
                   << " frameSeq=" << packet.frameSeq);
      return;
    }

    if (!isCurrentLiveVideoStream(packet.streamId)) {
      NDN_LOG_WARN("GS_VIDEO_STALE_STREAM_FEC dropped=" << packet.streamId
                   << " active=" << activeVideoStreamId()
                   << " seq=" << packet.packetSeq
                   << " frameSeq=" << packet.frameSeq);
      return;
    }

    const auto frameSeq = packet.frameSeq;
    const auto frameKey = std::make_pair(packet.streamSessionEpoch, frameSeq);
    auto it = m_fecFrames.find(frameKey);
    if (it == m_fecFrames.end()) {
      it = m_fecFrames.emplace(frameKey, FecFrameState{}).first;
      it->second.streamSessionEpoch = packet.streamSessionEpoch;
      it->second.frameSeq = frameSeq;
    }

    if (it->second.initialized && !it->second.streamId.empty() && it->second.streamId != packet.streamId) {
      NDN_LOG_INFO("GS_VIDEO_STALE_STREAM_FEC_STATE reset stream=" << it->second.streamId
                   << " frameSeq=" << frameSeq
                   << " incoming=" << packet.streamId
                   << " seq=" << packet.packetSeq);
      m_fecFrames.erase(it);
      it = m_fecFrames.emplace(frameKey, FecFrameState{}).first;
      it->second.streamSessionEpoch = packet.streamSessionEpoch;
      it->second.frameSeq = frameSeq;
      it->second.streamId = packet.streamId;
    }

    auto& state = it->second;

    if (!state.initialized) {
      state.streamId = packet.streamId;
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
      auto streamChunk = videoPacketToStreamChunk(packet);
      streamChunk.arrivalMs = receivedMs;
      insertStreamChunkForDecode(streamChunk, elapsedMs);
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
      ndn_service_framework::StreamChunk recoveredChunk;
      recoveredChunk.streamId = !state.streamId.empty() ? state.streamId : activeVideoStreamId();
      recoveredChunk.sessionEpoch = state.streamSessionEpoch;
      recoveredChunk.seq = recoveredSeq;
      recoveredChunk.payload = recovered;
      recoveredChunk.contentType = "video/h264";
      recoveredChunk.arrivalMs = state.firstArrivalMs;
      insertStreamChunkForDecode(recoveredChunk, recoveredElapsed);
      state.shards[missingIdx] = recovered;
      ++m_fecRecoveredChunks;
      NDN_LOG_INFO("GS_VIDEO_FEC_RECOVERED stream=" << recoveredChunk.streamId
                   << " session=" << recoveredChunk.sessionEpoch
                   << " frame_seq=" << state.frameSeq
                   << " packet_seq=" << recoveredSeq);
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
  insertStreamChunkForDecode(const ndn_service_framework::StreamChunk& chunk,
                             uint64_t elapsedMs)
  {
    insertChunkForDecode(chunk.seq,
                         chunk.payload,
                         chunk.streamId,
                         chunk.sessionEpoch,
                         elapsedMs);
  }

  bool
  appendDecoderReadyChunksUnderLock(
    const std::vector<ndn_service_framework::StreamChunk>& readyChunks)
  {
    for (const auto& ready : readyChunks) {
      DecoderStreamChunk chunk;
      chunk.packetSeq = ready.seq;
      chunk.arrivalMs = ready.arrivalMs;
      chunk.streamSessionEpoch = ready.sessionEpoch;
      chunk.streamId = ready.streamId;
      chunk.payload = ready.payload;
      try {
        chunk.elapsedMs = std::stoull(ready.metadata.at("decoderElapsedMs"));
      }
      catch (const std::exception&) {
        chunk.elapsedMs = 0;
      }
      m_chunkQueue.push_back(std::move(chunk));
    }
    return !readyChunks.empty();
  }

  void
  insertChunkForDecode(uint64_t packetSeq,
                      const std::vector<uint8_t>& payload,
                      const std::string& streamId,
                      uint64_t streamSessionEpoch,
                      uint64_t elapsedMs)
  {
    if (packetSeq == UINT64_MAX) {
      return;
    }
    bool notifyWriter = false;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      if (!m_decoderReorderBuffer ||
          m_decoderReorderStreamId != streamId ||
          m_decoderReorderSessionEpoch != streamSessionEpoch) {
        m_decoderReorderBuffer =
          std::make_unique<ndn_service_framework::StreamConsumerReorderBuffer>(
            streamId, streamSessionEpoch, 0,
            std::max<uint64_t>(1, m_decoderBacklogLimit));
        m_decoderReorderStreamId = streamId;
        m_decoderReorderSessionEpoch = streamSessionEpoch;
        m_chunkQueue.clear();
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }

      ndn_service_framework::StreamChunk streamChunk;
      streamChunk.streamId = streamId;
      streamChunk.sessionEpoch = streamSessionEpoch;
      streamChunk.seq = packetSeq;
      streamChunk.payload = payload;
      streamChunk.contentType = "video/h264";
      streamChunk.arrivalMs = m_firstFrameMs == 0 ? 0 : m_firstFrameMs + elapsedMs;
      streamChunk.metadata["decoderElapsedMs"] = std::to_string(elapsedMs);

      const auto before = m_decoderReorderBuffer->metrics();
      notifyWriter = appendDecoderReadyChunksUnderLock(
        m_decoderReorderBuffer->push(streamChunk));
      const auto after = m_decoderReorderBuffer->metrics();
      m_decoderDroppedChunks +=
        (after.duplicates - before.duplicates) + (after.stale - before.stale);
      m_nextChunkSeqToDecode = m_decoderReorderBuffer->nextSeq();

      if (m_decoderReorderBuffer->pendingCount() >= m_decoderBacklogLimit) {
        const auto pending = m_decoderReorderBuffer->pendingSequences(1);
        if (!pending.empty() && pending.front() > m_nextChunkSeqToDecode) {
          NDN_LOG_DEBUG("GS_VIDEO_SKIP_MISSING_CHUNKS start="
                       << m_nextChunkSeqToDecode << " to=" << pending.front() - 1);
          m_decoderDroppedChunks += (pending.front() - m_nextChunkSeqToDecode);
          m_decoderReorderBuffer->skipTo(pending.front());
          notifyWriter = appendDecoderReadyChunksUnderLock(
            m_decoderReorderBuffer->drainReady()) || notifyWriter;
          m_nextChunkSeqToDecode = m_decoderReorderBuffer->nextSeq();
        }
      }

      const auto missing = m_decoderReorderBuffer->missingSequences(1);
      if (missing.empty()) {
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }
      else if (m_decoderMissingChunkSeq != missing.front()) {
        m_decoderMissingChunkSeq = missing.front();
        m_decoderMissingChunkStartMs = nowMilliseconds();
      }
      m_decoderPendingChunkCount = m_decoderReorderBuffer->pendingCount();
      m_decoderPendingBytes = m_decoderReorderBuffer->pendingBytes();
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
  m_lastOutputChunkStreamId.clear();
  m_lastOutputChunkStreamSessionEpoch = 0;
  m_decoderDroppedChunks = 0;
    m_decoderMissingChunkSeq = UINT64_MAX;
    m_decoderMissingChunkStartMs = 0;
    m_closeDecoderInputWhenQueueDrained = false;
    m_decoderOutBuffer.clear();
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_decoderReorderBuffer = std::make_unique<ndn_service_framework::StreamConsumerReorderBuffer>(
        activeVideoStreamId(), videoStreamSessionEpoch(), 0,
        std::max<uint64_t>(1, m_decoderBacklogLimit));
      m_decoderReorderStreamId = activeVideoStreamId();
      m_decoderReorderSessionEpoch = videoStreamSessionEpoch();
      m_decoderPendingChunkCount = 0;
      m_decoderPendingBytes = 0;
      m_fecRecoveredChunks = 0;
      m_nextChunkSeqToDecode = 0;
    }

    m_decoderWriterThread = std::thread([this] { decoderWriterLoop(); });
    m_decoderReaderThread = std::thread([this] { decoderReaderLoop(); });
    publishStatus("Video decoder started");
  }

  void
  decodeRecordingFromFetchedChunksAsync(RecordingDataProductState manifest)
  {
    const auto sessionId = activeRecordingPlaybackStreamId();
    auto sessionGuard = sessionId;
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
      std::thread([this, manifest = std::move(manifest), chunks = std::move(chunks),
                   sessionGuard]() mutable {
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
            m_frameCallback(std::move(frame),
                            frameCount,
                            frameCount * 33,
                            sessionGuard,
                            streamSessionEpochForStreamId(sessionGuard));
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
      m_decoderReorderBuffer.reset();
      m_decoderReorderStreamId.clear();
      m_decoderReorderSessionEpoch = 0;
      m_decoderPendingChunkCount = 0;
      m_decoderPendingBytes = 0;
      m_decoderOutBuffer.clear();
      m_lastOutputChunkStreamId.clear();
      m_lastOutputChunkStreamSessionEpoch = 0;
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
    }
    m_decoderDroppedChunks = 0;
  }

  void
  decoderWriterLoop()
  {
    while (m_decoderRunning.load()) {
      DecoderStreamChunk chunk;
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
      {
        std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
        m_lastOutputChunkStreamId = chunk.streamId;
        m_lastOutputChunkStreamSessionEpoch = chunk.streamSessionEpoch;
      }

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
      std::string streamId;
      uint64_t streamSessionEpoch = 0;
      {
        std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
        streamId = m_lastOutputChunkStreamId;
        streamSessionEpoch = m_lastOutputChunkStreamSessionEpoch;
      }
      {
        std::lock_guard<std::mutex> guard(m_latestDecodedFrameMutex);
        m_latestDecodedFrame = frame;
      }
      const auto decoded = ++m_decodedVideoFrames;
      if (decoded <= 3 || decoded % 30 == 0) {
        publishVideoAdaptiveState("decoded");
      }
      if (m_frameCallback) {
        m_frameCallback(std::move(frame),
                        decoded,
                        m_lastOutputChunkElapsedMs,
                        streamId,
                        streamSessionEpoch);
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
    if (m_decoderMissingChunkSeq == UINT64_MAX || !m_decoderReorderBuffer ||
        m_decoderReorderBuffer->pendingCount() == 0 ||
        m_decoderRunning.load() == false) {
      return;
    }

    const auto now = nowMs;
    const auto pending = m_decoderReorderBuffer->pendingSequences(1);
    if (pending.empty() || pending.front() <= m_nextChunkSeqToDecode) {
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
      return;
    }

    if (pending.front() > m_nextChunkSeqToDecode &&
        now >= m_decoderMissingChunkStartMs + dynamicDecoderMissingTimeoutMs()) {
      NDN_LOG_DEBUG("GS_VIDEO_SKIP_MISSING_CHUNKS_TIMEOUT start=" << m_decoderMissingChunkSeq
                     << " to=" << pending.front() - 1
                     << " timeoutMs=" << dynamicDecoderMissingTimeoutMs()
                     << " nowMs=" << now);
      m_decoderDroppedChunks += (pending.front() - m_nextChunkSeqToDecode);
      m_decoderReorderBuffer->skipTo(pending.front());
      appendDecoderReadyChunksUnderLock(m_decoderReorderBuffer->drainReady());
      m_nextChunkSeqToDecode = m_decoderReorderBuffer->nextSeq();
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;

      const auto missing = m_decoderReorderBuffer->missingSequences(1);
      if (!missing.empty()) {
        m_decoderMissingChunkSeq = missing.front();
        m_decoderMissingChunkStartMs = nowMs;
      }
      m_decoderPendingChunkCount = m_decoderReorderBuffer->pendingCount();
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
  ndn_service_framework::ServiceContainer m_coreContainer;
  int m_ackTimeoutMs;
  int m_timeoutMs;
  std::string m_targetDroneId;
  bool m_targetDroneLocked = false;
  mutable std::mutex m_targetMutex;
  mutable std::mutex m_missionReadyMutex;
  mutable std::mutex m_missionProgressMutex;
  mutable std::mutex m_videoStateMutex;
  mutable std::mutex m_recordingManifestMutex;
  mutable std::mutex m_catalogMutex;
  mutable std::mutex m_parameterMutex;
  mutable std::mutex m_preflightMutex;
  mutable std::mutex m_analyzeMutex;
  mutable std::mutex m_operatorLeaseMutex;
  mutable std::mutex m_issuedOperatorLeaseMutex;
  mutable std::mutex m_operatorAuthorityAlertMutex;
  std::vector<std::string> m_missionReadyDrones;
  MissionPlan m_latestMissionPlan;
  MissionProgressState m_latestMissionProgress;
  std::string m_activeVideoDroneId;
  std::string m_recordingPlaybackDroneId;
  std::string m_recordingPlaybackStreamId;
  std::map<std::string, RecordingDataProductState> m_recordingManifests;
  std::map<std::string, UavDataProductCatalogState> m_catalogByDrone;
  std::map<std::string, VehicleParameterSnapshot> m_parameterSnapshots;
  std::map<std::string, std::vector<PreflightCheckItem>> m_preflightByDrone;
  std::map<std::string, UavAnalyzeSnapshot> m_analyzeSnapshots;
  OperatorAuthorityLease m_operatorLease;
  std::vector<OperatorAuthorityLease> m_issuedOperatorLeases;
  std::map<std::string, Fields> m_operatorRevocationRecords;
  std::vector<OperatorAuthorityAlert> m_operatorAuthorityAlerts;
  std::atomic<uint64_t> m_videoBitrateKbps{8000};
  uint64_t m_videoFrameWidth = 480;
  uint64_t m_videoFecParityShards = 1;
  std::vector<std::string> m_patrolDroneIds;
  mutable std::mutex m_patrolTaskMutex;
  std::string m_activePatrolTaskId;
  bool m_patrolCancelRequested = false;
  std::string m_yoloModel;
  std::string m_yoloScript;
  std::string m_yoloWorkerScript;
  uint64_t m_linkStaleMs = 3500;
  uint64_t m_linkLostMs = 8000;
  std::string m_lostLinkAction = "notify";
  std::string m_videoBitratePolicy = "manual";
  uint64_t m_videoBitrateAutoPressureMs = 2500;
  std::string m_missionPlanFilePath;
  std::string m_operatorId;
  std::string m_defaultOperatorLeaseDrone = "all";
  std::string m_defaultOperatorLeaseScope = "control";
  uint64_t m_defaultOperatorLeaseTtlMs = 0;
  std::string m_operatorAuthorityStateFile;
  std::set<std::string> m_operatorAdminIds;
  uint64_t m_operatorAuthorityRefreshIntervalMs = 0;
  std::thread m_operatorAuthorityRefreshThread;
  std::atomic<bool> m_operatorAuthorityRefreshInFlight{false};
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
  std::function<void(std::vector<uint8_t>, uint64_t, uint64_t, std::string, uint64_t)> m_frameCallback;
  std::atomic<bool> m_containerReady{false};
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_seenVideoStart{false};
  std::atomic<bool> m_videoStartInFlight{false};
  std::atomic<bool> m_videoStopInFlight{false};
  std::atomic<uint64_t> m_videoStopSuppressUntilMs{0};
  std::atomic<bool> m_videoStopDelayInjected{false};
  std::atomic<bool> m_recordingPlaybackActive{false};
  std::atomic<uint64_t> m_videoSessionCounter{0};
  std::atomic<uint64_t> m_videoStreamSessionEpoch{0};
  std::atomic<uint64_t> m_videoStartRetries{0};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
  std::atomic<uint64_t> m_highestReceivedVideoPacketSeq{UINT64_MAX};
  std::atomic<uint64_t> m_frameNacks{0};
  std::atomic<uint64_t> m_frameTimeouts{0};
  std::atomic<uint64_t> m_duplicateVideoPackets{0};
  std::atomic<uint64_t> m_videoFramesPublished{0};
  std::atomic<uint64_t> m_decodedVideoFrames{0};
  std::atomic<uint64_t> m_lastVideoAdaptiveLogMs{0};
  std::atomic<uint64_t> m_videoBitrateAdviceSinceMs{0};
  std::atomic<uint64_t> m_lastVideoBitrateApplyMs{0};
  std::atomic<bool> m_videoBitrateChangePending{false};
  std::atomic<uint64_t> m_videoBitrateChangeFromKbps{0};
  std::atomic<uint64_t> m_videoBitrateChangeToKbps{0};
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
  std::map<std::string, std::vector<RuntimeCommandSnapshot>> m_commandHistoryByDrone;
  std::map<std::string, SafetyState> m_safetyByDrone;
  ndn::Name m_streamPrefix;
  std::string m_activeStreamId;
  std::map<std::string, uint64_t> m_streamEpochByStreamId;
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
  std::string m_lastOutputChunkStreamId;
  uint64_t m_lastOutputChunkStreamSessionEpoch = 0;
  uint64_t m_lastOutputChunkElapsedMs = 0;
  static constexpr uint64_t VIDEO_FPS = 30;
  static constexpr uint64_t INITIAL_PACKET_PROBE = 4;
  static constexpr uint64_t DEFAULT_VIDEO_RTT_MS = 120;
  static constexpr uint64_t STREAM_PUMP_INTERVAL_MS = 25;
  static constexpr uint64_t MAX_VIDEO_START_RETRIES = 2;
  static constexpr uint64_t VIDEO_BITRATE_APPLY_COOLDOWN_MS = 8000;
  static constexpr uint64_t VIDEO_STOP_CLICK_SUPPRESS_MS = 900;
  static constexpr uint64_t VIDEO_STOP_TIMEOUT_RETRY_GUARD_MS = 2500;
  std::atomic<uint64_t> m_videoRttEwmaMs{DEFAULT_VIDEO_RTT_MS};
  std::atomic<uint64_t> m_videoTimeoutPressurePercent{0};
  std::atomic<uint64_t> m_videoProbePressurePercent{0};
  std::atomic<uint64_t> m_videoDuplicatePressurePercent{0};
  std::atomic<uint64_t> m_decoderPendingChunkCount{0};
  std::atomic<uint64_t> m_decoderPendingBytes{0};
  std::atomic<uint64_t> m_fecRecoveredChunks{0};
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_videoPumpScheduled{false};
  std::mutex m_videoPacketTrackingMutex;
  std::set<uint64_t> m_videoInFlightPacketSeqs;
  std::set<uint64_t> m_videoCompletedPacketSeqs;
  std::deque<uint64_t> m_videoCompletedPacketSeqOrder;
  static constexpr size_t MAX_VIDEO_PACKET_HISTORY = 4096;
  std::mutex m_decoderQueueMutex;
  std::condition_variable m_decoderQueueCv;
  std::deque<DecoderStreamChunk> m_chunkQueue;
  std::unique_ptr<ndn_service_framework::StreamConsumerReorderBuffer> m_decoderReorderBuffer;
  std::string m_decoderReorderStreamId;
  uint64_t m_decoderReorderSessionEpoch = 0;
  std::map<uint64_t, std::vector<uint8_t>> m_recordingPlaybackChunks;
  std::map<std::pair<uint64_t, uint64_t>, FecFrameState> m_fecFrames;
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
