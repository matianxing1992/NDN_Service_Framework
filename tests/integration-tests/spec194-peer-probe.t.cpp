/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ndn_service_framework {

struct ServiceProviderTestAccess
{
  static void observeGoodputStages(ServiceProvider& provider,
      std::function<void(const char*, size_t, size_t)> observer)
  {
    provider.m_peerGoodputStageObserver = std::move(observer);
  }
  static void anchorProbeValidation(ServiceProvider& receiver, ServiceProvider& source)
  {
    // The general fixture's trust-any schema bypasses signature checks. This
    // signature oracle instead uses the real validator with the source anchor.
    receiver.nac_validator.load(
      "rule { id probe-signatures for data checker { type hierarchical "
      "sig-type rsa-sha256 } }", "spec197-probe-signature-oracle");
    auto& keys = source.m_testSigningKeyChain ? *source.m_testSigningKeyChain : source.m_keyChain;
    auto certificate = keys.getPib().getIdentity(source.getName())
      .getDefaultKey().getDefaultCertificate();
    receiver.nac_validator.loadAnchor("spec197-probe-source", std::move(certificate));
  }
  static std::optional<ControllerVersion> controllerVersion(
      const ServiceProvider& provider, const ndn::Name& service)
  {
    return provider.getControllerVersion(service);
  }

  static void publishRequestAck(ServiceProvider& provider,
                                const ndn::Name& requester,
                                const ndn::Name& service,
                                const ndn::Name& requestId,
                                const ndn::Buffer& payload,
                                const RequestMessage& sourceRequest,
                                const ServiceProvider::AckDecision& decision)
  {
    provider.PublishRequestAckMessageV2(requester, service, requestId, true,
                                        "ready", payload, "user-token",
                                        "provider-token", &sourceRequest,
                                        &decision);
  }

  static bool ackPacketWithinLimit(ServiceProvider& provider,
                                  const ndn::Name& ackName,
                                  const HybridMessageEnvelope& envelope,
                                  std::size_t ciphertextBytes)
  {
    return provider.isHybridAckPacketWithinLimit(ackName, envelope, ciphertextBytes);
  }

  static std::optional<std::size_t> firstFittingAckCandidate(
      ServiceProvider& provider,
      const ndn::Name& ackName,
      const HybridMessageEnvelope& envelope,
      const std::vector<ndn::Buffer>& candidates)
  {
    return provider.firstHybridAckCandidateWithinLimit(ackName, envelope, candidates);
  }

  static void publishHybridAckCandidates(ServiceProvider& provider,
                                         const ndn::Name& ackName,
                                         std::vector<ndn::Buffer> candidates)
  {
    provider.publishHybridEncodedMessageCandidates(ackName, std::move(candidates));
  }

  static bool authorizeActiveProbeRequest(
    ServiceProvider& provider,
    const std::string& requestScope,
    const std::vector<ndn::Name>& candidates,
    const std::vector<NetworkTelemetryDataEdge>& rttPairs,
    const std::vector<NetworkTelemetryDataEdge>& edges,
    std::chrono::steady_clock::time_point expiresAt)
  {
    return provider.m_networkTelemetry.authorizeActiveProbeRequest(
      requestScope, candidates, rttPairs, edges, expiresAt);
  }

  static void signProbeData(ServiceProvider& provider, ndn::Data& data)
  {
    (provider.m_testSigningKeyChain ? *provider.m_testSigningKeyChain : provider.m_keyChain)
      .sign(data, provider.m_signingInfo);
  }

  static void queueSourceSegment(ServiceProvider& provider, const ndn::Name& observer,
                                 const std::string& scope, const std::string& nonce,
                                 size_t segment)
  {
    ndn::Name name = provider.getName();
    name.append("NDNSF").append("PEER-GOODPUT").append("test")
      .append(nonce).appendSegment(segment);
    provider.queuePeerGoodputTrain(ndn::Interest(name), observer, scope, nonce, segment);
  }

  static size_t pendingSourceTrains(const ServiceProvider& provider)
  {
    return provider.m_sourceGoodputTrains.size();
  }

  static bool authorizeSourceWithRegistration(ServiceProvider& provider,
      const std::string& scope, const std::vector<ndn::Name>& candidates,
      const std::vector<NetworkTelemetryDataEdge>& edges,
      const std::shared_ptr<std::atomic<bool>>& cancelled)
  {
    return provider.m_networkTelemetry.authorizeActiveProbeRequest(scope, candidates, {}, edges,
      std::chrono::steady_clock::now() + std::chrono::seconds(5), cancelled);
  }

};

namespace test {

using ndnsf::di::NativeJson;

struct PlacementProbeCall
{
  RequestMessage request;
  std::string scopeDigest;
  std::vector<ndn::Name> roster;
  std::vector<NetworkTelemetryDataEdge> rttPairs;
};

PlacementProbeCall
makePlacementProbeCall(const ndn::Name& requester,
                        const ndn::Name& observer,
                        const ndn::Name& target,
                        const ndn::Name& service,
                        std::vector<ndn::Name> roster,
                        bool reverseRttEdge = false,
                        const std::string& probeSequence = "one",
                        uint64_t scopeBudgetMs = 2000)
{
  std::sort(roster.begin(), roster.end());
  std::vector<std::string> rosterUris;
  for (const auto& provider : roster) rosterUris.push_back(provider.toUri());
  const std::string requestId = "/spec196/placement/request/" + probeSequence;
  const std::string intentNonce =
    ndnsf::di::nativePlanningDigest(probeSequence).substr(7, 32);
  const auto deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() + scopeBudgetMs);
  const auto rosterDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(NativeJson(rosterUris)));
  const auto source = (reverseRttEdge ? target : observer).toUri();
  const auto destination = (reverseRttEdge ? observer : target).toUri();
  NativeJson rttPairs = NativeJson::array({
    NativeJson{{"src_provider", source}, {"dst_provider", destination}}});
  const auto scope = NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", requestId}, {"attempt", 1},
    {"requester_identity", requester.toUri()},
    {"observer_provider", observer.toUri()}, {"service", service.toUri()},
    {"provider_roster", rosterUris}, {"roster_digest", rosterDigest},
    {"rtt_pairs", rttPairs}, {"goodput_edges", NativeJson::array()},
    {"deadline_ms", deadlineMs}, {"intent_nonce", intentNonce}};
  const auto scopeDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(scope));
  const auto wire = ndnsf::di::nativeCanonicalJson(NativeJson{
    {"action", "measure"},
    {"schema", "ndnsf-di-placement-probe-request-v1"},
    {"request_id", requestId}, {"attempt", 1},
    {"requester_identity", requester.toUri()},
    {"observer_provider", observer.toUri()}, {"service", service.toUri()},
    {"provider_roster", rosterUris}, {"roster_digest", rosterDigest},
    {"rtt_pairs", rttPairs}, {"goodput_edges", NativeJson::array()},
    {"measurement_kinds", NativeJson::array({"active_probe_rtt"})},
    {"deadline_ms", deadlineMs}, {"intent_nonce", intentNonce},
    {"scope_digest", scopeDigest}});
  ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(wire.data()),
                      wire.size());
  RequestMessage request;
  request.setTargetProvider(observer);
  request.setPayload(payload, payload.size());
  std::vector<NetworkTelemetryDataEdge> edges{
    {ndn::Name(source), ndn::Name(destination)}};
  return {std::move(request), scopeDigest, std::move(roster), std::move(edges)};
}

RequestMessage
makePlacementScopeAuthorizationRequest(const PlacementProbeCall& call,
                                       const ndn::Name& target)
{
  const auto payload = call.request.getPayload();
  const std::string wire(reinterpret_cast<const char*>(payload.data()), payload.size());
  auto requestJson = ndnsf::di::nativeParseJson(wire);
  requestJson["action"] = "authorize";
  const auto authorizationWire = ndnsf::di::nativeCanonicalJson(requestJson);
  ndn::Buffer authorizationPayload(
    reinterpret_cast<const std::uint8_t*>(authorizationWire.data()),
    authorizationWire.size());
  auto request = call.request;
  request.setTargetProvider(target);
  request.setPayload(authorizationPayload, authorizationPayload.size());
  request.setTokens({{"targeted.refill", "1"}});
  return request;
}

bool isActiveProbeMeasurementInterest(const ndn::Interest& interest)
{
  const auto uri = interest.getName().toUri();
  return uri.find("/PEER-GOODPUT/") != std::string::npos ||
         uri.find("/PEER-PROBE/") != std::string::npos;
}

BOOST_AUTO_TEST_SUITE(Spec194PeerProbe)

BOOST_AUTO_TEST_CASE(SignedAckPreviewMeasuresBothDataLayersAgainstNdnLimit)
{
  BootstrapProfile profile;
  profile.providerCount = 1;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();

  HybridMessageEnvelope envelope;
  envelope.setKeyId("test-key-id");
  envelope.setEpochId("test-epoch-id");
  envelope.setMessageType("ACK");
  envelope.setWrappedMessageKey(ndn::Buffer(512));
  const ndn::Name ackName("/spec194/provider/ACK/request");
  auto& provider = env.provider(0);

  BOOST_CHECK(ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, envelope, 1024));
  BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, envelope, ndn::MAX_NDN_PACKET_SIZE));
}

BOOST_AUTO_TEST_CASE(CoreMergesOnlyNamespacedV4AndPreservesV3AckPayload)
{
  BootstrapProfile profile;
  profile.providerCount = 1;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();

  auto& provider = env.provider(0);
  const ndn::Name requester("/spec194/user");
  const ndn::Name service("/spec194/service");
  const ndn::Name requestId("/spec194/request/core-merge");
  std::optional<ndn::Buffer> published;
  provider.setLocalPublicationHandler(
    [&] (const ndn::Name&, const ndn::Buffer& wire) { published = wire; });

  DeploymentIntent intent;
  intent.setField("legacyCapability", "keep");
  RequestMessage request;
  request.setDeploymentIntent(intent);
  const std::string v3PayloadText =
    R"({"schema":"DI_PLACEMENT_V3","payload":"preserve-byte-for-byte"})";
  ndn::Buffer v3Payload(v3PayloadText.begin(), v3PayloadText.end());
  ServiceProvider::AckDecision decision;
  decision.providerCapabilityFields = {
    {"ndnsf.di.placement.v4", "signed-v4-offer"}
  };
  decision.providerCapabilityFieldFallbacks = {
    {{"ndnsf.di.placement.v4", "signed-v4-truncated"}}, {}
  };

  ServiceProviderTestAccess::publishRequestAck(
    provider, requester, service, requestId, v3Payload, request, decision);
  BOOST_REQUIRE(published);

  RequestAckMessage ack;
  BOOST_REQUIRE(ack.WireDecode(ndn::Block(*published)));
  BOOST_REQUIRE(ack.hasProviderCapabilityOffer());
  const auto& offer = ack.getProviderCapabilityOffer();
  BOOST_CHECK(offer.hasField("providerIdentity"));
  BOOST_CHECK(offer.hasField("intentDigest"));
  BOOST_REQUIRE(offer.hasField("ndnsf.di.placement.v4"));
  BOOST_CHECK_EQUAL(offer.getField("ndnsf.di.placement.v4"), "signed-v4-offer");
  const auto decodedPayload = ack.getPayload();
  BOOST_REQUIRE_EQUAL(decodedPayload.size(), v3Payload.size());
  BOOST_CHECK(std::equal(decodedPayload.begin(), decodedPayload.end(),
                         v3Payload.begin()));
}

BOOST_AUTO_TEST_CASE(OversizedV4CandidatesSelectBaseOfferAndRetainV3Payload)
{
  BootstrapProfile profile;
  profile.providerCount = 1;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();

  auto& provider = env.provider(0);
  HybridMessageEnvelope envelope;
  envelope.setKeyId("test-key-id");
  envelope.setEpochId("test-epoch-id");
  envelope.setMessageType("ACK");
  envelope.setWrappedMessageKey(ndn::Buffer(512));
  const ndn::Name ackName("/spec194/provider/ACK/request");
  const std::string v3PayloadText = "{\"schema\":\"DI_PLACEMENT_V3\",\"data\":\"" +
    std::string(6200, 'v') + "\"}";
  ndn::Buffer v3Payload(v3PayloadText.begin(), v3PayloadText.end());

  ProviderCapabilityOffer baseOffer;
  baseOffer.setField("legacyCapability", "keep");
  auto base = RequestAckMessage();
  base.setStatus(true);
  base.setPayload(v3Payload, v3Payload.size());
  base.setProviderCapabilityOffer(baseOffer);

  auto fullOffer = baseOffer;
  fullOffer.setField("ndnsf.di.placement.v4", std::string(3900, 'f'));
  auto full = base;
  full.setProviderCapabilityOffer(fullOffer);

  auto trimmedOffer = baseOffer;
  trimmedOffer.setField("ndnsf.di.placement.v4", std::string(3000, 't'));
  auto trimmed = base;
  trimmed.setProviderCapabilityOffer(trimmedOffer);

  const auto bytes = [] (const RequestAckMessage& value) {
    const auto wire = value.WireEncode();
    return ndn::Buffer(wire.begin(), wire.end());
  };
  const std::vector<ndn::Buffer> candidates{bytes(full), bytes(trimmed), bytes(base)};
  BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, envelope, candidates[0].size()));
  BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, envelope, candidates[1].size()));
  BOOST_CHECK(ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, envelope, candidates[2].size()));
  const auto selected = ServiceProviderTestAccess::firstFittingAckCandidate(
    provider, ackName, envelope, candidates);
  BOOST_REQUIRE(selected);
  BOOST_CHECK_EQUAL(*selected, 2U);
  RequestAckMessage finalAck;
  BOOST_REQUIRE(finalAck.WireDecode(ndn::Block(candidates[*selected])));
  BOOST_REQUIRE(finalAck.hasProviderCapabilityOffer());
  BOOST_CHECK(!finalAck.getProviderCapabilityOffer().hasField("ndnsf.di.placement.v4"));
  BOOST_CHECK_EQUAL(finalAck.getProviderCapabilityOffer().getField("legacyCapability"), "keep");
  const auto actualPayload = finalAck.getPayload();
  BOOST_REQUIRE_EQUAL(actualPayload.size(), v3Payload.size());
  BOOST_CHECK(std::equal(actualPayload.begin(), actualPayload.end(), v3Payload.begin()));
}

BOOST_AUTO_TEST_CASE(ProductionAckPublisherSelectsFittingBaseAndPublishesSignedOuterData)
{
  BootstrapProfile profile;
  profile.providerCount = 1;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();

  auto& provider = env.provider(0);
  const ndn::Name requester("/spec194/user");
  const ndn::Name service("/spec194/service");
  const ndn::Name requestId("/spec194/request/published-ack-fallback");
  const auto ackName = makeRequestAckNameV2(
    provider.getName(), requester, service, requestId);

  const std::string v3PayloadText = R"({"schema":"DI_PLACEMENT_V3","data":")" +
                                    std::string(6200, 'v') + "\"}";
  ndn::Buffer v3Payload(v3PayloadText.begin(), v3PayloadText.end());
  ProviderCapabilityOffer baseOffer;
  baseOffer.setField("legacyCapability", "keep");

  RequestAckMessage base;
  base.setStatus(true);
  base.setPayload(v3Payload, v3Payload.size());
  base.setProviderCapabilityOffer(baseOffer);
  auto trimmedOffer = baseOffer;
  trimmedOffer.setField("ndnsf.di.placement.v4", std::string(3000, 't'));
  auto trimmed = base;
  trimmed.setProviderCapabilityOffer(trimmedOffer);
  auto fullOffer = baseOffer;
  fullOffer.setField("ndnsf.di.placement.v4", std::string(3900, 'f'));
  auto full = base;
  full.setProviderCapabilityOffer(fullOffer);

  const auto toBuffer = [] (const RequestAckMessage& ack) {
    const auto block = ack.WireEncode();
    return ndn::Buffer(block.begin(), block.end());
  };
  std::vector<ndn::Buffer> candidates{toBuffer(full), toBuffer(trimmed), toBuffer(base)};

  HybridMessageEnvelope preview;
  preview.setKeyId("test-key-id");
  preview.setEpochId("test-epoch-id");
  preview.setMessageType("ACK");
  preview.setWrappedMessageKey(ndn::Buffer(512));
  BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, preview, candidates[0].size()));
  BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, preview, candidates[1].size()));
  BOOST_CHECK(ServiceProviderTestAccess::ackPacketWithinLimit(
    provider, ackName, preview, candidates[2].size()));

  std::optional<ndn::Data> publishedOuter;
  auto publicationConnection = env.providerFace(0).onSendData.connect(
    [&] (const ndn::Data& outer) {
      try {
        if (outer.getContent().value_size() == 0) {
          return;
        }
        const ndn::Data inner(outer.getContent().blockFromValue());
        if (inner.getName() == ackName) {
          publishedOuter = outer;
        }
      }
      catch (const std::exception&) {
        // Ignore non-SVS Data and publications for other names.
      }
    });

  ServiceProviderTestAccess::publishHybridAckCandidates(
    provider, ackName, std::move(candidates));
  env.pumpUntil([&] { return publishedOuter.has_value(); });

  BOOST_REQUIRE(publishedOuter);
  BOOST_CHECK_LE(publishedOuter->wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
  BOOST_CHECK(publishedOuter->getSignatureValue().value_size() > 0);
  const ndn::Data inner(publishedOuter->getContent().blockFromValue());
  BOOST_CHECK_EQUAL(inner.getName(), ackName);
  BOOST_CHECK_LE(inner.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
  BOOST_CHECK(inner.getSignatureValue().value_size() > 0);

  HybridMessageEnvelope publishedEnvelope;
  BOOST_REQUIRE(publishedEnvelope.WireDecode(inner.getContent().blockFromValue()));
  // AES-GCM ciphertext length equals the selected plaintext candidate length.
  // This observes the actual production selection after signing/encryption.
  BOOST_CHECK_EQUAL(publishedEnvelope.getCipherText().size(), toBuffer(base).size());

  const ndn::Name oversizedRequestId("/spec194/request/published-ack-no-fit");
  const auto oversizedAckName = makeRequestAckNameV2(
    provider.getName(), requester, service, oversizedRequestId);
  const std::string oversizedV3Text = R"({"schema":"DI_PLACEMENT_V3","data":")" +
                                      std::string(9200, 'x') + "\"}";
  ndn::Buffer oversizedV3Payload(oversizedV3Text.begin(), oversizedV3Text.end());
  RequestAckMessage oversizedBase;
  oversizedBase.setStatus(true);
  oversizedBase.setPayload(oversizedV3Payload, oversizedV3Payload.size());
  oversizedBase.setProviderCapabilityOffer(baseOffer);
  auto oversizedTrimmed = oversizedBase;
  auto oversizedTrimmedOffer = baseOffer;
  oversizedTrimmedOffer.setField("ndnsf.di.placement.v4", std::string(3000, 't'));
  oversizedTrimmed.setProviderCapabilityOffer(oversizedTrimmedOffer);
  auto oversizedFull = oversizedBase;
  auto oversizedFullOffer = baseOffer;
  oversizedFullOffer.setField("ndnsf.di.placement.v4", std::string(3900, 'f'));
  oversizedFull.setProviderCapabilityOffer(oversizedFullOffer);
  std::vector<ndn::Buffer> noFitCandidates{
    toBuffer(oversizedFull), toBuffer(oversizedTrimmed), toBuffer(oversizedBase)};
  for (const auto& candidate : noFitCandidates) {
    BOOST_CHECK(!ServiceProviderTestAccess::ackPacketWithinLimit(
      provider, oversizedAckName, preview, candidate.size()));
  }

  std::map<std::size_t, std::pair<ndn::Data, ndn::Data>> oversizedSegments;
  std::optional<std::size_t> expectedFinalSegment;
  auto oversizedPublicationConnection = env.providerFace(0).onSendData.connect(
    [&] (const ndn::Data& outer) {
      try {
        if (outer.getContent().value_size() == 0) {
          return;
        }
        const ndn::Data innerData(outer.getContent().blockFromValue());
        const auto& innerName = innerData.getName();
        if (innerName.size() >= 2 && innerName[-1].isSegment() &&
            innerName[-2].isVersion() &&
            innerName.getPrefix(-2) == oversizedAckName) {
          const auto segmentNo = innerName[-1].toSegment();
          const auto finalBlock = innerData.getFinalBlock();
          if (finalBlock && finalBlock->isSegment()) {
            expectedFinalSegment = finalBlock->toSegment();
            oversizedSegments.emplace(segmentNo,
                                      std::make_pair(outer, innerData));
          }
        }
      }
      catch (const std::exception&) {
        // Ignore non-SVS Data and publications for other names.
      }
    });
  std::optional<ndn::Buffer> oversizedPublishedValue;
  const auto subscription = env.userPubSub().subscribeToProducer(
    env.profile().providerNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (publication.name == oversizedAckName) {
        oversizedPublishedValue.emplace(
          publication.data.begin(), publication.data.end());
      }
    });
  ServiceProviderTestAccess::publishHybridAckCandidates(
    provider, oversizedAckName, std::move(noFitCandidates));
  env.pumpUntil([&] {
    return oversizedPublishedValue.has_value() && expectedFinalSegment &&
           oversizedSegments.size() == *expectedFinalSegment + 1;
  });
  env.userPubSub().unsubscribe(subscription);

  BOOST_REQUIRE(oversizedPublishedValue);
  BOOST_REQUIRE(expectedFinalSegment);
  BOOST_REQUIRE_EQUAL(oversizedSegments.size(), *expectedFinalSegment + 1);
  for (std::size_t segmentNo = 0; segmentNo <= *expectedFinalSegment; ++segmentNo) {
    const auto segment = oversizedSegments.find(segmentNo);
    BOOST_REQUIRE(segment != oversizedSegments.end());
    BOOST_CHECK_LE(segment->second.first.wireEncode().size(),
                   ndn::MAX_NDN_PACKET_SIZE);
    BOOST_CHECK(segment->second.first.getSignatureValue().value_size() > 0);
    BOOST_CHECK_LE(segment->second.second.wireEncode().size(),
                   ndn::MAX_NDN_PACKET_SIZE);
    BOOST_CHECK(segment->second.second.getSignatureValue().value_size() > 0);
  }
  HybridMessageEnvelope oversizedEnvelope;
  BOOST_REQUIRE(oversizedEnvelope.WireDecode(
    ndn::Block(*oversizedPublishedValue)));
  BOOST_CHECK_EQUAL(oversizedEnvelope.getCipherText().size(),
                    toBuffer(oversizedBase).size());
}

BOOST_AUTO_TEST_CASE(UnregisteredRequestScopeEmitsNoPeerProbeInterests)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();

  auto& sourceProvider = env.provider(0);
  auto& observerProvider = env.provider(1);
  const auto source = sourceProvider.getName();
  const auto observer = observerProvider.getName();
  const std::vector<ndn::Name> candidates{source, observer};
  const std::vector<NetworkTelemetryDataEdge> edges{{source, observer}};
  const std::string unregisteredScope = "sha256:" + std::string(64, '4');

  std::vector<ndn::Interest> transmittedInterests;
  auto interestConnection = env.providerFace(1).onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (isActiveProbeMeasurementInterest(interest)) {
        transmittedInterests.push_back(interest);
      }
    });
  std::optional<ServiceProvider::PeerProbeCollection> result;
  observerProvider.collectCandidatePeerObservations(
    candidates, {{observer, source}}, edges, unregisteredScope,
    std::chrono::steady_clock::now() + std::chrono::seconds(2), nullptr,
    [&] (ServiceProvider::PeerProbeCollection collection) {
      result = std::move(collection);
    });
  env.pumpUntil([&] { return result.has_value(); });

  BOOST_REQUIRE(result);
  BOOST_CHECK_EQUAL(result->requestedTargetCount, 1);
  BOOST_CHECK_EQUAL(result->omittedTargetCount, 1);
  BOOST_CHECK_EQUAL(result->requestedGoodputEdgeCount, 1);
  BOOST_CHECK_EQUAL(result->omittedGoodputEdgeCount, 1);
  BOOST_CHECK(result->observations.empty());
  BOOST_CHECK(std::none_of(
    transmittedInterests.begin(), transmittedInterests.end(),
    [] (const ndn::Interest& interest) {
      return isActiveProbeMeasurementInterest(interest);
    }));
}

BOOST_AUTO_TEST_CASE(TargetedPlacementMeasurementUsesOnlyRequestedProviderEdge)
{
  BootstrapProfile profile;
  profile.providerCount = 3;
  profile.userAdditionalServices = {ndn::Name(NETWORK_PROBE_SERVICE)};
  profile.providerAdditionalServices = {ndn::Name(NETWORK_PROBE_SERVICE)};
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProductionIngressForTest();
  env.pumpUntilReady();

  auto& observerProvider = env.provider(0);
  auto& targetProvider = env.provider(1);
  const auto observer = observerProvider.getName();
  const auto target = targetProvider.getName();
  const auto requester = env.user().getName();
  const ndn::Name service(env.profile().serviceName);
  env.user().setUseTokens(false);
  observerProvider.setUseTokens(false);
  std::vector<ndn::Name> roster{
    observer, target, env.provider(2).getName()};

  std::vector<ndn::Name> probeTargets;
  std::size_t readinessInterestCount = 0;
  auto interestConnection = env.providerFace(0).onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName().toUri().find("/NDNSF/PEER-PROBE-READY/") !=
          std::string::npos) {
        ++readinessInterestCount;
      }
      if (isActiveProbeMeasurementInterest(interest)) {
        const auto& name = interest.getName();
        if (name.size() >= target.size())
          probeTargets.push_back(name.getPrefix(target.size()));
      }
    });
  std::optional<ResponseMessage> response;
  auto measurementHandler = ndnsf::di::makeNativePlacementMeasurementTargetedHandler(
    observerProvider);
  std::atomic<std::size_t> targetedHandlerCallCount{0};
  ServiceProvider::AsyncTargetedRequestHandler handler =
    [measurementHandler = std::move(measurementHandler), &targetedHandlerCallCount]
    (const ndn::Name& requesterName, const ndn::Name& providerName,
     const ndn::Name& serviceName, const ndn::Name& requestId,
     const RequestMessage& request, ServiceProvider::AsyncTargetedCompletion complete) mutable {
      ++targetedHandlerCallCount;
      measurementHandler(requesterName, providerName, serviceName, requestId,
                         request, std::move(complete));
    };
  std::atomic<bool> ordinaryAckHandlerCalled{false};
  auto registration = observerProvider.addScopedCollaborationHandler(
    service, {},
    [&] (const RequestMessage&) {
      ordinaryAckHandlerCalled = true;
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "ready";
      return decision;
    },
    ServiceProvider::AsyncAckStrategyHandler{},
    [] (ServiceProvider::CollaborationContext&,
        const RequestMessage&) {});
  const ndn::Name probeService(NETWORK_PROBE_SERVICE);
  auto probeRegistration = observerProvider.addScopedTargetedService(
    probeService, std::move(handler));
  auto targetMeasurementHandler =
    ndnsf::di::makeNativePlacementMeasurementTargetedHandler(targetProvider);
  std::atomic<std::size_t> targetTargetedHandlerCallCount{0};
  ServiceProvider::AsyncTargetedRequestHandler targetHandler =
    [measurementHandler = std::move(targetMeasurementHandler),
     &targetTargetedHandlerCallCount]
    (const ndn::Name& requesterName, const ndn::Name& providerName,
     const ndn::Name& serviceName, const ndn::Name& requestId,
     const RequestMessage& request, ServiceProvider::AsyncTargetedCompletion complete) mutable {
      ++targetTargetedHandlerCallCount;
      measurementHandler(requesterName, providerName, serviceName, requestId,
                         request, std::move(complete));
    };
  auto targetRegistration = targetProvider.addScopedTargetedService(
    probeService, std::move(targetHandler));
  bool ordinaryAckReceived = false;
  bool ordinaryAckAccepted = false;
  std::string ordinaryAckDetail = "no candidates";
  RequestMessage ordinaryRequest;
  ordinaryRequest.setPolicyEpoch(env.user().getCurrentPolicyEpoch());
  const auto ordinaryRequestId = env.user().RequestService(
    std::vector<ndn::Name>{observer}, service, ordinaryRequest, 1500,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        ordinaryAckReceived = true;
        if (!candidates.empty()) {
          ordinaryAckDetail = candidates.front().ack.getMessage();
        }
        ordinaryAckAccepted = candidates.size() == 1 &&
          candidates.front().providerName.equals(observer) &&
          candidates.front().ack.getStatus();
        return std::vector<AckSelectionCandidate>{};
      }),
    5000,
    [] (const ndn::Name&) {},
    [] (const ResponseMessage&) {});
  BOOST_REQUIRE(!ordinaryRequestId.empty());
  env.pumpUntil([&] { return ordinaryAckReceived; });
  BOOST_REQUIRE_MESSAGE(ordinaryAckAccepted,
                        "ordinary ACK was rejected beside targeted handler: " +
                        ordinaryAckDetail + "; handlerCalled=" +
                        (ordinaryAckHandlerCalled.load() ? "true" : "false"));

  // The native requester has no reusable ProviderToken for this observer on
  // its first measurement call. Exercise TargetedBootstrapRequest through
  // ACK/Selection so the async targeted handler receives the probe payload.
  env.user().setUseTokens(true);
  targetProvider.setUseTokens(true);
  observerProvider.setUseTokens(true);
  auto call = makePlacementProbeCall(requester, observer, target, service,
                                     roster);
  std::optional<ResponseMessage> authorizationResponse;
  bool authorizationTimedOut = false;
  const auto authorizationRequestId = env.user().RequestServiceTargeted(
    target, probeService, makePlacementScopeAuthorizationRequest(call, target), 3000,
    [&] (const ndn::Name&) { authorizationTimedOut = true; },
    [&] (const ResponseMessage& value) { authorizationResponse = value; });
  BOOST_REQUIRE(!authorizationRequestId.empty());
  env.pumpUntil([&] {
    return authorizationResponse.has_value() || authorizationTimedOut;
  });
  BOOST_REQUIRE_MESSAGE(authorizationResponse,
    "target Provider scope authorization timed out");
  BOOST_REQUIRE_MESSAGE(authorizationResponse->getStatus(),
                        authorizationResponse->getErrorInfo());
  const auto authorizationPayload = authorizationResponse->getPayload();
  const auto authorizationAck = ndnsf::di::nativeParseJson(std::string(
    reinterpret_cast<const char*>(authorizationPayload.data()),
    authorizationPayload.size()));
  BOOST_CHECK_EQUAL(authorizationAck.at("provider").get<std::string>(),
                    target.toUri());
  BOOST_CHECK_EQUAL(authorizationAck.at("observer_provider").get<std::string>(),
                    observer.toUri());
  BOOST_CHECK_EQUAL(authorizationAck.at("scope_digest").get<std::string>(),
                    call.scopeDigest);
  auto targetedRequest = call.request;
  targetedRequest.setTokens({{"targeted.refill", "1"}});
  bool timedOut = false;
  const auto targetedRequestId = env.user().RequestServiceTargeted(
    observer, probeService, std::move(targetedRequest), 3000,
    [&] (const ndn::Name&) { timedOut = true; },
    [&] (const ResponseMessage& value) { response = value; });
  BOOST_REQUIRE(!targetedRequestId.empty());
  env.pumpUntil([&] { return response.has_value() || timedOut; });

  BOOST_REQUIRE_MESSAGE(response,
    "targeted bootstrap timed out; async handler called=" +
    std::to_string(targetedHandlerCallCount.load()));
  BOOST_REQUIRE_MESSAGE(response->getStatus(), response->getErrorInfo());
  BOOST_REQUIRE_EQUAL(probeTargets.size(), 1);
  BOOST_CHECK_EQUAL(probeTargets.front().toUri(), target.toUri());
  BOOST_CHECK_EQUAL(readinessInterestCount, 0);
  const auto responsePayload = response->getPayload();
  const auto decoded = ndnsf::di::nativeParseJson(std::string(
    reinterpret_cast<const char*>(responsePayload.data()),
    responsePayload.size()));
  BOOST_CHECK_EQUAL(decoded.at("schema").get<std::string>(),
                    "ndnsf-di-placement-probe-response-v1");
  BOOST_CHECK_EQUAL(decoded.at("observer_provider").get<std::string>(),
                    observer.toUri());
  BOOST_CHECK_EQUAL(decoded.at("scope_digest").get<std::string>(),
                    call.scopeDigest);
  BOOST_CHECK_LE(responsePayload.size(), 4096);
  BOOST_CHECK_EQUAL(decoded.at("requested_rtt_count").get<std::size_t>(), 1);

  // The bootstrap response installs a reusable ProviderToken pair. A second
  // placement probe must use the TargetedRequest fast path and reach the same
  // measurement handler without being decoded as a collaboration assignment.
  auto cachedTokenCall = makePlacementProbeCall(
    requester, observer, target, service, roster, false, "two");
  std::optional<ResponseMessage> cachedAuthorizationResponse;
  bool cachedAuthorizationTimedOut = false;
  const auto cachedAuthorizationRequestId = env.user().RequestServiceTargeted(
    target, probeService,
    makePlacementScopeAuthorizationRequest(cachedTokenCall, target), 3000,
    [&] (const ndn::Name&) { cachedAuthorizationTimedOut = true; },
    [&] (const ResponseMessage& value) {
      cachedAuthorizationResponse = value;
    });
  BOOST_REQUIRE(!cachedAuthorizationRequestId.empty());
  env.pumpUntil([&] {
    return cachedAuthorizationResponse.has_value() ||
           cachedAuthorizationTimedOut;
  });
  BOOST_REQUIRE_MESSAGE(cachedAuthorizationResponse,
    "cached-token target authorization timed out");
  BOOST_REQUIRE_MESSAGE(cachedAuthorizationResponse->getStatus(),
                        cachedAuthorizationResponse->getErrorInfo());
  std::optional<ResponseMessage> cachedTokenResponse;
  bool cachedTokenTimedOut = false;
  const auto cachedTokenRequestId = env.user().RequestServiceTargeted(
    observer, probeService, cachedTokenCall.request, 3000,
    [&] (const ndn::Name&) { cachedTokenTimedOut = true; },
    [&] (const ResponseMessage& value) { cachedTokenResponse = value; });
  BOOST_REQUIRE(!cachedTokenRequestId.empty());
  env.pumpUntil([&] {
    return cachedTokenResponse.has_value() || cachedTokenTimedOut;
  });
  BOOST_REQUIRE_MESSAGE(cachedTokenResponse,
    "cached-token targeted request timed out; async handler calls=" +
    std::to_string(targetedHandlerCallCount.load()));
  BOOST_REQUIRE_MESSAGE(cachedTokenResponse->getStatus(),
                        cachedTokenResponse->getErrorInfo());
  BOOST_CHECK_EQUAL(targetedHandlerCallCount.load(), 2);
  BOOST_CHECK_EQUAL(targetTargetedHandlerCallCount.load(), 2);
  BOOST_REQUIRE_EQUAL(probeTargets.size(), 2);
  BOOST_CHECK_EQUAL(probeTargets[1].toUri(), target.toUri());
  BOOST_CHECK_EQUAL(readinessInterestCount, 0);

  probeRegistration.close();
  probeRegistration.close(); // idempotent close
  std::optional<ResponseMessage> closedResponse;
  bool closedTimedOut = false;
  const auto closedRequest = env.user().RequestServiceTargeted(
    observer, probeService, cachedTokenCall.request, 200,
    [&] (const ndn::Name&) { closedTimedOut = true; },
    [&] (const ResponseMessage& value) { closedResponse = value; });
  BOOST_REQUIRE(!closedRequest.empty());
  env.pumpUntil([&] { return closedTimedOut || closedResponse.has_value(); });
  BOOST_CHECK(closedTimedOut || (closedResponse && !closedResponse->getStatus()));
  BOOST_CHECK_EQUAL(targetedHandlerCallCount.load(), 2);
  BOOST_CHECK_EQUAL(probeTargets.size(), 2);

  // Closing the probe registration leaves the model's ACK path registered.
  env.user().setUseTokens(false);
  observerProvider.setUseTokens(false);
  ordinaryAckReceived = false;
  ordinaryAckAccepted = false;
  const auto modelAfterClose = env.user().RequestService(
    std::vector<ndn::Name>{observer}, service, ordinaryRequest, 1500,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        ordinaryAckReceived = true;
        ordinaryAckAccepted = candidates.size() == 1 &&
          candidates.front().providerName.equals(observer) &&
          candidates.front().ack.getStatus();
        return std::vector<AckSelectionCandidate>{};
      }), 5000, [] (const ndn::Name&) {}, [] (const ResponseMessage&) {});
  BOOST_REQUIRE(!modelAfterClose.empty());
  env.pumpUntil([&] { return ordinaryAckReceived; });
  BOOST_CHECK(ordinaryAckAccepted);
  env.user().setUseTokens(true);
  observerProvider.setUseTokens(true);

  // A newer Controller permission projection withdraws only the probe
  // service. Previously issued probe tokens must not bypass that withdrawal.
  PermissionResponse withdrawn;
  withdrawn.setTargetIdentity(requester.toUri());
  withdrawn.setPermissionKind(tlv::UserPermission);
  withdrawn.setPolicyEpoch(2);
  for (const auto& name : roster) {
    PermissionEntry retained;
    retained.setProviderName(name.toUri());
    retained.setServiceName(service.toUri());
    retained.setToken("");
    retained.setTtl(0);
    retained.setVersion(2);
    withdrawn.addEntry(retained);
  }
  env.user().applyPermissionResponse(withdrawn);
  std::size_t withdrawalInterests = 0;
  auto withdrawalConnection = env.userFace().onSendInterest.connect(
    [&withdrawalInterests] (const ndn::Interest&) { ++withdrawalInterests; });
  bool deniedCallback = false;
  const auto denied = env.user().RequestServiceTargeted(
    observer, probeService, cachedTokenCall.request, 3000,
    [&] (const ndn::Name&) { deniedCallback = true; },
    [&] (const ResponseMessage&) { deniedCallback = true; });
  BOOST_CHECK(denied.empty());
  BOOST_CHECK(!deniedCallback);
  BOOST_CHECK_EQUAL(withdrawalInterests, 0);
  BOOST_CHECK_EQUAL(targetedHandlerCallCount.load(), 2);
  BOOST_CHECK_EQUAL(probeTargets.size(), 2);
}

BOOST_AUTO_TEST_CASE(Spec197TwoSourceWarmedResponseSurvivesEncryptedSegmentationAndMerge)
{
  using namespace ndnsf::di;
  struct ThresholdGuard
  {
    std::optional<std::string> prior;
    ThresholdGuard()
    {
      if (const auto value = std::getenv("NDNSF_RESPONSE_LARGE_DATA_THRESHOLD")) prior = value;
      if (setenv("NDNSF_RESPONSE_LARGE_DATA_THRESHOLD", "4096", 1) != 0)
        throw std::runtime_error("cannot configure response threshold oracle");
    }
    ~ThresholdGuard()
    {
      if (prior) setenv("NDNSF_RESPONSE_LARGE_DATA_THRESHOLD", prior->c_str(), 1);
      else unsetenv("NDNSF_RESPONSE_LARGE_DATA_THRESHOLD");
    }
  } threshold;
  BootstrapProfile profile;
  profile.providerCount = 3;
  profile.userAdditionalServices = {ndn::Name(NETWORK_PROBE_SERVICE)};
  profile.providerAdditionalServices = {ndn::Name(NETWORK_PROBE_SERVICE)};
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready, env.failureReason());
  env.pumpUntilReady();
  env.enableProductionIngressForTest();
  env.pumpUntilReady();
  const auto observer = env.provider(0).getName();
  const auto requester = env.user().getName();
  const ndn::Name probeService(NETWORK_PROBE_SERVICE);
  const ndn::Name inferenceService(env.profile().serviceName);
  std::vector<ndn::Name> roster;
  std::vector<ServiceProvider::ServiceRegistration> registrations;
  env.user().setUseTokens(true);
  for (size_t index = 0; index < 3; ++index) {
    auto& provider = env.provider(index);
    provider.setUseTokens(true);
    roster.push_back(provider.getName());
    registrations.push_back(provider.addScopedTargetedService(
      probeService, makeNativePlacementMeasurementTargetedHandler(provider)));
  }
  // This Controller-free fixture installs the same typed status required by
  // protected Selection; real Controller transport is qualified in MiniNDN.
  const auto bootstrapStatusTimeMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  PolicyStatusData status;
  status.setServiceName(probeService);
  status.setControllerVersion(ControllerVersion{bootstrapStatusTimeMs, 1});
  status.setValidity(bootstrapStatusTimeMs - 1000, bootstrapStatusTimeMs + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ndn::Name("/controller/spec197/test"));
  BOOST_REQUIRE(env.user().installControllerStatus(status));
  const auto userVersion = env.user().getControllerVersion(probeService);
  BOOST_REQUIRE(userVersion);
  BOOST_CHECK_EQUAL(userVersion->compare(status.getControllerVersion()), 0);
  for (size_t index = 0; index < 3; ++index) {
    BOOST_REQUIRE(env.provider(index).installControllerStatus(status));
    const auto version = ServiceProviderTestAccess::controllerVersion(env.provider(index), probeService);
    BOOST_REQUIRE(version);
    BOOST_CHECK_EQUAL(version->compare(*userVersion), 0);
  }
  const auto cryptoReady = [&] {
    if (!env.user().isNacConsumerReadyForTest() || !env.user().isNacProducerReadyForTest()) return false;
    for (size_t index = 0; index < 3; ++index) {
      if (!env.provider(index).isNacConsumerReadyForTest() ||
          !env.provider(index).isNacProducerReadyForTest()) return false;
    }
    return true;
  };
  env.pumpUntilWithAttributeAuthority(cryptoReady);
  BOOST_REQUIRE(cryptoReady());
  auto call = makePlacementProbeCall(requester, observer, env.provider(1).getName(),
                                    inferenceService, roster, false, "warmed-two-source", 5000);
  const auto originalPayload = call.request.getPayload();
  auto request = nativeParseJson(std::string(
    reinterpret_cast<const char*>(originalPayload.data()), originalPayload.size()));
  request["rtt_pairs"] = NativeJson::array();
  request["goodput_edges"] = NativeJson::array();
  NativePlacementProbeObserverV1 intent;
  intent.observerProvider = observer.toUri();
  for (size_t index = 1; index < 3; ++index) {
    const auto source = env.provider(index).getName().toUri();
    request["rtt_pairs"].push_back({{"src_provider", observer.toUri()}, {"dst_provider", source}});
    request["goodput_edges"].push_back({{"src_provider", source}, {"dst_provider", observer.toUri()}});
    intent.rttPairs.push_back({observer.toUri(), source});
    intent.goodputEdges.push_back({source, observer.toUri()});
  }
  request["measurement_kinds"] = NativeJson::array({"active_probe_goodput", "active_probe_rtt"});
  auto scope = request;
  scope.erase("action");
  scope.erase("measurement_kinds");
  scope.erase("scope_digest");
  scope["schema"] = "ndnsf-di-placement-probe-scope-v1";
  call.scopeDigest = nativePlanningDigest(nativeCanonicalJson(scope));
  request["scope_digest"] = call.scopeDigest;
  const auto wire = nativeCanonicalJson(request);
  ndn::Buffer requestPayload(wire.begin(), wire.end());
  call.request.setPayload(requestPayload, requestPayload.size());
  RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  call.request.setRequestCapabilities(capabilities);
  call.request.setTokens({{"targeted.refill", "1"}});
  for (size_t index = 1; index < 3; ++index) {
    std::optional<ResponseMessage> authorization;
    bool timedOut = false;
    BOOST_REQUIRE(!env.user().RequestServiceTargeted(
      env.provider(index).getName(), probeService,
      makePlacementScopeAuthorizationRequest(call, env.provider(index).getName()), 3000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage& response) { authorization = response; }).empty());
    env.pumpUntil([&] { return authorization || timedOut; });
    BOOST_REQUIRE(authorization);
    BOOST_REQUIRE_MESSAGE(authorization->getStatus(), authorization->getErrorInfo());
  }
  size_t encryptedSegments = 0;
  auto segmentConnection = env.providerFace(0).onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName().toUri().find("/NDNSF/LARGE-RESPONSE/") != std::string::npos &&
        !data.getName().empty() && data.getName()[-1].isSegment()) ++encryptedSegments;
  });
  std::optional<ResponseMessage> response;
  bool timedOut = false;
  BOOST_REQUIRE(!env.user().RequestServiceTargeted(
    observer, probeService, call.request, 3000,
    [&] (const ndn::Name&) { timedOut = true; },
    [&] (const ResponseMessage& value) { response = value; }).empty());
  env.pumpUntil([&] { return response || timedOut; });
  BOOST_REQUIRE(response);
  BOOST_REQUIRE_MESSAGE(response->getStatus(), response->getErrorInfo());
  BOOST_REQUIRE(!response->getSignerCertificate().empty());
  BOOST_CHECK(ndn::security::extractIdentityFromCertName(
    ndn::Name(response->getSignerCertificate())) == observer);
  const auto payload = response->getPayload();
  BOOST_CHECK_GT(payload.size(), 4096);
  BOOST_CHECK_LE(payload.size(), NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES);
  BOOST_CHECK_GE(encryptedSegments, 2);
  const auto decoded = nativeParseJson(std::string(
    reinterpret_cast<const char*>(payload.data()), payload.size()));
  BOOST_CHECK_EQUAL(decoded.at("omitted_goodput_count").get<size_t>(), 0);
  BOOST_CHECK_EQUAL(decoded.at("omitted_rtt_count").get<size_t>(), 0);
  BOOST_REQUIRE_EQUAL(decoded.at("observations").size(), 4);

  NativeOfferBindingContext binding;
  binding.requestId = request.at("request_id").get<std::string>();
  binding.attempt = 1;
  binding.requesterIdentity = requester.toUri();
  binding.serviceName = inferenceService.toUri();
  binding.deadlineMs = request.at("deadline_ms").get<uint64_t>();
  binding.candidateProviders = request.at("provider_roster").get<std::vector<std::string>>();
  binding.allowedRttPairs = request.at("rtt_pairs").get<std::vector<NativeJson>>();
  binding.allowedGoodputEdges = request.at("goodput_edges").get<std::vector<NativeJson>>();
  NativePlacementObservationSnapshotV4 snapshot;
  snapshot.expiresAtMs = binding.deadlineMs;
  snapshot.value = {{"eligible_link_observations", NativeJson::array()},
                    {"provider_offers", NativeJson::array()}};
  for (size_t index = 0; index < 3; ++index)
    snapshot.value["provider_offers"].push_back({{"provider", env.provider(index).getName().toUri()},
      {"boot_epoch", env.provider(index).getProviderBootEpoch()}});
  NativePlacementProbeRoundV1 round;
  round.intent.observers.push_back(intent);
  round.intentNonceByObserver[observer.toUri()] = request.at("intent_nonce").get<std::string>();
  round.responseByObserver[observer.toUri()] = decoded;
  const auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  for (const auto& observation : decoded.at("observations")) {
    const auto capturedAtMs = observation.at("observed_at_ms").get<uint64_t>();
    const auto expiresAtMs = observation.at("expires_at_ms").get<uint64_t>();
    BOOST_TEST_MESSAGE("NDNSF_SPEC197_LARGE_RESPONSE_TIMING target=" << observation.at("target")
      << " captured=" << capturedAtMs << " expires=" << expiresAtMs
      << " deadline=" << binding.deadlineMs << " merge=" << nowMs);
    BOOST_REQUIRE_GT(capturedAtMs, 0);
    BOOST_REQUIRE_LE(capturedAtMs, nowMs);
    BOOST_REQUIRE_LE(nowMs - capturedAtMs, 5000);
    BOOST_REQUIRE_GT(expiresAtMs, capturedAtMs);
    BOOST_REQUIRE_LE(expiresAtMs, binding.deadlineMs);
    BOOST_REQUIRE_GT(expiresAtMs, nowMs);
  }
  const auto merged = mergeNativePlacementProbeRoundV1(snapshot, round, binding, nowMs);
  BOOST_REQUIRE_EQUAL(merged.value.at("eligible_link_observations").size(), 4);
  size_t goodputRows = 0;
  for (const auto& item : merged.value.at("eligible_link_observations")) {
    const auto& row = item.at("row");
    if (row.at("kind") == "ACTIVE_PROBE_GOODPUT") {
      ++goodputRows;
      BOOST_CHECK_EQUAL(row.at("probe_segments_sent").get<size_t>(), NETWORK_PROBE_WARMED_SEGMENTS);
      BOOST_CHECK_EQUAL(row.at("probe_data_arrival_gaps_ms").size(), 15);
      BOOST_CHECK_EQUAL(row.at("probe_source_send_gaps_ms").size(), 14);
    }
  }
  BOOST_CHECK_EQUAL(goodputRows, 2);
}

BOOST_AUTO_TEST_CASE(Spec197InferencePermissionDoesNotAuthorizeProbe)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  profile.providerAdditionalServices = {ndn::Name(NETWORK_PROBE_SERVICE)};
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                        env.failureReason());
  env.pumpUntilReady();
  env.enableProductionIngressForTest();
  env.pumpUntilReady();
  auto& provider = env.provider(0);
  std::size_t handlerCalls = 0;
  auto registration = provider.addScopedTargetedService(
    ndn::Name(NETWORK_PROBE_SERVICE),
    [&handlerCalls] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                    const ndn::Name&, const RequestMessage&,
                    ServiceProvider::AsyncTargetedCompletion complete) {
      ++handlerCalls;
      ResponseMessage response;
      response.setStatus(true);
      complete(std::move(response));
    });
  auto call = makePlacementProbeCall(
    env.user().getName(), provider.getName(), env.provider(1).getName(),
    profile.serviceName, {provider.getName(), env.provider(1).getName()});
  std::size_t interests = 0;
  auto connection = env.userFace().onSendInterest.connect(
    [&interests] (const ndn::Interest&) { ++interests; });
  const auto denied = env.user().RequestServiceTargeted(
    provider.getName(), ndn::Name(NETWORK_PROBE_SERVICE), call.request, 3000,
    [] (const ndn::Name&) {}, [] (const ResponseMessage&) {});
  BOOST_CHECK(denied.empty());
  BOOST_CHECK_EQUAL(handlerCalls, 0);
  BOOST_CHECK_EQUAL(interests, 0);
}

BOOST_AUTO_TEST_CASE(TargetedPlacementMeasurementRejectsUnrequestedDirection)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();

  auto& observerProvider = env.provider(0);
  const auto observer = observerProvider.getName();
  const auto target = env.provider(1).getName();
  const auto requester = env.user().getName();
  const ndn::Name service("/spec196/measurement-service");
  auto call = makePlacementProbeCall(
    requester, observer, target, service, {observer, target}, true);
  std::size_t probeCount = 0;
  auto interestConnection = env.providerFace(0).onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (isActiveProbeMeasurementInterest(interest)) ++probeCount;
    });
  std::optional<ResponseMessage> response;
  auto handler = ndnsf::di::makeNativePlacementMeasurementTargetedHandler(
    observerProvider);
  handler(requester, observer, ndn::Name(NETWORK_PROBE_SERVICE),
          ndn::Name("/spec196/targeted/two"),
          call.request, [&response](ResponseMessage value) {
            response = std::move(value);
          });
  env.pumpUntil([&] { return response.has_value(); });

  BOOST_REQUIRE(response);
  BOOST_CHECK(!response->getStatus());
  BOOST_CHECK_EQUAL(probeCount, 0);
}

BOOST_AUTO_TEST_CASE(AuthorizedProbesUseTimestampedPrefixNamesAndTargetSigner)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();
  env.pumpUntilReady();

  auto& targetProvider = env.provider(0);
  auto& observerProvider = env.provider(1);
  const auto target = targetProvider.getName();
  const auto observer = observerProvider.getName();
  const std::vector<ndn::Name> candidates{target, observer};
  const std::vector<NetworkTelemetryDataEdge> rttPairs{{observer, target}};
  const std::vector<NetworkTelemetryDataEdge> edges{{target, observer}};
  const std::string requestScope = "sha256:" + std::string(64, '9');
  const auto expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  BOOST_REQUIRE(ServiceProviderTestAccess::authorizeActiveProbeRequest(
    observerProvider, requestScope, candidates, rttPairs, edges, expiresAt));
  bool targetScopeAuthorized = false;
  auto targetAuthorizationAt = std::chrono::steady_clock::time_point::max();
  auto targetRegistrationTimer = std::make_shared<boost::asio::steady_timer>(
    env.providerFace(0).getIoContext());
  targetRegistrationTimer->expires_after(std::chrono::milliseconds(120));
  targetRegistrationTimer->async_wait(
    [&, targetRegistrationTimer](const boost::system::error_code& error) {
      if (!error) {
        targetScopeAuthorized =
          ServiceProviderTestAccess::authorizeActiveProbeRequest(
            targetProvider, requestScope, candidates, rttPairs, edges,
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
        targetAuthorizationAt = std::chrono::steady_clock::now();
      }
    });

  struct TransmittedProbe
  {
    ndn::Interest interest;
    int64_t sentAtMs;
    std::chrono::steady_clock::time_point sentAt;
  };
  std::vector<TransmittedProbe> transmittedInterests;
  std::vector<ndn::Interest> readinessInterests;
  auto interestConnection = env.providerFace(1).onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      const auto sentAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      if (interest.getName().toUri().find("/PEER-PROBE-READY/") !=
          std::string::npos) {
        readinessInterests.push_back(interest);
      }
      else if (isActiveProbeMeasurementInterest(interest)) {
        transmittedInterests.push_back(
          {interest, sentAtMs, std::chrono::steady_clock::now()});
      }
    });
  std::vector<ndn::Data> targetReplies;
  std::vector<ndn::Data> readinessReplies;
  auto dataConnection = env.providerFace(0).onSendData.connect(
    [&] (const ndn::Data& data) {
      if (data.getName().toUri().find("/PEER-PROBE-READY/") !=
          std::string::npos) {
        readinessReplies.push_back(data);
      }
      else {
        targetReplies.push_back(data);
      }
    });
  std::optional<ServiceProvider::PeerProbeCollection> result;
  observerProvider.collectCandidatePeerObservations(
    candidates, rttPairs, edges, requestScope,
    std::chrono::steady_clock::now() + std::chrono::seconds(2), nullptr,
    [&] (ServiceProvider::PeerProbeCollection collection) {
      result = std::move(collection);
    });
  env.pumpUntil([&] { return result.has_value(); });

  BOOST_REQUIRE(result);
  BOOST_CHECK(targetScopeAuthorized);
  BOOST_CHECK_GE(readinessInterests.size(), 2);
  BOOST_REQUIRE_EQUAL(readinessReplies.size(), 1);
  BOOST_REQUIRE_EQUAL(transmittedInterests.size(), NETWORK_PROBE_WARMED_SEGMENTS + 1);
  BOOST_REQUIRE_EQUAL(targetReplies.size(), NETWORK_PROBE_WARMED_SEGMENTS + 1);

  ndn::Name readinessPrefix = target;
  readinessPrefix.append("NDNSF").append("PEER-PROBE-READY");
  std::set<ndn::Name> expectedReadinessReplyNames;
  for (const auto& interest : readinessInterests) {
    BOOST_CHECK(interest.getCanBePrefix());
    BOOST_CHECK(interest.getMustBeFresh());
    BOOST_CHECK(interest.hasApplicationParameters());
    BOOST_CHECK(interest.isParametersDigestValid());
    BOOST_CHECK_EQUAL(interest.getName().size(), readinessPrefix.size() + 3);
    if (interest.getName().size() == readinessPrefix.size() + 3) {
      const auto timestampText = interest.getName()[readinessPrefix.size()].toUri();
      int64_t timestampMs = 0;
      const auto parsedTimestamp = std::from_chars(
        timestampText.data(), timestampText.data() + timestampText.size(), timestampMs);
      BOOST_REQUIRE(parsedTimestamp.ec == std::errc());
      BOOST_REQUIRE(parsedTimestamp.ptr == timestampText.data() + timestampText.size());
      BOOST_CHECK_LE(std::llabs(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - timestampMs),
        1000);
      auto expectedName = interest.getName();
      expectedName.append("reply");
      expectedReadinessReplyNames.insert(std::move(expectedName));
    }
  }
  BOOST_CHECK_EQUAL(expectedReadinessReplyNames.size(), readinessInterests.size());
  BOOST_CHECK(std::all_of(transmittedInterests.begin(), transmittedInterests.end(),
    [&] (const TransmittedProbe& sent) {
      return sent.sentAt >= targetAuthorizationAt;
    }));
  BOOST_CHECK(std::any_of(readinessReplies.begin(), readinessReplies.end(),
    [&] (const ndn::Data& data) {
      return expectedReadinessReplyNames.count(data.getName()) == 1;
    }));

  std::set<ndn::Name> expectedReplyNames;
  std::set<ndn::Name> receivedReplyNames;
  std::set<ndn::Name> distinctInterestNames;
  for (const auto& sent : transmittedInterests) {
    const auto& interest = sent.interest;
    BOOST_CHECK(interest.getCanBePrefix());
    BOOST_CHECK(interest.getMustBeFresh());
    BOOST_CHECK(interest.hasApplicationParameters());
    BOOST_CHECK(interest.isParametersDigestValid());
    BOOST_CHECK(distinctInterestNames.insert(interest.getName()).second);

    const bool isGoodput =
      interest.getName().toUri().find("PEER-GOODPUT") != std::string::npos;
    ndn::Name probePrefix = target;
    probePrefix.append("NDNSF").append(isGoodput ? "PEER-GOODPUT" : "PEER-PROBE");
    BOOST_REQUIRE_GT(interest.getName().size(), probePrefix.size());
    const auto timestampText = interest.getName()[probePrefix.size()].toUri();
    int64_t timestampMs = 0;
    const auto parsedTimestamp = std::from_chars(
      timestampText.data(), timestampText.data() + timestampText.size(), timestampMs);
    BOOST_REQUIRE(parsedTimestamp.ec == std::errc());
    BOOST_REQUIRE(parsedTimestamp.ptr == timestampText.data() + timestampText.size());
    BOOST_REQUIRE_GE(timestampMs, 0);
    BOOST_CHECK_LE(std::llabs(sent.sentAtMs - timestampMs), 1000);

    const auto expectedInterestSize = probePrefix.size() + (isGoodput ? 4 : 3);
    BOOST_CHECK_EQUAL(interest.getName().size(), expectedInterestSize);
    auto expectedReplyName = interest.getName();
    expectedReplyName.append("reply");
    expectedReplyNames.insert(std::move(expectedReplyName));
  }

  std::set<NetworkTelemetrySampleKind> observedKinds;
  for (const auto& observation : result->observations) {
    observedKinds.insert(observation.kind);
    BOOST_CHECK(observation.peerName == target);
  }
  BOOST_CHECK(observedKinds.count(NetworkTelemetrySampleKind::ActiveProbeRtt));
  BOOST_CHECK(observedKinds.count(NetworkTelemetrySampleKind::ActiveProbeGoodput));
  for (const auto& data : targetReplies) {
    BOOST_CHECK(expectedReplyNames.count(data.getName()));
    BOOST_CHECK(receivedReplyNames.insert(data.getName()).second);
    const auto& signatureInfo = data.getSignatureInfo();
    BOOST_REQUIRE(signatureInfo.hasKeyLocator());
    BOOST_CHECK(signatureInfo.getKeyLocator().getType() == ndn::tlv::Name);
    if (signatureInfo.hasKeyLocator() &&
        signatureInfo.getKeyLocator().getType() == ndn::tlv::Name) {
      BOOST_CHECK(ndn::security::extractIdentityFromCertName(
        signatureInfo.getKeyLocator().getName()) == target);
    }
  }
  BOOST_CHECK(receivedReplyNames == expectedReplyNames);
  const auto& readinessSignature = readinessReplies.front().getSignatureInfo();
  BOOST_REQUIRE(readinessSignature.hasKeyLocator());
  BOOST_CHECK(readinessSignature.getKeyLocator().getType() == ndn::tlv::Name);
  if (readinessSignature.hasKeyLocator() &&
      readinessSignature.getKeyLocator().getType() == ndn::tlv::Name) {
    BOOST_CHECK(ndn::security::extractIdentityFromCertName(
      readinessSignature.getKeyLocator().getName()) == target);
  }
}

BOOST_AUTO_TEST_CASE(AuthorizedProviderPairCollectsBothDirectionsWithinSharedDeadline)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  profile.deferBridgeDelivery = true;
  profile.providerFacesUseIndependentIoContexts = true;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();
  env.pumpUntilReady();

  auto& provider0 = env.provider(0);
  auto& provider1 = env.provider(1);
  const auto name0 = provider0.getName();
  const auto name1 = provider1.getName();
  const std::vector<ndn::Name> candidates{name0, name1};
  const std::vector<NetworkTelemetryDataEdge> rttPairs{
    {name0, name1}, {name1, name0}
  };
  const std::vector<NetworkTelemetryDataEdge> goodputEdges{
    {name0, name1}, {name1, name0}
  };
  const std::string requestScope = "sha256:" + std::string(64, '8');
  const auto collectionStart = std::chrono::steady_clock::now();
  const auto sharedDeadline = collectionStart + std::chrono::milliseconds(500);
  const auto scopeExpiry = collectionStart + std::chrono::seconds(2);
  BOOST_REQUIRE(provider0.authorizeCandidatePeerObservationScope(
    requestScope, candidates, rttPairs, goodputEdges, scopeExpiry));

  std::optional<ServiceProvider::PeerProbeCollection> provider0Result;
  std::optional<ServiceProvider::PeerProbeCollection> provider1Result;
  std::mutex resultsMutex;
  std::condition_variable resultsReady;
  provider0.collectCandidatePeerObservations(
    candidates, rttPairs, goodputEdges, requestScope, sharedDeadline, nullptr,
    [&] (ServiceProvider::PeerProbeCollection collection) {
      std::lock_guard<std::mutex> lock(resultsMutex);
      provider0Result = std::move(collection);
      resultsReady.notify_all();
    });

  auto provider1Registration = std::make_shared<boost::asio::steady_timer>(
    env.providerFace(1).getIoContext());
  provider1Registration->expires_after(std::chrono::milliseconds(120));
  provider1Registration->async_wait(
    [&, provider1Registration](const boost::system::error_code& error) {
      if (error) return;
      if (!provider1.authorizeCandidatePeerObservationScope(
            requestScope, candidates, rttPairs, goodputEdges, scopeExpiry)) {
        return;
      }
      provider1.collectCandidatePeerObservations(
        candidates, rttPairs, goodputEdges, requestScope, sharedDeadline, nullptr,
        [&] (ServiceProvider::PeerProbeCollection collection) {
          std::lock_guard<std::mutex> lock(resultsMutex);
          provider1Result = std::move(collection);
          resultsReady.notify_all();
        });
    });

  auto& provider0Io = env.providerFace(0).getIoContext();
  auto& provider1Io = env.providerFace(1).getIoContext();
  BOOST_REQUIRE_NE(&provider0Io, &provider1Io);
  auto provider0IoWork = boost::asio::make_work_guard(provider0Io);
  auto provider1IoWork = boost::asio::make_work_guard(provider1Io);
  std::thread provider0IoWorker([&] { provider0Io.run(); });
  std::thread provider1IoWorker([&] { provider1Io.run(); });
  bool resultsCompleted = false;
  {
    std::unique_lock<std::mutex> lock(resultsMutex);
    resultsCompleted = resultsReady.wait_for(
      lock, std::chrono::seconds(2),
      [&] { return provider0Result && provider1Result; });
  }
  provider0Io.stop();
  provider1Io.stop();
  provider0IoWork.reset();
  provider1IoWork.reset();
  provider0IoWorker.join();
  provider1IoWorker.join();
  BOOST_REQUIRE_MESSAGE(resultsCompleted,
                        "provider peer observations did not complete within 2 seconds");
  BOOST_REQUIRE(provider0Result);
  BOOST_REQUIRE(provider1Result);

  const auto checkCompletePair = [] (
      const ServiceProvider::PeerProbeCollection& collection,
      const ndn::Name& observer, const ndn::Name& peer) {
    BOOST_CHECK_EQUAL(collection.requestedTargetCount, 1);
    BOOST_CHECK_EQUAL(collection.omittedTargetCount, 0);
    BOOST_CHECK_EQUAL(collection.requestedGoodputEdgeCount, 1);
    BOOST_CHECK_EQUAL(collection.omittedGoodputEdgeCount, 0);
    BOOST_CHECK_EQUAL(collection.observations.size(), 2);
    BOOST_CHECK_EQUAL(std::count_if(
      collection.observations.begin(), collection.observations.end(),
      [&] (const NetworkTelemetrySnapshot& observation) {
        return observation.kind == NetworkTelemetrySampleKind::ActiveProbeRtt &&
               observation.providerName == observer && observation.peerName == peer;
      }), 1);
    const auto rtt = std::find_if(
      collection.observations.begin(), collection.observations.end(),
      [] (const NetworkTelemetrySnapshot& observation) {
        return observation.kind == NetworkTelemetrySampleKind::ActiveProbeRtt;
      });
    BOOST_REQUIRE(rtt != collection.observations.end());
    BOOST_CHECK(rtt->targetSignatureValidated);
    BOOST_CHECK(rtt->probeSignatureValid);
    BOOST_CHECK(!rtt->dataName.empty());
    BOOST_CHECK(rtt->dataName.toUri().find("/NDNSF/PEER-PROBE/") != std::string::npos);
    const auto goodput = std::find_if(
      collection.observations.begin(), collection.observations.end(),
      [] (const NetworkTelemetrySnapshot& observation) {
        return observation.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput;
      });
    BOOST_REQUIRE(goodput != collection.observations.end());
    const bool sourceTimingValid = goodput->probeSourceTimingValidated &&
      std::all_of(goodput->probeSourceSendGapsMs.begin(),
                  goodput->probeSourceSendGapsMs.end(),
                  [] (double gap) { return std::isfinite(gap) && gap >= 0 && gap <= 500; });
    const auto gaps = [] (const auto& values) {
      std::string result;
      for (const auto value : values) {
        if (!result.empty()) result += ",";
        result += std::to_string(value);
      }
      return result;
    };
    BOOST_CHECK_MESSAGE(
        goodput->providerName == observer && goodput->peerName == peer &&
        goodput->probeComplete && sourceTimingValid &&
        goodput->targetSignatureValidated && !goodput->dataName.empty() &&
        goodput->dataName.toUri().find("/NDNSF/PEER-GOODPUT/") != std::string::npos &&
        goodput->probeSegmentsSent == NETWORK_PROBE_WARMED_SEGMENTS && goodput->receivedSegments == NETWORK_PROBE_WARMED_SEGMENTS,
      "observer=" << observer << " peer=" << peer
      << " provider=" << goodput->providerName
      << " samplePeer=" << goodput->peerName
      << " complete=" << goodput->probeComplete
      << " signatureValid=" << goodput->probeSignatureValid
      << " sourceTimingValid=" << sourceTimingValid
      << " segmentsSent=" << goodput->probeSegmentsSent
      << " segmentsReceived=" << goodput->receivedSegments
      << " interestGapsMs=" << gaps(goodput->probeInterestGapsMs)
      << " dataGapsMs=" << gaps(goodput->probeDataArrivalGapsMs));
  };
  checkCompletePair(*provider0Result, name0, name1);
  checkCompletePair(*provider1Result, name1, name0);
}

BOOST_AUTO_TEST_CASE(CancelledProbeCollectionStopsAfterFirstGoodputInterest)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();
  env.pumpUntilReady();

  auto& targetProvider = env.provider(0);
  auto& observerProvider = env.provider(1);
  const auto target = targetProvider.getName();
  const auto observer = observerProvider.getName();
  const std::vector<ndn::Name> candidates{target, observer};
  const std::vector<NetworkTelemetryDataEdge> rttPairs{{observer, target}};
  const std::vector<NetworkTelemetryDataEdge> edges{{target, observer}};
  const std::string requestScope = "sha256:" + std::string(64, 'c');
  const auto expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (auto* provider : {&targetProvider, &observerProvider}) {
    BOOST_REQUIRE(provider->authorizeCandidatePeerObservationScope(
      requestScope, candidates, rttPairs, edges, expiresAt));
  }

  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  std::vector<ndn::Interest> transmittedInterests;
  auto interestConnection = env.providerFace(1).onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      const auto uri = interest.getName().toUri();
      if (isActiveProbeMeasurementInterest(interest)) {
        transmittedInterests.push_back(interest);
        if (uri.find("/PEER-GOODPUT/") != std::string::npos) {
          cancelled->store(true, std::memory_order_relaxed);
        }
      }
    });

  std::optional<ServiceProvider::PeerProbeCollection> result;
  observerProvider.collectCandidatePeerObservations(
    candidates, rttPairs, edges, requestScope,
    std::chrono::steady_clock::now() + std::chrono::seconds(2), cancelled,
    [&] (ServiceProvider::PeerProbeCollection collection) {
      result = std::move(collection);
    });
  env.pumpUntil([&] { return result.has_value(); });

  BOOST_REQUIRE(result);
  BOOST_CHECK(cancelled->load(std::memory_order_relaxed));
  BOOST_CHECK_EQUAL(std::count_if(transmittedInterests.begin(),
    transmittedInterests.end(), [] (const ndn::Interest& interest) {
      return interest.getName().toUri().find("/PEER-GOODPUT/") != std::string::npos;
    }), 1);
  BOOST_CHECK_EQUAL(std::count_if(transmittedInterests.begin(),
    transmittedInterests.end(), [] (const ndn::Interest& interest) {
      return interest.getName().toUri().find("/PEER-PROBE/") != std::string::npos;
    }), 1);
  BOOST_CHECK(result->observations.empty());
  BOOST_CHECK(!observerProvider.authorizeCandidatePeerObservationScope(
    requestScope, candidates, rttPairs, edges,
    std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

BOOST_AUTO_TEST_CASE(Spec197SourcePendingTrainQuotaExpiryAndCancellation)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready, env.failureReason());
  env.pumpUntilReady();
  auto& source = env.provider(0);
  const auto observer = env.provider(1).getName();
  const std::vector<ndn::Name> candidates{source.getName(), observer};
  const std::vector<NetworkTelemetryDataEdge> edges{{source.getName(), observer}};
  size_t responses = 0;
  auto connection = env.providerFace(0).onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName().toUri().find("/PEER-GOODPUT/") != std::string::npos)
      ++responses;
  });
  auto registration = std::make_shared<std::atomic<bool>>(false);
  // Exercise the post-validation production queue boundary. Every scope is
  // authorized; incomplete trains must neither publish nor displace a peer.
  for (size_t i = 0; i < 33; ++i) {
    const auto scope = "source-quota-" + std::to_string(i);
    BOOST_REQUIRE(ServiceProviderTestAccess::authorizeSourceWithRegistration(
      source, scope, candidates, edges, registration));
    ServiceProviderTestAccess::queueSourceSegment(source, observer, scope, "1", 0);
  }
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 32U);
  ServiceProviderTestAccess::queueSourceSegment(source, observer, "source-quota-0", "1", 0);
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 32U);
  registration->store(true);
  for (size_t segment = 1; segment < NETWORK_PROBE_WARMED_SEGMENTS; ++segment)
    ServiceProviderTestAccess::queueSourceSegment(
      source, observer, "source-quota-0", "1", segment);
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 32U);
  // A closed scope is rejected before touching the queue. Its original weak
  // expiry timer still drains the retained partial train within 500 ms.
  BOOST_CHECK_EQUAL(responses, 0U);
  env.pumpUntil([&] { return ServiceProviderTestAccess::pendingSourceTrains(source) == 0; });
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 0U);
  BOOST_CHECK_EQUAL(responses, 0U);
  auto reopened = std::make_shared<std::atomic<bool>>(false);
  BOOST_CHECK(!ServiceProviderTestAccess::authorizeSourceWithRegistration(
    source, "source-quota-0", candidates, edges, reopened));
  BOOST_REQUIRE(ServiceProviderTestAccess::authorizeSourceWithRegistration(
    source, "source-after-expiry", candidates, edges, reopened));
  ServiceProviderTestAccess::queueSourceSegment(
    source, observer, "source-after-expiry", "2", 0);
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 1U);
  // Environment destruction owns the partial queue and its outstanding weak
  // timer; the same selector is included in the sanitizer lifecycle run.
}

BOOST_AUTO_TEST_CASE(Spec197SourcePartialTrainExpiresWithoutDuplicateRenewal)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready, env.failureReason());
  env.pumpUntilReady();
  auto& source = env.provider(0);
  const auto observer = env.provider(1).getName();
  const std::vector<ndn::Name> candidates{source.getName(), observer};
  const std::vector<NetworkTelemetryDataEdge> edges{{source.getName(), observer}};
  auto registration = std::make_shared<std::atomic<bool>>(false);
  BOOST_REQUIRE(ServiceProviderTestAccess::authorizeSourceWithRegistration(
    source, "source-clock", candidates, edges, registration));
  size_t responses = 0;
  auto connection = env.providerFace(0).onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName().toUri().find("/PEER-GOODPUT/") != std::string::npos)
      ++responses;
  });
  const auto beforeAdmission = std::chrono::steady_clock::now();
  ServiceProviderTestAccess::queueSourceSegment(source, observer, "source-clock", "1", 0);
  const auto afterAdmission = std::chrono::steady_clock::now();
  BOOST_REQUIRE_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 1U);
  env.pumpUntil([&] {
    return std::chrono::steady_clock::now() >= afterAdmission + std::chrono::milliseconds(200);
  });
  BOOST_REQUIRE_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 1U);
  ServiceProviderTestAccess::queueSourceSegment(source, observer, "source-clock", "1", 0);
  BOOST_REQUIRE_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 1U);
  env.pumpUntil([&] {
    return ServiceProviderTestAccess::pendingSourceTrains(source) == 0 ||
      std::chrono::steady_clock::now() >= afterAdmission + std::chrono::milliseconds(650);
  });
  const auto drainedAt = std::chrono::steady_clock::now();
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 0U);
  BOOST_CHECK(drainedAt >= beforeAdmission + std::chrono::milliseconds(500));
  // Allow 100 ms for fixture pumping/scheduling, but reject a renewed 700 ms
  // deadline or expiry that depends on the five-second authorization scope.
  BOOST_CHECK(drainedAt <= afterAdmission + std::chrono::milliseconds(600));
  BOOST_CHECK_EQUAL(responses, 0U);
}

BOOST_AUTO_TEST_CASE(Spec197RegistrationCloseCancelsPendingSourceTrain)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready, env.failureReason());
  env.pumpUntilReady();
  auto& source = env.provider(0);
  const auto observer = env.provider(1).getName();
  const std::vector<ndn::Name> candidates{source.getName(), observer};
  const std::vector<NetworkTelemetryDataEdge> edges{{source.getName(), observer}};
  const auto expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const std::string scope = "sha256:" + std::string(64, 'e');
  size_t responses = 0;
  auto connection = env.providerFace(0).onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName().toUri().find("/PEER-GOODPUT/") != std::string::npos)
      ++responses;
  });
  auto registration = source.addScopedTargetedService(
    ndn::Name(NETWORK_PROBE_SERVICE),
    ndnsf::di::makeNativePlacementMeasurementTargetedHandler(source));
  // Authorize through the production registration owner, rather than
  // injecting a cancellation flag into the telemetry store.
  BOOST_REQUIRE(source.authorizeCandidatePeerObservationScope(
    scope, candidates, {}, edges, expiresAt));
  ServiceProviderTestAccess::queueSourceSegment(source, observer, scope, "1", 0);
  BOOST_REQUIRE_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 1U);
  registration.close();
  registration.close();
  for (size_t segment = 1; segment < NETWORK_PROBE_WARMED_SEGMENTS; ++segment)
    ServiceProviderTestAccess::queueSourceSegment(source, observer, scope, "1", segment);
  env.pumpUntil([&] { return ServiceProviderTestAccess::pendingSourceTrains(source) == 0; });
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 0U);
  BOOST_CHECK_EQUAL(responses, 0U);

  auto replacement = source.addScopedTargetedService(
    ndn::Name(NETWORK_PROBE_SERVICE),
    ndnsf::di::makeNativePlacementMeasurementTargetedHandler(source));
  BOOST_REQUIRE(std::chrono::steady_clock::now() < expiresAt);
  BOOST_CHECK(!source.authorizeCandidatePeerObservationScope(
    scope, candidates, {}, edges, expiresAt));
  const std::string freshScope = "sha256:" + std::string(64, 'f');
  BOOST_REQUIRE(source.authorizeCandidatePeerObservationScope(
    freshScope, candidates, {}, edges, expiresAt));
  for (size_t segment = 0; segment < NETWORK_PROBE_WARMED_SEGMENTS; ++segment)
    ServiceProviderTestAccess::queueSourceSegment(source, observer, freshScope, "2", segment);
  env.pumpUntil([&] { return responses == NETWORK_PROBE_WARMED_SEGMENTS; });
  BOOST_CHECK_EQUAL(responses, NETWORK_PROBE_WARMED_SEGMENTS);
  BOOST_CHECK_EQUAL(ServiceProviderTestAccess::pendingSourceTrains(source), 0U);
}

BOOST_AUTO_TEST_CASE(Spec197CollectorRejectsMalformedSignedTrainReports)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  profile.deferBridgeDelivery = true;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready, env.failureReason());
  env.pumpUntilReady();
  env.enableProviderProductionIngressForTest();
  env.pumpUntilReady();
  env.disconnectProviderPeerTransportForTest();
  auto& source = env.provider(0);
  auto& receiver = env.provider(1);
  ServiceProviderTestAccess::anchorProbeValidation(receiver, source);
  auto& sourceFace = env.providerFace(0);
  auto& receiverFace = env.providerFace(1);
  const std::vector<ndn::Name> candidates{source.getName(), receiver.getName()};
  const std::vector<NetworkTelemetryDataEdge> edges{{source.getName(), receiver.getName()}};
  auto interests = receiverFace.onSendInterest.connect([&] (const ndn::Interest& packet) {
    sourceFace.getIoContext().post([&, packet] { sourceFace.receive(packet); });
  });
  auto reverseInterests = sourceFace.onSendInterest.connect([&] (const ndn::Interest& packet) {
    receiverFace.getIoContext().post([&, packet] { receiverFace.receive(packet); });
  });
  auto reverseData = receiverFace.onSendData.connect([&] (const ndn::Data& packet) {
    sourceFace.getIoContext().post([&, packet] { sourceFace.receive(packet); });
  });
  std::string fault;
  size_t reports = 0;
  size_t captures = 0;
  size_t validationEntries = 0;
  size_t validationCompletions = 0;
  size_t terminals = 0;
  std::shared_ptr<std::atomic<bool>> cancelled;
  std::chrono::steady_clock::time_point collectionDeadline;
  std::chrono::steady_clock::time_point firstCaptureAt;
  std::chrono::steady_clock::time_point delayedValidationFinished;
  ServiceProviderTestAccess::observeGoodputStages(receiver,
    [&] (const char* phase, size_t captured, size_t retained) {
      const std::string stage(phase);
      if (stage == "capture") {
        ++captures;
        BOOST_CHECK_EQUAL(captured, captures);
        BOOST_CHECK_EQUAL(retained, captured);
        if (captures == 1)
          firstCaptureAt = std::chrono::steady_clock::now();
        if (fault == "capture-cancel" && captured == NETWORK_PROBE_WARMED_SEGMENTS)
          cancelled->store(true);
      }
      else if (stage == "validation-entry") {
        ++validationEntries;
        BOOST_CHECK_EQUAL(captures, NETWORK_PROBE_WARMED_SEGMENTS);
        BOOST_CHECK_EQUAL(captured, NETWORK_PROBE_WARMED_SEGMENTS);
        BOOST_CHECK_EQUAL(retained, NETWORK_PROBE_WARMED_SEGMENTS);
      }
      else if (stage == "validation-complete") {
        ++validationCompletions;
        BOOST_CHECK_EQUAL(captured, NETWORK_PROBE_WARMED_SEGMENTS);
        if (validationCompletions == 1) {
          if (fault == "validation-cancel")
            cancelled->store(true);
          else if (fault == "validation-deadline")
            std::this_thread::sleep_until(collectionDeadline + std::chrono::milliseconds(2));
          else if (fault == "validation-delay") {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            delayedValidationFinished = std::chrono::steady_clock::now();
          }
        }
      }
      else if (stage == "terminal") {
        ++terminals;
        BOOST_CHECK_EQUAL(retained, 0U);
        if (fault == "terminal-throw")
          throw std::runtime_error("fixture terminal observer failure");
      }
    });
  std::optional<ndn::Data> heldFirst;
  auto dataLink = sourceFace.onSendData.connect([&] (const ndn::Data& original) {
    auto packet = original;
    if (original.getName().toUri().find("/PEER-GOODPUT/") != std::string::npos) {
      auto content = original.getContent();
      content.parse();
      BOOST_REQUIRE_EQUAL(content.elements().size(), 1U);
      PeerNetworkGoodputProbeResponse decoded;
      BOOST_REQUIRE(decoded.WireDecode(content.elements().front()));
      if (fault == "reorder" && decoded.getField("segment") == "0") {
        heldFirst = packet;
        return;
      }
      if (fault == "reorder" && decoded.getField("segment") == "1") {
        BOOST_REQUIRE(heldFirst);
        receiverFace.getIoContext().post([&, packet] { receiverFace.receive(packet); });
        const auto first = *heldFirst;
        heldFirst.reset();
        receiverFace.getIoContext().post([&, first] { receiverFace.receive(first); });
        return;
      }
      if (decoded.getField("segment") == std::to_string(NETWORK_PROBE_WARMED_SEGMENTS - 1)) {
        ++reports;
        if (fault == "missing") {
          PeerNetworkGoodputProbeResponse replacement;
          for (const auto& [key, value] : decoded.getFields())
            if (key != "sourceSendGap13Ns") replacement.setField(key, value);
          decoded = std::move(replacement);
        }
        else if (fault == "profile")
          decoded.setField("probeProfileDigest", "sha256:" + std::string(64, '0'));
        else if (fault == "boot")
          decoded.setField("targetBootEpoch", "different-source-boot");
        else if (fault == "content") {
          // Keep every value within the production codec's 4096B field cap.
          // The extra field makes total Content exceed the collector budget.
          decoded.setField("budgetPadding", std::string(1536, 'x'));
        }
        else if (fault == "wire")
          packet.setFinalBlock(ndn::name::Component(std::string(2500, 'x')));
        else if (fault == "partial")
          return;
        else if (fault != "valid" && fault != "reorder" && fault != "signature" &&
                 fault != "capture-cancel" && fault != "validation-cancel" &&
                 fault != "validation-deadline" && fault != "validation-delay" &&
                 fault != "terminal-throw")
          decoded.setField("sourceSendGap0Ns", fault);
        packet.setContent(decoded.WireEncode());
        ServiceProviderTestAccess::signProbeData(source, packet);
        if (fault == "signature")
          packet.setSignatureValue(ndn::makeEmptyBlock(ndn::tlv::SignatureValue));
        if (fault == "content") {
          BOOST_CHECK_GT(packet.getContent().value_size(), NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES);
          BOOST_REQUIRE_LT(packet.wireEncode().size(), 8800U);
        }
        if (fault == "wire") {
          BOOST_CHECK_LE(packet.getContent().value_size(), NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES);
          BOOST_CHECK_GT(packet.wireEncode().size(), 6144U);
          BOOST_CHECK_LT(packet.wireEncode().size(), 8800U);
        }
      }
    }
    receiverFace.getIoContext().post([&, packet] { receiverFace.receive(packet); });
  });
  const std::vector<std::string> faults{
    "missing", "", "-1", "0.1", "nan", "500000001", "9999999999999999999999",
    "profile", "boot", "content", "wire", "signature", "partial", "reorder", "valid",
    "capture-cancel", "validation-cancel", "validation-deadline", "validation-delay",
    "terminal-throw"};
  for (size_t i = 0; i < faults.size(); ++i) {
    // Keep this parser oracle below the unchanged 128 Interests/sec admission
    // limit; otherwise later faults would only test the rate limiter.
    const auto admissionAt = std::chrono::steady_clock::now();
    fault = faults[i];
    captures = validationEntries = validationCompletions = terminals = 0;
    firstCaptureAt = delayedValidationFinished = {};
    cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto scope = "collector-report-fault-" + std::to_string(i);
    for (auto* owner : {&source, &receiver})
      BOOST_REQUIRE(owner->authorizeCandidatePeerObservationScope(scope, candidates, {}, edges,
        std::chrono::steady_clock::now() + std::chrono::seconds(5)));
    const auto before = reports;
    size_t callbacks = 0;
    std::optional<ServiceProvider::PeerProbeCollection> result;
    collectionDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    receiver.collectCandidatePeerObservations(candidates, {}, edges, scope,
      collectionDeadline, cancelled,
      [&] (ServiceProvider::PeerProbeCollection collection) {
        ++callbacks;
        result = std::move(collection);
      });
    env.pumpUntil([&] { return result.has_value(); });
    BOOST_REQUIRE_MESSAGE(result, "fault=" << fault);
    BOOST_CHECK_MESSAGE(reports > before, "negative oracle must reach signed report: " << fault);
    BOOST_CHECK_EQUAL(callbacks, 1U);
    BOOST_CHECK_EQUAL(terminals, 1U);
    if (fault == "valid" || fault == "validation-delay" || fault == "terminal-throw") {
      BOOST_CHECK_EQUAL(validationEntries, NETWORK_PROBE_WARMED_SEGMENTS);
      BOOST_CHECK_EQUAL(validationCompletions, NETWORK_PROBE_WARMED_SEGMENTS);
      BOOST_REQUIRE_EQUAL(result->observations.size(), 1U);
      BOOST_CHECK(result->observations.front().probeSourceTimingValidated);
      BOOST_CHECK_EQUAL(result->observations.front().probeProfileDigest,
        NETWORK_PROBE_TRAIN_PROFILE_DIGEST);
      if (fault == "validation-delay") {
        const auto& gaps = result->observations.front().probeDataArrivalGapsMs;
        double measuredSpanMs = 0;
        for (const auto gap : gaps) measuredSpanMs += gap;
        const double captureAndValidationSpanMs =
          std::chrono::duration<double, std::milli>(
            delayedValidationFinished - firstCaptureAt).count();
        BOOST_CHECK_LT(measuredSpanMs, captureAndValidationSpanMs);
      }
    }
    else if (fault == "reorder") {
      for (const auto& observation : result->observations)
        BOOST_CHECK(ndnsf::di::nativeProbeTrainRateMetadataV4(observation)
          .at("probe_rate_status") == "UNKNOWN");
    }
    else
      BOOST_CHECK_MESSAGE(result->observations.empty(), "fault=" << fault);
    if (fault == "capture-cancel")
      BOOST_CHECK_EQUAL(validationEntries, 0U);
    if (fault == "validation-cancel" || fault == "validation-deadline")
      BOOST_CHECK_GE(validationCompletions, 1U);
    env.pumpUntil([&] {
      return std::chrono::steady_clock::now() >= admissionAt + std::chrono::milliseconds(150);
    });
    BOOST_CHECK_EQUAL(callbacks, 1U);
    BOOST_CHECK_EQUAL(terminals, 1U);
  }
  ServiceProviderTestAccess::observeGoodputStages(receiver, {});
}

BOOST_AUTO_TEST_CASE(UnregisteredScopeCannotCollectReverseEdge)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment env(profile);
  env.bootstrap();
  BOOST_REQUIRE_MESSAGE(env.status() == EnvironmentStatus::Ready,
                       env.failureReason());
  auto& sourceProvider = env.provider(0);
  const auto source = sourceProvider.getName();
  const auto observer = env.provider(1).getName();
  std::optional<ServiceProvider::PeerProbeCollection> unauthorized;
  sourceProvider.collectCandidatePeerObservations(
    {source, observer}, {}, {{observer, source}}, "unregistered-reverse-scope",
    std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr,
    [&] (ServiceProvider::PeerProbeCollection collection) {
      unauthorized = std::move(collection);
    });
  BOOST_REQUIRE(unauthorized);
  BOOST_CHECK(unauthorized->observations.empty());
  BOOST_CHECK_EQUAL(unauthorized->requestedGoodputEdgeCount, 1);
  BOOST_CHECK_EQUAL(unauthorized->omittedGoodputEdgeCount, 1);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace test
} // namespace ndn_service_framework
