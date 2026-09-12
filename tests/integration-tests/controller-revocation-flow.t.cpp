#include "tests/boost-test.hpp"

#include "ndn-service-framework/ControllerGenerationStore.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include "ndn-service-framework/RuntimeStatusStore.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/utils.hpp"

#include <nac-abe/algo/abe-support.hpp>

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework {

struct ServiceControllerTestAccess
{
  static void
  registerHandlersForDummyFace(ServiceController& controller)
  {
    // These cases exercise real policy handlers over an in-process DummyFace.
    // start() additionally proves a two-transport round trip through NFD and
    // cannot run against this fixture. That boundary has its own standalone
    // readiness negative cases and real-NFD test; never mark it ready here.
    controller.registerInterestHandlers();
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

  static void
  policyStatusInterest(ServiceController& controller,
                       const ndn::Interest& interest)
  {
    controller.onPolicyStatusInterest(ndn::InterestFilter("/"), interest);
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
        controller.m_aa.m_pubParams, {attribute.toUri()},
        ndn::Buffer(reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size()));
  }

  static ndn::nacabe::algo::CipherText
  abeEncrypt(const ndn::nacabe::algo::PublicParams& publicParams,
             const ndn::Name& attribute,
             const std::string& plaintext)
  {
    return ndn::nacabe::algo::ABESupport::getInstance().kpEncrypt(
        publicParams, {attribute.toUri()},
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
    const auto attributes = controller.effectiveAttributesFor(identity.toUri());
    return boost::algorithm::join(attributes, " OR ");
  }
};

namespace integration_test {
namespace {

constexpr const char* SERVICE = "/ObjectDetection/YOLOv8";

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
  status.setValidity(now - 1000, now + 60000);
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
  status.setValidity(now - 1000, now + 60000);
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

BOOST_AUTO_TEST_SUITE(ControllerRevocationFlow)

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
                                      "examples/hello.policies");
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
                                      "examples/hello.policies");

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
        return entry.getServiceName() == "/HELLO";
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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-timestamped-status",
                         "tpm-memory:spec179-controller-timestamped-status");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-timestamped-status"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       "examples/hello.policies");
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

  // Exercise the published exact-name status route as well as the in-memory
  // snapshot.  A receiver refreshes by this immutable (generation, epoch)
  // name; serving only getPolicyStatus() would not prove that the Controller
  // can actually announce the withdrawal to another node.
  const auto exactName = makePolicyStatusName(
      controllerCert.getIdentity(), service, afterVersion);
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(exactName));
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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-stale-status",
                         "tpm-memory:spec179-controller-stale-status");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-stale-status"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                       "examples/hello.policies");
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto controllerPrefix = controllerCert.getIdentity();
  const auto service = ndn::Name(SERVICE);
  const auto before = serviceController.getControllerVersion();
  const auto oldName = makePolicyStatusName(controllerPrefix, service, before);
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(oldName));
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
      serviceController, ndn::Interest(oldName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  // A forged future version is equally unavailable; only the current exact
  // version may produce a signed status Data.
  const ControllerVersion forged{after.controllerGenerationTimestamp,
                                 after.controllerEpoch + 100};
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController,
      ndn::Interest(makePolicyStatusName(controllerPrefix, service, forged)));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  const auto currentName = makePolicyStatusName(controllerPrefix, service, after);
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(currentName));
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
                                       "examples/hello.policies");

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
                                       "examples/hello.policies");
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
                                      "examples/hello.policies");
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
                                       "examples/hello.policies");
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
                                      "examples/hello.policies");

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
        return entry.getServiceName() == hello.toUri();
      }));
  BOOST_CHECK(std::any_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [&store] (const PermissionEntry& entry) {
        return entry.getServiceName() == store.toUri();
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
        return entry.getServiceName() == hello.toUri();
      }));
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [&store] (const PermissionEntry& entry) {
        return entry.getServiceName() == store.toUri();
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
                                      "examples/hello.policies");

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
                            "examples/hello.policies");
    BOOST_REQUIRE(first.getControllerVersion().isValid());

    ServiceController second(secondFace, controllerCert, secondValidator,
                             "examples/hello.policies");
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
                                      "examples/hello.policies");

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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status-valid",
                         "tpm-memory:spec179-controller-status-valid");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-valid"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      "examples/hello.policies");
  const auto signingCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  RevocationTarget revokedUser;
  revokedUser.kind = RevocationKind::IDENTITY;
  revokedUser.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_REQUIRE(serviceController.revoke(revokedUser));
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);
  BOOST_REQUIRE(serviceController.getControllerVersion().isValid());

  const ndn::Name interestName(
      "/controller/spec179-status-valid/NDNSF/POLICY-STATUS/HELLO");
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(interestName));
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
  BOOST_CHECK(data.getName().size() > interestName.size());
  const auto parsedStatusName = parsePolicyStatusName(
      ndn::Name("/controller/spec179-status-valid"), data.getName());
  BOOST_REQUIRE(parsedStatusName);
  BOOST_CHECK(parsedStatusName->serviceName == ndn::Name("/HELLO"));
  BOOST_REQUIRE(parsedStatusName->version);
  BOOST_CHECK(*parsedStatusName->version == serviceController.getControllerVersion());
  BOOST_REQUIRE(data.getSignatureInfo());
  BOOST_CHECK(data.getSignatureValue().value_size() > 0);
  BOOST_CHECK(ndn::security::verifySignature(data, signingCert));

  PolicyStatusData status;
  const auto content = data.getContent();
  auto [parsed, statusBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(status.wireDecode(statusBlock));
  BOOST_CHECK_EQUAL(status.getServiceName().toUri(), "/HELLO");
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

  // Every status publication for one ControllerVersion is the same immutable
  // version-addressable Data name.  A repeated prefix request must not create
  // an unaddressable timestamp-only variant.
  const auto firstStatusName = data.getName();
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(interestName));
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
  const auto exactName = makePolicyStatusName(
      ndn::Name("/controller/spec179-status-valid"),
      ndn::Name("/HELLO"), currentVersion);
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(exactName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  BOOST_CHECK(face.sentData.back().getName() == exactName);

  auto unavailableVersion = currentVersion;
  ++unavailableVersion.controllerEpoch;
  const auto unavailableName = makePolicyStatusName(
      ndn::Name("/controller/spec179-status-valid"),
      ndn::Name("/HELLO"), unavailableVersion);
  face.sentData.clear();
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(unavailableName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(ServiceControllerStatusHandlerStripsParametersDigestAndRejectsWrongPrefix)
{
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-status-handler-normalization-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status-normalization",
                         "tpm-memory:spec179-controller-status-normalization");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-normalization"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      "examples/hello.policies");
  const auto signer = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  std::vector<uint8_t> digest(32, 0x42);
  ndn::Name digestInterest(
      "/controller/spec179-status-normalization/NDNSF/POLICY-STATUS/HELLO");
  digestInterest.appendParametersSha256Digest(
      ndn::span<const uint8_t>(digest.data(), digest.size()));
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(digestInterest));
  face.processEvents(ndn::time::milliseconds(1));

  const ndn::Data* published = nullptr;
  for (const auto& candidate : face.sentData) {
    // ParametersSha256Digest belongs to the Interest and is not part of the
    // canonical status Data name.  The handler must strip it before parsing
    // and publishing the version-addressable status Data.
    const auto normalizedInterest = digestInterest.getPrefix(-1);
    if (normalizedInterest.isPrefixOf(candidate.getName()) &&
        candidate.getName().size() > normalizedInterest.size()) {
      published = &candidate;
      break;
    }
  }
  BOOST_REQUIRE(published != nullptr);
  BOOST_REQUIRE(published->getSignatureInfo());
  BOOST_CHECK(ndn::security::verifySignature(*published, signer));
  PolicyStatusData status;
  const auto content = published->getContent();
  auto [parsed, statusBlock] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(content.value(), content.value_size()));
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(status.wireDecode(statusBlock));
  BOOST_CHECK_EQUAL(status.getServiceName().toUri(), "/HELLO");
  BOOST_CHECK(status.validate(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())));

  face.sentData.clear();
  const auto wrongPrefix = ndn::Interest(
      ndn::Name("/not-the-controller/NDNSF/POLICY-STATUS/HELLO"));
  ServiceControllerTestAccess::policyStatusInterest(serviceController, wrongPrefix);
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
                                      "examples/hello.policies");

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
                                        "examples/hello.policies");
    RevocationTarget target = makeIdentityRevocation("/example/hello/user");
    BOOST_REQUIRE(serviceController.revoke(target));
    first = serviceController.getControllerVersion();
  }
  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController restarted(face, controllerCert, validator,
                                 "examples/hello.policies");
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
                                         "examples/hello.policies");
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
                                "examples/hello.policies");
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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status-restart",
                         "tpm-memory:spec179-controller-status-restart");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-restart"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto target = makeIdentityRevocation("/example/hello/user");
  ControllerVersion firstVersion;
  ndn::Name firstStatusName;

  {
    boost::asio::io_context io;
    ndn::DummyClientFace face(io, keyChain);
    ndn::ValidatorConfig validator(face);
    ServiceController serviceController(face, controllerCert, validator,
                                        "examples/hello.policies");
    BOOST_REQUIRE(serviceController.getControllerVersion().isValid());
    ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
    BOOST_REQUIRE(serviceController.revoke(target));
    firstVersion = serviceController.getControllerVersion();
    ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

    const auto exactName = makePolicyStatusName(
        controllerPrefix, ndn::Name(SERVICE), firstVersion);
    ServiceControllerTestAccess::policyStatusInterest(
        serviceController, ndn::Interest(exactName));
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
        serviceController, ndn::Interest(exactName));
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
                                "examples/hello.policies");
    secondVersion = restarted.getControllerVersion();
    BOOST_REQUIRE(secondVersion.isValid());
    ServiceControllerTestAccess::ensureInternalControllerSigner(restarted);
    BOOST_CHECK(secondVersion.controllerGenerationTimestamp >
                firstVersion.controllerGenerationTimestamp);
    BOOST_CHECK_EQUAL(secondVersion.controllerEpoch, 1);
    BOOST_CHECK(restarted.isRevoked(target.targetIdentity, ndn::Name(SERVICE)));
    ServiceControllerTestAccess::registerHandlersForDummyFace(restarted);

    // A cached packet from the previous generation is not an authority
    // candidate for the restarted Controller and must produce no Data.
    face.sentData.clear();
    ServiceControllerTestAccess::policyStatusInterest(
        restarted, ndn::Interest(firstStatusName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_CHECK(face.sentData.empty());

    const auto currentName = makePolicyStatusName(
        controllerPrefix, ndn::Name(SERVICE), secondVersion);
    ServiceControllerTestAccess::policyStatusInterest(
        restarted, ndn::Interest(currentName));
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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-permission-wire",
                         "tpm-memory:spec179-controller-permission-wire");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-permission-wire"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      "examples/hello.policies");

  const ndn::Name user("/example/hello/user");
  const ndn::Name provider("/example/hello/provider");
  const ndn::Name unaffectedProvider("/example/hello/provider/A");
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, user);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, provider);
  const auto unaffectedProviderCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, unaffectedProvider);
  const auto internalSigner = ServiceControllerTestAccess::ensureInternalControllerSigner(
      serviceController);
  const ndn::Name controllerPrefix("/controller/spec179-permission-wire");
  BOOST_REQUIRE(!userCert.getName().empty());
  BOOST_REQUIRE(!providerCert.getName().empty());
  BOOST_REQUIRE(!internalSigner.getName().empty());
  BOOST_REQUIRE(serviceController.getControllerVersion().isValid());
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const auto userInterestName = ServiceControllerTestAccess::userPermissionsPrefix(
      serviceController).append(user);
  const auto providerInterestName = ServiceControllerTestAccess::providerPermissionsPrefix(
      serviceController).append(provider);
  const auto unaffectedProviderInterestName =
      ServiceControllerTestAccess::providerPermissionsPrefix(serviceController)
          .append(unaffectedProvider);
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
      ndn::Interest(ServiceControllerTestAccess::userPermissionsPrefix(
          serviceController).append("/unknown/identity")));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_CHECK(face.sentData.empty());

  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, ndn::Interest(userInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto encryptedUser = decodeEncrypted(face.sentData.back());
  BOOST_CHECK_EQUAL(encryptedUser.getRecipientCertName(), userCert.getName().toUri());
  const auto userBefore = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, encryptedUser);
  BOOST_CHECK_EQUAL(userBefore.getTargetIdentity(), user.toUri());
  BOOST_CHECK(!userBefore.getEntries().empty());
  const auto& userBeforeDataName = face.sentData.back().getName();
  BOOST_CHECK(userInterestName.isPrefixOf(userBeforeDataName));
  BOOST_REQUIRE_EQUAL(userBeforeDataName.size(), userInterestName.size() + 1);
  BOOST_CHECK(userBeforeDataName[-1].isTimestamp());

  // Exercise the Provider handler on the same pre-revocation authority
  // snapshot.  The post-revocation assertion below must therefore prove that
  // a service-scoped withdrawal changes only the affected User grant.
  face.sentData.clear();
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, ndn::Interest(providerInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto providerBeforeWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK_EQUAL(providerBeforeWire.getRecipientCertName(),
                    providerCert.getName().toUri());
  const auto providerBefore = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, providerBeforeWire);
  const auto& providerBeforeDataName = face.sentData.back().getName();
  BOOST_CHECK(providerInterestName.isPrefixOf(providerBeforeDataName));
  BOOST_REQUIRE_EQUAL(providerBeforeDataName.size(), providerInterestName.size() + 1);
  BOOST_CHECK(providerBeforeDataName[-1].isTimestamp());
  BOOST_CHECK(std::any_of(
      providerBefore.getEntries().begin(), providerBefore.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/HELLO";
      }));

  // Replacing the recipient certificate must make the same ciphertext
  // undecryptable; a service-wide DKEY would not provide this boundary.
  auto wrongRecipient = encryptedUser;
  wrongRecipient.setRecipientCertName(providerCert.getName().toUri());
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
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, ndn::Interest(userInterestName));
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
        return entry.getServiceName() == "/HELLO";
      }));
  BOOST_CHECK(std::none_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/HELLO";
      }));
  BOOST_CHECK(std::any_of(
      userAfter.getEntries().begin(), userAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/NDNSF/DistributedRepo/Store";
      }));

  // The Provider handler uses the same recipient-specific encryption and
  // keeps an unrelated service grant after a service-scoped withdrawal.
  face.sentData.clear();
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, ndn::Interest(providerInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto providerWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK_EQUAL(providerWire.getRecipientCertName(), providerCert.getName().toUri());
  const auto providerAfter = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, providerWire);
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/HELLO";
      }));
  BOOST_CHECK(std::any_of(
      providerAfter.getEntries().begin(), providerAfter.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/NDNSF/DistributedRepo/Store";
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
  ServiceControllerTestAccess::userPermissionsInterest(
      serviceController, ndn::Interest(userInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto identityRevokedUserWire = decodeEncrypted(face.sentData.back());
  const auto identityRevokedUser = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, identityRevokedUserWire);
  const auto& identityRevokedDataName = face.sentData.back().getName();
  BOOST_CHECK(userInterestName.isPrefixOf(identityRevokedDataName));
  BOOST_REQUIRE_EQUAL(identityRevokedDataName.size(), userInterestName.size() + 1);
  BOOST_CHECK(identityRevokedDataName[-1].isTimestamp());
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
      ServiceControllerTestAccess::certificateDigest(providerCert);
  BOOST_REQUIRE(serviceController.revoke(providerCertificate));
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, ndn::Interest(providerInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto revokedProviderWire = decodeEncrypted(face.sentData.back());
  const auto revokedProvider = ServiceControllerTestAccess::decryptPermissionResponse(
      serviceController, revokedProviderWire);
  const auto& revokedProviderDataName = face.sentData.back().getName();
  BOOST_CHECK(providerInterestName.isPrefixOf(revokedProviderDataName));
  BOOST_REQUIRE_EQUAL(revokedProviderDataName.size(), providerInterestName.size() + 1);
  BOOST_CHECK(revokedProviderDataName[-1].isTimestamp());
  BOOST_CHECK(revokedProvider.getEntries().empty());

  // Certificate-only withdrawal must not over-revoke another identity that
  // uses the same service.  Checking the affected Provider's empty snapshot
  // alone would not establish this live issuance control.
  face.sentData.clear();
  ServiceControllerTestAccess::providerPermissionsInterest(
      serviceController, ndn::Interest(unaffectedProviderInterestName));
  face.processEvents(ndn::time::milliseconds(1));
  BOOST_REQUIRE_EQUAL(face.sentData.size(), 1);
  const auto unaffectedProviderWire = decodeEncrypted(face.sentData.back());
  BOOST_CHECK_EQUAL(unaffectedProviderWire.getRecipientCertName(),
                    unaffectedProviderCert.getName().toUri());
  const auto unaffectedProviderResponse =
      ServiceControllerTestAccess::decryptPermissionResponse(
          serviceController, unaffectedProviderWire);
  BOOST_CHECK(std::any_of(
      unaffectedProviderResponse.getEntries().begin(),
      unaffectedProviderResponse.getEntries().end(),
      [] (const PermissionEntry& entry) {
        return entry.getServiceName() == "/HELLO";
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

  ndn::KeyChain keyChain("pib-memory:spec179-controller-status-signature",
                         "tpm-memory:spec179-controller-status-signature");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-status-signature"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace face(keyChain);
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      "examples/hello.policies");
  const auto signingCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);
  const auto wrongCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, ndn::Name("/controller/spec179-wrong-signer"));
  ServiceControllerTestAccess::registerHandlersForDummyFace(serviceController);

  const ndn::Name interestName(
      "/controller/spec179-status-signature/NDNSF/POLICY-STATUS/HELLO");
  ServiceControllerTestAccess::policyStatusInterest(
      serviceController, ndn::Interest(interestName));
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
           << "  filter { type name regex ^<spec179><>*$ }\n"
           << "  checker { type hierarchical sig-type rsa-sha256 }\n"
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

  const ndn::Name statusName(
      "/spec179/trusted-controller/NDNSF/POLICY-STATUS/"
      "ObjectDetection/YOLOv8/v=1/epoch/1");
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
           << "  filter { type name regex ^<spec179><>*$ }\n"
           << "  checker { type hierarchical sig-type rsa-sha256 }\n"
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

  const ndn::Name statusName(
      "/spec179/hier/ca/controller/NDNSF/POLICY-STATUS/"
      "ObjectDetection/YOLOv8/v=1/epoch/1");
  const ndn::Name rogueStatusName(
      "/spec179/hier/ca/controller/rogue/NDNSF/POLICY-STATUS/"
      "ObjectDetection/YOLOv8/v=1/epoch/1");

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
                                      "examples/hello.policies");

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

  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/controller-runtime"), userCert, aaCert,
                   "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/controller-runtime"), providerCert,
                           aaCert, "examples/trust-any.conf");
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

  const auto initialVersion = installCurrentControllerStatus();
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
                                      "examples/hello.policies");
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

  ndn::KeyChain runtimeKeys;
  ndn::DummyClientFace runtimeFace(runtimeKeys);
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/live-refresh"), userCert,
                   runtimeAaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/live-refresh"), providerCert,
                           runtimeAaCert, "examples/trust-any.conf");
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
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      "examples/hello.policies");
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

  ndn::KeyChain runtimeKeys;
  ndn::DummyClientFace runtimeFace(runtimeKeys);
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/grant-fetch"), userCert,
                   runtimeAaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/grant-fetch"), providerCert,
                           runtimeAaCert, "examples/trust-any.conf");
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
  pump();
  BOOST_CHECK(user.isNacConsumerReadyForTest());

  size_t grantedUserDkeyFetches = 0;
  size_t providerDkeyFetches = 0;
  auto dkeyCounter = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        const auto uri = interest.getName().toUri();
        if (uri.find("/DKEY") == std::string::npos) {
          return;
        }
        // NAC-ABE appends the raw key-name wire block after /DKEY, so each
        // plain-text identity component is preceded by escaped TLV type and
        // length bytes that full-URI matching would never see.  Match the
        // clean trailing leaf of each tracked identity instead.
        const auto grantedUserLeaf = grantedUser.get(-1).toUri();
        const auto unaffectedProviderLeaf = unaffectedProvider.get(-1).toUri();
        if (uri.find(grantedUserLeaf) != std::string::npos) {
          ++grantedUserDkeyFetches;
        }
        if (uri.find(unaffectedProviderLeaf) != std::string::npos) {
          ++providerDkeyFetches;
        }
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
  ndn::DummyClientFace::Options controllerFaceOptions;
  controllerFaceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace controllerFace(controllerKeys, controllerFaceOptions);
  ndn::ValidatorConfig controllerValidator(controllerFace);
  ServiceController serviceController(controllerFace, controllerCert,
                                      controllerValidator,
                                      "examples/hello.policies");
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
  ServiceUser user(ServiceUser::LocalMockTag{}, runtimeFace,
                   ndn::Name("/spec179/reverse-order"), userCert,
                   runtimeAaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, runtimeFace,
                           ndn::Name("/spec179/reverse-order"), providerCert,
                           runtimeAaCert, "examples/trust-any.conf");
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
  auto dkeyCounter = runtimeFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        const auto uri = interest.getName().toUri();
        if (uri.find("/DKEY") == std::string::npos) {
          return;
        }
        const auto grantedUserLeaf = grantedUser.get(-1).toUri();
        const auto unaffectedProviderLeaf = unaffectedProvider.get(-1).toUri();
        if (uri.find(grantedUserLeaf) != std::string::npos) {
          ++grantedUserDkeyFetches;
        }
        if (uri.find(unaffectedProviderLeaf) != std::string::npos) {
          ++providerDkeyFetches;
        }
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
                                      "examples/hello.policies");
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
                                   validator, "examples/hello.policies");
      ServiceControllerTestAccess::ensureInternalControllerSigner(controller);
      const ndn::Name user("/spec179/pending-rotation/user");
      ServiceControllerTestAccess::ensureInternalIdentity(controller, user);
      BOOST_REQUIRE(controller.grant(user, ndn::Name(SERVICE),
                                     ndn::Name("/PERMISSION").append(ndn::Name(SERVICE))));
      const auto before = controller.getPolicyStatus(ndn::Name(SERVICE));
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
      BOOST_CHECK_EQUAL(controller.getPolicyStatus(ndn::Name(SERVICE))
                            .getRevocations().size(), 1U);
      ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
      const auto publishedBeforeRecovery = controller.getPolicyStatus(ndn::Name(SERVICE));
      BOOST_CHECK(recover());
      const auto recovered = controller.getPolicyStatus(ndn::Name(SERVICE));
      BOOST_CHECK(recovered.getAbePublicParametersDigest() !=
                  before.getAbePublicParametersDigest());
      // Peers may have accepted the failure status. New parameter identity
      // at the same version is conflicting immutable authority, not recovery.
      BOOST_CHECK(recovered.getControllerVersion().compare(
                      publishedBeforeRecovery.getControllerVersion()) > 0);
      RevocationState runtime{ndn::Name(SERVICE)};
      const auto now = static_cast<uint64_t>(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now()
                                        .time_since_epoch()).count());
      BOOST_CHECK(runtime.acceptStatus(publishedBeforeRecovery, now));
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
                                       "examples/hello.policies");

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

  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec179/runtime"),
                   userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/runtime"), providerCert, aaCert,
                           "examples/trust-any.conf");

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
    ServiceUser user(ServiceUser::LocalMockTag{}, face, groupPrefix,
                     userCert, aaCert, "examples/trust-any.conf");
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
  ServiceUser restarted(ServiceUser::LocalMockTag{}, restartedFace, groupPrefix,
                         userCert, aaCert, "examples/trust-any.conf");
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
  ServiceUser user(ServiceUser::LocalMockTag{}, face, groupPrefix, userCert,
                   aaCert, "examples/trust-any.conf");
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
  const auto userCert = keyChain.createIdentity(userName, ndn::RsaKeyParams(2048))
                          .getDefaultKey().getDefaultCertificate();
  const auto providerCert =
      keyChain.createIdentity(providerName, ndn::RsaKeyParams(2048))
          .getDefaultKey().getDefaultCertificate();
  const auto aaCert = keyChain.createIdentity(aaName, ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  ServiceUser user(ServiceUser::LocalMockTag{}, face, ndn::Name("/spec179/runtime-scope"),
                   userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/runtime-scope"), providerCert, aaCert,
                           "examples/trust-any.conf");
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

  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/targeted"), providerCert,
                           aaCert, "examples/trust-any.conf");
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

  ServiceUser user(ServiceUser::LocalMockTag{}, face,
                   ndn::Name("/spec179/no-status"), userCert, aaCert,
                   "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{}, face,
                           ndn::Name("/spec179/no-status"), providerCert,
                           aaCert, "examples/trust-any.conf");
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
  ServiceUser user(face, ndn::Name("/spec179/unprovisioned/group"),
                   cert(userName), aa, "examples/trust-any.conf");
  ServiceProvider provider(face, ndn::Name("/spec179/unprovisioned/group"),
                           cert(providerName), aa, "examples/trust-any.conf");
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
  ServiceUser user(face, ndn::Name("/spec179/grant-pending/group"),
                   cert(userName), aa, "examples/trust-any.conf");
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

  // A named MessageKey consume fails synchronously when no DKEY exists.
  // Preserve the error callback across construction of the success closure.
  HybridMessageEnvelope envelope;
  envelope.setKeyId("0000000000000001");
  envelope.setEpochId("0000000000000001");
  envelope.setMessageType("ACK");
  envelope.setNonce(ndn::Buffer(12, 0));
  envelope.setCipherText(ndn::Buffer(1, 0));
  envelope.setAuthTag(ndn::Buffer(16, 0));
  size_t successes = 0;
  size_t errors = 0;
  std::string error;
  BOOST_REQUIRE(user.decryptHybridMessage(
      makeRequestAckNameV2(providerName, userName, ndn::Name(SERVICE), ndn::Name("no-dkey")),
      envelope.WireEncode(), [&] (const ndn::Buffer&) { ++successes; },
      [&] (const std::string& reason) { ++errors; error = reason; }));
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
                                      "examples/hello.policies");
  const auto controllerPrefix = controllerCert.getIdentity();
  const auto runtimeAaCert =
      ServiceControllerTestAccess::ensureInternalControllerSigner(serviceController);

  // Use the identities authorized by examples/hello.policies: the controller
  // issues zero permissions to identities outside its authorization tables,
  // and the automatic PolicyStatus fetch is driven by installed permissions.
  const ndn::Name userName("/example/hello/user");
  const ndn::Name providerName("/example/hello/provider");
  const ndn::Name serviceName("/HELLO");
  ndn::KeyChain runtimeKeys;
  const auto userCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, userName);
  const auto providerCert = ServiceControllerTestAccess::ensureInternalIdentity(
      serviceController, providerName);

  ControllerVersion firstVersion;
  ControllerVersion advancedVersion;
  {
    // First runtime construction: accept statuses over the live fetch path,
    // which is the only accept path that persists.
    ndn::DummyClientFace runtimeFaceA(runtimeKeys);
    ServiceUser userA(ServiceUser::LocalMockTag{}, runtimeFaceA,
                      ndn::Name("/spec179/restart"), userCert,
                      runtimeAaCert, "examples/trust-any.conf");
    ServiceProvider providerA(ServiceProvider::LocalMockTag{}, runtimeFaceA,
                              ndn::Name("/spec179/restart"), providerCert,
                              runtimeAaCert, "examples/trust-any.conf");
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
  ServiceUser userB(ServiceUser::LocalMockTag{}, runtimeFaceB,
                    ndn::Name("/spec179/restart"), userCert,
                    runtimeAaCert, "examples/trust-any.conf");
  ServiceProvider providerB(ServiceProvider::LocalMockTag{}, runtimeFaceB,
                            ndn::Name("/spec179/restart"), providerCert,
                            runtimeAaCert, "examples/trust-any.conf");
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace integration_test
} // namespace ndn_service_framework
