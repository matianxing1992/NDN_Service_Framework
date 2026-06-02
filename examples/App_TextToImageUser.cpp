#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/asio/steady_timer.hpp>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.AppTextToImageUser);

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name USER_IDENTITY("/example/hello/user");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name SERVICE_NAME("/TextToImage/Generate");

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

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;
    int exitCode = 1;
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const ndn::Name serviceName(getOption(argc, argv, "--service", SERVICE_NAME.toUri()));
    const ndn::Name providerName(getOption(argc, argv, "--provider", PROVIDER_IDENTITY.toUri()));
    const ndn::Name controllerPrefix(
      getOption(argc, argv, "--controller-prefix", CONTROLLER_PREFIX.toUri()));
    const std::string prompt = getOption(argc, argv, "--prompt", "a drone over Memphis");
    const int startupDelayMs = std::max(0, parseIntOption(argc, argv, "--startup-delay-ms", 2000));
    const int timeoutMs = std::max(1, parseIntOption(argc, argv, "--timeout-ms", 2000));
    const int statusIntervalMs =
      std::max(1, parseIntOption(argc, argv, "--status-interval-ms", 500));
    const int statusTimeoutMs =
      std::max(1, parseIntOption(argc, argv, "--status-timeout-ms", 300));

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(USER_IDENTITY));
    getOrCreateIdentity(keyChain, providerName);

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, userCert.getName());
    }

    ndn_service_framework::ServiceUser user(
      face,
      GROUP_PREFIX,
      userCert,
      controllerCert,
      "examples/trust-schema.conf");
    user.init();
    user.fetchPermissionsFromController(controllerPrefix);

    auto timer = std::make_shared<boost::asio::steady_timer>(face.getIoContext());
    timer->expires_after(std::chrono::milliseconds(startupDelayMs));
    timer->async_wait([&, timer] (const boost::system::error_code& error) {
      if (error) {
        return;
      }

      ndn_service_framework::RequestMessage request;
      ndn::Buffer payload(reinterpret_cast<const uint8_t*>(prompt.data()),
                          prompt.size());
      request.setPayload(payload, payload.size());
      request.setStrategy(ndn_service_framework::tlv::FirstResponding);

      ndn_service_framework::ServiceUser::SelectionStatusOptions options(
        true, statusIntervalMs, statusTimeoutMs);
      const auto requestId = user.RequestServiceTracked(
        std::vector<ndn::Name>{providerName},
        serviceName,
        request,
        timeoutMs,
        [&] (const ndn::Name& timedOutRequestId,
             const std::vector<ndn_service_framework::SelectionExecutionStatus>& statuses) {
          std::cout << "TEXT_TO_IMAGE_TRACKED_TIMEOUT requestId="
                    << timedOutRequestId.toUri()
                    << " statusCount=" << statuses.size() << std::endl;
          for (const auto& status : statuses) {
            std::cout << "TEXT_TO_IMAGE_SELECTION_STATUS"
                      << " provider=" << status.providerName.toUri()
                      << " service=" << status.serviceName.toUri()
                      << " state="
                      << ndn_service_framework::selectionExecutionStateToString(status.state)
                      << " message=\"" << status.message << "\""
                      << " response=" << status.responseName.toUri()
                      << std::endl;
          }
          exitCode = statuses.empty() ? 2 : 0;
          face.getIoContext().stop();
        },
        [&] (const ndn_service_framework::ResponseMessage& response) {
          const auto responsePayload = response.getPayload();
          const std::string text(reinterpret_cast<const char*>(responsePayload.data()),
                                 responsePayload.size());
          std::cout << "TEXT_TO_IMAGE_RESPONSE status=" << response.getStatus()
                    << " payload=" << text << std::endl;
          exitCode = response.getStatus() ? 0 : 3;
          face.getIoContext().stop();
        },
        ndn_service_framework::tlv::FirstResponding,
        options);

      std::cout << "TEXT_TO_IMAGE_REQUEST_SENT requestId=" << requestId.toUri()
                << " service=" << serviceName.toUri()
                << " provider=" << providerName.toUri()
                << " timeoutMs=" << timeoutMs
                << " statusIntervalMs=" << statusIntervalMs
                << std::endl;
    });

    face.processEvents();
    return exitCode;
  }
  catch (const std::exception& e) {
    std::cerr << "App_TextToImageUser error: " << e.what() << std::endl;
    return 1;
  }
}
