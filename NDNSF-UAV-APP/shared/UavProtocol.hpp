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

struct TelemetryState
{
  std::string droneId = "unknown";
  std::string lat = "unknown";
  std::string lon = "unknown";
  std::string altitudeM = "unknown";
  std::string groundspeedMps = "unknown";
  std::string batteryPercent = "unknown";
  std::string heartbeatSeen = "false";
  std::string flightControllerReady = "unknown";
  std::string gpsReady = "unknown";
  std::string ekfReady = "unknown";
  std::string batteryReady = "unknown";
  std::string armed = "unknown";
  std::string gpsFixType = "unknown";
  std::string gpsFixName = "unknown";
  std::string gpsSatellitesVisible = "unknown";
  std::string flightControllerState = "unknown";
  std::string systemStatus = "unknown";
  std::string systemStatusName = "unknown";
  std::string landedState = "unknown";
  std::string landedStateName = "unknown";
  std::string vtolStateName = "unknown";
  std::string batteryVoltageV = "unknown";
  std::string batteryCurrentA = "unknown";
  std::string readiness = "not-ready";
  std::string readinessReason = "waiting-heartbeat";
  std::string video = "unknown";
  std::string capture = "unknown";
  std::string recording = "unknown";
  uint64_t timestampMs = 0;

  static TelemetryState fromFields(const Fields& fields);
  Fields toFields() const;
  std::string statusLine() const;
  std::string mapSummary(const std::string& selectedDrone) const;
};

struct ReadinessState
{
  std::string droneId = "unknown";
  std::string heartbeatSeen = "false";
  std::string flightControllerReady = "unknown";
  std::string gpsReady = "unknown";
  std::string ekfReady = "unknown";
  std::string batteryReady = "unknown";
  std::string armed = "unknown";
  std::string mode = "unknown";
  std::string landedStateName = "unknown";
  std::string readiness = "not-ready";
  std::string readinessReason = "waiting-heartbeat";
  uint64_t timestampMs = 0;

  static ReadinessState fromFields(const Fields& fields);
  static ReadinessState fromTelemetry(const TelemetryState& telemetry);
  Fields toFields() const;
  bool readyForArm() const;
  bool readyForTakeoff() const;
  bool readyForLand() const;
  bool readyForManualControl() const;
  std::string statusLine() const;
};

struct VideoState
{
  std::string droneId = "unknown";
  std::string status = "unknown";
  std::string capture = "unknown";
  std::string recording = "unknown";
  std::string streamId = "unknown";
  std::string encoding = "unknown";
  std::string source = "unknown";
  uint64_t requestedBitrateKbps = 0;
  uint64_t acceptedBitrateKbps = 0;
  uint64_t requestedFrameWidth = 0;
  uint64_t acceptedFrameWidth = 0;
  uint64_t fps = 0;
  uint64_t streamPacketsPublished = 0;
  uint64_t framesPublished = 0;
  uint64_t fecGroupsPublished = 0;
  uint64_t recordingChunks = 0;
  uint64_t recordingBytes = 0;
  uint64_t rttMs = 0;
  uint64_t timeoutPressure = 0;
  uint64_t probePressure = 0;
  uint64_t backlogPressure = 0;
  uint64_t decodedFrames = 0;
  uint64_t updatedMs = 0;

  static VideoState fromFields(const Fields& fields);
  Fields toFields() const;
  bool isStreaming() const;
  std::string statusLine() const;
};

struct MissionState
{
  std::string droneId = "unknown";
  std::string missionId = "none";
  std::string partId = "none";
  std::string phase = "idle";
  std::string detail = "idle";
  std::string ack = "unknown";
  std::string transport = "unknown";
  std::string waypointsForwarded = "0";
  std::string waypointAcksAccepted = "0";
  uint64_t updatedMs = 0;

  static MissionState fromFields(const Fields& fields);
  Fields toFields() const;
  bool isIdle() const;
  bool isUploading() const;
  bool isUploaded() const;
  bool isExecuting() const;
  bool isStopping() const;
  bool isCompleted() const;
  bool isFailed() const;
  bool isCancelled() const;
  bool isTerminal() const;
  bool isAssigned() const;
  bool isBusyForAssignment() const;
  bool isStartable() const;
  bool isStoppable() const;
  std::string statusLine() const;
};

uint64_t
nowMilliseconds();

std::string
encodeFields(const Fields& fields);

Fields
decodeFields(const std::string& payload);

Fields
loadKeyValueConfig(const std::string& path);

std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet);

VideoPacket
decodeVideoPacket(const std::vector<uint8_t>& payload);

std::vector<uint8_t>
buildMockMavlinkFrame(const std::string& commandName, const Fields& params);

std::vector<uint8_t>
buildMavlinkHeartbeatFrame(const Fields& params = {});

std::vector<uint8_t>
buildMavlinkParamSetFrame(const std::string& paramName, float value,
                          uint8_t paramType, const Fields& params = {});

std::vector<uint8_t>
buildMavlinkMissionCountFrame(uint16_t count, const Fields& params = {});

std::vector<uint8_t>
buildMavlinkMissionItemIntFrame(uint16_t seq, double latitude, double longitude,
                                float altitudeM, bool current,
                                const Fields& params = {});

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
                   bool captureRequired,
                   const std::string& objectDetectionService = "/UAV/GS/ObjectDetection");

std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PROTOCOL_HPP
