#include "UavProtocol.hpp"
#include "UavNames.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

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
  else if (key == "service-camera-recording-chunk-suffix") {
    config.serviceCameraRecordingChunkSuffix = ndn::Name(value);
  }
  else if (key == "service-gs-object-detection") {
    config.serviceGsObjectDetection = ndn::Name(value);
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
droneCameraRecordingChunkService(const std::string& droneId)
{
  return droneCameraRecordingChunkService(UavRuntimeConfig{}, droneId);
}

ndn::Name
droneCameraRecordingChunkService(const UavRuntimeConfig& config, const std::string& droneId)
{
  ndn::Name service = droneIdentity(config, droneId);
  for (const auto& component : config.serviceCameraRecordingChunkSuffix) {
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

std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet)
{
  const auto header = encodeFields({
    {"capture_ms", std::to_string(packet.captureMs)},
    {"bucket_packet_count", std::to_string(packet.bucketPacketCount)},
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
  });
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
  packet.second = std::stoull(fieldOr(header, "second", "0"));
  packet.packetSeq = std::stoull(fieldOr(header, "packet_seq", "0"));
  packet.frameSeq = std::stoull(fieldOr(header, "frame_seq", "0"));
  packet.captureMs = std::stoull(fieldOr(header, "capture_ms", "0"));
  packet.frameFirstPacketSeq = std::stoull(fieldOr(header, "frame_first_packet_seq",
                                                   std::to_string(packet.packetSeq)));
  packet.frameLastPacketSeq = std::stoull(fieldOr(header, "frame_last_packet_seq",
                                                  std::to_string(packet.packetSeq)));
  packet.bucketPacketCount = std::stoull(fieldOr(header, "bucket_packet_count",
                                                 std::to_string(packet.packetSeq + 1)));
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

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback)
{
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

} // namespace ndnsf::examples::uav
