#include "../../NDNSF-UAV-APP/shared/UavFrameIngress.hpp"

#include <boost/test/unit_test.hpp>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavFrameIngressTests)

BOOST_AUTO_TEST_CASE(RejectsInvalidAndOutOfOrderFrames)
{
  UavFrameIngress ingress;
  FrameEnvelope frame{"UAV1", 1, 1, 10, 4, 4, "image/jpeg", {1, 2, 3}};
  std::string reason;
  BOOST_CHECK(ingress.submit(frame, &reason));
  frame.sequence = 1;
  BOOST_CHECK(!ingress.submit(frame, &reason));
  BOOST_CHECK_EQUAL(reason, "sequence is not strictly increasing");
  frame.sequence = 2;
  frame.cameraId = "UAV9";
  BOOST_CHECK(!ingress.submit(frame, &reason));
  BOOST_CHECK_EQUAL(reason, "unknown camera");
}

BOOST_AUTO_TEST_CASE(BoundsAndCloseAreEnforced)
{
  FrameIngressConfig config;
  config.maxQueuedFrames = 1;
  UavFrameIngress ingress(config);
  std::string reason;
  BOOST_CHECK(ingress.submit(FrameEnvelope{"UAV1", 1, 1, 0, 4, 4, "image/jpeg", {1}}, &reason));
  BOOST_CHECK(!ingress.submit(FrameEnvelope{"UAV1", 1, 2, 1, 4, 4, "image/jpeg", {2}}, &reason));
  BOOST_CHECK_EQUAL(reason, "bounded ingress queue is full");
  ingress.close();
  BOOST_CHECK(!ingress.submit(FrameEnvelope{"UAV1", 1, 3, 2, 4, 4, "image/jpeg", {3}}, &reason));
  BOOST_CHECK_EQUAL(reason, "ingress is closed");
}

BOOST_AUTO_TEST_CASE(MotionMetadataIsBounded)
{
  UavFrameIngress ingress;
  FrameEnvelope frame{"UAV1", 1, 1, 0, 4, 4, "image/jpeg", {1}};
  frame.motionMetadata.assign(256 * 1024 + 1, 'x');
  std::string reason;
  BOOST_CHECK(!ingress.submit(std::move(frame), &reason));
  BOOST_CHECK_EQUAL(reason, "motion metadata exceeds 256 KiB");
}

BOOST_AUTO_TEST_SUITE_END()
