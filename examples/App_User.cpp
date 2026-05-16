#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

int
parseMetadataInt(const std::string& payload, const std::string& key, int fallback)
{
  const std::string marker = key + "=";
  const auto start = payload.find(marker);
  if (start == std::string::npos) {
    return fallback;
  }

  const auto valueStart = start + marker.size();
  const auto valueEnd = payload.find(';', valueStart);
  const auto value = payload.substr(valueStart, valueEnd - valueStart);
  try {
    return std::stoi(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
providerLabel(const ndn::Name& providerName)
{
  if (providerName.empty()) {
    return "";
  }
  return providerName[-1].toUri();
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    KeyChainInitLock keyChainInitLock("/tmp/ndnsf-keychain-init.lock");
    ndn::KeyChain keyChain;
    ndn::Scheduler scheduler(face.getIoContext());

    const bool useCustomSelection = hasFlag(argc, argv, "--custom-selection");
    const int ackTimeoutMs = parseIntOption(argc, argv, "--ack-timeout-ms", 3000);
    const std::string expectedResponse = getOption(argc, argv, "--expect-response", "");

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(USER_IDENTITY));
    getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
    keyChainInitLock.unlock();

    ndn_service_framework::ServiceUser user(
      face,
      GROUP_PREFIX,
      userCert,
      controllerCert,
      "examples/trust-any.conf");

    user.init();
    user.fetchPermissionsFromController(CONTROLLER_PREFIX);

    scheduler.schedule(ndn::time::seconds(2), [&] {
      std::cout << "Sending HELLO request..." << std::endl;
      std::cout << "[App_User] selected providerName="
                << PROVIDER_IDENTITY.toUri() << std::endl;
      std::cout << "[App_User] selected serviceName=/HELLO" << std::endl;
      std::cout << "[App_User] final request name="
                   "/example/hello/user/NDNSF/REQUEST/1/HELLO/<bloomFilter>/<requestId>"
                << std::endl;

      const std::string requestText = "HELLO";
      ndn::Buffer requestPayload(
        reinterpret_cast<const uint8_t*>(requestText.data()),
        requestText.size());

      ndn_service_framework::RequestMessage request;
      request.setPayload(requestPayload, requestPayload.size());
      request.setStrategy(ndn_service_framework::tlv::FirstResponding);

      auto onTimeout = std::function<void(const ndn::Name&)>([&](const ndn::Name&) {
        std::cerr << "HELLO request timed out" << std::endl;
        face.getIoContext().stop();
      });
      auto onResponse = std::function<void(const ndn_service_framework::ResponseMessage&)>(
        [&](const ndn_service_framework::ResponseMessage& response) {
          const auto payload = response.getPayload();
          const std::string responseText(
            reinterpret_cast<const char*>(payload.data()),
            payload.size());
          std::cout << "Received response: " << responseText << std::endl;
          if (!expectedResponse.empty() && responseText == expectedResponse) {
            std::cout << "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS" << std::endl;
          }
          face.getIoContext().stop();
        });

      if (useCustomSelection) {
        const std::vector<ndn::Name> providers = {
          ndn::Name(PROVIDER_IDENTITY).append("A"),
          ndn::Name(PROVIDER_IDENTITY).append("B"),
          ndn::Name(PROVIDER_IDENTITY).append("C")
        };
        user.async_call(
          providers,
          ndn::Name("/HELLO"),
          request,
          ackTimeoutMs,
          ndn_service_framework::ServiceUser::AckCandidatesHandler(
            [ackTimeoutMs](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
              std::cout << "customSelectionStrategy ran after ackTimeoutMs="
                        << ackTimeoutMs
                        << " candidateCount=" << candidates.size() << std::endl;

              std::vector<ndn_service_framework::AckSelectionCandidate> selected;
              int bestRank = std::numeric_limits<int>::max();
              int bestQueue = std::numeric_limits<int>::max();
              for (const auto& candidate : candidates) {
                const auto payload = candidate.ack.getPayload();
                const std::string payloadText(
                  reinterpret_cast<const char*>(payload.data()),
                  payload.size());
                std::cout << "customSelectionStrategy candidate providerName="
                          << candidate.providerName.toUri()
                          << " status=" << candidate.ack.getStatus()
                          << " message=" << candidate.ack.getMessage()
                          << " payload=" << payloadText << std::endl;
                std::cout << "customSelectionStrategy ACK received timestampMs="
                          << nowMilliseconds()
                          << " providerName=" << candidate.providerName.toUri()
                          << std::endl;

                if (!candidate.ack.getStatus()) {
                  std::cout << "customSelectionStrategy rejected provider="
                            << providerLabel(candidate.providerName)
                            << " status=0" << std::endl;
                  continue;
                }

                const int rank = parseMetadataInt(payloadText, "rank",
                                                  std::numeric_limits<int>::max());
                const int queue = parseMetadataInt(payloadText, "queue",
                                                   std::numeric_limits<int>::max());
                std::cout << "collected ACK payload provider="
                          << providerLabel(candidate.providerName)
                          << " queue=" << queue
                          << " rank=" << rank << std::endl;
                if (selected.empty() ||
                    rank < bestRank ||
                    (rank == bestRank && queue < bestQueue)) {
                  selected.clear();
                  selected.push_back(candidate);
                  bestRank = rank;
                  bestQueue = queue;
                }
              }

              if (!selected.empty()) {
                std::cout << "customSelectionStrategy selected providerName="
                          << selected.front().providerName.toUri() << std::endl;
              }
              return selected;
            }),
          20000,
          onTimeout,
          onResponse);
      }
      else {
        user.async_call(
          ndn::Name("/HELLO"),
          request,
          ackTimeoutMs,
          ndn_service_framework::ServiceUser::AckCandidatesHandler(
            [](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
            std::vector<ndn_service_framework::AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              const auto payload = candidate.ack.getPayload();
              const std::string payloadText(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());
              std::cout << "[App_User] collected ACK provider="
                        << candidate.providerName.toUri()
                        << " status=" << candidate.ack.getStatus()
                        << " message=" << candidate.ack.getMessage()
                        << " payload=" << payloadText << std::endl;

              if (selected.empty() &&
                  candidate.ack.getStatus() &&
                  payloadText.find("model=hello-v1") != std::string::npos) {
                selected.push_back(candidate);
              }
            }
            if (selected.empty() && !candidates.empty()) {
              selected.push_back(candidates.front());
            }
            return selected;
            }),
          20000,
          onTimeout,
          onResponse);
      }
    });

    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_User error: " << e.what() << std::endl;
    return 1;
  }
}
