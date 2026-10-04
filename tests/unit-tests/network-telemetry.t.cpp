/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "ndn-service-framework/NetworkTelemetry.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "tests/boost-test.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(NetworkTelemetry)

BOOST_AUTO_TEST_CASE(EwmaConfidenceAndGoodput)
{
  NetworkTelemetryStore store(0.5, std::chrono::seconds(30));
  const ndn::Name provider("/provider/A");
  const ndn::Name service("/Inference/NativeTracer");

  store.updateAckRtt(provider, service, 10.0);
  store.updateAckRtt(provider, service, 30.0);

  auto snapshot = store.getServicePath(provider, service);
  BOOST_REQUIRE(snapshot);
  BOOST_CHECK_EQUAL(snapshot->providerName, provider);
  BOOST_CHECK_EQUAL(snapshot->serviceName, service);
  BOOST_CHECK_EQUAL(snapshot->sampleCount, 2);
  BOOST_CHECK_CLOSE(snapshot->rttMs, 20.0, 0.001);
  BOOST_CHECK_CLOSE(snapshot->confidence, 0.4, 0.001);
  BOOST_CHECK(!snapshot->stale);

  store.updateLargeDataFetch(ndn::Name("/provider/merge"),
                             ndn::Name("/provider/head0"),
                             "head0-to-merge",
                             ndn::Name("/provider/head0/data/v=1"),
                             100.0,
                             25.0,
                             1000,
                             1000,
                             2000,
                             4,
                             0,
                             0);
  auto edge = store.getDependencyEdge(ndn::Name("/provider/merge"),
                                      ndn::Name("/provider/head0"),
                                      "head0-to-merge");
  BOOST_REQUIRE(edge);
  BOOST_CHECK_EQUAL(edge->peerName, ndn::Name("/provider/head0"));
  BOOST_CHECK_EQUAL(edge->edgeName, "head0-to-merge");
  BOOST_CHECK_EQUAL(edge->payloadBytes, 1000);
  BOOST_CHECK_CLOSE(edge->goodputMbps, 0.08, 0.001);
  BOOST_CHECK_CLOSE(edge->firstByteMs, 25.0, 0.001);
}

BOOST_AUTO_TEST_CASE(TtlMarksSnapshotStaleAndReducesConfidence)
{
  NetworkTelemetryStore store(0.5, std::chrono::milliseconds(1));
  store.updateAckRtt(ndn::Name("/provider/A"), ndn::Name("/HELLO"), 5.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));

  auto snapshot = store.getServicePath(ndn::Name("/provider/A"),
                                       ndn::Name("/HELLO"));
  BOOST_REQUIRE(snapshot);
  BOOST_CHECK(snapshot->stale);
  BOOST_CHECK_LT(snapshot->confidence, 0.2);
}

BOOST_AUTO_TEST_CASE(ExpiredEntriesAreRemovedWhenNewSamplesArrive)
{
  NetworkTelemetryStore store(0.5, std::chrono::milliseconds(1), 4);
  const auto service = ndn::Name("/HELLO");
  store.updateAckRtt(ndn::Name("/provider/old"), service, 5.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));

  store.updateAckRtt(ndn::Name("/provider/current"), service, 7.0);

  BOOST_CHECK_EQUAL(store.size(), 1);
  BOOST_CHECK(!store.getServicePath(ndn::Name("/provider/old"), service));
  BOOST_CHECK(store.getServicePath(ndn::Name("/provider/current"), service));
}

BOOST_AUTO_TEST_CASE(ActiveProbeIsTypedAndDirectionScoped)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/observer");
  const auto target = ndn::Name("/provider/target");
  const std::string requestScope = "request-attempt-candidate-digest";
  const ndn::Name dataName(
    "/provider/target/NDNSF/PEER-PROBE/t=1/nonce/REPLY");

  store.updateActiveProbe(observer, target, requestScope, "target-boot-7",
                          dataName, 12.5);

  const auto forward = store.getActiveProbe(observer, target, requestScope);
  BOOST_REQUIRE(forward);
  BOOST_CHECK(forward->kind == NetworkTelemetrySampleKind::ActiveProbeRtt);
  BOOST_CHECK_EQUAL(forward->providerName, observer);
  BOOST_CHECK_EQUAL(forward->peerName, target);
  BOOST_CHECK_EQUAL(forward->peerBootEpoch, "target-boot-7");
  BOOST_CHECK_EQUAL(forward->dataName, dataName);
  BOOST_CHECK(forward->targetSignatureValidated);
  BOOST_CHECK(forward->probeSignatureValid);
  BOOST_CHECK(forward->edgeName.empty());
  BOOST_CHECK_CLOSE(forward->rttMs, 12.5, 0.001);
  BOOST_CHECK(!store.getActiveProbe(target, observer, requestScope));
  BOOST_CHECK(!store.getActiveProbe(observer, target, "another-request"));
  BOOST_CHECK(!store.getServicePath(observer, target));
}

BOOST_AUTO_TEST_CASE(ActiveProbeRequiresLiveAuthenticatedExactPairs)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/observer");
  const auto target = ndn::Name("/provider/target");
  const auto third = ndn::Name("/provider/third");
  const auto outsider = ndn::Name("/provider/outsider");
  const auto expiresAt = std::chrono::steady_clock::now() +
                         std::chrono::seconds(5);

  BOOST_CHECK(store.authorizeActiveProbeRequest(
    "authenticated-request-scope", {target, observer, third, observer},
    {{observer, target}, {target, third}},
    {{target, observer}, {third, target}},
    expiresAt));
  BOOST_CHECK(store.isActiveProbeAuthorized(
    "authenticated-request-scope", observer, target));
  BOOST_CHECK(store.isActiveProbeAuthorized(
    "authenticated-request-scope", target, third));
  BOOST_CHECK(store.isActiveProbeGoodputAuthorized(
    "authenticated-request-scope", target, observer));
  BOOST_CHECK(!store.isActiveProbeGoodputAuthorized(
    "authenticated-request-scope", observer, target));
  BOOST_CHECK(!store.isActiveProbeGoodputAuthorized(
    "authenticated-request-scope", outsider, observer));
  BOOST_CHECK(!store.isActiveProbeAuthorized(
    "authenticated-request-scope", observer, outsider));
  BOOST_CHECK(!store.isActiveProbeAuthorized(
    "another-request-scope", observer, target));
  BOOST_CHECK(!store.isActiveProbeAuthorized(
    "authenticated-request-scope", observer, observer));

  // Replays cannot replace the authorized roster for an existing scope.
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "authenticated-request-scope", {observer, outsider}, {}, {},
    std::chrono::steady_clock::now() + std::chrono::minutes(1)));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "expired-request-scope", {observer, target}, {}, {},
    std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "single-provider-scope", {observer, observer}, {}, {},
    std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "bad-edge-scope", {observer, target}, {}, {{outsider, observer}},
    std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "duplicate-edge-scope", {observer, target},
    {},
    {{target, observer}, {target, observer}},
    std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  BOOST_CHECK(!store.isActiveProbeAuthorized(
    "authenticated-request-scope", third, observer));
  BOOST_CHECK(!store.claimActiveProbeRttPair(
    "authenticated-request-scope", third, observer));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "duplicate-rtt-pair-scope", {observer, target},
    {{observer, target}, {observer, target}}, {},
    std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(ClosingProbeCollectionStopsOutboundClaimsWithoutBreakingReplies)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/observer");
  const auto target = ndn::Name("/provider/target");
  const std::string scope = "cancelled-collection-scope";
  const auto expiresAt = std::chrono::steady_clock::now() +
                         std::chrono::seconds(5);

  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    scope, {observer, target}, {{observer, target}}, {{target, observer}},
    expiresAt));
  BOOST_CHECK(store.isActiveProbeCollectionOpen(scope));
  BOOST_REQUIRE(store.closeActiveProbeCollection(scope));
  BOOST_CHECK(!store.isActiveProbeCollectionOpen(scope));
  BOOST_CHECK(!store.claimActiveProbeRttPair(scope, observer, target));
  BOOST_CHECK(!store.claimActiveProbeGoodputEdge(scope, target, observer));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    scope, {observer, target}, {{observer, target}}, {{target, observer}},
    std::chrono::steady_clock::now() + std::chrono::minutes(1)));

  // Closing the observer's outbound collection does not revoke this signed
  // request's receiver-side permission for an already-issued probe.
  BOOST_CHECK(store.isActiveProbeAuthorized(scope, observer, target));
  BOOST_CHECK(store.isActiveProbeGoodputAuthorized(scope, target, observer));
  BOOST_CHECK(store.acceptActiveProbeGoodputSegment(
    scope, target, observer, "run-1", 0, NETWORK_PROBE_WARMED_SEGMENTS));
}

BOOST_AUTO_TEST_CASE(LiveAuthorizationCapacityDoesNotResetProbeClaims)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/observer");
  const auto source = ndn::Name("/provider/source");
  const std::vector<ndn::Name> candidates{observer, source};
  const std::vector<NetworkTelemetryDataEdge> rttPairs{{observer, source}};
  const std::vector<NetworkTelemetryDataEdge> edges{{source, observer}};
  const auto expiresAt = std::chrono::steady_clock::now() +
                         std::chrono::minutes(1);

  for (size_t i = 0; i < 1024; ++i) {
    BOOST_REQUIRE(store.authorizeActiveProbeRequest(
      "capacity-scope-" + std::to_string(i), candidates, rttPairs, edges, expiresAt));
  }

  const std::string firstScope = "capacity-scope-0";
  BOOST_REQUIRE(store.claimActiveProbeRttPair(firstScope, observer, source));
  BOOST_REQUIRE(store.claimActiveProbeGoodputEdge(firstScope, source, observer));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "capacity-scope-overflow", candidates, rttPairs, edges, expiresAt));

  // A replay of a live row retains its original one-shot probe budget.
  BOOST_CHECK(store.authorizeActiveProbeRequest(
    firstScope, candidates, rttPairs, edges,
    std::chrono::steady_clock::now() + std::chrono::minutes(2)));
  BOOST_CHECK(!store.claimActiveProbeRttPair(firstScope, observer, source));
  BOOST_CHECK(!store.claimActiveProbeGoodputEdge(firstScope, source, observer));
}

BOOST_AUTO_TEST_CASE(ActiveRttProbePairClaimsAreAtomicAndBounded)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/observer");
  std::vector<ndn::Name> candidates{observer};
  std::vector<NetworkTelemetryDataEdge> rttPairs;
  for (size_t i = 0; i < 16; ++i) {
    ndn::Name target("/provider/target");
    target.append(std::to_string(i));
    candidates.push_back(std::move(target));
    rttPairs.push_back({observer, candidates.back()});
  }
  const std::string scope = "bounded-rtt-probe-scope";
  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    scope, candidates, rttPairs, {},
    std::chrono::steady_clock::now() + std::chrono::seconds(5)));

  std::atomic<bool> start{false};
  std::atomic<size_t> successfulClaims{0};
  std::vector<std::thread> claimers;
  for (size_t i = 0; i < 8; ++i) {
    claimers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (store.claimActiveProbeRttPair(scope, observer, candidates[1])) {
        successfulClaims.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& claimer : claimers) {
    claimer.join();
  }
  BOOST_CHECK_EQUAL(successfulClaims.load(), 1);
  BOOST_CHECK(!store.claimActiveProbeRttPair(scope, observer, candidates[1]));
  BOOST_CHECK(!store.claimActiveProbeRttPair(
    scope, ndn::Name("/provider/outsider"), candidates[1]));

  for (size_t i = 2; i < candidates.size(); ++i) {
    BOOST_CHECK(store.claimActiveProbeRttPair(scope, observer, candidates[i]));
  }
  BOOST_CHECK_EQUAL(candidates.size() - 1, 16);
  BOOST_CHECK(!store.claimActiveProbeRttPair(
    scope, candidates[1], observer));
}

BOOST_AUTO_TEST_CASE(ActiveGoodputEdgeClaimsBoundRequestAggregateBudget)
{
  NetworkTelemetryStore store;
  const auto destination = ndn::Name("/provider/destination");
  const std::string scope = "bounded-goodput-request-scope";
  std::vector<ndn::Name> candidates;
  std::vector<NetworkTelemetryDataEdge> edges;
  candidates.reserve(17);
  edges.reserve(16);
  for (size_t i = 0; i < 16; ++i) {
    auto source = ndn::Name("/provider/source");
    source.append(std::to_string(i));
    candidates.push_back(source);
    edges.push_back({source, destination});
  }
  candidates.push_back(destination);
  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    scope, candidates, {}, edges,
    std::chrono::steady_clock::now() + std::chrono::seconds(5)));

  std::atomic<bool> start{false};
  std::atomic<size_t> successfulClaims{0};
  std::vector<std::thread> claimers;
  for (size_t i = 0; i < 8; ++i) {
    claimers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (store.claimActiveProbeGoodputEdge(scope,
                                            edges.front().srcProvider,
                                            edges.front().dstProvider)) {
        successfulClaims.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& claimer : claimers) {
    claimer.join();
  }
  BOOST_CHECK_EQUAL(successfulClaims.load(), 1);

  size_t claimedEdges = 1;
  for (size_t i = 1; i < edges.size(); ++i) {
    if (store.claimActiveProbeGoodputEdge(scope,
                                          edges[i].srcProvider,
                                          edges[i].dstProvider)) {
      ++claimedEdges;
    }
  }
  BOOST_CHECK_EQUAL(claimedEdges, 16);
  constexpr size_t maxWireBytesPerEdge = NETWORK_PROBE_WARMED_SEGMENTS * (2048 + 6144);
  BOOST_CHECK_EQUAL(maxWireBytesPerEdge, 128 * 1024);
  BOOST_CHECK_EQUAL(claimedEdges * maxWireBytesPerEdge, 2 * 1024 * 1024);
  BOOST_CHECK(!store.claimActiveProbeGoodputEdge(
    scope, edges.front().srcProvider, edges.front().dstProvider));

  // Re-authorizing an identical scope does not reset spent edge reservations.
  BOOST_CHECK(store.authorizeActiveProbeRequest(
    scope, candidates, {}, edges,
    std::chrono::steady_clock::now() + std::chrono::minutes(1)));
  BOOST_CHECK(!store.claimActiveProbeGoodputEdge(
    scope, edges.front().srcProvider, edges.front().dstProvider));
}

BOOST_AUTO_TEST_CASE(Spec197CancelledRegistrationClosesScopeAndCannotReviveIt)
{
  NetworkTelemetryStore store;
  const ndn::Name source("/provider/source"), destination("/provider/destination");
  const std::vector<ndn::Name> candidates{source, destination};
  const std::vector<NetworkTelemetryDataEdge> rtts{{destination, source}};
  const std::vector<NetworkTelemetryDataEdge> edges{{source, destination}};
  const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto registration = std::make_shared<std::atomic<bool>>(false);
  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    "cancelled-registration-scope", candidates, rtts, edges, expires, registration));
  BOOST_REQUIRE(store.activeProbeScopeDeadline("cancelled-registration-scope"));
  registration->store(true);
  BOOST_CHECK(!store.activeProbeScopeDeadline("cancelled-registration-scope"));
  BOOST_CHECK(!store.isActiveProbeCollectionOpen("cancelled-registration-scope"));
  BOOST_CHECK(!store.isActiveProbeAuthorized("cancelled-registration-scope", destination, source));
  BOOST_CHECK(!store.isActiveProbeGoodputAuthorized("cancelled-registration-scope", source, destination));
  BOOST_CHECK(!store.claimActiveProbeRttPair("cancelled-registration-scope", destination, source));
  BOOST_CHECK(!store.claimActiveProbeGoodputEdge("cancelled-registration-scope", source, destination));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    "cancelled-registration-scope", source, destination, "1", 0, 4));
  auto newRegistration = std::make_shared<std::atomic<bool>>(false);
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "cancelled-registration-scope", candidates, rtts, edges, expires, newRegistration));
  BOOST_CHECK(!store.authorizeActiveProbeRequest(
    "already-cancelled-scope", candidates, rtts, edges, expires, registration));
  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    "new-registration-scope", candidates, rtts, edges, expires, newRegistration));
  BOOST_CHECK(store.isActiveProbeGoodputAuthorized("new-registration-scope", source, destination));
}

BOOST_AUTO_TEST_CASE(ActiveGoodputReceiverAdmitsOneTrainAndUniqueSegments)
{
  NetworkTelemetryStore store;
  const auto source = ndn::Name("/provider/source");
  const auto destination = ndn::Name("/provider/destination");
  const std::string scope = "receiver-goodput-request-scope";
  const std::vector<ndn::Name> candidates{source, destination};
  const std::vector<NetworkTelemetryDataEdge> edges{{source, destination}};
  const auto expiresAt = std::chrono::steady_clock::now() +
                         std::chrono::seconds(5);
  BOOST_REQUIRE(store.authorizeActiveProbeRequest(
    scope, candidates, {}, edges, expiresAt));

  std::atomic<bool> start{false};
  std::atomic<size_t> accepted{0};
  std::vector<std::thread> requesters;
  for (size_t i = 0; i < 8; ++i) {
    requesters.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (store.acceptActiveProbeGoodputSegment(
            scope, source, destination, "run-1", 0, NETWORK_PROBE_WARMED_SEGMENTS)) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& requester : requesters) {
    requester.join();
  }
  BOOST_CHECK_EQUAL(accepted.load(), 1);

  // Arrival order may vary, but the other unique segments from the same
  // authenticated train are accepted exactly once.
  BOOST_CHECK(store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 3, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 1, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 2, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 1, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-2", 0, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", NETWORK_PROBE_WARMED_SEGMENTS, NETWORK_PROBE_WARMED_SEGMENTS));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 0, 3));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-1", 0, 4));
  // Every upper bit must retain its admission, including segments 8..15.
  for (size_t segment = 4; segment < NETWORK_PROBE_WARMED_SEGMENTS; ++segment) {
    BOOST_CHECK(store.acceptActiveProbeGoodputSegment(
      scope, source, destination, "run-1", segment, NETWORK_PROBE_WARMED_SEGMENTS));
    BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
      scope, source, destination, "run-1", segment, NETWORK_PROBE_WARMED_SEGMENTS));
  }

  // A same-scope authorization replay preserves the receiver's train state.
  BOOST_CHECK(store.authorizeActiveProbeRequest(
    scope, candidates, {}, edges,
    std::chrono::steady_clock::now() + std::chrono::minutes(1)));
  BOOST_CHECK(!store.acceptActiveProbeGoodputSegment(
    scope, source, destination, "run-2", 0, NETWORK_PROBE_WARMED_SEGMENTS));
}

BOOST_AUTO_TEST_CASE(ActiveProbeGoodputUsesPayloadAndRequiresCompleteTrain)
{
  NetworkTelemetryStore store;
  const auto observer = ndn::Name("/provider/destination");
  const auto source = ndn::Name("/provider/source");
  const std::string scope = "goodput-scope";
  const std::string profile = "sha256:" + std::string(64, 'a');

  NetworkTelemetrySample incomplete;
  incomplete.kind = NetworkTelemetrySampleKind::ActiveProbeGoodput;
  incomplete.elapsedMs = 10.0;
  incomplete.payloadBytes = 4096;
  incomplete.wireBytes = 5000;
  incomplete.receivedSegments = 1;
  incomplete.probeSegmentsSent = 2;
  incomplete.probeProfileDigest = profile;
  incomplete.probeComplete = false;
  incomplete.probeSignatureValid = false;
  incomplete.peerBootEpoch = "boot-1";
  store.update({observer, ndn::Name(), source, scope}, incomplete);
  BOOST_CHECK_EQUAL(store.size(), 0);

  auto invalidSignature = incomplete;
  invalidSignature.elapsedMs = 20.0;
  invalidSignature.receivedSegments = NETWORK_PROBE_WARMED_SEGMENTS;
  invalidSignature.probeSegmentsSent = NETWORK_PROBE_WARMED_SEGMENTS;
  invalidSignature.probeComplete = true;
  invalidSignature.probeSignatureValid = true;
  invalidSignature.firstByteMs = 2.0;
  store.update({observer, ndn::Name(), source, scope}, invalidSignature);
  BOOST_CHECK_EQUAL(store.size(), 0);

  NetworkTelemetryStore goodputOnlyStore;
  auto goodputOnly = invalidSignature;
  goodputOnly.probeSignatureValid = false;
  goodputOnlyStore.update({observer, ndn::Name(), source, scope}, goodputOnly);
  const auto goodputOnlySample =
    goodputOnlyStore.getActiveProbeGoodput(observer, source, scope);
  BOOST_REQUIRE(goodputOnlySample);
  BOOST_CHECK(!goodputOnlySample->probeSignatureValid);
  BOOST_CHECK_EQUAL(goodputOnlySample->payloadBytes, 4096);

  NetworkProbePacketTimes rttMs; rttMs.fill(18.0);
  NetworkProbeArrivalGaps interestGapsMs; interestGapsMs.fill(4.0);
  NetworkProbeArrivalGaps dataArrivalGapsMs; dataArrivalGapsMs.fill(3.2);
  NetworkProbeWireSizes dataWireBytes; dataWireBytes.fill(2300);
  const ndn::Name dataName(
    "/provider/source/NDNSF/PEER-GOODPUT/t=1/run/seg=0/REPLY");
  store.updateActiveProbeGoodput(observer, source, scope, "boot-1", dataName,
                                4096, 10000, NETWORK_PROBE_WARMED_SEGMENTS, 0.0, 20.0, profile, true,
                                rttMs, interestGapsMs, dataArrivalGapsMs,
                                dataWireBytes);
  const auto sample = store.getActiveProbeGoodput(observer, source, scope);
  BOOST_REQUIRE(sample);
  BOOST_CHECK(sample->kind == NetworkTelemetrySampleKind::ActiveProbeGoodput);
  BOOST_CHECK_EQUAL(sample->payloadBytes, 4096);
  BOOST_CHECK_EQUAL(sample->firstByteMs, 0.0);
  BOOST_CHECK_EQUAL(sample->wireBytes, 10000);
  BOOST_CHECK_EQUAL(sample->receivedSegments, NETWORK_PROBE_WARMED_SEGMENTS);
  BOOST_CHECK_EQUAL(sample->probeSegmentsSent, NETWORK_PROBE_WARMED_SEGMENTS);
  BOOST_CHECK(sample->probeComplete);
  BOOST_CHECK(sample->targetSignatureValidated);
  BOOST_CHECK_EQUAL(sample->probeProfileDigest, profile);
  BOOST_CHECK(sample->probeSignatureValid);
  BOOST_CHECK_EQUAL(sample->probeRttMs[2], 18.0);
  BOOST_CHECK_EQUAL(sample->probeInterestGapsMs[1], 4.0);
  BOOST_CHECK_EQUAL(sample->probeDataArrivalGapsMs[2], 3.2);
  BOOST_CHECK_EQUAL(sample->probeDataWireBytes[0], 2300);
  BOOST_CHECK_EQUAL(sample->dataName, dataName);
  BOOST_CHECK(sample->edgeName.empty());
  BOOST_CHECK_CLOSE(sample->goodputMbps, 1.6384, 0.001);
  BOOST_CHECK(!store.getActiveProbeGoodput(source, observer, scope));
}

BOOST_AUTO_TEST_CASE(InvalidActiveProbeSamplesAreIgnored)
{
  NetworkTelemetryStore store;
  NetworkTelemetrySample unverifiedRtt;
  unverifiedRtt.kind = NetworkTelemetrySampleKind::ActiveProbeRtt;
  unverifiedRtt.rttMs = 1.0;
  unverifiedRtt.elapsedMs = 1.0;
  unverifiedRtt.peerBootEpoch = "boot";
  store.update({ndn::Name("/provider/A"), ndn::Name(),
                ndn::Name("/provider/B"), "scope"}, unverifiedRtt);
  store.updateActiveProbe(ndn::Name("/provider/A"),
                          ndn::Name("/provider/B"), "scope", "boot",
                          ndn::Name("/provider/B/NDNSF/PEER-PROBE/t=1/n/REPLY"),
                          std::numeric_limits<double>::infinity());
  store.updateActiveProbe(ndn::Name("/provider/A"),
                          ndn::Name("/provider/A"), "scope", "boot",
                          ndn::Name("/provider/A/NDNSF/PEER-PROBE/t=1/n/REPLY"), 1.0);
  store.updateActiveProbe(ndn::Name("/provider/A"),
                          ndn::Name("/provider/B"), "", "boot",
                          ndn::Name("/provider/B/NDNSF/PEER-PROBE/t=1/n/REPLY"), 1.0);
  store.updateActiveProbe(ndn::Name("/provider/A"),
                          ndn::Name("/provider/B"), "scope", "",
                          ndn::Name("/provider/B/NDNSF/PEER-PROBE/t=1/n/REPLY"), 1.0);

  BOOST_CHECK_EQUAL(store.size(), 0);
}

BOOST_AUTO_TEST_CASE(ActiveProbeDoesNotBlendAcrossTargetBoots)
{
  NetworkTelemetryStore store(1.0, std::chrono::seconds(30));
  const auto observer = ndn::Name("/provider/observer");
  const auto target = ndn::Name("/provider/target");

  store.updateActiveProbe(observer, target, "scope", "boot-1",
                          ndn::Name("/provider/target/NDNSF/PEER-PROBE/t=1/n1/REPLY"), 10.0);
  store.updateActiveProbe(observer, target, "scope", "boot-2",
                          ndn::Name("/provider/target/NDNSF/PEER-PROBE/t=2/n2/REPLY"), 20.0);

  const auto sample = store.getActiveProbe(observer, target, "scope");
  BOOST_REQUIRE(sample);
  BOOST_CHECK_EQUAL(sample->peerBootEpoch, "boot-2");
  BOOST_CHECK_EQUAL(sample->sampleCount, 1);
  BOOST_CHECK_CLOSE(sample->rttMs, 20.0, 0.001);
}

BOOST_AUTO_TEST_CASE(StoreEvictsOldestEntryAtConfiguredBound)
{
  NetworkTelemetryStore store(0.5, std::chrono::seconds(30), 2);
  const auto service = ndn::Name("/HELLO");
  const auto oldest = ndn::Name("/provider/oldest");
  const auto newer = ndn::Name("/provider/newer");
  const auto newest = ndn::Name("/provider/newest");

  store.updateAckRtt(oldest, service, 5.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  store.updateAckRtt(newer, service, 7.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  store.updateAckRtt(newest, service, 9.0);

  BOOST_CHECK_EQUAL(store.size(), 2);
  BOOST_CHECK(!store.getServicePath(oldest, service));
  BOOST_CHECK(store.getServicePath(newer, service));
  BOOST_CHECK(store.getServicePath(newest, service));
}

BOOST_AUTO_TEST_CASE(ConcurrentUpdatesAndReadsAreSynchronized)
{
  NetworkTelemetryStore store(1.0, std::chrono::seconds(30), 8);
  const auto provider = ndn::Name("/provider/shared");
  const auto service = ndn::Name("/HELLO");
  constexpr size_t THREAD_COUNT = 4;
  constexpr size_t UPDATES_PER_THREAD = 100;
  std::vector<std::thread> workers;

  for (size_t i = 0; i < THREAD_COUNT; ++i) {
    workers.emplace_back([&, i] {
      for (size_t j = 0; j < UPDATES_PER_THREAD; ++j) {
        store.updateAckRtt(provider, service,
                           static_cast<double>(i + j + 1));
        static_cast<void>(store.getServicePath(provider, service));
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto snapshot = store.getServicePath(provider, service);
  BOOST_REQUIRE(snapshot);
  BOOST_CHECK_EQUAL(snapshot->sampleCount, THREAD_COUNT * UPDATES_PER_THREAD);
  BOOST_CHECK_EQUAL(store.size(), 1);
}

BOOST_AUTO_TEST_CASE(CustomSelectionCanPreferLowerRttTelemetry)
{
  AckSelectionCandidate slow;
  slow.providerName = ndn::Name("/provider/slow");
  slow.serviceName = ndn::Name("/HELLO");
  slow.requestId = ndn::Name("/request/1");
  slow.ack.setStatus(true);
  NetworkTelemetrySnapshot slowTelemetry;
  slowTelemetry.rttMs = 30.0;
  slowTelemetry.confidence = 1.0;
  slow.telemetry = slowTelemetry;

  AckSelectionCandidate fast = slow;
  fast.providerName = ndn::Name("/provider/fast");
  NetworkTelemetrySnapshot fastTelemetry;
  fastTelemetry.rttMs = 8.0;
  fastTelemetry.confidence = 1.0;
  fast.telemetry = fastTelemetry;

  std::vector<AckSelectionCandidate> candidates{slow, fast};
  const auto best = std::min_element(
    candidates.begin(), candidates.end(),
    [] (const AckSelectionCandidate& lhs,
        const AckSelectionCandidate& rhs) {
      return lhs.telemetry->rttMs < rhs.telemetry->rttMs;
    });

  BOOST_REQUIRE(best != candidates.end());
  BOOST_CHECK_EQUAL(best->providerName, ndn::Name("/provider/fast"));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
