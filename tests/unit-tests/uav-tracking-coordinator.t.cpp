#include "../../NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"

#include "../../NDNSF-UAV-APP/shared/UavNames.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavTrackingCoordinatorTests)

BOOST_AUTO_TEST_CASE(BuildsThreeSourceOneComputePlan)
{
  const std::array<uint8_t, 3> assignmentBytes{{1, 2, 3}};
  const auto plan = UavTrackingCoordinator::makePlan(
    {droneIdentity("UAV1"), droneIdentity("UAV2"), droneIdentity("UAV3")},
    ndn::Name("/example/uav/compute"),
    ndn::Buffer(assignmentBytes.data(), assignmentBytes.size()));
  BOOST_REQUIRE_EQUAL(plan.roles.size(), 4);
  BOOST_CHECK_EQUAL(plan.roles[0].role, "Camera-UAV1");
  BOOST_CHECK_EQUAL(plan.roles[3].role, "Tracking-Compute");
  BOOST_CHECK(plan.roles[3].terminalResponseOwner);
  BOOST_REQUIRE_EQUAL(plan.keyScopes.size(), 3);
  BOOST_CHECK_EQUAL(plan.keyScopes[0].name, "uav-tracking-input");
  BOOST_CHECK_EQUAL(plan.keyScopes[1].name, "uav-tracking-observation");
  BOOST_CHECK_EQUAL(plan.keyScopes[2].name, "uav-tracking-result");
  BOOST_CHECK_EQUAL(plan.keyScopes[1].roles.size(), 4);
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 2);
  BOOST_CHECK_EQUAL(plan.dependencies[1].keyScope, "uav-tracking-observation");
  BOOST_CHECK_EQUAL(plan.dependencies[1].producers.size(), 3);
  BOOST_CHECK_EQUAL(plan.dependencies[1].consumers.size(), 1);
  BOOST_REQUIRE(plan.participantSelector);
}

BOOST_AUTO_TEST_CASE(RejectsMissingParticipant)
{
  ndn_service_framework::SelectedParticipant participant;
  participant.role = "Camera-UAV1";
  participant.provider = droneIdentity("UAV1");
  std::string reason;
  BOOST_CHECK(!UavTrackingCoordinator::hasRequiredParticipants({participant}, &reason));
  BOOST_CHECK(!reason.empty());
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateOrComputeAsSource)
{
  BOOST_CHECK_THROW(
    UavTrackingCoordinator::makePlan(
      {droneIdentity("UAV1"), droneIdentity("UAV1"), droneIdentity("UAV3")},
      ndn::Name("/example/uav/compute"), ndn::Buffer{1, 2, 3}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    UavTrackingCoordinator::makePlan(
      {droneIdentity("UAV1"), droneIdentity("UAV2"), ndn::Name("/example/uav/compute")},
      ndn::Name("/example/uav/compute"), ndn::Buffer{1, 2, 3}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AddsOnlyTheExplicitCalibrationProviderRole)
{
  const std::vector<ndn::Name> sources{
    droneIdentity("UAV1"), droneIdentity("UAV2"), droneIdentity("UAV3")};
  const ndn::Name compute("/example/uav/compute");
  const ndn::Name calibration("/example/uav/calibration-provider");
  const ndn::Buffer assignment{1, 2, 3};

  const auto ordinary = UavTrackingCoordinator::makePlan(sources, compute, assignment);
  BOOST_REQUIRE_EQUAL(ordinary.roles.size(), 4);
  BOOST_REQUIRE_EQUAL(ordinary.dependencies.size(), 2);

  const auto delegated = UavTrackingCoordinator::makePlan(
    sources, compute, assignment, 1000, 30000, calibration);
  BOOST_REQUIRE_EQUAL(delegated.roles.size(), 5);
  BOOST_CHECK_EQUAL(delegated.roles[4].role, "Calibration-Provider");
  BOOST_CHECK(!delegated.roles[4].terminalResponseOwner);
  BOOST_REQUIRE_EQUAL(delegated.keyScopes[1].roles.size(), 5);
  BOOST_CHECK_EQUAL(delegated.keyScopes[1].roles[4], "Calibration-Provider");
  BOOST_REQUIRE_EQUAL(delegated.dependencies.size(), 3);
  BOOST_CHECK_EQUAL(delegated.dependencies[2].producers[0], "Calibration-Provider");

  std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
  for (const auto& item : std::array<std::pair<const char*, const char*>, 5>{{
         {"Camera-UAV1", "/example/uav/drone/UAV1"},
         {"Camera-UAV2", "/example/uav/drone/UAV2"},
         {"Camera-UAV3", "/example/uav/drone/UAV3"},
         {"Tracking-Compute", "/example/uav/compute"},
         {"Calibration-Provider", "/example/uav/calibration-provider"}}}) {
    ndn_service_framework::AckSelectionCandidate candidate;
    candidate.providerName = ndn::Name(item.second);
    candidate.ack.setStatus(true);
    candidates.push_back(std::move(candidate));
  }
  std::string reason;
  auto selected = delegated.participantSelector->select(candidates, delegated.roles);
  BOOST_CHECK(UavTrackingCoordinator::hasRequiredParticipants(selected, &reason));
  candidates.back().providerName = compute;
  selected = delegated.participantSelector->select(candidates, delegated.roles);
  BOOST_CHECK(!UavTrackingCoordinator::hasRequiredParticipants(selected, &reason, true));
}

BOOST_AUTO_TEST_SUITE_END()
