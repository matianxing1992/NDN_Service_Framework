#ifndef NDN_SERVICE_FRAMEWORK_NETWORK_PROBE_RATE_HPP
#define NDN_SERVICE_FRAMEWORK_NETWORK_PROBE_RATE_HPP

#include <array>
#include <cstddef>
#include <string>

namespace ndn_service_framework {

inline constexpr std::size_t NETWORK_PROBE_PAYLOAD_BYTES = 2048;
// Canonical profile: four 2048-byte Data, pre-signed burst 0..2, signed ns
// enqueue-gap report in Data 3; 500 ms deadline and existing wire caps.
inline constexpr const char* NETWORK_PROBE_LEGACY_TRAIN_PROFILE_DIGEST =
  "sha256:d20efb4ed100fc05a0c01e72723570c1b818d49c3f0592d9be1bb9bdee95f230";

enum class NetworkProbeRateStatus
{
  Unknown,
  MeasuredGoodput,
};

struct NetworkProbeRateEstimate
{
  NetworkProbeRateStatus status = NetworkProbeRateStatus::Unknown;
  double goodputMbps = 0.0;
  std::string reason;
};

/** Estimate payload goodput of a complete authenticated four-Data train.
 * The caller must validate its profile, identities, scope and completeness.
 * Data 3 reports the actual source enqueue gaps of Data 0..2; its arrival gap
 * only proves order, since generating the report introduces extra work.
 * No cross-host clock subtraction or startup RTT enters this estimator.
 * The result is observed train goodput, not path capacity or generally
 * available bandwidth. Receiver scheduling can still limit that observation.
 */
NetworkProbeRateEstimate estimateNetworkProbeTrainRate(
  const std::array<double, 3>& arrivalGapsMs,
  const std::array<double, 2>& sourceSendGapsMs,
  std::size_t payloadBytesPerSegment,
  bool sourceTimingValidated);

inline constexpr std::size_t NETWORK_PROBE_WARMED_SEGMENTS = 16;
inline constexpr std::size_t NETWORK_PROBE_WARMED_PAYLOAD_BYTES = 4096;
inline constexpr std::size_t NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES = 5632;
inline constexpr std::size_t NETWORK_PROBE_WARMED_START = 2;
inline constexpr std::size_t NETWORK_PROBE_WARMED_MEASURED_SEGMENTS = 12;
static_assert(NETWORK_PROBE_WARMED_MEASURED_SEGMENTS > 0 &&
              NETWORK_PROBE_WARMED_MEASURED_SEGMENTS % 2 == 0);
static_assert(NETWORK_PROBE_WARMED_START + NETWORK_PROBE_WARMED_MEASURED_SEGMENTS <
              NETWORK_PROBE_WARMED_SEGMENTS - 1);
// SHA-256 of these exact UTF-8 bytes, without a newline. This profile is
// staged independently; producers must switch only with their consumers.
inline constexpr const char* NETWORK_PROBE_WARMED_PROFILE =
  "ndnsf-network-probe-warmed-train-v1;segments=16;payload=4096;warmup=2;"
  "arrival-window=2..14;halves=6+6;report=15;source-pending-ms=500;"
  "collector-ms=1000;arrival-capture=before-validation;validation=after-train;"
  "half-min-ms=0.6;half-ratio-max=2;single-gap-rest-ratio-max=2;"
  "source-half-ratio-max=0.2;content-max=5632;interest-wire-max=2048;"
  "data-wire-max=6144";
inline constexpr const char* NETWORK_PROBE_WARMED_PROFILE_DIGEST =
  "sha256:b60cf5924f3f7c78e19daed9ca5c212f2aa6a5351f49ce4b6000a75e401dbfaa";
inline constexpr const char* NETWORK_PROBE_TRAIN_PROFILE_DIGEST =
  NETWORK_PROBE_WARMED_PROFILE_DIGEST;

using NetworkProbePacketTimes = std::array<double, NETWORK_PROBE_WARMED_SEGMENTS>;
using NetworkProbeArrivalGaps = std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 1>;
using NetworkProbeSourceGaps = std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 2>;
using NetworkProbeWireSizes = std::array<std::size_t, NETWORK_PROBE_WARMED_SEGMENTS>;

/** Estimate a complete authenticated warmed train's payload goodput.
 * Data 0..1 are warmup; they do not prove burst exhaustion or steady state.
 * Data 2 is the timing origin; Data 3..14 supply twelve payloads, in two equal
 * windows. Data 15 contains the source timing report and is excluded.
 * The caller validates the warmed profile, identities and completeness.
 * Enqueue timings cannot prove NIC pacing or exclude receiver bottlenecks.
 */
NetworkProbeRateEstimate estimateWarmedNetworkProbeTrainRate(
  const std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 1>& arrivalGapsMs,
  const std::array<double, NETWORK_PROBE_WARMED_SEGMENTS - 2>& sourceSendGapsMs,
  std::size_t payloadBytesPerSegment,
  bool sourceTimingValidated);

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_NETWORK_PROBE_RATE_HPP
