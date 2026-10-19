#include "../../NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"
#include "../../NDNSF-UAV-APP/shared/UavObservationRefVerifier.hpp"

#include <limits>

#include <ndn-cxx/util/sha256.hpp>

#include <boost/test/unit_test.hpp>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavTrackingEvidenceTests)

BOOST_AUTO_TEST_CASE(StageSplitsBytesAndBindsProducerName)
{
  UavTrackingEvidenceStore store(3, 32);
  FrameEnvelope frame{"UAV1", 1, 1, 12000, 2, 2, "image/jpeg", {1, 2, 3, 4, 5, 6, 7}};
  frame.motionMetadata = "motion-window";
  std::string reason;
  const auto publication = store.stage(ndn::Name("/uav/1"), "mission", "window-0", frame, &reason);
  BOOST_REQUIRE(publication);
  BOOST_CHECK_EQUAL(publication->segments.size(), 3);
  BOOST_CHECK(publication->reference.exactDataName.isPrefixOf(ndn::Name("/uav/1")) == false);
  BOOST_CHECK_EQUAL(publication->reference.contentDigest.size(), 71);
  BOOST_CHECK_EQUAL(publication->reference.motionMetadataDigest.size(), 71);
  BOOST_CHECK_EQUAL(store.retainedBytes(), frame.bytes.size());
}

BOOST_AUTO_TEST_CASE(RejectsRetentionOverflow)
{
  UavTrackingEvidenceStore store(4, 4);
  FrameEnvelope frame{"UAV1", 1, 1, 0, 1, 1, "image/jpeg", {1, 2, 3, 4, 5}};
  std::string reason;
  BOOST_CHECK(!store.stage(ndn::Name("/uav/1"), "m", "w", frame, &reason));
  BOOST_CHECK_EQUAL(reason, "evidence retention bound exceeded");
}

BOOST_AUTO_TEST_CASE(StageBindsSourceObservationSidecarAndCountsItsRetention)
{
  const std::string metadata = R"({"schema":"uav-observation-v2"})";
  UavTrackingEvidenceStore store(3, 128);
  FrameEnvelope frame{"UAV1", 7, 2, 250000, 100, 80, "image/jpeg", {1, 2, 3, 4}};
  frame.observationMetadata = metadata;
  const auto publication = store.stage(
    ndn::Name("/example/uav/drone/UAV1"), "mission", "window-1", frame);
  BOOST_REQUIRE(publication);
  BOOST_CHECK(ndn::Name("/example/uav/drone/UAV1").isPrefixOf(
    publication->observationName));
  BOOST_CHECK_EQUAL(publication->observationName.toUri(),
    "/example/uav/drone/UAV1/UAV/OBS/UAV1/7/2/OBSERVATION/v=1");
  BOOST_CHECK_EQUAL(publication->observationPayload.size(), metadata.size());
  BOOST_CHECK_EQUAL(publication->observationDigest.size(), 71);
  BOOST_CHECK_EQUAL(store.retainedBytes(), frame.bytes.size() + metadata.size());
}

BOOST_AUTO_TEST_CASE(StaticReferenceNamesBindProducerRevisionAndTypedVersion)
{
  const ndn::Name producer("/example/uav/drone/UAV1");
  const std::string digest = "sha256:" + std::string(64, 'a');
  const auto clock = parseObservationReference(
    "/example/uav/drone/UAV1/UAV/CLOCK/4/v=17", digest, producer.toUri(), "CLOCK");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(clock, "CLOCK", "UAV1", 7, 2));
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(clock, "CLOCK", "UAV1", 8, 99));
  BOOST_CHECK_EQUAL(makeUavGroundDataName(producer, 4, 19).toUri(),
                    "/example/uav/drone/UAV1/UAV/GROUND/4/v=19");
  const auto ground = parseObservationReference(
    "/example/uav/drone/UAV1/UAV/GROUND/4/v=19", digest, producer.toUri(), "GROUND");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(ground, "GROUND", "UAV1", 7, 2));
  const auto external = parseObservationReference(
    "/example/calibration/provider/UAV/CALIBRATION/UAV1/4/v=23", digest,
    "/example/calibration/provider", "CALIBRATION");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(external, "CALIBRATION", "UAV1", 7, 2));
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(external, "CALIBRATION", "UAV1", 8, 99));
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(external, "CALIBRATION", "UAV2", 7, 2),
                    std::runtime_error);
  const auto maxName = makeUavClockDataName(producer, std::numeric_limits<uint64_t>::max(),
                                           std::numeric_limits<uint64_t>::max());
  const auto maxReference = parseObservationReference(maxName.toUri(), digest, producer.toUri(), "CLOCK");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(maxReference, "CLOCK", "UAV1", 7, 2));
  const auto literalCameraName = makeUavCalibrationDataName(producer, "UAV.+", 4, 23);
  const auto literalCamera = parseObservationReference(
    literalCameraName.toUri(), digest, producer.toUri(), "CALIBRATION");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(literalCamera, "CALIBRATION", "UAV.+", 7, 2));
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(literalCamera, "CALIBRATION", "UAVXYZ", 7, 2),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(StaticReferenceNamesRejectAliasesOverflowAndWrongTypes)
{
  const std::string digest = "sha256:" + std::string(64, 'a');
  const std::string producer = "/example/uav/drone/UAV1";
  const std::vector<std::string> tails{
    "0/v=17", "01/v=17", "18446744073709551616/v=17", "seg=1/v=17",
    "1/v=0", "1/17", "1/seq=17", "1/v=17/extra"};
  for (const auto& category : {std::string("CLOCK"), std::string("CALIBRATION"), std::string("GROUND")}) {
    const auto categoryPath = category == "CALIBRATION" ? category + "/UAV1" : category;
    for (const auto& tail : tails) {
      const auto name = producer + "/UAV/" + categoryPath + "/" + tail;
      const auto reference = parseObservationReference(ndn::Name(name).toUri(), digest, producer, category);
      BOOST_CHECK_THROW(requireObservationReferenceNameShape(reference, category, "UAV1", 7, 2),
                        std::runtime_error);
    }
    const auto old = parseObservationReference(
      producer + "/UAV/OBS/UAV1/7/2/" + category + "/1/v=1", digest, producer, category);
    BOOST_CHECK_THROW(requireObservationReferenceNameShape(old, category, "UAV1", 7, 2),
                      std::runtime_error);
  }
  for (const auto* version : {"v=017", "v=18446744073709551616"}) {
    BOOST_CHECK_THROW(parseObservationReference(producer + "/UAV/CLOCK/1/" + version,
                                                digest, producer, "CLOCK"), std::exception);
  }
  const auto pose = parseObservationReference(
    producer + "/UAV/OBS/UAV1/7/2/POSE/v=9", digest, producer, "POSE");
  BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(pose, "POSE", "UAV1", 7, 2));
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(pose, "POSE", "UAV1", 7, 3), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(StageRetainsSourceOwnedReferenceClosureAndRejectsWrongProducer)
{
  const ndn::Name producer("/example/uav/drone/UAV1");
  const std::string clockPayload = R"({"revision":"4"})";
  ndn::util::Sha256 sha;
  sha << clockPayload;
  ObservationDataObject clock;
  clock.category = "CLOCK";
  clock.exactName = makeUavClockDataName(producer, 4, 17).toUri();
  BOOST_CHECK_EQUAL(clock.exactName, "/example/uav/drone/UAV1/UAV/CLOCK/4/v=17");
  BOOST_CHECK_NE(makeUavClockDataName(producer, 4, 17), makeUavClockDataName(producer, 5, 17));
  BOOST_CHECK_NE(makeUavClockDataName(producer, 4, 17), makeUavClockDataName(producer, 4, 18));
  clock.contentSha256 = canonicalSha256Digest(sha.toString());
  clock.producerIdentity = producer.toUri();
  clock.payload.assign(clockPayload.begin(), clockPayload.end());

  FrameEnvelope frame{"UAV1", 7, 2, 250000, 100, 80, "image/jpeg", {1, 2, 3, 4}};
  frame.observationMetadata = R"({"schema":"uav-observation-v2"})";
  frame.observationObjects.push_back(clock);
  UavTrackingEvidenceStore store(32, 1024);
  std::string stageReason;
  const auto staged = store.stage(producer, "m", "window-1", frame, &stageReason);
  BOOST_REQUIRE_MESSAGE(staged, stageReason);
  BOOST_REQUIRE_EQUAL(staged->observationObjects.size(), 1);
  BOOST_CHECK_EQUAL(staged->observationObjects.front().exactName, clock.exactName);
  BOOST_CHECK_EQUAL(store.retainedBytes(), frame.bytes.size() +
                    frame.observationMetadata.size() + clock.payload.size());

  auto wrongProducer = frame;
  wrongProducer.observationObjects.front().exactName =
    "/example/uav/drone/UAV2/UAV/CLOCK/4/v=17";
  std::string reason;
  BOOST_CHECK(!store.stage(producer, "m", "window-1", wrongProducer, &reason));
  BOOST_CHECK_EQUAL(reason, "observation dependency identity or digest is invalid");

  auto wrongDigest = frame;
  wrongDigest.observationObjects.front().contentSha256 = "sha256:" + std::string(64, '0');
  BOOST_CHECK(!store.stage(producer, "m", "window-1", wrongDigest, &reason));
  BOOST_CHECK_EQUAL(reason, "observation dependency identity or digest is invalid");
}

BOOST_AUTO_TEST_CASE(ObservationSidecarCannotExceedRetentionBound)
{
  UavTrackingEvidenceStore store(4, 8);
  FrameEnvelope frame{"UAV1", 1, 1, 0, 1, 1, "image/jpeg", {1, 2, 3, 4}};
  frame.observationMetadata.assign(5, 'x');
  std::string reason;
  BOOST_CHECK(!store.stage(ndn::Name("/uav/1"), "m", "w", frame, &reason));
  BOOST_CHECK_EQUAL(reason, "evidence retention bound exceeded");
  BOOST_CHECK_EQUAL(store.retainedBytes(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
