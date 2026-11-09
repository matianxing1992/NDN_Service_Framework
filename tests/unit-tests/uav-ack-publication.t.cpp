#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/io_context.hpp>

#include <array>

namespace ndn_service_framework::test {
namespace {

class PublicationBoundaryUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;

  std::array<uint64_t, 4>
  publicationBounds(const ndn::Name& requestId) const
  {
    const auto& pending = m_pendingCalls.at(requestId);
    return {pending.createdAtUs, pending.publishedAtUs,
            pending.ackWindowDeadlineUs, pending.requestDeadlineUs};
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavAckPublication)

BOOST_AUTO_TEST_CASE(PreparationDoesNotConsumeAckWindow)
{
  RequestMessage published;
  bool publicationCompleted = false;
  uint64_t acceptedAtUs = 0;
  ndn::security::KeyChain keyChain("pib-memory:spec228-publication",
                                  "tpm-memory:spec228-publication");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/spec228/user");
  const ndn::Name provider("/spec228/provider");
  const ndn::Name service("/spec228/service");
  const ndn::Name requestId("/spec228/publication-boundary");
  const auto certificate = makeRsaIdentity(keyChain, requester);
  const auto authority = makeRsaIdentity(keyChain, "/spec228/authority");
  IsolatedTestTrust trust{certificate, authority};
  PublicationBoundaryUser user(face, "/spec228/group", certificate, authority,
                               trust.schemaPath());
  installUserPermissions(user, requester, service, {provider});
  user.setLocalPublicationHandler([&] (const ndn::Name& name, const ndn::Buffer& wire) {
    if (name != makeRequestNameV2(requester, service, requestId)) return;
    BOOST_REQUIRE(published.WireDecode(ndn::Block(wire)));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    acceptedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    publicationCompleted = true;
  });
  const ndn::Buffer payload(1, 0x42);
  user.BeginCollaboration(service, payload, 1000, 5000,
    [] (const CollaborationAckClosure&) {}, [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {}, requestId);
  BOOST_REQUIRE(publicationCompleted);
  const auto bounds = user.publicationBounds(requestId);
  BOOST_CHECK_GE(bounds[1], acceptedAtUs);
  BOOST_CHECK_EQUAL(bounds[2] - bounds[1], 1'000'000U);
  BOOST_CHECK_EQUAL(bounds[3] - bounds[0], 5'000'000U);
  BOOST_REQUIRE(user.getRequestStatus(requestId));
  BOOST_CHECK_GE(user.getRequestStatus(requestId)->publishTimestampUs, acceptedAtUs);
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

namespace {

void
checkQueuedPublication(bool cancelBeforePublish, bool expireBeforePublish = false)
{
  uint64_t queueReleasedAtUs = 0;
  uint64_t ackClosedAtUs = 0;
  size_t closedAcks = 0;
  bool queueDrained = false;
  bool timedOut = false;
  BootstrapProfile profile;
  profile.deferBridgeDelivery = true;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.user().setHandlerThreads(0);
  environment.enableProductionIngressForTest();
  auto& user = environment.user();
  auto& sync = environment.userPubSub().getSVSync().getCore();
  const auto node = sync.getNodeId();
  const auto sequenceBefore = sync.getSeqNo(node);
  boost::asio::post(environment.userFace().getIoContext(), [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(expireBeforePublish ? 1200 : 600));
    queueReleasedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  });
  const ndn::Buffer payload(1, 0x42);
  const auto requestId = user.BeginCollaboration(profile.serviceName, payload,
    expireBeforePublish ? 100 : 1000, expireBeforePublish ? 1000 : 5000,
    [&] (const CollaborationAckClosure&) {
      ++closedAcks;
      ackClosedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    },
    [] (const ResponseMessage&) {}, [&] (const ndn::Name&) { timedOut = true; });
  BOOST_REQUIRE(user.getRequestStatus(requestId));
  BOOST_CHECK_EQUAL(user.getRequestStatus(requestId)->publishTimestampUs, 0U);
  if (cancelBeforePublish) BOOST_REQUIRE(user.CancelCollaboration(requestId));
  boost::asio::post(environment.userFace().getIoContext(), [&] { queueDrained = true; });
  environment.pumpUntilWithAttributeAuthority([&] {
    const auto status = user.getRequestStatus(requestId);
    return queueDrained && queueReleasedAtUs != 0 && status &&
      (cancelBeforePublish || (expireBeforePublish ? timedOut :
       status->publishTimestampUs != 0 || timedOut));
  });
  BOOST_REQUIRE(queueReleasedAtUs != 0);
  const auto status = user.getRequestStatus(requestId);
  BOOST_REQUIRE(status);
  if (cancelBeforePublish || expireBeforePublish) {
    BOOST_CHECK_EQUAL(status->publishTimestampUs, 0U);
    BOOST_CHECK_EQUAL(sync.getSeqNo(node), sequenceBefore);
    BOOST_CHECK(status->state == (expireBeforePublish
      ? ServiceUser::RequestLifecycleState::TIMED_OUT
      : ServiceUser::RequestLifecycleState::CANCELLED_OR_DROPPED));
  }
  else {
    BOOST_CHECK_GE(status->publishTimestampUs, queueReleasedAtUs);
    BOOST_CHECK_GT(sync.getSeqNo(node), sequenceBefore);
    environment.pumpUntilWithAttributeAuthority([&] { return closedAcks != 0 || timedOut; });
    BOOST_REQUIRE(!timedOut);
    BOOST_REQUIRE_EQUAL(closedAcks, 1U);
    BOOST_REQUIRE_GE(ackClosedAtUs, status->publishTimestampUs);
    BOOST_CHECK_GE(ackClosedAtUs - status->publishTimestampUs, 1'000'000U);
    BOOST_CHECK(user.CancelCollaboration(requestId));
  }
  BOOST_CHECK_EQUAL(closedAcks, cancelBeforePublish || expireBeforePublish ? 0U : 1U);
  BOOST_CHECK_EQUAL(timedOut, expireBeforePublish);
}

} // namespace

BOOST_AUTO_TEST_CASE(ActualFaceQueueDoesNotStartAckBeforePublication)
{
  checkQueuedPublication(false);
}

BOOST_AUTO_TEST_CASE(CancelledQueuedPublicationRemainsTerminal)
{
  checkQueuedPublication(true);
}

BOOST_AUTO_TEST_CASE(ExpiredQueuedPublicationCannotResetTotalDeadline)
{
  checkQueuedPublication(false, true);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
