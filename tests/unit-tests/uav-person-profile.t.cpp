#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-detector-profile-golden.hpp"
#include "tests/fixtures/uav-person-profile-golden.hpp"

#include "NDNSF-UAV-APP/shared/uav-person-tracking-profile.hpp"
#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/detail/StrictJson.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ndnsf::test {
namespace {

using examples::uav::parseUavPersonTrackingProfile;

std::string
replaceOne(std::string input, const std::string& from, const std::string& to)
{
  const auto offset = input.find(from);
  if (offset == std::string::npos) throw std::logic_error("missing literal test token");
  input.replace(offset, from.size(), to);
  return input;
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonProfile)

BOOST_AUTO_TEST_CASE(Nms50CanonicalIdentity)
{
  const auto wire = replaceOne(std::string(PERSON_DETECTOR_PROFILE_GOLDEN),
                              "raw-box-v1", "raw-box-nms50-v1");
  const auto profile = parseUavPersonTrackingProfile(wire);
  BOOST_CHECK_EQUAL(profile.canonicalJson, wire);
  BOOST_CHECK_EQUAL(profile.profileDigest,
    "sha256:5b7013bc592de5ba46eb7e0c8261fd68f98892581f25c86e741e814cf362307e");
  BOOST_CHECK(profile.profileDigest != std::string(PERSON_DETECTOR_PROFILE_DIGEST));
}

BOOST_AUTO_TEST_CASE(ExplicitDetectorLiteralPreservesLegacyCanonicalIdentity)
{
  const std::string wire(PERSON_DETECTOR_PROFILE_GOLDEN);
  const auto profile = parseUavPersonTrackingProfile(wire);
  BOOST_CHECK_EQUAL(profile.canonicalJson, wire);
  BOOST_CHECK_EQUAL(profile.profileDigest, std::string(PERSON_DETECTOR_PROFILE_DIGEST));
  BOOST_CHECK(profile.profileDigest != std::string(PERSON_PROFILE_DIGEST));
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN)).profileDigest,
                    std::string(PERSON_PROFILE_DIGEST));
  const std::string detector = "\"detectorPreprocessId\":\"yolo-letterbox1280-raw-box-v1\"";
  const auto reordered = "{" + detector + "," + replaceOne(wire, detector + ",", "").substr(1);
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(reordered).canonicalJson, wire);
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(
    replaceOne(wire, "raw-box", "raw-\\u0062ox")).profileDigest, profile.profileDigest);
}

BOOST_AUTO_TEST_CASE(ExplicitDetectorRejectsMixedSchemasTypesAndUnknownModes)
{
  const std::string wire(PERSON_DETECTOR_PROFILE_GOLDEN);
  const std::string field = "\"detectorPreprocessId\":\"yolo-letterbox1280-raw-box-v1\"";
  for (const auto& invalid : {
        replaceOne(wire, field + ",", ""),
        replaceOne(wire, "profile-v2", "profile-v1"),
        replaceOne(wire, "profile-v2", "profile-v3"),
        replaceOne(wire, field, "\"detectorPreprocessId\":null"),
        replaceOne(wire, field, "\"detectorPreprocessId\":true"),
        replaceOne(wire, field, "\"detectorPreprocessId\":1280"),
        replaceOne(wire, field, "\"detectorPreprocessId\":[]"),
        replaceOne(wire, "letterbox1280-raw", "letterbox640-raw"),
        replaceOne(wire, "raw-box-v1", "raw-box-nms51-v1"),
        replaceOne(wire, "yolo-letterbox1280-raw-box-v1", "yolo-default640-stock-box-v1"),
        replaceOne(wire, field, field + "," + field)}) {
    BOOST_CHECK_THROW(parseUavPersonTrackingProfile(invalid), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(CanonicalBytesDigestAndRepoCompatibility)
{
  const std::string wire(PERSON_PROFILE_GOLDEN);
  const auto profile = parseUavPersonTrackingProfile(" \n" + wire + "\t");
  BOOST_CHECK_EQUAL(profile.canonicalJson, wire);
  BOOST_CHECK_EQUAL(profile.profileDigest, std::string(PERSON_PROFILE_DIGEST));
  BOOST_CHECK_EQUAL(profile.requestedDevice, "cuda:0");
  BOOST_CHECK_EQUAL(profile.limits.at("maxLocalKeys"), 1024);
  BOOST_CHECK(!profile.overlappingCameras.at({"UAV1", "UAV3"}));
  BOOST_CHECK(profile.overlappingCameras.at({"UAV1", "UAV2"}));
  const std::string mode = "\"trackingMode\":\"indoor-person-reid\"";
  const auto reorderedRoot = "{" + mode + "," + replaceOne(wire, "," + mode, "").substr(1);
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(reorderedRoot).canonicalJson, wire);
  const auto reorderedNested = replaceOne(replaceOne(wire, ",\"sampleHz\":2", ""),
                                         "\"limits\":{", "\"limits\":{\"sampleHz\":2,");
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(reorderedNested).canonicalJson, wire);
  static_assert(std::is_same_v<ndnsf_distributed_repo::detail::JsonValue,
                             ndn_service_framework::json::JsonValue>);
  const auto parsed = ndnsf_distributed_repo::detail::parseJson(wire, 16384);
  BOOST_CHECK_EQUAL(ndnsf_distributed_repo::detail::renderJson(parsed), wire);
  BOOST_CHECK_EQUAL(parseUavPersonTrackingProfile(
    replaceOne(wire, "UAV1", "UAV\\u0031")).profileDigest, profile.profileDigest);
}

BOOST_AUTO_TEST_CASE(GemReidPreprocessIdIsCanonicalAndClosed)
{
  const std::string gapId = "osnet-rgb-256x128-imagenet-l2-v1";
  const std::string gemId = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1";
  const std::string wire(PERSON_PROFILE_GOLDEN);
  const auto gap = parseUavPersonTrackingProfile(wire);
  BOOST_CHECK_EQUAL(gap.reidPreprocessId, gapId);

  const auto gemWire = replaceOne(wire, gapId, gemId);
  const auto gem = parseUavPersonTrackingProfile(gemWire);
  BOOST_CHECK_EQUAL(gem.canonicalJson, gemWire);
  BOOST_CHECK_EQUAL(gem.reidPreprocessId, gemId);
  BOOST_CHECK(gem.profileDigest != gap.profileDigest);
  BOOST_CHECK_THROW(parseUavPersonTrackingProfile(
    replaceOne(wire, gapId, "osnet-rgb-256x128-imagenet-unknown-v1")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectTypesDuplicatesBoundsAndUnknownFields)
{
  const std::string wire(PERSON_PROFILE_GOLDEN);
  std::vector<std::string> invalid = {
    "", "[]", "null", wire + "{}", std::string(16385, ' '),
    replaceOne(wire, "\"featureDimension\":512", "\"featureDimension\":true"),
    replaceOne(wire, "\"featureDimension\":512", "\"featureDimension\":512.0"),
    replaceOne(wire, "\"minTransitionUs\":0", "\"minTransitionUs\":-0"),
    replaceOne(wire, "\"minTransitionUs\":0", "\"minTransitionUs\":-1"),
    replaceOne(wire, "\"maxLocalKeys\":1024", "\"maxLocalKeys\":1025"),
    replaceOne(wire, "\"maxLocalKeys\":1024", "\"maxLocalKeys\":0"),
    replaceOne(wire, "\"maxLocalKeys\":1024", "\"maxLocalKeys\":18446744073709551616"),
    replaceOne(wire, "\"sampleHz\":2", "\"sampleHz\":true"),
    replaceOne(wire, "\"sampleHz\":2", "\"sampleHz\":\"2\""),
    replaceOne(wire, "\"sampleHz\":2", "\"sampleHz\":2,\"sampleHz\":2"),
    replaceOne(wire, "\"sampleHz\":2", "\"sampleHz\":2,\"extra\":1"),
    replaceOne(wire, "\"minTransitionUs\":0", "\"minTransitionUs\":30000001"),
    replaceOne(wire, "\"galleryTtlUs\":30000000", "\"galleryTtlUs\":1"),
    replaceOne(wire, "\"runDeadlineMs\":900000", "\"runDeadlineMs\":1"),
    replaceOne(wire, "\"maxBackendTracksPerCamera\":512", "\"maxBackendTracksPerCamera\":1"),
    replaceOne(wire, "\"requestedDevice\":\"cuda:0\"", "\"requestedDevice\":\"cuda:1\""),
    replaceOne(wire, "\"relation\":\"non-overlap\"", "\"relation\":\"road-lane\""),
    replaceOne(wire, "\"cameraA\":\"UAV1\"", "\"cameraA\":\"UAV2\""),
    replaceOne(wire, "\"cameraB\":\"UAV3\"", "\"cameraB\":\"UAV2\""),
    replaceOne(wire, "\"architecture\":\"osnet_x1_0\"", "\"architecture\":null"),
    replaceOne(wire, "sha256:aaaa", "sha256:AAAA"),
  };
  for (const auto& input : invalid) {
    BOOST_CHECK_THROW(parseUavPersonTrackingProfile(input), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(ComputationInputsChangeIdentity)
{
  const std::string wire(PERSON_PROFILE_GOLDEN);
  const auto original = parseUavPersonTrackingProfile(wire).profileDigest;
  for (const auto& changed : {
        replaceOne(wire, "sha256:aaaa", "sha256:caaa"),
        replaceOne(wire, "sha256:bbbb", "sha256:cbbb"),
        replaceOne(wire, "\"maxCosineDistancePpm\":300000", "\"maxCosineDistancePpm\":299999"),
        replaceOne(wire, "\"requestedDevice\":\"cuda:0\"", "\"requestedDevice\":\"cpu\"")}) {
    BOOST_CHECK(parseUavPersonTrackingProfile(changed).profileDigest != original);
  }
}

BOOST_AUTO_TEST_CASE(WindowCapacityAndCanonicalApplicationNames)
{
  using examples::uav::parseUavPersonWindowIndex;
  using examples::uav::requireUavPersonWindowCapacity;
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  BOOST_CHECK_NO_THROW(requireUavPersonWindowCapacity(profile, 1));
  BOOST_CHECK_NO_THROW(requireUavPersonWindowCapacity(profile, 120));
  for (const auto invalid : {uint64_t{0}, uint64_t{121}, std::numeric_limits<uint64_t>::max()}) {
    BOOST_CHECK_THROW(requireUavPersonWindowCapacity(profile, invalid), std::invalid_argument);
  }
  BOOST_CHECK_EQUAL(parseUavPersonWindowIndex(profile, "window-0"), 0);
  BOOST_CHECK_EQUAL(parseUavPersonWindowIndex(profile, "window-119"), 119);
  for (const auto* invalid : {"", "window", "window-", "window-00", "window-01", "window-+1",
                            "window--1", "window- 1", "window-1 ", "window-1-extra", "WINDOW-1",
                            "window-120", "window-18446744073709551615",
                            "window-18446744073709551616"}) {
    BOOST_CHECK_THROW(parseUavPersonWindowIndex(profile, invalid), std::invalid_argument);
  }
  const auto limited = parseUavPersonTrackingProfile(replaceOne(std::string(PERSON_PROFILE_GOLDEN),
    "\"maxRetainedTransactionWindows\":120", "\"maxRetainedTransactionWindows\":2"));
  BOOST_CHECK_NO_THROW(requireUavPersonWindowCapacity(limited, 2));
  BOOST_CHECK_THROW(requireUavPersonWindowCapacity(limited, 3), std::invalid_argument);
  BOOST_CHECK_EQUAL(parseUavPersonWindowIndex(limited, "window-1"), 1);
  BOOST_CHECK_THROW(parseUavPersonWindowIndex(limited, "window-2"), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
