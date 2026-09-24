#include "../../NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"

#include <boost/test/unit_test.hpp>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavTrackingEvidenceTests)

BOOST_AUTO_TEST_CASE(StageSplitsBytesAndBindsProducerName)
{
  UavTrackingEvidenceStore store(3, 32);
  FrameEnvelope frame{"UAV1", 1, 1, 12000, 2, 2, "image/jpeg", {1, 2, 3, 4, 5, 6, 7}};
  std::string reason;
  const auto publication = store.stage(ndn::Name("/uav/1"), "mission", "window-0", frame, &reason);
  BOOST_REQUIRE(publication);
  BOOST_CHECK_EQUAL(publication->segments.size(), 3);
  BOOST_CHECK(publication->reference.exactDataName.isPrefixOf(ndn::Name("/uav/1")) == false);
  BOOST_CHECK_EQUAL(publication->reference.contentDigest.size(), 71);
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

BOOST_AUTO_TEST_SUITE_END()
