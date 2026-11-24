#include "tests/fixtures/test-resource-paths.hpp"
#include "isolated-test-trust.hpp"
#include "tests/boost-test.hpp"

#include "ndn-service-framework/AbeHistoricalAuthorization.hpp"
#include "ndn-service-framework/AbeHistoricalKeyRequest.hpp"
#include "ndn-service-framework/AbePublicParametersArchive.hpp"
#include "ndn-service-framework/ControllerGenerationStore.hpp"
#include "ndn-service-framework/FileKeyRing.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include "ndn-service-framework/RuntimeStatusStore.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/common.hpp"
#include "ndn-service-framework/utils.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/HistoricalReadClient.hpp"
#include "ndn-service-framework/detail/historical-policy-status.hpp"
#include "ndn-service-framework/historical-object-publisher.hpp"
#include "ndn-service-framework/CertificateBootstrap.hpp"

#include <ndn-cxx/security/certificate-fetcher-offline.hpp>
#include <ndn-cxx/security/validation-policy.hpp>

#include <nac-abe/algo/abe-support.hpp>
#include <nac-abe/ndn-crypto/data-enc-dec.hpp>

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validity-period.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/time-unit-test-clock.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework {

struct ServiceControllerTestAccess
{
  static ndn::security::Certificate
  recipientCertificate(ServiceController& controller, const ndn::Name& identity)
  {
    return controller.getTargetIdentityCertificate(identity);
  }

  static bool
  hasRecipientPrivateKey(ServiceController& controller, const ndn::Name& key)
  {
    return controller.m_keyChain.getTpm().hasKey(key);
  }

  static size_t
  identityCount(const ServiceController& controller)
  {
    return controller.m_keyChain.getPib().getIdentities().size();
  }

  static void
  installRecipientCertificate(ServiceController& controller,
                              const ndn::security::Certificate& certificate)
  {
    const auto key = controller.m_keyChain.getPib().getIdentity(certificate.getIdentity())
        .getKey(certificate.getKeyName());
    controller.m_keyChain.addCertificate(key, certificate);
    controller.m_keyChain.setDefaultCertificate(key, certificate);
  }

  static void
  requestDkey(ServiceController& controller, const ndn::Interest& interest)
  {
    controller.onCurrentAbeKeyInterest(interest);
  }

  static void
  requestAbeParameters(ServiceController& controller, const ndn::Interest& interest)
  {
    controller.onCurrentAbeParametersInterest(interest);
  }

  static bool
  acceptsHistoricalGrantWire(const ServiceController& controller, const std::string& wire)
  {
    return controller.acceptsHistoricalGrantWire(wire);
  }

  static void
  storeHistoricalReceipt(ServiceController& controller, const ndn::Data& publication,
                         const ndn::Data& receipt)
  {
    const auto& wire = receipt.wireEncode();
    controller.m_generationStore->recordHistoricalPublicationReceipt(publication.getFullName(),
      std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  }

  static bool
  acceptsHistoricalPublication(const ServiceController& controller, const ndn::Data& publication)
  {
    return controller.acceptsHistoricalPublication(publication);
  }

  static bool
  hasHistoricalGrant(const ServiceController& controller, const ndn::Name& identity,
                     const ControllerVersion& version, const std::string& policy)
  {
    return controller.hasHistoricalGrant(identity, version, policy);
  }

  static std::optional<std::string>
  loadGrantRecord(const ServiceController& controller, const ndn::Name& identity,
                  const ControllerVersion& version)
  {
    return controller.m_generationStore->loadGrant(identity.toUri(ndn::name::UriFormat::CANONICAL),
        version.controllerGenerationTimestamp, version.controllerEpoch);
  }

  static size_t
  originalReplyCount(const ServiceController& controller)
  {
    return controller.m_originalControlReplies.size();
  }

  static std::optional<ControllerVersion>
  durableVersion(const ServiceController& controller)
  {
    return controller.m_generationStore->load();
  }

  static std::shared_ptr<const ndn::Data>
  originalReply(const ServiceController& controller, const ndn::Name& name)
  {
    const auto found = controller.m_originalControlReplies.find(name);
    return found == controller.m_originalControlReplies.end() ? nullptr : found->second.packet;
  }

  static ndn::Name
  dkeyInterestPrefix(const ndn::Name& authority, const ndn::security::Certificate& certificate)
  {
    const auto keyName = certificate.getKeyName();
    const auto& wire = keyName.wireEncode();
    ndn::Name prefix(authority);
    prefix.append("DKEY").append(wire.begin(), wire.end());
    return prefix;
  }

  static ndn::Interest
  statusQuery(const ndn::Name& producer, const ndn::Name& service,
              const ControllerVersion& version = {})
  {
    const auto parameters = encodeControlQueryParameters({ndn::Name(), service,
        version.controllerGenerationTimestamp, version.controllerEpoch});
    ndn::Interest query(makeControlRpcName(producer, "POLICY-STATUS", parameters,
                                         makeControlQueryNonce()));
    query.setApplicationParameters(parameters);
    query.setMustBeFresh(true);
    query.setCanBePrefix(false);
    return query;
  }

  static void
  registerHandlersForDummyFace(ServiceController& controller)
  {
    // These cases exercise real policy handlers over an in-process DummyFace.
    // start() additionally proves a two-transport round trip through NFD and
    // cannot run against this fixture. That boundary has its own standalone
    // readiness negative cases and real-NFD test; never mark it ready here.
    controller.registerInterestHandlers();
  }

  static void
  receiptFetchInterest(ServiceController& controller, const ndn::Interest& interest)
  {
    controller.serveHistoricalReceipt(interest);
  }

  static void
  publicationReceiptInterest(ServiceController& controller, const ndn::Interest& interest)
  {
    // Same reason as historicalKeyInterest: the RPC filter does not dispatch in this fixture.
    controller.onHistoricalPublicationInterest(interest);
  }

  static void
  historicalKeyInterest(ServiceController& controller, const ndn::Interest& interest)
  {
    // The DKEY handler is not reachable through a DummyFace filter in this fixture; call the
    // private entry point the way the permission cases call theirs.
    controller.serveHistoricalKeyRequest(interest);
  }

  static ndn::security::Certificate
  ensureInternalControllerSigner(ServiceController& controller)
  {
    const auto identity = controller.m_aaCert.getIdentity();
    try {
      return controller.m_keyChain.getPib().getIdentity(identity)
          .getDefaultKey().getDefaultCertificate();
    }
    catch (const std::exception&) {
      const auto created = controller.m_keyChain.createIdentity(
          identity, ndn::RsaKeyParams(2048));
      return created.getDefaultKey().getDefaultCertificate();
    }
  }

  static ndn::security::Certificate
  ensureInternalIdentity(ServiceController& controller, const ndn::Name& identity)
  {
    try {
      return controller.m_keyChain.getPib().getIdentity(identity)
          .getDefaultKey().getDefaultCertificate();
    }
    catch (const std::exception&) {
      const auto created = controller.m_keyChain.createIdentity(
          identity, ndn::RsaKeyParams(2048));
      return created.getDefaultKey().getDefaultCertificate();
    }
  }

  static ndn::security::Certificate
  rsaEncryptionCertificate(ServiceController& controller,
                           const ndn::security::Certificate& identityHint)
  {
    return getRsaEncryptionCertificateOrThrow(controller.m_keyChain, identityHint);
  }

  static ndn::security::Certificate
  addAlternateSigningCertificate(ServiceController& controller,
                                 const ndn::Name& identity)
  {
    const auto pibIdentity = controller.m_keyChain.getPib().getIdentity(identity);
    const auto signingKey = controller.m_keyChain.createKey(
        pibIdentity, ndn::EcKeyParams());
    controller.m_keyChain.setDefaultKey(pibIdentity, signingKey);
    return signingKey.getDefaultCertificate();
  }

  static void
  refreshAbeIdentityPolicies(ServiceController& controller)
  {
    controller.addAttributesForUsersAccordingToServicePolicy();
  }

  static void
  policyStatusInterest(ServiceController& controller,
                       const ndn::Interest& interest)
  {
    controller.onPolicyStatusInterest(ndn::InterestFilter("/"), interest);
  }

  static void
  archivedPublicParametersInterest(ServiceController& controller, const ndn::Name& index,
                                   const ndn::Interest& interest)
  {
    controller.onArchivedPublicParametersInterest(index, interest);
  }

  static void
  certificateBootstrapInterest(ServiceController& controller, const ndn::Interest& interest)
  {
    BOOST_TEST_MESSAGE("BOOTSTRAP_ADMISSION prefix=" << controller.m_controllerPrefix
        << " parsed=" << bool(parseControlRpcName(interest, controller.m_controllerPrefix, "CERTBOOTSTRAP"))
        << " nonce=" << bool(controlNonceExpiry(controller.m_controllerPrefix, interest))
        << " wireBytes=" << interest.wireEncode().size()
        << " nameBytes=" << interest.getName().wireEncode().size());
    BOOST_REQUIRE(controller.admitControlQuery(interest, "CERTBOOTSTRAP"));
    controller.onCertificateBootstrapInterest(ndn::InterestFilter("/"), interest);
  }

  static void
  userPermissionsInterest(ServiceController& controller,
                          const ndn::Interest& interest)
  {
    controller.onUserPermissionsInterest(ndn::InterestFilter("/"), interest);
  }

  static void
  providerPermissionsInterest(ServiceController& controller,
                              const ndn::Interest& interest)
  {
    controller.onProviderPermissionsInterest(ndn::InterestFilter("/"), interest);
  }

  static ndn::Name
  userPermissionsPrefix(const ServiceController& controller)
  {
    return controller.m_prefixUserPermissions;
  }

  static ndn::Name
  providerPermissionsPrefix(const ServiceController& controller)
  {
    return controller.m_prefixProviderPermissions;
  }

  static PermissionResponse
  decryptPermissionResponse(ServiceController& controller,
                            const EncryptedPermissionResponse& encrypted)
  {
    return decryptPermissionResponseWithKeyChain(encrypted, controller.m_keyChain);
  }

  static PermissionResponse
  userPermissions(const ServiceController& controller,
                  const ndn::Name& identity)
  {
    return controller.buildUserPermissionResponse(identity);
  }

  static PermissionResponse
  providerPermissions(const ServiceController& controller,
                      const ndn::Name& identity)
  {
    return controller.buildProviderPermissionResponse(identity);
  }

  static std::string
  certificateDigest(const ndn::security::Certificate& certificate)
  {
    return ServiceController::certificateDigest(certificate);
  }

  static ndn::Name
  abePublicParametersName(const ServiceController& controller)
  {
    return controller.m_abePublicParametersName;
  }

  static ControllerVersion
  abeMaterialVersion(const ServiceController& controller)
  {
    return controller.m_abeMaterialVersion;
  }

  static bool reconcilePendingAbeRotation(ServiceController& controller)
  {
    return controller.reconcilePendingAbeRotation();
  }

  static std::string
  abePublicParametersDigest(const ServiceController& controller)
  {
    return controller.m_abePublicParametersDigest;
  }

  static std::string
  abePublicParametersWire(const ServiceController& controller)
  {
    return controller.m_aa.m_pubParams.m_pub;
  }

  static ndn::nacabe::algo::PublicParams
  abePublicParameters(const ServiceController& controller)
  {
    auto params = controller.m_aa.m_pubParams;
    return params;
  }

  static std::string
  abeMasterKey(const ServiceController& controller)
  {
    return controller.m_aa.m_masterKey.m_msk;
  }

  static ndn::nacabe::algo::PrivateKey
  abePrivateKey(const ServiceController& controller, const ndn::Name& identity)
  {
    const auto policy = abePolicy(controller, identity);
    if (policy.empty())
      throw std::runtime_error("missing ABE policy for test identity");
    auto params = controller.m_aa.m_pubParams;
    auto master = controller.m_aa.m_masterKey;
    return ndn::nacabe::algo::ABESupport::getInstance().kpPrvKeyGen(
        params, master, policy);
  }

  static ndn::nacabe::algo::PrivateKey
  abePrivateKey(const ndn::nacabe::algo::PublicParams& publicParams,
                const ndn::nacabe::algo::MasterKey& masterKey,
                const std::string& policy)
  {
    auto params = publicParams;
    auto master = masterKey;
    return ndn::nacabe::algo::ABESupport::getInstance().kpPrvKeyGen(
        params, master, policy);
  }

  static ndn::nacabe::algo::CipherText
  abeEncrypt(const ServiceController& controller, const ndn::Name& attribute,
             const std::string& plaintext)
  {
    return ndn::nacabe::algo::ABESupport::getInstance().kpEncrypt(
        controller.m_aa.m_pubParams, {makeAbeServiceAttribute(attribute.getSubName(1), std::string(reinterpret_cast<const char*>(attribute[0].value()), attribute[0].value_size()))},
        ndn::Buffer(reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()));
  }

  static ndn::nacabe::algo::CipherText
  abeEncrypt(const ndn::nacabe::algo::PublicParams& publicParams,
             const ndn::Name& attribute,
             const std::string& plaintext)
  {
    return ndn::nacabe::algo::ABESupport::getInstance().kpEncrypt(
        publicParams, {makeAbeServiceAttribute(attribute.getSubName(1), std::string(reinterpret_cast<const char*>(attribute[0].value()), attribute[0].value_size()))},
        ndn::Buffer(reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()));
  }

  static ndn::Buffer
  abeDecrypt(const ServiceController& controller,
             const ndn::nacabe::algo::PrivateKey& key,
             ndn::nacabe::algo::CipherText cipher)
  {
    return ndn::nacabe::algo::ABESupport::getInstance().kpDecrypt(
        controller.m_aa.m_pubParams, key, std::move(cipher));
  }

  static ndn::Buffer
  abeDecrypt(const ndn::nacabe::algo::PublicParams& publicParams,
             const ndn::nacabe::algo::PrivateKey& key,
             ndn::nacabe::algo::CipherText cipher)
  {
    return ndn::nacabe::algo::ABESupport::getInstance().kpDecrypt(
        publicParams, key, std::move(cipher));
  }

  // A generation-mismatched decrypt must fail closed.  OpenABE either throws
  // (pairing/tag mismatch) or returns content that does not recover the
  // plaintext; both outcomes prove the plaintext is unavailable and are
  // accepted deterministically by the cryptographic exclusion matrix.
  static bool
  decryptFailsClosed(const ndn::nacabe::algo::PublicParams& publicParams,
                     const ndn::nacabe::algo::PrivateKey& key,
                     ndn::nacabe::algo::CipherText cipher,
                     const std::string& expectedPlaintext)
  {
    try {
      const auto recovered = ndn::nacabe::algo::ABESupport::getInstance().kpDecrypt(
          publicParams, key, std::move(cipher));
      if (recovered.size() != expectedPlaintext.size())
        return true;
      return !std::equal(recovered.begin(), recovered.end(),
                         expectedPlaintext.begin());
    }
    catch (const std::exception&) {
      return true;
    }
  }

  static std::string
  abePolicy(const ServiceController& controller, const ndn::Name& identity)
  {
    const auto attributes = controller.effectiveAttributesFor(identity.toUri(ndn::name::UriFormat::CANONICAL));
    return boost::algorithm::join(attributes, " OR ");
  }
};

namespace integration_test {
namespace {

constexpr const char* SERVICE = "/ObjectDetection/YOLOv8";

struct PortableRecipientFixture
{
  std::filesystem::path root = test::trust_detail::makeTestTrustDirectory();
  ndn::Name subject = ndn::Name("/spec217/recipient").append(root.filename().string());
  ndn::KeyChain localKeys;
  ndn::KeyChain remoteKeys{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{localKeys};
  ndn::ValidatorConfig validator{face};
  std::unique_ptr<ServiceController> controller;
  std::optional<std::string> previousState;

  explicit PortableRecipientFixture(const char* secondUserService = "")
  {
    if (const auto* value = ::getenv("NDNSF_CONTROLLER_GENERATION_STATE"))
      previousState = value;
    ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", (root / "generation").c_str(), 1);
    const auto certificate = localKeys.createIdentity(
        ndn::Name(subject).append("controller"), ndn::RsaKeyParams(2048))
        .getDefaultKey().getDefaultCertificate();
    std::ofstream policy(root / "policy");
    policy << "provider-policies\n{\n provider-policy\n {\n  for "
           << ndn::Name(subject).append("provider").toUri()
           << "\n  allow\n  {\n   /HELLO\n  }\n }\n}\n"
           << "user-policies\n{\n user-policy\n {\n  for "
           << subject.toUri()
           << "\n  allow\n  {\n   /HELLO\n";
    if (*secondUserService != '\0')
      policy << "   " << secondUserService << '\n';
    policy << "  }\n }\n}\n";
    policy.close();
    controller = std::make_unique<ServiceController>(face, certificate, validator,
                                                    (root / "policy").string());
  }

  ~PortableRecipientFixture()
  {
    controller.reset();
    if (previousState)
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", previousState->c_str(), 1);
    else
      ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  void
  anchor(const ndn::security::Certificate& certificate)
  {
    validator.loadAnchor(certificate.getName().toUri(),
                         ndn::security::Certificate(certificate));
  }
};

struct MultiServiceRecipientFixture : PortableRecipientFixture
{
  MultiServiceRecipientFixture()
    : PortableRecipientFixture("/OTHER")
  {
  }
};

PolicyStatusData
statusFor(const ControllerVersion& version,
          const std::vector<RevocationTarget>& revocations = {})
{
  PolicyStatusData status;
  status.setServiceName(ndn::Name(SERVICE));
  status.setControllerVersion(version);
  status.setValidity(900, 5000);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("controller-signature"),
                                  20));
  for (const auto& target : revocations)
    status.addRevocation(target);
  return status;
}

PolicyStatusData
liveStatusFor(const ControllerVersion& version,
              const std::vector<RevocationTarget>& revocations = {})
{
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  auto status = statusFor(version, revocations);
  // Runtime installControllerStatus validates against the actual wall clock;
  // keep this helper independent from the deterministic 900..5000 test
  // window used by the state-only cases above.
  status.setValidity(now - 1000, now + 59000);
  return status;
}

PolicyStatusData
liveStatusForService(const ndn::Name& serviceName,
                     const ControllerVersion& version,
                     const std::vector<RevocationTarget>& revocations = {})
{
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(version);
  status.setValidity(now - 1000, now + 59000);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("controller-signature"),
                                  20));
  for (const auto& target : revocations)
    status.addRevocation(target);
  return status;
}

PermissionResponse
runtimePermission(const ndn::Name& targetIdentity,
                  size_t permissionKind,
                  const ndn::Name& providerName,
                  const ndn::Name& serviceName,
                  const ControllerVersion& version)
{
  PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(version.controllerEpoch);

  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.setPolicyEpoch(version.controllerEpoch);
  response.setControllerVersion(version);
  response.addEntry(entry);
  return response;
}

RevocationTarget
makeIdentityRevocation(const char* identity)
{
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = ndn::Name(identity);
  return target;
}

RevocationTarget
makeCertificateRevocation(const char* identity, const char* digest)
{
  RevocationTarget target;
  target.kind = RevocationKind::CERTIFICATE;
  target.targetIdentity = ndn::Name(identity);
  target.certificateDigest = digest;
  return target;
}

RevocationTarget
makeServiceRevocation(const char* identity)
{
  RevocationTarget target;
  target.kind = RevocationKind::SERVICE_AUTHORIZATION;
  target.targetIdentity = ndn::Name(identity);
  target.serviceName = ndn::Name(SERVICE);
  target.authorizationAttribute = ndn::Name("/SERVICE").append(ndn::Name(SERVICE));
  return target;
}

RevocationTarget
makeUserServiceRevocation(const char* identity)
{
  RevocationTarget target = makeServiceRevocation(identity);
  target.authorizationAttribute = ndn::Name("/PERMISSION").append(
      ndn::Name(SERVICE));
  return target;
}

struct TerminalLedger
{
  bool execute(const std::string& requestId, const RevocationDecision& decision)
  {
    if (!decision.allowed || terminal.count(requestId) != 0)
      return false;
    ++executions[requestId];
    terminal.emplace(requestId, "executed");
    return true;
  }

  void rejectExactlyOnce(const std::string& requestId,
                         const RevocationDecision& decision)
  {
    if (decision.allowed || terminal.count(requestId) != 0)
      return;
    terminal.emplace(requestId, decision.reason);
  }

  std::map<std::string, unsigned> executions;
  std::map<std::string, std::string> terminal;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ControllerRevocationFlow,
  *boost::unit_test::fixture<ndn_service_framework::test::ControllerSealFixture>())

// Selected explicitly by the bounded mount-namespace fixture, never on a shared /tmp.
BOOST_AUTO_TEST_CASE(DefaultControllersWithoutOverrideRemainIndependent,
                     *boost::unit_test::disabled())
{
  const auto* isolated = ::getenv("NDNSF_TEST_ISOLATED_CONTROLLER_DEFAULT");
  BOOST_REQUIRE(isolated != nullptr && std::string(isolated) == "1");
  BOOST_REQUIRE(::getenv("NDNSF_CONTROLLER_GENERATION_STATE") == nullptr);
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", ("pib-sqlite3:" + (root / ".ndn").string()).c_str(), 1);
  ::setenv("NDN_CLIENT_TPM", ("tpm-file:" + (root / ".ndn").string()).c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec217/provider allow { /HELLO } } }\n"
    "user-policies { user-policy { for /spec217/user allow { /HELLO } } }\n";
  ndn::KeyChain keys;
  const auto certA = keys.createIdentity(ndn::Name("/spec217/default/a"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto certB = keys.createIdentity(ndn::Name("/spec217/default/b"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace faceA(keys);
  ndn::DummyClientFace faceB(keys);
  ndn::ValidatorConfig validatorA(faceA);
  ndn::ValidatorConfig validatorB(faceB);
  auto first = std::make_unique<ServiceController>(faceA, certA, validatorA, policy.string());
  auto second = std::make_unique<ServiceController>(faceB, certB, validatorB, policy.string());
  BOOST_REQUIRE(first->getControllerVersion().isValid());
  BOOST_REQUIRE(second->getControllerVersion().isValid());
  RevocationTarget revoked;
  revoked.kind = RevocationKind::IDENTITY;
  revoked.targetIdentity = ndn::Name("/spec217/user");
  BOOST_REQUIRE(first->revoke(revoked));
  const auto version = first->getControllerVersion();
  BOOST_CHECK(first->isRevoked(revoked.targetIdentity, ndn::Name("/HELLO")));
  BOOST_CHECK(!second->isRevoked(revoked.targetIdentity, ndn::Name("/HELLO")));
  first.reset();
  const auto movedPolicy = root / "moved-policy";
  std::filesystem::copy_file(policy, movedPolicy);
  first = std::make_unique<ServiceController>(faceA, certA, validatorA, movedPolicy.string());
  BOOST_REQUIRE(first->getControllerVersion().isValid());
  BOOST_CHECK(first->getControllerVersion().controllerGenerationTimestamp >
              version.controllerGenerationTimestamp);
  BOOST_CHECK(first->isRevoked(revoked.targetIdentity, ndn::Name("/HELLO")));
  BOOST_CHECK(!second->isRevoked(revoked.targetIdentity, ndn::Name("/HELLO")));
}

BOOST_FIXTURE_TEST_CASE(PortableRecipientPublicAnchorSupportsPermissionAndDkey,
                       PortableRecipientFixture)
{
  const auto certificate = remoteKeys.createIdentity(subject, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  const auto identities = ServiceControllerTestAccess::identityCount(*controller);
  BOOST_CHECK_THROW(localKeys.getPib().getIdentity(subject), ndn::security::pib::Pib::Error);
  anchor(certificate);
  const auto actual = ServiceControllerTestAccess::recipientCertificate(*controller, subject);
  BOOST_CHECK(actual.wireEncode() == certificate.wireEncode());
  BOOST_CHECK(!ServiceControllerTestAccess::hasRecipientPrivateKey(*controller,
                                                                  certificate.getKeyName()));
  ServiceControllerTestAccess::refreshAbeIdentityPolicies(*controller);
  face.sentData.clear();
  const auto dkey = ServiceControllerTestAccess::dkeyInterestPrefix(
      ndn::Name(subject).append("controller"), certificate);
  ServiceControllerTestAccess::requestDkey(*controller, ndn::Interest(dkey).setCanBePrefix(true));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(!face.sentData.empty());
  BOOST_CHECK(dkey.isPrefixOf(face.sentData.back().getName()));
  BOOST_CHECK_NE(face.sentData.back().getContent().value_size(), 0U);
  controller->setControllerPrefix(ndn::Name(subject).append("controller"));
  ServiceControllerTestAccess::registerHandlersForDummyFace(*controller);
  const auto parameters = encodeControlQueryParameters(
    {subject, ndn::Name(), 0, 0, certificate.getFullName()});
  ndn::Interest query(makeControlRpcName(ndn::Name(subject).append("controller"),
      "USER-PERMISSIONS", parameters, makeControlQueryNonce()));
  query.setApplicationParameters(parameters);
  query.setCanBePrefix(false);
  face.sentData.clear();
  face.sentInterests.clear();
  ServiceControllerTestAccess::userPermissionsInterest(*controller, query);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  BOOST_CHECK(std::none_of(face.sentInterests.begin(), face.sentInterests.end(),
    [&](const auto& outgoing) { return outgoing.getName() == certificate.getFullName(); }));
  const auto& content = face.sentData.back().getContent();
  auto [decoded, block] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(decoded);
  EncryptedPermissionResponse encrypted;
  BOOST_REQUIRE(encrypted.WireDecode(block));
  const auto permission = decryptPermissionResponseWithKeyChain(encrypted, remoteKeys);
  BOOST_CHECK(ndn::Name(permission.getTargetIdentity()) == subject);
  BOOST_CHECK_EQUAL(permission.getEntries().size(), 1U);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::identityCount(*controller), identities);
  BOOST_CHECK_THROW(localKeys.getPib().getIdentity(subject), ndn::security::pib::Pib::Error);
  const auto stranger = ndn::Name(subject).append("stranger");
  anchor(remoteKeys.createIdentity(stranger, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate());
  BOOST_CHECK(ServiceControllerTestAccess::userPermissions(*controller, stranger)
      .getEntries().empty());
}

BOOST_AUTO_TEST_CASE(CurrentApplicationReadUsesIdentityDkeyAndRejectsReplacedOriginalCk)
{
  // ServiceController owns its default KeyChain. Scope that on-disk PIB/TPM before
  // constructing the fixture; the application has only its independent in-memory keys.
  const auto home = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
  });
  for (const auto& [name, value] : std::map<std::string, std::string>{
         {"HOME", home.string()}, {"NDN_CLIENT_PIB", "pib-sqlite3:"},
         {"NDN_CLIENT_TPM", "tpm-file:"}}) {
    if (const auto* old = ::getenv(name.c_str())) previous[name] = std::string(old);
    else previous[name] = std::nullopt;
    ::setenv(name.c_str(), value.c_str(), 1);
  }

  PortableRecipientFixture fixture;
  auto& controller = *fixture.controller;
  const auto issuer = ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
  const auto producer = fixture.remoteKeys.createIdentity(ndn::Name(fixture.subject).append("provider"),
    ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto reader = fixture.remoteKeys.createIdentity(fixture.subject, ndn::RsaKeyParams(2048))
                        .getDefaultKey().getDefaultCertificate();
  test::IsolatedTestTrust trust({issuer, producer, reader});
  fixture.validator.load(trust.schemaPath());
  controller.setControllerPrefix(issuer.getIdentity());
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  ServiceControllerTestAccess::refreshAbeIdentityPolicies(controller);
  auto& io = fixture.face.getIoContext();
  ndn::DummyClientFace::Options options;
  options.processEventsOverride = [&io] (auto) {
    if (io.stopped()) io.restart();
    io.poll();
  };
  ndn::DummyClientFace actor(io, fixture.remoteKeys, options);
  const auto reply = fixture.face.onSendData.connect([&] (const ndn::Data& data) { actor.receive(data); });
  const auto rpc = ndn::Name(issuer.getIdentity()).append("NDNSF").append("RPC");
  const auto params = ndn::Name(issuer.getIdentity()).append(ndn::nacabe::PUBLIC_PARAMS);
  const auto dkey = ServiceControllerTestAccess::dkeyInterestPrefix(issuer.getIdentity(), reader);
  std::map<ndn::Name, ndn::Data> originals;
  size_t statusRequests = 0;
  size_t dkeyRequests = 0;
  size_t ckReads = 0;
  ndn::Name ckName;
  // Dispatch to the production handlers. The map is the only storage stand-in and serves exact originals.
  const auto interests = actor.onSendInterest.connect([&] (const ndn::Interest& query) {
    const auto& name = query.getName();
    if (ndn::Name(rpc).append("POLICY-STATUS").isPrefixOf(name)) {
      ++statusRequests;
      ServiceControllerTestAccess::policyStatusInterest(controller, query);
    }
    else if (ndn::Name(rpc).append("USER-PERMISSIONS").isPrefixOf(name))
      ServiceControllerTestAccess::userPermissionsInterest(controller, query);
    else if (ndn::Name(rpc).append("PUBLISH-HISTORY").isPrefixOf(name))
      ServiceControllerTestAccess::publicationReceiptInterest(controller, query);
    else if (params.isPrefixOf(name))
      ServiceControllerTestAccess::requestAbeParameters(controller, query);
    else if (dkey.isPrefixOf(name)) {
      ++dkeyRequests;
      ServiceControllerTestAccess::requestDkey(controller, query);
    }
    else if (const auto found = originals.find(name); found != originals.end()) {
      if (name == ckName) ++ckReads;
      actor.receive(found->second);
    }
  });

  const ndn::Name service("/HELLO");
  const ndn::Buffer input(8000, 0x57);
  HistoricalReadRequest read;
  publishHistoricalObject(actor, fixture.remoteKeys, producer, issuer, service,
    ndn::Name(producer.getIdentity()).append("objects"), input, [&] (const auto& publication) {
      const auto decoded = decodeHistoricalPublication(publication.mapping);
      BOOST_REQUIRE(decoded);
      BOOST_REQUIRE_GT(publication.segments.size(), 1U);
      read.controllerPrefix = issuer.getIdentity();
      read.controllerCertificate = issuer;
      read.recipientCertificate = reader;
      read.producerCertificate = producer;
      read.producerName = producer.getIdentity();
      read.serviceName = service;
      read.publicationName = publication.mapping.getName();
      read.ckName = ckName = publication.ck.getName();
      read.objectName = read.payloadName = decoded->objectName;
      read.generation = decoded->generation;
      read.epoch = decoded->epoch;
      read.policy = decoded->policy;
      originals.emplace(read.publicationName, publication.mapping);
      originals.emplace(ckName, publication.ck);
      for (const auto& segment : publication.segments)
        originals.emplace(segment->getName(), *segment);
    });
  const auto recovered = detail::readCurrentNacObject(actor, fixture.remoteKeys, read, trust.schemaPath());
  BOOST_REQUIRE(recovered);
  BOOST_CHECK_EQUAL(*recovered, std::string(input.begin(), input.end()));
  BOOST_REQUIRE_GT(dkeyRequests, 0U);
  BOOST_REQUIRE_EQUAL(ckReads, 1U);

  auto& replacement = originals.at(ckName);
  const auto originalFullName = replacement.getFullName();
  replacement.setFreshnessPeriod(ndn::time::milliseconds(17));
  fixture.remoteKeys.sign(replacement, ndn::security::signingByCertificate(producer));
  BOOST_REQUIRE(replacement.getFullName() != originalFullName);
  BOOST_REQUIRE(ndn::security::verifySignature(replacement, producer));
  const auto priorDkeyRequests = dkeyRequests;
  BOOST_CHECK(!detail::readCurrentNacObject(actor, fixture.remoteKeys, read, trust.schemaPath()));
  BOOST_CHECK_EQUAL(ckReads, 2U);
  BOOST_CHECK_EQUAL(dkeyRequests, priorDkeyRequests);

  const auto priorStatusRequests = statusRequests;
  read.totalTimeoutMs = 0;
  BOOST_CHECK(!detail::readCurrentNacObject(actor, fixture.remoteKeys, read, trust.schemaPath()));
  if (io.stopped()) io.restart();
  io.poll();
  BOOST_CHECK_EQUAL(statusRequests, priorStatusRequests);
}

/** Accept Interests, reject every Data packet.
 *
 * The DKEY handler validates the request and the CK evidence through one validator, so the only way
 * to exercise the evidence-signature branch is a policy that treats the two packet kinds differently.
 */
/** Build a CK name that carries the producer locator the publication check requires.
 *
 * Mirrors the attribute-scoped naming used by the historical publication path; the same construction
 * exists in the unit-test file, where it is private, so the integration case needs its own copy.
 */
ndn::Name
makeHistoricalCkNameForTest(const ndn::Name& producer, const ndn::Name& service,
                            const std::string& attribute)
{
  const auto bytes = [] (const ndn::Name& value) {
    const auto& wire = value.wireEncode();
    return std::vector<uint8_t>(wire.data(), wire.data() + wire.size());
  };
  const auto scope = makeNamingDigestComponent(computeNamingDigest(
    "ndnsf-naming/hybrid-key-scope",
    {{1, 1, bytes(service)}, {2, 1, bytes(producer)},
     {3, 2, std::vector<uint8_t>(attribute.begin(), attribute.end())}}));
  ndn::Name name = makeProducerDataPrefix(producer, "KEY");
  name.append("kind=HYBRID-MESSAGE").append("SCOPE").append("SERVICE").append(scope)
    .append("EPOCH")
    .append(makeNamingDigestComponent(computeNamingDigest(
      "ndnsf-naming/opaque/protection-epoch", {{1, 4, std::vector<uint8_t>{'e', '1'}}})))
    .append("OBJECT").append(makeNamingDigestComponent(std::string(64, 'a')));
  return name;
}

class RejectEvidenceDataPolicy : public ndn::security::ValidationPolicy
{
public:
  void
  checkPolicy(const ndn::Data&, const std::shared_ptr<ndn::security::ValidationState>& state,
              const ValidationContinuation&) override
  {
    state->fail({ndn::security::ValidationError::INVALID_SIGNATURE,
                 "evidence rejected by the test policy"});
  }

  void
  checkPolicy(const ndn::Interest&, const std::shared_ptr<ndn::security::ValidationState>& state,
              const ValidationContinuation& continueValidation) override
  {
    continueValidation(nullptr, state);
  }
};

BOOST_AUTO_TEST_CASE(ControllerAcceptsCommittedSignerHandoff)
{
  // A committed signer handoff is accepted only when the successor certificate is the identity's
  // default key and the key chain holds that key. Witness-backed receipt acceptance after a handoff
  // is already covered by the existing handoff/restart cases; this case only pins the startup gate.
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                           "NDNSF_CONTROLLER_GENERATION_STATE", "NDNSF_CONTROLLER_HANDOFF"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", "pib-sqlite3:", 1);
  ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", (root / "state").c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /spec243/service } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /spec243/service } } }\n";
  ndn::KeyChain keys;
  const auto identity = keys.createIdentity(ndn::Name("/spec243/controller"),
                                            ndn::RsaKeyParams(2048));
  const auto predecessorCert = identity.getDefaultKey().getDefaultCertificate();
  ControllerVersion materialVersion;
  {
    ndn::DummyClientFace face(keys);
    ndn::security::ValidatorNull validator;
    ServiceController controller(face, predecessorCert, validator, policy.string());
    BOOST_REQUIRE(controller.getControllerVersion().isValid());
    controller.setControllerPrefix(predecessorCert.getIdentity());
    materialVersion = controller.getPolicyStatus(ndn::Name("/spec243/service")).getAbeMaterialVersion();
    BOOST_REQUIRE(materialVersion.isValid());
  }

  const auto successorKey = keys.createKey(identity, ndn::RsaKeyParams(2048));
  keys.setDefaultKey(identity, successorKey);
  const auto successorCert = successorKey.getDefaultCertificate();
  const auto handoff = makeControllerSignerHandoff(predecessorCert, successorCert,
                                                   materialVersion, keys);
  const auto handoffPath = root / "handoff.bin";
  {
    const auto& wire = handoff.wireEncode();
    std::ofstream(handoffPath, std::ios::binary)
      .write(reinterpret_cast<const char*>(wire.data()), wire.size());
  }
  ::setenv("NDNSF_CONTROLLER_HANDOFF", handoffPath.c_str(), 1);

  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController successor(face, successorCert, validator, policy.string());
  BOOST_CHECK_MESSAGE(successor.getControllerVersion().isValid(),
                      "the successor did not accept the committed handoff");
}

BOOST_AUTO_TEST_CASE(ControllerRecordsHistoricalGrantForAuthorisedUser)
{
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                           "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", "pib-sqlite3:", 1);
  ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", (root / "state").c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /spec243/service } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /spec243/service } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto userCert = keys.createIdentity(ndn::Name("/spec243/user"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  const auto version = controller.getControllerVersion();

  const auto parameters = encodeControlQueryParameters(
    {userCert.getIdentity(), ndn::Name(), 0, 0, userCert.getFullName()});
  ndn::Interest query(makeControlRpcName(authority, "USER-PERMISSIONS", parameters,
                                         makeControlQueryNonce()));
  query.setApplicationParameters(parameters);
  query.setCanBePrefix(false);
  face.sentData.clear();
  ServiceControllerTestAccess::userPermissionsInterest(controller, query);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE_MESSAGE(!face.sentData.empty(), "the controller answered no permission query");

  const auto granted = ServiceControllerTestAccess::hasHistoricalGrant(
    controller, userCert.getIdentity(), version, "/PERMISSION/spec243/service");
  BOOST_CHECK_MESSAGE(granted, "no historical grant covers /spec243/service for the user");
  const auto record = ServiceControllerTestAccess::loadGrantRecord(controller, userCert.getIdentity(),
                                                                  version);
  BOOST_REQUIRE(record.has_value());
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();
}

BOOST_AUTO_TEST_CASE(ControllerStoresPublicationReceiptForOrdinaryPublication)
{
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                           "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", "pib-sqlite3:", 1);
  ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
  const auto stateFile = root / "controller-generation.state";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", stateFile.c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /spec243/service } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /HELLO } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto producerCert = keys.createIdentity(ndn::Name("/spec243/producer"),
                                                ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  const auto version = controller.getControllerVersion();

  HistoricalPublication publication;
  publication.controller = authority;
  publication.producer = producerCert.getIdentity();
  publication.service = ndn::Name("/spec243/service");
  publication.objectName = ndn::Name("/spec243/producer").append("objects").append("1");
  publication.policyGeneration = version.controllerGenerationTimestamp;
  publication.policyEpoch = version.controllerEpoch;
  publication.segments = 1;
  publication.policy = "/PERMISSION/spec243/service";
  publication.objectDigest = "sha256:" + std::string(64, 'b');
  const auto status = controller.getPolicyStatus(publication.service);
  publication.publicParametersName = status.getAbePublicParametersName();
  // The status advertises an uppercase digest; the publication format and its builder require the
  // canonical lowercase form, while the receipt comparison is case-insensitive.
  auto parametersDigest = status.getAbePublicParametersDigest();
  std::transform(parametersDigest.begin(), parametersDigest.end(), parametersDigest.begin(),
                 [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
  publication.publicParametersDigest = parametersDigest;
  const auto material = status.getAbeMaterialVersion();
  publication.generation = material.controllerGenerationTimestamp;
  publication.epoch = material.controllerEpoch;
  const auto ckBasis = makeHistoricalCkNameForTest(publication.producer, publication.service,
                                                   publication.policy).getPrefix(-1);
  publication.ckFullName = ndn::Name(ckBasis);
  publication.ckFullName.appendImplicitSha256Digest(std::vector<uint8_t>(32, 0x11));
  const auto publicationData = makeHistoricalPublicationData(
    publication, keys, ndn::security::signingByCertificate(producerCert));

  const auto& wire = publicationData.wireEncode();
  const ndn::Block parameters(wire);
  ndn::Interest interest(makeControlRpcName(authority, "PUBLISH-HISTORY", parameters,
                                            makeControlQueryNonce()));
  interest.setApplicationParameters(parameters);
  ndn::security::InterestSigner signer(keys);
  signer.makeSignedInterest(interest, ndn::security::signingByCertificate(producerCert));
  face.sentData.clear();
  ServiceControllerTestAccess::publicationReceiptInterest(controller, interest);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  const auto receiptDir = stateFile.string() + ".historical-publications";
  BOOST_CHECK_MESSAGE(std::filesystem::exists(receiptDir) &&
                      !std::filesystem::is_empty(receiptDir),
                      "no publication receipt was persisted");
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();
}

BOOST_AUTO_TEST_CASE(ControllerServesHistoricalKeyAfterRotation)
{
  // Restored case: a grant, a committed publication receipt, then a revocation that advances the
  // controller epoch, then a historical DKEY request for the pre-rotation publication. The controller
  // must answer with the retired material rather than a denial.
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                           "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", "pib-sqlite3:", 1);
  ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
  const auto stateFile = root / "controller-generation.state";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", stateFile.c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /spec243/service } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /spec243/service } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto producerCert = keys.createIdentity(ndn::Name("/spec243/producer"),
                                                ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto userCert = keys.createIdentity(ndn::Name("/spec243/user"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);

  // The user's grant is what authorizes the historical read.
  const auto permissionParameters = encodeControlQueryParameters(
    {userCert.getIdentity(), ndn::Name(), 0, 0, userCert.getFullName()});
  ndn::Interest permissionQuery(makeControlRpcName(authority, "USER-PERMISSIONS",
                                                   permissionParameters, makeControlQueryNonce()));
  permissionQuery.setApplicationParameters(permissionParameters);
  permissionQuery.setCanBePrefix(false);
  face.sentData.clear();
  ServiceControllerTestAccess::userPermissionsInterest(controller, permissionQuery);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE(!face.sentData.empty());

  const auto version = controller.getControllerVersion();
  HistoricalPublication publication;
  publication.controller = authority;
  publication.producer = producerCert.getIdentity();
  publication.service = ndn::Name("/spec243/service");
  publication.objectName = ndn::Name("/spec243/producer").append("objects").append("1");
  publication.policyGeneration = version.controllerGenerationTimestamp;
  publication.policyEpoch = version.controllerEpoch;
  publication.segments = 1;
  publication.policy = "/PERMISSION/spec243/service";
  publication.objectDigest = "sha256:" + std::string(64, 'b');
  const auto status = controller.getPolicyStatus(publication.service);
  publication.publicParametersName = status.getAbePublicParametersName();
  auto parametersDigest = status.getAbePublicParametersDigest();
  std::transform(parametersDigest.begin(), parametersDigest.end(), parametersDigest.begin(),
                 [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
  publication.publicParametersDigest = parametersDigest;
  const auto material = status.getAbeMaterialVersion();
  publication.generation = material.controllerGenerationTimestamp;
  publication.epoch = material.controllerEpoch;
  const auto ckBasis = makeHistoricalCkNameForTest(publication.producer, publication.service,
                                                   publication.policy).getPrefix(-1);
  ndn::Data ck = ndn::Data(ndn::Name(ckBasis));
  keys.sign(ck, ndn::security::signingByCertificate(producerCert));
  publication.ckFullName = ck.getFullName();
  const auto publicationData = makeHistoricalPublicationData(
    publication, keys, ndn::security::signingByCertificate(producerCert));
  const auto& publicationWire = publicationData.wireEncode();
  const ndn::Block publicationParameters(publicationWire);
  ndn::Interest receiptInterest(makeControlRpcName(authority, "PUBLISH-HISTORY",
                                                   publicationParameters,
                                                   makeControlQueryNonce()));
  receiptInterest.setApplicationParameters(publicationParameters);
  ndn::security::InterestSigner signer(keys);
  signer.makeSignedInterest(receiptInterest, ndn::security::signingByCertificate(producerCert));
  face.sentData.clear();
  ServiceControllerTestAccess::publicationReceiptInterest(controller, receiptInterest);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE_MESSAGE(!face.sentData.empty(), "the controller stored no receipt");

  // A revocation of an unrelated identity advances the epoch without withdrawing the user's grant.
  const auto revoked = controller.revoke(makeServiceRevocation("/spec243/unrelated"));
  BOOST_REQUIRE(revoked);
  const auto rotatedVersion = controller.getControllerVersion();
  BOOST_REQUIRE_MESSAGE(rotatedVersion.compare(version) > 0,
                        "the controller epoch did not advance");

  HistoricalKeyRequest request;
  request.controllerPrefix = authority;
  request.generation = publication.generation;
  request.epoch = publication.epoch;
  request.objectName = publication.objectName;
  request.producerName = publication.producer;
  request.serviceName = publication.service;
  request.ckName = ck.getName();
  request.policy = publication.policy;
  request.recipientCertificate = userCert.getFullName();
  const auto nowMs = static_cast<uint64_t>(ndn::time::toUnixTimestamp(
    ndn::time::system_clock::now()).count());
  request.mintedAtMs = nowMs;
  request.expiresAtMs = request.mintedAtMs + 10000;
  request.currentControllerVersion = rotatedVersion;
  const auto ckWire = ck.wireEncode();
  request.ckEvidence.assign(ckWire.begin(), ckWire.end());
  request.publicationEvidence.assign(publicationWire.begin(), publicationWire.end());

  ndn::Interest requestInterest(makeHistoricalKeyRequestName(authority, request));
  requestInterest.setApplicationParameters(ndn::makeStringBlock(
    ndn::tlv::ApplicationParameters, encodeHistoricalKeyRequest(request)));
  signHistoricalKeyRequest(requestInterest, request, keys,
                           ndn::security::signingByCertificate(userCert));
  face.sentData.clear();
  ServiceControllerTestAccess::historicalKeyInterest(controller, requestInterest);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE_MESSAGE(!face.sentData.empty(), "the controller answered nothing");

  const auto& reply = face.sentData.back();
  const auto content = reply.getContent();
  const std::string body(reinterpret_cast<const char*>(content.value()), content.value_size());
  BOOST_CHECK_MESSAGE(body.find("NDNSF-HISTORICAL-DKEY-DENIED-v1") == std::string::npos,
                      "the controller denied: " + body.substr(0, 120));
  BOOST_CHECK_EQUAL(reply.getName(), requestInterest.getName());
  BOOST_CHECK(ndn::security::verifySignature(reply, controllerCert.getPublicKey()));
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();
}

BOOST_AUTO_TEST_CASE(ControllerIssuesGrantReceiptForGrantProtectedPublication)
{
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                           "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", "pib-sqlite3:", 1);
  ::setenv("NDN_CLIENT_TPM", "tpm-file:", 1);
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", (root / "controller-generation.state").c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /spec243/service } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /spec243/service } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto producerCert = keys.createIdentity(ndn::Name("/spec243/producer"),
                                                ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);

  ndn::Data publication = ndn::Data(ndn::Name("/spec243/producer")
    .append("NDNSF").append("OBJECT").append("grant-object"));
  publication.setContent(ndn::makeStringBlock(ndn::tlv::Content, std::string("payload")));
  keys.sign(publication, ndn::security::signingByCertificate(producerCert));

  HistoricalGrantReceiptRequest request;
  request.controllerPrefix = authority;
  request.publicationWire.assign(publication.wireEncode().begin(), publication.wireEncode().end());
  request.grantDigest = "sha256:" + std::string(64, 'a');
  request.recipient = ndn::Name("/spec243/user");
  // Must be the service the Controller's policy actually grants this Producer.
  request.service = "/spec243/service";
  request.currentControllerVersion = controller.getControllerVersion();
  const auto body = encodeHistoricalGrantReceiptRequest(request);
  const ndn::Block parameters(ndn::makeStringBlock(ndn::tlv::ApplicationParameters, body));
  ndn::Interest interest(makeControlRpcName(authority, "PUBLISH-HISTORY", parameters,
                                            makeControlQueryNonce()));
  interest.setApplicationParameters(parameters);
  ndn::security::InterestSigner signer(keys);
  signer.makeSignedInterest(interest, ndn::security::signingByCertificate(producerCert));
  face.sentData.clear();
  ServiceControllerTestAccess::publicationReceiptInterest(controller, interest);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE_MESSAGE(!face.sentData.empty(), "the controller issued no grant receipt");
  const auto& ack = face.sentData.back();
  BOOST_CHECK_EQUAL(ack.getName(), interest.getName());
  BOOST_CHECK(ndn::security::verifySignature(ack, controllerCert.getPublicKey()));
  // The acknowledgement names the receipt; a reader must be able to fetch that exact object back.
  const auto receiptName = ndn::Name(ndn::readString(ack.getContent()));
  face.sentData.clear();
  ServiceControllerTestAccess::receiptFetchInterest(controller, ndn::Interest(receiptName));
  for (int i = 0; i < 50 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE_MESSAGE(face.sentData.size() == 1,
                        "the acknowledged receipt was not retrievable: replies="
                          << face.sentData.size() << " expected=" << receiptName);
  const auto fetched = face.sentData.back();
  // The request named the FullName, so the reply must match it including the implicit digest.
  BOOST_CHECK_EQUAL(fetched.getFullName(), receiptName);
  const auto decodedReceipt = decodeHistoricalGrantReceipt(fetched, authority);
  BOOST_REQUIRE(decodedReceipt);
  BOOST_CHECK_EQUAL(decodedReceipt->service, "/spec243/service");
  BOOST_CHECK(ndn::security::verifySignature(fetched, controllerCert.getPublicKey()));
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();

  // Admission is part of the receipt, not of the transport: a Producer may not be receipted for a
  // namespace it does not own, nor for a service its permissions do not cover.
  const auto askFor = [&](const ndn::Data& subject, const std::string& service) {
    HistoricalGrantReceiptRequest attempt;
    attempt.controllerPrefix = authority;
    attempt.publicationWire.assign(subject.wireEncode().begin(), subject.wireEncode().end());
    attempt.grantDigest = "sha256:" + std::string(64, 'a');
    attempt.recipient = ndn::Name("/spec243/user");
    attempt.service = service;
    attempt.currentControllerVersion = controller.getControllerVersion();
    const auto attemptBody = encodeHistoricalGrantReceiptRequest(attempt);
    const ndn::Block attemptParameters(
      ndn::makeStringBlock(ndn::tlv::ApplicationParameters, attemptBody));
    ndn::Interest attemptInterest(makeControlRpcName(authority, "PUBLISH-HISTORY",
                                                     attemptParameters,
                                                     makeControlQueryNonce()));
    attemptInterest.setApplicationParameters(attemptParameters);
    ndn::security::InterestSigner attemptSigner(keys);
    attemptSigner.makeSignedInterest(attemptInterest,
                                     ndn::security::signingByCertificate(producerCert));
    ServiceControllerTestAccess::publicationReceiptInterest(controller, attemptInterest);
    for (int i = 0; i < 50; ++i)
      face.processEvents(ndn::time::milliseconds(5));
    const auto replies = face.sentData.size();
    face.sentData.clear();
    return replies;
  };
  {
    // Signed by the Producer, but named inside another identity's namespace.
    ndn::Data foreign(ndn::Name("/spec243/other").append("NDNSF").append("OBJECT").append("x"));
    foreign.setContent(ndn::makeStringBlock(ndn::tlv::Content, std::string("payload")));
    keys.sign(foreign, ndn::security::signingByCertificate(producerCert));
    BOOST_CHECK_EQUAL(askFor(foreign, "/spec243/service"), 0U);
  }
  {
    // The Producer owns the namespace but is not granted this service.
    BOOST_CHECK_EQUAL(askFor(publication, "/spec243/unpermitted"), 0U);
  }
}

BOOST_AUTO_TEST_CASE(ProviderBuildsAGrantReceiptRequestTheControllerCanParse)
{
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", ("pib-sqlite3:" + (root / ".ndn").string()).c_str(), 1);
  ::setenv("NDN_CLIENT_TPM", ("tpm-file:" + (root / ".ndn").string()).c_str(), 1);
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto producerCert = keys.createIdentity(ndn::Name("/spec243/producer"),
                                                ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();

  HistoricalGrantReceiptRequest request;
  request.controllerPrefix = controllerCert.getIdentity();
  request.publicationWire = "publication-wire-bytes";
  request.grantDigest = "sha256:" + std::string(64, 'a');
  request.recipient = ndn::Name("/spec243/user");
  request.service = "/spec243/service";
  request.currentControllerVersion = ControllerVersion{1000, 1};

  const auto interest = makeHistoricalGrantReceiptRequestInterest(request, producerCert, keys);
  BOOST_REQUIRE(interest.hasApplicationParameters());
  const auto& parameters = interest.getApplicationParameters();
  const std::string body(reinterpret_cast<const char*>(parameters.value()),
                         parameters.value_size());
  HistoricalGrantReceiptRequest decoded;
  BOOST_REQUIRE(decodeHistoricalGrantReceiptRequest(body, decoded));
  BOOST_CHECK_EQUAL(decoded.service, request.service);
  BOOST_CHECK_EQUAL(decoded.grantDigest, request.grantDigest);
  BOOST_CHECK(decoded.recipient == request.recipient);
  BOOST_CHECK_EQUAL(decoded.publicationWire, request.publicationWire);
  BOOST_CHECK(decoded.currentControllerVersion == request.currentControllerVersion);
  // The Controller only accepts this under its control-RPC parser, so the name has to carry the
  // operation and the parameters digest rather than being a free-form name.
}

BOOST_AUTO_TEST_CASE(HistoricalDkeyRefusesEvidenceSignedByAnotherProducer)
{
  // Same construction as the denial case, but the evidence validates (ValidatorNull) while its signer
  // is not the producer the request names - the branch that remained open.
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM", "NDNSF_FILE_KEYRING",
                           "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", ("pib-sqlite3:" + (root / ".ndn").string()).c_str(), 1);
  ::setenv("NDN_CLIENT_TPM", ("tpm-file:" + (root / ".ndn").string()).c_str(), 1);
  const auto keyring = root / "file-keyring";
  std::ofstream(keyring) << "controller-abe "
    << "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
  std::filesystem::permissions(keyring, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
  ::setenv("NDNSF_FILE_KEYRING", keyring.c_str(), 1);
  const auto stateFile = root / "controller-generation.state";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", stateFile.c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/producer allow { /HELLO } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /HELLO } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto requesterCert = keys.createIdentity(ndn::Name("/spec243/requester"),
                                                 ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto otherCert = keys.createIdentity(ndn::Name("/spec243/other"),
                                             ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::security::ValidatorNull validator;
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  const auto version = controller.getControllerVersion();
  const auto nowMs = static_cast<uint64_t>(ndn::time::toUnixTimestamp(
    ndn::time::system_clock::now()).count());

  HistoricalKeyRequest request;
  request.controllerPrefix = authority;
  request.generation = version.controllerGenerationTimestamp;
  request.epoch = version.controllerEpoch;
  request.currentControllerVersion = version;
  request.policy = "/PERMISSION/HELLO";
  request.producerName = ndn::Name("/spec243/producer");
  request.serviceName = ndn::Name("/spec243/service");
  request.objectName = request.producerName;
  request.objectName.append("objects").append("1");
  request.ckName = makeHistoricalCkNameForTest(request.producerName, request.serviceName,
                                               request.policy);
  request.recipientCertificate = requesterCert.getName();
  request.mintedAtMs = nowMs;
  request.expiresAtMs = nowMs + 5000;

  ndn::Data ck(ndn::Name(request.ckName));
  keys.sign(ck, ndn::security::signingByCertificate(otherCert));   // not the named producer
  const auto ckWire = ck.wireEncode();
  request.ckEvidence.assign(ckWire.begin(), ckWire.end());

  HistoricalPublication publication;
  publication.controller = request.controllerPrefix;
  publication.producer = request.producerName;
  publication.service = request.serviceName;
  publication.objectName = request.objectName;
  publication.ckFullName = ck.getFullName();
  publication.generation = request.generation;
  publication.epoch = request.epoch;
  publication.policyGeneration = version.controllerGenerationTimestamp;
  publication.policyEpoch = version.controllerEpoch;
  publication.segments = 1;
  publication.policy = request.policy;
  publication.objectDigest = "sha256:" + std::string(64, 'b');
  publication.publicParametersName =
    ndn::Name("/spec243/controller").append("NDNSF").append("ABE").append("PUBPARAMS").append("1");
  publication.publicParametersDigest = "sha256:" + std::string(64, 'c');
  const auto publicationData = makeHistoricalPublicationData(
    publication, keys, ndn::security::signingByCertificate(requesterCert));
  const auto publicationWire = publicationData.wireEncode();
  request.publicationEvidence.assign(publicationWire.begin(), publicationWire.end());

  ndn::Interest interest(makeHistoricalKeyRequestName(authority, request));
  const auto parameters = encodeHistoricalKeyRequest(request);
  interest.setApplicationParameters(
    ndn::makeStringBlock(ndn::tlv::ApplicationParameters, parameters));
  ndn::security::InterestSigner signer(keys);
  signer.makeSignedInterest(interest, ndn::security::signingByCertificate(requesterCert));
  face.sentData.clear();
  ServiceControllerTestAccess::historicalKeyInterest(controller, interest);
  for (int i = 0; i < 100 && face.sentData.empty(); ++i)
    face.processEvents(ndn::time::milliseconds(5));
  BOOST_REQUIRE(!face.sentData.empty());
  const auto content = face.sentData.back().getContent();
  const std::string denial(reinterpret_cast<const char*>(content.value()), content.value_size());
  BOOST_CHECK(denial.find("ck-evidence-producer") != std::string::npos);
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();
}

BOOST_AUTO_TEST_CASE(HistoricalDkeyRefusesAbsentAndMismatchedCkEvidence)
{
  // Self-contained: the trust schema of the deployment is what makes a DKEY request acceptable, and
  // reproducing it here would test the schema rather than the conflict checks. ValidatorNull accepts
  // the signature so the handler reaches the evidence checks under test.
  const auto root = test::trust_detail::makeTestTrustDirectory();
  std::map<std::string, std::optional<std::string>> previous;
  const auto restore = ndn::make_scope_exit([&] {
    for (const auto& [name, value] : previous) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  for (const auto* name : {"HOME", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM", "NDNSF_FILE_KEYRING",
                                    "NDNSF_CONTROLLER_GENERATION_STATE"}) {
    const auto* old = ::getenv(name);
    previous[name] = old ? std::optional<std::string>(old) : std::nullopt;
  }
  ::setenv("HOME", root.c_str(), 1);
  ::setenv("NDN_CLIENT_PIB", ("pib-sqlite3:" + (root / ".ndn").string()).c_str(), 1);
  ::setenv("NDN_CLIENT_TPM", ("tpm-file:" + (root / ".ndn").string()).c_str(), 1);
  // The Controller seals its generation state with the "controller-abe" key ring entry, so a
  // controller without one cannot become entitled; this case provides the ring it needs.
  const auto keyring = root / "file-keyring";
  std::ofstream(keyring) << "controller-abe "
    << "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n";
  std::filesystem::permissions(keyring, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
  ::setenv("NDNSF_FILE_KEYRING", keyring.c_str(), 1);
  // Without this the controller falls back to a fixed /tmp state file whose ownership it has to
  // accept from a previous run, which makes entitlement depend on leftovers rather than the case.
  const auto stateFile = root / "controller-generation.state";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", stateFile.c_str(), 1);
  const auto policy = root / "policy";
  std::ofstream(policy) <<
    "provider-policies { provider-policy { for /spec243/provider allow { /HELLO } } }\n"
    "user-policies { user-policy { for /spec243/user allow { /HELLO } } }\n";
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/spec243/controller"),
                                                  ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto requesterCert = keys.createIdentity(ndn::Name("/spec243/requester"),
                                                 ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  // Accepts the request, rejects every evidence Data packet - what the signature branch needs.
  ndn::security::Validator validator(std::make_unique<RejectEvidenceDataPolicy>(),
                                     std::make_unique<ndn::security::CertificateFetcherOffline>());
  ServiceController controller(face, controllerCert, validator, policy.string());
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  const auto authority = controllerCert.getIdentity();
  controller.setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  const auto version = controller.getControllerVersion();

  const auto nowMs = static_cast<uint64_t>(ndn::time::toUnixTimestamp(
    ndn::time::system_clock::now()).count());
  const auto makeRequest = [&] {
    HistoricalKeyRequest request;
    request.controllerPrefix = authority;
    request.generation = version.controllerGenerationTimestamp;
    request.epoch = version.controllerEpoch;
    request.currentControllerVersion = version;
    request.policy = "/PERMISSION/HELLO";
    request.producerName = ndn::Name("/spec243/producer");
    request.serviceName = ndn::Name("/spec243/service");
    request.ckName = makeHistoricalCkNameForTest(request.producerName, request.serviceName,
                                                 request.policy);
    // The publication binding requires the object and the public parameters to live under their
    // respective owners, so both names are derived from the producer and the controller.
    request.objectName = ndn::Name("/spec243/producer").append("objects").append("1");
    request.recipientCertificate = requesterCert.getName();
    request.mintedAtMs = nowMs;
    request.expiresAtMs = nowMs + 5000;
    return request;
  };
  const auto denialFor = [&] (const HistoricalKeyRequest& request) {
    ndn::Interest interest(makeHistoricalKeyRequestName(authority, request));
    const auto parameters = encodeHistoricalKeyRequest(request);
    interest.setApplicationParameters(
      ndn::makeStringBlock(ndn::tlv::ApplicationParameters, parameters));
    ndn::security::InterestSigner signer(keys);
    signer.makeSignedInterest(interest, ndn::security::signingByCertificate(requesterCert));
    face.sentData.clear();
    ServiceControllerTestAccess::historicalKeyInterest(controller, interest);
    for (int i = 0; i < 100 && face.sentData.empty(); ++i)
      face.processEvents(ndn::time::milliseconds(5));
    if (face.sentData.empty())
      return std::string();
    const auto content = face.sentData.back().getContent();
    return std::string(reinterpret_cast<const char*>(content.value()), content.value_size());
  };

  const auto absentRequest = makeRequest();
  const auto absent = denialFor(absentRequest);
  BOOST_REQUIRE(!absent.empty());
  BOOST_CHECK(absent.find("NDNSF-HISTORICAL-DKEY-DENIED-v1\nck-evidence-required") == 0);

  // A replay of the same request re-publishes the *stored* denial rather than minting a new one
  // (serveHistoricalTerminal replays the persisted reply), so the bytes must be identical.
  const auto replayed = denialFor(absentRequest);
  BOOST_REQUIRE(!replayed.empty());
  BOOST_CHECK_EQUAL(replayed, absent);


  auto malformed = makeRequest();
  malformed.ckEvidence = {0x05, 0x01};
  malformed.publicationEvidence = {0x05, 0x02};
  const auto malformedDenial = denialFor(malformed);
  BOOST_REQUIRE(!malformedDenial.empty());
  BOOST_CHECK(malformedDenial.find("ck-evidence-malformed") != std::string::npos);

  // Evidence that decodes but names a different CK than the request claims. The name check runs
  // before any signature validation, so an unsigned packet is enough to reach it.
  auto mismatched = makeRequest();
  ndn::Data otherCk(ndn::Name("/spec243/other/CK"));
  keys.sign(otherCk, ndn::security::signingByCertificate(requesterCert));
  const auto otherCkWire = otherCk.wireEncode();
  mismatched.ckEvidence.assign(otherCkWire.begin(), otherCkWire.end());
  ndn::Data otherPublication(ndn::Name("/spec243/other/PUBLICATION"));
  keys.sign(otherPublication, ndn::security::signingByCertificate(requesterCert));
  const auto otherPublicationWire = otherPublication.wireEncode();
  mismatched.publicationEvidence.assign(otherPublicationWire.begin(), otherPublicationWire.end());
  const auto mismatchedDenial = denialFor(mismatched);
  BOOST_REQUIRE(!mismatchedDenial.empty());
  BOOST_CHECK(mismatchedDenial.find("ck-evidence-name") != std::string::npos);

  // A well-formed CK and a publication mapping that names it: this passes the naming and binding
  // checks and fails only when the evidence signature is validated, which the test policy rejects.
  auto signedCase = makeRequest();
  ndn::Data ck(ndn::Name(signedCase.ckName));
  keys.sign(ck, ndn::security::signingByCertificate(requesterCert));
  const auto ckWire = ck.wireEncode();
  signedCase.ckEvidence.assign(ckWire.begin(), ckWire.end());
  HistoricalPublication publication;
  publication.controller = signedCase.controllerPrefix;
  publication.producer = signedCase.producerName;
  publication.service = signedCase.serviceName;
  publication.objectName = signedCase.objectName;
  publication.ckFullName = ck.getFullName();
  publication.generation = signedCase.generation;
  publication.epoch = signedCase.epoch;
  publication.policyGeneration = signedCase.currentControllerVersion.controllerGenerationTimestamp;
  publication.policyEpoch = signedCase.currentControllerVersion.controllerEpoch;
  publication.segments = 1;
  publication.policy = signedCase.policy;
  publication.objectDigest = "sha256:" + std::string(64, 'b');
  publication.publicParametersName =
    ndn::Name("/spec243/controller").append("NDNSF").append("ABE").append("PUBPARAMS").append("1");
  publication.publicParametersDigest = "sha256:" + std::string(64, 'c');
  const auto publicationData = makeHistoricalPublicationData(
    publication, keys, ndn::security::signingByCertificate(requesterCert));
  const auto publicationWire = publicationData.wireEncode();
  signedCase.publicationEvidence.assign(publicationWire.begin(), publicationWire.end());
  const auto signedDenial = denialFor(signedCase);
  BOOST_REQUIRE(!signedDenial.empty());
  BOOST_CHECK(signedDenial.find("ck-evidence-signature") != std::string::npos);

  // The handler validates the request through the face's IO context. Drain it before this case's
  // controller and face are destroyed, otherwise a late callback can outlive them and abort an
  // unrelated case later in the same binary.
  for (int i = 0; i < 50; ++i)
    face.processEvents(ndn::time::milliseconds(2));
  face.sentData.clear();
}

BOOST_FIXTURE_TEST_CASE(PermissionRepliesBindExactRecipientAndPreserveEligibleGrant,
                       PortableRecipientFixture)
{
  const auto authority = ndn::Name(subject).append("controller");
  const auto authorityCert = localKeys.getPib().getIdentity(authority).getDefaultKey()
                              .getDefaultCertificate();
  controller->setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(*controller);
  for (const auto user : {true, false}) {
    const auto target = user ? subject : ndn::Name(subject).append("provider");
    const auto identity = localKeys.createIdentity(target, ndn::RsaKeyParams(2048));
    const auto defaultKey = identity.getDefaultKey();
    const auto oldCert = defaultKey.getDefaultCertificate();
    const auto alternate = localKeys.createKey(identity, ndn::RsaKeyParams(2048))
                             .getDefaultCertificate();
    localKeys.setDefaultKey(identity, defaultKey);
    const auto digest = ServiceControllerTestAccess::certificateDigest(oldCert);
    BOOST_REQUIRE(controller->revoke(makeCertificateRevocation(target.toUri().c_str(),
                                                               digest.c_str())));
    const std::string operation = user ? "USER-PERMISSIONS" : "PROVIDER-PERMISSIONS";
    const auto request = [&] (const ndn::Name& recipient) {
      const auto parameters = encodeControlQueryParameters({target, {}, 0, 0, recipient});
      ndn::Interest interest(makeControlRpcName(authority, operation, parameters,
                                                makeControlQueryNonce()));
      interest.setApplicationParameters(parameters);
      face.sentData.clear();
      if (user)
        ServiceControllerTestAccess::userPermissionsInterest(*controller, interest);
      else
        ServiceControllerTestAccess::providerPermissionsInterest(*controller, interest);
      face.processEvents(ndn::time::milliseconds(1));
      return interest.getName();
    };
    const auto receive = [&] (const ndn::security::Certificate& cert) {
      const auto expected = request(cert.getFullName());
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      const auto& data = face.sentData.front();
      BOOST_CHECK_EQUAL(data.getName(), expected);
      BOOST_CHECK(ndn::security::verifySignature(data, authorityCert.getPublicKey()));
      EncryptedPermissionResponse encrypted;
      BOOST_REQUIRE(encrypted.WireDecode(data.getContent().blockFromValue()));
      BOOST_CHECK(ndn::Name(encrypted.getRecipientCertName()) == cert.getName());
      return decryptPermissionResponseWithKeyChain(encrypted, localKeys);
    };
    const auto allowed = receive(alternate);
    BOOST_REQUIRE_EQUAL(allowed.getEntries().size(), 1U);
    BOOST_CHECK(ndn::Name(allowed.getEntries().front().getServiceName()) == ndn::Name("/HELLO"));
    const auto record = user ? ServiceControllerTestAccess::loadGrantRecord(
      *controller, target, allowed.getControllerVersion()) : std::nullopt;
    if (user)
      BOOST_REQUIRE(record);
    BOOST_CHECK(receive(oldCert).getEntries().empty());
    if (user)
      BOOST_CHECK(ServiceControllerTestAccess::loadGrantRecord(
        *controller, target, allowed.getControllerVersion()) == record);
    std::array<uint8_t, 32> wrongDigest{};
    request(ndn::Name(alternate.getName()).appendImplicitSha256Digest(wrongDigest));
    BOOST_CHECK(face.sentData.empty());
    BOOST_CHECK_THROW(encodeControlQueryParameters(
      {authority, {}, 0, 0, alternate.getFullName()}), std::invalid_argument);
  }
}

BOOST_FIXTURE_TEST_CASE(ColdPermissionCertificateFetchBindsDigestAndCurrentGeneration,
                       PortableRecipientFixture)
{
  const auto authority = ndn::Name(subject).append("controller");
  const auto authorityCert = localKeys.getPib().getIdentity(authority).getDefaultKey()
                              .getDefaultCertificate();
  test::trust_detail::writeTestTrustSchema(root, {authorityCert});
  validator.load((root / "trust.conf").string());
  controller->setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(*controller);
  const auto local = remoteKeys.createIdentity(subject, ndn::RsaKeyParams(2048))
                       .getDefaultKey().getDefaultCertificate();
  ndn::Data issued(local);
  auto name = ndn::Name(local.getKeyName()).append("controller-issued");
  name.appendVersion(local.getName().at(-1).toVersion());
  issued.setName(name);
  ndn::SignatureInfo info;
  info.setValidityPeriod(local.getValidityPeriod());
  auto signing = ndn::security::signingByCertificate(authorityCert);
  signing.setSignatureInfo(info);
  localKeys.sign(issued, signing);
  const ndn::security::Certificate cert(issued);
  const auto params = encodeControlQueryParameters({subject, {}, 0, 0, cert.getFullName()});
  const auto request = [&] {
    ndn::Interest interest(makeControlRpcName(authority, "USER-PERMISSIONS", params,
                                              makeControlQueryNonce()));
    interest.setApplicationParameters(params);
    face.sentData.clear();
    face.sentInterests.clear();
    ServiceControllerTestAccess::userPermissionsInterest(*controller, interest);
    face.processEvents(ndn::time::milliseconds(2));
    return interest.getName();
  };
  request();
  BOOST_REQUIRE_EQUAL(std::count_if(face.sentInterests.begin(), face.sentInterests.end(),
    [&] (const auto& sent) {
      return !sent.getCanBePrefix() && sent.getName() == cert.getFullName();
    }), 1);
  BOOST_CHECK(face.sentData.empty());
  BOOST_REQUIRE(controller->revoke(makeIdentityRevocation("/unrelated/credential")));
  face.receive(cert);
  face.processEvents(ndn::time::milliseconds(2));
  BOOST_CHECK(face.sentData.empty());

  const auto replyName = request();
  if (face.sentData.empty()) {
    BOOST_REQUIRE_EQUAL(std::count_if(face.sentInterests.begin(), face.sentInterests.end(),
      [&] (const auto& sent) { return sent.getName() == cert.getFullName(); }), 1);
    face.receive(cert);
    face.processEvents(ndn::time::milliseconds(2));
  }
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  const auto& reply = face.sentData.front();
  BOOST_CHECK_EQUAL(reply.getName(), replyName);
  BOOST_CHECK(ndn::security::verifySignature(reply, authorityCert.getPublicKey()));
  EncryptedPermissionResponse encrypted;
  BOOST_REQUIRE(encrypted.WireDecode(reply.getContent().blockFromValue()));
  BOOST_CHECK(ndn::Name(encrypted.getRecipientCertName()) == cert.getName());
  const auto response = decryptPermissionResponseWithKeyChain(encrypted, remoteKeys);
  BOOST_CHECK_EQUAL(response.getEntries().size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(PolicyStatusRenewsOnlyForAUniqueQuery, PortableRecipientFixture)
{
  const auto authority = ndn::Name(subject).append("controller");
  const auto authorityCert = localKeys.getPib().getIdentity(authority).getDefaultKey()
                              .getDefaultCertificate();
  const ndn::Name service("/HELLO");
  controller->setControllerPrefix(authority);
  ServiceControllerTestAccess::registerHandlersForDummyFace(*controller);
  const auto fetch = [&] (const ndn::Interest& query) {
    face.sentData.clear();
    ServiceControllerTestAccess::policyStatusInterest(*controller, query);
    face.processEvents(ndn::time::milliseconds(2));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    const auto data = face.sentData.back();
    BOOST_CHECK(data.getName() == query.getName());
    BOOST_CHECK(ndn::security::verifySignature(data, authorityCert.getPublicKey()));
    return data;
  };
  const auto query = ServiceControllerTestAccess::statusQuery(authority, service);
  const auto first = fetch(query);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  BOOST_CHECK(fetch(query).wireEncode() == first.wireEncode());
  const auto next = fetch(ServiceControllerTestAccess::statusQuery(authority, service));
  PolicyStatusData before, after;
  BOOST_REQUIRE(before.wireDecode(first.getContent().blockFromValue()));
  BOOST_REQUIRE(after.wireDecode(next.getContent().blockFromValue()));
  BOOST_CHECK_EQUAL(before.getValidUntilMs() - before.getValidFromMs(), 60000U);
  BOOST_CHECK_GT(after.getValidUntilMs(), before.getValidUntilMs());
  BOOST_CHECK(after.canReplaceSameVersion(before, true));
  BOOST_CHECK(!after.canReplaceSameVersion(before, false));
}

BOOST_FIXTURE_TEST_CASE(ControllerRequiresDedicatedSealAndPinnedLegacyImport,
                       PortableRecipientFixture)
{
  const auto authority = ndn::Name(subject).append("controller");
  const auto certificate = localKeys.getPib().getIdentity(authority).getDefaultKey()
                            .getDefaultCertificate();
  const auto* configuredRing = std::getenv("NDNSF_FILE_KEYRING");
  BOOST_REQUIRE(configuredRing);
  const std::string ring(configuredRing);
  auto restore = ndn::make_scope_exit([&] {
    ::setenv("NDNSF_FILE_KEYRING", ring.c_str(), 1);
    ::unsetenv("NDNSF_CONTROLLER_RECOVERY_CHECKPOINT");
    ::unsetenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256");
    ::unsetenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_WITNESS_SHA256");
    ::unsetenv("NDNSF_CONTROLLER_RECOVERY_BOOTSTRAP");
  });
  controller.reset();
  const auto stateDirectory = root / "state";
  const auto checkpointDirectory = root / "checkpoint";
  for (const auto& directory : {stateDirectory, checkpointDirectory}) {
    std::filesystem::create_directory(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  }
  const auto state = stateDirectory / "generation";
  const auto checkpoint = checkpointDirectory / "current";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", state.c_str(), 1);
  const auto wrongRing = root / "wrong-id.ring";
  std::ofstream(wrongRing) << "requester-only " << std::string(64, 'c') << '\n';
  std::filesystem::permissions(wrongRing, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
  ::unsetenv("NDNSF_FILE_KEYRING");
  {
    ServiceController withoutRing(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!withoutRing.getControllerVersion().isValid());
    BOOST_CHECK(!std::filesystem::exists(state));
    BOOST_CHECK(!std::filesystem::exists(state.string() + ".lock"));
  }
  ::setenv("NDNSF_FILE_KEYRING", wrongRing.c_str(), 1);
  {
    ServiceController wrongId(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!wrongId.getControllerVersion().isValid());
    BOOST_CHECK(!std::filesystem::exists(state));
    BOOST_CHECK(!std::filesystem::exists(state.string() + ".lock"));
  }
  const auto readState = [&] {
    std::ifstream input(state, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  {
    ControllerGenerationStore legacy(state);
    BOOST_REQUIRE(legacy.acquireWriter(authority.toUri(ndn::name::UriFormat::CANONICAL)));
    legacy.startGeneration(1000);
  }
  const auto previous = readState();
  ::setenv("NDNSF_FILE_KEYRING", ring.c_str(), 1);
  {
    ServiceController unpinned(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!unpinned.getControllerVersion().isValid());
    BOOST_CHECK_EQUAL(readState(), previous);
  }
  ndn::util::Sha256 sha;
  sha << previous;
  const auto importDigest = "sha256:" + sha.toString();
  ::setenv("NDNSF_CONTROLLER_RECOVERY_CHECKPOINT", checkpoint.c_str(), 1);
  ::setenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256", importDigest.c_str(), 1);
  {
    ServiceController migrated(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(migrated.getControllerVersion().isValid());
    BOOST_CHECK_EQUAL(readState().substr(0, 8), "NDNSFCS9");
    BOOST_CHECK(std::filesystem::exists(checkpoint));
  }
  ::unsetenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256");
  {
    ServiceController reopened(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(reopened.getControllerVersion().isValid());
  }
  std::filesystem::rename(state, stateDirectory / "saved-generation");
  std::filesystem::rename(checkpoint, checkpointDirectory / "saved-current");
  {
    ServiceController missing(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!missing.getControllerVersion().isValid());
    BOOST_CHECK(!std::filesystem::exists(state));
    BOOST_CHECK(!std::filesystem::exists(checkpoint));
  }
  ::setenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256", importDigest.c_str(), 1);
  {
    ServiceController missingImport(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!missingImport.getControllerVersion().isValid());
    BOOST_CHECK(!std::filesystem::exists(state));
    BOOST_CHECK(!std::filesystem::exists(checkpoint));
  }
  ::unsetenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256");
  const auto freshState = stateDirectory / "new-generation";
  const auto freshCheckpoint = checkpointDirectory / "new-current";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", freshState.c_str(), 1);
  ::setenv("NDNSF_CONTROLLER_RECOVERY_CHECKPOINT", freshCheckpoint.c_str(), 1);
  ::setenv("NDNSF_CONTROLLER_RECOVERY_BOOTSTRAP", "1", 1);
  {
    ServiceController fresh(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(fresh.getControllerVersion().isValid());
    BOOST_CHECK(std::filesystem::exists(freshState));
    BOOST_CHECK(std::filesystem::exists(freshCheckpoint));
  }
  ::unsetenv("NDNSF_CONTROLLER_RECOVERY_BOOTSTRAP");
  ControllerVersion warmVersion;
  {
    ServiceController warm(face, certificate, validator, (root / "policy").string());
    warmVersion = warm.getControllerVersion();
    BOOST_REQUIRE(warmVersion.isValid());
  }

  std::array<uint8_t, 32> grantDigest{};
  grantDigest.fill(0x56);
  const ndn::span<const uint8_t> body(grantDigest.data(), grantDigest.size());
  ndn::util::Sha256 bodyHash;
  bodyHash << body;
  const auto nameDigest = bodyHash.computeDigest();
  auto name = makeProducerDataPrefix(authority, "HISTORICAL-GRANT-WITNESS");
  name.appendVersion(warmVersion.controllerGenerationTimestamp)
      .appendVersion(warmVersion.controllerEpoch)
      .append(ndn::name::Component(ndn::span<const uint8_t>(nameDigest->data(),
                                                          nameDigest->size())));
  ndn::Data witness(name);
  witness.setContent(body);
  localKeys.sign(witness, ndn::security::signingByCertificate(certificate));
  const auto& wire = witness.wireEncode();
  ndn::util::Sha256 signerHash;
  signerHash << certificate.getPublicKey();
  const auto oldWitness = freshState.string() + ".grant-witness." + signerHash.toString();
  std::ofstream(oldWitness, std::ios::binary)
    .write(reinterpret_cast<const char*>(wire.data()), wire.size());
  std::filesystem::permissions(oldWitness, std::filesystem::perms::owner_read |
                                            std::filesystem::perms::owner_write);
  {
    ServiceController unpinned(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!unpinned.getControllerVersion().isValid());
  }
  const auto wrongDigest = "sha256:" + std::string(64, '0');
  ::setenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_WITNESS_SHA256", wrongDigest.c_str(), 1);
  {
    ServiceController wrongWitness(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(!wrongWitness.getControllerVersion().isValid());
  }
  ndn::util::Sha256 wireHash;
  wireHash << ndn::span<const uint8_t>(wire.data(), wire.size());
  const auto witnessDigest = "sha256:" + wireHash.toString();
  ::setenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_WITNESS_SHA256", witnessDigest.c_str(), 1);
  {
    ServiceController migrated(face, certificate, validator, (root / "policy").string());
    BOOST_CHECK(migrated.getControllerVersion().compare(warmVersion) > 0);
  }
  ::unsetenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_WITNESS_SHA256");
  std::filesystem::remove(oldWitness);
  ServiceController restored(face, certificate, validator, (root / "policy").string());
  BOOST_CHECK(restored.getControllerVersion().isValid());
}

BOOST_FIXTURE_TEST_CASE(DkeyRevocationChecksActualRecipientAndCommittedAttributes,
                       MultiServiceRecipientFixture)
{
  const auto recipient = localKeys.createIdentity(subject, ndn::RsaKeyParams(2048));
  const auto oldKey = recipient.getDefaultKey();
  const auto revoked = oldKey.getDefaultCertificate();
  const auto replacement = localKeys.createKey(recipient, ndn::RsaKeyParams(2048))
                           .getDefaultCertificate();
  localKeys.setDefaultKey(recipient, oldKey);
  const auto authority = ndn::Name(subject).append("controller");
  const auto authorityCertificate = localKeys.getPib().getIdentity(authority).getDefaultKey()
                                    .getDefaultCertificate();
  const auto queryFor = [&] (const auto& certificate) {
    return ndn::Interest(ServiceControllerTestAccess::dkeyInterestPrefix(authority, certificate))
      .setCanBePrefix(true);
  };
  const auto request = [&] (const ndn::Interest& interest) {
    face.sentData.clear();
    ServiceControllerTestAccess::requestDkey(*controller, interest);
    face.processEvents(ndn::time::milliseconds(1));
  };
  const auto originalQuery = queryFor(revoked);
  request(originalQuery);
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  const ndn::Interest oldContinuation(face.sentData.front().getName());
  const auto digest = ServiceControllerTestAccess::certificateDigest(revoked);
  BOOST_REQUIRE(controller->revoke(makeCertificateRevocation(subject.toUri().c_str(),
                                                              digest.c_str())));
  BOOST_REQUIRE(localKeys.getPib().getIdentity(subject).getDefaultKey().getName() ==
                revoked.getKeyName());
  request(originalQuery);
  BOOST_CHECK(face.sentData.empty());
  request(oldContinuation);
  BOOST_CHECK(face.sentData.empty());

  const auto readReplacementBundle = [&] {
    const auto replacementQuery = queryFor(replacement);
    request(replacementQuery);
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    const auto first = face.sentData.front();
    BOOST_CHECK(replacementQuery.getName().isPrefixOf(first.getName()));
    BOOST_CHECK(ndn::security::verifySignature(first, authorityCertificate.getPublicKey()));
    const auto last = first.getFinalBlock();
    BOOST_REQUIRE(last && last->isSegment());
    const auto bundle = first.getName().getPrefix(-1);
    ndn::Buffer encrypted(first.getContent().value_begin(), first.getContent().value_end());
    for (uint64_t segment = 1; segment <= last->toSegment(); ++segment) {
      const auto name = ndn::Name(bundle).appendSegment(segment);
      request(ndn::Interest(name));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      const auto& packet = face.sentData.front();
      BOOST_CHECK_EQUAL(packet.getName(), name);
      BOOST_CHECK(ndn::security::verifySignature(packet, authorityCertificate.getPublicKey()));
      encrypted.insert(encrypted.end(), packet.getContent().value_begin(),
                       packet.getContent().value_end());
    }
    return ndn::Block(ndn::span<const uint8_t>(encrypted.data(), encrypted.size()));
  };
  const auto sealed = readReplacementBundle();
  const auto decoded = ndn::nacabe::decryptDataContent(sealed, localKeys.getTpm(),
                                                     replacement.getName());
  BOOST_REQUIRE(!decoded.empty());
  auto oldRecipientOpened = false;
  try {
    oldRecipientOpened = !ndn::nacabe::decryptDataContent(sealed, localKeys.getTpm(),
                                                          revoked.getName()).empty();
  }
  catch (const std::exception&) {
  }
  BOOST_CHECK(!oldRecipientOpened);
  ndn::nacabe::algo::PrivateKey key;
  key.fromBuffer(decoded);
  const auto ciphertext = ServiceControllerTestAccess::abeEncrypt(
    *controller, ndn::Name("/PERMISSION/HELLO"), "replacement-recipient");
  const auto plaintext = ServiceControllerTestAccess::abeDecrypt(*controller, key, ciphertext);
  BOOST_CHECK_EQUAL(std::string(plaintext.begin(), plaintext.end()), "replacement-recipient");

  RevocationTarget service;
  service.kind = RevocationKind::SERVICE_AUTHORIZATION;
  service.targetIdentity = subject;
  service.serviceName = ndn::Name("/HELLO");
  service.authorizationAttribute = ndn::Name("/PERMISSION/HELLO");
  BOOST_REQUIRE(controller->revoke(service));
  controller.reset();
  controller = std::make_unique<ServiceController>(face, authorityCertificate, validator,
                                                    (root / "policy").string());
  request(originalQuery);
  BOOST_CHECK(face.sentData.empty());
  const auto cold = readReplacementBundle();
  const auto coldKey = ndn::nacabe::decryptDataContent(cold, localKeys.getTpm(),
                                                      replacement.getName());
  BOOST_REQUIRE(!coldKey.empty());
  key.fromBuffer(coldKey);
  const auto blocked = ServiceControllerTestAccess::abeEncrypt(
    *controller, ndn::Name("/PERMISSION/HELLO"), "withdrawn-service");
  BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
    ServiceControllerTestAccess::abePublicParameters(*controller), key, blocked,
    "withdrawn-service"));
  const auto allowed = ServiceControllerTestAccess::abeEncrypt(
    *controller, ndn::Name("/PERMISSION/OTHER"), "remaining-service");
  const auto retained = ServiceControllerTestAccess::abeDecrypt(*controller, key, allowed);
  BOOST_CHECK_EQUAL(std::string(retained.begin(), retained.end()), "remaining-service");
}

BOOST_FIXTURE_TEST_CASE(PortableRecipientRejectsMissingExpiredEcAndWrongIdentity,
                       PortableRecipientFixture)
{
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, subject),
                    std::exception);
  const auto child = ndn::Name(subject).append("KEY").append("child");
  anchor(remoteKeys.createIdentity(child, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate());
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, subject),
                    std::exception);
  const auto ecIdentity = ndn::Name(subject).append("ec");
  anchor(remoteKeys.createIdentity(ecIdentity, ndn::EcKeyParams())
      .getDefaultKey().getDefaultCertificate());
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, ecIdentity),
                    std::exception);
  const auto expiredIdentity = ndn::Name(subject).append("expired");
  const auto key = remoteKeys.createIdentity(expiredIdentity,
      ndn::RsaKeyParams(2048)).getDefaultKey();
  auto expired = key.getDefaultCertificate();
  auto info = expired.getSignatureInfo();
  const auto now = ndn::time::system_clock::now();
  info.setValidityPeriod(ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                     now - ndn::time::hours(1)));
  auto signing = ndn::security::signingByKey(key);
  signing.setSignatureInfo(info);
  remoteKeys.sign(expired, signing);
  BOOST_REQUIRE(!expired.isValid());
  anchor(expired);
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, expiredIdentity),
                    std::exception);
  BOOST_CHECK_THROW(localKeys.getPib().getIdentity(subject), ndn::security::pib::Pib::Error);
  BOOST_CHECK(!ServiceControllerTestAccess::hasRecipientPrivateKey(*controller, key.getName()));
}

BOOST_FIXTURE_TEST_CASE(PortableRecipientPreservesPibPrecedenceAndRejectsExpiredPib,
                       PortableRecipientFixture)
{
  const auto key = localKeys.createIdentity(subject, ndn::RsaKeyParams(2048)).getDefaultKey();
  const auto certificate = key.getDefaultCertificate();
  const auto other = remoteKeys.createIdentity(subject, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  anchor(other);
  BOOST_CHECK(ServiceControllerTestAccess::recipientCertificate(*controller, subject)
      .wireEncode() == certificate.wireEncode());
  auto expired = certificate;
  expired.setName(ndn::Name(key.getName()).append("expired").appendVersion(1));
  auto info = expired.getSignatureInfo();
  const auto now = ndn::time::system_clock::now();
  info.setValidityPeriod(ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                     now - ndn::time::hours(1)));
  auto signing = ndn::security::signingByKey(key);
  signing.setSignatureInfo(info);
  localKeys.sign(expired, signing);
  ServiceControllerTestAccess::installRecipientCertificate(*controller, expired);
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, subject),
                    std::exception);
}

BOOST_FIXTURE_TEST_CASE(PortableRecipientDoesNotSelectPastFirstEcAnchor,
                       PortableRecipientFixture)
{
  const auto identity = remoteKeys.createIdentity(subject,
      ndn::EcKeyParams(ndn::name::Component("a")));
  const auto ec = identity.getDefaultKey().getDefaultCertificate();
  const auto rsa = remoteKeys.createKey(identity,
      ndn::RsaKeyParams(ndn::name::Component("z"), 2048)).getDefaultCertificate();
  BOOST_REQUIRE(ec.getName() < rsa.getName());
  anchor(ec);
  anchor(rsa);
  // The public anchor API returns the first prefix match, not a key-type selector.
  BOOST_CHECK_THROW(ServiceControllerTestAccess::recipientCertificate(*controller, subject),
                    std::exception);
}

BOOST_FIXTURE_TEST_CASE(PortableRecipientLostWriterRejectsReadinessAndProtectedIssuance,
                       PortableRecipientFixture)
{
  anchor(remoteKeys.createIdentity(subject, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate());
  controller->setControllerPrefix(ndn::Name(subject).append("controller"));
  ServiceControllerTestAccess::registerHandlersForDummyFace(*controller);
  const auto parameters = encodeControlQueryParameters({subject, ndn::Name(), 0, 0});
  ndn::Interest query(makeControlRpcName(ndn::Name(subject).append("controller"),
      "USER-PERMISSIONS", parameters, makeControlQueryNonce()));
  query.setApplicationParameters(parameters);
  face.sentData.clear();
  ServiceControllerTestAccess::userPermissionsInterest(*controller, query);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
  {
    std::ofstream replaced(root / "generation.lock", std::ios::trunc);
    replaced << "replacement\n999999\n";
  }
  BOOST_CHECK_EXCEPTION(controller->start(), std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("generation writer") != std::string::npos;
      });
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(*controller,
      ServiceControllerTestAccess::statusQuery(ndn::Name(subject).append("controller"),
                                               ndn::Name("/HELLO")));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());
  BOOST_CHECK(controller->getPolicyStatus(ndn::Name("/HELLO"))
      .getControllerVersion().isValid());
  ServiceControllerTestAccess::userPermissionsInterest(*controller, query);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());
}

BOOST_AUTO_TEST_CASE(PolicyStatusOriginalReplyCapacityExpiresWithoutRenamingOldReplies)
{
  struct StateGuard
  {
    std::optional<std::string> previous;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("spec205-status-capacity-" + std::to_string(::getpid()) + ".bin");
    std::shared_ptr<ndn::time::UnitTestSystemClock> system =
      std::make_shared<ndn::time::UnitTestSystemClock>();
    StateGuard()
    {
      if (const auto* value = ::getenv("NDNSF_CONTROLLER_GENERATION_STATE")) previous = value;
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", path.c_str(), 1);
      system->setNow(ndn::time::system_clock::now().time_since_epoch());
      ndn::time::setCustomClocks(nullptr, system);
    }
    ~StateGuard()
    {
      ndn::time::setCustomClocks();
      if (previous) ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", previous->c_str(), 1);
      else ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".lock", ignored);
    }
  } state;
  ndn::KeyChain keys;
  const auto certificate = keys.createIdentity(ndn::Name("/controller/spec205-capacity"),
      ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::ValidatorConfig validator(face);
  ServiceController controller(face, certificate, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  const auto query = [&] {
    return ServiceControllerTestAccess::statusQuery(certificate.getIdentity(), ndn::Name("/HELLO"));
  };
  const auto first = query();
  ServiceControllerTestAccess::policyStatusInterest(controller, first);
  const auto original = ServiceControllerTestAccess::originalReply(controller, first.getName());
  BOOST_REQUIRE(original);
  BOOST_REQUIRE(ndn::security::verifySignature(*original, signer));
  const ndn::Block originalWire = original->wireEncode();
  for (size_t i = 1; i < 4096; ++i) {
    ServiceControllerTestAccess::policyStatusInterest(controller, query());
  }
  BOOST_REQUIRE_EQUAL(ServiceControllerTestAccess::originalReplyCount(controller), 4096);
  const auto overflow = query();
  ServiceControllerTestAccess::policyStatusInterest(controller, overflow);
  BOOST_CHECK(!ServiceControllerTestAccess::originalReply(controller, overflow.getName()));
  ServiceControllerTestAccess::policyStatusInterest(controller, first);
  const auto replay = ServiceControllerTestAccess::originalReply(controller, first.getName());
  BOOST_REQUIRE(replay);
  BOOST_CHECK(replay->wireEncode() == originalWire);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::originalReplyCount(controller), 4096);

  state.system->advance(ndn::time::milliseconds(10000));
  const auto renewed = query();
  ServiceControllerTestAccess::policyStatusInterest(controller, renewed);
  BOOST_REQUIRE_EQUAL(ServiceControllerTestAccess::originalReplyCount(controller), 1);
  const auto current = ServiceControllerTestAccess::originalReply(controller, renewed.getName());
  BOOST_REQUIRE(current);
  BOOST_CHECK(ndn::security::verifySignature(*current, signer));
  // Expired query identities cannot regenerate a packet under the old name.
  ServiceControllerTestAccess::policyStatusInterest(controller, first);
  BOOST_CHECK(!ServiceControllerTestAccess::originalReply(controller, first.getName()));
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::originalReplyCount(controller), 1);
  BOOST_CHECK(original->wireEncode() == originalWire);
}

BOOST_AUTO_TEST_CASE(PolicyStatusRetryChangesNetworkNonceAndPreservesOriginalReply)
{
  struct EnvironmentGuard {
    std::map<std::string, std::optional<std::string>> previous;
    std::filesystem::path state = std::filesystem::temp_directory_path() /
      ("spec205-status-retry-" + std::to_string(::getpid()) + ".bin");

    void set(const std::string& name, const std::string& value)
    {
      const auto old = std::getenv(name.c_str());
      previous.emplace(name, old ? std::optional<std::string>(old) : std::nullopt);
      ::setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard()
    {
      for (const auto& entry : previous) {
        if (entry.second) ::setenv(entry.first.c_str(), entry.second->c_str(), 1);
        else ::unsetenv(entry.first.c_str());
      }
      std::error_code ignored;
      std::filesystem::remove(state, ignored);
      std::filesystem::remove(state.string() + ".lock", ignored);
    }
  } environment;
  environment.set("NDNSF_CONTROLLER_GENERATION_STATE", environment.state.string());
  environment.set("NDNSF_PERMISSION_FETCH_LIFETIME_MS", "500");
  environment.set("NDNSF_PERMISSION_FETCH_RETRY_BACKOFF_MS", "0");
  environment.set("NDNSF_PERMISSION_FETCH_MAX_ATTEMPTS", "2");
  ndn::KeyChain keys;
  const auto controllerCert = keys.createIdentity(ndn::Name("/controller/spec205-retry"),
      ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options options;
  options.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(keys, options);
  ndn::ValidatorConfig validator(controllerFace);
  ServiceController controller(controllerFace, controllerCert, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  controllerFace.processEvents(ndn::time::milliseconds(-1));
  const auto preflight = ServiceControllerTestAccess::statusQuery(
      controllerCert.getIdentity(), ndn::Name("/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(controller, preflight);
  controllerFace.processEvents(ndn::time::milliseconds(-1));
  const auto preflightReply = std::find_if(controllerFace.sentData.begin(),
      controllerFace.sentData.end(), [&] (const ndn::Data& data) {
        return data.getName() == preflight.getName();
      });
  BOOST_REQUIRE(preflightReply != controllerFace.sentData.end());
  BOOST_REQUIRE(ndn::security::verifySignature(*preflightReply, signer));
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      controller, ndn::Name("/example/hello/user"));
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      controller, ndn::Name("/example/hello/provider"));
  const auto prefix = controllerCert.getIdentity();
  const ndn::Name service("/HELLO");
  const auto version = controller.getControllerVersion();
  BOOST_REQUIRE(version.isValid());

  for (bool renewAfterExpiry : {false, true}) {
    environment.set("NDNSF_PERMISSION_FETCH_LIFETIME_MS", renewAfterExpiry ? "11000" : "500");
    for (bool providerRole : {false, true}) {
    ndn::DummyClientFace actorFace(keys, options);
    std::vector<ndn::Interest> queries;
    std::vector<ndn::Block> replies;
    ndn::signal::ScopedConnection interestRelay = actorFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (!parseControlRpcName(interest, prefix, "POLICY-STATUS")) return;
        queries.push_back(interest);
        ServiceControllerTestAccess::policyStatusInterest(controller, interest);
      });
    ndn::signal::ScopedConnection dataRelay = controllerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        if (queries.empty() || data.getName() != queries.back().getName()) return;
        BOOST_CHECK(ndn::security::verifySignature(data, signer));
        replies.push_back(data.wireEncode());
        if (replies.size() > 1) actorFace.receive(data);
      });
    ndn_service_framework::test::IsolatedTestTrust fixedTrust1{
      controllerCert, signer, userCert, providerCert};
    std::unique_ptr<ServiceUser> user;
    std::unique_ptr<ServiceProvider> provider;
    if (providerRole) {
      provider = std::make_unique<ServiceProvider>(ServiceProvider::LocalMockTag{}, actorFace,
          ndn::Name("/spec205/status-retry"), providerCert, signer, fixedTrust1.schemaPath());
      provider->fetchPolicyStatusFromController(prefix, service);
    }
    else {
      user = std::make_unique<ServiceUser>(ServiceUser::LocalMockTag{}, actorFace,
          ndn::Name("/spec205/status-retry"), userCert, signer, fixedTrust1.schemaPath());
      user->fetchPolicyStatusFromController(prefix, service);
    }
    const auto deadline = ndn::time::steady_clock::now() +
      ndn::time::seconds(renewAfterExpiry ? 14 : 3);
    do {
      actorFace.processEvents(ndn::time::milliseconds(-1));
      controllerFace.processEvents(ndn::time::milliseconds(-1));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (replies.size() < 2 && ndn::time::steady_clock::now() < deadline);
    // Drain validation after receipt before inspecting the installed version.
    actorFace.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(queries.size(), 2U);
    BOOST_REQUIRE_EQUAL(replies.size(), 2U);
    if (renewAfterExpiry) {
      // Real prepared-cache expiry precedes the first transport timeout.
      // The callback must retain the bootstrap Controller and renew the name.
      BOOST_CHECK(queries[0].getName() != queries[1].getName());
    }
    else {
      BOOST_CHECK(queries[0].getName() == queries[1].getName());
      BOOST_CHECK(replies[0] == replies[1]);
    }
    BOOST_CHECK(queries[0].getApplicationParameters() == queries[1].getApplicationParameters());
    BOOST_REQUIRE(queries[0].isParametersDigestValid());
    BOOST_REQUIRE(queries[1].isParametersDigestValid());
    BOOST_CHECK_NE(queries[0].getNonce(), queries[1].getNonce());
    const auto installed = providerRole ? provider->getControllerVersion(service) :
                                         user->getControllerVersion(service);
    BOOST_REQUIRE(installed);
    BOOST_CHECK(*installed == version);
    }
  }
}

BOOST_AUTO_TEST_CASE(ActualBootstrapRequesterPinsControllerAndOriginalSubjectKey)
{
  struct Files {
    std::optional<std::string> previousState, previousCertificate;
    std::filesystem::path state = std::filesystem::temp_directory_path() /
        ("spec201-bootstrap-binding-" + std::to_string(::getpid()) + ".bin");
    std::filesystem::path tokens = state.string() + ".tokens";
    Files() {
      if (const auto* value = ::getenv("NDNSF_CONTROLLER_GENERATION_STATE")) previousState = value;
      if (const auto* value = ::getenv("NDNSF_CONTROLLER_CERT_FILE")) previousCertificate = value;
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", state.c_str(), 1);
      ::unsetenv("NDNSF_CONTROLLER_CERT_FILE");
    }
    ~Files() {
      if (previousState) ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", previousState->c_str(), 1);
      else ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
      if (previousCertificate) ::setenv("NDNSF_CONTROLLER_CERT_FILE", previousCertificate->c_str(), 1);
      else ::unsetenv("NDNSF_CONTROLLER_CERT_FILE");
      std::error_code ignored;
      std::filesystem::remove(state, ignored);
      std::filesystem::remove(state.string() + ".lock", ignored);
      std::filesystem::remove(tokens, ignored);
    }
  } files;
  ndn::KeyChain keys;
  const auto issuer = keys.createIdentity(ndn::Name("/spec201/enrollment/controller"), ndn::RsaKeyParams(2048));
  const auto subject = keys.createIdentity(ndn::Name("/spec201/enrollment/user"), ndn::RsaKeyParams(2048));
  const auto foreign = keys.createIdentity(ndn::Name("/spec201/enrollment/foreign"), ndn::RsaKeyParams(2048));
  const auto controllerCert = issuer.getDefaultKey().getDefaultCertificate();
  const auto original = subject.getDefaultKey().getDefaultCertificate();
  {
    std::ofstream tokens(files.tokens);
    tokens << subject.getName().toUri(ndn::name::UriFormat::CANONICAL) << " spec201-test-token user\n";
  }
  boost::asio::io_context io;
  ndn::DummyClientFace controllerFace(io, keys), requesterFace(io, keys);
  ndn::ValidatorConfig validator(controllerFace);
  ServiceController controller(controllerFace, controllerCert, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
  controller.setControllerPrefix(issuer.getName());
  controller.setBootstrapTokenFile(files.tokens.string());
  enum class Fault { ForeignReply, ForeignIssuer, WrongSubject, CertificateSignature, ReplySignature, None };
  Fault fault = Fault::None;
  size_t queries = 0, replies = 0;
  ndn::Block acceptedWire;
  auto interests = requesterFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    BOOST_REQUIRE(parseControlRpcName(interest, issuer.getName(), "CERTBOOTSTRAP"));
    BOOST_REQUIRE(interest.isParametersDigestValid());
    ++queries;
    ServiceControllerTestAccess::certificateBootstrapInterest(controller, interest);
  });
  auto data = controllerFace.onSendData.connect([&](const ndn::Data& originalReply) {
    CertificateBootstrapResponse response;
    BOOST_REQUIRE(response.wireDecode(originalReply.getContent()));
    BOOST_REQUIRE_MESSAGE(response.status, response.message);
    ++replies;
    ndn::Data reply = originalReply;
    if (fault == Fault::ForeignIssuer) {
      response.issuedCertificate = keys.makeCertificate(subject.getDefaultKey(),
          ndn::security::signingByIdentity(foreign.getName()));
    }
    else if (fault == Fault::WrongSubject) {
      response.issuedCertificate = keys.makeCertificate(foreign.getDefaultKey(),
          ndn::security::signingByIdentity(issuer.getName()));
    }
    else if (fault == Fault::CertificateSignature) {
      auto signature = response.issuedCertificate.getSignatureValue().value_bytes();
      ndn::Buffer damaged(signature.begin(), signature.end());
      BOOST_REQUIRE(!damaged.empty());
      damaged.front() ^= 1;
      response.issuedCertificate.setSignatureValue(ndn::span<const uint8_t>(damaged));
    }
    if (fault != Fault::None) {
      reply.setContent(response.wireEncode());
      keys.sign(reply, ndn::security::signingByIdentity(
          fault == Fault::ForeignReply ? foreign.getName() : issuer.getName()));
      if (fault == Fault::ReplySignature) {
        response.message = "tampered reply";
        reply.setContent(response.wireEncode());
      }
    }
    else acceptedWire = response.issuedCertificate.wireEncode();
    requesterFace.receive(reply);
  });
  for (const auto negative : {Fault::ForeignReply, Fault::ForeignIssuer, Fault::WrongSubject,
                             Fault::CertificateSignature, Fault::ReplySignature}) {
    fault = negative;
    BOOST_CHECK_THROW(requestControllerSignedCertificate(requesterFace, keys, issuer.getName(),
        subject.getName(), "spec201-test-token", ndn::time::milliseconds(1000)), std::runtime_error);
    BOOST_CHECK(subject.getDefaultKey().getDefaultCertificate().wireEncode() == original.wireEncode());
  }
  fault = Fault::None;
  const auto issued = requestControllerSignedCertificate(requesterFace, keys, issuer.getName(),
      subject.getName(), "spec201-test-token", ndn::time::milliseconds(1000));
  BOOST_CHECK(issued.getIdentity() == subject.getName());
  BOOST_CHECK(issued.getKeyName() == original.getKeyName());
  BOOST_CHECK(ndn::security::verifySignature(issued, controllerCert));
  BOOST_CHECK(issued.wireEncode() == acceptedWire);
  BOOST_CHECK_EQUAL(queries, 6);
  BOOST_CHECK_EQUAL(replies, 6);
  const auto reused = ensureControllerSignedCertificate(requesterFace, keys, issuer.getName(),
      subject.getName(), "spec201-test-token", ndn::time::milliseconds(1000));
  BOOST_CHECK(reused.wireEncode() == acceptedWire);
  BOOST_CHECK_EQUAL(queries, 6);
}

BOOST_AUTO_TEST_CASE(RevokedUserAndProviderHavePairedUnaffectedControls)
{
  RevocationState userState{ndn::Name(SERVICE)};
  BOOST_REQUIRE(userState.acceptStatus(
      statusFor(ControllerVersion{5000, 1}), 1000));

  const AuthorizationSubject userAlice{
      ndn::Name("/user/alice"), "sha256:alice-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject userBob{
      ndn::Name("/user/bob"), "sha256:bob-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  TerminalLedger ledger;
  BOOST_CHECK(ledger.execute("alice-before", userState.authorize(
      userAlice, ProtectedTransition::PROVIDER_EXECUTION, 1000)));

  const auto revokedUser = statusFor(
      ControllerVersion{5000, 2}, {makeIdentityRevocation("/user/alice")});
  BOOST_REQUIRE(userState.acceptStatus(revokedUser, 1000));
  const auto denied = userState.authorize(
      userAlice, ProtectedTransition::DISCOVERY, 1000);
  ledger.rejectExactlyOnce("alice-after", denied);
  ledger.rejectExactlyOnce("alice-after", denied);
  BOOST_CHECK(!denied.allowed);
  BOOST_CHECK_EQUAL(ledger.terminal.count("alice-after"), 1);
  BOOST_CHECK_EQUAL(ledger.executions["alice-before"], 1);
  BOOST_CHECK(userState.authorize(
      userBob, ProtectedTransition::DISCOVERY, 1000).allowed);

  RevocationState providerState{ndn::Name(SERVICE)};
  BOOST_REQUIRE(providerState.acceptStatus(
      statusFor(ControllerVersion{6000, 1}), 1000));
  const AuthorizationSubject providerP1{
      ndn::Name("/provider/p1"), "sha256:p1-cert", ndn::Name(SERVICE),
      ndn::Name("/SERVICE").append(ndn::Name(SERVICE))};
  const AuthorizationSubject providerP2{
      ndn::Name("/provider/p2"), "sha256:p2-cert", ndn::Name(SERVICE),
      ndn::Name("/SERVICE").append(ndn::Name(SERVICE))};
  BOOST_REQUIRE(providerState.acceptStatus(statusFor(
      ControllerVersion{6000, 2}, {makeServiceRevocation("/provider/p1")}), 1000));
  BOOST_CHECK(!providerState.authorize(
      providerP1, ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
  BOOST_CHECK(providerState.authorize(
      providerP2, ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(IdentityRevocationDeniesEveryCutPointForBothRoles)
{
  // Exercise the real component-level state boundary for an identity-wide
  // withdrawal.  The unit suite checks the predicate itself; this case keeps
  // the User and Provider roles paired and verifies every enforcing owner.
  RevocationState state{ndn::Name(SERVICE)};
  BOOST_REQUIRE(state.acceptStatus(statusFor(ControllerVersion{6100, 1}), 1000));

  const AuthorizationSubject revokedUser{
      ndn::Name("/user/alice"), "sha256:alice-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject unaffectedUser{
      ndn::Name("/user/bob"), "sha256:bob-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject revokedProvider{
      ndn::Name("/provider/p1"), "sha256:p1-cert", ndn::Name(SERVICE),
      ndn::Name("/SERVICE").append(ndn::Name(SERVICE))};
  const AuthorizationSubject unaffectedProvider{
      ndn::Name("/provider/p2"), "sha256:p2-cert", ndn::Name(SERVICE),
      ndn::Name("/SERVICE").append(ndn::Name(SERVICE))};

  const auto revoked = statusFor(
      ControllerVersion{6100, 2},
      {makeIdentityRevocation("/user/alice"),
       makeIdentityRevocation("/provider/p1")});
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));

  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    const auto userDecision = state.authorize(revokedUser, transition, 1000);
    const auto providerDecision = state.authorize(revokedProvider, transition, 1000);
    BOOST_CHECK(!userDecision.allowed);
    BOOST_CHECK(!providerDecision.allowed);
    BOOST_CHECK(userDecision.reason.find("revoked_before_") == 0);
    BOOST_CHECK(providerDecision.reason.find("revoked_before_") == 0);
    BOOST_CHECK(state.authorize(unaffectedUser, transition, 1000).allowed);
    BOOST_CHECK(state.authorize(unaffectedProvider, transition, 1000).allowed);
  }
}

BOOST_AUTO_TEST_CASE(CertificateRevocationKeepsReplacementAndUnrelatedIdentityLive)
{
  RevocationState state{ndn::Name(SERVICE)};
  const auto oldDigest = std::string("sha256:old-cert");
  BOOST_REQUIRE(state.acceptStatus(statusFor(
      ControllerVersion{6200, 1},
      {makeCertificateRevocation("/user/alice", oldDigest.c_str())}), 1000));

  const AuthorizationSubject oldCertificate{
      ndn::Name("/user/alice"), oldDigest, ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject replacement{
      ndn::Name("/user/alice"), "sha256:new-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject unrelated{
      ndn::Name("/user/bob"), "sha256:old-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};

  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    BOOST_CHECK(!state.authorize(oldCertificate, transition, 1000).allowed);
    BOOST_CHECK(state.authorize(replacement, transition, 1000).allowed);
    BOOST_CHECK(state.authorize(unrelated, transition, 1000).allowed);
  }
}

BOOST_AUTO_TEST_CASE(ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial)
{
  RevocationState serviceState{ndn::Name(SERVICE)};
  BOOST_REQUIRE(serviceState.acceptStatus(statusFor(ControllerVersion{6500, 1}), 1000));
  const AuthorizationSubject provider{
      ndn::Name("/provider/p1"), "sha256:p1-cert", ndn::Name(SERVICE),
      ndn::Name("/SERVICE").append(ndn::Name(SERVICE))};
  const auto revoked = statusFor(
      ControllerVersion{6500, 2}, {makeServiceRevocation("/provider/p1")});
  BOOST_REQUIRE(serviceState.acceptStatus(revoked, 1000));

  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    const auto decision = serviceState.authorize(provider, transition, 1000);
    BOOST_CHECK(!decision.allowed);
    BOOST_CHECK(decision.reason.find("revoked_before_") == 0);
  }

  RevocationState otherServiceState{ndn::Name("/OtherService")};
  auto otherStatus = statusFor(ControllerVersion{6500, 2});
  otherStatus.setServiceName(ndn::Name("/OtherService"));
  BOOST_REQUIRE(otherServiceState.acceptStatus(otherStatus, 1000));
  const AuthorizationSubject sameProviderOtherService{
      ndn::Name("/provider/p1"), "sha256:p1-cert", ndn::Name("/OtherService"),
      ndn::Name("/SERVICE/OtherService")};
  BOOST_CHECK(otherServiceState.authorize(
      sameProviderOtherService, ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(CertificateOnlyRevocationAllowsReplacementAndAllCutPointsFailClosed)
{
  RevocationState state{ndn::Name(SERVICE)};
  BOOST_REQUIRE(state.acceptStatus(statusFor(
      ControllerVersion{7000, 1},
      {makeCertificateRevocation("/user/alice", "sha256:old-cert")}), 1000));
  const AuthorizationSubject oldCertificate{
      ndn::Name("/user/alice"), "sha256:old-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject replacement{
      ndn::Name("/user/alice"), "sha256:new-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT})
    BOOST_CHECK(!state.authorize(oldCertificate, transition, 1000).allowed);
  BOOST_CHECK(state.authorize(replacement,
                              ProtectedTransition::RESPONSE_DELIVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(RestartClockRollbackAndControllerUnavailableAreBounded)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-flow-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerVersion first;
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("controller"));
    first = store.startGeneration(8000);
  }
  ControllerVersion restarted;
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("controller-restarted"));
    restarted = store.startGeneration(7000);
  }
  BOOST_CHECK(restarted.controllerGenerationTimestamp > first.controllerGenerationTimestamp);

  RevocationState state{ndn::Name(SERVICE)};
  BOOST_REQUIRE(state.acceptStatus(statusFor(restarted), 1000));
  const AuthorizationSubject unaffected{
      ndn::Name("/user/bob"), "sha256:bob-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  // An unavailable Controller does not interrupt still-valid equal-version
  // traffic; once the signed status expires, the same transition fails closed.
  BOOST_CHECK(state.authorize(unaffected,
                              ProtectedTransition::SELECTION, 1000).allowed);
  BOOST_CHECK(!state.authorize(unaffected,
                               ProtectedTransition::SELECTION, 5000).allowed);

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ControllerProviderAndCacheCopiesHaveIdenticalAuthority)
{
  const auto version = ControllerVersion{9000, 4};
  const auto canonical = statusFor(version, {makeIdentityRevocation("/user/alice")});
  RevocationState fromController{ndn::Name(SERVICE)};
  RevocationState fromProvider{ndn::Name(SERVICE)};
  RevocationState fromCache{ndn::Name(SERVICE)};
  BOOST_REQUIRE(fromController.acceptStatus(canonical, 1000));
  BOOST_REQUIRE(fromProvider.acceptStatus(canonical, 1000));
  BOOST_REQUIRE(fromCache.acceptStatus(canonical, 1000));

  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  const AuthorizationSubject bob{
      ndn::Name("/user/bob"), "sha256:bob-cert", ndn::Name(SERVICE),
      ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))};
  for (const RevocationState* state : {&fromController, &fromProvider, &fromCache}) {
    BOOST_CHECK(!state->authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);
    BOOST_CHECK(state->authorize(bob, ProtectedTransition::DISCOVERY, 1000).allowed);
  }

  auto wrongSigner = canonical;
  BOOST_CHECK(!fromController.acceptStatus(wrongSigner, 1000, false));
  auto wrongScope = canonical;
  wrongScope.setServiceName(ndn::Name("/OtherService"));
  BOOST_CHECK(!fromProvider.acceptStatus(wrongScope, 1000));
  auto expired = canonical;
  expired.setValidity(100, 200);
  BOOST_CHECK(!fromCache.acceptStatus(expired, 1000));
}

BOOST_AUTO_TEST_CASE(ServiceControllerMutatesTypedRevocationAndAdvancesEpoch)
{
  // This is a real ServiceController construction.  It deliberately stops
  // before start() so the test exercises authority state and policy mutation
  // without depending on an NFD registration loop.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-authority-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller", "tpm-memory:spec179-controller");
  const auto controllerIdentity = ndn::Name("/controller/spec179");
  const auto controller = keyChain.createIdentity(controllerIdentity, ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keyChain);
  ndn::ValidatorConfig validator(face);

  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  // The grant record is signed with the Controller's AA certificate through the Controller's own signer; without it the
  // record cannot be written and, now, the reply is withheld.
  ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  const auto initial = serviceController.getControllerVersion();
  BOOST_REQUIRE(initial.isValid());

  const auto service = ndn::Name("/HELLO");
  const auto otherService = ndn::Name("/OtherService");
  auto initialStatus = serviceController.getPolicyStatus(service);
  BOOST_CHECK(initialStatus.getRevocations().empty());
  BOOST_CHECK(initialStatus.getControllerVersion() == initial);
  const auto initialValidFrom = initialStatus.getValidFromMs();
  const auto initialValidUntil = initialStatus.getValidUntilMs();
  BOOST_CHECK(initialValidFrom < initialValidUntil);

  RevocationTarget user;
  user.kind = RevocationKind::IDENTITY;
  user.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_REQUIRE(serviceController.revoke(user));
  const auto afterUser = serviceController.getControllerVersion();
  BOOST_CHECK(afterUser.compare(initial) > 0);
  BOOST_CHECK(serviceController.isRevoked(user.targetIdentity, service));
  BOOST_CHECK(serviceController.isRevoked(user.targetIdentity, otherService));
  BOOST_CHECK(!serviceController.isRevoked(ndn::Name("/example/hello/user-b"), service));
  const auto afterUserStatus = serviceController.getPolicyStatus(service);
  BOOST_CHECK(afterUserStatus.getControllerVersion() == afterUser);
  BOOST_CHECK(afterUserStatus.getValidFromMs() >= initialValidFrom);
  BOOST_CHECK(afterUserStatus.getValidUntilMs() > afterUserStatus.getValidFromMs());
  BOOST_CHECK(afterUserStatus.getValidUntilMs() >= initialValidUntil);
  const auto afterFirstUserRevocation = serviceController.getControllerVersion();
  BOOST_CHECK(!serviceController.revoke(user));
  BOOST_CHECK(serviceController.getControllerVersion() == afterFirstUserRevocation);
  const auto afterDuplicateStatus = serviceController.getPolicyStatus(service);
  BOOST_CHECK(afterDuplicateStatus.getControllerVersion() == afterFirstUserRevocation);
  BOOST_CHECK_EQUAL(afterDuplicateStatus.getValidFromMs(),
                    afterUserStatus.getValidFromMs());
  BOOST_CHECK_EQUAL(afterDuplicateStatus.getValidUntilMs(),
                    afterUserStatus.getValidUntilMs());

  RevocationTarget providerService;
  providerService.kind = RevocationKind::SERVICE_AUTHORIZATION;
  providerService.targetIdentity = ndn::Name("/example/hello/provider");
  providerService.serviceName = service;
  providerService.authorizationAttribute = ndn::Name("/SERVICE").append(service);
  BOOST_REQUIRE(serviceController.revoke(providerService));
  const auto afterProvider = serviceController.getControllerVersion();
  BOOST_CHECK_EQUAL(afterProvider.controllerGenerationTimestamp,
                    afterUser.controllerGenerationTimestamp);
  BOOST_CHECK_EQUAL(afterProvider.controllerEpoch, afterUser.controllerEpoch + 1);
  const auto serviceStatus = serviceController.getPolicyStatus(service);
  BOOST_CHECK_EQUAL(serviceStatus.getRevocations().size(), 2);
  const auto otherStatus = serviceController.getPolicyStatus(otherService);
  BOOST_CHECK_EQUAL(otherStatus.getRevocations().size(), 1);
  BOOST_CHECK(serviceController.isRevoked(providerService.targetIdentity, service));
  BOOST_CHECK(!serviceController.isRevoked(providerService.targetIdentity, otherService));

  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  // Keep certificate-only revocation independent from the identity-wide
  // revocation above; a replacement certificate cannot restore an identity
  // that is still globally revoked.
  certificate.targetIdentity = ndn::Name("/example/hello/provider-c");
  certificate.certificateDigest = "sha256:old-certificate";
  BOOST_REQUIRE(serviceController.revoke(certificate));
  const auto afterCertificate = serviceController.getControllerVersion();
  BOOST_CHECK_EQUAL(afterCertificate.controllerGenerationTimestamp,
                    afterProvider.controllerGenerationTimestamp);
  BOOST_CHECK_EQUAL(afterCertificate.controllerEpoch, afterProvider.controllerEpoch + 1);
  BOOST_CHECK(serviceController.isRevoked(certificate.targetIdentity, service,
                                          "sha256:old-certificate"));
  BOOST_CHECK(!serviceController.isRevoked(certificate.targetIdentity, service,
                                           "sha256:new-certificate"));

  RevocationTarget invalid;
  invalid.kind = RevocationKind::SERVICE_AUTHORIZATION;
  invalid.targetIdentity = ndn::Name("/example/hello/provider");
  const auto beforeInvalid = serviceController.getControllerVersion();
  BOOST_CHECK(!serviceController.revoke(invalid));
  BOOST_CHECK(serviceController.getControllerVersion() == beforeInvalid);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(HistoricalGrantCapturesEligibilityBeforeFirstPublicParametersQuery)
{
  const auto root = test::trust_detail::makeTestTrustDirectory();
  const auto state = root / "history-first-grant.bin";
  const auto previousState = std::getenv("NDNSF_CONTROLLER_GENERATION_STATE");
  const auto oldState = previousState ? std::optional<std::string>(previousState) : std::nullopt;
  const auto restore = ndn::make_scope_exit([&] {
    if (oldState)
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", oldState->c_str(), 1);
    else
      ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_CONTROLLER_GENERATION_STATE", state.c_str(), 1), 0);
  ndn::KeyChain keys;
  const auto identity = keys.createIdentity(ndn::Name("/controller/history-first-grant"),
                                            ndn::RsaKeyParams(2048));
  const auto certificate = identity.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keys);
  ndn::ValidatorConfig validator(face);
  ServiceController controller(face, certificate, validator,
    ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto birth = ServiceControllerTestAccess::abeMaterialVersion(controller);
  BOOST_REQUIRE(birth.isValid());
  BOOST_REQUIRE(birth == controller.getControllerVersion());
  const auto original = ndn::Name("/example/hello/user");
  const auto late = ndn::Name("/example/hello/user/history-late");
  const auto scope = std::string("/PERMISSION/HELLO");
  BOOST_REQUIRE(controller.grant(late, ndn::Name("/HELLO"), ndn::Name(scope)));
  BOOST_CHECK(controller.getControllerVersion().compare(birth) > 0);
  BOOST_CHECK(ServiceControllerTestAccess::abeMaterialVersion(controller) == birth);
  BOOST_CHECK(ServiceControllerTestAccess::hasHistoricalGrant(controller, original, birth, scope));
  BOOST_CHECK(!ServiceControllerTestAccess::hasHistoricalGrant(controller, late, birth, scope));
  BOOST_CHECK(ServiceControllerTestAccess::abePolicy(controller, late).find(scope) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-grant-only-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-grant", "tpm-memory:spec179-grant");
  const auto controllerIdentity = ndn::Name("/controller/spec179-grant");
  const auto controller = keyChain.createIdentity(controllerIdentity, ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  const auto user = ndn::Name("/example/hello/user-b");
  const auto unaffected = ndn::Name("/example/hello/provider/A");

  // Seed the target with one existing attribute first.  The subsequent
  // grant must replace the target's complete monolithic policy, preserving
  // this attribute while adding the new one; it must not patch or fan out a
  // DKEY for other identities.
  BOOST_REQUIRE(serviceController.grant(
      user, ndn::Name("/EXISTING"), ndn::Name("/PERMISSION/EXISTING")));
  const auto initialVersion = serviceController.getControllerVersion();
  const auto initialName = ServiceControllerTestAccess::abePublicParametersName(
      serviceController);
  const auto initialDigest = ServiceControllerTestAccess::abePublicParametersDigest(
      serviceController);
  const auto initialPublicParameters = ServiceControllerTestAccess::abePublicParametersWire(
      serviceController);
  const auto initialMasterKey = ServiceControllerTestAccess::abeMasterKey(serviceController);
  const auto unaffectedPolicy = ServiceControllerTestAccess::abePolicy(
      serviceController, unaffected);
  BOOST_REQUIRE(initialVersion.isValid());
  BOOST_REQUIRE(!initialName.empty());
  BOOST_REQUIRE(!initialDigest.empty());
  BOOST_REQUIRE(!unaffectedPolicy.empty());

  // Capture the target's DKEY before the grant.  It must remain usable for
  // its original attribute, but must not gain the newly granted attribute.
  const auto oldTargetKey = ServiceControllerTestAccess::abePrivateKey(
      serviceController, user);

  BOOST_REQUIRE(serviceController.grant(
      user, ndn::Name("/HELLO"), ndn::Name("/PERMISSION/HELLO")));
  const auto afterVersion = serviceController.getControllerVersion();
  BOOST_CHECK(afterVersion.compare(initialVersion) > 0);
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              initialName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    initialDigest);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersWire(serviceController),
                    initialPublicParameters);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abeMasterKey(serviceController),
                    initialMasterKey);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePolicy(serviceController, unaffected),
                    unaffectedPolicy);

  // Grant-only updates replace the target's complete KP-ABE policy but retain
  // the same ABE pair.  The old same-generation DKEY remains valid for its
  // original attribute and must not satisfy ciphertext requiring the grant.
  const auto newAttributeCipher = ServiceControllerTestAccess::abeEncrypt(
      serviceController, ndn::Name("/PERMISSION/HELLO"), "grant-only-secret");
  BOOST_CHECK_THROW(ServiceControllerTestAccess::abeDecrypt(
                        serviceController, oldTargetKey, newAttributeCipher),
                    std::exception);

  const auto grantedPolicy = ServiceControllerTestAccess::abePolicy(
      serviceController, user);
  BOOST_CHECK(grantedPolicy.find("/PERMISSION/EXISTING") != std::string::npos);
  BOOST_CHECK(grantedPolicy.find("/PERMISSION/HELLO") != std::string::npos);
  const auto replacementTargetKey = ServiceControllerTestAccess::abePrivateKey(
      serviceController, user);
  const auto decryptedGrant = ServiceControllerTestAccess::abeDecrypt(
      serviceController, replacementTargetKey, newAttributeCipher);
  BOOST_CHECK_EQUAL_COLLECTIONS(decryptedGrant.begin(), decryptedGrant.end(),
                                reinterpret_cast<const uint8_t*>("grant-only-secret"),
                                reinterpret_cast<const uint8_t*>("grant-only-secret") +
                                  std::string("grant-only-secret").size());
  const auto permissions = ServiceControllerTestAccess::userPermissions(
      serviceController, user);
  BOOST_CHECK(std::any_of(
      permissions.getEntries().begin(), permissions.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));

  const auto providerGrant = ndn::Name("/example/hello/provider/grant-only");
  BOOST_REQUIRE(serviceController.grant(
      providerGrant, ndn::Name("/HELLO"), ndn::Name("/SERVICE/HELLO")));
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              initialName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    initialDigest);
  const auto providerGrantPolicy = ServiceControllerTestAccess::abePolicy(
      serviceController, providerGrant);
  BOOST_CHECK(providerGrantPolicy.find("/SERVICE/HELLO") != std::string::npos);

  const auto preWithdrawalName = ServiceControllerTestAccess::abePublicParametersName(
      serviceController);
  const auto preWithdrawalDigest = ServiceControllerTestAccess::abePublicParametersDigest(
      serviceController);
  const auto preWithdrawalPublicParameters = ServiceControllerTestAccess::abePublicParametersWire(
      serviceController);
  const auto preWithdrawalMasterKey = ServiceControllerTestAccess::abeMasterKey(serviceController);
  const auto providerOldKey = ServiceControllerTestAccess::abePrivateKey(
      serviceController, unaffected);
  // RV-U20 requires retaining every pre-withdrawal artifact.  Capture the old
  // parameter object and one old-generation ciphertext under an attribute that
  // survives the withdrawal, so the full same-/mixed-generation matrix below
  // can be exercised after the rotation.
  const auto retainedAttribute = ndn::Name("/SERVICE/NDNSF/DistributedRepo/Store");
  const auto preWithdrawalParams = ServiceControllerTestAccess::abePublicParameters(
      serviceController);
  const auto preWithdrawalCipher = ServiceControllerTestAccess::abeEncrypt(
      serviceController, retainedAttribute, "pre-withdrawal-secret");
  RevocationTarget withdrawal;
  withdrawal.kind = RevocationKind::SERVICE_AUTHORIZATION;
  withdrawal.targetIdentity = unaffected;
  withdrawal.serviceName = ndn::Name("/HELLO");
  withdrawal.authorizationAttribute = ndn::Name("/SERVICE/HELLO");
  BOOST_REQUIRE(serviceController.revoke(withdrawal));
  const auto postWithdrawalName = ServiceControllerTestAccess::abePublicParametersName(
      serviceController);
  const auto postWithdrawalDigest = ServiceControllerTestAccess::abePublicParametersDigest(
      serviceController);
  BOOST_CHECK(postWithdrawalName != preWithdrawalName);
  BOOST_CHECK(postWithdrawalDigest != preWithdrawalDigest);
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersWire(serviceController) !=
              preWithdrawalPublicParameters);
  BOOST_CHECK(ServiceControllerTestAccess::abeMasterKey(serviceController) !=
              preWithdrawalMasterKey);

  // A withdrawal creates a new global ABE pair.  A retained identity receives
  // a filtered current-generation DKEY; its pre-withdrawal DKEY cannot decrypt
  // even an attribute that remains authorized in the new generation.
  const auto retainedCipher = ServiceControllerTestAccess::abeEncrypt(
      serviceController, retainedAttribute, "post-withdrawal-secret");
  BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
      ServiceControllerTestAccess::abePublicParameters(serviceController),
      providerOldKey, retainedCipher, "post-withdrawal-secret"));
  const auto providerReplacementKey = ServiceControllerTestAccess::abePrivateKey(
      serviceController, unaffected);
  const auto retainedPlaintext = ServiceControllerTestAccess::abeDecrypt(
      serviceController, providerReplacementKey, retainedCipher);
  BOOST_CHECK_EQUAL_COLLECTIONS(retainedPlaintext.begin(), retainedPlaintext.end(),
                                reinterpret_cast<const uint8_t*>("post-withdrawal-secret"),
                                reinterpret_cast<const uint8_t*>("post-withdrawal-secret") +
                                  std::string("post-withdrawal-secret").size());

  // RV-U20 same-/mixed-generation decrypt matrix.  Old parameters plus the
  // retained old DKEY still recover pre-withdrawal ciphertext (the documented
  // historical limitation); every mixed combination of parameters, DKEY, and
  // ciphertext generation must fail closed.
  const auto historicalPlaintext = ServiceControllerTestAccess::abeDecrypt(
      preWithdrawalParams, providerOldKey, preWithdrawalCipher);
  BOOST_CHECK_EQUAL_COLLECTIONS(historicalPlaintext.begin(), historicalPlaintext.end(),
                                reinterpret_cast<const uint8_t*>("pre-withdrawal-secret"),
                                reinterpret_cast<const uint8_t*>("pre-withdrawal-secret") +
                                  std::string("pre-withdrawal-secret").size());
  // Each mixed combination uses a FRESH ciphertext.  The content-key cache
  // records the decrypted AES key of a ciphertext after its first successful
  // decrypt; reusing an already-successfully-decrypted ciphertext would hit
  // the cache and mask the generation mismatch.
  // New-generation DKEY with old parameters and an old-generation ciphertext.
  const auto mixedOldCipher1 = ServiceControllerTestAccess::abeEncrypt(
      preWithdrawalParams, retainedAttribute, "mixed-old-generation-1");
  BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
      preWithdrawalParams, providerReplacementKey, mixedOldCipher1,
      "mixed-old-generation-1"));
  // Retained old DKEY with old parameters and a new-generation ciphertext.
  const auto mixedNewCipher2 = ServiceControllerTestAccess::abeEncrypt(
      ServiceControllerTestAccess::abePublicParameters(serviceController),
      retainedAttribute, "mixed-new-generation-2");
  BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
      preWithdrawalParams, providerOldKey, mixedNewCipher2,
      "mixed-new-generation-2"));
  // New-generation DKEY with current parameters and an old-generation
  // ciphertext.
  const auto mixedOldCipher3 = ServiceControllerTestAccess::abeEncrypt(
      preWithdrawalParams, retainedAttribute, "mixed-old-generation-3");
  BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
      ServiceControllerTestAccess::abePublicParameters(serviceController),
      providerReplacementKey, mixedOldCipher3,
      "mixed-old-generation-3"));

  // Reauthorization uses the current post-withdrawal generation.  It does
  // not roll the AA back to the pre-withdrawal pair or fan a replacement DKEY
  // out to unrelated identities.
  BOOST_REQUIRE(serviceController.grant(
      unaffected, ndn::Name("/HELLO"), ndn::Name("/SERVICE/HELLO")));
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              postWithdrawalName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    postWithdrawalDigest);
  BOOST_CHECK(ServiceControllerTestAccess::abePolicy(serviceController, unaffected).find(
                  "/SERVICE/HELLO") != std::string::npos);

  // Dual-role boundary: one identity holding both the use and the provision
  // attribute for the same service.  Withdrawing one attribute must leave the
  // other intact, in both directions.
  const auto dualUse = ndn::Name("/example/hello/dual-role-use");
  BOOST_REQUIRE(serviceController.grant(
      dualUse, ndn::Name("/HELLO"), ndn::Name("/PERMISSION/HELLO")));
  BOOST_REQUIRE(serviceController.grant(
      dualUse, ndn::Name("/HELLO"), ndn::Name("/SERVICE/HELLO")));
  RevocationTarget dualUseWithdrawal;
  dualUseWithdrawal.kind = RevocationKind::SERVICE_AUTHORIZATION;
  dualUseWithdrawal.targetIdentity = dualUse;
  dualUseWithdrawal.serviceName = ndn::Name("/HELLO");
  dualUseWithdrawal.authorizationAttribute = ndn::Name("/PERMISSION/HELLO");
  BOOST_REQUIRE(serviceController.revoke(dualUseWithdrawal));
  BOOST_CHECK(serviceController.isRevoked(
      dualUse, ndn::Name("/HELLO"), {}, ndn::Name("/PERMISSION/HELLO")));
  BOOST_CHECK(!serviceController.isRevoked(
      dualUse, ndn::Name("/HELLO"), {}, ndn::Name("/SERVICE/HELLO")));
  const auto dualUsePolicy = ServiceControllerTestAccess::abePolicy(
      serviceController, dualUse);
  BOOST_CHECK(dualUsePolicy.find("/PERMISSION/HELLO") == std::string::npos);
  BOOST_CHECK(dualUsePolicy.find("/SERVICE/HELLO") != std::string::npos);

  const auto dualProvision = ndn::Name("/example/hello/dual-role-provision");
  BOOST_REQUIRE(serviceController.grant(
      dualProvision, ndn::Name("/HELLO"), ndn::Name("/PERMISSION/HELLO")));
  BOOST_REQUIRE(serviceController.grant(
      dualProvision, ndn::Name("/HELLO"), ndn::Name("/SERVICE/HELLO")));
  RevocationTarget dualProvisionWithdrawal;
  dualProvisionWithdrawal.kind = RevocationKind::SERVICE_AUTHORIZATION;
  dualProvisionWithdrawal.targetIdentity = dualProvision;
  dualProvisionWithdrawal.serviceName = ndn::Name("/HELLO");
  dualProvisionWithdrawal.authorizationAttribute = ndn::Name("/SERVICE/HELLO");
  BOOST_REQUIRE(serviceController.revoke(dualProvisionWithdrawal));
  BOOST_CHECK(serviceController.isRevoked(
      dualProvision, ndn::Name("/HELLO"), {}, ndn::Name("/SERVICE/HELLO")));
  BOOST_CHECK(!serviceController.isRevoked(
      dualProvision, ndn::Name("/HELLO"), {}, ndn::Name("/PERMISSION/HELLO")));
  const auto dualProvisionPolicy = ServiceControllerTestAccess::abePolicy(
      serviceController, dualProvision);
  BOOST_CHECK(dualProvisionPolicy.find("/SERVICE/HELLO") == std::string::npos);
  BOOST_CHECK(dualProvisionPolicy.find("/PERMISSION/HELLO") != std::string::npos);

  // Identity-wide withdrawal must also be reversible through an explicit
  // grant for an already configured service.  The old service and attribute
  // entries remain in the policy map, so grant() must not mistake this for an
  // idempotent duplicate and must reauthorize against the current generation.
  const auto identityReauthorized = ndn::Name("/example/hello/user");
  BOOST_REQUIRE(serviceController.revoke(
      makeIdentityRevocation(identityReauthorized.toUri().c_str())));
  const auto identityRevokedName = ServiceControllerTestAccess::abePublicParametersName(
      serviceController);
  const auto identityRevokedDigest = ServiceControllerTestAccess::abePublicParametersDigest(
      serviceController);
  BOOST_REQUIRE(serviceController.grant(
      identityReauthorized, ndn::Name("/HELLO"), ndn::Name("/PERMISSION/HELLO")));
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              identityRevokedName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    identityRevokedDigest);
  BOOST_CHECK(!serviceController.isRevoked(identityReauthorized, ndn::Name("/HELLO")));
  BOOST_CHECK(ServiceControllerTestAccess::abePolicy(serviceController,
                                                       identityReauthorized).find(
                  "/PERMISSION/HELLO") != std::string::npos);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerPublishesStableTimestampedRevocationSnapshot)
{
  // The status snapshot is the authority that receivers install.  Exercise
  // the real Controller boundary to ensure an accepted revocation produces a
  // valid current-time window, a new version, and one immutable payload for
  // repeated reads (rather than a timestamp-only or partially updated view).
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-timestamped-status-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-timestamped-status"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signingCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const ndn::Name service(SERVICE);
  const auto before = serviceController.getPolicyStatus(service);
  const auto beforeVersion = before.getControllerVersion();
  BOOST_REQUIRE(beforeVersion.isValid());
  BOOST_REQUIRE(before.validate(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())));

  const auto target = makeIdentityRevocation("/spec179/timestamped/user");
  BOOST_REQUIRE(serviceController.revoke(target));
  const auto after = serviceController.getPolicyStatus(service);
  const auto afterVersion = after.getControllerVersion();
  BOOST_REQUIRE(afterVersion.compare(beforeVersion) > 0);

  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  BOOST_CHECK(after.validate(now));
  BOOST_CHECK(after.getValidFromMs() <= now);
  BOOST_CHECK(after.getValidUntilMs() > now);
  BOOST_CHECK(after.getValidUntilMs() > after.getValidFromMs());
  BOOST_REQUIRE_EQUAL(after.getRevocations().size(), 1);
  BOOST_CHECK(after.getRevocations().front().targetIdentity == target.targetIdentity);

  // Repeated reads at one ControllerVersion must be byte-identical.  This is
  // important for caches and for exact-name refresh: a same-version payload
  // must never change underneath a receiver.
  const auto repeated = serviceController.getPolicyStatus(service);
  const auto firstWire = after.wireEncode();
  const auto repeatedWire = repeated.wireEncode();
  BOOST_CHECK_EQUAL_COLLECTIONS(firstWire.begin(), firstWire.end(),
                                repeatedWire.begin(), repeatedWire.end());

  // Exercise the actual version-pinned prepared RPC, with an immutable reply
  // at its complete final Interest Name, in addition to the local snapshot.
  const auto exactQuery = ServiceControllerTestAccess::statusQuery(
      controllerCert.getIdentity(), service, afterVersion);
  const auto exactName = exactQuery.getName();
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, exactQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto& published = face.sentData.front();
  BOOST_CHECK(published.getName() == exactName);
  BOOST_REQUIRE(published.getSignatureInfo());
  BOOST_CHECK(ndn::security::verifySignature(published, signingCert));
  PolicyStatusData announced;
  const auto announcedContent = published.getContent();
  auto [announcedParsed, announcedBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(announcedContent.value(),
                               announcedContent.value_size()));
  BOOST_REQUIRE(announcedParsed);
  BOOST_REQUIRE(announced.wireDecode(announcedBlock));
  BOOST_CHECK(announced.getControllerVersion() == afterVersion);
  BOOST_CHECK(announced.validate(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())));

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRejectsStaleExactStatusAfterRevocation)
{
  // A cache may retain the old exact status name, but the Controller must not
  // serve that snapshot after its authority advances.  Otherwise a restarted
  // or disconnected runtime could silently reinstall pre-revocation grants.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-stale-status-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-stale-status"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto controllerPrefix = controllerCert.getIdentity();
  const auto service = ndn::Name(SERVICE);
  const auto before = serviceController.getControllerVersion();
  const auto oldQuery = ServiceControllerTestAccess::statusQuery(controllerPrefix, service, before);
  const auto oldName = oldQuery.getName();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, oldQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  BOOST_CHECK(face.sentData.front().getName() == oldName);
  BOOST_REQUIRE(ndn::security::verifySignature(face.sentData.front(), signer));

  BOOST_REQUIRE(serviceController.revoke(
      makeIdentityRevocation("/spec179/stale-status/user")));
  const auto after = serviceController.getControllerVersion();
  BOOST_REQUIRE(after.compare(before) > 0);

  // The old exact name is immutable history, not current authority.  It must
  // never be republished by the current Controller after the revocation.
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, oldQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  // A forged future version is equally unavailable; only the current exact
  // version may produce a signed status Data.
  const ControllerVersion forged{after.controllerGenerationTimestamp,
                                 after.controllerEpoch + 100};
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController,
      ServiceControllerTestAccess::statusQuery(controllerPrefix, service, forged));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  const auto currentQuery = ServiceControllerTestAccess::statusQuery(controllerPrefix, service, after);
  const auto currentName = currentQuery.getName();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, currentQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  BOOST_CHECK(face.sentData.front().getName() == currentName);
  PolicyStatusData status;
  const auto content = face.sentData.front().getContent();
  auto [parsed, statusBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(status.wireDecode(statusBlock));
  BOOST_CHECK(status.getControllerVersion() == after);
  BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerGlobalCertificateRevocationUsesDigestScope)
{
  // A certificate-only target may intentionally omit targetIdentity.  The
  // Controller must then revoke every subject presenting that certificate
  // digest, regardless of service, while a replacement certificate remains
  // eligible.  This exercises the global-certificate branch in the real
  // ServiceController rather than only RevocationState's predicate.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-global-certificate-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-global-certificate",
                         "tpm-memory:spec179-controller-global-certificate");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-global-certificate"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());

  const auto providerA = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/example/hello/provider/A"));
  const auto providerB = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/example/hello/provider/B"));
  const auto digestA = ServiceControllerTestAccess::certificateDigest(providerA);
  const auto digestB = ServiceControllerTestAccess::certificateDigest(providerB);
  BOOST_REQUIRE(!digestA.empty());
  BOOST_REQUIRE(digestA != digestB);

  const auto before = serviceController.getControllerVersion();
  RevocationTarget globalCertificate;
  globalCertificate.kind = RevocationKind::CERTIFICATE;
  globalCertificate.certificateDigest = digestA;
  BOOST_REQUIRE(globalCertificate.targetIdentity.empty());
  BOOST_REQUIRE(serviceController.revoke(globalCertificate));
  const auto after = serviceController.getControllerVersion();
  BOOST_CHECK(after.compare(before) > 0);

  BOOST_CHECK(serviceController.isRevoked(
      providerA.getIdentity(), ndn::Name("/HELLO"), digestA));
  BOOST_CHECK(serviceController.isRevoked(
      providerA.getIdentity(), ndn::Name("/OtherService"), digestA));
  BOOST_CHECK(serviceController.isRevoked(
      providerB.getIdentity(), ndn::Name("/HELLO"), digestA));
  BOOST_CHECK(!serviceController.isRevoked(
      providerA.getIdentity(), ndn::Name("/HELLO"), digestB));
  BOOST_CHECK(!serviceController.isRevoked(
      providerA.getIdentity(), ndn::Name("/HELLO"), "sha256:replacement"));

  const auto helloStatus = serviceController.getPolicyStatus(ndn::Name("/HELLO"));
  const auto otherStatus = serviceController.getPolicyStatus(ndn::Name("/OtherService"));
  BOOST_REQUIRE_EQUAL(helloStatus.getRevocations().size(), 1);
  BOOST_REQUIRE_EQUAL(otherStatus.getRevocations().size(), 1);
  BOOST_CHECK(helloStatus.getRevocations().front().targetIdentity.empty());
  BOOST_CHECK(otherStatus.getRevocations().front().certificateDigest == digestA);

  const auto stableVersion = serviceController.getControllerVersion();
  BOOST_CHECK(!serviceController.revoke(globalCertificate));
  BOOST_CHECK(serviceController.getControllerVersion() == stableVersion);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-invalid-targets-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-invalid-targets",
                         "tpm-memory:spec179-controller-invalid-targets");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-invalid-targets"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto initial = serviceController.getControllerVersion();
  BOOST_REQUIRE(initial.isValid());

  RevocationTarget missingIdentity;
  missingIdentity.kind = RevocationKind::IDENTITY;

  RevocationTarget identityWithService;
  identityWithService.kind = RevocationKind::IDENTITY;
  identityWithService.targetIdentity = ndn::Name("/user/alice");
  identityWithService.serviceName = ndn::Name("/HELLO");

  RevocationTarget identityWithCertificate;
  identityWithCertificate.kind = RevocationKind::IDENTITY;
  identityWithCertificate.targetIdentity = ndn::Name("/user/alice");
  identityWithCertificate.certificateDigest = "sha256:0123456789abcdef";

  RevocationTarget missingCertificateDigest;
  missingCertificateDigest.kind = RevocationKind::CERTIFICATE;
  missingCertificateDigest.targetIdentity = ndn::Name("/user/alice");

  RevocationTarget certificateWithService;
  certificateWithService.kind = RevocationKind::CERTIFICATE;
  certificateWithService.certificateDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  certificateWithService.serviceName = ndn::Name("/HELLO");

  RevocationTarget missingServiceIdentity;
  missingServiceIdentity.kind = RevocationKind::SERVICE_AUTHORIZATION;
  missingServiceIdentity.serviceName = ndn::Name("/HELLO");

  RevocationTarget missingServiceName;
  missingServiceName.kind = RevocationKind::SERVICE_AUTHORIZATION;
  missingServiceName.targetIdentity = ndn::Name("/provider/p1");

  RevocationTarget serviceWithCertificate;
  serviceWithCertificate.kind = RevocationKind::SERVICE_AUTHORIZATION;
  serviceWithCertificate.targetIdentity = ndn::Name("/provider/p1");
  serviceWithCertificate.serviceName = ndn::Name("/HELLO");
  serviceWithCertificate.authorizationAttribute = ndn::Name("/SERVICE").append("HELLO");
  serviceWithCertificate.certificateDigest = "sha256:0123456789abcdef";

  RevocationTarget unknownKind;
  unknownKind.kind = static_cast<RevocationKind>(99);
  unknownKind.targetIdentity = ndn::Name("/user/alice");

  const std::vector<RevocationTarget> invalidTargets{
      missingIdentity, identityWithService, identityWithCertificate,
      missingCertificateDigest, certificateWithService, missingServiceIdentity,
      missingServiceName, serviceWithCertificate, unknownKind};
  for (const auto& target : invalidTargets) {
    BOOST_CHECK(!target.isValid());
    BOOST_CHECK(!serviceController.revoke(target));
    BOOST_CHECK(serviceController.getControllerVersion() == initial);
    BOOST_CHECK(serviceController.getPolicyStatus(ndn::Name("/HELLO"))
                .getRevocations().empty());
  }

  // A valid target still advances the epoch after all malformed attempts; the
  // failed calls above must not poison the in-memory or durable authority.
  const auto valid = makeIdentityRevocation("/user/alice");
  BOOST_REQUIRE(serviceController.revoke(valid));
  BOOST_CHECK(serviceController.getControllerVersion().compare(initial) > 0);
  BOOST_CHECK(serviceController.isRevoked(valid.targetIdentity,
                                          ndn::Name("/HELLO")));

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRevocationRollsBackAfterWriterLoss)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-revoke-rollback-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-rollback",
                         "tpm-memory:spec179-controller-rollback");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-rollback"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto initial = serviceController.getControllerVersion();
  BOOST_REQUIRE(initial.isValid());

  // Replace the durable lease after construction.  The revoke transaction
  // must fail before publication and restore both the in-memory target list
  // and the prior ControllerVersion.
  {
    std::ofstream lock(statePath.string() + ".lock", std::ios::trunc);
    lock << "replacement\n999999\n";
  }
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_CHECK(!serviceController.revoke(target));
  BOOST_CHECK(serviceController.getControllerVersion() == initial);
  BOOST_CHECK(serviceController.getPolicyStatus(ndn::Name("/HELLO"))
                  .getRevocations().empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRevocationRollsBackWhenStateCannotBeRead)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-revoke-persist-failure-" +
                          std::to_string(::getpid()) + ".bin");
  const auto savedPath = statePath.string() + ".saved";
  std::filesystem::remove_all(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  std::filesystem::remove(savedPath);
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-persist-failure",
                         "tpm-memory:spec179-controller-persist-failure");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-persist-failure"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto initial = serviceController.getControllerVersion();
  BOOST_REQUIRE(initial.isValid());

  // Make the durable target unreadable as a state record.  The Controller
  // must roll back the in-memory append and version when advanceEpoch()
  // cannot complete; this models a failure before atomic replacement.
  BOOST_REQUIRE(std::filesystem::exists(statePath));
  std::filesystem::rename(statePath, savedPath);
  BOOST_REQUIRE(std::filesystem::create_directory(statePath));

  const auto target = makeIdentityRevocation("/example/hello/user");
  BOOST_CHECK(!serviceController.revoke(target));
  BOOST_CHECK(serviceController.getControllerVersion() == initial);
  BOOST_CHECK(serviceController.getPolicyStatus(ndn::Name(SERVICE))
                  .getRevocations().empty());

  std::filesystem::remove_all(statePath);
  std::filesystem::rename(savedPath, statePath);
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerFiltersRevokedPermissionSnapshots)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-permission-filter-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-filter",
                         "tpm-memory:spec179-controller-filter");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-filter"), ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  const ndn::Name user("/example/hello/user");
  const ndn::Name provider("/example/hello/provider");
  const ndn::Name hello("/HELLO");
  const ndn::Name store("/NDNSF/DistributedRepo/Store");

  const auto userBefore = ServiceControllerTestAccess::userPermissions(
      serviceController, user);
  const auto providerBefore = ServiceControllerTestAccess::providerPermissions(
      serviceController, provider);
  BOOST_REQUIRE(!userBefore.getEntries().empty());
  BOOST_REQUIRE(!providerBefore.getEntries().empty());

  RevocationTarget userService;
  userService.kind = RevocationKind::SERVICE_AUTHORIZATION;
  userService.targetIdentity = user;
  userService.serviceName = hello;
  userService.authorizationAttribute = ndn::Name("/PERMISSION").append(hello);
  BOOST_REQUIRE(serviceController.revoke(userService));
  const auto userAfter = ServiceControllerTestAccess::userPermissions(
      serviceController, user);
  BOOST_CHECK(std::none_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [&hello] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == hello;
      }));
  BOOST_CHECK(std::any_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [&store] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == store;
      }));

  // Identity-wide withdrawal must remove the remaining service grants for
  // this role, not merely the service that was revoked above.
  RevocationTarget userIdentity;
  userIdentity.kind = RevocationKind::IDENTITY;
  userIdentity.targetIdentity = user;
  BOOST_REQUIRE(serviceController.revoke(userIdentity));
  const auto userGloballyRevoked = ServiceControllerTestAccess::userPermissions(
      serviceController, user);
  BOOST_CHECK(userGloballyRevoked.getEntries().empty());

  RevocationTarget providerService;
  providerService.kind = RevocationKind::SERVICE_AUTHORIZATION;
  providerService.targetIdentity = provider;
  providerService.serviceName = hello;
  providerService.authorizationAttribute = ndn::Name("/SERVICE").append(hello);
  BOOST_REQUIRE(serviceController.revoke(providerService));
  const auto providerAfter = ServiceControllerTestAccess::providerPermissions(
      serviceController, provider);
  BOOST_CHECK(std::none_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [&hello] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == hello;
      }));
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [&store] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == store;
      }));

  RevocationTarget providerIdentity;
  providerIdentity.kind = RevocationKind::IDENTITY;
  providerIdentity.targetIdentity = provider;
  BOOST_REQUIRE(serviceController.revoke(providerIdentity));
  const auto providerGloballyRevoked =
      ServiceControllerTestAccess::providerPermissions(serviceController, provider);
  BOOST_CHECK(providerGloballyRevoked.getEntries().empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerFailsClosedWhenGenerationStateIsCorrupt)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-authority-corrupt-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  {
    std::ofstream out(statePath, std::ios::binary);
    out << "not-a-generation-record";
  }
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-corrupt",
                         "tpm-memory:spec179-controller-corrupt");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-corrupt"), ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  ndn::DummyClientFace face(keyChain, faceOptions);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  BOOST_CHECK(!serviceController.getControllerVersion().isValid());
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_CHECK(!serviceController.revoke(target));
  BOOST_CHECK_THROW(serviceController.getPolicyStatus(ndn::Name("/HELLO")),
                    std::runtime_error);

  // Exercise the real status Interest handler, not only the public snapshot
  // builder.  A corrupt generation must not publish an authority Data packet.
  const auto statusInterest = ndn::Interest(
      ndn::Name("/controller/spec179-corrupt/NDNSF/POLICY-STATUS/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(serviceController,
                                                     statusInterest);
  BOOST_CHECK(face.sentData.empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRejectsSecondWriterAndPreservesAuthority)
{
  // The generation store has a unit-level fencing test, but the Controller
  // boundary must also refuse protected issuance when another instance owns
  // the same identity's durable writer lease.  This prevents a component
  // caller from mistaking a constructed-but-unready Controller for an
  // authority and publishing revocation/status Data from it.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-writer-conflict-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-writer-conflict",
                         "tpm-memory:spec179-controller-writer-conflict");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-writer-conflict"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace firstFace(keyChain);
  ndn::DummyClientFace secondFace(keyChain);
  ndn::ValidatorConfig firstValidator(firstFace);
  ndn::ValidatorConfig secondValidator(secondFace);

  {
    ServiceController first(firstFace, controllerCert, firstValidator,
                            ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_REQUIRE(first.getControllerVersion().isValid());

    ServiceController second(secondFace, controllerCert, secondValidator,
                             ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_CHECK(!second.getControllerVersion().isValid());

    RevocationTarget target;
    target.kind = RevocationKind::IDENTITY;
    target.targetIdentity = ndn::Name("/example/hello/user");
    BOOST_CHECK(!second.revoke(target));
    BOOST_CHECK_THROW(second.getPolicyStatus(ndn::Name("/HELLO")),
                      std::runtime_error);

    const auto statusInterest = ndn::Interest(
        ndn::Name("/controller/spec179-writer-conflict/NDNSF/POLICY-STATUS/HELLO"));
    ServiceControllerTestAccess::policyStatusInterest(second, statusInterest);
    BOOST_CHECK(secondFace.sentData.empty());

    // Losing the second instance does not fence or invalidate the original
    // writer.  The owner can still advance the epoch and publish its target.
    BOOST_REQUIRE(first.revoke(target));
    BOOST_CHECK(first.getControllerVersion().isValid());
    BOOST_CHECK(first.isRevoked(target.targetIdentity, ndn::Name("/HELLO")));
  }

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusHandlerRejectsMissingService)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-handler-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status-handler",
                         "tpm-memory:spec179-controller-status-handler");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-handler"), ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  ndn::DummyClientFace face(keyChain, faceOptions);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  const auto malformedInterest = ndn::Interest(
      ndn::Name("/controller/spec179-status-handler/NDNSF/POLICY-STATUS"));
  ServiceControllerTestAccess::policyStatusInterest(serviceController,
                                                     malformedInterest);
  BOOST_CHECK(face.sentData.empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusHandlerPublishesSignedCurrentStatus)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-handler-valid-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-valid"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signingCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  RevocationTarget revokedUser;
  revokedUser.kind = RevocationKind::IDENTITY;
  revokedUser.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_REQUIRE(serviceController.revoke(revokedUser));
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
  BOOST_REQUIRE(serviceController.getControllerVersion().isValid());

  const auto query = ServiceControllerTestAccess::statusQuery(
      controllerCert.getIdentity(), ndn::Name("/HELLO"));
  const auto interestName = query.getName();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, query);
  face.processEvents(ndn::time::milliseconds(1));
  const ndn::Data* published = nullptr;
  for (const auto& candidate : face.sentData) {
    const auto content = candidate.getContent();
    auto [parsed, inner] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(content.value(), content.value_size()));
    if (interestName.isPrefixOf(candidate.getName()) && parsed &&
        inner.type() == PolicyStatusData::TYPE) {
      published = &candidate;
      break;
    }
  }
  BOOST_REQUIRE(published != nullptr);
  const auto& data = *published;
  BOOST_CHECK(interestName.isPrefixOf(data.getName()));
  BOOST_CHECK(data.getName() == interestName);
  const auto parsedQuery = decodeControlQueryParameters(query.getApplicationParameters());
  BOOST_REQUIRE(parsedQuery);
  BOOST_CHECK(parsedQuery->service == ndn::Name("/HELLO"));
  BOOST_REQUIRE(data.getSignatureInfo());
  BOOST_CHECK(data.getSignatureValue().value_size() > 0);
  BOOST_CHECK(ndn::security::verifySignature(data, signingCert));

  PolicyStatusData status;
  const auto content = data.getContent();
  auto [parsed, statusBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(status.wireDecode(statusBlock));
  BOOST_CHECK(status.getServiceName() == ndn::Name("/HELLO"));
  BOOST_CHECK(status.getControllerVersion() ==
              serviceController.getControllerVersion());
  BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1);
  BOOST_CHECK(status.getRevocations().front().kind == RevocationKind::IDENTITY);
  BOOST_CHECK(status.getRevocations().front().targetIdentity ==
              revokedUser.targetIdentity);
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  BOOST_CHECK(status.validate(now));

  // Replaying the same prepared RPC must preserve its original reply name.
  const auto firstStatusName = data.getName();
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, query);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto secondStatusName = face.sentData.back().getName();
  BOOST_CHECK(interestName.isPrefixOf(secondStatusName));
  BOOST_CHECK(secondStatusName == firstStatusName);

  // A higher-version message hint must be resolvable to one immutable exact
  // status name.  The Controller serves that name only when the requested
  // version is its current authority; it must not silently substitute a
  // newer/older snapshot.
  const auto currentVersion = serviceController.getControllerVersion();
  const auto exactQuery = ServiceControllerTestAccess::statusQuery(
      ndn::Name("/controller/spec179-status-valid"),
      ndn::Name("/HELLO"), currentVersion);
  const auto exactName = exactQuery.getName();
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, exactQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  BOOST_CHECK(face.sentData.back().getName() == exactName);

  auto unavailableVersion = currentVersion;
  ++unavailableVersion.controllerEpoch;
  const auto unavailableQuery = ServiceControllerTestAccess::statusQuery(
      ndn::Name("/controller/spec179-status-valid"),
      ndn::Name("/HELLO"), unavailableVersion);
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, unavailableQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusHandlerPreservesParametersDigestAndRejectsWrongPrefix)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-handler-normalization-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-normalization"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto digestInterest = ServiceControllerTestAccess::statusQuery(
      controllerCert.getIdentity(), ndn::Name("/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, digestInterest);
  face.processEvents(ndn::time::milliseconds(1));

  const ndn::Data* published = nullptr;
  for (const auto& candidate : face.sentData) {
    // The final parameter digest is part of the exact original RPC reply.
    if (candidate.getName() == digestInterest.getName()) {
      published = &candidate;
      break;
    }
  }
  BOOST_REQUIRE(published != nullptr);
  BOOST_REQUIRE(published->getName() == digestInterest.getName());
  BOOST_REQUIRE(published->getSignatureInfo());
  BOOST_CHECK(ndn::security::verifySignature(*published, signer));
  PolicyStatusData status;
  const auto content = published->getContent();
  auto [parsed, statusBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(status.wireDecode(statusBlock));
  BOOST_CHECK(status.getServiceName() == ndn::Name("/HELLO"));
  BOOST_CHECK(status.validate(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())));

  face.sentData.clear();
  const auto wrongPrefix = ServiceControllerTestAccess::statusQuery(
      ndn::Name("/not-the-controller"), ndn::Name("/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(serviceController, wrongPrefix);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  auto malformedQuery = digestInterest;
  ndn::Name malformedName = malformedQuery.getName().getPrefix(-3);
  const std::vector<uint8_t> badDigest(32, 0x42);
  malformedName.append(ndn::name::Component(ndn::span<const uint8_t>(badDigest)));
  malformedName.append(digestInterest.getName()[-2]);
  malformedQuery.setName(malformedName);
  BOOST_REQUIRE(malformedQuery.isParametersDigestValid());
  BOOST_REQUIRE(!parseControlRpcName(malformedQuery, controllerCert.getIdentity(), "POLICY-STATUS"));
  ServiceControllerTestAccess::policyStatusInterest(serviceController, malformedQuery);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusRoundTripPreservesAuthorityScopes)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-wire-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status",
                         "tpm-memory:spec179-controller-status");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status"), ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  boost::asio::io_context io;
  ndn::DummyClientFace face(io, keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  RevocationTarget identity = makeIdentityRevocation("/example/hello/user");
  RevocationTarget certificate = makeCertificateRevocation(
      "/example/hello/provider-c", "sha256:old-certificate");
  certificate.targetIdentity.clear();
  RevocationTarget service = makeServiceRevocation("/example/hello/provider");
  BOOST_REQUIRE(serviceController.revoke(identity));
  BOOST_REQUIRE(serviceController.revoke(certificate));
  BOOST_REQUIRE(serviceController.revoke(service));

  const auto status = serviceController.getPolicyStatus(ndn::Name(SERVICE));
  const auto statusWire = status.wireEncode();
  PolicyStatusData decoded;
  BOOST_REQUIRE(decoded.wireDecode(statusWire));
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  BOOST_REQUIRE(decoded.validate(now));
  BOOST_CHECK_EQUAL(decoded.getRevocations().size(), 3);
  BOOST_CHECK(serviceController.isRevoked(ndn::Name("/example/hello/user"),
                                          ndn::Name("/OtherService")));
  BOOST_CHECK(serviceController.isRevoked(ndn::Name("/example/hello/provider-c"),
                                          ndn::Name(SERVICE), "sha256:old-certificate"));
  BOOST_CHECK(serviceController.isRevoked(ndn::Name("/another/provider"),
                                          ndn::Name(SERVICE), "sha256:old-certificate"));
  BOOST_CHECK(!serviceController.isRevoked(ndn::Name("/example/hello/provider-c"),
                                           ndn::Name(SERVICE), "sha256:new-certificate"));

  const auto otherStatus = serviceController.getPolicyStatus(ndn::Name("/OtherService"));
  BOOST_CHECK_EQUAL(otherStatus.getRevocations().size(), 2);
  BOOST_CHECK(serviceController.isRevoked(ndn::Name("/example/hello/provider"),
                                          ndn::Name(SERVICE)));
  BOOST_CHECK(!serviceController.isRevoked(ndn::Name("/example/hello/provider"),
                                           ndn::Name("/OtherService")));
  BOOST_CHECK_THROW(serviceController.getPolicyStatus(ndn::Name()), std::runtime_error);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerReusesAbePairAcrossRestartAndAfterRevocation)
{
  const auto statePath = std::filesystem::temp_directory_path() /
    ("ndnsf-controller-abe-restart-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);
  ndn::KeyChain keys;
  const auto certificate = keys.createIdentity(ndn::Name("/controller/spec220-abe"),
                                                ndn::RsaKeyParams(2048))
                             .getDefaultKey().getDefaultCertificate();
  std::string originalDigest;
  std::string revokedDigest;
  ndn::Name originalName;
  ndn::Block originalWire;
  ndn::Name revokedName;
  ndn::Block revokedWire;
  const auto fetch = [] (ServiceController& controller, ndn::DummyClientFace& face,
                         const ndn::Name& name) {
    face.sentData.clear();
    ServiceControllerTestAccess::requestAbeParameters(controller, ndn::Interest(name));
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    return face.sentData.front().wireEncode();
  };
  constexpr uint64_t highArchivedVersion = std::numeric_limits<uint64_t>::max() - 2;
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, certificate, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
    const auto status = controller.getPolicyStatus(ndn::Name(SERVICE));
    originalDigest = status.getAbePublicParametersDigest();
    originalName = status.getAbePublicParametersName();
    originalWire = fetch(controller, face, originalName);
    BOOST_REQUIRE(!originalDigest.empty());
  }
  {
    ControllerGenerationStore store(statePath);
    auto seal = loadFileKeyRingEntry("controller-abe");
    BOOST_REQUIRE(seal);
    store.setSecretSeal(std::move(seal->key), seal->keyId);
    BOOST_REQUIRE(store.acquireWriter(certificate.getIdentity().toUri(ndn::name::UriFormat::CANONICAL)));
    const auto current = store.loadAbeMaterial();
    BOOST_REQUIRE(current);
    ControllerGenerationStore::AbeHistoryEntry legacy;
    legacy.generationTimestamp = 1000;
    legacy.epoch = 2001;
    legacy.publicParameters = current->publicParameters;
    legacy.masterSecret = current->masterSecret;
    legacy.publicParametersName = originalName.getPrefix(-1).appendVersion(highArchivedVersion)
      .toUri(ndn::name::UriFormat::CANONICAL);
    legacy.publicParametersWireDigest = originalDigest;
    legacy.reason = "fixture-high-legacy-version";
    store.recordAbeHistory(legacy);
  }
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, certificate, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_REQUIRE(controller.getControllerVersion().isValid());
    BOOST_CHECK_EQUAL(controller.getPolicyStatus(ndn::Name(SERVICE)).getAbePublicParametersDigest(),
                      originalDigest);
    BOOST_CHECK(fetch(controller, face, originalName) == originalWire);
    BOOST_REQUIRE(controller.revoke(makeIdentityRevocation("/example/hello/user")));
    const auto status = controller.getPolicyStatus(ndn::Name(SERVICE));
    revokedDigest = status.getAbePublicParametersDigest();
    revokedName = status.getAbePublicParametersName();
    BOOST_CHECK_EQUAL(revokedName.at(-1).toVersion(), highArchivedVersion + 1);
    revokedWire = fetch(controller, face, revokedName);
    BOOST_CHECK(fetch(controller, face, originalName) == originalWire);
    face.sentData.clear();
    auto discovery = ndn::Interest(originalName.getPrefix(-1));
    discovery.setCanBePrefix(true);
    ServiceControllerTestAccess::requestAbeParameters(controller, discovery);
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    BOOST_CHECK(face.sentData.front().wireEncode() == revokedWire);
    BOOST_CHECK_NE(revokedDigest, originalDigest);
  }
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, certificate, validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_REQUIRE(controller.getControllerVersion().isValid());
    BOOST_CHECK_EQUAL(controller.getPolicyStatus(ndn::Name(SERVICE)).getAbePublicParametersDigest(),
                      revokedDigest);
    BOOST_CHECK(fetch(controller, face, revokedName) == revokedWire);
    BOOST_CHECK(fetch(controller, face, ndn::Data(originalWire).getFullName()) == originalWire);
    BOOST_CHECK(controller.isRevoked(ndn::Name("/example/hello/user"), ndn::Name(SERVICE)));
  }
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRestoresRevocationsAfterRestart)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-restart-revocations-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-restart",
                         "tpm-memory:spec179-controller-restart");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-restart"), ndn::RsaKeyParams(2048));
  auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ControllerVersion first;
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController serviceController(face, controllerCert, validator,
                                        ndnsf::test::testFixturePath("controller-policies.conf").string());
    RevocationTarget target = makeIdentityRevocation("/example/hello/user");
    BOOST_REQUIRE(serviceController.revoke(target));
    first = serviceController.getControllerVersion();
  }
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController restarted(face, controllerCert, validator,
                                 ndnsf::test::testFixturePath("controller-policies.conf").string());
    const auto second = restarted.getControllerVersion();
    BOOST_CHECK(second.controllerGenerationTimestamp > first.controllerGenerationTimestamp);
    BOOST_CHECK_EQUAL(second.controllerEpoch, 1);
    BOOST_CHECK(restarted.isRevoked(ndn::Name("/example/hello/user"),
                                    ndn::Name(SERVICE)));
    BOOST_CHECK_EQUAL(restarted.getPolicyStatus(ndn::Name(SERVICE)).getRevocations().size(), 1);
  }

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerRestoresEveryRevocationKindAfterRestart)
{
  // Persist all three typed target kinds together, then restart the real
  // Controller and verify that the restored snapshot preserves both global
  // and service-scoped matching.  This complements the single identity
  // restart case above and guards against silently dropping certificate or
  // service targets when a new generation is created.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-restart-all-targets-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-restart-all-targets",
                         "tpm-memory:spec179-controller-restart-all-targets");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-restart-all-targets"),
      ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  const ndn::Name user("/example/hello/user");
  const ndn::Name provider("/example/hello/provider");
  const ndn::Name hello("/HELLO");
  const ndn::Name otherService("/OtherService");
  const std::string oldCertificateDigest =
      "sha256:old-provider-certificate";
  ControllerVersion firstVersion;

  {
    ndn::DummyClientFace face(keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController serviceController(face, controllerCert, validator,
                                         ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_REQUIRE(serviceController.revoke(makeIdentityRevocation(user.toUri().c_str())));

    RevocationTarget globalCertificate;
    globalCertificate.kind = RevocationKind::CERTIFICATE;
    globalCertificate.certificateDigest = oldCertificateDigest;
    BOOST_REQUIRE(serviceController.revoke(globalCertificate));
    RevocationTarget serviceTarget;
    serviceTarget.kind = RevocationKind::SERVICE_AUTHORIZATION;
    serviceTarget.targetIdentity = provider;
    serviceTarget.serviceName = hello;
    serviceTarget.authorizationAttribute = ndn::Name("/SERVICE").append(hello);
    BOOST_REQUIRE(serviceController.revoke(serviceTarget));
    firstVersion = serviceController.getControllerVersion();

    const auto helloStatus = serviceController.getPolicyStatus(hello);
    const auto otherStatus = serviceController.getPolicyStatus(otherService);
    BOOST_REQUIRE_EQUAL(helloStatus.getRevocations().size(), 3);
    BOOST_REQUIRE_EQUAL(otherStatus.getRevocations().size(), 2);
  }

  {
    ndn::DummyClientFace face(keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController restarted(face, controllerCert, validator,
                                ndnsf::test::testFixturePath("controller-policies.conf").string());
    const auto secondVersion = restarted.getControllerVersion();
    BOOST_CHECK(secondVersion.controllerGenerationTimestamp >
                firstVersion.controllerGenerationTimestamp);
    BOOST_CHECK_EQUAL(secondVersion.controllerEpoch, 1);

    // Identity-wide withdrawal survives restart for every service.
    BOOST_CHECK(restarted.isRevoked(user, hello));
    BOOST_CHECK(restarted.isRevoked(user, otherService));
    // Global certificate withdrawal survives restart for any identity and
    // service presenting that digest, while a replacement digest remains live.
    BOOST_CHECK(restarted.isRevoked(provider, hello, oldCertificateDigest));
    BOOST_CHECK(restarted.isRevoked(ndn::Name("/unrelated/provider"),
                                    otherService, oldCertificateDigest));
    BOOST_CHECK(!restarted.isRevoked(provider, otherService,
                                     "sha256:replacement-certificate"));
    // Service authorization remains scoped to /HELLO after restart.
    BOOST_CHECK(restarted.isRevoked(provider, hello));
    BOOST_CHECK(!restarted.isRevoked(provider, otherService));

    const auto helloStatus = restarted.getPolicyStatus(hello);
    const auto otherStatus = restarted.getPolicyStatus(otherService);
    BOOST_REQUIRE_EQUAL(helloStatus.getRevocations().size(), 3);
    BOOST_REQUIRE_EQUAL(otherStatus.getRevocations().size(), 2);
    BOOST_TEST_MESSAGE(
        "NDNSF_REVOCATION_RESTART target_kinds=identity,certificate,service"
        " hello_targets=3 other_service_targets=2 restored_epoch=1");
  }

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusVersionIsMonotonicAcrossRestarts)
{
  // This is the component-level counterpart to the generation-store clock
  // rollback test.  It exercises the production Controller status handler
  // across multiple starts: one generation's exact status name is stable,
  // the next start advances the persisted generation, and an old exact name
  // cannot make the restarted Controller publish stale authority.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-restart-monotonic-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-restart"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto target = makeIdentityRevocation("/example/hello/user");
  ControllerVersion firstVersion;
  ndn::Name firstStatusName;
  ndn::Interest firstStatusQuery;
  ndn::Name firstParametersName;
  std::string firstParametersDigest;

  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController serviceController(face, controllerCert, validator,
                                        ndnsf::test::testFixturePath("controller-policies.conf").string());
    BOOST_REQUIRE(serviceController.getControllerVersion().isValid());
    ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
    BOOST_REQUIRE(serviceController.revoke(target));
    firstVersion = serviceController.getControllerVersion();
    const auto firstStatus = serviceController.getPolicyStatus(ndn::Name(SERVICE));
    firstParametersName = firstStatus.getAbePublicParametersName();
    firstParametersDigest = firstStatus.getAbePublicParametersDigest();
    ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

    const auto exactQuery = ServiceControllerTestAccess::statusQuery(
        controllerPrefix, ndn::Name(SERVICE), firstVersion);
    const auto exactName = exactQuery.getName();
    firstStatusQuery = exactQuery;
    ServiceControllerTestAccess::policyStatusInterest(
        serviceController, exactQuery);
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
    firstStatusName = face.sentData.front().getName();
    BOOST_CHECK(firstStatusName == exactName);

    PolicyStatusData status;
    const auto content = face.sentData.front().getContent();
    auto [parsed, statusBlock] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(content.value(), content.value_size()));
    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE(status.wireDecode(statusBlock));
    BOOST_CHECK(status.getControllerVersion() == firstVersion);
    BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1);
    BOOST_CHECK(status.getRevocations().front().targetIdentity ==
                target.targetIdentity);

    // Repeating an exact request in the same Controller generation must not
    // create a second timestamp/version identity.  The status validity
    // interval is also stable, so a cache can safely key the snapshot by its
    // exact (generation, epoch) name.
    const auto firstValidFrom = status.getValidFromMs();
    const auto firstValidUntil = status.getValidUntilMs();
    face.sentData.clear();
    ServiceControllerTestAccess::policyStatusInterest(
        serviceController, exactQuery);
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
    BOOST_CHECK(face.sentData.front().getName() == firstStatusName);
    PolicyStatusData repeated;
    const auto repeatedContent = face.sentData.front().getContent();
    auto [repeatedParsed, repeatedBlock] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(repeatedContent.value(),
                                 repeatedContent.value_size()));
    BOOST_REQUIRE(repeatedParsed);
    BOOST_REQUIRE(repeated.wireDecode(repeatedBlock));
    BOOST_CHECK_EQUAL(repeated.getValidFromMs(), firstValidFrom);
    BOOST_CHECK_EQUAL(repeated.getValidUntilMs(), firstValidUntil);
    BOOST_CHECK(repeated.getControllerVersion() == firstVersion);
  }

  ControllerVersion secondVersion;
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController restarted(face, controllerCert, validator,
                                ndnsf::test::testFixturePath("controller-policies.conf").string());
    secondVersion = restarted.getControllerVersion();
    BOOST_REQUIRE(secondVersion.isValid());
    ServiceControllerTestAccess::ensureInternalControllerSigner(restarted);
    BOOST_CHECK(secondVersion.controllerGenerationTimestamp >
                firstVersion.controllerGenerationTimestamp);
    BOOST_CHECK_EQUAL(secondVersion.controllerEpoch, 1);
    BOOST_CHECK(restarted.isRevoked(target.targetIdentity, ndn::Name(SERVICE)));
    const auto secondStatus = restarted.getPolicyStatus(ndn::Name(SERVICE));
    BOOST_CHECK_EQUAL(secondStatus.getAbePublicParametersName(), firstParametersName);
    BOOST_CHECK_EQUAL(secondStatus.getAbePublicParametersDigest(), firstParametersDigest);
    ServiceControllerTestAccess::registerHandlersForDummyFace(restarted);

    // A cached packet from the previous generation is not an authority
    // candidate for the restarted Controller and must produce no Data.
    face.sentData.clear();
    ServiceControllerTestAccess::policyStatusInterest(
        restarted, firstStatusQuery);
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_CHECK(face.sentData.empty());

    const auto currentQuery = ServiceControllerTestAccess::statusQuery(
        controllerPrefix, ndn::Name(SERVICE), secondVersion);
    const auto currentName = currentQuery.getName();
    ServiceControllerTestAccess::policyStatusInterest(
        restarted, currentQuery);
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
    BOOST_CHECK(face.sentData.front().getName() == currentName);

    PolicyStatusData status;
    const auto content = face.sentData.front().getContent();
    auto [parsed, statusBlock] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(content.value(), content.value_size()));
    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE(status.wireDecode(statusBlock));
    BOOST_CHECK(status.getControllerVersion() == secondVersion);
    BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1);
    BOOST_CHECK(status.getRevocations().front().targetIdentity ==
                target.targetIdentity);
  }

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerPermissionHandlersEncryptCurrentRevocation)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-permission-wire-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-permission-wire"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  const ndn::Name user("/example/hello/user");
  const ndn::Name provider("/example/hello/provider");
  const ndn::Name unaffectedProvider("/example/hello/provider/A");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, user);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, provider);
  const auto unaffectedProviderCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, unaffectedProvider);
  const auto userEncryptionCert =
      ServiceControllerTestAccess::rsaEncryptionCertificate(serviceController, userCert);
  const auto providerEncryptionCert =
      ServiceControllerTestAccess::rsaEncryptionCertificate(serviceController, providerCert);
  const auto unaffectedProviderEncryptionCert =
      ServiceControllerTestAccess::rsaEncryptionCertificate(
          serviceController, unaffectedProviderCert);
  const auto internalSigner = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  const ndn::Name controllerPrefix("/controller/spec179-permission-wire");
  BOOST_REQUIRE(!userCert.getName().empty());
  BOOST_REQUIRE(!providerCert.getName().empty());
  BOOST_REQUIRE(!internalSigner.getName().empty());
  BOOST_REQUIRE(serviceController.getControllerVersion().isValid());
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto permissionQuery = [&] (const std::string& operation, const ndn::Name& subject) {
    const auto parameters = encodeControlQueryParameters({subject, ndn::Name(), 0, 0});
    ndn::Interest query(makeControlRpcName(controllerPrefix, operation, parameters,
                                         makeControlQueryNonce()));
    query.setApplicationParameters(parameters);
    query.setMustBeFresh(true);
    query.setCanBePrefix(false);
    return query;
  };
  auto userInterest = permissionQuery("USER-PERMISSIONS", user);
  auto providerInterest = permissionQuery("PROVIDER-PERMISSIONS", provider);
  const auto unaffectedProviderInterest =
      permissionQuery("PROVIDER-PERMISSIONS", unaffectedProvider);
  const auto decodeEncrypted = [] (const ndn::Data& data) {
    EncryptedPermissionResponse response;
    const auto& content = data.getContent();
    if (content.type() == tlv::EncryptedPermissionResponseType) {
      if (!response.WireDecode(content))
        throw std::runtime_error("invalid encrypted permission content");
      return response;
    }
    auto [ok, block] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(content.value(), content.value_size()));
    if (!ok || !response.WireDecode(block))
      throw std::runtime_error("invalid nested encrypted permission content");
    return response;
  };

  // Permission handlers must not issue protected material for malformed or
  // unknown targets.  This is distinct from a valid revoked target, which
  // receives a current-version encrypted empty snapshot below.
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController,
      ndn::Interest(ServiceControllerTestAccess::userPermissionsPrefix(
          serviceController)));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController,
      permissionQuery("USER-PERMISSIONS", ndn::Name("/unknown/identity")));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, userInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto encryptedUser = decodeEncrypted(face.sentData.back());
  BOOST_CHECK(ndn::Name(encryptedUser.getRecipientCertName()) == userEncryptionCert.getName());
  const auto userBefore = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, encryptedUser);
  BOOST_CHECK(ndn::Name(userBefore.getTargetIdentity()) == user);
  BOOST_CHECK(!userBefore.getEntries().empty());
  const auto& userBeforeDataName = face.sentData.back().getName();
  BOOST_CHECK(userBeforeDataName == userInterest.getName());

  // Exercise the Provider handler on the same pre-revocation authority
  // snapshot.  The post-revocation assertion below must therefore prove that
  // a service-scoped withdrawal changes only the affected User grant.
  face.sentData.clear();
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, providerInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto providerBeforeWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK(ndn::Name(providerBeforeWire.getRecipientCertName()) ==
              providerEncryptionCert.getName());
  const auto providerBefore = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, providerBeforeWire);
  const auto& providerBeforeDataName = face.sentData.back().getName();
  BOOST_CHECK(providerBeforeDataName == providerInterest.getName());
  BOOST_CHECK(std::any_of(
      providerBefore.getEntries().begin(), providerBefore.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));

  // Replacing the recipient certificate must make the same ciphertext
  // undecryptable; a service-wide DKEY would not provide this boundary.
  auto wrongRecipient = encryptedUser;
  wrongRecipient.setRecipientCertName(providerEncryptionCert.getName().toUri());
  BOOST_CHECK_THROW(
      ServiceControllerTestAccess::decryptPermissionResponse(
          serviceController, wrongRecipient), std::exception);

  face.sentData.clear();
  RevocationTarget userService;
  userService.kind = RevocationKind::SERVICE_AUTHORIZATION;
  userService.targetIdentity = user;
  userService.serviceName = ndn::Name("/HELLO");
  userService.authorizationAttribute = ndn::Name("/PERMISSION").append("HELLO");
  BOOST_REQUIRE(serviceController.revoke(userService));
  userInterest = permissionQuery("USER-PERMISSIONS", user);
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, userInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto revokedUserWire = decodeEncrypted(face.sentData.back());
  const auto userAfter = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, revokedUserWire);
  BOOST_CHECK(userAfter.getControllerVersion().compare(
      userBefore.getControllerVersion()) > 0);
  // Revocation is forward-only: the already-issued snapshot remains a valid
  // historical artifact for its original holder, while renewal is empty and
  // bound to the newer ControllerVersion.
  BOOST_CHECK(userBefore.getControllerVersion().compare(
      userAfter.getControllerVersion()) < 0);
  BOOST_CHECK(std::any_of(
      userBefore.getEntries().begin(), userBefore.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));
  BOOST_CHECK(std::none_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));
  BOOST_CHECK(std::any_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/NDNSF/DistributedRepo/Store");
      }));

  // The Provider handler uses the same recipient-specific encryption and
  // keeps an unrelated service grant after a service-scoped withdrawal.
  face.sentData.clear();
  providerInterest = permissionQuery("PROVIDER-PERMISSIONS", provider);
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, providerInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto providerWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK(ndn::Name(providerWire.getRecipientCertName()) ==
              providerEncryptionCert.getName());
  const auto providerAfter = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, providerWire);
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/NDNSF/DistributedRepo/Store");
      }));

  // Identity-wide withdrawal must deny renewal for the whole role, while the
  // Controller still returns a current-version, recipient-bound snapshot
  // that contains no grants.  This exercises the authority decision itself,
  // not only the service-table filtering above.
  face.sentData.clear();
  RevocationTarget userIdentity;
  userIdentity.kind = RevocationKind::IDENTITY;
  userIdentity.targetIdentity = user;
  BOOST_REQUIRE(serviceController.revoke(userIdentity));
  userInterest = permissionQuery("USER-PERMISSIONS", user);
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, userInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto identityRevokedUserWire = decodeEncrypted(face.sentData.back());
  const auto identityRevokedUser = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, identityRevokedUserWire);
  const auto& identityRevokedDataName = face.sentData.back().getName();
  BOOST_CHECK(identityRevokedDataName == userInterest.getName());
  BOOST_CHECK(identityRevokedUser.getEntries().empty());
  BOOST_CHECK(identityRevokedUser.getControllerVersion().compare(
      providerAfter.getControllerVersion()) > 0);

  // Certificate-only withdrawal must also deny renewal for the revoked
  // certificate without relying on identity-wide withdrawal semantics.
  face.sentData.clear();
  RevocationTarget providerCertificate;
  providerCertificate.kind = RevocationKind::CERTIFICATE;
  providerCertificate.targetIdentity = provider;
  providerCertificate.certificateDigest =
      ServiceControllerTestAccess::certificateDigest(providerEncryptionCert);
  BOOST_REQUIRE(serviceController.revoke(providerCertificate));
  providerInterest = permissionQuery("PROVIDER-PERMISSIONS", provider);
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, providerInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto revokedProviderWire = decodeEncrypted(face.sentData.back());
  const auto revokedProvider = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, revokedProviderWire);
  const auto& revokedProviderDataName = face.sentData.back().getName();
  BOOST_CHECK(revokedProviderDataName == providerInterest.getName());
  BOOST_CHECK(revokedProvider.getEntries().empty());

  // Certificate-only withdrawal must not over-revoke another identity that
  // uses the same service.  Checking the affected Provider's empty snapshot
  // alone would not establish this live issuance control.
  face.sentData.clear();
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, unaffectedProviderInterest);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto unaffectedProviderWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK(ndn::Name(unaffectedProviderWire.getRecipientCertName()) ==
              unaffectedProviderEncryptionCert.getName());
  const auto unaffectedProviderResponse =
      ServiceControllerTestAccess::decryptPermissionResponse(
          serviceController, unaffectedProviderWire);
  BOOST_CHECK(std::any_of(
      unaffectedProviderResponse.getEntries().begin(),
      unaffectedProviderResponse.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return ndn::Name(entry.getServiceName()) == ndn::Name("/HELLO");
      }));

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusSignatureRejectsWrongSignerAndTampering)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-signature-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain;
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-signature"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signingCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  const auto wrongCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/controller/spec179-wrong-signer"));
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto query = ServiceControllerTestAccess::statusQuery(
      controllerCert.getIdentity(), ndn::Name("/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, query);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE(!face.sentData.empty());
  const auto& published = face.sentData.back();
  BOOST_REQUIRE(published.getSignatureInfo());
  BOOST_CHECK(ndn::security::verifySignature(published, signingCert));
  BOOST_CHECK(!ndn::security::verifySignature(published, wrongCert));

  std::vector<uint8_t> tamperedBytes(published.getContent().value(),
                                     published.getContent().value() +
                                       published.getContent().value_size());
  tamperedBytes.push_back(0);
  ndn::Buffer tamperedContent(tamperedBytes.data(), tamperedBytes.size());
  ndn::Data tampered = published;
  tampered.setContent(tamperedContent);
  BOOST_CHECK(!ndn::security::verifySignature(tampered, signingCert));

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ConfiguredTrustSchemaControlsControllerStatusValidation)
{
  // Controller status is authority material.  It must go through the
  // configured trust anchor even when a matching certificate happens to be
  // present in a local PIB; the ordinary SVS application-data validator's
  // local signature shortcut is intentionally not sufficient here.
  const auto nonce = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
  const auto rootPath = std::filesystem::temp_directory_path() /
                        ("ndnsf-spec179-trust-root-" + nonce + ".cert");
  const auto schemaPath = std::filesystem::temp_directory_path() /
                          ("ndnsf-spec179-trust-schema-" + nonce + ".conf");

  ndn::KeyChain trustedKeys("pib-memory:spec179-trust-root",
                            "tpm-memory:spec179-trust-root");
  const auto trustedIdentity = trustedKeys.createIdentity(
      ndn::Name("/spec179/trusted-controller"), ndn::RsaKeyParams(2048));
  const auto trustedCert = trustedIdentity.getDefaultKey().getDefaultCertificate();
  ndn::io::save(trustedCert, rootPath.string());

  ndn::KeyChain untrustedKeys("pib-memory:spec179-untrusted-status",
                              "tpm-memory:spec179-untrusted-status");
  const auto untrustedIdentity = untrustedKeys.createIdentity(
      ndn::Name("/spec179/untrusted-controller"), ndn::RsaKeyParams(2048));
  const auto untrustedCert = untrustedIdentity.getDefaultKey().getDefaultCertificate();

  {
    std::ofstream schema(schemaPath);
    schema << "rule\n{\n"
           << "  id \"Spec179 controller status\"\n"
           << "  for data\n"
           << "  filter { type name regex ^<spec179|8=spec179><>*$ }\n"
           << "  checker { type customized sig-type rsa-sha256\n"
           << "    key-locator { type name hyper-relation {\n"
           << "      k-regex \"^(<>*)<KEY|8=KEY><>{1,3}$\" k-expand \"\\\\1\"\n"
           << "      h-relation is-prefix-of p-regex \"^(<>*)$\" p-expand \"\\\\1\"\n"
           << "    } } }\n"
           << "}\n"
           << "trust-anchor\n{\n"
           << "  type file\n"
           << "  file-name \"" << rootPath.string() << "\"\n"
           << "}\n";
  }

  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(ControllerVersion{1, 1});
  status.setValidity(1, 5000000000000ULL);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(trustedCert.getName());

  const auto statusName = ServiceControllerTestAccess::statusQuery(
      trustedCert.getIdentity(), status.getServiceName(), status.getControllerVersion()).getName();
  ndn::Data trustedData(statusName);
  trustedData.setContent(status.wireEncode());
  trustedKeys.sign(trustedData, ndn::security::signingByCertificate(trustedCert));

  ndn::Data untrustedData(statusName);
  untrustedData.setContent(status.wireEncode());
  untrustedKeys.sign(untrustedData,
                     ndn::security::signingByCertificate(untrustedCert));

  ndn::DummyClientFace face;
  auto validator = std::make_shared<MessageValidator>(
      schemaPath.string(), std::nullopt, &face);
  size_t trustedSuccess = 0;
  size_t untrustedSuccess = 0;
  size_t failures = 0;
  validator->validateWithConfiguredTrustSchema(
      trustedData,
      [&] (const ndn::Data&) { ++trustedSuccess; },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) { ++failures; });
  validator->validateWithConfiguredTrustSchema(
      untrustedData,
      [&] (const ndn::Data&) { ++untrustedSuccess; },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) { ++failures; });
  for (size_t i = 0; i < 100 && trustedSuccess == 0 && untrustedSuccess == 0 &&
                         failures < 2; ++i) {
    face.processEvents(ndn::time::milliseconds(5));
  }

  BOOST_CHECK_EQUAL(trustedSuccess, 1U);
  BOOST_CHECK_EQUAL(untrustedSuccess, 0U);
  BOOST_CHECK_EQUAL(failures, 1U);

  std::filesystem::remove(schemaPath);
  std::filesystem::remove(rootPath);
}

BOOST_AUTO_TEST_CASE(HierarchicalConfiguredTrustAnchorControlsControllerStatusValidation)
{
  // FR-040 / RV-I33: a hierarchical configured file trust anchor (root CA ->
  // intermediate CA -> Controller certificate) must accept live Controller
  // status when the whole chain is anchored, and must reject a chain-external
  // signer and a broken-chain intermediate before installation.  The
  // depth-0 case (signer == file anchor) is covered by
  // ConfiguredTrustSchemaControlsControllerStatusValidation above; this case
  // resolves the intermediate certificates over the callback face exactly
  // like the production CertificateFetcherFromNetwork path does.
  const auto nonce = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
  const auto rootPath = std::filesystem::temp_directory_path() /
                        ("ndnsf-spec179-hier-root-" + nonce + ".cert");
  const auto schemaPath = std::filesystem::temp_directory_path() /
                          ("ndnsf-spec179-hier-schema-" + nonce + ".conf");

  // --- PKI: anchor /spec179/hier, CA /spec179/hier/ca, Controller
  // /spec179/hier/ca/controller.  Each level owns an isolated PIB/TPM; the
  // intermediate and Controller certificates are issued by their parent with
  // KeyChain::makeCertificate() and deterministic names.
  ndn::KeyChain rootKeys("pib-memory:spec179-hier-root-" + nonce,
                         "tpm-memory:spec179-hier-root-" + nonce);
  const auto rootIdentity =
      rootKeys.createIdentity(ndn::Name("/spec179/hier"), ndn::RsaKeyParams(2048));
  const auto rootKey = rootIdentity.getDefaultKey();
  const auto rootCert = rootKey.getDefaultCertificate();
  ndn::io::save(rootCert, rootPath.string());

  ndn::KeyChain caKeys("pib-memory:spec179-hier-ca-" + nonce,
                       "tpm-memory:spec179-hier-ca-" + nonce);
  const auto caIdentity =
      caKeys.createIdentity(ndn::Name("/spec179/hier/ca"), ndn::RsaKeyParams(2048));
  const auto caKey = caIdentity.getDefaultKey();

  ndn::KeyChain ctlKeys("pib-memory:spec179-hier-ctl-" + nonce,
                        "tpm-memory:spec179-hier-ctl-" + nonce);
  const auto ctlIdentity = ctlKeys.createIdentity(
      ndn::Name("/spec179/hier/ca/controller"), ndn::RsaKeyParams(2048));
  const auto ctlKey = ctlIdentity.getDefaultKey();

  ndn::security::MakeCertificateOptions caOpts;
  caOpts.issuerId = ndn::Name::Component("ROOT");
  caOpts.version = 1;
  const auto caCert = rootKeys.makeCertificate(
      caKey, ndn::security::signingByCertificate(rootCert), caOpts);
  caKeys.addCertificate(caKey, caCert);

  ndn::security::MakeCertificateOptions ctlOpts;
  ctlOpts.issuerId = ndn::Name::Component("CA");
  ctlOpts.version = 1;
  const auto ctlCert = caKeys.makeCertificate(
      ctlKey, ndn::security::signingByCertificate(caCert), ctlOpts);
  // makeCertificate only needs the subject key; register the issued
  // certificate so signing below can resolve signingByCertificate(ctlCert)
  // in the Controller's own KeyChain.
  ctlKeys.addCertificate(ctlKey, ctlCert);

  // Chain-external signer: an attacker-issued identity that sits inside the
  // Controller name space and self-signs its certificate.  Its status must
  // be rejected even though the name rules pass and the certificate is
  // fetchable.
  ndn::KeyChain rogueKeys("pib-memory:spec179-hier-rogue-" + nonce,
                          "tpm-memory:spec179-hier-rogue-" + nonce);
  const auto rogueCert = rogueKeys.createIdentity(
      ndn::Name("/spec179/hier/ca/controller/rogue"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();

  // Broken-chain intermediate: an entity outside the anchor signs a
  // certificate for the CA key.  The Controller certificate below is then
  // signed by the real CA key against that counterfeit CA certificate, so
  // the presented chain carries the right key names but its intermediate
  // does not terminate at the configured anchor (stranger self-signed).
  // makeCertificate() resolves the signer by the signer-certificate key
  // name, so the CA-issued Controller certificate must be produced by the
  // KeyChain that actually holds the CA private key.
  ndn::KeyChain strangerKeys("pib-memory:spec179-hier-stranger-" + nonce,
                             "tpm-memory:spec179-hier-stranger-" + nonce);
  const auto strangerSelfCert = strangerKeys.createIdentity(
      ndn::Name("/spec179/stranger"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  ndn::security::MakeCertificateOptions brokenCaOpts;
  brokenCaOpts.issuerId = ndn::Name::Component("STRANGER");
  brokenCaOpts.version = 2;
  const auto brokenCaCert = strangerKeys.makeCertificate(
      caKey, ndn::security::signingByCertificate(strangerSelfCert),
      brokenCaOpts);
  ndn::security::MakeCertificateOptions brokenCtlOpts;
  brokenCtlOpts.issuerId = ndn::Name::Component("STRANGER");
  brokenCtlOpts.version = 2;
  const auto brokenCtlCert = caKeys.makeCertificate(
      ctlKey, ndn::security::signingByCertificate(brokenCaCert),
      brokenCtlOpts);
  ctlKeys.addCertificate(ctlKey, brokenCtlCert);

  {
    std::ofstream schema(schemaPath);
    schema << "rule\n{\n"
           << "  id \"Spec179 hierarchical controller status\"\n"
           << "  for data\n"
           << "  filter { type name regex ^<spec179|8=spec179><>*$ }\n"
           << "  checker { type customized sig-type rsa-sha256\n"
           << "    key-locator { type name hyper-relation {\n"
           << "      k-regex \"^(<>*)<KEY|8=KEY><>{1,3}$\" k-expand \"\\\\1\"\n"
           << "      h-relation is-prefix-of p-regex \"^(<>*)$\" p-expand \"\\\\1\"\n"
           << "    } } }\n"
           << "}\n"
           << "trust-anchor\n{\n"
           << "  type file\n"
           << "  file-name \"" << rootPath.string() << "\"\n"
           << "}\n";
  }

  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(ControllerVersion{1, 1});
  status.setValidity(1, 5000000000000ULL);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ctlCert.getName());

  const auto statusName = ServiceControllerTestAccess::statusQuery(
      ctlCert.getIdentity(), status.getServiceName(), status.getControllerVersion()).getName();
  const auto rogueStatusName = ServiceControllerTestAccess::statusQuery(
      rogueCert.getIdentity(), status.getServiceName(), status.getControllerVersion()).getName();

  ndn::Data anchoredData(statusName);
  anchoredData.setContent(status.wireEncode());
  ctlKeys.sign(anchoredData, ndn::security::signingByCertificate(ctlCert));

  ndn::Data rogueData(rogueStatusName);
  rogueData.setContent(status.wireEncode());
  rogueKeys.sign(rogueData,
                 ndn::security::signingByCertificate(rogueCert));

  ndn::Data brokenChainData(statusName);
  brokenChainData.setContent(status.wireEncode());
  ctlKeys.sign(brokenChainData,
               ndn::security::signingByCertificate(brokenCtlCert));

  // Certificate fetch relay: the configured trust schema resolves every
  // non-anchor signer through CertificateFetcherFromNetwork on the callback
  // face.  Serve the issued certificates under their exact names.
  const std::vector<std::pair<ndn::Name, ndn::Data>> fetchableCerts = {
      {caCert.getName(), caCert},
      {ctlCert.getName(), ctlCert},
      {rogueCert.getName(), rogueCert},
      {strangerSelfCert.getName(), strangerSelfCert},
      {brokenCaCert.getName(), brokenCaCert},
      {brokenCtlCert.getName(), brokenCtlCert},
  };

  ndn::DummyClientFace face;
  auto certRelay = face.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        for (const auto& entry : fetchableCerts) {
          if (interest.getName() == entry.first ||
              entry.first.isPrefixOf(interest.getName())) {
            face.receive(entry.second);
            return;
          }
        }
      });

  auto validator = std::make_shared<MessageValidator>(
      schemaPath.string(), std::nullopt, &face);
  size_t anchoredSuccess = 0;
  size_t rogueSuccess = 0;
  size_t brokenSuccess = 0;
  size_t failures = 0;
  validator->validateWithConfiguredTrustSchema(
      anchoredData,
      [&] (const ndn::Data&) { ++anchoredSuccess; },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) { ++failures; });
  validator->validateWithConfiguredTrustSchema(
      rogueData,
      [&] (const ndn::Data&) { ++rogueSuccess; },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) { ++failures; });
  validator->validateWithConfiguredTrustSchema(
      brokenChainData,
      [&] (const ndn::Data&) { ++brokenSuccess; },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) { ++failures; });
  for (size_t i = 0; i < 200 && (anchoredSuccess == 0 || failures < 2); ++i) {
    face.processEvents(ndn::time::milliseconds(5));
  }

  BOOST_CHECK_EQUAL(anchoredSuccess, 1U);
  BOOST_CHECK_EQUAL(rogueSuccess, 0U);
  BOOST_CHECK_EQUAL(brokenSuccess, 0U);
  BOOST_CHECK_EQUAL(failures, 2U);

  std::filesystem::remove(schemaPath);
  std::filesystem::remove(rootPath);
}

BOOST_AUTO_TEST_CASE(RealControllerStatusDrivesUserAndProviderRevocation)
{
  // Bridge the Controller mutation to the production User/Provider runtime
  // boundary.  This deliberately uses LocalMock only for transport; status
  // construction, version advancement, permission refresh, and enforcement
  // all come from the real ServiceController/User/Provider APIs.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-runtime-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain controllerKeys("pib-memory:spec179-controller-runtime",
                               "tpm-memory:spec179-controller-runtime");
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-runtime"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());

  ndn::Face runtimeFace;
  ndn::security::KeyChain runtimeKeys(
      "pib-memory:spec179-controller-runtime-users",
      "tpm-memory:spec179-controller-runtime-users");
  const ndn::Name userName("/example/hello/user");
  const ndn::Name providerName("/example/hello/provider");
  const ndn::Name aaName("/spec179/controller-runtime/aa");
  const ndn::Name serviceName("/HELLO");
  const auto userCert = runtimeKeys.createIdentity(
      userName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto providerCert = runtimeKeys.createIdentity(
      providerName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto aaCert = runtimeKeys.createIdentity(
      aaName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();

  ndn_service_framework::test::IsolatedTestTrust fixedTrust3{
    controllerCert, userCert, providerCert, aaCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/controller-runtime"), userCert, aaCert,
                   fixedTrust3.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust4{
    controllerCert, userCert, providerCert, aaCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/controller-runtime"), providerCert,
                           aaCert, fixedTrust4.schemaPath());
  size_t published = 0;
  size_t executions = 0;
  ndn::Name lastRequestName;
  RequestMessage lastRequest;
  user.setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&, const ndn::Name&,
           const RequestMessage& request, size_t) {
        ++published;
        lastRequestName = requestName;
        lastRequest = request;
      });
  provider.addService(
      serviceName,
      ServiceProvider::RequestHandler(
          [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
               const ndn::Name&, const RequestMessage&) {
            ++executions;
            ResponseMessage response;
            response.setStatus(true);
            return response;
          }));

  const auto installCurrentControllerStatus = [&] () {
    const auto version = serviceController.getControllerVersion();
    const auto status = serviceController.getPolicyStatus(serviceName);
    BOOST_REQUIRE(status.getControllerVersion() == version);
    user.applyPermissionResponse(runtimePermission(
        userName, tlv::UserPermission, providerName, serviceName, version));
    provider.applyPermissionResponse(runtimePermission(
        providerName, tlv::ProviderPermission, providerName, serviceName, version));
    BOOST_REQUIRE(user.installControllerStatus(status));
    BOOST_REQUIRE(provider.installControllerStatus(status));
    return version;
  };

  const auto coldStatus = serviceController.getPolicyStatus(serviceName);
  auto futureFloor = coldStatus.getControllerVersion();
  ++futureFloor.controllerEpoch;
  {
    const auto* configured = std::getenv("NDNSF_CONTROLLER_MIN_VERSION");
    const auto previous = configured ? std::optional<std::string>(configured) : std::nullopt;
    auto clear = ndn::make_scope_exit([&] {
      if (previous)
        ::setenv("NDNSF_CONTROLLER_MIN_VERSION", previous->c_str(), 1);
      else
        ::unsetenv("NDNSF_CONTROLLER_MIN_VERSION");
    });
    for (const auto& floor : {futureFloor.toString(), std::string("1:overflow")}) {
      ::setenv("NDNSF_CONTROLLER_MIN_VERSION", floor.c_str(), 1);
      BOOST_CHECK(!user.installControllerStatus(coldStatus));
      BOOST_CHECK(!provider.installControllerStatus(coldStatus));
      BOOST_CHECK(!user.getControllerVersion());
      BOOST_CHECK(!provider.getControllerVersion());
    }
    ::setenv("NDNSF_CONTROLLER_MIN_VERSION", coldStatus.getControllerVersion().toString().c_str(), 1);
    BOOST_CHECK(installCurrentControllerStatus() == coldStatus.getControllerVersion());
  }
  const auto initialVersion = coldStatus.getControllerVersion();
  BOOST_REQUIRE(initialVersion.isValid());

  // Runtime status installation must fail closed when the authenticated
  // Controller signature check fails; a newer version hint must not replace
  // the last accepted authority on either role.
  const ControllerVersion forgedVersion{
      initialVersion.controllerGenerationTimestamp, initialVersion.controllerEpoch + 1};
  const auto forgedStatus = liveStatusFor(forgedVersion);
  BOOST_CHECK(!user.installControllerStatus(forgedStatus, false));
  BOOST_CHECK(!provider.installControllerStatus(forgedStatus, false));
  BOOST_REQUIRE(user.getControllerVersion());
  BOOST_REQUIRE(provider.getControllerVersion());
  BOOST_CHECK(*user.getControllerVersion() == initialVersion);
  BOOST_CHECK(*provider.getControllerVersion() == initialVersion);

  const auto invoke = [&] (const std::string& token) {
    RequestMessage request;
    request.setUserToken(token);
    const std::string payload = "controller-generated-status-input";
    ndn::Buffer bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    request.setPayload(bytes, bytes.size());
    const auto requestId = user.RequestService(
        {providerName}, serviceName, request, 1000,
        [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
        tlv::FirstResponding);
    BOOST_REQUIRE(!requestId.empty());
    BOOST_REQUIRE(!lastRequestName.empty());
    return provider.handleDecryptedRequestByName(lastRequestName, lastRequest);
  };

  BOOST_CHECK(invoke("controller-runtime-before-provider-revoke").getStatus());
  BOOST_CHECK_EQUAL(executions, 1);

  RevocationTarget providerService;
  providerService.kind = RevocationKind::SERVICE_AUTHORIZATION;
  providerService.targetIdentity = providerName;
  providerService.serviceName = serviceName;
  providerService.authorizationAttribute = ndn::Name("/SERVICE").append(serviceName);
  BOOST_REQUIRE(serviceController.revoke(providerService));
  const auto providerRevokedVersion = installCurrentControllerStatus();
  BOOST_CHECK(providerRevokedVersion.compare(initialVersion) > 0);
  const auto providerDenied = invoke("controller-runtime-after-provider-revoke");
  BOOST_CHECK(!providerDenied.getStatus());
  BOOST_CHECK_EQUAL(executions, 1);
  BOOST_CHECK_EQUAL(published, 2);

  RevocationTarget revokedUser = makeIdentityRevocation(userName.toUri().c_str());
  BOOST_REQUIRE(serviceController.revoke(revokedUser));
  const auto userRevokedVersion = installCurrentControllerStatus();
  BOOST_CHECK(userRevokedVersion.compare(providerRevokedVersion) > 0);
  const auto beforeUserRevoke = published;
  const auto deniedRequest = user.RequestService(
      {providerName}, serviceName, lastRequest, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_CHECK(deniedRequest.empty());
  BOOST_CHECK_EQUAL(published, beforeUserRevoke);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_CONTROLLER_RUNTIME controller_status=accepted"
      " provider_revoke=execution_denied user_revoke=publication_denied");

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(LiveControllerStatusRefreshRejectsRevokedRenewal)
{
  // Exercise the actual Controller -> User/Provider Interest/Data path.  The
  // LocalMock runtimes still use a DummyClientFace, but permissions and
  // version-addressable PolicyStatus Data are produced, signed, encrypted,
  // fetched, validated, and installed by the production handlers.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-live-refresh-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  // ServiceController owns its AttributeAuthority KeyChain internally.  Use
  // the same default PIB/TPM here so the supplied controller certificate is
  // actually available to the AA's signing path (isolated KeyChains would
  // leave the certificate name without a private key).
  ndn::KeyChain controllerKeys;
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-live-refresh"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto controllerPrefix = controllerCert.getIdentity();
  // ServiceController owns the signing KeyChain used by its NAC-ABE
  // AttributeAuthority.  Use that internally provisioned certificate for the
  // LocalMock runtimes; the externally created certificate is only the
  // controller identity input and is not present in the controller's
  // internal PIB.
  const auto runtimeAaCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);

  const ndn::Name userName("/example/hello/user");
  const ndn::Name providerName("/example/hello/provider");
  const ndn::Name serviceName("/HELLO");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, userName);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, providerName);
  // The runtime Provider consumes NAC-ABE DKEYs with its RSA encryption
  // certificate while the Controller's default key is now EC signing. Re-run
  // policy registration to prove the Attribute Authority caches both certs.
  const auto providerSigningCert =
      ServiceControllerTestAccess::addAlternateSigningCertificate(
          serviceController, providerName);
  BOOST_REQUIRE(providerSigningCert.getKeyName() != providerCert.getKeyName());
  ServiceControllerTestAccess::refreshAbeIdentityPolicies(serviceController);

  ndn::KeyChain runtimeKeys;
  ndn::DummyClientFace runtimeFace(runtimeKeys);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust5{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/live-refresh"), userCert,
                   runtimeAaCert, fixedTrust5.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust6{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/live-refresh"), providerCert,
                           runtimeAaCert, fixedTrust6.schemaPath());
  // Install the fixture-owned NAC Consumer/Producer pair for the runtime
  // keychain, matching the production bootstrap.  The LocalMock member
  // consumers would otherwise defer every post-install DKEY re-arm and the
  // pump-driven DKEY segments could never be validated.
  user.useSigningKeyChainForTest(runtimeKeys);
  provider.useSigningKeyChainForTest(runtimeKeys);
  user.setUseTokens(false);
  provider.setUseTokens(false);

  // Forward the runtime's Interests into the Controller and return the
  // Controller's signed Data to the runtime.  This is the same packet-level
  // boundary as the production Face, without requiring a live NFD process.
  auto interestRelay = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        controllerFace.receive(interest);
      });
  auto dataRelay = controllerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        runtimeFace.receive(data);
      });
  // Positive-timeout processEvents() is terminated by an io_context stop
  // scheduled from inside the run loop.  ndn-cxx cancels that stop timer
  // once the face has no pending Interests and no registered prefix, so if
  // any component keeps an unrelated long timer alive on this io_context
  // (the Controller's authority refresh does) the call can block far beyond
  // its requested timeout.  Pump with a negative timeout (poll semantics,
  // non-blocking, drain ready events) in a bounded wall-clock loop instead;
  // this mirrors pumpFaceFor() in ServiceUser.cpp.
  const auto pump = [&] {
    const auto pumpDeadline = ndn::time::steady_clock::now() +
                              ndn::time::milliseconds(800);
    do {
      runtimeFace.processEvents(ndn::time::milliseconds(-1));
      controllerFace.processEvents(ndn::time::milliseconds(-1));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (ndn::time::steady_clock::now() < pumpDeadline);
  };

  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
  // Complete DummyClientFace prefix registration before sending the first
  // runtime Interest.  The combined setInterestFilter overload installs its
  // callback only after the registration reply has been processed.
  pump();

  user.fetchPermissionsFromController(controllerPrefix);
  provider.fetchPermissionsFromController(controllerPrefix);
  pump();

  const auto initialVersion = serviceController.getControllerVersion();
  const auto initialStatus = serviceController.getPolicyStatus(serviceName);
  BOOST_REQUIRE(initialVersion.isValid());
  BOOST_REQUIRE(initialStatus.getControllerVersion() == initialVersion);
  const auto userVersion = user.getControllerVersion();
  const auto providerVersion = provider.getControllerVersion();
  BOOST_REQUIRE(userVersion);
  BOOST_REQUIRE(providerVersion);
  BOOST_CHECK(*userVersion == initialVersion);
  BOOST_CHECK(*providerVersion == initialVersion);

  size_t published = 0;
  bool defaultRequestScoped = false;
  bool defaultDiscoveryPayloadEmpty = false;
  user.setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
           const ndn::Name&, const RequestMessage& publishedRequest, size_t) {
        ++published;
        defaultRequestScoped =
            publishedRequest.hasRequestCapabilities() &&
            publishedRequest.getRequestCapabilities().hasField(
                "RequestScopedConfidentialityV1") &&
            publishedRequest.getRequestCapabilities().getField(
                "RequestScopedConfidentialityV1") == "required";
        defaultDiscoveryPayloadEmpty = publishedRequest.getPayload().empty();
      });
  provider.addService(
      serviceName,
      ServiceProvider::RequestHandler(
          [] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
              const ndn::Name&, const RequestMessage&) {
            ResponseMessage response;
            response.setStatus(true);
            return response;
          }));

  RequestMessage request;
  const auto firstRequest = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_REQUIRE(!firstRequest.empty());
  BOOST_CHECK_EQUAL(published, 1);
  BOOST_CHECK(defaultRequestScoped);
  BOOST_CHECK(defaultDiscoveryPayloadEmpty);

  RevocationTarget revokedUser = makeIdentityRevocation(userName.toUri().c_str());
  BOOST_REQUIRE(serviceController.revoke(revokedUser));
  const auto revokedVersion = serviceController.getControllerVersion();
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  const auto refreshedUserVersion = user.getControllerVersion();
  BOOST_REQUIRE(refreshedUserVersion);
  BOOST_CHECK(*refreshedUserVersion == revokedVersion);
  // Replaying a previously valid snapshot must not roll the runtime back
  // after the Controller has advanced its authority.  This is the live
  // User-side counterpart to exact-name stale status refusal at the
  // Controller boundary.
  BOOST_CHECK(!user.installControllerStatus(initialStatus));
  BOOST_REQUIRE(user.getControllerVersion());
  BOOST_CHECK(*user.getControllerVersion() == revokedVersion);
  // The revoked renewal is intentionally an empty permission snapshot.  The
  // runtime must still retain the previous service scope long enough to fetch
  // and install the Controller status that explains why the snapshot is empty.
  BOOST_CHECK(user.getAllowedServices().empty());
  const auto deniedRequest = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_CHECK(deniedRequest.empty());
  BOOST_CHECK_EQUAL(published, 1);

  RevocationTarget revokedProvider;
  revokedProvider.kind = RevocationKind::SERVICE_AUTHORIZATION;
  revokedProvider.targetIdentity = providerName;
  revokedProvider.serviceName = serviceName;
  revokedProvider.authorizationAttribute = ndn::Name("/SERVICE").append(serviceName);
  BOOST_REQUIRE(serviceController.revoke(revokedProvider));
  const auto providerRevokedVersion = serviceController.getControllerVersion();
  provider.fetchPermissionsFromController(controllerPrefix);
  pump();
  const auto refreshedProviderVersion = provider.getControllerVersion();
  BOOST_REQUIRE(refreshedProviderVersion);
  BOOST_CHECK(*refreshedProviderVersion == providerRevokedVersion);

  RequestMessage providerRequest;
  providerRequest.setPolicyEpoch(providerRevokedVersion.controllerEpoch);
  providerRequest.setControllerVersion(providerRevokedVersion);
  const auto providerResponse = provider.handleDecryptedRequestByName(
      makeRequestNameV2(userName, serviceName, ndn::Name("live-refresh-request")),
      providerRequest);
  BOOST_CHECK(!providerResponse.getStatus());
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_LIVE_REFRESH permission_fetch=accepted"
      " user_revoke=renewal_denied provider_revoke=execution_denied");

  interestRelay.disconnect();
  dataRelay.disconnect();
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut)
{
  // RV-U21 cross-process grant-only evidence: the granted identity performs
  // exactly one DKEY-only refresh fetch after a grant-only ControllerVersion
  // advance, unaffected identities are never refetched, and a repeated
  // permission/status cycle is idempotent.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-grant-fetch-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain controllerKeys;
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-grant-fetch"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  // Register the seeded Provider certificate before the AA loads its policy.
  controllerKeys.createIdentity(ndn::Name("/example/hello/provider"), ndn::RsaKeyParams(2048));
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto runtimeAaCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);

  const ndn::Name grantedUser("/example/hello/user-grant-fetch");
  const ndn::Name unaffectedProvider("/example/hello/provider");
  const ndn::Name seededService("/HELLO");
  // The grant-only addition must be a service that hello.policies actually
  // routes to providers; a grant for an unrouted service produces no
  // permission-table entry and cannot drive the refresh under test.
  const ndn::Name grantedService("/NDNSF/DistributedRepo/Store");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, grantedUser);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, unaffectedProvider);
  const auto userEncryptionCert =
      ServiceControllerTestAccess::rsaEncryptionCertificate(serviceController, userCert);
  const auto providerEncryptionCert =
      ServiceControllerTestAccess::rsaEncryptionCertificate(serviceController, providerCert);

  ndn::KeyChain runtimeKeys;
  ndn::DummyClientFace runtimeFace(runtimeKeys);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust7{
    controllerCert, runtimeAaCert, userCert, providerCert, userEncryptionCert, providerEncryptionCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/grant-fetch"), userEncryptionCert, userCert,
                   runtimeAaCert, fixedTrust7.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust8{
    controllerCert, runtimeAaCert, userCert, providerCert, userEncryptionCert, providerEncryptionCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/grant-fetch"), providerEncryptionCert, providerCert,
                           runtimeAaCert, fixedTrust8.schemaPath());
  user.setUseTokens(false);
  provider.setUseTokens(false);

  size_t boundUserQueries = 0;
  size_t boundProviderQueries = 0;
  auto interestRelay = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        for (const bool isUser : {true, false}) {
          const auto operation = isUser ? "USER-PERMISSIONS" : "PROVIDER-PERMISSIONS";
          if (!parseControlRpcName(interest, controllerPrefix, operation))
            continue;
          const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
          BOOST_REQUIRE(query);
          BOOST_CHECK(query->recipientCertificate ==
                      (isUser ? userEncryptionCert.getFullName() :
                                providerEncryptionCert.getFullName()));
          ++(isUser ? boundUserQueries : boundProviderQueries);
        }
        controllerFace.receive(interest);
      });
  auto dataRelay = controllerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        runtimeFace.receive(data);
      });
  // Poll-based bounded pump: a positive processEvents() timeout is stopped
  // by an io_context stop scheduled from inside its run loop, which ndn-cxx
  // cancels once the face has no pending Interests or registered prefix.
  // If any component keeps a long timer alive on the io_context the call can
  // block far beyond the requested timeout, so drain ready events instead.
  const auto pump = [&] {
    const auto pumpDeadline = ndn::time::steady_clock::now() +
                              ndn::time::milliseconds(800);
    do {
      runtimeFace.processEvents(ndn::time::milliseconds(-1));
      controllerFace.processEvents(ndn::time::milliseconds(-1));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (ndn::time::steady_clock::now() < pumpDeadline);
  };

  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
  pump();

  // Seed the granted identity so both runtimes can install their initial
  // NAC-ABE material before refresh fetches are counted.
  const auto seedVersion = serviceController.getControllerVersion();
  BOOST_REQUIRE(serviceController.grant(
      grantedUser, seededService,
      ndn::Name("/PERMISSION").append(seededService)));
  const auto seedParamsName =
      ServiceControllerTestAccess::abePublicParametersName(serviceController);
  const auto seedParamsDigest =
      ServiceControllerTestAccess::abePublicParametersDigest(serviceController);

  user.useSigningKeyChainForTest(runtimeKeys);
  provider.useSigningKeyChainForTest(runtimeKeys);
  pump();
  BOOST_CHECK(user.isNacConsumerReadyForTest());
  BOOST_CHECK(provider.isNacConsumerReadyForTest());

  // Establish the granted identity's permission table and /HELLO status
  // before refresh fetches are counted.  The initial permission/status
  // install performs a generation-change DKEY fetch that must not be
  // attributed to the later grant-only advance.
  user.fetchPermissionsFromController(controllerPrefix);
  provider.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK_GT(boundUserQueries, 0U);
  BOOST_CHECK_GT(boundProviderQueries, 0U);
  BOOST_CHECK(user.isNacConsumerReadyForTest());

  size_t grantedUserDkeyFetches = 0;
  size_t providerDkeyFetches = 0;
  const auto userDkeyPrefix = ServiceControllerTestAccess::dkeyInterestPrefix(controllerPrefix, userCert);
  const auto providerDkeyPrefix = ServiceControllerTestAccess::dkeyInterestPrefix(controllerPrefix, providerCert);
  auto dkeyCounter = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (userDkeyPrefix.isPrefixOf(interest.getName())) ++grantedUserDkeyFetches;
        if (providerDkeyPrefix.isPrefixOf(interest.getName())) ++providerDkeyFetches;
      });

  // The grant-only change advances ControllerVersion while the global ABE
  // pair stays byte-identical.
  BOOST_REQUIRE(serviceController.grant(
      grantedUser, grantedService,
      ndn::Name("/PERMISSION").append(grantedService)));
  BOOST_CHECK(serviceController.getControllerVersion().compare(seedVersion) > 0);
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              seedParamsName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    seedParamsDigest);

  user.fetchPermissionsFromController(controllerPrefix);
  pump();

  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 1U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);
  BOOST_CHECK(user.isNacConsumerReadyForTest());
  BOOST_CHECK(provider.isNacConsumerReadyForTest());

  // Idempotence: an already current-generation DKEY is never refetched by a
  // repeated permission/status cycle.
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 1U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);

  // A newly authorized Provider adds a route for an already held User
  // service attribute. It must not trigger another User DKEY refresh.
  BOOST_REQUIRE(serviceController.grant(
      ndn::Name("/example/hello/provider/additional-grant-fetch"), seededService,
      ndn::Name("/SERVICE").append(seededService)));
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 1U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);

  interestRelay.disconnect();
  dataRelay.disconnect();
  dkeyCounter.disconnect();
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(GrantOnlyRefreshSurvivesReverseOrderStatusFirstInstall)
{
  // RV-I34 (Finding A, 2026-09-05): when the status channel installs a
  // grant-only ControllerVersion advance BEFORE the PermissionResponse that
  // records the grant, the later equal-version install which consumes the
  // pending grant-only refresh must still drive the single target-only DKEY
  // fetch.  Pre-fix the refresh was gated behind the version-change
  // invalidate (ServiceUser.cpp installControllerStatus), so the pending
  // entry was consumed and its wave marker set while no fetch was issued;
  // the following version-change install of the newly granted service was
  // then suppressed by the same wave marker: the whole grant wave refreshed
  // nothing until the next real version advance.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-reverse-order-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain controllerKeys;
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-reverse-order"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  // Register the seeded Provider certificate before the AA loads its policy.
  controllerKeys.createIdentity(ndn::Name("/example/hello/provider"), ndn::RsaKeyParams(2048));
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto runtimeAaCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);

  const ndn::Name grantedUser("/example/hello/user-grant-fetch");
  const ndn::Name unaffectedProvider("/example/hello/provider");
  const ndn::Name seededService("/HELLO");
  const ndn::Name grantedService("/NDNSF/DistributedRepo/Store");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, grantedUser);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, unaffectedProvider);

  ndn::KeyChain runtimeKeys;
  ndn::DummyClientFace runtimeFace(runtimeKeys);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust9{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/reverse-order"), userCert,
                   runtimeAaCert, fixedTrust9.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust10{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/reverse-order"), providerCert,
                           runtimeAaCert, fixedTrust10.schemaPath());
  user.setUseTokens(false);
  provider.setUseTokens(false);

  auto interestRelay = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        controllerFace.receive(interest);
      });
  auto dataRelay = controllerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        runtimeFace.receive(data);
      });
  const auto pump = [&] {
    const auto pumpDeadline = ndn::time::steady_clock::now() +
                              ndn::time::milliseconds(800);
    do {
      runtimeFace.processEvents(ndn::time::milliseconds(-1));
      controllerFace.processEvents(ndn::time::milliseconds(-1));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (ndn::time::steady_clock::now() < pumpDeadline);
  };

  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
  pump();

  // Seed the granted identity (permission + status at the seed version)
  // before refresh fetches are counted.
  const auto seedVersion = serviceController.getControllerVersion();
  BOOST_REQUIRE(serviceController.grant(
      grantedUser, seededService,
      ndn::Name("/PERMISSION").append(seededService)));
  const auto seedParamsName =
      ServiceControllerTestAccess::abePublicParametersName(serviceController);
  const auto seedParamsDigest =
      ServiceControllerTestAccess::abePublicParametersDigest(serviceController);

  user.useSigningKeyChainForTest(runtimeKeys);
  provider.useSigningKeyChainForTest(runtimeKeys);
  pump();
  BOOST_CHECK(user.isNacConsumerReadyForTest());
  BOOST_CHECK(provider.isNacConsumerReadyForTest());
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK(user.isNacConsumerReadyForTest());

  size_t grantedUserDkeyFetches = 0;
  size_t providerDkeyFetches = 0;
  const auto userDkeyPrefix = ServiceControllerTestAccess::dkeyInterestPrefix(controllerPrefix, userCert);
  const auto providerDkeyPrefix = ServiceControllerTestAccess::dkeyInterestPrefix(controllerPrefix, providerCert);
  auto dkeyCounter = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (userDkeyPrefix.isPrefixOf(interest.getName())) ++grantedUserDkeyFetches;
        if (providerDkeyPrefix.isPrefixOf(interest.getName())) ++providerDkeyFetches;
      });

  // The grant-only change advances ControllerVersion while the global ABE
  // pair stays byte-identical.
  BOOST_REQUIRE(serviceController.grant(
      grantedUser, grantedService,
      ndn::Name("/PERMISSION").append(grantedService)));
  BOOST_CHECK(serviceController.getControllerVersion().compare(seedVersion) > 0);
  BOOST_CHECK(ServiceControllerTestAccess::abePublicParametersName(serviceController) ==
              seedParamsName);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    seedParamsDigest);

  // REVERSE ORDER, leg 1 — the status channel installs the advanced version
  // before any permission refetch (as a scheduled refresh would): the seeded
  // /HELLO status advances v1 -> v2 with an empty pending set, so no refresh
  // is expected (and the wave marker must stay unset).
  BOOST_REQUIRE(user.installControllerStatus(
      serviceController.getPolicyStatus(seededService), true));
  pump();
  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 0U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);

  // REVERSE ORDER, leg 2 — the permission response arrives late.  The
  // whole-table diff marks every service pending, and the tail refetch of the
  // already-installed /HELLO status is an equal-version install: it consumes
  // the pending entry and must be the one driving the single target-only
  // DKEY refresh.  (If the newly granted service's version-change install
  // were to arrive first it would drive the same single fetch; the fix makes
  // either order issue exactly one.)
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 1U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);
  BOOST_CHECK(user.isNacConsumerReadyForTest());
  BOOST_CHECK(provider.isNacConsumerReadyForTest());

  // Idempotence: an already current-generation DKEY is never refetched by a
  // repeated permission/status cycle.
  user.fetchPermissionsFromController(controllerPrefix);
  pump();
  BOOST_CHECK_EQUAL(grantedUserDkeyFetches, 1U);
  BOOST_CHECK_EQUAL(providerDkeyFetches, 0U);

  interestRelay.disconnect();
  dataRelay.disconnect();
  dkeyCounter.disconnect();
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ControllerVersionFloorRejectsAnAuthenticPreRevocationBackup)
{
  const auto root = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-version-floor-" + std::to_string(::getpid()));
  BOOST_REQUIRE(std::filesystem::create_directory(root));
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  const auto state = root / "authority.state";
  const auto backup = root / "backup.state";
  const auto current = root / "current.state";
  auto cleanup = ndn::make_scope_exit([&] {
    ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
    ::unsetenv("NDNSF_CONTROLLER_MIN_VERSION");
    std::filesystem::remove_all(root);
  });
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", state.c_str(), 1);
  ::unsetenv("NDNSF_CONTROLLER_MIN_VERSION");
  ndn::KeyChain keys;
  const auto identity = keys.createIdentity(ndn::Name("/controller/spec243-floor"),
                                           ndn::RsaKeyParams(2048));
  const auto cert = identity.getDefaultKey().getDefaultCertificate();
  const auto policy = ndnsf::test::testFixturePath("controller-policies.conf").string();
  const auto target = makeServiceRevocation("/spec243/version-floor/revoked");
  ControllerVersion floor;
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController authority(face, cert, validator, policy);
    BOOST_REQUIRE(authority.getControllerVersion().isValid());
    std::filesystem::copy_file(state, backup);
    BOOST_REQUIRE(authority.revoke(target));
    floor = authority.getControllerVersion();
    BOOST_CHECK(authority.isRevoked(target.targetIdentity, ndn::Name(SERVICE)));
  }
  std::filesystem::copy_file(state, current);
  std::filesystem::copy_file(backup, state, std::filesystem::copy_options::overwrite_existing);
  ::setenv("NDNSF_CONTROLLER_MIN_VERSION", floor.toString().c_str(), 1);
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController refused(face, cert, validator, policy);
    BOOST_CHECK(!refused.getControllerVersion().isValid());
    BOOST_CHECK_THROW(refused.getPolicyStatus(ndn::Name(SERVICE)), std::runtime_error);
    ServiceControllerTestAccess::policyStatusInterest(refused,
        ServiceControllerTestAccess::statusQuery(identity.getName(), ndn::Name(SERVICE), floor));
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_CHECK(face.sentData.empty());
  }
  const auto bytes = [] (const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  BOOST_CHECK(bytes(state) == bytes(backup));
  std::filesystem::copy_file(current, state, std::filesystem::copy_options::overwrite_existing);
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController recovered(face, cert, validator, policy);
    BOOST_CHECK(recovered.getControllerVersion().compare(floor) > 0);
    BOOST_CHECK(recovered.isRevoked(target.targetIdentity, ndn::Name(SERVICE)));
  }
}

BOOST_AUTO_TEST_CASE(ControllerSignerHandoffRequiresCurrentPredecessorAndExactSuccessor)
{
  const auto root = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-handoff-" + std::to_string(::getpid()));
  BOOST_REQUIRE(std::filesystem::create_directory(root));
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  const char* configuredCertificate = std::getenv("NDNSF_CONTROLLER_CERT_FILE");
  const auto previousCertificate = configuredCertificate ?
    std::optional<std::string>(configuredCertificate) : std::nullopt;
  auto cleanup = ndn::make_scope_exit([&] {
    ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
    ::unsetenv("NDNSF_CONTROLLER_HANDOFF");
    if (previousCertificate)
      ::setenv("NDNSF_CONTROLLER_CERT_FILE", previousCertificate->c_str(), 1);
    else
      ::unsetenv("NDNSF_CONTROLLER_CERT_FILE");
    std::filesystem::remove_all(root);
  });
  const auto state = root / "authority.state";
  const auto proof = root / "handoff.ndn";
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", state.c_str(), 1);
  ::unsetenv("NDNSF_CONTROLLER_HANDOFF");

  ndn::KeyChain keys;
  const auto identity = keys.createIdentity(ndn::Name("/controller/spec243-handoff"),
                                             ndn::RsaKeyParams(2048));
  const auto predecessorKey = identity.getDefaultKey();
  const auto predecessor = predecessorKey.getDefaultCertificate();
  const auto successorKey = keys.createKey(identity, ndn::RsaKeyParams(2048));
  const auto successor = successorKey.getDefaultCertificate();
  const auto policy = ndnsf::test::testFixturePath("controller-policies.conf").string();
  const auto revoked = makeServiceRevocation("/spec179/handoff/revoked");
  ControllerVersion original;
  ControllerVersion originalMaterial;
  ndn::Name originalParameters;
  std::string originalGrant;
  std::optional<ndn::Data> archivedProofBeforeRestart;
  ndn::Data witnessedPublication, unwitnessedPublication, unwitnessedReceipt;
  const ndn::Name reader("/example/hello/user");
  keys.setDefaultKey(identity, predecessorKey);
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, predecessor, validator, policy);
    BOOST_REQUIRE(controller.revoke(revoked));
    original = controller.getControllerVersion();
    originalMaterial = ServiceControllerTestAccess::abeMaterialVersion(controller);
    originalParameters = controller.getPolicyStatus(ndn::Name(SERVICE)).getAbePublicParametersName();
    ServiceControllerTestAccess::ensureInternalIdentity(controller, reader);
    ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
    const auto parameters = encodeControlQueryParameters({reader, ndn::Name(), 0, 0});
    ndn::Interest query(makeControlRpcName(identity.getName(), "USER-PERMISSIONS",
                                          parameters, makeControlQueryNonce()));
    query.setApplicationParameters(parameters);
    ServiceControllerTestAccess::userPermissionsInterest(controller, query);
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    const auto record = ServiceControllerTestAccess::loadGrantRecord(controller, reader, original);
    BOOST_REQUIRE(record);
    originalGrant = *record;
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalGrantWire(controller, originalGrant));
    const ndn::Name service("/HELLO");
    const auto status = controller.getPolicyStatus(service);
    const auto producer = keys.createIdentity(ndn::Name("/example/hello/provider"),
                                               ndn::RsaKeyParams()).getDefaultKey()
                          .getDefaultCertificate();
    auto digest = status.getAbePublicParametersDigest();
    std::transform(digest.begin(), digest.end(), digest.begin(), [] (unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    const auto makeReceipt = [&] (const std::string& id) {
      ndn::Data ck(makeHybridMessageKeyDataName(service, producer.getIdentity(),
                                                "/PERMISSION/HELLO", id));
      ck.setContent("test CK");
      const auto signing = ndn::security::signingByCertificate(producer);
      keys.sign(ck, signing);
      auto publication = makeHistoricalPublicationData({
        identity.getName(), producer.getIdentity(), service,
        ndn::Name(producer.getIdentity()).append("OBJECT").append(id), ck.getFullName(),
        status.getAbePublicParametersName(), originalMaterial.controllerGenerationTimestamp,
        originalMaterial.controllerEpoch, original.controllerGenerationTimestamp,
        original.controllerEpoch, 1, "/PERMISSION/HELLO", "sha256:" + std::string(64, 'a'),
        digest}, keys, signing);
      auto receipt = makeHistoricalPublicationReceipt(publication, producer, status, predecessor,
                                                       keys, status.getValidFromMs());
      return std::pair{std::move(publication), std::move(receipt)};
    };
    auto [publication, receipt] = makeReceipt("recorded");
    witnessedPublication = std::move(publication);
    ServiceControllerTestAccess::storeHistoricalReceipt(controller, witnessedPublication, receipt);
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalPublication(controller,
                                                                          witnessedPublication));
    std::tie(unwitnessedPublication, unwitnessedReceipt) = makeReceipt("unrecorded");
  }
  std::string legacyGrant;
  std::vector<std::string> rejectedGrants;
  {
    ControllerGenerationStore store(state);
    BOOST_REQUIRE(store.acquireWriter(identity.getName().toUri(ndn::name::UriFormat::CANONICAL)));
    const auto add = [&] (const ndn::Name& subject, ControllerVersion version,
                          const ndn::Name& name, const ndn::Name& slot,
                          std::string suffix = {}) {
      ndn::Data data(name);
      data.setContent(makeHistoricalGrantRecordHeader(subject,
        version.controllerGenerationTimestamp, version.controllerEpoch) +
        "granted /HELLO\n" + suffix);
      keys.sign(data, ndn::security::signingByCertificate(predecessor));
      const auto& wire = data.wireEncode();
      std::string bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
      store.recordGrant(slot.toUri(ndn::name::UriFormat::CANONICAL),
                        version.controllerGenerationTimestamp, version.controllerEpoch, bytes);
      return bytes;
    };
    const ndn::Name legacyReader("/history/legacy");
    legacyGrant = add(legacyReader, original, {}, legacyReader);
    const ndn::Name futureReader("/history/future");
    auto futureVersion = original;
    ++futureVersion.controllerEpoch;
    rejectedGrants.push_back(add(futureReader, futureVersion, {}, futureReader));
    rejectedGrants.push_back(add(ndn::Name("/history/moved"), original, {},
                                 ndn::Name("/history/wrong-slot")));
    const ndn::Name badPurpose("/history/bad-purpose");
    rejectedGrants.push_back(add(badPurpose, original, ndn::Name("/other/packet"), badPurpose));
    const ndn::Name malformed("/history/malformed");
    rejectedGrants.push_back(add(malformed, original, {}, malformed, "unexpected data\n"));
  }
  const auto stateBytes = [&] {
    std::ifstream in(state, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  const auto originalBytes = stateBytes();
  BOOST_REQUIRE(!originalBytes.empty());
  const auto storeProof = [&] (const ndn::Data& data) {
    const auto& wire = data.wireEncode();
    std::ofstream output(proof, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(wire.data()), wire.size());
    output.close();
    BOOST_REQUIRE(output);
    ::setenv("NDNSF_CONTROLLER_HANDOFF", proof.c_str(), 1);
  };
  const auto verifyRefusal = [&] (const ndn::security::Certificate& cert) {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController refused(face, cert, validator, policy);
    BOOST_CHECK(!refused.getControllerVersion().isValid());
    BOOST_CHECK_THROW(refused.getPolicyStatus(ndn::Name(SERVICE)), std::runtime_error);
  };

  keys.setDefaultKey(identity, successorKey);
  verifyRefusal(successor);
  BOOST_CHECK(stateBytes() == originalBytes);

  const auto handoff = makeControllerSignerHandoff(predecessor, successor, originalMaterial, keys);
  auto forged = handoff;
  keys.sign(forged, ndn::security::signingByCertificate(successor));
  storeProof(forged);
  verifyRefusal(successor);
  BOOST_CHECK(stateBytes() == originalBytes);

  auto stale = originalMaterial;
  ++stale.controllerEpoch;
  storeProof(makeControllerSignerHandoff(predecessor, successor, stale, keys));
  verifyRefusal(successor);
  BOOST_CHECK(stateBytes() == originalBytes);

  storeProof(handoff);
  std::string laterRecord;
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, successor, validator, policy);
    const auto status = controller.getPolicyStatus(ndn::Name(SERVICE));
    BOOST_CHECK(status.getControllerVersion().compare(original) > 0);
    BOOST_CHECK_EQUAL(status.getControllerCertificate(), successor.getName());
    BOOST_CHECK(status.getAbePublicParametersName() != originalParameters);
    BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1U);
    BOOST_CHECK_EQUAL(status.getRevocations().front().targetIdentity, revoked.targetIdentity);
    ndn::Data laterGrant(ndn::Name(identity.getName()).append("HISTORICAL-GRANT"));
    laterGrant.setContent(makeHistoricalGrantRecordHeader(reader,
      original.controllerGenerationTimestamp, original.controllerEpoch) + "granted /HELLO\n");
    keys.sign(laterGrant, ndn::security::signingByCertificate(predecessor));
    BOOST_CHECK(ndn::security::verifySignature(laterGrant, predecessor.getPublicKey()));
    const auto& laterWire = laterGrant.wireEncode();
    laterRecord.assign(reinterpret_cast<const char*>(laterWire.data()), laterWire.size());
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalGrantWire(controller, originalGrant));
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalGrantWire(controller, legacyGrant));
    for (const auto& wire : rejectedGrants)
      BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalGrantWire(controller, wire));
    BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalGrantWire(controller, laterRecord));
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalPublication(controller,
                                                                          witnessedPublication));
    ServiceControllerTestAccess::storeHistoricalReceipt(controller, unwitnessedPublication,
                                                        unwitnessedReceipt);
    BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalPublication(controller,
                                                                           unwitnessedPublication));

    const auto pin = root / "current-controller.cert";
    ndn::io::save(successor, pin.string());
    ::setenv("NDNSF_CONTROLLER_CERT_FILE", pin.c_str(), 1);
    const auto version = status.getControllerVersion();
    const auto alternate = ServiceControllerTestAccess::addAlternateSigningCertificate(
      controller, identity.getName());
    BOOST_REQUIRE(alternate.getKeyName() != successor.getKeyName());
    ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
    const auto statusQuery = ServiceControllerTestAccess::statusQuery(
      identity.getName(), ndn::Name(SERVICE), version);
    face.sentData.clear();
    ServiceControllerTestAccess::policyStatusInterest(controller, statusQuery);
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    const auto current = face.sentData.front();
    BOOST_CHECK_EQUAL(current.getName(), statusQuery.getName());
    BOOST_CHECK(!ndn::security::verifySignature(current, alternate.getPublicKey()));
    BOOST_CHECK(matchesConfiguredControllerSigner(current, identity.getName(),
                                                   &status.getControllerCertificate()));
    BOOST_CHECK(!matchesConfiguredControllerSigner(current, identity.getName(),
                                                    &predecessor.getName()));

    ServiceControllerTestAccess::ensureInternalIdentity(controller, reader);
    const auto parameters = encodeControlQueryParameters({reader, ndn::Name(), 0, 0});
    ndn::Interest permission(makeControlRpcName(identity.getName(), "USER-PERMISSIONS",
                                               parameters, makeControlQueryNonce()));
    permission.setApplicationParameters(parameters);
    face.sentData.clear();
    ServiceControllerTestAccess::userPermissionsInterest(controller, permission);
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    BOOST_CHECK_EQUAL(face.sentData.front().getName(), permission.getName());
    BOOST_CHECK(matchesConfiguredControllerSigner(face.sentData.front(), identity.getName(),
                                                  &successor.getName()));

    auto claimedFuture = status;
    claimedFuture.setControllerVersion({version.controllerGenerationTimestamp + 1, 1});
    ndn::Data retired(statusQuery.getName());
    retired.setContent(claimedFuture.wireEncode());
    keys.sign(retired, ndn::security::signingByCertificate(predecessor));
    BOOST_CHECK(ndn::security::verifySignature(retired, predecessor.getPublicKey()));
    BOOST_CHECK(!matchesConfiguredControllerSigner(retired, identity.getName(),
                                                    &status.getControllerCertificate()));
    BOOST_CHECK(!matchesConfiguredControllerSigner(retired, identity.getName()));

    const auto indexName = ndn::Name(identity.getName()).append("NDNSF").append("HISTORY")
                           .append("PUBPARAMS");
    const auto readPublic = [&] (const ndn::Name& name) {
      face.sentData.clear();
      ServiceControllerTestAccess::archivedPublicParametersInterest(
        controller, indexName, ndn::Interest(name));
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      const auto packet = face.sentData.front();
      BOOST_CHECK_EQUAL(packet.getName(), name);
      BOOST_CHECK(ndn::security::verifySignature(packet, successor.getPublicKey()));
      return packet;
    };
    const auto versionedIndex = historicalPublicParametersIndexName(identity.getName(),
      version.controllerGenerationTimestamp,
      ServiceControllerTestAccess::abeMaterialVersion(controller));
    const auto indexPacket = readPublic(versionedIndex);
    const auto& content = indexPacket.getContent();
    std::vector<std::tuple<uint64_t, uint64_t, std::string, std::string>> entries;
    BOOST_REQUIRE(parseHistoricalPublicParametersIndex(
      std::string(content.value_begin(), content.value_end()), entries));
    BOOST_REQUIRE(!entries.empty());
    const auto& [generation, epoch, digest, originalName] = entries.front();
    const auto proofName = historicalPublicParametersName(identity.getName(), generation,
      epoch, digest, version.controllerGenerationTimestamp);
    const auto proofPacket = readPublic(proofName);
    archivedProofBeforeRestart = proofPacket;
    {
      const auto hidden = state.string() + ".unavailable";
      std::filesystem::rename(state, hidden);
      auto restore = ndn::make_scope_exit([&] { std::filesystem::rename(hidden, state); });
      BOOST_CHECK(readPublic(versionedIndex).wireEncode() == indexPacket.wireEncode());
      BOOST_CHECK(readPublic(proofName).wireEncode() == proofPacket.wireEncode());
    }
  }
  BOOST_CHECK(stateBytes().find(originalParameters.toUri(ndn::name::UriFormat::CANONICAL)) !=
              std::string::npos);
  ::unsetenv("NDNSF_CONTROLLER_HANDOFF");
  {
    ndn::DummyClientFace face(keys);
    ndn::ValidatorConfig validator(face);
    ServiceController restarted(face, successor, validator, policy);
    BOOST_REQUIRE(restarted.getControllerVersion().isValid());
    BOOST_REQUIRE(archivedProofBeforeRestart);
    const auto previousName = archivedProofBeforeRestart->getName();
    const auto changedName = ndn::Name(previousName.getPrefix(-1)).appendVersion(
      restarted.getControllerVersion().controllerGenerationTimestamp);
    BOOST_CHECK(changedName != previousName);
    const auto archivePrefix = ndn::Name(identity.getName()).append("NDNSF").append("HISTORY")
      .append("PUBPARAMS");
    ServiceControllerTestAccess::archivedPublicParametersInterest(
      restarted, archivePrefix, ndn::Interest(previousName));
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_CHECK(face.sentData.empty());
    ServiceControllerTestAccess::archivedPublicParametersInterest(
      restarted, archivePrefix, ndn::Interest(changedName));
    face.processEvents(ndn::time::milliseconds(-1));
    BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
    BOOST_CHECK(face.sentData.front().getContent() == archivedProofBeforeRestart->getContent());
    BOOST_CHECK(ndn::security::verifySignature(face.sentData.front(), successor.getPublicKey()));
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalGrantWire(restarted, originalGrant));
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalGrantWire(restarted, legacyGrant));
    for (const auto& wire : rejectedGrants)
      BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalGrantWire(restarted, wire));
    BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalGrantWire(restarted, laterRecord));
    BOOST_CHECK(ServiceControllerTestAccess::acceptsHistoricalPublication(restarted,
                                                                          witnessedPublication));
    BOOST_CHECK(!ServiceControllerTestAccess::acceptsHistoricalPublication(restarted,
                                                                           unwitnessedPublication));
  }
  const auto updated = stateBytes();
  const auto witness = std::find_if(std::filesystem::directory_iterator(root),
    std::filesystem::directory_iterator{}, [] (const auto& file) {
      return file.path().filename().string().find("authority.state.grant-witness.") == 0;
    });
  BOOST_REQUIRE(witness != std::filesystem::directory_iterator{});
  const auto witnessPath = witness->path();
  std::ifstream witnessInput(witnessPath, std::ios::binary);
  std::string witnessWire(std::istreambuf_iterator<char>(witnessInput), {});
  BOOST_REQUIRE(!witnessWire.empty());
  witnessWire.back() ^= 1;
  {
    std::ofstream out(witnessPath, std::ios::binary | std::ios::trunc);
    out.write(witnessWire.data(), witnessWire.size());
    BOOST_REQUIRE(out);
  }
  verifyRefusal(successor);
  BOOST_CHECK(stateBytes() == updated);
  keys.setDefaultKey(identity, predecessorKey);
  storeProof(handoff);
  verifyRefusal(predecessor);
  BOOST_CHECK(stateBytes() == updated);
}

/** The commit boundary across a restart: a revocation whose rotation failed is durable, so the next controller
 *  must recover by completing the rotation - not by advertising an epoch whose material never existed, and not by
 *  rejecting its own stored material. */
BOOST_AUTO_TEST_CASE(PersistedRotationFailureRecoversInANewController)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-recover-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain controllerKeys;
  const auto identity = controllerKeys.createIdentity(
      ndn::Name("/controller/spec243-recover"), ndn::RsaKeyParams(2048));
  const auto certificate = identity.getDefaultKey().getDefaultCertificate();
  const auto policies = ndnsf::test::testFixturePath("controller-policies.conf").string();

  ndn::Name paramsNameBefore;
  std::string paramsDigestBefore;
  {
    ndn::DummyClientFace face(controllerKeys);
    ndn::ValidatorConfig validator(face);
    ServiceController controller(face, certificate, validator, policies);
    ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
    ServiceControllerTestAccess::ensureInternalIdentity(controller,
      ndn::Name("/spec179/rotate-fail/user"));
    paramsNameBefore = ServiceControllerTestAccess::abePublicParametersName(controller);
    paramsDigestBefore = ServiceControllerTestAccess::abePublicParametersDigest(controller);

    // The revocation is accepted and made durable, then the rotation behind it fails.
    ::setenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE", "1", 1);
    BOOST_CHECK(!controller.revoke(makeServiceRevocation("/spec179/rotate-fail/user")));
    ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");

    // Fail-closed: the status still describes the material that exists, never the advanced epoch.
    const auto status = controller.getPolicyStatus(ndn::Name(SERVICE));
    BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1U);
    BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersName(controller),
                      paramsNameBefore);
    BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(controller),
                      paramsDigestBefore);
  }

  {
    ndn::DummyClientFace face(controllerKeys);
    ndn::ValidatorConfig validator(face);
    ServiceController recovered(face, certificate, validator, policies);
    // Starting is the first assertion: the stored material belongs to the same identity and must not be
    // rejected as a changed signing key.
    const auto status = recovered.getPolicyStatus(ndn::Name(SERVICE));
    BOOST_REQUIRE_EQUAL(status.getRevocations().size(), 1U);

    // Recovery completed the rotation, so the parameters are no longer the ones the failed controller served.
    const auto paramsNameAfter = ServiceControllerTestAccess::abePublicParametersName(recovered);
    const auto paramsDigestAfter = ServiceControllerTestAccess::abePublicParametersDigest(recovered);
    BOOST_CHECK(paramsNameAfter != paramsNameBefore || paramsDigestAfter != paramsDigestBefore);

    // The retired generation stayed readable: the store records why it was archived.
    std::ifstream state(statePath, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(state),
                            std::istreambuf_iterator<char>()};
    BOOST_CHECK(bytes.find("revocation-rotation") != std::string::npos);
    BOOST_CHECK(bytes.find(paramsNameBefore.toUri(ndn::name::UriFormat::CANONICAL)) !=
                std::string::npos);
    // The material's version survives the restart. Without it a reloaded Controller cannot tell which version its keys
    // were generated for, and the check that refuses a version without material is skipped exactly when it matters.
    BOOST_CHECK(bytes.find("NDNSFCSA") != std::string::npos);
    const auto materialVersion = ServiceControllerTestAccess::abeMaterialVersion(recovered);
    BOOST_REQUIRE(materialVersion.isValid());
    BOOST_CHECK_EQUAL(materialVersion.controllerGenerationTimestamp,
                      recovered.getPolicyStatus(ndn::Name(SERVICE))
                        .getControllerVersion().controllerGenerationTimestamp);
    // Material may lag the served version (a grant moves the policy, not the keys) but never lead it.
    BOOST_CHECK(materialVersion.compare(
      recovered.getPolicyStatus(ndn::Name(SERVICE)).getControllerVersion()) <= 0);

    // The same revocation is now a duplicate and must stay a no-op: recovery already finished the work.
    BOOST_CHECK(!recovered.revoke(makeServiceRevocation("/spec179/rotate-fail/user")));
    BOOST_CHECK_EQUAL(recovered.getPolicyStatus(ndn::Name(SERVICE)).getRevocations().size(), 1U);
    // New work is accepted under the rotated generation.
    ServiceControllerTestAccess::ensureInternalIdentity(recovered,
      ndn::Name("/spec179/rotate-fail/second-provider"));
    BOOST_CHECK(recovered.revoke(makeServiceRevocation("/spec179/rotate-fail/second-provider")));
  }

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ControllerRevokeRotationFailureRecordsPendingAndReconciles)
{
  // RV-U24 (R1, 2026-09-05): a revocation whose ABE rotation throws still
  // advances the durable epoch and stays enforced in memory and in every
  // published status (fail-closed), but the crypto identity must remain
  // untouched; the next revoke entry reconciles the pending rotation before
  // accepting another target, so the mixed state cannot grow or persist.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-rotate-fail-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain controllerKeys;
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-rotate-fail"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace controllerFace(controllerKeys);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/spec179/rotate-fail/user"));
  ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/spec179/rotate-fail/provider"));

  const auto paramsNameBefore =
      ServiceControllerTestAccess::abePublicParametersName(serviceController);
  const auto paramsDigestBefore =
      ServiceControllerTestAccess::abePublicParametersDigest(serviceController);

  // Rotation failure injection (test-only env arm inside
  // rotateAbeGenerationAndReissuePolicies): revoke records the revocation
  // and the durable epoch advance but cannot rotate the ABE generation.
  ::setenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE", "1", 1);
  BOOST_CHECK(!serviceController.revoke(makeServiceRevocation(
      "/spec179/rotate-fail/user")));
  ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");

  // Fail-closed while rotation is pending: the revocation is already
  // enforced in the published status and the crypto identity is unchanged.
  const auto statusAfterFailure =
      serviceController.getPolicyStatus(ndn::Name(SERVICE));
  BOOST_REQUIRE_EQUAL(statusAfterFailure.getRevocations().size(), 1U);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersName(serviceController),
                    paramsNameBefore);
  BOOST_CHECK_EQUAL(ServiceControllerTestAccess::abePublicParametersDigest(serviceController),
                    paramsDigestBefore);

  // The next revoke entry reconciles the pending rotation first, then
  // accepts the new target under a fully rotated generation.
  BOOST_CHECK(serviceController.revoke(makeServiceRevocation(
      "/spec179/rotate-fail/provider")));
  const auto paramsNameAfter =
      ServiceControllerTestAccess::abePublicParametersName(serviceController);
  const auto paramsDigestAfter =
      ServiceControllerTestAccess::abePublicParametersDigest(serviceController);
  BOOST_CHECK(paramsNameAfter != paramsNameBefore ||
              paramsDigestAfter != paramsDigestBefore);
  const auto statusAfterReconcile =
      serviceController.getPolicyStatus(ndn::Name(SERVICE));
  BOOST_REQUIRE_EQUAL(statusAfterReconcile.getRevocations().size(), 2U);

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(PendingRotationFencesGrantAndPreservesImmutableStatus)
{
  // Exercise each public recovery entry independently: a normal duplicate
  // remains a no-op, but a failed withdrawal must be recoverable by retrying
  // that same operation, and grant must never reuse its old ABE generation.
  for (const std::string entry : {"revoke", "grant", "reconcile"}) {
    BOOST_TEST_CONTEXT("recovery entry=" << entry) {
      const auto statePath = std::filesystem::temp_directory_path() /
          ("ndnsf-pending-rotation-" + entry + std::to_string(::getpid()) + ".bin");
      std::filesystem::remove(statePath);
      std::filesystem::remove(statePath.string() + ".lock");
      ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);
      ndn::KeyChain keys;
      const auto identity = keys.createIdentity(
          ndn::Name("/controller/pending-rotation/" + entry), ndn::RsaKeyParams(2048));
      ndn::DummyClientFace face(keys);
      ndn::ValidatorConfig validator(face);
      ServiceController controller(face, identity.getDefaultKey().getDefaultCertificate(),
                                   validator, ndnsf::test::testFixturePath("controller-policies.conf").string());
      ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
      const ndn::Name user("/spec179/pending-rotation/user");
      ServiceControllerTestAccess::ensureInternalIdentity(controller, user);
      BOOST_REQUIRE(controller.grant(user, ndn::Name(SERVICE),
                                     ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))));
      const auto before = controller.getPolicyStatus(ndn::Name(SERVICE));
      ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
      const auto latestStatus = [&] {
        return ServiceControllerTestAccess::statusQuery(identity.getName(), ndn::Name(SERVICE));
      };
      const auto admitted = latestStatus();
      const auto params = ndn::Interest(before.getAbePublicParametersName());
      const auto cert = ServiceControllerTestAccess::recipientCertificate(controller, user);
      ndn::Interest dkey(ServiceControllerTestAccess::dkeyInterestPrefix(identity.getName(), cert));
      dkey.setCanBePrefix(true);
      ServiceControllerTestAccess::policyStatusInterest(controller, admitted);
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      face.sentData.clear();
      ServiceControllerTestAccess::requestAbeParameters(controller, params);
      ServiceControllerTestAccess::requestDkey(controller, dkey);
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 2U);
      BOOST_CHECK_EQUAL(face.sentData.front().getName(), params.getName());
      const ndn::Interest oldSegment(face.sentData.back().getName());
      BOOST_CHECK(dkey.getName().isPrefixOf(oldSegment.getName()));
      face.sentData.clear();
      const auto retainedKey = ServiceControllerTestAccess::abePrivateKey(controller, user);
      const auto target = makeUserServiceRevocation(user.toUri().c_str());
      ::setenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE", "1", 1);
      BOOST_CHECK(!controller.revoke(target));
      const auto failed = controller.getPolicyStatus(ndn::Name(SERVICE));
      BOOST_CHECK_EQUAL(failed.getRevocations().size(), 1U);
      const auto recover = [&] {
        if (entry == "revoke")
          return controller.revoke(target);
        if (entry == "grant")
          return controller.grant(user, ndn::Name(SERVICE),
                                  ndn::Name("/PERMISSION").append(ndn::Name(SERVICE)));
        return ServiceControllerTestAccess::reconcilePendingAbeRotation(controller);
      };
      // A still-failing retry must not remove the durable withdrawal.
      BOOST_CHECK(!recover());
      BOOST_CHECK(controller.getControllerVersion() == before.getControllerVersion());
      const auto written = ServiceControllerTestAccess::durableVersion(controller);
      BOOST_REQUIRE(written);
      BOOST_CHECK(*written == before.getControllerVersion());
      BOOST_CHECK_EQUAL(controller.getPolicyStatus(ndn::Name(SERVICE))
                            .getRevocations().size(), 1U);
      // Neither a retry of an already-issued query nor a fresh query may
      // receive positive authority while the rotation is pending.
      ServiceControllerTestAccess::policyStatusInterest(controller, admitted);
      ServiceControllerTestAccess::policyStatusInterest(controller, latestStatus());
      ServiceControllerTestAccess::requestAbeParameters(controller, params);
      ServiceControllerTestAccess::requestDkey(controller, dkey);
      ServiceControllerTestAccess::requestDkey(controller, oldSegment);
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_CHECK(face.sentData.empty());
      ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
      BOOST_CHECK(recover());
      const auto recovered = controller.getPolicyStatus(ndn::Name(SERVICE));
      BOOST_CHECK(recovered.getAbePublicParametersDigest() !=
                  before.getAbePublicParametersDigest());
      const auto committed = ServiceControllerTestAccess::durableVersion(controller);
      BOOST_REQUIRE(committed);
      BOOST_CHECK(*committed == recovered.getControllerVersion());
      BOOST_CHECK_EQUAL(recovered.getControllerVersion().controllerGenerationTimestamp,
                        before.getControllerVersion().controllerGenerationTimestamp);
      BOOST_CHECK_EQUAL(recovered.getControllerVersion().controllerEpoch,
                        before.getControllerVersion().controllerEpoch + (entry == "grant" ? 2 : 1));
      ServiceControllerTestAccess::policyStatusInterest(controller, latestStatus());
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      PolicyStatusData received;
      BOOST_REQUIRE(received.wireDecode(face.sentData.back().getContent().blockFromValue()));
      BOOST_CHECK(received.getControllerVersion() == recovered.getControllerVersion());
      face.sentData.clear();
      ServiceControllerTestAccess::requestAbeParameters(
        controller, ndn::Interest(recovered.getAbePublicParametersName()));
      ServiceControllerTestAccess::requestDkey(controller, oldSegment);
      face.processEvents(ndn::time::milliseconds(-1));
      BOOST_REQUIRE_EQUAL(face.sentData.size(), 1U);
      BOOST_CHECK_EQUAL(face.sentData.front().getName(), recovered.getAbePublicParametersName());
      RevocationState runtime{ndn::Name(SERVICE)};
      const auto now = static_cast<uint64_t>(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now()
                                        .time_since_epoch()).count());
      BOOST_CHECK(runtime.acceptStatus(before, now));
      BOOST_CHECK(runtime.acceptStatus(recovered, now));
      BOOST_CHECK_EQUAL(recovered.getRevocations().size(), entry == "grant" ? 0U : 1U);
      if (entry == "grant") {
        const ndn::Name attribute = ndn::Name("/PERMISSION").append(ndn::Name(SERVICE));
        const std::string secret = "fresh-after-recovery";
        const auto ciphertext = ServiceControllerTestAccess::abeEncrypt(controller, attribute, secret);
        BOOST_CHECK(ServiceControllerTestAccess::decryptFailsClosed(
            ServiceControllerTestAccess::abePublicParameters(controller), retainedKey,
            ciphertext, secret));
        const auto replacement = ServiceControllerTestAccess::abePrivateKey(controller, user);
        const auto recoveredPlaintext = ServiceControllerTestAccess::abeDecrypt(
            controller, replacement, ciphertext);
        BOOST_CHECK_EQUAL(std::string(recoveredPlaintext.begin(), recoveredPlaintext.end()), secret);
      }
      if (entry == "revoke")
        BOOST_CHECK(!controller.revoke(target)); // completed duplicate stays a no-op
      ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
      std::filesystem::remove(statePath);
      std::filesystem::remove(statePath.string() + ".lock");
    }
  }
}

BOOST_AUTO_TEST_CASE(RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint)
{
  // Keep the target-shape matrix at the real Controller boundary.  The unit
  // suite checks RevocationState predicates directly; this case proves that
  // each typed target emitted by ServiceController produces a valid status
  // that the enforcing state accepts and applies at every protected cut
  // point, without over-revoking a replacement or unrelated subject.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-target-matrix-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-target-matrix",
                         "tpm-memory:spec179-controller-target-matrix");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-target-matrix"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       ndnsf::test::testFixturePath("controller-policies.conf").string());

  const ndn::Name service(SERVICE);
  const ndn::Name otherService("/OtherService");
  const auto nowMs = [] {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
  };
  const auto transitions = {ProtectedTransition::DISCOVERY,
                            ProtectedTransition::ACK_COLLECTION,
                            ProtectedTransition::SELECTION,
                            ProtectedTransition::PROVIDER_EXECUTION,
                            ProtectedTransition::RESPONSE_DELIVERY,
                            ProtectedTransition::STREAM_EVENT};

  const auto checkStatus = [&] (const RevocationTarget& target,
                                const AuthorizationSubject& revoked,
                                const AuthorizationSubject& replacement,
                                const AuthorizationSubject& unaffected) {
    BOOST_REQUIRE(serviceController.revoke(target));
    const auto status = serviceController.getPolicyStatus(service);
    RevocationState state(service);
    BOOST_REQUIRE(state.acceptStatus(status, nowMs()));
    BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
    for (const auto transition : transitions) {
      const auto denied = state.authorize(revoked, transition, nowMs());
      BOOST_CHECK(!denied.allowed);
      BOOST_CHECK(denied.reason.find("revoked_before_") == 0);
      BOOST_CHECK(state.authorize(replacement, transition, nowMs()).allowed);
      BOOST_CHECK(state.authorize(unaffected, transition, nowMs()).allowed);
    }
  };

  const ndn::Name identityTarget("/spec179/controller-target/user");
  checkStatus(makeIdentityRevocation(identityTarget.toUri().c_str()),
              AuthorizationSubject{identityTarget, "sha256:identity-cert", service,
                                   ndn::Name("/PERMISSION").append(service)},
              AuthorizationSubject{ndn::Name("/spec179/controller-target/user-replacement"),
                                   "sha256:replacement-cert", service,
                                   ndn::Name("/PERMISSION").append(service)},
              AuthorizationSubject{ndn::Name("/spec179/controller-target/unrelated"),
                                   "sha256:unrelated-cert", service,
                                   ndn::Name("/PERMISSION").append(service)});

  const ndn::Name certificateTarget("/spec179/controller-target/provider");
  checkStatus(makeCertificateRevocation(certificateTarget.toUri().c_str(),
                                        "sha256:old-provider-cert"),
              AuthorizationSubject{certificateTarget, "sha256:old-provider-cert", service,
                                   ndn::Name("/SERVICE").append(service)},
              AuthorizationSubject{certificateTarget, "sha256:new-provider-cert", service,
                                   ndn::Name("/SERVICE").append(service)},
              AuthorizationSubject{ndn::Name("/spec179/controller-target/provider-other"),
                                   "sha256:old-provider-cert", service,
                                   ndn::Name("/SERVICE").append(service)});

  const ndn::Name serviceTarget("/spec179/controller-target/service-provider");
  checkStatus(makeServiceRevocation(serviceTarget.toUri().c_str()),
              AuthorizationSubject{serviceTarget, "sha256:service-provider-cert", service,
                                   ndn::Name("/SERVICE").append(service)},
              AuthorizationSubject{ndn::Name("/spec179/controller-target/service-replacement"),
                                   "sha256:replacement-cert", service,
                                   ndn::Name("/SERVICE").append(service)},
              AuthorizationSubject{ndn::Name("/spec179/controller-target/service-unrelated"),
                                   "sha256:unrelated-cert", service,
                                   ndn::Name("/SERVICE").append(service)});

  // A service-scoped target must not deny the same identity when a status is
  // requested for another service.
  const auto otherStatus = serviceController.getPolicyStatus(otherService);
  RevocationState otherState(otherService);
  BOOST_REQUIRE(otherState.acceptStatus(otherStatus, nowMs()));
  BOOST_CHECK(otherState.authorize(
      AuthorizationSubject{serviceTarget, "sha256:service-provider-cert", otherService,
                           ndn::Name("/SERVICE").append(otherService)},
      ProtectedTransition::PROVIDER_EXECUTION, nowMs()).allowed);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_CONTROLLER_TARGET_MATRIX identity=6/6"
      " certificate=6/6 service=6/6 cross_service=allow");

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(RuntimePolicyLeaseExpiresWithoutTraffic)
{
  ndn::KeyChain keys("pib-memory:runtime-lease", "tpm-memory:runtime-lease");
  ndn::DummyClientFace face(keys);
  const ndn::Name userName("/rollover/lease/user"), providerName("/rollover/lease/provider");
  const auto makeCertificate = [&] (const ndn::Name& name) {
    return keys.createIdentity(name, ndn::RsaKeyParams(2048)).getDefaultKey()
               .getDefaultCertificate();
  };
  const auto userCertificate = makeCertificate(userName);
  const auto providerCertificate = makeCertificate(providerName);
  const auto controllerCertificate = makeCertificate(ndn::Name("/rollover/lease/controller"));
  ndn_service_framework::test::IsolatedTestTrust trust{
    userCertificate, providerCertificate, controllerCertificate};
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/rollover/lease"),
                   userCertificate, controllerCertificate, trust.schemaPath());
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face, ndn::Name("/rollover/lease"),
                           providerCertificate, controllerCertificate, trust.schemaPath());
  const ndn::Name service(SERVICE);
  const ControllerVersion version{9700, 1};
  user.applyPermissionResponse(runtimePermission(
      userName, tlv::UserPermission, providerName, service, version));
  provider.applyPermissionResponse(runtimePermission(
      providerName, tlv::ProviderPermission, providerName, service, version));
  auto status = liveStatusFor(version);
  const auto start = status.getValidFromMs() + 1000;
  status.setValidity(status.getValidFromMs(), start + 200);
  BOOST_REQUIRE(user.installControllerStatus(status));
  BOOST_REQUIRE(provider.installControllerStatus(status));
  RequestMessage request;
  ndn::Name publishedName;
  RequestMessage publishedMessage;
  size_t terminal = 0;
  user.setRequestPublisher([&] (const ndn::Name&, const ndn::Name& name,
      const std::vector<ndn::Name>&, const ndn::Name&, const RequestMessage& message, size_t) {
    publishedName = name;
    publishedMessage = message;
  });
  const auto invoke = [&] {
    return user.RequestService({providerName}, service, request, 10000,
      [&] (const ndn::Name&) { ++terminal; }, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  };
  BOOST_REQUIRE(!invoke().empty());
  BOOST_REQUIRE(!publishedName.empty());
  BOOST_CHECK_EQUAL(user.getPendingCallCount(), 1U);
  face.processEvents(ndn::time::milliseconds(400));
  BOOST_CHECK_EQUAL(terminal, 1U);
  BOOST_CHECK_EQUAL(user.getPendingCallCount(), 0U);
  BOOST_CHECK(invoke().empty());
  BOOST_CHECK(!provider.handleDecryptedRequestByName(publishedName, publishedMessage).getStatus());
  face.processEvents(ndn::time::milliseconds(2));
  BOOST_CHECK_EQUAL(terminal, 1U);
}

BOOST_AUTO_TEST_CASE(RealUserAndProviderEnforceControllerRevocationAtRuntime)
{
  // Keep this case on the LocalMock construction path so it exercises the
  // production ServiceUser/ServiceProvider authorization boundaries without
  // depending on a live NFD or NAC-ABE bootstrap.  The Controller status is
  // still installed through the public runtime API and the request crosses
  // the real User publication and Provider dispatch functions.
  ndn::Face face;
  ndn::security::KeyChain keyChain(
      "pib-memory:spec179-runtime-revocation",
      "tpm-memory:spec179-runtime-revocation");
  const ndn::Name userName("/spec179/runtime/user");
  const ndn::Name providerName("/spec179/runtime/provider");
  const ndn::Name aaName("/spec179/runtime/aa");
  const ndn::Name serviceName(SERVICE);
  const auto userCert = keyChain.createIdentity(userName, ndn::RsaKeyParams(2048))
                          .getDefaultKey().getDefaultCertificate();
  const auto providerCert =
      keyChain.createIdentity(providerName, ndn::RsaKeyParams(2048))
          .getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(aaName, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  ndn_service_framework::test::IsolatedTestTrust fixedTrust11{
    userCert, providerCert, aaCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec179/runtime"),
                   userCert, aaCert, fixedTrust11.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust12{
    userCert, providerCert, aaCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/runtime"), providerCert, aaCert,
                           fixedTrust12.schemaPath());

  const ControllerVersion version1{9100, 1};
  user.applyPermissionResponse(runtimePermission(
      userName, tlv::UserPermission, providerName, serviceName, version1));
  provider.applyPermissionResponse(runtimePermission(
      providerName, tlv::ProviderPermission, providerName, serviceName, version1));
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(version1)));
  BOOST_REQUIRE(provider.installControllerStatus(liveStatusFor(version1)));

  size_t published = 0;
  size_t providerExecutions = 0;
  RequestMessage publishedRequest;
  ndn::Name publishedName;
  user.setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&, const ndn::Name&,
           const RequestMessage& request, size_t) {
        ++published;
        publishedName = requestName;
        publishedRequest = request;
      });
  provider.addService(
      serviceName,
      ServiceProvider::RequestHandler(
          [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
               const ndn::Name&, const RequestMessage&) {
            ++providerExecutions;
            ResponseMessage response;
            response.setStatus(true);
            return response;
          }));

  RequestMessage request;
  request.setUserToken("spec179-runtime-user-token");
  const std::string payload = "runtime-revocation-input";
  ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
  request.setPayload(payloadBuffer, payloadBuffer.size());
  const auto requestId = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_CHECK_EQUAL(published, 1);
  BOOST_REQUIRE(!publishedName.empty());
  BOOST_CHECK(publishedRequest.hasControllerVersion());
  BOOST_CHECK(publishedRequest.getControllerVersion() == version1);

  const auto responseBefore = provider.handleDecryptedRequestByName(
      publishedName, publishedRequest);
  BOOST_CHECK(responseBefore.getStatus());
  BOOST_CHECK_EQUAL(providerExecutions, 1);

  // A User identity withdrawal is enforced before the next Request is
  // published, so stale permission-table entries cannot keep discovery alive.
  const ControllerVersion version2{9100, 2};
  BOOST_REQUIRE(user.installControllerStatus(
      liveStatusFor(version2, {makeIdentityRevocation(userName.toUri().c_str())})));
  const auto deniedUserRequest = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_CHECK(deniedUserRequest.empty());
  BOOST_CHECK_EQUAL(published, 1);

  // A Provider withdrawal is enforced at execution even when an already
  // authorized User can still publish a current-version Request.
  const ControllerVersion version3{9100, 3};
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(version3)));
  BOOST_REQUIRE(provider.installControllerStatus(
      liveStatusFor(version3, {makeIdentityRevocation(providerName.toUri().c_str())})));
  const auto requestAfterProviderRevocation = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestAfterProviderRevocation.empty());
  BOOST_CHECK_EQUAL(published, 2);
  const auto responseAfter = provider.handleDecryptedRequestByName(
      publishedName, publishedRequest);
  BOOST_CHECK(!responseAfter.getStatus());
  BOOST_CHECK_EQUAL(providerExecutions, 1);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_RUNTIME user_prepublication=deny"
      " provider_execution=deny unaffected_before=allow");
}

BOOST_AUTO_TEST_CASE(RuntimeRestartDropsControllerStatusAndFailsClosed)
{
  // Runtime authority is intentionally process-local.  A restart must not
  // resurrect an obsolete status, request binding, or consumed credential;
  // until a fresh Controller-signed status is installed, a configured
  // runtime fails closed even if a stale permission snapshot is present.
  const ndn::Name controllerPrefix("/spec179/restart/controller");
  const ndn::Name groupPrefix("/spec179/restart");
  const ndn::Name userName("/spec179/restart/user");
  const ndn::Name providerName("/spec179/restart/provider");
  const ndn::Name aaName("/spec179/restart/aa");
  const ndn::Name serviceName(SERVICE);
  const ControllerVersion version{9300, 1};

  ndn::security::KeyChain keyChain(
      "pib-memory:spec179-runtime-restart",
      "tpm-memory:spec179-runtime-restart");
  const auto userCert = keyChain.createIdentity(
      userName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto providerCert = keyChain.createIdentity(
      providerName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(
      aaName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();

  {
    ndn::Face face;
    ndn_service_framework::test::IsolatedTestTrust fixedTrust13{
      userCert, providerCert, aaCert};
    ServiceUser user(ServiceUser::LocalMockTag{}, face, groupPrefix,
                     userCert, aaCert, fixedTrust13.schemaPath());
    // This sets the runtime's configured-controller boundary without requiring
    // a live Controller; no event processing is needed for this assertion.
    user.fetchPermissionsFromController(controllerPrefix);
    user.applyPermissionResponse(runtimePermission(
        userName, tlv::UserPermission, providerName, serviceName, version));
    BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(version)));
    BOOST_REQUIRE(user.getControllerVersion(serviceName));
    BOOST_CHECK_EQUAL(user.getCurrentPolicyEpoch(serviceName), version.controllerEpoch);
  }

  ndn::Face restartedFace;
  ndn_service_framework::test::IsolatedTestTrust fixedTrust14{
    userCert, providerCert, aaCert};
  ServiceUser restarted(ServiceUser::LocalMockTag{}, restartedFace, groupPrefix,
                         userCert, aaCert, fixedTrust14.schemaPath());
  restarted.fetchPermissionsFromController(controllerPrefix);
  // A stale permission snapshot by itself is not authority after restart.
  restarted.applyPermissionResponse(runtimePermission(
      userName, tlv::UserPermission, providerName, serviceName, version));
  BOOST_CHECK(!restarted.getControllerVersion(serviceName));
  BOOST_CHECK_EQUAL(restarted.getCurrentPolicyEpoch(serviceName), 0U);

  size_t published = 0;
  restarted.setRequestPublisher(
      [&published] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
                    const ndn::Name&, const RequestMessage&, size_t) {
        ++published;
      });
  RequestMessage request;
  request.setUserToken("restart-stale-token");
  const std::string payload = "restart-input";
  ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
  request.setPayload(payloadBuffer, payloadBuffer.size());
  const auto requestId = restarted.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_CHECK(requestId.empty());
  BOOST_CHECK_EQUAL(published, 0U);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_RESTART status_lost=fail_closed stale_permission=denied");
}

BOOST_AUTO_TEST_CASE(NewerServiceStatusDoesNotAuthorizeOtherService)
{
  // A process-wide maximum may be retained for legacy diagnostics, but it is
  // never an authorization source.  Service B must continue to compare an
  // incoming request with B's own accepted status when service A advances.
  const ndn::Name controllerPrefix("/spec179/service-isolation/controller");
  const ndn::Name groupPrefix("/spec179/service-isolation");
  const ndn::Name userName("/spec179/service-isolation/user");
  const ndn::Name providerName("/spec179/service-isolation/provider");
  const ndn::Name aaName("/spec179/service-isolation/aa");
  const ndn::Name serviceA("/ObjectDetection/YOLOv8");
  const ndn::Name serviceB("/FlightControl/Takeoff");

  ndn::KeyChain keyChain("pib-memory:spec179-service-isolation",
                         "tpm-memory:spec179-service-isolation");
  const auto userCert = keyChain.createIdentity(
      userName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(
      aaName, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  ndn::Face face;
  ndn_service_framework::test::IsolatedTestTrust fixedTrust15{
    userCert, aaCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, face, groupPrefix, userCert,
                   aaCert, fixedTrust15.schemaPath());
  user.setUseTokens(false);
  user.fetchPermissionsFromController(controllerPrefix);

  const ControllerVersion versionA1{9400, 1};
  const ControllerVersion versionB1{9400, 1};
  const ControllerVersion versionA2{9400, 2};
  PermissionResponse permissions;
  permissions.setTargetIdentity(userName.toUri());
  permissions.setPermissionKind(tlv::UserPermission);
  permissions.setPolicyEpoch(1);
  permissions.setControllerVersion(versionA1);
  for (const auto& service : {serviceA, serviceB}) {
    PermissionEntry entry;
    entry.setProviderName(providerName.toUri());
    entry.setServiceName(service.toUri());
    entry.setToken("");
    entry.setTtl(0);
    entry.setVersion(1);
    permissions.addEntry(entry);
  }
  user.applyPermissionResponse(permissions);
  BOOST_REQUIRE(user.installControllerStatus(
      liveStatusForService(serviceA, versionA1)));
  BOOST_REQUIRE(user.installControllerStatus(
      liveStatusForService(serviceB, versionB1)));
  BOOST_REQUIRE(user.installControllerStatus(
      liveStatusForService(serviceA, versionA2)));
  BOOST_REQUIRE(user.getControllerVersion(serviceA));
  BOOST_REQUIRE(user.getControllerVersion(serviceB));
  BOOST_CHECK(*user.getControllerVersion(serviceA) == versionA2);
  BOOST_CHECK(*user.getControllerVersion(serviceB) == versionB1);

  size_t published = 0;
  user.setRequestPublisher(
      [&published] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
                    const ndn::Name&, const RequestMessage&, size_t) {
        ++published;
      });
  RequestMessage staleRequest;
  staleRequest.setControllerVersion(versionA2);
  const std::string payload = "service-isolation";
  ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
  staleRequest.setPayload(payloadBuffer, payloadBuffer.size());
  BOOST_CHECK(user.RequestService(
      {providerName}, serviceB, staleRequest, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding).empty());
  BOOST_CHECK_EQUAL(published, 0U);

  RequestMessage currentRequest;
  currentRequest.setControllerVersion(versionB1);
  currentRequest.setPayload(payloadBuffer, payloadBuffer.size());
  BOOST_CHECK(!user.RequestService(
      {providerName}, serviceB, currentRequest, 1,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding).empty());
  BOOST_CHECK_EQUAL(published, 1U);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_SERVICE_ISOLATION service_a=newer service_b=exact");
}

BOOST_AUTO_TEST_CASE(RealRuntimeEnforcesCertificateAndServiceRevocation)
{
  // Identity-wide withdrawal is covered above.  This case keeps the same
  // production LocalMock request/dispatch boundary but changes only the
  // revocation target, proving that certificate-only and service-scoped
  // withdrawal are not accidentally implemented as identity-wide denial.
  ndn::Face face;
  ndn::security::KeyChain keyChain(
      "pib-memory:spec179-runtime-scope",
      "tpm-memory:spec179-runtime-scope");
  const ndn::Name userName("/spec179/runtime-scope/user");
  const ndn::Name providerName("/spec179/runtime-scope/provider");
  const ndn::Name aaName("/spec179/runtime-scope/aa");
  const ndn::Name serviceName(SERVICE);
  const auto userIdentity = keyChain.createIdentity(userName, ndn::RsaKeyParams(2048));
  const auto providerIdentity = keyChain.createIdentity(providerName, ndn::RsaKeyParams(2048));
  const auto userCert = userIdentity.getDefaultKey().getDefaultCertificate();
  const auto providerCert = providerIdentity.getDefaultKey().getDefaultCertificate();
  const auto userSigningCert = keyChain.createKey(userIdentity, ndn::EcKeyParams())
                                 .getDefaultCertificate();
  const auto providerSigningCert = keyChain.createKey(providerIdentity, ndn::EcKeyParams())
                                     .getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(aaName, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  test::IsolatedTestTrust trust{userCert, userSigningCert, providerCert, providerSigningCert, aaCert};
  std::vector<StreamedInvocationError> streamErrors;
  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec179/runtime-scope"),
                   userCert, userSigningCert, aaCert, trust.schemaPath());
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/runtime-scope"), providerCert, providerSigningCert,
                           aaCert, trust.schemaPath());
  size_t published = 0;
  size_t executions = 0;
  RequestMessage lastRequest;
  ndn::Name lastRequestName;
  user.setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&, const ndn::Name&,
           const RequestMessage& request, size_t) {
        ++published;
        lastRequestName = requestName;
        lastRequest = request;
      });
  provider.addService(
      serviceName,
      ServiceProvider::RequestHandler(
          [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
               const ndn::Name&, const RequestMessage&) {
            ++executions;
            ResponseMessage response;
            response.setStatus(true);
            return response;
          }));

  const auto install = [&] (const ControllerVersion& version,
                            const std::vector<RevocationTarget>& userRevocations,
                            const std::vector<RevocationTarget>& providerRevocations) {
    user.applyPermissionResponse(runtimePermission(
        userName, tlv::UserPermission, providerName, serviceName, version));
    provider.applyPermissionResponse(runtimePermission(
        providerName, tlv::ProviderPermission, providerName, serviceName, version));
    BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(version, userRevocations)));
    BOOST_REQUIRE(provider.installControllerStatus(
        liveStatusFor(version, providerRevocations)));
  };
  const auto invoke = [&] (bool expectPublication = true) {
    const auto publishedBefore = published;
    RequestMessage request;
    request.setUserToken("scope-token-" + std::to_string(published + 1));
    const std::string payload = "scope-input";
    ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                              payload.size());
    request.setPayload(payloadBuffer, payloadBuffer.size());
    const auto id = user.RequestService(
        {providerName}, serviceName, request, 1000,
        [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
        tlv::FirstResponding);
    if (id.empty()) {
      BOOST_CHECK(!expectPublication);
      BOOST_CHECK_EQUAL(published, publishedBefore);
      return false;
    }
    BOOST_REQUIRE(expectPublication);
    BOOST_REQUIRE_EQUAL(published, publishedBefore + 1);
    const auto response = provider.handleDecryptedRequestByName(
        lastRequestName, lastRequest);
    return response.getStatus();
  };

  const ControllerVersion v1{9200, 1};
  install(v1, {}, {});
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, 1);

  const auto providerDigest = ServiceControllerTestAccess::certificateDigest(providerCert);
  const ControllerVersion v2{9200, 2};
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, 2);

  // The target is not active until the newer status is installed on the
  // Provider.  User traffic remains publishable at the current version.
  install(v2, {}, {makeCertificateRevocation(
      providerName.toUri().c_str(), providerDigest.c_str())});
  BOOST_CHECK(!invoke());
  BOOST_CHECK_EQUAL(executions, 2);

  // A newer status without that certificate target reauthorizes the same
  // identity, showing that certificate-only withdrawal is forward-only.
  const ControllerVersion v3{9200, 3};
  install(v3, {}, {});
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, 3);

  const ControllerVersion v4{9200, 4};
  install(v4, {}, {makeServiceRevocation(providerName.toUri().c_str())});
  BOOST_CHECK(!invoke());
  BOOST_CHECK_EQUAL(executions, 3);

  // The same target kinds must be enforced on the User side before the next
  // Request publication, not only on Provider execution.  A newer status
  // without the target restores the same identity, demonstrating that the
  // certificate/service withdrawal is forward-only for both roles.
  const auto userDigest = ServiceControllerTestAccess::certificateDigest(userCert);
  const ControllerVersion v5{9200, 5};
  install(v5, {makeCertificateRevocation(userName.toUri().c_str(), userDigest.c_str())}, {});
  BOOST_CHECK(!invoke(false));
  BOOST_CHECK_EQUAL(executions, 3);

  const ControllerVersion v6{9200, 6};
  install(v6, {}, {});
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, 4);

  // Identity-wide Provider withdrawal is distinct from certificate- and
  // service-scoped withdrawal: the User may still publish a current-version
  // request, but the Provider must reject execution until a newer status
  // explicitly reauthorizes that identity.
  const ControllerVersion v7{9200, 7};
  install(v7, {}, {makeIdentityRevocation(providerName.toUri().c_str())});
  BOOST_CHECK(!invoke());
  BOOST_CHECK_EQUAL(executions, 4);

  const ControllerVersion v8{9200, 8};
  install(v8, {}, {});
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, 5);

  const ControllerVersion v9{9200, 9};
  install(v9, {makeUserServiceRevocation(userName.toUri().c_str())}, {});
  BOOST_CHECK(!invoke(false));
  BOOST_CHECK_EQUAL(executions, 5);

  // Installing a newer, otherwise-unrevoked status must also make the
  // Provider reject an old message-carried version.  A stale request cannot
  // be treated as an implicit reauthorization or execute a second time.
  BOOST_REQUIRE(!lastRequest.getControllerVersion().isValid() ||
                lastRequest.getControllerVersion() != v9);
  const auto executionsBeforeStale = executions;
  const auto staleResponse = provider.handleDecryptedRequestByName(
      lastRequestName, lastRequest);
  BOOST_CHECK(!staleResponse.getStatus());
  BOOST_CHECK_EQUAL(executions, executionsBeforeStale);

  // Selection-free Targeted must use the same discovery authorization gate
  // before consuming a cached token or launching a bootstrap/refill.  This
  // is deliberately checked after service-scoped User revocation, where a
  // stale permission-table entry alone must not keep the fast path alive.
  RequestMessage targetedRequest;
  const std::string targetedPayload = "targeted-scope-input";
  ndn::Buffer targetedBytes(
      reinterpret_cast<const uint8_t*>(targetedPayload.data()),
      targetedPayload.size());
  targetedRequest.setPayload(targetedBytes, targetedBytes.size());
  const auto targetedDenied = user.RequestServiceTargeted(
      providerName, serviceName, targetedRequest, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {});
  BOOST_CHECK(targetedDenied.empty());
  BOOST_CHECK_EQUAL(published, 8);
  const auto userSigner = ServiceControllerTestAccess::certificateDigest(userSigningCert);
  const auto providerSigner = ServiceControllerTestAccess::certificateDigest(providerSigningCert);
  BOOST_REQUIRE_NE(userDigest, userSigner);
  BOOST_REQUIRE_NE(providerDigest, providerSigner);
  install({9200, 10}, {makeCertificateRevocation(userName.toUri().c_str(), userSigner.c_str())}, {});
  BOOST_CHECK(!invoke(false));
  BOOST_CHECK_EQUAL(executions, executionsBeforeStale);
  install({9200, 11}, {},
          {makeCertificateRevocation(providerName.toUri().c_str(), providerSigner.c_str())});
  BOOST_CHECK(!invoke());
  BOOST_CHECK_EQUAL(executions, executionsBeforeStale);
  install({9200, 12}, {}, {});
  BOOST_CHECK(invoke());
  BOOST_CHECK_EQUAL(executions, executionsBeforeStale + 1);
  const ndn::Name otherProvider("/spec179/runtime-scope/other-provider");
  auto twoProviders = runtimePermission(userName, tlv::UserPermission, providerName, serviceName,
                                        {9200, 12});
  twoProviders.addEntry(runtimePermission(userName, tlv::UserPermission, otherProvider, serviceName,
                                         {9200, 12}).getEntries().front());
  user.applyPermissionResponse(twoProviders);
  StreamedInvocationOptions streamOptions;
  streamOptions.mode = InvocationMode::Targeted;
  streamOptions.maxEventWireBytes = 4096;
  const auto active = user.RequestServiceStreaming<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      providerName, serviceName, targetedBytes, streamOptions,
      [](const ndn::Buffer&) {}, [](const ndn::Buffer&) {},
      [&](const StreamedInvocationError& error) { streamErrors.push_back(error); });
  BOOST_REQUIRE(active);
  BOOST_REQUIRE(streamErrors.empty());
  std::vector<StreamedInvocationError> otherErrors, discoveryErrors;
  const auto other = user.RequestServiceStreaming<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      otherProvider, serviceName, targetedBytes, streamOptions,
      [](const ndn::Buffer&) {}, [](const ndn::Buffer&) {},
      [&](const StreamedInvocationError& error) {
        otherErrors.push_back(error);
        BOOST_CHECK(!user.hasStreamStateForTest(error.requestId));
        user.cancelStreamRequest(error.requestId);
      });
  BOOST_REQUIRE(other);
  streamOptions.mode = InvocationMode::Normal;
  const auto discovery = user.RequestServiceStreaming<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName, targetedBytes, streamOptions, [](const ndn::Buffer&) {},
      [](const ndn::Buffer&) {}, [&](const auto& error) { discoveryErrors.push_back(error); });
  BOOST_REQUIRE(discovery);
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(
      {9200, 13}, {makeServiceRevocation(otherProvider.toUri().c_str())})));
  BOOST_REQUIRE_EQUAL(otherErrors.size(), 1U);
  BOOST_CHECK(otherErrors.front().code == StreamedInvocationErrorCode::Unauthorized);
  BOOST_CHECK_EQUAL(otherErrors.front().requestId, other->requestId());
  BOOST_CHECK(other->status() == StreamedInvocationStatus::Failed);
  BOOST_CHECK(streamErrors.empty());
  BOOST_CHECK(discoveryErrors.empty());
  BOOST_CHECK(user.hasStreamStateForTest(active->requestId()));
  BOOST_CHECK(user.hasStreamStateForTest(discovery->requestId()));

  RevocationTarget recipientRevoked;
  recipientRevoked.kind = RevocationKind::CERTIFICATE;
  recipientRevoked.certificateDigest = userDigest;
  install({9200, 14}, {recipientRevoked}, {});
  BOOST_REQUIRE_EQUAL(streamErrors.size(), 1U);
  BOOST_CHECK(streamErrors.front().code == StreamedInvocationErrorCode::Unauthorized);
  BOOST_CHECK(active->status() == StreamedInvocationStatus::Failed);
  BOOST_REQUIRE_EQUAL(discoveryErrors.size(), 1U);
  BOOST_CHECK(discoveryErrors.front().code == StreamedInvocationErrorCode::Unauthorized);
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor({9200, 15}, {recipientRevoked})));
  BOOST_CHECK_EQUAL(streamErrors.size(), 1U);
  BOOST_CHECK_EQUAL(otherErrors.size(), 1U);
  BOOST_CHECK_EQUAL(discoveryErrors.size(), 1U);
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_RUNTIME provider_certificate_only=deny"
      " provider_service_scope=deny user_certificate_only=deny"
      " provider_identity_only=deny provider_identity_reauthorized=allow"
      " user_reauthorized=allow user_service_scope=deny"
      " targeted_discovery=deny");
}

BOOST_AUTO_TEST_CASE(RealTargetedProviderRejectsRevokedExecution)
{
  // Targeted requests skip ACK/Selection only after a token bootstrap, but
  // Provider execution remains a Controller-protected transition.  Disable
  // token checking here to isolate that revocation boundary from token-pool
  // bookkeeping; the token lifecycle has its own focused unit coverage.
  ndn::Face face;
  ndn::security::KeyChain keyChain(
      "pib-memory:spec179-targeted-provider",
      "tpm-memory:spec179-targeted-provider");
  const ndn::Name userName("/spec179/targeted/user");
  const ndn::Name providerName("/spec179/targeted/provider");
  const ndn::Name aaName("/spec179/targeted/aa");
  const ndn::Name serviceName(SERVICE);
  const auto userCert = keyChain.createIdentity(userName, ndn::RsaKeyParams(2048))
                          .getDefaultKey().getDefaultCertificate();
  const auto providerCert =
      keyChain.createIdentity(providerName, ndn::RsaKeyParams(2048))
          .getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(aaName, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  ndn_service_framework::test::IsolatedTestTrust fixedTrust18{
    userCert, providerCert, aaCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/targeted"), providerCert,
                           aaCert, fixedTrust18.schemaPath());
  provider.setUseTokens(false);
  size_t providerExecutions = 0;
  provider.addTargetedService(
      serviceName,
      [&] (const RequestMessage&) {
        ++providerExecutions;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      });

  const auto invoke = [&] (const ControllerVersion& version) {
    provider.applyPermissionResponse(runtimePermission(
        providerName, tlv::ProviderPermission, providerName, serviceName, version));
    BOOST_REQUIRE(provider.installControllerStatus(liveStatusFor(version)));
    RequestMessage request;
    request.setRequestMode(tlv::TargetedRequest);
    request.setTargetProvider(providerName);
    request.setPolicyEpoch(version.controllerEpoch);
    request.setControllerVersion(version);
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>("targeted"), 8);
    request.setPayload(payload, payload.size());
    return provider.handleDecryptedRequestByName(
        makeRequestNameV2(userName, serviceName,
                          ndn::Name("/targeted-" +
                                    std::to_string(version.controllerEpoch))),
        request);
  };

  const ControllerVersion v1{9400, 1};
  const auto before = invoke(v1);
  BOOST_CHECK(before.getStatus());
  BOOST_CHECK_EQUAL(providerExecutions, 1U);

  const ControllerVersion v2{9400, 2};
  provider.applyPermissionResponse(runtimePermission(
      providerName, tlv::ProviderPermission, providerName, serviceName, v2));
  BOOST_REQUIRE(provider.installControllerStatus(
      liveStatusFor(v2, {makeIdentityRevocation(providerName.toUri().c_str())})));
  RequestMessage revokedRequest;
  revokedRequest.setRequestMode(tlv::TargetedRequest);
  revokedRequest.setTargetProvider(providerName);
  revokedRequest.setPolicyEpoch(v2.controllerEpoch);
  revokedRequest.setControllerVersion(v2);
  ndn::Buffer revokedPayload(reinterpret_cast<const uint8_t*>("targeted"), 8);
  revokedRequest.setPayload(revokedPayload, revokedPayload.size());
  const auto denied = provider.handleDecryptedRequestByName(
      makeRequestNameV2(userName, serviceName, ndn::Name("/targeted-revoked")),
      revokedRequest);
  BOOST_CHECK(!denied.getStatus());
  BOOST_CHECK_EQUAL(providerExecutions, 1U);

  const ControllerVersion v3{9400, 3};
  const auto restored = invoke(v3);
  BOOST_CHECK(restored.getStatus());
  BOOST_CHECK_EQUAL(providerExecutions, 2U);
  BOOST_TEST_MESSAGE("NDNSF_REVOCATION_TARGETED provider_execution=deny"
                     " reauthorized_after_new_version=allow");
}

BOOST_AUTO_TEST_CASE(ConfiguredControllerFailsClosedBeforeStatusInstallation)
{
  // Permission tables are not an authority snapshot.  Once a runtime has a
  // Controller prefix, both User discovery and Provider execution must stop
  // until the exact signed PolicyStatus for the service is installed.
  ndn::Face face;
  ndn::security::KeyChain keyChain(
      "pib-memory:spec179-no-status",
      "tpm-memory:spec179-no-status");
  const ndn::Name userName("/spec179/no-status/user");
  const ndn::Name providerName("/spec179/no-status/provider");
  const ndn::Name aaName("/spec179/no-status/aa");
  const ndn::Name controllerPrefix("/controller/spec179-no-status");
  const ndn::Name serviceName(SERVICE);
  const auto userCert = keyChain.createIdentity(userName, ndn::RsaKeyParams(2048))
                          .getDefaultKey().getDefaultCertificate();
  const auto providerCert =
      keyChain.createIdentity(providerName, ndn::RsaKeyParams(2048))
          .getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(aaName, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  ndn_service_framework::test::IsolatedTestTrust fixedTrust19{
    userCert, providerCert, aaCert};
  ServiceUser user(ServiceUser::LocalMockTag{}, face,
                   ndn::Name("/spec179/no-status"), userCert, aaCert,
                   fixedTrust19.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust20{
    userCert, providerCert, aaCert};
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/no-status"), providerCert,
                           aaCert, fixedTrust20.schemaPath());
  // Configure the Controller prefix before applying a permission response so
  // its non-zero version is exercised as an authenticated-looking hint.  A
  // permission/manifest hint must not become authority without exact signed
  // PolicyStatus Data.
  user.fetchPermissionsFromController(controllerPrefix);
  provider.fetchPermissionsFromController(controllerPrefix);
  const ControllerVersion hintedVersion{9300, 4};
  user.applyPermissionResponse(runtimePermission(
      userName, tlv::UserPermission, providerName, serviceName, hintedVersion));
  provider.applyPermissionResponse(runtimePermission(
      providerName, tlv::ProviderPermission, providerName, serviceName, hintedVersion));
  BOOST_CHECK(!user.getControllerVersion());
  BOOST_CHECK(!provider.getControllerVersion());
  provider.addService(
      serviceName,
      ServiceProvider::RequestHandler(
          [] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
              const ndn::Name&, const RequestMessage&) {
            ResponseMessage response;
            response.setStatus(true);
            return response;
          }));

  size_t published = 0;
  user.setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
           const ndn::Name&, const RequestMessage&, size_t) {
        ++published;
      });
  RequestMessage request;
  const std::string payload = "must-not-start";
  ndn::Buffer payloadBuffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size());
  request.setPayload(payloadBuffer, payloadBuffer.size());
  const auto requestId = user.RequestService(
      {providerName}, serviceName, request, 1000,
      [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
      tlv::FirstResponding);
  BOOST_CHECK(requestId.empty());
  BOOST_CHECK_EQUAL(published, 0);

  RequestMessage providerRequest;
  providerRequest.setPolicyEpoch(1);
  const auto providerResponse = provider.handleDecryptedRequestByName(
      makeRequestNameV2(userName, serviceName, ndn::Name("no-status-request")),
      providerRequest);
  BOOST_CHECK(!providerResponse.getStatus());
  BOOST_TEST_MESSAGE(
      "NDNSF_REVOCATION_RUNTIME no_status=fail_closed user_publish=0 provider_execute=0");
}

BOOST_AUTO_TEST_CASE(UnprovisionedRuntimesConstructAndRemainUnauthorized,
                     *boost::unit_test::timeout(5))
{
  // Real constructors, no AA relay and no DKEY. They must return so an
  // application can drive online permission renewal, without admitting work.
  ndn::KeyChain keys;
  ndn::DummyClientFace face(keys);
  const ndn::Name userName("/spec179/unprovisioned/user");
  const ndn::Name providerName("/spec179/unprovisioned/provider");
  const auto cert = [&] (const ndn::Name& name) {
    return keys.createIdentity(name, ndn::RsaKeyParams(2048))
        .getDefaultKey().getDefaultCertificate();
  };
  const auto aa = cert(ndn::Name("/spec179/unprovisioned/controller"));
  const auto userCertificate = cert(userName);
  const auto signingCertificate = keys.createKey(keys.getPib().getIdentity(userName),
                                                 ndn::EcKeyParams()).getDefaultCertificate();
  const auto providerCertificate = cert(providerName);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust21{
    aa, userCertificate, signingCertificate, providerCertificate};
  ServiceUser user(face, ndn::Name("/spec179/unprovisioned/group"),
                   userCertificate, signingCertificate, aa, fixedTrust21.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust22{
    aa, userCertificate, providerCertificate};
  ServiceProvider provider(face, ndn::Name("/spec179/unprovisioned/group"),
                           providerCertificate, aa, fixedTrust22.schemaPath());
  BOOST_CHECK(!user.isNacConsumerReadyForTest());
  BOOST_CHECK(!provider.isNacConsumerReadyForTest());
  user.fetchPermissionsFromController(aa.getIdentity());
  provider.fetchPermissionsFromController(aa.getIdentity());
  BOOST_CHECK(!user.getControllerVersion());
  BOOST_CHECK(!provider.getControllerVersion());
  size_t executed = 0;
  provider.addService(ndn::Name(SERVICE), ServiceProvider::RequestHandler(
      [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const ndn::Name&, const RequestMessage&) {
        ++executed;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));
  size_t published = 0;
  user.setRequestPublisher([&] (const ndn::Name&, const ndn::Name&,
      const std::vector<ndn::Name>&, const ndn::Name&, const RequestMessage&, size_t) {
    ++published;
  });
  RequestMessage request;
  const auto id = user.RequestService({providerName}, ndn::Name(SERVICE), request,
      1000, [] (const ndn::Name&) {}, [] (const ResponseMessage&) {}, tlv::FirstResponding);
  BOOST_CHECK(id.empty());
  BOOST_CHECK_EQUAL(published, 0U);
  BOOST_CHECK(!provider.handleDecryptedRequestByName(
      makeRequestNameV2(userName, ndn::Name(SERVICE), ndn::Name("no-dkey")), request)
      .getStatus());
  BOOST_CHECK_EQUAL(executed, 0U);

  const auto service = ndn::Name(SERVICE);
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor({9850, 1})));
  BOOST_CHECK(user.checkRequestBootstrap(service, {providerName}, false) ==
              ServiceUser::RequestBootstrapState::WAITING);
  const auto revoke = [&](const ndn::security::Certificate& certificate, uint64_t epoch) {
    const auto fingerprint = ServiceControllerTestAccess::certificateDigest(certificate);
    BOOST_REQUIRE(user.installControllerStatus(liveStatusFor({9850, epoch},
        {makeCertificateRevocation(userName.toUri().c_str(), fingerprint.c_str())})));
    BOOST_CHECK(user.checkRequestBootstrap(service, {providerName}, false) ==
                ServiceUser::RequestBootstrapState::REVOKED);
  };
  revoke(signingCertificate, 2);
  revoke(userCertificate, 3);
  BOOST_CHECK_EQUAL(published, 0U);
}

BOOST_AUTO_TEST_CASE(OnlineGrantWaitsForInitialDkeyAndReportsUnwrapFailure,
                     *boost::unit_test::timeout(5))
{
  ndn::KeyChain keys;
  ndn::DummyClientFace face(keys);
  const ndn::Name userName("/spec179/grant-pending/user");
  const ndn::Name providerName("/spec179/grant-pending/provider");
  const auto cert = [&] (const ndn::Name& name) {
    return keys.createIdentity(name, ndn::RsaKeyParams(2048))
        .getDefaultKey().getDefaultCertificate();
  };
  const auto aa = cert(ndn::Name("/spec179/grant-pending/controller"));
  const auto userCertificate = cert(userName);
  const auto providerCertificate = cert(providerName);
  ndn_service_framework::test::IsolatedTestTrust fixedTrust23{
    aa, userCertificate, providerCertificate};
  ServiceUser user(face, ndn::Name("/spec179/grant-pending/group"),
                   userCertificate, aa, fixedTrust23.schemaPath());
  const ControllerVersion version{9900, 1};
  user.applyPermissionResponse(runtimePermission(
      userName, tlv::UserPermission, providerName, ndn::Name(SERVICE), version));
  BOOST_REQUIRE(user.installControllerStatus(liveStatusFor(version)));
  BOOST_REQUIRE(!user.isNacConsumerReadyForTest());
  size_t published = 0;
  user.setRequestPublisher([&] (const ndn::Name&, const ndn::Name&,
      const std::vector<ndn::Name>&, const ndn::Name&, const RequestMessage&, size_t) {
    ++published;
  });
  RequestMessage request;
  const auto id = user.RequestService({providerName}, ndn::Name(SERVICE), request,
      1000, [] (const ndn::Name&) {}, [] (const ResponseMessage&) {}, tlv::FirstResponding);
  BOOST_CHECK(id.empty());
  BOOST_CHECK_EQUAL(published, 0U);

  // Deliver the dedicated named wrapper before testing the missing-DKEY consume boundary.
  // Preserve the error callback across construction of the success closure.
  HybridMessageEnvelope envelope;
  envelope.setKeyId("0000000000000001");
  envelope.setEpochId("0000000000000001");
  envelope.setMessageType("ACK");
  envelope.setNonce(ndn::Buffer(12, 0));
  envelope.setCipherText(ndn::Buffer(1, 0));
  envelope.setAuthTag(ndn::Buffer(16, 0));
  const auto ackName = makeRequestAckNameV2(providerName, userName, ndn::Name(SERVICE), ndn::Name("no-dkey"));
  const auto attribute = hybridAccessAttributeForName(ackName, ndn::Name(SERVICE), providerName);
  const auto locator = makeHybridMessageKeyDataName(ndn::Name(SERVICE), providerName, attribute, envelope.getEpochId());
  // A canonical Block is sufficient: NAC must reject the missing DKEY before
  // decoding encrypted content, as its existing consume overload requires.
  ndn::Block wrapped(128);
  wrapped.encode();
  const auto packets = prepareHybridMessageKeyPackets(ndn::Name(SERVICE), providerName,
      attribute, envelope.getEpochId(), ndn::span<const uint8_t>(wrapped.data(), wrapped.size()),
      keys, ndn::security::signingByIdentity(providerName), ndn::time::milliseconds(1000),
      ndn::tlv::ContentType_Blob);
  BOOST_REQUIRE_EQUAL(packets.size(), 1);
  size_t wrapperInterests = 0;
  auto wrapperRelay = face.onSendInterest.connect([&] (const ndn::Interest& interest) {
    if (!locator.isPrefixOf(interest.getName())) return;
    ++wrapperInterests;
    face.receive(*packets.front());
  });
  size_t successes = 0;
  size_t errors = 0;
  std::string error;
  BOOST_REQUIRE(user.decryptHybridMessage(
      ackName,
      envelope.WireEncode(), providerName, ndn::Name(SERVICE), ndn::Name("no-dkey"),
      [&] (const ndn::Buffer&) { ++successes; },
      [&] (const std::string& reason) { ++errors; error = reason; }));
  const auto deadline = ndn::time::steady_clock::now() + ndn::time::seconds(1);
  while (errors == 0 && ndn::time::steady_clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(-1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_CHECK_EQUAL(wrapperInterests, 1U);
  BOOST_CHECK_EQUAL(successes, 0U);
  BOOST_CHECK_EQUAL(errors, 1U);
  BOOST_CHECK(error.find("decryption key") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(PersistedRuntimeStatusSurvivesRuntimeRestart)
{
  // RV-I32 (FR-039): with NDNSF_PERSIST_RUNTIME_STATE enabled, statuses
  // accepted over the live Controller fetch path are persisted; a second
  // runtime construction of the same identity re-verifies and seeds them
  // offline, then a bounded online confirmation fetch converges on the
  // Controller's authoritative (advanced) status.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-restart-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);
  const auto runtimeStateDir = std::filesystem::temp_directory_path() /
                               ("ndnsf-restart-runtime-" +
                                std::to_string(::getpid()));
  std::filesystem::remove_all(runtimeStateDir);
  ::setenv("NDNSF_RUNTIME_STATE_DIR", runtimeStateDir.c_str(), 1);
  ::setenv("NDNSF_PERSIST_RUNTIME_STATE", "1", 1);

  ndn::KeyChain controllerKeys;
  const auto controller = controllerKeys.createIdentity(
      ndn::Name("/controller/spec179-restart"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto runtimeAaCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);

  // Use the identities authorized by examples/hello.policies: the controller
  // issues zero permissions to identities outside its authorization tables,
  // and the automatic PolicyStatus fetch is driven by installed permissions.
  const ndn::Name userName("/example/hello/user");
  const ndn::Name providerName("/example/hello/provider");
  const ndn::Name serviceName("/HELLO");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, userName);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, providerName);
  ndn::KeyChain runtimeKeys;

  ControllerVersion firstVersion;
  ControllerVersion advancedVersion;
  {
    // First runtime construction: accept statuses over the live fetch path,
    // which is the only accept path that persists.
    ndn::DummyClientFace runtimeFaceA(runtimeKeys);
    ndn_service_framework::test::IsolatedTestTrust fixedTrust24{
      controllerCert, runtimeAaCert, userCert, providerCert};
    ServiceUser userA(ServiceUser::LocalMockTag{}, runtimeFaceA,
                      ndn::Name("/spec179/restart"), userCert,
                      runtimeAaCert, fixedTrust24.schemaPath());
    ndn_service_framework::test::IsolatedTestTrust fixedTrust25{
      controllerCert, runtimeAaCert, userCert, providerCert};
    ServiceProvider providerA(ServiceProvider::LocalMockTag{}, runtimeFaceA,
                              ndn::Name("/spec179/restart"), providerCert,
                              runtimeAaCert, fixedTrust25.schemaPath());
    userA.useSigningKeyChainForTest(runtimeKeys);
    providerA.useSigningKeyChainForTest(runtimeKeys);
    userA.setUseTokens(false);
    providerA.setUseTokens(false);
    auto interestRelay = runtimeFaceA.onSendInterest.connect(
        [&] (const ndn::Interest& interest) {
          controllerFace.receive(interest);
        });
    auto dataRelay = controllerFace.onSendData.connect(
        [&] (const ndn::Data& data) {
          runtimeFaceA.receive(data);
        });
    const auto pumpA = [&] {
      const auto deadline = ndn::time::steady_clock::now() +
                            ndn::time::milliseconds(800);
      do {
        runtimeFaceA.processEvents(ndn::time::milliseconds(-1));
        controllerFace.processEvents(ndn::time::milliseconds(-1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } while (ndn::time::steady_clock::now() < deadline);
    };
    ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
    pumpA();
    userA.fetchPermissionsFromController(controllerPrefix);
    providerA.fetchPermissionsFromController(controllerPrefix);
    pumpA();
    firstVersion = serviceController.getControllerVersion();
    BOOST_REQUIRE(firstVersion.isValid());
    BOOST_REQUIRE(userA.getControllerVersion());
    BOOST_REQUIRE(providerA.getControllerVersion());
    BOOST_CHECK(*userA.getControllerVersion() == firstVersion);
    BOOST_CHECK(*providerA.getControllerVersion() == firstVersion);
    // The accept path must have persisted both roles' records.
    BOOST_CHECK(std::filesystem::exists(RuntimeStatusStore::defaultStorePath(
        "user", userName)));
    BOOST_CHECK(std::filesystem::exists(RuntimeStatusStore::defaultStorePath(
        "provider", providerName)));
    interestRelay.disconnect();
    dataRelay.disconnect();
  } // userA/providerA/runtimeFaceA destroyed; records remain on disk.

  // While the runtime is gone, the Controller advances its authority.
  BOOST_REQUIRE(
      serviceController.revoke(makeIdentityRevocation(userName.toUri().c_str())));
  advancedVersion = serviceController.getControllerVersion();
  BOOST_REQUIRE(advancedVersion.compare(firstVersion) > 0);

  // Second runtime construction over the same store directory.  Connect the
  // relay before constructing so the constructor's confirmation Interests
  // are forwarded.  The offline seed itself is synchronous under the
  // LocalMock trust schema, so the restored status is authority before any
  // online round-trip.
  ndn::DummyClientFace runtimeFaceB(runtimeKeys);
  auto interestRelayB = runtimeFaceB.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        controllerFace.receive(interest);
      });
  auto dataRelayB = controllerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        runtimeFaceB.receive(data);
      });
  ndn_service_framework::test::IsolatedTestTrust fixedTrust26{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceUser userB(ServiceUser::LocalMockTag{}, runtimeFaceB,
                    ndn::Name("/spec179/restart"), userCert,
                    runtimeAaCert, fixedTrust26.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust27{
    controllerCert, runtimeAaCert, userCert, providerCert};
  ServiceProvider providerB(ServiceProvider::LocalMockTag{}, runtimeFaceB,
                            ndn::Name("/spec179/restart"), providerCert,
                            runtimeAaCert, fixedTrust27.schemaPath());
  userB.useSigningKeyChainForTest(runtimeKeys);
  providerB.useSigningKeyChainForTest(runtimeKeys);
  userB.setUseTokens(false);
  providerB.setUseTokens(false);
  BOOST_REQUIRE(userB.getControllerVersion());
  BOOST_REQUIRE(providerB.getControllerVersion());
  BOOST_CHECK(*userB.getControllerVersion() == firstVersion);
  BOOST_CHECK(*providerB.getControllerVersion() == firstVersion);

  // Bounded online confirmation: the Controller's current status is
  // authority and supersedes the offline seed on both roles.
  const auto converge = [&] {
    const auto deadline = ndn::time::steady_clock::now() +
                          ndn::time::seconds(3);
    while (ndn::time::steady_clock::now() < deadline) {
      runtimeFaceB.processEvents(ndn::time::milliseconds(-1));
      controllerFace.processEvents(ndn::time::milliseconds(-1));
      const auto userConverged = userB.getControllerVersion() &&
          *userB.getControllerVersion() == advancedVersion;
      const auto providerConverged = providerB.getControllerVersion() &&
          *providerB.getControllerVersion() == advancedVersion;
      if (userConverged && providerConverged)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  };
  BOOST_REQUIRE(converge());
  BOOST_TEST_MESSAGE(
      "NDNSF_RUNTIME_RESTART restore=offline_seed"
      << " first_epoch=" << firstVersion.controllerEpoch
      << " advanced_epoch=" << advancedVersion.controllerEpoch);
  interestRelayB.disconnect();
  dataRelayB.disconnect();
  ::unsetenv("NDNSF_PERSIST_RUNTIME_STATE");
  ::unsetenv("NDNSF_RUNTIME_STATE_DIR");
  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  std::filesystem::remove_all(runtimeStateDir);
}

BOOST_AUTO_TEST_CASE(Spec201CurrentPermissionRpcPreservesOriginalAndRefreshesAfterRevocation)
{
  const auto statePath = std::filesystem::temp_directory_path() /
      ("ndnsf-spec201-controller-rpc-" + std::to_string(::getpid()) + ".bin");
  struct StateCleanup {
    std::filesystem::path path;
    bool hadPrevious;
    std::string previous;
    ~StateCleanup()
    {
      if (hadPrevious) ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", previous.c_str(), 1);
      else ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".lock", ignored);
    }
  } cleanup{statePath, std::getenv("NDNSF_CONTROLLER_GENERATION_STATE") != nullptr,
      std::getenv("NDNSF_CONTROLLER_GENERATION_STATE") ?
        std::getenv("NDNSF_CONTROLLER_GENERATION_STATE") : ""};
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keys;
  const ndn::Name producer("/controller/spec201/final"), user("/example/hello/user"),
                  provider("/spec201/controller-rpc/provider"),
                  granted("/spec201/service");
  const auto identity = keys.createIdentity(producer, ndn::RsaKeyParams(2048));
  ndn::DummyClientFace face(keys);
  ndn::ValidatorConfig validator(face);
  ServiceController controller(face, identity.getDefaultKey().getDefaultCertificate(), validator,
                               ndnsf::test::testFixturePath("controller-policies.conf").string());
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
  ServiceControllerTestAccess::ensureInternalIdentity(controller, user);
  ServiceControllerTestAccess::registerHandlersForDummyFace(controller);
  BOOST_REQUIRE(controller.getControllerVersion().isValid());
  // User permission entries require a Provider authorized for the same service.
  ServiceControllerTestAccess::ensureInternalIdentity(controller, provider);
  BOOST_REQUIRE(controller.grant(provider, granted, ndn::Name("/SERVICE").append(granted)));
  BOOST_REQUIRE(controller.grant(user, granted, ndn::Name("/PERMISSION").append(granted)));
  face.processEvents(ndn::time::milliseconds(1));
  face.sentData.clear();

  // Actual Controller-generated policy/key and the shared ABE attribute spelling.
  // In particular, canonical URI mode must never put "8=" into OpenABE's DSL.
  const std::string plaintext = "spec201 actual controller ABE";
  const auto privateKey = ServiceControllerTestAccess::abePrivateKey(controller, user);
  const auto cipher = ServiceControllerTestAccess::abeEncrypt(
      controller, ndn::Name("/PERMISSION").append(granted), plaintext);
  const auto decoded = ServiceControllerTestAccess::abeDecrypt(controller, privateKey, cipher);
  BOOST_REQUIRE_EQUAL(decoded.size(), plaintext.size());
  BOOST_CHECK(std::equal(decoded.begin(), decoded.end(), plaintext.begin()));

  const auto parameters = encodeControlQueryParameters({user, ndn::Name(), 0, 0});
  const auto makeQuery = [&] {
    ndn::Interest interest(makeControlRpcName(producer, "USER-PERMISSIONS", parameters,
                                            makeControlQueryNonce()));
    interest.setApplicationParameters(parameters);
    interest.setMustBeFresh(true);
    interest.setCanBePrefix(false);
    return interest;
  };
  const auto decode = [&] (const ndn::Data& packet) {
    EncryptedPermissionResponse encrypted;
    const auto& content = packet.getContent();
    if (content.type() == tlv::EncryptedPermissionResponseType) {
      BOOST_REQUIRE(encrypted.WireDecode(content));
    }
    else {
      auto parsed = ndn::Block::fromBuffer(ndn::span<const uint8_t>(content.value(), content.value_size()));
      BOOST_REQUIRE(std::get<0>(parsed));
      BOOST_REQUIRE(encrypted.WireDecode(std::get<1>(parsed)));
    }
    return ServiceControllerTestAccess::decryptPermissionResponse(controller, encrypted);
  };
  const auto contains = [&] (const PermissionResponse& response, const ndn::Name& service) {
    return std::any_of(response.getEntries().begin(), response.getEntries().end(),
      [&] (const PermissionEntry& entry) { return ndn::Name(entry.getServiceName()) == service; });
  };
  const auto request = makeQuery();
  BOOST_REQUIRE(parseControlRpcName(request, producer, "USER-PERMISSIONS"));
  ServiceControllerTestAccess::userPermissionsInterest(controller, request);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto original = face.sentData.back();
  BOOST_CHECK(original.getName() == request.getName());
  BOOST_REQUIRE(ndn::security::verifySignature(original, signer));
  const auto before = decode(original);
  BOOST_REQUIRE(contains(before, granted));

  auto retry = request;
  retry.refreshNonce();
  ServiceControllerTestAccess::userPermissionsInterest(controller, retry);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 2);
  BOOST_CHECK(face.sentData.back().wireEncode() == original.wireEncode());

  RevocationTarget target;
  target.kind = RevocationKind::SERVICE_AUTHORIZATION;
  target.targetIdentity = user;
  target.serviceName = granted;
  target.authorizationAttribute = ndn::Name("/PERMISSION").append(granted);
  BOOST_REQUIRE(controller.revoke(target));
  face.processEvents(ndn::time::milliseconds(1));
  face.sentData.clear();
  const auto fresh = makeQuery();
  BOOST_CHECK(fresh.getName() != request.getName());
  ServiceControllerTestAccess::userPermissionsInterest(controller, fresh);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto current = face.sentData.back();
  BOOST_CHECK(current.getName() == fresh.getName());
  BOOST_REQUIRE(ndn::security::verifySignature(current, signer));
  const auto after = decode(current);
  BOOST_CHECK(!contains(after, granted));
  BOOST_CHECK(contains(after, ndn::Name("/HELLO")));
  BOOST_CHECK(contains(after, ndn::Name("/NDNSF/DistributedRepo/Store")));
  BOOST_CHECK(after.getControllerVersion().compare(before.getControllerVersion()) > 0);
  ServiceControllerTestAccess::userPermissionsInterest(controller, fresh);
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 2);
  BOOST_CHECK(face.sentData.back().wireEncode() == current.wireEncode());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace integration_test
} // namespace ndn_service_framework
