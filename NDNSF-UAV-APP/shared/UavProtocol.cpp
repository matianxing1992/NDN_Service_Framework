#include "UavProtocol.hpp"
#include "UavNames.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

ndn::Name
droneIdentity(const std::string& droneId)
{
  if (droneId.empty()) {
    return DRONE_IDENTITY_PREFIX;
  }
  return ndn::Name(DRONE_IDENTITY_PREFIX).append(droneId);
}

ndn::Name
droneVideoControlService(const std::string& droneId)
{
  ndn::Name service = droneIdentity(droneId);
  for (const auto& component : SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX) {
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
  auto frame = buildMockMavlinkFrame(commandName, params);
  Fields fields = params;
  fields["type"] = "mavlink-command";
  fields["command"] = commandName;
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
                   bool captureRequired)
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
    {"object_detection_service", SERVICE_GS_OBJECT_DETECTION.toUri()},
  });
}

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback)
{
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

} // namespace ndnsf::examples::uav
