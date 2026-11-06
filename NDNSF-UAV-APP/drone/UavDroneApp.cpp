#include "drone-service-container.hpp"

namespace {

#include "DroneWindow.inc.hpp"

} // namespace

int
main(int argc, char** argv)
{
  try {
    const auto describe = getOption(argc, argv, "--describe-person-computation", "");
    if (hasFlag(argc, argv, "--describe-person-computation")) {
      if (describe != "stdout") throw std::invalid_argument("descriptor accepts stdout only");
      const auto requiredPath = [&] (const std::string& option) {
        const auto path = getOption(argc, argv, option, "");
        if (path.empty() || path.rfind("--", 0) == 0) {
          throw std::invalid_argument("missing descriptor option " + option);
        }
        return std::filesystem::path(path);
      };
      std::cout << describeUavPersonComputation(
        requiredPath("--model"), requiredPath("--tracking-profile"),
        requiredPath("--reid-model"), requiredPath("--worker-python"),
        requiredPath("--worker-script")) << std::endl;
      return 0;
    }
    const auto appConfig = loadKeyValueConfig(getOption(argc, argv, "--app-config", ""));
    const std::string droneId = getConfigOption(argc, argv, appConfig, "--drone-id", "drone-id", "A");
    const bool available = getConfigBoolInvertedFlag(
      argc, argv, appConfig, "--unavailable", "available", true);
    const bool serveCertificates = getConfigBoolInvertedFlag(
      argc, argv, appConfig, "--no-serve-certificates", "serve-certificates", true);
    const std::string videoPath = resolveVideoSource(argc, argv, appConfig);
    const std::string flightControllerBackend =
      getConfigOption(argc, argv, appConfig, "--flight-controller-backend",
                      "flight-controller-backend", "mock");
    const std::string mavlinkUdpHost =
      getConfigOption(argc, argv, appConfig, "--mavlink-udp-host",
                      "mavlink-udp-host", "127.0.0.1");
    const std::string mavlinkUdpPort =
      getConfigOption(argc, argv, appConfig, "--mavlink-udp-port",
                      "mavlink-udp-port", "18570");
    const std::string mavlinkUdpListenPort =
      getConfigOption(argc, argv, appConfig, "--mavlink-udp-listen-port",
                      "mavlink-udp-listen-port", "14550");
    const std::string mavlinkSerialDevice =
      getConfigOption(argc, argv, appConfig, "--mavlink-serial-device",
                      "mavlink-serial-device", "/dev/ttyAMA0");
    const std::string mavlinkSerialBaud =
      getConfigOption(argc, argv, appConfig, "--mavlink-serial-baud",
                      "mavlink-serial-baud", "57600");
    const bool configurePx4SitlDemoParams = getConfigBool(
      argc, argv, appConfig, "--configure-px4-sitl-demo-params",
      "configure-px4-sitl-demo-params", false);
    VideoPublisher::CameraRuntimeOptions cameraOptions;
    cameraOptions.captureOnStart = getConfigBool(
      argc, argv, appConfig, "--camera-capture-on-start",
      "camera-capture-on-start", false);
    cameraOptions.recordToLocalRepo = getConfigBool(
      argc, argv, appConfig, "--camera-record-to-local-repo",
      "camera-record-to-local-repo", false);
    cameraOptions.recordRepoPath = getConfigOption(
      argc, argv, appConfig, "--camera-record-repo-path",
      "camera-record-repo-path", "");
    cameraOptions.recordObjectPrefix = getConfigOption(
      argc, argv, appConfig, "--camera-record-object-prefix",
      "camera-record-object-prefix", "");
    cameraOptions.recordPacketLimit = std::stoull(getConfigOption(
      argc, argv, appConfig, "--camera-retention-packet-limit",
      "camera-retention-packet-limit", "0"));
    cameraOptions.v4l2InputFormat = getConfigOption(
      argc, argv, appConfig, "--camera-v4l2-input-format",
      "camera-v4l2-input-format", cameraOptions.v4l2InputFormat);
    cameraOptions.v4l2InputSize = getConfigOption(
      argc, argv, appConfig, "--camera-v4l2-input-size",
      "camera-v4l2-input-size", cameraOptions.v4l2InputSize);
    cameraOptions.v4l2InputFps = std::stoull(getConfigOption(
      argc, argv, appConfig, "--camera-v4l2-input-fps",
      "camera-v4l2-input-fps", std::to_string(cameraOptions.v4l2InputFps)));
    const bool autoCameraRecordSmoke = getConfigBool(
      argc, argv, appConfig, "--auto-camera-record-smoke",
      "auto-camera-record-smoke", false);
    const bool headless = getConfigBool(
      argc, argv, appConfig, "--headless", "headless", false);
    const auto autoCameraRecordExpectedChunks = std::stoull(getConfigOption(
      argc, argv, appConfig, "--auto-camera-record-expected-chunks",
      "auto-camera-record-expected-chunks", "3"));
    const auto autoCameraRecordTimeoutSeconds = std::stoull(getConfigOption(
      argc, argv, appConfig, "--auto-camera-record-timeout-seconds",
      "auto-camera-record-timeout-seconds", "10"));
    const std::string flightControllerStatusFile =
      getConfigOption(argc, argv, appConfig, "--fc-status-file", "fc-status-file", "");
    UavRuntimeConfig config = loadUavRuntimeConfig(
      getConfigOption(argc, argv, appConfig, "--runtime-config", "runtime-config",
                      "NDNSF-UAV-APP/configs/uav_runtime.conf"));
    config.groupPrefix = ndn::Name(getConfigOption(argc, argv, appConfig, "--group-prefix", "group-prefix", config.groupPrefix.toUri()));
    config.controllerPrefix = ndn::Name(getConfigOption(argc, argv, appConfig, "--controller-prefix", "controller-prefix", config.controllerPrefix.toUri()));
    config.droneIdentityPrefix = ndn::Name(getConfigOption(argc, argv, appConfig, "--drone-prefix", "drone-prefix", config.droneIdentityPrefix.toUri()));
    config.trustSchema = getConfigOption(argc, argv, appConfig, "--trust-schema", "trust-schema", config.trustSchema);
    config.serviceMavlinkExecute = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-mavlink-execute", "service-mavlink-execute", config.serviceMavlinkExecute.toUri()));
    config.serviceMissionAssign = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-mission-assign", "service-mission-assign", config.serviceMissionAssign.toUri()));
    config.serviceTelemetryStatus = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-telemetry-status", "service-telemetry-status", config.serviceTelemetryStatus.toUri()));
    config.serviceCameraFrame = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-camera-frame", "service-camera-frame", config.serviceCameraFrame.toUri()));
    config.serviceCameraVideoControlSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-camera-video-control-suffix", "service-camera-video-control-suffix", config.serviceCameraVideoControlSuffix.toUri()));
    config.serviceCameraRecordingManifestSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-camera-recording-manifest-suffix", "service-camera-recording-manifest-suffix", config.serviceCameraRecordingManifestSuffix.toUri()));
    config.serviceCameraRepoCatalogSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-camera-repo-catalog-suffix", "service-camera-repo-catalog-suffix", config.serviceCameraRepoCatalogSuffix.toUri()));
    config.serviceMavlinkParametersSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-mavlink-parameters-suffix", "service-mavlink-parameters-suffix", config.serviceMavlinkParametersSuffix.toUri()));
    config.serviceMavlinkParameterEditSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-mavlink-parameter-edit-suffix", "service-mavlink-parameter-edit-suffix", config.serviceMavlinkParameterEditSuffix.toUri()));
    config.serviceMavlinkAnalyzeSnapshotSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-mavlink-analyze-snapshot-suffix", "service-mavlink-analyze-snapshot-suffix", config.serviceMavlinkAnalyzeSnapshotSuffix.toUri()));
    config.servicePreflightChecklistSuffix = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-preflight-checklist-suffix", "service-preflight-checklist-suffix", config.servicePreflightChecklistSuffix.toUri()));
    config.serviceGsObjectDetection = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-object-detection", "service-gs-object-detection", config.serviceGsObjectDetection.toUri()));

    if (autoCameraRecordSmoke) {
      cameraOptions.captureOnStart = true;
      cameraOptions.recordToLocalRepo = true;
      if (cameraOptions.recordPacketLimit < autoCameraRecordExpectedChunks + 100) {
        cameraOptions.recordPacketLimit = autoCameraRecordExpectedChunks + 100;
      }
      if (cameraOptions.recordRepoPath.empty()) {
        cameraOptions.recordRepoPath = "/tmp/ndnsf-uav-camera-record-smoke.sqlite3";
      }

      ndn::Face smokeFace;
      ndn::KeyChain smokeKeyChain;
      ndn_service_framework::LocalServiceRegistry smokeLocalRegistry;
      const auto smokeProviderCert = getOrCreateIdentity(
        smokeKeyChain, droneIdentity(config, droneId));
      const auto smokeControllerCert = getOrCreateIdentity(
        smokeKeyChain, config.controllerPrefix);
      ndn_service_framework::ServiceProvider smokeProvider(
        ndn_service_framework::ServiceProvider::LocalMockTag{},
        smokeFace, config.groupPrefix, smokeProviderCert, smokeControllerCert,
        config.trustSchema);
      const auto smokeLocalRecordingChunkService =
        droneIdentity(config, droneId).append("Local").append("Recording").append("Chunk");
      VideoPublisher smokePublisher(
        smokeProvider, smokeFace, smokeKeyChain, smokeLocalRegistry,
        smokeLocalRecordingChunkService,
        config, droneId, videoPath, cameraOptions);
      std::thread smokeFaceThread([&smokeFace] {
        try {
          smokeFace.processEvents();
        }
        catch (const std::exception&) {
        }
      });
      auto smokeFaceGuard = std::unique_ptr<ndn::Face, std::function<void(ndn::Face*)>>(
        &smokeFace, [&smokeFaceThread] (ndn::Face* face) {
          face->shutdown();
          if (smokeFaceThread.joinable()) {
            smokeFaceThread.join();
          }
        });

      const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(autoCameraRecordTimeoutSeconds);
      while (std::chrono::steady_clock::now() < deadline &&
             smokePublisher.recordingChunks() < 1) {
        std::this_thread::sleep_for(100ms);
      }
      const auto chunksBeforeStream = smokePublisher.recordingChunks();
      smokePublisher.start(Fields{
        {"requested_bitrate_kbps", "8000"},
        {"requested_frame_width", "480"},
        {"fps", "30"},
      });
      while (std::chrono::steady_clock::now() < deadline &&
             smokePublisher.recordingChunks() <= chunksBeforeStream) {
        std::this_thread::sleep_for(100ms);
      }
      smokePublisher.stop();
      const auto chunksAfterStop = smokePublisher.recordingChunks();
      const auto targetChunks = std::max<uint64_t>(
        autoCameraRecordExpectedChunks, chunksAfterStop + 1);
      while (std::chrono::steady_clock::now() < deadline &&
             smokePublisher.recordingChunks() < targetChunks) {
        std::this_thread::sleep_for(100ms);
      }
      smokePublisher.shutdown();
      smokeFaceGuard.reset();
      if (smokePublisher.recordingChunks() < targetChunks ||
          smokePublisher.recordingBytes() == 0 ||
          smokePublisher.recordingChunks() <= chunksAfterStop) {
        const auto manifest = smokePublisher.recordingManifestFields();
        std::cerr << "DRONE_CAMERA_RECORD_SMOKE_FAILED chunks="
                  << smokePublisher.recordingChunks()
                  << " bytes=" << smokePublisher.recordingBytes()
                  << " chunks_after_stop=" << chunksAfterStop
                  << " catalog_entries=" << fieldOr(manifest, "packet_catalog_entries", "0")
                  << " repo=" << cameraOptions.recordRepoPath << std::endl;
        return 1;
      }
      const auto manifest = smokePublisher.recordingManifestFields();
      std::cout << "DRONE_CAMERA_RECORD_SMOKE_OK chunks="
                << smokePublisher.recordingChunks()
                << " bytes=" << smokePublisher.recordingBytes()
                << " chunks_after_stop=" << chunksAfterStop
                << " catalog_entries=" << fieldOr(manifest, "packet_catalog_entries", "0")
                << " repo=" << cameraOptions.recordRepoPath
                << " prefix=" << smokePublisher.recordingPrefix()
                << std::endl;
      return 0;
    }

    std::optional<UavTrackingProviderConfig> trackingConfig;
    const auto trackingConfigPath = getConfigOption(
      argc, argv, appConfig, "--tracking-provider-config", "tracking-provider-config", "");
    if (!trackingConfigPath.empty()) trackingConfig = loadTrackingProviderConfig(trackingConfigPath);

    std::cerr << "DRONE_RUNTIME_CONSTRUCT_BEGIN drone=" << droneId << std::endl;
    auto runtime = std::make_unique<DroneServiceContainer>(
      droneId, available, serveCertificates, config, videoPath,
      flightControllerBackend, mavlinkUdpHost, mavlinkUdpPort, mavlinkUdpListenPort,
      mavlinkSerialDevice, mavlinkSerialBaud, configurePx4SitlDemoParams,
      std::move(cameraOptions), std::move(trackingConfig));
    std::cerr << "DRONE_RUNTIME_CONSTRUCT_END drone=" << droneId << std::endl;
    // Keep startup failures observable in headless MiniNDN/SITL runs.  The
    // container already emits structured status messages; wire them to the
    // process log before start() so a readiness timeout cannot hide the cause.
    runtime->setStatusCallback([droneId] (std::string status) {
      std::cerr << "DRONE_RUNTIME_STATUS drone=" << droneId
                << " status=" << status << std::endl;
    });
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("drone NDNSF runtime did not become ready");
    }
    if (headless) {
      std::signal(SIGINT, handleStopSignal);
      std::signal(SIGTERM, handleStopSignal);
      NDN_LOG_INFO("UavDroneApp headless ready identity=" << runtime->identityUri()
                   << " available=" << available
                   << " video_source=" << videoPath
                   << " flight_controller_backend=" << flightControllerBackend
                   << " camera_capture=" << (runtime->isCapturing() ? "on" : "off")
                   << " camera_recording=" << (runtime->isRecording() ? "on" : "off"));
      std::cout << "DRONE_HEADLESS_READY identity=" << runtime->identityUri()
                << " video_source=" << videoPath
                << " flight_controller_backend=" << flightControllerBackend
                << " camera_capture=" << (runtime->isCapturing() ? "on" : "off")
                << " camera_recording=" << (runtime->isRecording() ? "on" : "off")
                << std::endl;
      while (g_stopHeadless == 0) {
        std::this_thread::sleep_for(500ms);
        const auto telemetry = runtime->latestTelemetryState();
        const auto readiness = runtime->latestReadinessState();
        const auto video = runtime->latestVideoState();
        const auto safety = SafetyState::fromTelemetry(telemetry);
        NDN_LOG_INFO("DRONE_HEADLESS_STATUS identity=" << runtime->identityUri()
                     << " readiness=" << readiness.readiness
                     << " readiness_reason=" << readiness.readinessReason
                     << " heartbeat=" << readiness.heartbeatSeen
                     << " fc_ready=" << readiness.flightControllerReady
                     << " gps_ready=" << readiness.gpsReady
                     << " ekf_ready=" << readiness.ekfReady
                     << " armed=" << readiness.armed
                     << " landed=" << readiness.landedStateName
                     << " video=" << video.status
                     << " capture=" << video.capture
                     << " recording=" << video.recording
                     << " stream_packets=" << runtime->streamPacketsPublished()
                     << " fec_groups=" << runtime->fecGroupsPublished()
                     << " recorded_chunks=" << runtime->recordingChunks()
                     << " lat=" << telemetry.lat
                     << " lon=" << telemetry.lon
                     << " alt=" << telemetry.altitudeM
                     << " link=" << safety.linkState
                     << " manual=" << safety.manualControlState
                     << " neutral=" << safety.manualNeutralSent);
      }
      if (!runtime->shutdownRuntime()) std::_Exit(EXIT_FAILURE);
      std::cout << "DRONE_HEADLESS_EXIT identity=" << runtime->identityUri()
                << std::endl;
      return 0;
    }
    auto app = Gtk::Application::create("org.ndnsf.uav.drone", Gio::APPLICATION_NON_UNIQUE);
    DroneWindow window(*runtime, flightControllerStatusFile);
    NDN_LOG_INFO("UavDroneApp ready identity=" << runtime->identityUri()
                 << " available=" << available
                 << " video_source=" << videoPath
                 << " flight_controller_backend=" << flightControllerBackend
                 << " mavlink_udp=" << mavlinkUdpHost << ":" << mavlinkUdpPort
                 << " mavlink_listen_port=" << mavlinkUdpListenPort
                 << " mavlink_serial=" << mavlinkSerialDevice << "@" << mavlinkSerialBaud
                 << " configure_px4_sitl_demo_params="
                 << (configurePx4SitlDemoParams ? "true" : "false")
                 << " camera_capture=" << (runtime->isCapturing() ? "on" : "off")
                 << " camera_recording=" << (runtime->isRecording() ? "on" : "off"));
    std::cout << "DRONE_GUI_READY identity=" << runtime->identityUri()
              << " video_source=" << videoPath
              << " flight_controller_backend=" << flightControllerBackend
              << " mavlink_udp=" << mavlinkUdpHost << ":" << mavlinkUdpPort
              << " mavlink_listen_port=" << mavlinkUdpListenPort
              << " mavlink_serial=" << mavlinkSerialDevice << "@" << mavlinkSerialBaud
              << " configure_px4_sitl_demo_params="
              << (configurePx4SitlDemoParams ? "true" : "false")
              << " camera_capture=" << (runtime->isCapturing() ? "on" : "off")
              << " camera_recording=" << (runtime->isRecording() ? "on" : "off")
              << std::endl;
    const int rc = app->run(window);
    if (!runtime->shutdownRuntime()) std::_Exit(EXIT_FAILURE);
    std::cout << "DRONE_GUI_EXIT rc=" << rc << std::endl;
    return rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavDroneApp error: " << e.what() << std::endl;
    return 1;
  }
}
