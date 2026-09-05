#include "tests/boost-test.hpp"

#include "ndn-service-framework/ControllerGenerationStore.hpp"
#include "ndn-service-framework/PolicyRefreshCoordinator.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/RevocationState.hpp"
#include "ndn-service-framework/ServiceController.hpp"

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace ndn_service_framework {
namespace integration_test {

namespace {

uint64_t
wallClockMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

BOOST_AUTO_TEST_SUITE(ControllerVersionRefresh)

BOOST_AUTO_TEST_CASE(ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic)
{
  // Compose the production Controller status snapshot with the refresh and
  // enforcement components.  This remains a component integration test: the
  // enclosing Data trust-schema and cross-process delivery are MiniNDN gates.
  const auto statePath = std::filesystem::temp_directory_path() /
                         ("ndnsf-controller-refresh-" +
                          std::to_string(::getpid()) + ".bin");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
  ::setenv("NDNSF_CONTROLLER_GENERATION_STATE", statePath.c_str(), 1);

  ndn::KeyChain keyChain("pib-memory:spec179-refresh",
                         "tpm-memory:spec179-refresh");
  const auto controller = keyChain.createIdentity(
      ndn::Name("/controller/spec179-refresh"), ndn::RsaKeyParams(2048));
  const auto controllerCert = controller.getDefaultKey().getDefaultCertificate();
  ndn::Face face;
  ndn::ValidatorConfig validator(face);
  ServiceController serviceController(face, controllerCert, validator,
                                      "examples/hello.policies");

  const ndn::Name service("/HELLO");
  const auto version1 = serviceController.getControllerVersion();
  const auto status1 = serviceController.getPolicyStatus(service);
  PolicyRefreshCoordinator refresh(service, 3, 5);
  RevocationState state(service);
  BOOST_REQUIRE(refresh.installCurrentStatus(status1, wallClockMs(), true));
  BOOST_REQUIRE(state.acceptStatus(status1, wallClockMs(), true));

  // A hintless pre-expiry refresh is single-flight and can complete with the
  // same immutable status snapshot.
  const auto scheduled = refresh.startScheduledRefresh(
      status1.getValidUntilMs() - 1, 10);
  BOOST_CHECK(scheduled.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_CHECK(refresh.startScheduledRefresh(
      status1.getValidUntilMs() - 1, 10).outcome ==
              PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  BOOST_CHECK(refresh.completeFetch(status1,
                                    status1.getValidUntilMs() - 1,
                                    true).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);

  RevocationTarget revokedUser;
  revokedUser.kind = RevocationKind::IDENTITY;
  revokedUser.targetIdentity = ndn::Name("/example/hello/user");
  BOOST_REQUIRE(serviceController.revoke(revokedUser));
  const auto status2 = serviceController.getPolicyStatus(service);
  BOOST_REQUIRE(status2.getControllerVersion().compare(version1) > 0);

  const auto hint = refresh.observeHint(status2.getControllerVersion(), true,
                                        wallClockMs());
  BOOST_CHECK(hint.outcome == PolicyRefreshCoordinator::Outcome::FETCH_STARTED);
  BOOST_CHECK(refresh.currentVersion() == version1);
  BOOST_CHECK(state.currentVersion() == version1);
  BOOST_CHECK(refresh.observeHint(status2.getControllerVersion(), true,
                                  wallClockMs()).outcome ==
              PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
  const auto wire = status2.wireEncode();
  PolicyStatusData decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(refresh.completeFetch(decoded, wallClockMs(), true).outcome ==
              PolicyRefreshCoordinator::Outcome::ACCEPTED);
  BOOST_REQUIRE(state.acceptStatus(decoded, wallClockMs(), true));
  BOOST_CHECK_EQUAL(state.invalidatedCacheCount(), 6);
  for (const auto family : {"abe", "message-key", "targeted-token",
                            "selection-binding", "nonce", "replay"})
    BOOST_CHECK(state.cacheInvalidated(family));

  // The status object is service-scoped.  A valid snapshot for another
  // service must be rejected by this state and cannot invalidate its caches.
  const ndn::Name otherService("/OtherService");
  RevocationState otherState(otherService);
  const auto otherStatus = serviceController.getPolicyStatus(otherService);
  BOOST_REQUIRE(otherState.acceptStatus(otherStatus, wallClockMs(), true));
  otherState.clearInvalidationEvidence();
  BOOST_CHECK(!otherState.acceptStatus(decoded, wallClockMs(), true));
  BOOST_CHECK_EQUAL(otherState.invalidatedCacheCount(), 0);

  const AuthorizationSubject revoked{
      revokedUser.targetIdentity, "sha256:user-cert", service,
      ndn::Name("/PERMISSION").append(service)};
  const AuthorizationSubject unaffected{
      ndn::Name("/example/hello/user-b"), "sha256:user-b-cert", service,
      ndn::Name("/PERMISSION").append(service)};
  BOOST_CHECK(!state.authorize(
      revoked, ProtectedTransition::DISCOVERY, wallClockMs()).allowed);
  BOOST_CHECK(state.authorize(
      unaffected, ProtectedTransition::DISCOVERY, wallClockMs()).allowed);
  BOOST_CHECK(!refresh.hasExpiredStatus(wallClockMs()));

  ::unsetenv("NDNSF_CONTROLLER_GENERATION_STATE");
  std::filesystem::remove(statePath);
  std::filesystem::remove(statePath.string() + ".lock");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace integration_test
} // namespace ndn_service_framework
