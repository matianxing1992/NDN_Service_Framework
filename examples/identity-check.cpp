#include "ndn-service-framework/trust-schema-resources.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/random.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

int
main(int argc, char** argv)
{
  try {
    if (argc == 3 && std::string(argv[1]) == "--trust-resources") {
      constexpr size_t MAX_RESOURCE_BYTES =
        ndn_service_framework::MAX_TRUST_SCHEMA_INSPECTION_BYTES;
      const auto resources = ndn_service_framework::enumerateTrustSchemaResources(argv[2]);
      std::string output = "NDNSF_TRUST_RESOURCES_V1";
      output.push_back('\0');
      for (const auto& resource : resources) {
        if (resource.path.find('\0') != std::string::npos ||
            output.size() > MAX_RESOURCE_BYTES - 3 ||
            resource.path.size() > MAX_RESOURCE_BYTES - output.size() - 3)
          throw std::invalid_argument("trust resource path exceeds inspection byte limit");
        output.push_back(resource.required ? 'R' : 'O');
        output.push_back('\0');
        output.append(resource.path);
        output.push_back('\0');
      }
      std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
      return std::cout ? 0 : 1;
    }
    if (argc != 2 && argc != 3)
      throw std::invalid_argument(
        "usage: ndnsf-identity-check HOME [IDENTITY] | --trust-resources SCHEMA");
    const std::filesystem::path home(argv[1]);
    if (!home.is_absolute() || std::filesystem::canonical(home) != home)
      throw std::invalid_argument("credential HOME must be an absolute canonical directory");
    const auto pib = home / ".ndn";
    const auto tpm = pib / "ndnsec-key-file";
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(pib / "pib.db")) ||
        !std::filesystem::is_directory(std::filesystem::symlink_status(pib)) ||
        !std::filesystem::is_directory(std::filesystem::symlink_status(tpm)))
      throw std::runtime_error("existing credential PIB and TPM directories are required");

    const char* configuredTpm = ::getenv("NDN_CLIENT_TPM");
    const std::string tpmLocator = configuredTpm == nullptr ? "tpm-file:" : configuredTpm;
    if (tpmLocator != "tpm-file:" && tpmLocator != "tpm-file:" + pib.string())
      throw std::invalid_argument("TPM locator must stay within credential HOME");
    // A fresh helper process must avoid ndn-cxx's implicit default-PIB reset path.
    if (::setenv("NDN_CLIENT_PIB", "pib-memory:", 1) != 0 ||
        ::setenv("NDN_CLIENT_TPM", "tpm-memory:", 1) != 0 ||
        ::setenv("HOME", home.c_str(), 1) != 0)
      throw std::runtime_error("cannot isolate KeyChain defaults");
    ndn::security::KeyChain keyChain("pib-sqlite3:" + pib.string(),
                                    tpmLocator, false);
    const auto identity = argc == 3 ? ndn::Name(argv[2]) :
      keyChain.getPib().getDefaultIdentity().getName();
    const auto certificate = keyChain.getPib().getIdentity(identity)
                               .getDefaultKey().getDefaultCertificate();
    if (identity.empty() || certificate.getIdentity() != identity || !certificate.isValid())
      throw std::runtime_error("credential identity or certificate validity does not match");
    ndn::Data proof(ndn::Name(identity).append("credential-check")
                    .appendNumber(ndn::random::generateWord64()));
    keyChain.sign(proof, ndn::security::signingByCertificate(certificate.getName()));
    if (!ndn::security::verifySignature(proof, std::optional<ndn::security::Certificate>(certificate)))
      throw std::runtime_error("credential private key does not match certificate");
    std::cout << "NDNSF_IDENTITY_CHECKED identity=" << identity << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NDNSF_IDENTITY_CHECK_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
