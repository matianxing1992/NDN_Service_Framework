#include "NetworkProbeRate.hpp"

#include <algorithm>
#include <cmath>

namespace ndn_service_framework {

NetworkProbeRateEstimate
estimateNetworkProbeTrainRate(const std::array<double, 3>& arrivalGapsMs,
                             const std::array<double, 2>& sourceSendGapsMs,
                             std::size_t payloadBytesPerSegment,
                             bool sourceTimingValidated)
{
  const auto unknown = [] (const char* reason) {
    return NetworkProbeRateEstimate{NetworkProbeRateStatus::Unknown, 0.0, reason};
  };
  if (!sourceTimingValidated)
    return unknown("SOURCE_TIMING_UNVERIFIED");
  if (payloadBytesPerSegment != NETWORK_PROBE_PAYLOAD_BYTES)
    return unknown("INVALID_PAYLOAD_SIZE");
  for (const auto gap : arrivalGapsMs) {
    if (!std::isfinite(gap) || gap <= 0.0)
      return unknown("INVALID_ARRIVAL_GAP");
  }
  for (const auto gap : sourceSendGapsMs) {
    if (!std::isfinite(gap) || gap < 0.0 || gap > 500.0)
      return unknown("INVALID_SOURCE_GAP");
  }
  const auto shortest = std::min(arrivalGapsMs[0], arrivalGapsMs[1]);
  const auto longest = std::max(arrivalGapsMs[0], arrivalGapsMs[1]);
  if (shortest < 0.2)
    return unknown("CLOCK_RESOLUTION");
  if (longest > shortest * 2.0)
    return unknown("ARRIVAL_DISPERSION_UNSTABLE");

  const auto arrivalSpan = arrivalGapsMs[0] + arrivalGapsMs[1];
  const auto sourceSpan = sourceSendGapsMs[0] + sourceSendGapsMs[1];
  if (!std::isfinite(arrivalSpan))
    return unknown("INVALID_ARRIVAL_GAP");
  if (sourceSpan > arrivalSpan * 0.2)
    return unknown("SOURCE_LIMITED");
  const auto rate = (2.0 * static_cast<double>(payloadBytesPerSegment) * 8.0) /
                    (arrivalSpan * 1000.0);
  if (!std::isfinite(rate) || rate <= 0.0)
    return unknown("INVALID_RATE");
  return {NetworkProbeRateStatus::MeasuredGoodput, rate, "MEASURED_PACKET_TRAIN"};
}

NetworkProbeRateEstimate
estimateWarmedNetworkProbeTrainRate(
  const std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 1>& arrivalGapsMs,
  const std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 2>& sourceSendGapsMs,
  std::size_t payloadBytesPerSegment,
  bool sourceTimingValidated)
{
  const auto unknown = [] (const char* reason) {
    return NetworkProbeRateEstimate{NetworkProbeRateStatus::Unknown, 0.0, reason};
  };
  if (!sourceTimingValidated)
    return unknown("SOURCE_TIMING_UNVERIFIED");
  if (payloadBytesPerSegment != NETWORK_PROBE_WARMED_PAYLOAD_BYTES)
    return unknown("INVALID_PAYLOAD_SIZE");
  for (const auto gap : arrivalGapsMs) {
    if (!std::isfinite(gap) || gap <= 0.0)
      return unknown("INVALID_ARRIVAL_GAP");
  }
  for (const auto gap : sourceSendGapsMs) {
    if (!std::isfinite(gap) || gap < 0.0 || gap > 500.0)
      return unknown("INVALID_SOURCE_GAP");
  }

  std::array<double, 2> arrivalSpans{};
  std::array<double, 2> sourceSpans{};
  constexpr auto halfSegments = NETWORK_PROBE_WARMED_MEASURED_SEGMENTS / 2;
  for (std::size_t half = 0; half < arrivalSpans.size(); ++half) {
    double largestGap = 0.0;
    for (std::size_t offset = 0; offset < halfSegments; ++offset) {
      const auto index = NETWORK_PROBE_WARMED_START + half * halfSegments + offset;
      arrivalSpans[half] += arrivalGapsMs[index];
      sourceSpans[half] += sourceSendGapsMs[index];
      largestGap = std::max(largestGap, arrivalGapsMs[index]);
    }
    if (!std::isfinite(arrivalSpans[half]))
      return unknown("INVALID_ARRIVAL_GAP");
    if (arrivalSpans[half] < 0.6)
      return unknown("CLOCK_RESOLUTION");
    // Similar half durations can each hide one long scheduling pause.
    // Such a train cannot establish a representative transfer rate.
    if (largestGap > (arrivalSpans[half] - largestGap) * 2.0)
      return unknown("ARRIVAL_GAP_DOMINATED");
  }
  const auto shortest = std::min(arrivalSpans[0], arrivalSpans[1]);
  const auto longest = std::max(arrivalSpans[0], arrivalSpans[1]);
  if (longest > shortest * 2.0)
    return unknown("ARRIVAL_DISPERSION_UNSTABLE");
  for (std::size_t half = 0; half < arrivalSpans.size(); ++half) {
    if (sourceSpans[half] > arrivalSpans[half] * 0.2)
      return unknown("SOURCE_LIMITED");
  }
  const auto totalSpan = arrivalSpans[0] + arrivalSpans[1];
  if (!std::isfinite(totalSpan))
    return unknown("INVALID_ARRIVAL_GAP");
  const auto rate = (static_cast<double>(NETWORK_PROBE_WARMED_MEASURED_SEGMENTS) *
                     static_cast<double>(payloadBytesPerSegment) * 8.0) /
                    (totalSpan * 1000.0);
  if (!std::isfinite(rate) || rate <= 0.0)
    return unknown("INVALID_RATE");
  return {NetworkProbeRateStatus::MeasuredGoodput, rate, "MEASURED_WARMED_PACKET_TRAIN"};
}

} // namespace ndn_service_framework
