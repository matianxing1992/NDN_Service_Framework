#include "tests/boost-test.hpp"

#include "ndn-service-framework/ControllerGenerationStore.hpp"
#include "ndn-service-framework/ControllerVersion.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/RevocationState.hpp"

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace ndn_service_framework::test {

namespace {

PolicyStatusData
makeStatus(const ControllerVersion& version,
           const std::vector<RevocationTarget>& revocations = {})
{
  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(version);
  status.setValidity(900, 3000);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("sig"), 3));
  for (const auto& revocation : revocations)
    status.addRevocation(revocation);
  return status;
}

RevocationTarget
identityRevocation(const char* identity)
{
  RevocationTarget target;
  target.kind = RevocationKind::IDENTITY;
  target.targetIdentity = ndn::Name(identity);
  return target;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ControllerRevocationPolicy)

BOOST_AUTO_TEST_CASE(ControllerVersionOrdersAndRoundTrips)
{
  ControllerVersion first{1000, 7};
  ControllerVersion same{1000, 7};
  ControllerVersion newerEpoch{1000, 8};
  ControllerVersion newerGeneration{1001, 1};
  ControllerVersion older{999, 99};

  BOOST_REQUIRE(first.isValid());
  BOOST_CHECK(first == same);
  BOOST_CHECK(first.compare(newerEpoch) < 0);
  BOOST_CHECK(newerEpoch.compare(newerGeneration) < 0);
  BOOST_CHECK(newerGeneration.compare(older) > 0);
  const ControllerVersion zeroGeneration{0, 1};
  const ControllerVersion zeroEpoch{1000, 0};
  BOOST_CHECK(!zeroGeneration.isValid());
  BOOST_CHECK(!zeroEpoch.isValid());

  const auto wire = first.wireEncode();
  ControllerVersion decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == first);

  ControllerVersion malformed;
  BOOST_CHECK(!malformed.wireDecode(ndn::Block(ControllerVersion::TYPE)));
}

BOOST_AUTO_TEST_CASE(PolicyStatusRequiresTypedRevocationTargets)
{
  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(ControllerVersion{1000, 2});
  status.setValidity(900, 2000);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("sig"), 3));

  RevocationTarget identity;
  identity.kind = RevocationKind::IDENTITY;
  identity.targetIdentity = ndn::Name("/user/alice");
  BOOST_REQUIRE(identity.isValid());
  status.addRevocation(identity);

  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  certificate.certificateDigest = "sha256:certificate";
  BOOST_REQUIRE(certificate.isValid());
  status.addRevocation(certificate);

  RevocationTarget service;
  service.kind = RevocationKind::SERVICE_AUTHORIZATION;
  service.targetIdentity = ndn::Name("/provider/p1");
  service.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  service.authorizationAttribute = ndn::Name("/SERVICE/ObjectDetection/YOLOv8");
  BOOST_REQUIRE(service.isValid());
  status.addRevocation(service);

  BOOST_REQUIRE(status.validate(1000));
  const auto wire = status.wireEncode();
  PolicyStatusData decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded.validate(1000));
  BOOST_CHECK_EQUAL(decoded.getRevocations().size(), 3);

  RevocationTarget contradictory;
  contradictory.kind = RevocationKind::IDENTITY;
  contradictory.targetIdentity = ndn::Name("/user/bob");
  contradictory.serviceName = ndn::Name("/only-one-service");
  BOOST_CHECK(!contradictory.isValid());

  PolicyStatusData wrongScope = status;
  wrongScope.setServiceName(ndn::Name("/OtherService"));
  BOOST_CHECK(!wrongScope.validate(1000));
}

BOOST_AUTO_TEST_CASE(PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates)
{
  const auto version = ControllerVersion{1500, 1};
  const auto identity = identityRevocation("/user/alice");
  auto status = makeStatus(version, {identity});

  auto badInterval = status;
  badInterval.setValidity(2000, 2000);
  BOOST_CHECK(!badInterval.validate(2000));

  auto expired = status;
  expired.setValidity(100, 200);
  BOOST_CHECK(!expired.validate(200));

  auto badDigest = status;
  badDigest.setPolicyDigest("sha256:not-a-64-byte-digest");
  BOOST_CHECK(!badDigest.validate(1000));

  auto missingControllerCertificate = status;
  missingControllerCertificate.setControllerCertificate({});
  BOOST_CHECK(!missingControllerCertificate.validate(1000));

  auto zeroVersion = status;
  zeroVersion.setControllerVersion(ControllerVersion{0, 1});
  BOOST_CHECK(!zeroVersion.validate(1000));

  auto duplicate = status;
  duplicate.addRevocation(identity);
  BOOST_CHECK(!duplicate.validate(1000));

  auto malformedTarget = status;
  RevocationTarget malformed = identity;
  malformed.serviceName = ndn::Name("/unexpected");
  malformedTarget.addRevocation(malformed);
  BOOST_CHECK(!malformedTarget.validate(1000));
}

BOOST_AUTO_TEST_CASE(PolicyStatusDoesNotBecomeValidBeforeItsValidityWindow)
{
  auto status = makeStatus(ControllerVersion{1550, 1});
  status.setValidity(2000, 3000);
  BOOST_CHECK(!status.validate(1999));
  BOOST_CHECK(status.validate(2000));
  BOOST_CHECK(status.validate(2999));
  BOOST_CHECK(!status.validate(3000));

  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_CHECK(!state.acceptStatus(status, 1999));
  BOOST_REQUIRE(state.acceptStatus(status, 2000));
}

BOOST_AUTO_TEST_CASE(PolicyStatusWirePreservesValidityTimestamps)
{
  // The validity interval is part of the Controller-signed status contract.
  // It must survive the wire round trip exactly; otherwise a restarted or
  // refreshed runtime could accept a status outside the Controller's window.
  auto status = makeStatus(ControllerVersion{1560, 4});
  constexpr uint64_t validFrom = 1700000000123ULL;
  constexpr uint64_t validUntil = 1700003600456ULL;
  status.setValidity(validFrom, validUntil);

  const auto wire = status.wireEncode();
  PolicyStatusData decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded.getControllerVersion() == status.getControllerVersion());
  BOOST_CHECK_EQUAL(decoded.getValidFromMs(), validFrom);
  BOOST_CHECK_EQUAL(decoded.getValidUntilMs(), validUntil);
  BOOST_CHECK(decoded.validate(validFrom));
  BOOST_CHECK(decoded.validate(validUntil - 1));
  BOOST_CHECK(!decoded.validate(validFrom - 1));
  BOOST_CHECK(!decoded.validate(validUntil));
}

BOOST_AUTO_TEST_CASE(PolicyStatusBindsAbeGenerationAsAnExactPair)
{
  auto status = makeStatus(ControllerVersion{1561, 1});
  const ndn::Name parametersName(
      "/controller/KEY/params/CP-ABE/v=1561001");
  const std::string parametersDigest =
      "sha256:abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
  status.setAbePublicParametersName(parametersName);
  status.setAbePublicParametersDigest(parametersDigest);

  const auto wire = status.wireEncode();
  PolicyStatusData decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded.getAbePublicParametersName() == parametersName);
  BOOST_CHECK_EQUAL(decoded.getAbePublicParametersDigest(), parametersDigest);
  BOOST_CHECK(decoded.validate(1000));

  auto missingDigest = status;
  missingDigest.setAbePublicParametersDigest({});
  BOOST_CHECK(!missingDigest.validate(1000));

  auto missingName = status;
  missingName.setAbePublicParametersName({});
  BOOST_CHECK(!missingName.validate(1000));

  auto malformedDigest = status;
  malformedDigest.setAbePublicParametersDigest("sha256:not-a-digest");
  BOOST_CHECK(!malformedDigest.validate(1000));
}

BOOST_AUTO_TEST_CASE(GlobalCertificateRevocationDoesNotOverRevokeReplacement)
{
  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  certificate.certificateDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(state.acceptStatus(
      makeStatus(ControllerVersion{1600, 1}, {certificate}), 1000));

  const AuthorizationSubject oldUser{
      ndn::Name("/user/alice"), certificate.certificateDigest,
      ndn::Name("/ObjectDetection/YOLOv8"), {}};
  const AuthorizationSubject oldProvider{
      ndn::Name("/provider/p1"), certificate.certificateDigest,
      ndn::Name("/ObjectDetection/YOLOv8"), {}};
  const AuthorizationSubject replacement{
      ndn::Name("/user/alice"), "sha256:replacement",
      ndn::Name("/ObjectDetection/YOLOv8"), {}};
  BOOST_CHECK(!state.authorize(oldUser, ProtectedTransition::DISCOVERY, 1000).allowed);
  BOOST_CHECK(!state.authorize(oldProvider, ProtectedTransition::PROVIDER_EXECUTION,
                               1000).allowed);
  BOOST_CHECK(state.authorize(replacement, ProtectedTransition::DISCOVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(GlobalCertificateRevocationMatchesDigestAcrossIdentitiesAndServices)
{
  // An omitted target identity deliberately makes a certificate revocation
  // global.  The digest, not the identity or service, is the authority key;
  // a replacement certificate must remain usable in every service scope.
  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  certificate.certificateDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  BOOST_REQUIRE(certificate.targetIdentity.empty());

  const auto service = ndn::Name("/ObjectDetection/YOLOv8");
  const auto otherService = ndn::Name("/OtherService");
  RevocationState serviceState(service);
  BOOST_REQUIRE(serviceState.acceptStatus(
      makeStatus(ControllerVersion{1610, 1}, {certificate}), 1000));

  const std::array<AuthorizationSubject, 3> affected{{
      {ndn::Name("/user/alice"), certificate.certificateDigest, service, {}},
      {ndn::Name("/provider/p1"), certificate.certificateDigest, service, {}},
      {ndn::Name("/user/alice"), certificate.certificateDigest, otherService, {}},
  }};
  for (const auto& subject : affected) {
    // The service-scoped state rejects a subject from another service before
    // applying the target, while both in-scope subjects are denied by the
    // same global digest target.
    const auto decision = serviceState.authorize(subject,
                                                   ProtectedTransition::DISCOVERY,
                                                   1000);
    BOOST_CHECK(!decision.allowed);
  }

  const AuthorizationSubject replacementInService{
      ndn::Name("/user/alice"), "sha256:replacement", service, {}};
  BOOST_CHECK(serviceState.authorize(
      replacementInService, ProtectedTransition::DISCOVERY, 1000).allowed);

  // A status for another service carries the same global target and must
  // produce the same denial there, independent of the original identity.
  auto otherStatus = makeStatus(ControllerVersion{1610, 1}, {certificate});
  otherStatus.setServiceName(otherService);
  RevocationState otherState(otherService);
  BOOST_REQUIRE(otherState.acceptStatus(otherStatus, 1000));
  BOOST_CHECK(!otherState.authorize(
      {ndn::Name("/provider/p2"), certificate.certificateDigest, otherService, {}},
      ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
  BOOST_CHECK(otherState.authorize(
      {ndn::Name("/provider/p2"), "sha256:replacement", otherService, {}},
      ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(BoundCertificateRevocationDoesNotOverRevokeAnotherIdentity)
{
  // A certificate target with an identity is narrower than a global digest
  // target.  The digest may be shared by another identity, which must remain
  // authorized until that identity or certificate is separately withdrawn.
  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  certificate.targetIdentity = ndn::Name("/user/alice");
  certificate.certificateDigest = "sha256:bound-alice";

  const auto service = ndn::Name("/ObjectDetection/YOLOv8");
  RevocationState state(service);
  BOOST_REQUIRE(state.acceptStatus(
      makeStatus(ControllerVersion{1620, 1}, {certificate}), 1000));

  const AuthorizationSubject revoked{
      ndn::Name("/user/alice"), "sha256:bound-alice", service, {}};
  const AuthorizationSubject sameDigestOtherIdentity{
      ndn::Name("/user/bob"), "sha256:bound-alice", service, {}};
  const AuthorizationSubject replacement{
      ndn::Name("/user/alice"), "sha256:new-alice", service, {}};

  BOOST_CHECK(!state.authorize(
      revoked, ProtectedTransition::DISCOVERY, 1000).allowed);
  BOOST_CHECK(state.authorize(
      sameDigestOtherIdentity, ProtectedTransition::DISCOVERY, 1000).allowed);
  BOOST_CHECK(state.authorize(
      replacement, ProtectedTransition::DISCOVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(RevocationStateRejectsAtStatusValidityEnd)
{
  // PolicyStatus validity is half-open: validFrom is accepted, validUntil is
  // not.  The exact boundary must fail closed for every protected transition,
  // rather than allowing a revoked/expired status for one extra request.
  auto status = makeStatus(ControllerVersion{1630, 1});
  status.setValidity(1000, 2000);
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(state.acceptStatus(status, 1000));

  const AuthorizationSubject subject{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    BOOST_CHECK(state.authorize(subject, transition, 1999).allowed);
    const auto decision = state.authorize(subject, transition, 2000);
    BOOST_CHECK(!decision.allowed);
    BOOST_CHECK_EQUAL(decision.reason, "controller_status_expired");
  }
}

BOOST_AUTO_TEST_CASE(EqualVersionConflictingStatusCannotReplaceAuthority)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto version = ControllerVersion{1650, 1};
  BOOST_REQUIRE(state.acceptStatus(makeStatus(version), 1000));
  auto conflicting = makeStatus(version, {identityRevocation("/user/alice")});
  BOOST_CHECK(!state.acceptStatus(conflicting, 1000));

  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  BOOST_CHECK(state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(PolicyStatusWireRejectsMalformedRevocationTargets)
{
  ndn::Block unknownKind(PolicyStatusData::RevocationTargetType);
  unknownKind.push_back(ndn::makeNonNegativeIntegerBlock(0xF719, 99));
  unknownKind.encode();
  RevocationTarget decoded;
  BOOST_CHECK(!decoded.wireDecode(unknownKind));

  ndn::Block duplicateKind(PolicyStatusData::RevocationTargetType);
  duplicateKind.push_back(ndn::makeNonNegativeIntegerBlock(
      0xF719, static_cast<uint64_t>(RevocationKind::IDENTITY)));
  duplicateKind.push_back(ndn::makeNonNegativeIntegerBlock(
      0xF719, static_cast<uint64_t>(RevocationKind::IDENTITY)));
  duplicateKind.encode();
  BOOST_CHECK(!decoded.wireDecode(duplicateKind));

  ndn::Block wrongType(0xF700);
  wrongType.encode();
  BOOST_CHECK(!decoded.wireDecode(wrongType));
}

BOOST_AUTO_TEST_CASE(PolicyStatusWireRejectsNonCanonicalFieldOrder)
{
  auto status = makeStatus(ControllerVersion{1750, 1},
                           {identityRevocation("/user/alice")});
  auto wire = status.wireEncode();
  wire.parse();
  const auto& elements = wire.elements();
  BOOST_REQUIRE_EQUAL(elements.size(), 8);

  // PolicyStatusData has a deliberately canonical field order.  Reordering
  // the version and service fields must not create a second accepted encoding
  // of the same authority snapshot.
  ndn::Block reordered(PolicyStatusData::TYPE);
  reordered.push_back(elements[1]);
  reordered.push_back(elements[0]);
  for (size_t i = 2; i < elements.size(); ++i)
    reordered.push_back(elements[i]);
  reordered.encode();

  PolicyStatusData decoded;
  BOOST_CHECK(!decoded.wireDecode(reordered));
}

BOOST_AUTO_TEST_CASE(RevocationTargetValidationMatrix)
{
  RevocationTarget identity;
  BOOST_CHECK(!identity.isValid());
  identity.targetIdentity = ndn::Name("/user/alice");
  BOOST_CHECK(identity.isValid());
  identity.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  BOOST_CHECK(!identity.isValid());
  identity.serviceName = {};
  identity.certificateDigest = "sha256:certificate";
  BOOST_CHECK(!identity.isValid());

  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  BOOST_CHECK(!certificate.isValid());
  certificate.certificateDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  BOOST_CHECK(certificate.isValid());
  certificate.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  BOOST_CHECK(!certificate.isValid());

  RevocationTarget service;
  service.kind = RevocationKind::SERVICE_AUTHORIZATION;
  service.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  BOOST_CHECK(!service.isValid());
  service.targetIdentity = ndn::Name("/provider/p1");
  BOOST_CHECK(!service.isValid());
  service.authorizationAttribute = ndn::Name("/SERVICE/ObjectDetection/YOLOv8");
  BOOST_CHECK(service.isValid());
  service.certificateDigest = "sha256:certificate";
  BOOST_CHECK(!service.isValid());

  RevocationTarget unknown;
  unknown.kind = static_cast<RevocationKind>(99);
  unknown.targetIdentity = ndn::Name("/user/alice");
  BOOST_CHECK(!unknown.isValid());

  BOOST_CHECK_THROW(unknown.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RevocationStateCoversAllProtectedTransitions)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto initial = makeStatus(ControllerVersion{2000, 1});
  BOOST_REQUIRE(state.acceptStatus(initial, 1000));
  state.clearInvalidationEvidence();

  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  const AuthorizationSubject bob{
      ndn::Name("/user/bob"), "sha256:bob", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT})
    BOOST_CHECK(state.authorize(alice, transition, 1000).allowed);

  const auto revoked = makeStatus(
      ControllerVersion{2000, 2}, {identityRevocation("/user/alice")});
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  for (const auto family : {"abe", "message-key", "targeted-token",
                            "selection-binding", "nonce", "replay"})
    BOOST_CHECK(state.cacheInvalidated(family));
  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    const auto decision = state.authorize(alice, transition, 1000);
    BOOST_CHECK(!decision.allowed);
    BOOST_CHECK(!decision.reason.empty());
  }
  BOOST_CHECK(state.authorize(bob, ProtectedTransition::SELECTION, 1000).allowed);
  BOOST_CHECK(!state.acceptStatus(initial, 1000));
  BOOST_CHECK(!state.acceptStatus(
      makeStatus(ControllerVersion{2000, 2}, {identityRevocation("/user/bob")}),
      1000));
  BOOST_CHECK(state.acceptStatus(revoked, 1000));
  BOOST_CHECK(!state.acceptStatus(revoked, 1000, false));
}

BOOST_AUTO_TEST_CASE(RevocationStateReportsRedactedTypedReasons)
{
  const auto service = ndn::Name("/ObjectDetection/YOLOv8");
  const auto identity = ndn::Name("/user/alice");
  RevocationState state(service);
  BOOST_REQUIRE(state.acceptStatus(makeStatus(
      ControllerVersion{2025, 1}, {identityRevocation(identity.toUri().c_str())}), 1000));

  const AuthorizationSubject revoked{
      identity, "sha256:alice-secret-certificate", service, {}};
  const std::array<std::pair<ProtectedTransition, const char*>, 6> transitions{{
      {ProtectedTransition::DISCOVERY, "discovery"},
      {ProtectedTransition::ACK_COLLECTION, "ack_collection"},
      {ProtectedTransition::SELECTION, "selection"},
      {ProtectedTransition::PROVIDER_EXECUTION, "provider_execution"},
      {ProtectedTransition::RESPONSE_DELIVERY, "response_delivery"},
      {ProtectedTransition::STREAM_EVENT, "stream_event"},
  }};

  for (const auto& [transition, name] : transitions) {
    const auto decision = state.authorize(revoked, transition, 1000);
    BOOST_CHECK(!decision.allowed);
    BOOST_CHECK_EQUAL(decision.reason,
                      std::string("revoked_before_") + name);
    BOOST_CHECK(decision.reason.find(identity.toUri()) == std::string::npos);
    BOOST_CHECK(decision.reason.find("alice-secret-certificate") == std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(RevocationStateDistinguishesCertificateAndServiceScope)
{
  RevocationTarget certificate;
  certificate.kind = RevocationKind::CERTIFICATE;
  certificate.targetIdentity = ndn::Name("/user/alice");
  certificate.certificateDigest = "sha256:cert-alice";

  RevocationTarget service;
  service.kind = RevocationKind::SERVICE_AUTHORIZATION;
  service.targetIdentity = ndn::Name("/provider/p1");
  service.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  service.authorizationAttribute = ndn::Name("/SERVICE/ObjectDetection/YOLOv8");

  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(state.acceptStatus(
      makeStatus(ControllerVersion{3000, 1}, {certificate, service}), 1000));
  const AuthorizationSubject revokedCertificate{
      ndn::Name("/user/alice"), "sha256:cert-alice", ndn::Name("/ObjectDetection/YOLOv8"),
      ndn::Name("/PERMISSION/ObjectDetection/YOLOv8")};
  const AuthorizationSubject replacementCertificate{
      ndn::Name("/user/alice"), "sha256:cert-new", ndn::Name("/ObjectDetection/YOLOv8"),
      ndn::Name("/PERMISSION/ObjectDetection/YOLOv8")};
  const AuthorizationSubject revokedService{
      ndn::Name("/provider/p1"), "sha256:provider", ndn::Name("/ObjectDetection/YOLOv8"),
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
  const AuthorizationSubject otherService{
      ndn::Name("/provider/p1"), "sha256:provider", ndn::Name("/OtherService"),
      ndn::Name("/SERVICE/OtherService")};
  BOOST_CHECK(!state.authorize(revokedCertificate,
                               ProtectedTransition::SELECTION, 1000).allowed);
  BOOST_CHECK(state.authorize(replacementCertificate,
                              ProtectedTransition::SELECTION, 1000).allowed);
  BOOST_CHECK(!state.authorize(revokedService,
                               ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
  BOOST_CHECK(!state.authorize(otherService,
                               ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(RevocationStateDistinguishesUseAndProvisionForDualRole)
{
  const auto service = ndn::Name("/ObjectDetection/YOLOv8");
  RevocationTarget use;
  use.kind = RevocationKind::SERVICE_AUTHORIZATION;
  use.targetIdentity = ndn::Name("/dual-role/node");
  use.serviceName = service;
  use.authorizationAttribute = ndn::Name("/PERMISSION/ObjectDetection/YOLOv8");

  RevocationState state(service);
  BOOST_REQUIRE(state.acceptStatus(
      makeStatus(ControllerVersion{3060, 1}, {use}), 1000));

  const AuthorizationSubject user{
      ndn::Name("/dual-role/node"), "sha256:dual", service,
      ndn::Name("/PERMISSION/ObjectDetection/YOLOv8")};
  const AuthorizationSubject provider{
      ndn::Name("/dual-role/node"), "sha256:dual", service,
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
  BOOST_CHECK(!state.authorize(
      user, ProtectedTransition::DISCOVERY, 1000).allowed);
  BOOST_CHECK(state.authorize(
      provider, ProtectedTransition::PROVIDER_EXECUTION, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(RevocationTargetKindsCoverEveryProtectedTransition)
{
  // Keep the Controller-authority target matrix explicit: each canonical
  // target shape must deny the named subject at every protected cut point,
  // while the corresponding replacement/unaffected subject remains live.
  const auto transitions = {ProtectedTransition::DISCOVERY,
                            ProtectedTransition::ACK_COLLECTION,
                            ProtectedTransition::SELECTION,
                            ProtectedTransition::PROVIDER_EXECUTION,
                            ProtectedTransition::RESPONSE_DELIVERY,
                            ProtectedTransition::STREAM_EVENT};

  {
    RevocationState state{ndn::Name("/ObjectDetection/YOLOv8")};
    BOOST_REQUIRE(state.acceptStatus(
        makeStatus(ControllerVersion{3050, 1},
                   {identityRevocation("/user/alice")}), 1000));
    const AuthorizationSubject revoked{
        ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
    const AuthorizationSubject unaffected{
        ndn::Name("/user/bob"), "sha256:bob", ndn::Name("/ObjectDetection/YOLOv8"), {}};
    for (const auto transition : transitions) {
      BOOST_CHECK(!state.authorize(revoked, transition, 1000).allowed);
      BOOST_CHECK(state.authorize(unaffected, transition, 1000).allowed);
    }
  }

  {
    RevocationTarget target;
    target.kind = RevocationKind::CERTIFICATE;
    target.targetIdentity = ndn::Name("/user/alice");
    target.certificateDigest = "sha256:old-alice";
    RevocationState state{ndn::Name("/ObjectDetection/YOLOv8")};
    BOOST_REQUIRE(state.acceptStatus(makeStatus(
        ControllerVersion{3050, 2}, {target}), 1000));
    const AuthorizationSubject revoked{
        ndn::Name("/user/alice"), "sha256:old-alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
    const AuthorizationSubject replacement{
        ndn::Name("/user/alice"), "sha256:new-alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
    for (const auto transition : transitions) {
      BOOST_CHECK(!state.authorize(revoked, transition, 1000).allowed);
      BOOST_CHECK(state.authorize(replacement, transition, 1000).allowed);
    }
  }

  {
    RevocationTarget target;
    target.kind = RevocationKind::SERVICE_AUTHORIZATION;
    target.targetIdentity = ndn::Name("/provider/p1");
    target.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
    target.authorizationAttribute = ndn::Name("/SERVICE/ObjectDetection/YOLOv8");
    RevocationState state{ndn::Name("/ObjectDetection/YOLOv8")};
    BOOST_REQUIRE(state.acceptStatus(makeStatus(
        ControllerVersion{3050, 3}, {target}), 1000));
    const AuthorizationSubject revoked{
        ndn::Name("/provider/p1"), "sha256:p1", ndn::Name("/ObjectDetection/YOLOv8"),
        ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
    const AuthorizationSubject unaffected{
        ndn::Name("/provider/p2"), "sha256:p2", ndn::Name("/ObjectDetection/YOLOv8"),
        ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
    for (const auto transition : transitions) {
      BOOST_CHECK(!state.authorize(revoked, transition, 1000).allowed);
      BOOST_CHECK(state.authorize(unaffected, transition, 1000).allowed);
    }
  }
}

BOOST_AUTO_TEST_CASE(RevocationStateSupportsForwardOnlyReauthorization)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto revoked = makeStatus(
      ControllerVersion{4000, 2}, {identityRevocation("/user/alice")});
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  BOOST_CHECK(!state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);

  const auto reauthorized = makeStatus(ControllerVersion{4000, 3});
  BOOST_REQUIRE(state.acceptStatus(reauthorized, 1000));
  BOOST_CHECK(state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);
  BOOST_CHECK(!state.acceptStatus(revoked, 1000));
  BOOST_CHECK(!state.authorize(alice, ProtectedTransition::DISCOVERY, 3000).allowed);
}

BOOST_AUTO_TEST_CASE(IdentityRevocationAppliesToEveryRoleForOneIdentity)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto identity = ndn::Name("/shared/user-and-provider");
  const AuthorizationSubject subject{
      identity, "sha256:shared-cert", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  BOOST_REQUIRE(state.acceptStatus(makeStatus(ControllerVersion{4050, 1}), 1000));
  BOOST_REQUIRE(state.acceptStatus(makeStatus(
      ControllerVersion{4050, 2}, {identityRevocation(identity.toUri().c_str())}), 1000));

  // The identity target is role-independent: every protected transition must
  // fail for the same certificate, including transitions normally owned by a
  // User and those owned by a Provider.
  for (const auto transition : {ProtectedTransition::DISCOVERY,
                                ProtectedTransition::ACK_COLLECTION,
                                ProtectedTransition::SELECTION,
                                ProtectedTransition::PROVIDER_EXECUTION,
                                ProtectedTransition::RESPONSE_DELIVERY,
                                ProtectedTransition::STREAM_EVENT}) {
    const auto decision = state.authorize(subject, transition, 1000);
    BOOST_CHECK(!decision.allowed);
    BOOST_CHECK(decision.reason.find("revoked_before_") == 0);
  }
}

BOOST_AUTO_TEST_CASE(InvalidStatusCannotReplaceLastAcceptedAuthority)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  const auto current = makeStatus(ControllerVersion{4075, 1});
  BOOST_REQUIRE(state.acceptStatus(current, 1000));
  state.clearInvalidationEvidence();

  auto wrongScope = makeStatus(ControllerVersion{4075, 2});
  wrongScope.setServiceName(ndn::Name("/OtherService"));
  BOOST_CHECK(!state.acceptStatus(wrongScope, 1000));
  BOOST_CHECK(state.currentVersion() == current.getControllerVersion());
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);
  BOOST_CHECK(state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);

  const auto unauthenticatedNewer = makeStatus(
      ControllerVersion{4075, 2}, {identityRevocation("/user/alice")});
  BOOST_CHECK(!state.acceptStatus(unauthenticatedNewer, 1000, false));
  BOOST_CHECK(state.currentVersion() == current.getControllerVersion());
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);
  BOOST_CHECK(state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);

  BOOST_REQUIRE(state.acceptStatus(unauthenticatedNewer, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  BOOST_CHECK(!state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(InvalidStatusPreservesAuthorityAndCacheEvidence)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto current = makeStatus(ControllerVersion{4080, 1});
  BOOST_REQUIRE(state.acceptStatus(current, 1000));
  state.clearInvalidationEvidence();

  // A newer status is not enough to invalidate local authorization.  The
  // candidate must pass signature, service-scope, validity, and wire checks
  // before the state replaces its last accepted authority.
  auto invalid = makeStatus(ControllerVersion{4080, 2},
                            {identityRevocation("/user/alice")});
  invalid.setServiceName(ndn::Name("/OtherService"));
  BOOST_CHECK(!state.acceptStatus(invalid, 1000));
  BOOST_CHECK(state.currentVersion() == current.getControllerVersion());
  BOOST_CHECK(state.currentStatus().getRevocations().empty());
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);

  auto forged = makeStatus(ControllerVersion{4080, 2},
                           {identityRevocation("/user/alice")});
  BOOST_CHECK(!state.acceptStatus(forged, 1000, false));
  BOOST_CHECK(state.currentVersion() == current.getControllerVersion());
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);

  // Only the authenticated replacement invalidates all six authorization
  // material families and makes the affected subject fail closed.
  BOOST_REQUIRE(state.acceptStatus(forged, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  const AuthorizationSubject alice{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};
  BOOST_CHECK(!state.authorize(alice, ProtectedTransition::DISCOVERY, 1000).allowed);
}

BOOST_AUTO_TEST_CASE(RevocationStateFailsClosedForMissingOrInvalidSubject)
{
  RevocationState state(ndn::Name("/ObjectDetection/YOLOv8"));
  const AuthorizationSubject complete{
      ndn::Name("/user/alice"), "sha256:alice", ndn::Name("/ObjectDetection/YOLOv8"), {}};

  auto decision = state.authorize(complete, ProtectedTransition::DISCOVERY, 1000);
  BOOST_CHECK(!decision.allowed);
  BOOST_CHECK_EQUAL(decision.reason, "no_authenticated_controller_status");

  BOOST_REQUIRE(state.acceptStatus(makeStatus(ControllerVersion{4100, 1}), 1000));
  auto missingIdentity = complete;
  missingIdentity.identity = {};
  BOOST_CHECK_EQUAL(state.authorize(missingIdentity, ProtectedTransition::DISCOVERY, 1000).reason,
                    "incomplete_authorization_subject");
  auto missingCertificate = complete;
  missingCertificate.certificateDigest.clear();
  BOOST_CHECK_EQUAL(state.authorize(missingCertificate, ProtectedTransition::DISCOVERY, 1000).reason,
                    "incomplete_authorization_subject");
  auto wrongService = complete;
  wrongService.serviceName = ndn::Name("/OtherService");
  BOOST_CHECK_EQUAL(state.authorize(wrongService, ProtectedTransition::DISCOVERY, 1000).reason,
                    "service_scope_mismatch");
  BOOST_CHECK_EQUAL(state.authorize(complete, ProtectedTransition::DISCOVERY, 3000).reason,
                    "controller_status_expired");
}

BOOST_AUTO_TEST_CASE(ProtectedMessagesRoundTripControllerVersion)
{
  const ControllerVersion version{4500, 9};
  RequestMessage request;
  request.setControllerVersion(version);
  const auto requestWire = request.WireEncode();
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(requestWire));
  BOOST_REQUIRE(decodedRequest.hasControllerVersion());
  BOOST_CHECK(decodedRequest.getControllerVersion() == version);

  ResponseMessage response;
  response.setControllerVersion(version);
  const auto responseWire = response.WireEncode();
  ResponseMessage decodedResponse;
  BOOST_REQUIRE(decodedResponse.WireDecode(responseWire));
  BOOST_CHECK(decodedResponse.hasControllerVersion());
  BOOST_CHECK(decodedResponse.getControllerVersion() == version);

  RequestAckMessage ack;
  ack.setControllerVersion(version);
  const auto ackWire = ack.WireEncode();
  RequestAckMessage decodedAck;
  BOOST_REQUIRE(decodedAck.WireDecode(ackWire));
  BOOST_CHECK(decodedAck.hasControllerVersion());
  BOOST_CHECK(decodedAck.getControllerVersion() == version);

  ServiceSelectionMessage selection;
  selection.setControllerVersion(version);
  const auto selectionWire = selection.WireEncode();
  ServiceSelectionMessage decodedSelection;
  BOOST_REQUIRE(decodedSelection.WireDecode(selectionWire));
  BOOST_CHECK(decodedSelection.hasControllerVersion());
  BOOST_CHECK(decodedSelection.getControllerVersion() == version);

  PermissionResponse permissions;
  permissions.setTargetIdentity("/user/alice");
  permissions.setPermissionKind(tlv::UserPermission);
  permissions.setPolicyEpoch(version.controllerEpoch);
  permissions.setControllerVersion(version);
  const auto permissionWire = permissions.WireEncode();
  PermissionResponse decodedPermissions;
  BOOST_REQUIRE(decodedPermissions.WireDecode(permissionWire));
  BOOST_REQUIRE(decodedPermissions.hasControllerVersion());
  BOOST_CHECK(decodedPermissions.getControllerVersion() == version);

  PolicyManifest manifest;
  manifest.setPolicyEpoch(version.controllerEpoch);
  manifest.setRequiredKeyEpoch(version.controllerEpoch);
  manifest.setValidFromMs(100);
  manifest.setGracePeriodMs(50);
  manifest.setControllerVersion(version);
  const auto manifestWire = manifest.WireEncode();
  PolicyManifest decodedManifest;
  BOOST_REQUIRE(decodedManifest.WireDecode(manifestWire));
  BOOST_REQUIRE(decodedManifest.hasControllerVersion());
  BOOST_CHECK(decodedManifest.getControllerVersion() == version);
}

BOOST_AUTO_TEST_CASE(ProtectedMessagesRejectZeroOrDuplicateControllerVersion)
{
  // A message-carried version is only a refresh hint.  It still has to be a
  // well-formed, non-zero value before the receiver can start a bounded
  // exact-status fetch.  Build malformed nested TLVs directly because the
  // public setters intentionally reject invalid versions.
  const ControllerVersion version{4501, 9};

  const auto replaceVersion = [] (const ndn::Block& encoded,
                                  uint64_t generation,
                                  uint64_t epoch,
                                  bool duplicate) {
    auto input = encoded;
    input.parse();
    ndn::Block invalid(ControllerVersion::TYPE);
    invalid.push_back(ndn::makeNonNegativeIntegerBlock(
        ControllerVersion::GenerationTimestampType, generation));
    invalid.push_back(ndn::makeNonNegativeIntegerBlock(
        ControllerVersion::EpochType, epoch));
    invalid.encode();

    ndn::Block output(input.type());
    for (const auto& element : input.elements()) {
      output.push_back(element.type() == tlv::ControllerVersionType ? invalid : element);
      if (duplicate && element.type() == tlv::ControllerVersionType)
        output.push_back(invalid);
    }
    output.encode();
    return output;
  };

  RequestMessage request;
  request.setControllerVersion(version);
  auto requestZero = replaceVersion(request.WireEncode(), 0, 1, false);
  auto requestDuplicate = replaceVersion(request.WireEncode(), 4501, 10, true);
  BOOST_CHECK(!RequestMessage{}.WireDecode(requestZero));
  BOOST_CHECK(!RequestMessage{}.WireDecode(requestDuplicate));

  ResponseMessage response;
  response.setControllerVersion(version);
  auto responseZero = replaceVersion(response.WireEncode(), 4501, 0, false);
  auto responseDuplicate = replaceVersion(response.WireEncode(), 4501, 10, true);
  BOOST_CHECK(!ResponseMessage{}.WireDecode(responseZero));
  BOOST_CHECK(!ResponseMessage{}.WireDecode(responseDuplicate));

  RequestAckMessage ack;
  ack.setControllerVersion(version);
  auto ackZero = replaceVersion(ack.WireEncode(), 0, 0, false);
  auto ackDuplicate = replaceVersion(ack.WireEncode(), 4501, 10, true);
  BOOST_CHECK(!RequestAckMessage{}.WireDecode(ackZero));
  BOOST_CHECK(!RequestAckMessage{}.WireDecode(ackDuplicate));

  ServiceSelectionMessage selection;
  selection.setControllerVersion(version);
  auto selectionZero = replaceVersion(selection.WireEncode(), 0, 9, false);
  auto selectionDuplicate = replaceVersion(selection.WireEncode(), 4501, 10, true);
  BOOST_CHECK(!ServiceSelectionMessage{}.WireDecode(selectionZero));
  BOOST_CHECK(!ServiceSelectionMessage{}.WireDecode(selectionDuplicate));
}

BOOST_AUTO_TEST_CASE(GenerationPersistsAcrossRestartAndClockRollback)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-generation-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerVersion first;
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("controller-a"));
    first = store.startGeneration(1000);
    BOOST_CHECK_EQUAL(first.controllerEpoch, 1);
    auto changed = store.advanceEpoch();
    BOOST_CHECK_EQUAL(changed.controllerEpoch, 2);
  }

  {
    ControllerGenerationStore restarted(path);
    BOOST_REQUIRE(restarted.acquireWriter("controller-b"));
    auto second = restarted.startGeneration(900);
    BOOST_CHECK(second.controllerGenerationTimestamp > first.controllerGenerationTimestamp);
    BOOST_CHECK_EQUAL(second.controllerEpoch, 1);
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(GenerationStoreRejectsInvalidWriterAndStartInputs)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-generation-inputs-" +
                     std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerGenerationStore store(path);
  BOOST_CHECK(!store.acquireWriter(""));
  BOOST_CHECK_THROW(store.startGeneration(1000), std::runtime_error);
  BOOST_CHECK_THROW(store.advanceEpoch(), std::runtime_error);

  BOOST_REQUIRE(store.acquireWriter("controller"));
  BOOST_CHECK_THROW(store.startGeneration(0), std::invalid_argument);
  BOOST_REQUIRE(store.startGeneration(1000).isValid());
  BOOST_CHECK_THROW(store.startGeneration(0), std::invalid_argument);
  store.releaseWriter();

  ControllerGenerationStore outsider(path);
  BOOST_CHECK_THROW(outsider.advanceEpoch(), std::runtime_error);

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(GenerationPersistsRevocationsAcrossRestart)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-revocations-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  const auto user = identityRevocation("/user/alice");
  RevocationTarget service;
  service.kind = RevocationKind::SERVICE_AUTHORIZATION;
  service.targetIdentity = ndn::Name("/provider/p1");
  service.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  service.authorizationAttribute = ndn::Name("/SERVICE/ObjectDetection/YOLOv8");
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("controller"));
    BOOST_REQUIRE(store.startGeneration(1000, {user}).isValid());
    BOOST_REQUIRE(store.advanceEpoch({user, service}).isValid());
  }
  {
    ControllerGenerationStore restarted(path);
    BOOST_REQUIRE(restarted.acquireWriter("controller-restarted"));
    const auto recovered = restarted.loadRevocations();
    BOOST_REQUIRE_EQUAL(recovered.size(), 2);
    BOOST_CHECK(recovered[0].targetIdentity == user.targetIdentity);
    BOOST_CHECK(recovered[1].serviceName == service.serviceName);
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(OnlyOneWriterCanPublish)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-writer-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerGenerationStore first(path);
  ControllerGenerationStore second(path);
  BOOST_REQUIRE(first.acquireWriter("first"));
  BOOST_CHECK(!second.acquireWriter("second"));
  BOOST_REQUIRE(first.startGeneration(1000).isValid());
  first.releaseWriter();
  BOOST_REQUIRE(second.acquireWriter("second"));
  BOOST_REQUIRE(second.advanceEpoch().isValid());

  second.releaseWriter();
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(LostWriterLeaseCannotPublishWithStaleFence)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-lost-lease-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerGenerationStore first(path);
  BOOST_REQUIRE(first.acquireWriter("first"));
  const auto initial = first.startGeneration(1000);
  BOOST_REQUIRE(initial.isValid());

  // Simulate another Controller taking over the lease.  The old instance
  // must fail closed before it can advance or publish a new authority
  // version, even though its in-memory fence is still non-zero.
  {
    std::ofstream lock(path.string() + ".lock", std::ios::trunc);
    lock << "replacement\n999999\n";
  }
  BOOST_CHECK(!first.ownsWriter());
  BOOST_CHECK_THROW(first.advanceEpoch(), std::runtime_error);

  std::filesystem::remove(path.string() + ".lock");
  ControllerGenerationStore replacement(path);
  BOOST_REQUIRE(replacement.acquireWriter("replacement"));
  const auto next = replacement.advanceEpoch();
  BOOST_CHECK(next.compare(initial) > 0);
  replacement.releaseWriter();
  first.releaseWriter();
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(RepeatedControllerStartsRemainStrictlyMonotonic)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-repeated-start-" +
                     std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");

  ControllerVersion first;
  ControllerVersion second;
  ControllerVersion third;
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("first"));
    first = store.startGeneration(5000);
  }
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("second"));
    second = store.startGeneration(4000); // clock rollback
  }
  {
    ControllerGenerationStore store(path);
    BOOST_REQUIRE(store.acquireWriter("third"));
    third = store.startGeneration(3000); // repeated rollback
  }
  BOOST_CHECK(second.controllerGenerationTimestamp > first.controllerGenerationTimestamp);
  BOOST_CHECK(third.controllerGenerationTimestamp > second.controllerGenerationTimestamp);
  BOOST_CHECK_EQUAL(first.controllerEpoch, 1);
  BOOST_CHECK_EQUAL(second.controllerEpoch, 1);
  BOOST_CHECK_EQUAL(third.controllerEpoch, 1);

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_CASE(CorruptGenerationStateFailsClosed)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("ndnsf-controller-corrupt-" + std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
  {
    std::ofstream out(path, std::ios::binary);
    out << "corrupt";
  }

  ControllerGenerationStore store(path);
  BOOST_REQUIRE(store.acquireWriter("controller"));
  BOOST_CHECK_THROW(store.startGeneration(1000), std::runtime_error);
  store.releaseWriter();
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".lock");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
