#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::FlightCommandState;
using ndnsf::examples::uav::DroneListRowState;
using ndnsf::examples::uav::Fields;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionPart;
using ndnsf::examples::uav::MissionPlan;
using ndnsf::examples::uav::MissionProgressState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::MissionWaypoint;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::RecordingDataProductState;
using ndnsf::examples::uav::SafetyState;
using ndnsf::examples::uav::TelemetryState;
using ndnsf::examples::uav::VideoAdaptiveState;
using ndnsf::examples::uav::VideoAdaptivePolicyInput;
using ndnsf::examples::uav::VideoState;
using ndnsf::examples::uav::buildPatrolMissionPlan;
using ndnsf::examples::uav::computeVideoAdaptivePolicy;

ReadinessState
makeReadyState(bool armed)
{
  ReadinessState readiness;
  readiness.droneId = "A";
  readiness.heartbeatSeen = "true";
  readiness.flightControllerReady = "true";
  readiness.gpsReady = "true";
  readiness.ekfReady = "true";
  readiness.batteryReady = "true";
  readiness.armed = armed ? "true" : "false";
  readiness.readiness = "ready";
  readiness.readinessReason = "ok";
  readiness.mode = armed ? "GUIDED" : "STANDBY";
  readiness.landedStateName = "on-ground";
  return readiness;
}

SafetyState
makeSafeState()
{
  SafetyState safety;
  safety.droneId = "A";
  safety.linkState = "connected";
  safety.manualControlState = "idle";
  safety.manualNeutralSent = "true";
  safety.detail = "ok";
  return safety;
}

MissionState
makeMissionState(const std::string& phase)
{
  MissionState mission;
  mission.droneId = "A";
  mission.missionId = "mission-test";
  mission.partId = "part-A";
  mission.phase = phase;
  mission.detail = "test";
  return mission;
}

BOOST_AUTO_TEST_SUITE(UavProtocolState)

BOOST_AUTO_TEST_CASE(FlightSafetyGateCombinesReadinessAndSafety)
{
  std::string reason;
  auto gate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  BOOST_CHECK(gate.actionAllowed("arm", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "not-armed");
  BOOST_CHECK(!gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "not-armed");

  gate = FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState());
  BOOST_CHECK(gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("control_panel", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  auto airborne = makeReadyState(true);
  airborne.landedStateName = "in-air";
  gate = FlightSafetyGateState::fromStates("A", airborne, makeSafeState());
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "not-on-ground");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  TelemetryState airborneTelemetry;
  airborneTelemetry.heartbeatSeen = "true";
  airborneTelemetry.flightControllerReady = "true";
  airborneTelemetry.gpsReady = "true";
  airborneTelemetry.ekfReady = "true";
  airborneTelemetry.batteryReady = "true";
  airborneTelemetry.armed = "true";
  airborneTelemetry.readiness = "ready";
  airborneTelemetry.landedStateName = "in-air";
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("ready_for_takeoff"), "false");
  airborneTelemetry.landedStateName = "on-ground";
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("ready_for_takeoff"), "true");

  auto safety = makeSafeState();
  safety.linkState = "lost";
  gate = FlightSafetyGateState::fromStates("A", makeReadyState(true), safety);
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(!gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(!gate.actionAllowed("control_panel", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("emergency_stop", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
}

BOOST_AUTO_TEST_CASE(MissionStartGateCombinesMissionAndFlightReadiness)
{
  auto mission = makeMissionState("idle");
  auto flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  auto gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "mission-idle");
  BOOST_CHECK(!gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "mission-idle");

  mission = makeMissionState("uploaded");
  gate = MissionStartGateState::fromStates("A", mission, std::nullopt);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "no-flight-gate");
  BOOST_CHECK(gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "ok");

  flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "ok");
  BOOST_CHECK(gate.canStop);

  auto safety = makeSafeState();
  safety.linkState = "lost";
  flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(true), safety);
  gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "link-lost");
  BOOST_CHECK(gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "ok");
}

BOOST_AUTO_TEST_CASE(MissionProgressTracksCompensationAndCompletion)
{
  MissionProgressState progress;
  progress.taskId = "patrol-test";
  progress.phase = "waiting-compensation";
  progress.assignment = "clustered-waypoints-return-to-start";
  progress.drones = "A,B";
  progress.attempts = 1;
  progress.totalParts = 2;
  progress.completedParts = 1;
  progress.missingParts = 1;
  progress.compensatedParts = 0;
  progress.returnHomePlanned = true;
  progress.completedPartIds = "part1";
  progress.missingPartIds = "part0";
  progress.pendingPartIds = "none";

  BOOST_CHECK(progress.isActive());
  BOOST_CHECK(progress.needsCompensation());
  BOOST_CHECK(!progress.isComplete());
  BOOST_CHECK(!progress.isFailed());
  BOOST_CHECK_NE(progress.statusLine().find("return_home=true"), std::string::npos);
  BOOST_CHECK_NE(progress.statusLine().find("missing=part0"), std::string::npos);

  progress.phase = "completed";
  progress.attempts = 2;
  progress.completedParts = 2;
  progress.missingParts = 0;
  progress.compensatedParts = 1;
  progress.completedPartIds = "part0,part1";
  progress.missingPartIds = "none";
  progress.compensatedPartIds = "part0";

  BOOST_CHECK(!progress.isActive());
  BOOST_CHECK(!progress.needsCompensation());
  BOOST_CHECK(progress.isComplete());
  BOOST_CHECK(!progress.isFailed());
  BOOST_CHECK_NE(progress.statusLine().find("compensated_parts=1"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissionPlanClustersWaypointsAndReturnsHome)
{
  const std::vector<std::string> drones{"A", "B"};
  const std::vector<MissionWaypoint> route{
    {35.118600, -89.937500},
    {35.118700, -89.937400},
    {35.121000, -89.934000},
    {35.121100, -89.933900},
  };
  const std::map<std::string, MissionWaypoint> departures{
    {"A", {35.117000, -89.938000}},
    {"B", {35.122000, -89.933000}},
  };

  const auto plan = buildPatrolMissionPlan("patrol-test", 35.1186, -89.9375,
                                           140.0, drones, route, departures);
  BOOST_CHECK_EQUAL(plan.taskId, "patrol-test");
  BOOST_CHECK_EQUAL(plan.assignment, "clustered-waypoints-return-to-start");
  BOOST_CHECK_EQUAL(plan.parts.size(), 2);
  BOOST_CHECK(plan.returnHomePlanned);
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B");
  BOOST_CHECK_NE(plan.statusLine().find("parts=2"), std::string::npos);

  BOOST_CHECK_EQUAL(plan.parts[0].assignedDrone, "A");
  BOOST_CHECK_EQUAL(plan.parts[1].assignedDrone, "B");
  for (const auto& part : plan.parts) {
    BOOST_CHECK(part.returnHomePlanned);
    BOOST_CHECK_GE(part.waypoints.size(), 3);
    BOOST_CHECK_NE(part.waypointText().find(part.role + ":"), std::string::npos);
    const auto departure = departures.at(part.assignedDrone);
    BOOST_CHECK_CLOSE(part.waypoints.back().lat, departure.lat, 0.0001);
    BOOST_CHECK_CLOSE(part.waypoints.back().lon, departure.lon, 0.0001);
    BOOST_CHECK_NE(part.statusLine().find("return_home=true"), std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(MissionPlanBuildsDefaultSectorsWithoutRoute)
{
  const std::vector<std::string> drones{"A", "B", "C"};
  const auto plan = buildPatrolMissionPlan("patrol-auto", 35.1186, -89.9375,
                                           140.0, drones);
  BOOST_CHECK_EQUAL(plan.parts.size(), 3);
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B,C");
  for (size_t i = 0; i < plan.parts.size(); ++i) {
    const auto& part = plan.parts[i];
    BOOST_CHECK_EQUAL(part.id, "part" + std::to_string(i));
    BOOST_CHECK_EQUAL(part.assignedDrone, drones[i]);
    BOOST_CHECK_EQUAL(part.waypoints.size(), 5);
    BOOST_CHECK_EQUAL(part.waypoints.back().str(), part.waypoints.front().str());
  }
}

BOOST_AUTO_TEST_CASE(VideoAdaptiveStateRoundTripsAndReportsPressure)
{
  VideoAdaptiveState state;
  state.droneId = "A";
  state.state = "streaming";
  state.rttMs = 142;
  state.requestedBitrateKbps = 8000;
  state.acceptedBitrateKbps = 6000;
  state.suggestedBitrateKbps = 4000;
  state.bitrateAction = "decrease";
  state.bitrateReason = "pressure";
  state.window = 64;
  state.lookahead = 18;
  state.futureProbeLimit = 3;
  state.interestLifetimeMs = 620;
  state.missingTimeoutMs = 240;
  state.timeoutPressure = 55;
  state.probePressure = 20;
  state.duplicatePressure = 10;
  state.lossPressure = 8;
  state.backlogPressure = 30;
  state.primaryPressure = "timeout";
  state.policyReason = "pressure-timeout";
  state.pendingChunks = 12;
  state.receivedChunks = 100;
  state.timeouts = 2;
  state.nacks = 1;
  state.duplicates = 3;
  state.decodedFrames = 45;
  state.updatedMs = 123456;

  const auto decoded = VideoAdaptiveState::fromFields(state.toFields());
  BOOST_CHECK_EQUAL(decoded.droneId, "A");
  BOOST_CHECK_EQUAL(decoded.state, "streaming");
  BOOST_CHECK_EQUAL(decoded.rttMs, 142);
  BOOST_CHECK_EQUAL(decoded.requestedBitrateKbps, 8000);
  BOOST_CHECK_EQUAL(decoded.acceptedBitrateKbps, 6000);
  BOOST_CHECK_EQUAL(decoded.suggestedBitrateKbps, 4000);
  BOOST_CHECK_EQUAL(decoded.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(decoded.bitrateReason, "pressure");
  BOOST_CHECK_EQUAL(decoded.window, 64);
  BOOST_CHECK_EQUAL(decoded.missingTimeoutMs, 240);
  BOOST_CHECK_EQUAL(decoded.timeoutPressure, 55);
  BOOST_CHECK_EQUAL(decoded.primaryPressure, "timeout");
  BOOST_CHECK_EQUAL(decoded.policyReason, "pressure-timeout");
  BOOST_CHECK(decoded.underPressure());
  BOOST_CHECK_NE(decoded.statusLine().find("VideoAdaptive drone=A"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("suggested_bitrate_kbps=4000"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("bitrate_action=decrease"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("primary_pressure=timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("policy_reason=pressure-timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("window=64"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frames=45"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyShrinksUnderPressure)
{
  VideoAdaptivePolicyInput base;
  base.rttMs = 120;
  base.fps = 30;
  base.deltaPacketsPerSecond = 180;
  base.timeoutBudgetMs = 2500;
  base.dynamicWindowMax = 180;
  base.dynamicLookaheadMax = 80;
  base.decoderBacklogLimit = 80;
  base.receivedChunks = 1000;
  base.acceptedBitrateKbps = 8000;
  base.requestedBitrateKbps = 8000;

  auto pressured = base;
  pressured.timeoutPressure = 95;
  pressured.probePressure = 80;
  pressured.decoderPendingChunks = 120;
  pressured.timeouts = 120;
  pressured.nacks = 20;
  pressured.receivedChunks = 200;

  const auto relaxed = computeVideoAdaptivePolicy(base);
  const auto stressed = computeVideoAdaptivePolicy(pressured);

  BOOST_CHECK_LT(stressed.window, relaxed.window);
  BOOST_CHECK_LT(stressed.lookahead, relaxed.lookahead);
  BOOST_CHECK_LE(stressed.missingTimeoutMs, relaxed.missingTimeoutMs);
  BOOST_CHECK_EQUAL(stressed.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(stressed.bitrateReason, "pressure");
  BOOST_CHECK_EQUAL(stressed.primaryPressure, "backlog");
  BOOST_CHECK_EQUAL(stressed.policyReason, "pressure-backlog");
  BOOST_CHECK_LT(stressed.suggestedBitrateKbps, pressured.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyHandlesHighRttAndRecovery)
{
  VideoAdaptivePolicyInput highRtt;
  highRtt.rttMs = 950;
  highRtt.fps = 30;
  highRtt.deltaPacketsPerSecond = 180;
  highRtt.timeoutBudgetMs = 2500;
  highRtt.dynamicWindowMax = 180;
  highRtt.dynamicLookaheadMax = 80;
  highRtt.decoderBacklogLimit = 80;
  highRtt.receivedChunks = 1000;
  highRtt.acceptedBitrateKbps = 6000;
  highRtt.requestedBitrateKbps = 8000;

  const auto slowLink = computeVideoAdaptivePolicy(highRtt);
  BOOST_CHECK_EQUAL(slowLink.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(slowLink.bitrateReason, "high-rtt");
  BOOST_CHECK_EQUAL(slowLink.policyReason, "high-rtt");
  BOOST_CHECK_LT(slowLink.suggestedBitrateKbps, highRtt.acceptedBitrateKbps);

  auto recovering = highRtt;
  recovering.rttMs = 120;
  recovering.acceptedBitrateKbps = 2500;
  recovering.requestedBitrateKbps = 8000;

  const auto recovered = computeVideoAdaptivePolicy(recovering);
  BOOST_CHECK_EQUAL(recovered.bitrateAction, "increase");
  BOOST_CHECK_EQUAL(recovered.bitrateReason, "recovery");
  BOOST_CHECK_EQUAL(recovered.policyReason, "recovery");
  BOOST_CHECK_GT(recovered.suggestedBitrateKbps, recovering.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyIdentifiesPressureProfiles)
{
  VideoAdaptivePolicyInput base;
  base.rttMs = 120;
  base.fps = 30;
  base.deltaPacketsPerSecond = 180;
  base.timeoutBudgetMs = 2500;
  base.dynamicWindowMax = 180;
  base.dynamicLookaheadMax = 80;
  base.decoderBacklogLimit = 80;
  base.receivedChunks = 1000;
  base.acceptedBitrateKbps = 8000;
  base.requestedBitrateKbps = 8000;

  auto congestion = base;
  congestion.timeoutPressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(congestion).primaryPressure, "congestion");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(congestion).policyReason, "pressure-congestion");

  auto backlog = base;
  backlog.decoderPendingChunks = 120;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).primaryPressure, "backlog");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).policyReason, "pressure-backlog");

  auto probe = base;
  probe.probePressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).primaryPressure, "probe");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).policyReason, "pressure-probe");
}

BOOST_AUTO_TEST_CASE(DroneListRowStateUsesSharedTelemetryMissionAndVideoModels)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.batteryPercent = "87";
  telemetry.video = "streaming";
  telemetry.readiness = "ready";
  telemetry.armed = "true";
  telemetry.gpsFixName = "3d-fix";

  auto readiness = makeReadyState(true);
  auto mission = makeMissionState("executing");

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.rttMs = 115;
  adaptive.window = 36;
  adaptive.timeoutPressure = 30;
  adaptive.probePressure = 10;
  adaptive.backlogPressure = 55;
  adaptive.primaryPressure = "backlog";
  adaptive.acceptedBitrateKbps = 6000;
  adaptive.suggestedBitrateKbps = 4000;
  adaptive.bitrateAction = "decrease";
  adaptive.policyReason = "pressure-backlog";

  FlightCommandState command;
  command.droneId = "A";
  command.command = "takeoff";
  command.ackResult = "accepted";

  auto safety = makeSafeState();

  MissionProgressState progress;
  progress.phase = "executing";
  progress.drones = "A,B";

  BOOST_CHECK(progress.appliesToDrone("A"));
  BOOST_CHECK(progress.appliesToDrone("B"));
  BOOST_CHECK(!progress.appliesToDrone("C"));
  BOOST_CHECK_EQUAL(adaptive.maxPressure(), 55);
  BOOST_CHECK_NE(adaptive.compactSummary().find("pressure=55/backlog"), std::string::npos);

  const auto row = DroneListRowState::fromStates("A", true, telemetry, readiness,
                                                 mission, video, adaptive, command,
                                                 safety, progress);
  BOOST_CHECK(row.selected);
  BOOST_CHECK(row.hasTelemetry);
  BOOST_CHECK(row.hasReadiness);
  BOOST_CHECK(row.hasMission);
  BOOST_CHECK(row.hasMissionProgress);
  BOOST_CHECK(row.hasVideo);
  BOOST_CHECK(row.hasVideoAdaptive);
  BOOST_CHECK(row.hasCommand);
  BOOST_CHECK(row.hasSafety);
  BOOST_CHECK_EQUAL(row.readiness, "ready");
  BOOST_CHECK_EQUAL(row.armed, "true");
  BOOST_CHECK_EQUAL(row.gps, "true");
  BOOST_CHECK_EQUAL(row.battery, "87%");
  BOOST_CHECK_EQUAL(row.mission, "executing");
  BOOST_CHECK_EQUAL(row.missionProgress, "executing");
  BOOST_CHECK_EQUAL(row.video, "streaming");
  BOOST_CHECK_NE(row.rowText.find("Drone A active"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("progress=executing"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("adaptive=rtt=115ms"), std::string::npos);

  const auto unrelatedRow = DroneListRowState::fromStates("C", false, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          progress);
  BOOST_CHECK(!unrelatedRow.hasMissionProgress);
  BOOST_CHECK_EQUAL(unrelatedRow.missionProgress, "idle");
  BOOST_CHECK_NE(unrelatedRow.rowText.find("Drone C standby"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RecordingDataProductTracksEncryptedManifest)
{
  Fields fields{
    {"type", "camera-recording-manifest"},
    {"drone_id", "A"},
    {"recording_session_id", "record-123"},
    {"recording_object_prefix", "/example/uav/drone/A/repo/camera/recording"},
    {"recording_encryption", "hybrid-aes-256-gcm-at-rest"},
    {"recording_encryption_key_id", "/example/uav/drone/A/repo/key"},
    {"recording_encryption_content_key_hex", "00112233445566778899aabbccddeeff"},
    {"recording_chunks", "42"},
    {"recording_bytes", "123456"},
  };

  const auto product = RecordingDataProductState::fromFields(fields);
  BOOST_CHECK_EQUAL(product.droneId, "A");
  BOOST_CHECK_EQUAL(product.productType, "camera-recording");
  BOOST_CHECK_EQUAL(product.sessionId, "record-123");
  BOOST_CHECK_EQUAL(product.chunks, 42);
  BOOST_CHECK_EQUAL(product.bytes, 123456);
  BOOST_CHECK(product.isAvailable());
  BOOST_CHECK(product.isEncrypted());
  BOOST_CHECK(product.isPlayable());
  BOOST_CHECK_EQUAL(product.chunkObjectName(7),
                    "/example/uav/drone/A/repo/camera/recording/record-123/chunk/7");

  const auto roundTrip = RecordingDataProductState::fromFields(product.toFields());
  BOOST_CHECK_EQUAL(roundTrip.keyId, product.keyId);
  BOOST_CHECK_EQUAL(roundTrip.contentKey.size(), product.contentKey.size());
  BOOST_CHECK_NE(roundTrip.statusLine().find("RecordingDataProduct drone=A"), std::string::npos);
  BOOST_CHECK_NE(roundTrip.statusLine().find("playable=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RecordingDataProductRejectsEncryptedManifestWithoutKey)
{
  Fields fields{
    {"drone_id", "A"},
    {"recording_session_id", "record-123"},
    {"recording_object_prefix", "/example/uav/drone/A/repo/camera/recording"},
    {"recording_encryption", "hybrid-aes-256-gcm-at-rest"},
    {"recording_chunks", "2"},
    {"recording_bytes", "100"},
  };

  const auto product = RecordingDataProductState::fromFields(fields);
  BOOST_CHECK(product.isAvailable());
  BOOST_CHECK(product.isEncrypted());
  BOOST_CHECK(!product.isPlayable());
  BOOST_CHECK(product.toFields(false).count("recording_encryption_content_key_hex") == 0);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
