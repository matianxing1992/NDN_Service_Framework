#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "tests/boost-test.hpp"

#include <limits>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(Spec197NetworkProbeRate)

BOOST_AUTO_TEST_CASE(TrainGoodputExcludesReportDelay)
{
  const auto fastReport = estimateNetworkProbeTrainRate({4.0, 4.0, 0.01},
                                                       {0.01, 0.01}, 2048, true);
  const auto slowReport = estimateNetworkProbeTrainRate({4.0, 4.0, 200.0},
                                                       {0.01, 0.01}, 2048, true);
  BOOST_CHECK(fastReport.status == NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK(slowReport.status == NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK_CLOSE(fastReport.goodputMbps, 4.096, 0.000001);
  BOOST_CHECK_EQUAL(slowReport.goodputMbps, fastReport.goodputMbps);
}

BOOST_AUTO_TEST_CASE(ResolutionDispersionAndSourceBoundaries)
{
  BOOST_CHECK(estimateNetworkProbeTrainRate({0.2, 0.4, 1.0}, {0.06, 0.06},
                                           2048, true).status ==
              NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK_EQUAL(estimateNetworkProbeTrainRate({0.199, 0.4, 1.0}, {0.0, 0.0},
                                                 2048, true).reason,
                    "CLOCK_RESOLUTION");
  BOOST_CHECK_EQUAL(estimateNetworkProbeTrainRate({1.0, 2.001, 1.0}, {0.01, 0.01},
                                                 2048, true).reason,
                    "ARRIVAL_DISPERSION_UNSTABLE");
  BOOST_CHECK_EQUAL(estimateNetworkProbeTrainRate({1.0, 1.0, 1.0}, {0.201, 0.2},
                                                 2048, true).reason,
                    "SOURCE_LIMITED");
}

BOOST_AUTO_TEST_CASE(UntrustedMalformedAndReorderedInputsAreUnknown)
{
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto inf = std::numeric_limits<double>::infinity();
  BOOST_CHECK_EQUAL(estimateNetworkProbeTrainRate({1.0, 1.0, 1.0}, {0.0, 0.0},
                                                 2048, false).reason,
                    "SOURCE_TIMING_UNVERIFIED");
  BOOST_CHECK_EQUAL(estimateNetworkProbeTrainRate({1.0, 1.0, 1.0}, {0.0, 0.0},
                                                 2047, true).reason,
                    "INVALID_PAYLOAD_SIZE");
  for (const auto value : {nan, inf, 0.0, -1.0}) {
    for (std::size_t index = 0; index < 3; ++index) {
      std::array<double, 3> gaps{{1.0, 1.0, 1.0}};
      gaps[index] = value;
      BOOST_CHECK(estimateNetworkProbeTrainRate(gaps, {0.0, 0.0}, 2048, true).status ==
                  NetworkProbeRateStatus::Unknown);
    }
  }
  for (const auto value : {nan, inf, -1.0, 500.001}) {
    BOOST_CHECK(estimateNetworkProbeTrainRate({1.0, 1.0, 1.0}, {value, 0.0},
                                             2048, true).status ==
                NetworkProbeRateStatus::Unknown);
    BOOST_CHECK(estimateNetworkProbeTrainRate({1.0, 1.0, 1.0}, {0.0, value},
                                             2048, true).status ==
                NetworkProbeRateStatus::Unknown);
  }
}

BOOST_AUTO_TEST_CASE(WarmedWindowExcludesStartupAndReport)
{
  std::array<double, 15> arrival;
  std::array<double, 14> source;
  arrival.fill(4.0);
  source.fill(0.01);
  const auto estimate = estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
  BOOST_REQUIRE(estimate.status == NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK_CLOSE(estimate.goodputMbps, 8.192, 0.000001);
  for (std::size_t i = 0; i < NETWORK_PROBE_WARMED_START; ++i) {
    arrival[i] = 100.0;
    source[i] = 500.0;
  }
  arrival.back() = 200.0;
  const auto changed = estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
  BOOST_CHECK(changed.status == NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK_EQUAL(changed.goodputMbps, estimate.goodputMbps);
  BOOST_CHECK_EQUAL(NETWORK_PROBE_WARMED_START, 2);
  BOOST_CHECK_EQUAL(NETWORK_PROBE_WARMED_MEASURED_SEGMENTS, 12);
}

BOOST_AUTO_TEST_CASE(WarmedWindowsAggregateCoalescedGapsAndRejectDrift)
{
  std::array<double, 15> arrival;
  std::array<double, 14> source{};
  arrival.fill(1.0);
  // Equal windows dominated by one pause do not establish a stable rate.
  for (std::size_t i = 2; i < 14; ++i)
    arrival[i] = 0.01;
  arrival[7] = arrival[13] = 23.95;
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source,
    NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason, "ARRIVAL_GAP_DOMINATED");
  // Coalescing across two gaps remains measurable when neither dominates.
  arrival[6] = arrival[7] = arrival[12] = arrival[13] = 11.98;
  const auto measured = estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
  BOOST_REQUIRE(measured.status == NetworkProbeRateStatus::MeasuredGoodput);
  BOOST_CHECK_CLOSE(measured.goodputMbps, 8.192, 0.000001);
  arrival.fill(4.0);
  for (std::size_t i = 8; i < 14; ++i)
    arrival[i] = 8.0;
  BOOST_CHECK(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).status ==
              NetworkProbeRateStatus::MeasuredGoodput);
  arrival[13] = 8.01;
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                    "ARRIVAL_DISPERSION_UNSTABLE");
  arrival.fill(0.1);
  BOOST_CHECK(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).status ==
              NetworkProbeRateStatus::MeasuredGoodput);
  arrival[2] = 0.099;
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                    "CLOCK_RESOLUTION");
  arrival.fill(1.0);
  source[2] = 1.2;
  BOOST_CHECK(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).status ==
              NetworkProbeRateStatus::MeasuredGoodput);
  source[2] = 1.201;
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                    "SOURCE_LIMITED");
  source[2] = 0;
  source[8] = 1.201;
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                    "SOURCE_LIMITED");
}

BOOST_AUTO_TEST_CASE(WarmedSingleGapDominanceBoundaryInEveryMeasuredPosition)
{
  NetworkProbeArrivalGaps arrival;
  NetworkProbeSourceGaps source{};
  for (std::size_t index = NETWORK_PROBE_WARMED_START;
       index < NETWORK_PROBE_WARMED_START + NETWORK_PROBE_WARMED_MEASURED_SEGMENTS;
       ++index) {
    arrival.fill(1.0);
    const auto otherHalfStart = index < NETWORK_PROBE_WARMED_START + 6
      ? NETWORK_PROBE_WARMED_START + 6 : NETWORK_PROBE_WARMED_START;
    for (std::size_t offset = 0; offset < 6; ++offset)
      arrival[otherHalfStart + offset] = 2.5;
    arrival[index] = 10.0; // Exactly twice the other five gaps: accepted.
    BOOST_CHECK(estimateWarmedNetworkProbeTrainRate(arrival, source,
      NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).status == NetworkProbeRateStatus::MeasuredGoodput);
    arrival[index] = 10.001;
    const auto rejected = estimateWarmedNetworkProbeTrainRate(arrival, source,
      NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true);
    BOOST_CHECK_EQUAL(rejected.reason, "ARRIVAL_GAP_DOMINATED");
    BOOST_CHECK(rejected.status == NetworkProbeRateStatus::Unknown);
    BOOST_CHECK_EQUAL(rejected.goodputMbps, 0.0);
  }
}

BOOST_AUTO_TEST_CASE(WarmedMalformedInputsAreUnknownInEveryPosition)
{
  std::array<double, 15> arrival;
  std::array<double, 14> source{};
  arrival.fill(1.0);
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, false).reason,
                    "SOURCE_TIMING_UNVERIFIED");
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_PAYLOAD_BYTES, true).reason,
                    "INVALID_PAYLOAD_SIZE");
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto inf = std::numeric_limits<double>::infinity();
  for (const auto value : {nan, inf, 0.0, -1.0}) {
    for (std::size_t i = 0; i < arrival.size(); ++i) {
      auto invalid = arrival;
      invalid[i] = value;
      BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(invalid, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                        "INVALID_ARRIVAL_GAP");
    }
  }
  for (const auto value : {nan, inf, -1.0, 500.001}) {
    for (std::size_t i = 0; i < source.size(); ++i) {
      auto invalid = source;
      invalid[i] = value;
      BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, invalid, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                        "INVALID_SOURCE_GAP");
    }
  }
  arrival.fill(std::numeric_limits<double>::max());
  BOOST_CHECK_EQUAL(estimateWarmedNetworkProbeTrainRate(arrival, source, NETWORK_PROBE_WARMED_PAYLOAD_BYTES, true).reason,
                    "INVALID_ARRIVAL_GAP");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
