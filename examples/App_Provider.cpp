#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");

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

bool
parseAckStatus(const std::string& value)
{
  return !(value == "false" || value == "0" || value == "reject" || value == "no");
}

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    KeyChainInitLock keyChainInitLock("/tmp/ndnsf-keychain-init.lock");
    ndn::KeyChain keyChain;

    const std::string providerId = getOption(argc, argv, "--provider-id", "");
    const bool benchmark = hasFlag(argc, argv, "--benchmark");
    const std::string providerLabel = providerId.empty() ? "default" : providerId;
    const ndn::Name providerIdentity = providerId.empty()
      ? PROVIDER_IDENTITY
      : ndn::Name(PROVIDER_IDENTITY).append(providerId);
    const std::string ackPayloadText = getOption(
      argc, argv, "--ack-payload", "queue=0;gpu=idle;model=hello-v1");
    const bool ackStatus = parseAckStatus(
      getOption(argc, argv, "--ack-status", "true"));
    const std::string ackMessage = getOption(
      argc, argv, "--ack-message",
      ackStatus ? "HELLO provider ready" : "HELLO provider rejected");
    const std::string responseText = getOption(
      argc, argv, "--response-payload", "HELLO");

    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));
    keyChainInitLock.unlock();

    std::cout << "[App_Provider] provider identity="
              << providerIdentity.toUri()
              << " providerId=" << providerLabel
              << " benchmark=" << benchmark
              << " ackStatus=" << ackStatus
              << " ackPayload=" << ackPayloadText
              << " responsePayload=" << responseText
              << std::endl;

    KeyChainInitLock routeRegistrationLock("/tmp/ndnsf-provider-route-registration.lock");
    ndn_service_framework::ServiceProvider provider(
      face,
      GROUP_PREFIX,
      providerCert,
      controllerCert,
      "examples/trust-any.conf");
    routeRegistrationLock.unlock();

    provider.addService(
      ndn::Name("/HELLO"),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [providerLabel, ackStatus, ackMessage, ackPayloadText](
          const ndn_service_framework::RequestMessage&) {
          std::cout << "Provider " << providerLabel
                    << " selective ACK handler received request" << std::endl;
          std::cout << "Provider " << providerLabel
                    << " request received timestampMs=" << nowMilliseconds()
                    << std::endl;
          if (!ackStatus) {
            std::cout << "Provider " << providerLabel
                      << " selective ACK handler rejected request" << std::endl;
          }

          const std::string metadata = ackPayloadText;
          ndn::Buffer ackPayload(
            reinterpret_cast<const uint8_t*>(metadata.data()),
            metadata.size());

          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = ackStatus;
          decision.message = ackMessage;
          decision.payload = ackPayload;
          std::cout << "Provider " << providerLabel
                    << " publishing HELLO ACK status=" << decision.status
                    << " message=" << decision.message
                    << " payload=" << metadata << std::endl;
          std::cout << "Publishing HELLO ACK payload: " << metadata << std::endl;
          return decision;
        }),
      std::function<ndn_service_framework::ResponseMessage(
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn_service_framework::RequestMessage&)>(
        [providerLabel, responseText](const ndn::Name&,
                                      const ndn::Name&,
                                      const ndn::Name& serviceName,
                                      const ndn::Name&,
                                      const ndn_service_framework::RequestMessage& request) {
          const auto requestPayload = request.getPayload();
          const std::string requestText(
            reinterpret_cast<const char*>(requestPayload.data()),
            requestPayload.size());

          if (requestText != "HELLO") {
            ndn_service_framework::ResponseMessage response;
            response.setStatus(false);
            response.setErrorInfo("Unexpected payload for " + serviceName.toUri());
            return response;
          }

          std::cout << "Received HELLO request" << std::endl;
          std::cout << "Provider " << providerLabel
                    << " executing selected request" << std::endl;
          std::cout << "Provider " << providerLabel
                    << " publishing final response: " << responseText << std::endl;

          ndn::Buffer responsePayload(
            reinterpret_cast<const uint8_t*>(responseText.data()),
            responseText.size());

          ndn_service_framework::ResponseMessage response;
          response.setStatus(true);
          response.setErrorInfo("No error");
          response.setPayload(responsePayload, responsePayload.size());
          return response;
        }));
    provider.init();
    provider.fetchPermissionsFromController(CONTROLLER_PREFIX);

    std::cout << "Provider " << providerLabel
              << " registered service /HELLO" << std::endl;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_Provider error: " << e.what() << std::endl;
    return 1;
  }
}
