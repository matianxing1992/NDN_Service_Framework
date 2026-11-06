#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"

#include "NDNSF-UAV-APP/tracking/uav-vehicle-tracking-engine.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using person_json::Json;

std::string
digest(char value)
{
  return "sha256:" + std::string(64, value);
}

VehicleWindowContext
window(uint64_t sequence = 1, double cameraEast = 0)
{
  VehicleWindowContext context;
  context.runId = "vehicle-run";
  context.missionId = "tracking-vehicle-run";
  context.windowId = "window-" + std::to_string(sequence - 1);
  context.nativeDecisionDigest = digest('a');
  context.sequence = sequence;
  for (size_t i = 0; i < context.frames.size(); ++i) {
    auto& frame = context.frames[i];
    frame.cameraId = "UAV" + std::to_string(i + 1);
    frame.sourceId = "source-" + frame.cameraId;
    frame.sourceEpoch = 7;
    frame.sequence = sequence;
    frame.commonPtsUs = static_cast<int64_t>((sequence - 1) * 500000);
    frame.width = frame.height = 100;
    frame.inputDigest = digest('b');
    frame.imageName = "/source/" + frame.cameraId + "/FRAME/7/" + std::to_string(sequence);
    frame.observationName = "/source/" + frame.cameraId + "/OBSERVATION/7/" + std::to_string(sequence);
    frame.observationDigest = digest('c');
    frame.observation = {
      {"schema", "uav-observation-v2"},
      {"identity", {{"sourceId", frame.sourceId}, {"droneId", frame.cameraId},
                    {"cameraId", frame.cameraId}, {"epoch", frame.sourceEpoch},
                    {"worldFrameId", "ENU-local"}, {"acquisitionKind", "simulated"}, {"deliveryMode", "replay"}}},
      {"key", {{"sourceId", frame.sourceId}, {"cameraId", frame.cameraId},
               {"epoch", frame.sourceEpoch}, {"sequence", sequence}}},
      {"captureTimeUs", frame.commonPtsUs}, {"timeUncertaintyUs", uint64_t(0)},
      {"width", 100}, {"height", 100},
      {"clockBinding", {{"clockId", "clock-" + frame.cameraId}, {"clockEpoch", uint64_t(7)},
                        {"revision", uint64_t(1)}, {"a", 1.0}, {"bUs", 0.0}}},
      {"calibration", {{"revision", uint64_t(1)}, {"fx", 100.0}, {"fy", 100.0},
                       {"cx", 50.0}, {"cy", 50.0}, {"distortion", std::vector<double>(5, 0)},
                       {"imageTransform", "raw"}, {"parameterCovariance", std::vector<double>(81, 0)}}},
      {"ground", {{"normal", {0.0, 0.0, 1.0}}, {"offsetM", 0.0},
                  {"roiVertices", {{-100.0, -100.0, 0.0}, {100.0, -100.0, 0.0},
                                   {100.0, 100.0, 0.0}, {-100.0, 100.0, 0.0}}},
                  {"parameterCovariance", std::vector<double>(16, 0)}}},
      {"poseStatus", "aligned"}, {"positionW", {cameraEast, 0.0, 10.0}},
      {"qWC", {0.0, 1.0, 0.0, 0.0}}, {"poseCovariance", std::vector<double>(36, 0)},
      {"anchorSigmaPixels", 0.0}, {"geometryValidity", "analytic-fixture"}, {"reason", ""}};
  }
  return context;
}

Json
candidate(const VehicleWindowContext& context, int anchorEast = 50)
{
  Json frames = Json::array();
  Json cameras = Json::object();
  for (const auto& frame : context.frames) {
    frames.push_back({{"cameraId", frame.cameraId}, {"width", 100}, {"height", 100},
                      {"detections", Json::array({{{"box", {anchorEast - 5, 40, anchorEast + 5, 50}},
                                                  {"confidence", 0.9}, {"class", "car"},
                                                  {"byteTrackId", nullptr}}})}});
    cameras[frame.cameraId] = nullptr;
  }
  return {{"schema", "uav-vehicle-backend-window-v1"}, {"runId", context.runId},
          {"missionId", context.missionId}, {"windowId", context.windowId},
          {"sequence", context.sequence}, {"actualDevice", "cpu"}, {"frames", frames},
          {"backendState", {{"schema", "uav-vehicle-backend-state-v1"}, {"cameras", cameras}}}};
}

VehicleWindowContext
legacyWindow(uint64_t sequence, bool specialNumbers = false)
{
  auto context = window(sequence);
  context.observationMode = false;
  Json telemetry = Json::array();
  Json calibrations = Json::array();
  for (const auto& frame : context.frames) {
    telemetry.push_back({{"droneId", frame.cameraId}, {"cameraId", frame.cameraId},
                         {"sourceEpoch", frame.sourceEpoch}, {"sequence", frame.sequence},
                         {"sourceTimestampUs", frame.commonPtsUs}, {"latitudeE7", 0},
                         {"longitudeE7", static_cast<int64_t>((sequence - 1) * 10)},
                         {"altitudeMm", 10000}, {"headingCdeg", 0}, {"groundSpeedMmps", 0},
                         {"freshness", "fresh"}, {"provenance", "simulated-input"}});
    calibrations.push_back({{"cameraId", frame.cameraId}, {"imageWidth", 100}, {"imageHeight", 100},
                            {"coordinateFrame", "ground-mm-ned"}, {"distortion", "ignored-after-calibration"},
                            {"homography", {{5.0, specialNumbers ? -0.0 : 0.0, specialNumbers ? 1e-7 : 0.0},
                                            {0.0, 5.0, specialNumbers ? 1e15 : 0.0}, {0.0, 0.0, 1.0}}}});
  }
  context.legacyMotion = Json::parse(Json{{"schema", "spec191-motion-window-v1"},
    {"telemetry", telemetry}, {"calibrations", calibrations}, {"oracle", Json::object()}}.dump());
  return context;
}

VehicleWindowOutput
process(const VehicleWindowContext& context, const Json& backend, const std::string& state = {})
{
  UavVehicleTrackingEngine engine;
  engine.restoreState(state, context);
  return engine.processWindow(context, parseUavVehicleBackendWindow(backend.dump(), context));
}

Json
threeSamples(bool movingCamera, bool covariance = true, bool stationary = false)
{
  std::string state;
  Json result;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    const double seconds = (sequence - 1) * 0.5;
    auto context = window(sequence, movingCamera ? seconds : 0);
    if (!covariance) {
      for (auto& frame : context.frames) frame.observation["poseCovariance"] = nullptr;
    }
    const auto relativeEast = (stationary ? 0.0 : 2 * seconds) - (movingCamera ? seconds : 0.0);
    const auto anchor = static_cast<int>(50 + relativeEast * 10);
    const auto output = process(context, candidate(context, anchor), state);
    state = output.candidateStateJson;
    result = Json::parse(output.resultJson);
  }
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavVehicleTrackingEngineTests)

BOOST_AUTO_TEST_CASE(KnownTenMetreCameraProjectsTwoMetresAndEastwardTwoMetresPerSecond)
{
  const auto result = threeSamples(false);
  const auto& measurement = result.at("frames").at(0).at("tracks").at(0).at("measurement");
  BOOST_CHECK_CLOSE(measurement.at("horizontalRangeM").get<double>(), 2.0, 1e-8);
  BOOST_CHECK_CLOSE(measurement.at("slantRangeM").get<double>(), std::sqrt(104.0), 1e-8);
  BOOST_CHECK_CLOSE(measurement.at("speedMps").get<double>(), 2.0, 1e-8);
  BOOST_CHECK_CLOSE(measurement.at("courseDeg").get<double>(), 90.0, 1e-8);
  BOOST_CHECK_EQUAL(measurement.at("estimateTimeUs").get<int64_t>(), 500000);
  BOOST_CHECK_EQUAL(measurement.at("speedStatus"), "valid");
  BOOST_CHECK_EQUAL(measurement.at("bearingStatus"), "valid");
  BOOST_CHECK(!measurement.at("imageArrow").is_null());
  BOOST_CHECK_EQUAL(result.at("decisionSchema"), "uav-vehicle-native-decision-v1");
}

BOOST_AUTO_TEST_CASE(MovingCameraDoesNotSubtractEgoMotionTwice)
{
  const auto result = threeSamples(true);
  const auto& measurement = result.at("motion").at("measurements").at(0);
  BOOST_CHECK_CLOSE(measurement.at("speedMps").get<double>(), 2.0, 1e-8);
  BOOST_CHECK_CLOSE(measurement.at("courseDeg").get<double>(), 90.0, 1e-8);
  BOOST_CHECK_CLOSE(measurement.at("horizontalRangeM").get<double>(), 1.0, 1e-8);
}

BOOST_AUTO_TEST_CASE(StationaryObjectHasNoDirection)
{
  const auto result = threeSamples(false, true, true);
  const auto& measurement = result.at("motion").at("measurements").at(0);
  BOOST_CHECK_SMALL(measurement.at("speedMps").get<double>(), 1e-8);
  BOOST_CHECK_EQUAL(measurement.at("quality"), "stationary");
  BOOST_CHECK(measurement.at("courseDeg").is_null());
  BOOST_CHECK(measurement.at("imageArrow").is_null());
}

BOOST_AUTO_TEST_CASE(MissingCovariancePreservesSpeedAndMarksBearingUnknown)
{
  const auto result = threeSamples(false, false);
  const auto& measurement = result.at("motion").at("measurements").at(0);
  BOOST_CHECK_CLOSE(measurement.at("speedMps").get<double>(), 2.0, 1e-8);
  BOOST_CHECK_EQUAL(measurement.at("speedStatus"), "valid");
  BOOST_CHECK_EQUAL(measurement.at("bearingStatus"), "unknown");
  BOOST_CHECK(measurement.at("sigmaSpeedMps").is_null());
  BOOST_CHECK(measurement.at("courseDeg").is_null());
}

BOOST_AUTO_TEST_CASE(IndependentPixelNoiseProducesKnownVelocityAndBearingUncertainty)
{
  std::string state;
  Json result;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    auto context = window(sequence);
    for (auto& frame : context.frames) {
      frame.observation["anchorSigmaPixels"] = 1.0;
      frame.observation["calibration"]["anchorSigmaPixels"] = 1.0;
    }
    const auto output = process(context, candidate(context, 50 + static_cast<int>((sequence - 1) * 10)), state);
    state = output.candidateStateJson;
    result = Json::parse(output.resultJson);
  }
  const auto& measurement = result["motion"]["measurements"][0];
  // 10 m / 100 px gives 0.1 m noise. The independent LS weights are -1,0,+1 /s.
  BOOST_CHECK_CLOSE(measurement["sigmaSpeedMps"].get<double>(), std::sqrt(0.02), 1e-5);
  BOOST_CHECK_CLOSE(measurement["sigmaCourseDeg"].get<double>(),
                    std::sqrt(0.02) / 2 * 180 / 3.14159265358979323846, 1e-5);
  BOOST_CHECK_CLOSE(measurement["sigmaRangeM"].get<double>(), 0.2 / std::sqrt(104.0), 1e-5);
  BOOST_CHECK_EQUAL(measurement["bearingStatus"], "valid");
}

BOOST_AUTO_TEST_CASE(TimeUncertaintyWithoutPoseCovarianceCannotProduceRange)
{
  auto context = window();
  for (auto& frame : context.frames) {
    frame.observation["poseCovariance"] = nullptr;
    frame.observation["timeUncertaintyUs"] = uint64_t(1);
  }
  const auto output = process(context, candidate(context));
  const auto measurement = Json::parse(output.resultJson).at("motion").at("measurements").at(0);
  BOOST_CHECK_EQUAL(measurement.at("rangeStatus"), "unknown");
  BOOST_CHECK_EQUAL(measurement.at("reasonCodes").at(0), "TIME_UNCERTAINTY_UNBOUNDED");
}

BOOST_AUTO_TEST_CASE(HorizonRayCannotInventGroundIntersection)
{
  auto context = window();
  for (auto& frame : context.frames) {
    frame.observation["qWC"] = {std::sqrt(0.5), 0.0, std::sqrt(0.5), 0.0};
  }
  const auto output = process(context, candidate(context));
  const auto measurement = Json::parse(output.resultJson).at("motion").at("measurements").at(0);
  BOOST_CHECK_EQUAL(measurement.at("reasonCodes").at(0), "RAY_NEAR_HORIZON");
  BOOST_CHECK(measurement.at("slantRangeM").is_null());
}

BOOST_AUTO_TEST_CASE(ChangedEpochClearsHistoryBeforeBackendExport)
{
  const auto first = window();
  auto initial = candidate(first);
  initial["frames"][0]["detections"][0]["byteTrackId"] = uint64_t(9);
  initial["backendState"]["cameras"]["UAV1"] = {
    {"frameId", uint64_t(1)}, {"tracked", Json::array({person_fixture::stockTrack(9)})},
    {"lost", Json::array()}, {"removed", Json::array()}};
  const auto output = process(first, initial);
  BOOST_REQUIRE(!Json::parse(output.candidateStateJson)["backendState"]["cameras"]["UAV1"].is_null());
  BOOST_REQUIRE_EQUAL(Json::parse(output.resultJson)["frames"][0]["tracks"][0]["localId"], 9);
  auto next = window(2);
  for (auto& frame : next.frames) {
    frame.sourceEpoch = 8;
    frame.observation["identity"]["epoch"] = uint64_t(8);
    frame.observation["key"]["epoch"] = uint64_t(8);
  }
  UavVehicleTrackingEngine engine;
  engine.restoreState(output.candidateStateJson, next);
  const auto exported = Json::parse(engine.backendStateJson());
  for (const auto& camera : exported.at("cameras")) BOOST_CHECK(camera.is_null());
  const auto decision = engine.processWindow(next, parseUavVehicleBackendWindow(candidate(next).dump(), next));
  const auto result = Json::parse(decision.resultJson);
  BOOST_CHECK(result.contains("stateResetReason"));
  BOOST_CHECK_EQUAL(result["frames"][0]["tracks"][0]["localId"], 1);
  BOOST_CHECK(result.at("motion").at("measurements").at(0).at("speedMps").is_null());
}

BOOST_AUTO_TEST_CASE(ChangedRunModeDecisionOrSequenceCannotRestoreCommittedState)
{
  const auto first = window();
  const auto state = process(first, candidate(first)).candidateStateJson;
  for (int change = 0; change < 4; ++change) {
    auto next = window(2);
    if (change == 0) next.runId = "other-run";
    if (change == 1) next.observationMode = false;
    if (change == 2) next.nativeDecisionDigest = digest('f');
    if (change == 3) next = window(1);
    UavVehicleTrackingEngine engine;
    BOOST_CHECK_THROW(engine.restoreState(state, next), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(SourceResetReasonKeepsTheOriginalFieldPriority)
{
  const auto first = window();
  const auto state = process(first, candidate(first)).candidateStateJson;
  auto next = window(2);
  auto& frame = next.frames[0];
  frame.sourceId = "replacement-source";
  frame.sourceEpoch = 8;
  frame.observation["identity"]["sourceId"] = frame.sourceId;
  frame.observation["identity"]["epoch"] = frame.sourceEpoch;
  frame.observation["key"]["sourceId"] = frame.sourceId;
  frame.observation["key"]["epoch"] = frame.sourceEpoch;
  const auto result = Json::parse(process(next, candidate(next), state).resultJson);
  BOOST_CHECK_EQUAL(result["stateResetReason"], "STATE_BINDING_MISMATCH:sources.UAV1.sourceId");
}

BOOST_AUTO_TEST_CASE(BackendCannotSupplyFinalIdsOrUnknownTrackerIdentity)
{
  const auto context = window();
  auto backend = candidate(context);
  backend["frames"][0]["detections"][0]["globalId"] = uint64_t(1);
  BOOST_CHECK_THROW(parseUavVehicleBackendWindow(backend.dump(), context), std::exception);
  backend = candidate(context);
  backend["frames"][0]["detections"][0]["byteTrackId"] = uint64_t(1);
  BOOST_CHECK_THROW(parseUavVehicleBackendWindow(backend.dump(), context), std::exception);
  backend = candidate(context);
  backend["frames"][0]["detections"] = Json::array();
  for (size_t i = 0; i < 301; ++i) {
    backend["frames"][0]["detections"].push_back(candidate(context)["frames"][0]["detections"][0]);
  }
  BOOST_CHECK_THROW(parseUavVehicleBackendWindow(backend.dump(), context), std::exception);
}

BOOST_AUTO_TEST_CASE(OrdinaryCocoLabelWithSpacesSurvivesProcessAndRestore)
{
  auto context = window();
  auto backend = candidate(context);
  backend["frames"][0]["detections"][0]["class"] = "traffic light";
  const auto first = process(context, backend);
  BOOST_CHECK_EQUAL(Json::parse(first.resultJson)["frames"][0]["tracks"][0]["class"],
                    "traffic light");
  context = window(2);
  backend = candidate(context);
  backend["frames"][0]["detections"][0]["class"] = "traffic light";
  const auto second = process(context, backend, first.candidateStateJson);
  BOOST_CHECK_EQUAL(Json::parse(second.resultJson)["frames"][0]["tracks"][0]["localId"], 1);
}

BOOST_AUTO_TEST_CASE(AdmittedCandidateCannotBeReboundToDifferentFrame)
{
  const auto context = window();
  const auto backend = parseUavVehicleBackendWindow(candidate(context).dump(), context);
  auto other = context;
  other.frames[0].inputDigest = digest('e');
  UavVehicleTrackingEngine engine;
  engine.restoreState({}, other);
  BOOST_CHECK_THROW(engine.processWindow(other, backend), std::exception);
}

BOOST_AUTO_TEST_CASE(ObservationAndNativeFrameMustHaveTheSameIdentityAndTime)
{
  for (int change = 0; change < 3; ++change) {
    auto context = window();
    if (change == 0) context.frames[0].observation["identity"]["sourceId"] = "other-source";
    if (change == 1) context.frames[0].observation["key"]["sequence"] = uint64_t(2);
    if (change == 2) context.frames[0].observation["captureTimeUs"] = "1";
    BOOST_CHECK_THROW(parseUavVehicleBackendWindow(candidate(context).dump(), context), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(AliasedWindowNamesAndOversizedReasonsAreRejectedAtAdmission)
{
  auto context = window();
  context.windowId = "window-00";
  BOOST_CHECK_THROW(parseUavVehicleBackendWindow(candidate(context).dump(), context), std::exception);
  context = window();
  context.frames[0].observation["reason"] = std::string(1025, 'x');
  BOOST_CHECK_THROW(parseUavVehicleBackendWindow(candidate(context).dump(), context), std::exception);
}

BOOST_AUTO_TEST_CASE(SourceEpochUsesAllSixtyFourBitsWithoutFloatingConversion)
{
  std::string state;
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    auto context = window(sequence);
    for (auto& frame : context.frames) {
      frame.sourceEpoch = std::numeric_limits<uint64_t>::max();
      frame.observation["identity"]["epoch"] = std::to_string(frame.sourceEpoch);
      frame.observation["key"]["epoch"] = std::to_string(frame.sourceEpoch);
      frame.observation["key"]["sequence"] = std::to_string(frame.sequence);
      frame.observation["captureTimeUs"] = std::to_string(frame.commonPtsUs);
    }
    const auto output = process(context, candidate(context), state);
    state = output.candidateStateJson;
    BOOST_CHECK_EQUAL(Json::parse(output.resultJson)["frames"][0]["sourceEpoch"].get<uint64_t>(),
                      std::numeric_limits<uint64_t>::max());
  }
}

BOOST_AUTO_TEST_CASE(TrackerIdsRequireIncrementRoomAndActivatedSnapshotMembership)
{
  const auto context = window();
  for (const auto id : {uint64_t(9), std::numeric_limits<uint64_t>::max() - 1,
                         std::numeric_limits<uint64_t>::max()}) {
    auto backend = candidate(context);
    backend["frames"][0]["detections"][0]["byteTrackId"] = id;
    backend["backendState"]["cameras"]["UAV1"] = {
      {"frameId", uint64_t(1)},
      {"tracked", Json::array({person_fixture::stockTrack(id)})},
      {"lost", Json::array()}, {"removed", Json::array()}};
    if (id == 9) {
      const auto output = process(context, backend);
      BOOST_CHECK_EQUAL(Json::parse(output.resultJson)["frames"][0]["tracks"][0]["localId"], 9);
      UavVehicleTrackingEngine engine;
      BOOST_CHECK_NO_THROW(engine.restoreState(output.candidateStateJson, window(2)));
      backend["backendState"]["cameras"]["UAV1"]["tracked"][0]["isActivated"] = false;
    }
    BOOST_CHECK_THROW(parseUavVehicleBackendWindow(backend.dump(), context), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(GlobalCounterExhaustionDoesNotReturnUnrestorableCandidate)
{
  auto context = window();
  auto state = Json::parse(process(context, candidate(context)).candidateStateJson);
  state["nextGlobal"] = std::numeric_limits<uint64_t>::max() - 1;
  context = window(2);
  UavVehicleTrackingEngine engine;
  engine.restoreState(state.dump(), context);
  auto backend = candidate(context);
  backend["frames"][0]["detections"][0]["class"] = "bus";
  BOOST_CHECK_THROW(engine.processWindow(context,
    parseUavVehicleBackendWindow(backend.dump(), context)), std::exception);
}

BOOST_AUTO_TEST_CASE(FailedRestoreLeavesThePreviouslyPreparedWindowIntact)
{
  auto context = window();
  const auto state = process(context, candidate(context)).candidateStateJson;
  context = window(2);
  UavVehicleTrackingEngine engine;
  engine.restoreState(state, context);
  const auto before = engine.backendStateJson();
  auto invalid = Json::parse(state);
  invalid["last"]["UAV1"][0]["globalId"] = uint64_t(9000);
  BOOST_CHECK_THROW(engine.restoreState(invalid.dump(), context), std::exception);
  BOOST_CHECK_EQUAL(engine.backendStateJson(), before);
  BOOST_CHECK_NO_THROW(engine.processWindow(context,
    parseUavVehicleBackendWindow(candidate(context).dump(), context)));
}

BOOST_AUTO_TEST_CASE(InvalidCovarianceRetainsUnqualifiedSpeed)
{
  std::string state;
  Json result;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    auto context = window(sequence);
    for (auto& frame : context.frames) frame.observation["poseCovariance"][0] = -1.0;
    const auto output = process(context, candidate(context, 50 + static_cast<int>((sequence - 1) * 10)), state);
    state = output.candidateStateJson;
    result = Json::parse(output.resultJson);
  }
  const auto& measurement = result["motion"]["measurements"][0];
  BOOST_CHECK_CLOSE(measurement["speedMps"].get<double>(), 2.0, 1e-8);
  BOOST_CHECK_EQUAL(measurement["bearingStatus"], "unknown");
  BOOST_CHECK(measurement["sigmaSpeedMps"].is_null());
}

BOOST_AUTO_TEST_CASE(NumericStringsRetainTheSameGeometry)
{
  const auto numeric = window();
  auto strings = numeric;
  for (auto& frame : strings.frames) {
    frame.observation["calibration"]["fx"] = "100.0";
    frame.observation["positionW"][2] = "10.0";
    frame.observation["ground"]["offsetM"] = "0.0";
  }
  const auto original = Json::parse(process(numeric, candidate(numeric, 70)).resultJson);
  const auto changed = Json::parse(process(strings, candidate(strings, 70)).resultJson);
  BOOST_CHECK_CLOSE(changed["motion"]["measurements"][0]["horizontalRangeM"].get<double>(),
                    original["motion"]["measurements"][0]["horizontalRangeM"].get<double>(), 1e-8);
}

BOOST_AUTO_TEST_CASE(LegacyHomographyRetainsSourceTimeSpeedAndCompensation)
{
  const auto first = legacyWindow(1);
  const auto state = process(first, candidate(first)).candidateStateJson;
  auto next = legacyWindow(2);
  const auto output = process(next, candidate(next, 60), state);
  const auto result = Json::parse(output.resultJson);
  BOOST_CHECK_EQUAL(result["schema"], "spec191-tracking-result-v1");
  BOOST_REQUIRE_EQUAL(result["motion"]["estimates"].size(), 3);
  const auto& estimate = result["motion"]["estimates"][0];
  // A 10 pixel displacement at 5 mm/pixel over 0.5 seconds gives 100 mm/s.
  BOOST_CHECK_EQUAL(estimate["vehicleSpeedMmps"], 100);
  BOOST_CHECK_EQUAL(estimate["egoMotionCompensation"], "applied");
  BOOST_CHECK_EQUAL(estimate["calibrationDigest"],
    "sha256:90218428e697404ed57ba33d8b5dca02c111bdf7fb8823f0cb67c3480254ac2b");
  next.legacyMotion["motionPolicy"] = {{"egoMotionCompensation", "disabled"}};
  const auto disabled = Json::parse(process(next, candidate(next, 60), state).resultJson);
  BOOST_CHECK_EQUAL(disabled["motion"]["estimates"][0]["status"], "unknown");
  BOOST_CHECK(disabled["motion"]["estimates"][0]["vehicleSpeedMmps"].is_null());
}

BOOST_AUTO_TEST_CASE(LegacyDigestPreservesScientificNotationAndSignedZero)
{
  const auto context = legacyWindow(1, true);
  const auto result = Json::parse(process(context, candidate(context)).resultJson);
  // Independent hashlib/json.dumps oracle over the six canonical profile fields.
  BOOST_CHECK_EQUAL(result["motion"]["calibrationDigests"]["UAV1"],
    "sha256:06b47705685fb5201fd44ccb5b65f14a8f02ae33dbb8f2e8e8b00aea3035492f");
  auto wrong = context;
  wrong.legacyMotion["calibrations"][0]["calibrationDigest"] = digest('f');
  BOOST_CHECK_THROW(process(wrong, candidate(wrong)), std::exception);
}

BOOST_AUTO_TEST_CASE(DirectionalGateAssociatesOnlyMatchingEntryAndLane)
{
  for (int destinationCenter : {10, 50}) {
    auto context = window();
    auto backend = candidate(context);
    backend["frames"][0]["detections"][0]["box"] = {80, 40, 90, 50};
    backend["frames"][1]["detections"][0]["box"] =
      {destinationCenter - 5, 40, destinationCenter + 5, 50};
    const auto result = Json::parse(process(context, backend).resultJson);
    const auto first = result["frames"][0]["tracks"][0]["globalId"];
    const auto second = result["frames"][1]["tracks"][0]["globalId"];
    BOOST_CHECK((first == second) == (destinationCenter == 10));
    BOOST_CHECK_EQUAL(result["frames"][1]["associations"].size(), destinationCenter == 10 ? 1 : 0);
  }
}

BOOST_AUTO_TEST_CASE(EntryInsideTheWrongLaneCannotConsumeASourceIdentity)
{
  const auto context = window();
  auto backend = candidate(context);
  backend["frames"][0]["detections"][0]["box"] = {80, 40, 90, 50};
  backend["frames"][1]["detections"][0]["box"] = {5, 60, 15, 70};
  const auto result = Json::parse(process(context, backend).resultJson);
  BOOST_CHECK(result["frames"][0]["tracks"][0]["globalId"] != result["frames"][1]["tracks"][0]["globalId"]);
  BOOST_CHECK(result["frames"][1]["associations"].empty());
}

BOOST_AUTO_TEST_CASE(AssociationGapIncludesItsBoundaryAndRejectsTheNextMicrosecond)
{
  for (const auto gap : {int64_t(5000000), int64_t(5000001)}) {
    auto context = window();
    context.frames[1].commonPtsUs = gap;
    context.frames[1].observation["captureTimeUs"] = gap;
    auto backend = candidate(context);
    backend["frames"][0]["detections"][0]["box"] = {80, 40, 90, 50};
    backend["frames"][1]["detections"][0]["box"] = {5, 40, 15, 50};
    const auto result = Json::parse(process(context, backend).resultJson);
    BOOST_CHECK_EQUAL(result["frames"][1]["associations"].size(), gap == 5000000 ? 1 : 0);
  }
}

BOOST_AUTO_TEST_CASE(ExactAssociationTieRetainsFirstPublishedSourceCandidate)
{
  const auto context = window();
  auto backend = candidate(context);
  backend["frames"][0]["detections"][0]["box"] = {80, 40, 90, 50};
  backend["frames"][0]["detections"].push_back(backend["frames"][0]["detections"][0]);
  backend["frames"][1]["detections"][0]["box"] = {5, 40, 15, 50};
  const auto result = Json::parse(process(context, backend).resultJson);
  BOOST_REQUIRE_EQUAL(result["frames"][1]["associations"].size(), 1);
  BOOST_CHECK_EQUAL(result["frames"][1]["associations"][0]["sourceLocalId"], 1);
  BOOST_CHECK_EQUAL(result["frames"][1]["tracks"][0]["globalId"],
                    result["frames"][0]["tracks"][0]["globalId"]);
}

BOOST_AUTO_TEST_CASE(LegacyCalibrationKeepsItsDeclaredCoordinates)
{
  auto context = legacyWindow(1);
  for (auto& calibration : context.legacyMotion["calibrations"]) {
    calibration["imageWidth"] = uint64_t(1920);
    calibration["imageHeight"] = uint64_t(1080);
  }
  const auto output = process(context, candidate(context));
  BOOST_CHECK_EQUAL(Json::parse(output.resultJson)["frames"][0]["imageWidth"], 100);
}

BOOST_AUTO_TEST_CASE(NativeCandidateTransactionsPreserveStateAndRejectForeignInputs)
{
  struct Workspace {
    std::filesystem::path path;
    Workspace()
    {
      char name[] = "/tmp/spec228-native-transaction-XXXXXX";
      const auto created = ::mkdtemp(name);
      if (!created) throw std::runtime_error("mkdtemp failed");
      path = created;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); }
  } workspace;
  const auto context = window();
  const auto output = process(context, candidate(context));
  const ndn::Buffer result(output.resultJson.begin(), output.resultJson.end());
  const ndn::Buffer state(output.candidateStateJson.begin(), output.candidateStateJson.end());
  UavTrackingWorkerTransaction transaction(workspace.path);
  bool sawStagedPair = false;
  BOOST_CHECK(!transaction.commit("window-0", digest('d'), result, state, [&] {
    sawStagedPair = std::filesystem::is_regular_file(workspace.path / "window-0/result.json") &&
                    std::filesystem::is_regular_file(workspace.path / "window-0/state.json") &&
                    !std::filesystem::exists(workspace.path / "window-0/manifest.json");
    return false;
  }));
  BOOST_CHECK(sawStagedPair);
  BOOST_CHECK(!transaction.committedStatePath("window-0"));
  BOOST_REQUIRE(transaction.commit("window-0", digest('d'), result, state, [] { return true; }));
  const auto cached = transaction.loadCommittedResult("window-0", digest('d'));
  BOOST_REQUIRE(cached);
  BOOST_CHECK(*cached == result);
  BOOST_CHECK_THROW(transaction.loadCommittedResult("window-0", digest('e')), std::runtime_error);
  const auto statePath = transaction.committedStatePath("window-0");
  BOOST_REQUIRE(statePath);
  std::ifstream input(*statePath);
  const std::string restored((std::istreambuf_iterator<char>(input)), {});
  BOOST_CHECK_EQUAL(restored, output.candidateStateJson);
  const auto next = window(2);
  BOOST_CHECK_NO_THROW(process(next, candidate(next), restored));
  auto foreign = next;
  foreign.nativeDecisionDigest = digest('f');
  BOOST_CHECK_THROW(process(foreign, candidate(foreign), restored), std::invalid_argument);
  foreign = next;
  foreign.runId = "different-run";
  BOOST_CHECK_THROW(process(foreign, candidate(foreign), restored), std::invalid_argument);
  foreign = next;
  foreign.observationMode = false;
  BOOST_CHECK_THROW(process(foreign, candidate(foreign), restored), std::invalid_argument);
  BOOST_CHECK_THROW(process(next, candidate(next), "{\"schema\":\"uav-person-tracking-state-v1\"}"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ExportNumericalInputsForIndependentPythonOracle)
{
  Json cases = Json::array();
  for (const auto* name : {"moving", "moving-camera", "stationary", "missing-covariance",
                           "nonzero-covariance", "distortion"}) {
    std::string state;
    Json samples = Json::array();
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
      const auto seconds = (sequence - 1) * 0.5;
      const bool movingCamera = std::string(name) == "moving-camera";
      auto context = window(sequence, movingCamera ? seconds : 0);
      for (auto& frame : context.frames) {
        if (std::string(name) == "missing-covariance") frame.observation["poseCovariance"] = nullptr;
        if (std::string(name) == "nonzero-covariance") {
          frame.observation["anchorSigmaPixels"] = 0.2;
          frame.observation["poseCovariance"][0] = 0.001;
        }
        if (std::string(name) == "distortion") {
          frame.observation["calibration"]["distortion"][0] = 0.03;
        }
      }
      const auto east = (std::string(name) == "stationary" ? 0.0 : 2 * seconds) -
                        (movingCamera ? seconds : 0.0);
      const auto anchor = static_cast<int>(50 + east * 10);
      const auto output = process(context, candidate(context, anchor), state);
      state = output.candidateStateJson;
      const auto result = Json::parse(output.resultJson);
      BOOST_REQUIRE_EQUAL(result["motion"]["measurements"].size(), 3);
      samples.push_back({{"raw", context.frames[0].observation}, {"anchor", {anchor, 50}},
        {"native", result["motion"]["measurements"][0]}});
    }
    cases.push_back({{"case", name}, {"samples", samples}});
  }
  if (const auto* path = std::getenv("NDNSF_UAV_VEHICLE_ORACLE_OUTPUT")) {
    std::ofstream output(path);
    output << cases.dump() << '\n';
    output.flush();
    BOOST_REQUIRE(output.good());
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
