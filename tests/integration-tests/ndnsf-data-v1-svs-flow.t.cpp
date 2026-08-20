#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di::tests {
namespace {

using Bytes = ProviderGroupBytes;

ProviderGroupCoordinatorOptions
makeOptions()
{
  ProviderGroupCoordinatorOptions options;
  options.randomBytes = [] (std::size_t size) {
    return Bytes(size, 0x4d);
  };
  options.wrapEpochKey = [] (const std::string& provider, const Bytes& key) {
    Bytes result(provider.begin(), provider.end());
    result.push_back(':');
    result.insert(result.end(), key.begin(), key.end());
    return result;
  };
  const Bytes signingKey(32, 0x73);
  auto sign = [signingKey] (const Bytes& input) {
    Bytes mixed = signingKey;
    mixed.insert(mixed.end(), input.begin(), input.end());
    Bytes result(32, 0);
    for (std::size_t index = 0; index < mixed.size(); ++index) {
      result[index % result.size()] ^= mixed[index];
    }
    return result;
  };
  options.signCapability = sign;
  options.signManifest = sign;
  options.verifyCapability = [sign] (const Bytes& input, const Bytes& signature) {
    return sign(input) == signature;
  };
  options.verifyManifest = options.verifyCapability;
  return options;
}

GroupOperationV1
makeOperation(const std::string& consumerRank)
{
  GroupOperationV1 operation;
  operation.operationIndex = 7;
  operation.kind = "ALL_GATHER";
  operation.producerRanks = {"0", "1"};
  operation.consumerRanks = {consumerRank};
  operation.tensorLayoutDigest = "layout-v1";
  operation.maxBytes = 32;
  operation.maxSegments = 2;
  return operation;
}

ndn::svs::SecurityOptions
makeSecurityOptions(ndn::KeyChain& keyChain)
{
  ndn::svs::SecurityOptions options(keyChain);
  options.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  options.dataSigner->signingInfo = ndn::security::signingWithSha256();
  options.pubSigner->signingInfo = ndn::security::signingWithSha256();
  options.validator = std::make_shared<ndn::svs::BaseValidator>();
  options.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  return options;
}

template<typename Done>
void
pump(std::vector<ndn::DummyClientFace*> faces, Done&& done, int rounds = 1200)
{
  for (int round = 0; round < rounds && !done(); ++round) {
    for (auto* face : faces) {
      face->processEvents(ndn::time::milliseconds(5));
    }
    for (auto* face : faces) {
      face->getIoContext().restart();
    }
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(NdnsfDataV1SvsFlow)

BOOST_AUTO_TEST_CASE(IndependentSegmentsUseSvsMappingRepairAndReplayFence)
{
  boost::asio::io_context ioP0;
  boost::asio::io_context ioP1;
  boost::asio::io_context ioReceiver;
  ndn::KeyChain keyChain("pib-memory:ndnsf-data-v1-svs",
                         "tpm-memory:ndnsf-data-v1-svs");

  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace p0Face(ioP0, keyChain, faceOptions);
  ndn::DummyClientFace p1Face(ioP1, keyChain, faceOptions);
  ndn::DummyClientFace receiverFace(ioReceiver, keyChain, faceOptions);

  auto securityOptions = makeSecurityOptions(keyChain);
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;
  svsOptions.repairRequestRepeatCount = 1;
  svsOptions.publicationFetchRetries = 2;
  svsOptions.publicationFetchInnerRetries = 1;
  svsOptions.publicationFetchInterestLifetime = ndn::time::milliseconds(50);
  svsOptions.publicationFetchMinInterestLifetime = ndn::time::milliseconds(50);
  svsOptions.publicationFetchMaxInterestLifetime = ndn::time::milliseconds(200);

  const ndn::Name syncPrefix("/ndnsf/data-v1-svs/sync");
  const ndn::Name p0Node("/ndnsf/data-v1-svs/p0");
  const ndn::Name p1Node("/ndnsf/data-v1-svs/p1");
  const ndn::Name receiverNode("/ndnsf/data-v1-svs/receiver");
  ndn::svs::SVSPubSub p0Pub(syncPrefix, p0Node, p0Face,
                            [] (const auto&) {}, svsOptions, securityOptions);
  ndn::svs::SVSPubSub p1Pub(syncPrefix, p1Node, p1Face,
                            [] (const auto&) {}, svsOptions, securityOptions);
  ndn::svs::SVSPubSub receiverPub(syncPrefix, receiverNode, receiverFace,
                                  [] (const auto&) {}, svsOptions, securityOptions);

  const std::vector<ndn::DummyClientFace*> p0Peers{&p1Face, &receiverFace};
  const std::vector<ndn::DummyClientFace*> p1Peers{&p0Face, &receiverFace};
  const std::vector<ndn::DummyClientFace*> receiverPeers{&p0Face, &p1Face};
  auto forwardInterests = [] (ndn::DummyClientFace& from,
                              std::vector<ndn::DummyClientFace*> peers) {
    return from.onSendInterest.connect(
      [peers = std::move(peers)] (const ndn::Interest& interest) {
        for (auto* peer : peers) {
          peer->receive(interest);
        }
      });
  };
  auto forwardData = [] (ndn::DummyClientFace& from,
                         std::vector<ndn::DummyClientFace*> peers,
                         std::string* dropInnerName,
                         bool* dropped,
                         std::vector<std::string>* observedInnerNames) {
    return from.onSendData.connect(
      [peers = std::move(peers), dropInnerName, dropped, observedInnerNames]
      (const ndn::Data& data) {
        if (dropInnerName != nullptr && dropped != nullptr && !*dropped &&
            !dropInnerName->empty() && data.getContent().value_size() != 0) {
          try {
            const ndn::Data inner(data.getContent().blockFromValue());
            if (observedInnerNames != nullptr) {
              observedInnerNames->push_back(inner.getName().toUri());
            }
            if (inner.getName().toUri() == *dropInnerName) {
              *dropped = true;
              return;
            }
          }
          catch (const std::exception&) {
            // This is not the target publication; forward it normally.
          }
        }
        for (auto* peer : peers) {
          peer->receive(data);
        }
      });
  };

  auto p0InterestConnection = forwardInterests(p0Face, p0Peers);
  auto p1InterestConnection = forwardInterests(p1Face, p1Peers);
  auto receiverInterestConnection = forwardInterests(receiverFace, receiverPeers);
  std::string dropInnerName;
  bool droppedTarget = false;
  std::vector<std::string> observedP1InnerNames;
  auto p0DataConnection = forwardData(p0Face, p0Peers, nullptr, nullptr, nullptr);
  auto p1DataConnection = forwardData(p1Face, p1Peers, &dropInnerName,
                                       &droppedTarget, &observedP1InnerNames);
  auto receiverDataConnection = forwardData(receiverFace, receiverPeers,
                                             nullptr, nullptr, nullptr);

  ProviderGroupCoordinator p0(makeOptions());
  const auto operation = makeOperation("2");
  const auto capability = p0.createCapability(
    "/request/svs", "attempt-1", "plan-svs", "group-svs", 3,
    {{p0Node.toUri(), 0, "offer-p0", p0Node.toUri()},
     {p1Node.toUri(), 1, "offer-p1", p1Node.toUri()},
     {receiverNode.toUri(), 2, "offer-receiver", receiverNode.toUri()}},
    {operation}, 64, 1000, 5000);
  const auto epochKey = p0.epochKeyForProvider(p0Node.toUri());
  ProviderGroupCoordinator p1(makeOptions());
  p1.installCapability(capability, epochKey, true);
  ProviderGroupCoordinator receiver(makeOptions());
  receiver.installCapability(capability, epochKey, true);

  const auto sealedP0 = p0.sealOperation(
    operation, "0", "src-0", "dst", "tensor-0", {{'a', 'b'}, {'c', 'd'}}, 100);
  const auto sealedP1 = p1.sealOperation(
    operation, "1", "src-1", "dst", "tensor-1", {{'e', 'f'}, {'g', 'h'}}, 100);
  dropInnerName = sealedP1.segments[1].dataName;

  size_t accepted = 0;
  size_t duplicates = 0;
  std::vector<std::string> failures;
  auto consume = [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
    try {
      const Bytes wire(publication.data.begin(), publication.data.end());
      const auto decoded = ProviderGroupCoordinator::decodeSegment(wire);
      const auto result = receiver.acceptSegment(decoded.manifest,
                                                 decoded.segments.front());
      if (result == DataSegmentReplayWindow::Result::Accepted) {
        ++accepted;
      }
      else {
        ++duplicates;
      }
    }
    catch (const std::exception& error) {
      failures.push_back(error.what());
    }
  };
  receiverPub.subscribeToProducer(p0Node, consume, true);
  receiverPub.subscribeToProducer(p1Node, consume, true);

  // Publish out of order per Provider. SVS assigns independent sequence
  // numbers and fetches the missing mapping/data through the same bridge.
  p0Pub.publish(sealedP0.segments[1].dataName,
               ProviderGroupCoordinator::encodeSegment(
                 sealedP0.manifest, sealedP0.segments[1]));
  p0Pub.publish(sealedP0.segments[0].dataName,
               ProviderGroupCoordinator::encodeSegment(
                 sealedP0.manifest, sealedP0.segments[0]));
  p1Pub.publish(sealedP1.segments[1].dataName,
               ProviderGroupCoordinator::encodeSegment(
                 sealedP1.manifest, sealedP1.segments[1]));
  p1Pub.publish(sealedP1.segments[0].dataName,
               ProviderGroupCoordinator::encodeSegment(
                 sealedP1.manifest, sealedP1.segments[0]));

  pump({&p0Face, &p1Face, &receiverFace}, [&] {
    return accepted == 4 || !failures.empty();
  });
  if (!failures.empty()) {
    BOOST_FAIL(failures.front());
  }
  BOOST_REQUIRE_EQUAL(accepted, 4U);
  if (!droppedTarget) {
    BOOST_TEST_MESSAGE("drop target=" << dropInnerName);
    for (const auto& name : observedP1InnerNames) {
      BOOST_TEST_MESSAGE("observed p1 inner=" << name);
    }
  }
  BOOST_CHECK(droppedTarget);

  // A new SVS publication carrying the same segment is an exact duplicate at
  // the NDNSF_DATA_V1 operation window, not a second accepted tensor.
  p0Pub.publish(sealedP0.segments[0].dataName,
                ProviderGroupCoordinator::encodeSegment(
                  sealedP0.manifest, sealedP0.segments[0]));
  pump({&p0Face, &p1Face, &receiverFace}, [&] { return duplicates >= 1; }, 400);
  BOOST_CHECK_GE(duplicates, 1U);
  BOOST_CHECK(receiver.terminal() == false);
}

BOOST_AUTO_TEST_CASE(ProductionProviderContextUsesSvsSegments)
{
  boost::asio::io_context ioProducer;
  boost::asio::io_context ioReceiver;
  ndn::KeyChain keyChain("pib-memory:ndnsf-data-v1-provider-context",
                         "tpm-memory:ndnsf-data-v1-provider-context");

  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace producerFace(ioProducer, keyChain, faceOptions);
  ndn::DummyClientFace receiverFace(ioReceiver, keyChain, faceOptions);

  auto securityOptions = makeSecurityOptions(keyChain);
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;
  svsOptions.repairRequestRepeatCount = 1;
  svsOptions.publicationFetchRetries = 2;
  svsOptions.publicationFetchInnerRetries = 1;
  svsOptions.publicationFetchInterestLifetime = ndn::time::milliseconds(50);
  svsOptions.publicationFetchMinInterestLifetime = ndn::time::milliseconds(50);
  svsOptions.publicationFetchMaxInterestLifetime = ndn::time::milliseconds(200);

  const ndn::Name syncPrefix("/ndnsf/data-v1-provider-context/sync");
  const ndn::Name producerNode("/ndnsf/data-v1-provider-context/producer");
  const ndn::Name receiverNode("/ndnsf/data-v1-provider-context/receiver");
  auto producerPub = std::make_shared<ndn::svs::SVSPubSub>(
      syncPrefix, producerNode, producerFace, [] (const auto&) {},
      svsOptions, securityOptions);
  bool receiverObservedProducerState = false;
  auto receiverPub = std::make_shared<ndn::svs::SVSPubSub>(
      syncPrefix, receiverNode, receiverFace,
      [&] (const std::vector<ndn::svs::MissingDataInfo>& updates) {
        receiverObservedProducerState = std::any_of(
          updates.begin(), updates.end(), [&] (const auto& update) {
            return update.nodeId == producerNode && update.high >= 2;
          });
      },
      svsOptions, securityOptions);

  auto producerCert = keyChain.createIdentity(
      ndn::Name("/test/provider/data-v1-producer"), ndn::RsaKeyParams(2048))
                        .getDefaultKey().getDefaultCertificate();
  auto receiverCert = keyChain.createIdentity(
      ndn::Name("/test/provider/data-v1-receiver"), ndn::RsaKeyParams(2048))
                        .getDefaultKey().getDefaultCertificate();
  auto authorityCert = keyChain.createIdentity(
      ndn::Name("/test/authority/data-v1"), ndn::RsaKeyParams(2048))
                         .getDefaultKey().getDefaultCertificate();

  using FrameworkProvider = ndn_service_framework::ServiceProvider;
  FrameworkProvider producer(FrameworkProvider::LocalMockTag{}, producerFace,
                             ndn::Name("/test/group"), producerCert,
                             authorityCert, "examples/trust-any.conf");
  FrameworkProvider receiver(FrameworkProvider::LocalMockTag{}, receiverFace,
                             ndn::Name("/test/group"), receiverCert,
                             authorityCert, "examples/trust-any.conf");
  producer.attachLocalMockPubSubForTest(producerPub);
  receiver.attachLocalMockPubSubForTest(receiverPub);

  auto producerInterestConnection = producerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) { receiverFace.receive(interest); });
  auto receiverInterestConnection = receiverFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  auto producerDataConnection = producerFace.onSendData.connect(
      [&] (const ndn::Data& data) { receiverFace.receive(data); });
  auto receiverDataConnection = receiverFace.onSendData.connect(
      [&] (const ndn::Data& data) { producerFace.receive(data); });

  ProviderGroupCoordinator producerCoordinator(makeOptions());
  const auto operation = makeOperation("1");
  const auto capability = producerCoordinator.createCapability(
      "/request/provider-context", "attempt-1", "plan-provider-context",
      "group-provider-context", 3,
      {{producerNode.toUri(), 0, "offer-producer", producerNode.toUri()},
       {receiverNode.toUri(), 1, "offer-receiver", receiverNode.toUri()}},
      {operation}, 64, 1000, 5000);
  const auto epochKey = producerCoordinator.epochKeyForProvider(
      producerNode.toUri());
  ProviderGroupCoordinator receiverCoordinator(makeOptions());
  receiverCoordinator.installCapability(capability, epochKey, true);
  const auto sealed = producerCoordinator.sealOperation(
      operation, "0", "src-layout", "dst-layout", "tensor-provider-context",
      {{'a', 'b'}, {'c', 'd'}}, 100);

  // The receiver subscribes to a shared SVS producer stream.  Publish an
  // older request with the same operation/rank/tensor tuple first; the
  // request-scoped name filter must ignore it during catch-up instead of
  // filling the current request's segment slot and deferring the mismatch to
  // ProviderGroupCoordinator.
  ProviderGroupCoordinator staleCoordinator(makeOptions());
  const auto staleCapability = staleCoordinator.createCapability(
      "/request/provider-context-old", "attempt-1", "plan-provider-context",
      "group-provider-context-old", 3,
      {{producerNode.toUri(), 0, "offer-producer", producerNode.toUri()},
       {receiverNode.toUri(), 1, "offer-receiver", receiverNode.toUri()}},
      {operation}, 64, 1000, 5000);
  BOOST_REQUIRE_EQUAL(staleCapability.requestId, "/request/provider-context-old");
  const auto staleSealed = staleCoordinator.sealOperation(
      operation, "0", "src-layout", "dst-layout", "tensor-provider-context",
      {{'x', 'y'}, {'z', 'w'}}, 100);

  FrameworkProvider::CollaborationAssignment assignment;
  assignment.role = "worker";
  assignment.service = ndn::Name("/DI/collective");
  FrameworkProvider::CollaborationContext producerContext(
      producer, ndn::Name("/test/user"), ndn::Name("/request/provider-context"),
      ndn_service_framework::RequestMessage(), assignment);
  FrameworkProvider::CollaborationContext receiverContext(
      receiver, ndn::Name("/test/user"), ndn::Name("/request/provider-context"),
      ndn_service_framework::RequestMessage(), assignment);

  auto makePublication = [&] (std::size_t index) {
    const auto wire = ProviderGroupCoordinator::encodeSegment(
        sealed.manifest, sealed.segments[index]);
    return std::make_pair(ndn::Name(sealed.segments[index].dataName),
                          ndn::Buffer(wire.begin(), wire.end()));
  };
  const std::vector<std::pair<ndn::Name, ndn::Buffer>> publications = {
      makePublication(0), makePublication(1)};
  producerPub->publish(
      staleSealed.segments[0].dataName,
      ProviderGroupCoordinator::encodeSegment(
          staleSealed.manifest, staleSealed.segments[0]));
  BOOST_REQUIRE(producerContext.publishDataV1Segments(
      "/scope/provider-context", publications, 60000));

  // Reproduce the production ordering: both publications are synchronized
  // before the downstream Provider posts its dependency subscription.
  pump({&producerFace, &receiverFace},
       [&] { return receiverObservedProducerState; });
  BOOST_REQUIRE(receiverObservedProducerState);

  auto fetched = std::async(std::launch::async, [&] {
    return receiverContext.fetchDataV1Segments(
        "/scope/provider-context", producerNode, operation.operationIndex,
        "0", "tensor-provider-context", sealed.segments.size(),
        operation.maxSegments, 3000);
  });

  pump({&producerFace, &receiverFace}, [&] {
    return fetched.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto wires = fetched.get();
  BOOST_REQUIRE(wires);
  BOOST_REQUIRE_EQUAL(wires->size(), sealed.segments.size());

  std::vector<std::uint8_t> payload;
  for (const auto& wire : *wires) {
    const auto decoded = ProviderGroupCoordinator::decodeSegment(
        Bytes(wire.begin(), wire.end()));
    BOOST_REQUIRE_EQUAL(decoded.segments.size(), 1U);
    BOOST_CHECK_EQUAL(decoded.manifest.requestId, "/request/provider-context");
    const auto result = receiverCoordinator.acceptSegment(
        decoded.manifest, decoded.segments.front());
    BOOST_REQUIRE(result == DataSegmentReplayWindow::Result::Accepted);
    const auto plaintext = receiverCoordinator.openSegment(
        decoded.manifest, decoded.segments.front());
    payload.insert(payload.end(), plaintext.begin(), plaintext.end());
  }
  const std::vector<std::uint8_t> expectedPayload{'a', 'b', 'c', 'd'};
  BOOST_CHECK_EQUAL_COLLECTIONS(payload.begin(), payload.end(),
                                expectedPayload.begin(), expectedPayload.end());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
