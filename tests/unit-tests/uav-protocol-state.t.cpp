#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionProgressState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::SafetyState;
using ndnsf::examples::uav::VideoAdaptiveState;

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

BOOST_AUTO_TEST_CASE(VideoAdaptiveStateRoundTripsAndReportsPressure)
{
  VideoAdaptiveState state;
  state.droneId = "A";
  state.state = "streaming";
  state.rttMs = 142;
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
  BOOST_CHECK_EQUAL(decoded.window, 64);
  BOOST_CHECK_EQUAL(decoded.missingTimeoutMs, 240);
  BOOST_CHECK_EQUAL(decoded.timeoutPressure, 55);
  BOOST_CHECK(decoded.underPressure());
  BOOST_CHECK_NE(decoded.statusLine().find("VideoAdaptive drone=A"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("window=64"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frames=45"), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
