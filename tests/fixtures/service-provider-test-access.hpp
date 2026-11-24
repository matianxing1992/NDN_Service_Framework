#ifndef NDNSF_TESTS_FIXTURES_SERVICE_PROVIDER_TEST_ACCESS_HPP
#define NDNSF_TESTS_FIXTURES_SERVICE_PROVIDER_TEST_ACCESS_HPP

#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "ndn-service-framework/PeerProbeNaming.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "tests/boost-test.hpp"

#include <ndn-cxx/util/io.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ndn_service_framework {

/** One definition across all native fixture translation units. */
struct NetworkTelemetryStoreTestAccess
{
  static void ageCaptures(NetworkTelemetryStore& store, uint64_t ageMs)
  {
    std::lock_guard<std::mutex> lock(store.m_mutex);
    const auto now = NetworkTelemetryStore::nowMs();
    for (auto& entry : store.m_snapshots)
      entry.second.lastUpdatedMs = now - ageMs;
  }

  static void tieCaptureTimes(NetworkTelemetryStore& store)
  {
    std::lock_guard<std::mutex> lock(store.m_mutex);
    uint64_t latest = 0;
    for (const auto& entry : store.m_snapshots)
      latest = std::max(latest, entry.second.lastUpdatedMs);
    for (auto& entry : store.m_snapshots)
      entry.second.lastUpdatedMs = latest;
  }

  static void setTtl(NetworkTelemetryStore& store, std::chrono::milliseconds ttl)
  {
    std::lock_guard<std::mutex> lock(store.m_mutex);
    store.m_ttl = ttl;
  }
};

/** Test-only private access; no independent protocol or lifecycle implementation. */
struct ServiceProviderTestAccess
{
  /** Capture the registered callbacks on the Face thread before injecting a test fault. */
  static std::pair<ServiceProvider::AckStrategyHandler, ServiceProvider::RequestHandler>
  copyLegacyServiceHandlers(ServiceProvider& provider, const ndn::Name& name)
  {
    const auto& service = provider.m_services.at(name);
    BOOST_REQUIRE(!service.registrationState);
    BOOST_REQUIRE(service.ackHandler && service.requestHandler);
    return {service.ackHandler, service.requestHandler};
  }

  static void recordSelectionReceived(ServiceProvider& provider, const std::string& digest,
                                      const ndn::Name& service, const ndn::Name& request)
  {
    provider.updateSelectionExecutionStatus(digest, SelectionExecutionState::Received,
      provider.getName(), service, request);
  }

  static void useSigningKeyChainForPeerProbe(ServiceProvider& provider,
                                             ndn::KeyChain& keyChain)
  {
    provider.m_testSigningKeyChain = &keyChain;
  }

  /** CK names of the provider's most recent protected publication. */
  static std::vector<ndn::Name> lastPublishedCkNames(ServiceProvider& provider)
  {
    return provider.m_lastPublishedCkNames;
  }

  static void configureFaceBackedProducerValidator(ServiceProvider& provider,
    const std::string& trustSchemaPath, ndn::Face& face)
  {
    provider.validator = std::make_shared<MessageValidator>(trustSchemaPath, std::nullopt, &face);
    provider.m_isLocalMock = false;
  }

  static std::function<std::size_t()> observeProducerValidationWaiters(ServiceProvider& provider)
  {
    const auto control = provider.validator->m_producerPacketValidationControl;
    return [control] {
      std::lock_guard<std::mutex> lock(control->mutex);
      return control->outstanding;
    };
  }

  static bool knowsCertificate(ServiceProvider& provider, const ndn::Name& name)
  {
    return provider.validator->getConfiguredValidatorForSegmentFetcher().isCertKnown(name);
  }

  static std::function<size_t()> observeRecipientDecrypts(ServiceProvider& provider)
  {
    const auto pending = provider.m_pendingContextCrypto;
    return [pending] { return pending->load(); };
  }

  static void checkCertificateTrust(ServiceProvider& provider, const ndn::Data& data)
  {
    provider.validator->validateWithConfiguredTrustSchema(data,
      [](const ndn::Data&) { BOOST_TEST_MESSAGE("certificate schema accepted"); },
      [](const ndn::Data&, const ndn::security::ValidationError& error) {
        BOOST_ERROR("certificate schema rejected: " << error);
      });
  }

  static ServiceProvider*
  acquire(ServiceProvider& provider)
  {
    return provider.m_registrationControl->acquireOwner();
  }

  static void
  release(ServiceProvider& provider)
  {
    provider.m_registrationControl->releaseOwner();
  }

  static bool
  pending(const ServiceProvider& provider)
  {
    return provider.m_registrationControl->activeOwnerCalls != 0;
  }

  static void ageProbeCaptures(ServiceProvider& provider, uint64_t ageMs)
  {
    NetworkTelemetryStoreTestAccess::ageCaptures(provider.m_networkTelemetry, ageMs);
  }

  static bool activeProbeScopeOpen(ServiceProvider& provider, const std::string& scope)
  {
    return provider.m_networkTelemetry.isActiveProbeCollectionOpen(scope);
  }

  static void setTelemetryTtl(ServiceProvider& provider, std::chrono::milliseconds ttl)
  {
    NetworkTelemetryStoreTestAccess::setTtl(provider.m_networkTelemetry, ttl);
  }

  static void seedReusableProbeSamples(ServiceProvider& provider, const ndn::Name& peer,
                                      const std::string& peerBoot, bool goodputSignature = true)
  {
    // Seed the already-validated Core boundary, not an independent rate oracle.
    // Actual signature ingestion is exercised by the production-wire fixtures.
    NetworkProbePacketTimes rtts{}; rtts.fill(20.0);
    NetworkProbeArrivalGaps gaps{}; gaps.fill(4.0);
    NetworkProbeSourceGaps sends{}; sends.fill(0.5);
    NetworkProbeWireSizes wire{}; wire.fill(10000);
    const auto payload = NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES;
    const auto suffix = std::string(230, 'a');
    provider.m_networkTelemetry.updateActiveProbe(provider.getName(), peer,
      "cache-control", peerBoot, ndn::Name(peer).append("cached-rtt").append(suffix), 20.0);
    provider.m_networkTelemetry.updateActiveProbeGoodput(provider.getName(), peer,
      "cache-control", peerBoot, ndn::Name(peer).append("cached-goodput").append(suffix),
      payload, payload + 10000, NETWORK_PROBE_WARMED_SEGMENTS, 1.0, 30.0,
      NETWORK_PROBE_TRAIN_PROFILE_DIGEST, goodputSignature, rtts, gaps, gaps,
      wire, sends, true);
  }

  static void observeGoodputStages(ServiceProvider& provider,
      std::function<void(const char*, size_t, size_t)> observer)
  {
    provider.m_peerGoodputStageObserver = std::move(observer);
  }
  static std::string fixedProbeAnchor(ServiceProvider& provider)
  {
    auto& keys = provider.m_testSigningKeyChain ? *provider.m_testSigningKeyChain : provider.m_keyChain;
    const auto certificate = keys.getPib().getIdentity(provider.getName())
      .getDefaultKey().getDefaultCertificate();
    std::ostringstream encoded;
    ndn::io::save(certificate, encoded);
    auto base64 = encoded.str();
    base64.erase(std::remove(base64.begin(), base64.end(), '\n'), base64.end());
    return "trust-anchor { type base64 base64-string \"" + base64 + "\" }";
  }
  static void anchorProbeValidation(ServiceProvider& receiver, ServiceProvider& source)
  {
    // Restrict this signature oracle to the fixture's exact source certificate.
    receiver.nac_validator.load(
      "rule { id probe-signatures for data checker { type customized "
      "sig-type rsa-sha256 key-locator { type name name " +
      source.getName().toUri(ndn::name::UriFormat::CANONICAL) +
      " relation equal } } }\n" + fixedProbeAnchor(source), "spec197-probe-signature-oracle");
  }
  static void anchorProbeInterestValidation(ServiceProvider& target, ServiceProvider& observer)
  {
    target.nac_validator.load(
      "rule { id probe-request-signatures for interest checker { type customized "
      "sig-type rsa-sha256 key-locator { type name name " +
      observer.getName().toUri(ndn::name::UriFormat::CANONICAL) +
      " relation equal } } }\n" + fixedProbeAnchor(observer), "spec201-probe-request-oracle");
  }
  static void receiveSignedProbe(ServiceProvider& target, ServiceProvider& observer,
                                 ndn::Interest& interest)
  {
    observer.signPeerProbeInterest(interest);
    BOOST_REQUIRE(target.handlePeerNetworkProbeInterest(interest));
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
    const PeerProbeCall call{"PROBE-GOODPUT", observer, provider.getName(), scope,
      nonce, segment, NETWORK_PROBE_WARMED_SEGMENTS, NETWORK_PROBE_WARMED_PAYLOAD_BYTES,
      false, true};
    PeerNetworkGoodputProbe probe;
    probe.setField("observerProvider", observer.toUri());
    probe.setField("targetProvider", provider.getName().toUri());
    probe.setField("requestScopeDigest", scope);
    probe.setField("runNonce", nonce);
    probe.setField("segment", std::to_string(segment));
    probe.setField("segmentCount", std::to_string(call.segmentCount));
    probe.setField("contentBytes", std::to_string(call.contentBytes));
    ndn::Interest interest(makePeerProbeRpcName(call));
    interest.setApplicationParameters(probe.WireEncode());
    provider.queuePeerGoodputTrain(interest, observer, scope, nonce, segment);
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


  static void seedCachedRtt(ServiceProvider& observer, ServiceProvider& peer)
  {
    // Controlled validated-store boundary; targeted response authentication and
    // requester cache admission still use the production implementations.
    observer.m_networkTelemetry.updateActiveProbe(observer.getName(), peer.getName(),
      "spec199-cache-fixture", peer.getProviderBootEpoch(),
      ndn::Name(peer.getName()).append("spec199-cached-rtt"), 20.0);
  }

  static ndn::Name extractLargeDataProducerPrefix(const ndn::Name& dataName)
  {
    return ServiceProvider::extractLargeDataProducerPrefix(dataName);
  }

  static void
  scheduleCleanup(ServiceProvider& provider, const ndn::Name& key,
                  ndn::time::milliseconds ttl, bool authoritative)
  {
    provider.schedulePendingRequestCleanup(key, ttl, authoritative);
  }

  static std::int64_t
  cleanupDeadlineNs(ServiceProvider& provider, const ndn::Name& key)
  {
    std::lock_guard<std::mutex> lock(provider.m_pendingCleanupDeadlineMutex);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      provider.m_pendingCleanupDeadlines.at(key).time_since_epoch()).count();
  }

  static std::int64_t
  workDeadlineNs(ServiceProvider& provider, const ndn::Name& requester,
                 const ndn::Name& request, const ndn::Name& service)
  {
    const auto fence = provider.makeCollaborationWorkFence(
      requester, request, service, std::nullopt);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      fence.deadline.time_since_epoch()).count();
  }

};

} // namespace ndn_service_framework

#endif
