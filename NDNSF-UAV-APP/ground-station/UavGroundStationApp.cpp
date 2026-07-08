#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "GroundStationRuntimeState.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ServiceContainer.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/pixbufloader.h>
#include <gtkmm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/joystick.h>
#include <map>
#include <pwd.h>
#include <signal.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <set>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.examples.UavGroundStationApp);

using namespace ndnsf::examples::uav;
using namespace std::chrono_literals;

constexpr const char* PX4_SITL_TAKEOFF_AMSL_M = "505";

std::string
mavlinkTargetSystemForDrone(const std::string& droneId)
{
  if (droneId.size() == 1 && droneId[0] >= 'A' && droneId[0] <= 'Z') {
    return std::to_string(static_cast<int>(droneId[0] - 'A') + 1);
  }
  if (droneId.size() == 1 && droneId[0] >= 'a' && droneId[0] <= 'z') {
    return std::to_string(static_cast<int>(droneId[0] - 'a') + 1);
  }
  if (!droneId.empty() &&
      std::all_of(droneId.begin(), droneId.end(),
                  [] (char ch) { return ch >= '0' && ch <= '9'; })) {
    return droneId;
  }
  return "1";
}

std::string
findJoystickDevice()
{
  for (int i = 0; i < 8; ++i) {
    const auto path = "/dev/input/js" + std::to_string(i);
    if (access(path.c_str(), R_OK) == 0) {
      return path;
    }
  }
  return "";
}

int
scaleJoystickAxis(int value)
{
  constexpr int deadzone = 6000;
  if (std::abs(value) < deadzone) {
    return 0;
  }
  return std::clamp(static_cast<int>(std::lround(value * 1000.0 / 32767.0)),
                    -1000, 1000);
}

class KeyChainInitLock
{
public:
  explicit KeyChainInitLock(const char* path)
  {
    m_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0) {
      throw std::runtime_error("failed to acquire keychain lock");
    }
    if (flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
      close(m_fd);
      m_fd = -1;
    }
  }

  ~KeyChainInitLock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
    }
  }

private:
  int m_fd = -1;
};

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

std::string
getConfigOption(int argc, char** argv, const Fields& config,
                const std::string& option, const std::string& key,
                const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fieldOr(config, key, fallback);
}

std::string
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

std::string
pythonUserEnvironmentPrefix()
{
  const char* sudoUser = std::getenv("SUDO_USER");
  if (geteuid() != 0 || sudoUser == nullptr || *sudoUser == '\0') {
    return "";
  }

  if (auto* passwd = getpwnam(sudoUser)) {
    const std::string home = passwd->pw_dir;
    return "HOME=" + shellQuote(home) + " XDG_CACHE_HOME=" +
           shellQuote(home + "/.cache") + " ";
  }

  return "";
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

bool
getConfigBool(int argc, char** argv, const Fields& config,
              const std::string& trueFlag, const std::string& key,
              bool fallback)
{
  if (!trueFlag.empty() && hasFlag(argc, argv, trueFlag)) {
    return true;
  }
  const auto value = fieldOr(config, key, fallback ? "true" : "false");
  return value == "true" || value == "1" || value == "yes" || value == "on";
}

bool
getConfigBoolInvertedFlag(int argc, char** argv, const Fields& config,
                          const std::string& falseFlag, const std::string& key,
                          bool fallback)
{
  if (!falseFlag.empty() && hasFlag(argc, argv, falseFlag)) {
    return false;
  }
  return getConfigBool(argc, argv, config, "", key, fallback);
}

bool
hasOption(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

std::vector<ndn::Name>
listInstalledIdentities()
{
  std::vector<ndn::Name> identities;
  ndn::KeyChain keyChain;
  const auto& container = keyChain.getPib().getIdentities();
  for (auto it = container.begin(); it != container.end(); ++it) {
    identities.push_back((*it).getName());
  }
  std::sort(identities.begin(), identities.end());
  return identities;
}

ndn::Name
chooseGroundStationIdentity(const ndn::Name& configuredIdentity)
{
  const auto identities = listInstalledIdentities();
  if (identities.empty()) {
    return configuredIdentity;
  }

  Gtk::Dialog dialog("Select NDNSF ground-station certificate", true);
  dialog.set_default_size(520, 220);
  dialog.add_button("Use Selected Certificate", Gtk::RESPONSE_OK);
  dialog.add_button("Use Configured Name", Gtk::RESPONSE_CANCEL);

  auto* content = dialog.get_content_area();
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 10);
  box.set_border_width(14);
  Gtk::Label title;
  title.set_markup("<b>Choose the local identity used by this Ground Station</b>");
  title.set_xalign(0.0F);
  Gtk::Label hint("The selected identity must have a certificate trusted by the UAV deployment trust schema.");
  hint.set_line_wrap(true);
  hint.set_xalign(0.0F);
  Gtk::ComboBoxText combo;
  int active = 0;
  for (size_t i = 0; i < identities.size(); ++i) {
    const auto text = identities[i].toUri();
    combo.append(text);
    if (identities[i] == configuredIdentity) {
      active = static_cast<int>(i);
    }
  }
  combo.set_active(active);
  Gtk::Label configured("Configured fallback: " + configuredIdentity.toUri());
  configured.set_xalign(0.0F);
  box.pack_start(title, Gtk::PACK_SHRINK);
  box.pack_start(hint, Gtk::PACK_SHRINK);
  box.pack_start(combo, Gtk::PACK_SHRINK);
  box.pack_start(configured, Gtk::PACK_SHRINK);
  content->pack_start(box, Gtk::PACK_EXPAND_WIDGET);
  dialog.show_all_children();

  if (dialog.run() == Gtk::RESPONSE_OK) {
    const auto selected = combo.get_active_text();
    if (!selected.empty()) {
      return ndn::Name(selected);
    }
  }
  return configuredIdentity;
}

ndn::Buffer
bufferFromString(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

ndn_service_framework::RequestMessage
makeRequest(const std::string& payload, size_t strategy = ndn_service_framework::tlv::FirstResponding)
{
  auto requestPayload = bufferFromString(payload);
  ndn_service_framework::RequestMessage request;
  request.setPayload(requestPayload, requestPayload.size());
  request.setStrategy(strategy);
  return request;
}

std::string
responsePayload(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

std::vector<std::string>
splitCsv(const std::string& value)
{
  std::vector<std::string> out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

ndn_service_framework::ResponseMessage
makeResponse(bool status, const std::string& payload, const std::string& error = "No error")
{
  auto responsePayload = bufferFromString(payload);
  ndn_service_framework::ResponseMessage response;
  response.setStatus(status);
  response.setErrorInfo(error);
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

#include "GroundStationServiceContainer.inc.hpp"

#include "GroundStationWindow.inc.hpp"

} // namespace

int
main(int argc, char** argv)
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
    const bool autoApplyBitrateTest = getConfigBool(argc, argv, appConfig, "--auto-apply-bitrate-test", "auto-apply-bitrate-test", false);
    const bool autoVideoPressureProfileTest = getConfigBool(argc, argv, appConfig, "--auto-video-pressure-profile-test", "auto-video-pressure-profile-test", false);
    const bool autoPatrolTest = getConfigBool(argc, argv, appConfig, "--auto-patrol-test", "auto-patrol-test", false);
    const bool autoSingleMissionTest = getConfigBool(argc, argv, appConfig, "--auto-single-mission-test", "auto-single-mission-test", false);
    const bool autoSingleMissionStartTest = getConfigBool(argc, argv, appConfig, "--auto-single-mission-start-test", "auto-single-mission-start-test", false);
    const bool autoLoadedMissionPlanTest = getConfigBool(argc, argv, appConfig, "--auto-loaded-mission-plan-test", "auto-loaded-mission-plan-test", false);
    const bool autoRepeatStopTest = getConfigBool(argc, argv, appConfig, "--auto-repeat-stop-test", "auto-repeat-stop-test", false);
    const int autoStopSeconds = std::stoi(getConfigOption(argc, argv, appConfig, "--auto-stop-seconds", "auto-stop-seconds", "10"));
    const int autoStartDelayMs = std::stoi(getConfigOption(argc, argv, appConfig, "--auto-start-delay-ms", "auto-start-delay-ms", "3000"));
    const std::string targetDroneId = getConfigOption(argc, argv, appConfig, "--target-drone", "target-drone", "A");
    auto patrolDroneIds = splitCsv(getConfigOption(argc, argv, appConfig, "--patrol-drones", "patrol-drones", targetDroneId));
    const int ackTimeoutMs = std::stoi(getConfigOption(argc, argv, appConfig, "--ack-timeout-ms", "ack-timeout-ms", "500"));
    const int timeoutMs = std::stoi(getConfigOption(argc, argv, appConfig, "--timeout-ms", "timeout-ms", "10000"));
    const auto videoBitrateKbps = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--video-bitrate-kbps", "video-bitrate-kbps", "8000")));
    const auto videoFrameWidth = static_cast<uint64_t>(
      std::stoull(getConfigOption(argc, argv, appConfig, "--video-width", "video-width", "480")));
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
  config.serviceGsObjectDetection = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-object-detection", "service-gs-object-detection", config.serviceGsObjectDetection.toUri()));
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
    auto app = Gtk::Application::create("org.ndnsf.uav.gs", Gio::APPLICATION_NON_UNIQUE);

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
                                  autoApplyBitrateTest ||
                                  autoPatrolTest || autoSingleMissionTest ||
                                  autoLoadedMissionPlanTest);
    if (interactiveGui && !hasFlag(argc, argv, "--no-cert-dialog") &&
        !hasOption(argc, argv, "--ground-station-identity")) {
      config.groundStationIdentity = chooseGroundStationIdentity(config.groundStationIdentity);
    }

    auto runtime = std::make_unique<GroundStationServiceContainer>(
      serveCertificates, ackTimeoutMs, timeoutMs, config, targetDroneId,
      videoBitrateKbps, videoFrameWidth, patrolDroneIds, yoloModel, yoloScript,
      yoloWorkerScript, linkStaleMs, linkLostMs, lostLinkAction,
      videoBitratePolicy, videoBitrateAutoPressureMs, missionPlanFile);
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("ground-station NDNSF runtime did not become ready");
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
                               autoApplyBitrateTest,
                               autoVideoPressureProfileTest,
                               autoRepeatStopTest,
                               patrolDroneIds,
                               config.groundStationMapLat,
                               config.groundStationMapLon);
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
              << " auto_apply_bitrate_test=" << (autoApplyBitrateTest ? "true" : "false")
              << " auto_video_pressure_profile_test=" << (autoVideoPressureProfileTest ? "true" : "false")
              << " video_bitrate_policy=" << videoBitratePolicy
              << std::endl;
    const int rc = app->run(window);
    runtime->shutdownRuntime();
    std::cout << "GS_GUI_EXIT rc=" << rc << std::endl;
    return rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavGroundStationApp error: " << e.what() << std::endl;
    return 1;
  }
}
