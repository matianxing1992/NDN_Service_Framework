#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/mgmt/nfd/control-parameters.hpp>
#include <ndn-cxx/mgmt/nfd/control-response.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

namespace ndn_service_framework::test {
namespace {

class IdentityRegistrationUser : public ServiceUser
{
public:
  using ServiceUser::ServiceUser;
  using ServiceUser::registerIdentityPrefixWithRetry;
};

struct IdentityRegistrationFixture
{
  IdentityRegistrationFixture()
    : keys("pib-memory:identity-registration", "tpm-memory:identity-registration")
    , certificate(makeRsaIdentity(keys, ndn::Name("/test/identity-registration/user")))
    , face(keys)
    , user(std::make_unique<IdentityRegistrationUser>(
        ServiceUser::LocalMockTag{}, face, ndn::Name("/test/identity-registration"),
        certificate, certificate, "examples/trust-any.conf"))
  {
    face.onSendInterest.connect([this](const ndn::Interest& interest) {
      const auto& name = interest.getName();
      if (name.size() <= 4 || !ndn::Name("/localhost/nfd/rib").isPrefixOf(name)) {
        return;
      }
      ndn::nfd::ControlParameters parameters(name[4].blockFromValue());
      if (parameters.getName() != certificate.getIdentity()) {
        return;
      }
      const bool registering = name[3] == ndn::name::Component("register");
      if (registering) {
        ++registrations;
      }
      else if (name[3] == ndn::name::Component("unregister")) {
        ++unregistrations;
      }
      const bool reject = registering && registrations <= failures;
      parameters.setFaceId(100).setOrigin(ndn::nfd::ROUTE_ORIGIN_APP);
      if (registering) {
        parameters.setCost(0);
      }
      ndn::nfd::ControlResponse response;
      response.setCode(reject ? 503 : 200).setText(reject ? "try again" : "OK");
      response.setBody(parameters.wireEncode());
      auto data = std::make_shared<ndn::Data>(name);
      data->setContent(response.wireEncode());
      keys.sign(*data, ndn::security::signingWithSha256());
      boost::asio::post(face.getIoContext(), [this, data] { face.receive(*data); });
    });
  }

  void pumpFor(std::chrono::milliseconds duration)
  {
    auto& io = face.getIoContext();
    io.restart();
    auto work = boost::asio::make_work_guard(io);
    io.run_for(duration);
  }

  bool waitForRegistrations(size_t expected)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (registrations < expected && std::chrono::steady_clock::now() < deadline) {
      pumpFor(std::chrono::milliseconds(10));
    }
    return registrations == expected;
  }

  ndn::KeyChain keys;
  ndn::security::Certificate certificate;
  ndn::DummyClientFace face;
  std::unique_ptr<IdentityRegistrationUser> user;
  size_t failures = 1;
  size_t registrations = 0;
  size_t unregistrations = 0;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ServiceUserIdentityRegistration)

BOOST_FIXTURE_TEST_CASE(FailedRegistrationRetriesThenUnregistersOnDestruction,
                        IdentityRegistrationFixture)
{
  user->registerIdentityPrefixWithRetry();
  BOOST_REQUIRE(waitForRegistrations(2));
  pumpFor(std::chrono::milliseconds(350));
  BOOST_CHECK_EQUAL(registrations, 2);
  BOOST_CHECK_EQUAL(unregistrations, 0);

  user.reset();
  pumpFor(std::chrono::milliseconds(350));
  BOOST_CHECK_EQUAL(registrations, 2);
  BOOST_CHECK_EQUAL(unregistrations, 1);
}

BOOST_FIXTURE_TEST_CASE(DestructionCancelsPendingRegistrationRetry,
                        IdentityRegistrationFixture)
{
  failures = 8;
  user->registerIdentityPrefixWithRetry();
  BOOST_REQUIRE(waitForRegistrations(1));
  pumpFor(std::chrono::milliseconds(20));
  user.reset();
  pumpFor(std::chrono::milliseconds(500));
  BOOST_CHECK_EQUAL(registrations, 1);
}

BOOST_FIXTURE_TEST_CASE(RegistrationStopsAfterEightFailures,
                        IdentityRegistrationFixture)
{
  failures = 9;
  user->registerIdentityPrefixWithRetry();
  BOOST_REQUIRE(waitForRegistrations(8));
  pumpFor(std::chrono::milliseconds(350));
  BOOST_CHECK_EQUAL(registrations, 8);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
