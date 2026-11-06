#ifndef NDNSF_UAV_DRONE_SERVICE_CONTAINER_HPP
#define NDNSF_UAV_DRONE_SERVICE_CONTAINER_HPP

#include "../shared/UavNames.hpp"
#include "../tracking/uav-tracking-provider.hpp"
#include "drone-callback-gate.hpp"

#include <optional>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include "../shared/UavProtocol.hpp"
#include "../shared/UavVideoPipeline.hpp"
#include "../shared/UavSensorStreams.hpp"
#include "../shared/UavDiagnostics.hpp"
#include "UavCollaborationParticipant.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceContainer.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/StreamFacade.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <boost/asio/post.hpp>

#include <openssl/rand.h>

#include <gtkmm.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netdb.h>
#include <netinet/in.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.examples.UavDroneApp);

using namespace ndnsf::examples::uav;
using namespace std::chrono_literals;

volatile std::sig_atomic_t g_stopHeadless = 0;

void
handleStopSignal(int)
{
  g_stopHeadless = 1;
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
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

bool
fileExists(const std::string& path)
{
  struct stat st {};
  return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string
findCaptureDevice()
{
  for (int i = 0; i < 16; ++i) {
    const auto path = "/dev/video" + std::to_string(i);
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) {
      continue;
    }

    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }

    v4l2_capability cap {};
    const bool ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
    close(fd);
    if (!ok) {
      continue;
    }

    const auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ?
      cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) != 0 ||
        (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0) {
      return path;
    }
  }
  return "";
}

std::string
resolveVideoSource(int argc, char** argv, const Fields& config)
{
  std::string configured;
  if (hasOption(argc, argv, "--video-source")) {
    configured = getOption(argc, argv, "--video-source", "");
  }
  else {
    const auto it = config.find("video-source");
    if (it != config.end()) {
      configured = it->second;
    }
  }

  if (!configured.empty() && configured != "auto") {
    return configured;
  }

  if (const auto device = findCaptureDevice(); !device.empty()) {
    std::cout << "DRONE_CAMERA_SOURCE_AUTO selected=" << device << std::endl;
    return device;
  }

  for (const auto& fallback : {
         std::string("videos/drone.mp4"),
         std::string("NDNSF-UAV-APP/videos/drone.mp4")
       }) {
    if (fileExists(fallback)) {
      std::cout << "DRONE_CAMERA_SOURCE_AUTO selected=" << fallback
                << " reason=no-capture-device" << std::endl;
      return fallback;
    }
  }

  std::cout << "DRONE_CAMERA_SOURCE_AUTO selected=videos/drone.mp4"
            << " reason=no-capture-device-no-sample-found" << std::endl;
  return "videos/drone.mp4";
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

ndn_service_framework::ResponseMessage
makeBinaryResponse(bool status, const std::vector<uint8_t>& payload,
                   const std::string& error = "No error")
{
  ndn::Buffer responsePayload(payload.data(), payload.size());
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


} // namespace

#endif // NDNSF_UAV_DRONE_SERVICE_CONTAINER_HPP
