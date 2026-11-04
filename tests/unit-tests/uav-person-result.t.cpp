#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using namespace person_fixture;

struct ResultFixture
{
  UavPersonTrackingProfile profile;
  UavPersonWindowContext context = atWindow(1, 1000000);
  UavPersonBackendWindow backend;
  UavPersonWindowResult result;
  std::array<std::string, 3> observationNames;
  std::array<std::string, 3> observationDigests;
  UavTrackingResultExpectation expected;
  std::string name;
  std::string encoded;

  explicit ResultFixture(UavPersonTrackingProfile selectedProfile =
    parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN)))
    : profile(std::move(selectedProfile))
  {
    backend = admitWindow(context, profile, {{{person(7)}, {person(7)}, {person(8)}}});
    UavPersonTrackingEngine engine(profile);
    result = engine.processWindow(context, backend);
    expected.runId = "run";
    expected.missionId = "mission";
    expected.windowId = "window-1";
    expected.sequence = 1;
    expected.sourceEpoch = 1;
    expected.computeProvider = ndn::Name("/example/uav/compute");
    expected.personTracking =
      UavPersonTrackingResultExpectation{profile, context.computationDigest};
    for (size_t i = 0; i < 3; ++i) {
      const auto& frame = context.frames[i];
      observationNames[i] = makeUavObservationDataName(
        droneIdentity(frame.cameraId), frame.cameraId, 1, 1, "OBSERVATION", 1).toUri();
      observationDigests[i] = "sha256:" + std::string(64, static_cast<char>('c' + i));
      expected.sourceIds[i] = frame.sourceId;
      expected.inputDigests[i] = frame.inputDigest;
    }
    name = makeUavMultiViewResultName(
      expected.computeProvider, "mission", "window-1", 1, 1).toUri();
    encoded = makeUavPersonTrackingResultJson(result, backend, profile, observationNames,
                                             observationDigests);
  }

  void
  validate(const std::string& wire) const
  {
    validateUavTrackingResultContext(name, expected.computeProvider.toUri(), wire, expected);
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonResult)

BOOST_AUTO_TEST_CASE(ExplicitMatchOriginAndTargetAreBoundToAdmittedRows)
{
  const ResultFixture fixture;
  auto wire = Json::parse(fixture.encoded);
  const auto origin = wire["frames"][1]["tracks"][0]["associationOrigin"];
  BOOST_CHECK_EQUAL(origin["localKey"]["cameraId"].get<std::string>(), "UAV1");
  BOOST_CHECK_EQUAL(origin["localKey"]["byteTrackId"].get<uint64_t>(), 7);
  BOOST_CHECK_EQUAL(origin["ptsUs"].get<uint64_t>(), 1000000);
  BOOST_CHECK(origin["active"].get<bool>());
  BOOST_CHECK_EQUAL(wire["associationEvents"][1]["origin"].dump(), origin.dump());
  BOOST_CHECK_EQUAL(wire["associationEvents"][1]["targetLocalKey"]["cameraId"].get<std::string>(),
                    "UAV2");
  BOOST_CHECK_EQUAL(wire["associationEvents"][1]["ptsUs"].get<uint64_t>(), 1000000);
  BOOST_CHECK_NO_THROW(fixture.validate(wire.dump()));
  const auto original = wire;
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& x) { x = nullptr; },
    [] (auto& x) { x["localKey"]["cameraId"] = "UAV9"; },
    [] (auto& x) { x["localKey"]["sourceId"] = "another-source"; },
    [] (auto& x) { x["localKey"]["sourceEpoch"] = 2; },
    [] (auto& x) { x["localKey"]["trackerGeneration"] = 2; },
    [] (auto& x) { x["localKey"]["byteTrackId"] = 0; },
    [] (auto& x) { x["ptsUs"] = 1000001; },
    [] (auto& x) { x["active"] = "true"; },
    [] (auto& x) { x["ptsUs"] = 0; },
  };
  for (const auto& mutate : mutations) {
    wire = original;
    mutate(wire["frames"][1]["tracks"][0]["associationOrigin"]);
    // Keep the event mirror coherent so it cannot mask origin admission defects.
    wire["associationEvents"][1]["origin"] = wire["frames"][1]["tracks"][0]["associationOrigin"];
    BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  }
  wire = original;
  wire["frames"][0]["tracks"][0]["associationOrigin"] = origin;
  wire["associationEvents"][0]["origin"] = origin;
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  wire = original;
  wire["associationEvents"][1]["targetLocalKey"]["sourceEpoch"] = 2;
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  wire = original;
  auto& matched = wire["frames"][1]["tracks"][0];
  matched["associationOrigin"]["localKey"] = {
    {"cameraId", matched["cameraId"]}, {"sourceId", matched["sourceId"]},
    {"sourceEpoch", matched["sourceEpoch"]}, {"trackerGeneration", matched["trackerGeneration"]},
    {"byteTrackId", matched["byteTrackId"]}};
  matched["associationOrigin"]["active"] = false;
  wire["associationEvents"][1]["origin"] = matched["associationOrigin"];
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  for (const size_t index : {0, 1}) {
    wire = original;
    wire["frames"][index]["clockAvailable"] = false;
    BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
    wire = original;
    wire["frames"][index]["clockUncertaintyUs"] = 100001;
    BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  }
  wire = original;
  wire["frames"][0]["clockUncertaintyUs"] = 70000;
  wire["frames"][1]["clockUncertaintyUs"] = 40000;
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
  wire = original;
  const auto& nonoverlap = wire["frames"][2];
  wire["frames"][1]["tracks"][0]["associationOrigin"]["localKey"] = {
    {"cameraId", nonoverlap["cameraId"]}, {"sourceId", nonoverlap["sourceId"]},
    {"sourceEpoch", nonoverlap["sourceEpoch"]},
    {"trackerGeneration", nonoverlap["trackerGeneration"]}, {"byteTrackId", 8}};
  wire["associationEvents"][1]["origin"] = wire["frames"][1]["tracks"][0]["associationOrigin"];
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ActiveOriginCannotOutliveGalleryEvenWithinClockSkew)
{
  auto config = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
  config["limits"]["galleryTtlUs"] = 50000;
  config["limits"]["minTransitionUs"] = 0;
  config["limits"]["maxTransitionUs"] = 50000;
  const ResultFixture fixture(parseUavPersonTrackingProfile(config.dump()));
  BOOST_REQUIRE_EQUAL(fixture.profile.limits.at("maxClockSkewUs"), 100000);
  auto wire = Json::parse(fixture.encoded);
  auto& origin = wire["frames"][1]["tracks"][0]["associationOrigin"];
  BOOST_REQUIRE(origin["active"].get<bool>());
  // The receiver checks temporal eligibility; historical existence belongs to the producer.
  origin["ptsUs"] = 950000;
  wire["associationEvents"][1]["origin"] = origin;
  BOOST_CHECK_NO_THROW(fixture.validate(wire.dump()));
  origin["ptsUs"] = 949999;
  wire["associationEvents"][1]["origin"] = origin;
  BOOST_CHECK_THROW(fixture.validate(wire.dump()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeCompositionPreservesOriginalRefsAndExcludesPrivateVectors)
{
  const ResultFixture fixture;
  BOOST_CHECK_NO_THROW(fixture.validate(fixture.encoded));
  const auto wire = Json::parse(fixture.encoded);
  BOOST_CHECK_EQUAL(wire.at("schema").get<std::string>(), "uav-person-tracking-result-v1");
  BOOST_CHECK(wire.at("terminal").is_boolean());
  BOOST_CHECK(wire.at("terminal").get<bool>());
  BOOST_CHECK_EQUAL(wire.at("profileDigest").get<std::string>(),
                    std::string(PERSON_PROFILE_DIGEST));
  BOOST_CHECK_EQUAL(wire.at("frames").size(), 3);
  BOOST_CHECK_EQUAL(wire.at("associationEvents").size(), 3);
  BOOST_CHECK_EQUAL(wire["frames"][0]["tracks"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK_EQUAL(wire["frames"][1]["tracks"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK_EQUAL(wire["frames"][2]["tracks"][0]["globalId"].get<uint64_t>(), 2);
  for (size_t i = 0; i < 3; ++i) {
    const auto& frame = wire.at("frames")[i];
    BOOST_CHECK_EQUAL(frame.at("geometryStatus").get<std::string>(), "UNKNOWN");
    BOOST_CHECK_EQUAL(frame.at("observationName").get<std::string>(), fixture.observationNames[i]);
    BOOST_CHECK_EQUAL(frame["observation"]["observationRef"]["digest"].get<std::string>(),
                      fixture.observationDigests[i]);
    BOOST_CHECK_EQUAL(frame["observation"]["imageRef"]["digest"].get<std::string>(),
                      "sha256:" + std::string(64, 'a'));
  }
  BOOST_CHECK_EQUAL(wire["frames"][0]["observation"]["imageRef"]["producerIdentity"]
                      .get<std::string>(), "/example/uav/drone/UAV1");
  BOOST_CHECK_EQUAL(fixture.encoded.find("embedding"), std::string::npos);
  BOOST_CHECK_EQUAL(fixture.encoded.find("backendState"), std::string::npos);
  BOOST_CHECK_EQUAL(fixture.encoded.find("candidateStateJson"), std::string::npos);
  BOOST_CHECK_EQUAL(fixture.encoded.find("positionW"), std::string::npos);
  BOOST_CHECK_EQUAL(makeUavPersonTrackingResultJson(
    fixture.result, fixture.backend, fixture.profile, fixture.observationNames,
    fixture.observationDigests), fixture.encoded);
  // Declared CUDA fields in a fixture test admission; they do not prove GPU execution.
}

BOOST_AUTO_TEST_CASE(ProfileReidModeIsSerializedVerifiedAndBound)
{
  const ResultFixture gap;
  auto gemConfig = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
  gemConfig["preprocessId"] = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1";
  const auto gemProfile = parseUavPersonTrackingProfile(gemConfig.dump());
  const ResultFixture gem(gemProfile);

  for (const auto* fixture : {&gap, &gem}) {
    auto wire = Json::parse(fixture->encoded);
    BOOST_CHECK_EQUAL(wire["preprocessId"].get<std::string>(),
                      fixture->profile.reidPreprocessId);
    BOOST_CHECK_NO_THROW(fixture->validate(wire.dump()));
    wire["preprocessId"] = fixture == &gap ? gem.profile.reidPreprocessId :
                                              gap.profile.reidPreprocessId;
    BOOST_CHECK_THROW(fixture->validate(wire.dump()), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(RejectBodyForgeriesWithAnAcceptedIndependentBaseline)
{
  const ResultFixture fixture;
  BOOST_REQUIRE_NO_THROW(fixture.validate(fixture.encoded));
  const auto baseline = Json::parse(fixture.encoded);
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& w) { w["schema"] = "spec210-tracking-result-v2"; },
    [] (auto& w) { w["terminal"] = "true"; },
    [] (auto& w) { w["runId"] = "other"; },
    [] (auto& w) { w["sequence"] = true; },
    [] (auto& w) { w["computationDigest"] = "sha256:" + std::string(64, 'c'); },
    [] (auto& w) { w["profileDigest"] = "wrong"; },
    [] (auto& w) { w["detectorDigest"] = "wrong"; },
    [] (auto& w) { w["reidDigest"] = "wrong"; },
    [] (auto& w) { w["preprocessId"] = "other"; },
    [] (auto& w) { w["actualDetectorDevice"] = "cpu"; },
    [] (auto& w) { w["detectorModelDevice"] = "cpu"; },
    [] (auto& w) { w["reidInputDevice"] = "cpu"; },
    [] (auto& w) { w["reidInferenceBatches"] = 0; },
    [] (auto& w) { w["frames"][1]["tracks"][0]["appearanceDistance"] = 0.9;
                  w["associationEvents"][1]["distance"] = 0.9; },
    [] (auto& w) { w["reidInferenceBatches"] = 0;
                  w["actualReidDevice"] = "not-run";
                  w["reidInputDevice"] = "not-run";
                  for (const auto index : {0, 2}) {
                    w["frames"][index]["tracks"][0]["associationStatus"] = "NO_FEATURE";
                    w["associationEvents"][index]["status"] = "NO_FEATURE";
                  } },
    [] (auto& w) { w["limits"]["maxPersonsPerFrame"] = 64.0; },
    [] (auto& w) { w["counters"]["localKeyCount"] = 1025; },
    [] (auto& w) { w["counters"]["watermarkUs"] = 1000001; },
    [] (auto& w) { w["associationEvents"][0]["globalId"] = 999; },
    [] (auto& w) { w["frames"][0]["sourceEpoch"] = 2; },
    [] (auto& w) { w["frames"][0]["inputDigest"] = "wrong"; },
    [] (auto& w) { w["frames"][0]["width"] = 0; },
    [] (auto& w) { w["frames"][0]["clockAvailable"] = "true"; },
    [] (auto& w) { w["frames"][0]["observationName"] = "/another/producer"; },
    [] (auto& w) { w["frames"][0]["observation"]["imageRef"]["digest"] = "wrong"; },
    [] (auto& w) { w["frames"][0]["observation"]["observationRef"]["producerIdentity"] =
                    "/example/uav/compute"; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["class"] = "car"; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["localId"] = 7.0; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["globalId"] = 0; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["confidence"] = -0.1; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["box"] = {0, 0, 641, 480}; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["ptsUs"] = 1.0; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["appearanceDistance"] = 0.1; },
    [] (auto& w) { w["frames"][0]["tracks"][0]["embedding"] = {1.0}; },
    [] (auto& w) { auto row = w["frames"][0]["tracks"][0];
                  w["frames"][0]["tracks"].push_back(row); },
    [] (auto& w) { w["frames"][0]["lostByteTrackIds"] = {7}; },
    [] (auto& w) { std::swap(w["frames"][0], w["frames"][1]); },
    [] (auto& w) { w["frames"].erase(2); },
  };
  for (size_t i = 0; i < mutations.size(); ++i) {
    auto malformed = baseline;
    mutations[i](malformed);
    BOOST_TEST_CONTEXT("person result mutation " << i) {
      BOOST_CHECK_THROW(fixture.validate(malformed.dump()), std::exception);
    }
  }
  BOOST_CHECK_THROW(fixture.validate("{\"terminal\":true," + fixture.encoded.substr(1)),
                    std::invalid_argument);
  BOOST_CHECK_THROW(fixture.validate(std::string(4194305, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ExplicitModeSelectedProducerAndImmutableResultNameAreRequired)
{
  const ResultFixture fixture;
  auto legacy = fixture.expected;
  legacy.personTracking.reset();
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name,
    fixture.expected.computeProvider.toUri(), fixture.encoded, legacy), std::runtime_error);
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name, "/example/uav/other",
    fixture.encoded, fixture.expected), std::runtime_error);
  const auto wrongName = makeUavMultiViewResultName(ndn::Name("/example/uav/ground-station"),
    "mission", "window-1", 1, 1).toUri();
  BOOST_CHECK_THROW(validateUavTrackingResultContext(wrongName,
    fixture.expected.computeProvider.toUri(), fixture.encoded, fixture.expected),
    std::runtime_error);
  const auto oldAttempt = makeUavMultiViewResultName(fixture.expected.computeProvider,
    "mission", "window-1", 1, 2).toUri();
  BOOST_CHECK_THROW(validateUavTrackingResultContext(oldAttempt,
    fixture.expected.computeProvider.toUri(), fixture.encoded, fixture.expected),
    std::runtime_error);
  auto changed = fixture.expected;
  changed.personTracking->computationDigest = "sha256:" + std::string(64, 'd');
  BOOST_CHECK_THROW(validateUavTrackingResultContext(fixture.name,
    fixture.expected.computeProvider.toUri(), fixture.encoded, changed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(BuilderRejectsMismatchedNativeContextAndInvalidDecisionValues)
{
  const ResultFixture fixture;
  auto backend = fixture.backend;
  ++backend.context.frames[0].commonPtsUs;
  BOOST_CHECK_THROW(makeUavPersonTrackingResultJson(fixture.result, backend, fixture.profile,
    fixture.observationNames, fixture.observationDigests), std::invalid_argument);
  auto result = fixture.result;
  result.tracks[0][0].distance = std::numeric_limits<double>::quiet_NaN();
  BOOST_CHECK_THROW(makeUavPersonTrackingResultJson(result, fixture.backend, fixture.profile,
    fixture.observationNames, fixture.observationDigests), std::invalid_argument);
  result = fixture.result;
  result.tracks[0][0].confidence = std::numeric_limits<double>::quiet_NaN();
  BOOST_CHECK_THROW(makeUavPersonTrackingResultJson(result, fixture.backend, fixture.profile,
    fixture.observationNames, fixture.observationDigests), std::invalid_argument);
  auto names = fixture.observationNames;
  names[0] = fixture.observationNames[1];
  BOOST_CHECK_THROW(makeUavPersonTrackingResultJson(
    fixture.result, fixture.backend, fixture.profile, names, fixture.observationDigests),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ZeroBatchNoFeatureAndContinuedRowsRemainValid)
{
  const ResultFixture fixture;
  UavPersonTrackingEngine engine(fixture.profile);
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    const auto context = atWindow(sequence, 500000 + static_cast<int64_t>(sequence) * 500000);
    auto backend = admitWindow(context, fixture.profile, {{{person(7, 1.0, 0.0, false)}, {}, {}}});
    backend.reidInferenceBatches = 0;
    backend.actualReidDevice = "not-run";
    backend.reidInputDevice = "not-run";
    const auto result = engine.processWindow(context, backend);
    auto names = fixture.observationNames;
    for (size_t i = 0; i < 3; ++i) {
      const auto& frame = context.frames[i];
      names[i] = makeUavObservationDataName(droneIdentity(frame.cameraId), frame.cameraId,
        1, sequence, "OBSERVATION", 1).toUri();
    }
    const auto encoded = makeUavPersonTrackingResultJson(result, backend, fixture.profile,
      names, fixture.observationDigests);
    const auto wire = Json::parse(encoded);
    BOOST_CHECK_EQUAL(wire["actualReidDevice"].get<std::string>(), "not-run");
    BOOST_CHECK_EQUAL(wire["frames"][0]["tracks"][0]["associationStatus"].get<std::string>(),
                      sequence == 1 ? "NO_FEATURE" : "CONTINUED");
    BOOST_CHECK_EQUAL(wire["frames"][0]["tracks"][0]["globalId"].get<uint64_t>(), 1);
    auto expected = fixture.expected;
    expected.windowId = context.windowId;
    expected.sequence = sequence;
    const auto name = makeUavMultiViewResultName(expected.computeProvider, "mission",
      context.windowId, sequence, 1).toUri();
    BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(name, expected.computeProvider.toUri(),
                                                         encoded, expected));
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
