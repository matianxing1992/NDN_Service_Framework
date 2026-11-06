#include "../../NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"

#include "../../NDNSF-UAV-APP/shared/UavNames.hpp"
#include "../../NDNSF-UAV-APP/shared/UavProtocol.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavTrackingCoordinatorTests)

BOOST_AUTO_TEST_CASE(ModeReadinessAndServiceMustMatchBeforeSelection)
{
  const std::vector<ndn::Name> sources{droneIdentity("UAV1"), droneIdentity("UAV2"),
                                        droneIdentity("UAV3")};
  const ndn::Name compute("/example/uav/compute");
  const auto plan = UavTrackingCoordinator::makePlan(sources, compute, {}, 1000, 3000, {},
    {{"Tracking-Compute", {{"tracking_mode", "person"}}}});
  std::vector<ndn_service_framework::AckCandidate> candidates;
  for (size_t i = 0; i < 4; ++i) {
    ndn_service_framework::AckCandidate candidate;
    candidate.providerName = i < 3 ? sources[i] : compute;
    candidate.serviceName = SERVICE_TRACKING_WINDOW;
    candidate.ack.setStatus(true);
    auto fields = encodeFields({{"tracking_mode", "person"}});
    ndn::Buffer payload(fields.begin(), fields.end());
    candidate.ack.setPayload(payload, payload.size());
    candidates.push_back(std::move(candidate));
  }
  auto selected = plan.participantSelector->select(candidates, plan.roles);
  BOOST_CHECK(UavTrackingCoordinator::hasRequiredParticipants(selected));
  candidates[0].serviceName = ndn::Name("/Other/Service");
  selected = plan.participantSelector->select(candidates, plan.roles);
  BOOST_CHECK(!UavTrackingCoordinator::hasRequiredParticipants(selected));
  candidates[0].serviceName = SERVICE_TRACKING_WINDOW;
  auto fields = encodeFields({{"tracking_mode", "vehicle"}});
  ndn::Buffer payload(fields.begin(), fields.end());
  candidates[3].ack.setPayload(payload, payload.size());
  selected = plan.participantSelector->select(candidates, plan.roles);
  BOOST_CHECK(!UavTrackingCoordinator::hasRequiredParticipants(selected));
}

BOOST_AUTO_TEST_CASE(CalibrationMustAdvertiseTheDrainProtocolAndMatchingEpoch)
{
  const std::vector<ndn::Name> sources{
    droneIdentity("UAV1"), droneIdentity("UAV2"), droneIdentity("UAV3")};
  const ndn::Name compute("/example/uav/compute");
  const ndn::Name calibration("/example/uav/calibration");
  const Fields expected{{"tracking_mode", "vehicle"}, {"source_epoch", "17"},
    {"tracking_work_drained_schema", TRACKING_WORK_DRAINED_SCHEMA}};
  const auto plan = UavTrackingCoordinator::makePlan(sources, compute, {}, 1000, 3000,
    calibration, {{"Calibration-Provider", expected}});
  std::vector<ndn_service_framework::AckCandidate> candidates;
  for (size_t i = 0; i < 5; ++i) {
    ndn_service_framework::AckCandidate candidate;
    candidate.providerName = i < 3 ? sources[i] : i == 3 ? compute : calibration;
    candidate.serviceName = SERVICE_TRACKING_WINDOW;
    candidate.ack.setStatus(true);
    candidates.push_back(std::move(candidate));
  }
  const auto selectsAll = [&] {
    return UavTrackingCoordinator::hasRequiredParticipants(
      plan.participantSelector->select(candidates, plan.roles), nullptr, true);
  };
  BOOST_CHECK(!selectsAll());
  const auto setFeedback = [&] (const Fields& fields) {
    const auto text = encodeFields(fields);
    ndn::Buffer payload(text.begin(), text.end());
    candidates.back().ack.setPayload(payload, payload.size());
  };
  auto wrong = expected;
  wrong["source_epoch"] = "18";
  setFeedback(wrong);
  BOOST_CHECK(!selectsAll());
  setFeedback(expected);
  BOOST_CHECK(selectsAll());
}

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
    candidate.serviceName = SERVICE_TRACKING_WINDOW;
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

BOOST_AUTO_TEST_CASE(ProtectedDiscoveryPublishesOnlyBoundedSelectionMetadata)
{
  namespace nsf = ndn_service_framework;
  Fields initial{{"schema", "spec191-window-request-v1"}, {"run_id", "run"},
    {"mission_id", "tracking-run"}, {"window_id", "window-0"},
    {"tracking_request_timeout_ms", "60000"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "7"},
    {"source_uav1_id", "source1"}, {"source_uav2_id", "source2"}, {"source_uav3_id", "source3"},
    {"tracking_mode", "indoor-person-reid"},
    {"tracking_profile_digest", "sha256:" + std::string(64, 'a')},
    {"tracking_backend_closure_digest", "sha256:" + std::string(64, 'b')},
    {"tracking_native_closure_digest", "sha256:" + std::string(64, 'c')},
    {"tracking_computation_digest", "sha256:" + std::string(64, 'd')},
    {"tracking_profile_json", std::string(8192, 'p')},
    {"motion_window_json", std::string(8192, 'm')}, {"motion_window_digest", "private-digest"},
    {"f4_altered_frame", "1"}};
  const auto discovery = makeUavTrackingDiscoveryFields(initial);
  BOOST_REQUIRE_EQUAL(discovery.size(), 15);
  BOOST_CHECK_EQUAL(discovery.at("run_id"), "run");
  BOOST_CHECK_EQUAL(discovery.at("tracking_computation_digest"), initial.at("tracking_computation_digest"));
  BOOST_CHECK_EQUAL(discovery.count("tracking_profile_json"), 0);
  BOOST_CHECK_EQUAL(discovery.count("motion_window_json"), 0);
  BOOST_CHECK_EQUAL(discovery.count("motion_window_digest"), 0);
  BOOST_CHECK_EQUAL(discovery.count("f4_altered_frame"), 0);
  nsf::RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  capabilities.setField("UavTrackingDiscoveryV1", "required");
  for (const auto& [key, value] : discovery) capabilities.setField("UavTracking." + key, value);
  nsf::RequestMessage request;
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK(readUavTrackingAckFields(request) == discovery);
  BOOST_CHECK_THROW(validateUavTrackingDiscoveryBinding(request), std::invalid_argument);
  auto payloadText = encodeFields(initial);
  ndn::Buffer payload(payloadText.begin(), payloadText.end());
  request.setPayload(payload, payload.size());
  BOOST_CHECK_NO_THROW(validateUavTrackingDiscoveryBinding(request));
  initial["tracking_request_timeout_ms"] = "60001";
  payloadText = encodeFields(initial);
  payload = ndn::Buffer(payloadText.begin(), payloadText.end());
  request.setPayload(payload, payload.size());
  BOOST_CHECK_THROW(validateUavTrackingDiscoveryBinding(request), std::invalid_argument);
  initial["tracking_request_timeout_ms"] = "60000";
  initial["source_epoch"] = "8";
  payloadText = encodeFields(initial);
  payload = ndn::Buffer(payloadText.begin(), payloadText.end());
  request.setPayload(payload, payload.size());
  BOOST_CHECK_THROW(validateUavTrackingDiscoveryBinding(request), std::invalid_argument);
  initial["run_id"] = std::string(nsf::RequestCapabilities::MAX_FIELD_VALUE + 1, 'x');
  BOOST_CHECK_THROW(makeUavTrackingDiscoveryFields(initial), std::invalid_argument);
  capabilities.setField("UavTracking.motion_window_json", "forbidden");
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK_THROW(readUavTrackingAckFields(request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequestBudgetHasCanonicalBoundsAndExplicitHistoricalDefault)
{
  BOOST_CHECK_EQUAL(readUavTrackingRequestTimeout({}, false), 30000);
  BOOST_CHECK_THROW(readUavTrackingRequestTimeout({}, true), std::invalid_argument);
  for (const auto* valid : {"1001", "30000", "1200000"}) {
    BOOST_CHECK_EQUAL(readUavTrackingRequestTimeout({{"tracking_request_timeout_ms", valid}}, true),
                      std::stoull(valid));
  }
  for (const auto* bad : {"", "0", "1000", "01001", "+1001", "-1001", "1001x", "1200001",
                          "18446744073709551616"}) {
    BOOST_CHECK_THROW(readUavTrackingRequestTimeout({{"tracking_request_timeout_ms", bad}}, true),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(ProtectedDiscoveryCannotOmitOrAlterItsProtocolMarker)
{
  namespace nsf = ndn_service_framework;
  nsf::RequestMessage request;
  nsf::RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK_THROW(readUavTrackingAckFields(request), std::invalid_argument);
  capabilities.setField("UavTracking.schema", "spec191-window-request-v1");
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK_THROW(readUavTrackingAckFields(request), std::invalid_argument);
  capabilities.setField("UavTrackingDiscoveryV1", "wrong");
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK_THROW(readUavTrackingAckFields(request), std::invalid_argument);
  capabilities.setField("UavTrackingDiscoveryV1", "required");
  capabilities.setField("RequestScopedConfidentialityV1", "optional");
  request.setRequestCapabilities(capabilities);
  BOOST_CHECK_THROW(readUavTrackingAckFields(request), std::invalid_argument);
  Fields fields{{"schema", "other"}};
  BOOST_CHECK_THROW(makeUavTrackingDiscoveryFields(fields), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
