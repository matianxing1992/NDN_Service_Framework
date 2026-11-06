#ifndef NDNSF_UAV_GROUND_STATION_SERVICE_CONTAINER_HPP
#define NDNSF_UAV_GROUND_STATION_SERVICE_CONTAINER_HPP

#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "../shared/UavVideoPipeline.hpp"
#include "../shared/UavSensorStreams.hpp"
#include "../shared/UavMissionSession.hpp"
#include "../shared/UavDetectorProvider.hpp"
#include "../shared/UavCollaborationPolicy.hpp"
#include "UavIncidentCoordinator.hpp"
#include "GroundStationRuntimeState.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ServiceContainer.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/StreamFacade.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>
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
#include <exception>
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

} // namespace

#endif // NDNSF_UAV_GROUND_STATION_SERVICE_CONTAINER_HPP
