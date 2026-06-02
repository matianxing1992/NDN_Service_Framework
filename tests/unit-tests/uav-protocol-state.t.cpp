#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::SafetyState;

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

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
