#ifndef NDNSF_EXAMPLES_UAV_PROTOCOL_HPP
#define NDNSF_EXAMPLES_UAV_PROTOCOL_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

using Fields = std::map<std::string, std::string>;

struct VideoPacket
{
  uint64_t second = 0;
  uint64_t packetSeq = 0;
  uint64_t frameSeq = 0;
  uint64_t captureMs = 0;
  uint64_t frameFirstPacketSeq = 0;
  uint64_t frameLastPacketSeq = 0;
  uint64_t bucketPacketCount = 0;
  uint32_t frameSegmentIndex = 0;
  uint32_t frameSegmentCount = 0;
  bool keyFrame = false;
  std::string encoding;
  uint32_t fecDataShards = 0;
  uint32_t fecParityShards = 0;
  uint32_t fecSymbolIndex = 0;
  uint32_t fecSymbolCount = 0;
  std::string fecDataLengths;
  std::vector<uint8_t> payload;
};

uint64_t
nowMilliseconds();

std::string
encodeFields(const Fields& fields);

Fields
decodeFields(const std::string& payload);

std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet);

VideoPacket
decodeVideoPacket(const std::vector<uint8_t>& payload);

std::vector<uint8_t>
buildMockMavlinkFrame(const std::string& commandName, const Fields& params);

std::vector<uint8_t>
buildMockJpeg(const std::string& droneId, const std::string& frameId);

std::string
hexEncode(const std::vector<uint8_t>& value);

std::vector<uint8_t>
hexDecode(const std::string& value);

std::string
makeMavlinkCommandPayload(const std::string& commandName,
                          const std::string& missionId,
                          const Fields& params);

std::string
makeMissionPayload(const std::string& missionId,
                   const std::string& role,
                   const std::string& area,
                   const std::vector<std::string>& waypoints,
                   bool captureRequired);

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PROTOCOL_HPP
