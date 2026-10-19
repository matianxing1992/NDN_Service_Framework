#include "../../NDNSF-UAV-APP/shared/UavObservationRefVerifier.hpp"
#include "../../NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ndnsf::examples::uav;

namespace {

std::string
sha256(const std::vector<uint8_t>& bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return canonicalSha256Digest(digest.toString());
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavObservationFlowIntegrationTests)

BOOST_AUTO_TEST_CASE(StoredDependencyAndVerifiedReferenceShareOneExactIdentity)
{
  const ndn::Name producer("/example/uav/drone/UAV1");
  const std::string exactName =
    "/example/uav/drone/UAV1/UAV/CLOCK/4/v=1";
  const std::vector<uint8_t> payload{'{', '"', 'r', 'e', 'v', 'i', 's', 'i', 'o', 'n',
                                     '"', ':', '"', '4', '"', '}'};
  const auto contentDigest = sha256(payload);

  ObservationDataObject clock;
  clock.category = "CLOCK";
  clock.exactName = exactName;
  clock.contentSha256 = contentDigest;
  clock.producerIdentity = producer.toUri();
  clock.payload = payload;

  FrameEnvelope frame;
  frame.cameraId = "UAV1";
  frame.sourceEpoch = 7;
  frame.sequence = 2;
  frame.ptsUs = 250000;
  frame.width = 640;
  frame.height = 480;
  frame.encoding = "image/jpeg";
  frame.bytes = {0xff, 0xd8, 0xff, 0xd9};
  frame.observationMetadata = R"({"schema":"uav-observation-v2"})";
  frame.observationObjects.push_back(clock);

  UavTrackingEvidenceStore store(16, 1024);
  std::string reason;
  const auto staged = store.stage(producer, "mission", "window-1", frame, &reason);
  BOOST_REQUIRE_MESSAGE(staged, reason);
  BOOST_REQUIRE_EQUAL(staged->observationObjects.size(), 1U);
  BOOST_CHECK_EQUAL(staged->observationObjects.front().exactName, exactName);
  BOOST_CHECK_EQUAL(staged->observationObjects.front().contentSha256, contentDigest);

  const auto reference = parseObservationReference(
    exactName, contentDigest, producer.toUri(), "CLOCK");
  requireObservationReferenceNameShape(reference, "CLOCK", "UAV1", 7, 2);
  validateObservationReference(reference, ndn::Name(exactName), producer,
                              staged->observationObjects.front().contentSha256, "CLOCK");

  auto nextFrame = frame;
  nextFrame.sourceEpoch = 8;
  nextFrame.sequence = 99;
  const auto nextStaged = store.stage(producer, "mission", "window-2", nextFrame, &reason);
  BOOST_REQUIRE_MESSAGE(nextStaged, reason);
  BOOST_REQUIRE_EQUAL(nextStaged->observationObjects.size(), 1U);
  BOOST_CHECK_EQUAL(nextStaged->observationObjects.front().exactName, exactName);
  BOOST_CHECK_EQUAL(nextStaged->observationObjects.front().contentSha256, contentDigest);
}

BOOST_AUTO_TEST_CASE(ReferenceVerifierRejectsTamperingAndStaleOrWrongIdentity)
{
  const ndn::Name producer("/example/uav/drone/UAV1");
  const ndn::Name otherProducer("/example/uav/drone/UAV2");
  const std::string exactName =
    "/example/uav/drone/UAV1/UAV/CLOCK/4/v=1";
  const auto contentDigest = "sha256:" + std::string(64, 'a');

  const auto reference = parseObservationReference(
    exactName, contentDigest, producer.toUri(), "CLOCK");
  BOOST_CHECK_THROW(parseObservationReference(exactName, "sha256:bad", producer.toUri(), "CLOCK"),
                    std::runtime_error);
  BOOST_CHECK_THROW(parseObservationReference(exactName, contentDigest,
                                               otherProducer.toUri(), "CLOCK"),
                    std::runtime_error);
  BOOST_CHECK_THROW(validateObservationReference(reference, ndn::Name(exactName), producer,
                                                  "sha256:" + std::string(64, 'b'), "CLOCK"),
                    std::runtime_error);
  BOOST_CHECK_THROW(validateObservationReference(reference, ndn::Name("/expected/other"),
                                                  producer, contentDigest, "CLOCK"),
                    std::runtime_error);
  BOOST_CHECK_THROW(validateObservationReference(reference, ndn::Name(exactName), otherProducer,
                                                  contentDigest, "CLOCK"),
                    std::runtime_error);
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(reference, "CLOCK", "UAV1", 8, 2));
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(reference, "CLOCK", "UAV1", 7, 3));
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(reference, "GROUND", "UAV1", 7, 2),
                    std::runtime_error);

  const auto poseReference = parseObservationReference(
    "/example/uav/drone/UAV1/UAV/OBS/UAV1/7/2/POSE/v=3", contentDigest,
    producer.toUri(), "POSE");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(poseReference, "POSE", "UAV1", 7, 2));
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(poseReference, "POSE", "UAV1", 8, 2),
                    std::runtime_error);
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(poseReference, "POSE", "UAV1", 7, 3),
                    std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
