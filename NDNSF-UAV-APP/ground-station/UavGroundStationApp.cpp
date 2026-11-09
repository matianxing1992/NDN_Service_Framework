#include "ground-station-service-container.hpp"
#include "uav-tracking-panel.hpp"
#include "../shared/uav-gtk-runtime.hpp"

#include <csignal>

namespace {

volatile std::sig_atomic_t g_trackingStop = 0;

void
onTrackingSignal(int)
{
  g_trackingStop = 1;
}

#include "GroundStationWindow.inc.hpp"

} // namespace

namespace {

int
runGroundStationApp(int argc, char** argv, bool& gtkInitialized)
{
  try {
    const auto appConfig = loadKeyValueConfig(getOption(argc, argv, "--app-config", ""));
    const bool serveCertificates = getConfigBoolInvertedFlag(
      argc, argv, appConfig, "--no-serve-certificates", "serve-certificates", true);
    const bool objectDetectionMode = getConfigBool(
      argc, argv, appConfig, "--serve-object-detection", "serve-object-detection", false);
    const bool autoStart = getConfigBool(argc, argv, appConfig, "--auto-video-test", "auto-video-test", false);
    const bool autoMavlinkTest = getConfigBool(argc, argv, appConfig, "--auto-mavlink-test", "auto-mavlink-test", false);
    const bool autoTelemetryTest = getConfigBool(argc, argv, appConfig, "--auto-telemetry-test", "auto-telemetry-test", false);
    const bool autoTelemetryAllowMockFields = getConfigBool(argc, argv, appConfig,
                                                            "--auto-telemetry-allow-mock-fields",
                                                            "auto-telemetry-allow-mock-fields", false);
    const bool autoKeyboardTest = getConfigBool(argc, argv, appConfig, "--auto-keyboard-test", "auto-keyboard-test", false);
    const bool autoManualControlTest = getConfigBool(argc, argv, appConfig, "--auto-manual-control-test", "auto-manual-control-test", false);
    const bool autoTwoDroneSwitchTest = getConfigBool(argc, argv, appConfig, "--auto-two-drone-switch-test", "auto-two-drone-switch-test", false);
    const bool autoLinkStateTest = getConfigBool(argc, argv, appConfig, "--auto-link-state-test", "auto-link-state-test", false);
    const bool autoVideoSelectionTest = getConfigBool(argc, argv, appConfig, "--auto-video-selection-test", "auto-video-selection-test", false);
    const bool autoMissionControlsTest = getConfigBool(argc, argv, appConfig, "--auto-mission-controls-test", "auto-mission-controls-test", false);
    const bool autoFlightControlsTest = getConfigBool(argc, argv, appConfig, "--auto-flight-controls-test", "auto-flight-controls-test", false);
    const bool autoRecordingPlaybackTest = getConfigBool(argc, argv, appConfig, "--auto-recording-playback-test", "auto-recording-playback-test", false);
    const bool autoRepoCatalogBrowseTest = getConfigBool(argc, argv, appConfig, "--auto-repo-catalog-browse-test", "auto-repo-catalog-browse-test", false);
    const bool autoParameterCacheTest = getConfigBool(argc, argv, appConfig, "--auto-parameter-cache-test", "auto-parameter-cache-test", false);
    const bool autoParameterEditTest = getConfigBool(argc, argv, appConfig, "--auto-parameter-edit-test", "auto-parameter-edit-test", false);
    const bool autoPreflightChecklistTest = getConfigBool(argc, argv, appConfig, "--auto-preflight-checklist-test", "auto-preflight-checklist-test", false);
    const bool autoAnalyzeSnapshotTest = getConfigBool(argc, argv, appConfig, "--auto-analyze-snapshot-test", "auto-analyze-snapshot-test", false);
    const bool autoDashboardSnapshotTest = getConfigBool(argc, argv, appConfig, "--auto-dashboard-snapshot-test", "auto-dashboard-snapshot-test", false);
    const bool autoDashboardPanelTest = getConfigBool(argc, argv, appConfig, "--auto-dashboard-panel-test", "auto-dashboard-panel-test", false);
    const bool autoDashboardDetailPanelTest = getConfigBool(argc, argv, appConfig, "--auto-dashboard-detail-panel-test", "auto-dashboard-detail-panel-test", false);
    const bool autoDashboardRefreshButtonsTest = getConfigBool(argc, argv, appConfig, "--auto-dashboard-refresh-buttons-test", "auto-dashboard-refresh-buttons-test", false);
    const bool autoParameterEditPanelTest = getConfigBool(argc, argv, appConfig, "--auto-parameter-edit-panel-test", "auto-parameter-edit-panel-test", false);
    const bool autoAuthorityLeaseTest = getConfigBool(argc, argv, appConfig, "--auto-authority-lease-test", "auto-authority-lease-test", false);
    const bool autoAuthorityConfigTest = getConfigBool(argc, argv, appConfig, "--auto-authority-config-test", "auto-authority-config-test", false);
    const bool autoAuthorityIssuerTest = getConfigBool(argc, argv, appConfig, "--auto-authority-issuer-test", "auto-authority-issuer-test", false);
    const bool autoAuthorityArbitrationTest = getConfigBool(argc, argv, appConfig, "--auto-authority-arbitration-test", "auto-authority-arbitration-test", false);
    const bool autoAuthorityPersistenceTest = getConfigBool(argc, argv, appConfig, "--auto-authority-persistence-test", "auto-authority-persistence-test", false);
    const bool autoAuthorityRevocationTest = getConfigBool(argc, argv, appConfig, "--auto-authority-revocation-test", "auto-authority-revocation-test", false);
    const bool autoAuthorityRefreshTest = getConfigBool(argc, argv, appConfig, "--auto-authority-refresh-test", "auto-authority-refresh-test", false);
    const bool autoAuthorityRefreshTimerTest = getConfigBool(argc, argv, appConfig, "--auto-authority-refresh-timer-test", "auto-authority-refresh-timer-test", false);
    const bool autoAuthorityAlertHistoryTest = getConfigBool(argc, argv, appConfig, "--auto-authority-alert-history-test", "auto-authority-alert-history-test", false);
    const bool autoAuthorityAuditQueryTest = getConfigBool(argc, argv, appConfig, "--auto-authority-audit-query-test", "auto-authority-audit-query-test", false);
    const bool autoApplyBitrateTest = getConfigBool(argc, argv, appConfig, "--auto-apply-bitrate-test", "auto-apply-bitrate-test", false);
    const bool autoVideoPressureProfileTest = getConfigBool(argc, argv, appConfig, "--auto-video-pressure-profile-test", "auto-video-pressure-profile-test", false);
    const bool autoPatrolTest = getConfigBool(argc, argv, appConfig, "--auto-patrol-test", "auto-patrol-test", false);
    const bool autoSingleMissionTest = getConfigBool(argc, argv, appConfig, "--auto-single-mission-test", "auto-single-mission-test", false);
    const bool autoSingleMissionStartTest = getConfigBool(argc, argv, appConfig, "--auto-single-mission-start-test", "auto-single-mission-start-test", false);
    const bool autoLoadedMissionPlanTest = getConfigBool(argc, argv, appConfig, "--auto-loaded-mission-plan-test", "auto-loaded-mission-plan-test", false);
    const bool autoRepeatStopTest = getConfigBool(argc, argv, appConfig, "--auto-repeat-stop-test", "auto-repeat-stop-test", false);
    const bool autoIncidentCollaborationTest = getConfigBool(
      argc, argv, appConfig, "--auto-incident-collaboration-test",
      "auto-incident-collaboration-test", false);
    const bool autoSpec176SitlTest = getConfigBool(
      argc, argv, appConfig, "--auto-spec176-sitl-test",
      "auto-spec176-sitl-test", false);
    const int incidentCollaborationTimeoutSeconds = std::stoi(getConfigOption(
      argc, argv, appConfig, "--incident-collaboration-timeout-seconds",
      "incident-collaboration-timeout-seconds", "30"));
    const std::string spec176FailureCase = getConfigOption(
      argc, argv, appConfig, "--spec176-failure-case", "spec176-failure-case", "");
    const int autoStopSeconds = std::stoi(getConfigOption(argc, argv, appConfig, "--auto-stop-seconds", "auto-stop-seconds", "10"));
    const int autoStartDelayMs = std::stoi(getConfigOption(argc, argv, appConfig, "--auto-start-delay-ms", "auto-start-delay-ms", "3000"));
    const std::string targetDroneId = getConfigOption(argc, argv, appConfig, "--target-drone", "target-drone", "A");
    auto patrolDroneIds = splitCsv(getConfigOption(argc, argv, appConfig, "--patrol-drones", "patrol-drones", targetDroneId));
    const int ackTimeoutMs = std::stoi(getConfigOption(argc, argv, appConfig, "--ack-timeout-ms", "ack-timeout-ms", "500"));
    const int timeoutMs = std::stoi(getConfigOption(argc, argv, appConfig, "--timeout-ms", "timeout-ms", "10000"));
    const auto videoBitrateKbps = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--video-bitrate-kbps", "video-bitrate-kbps", "8000")));
    const auto videoFps = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig,
                                  "--video-fps", "video-fps", "30")));
    if (videoFps < 1 || videoFps > 60) {
      throw std::invalid_argument("--video-fps must be between 1 and 60");
    }
    const auto videoFrameWidth = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--video-width", "video-width", "480")));
    const auto videoFecParityShards = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig,
                                  "--video-fec-parity-shards",
                                  "video-fec-parity-shards", "1")));
    if (videoFecParityShards > 1) {
      throw std::invalid_argument("--video-fec-parity-shards must be 0 or 1");
    }
    const std::string liveStreamPrefetchPolicy = getConfigOption(
      argc, argv, appConfig, "--live-stream-prefetch-policy",
      "live-stream-prefetch-policy", "mapped-pressure");
    const std::string yoloModel = getConfigOption(argc, argv, appConfig, "--yolo-model", "yolo-model", "yolo26n.pt");
    const std::string yoloScript = getConfigOption(
      argc, argv, appConfig, "--yolo-script", "yolo-script",
      "NDNSF-UAV-APP/tools/yolo_detect_once.py");
    const std::string yoloWorkerScript = getConfigOption(
      argc, argv, appConfig, "--yolo-worker-script", "yolo-worker-script",
      "NDNSF-UAV-APP/tools/yolo_detect_worker.py");
    const auto linkStaleMs = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--link-stale-ms", "link-stale-ms", "3500")));
    const auto linkLostMs = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--link-lost-ms", "link-lost-ms", "8000")));
    const std::string lostLinkAction = getConfigOption(
      argc, argv, appConfig, "--lost-link-action", "lost-link-action", "notify");
    const std::string videoBitratePolicy = getConfigOption(
      argc, argv, appConfig, "--video-bitrate-policy", "video-bitrate-policy", "manual");
    const auto videoBitrateAutoPressureMs = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig,
                                  "--video-bitrate-auto-pressure-ms",
                                  "video-bitrate-auto-pressure-ms", "2500")));
    const std::string missionPlanFile = getConfigOption(
      argc, argv, appConfig, "--mission-plan-file", "mission-plan-file", "");
    UavRuntimeConfig config = loadUavRuntimeConfig(
      getConfigOption(argc, argv, appConfig, "--runtime-config", "runtime-config",
                      "NDNSF-UAV-APP/configs/uav_runtime.conf"));
    config.groupPrefix = ndn::Name(getConfigOption(argc, argv, appConfig, "--group-prefix", "group-prefix", config.groupPrefix.toUri()));
    config.controllerPrefix = ndn::Name(getConfigOption(argc, argv, appConfig, "--controller-prefix", "controller-prefix", config.controllerPrefix.toUri()));
  config.groundStationIdentity = ndn::Name(getConfigOption(argc, argv, appConfig, "--ground-station-identity", "ground-station-identity", config.groundStationIdentity.toUri()));
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
  config.serviceGsOperatorAuthorityLease = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-operator-authority-lease", "service-gs-operator-authority-lease", config.serviceGsOperatorAuthorityLease.toUri()));
  config.serviceGsOperatorAuthorityRevocation = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-operator-authority-revocation", "service-gs-operator-authority-revocation", config.serviceGsOperatorAuthorityRevocation.toUri()));
  config.serviceGsOperatorAuthorityAudit = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-operator-authority-audit", "service-gs-operator-authority-audit", config.serviceGsOperatorAuthorityAudit.toUri()));
  const auto groundStationMapLatText =
    getConfigOption(argc, argv, appConfig, "--ground-station-map-lat", "ground-station-map-lat", std::to_string(config.groundStationMapLat));
  const auto groundStationMapLonText =
    getConfigOption(argc, argv, appConfig, "--ground-station-map-lon", "ground-station-map-lon", std::to_string(config.groundStationMapLon));
  try {
    config.groundStationMapLat = std::stod(groundStationMapLatText);
  }
  catch (const std::exception&) {
    config.groundStationMapLat = std::numeric_limits<double>::quiet_NaN();
  }
  try {
    config.groundStationMapLon = std::stod(groundStationMapLonText);
  }
  catch (const std::exception&) {
    config.groundStationMapLon = std::numeric_limits<double>::quiet_NaN();
  }

  if (std::isnan(config.groundStationMapLat)) {
    config.groundStationMapLat = DEFAULT_GS_MAP_LAT;
  }
  if (std::isnan(config.groundStationMapLon)) {
    config.groundStationMapLon = DEFAULT_GS_MAP_LON;
  }
    const std::string operatorId = getConfigOption(
      argc, argv, appConfig, "--operator-id", "operator-id",
      config.groundStationIdentity.toUri());
    const std::string operatorLeaseDrone = getConfigOption(
      argc, argv, appConfig, "--operator-lease-drone", "operator-lease-drone", "all");
    const std::string operatorLeaseScope = getConfigOption(
      argc, argv, appConfig, "--operator-lease-scope", "operator-lease-scope", "control");
    const auto operatorLeaseTtlMs = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig,
                                  "--operator-lease-ttl-ms",
                                  "operator-lease-ttl-ms", "0")));
    const std::string operatorAuthorityStateFile = getConfigOption(
      argc, argv, appConfig, "--operator-authority-state-file",
      "operator-authority-state-file", "");
    const std::string operatorAdminIds = getConfigOption(
      argc, argv, appConfig, "--operator-admin-ids", "operator-admin-ids", "");
    const auto operatorAuthorityRefreshIntervalMs = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig,
                                  "--operator-authority-refresh-interval-ms",
                                  "operator-authority-refresh-interval-ms", "0")));
    const bool trackingHeadless = hasFlag(argc, argv, "--headless");
    const bool trackingAutomatic = hasFlag(argc, argv, "--tracking-auto-start");
    std::vector<TrackingJobDefinition> trackingDefinitions;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--tracking-job-config") {
        if (++i == argc || trackingDefinitions.size() >= 16) {
          throw std::invalid_argument("tracking job configurations require 1..16 paths");
        }
        trackingDefinitions.push_back(loadTrackingJobDefinition(argv[i]));
      }
    }
    if (trackingAutomatic && trackingDefinitions.empty()) {
      throw std::invalid_argument("tracking auto-start requires prepared job configurations");
    }
    if (trackingHeadless && !trackingAutomatic) {
      throw std::invalid_argument("headless ground-station requires tracking auto-start");
    }
    Glib::RefPtr<Gtk::Application> app;
    if (!trackingHeadless) {
      app = Gtk::Application::create("org.ndnsf.uav.gs", Gio::APPLICATION_NON_UNIQUE);
      gtkInitialized = true;
    }

    if (objectDetectionMode) {
      ndn::Face faceForObjectDetection;
      KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
      ndn::KeyChain keyChain;
      auto gsCert = getOrCreateIdentity(keyChain, config.groundStationIdentity);
      auto controllerCert = getOrCreateIdentity(keyChain, config.controllerPrefix);
      keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(config.groundStationIdentity));
      return serveObjectDetection(faceForObjectDetection, keyChain, gsCert, controllerCert, config,
                                  serveCertificates);
    }

    const bool interactiveGui = !(autoStart || autoMavlinkTest || autoTelemetryTest || autoKeyboardTest ||
                                  autoManualControlTest || autoTwoDroneSwitchTest ||
                                  autoLinkStateTest || autoVideoSelectionTest ||
                                  autoMissionControlsTest ||
                                  autoFlightControlsTest ||
                                  autoRecordingPlaybackTest ||
                                  autoRepoCatalogBrowseTest ||
                                  autoParameterCacheTest ||
                                  autoParameterEditTest ||
                                  autoPreflightChecklistTest ||
                                  autoAnalyzeSnapshotTest ||
                                  autoDashboardSnapshotTest ||
                                  autoDashboardPanelTest ||
                                  autoDashboardDetailPanelTest ||
                                  autoDashboardRefreshButtonsTest ||
                                  autoParameterEditPanelTest ||
                                  autoAuthorityLeaseTest ||
                                  autoAuthorityConfigTest ||
                                  autoAuthorityIssuerTest ||
                                  autoAuthorityArbitrationTest ||
                                  autoAuthorityPersistenceTest ||
                                  autoAuthorityRevocationTest ||
                                  autoAuthorityRefreshTest ||
                                  autoAuthorityRefreshTimerTest ||
                                  autoAuthorityAlertHistoryTest ||
                                  autoAuthorityAuditQueryTest ||
                                  autoApplyBitrateTest ||
                                  autoPatrolTest || autoSingleMissionTest ||
                                  autoSpec176SitlTest ||
                                  autoLoadedMissionPlanTest);
    if (interactiveGui && !trackingHeadless && !hasFlag(argc, argv, "--no-cert-dialog") &&
        !hasOption(argc, argv, "--ground-station-identity")) {
      config.groundStationIdentity = chooseGroundStationIdentity(config.groundStationIdentity);
    }

    auto runtime = std::make_unique<GroundStationServiceContainer>(
      serveCertificates, ackTimeoutMs, timeoutMs, config, targetDroneId,
      videoBitrateKbps, videoFps, videoFrameWidth, videoFecParityShards,
      liveStreamPrefetchPolicy,
      patrolDroneIds, yoloModel, yoloScript,
      yoloWorkerScript, linkStaleMs, linkLostMs, lostLinkAction,
      videoBitratePolicy, videoBitrateAutoPressureMs, missionPlanFile,
      operatorId, operatorLeaseDrone, operatorLeaseScope, operatorLeaseTtlMs,
      operatorAuthorityStateFile, operatorAdminIds, operatorAuthorityRefreshIntervalMs);
    runtime->setStatusCallback([] (std::string status) {
      std::cerr << "GS_RUNTIME_STATUS status=" << status << std::endl;
    });
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("ground-station NDNSF runtime did not become ready");
    }
    GroundStationTrackingControls trackingControls(*runtime, std::move(trackingDefinitions),
                                                   trackingAutomatic);
    if (!trackingControls.definitions().empty()) {
      std::signal(SIGINT, onTrackingSignal);
      std::signal(SIGTERM, onTrackingSignal);
    }
    if (trackingHeadless) {
      while (!g_trackingStop && !trackingControls.finished()) {
        trackingControls.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      const bool drained = runtime->shutdownRuntime();
      const bool success = trackingControls.finalize(drained);
      if (!drained) std::_Exit(1);
      return success ? 0 : 1;
    }
    if (autoPatrolTest) {
      const bool ok = runtime->runAutoPatrolCompensationDemo(std::chrono::seconds(30));
      std::cout << "GS_PATROL_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoSingleMissionTest) {
      const bool ok = runtime->runSingleDroneMissionUploadTest(std::chrono::seconds(45),
                                                               autoSingleMissionStartTest);
      std::cout << "GS_SINGLE_MISSION_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoLoadedMissionPlanTest) {
      const bool ok = runtime->runLoadedMissionPlanUploadTest(std::chrono::seconds(45),
                                                              missionPlanFile);
      std::cout << "GS_LOADED_MISSION_PLAN_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoIncidentCollaborationTest) {
      const bool ok = runtime->runIncidentCollaborationTest(
        std::chrono::seconds(std::max(1, incidentCollaborationTimeoutSeconds)),
        spec176FailureCase);
      std::cout << "GS_INCIDENT_COLLABORATION_EXIT ok="
                << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoSpec176SitlTest) {
      const bool ok = runtime->runSpec176SitlAcceptance(
        std::chrono::seconds(std::max(1, incidentCollaborationTimeoutSeconds)));
      std::cout << "GS_SPEC176_SITL_EXIT ok="
                << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoRepoCatalogBrowseTest) {
      const bool ok = runtime->runRepoCatalogBrowseTest(std::chrono::seconds(30));
      std::cout << "GS_REPO_CATALOG_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoParameterCacheTest) {
      const bool ok = runtime->runParameterCacheTest(std::chrono::seconds(30));
      std::cout << "GS_PARAMETER_CACHE_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoParameterEditTest) {
      const bool ok = runtime->runParameterEditTest(std::chrono::seconds(30));
      std::cout << "GS_PARAMETER_EDIT_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoPreflightChecklistTest) {
      const bool ok = runtime->runPreflightChecklistTest(std::chrono::seconds(30));
      std::cout << "GS_PREFLIGHT_CHECKLIST_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAnalyzeSnapshotTest) {
      const bool ok = runtime->runAnalyzeSnapshotTest(std::chrono::seconds(30));
      std::cout << "GS_ANALYZE_SNAPSHOT_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoDashboardSnapshotTest) {
      const bool ok = runtime->runOperatorDashboardSnapshotTest(std::chrono::seconds(30));
      std::cout << "GS_DASHBOARD_SNAPSHOT_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityLeaseTest) {
      const bool ok = runtime->runAuthorityLeaseGateTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_LEASE_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityConfigTest) {
      const bool ok = runtime->runConfiguredAuthorityLeaseTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_CONFIG_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityIssuerTest) {
      const bool ok = runtime->runAuthorityLeaseIssuerTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_ISSUER_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityArbitrationTest) {
      const bool ok = runtime->runAuthorityLeaseArbitrationTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_ARBITRATION_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityPersistenceTest) {
      const bool ok = runtime->runAuthorityLeasePersistenceTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_PERSISTENCE_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityRevocationTest) {
      const bool ok = runtime->runAuthorityRevocationLookupTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_REVOCATION_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityRefreshTest) {
      const bool ok = runtime->runAuthorityLeaseRefreshTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_REFRESH_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityRefreshTimerTest) {
      const bool ok = runtime->runAuthorityLeaseRefreshTimerTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_REFRESH_TIMER_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityAlertHistoryTest) {
      const bool ok = runtime->runAuthorityAlertHistoryTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_ALERT_HISTORY_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoAuthorityAuditQueryTest) {
      const bool ok = runtime->runAuthorityAuditQueryTest(std::chrono::seconds(10));
      std::cout << "GS_AUTHORITY_AUDIT_QUERY_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoTelemetryTest) {
      const bool ok = runtime->runTelemetryLiveTest(std::chrono::seconds(45),
                                                    !autoTelemetryAllowMockFields);
      std::cout << "GS_TELEMETRY_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    if (autoLinkStateTest) {
      const bool ok = runtime->runLinkStateAgingTest(std::chrono::seconds(15));
      std::cout << "GS_LINK_STATE_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
    }
    GroundStationWindow window(*runtime, autoStart, autoStopSeconds,
                               autoStartDelayMs, autoMavlinkTest,
                               autoKeyboardTest, autoManualControlTest,
                               autoTwoDroneSwitchTest,
                               autoVideoSelectionTest,
                               autoMissionControlsTest,
                               autoFlightControlsTest,
                               autoRecordingPlaybackTest,
                               autoDashboardPanelTest,
                               autoDashboardDetailPanelTest,
                               autoDashboardRefreshButtonsTest,
                               autoParameterEditPanelTest,
                               autoApplyBitrateTest,
                               autoVideoPressureProfileTest,
                               autoRepeatStopTest,
                               patrolDroneIds,
                               config.groundStationMapLat,
                               config.groundStationMapLon,
                               operatorAuthorityRefreshIntervalMs,
                               &trackingControls,
                               [] { return g_trackingStop != 0; });
    NDN_LOG_INFO("UavGroundStationApp GUI ready");
    std::cout << "GS_GUI_READY target_drone=" << targetDroneId
              << " auto_video_test=" << (autoStart ? "true" : "false")
              << " auto_mavlink_test=" << (autoMavlinkTest ? "true" : "false")
              << " auto_telemetry_test=" << (autoTelemetryTest ? "true" : "false")
              << " auto_link_state_test=" << (autoLinkStateTest ? "true" : "false")
              << " auto_keyboard_test=" << (autoKeyboardTest ? "true" : "false")
              << " auto_manual_control_test=" << (autoManualControlTest ? "true" : "false")
              << " auto_two_drone_switch_test=" << (autoTwoDroneSwitchTest ? "true" : "false")
              << " auto_video_selection_test=" << (autoVideoSelectionTest ? "true" : "false")
              << " auto_mission_controls_test=" << (autoMissionControlsTest ? "true" : "false")
              << " auto_flight_controls_test=" << (autoFlightControlsTest ? "true" : "false")
              << " auto_recording_playback_test=" << (autoRecordingPlaybackTest ? "true" : "false")
              << " auto_repo_catalog_browse_test=" << (autoRepoCatalogBrowseTest ? "true" : "false")
              << " auto_parameter_cache_test=" << (autoParameterCacheTest ? "true" : "false")
              << " auto_parameter_edit_test=" << (autoParameterEditTest ? "true" : "false")
              << " auto_preflight_checklist_test=" << (autoPreflightChecklistTest ? "true" : "false")
              << " auto_analyze_snapshot_test=" << (autoAnalyzeSnapshotTest ? "true" : "false")
              << " auto_dashboard_snapshot_test=" << (autoDashboardSnapshotTest ? "true" : "false")
              << " auto_dashboard_panel_test=" << (autoDashboardPanelTest ? "true" : "false")
              << " auto_dashboard_detail_panel_test=" << (autoDashboardDetailPanelTest ? "true" : "false")
              << " auto_dashboard_refresh_buttons_test=" << (autoDashboardRefreshButtonsTest ? "true" : "false")
              << " auto_parameter_edit_panel_test=" << (autoParameterEditPanelTest ? "true" : "false")
              << " auto_authority_lease_test=" << (autoAuthorityLeaseTest ? "true" : "false")
              << " auto_authority_config_test=" << (autoAuthorityConfigTest ? "true" : "false")
              << " auto_authority_issuer_test=" << (autoAuthorityIssuerTest ? "true" : "false")
              << " auto_authority_arbitration_test=" << (autoAuthorityArbitrationTest ? "true" : "false")
              << " auto_authority_persistence_test=" << (autoAuthorityPersistenceTest ? "true" : "false")
              << " auto_authority_revocation_test=" << (autoAuthorityRevocationTest ? "true" : "false")
              << " auto_authority_refresh_test=" << (autoAuthorityRefreshTest ? "true" : "false")
              << " auto_authority_refresh_timer_test=" << (autoAuthorityRefreshTimerTest ? "true" : "false")
              << " auto_authority_alert_history_test=" << (autoAuthorityAlertHistoryTest ? "true" : "false")
              << " auto_authority_audit_query_test=" << (autoAuthorityAuditQueryTest ? "true" : "false")
              << " operator_authority_refresh_interval_ms=" << operatorAuthorityRefreshIntervalMs
              << " auto_apply_bitrate_test=" << (autoApplyBitrateTest ? "true" : "false")
              << " auto_video_pressure_profile_test=" << (autoVideoPressureProfileTest ? "true" : "false")
              << " video_fps=" << videoFps
              << " video_bitrate_policy=" << videoBitratePolicy
              << " live_stream_prefetch_policy=" << liveStreamPrefetchPolicy
              << " operator_id=" << operatorId
              << " operator_lease_drone=" << operatorLeaseDrone
              << " operator_lease_scope=" << operatorLeaseScope
              << " operator_lease_ttl_ms=" << operatorLeaseTtlMs
              << " operator_authority_state_file=" << operatorAuthorityStateFile
              << std::endl;
    const int rc = app->run(window);
    const bool drained = runtime->shutdownRuntime();
    const bool trackingSuccess = trackingControls.finalize(drained);
    if (!drained) {
      std::cerr << "GS_GUI_EXIT cleanup=INCOMPLETE" << std::endl;
      std::_Exit(1);
    }
    std::cout << "GS_GUI_EXIT rc=" << rc << std::endl;
    return trackingAutomatic && !trackingSuccess ? 1 : rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavGroundStationApp error: " << e.what() << std::endl;
    return 1;
  }
}

} // namespace

int
main(int argc, char** argv)
{
  bool gtkInitialized = false;
  const int result = runGroundStationApp(argc, argv, gtkInitialized);
  if (gtkInitialized && !ndnsf::examples::uav::finishUavGtkRuntime()) {
    std::cerr << "GS_GUI_EXIT font_cleanup=INCOMPLETE" << std::endl;
    return 1;
  }
  return result;
}
