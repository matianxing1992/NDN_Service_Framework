#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name USER_IDENTITY("/example/hello/user");

class KeyChainInitLock
{
public:
  explicit KeyChainInitLock(const char* path)
  {
    m_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
      throw std::runtime_error("Failed to acquire KeyChain initialization lock");
    }
  }

  ~KeyChainInitLock()
  {
    unlock();
  }

  void
  unlock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
      m_fd = -1;
    }
  }

private:
  int m_fd = -1;
};

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

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

int
parseIntOption(int argc, char** argv, const std::string& option, int fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

double
parseDoubleOption(int argc, char** argv, const std::string& option, double fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stod(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

ndn::Buffer
makeBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string
payloadToString(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    const ndn::Name serviceName(getOption(argc, argv, "--service", "/HELLO"));
    const double rateRps = parseDoubleOption(argc, argv, "--rate-rps", 50.0);
    const int durationMs = parseIntOption(argc, argv, "--duration-ms", 60000);
    const int startupDelayMs = parseIntOption(argc, argv, "--startup-delay-ms", 2500);
    const int ackTimeoutMs = parseIntOption(argc, argv, "--ack-timeout-ms", 200);
    const int requestTimeoutMs = parseIntOption(argc, argv, "--timeout-ms", 5000);
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");

    ndn::Face face;
    ndn::Scheduler scheduler(face.getIoContext());
    ndn::KeyChain keyChain;

    KeyChainInitLock keyLock("/tmp/ndnsf-intermittent-user-keychain.lock");
    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(USER_IDENTITY));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
    keyLock.unlock();

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, userCert.getName());
    }

    ndn_service_framework::ServiceUser user(
      face, GROUP_PREFIX, userCert, controllerCert, "examples/trust-any.conf");
    user.init();
    user.setPerformanceMode(true);
    user.setUseTokens(true);
    ndn_service_framework::ServiceUser::AdaptiveAdmissionOptions admission;
    admission.enabled = false;
    user.setAdaptiveAdmissionControl(admission);
    user.fetchPermissionsFromController(CONTROLLER_PREFIX);

    struct Counters
    {
      size_t sent = 0;
      size_t accepted = 0;
      size_t success = 0;
      size_t timeout = 0;
      size_t badResponse = 0;
      std::set<std::string> outstanding;
      std::chrono::steady_clock::time_point firstSend;
      std::chrono::steady_clock::time_point lastSend;
    };
    auto counters = std::make_shared<Counters>();
    auto stoppedSending = std::make_shared<bool>(false);

    const auto interval = ndn::time::nanoseconds(
      static_cast<int64_t>(1000000000.0 / std::max(0.001, rateRps)));
    const auto startAt =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(startupDelayMs);
    const auto stopAt = startAt + std::chrono::milliseconds(durationMs);

    auto finishIfDone = std::make_shared<std::function<void()>>();
    *finishIfDone = [&, counters, stoppedSending] {
      const auto completed =
        counters->success + counters->timeout + counters->badResponse;
      if (*stoppedSending && completed >= counters->accepted) {
        face.getIoContext().stop();
      }
    };

    auto sendOne = std::make_shared<std::function<void()>>();
    *sendOne = [&, counters, stoppedSending, sendOne, finishIfDone, stopAt, interval] {
      const auto now = std::chrono::steady_clock::now();
      if (now >= stopAt) {
        *stoppedSending = true;
        scheduler.schedule(ndn::time::milliseconds(requestTimeoutMs + 1000),
                           [&] { face.getIoContext().stop(); });
        (*finishIfDone)();
        return;
      }

      auto requestPayload = makeBuffer("HELLO");
      ++counters->sent;
      if (counters->accepted == 0) {
        counters->firstSend = now;
      }
      counters->lastSend = now;

      ndn::Name requestId = user.RequestService(
        serviceName,
        requestPayload,
        ackTimeoutMs,
        ndn_service_framework::strategy::FirstResponding,
        requestTimeoutMs,
        [counters, finishIfDone](const ndn_service_framework::ResponseMessage& response) {
          if (response.getStatus() && payloadToString(response) == "HELLO") {
            ++counters->success;
          }
          else {
            ++counters->badResponse;
          }
          (*finishIfDone)();
        },
        [counters, finishIfDone](const ndn_service_framework::RequestId& requestId) {
          ++counters->timeout;
          counters->outstanding.erase(requestId.toUri());
          (*finishIfDone)();
        });

      if (!requestId.empty()) {
        ++counters->accepted;
        counters->outstanding.insert(requestId.toUri());
      }
      scheduler.schedule(interval, [sendOne] { (*sendOne)(); });
    };

    std::cout << "INTERMITTENT_USER_READY service=" << serviceName.toUri()
              << " rateRps=" << rateRps
              << " durationMs=" << durationMs
              << " ackTimeoutMs=" << ackTimeoutMs
              << " timeoutMs=" << requestTimeoutMs
              << " adaptiveAdmission=disabled"
              << std::endl;

    scheduler.schedule(ndn::time::milliseconds(startupDelayMs),
                       [sendOne] { (*sendOne)(); });
    face.processEvents();

    const double seconds =
      std::max(0.001, std::chrono::duration<double>(
        counters->lastSend - counters->firstSend).count());
    const auto completed = counters->success + counters->timeout + counters->badResponse;
    const double successRate =
      counters->sent == 0 ? 0.0 : 100.0 * static_cast<double>(counters->success) /
                                  static_cast<double>(counters->sent);
    const double timeoutRate =
      counters->sent == 0 ? 0.0 : 100.0 * static_cast<double>(counters->timeout) /
                                  static_cast<double>(counters->sent);

    std::cout << std::fixed << std::setprecision(2)
              << "INTERMITTENT_USER_SUMMARY"
              << " offered_rps=" << rateRps
              << " actual_rps=" << (static_cast<double>(counters->accepted) / seconds)
              << " sent=" << counters->sent
              << " accepted=" << counters->accepted
              << " completed=" << completed
              << " success=" << counters->success
              << " timeout=" << counters->timeout
              << " bad_response=" << counters->badResponse
              << " success_rate=" << successRate
              << " timeout_rate=" << timeoutRate
              << std::endl;
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_IntermittentUser error: " << e.what() << std::endl;
    return 1;
  }
}
