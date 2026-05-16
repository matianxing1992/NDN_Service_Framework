#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <functional>
#include <iostream>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");

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

} // namespace

int
main()
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;

    auto providerCert = getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);

    std::cout << "[App_Provider] provider identity="
              << PROVIDER_IDENTITY.toUri() << std::endl;

    ndn_service_framework::ServiceProvider provider(
      face,
      GROUP_PREFIX,
      providerCert,
      controllerCert,
      "examples/trust-any.conf");

    provider.addService(
      ndn::Name("/HELLO"),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [](const ndn_service_framework::RequestMessage&) {
          const std::string metadata = "queue=0;gpu=idle;model=hello-v1";
          ndn::Buffer ackPayload(
            reinterpret_cast<const uint8_t*>(metadata.data()),
            metadata.size());

          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "HELLO provider ready";
          decision.payload = ackPayload;
          std::cout << "Publishing HELLO ACK payload: " << metadata << std::endl;
          return decision;
        }),
      std::function<ndn_service_framework::ResponseMessage(
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn_service_framework::RequestMessage&)>(
        [](const ndn::Name&,
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

          const std::string responseText = "HELLO";
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

    std::cout << "Provider registered service /HELLO" << std::endl;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_Provider error: " << e.what() << std::endl;
    return 1;
  }
}
