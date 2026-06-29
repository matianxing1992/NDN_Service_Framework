#ifndef NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP
#define NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP

#include "common.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace ndn_service_framework {

enum class NetworkTelemetrySampleKind
{
  AckRtt,
  ResponseRtt,
  LargeDataFetch,
  ActiveProbe,
};

struct NetworkTelemetrySample
{
  NetworkTelemetrySampleKind kind = NetworkTelemetrySampleKind::AckRtt;
  double rttMs = 0.0;
  double firstByteMs = 0.0;
  double elapsedMs = 0.0;
  size_t encodedBytes = 0;
  size_t wireBytes = 0;
  size_t receivedSegments = 0;
  size_t timeoutCount = 0;
  size_t nackCount = 0;
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
  size_t wireBytes = 0;
  double goodputMbps = 0.0;
  size_t receivedSegments = 0;
  size_t timeoutCount = 0;
  size_t nackCount = 0;
  size_t sampleCount = 0;
  uint64_t lastUpdatedMs = 0;
  double confidence = 0.0;
  bool stale = false;
  ndn::Name dataName;
};

struct NetworkTelemetryKey
{
  ndn::Name providerName;
  ndn::Name serviceName;
  ndn::Name peerName;
  std::string edgeName;

  bool operator<(const NetworkTelemetryKey& other) const;
};

double networkTelemetryGoodputMbps(size_t wireBytes, double elapsedMs);
const char* toString(NetworkTelemetrySampleKind kind);

class NetworkTelemetryStore
{
public:
  explicit NetworkTelemetryStore(
    double alpha = 0.25,
    std::chrono::milliseconds ttl = std::chrono::seconds(30));

  void update(const NetworkTelemetryKey& key,
              const NetworkTelemetrySample& sample);

  std::optional<NetworkTelemetrySnapshot>
  get(const NetworkTelemetryKey& key) const;

  void updateAckRtt(const ndn::Name& providerName,
                    const ndn::Name& serviceName,
                    double rttMs);

  void updateLargeDataFetch(const ndn::Name& consumerProvider,
                            const ndn::Name& producerProvider,
                            const std::string& keyScope,
                            const ndn::Name& dataName,
                            double elapsedMs,
                            double firstByteMs,
                            size_t encodedBytes,
                            size_t wireBytes,
                            size_t receivedSegments,
                            size_t timeoutCount,
                            size_t nackCount);

  std::optional<NetworkTelemetrySnapshot>
  getServicePath(const ndn::Name& providerName,
                 const ndn::Name& serviceName) const;

  std::optional<NetworkTelemetrySnapshot>
  getDependencyEdge(const ndn::Name& consumerProvider,
                    const ndn::Name& producerProvider,
                    const std::string& keyScope) const;

  size_t size() const;

private:
  NetworkTelemetrySnapshot makeSnapshot(const NetworkTelemetryKey& key,
                                        const NetworkTelemetrySample& sample) const;
  void refreshDerivedFields(NetworkTelemetrySnapshot& snapshot) const;
  double smooth(double oldValue, double newValue) const;
  static uint64_t nowMs();

private:
  double m_alpha = 0.25;
  std::chrono::milliseconds m_ttl;
  std::map<NetworkTelemetryKey, NetworkTelemetrySnapshot> m_snapshots;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_NETWORK_TELEMETRY_HPP
