#include "tests/boost-test.hpp"
#include "examples/DI_NetworkProbeCalibration.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <array>
#include <numeric>
#include <utility>

using namespace ndnsf::di;
using namespace ndnsf::di::probe_calibration;
using namespace ndn_service_framework;

namespace {
NativeJson request()
{
  return NativeJson{{"request_id", "/example/probe/user/request/1"}, {"attempt", 1},
    {"requester_identity", "/example/probe/user"}, {"observer_provider", "/example/probe/observer"},
    {"service", "/NDNSF/Calibration"}, {"scope_digest", "sha256:" + std::string(64, 'a')},
    {"intent_nonce", std::string(32, 'b')}, {"deadline_ms", 2000},
    {"goodput_edges", NativeJson::array({{{"src_provider", "/example/probe/source"},
      {"dst_provider", "/example/probe/observer"}}})}};
}

NativeJson response()
{
  auto value = request();
  value.erase("goodput_edges");
  value["schema"] = "ndnsf-di-placement-probe-response-v1";
  value["observer_boot_epoch"] = "observer-boot";
  value["completed_at_ms"] = 1100;
  value["requested_rtt_count"] = 1;
  value["requested_goodput_count"] = 1;
  value["omitted_rtt_count"] = 0;
  value["omitted_goodput_count"] = 0;
  NetworkTelemetrySnapshot timing;
  timing.kind = NetworkTelemetrySampleKind::ActiveProbeGoodput;
  timing.probeComplete = true;
  timing.probeSegmentsSent = NETWORK_PROBE_WARMED_SEGMENTS;
  timing.receivedSegments = NETWORK_PROBE_WARMED_SEGMENTS;
  timing.targetSignatureValidated = true;
  timing.probeSignatureValid = true;
  timing.probeProfileDigest = NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
  timing.probeSourceTimingValidated = true;
  timing.probeDataArrivalGapsMs.fill(4.0);
  timing.probeSourceSendGapsMs.fill(0.01);
  timing.probeDataWireBytes.fill(4400);
  NativeJson row{{"observer", "/example/probe/observer"}, {"target", "/example/probe/source"},
    {"kind", "active_probe_goodput"}, {"observed_at_ms", 1000}, {"expires_at_ms", 2000},
    {"peer_boot_epoch", "source-boot"}, {"data_name", "/example/probe/source/NDNSF/PEER-GOODPUT/reply"},
    {"target_signature_validated", true}, {"train_complete", true}, {"sent_segments", NETWORK_PROBE_WARMED_SEGMENTS},
    {"received_segments", NETWORK_PROBE_WARMED_SEGMENTS}, {"payload_bytes", 69000}, {"probe_profile_validated", true},
    {"profile_digest", NETWORK_PROBE_TRAIN_PROFILE_DIGEST},
    {"probe_data_arrival_gaps_ms", timing.probeDataArrivalGapsMs},
    {"probe_data_wire_bytes", timing.probeDataWireBytes}};
  row.update(nativeProbeTrainRateMetadataV4(timing));
  NativeJson rtt{{"observer", "/example/probe/observer"}, {"target", "/example/probe/source"},
    {"kind", "active_probe_rtt"}, {"value", 20.0}, {"observed_at_ms", 1000},
    {"expires_at_ms", 2000}, {"peer_boot_epoch", "source-boot"},
    {"data_name", "/example/probe/source/NDNSF/PEER-PROBE/reply"},
    {"target_signature_validated", true}};
  value["observations"] = NativeJson::array({row, rtt});
  return value;
}

NativeJson point(double rate)
{
  return NativeJson{{"status", "MEASURED_GOODPUT"}, {"wire_equivalent_mbps", rate}};
}

NativeJson matrix()
{
  const std::array<std::pair<unsigned, unsigned>, 6> links{{
    {1, 10}, {4, 10}, {10, 10}, {4, 2}, {4, 30}, {4, 70}}};
  auto cells = NativeJson::array();
  for (const auto& link : links) {
    auto records = NativeJson::array();
    for (unsigned i = 0; i < 5; ++i) {
      auto record = point(link.first);
      record["response"] = NativeJson{{"completed_at_ms", std::uint64_t{100'000 + i * 15'000}}};
      records.push_back(std::move(record));
    }
    cells.push_back(NativeJson{{"bandwidth_mbps", link.first}, {"delay_ms", link.second},
      {"control", nullptr}, {"authorization_case", nullptr},
      {"native_result", summarize(std::move(records), link.first)}});
  }
  return cells;
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec197ProbeCalibration)

BOOST_AUTO_TEST_CASE(ValidTrainUsesMeasuredWindowWireSizes)
{
  const auto record = recordFromResponse(response(), request(), 1);
  BOOST_CHECK_EQUAL(record.at("status").get<std::string>(), "MEASURED_GOODPUT");
  BOOST_CHECK_CLOSE(record.at("goodput_mbps").get<double>(), 8.192, 1e-9);
  BOOST_CHECK_CLOSE(record.at("wire_equivalent_mbps").get<double>(), 8.8, 1e-9);
  BOOST_CHECK_EQUAL(record.at("reference_data_wire_bytes").get<double>(), 52800.0);
  BOOST_CHECK_EQUAL(record.at("rtt_ms").get<double>(), 20.0);
  auto altered = response();
  altered["observations"][0]["probe_data_wire_bytes"][0] = 6000;
  altered["observations"][0]["probe_data_wire_bytes"][15] = 6000;
  altered["observations"][1]["value"] = 140.0;
  const auto second = recordFromResponse(altered, request(), 2);
  BOOST_CHECK_EQUAL(second.at("wire_equivalent_mbps"), record.at("wire_equivalent_mbps"));
  BOOST_CHECK_EQUAL(second.at("rtt_ms").get<double>(), 140.0);
}

BOOST_AUTO_TEST_CASE(DominatedTrainRemainsUnknownThroughProductionMetadataAndCalibration)
{
  auto reply = response();
  NetworkTelemetrySnapshot timing;
  timing.kind = NetworkTelemetrySampleKind::ActiveProbeGoodput;
  timing.probeComplete = true;
  timing.probeSegmentsSent = NETWORK_PROBE_WARMED_SEGMENTS;
  timing.receivedSegments = NETWORK_PROBE_WARMED_SEGMENTS;
  timing.targetSignatureValidated = true;
  timing.probeSignatureValid = true;
  timing.probeProfileDigest = NETWORK_PROBE_TRAIN_PROFILE_DIGEST;
  timing.probeSourceTimingValidated = true;
  timing.probeDataArrivalGapsMs.fill(0.01);
  timing.probeDataArrivalGapsMs[7] = 6.630;
  timing.probeDataArrivalGapsMs[13] = 9.312;
  timing.probeSourceSendGapsMs.fill(0.001);
  timing.probeDataWireBytes.fill(4400);
  auto& row = reply["observations"][0];
  row["probe_data_arrival_gaps_ms"] = timing.probeDataArrivalGapsMs;
  row.update(nativeProbeTrainRateMetadataV4(timing));
  BOOST_CHECK_EQUAL(row.at("probe_rate_status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(row.at("probe_rate_reason").get<std::string>(), "ARRIVAL_GAP_DOMINATED");
  BOOST_CHECK(row.at("probe_train_goodput_mbps").is_null());
  const auto record = recordFromResponse(reply, request(), 1);
  BOOST_CHECK_EQUAL(record.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(record.at("reason").get<std::string>(), "ARRIVAL_GAP_DOMINATED");
  BOOST_CHECK(record.at("goodput_mbps").is_null());
  BOOST_CHECK(record.at("wire_equivalent_mbps").is_null());
  BOOST_CHECK_EQUAL(record.at("rtt_ms").get<double>(), 20.0);
}

BOOST_AUTO_TEST_CASE(ProductionEncodedContentIsDistinctFromDedicatedPayload)
{
  auto actual = response();
  auto& row = actual["observations"][0];
  std::size_t contentBytes = 0;
  NetworkProbeWireSizes wireSizes{};
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const std::string source = "/NDNSF-DistributeInference/example/provider/A";
  const std::string observer = "/NDNSF-DistributeInference/example/provider/B";
  keys.createIdentity(ndn::Name(source), ndn::RsaKeyParams(2048));
  BOOST_REQUIRE_LE(NETWORK_PROBE_WARMED_PAYLOAD_BYTES,
                   DeploymentControlMessage::MAX_FIELD_VALUE);
  for (std::size_t index = 0; index < NETWORK_PROBE_WARMED_SEGMENTS; ++index) {
    // Match the source's nine fields, plus fourteen timing fields in Data 15.
    // This checks the production encoder's byte semantics, not peer trust.
    PeerNetworkGoodputProbeResponse encoded;
    encoded.setField("observerProvider", observer);
    encoded.setField("targetProvider", source);
    encoded.setField("requestScopeDigest", "sha256:" + std::string(64, 'a'));
    encoded.setField("runNonce", "18446744073709551615");
    encoded.setField("segment", std::to_string(index));
    encoded.setField("segmentCount", std::to_string(NETWORK_PROBE_WARMED_SEGMENTS));
    encoded.setField("targetBootEpoch", std::string(128, 'b'));
    encoded.setField("payload", std::string(NETWORK_PROBE_WARMED_PAYLOAD_BYTES, 'x'));
    encoded.setField("probeProfileDigest", NETWORK_PROBE_TRAIN_PROFILE_DIGEST);
    if (index == NETWORK_PROBE_WARMED_SEGMENTS - 1) {
      for (std::size_t gap = 0; gap < NETWORK_PROBE_WARMED_SEGMENTS - 2; ++gap)
        encoded.setField("sourceSendGap" + std::to_string(gap) + "Ns", "500000000");
    }
    ndn::Data data(ndn::Name(source + "/NDNSF/PEER-GOODPUT/1791002497438/"
      "18446744073709551615/seg=" + std::to_string(index) +
      "/params-sha256=" + std::string(64, 'a') + "/reply"));
    data.setFreshnessPeriod(ndn::time::milliseconds::zero());
    data.setContent(encoded.WireEncode());
    keys.sign(data, ndn::security::signingByIdentity(ndn::Name(source)));
    BOOST_CHECK_LE(data.getContent().value_size(), NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES);
    BOOST_CHECK_LE(data.wireEncode().size(), 6144);
    contentBytes += data.getContent().value_size();
    wireSizes[index] = data.wireEncode().size();
  }
  BOOST_REQUIRE_GT(contentBytes, NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES);
  row["payload_bytes"] = contentBytes;
  row["probe_data_wire_bytes"] = wireSizes;
  const auto record = recordFromResponse(actual, request(), 1);
  BOOST_CHECK_CLOSE(record.at("goodput_mbps").get<double>(), 8.192, 1e-9);
  BOOST_CHECK_CLOSE(record.at("wire_equivalent_mbps").get<double>(),
                    static_cast<double>(std::accumulate(wireSizes.begin() + 3,
                      wireSizes.begin() + 15, std::size_t{0})) / 6000.0, 1e-9);
  auto previousProfile = response();
  previousProfile["observations"][0]["profile_digest"] =
    "sha256:a90e70c4d0e7582bfbc939060bde663ab34ca051685cf1f7dff1a8f28c416942";
  BOOST_CHECK_THROW(recordFromResponse(previousProfile, request(), 1), std::exception);
  previousProfile["observations"][0]["profile_digest"] =
    "sha256:54c49db41d669a3392a9d7bedd44f93811a29d3bdbba8744748d6b5a7dd0a51e";
  BOOST_CHECK_THROW(recordFromResponse(previousProfile, request(), 1), std::exception);
  previousProfile["observations"][0]["profile_digest"] =
    "sha256:7488a1b40f3323148a898ce1f80176a124ff844f56c43b9448444db5a36e19a8";
  BOOST_CHECK_THROW(recordFromResponse(previousProfile, request(), 1), std::exception);
  row["payload_bytes"] = NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES;
  BOOST_CHECK_THROW(recordFromResponse(actual, request(), 1), std::exception);
  row["payload_bytes"] = NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES + 1;
  BOOST_CHECK_THROW(recordFromResponse(actual, request(), 1), std::exception);
  row["payload_bytes"] = std::accumulate(wireSizes.begin(), wireSizes.end(), std::size_t{0});
  BOOST_CHECK_THROW(recordFromResponse(actual, request(), 1), std::exception);
  auto legacyProfile = response();
  legacyProfile["observations"][0]["profile_digest"] =
    "sha256:353752eabb3d2b6eb3dc8ebef6432336d67018eba5312f56af9eb6a0295b79c9";
  BOOST_CHECK_THROW(recordFromResponse(legacyProfile, request(), 1), std::exception);
}

BOOST_AUTO_TEST_CASE(MissingAndReorderedTrainsRemainUnknown)
{
  auto absent = response();
  absent["observations"].erase(0);
  absent["omitted_goodput_count"] = 1;
  const auto missing = recordFromResponse(absent, request(), 1);
  BOOST_CHECK_EQUAL(missing.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(missing.at("reason").get<std::string>(), "MISSING_TRAIN");
  auto reordered = response();
  auto& row = reordered["observations"][0];
  row["probe_profile_validated"] = false;
  row["probe_data_arrival_gaps_ms"][0] = -4.0;
  row["probe_rate_status"] = "UNKNOWN";
  row["probe_rate_reason"] = "SOURCE_TIMING_UNVERIFIED";
  row["probe_train_goodput_mbps"] = nullptr;
  const auto record = recordFromResponse(reordered, request(), 2);
  BOOST_CHECK_EQUAL(record.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(record.at("reason").get<std::string>(), "SOURCE_TIMING_UNVERIFIED");
  BOOST_CHECK(record.at("goodput_mbps").is_null());
  const auto allUnknown = summarize(NativeJson::array({missing, record}), 4.0);
  BOOST_CHECK_EQUAL(allUnknown.at("unknown_fraction").get<double>(), 1.0);
  BOOST_CHECK(!allUnknown.at("accuracy_pass").get<bool>());
  BOOST_CHECK(allUnknown.at("median_wire_equivalent_mbps").is_null());
  auto unverified = response();
  unverified["observations"][0]["probe_source_timing_validated"] = false;
  unverified["observations"][0]["probe_rate_status"] = "UNKNOWN";
  unverified["observations"][0]["probe_rate_reason"] = "SOURCE_TIMING_UNVERIFIED";
  unverified["observations"][0]["probe_train_goodput_mbps"] = nullptr;
  const auto sourceUnknown = recordFromResponse(unverified, request(), 3);
  BOOST_CHECK_EQUAL(sourceUnknown.at("status").get<std::string>(), "UNKNOWN");
  BOOST_CHECK_EQUAL(sourceUnknown.at("reason").get<std::string>(), "SOURCE_TIMING_UNVERIFIED");
  BOOST_CHECK(sourceUnknown.at("goodput_mbps").is_null());
}

BOOST_AUTO_TEST_CASE(ForgedAndMalformedResponsesAreRejected)
{
  for (const auto& key : {"request_id", "requester_identity", "observer_provider", "service",
                          "scope_digest", "intent_nonce", "observer_boot_epoch"}) {
    auto forged = response();
    forged[key] = "";
    BOOST_CHECK_THROW(recordFromResponse(forged, request(), 1), std::exception);
  }
  auto duplicate = response();
  duplicate["observations"][1] = duplicate["observations"][0];
  BOOST_CHECK_THROW(recordFromResponse(duplicate, request(), 1), std::exception);
  for (const auto& key : {"probe_data_wire_bytes", "probe_data_arrival_gaps_ms", "probe_source_send_gaps_ms"}) {
    auto malformed = response();
    malformed["observations"][0][key] = NativeJson::array();
    BOOST_CHECK_THROW(recordFromResponse(malformed, request(), 1), std::exception);
  }
  for (const auto& key : {"target", "peer_boot_epoch", "data_name", "profile_digest"}) {
    auto malformed = response();
    malformed["observations"][0][key] = "";
    BOOST_CHECK_THROW(recordFromResponse(malformed, request(), 1), std::exception);
  }
  auto wrongRate = response();
  wrongRate["observations"][0]["probe_train_goodput_mbps"] = 999.0;
  BOOST_CHECK_THROW(recordFromResponse(wrongRate, request(), 1), std::exception);
  auto hugePacket = response();
  hugePacket["observations"][0]["probe_data_wire_bytes"][1] = 6145;
  BOOST_CHECK_THROW(recordFromResponse(hugePacket, request(), 1), std::exception);
  auto expired = response();
  expired["observations"][0]["expires_at_ms"] = -1;
  BOOST_CHECK_THROW(recordFromResponse(expired, request(), 1), std::exception);
  expired["observations"][0]["expires_at_ms"] = 1099;
  BOOST_CHECK_THROW(recordFromResponse(expired, request(), 1), std::exception);
}

BOOST_AUTO_TEST_CASE(SummaryChecksMedianErrorAndUnknownBoundaries)
{
  const NativeJson unknown{{"status", "UNKNOWN"}, {"wire_equivalent_mbps", nullptr}};
  auto atBoundary = summarize(NativeJson::array({point(5), point(5), point(5), point(5), unknown}), 4);
  BOOST_CHECK_CLOSE(atBoundary.at("unknown_fraction").get<double>(), 0.2, 1e-9);
  BOOST_CHECK_EQUAL(atBoundary.at("median_absolute_relative_error").get<double>(), 0.25);
  BOOST_CHECK(atBoundary.at("accuracy_pass").get<bool>());
  auto tooManyUnknown = summarize(NativeJson::array({point(4), point(4), point(4), unknown, unknown}), 4);
  BOOST_CHECK(!tooManyUnknown.at("accuracy_pass").get<bool>());
  auto tooMuchError = summarize(NativeJson::array({point(5.0001)}), 4);
  BOOST_CHECK(!tooMuchError.at("accuracy_pass").get<bool>());
  auto medianRecord = summarize(NativeJson::array({point(2), point(6)}), 4);
  BOOST_CHECK_EQUAL(medianRecord.at("median_wire_equivalent_mbps").get<double>(), 4.0);
  BOOST_CHECK_EQUAL(medianRecord.at("median_absolute_relative_error").get<double>(), 0.5);
  BOOST_CHECK_THROW(summarize(NativeJson::array(), 4), std::exception);
  BOOST_CHECK_THROW(summarize(NativeJson::array({point(-1)}), 4), std::exception);
}

BOOST_AUTO_TEST_CASE(TransportSignerEvidenceMustMatchProvider)
{
  ResponseMessage reply;
  BOOST_CHECK(!hasAuthenticatedSigner(reply, "/example/probe/source"));
  reply.setAuthenticatedTransportEvidence("/reply", "/example/probe/source/KEY/k/controller/v=1", "sha256:test");
  BOOST_CHECK(hasAuthenticatedSigner(reply, "/example/probe/source"));
  BOOST_CHECK(!hasAuthenticatedSigner(reply, "/example/probe/observer"));
  reply.setAuthenticatedTransportEvidence("", "/example/probe/source/KEY/k/controller/v=1", "sha256:test");
  BOOST_CHECK(!hasAuthenticatedSigner(reply, "/example/probe/source"));
  reply.setAuthenticatedTransportEvidence("/reply", "/malformed", "sha256:test");
  BOOST_CHECK(!hasAuthenticatedSigner(reply, "/example/probe/source"));
}

BOOST_AUTO_TEST_CASE(RttSummaryIncludesUnknownTrainsWithoutChangingRateAccuracy)
{
  auto first = point(4.0);
  first["rtt_ms"] = 10.0;
  auto second = point(4.0);
  second["rtt_ms"] = 30.0;
  const NativeJson unknown{{"status", "UNKNOWN"}, {"rtt_ms", 200.0},
    {"wire_equivalent_mbps", nullptr}};
  const auto summary = summarize(NativeJson::array({first, second, unknown}), 4.0);
  BOOST_CHECK_EQUAL(summary.at("rtt_samples").get<std::size_t>(), 3);
  BOOST_CHECK_EQUAL(summary.at("median_rtt_ms").get<double>(), 30.0);
  BOOST_CHECK_EQUAL(summary.at("median_wire_equivalent_mbps").get<double>(), 4.0);
  BOOST_CHECK_EQUAL(summary.at("median_absolute_relative_error").get<double>(), 0.0);
  BOOST_CHECK_CLOSE(summary.at("unknown_fraction").get<double>(), 1.0 / 3.0, 1e-9);
  BOOST_CHECK(!summary.at("accuracy_pass").get<bool>());
  const auto absent = summarize(NativeJson::array({point(4.0)}), 4.0);
  BOOST_CHECK_EQUAL(absent.at("rtt_samples").get<std::size_t>(), 0);
  BOOST_CHECK(absent.at("median_rtt_ms").is_null());
  first["rtt_ms"] = 0;
  BOOST_CHECK_THROW(summarize(NativeJson::array({first}), 4.0), std::exception);
  first["rtt_ms"] = -1;
  BOOST_CHECK_THROW(summarize(NativeJson::array({first}), 4.0), std::exception);
}

BOOST_AUTO_TEST_CASE(RepetitionBudgetHasFiniteIntervalAndTotalDuration)
{
  BOOST_CHECK(isValidRepetitionBudget(5, 1, 15000));
  BOOST_CHECK(isValidRepetitionBudget(100, 3, 0));
  BOOST_CHECK(isValidRepetitionBudget(28, 0, 15000));
  BOOST_CHECK(isValidRepetitionBudget(95, 0, 1000));
  BOOST_CHECK(!isValidRepetitionBudget(96, 0, 1000));
  BOOST_CHECK(!isValidRepetitionBudget(29, 0, 15000));
  BOOST_CHECK(!isValidRepetitionBudget(0, 0, 0));
  BOOST_CHECK(!isValidRepetitionBudget(101, 0, 0));
  BOOST_CHECK(!isValidRepetitionBudget(1, 4, 0));
  BOOST_CHECK(!isValidRepetitionBudget(1, 0, 15001));
  BOOST_CHECK(!isValidRepetitionBudget(~0U, ~0U, ~0U));
}

BOOST_AUTO_TEST_CASE(MatrixRecomputesErrorUnknownAndRttStability)
{
  auto cells = matrix();
  BOOST_CHECK(summarizeMatrix(cells).at("qualification_pass").get<bool>());
  for (auto& record : cells[5]["native_result"]["records"])
    record["wire_equivalent_mbps"] = 5.0;
  auto result = summarizeMatrix(cells);
  BOOST_CHECK(result.at("qualification_pass").get<bool>());
  BOOST_CHECK_EQUAL(result.at("fixed_bandwidth_rate_ratio").get<double>(), 1.25);
  cells = matrix();
  for (auto& record : cells[3]["native_result"]["records"])
    record["wire_equivalent_mbps"] = 3.1;
  result = summarizeMatrix(cells);
  BOOST_CHECK(result.at("cell_accuracy_pass").get<bool>());
  BOOST_CHECK(!result.at("rtt_stability_pass").get<bool>());
  BOOST_CHECK(!result.at("qualification_pass").get<bool>());
  cells = matrix();
  auto& records = cells[0]["native_result"]["records"];
  records[0]["status"] = "UNKNOWN";
  records[0]["wire_equivalent_mbps"] = nullptr;
  BOOST_CHECK(summarizeMatrix(cells).at("qualification_pass").get<bool>());
  records[1]["status"] = "UNKNOWN";
  records[1]["wire_equivalent_mbps"] = nullptr;
  // Stored PASS fields cannot replace recomputation from the round records.
  cells[0]["native_result"]["accuracy_pass"] = true;
  BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
  cells = matrix();
  for (auto& record : cells[2]["native_result"]["records"])
    record["wire_equivalent_mbps"] = 13.0;
  BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
}

BOOST_AUTO_TEST_CASE(MatrixRejectsMissingDuplicateAndShortRepetitions)
{
  auto cells = matrix();
  cells.erase(cells.begin());
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[5] = cells[0];
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[0]["control"] = "default-burst";
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[0]["authorization_case"] = "deny";
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[0]["native_result"]["expected_bandwidth_mbps"] = 4;
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[0]["bandwidth_mbps"] = -1;
  BOOST_CHECK_THROW(summarizeMatrix(cells), std::exception);
  cells = matrix();
  cells[0]["native_result"]["records"].erase(cells[0]["native_result"]["records"].begin());
  BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
  cells = matrix();
  cells[0]["native_result"]["records"][4]["response"]["completed_at_ms"] = std::uint64_t{159'999};
  BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
  cells = matrix();
  cells[0]["native_result"]["records"][2]["response"]["completed_at_ms"] = std::uint64_t{100'000};
  BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
  for (const auto& invalid : {NativeJson(-1), NativeJson("160000"), NativeJson(nullptr)}) {
    cells = matrix();
    cells[0]["native_result"]["records"][2]["response"]["completed_at_ms"] = invalid;
    BOOST_CHECK(!summarizeMatrix(cells).at("qualification_pass").get<bool>());
  }
}

BOOST_AUTO_TEST_SUITE_END()
