#include "ndn-service-framework/common.hpp"
#include "tests/boost-test.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework::test {
namespace {

struct RecoveryResult
{
  size_t dropped = 0;
  size_t delivered = 0;
  int64_t deliveryMs = -1;
};

RecoveryResult
probeNotificationLoss(bool drop)
{
  boost::asio::io_context io;
  ndn::KeyChain keys{"pib-memory:spec223-notification", "tpm-memory:spec223-notification"};
  const ndn::DummyClientFace::Options faceOptions{true, true};
  ndn::DummyClientFace producerFace(io, keys, faceOptions);
  ndn::DummyClientFace consumerFace(io, keys, faceOptions);
  const ndn::Name group(drop ? "/spec223/lost/group" : "/spec223/control/group");
  const ndn::Name producer(drop ? "/spec223/lost/producer" : "/spec223/control/producer");
  const ndn::Name consumer(drop ? "/spec223/lost/consumer" : "/spec223/control/consumer");
  const auto name = ndn::Name(producer).append("output");
  RecoveryResult result;
  bool armed = false;
  auto publishedAt = ndn::time::steady_clock::now();
  ndn::signal::ScopedConnection producerInterest =
    producerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (armed && drop && result.dropped == 0 && group.isPrefixOf(interest.getName())) {
      ++result.dropped;
      return;
    }
    boost::asio::post(io, [&, packet = interest] { consumerFace.receive(packet); });
  });
  ndn::signal::ScopedConnection consumerInterest =
    consumerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    boost::asio::post(io, [&, packet = interest] { producerFace.receive(packet); });
  });
  ndn::signal::ScopedConnection producerData =
    producerFace.onSendData.connect([&](const ndn::Data& data) {
    boost::asio::post(io, [&, packet = data] { consumerFace.receive(packet); });
  });
  ndn::signal::ScopedConnection consumerData =
    consumerFace.onSendData.connect([&](const ndn::Data& data) {
    boost::asio::post(io, [&, packet = data] { producerFace.receive(packet); });
  });
  ndn::svs::SVSPubSubOptions options;
  options.syncProtocol.version = ndn::svs::SvsProtocolVersion::V3;
  configureSvsPubSubOptionsFromEnvironment(options);
  ndn::svs::SecurityOptions security(keys);
  ndn::svs::SVSPubSub sender(group, producer, producerFace, [](const auto&) {}, options, security);
  ndn::svs::SVSPubSub receiver(group, consumer, consumerFace, [](const auto&) {}, options, security);
  receiver.subscribe(producer, [&](const auto& publication) {
    if (publication.name != name)
      return;
    ++result.delivered;
    result.deliveryMs = ndn::time::duration_cast<ndn::time::milliseconds>(
      ndn::time::steady_clock::now() - publishedAt).count();
    io.stop();
  });
  // Drain registration and initial empty-state traffic before injecting loss.
  producerFace.processEvents(ndn::time::milliseconds(600), true);
  armed = true;
  publishedAt = ndn::time::steady_clock::now();
  const std::vector<uint8_t> payload{'p', 'r', 'o', 'b', 'e'};
  sender.publish(name, ndn::span<const uint8_t>(payload.data(), payload.size()));
  producerFace.processEvents(ndn::time::milliseconds(5200), true);
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec223SvsLoss)

BOOST_AUTO_TEST_CASE(FirstLostNotificationRecoversWithinAckBudget)
{
  struct RestoreEnvironment
  {
    std::map<std::string, std::optional<std::string>> values;
    RestoreEnvironment()
    {
      for (const auto* key : {"NDNSF_SVS_PERIODIC_SYNC_MS", "NDNSF_SVS_MAX_APP_PARAMS_BYTES",
                              "NDNSF_SVS_MAX_PIGGYDATA_BYTES"}) {
        const auto* value = std::getenv(key);
        values[key] = value ? std::optional<std::string>(value) : std::nullopt;
        ::unsetenv(key);
      }
    }
    ~RestoreEnvironment()
    {
      for (const auto& [key, value] : values) {
        if (value) ::setenv(key.c_str(), value->c_str(), 1);
        else ::unsetenv(key.c_str());
      }
    }
  } restore;
  // This tests configured transport recovery, not application authorization.
  const auto control = probeNotificationLoss(false);
  BOOST_REQUIRE_EQUAL(control.delivered, 1);
  BOOST_REQUIRE_LE(control.deliveryMs, 5000);
  const auto lost = probeNotificationLoss(true);
  BOOST_REQUIRE_EQUAL(lost.dropped, 1);
  BOOST_CHECK_EQUAL(lost.delivered, 1);
  BOOST_CHECK_MESSAGE(lost.deliveryMs >= 0 && lost.deliveryMs <= 5000,
                      "lost notification delivery ms=" << lost.deliveryMs);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
