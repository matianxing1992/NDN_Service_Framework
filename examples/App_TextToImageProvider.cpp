#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

NDN_LOG_INIT(ndn_service_framework.AppTextToImageProvider);

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
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
    const int delayMs = std::max(0, parseIntOption(argc, argv, "--delay-ms", 5000));
    const int handlerThreads = std::max(1, parseIntOption(argc, argv, "--handler-threads", 1));
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const bool statusQueryable = !hasFlag(argc, argv, "--no-selection-status-query");
    const ndn::Name serviceName(getOption(argc, argv, "--service", SERVICE_NAME.toUri()));
    const ndn::Name providerIdentity(
      getOption(argc, argv, "--provider-identity", PROVIDER_IDENTITY.toUri()));
    const ndn::Name controllerPrefix(
      getOption(argc, argv, "--controller-prefix", CONTROLLER_PREFIX.toUri()));

    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, providerCert.getName());
    }

    ndn_service_framework::ServiceProvider provider(
      face,
      GROUP_PREFIX,
      providerCert,
      controllerCert,
      "examples/trust-schema.conf");
    provider.setHandlerThreads(static_cast<size_t>(handlerThreads));
    provider.addService(
      serviceName,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [] (const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "text-to-image provider ready";
          return decision;
        }),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [delayMs] (const ndn::Name&,
                   const ndn::Name&,
                   const ndn::Name& serviceName,
                   const ndn::Name& requestId,
                   const ndn_service_framework::RequestMessage& request) {
          const auto payload = request.getPayload();
          const std::string prompt(reinterpret_cast<const char*>(payload.data()),
                                   payload.size());
          NDN_LOG_INFO("TEXT_TO_IMAGE_GENERATION_START requestId="
                       << requestId.toUri()
                       << " service=" << serviceName.toUri()
                       << " prompt=" << prompt
                       << " delayMs=" << delayMs);
          std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

          const std::string responseText =
            "mock-image-result prompt=\"" + prompt + "\"";
          ndn::Buffer responsePayload(
            reinterpret_cast<const uint8_t*>(responseText.data()),
            responseText.size());
          ndn_service_framework::ResponseMessage response;
          response.setStatus(true);
          response.setPayload(responsePayload, responsePayload.size());
          NDN_LOG_INFO("TEXT_TO_IMAGE_GENERATION_DONE requestId="
                       << requestId.toUri());
          return response;
        }));
    provider.setSelectionStatusQueryable(serviceName, statusQueryable);
    provider.init();
    provider.fetchPermissionsFromController(controllerPrefix);

    std::cout << "TEXT_TO_IMAGE_PROVIDER_READY service=" << serviceName.toUri()
              << " delayMs=" << delayMs
              << " selectionStatusQueryable=" << (statusQueryable ? 1 : 0)
              << std::endl;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_TextToImageProvider error: " << e.what() << std::endl;
    return 1;
  }
}
