#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"

#include <cstdio>

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::FlightActionControlState;
using ndnsf::examples::uav::FlightCommandState;
using ndnsf::examples::uav::DroneListRowState;
using ndnsf::examples::uav::Fields;
using ndnsf::examples::uav::MissionControlState;
using ndnsf::examples::uav::MissionPlanDocument;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionPart;
using ndnsf::examples::uav::MissionPlan;
using ndnsf::examples::uav::MissionProgressState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::MissionWaypoint;
using ndnsf::examples::uav::MavlinkMessageSummary;
using ndnsf::examples::uav::PreflightCheckItem;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::RecordingDataProductState;
using ndnsf::examples::uav::OperatorAuthorityLease;
using ndnsf::examples::uav::OperatorAuthorityLeaseRequest;
using ndnsf::examples::uav::SafetyState;
using ndnsf::examples::uav::SelectedActionState;
using ndnsf::examples::uav::SelectedDroneSummaryState;
using ndnsf::examples::uav::TelemetryState;
using ndnsf::examples::uav::UavDataProductCatalogState;
using ndnsf::examples::uav::UavFunctionalityState;
using ndnsf::examples::uav::UavAnalyzeSnapshot;
using ndnsf::examples::uav::UavOperatorDashboardSnapshot;
using ndnsf::examples::uav::UavPracticalityState;
using ndnsf::examples::uav::UavStabilityState;
using ndnsf::examples::uav::VehicleParameterEditRequest;
using ndnsf::examples::uav::VehicleParameterEditResult;
using ndnsf::examples::uav::VehicleParameterSnapshot;
using ndnsf::examples::uav::VideoAdaptiveState;
using ndnsf::examples::uav::VideoAdaptivePolicyInput;
using ndnsf::examples::uav::VideoControlState;
using ndnsf::examples::uav::VideoPacket;
using ndnsf::examples::uav::VideoState;
using ndnsf::examples::uav::buildPatrolMissionPlan;
using ndnsf::examples::uav::computeVideoAdaptivePolicy;
using ndnsf::examples::uav::decodeVideoPacket;
using ndnsf::examples::uav::encodeVideoPacket;
using ndnsf::examples::uav::loadMissionPlanDocument;
using ndnsf::examples::uav::saveMissionPlanDocument;
using ndnsf::examples::uav::streamChunkToVideoPacket;
using ndnsf::examples::uav::toServiceOperationStatus;
using ndnsf::examples::uav::videoPacketToStreamChunk;

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
  BOOST_CHECK_EQUAL(plan.completionObjective, "return-to-start");
  BOOST_CHECK_EQUAL(plan.parts.size(), 2);
  BOOST_CHECK(plan.returnHomePlanned);
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B");
  BOOST_CHECK_NE(plan.statusLine().find("parts=2"), std::string::npos);
  BOOST_CHECK_NE(plan.statusLine().find("completion_objective=return-to-start"), std::string::npos);

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
  BOOST_CHECK_EQUAL(plan.completionObjective, "return-to-start");
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B,C");
  for (size_t i = 0; i < plan.parts.size(); ++i) {
    const auto& part = plan.parts[i];
    BOOST_CHECK_EQUAL(part.id, "part" + std::to_string(i));
    BOOST_CHECK_EQUAL(part.assignedDrone, drones[i]);
    BOOST_CHECK_EQUAL(part.waypoints.size(), 5);
    BOOST_CHECK_EQUAL(part.waypoints.back().str(), part.waypoints.front().str());
  }
}

BOOST_AUTO_TEST_CASE(MissionPlanDeterministicClusteringPrototype)
{
  const std::vector<std::string> drones{"A", "B", "C"};
  const std::vector<MissionWaypoint> route{
    {35.119100, -89.936100},
    {35.119200, -89.936300},
    {35.118900, -89.937900},
    {35.119900, -89.937100},
    {35.120100, -89.936500},
  };
  const std::map<std::string, MissionWaypoint> departures{
    {"A", {35.118000, -89.938000}},
    {"B", {35.119000, -89.935000}},
    {"C", {35.120000, -89.938000}},
  };

  const auto plan1 = buildPatrolMissionPlan("patrol-deterministic", 35.1186, -89.9375,
                                           140.0, drones, route, departures);
  const auto plan2 = buildPatrolMissionPlan("patrol-deterministic", 35.1186, -89.9375,
                                           140.0, drones, route, departures);

  BOOST_CHECK_EQUAL(plan1.parts.size(), plan2.parts.size());
  BOOST_CHECK_EQUAL(plan1.droneList(), plan2.droneList());
  BOOST_CHECK_EQUAL(plan1.assignment, plan2.assignment);
  BOOST_CHECK_EQUAL(plan1.returnHomePlanned, plan2.returnHomePlanned);
  for (size_t i = 0; i < plan1.parts.size(); ++i) {
    const auto& p1 = plan1.parts[i];
    const auto& p2 = plan2.parts[i];
    BOOST_CHECK_EQUAL(p1.id, p2.id);
    BOOST_CHECK_EQUAL(p1.role, p2.role);
    BOOST_CHECK_EQUAL(p1.assignedDrone, p2.assignedDrone);
    BOOST_CHECK_EQUAL(p1.returnHomePlanned, p2.returnHomePlanned);
    BOOST_CHECK_EQUAL(p1.waypoints.size(), p2.waypoints.size());
    for (size_t j = 0; j < p1.waypoints.size(); ++j) {
      BOOST_CHECK_CLOSE(p1.waypoints[j].lat, p2.waypoints[j].lat, 0.000001);
      BOOST_CHECK_CLOSE(p1.waypoints[j].lon, p2.waypoints[j].lon, 0.000001);
    }
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
  state.pendingBytes = 4096;
  state.receivedChunks = 100;
  state.fecRecoveredChunks = 4;
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
  BOOST_CHECK_EQUAL(decoded.pendingBytes, 4096);
  BOOST_CHECK_EQUAL(decoded.fecRecoveredChunks, 4);
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
  BOOST_CHECK_NE(decoded.statusLine().find("pending_bytes=4096"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("fec_recovered_chunks=4"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("published_frames=90"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frame_gap=45"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("frame_gap_pressure=35"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frames=45"), std::string::npos);

  const auto health = decoded.toStreamHealth(
    7, ndn::Name("/uav/A/video"), 3000, 123999);
  BOOST_CHECK_EQUAL(health.streamId, "A-video");
  BOOST_CHECK_EQUAL(health.sessionEpoch, 7);
  BOOST_CHECK_EQUAL(health.nextSeq, decoded.receivedChunks + decoded.pendingChunks);
  BOOST_CHECK_EQUAL(ndn_service_framework::toString(health.state), "DEGRADED");
  BOOST_CHECK_EQUAL(health.metrics.timeouts, 2);
  BOOST_CHECK_EQUAL(health.metrics.nacks, 1);
  BOOST_CHECK_EQUAL(health.fetchDecision.window, 64);
  BOOST_CHECK_EQUAL(health.metadata.at("primary_pressure"), "timeout");
  const auto healthSummary = decoded.streamHealthSummary(
    7, ndn::Name("/uav/A/video"), 3000, 123999);
  BOOST_CHECK_NE(healthSummary.find("stream_health=DEGRADED"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("reason=loss-or-gap"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("window=64"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("gaps=45"), std::string::npos);
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

BOOST_AUTO_TEST_CASE(UavStatesMapToCoreServiceOperationStatus)
{
  FlightCommandState command;
  command.droneId = "A";
  command.command = "takeoff";
  command.accepted = "true";
  command.ackResult = "accepted";
  command.updatedMs = 1000;

  auto status = toServiceOperationStatus(
    command, ndn::Name("/UAV/FlightCommand"), ndn::Name("/uav/drone/A"),
    ndn::Name("/request/flight/1"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_FLIGHT_COMMAND");
  BOOST_CHECK_EQUAL(status.operationId, "A:takeoff");
  BOOST_CHECK_EQUAL(status.state, "DONE");
  BOOST_CHECK_EQUAL(status.reasonCode, "OK");
  BOOST_CHECK_CLOSE(status.progress, 1.0, 0.001);
  BOOST_CHECK(status.serviceName == ndn::Name("/UAV/FlightCommand"));
  BOOST_CHECK(status.providerName == ndn::Name("/uav/drone/A"));

  const auto commandPayload =
    ndn_service_framework::ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto parsed =
    ndn_service_framework::ServiceProvider::parseServiceOperationStatusPayload(commandPayload);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->operation, "UAV_FLIGHT_COMMAND");
  BOOST_CHECK_EQUAL(parsed->state, "DONE");

  MissionState mission = makeMissionState("executing");
  mission.updatedMs = 2000;
  status = toServiceOperationStatus(
    mission, ndn::Name("/UAV/MissionAssign"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_MISSION_PART");
  BOOST_CHECK_EQUAL(status.state, "RUNNING");
  BOOST_CHECK_EQUAL(status.reasonCode, "executing");
  BOOST_CHECK_CLOSE(status.progress, 0.75, 0.001);

  mission = makeMissionState("uploaded");
  status = toServiceOperationStatus(mission);
  BOOST_CHECK_EQUAL(status.state, "WAITING_INPUT");
  BOOST_CHECK_CLOSE(status.progress, 0.35, 0.001);

  MissionProgressState progress;
  progress.taskId = "mission-test";
  progress.phase = "executing";
  progress.totalParts = 4;
  progress.completedParts = 3;
  status = toServiceOperationStatus(progress, ndn::Name("/UAV/MissionProgress"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_MISSION");
  BOOST_CHECK_EQUAL(status.state, "RUNNING");
  BOOST_CHECK_CLOSE(status.progress, 0.75, 0.001);

  progress.phase = "waiting-compensation";
  status = toServiceOperationStatus(progress);
  BOOST_CHECK_EQUAL(status.state, "WAITING_INPUT");
}

BOOST_AUTO_TEST_CASE(UavRecordingStatusCarriesCoreDataProductReference)
{
  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.sessionId = "session-7";
  recording.objectPrefix = "/uav/A/recording";
  recording.chunks = 12;
  recording.bytes = 4096;
  recording.updatedMs = 3000;

  const auto reference = recording.toDataProductReference(
    ndn::Name("/UAV/RecordingManifest"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK(reference.name == ndn::Name("/uav/A/recording/session-7"));
  BOOST_CHECK_EQUAL(reference.objectClass, "camera-recording");
  BOOST_CHECK_EQUAL(reference.contentType, "video/h264");
  BOOST_CHECK_EQUAL(reference.segmentCount, 12);

  const auto status = toServiceOperationStatus(
    recording, ndn::Name("/UAV/RecordingManifest"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_RECORDING");
  BOOST_CHECK_EQUAL(status.state, "DONE");
  BOOST_REQUIRE(status.resultReference);
  BOOST_CHECK(status.resultReference->name == ndn::Name("/uav/A/recording/session-7"));

  const auto payload =
    ndn_service_framework::ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto parsed =
    ndn_service_framework::ServiceProvider::parseServiceOperationStatusPayload(payload);
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(parsed->resultReference);
  BOOST_CHECK_EQUAL(parsed->resultReference->objectClass, "camera-recording");
  BOOST_CHECK_EQUAL(parsed->resultReference->segmentCount, 12);
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

BOOST_AUTO_TEST_CASE(UavFunctionalityStateTracksImplementedAndMissingCapabilities)
{
  MissionPlan plan;
  plan.taskId = "patrol-functionality";
  MissionPart part;
  part.id = "part-A";
  part.assignedDrone = "A";
  part.waypoints = {{35.1186, -89.9375}};
  plan.parts.push_back(part);

  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.sessionId = "record-1";
  recording.objectPrefix = "/example/uav/drone/A/repo/camera/recording";
  recording.encryption = "hybrid-aes-256-gcm-at-rest";
  recording.keyId = "/example/uav/drone/A/repo/key";
  recording.contentKey = {0x01, 0x02, 0x03};
  recording.chunks = 3;
  recording.bytes = 1024;

  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.flightControllerBackend = "udp";
  telemetry.flightControllerState = "ready";
  telemetry.systemStatusName = "active";
  telemetry.batteryVoltageV = "12.1";

  const auto functionality = UavFunctionalityState::fromStates(plan, part, recording,
                                                               telemetry, true, 3);
  BOOST_CHECK_EQUAL(functionality.missionEditor, "prototype");
  BOOST_CHECK_EQUAL(functionality.perDroneMissionReview, "available");
  BOOST_CHECK_EQUAL(functionality.persistentMissionFiles, "available");
  BOOST_CHECK_EQUAL(functionality.recordingLogBrowsing, "available");
  BOOST_CHECK_EQUAL(functionality.parameterStatusInspection, "limited");
  BOOST_CHECK_EQUAL(functionality.objectDetectionDisplay, "metadata-only");
  BOOST_CHECK_EQUAL(functionality.multiDroneServiceSelection, "available");
  BOOST_CHECK_EQUAL(functionality.implementedCapabilityCount(), 7);
  BOOST_CHECK_EQUAL(functionality.missingOrLimitedCapabilities().find("persistent-mission-files"),
                    std::string::npos);
  BOOST_CHECK_NE(functionality.statusLine().find("mission_editor=prototype"), std::string::npos);

  const auto roundTrip = UavFunctionalityState::fromFields(functionality.toFields());
  BOOST_CHECK_EQUAL(roundTrip.recordingLogBrowsing, functionality.recordingLogBrowsing);
  BOOST_CHECK_EQUAL(roundTrip.multiDroneServiceSelection, functionality.multiDroneServiceSelection);

  const auto empty = UavFunctionalityState::fromStates(std::nullopt, std::nullopt,
                                                       std::nullopt, std::nullopt, false, 1);
  BOOST_CHECK_EQUAL(empty.implementedCapabilityCount(), 0);
  BOOST_CHECK_NE(empty.missingOrLimitedCapabilities().find("mission-editor=missing"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavPracticalityStateTracksDeploymentUsability)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.cameraAvailable = "true";
  telemetry.cameraSource = "/dev/video0";
  telemetry.cameraReason = "ok";
  telemetry.flightControllerBackend = "udp";
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReason = "ok";

  const auto practicality = UavPracticalityState::fromStates(telemetry, makeReadyState(true),
                                                             true, true, true);
  BOOST_CHECK_EQUAL(practicality.preflightSummary, "available");
  BOOST_CHECK_EQUAL(practicality.hardwareCompatibilityNotes, "documented");
  BOOST_CHECK_EQUAL(practicality.cameraDiagnostics, "available");
  BOOST_CHECK_EQUAL(practicality.flightControllerDiagnostics, "available");
  BOOST_CHECK_EQUAL(practicality.configValidation, "available");
  BOOST_CHECK_EQUAL(practicality.identityCertificateGuidance, "documented");
  BOOST_CHECK_EQUAL(practicality.operatorWorkflowGuidance, "documented");
  BOOST_CHECK_EQUAL(practicality.practicalCapabilityCount(), 7);
  BOOST_CHECK_NE(practicality.missingOrLimitedCapabilities().find("hardware-notes=documented"),
                 std::string::npos);
  BOOST_CHECK_NE(practicality.statusLine().find("camera_diagnostics=available"), std::string::npos);

  const auto roundTrip = UavPracticalityState::fromFields(practicality.toFields());
  BOOST_CHECK_EQUAL(roundTrip.preflightSummary, practicality.preflightSummary);
  BOOST_CHECK_EQUAL(roundTrip.operatorWorkflowGuidance, practicality.operatorWorkflowGuidance);

  TelemetryState unavailableCamera;
  unavailableCamera.cameraAvailable = "false";
  unavailableCamera.cameraReason = "device-not-opened";
  const auto weak = UavPracticalityState::fromStates(unavailableCamera, std::nullopt,
                                                     false, false, false);
  BOOST_CHECK_EQUAL(weak.preflightSummary, "missing");
  BOOST_CHECK_EQUAL(weak.cameraDiagnostics, "limited");
  BOOST_CHECK_EQUAL(weak.flightControllerDiagnostics, "missing");
  BOOST_CHECK_NE(weak.missingOrLimitedCapabilities().find("preflight-summary=missing"),
                 std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavStabilityStateTracksTransportAndControlGuards)
{
  FlightCommandState command;
  command.droneId = "A";
  command.command = "land";
  command.accepted = "false";
  command.ackResult = "timeout";
  command.timeoutMs = 2500;

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.streamId = "live|A|42";
  video.framesPublished = 12;

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.timeoutPressure = 40;
  adaptive.backlogPressure = 20;
  adaptive.primaryPressure = "timeout";

  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "stale";

  SafetyState safety;
  safety.droneId = "A";
  safety.manualNeutralSent = "true";
  safety.manualControlState = "stale-neutral";

  const auto stability = UavStabilityState::fromStates(command, video, adaptive,
                                                       telemetry, safety, true, true);
  BOOST_CHECK_EQUAL(stability.commandTimeoutHandling, "operator-decision");
  BOOST_CHECK_EQUAL(stability.stopVideoIdempotence, "available");
  BOOST_CHECK_EQUAL(stability.streamSessionGuard, "available");
  BOOST_CHECK_EQUAL(stability.frameSequenceGuard, "available");
  BOOST_CHECK_EQUAL(stability.adaptiveVideoPressure, "active");
  BOOST_CHECK_EQUAL(stability.telemetryFreshness, "stale");
  BOOST_CHECK_EQUAL(stability.manualNeutralFallback, "available");
  BOOST_CHECK_EQUAL(stability.longDurationProfiles, "documented");
  BOOST_CHECK_EQUAL(stability.stableCapabilityCount(), 8);
  BOOST_CHECK_NE(stability.missingOrLimitedCapabilities().find("command-timeout=operator-decision"),
                 std::string::npos);
  BOOST_CHECK_NE(stability.statusLine().find("adaptive_video=active"), std::string::npos);

  const auto roundTrip = UavStabilityState::fromFields(stability.toFields());
  BOOST_CHECK_EQUAL(roundTrip.streamSessionGuard, stability.streamSessionGuard);
  BOOST_CHECK_EQUAL(roundTrip.telemetryFreshness, stability.telemetryFreshness);

  const auto empty = UavStabilityState::fromStates(std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt,
                                                   std::nullopt, false, false);
  BOOST_CHECK_EQUAL(empty.stableCapabilityCount(), 0);
  BOOST_CHECK_NE(empty.missingOrLimitedCapabilities().find("stop-video=missing"),
                 std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavMissionPlanDocumentSupportsPersistentOperationalPlan)
{
  auto plan = buildPatrolMissionPlan("patrol-v2", 35.1186, -89.9375, 120.0, {"A", "B"});
  auto document = MissionPlanDocument::fromPlan(plan, "plan-001", "Memphis patrol", "operator-1", 1000);
  document.geofence = {{35.1180, -89.9380}, {35.1190, -89.9380}, {35.1190, -89.9370}};
  document.rallyPoints = {{35.1185, -89.9375}};
  document.metadata["source"] = "unit-test";

  BOOST_CHECK(document.isSaveable());
  BOOST_CHECK(document.hasFenceOrRally());
  BOOST_CHECK_EQUAL(document.plan.parts.size(), 2);

  const auto fields = document.toFields();
  const auto roundTrip = MissionPlanDocument::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.schema, "ndnsf-uav-mission-plan-v2");
  BOOST_CHECK_EQUAL(roundTrip.planId, "plan-001");
  BOOST_CHECK_EQUAL(roundTrip.displayName, "Memphis patrol");
  BOOST_CHECK_EQUAL(roundTrip.operatorId, "operator-1");
  BOOST_CHECK_EQUAL(roundTrip.plan.taskId, "patrol-v2");
  BOOST_CHECK_EQUAL(roundTrip.plan.parts.size(), document.plan.parts.size());
  BOOST_CHECK_EQUAL(roundTrip.geofence.size(), 3);
  BOOST_CHECK_EQUAL(roundTrip.rallyPoints.size(), 1);
  BOOST_CHECK_EQUAL(roundTrip.metadata.at("source"), "unit-test");
  BOOST_CHECK_NE(roundTrip.statusLine().find("saveable=true"), std::string::npos);

  const auto path = std::string("/tmp/ndnsf-uav-mission-plan-document-test.conf");
  saveMissionPlanDocument(document, path);
  const auto loaded = loadMissionPlanDocument(path);
  BOOST_CHECK_EQUAL(loaded.planId, document.planId);
  BOOST_CHECK_EQUAL(loaded.plan.parts.size(), document.plan.parts.size());
  BOOST_CHECK_EQUAL(loaded.geofence.size(), document.geofence.size());
  BOOST_CHECK_EQUAL(loaded.rallyPoints.size(), document.rallyPoints.size());
  BOOST_CHECK_EQUAL(loaded.metadata.at("source"), "unit-test");
  std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(UavDataProductCatalogSummarizesQueryableProducts)
{
  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.productType = "camera-recording";
  recording.sessionId = "record-42";
  recording.objectPrefix = "/example/uav/drone/A/repo/camera/recording/42";
  recording.chunks = 4;
  recording.bytes = 4096;
  recording.updatedMs = 12345;

  auto catalog = UavDataProductCatalogState::fromRecording(recording);
  catalog.telemetryLogProducts = 2;
  catalog.detectionProducts = 1;
  catalog.repoObjects = 4;
  catalog.sourceRepo = "/example/uav/drone/A/local-repo";
  BOOST_CHECK(catalog.hasQueryableProducts());
  BOOST_CHECK_EQUAL(catalog.totalProducts(), 4);
  BOOST_CHECK_EQUAL(catalog.repoObjects, 4);
  BOOST_CHECK_EQUAL(catalog.totalBytes, 4096);
  BOOST_CHECK_EQUAL(catalog.latestObjectPrefix, recording.objectPrefix);

  const auto roundTrip = UavDataProductCatalogState::fromFields(catalog.toFields());
  BOOST_CHECK_EQUAL(roundTrip.totalProducts(), 4);
  BOOST_CHECK_EQUAL(roundTrip.repoObjects, 4);
  BOOST_CHECK_EQUAL(roundTrip.sourceRepo, "/example/uav/drone/A/local-repo");
  BOOST_CHECK_EQUAL(roundTrip.telemetryLogProducts, 2);
  BOOST_CHECK_NE(roundTrip.statusLine().find("detections=1"), std::string::npos);

  std::vector<Fields> repoEntries{
    {{"object_name", "/example/uav/drone/A/repo/camera/recording/record-1/chunk/0"},
     {"object_type", "video/h264-chunk"}, {"size", "1000"}, {"updated_ms", "10"}},
    {{"object_name", "/example/uav/drone/A/repo/camera/recording/record-1/chunk/1"},
     {"object_type", "video/h264-chunk"}, {"size", "1200"}, {"updated_ms", "11"}},
    {{"object_name", "/example/uav/drone/A/repo/telemetry/log-1"},
     {"object_type", "telemetry-log"}, {"size", "300"}},
    {{"object_name", "/example/uav/drone/A/repo/detection/yolo-1"},
     {"object_type", "detection-log"}, {"size", "400"}},
    {{"object_name", "/example/uav/drone/A/repo/mission/mission-1"},
     {"object_type", "mission-log"}, {"size", "500"}},
  };
  const auto repoCatalog = UavDataProductCatalogState::fromCatalogProductFields(
    repoEntries, "/example/uav/drone/A/local-repo", 99);
  BOOST_CHECK_EQUAL(repoCatalog.repoObjects, 5);
  BOOST_CHECK_EQUAL(repoCatalog.recordingProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.telemetryLogProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.detectionProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.missionLogProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.totalBytes, 3400);
  BOOST_CHECK_EQUAL(repoCatalog.sourceRepo, "/example/uav/drone/A/local-repo");
  BOOST_CHECK(repoCatalog.hasQueryableProducts());
}

BOOST_AUTO_TEST_CASE(VehicleParameterSnapshotCarriesCapabilityView)
{
  VehicleParameterSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.source = "mavlink-param-cache";
  snapshot.firmware = "PX4-1.14";
  snapshot.vehicleType = "quadrotor";
  snapshot.flightModes = "MANUAL,POSCTL,AUTO.MISSION";
  snapshot.completePercent = 80;
  snapshot.parameters["NAV_RCL_ACT"] = "2";
  snapshot.parameters["COM_RC_LOSS_T"] = "5";

  BOOST_CHECK(snapshot.isUsable());
  const auto fields = snapshot.toFields();
  const auto roundTrip = VehicleParameterSnapshot::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.parameterCount, 2);
  BOOST_CHECK_EQUAL(roundTrip.parameters.at("NAV_RCL_ACT"), "2");
  BOOST_CHECK_EQUAL(roundTrip.firmware, "PX4-1.14");
  BOOST_CHECK_NE(roundTrip.statusLine().find("usable=true"), std::string::npos);

  const auto compact = VehicleParameterSnapshot::fromFields(snapshot.toFields(false));
  BOOST_CHECK_EQUAL(compact.parameterCount, 2);
  BOOST_CHECK(compact.parameters.empty());
  BOOST_CHECK(compact.isUsable());
}

BOOST_AUTO_TEST_CASE(VehicleParameterEditContractsRoundTripAndValidate)
{
  VehicleParameterEditRequest request;
  request.requestId = "param-req-1";
  request.operatorId = "operator-1";
  request.droneId = "A";
  request.parameterName = "NAV_RCL_ACT";
  request.expectedValue = "2";
  request.requestedValue = "1";
  request.valueType = "MAV_PARAM_TYPE_INT32";
  request.targetSystem = 7;
  request.targetComponent = 1;
  request.requestedMs = 4567;

  std::string reason;
  BOOST_CHECK(request.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  const auto requestRoundTrip = VehicleParameterEditRequest::fromFields(request.toFields());
  BOOST_CHECK_EQUAL(requestRoundTrip.requestId, "param-req-1");
  BOOST_CHECK_EQUAL(requestRoundTrip.parameterName, "NAV_RCL_ACT");
  BOOST_CHECK_EQUAL(requestRoundTrip.requestedValue, "1");
  BOOST_CHECK_EQUAL(requestRoundTrip.targetSystem, 7);
  BOOST_CHECK_NE(requestRoundTrip.statusLine().find("valid=true"), std::string::npos);

  auto invalid = request;
  invalid.parameterName = "THIS_PARAM_NAME_IS_TOO_LONG";
  BOOST_CHECK(!invalid.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "parameter-name-too-long");

  VehicleParameterEditResult result;
  result.requestId = request.requestId;
  result.droneId = request.droneId;
  result.parameterName = request.parameterName;
  result.valueType = request.valueType;
  result.accepted = true;
  result.applied = true;
  result.verified = true;
  result.reason = "ok";
  result.previousValue = "2";
  result.requestedValue = "1";
  result.verifiedValue = "1";
  result.updatedMs = 5000;

  BOOST_CHECK(result.successful());
  const auto resultRoundTrip = VehicleParameterEditResult::fromFields(result.toFields());
  BOOST_CHECK(resultRoundTrip.successful());
  BOOST_CHECK_EQUAL(resultRoundTrip.verifiedValue, "1");
  BOOST_CHECK_NE(resultRoundTrip.statusLine().find("verified=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(PreflightAndAnalyzeContractsSupportQgcStylePanels)
{
  PreflightCheckItem gps;
  gps.checkId = "gps-fix";
  gps.droneId = "A";
  gps.label = "GPS Fix";
  gps.category = "Sensors";
  gps.status = "fail";
  gps.reason = "waiting-for-3d-fix";
  gps.blocking = true;
  gps.order = 10;
  gps.updatedMs = 1000;

  BOOST_CHECK(gps.isBlockingFailure());
  const auto gpsRoundTrip = PreflightCheckItem::fromFields(gps.toFields());
  BOOST_CHECK(gpsRoundTrip.isBlockingFailure());
  BOOST_CHECK_EQUAL(gpsRoundTrip.label, "GPS Fix");
  BOOST_CHECK_NE(gpsRoundTrip.statusLine().find("blocking_failure=true"), std::string::npos);

  MavlinkMessageSummary heartbeat;
  heartbeat.messageName = "HEARTBEAT";
  heartbeat.messageId = 0;
  heartbeat.systemId = 1;
  heartbeat.componentId = 1;
  heartbeat.count = 120;
  heartbeat.rateHz = "1.0";
  heartbeat.lastSeenMs = 9000;

  MavlinkMessageSummary position;
  position.messageName = "GLOBAL_POSITION_INT";
  position.messageId = 33;
  position.systemId = 1;
  position.componentId = 1;
  position.count = 360;
  position.rateHz = "3.0";
  position.lastSeenMs = 3000;

  UavAnalyzeSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.linkState = "connected";
  snapshot.flightMode = "GUIDED";
  snapshot.missionPhase = "executing";
  snapshot.videoState = "streaming";
  snapshot.parameterCacheStatus = "complete";
  snapshot.updatedMs = 10000;
  snapshot.messages = {heartbeat, position};

  const auto roundTrip = UavAnalyzeSnapshot::fromFields(snapshot.toFields());
  BOOST_CHECK_EQUAL(roundTrip.messages.size(), 2);
  BOOST_CHECK_EQUAL(roundTrip.messages[0].messageName, "HEARTBEAT");
  BOOST_CHECK_EQUAL(roundTrip.messages[1].messageId, 33);
  BOOST_CHECK_EQUAL(roundTrip.activeMessageCount(10000, 3000), 1);
  BOOST_CHECK_NE(roundTrip.statusLine().find("messages=2"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(OperatorDashboardSnapshotSummarizesQgcStyleState)
{
  UavOperatorDashboardSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.telemetryFreshness = "fresh";
  snapshot.readiness = "ready";
  snapshot.readinessReason = "ok";
  snapshot.linkState = "connected";
  snapshot.flightMode = "AUTO.MISSION";
  snapshot.missionPhase = "executing";
  snapshot.videoState = "streaming";
  snapshot.parameterCacheStatus = "complete";
  snapshot.parameterCount = 42;
  snapshot.preflightTotal = 6;
  snapshot.preflightBlockingFailures = 0;
  snapshot.mavlinkMessageCount = 4;
  snapshot.activeMavlinkMessageCount = 3;
  snapshot.canArm = true;
  snapshot.canTakeoff = true;
  snapshot.canLand = true;
  snapshot.canManualControl = true;
  snapshot.canEmergencyStop = true;
  snapshot.updatedMs = 12345;

  BOOST_CHECK(snapshot.operatorReady());
  const auto roundTrip = UavOperatorDashboardSnapshot::fromFields(snapshot.toFields());
  BOOST_CHECK(roundTrip.operatorReady());
  BOOST_CHECK_EQUAL(roundTrip.droneId, "A");
  BOOST_CHECK_EQUAL(roundTrip.parameterCount, 42);
  BOOST_CHECK_EQUAL(roundTrip.preflightTotal, 6);
  BOOST_CHECK_EQUAL(roundTrip.activeMavlinkMessageCount, 3);
  BOOST_CHECK(roundTrip.canEmergencyStop);
  BOOST_CHECK_NE(roundTrip.statusLine().find("operator_ready=true"), std::string::npos);

  auto blocked = roundTrip;
  blocked.preflightBlockingFailures = 1;
  BOOST_CHECK(!blocked.operatorReady());
}

BOOST_AUTO_TEST_CASE(OperatorAuthorityLeaseBlocksConflictingControl)
{
  OperatorAuthorityLease lease;
  lease.leaseId = "lease-A";
  lease.operatorId = "operator-1";
  lease.droneId = "A";
  lease.scope = "control";
  lease.issuedMs = 1000;
  lease.expiresMs = 5000;

  std::string reason;
  BOOST_CHECK(lease.allowsCommand("A", "takeoff", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(!lease.allowsCommand("B", "takeoff", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "wrong-drone");
  BOOST_CHECK(!lease.allowsCommand("A", "takeoff", 6000, reason));
  BOOST_CHECK_EQUAL(reason, "lease-expired");

  lease.scope = "monitor";
  BOOST_CHECK(lease.allowsCommand("A", "telemetry", 2000, reason));
  BOOST_CHECK(!lease.allowsCommand("A", "land", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "monitor-scope");

  const auto roundTrip = OperatorAuthorityLease::fromFields(lease.toFields());
  BOOST_CHECK_EQUAL(roundTrip.leaseId, "lease-A");
  BOOST_CHECK_EQUAL(roundTrip.scope, "monitor");
  BOOST_CHECK_NE(roundTrip.statusLine().find("operator=operator-1"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(OperatorAuthorityLeaseRequestRoundTripsAndValidates)
{
  OperatorAuthorityLeaseRequest request;
  request.requestId = "req-1";
  request.operatorId = "operator-1";
  request.droneId = "A";
  request.scope = "mission";
  request.ttlMs = 45000;
  request.requestedMs = 1234;

  std::string reason;
  BOOST_CHECK(request.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  const auto fields = request.toFields();
  BOOST_CHECK_EQUAL(fields.at("type"), "operator-authority-lease-request");
  BOOST_CHECK_EQUAL(fields.at("lease_request_id"), "req-1");
  BOOST_CHECK_EQUAL(fields.at("lease_operator"), "operator-1");
  BOOST_CHECK_EQUAL(fields.at("lease_drone"), "A");
  BOOST_CHECK_EQUAL(fields.at("lease_scope"), "mission");
  BOOST_CHECK_EQUAL(fields.at("lease_ttl_ms"), "45000");
  BOOST_CHECK_EQUAL(fields.at("lease_requested_ms"), "1234");

  const auto roundTrip = OperatorAuthorityLeaseRequest::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.requestId, "req-1");
  BOOST_CHECK_EQUAL(roundTrip.operatorId, "operator-1");
  BOOST_CHECK_EQUAL(roundTrip.droneId, "A");
  BOOST_CHECK_EQUAL(roundTrip.scope, "mission");
  BOOST_CHECK_EQUAL(roundTrip.ttlMs, 45000);
  BOOST_CHECK_EQUAL(roundTrip.requestedMs, 1234);
  BOOST_CHECK_NE(roundTrip.statusLine().find("scope=mission"), std::string::npos);

  auto invalid = request;
  invalid.scope = "fly-anywhere";
  BOOST_CHECK(!invalid.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "unsupported-scope");
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

BOOST_AUTO_TEST_CASE(DroneListAvailabilitySummaryShowsSubsystems)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "fresh";
  telemetry.readiness = "ready";
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReady = "true";
  telemetry.flightControllerBackend = "udp";
  telemetry.cameraAvailable = "true";
  telemetry.cameraSource = "/dev/video0";
  telemetry.cameraReason = "ok";
  telemetry.video = "streaming";
  telemetry.capture = "active";
  telemetry.recording = "recording";

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.capture = "active";
  video.recording = "recording";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";
  video.recordingChunks = 5;
  video.recordingBytes = 1234;

  const auto row = DroneListRowState::fromStates(
    "A", true, telemetry, makeReadyState(true), makeMissionState("executing"),
    video, std::nullopt, std::nullopt, makeSafeState(), std::nullopt,
    "available", "available", "available", "recording", "stored");

  BOOST_CHECK(row.hasTelemetry);
  BOOST_CHECK(row.hasReadiness);
  BOOST_CHECK(row.hasMission);
  BOOST_CHECK(row.hasVideo);
  BOOST_CHECK_EQUAL(row.serviceCamera, "available");
  BOOST_CHECK_EQUAL(row.serviceMavlink, "available");
  BOOST_CHECK_EQUAL(row.serviceMission, "available");
  BOOST_CHECK_EQUAL(row.serviceRecording, "recording");
  BOOST_CHECK_EQUAL(row.serviceRepo, "stored");
  BOOST_CHECK_NE(row.rowText.find("fc=true/true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("cam=true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("mission=executing"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("video=streaming"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("recording=recording"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("repo=stored"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(TelemetryFreshnessAndManualNeutralRegression)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "fresh";
  BOOST_CHECK(telemetry.telemetryIsFresh());
  BOOST_CHECK(!telemetry.telemetryIsStale());
  BOOST_CHECK(!telemetry.telemetryIsMissing());

  telemetry.telemetryFreshness = "stale";
  BOOST_CHECK(telemetry.telemetryIsStale());
  BOOST_CHECK_NE(telemetry.statusLine().find("freshness=stale"), std::string::npos);

  telemetry.telemetryFreshness = "missing";
  BOOST_CHECK(telemetry.telemetryIsMissing());

  SafetyState safety;
  safety.droneId = "A";
  safety.linkState = "connected";
  safety.manualControlState = "fresh";
  safety.manualReplayActive = "true";
  safety.manualNeutralSent = "false";
  safety.manualFreshForMs = 120;
  BOOST_CHECK(safety.manualControlFresh());
  BOOST_CHECK(!safety.needsOperatorAttention());

  safety.manualControlState = "stale-waiting-neutral";
  safety.manualReplayActive = "false";
  safety.manualNeutralSent = "true";
  safety.manualFreshForMs = 0;
  BOOST_CHECK(!safety.manualControlFresh());
  BOOST_CHECK(safety.needsOperatorAttention());
  BOOST_CHECK_NE(safety.statusLine().find("neutral_sent=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(FlightCommandLifecycleTimeoutAndRttAreVisible)
{
  Fields successFields{
    {"drone_id", "A"},
    {"command", "takeoff"},
    {"accepted", "true"},
    {"ack_result", "accepted"},
    {"fc_state", "ready"},
    {"rtt_ms", "87"},
    {"timeout_ms", "0"},
    {"detail", "success"},
  };
  const auto success = FlightCommandState::fromFields(successFields);
  BOOST_CHECK(success.isAccepted());
  BOOST_CHECK(!success.isTimeout());
  BOOST_CHECK(success.isSafetyCritical());
  BOOST_CHECK_EQUAL(success.rttMs, 87);
  BOOST_CHECK_NE(success.statusLine().find("command=takeoff"), std::string::npos);
  BOOST_CHECK_NE(success.statusLine().find("rtt_ms=87"), std::string::npos);

  Fields timeoutFields{
    {"drone_id", "A"},
    {"command", "land"},
    {"accepted", "false"},
    {"ack_result", "timeout"},
    {"timeout_ms", "2500"},
    {"detail", "operator-retry-required"},
  };
  const auto timeout = FlightCommandState::fromFields(timeoutFields);
  BOOST_CHECK(!timeout.isAccepted());
  BOOST_CHECK(timeout.isTimeout());
  BOOST_CHECK(timeout.isSafetyCritical());
  BOOST_CHECK_EQUAL(timeout.timeoutMs, 2500);
  BOOST_CHECK_NE(timeout.statusLine().find("ack=timeout"), std::string::npos);
  BOOST_CHECK_NE(timeout.statusLine().find("detail=operator-retry-required"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissionPartialRecoveryAndCancelStatesAreExplicit)
{
  MissionProgressState partial;
  partial.taskId = "patrol-test";
  partial.phase = "waiting-compensation";
  partial.drones = "A,B";
  partial.totalParts = 2;
  partial.completedParts = 1;
  partial.missingParts = 1;
  partial.completedPartIds = "part0";
  partial.missingPartIds = "part1";
  partial.pendingPartIds = "part1";

  BOOST_CHECK(partial.isActive());
  BOOST_CHECK(partial.needsCompensation());
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part0", "executing"), "DONE");
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part1", "executing"), "RUNNING");
  BOOST_CHECK_NE(partial.statusLine().find("missing_parts=1"), std::string::npos);

  MissionState cancelled = makeMissionState("cancelled");
  BOOST_CHECK(cancelled.isCancelled());
  BOOST_CHECK(cancelled.isTerminal());
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part1", cancelled.phase), "FAILED");
}

BOOST_AUTO_TEST_CASE(VideoPacketSessionMetadataRoundTrips)
{
  VideoPacket packet;
  packet.streamId = "live|A|1";
  packet.streamSessionEpoch = 42;
  packet.packetSeq = 7;
  packet.frameSeq = 3;
  packet.frameFirstPacketSeq = 6;
  packet.frameLastPacketSeq = 8;
  packet.frameSegmentIndex = 1;
  packet.frameSegmentCount = 3;
  packet.encoding = "h264";
  packet.keyFrame = false;
  packet.payload = {0x01, 0x02, 0x03};

  const auto decoded = decodeVideoPacket(encodeVideoPacket(packet));
  BOOST_CHECK_EQUAL(decoded.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(decoded.streamSessionEpoch, 42);
  BOOST_CHECK_EQUAL(decoded.packetSeq, 7);
  BOOST_CHECK_EQUAL(decoded.frameSeq, 3);
  BOOST_CHECK_EQUAL(decoded.frameFirstPacketSeq, 6);
  BOOST_CHECK_EQUAL(decoded.frameLastPacketSeq, 8);
  BOOST_CHECK_EQUAL(decoded.payload.size(), 3);

  VideoPacket oldSession = decoded;
  oldSession.streamSessionEpoch = 41;
  BOOST_CHECK_NE(oldSession.streamSessionEpoch, decoded.streamSessionEpoch);
}

BOOST_AUTO_TEST_CASE(VideoPacketMapsToCoreStreamChunkWithoutChangingWire)
{
  VideoPacket packet;
  packet.streamId = "live|A|2";
  packet.streamSessionEpoch = 77;
  packet.second = 1234;
  packet.packetSeq = 9;
  packet.frameSeq = 4;
  packet.captureMs = 5555;
  packet.frameFirstPacketSeq = 8;
  packet.frameLastPacketSeq = 11;
  packet.bucketPacketCount = 12;
  packet.frameSegmentIndex = 1;
  packet.frameSegmentCount = 4;
  packet.encoding = "video/h264";
  packet.keyFrame = true;
  packet.fecDataShards = 3;
  packet.fecParityShards = 1;
  packet.fecSymbolIndex = 3;
  packet.fecSymbolCount = 4;
  packet.fecDataLengths = "100,101,102";
  packet.payload = {0x01, 0x02, 0x03, 0x04};

  const auto streamChunk = videoPacketToStreamChunk(packet);
  BOOST_CHECK_EQUAL(streamChunk.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(streamChunk.sessionEpoch, packet.streamSessionEpoch);
  BOOST_CHECK_EQUAL(streamChunk.seq, packet.packetSeq);
  BOOST_CHECK_EQUAL(streamChunk.contentType, packet.encoding);
  BOOST_CHECK_EQUAL(streamChunk.frameId, packet.frameSeq);
  BOOST_CHECK_EQUAL(streamChunk.metadata.at("uav.second"), "1234");
  BOOST_CHECK_EQUAL(streamChunk.metadata.at("uav.bucket_packet_count"), "12");
  BOOST_REQUIRE(streamChunk.fec);
  BOOST_CHECK_EQUAL(streamChunk.fec->scheme, "xor-parity");
  BOOST_CHECK(streamChunk.fec->repairSymbol);
  BOOST_CHECK_EQUAL(streamChunk.fec->dataLengths.size(), 3);

  const auto restored = streamChunkToVideoPacket(streamChunk);
  BOOST_CHECK_EQUAL(restored.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(restored.streamSessionEpoch, packet.streamSessionEpoch);
  BOOST_CHECK_EQUAL(restored.second, packet.second);
  BOOST_CHECK_EQUAL(restored.packetSeq, packet.packetSeq);
  BOOST_CHECK_EQUAL(restored.frameSeq, packet.frameSeq);
  BOOST_CHECK_EQUAL(restored.captureMs, packet.captureMs);
  BOOST_CHECK_EQUAL(restored.frameFirstPacketSeq, packet.frameFirstPacketSeq);
  BOOST_CHECK_EQUAL(restored.frameLastPacketSeq, packet.frameLastPacketSeq);
  BOOST_CHECK_EQUAL(restored.bucketPacketCount, packet.bucketPacketCount);
  BOOST_CHECK_EQUAL(restored.frameSegmentIndex, packet.frameSegmentIndex);
  BOOST_CHECK_EQUAL(restored.frameSegmentCount, packet.frameSegmentCount);
  BOOST_CHECK_EQUAL(restored.encoding, packet.encoding);
  BOOST_CHECK_EQUAL(restored.keyFrame, packet.keyFrame);
  BOOST_CHECK_EQUAL(restored.fecDataShards, packet.fecDataShards);
  BOOST_CHECK_EQUAL(restored.fecParityShards, packet.fecParityShards);
  BOOST_CHECK_EQUAL(restored.fecSymbolIndex, packet.fecSymbolIndex);
  BOOST_CHECK_EQUAL(restored.fecSymbolCount, packet.fecSymbolCount);
  BOOST_CHECK_EQUAL(restored.fecDataLengths, packet.fecDataLengths);
  BOOST_CHECK(restored.payload == packet.payload);

  BOOST_CHECK(encodeVideoPacket(restored) == encodeVideoPacket(packet));
}

BOOST_AUTO_TEST_CASE(StreamChunkHandoffPreservesFecRecoveryInputs)
{
  const std::vector<uint8_t> shard0{0x10, 0x20, 0x30, 0x40};
  const std::vector<uint8_t> shard1{0x01, 0x02, 0x03};
  std::vector<uint8_t> parity(shard0.size(), 0);
  for (size_t i = 0; i < shard0.size(); ++i) {
    parity[i] ^= shard0[i];
  }
  for (size_t i = 0; i < shard1.size(); ++i) {
    parity[i] ^= shard1[i];
  }

  auto makePacket = [] (uint64_t packetSeq,
                        uint32_t symbolIndex,
                        std::vector<uint8_t> payload) {
    VideoPacket packet;
    packet.streamId = "live|A|fec";
    packet.streamSessionEpoch = 88;
    packet.second = 123;
    packet.packetSeq = packetSeq;
    packet.frameSeq = 5;
    packet.captureMs = 6789;
    packet.frameFirstPacketSeq = 20;
    packet.frameLastPacketSeq = 22;
    packet.bucketPacketCount = 23;
    packet.frameSegmentIndex = symbolIndex;
    packet.frameSegmentCount = 3;
    packet.encoding = "video/h264";
    packet.fecDataShards = 2;
    packet.fecParityShards = 1;
    packet.fecSymbolIndex = symbolIndex;
    packet.fecSymbolCount = 3;
    packet.fecDataLengths = "4,3";
    packet.payload = std::move(payload);
    return packet;
  };

  const auto receivedData0 = videoPacketToStreamChunk(makePacket(20, 0, shard0));
  const auto receivedParity = videoPacketToStreamChunk(makePacket(22, 2, parity));

  BOOST_REQUIRE(receivedData0.fec);
  BOOST_REQUIRE(receivedParity.fec);
  BOOST_CHECK_EQUAL(receivedData0.fec->dataLengths.size(), 2);
  BOOST_CHECK_EQUAL(receivedParity.fec->dataLengths[1], shard1.size());
  BOOST_CHECK(receivedParity.fec->repairSymbol);

  const auto missingIdx = 1U;
  std::vector<uint8_t> recovered(receivedParity.fec->dataLengths[missingIdx], 0);
  for (size_t i = 0; i < recovered.size(); ++i) {
    recovered[i] ^= (i < receivedParity.payload.size() ? receivedParity.payload[i] : 0);
    recovered[i] ^= (i < receivedData0.payload.size() ? receivedData0.payload[i] : 0);
  }

  ndn_service_framework::StreamChunk recoveredChunk;
  recoveredChunk.streamId = receivedData0.streamId;
  recoveredChunk.sessionEpoch = receivedData0.sessionEpoch;
  recoveredChunk.seq = 21;
  recoveredChunk.payload = recovered;
  recoveredChunk.contentType = receivedData0.contentType;
  recoveredChunk.frameId = receivedData0.frameId;
  recoveredChunk.frameFirstSeq = receivedData0.frameFirstSeq;
  recoveredChunk.frameLastSeq = receivedData0.frameLastSeq;
  recoveredChunk.segmentIndex = missingIdx;
  recoveredChunk.segmentCount = receivedData0.segmentCount;

  BOOST_CHECK(recoveredChunk.payload == shard1);
  BOOST_CHECK_EQUAL(recoveredChunk.streamId, "live|A|fec");
  BOOST_CHECK_EQUAL(recoveredChunk.sessionEpoch, 88);
  BOOST_CHECK_EQUAL(recoveredChunk.seq, 21);
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
