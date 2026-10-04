#include "NetworkTelemetry.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace ndn_service_framework {

bool
NetworkTelemetryKey::operator<(const NetworkTelemetryKey& other) const
{
  return std::tie(providerName, serviceName, peerName, edgeName, kind) <
         std::tie(other.providerName, other.serviceName, other.peerName,
                  other.edgeName, other.kind);
}

bool
NetworkTelemetryDataEdge::operator<(const NetworkTelemetryDataEdge& other) const
{
  return std::tie(srcProvider, dstProvider) <
         std::tie(other.srcProvider, other.dstProvider);
}

double
networkTelemetryGoodputMbps(size_t payloadBytes, double elapsedMs)
{
  if (payloadBytes == 0 || !std::isfinite(elapsedMs) || elapsedMs <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(payloadBytes) * 8.0 / (elapsedMs * 1000.0);
}

const char*
toString(NetworkTelemetrySampleKind kind)
{
  switch (kind) {
  case NetworkTelemetrySampleKind::AckRtt:
    return "ack-rtt";
  case NetworkTelemetrySampleKind::ResponseRtt:
    return "response-rtt";
  case NetworkTelemetrySampleKind::LargeDataFetch:
    return "large-data-fetch";
  case NetworkTelemetrySampleKind::ActiveProbeRtt:
    return "active-probe-rtt";
  case NetworkTelemetrySampleKind::ActiveProbeGoodput:
    return "active-probe-goodput";
  }
  return "unknown";
}

NetworkTelemetryStore::NetworkTelemetryStore(double alpha,
                                             std::chrono::milliseconds ttl,
                                             size_t maxEntries)
  : m_alpha(std::min(1.0, std::max(0.0, alpha)))
  , m_ttl(ttl)
  , m_maxEntries(std::max<size_t>(1, maxEntries))
{
}

void
NetworkTelemetryStore::update(const NetworkTelemetryKey& key,
                              const NetworkTelemetrySample& sample)
{
  if (!std::isfinite(sample.rttMs) || sample.rttMs < 0.0 ||
      !std::isfinite(sample.firstByteMs) || sample.firstByteMs < 0.0 ||
      !std::isfinite(sample.elapsedMs) || sample.elapsedMs < 0.0) {
    return;
  }
  if (sample.kind == NetworkTelemetrySampleKind::ActiveProbeRtt &&
      (sample.rttMs == 0.0 || key.providerName.empty() ||
       key.peerName.empty() || key.providerName == key.peerName ||
       key.edgeName.empty() || key.edgeName.size() > 128 ||
       sample.peerBootEpoch.empty() || sample.peerBootEpoch.size() > 128 ||
       !sample.targetSignatureValidated || !sample.probeSignatureValid ||
       sample.dataName.empty() ||
       sample.dataName.toUri().size() > 512)) {
    return;
  }
  if (sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput &&
      sample.probeSourceTimingValidated &&
      std::any_of(sample.probeSourceSendGapsMs.begin(),
                  sample.probeSourceSendGapsMs.end(),
                  [] (double value) {
                    return !std::isfinite(value) || value < 0.0 || value > 500.0;
                  })) {
    return;
  }
  if (sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput &&
      (key.providerName.empty() || key.peerName.empty() ||
       key.providerName == key.peerName || key.edgeName.empty() ||
       key.edgeName.size() > 128 || sample.peerBootEpoch.empty() ||
       sample.peerBootEpoch.size() > 128 || sample.payloadBytes == 0 ||
       sample.wireBytes < sample.payloadBytes || sample.elapsedMs <= 0.0 ||
       sample.firstByteMs < 0.0 || sample.firstByteMs > sample.elapsedMs ||
        sample.receivedSegments != NETWORK_PROBE_WARMED_SEGMENTS ||
       sample.probeSegmentsSent != sample.receivedSegments ||
       !sample.probeComplete || sample.probeProfileDigest.size() != 71 ||
       sample.probeProfileDigest.compare(0, 7, "sha256:") != 0 ||
       (sample.probeSignatureValid &&
        (std::any_of(sample.probeRttMs.begin(), sample.probeRttMs.end(),
                   [] (double value) { return !std::isfinite(value) || value <= 0.0; }) ||
         std::any_of(sample.probeInterestGapsMs.begin(), sample.probeInterestGapsMs.end(),
                   [] (double value) { return !std::isfinite(value) || value <= 0.0; }) ||
         std::any_of(sample.probeDataArrivalGapsMs.begin(),
                     sample.probeDataArrivalGapsMs.end(),
                     [] (double value) { return !std::isfinite(value) || value <= 0.0; }) ||
         std::any_of(sample.probeDataWireBytes.begin(), sample.probeDataWireBytes.end(),
                     [] (size_t value) { return value == 0; }))))) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  pruneExpired(nowMs());

  auto typedKey = key;
  typedKey.kind = sample.kind;
  auto it = m_snapshots.find(typedKey);
  if (it == m_snapshots.end()) {
    if (m_snapshots.size() >= m_maxEntries) {
      const auto oldest = std::min_element(
        m_snapshots.begin(), m_snapshots.end(),
        [] (const auto& lhs, const auto& rhs) {
          return lhs.second.lastUpdatedMs < rhs.second.lastUpdatedMs;
        });
      if (oldest != m_snapshots.end()) {
        m_snapshots.erase(oldest);
      }
    }
    m_snapshots.emplace(typedKey, makeSnapshot(typedKey, sample));
    return;
  }

  auto& snapshot = it->second;
  if ((sample.kind == NetworkTelemetrySampleKind::ActiveProbeRtt ||
       sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput) &&
      snapshot.peerBootEpoch != sample.peerBootEpoch) {
    snapshot = makeSnapshot(typedKey, sample);
    return;
  }
  snapshot.kind = sample.kind;
  snapshot.peerBootEpoch = sample.peerBootEpoch;
  snapshot.dataName = sample.dataName;
  if (sample.rttMs > 0.0) {
    snapshot.rttMs = smooth(snapshot.rttMs, sample.rttMs);
  }
  if (sample.firstByteMs > 0.0) {
    snapshot.firstByteMs = smooth(snapshot.firstByteMs, sample.firstByteMs);
  }
  if (sample.elapsedMs > 0.0) {
    snapshot.elapsedMs = smooth(snapshot.elapsedMs, sample.elapsedMs);
  }
  if (sample.encodedBytes > 0) {
    snapshot.encodedBytes = static_cast<size_t>(
      std::llround(smooth(static_cast<double>(snapshot.encodedBytes),
                          static_cast<double>(sample.encodedBytes))));
  }
  if (sample.payloadBytes > 0) {
    snapshot.payloadBytes = static_cast<size_t>(
      std::llround(smooth(static_cast<double>(snapshot.payloadBytes),
                          static_cast<double>(sample.payloadBytes))));
  }
  if (sample.wireBytes > 0) {
    snapshot.wireBytes = static_cast<size_t>(
      std::llround(smooth(static_cast<double>(snapshot.wireBytes),
                          static_cast<double>(sample.wireBytes))));
  }
  if (sample.receivedSegments > 0) {
    snapshot.receivedSegments = static_cast<size_t>(
      std::llround(smooth(static_cast<double>(snapshot.receivedSegments),
                          static_cast<double>(sample.receivedSegments))));
  }
  if (sample.probeSegmentsSent > 0) {
    snapshot.probeSegmentsSent = sample.probeSegmentsSent;
  }
  if (!sample.probeProfileDigest.empty()) {
    snapshot.probeProfileDigest = sample.probeProfileDigest;
  }
  if (sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    snapshot.probeRttMs = sample.probeRttMs;
    snapshot.probeInterestGapsMs = sample.probeInterestGapsMs;
    snapshot.probeDataArrivalGapsMs = sample.probeDataArrivalGapsMs;
    snapshot.probeDataWireBytes = sample.probeDataWireBytes;
    snapshot.probeSourceSendGapsMs = sample.probeSourceSendGapsMs;
    snapshot.probeSourceTimingValidated = sample.probeSourceTimingValidated;
    snapshot.probeSignatureValid = sample.probeSignatureValid;
  }
  if (sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    snapshot.probeComplete = sample.probeComplete;
  }
  snapshot.timeoutCount += sample.timeoutCount;
  snapshot.nackCount += sample.nackCount;
  ++snapshot.sampleCount;
  snapshot.lastUpdatedMs = nowMs();
  refreshDerivedFields(snapshot);
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::get(const NetworkTelemetryKey& key) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_snapshots.find(key);
  if (it == m_snapshots.end()) {
    return std::nullopt;
  }
  auto snapshot = it->second;
  refreshDerivedFields(snapshot);
  return snapshot;
}

void
NetworkTelemetryStore::updateAckRtt(const ndn::Name& providerName,
                                    const ndn::Name& serviceName,
                                    double rttMs)
{
  NetworkTelemetrySample sample;
  sample.kind = NetworkTelemetrySampleKind::AckRtt;
  sample.rttMs = rttMs;
  sample.elapsedMs = rttMs;
  update(NetworkTelemetryKey{providerName, serviceName, ndn::Name(), ""},
         sample);
}

void
NetworkTelemetryStore::updateActiveProbe(const ndn::Name& observerProvider,
                                         const ndn::Name& targetProvider,
                                         const std::string& requestScopeDigest,
                                         const std::string& targetBootEpoch,
                                         const ndn::Name& validatedDataName,
                                         double rttMs)
{
  NetworkTelemetrySample sample;
  sample.kind = NetworkTelemetrySampleKind::ActiveProbeRtt;
  sample.rttMs = rttMs;
  sample.elapsedMs = rttMs;
  sample.peerBootEpoch = targetBootEpoch;
  sample.dataName = validatedDataName;
  sample.targetSignatureValidated = true;
  sample.probeSignatureValid = true;
  update(NetworkTelemetryKey{observerProvider, ndn::Name(), targetProvider,
                             requestScopeDigest}, sample);
}

void
NetworkTelemetryStore::updateActiveProbeGoodput(
  const ndn::Name& observerProvider,
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
  const NetworkProbeSourceGaps& probeSourceSendGapsMs,
  bool probeSourceTimingValidated)
{
  NetworkTelemetrySample sample;
  sample.kind = NetworkTelemetrySampleKind::ActiveProbeGoodput;
  sample.firstByteMs = firstByteMs;
  sample.elapsedMs = elapsedMs;
  sample.payloadBytes = payloadBytes;
  sample.wireBytes = wireBytes;
  sample.receivedSegments = receivedSegments;
  sample.probeSegmentsSent = receivedSegments;
  sample.probeProfileDigest = probeProfileDigest;
  sample.probeRttMs = probeRttMs;
  sample.probeInterestGapsMs = probeInterestGapsMs;
  sample.probeDataArrivalGapsMs = probeDataArrivalGapsMs;
  sample.probeDataWireBytes = probeDataWireBytes;
  sample.probeSourceSendGapsMs = probeSourceSendGapsMs;
  sample.probeSourceTimingValidated = probeSourceTimingValidated;
  sample.probeComplete = true;
  // This writer is called only after the complete train passed NacValidator,
  // exact-name, and target-identity checks. The profile bit below is separate.
  sample.targetSignatureValidated = true;
  sample.probeSignatureValid = probeSignatureValid;
  sample.peerBootEpoch = targetBootEpoch;
  sample.dataName = validatedDataName;
  update(NetworkTelemetryKey{observerProvider, ndn::Name(), sourceProvider,
                             requestScopeDigest}, sample);
}

void
NetworkTelemetryStore::updateLargeDataFetch(const ndn::Name& consumerProvider,
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
                                            size_t nackCount)
{
  NetworkTelemetrySample sample;
  sample.kind = NetworkTelemetrySampleKind::LargeDataFetch;
  sample.firstByteMs = firstByteMs;
  sample.elapsedMs = elapsedMs;
  sample.encodedBytes = encodedBytes;
  sample.payloadBytes = payloadBytes;
  sample.wireBytes = wireBytes;
  sample.receivedSegments = receivedSegments;
  sample.timeoutCount = timeoutCount;
  sample.nackCount = nackCount;
  sample.dataName = dataName;
  update(NetworkTelemetryKey{consumerProvider, ndn::Name(), producerProvider,
                             keyScope},
         sample);
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::getServicePath(const ndn::Name& providerName,
                                      const ndn::Name& serviceName) const
{
  return get(NetworkTelemetryKey{providerName, serviceName, ndn::Name(), "",
                                 NetworkTelemetrySampleKind::AckRtt});
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::getActiveProbe(const ndn::Name& observerProvider,
                                      const ndn::Name& targetProvider,
                                      const std::string& requestScopeDigest) const
{
  return get(NetworkTelemetryKey{observerProvider, ndn::Name(), targetProvider,
                                 requestScopeDigest,
                                 NetworkTelemetrySampleKind::ActiveProbeRtt});
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::getActiveProbeGoodput(
  const ndn::Name& observerProvider,
  const ndn::Name& sourceProvider,
  const std::string& requestScopeDigest) const
{
  return get(NetworkTelemetryKey{observerProvider, ndn::Name(), sourceProvider,
                                 requestScopeDigest,
                                 NetworkTelemetrySampleKind::ActiveProbeGoodput});
}

bool
NetworkTelemetryStore::authorizeActiveProbeRequest(
  const std::string& requestScopeDigest,
  const std::vector<ndn::Name>& candidateProviders,
  const std::vector<NetworkTelemetryDataEdge>& allowedRttPairs,
  const std::vector<NetworkTelemetryDataEdge>& allowedGoodputEdges,
  std::chrono::steady_clock::time_point expiresAt,
  std::shared_ptr<std::atomic<bool>> registrationCancelled)
{
  constexpr size_t MAX_AUTHORIZED_CANDIDATES = 17;
  constexpr size_t MAX_AUTHORIZED_RTT_PAIRS = 16;
  constexpr size_t MAX_AUTHORIZED_GOODPUT_EDGES = 16;
  constexpr size_t MAX_AUTHORIZATION_ROWS = 1024;
  const auto now = std::chrono::steady_clock::now();
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      candidateProviders.size() < 2 ||
      candidateProviders.size() > MAX_AUTHORIZED_CANDIDATES ||
      allowedRttPairs.size() > MAX_AUTHORIZED_RTT_PAIRS ||
      allowedGoodputEdges.size() > MAX_AUTHORIZED_GOODPUT_EDGES ||
      expiresAt <= now) {
    return false;
  }

  auto canonicalCandidates = candidateProviders;
  if (std::any_of(canonicalCandidates.begin(), canonicalCandidates.end(),
                  [] (const ndn::Name& candidate) { return candidate.empty(); })) {
    return false;
  }
  std::sort(canonicalCandidates.begin(), canonicalCandidates.end());
  canonicalCandidates.erase(
    std::unique(canonicalCandidates.begin(), canonicalCandidates.end()),
    canonicalCandidates.end());
  if (canonicalCandidates.size() < 2) {
    return false;
  }

  auto canonicalRttPairs = allowedRttPairs;
  if (std::any_of(canonicalRttPairs.begin(), canonicalRttPairs.end(),
                  [] (const NetworkTelemetryDataEdge& pair) {
                    return pair.srcProvider.empty() || pair.dstProvider.empty() ||
                           pair.srcProvider == pair.dstProvider;
                  })) {
    return false;
  }
  std::sort(canonicalRttPairs.begin(), canonicalRttPairs.end());
  if (std::adjacent_find(canonicalRttPairs.begin(), canonicalRttPairs.end(),
                         [] (const NetworkTelemetryDataEdge& lhs,
                             const NetworkTelemetryDataEdge& rhs) {
                           return !(lhs < rhs) && !(rhs < lhs);
                         }) != canonicalRttPairs.end()) {
    return false;
  }
  for (const auto& pair : canonicalRttPairs) {
    if (!std::binary_search(canonicalCandidates.begin(),
                            canonicalCandidates.end(), pair.srcProvider) ||
        !std::binary_search(canonicalCandidates.begin(),
                            canonicalCandidates.end(), pair.dstProvider)) {
      return false;
    }
  }

  auto canonicalEdges = allowedGoodputEdges;
  if (std::any_of(canonicalEdges.begin(), canonicalEdges.end(),
                  [] (const NetworkTelemetryDataEdge& edge) {
                    return edge.srcProvider.empty() || edge.dstProvider.empty() ||
                           edge.srcProvider == edge.dstProvider;
                  })) {
    return false;
  }
  std::sort(canonicalEdges.begin(), canonicalEdges.end());
  if (std::adjacent_find(canonicalEdges.begin(), canonicalEdges.end(),
                         [] (const NetworkTelemetryDataEdge& lhs,
                             const NetworkTelemetryDataEdge& rhs) {
                           return !(lhs < rhs) && !(rhs < lhs);
                         }) != canonicalEdges.end()) {
    return false;
  }
  for (const auto& edge : canonicalEdges) {
    if (!std::binary_search(canonicalCandidates.begin(),
                            canonicalCandidates.end(), edge.srcProvider) ||
        !std::binary_search(canonicalCandidates.begin(),
                            canonicalCandidates.end(), edge.dstProvider)) {
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  pruneActiveProbeAuthorizations(now);
  const auto existing = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (existing != m_activeProbeAuthorizations.end()) {
    // A replay can reuse the original authorization, but cannot broaden its
    // roster, renew its lifetime, or reopen a completed/cancelled collection.
    return !existing->second.collectionClosed &&
           !authorizationCancelled(existing->second) &&
           existing->second.candidateProviders == canonicalCandidates &&
           existing->second.allowedRttPairs.size() == canonicalRttPairs.size() &&
           std::equal(existing->second.allowedRttPairs.begin(),
                      existing->second.allowedRttPairs.end(),
                      canonicalRttPairs.begin(),
                      [] (const NetworkTelemetryDataEdge& lhs,
                          const NetworkTelemetryDataEdge& rhs) {
                        return !(lhs < rhs) && !(rhs < lhs);
                      }) &&
           existing->second.allowedGoodputEdges.size() == canonicalEdges.size() &&
           std::equal(existing->second.allowedGoodputEdges.begin(),
                      existing->second.allowedGoodputEdges.end(),
                      canonicalEdges.begin(),
                      [] (const NetworkTelemetryDataEdge& lhs,
                          const NetworkTelemetryDataEdge& rhs) {
                        return !(lhs < rhs) && !(rhs < lhs);
                      });
  }

  if (m_activeProbeAuthorizations.size() >= MAX_AUTHORIZATION_ROWS) {
    // Preserve every live scope's probe claims until expiry. Evicting one
    // would let a replay re-register it and reset its request-wide budget.
    return false;
  }
  ActiveProbeAuthorization authorization;
  authorization.candidateProviders = std::move(canonicalCandidates);
  authorization.allowedRttPairs = std::move(canonicalRttPairs);
  authorization.allowedGoodputEdges = std::move(canonicalEdges);
  authorization.expiresAt = expiresAt;
  authorization.registrationCancelled = std::move(registrationCancelled);
  if (authorizationCancelled(authorization))
    return false;
  m_activeProbeAuthorizations.emplace(requestScopeDigest,
                                      std::move(authorization));
  return true;
}

std::optional<std::chrono::steady_clock::time_point>
NetworkTelemetryStore::activeProbeScopeDeadline(
  const std::string& requestScopeDigest) const
{
  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second))
    return std::nullopt;
  return it->second.expiresAt;
}

bool
NetworkTelemetryStore::closeActiveProbeCollection(
  const std::string& requestScopeDigest)
{
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second)) {
    return false;
  }
  it->second.collectionClosed = true;
  return true;
}

bool
NetworkTelemetryStore::isActiveProbeCollectionOpen(
  const std::string& requestScopeDigest) const
{
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  return it != m_activeProbeAuthorizations.end() &&
         it->second.expiresAt > std::chrono::steady_clock::now() &&
         !authorizationCancelled(it->second) &&
         !it->second.collectionClosed;
}

bool
NetworkTelemetryStore::isActiveProbeGoodputAuthorized(
  const std::string& requestScopeDigest,
  const ndn::Name& sourceProvider,
  const ndn::Name& destinationProvider) const
{
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      sourceProvider.empty() || destinationProvider.empty() ||
      sourceProvider == destinationProvider) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second)) {
    return false;
  }
  const NetworkTelemetryDataEdge edge{sourceProvider, destinationProvider};
  return std::binary_search(it->second.allowedGoodputEdges.begin(),
                            it->second.allowedGoodputEdges.end(), edge);
}

bool
NetworkTelemetryStore::claimActiveProbeGoodputEdge(
  const std::string& requestScopeDigest,
  const ndn::Name& sourceProvider,
  const ndn::Name& destinationProvider)
{
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      sourceProvider.empty() || destinationProvider.empty() ||
      sourceProvider == destinationProvider) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second) ||
      it->second.collectionClosed) {
    return false;
  }

  const NetworkTelemetryDataEdge edge{sourceProvider, destinationProvider};
  const auto allowed = std::lower_bound(it->second.allowedGoodputEdges.begin(),
                                        it->second.allowedGoodputEdges.end(), edge);
  if (allowed == it->second.allowedGoodputEdges.end() ||
      edge < *allowed || *allowed < edge) {
    return false;
  }

  const auto claimed = std::lower_bound(it->second.claimedGoodputEdges.begin(),
                                        it->second.claimedGoodputEdges.end(), edge);
  if (claimed != it->second.claimedGoodputEdges.end() &&
      !(edge < *claimed) && !(*claimed < edge)) {
    return false;
  }
  it->second.claimedGoodputEdges.insert(claimed, edge);
  return true;
}

bool
NetworkTelemetryStore::acceptActiveProbeGoodputSegment(
  const std::string& requestScopeDigest,
  const ndn::Name& sourceProvider,
  const ndn::Name& destinationProvider,
  const std::string& runNonce,
  size_t segment,
  size_t segmentCount)
{
  constexpr size_t MAX_ACTIVE_GOODPUT_SEGMENTS = NETWORK_PROBE_WARMED_SEGMENTS;
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      sourceProvider.empty() || destinationProvider.empty() ||
      sourceProvider == destinationProvider || runNonce.empty() ||
      runNonce.size() > 20 || segmentCount != MAX_ACTIVE_GOODPUT_SEGMENTS ||
      segment >= segmentCount) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto authorization = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (authorization == m_activeProbeAuthorizations.end() ||
      authorization->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(authorization->second)) {
    return false;
  }

  const NetworkTelemetryDataEdge edge{sourceProvider, destinationProvider};
  if (!std::binary_search(authorization->second.allowedGoodputEdges.begin(),
                          authorization->second.allowedGoodputEdges.end(), edge)) {
    return false;
  }

  auto train = authorization->second.receivedGoodputTrains.find(edge);
  if (train == authorization->second.receivedGoodputTrains.end()) {
    train = authorization->second.receivedGoodputTrains.emplace(
      edge, ActiveProbeAuthorization::GoodputTrain{runNonce, 0}).first;
  }
  else if (train->second.runNonce != runNonce) {
    return false;
  }

  static_assert(NETWORK_PROBE_WARMED_SEGMENTS <= 32, "probe admission bitmap is bounded");
  const auto segmentBit = uint32_t{1} << segment;
  if ((train->second.acceptedSegmentMask & segmentBit) != 0) {
    return false;
  }
  train->second.acceptedSegmentMask |= segmentBit;
  return true;
}

bool
NetworkTelemetryStore::isActiveProbeAuthorized(
  const std::string& requestScopeDigest,
  const ndn::Name& observerProvider,
  const ndn::Name& targetProvider) const
{
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      observerProvider.empty() || targetProvider.empty() ||
      observerProvider == targetProvider) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second)) {
    return false;
  }
  const NetworkTelemetryDataEdge pair{observerProvider, targetProvider};
  return std::binary_search(it->second.allowedRttPairs.begin(),
                            it->second.allowedRttPairs.end(), pair);
}

bool
NetworkTelemetryStore::claimActiveProbeRttPair(
  const std::string& requestScopeDigest,
  const ndn::Name& observerProvider,
  const ndn::Name& targetProvider)
{
  constexpr size_t MAX_ACTIVE_PROBE_RTT_PAIRS = 16;
  if (requestScopeDigest.empty() || requestScopeDigest.size() > 128 ||
      observerProvider.empty() || targetProvider.empty() ||
      observerProvider == targetProvider) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_activeProbeAuthorizationMutex);
  const auto it = m_activeProbeAuthorizations.find(requestScopeDigest);
  if (it == m_activeProbeAuthorizations.end() ||
      it->second.expiresAt <= std::chrono::steady_clock::now() ||
      authorizationCancelled(it->second) ||
      it->second.collectionClosed ||
      !std::binary_search(it->second.allowedRttPairs.begin(),
                          it->second.allowedRttPairs.end(),
                          NetworkTelemetryDataEdge{observerProvider, targetProvider})) {
    return false;
  }

  const NetworkTelemetryDataEdge pair{observerProvider, targetProvider};
  const auto claimed = std::lower_bound(it->second.claimedRttPairs.begin(),
                                        it->second.claimedRttPairs.end(), pair);
  if (it->second.claimedRttPairs.size() >= MAX_ACTIVE_PROBE_RTT_PAIRS ||
      (claimed != it->second.claimedRttPairs.end() &&
       !(pair < *claimed) && !(*claimed < pair))) {
    return false;
  }
  it->second.claimedRttPairs.insert(claimed, pair);
  return true;
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::getDependencyEdge(const ndn::Name& consumerProvider,
                                         const ndn::Name& producerProvider,
                                         const std::string& keyScope) const
{
  return get(NetworkTelemetryKey{consumerProvider, ndn::Name(), producerProvider,
                                 keyScope,
                                 NetworkTelemetrySampleKind::LargeDataFetch});
}

size_t
NetworkTelemetryStore::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_snapshots.size();
}

void
NetworkTelemetryStore::pruneActiveProbeAuthorizations(
  std::chrono::steady_clock::time_point now)
{
  for (auto it = m_activeProbeAuthorizations.begin();
       it != m_activeProbeAuthorizations.end();) {
    if (it->second.expiresAt <= now) {
      it = m_activeProbeAuthorizations.erase(it);
    }
    else {
      ++it;
    }
  }
}

NetworkTelemetrySnapshot
NetworkTelemetryStore::makeSnapshot(const NetworkTelemetryKey& key,
                                    const NetworkTelemetrySample& sample) const
{
  NetworkTelemetrySnapshot snapshot;
  snapshot.providerName = key.providerName;
  snapshot.serviceName = key.serviceName;
  snapshot.peerName = key.peerName;
  snapshot.edgeName = (sample.kind == NetworkTelemetrySampleKind::ActiveProbeRtt ||
                       sample.kind == NetworkTelemetrySampleKind::ActiveProbeGoodput) ?
    "" : key.edgeName;
  snapshot.kind = sample.kind;
  snapshot.rttMs = sample.rttMs;
  snapshot.firstByteMs = sample.firstByteMs;
  snapshot.elapsedMs = sample.elapsedMs;
  snapshot.encodedBytes = sample.encodedBytes;
  snapshot.payloadBytes = sample.payloadBytes;
  snapshot.wireBytes = sample.wireBytes;
  snapshot.receivedSegments = sample.receivedSegments;
  snapshot.probeSegmentsSent = sample.probeSegmentsSent;
  snapshot.probeRttMs = sample.probeRttMs;
  snapshot.probeInterestGapsMs = sample.probeInterestGapsMs;
  snapshot.probeDataArrivalGapsMs = sample.probeDataArrivalGapsMs;
  snapshot.probeDataWireBytes = sample.probeDataWireBytes;
  snapshot.probeSourceSendGapsMs = sample.probeSourceSendGapsMs;
  snapshot.probeSourceTimingValidated = sample.probeSourceTimingValidated;
  snapshot.probeProfileDigest = sample.probeProfileDigest;
  snapshot.probeComplete = sample.probeComplete;
  snapshot.targetSignatureValidated = sample.targetSignatureValidated;
  snapshot.probeSignatureValid = sample.probeSignatureValid;
  snapshot.timeoutCount = sample.timeoutCount;
  snapshot.nackCount = sample.nackCount;
  snapshot.sampleCount = 1;
  snapshot.lastUpdatedMs = nowMs();
  snapshot.peerBootEpoch = sample.peerBootEpoch;
  snapshot.dataName = sample.dataName;
  refreshDerivedFields(snapshot);
  return snapshot;
}

void
NetworkTelemetryStore::refreshDerivedFields(NetworkTelemetrySnapshot& snapshot) const
{
  snapshot.goodputMbps = networkTelemetryGoodputMbps(
    snapshot.payloadBytes, snapshot.elapsedMs);
  const auto ageMs =
    snapshot.lastUpdatedMs == 0 ? 0 : nowMs() - snapshot.lastUpdatedMs;
  snapshot.stale = m_ttl.count() > 0 &&
                   ageMs > static_cast<uint64_t>(m_ttl.count());
  snapshot.confidence =
    std::min(1.0, static_cast<double>(snapshot.sampleCount) / 5.0);
  if (snapshot.stale) {
    snapshot.confidence *= 0.25;
  }
  const auto failedSignals = snapshot.timeoutCount + snapshot.nackCount;
  const auto totalSignals = failedSignals + snapshot.receivedSegments;
  if (totalSignals > 0 && failedSignals > 0) {
    const double failureRatio =
      static_cast<double>(failedSignals) / static_cast<double>(totalSignals);
    snapshot.confidence *= std::max(0.0, 1.0 - failureRatio);
  }
}

void
NetworkTelemetryStore::pruneExpired(uint64_t now)
{
  if (m_ttl.count() <= 0) {
    return;
  }
  const auto ttlMs = static_cast<uint64_t>(m_ttl.count());
  for (auto it = m_snapshots.begin(); it != m_snapshots.end();) {
    const auto updated = it->second.lastUpdatedMs;
    if (updated != 0 && now >= updated && now - updated > ttlMs) {
      it = m_snapshots.erase(it);
    }
    else {
      ++it;
    }
  }
}

double
NetworkTelemetryStore::smooth(double oldValue, double newValue) const
{
  if (oldValue <= 0.0) {
    return newValue;
  }
  return (m_alpha * newValue) + ((1.0 - m_alpha) * oldValue);
}

uint64_t
NetworkTelemetryStore::nowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace ndn_service_framework
