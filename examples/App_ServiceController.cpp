#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceController.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
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

int64_t
getIntegerOption(int argc, char** argv, const std::string& option, int64_t fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty())
    return fallback;
  try {
    size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed);
    if (consumed != value.size())
      throw std::invalid_argument("trailing characters");
    return parsed;
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid integer for " + option + ": " + value);
  }
}

ndn_service_framework::RevocationKind
parseRevocationKind(const std::string& value)
{
  if (value == "identity")
    return ndn_service_framework::RevocationKind::IDENTITY;
  if (value == "certificate")
    return ndn_service_framework::RevocationKind::CERTIFICATE;
  if (value == "service")
    return ndn_service_framework::RevocationKind::SERVICE_AUTHORIZATION;
  throw std::invalid_argument(
    "--revoke-kind must be identity, certificate, or service");
}

void
ensureIdentities(ndn::security::KeyChain& keyChain, const std::string& csv)
{
  std::stringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto first = item.find_first_not_of(" \t\r\n");
    const auto last = item.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    item = item.substr(first, last - first + 1);
    if (item.front() != '/')
      throw std::invalid_argument("--ensure-identities entries must be NDN names");
    getOrCreateIdentity(keyChain, ndn::Name(item));
  }
}

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;
    ndn::ValidatorConfig validator(face);
    ndn::Scheduler scheduler(face.getIoContext());
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const std::string policyFile = getOption(argc, argv, "--policy-file", "examples/hello.policies");
    const std::string trustSchema = getOption(argc, argv, "--trust-schema", "examples/trust-schema.conf");
    const std::string bootstrapTokenFile = getOption(argc, argv, "--bootstrap-token-file", "");
    const std::string ensureIdentityCsv = getOption(
      argc, argv, "--ensure-identities", "");
    const ndn::Name controllerPrefix(
      getOption(argc, argv, "--controller-prefix", DEFAULT_CONTROLLER_PREFIX.toUri()));
    const auto revokeAfterMs = getIntegerOption(argc, argv, "--revoke-after-ms", -1);
    const auto revokeRetryAfterMs = getIntegerOption(argc, argv, "--revoke-retry-after-ms", -1);
    const auto runForMs = getIntegerOption(argc, argv, "--run-for-ms", 0);
    // Deterministic grant-only version advance (Spec179 MiniNDN gate): issue
    // one additional role-specific grant at a bounded offset while the global
    // ABE pair stays byte-identical, so the campaign can observe the single
    // lazy replacement-DKEY fetch of the granted identity and the unchanged
    // behavior of every unaffected identity.
    const auto grantAfterMs = getIntegerOption(argc, argv, "--grant-additional-after-ms", -1);
    const auto grantIdentity = getOption(argc, argv, "--grant-additional-identity", "");
    const auto grantService = getOption(argc, argv, "--grant-additional-service", "/HELLO");
    const auto grantRole = getOption(argc, argv, "--grant-additional-role", "user");
    if (grantRole != "user" && grantRole != "provider")
      throw std::invalid_argument("--grant-additional-role must be user or provider");
    if (revokeAfterMs < -1 || runForMs < 0 || grantAfterMs < -1)
      throw std::invalid_argument(
        "--revoke-after-ms/--run-for-ms/--grant-additional-after-ms must be >= 0 or omitted");
    if (revokeRetryAfterMs < -1 ||
        (revokeRetryAfterMs >= 0 && (revokeAfterMs < 0 || revokeRetryAfterMs <= revokeAfterMs)))
      throw std::invalid_argument("--revoke-retry-after-ms must follow --revoke-after-ms");
    if (grantAfterMs >= 0 && grantIdentity.empty())
      throw std::invalid_argument(
        "--grant-additional-identity is required when --grant-additional-after-ms is set");

    auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(controllerPrefix));
    if (bootstrapTokenFile.empty()) {
      getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
      getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
      getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
      getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
      getOrCreateIdentity(keyChain, USER_IDENTITY);
    }
    ensureIdentities(keyChain, ensureIdentityCsv);

    validator.load(trustSchema);

    std::cout << "[App_ServiceController] authority identity="
              << controllerCert.getIdentity().toUri()
              << " serveCertificates=" << serveCertificates
              << " bootstrapTokenFile=" << (bootstrapTokenFile.empty() ? "<none>" : bootstrapTokenFile)
              << " dkeyPrefix="
              << ndn::Name(controllerCert.getIdentity()).append("DKEY").toUri()
              << std::endl;

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        // Pass the identity name rather than the certificate name.  The
        // publisher accepts either form, but resolving an already-known
        // identity avoids forcing the current ndn-cxx PIB implementation
        // through its certificate-name exception path during isolated
        // MiniNDN startup.
        face, keyChain, controllerPrefix);
    }

    ndn_service_framework::ServiceController controller(
      face,
      controllerCert,
      validator,
      policyFile);
    controller.setControllerPrefix(controllerPrefix);
    controller.setBootstrapTokenFile(bootstrapTokenFile);

    if (revokeAfterMs >= 0) {
      const auto kind = parseRevocationKind(
        getOption(argc, argv, "--revoke-kind", "identity"));
      ndn_service_framework::RevocationTarget target;
      target.kind = kind;
      const auto targetIdentity = getOption(argc, argv, "--revoke-identity", "");
      const auto serviceName = getOption(argc, argv, "--revoke-service", "");
      target.certificateDigest = getOption(
        argc, argv, "--revoke-certificate-digest", "");
      const auto authorizationAttribute = getOption(
        argc, argv, "--revoke-attribute", "");
      if (!targetIdentity.empty())
        target.targetIdentity = ndn::Name(targetIdentity);
      if (!serviceName.empty())
        target.serviceName = ndn::Name(serviceName);
      if (!authorizationAttribute.empty())
        target.authorizationAttribute = ndn::Name(authorizationAttribute);
      if (!target.isValid()) {
        throw std::invalid_argument(
          "invalid revocation target; provide the fields required by --revoke-kind");
      }
      const auto applyRevocation = [&controller, target] {
          const bool success = controller.revoke(target);
          const auto version = controller.getControllerVersion();
          std::cout << "NDNSF_REVOCATION_APPLIED success=" << (success ? 1 : 0)
                    << " kind=" << static_cast<int>(target.kind)
                    << " generation=" << version.controllerGenerationTimestamp
                    << " epoch=" << version.controllerEpoch << std::endl;
        };
      scheduler.schedule(ndn::time::milliseconds(revokeAfterMs), applyRevocation);
      if (revokeRetryAfterMs >= 0)
        scheduler.schedule(ndn::time::milliseconds(revokeRetryAfterMs), applyRevocation);
    }

    if (grantAfterMs >= 0) {
      const ndn::Name targetIdentity(grantIdentity);
      const ndn::Name serviceName(grantService);
      const ndn::Name attribute = ndn::Name(
          grantRole == "provider" ? "/SERVICE" : "/PERMISSION").append(serviceName);
      scheduler.schedule(ndn::time::milliseconds(grantAfterMs),
        [&controller, targetIdentity, serviceName, attribute] {
          const bool success = controller.grant(
            targetIdentity, serviceName, attribute);
          const auto version = controller.getControllerVersion();
          std::cout << "NDNSF_GRANT_ONLY_APPLIED success=" << (success ? 1 : 0)
                    << " identity=" << targetIdentity.toUri()
                    << " service=" << serviceName.toUri()
                    << " generation=" << version.controllerGenerationTimestamp
                    << " epoch=" << version.controllerEpoch << std::endl;
        });
    }

    std::cout << "ServiceController started..." << std::endl;
    // The textual started marker means only that the process constructed its
    // controller.  Protected clients must wait until start() has completed
    // the prefix-registration and PUBPARAMS readiness barrier.  Keep the
    // marker separate so launchers cannot confuse process liveness with
    // network/authorization readiness.
    controller.start();
    std::cout << "NDNSF_CONTROLLER_READY identity="
              << controllerPrefix.toUri() << std::endl;
    if (runForMs != 0) {
      const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(runForMs);
      while (std::chrono::steady_clock::now() < deadline) {
        face.getIoContext().restart();
        face.processEvents(ndn::time::milliseconds(100));
      }
      face.getIoContext().stop();
      std::cout << "NDNSF_CONTROLLER_STOPPED reason=run-for-ms" << std::endl;
    }
    else {
      while (true) {
        face.getIoContext().restart();
        face.processEvents(ndn::time::milliseconds(1000));
      }
    }
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_ServiceController error: " << e.what() << std::endl;
    return 1;
  }
}
