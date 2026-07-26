#include "UavSensorStreams.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace ndnsf::examples::uav {
namespace {

constexpr std::array<uint8_t, 4> TELEMETRY_MAGIC{{'U', 'T', 'S', '1'}};
constexpr std::array<uint8_t, 4> ACOUSTIC_MAGIC{{'U', 'A', 'S', '1'}};
constexpr size_t ACOUSTIC_SOURCE_HEADER_SIZE = 27;
constexpr size_t ACOUSTIC_SOURCE_WIRE_LIMIT = 512;
constexpr size_t ACOUSTIC_OPAQUE_BYTES_LIMIT =
  ACOUSTIC_SOURCE_WIRE_LIMIT - ACOUSTIC_SOURCE_HEADER_SIZE;

void
appendUint(std::vector<uint8_t>& output, uint64_t value, size_t width)
{
  for (size_t i = 0; i < width; ++i) {
    const auto shift = (width - i - 1) * 8;
    output.push_back(static_cast<uint8_t>(value >> shift));
  }
}

std::optional<uint64_t>
readUint(const std::vector<uint8_t>& input, size_t& offset, size_t width)
{
  if (width > sizeof(uint64_t) || offset > input.size() ||
      width > input.size() - offset) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value = (value << 8) | input[offset++];
  return value;
}

uint8_t
paddingByte(uint64_t id, size_t offset)
{
  return static_cast<uint8_t>((id * 131 + offset * 17 + 0x5a) & 0xff);
}

bool
readMagic(const std::vector<uint8_t>& wire, size_t& offset,
          const std::array<uint8_t, 4>& expected)
{
  if (wire.size() < expected.size() ||
      !std::equal(expected.begin(), expected.end(), wire.begin())) {
    return false;
  }
  offset = expected.size();
  return true;
}

} // namespace

size_t
CompactTelemetrySample::encodedSizeFor(uint64_t sampleId)
{
  static constexpr std::array<size_t, 3> SIZES{{256, 384, 512}};
  return SIZES.at(sampleId % SIZES.size());
}

CompactTelemetrySample
CompactTelemetrySample::deterministic(uint64_t sampleId,
                                      uint64_t sourceTimestampNs,
                                      std::string droneId)
{
  CompactTelemetrySample value;
  value.sampleId = sampleId;
  value.sourceTimestampNs = sourceTimestampNs;
  value.droneId = std::move(droneId);
  value.latitudeE7 = 350000000 + static_cast<int32_t>(sampleId % 10000);
  value.longitudeE7 = -899000000 + static_cast<int32_t>(sampleId % 10000);
  value.altitudeMm = 15000 + static_cast<int32_t>(sampleId % 3000);
  value.groundSpeedMmps = 4000 + static_cast<int32_t>(sampleId % 2000);
  value.batteryPermille = static_cast<uint16_t>(900 - (sampleId % 400));
  value.readinessFlags = 0x0f;
  value.linkQuality = static_cast<uint8_t>(80 + (sampleId % 21));
  return value;
}

std::vector<uint8_t>
CompactTelemetrySample::encode() const
{
  if (droneId.empty() || droneId.size() > 64 || batteryPermille > 1000) {
    throw std::invalid_argument("invalid compact telemetry fields");
  }
  std::vector<uint8_t> wire(TELEMETRY_MAGIC.begin(), TELEMETRY_MAGIC.end());
  appendUint(wire, 1, 1);
  appendUint(wire, sampleId, 8);
  appendUint(wire, sourceTimestampNs, 8);
  appendUint(wire, droneId.size(), 1);
  wire.insert(wire.end(), droneId.begin(), droneId.end());
  appendUint(wire, static_cast<uint32_t>(latitudeE7), 4);
  appendUint(wire, static_cast<uint32_t>(longitudeE7), 4);
  appendUint(wire, static_cast<uint32_t>(altitudeMm), 4);
  appendUint(wire, static_cast<uint32_t>(groundSpeedMmps), 4);
  appendUint(wire, batteryPermille, 2);
  appendUint(wire, readinessFlags, 1);
  appendUint(wire, linkQuality, 1);
  const auto target = encodedSizeFor(sampleId);
  if (wire.size() > target) throw std::length_error("telemetry header exceeds target");
  while (wire.size() < target) wire.push_back(paddingByte(sampleId, wire.size()));
  return wire;
}

std::optional<CompactTelemetrySample>
CompactTelemetrySample::decode(const std::vector<uint8_t>& wire)
{
  size_t offset = 0;
  if (!readMagic(wire, offset, TELEMETRY_MAGIC)) return std::nullopt;
  const auto version = readUint(wire, offset, 1);
  const auto sampleId = readUint(wire, offset, 8);
  const auto sourceTimestampNs = readUint(wire, offset, 8);
  const auto droneLength = readUint(wire, offset, 1);
  if (!version || *version != 1 || !sampleId || !sourceTimestampNs ||
      !droneLength || *droneLength == 0 || *droneLength > 64 ||
      offset + *droneLength > wire.size() ||
      wire.size() != encodedSizeFor(*sampleId)) {
    return std::nullopt;
  }
  CompactTelemetrySample value;
  value.sampleId = *sampleId;
  value.sourceTimestampNs = *sourceTimestampNs;
  value.droneId.assign(wire.begin() + offset,
                       wire.begin() + offset + *droneLength);
  offset += *droneLength;
  const auto latitude = readUint(wire, offset, 4);
  const auto longitude = readUint(wire, offset, 4);
  const auto altitude = readUint(wire, offset, 4);
  const auto speed = readUint(wire, offset, 4);
  const auto battery = readUint(wire, offset, 2);
  const auto readiness = readUint(wire, offset, 1);
  const auto link = readUint(wire, offset, 1);
  if (!latitude || !longitude || !altitude || !speed || !battery ||
      !readiness || !link || *battery > 1000) {
    return std::nullopt;
  }
  value.latitudeE7 = static_cast<int32_t>(*latitude);
  value.longitudeE7 = static_cast<int32_t>(*longitude);
  value.altitudeMm = static_cast<int32_t>(*altitude);
  value.groundSpeedMmps = static_cast<int32_t>(*speed);
  value.batteryPermille = static_cast<uint16_t>(*battery);
  value.readinessFlags = static_cast<uint8_t>(*readiness);
  value.linkQuality = static_cast<uint8_t>(*link);
  for (; offset < wire.size(); ++offset) {
    if (wire[offset] != paddingByte(value.sampleId, offset)) return std::nullopt;
  }
  return value;
}

LatestTelemetryAdmission::LatestTelemetryAdmission(std::string expectedDroneId)
  : m_expectedDroneId(std::move(expectedDroneId))
{
}

TelemetryAdmissionResult
LatestTelemetryAdmission::admit(const std::vector<uint8_t>& wire,
                                uint64_t receivedTimestampNs)
{
  TelemetryAdmissionResult result;
  const auto decoded = CompactTelemetrySample::decode(wire);
  if (!decoded || decoded->droneId != m_expectedDroneId ||
      receivedTimestampNs < decoded->sourceTimestampNs) {
    result.reason = !decoded ? "invalid-payload" :
                    decoded->droneId != m_expectedDroneId ? "wrong-drone" :
                    "clock-domain-regression";
    return result;
  }
  result.valid = true;
  result.ageNs = receivedTimestampNs - decoded->sourceTimestampNs;
  result.sampleId = decoded->sampleId;
  if (!m_seenSampleIds.insert(decoded->sampleId).second) {
    result.duplicate = true;
    result.reason = "duplicate";
    ++m_duplicates;
    return result;
  }
  result.newSample = true;
  ++m_admitted;
  if (m_latest && decoded->sampleId < m_latest->sampleId) {
    result.outOfOrder = true;
    result.reason = "out-of-order";
    ++m_outOfOrder;
    return result;
  }
  m_latest = *decoded;
  result.stateAdvanced = true;
  result.reason = "admitted";
  return result;
}

std::optional<CompactTelemetrySample>
LatestTelemetryAdmission::latest() const
{
  return m_latest;
}

uint64_t LatestTelemetryAdmission::admittedCount() const { return m_admitted; }
uint64_t LatestTelemetryAdmission::duplicateCount() const { return m_duplicates; }
uint64_t LatestTelemetryAdmission::outOfOrderCount() const { return m_outOfOrder; }

size_t
OpaqueAcousticSource::sourceCountFor(uint64_t blockId)
{
  return 2 + blockId % 3;
}

OpaqueAcousticSource
OpaqueAcousticSource::deterministic(uint64_t blockId,
                                    uint64_t captureTimestampNs,
                                    size_t sourceIndex)
{
  OpaqueAcousticSource value;
  value.blockId = blockId;
  value.captureTimestampNs = captureTimestampNs;
  value.sourceCount = static_cast<uint16_t>(sourceCountFor(blockId));
  if (sourceIndex >= value.sourceCount) {
    throw std::out_of_range("acoustic source index");
  }
  value.sourceIndex = static_cast<uint16_t>(sourceIndex);
  // Keep the complete APP source item (27-byte binding header plus opaque
  // bytes) within the frozen 512-byte contract.
  const auto size = 288 + ((blockId + sourceIndex) % 4) * 64;
  value.opaqueBytes.resize(size);
  for (size_t i = 0; i < value.opaqueBytes.size(); ++i) {
    value.opaqueBytes[i] = paddingByte(blockId + sourceIndex * 1009, i);
  }
  return value;
}

std::vector<uint8_t>
OpaqueAcousticSource::encode() const
{
  if (sourceCount < 2 || sourceCount > 4 || sourceIndex >= sourceCount ||
      opaqueBytes.empty() || opaqueBytes.size() > ACOUSTIC_OPAQUE_BYTES_LIMIT) {
    throw std::invalid_argument("invalid acoustic source");
  }
  std::vector<uint8_t> wire(ACOUSTIC_MAGIC.begin(), ACOUSTIC_MAGIC.end());
  appendUint(wire, 1, 1);
  appendUint(wire, blockId, 8);
  appendUint(wire, captureTimestampNs, 8);
  appendUint(wire, sourceIndex, 2);
  appendUint(wire, sourceCount, 2);
  appendUint(wire, opaqueBytes.size(), 2);
  wire.insert(wire.end(), opaqueBytes.begin(), opaqueBytes.end());
  return wire;
}

std::optional<OpaqueAcousticSource>
OpaqueAcousticSource::decode(const std::vector<uint8_t>& wire)
{
  size_t offset = 0;
  if (!readMagic(wire, offset, ACOUSTIC_MAGIC)) return std::nullopt;
  const auto version = readUint(wire, offset, 1);
  const auto block = readUint(wire, offset, 8);
  const auto captured = readUint(wire, offset, 8);
  const auto index = readUint(wire, offset, 2);
  const auto count = readUint(wire, offset, 2);
  const auto size = readUint(wire, offset, 2);
  if (!version || *version != 1 || !block || !captured || !index || !count ||
      !size || *count < 2 || *count > 4 || *index >= *count ||
      *count != sourceCountFor(*block) || *size == 0 ||
      *size > ACOUSTIC_OPAQUE_BYTES_LIMIT ||
      offset + *size != wire.size()) {
    return std::nullopt;
  }
  OpaqueAcousticSource value;
  value.blockId = *block;
  value.captureTimestampNs = *captured;
  value.sourceIndex = static_cast<uint16_t>(*index);
  value.sourceCount = static_cast<uint16_t>(*count);
  value.opaqueBytes.assign(wire.begin() + offset, wire.end());
  const auto expected = deterministic(value.blockId, value.captureTimestampNs,
                                      value.sourceIndex);
  if (value.opaqueBytes != expected.opaqueBytes) return std::nullopt;
  return value;
}

std::string
acousticSourceCountClass(size_t sourceCount)
{
  if (sourceCount < 2 || sourceCount > 4) {
    throw std::invalid_argument("acoustic source count must be between 2 and 4");
  }
  return "opaque-block-" + std::to_string(sourceCount);
}

CompleteAcousticBlockAdmission::CompleteAcousticBlockAdmission(
  std::string expectedStreamId)
  : m_expectedStreamId(std::move(expectedStreamId))
{
}

AcousticAdmissionResult
CompleteAcousticBlockAdmission::admit(
  const std::vector<uint8_t>& wire,
  ndn_service_framework::LiveStreamItemProvenance provenance,
  uint64_t receivedTimestampNs)
{
  AcousticAdmissionResult result;
  const auto decoded = OpaqueAcousticSource::decode(wire);
  if (!decoded || receivedTimestampNs < decoded->captureTimestampNs) {
    ++m_invalid;
    result.reason = !decoded ? "invalid-source" : "clock-domain-regression";
    return result;
  }
  result.valid = true;
  if (m_completed.count(decoded->blockId) != 0) {
    ++m_duplicates;
    result.duplicate = true;
    result.late = true;
    result.reason = "completed-block-duplicate";
    return result;
  }
  auto& pending = m_pending[decoded->blockId];
  if (pending.sourceCount == 0) {
    pending.sourceCount = decoded->sourceCount;
    pending.captureTimestampNs = decoded->captureTimestampNs;
  }
  if (pending.sourceCount != decoded->sourceCount ||
      pending.captureTimestampNs != decoded->captureTimestampNs) {
    ++m_invalid;
    result.valid = false;
    result.reason = "conflicting-block-header";
    return result;
  }
  if (!pending.sources.emplace(decoded->sourceIndex, wire).second) {
    ++m_duplicates;
    result.duplicate = true;
    result.reason = "duplicate-source";
    return result;
  }
  if (provenance == ndn_service_framework::LiveStreamItemProvenance::FecRecovered) {
    ++pending.recoveredSources;
  }
  if (pending.sources.size() != pending.sourceCount) {
    result.reason = "partial";
    return result;
  }
  CompleteAcousticBlock complete;
  complete.blockId = decoded->blockId;
  complete.captureTimestampNs = pending.captureTimestampNs;
  complete.completedTimestampNs = receivedTimestampNs;
  complete.recoveredSources = pending.recoveredSources;
  for (size_t index = 0; index < pending.sourceCount; ++index) {
    complete.orderedSources.push_back(pending.sources.at(index));
  }
  m_pending.erase(decoded->blockId);
  m_completed[decoded->blockId] = true;
  ++m_completedCount;
  result.completed = std::move(complete);
  result.reason = "complete";
  return result;
}

uint64_t CompleteAcousticBlockAdmission::completedCount() const
{
  return m_completedCount;
}
uint64_t CompleteAcousticBlockAdmission::duplicateCount() const
{
  return m_duplicates;
}
uint64_t CompleteAcousticBlockAdmission::invalidCount() const
{
  return m_invalid;
}

ndn_service_framework::LiveStreamDefinition
makeUavTelemetryStreamDefinition(const ndn::Name& provider,
                                 uint64_t sessionEpoch,
                                 uint64_t mappingVersion)
{
  ndn_service_framework::LiveStreamDefinition definition;
  definition.contractVersion =
    ndn_service_framework::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "uav-telemetry-" + std::to_string(sessionEpoch);
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("sensor")
    .append("state").append(definition.streamId).appendVersion(mappingVersion);
  definition.sessionEpoch = sessionEpoch;
  definition.mappingVersion = mappingVersion;
  // announceSample() commits one atomic Mapping block.  A single-item
  // telemetry sample therefore uses capacity 1 instead of padding 31
  // tombstones on every 20 Hz update.
  definition.mappingBlockCapacity = 1;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 512;
  definition.maxNameReservations = 8192;
  definition.maxPendingInterests = 128;
  definition.samplePeriodMs = UAV_TELEMETRY_PERIOD_MS;
  definition.sampleClasses = {
    ndn_service_framework::SampleClassProfile::bounded("compact-state", 1, 1, 8, 0),
  };
  definition.fec = ndn_service_framework::LiveStreamFecOptions::none();
  return definition;
}

ndn_service_framework::LiveStreamDefinition
makeUavAcousticStreamDefinition(const ndn::Name& provider,
                                uint64_t sessionEpoch,
                                uint64_t mappingVersion)
{
  ndn_service_framework::LiveStreamDefinition definition;
  definition.contractVersion =
    ndn_service_framework::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
  definition.streamId = "uav-acoustic-" + std::to_string(sessionEpoch);
  definition.provider = provider;
  definition.semanticDataPrefix = ndn::Name(provider).append("sensor")
    .append("opaque-block").append(definition.streamId).appendVersion(mappingVersion);
  definition.sessionEpoch = sessionEpoch;
  definition.mappingVersion = mappingVersion;
  // One acoustic block contains at most four sources plus two repair symbols.
  definition.mappingBlockCapacity = 6;
  definition.mappingAheadBlocks = 4;
  definition.retainedItems = 1024;
  definition.maxNameReservations = 32766;
  definition.maxPendingInterests = 256;
  definition.samplePeriodMs = UAV_ACOUSTIC_BLOCK_PERIOD_MS;
  definition.sampleClasses = {
    ndn_service_framework::SampleClassProfile::bounded("opaque-block-2", 2, 2, 16, 0),
    ndn_service_framework::SampleClassProfile::bounded("opaque-block-3", 3, 3, 16, 0),
    ndn_service_framework::SampleClassProfile::bounded("opaque-block-4", 4, 4, 16, 0),
  };
  definition.fec =
    ndn_service_framework::LiveStreamFecOptions::gf256TwoRepair(4, 600, 500);
  return definition;
}

} // namespace ndnsf::examples::uav
