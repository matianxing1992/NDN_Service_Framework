#include "UavProtocol.hpp"
#include "UavNames.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

double
missionDistanceSq(const MissionWaypoint& a, const MissionWaypoint& b, double referenceLat);

ndn_service_framework::ServiceProvider::ServiceOperationStatus
makeUavOperationStatus(const std::string& operationId,
                       const std::string& operation,
                       const ndn::Name& serviceName,
                       const ndn::Name& providerName,
                       const ndn::Name& requestId,
                       const std::string& state,
                       const std::string& reasonCode,
                       const std::string& message,
                       double progress,
                       uint64_t updatedMs);

double
missionStateProgress(const MissionState& mission);

double
missionProgressFraction(const MissionProgressState& progress);

struct DeterministicIndexedWaypoint
{
  MissionWaypoint point;
  size_t index = 0;
};

std::vector<DeterministicIndexedWaypoint>
canonicalizeRouteWaypoints(const std::vector<MissionWaypoint>& routeWaypoints)
{
  std::vector<DeterministicIndexedWaypoint> indexed;
  indexed.reserve(routeWaypoints.size());
  for (size_t i = 0; i < routeWaypoints.size(); ++i) {
    indexed.push_back({routeWaypoints[i], i});
  }

  std::sort(indexed.begin(), indexed.end(),
            [] (const DeterministicIndexedWaypoint& a, const DeterministicIndexedWaypoint& b) {
    if (a.point.lat != b.point.lat) {
      return a.point.lat < b.point.lat;
    }
    if (a.point.lon != b.point.lon) {
      return a.point.lon < b.point.lon;
    }
    return a.index < b.index;
  });
  return indexed;
}

std::vector<std::vector<MissionWaypoint>>
clusterPatrolWaypointsDeterministic(const std::vector<MissionWaypoint>& routeWaypoints,
                                   size_t clusterCount,
                                   double referenceLat)
{
  if (routeWaypoints.empty() || clusterCount == 0) {
    return {};
  }

  auto canonical = canonicalizeRouteWaypoints(routeWaypoints);
  std::vector<MissionWaypoint> sortedRoute;
  sortedRoute.reserve(canonical.size());
  for (const auto& wp : canonical) {
    sortedRoute.push_back(wp.point);
  }

  std::vector<MissionWaypoint> centers;
  centers.reserve(clusterCount);
  for (size_t i = 0; i < clusterCount; ++i) {
    const size_t index = std::min(sortedRoute.size() - 1, i * sortedRoute.size() / clusterCount);
    centers.push_back(sortedRoute[index]);
  }

  std::vector<size_t> assignments(routeWaypoints.size(), 0);
  for (int iteration = 0; iteration < 8; ++iteration) {
    std::vector<std::vector<MissionWaypoint>> groups(clusterCount);
    for (size_t pointIndex = 0; pointIndex < sortedRoute.size(); ++pointIndex) {
      size_t best = 0;
      double bestDistance = missionDistanceSq(sortedRoute[pointIndex], centers.front(), referenceLat);
      for (size_t centerIndex = 1; centerIndex < centers.size(); ++centerIndex) {
        const double candidateDistance =
          missionDistanceSq(sortedRoute[pointIndex], centers[centerIndex], referenceLat);
        if (candidateDistance < bestDistance) {
          best = centerIndex;
          bestDistance = candidateDistance;
        }
      }
      assignments[pointIndex] = best;
      groups[best].push_back(sortedRoute[pointIndex]);
    }
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
      if (groups[groupIndex].empty()) {
        continue;
      }
      MissionWaypoint nextCenter{};
      for (const auto& point : groups[groupIndex]) {
        nextCenter.lat += point.lat;
        nextCenter.lon += point.lon;
      }
      nextCenter.lat /= static_cast<double>(groups[groupIndex].size());
      nextCenter.lon /= static_cast<double>(groups[groupIndex].size());
      centers[groupIndex] = nextCenter;
    }
  }

  std::vector<std::vector<MissionWaypoint>> groups(clusterCount);
  for (size_t pointIndex = 0; pointIndex < sortedRoute.size(); ++pointIndex) {
    groups[assignments[pointIndex]].push_back(sortedRoute[pointIndex]);
  }
  return groups;
}

std::string
trimConfigText(std::string text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

void
assignConfigValue(UavRuntimeConfig& config, const std::string& key, const std::string& value)
{
  if (key == "group-prefix") {
    config.groupPrefix = ndn::Name(value);
  }
  else if (key == "controller-prefix") {
    config.controllerPrefix = ndn::Name(value);
  }
  else if (key == "ground-station-identity") {
    config.groundStationIdentity = ndn::Name(value);
  }
  else if (key == "drone-prefix") {
    config.droneIdentityPrefix = ndn::Name(value);
  }
  else if (key == "trust-schema") {
    config.trustSchema = value;
  }
  else if (key == "service-mavlink-execute") {
    config.serviceMavlinkExecute = ndn::Name(value);
  }
  else if (key == "service-mission-assign") {
    config.serviceMissionAssign = ndn::Name(value);
  }
  else if (key == "service-telemetry-status") {
    config.serviceTelemetryStatus = ndn::Name(value);
  }
  else if (key == "service-camera-frame") {
    config.serviceCameraFrame = ndn::Name(value);
  }
  else if (key == "service-camera-video-control-suffix") {
    config.serviceCameraVideoControlSuffix = ndn::Name(value);
  }
  else if (key == "service-camera-recording-manifest-suffix") {
    config.serviceCameraRecordingManifestSuffix = ndn::Name(value);
  }
  else if (key == "service-camera-repo-catalog-suffix") {
    config.serviceCameraRepoCatalogSuffix = ndn::Name(value);
  }
  else if (key == "service-mavlink-parameters-suffix") {
    config.serviceMavlinkParametersSuffix = ndn::Name(value);
  }
  else if (key == "service-mavlink-parameter-edit-suffix") {
    config.serviceMavlinkParameterEditSuffix = ndn::Name(value);
  }
  else if (key == "service-mavlink-analyze-snapshot-suffix") {
    config.serviceMavlinkAnalyzeSnapshotSuffix = ndn::Name(value);
  }
  else if (key == "service-preflight-checklist-suffix") {
    config.servicePreflightChecklistSuffix = ndn::Name(value);
  }
  else if (key == "service-gs-object-detection") {
    config.serviceGsObjectDetection = ndn::Name(value);
  }
  else if (key == "service-gs-operator-authority-lease") {
    config.serviceGsOperatorAuthorityLease = ndn::Name(value);
  }
  else if (key == "service-gs-operator-authority-revocation") {
    config.serviceGsOperatorAuthorityRevocation = ndn::Name(value);
  }
  else if (key == "service-gs-operator-authority-audit") {
    config.serviceGsOperatorAuthorityAudit = ndn::Name(value);
  }
  else if (key == "ground-station-map-lat") {
    try {
      config.groundStationMapLat = std::stod(value);
    }
    catch (const std::exception&) {
      config.groundStationMapLat = std::numeric_limits<double>::quiet_NaN();
    }
  }
  else if (key == "ground-station-map-lon") {
    try {
      config.groundStationMapLon = std::stod(value);
    }
    catch (const std::exception&) {
      config.groundStationMapLon = std::numeric_limits<double>::quiet_NaN();
    }
  }
}

uint64_t
uint64FieldOr(const Fields& fields, const std::string& key, uint64_t fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  try {
    return std::stoull(it->second);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

} // namespace

Fields
loadKeyValueConfig(const std::string& path)
{
  Fields fields;
  if (path.empty()) {
    return fields;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open UAV config: " + path);
  }

  std::string line;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    line = trimConfigText(line);
    if (line.empty()) {
      continue;
    }

    std::string key;
    std::string value;
    const auto equal = line.find('=');
    if (equal != std::string::npos) {
      key = trimConfigText(line.substr(0, equal));
      value = trimConfigText(line.substr(equal + 1));
    }
    else {
      std::istringstream is(line);
      is >> key;
      std::getline(is, value);
      value = trimConfigText(value);
    }
    if (!key.empty() && !value.empty()) {
      fields[key] = value;
    }
  }
  return fields;
}

UavRuntimeConfig
loadUavRuntimeConfig(const std::string& path)
{
  UavRuntimeConfig config;
  if (path.empty()) {
    return config;
  }

  for (const auto& [key, value] : loadKeyValueConfig(path)) {
    assignConfigValue(config, key, value);
  }

  return config;
}

ndn::Name
droneIdentity(const std::string& droneId)
{
  return droneIdentity(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneIdentity(const UavRuntimeConfig& config, const std::string& droneId)
{
  if (droneId.empty()) {
    return config.droneIdentityPrefix;
  }
  return ndn::Name(config.droneIdentityPrefix).append(droneId);
}

ndn::Name
droneVideoControlService(const std::string& droneId)
{
  return droneVideoControlService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneVideoControlService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraVideoControlSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneCameraRecordingManifestService(const std::string& droneId)
{
  return droneCameraRecordingManifestService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneCameraRecordingManifestService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraRecordingManifestSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneCameraRepoCatalogService(const std::string& droneId)
{
  return droneCameraRepoCatalogService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneCameraRepoCatalogService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraRepoCatalogSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneMavlinkParametersService(const std::string& droneId)
{
  return droneMavlinkParametersService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneMavlinkParametersService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceMavlinkParametersSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneMavlinkParameterEditService(const std::string& droneId)
{
  return droneMavlinkParameterEditService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneMavlinkParameterEditService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceMavlinkParameterEditSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
droneMavlinkAnalyzeSnapshotService(const std::string& droneId)
{
  return droneMavlinkAnalyzeSnapshotService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneMavlinkAnalyzeSnapshotService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceMavlinkAnalyzeSnapshotSuffix) {
    service.append(component);
  }
  return service;
}

ndn::Name
dronePreflightChecklistService(const std::string& droneId)
{
  return dronePreflightChecklistService(UavRuntimeConfig{}, droneId);
}

ndn::Name
dronePreflightChecklistService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.servicePreflightChecklistSuffix) {
    service.append(component);
  }
  return service;
}

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
encodeFields(const Fields& fields)
{
  std::ostringstream os;
  bool first = true;
  for (const auto& [key, value] : fields) {
    if (!first) {
      os << ';';
    }
    first = false;
    os << key << '=';
    for (const char ch : value) {
      if (ch == '%' || ch == ';' || ch == '=') {
        os << '%' << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch))
           << std::dec << std::nouppercase;
      }
      else {
        os << ch;
      }
    }
  }
  return os.str();
}

Fields
decodeFields(const std::string& payload)
{
  Fields fields;
  size_t start = 0;
  while (start <= payload.size()) {
    const auto end = payload.find(';', start);
    const auto part = payload.substr(start, end == std::string::npos ? end : end - start);
    if (!part.empty()) {
      const auto equal = part.find('=');
      if (equal != std::string::npos) {
        std::string value;
        for (size_t i = equal + 1; i < part.size(); ++i) {
          if (part[i] == '%' && i + 2 < part.size()) {
            const auto byte = std::stoi(part.substr(i + 1, 2), nullptr, 16);
            value.push_back(static_cast<char>(byte));
            i += 2;
          }
          else {
            value.push_back(part[i]);
          }
        }
        fields[part.substr(0, equal)] = value;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return fields;
}

namespace {

const std::set<std::string>&
videoStreamDescriptorRequiredFields()
{
  static const std::set<std::string> fields{
    "data_prefix",
    "key_epoch",
    "latest_join_cursor",
    "latest_produced_cursor",
    "mapping_anchor_block",
    "mapping_anchor_content_digest",
    "mapping_block_capacity",
    "mapping_committed_through_cursor",
    "mapping_root",
    "mapping_version",
    "max_name_reservations",
    "next_reserved_cursor",
    "nonce_salt_hex",
    "oldest_retained_cursor",
    "prefetch_eligibility",
    "sample_period_ms",
    "sample_unit",
    "stream_cipher",
    "stream_contract_version",
    "stream_id",
    "stream_key_hex",
    "stream_session_epoch",
  };
  return fields;
}

bool
isCanonicalDescriptorKey(const std::string& key)
{
  if (key.empty()) {
    return false;
  }
  return std::all_of(key.begin(), key.end(), [] (unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
  });
}

int
uppercaseHexValue(char ch)
{
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

Fields
decodeCanonicalDescriptorFields(const std::string& payload)
{
  if (payload.empty()) {
    throw std::invalid_argument("empty UAV stream descriptor");
  }

  Fields fields;
  std::string previousKey;
  size_t start = 0;
  while (start < payload.size()) {
    const auto end = payload.find(';', start);
    const auto partEnd = end == std::string::npos ? payload.size() : end;
    if (partEnd == start) {
      throw std::invalid_argument("empty UAV stream descriptor field");
    }

    const auto equal = payload.find('=', start);
    if (equal == std::string::npos || equal >= partEnd || equal == start) {
      throw std::invalid_argument("malformed UAV stream descriptor field");
    }
    const auto key = payload.substr(start, equal - start);
    if (!isCanonicalDescriptorKey(key) ||
        (!previousKey.empty() && key <= previousKey)) {
      throw std::invalid_argument("non-canonical UAV stream descriptor key order");
    }

    std::string value;
    value.reserve(partEnd - equal - 1);
    for (size_t i = equal + 1; i < partEnd; ++i) {
      const char ch = payload[i];
      if (ch == '=') {
        throw std::invalid_argument("unescaped UAV stream descriptor delimiter");
      }
      if (ch != '%') {
        value.push_back(ch);
        continue;
      }
      if (i + 2 >= partEnd) {
        throw std::invalid_argument("truncated UAV stream descriptor escape");
      }
      const int high = uppercaseHexValue(payload[i + 1]);
      const int low = uppercaseHexValue(payload[i + 2]);
      if (high < 0 || low < 0) {
        throw std::invalid_argument("non-canonical UAV stream descriptor escape");
      }
      const char decoded = static_cast<char>((high << 4) | low);
      if (decoded != '%' && decoded != ';' && decoded != '=') {
        throw std::invalid_argument("unnecessary UAV stream descriptor escape");
      }
      value.push_back(decoded);
      i += 2;
    }

    if (!fields.emplace(key, std::move(value)).second) {
      throw std::invalid_argument("duplicate UAV stream descriptor field");
    }
    previousKey = key;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
    if (start == payload.size()) {
      throw std::invalid_argument("trailing UAV stream descriptor delimiter");
    }
  }

  if (encodeFields(fields) != payload) {
    throw std::invalid_argument("non-canonical UAV stream descriptor encoding");
  }
  return fields;
}

const std::string&
requiredDescriptorField(const Fields& fields, const std::string& key)
{
  const auto it = fields.find(key);
  if (it == fields.end()) {
    throw std::invalid_argument("missing UAV stream descriptor field: " + key);
  }
  return it->second;
}

uint64_t
parseCanonicalUint64(const Fields& fields, const std::string& key)
{
  const auto& value = requiredDescriptorField(fields, key);
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    throw std::invalid_argument("non-canonical UAV stream descriptor integer: " + key);
  }
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("invalid UAV stream descriptor integer: " + key);
  }
  return result;
}

bool
isLowerHex(const std::string& value)
{
  return std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

ndn::Buffer
decodeFixedLowerHex(const Fields& fields, const std::string& key, size_t bytes)
{
  const auto& value = requiredDescriptorField(fields, key);
  if (value.size() != bytes * 2 || !isLowerHex(value)) {
    throw std::invalid_argument("invalid UAV stream descriptor hex field: " + key);
  }
  ndn::Buffer decoded(bytes);
  for (size_t i = 0; i < bytes; ++i) {
    auto digit = [] (char ch) -> uint8_t {
      return static_cast<uint8_t>(ch >= 'a' ? ch - 'a' + 10 : ch - '0');
    };
    decoded[i] = static_cast<uint8_t>((digit(value[2 * i]) << 4) |
                                      digit(value[2 * i + 1]));
  }
  return decoded;
}

template<typename Range>
std::string
encodeLowerHex(const Range& value)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const auto byte : value) {
    encoded.push_back(digits[(static_cast<uint8_t>(byte) >> 4) & 0x0f]);
    encoded.push_back(digits[static_cast<uint8_t>(byte) & 0x0f]);
  }
  return encoded;
}

ndn::Name
parseCanonicalName(const Fields& fields, const std::string& key)
{
  const auto& value = requiredDescriptorField(fields, key);
  try {
    ndn::Name name(value);
    if (name.empty() || name.toUri() != value) {
      throw std::invalid_argument("non-canonical UAV stream descriptor name: " + key);
    }
    return name;
  }
  catch (const std::invalid_argument&) {
    throw;
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid UAV stream descriptor name: " + key);
  }
}

bool
isCanonicalStreamId(const std::string& streamId)
{
  static const std::string prefix = "stream-";
  return streamId.size() == prefix.size() + 32 &&
         streamId.compare(0, prefix.size(), prefix) == 0 &&
         isLowerHex(streamId.substr(prefix.size()));
}

void
validateVideoStreamSessionContext(const VideoStreamDescriptor& descriptor)
{
  using namespace ndn_service_framework;

  if (descriptor.contractVersion != 2 ||
      descriptor.providerIdentity.empty() || descriptor.serviceName.empty() ||
      !isCanonicalStreamId(descriptor.streamId) ||
      descriptor.sessionEpoch == 0 || descriptor.mappingVersion == 0 ||
      descriptor.keyEpoch == 0) {
    throw std::invalid_argument("invalid UAV stream descriptor identity context");
  }
  if (descriptor.mappingRoot !=
      makeStreamNameMapRoot(descriptor.providerIdentity, descriptor.streamId)) {
    throw std::invalid_argument("invalid UAV stream Mapping root");
  }
  if (!descriptor.providerIdentity.isPrefixOf(descriptor.dataPrefix) ||
      descriptor.dataPrefix.size() != descriptor.providerIdentity.size() + 4) {
    throw std::invalid_argument("invalid UAV stream payload prefix");
  }
  const auto videoIndex = descriptor.providerIdentity.size();
  const auto& video = descriptor.dataPrefix.get(videoIndex);
  const auto& camera = descriptor.dataPrefix.get(videoIndex + 1);
  const auto& stream = descriptor.dataPrefix.get(videoIndex + 2);
  const auto& version = descriptor.dataPrefix.get(videoIndex + 3);
  if (!video.isGeneric() || video.toUri() != "video" ||
      !camera.isGeneric() || camera.value_size() == 0 ||
      !stream.isGeneric() || stream.toUri() != descriptor.streamId ||
      !version.isVersion() || version.toVersion() != descriptor.mappingVersion) {
    throw std::invalid_argument("invalid UAV stream semantic payload prefix");
  }
  if (descriptor.mappingBlockCapacity == 0 ||
      descriptor.mappingBlockCapacity > STREAM_NAME_MAP_MAX_BLOCK_CAPACITY) {
    throw std::invalid_argument("invalid UAV stream Mapping block capacity");
  }
  if (descriptor.maxNameReservations == 0 ||
      descriptor.maxNameReservations > STREAM_NAME_MAP_MAX_REVERSE_ENTRIES ||
      descriptor.maxNameReservations < descriptor.mappingBlockCapacity) {
    throw std::invalid_argument("invalid UAV stream name-reservation limit");
  }
  if (descriptor.sampleUnit != "fec-group" || descriptor.samplePeriodMs == 0 ||
      (descriptor.prefetchEligibility != "ahead-mapped" &&
       descriptor.prefetchEligibility != "retrieval-only" &&
       descriptor.prefetchEligibility != "predictive-sequential") ||
      descriptor.cipher != "aes-256-gcm" || descriptor.streamKey.size() != 32 ||
      descriptor.nonceSalt.size() != 4) {
    throw std::invalid_argument("invalid UAV stream crypto or prefetch contract");
  }
  for (const auto& [key, value] : descriptor.extensions) {
    (void)value;
    if (!isCanonicalDescriptorKey(key) ||
        videoStreamDescriptorRequiredFields().count(key) != 0) {
      throw std::invalid_argument("invalid UAV stream descriptor extension key");
    }
  }
}

void
validateVideoStreamDescriptor(const VideoStreamDescriptor& descriptor)
{
  validateVideoStreamSessionContext(descriptor);
  if (const auto error = descriptor.frontiers.validate(
        descriptor.mappingBlockCapacity, descriptor.mappingAnchorBlock)) {
    throw std::invalid_argument("invalid UAV stream cursor frontiers: " + *error);
  }
  const auto retainedBlockCount =
    descriptor.frontiers.mappingCommittedThrough / descriptor.mappingBlockCapacity -
    descriptor.frontiers.oldestRetained / descriptor.mappingBlockCapacity + 1;
  if (retainedBlockCount >
      descriptor.maxNameReservations / descriptor.mappingBlockCapacity) {
    throw std::invalid_argument("UAV stream retained range exceeds reservation limit");
  }
  if (descriptor.mappingAnchorBlock !=
      descriptor.frontiers.latestJoin / descriptor.mappingBlockCapacity) {
    throw std::invalid_argument("UAV stream Mapping anchor does not cover join cursor");
  }
}

} // namespace

UavH264ReadinessTracker::UavH264ReadinessTracker(uint64_t minimumGroups)
  : m_minimumGroups(minimumGroups)
{
  if (m_minimumGroups < 3) {
    throw std::invalid_argument("UAV stream readiness requires at least three groups");
  }
}

void
UavH264ReadinessTracker::reset()
{
  m_completedGroups = 0;
  m_previousPublishedMs = 0;
  m_periodSumMs = 0;
  m_periodSamples = 0;
  m_latestProducedCursor = 0;
  m_lastSpsCursor = 0;
  m_lastPpsCursor = 0;
  m_latestJoinCursor = 0;
  m_hasProduced = false;
  m_hasSps = false;
  m_hasPps = false;
  m_hasIdrJoin = false;
  m_annexBTail.clear();
}

void
UavH264ReadinessTracker::observePublicationGroup(
  uint64_t firstCursor,
  uint64_t lastCursor,
  uint64_t publishedMonotonicMs,
  const std::vector<uint8_t>& annexBBytes)
{
  if (firstCursor > lastCursor || publishedMonotonicMs == 0 ||
      (m_hasProduced && firstCursor <= m_latestProducedCursor) ||
      (m_previousPublishedMs != 0 && publishedMonotonicMs <= m_previousPublishedMs)) {
    throw std::invalid_argument("stale or invalid UAV publication-group observation");
  }

  if (m_previousPublishedMs != 0) {
    const auto period = publishedMonotonicMs - m_previousPublishedMs;
    if (m_periodSumMs > std::numeric_limits<uint64_t>::max() - period) {
      throw std::overflow_error("UAV publication-period accumulator overflow");
    }
    m_periodSumMs += period;
    ++m_periodSamples;
  }
  m_previousPublishedMs = publishedMonotonicMs;
  ++m_completedGroups;
  m_latestProducedCursor = lastCursor;
  m_hasProduced = true;

  std::vector<uint8_t> scan;
  scan.reserve(m_annexBTail.size() + annexBBytes.size());
  scan.insert(scan.end(), m_annexBTail.begin(), m_annexBTail.end());
  scan.insert(scan.end(), annexBBytes.begin(), annexBBytes.end());
  for (size_t i = 0; i + 3 < scan.size(); ++i) {
    size_t header = 0;
    if (scan[i] == 0 && scan[i + 1] == 0 && scan[i + 2] == 1) {
      header = i + 3;
    }
    else if (i + 4 < scan.size() && scan[i] == 0 && scan[i + 1] == 0 &&
             scan[i + 2] == 0 && scan[i + 3] == 1) {
      header = i + 4;
    }
    if (header == 0 || header >= scan.size()) {
      continue;
    }
    const auto nalType = static_cast<uint8_t>(scan[header] & 0x1f);
    if (nalType == 7) {
      m_hasSps = true;
      m_lastSpsCursor = firstCursor;
    }
    else if (nalType == 8) {
      m_hasPps = true;
      m_lastPpsCursor = firstCursor;
    }
    else if (nalType == 5 && m_hasSps && m_hasPps) {
      m_hasIdrJoin = true;
      m_latestJoinCursor = std::min({m_lastSpsCursor, m_lastPpsCursor, firstCursor});
    }
  }

  const auto tailSize = std::min<size_t>(4, scan.size());
  m_annexBTail.assign(scan.end() - tailSize, scan.end());
}

bool
UavH264ReadinessTracker::ready() const
{
  return m_completedGroups >= m_minimumGroups && m_periodSamples != 0 && m_hasIdrJoin;
}

uint64_t
sourceMediaSequenceForJoinCursor(ndn_service_framework::StreamCursor cursor,
                                 uint32_t dataShards,
                                 uint32_t parityShards)
{
  if (dataShards == 0) {
    throw std::invalid_argument("UAV media join requires source shards");
  }
  const uint64_t groupSize = static_cast<uint64_t>(dataShards) + parityShards;
  if (groupSize == 0) {
    throw std::overflow_error("UAV media join group size overflow");
  }
  const auto group = cursor / groupSize;
  const auto symbol = cursor % groupSize;
  if (group > std::numeric_limits<uint64_t>::max() / dataShards) {
    throw std::overflow_error("UAV media join sequence overflow");
  }
  const auto groupFirstSource = group * dataShards;
  if (symbol < dataShards) {
    return groupFirstSource + symbol;
  }
  if (groupFirstSource > std::numeric_limits<uint64_t>::max() - dataShards) {
    throw std::overflow_error("UAV media join repair skip overflow");
  }
  return groupFirstSource + dataShards;
}

// ── Predictive Stream Helpers (Spec 148) ──

ndn::Name
makeUavPredictiveSessionPrefix(const std::string& droneId, uint64_t epoch)
{
  ndn::Name prefix("/example/uav/drone");
  prefix.append(droneId);
  prefix.append("video");
  prefix.append("v").appendNumber(epoch);
  return prefix;
}

ndn::Name
makeUavPredictiveMappingName(const ndn::Name& sessionPrefix,
                              uint64_t mappingVersion,
                              uint64_t cursor)
{
  ndn::Name name(sessionPrefix);
  name.append("v").appendNumber(mappingVersion);
  name.appendSequenceNumber(cursor);
  return name;
}

std::shared_ptr<ndn::Data>
uavVideoPacketToSignedData(const VideoPacket& packet,
                            const ndn::Name& sessionPrefix,
                            uint64_t mappingVersion,
                            ndn::KeyChain& keyChain,
                            const ndn::security::SigningInfo& signingInfo)
{
  auto data = std::make_shared<ndn::Data>(
    makeUavPredictiveMappingName(sessionPrefix, mappingVersion, packet.packetSeq));
  data->setFreshnessPeriod(ndn::time::milliseconds(1000));
  data->setContent(ndn::span<const uint8_t>(packet.payload.data(),
                                              packet.payload.size()));
  keyChain.sign(*data, signingInfo);
  return data;
}

uint64_t
UavH264ReadinessTracker::completedGroups() const
{
  return m_completedGroups;
}

uint64_t
UavH264ReadinessTracker::samplePeriodMs() const
{
  if (m_periodSamples == 0) {
    return 0;
  }
  return std::max<uint64_t>(1, (m_periodSumMs + m_periodSamples / 2) / m_periodSamples);
}

uint64_t
UavH264ReadinessTracker::latestJoinCursor() const
{
  if (!m_hasIdrJoin) {
    throw std::logic_error("UAV stream has no decoder-safe join cursor");
  }
  return m_latestJoinCursor;
}

uint64_t
UavH264ReadinessTracker::latestProducedCursor() const
{
  if (!m_hasProduced) {
    throw std::logic_error("UAV stream has no produced cursor");
  }
  return m_latestProducedCursor;
}

std::string
UavH264ReadinessTracker::reason() const
{
  if (m_completedGroups < m_minimumGroups) {
    return "waiting-publication-groups";
  }
  if (!m_hasSps) {
    return "waiting-sps";
  }
  if (!m_hasPps) {
    return "waiting-pps";
  }
  if (!m_hasIdrJoin) {
    return "waiting-idr";
  }
  if (m_periodSamples == 0) {
    return "waiting-sample-period";
  }
  return "ready";
}

std::string
encodeVideoStreamDescriptor(const VideoStreamDescriptor& descriptor)
{
  validateVideoStreamDescriptor(descriptor);
  Fields fields = descriptor.extensions;
  fields["data_prefix"] = descriptor.dataPrefix.toUri();
  fields["key_epoch"] = std::to_string(descriptor.keyEpoch);
  fields["latest_join_cursor"] = std::to_string(descriptor.frontiers.latestJoin);
  fields["latest_produced_cursor"] = std::to_string(descriptor.frontiers.latestProduced);
  fields["mapping_anchor_block"] = std::to_string(descriptor.mappingAnchorBlock);
  fields["mapping_anchor_content_digest"] =
    encodeLowerHex(descriptor.mappingAnchorContentDigest);
  fields["mapping_block_capacity"] = std::to_string(descriptor.mappingBlockCapacity);
  fields["mapping_committed_through_cursor"] =
    std::to_string(descriptor.frontiers.mappingCommittedThrough);
  fields["mapping_root"] = descriptor.mappingRoot.toUri();
  fields["mapping_version"] = std::to_string(descriptor.mappingVersion);
  fields["max_name_reservations"] = std::to_string(descriptor.maxNameReservations);
  fields["next_reserved_cursor"] = std::to_string(descriptor.frontiers.nextReserved);
  fields["nonce_salt_hex"] = encodeLowerHex(descriptor.nonceSalt);
  fields["oldest_retained_cursor"] = std::to_string(descriptor.frontiers.oldestRetained);
  fields["prefetch_eligibility"] = descriptor.prefetchEligibility;
  fields["sample_period_ms"] = std::to_string(descriptor.samplePeriodMs);
  fields["sample_unit"] = descriptor.sampleUnit;
  fields["stream_cipher"] = descriptor.cipher;
  fields["stream_contract_version"] = std::to_string(descriptor.contractVersion);
  fields["stream_id"] = descriptor.streamId;
  fields["stream_key_hex"] = encodeLowerHex(descriptor.streamKey);
  fields["stream_session_epoch"] = std::to_string(descriptor.sessionEpoch);
  return encodeFields(fields);
}

ndn_service_framework::LiveStreamDescriptor
toCoreLiveStreamDescriptor(const VideoStreamDescriptor& descriptor)
{
  validateVideoStreamDescriptor(descriptor);
  ndn_service_framework::LiveStreamDescriptor core;
  core.definition.contractVersion = descriptor.contractVersion;
  core.definition.streamId = descriptor.streamId;
  core.definition.provider = descriptor.providerIdentity;
  core.definition.semanticDataPrefix = descriptor.dataPrefix;
  core.definition.sessionEpoch = descriptor.sessionEpoch;
  core.definition.mappingVersion = descriptor.mappingVersion;
  core.definition.mappingBlockCapacity = descriptor.mappingBlockCapacity;
  core.definition.mappingAheadBlocks = 4;
  const auto retainedSpan = descriptor.frontiers.latestProduced >=
                              descriptor.frontiers.oldestRetained ?
    descriptor.frontiers.latestProduced - descriptor.frontiers.oldestRetained + 1 : 0;
  const auto roundedRetainedSpan = retainedSpan == 0 ? size_t{0} :
    static_cast<size_t>(((retainedSpan + descriptor.mappingBlockCapacity - 1) /
                         descriptor.mappingBlockCapacity) *
                        descriptor.mappingBlockCapacity);
  core.definition.retainedItems = std::min<size_t>(
    descriptor.maxNameReservations,
    std::max<size_t>(std::min<size_t>(600, descriptor.maxNameReservations),
                     roundedRetainedSpan));
  core.definition.maxNameReservations = descriptor.maxNameReservations;
  core.definition.maxPendingInterests = 256;
  core.definition.samplePeriodMs = static_cast<double>(descriptor.samplePeriodMs);
  const auto dataShards = std::stoull(fieldOr(descriptor.extensions, "fec_data_shards", "0"));
  const auto parityShards = std::stoull(fieldOr(descriptor.extensions, "fec_parity_shards", "0"));
  if (parityShards == 1 && dataShards >= 2) {
    core.definition.fec = ndn_service_framework::LiveStreamFecOptions::xorOneRepair(
      static_cast<size_t>(dataShards), 7000, 500);
  }
  if (descriptor.contractVersion == 2) {
    const auto classMode = fieldOr(
      descriptor.extensions, "sample_class_mode", "exact-key-delta");
    const auto keySeed = std::stoull(fieldOr(
      descriptor.extensions, "sample_class_key_seed", std::to_string(dataShards)));
    const auto deltaSeed = std::stoull(fieldOr(
      descriptor.extensions, "sample_class_delta_seed",
      std::to_string(std::min<uint64_t>(3, dataShards))));
    if (classMode == "bounded-opaque") {
      const auto opaqueSeed = std::stoull(fieldOr(
        descriptor.extensions, "sample_class_opaque_seed",
        std::to_string(dataShards)));
      core.definition.sampleClasses = {
        ndn_service_framework::SampleClassProfile::bounded(
          "opaque", static_cast<size_t>(opaqueSeed),
          static_cast<size_t>(dataShards), 32, 0),
      };
    }
    else if (classMode == "exact-key-delta") {
      core.definition.sampleClasses = {
        ndn_service_framework::SampleClassProfile::bounded(
          "key", static_cast<size_t>(keySeed),
          static_cast<size_t>(dataShards), 32, 0),
        ndn_service_framework::SampleClassProfile::bounded(
          "delta", static_cast<size_t>(deltaSeed),
          static_cast<size_t>(dataShards), 32, 0),
      };
    }
    else {
      throw std::invalid_argument("unsupported UAV video sample-class mode");
    }
  }
  core.checkpoint.frontiers = descriptor.frontiers;
  core.checkpoint.blockNumber = descriptor.mappingAnchorBlock;
  core.checkpoint.contentDigest = descriptor.mappingAnchorContentDigest;
  core.measuredSamplePeriodMs = descriptor.samplePeriodMs;
  core.safeJoinCursor = descriptor.frontiers.latestJoin;
  if (const auto error = core.validate()) {
    throw std::invalid_argument("invalid UAV-to-Core LiveStream projection: " + *error);
  }
  return core;
}

ndn_service_framework::PredictiveStreamDescriptor
toCorePredictiveStreamDescriptor(const VideoStreamDescriptor& descriptor)
{
  const auto legacyProjection = toCoreLiveStreamDescriptor(descriptor);
  ndn_service_framework::PredictiveStreamDescriptor core;
  core.definition = legacyProjection.definition;
  core.checkpoint.initialSampleId = descriptor.frontiers.latestJoin;
  core.checkpoint.oldestRetainedSampleId =
    descriptor.frontiers.oldestRetained;
  core.checkpoint.latestProducedSampleId =
    descriptor.frontiers.latestProduced;
  core.checkpoint.nextExpectedSampleId =
    std::max(descriptor.frontiers.nextReserved,
             descriptor.frontiers.latestProduced + 1);
  core.frontierName = ndn_service_framework::makePredictiveFrontierName(
    core.definition.mappingRoot());
  core.measuredSamplePeriodMs =
    static_cast<double>(descriptor.samplePeriodMs);
  if (const auto error = core.validate()) {
    throw std::invalid_argument(
      "invalid UAV-to-Core predictive projection: " + *error);
  }
  return core;
}

void
applyCoreLiveStreamDescriptor(
  VideoStreamDescriptor& descriptor,
  const ndn_service_framework::LiveStreamDescriptor& core)
{
  if (const auto error = core.validate()) {
    throw std::invalid_argument("invalid Core LiveStream descriptor: " + *error);
  }
  if (core.definition.streamId != descriptor.streamId ||
      core.definition.contractVersion != descriptor.contractVersion ||
      core.definition.provider != descriptor.providerIdentity ||
      core.definition.semanticDataPrefix != descriptor.dataPrefix ||
      core.definition.sessionEpoch != descriptor.sessionEpoch ||
      core.definition.mappingVersion != descriptor.mappingVersion ||
      core.definition.mappingBlockCapacity != descriptor.mappingBlockCapacity) {
    throw std::invalid_argument("Core/UAV LiveStream descriptor binding mismatch");
  }
  descriptor.mappingRoot = core.definition.mappingRoot();
  descriptor.mappingAnchorBlock = core.checkpoint.blockNumber;
  descriptor.mappingAnchorContentDigest = core.checkpoint.contentDigest;
  descriptor.samplePeriodMs = static_cast<uint64_t>(std::max(1.0,
    std::round(core.measuredSamplePeriodMs)));
  descriptor.frontiers = core.checkpoint.frontiers;
}

void
applyCorePredictiveStreamDescriptor(
  VideoStreamDescriptor& descriptor,
  const ndn_service_framework::PredictiveStreamDescriptor& core)
{
  if (const auto error = core.validate()) {
    throw std::invalid_argument(
      "invalid Core predictive descriptor: " + *error);
  }
  if (core.definition.streamId != descriptor.streamId ||
      core.definition.contractVersion != descriptor.contractVersion ||
      core.definition.provider != descriptor.providerIdentity ||
      core.definition.sessionEpoch != descriptor.sessionEpoch) {
    throw std::invalid_argument(
      "Core/UAV predictive descriptor binding mismatch");
  }

  descriptor.dataPrefix = core.definition.semanticDataPrefix;
  descriptor.mappingRoot = core.definition.mappingRoot();
  descriptor.mappingVersion = core.definition.mappingVersion;
  descriptor.mappingBlockCapacity = core.definition.mappingBlockCapacity;
  descriptor.samplePeriodMs = static_cast<uint64_t>(std::max(
    1.0, std::round(core.measuredSamplePeriodMs)));
  descriptor.frontiers.oldestRetained =
    core.checkpoint.oldestRetainedSampleId;
  descriptor.frontiers.latestJoin = core.checkpoint.initialSampleId;
  descriptor.frontiers.latestProduced =
    core.checkpoint.latestProducedSampleId;
  const auto capacity = descriptor.mappingBlockCapacity;
  descriptor.frontiers.mappingCommittedThrough =
    ((descriptor.frontiers.latestProduced / capacity) + 1) * capacity - 1;
  descriptor.frontiers.nextReserved =
    descriptor.frontiers.mappingCommittedThrough + 1;
  descriptor.mappingAnchorBlock =
    descriptor.frontiers.latestJoin / capacity;
  descriptor.mappingAnchorContentDigest = {};
  descriptor.prefetchEligibility = "predictive-sequential";
}

void
applyCoreLiveStreamStatus(
  VideoStreamDescriptor& descriptor,
  const ndn_service_framework::LiveStreamStatus& status,
  ndn_service_framework::StreamCursor latestProduced)
{
  auto frontiers = status.frontiers;
  frontiers.latestProduced = latestProduced;
  if (const auto error = frontiers.validate(
        descriptor.mappingBlockCapacity, descriptor.mappingAnchorBlock)) {
    throw std::invalid_argument("invalid provisional Core LiveStream frontiers: " + *error);
  }
  descriptor.frontiers = frontiers;
}

VideoStreamDescriptor
decodeVideoStreamDescriptorStrict(const std::string& payload,
                                  const ndn::Name& expectedProvider,
                                  const ndn::Name& verifiedProvider,
                                  const ndn::Name& expectedService,
                                  const ndn::Name& responseService)
{
  if (expectedProvider.empty() || verifiedProvider != expectedProvider) {
    throw std::invalid_argument("UAV stream descriptor Provider binding failed");
  }
  if (expectedService.empty() || responseService != expectedService) {
    throw std::invalid_argument("UAV stream descriptor service binding failed");
  }

  const auto fields = decodeCanonicalDescriptorFields(payload);
  for (const auto& field : videoStreamDescriptorRequiredFields()) {
    requiredDescriptorField(fields, field);
  }

  VideoStreamDescriptor descriptor;
  descriptor.providerIdentity = expectedProvider;
  descriptor.serviceName = expectedService;
  descriptor.contractVersion = parseCanonicalUint64(fields, "stream_contract_version");
  descriptor.streamId = requiredDescriptorField(fields, "stream_id");
  descriptor.sessionEpoch = parseCanonicalUint64(fields, "stream_session_epoch");
  descriptor.dataPrefix = parseCanonicalName(fields, "data_prefix");
  descriptor.mappingRoot = parseCanonicalName(fields, "mapping_root");
  descriptor.mappingVersion = parseCanonicalUint64(fields, "mapping_version");
  descriptor.mappingBlockCapacity =
    parseCanonicalUint64(fields, "mapping_block_capacity");
  const auto maxNameReservations =
    parseCanonicalUint64(fields, "max_name_reservations");
  if (maxNameReservations > std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument("UAV stream name-reservation limit exceeds size_t");
  }
  descriptor.maxNameReservations = static_cast<size_t>(maxNameReservations);
  descriptor.mappingAnchorBlock = parseCanonicalUint64(fields, "mapping_anchor_block");
  const auto anchorDigest = decodeFixedLowerHex(
    fields, "mapping_anchor_content_digest", descriptor.mappingAnchorContentDigest.size());
  std::copy(anchorDigest.begin(), anchorDigest.end(),
            descriptor.mappingAnchorContentDigest.begin());
  descriptor.sampleUnit = requiredDescriptorField(fields, "sample_unit");
  descriptor.samplePeriodMs = parseCanonicalUint64(fields, "sample_period_ms");
  descriptor.frontiers.oldestRetained =
    parseCanonicalUint64(fields, "oldest_retained_cursor");
  descriptor.frontiers.latestJoin =
    parseCanonicalUint64(fields, "latest_join_cursor");
  descriptor.frontiers.latestProduced =
    parseCanonicalUint64(fields, "latest_produced_cursor");
  descriptor.frontiers.mappingCommittedThrough =
    parseCanonicalUint64(fields, "mapping_committed_through_cursor");
  descriptor.frontiers.nextReserved =
    parseCanonicalUint64(fields, "next_reserved_cursor");
  descriptor.prefetchEligibility = requiredDescriptorField(fields, "prefetch_eligibility");
  descriptor.cipher = requiredDescriptorField(fields, "stream_cipher");
  descriptor.keyEpoch = parseCanonicalUint64(fields, "key_epoch");
  descriptor.streamKey = decodeFixedLowerHex(fields, "stream_key_hex", 32);
  descriptor.nonceSalt = decodeFixedLowerHex(fields, "nonce_salt_hex", 4);

  for (const auto& [key, value] : fields) {
    if (videoStreamDescriptorRequiredFields().count(key) == 0) {
      descriptor.extensions.emplace(key, value);
    }
  }
  validateVideoStreamDescriptor(descriptor);
  return descriptor;
}

UavVideoDataName
makeUavVideoDataName(const VideoStreamDescriptor& descriptor,
                     const VideoPacket& packet)
{
  // Payload names and AEAD bindings belong to the immutable stream session.
  // A long-lived producer may legitimately outlive the descriptor's join
  // checkpoint as retention advances, so packet publication must not depend
  // on that historical frontier snapshot remaining current.
  validateVideoStreamSessionContext(descriptor);
  if (packet.streamId != descriptor.streamId ||
      packet.streamSessionEpoch != descriptor.sessionEpoch) {
    throw std::invalid_argument("VideoPacket does not belong to UAV stream descriptor");
  }
  const uint64_t symbolCount = static_cast<uint64_t>(packet.fecDataShards) +
                               packet.fecParityShards;
  if (packet.fecDataShards == 0 || symbolCount == 0 ||
      packet.fecSymbolCount != symbolCount || packet.frameSegmentCount != symbolCount ||
      packet.fecSymbolIndex >= symbolCount ||
      packet.frameSegmentIndex != packet.fecSymbolIndex) {
    throw std::invalid_argument("invalid UAV video FEC-group coordinates");
  }

  UavVideoDataName result;
  result.cursor = packet.packetSeq;
  if (descriptor.prefetchEligibility == "predictive-sequential") {
    if (packet.fecSymbolIndex >= packet.fecDataShards) {
      throw std::invalid_argument(
        "predictive UAV source Data cannot carry an app parity symbol");
    }
    result.name = descriptor.mappingRoot;
    result.name.append("v").appendNumber(descriptor.mappingVersion)
      .appendSequenceNumber(packet.packetSeq);
    result.parity = false;
    return result;
  }
  result.name = descriptor.dataPrefix;
  result.name.append("fec-group").appendSequenceNumber(packet.frameSeq);
  if (packet.fecSymbolIndex < packet.fecDataShards) {
    result.parity = false;
    result.name.append("data").appendSegment(packet.fecSymbolIndex);
    result.finalBlockId = ndn::name::Component::fromSegment(packet.fecDataShards - 1);
  }
  else {
    if (packet.fecParityShards == 0) {
      throw std::invalid_argument("parity symbol requires UAV video parity shards");
    }
    result.parity = true;
    result.name.append("parity").appendSegment(
      packet.fecSymbolIndex - packet.fecDataShards);
    result.finalBlockId = ndn::name::Component::fromSegment(packet.fecParityShards - 1);
  }
  return result;
}

ndn_service_framework::StreamNameMapResolverConfig
makeUavStreamNameMapResolverConfig(const VideoStreamDescriptor& descriptor)
{
  validateVideoStreamDescriptor(descriptor);
  ndn_service_framework::StreamNameMapResolverConfig config;
  config.contractVersion = descriptor.contractVersion;
  config.streamId = descriptor.streamId;
  config.sessionEpoch = descriptor.sessionEpoch;
  config.mappingVersion = descriptor.mappingVersion;
  config.blockCapacity = descriptor.mappingBlockCapacity;
  config.expectedProvider = descriptor.providerIdentity;
  config.mappingRoot = descriptor.mappingRoot;
  config.payloadPrefix = descriptor.dataPrefix;
  config.maxReverseEntries = descriptor.maxNameReservations;
  return config;
}

ndn_service_framework::StreamNameMapCheckpoint
makeUavStreamNameMapCheckpoint(const VideoStreamDescriptor& descriptor)
{
  validateVideoStreamDescriptor(descriptor);
  ndn_service_framework::StreamNameMapCheckpoint checkpoint;
  checkpoint.frontiers = descriptor.frontiers;
  checkpoint.blockNumber = descriptor.mappingAnchorBlock;
  checkpoint.contentDigest = descriptor.mappingAnchorContentDigest;
  return checkpoint;
}

ndn::Buffer
deriveUavVideoNonce(const ndn::Buffer& nonceSalt,
                    ndn_service_framework::StreamCursor cursor)
{
  if (nonceSalt.size() != 4) {
    throw std::invalid_argument("UAV video nonce salt must be exactly four bytes");
  }
  ndn::Buffer nonce(12);
  std::copy(nonceSalt.begin(), nonceSalt.end(), nonce.begin());
  for (size_t i = 0; i < sizeof(cursor); ++i) {
    nonce[4 + i] = static_cast<uint8_t>(cursor >> (56 - i * 8));
  }
  return nonce;
}

namespace {

void
validateUavVideoAad(const UavVideoAad& aad)
{
  if (aad.version != 1 || aad.exactDataName.empty() ||
      aad.providerIdentity.empty() || aad.serviceName.empty() ||
      !aad.providerIdentity.isPrefixOf(aad.exactDataName) ||
      aad.exactDataName.size() <= aad.providerIdentity.size() ||
      !isCanonicalStreamId(aad.streamId) || aad.sessionEpoch == 0 ||
      aad.mappingVersion == 0 || aad.keyEpoch == 0) {
    throw std::invalid_argument("invalid UAV video AAD context");
  }
}

ndn::Block
makeNestedNameBlock(uint32_t type, const ndn::Name& name)
{
  ndn::Block wrapper(type);
  wrapper.push_back(name.wireEncode());
  wrapper.encode();
  return wrapper;
}

ndn::Name
readNestedNameBlock(const ndn::Block& wrapper)
{
  wrapper.parse();
  if (wrapper.elements().size() != 1 ||
      wrapper.elements().front().type() != ndn::tlv::Name) {
    throw std::invalid_argument("invalid nested Name in UAV video AAD");
  }
  return ndn::Name(wrapper.elements().front());
}

bool
hasSameWire(const ndn::Block& lhs, const ndn::Block& rhs)
{
  return lhs.isValid() && rhs.isValid() && lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

} // namespace

ndn::Block
UavVideoAad::wireEncode() const
{
  validateUavVideoAad(*this);
  ndn::Block block(uav_stream_tlv::UavVideoAadType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    uav_stream_tlv::UavVideoAadVersionType, version));
  block.push_back(makeNestedNameBlock(
    uav_stream_tlv::UavVideoAadExactDataNameType, exactDataName));
  block.push_back(makeNestedNameBlock(
    uav_stream_tlv::UavVideoAadProviderIdentityType, providerIdentity));
  block.push_back(makeNestedNameBlock(
    uav_stream_tlv::UavVideoAadServiceNameType, serviceName));
  block.push_back(ndn::makeStringBlock(
    uav_stream_tlv::UavVideoAadStreamIdType, streamId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    uav_stream_tlv::UavVideoAadSessionEpochType, sessionEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    uav_stream_tlv::UavVideoAadMappingVersionType, mappingVersion));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    uav_stream_tlv::UavVideoAadKeyEpochType, keyEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    uav_stream_tlv::UavVideoAadCursorType, cursor));
  block.encode();
  return block;
}

UavVideoAad
UavVideoAad::wireDecodeStrict(const ndn::Block& block)
{
  try {
    if (block.type() != uav_stream_tlv::UavVideoAadType) {
      throw std::invalid_argument("unexpected UAV video AAD TLV type");
    }
    block.parse();
    if (block.elements().size() != 9) {
      throw std::invalid_argument("unexpected UAV video AAD field count");
    }

    size_t index = 0;
    const auto take = [&] (uint32_t expectedType) -> const ndn::Block& {
      if (index >= block.elements().size() ||
          block.elements()[index].type() != expectedType) {
        throw std::invalid_argument("non-canonical UAV video AAD field order");
      }
      return block.elements()[index++];
    };

    UavVideoAad decoded;
    decoded.version = ndn::readNonNegativeInteger(
      take(uav_stream_tlv::UavVideoAadVersionType));
    decoded.exactDataName = readNestedNameBlock(
      take(uav_stream_tlv::UavVideoAadExactDataNameType));
    decoded.providerIdentity = readNestedNameBlock(
      take(uav_stream_tlv::UavVideoAadProviderIdentityType));
    decoded.serviceName = readNestedNameBlock(
      take(uav_stream_tlv::UavVideoAadServiceNameType));
    decoded.streamId = ndn::readString(
      take(uav_stream_tlv::UavVideoAadStreamIdType));
    decoded.sessionEpoch = ndn::readNonNegativeInteger(
      take(uav_stream_tlv::UavVideoAadSessionEpochType));
    decoded.mappingVersion = ndn::readNonNegativeInteger(
      take(uav_stream_tlv::UavVideoAadMappingVersionType));
    decoded.keyEpoch = ndn::readNonNegativeInteger(
      take(uav_stream_tlv::UavVideoAadKeyEpochType));
    decoded.cursor = ndn::readNonNegativeInteger(
      take(uav_stream_tlv::UavVideoAadCursorType));
    if (index != block.elements().size()) {
      throw std::invalid_argument("trailing UAV video AAD fields");
    }
    validateUavVideoAad(decoded);
    if (!hasSameWire(decoded.wireEncode(), block)) {
      throw std::invalid_argument("non-canonical UAV video AAD wire");
    }
    return decoded;
  }
  catch (const std::invalid_argument&) {
    throw;
  }
  catch (const std::exception& error) {
    throw std::invalid_argument(std::string("invalid UAV video AAD: ") + error.what());
  }
}

UavVideoAad
makeUavVideoAad(const VideoStreamDescriptor& descriptor,
                const UavVideoDataName& binding)
{
  validateVideoStreamSessionContext(descriptor);
  const auto& authorityPrefix =
    descriptor.prefetchEligibility == "predictive-sequential"
      ? descriptor.mappingRoot : descriptor.dataPrefix;
  if (!authorityPrefix.isPrefixOf(binding.name) ||
      binding.name.size() <= authorityPrefix.size()) {
    throw std::invalid_argument("UAV video Data name is outside descriptor prefix");
  }
  UavVideoAad aad;
  aad.exactDataName = binding.name;
  aad.providerIdentity = descriptor.providerIdentity;
  aad.serviceName = descriptor.serviceName;
  aad.streamId = descriptor.streamId;
  aad.sessionEpoch = descriptor.sessionEpoch;
  aad.mappingVersion = descriptor.mappingVersion;
  aad.keyEpoch = descriptor.keyEpoch;
  aad.cursor = binding.cursor;
  validateUavVideoAad(aad);
  return aad;
}

UavVideoNonceUseGuard::UavVideoNonceUseGuard(
  const VideoStreamDescriptor& descriptor)
  : m_contractVersion(descriptor.contractVersion)
  , m_streamId(descriptor.streamId)
  , m_sessionEpoch(descriptor.sessionEpoch)
  , m_keyEpoch(descriptor.keyEpoch)
  , m_providerIdentity(descriptor.providerIdentity)
  , m_serviceName(descriptor.serviceName)
  , m_dataPrefix(descriptor.dataPrefix)
  , m_mappingRoot(descriptor.mappingRoot)
  , m_mappingVersion(descriptor.mappingVersion)
  , m_mappingBlockCapacity(descriptor.mappingBlockCapacity)
  , m_cipher(descriptor.cipher)
  , m_streamKey(descriptor.streamKey)
  , m_nonceSalt(descriptor.nonceSalt)
  , m_maxNameReservations(descriptor.maxNameReservations)
{
  validateVideoStreamDescriptor(descriptor);
}

void
UavVideoNonceUseGuard::reserve(const VideoStreamDescriptor& descriptor,
                               const UavVideoDataName& binding)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool contextMismatch = descriptor.contractVersion != m_contractVersion ||
                               descriptor.streamId != m_streamId ||
                               descriptor.sessionEpoch != m_sessionEpoch ||
                               descriptor.keyEpoch != m_keyEpoch ||
                               descriptor.providerIdentity != m_providerIdentity ||
                               descriptor.serviceName != m_serviceName ||
                               descriptor.dataPrefix != m_dataPrefix ||
                               descriptor.mappingRoot != m_mappingRoot ||
                               descriptor.mappingVersion != m_mappingVersion ||
                               descriptor.mappingBlockCapacity != m_mappingBlockCapacity ||
                               descriptor.maxNameReservations != m_maxNameReservations ||
                               descriptor.cipher != m_cipher ||
                               descriptor.streamKey != m_streamKey ||
                               descriptor.nonceSalt != m_nonceSalt;
  const auto& authorityPrefix =
    descriptor.prefetchEligibility == "predictive-sequential"
      ? descriptor.mappingRoot : descriptor.dataPrefix;
  const bool nameMismatch = !authorityPrefix.isPrefixOf(binding.name) ||
                            binding.name.size() <= authorityPrefix.size();
  // Continuation reservations may append after already announced future
  // samples, so valid unique cursors can materialize out of order. AES-GCM
  // requires nonce uniqueness, not monotonic encryption order.
  const bool cursorViolation =
    binding.cursor == std::numeric_limits<ndn_service_framework::StreamCursor>::max() ||
    m_reservedCursors.count(binding.cursor) != 0;
  const bool explicitNameReuse = m_reservedNames.count(binding.name) != 0;
  const bool reservationOverflow =
    m_reservedNames.size() >= m_maxNameReservations;
  if (m_closed || contextMismatch || nameMismatch || cursorViolation ||
      explicitNameReuse || reservationOverflow) {
    m_closed = true;
    throw std::invalid_argument(
      "UAV video reservation would reuse a nonce or explicit Data name");
  }
  m_reservedCursors.insert(binding.cursor);
  m_reservedNames.emplace(binding.name, binding.cursor);
}

void
UavVideoNonceUseGuard::closeForUncertainUse()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_closed = true;
}

bool
UavVideoNonceUseGuard::isClosed() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_closed;
}

ndn_service_framework::HybridMessageEnvelope
decodeUavVideoEnvelopeStrict(const ndn::Buffer& wire,
                             const VideoStreamDescriptor& descriptor,
                             const UavVideoDataName& binding)
{
  using namespace ndn_service_framework;
  try {
    validateVideoStreamSessionContext(descriptor);
    makeUavVideoAad(descriptor, binding);
    if (wire.empty()) {
      throw std::invalid_argument("empty UAV video envelope");
    }
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    if (block.size() != wire.size() || block.type() != tlv::HybridMessageEnvelopeType) {
      throw std::invalid_argument("trailing or unexpected UAV video envelope wire");
    }
    block.parse();
    static constexpr std::array<uint32_t, 8> EXPECTED_TYPES{
      tlv::VersionType,
      tlv::AlgorithmType,
      tlv::KeyIdType,
      tlv::EpochIdType,
      tlv::MessageTypeType,
      tlv::NonceType,
      tlv::CipherTextType,
      tlv::AuthTagType,
    };
    if (block.elements().size() != EXPECTED_TYPES.size()) {
      throw std::invalid_argument("unexpected UAV video envelope field count");
    }
    for (size_t i = 0; i < EXPECTED_TYPES.size(); ++i) {
      if (block.elements()[i].type() != EXPECTED_TYPES[i]) {
        throw std::invalid_argument("non-canonical UAV video envelope field order");
      }
    }

    HybridMessageEnvelope envelope;
    if (!envelope.WireDecode(block) || envelope.getVersion() != 1 ||
        envelope.getAlgorithm() != "AES-256-GCM" ||
        envelope.getKeyId() != descriptor.streamId ||
        envelope.getEpochId() != std::to_string(descriptor.keyEpoch) ||
        envelope.getMessageType() != "uav-live-video-packet" ||
        envelope.getNonce().size() != HybridMessageCrypto::NONCE_SIZE ||
        envelope.getCipherText().empty() ||
        envelope.getAuthTag().size() != HybridMessageCrypto::TAG_SIZE ||
        envelope.hasWrappedMessageKey()) {
      throw std::invalid_argument("invalid UAV video envelope context");
    }
    const auto expectedNonce = deriveUavVideoNonce(descriptor.nonceSalt, binding.cursor);
    if (envelope.getNonce() != expectedNonce) {
      throw std::invalid_argument("UAV video envelope nonce does not match cursor");
    }
    if (!hasSameWire(envelope.WireEncode(), block)) {
      throw std::invalid_argument("non-canonical UAV video envelope wire");
    }
    return envelope;
  }
  catch (const std::invalid_argument&) {
    throw;
  }
  catch (const std::exception& error) {
    throw std::invalid_argument(std::string("invalid UAV video envelope: ") + error.what());
  }
}

ndn::Buffer
protectUavVideoPacket(const VideoStreamDescriptor& descriptor,
                      const UavVideoDataName& binding,
                      const VideoPacket& packet,
                      UavVideoNonceUseGuard& nonceGuard)
{
  using namespace ndn_service_framework;
  const auto canonicalBinding = makeUavVideoDataName(descriptor, packet);
  if (binding.name != canonicalBinding.name ||
      binding.finalBlockId != canonicalBinding.finalBlockId ||
      binding.cursor != canonicalBinding.cursor || binding.parity != canonicalBinding.parity) {
    nonceGuard.closeForUncertainUse();
    throw std::invalid_argument("VideoPacket does not match its immutable UAV Data binding");
  }

  const auto plaintext = encodeVideoPacket(packet);
  const auto nonce = deriveUavVideoNonce(descriptor.nonceSalt, binding.cursor);
  const auto aad = makeUavVideoAad(descriptor, binding).wireEncode();
  nonceGuard.reserve(descriptor, binding);
  try {
    const auto encrypted = hybridAesGcmEncryptWithNonce(
      descriptor.streamKey,
      ndn::span<const uint8_t>(nonce.data(), nonce.size()),
      ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(aad.data(), aad.size()));

    HybridMessageEnvelope envelope;
    envelope.setVersion(1);
    envelope.setAlgorithm("AES-256-GCM");
    envelope.setKeyId(descriptor.streamId);
    envelope.setEpochId(std::to_string(descriptor.keyEpoch));
    envelope.setMessageType("uav-live-video-packet");
    envelope.setNonce(encrypted.nonce);
    envelope.setCipherText(encrypted.ciphertext);
    envelope.setAuthTag(encrypted.tag);
    const auto envelopeWire = envelope.WireEncode();
    return ndn::Buffer(envelopeWire.begin(), envelopeWire.end());
  }
  catch (...) {
    nonceGuard.closeForUncertainUse();
    throw;
  }
}

VideoPacket
unprotectUavVideoPacket(const VideoStreamDescriptor& descriptor,
                        const UavVideoDataName& binding,
                        const ndn::Name& verifiedProvider,
                        const ndn::Buffer& wire)
{
  using namespace ndn_service_framework;
  if (verifiedProvider != descriptor.providerIdentity) {
    throw std::invalid_argument("UAV video Provider binding failed");
  }
  const auto envelope = decodeUavVideoEnvelopeStrict(wire, descriptor, binding);
  const auto aad = makeUavVideoAad(descriptor, binding).wireEncode();
  ndn::Buffer plaintext;
  if (!hybridAesGcmDecrypt(
        descriptor.streamKey, envelope,
        ndn::span<const uint8_t>(aad.data(), aad.size()), plaintext)) {
    throw std::invalid_argument("UAV video authentication failed");
  }

  VideoPacket packet;
  try {
    packet = decodeVideoPacket(plaintext);
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid authenticated UAV VideoPacket");
  }
  const auto canonicalPlaintext = encodeVideoPacket(packet);
  if (canonicalPlaintext.size() != plaintext.size() ||
      !std::equal(canonicalPlaintext.begin(), canonicalPlaintext.end(), plaintext.begin())) {
    throw std::invalid_argument("non-canonical authenticated UAV VideoPacket");
  }
  const auto canonicalBinding = makeUavVideoDataName(descriptor, packet);
  if (packet.packetSeq != binding.cursor || canonicalBinding.name != binding.name ||
      (!binding.finalBlockId.empty() &&
       canonicalBinding.finalBlockId != binding.finalBlockId) ||
      canonicalBinding.parity != binding.parity) {
    throw std::invalid_argument("authenticated UAV VideoPacket binding mismatch");
  }
  return packet;
}

TelemetryState
TelemetryState::fromFields(const Fields& fields)
{
  TelemetryState state;
  state.telemetryFreshness = fieldOr(fields, "telemetry_freshness", state.telemetryFreshness);
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.lat = fieldOr(fields, "lat", state.lat);
  state.lon = fieldOr(fields, "lon", state.lon);
  state.altitudeM = fieldOr(fields, "altitude_m", state.altitudeM);
  state.groundspeedMps = fieldOr(fields, "groundspeed_mps", state.groundspeedMps);
  state.batteryPercent = fieldOr(fields, "battery_percent", state.batteryPercent);
  state.heartbeatSeen = fieldOr(fields, "heartbeat_seen", state.heartbeatSeen);
  state.flightControllerReady = fieldOr(fields, "flight_controller_ready", state.flightControllerReady);
  state.gpsReady = fieldOr(fields, "gps_ready", state.gpsReady);
  state.ekfReady = fieldOr(fields, "ekf_ready", state.ekfReady);
  state.batteryReady = fieldOr(fields, "battery_ready", state.batteryReady);
  state.armed = fieldOr(fields, "armed", state.armed);
  state.gpsFixType = fieldOr(fields, "gps_fix_type", state.gpsFixType);
  state.gpsFixName = fieldOr(fields, "gps_fix_name", state.gpsFixName);
  state.gpsSatellitesVisible = fieldOr(fields, "gps_satellites_visible", state.gpsSatellitesVisible);
  state.flightControllerBackend = fieldOr(fields, "flight_controller_backend",
                                          state.flightControllerBackend);
  state.flightControllerAvailable = fieldOr(fields, "flight_controller_available",
                                            state.flightControllerAvailable);
  state.flightControllerState = fieldOr(fields, "fc_state", state.flightControllerState);
  state.flightControllerReason = fieldOr(fields, "flight_controller_reason",
                                         state.flightControllerReason);
  state.systemStatus = fieldOr(fields, "system_status", state.systemStatus);
  state.systemStatusName = fieldOr(fields, "system_status_name", state.systemStatusName);
  state.landedState = fieldOr(fields, "landed_state", state.landedState);
  state.landedStateName = fieldOr(fields, "landed_state_name", state.landedStateName);
  state.vtolStateName = fieldOr(fields, "vtol_state_name", state.vtolStateName);
  state.batteryVoltageV = fieldOr(fields, "battery_voltage_v", state.batteryVoltageV);
  state.batteryCurrentA = fieldOr(fields, "battery_current_a", state.batteryCurrentA);
  state.video = fieldOr(fields, "video", state.video);
  state.capture = fieldOr(fields, "capture", state.capture);
  state.recording = fieldOr(fields, "recording", state.recording);
  state.cameraAvailable = fieldOr(fields, "camera_available", state.cameraAvailable);
  state.cameraSource = fieldOr(fields, "camera_source", fieldOr(fields, "source", state.cameraSource));
  state.cameraReason = fieldOr(fields, "camera_reason", state.cameraReason);
  state.linkState = fieldOr(fields, "link_state", state.linkState);
  state.manualControlState = fieldOr(fields, "manual_control_state", state.manualControlState);
  state.manualReplayActive = fieldOr(fields, "manual_replay_active", state.manualReplayActive);
  state.manualNeutralSent = fieldOr(fields, "manual_neutral_sent", state.manualNeutralSent);
  state.manualFreshForMs = fieldOr(fields, "manual_fresh_for_ms", state.manualFreshForMs);
  state.manualReplayCount = fieldOr(fields, "manual_replay_count", state.manualReplayCount);
  state.safetyDetail = fieldOr(fields, "safety_detail", state.safetyDetail);

  const auto timestamp = fieldOr(fields, "timestamp_ms", "");
  if (!timestamp.empty()) {
    try {
      state.timestampMs = std::stoull(timestamp);
    }
    catch (const std::exception&) {
      state.timestampMs = 0;
    }
  }

  std::string reason = fieldOr(fields, "readiness_reason", "");
  if (reason.empty()) {
    if (state.heartbeatSeen != "true") {
      reason = "waiting-heartbeat";
    }
    else if (state.flightControllerReady == "false") {
      reason = "fc-not-ready";
    }
    else if (state.gpsReady == "false") {
      reason = "gps-not-ready";
    }
    else if (state.ekfReady == "false") {
      reason = "ekf-not-ready";
    }
    else if (state.batteryReady == "false") {
      reason = "battery-low";
    }
    else if (state.flightControllerReady == "unknown" ||
             state.gpsReady == "unknown" ||
             state.ekfReady == "unknown" ||
             state.batteryReady == "unknown") {
      reason = "readiness-unknown";
    }
    else {
      reason = "ok";
    }
  }
  state.readinessReason = reason;
  state.readiness = fieldOr(fields, "readiness", reason == "ok" ? "ready" : "not-ready");
  return state;
}

Fields
TelemetryState::toFields() const
{
  const bool readyForTakeoff = heartbeatSeen == "true" &&
                               flightControllerReady == "true" &&
                               gpsReady == "true" &&
                               ekfReady == "true" &&
                               batteryReady == "true" &&
                               armed == "true" &&
                               landedStateName == "on-ground";
  return {
    {"drone_id", droneId},
    {"lat", lat},
    {"lon", lon},
    {"altitude_m", altitudeM},
    {"groundspeed_mps", groundspeedMps},
    {"battery_percent", batteryPercent},
    {"heartbeat_seen", heartbeatSeen},
    {"flight_controller_ready", flightControllerReady},
    {"gps_ready", gpsReady},
    {"ekf_ready", ekfReady},
    {"battery_ready", batteryReady},
    {"armed", armed},
    {"gps_fix_type", gpsFixType},
    {"gps_fix_name", gpsFixName},
    {"gps_satellites_visible", gpsSatellitesVisible},
    {"flight_controller_backend", flightControllerBackend},
    {"flight_controller_available", flightControllerAvailable},
    {"fc_state", flightControllerState},
    {"flight_controller_reason", flightControllerReason},
    {"system_status", systemStatus},
    {"system_status_name", systemStatusName},
    {"landed_state", landedState},
    {"landed_state_name", landedStateName},
    {"vtol_state_name", vtolStateName},
    {"battery_voltage_v", batteryVoltageV},
    {"battery_current_a", batteryCurrentA},
    {"readiness", readiness},
    {"readiness_reason", readinessReason},
    {"telemetry_freshness", telemetryFreshness},
    {"ready_for_takeoff", readyForTakeoff ? "true" : "false"},
    {"video", video},
    {"capture", capture},
    {"recording", recording},
    {"camera_available", cameraAvailable},
    {"camera_source", cameraSource},
    {"camera_reason", cameraReason},
    {"link_state", linkState},
    {"manual_control_state", manualControlState},
    {"manual_replay_active", manualReplayActive},
    {"manual_neutral_sent", manualNeutralSent},
    {"manual_fresh_for_ms", manualFreshForMs},
    {"manual_replay_count", manualReplayCount},
    {"safety_detail", safetyDetail},
    {"timestamp_ms", std::to_string(timestampMs)},
  };
}

std::string
TelemetryState::statusLine() const
{
  return "Telemetry drone=" + droneId +
         " alt=" + altitudeM + "m" +
         " lat=" + lat +
         " lon=" + lon +
         " battery=" + batteryPercent + "%" +
         " ready=" + readiness +
         " reason=" + readinessReason +
         " armed=" + armed +
         " gps=" + gpsReady +
         " ekf=" + ekfReady +
         " fc_backend=" + flightControllerBackend +
         " fc_available=" + flightControllerAvailable +
         " fc_state=" + flightControllerState +
         " landed=" + landedStateName +
         " speed=" + groundspeedMps + "m/s" +
         " video=" + video +
         " camera_available=" + cameraAvailable +
         " link=" + linkState +
         " freshness=" + telemetryFreshnessLabel() +
         " manual=" + manualControlState;
}

std::string
TelemetryState::telemetryFreshnessLabel() const
{
  return telemetryFreshness.empty() ? "unknown" : telemetryFreshness;
}

bool
TelemetryState::telemetryIsFresh() const
{
  return telemetryFreshnessLabel() == "fresh";
}

bool
TelemetryState::telemetryIsStale() const
{
  return telemetryFreshnessLabel() == "stale";
}

bool
TelemetryState::telemetryIsMissing() const
{
  return telemetryFreshnessLabel() == "missing";
}

std::string
TelemetryState::mapSummary(const std::string& selectedDrone) const
{
  return "Map / mission workspace\n\n"
         "Selected drone: " + selectedDrone + "\n"
         "Telemetry source: Drone " + droneId + "\n"
         "Position: lat " + lat + "  lon " + lon + "\n"
         "Altitude: " + altitudeM + "\n"
         "Readiness: " + readiness + " (" + readinessReason + ")  Armed: " + armed + "\n"
         "GPS: " + gpsReady + " fix=" + gpsFixName + " (" + gpsFixType + ")" +
         " sats=" + gpsSatellitesVisible + " EKF=" + ekfReady + "\n"
         "Flight controller: backend=" + flightControllerBackend +
         " available=" + flightControllerAvailable +
         " ready=" + flightControllerReady +
         " state=" + flightControllerState +
         " reason=" + flightControllerReason +
         " system=" + systemStatusName + " landed=" + landedStateName +
         " vtol=" + vtolStateName + "\n"
         "Battery: " + batteryPercent + "% " + batteryVoltageV + "V " +
         batteryCurrentA + "A  Speed: " + groundspeedMps + " m/s\n"
         "Camera: available=" + cameraAvailable + " source=" + cameraSource +
         " reason=" + cameraReason + "\n"
         "Video: " + video + "  Capture: " + capture + "  Recording: " + recording + "\n\n"
         "Safety: link=" + linkState + " manual=" + manualControlState +
         " neutral=" + manualNeutralSent + " fresh_for=" + manualFreshForMs + "ms\n\n"
         "Map tile: OpenStreetMap, centered on the ground station.\n"
         "Click the map to append mission waypoints.";
}

ReadinessState
ReadinessState::fromFields(const Fields& fields)
{
  ReadinessState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.heartbeatSeen = fieldOr(fields, "heartbeat_seen", state.heartbeatSeen);
  state.flightControllerReady = fieldOr(fields, "flight_controller_ready", state.flightControllerReady);
  state.gpsReady = fieldOr(fields, "gps_ready", state.gpsReady);
  state.ekfReady = fieldOr(fields, "ekf_ready", state.ekfReady);
  state.batteryReady = fieldOr(fields, "battery_ready", state.batteryReady);
  state.armed = fieldOr(fields, "armed", state.armed);
  state.mode = fieldOr(fields, "mode", fieldOr(fields, "system_status_name", state.mode));
  state.landedStateName = fieldOr(fields, "landed_state_name", state.landedStateName);
  state.readiness = fieldOr(fields, "readiness", state.readiness);
  auto readinessReason = fieldOr(fields, "readiness_reason", "");
  state.timestampMs = uint64FieldOr(fields, "timestamp_ms", state.timestampMs);

  if (readinessReason.empty() || readinessReason == "unknown") {
    if (state.heartbeatSeen != "true") {
      readinessReason = "waiting-heartbeat";
    }
    else if (state.flightControllerReady == "false") {
      readinessReason = "flight-controller-not-ready";
    }
    else if (state.gpsReady == "false") {
      readinessReason = "gps-not-ready";
    }
    else if (state.ekfReady == "false") {
      readinessReason = "ekf-not-ready";
    }
    else if (state.batteryReady == "false") {
      readinessReason = "battery-not-ready";
    }
    else if (state.flightControllerReady == "true" &&
             state.gpsReady == "true" &&
             state.ekfReady == "true" &&
             state.batteryReady == "true") {
      readinessReason = "ok";
    }
    else {
      readinessReason = "readiness-unknown";
    }
  }
  state.readinessReason = readinessReason;
  if (fieldOr(fields, "readiness", "").empty()) {
    state.readiness = state.readinessReason == "ok" ? "ready" : "not-ready";
  }
  return state;
}

ReadinessState
ReadinessState::fromTelemetry(const TelemetryState& telemetry)
{
  return fromFields(telemetry.toFields());
}

Fields
ReadinessState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"heartbeat_seen", heartbeatSeen},
    {"flight_controller_ready", flightControllerReady},
    {"gps_ready", gpsReady},
    {"ekf_ready", ekfReady},
    {"battery_ready", batteryReady},
    {"armed", armed},
    {"mode", mode},
    {"landed_state_name", landedStateName},
    {"readiness", readiness},
    {"readiness_reason", readinessReason},
    {"ready_for_arm", readyForArm() ? "true" : "false"},
    {"ready_for_takeoff", readyForTakeoff() ? "true" : "false"},
    {"ready_for_land", readyForLand() ? "true" : "false"},
    {"ready_for_manual_control", readyForManualControl() ? "true" : "false"},
    {"timestamp_ms", std::to_string(timestampMs)},
  };
}

bool
ReadinessState::readyForArm() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         gpsReady == "true" &&
         ekfReady == "true" &&
         batteryReady == "true";
}

bool
ReadinessState::landedForTakeoff() const
{
  return landedStateName == "on-ground";
}

bool
ReadinessState::readyForTakeoff() const
{
  return readyForArm() && armed == "true" && landedForTakeoff();
}

bool
ReadinessState::readyForLand() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         armed == "true";
}

bool
ReadinessState::readyForManualControl() const
{
  return heartbeatSeen == "true" &&
         flightControllerReady == "true" &&
         armed == "true";
}

std::string
ReadinessState::statusLine() const
{
  return "Readiness drone=" + droneId +
         " state=" + readiness +
         " reason=" + readinessReason +
         " heartbeat=" + heartbeatSeen +
         " fc=" + flightControllerReady +
         " gps=" + gpsReady +
         " ekf=" + ekfReady +
         " battery=" + batteryReady +
         " armed=" + armed +
         " landed=" + landedStateName;
}

FlightCommandState
FlightCommandState::fromFields(const Fields& fields)
{
  FlightCommandState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.command = fieldOr(fields, "command", state.command);
  state.accepted = fieldOr(fields, "accepted", state.accepted);
  state.ackResult = fieldOr(fields, "ack_result", fieldOr(fields, "ack", state.ackResult));
  state.flightControllerState = fieldOr(fields, "fc_state", state.flightControllerState);
  state.altitudeM = fieldOr(fields, "altitude_m", state.altitudeM);
  state.groundspeedMps = fieldOr(fields, "groundspeed_mps", state.groundspeedMps);
  state.batteryPercent = fieldOr(fields, "battery_percent", state.batteryPercent);
  state.forwardedBytes = fieldOr(fields, "forwarded_bytes", state.forwardedBytes);
  state.detail = fieldOr(fields, "detail", fieldOr(fields, "reason", state.detail));
  state.rttMs = uint64FieldOr(fields, "rtt_ms", state.rttMs);
  state.updatedMs = uint64FieldOr(fields, "updated_ms", state.updatedMs);
  state.timeoutMs = uint64FieldOr(fields, "timeout_ms", state.timeoutMs);
  if (state.updatedMs == 0) {
    state.updatedMs = uint64FieldOr(fields, "timestamp_ms", state.updatedMs);
  }
  return state;
}

Fields
FlightCommandState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"command", command},
    {"accepted", accepted},
    {"ack_result", ackResult},
    {"fc_state", flightControllerState},
    {"altitude_m", altitudeM},
    {"groundspeed_mps", groundspeedMps},
    {"battery_percent", batteryPercent},
    {"forwarded_bytes", forwardedBytes},
    {"detail", detail},
    {"rtt_ms", std::to_string(rttMs)},
    {"updated_ms", std::to_string(updatedMs)},
    {"timeout_ms", std::to_string(timeoutMs)},
  };
}

bool
FlightCommandState::isAccepted() const
{
  return accepted == "true";
}

FlightCommandState
FlightCommandState::makePending(const std::string& droneId,
                                const std::string& command,
                                uint64_t attemptMs,
                                uint64_t timeoutMs)
{
  FlightCommandState state;
  state.droneId = droneId;
  state.command = command;
  state.detail = "targeted-request-sent";
  state.rttMs = 0;
  state.updatedMs = attemptMs;
  state.timeoutMs = timeoutMs;
  return state;
}

FlightCommandState
FlightCommandState::makeTimeout(const std::string& droneId,
                                const std::string& command,
                                uint64_t attemptMs,
                                uint64_t terminalMs,
                                uint64_t timeoutMs)
{
  FlightCommandState state;
  state.droneId = droneId;
  state.command = command;
  state.accepted = "false";
  state.ackResult = "timeout";
  state.forwardedBytes = "0";
  state.detail = "operator-timeout-decision";
  state.updatedMs = std::max(attemptMs, terminalMs);
  state.rttMs = terminalMs >= attemptMs ? terminalMs - attemptMs : 0;
  state.timeoutMs = timeoutMs;
  return state;
}

bool
FlightCommandState::isTimeout() const
{
  return ackResult == "timeout";
}

bool
FlightCommandState::isSafetyCritical() const
{
  return command == "arm" ||
         command == "takeoff" ||
         command == "land" ||
         command == "emergency_stop" ||
         command == "manual_control";
}

std::string
FlightCommandState::statusLine() const
{
  return "Command drone=" + droneId +
         " command=" + command +
         " accepted=" + accepted +
         " ack=" + ackResult +
         " rtt_ms=" + std::to_string(rttMs) +
         " state=" + flightControllerState +
         " alt=" + altitudeM + "m" +
         " speed=" + groundspeedMps + "m/s" +
         " battery=" + batteryPercent + "%" +
         " bytes=" + forwardedBytes +
         " detail=" + detail;
}

SafetyState
SafetyState::fromFields(const Fields& fields)
{
  SafetyState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.linkState = fieldOr(fields, "link_state", state.linkState);
  state.manualControlState = fieldOr(fields, "manual_control_state", state.manualControlState);
  state.manualReplayActive = fieldOr(fields, "manual_replay_active", state.manualReplayActive);
  state.manualNeutralSent = fieldOr(fields, "manual_neutral_sent", state.manualNeutralSent);
  state.manualFreshForMs = uint64FieldOr(fields, "manual_fresh_for_ms", state.manualFreshForMs);
  state.manualReplayCount = uint64FieldOr(fields, "manual_replay_count", state.manualReplayCount);
  state.linkAgeMs = uint64FieldOr(fields, "link_age_ms", state.linkAgeMs);
  state.lostLinkAction = fieldOr(fields, "lost_link_action", state.lostLinkAction);
  state.detail = fieldOr(fields, "safety_detail", state.detail);
  state.updatedMs = uint64FieldOr(fields, "timestamp_ms", uint64FieldOr(fields, "updated_ms", state.updatedMs));
  return state;
}

SafetyState
SafetyState::fromTelemetry(const TelemetryState& telemetry)
{
  return fromFields(telemetry.toFields());
}

Fields
SafetyState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"link_state", linkState},
    {"manual_control_state", manualControlState},
    {"manual_replay_active", manualReplayActive},
    {"manual_neutral_sent", manualNeutralSent},
    {"manual_fresh_for_ms", std::to_string(manualFreshForMs)},
    {"manual_replay_count", std::to_string(manualReplayCount)},
    {"link_age_ms", std::to_string(linkAgeMs)},
    {"lost_link_action", lostLinkAction},
    {"safety_detail", detail},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
SafetyState::manualControlFresh() const
{
  return manualReplayActive == "true" &&
         manualControlState == "fresh" &&
         manualFreshForMs > 0;
}

bool
SafetyState::needsOperatorAttention() const
{
  return linkState == "lost" ||
         linkState == "stale" ||
         linkState == "waiting-heartbeat" ||
         manualControlState == "stale-waiting-neutral" ||
         manualControlState == "send-failed";
}

std::string
SafetyState::statusLine() const
{
  return "Safety drone=" + droneId +
         " link=" + linkState +
         " manual=" + manualControlState +
         " replay_active=" + manualReplayActive +
         " neutral_sent=" + manualNeutralSent +
         " fresh_for_ms=" + std::to_string(manualFreshForMs) +
         " replay_count=" + std::to_string(manualReplayCount) +
         " link_age_ms=" + std::to_string(linkAgeMs) +
         " lost_link_action=" + lostLinkAction +
         " attention=" + std::string(needsOperatorAttention() ? "yes" : "no") +
         " detail=" + detail;
}

namespace {

std::string
safetyBlockReason(const SafetyState& safety)
{
  if (safety.linkState == "lost" ||
      safety.linkState == "stale" ||
      safety.linkState == "waiting-heartbeat") {
    return "link-" + safety.linkState;
  }
  if (safety.manualControlState == "stale-waiting-neutral" ||
      safety.manualControlState == "send-failed") {
    return "manual-" + safety.manualControlState;
  }
  return "safety-attention";
}

void
setNoReadinessReasons(FlightSafetyGateState& state)
{
  state.armReason = "no-telemetry";
  state.takeoffReason = "no-telemetry";
  state.landReason = "no-telemetry";
  state.manualControlReason = "no-telemetry";
  state.controlPanelReason = "no-telemetry";
}

} // namespace

FlightSafetyGateState
FlightSafetyGateState::fromStates(const std::string& droneId,
                                  const std::optional<ReadinessState>& readiness,
                                  const std::optional<SafetyState>& safety)
{
  FlightSafetyGateState state;
  state.droneId = droneId.empty() ? "unknown" : droneId;
  state.hasReadiness = readiness.has_value();
  state.hasSafety = safety.has_value();
  state.canEmergencyStop = !droneId.empty();
  state.emergencyStopReason = state.canEmergencyStop ? "ok" : "no-drone";
  if (safety) {
    state.operatorAttention = safety->needsOperatorAttention();
    state.linkState = safety->linkState;
    state.manualControlState = safety->manualControlState;
  }
  if (!readiness) {
    setNoReadinessReasons(state);
    return state;
  }

  state.readiness = readiness->readiness;
  state.readinessReason = readiness->readinessReason;
  state.armed = readiness->armed;

  const auto safetyReason = safety && state.operatorAttention ?
                            safetyBlockReason(*safety) : std::string();
  auto blockIfAttention = [&safetyReason] (bool allowed, std::string& reason) {
    if (!allowed) {
      return false;
    }
    if (!safetyReason.empty()) {
      reason = safetyReason;
      return false;
    }
    reason = "ok";
    return true;
  };

  if (readiness->armed == "true") {
    state.armReason = "already-armed";
  }
  else if (!readiness->readyForArm()) {
    state.armReason = readiness->readinessReason;
  }
  else {
    state.canArm = blockIfAttention(true, state.armReason);
  }

  if (!readiness->readyForTakeoff()) {
    if (!readiness->readyForArm()) {
      state.takeoffReason = readiness->readinessReason;
    }
    else if (readiness->armed != "true") {
      state.takeoffReason = "not-armed";
    }
    else if (!readiness->landedForTakeoff()) {
      state.takeoffReason = "not-on-ground";
    }
    else {
      state.takeoffReason = "not-ready";
    }
  }
  else {
    state.canTakeoff = blockIfAttention(true, state.takeoffReason);
  }

  if (!readiness->readyForLand()) {
    state.landReason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
  }
  else {
    state.canLand = true;
    state.landReason = "ok";
  }

  if (!readiness->readyForManualControl()) {
    state.manualControlReason = readiness->armed == "true" ? readiness->readinessReason : "not-armed";
    state.controlPanelReason = state.manualControlReason;
  }
  else {
    state.canManualControl = blockIfAttention(true, state.manualControlReason);
    state.canControlPanel = blockIfAttention(true, state.controlPanelReason);
  }

  return state;
}

bool
FlightSafetyGateState::actionAllowed(const std::string& action, std::string& reason) const
{
  if (action == "arm") {
    reason = armReason;
    return canArm;
  }
  if (action == "takeoff") {
    reason = takeoffReason;
    return canTakeoff;
  }
  if (action == "land") {
    reason = landReason;
    return canLand;
  }
  if (action == "manual_control") {
    reason = manualControlReason;
    return canManualControl;
  }
  if (action == "control_panel") {
    reason = controlPanelReason;
    return canControlPanel;
  }
  if (action == "emergency_stop") {
    reason = emergencyStopReason;
    return canEmergencyStop;
  }
  reason = "ok";
  return true;
}

bool
AutoControlSequenceStep::beginWait(std::string commandName,
                                   std::string prerequisiteName,
                                   uint64_t nowMs)
{
  if (phase != "idle" || terminal || dispatched) {
    return false;
  }
  command = std::move(commandName);
  prerequisite = std::move(prerequisiteName);
  phase = "wait-begin";
  reason = "waiting";
  startedMs = nowMs;
  finishedMs = 0;
  return true;
}

bool
AutoControlSequenceStep::satisfy(std::string observedReason, uint64_t nowMs)
{
  if (phase != "wait-begin" || terminal) {
    return false;
  }
  phase = "satisfied";
  reason = std::move(observedReason);
  finishedMs = nowMs;
  return true;
}

bool
AutoControlSequenceStep::expire(std::string expiryReason, uint64_t nowMs)
{
  if (phase != "wait-begin" || terminal || dispatched) {
    return false;
  }
  phase = "expired";
  reason = std::move(expiryReason);
  finishedMs = nowMs;
  terminal = true;
  return true;
}

bool
AutoControlSequenceStep::markDispatched(uint64_t nowMs)
{
  if (phase != "satisfied" || terminal || dispatched) {
    return false;
  }
  phase = "dispatch";
  reason = "single-attempt";
  finishedMs = nowMs;
  dispatched = true;
  ++dispatchCount;
  return true;
}

bool
AutoControlSequenceStep::terminate(std::string terminalReason, uint64_t nowMs)
{
  if (!dispatched || terminal || phase != "dispatch") {
    return false;
  }
  phase = "terminal";
  reason = std::move(terminalReason);
  finishedMs = nowMs;
  terminal = true;
  return true;
}

bool
AutoControlSequenceStep::isTerminal() const
{
  return terminal;
}

uint64_t
AutoControlSequenceStep::elapsedMs(uint64_t nowMs) const
{
  if (startedMs == 0) {
    return 0;
  }
  const auto endMs = finishedMs == 0 ? nowMs : finishedMs;
  return endMs >= startedMs ? endMs - startedMs : 0;
}

std::string
FlightSafetyGateState::statusLine() const
{
  return "FlightSafetyGate drone=" + droneId +
         " has_readiness=" + std::string(hasReadiness ? "true" : "false") +
         " has_safety=" + std::string(hasSafety ? "true" : "false") +
         " readiness=" + readiness +
         " reason=" + readinessReason +
         " armed=" + armed +
         " link=" + linkState +
         " manual=" + manualControlState +
         " attention=" + std::string(operatorAttention ? "true" : "false") +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " can_emergency_stop=" + std::string(canEmergencyStop ? "true" : "false");
}

FlightActionControlState
FlightActionControlState::fromGate(const FlightSafetyGateState& gate)
{
  FlightActionControlState state;
  state.selectedDrone = gate.droneId;
  state.hasReadiness = gate.hasReadiness;
  state.hasSafety = gate.hasSafety;
  state.operatorAttention = gate.operatorAttention;
  state.canArm = gate.canArm;
  state.canTakeoff = gate.canTakeoff;
  state.canLand = gate.canLand;
  state.canManualControl = gate.canManualControl;
  state.canControlPanel = gate.canControlPanel;
  state.canEmergencyStop = gate.canEmergencyStop;
  state.armReason = gate.armReason;
  state.takeoffReason = gate.takeoffReason;
  state.landReason = gate.landReason;
  state.manualControlReason = gate.manualControlReason;
  state.controlPanelReason = gate.controlPanelReason;
  state.emergencyStopReason = gate.emergencyStopReason;
  state.linkState = gate.linkState;
  state.manualControlState = gate.manualControlState;
  return state;
}

std::string
FlightActionControlState::statusLine() const
{
  return "FlightAction selected=" + selectedDrone +
         " has_readiness=" + std::string(hasReadiness ? "true" : "false") +
         " has_safety=" + std::string(hasSafety ? "true" : "false") +
         " safety_attention=" + std::string(operatorAttention ? "true" : "false") +
         " link=" + linkState +
         " manual_state=" + manualControlState +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " arm_reason=" + armReason +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " takeoff_reason=" + takeoffReason +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " land_reason=" + landReason +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " manual_reason=" + manualControlReason +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " panel_reason=" + controlPanelReason +
         " emergency_stop=" + std::string(canEmergencyStop ? "true" : "false") +
         " emergency_reason=" + emergencyStopReason;
}

VideoState
VideoState::fromFields(const Fields& fields)
{
  VideoState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.status = fieldOr(fields, "video", fieldOr(fields, "status", state.status));
  state.capture = fieldOr(fields, "capture", state.capture);
  state.recording = fieldOr(fields, "recording", state.recording);
  state.streamId = fieldOr(fields, "stream_id", state.streamId);
  state.encoding = fieldOr(fields, "encoding", state.encoding);
  state.source = fieldOr(fields, "source", fieldOr(fields, "camera_source", state.source));
  state.cameraAvailable = fieldOr(fields, "camera_available", state.cameraAvailable);
  state.cameraReason = fieldOr(fields, "camera_reason", state.cameraReason);
  state.requestedBitrateKbps = uint64FieldOr(fields, "requested_bitrate_kbps", state.requestedBitrateKbps);
  state.acceptedBitrateKbps = uint64FieldOr(fields, "accepted_bitrate_kbps", state.acceptedBitrateKbps);
  state.requestedFrameWidth = uint64FieldOr(fields, "requested_frame_width", state.requestedFrameWidth);
  state.acceptedFrameWidth = uint64FieldOr(fields, "accepted_frame_width",
                                           uint64FieldOr(fields, "frame_width", state.acceptedFrameWidth));
  state.fps = uint64FieldOr(fields, "fps", state.fps);
  state.streamPacketsPublished = uint64FieldOr(fields, "stream_packets_published",
                                               uint64FieldOr(fields, "packets", state.streamPacketsPublished));
  state.framesPublished = uint64FieldOr(fields, "frames_published", state.framesPublished);
  state.fecGroupsPublished = uint64FieldOr(fields, "fec_groups_published", state.fecGroupsPublished);
  state.recordingChunks = uint64FieldOr(fields, "recording_chunks", state.recordingChunks);
  state.recordingBytes = uint64FieldOr(fields, "recording_bytes", state.recordingBytes);
  state.rttMs = uint64FieldOr(fields, "rtt_ms", state.rttMs);
  state.timeoutPressure = uint64FieldOr(fields, "timeout_pressure", state.timeoutPressure);
  state.probePressure = uint64FieldOr(fields, "probe_pressure", state.probePressure);
  state.backlogPressure = uint64FieldOr(fields, "backlog_pressure", state.backlogPressure);
  state.decodedFrames = uint64FieldOr(fields, "decoded_frames", state.decodedFrames);
  state.updatedMs = uint64FieldOr(fields, "timestamp_ms",
                                  uint64FieldOr(fields, "video_updated_ms", state.updatedMs));
  return state;
}

Fields
VideoState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"video", status},
    {"capture", capture},
    {"recording", recording},
    {"stream_id", streamId},
    {"encoding", encoding},
    {"source", source},
    {"camera_available", cameraAvailable},
    {"camera_reason", cameraReason},
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"accepted_bitrate_kbps", std::to_string(acceptedBitrateKbps)},
    {"requested_frame_width", std::to_string(requestedFrameWidth)},
    {"accepted_frame_width", std::to_string(acceptedFrameWidth)},
    {"fps", std::to_string(fps)},
    {"stream_packets_published", std::to_string(streamPacketsPublished)},
    {"frames_published", std::to_string(framesPublished)},
    {"fec_groups_published", std::to_string(fecGroupsPublished)},
    {"recording_chunks", std::to_string(recordingChunks)},
    {"recording_bytes", std::to_string(recordingBytes)},
    {"rtt_ms", std::to_string(rttMs)},
    {"timeout_pressure", std::to_string(timeoutPressure)},
    {"probe_pressure", std::to_string(probePressure)},
    {"backlog_pressure", std::to_string(backlogPressure)},
    {"decoded_frames", std::to_string(decodedFrames)},
    {"video_updated_ms", std::to_string(updatedMs)},
  };
}

bool
VideoState::isStreaming() const
{
  return status == "streaming";
}

std::string
VideoState::statusLine() const
{
  return "Video drone=" + droneId +
         " state=" + status +
         " capture=" + capture +
         " recording=" + recording +
         " stream=" + streamId +
         " camera_available=" + cameraAvailable +
         " camera_reason=" + cameraReason +
         " bitrate=" + std::to_string(acceptedBitrateKbps) + "kbps" +
         " width=" + std::to_string(acceptedFrameWidth) +
         " packets=" + std::to_string(streamPacketsPublished) +
         " fec_groups=" + std::to_string(fecGroupsPublished) +
         " decoded=" + std::to_string(decodedFrames);
}

VideoControlState
VideoControlState::fromStates(const std::string& selectedDrone,
                              const std::optional<VideoState>& video,
                              bool displayActive)
{
  VideoControlState state;
  state.selectedDrone = selectedDrone.empty() ? "unknown" : selectedDrone;
  state.remoteStreaming = video && video->isStreaming();
  state.displayActive = displayActive;
  state.canStart = !state.remoteStreaming && !state.displayActive;
  state.canStop = state.remoteStreaming || state.displayActive;
  return state;
}

std::string
VideoControlState::statusLine() const
{
  return "VideoControl selected=" + selectedDrone +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " remote_streaming=" + std::string(remoteStreaming ? "true" : "false") +
         " display_active=" + std::string(displayActive ? "true" : "false");
}

VideoAdaptiveState
VideoAdaptiveState::fromFields(const Fields& fields)
{
  VideoAdaptiveState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.state = fieldOr(fields, "adaptive_state", fieldOr(fields, "state", state.state));
  state.rttMs = uint64FieldOr(fields, "rtt_ms", state.rttMs);
  state.requestedBitrateKbps = uint64FieldOr(fields, "requested_bitrate_kbps", state.requestedBitrateKbps);
  state.acceptedBitrateKbps = uint64FieldOr(fields, "accepted_bitrate_kbps", state.acceptedBitrateKbps);
  state.suggestedBitrateKbps = uint64FieldOr(fields, "suggested_bitrate_kbps", state.suggestedBitrateKbps);
  state.bitrateAction = fieldOr(fields, "bitrate_action", state.bitrateAction);
  state.bitrateReason = fieldOr(fields, "bitrate_reason", state.bitrateReason);
  state.coreFetchDecisionAvailable =
    fieldOr(fields, "core_fetch_decision_available", "false") == "true";
  state.coreFetchDecisionSource =
    fieldOr(fields, "core_fetch_decision_source", state.coreFetchDecisionSource);
  state.coreFetchDecisionGeneration =
    uint64FieldOr(fields, "core_fetch_decision_generation",
                  state.coreFetchDecisionGeneration);
  state.coreFetchDecisionObservedAtMs =
    uint64FieldOr(fields, "core_fetch_decision_observed_at_ms",
                  state.coreFetchDecisionObservedAtMs);
  state.coreFetchPhase = fieldOr(fields, "core_fetch_phase", state.coreFetchPhase);
  state.coreFetchPolicyMode =
    fieldOr(fields, "core_fetch_policy_mode", state.coreFetchPolicyMode);
  state.coreFetchCapacityReason =
    fieldOr(fields, "core_fetch_capacity_reason", state.coreFetchCapacityReason);
  state.coreFetchReason =
    fieldOr(fields, "core_fetch_reason", state.coreFetchReason);
  state.window = uint64FieldOr(fields, "window", state.window);
  state.lookahead = uint64FieldOr(fields, "lookahead", state.lookahead);
  state.futureProbeLimit = uint64FieldOr(fields, "future_probe_limit", state.futureProbeLimit);
  state.futureProbeLimitSource =
    fieldOr(fields, "future_probe_limit_source", state.futureProbeLimitSource);
  state.interestLifetimeMs = uint64FieldOr(fields, "interest_lifetime_ms", state.interestLifetimeMs);
  state.missingTimeoutMs = uint64FieldOr(fields, "missing_timeout_ms", state.missingTimeoutMs);
  state.timeoutPressure = uint64FieldOr(fields, "timeout_pressure", state.timeoutPressure);
  state.probePressure = uint64FieldOr(fields, "probe_pressure", state.probePressure);
  state.duplicatePressure = uint64FieldOr(fields, "duplicate_pressure", state.duplicatePressure);
  state.lossPressure = uint64FieldOr(fields, "loss_pressure", state.lossPressure);
  state.backlogPressure = uint64FieldOr(fields, "backlog_pressure", state.backlogPressure);
  state.primaryPressure = fieldOr(fields, "primary_pressure", state.primaryPressure);
  state.policyReason = fieldOr(fields, "policy_reason", state.policyReason);
  state.pendingChunks = uint64FieldOr(fields, "pending_chunks", state.pendingChunks);
  state.maxReorderDepth = uint64FieldOr(fields, "max_reorder_depth", state.maxReorderDepth);
  state.pendingBytes = uint64FieldOr(fields, "pending_bytes", state.pendingBytes);
  state.receivedChunks = uint64FieldOr(fields, "received_chunks", state.receivedChunks);
  state.fecRecoveredChunks = uint64FieldOr(fields, "fec_recovered_chunks", state.fecRecoveredChunks);
  state.timeouts = uint64FieldOr(fields, "timeouts", state.timeouts);
  state.nacks = uint64FieldOr(fields, "nacks", state.nacks);
  state.duplicates = uint64FieldOr(fields, "duplicates", state.duplicates);
  state.publishedFrames = uint64FieldOr(fields, "published_frames", state.publishedFrames);
  state.decodedFrames = uint64FieldOr(fields, "decoded_frames", state.decodedFrames);
  state.decodedFrameGap = uint64FieldOr(fields, "decoded_frame_gap", state.decodedFrameGap);
  state.frameGapPressure = uint64FieldOr(fields, "frame_gap_pressure", state.frameGapPressure);
  state.updatedMs = uint64FieldOr(fields, "updated_ms", state.updatedMs);
  return state;
}

Fields
VideoAdaptiveState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"adaptive_state", state},
    {"rtt_ms", std::to_string(rttMs)},
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"accepted_bitrate_kbps", std::to_string(acceptedBitrateKbps)},
    {"suggested_bitrate_kbps", std::to_string(suggestedBitrateKbps)},
    {"bitrate_action", bitrateAction},
    {"bitrate_reason", bitrateReason},
    {"core_fetch_decision_available", coreFetchDecisionAvailable ? "true" : "false"},
    {"core_fetch_decision_source", coreFetchDecisionSource},
    {"core_fetch_decision_generation", std::to_string(coreFetchDecisionGeneration)},
    {"core_fetch_decision_observed_at_ms", std::to_string(coreFetchDecisionObservedAtMs)},
    {"core_fetch_phase", coreFetchPhase},
    {"core_fetch_policy_mode", coreFetchPolicyMode},
    {"core_fetch_capacity_reason", coreFetchCapacityReason},
    {"core_fetch_reason", coreFetchReason},
    {"window", std::to_string(window)},
    {"lookahead", std::to_string(lookahead)},
    {"future_probe_limit", std::to_string(futureProbeLimit)},
    {"future_probe_limit_source", futureProbeLimitSource},
    {"interest_lifetime_ms", std::to_string(interestLifetimeMs)},
    {"missing_timeout_ms", std::to_string(missingTimeoutMs)},
    {"timeout_pressure", std::to_string(timeoutPressure)},
    {"probe_pressure", std::to_string(probePressure)},
    {"duplicate_pressure", std::to_string(duplicatePressure)},
    {"loss_pressure", std::to_string(lossPressure)},
    {"backlog_pressure", std::to_string(backlogPressure)},
    {"primary_pressure", primaryPressure},
    {"policy_reason", policyReason},
    {"pending_chunks", std::to_string(pendingChunks)},
    {"max_reorder_depth", std::to_string(maxReorderDepth)},
    {"pending_bytes", std::to_string(pendingBytes)},
    {"received_chunks", std::to_string(receivedChunks)},
    {"fec_recovered_chunks", std::to_string(fecRecoveredChunks)},
    {"timeouts", std::to_string(timeouts)},
    {"nacks", std::to_string(nacks)},
    {"duplicates", std::to_string(duplicates)},
    {"published_frames", std::to_string(publishedFrames)},
    {"decoded_frames", std::to_string(decodedFrames)},
    {"decoded_frame_gap", std::to_string(decodedFrameGap)},
    {"frame_gap_pressure", std::to_string(frameGapPressure)},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
VideoAdaptiveState::underPressure() const
{
  return maxPressure() >= 50;
}

uint64_t
VideoAdaptiveState::maxPressure() const
{
  return std::max({timeoutPressure, probePressure, duplicatePressure,
                   lossPressure, backlogPressure, frameGapPressure});
}

ndn_service_framework::StreamHealth
VideoAdaptiveState::toStreamHealth(uint64_t streamSessionEpoch,
                                   const ndn::Name& streamPrefix,
                                   uint64_t staleAfterMs,
                                   uint64_t nowMs) const
{
  ndn_service_framework::StreamInfo info;
  info.streamId = droneId + "-video";
  info.sessionEpoch = streamSessionEpoch;
  info.streamPrefix = streamPrefix;
  info.nextSeq = receivedChunks + pendingChunks;
  info.contentType = "video/h264";
  info.window = window;
  info.lookahead = lookahead;
  info.interestLifetimeMs = interestLifetimeMs;
  info.missingTimeoutMs = missingTimeoutMs;
  info.metadata = {
    {"drone_id", droneId},
    {"adaptive_state", state},
    {"bitrate_action", bitrateAction},
    {"primary_pressure", primaryPressure},
    {"policy_reason", policyReason},
    {"core_fetch_decision_available", coreFetchDecisionAvailable ? "true" : "false"},
    {"core_fetch_decision_source", coreFetchDecisionSource},
    {"core_fetch_phase", coreFetchPhase},
    {"core_fetch_policy_mode", coreFetchPolicyMode},
    {"core_fetch_capacity_reason", coreFetchCapacityReason},
    {"core_fetch_reason", coreFetchReason},
    {"future_probe_limit_source", futureProbeLimitSource},
  };

  ndn_service_framework::StreamMetrics metrics;
  metrics.received = receivedChunks;
  metrics.timeouts = timeouts;
  metrics.nacks = nacks;
  metrics.duplicates = duplicates;
  metrics.gaps = decodedFrameGap;

  ndn_service_framework::StreamFetchDecision decision;
  decision.window = window;
  decision.lookahead = lookahead;
  decision.interestLifetimeMs = interestLifetimeMs;
  decision.missingTimeoutMs = missingTimeoutMs;
  decision.pressure = static_cast<double>(maxPressure()) / 100.0;
  decision.reason = coreFetchDecisionAvailable ? coreFetchReason : "unavailable";
  const auto applicationCondition = (
    backlogPressure >= 80 || timeoutPressure >= 80 || probePressure >= 80
  ) ? "congested" : policyReason;

  auto health = ndn_service_framework::StreamHealth::fromStream(
    info,
    metrics,
    decision,
    info.nextSeq,
    updatedMs,
    state == "stopped" || state == "idle",
    staleAfterMs,
    nowMs);
  health.metadata = {
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"accepted_bitrate_kbps", std::to_string(acceptedBitrateKbps)},
    {"suggested_bitrate_kbps", std::to_string(suggestedBitrateKbps)},
    {"primary_pressure", primaryPressure},
    {"policy_reason", policyReason},
    {"application_condition", applicationCondition},
    {"core_fetch_decision_available", coreFetchDecisionAvailable ? "true" : "false"},
    {"core_fetch_decision_source", coreFetchDecisionSource},
    {"core_fetch_phase", coreFetchPhase},
    {"core_fetch_policy_mode", coreFetchPolicyMode},
    {"core_fetch_capacity_reason", coreFetchCapacityReason},
    {"core_fetch_reason", coreFetchReason},
    {"future_probe_limit", std::to_string(futureProbeLimit)},
    {"future_probe_limit_source", futureProbeLimitSource},
  };
  return health;
}

std::string
VideoAdaptiveState::streamHealthSummary(uint64_t streamSessionEpoch,
                                        const ndn::Name& streamPrefix,
                                        uint64_t staleAfterMs,
                                        uint64_t nowMs) const
{
  const auto health = toStreamHealth(streamSessionEpoch, streamPrefix, staleAfterMs, nowMs);
  std::ostringstream os;
  os << "stream_health=" << ndn_service_framework::toString(health.state)
     << " reason=" << health.reason
     << " pressure=" << static_cast<uint64_t>(health.fetchDecision.pressure * 100.0)
     << " window=" << health.fetchDecision.window
     << " lookahead=" << health.fetchDecision.lookahead
     << " next_seq=" << health.nextSeq
     << " timeouts=" << health.metrics.timeouts
     << " nacks=" << health.metrics.nacks
     << " gaps=" << health.metrics.gaps;
  return os.str();
}

std::string
VideoAdaptiveState::compactSummary() const
{
  return "rtt=" + std::to_string(rttMs) +
         "ms,win=" + std::to_string(window) +
         ",pressure=" + std::to_string(maxPressure()) +
         "/" + primaryPressure +
         ",bitrate=" + std::to_string(acceptedBitrateKbps) +
         "->" + std::to_string(suggestedBitrateKbps) +
         "kbps/" + bitrateAction +
         ",app_reason=" + policyReason +
         ",core_fetch=" + (coreFetchDecisionAvailable ? coreFetchPhase : "unavailable");
}

std::string
VideoAdaptiveState::statusLine() const
{
  return "VideoAdaptive drone=" + droneId +
         " state=" + state +
         " rtt_ms=" + std::to_string(rttMs) +
         " requested_bitrate_kbps=" + std::to_string(requestedBitrateKbps) +
         " accepted_bitrate_kbps=" + std::to_string(acceptedBitrateKbps) +
         " suggested_bitrate_kbps=" + std::to_string(suggestedBitrateKbps) +
         " bitrate_action=" + bitrateAction +
         " bitrate_reason=" + bitrateReason +
         " core_fetch_decision_available=" +
           std::string(coreFetchDecisionAvailable ? "true" : "false") +
         " core_fetch_decision_source=" + coreFetchDecisionSource +
         " core_fetch_decision_generation=" +
           std::to_string(coreFetchDecisionGeneration) +
         " core_fetch_decision_observed_at_ms=" +
           std::to_string(coreFetchDecisionObservedAtMs) +
         " core_fetch_phase=" + coreFetchPhase +
         " core_fetch_policy_mode=" + coreFetchPolicyMode +
         " core_fetch_capacity_reason=" + coreFetchCapacityReason +
         " core_fetch_reason=" + coreFetchReason +
         " window=" + std::to_string(window) +
         " lookahead=" + std::to_string(lookahead) +
         " future_probe_limit=" + std::to_string(futureProbeLimit) +
         " future_probe_limit_source=" + futureProbeLimitSource +
         " interest_lifetime_ms=" + std::to_string(interestLifetimeMs) +
         " missing_timeout_ms=" + std::to_string(missingTimeoutMs) +
         " timeout_pressure=" + std::to_string(timeoutPressure) +
         " probe_pressure=" + std::to_string(probePressure) +
         " duplicate_pressure=" + std::to_string(duplicatePressure) +
         " loss_pressure=" + std::to_string(lossPressure) +
         " backlog_pressure=" + std::to_string(backlogPressure) +
         " primary_pressure=" + primaryPressure +
         " policy_reason=" + policyReason +
         " pending_chunks=" + std::to_string(pendingChunks) +
         " max_reorder_depth=" + std::to_string(maxReorderDepth) +
         " pending_bytes=" + std::to_string(pendingBytes) +
         " received_chunks=" + std::to_string(receivedChunks) +
         " fec_recovered_chunks=" + std::to_string(fecRecoveredChunks) +
         " timeouts=" + std::to_string(timeouts) +
         " nacks=" + std::to_string(nacks) +
         " duplicates=" + std::to_string(duplicates) +
         " published_frames=" + std::to_string(publishedFrames) +
         " decoded_frames=" + std::to_string(decodedFrames) +
         " decoded_frame_gap=" + std::to_string(decodedFrameGap) +
         " frame_gap_pressure=" + std::to_string(frameGapPressure);
}

void
VideoCoreFetchDecisionSnapshot::reset(uint64_t newGeneration)
{
  generation = newGeneration;
  observedAtMs = 0;
  decision.reset();
}

bool
VideoCoreFetchDecisionSnapshot::observe(
  uint64_t activeGeneration,
  uint64_t callbackGeneration,
  const ndn_service_framework::LiveStreamStatus& status,
  uint64_t nowMs)
{
  if (callbackGeneration != activeGeneration || callbackGeneration != generation) {
    return false;
  }
  observedAtMs = nowMs;
  if (status.state == ndn_service_framework::LiveStreamLifecycleState::Active &&
      status.fetchDecision) {
    decision = *status.fetchDecision;
  }
  else {
    decision.reset();
  }
  return true;
}

void
VideoCoreFetchDecisionSnapshot::applyTo(VideoAdaptiveState& state) const
{
  state.coreFetchDecisionGeneration = generation;
  state.coreFetchDecisionObservedAtMs = observedAtMs;
  state.coreFetchDecisionAvailable = decision.has_value();
  state.coreFetchDecisionSource =
    decision ? "core-live-stream-status" : "unavailable";
  state.window = decision ? decision->window : 0;
  state.lookahead = decision ? decision->lookahead : 0;
  state.interestLifetimeMs = decision ? decision->interestLifetimeMs : 0;
  state.missingTimeoutMs = decision ? decision->missingTimeoutMs : 0;
  state.coreFetchPhase =
    decision ? ndn_service_framework::toString(decision->phase) : "INACTIVE";
  state.coreFetchPolicyMode = decision ? decision->policyMode : "none";
  state.coreFetchCapacityReason =
    decision && !decision->capacityReason.empty() ?
      decision->capacityReason : "unavailable";
  state.coreFetchReason =
    decision && !decision->reason.empty() ? decision->reason : "unavailable";
}

namespace {

uint64_t
policyRttMs(const VideoAdaptivePolicyInput& input)
{
  return std::clamp<uint64_t>(input.rttMs, 20, 2000);
}

uint64_t
policyFrameDurationMs(const VideoAdaptivePolicyInput& input)
{
  return std::max<uint64_t>(1, 1000 / std::max<uint64_t>(1, input.fps));
}

uint64_t
policyPacketsForDurationMs(const VideoAdaptivePolicyInput& input,
                           uint64_t durationMs, uint64_t minValue,
                           uint64_t maxValue)
{
  const auto packets = (std::max<uint64_t>(1, input.deltaPacketsPerSecond) *
                        durationMs + 999) / 1000;
  return std::clamp<uint64_t>(packets, minValue, maxValue);
}

uint64_t
policyLossPressurePercent(const VideoAdaptivePolicyInput& input)
{
  const auto received = input.receivedChunks;
  const auto losses = input.nacks + input.timeouts;
  if (received + losses < 20) {
    return 0;
  }
  return std::clamp<uint64_t>((losses * 100) /
                              std::max<uint64_t>(1, received + losses),
                              0, 80);
}

uint64_t
policyBacklogPressurePercent(const VideoAdaptivePolicyInput& input)
{
  if (input.decoderBacklogLimit == 0) {
    return 0;
  }
  return std::clamp<uint64_t>((input.decoderPendingChunks * 100) /
                              input.decoderBacklogLimit, 0, 100);
}

uint64_t
policyFrameGapPressurePercent(const VideoAdaptivePolicyInput& input)
{
  const auto fps = std::max<uint64_t>(1, input.fps);
  if (input.publishedFrames < fps * 2 ||
      input.publishedFrames <= input.decodedFrames) {
    return 0;
  }
  const auto gap = input.publishedFrames - input.decodedFrames;
  const auto allowedGap = std::max<uint64_t>(3, fps);
  if (gap <= allowedGap) {
    return 0;
  }
  const auto severeGap = std::max<uint64_t>(allowedGap + 1, fps * 3);
  return std::clamp<uint64_t>(((gap - allowedGap) * 100) /
                              std::max<uint64_t>(1, severeGap - allowedGap),
                              0, 100);
}

uint64_t
lowerVideoBitrateStep(uint64_t currentKbps)
{
  if (currentKbps > 6000) {
    return 6000;
  }
  if (currentKbps > 4000) {
    return 4000;
  }
  if (currentKbps > 2500) {
    return 2500;
  }
  if (currentKbps > 1500) {
    return 1500;
  }
  if (currentKbps > 800) {
    return 800;
  }
  return currentKbps;
}

uint64_t
higherVideoBitrateStep(uint64_t currentKbps, uint64_t requestedKbps)
{
  if (currentKbps < 800) {
    return std::min<uint64_t>(800, requestedKbps);
  }
  if (currentKbps < 1500) {
    return std::min<uint64_t>(1500, requestedKbps);
  }
  if (currentKbps < 2500) {
    return std::min<uint64_t>(2500, requestedKbps);
  }
  if (currentKbps < 4000) {
    return std::min<uint64_t>(4000, requestedKbps);
  }
  if (currentKbps < 6000) {
    return std::min<uint64_t>(6000, requestedKbps);
  }
  if (currentKbps < 8000) {
    return std::min<uint64_t>(8000, requestedKbps);
  }
  return std::min(currentKbps, requestedKbps);
}

std::string
primaryVideoPressure(uint64_t lossPressure, uint64_t timeoutPressure,
                     uint64_t duplicatePressure, uint64_t backlogPressure,
                     uint64_t probePressure, uint64_t frameGapPressure)
{
  const auto primary = std::max({lossPressure, timeoutPressure, duplicatePressure,
                                 backlogPressure, probePressure, frameGapPressure});
  if (primary == 0) {
    return "none";
  }
  if (primary == timeoutPressure) {
    return "timeout";
  }
  if (primary == lossPressure) {
    return "loss";
  }
  if (primary == duplicatePressure) {
    return "duplicate";
  }
  if (primary == backlogPressure) {
    return "backlog";
  }
  if (primary == frameGapPressure) {
    return "decode-gap";
  }
  return "probe";
}

} // namespace

VideoAdaptivePolicyDecision
computeVideoAdaptivePolicy(const VideoAdaptivePolicyInput& input)
{
  VideoAdaptivePolicyDecision decision;
  const auto rtt = policyRttMs(input);
  const auto frameMs = policyFrameDurationMs(input);
  const auto timeoutBudgetMs = std::clamp<uint64_t>(input.timeoutBudgetMs, 800, 6000);
  const auto dynamicWindowMax = std::max<uint64_t>(1, input.dynamicWindowMax);
  const auto dynamicLookaheadMax = std::max<uint64_t>(1, input.dynamicLookaheadMax);

  decision.lossPressure = policyLossPressurePercent(input);
  decision.backlogPressure = policyBacklogPressurePercent(input);
  decision.frameGapPressure = policyFrameGapPressurePercent(input);
  decision.probePressure = std::max(input.probePressure,
                                    input.duplicatePressure / 2);
  decision.congestionPressure = std::max({
    decision.lossPressure,
    input.timeoutPressure,
    input.duplicatePressure / 2
  });
  decision.primaryPressure = primaryVideoPressure(decision.lossPressure,
                                                  input.timeoutPressure,
                                                  input.duplicatePressure / 2,
                                                  decision.backlogPressure,
                                                  decision.probePressure,
                                                  decision.frameGapPressure);

  const auto windowPressure = std::max(decision.congestionPressure,
                                       std::max(decision.backlogPressure,
                                                decision.frameGapPressure));
  const auto windowTimeoutCapMs = std::clamp<uint64_t>(timeoutBudgetMs / 2, 350, 1000);
  const auto targetBufferMs = std::clamp<uint64_t>(rtt * 2 + frameMs * 2,
                                                  180, windowTimeoutCapMs);
  const auto windowPressureCap = windowPressure > 0 ?
    std::max<uint64_t>(16, dynamicWindowMax *
                           (100 - std::min<uint64_t>(windowPressure, 75)) / 100) :
    dynamicWindowMax;
  const auto minWindow = policyPacketsForDurationMs(
    input, std::clamp<uint64_t>(rtt / 2 + frameMs, 80, 300), 8, 128);
  decision.window = policyPacketsForDurationMs(input, targetBufferMs,
                                               std::min(minWindow, windowPressureCap),
                                               windowPressureCap);

  const auto lookaheadPressure = std::max({
    decision.congestionPressure,
    decision.probePressure,
    decision.backlogPressure,
    decision.frameGapPressure
  });
  const auto lookaheadTimeoutCapMs = std::clamp<uint64_t>(timeoutBudgetMs / 5, 160, 500);
  const auto futureMs = std::clamp<uint64_t>(rtt + frameMs * 2, 100, lookaheadTimeoutCapMs);
  const auto lookaheadPressureCap = lookaheadPressure > 0 ?
    std::max<uint64_t>(4, dynamicLookaheadMax *
                          (100 - std::min<uint64_t>(lookaheadPressure, 85)) / 100) :
    dynamicLookaheadMax;
  decision.lookahead = policyPacketsForDurationMs(input, futureMs, 2,
                                                  lookaheadPressureCap);

  const auto rttProbeMs = std::clamp<uint64_t>(rtt / 3 + frameMs,
                                              frameMs, 180);
  auto probeLimit = policyPacketsForDurationMs(input, rttProbeMs, 1, 24);
  const auto probeLimitPressure = std::max(decision.probePressure,
                                           decision.congestionPressure);
  if (probeLimitPressure > 0) {
    probeLimit = std::max<uint64_t>(1, probeLimit *
                                      (100 - std::min<uint64_t>(probeLimitPressure, 90)) / 100);
  }
  decision.futureProbeLimit = std::clamp<uint64_t>(probeLimit, 1, 24);

  const auto pressureDelay =
    decision.probePressure * 8 + decision.congestionPressure * 4;
  decision.probeBackoffMs = std::clamp<uint64_t>(rtt / 2 + pressureDelay, 60, 1200);

  const auto interestLower = std::clamp<uint64_t>(rtt + frameMs * 2, 350, 1000);
  const auto interestUpper = std::clamp<uint64_t>(timeoutBudgetMs - 100,
                                                 interestLower, 3500);
  const auto lossSlackMs = std::min<uint64_t>(decision.congestionPressure * 8, 600);
  decision.interestLifetimeMs = std::clamp<uint64_t>(rtt * 2 + frameMs * 4 + 200 +
                                                    lossSlackMs,
                                                    interestLower, interestUpper);

  const auto minWaitMs = std::clamp<uint64_t>(std::max<uint64_t>(frameMs * 2, rtt / 2),
                                             100, 350);
  const auto maxWaitMs = std::clamp<uint64_t>(
    std::min<uint64_t>(timeoutBudgetMs / 2, rtt * 2 + frameMs * 4),
    300, 1800);
  const auto baseWaitMs = std::clamp<uint64_t>(rtt + frameMs * 3, minWaitMs, maxWaitMs);
  const auto waitPressureReductionMs =
    std::min<uint64_t>(decision.lossPressure * 3 +
                       decision.backlogPressure * 2 +
                       decision.frameGapPressure * 2,
                       baseWaitMs / 2);
  decision.missingTimeoutMs = std::clamp<uint64_t>(baseWaitMs - waitPressureReductionMs,
                                                  minWaitMs, maxWaitMs);

  const auto requestedKbps = std::max<uint64_t>(128, input.requestedBitrateKbps);
  const auto acceptedKbps = std::max<uint64_t>(128, input.acceptedBitrateKbps);
  decision.suggestedBitrateKbps = acceptedKbps;
  const auto bitratePressure = std::max({
    decision.congestionPressure,
    decision.backlogPressure,
    decision.probePressure,
    decision.frameGapPressure
  });
  const auto highRttThreshold = std::clamp<uint64_t>(
    timeoutBudgetMs / 3 + frameMs * 4, 350, 900);
  if ((bitratePressure >= 65 || rtt >= highRttThreshold) && acceptedKbps > 800) {
    decision.suggestedBitrateKbps = lowerVideoBitrateStep(acceptedKbps);
    decision.bitrateAction =
      decision.suggestedBitrateKbps < acceptedKbps ? "decrease" : "hold";
    decision.bitrateReason = bitratePressure >= 65 ? "pressure" : "high-rtt";
  }
  else if (bitratePressure <= 15 && rtt < highRttThreshold / 2 &&
           acceptedKbps < requestedKbps) {
    decision.suggestedBitrateKbps = higherVideoBitrateStep(acceptedKbps,
                                                           requestedKbps);
    decision.bitrateAction =
      decision.suggestedBitrateKbps > acceptedKbps ? "increase" : "hold";
    decision.bitrateReason = "recovery";
  }
  else {
    decision.bitrateReason = "stable";
  }

  if (decision.bitrateReason == "pressure") {
    decision.policyReason = "pressure-" + decision.primaryPressure;
  }
  else if (decision.bitrateReason == "high-rtt" ||
           decision.bitrateReason == "recovery") {
    decision.policyReason = decision.bitrateReason;
  }
  else if (bitratePressure > 0) {
    decision.policyReason = "pressure-" + decision.primaryPressure;
  }
  else {
    decision.policyReason = "stable";
  }

  return decision;
}

std::optional<std::string>
CanonicalVideoRecordingManifest::validate() const
{
  if (contractVersion != 1 || manifestVersion == 0 || recordingId.empty() ||
      !isCanonicalStreamId(streamId) || sessionEpoch == 0 || mappingVersion == 0 ||
      keyEpoch == 0 || providerIdentity.empty() || serviceName.empty() ||
      signerCertificateName.empty() || trustPolicyVersion.empty() ||
      archivedCertificateObjects.empty() ||
      keyAuthorizationObject.empty() ||
      (packetCatalogEntries != 0 && packetCatalogPrefix.empty())) {
    return "invalid recording manifest identity";
  }
  if (!packets.empty() && (lastCommittedCursor < firstCommittedCursor ||
      safeJoinCursor < firstCommittedCursor || safeJoinCursor > lastCommittedCursor)) {
    return "invalid recording manifest cursor range";
  }
  if (complete && endedMs < startedMs) return "invalid recording manifest interval";
  std::map<ndn::Name, ndn_service_framework::StreamContentDigest> bindings;
  for (const auto& packet : packets) {
    if (packet.kind != "mapping" && packet.kind != "source" && packet.kind != "repair")
      return "invalid retained packet kind";
    if (packet.dataName.empty()) return "empty retained packet name";
    if (!bindings.emplace(packet.dataName, packet.wireDigest).second)
      return "duplicate retained packet name";
    // Catalog order is publication order, not cursor order: a Mapping block
    // intentionally announces future cursors before their source Data exists.
    // The signed entry index/hash chain is monotonic; source cursor bounds are
    // carried by the committed checkpoint.
  }
  uint64_t previousGapEnd = 0;
  bool haveGap = false;
  for (const auto& gap : gaps) {
    if (gap.reason.empty() || gap.lastCursor < gap.firstCursor ||
        (haveGap && gap.firstCursor <= previousGapEnd))
      return "invalid retention gap";
    previousGapEnd = gap.lastCursor;
    haveGap = true;
  }
  if (complete && !gaps.empty()) return "gapped recording cannot be complete";
  if (redactedStreamDescriptor.empty() ||
      redactedStreamDescriptor.count("stream_key_hex") != 0 ||
      redactedStreamDescriptor.count("nonce_salt_hex") != 0)
    return "recording manifest descriptor is missing or contains secrets";
  return std::nullopt;
}

Fields
CanonicalVideoRecordingManifest::toFields() const
{
  if (const auto error = validate()) throw std::invalid_argument(*error);
  Fields fields{
    {"type", "canonical-video-recording-manifest"},
    {"contract_version", std::to_string(contractVersion)},
    {"manifest_version", std::to_string(manifestVersion)},
    {"recording_id", recordingId}, {"stream_id", streamId},
    {"session_epoch", std::to_string(sessionEpoch)},
    {"mapping_version", std::to_string(mappingVersion)},
    {"key_epoch", std::to_string(keyEpoch)},
    {"provider_identity", providerIdentity.toUri()},
    {"service_name", serviceName.toUri()},
    {"first_committed_cursor", std::to_string(firstCommittedCursor)},
    {"last_committed_cursor", std::to_string(lastCommittedCursor)},
    {"safe_join_cursor", std::to_string(safeJoinCursor)},
    {"started_ms", std::to_string(startedMs)}, {"ended_ms", std::to_string(endedMs)},
    {"complete", complete ? "true" : "false"},
    {"signer_certificate_name", signerCertificateName},
    {"signer_certificate_digest", hexEncode(std::vector<uint8_t>(
      signerCertificateDigest.begin(), signerCertificateDigest.end()))},
    {"trust_policy_version", trustPolicyVersion},
    {"archived_certificate_count", std::to_string(archivedCertificateObjects.size())},
    {"key_authorization_object", keyAuthorizationObject.toUri()},
    {"packet_catalog_prefix", packetCatalogPrefix.toUri()},
    {"packet_catalog_entries", std::to_string(packetCatalogEntries)},
    {"packet_catalog_head_digest", hexEncode(std::vector<uint8_t>(
      packetCatalogHeadDigest.begin(), packetCatalogHeadDigest.end()))},
    {"packet_count", std::to_string(packets.size())},
    {"gap_count", std::to_string(gaps.size())},
  };
  fields["descriptor_field_count"] = std::to_string(redactedStreamDescriptor.size());
  size_t descriptorIndex = 0;
  for (const auto& [key, value] : redactedStreamDescriptor) {
    const auto prefix = "descriptor." + std::to_string(descriptorIndex++) + ".";
    fields[prefix + "key"] = key;
    fields[prefix + "value"] = value;
  }
  for (size_t i = 0; i < archivedCertificateObjects.size(); ++i)
    fields["archived_certificate." + std::to_string(i)] =
      archivedCertificateObjects[i].toUri();
  for (size_t i = 0; i < packets.size(); ++i) {
    const auto prefix = "packet." + std::to_string(i) + ".";
    fields[prefix + "kind"] = packets[i].kind;
    fields[prefix + "cursor"] = packets[i].cursor ?
      std::to_string(*packets[i].cursor) : "mapping";
    fields[prefix + "name"] = packets[i].dataName.toUri();
    fields[prefix + "wire_digest"] = hexEncode(std::vector<uint8_t>(
      packets[i].wireDigest.begin(), packets[i].wireDigest.end()));
  }
  for (size_t i = 0; i < gaps.size(); ++i) {
    const auto prefix = "gap." + std::to_string(i) + ".";
    fields[prefix + "first"] = std::to_string(gaps[i].firstCursor);
    fields[prefix + "last"] = std::to_string(gaps[i].lastCursor);
    fields[prefix + "reason"] = gaps[i].reason;
  }
  return fields;
}

CanonicalVideoRecordingManifest
CanonicalVideoRecordingManifest::fromFields(const Fields& fields)
{
  CanonicalVideoRecordingManifest value;
  if (fieldOr(fields, "type", "") != "canonical-video-recording-manifest")
    throw std::invalid_argument("unsupported recording manifest format");
  value.contractVersion = std::stoull(fieldOr(fields, "contract_version", "0"));
  value.manifestVersion = std::stoull(fieldOr(fields, "manifest_version", "0"));
  value.recordingId = fieldOr(fields, "recording_id", "");
  value.streamId = fieldOr(fields, "stream_id", "");
  value.sessionEpoch = std::stoull(fieldOr(fields, "session_epoch", "0"));
  value.mappingVersion = std::stoull(fieldOr(fields, "mapping_version", "0"));
  value.keyEpoch = std::stoull(fieldOr(fields, "key_epoch", "0"));
  value.providerIdentity = ndn::Name(fieldOr(fields, "provider_identity", ""));
  value.serviceName = ndn::Name(fieldOr(fields, "service_name", ""));
  value.firstCommittedCursor = std::stoull(fieldOr(fields, "first_committed_cursor", "0"));
  value.lastCommittedCursor = std::stoull(fieldOr(fields, "last_committed_cursor", "0"));
  value.safeJoinCursor = std::stoull(fieldOr(fields, "safe_join_cursor", "0"));
  value.startedMs = std::stoull(fieldOr(fields, "started_ms", "0"));
  value.endedMs = std::stoull(fieldOr(fields, "ended_ms", "0"));
  value.complete = fieldOr(fields, "complete", "false") == "true";
  value.signerCertificateName = fieldOr(fields, "signer_certificate_name", "");
  const auto certificateDigest = hexDecode(fieldOr(fields, "signer_certificate_digest", ""));
  if (certificateDigest.size() != value.signerCertificateDigest.size())
    throw std::invalid_argument("invalid signer certificate digest");
  std::copy(certificateDigest.begin(), certificateDigest.end(),
            value.signerCertificateDigest.begin());
  value.trustPolicyVersion = fieldOr(fields, "trust_policy_version", "");
  const auto descriptorCount = std::stoull(fieldOr(fields, "descriptor_field_count", "0"));
  for (size_t i = 0; i < descriptorCount; ++i) {
    const auto prefix = "descriptor." + std::to_string(i) + ".";
    if (!value.redactedStreamDescriptor.emplace(
          fieldOr(fields, prefix + "key", ""),
          fieldOr(fields, prefix + "value", "")).second)
      throw std::invalid_argument("duplicate manifest descriptor field");
  }
  const auto certificateCount = std::stoull(
    fieldOr(fields, "archived_certificate_count", "0"));
  for (size_t i = 0; i < certificateCount; ++i)
    value.archivedCertificateObjects.emplace_back(
      fieldOr(fields, "archived_certificate." + std::to_string(i), ""));
  value.keyAuthorizationObject = ndn::Name(
    fieldOr(fields, "key_authorization_object", ""));
  value.packetCatalogPrefix = ndn::Name(fieldOr(fields, "packet_catalog_prefix", ""));
  value.packetCatalogEntries = std::stoull(fieldOr(fields, "packet_catalog_entries", "0"));
  const auto catalogDigest = hexDecode(fieldOr(fields, "packet_catalog_head_digest",
                                                std::string(64, '0')));
  if (catalogDigest.size() != value.packetCatalogHeadDigest.size())
    throw std::invalid_argument("invalid packet catalog head digest");
  std::copy(catalogDigest.begin(), catalogDigest.end(), value.packetCatalogHeadDigest.begin());
  const auto packetCount = std::stoull(fieldOr(fields, "packet_count", "0"));
  for (size_t i = 0; i < packetCount; ++i) {
    const auto prefix = "packet." + std::to_string(i) + ".";
    RetainedVideoPacketReference packet;
    packet.kind = fieldOr(fields, prefix + "kind", "");
    const auto cursor = fieldOr(fields, prefix + "cursor", "");
    if (cursor != "mapping") packet.cursor = std::stoull(cursor);
    packet.dataName = ndn::Name(fieldOr(fields, prefix + "name", ""));
    const auto digest = hexDecode(fieldOr(fields, prefix + "wire_digest", ""));
    if (digest.size() != packet.wireDigest.size())
      throw std::invalid_argument("invalid retained packet digest");
    std::copy(digest.begin(), digest.end(), packet.wireDigest.begin());
    value.packets.push_back(std::move(packet));
  }
  const auto gapCount = std::stoull(fieldOr(fields, "gap_count", "0"));
  for (size_t i = 0; i < gapCount; ++i) {
    const auto prefix = "gap." + std::to_string(i) + ".";
    value.gaps.push_back({std::stoull(fieldOr(fields, prefix + "first", "0")),
                          std::stoull(fieldOr(fields, prefix + "last", "0")),
                          fieldOr(fields, prefix + "reason", "")});
  }
  if (const auto error = value.validate()) throw std::invalid_argument(*error);
  return value;
}

std::optional<std::string>
UavVideoContentKeyGrant::validate() const
{
  if (contractVersion != 1 || recipientIdentity.empty() || providerIdentity.empty() ||
      serviceName.empty() || permission.empty() || !isCanonicalStreamId(streamId) ||
      sessionEpoch == 0 || keyEpoch == 0 || cipher != "aes-256-gcm" ||
      protectedKeyMaterial.size() != 32 || protectedNonceSalt.size() != 4 ||
      issuedMs == 0 || expiresMs <= issuedMs)
    return "invalid UAV video content-key grant";
  return std::nullopt;
}

Fields
UavVideoContentKeyGrant::toProtectedFields() const
{
  if (const auto error = validate()) throw std::invalid_argument(*error);
  return {{"grant_contract_version", std::to_string(contractVersion)},
          {"grant_recipient", recipientIdentity},
          {"grant_provider", providerIdentity.toUri()},
          {"grant_service", serviceName.toUri()}, {"grant_permission", permission},
          {"grant_stream_id", streamId},
          {"grant_session_epoch", std::to_string(sessionEpoch)},
          {"grant_key_epoch", std::to_string(keyEpoch)}, {"grant_cipher", cipher},
          {"grant_protected_key_hex", hexEncode(std::vector<uint8_t>(
            protectedKeyMaterial.begin(), protectedKeyMaterial.end()))},
          {"grant_protected_nonce_salt_hex", hexEncode(std::vector<uint8_t>(
            protectedNonceSalt.begin(), protectedNonceSalt.end()))},
          {"grant_issued_ms", std::to_string(issuedMs)},
          {"grant_expires_ms", std::to_string(expiresMs)}};
}

RecordingDataProductState
RecordingDataProductState::fromFields(const Fields& fields,
                                      const std::string& fallbackDroneId)
{
  RecordingDataProductState state;
  state.droneId = fieldOr(fields, "drone_id", fallbackDroneId.empty() ? state.droneId : fallbackDroneId);
  state.productType = fieldOr(fields, "product_type",
                              fieldOr(fields, "type", state.productType));
  if (state.productType == "camera-recording-manifest") {
    state.productType = "camera-recording";
  }
  state.sessionId = fieldOr(fields, "recording_session_id",
                            fieldOr(fields, "session_id", state.sessionId));
  state.objectPrefix = fieldOr(fields, "recording_object_prefix",
                               fieldOr(fields, "object_prefix", state.objectPrefix));
  state.chunks = uint64FieldOr(fields, "recording_chunks",
                               uint64FieldOr(fields, "packet_catalog_entries",
                                 uint64FieldOr(fields, "chunks", state.chunks)));
  state.bytes = uint64FieldOr(fields, "recording_bytes",
                              uint64FieldOr(fields, "bytes", state.bytes));
  state.updatedMs = uint64FieldOr(fields, "updated_ms",
                                  uint64FieldOr(fields, "timestamp_ms", state.updatedMs));
  return state;
}

Fields
RecordingDataProductState::toFields() const
{
  return {
    {"type", productType + "-manifest"},
    {"product_type", productType},
    {"drone_id", droneId},
    {"recording_session_id", sessionId},
    {"recording_object_prefix", objectPrefix},
    {"recording_chunks", std::to_string(chunks)},
    {"recording_bytes", std::to_string(bytes)},
    {"updated_ms", std::to_string(updatedMs)},
  };
}

bool
RecordingDataProductState::isAvailable() const
{
  return !sessionId.empty() && !objectPrefix.empty() && chunks > 0;
}

bool
RecordingDataProductState::isPlayable() const
{
  return isAvailable();
}

ndn_service_framework::ServiceProvider::DataProductReference
RecordingDataProductState::toDataProductReference(const ndn::Name& serviceName,
                                                  const ndn::Name& producerName) const
{
  ndn_service_framework::ServiceProvider::DataProductReference reference;
  if (isAvailable()) {
    reference.name = ndn::Name(objectPrefix).append(sessionId);
  }
  reference.producerName = producerName;
  reference.serviceName = serviceName;
  reference.objectClass = productType;
  reference.contentType = "video/h264";
  reference.sizeBytes = bytes;
  reference.segmentCount = chunks;
  return reference;
}

std::string
RecordingDataProductState::statusLine() const
{
  return "RecordingDataProduct drone=" + droneId +
         " type=" + productType +
         " session=" + sessionId +
         " prefix=" + objectPrefix +
         " chunks=" + std::to_string(chunks) +
         " bytes=" + std::to_string(bytes) +
         " representation=canonical-signed-data" +
         " available=" + std::string(isAvailable() ? "true" : "false") +
         " playable=" + std::string(isPlayable() ? "true" : "false");
}

ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const FlightCommandState& command,
                         const ndn::Name& serviceName,
                         const ndn::Name& providerName,
                         const ndn::Name& requestId)
{
  std::string state = "RUNNING";
  std::string reasonCode = command.ackResult;
  double progress = 0.5;
  if (command.isTimeout()) {
    state = "EXPIRED";
    progress = 1.0;
  }
  else if (command.accepted == "false" || command.ackResult == "failed") {
    state = "FAILED";
    progress = 1.0;
  }
  else if (command.isAccepted()) {
    state = "DONE";
    reasonCode = "OK";
    progress = 1.0;
  }
  else if (command.accepted == "unknown" && command.ackResult == "unknown") {
    state = "QUEUED";
    progress = 0.0;
  }

  return makeUavOperationStatus(
    command.droneId + ":" + command.command,
    "UAV_FLIGHT_COMMAND",
    serviceName,
    providerName,
    requestId,
    state,
    reasonCode,
    command.statusLine(),
    progress,
    command.updatedMs);
}

ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const RecordingDataProductState& recording,
                         const ndn::Name& serviceName,
                         const ndn::Name& providerName,
                         const ndn::Name& requestId)
{
  const bool available = recording.isAvailable();
  auto status = makeUavOperationStatus(
    recording.droneId + ":" + (recording.sessionId.empty() ? "recording" : recording.sessionId),
    "UAV_RECORDING",
    serviceName,
    providerName,
    requestId,
    available ? "DONE" : "RUNNING",
    available ? "OK" : "RECORDING_NOT_READY",
    recording.statusLine(),
    available ? 1.0 : 0.5,
    recording.updatedMs);
  if (available) {
    status.resultReference = recording.toDataProductReference(serviceName, providerName);
  }
  return status;
}

ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const MissionState& mission,
                         const ndn::Name& serviceName,
                         const ndn::Name& providerName,
                         const ndn::Name& requestId)
{
  std::string state = "QUEUED";
  std::string reasonCode = mission.phase;
  if (mission.isUploading() || mission.isExecuting() || mission.isStopping()) {
    state = "RUNNING";
  }
  else if (mission.isUploaded()) {
    state = "WAITING_INPUT";
  }
  else if (mission.isCompleted()) {
    state = "DONE";
    reasonCode = "OK";
  }
  else if (mission.isFailed()) {
    state = "FAILED";
  }
  else if (mission.isCancelled()) {
    state = "CANCELED";
  }

  return makeUavOperationStatus(
    mission.missionId + ":" + mission.partId,
    "UAV_MISSION_PART",
    serviceName,
    providerName,
    requestId,
    state,
    reasonCode,
    mission.statusLine(),
    missionStateProgress(mission),
    mission.updatedMs);
}

ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const MissionProgressState& progress,
                         const ndn::Name& serviceName,
                         const ndn::Name& providerName,
                         const ndn::Name& requestId)
{
  std::string state = "QUEUED";
  std::string reasonCode = progress.phase;
  if (progress.isComplete()) {
    state = "DONE";
    reasonCode = "OK";
  }
  else if (progress.isFailed()) {
    state = "FAILED";
  }
  else if (progress.phase == "waiting-compensation") {
    state = "WAITING_INPUT";
  }
  else if (progress.isActive()) {
    state = "RUNNING";
  }

  return makeUavOperationStatus(
    progress.taskId,
    "UAV_MISSION",
    serviceName,
    providerName,
    requestId,
    state,
    reasonCode,
    progress.statusLine(),
    missionProgressFraction(progress),
    0);
}

MissionState
MissionState::fromFields(const Fields& fields)
{
  MissionState state;
  state.droneId = fieldOr(fields, "drone_id", state.droneId);
  state.missionId = fieldOr(fields, "mission_id", fieldOr(fields, "active_mission_id", state.missionId));
  state.partId = fieldOr(fields, "part_id", fieldOr(fields, "active_mission_part", state.partId));
  state.phase = fieldOr(fields, "mission_phase", fieldOr(fields, "mission_status", state.phase));
  state.detail = fieldOr(fields, "mission_detail", fieldOr(fields, "status", state.detail));
  state.ack = fieldOr(fields, "mission_ack", state.ack);
  state.transport = fieldOr(fields, "mission_transport", state.transport);
  state.waypointsForwarded = fieldOr(fields, "waypoints_forwarded", state.waypointsForwarded);
  state.waypointAcksAccepted = fieldOr(fields, "waypoint_acks_accepted", state.waypointAcksAccepted);
  const auto updated = fieldOr(fields, "mission_updated_ms", "");
  if (!updated.empty()) {
    try {
      state.updatedMs = std::stoull(updated);
    }
    catch (const std::exception&) {
      state.updatedMs = 0;
    }
  }
  return state;
}

Fields
MissionState::toFields() const
{
  return {
    {"drone_id", droneId},
    {"mission_id", missionId},
    {"part_id", partId},
    {"mission_phase", phase},
    {"mission_detail", detail},
    {"mission_ack", ack},
    {"mission_transport", transport},
    {"waypoints_forwarded", waypointsForwarded},
    {"waypoint_acks_accepted", waypointAcksAccepted},
    {"mission_updated_ms", std::to_string(updatedMs)},
  };
}

bool
MissionState::isIdle() const
{
  return phase == "idle" || phase == "none";
}

bool
MissionState::isUploading() const
{
  return phase == "uploading";
}

bool
MissionState::isUploaded() const
{
  return phase == "uploaded";
}

bool
MissionState::isExecuting() const
{
  return phase == "executing";
}

bool
MissionState::isStopping() const
{
  return phase == "stopping";
}

bool
MissionState::isCompleted() const
{
  return phase == "completed";
}

bool
MissionState::isFailed() const
{
  return phase == "failed";
}

bool
MissionState::isCancelled() const
{
  return phase == "cancelled";
}

bool
MissionState::isTerminal() const
{
  return isCompleted() || isFailed() || isCancelled();
}

bool
MissionState::isAssigned() const
{
  return isUploading() || isUploaded() || isExecuting() || isStopping();
}

bool
MissionState::isBusyForAssignment() const
{
  return isUploading() || isExecuting() || isStopping();
}

bool
MissionState::isStartable() const
{
  return isUploaded();
}

bool
MissionState::isStoppable() const
{
  return isUploaded() || isExecuting() || isStopping();
}

std::string
MissionState::statusLine() const
{
  return "Mission drone=" + droneId +
         " mission=" + missionId +
         " part=" + partId +
         " phase=" + phase +
         " detail=" + detail +
         " ack=" + ack +
         " waypoints=" + waypointsForwarded +
         " accepted=" + waypointAcksAccepted;
}

namespace {

std::vector<MissionWaypoint>
decodeMissionWaypoints(const std::string& value)
{
  std::vector<MissionWaypoint> waypoints;
  if (value.empty() || value == "none") {
    return waypoints;
  }

  std::istringstream parts(value);
  std::string pairText;
  while (std::getline(parts, pairText, ';')) {
    auto commaPos = pairText.find(',');
    if (commaPos == std::string::npos) {
      continue;
    }

    const auto latText = pairText.substr(0, commaPos);
    const auto lonText = pairText.substr(commaPos + 1);
    if (latText.empty() || lonText.empty()) {
      continue;
    }

    try {
      waypoints.push_back({
        std::stod(latText),
        std::stod(lonText)
      });
    }
    catch (const std::exception&) {
      continue;
    }
  }
  return waypoints;
}

std::string
encodeMissionWaypoints(const std::vector<MissionWaypoint>& waypoints)
{
  if (waypoints.empty()) {
    return "none";
  }

  std::ostringstream out;
  for (size_t i = 0; i < waypoints.size(); ++i) {
    if (i > 0) {
      out << ';';
    }
    out << waypoints[i].lat << "," << waypoints[i].lon;
  }
  return out.str();
}

std::string
encodeAssignedDrones(const std::vector<std::string>& drones)
{
  if (drones.empty()) {
    return "none";
  }
  std::ostringstream out;
  for (size_t i = 0; i < drones.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << drones[i];
  }
  return out.str();
}

std::vector<std::string>
decodeAssignedDrones(const std::string& value)
{
  std::vector<std::string> drones;
  if (value.empty() || value == "none") {
    return drones;
  }

  std::istringstream parts(value);
  std::string drone;
  while (std::getline(parts, drone, ',')) {
    if (!drone.empty()) {
      drones.push_back(drone);
    }
  }
  return drones;
}

ndn_service_framework::ServiceProvider::ServiceOperationStatus
makeUavOperationStatus(const std::string& operationId,
                       const std::string& operation,
                       const ndn::Name& serviceName,
                       const ndn::Name& providerName,
                       const ndn::Name& requestId,
                       const std::string& state,
                       const std::string& reasonCode,
                       const std::string& message,
                       double progress,
                       uint64_t updatedMs)
{
  ndn_service_framework::ServiceProvider::ServiceOperationStatus status;
  status.operationId = operationId.empty() ? operation : operationId;
  status.operation = operation;
  status.serviceName = serviceName;
  status.providerName = providerName;
  status.requestId = requestId;
  status.state = state;
  status.reasonCode = reasonCode;
  status.message = message;
  status.progress = std::clamp(progress, 0.0, 1.0);
  status.updatedAtMs = updatedMs;
  return status;
}

double
missionStateProgress(const MissionState& mission)
{
  if (mission.isCompleted() || mission.isFailed() || mission.isCancelled()) {
    return 1.0;
  }
  if (mission.isExecuting() || mission.isStopping()) {
    return 0.75;
  }
  if (mission.isUploaded()) {
    return 0.35;
  }
  if (mission.isUploading()) {
    return 0.2;
  }
  return 0.0;
}

double
missionProgressFraction(const MissionProgressState& progress)
{
  if (progress.isComplete() || progress.isFailed()) {
    return 1.0;
  }
  if (progress.totalParts > 0) {
    return std::clamp(static_cast<double>(progress.completedParts) /
                        static_cast<double>(progress.totalParts),
                      0.0, 1.0);
  }
  return progress.isActive() ? 0.5 : 0.0;
}

} // namespace

MissionObject
MissionObject::fromFields(const Fields& fields, const std::string& fallbackMissionId)
{
  MissionObject mission;
  mission.state = MissionState::fromFields(fields);
  mission.missionId = fieldOr(fields, "mission_id", fieldOr(fields, "missionId", fallbackMissionId));
  if (mission.missionId == "none" || mission.missionId.empty()) {
    mission.missionId = mission.state.missionId.empty() ? fallbackMissionId : mission.state.missionId;
  }
  mission.state.missionId = mission.missionId;

  mission.waypoints = decodeMissionWaypoints(fieldOr(fields, "mission_waypoints", ""));
  mission.assignedDrones = decodeAssignedDrones(fieldOr(fields, "mission_assigned_drones", ""));

  mission.progress.taskId = fieldOr(fields, "mission_progress_task_id", mission.state.partId);
  mission.progress.phase = fieldOr(fields, "mission_progress_phase", mission.progress.phase);
  mission.progress.assignment = fieldOr(fields, "mission_progress_assignment", mission.progress.assignment);
  mission.progress.completionObjective = fieldOr(fields, "mission_progress_completion_objective",
                                               mission.progress.completionObjective);
  mission.progress.drones = fieldOr(fields, "mission_progress_drones", mission.progress.drones);
  mission.progress.attempts = uint64FieldOr(fields, "mission_progress_attempts", mission.progress.attempts);
  mission.progress.totalParts = uint64FieldOr(fields, "mission_progress_total_parts", mission.progress.totalParts);
  mission.progress.completedParts = uint64FieldOr(fields, "mission_progress_completed_parts", mission.progress.completedParts);
  mission.progress.missingParts = uint64FieldOr(fields, "mission_progress_missing_parts", mission.progress.missingParts);
  mission.progress.compensatedParts = uint64FieldOr(fields, "mission_progress_compensated_parts", mission.progress.compensatedParts);
  mission.progress.returnHomePlanned = fieldOr(fields, "mission_progress_return_home", "false") == "true";
  mission.progress.completedPartIds = fieldOr(fields, "mission_progress_completed_part_ids", mission.progress.completedPartIds);
  mission.progress.missingPartIds = fieldOr(fields, "mission_progress_missing_part_ids", mission.progress.missingPartIds);
  mission.progress.compensatedPartIds = fieldOr(fields, "mission_progress_compensated_part_ids", mission.progress.compensatedPartIds);
  mission.progress.pendingPartIds = fieldOr(fields, "mission_progress_pending_part_ids", mission.progress.pendingPartIds);

  return mission;
}

Fields
MissionObject::toFields() const
{
  auto fields = state.toFields();
  fields["mission_assigned_drones"] = encodeAssignedDrones(assignedDrones);
  fields["mission_waypoints"] = encodeMissionWaypoints(waypoints);
  fields["mission_progress_task_id"] = progress.taskId;
  fields["mission_progress_phase"] = progress.phase;
  fields["mission_progress_assignment"] = progress.assignment;
  fields["mission_progress_completion_objective"] = progress.completionObjective;
  fields["mission_progress_drones"] = progress.drones;
  fields["mission_progress_attempts"] = std::to_string(progress.attempts);
  fields["mission_progress_total_parts"] = std::to_string(progress.totalParts);
  fields["mission_progress_completed_parts"] = std::to_string(progress.completedParts);
  fields["mission_progress_missing_parts"] = std::to_string(progress.missingParts);
  fields["mission_progress_compensated_parts"] = std::to_string(progress.compensatedParts);
  fields["mission_progress_return_home"] = progress.returnHomePlanned ? "true" : "false";
  fields["mission_progress_completed_part_ids"] = progress.completedPartIds;
  fields["mission_progress_missing_part_ids"] = progress.missingPartIds;
  fields["mission_progress_compensated_part_ids"] = progress.compensatedPartIds;
  fields["mission_progress_pending_part_ids"] = progress.pendingPartIds;
  return fields;
}

bool
MissionObject::isKnown() const
{
  return !missionId.empty() && missionId != "none";
}

bool
MissionObject::hasAssignment(const std::string& droneId) const
{
  if (droneId.empty()) {
    return false;
  }
  return std::find(assignedDrones.begin(), assignedDrones.end(), droneId) != assignedDrones.end();
}

size_t
MissionObject::waypointCount() const
{
  return waypoints.size();
}

std::string
MissionObject::statusLine() const
{
  return "MissionObject id=" + missionId +
         " state=" + state.phase +
         " waypoints=" + std::to_string(waypointCount()) +
         " assigned=" + encodeAssignedDrones(assignedDrones) +
         " progress=" + progress.phase;
}

MissionStartGateState
MissionStartGateState::fromStates(const std::string& droneId,
                                  const std::optional<MissionState>& mission,
                                  const std::optional<FlightSafetyGateState>& flightGate)
{
  MissionStartGateState state;
  state.droneId = droneId.empty() ? "unknown" : droneId;
  state.hasMission = mission.has_value();
  state.hasFlightGate = flightGate.has_value();
  if (!mission) {
    return state;
  }

  state.missionPhase = mission->phase;
  state.missionUploaded = mission->isStartable();
  state.canStop = mission->isStoppable();
  state.stopReason = state.canStop ? "ok" : "mission-" + mission->phase;
  if (!mission->isStartable()) {
    state.startReason = "mission-" + mission->phase;
    return state;
  }
  if (!flightGate) {
    state.startReason = "no-flight-gate";
    return state;
  }
  if (flightGate->operatorAttention) {
    state.startReason = !flightGate->takeoffReason.empty() ? flightGate->takeoffReason :
                        !flightGate->armReason.empty() ? flightGate->armReason : "safety-attention";
    return state;
  }
  if (flightGate->canArm || flightGate->canTakeoff) {
    state.canStart = true;
    state.startReason = "ok";
    return state;
  }
  state.startReason = !flightGate->takeoffReason.empty() ? flightGate->takeoffReason :
                      !flightGate->armReason.empty() ? flightGate->armReason : "not-ready";
  return state;
}

std::string
MissionStartGateState::statusLine() const
{
  return "MissionStartGate drone=" + droneId +
         " has_mission=" + std::string(hasMission ? "true" : "false") +
         " has_flight_gate=" + std::string(hasFlightGate ? "true" : "false") +
         " mission_uploaded=" + std::string(missionUploaded ? "true" : "false") +
         " phase=" + missionPhase +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " start_reason=" + startReason +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " stop_reason=" + stopReason;
}

bool
MissionProgressState::isActive() const
{
  return phase == "assigning" ||
         phase == "waiting-compensation" ||
         phase == "compensating" ||
         phase == "executing";
}

bool
MissionProgressState::needsCompensation() const
{
  return missingParts > 0 && !isComplete() && !isFailed();
}

bool
MissionProgressState::isComplete() const
{
  return phase == "completed" ||
         (totalParts > 0 && completedParts >= totalParts && missingParts == 0);
}

bool
MissionProgressState::isFailed() const
{
  return phase == "failed";
}

namespace {

bool
commaSeparatedContains(const std::string& list, const std::string& value)
{
  if (value.empty()) {
    return false;
  }
  std::stringstream input(list);
  std::string token;
  while (std::getline(input, token, ',')) {
    if (token == value) {
      return true;
    }
  }
  return false;
}

} // namespace

bool
MissionProgressState::appliesToDrone(const std::string& droneId) const
{
  return drones == "all" || drones == droneId || commaSeparatedContains(drones, droneId);
}

std::string
MissionProgressState::segmentStateForPart(const std::string& partId, const std::string& missionPhase) const
{
  if (partId.empty() || partId == "none") {
    return "PENDING";
  }
  if (isFailed() || missionPhase == "failed" || missionPhase == "cancelled" ||
      missionPhase == "stopping") {
    return "FAILED";
  }
  if (commaSeparatedContains(completedPartIds, partId)) {
    return "DONE";
  }
  if (isActive() || commaSeparatedContains(pendingPartIds, partId) ||
      missionPhase == "assigning" || missionPhase == "waiting-compensation" ||
      missionPhase == "compensating" || missionPhase == "executing") {
    return "RUNNING";
  }
  if (commaSeparatedContains(missingPartIds, partId)) {
    return "FAILED";
  }
  if (isComplete() && missionPhase == "completed") {
    return "DONE";
  }
  return "PENDING";
}

std::string
MissionProgressState::statusLine() const
{
  return "MissionProgress task=" + taskId +
         " phase=" + phase +
         " assignment=" + assignment +
         " drones=" + drones +
         " attempts=" + std::to_string(attempts) +
         " completion_objective=" + completionObjective +
         " total_parts=" + std::to_string(totalParts) +
         " completed_parts=" + std::to_string(completedParts) +
         " missing_parts=" + std::to_string(missingParts) +
         " compensated_parts=" + std::to_string(compensatedParts) +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false") +
         " completed=" + completedPartIds +
         " missing=" + missingPartIds +
         " compensated=" + compensatedPartIds +
         " pending=" + pendingPartIds;
}

namespace {

bool
missionPhaseTerminal(const std::string& phase)
{
  return phase == "completed" || phase == "failed" || phase == "cancelled";
}

void
appendCommaList(std::string& list, const std::string& value)
{
  if (!list.empty() && list != "none") {
    list += ",";
  }
  if (list == "none") {
    list.clear();
  }
  list += value;
}

} // namespace

MissionControlState
MissionControlState::fromStates(const std::vector<MissionStartGateState>& missionGates,
                                const std::optional<MissionProgressState>& progress,
                                bool uploadPending, bool startPending, bool stopPending)
{
  MissionControlState state;
  state.uploadPending = uploadPending;
  state.startPending = startPending;
  state.stopPending = stopPending;
  if (progress) {
    state.hasProgress = true;
    state.progressPhase = progress->phase;
    state.progressActive = progress->isActive();
    state.progressNeedsCompensation = progress->needsCompensation();
    state.progressComplete = progress->isComplete();
    state.progressFailed = progress->isFailed();
  }

  state.phases.clear();
  state.startEligible.clear();
  state.startBlocked.clear();
  for (const auto& gate : missionGates) {
    if (!gate.hasMission) {
      continue;
    }
    appendCommaList(state.phases, gate.droneId + ":" + gate.missionPhase);
    state.hasUploaded = state.hasUploaded || gate.missionUploaded;
    state.hasExecuting = state.hasExecuting || gate.missionPhase == "executing";
    state.hasStopping = state.hasStopping || gate.missionPhase == "stopping";
    state.hasTerminal = state.hasTerminal || missionPhaseTerminal(gate.missionPhase);
    if (gate.missionUploaded) {
      ++state.startableCount;
      if (gate.canStart) {
        appendCommaList(state.startEligible, gate.droneId);
        ++state.startEligibleCount;
      }
      else {
        appendCommaList(state.startBlocked, gate.droneId + ":" + gate.startReason);
        ++state.startBlockedCount;
      }
    }
  }
  if (state.phases.empty()) {
    state.phases = "none";
  }
  if (state.startEligible.empty()) {
    state.startEligible = "none";
  }
  if (state.startBlocked.empty()) {
    state.startBlocked = "none";
  }

  state.canUpload = !state.uploadPending && !state.startPending && !state.stopPending &&
                    !state.hasExecuting && !state.hasStopping && !state.progressActive;
  if (state.uploadPending) {
    state.uploadReason = "upload-pending";
  }
  else if (state.startPending) {
    state.uploadReason = "start-pending";
  }
  else if (state.stopPending) {
    state.uploadReason = "stop-pending";
  }
  else if (state.hasExecuting) {
    state.uploadReason = "mission-executing";
  }
  else if (state.hasStopping) {
    state.uploadReason = "mission-stopping";
  }
  else if (state.progressActive) {
    state.uploadReason = "progress-active";
  }
  else {
    state.uploadReason = "ok";
  }

  state.canStart = state.hasUploaded &&
                   state.startableCount > 0 &&
                   state.startEligibleCount == state.startableCount &&
                   state.startBlockedCount == 0 &&
                   !state.uploadPending && !state.startPending && !state.stopPending &&
                   !state.hasExecuting && !state.hasStopping &&
                   !state.progressActive &&
                   !state.progressNeedsCompensation &&
                   !state.progressFailed;
  state.canStop = !state.stopPending &&
                  (state.startPending || state.hasUploaded || state.hasExecuting ||
                   state.hasStopping || state.progressActive);
  if (!state.hasUploaded || state.startableCount == 0) {
    state.startReason = "no-uploaded-mission";
  }
  else if (state.startBlockedCount > 0) {
    state.startReason = "blocked-" + state.startBlocked;
  }
  else if (state.uploadPending) {
    state.startReason = "upload-pending";
  }
  else if (state.startPending) {
    state.startReason = "start-pending";
  }
  else if (state.stopPending) {
    state.startReason = "stop-pending";
  }
  else if (state.hasExecuting) {
    state.startReason = "mission-executing";
  }
  else if (state.hasStopping) {
    state.startReason = "mission-stopping";
  }
  else if (state.progressActive) {
    state.startReason = "progress-active";
  }
  else if (state.progressNeedsCompensation) {
    state.startReason = "progress-needs-compensation";
  }
  else if (state.progressFailed) {
    state.startReason = "progress-failed";
  }
  else {
    state.startReason = "ok";
  }

  if (state.stopPending) {
    state.stopReason = "stop-pending";
  }
  else {
    state.stopReason = state.canStop ? "ok" : "no-active-mission";
  }
  return state;
}

std::string
MissionControlState::statusLine() const
{
  return "MissionControl can_upload=" + std::string(canUpload ? "true" : "false") +
         " upload_reason=" + uploadReason +
         " can_start=" + std::string(canStart ? "true" : "false") +
         " start_reason=" + startReason +
         " can_stop=" + std::string(canStop ? "true" : "false") +
         " stop_reason=" + stopReason +
         " upload_pending=" + std::string(uploadPending ? "true" : "false") +
         " start_pending=" + std::string(startPending ? "true" : "false") +
         " stop_pending=" + std::string(stopPending ? "true" : "false") +
         " startable_count=" + std::to_string(startableCount) +
         " start_eligible_count=" + std::to_string(startEligibleCount) +
         " start_blocked_count=" + std::to_string(startBlockedCount) +
         " phases=" + phases +
         " progress_phase=" + progressPhase;
}

SelectedActionState
SelectedActionState::fromStates(const std::string& selectedDrone,
                                const FlightActionControlState& flight,
                                const MissionControlState& mission,
                                bool manualMode,
                                bool manualInputActive)
{
  SelectedActionState state;
  state.selectedDrone = selectedDrone.empty() ? flight.selectedDrone : selectedDrone;
  state.flight = flight;
  state.mission = mission;
  state.manualMode = manualMode;
  state.manualInputActive = manualInputActive;
  state.emergencyStopAvailable = flight.canEmergencyStop;
  return state;
}

std::string
SelectedActionState::statusLine() const
{
  return "SelectedAction selected=" + selectedDrone +
         " can_arm=" + std::string(flight.canArm ? "true" : "false") +
         " can_takeoff=" + std::string(flight.canTakeoff ? "true" : "false") +
         " can_land=" + std::string(flight.canLand ? "true" : "false") +
         " can_manual=" + std::string(flight.canManualControl ? "true" : "false") +
         " can_panel=" + std::string(flight.canControlPanel ? "true" : "false") +
         " mission_can_start=" + std::string(mission.canStart ? "true" : "false") +
         " mission_start_reason=" + mission.startReason +
         " mission_can_stop=" + std::string(mission.canStop ? "true" : "false") +
         " mission_stop_reason=" + mission.stopReason +
         " mission_phases=" + mission.phases +
         " mission_progress=" + mission.progressPhase +
         " manual_mode=" + std::string(manualMode ? "true" : "false") +
         " manual_active=" + std::string(manualInputActive ? "true" : "false") +
         " emergency_stop=" + std::string(emergencyStopAvailable ? "true" : "false");
}

DroneListRowState
DroneListRowState::fromStates(const std::string& droneId,
                              bool selected,
                              const std::optional<TelemetryState>& telemetry,
                              const std::optional<ReadinessState>& readiness,
                              const std::optional<MissionState>& mission,
                              const std::optional<VideoState>& video,
                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                              const std::optional<FlightCommandState>& command,
                              const std::optional<SafetyState>& safety,
                              const std::optional<MissionProgressState>& progress,
                              const std::optional<MissionPart>& missionPart,
                              const std::string& cameraService,
                              const std::string& mavlinkService,
                              const std::string& missionService,
                              const std::string& recordingService,
                              const std::string& repoService)
{
  DroneListRowState state;
  state.droneId = droneId;
  state.selected = selected;
  state.hasTelemetry = telemetry.has_value();
  state.hasReadiness = readiness.has_value();
  state.hasMission = mission.has_value();
  state.hasVideo = video.has_value();
  state.hasCommand = command.has_value() && command->command != "none";
  state.hasSafety = safety.has_value();
  state.hasMissionProgress = progress && progress->appliesToDrone(droneId);
  state.hasVideoAdaptive = videoAdaptive.has_value();

  state.readiness = readiness ? readiness->readiness :
                    telemetry ? telemetry->readiness : "unknown";
  state.armed = readiness ? readiness->armed :
                telemetry ? telemetry->armed : "unknown";
  state.gps = readiness ? readiness->gpsReady :
              telemetry ? telemetry->gpsFixName : "unknown";
  state.battery = telemetry ? telemetry->batteryPercent + "%" : "unknown";
  state.mission = mission ? mission->phase : "idle";
  state.missionProgress = state.hasMissionProgress ? progress->phase : "idle";
  state.missionPartId = missionPart ? missionPart->id : "none";
  state.missionSegmentState = missionPart && progress && progress->appliesToDrone(droneId)
    ? progress->segmentStateForPart(missionPart->id, state.missionProgress)
    : "PENDING";
  state.video = video ? video->status :
                telemetry ? telemetry->video : "unknown";
  state.videoAdaptive = videoAdaptive ? videoAdaptive->compactSummary() : "unknown";
  state.command = state.hasCommand ? command->command + ":" + command->ackResult : "none";
  state.safety = safety ? safety->manualControlState + "/" + safety->linkState : "unknown";
  state.serviceCamera = cameraService;
  state.serviceMavlink = mavlinkService;
  state.serviceMission = missionService;
  state.serviceRecording = recordingService;
  state.serviceRepo = repoService;

  state.rowText = std::string(selected ? "● " : "○ ") + "Drone " + droneId +
                  (selected ? " active" : " standby");
  if (state.hasReadiness || state.hasTelemetry) {
    state.rowText += " " + state.readiness +
                     " armed=" + state.armed +
                     " gps=" + state.gps;
  }
  if (state.hasTelemetry) {
    state.rowText += " bat=" + state.battery +
                     " fc=" + telemetry->flightControllerAvailable +
                     "/" + telemetry->flightControllerReady +
                     " cam=" + telemetry->cameraAvailable;
  }
  if (state.hasMission && state.mission != "idle") {
    state.rowText += " mission=" + state.mission;
  }
  if (state.hasMissionProgress && state.missionProgress != "idle") {
    state.rowText += " progress=" + state.missionProgress;
  }
  if (state.missionPartId != "none") {
    state.rowText += " segment=" + state.missionPartId + ":" + state.missionSegmentState;
  }
  if ((state.hasVideo || state.hasTelemetry) && state.video != "unknown") {
    state.rowText += " video=" + state.video;
  }
  if (state.hasVideoAdaptive) {
    state.rowText += " adaptive=" + state.videoAdaptive;
  }
  if (state.hasCommand) {
    state.rowText += " cmd=" + state.command;
  }
  if (state.hasSafety) {
    state.rowText += " safe=" + state.safety;
  }
  if (!state.serviceCamera.empty() || !state.serviceMavlink.empty() ||
      !state.serviceMission.empty() || !state.serviceRecording.empty() ||
      !state.serviceRepo.empty()) {
    state.rowText += " services[camera=" + state.serviceCamera +
                     " mavlink=" + state.serviceMavlink +
                     " mission=" + state.serviceMission +
                     " recording=" + state.serviceRecording +
                     " repo=" + state.serviceRepo + "]";
  }
  return state;
}

DroneListRowState
DroneListRowState::fromStates(const std::string& droneId,
                              bool selected,
                              const std::optional<TelemetryState>& telemetry,
                              const std::optional<ReadinessState>& readiness,
                              const std::optional<MissionState>& mission,
                              const std::optional<VideoState>& video,
                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                              const std::optional<FlightCommandState>& command,
                              const std::optional<SafetyState>& safety,
                              const std::optional<MissionProgressState>& progress,
                              const std::string& cameraService,
                              const std::string& mavlinkService,
                              const std::string& missionService,
                              const std::string& recordingService,
                              const std::string& repoService)
{
  return fromStates(droneId, selected, telemetry, readiness, mission, video, videoAdaptive,
                    command, safety, progress, std::nullopt, cameraService, mavlinkService,
                    missionService, recordingService, repoService);
}

std::string
MissionWaypoint::str() const
{
  std::ostringstream os;
  os << std::fixed << std::setprecision(6) << lat << "," << lon;
  return os.str();
}

MissionWaypoint
MissionPart::firstWaypointOr(MissionWaypoint fallback) const
{
  if (waypoints.empty()) {
    return fallback;
  }
  return waypoints.front();
}

std::vector<std::string>
MissionPart::waypointStrings() const
{
  std::vector<std::string> out;
  out.reserve(waypoints.size());
  for (const auto& waypoint : waypoints) {
    out.push_back(waypoint.str());
  }
  return out;
}

std::string
MissionPart::waypointText() const
{
  std::ostringstream os;
  os << (role.empty() ? "route" : role) << ":";
  for (size_t i = 0; i < waypoints.size(); ++i) {
    if (i > 0) {
      os << ">";
    }
    os << waypoints[i].str();
  }
  return os.str();
}

std::string
MissionPart::statusLine() const
{
  return "MissionPart id=" + id +
         " role=" + role +
         " drone=" + assignedDrone +
         " waypoints=" + std::to_string(waypoints.size()) +
         " attempt=" + std::to_string(attempt) +
         " done=" + std::string(done ? "true" : "false") +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false");
}

std::string
MissionPlan::droneList() const
{
  std::string out;
  for (const auto& part : parts) {
    if (part.assignedDrone.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += ",";
    }
    out += part.assignedDrone;
  }
  return out.empty() ? "none" : out;
}

std::string
MissionPlan::statusLine() const
{
  return "MissionPlan task=" + taskId +
         " assignment=" + assignment +
         " completion_objective=" + completionObjective +
         " drones=" + droneList() +
         " parts=" + std::to_string(parts.size()) +
         " return_home=" + std::string(returnHomePlanned ? "true" : "false");
}

MissionPlanDocument
MissionPlanDocument::fromPlan(const MissionPlan& plan,
                              const std::string& planId,
                              const std::string& displayName,
                              const std::string& operatorId,
                              uint64_t nowMs)
{
  MissionPlanDocument document;
  document.plan = plan;
  document.planId = planId.empty() ? plan.taskId : planId;
  document.displayName = displayName.empty() ? document.planId : displayName;
  document.operatorId = operatorId.empty() ? "unknown" : operatorId;
  const auto timestamp = nowMs == 0 ? nowMilliseconds() : nowMs;
  document.createdMs = timestamp;
  document.updatedMs = timestamp;
  return document;
}

MissionPlanDocument
MissionPlanDocument::fromFields(const Fields& fields)
{
  MissionPlanDocument document;
  document.schema = fieldOr(fields, "mission_plan_schema", document.schema);
  document.planId = fieldOr(fields, "mission_plan_id", fieldOr(fields, "plan_id", document.planId));
  document.displayName = fieldOr(fields, "mission_plan_name", document.displayName);
  document.operatorId = fieldOr(fields, "mission_plan_operator", document.operatorId);
  document.createdMs = uint64FieldOr(fields, "mission_plan_created_ms", 0);
  document.updatedMs = uint64FieldOr(fields, "mission_plan_updated_ms", 0);
  document.plan.taskId = fieldOr(fields, "mission_plan_task", fieldOr(fields, "task_id", ""));
  document.plan.assignment = fieldOr(fields, "mission_plan_assignment", document.plan.assignment);
  document.plan.completionObjective = fieldOr(fields, "mission_plan_completion_objective",
                                              document.plan.completionObjective);
  document.plan.returnHomePlanned = fieldOr(fields, "mission_plan_return_home", "false") == "true";
  document.geofence = decodeMissionWaypoints(fieldOr(fields, "mission_plan_geofence", ""));
  document.rallyPoints = decodeMissionWaypoints(fieldOr(fields, "mission_plan_rally_points", ""));
  document.metadata = decodeFields(fieldOr(fields, "mission_plan_metadata", ""));
  const auto partCount = uint64FieldOr(fields, "mission_plan_part_count", 0);
  for (uint64_t i = 0; i < partCount; ++i) {
    const auto prefix = "mission_plan_part_" + std::to_string(i) + "_";
    MissionPart part;
    part.id = fieldOr(fields, prefix + "id", "");
    part.role = fieldOr(fields, prefix + "role", "");
    part.assignedDrone = fieldOr(fields, prefix + "drone", "");
    part.completedBy = fieldOr(fields, prefix + "completed_by", "");
    part.waypoints = decodeMissionWaypoints(fieldOr(fields, prefix + "waypoints", ""));
    part.attempt = static_cast<int>(uint64FieldOr(fields, prefix + "attempt", 0));
    part.done = fieldOr(fields, prefix + "done", "false") == "true";
    part.returnHomePlanned = fieldOr(fields, prefix + "return_home", "false") == "true";
    document.plan.parts.push_back(part);
  }
  return document;
}

Fields
MissionPlanDocument::toFields() const
{
  Fields fields{
    {"mission_plan_schema", schema},
    {"mission_plan_id", planId},
    {"mission_plan_name", displayName},
    {"mission_plan_operator", operatorId},
    {"mission_plan_created_ms", std::to_string(createdMs)},
    {"mission_plan_updated_ms", std::to_string(updatedMs)},
    {"mission_plan_task", plan.taskId},
    {"mission_plan_assignment", plan.assignment},
    {"mission_plan_completion_objective", plan.completionObjective},
    {"mission_plan_return_home", plan.returnHomePlanned ? "true" : "false"},
    {"mission_plan_part_count", std::to_string(plan.parts.size())},
    {"mission_plan_geofence", encodeMissionWaypoints(geofence)},
    {"mission_plan_rally_points", encodeMissionWaypoints(rallyPoints)},
    {"mission_plan_metadata", encodeFields(metadata)},
  };
  for (size_t i = 0; i < plan.parts.size(); ++i) {
    const auto& part = plan.parts[i];
    const auto prefix = "mission_plan_part_" + std::to_string(i) + "_";
    fields[prefix + "id"] = part.id;
    fields[prefix + "role"] = part.role;
    fields[prefix + "drone"] = part.assignedDrone;
    fields[prefix + "completed_by"] = part.completedBy;
    fields[prefix + "waypoints"] = encodeMissionWaypoints(part.waypoints);
    fields[prefix + "attempt"] = std::to_string(part.attempt);
    fields[prefix + "done"] = part.done ? "true" : "false";
    fields[prefix + "return_home"] = part.returnHomePlanned ? "true" : "false";
  }
  return fields;
}

bool
MissionPlanDocument::isSaveable() const
{
  return !planId.empty() && planId != "none" &&
         !plan.taskId.empty() &&
         !plan.parts.empty();
}

bool
MissionPlanDocument::hasFenceOrRally() const
{
  return !geofence.empty() || !rallyPoints.empty();
}

std::string
MissionPlanDocument::statusLine() const
{
  return "MissionPlanDocument id=" + planId +
         " name=" + displayName +
         " operator=" + operatorId +
         " task=" + plan.taskId +
         " parts=" + std::to_string(plan.parts.size()) +
         " geofence=" + std::to_string(geofence.size()) +
         " rally=" + std::to_string(rallyPoints.size()) +
         " saveable=" + std::string(isSaveable() ? "true" : "false");
}

void
saveMissionPlanDocument(const MissionPlanDocument& document, const std::string& path)
{
  if (path.empty()) {
    throw std::runtime_error("cannot save UAV mission plan: empty path");
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot save UAV mission plan: " + path);
  }
  output << "# NDNSF-UAV mission plan document\n";
  for (const auto& [key, value] : document.toFields()) {
    output << key << "=" << value << "\n";
  }
}

MissionPlanDocument
loadMissionPlanDocument(const std::string& path)
{
  if (path.empty()) {
    throw std::runtime_error("cannot load UAV mission plan: empty path");
  }
  return MissionPlanDocument::fromFields(loadKeyValueConfig(path));
}

UavDataProductCatalogState
UavDataProductCatalogState::fromFields(const Fields& fields)
{
  UavDataProductCatalogState state;
  state.repoObjects = uint64FieldOr(fields, "catalog_repo_objects", 0);
  state.recordingProducts = uint64FieldOr(fields, "catalog_recording_products", 0);
  state.telemetryLogProducts = uint64FieldOr(fields, "catalog_telemetry_log_products", 0);
  state.detectionProducts = uint64FieldOr(fields, "catalog_detection_products", 0);
  state.missionLogProducts = uint64FieldOr(fields, "catalog_mission_log_products", 0);
  state.totalBytes = uint64FieldOr(fields, "catalog_total_bytes", 0);
  state.sourceRepo = fieldOr(fields, "catalog_source_repo", state.sourceRepo);
  state.latestProductType = fieldOr(fields, "catalog_latest_product_type", state.latestProductType);
  state.latestObjectPrefix = fieldOr(fields, "catalog_latest_object_prefix", state.latestObjectPrefix);
  state.latestMissionId = fieldOr(fields, "catalog_latest_mission_id", state.latestMissionId);
  state.updatedMs = uint64FieldOr(fields, "catalog_updated_ms", 0);
  return state;
}

UavDataProductCatalogState
UavDataProductCatalogState::fromRecording(const RecordingDataProductState& recording)
{
  UavDataProductCatalogState state;
  if (recording.isAvailable()) {
    state.repoObjects = recording.chunks;
    state.recordingProducts = 1;
    state.totalBytes = recording.bytes;
    state.sourceRepo = recording.droneId;
    state.latestProductType = recording.productType;
    state.latestObjectPrefix = recording.objectPrefix;
    state.latestMissionId = fieldOr(recording.toFields(), "mission_id", "none");
    state.updatedMs = recording.updatedMs;
  }
  return state;
}

UavDataProductCatalogState
UavDataProductCatalogState::fromCatalogProductFields(const std::vector<Fields>& entries,
                                                     const std::string& sourceRepo,
                                                     uint64_t updatedMs)
{
  UavDataProductCatalogState state;
  state.repoObjects = entries.size();
  state.sourceRepo = sourceRepo.empty() ? state.sourceRepo : sourceRepo;
  state.updatedMs = updatedMs;

  std::set<std::string> recordingKeys;
  std::set<std::string> telemetryKeys;
  std::set<std::string> detectionKeys;
  std::set<std::string> missionKeys;

  auto lower = [] (std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [] (unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
  };
  auto productKey = [] (const std::string& objectName) {
    const auto chunkPos = objectName.rfind("/chunk/");
    if (chunkPos != std::string::npos) {
      return objectName.substr(0, chunkPos);
    }
    const auto segmentPos = objectName.rfind("/seg/");
    if (segmentPos != std::string::npos) {
      return objectName.substr(0, segmentPos);
    }
    return objectName.empty() ? std::string("unknown") : objectName;
  };
  auto addKey = [&productKey] (std::set<std::string>& keys, const std::string& objectName,
                              const std::string& fallback) {
    const auto key = productKey(objectName);
    keys.insert(key == "unknown" ? fallback : key);
  };

  uint64_t latestUpdatedMs = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    const auto objectName = fieldOr(entry, "object_name",
                                    fieldOr(entry, "objectName", fieldOr(entry, "name", "")));
    const auto objectType = fieldOr(entry, "object_type",
                                    fieldOr(entry, "objectType", fieldOr(entry, "type", "")));
    const auto text = lower(objectName + " " + objectType);
    const auto size = uint64FieldOr(entry, "size", uint64FieldOr(entry, "bytes", 0));
    const auto entryUpdatedMs = uint64FieldOr(entry, "updated_ms",
                                              uint64FieldOr(entry, "timestamp_ms", 0));
    state.totalBytes += size;

    if (text.find("recording") != std::string::npos ||
        text.find("h264") != std::string::npos ||
        text.find("video") != std::string::npos) {
      addKey(recordingKeys, objectName, "recording-" + std::to_string(i));
      state.latestProductType = objectType.empty() ? "camera-recording" : objectType;
      state.latestObjectPrefix = productKey(objectName);
    }
    else if (text.find("telemetry") != std::string::npos) {
      addKey(telemetryKeys, objectName, "telemetry-" + std::to_string(i));
      state.latestProductType = objectType.empty() ? "telemetry-log" : objectType;
      state.latestObjectPrefix = productKey(objectName);
    }
    else if (text.find("detection") != std::string::npos ||
             text.find("yolo") != std::string::npos) {
      addKey(detectionKeys, objectName, "detection-" + std::to_string(i));
      state.latestProductType = objectType.empty() ? "detection-log" : objectType;
      state.latestObjectPrefix = productKey(objectName);
    }
    else if (text.find("mission") != std::string::npos ||
             text.find("flight-log") != std::string::npos) {
      addKey(missionKeys, objectName, "mission-" + std::to_string(i));
      state.latestProductType = objectType.empty() ? "mission-log" : objectType;
      state.latestObjectPrefix = productKey(objectName);
    }
    if (entryUpdatedMs >= latestUpdatedMs) {
      latestUpdatedMs = entryUpdatedMs;
      state.latestMissionId = fieldOr(entry, "mission_id", state.latestMissionId);
    }
  }

  state.recordingProducts = recordingKeys.size();
  state.telemetryLogProducts = telemetryKeys.size();
  state.detectionProducts = detectionKeys.size();
  state.missionLogProducts = missionKeys.size();
  if (state.updatedMs == 0) {
    state.updatedMs = latestUpdatedMs;
  }
  return state;
}

Fields
UavDataProductCatalogState::toFields() const
{
  return {
    {"catalog_repo_objects", std::to_string(repoObjects)},
    {"catalog_recording_products", std::to_string(recordingProducts)},
    {"catalog_telemetry_log_products", std::to_string(telemetryLogProducts)},
    {"catalog_detection_products", std::to_string(detectionProducts)},
    {"catalog_mission_log_products", std::to_string(missionLogProducts)},
    {"catalog_total_bytes", std::to_string(totalBytes)},
    {"catalog_source_repo", sourceRepo},
    {"catalog_latest_product_type", latestProductType},
    {"catalog_latest_object_prefix", latestObjectPrefix},
    {"catalog_latest_mission_id", latestMissionId},
    {"catalog_updated_ms", std::to_string(updatedMs)},
  };
}

uint64_t
UavDataProductCatalogState::totalProducts() const
{
  return recordingProducts + telemetryLogProducts + detectionProducts + missionLogProducts;
}

bool
UavDataProductCatalogState::hasQueryableProducts() const
{
  return totalProducts() > 0;
}

std::string
UavDataProductCatalogState::statusLine() const
{
  return "UavDataProductCatalog products=" + std::to_string(totalProducts()) +
         " repo_objects=" + std::to_string(repoObjects) +
         " source=" + sourceRepo +
         " recordings=" + std::to_string(recordingProducts) +
         " telemetry_logs=" + std::to_string(telemetryLogProducts) +
         " detections=" + std::to_string(detectionProducts) +
         " mission_logs=" + std::to_string(missionLogProducts) +
         " bytes=" + std::to_string(totalBytes) +
         " latest=" + latestProductType + ":" + latestObjectPrefix;
}

namespace {

std::string
encodeParameters(const Fields& parameters)
{
  return encodeFields(parameters);
}

Fields
decodeParameters(const std::string& value)
{
  return decodeFields(value);
}

} // namespace

VehicleParameterSnapshot
VehicleParameterSnapshot::fromFields(const Fields& fields)
{
  VehicleParameterSnapshot snapshot;
  snapshot.droneId = fieldOr(fields, "parameter_drone", snapshot.droneId);
  snapshot.source = fieldOr(fields, "parameter_source", snapshot.source);
  snapshot.firmware = fieldOr(fields, "parameter_firmware", snapshot.firmware);
  snapshot.vehicleType = fieldOr(fields, "parameter_vehicle_type", snapshot.vehicleType);
  snapshot.flightModes = fieldOr(fields, "parameter_flight_modes", snapshot.flightModes);
  snapshot.parameterCount = uint64FieldOr(fields, "parameter_count", 0);
  snapshot.completePercent = uint64FieldOr(fields, "parameter_complete_percent", 0);
  snapshot.updatedMs = uint64FieldOr(fields, "parameter_updated_ms", 0);
  snapshot.parameters = decodeParameters(fieldOr(fields, "parameter_values", ""));
  if (snapshot.parameterCount == 0 && !snapshot.parameters.empty()) {
    snapshot.parameterCount = snapshot.parameters.size();
  }
  return snapshot;
}

Fields
VehicleParameterSnapshot::toFields(bool includeParameters) const
{
  Fields fields{
    {"parameter_drone", droneId},
    {"parameter_source", source},
    {"parameter_firmware", firmware},
    {"parameter_vehicle_type", vehicleType},
    {"parameter_flight_modes", flightModes},
    {"parameter_count", std::to_string(parameterCount == 0 ? parameters.size() : parameterCount)},
    {"parameter_complete_percent", std::to_string(completePercent)},
    {"parameter_updated_ms", std::to_string(updatedMs)},
  };
  if (includeParameters) {
    fields["parameter_values"] = encodeParameters(parameters);
  }
  return fields;
}

bool
VehicleParameterSnapshot::isUsable() const
{
  return parameterCount > 0 || !parameters.empty() || firmware != "unknown" ||
         vehicleType != "unknown" || flightModes != "unknown";
}

std::string
VehicleParameterSnapshot::statusLine() const
{
  return "VehicleParameterSnapshot drone=" + droneId +
         " source=" + source +
         " firmware=" + firmware +
         " vehicle=" + vehicleType +
         " modes=" + flightModes +
         " parameters=" + std::to_string(parameterCount == 0 ? parameters.size() : parameterCount) +
         " complete=" + std::to_string(completePercent) +
         " usable=" + std::string(isUsable() ? "true" : "false");
}

VehicleParameterEditRequest
VehicleParameterEditRequest::fromFields(const Fields& fields)
{
  VehicleParameterEditRequest request;
  request.requestId = fieldOr(fields, "parameter_edit_request_id", request.requestId);
  request.operatorId = fieldOr(fields, "parameter_operator", request.operatorId);
  request.droneId = fieldOr(fields, "parameter_drone", request.droneId);
  request.parameterName = fieldOr(fields, "parameter_name", request.parameterName);
  request.expectedValue = fieldOr(fields, "parameter_expected_value", request.expectedValue);
  request.requestedValue = fieldOr(fields, "parameter_requested_value", request.requestedValue);
  request.valueType = fieldOr(fields, "parameter_value_type", request.valueType);
  request.targetSystem = uint64FieldOr(fields, "parameter_target_system", request.targetSystem);
  request.targetComponent = uint64FieldOr(fields, "parameter_target_component", request.targetComponent);
  request.dryRun = fieldOr(fields, "parameter_dry_run", "false") == "true";
  request.requestedMs = uint64FieldOr(fields, "parameter_requested_ms", request.requestedMs);
  return request;
}

Fields
VehicleParameterEditRequest::toFields() const
{
  return {
    {"type", "vehicle-parameter-edit-request"},
    {"parameter_edit_request_id", requestId},
    {"parameter_operator", operatorId},
    {"parameter_drone", droneId},
    {"parameter_name", parameterName},
    {"parameter_expected_value", expectedValue},
    {"parameter_requested_value", requestedValue},
    {"parameter_value_type", valueType},
    {"parameter_target_system", std::to_string(targetSystem)},
    {"parameter_target_component", std::to_string(targetComponent)},
    {"parameter_dry_run", dryRun ? "true" : "false"},
    {"parameter_requested_ms", std::to_string(requestedMs)},
  };
}

bool
VehicleParameterEditRequest::isValid(std::string& reason) const
{
  if (operatorId.empty() || operatorId == "unknown") {
    reason = "missing-operator";
    return false;
  }
  if (droneId.empty() || droneId == "unknown") {
    reason = "missing-drone";
    return false;
  }
  if (parameterName.empty()) {
    reason = "missing-parameter";
    return false;
  }
  if (parameterName.size() > 16) {
    reason = "parameter-name-too-long";
    return false;
  }
  if (!dryRun && requestedValue.empty()) {
    reason = "missing-requested-value";
    return false;
  }
  reason = "ok";
  return true;
}

std::string
VehicleParameterEditRequest::statusLine() const
{
  std::string reason;
  const bool valid = isValid(reason);
  return "VehicleParameterEditRequest request=" + requestId +
         " operator=" + operatorId +
         " drone=" + droneId +
         " param=" + parameterName +
         " type=" + valueType +
         " dry_run=" + std::string(dryRun ? "true" : "false") +
         " valid=" + std::string(valid ? "true" : "false") +
         " reason=" + reason;
}

VehicleParameterEditResult
VehicleParameterEditResult::fromFields(const Fields& fields)
{
  VehicleParameterEditResult result;
  result.requestId = fieldOr(fields, "parameter_edit_request_id", result.requestId);
  result.droneId = fieldOr(fields, "parameter_drone", result.droneId);
  result.parameterName = fieldOr(fields, "parameter_name", result.parameterName);
  result.valueType = fieldOr(fields, "parameter_value_type", result.valueType);
  result.accepted = fieldOr(fields, "parameter_accepted", "false") == "true";
  result.applied = fieldOr(fields, "parameter_applied", "false") == "true";
  result.verified = fieldOr(fields, "parameter_verified", "false") == "true";
  result.reason = fieldOr(fields, "parameter_reason", result.reason);
  result.previousValue = fieldOr(fields, "parameter_previous_value", result.previousValue);
  result.requestedValue = fieldOr(fields, "parameter_requested_value", result.requestedValue);
  result.verifiedValue = fieldOr(fields, "parameter_verified_value", result.verifiedValue);
  result.updatedMs = uint64FieldOr(fields, "parameter_updated_ms", result.updatedMs);
  return result;
}

Fields
VehicleParameterEditResult::toFields() const
{
  return {
    {"type", "vehicle-parameter-edit-result"},
    {"parameter_edit_request_id", requestId},
    {"parameter_drone", droneId},
    {"parameter_name", parameterName},
    {"parameter_value_type", valueType},
    {"parameter_accepted", accepted ? "true" : "false"},
    {"parameter_applied", applied ? "true" : "false"},
    {"parameter_verified", verified ? "true" : "false"},
    {"parameter_reason", reason},
    {"parameter_previous_value", previousValue},
    {"parameter_requested_value", requestedValue},
    {"parameter_verified_value", verifiedValue},
    {"parameter_updated_ms", std::to_string(updatedMs)},
  };
}

bool
VehicleParameterEditResult::successful() const
{
  return accepted && applied && verified;
}

std::string
VehicleParameterEditResult::statusLine() const
{
  return "VehicleParameterEditResult request=" + requestId +
         " drone=" + droneId +
         " param=" + parameterName +
         " accepted=" + std::string(accepted ? "true" : "false") +
         " applied=" + std::string(applied ? "true" : "false") +
         " verified=" + std::string(verified ? "true" : "false") +
         " reason=" + reason;
}

PreflightCheckItem
PreflightCheckItem::fromFields(const Fields& fields)
{
  PreflightCheckItem item;
  item.checkId = fieldOr(fields, "preflight_check_id", item.checkId);
  item.droneId = fieldOr(fields, "preflight_drone", item.droneId);
  item.label = fieldOr(fields, "preflight_label", item.label);
  item.category = fieldOr(fields, "preflight_category", item.category);
  item.status = fieldOr(fields, "preflight_status", item.status);
  item.reason = fieldOr(fields, "preflight_reason", item.reason);
  item.blocking = fieldOr(fields, "preflight_blocking", "true") == "true";
  item.order = uint64FieldOr(fields, "preflight_order", item.order);
  item.updatedMs = uint64FieldOr(fields, "preflight_updated_ms", item.updatedMs);
  return item;
}

Fields
PreflightCheckItem::toFields() const
{
  return {
    {"type", "preflight-check-item"},
    {"preflight_check_id", checkId},
    {"preflight_drone", droneId},
    {"preflight_label", label},
    {"preflight_category", category},
    {"preflight_status", status},
    {"preflight_reason", reason},
    {"preflight_blocking", blocking ? "true" : "false"},
    {"preflight_order", std::to_string(order)},
    {"preflight_updated_ms", std::to_string(updatedMs)},
  };
}

bool
PreflightCheckItem::isPass() const
{
  return status == "pass" || status == "passed" || status == "ok";
}

bool
PreflightCheckItem::isBlockingFailure() const
{
  return blocking && (status == "fail" || status == "failed" || status == "error");
}

std::string
PreflightCheckItem::statusLine() const
{
  return "PreflightCheckItem id=" + checkId +
         " drone=" + droneId +
         " label=" + label +
         " category=" + category +
         " status=" + status +
         " blocking_failure=" + std::string(isBlockingFailure() ? "true" : "false") +
         " reason=" + reason;
}

MavlinkMessageSummary
MavlinkMessageSummary::fromFields(const Fields& fields, const std::string& prefix)
{
  MavlinkMessageSummary summary;
  summary.messageName = fieldOr(fields, prefix + "mavlink_message_name", summary.messageName);
  summary.messageId = uint64FieldOr(fields, prefix + "mavlink_message_id", summary.messageId);
  summary.systemId = uint64FieldOr(fields, prefix + "mavlink_system_id", summary.systemId);
  summary.componentId = uint64FieldOr(fields, prefix + "mavlink_component_id", summary.componentId);
  summary.count = uint64FieldOr(fields, prefix + "mavlink_message_count", summary.count);
  summary.rateHz = fieldOr(fields, prefix + "mavlink_rate_hz", summary.rateHz);
  summary.lastSeenMs = uint64FieldOr(fields, prefix + "mavlink_last_seen_ms", summary.lastSeenMs);
  return summary;
}

Fields
MavlinkMessageSummary::toFields(const std::string& prefix) const
{
  return {
    {prefix + "mavlink_message_name", messageName},
    {prefix + "mavlink_message_id", std::to_string(messageId)},
    {prefix + "mavlink_system_id", std::to_string(systemId)},
    {prefix + "mavlink_component_id", std::to_string(componentId)},
    {prefix + "mavlink_message_count", std::to_string(count)},
    {prefix + "mavlink_rate_hz", rateHz},
    {prefix + "mavlink_last_seen_ms", std::to_string(lastSeenMs)},
  };
}

bool
MavlinkMessageSummary::isActive(uint64_t nowMs, uint64_t staleAfterMs) const
{
  const auto current = nowMs == 0 ? nowMilliseconds() : nowMs;
  return lastSeenMs > 0 && current >= lastSeenMs && current - lastSeenMs <= staleAfterMs;
}

std::string
MavlinkMessageSummary::statusLine() const
{
  return "MavlinkMessageSummary name=" + messageName +
         " id=" + std::to_string(messageId) +
         " sys=" + std::to_string(systemId) +
         " comp=" + std::to_string(componentId) +
         " count=" + std::to_string(count) +
         " rate_hz=" + rateHz;
}

UavAnalyzeSnapshot
UavAnalyzeSnapshot::fromFields(const Fields& fields)
{
  UavAnalyzeSnapshot snapshot;
  snapshot.droneId = fieldOr(fields, "analyze_drone", snapshot.droneId);
  snapshot.linkState = fieldOr(fields, "analyze_link_state", snapshot.linkState);
  snapshot.flightMode = fieldOr(fields, "analyze_flight_mode", snapshot.flightMode);
  snapshot.missionPhase = fieldOr(fields, "analyze_mission_phase", snapshot.missionPhase);
  snapshot.videoState = fieldOr(fields, "analyze_video_state", snapshot.videoState);
  snapshot.parameterCacheStatus = fieldOr(fields, "analyze_parameter_cache", snapshot.parameterCacheStatus);
  snapshot.updatedMs = uint64FieldOr(fields, "analyze_updated_ms", snapshot.updatedMs);
  const auto messageCount = uint64FieldOr(fields, "analyze_message_count", 0);
  snapshot.messages.reserve(static_cast<size_t>(messageCount));
  for (uint64_t i = 0; i < messageCount; ++i) {
    snapshot.messages.push_back(MavlinkMessageSummary::fromFields(
      fields, "message." + std::to_string(i) + "."));
  }
  return snapshot;
}

Fields
UavAnalyzeSnapshot::toFields() const
{
  Fields fields{
    {"type", "uav-analyze-snapshot"},
    {"analyze_drone", droneId},
    {"analyze_link_state", linkState},
    {"analyze_flight_mode", flightMode},
    {"analyze_mission_phase", missionPhase},
    {"analyze_video_state", videoState},
    {"analyze_parameter_cache", parameterCacheStatus},
    {"analyze_updated_ms", std::to_string(updatedMs)},
    {"analyze_message_count", std::to_string(messages.size())},
  };
  for (size_t i = 0; i < messages.size(); ++i) {
    const auto messageFields = messages[i].toFields("message." + std::to_string(i) + ".");
    fields.insert(messageFields.begin(), messageFields.end());
  }
  return fields;
}

uint64_t
UavAnalyzeSnapshot::activeMessageCount(uint64_t nowMs, uint64_t staleAfterMs) const
{
  return static_cast<uint64_t>(std::count_if(
    messages.begin(), messages.end(),
    [=] (const MavlinkMessageSummary& message) {
      return message.isActive(nowMs, staleAfterMs);
    }));
}

std::string
UavAnalyzeSnapshot::statusLine() const
{
  return "UavAnalyzeSnapshot drone=" + droneId +
         " link=" + linkState +
         " mode=" + flightMode +
         " mission=" + missionPhase +
         " video=" + videoState +
         " parameters=" + parameterCacheStatus +
         " messages=" + std::to_string(messages.size());
}

UavOperatorDashboardSnapshot
UavOperatorDashboardSnapshot::fromFields(const Fields& fields)
{
  UavOperatorDashboardSnapshot snapshot;
  snapshot.droneId = fieldOr(fields, "dashboard_drone", snapshot.droneId);
  snapshot.telemetryFreshness = fieldOr(fields, "dashboard_telemetry_freshness",
                                        snapshot.telemetryFreshness);
  snapshot.readiness = fieldOr(fields, "dashboard_readiness", snapshot.readiness);
  snapshot.readinessReason = fieldOr(fields, "dashboard_readiness_reason",
                                     snapshot.readinessReason);
  snapshot.linkState = fieldOr(fields, "dashboard_link_state", snapshot.linkState);
  snapshot.flightMode = fieldOr(fields, "dashboard_flight_mode", snapshot.flightMode);
  snapshot.missionPhase = fieldOr(fields, "dashboard_mission_phase", snapshot.missionPhase);
  snapshot.videoState = fieldOr(fields, "dashboard_video_state", snapshot.videoState);
  snapshot.parameterCacheStatus = fieldOr(fields, "dashboard_parameter_cache",
                                          snapshot.parameterCacheStatus);
  snapshot.parameterCount = uint64FieldOr(fields, "dashboard_parameter_count",
                                          snapshot.parameterCount);
  snapshot.preflightTotal = uint64FieldOr(fields, "dashboard_preflight_total",
                                          snapshot.preflightTotal);
  snapshot.preflightBlockingFailures = uint64FieldOr(fields,
                                                     "dashboard_preflight_blocking_failures",
                                                     snapshot.preflightBlockingFailures);
  snapshot.mavlinkMessageCount = uint64FieldOr(fields, "dashboard_mavlink_messages",
                                               snapshot.mavlinkMessageCount);
  snapshot.activeMavlinkMessageCount = uint64FieldOr(fields,
                                                     "dashboard_active_mavlink_messages",
                                                     snapshot.activeMavlinkMessageCount);
  snapshot.canArm = fieldOr(fields, "dashboard_can_arm", "false") == "true";
  snapshot.canTakeoff = fieldOr(fields, "dashboard_can_takeoff", "false") == "true";
  snapshot.canLand = fieldOr(fields, "dashboard_can_land", "false") == "true";
  snapshot.canManualControl = fieldOr(fields, "dashboard_can_manual_control", "false") == "true";
  snapshot.canEmergencyStop = fieldOr(fields, "dashboard_can_emergency_stop", "false") == "true";
  snapshot.updatedMs = uint64FieldOr(fields, "dashboard_updated_ms", snapshot.updatedMs);
  return snapshot;
}

Fields
UavOperatorDashboardSnapshot::toFields() const
{
  return {
    {"type", "uav-operator-dashboard-snapshot"},
    {"dashboard_drone", droneId},
    {"dashboard_telemetry_freshness", telemetryFreshness},
    {"dashboard_readiness", readiness},
    {"dashboard_readiness_reason", readinessReason},
    {"dashboard_link_state", linkState},
    {"dashboard_flight_mode", flightMode},
    {"dashboard_mission_phase", missionPhase},
    {"dashboard_video_state", videoState},
    {"dashboard_parameter_cache", parameterCacheStatus},
    {"dashboard_parameter_count", std::to_string(parameterCount)},
    {"dashboard_preflight_total", std::to_string(preflightTotal)},
    {"dashboard_preflight_blocking_failures", std::to_string(preflightBlockingFailures)},
    {"dashboard_mavlink_messages", std::to_string(mavlinkMessageCount)},
    {"dashboard_active_mavlink_messages", std::to_string(activeMavlinkMessageCount)},
    {"dashboard_can_arm", canArm ? "true" : "false"},
    {"dashboard_can_takeoff", canTakeoff ? "true" : "false"},
    {"dashboard_can_land", canLand ? "true" : "false"},
    {"dashboard_can_manual_control", canManualControl ? "true" : "false"},
    {"dashboard_can_emergency_stop", canEmergencyStop ? "true" : "false"},
    {"dashboard_updated_ms", std::to_string(updatedMs)},
  };
}

bool
UavOperatorDashboardSnapshot::operatorReady() const
{
  return telemetryFreshness == "fresh" && readiness == "ready" &&
         preflightBlockingFailures == 0 && activeMavlinkMessageCount > 0;
}

std::string
UavOperatorDashboardSnapshot::statusLine() const
{
  return "UavOperatorDashboardSnapshot drone=" + droneId +
         " telemetry=" + telemetryFreshness +
         " readiness=" + readiness +
         " reason=" + readinessReason +
         " link=" + linkState +
         " mode=" + flightMode +
         " mission=" + missionPhase +
         " video=" + videoState +
         " parameters=" + parameterCacheStatus +
         " parameter_count=" + std::to_string(parameterCount) +
         " preflight=" + std::to_string(preflightTotal) +
         " blocking_failures=" + std::to_string(preflightBlockingFailures) +
         " mavlink_active=" + std::to_string(activeMavlinkMessageCount) +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " operator_ready=" + std::string(operatorReady() ? "true" : "false");
}

OperatorAuthorityLease
OperatorAuthorityLease::fromFields(const Fields& fields)
{
  OperatorAuthorityLease lease;
  lease.leaseId = fieldOr(fields, "lease_id", lease.leaseId);
  lease.operatorId = fieldOr(fields, "lease_operator", lease.operatorId);
  lease.droneId = fieldOr(fields, "lease_drone", lease.droneId);
  lease.scope = fieldOr(fields, "lease_scope", lease.scope);
  lease.issuedMs = uint64FieldOr(fields, "lease_issued_ms", 0);
  lease.expiresMs = uint64FieldOr(fields, "lease_expires_ms", 0);
  lease.revoked = fieldOr(fields, "lease_revoked", "false") == "true";
  return lease;
}

Fields
OperatorAuthorityLease::toFields() const
{
  return {
    {"lease_id", leaseId},
    {"lease_operator", operatorId},
    {"lease_drone", droneId},
    {"lease_scope", scope},
    {"lease_issued_ms", std::to_string(issuedMs)},
    {"lease_expires_ms", std::to_string(expiresMs)},
    {"lease_revoked", revoked ? "true" : "false"},
  };
}

bool
OperatorAuthorityLease::isFresh(uint64_t nowMs) const
{
  const auto current = nowMs == 0 ? nowMilliseconds() : nowMs;
  return !revoked && !leaseId.empty() && leaseId != "none" &&
         (expiresMs == 0 || current < expiresMs);
}

bool
OperatorAuthorityLease::allowsCommand(const std::string& targetDrone,
                                      const std::string& commandName,
                                      uint64_t nowMs,
                                      std::string& reason) const
{
  if (!isFresh(nowMs)) {
    reason = revoked ? "lease-revoked" : "lease-expired";
    return false;
  }
  if (!droneId.empty() && droneId != "all" && droneId != targetDrone) {
    reason = "wrong-drone";
    return false;
  }
  const bool monitorOnly = commandName == "telemetry" || commandName == "status" ||
                           commandName == "get_status";
  if (scope == "monitor") {
    reason = monitorOnly ? "ok" : "monitor-scope";
    return monitorOnly;
  }
  if (scope == "control" || scope == "mission" || scope == "admin") {
    reason = "ok";
    return true;
  }
  reason = "unsupported-scope";
  return false;
}

std::string
OperatorAuthorityLease::statusLine() const
{
  return "OperatorAuthorityLease id=" + leaseId +
         " operator=" + operatorId +
         " drone=" + droneId +
         " scope=" + scope +
         " revoked=" + std::string(revoked ? "true" : "false") +
         " expires=" + std::to_string(expiresMs);
}

OperatorAuthorityLeaseRequest
OperatorAuthorityLeaseRequest::fromFields(const Fields& fields)
{
  OperatorAuthorityLeaseRequest request;
  request.requestId = fieldOr(fields, "lease_request_id", request.requestId);
  request.operatorId = fieldOr(fields, "lease_operator", request.operatorId);
  request.droneId = fieldOr(fields, "lease_drone", request.droneId);
  request.scope = fieldOr(fields, "lease_scope", request.scope);
  request.ttlMs = uint64FieldOr(fields, "lease_ttl_ms", request.ttlMs);
  request.requestedMs = uint64FieldOr(fields, "lease_requested_ms", request.requestedMs);
  return request;
}

Fields
OperatorAuthorityLeaseRequest::toFields() const
{
  return {
    {"type", "operator-authority-lease-request"},
    {"lease_request_id", requestId},
    {"lease_operator", operatorId},
    {"lease_drone", droneId},
    {"lease_scope", scope},
    {"lease_ttl_ms", std::to_string(ttlMs)},
    {"lease_requested_ms", std::to_string(requestedMs)},
  };
}

bool
OperatorAuthorityLeaseRequest::isValid(std::string& reason) const
{
  if (operatorId.empty() || operatorId == "unknown") {
    reason = "missing-operator";
    return false;
  }
  if (droneId.empty()) {
    reason = "missing-drone";
    return false;
  }
  if (scope != "monitor" && scope != "control" && scope != "mission" && scope != "admin") {
    reason = "unsupported-scope";
    return false;
  }
  reason = "ok";
  return true;
}

std::string
OperatorAuthorityLeaseRequest::statusLine() const
{
  return "OperatorAuthorityLeaseRequest id=" + requestId +
         " operator=" + operatorId +
         " drone=" + droneId +
         " scope=" + scope +
         " ttl_ms=" + std::to_string(ttlMs);
}

SelectedDroneSummaryState
SelectedDroneSummaryState::fromStates(const std::string& selectedDrone,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<ReadinessState>& readiness,
                                      const std::optional<MissionState>& mission,
                                      const std::optional<MissionPlan>& missionPlan,
                                      const std::optional<MissionPart>& missionPart,
                                      const std::optional<MissionProgressState>& missionProgress,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<SafetyState>& safety)
{
  SelectedDroneSummaryState state;
  state.selectedDrone = selectedDrone.empty() ? "unknown" : selectedDrone;
  state.hasTelemetry = telemetry.has_value();
  state.readiness = readiness ? readiness->readiness :
                    telemetry ? telemetry->readiness : "unknown";
  state.missionPhase = mission ? mission->phase : "idle";
  state.missionProgressPhase = missionProgress ? missionProgress->phase : "idle";
  state.missionSegmentState = missionPart && missionProgress ? missionProgress->segmentStateForPart(
                              missionPart->id, state.missionProgressPhase) : "none";
  state.missionPlanTask = missionPlan ? missionPlan->taskId : "none";
  state.missionPartId = missionPart ? missionPart->id : "none";
  state.missionPartWaypoints = missionPart ? missionPart->waypoints.size() : 0;
  state.videoStatus = video ? video->status :
                      telemetry ? telemetry->video : "unknown";
  state.videoAdaptive = videoAdaptive ? videoAdaptive->compactSummary() : "unknown";
  state.linkState = safety ? safety->linkState :
                    telemetry ? telemetry->linkState : "unknown";

  const auto flight = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates(state.selectedDrone, readiness, safety));
  state.safetyAttention = flight.operatorAttention;
  state.canArm = flight.canArm;
  state.canTakeoff = flight.canTakeoff;
  state.canLand = flight.canLand;
  state.canManualControl = flight.canManualControl;
  state.canControlPanel = flight.canControlPanel;
  state.armReason = flight.armReason;
  state.takeoffReason = flight.takeoffReason;
  state.landReason = flight.landReason;
  state.manualControlReason = flight.manualControlReason;
  state.controlPanelReason = flight.controlPanelReason;
  return state;
}

std::string
SelectedDroneSummaryState::statusLine() const
{
  return "SelectedDroneSummary selected=" + selectedDrone +
         " has_telemetry=" + std::string(hasTelemetry ? "true" : "false") +
         " readiness=" + readiness +
         " mission=" + missionPhase +
         " mission_progress=" + missionProgressPhase +
         " mission_segment_state=" + missionSegmentState +
         " mission_plan=" + missionPlanTask +
         " mission_part=" + missionPartId +
         " mission_part_waypoints=" + std::to_string(missionPartWaypoints) +
         " video=" + videoStatus +
         " video_adaptive=" + videoAdaptive +
         " link=" + linkState +
         " safety_attention=" + std::string(safetyAttention ? "true" : "false") +
         " can_arm=" + std::string(canArm ? "true" : "false") +
         " arm_reason=" + armReason +
         " can_takeoff=" + std::string(canTakeoff ? "true" : "false") +
         " takeoff_reason=" + takeoffReason +
         " can_land=" + std::string(canLand ? "true" : "false") +
         " land_reason=" + landReason +
         " can_manual=" + std::string(canManualControl ? "true" : "false") +
         " manual_reason=" + manualControlReason +
         " can_panel=" + std::string(canControlPanel ? "true" : "false") +
         " panel_reason=" + controlPanelReason;
}

UavFunctionalityState
UavFunctionalityState::fromFields(const Fields& fields)
{
  UavFunctionalityState state;
  state.missionEditor = fieldOr(fields, "functionality_mission_editor", state.missionEditor);
  state.perDroneMissionReview = fieldOr(fields, "functionality_per_drone_mission_review",
                                        state.perDroneMissionReview);
  state.persistentMissionFiles = fieldOr(fields, "functionality_persistent_mission_files",
                                         state.persistentMissionFiles);
  state.recordingLogBrowsing = fieldOr(fields, "functionality_recording_log_browsing",
                                       state.recordingLogBrowsing);
  state.parameterStatusInspection = fieldOr(fields, "functionality_parameter_status_inspection",
                                            state.parameterStatusInspection);
  state.objectDetectionDisplay = fieldOr(fields, "functionality_object_detection_display",
                                         state.objectDetectionDisplay);
  state.multiDroneServiceSelection = fieldOr(fields, "functionality_multi_drone_service_selection",
                                             state.multiDroneServiceSelection);
  return state;
}

UavFunctionalityState
UavFunctionalityState::fromStates(const std::optional<MissionPlan>& missionPlan,
                                  const std::optional<MissionPart>& selectedMissionPart,
                                  const std::optional<RecordingDataProductState>& recording,
                                  const std::optional<TelemetryState>& telemetry,
                                  bool objectDetectionServiceAvailable,
                                  size_t droneCount)
{
  UavFunctionalityState state;
  if (missionPlan && !missionPlan->parts.empty()) {
    state.missionEditor = "prototype";
    state.persistentMissionFiles = "available";
  }
  if (selectedMissionPart && !selectedMissionPart->id.empty()) {
    state.perDroneMissionReview = "available";
  }
  if (recording && recording->isAvailable()) {
    state.recordingLogBrowsing = recording->isPlayable() ? "available" : "limited";
  }
  if (telemetry &&
      (telemetry->flightControllerBackend != "unknown" ||
       telemetry->flightControllerState != "unknown" ||
       telemetry->systemStatusName != "unknown" ||
       telemetry->batteryVoltageV != "unknown")) {
    state.parameterStatusInspection = "limited";
  }
  if (objectDetectionServiceAvailable) {
    state.objectDetectionDisplay = "metadata-only";
  }
  if (droneCount > 1) {
    state.multiDroneServiceSelection = "available";
  }
  return state;
}

Fields
UavFunctionalityState::toFields() const
{
  return {
    {"functionality_mission_editor", missionEditor},
    {"functionality_per_drone_mission_review", perDroneMissionReview},
    {"functionality_persistent_mission_files", persistentMissionFiles},
    {"functionality_recording_log_browsing", recordingLogBrowsing},
    {"functionality_parameter_status_inspection", parameterStatusInspection},
    {"functionality_object_detection_display", objectDetectionDisplay},
    {"functionality_multi_drone_service_selection", multiDroneServiceSelection},
  };
}

size_t
UavFunctionalityState::implementedCapabilityCount() const
{
  const std::vector<std::string> values{
    missionEditor,
    perDroneMissionReview,
    persistentMissionFiles,
    recordingLogBrowsing,
    parameterStatusInspection,
    objectDetectionDisplay,
    multiDroneServiceSelection,
  };
  return static_cast<size_t>(std::count_if(values.begin(), values.end(), [] (const std::string& value) {
    return value != "missing";
  }));
}

std::string
UavFunctionalityState::missingOrLimitedCapabilities() const
{
  std::vector<std::string> items;
  auto appendIfWeak = [&items] (const std::string& label, const std::string& value) {
    if (value == "missing" || value == "limited" || value == "prototype" || value == "metadata-only") {
      items.push_back(label + "=" + value);
    }
  };

  appendIfWeak("mission-editor", missionEditor);
  appendIfWeak("per-drone-review", perDroneMissionReview);
  appendIfWeak("persistent-mission-files", persistentMissionFiles);
  appendIfWeak("recording-log-browsing", recordingLogBrowsing);
  appendIfWeak("parameter-status-inspection", parameterStatusInspection);
  appendIfWeak("object-detection-display", objectDetectionDisplay);
  appendIfWeak("multi-drone-selection", multiDroneServiceSelection);

  if (items.empty()) {
    return "none";
  }

  std::ostringstream os;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << items[i];
  }
  return os.str();
}

std::string
UavFunctionalityState::statusLine() const
{
  return "UavFunctionality mission_editor=" + missionEditor +
         " per_drone_review=" + perDroneMissionReview +
         " persistent_mission_files=" + persistentMissionFiles +
         " recording_log_browsing=" + recordingLogBrowsing +
         " parameter_status_inspection=" + parameterStatusInspection +
         " object_detection_display=" + objectDetectionDisplay +
         " multi_drone_selection=" + multiDroneServiceSelection +
         " implemented=" + std::to_string(implementedCapabilityCount()) +
         " weak=" + missingOrLimitedCapabilities();
}

UavPracticalityState
UavPracticalityState::fromFields(const Fields& fields)
{
  UavPracticalityState state;
  state.preflightSummary = fieldOr(fields, "practicality_preflight_summary", state.preflightSummary);
  state.hardwareCompatibilityNotes = fieldOr(fields, "practicality_hardware_compatibility_notes",
                                             state.hardwareCompatibilityNotes);
  state.cameraDiagnostics = fieldOr(fields, "practicality_camera_diagnostics", state.cameraDiagnostics);
  state.flightControllerDiagnostics = fieldOr(fields, "practicality_flight_controller_diagnostics",
                                              state.flightControllerDiagnostics);
  state.configValidation = fieldOr(fields, "practicality_config_validation", state.configValidation);
  state.identityCertificateGuidance = fieldOr(fields, "practicality_identity_certificate_guidance",
                                             state.identityCertificateGuidance);
  state.operatorWorkflowGuidance = fieldOr(fields, "practicality_operator_workflow_guidance",
                                           state.operatorWorkflowGuidance);
  return state;
}

UavPracticalityState
UavPracticalityState::fromStates(const std::optional<TelemetryState>& telemetry,
                                 const std::optional<ReadinessState>& readiness,
                                 bool hasPreflightTool,
                                 bool hasRuntimeConfig,
                                 bool hasReleaseManual)
{
  UavPracticalityState state;
  state.preflightSummary = hasPreflightTool ? "available" : "missing";
  state.hardwareCompatibilityNotes = hasReleaseManual ? "documented" : "missing";
  state.configValidation = hasRuntimeConfig ? "available" : "missing";
  state.identityCertificateGuidance = hasReleaseManual ? "documented" : "missing";
  state.operatorWorkflowGuidance = hasReleaseManual ? "documented" : "missing";

  if (telemetry &&
      (telemetry->cameraAvailable != "unknown" ||
       telemetry->cameraSource != "unknown" ||
       telemetry->cameraReason != "unknown")) {
    state.cameraDiagnostics = telemetry->cameraAvailable == "true" ? "available" : "limited";
  }

  if ((telemetry &&
       (telemetry->flightControllerBackend != "unknown" ||
        telemetry->flightControllerAvailable != "unknown" ||
        telemetry->flightControllerReason != "unknown")) ||
      readiness.has_value()) {
    state.flightControllerDiagnostics = readiness && readiness->readinessReason == "ok" ?
                                        "available" : "limited";
  }

  return state;
}

Fields
UavPracticalityState::toFields() const
{
  return {
    {"practicality_preflight_summary", preflightSummary},
    {"practicality_hardware_compatibility_notes", hardwareCompatibilityNotes},
    {"practicality_camera_diagnostics", cameraDiagnostics},
    {"practicality_flight_controller_diagnostics", flightControllerDiagnostics},
    {"practicality_config_validation", configValidation},
    {"practicality_identity_certificate_guidance", identityCertificateGuidance},
    {"practicality_operator_workflow_guidance", operatorWorkflowGuidance},
  };
}

size_t
UavPracticalityState::practicalCapabilityCount() const
{
  const std::vector<std::string> values{
    preflightSummary,
    hardwareCompatibilityNotes,
    cameraDiagnostics,
    flightControllerDiagnostics,
    configValidation,
    identityCertificateGuidance,
    operatorWorkflowGuidance,
  };
  return static_cast<size_t>(std::count_if(values.begin(), values.end(), [] (const std::string& value) {
    return value != "missing";
  }));
}

std::string
UavPracticalityState::missingOrLimitedCapabilities() const
{
  std::vector<std::string> items;
  auto appendIfWeak = [&items] (const std::string& label, const std::string& value) {
    if (value == "missing" || value == "limited" || value == "documented") {
      items.push_back(label + "=" + value);
    }
  };

  appendIfWeak("preflight-summary", preflightSummary);
  appendIfWeak("hardware-notes", hardwareCompatibilityNotes);
  appendIfWeak("camera-diagnostics", cameraDiagnostics);
  appendIfWeak("flight-controller-diagnostics", flightControllerDiagnostics);
  appendIfWeak("config-validation", configValidation);
  appendIfWeak("identity-guidance", identityCertificateGuidance);
  appendIfWeak("operator-workflow", operatorWorkflowGuidance);

  if (items.empty()) {
    return "none";
  }

  std::ostringstream os;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << items[i];
  }
  return os.str();
}

std::string
UavPracticalityState::statusLine() const
{
  return "UavPracticality preflight=" + preflightSummary +
         " hardware_notes=" + hardwareCompatibilityNotes +
         " camera_diagnostics=" + cameraDiagnostics +
         " flight_controller_diagnostics=" + flightControllerDiagnostics +
         " config_validation=" + configValidation +
         " identity_guidance=" + identityCertificateGuidance +
         " operator_workflow=" + operatorWorkflowGuidance +
         " implemented=" + std::to_string(practicalCapabilityCount()) +
         " weak=" + missingOrLimitedCapabilities();
}

UavStabilityState
UavStabilityState::fromFields(const Fields& fields)
{
  UavStabilityState state;
  state.commandTimeoutHandling = fieldOr(fields, "stability_command_timeout_handling",
                                         state.commandTimeoutHandling);
  state.stopVideoIdempotence = fieldOr(fields, "stability_stop_video_idempotence",
                                       state.stopVideoIdempotence);
  state.streamSessionGuard = fieldOr(fields, "stability_stream_session_guard",
                                     state.streamSessionGuard);
  state.frameSequenceGuard = fieldOr(fields, "stability_frame_sequence_guard",
                                     state.frameSequenceGuard);
  state.adaptiveVideoPressure = fieldOr(fields, "stability_adaptive_video_pressure",
                                        state.adaptiveVideoPressure);
  state.telemetryFreshness = fieldOr(fields, "stability_telemetry_freshness",
                                     state.telemetryFreshness);
  state.manualNeutralFallback = fieldOr(fields, "stability_manual_neutral_fallback",
                                        state.manualNeutralFallback);
  state.longDurationProfiles = fieldOr(fields, "stability_long_duration_profiles",
                                       state.longDurationProfiles);
  return state;
}

UavStabilityState
UavStabilityState::fromStates(const std::optional<FlightCommandState>& command,
                              const std::optional<VideoState>& video,
                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                              const std::optional<TelemetryState>& telemetry,
                              const std::optional<SafetyState>& safety,
                              bool stopVideoGuardEnabled,
                              bool longDurationProfilesDocumented)
{
  UavStabilityState state;
  if (command) {
    state.commandTimeoutHandling = command->isTimeout() ? "operator-decision" : "available";
  }
  if (stopVideoGuardEnabled) {
    state.stopVideoIdempotence = "available";
  }
  if (video && (!video->streamId.empty() && video->streamId != "unknown")) {
    state.streamSessionGuard = "available";
  }
  if (video && video->framesPublished > 0) {
    state.frameSequenceGuard = "available";
  }
  if (videoAdaptive) {
    state.adaptiveVideoPressure = videoAdaptive->maxPressure() > 0 ? "active" : "available";
  }
  if (telemetry) {
    if (telemetry->telemetryIsFresh()) {
      state.telemetryFreshness = "fresh";
    }
    else if (telemetry->telemetryIsStale()) {
      state.telemetryFreshness = "stale";
    }
    else if (telemetry->telemetryIsMissing()) {
      state.telemetryFreshness = "missing-runtime";
    }
    else {
      state.telemetryFreshness = "available";
    }
  }
  if (safety) {
    state.manualNeutralFallback = safety->manualNeutralSent == "true" ? "available" : "armed";
  }
  state.longDurationProfiles = longDurationProfilesDocumented ? "documented" : "missing";
  return state;
}

Fields
UavStabilityState::toFields() const
{
  return {
    {"stability_command_timeout_handling", commandTimeoutHandling},
    {"stability_stop_video_idempotence", stopVideoIdempotence},
    {"stability_stream_session_guard", streamSessionGuard},
    {"stability_frame_sequence_guard", frameSequenceGuard},
    {"stability_adaptive_video_pressure", adaptiveVideoPressure},
    {"stability_telemetry_freshness", telemetryFreshness},
    {"stability_manual_neutral_fallback", manualNeutralFallback},
    {"stability_long_duration_profiles", longDurationProfiles},
  };
}

size_t
UavStabilityState::stableCapabilityCount() const
{
  const std::vector<std::string> values{
    commandTimeoutHandling,
    stopVideoIdempotence,
    streamSessionGuard,
    frameSequenceGuard,
    adaptiveVideoPressure,
    telemetryFreshness,
    manualNeutralFallback,
    longDurationProfiles,
  };
  return static_cast<size_t>(std::count_if(values.begin(), values.end(), [] (const std::string& value) {
    return value != "missing";
  }));
}

std::string
UavStabilityState::missingOrLimitedCapabilities() const
{
  std::vector<std::string> items;
  auto appendIfWeak = [&items] (const std::string& label, const std::string& value) {
    if (value == "missing" || value == "documented" || value == "armed" ||
        value == "stale" || value == "missing-runtime" || value == "operator-decision") {
      items.push_back(label + "=" + value);
    }
  };

  appendIfWeak("command-timeout", commandTimeoutHandling);
  appendIfWeak("stop-video", stopVideoIdempotence);
  appendIfWeak("stream-session", streamSessionGuard);
  appendIfWeak("frame-sequence", frameSequenceGuard);
  appendIfWeak("adaptive-video", adaptiveVideoPressure);
  appendIfWeak("telemetry-freshness", telemetryFreshness);
  appendIfWeak("manual-neutral", manualNeutralFallback);
  appendIfWeak("long-duration", longDurationProfiles);

  if (items.empty()) {
    return "none";
  }

  std::ostringstream os;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << items[i];
  }
  return os.str();
}

std::string
UavStabilityState::statusLine() const
{
  return "UavStability command_timeout=" + commandTimeoutHandling +
         " stop_video=" + stopVideoIdempotence +
         " stream_session=" + streamSessionGuard +
         " frame_sequence=" + frameSequenceGuard +
         " adaptive_video=" + adaptiveVideoPressure +
         " telemetry_freshness=" + telemetryFreshness +
         " manual_neutral=" + manualNeutralFallback +
         " long_duration=" + longDurationProfiles +
         " implemented=" + std::to_string(stableCapabilityCount()) +
         " weak=" + missingOrLimitedCapabilities();
}

namespace {

double
missionDistanceSq(const MissionWaypoint& a, const MissionWaypoint& b, double referenceLat)
{
  const auto latScale = 111320.0;
  const auto lonScale = 111320.0 * std::max(0.2, std::cos(referenceLat * M_PI / 180.0));
  const auto dLat = (a.lat - b.lat) * latScale;
  const auto dLon = (a.lon - b.lon) * lonScale;
  return dLat * dLat + dLon * dLon;
}

std::vector<MissionWaypoint>
nearestNeighborMissionRoute(std::vector<MissionWaypoint> points, double referenceLat)
{
  std::vector<MissionWaypoint> route;
  if (points.empty()) {
    return route;
  }
  auto startIt = std::min_element(points.begin(), points.end(),
    [] (const MissionWaypoint& a, const MissionWaypoint& b) {
      if (a.lat == b.lat) {
        return a.lon < b.lon;
      }
      return a.lat < b.lat;
    });
  route.push_back(*startIt);
  points.erase(startIt);
  while (!points.empty()) {
    const auto current = route.back();
    auto nextIt = std::min_element(points.begin(), points.end(),
      [current, referenceLat] (const MissionWaypoint& a, const MissionWaypoint& b) {
        return missionDistanceSq(current, a, referenceLat) <
               missionDistanceSq(current, b, referenceLat);
      });
    route.push_back(*nextIt);
    points.erase(nextIt);
  }
  return route;
}

} // namespace

MissionPlan
buildPatrolMissionPlan(const std::string& taskId,
                       double centerLat,
                       double centerLon,
                       double sideMeters,
                       const std::vector<std::string>& droneIds,
                       const std::vector<MissionWaypoint>& routeWaypoints,
                       const std::map<std::string, MissionWaypoint>& departurePoints)
{
  MissionPlan plan;
  plan.taskId = taskId;
  plan.completionObjective = "return-to-start";
  plan.returnHomePlanned = true;
  if (droneIds.empty()) {
    return plan;
  }

  sideMeters = std::clamp(sideMeters, 40.0, 1000.0);
  const auto latStep = sideMeters / 111320.0;
  const auto lonStep = sideMeters / (111320.0 * std::max(0.2, std::cos(centerLat * M_PI / 180.0)));

  if (routeWaypoints.size() >= 2) {
    const size_t clusterCount = std::min(droneIds.size(), routeWaypoints.size());
    const auto groups = clusterPatrolWaypointsDeterministic(routeWaypoints, clusterCount, centerLat);
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
      if (groups[groupIndex].empty()) {
        continue;
      }
      MissionPart part;
      part.id = "part" + std::to_string(plan.parts.size());
      part.role = "waypoint-cluster-" + std::to_string(groupIndex);
      part.waypoints = nearestNeighborMissionRoute(groups[groupIndex], centerLat);
      part.returnHomePlanned = true;
      plan.parts.push_back(std::move(part));
    }
  }

  if (plan.parts.empty()) {
    const auto spacing = lonStep * 1.20;
    const auto startLon = centerLon - spacing * (static_cast<double>(droneIds.size()) - 1.0) / 2.0;
    plan.parts.reserve(droneIds.size());
    for (size_t i = 0; i < droneIds.size(); ++i) {
      const auto sectorLon = startLon + spacing * static_cast<double>(i);
      const auto sectorLat = centerLat - latStep / 2.0;
      MissionPart part;
      part.id = "part" + std::to_string(plan.parts.size());
      part.role = "patrol-cluster-" + std::to_string(i);
      part.waypoints = {
        {sectorLat, sectorLon - lonStep / 2.0},
        {sectorLat + latStep, sectorLon - lonStep / 2.0},
        {sectorLat + latStep, sectorLon + lonStep / 2.0},
        {sectorLat, sectorLon + lonStep / 2.0},
      };
      part.returnHomePlanned = true;
      plan.parts.push_back(std::move(part));
    }
  }

  for (size_t i = 0; i < plan.parts.size(); ++i) {
    auto& part = plan.parts[i];
    part.assignedDrone = droneIds[i % droneIds.size()];
    const auto fallback = part.firstWaypointOr(MissionWaypoint{centerLat, centerLon});
    const auto it = departurePoints.find(part.assignedDrone);
    part.waypoints.push_back(it == departurePoints.end() ? fallback : it->second);
  }

  return plan;
}

std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet)
{
  Fields headerFields{
    {"stream_id", packet.streamId},
    {"stream_session_epoch", std::to_string(packet.streamSessionEpoch)},
    {"capture_ms", std::to_string(packet.captureMs)},
    {"bucket_packet_count", std::to_string(packet.bucketPacketCount)},
    {"media_sequence", std::to_string(packet.mediaSequence)},
    {"encoding", packet.encoding},
    {"frame_first_packet_seq", std::to_string(packet.frameFirstPacketSeq)},
    {"frame_last_packet_seq", std::to_string(packet.frameLastPacketSeq)},
    {"frame_segment_count", std::to_string(packet.frameSegmentCount)},
    {"frame_segment_index", std::to_string(packet.frameSegmentIndex)},
    {"frame_seq", std::to_string(packet.frameSeq)},
    {"key_frame", packet.keyFrame ? "true" : "false"},
    {"fec_data_shards", std::to_string(packet.fecDataShards)},
    {"fec_parity_shards", std::to_string(packet.fecParityShards)},
    {"fec_symbol_index", std::to_string(packet.fecSymbolIndex)},
    {"fec_symbol_count", std::to_string(packet.fecSymbolCount)},
    {"fec_data_lengths", packet.fecDataLengths},
    {"packet_seq", std::to_string(packet.packetSeq)},
    {"second", std::to_string(packet.second)},
  };
  if (packet.frameBindingVersion != 0) {
    headerFields["frame_binding_version"] = std::to_string(packet.frameBindingVersion);
    headerFields["source_frame_id"] = std::to_string(packet.sourceFrameId);
    headerFields["capture_origin_ns"] = std::to_string(packet.captureOriginNs);
    headerFields["capture_clock_id"] = packet.captureClockId;
    headerFields["codec_pts"] = std::to_string(packet.codecPts);
    headerFields["codec_time_base_num"] = std::to_string(packet.codecTimeBaseNum);
    headerFields["codec_time_base_den"] = std::to_string(packet.codecTimeBaseDen);
    headerFields["codec_config_epoch"] = std::to_string(packet.codecConfigEpoch);
  }
  const auto header = encodeFields(headerFields);
  if (header.size() > 0xffffffffULL) {
    throw std::runtime_error("video packet header too large");
  }

  std::vector<uint8_t> output;
  output.reserve(4 + header.size() + packet.payload.size());
  const auto headerSize = static_cast<uint32_t>(header.size());
  output.push_back(static_cast<uint8_t>((headerSize >> 24) & 0xff));
  output.push_back(static_cast<uint8_t>((headerSize >> 16) & 0xff));
  output.push_back(static_cast<uint8_t>((headerSize >> 8) & 0xff));
  output.push_back(static_cast<uint8_t>(headerSize & 0xff));
  output.insert(output.end(), header.begin(), header.end());
  output.insert(output.end(), packet.payload.begin(), packet.payload.end());
  return output;
}

VideoPacket
decodeVideoPacket(const std::vector<uint8_t>& payload)
{
  if (payload.size() < 4) {
    throw std::runtime_error("video packet too short");
  }
  const uint32_t headerSize =
    (static_cast<uint32_t>(payload[0]) << 24) |
    (static_cast<uint32_t>(payload[1]) << 16) |
    (static_cast<uint32_t>(payload[2]) << 8) |
    static_cast<uint32_t>(payload[3]);
  if (payload.size() < 4 + headerSize) {
    throw std::runtime_error("video packet header exceeds payload");
  }

  const auto header = decodeFields(std::string(
    reinterpret_cast<const char*>(payload.data() + 4), headerSize));
  VideoPacket packet;
  packet.streamId = fieldOr(header, "stream_id", "");
  packet.streamSessionEpoch = std::stoull(fieldOr(header, "stream_session_epoch", "0"));
  packet.second = std::stoull(fieldOr(header, "second", "0"));
  packet.packetSeq = std::stoull(fieldOr(header, "packet_seq", "0"));
  packet.frameSeq = std::stoull(fieldOr(header, "frame_seq", "0"));
  packet.captureMs = std::stoull(fieldOr(header, "capture_ms", "0"));
  packet.frameBindingVersion = std::stoull(
    fieldOr(header, "frame_binding_version", "0"));
  packet.sourceFrameId = std::stoull(fieldOr(header, "source_frame_id", "0"));
  packet.captureOriginNs = std::stoull(fieldOr(header, "capture_origin_ns", "0"));
  packet.captureClockId = fieldOr(header, "capture_clock_id", "");
  packet.codecPts = std::stoll(fieldOr(header, "codec_pts", "0"));
  packet.codecTimeBaseNum = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "codec_time_base_num", "0")));
  packet.codecTimeBaseDen = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "codec_time_base_den", "0")));
  packet.codecConfigEpoch = std::stoull(
    fieldOr(header, "codec_config_epoch", "0"));
  packet.frameFirstPacketSeq = std::stoull(fieldOr(header, "frame_first_packet_seq",
                                                   std::to_string(packet.packetSeq)));
  packet.frameLastPacketSeq = std::stoull(fieldOr(header, "frame_last_packet_seq",
                                                  std::to_string(packet.packetSeq)));
  packet.bucketPacketCount = std::stoull(fieldOr(header, "bucket_packet_count",
                                                 std::to_string(packet.packetSeq + 1)));
  packet.mediaSequence = std::stoull(fieldOr(header, "media_sequence",
                                              std::to_string(packet.packetSeq)));
  packet.frameSegmentIndex = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "frame_segment_index", "0")));
  packet.frameSegmentCount = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "frame_segment_count", "0")));
  packet.fecDataShards = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_data_shards", "0")));
  packet.fecParityShards = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_parity_shards", "0")));
  packet.fecSymbolIndex = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_symbol_index", "0")));
  packet.fecSymbolCount = static_cast<uint32_t>(
    std::stoul(fieldOr(header, "fec_symbol_count", "0")));
  packet.fecDataLengths = fieldOr(header, "fec_data_lengths", "");
  packet.keyFrame = fieldOr(header, "key_frame", "false") == "true";
  packet.encoding = fieldOr(header, "encoding", "");
  packet.payload.assign(payload.begin() + 4 + headerSize, payload.end());
  return packet;
}

bool
hasExactVideoFrameBinding(const VideoPacket& packet)
{
  return packet.frameBindingVersion == 1 &&
         packet.streamSessionEpoch != 0 &&
         packet.captureOriginNs != 0 &&
         !packet.captureClockId.empty() &&
         packet.codecTimeBaseNum != 0 &&
         packet.codecTimeBaseDen != 0 &&
         packet.codecConfigEpoch != 0;
}

void
validateVideoFrameBinding(const VideoPacket& packet,
                          uint64_t expectedSessionEpoch)
{
  if (!hasExactVideoFrameBinding(packet)) {
    throw std::invalid_argument("missing or unsupported UAV video frame binding");
  }
  if (packet.streamSessionEpoch != expectedSessionEpoch) {
    throw std::invalid_argument("stale UAV video frame binding session");
  }
  if (packet.codecTimeBaseDen > 1000000000U ||
      packet.codecTimeBaseNum > packet.codecTimeBaseDen) {
    throw std::invalid_argument("invalid UAV video codec time base");
  }
}

namespace {

std::vector<uint64_t>
parseVideoFecDataLengths(const std::string& value)
{
  std::vector<uint64_t> lengths;
  if (value.empty()) {
    return lengths;
  }
  std::stringstream parser(value);
  std::string token;
  while (std::getline(parser, token, ',')) {
    if (token.empty()) {
      continue;
    }
    lengths.push_back(std::stoull(token));
  }
  return lengths;
}

std::string
formatVideoFecDataLengths(const std::vector<uint64_t>& lengths)
{
  std::ostringstream os;
  for (size_t i = 0; i < lengths.size(); ++i) {
    if (i > 0) {
      os << ',';
    }
    os << lengths[i];
  }
  return os.str();
}

uint64_t
metadataUint64(const std::map<std::string, std::string>& metadata,
               const std::string& key,
               uint64_t fallback)
{
  const auto it = metadata.find(key);
  if (it == metadata.end() || it->second.empty()) {
    return fallback;
  }
  return std::stoull(it->second);
}

} // namespace

ndn_service_framework::StreamChunk
videoPacketToStreamChunk(const VideoPacket& packet)
{
  ndn_service_framework::StreamChunk chunk;
  chunk.streamId = packet.streamId;
  chunk.sessionEpoch = packet.streamSessionEpoch;
  chunk.seq = packet.packetSeq;
  chunk.payload = packet.payload;
  chunk.contentType = packet.encoding.empty() ? "video/h264" : packet.encoding;
  chunk.captureMs = packet.captureMs;
  chunk.keyChunk = packet.keyFrame;
  chunk.frameId = packet.frameSeq;
  chunk.frameFirstSeq = packet.frameFirstPacketSeq;
  chunk.frameLastSeq = packet.frameLastPacketSeq;
  chunk.segmentIndex = packet.frameSegmentIndex;
  chunk.segmentCount = packet.frameSegmentCount;
  chunk.metadata["uav.second"] = std::to_string(packet.second);
  chunk.metadata["uav.bucket_packet_count"] = std::to_string(packet.bucketPacketCount);
  chunk.metadata["uav.media_sequence"] = std::to_string(packet.mediaSequence);
  chunk.metadata["uav.frame_binding_version"] = std::to_string(packet.frameBindingVersion);
  chunk.metadata["uav.source_frame_id"] = std::to_string(packet.sourceFrameId);
  chunk.metadata["uav.capture_origin_ns"] = std::to_string(packet.captureOriginNs);
  chunk.metadata["uav.capture_clock_id"] = packet.captureClockId;
  chunk.metadata["uav.codec_pts"] = std::to_string(packet.codecPts);
  chunk.metadata["uav.codec_time_base_num"] = std::to_string(packet.codecTimeBaseNum);
  chunk.metadata["uav.codec_time_base_den"] = std::to_string(packet.codecTimeBaseDen);
  chunk.metadata["uav.codec_config_epoch"] = std::to_string(packet.codecConfigEpoch);

  if (packet.fecDataShards > 0 || packet.fecParityShards > 0 ||
      packet.fecSymbolCount > 0 || !packet.fecDataLengths.empty()) {
    ndn_service_framework::StreamFecInfo fec;
    fec.scheme = "xor-parity";
    fec.dataShards = packet.fecDataShards;
    fec.parityShards = packet.fecParityShards;
    fec.symbolIndex = packet.fecSymbolIndex;
    fec.symbolCount = packet.fecSymbolCount;
    fec.dataLengths = parseVideoFecDataLengths(packet.fecDataLengths);
    fec.sourceBlockId = std::to_string(packet.frameSeq);
    fec.repairSymbol = packet.fecDataShards > 0 &&
                       packet.fecSymbolIndex >= packet.fecDataShards;
    chunk.fec = fec;
  }
  return chunk;
}

VideoPacket
streamChunkToVideoPacket(const ndn_service_framework::StreamChunk& chunk)
{
  VideoPacket packet;
  packet.streamId = chunk.streamId;
  packet.streamSessionEpoch = chunk.sessionEpoch;
  packet.second = metadataUint64(chunk.metadata, "uav.second", 0);
  packet.packetSeq = chunk.seq;
  packet.frameSeq = chunk.frameId;
  packet.captureMs = chunk.captureMs;
  packet.frameFirstPacketSeq = chunk.frameFirstSeq;
  packet.frameLastPacketSeq = chunk.frameLastSeq;
  packet.bucketPacketCount =
    metadataUint64(chunk.metadata, "uav.bucket_packet_count", chunk.seq + 1);
  packet.mediaSequence =
    metadataUint64(chunk.metadata, "uav.media_sequence", chunk.seq);
  packet.frameBindingVersion =
    metadataUint64(chunk.metadata, "uav.frame_binding_version", 0);
  packet.sourceFrameId = metadataUint64(chunk.metadata, "uav.source_frame_id", 0);
  packet.captureOriginNs = metadataUint64(chunk.metadata, "uav.capture_origin_ns", 0);
  const auto captureClock = chunk.metadata.find("uav.capture_clock_id");
  packet.captureClockId = captureClock == chunk.metadata.end() ? "" : captureClock->second;
  const auto codecPts = chunk.metadata.find("uav.codec_pts");
  packet.codecPts = codecPts == chunk.metadata.end() ? 0 : std::stoll(codecPts->second);
  packet.codecTimeBaseNum = static_cast<uint32_t>(
    metadataUint64(chunk.metadata, "uav.codec_time_base_num", 0));
  packet.codecTimeBaseDen = static_cast<uint32_t>(
    metadataUint64(chunk.metadata, "uav.codec_time_base_den", 0));
  packet.codecConfigEpoch = metadataUint64(chunk.metadata, "uav.codec_config_epoch", 0);
  packet.frameSegmentIndex = static_cast<uint32_t>(chunk.segmentIndex);
  packet.frameSegmentCount = static_cast<uint32_t>(chunk.segmentCount);
  packet.keyFrame = chunk.keyChunk;
  packet.encoding = chunk.contentType;
  packet.payload = chunk.payload;

  if (chunk.fec) {
    packet.fecDataShards = static_cast<uint32_t>(chunk.fec->dataShards);
    packet.fecParityShards = static_cast<uint32_t>(chunk.fec->parityShards);
    packet.fecSymbolIndex = static_cast<uint32_t>(chunk.fec->symbolIndex);
    packet.fecSymbolCount = static_cast<uint32_t>(chunk.fec->symbolCount);
    packet.fecDataLengths = formatVideoFecDataLengths(chunk.fec->dataLengths);
  }
  return packet;
}

std::vector<uint8_t>
buildMockMavlinkFrame(const std::string& commandName, const Fields& params)
{
  auto body = encodeFields(params);
  body = "magic=MAVLINK-MOCK-v1;command=" + commandName + ";" + body;
  std::vector<uint8_t> frame;
  frame.push_back(0xfe);
  frame.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xff));
  frame.push_back(static_cast<uint8_t>(body.size() & 0xff));
  frame.insert(frame.end(), body.begin(), body.end());
  uint8_t checksum = 0;
  for (const auto byte : frame) {
    checksum ^= byte;
  }
  frame.push_back(checksum);
  return frame;
}

namespace {

uint16_t
mavlinkCrcAccumulate(uint8_t data, uint16_t crc)
{
  data ^= static_cast<uint8_t>(crc & 0xff);
  data ^= static_cast<uint8_t>(data << 4);
  return static_cast<uint16_t>(
    (crc >> 8) ^
    (static_cast<uint16_t>(data) << 8) ^
    (static_cast<uint16_t>(data) << 3) ^
    (static_cast<uint16_t>(data) >> 4));
}

uint16_t
mavlinkCrcX25(const std::vector<uint8_t>& bytes, uint8_t extra)
{
  uint16_t crc = 0xffff;
  for (const auto byte : bytes) {
    crc = mavlinkCrcAccumulate(byte, crc);
  }
  return mavlinkCrcAccumulate(extra, crc);
}

void
appendFloatLe(std::vector<uint8_t>& out, float value)
{
  static_assert(sizeof(float) == 4, "MAVLink float must be 32 bits");
  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  out.push_back(static_cast<uint8_t>(raw & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((raw >> 24) & 0xff));
}

void
appendUint16Le(std::vector<uint8_t>& out, uint16_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void
appendInt16Le(std::vector<uint8_t>& out, int16_t value)
{
  appendUint16Le(out, static_cast<uint16_t>(value));
}

void
appendUint32Le(std::vector<uint8_t>& out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void
appendInt32Le(std::vector<uint8_t>& out, int32_t value)
{
  appendUint32Le(out, static_cast<uint32_t>(value));
}

float
fieldFloatOr(const Fields& fields, const std::string& key, float fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  if (it->second == "true") {
    return 1.0F;
  }
  if (it->second == "false") {
    return 0.0F;
  }
  return std::stof(it->second);
}

uint8_t
fieldUint8Or(const Fields& fields, const std::string& key, uint8_t fallback)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  return static_cast<uint8_t>(std::stoul(it->second));
}

std::vector<uint8_t>
buildMavlinkV1Frame(uint8_t msgId, uint8_t crcExtra, uint8_t sourceSystem,
                    uint8_t sourceComponent, std::vector<uint8_t> payload)
{
  constexpr uint8_t mavlinkStx = 0xfe;
  static uint8_t sequence = 0;

  std::vector<uint8_t> checksumInput;
  checksumInput.reserve(5 + payload.size());
  checksumInput.push_back(static_cast<uint8_t>(payload.size()));
  checksumInput.push_back(sequence);
  checksumInput.push_back(sourceSystem);
  checksumInput.push_back(sourceComponent);
  checksumInput.push_back(msgId);
  checksumInput.insert(checksumInput.end(), payload.begin(), payload.end());
  const auto crc = mavlinkCrcX25(checksumInput, crcExtra);

  std::vector<uint8_t> frame;
  frame.reserve(8 + payload.size());
  frame.push_back(mavlinkStx);
  frame.insert(frame.end(), checksumInput.begin(), checksumInput.end());
  frame.push_back(static_cast<uint8_t>(crc & 0xff));
  frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xff));
  ++sequence;
  return frame;
}

int16_t
fieldInt16ClampedOr(const Fields& fields, const std::string& key,
                    int16_t fallback, int16_t minValue, int16_t maxValue)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  const auto value = std::stoi(it->second);
  return static_cast<int16_t>(std::clamp(value, static_cast<int>(minValue),
                                        static_cast<int>(maxValue)));
}

uint16_t
fieldUint16ClampedOr(const Fields& fields, const std::string& key,
                     uint16_t fallback, uint16_t maxValue)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return fallback;
  }
  const auto value = std::stoul(it->second);
  return static_cast<uint16_t>(std::min<unsigned long>(value, maxValue));
}

std::vector<uint8_t>
buildMavlinkManualControlFrame(const Fields& params)
{
  constexpr uint8_t manualControlMsgId = 69;
  constexpr uint8_t manualControlCrcExtra = 243;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  const auto x = fieldInt16ClampedOr(params, "x", 0, -1000, 1000);
  const auto y = fieldInt16ClampedOr(params, "y", 0, -1000, 1000);
  const auto z = fieldInt16ClampedOr(params, "z", 500, 0, 1000);
  const auto r = fieldInt16ClampedOr(params, "r", 0, -1000, 1000);
  const auto buttons = fieldUint16ClampedOr(params, "buttons", 0, 0xffff);

  std::vector<uint8_t> payload;
  payload.reserve(11);
  appendInt16Le(payload, x);
  appendInt16Le(payload, y);
  appendInt16Le(payload, z);
  appendInt16Le(payload, r);
  appendUint16Le(payload, buttons);
  payload.push_back(targetSystem);
  return buildMavlinkV1Frame(manualControlMsgId, manualControlCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkCommandLongFrame(const std::string& commandName, const Fields& params)
{
  constexpr uint8_t commandLongMsgId = 76;
  constexpr uint8_t commandLongCrcExtra = 152;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  uint16_t command = 0;
  std::array<float, 7> p = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  if (commandName == "arm") {
    command = 400; // MAV_CMD_COMPONENT_ARM_DISARM
    p[0] = fieldFloatOr(params, "arm", 1.0F);
  }
  else if (commandName == "disarm") {
    command = 400;
    p[0] = 0.0F;
  }
  else if (commandName == "emergency_stop") {
    command = 400; // MAV_CMD_COMPONENT_ARM_DISARM, force disarm magic.
    p[0] = 0.0F;
    p[1] = fieldFloatOr(params, "force_code", 21196.0F);
  }
  else if (commandName == "takeoff") {
    command = 22; // MAV_CMD_NAV_TAKEOFF
    p[6] = fieldFloatOr(params, "altitude_m", 15.0F);
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
  }
  else if (commandName == "land") {
    command = 21; // MAV_CMD_NAV_LAND
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
  }
  else if (commandName == "start_mission") {
    command = 300; // MAV_CMD_MISSION_START
    p[0] = fieldFloatOr(params, "first_item", 0.0F);
    p[1] = fieldFloatOr(params, "last_item", 0.0F);
  }
  else if (commandName == "goto" || commandName == "waypoint") {
    command = 16; // MAV_CMD_NAV_WAYPOINT
    p[0] = fieldFloatOr(params, "hold_time_s", 0.0F);
    p[4] = fieldFloatOr(params, "latitude", std::numeric_limits<float>::quiet_NaN());
    p[5] = fieldFloatOr(params, "longitude", std::numeric_limits<float>::quiet_NaN());
    p[6] = fieldFloatOr(params, "altitude_m", 15.0F);
  }
  else {
    return buildMockMavlinkFrame(commandName, params);
  }

  for (size_t i = 0; i < p.size(); ++i) {
    p[i] = fieldFloatOr(params, "param" + std::to_string(i + 1), p[i]);
  }

  std::vector<uint8_t> payload;
  payload.reserve(33);
  for (const auto value : p) {
    appendFloatLe(payload, value);
  }
  appendUint16Le(payload, command);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  payload.push_back(0); // confirmation
  return buildMavlinkV1Frame(commandLongMsgId, commandLongCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

} // namespace

std::vector<uint8_t>
buildMavlinkParamSetFrame(const std::string& paramName, float value,
                          uint8_t paramType, const Fields& params)
{
  constexpr uint8_t paramSetMsgId = 23;
  constexpr uint8_t paramSetCrcExtra = 168;

  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  std::vector<uint8_t> payload;
  payload.reserve(23);
  appendFloatLe(payload, value);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  for (size_t i = 0; i < 16; ++i) {
    payload.push_back(i < paramName.size() ? static_cast<uint8_t>(paramName[i]) : 0);
  }
  payload.push_back(paramType);
  return buildMavlinkV1Frame(paramSetMsgId, paramSetCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkHeartbeatFrame(const Fields& params)
{
  constexpr uint8_t heartbeatMsgId = 0;
  constexpr uint8_t heartbeatCrcExtra = 50;
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  const auto mavTypeGcs = fieldUint8Or(params, "type", 6);
  const auto autopilotInvalid = fieldUint8Or(params, "autopilot", 8);

  std::vector<uint8_t> payload;
  payload.reserve(9);
  appendUint32Le(payload, 0); // custom_mode
  payload.push_back(mavTypeGcs);
  payload.push_back(autopilotInvalid);
  payload.push_back(0); // base_mode
  payload.push_back(0); // system_status
  payload.push_back(3); // mavlink_version
  return buildMavlinkV1Frame(heartbeatMsgId, heartbeatCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkMissionCountFrame(uint16_t count, const Fields& params)
{
  constexpr uint8_t missionCountMsgId = 44;
  constexpr uint8_t missionCountCrcExtra = 221;
  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);

  std::vector<uint8_t> payload;
  payload.reserve(4);
  appendUint16Le(payload, count);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  return buildMavlinkV1Frame(missionCountMsgId, missionCountCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMavlinkMissionItemIntFrame(uint16_t seq, double latitude, double longitude,
                                float altitudeM, bool current,
                                const Fields& params)
{
  constexpr uint8_t missionItemIntMsgId = 73;
  constexpr uint8_t missionItemIntCrcExtra = 38;
  const auto targetSystem = fieldUint8Or(params, "target_system", 1);
  const auto targetComponent = fieldUint8Or(params, "target_component", 1);
  const auto sourceSystem = fieldUint8Or(params, "source_system", 255);
  const auto sourceComponent = fieldUint8Or(params, "source_component", 190);
  constexpr uint16_t mavCmdNavWaypoint = 16;
  constexpr uint8_t mavFrameGlobalRelativeAltInt = 6;

  std::vector<uint8_t> payload;
  payload.reserve(37);
  appendFloatLe(payload, fieldFloatOr(params, "hold_time_s", 0.0F));
  appendFloatLe(payload, fieldFloatOr(params, "acceptance_radius_m", 2.0F));
  appendFloatLe(payload, fieldFloatOr(params, "pass_radius_m", 0.0F));
  appendFloatLe(payload, fieldFloatOr(params, "yaw_deg", std::numeric_limits<float>::quiet_NaN()));
  appendInt32Le(payload, static_cast<int32_t>(std::llround(latitude * 10000000.0)));
  appendInt32Le(payload, static_cast<int32_t>(std::llround(longitude * 10000000.0)));
  appendFloatLe(payload, altitudeM);
  appendUint16Le(payload, seq);
  appendUint16Le(payload, mavCmdNavWaypoint);
  payload.push_back(targetSystem);
  payload.push_back(targetComponent);
  payload.push_back(mavFrameGlobalRelativeAltInt);
  payload.push_back(current ? 1 : 0);
  payload.push_back(1); // autocontinue
  return buildMavlinkV1Frame(missionItemIntMsgId, missionItemIntCrcExtra,
                             sourceSystem, sourceComponent, std::move(payload));
}

std::vector<uint8_t>
buildMockJpeg(const std::string& droneId, const std::string& frameId)
{
  const auto body = "mock-jpeg drone=" + droneId + " frame=" + frameId +
                    " timestamp_ms=" + std::to_string(nowMilliseconds());
  std::vector<uint8_t> image{0xff, 0xd8};
  image.insert(image.end(), body.begin(), body.end());
  image.push_back(0xff);
  image.push_back(0xd9);
  return image;
}

std::string
hexEncode(const std::vector<uint8_t>& value)
{
  std::ostringstream os;
  for (const auto byte : value) {
    os << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(byte);
  }
  return os.str();
}

std::vector<uint8_t>
hexDecode(const std::string& value)
{
  if (value.size() % 2 != 0) {
    throw std::runtime_error("invalid hex payload length");
  }
  std::vector<uint8_t> output;
  output.reserve(value.size() / 2);
  for (size_t i = 0; i < value.size(); i += 2) {
    output.push_back(static_cast<uint8_t>(
      std::stoi(value.substr(i, 2), nullptr, 16)));
  }
  return output;
}

std::string
makeMavlinkCommandPayload(const std::string& commandName,
                          const std::string& missionId,
                          const Fields& params)
{
  auto frame = buildMavlinkCommandLongFrame(commandName, params);
  if (commandName == "manual_control") {
    frame = buildMavlinkManualControlFrame(params);
  }
  Fields fields = params;
  fields["type"] = "mavlink-command";
  fields["command"] = commandName;
  fields["mavlink_encoding"] = "mavlink-mock";
  if (frame.size() > 5 && frame[0] == 0xfe) {
    if (frame[5] == 76) {
      fields["mavlink_encoding"] = "mavlink-v1-command-long";
    }
    else if (frame[5] == 69) {
      fields["mavlink_encoding"] = "mavlink-v1-manual-control";
    }
  }
  fields["mission_id"] = missionId;
  fields["timestamp_ms"] = std::to_string(nowMilliseconds());
  fields["mavlink_hex"] = hexEncode(frame);
  return encodeFields(fields);
}

std::string
makeMissionPayload(const std::string& missionId,
                   const std::string& role,
                   const std::string& area,
                   const std::vector<std::string>& waypoints,
                   bool captureRequired,
                   const std::string& objectDetectionService)
{
  std::ostringstream wp;
  for (size_t i = 0; i < waypoints.size(); ++i) {
    if (i > 0) {
      wp << '|';
    }
    wp << waypoints[i];
  }
  return encodeFields({
    {"type", "mission-plan"},
    {"mission_id", missionId},
    {"role", role},
    {"area", area},
    {"waypoints", wp.str()},
    {"capture_required", captureRequired ? "true" : "false"},
    {"object_detection_service", objectDetectionService},
  });
}

Fields
makeVideoStartFields(uint64_t fps, uint64_t requestedBitrateKbps,
                     uint64_t requestedFrameWidth, uint64_t fecParityShards)
{
  if (fecParityShards > 1) {
    throw std::invalid_argument("video fec parity shards must be 0 or 1");
  }
  return {
    {"type", "video-control"},
    {"action", "start"},
    {"fps", std::to_string(fps)},
    {"requested_bitrate_kbps", std::to_string(requestedBitrateKbps)},
    {"requested_frame_width", std::to_string(requestedFrameWidth)},
    {"fec_parity_shards", std::to_string(fecParityShards)},
  };
}

size_t
computeLiveVideoRetentionItems(uint64_t fps,
                               uint64_t dataShards,
                               uint64_t parityShards,
                               uint64_t retentionMs)
{
  if (fps == 0 || dataShards == 0 || retentionMs == 0 ||
      parityShards > std::numeric_limits<uint64_t>::max() - dataShards) {
    throw std::invalid_argument("invalid live video retention inputs");
  }
  const auto itemsPerFrame = dataShards + parityShards;
  const auto cap = static_cast<uint64_t>(UAV_VIDEO_MAX_RETAINED_ITEMS);
  // Saturate before multiplication. The configured cap is the authority; no
  // caller can turn a duration hint into unbounded live memory.
  if (fps > cap || itemsPerFrame > cap || retentionMs > cap * 1000 ||
      fps > std::numeric_limits<uint64_t>::max() / itemsPerFrame) {
    return UAV_VIDEO_MAX_RETAINED_ITEMS;
  }
  const auto itemsPerSecond = fps * itemsPerFrame;
  if (itemsPerSecond > std::numeric_limits<uint64_t>::max() / retentionMs) {
    return UAV_VIDEO_MAX_RETAINED_ITEMS;
  }
  const auto numerator = itemsPerSecond * retentionMs;
  const auto requested = numerator / 1000 + (numerator % 1000 == 0 ? 0 : 1);
  return static_cast<size_t>(std::clamp<uint64_t>(
    requested, itemsPerFrame, UAV_VIDEO_MAX_RETAINED_ITEMS));
}

uint64_t
parseVideoFecParityShards(const Fields& fields, uint64_t fallback)
{
  const auto text = fieldOr(fields, "fec_parity_shards", std::to_string(fallback));
  size_t consumed = 0;
  uint64_t value = 0;
  try {
    value = std::stoull(text, &consumed);
  }
  catch (const std::exception&) {
    throw std::invalid_argument("video fec parity shards must be 0 or 1");
  }
  if (consumed != text.size() || value > 1) {
    throw std::invalid_argument("video fec parity shards must be 0 or 1");
  }
  return value;
}

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback)
{
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

} // namespace ndnsf::examples::uav
