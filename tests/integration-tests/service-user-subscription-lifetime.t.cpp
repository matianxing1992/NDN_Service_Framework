#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

namespace ndn_service_framework::test {
namespace {

// Hold actual publication-validation completions, not synthetic subscriptions.
class HeldPublicationValidator final : public ndn::svs::BaseValidator
{
public:
  explicit HeldPublicationValidator(ndn::security::Certificate certificate)
    : certificate(std::move(certificate))
  {}

  void validate(const ndn::Data& data,
                const ndn::security::DataValidationSuccessCallback& success,
                const ndn::security::DataValidationFailureCallback&) override
  {
    BOOST_REQUIRE(ndn::security::verifySignature(data, certificate));
    if (holding)
      pending.emplace_back(std::make_shared<ndn::Data>(data), success);
    else
      success(data);
  }

  void release()
  {
    holding = false;
    auto ready = std::move(pending);
    pending.clear();
    for (const auto& completion : ready) completion.second(*completion.first);
  }

  ndn::security::Certificate certificate;
  bool holding = true;
  std::vector<std::pair<std::shared_ptr<ndn::Data>,
                        ndn::security::DataValidationSuccessCallback>> pending;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ServiceUserSubscriptionLifetime)

BOOST_AUTO_TEST_CASE(PendingValidationOutlivesUserButSharedSubscriberContinues)
{
  ndn::KeyChain keys("pib-memory:subscription-lifetime", "tpm-memory:subscription-lifetime");
  const auto userCertificate = makeRsaIdentity(keys, ndn::Name("/test/lifetime/user"));
  const auto producerCertificate = makeRsaIdentity(keys, ndn::Name("/test/lifetime/producer"));
  boost::asio::io_context io;
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace receiverFace(io, keys, faceOptions), senderFace(io, keys, faceOptions);
  auto work = boost::asio::make_work_guard(io);
  auto connect = [&io](ndn::DummyClientFace& from, ndn::DummyClientFace& to) {
    from.onSendInterest.connect([&io, &to](const ndn::Interest& interest) {
      boost::asio::post(io, [&to, interest] { to.receive(interest); });
    });
    from.onSendData.connect([&io, &to](const ndn::Data& data) {
      boost::asio::post(io, [&to, data] { to.receive(data); });
    });
  };
  connect(receiverFace, senderFace);
  connect(senderFace, receiverFace);
  auto held = std::make_shared<HeldPublicationValidator>(producerCertificate);
  ndn::svs::SecurityOptions receiverSecurity(keys), senderSecurity(keys);
  receiverSecurity.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  senderSecurity.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  receiverSecurity.validator = std::make_shared<ndn::svs::BaseValidator>();
  senderSecurity.validator = std::make_shared<ndn::svs::BaseValidator>();
  receiverSecurity.encapsulatedDataValidator = held;
  senderSecurity.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  receiverSecurity.dataSigner->signingInfo = ndn::security::signingByCertificate(userCertificate);
  receiverSecurity.pubSigner->signingInfo = ndn::security::signingByCertificate(userCertificate);
  senderSecurity.dataSigner->signingInfo = ndn::security::signingByCertificate(producerCertificate);
  senderSecurity.pubSigner->signingInfo = ndn::security::signingByCertificate(producerCertificate);
  const ndn::Name sync("/test/lifetime/sync");
  auto receiver = std::make_shared<ndn::svs::SVSPubSub>(
    sync, ndn::Name(userCertificate.getIdentity()).append("user").append("0"), receiverFace,
    [](const std::vector<ndn::svs::MissingDataInfo>&) {},
    ndn::svs::SVSPubSubOptions{}, receiverSecurity);
  auto sender = std::make_shared<ndn::svs::SVSPubSub>(
    sync, ndn::Name(producerCertificate.getIdentity()).append("provider").append("0"), senderFace,
    [](const std::vector<ndn::svs::MissingDataInfo>&) {},
    ndn::svs::SVSPubSubOptions{}, senderSecurity);
  auto user = std::make_unique<LocalServiceUser>(receiverFace, ndn::Name("/test/lifetime"),
    userCertificate, producerCertificate, "examples/trust-any.conf");
  user->attachLocalMockPubSubForTest(receiver);
  user->init();
  user->init();
  std::vector<ndn::Name> received;
  const auto survivor = receiver->subscribeWithRegex(ndn::Regex("^(<>*)<NDNSF><ACK>(<>*)$"),
    [&received](const ndn::svs::SVSPubSub::SubscriptionData& data) {
      BOOST_REQUIRE(data.packet);
      BOOST_CHECK(data.packet->getName() == data.name);
      received.push_back(data.name);
    }, true, true);
  auto pumpUntil = [&](const std::function<bool()>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < end) {
      io.restart();
      io.run_for(std::chrono::milliseconds(10));
    }
    return done();
  };
  const auto first = ndn::Name(producerCertificate.getIdentity()).append("NDNSF").append("ACK").append("first");
  const auto second = ndn::Name(producerCertificate.getIdentity()).append("NDNSF").append("ACK").append("second");
  const std::vector<uint8_t> payload{1, 2, 3};
  sender->publish(first, payload);
  BOOST_REQUIRE(pumpUntil([&] { return !held->pending.empty(); }));
  BOOST_CHECK(received.empty());
  // No Face callback runs concurrently with this destruction.
  user.reset();
  boost::asio::post(io, [held] { held->release(); });
  BOOST_REQUIRE(pumpUntil([&] { return received.size() == 1; }));
  BOOST_CHECK(received.front() == first);
  sender->publish(second, payload);
  BOOST_REQUIRE(pumpUntil([&] { return received.size() == 2; }));
  BOOST_CHECK(received.back() == second);
  BOOST_CHECK(held->pending.empty());
  receiver->unsubscribe(survivor);
  // Drain posted bridge deliveries while Faces and all callback owners survive.
  sender.reset();
  receiver.reset();
  io.restart();
  io.run_for(std::chrono::milliseconds(20));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
