#include "../../NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"

#include "../../NDNSF-UAV-APP/shared/UavNames.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>

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
  BOOST_REQUIRE_EQUAL(plan.dependencies.size(), 1);
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

BOOST_AUTO_TEST_SUITE_END()
