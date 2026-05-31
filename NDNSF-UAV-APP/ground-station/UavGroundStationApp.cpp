#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

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
#include <csignal>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pwd.h>
#include <signal.h>
#include <memory>
#include <mutex>
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
    if (m_yoloPrewarmThread.joinable()) {
      m_yoloPrewarmThread.join();
    }
    stopYoloWorker();
    stopDecoder();
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
      SERVICE_TELEMETRY_STATUS,
      encodeFields({{"type", "telemetry-status"}, {"target_drone", droneId}}),
      [this, droneId](const std::string& payload) {
        m_telemetryInFlight = false;
        const auto fields = decodeFields(payload);
        publishStatus("Telemetry drone=" + fieldOr(fields, "drone_id", droneId) +
                      " alt=" + fieldOr(fields, "altitude_m", "unknown") + "m" +
                      " lat=" + fieldOr(fields, "lat", "unknown") +
                      " lon=" + fieldOr(fields, "lon", "unknown") +
                      " battery=" + fieldOr(fields, "battery_percent", "unknown") + "%" +
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

  std::string
  activeVideoDroneId() const
  {
    std::lock_guard<std::mutex> guard(m_videoStateMutex);
    return m_activeVideoDroneId;
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
      SERVICE_MAVLINK_EXECUTE,
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
      SERVICE_MAVLINK_EXECUTE,
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
      SERVICE_TELEMETRY_STATUS,
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
        SERVICE_MISSION_ASSIGN,
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
          SERVICE_MISSION_ASSIGN,
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
    m_objectDetectionProvider->addService(
      SERVICE_GS_OBJECT_DETECTION,
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
        [onSuccess, service](const ndn_service_framework::ResponseMessage& response) {
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_RESPONSE service=" << service << " payload=" << payloadText);
          onSuccess(payloadText);
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
        [onSuccess, service](const ndn_service_framework::ResponseMessage& response) {
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_TARGETED_RESPONSE service=" << service
                       << " payload=" << payloadText);
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
    requestVideoLane(m_deltaLane, m_deltaWindow);
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
    m_deltaWindow = std::clamp<uint64_t>(
      (m_deltaPacketsPerSecond + 2) / 3, 24, 48);

    NDN_LOG_INFO("GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
                 << " frameWidth=" << frameWidth
                 << " payloadBytes=" << payloadBytes
                 << " fps=" << fps
                 << " keyBudget=" << m_keyPacketsPerSecond
                 << " deltaBudget=" << m_deltaPacketsPerSecond
                 << " keyWindow=" << m_keyWindow
                 << " deltaWindow=" << m_deltaWindow);
    std::cout << "GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
              << " frameWidth=" << frameWidth
              << " keyBudget=" << m_keyPacketsPerSecond
              << " deltaBudget=" << m_deltaPacketsPerSecond
              << " deltaWindow=" << m_deltaWindow << std::endl;
  }

  void
  requestVideoLane(PacketLane& lane, uint64_t window)
  {
    advanceLaneIfStale(lane);
    while (m_streaming.load() && lane.inFlight < window) {
      advanceLaneIfStale(lane);
      const auto highWaterLimit = lane.advertisedPackets == 0 ?
        INITIAL_PACKET_PROBE :
        lane.advertisedPackets + VIDEO_PACKET_LOOKAHEAD;
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
        scheduleVideoPump(PROBE_RETRY_BACKOFF_MS);
        break;
      }
      const auto packetSeq = lane.nextSeq++;
      ++lane.inFlight;
      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packetSeq));
      auto interest = ndn::Interest(packetName);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(300_ms);

      m_face.expressInterest(
        interest,
        [this, &lane, packetSeq](const ndn::Interest&, const ndn::Data& data) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          lane.probeNotBeforeMs = 0;
          advanceLaneIfStale(lane);
        const auto receivedCount = ++m_receivedChunks;
        if (receivedCount <= 3 || receivedCount % 30 == 0) {
          NDN_LOG_INFO("GS_VIDEO_CHUNK count=" << receivedCount
                         << " packetSeq=" << packetSeq
                       << " name=" << data.getName()
                       << " bytes=" << data.getContent().value_size());
          std::cout << "GS_VIDEO_CHUNK count=" << receivedCount
                      << " packetSeq=" << packetSeq
                    << " bytes=" << data.getContent().value_size() << std::endl;
        }
        const auto receivedMs = nowMilliseconds();
        if (m_firstFrameMs == 0) {
          m_firstFrameMs = receivedMs;
        }
        const auto content = data.getContent();
        std::vector<uint8_t> bytes(content.value(), content.value() + content.value_size());
          try {
            const auto packet = decodeVideoPacket(bytes);
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
        const auto timeoutCount = ++m_frameTimeouts;
        if (timeoutCount <= 3 || timeoutCount % 30 == 0) {
            NDN_LOG_INFO("GS_VIDEO_TIMEOUT count=" << timeoutCount
                         << " packetSeq=" << packetSeq);
          std::cout << "GS_VIDEO_TIMEOUT count=" << timeoutCount
                      << " packetSeq=" << packetSeq << std::endl;
        }
          if (packetSeq >= lane.advertisedPackets &&
              lane.nextSeq > packetSeq) {
            lane.nextSeq = packetSeq;
            lane.probeNotBeforeMs = nowMilliseconds() + PROBE_RETRY_BACKOFF_MS;
          }
          advanceLaneIfStale(lane);
          scheduleVideoPump(PROBE_RETRY_BACKOFF_MS);
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
          NDN_LOG_WARN("GS_VIDEO_SKIP_MISSING_CHUNKS start="
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
      "-analyzeduration 0 -probesize 32 -f h264 -i pipe:0 -f image2pipe -vcodec mjpeg -";

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
  stopDecoder()
  {
    m_decoderRunning = false;
    m_decoderQueueCv.notify_all();

    if (m_decoderInFd >= 0) {
      shutdown(m_decoderInFd, SHUT_WR);
      close(m_decoderInFd);
      m_decoderInFd = -1;
    }
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
    return nowMs >= m_decoderMissingChunkStartMs + m_decoderMissingTimeoutMs;
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
        now >= m_decoderMissingChunkStartMs + m_decoderMissingTimeoutMs) {
      NDN_LOG_WARN("GS_VIDEO_SKIP_MISSING_CHUNKS_TIMEOUT start=" << m_decoderMissingChunkSeq
                     << " to=" << first->first - 1
                     << " timeoutMs=" << m_decoderMissingTimeoutMs
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
  std::vector<std::string> m_missionReadyDrones;
  std::string m_activeVideoDroneId;
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
  std::atomic<uint64_t> m_videoStartRetries{0};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
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
  static constexpr uint64_t INITIAL_PACKET_PROBE = 8;
  static constexpr uint64_t VIDEO_PACKET_LOOKAHEAD = 12;
  static constexpr uint64_t PROBE_RETRY_BACKOFF_MS = 80;
  static constexpr uint64_t STREAM_PUMP_INTERVAL_MS = 25;
  static constexpr uint64_t m_decoderReorderWindow = 12;
  static constexpr uint64_t m_decoderMissingTimeoutMs = 80;
  static constexpr uint64_t MAX_VIDEO_START_RETRIES = 2;
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_videoPumpScheduled{false};
  std::mutex m_decoderQueueMutex;
  std::condition_variable m_decoderQueueCv;
  std::deque<StreamChunk> m_chunkQueue;
  std::map<uint64_t, StreamChunk> m_pendingChunks;
  std::map<uint64_t, FecFrameState> m_fecFrames;
  std::vector<uint8_t> m_decoderOutBuffer;
  std::thread m_decoderWriterThread;
  std::thread m_decoderReaderThread;
  std::atomic<bool> m_decoderRunning{false};
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
    SERVICE_GS_OBJECT_DETECTION,
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
      }));
  provider.init();
  provider.fetchPermissionsFromController(config.controllerPrefix);
  NDN_LOG_INFO("UavGroundStationApp object detection service ready");
  face.processEvents();
  return 0;
}

class GroundStationWindow : public Gtk::Window
{
public:
  GroundStationWindow(GroundStationServiceContainer& runtime, bool autoStart,
                      int autoStopSeconds, int autoStartDelayMs,
                      bool autoMavlinkTest, bool autoKeyboardTest,
                      bool autoManualControlTest,
                      bool autoTwoDroneSwitchTest,
                      std::vector<std::string> droneIds)
    : m_runtime(runtime)
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
    , m_buttons(Gtk::ORIENTATION_HORIZONTAL, 8)
    , m_patrolControls(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_workspace(Gtk::ORIENTATION_HORIZONTAL, 10)
    , m_vehicleFrame("Vehicles")
    , m_vehiclePanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_centerFrame("Fly View")
    , m_centerPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_mapControls(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_statusFrame("Inspector")
    , m_statusPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_start("Start Video")
    , m_stop("Stop Video")
    , m_arm("Arm")
    , m_takeoff("Takeoff")
    , m_land("Land")
    , m_patrol("Upload Patrol Mission")
    , m_startMission("Start Mission")
    , m_stopPatrol("Stop Patrol")
    , m_controlToggle("Start Control")
    , m_mapZoomIn("+")
    , m_mapZoomOut("-")
    , m_mapCenterGs("Center GS")
    , m_mapUndoWp("Undo WP")
    , m_mapClearWp("Clear WPs")
    , m_controlPanel(Gtk::ORIENTATION_VERTICAL, 6)
    , m_manualKeyRow(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_commandKeyRow(Gtk::ORIENTATION_HORIZONTAL, 6)
    , m_keyW("W  Forward")
    , m_keyA("A  Yaw Left")
    , m_keyS("S  Back")
    , m_keyD("D  Yaw Right")
    , m_keyQ("Q  Roll Left")
    , m_keyE("E  Roll Right")
    , m_keyR("R  Throttle Up")
    , m_keyF("F  Throttle Down")
    , m_keyI("I  Arm")
    , m_keyT("T  Takeoff")
    , m_keyL("L  Land")
    , m_keyV("V  Video")
    , m_keyX("X  Stop Video")
    , m_droneIds(std::move(droneIds))
  {
    set_title("NDNSF UAV Ground Station");
    set_default_size(1180, 740);
    set_border_width(12);
    set_can_focus(true);
    if (m_droneIds.empty()) {
      m_droneIds.push_back("A");
    }

    m_status.set_text("Video stopped");
    m_stats.set_text("Frames: 0");
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "GS center: University of Memphis\n"
                          "Selected drone: " + m_runtime.targetDroneId() + "\n"
                          "Markers: GS, drone A/B, and mission waypoints\n"
                          "Click to append WP1/WP2/..., drag to pan, Center GS to return.\n"
                          "Upload Patrol Mission sends the route; arm/takeoff/mission mode makes PX4 fly it.");
    m_services.set_text("Services: video, targeted MAVLink, telemetry, camera, mission");
    m_telemetry.set_text("Telemetry: waiting for flight-controller response");
    m_stop.set_sensitive(false);

    m_buttons.pack_start(m_start, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_stop, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_arm, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_takeoff, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_land, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_patrol, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_startMission, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_stopPatrol, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_controlToggle, Gtk::PACK_SHRINK);
    m_box.pack_start(m_buttons, Gtk::PACK_SHRINK);

    m_patrolHint.set_text("Patrol center / size");
    m_patrolLat.set_text("35.1186");
    m_patrolLon.set_text("-89.9375");
    m_patrolSizeMeters.set_text("140");
    m_patrolLat.set_width_chars(9);
    m_patrolLon.set_width_chars(10);
    m_patrolSizeMeters.set_width_chars(6);
    m_patrolControls.pack_start(m_patrolHint, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLat, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolLon, Gtk::PACK_SHRINK);
    m_patrolControls.pack_start(m_patrolSizeMeters, Gtk::PACK_SHRINK);
    m_box.pack_start(m_patrolControls, Gtk::PACK_SHRINK);

    m_vehiclePanel.set_border_width(8);
    m_vehicleHint.set_text("Connected / expected drones");
    m_vehiclePanel.pack_start(m_vehicleHint, Gtk::PACK_SHRINK);
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      const auto selected = i == 0;
      auto* rowLabel = Gtk::manage(new Gtk::Label(
        std::string(selected ? "● " : "○ ") + "Drone " + m_droneIds[i] +
        (selected ? "  active" : "  standby")));
      rowLabel->set_xalign(0.0F);
      m_vehicleList.append(*rowLabel);
    }
    m_vehicleList.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
      if (row == nullptr) {
        return;
      }
      const auto index = row->get_index();
      if (index < 0 || static_cast<size_t>(index) >= m_droneIds.size()) {
        return;
      }
      m_runtime.setTargetDroneId(m_droneIds[static_cast<size_t>(index)]);
      updateVehicleRows();
      updateVideoViewForSelected();
      m_runtime.requestTelemetryStatus();
    });
    m_vehiclePanel.pack_start(m_vehicleList, Gtk::PACK_SHRINK);
    m_vehicleFrame.add(m_vehiclePanel);
    m_workspace.pack_start(m_vehicleFrame, Gtk::PACK_SHRINK);

    m_centerPanel.set_border_width(8);
    m_mapMission.set_xalign(0.0F);
    m_mapZoomIn.set_tooltip_text("Zoom in");
    m_mapZoomOut.set_tooltip_text("Zoom out");
    m_mapCenterGs.set_tooltip_text("Return map view to the ground station");
    m_mapUndoWp.set_tooltip_text("Remove the last mission waypoint");
    m_mapClearWp.set_tooltip_text("Clear all mission waypoints");
    m_mapControls.pack_start(m_mapZoomIn, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapZoomOut, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapCenterGs, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapUndoWp, Gtk::PACK_SHRINK);
    m_mapControls.pack_start(m_mapClearWp, Gtk::PACK_SHRINK);
    m_centerPanel.pack_start(m_mapControls, Gtk::PACK_SHRINK);
    m_mapImage.set_size_request(256, 256);
    m_mapEventBox.set_size_request(256, 256);
    m_mapImage.set_halign(Gtk::ALIGN_START);
    m_mapImage.set_valign(Gtk::ALIGN_START);
    m_mapEventBox.set_halign(Gtk::ALIGN_START);
    m_mapEventBox.set_valign(Gtk::ALIGN_START);
    m_mapEventBox.add(m_mapImage);
    m_mapEventBox.add_events(Gdk::BUTTON_PRESS_MASK |
                             Gdk::BUTTON_RELEASE_MASK |
                             Gdk::POINTER_MOTION_MASK |
                             Gdk::SCROLL_MASK |
                             Gdk::SMOOTH_SCROLL_MASK);
    m_mapEventBox.signal_button_press_event().connect([this](GdkEventButton* event) {
      if (event == nullptr || event->button != 1) {
        return false;
      }
      beginMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_button_release_event().connect([this](GdkEventButton* event) {
      if (event == nullptr || event->button != 1) {
        return false;
      }
      finishMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_motion_notify_event().connect([this](GdkEventMotion* event) {
      if (event == nullptr || !m_mapDragging) {
        return false;
      }
      updateMapDrag(event->x, event->y);
      return true;
    });
    m_mapEventBox.signal_scroll_event().connect([this](GdkEventScroll* event) {
      if (event == nullptr) {
        return false;
      }
      if (event->direction == GDK_SCROLL_UP ||
          (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0.0)) {
        zoomMap(1);
        return true;
      }
      if (event->direction == GDK_SCROLL_DOWN ||
          (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0.0)) {
        zoomMap(-1);
        return true;
      }
      return false;
    });
    m_centerPanel.pack_start(m_mapEventBox, Gtk::PACK_SHRINK);
    m_centerPanel.pack_start(m_mapMission, Gtk::PACK_SHRINK);
    m_centerPanel.pack_start(m_image, Gtk::PACK_EXPAND_WIDGET);
    m_centerFrame.add(m_centerPanel);
    m_workspace.pack_start(m_centerFrame, Gtk::PACK_EXPAND_WIDGET);

    m_statusPanel.set_border_width(8);
    m_status.set_xalign(0.0F);
    m_stats.set_xalign(0.0F);
    m_services.set_xalign(0.0F);
    m_telemetry.set_xalign(0.0F);
    m_statusPanel.pack_start(m_status, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_stats, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_services, Gtk::PACK_SHRINK);
    m_statusPanel.pack_start(m_telemetry, Gtk::PACK_SHRINK);
    m_statusFrame.add(m_statusPanel);
    m_workspace.pack_start(m_statusFrame, Gtk::PACK_SHRINK);

    m_controlHelp.set_text(
      "Manual control: hold W/A/S/D/Q/E/R/F to fly. I/T/L send arm/takeoff/land. "
      "The active key turns black while pressed.");
    m_controlPanel.pack_start(m_controlHelp, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyW, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyA, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyS, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyD, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyQ, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyE, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyR, Gtk::PACK_SHRINK);
    m_manualKeyRow.pack_start(m_keyF, Gtk::PACK_SHRINK);
    m_commandKeyRow.pack_start(m_keyI, Gtk::PACK_SHRINK);
    m_commandKeyRow.pack_start(m_keyT, Gtk::PACK_SHRINK);
    m_commandKeyRow.pack_start(m_keyL, Gtk::PACK_SHRINK);
    m_commandKeyRow.pack_start(m_keyV, Gtk::PACK_SHRINK);
    m_commandKeyRow.pack_start(m_keyX, Gtk::PACK_SHRINK);
    m_controlPanel.pack_start(m_manualKeyRow, Gtk::PACK_SHRINK);
    m_controlPanel.pack_start(m_commandKeyRow, Gtk::PACK_SHRINK);
    m_box.pack_start(m_controlPanel, Gtk::PACK_SHRINK);
    m_box.pack_start(m_workspace, Gtk::PACK_EXPAND_WIDGET);
    installControlCss();
    configureKeycap(m_keyW);
    configureKeycap(m_keyA);
    configureKeycap(m_keyS);
    configureKeycap(m_keyD);
    configureKeycap(m_keyQ);
    configureKeycap(m_keyE);
    configureKeycap(m_keyR);
    configureKeycap(m_keyF);
    configureKeycap(m_keyI);
    configureKeycap(m_keyT);
    configureKeycap(m_keyL);
    configureKeycap(m_keyV);
    configureKeycap(m_keyX);
    m_controlPanel.hide();
    add(m_box);
    show_all_children();
    m_controlPanel.hide();
    if (auto* firstRow = m_vehicleList.get_row_at_index(0)) {
      m_vehicleList.select_row(*firstRow);
      updateVehicleRows();
    }
    refreshMapTile();

    m_start.signal_clicked().connect([this] {
      m_start.set_sensitive(false);
      m_stop.set_sensitive(false);
      m_runtime.startVideo();
    });
    m_stop.signal_clicked().connect([this] {
      m_stop.set_sensitive(false);
      m_acceptFrames = false;
      m_runtime.stopVideo();
    });
    m_arm.signal_clicked().connect([this] {
      m_runtime.sendMavlinkCommand("arm", {{"arm", "true"}});
    });
    m_takeoff.signal_clicked().connect([this] {
      m_runtime.sendMavlinkCommand("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
    });
    m_land.signal_clicked().connect([this] {
      m_runtime.sendMavlinkCommand("land");
    });
    m_patrol.signal_clicked().connect([this] {
      m_patrol.set_sensitive(false);
      m_status.set_text("Uploading cooperative patrol mission...");
      double centerLat = 35.1186;
      double centerLon = -89.9375;
      double sideMeters = 140.0;
      try {
        centerLat = std::stod(m_patrolLat.get_text());
        centerLon = std::stod(m_patrolLon.get_text());
        sideMeters = std::stod(m_patrolSizeMeters.get_text());
      }
      catch (const std::exception&) {
        m_status.set_text("Invalid patrol input; using University of Memphis fallback");
      }
      const auto routeWaypoints = m_planWaypoints;
      std::thread([this, centerLat, centerLon, sideMeters, routeWaypoints] {
        const bool ok = m_runtime.runPatrolCompensationTask(
          std::chrono::seconds(30), centerLat, centerLon, sideMeters, false, routeWaypoints);
        Glib::signal_idle().connect_once([this, ok] {
          m_status.set_text(ok ? "Patrol mission uploaded; arm/takeoff and start mission mode to fly it"
                               : "Patrol mission upload failed");
          m_patrol.set_sensitive(true);
        });
      }).detach();
    });
    m_startMission.signal_clicked().connect([this] {
      m_status.set_text("Starting patrol mission by phase: arm all, take off all, start all...");
      scheduleMissionStartPhase(0, 0);
    });
    m_stopPatrol.signal_clicked().connect([this] {
      m_status.set_text("Stopping patrol: landing patrol drones...");
      schedulePatrolLandSequence(0);
    });
    m_controlToggle.signal_clicked().connect([this] {
      setControlMode(!m_controlMode);
    });
    m_mapZoomIn.signal_clicked().connect([this] {
      zoomMap(1);
    });
    m_mapZoomOut.signal_clicked().connect([this] {
      zoomMap(-1);
    });
    m_mapCenterGs.signal_clicked().connect([this] {
      centerMapOnGroundStation();
    });
    m_mapUndoWp.signal_clicked().connect([this] {
      undoMissionWaypoint();
    });
    m_mapClearWp.signal_clicked().connect([this] {
      clearMissionWaypoints();
    });
    signal_key_press_event().connect(
      [this](GdkEventKey* event) {
        if (event == nullptr) {
          return false;
        }
        return handleShortcutKeyPress(event->keyval);
      },
      false);
    signal_key_release_event().connect(
      [this](GdkEventKey* event) {
        if (event == nullptr) {
          return false;
        }
        return handleShortcutKeyRelease(event->keyval);
      },
      false);

    m_runtime.setStatusCallback([this](std::string status) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (status.rfind("Video packet stream", 0) == 0) {
          const auto droneId = statusField(status, "drone", "");
          if (droneId == m_runtime.targetDroneId()) {
            beginLocalStreamViewLocked();
          }
          updateVideoViewForSelectedLocked();
        }
        else if (status.rfind("Video stopped", 0) == 0) {
          updateVideoViewForSelectedLocked();
          status += ", decoded=" + std::to_string(m_decodedFrames.load());
        }
        else if (status.rfind("Timeout waiting for ", 0) == 0 ||
                 status.rfind("Video control response missing", 0) == 0 ||
                 status.rfind("NDNSF runtime not ready", 0) == 0 ||
                 status.rfind("No video streaming for selected drone ", 0) == 0 ||
                 status.rfind("Video already streaming drone=", 0) == 0) {
          updateVideoViewForSelectedLocked();
        }
        if (status.rfind("MAVLink ", 0) == 0 ||
            status.rfind("Telemetry ", 0) == 0) {
          m_pendingTelemetry = status;
          if (status.rfind("Telemetry ", 0) == 0) {
            m_pendingMap = mapTextForTelemetry(status);
            m_pendingMapLat = statusField(status, "lat", "35.1186");
            m_pendingMapLon = statusField(status, "lon", "-89.9375");
            try {
              m_dronePositions[statusField(status, "drone", m_runtime.targetDroneId())] = {
                std::stod(m_pendingMapLat), std::stod(m_pendingMapLon)
              };
            }
            catch (const std::exception&) {
            }
          }
        }
        m_pendingStatus = std::move(status);
      }
      m_statusDispatcher.emit();
    });
    m_runtime.setFrameCallback([this](std::vector<uint8_t> frame, uint64_t seq, uint64_t elapsedMs) {
      pushEncodedChunk(std::move(frame), seq, elapsedMs);
    });

    m_statusDispatcher.connect([this] {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (m_pendingButtonState) {
        m_start.set_sensitive(m_pendingStartSensitive);
        m_stop.set_sensitive(m_pendingStopSensitive);
        m_pendingButtonState = false;
      }
      if (m_pendingClearFrame) {
        m_image.clear();
        m_stats.set_text("Decoded frames: 0");
        m_pendingClearFrame = false;
      }
      m_status.set_text(m_pendingStatus);
      if (!m_pendingTelemetry.empty()) {
        m_telemetry.set_text("Telemetry: " + m_pendingTelemetry);
      }
      if (!m_pendingMap.empty()) {
        m_mapMission.set_text(m_pendingMap);
        refreshMapTile();
      }
    });
    m_frameDispatcher.connect([this] {
      Glib::RefPtr<Gdk::Pixbuf> pixbuf;
      uint64_t seq = 0;
      uint64_t elapsedMs = 0;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        pixbuf = m_pendingPixbuf;
        seq = m_pendingSeq;
        elapsedMs = m_pendingElapsedMs;
      }
      if (pixbuf) {
        m_image.set(pixbuf);
        m_stats.set_text("Decoded frames: " + std::to_string(m_decodedFrames.load()) +
                         "  latest chunk: " + std::to_string(seq) +
                         "  stream elapsed: " + std::to_string(elapsedMs) + " ms");
      }
    });
    Glib::signal_timeout().connect([this] {
      if (!m_droneIds.empty()) {
        const auto droneId = m_droneIds[m_telemetryPollIndex++ % m_droneIds.size()];
        m_runtime.requestTelemetryStatusForDrone(droneId);
      }
      else {
        m_runtime.requestTelemetryStatus();
      }
      return true;
    }, 1500);

	    if (autoStart) {
	      std::thread([this, autoStopSeconds, autoStartDelayMs] {
	        std::this_thread::sleep_for(std::chrono::milliseconds(autoStartDelayMs));
	        m_runtime.startVideo();
        for (int i = 0; i < 100 && !m_runtime.isStreaming(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::seconds(autoStopSeconds));
        m_runtime.stopVideo();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }

    if (autoMavlinkTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_runtime.sendMavlinkCommand("arm", {{"arm", "true"}});
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
        std::this_thread::sleep_for(std::chrono::seconds(3));
        m_runtime.sendMavlinkCommand("land");
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }

    if (autoKeyboardTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
          handleShortcutKeyPress(GDK_KEY_i);
          handleShortcutKeyRelease(GDK_KEY_i);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_t);
          handleShortcutKeyRelease(GDK_KEY_t);
        });
        std::this_thread::sleep_for(std::chrono::seconds(8));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_l);
          handleShortcutKeyRelease(GDK_KEY_l);
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    if (autoManualControlTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          setControlMode(true);
          handleShortcutKeyPress(GDK_KEY_i);
          handleShortcutKeyRelease(GDK_KEY_i);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_t);
          handleShortcutKeyRelease(GDK_KEY_t);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_w);
          handleShortcutKeyPress(GDK_KEY_r);
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyRelease(GDK_KEY_w);
          handleShortcutKeyRelease(GDK_KEY_r);
        });
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this] {
          handleShortcutKeyPress(GDK_KEY_l);
          handleShortcutKeyRelease(GDK_KEY_l);
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    if (autoTwoDroneSwitchTest) {
      std::thread([this] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (m_droneIds.size() < 2) {
          Glib::signal_idle().connect_once([this] {
            m_status.set_text("Two-drone switch test needs at least two drones");
            hide();
          });
          return;
        }
        const auto first = m_droneIds[0];
        const auto second = m_droneIds[1];
        Glib::signal_idle().connect_once([this, second] {
          setControlMode(true);
          m_runtime.setTargetDroneId(second);
          updateVehicleRows();
          updateVideoViewForSelected();
          m_runtime.requestTelemetryStatus();
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("manual_control", {
          {"x", "500"}, {"y", "0"}, {"z", "520"}, {"r", "0"},
        });
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Glib::signal_idle().connect_once([this, first] {
          m_runtime.setTargetDroneId(first);
          updateVehicleRows();
          updateVideoViewForSelected();
          m_runtime.requestTelemetryStatus();
        });
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_runtime.sendMavlinkCommand("manual_control", {
          {"x", "-500"}, {"y", "0"}, {"z", "520"}, {"r", "0"},
        });
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
    m_manualControlThread = std::thread([this] {
      runManualControlLoop();
    });
  }

  ~GroundStationWindow() override
  {
    m_acceptFrames = false;
    if (m_runtime.isStreaming()) {
      m_runtime.stopVideo();
    }
    m_controlMode = false;
    m_manualActive = false;
    m_manualControlDone = true;
    if (m_manualControlThread.joinable()) {
      m_manualControlThread.join();
    }
  }

private:
  bool
  handleShortcutKeyPress(guint keyval)
  {
    if (!m_controlMode) {
      return false;
    }
    const auto normalized = normalizeShortcutKey(keyval);
    if (normalized == 0) {
      return false;
    }
    setKeyPressed(normalized, true);
    if (!m_pressedKeys.insert(normalized).second) {
      return true;
    }

    switch (normalized) {
    case GDK_KEY_w:
    case GDK_KEY_a:
    case GDK_KEY_s:
    case GDK_KEY_d:
    case GDK_KEY_q:
    case GDK_KEY_e:
    case GDK_KEY_r:
    case GDK_KEY_f:
      updateManualAxisState();
      return true;
    case GDK_KEY_i:
      m_runtime.sendMavlinkCommand("arm", {{"arm", "true"}});
      return true;
    case GDK_KEY_t:
      m_runtime.sendMavlinkCommand("takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
      return true;
    case GDK_KEY_l:
      m_runtime.sendMavlinkCommand("land");
      return true;
    case GDK_KEY_v:
      if (!m_runtime.isStreamingForDrone(m_runtime.targetDroneId())) {
        m_start.set_sensitive(false);
        m_stop.set_sensitive(false);
        m_runtime.startVideo();
      }
      return true;
    case GDK_KEY_x:
      if (m_runtime.isStreamingForDrone(m_runtime.targetDroneId())) {
        m_stop.set_sensitive(false);
        m_acceptFrames = false;
        m_runtime.stopVideo();
      }
      return true;
    default:
      return false;
    }
  }

  bool
  handleShortcutKeyRelease(guint keyval)
  {
    const auto normalized = normalizeShortcutKey(keyval);
    if (normalized == 0) {
      return false;
    }
    m_pressedKeys.erase(normalized);
    setKeyPressed(normalized, false);
    updateManualAxisState();
    return m_controlMode;
  }

  guint
  normalizeShortcutKey(guint keyval) const
  {
    switch (keyval) {
    case GDK_KEY_w:
    case GDK_KEY_W:
      return GDK_KEY_w;
    case GDK_KEY_a:
    case GDK_KEY_A:
      return GDK_KEY_a;
    case GDK_KEY_s:
    case GDK_KEY_S:
      return GDK_KEY_s;
    case GDK_KEY_d:
    case GDK_KEY_D:
      return GDK_KEY_d;
    case GDK_KEY_q:
    case GDK_KEY_Q:
      return GDK_KEY_q;
    case GDK_KEY_e:
    case GDK_KEY_E:
      return GDK_KEY_e;
    case GDK_KEY_r:
    case GDK_KEY_R:
      return GDK_KEY_r;
    case GDK_KEY_f:
    case GDK_KEY_F:
      return GDK_KEY_f;
    case GDK_KEY_i:
    case GDK_KEY_I:
      return GDK_KEY_i;
    case GDK_KEY_t:
    case GDK_KEY_T:
      return GDK_KEY_t;
    case GDK_KEY_l:
    case GDK_KEY_L:
      return GDK_KEY_l;
    case GDK_KEY_v:
    case GDK_KEY_V:
      return GDK_KEY_v;
    case GDK_KEY_x:
    case GDK_KEY_X:
      return GDK_KEY_x;
    default:
      return 0;
    }
  }

  void
  setControlMode(bool enabled)
  {
    m_controlMode = enabled;
    m_controlToggle.set_label(enabled ? "Stop Control" : "Start Control");
    if (enabled) {
      m_controlPanel.show();
      grab_focus();
    }
    else {
      m_pressedKeys.clear();
      m_manualX = 0;
      m_manualY = 0;
      m_manualZ = 500;
      m_manualR = 0;
      m_manualActive = false;
      sendManualControlOnce();
      setKeyPressed(GDK_KEY_w, false);
      setKeyPressed(GDK_KEY_a, false);
      setKeyPressed(GDK_KEY_s, false);
      setKeyPressed(GDK_KEY_d, false);
      setKeyPressed(GDK_KEY_q, false);
      setKeyPressed(GDK_KEY_e, false);
      setKeyPressed(GDK_KEY_r, false);
      setKeyPressed(GDK_KEY_f, false);
      setKeyPressed(GDK_KEY_i, false);
      setKeyPressed(GDK_KEY_t, false);
      setKeyPressed(GDK_KEY_l, false);
      setKeyPressed(GDK_KEY_v, false);
      setKeyPressed(GDK_KEY_x, false);
      m_controlPanel.hide();
    }
  }

  void
  configureKeycap(Gtk::Button& button)
  {
    button.set_sensitive(false);
    button.get_style_context()->add_class("uav-keycap");
  }

  void
  installControlCss()
  {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(
      ".uav-keycap {"
      "  color: #202124;"
      "  background: #f3f4f6;"
      "  border: 1px solid #9aa0a6;"
      "  border-radius: 4px;"
      "  padding: 6px 10px;"
      "}"
      ".uav-keycap-active {"
      "  color: white;"
      "  background: #111111;"
      "  border: 1px solid #111111;"
      "}"
    );
    auto screen = Gdk::Screen::get_default();
    if (screen) {
      Gtk::StyleContext::add_provider_for_screen(
        screen, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
  }

  void
  setKeyPressed(guint keyval, bool pressed)
  {
    Gtk::Button* button = nullptr;
    switch (keyval) {
    case GDK_KEY_w:
      button = &m_keyW;
      break;
    case GDK_KEY_a:
      button = &m_keyA;
      break;
    case GDK_KEY_s:
      button = &m_keyS;
      break;
    case GDK_KEY_d:
      button = &m_keyD;
      break;
    case GDK_KEY_q:
      button = &m_keyQ;
      break;
    case GDK_KEY_e:
      button = &m_keyE;
      break;
    case GDK_KEY_r:
      button = &m_keyR;
      break;
    case GDK_KEY_f:
      button = &m_keyF;
      break;
    case GDK_KEY_i:
      button = &m_keyI;
      break;
    case GDK_KEY_t:
      button = &m_keyT;
      break;
    case GDK_KEY_l:
      button = &m_keyL;
      break;
    case GDK_KEY_v:
      button = &m_keyV;
      break;
    case GDK_KEY_x:
      button = &m_keyX;
      break;
    default:
      return;
    }
    auto context = button->get_style_context();
    if (pressed) {
      context->add_class("uav-keycap-active");
    }
    else {
      context->remove_class("uav-keycap-active");
    }
  }

  void
  updateManualAxisState()
  {
    auto has = [this](guint key) {
      return m_pressedKeys.find(key) != m_pressedKeys.end();
    };
    const int x = (has(GDK_KEY_w) ? 650 : 0) + (has(GDK_KEY_s) ? -650 : 0);
    const int y = (has(GDK_KEY_e) ? 500 : 0) + (has(GDK_KEY_q) ? -500 : 0);
    const int r = (has(GDK_KEY_d) ? 550 : 0) + (has(GDK_KEY_a) ? -550 : 0);
    const int z = 500 + (has(GDK_KEY_r) ? 250 : 0) + (has(GDK_KEY_f) ? -250 : 0);
    m_manualX = std::clamp(x, -1000, 1000);
    m_manualY = std::clamp(y, -1000, 1000);
    m_manualZ = std::clamp(z, 0, 1000);
    m_manualR = std::clamp(r, -1000, 1000);
    m_manualActive = has(GDK_KEY_w) || has(GDK_KEY_a) || has(GDK_KEY_s) ||
                     has(GDK_KEY_d) || has(GDK_KEY_q) || has(GDK_KEY_e) ||
                     has(GDK_KEY_r) || has(GDK_KEY_f);
  }

  void
  runManualControlLoop()
  {
    while (!m_manualControlDone.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (!m_controlMode) {
        continue;
      }
      sendManualControlOnce();
    }
  }

  void
  sendManualControlOnce()
  {
    m_runtime.sendMavlinkCommand("manual_control", {
      {"x", std::to_string(m_manualX.load())},
      {"y", std::to_string(m_manualY.load())},
      {"z", std::to_string(m_manualZ.load())},
      {"r", std::to_string(m_manualR.load())},
      {"buttons", "0"},
    });
  }

  void
  setPendingButtonStateLocked(bool startSensitive, bool stopSensitive)
  {
    m_pendingButtonState = true;
    m_pendingStartSensitive = startSensitive;
    m_pendingStopSensitive = stopSensitive;
  }

  void
  beginLocalStreamView()
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    beginLocalStreamViewLocked();
  }

  void
  beginLocalStreamViewLocked()
  {
    ++m_streamGeneration;
    m_acceptFrames = true;
    m_decodedFrames = 0;
    m_pendingSeq = 0;
    m_pendingElapsedMs = 0;
    m_pendingPixbuf.reset();
  }

  void
  updateVideoViewForSelected()
  {
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      updateVideoViewForSelectedLocked();
    }
    m_statusDispatcher.emit();
  }

  void
  updateVideoViewForSelectedLocked()
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    const bool selectedStreaming = m_runtime.isStreamingForDrone(selectedDrone);
    setPendingButtonStateLocked(!selectedStreaming, selectedStreaming);
    if (selectedStreaming) {
      if (!m_acceptFrames) {
        beginLocalStreamViewLocked();
      }
      m_pendingClearFrame = false;
      return;
    }

    ++m_streamGeneration;
    m_acceptFrames = false;
    m_pendingPixbuf.reset();
    m_pendingSeq = 0;
    m_pendingElapsedMs = 0;
    m_decodedFrames = 0;
    m_pendingClearFrame = true;
  }

  void
  pushEncodedChunk(std::vector<uint8_t> chunk, uint64_t seq, uint64_t elapsedMs)
  {
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_acceptFrames || !m_runtime.isStreamingForDrone(m_runtime.targetDroneId())) {
        return;
      }
      generation = m_streamGeneration;
    }
    Glib::signal_idle().connect_once([this, chunk = std::move(chunk), seq, elapsedMs, generation] {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_acceptFrames ||
            generation != m_streamGeneration ||
            !m_runtime.isStreamingForDrone(m_runtime.targetDroneId())) {
          return;
        }
      }
      auto loader = Gdk::PixbufLoader::create();
      try {
        loader->write(chunk.data(), chunk.size());
        loader->close();
        auto pixbuf = loader->get_pixbuf();
        if (pixbuf) {
          {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_pendingPixbuf = pixbuf;
            m_pendingSeq = seq;
            m_pendingElapsedMs = elapsedMs;
          }
          ++m_decodedFrames;
          if (m_decodedFrames.load() % 30 == 0) {
            std::cout << "GS_DECODED_FRAMES count=" << m_decodedFrames.load() << std::endl;
          }
          m_frameDispatcher.emit();
        }
      }
      catch (const Glib::Error& e) {
        NDN_LOG_WARN("GS_DECODER_ERROR " << e.what());
        std::cout << "GS_DECODER_ERROR " << e.what() << std::endl;
      }
    });
  }

  void
  updateVehicleRows()
  {
    const auto selectedDrone = m_runtime.targetDroneId();
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      auto* row = m_vehicleList.get_row_at_index(static_cast<int>(i));
      if (row == nullptr) {
        continue;
      }
      auto* label = dynamic_cast<Gtk::Label*>(row->get_child());
      if (label == nullptr) {
        continue;
      }
      const bool selected = m_droneIds[i] == selectedDrone;
      label->set_text(std::string(selected ? "● " : "○ ") + "Drone " + m_droneIds[i] +
                      (selected ? "  active" : "  standby"));
    }
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "GS center: University of Memphis\n"
                          "Selected drone: " + selectedDrone + "\n"
                          "Map markers show GS, drones, and mission waypoints.\n"
                          "Click map to append waypoints, then upload/start the mission.");
    m_services.set_text("Services for Drone " + selectedDrone +
                        ": video, targeted MAVLink, telemetry, camera, mission");
    refreshMapTile();
  }

  static std::string
  statusField(const std::string& status, const std::string& key,
              const std::string& fallback = "unknown")
  {
    const auto marker = key + "=";
    const auto start = status.find(marker);
    if (start == std::string::npos) {
      return fallback;
    }
    const auto valueStart = start + marker.size();
    const auto valueEnd = status.find(' ', valueStart);
    return status.substr(valueStart, valueEnd == std::string::npos ?
                         std::string::npos : valueEnd - valueStart);
  }

  static std::string
  mapTextForTelemetry(const std::string& status)
  {
    const auto drone = statusField(status, "drone");
    const auto lat = statusField(status, "lat");
    const auto lon = statusField(status, "lon");
    const auto alt = statusField(status, "alt");
    const auto mission = statusField(status, "mission");
    return "Map / mission workspace\n\n"
           "Selected drone: " + drone + "\n"
           "Position: lat " + lat + "  lon " + lon + "\n"
           "Altitude: " + alt + "\n"
           "Mission: " + mission + "\n\n"
           "Map tile: OpenStreetMap, centered on the ground station.\n"
           "Click the map to append mission waypoints.";
  }

  struct MapMarker
  {
    std::string label;
    double lat = 35.1186;
    double lon = -89.9375;
    uint8_t r = 220;
    uint8_t g = 20;
    uint8_t b = 40;
  };

  static std::pair<double, double>
  tileFloatForLatLon(double lat, double lon, int zoom)
  {
    const auto latRad = lat * M_PI / 180.0;
    const auto n = std::pow(2.0, zoom);
    return {
      (lon + 180.0) / 360.0 * n,
      (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * n
    };
  }

  static std::pair<double, double>
  latLonForTilePixel(int tileX, int tileY, int zoom, double pixelX, double pixelY)
  {
    const auto n = std::pow(2.0, zoom);
    const auto x = (static_cast<double>(tileX) + pixelX / 256.0) / n;
    const auto y = (static_cast<double>(tileY) + pixelY / 256.0) / n;
    const auto lon = x * 360.0 - 180.0;
    const auto latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y)));
    return {latRad * 180.0 / M_PI, lon};
  }

  static std::pair<double, double>
  latLonForTileFloat(double tileX, double tileY, int zoom)
  {
    const auto n = std::pow(2.0, zoom);
    const auto lon = tileX / n * 360.0 - 180.0;
    const auto latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * tileY / n)));
    return {latRad * 180.0 / M_PI, lon};
  }

  static void
  drawTinyText(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf, int x, int y,
               const std::string& text, uint8_t r, uint8_t g, uint8_t b)
  {
    if (!pixbuf) {
      return;
    }
    const int width = pixbuf->get_width();
    const int height = pixbuf->get_height();
    const int channels = pixbuf->get_n_channels();
    const int rowstride = pixbuf->get_rowstride();
    auto* pixels = pixbuf->get_pixels();
    if (pixels == nullptr || channels < 3) {
      return;
    }

    auto setPixel = [&] (int x, int y, uint8_t r, uint8_t g, uint8_t b) {
      if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
      }
      auto* p = pixels + y * rowstride + x * channels;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      if (channels >= 4) {
        p[3] = 255;
      }
    };

    auto patternFor = [] (char ch) -> std::array<const char*, 7> {
      switch (ch) {
      case 'A': return {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
      case 'B': return {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
      case 'D': return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
      case 'G': return {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
      case 'N': return {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
      case 'O': return {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
      case 'P': return {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
      case 'R': return {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
      case 'S': return {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
      case 'W': return {"10001", "10001", "10001", "10101", "10101", "11011", "10001"};
      case '0': return {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
      case '1': return {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
      case '2': return {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
      case '3': return {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
      case '4': return {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
      case '5': return {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
      case '6': return {"01110", "10000", "10000", "11110", "10001", "10001", "01110"};
      case '7': return {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
      case '8': return {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
      case '9': return {"01110", "10001", "10001", "01111", "00001", "00001", "01110"};
      default: return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
      }
    };

    int cursor = x;
    for (char ch : text) {
      if (ch >= 'a' && ch <= 'z') {
        ch = static_cast<char>(ch - 'a' + 'A');
      }
      if (ch == ' ') {
        cursor += 4;
        continue;
      }
      const auto pattern = patternFor(ch);
      for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
          if (pattern[row][col] == '1') {
            setPixel(cursor + col, y + row, r, g, b);
          }
        }
      }
      cursor += 6;
    }
  }

  static void
  drawMapMarker(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf, int cx, int cy,
                const std::string& label, uint8_t r, uint8_t g, uint8_t b)
  {
    if (!pixbuf) {
      return;
    }
    const int width = pixbuf->get_width();
    const int height = pixbuf->get_height();
    const int channels = pixbuf->get_n_channels();
    const int rowstride = pixbuf->get_rowstride();
    auto* pixels = pixbuf->get_pixels();
    if (pixels == nullptr || channels < 3) {
      return;
    }

    auto setPixel = [&] (int x, int y, uint8_t r, uint8_t g, uint8_t b) {
      if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
      }
      auto* p = pixels + y * rowstride + x * channels;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      if (channels >= 4) {
        p[3] = 255;
      }
    };

    for (int dy = -8; dy <= 8; ++dy) {
      for (int dx = -8; dx <= 8; ++dx) {
        const auto dist2 = dx * dx + dy * dy;
        if (dist2 <= 64 && dist2 >= 36) {
          setPixel(cx + dx, cy + dy, 255, 255, 255);
        }
        if (std::abs(dx) <= 1 || std::abs(dy) <= 1) {
          setPixel(cx + dx, cy + dy, r, g, b);
        }
      }
    }
    for (int d = -4; d <= 4; ++d) {
      setPixel(cx + d, cy + d, r, g, b);
      setPixel(cx + d, cy - d, r, g, b);
    }
    drawTinyText(pixbuf, std::clamp(cx + 10, 0, width - 24),
                 std::clamp(cy - 4, 0, height - 8), label, r, g, b);
  }

  std::vector<MapMarker>
  currentMapMarkers() const
  {
    std::vector<MapMarker> markers;
    markers.push_back({"GS", m_groundStationLat, m_groundStationLon, 20, 80, 220});
    for (size_t i = 0; i < m_droneIds.size(); ++i) {
      const auto& droneId = m_droneIds[i];
      const auto found = m_dronePositions.find(droneId);
      double lat = m_groundStationLat;
      double lon = m_groundStationLon + (static_cast<double>(i) + 1.0) * 0.0007;
      if (found != m_dronePositions.end()) {
        lat = found->second.first;
        lon = found->second.second;
      }
      if (std::abs(lat - m_groundStationLat) < 0.00001 &&
          std::abs(lon - m_groundStationLon) < 0.00001) {
        lon += (static_cast<double>(i) + 1.0) * 0.00055;
      }
      markers.push_back({droneId, lat, lon,
                         static_cast<uint8_t>(i == 0 ? 220 : 20),
                         static_cast<uint8_t>(i == 0 ? 40 : 160),
                         static_cast<uint8_t>(i == 0 ? 40 : 70)});
    }
    if (!m_planWaypoints.empty()) {
      for (size_t i = 0; i < m_planWaypoints.size(); ++i) {
        markers.push_back({"WP" + std::to_string(i + 1),
                           m_planWaypoints[i].first, m_planWaypoints[i].second,
                           245, 160, 20});
      }
    }
    else if (m_hasPatrolCenter) {
      markers.push_back({"WP", m_patrolCenterLat, m_patrolCenterLon, 245, 160, 20});
    }
    return markers;
  }

  void
  updatePatrolInputsFromWaypoints()
  {
    if (m_planWaypoints.empty()) {
      m_hasPatrolCenter = false;
      return;
    }
    double latSum = 0.0;
    double lonSum = 0.0;
    for (const auto& point : m_planWaypoints) {
      latSum += point.first;
      lonSum += point.second;
    }
    m_patrolCenterLat = latSum / static_cast<double>(m_planWaypoints.size());
    m_patrolCenterLon = lonSum / static_cast<double>(m_planWaypoints.size());
    m_hasPatrolCenter = true;
    std::ostringstream latText;
    std::ostringstream lonText;
    latText << std::fixed << std::setprecision(6) << m_patrolCenterLat;
    lonText << std::fixed << std::setprecision(6) << m_patrolCenterLon;
    m_patrolLat.set_text(latText.str());
    m_patrolLon.set_text(lonText.str());
  }

  void
  undoMissionWaypoint()
  {
    if (m_planWaypoints.empty()) {
      return;
    }
    m_planWaypoints.pop_back();
    updatePatrolInputsFromWaypoints();
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Removed last waypoint. Current mission waypoint count: " +
                          std::to_string(m_planWaypoints.size()) + ".");
    refreshMapTile();
  }

  void
  clearMissionWaypoints()
  {
    m_planWaypoints.clear();
    updatePatrolInputsFromWaypoints();
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Mission waypoints cleared. Click the map to append WP1/WP2/...");
    refreshMapTile();
  }

  void
  scheduleMissionStartPhase(int phase, size_t droneIndex)
  {
    const auto readyDrones = m_runtime.missionReadyDrones();
    if (readyDrones.empty()) {
      m_status.set_text("No uploaded patrol mission is ready; upload mission before Start Mission");
      return;
    }
    if (droneIndex >= readyDrones.size()) {
      if (phase == 0) {
        m_status.set_text("Mission sequence: all patrol drones armed; taking off next");
        Glib::signal_timeout().connect([this] {
          scheduleMissionStartPhase(1, 0);
          return false;
        }, 1800);
        return;
      }
      if (phase == 1) {
        m_status.set_text("Mission sequence: all patrol drones takeoff sent; starting missions next");
        Glib::signal_timeout().connect([this] {
          scheduleMissionStartPhase(2, 0);
          return false;
        }, 6500);
        return;
      }
      m_status.set_text("Mission start sequence sent to patrol drones");
      return;
    }

    const auto droneId = readyDrones[droneIndex];
    if (phase == 0) {
      m_status.set_text("Mission sequence: arming Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(readyDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "arm", {{"arm", "true"}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(0, droneIndex + 1);
        return false;
      }, 900);
      return;
    }
    if (phase == 1) {
      m_status.set_text("Mission sequence: takeoff Drone " + droneId +
                        " (" + std::to_string(droneIndex + 1) + "/" +
                        std::to_string(readyDrones.size()) + ")");
      m_runtime.sendMavlinkCommandToDrone(droneId, "takeoff", {{"altitude_m", PX4_SITL_TAKEOFF_AMSL_M}});
      Glib::signal_timeout().connect([this, droneIndex] {
        scheduleMissionStartPhase(1, droneIndex + 1);
        return false;
      }, 900);
      return;
    }

    m_status.set_text("Mission sequence: starting mission on Drone " + droneId +
                      " (" + std::to_string(droneIndex + 1) + "/" +
                      std::to_string(readyDrones.size()) + ")");
    m_runtime.sendMavlinkCommandToDrone(droneId, "start_mission");
    Glib::signal_timeout().connect([this, droneIndex] {
      scheduleMissionStartPhase(2, droneIndex + 1);
      return false;
    }, 900);
  }

  void
  schedulePatrolLandSequence(size_t droneIndex)
  {
    if (droneIndex >= m_droneIds.size()) {
      m_status.set_text("Stop Patrol sequence sent to patrol drones");
      return;
    }
    const auto droneId = m_droneIds[droneIndex];
    m_status.set_text("Stop Patrol: landing Drone " + droneId);
    m_runtime.sendMavlinkCommandToDrone(droneId, "land");
    Glib::signal_timeout().connect([this, droneIndex] {
      schedulePatrolLandSequence(droneIndex + 1);
      return false;
    }, 1800);
  }

  std::pair<double, double>
  mapEventToImagePixel(double pixelX, double pixelY)
  {
    const auto allocation = m_mapImage.get_allocation();
    const auto width = std::max(1, allocation.get_width());
    const auto height = std::max(1, allocation.get_height());
    const auto imageX = std::clamp(pixelX - static_cast<double>(allocation.get_x()),
                                   0.0, static_cast<double>(width - 1));
    const auto imageY = std::clamp(pixelY - static_cast<double>(allocation.get_y()),
                                   0.0, static_cast<double>(height - 1));
    return {
      imageX * 256.0 / static_cast<double>(width),
      imageY * 256.0 / static_cast<double>(height)
    };
  }

  void
  beginMapDrag(double pixelX, double pixelY)
  {
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    m_mapDragging = true;
    m_mapDragStartX = imagePixel.first;
    m_mapDragStartY = imagePixel.second;
    const auto [tileX, tileY] = tileFloatForLatLon(m_mapCenterLat, m_mapCenterLon, m_mapZoom);
    m_mapDragStartTileX = tileX;
    m_mapDragStartTileY = tileY;
  }

  void
  finishMapDrag(double pixelX, double pixelY)
  {
    if (!m_mapDragging) {
      return;
    }
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    m_mapDragging = false;
    const auto dx = imagePixel.first - m_mapDragStartX;
    const auto dy = imagePixel.second - m_mapDragStartY;
    if (dx * dx + dy * dy < 36.0) {
      placePatrolCenterFromMapClick(imagePixel.first, imagePixel.second);
      return;
    }
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Map panned. Click to append a waypoint, or press Center GS.");
    refreshMapTile();
  }

  void
  updateMapDrag(double pixelX, double pixelY)
  {
    const auto imagePixel = mapEventToImagePixel(pixelX, pixelY);
    const auto dx = imagePixel.first - m_mapDragStartX;
    const auto dy = imagePixel.second - m_mapDragStartY;
    if (dx * dx + dy * dy < 16.0) {
      return;
    }
    const auto newTileX = m_mapDragStartTileX - dx / 256.0;
    const auto newTileY = m_mapDragStartTileY - dy / 256.0;
    const auto [lat, lon] = latLonForTileFloat(newTileX, newTileY, m_mapZoom);
    m_mapCenterLat = lat;
    m_mapCenterLon = lon;
    refreshMapTile();
  }

  void
  zoomMap(int delta)
  {
    m_mapZoom = std::clamp(m_mapZoom + delta, 2, 19);
    refreshMapTile();
  }

  void
  centerMapOnGroundStation()
  {
    m_mapCenterLat = m_groundStationLat;
    m_mapCenterLon = m_groundStationLon;
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "Map centered on GS / University of Memphis.\n"
                          "Click to append mission waypoints, drag to pan, +/- to zoom.");
    refreshMapTile();
  }

  void
  placePatrolCenterFromMapClick(double pixelX, double pixelY)
  {
    const auto latLon = latLonForTileFloat(
      (m_mapSourcePixelX + std::clamp(pixelX, 0.0, 255.0)) / 256.0,
      (m_mapSourcePixelY + std::clamp(pixelY, 0.0, 255.0)) / 256.0,
      m_mapZoom);
    m_planWaypoints.push_back(latLon);
    updatePatrolInputsFromWaypoints();
    std::ostringstream pointText;
    pointText << std::fixed << std::setprecision(6)
              << latLon.first << "," << latLon.second;
    m_mapMission.set_text("Map / mission workspace\n\n"
                          "GS center: University of Memphis\n"
                          "Added WP" + std::to_string(m_planWaypoints.size()) +
                          " at " + pointText.str() + "\n"
                          "Upload Patrol Mission sends this route; Undo/Clear edits it.");
    refreshMapTile();
  }

  void
  refreshMapTile()
  {
    const int zoom = m_mapZoom;
    const auto [xFloat, yFloat] = tileFloatForLatLon(m_mapCenterLat, m_mapCenterLon, zoom);
    const auto centerPixelX = xFloat * 256.0;
    const auto centerPixelY = yFloat * 256.0;
    const auto sourcePixelX = centerPixelX - 128.0;
    const auto sourcePixelY = centerPixelY - 128.0;
    const auto baseTileX = static_cast<int>(std::floor(sourcePixelX / 256.0));
    const auto baseTileY = static_cast<int>(std::floor(sourcePixelY / 256.0));
    const auto cropX = std::clamp(static_cast<int>(std::round(sourcePixelX - baseTileX * 256.0)), 0, 255);
    const auto cropY = std::clamp(static_cast<int>(std::round(sourcePixelY - baseTileY * 256.0)), 0, 255);
    m_mapTileX = baseTileX;
    m_mapTileY = baseTileY;
    m_mapSourcePixelX = sourcePixelX;
    m_mapSourcePixelY = sourcePixelY;
    const auto generation = ++m_mapRenderGeneration;
    const auto markers = currentMapMarkers();
    const auto tileKey = std::to_string(zoom) + "/" + std::to_string(baseTileX) +
                         "/" + std::to_string(baseTileY) + "/" +
                         std::to_string(cropX / 8) + "/" + std::to_string(cropY / 8);
    if (m_mapTileLoading.load()) {
      m_mapRefreshPending = true;
      return;
    }
    m_mapRefreshPending = false;
    m_mapTileKey = tileKey;
    m_mapTileLoading = true;
    std::thread([this, zoom, baseTileX, baseTileY, cropX, cropY,
                 sourcePixelX, sourcePixelY, markers, generation] {
      struct LoadedTile
      {
        int x = 0;
        int y = 0;
        std::string path;
      };

      std::vector<LoadedTile> tiles;
      bool ok = true;
      for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
          const auto tileX = baseTileX + dx;
          const auto tileY = baseTileY + dy;
          const auto tileKey = std::to_string(zoom) + "/" + std::to_string(tileX) +
                               "/" + std::to_string(tileY);
          auto tilePath = "NDNSF-UAV-APP/maps/osm/" + std::to_string(zoom) +
                          "/" + std::to_string(tileX) + "/" +
                          std::to_string(tileY) + ".png";
          auto tileOk = access(tilePath.c_str(), R_OK) == 0;
          if (!tileOk) {
            tilePath = "/tmp/ndnsf-uav-map-" + std::to_string(zoom) + "-" +
                       std::to_string(tileX) + "-" + std::to_string(tileY) + ".png";
            tileOk = access(tilePath.c_str(), R_OK) == 0;
          }
          if (!tileOk) {
            const auto url = "https://tile.openstreetmap.org/" + tileKey + ".png";
            const auto command = "curl -fsSL --max-time 5 -A ndnsf-uav-app -o " +
                                 shellQuote(tilePath) + " " + shellQuote(url);
            tileOk = std::system(command.c_str()) == 0;
          }
          ok = ok && tileOk;
          if (tileOk) {
            tiles.push_back({tileX, tileY, tilePath});
          }
        }
      }
      Glib::signal_idle().connect_once([this, tiles, ok, baseTileX, baseTileY,
                                        cropX, cropY, sourcePixelX, sourcePixelY,
                                        markers, zoom, generation] {
        m_mapTileLoading = false;
        auto refreshPending = [this] {
          if (m_mapRefreshPending.exchange(false)) {
            refreshMapTile();
          }
        };
        if (generation != m_mapRenderGeneration.load()) {
          refreshPending();
          return;
        }
        if (!ok) {
          refreshPending();
          return;
        }
        try {
          auto canvas = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 512, 512);
          canvas->fill(0xffffffff);
          for (const auto& tile : tiles) {
            auto pixbuf = Gdk::Pixbuf::create_from_file(tile.path, 256, 256, true);
            if (pixbuf) {
              pixbuf->copy_area(0, 0, 256, 256, canvas,
                                (tile.x - baseTileX) * 256,
                                (tile.y - baseTileY) * 256);
            }
          }
          auto marked = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 256, 256);
          canvas->copy_area(cropX, cropY, 256, 256, marked, 0, 0);
            for (const auto& marker : markers) {
              const auto [mxFloat, myFloat] = tileFloatForLatLon(marker.lat, marker.lon, zoom);
            const auto markerX = static_cast<int>(std::round(mxFloat * 256.0 - sourcePixelX));
            const auto markerY = static_cast<int>(std::round(myFloat * 256.0 - sourcePixelY));
            if (markerX < -24 || markerY < -24 || markerX > 280 || markerY > 280) {
              continue;
            }
              drawMapMarker(marked, markerX, markerY, marker.label, marker.r, marker.g, marker.b);
            }
            m_mapImage.set(marked);
        }
        catch (const Glib::Error& e) {
          NDN_LOG_WARN("GS_MAP_TILE_LOAD_FAILED " << e.what());
        }
        refreshPending();
      });
    }).detach();
  }

private:
  GroundStationServiceContainer& m_runtime;
  Gtk::Box m_box;
  Gtk::Box m_buttons;
  Gtk::Box m_patrolControls;
  Gtk::Box m_workspace;
  Gtk::Frame m_vehicleFrame;
  Gtk::Box m_vehiclePanel;
  Gtk::Label m_vehicleHint;
  Gtk::ListBox m_vehicleList;
  Gtk::Frame m_centerFrame;
  Gtk::Box m_centerPanel;
  Gtk::Box m_mapControls;
  Gtk::EventBox m_mapEventBox;
  Gtk::Image m_mapImage;
  Gtk::Label m_mapMission;
  Gtk::Frame m_statusFrame;
  Gtk::Box m_statusPanel;
  Gtk::Button m_start;
  Gtk::Button m_stop;
  Gtk::Button m_arm;
  Gtk::Button m_takeoff;
  Gtk::Button m_land;
  Gtk::Button m_patrol;
  Gtk::Button m_startMission;
  Gtk::Button m_stopPatrol;
  Gtk::Button m_controlToggle;
  Gtk::Button m_mapZoomIn;
  Gtk::Button m_mapZoomOut;
  Gtk::Button m_mapCenterGs;
  Gtk::Button m_mapUndoWp;
  Gtk::Button m_mapClearWp;
  Gtk::Label m_patrolHint;
  Gtk::Entry m_patrolLat;
  Gtk::Entry m_patrolLon;
  Gtk::Entry m_patrolSizeMeters;
  Gtk::Box m_controlPanel;
  Gtk::Box m_manualKeyRow;
  Gtk::Box m_commandKeyRow;
  Gtk::Label m_controlHelp;
  Gtk::Button m_keyW;
  Gtk::Button m_keyA;
  Gtk::Button m_keyS;
  Gtk::Button m_keyD;
  Gtk::Button m_keyQ;
  Gtk::Button m_keyE;
  Gtk::Button m_keyR;
  Gtk::Button m_keyF;
  Gtk::Button m_keyI;
  Gtk::Button m_keyT;
  Gtk::Button m_keyL;
  Gtk::Button m_keyV;
  Gtk::Button m_keyX;
  Gtk::Label m_status;
  Gtk::Label m_services;
  Gtk::Label m_telemetry;
  Gtk::Image m_image;
  Gtk::Label m_stats;
  Glib::Dispatcher m_statusDispatcher;
  Glib::Dispatcher m_frameDispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
  std::string m_pendingTelemetry;
  std::string m_pendingMap;
  std::string m_pendingMapLat = "35.1186";
  std::string m_pendingMapLon = "-89.9375";
  std::string m_mapTileKey;
  std::atomic<bool> m_mapTileLoading{false};
  std::atomic<bool> m_mapRefreshPending{false};
  std::atomic<uint64_t> m_mapRenderGeneration{0};
  const double m_groundStationLat = 35.1186;
  const double m_groundStationLon = -89.9375;
  double m_mapCenterLat = 35.1186;
  double m_mapCenterLon = -89.9375;
  int m_mapZoom = 15;
  int m_mapTileX = 0;
  int m_mapTileY = 0;
  double m_mapSourcePixelX = 0.0;
  double m_mapSourcePixelY = 0.0;
  bool m_mapDragging = false;
  double m_mapDragStartX = 0.0;
  double m_mapDragStartY = 0.0;
  double m_mapDragStartTileX = 0.0;
  double m_mapDragStartTileY = 0.0;
  bool m_hasPatrolCenter = false;
  double m_patrolCenterLat = 35.1186;
  double m_patrolCenterLon = -89.9375;
  std::vector<std::pair<double, double>> m_planWaypoints;
  std::map<std::string, std::pair<double, double>> m_dronePositions;
  size_t m_telemetryPollIndex = 0;
  Glib::RefPtr<Gdk::Pixbuf> m_pendingPixbuf;
  uint64_t m_pendingSeq = 0;
  uint64_t m_pendingElapsedMs = 0;
  bool m_pendingButtonState = false;
  bool m_pendingStartSensitive = true;
  bool m_pendingStopSensitive = false;
  bool m_pendingClearFrame = false;
  bool m_controlMode = false;
  std::set<guint> m_pressedKeys;
  std::thread m_manualControlThread;
  std::atomic<bool> m_manualControlDone{false};
  std::atomic<bool> m_manualActive{false};
  std::atomic<int> m_manualX{0};
  std::atomic<int> m_manualY{0};
  std::atomic<int> m_manualZ{500};
  std::atomic<int> m_manualR{0};
  uint64_t m_streamGeneration = 0;
  bool m_acceptFrames = false;
  std::atomic<uint64_t> m_decodedFrames{0};
  std::vector<std::string> m_droneIds;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const bool objectDetectionMode = hasFlag(argc, argv, "--serve-object-detection");
	    const bool autoStart = hasFlag(argc, argv, "--auto-video-test");
	    const bool autoMavlinkTest = hasFlag(argc, argv, "--auto-mavlink-test");
	    const bool autoKeyboardTest = hasFlag(argc, argv, "--auto-keyboard-test");
	    const bool autoManualControlTest = hasFlag(argc, argv, "--auto-manual-control-test");
	    const bool autoTwoDroneSwitchTest = hasFlag(argc, argv, "--auto-two-drone-switch-test");
	    const bool autoPatrolTest = hasFlag(argc, argv, "--auto-patrol-test");
	    const bool autoSingleMissionTest = hasFlag(argc, argv, "--auto-single-mission-test");
	    const bool autoSingleMissionStartTest = hasFlag(argc, argv, "--auto-single-mission-start-test");
	    const int autoStopSeconds = std::stoi(getOption(argc, argv, "--auto-stop-seconds", "10"));
	    const int autoStartDelayMs = std::stoi(getOption(argc, argv, "--auto-start-delay-ms", "3000"));
    const std::string targetDroneId = getOption(argc, argv, "--target-drone", "A");
    auto patrolDroneIds = splitCsv(getOption(argc, argv, "--patrol-drones", targetDroneId));
    const int ackTimeoutMs = std::stoi(getOption(argc, argv, "--ack-timeout-ms", "500"));
    const int timeoutMs = std::stoi(getOption(argc, argv, "--timeout-ms", "10000"));
    const auto videoBitrateKbps = static_cast<uint64_t>(
      std::stoull(getOption(argc, argv, "--video-bitrate-kbps", "8000")));
    const auto videoFrameWidth = static_cast<uint64_t>(
      std::stoull(getOption(argc, argv, "--video-width", "480")));
    const std::string yoloModel = getOption(argc, argv, "--yolo-model", "yolo26n.pt");
    const std::string yoloScript = getOption(argc, argv, "--yolo-script",
                                             "NDNSF-UAV-APP/tools/yolo_detect_once.py");
    const std::string yoloWorkerScript = getOption(argc, argv, "--yolo-worker-script",
                                                   "NDNSF-UAV-APP/tools/yolo_detect_worker.py");
    UavRuntimeConfig config;
    config.groupPrefix = ndn::Name(getOption(argc, argv, "--group-prefix", config.groupPrefix.toUri()));
    config.controllerPrefix = ndn::Name(getOption(argc, argv, "--controller-prefix", config.controllerPrefix.toUri()));
    config.groundStationIdentity = ndn::Name(getOption(argc, argv, "--ground-station-identity", config.groundStationIdentity.toUri()));
    config.droneIdentityPrefix = ndn::Name(getOption(argc, argv, "--drone-prefix", config.droneIdentityPrefix.toUri()));
    config.trustSchema = getOption(argc, argv, "--trust-schema", config.trustSchema);
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

    const bool interactiveGui = !(autoStart || autoMavlinkTest || autoKeyboardTest ||
                                  autoManualControlTest || autoTwoDroneSwitchTest ||
                                  autoPatrolTest || autoSingleMissionTest);
    if (interactiveGui && !hasFlag(argc, argv, "--no-cert-dialog") &&
        !hasOption(argc, argv, "--ground-station-identity")) {
      config.groundStationIdentity = chooseGroundStationIdentity(config.groundStationIdentity);
    }

    auto runtime = std::make_unique<GroundStationServiceContainer>(
      serveCertificates, ackTimeoutMs, timeoutMs, config, targetDroneId,
      videoBitrateKbps, videoFrameWidth, patrolDroneIds, yoloModel, yoloScript,
      yoloWorkerScript);
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
	    GroundStationWindow window(*runtime, autoStart, autoStopSeconds,
                                 autoStartDelayMs, autoMavlinkTest,
                                 autoKeyboardTest, autoManualControlTest,
                                 autoTwoDroneSwitchTest,
                                 patrolDroneIds);
    NDN_LOG_INFO("UavGroundStationApp GUI ready");
    std::cout << "GS_GUI_READY target_drone=" << targetDroneId
              << " auto_video_test=" << (autoStart ? "true" : "false")
              << " auto_mavlink_test=" << (autoMavlinkTest ? "true" : "false")
              << " auto_keyboard_test=" << (autoKeyboardTest ? "true" : "false")
              << " auto_manual_control_test=" << (autoManualControlTest ? "true" : "false")
              << " auto_two_drone_switch_test=" << (autoTwoDroneSwitchTest ? "true" : "false")
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
