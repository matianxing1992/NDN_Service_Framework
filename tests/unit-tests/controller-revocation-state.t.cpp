#include "tests/boost-test.hpp"

#include "ndn-service-framework/PolicyRefreshCoordinator.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include "ndn-service-framework/utils.hpp"

#include <cstdint>
#include <array>
#include <string>
#include <stdexcept>

namespace ndn_service_framework::test {
namespace {

PolicyStatusData
makeRefreshStatus(const ControllerVersion& version,
                  uint64_t validFrom = 100,
                  uint64_t validUntil = 5000)
{
  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(version);
  status.setValidity(validFrom, validUntil);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer(reinterpret_cast<const uint8_t*>("signature"), 9));
  return status;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ControllerRevocationState)

BOOST_AUTO_TEST_CASE(PersistedUppercaseCertificateTargetMatchesCanonicalSubject)
{
  auto status = makeRefreshStatus(ControllerVersion{1000, 1});
  RevocationTarget target;
  target.kind = RevocationKind::CERTIFICATE;
  target.targetIdentity = ndn::Name("/provider");
  target.certificateDigest = "sha256:" + std::string(64, 'A');
  status.addRevocation(target);
  const auto originalWire = status.wireEncode();
  PolicyStatusData restored;
  BOOST_REQUIRE(restored.wireDecode(originalWire));
  BOOST_CHECK(restored.wireEncode() == originalWire);
  BOOST_CHECK_EQUAL(restored.getRevocations().front().certificateDigest,
                    target.certificateDigest);
  RevocationState state(status.getServiceName());
  BOOST_REQUIRE(state.acceptStatus(restored, 200, true));
  AuthorizationSubject subject{target.targetIdentity,
      "sha256:" + std::string(64, 'a'), status.getServiceName(),
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")};
  BOOST_CHECK(!state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  subject.certificateDigest.back() = 'b';
  BOOST_CHECK(state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  subject.certificateDigest = "sha256:" + std::string(64, 'a');
  subject.identity = ndn::Name("/another-provider");
  BOOST_CHECK(state.authorize(subject, ProtectedTransition::PROVIDER_EXECUTION, 200).allowed);
  BOOST_CHECK(!sameCertificateDigest("sha256:AB", "sha256:ab"));
  BOOST_CHECK(!sameCertificateDigest("opaque-A", "opaque-a"));
}

BOOST_AUTO_TEST_CASE(UnauthenticatedHintsNeverChangeRefreshState)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_CHECK(coordinator.observeHint(ControllerVersion{1000, 2}, false, 200).outcome ==
              PolicyRefreshCoordinator::Outcome::REJECTED_UNAUTHENTICATED_HINT);
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK(!coordinator.hasCurrentStatus());
}

BOOST_AUTO_TEST_CASE(AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch)
{
  // A protected message may be the first evidence that this runtime is behind
  // the Controller.  The hint starts a bounded exact-name fetch, but it is not
  // an authority update and therefore cannot authorize a transition by itself.
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 2, 10);
  const ControllerVersion hintedVersion{1100, 1};
  const auto started = coordinator.observeHint(hintedVersion, true, 100);
  BOOST_REQUIRE(started.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(started.request);
  BOOST_CHECK(started.request->version == hintedVersion);
  BOOST_CHECK(!coordinator.hasCurrentStatus());
  BOOST_CHECK(!coordinator.currentVersion().isValid());

  // A forged/invalid exact result leaves the runtime without authority and
  // schedules only the bounded retry; a valid signed result is the sole path
  // that installs the version and permits later authorization.
  const auto rejected = coordinator.completeFetch(
      makeRefreshStatus(hintedVersion), 200, false);
  BOOST_CHECK(rejected.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK(!coordinator.hasCurrentStatus());
  BOOST_REQUIRE(rejected.request);
  BOOST_CHECK_EQUAL(rejected.request->attempt, 2);
  BOOST_CHECK(coordinator.failFetch(210).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.hasCurrentStatus());

  const auto retry = coordinator.observeHint(hintedVersion, true, 300);
  BOOST_REQUIRE(retry.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(retry.request);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(hintedVersion), 400).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(coordinator.hasCurrentStatus());
  BOOST_CHECK(coordinator.currentVersion() == hintedVersion);
}

BOOST_AUTO_TEST_CASE(PolicyStatusNameIsExactAndVersionAddressable)
{
  const ndn::Name controller("/controller/spec179");
  const ndn::Name service("/ObjectDetection/YOLOv8");
  const ControllerVersion version{1788285600123ULL, 7};
  const auto exact = makePolicyStatusName(controller, service, version);

  ndn::Name expected(controller);
  expected.append("NDNSF").append("POLICY-STATUS").append(service)
      .append("version")
      .appendNumber(version.controllerGenerationTimestamp)
      .appendNumber(version.controllerEpoch);
  BOOST_CHECK(exact == expected);
  const auto parsed = parsePolicyStatusName(controller, exact);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->serviceName == service);
  BOOST_REQUIRE(parsed->version);
  BOOST_CHECK(*parsed->version == version);

  // A ParametersSha256Digest belongs to the Interest name and must not
  // change the (service, generation, epoch) identity used for retrieval.
  auto withDigest = exact;
  std::array<uint8_t, 32> digest{};
  withDigest.appendParametersSha256Digest(
      ndn::span<const uint8_t>(digest.data(), digest.size()));
  const auto parsedWithDigest = parsePolicyStatusName(controller, withDigest);
  BOOST_REQUIRE(parsedWithDigest);
  BOOST_CHECK(parsedWithDigest->serviceName == service);
  BOOST_REQUIRE(parsedWithDigest->version);
  BOOST_CHECK(*parsedWithDigest->version == version);

  // Bare service names remain the only bootstrap form; they carry no
  // authority version and therefore cannot be mistaken for an exact result.
  const auto bootstrap = parsePolicyStatusName(
      controller,
      ndn::Name("/controller/spec179/NDNSF/POLICY-STATUS/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(bootstrap);
  BOOST_CHECK(bootstrap->serviceName == service);
  BOOST_CHECK(!bootstrap->version);
  BOOST_CHECK(!parsePolicyStatusName(
      ndn::Name("/controller/other"), exact));
  BOOST_CHECK_THROW(makePolicyStatusName(controller, service, ControllerVersion{}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(HigherHintsCoalesceToOneHighestCandidate)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{1000, 1}), 200));

  const auto first = coordinator.observeHint(ControllerVersion{1000, 2}, true, 300);
  BOOST_REQUIRE(first.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(first.request);
  BOOST_CHECK((first.request->version == ControllerVersion{1000, 2}));
  BOOST_CHECK_EQUAL(first.request->attempt, 1);

  const auto duplicate = coordinator.observeHint(ControllerVersion{1000, 2}, true, 301);
  BOOST_CHECK(duplicate.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  const auto higher = coordinator.observeHint(ControllerVersion{1001, 1}, true, 302);
  BOOST_CHECK(higher.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  BOOST_REQUIRE(coordinator.highestCandidate());
  BOOST_CHECK((*coordinator.highestCandidate() == ControllerVersion{1001, 1}));

  const auto accepted = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1001, 1}), 400);
  BOOST_CHECK(accepted.outcome == PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(!coordinator.inFlight());
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{1001, 1}));
}

BOOST_AUTO_TEST_CASE(HigherHintNeverBecomesAuthorityBeforeExactStatusFetch)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  const auto current = makeRefreshStatus(ControllerVersion{1250, 1});
  BOOST_REQUIRE(coordinator.installCurrentStatus(current, 200));

  const auto hint = coordinator.observeHint(ControllerVersion{1250, 2}, true, 300);
  BOOST_REQUIRE(hint.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(hint.request);
  BOOST_CHECK(coordinator.currentVersion() == current.getControllerVersion());
  BOOST_CHECK(coordinator.hasCurrentStatus());

  // An invalid exact-name fetch must leave the last accepted authority in
  // place; only a validated status may advance it.
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1250, 2}), 400, false).request);
  BOOST_CHECK(coordinator.currentVersion() == current.getControllerVersion());
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{1250, 2}), 500).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  const ControllerVersion advanced{1250, 2};
  BOOST_CHECK(coordinator.currentVersion() == advanced);
}

BOOST_AUTO_TEST_CASE(ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{2000, 1}, 100, 1000), 200));
  BOOST_CHECK(coordinator.startScheduledRefresh(700, 100).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  const auto started = coordinator.startScheduledRefresh(950, 100);
  BOOST_CHECK(started.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  const auto coalesced = coordinator.startScheduledRefresh(951, 100);
  BOOST_CHECK(coalesced.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  BOOST_CHECK(coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(InvalidStatusRetriesWithBoundedAttempts)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 3, 10);
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{3000, 1}), 200));
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{3000, 2}, true, 300).request);

  const auto invalid = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{3000, 2}), 400, false);
  BOOST_CHECK(invalid.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_REQUIRE(invalid.request);
  BOOST_CHECK_EQUAL(invalid.request->attempt, 2);
  BOOST_CHECK(!coordinator.retryIfDue(419));
  BOOST_CHECK(coordinator.retryIfDue(420));

  const auto timeout = coordinator.failFetch(420);
  BOOST_CHECK(timeout.outcome == PolicyRefreshCoordinator::Outcome::RETRY_SCHEDULED);
  BOOST_REQUIRE(timeout.request);
  BOOST_CHECK_EQUAL(timeout.request->attempt, 3);
  const auto exhausted = coordinator.failFetch(500);
  BOOST_CHECK(exhausted.outcome == PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(OlderAndConflictingEqualVersionStatusFailClosed)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_REQUIRE(coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{4000, 1}), 200));
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{4000, 2}, true, 300).request);

  const auto stale = coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{4000, 1}), 400);
  BOOST_CHECK(stale.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{4000, 1}));

  // Exhaust the bounded retry state before starting a fresh candidate.
  BOOST_CHECK(coordinator.failFetch(400).outcome ==
              PolicyRefreshCoordinator::Outcome::RETRY_SCHEDULED);
  BOOST_CHECK(coordinator.failFetch(420).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_REQUIRE(coordinator.observeHint(ControllerVersion{4000, 2}, true, 500).request);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{4000, 2}), 600).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);

  // A scheduled refresh can fetch the current version again.  A different
  // payload at that same version must not replace the accepted authority.
  BOOST_REQUIRE(coordinator.startScheduledRefresh(4999, 10).request);
  auto conflicting = makeRefreshStatus(ControllerVersion{4000, 2});
  conflicting.addRevocation(RevocationTarget{
      RevocationKind::IDENTITY, ndn::Name("/user/alice"), {}, {}, {}});
  const auto result = coordinator.completeFetch(conflicting, 800);
  BOOST_CHECK(result.outcome == PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK((coordinator.currentVersion() == ControllerVersion{4000, 2}));
}

BOOST_AUTO_TEST_CASE(ServiceScopeAndMissingStatusFailClosed)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"));
  BOOST_CHECK(coordinator.startScheduledRefresh(100, 10).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  BOOST_CHECK(!coordinator.installCurrentStatus(
      makeRefreshStatus(ControllerVersion{5000, 1}), 200, false));
  BOOST_CHECK(!coordinator.installCurrentStatus(
      [] {
        auto status = makeRefreshStatus(ControllerVersion{5000, 1});
        status.setServiceName(ndn::Name("/OtherService"));
        return status;
      }(), 200));
  BOOST_CHECK(!coordinator.hasCurrentStatus());
}

BOOST_AUTO_TEST_CASE(RefreshCoordinatorHandlesTerminalAndExpiryBoundaries)
{
  PolicyRefreshCoordinator coordinator(ndn::Name("/ObjectDetection/YOLOv8"), 2, 25);

  // A scheduled refresh cannot invent authority, and completion/failure
  // callbacks without an outstanding fetch terminate deterministically.
  BOOST_CHECK(coordinator.startScheduledRefresh(100, 10).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);
  BOOST_CHECK(coordinator.completeFetch(
      makeRefreshStatus(ControllerVersion{6000, 1}), 100).outcome ==
              PolicyRefreshCoordinator::Outcome::REJECTED_STATUS);
  BOOST_CHECK(coordinator.failFetch(100).outcome ==
              PolicyRefreshCoordinator::Outcome::EXHAUSTED);
  BOOST_CHECK(!coordinator.retryIfDue(100));

  const auto current = makeRefreshStatus(ControllerVersion{6000, 1}, 100, 1000);
  BOOST_REQUIRE(coordinator.installCurrentStatus(current, 200));
  BOOST_CHECK(!coordinator.hasExpiredStatus(999));
  BOOST_CHECK(coordinator.hasExpiredStatus(1000));
  BOOST_CHECK(coordinator.observeHint(current.getControllerVersion(), true, 300).outcome ==
              PolicyRefreshCoordinator::Outcome::IGNORED_CURRENT);

  const auto scheduled = coordinator.startScheduledRefresh(950, 100);
  BOOST_REQUIRE(scheduled.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_REQUIRE(scheduled.request);
  BOOST_CHECK(!coordinator.retryIfDue(949));
  BOOST_CHECK(coordinator.retryIfDue(950));
  BOOST_CHECK(coordinator.completeFetch(current, 950).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_CHECK(!coordinator.inFlight());
}

BOOST_AUTO_TEST_CASE(RevocationStateInvalidatesOnlyAcceptedServiceFamilies)
{
  const ndn::Name service("/ObjectDetection/YOLOv8");
  const ndn::Name otherService("/OtherService");
  const auto current = makeRefreshStatus(ControllerVersion{7000, 1});

  RevocationState state(service);
  BOOST_REQUIRE(state.acceptStatus(current, 1000));
  state.clearInvalidationEvidence();

  auto revoked = makeRefreshStatus(ControllerVersion{7000, 2});
  revoked.addRevocation(RevocationTarget{
      RevocationKind::SERVICE_AUTHORIZATION,
      ndn::Name("/provider/p1"), service, {},
      ndn::Name("/SERVICE/ObjectDetection/YOLOv8")});
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  for (const auto family : {"abe", "message-key", "targeted-token",
                            "selection-binding", "nonce", "replay"})
    BOOST_CHECK(state.cacheInvalidated(family));

  // Re-reading the identical immutable status is idempotent.  It must not
  // evict request keys or replay state a second time merely because refresh
  // was scheduled or a duplicate status Data arrived.
  state.clearInvalidationEvidence();
  BOOST_REQUIRE(state.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 0);

  // A status for another service cannot become authority here or invalidate
  // this service's authorization material.
  RevocationState otherState(otherService);
  auto otherCurrent = makeRefreshStatus(ControllerVersion{7000, 1});
  otherCurrent.setServiceName(otherService);
  BOOST_REQUIRE(otherState.acceptStatus(otherCurrent, 1000));
  otherState.clearInvalidationEvidence();
  BOOST_CHECK(!otherState.acceptStatus(revoked, 1000));
  BOOST_CHECK_EQUAL(otherState.invalidatedCacheCount(), 0);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
