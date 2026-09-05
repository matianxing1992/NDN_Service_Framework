#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp"
#include "NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp"
#include "NDNSF-UAV-APP/shared/UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

namespace ndnsf::examples::uav::tests {
namespace {

std::string digest(const ndn::Buffer& content)
{
  ndn::util::Sha256 sha;
  sha << std::string(reinterpret_cast<const char*>(content.data()), content.size());
  return "sha256:" + sha.toString();
}

BOOST_AUTO_TEST_SUITE(UavMultiViewIntegration)

BOOST_AUTO_TEST_CASE(CpuProviderConsumesOnlyVerifiedMultiViewInputs)
{
  MultiViewModelProfile profile;
  profile.profileId = "vehicle-mvcnn-v1";
  profile.algorithmId = "detector-guided-mvcnn-pooling/v1";
  profile.modelId = "cpu-fixture";
  profile.modelDigest = "sha256:" + std::string(64, 'a');
  profile.preprocessingProfile = "rgb-resize-224-center-crop/v1";

  MultiViewRecognitionJob job;
  job.missionSessionId = "mission";
  job.jobId = "job";
  job.targetId = "car";
  job.captureWindowStartMs = 0;
  job.captureWindowEndMs = 1000;
  job.modelProfileId = profile.profileId;
  std::vector<VerifiedMultiViewInput> inputs;
  for (size_t i = 0; i < 2; ++i) {
    ViewEvidenceReference reference;
    reference.viewId = "view-" + std::to_string(i + 1);
    reference.producerIdentity = ndn::Name(i == 0 ? "/uav/A" : "/uav/B");
    reference.exactDataName = reference.producerIdentity;
    reference.exactDataName.append("UAV").append("IMAGE").append(reference.viewId).appendVersion(1);
    reference.captureTimeMs = 100 + i;
    reference.targetId = "car";
    VerifiedMultiViewInput input;
    input.reference = reference;
    input.content = ndn::Buffer{static_cast<uint8_t>('x' + i)};
    input.reference.contentDigest = digest(input.content);
    input.signerIdentity = reference.producerIdentity;
    input.nameVerified = input.signatureVerified = input.digestVerified = true;
    job.views.push_back(input.reference);
    inputs.push_back(input);
  }

  const auto execution = executeMultiViewJob(job, profile, inputs, ndn::Name("/provider/cpu"));
  BOOST_REQUIRE_MESSAGE(execution.success, execution.detail);
  BOOST_CHECK(execution.result.status == MultiViewTerminalStatus::Completed);
  BOOST_CHECK_EQUAL(execution.result.consumedViewCount, 2U);
  BOOST_CHECK_EQUAL(execution.result.annotatedViews.size(), 2U);
}

BOOST_AUTO_TEST_CASE(UnverifiedOrMissingViewCannotReachFusion)
{
  MultiViewModelProfile profile;
  profile.profileId = "vehicle-mvcnn-v1";
  profile.algorithmId = "detector-guided-mvcnn-pooling/v1";
  profile.modelId = "cpu-fixture";
  profile.modelDigest = "sha256:" + std::string(64, 'b');
  profile.preprocessingProfile = "rgb-resize-224-center-crop/v1";
  MultiViewRecognitionJob job;
  job.missionSessionId = "mission";
  job.jobId = "job";
  job.targetId = "car";
  job.captureWindowEndMs = 1000;
  job.modelProfileId = profile.profileId;
  ViewEvidenceReference view;
  view.viewId = "view-1";
  view.producerIdentity = ndn::Name("/uav/A");
  view.exactDataName = ndn::Name("/uav/A/UAV/IMAGE/view-1").appendVersion(1);
  view.contentDigest = "sha256:" + std::string(64, 'c');
  view.captureTimeMs = 100;
  view.targetId = "car";
  job.views = {view};
  VerifiedMultiViewInput input;
  input.reference = view;
  input.content = ndn::Buffer{'x'};
  input.signerIdentity = ndn::Name("/attacker");
  const auto execution = executeMultiViewJob(job, profile, {input}, ndn::Name("/provider/cpu"));
  BOOST_CHECK(!execution.success);
  BOOST_CHECK(execution.result.status != MultiViewTerminalStatus::Completed);
}

BOOST_AUTO_TEST_CASE(CoordinatorAcceptsOnlySelectedProviderMultiViewResult)
{
  auto mission = UavMissionSession::create(
    "mission-mv", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(mission.start());

  const auto producer = ndn::Name("/example/uav/drone/A");
  const auto evidenceContent = ndn::Buffer{'e', 'v', 'i', 'd', 'e', 'n', 'c', 'e'};
  UavEvidenceReference evidence;
  evidence.producerIdentity = producer;
  evidence.streamId = "camera-A";
  evidence.streamSessionEpoch = 1;
  evidence.firstSequence = 1;
  evidence.lastSequence = 1;
  evidence.windowStartMs = 100;
  evidence.windowEndMs = 200;
  evidence.exactDataName = makeUavEvidenceName(
    producer, "mission-mv", "incident-mv", "frame-1", 1);
  evidence.version = 1;
  evidence.contentDigest = digest(evidenceContent);
  evidence.contentType = "image/png";
  evidence.retentionDeadlineMs = 10000;

  UavIncidentRecord incident;
  incident.incidentId = "incident-mv";
  incident.missionId = "mission-mv";
  incident.triggerKind = "operator";
  incident.triggerTimeMs = 100;
  incident.currentAttemptId = "attempt-mv";
  incident.evidence.push_back(evidence);
  BOOST_REQUIRE(mission.recordIncident(incident));

  UavIncidentCoordinator coordinator(
    mission, incident, UavIncidentCoordinatorConfig{100, 1000, 4, 4, 1, {}});
  MultiViewModelProfile profile;
  profile.profileId = "vehicle-mvcnn-v1";
  profile.algorithmId = "detector-guided-mvcnn-pooling/v1";
  profile.modelId = "cpu-fixture";
  profile.modelDigest = "sha256:" + std::string(64, 'd');
  profile.preprocessingProfile = "rgb-resize-224-center-crop/v1";
  MultiViewRecognitionJob job;
  job.missionSessionId = "mission-mv";
  job.jobId = "job-mv";
  job.targetId = "car-1";
  job.captureWindowStartMs = 100;
  job.captureWindowEndMs = 200;
  job.modelProfileId = profile.profileId;
  for (size_t i = 0; i < 2; ++i) {
    ViewEvidenceReference view;
    view.viewId = "view-" + std::to_string(i + 1);
    view.producerIdentity = ndn::Name(i == 0 ? "/uav/A" : "/uav/B");
    view.exactDataName = view.producerIdentity;
    view.exactDataName.append("UAV").append("IMAGE").append(view.viewId).appendVersion(1);
    view.captureTimeMs = 100 + i;
    view.targetId = job.targetId;
    view.contentDigest = digest(ndn::Buffer{static_cast<uint8_t>('a' + i)});
    job.views.push_back(view);
  }

  std::string reason;
  BOOST_REQUIRE(coordinator.beginMultiViewJob(job, 0, &reason));
  BOOST_REQUIRE(coordinator.closeAcks(100, &reason));
  UavRoleAssignment evidenceRole;
  evidenceRole.role = "EvidenceSource";
  evidenceRole.providerIdentity = producer;
  evidenceRole.evidence.push_back(evidence);
  UavRoleAssignment detectorRole;
  detectorRole.role = "DetectorReporter";
  detectorRole.providerIdentity = ndn::Name("/provider/gpu");
  detectorRole.terminalResponseOwner = true;
  detectorRole.evidence.push_back(evidence);
  BOOST_REQUIRE(coordinator.commitPlan(
    {evidenceRole, detectorRole}, detectorRole.providerIdentity,
    "sha256:plan-mv", &reason));
  BOOST_REQUIRE(coordinator.selectProvider(detectorRole.providerIdentity, &reason));
  BOOST_REQUIRE(coordinator.markEvidenceReady(&reason));
  BOOST_REQUIRE(coordinator.markExecuting(&reason));
  BOOST_REQUIRE(coordinator.markReporting(&reason));

  std::vector<VerifiedMultiViewInput> inputs;
  for (const auto& view : job.views) {
    VerifiedMultiViewInput input;
    input.reference = view;
    input.content = ndn::Buffer{static_cast<uint8_t>(
      'a' + (view.viewId.back() - '1'))};
    input.signerIdentity = view.producerIdentity;
    input.nameVerified = input.signatureVerified = input.digestVerified = true;
    inputs.push_back(std::move(input));
  }
  // Bind each test payload to its declared digest before it crosses the
  // same verification boundary used by the production adapter.
  for (auto& input : inputs) input.reference.contentDigest = digest(input.content);
  auto execution = executeMultiViewJob(
    job, profile, inputs, detectorRole.providerIdentity);
  BOOST_REQUIRE_MESSAGE(execution.success, execution.detail);
  BOOST_REQUIRE(coordinator.acceptMultiViewResult(execution.result, 500, &reason));
  BOOST_CHECK(coordinator.job().state == UavCollaborationJobState::Succeeded);
  BOOST_CHECK(mission.record().jobs.front().state == UavCollaborationJobState::Succeeded);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndnsf::examples::uav::tests
