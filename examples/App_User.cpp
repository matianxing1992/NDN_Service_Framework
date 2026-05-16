#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <functional>
#include <iostream>
#include <vector>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name USER_IDENTITY("/example/hello/user");

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
    ndn::Scheduler scheduler(face.getIoContext());

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);

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

      user.async_call(
        ndn::Name("/HELLO"),
        request,
        3000,
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
        std::function<void(const ndn::Name&)>([&](const ndn::Name&) {
          std::cerr << "HELLO request timed out" << std::endl;
          face.getIoContext().stop();
        }),
        std::function<void(const ndn_service_framework::ResponseMessage&)>(
          [&](const ndn_service_framework::ResponseMessage& response) {
            const auto payload = response.getPayload();
            const std::string responseText(
              reinterpret_cast<const char*>(payload.data()),
              payload.size());
            std::cout << "Received response: " << responseText << std::endl;
            face.getIoContext().stop();
          }));
    });

    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_User error: " << e.what() << std::endl;
    return 1;
  }
}
