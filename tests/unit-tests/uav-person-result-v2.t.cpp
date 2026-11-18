#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"
#include "tests/fixtures/uav-tracking-job-config.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"
#include "NDNSF-UAV-APP/shared/uav-byte-track-state.hpp"
#include "NDNSF-UAV-APP/shared/uav-byte-tracker.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-backend.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-provider.hpp"

#include <onnxruntime_c_api.h>
#include <string>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using namespace person_fixture;

std::string
digest(char digit)
{
  return "sha256:" + std::string(64, digit);
}

struct V2Fixture
{
  UavPersonTrackingProfile profile =
    parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonWindowContext context = windowContext();
  UavPersonBackendWindow backend;
  UavPersonWindowResult result;
  UavTrackingResultExpectation expected;
  std::string name;
  std::string encoded;

  V2Fixture()
  {
    const uint64_t epochs[3] = {7, 3, 11};
    const uint64_t sequences[3] = {100, 63, 207};
    for (size_t i = 0; i < 3; ++i) {
      context.frames[i].sourceEpoch = epochs[i];
      context.frames[i].sequence = sequences[i];
      context.frames[i].commonPtsUs = static_cast<int64_t>(1000000 * i);
      context.frames[i].inputDigest = digest(static_cast<char>('a' + i));
    }
    context.windowId = "window-0";
    context.sequence = 1;
    backend = admitWindow(context, profile, {{{person(7)}, {person(7)}, {person(8)}}});
    UavPersonTrackingEngine engine(profile);
    result = engine.processWindow(context, backend);

    expected.runId = context.runId;
    expected.missionId = context.missionId;
    expected.windowId = context.windowId;
    expected.sequence = 1;
    expected.sourceEpoch = context.frames[0].sourceEpoch;
    expected.computeProvider = ndn::Name("/example/uav/compute");
    expected.personTracking =
      UavPersonTrackingResultExpectation{profile, context.computationDigest};
    std::array<std::string, 3> observationNames;
    std::array<std::string, 3> observationDigests;
    for (size_t i = 0; i < 3; ++i) {
      const auto& frame = context.frames[i];
      const auto producer = ndn::Name("/example/uav/drone").append(frame.cameraId);
      expected.sourceIds[i] = frame.sourceId;
      expected.inputDigests[i] = frame.inputDigest;
      expected.sourceProviders[i] = producer;
      expected.personFrames[i] = frame;
      observationNames[i] = makeUavObservationDataName(producer,
        frame.cameraId, frame.sourceEpoch, frame.sequence, "OBSERVATION", 1).toUri();
      observationDigests[i] = digest(static_cast<char>('c' + i));
      expected.windowBindings[i] = UavBindingObjectRef{
        makeUavWindowBindingName(producer, context.runId,
          "tracking-" + context.runId, context.windowId, "req-1").toUri(),
        digest(static_cast<char>('d' + i)), producer};
    }
    name = makeUavMultiViewResultName(expected.computeProvider, context.missionId,
      context.windowId, 1, 1).toUri();
    encoded = makeUavPersonTrackingResultJsonV2(result, backend, profile,
      observationNames, observationDigests, expected.sourceProviders,
      {*expected.windowBindings[0], *expected.windowBindings[1], *expected.windowBindings[2]});
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonResultV2)

BOOST_FIXTURE_TEST_CASE(NativeDescriptionBindsTheLoadedRuntime, tracking_fixture::ConfigFixture)
{
  const auto model = root / "native-model";
  const auto profilePath = root / "native-profile.json";
  std::ofstream(model) << "abc";
  auto profile = Json::parse(PERSON_PROFILE_GOLDEN);
  profile["detectorDigest"] = tracking_fixture::ABC_DIGEST;
  profile["reidDigest"] = tracking_fixture::ABC_DIGEST;
  std::ofstream(profilePath) << profile.dump();
  const auto described = Json::parse(describeUavPersonNativeComputation(model, profilePath, model));
  BOOST_CHECK_EQUAL(described.at("ortRuntime").get<std::string>(),
    "onnxruntime-" + std::string(OrtGetApiBase()->GetVersionString()));
  BOOST_CHECK(described.at("backendKind") == "native-onnx");
}

BOOST_AUTO_TEST_CASE(PerFrameEpochSequenceAndBindingAreAccepted)
{
  V2Fixture fixture;
  BOOST_CHECK(Json::parse(fixture.encoded).at("schema") ==
              "uav-person-tracking-result-v2");
  BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(fixture.name,
    "/example/uav/compute", fixture.encoded, fixture.expected));
}

BOOST_AUTO_TEST_CASE(WindowSequenceCannotReplaceFrameSequence)
{
  V2Fixture fixture;
  auto tampered = Json::parse(fixture.encoded);
  tampered["frames"][0]["sequence"] = 1;
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name,
    "/example/uav/compute", tampered.dump(), fixture.expected), std::exception);
}

BOOST_AUTO_TEST_CASE(WrongWindowBindingIsRejected)
{
  V2Fixture fixture;
  auto tampered = Json::parse(fixture.encoded);
  tampered["frames"][1]["windowBinding"]["digest"] = digest('9');
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name,
    "/example/uav/compute", tampered.dump(), fixture.expected), std::exception);
}

BOOST_AUTO_TEST_CASE(V2ExpectationRequiresPerFrameContexts)
{
  V2Fixture fixture;
  auto expectation = fixture.expected;
  expectation.personFrames[2].reset();
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name,
    "/example/uav/compute", fixture.encoded, expectation), std::exception);
}

BOOST_AUTO_TEST_CASE(LateBirthOwnerIsPresentInStateBeforeActivation)
{
  byte_track::UavByteTracker tracker;
  byte_track::Detection first;
  first.tlwh = {10, 10, 40, 80};
  first.score = 0.9;
  first.index = 0;
  tracker.update({first});
  byte_track::Detection second = first;
  second.tlwh = {100, 10, 40, 80};
  second.index = 1;
  const auto output = tracker.update({first, second});
  BOOST_CHECK_EQUAL(output.size(), 1);
  BOOST_REQUIRE_EQUAL(tracker.tracked().size(), 2);
  const auto state = byte_track::checkTracker(
    person_json::parseBounded(tracker.stateJson(), 8U * 1024U * 1024U), 512,
    byte_track::BackendStateFormat::COMPACT_CURRENT);
  BOOST_CHECK_EQUAL(state.currentIds.size(), tracker.tracked().size());
  bool newborn = false;
  for (const auto& track : tracker.tracked()) {
    if (track.index == 1) newborn = !track.isActivated;
  }
  BOOST_CHECK(newborn);
}

BOOST_AUTO_TEST_CASE(UnsupportedNativeDetectorPreprocessIsRejectedBeforeLoad)
{
  auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  profile.detectorPreprocessId = "yolo-letterbox1280-raw-box-v1";
  BOOST_CHECK_THROW(UavPersonModelBackend(profile, "/nonexistent/detector",
    "/nonexistent/reid", 1), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
