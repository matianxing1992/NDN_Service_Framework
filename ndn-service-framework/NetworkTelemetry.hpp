#ifndef NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP
#define NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP

#include "common.hpp"
#include "NetworkProbeRate.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndn_service_framework {

// Controller permission and Core registration identity for targeted probes.
// An inference service carried in the probe scope is a separate context.
inline constexpr const char* NETWORK_PROBE_SERVICE = "/NDNSF/Network/Probe/v1";

enum class NetworkTelemetrySampleKind
{
  AckRtt,
  ResponseRtt,
  LargeDataFetch,
  ActiveProbeRtt,
  ActiveProbeGoodput,
};

struct NetworkTelemetryDataEdge
{
  ndn::Name srcProvider;
  ndn::Name dstProvider;

  bool operator<(const NetworkTelemetryDataEdge& other) const;
};

struct NetworkTelemetrySample
{
  NetworkTelemetrySampleKind kind = NetworkTelemetrySampleKind::AckRtt;
  double rttMs = 0.0;
  double firstByteMs = 0.0;
  double elapsedMs = 0.0;
  size_t encodedBytes = 0;
  size_t payloadBytes = 0;
  size_t wireBytes = 0;
  size_t receivedSegments = 0;
  size_t probeSegmentsSent = 0;
  NetworkProbePacketTimes probeRttMs{};
  NetworkProbeArrivalGaps probeInterestGapsMs{};
  NetworkProbeArrivalGaps probeDataArrivalGapsMs{};
  NetworkProbeWireSizes probeDataWireBytes{};
  NetworkProbeSourceGaps probeSourceSendGapsMs{};
  bool probeSourceTimingValidated = false;
  std::string probeProfileDigest;
  bool probeComplete = false;
  bool targetSignatureValidated = false;
  // Whether the observed chirp met the timing profile used by the estimator.
  bool probeSignatureValid = false;
  size_t timeoutCount = 0;
  size_t nackCount = 0;
  std::string peerBootEpoch;
  ndn::Name dataName;
};

struct NetworkTelemetrySnapshot
{
  ndn::Name providerName;
  ndn::Name serviceName;
  ndn::Name peerName;
  std::string edgeName;
  NetworkTelemetrySampleKind kind = NetworkTelemetrySampleKind::AckRtt;
  double rttMs = 0.0;
  double firstByteMs = 0.0;
  double elapsedMs = 0.0;
  size_t encodedBytes = 0;
  size_t payloadBytes = 0;
  size_t wireBytes = 0;
  double goodputMbps = 0.0;
  size_t receivedSegments = 0;
  size_t probeSegmentsSent = 0;
  NetworkProbePacketTimes probeRttMs{};
  NetworkProbeArrivalGaps probeInterestGapsMs{};
  NetworkProbeArrivalGaps probeDataArrivalGapsMs{};
  NetworkProbeWireSizes probeDataWireBytes{};
  NetworkProbeSourceGaps probeSourceSendGapsMs{};
  bool probeSourceTimingValidated = false;
  std::string probeProfileDigest;
  bool probeComplete = false;
  bool targetSignatureValidated = false;
  bool probeSignatureValid = false;
  size_t timeoutCount = 0;
  size_t nackCount = 0;
  size_t sampleCount = 0;
  uint64_t lastUpdatedMs = 0;
  double confidence = 0.0;
  bool stale = false;
  std::string peerBootEpoch;
  ndn::Name dataName;
};

struct NetworkTelemetryKey
{
  ndn::Name providerName;
  ndn::Name serviceName;
  ndn::Name peerName;
  std::string edgeName;
  NetworkTelemetrySampleKind kind = NetworkTelemetrySampleKind::AckRtt;

  bool operator<(const NetworkTelemetryKey& other) const;
};

double networkTelemetryGoodputMbps(size_t payloadBytes, double elapsedMs);
const char* toString(NetworkTelemetrySampleKind kind);

class NetworkTelemetryStore
{
public:
  /** In-memory sample store; expired rows are pruned on update and the
   * oldest row is evicted when maxEntries is reached. */
  explicit NetworkTelemetryStore(
    double alpha = 0.25,
    std::chrono::milliseconds ttl = std::chrono::seconds(30),
    size_t maxEntries = 1024);

  void update(const NetworkTelemetryKey& key,
              const NetworkTelemetrySample& sample);

  std::optional<NetworkTelemetrySnapshot>
  get(const NetworkTelemetryKey& key) const;

  void updateAckRtt(const ndn::Name& providerName,
                    const ndn::Name& serviceName,
                    double rttMs);

  void updateActiveProbe(const ndn::Name& observerProvider,
                         const ndn::Name& targetProvider,
                         const std::string& requestScopeDigest,
                         const std::string& targetBootEpoch,
                         const ndn::Name& validatedDataName,
                         double rttMs);

  void updateActiveProbeGoodput(const ndn::Name& observerProvider,
                                const ndn::Name& sourceProvider,
                                const std::string& requestScopeDigest,
                                const std::string& targetBootEpoch,
                                const ndn::Name& validatedDataName,
                                size_t payloadBytes,
                                size_t wireBytes,
                                size_t receivedSegments,
                                double firstByteMs,
                                double elapsedMs,
                                const std::string& probeProfileDigest,
                                bool probeSignatureValid,
                                const NetworkProbePacketTimes& probeRttMs,
                                const NetworkProbeArrivalGaps& probeInterestGapsMs,
                                const NetworkProbeArrivalGaps& probeDataArrivalGapsMs,
                                const NetworkProbeWireSizes& probeDataWireBytes,
                                const NetworkProbeSourceGaps& probeSourceSendGapsMs = {},
                                bool probeSourceTimingValidated = false);

  void updateLargeDataFetch(const ndn::Name& consumerProvider,
                            const ndn::Name& producerProvider,
                            const std::string& keyScope,
                            const ndn::Name& dataName,
                            double elapsedMs,
                            double firstByteMs,
                            size_t encodedBytes,
                            size_t payloadBytes,
                            size_t wireBytes,
                            size_t receivedSegments,
                            size_t timeoutCount,
                            size_t nackCount);

  std::optional<NetworkTelemetrySnapshot>
  getServicePath(const ndn::Name& providerName,
                 const ndn::Name& serviceName) const;

  std::optional<NetworkTelemetrySnapshot>
  getActiveProbe(const ndn::Name& observerProvider,
                 const ndn::Name& targetProvider,
                 const std::string& requestScopeDigest) const;

  std::optional<NetworkTelemetrySnapshot>
  getActiveProbeGoodput(const ndn::Name& observerProvider,
                        const ndn::Name& sourceProvider,
                        const std::string& requestScopeDigest) const;

  /** Reuse a measured fact, never an old request's authorization. The caller
   * must confirm the peer epoch with fresh signed readiness and authorize the
   * current edge. Returns an owning copy with its original time and Data name. */
  std::optional<NetworkTelemetrySnapshot>
  getReusableActiveProbe(const ndn::Name& observerProvider,
                        const ndn::Name& targetProvider,
                        NetworkTelemetrySampleKind kind,
                        const std::string& confirmedPeerBootEpoch) const;

  /** Record a roster only after its request has been authenticated. A scope
   * cannot be rebound to a different roster or have its original expiry
   * extended by replay. */
  bool authorizeActiveProbeRequest(
    const std::string& requestScopeDigest,
    const std::vector<ndn::Name>& candidateProviders,
    const std::vector<NetworkTelemetryDataEdge>& allowedRttPairs,
    const std::vector<NetworkTelemetryDataEdge>& allowedGoodputEdges,
    std::chrono::steady_clock::time_point expiresAt,
    std::shared_ptr<std::atomic<bool>> registrationCancelled = {});

  /** Original authorization expiry, absent when expired or registration closed. */
  std::optional<std::chrono::steady_clock::time_point>
  activeProbeScopeDeadline(const std::string& requestScopeDigest) const;

  /** Close only this Provider's outbound collection window. The authenticated
   * roster remains available to answer already-issued peer probes until its
   * original expiry, and a closed scope cannot be replayed to reset claims. */
  bool closeActiveProbeCollection(const std::string& requestScopeDigest);
  bool isActiveProbeCollectionOpen(
    const std::string& requestScopeDigest) const;

  /** Both endpoints must belong to the same still-live authenticated roster. */
  bool isActiveProbeAuthorized(
    const std::string& requestScopeDigest,
    const ndn::Name& observerProvider,
    const ndn::Name& targetProvider) const;

  /** Atomically reserve one observer-to-target RTT probe per request scope.
   * Retries and concurrent collectors cannot issue duplicate RTT Interests. */
  bool claimActiveProbeRttPair(
    const std::string& requestScopeDigest,
    const ndn::Name& observerProvider,
    const ndn::Name& targetProvider);

  bool isActiveProbeGoodputAuthorized(
    const std::string& requestScopeDigest,
    const ndn::Name& sourceProvider,
    const ndn::Name& destinationProvider) const;

  /** Atomically reserve this directed edge's single goodput train for the
   * request scope. Each authorized edge can be claimed only once, so retries
   * and concurrent collection calls cannot exceed the request-wide bound. */
  bool claimActiveProbeGoodputEdge(
    const std::string& requestScopeDigest,
    const ndn::Name& sourceProvider,
    const ndn::Name& destinationProvider);

  /** Admit one distinct segment from the single authorized receiver-side
   * train for this edge and scope. Duplicate segments and later run nonces
   * are rejected. */
  bool acceptActiveProbeGoodputSegment(
    const std::string& requestScopeDigest,
    const ndn::Name& sourceProvider,
    const ndn::Name& destinationProvider,
    const std::string& runNonce,
    size_t segment,
    size_t segmentCount);

  std::optional<NetworkTelemetrySnapshot>
  getDependencyEdge(const ndn::Name& consumerProvider,
                    const ndn::Name& producerProvider,
                    const std::string& keyScope) const;

  size_t size() const;

  /** Passive copies of recent validated dedicated-probe samples. Historical
   * peer boot IDs are preserved; the consumer checks its current peer roster.
   * The second value is the original validity bound, never a refreshed TTL.
   */
  std::vector<std::pair<NetworkTelemetrySnapshot, uint64_t>> getCachedActiveProbeSamples(
    const ndn::Name& observerProvider, const std::vector<ndn::Name>& allowedPeers) const;

private:
  friend struct NetworkTelemetryStoreTestAccess;
  bool isReusableActiveProbeSample(const NetworkTelemetrySnapshot& sample, uint64_t now) const;
  NetworkTelemetrySnapshot makeSnapshot(const NetworkTelemetryKey& key,
                                        const NetworkTelemetrySample& sample) const;
  void refreshDerivedFields(NetworkTelemetrySnapshot& snapshot) const;
  void pruneExpired(uint64_t nowMs);
  double smooth(double oldValue, double newValue) const;
  static uint64_t nowMs();
  void pruneActiveProbeAuthorizations(
    std::chrono::steady_clock::time_point now);

  struct ActiveProbeAuthorization
  {
    struct GoodputTrain
    {
      std::string runNonce;
      uint32_t acceptedSegmentMask = 0;
    };

    std::vector<ndn::Name> candidateProviders;
    // RTT pair srcProvider is the authenticated observer, dstProvider the
    // requested target. Roster membership alone does not authorize a probe.
    std::vector<NetworkTelemetryDataEdge> allowedRttPairs;
    std::vector<NetworkTelemetryDataEdge> allowedGoodputEdges;
    std::vector<NetworkTelemetryDataEdge> claimedRttPairs;
    std::vector<NetworkTelemetryDataEdge> claimedGoodputEdges;
    std::map<NetworkTelemetryDataEdge, GoodputTrain> receivedGoodputTrains;
    std::chrono::steady_clock::time_point expiresAt;
    std::shared_ptr<std::atomic<bool>> registrationCancelled;
    bool collectionClosed = false;
  };

private:
  static bool authorizationCancelled(const ActiveProbeAuthorization& authorization)
  {
    return authorization.registrationCancelled &&
           authorization.registrationCancelled->load(std::memory_order_relaxed);
  }

  double m_alpha = 0.25;
  std::chrono::milliseconds m_ttl;
  size_t m_maxEntries = 1024;
  mutable std::mutex m_mutex;
  std::map<NetworkTelemetryKey, NetworkTelemetrySnapshot> m_snapshots;
  mutable std::mutex m_activeProbeAuthorizationMutex;
  std::map<std::string, ActiveProbeAuthorization> m_activeProbeAuthorizations;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP
