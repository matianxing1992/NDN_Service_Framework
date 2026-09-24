#include "../../NDNSF-UAV-APP/shared/UavFrameIngress.hpp"
#include "../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp"
#include "../../NDNSF-UAV-APP/shared/UavNames.hpp"
#include "../../NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"
#include "../../NDNSF-UAV-APP/tracking/UavTrackingSession.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace uav = ndnsf::examples::uav;

namespace {

std::string
digest(const ndn::Buffer& bytes)
{
  ndn::util::Sha256 sha;
  sha << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return "sha256:" + sha.toString();
}

uav::VerifiedMultiViewInput
makeInput(const std::string& id, const ndn::Name& producer, uint64_t time,
          const std::string& payload)
{
  const ndn::Name dataName = uav::makeUavEvidenceName(
    producer, "mission-1", "window-0", id, 1);
  const ndn::Buffer bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  uav::VerifiedMultiViewInput input;
  input.reference.viewId = id;
  input.reference.producerIdentity = producer;
  input.reference.exactDataName = dataName;
  input.reference.contentDigest = digest(bytes);
  input.reference.captureTimeMs = time;
  input.reference.targetId = "target-1";
  input.reference.mediaType = "image/jpeg";
  input.reference.viewpoint = id == "UAV1" ? "front" :
                              id == "UAV2" ? "side" : "rear";
  input.content = bytes;
  input.signerIdentity = producer;
  input.nameVerified = true;
  input.signatureVerified = true;
  input.digestVerified = true;
  return input;
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingFlowIntegrationTests)

BOOST_AUTO_TEST_CASE(ThreeSourcesProduceBoundedNamedEvidenceAndOneTerminalResult)
{
  uav::UavTrackingEvidenceStore store(4, 32);
  uav::FrameIngressConfig config;
  config.maxQueuedFrames = 4;
  config.maxQueuedBytes = 32;
  config.maxFrameBytes = 16;
  uav::UavFrameIngress ingress(config);

  for (const auto& camera : {std::string("UAV1"), std::string("UAV2"), std::string("UAV3")}) {
    uav::FrameEnvelope frame;
    frame.cameraId = camera;
    frame.sourceEpoch = 1;
    frame.sequence = 1;
    frame.ptsUs = 1000;
    frame.width = 2;
    frame.height = 2;
    frame.bytes = {1, 2, 3, 4};
    BOOST_REQUIRE(ingress.submit(frame));
    std::string reason;
    const auto staged = store.stage(uav::droneIdentity(camera), "mission-1", "window-0",
                                    frame, &reason);
    BOOST_REQUIRE_MESSAGE(staged, reason);
    BOOST_REQUIRE(uav::droneIdentity(camera).isPrefixOf(
      staged->reference.exactDataName));
    BOOST_CHECK_EQUAL(staged->segments.size(), 1);
    BOOST_CHECK_EQUAL(staged->payload.size(), frame.bytes.size());
  }

  const auto p1 = uav::droneIdentity("UAV1");
  const auto p2 = uav::droneIdentity("UAV2");
  const auto p3 = uav::droneIdentity("UAV3");
  uav::MultiViewModelProfile profile;
  profile.profileId = "mvcpu-v1";
  profile.algorithmId = "mvcpu-max-v1";
  profile.modelId = "3UAVs.pt";
  profile.modelDigest = "sha256:" + std::string(64, 'a');
  profile.preprocessingProfile = "jpeg-native";
  profile.deviceClass = "cpu";
  profile.minimumViews = 3;
  profile.minimumDistinctProducers = 3;

  uav::MultiViewRecognitionJob job;
  job.missionSessionId = "mission-1";
  job.jobId = "window-0";
  job.targetId = "target-1";
  job.captureWindowEndMs = 1000;
  job.views = {{"UAV1", p1, {}, {}, 1000, "target-1", "image/jpeg", "front", std::nullopt},
               {"UAV2", p2, {}, {}, 1000, "target-1", "image/jpeg", "side", std::nullopt},
               {"UAV3", p3, {}, {}, 1000, "target-1", "image/jpeg", "rear", std::nullopt}};
  job.minimumViews = 3;
  job.minimumDistinctProducers = 3;
  job.modelProfileId = profile.profileId;
  // Bind exact names and digests to the same bytes used by the execution path.
  const auto inputs = std::vector<uav::VerifiedMultiViewInput>{
    makeInput("UAV1", p1, 1000, "frame-1"),
    makeInput("UAV2", p2, 1000, "frame-2"),
    makeInput("UAV3", p3, 1000, "frame-3")};
  for (size_t index = 0; index < inputs.size(); ++index) {
    job.views[index].exactDataName = inputs[index].reference.exactDataName;
    job.views[index].contentDigest = inputs[index].reference.contentDigest;
  }
  std::string reason;
  BOOST_REQUIRE_MESSAGE(job.isValid(&profile, &reason), reason);
  const auto execution = uav::executeMultiViewJob(
    job, profile, inputs, ndn::Name("/example/uav/compute"));
  BOOST_REQUIRE_MESSAGE(execution.success, execution.detail);
  BOOST_CHECK(static_cast<int>(execution.result.status) ==
              static_cast<int>(uav::MultiViewTerminalStatus::Completed));
  BOOST_CHECK_EQUAL(execution.result.consumedViewCount, 3);
  BOOST_CHECK_EQUAL(execution.result.terminalOwner.toUri(), "/example/uav/compute");
}

BOOST_AUTO_TEST_CASE(SessionRejectsSecondTerminalWindowAfterAbort)
{
  uav::UavTrackingSession session("mission-1", 2, 2);
  const auto executor = [] (const uav::TrackingWindow& window) {
    return std::string("result-") + std::to_string(window.index);
  };
  uav::TrackingWindow first{"request-1", 1, 0, "sha256:" + std::string(64, 'a'), 0, 100};
  std::string reason;
  BOOST_REQUIRE(session.process(first, executor, &reason));
  session.abort("compute worker stopped");
  uav::TrackingWindow second{"request-2", 1, 1, "sha256:" + std::string(64, 'b'), 100, 200};
  BOOST_CHECK(!session.process(second, executor, &reason));
  BOOST_CHECK(static_cast<int>(session.state()) ==
              static_cast<int>(uav::TrackingSessionState::Aborted));
  BOOST_CHECK_EQUAL(reason, "session is aborted");
}

BOOST_AUTO_TEST_SUITE_END()
