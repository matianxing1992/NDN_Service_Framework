/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "ndn-service-framework/NetworkTelemetry.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "tests/boost-test.hpp"

#include <algorithm>
#include <thread>

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
  BOOST_CHECK_CLOSE(edge->goodputMbps, 0.16, 0.001);
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
