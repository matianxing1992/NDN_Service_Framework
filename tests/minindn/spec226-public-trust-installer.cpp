#include "ndn-cxx/security/certificate.hpp"
#include "ndn-cxx/security/key-chain.hpp"
#include "ndn-cxx/util/io.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int
usage(const char* program)
{
  std::cerr << "Usage: " << program
            << " PIB_DIRECTORY TPM_LOCATOR CERTIFICATE...\n";
  return 2;
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc < 4) {
    return usage(argv[0]);
  }

  try {
    const std::string expectedPibLocator = "pib-sqlite3:" + std::string(argv[1]);
    const std::string expectedTpmLocator = argv[2];
    ndn::security::KeyChain keyChain(expectedPibLocator, expectedTpmLocator);
    // This test-only PIB API inserts public keys without generating peer private keys.
    auto& pib = const_cast<ndn::security::Pib&>(keyChain.getPib());
    if (pib.getPibLocator() != expectedPibLocator ||
        pib.getTpmLocator() != expectedTpmLocator) {
      throw std::runtime_error("PIB or TPM locator does not match the actor keychain");
    }
    const auto defaultIdentity = pib.getDefaultIdentity().getName();

    for (int i = 3; i < argc; ++i) {
      std::ifstream input(argv[i]);
      if (!input) {
        throw std::runtime_error("cannot open certificate: " + std::string(argv[i]));
      }

      auto certificate = ndn::io::loadTlv<ndn::security::Certificate>(input);
      auto identity = pib.addIdentity(certificate.getIdentity());
      auto key = identity.addKey(certificate.getPublicKey(), certificate.getKeyName());
      key.addCertificate(certificate);
      std::cout << "SPEC226_PUBLIC_CERT_INSTALLED identity="
                << certificate.getIdentity() << '\n';
    }
    if (pib.getDefaultIdentity().getName() != defaultIdentity) {
      throw std::runtime_error("public certificate import changed the default identity");
    }
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC226_PUBLIC_CERT_INSTALL_FAILED error=" << error.what() << '\n';
    return 1;
  }

  return 0;
}
