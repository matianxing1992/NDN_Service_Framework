#include "NetworkTelemetry.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace ndn_service_framework {

bool
NetworkTelemetryKey::operator<(const NetworkTelemetryKey& other) const
{
  return std::tie(providerName, serviceName, peerName, edgeName) <
         std::tie(other.providerName, other.serviceName, other.peerName,
                  other.edgeName);
}

double
networkTelemetryGoodputMbps(size_t wireBytes, double elapsedMs)
{
  if (wireBytes == 0 || elapsedMs <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(wireBytes) * 8.0 / (elapsedMs * 1000.0);
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
  case NetworkTelemetrySampleKind::ActiveProbe:
    return "active-probe";
  }
  return "unknown";
}

NetworkTelemetryStore::NetworkTelemetryStore(double alpha,
                                             std::chrono::milliseconds ttl)
  : m_alpha(std::min(1.0, std::max(0.0, alpha)))
  , m_ttl(ttl)
{
}

void
NetworkTelemetryStore::update(const NetworkTelemetryKey& key,
                              const NetworkTelemetrySample& sample)
{
  auto it = m_snapshots.find(key);
  if (it == m_snapshots.end()) {
    m_snapshots.emplace(key, makeSnapshot(key, sample));
    return;
  }

  auto& snapshot = it->second;
  snapshot.kind = sample.kind;
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
  snapshot.timeoutCount += sample.timeoutCount;
  snapshot.nackCount += sample.nackCount;
  ++snapshot.sampleCount;
  snapshot.lastUpdatedMs = nowMs();
  refreshDerivedFields(snapshot);
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::get(const NetworkTelemetryKey& key) const
{
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
NetworkTelemetryStore::updateLargeDataFetch(const ndn::Name& consumerProvider,
                                            const ndn::Name& producerProvider,
                                            const std::string& keyScope,
                                            const ndn::Name& dataName,
                                            double elapsedMs,
                                            double firstByteMs,
                                            size_t encodedBytes,
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
  return get(NetworkTelemetryKey{providerName, serviceName, ndn::Name(), ""});
}

std::optional<NetworkTelemetrySnapshot>
NetworkTelemetryStore::getDependencyEdge(const ndn::Name& consumerProvider,
                                         const ndn::Name& producerProvider,
                                         const std::string& keyScope) const
{
  return get(NetworkTelemetryKey{consumerProvider, ndn::Name(), producerProvider,
                                 keyScope});
}

size_t
NetworkTelemetryStore::size() const
{
  return m_snapshots.size();
}

NetworkTelemetrySnapshot
NetworkTelemetryStore::makeSnapshot(const NetworkTelemetryKey& key,
                                    const NetworkTelemetrySample& sample) const
{
  NetworkTelemetrySnapshot snapshot;
  snapshot.providerName = key.providerName;
  snapshot.serviceName = key.serviceName;
  snapshot.peerName = key.peerName;
  snapshot.edgeName = key.edgeName;
  snapshot.kind = sample.kind;
  snapshot.rttMs = sample.rttMs;
  snapshot.firstByteMs = sample.firstByteMs;
  snapshot.elapsedMs = sample.elapsedMs;
  snapshot.encodedBytes = sample.encodedBytes;
  snapshot.wireBytes = sample.wireBytes;
  snapshot.receivedSegments = sample.receivedSegments;
  snapshot.timeoutCount = sample.timeoutCount;
  snapshot.nackCount = sample.nackCount;
  snapshot.sampleCount = 1;
  snapshot.lastUpdatedMs = nowMs();
  snapshot.dataName = sample.dataName;
  refreshDerivedFields(snapshot);
  return snapshot;
}

void
NetworkTelemetryStore::refreshDerivedFields(NetworkTelemetrySnapshot& snapshot) const
{
  snapshot.goodputMbps =
    networkTelemetryGoodputMbps(snapshot.wireBytes, snapshot.elapsedMs);
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
