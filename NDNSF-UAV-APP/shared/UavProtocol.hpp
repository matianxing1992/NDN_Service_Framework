#ifndef NDNSF_EXAMPLES_UAV_PROTOCOL_HPP
#define NDNSF_EXAMPLES_UAV_PROTOCOL_HPP

#include <cstdint>
#include <map>
#include <optional>
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
  std::string linkState = "unknown";
  std::string manualControlState = "idle";
  std::string manualReplayActive = "false";
  std::string manualNeutralSent = "true";
  std::string manualFreshForMs = "0";
  std::string manualReplayCount = "0";
  std::string safetyDetail = "idle";
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
  bool landedForTakeoff() const;
  bool readyForTakeoff() const;
  bool readyForLand() const;
  bool readyForManualControl() const;
  std::string statusLine() const;
};

struct FlightCommandState
{
  std::string droneId = "unknown";
  std::string command = "none";
  std::string accepted = "unknown";
  std::string ackResult = "unknown";
  std::string flightControllerState = "unknown";
  std::string altitudeM = "unknown";
  std::string groundspeedMps = "unknown";
  std::string batteryPercent = "unknown";
  std::string forwardedBytes = "0";
  std::string detail = "idle";
  uint64_t updatedMs = 0;

  static FlightCommandState fromFields(const Fields& fields);
  Fields toFields() const;
  bool isAccepted() const;
  bool isTimeout() const;
  bool isSafetyCritical() const;
  std::string statusLine() const;
};

struct SafetyState
{
  std::string droneId = "unknown";
  std::string linkState = "unknown";
  std::string manualControlState = "idle";
  std::string manualReplayActive = "false";
  std::string manualNeutralSent = "true";
  uint64_t manualFreshForMs = 0;
  uint64_t manualReplayCount = 0;
  uint64_t linkAgeMs = 0;
  std::string lostLinkAction = "notify";
  std::string detail = "idle";
  uint64_t updatedMs = 0;

  static SafetyState fromFields(const Fields& fields);
  static SafetyState fromTelemetry(const TelemetryState& telemetry);
  Fields toFields() const;
  bool manualControlFresh() const;
  bool needsOperatorAttention() const;
  std::string statusLine() const;
};

struct FlightSafetyGateState
{
  std::string droneId = "unknown";
  bool hasReadiness = false;
  bool hasSafety = false;
  bool operatorAttention = false;
  std::string readiness = "unknown";
  std::string readinessReason = "no-telemetry";
  std::string armed = "unknown";
  std::string linkState = "unknown";
  std::string manualControlState = "unknown";
  bool canArm = false;
  bool canTakeoff = false;
  bool canLand = false;
  bool canManualControl = false;
  bool canControlPanel = false;
  bool canEmergencyStop = false;
  std::string armReason = "no-telemetry";
  std::string takeoffReason = "no-telemetry";
  std::string landReason = "no-telemetry";
  std::string manualControlReason = "no-telemetry";
  std::string controlPanelReason = "no-telemetry";
  std::string emergencyStopReason = "ok";

  static FlightSafetyGateState fromStates(const std::string& droneId,
                                          const std::optional<ReadinessState>& readiness,
                                          const std::optional<SafetyState>& safety);
  bool actionAllowed(const std::string& action, std::string& reason) const;
  std::string statusLine() const;
};

struct FlightActionControlState
{
  std::string selectedDrone = "unknown";
  bool hasReadiness = false;
  bool hasSafety = false;
  bool operatorAttention = false;
  bool canArm = false;
  bool canTakeoff = false;
  bool canLand = false;
  bool canManualControl = false;
  bool canControlPanel = false;
  bool canEmergencyStop = false;
  std::string armReason = "unknown";
  std::string takeoffReason = "unknown";
  std::string landReason = "unknown";
  std::string manualControlReason = "unknown";
  std::string controlPanelReason = "unknown";
  std::string emergencyStopReason = "unknown";
  std::string linkState = "unknown";
  std::string manualControlState = "unknown";

  static FlightActionControlState fromGate(const FlightSafetyGateState& gate);
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

struct VideoControlState
{
  std::string selectedDrone = "unknown";
  bool remoteStreaming = false;
  bool displayActive = false;
  bool canStart = true;
  bool canStop = false;

  static VideoControlState fromStates(const std::string& selectedDrone,
                                      const std::optional<VideoState>& video,
                                      bool displayActive);
  std::string statusLine() const;
};

struct VideoAdaptiveState
{
  std::string droneId = "unknown";
  std::string state = "idle";
  uint64_t rttMs = 0;
  uint64_t requestedBitrateKbps = 0;
  uint64_t acceptedBitrateKbps = 0;
  uint64_t suggestedBitrateKbps = 0;
  std::string bitrateAction = "hold";
  std::string bitrateReason = "unknown";
  uint64_t window = 0;
  uint64_t lookahead = 0;
  uint64_t futureProbeLimit = 0;
  uint64_t interestLifetimeMs = 0;
  uint64_t missingTimeoutMs = 0;
  uint64_t timeoutPressure = 0;
  uint64_t probePressure = 0;
  uint64_t duplicatePressure = 0;
  uint64_t lossPressure = 0;
  uint64_t backlogPressure = 0;
  std::string primaryPressure = "none";
  std::string policyReason = "stable";
  uint64_t pendingChunks = 0;
  uint64_t receivedChunks = 0;
  uint64_t timeouts = 0;
  uint64_t nacks = 0;
  uint64_t duplicates = 0;
  uint64_t decodedFrames = 0;
  uint64_t updatedMs = 0;

  static VideoAdaptiveState fromFields(const Fields& fields);
  Fields toFields() const;
  bool underPressure() const;
  uint64_t maxPressure() const;
  std::string compactSummary() const;
  std::string statusLine() const;
};

struct VideoAdaptivePolicyInput
{
  uint64_t rttMs = 120;
  uint64_t fps = 30;
  uint64_t deltaPacketsPerSecond = 160;
  uint64_t timeoutBudgetMs = 2500;
  uint64_t dynamicWindowMax = 128;
  uint64_t dynamicLookaheadMax = 64;
  uint64_t decoderBacklogLimit = 48;
  uint64_t decoderPendingChunks = 0;
  uint64_t receivedChunks = 0;
  uint64_t timeouts = 0;
  uint64_t nacks = 0;
  uint64_t timeoutPressure = 0;
  uint64_t probePressure = 0;
  uint64_t duplicatePressure = 0;
  uint64_t requestedBitrateKbps = 8000;
  uint64_t acceptedBitrateKbps = 8000;
};

struct VideoAdaptivePolicyDecision
{
  uint64_t window = 0;
  uint64_t lookahead = 0;
  uint64_t futureProbeLimit = 0;
  uint64_t probeBackoffMs = 0;
  uint64_t interestLifetimeMs = 0;
  uint64_t missingTimeoutMs = 0;
  uint64_t lossPressure = 0;
  uint64_t congestionPressure = 0;
  uint64_t probePressure = 0;
  uint64_t backlogPressure = 0;
  std::string primaryPressure = "none";
  std::string policyReason = "stable";
  uint64_t suggestedBitrateKbps = 0;
  std::string bitrateAction = "hold";
  std::string bitrateReason = "stable";
};

VideoAdaptivePolicyDecision
computeVideoAdaptivePolicy(const VideoAdaptivePolicyInput& input);

struct RecordingDataProductState
{
  std::string droneId = "unknown";
  std::string productType = "camera-recording";
  std::string sessionId;
  std::string objectPrefix;
  std::string encryption = "none";
  std::string keyId;
  std::vector<uint8_t> contentKey;
  uint64_t chunks = 0;
  uint64_t bytes = 0;
  uint64_t updatedMs = 0;

  static RecordingDataProductState fromFields(const Fields& fields,
                                              const std::string& fallbackDroneId = "unknown");
  Fields toFields(bool includeContentKey = true) const;
  bool isAvailable() const;
  bool isEncrypted() const;
  bool isPlayable() const;
  std::string chunkObjectName(uint64_t index) const;
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

struct MissionStartGateState
{
  std::string droneId = "unknown";
  bool hasMission = false;
  bool hasFlightGate = false;
  bool missionUploaded = false;
  bool canStart = false;
  bool canStop = false;
  std::string missionPhase = "idle";
  std::string startReason = "no-mission";
  std::string stopReason = "no-mission";

  static MissionStartGateState fromStates(const std::string& droneId,
                                          const std::optional<MissionState>& mission,
                                          const std::optional<FlightSafetyGateState>& flightGate);
  std::string statusLine() const;
};

struct MissionProgressState
{
  std::string taskId = "none";
  std::string phase = "idle";
  std::string assignment = "unknown";
  std::string drones = "none";
  uint64_t attempts = 0;
  uint64_t totalParts = 0;
  uint64_t completedParts = 0;
  uint64_t missingParts = 0;
  uint64_t compensatedParts = 0;
  bool returnHomePlanned = false;
  std::string completedPartIds = "none";
  std::string missingPartIds = "none";
  std::string compensatedPartIds = "none";
  std::string pendingPartIds = "none";

  bool isActive() const;
  bool needsCompensation() const;
  bool isComplete() const;
  bool isFailed() const;
  bool appliesToDrone(const std::string& droneId) const;
  std::string statusLine() const;
};

struct MissionControlState
{
  bool uploadPending = false;
  bool startPending = false;
  bool stopPending = false;
  bool hasUploaded = false;
  bool hasExecuting = false;
  bool hasStopping = false;
  bool hasTerminal = false;
  bool hasProgress = false;
  bool progressActive = false;
  bool progressNeedsCompensation = false;
  bool progressComplete = false;
  bool progressFailed = false;
  bool canUpload = true;
  bool canStart = false;
  bool canStop = false;
  size_t startableCount = 0;
  size_t startEligibleCount = 0;
  size_t startBlockedCount = 0;
  std::string progressPhase = "idle";
  std::string phases = "none";
  std::string startEligible = "none";
  std::string startBlocked = "none";
  std::string uploadReason = "ok";
  std::string startReason = "no-uploaded-mission";
  std::string stopReason = "no-active-mission";

  static MissionControlState fromStates(const std::vector<MissionStartGateState>& missionGates,
                                        const std::optional<MissionProgressState>& progress,
                                        bool uploadPending,
                                        bool startPending,
                                        bool stopPending);
  std::string statusLine() const;
};

struct SelectedActionState
{
  std::string selectedDrone = "unknown";
  FlightActionControlState flight;
  MissionControlState mission;
  bool manualMode = false;
  bool manualInputActive = false;
  bool emergencyStopAvailable = false;

  static SelectedActionState fromStates(const std::string& selectedDrone,
                                        const FlightActionControlState& flight,
                                        const MissionControlState& mission,
                                        bool manualMode,
                                        bool manualInputActive);
  std::string statusLine() const;
};

struct DroneListRowState
{
  std::string droneId;
  bool selected = false;
  bool hasTelemetry = false;
  bool hasReadiness = false;
  bool hasMission = false;
  bool hasVideo = false;
  bool hasCommand = false;
  bool hasSafety = false;
  bool hasMissionProgress = false;
  bool hasVideoAdaptive = false;
  std::string readiness = "unknown";
  std::string armed = "unknown";
  std::string gps = "unknown";
  std::string battery = "unknown";
  std::string mission = "idle";
  std::string missionProgress = "idle";
  std::string video = "unknown";
  std::string videoAdaptive = "unknown";
  std::string command = "none";
  std::string safety = "unknown";
  std::string rowText;

  static DroneListRowState fromStates(const std::string& droneId,
                                      bool selected,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<ReadinessState>& readiness,
                                      const std::optional<MissionState>& mission,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<FlightCommandState>& command,
                                      const std::optional<SafetyState>& safety,
                                      const std::optional<MissionProgressState>& progress);
};

struct MissionWaypoint
{
  double lat = 0.0;
  double lon = 0.0;

  std::string str() const;
};

struct MissionPart
{
  std::string id;
  std::string role;
  std::string assignedDrone;
  std::string completedBy;
  std::vector<MissionWaypoint> waypoints;
  int attempt = 0;
  bool done = false;
  bool returnHomePlanned = false;

  MissionWaypoint firstWaypointOr(MissionWaypoint fallback) const;
  std::vector<std::string> waypointStrings() const;
  std::string waypointText() const;
  std::string statusLine() const;
};

struct MissionPlan
{
  std::string taskId;
  std::string assignment = "clustered-waypoints-return-to-start";
  std::vector<MissionPart> parts;
  bool returnHomePlanned = false;

  std::string droneList() const;
  std::string statusLine() const;
};

struct SelectedDroneSummaryState
{
  std::string selectedDrone = "unknown";
  bool hasTelemetry = false;
  std::string readiness = "unknown";
  std::string missionPhase = "unknown";
  std::string missionProgressPhase = "unknown";
  std::string missionPlanTask = "none";
  std::string missionPartId = "none";
  uint64_t missionPartWaypoints = 0;
  std::string videoStatus = "unknown";
  std::string videoAdaptive = "unknown";
  std::string linkState = "unknown";
  bool safetyAttention = false;
  bool canArm = false;
  bool canTakeoff = false;
  bool canLand = false;
  bool canManualControl = false;
  bool canControlPanel = false;
  std::string armReason = "unknown";
  std::string takeoffReason = "unknown";
  std::string landReason = "unknown";
  std::string manualControlReason = "unknown";
  std::string controlPanelReason = "unknown";

  static SelectedDroneSummaryState fromStates(const std::string& selectedDrone,
                                              const std::optional<TelemetryState>& telemetry,
                                              const std::optional<ReadinessState>& readiness,
                                              const std::optional<MissionState>& mission,
                                              const std::optional<MissionPlan>& missionPlan,
                                              const std::optional<MissionPart>& missionPart,
                                              const std::optional<MissionProgressState>& missionProgress,
                                              const std::optional<VideoState>& video,
                                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                                              const std::optional<SafetyState>& safety);
  std::string statusLine() const;
};

MissionPlan
buildPatrolMissionPlan(const std::string& taskId,
                       double centerLat,
                       double centerLon,
                       double sideMeters,
                       const std::vector<std::string>& droneIds,
                       const std::vector<MissionWaypoint>& routeWaypoints = {},
                       const std::map<std::string, MissionWaypoint>& departurePoints = {});

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
