#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceController.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>

#include <iostream>
#include <memory>
#include <string>

namespace {

const ndn::Name DEFAULT_CONTROLLER_PREFIX("/example/hello/controller");
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

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;
    ndn::ValidatorConfig validator(face);
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const std::string policyFile = getOption(argc, argv, "--policy-file", "examples/hello.policies");
    const std::string trustSchema = getOption(argc, argv, "--trust-schema", "examples/trust-schema.conf");
    const ndn::Name controllerPrefix(
      getOption(argc, argv, "--controller-prefix", DEFAULT_CONTROLLER_PREFIX.toUri()));

    auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(controllerPrefix));
    getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
    getOrCreateIdentity(keyChain, USER_IDENTITY);

    validator.load(trustSchema);

    std::cout << "[App_ServiceController] authority identity="
              << controllerCert.getIdentity().toUri()
              << " serveCertificates=" << serveCertificates
              << " dkeyPrefix="
              << ndn::Name(controllerCert.getIdentity()).append("DKEY").toUri()
              << std::endl;

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, controllerCert.getName());
    }

    ndn_service_framework::ServiceController controller(
      face,
      controllerCert,
      validator,
      policyFile);
    controller.setControllerPrefix(controllerPrefix);

    std::cout << "ServiceController started..." << std::endl;
    controller.run();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_ServiceController error: " << e.what() << std::endl;
    return 1;
  }
}
