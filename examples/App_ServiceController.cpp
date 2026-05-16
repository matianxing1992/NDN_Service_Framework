#include "ndn-service-framework/ServiceController.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>

#include <iostream>

namespace {

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
    ndn::ValidatorConfig validator(face);

    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(CONTROLLER_PREFIX));
    getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
    getOrCreateIdentity(keyChain, USER_IDENTITY);

    validator.load("examples/trust-any.conf");

    ndn_service_framework::ServiceController controller(
      face,
      controllerCert,
      validator,
      "examples/hello.policies");
    controller.setControllerPrefix(CONTROLLER_PREFIX);

    std::cout << "ServiceController started..." << std::endl;
    controller.run();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_ServiceController error: " << e.what() << std::endl;
    return 1;
  }
}
