#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace ndn_service_framework::test {
namespace {

enum class StopPoint { PENDING, BACKOFF, VALIDATION, REPLACEMENT, LIVE };

class ControllerBootstrapProbe : public ServiceUser
{
public:
  using ServiceUser::ServiceUser;

  void
  bindFaceValidator(ndn::Face& face, const BootstrapProfile& profile)
  {
    // LocalMockTag's default validator is synchronous. Match the production
    // constructor's Face dispatch so queued-validation teardown is observable.
    validator = std::make_shared<MessageValidator>(profile.trustSchemaPath,
      profile.groupPrefix, &face);
  }
};

void
checkBootstrapLifetime(StopPoint stopPoint)
{
  const char* lifetimeName = "NDNSF_PERMISSION_FETCH_LIFETIME_MS";
  const char* backoffName = "NDNSF_PERMISSION_FETCH_RETRY_BACKOFF_MS";
  const auto previous = [](const char* name) -> std::optional<std::string> {
    const auto value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
  };
  const auto lifetime = previous(lifetimeName), backoff = previous(backoffName);
  auto restore = ndn::make_scope_exit([&] {
    if (lifetime) ::setenv(lifetimeName, lifetime->c_str(), 1);
    else ::unsetenv(lifetimeName);
    if (backoff) ::setenv(backoffName, backoff->c_str(), 1);
    else ::unsetenv(backoffName);
  });
  BOOST_REQUIRE_EQUAL(::setenv(lifetimeName, "500", 1), 0);
  BOOST_REQUIRE_EQUAL(::setenv(backoffName, "400", 1), 0);
  BootstrapProfile profile;
  profile.deferBridgeDelivery = true;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  auto& keyChain = environment.keyChain();
  const auto certificate = keyChain.getPib().getIdentity(profile.userIdentity)
    .getDefaultKey().getDefaultCertificate();
  const auto authority = keyChain.getPib().getIdentity(profile.attributeAuthority)
    .getDefaultKey().getDefaultCertificate();
  auto client = std::make_unique<ControllerBootstrapProbe>(ServiceUser::LocalMockTag{},
    environment.userFace(), profile.groupPrefix, certificate, authority,
    environment.profile().trustSchemaPath);
  client->useSigningKeyChainForTest(keyChain);
  client->bindFaceValidator(environment.userFace(), environment.profile());
  std::vector<ndn::Interest> queries;
  ndn::signal::ScopedConnection sent = environment.userFace().onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      const auto name = interest.getName().toUri(ndn::name::UriFormat::ALTERNATE);
      if (name.find("/POLICY-MANIFEST/") != std::string::npos ||
          name.find("/USER-PERMISSIONS/") != std::string::npos)
        queries.push_back(interest);
    });
  const auto pump = [&](int milliseconds) {
    environment.userFace().getIoContext().restart();
    environment.userFace().processEvents(ndn::time::milliseconds(milliseconds));
  };
  client->fetchPermissionsFromController(profile.attributeAuthority);
  pump(20);
  BOOST_REQUIRE_EQUAL(queries.size(), 2U);
  if (stopPoint == StopPoint::BACKOFF) {
    pump(600);
    BOOST_REQUIRE_EQUAL(queries.size(), 2U);
  }
  if (stopPoint == StopPoint::LIVE) {
    pump(1000);
    BOOST_REQUIRE_GE(queries.size(), 4U);
  }
  if (stopPoint == StopPoint::VALIDATION || stopPoint == StopPoint::REPLACEMENT ||
      stopPoint == StopPoint::LIVE) {
    const auto found = std::find_if(queries.rbegin(), queries.rend(), [](const auto& query) {
      return query.getName().toUri(ndn::name::UriFormat::ALTERNATE)
        .find("/POLICY-MANIFEST/") != std::string::npos;
    });
    BOOST_REQUIRE(found != queries.rend());
    PolicyManifest manifest;
    manifest.setPolicyEpoch(42);
    manifest.setRequiredKeyEpoch(42);
    manifest.setControllerVersion(ControllerVersion{1, 42});
    ndn::Data data(found->getName());
    data.setContent(manifest.WireEncode());
    keyChain.sign(data, ndn::security::signingByCertificate(authority));
    if (stopPoint == StopPoint::LIVE) {
      environment.userFace().receive(data);
      pump(50);
      BOOST_CHECK_EQUAL(client->getCurrentPolicyEpoch(), 42U);
    }
    else {
      // Queue real trust validation, then retire its owner before completion.
      client->onPolicyManifestData(*found, data);
      BOOST_CHECK_EQUAL(client->getCurrentPolicyEpoch(), 0U);
    }
  }
  if (stopPoint == StopPoint::REPLACEMENT) {
    client->fetchPermissionsFromController(profile.attributeAuthority);
    pump(20);
    BOOST_CHECK_EQUAL(client->getCurrentPolicyEpoch(), 0U);
  }
  client.reset();
  pump(20);
  const auto stoppedQueries = queries.size();
  pump(1100);
  BOOST_CHECK_EQUAL(queries.size(), stoppedQueries);
}

BOOST_AUTO_TEST_SUITE(ControllerBootstrapLifetime)

BOOST_AUTO_TEST_CASE(PumpDoesNotSignalUserShutdownToProvider)
{
  NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  bool observed = false;
  boost::asio::post(environment.providerFace(0).getIoContext(), [&] {
    BOOST_CHECK(!environment.userFace().getIoContext().stopped());
    observed = true;
  });
  environment.pumpUntilWithAttributeAuthority([&] { return observed; });
  BOOST_CHECK(observed);
  BOOST_CHECK(!environment.userFace().getIoContext().stopped());
}

BOOST_AUTO_TEST_CASE(DestroyWithPendingInterests)
{
  checkBootstrapLifetime(StopPoint::PENDING);
}

BOOST_AUTO_TEST_CASE(DestroyDuringRetryBackoff)
{
  checkBootstrapLifetime(StopPoint::BACKOFF);
}

BOOST_AUTO_TEST_CASE(DestroyBeforeValidationCompletion)
{
  checkBootstrapLifetime(StopPoint::VALIDATION);
}

BOOST_AUTO_TEST_CASE(ReplaceBeforeValidationCompletion)
{
  checkBootstrapLifetime(StopPoint::REPLACEMENT);
}

BOOST_AUTO_TEST_CASE(LiveOwnerRetriesAndValidatesManifest)
{
  checkBootstrapLifetime(StopPoint::LIVE);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
