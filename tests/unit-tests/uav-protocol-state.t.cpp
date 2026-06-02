#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::FlightActionControlState;
using ndnsf::examples::uav::FlightCommandState;
using ndnsf::examples::uav::DroneListRowState;
using ndnsf::examples::uav::Fields;
using ndnsf::examples::uav::MissionControlState;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionPart;
using ndnsf::examples::uav::MissionPlan;
using ndnsf::examples::uav::MissionProgressState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::MissionWaypoint;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::RecordingDataProductState;
using ndnsf::examples::uav::SafetyState;
using ndnsf::examples::uav::SelectedActionState;
using ndnsf::examples::uav::SelectedDroneSummaryState;
using ndnsf::examples::uav::TelemetryState;
using ndnsf::examples::uav::VideoAdaptiveState;
using ndnsf::examples::uav::VideoAdaptivePolicyInput;
using ndnsf::examples::uav::VideoControlState;
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
  airborneTelemetry.flightControllerBackend = "mock";
  airborneTelemetry.flightControllerAvailable = "true";
  airborneTelemetry.flightControllerState = "mock-ready";
  airborneTelemetry.flightControllerReason = "ok";
  airborneTelemetry.cameraAvailable = "true";
  airborneTelemetry.cameraSource = "/dev/video42";
  airborneTelemetry.cameraReason = "ok";
  airborneTelemetry.armed = "true";
  airborneTelemetry.readiness = "ready";
  airborneTelemetry.landedStateName = "in-air";
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("ready_for_takeoff"), "false");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("flight_controller_backend"), "mock");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("flight_controller_available"), "true");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("camera_available"), "true");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("camera_source"), "/dev/video42");
  const auto telemetryRoundTrip = TelemetryState::fromFields(airborneTelemetry.toFields());
  BOOST_CHECK_EQUAL(telemetryRoundTrip.flightControllerBackend, "mock");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.flightControllerAvailable, "true");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.cameraAvailable, "true");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.cameraReason, "ok");
  BOOST_CHECK_NE(telemetryRoundTrip.statusLine().find("fc_backend=mock"), std::string::npos);
  BOOST_CHECK_NE(telemetryRoundTrip.statusLine().find("camera_available=true"), std::string::npos);
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

BOOST_AUTO_TEST_CASE(FlightActionControlStateMirrorsSafetyGate)
{
  const auto readyGate = FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState());
  auto action = FlightActionControlState::fromGate(readyGate);
  BOOST_CHECK_EQUAL(action.selectedDrone, "A");
  BOOST_CHECK(action.hasReadiness);
  BOOST_CHECK(action.hasSafety);
  BOOST_CHECK(action.canTakeoff);
  BOOST_CHECK(action.canLand);
  BOOST_CHECK(action.canManualControl);
  BOOST_CHECK(action.canControlPanel);
  BOOST_CHECK(action.canEmergencyStop);
  BOOST_CHECK_EQUAL(action.takeoffReason, "ok");
  BOOST_CHECK_NE(action.statusLine().find("can_takeoff=true"), std::string::npos);
  BOOST_CHECK_NE(action.statusLine().find("emergency_stop=true"), std::string::npos);

  auto safety = makeSafeState();
  safety.linkState = "lost";
  action = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates("A", makeReadyState(true), safety));
  BOOST_CHECK(!action.canTakeoff);
  BOOST_CHECK(!action.canManualControl);
  BOOST_CHECK(action.canEmergencyStop);
  BOOST_CHECK_EQUAL(action.takeoffReason, "link-lost");
  BOOST_CHECK_EQUAL(action.manualControlReason, "link-lost");
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

BOOST_AUTO_TEST_CASE(MissionControlStateCombinesGatesAndProgress)
{
  MissionStartGateState readyA;
  readyA.droneId = "A";
  readyA.hasMission = true;
  readyA.hasFlightGate = true;
  readyA.missionUploaded = true;
  readyA.missionPhase = "uploaded";
  readyA.canStart = true;
  readyA.startReason = "ok";
  readyA.canStop = true;
  readyA.stopReason = "ok";

  auto control = MissionControlState::fromStates({readyA}, std::nullopt, false, false, false);
  BOOST_CHECK(control.canUpload);
  BOOST_CHECK(control.canStart);
  BOOST_CHECK(control.canStop);
  BOOST_CHECK_EQUAL(control.startableCount, 1);
  BOOST_CHECK_EQUAL(control.startEligible, "A");
  BOOST_CHECK_EQUAL(control.startReason, "ok");
  BOOST_CHECK_NE(control.statusLine().find("can_start=true"), std::string::npos);

  auto blockedB = readyA;
  blockedB.droneId = "B";
  blockedB.canStart = false;
  blockedB.startReason = "link-lost";
  control = MissionControlState::fromStates({readyA, blockedB}, std::nullopt, false, false, false);
  BOOST_CHECK(!control.canStart);
  BOOST_CHECK_EQUAL(control.startableCount, 2);
  BOOST_CHECK_EQUAL(control.startEligibleCount, 1);
  BOOST_CHECK_EQUAL(control.startBlockedCount, 1);
  BOOST_CHECK_EQUAL(control.startBlocked, "B:link-lost");
  BOOST_CHECK_EQUAL(control.startReason, "blocked-B:link-lost");

  MissionProgressState progress;
  progress.phase = "executing";
  progress.totalParts = 2;
  progress.completedParts = 1;
  control = MissionControlState::fromStates({readyA}, progress, false, false, false);
  BOOST_CHECK(!control.canUpload);
  BOOST_CHECK(!control.canStart);
  BOOST_CHECK(control.canStop);
  BOOST_CHECK(control.progressActive);
  BOOST_CHECK_EQUAL(control.uploadReason, "progress-active");
  BOOST_CHECK_EQUAL(control.startReason, "progress-active");
}

BOOST_AUTO_TEST_CASE(SelectedActionStateCombinesFlightMissionAndManualMode)
{
  MissionStartGateState missionGate;
  missionGate.droneId = "A";
  missionGate.hasMission = true;
  missionGate.hasFlightGate = true;
  missionGate.missionUploaded = true;
  missionGate.missionPhase = "uploaded";
  missionGate.canStart = true;
  missionGate.startReason = "ok";
  missionGate.canStop = true;
  missionGate.stopReason = "ok";

  const auto mission = MissionControlState::fromStates({missionGate}, std::nullopt,
                                                       false, false, false);
  const auto flight = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState()));
  const auto action = SelectedActionState::fromStates("A", flight, mission, true, true);

  BOOST_CHECK_EQUAL(action.selectedDrone, "A");
  BOOST_CHECK(action.flight.canTakeoff);
  BOOST_CHECK(action.flight.canManualControl);
  BOOST_CHECK(action.mission.canStart);
  BOOST_CHECK(action.mission.canStop);
  BOOST_CHECK(action.manualMode);
  BOOST_CHECK(action.manualInputActive);
  BOOST_CHECK(action.emergencyStopAvailable);
  BOOST_CHECK_NE(action.statusLine().find("mission_can_start=true"), std::string::npos);
  BOOST_CHECK_NE(action.statusLine().find("manual_mode=true"), std::string::npos);
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
  state.publishedFrames = 90;
  state.decodedFrames = 45;
  state.decodedFrameGap = 45;
  state.frameGapPressure = 35;
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
  BOOST_CHECK_EQUAL(decoded.publishedFrames, 90);
  BOOST_CHECK_EQUAL(decoded.decodedFrameGap, 45);
  BOOST_CHECK_EQUAL(decoded.frameGapPressure, 35);
  BOOST_CHECK(decoded.underPressure());
  BOOST_CHECK_NE(decoded.statusLine().find("VideoAdaptive drone=A"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("suggested_bitrate_kbps=4000"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("bitrate_action=decrease"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("primary_pressure=timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("policy_reason=pressure-timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("window=64"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("published_frames=90"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frame_gap=45"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("frame_gap_pressure=35"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frames=45"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(VideoControlStateDerivesStartStopActions)
{
  const auto idle = VideoControlState::fromStates("A", std::nullopt, false);
  BOOST_CHECK_EQUAL(idle.selectedDrone, "A");
  BOOST_CHECK(!idle.remoteStreaming);
  BOOST_CHECK(!idle.displayActive);
  BOOST_CHECK(idle.canStart);
  BOOST_CHECK(!idle.canStop);
  BOOST_CHECK_NE(idle.statusLine().find("can_start=true"), std::string::npos);

  VideoState streaming;
  streaming.droneId = "A";
  streaming.status = "streaming";
  const auto remoteStreaming = VideoControlState::fromStates("A", streaming, false);
  BOOST_CHECK(remoteStreaming.remoteStreaming);
  BOOST_CHECK(!remoteStreaming.displayActive);
  BOOST_CHECK(!remoteStreaming.canStart);
  BOOST_CHECK(remoteStreaming.canStop);

  VideoState stopped;
  stopped.droneId = "A";
  stopped.status = "stopped";
  const auto localDisplay = VideoControlState::fromStates("A", stopped, true);
  BOOST_CHECK(!localDisplay.remoteStreaming);
  BOOST_CHECK(localDisplay.displayActive);
  BOOST_CHECK(!localDisplay.canStart);
  BOOST_CHECK(localDisplay.canStop);
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

  auto timeout = base;
  timeout.timeoutPressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(timeout).primaryPressure, "timeout");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(timeout).policyReason, "pressure-timeout");

  auto loss = base;
  loss.timeouts = 120;
  loss.nacks = 80;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(loss).primaryPressure, "loss");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(loss).policyReason, "pressure-loss");

  auto duplicate = base;
  duplicate.duplicatePressure = 180;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(duplicate).primaryPressure, "duplicate");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(duplicate).policyReason, "pressure-duplicate");

  auto backlog = base;
  backlog.decoderPendingChunks = 120;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).primaryPressure, "backlog");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).policyReason, "pressure-backlog");

  auto probe = base;
  probe.probePressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).primaryPressure, "probe");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).policyReason, "pressure-probe");

  auto decodeGap = base;
  decodeGap.publishedFrames = 120;
  decodeGap.decodedFrames = 10;
  const auto decodeGapDecision = computeVideoAdaptivePolicy(decodeGap);
  BOOST_CHECK_EQUAL(decodeGapDecision.primaryPressure, "decode-gap");
  BOOST_CHECK_EQUAL(decodeGapDecision.policyReason, "pressure-decode-gap");
  BOOST_CHECK_EQUAL(decodeGapDecision.bitrateAction, "decrease");
  BOOST_CHECK_GT(decodeGapDecision.frameGapPressure, 0);
  BOOST_CHECK_LT(decodeGapDecision.suggestedBitrateKbps, decodeGap.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(SelectedDroneSummaryStateUsesSharedModels)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.readiness = "ready";
  telemetry.video = "streaming";
  telemetry.linkState = "connected";

  auto readiness = makeReadyState(true);
  auto mission = makeMissionState("uploaded");

  MissionPlan plan;
  plan.taskId = "patrol-test";
  plan.assignment = "clustered-waypoints-return-to-start";

  MissionPart part;
  part.id = "part-A";
  part.assignedDrone = "A";
  part.waypoints = {{35.1186, -89.9375}, {35.1187, -89.9374}};
  plan.parts.push_back(part);

  MissionProgressState progress;
  progress.phase = "executing";
  progress.drones = "A,B";

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.rttMs = 105;
  adaptive.backlogPressure = 42;
  adaptive.primaryPressure = "backlog";

  const auto summary = SelectedDroneSummaryState::fromStates("A", telemetry, readiness,
                                                            mission, plan, part, progress,
                                                            video, adaptive, makeSafeState());
  BOOST_CHECK(summary.hasTelemetry);
  BOOST_CHECK_EQUAL(summary.selectedDrone, "A");
  BOOST_CHECK_EQUAL(summary.readiness, "ready");
  BOOST_CHECK_EQUAL(summary.missionPhase, "uploaded");
  BOOST_CHECK_EQUAL(summary.missionProgressPhase, "executing");
  BOOST_CHECK_EQUAL(summary.missionPlanTask, "patrol-test");
  BOOST_CHECK_EQUAL(summary.missionPartId, "part-A");
  BOOST_CHECK_EQUAL(summary.missionPartWaypoints, 2);
  BOOST_CHECK_EQUAL(summary.videoStatus, "streaming");
  BOOST_CHECK_EQUAL(summary.linkState, "connected");
  BOOST_CHECK(!summary.safetyAttention);
  BOOST_CHECK(!summary.canArm);
  BOOST_CHECK(summary.canTakeoff);
  BOOST_CHECK(summary.canManualControl);
  BOOST_CHECK_NE(summary.statusLine().find("mission_part=part-A"), std::string::npos);
  BOOST_CHECK_NE(summary.statusLine().find("video_adaptive=rtt=105ms"), std::string::npos);

  const auto empty = SelectedDroneSummaryState::fromStates("B", std::nullopt, std::nullopt,
                                                          std::nullopt, plan, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt);
  BOOST_CHECK(!empty.hasTelemetry);
  BOOST_CHECK_EQUAL(empty.readiness, "unknown");
  BOOST_CHECK_EQUAL(empty.missionPhase, "idle");
  BOOST_CHECK_EQUAL(empty.missionPlanTask, "patrol-test");
  BOOST_CHECK_EQUAL(empty.missionPartId, "none");
  BOOST_CHECK(!empty.canArm);
  BOOST_CHECK_EQUAL(empty.armReason, "no-telemetry");
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
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReady = "true";
  telemetry.cameraAvailable = "true";

  auto readiness = makeReadyState(true);
  auto mission = makeMissionState("executing");

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";

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
  BOOST_CHECK_NE(row.rowText.find("fc=true/true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("cam=true"), std::string::npos);
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
