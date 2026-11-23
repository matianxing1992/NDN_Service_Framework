#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"
#include "tests/fixtures/uav-person-tracker-golden.hpp"
#include "tests/fixtures/uav-tracking-job-config.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"
#include "NDNSF-UAV-APP/shared/uav-byte-track-state.hpp"
#include "NDNSF-UAV-APP/shared/uav-byte-tracker.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-backend.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-provider.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/string-helper.hpp>

#include <algorithm>
#include <cmath>
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
  profile["requestedDevice"] = "cpu";
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
  profile.requestedDevice = "cpu";
  profile.detectorPreprocessId = "yolo-letterbox1280-raw-box-v1";
  BOOST_CHECK_EXCEPTION(UavPersonModelBackend(profile, "/nonexistent/detector",
    "/nonexistent/reid", 1), std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "unsupported native person detector preprocessing";
    });
  profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  profile.requestedDevice = "cuda:0";
  BOOST_CHECK_EXCEPTION(UavPersonModelBackend(profile, "/nonexistent/detector",
    "/nonexistent/reid", 1), std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()).find("CPU only") != std::string::npos;
    });
  profile.requestedDevice = "cpu";
  profile.reidPreprocessId = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1";
  BOOST_CHECK_EXCEPTION(UavPersonModelBackend(profile, "/nonexistent/detector",
    "/nonexistent/reid", 1), std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()).find("GAP ReID") != std::string::npos;
    });
}

byte_track::Detection
stockDetection(double left, double top, double width, double height, float score, size_t index)
{
  byte_track::Detection value;
  value.tlwh = {left, top, width, height};
  value.score = score;
  value.index = static_cast<double>(index);
  return value;
}

void
checkStockValue(const Json& actual, const Json& expected, double tolerance = 1e-4)
{
  if (expected.is_object()) {
    BOOST_REQUIRE(actual.is_object());
    BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
    for (const auto& item : expected.items()) {
      BOOST_REQUIRE(actual.contains(item.key()));
      BOOST_TEST_CONTEXT(item.key()) { checkStockValue(actual.at(item.key()), item.value(), tolerance); }
    }
  }
  else if (expected.is_array()) {
    BOOST_REQUIRE(actual.is_array());
    BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      BOOST_TEST_CONTEXT(i) { checkStockValue(actual.at(i), expected.at(i), tolerance); }
    }
  }
  else if (expected.is_number_float()) {
    BOOST_REQUIRE(actual.is_number());
    const double value = actual.get<double>();
    BOOST_REQUIRE(std::isfinite(value));
    BOOST_CHECK_SMALL(value - expected.get<double>(), tolerance);
  }
  else {
    BOOST_CHECK(actual == expected);
  }
}

Json
stockDiscrete(const Json& state)
{
  Json result = Json::array({state.at("frameId"), state.at("idHighWatermark"),
                             state.at("removedIds")});
  for (const auto* group : {"tracked", "lost"}) {
    Json rows = Json::array();
    for (const auto& track : state.at(group)) {
      Json row = Json::array();
      for (const auto* key : {"trackId", "state", "isActivated", "frameId", "startFrame",
                              "timeSinceUpdate", "trackletLength"}) {
        row.push_back(track.at(key));
      }
      rows.push_back(std::move(row));
    }
    result.push_back(std::move(rows));
  }
  return result;
}

BOOST_AUTO_TEST_CASE(StockCrossingOcclusionExpiryAndLowConfidenceSurviveNativeRestore)
{
  const auto golden = Json::parse(STOCK_TRACKER_GOLDEN);
  using Inputs = std::vector<std::vector<byte_track::Detection>>;
  Inputs crossing;
  for (size_t step = 0; step < 31; ++step) {
    std::vector<byte_track::Detection> row{
      stockDetection(40 + 4 * step, 50, 40, 100, 0.9F, 0),
      stockDetection(160 - 4 * step, 74, 40, 100, 0.9F, 1)};
    if (step % 2 != 0) std::reverse(row.begin(), row.end());
    for (size_t i = 0; i < row.size(); ++i) row[i].index = static_cast<double>(i);
    crossing.push_back(std::move(row));
  }
  Inputs occlusion;
  for (size_t step = 0; step < 5; ++step) {
    occlusion.push_back(step == 2 || step == 3 ? std::vector<byte_track::Detection>{} :
      std::vector<byte_track::Detection>{stockDetection(10 + step, 20, 40, 100, 0.9F, 0)});
  }
  const std::vector<byte_track::Detection> detected{stockDetection(10, 20, 40, 100, 0.9F, 0)};
  const Inputs expiry{detected, {}, {}, {}, detected, detected};
  const Inputs low{{stockDetection(80, 110, 40, 100, 0.9F, 0)},
    {stockDetection(82, 110, 40, 100, 0.2F, 0),
     stockDetection(380, 110, 40, 100, 0.2F, 1),
     stockDetection(480, 110, 40, 100, 0.05F, 2)}};
  const Inputs mixed{
    {stockDetection(10, 20, 40, 100, 0.9F, 0), stockDetection(110, 20, 40, 100, 0.9F, 1)},
    {stockDetection(11, 20, 40, 100, 0.9F, 0)},
    {stockDetection(12, 20, 40, 100, 0.9F, 0), stockDetection(112, 20, 40, 100, 0.9F, 1),
     stockDetection(230, 20, 40, 100, 0.9F, 2)},
    {stockDetection(13, 20, 40, 100, 0.9F, 0), stockDetection(113, 20, 40, 100, 0.9F, 1),
     stockDetection(231, 20, 40, 100, 0.9F, 2)}};
  const Inputs lossOrder{mixed[0], mixed[1], {},
    {stockDetection(13, 20, 40, 100, 0.9F, 0), stockDetection(113, 20, 40, 100, 0.9F, 1)}};
  const Inputs unconfirmed{
    {stockDetection(10, 20, 40, 100, 0.9F, 0)},
    {stockDetection(10, 20, 40, 100, 0.9F, 0), stockDetection(110, 20, 40, 100, 0.9F, 1)},
    {stockDetection(10, 20, 40, 100, 0.9F, 0), stockDetection(131, 20, 40, 100, 0.9F, 1)},
    {stockDetection(10, 20, 40, 100, 0.9F, 0), stockDetection(132, 20, 40, 100, 0.9F, 1)}};
  const Inputs costLimit{
    {stockDetection(100, 20, 100, 100, 0.9F, 0), stockDetection(130, 20, 100, 100, 0.9F, 1)},
    {stockDetection(114, 20, 100, 100, 0.9F, 0), stockDetection(25, 20, 100, 100, 0.9F, 1)}};
  const Inputs optionalMatch{
    {stockDetection(20, 20, 100, 100, 0.9F, 0), stockDetection(10, 20, 55, 100, 0.9F, 1)},
    {stockDetection(10, 20, 110, 100, 0.9F, 0), stockDetection(70, 20, 50, 100, 0.9F, 1)}};
  const Inputs firstBoundary{{stockDetection(0, 0, 100, 100, 0.9F, 0)},
                             {stockDetection(0, 0, 40, 100, 0.5F, 0)}};
  const Inputs secondBoundary{{stockDetection(0, 0, 100, 100, 0.9F, 0)},
                              {stockDetection(0, 0, 50, 100, 0.2F, 0)}};
  const std::array<std::pair<const char*, const Inputs*>, 11> scenarios{{
    {"crossing", &crossing}, {"occlusion", &occlusion}, {"expiry", &expiry}, {"low", &low},
    {"mixed", &mixed}, {"lossOrder", &lossOrder}, {"unconfirmed", &unconfirmed},
    {"costLimit", &costLimit}, {"optionalMatch", &optionalMatch},
    {"firstBoundary", &firstBoundary}, {"secondBoundary", &secondBoundary}}};
  for (const auto& [name, inputs] : scenarios) {
    byte_track::TrackerConfig config;
    if (std::string(name) == "expiry") config.trackBuffer = 1;
    byte_track::UavByteTracker continuous(config);
    std::string previous;
    BOOST_REQUIRE_EQUAL(golden.at(name).size(), inputs->size());
    for (size_t step = 0; step < inputs->size(); ++step) {
      BOOST_TEST_CONTEXT(name << " frame=" << step + 1) {
        continuous.update(inputs->at(step));
        const auto expected = golden.at(name).at(step);
        checkStockValue(Json::parse(continuous.stateJson()), expected);
        byte_track::UavByteTracker resumed(config);
        if (!previous.empty()) resumed.restoreState(previous);
        const auto output = resumed.update(inputs->at(step));
        checkStockValue(Json::parse(resumed.stateJson()), expected);
        BOOST_CHECK_EQUAL(resumed.stateJson(), continuous.stateJson());
        Json ids = Json::array();
        Json expectedIds = Json::array();
        for (const auto& row : output) ids.push_back(row.trackId);
        for (const auto& row : expected.at("tracked")) {
          if (row.at("isActivated").get<bool>()) expectedIds.push_back(row.at("trackId"));
        }
        BOOST_CHECK(ids == expectedIds);
        Json geometry = Json::array();
        for (const auto& track : output) {
          const auto box = track.tlwh();
          geometry.push_back(Json::array({static_cast<float>(box[0]),
            static_cast<float>(box[1]), static_cast<float>(box[0] + box[2]),
            static_cast<float>(box[1] + box[3]), static_cast<float>(track.trackId),
            static_cast<float>(track.score), static_cast<float>(track.cls),
            static_cast<float>(track.index)}));
        }
        checkStockValue(geometry, golden.at("outputs").at(name).at(step),
                        std::string(name) == "expiry" ? 1e-6 : 1e-4);
        previous = continuous.stateJson();
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(StockLongRemovalFifoMatchesIndependentTraceAndLiveRestore)
{
  const auto golden = Json::parse(STOCK_TRACKER_GOLDEN);
  byte_track::TrackerConfig config;
  config.trackBuffer = 1;
  byte_track::UavByteTracker continuous(config), resumed(config);
  ndn::util::Sha256 trace;
  std::string previous;
  for (size_t step = 0; step < 1006 * 5; ++step) {
    const std::vector<byte_track::Detection> inputs = step % 5 < 2
      ? std::vector<byte_track::Detection>{stockDetection(100, 100, 40, 100, 0.9F, 0)}
      : std::vector<byte_track::Detection>{};
    continuous.update(inputs);
    if (!previous.empty()) resumed.restoreState(previous);
    resumed.update(inputs);
    const auto serialized = continuous.stateJson();
    BOOST_REQUIRE_EQUAL(resumed.stateJson(), serialized);
    const auto state = Json::parse(serialized);
    trace << (stockDiscrete(state).dump() + "\n");
    const auto& prototype = golden.at("fifoPrototypes").at(step < 5 ? step : 5 + step % 5);
    for (const auto* group : {"tracked", "lost"}) {
      BOOST_REQUIRE_EQUAL(state.at(group).size(), prototype.at(group).size());
      for (size_t i = 0; i < state.at(group).size(); ++i) {
        for (const auto* key : {"tlwh", "mean", "covariance", "score", "class", "index", "angle"}) {
          checkStockValue(state.at(group).at(i).at(key), prototype.at(group).at(i).at(key));
        }
      }
    }
    previous = serialized;
  }
  BOOST_CHECK_EQUAL(ndn::toHex(*trace.computeDigest(), false),
                    golden.at("fifoTraceSha256").get<std::string>());
  checkStockValue(Json::parse(previous), golden.at("fifoFinal"));
  BOOST_CHECK_EQUAL(continuous.idHighWatermark(), 1006);
}

BOOST_AUTO_TEST_CASE(NativeAllocatorOutlivesFifoAndRejectsRestoreWithoutMutation)
{
  const auto golden = Json::parse(STOCK_TRACKER_GOLDEN);
  byte_track::TrackerConfig config;
  config.trackBuffer = 1;
  byte_track::UavByteTracker tracker(config);
  std::vector<byte_track::Detection> rows;
  for (size_t i = 0; i < 512; ++i) {
    rows.push_back(stockDetection(10 * i, 10, 8, 80, 0.9F, i));
  }
  tracker.update(rows);
  const auto first = rows.front();
  rows.pop_back();
  for (size_t i = 0; i < 3; ++i) tracker.update(rows);
  for (size_t i = 0; i < 3; ++i) tracker.update({});
  const auto predecessor = tracker.stateJson();
  checkStockValue(Json::parse(predecessor), golden.at("allocatorPredecessor"));
  BOOST_CHECK_EQUAL(tracker.idHighWatermark(), 512);
  std::vector<Json> invalid;
  for (const auto& high : {Json(true), Json(-1), Json(1.5), Json(16777215),
                            Json(uint64_t{1} << 63), Json(0)}) {
    auto state = Json::parse(predecessor);
    state["idHighWatermark"] = high;
    invalid.push_back(std::move(state));
  }
  for (const auto& removed : {Json::array({0}), Json::array({-1}), Json::array({true}),
                              Json::array({1.5}), Json::array({513}), Json("not-a-list")}) {
    auto state = Json::parse(predecessor);
    state["removedIds"] = removed;
    invalid.push_back(std::move(state));
  }
  auto tooLong = Json::parse(predecessor);
  tooLong["removedIds"] = std::vector<unsigned>(1001, 1);
  invalid.push_back(std::move(tooLong));
  auto badFrame = Json::parse(predecessor);
  badFrame["frameId"] = true;
  invalid.push_back(std::move(badFrame));
  auto oldHistory = Json::parse(predecessor);
  oldHistory["removed"] = Json::array();
  invalid.push_back(std::move(oldHistory));
  for (const auto& state : invalid) {
    BOOST_CHECK_THROW(tracker.restoreState(state.dump()), std::invalid_argument);
    BOOST_CHECK_EQUAL(tracker.stateJson(), predecessor);
  }
  tracker.restoreState(predecessor);
  tracker.update({first});
  const auto output = tracker.update({first});
  BOOST_REQUIRE_EQUAL(output.size(), 1);
  BOOST_CHECK_EQUAL(output.front().trackId, 513);
  checkStockValue(Json::parse(tracker.stateJson()), golden.at("allocatorFinal"));
}

BOOST_AUTO_TEST_CASE(NativeAllocatorsAreCameraLocalAndReplayableInEveryOrder)
{
  std::array<std::string, 3> predecessors;
  std::array<std::vector<byte_track::Detection>, 3> inputs;
  for (size_t camera = 0; camera < 3; ++camera) {
    byte_track::UavByteTracker tracker;
    for (size_t i = 0; i <= camera; ++i) {
      inputs[camera].push_back(stockDetection(60 + 150 * i, 110, 40, 100, 0.9F, i));
    }
    tracker.update(inputs[camera]);
    predecessors[camera] = tracker.stateJson();
    inputs[camera].push_back(stockDetection(60 + 150 * (camera + 1), 110, 40, 100, 0.9F,
                                             camera + 1));
  }
  std::array<size_t, 3> order{0, 1, 2};
  std::array<std::string, 3> expected;
  byte_track::UavByteTracker reused;
  do {
    for (const auto camera : order) {
      reused.restoreState(predecessors[camera]);
      reused.update(inputs[camera]);
      BOOST_CHECK_EQUAL(reused.idHighWatermark(), camera + 2);
      if (expected[camera].empty()) expected[camera] = reused.stateJson();
      BOOST_CHECK_EQUAL(reused.stateJson(), expected[camera]);
    }
  } while (std::next_permutation(order.begin(), order.end()));
  auto exhausted = Json::parse(predecessors[0]);
  exhausted["idHighWatermark"] = 16777214;
  reused.restoreState(exhausted.dump());
  reused.update(inputs[0]);
  BOOST_CHECK_THROW(byte_track::checkTracker(Json::parse(reused.stateJson()), 512,
    byte_track::BackendStateFormat::COMPACT_CURRENT), std::invalid_argument);
  reused.restoreState(predecessors[0]);
  reused.update(inputs[0]);
  BOOST_CHECK_EQUAL(reused.stateJson(), expected[0]);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
