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
                       std::string yoloWorkerScript = "NDNSF-UAV-APP/tools/yolo_detect_worker.py")
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
    , m_videoPumpTimer(m_face.getIoContext())
  {
    if (m_patrolDroneIds.empty()) {
      m_patrolDroneIds.push_back(m_targetDroneId);
    }
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_gsCert = getOrCreateIdentity(m_keyChain, m_config.groundStationIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_config.controllerPrefix);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_config.groundStationIdentity));
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
    if (m_telemetryInFlight.exchange(true)) {
      return;
    }
    postTargetedRequest(
      droneIdentity(m_config, droneId),
      m_config.serviceTelemetryStatus,
      encodeFields({{"type", "telemetry-status"}, {"target_drone", droneId}}),
      [this, droneId](const std::string& payload) {
        m_telemetryInFlight = false;
        const auto fields = decodeFields(payload);
        publishStatus("Telemetry drone=" + fieldOr(fields, "drone_id", droneId) +
                      " alt=" + fieldOr(fields, "altitude_m", "unknown") + "m" +
                      " lat=" + fieldOr(fields, "lat", "unknown") +
                      " lon=" + fieldOr(fields, "lon", "unknown") +
                      " battery=" + fieldOr(fields, "battery_percent", "unknown") + "%" +
                      " ready=" + fieldOr(fields, "readiness", "unknown") +
                      " reason=" + fieldOr(fields, "readiness_reason", "unknown") +
                      " armed=" + fieldOr(fields, "armed", "unknown") +
                      " gps=" + fieldOr(fields, "gps_ready", "unknown") +
                      " mission=" + fieldOr(fields, "mission_status", "unknown") +
                      " video=" + fieldOr(fields, "video", "unknown"));
      },
      [this, droneId] {
        m_telemetryInFlight = false;
        publishStatus("Telemetry timeout for drone " + droneId);
      });
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
    startVideoAttempt(droneId);
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
    if (!m_streaming.load() || activeDrone != droneId) {
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
    stopDecoder();
    postRequestForDrone(droneId, droneVideoControlService(m_config, droneId),
                encodeFields({{"type", "video-control"}, {"action", "stop"}}),
                [this, droneId](const std::string& payload) {
                  m_videoStopInFlight = false;
                  {
                    std::lock_guard<std::mutex> guard(m_videoStateMutex);
                    if (m_activeVideoDroneId == droneId) {
                      m_activeVideoDroneId.clear();
                    }
                  }
                  const auto fields = decodeFields(payload);
                  publishStatus("Video stopped drone=" + droneId + ", packets=" +
                                fieldOr(fields, "stream_packets_published",
                                        fieldOr(fields, "frames_published", "0")) +
                                ", fec_groups=" +
                                fieldOr(fields, "fec_groups_published", "0"));
                },
                {},
                [this] {
                  m_videoStopInFlight = false;
                });
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
    auto& inFlight = isManualControl ? m_manualControlInFlight : m_mavlinkCommandInFlight;
    if (inFlight.exchange(true)) {
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
      [this, commandName, isManualControl, droneId](const std::string& responsePayload) {
        (isManualControl ? m_manualControlInFlight : m_mavlinkCommandInFlight) = false;
        const auto fields = decodeFields(responsePayload);
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
      [this, commandName, isManualControl] {
        (isManualControl ? m_manualControlInFlight : m_mavlinkCommandInFlight) = false;
        publishStatus("Timeout waiting for targeted MAVLink " + commandName);
      });
    return true;
  }

  bool
  sendMavlinkCommandToDroneSync(const std::string& droneId, const std::string& commandName,
                                Fields params, std::chrono::milliseconds timeout)
  {
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
        {
          std::lock_guard<std::mutex> guard(mutex);
          ok = accepted == "true" &&
               (ackResult == "accepted" || ackResult.rfind("mock", 0) == 0);
          done = true;
        }
        cv.notify_all();
      },
      [&] {
        std::lock_guard<std::mutex> guard(mutex);
        ackResult = "timeout";
        done = true;
        ok = false;
        cv.notify_all();
      });
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [&] { return done; });
    std::cout << "SINGLE_MISSION_COMMAND command=" << commandName
              << " ok=" << (done && ok ? "true" : "false")
              << " ack=" << ackResult << std::endl;
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
    return out;
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
    std::cout << "SINGLE_MISSION_START task=" << taskId
              << " provider=" << droneId << std::endl;

    auto requestMessage = makeRequest(payload);
    std::vector<ndn::Name> providerNames{droneIdentity(m_config, droneId)};
    m_face.getIoContext().post([this, requestMessage = std::move(requestMessage),
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
          std::cout << "SINGLE_MISSION_TIMEOUT task=" << taskId << std::endl;
          std::lock_guard<std::mutex> guard(mutex);
          done = true;
          ok = false;
          cv.notify_all();
        },
        [&mutex, &cv, &done, &ok, taskId](const ndn_service_framework::ResponseMessage& response) {
          const auto fields = decodeFields(responsePayload(response));
          const bool responseOk = response.getStatus() && fieldOr(fields, "accepted", "false") == "true";
          std::cout << "SINGLE_MISSION_DONE task=" << taskId
                    << " ok=" << (responseOk ? "true" : "false")
                    << " provider=" << fieldOr(fields, "drone_id", "unknown")
                    << " mission_transport=" << fieldOr(fields, "mission_transport", "unknown")
                    << " mission_ack=" << fieldOr(fields, "mission_ack", "unknown")
                    << " waypoints_forwarded=" << fieldOr(fields, "waypoints_forwarded", "0")
                    << std::endl;
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
      std::cout << "SINGLE_MISSION_TELEMETRY sample=" << i
                << " drone=" << fieldOr(telemetry, "drone_id", droneId)
                << " lat=" << fieldOr(telemetry, "lat", "unknown")
                << " lon=" << fieldOr(telemetry, "lon", "unknown")
                << " local_north_m=" << fieldOr(telemetry, "local_north_m", "unknown")
                << " local_east_m=" << fieldOr(telemetry, "local_east_m", "unknown")
                << " alt=" << fieldOr(telemetry, "altitude_m", "unknown")
                << " speed=" << fieldOr(telemetry, "groundspeed_mps", "unknown")
                << std::endl;
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
    struct GeoPoint
    {
      double lat = 0.0;
      double lon = 0.0;
    };

    struct PatrolPart
    {
      std::string id;
      std::string role;
      std::string waypoints;
      std::string assignedDrone;
      std::string completedBy;
      int attempt = 0;
      bool done = false;
    };

    struct PatrolDemoState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::map<std::string, PatrolPart> parts;
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
    sideMeters = std::clamp(sideMeters, 40.0, 1000.0);
    const auto latStep = sideMeters / 111320.0;
    const auto lonStep = sideMeters / (111320.0 * std::max(0.2, std::cos(centerLat * M_PI / 180.0)));
    auto makeWaypointText = [] (const std::string& sector,
                                const std::vector<GeoPoint>& points) {
      std::ostringstream os;
      os << sector << ":";
      for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0) {
          os << ">";
        }
        os << std::fixed << std::setprecision(6)
           << points[i].lat << "," << points[i].lon;
      }
      return os.str();
    };

    auto distanceSq = [] (const GeoPoint& a, const GeoPoint& b, double referenceLat) {
      const auto latScale = 111320.0;
      const auto lonScale = 111320.0 * std::max(0.2, std::cos(referenceLat * M_PI / 180.0));
      const auto dLat = (a.lat - b.lat) * latScale;
      const auto dLon = (a.lon - b.lon) * lonScale;
      return dLat * dLat + dLon * dLon;
    };

    auto nearestNeighborRoute = [&distanceSq] (std::vector<GeoPoint> points, double referenceLat) {
      std::vector<GeoPoint> route;
      if (points.empty()) {
        return route;
      }
      auto startIt = std::min_element(points.begin(), points.end(),
        [] (const GeoPoint& a, const GeoPoint& b) {
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
          [current, referenceLat, &distanceSq] (const GeoPoint& a, const GeoPoint& b) {
            return distanceSq(current, a, referenceLat) < distanceSq(current, b, referenceLat);
          });
        route.push_back(*nextIt);
        points.erase(nextIt);
      }
      return route;
    };

    auto clusterRouteWaypoints =
      [&distanceSq, &nearestNeighborRoute, &makeWaypointText, centerLat] (
        const std::vector<std::pair<double, double>>& points,
        size_t requestedClusters) {
        std::vector<PatrolPart> parts;
        if (points.empty() || requestedClusters == 0) {
          return parts;
        }
        std::vector<GeoPoint> allPoints;
        allPoints.reserve(points.size());
        for (const auto& point : points) {
          allPoints.push_back({point.first, point.second});
        }
        const size_t clusterCount = std::min(requestedClusters, allPoints.size());
        std::vector<GeoPoint> centers;
        centers.reserve(clusterCount);
        auto sorted = allPoints;
        std::sort(sorted.begin(), sorted.end(), [] (const GeoPoint& a, const GeoPoint& b) {
          if (a.lon == b.lon) {
            return a.lat < b.lat;
          }
          return a.lon < b.lon;
        });
        for (size_t i = 0; i < clusterCount; ++i) {
          const size_t index = std::min(sorted.size() - 1,
                                        i * sorted.size() / clusterCount);
          centers.push_back(sorted[index]);
        }

        std::vector<size_t> assignments(allPoints.size(), 0);
        for (int iteration = 0; iteration < 8; ++iteration) {
          std::vector<std::vector<GeoPoint>> groups(clusterCount);
          for (size_t pointIndex = 0; pointIndex < allPoints.size(); ++pointIndex) {
            size_t best = 0;
            double bestDistance = distanceSq(allPoints[pointIndex], centers.front(), centerLat);
            for (size_t centerIndex = 1; centerIndex < centers.size(); ++centerIndex) {
              const auto candidateDistance =
                distanceSq(allPoints[pointIndex], centers[centerIndex], centerLat);
              if (candidateDistance < bestDistance) {
                best = centerIndex;
                bestDistance = candidateDistance;
              }
            }
            assignments[pointIndex] = best;
            groups[best].push_back(allPoints[pointIndex]);
          }
          for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            if (groups[groupIndex].empty()) {
              continue;
            }
            GeoPoint nextCenter{};
            for (const auto& point : groups[groupIndex]) {
              nextCenter.lat += point.lat;
              nextCenter.lon += point.lon;
            }
            nextCenter.lat /= static_cast<double>(groups[groupIndex].size());
            nextCenter.lon /= static_cast<double>(groups[groupIndex].size());
            centers[groupIndex] = nextCenter;
          }
        }

        std::vector<std::vector<GeoPoint>> groups(clusterCount);
        for (size_t pointIndex = 0; pointIndex < allPoints.size(); ++pointIndex) {
          groups[assignments[pointIndex]].push_back(allPoints[pointIndex]);
        }
        for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
          if (groups[groupIndex].empty()) {
            continue;
          }
          const auto role = "waypoint-cluster-" + std::to_string(groupIndex);
          const auto route = nearestNeighborRoute(groups[groupIndex], centerLat);
          const auto id = "part" + std::to_string(parts.size());
          parts.push_back(PatrolPart{id, role, makeWaypointText(role, route),
                                     "", "", 0, false});
        }
        return parts;
      };

    auto makeAutoSectorParts =
      [&makeWaypointText, centerLat, centerLon, latStep, lonStep] (size_t partCount) {
        std::vector<PatrolPart> parts;
        parts.reserve(partCount);
        const auto spacing = lonStep * 1.20;
        const auto startLon = centerLon - spacing * (static_cast<double>(partCount) - 1.0) / 2.0;
        for (size_t i = 0; i < partCount; ++i) {
          const auto sectorLon = startLon + spacing * static_cast<double>(i);
          const auto sectorLat = centerLat - latStep / 2.0;
          const auto role = "patrol-cluster-" + std::to_string(i);
          std::vector<GeoPoint> route{
            {sectorLat, sectorLon - lonStep / 2.0},
            {sectorLat + latStep, sectorLon - lonStep / 2.0},
            {sectorLat + latStep, sectorLon + lonStep / 2.0},
            {sectorLat, sectorLon + lonStep / 2.0},
          };
          const auto id = "part" + std::to_string(parts.size());
          parts.push_back(PatrolPart{id, role, makeWaypointText(role, route),
                                     "", "", 0, false});
        }
        return parts;
      };

    auto parseFirstWaypointOr = [] (const std::string& waypoints, GeoPoint fallback) {
      const auto colon = waypoints.find(':');
      const auto bodyStart = colon == std::string::npos ? 0 : colon + 1;
      const auto itemEnd = waypoints.find('>', bodyStart);
      const auto item = waypoints.substr(bodyStart, itemEnd == std::string::npos ?
                                                   std::string::npos : itemEnd - bodyStart);
      const auto comma = item.find(',');
      if (comma == std::string::npos) {
        return fallback;
      }
      try {
        return GeoPoint{std::stod(item.substr(0, comma)),
                        std::stod(item.substr(comma + 1))};
      }
      catch (const std::exception&) {
        return fallback;
      }
    };

    auto appendReturnWaypoint = [] (std::string waypoints, GeoPoint returnPoint) {
      if (waypoints.empty()) {
        waypoints = "route";
      }
      if (waypoints.find(':') == std::string::npos) {
        waypoints += ':';
      }
      else if (waypoints.back() != ':') {
        waypoints += '>';
      }
      std::ostringstream point;
      point << std::fixed << std::setprecision(6)
            << returnPoint.lat << "," << returnPoint.lon;
      waypoints += point.str();
      return waypoints;
    };

    auto departurePointForDrone = [this] (const std::string& droneId, GeoPoint fallback) {
      const auto telemetry = requestTelemetryStatusForDroneSync(droneId, std::chrono::milliseconds(900));
      try {
        const auto lat = std::stod(fieldOr(telemetry, "lat", ""));
        const auto lon = std::stod(fieldOr(telemetry, "lon", ""));
        if (std::isfinite(lat) && std::isfinite(lon)) {
          return GeoPoint{lat, lon};
        }
      }
      catch (const std::exception&) {
      }
      return fallback;
    };

    std::vector<PatrolPart> plannedParts;
    if (routeWaypoints.size() >= 2) {
      plannedParts = clusterRouteWaypoints(routeWaypoints, m_patrolDroneIds.size());
    }
    if (plannedParts.empty()) {
      plannedParts = makeAutoSectorParts(m_patrolDroneIds.size());
    }
    for (size_t i = 0; i < plannedParts.size(); ++i) {
      const auto droneId = m_patrolDroneIds[i % m_patrolDroneIds.size()];
      plannedParts[i].assignedDrone = droneId;
      const auto routeStart = parseFirstWaypointOr(plannedParts[i].waypoints,
                                                   GeoPoint{centerLat, centerLon});
      const auto departure = departurePointForDrone(droneId, routeStart);
      plannedParts[i].waypoints = appendReturnWaypoint(plannedParts[i].waypoints, departure);
      state->parts.emplace(plannedParts[i].id, plannedParts[i]);
    }

    auto logLedger = [&] (const std::string& line) {
      NDN_LOG_INFO(line);
      std::cout << line << std::endl;
    };

    auto allDone = [state] {
      for (const auto& item : state->parts) {
        if (!item.second.done) {
          return false;
        }
      }
      return true;
    };

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

    auto dispatchPart = [&] (const std::string& partId, std::vector<std::string> droneIds,
                             int attempt, bool simulateNoResponse) {
      const std::string candidateText = joinDroneIds(droneIds);
      PatrolPart part;
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
        {"waypoints", part.waypoints},
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
                " waypoints=" + part.waypoints);

      auto requestMessage = makeRequest(payload);
      std::vector<ndn::Name> providerNames;
      providerNames.reserve(droneIds.size());
      for (const auto& droneId : droneIds) {
        providerNames.push_back(droneIdentity(m_config, droneId));
      }
      m_face.getIoContext().post([this, requestMessage = std::move(requestMessage),
                                  providerNames = std::move(providerNames),
                                  taskId, partId, candidateText,
                                  attempt, state,
                                  logLedger] () mutable {
        if (!m_containerReady.load() || !m_user) {
          logLedger("PATROL_RUNTIME_NOT_READY task=" + taskId +
                    " part=" + partId);
          std::lock_guard<std::mutex> guard(state->mutex);
          state->timedOut.insert(partId);
          state->cv.notify_all();
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
          [taskId, partId, candidateText, attempt, state, logLedger](const ndn::Name&) {
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
          },
          [this, taskId, partId, candidateText, attempt, state, logLedger](
            const ndn_service_framework::ResponseMessage& response) {
            const auto fields = decodeFields(responsePayload(response));
            const auto responder = fieldOr(fields, "drone_id", candidateText);
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
                        " mission_transport=" + fieldOr(fields, "mission_transport", "unknown") +
                        " waypoints_forwarded=" + fieldOr(fields, "waypoints_forwarded", "0") +
                        " waypoint_acks_accepted=" + fieldOr(fields, "waypoint_acks_accepted", "0") +
                        " last_waypoint_ack=" + fieldOr(fields, "last_waypoint_ack", "unknown"));
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
              " drones=" + joinDroneIds(m_patrolDroneIds) +
              " assignment=clustered-waypoints-return-to-start" +
              " center_lat=" + std::to_string(centerLat) +
              " center_lon=" + std::to_string(centerLon) +
              " side_m=" + std::to_string(sideMeters));
    logLedger("PATROL_ATTEMPT task=" + taskId + " attempt=1 parts=" + allPartIds);
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
        logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=1");
        return true;
      }
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

    for (const auto& partId : missingParts) {
      logLedger("PATROL_COMPENSATION task=" + taskId +
                " attempt=2 parts=" + partId +
                " candidates=" + joinDroneIds(m_patrolDroneIds));
      dispatchPart(partId, m_patrolDroneIds, 2, false);
    }

    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, allDone);
      if (!allDone()) {
        logLedger("PATROL_TASK_FAILED task=" + taskId);
        return false;
      }
    }
    logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=2");
    return true;
  }

private:
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
  startVideoAttempt(std::string droneId)
  {
    postRequestForDrone(droneId, droneVideoControlService(m_config, droneId),
                encodeFields({
                  {"type", "video-control"},
                  {"action", "start"},
                  {"fps", std::to_string(VIDEO_FPS)},
                  {"requested_bitrate_kbps", std::to_string(m_videoBitrateKbps)},
                  {"requested_frame_width", std::to_string(m_videoFrameWidth)},
                }),
                [this, droneId](const std::string& payload) {
                  const auto fields = decodeFields(payload);
                  const auto prefix = fieldOr(fields, "stream_prefix", "");
                  const auto seqText = fieldOr(fields, "next_seq", "0");
                  if (prefix.empty()) {
                    publishStatus("Video control response missing stream prefix");
                    return;
                  }

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
                  m_highestReceivedVideoPacketSeq = UINT64_MAX;
                  m_nextChunkSeqToDecode = 0;
                  {
                    std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
                    m_chunkQueue.clear();
                    m_pendingChunks.clear();
                    m_decoderOutBuffer.clear();
                  }
                  stopDecoder();
                  startDecoder();
                  publishStatus("Video packet stream drone=" + droneId + " from " + prefix);
                  requestVideoPackets();
                },
                [this, droneId] {
                  if (m_seenVideoStart.load()) {
                    return true;
                  }
                  const uint64_t retry = m_videoStartRetries.fetch_add(1);
                  if (retry < MAX_VIDEO_START_RETRIES) {
                    publishStatus("Video start retry " + std::to_string(retry + 1));
                    m_face.getIoContext().post([this, droneId] {
                      startVideoAttempt(droneId);
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

  struct RecordingManifest
  {
    std::string droneId;
    std::string sessionId;
    std::string objectPrefix;
    std::string encryption;
    std::string keyId;
    std::vector<uint8_t> contentKey;
    uint64_t chunks = 0;
    uint64_t bytes = 0;
  };

  void
  requestRecordingManifestForDrone(const std::string& droneId, bool playAfterRefresh)
  {
    postRequestForDrone(
      droneId,
      droneCameraRecordingManifestService(m_config, droneId),
      encodeFields({{"type", "camera-recording-manifest-request"}}),
      [this, droneId, playAfterRefresh](const std::string& payload) {
        const auto fields = decodeFields(payload);
        RecordingManifest manifest;
        manifest.droneId = fieldOr(fields, "drone_id", droneId);
        manifest.sessionId = fieldOr(fields, "recording_session_id", "");
        manifest.objectPrefix = fieldOr(fields, "recording_object_prefix", "");
        manifest.encryption = fieldOr(fields, "recording_encryption", "none");
        manifest.keyId = fieldOr(fields, "recording_encryption_key_id", "");
        manifest.contentKey = hexDecode(fieldOr(fields, "recording_encryption_content_key_hex", ""));
        manifest.chunks = fieldAsUint64(fields, "recording_chunks", 0);
        manifest.bytes = fieldAsUint64(fields, "recording_bytes", 0);
        {
          std::lock_guard<std::mutex> guard(m_recordingManifestMutex);
          m_recordingManifests[droneId] = manifest;
        }
        publishStatus("Recording manifest drone=" + droneId +
                      " chunks=" + std::to_string(manifest.chunks) +
                      " bytes=" + std::to_string(manifest.bytes) +
                      " session=" + manifest.sessionId +
                      " encryption=" + manifest.encryption);
        if (playAfterRefresh) {
          startRecordingPlayback(manifest);
        }
      });
  }

  void
  startRecordingPlayback(const RecordingManifest& manifest)
  {
    if (manifest.objectPrefix.empty() || manifest.sessionId.empty() || manifest.chunks == 0) {
      publishStatus("No recorded video chunks for drone " + manifest.droneId);
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
  fetchRecordingChunk(RecordingManifest manifest, uint64_t index, uint64_t stride)
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

    const auto objectName = manifest.objectPrefix + "/" + manifest.sessionId +
      "/chunk/" + std::to_string(index);
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
  fetchRecordingChunkData(const RecordingManifest& manifest,
                          const std::string& objectName,
                          std::function<void(std::vector<uint8_t>)> onData,
                          std::function<void()> onTimeout)
  {
    m_face.getIoContext().post([this, manifest, objectName,
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
  decryptRecordingChunkData(const RecordingManifest& manifest,
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
    std::cout << "GS_STATUS " << value << std::endl;
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
    m_face.getIoContext().post([this, service, payload,
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
    m_face.getIoContext().post([this, provider, droneId, service, payload,
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
    m_face.getIoContext().post([this, provider, service, payload,
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
                         fieldAsUint64(fields, "target_bitrate_kbps", m_videoBitrateKbps)));
    const auto payloadBytes = std::max<uint64_t>(
      512, fieldAsUint64(fields, "max_payload_bytes", 3600));
    const auto fps = std::max<uint64_t>(1, fieldAsUint64(fields, "fps", VIDEO_FPS));
    const auto frameWidth = std::max<uint64_t>(
      1, fieldAsUint64(fields, "accepted_frame_width",
                       fieldAsUint64(fields, "frame_width", m_videoFrameWidth)));
    const auto bytesPerSecond = (bitrateKbps * 1000 + 7) / 8;
    const auto estimatedPacketsPerSecond =
      std::max<uint64_t>(fps, (bytesPerSecond + payloadBytes - 1) / payloadBytes);

    m_keyPacketsPerSecond = std::clamp<uint64_t>(
      (estimatedPacketsPerSecond + 7) / 8, 4, 16);
    m_deltaPacketsPerSecond = std::clamp<uint64_t>(
      estimatedPacketsPerSecond + m_keyPacketsPerSecond + 8, 24, 180);
    m_keyWindow = std::clamp<uint64_t>(m_keyPacketsPerSecond, 4, 16);
    m_videoRttEwmaMs = DEFAULT_VIDEO_RTT_MS;
    m_deltaWindow = dynamicVideoWindow();

    NDN_LOG_INFO("GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
                 << " frameWidth=" << frameWidth
                 << " payloadBytes=" << payloadBytes
                 << " fps=" << fps
                 << " keyBudget=" << m_keyPacketsPerSecond
                 << " deltaBudget=" << m_deltaPacketsPerSecond
                 << " keyWindow=" << m_keyWindow
                 << " deltaWindow=" << m_deltaWindow
                 << " rttMs=" << videoRttMs());
    std::cout << "GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
              << " frameWidth=" << frameWidth
              << " keyBudget=" << m_keyPacketsPerSecond
              << " deltaBudget=" << m_deltaPacketsPerSecond
              << " deltaWindow=" << m_deltaWindow
              << " rttMs=" << videoRttMs() << std::endl;
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

  uint64_t
  packetsForDurationMs(uint64_t durationMs, uint64_t minValue, uint64_t maxValue) const
  {
    const auto packets = (m_deltaPacketsPerSecond * durationMs + 999) / 1000;
    return std::clamp<uint64_t>(packets, minValue, maxValue);
  }

  uint64_t
  dynamicVideoWindow() const
  {
    const auto rtt = videoRttMs();
    const auto targetBufferMs = std::clamp<uint64_t>(rtt + 80, 180, 750);
    return packetsForDurationMs(targetBufferMs, 8, 128);
  }

  uint64_t
  dynamicVideoLookahead() const
  {
    const auto rtt = videoRttMs();
    const auto futureMs = std::clamp<uint64_t>(rtt / 3 + 40, 50, 250);
    return packetsForDurationMs(futureMs, 2, 64);
  }

  uint64_t
  dynamicProbeBackoffMs() const
  {
    return std::clamp<uint64_t>(videoRttMs() / 2, 60, 500);
  }

  uint64_t
  dynamicInterestLifetimeMs() const
  {
    return std::clamp<uint64_t>(videoRttMs() * 3 + 200, 600, 3000);
  }

  uint64_t
  dynamicDecoderMissingTimeoutMs() const
  {
    return std::clamp<uint64_t>(videoRttMs() + videoRttMs() / 2, 180, 1200);
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
      if (lane.probeNotBeforeMs > 0 &&
          nowMilliseconds() < lane.probeNotBeforeMs &&
          lane.nextSeq >= lane.advertisedPackets) {
        scheduleVideoPump(dynamicProbeBackoffMs());
        break;
      }
      const auto packetSeq = lane.nextSeq++;
      ++lane.inFlight;
      const auto sentMs = nowMilliseconds();
      const auto advertisedAtSend = lane.advertisedPackets;
      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packetSeq));
      auto interest = ndn::Interest(packetName);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(ndn::time::milliseconds(dynamicInterestLifetimeMs()));

      m_face.expressInterest(
        interest,
        [this, &lane, packetSeq, sentMs, advertisedAtSend](const ndn::Interest&, const ndn::Data& data) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          lane.probeNotBeforeMs = 0;
          advanceLaneIfStale(lane);
        const auto receivedCount = ++m_receivedChunks;
        const auto receivedMs = nowMilliseconds();
        if (advertisedAtSend > packetSeq) {
          recordVideoRtt(sentMs, receivedMs);
        }
        if (receivedCount <= 3 || receivedCount % 30 == 0) {
          NDN_LOG_INFO("GS_VIDEO_CHUNK count=" << receivedCount
                         << " packetSeq=" << packetSeq
                       << " name=" << data.getName()
                       << " bytes=" << data.getContent().value_size()
                       << " rttMs=" << videoRttMs()
                       << " window=" << dynamicVideoWindow());
          std::cout << "GS_VIDEO_CHUNK count=" << receivedCount
                      << " packetSeq=" << packetSeq
                    << " bytes=" << data.getContent().value_size()
                    << " rttMs=" << videoRttMs()
                    << " window=" << dynamicVideoWindow() << std::endl;
        }
        if (m_firstFrameMs == 0) {
          m_firstFrameMs = receivedMs;
        }
        const auto content = data.getContent();
        std::vector<uint8_t> bytes(content.value(), content.value() + content.value_size());
          try {
            const auto packet = decodeVideoPacket(bytes);
            updateHighestReceivedVideoPacket(packet.packetSeq);
            updateLaneHighWatermark(lane, packet);
            queueStreamChunk(packet, receivedMs);
          }
          catch (const std::exception& e) {
            NDN_LOG_WARN("GS_VIDEO_PACKET_DECODE_FAILED " << e.what());
          }
          requestVideoPackets();
      },
        [this, &lane, packetSeq](const ndn::Interest&, const ndn::lp::Nack&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
        const auto nackCount = ++m_frameNacks;
        if (nackCount <= 3 || nackCount % 30 == 0) {
            NDN_LOG_INFO("GS_VIDEO_NACK count=" << nackCount << " packetSeq=" << packetSeq);
          std::cout << "GS_VIDEO_NACK count=" << nackCount
                      << " packetSeq=" << packetSeq << std::endl;
        }
          advanceLaneIfStale(lane);
          requestVideoPackets();
      },
        [this, &lane, packetSeq](const ndn::Interest&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          const bool isFutureProbe =
            packetSeq >= lane.advertisedPackets ||
            isBeyondHighestReceivedVideoPacket(packetSeq);
          if (isFutureProbe && lane.nextSeq > packetSeq) {
            NDN_LOG_DEBUG("GS_VIDEO_FUTURE_PROBE_TIMEOUT packetSeq=" << packetSeq
                          << " advertisedPackets=" << lane.advertisedPackets
                          << " highestReceived=" << m_highestReceivedVideoPacketSeq.load());
            lane.nextSeq = packetSeq;
            lane.probeNotBeforeMs = nowMilliseconds() + dynamicProbeBackoffMs();
          }
          else {
            const auto timeoutCount = ++m_frameTimeouts;
            if (timeoutCount <= 3 || timeoutCount % 30 == 0) {
              NDN_LOG_INFO("GS_VIDEO_TIMEOUT count=" << timeoutCount
                           << " packetSeq=" << packetSeq);
              std::cout << "GS_VIDEO_TIMEOUT count=" << timeoutCount
                        << " packetSeq=" << packetSeq << std::endl;
            }
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

      if (m_pendingChunks.size() > m_decoderReorderWindow * 4 &&
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
      m_nextChunkSeqToDecode = 0;
    }

    m_decoderWriterThread = std::thread([this] { decoderWriterLoop(); });
    m_decoderReaderThread = std::thread([this] { decoderReaderLoop(); });
    publishStatus("Video decoder started");
  }

  void
  decodeRecordingFromFetchedChunksAsync(RecordingManifest manifest)
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
  mutable std::mutex m_videoStateMutex;
  mutable std::mutex m_recordingManifestMutex;
  std::vector<std::string> m_missionReadyDrones;
  std::string m_activeVideoDroneId;
  std::string m_recordingPlaybackDroneId;
  std::map<std::string, RecordingManifest> m_recordingManifests;
  uint64_t m_videoBitrateKbps = 8000;
  uint64_t m_videoFrameWidth = 480;
  std::vector<std::string> m_patrolDroneIds;
  std::string m_yoloModel;
  std::string m_yoloScript;
  std::string m_yoloWorkerScript;
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
  std::atomic<bool> m_recordingPlaybackActive{false};
  std::atomic<uint64_t> m_videoStartRetries{0};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
  std::atomic<uint64_t> m_highestReceivedVideoPacketSeq{UINT64_MAX};
  std::atomic<uint64_t> m_frameNacks{0};
  std::atomic<uint64_t> m_frameTimeouts{0};
  std::atomic<bool> m_mavlinkCommandInFlight{false};
  std::atomic<bool> m_manualControlInFlight{false};
  std::atomic<bool> m_telemetryInFlight{false};
  ndn::Name m_streamPrefix;
  PacketLane m_keyLane;
  PacketLane m_deltaLane;
  uint64_t m_keyPacketsPerSecond = 16;
  uint64_t m_deltaPacketsPerSecond = 160;
  uint64_t m_keyWindow = 16;
  uint64_t m_deltaWindow = 108;
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
  static constexpr uint64_t m_decoderReorderWindow = 12;
  static constexpr uint64_t MAX_VIDEO_START_RETRIES = 2;
  std::atomic<uint64_t> m_videoRttEwmaMs{DEFAULT_VIDEO_RTT_MS};
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_videoPumpScheduled{false};
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
