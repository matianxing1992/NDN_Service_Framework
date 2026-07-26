#ifndef NDNSF_UAV_SENSOR_STREAMS_HPP
#define NDNSF_UAV_SENSOR_STREAMS_HPP

#include "ndn-service-framework/Stream.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

inline constexpr uint64_t UAV_TELEMETRY_PERIOD_MS = 50;
inline constexpr uint64_t UAV_ACOUSTIC_BLOCK_PERIOD_MS = 40;

struct CompactTelemetrySample
{
  uint64_t sampleId = 0;
  uint64_t sourceTimestampNs = 0;
  std::string droneId;
  int32_t latitudeE7 = 0;
  int32_t longitudeE7 = 0;
  int32_t altitudeMm = 0;
  int32_t groundSpeedMmps = 0;
  uint16_t batteryPermille = 0;
  uint8_t readinessFlags = 0;
  uint8_t linkQuality = 0;

  static size_t encodedSizeFor(uint64_t sampleId);
  static CompactTelemetrySample deterministic(uint64_t sampleId,
                                              uint64_t sourceTimestampNs,
                                              std::string droneId);
  std::vector<uint8_t> encode() const;
  static std::optional<CompactTelemetrySample>
  decode(const std::vector<uint8_t>& wire);
};

struct TelemetryAdmissionResult
{
  bool valid = false;
  bool stateAdvanced = false;
  bool newSample = false;
  bool duplicate = false;
  bool outOfOrder = false;
  uint64_t sampleId = 0;
  uint64_t ageNs = 0;
  std::string reason;
};

/** APP-owned latest-state admission; Core still owns validation and fetching. */
class LatestTelemetryAdmission
{
public:
  explicit LatestTelemetryAdmission(std::string expectedDroneId);
  TelemetryAdmissionResult admit(const std::vector<uint8_t>& wire,
                                 uint64_t receivedTimestampNs);
  std::optional<CompactTelemetrySample> latest() const;
  uint64_t admittedCount() const;
  uint64_t duplicateCount() const;
  uint64_t outOfOrderCount() const;

private:
  std::string m_expectedDroneId;
  std::optional<CompactTelemetrySample> m_latest;
  std::set<uint64_t> m_seenSampleIds;
  uint64_t m_admitted = 0;
  uint64_t m_duplicates = 0;
  uint64_t m_outOfOrder = 0;
};

struct OpaqueAcousticSource
{
  uint64_t blockId = 0;
  uint64_t captureTimestampNs = 0;
  uint16_t sourceIndex = 0;
  uint16_t sourceCount = 0;
  std::vector<uint8_t> opaqueBytes;

  static size_t sourceCountFor(uint64_t blockId);
  static OpaqueAcousticSource deterministic(uint64_t blockId,
                                            uint64_t captureTimestampNs,
                                            size_t sourceIndex);
  std::vector<uint8_t> encode() const;
  static std::optional<OpaqueAcousticSource>
  decode(const std::vector<uint8_t>& wire);
};

std::string
acousticSourceCountClass(size_t sourceCount);

struct CompleteAcousticBlock
{
  uint64_t blockId = 0;
  uint64_t captureTimestampNs = 0;
  uint64_t completedTimestampNs = 0;
  size_t recoveredSources = 0;
  std::vector<std::vector<uint8_t>> orderedSources;
};

struct AcousticAdmissionResult
{
  bool valid = false;
  bool duplicate = false;
  bool late = false;
  std::optional<CompleteAcousticBlock> completed;
  std::string reason;
};

/** APP-owned complete-block admission over generic signed/recovered items. */
class CompleteAcousticBlockAdmission
{
public:
  explicit CompleteAcousticBlockAdmission(std::string expectedStreamId);
  AcousticAdmissionResult admit(
    const std::vector<uint8_t>& wire,
    ndn_service_framework::LiveStreamItemProvenance provenance,
    uint64_t receivedTimestampNs);
  uint64_t completedCount() const;
  uint64_t duplicateCount() const;
  uint64_t invalidCount() const;

private:
  struct PendingBlock
  {
    uint64_t captureTimestampNs = 0;
    size_t sourceCount = 0;
    size_t recoveredSources = 0;
    std::map<size_t, std::vector<uint8_t>> sources;
  };

  std::string m_expectedStreamId;
  std::map<uint64_t, PendingBlock> m_pending;
  std::map<uint64_t, bool> m_completed;
  uint64_t m_completedCount = 0;
  uint64_t m_duplicates = 0;
  uint64_t m_invalid = 0;
};

ndn_service_framework::LiveStreamDefinition
makeUavTelemetryStreamDefinition(const ndn::Name& provider,
                                 uint64_t sessionEpoch,
                                 uint64_t mappingVersion);

ndn_service_framework::LiveStreamDefinition
makeUavAcousticStreamDefinition(const ndn::Name& provider,
                                uint64_t sessionEpoch,
                                uint64_t mappingVersion);

} // namespace ndnsf::examples::uav

#endif
