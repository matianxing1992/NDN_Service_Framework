#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <gtkmm.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.examples.UavDroneApp);

using namespace ndnsf::examples::uav;
using namespace std::chrono_literals;

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

ndn::Buffer
bufferFromString(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::vector<std::pair<std::string, std::string>>
parseWaypointPairs(const std::string& waypoints)
{
  std::vector<std::pair<std::string, std::string>> out;
  const auto colon = waypoints.find(':');
  std::string body = colon == std::string::npos ? waypoints : waypoints.substr(colon + 1);
  std::stringstream ss(body);
  std::string item;
  while (std::getline(ss, item, '>')) {
    const auto comma = item.find(',');
    if (comma == std::string::npos) {
      continue;
    }
    out.emplace_back(item.substr(0, comma), item.substr(comma + 1));
  }
  return out;
}

std::string
payloadToString(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
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

#include "DroneServiceContainer.inc.hpp"

#include "DroneWindow.inc.hpp"

} // namespace

int
main(int argc, char** argv)
{
  try {
    const auto appConfig = loadKeyValueConfig(getOption(argc, argv, "--app-config", ""));
    const std::string droneId = getConfigOption(argc, argv, appConfig, "--drone-id", "drone-id", "A");
    const bool available = getConfigBoolInvertedFlag(
      argc, argv, appConfig, "--unavailable", "available", true);
    const bool serveCertificates = getConfigBoolInvertedFlag(
      argc, argv, appConfig, "--no-serve-certificates", "serve-certificates", true);
    const std::string videoPath = getConfigOption(
      argc, argv, appConfig, "--video-source", "video-source",
      "NDNSF-UAV-APP/videos/drone.mp4");
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
    cameraOptions.recordChunkLimit = std::stoull(getConfigOption(
      argc, argv, appConfig, "--camera-record-chunk-limit",
      "camera-record-chunk-limit", "0"));
    const bool autoCameraRecordSmoke = getConfigBool(
      argc, argv, appConfig, "--auto-camera-record-smoke",
      "auto-camera-record-smoke", false);
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
    config.serviceGsObjectDetection = ndn::Name(getConfigOption(argc, argv, appConfig, "--service-gs-object-detection", "service-gs-object-detection", config.serviceGsObjectDetection.toUri()));

    if (autoCameraRecordSmoke) {
      cameraOptions.captureOnStart = true;
      cameraOptions.recordToLocalRepo = true;
      if (cameraOptions.recordChunkLimit < autoCameraRecordExpectedChunks + 100) {
        cameraOptions.recordChunkLimit = autoCameraRecordExpectedChunks + 100;
      }
      if (cameraOptions.recordRepoPath.empty()) {
        cameraOptions.recordRepoPath = "/tmp/ndnsf-uav-camera-record-smoke.sqlite3";
      }

      ndn::Face smokeFace;
      ndn::KeyChain smokeKeyChain;
      VideoPublisher smokePublisher(
        smokeFace, smokeKeyChain, config, droneId, videoPath, cameraOptions);

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
      if (smokePublisher.recordingChunks() < targetChunks ||
          smokePublisher.recordingBytes() == 0 ||
          smokePublisher.recordingChunks() <= chunksAfterStop) {
        std::cerr << "DRONE_CAMERA_RECORD_SMOKE_FAILED chunks="
                  << smokePublisher.recordingChunks()
                  << " bytes=" << smokePublisher.recordingBytes()
                  << " chunks_after_stop=" << chunksAfterStop
                  << " repo=" << cameraOptions.recordRepoPath << std::endl;
        return 1;
      }
      std::cout << "DRONE_CAMERA_RECORD_SMOKE_OK chunks="
                << smokePublisher.recordingChunks()
                << " bytes=" << smokePublisher.recordingBytes()
                << " chunks_after_stop=" << chunksAfterStop
                << " repo=" << cameraOptions.recordRepoPath
                << " prefix=" << smokePublisher.recordingPrefix()
                << std::endl;
      return 0;
    }

    auto runtime = std::make_unique<DroneServiceContainer>(
      droneId, available, serveCertificates, config, videoPath,
      flightControllerBackend, mavlinkUdpHost, mavlinkUdpPort, mavlinkUdpListenPort,
      mavlinkSerialDevice, mavlinkSerialBaud, configurePx4SitlDemoParams,
      std::move(cameraOptions));
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("drone NDNSF runtime did not become ready");
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
    std::cout << "DRONE_GUI_EXIT rc=" << rc << std::endl;
    return rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavDroneApp error: " << e.what() << std::endl;
    return 1;
  }
}
