#include "../../NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"

#include "../../NDNSF-UAV-APP/shared/UavNames.hpp"
#include "../../NDNSF-UAV-APP/shared/UavProtocol.hpp"
#include "../../NDNSF-UAV-APP/shared/uav-person-json.hpp"

#include <boost/test/unit_test.hpp>

#include <sstream>

using namespace ndnsf::examples::uav;

namespace {

UavTrackingResultExpectation
expectation()
{
  UavTrackingResultExpectation value;
  value.runId = "run-210";
  value.missionId = "mission-210";
  value.windowId = "window-0";
  value.sequence = 1;
  value.sourceEpoch = 42;
  value.computeProvider = ndn::Name("/example/uav/compute");
  for (size_t index = 0; index < value.sourceIds.size(); ++index) {
    value.sourceIds[index] = "source-UAV" + std::to_string(index + 1);
    value.inputDigests[index] = "sha256:" + std::string(64, static_cast<char>('a' + index));
  }
  return value;
}

std::string
makeResultJson(const UavTrackingResultExpectation& expected, bool includePresentationMetadata = false)
{
  std::ostringstream json;
  json << "{\"schema\":\"spec210-tracking-result-v2\",\"runId\":\""
       << expected.runId << "\",\"missionId\":\"" << expected.missionId
       << "\",\"windowId\":\"" << expected.windowId << "\",\"sequence\":"
       << expected.sequence << ",\"terminal\":true,\"frames\":[";
  for (size_t index = 0; index < expected.sourceIds.size(); ++index) {
    if (index != 0) json << ',';
    const std::string camera = "UAV" + std::to_string(index + 1);
    const auto observationName = makeUavObservationDataName(
      droneIdentity(camera), camera, expected.sourceEpoch, expected.sequence, "OBSERVATION", 1);
    const std::string observationDigest = "sha256:" + std::string(64, 'd');
    json << "{\"cameraId\":\"" << camera << "\",\"sequence\":" << expected.sequence
         << ",\"sourceId\":\"" << expected.sourceIds[index]
         << "\",\"sourceEpoch\":" << expected.sourceEpoch
         << ",\"inputDigest\":\"" << expected.inputDigests[index]
         << "\",\"observationName\":\"" << observationName.toUri()
         << "\",\"observationDigest\":\"" << observationDigest
         << "\",\"observation\":{\"schema\":\"uav-observation-v2\",\"sourceId\":\""
         << expected.sourceIds[index] << "\",\"sourceEpoch\":" << expected.sourceEpoch
         << ",\"sequence\":" << expected.sequence;
    if (includePresentationMetadata) {
      json << ",\"identity\":{\"sourceId\":\"" << expected.sourceIds[index]
           << "\",\"droneId\":\"" << camera << "\",\"cameraId\":\"" << camera
           << "\",\"epoch\":" << expected.sourceEpoch
           << ",\"worldFrameId\":\"ENU-local\",\"acquisitionKind\":\"simulated\","
              "\"deliveryMode\":\"replay\"},\"clockBinding\":{\"revision\":1,"
              "\"clockId\":\"playback\",\"clockEpoch\":1,\"a\":1,\"bUs\":0,"
              "\"validFromUs\":0,\"validUntilUs\":9000000000,\"uncertaintyUs\":0},"
              "\"timeUncertaintyUs\":0,\"anchorApproximation\":true";
    }
    json << ",\"observationRef\":{\"name\":\""
         << observationName.toUri() << "\",\"digest\":\"" << observationDigest << "\"}}}";
  }
  json << "]}";
  return json.str();
}

void
replaceFirst(std::string& value, const std::string& before, const std::string& after)
{
  const auto position = value.find(before);
  BOOST_REQUIRE(position != std::string::npos);
  value.replace(position, before.size(), after);
}

void
replaceSecond(std::string& value, const std::string& before, const std::string& after)
{
  const auto first = value.find(before);
  BOOST_REQUIRE(first != std::string::npos);
  const auto second = value.find(before, first + before.size());
  BOOST_REQUIRE(second != std::string::npos);
  value.replace(second, before.size(), after);
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingResultVerifierTests)

BOOST_AUTO_TEST_CASE(ExplicitConfiguredProducersAreRequiredForEachCamera)
{
  auto expected = expectation();
  auto result = makeResultJson(expected);
  for (size_t i = 0; i < 3; ++i) {
    const auto before = "/example/uav/drone/UAV" + std::to_string(i + 1);
    const auto after = "/lab/uav/source-" + std::to_string(i + 1);
    expected.sourceProviders[i] = ndn::Name(after);
    size_t position = 0;
    while ((position = result.find(before, position)) != std::string::npos) {
      result.replace(position, before.size(), after);
      position += after.size();
    }
  }
  const auto resultName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), result, expected));
  auto changed = expected;
  changed.sourceProviders[1] = changed.sourceProviders[0];
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), result, changed), std::invalid_argument);
  changed = expected;
  changed.sourceProviders[1] = ndn::Name();
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), result, changed), std::invalid_argument);
  changed = expected;
  std::swap(changed.sourceProviders[0], changed.sourceProviders[1]);
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), result, changed), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(AcceptsResultBoundToSelectedRequestAndSources)
{
  const auto expected = expectation();
  const auto resultName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), makeResultJson(expected), expected));
}

BOOST_AUTO_TEST_CASE(AcceptsAdditivePresentationMetadataWithoutWeakeningContextBinding)
{
  const auto expected = expectation();
  const auto resultName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  const auto result = makeResultJson(expected, true);
  BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), result, expected));
  auto changed = result;
  replaceFirst(changed, "\"sourceId\":\"source-UAV1\"", "\"sourceId\":\"other\"");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), changed, expected), std::runtime_error);
  changed = result;
  replaceFirst(changed, "\"timeUncertaintyUs\":0", "\"timeUncertaintyUs\":0,\"timeUncertaintyUs\":1");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), changed, expected), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RejectsMismatchedResultRequestAndSourceBindings)
{
  const auto expected = expectation();
  const auto resultName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  const auto valid = makeResultJson(expected);

  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), "/example/uav/unselected-compute", valid, expected), std::runtime_error);
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    "/example/uav/compute/UAV/MULTIVIEW/MISSION/old/JOB/window-0/RESULT/1/v=1",
    expected.computeProvider.toUri(), valid, expected), std::runtime_error);

  auto wrongRun = valid;
  replaceFirst(wrongRun, "run-210", "run-old");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), wrongRun, expected), std::runtime_error);

  auto wrongFrameSource = valid;
  replaceFirst(wrongFrameSource, "source-UAV2", "source-old2");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), wrongFrameSource, expected),
    std::runtime_error);

  auto wrongObservationEpoch = valid;
  replaceFirst(wrongObservationEpoch, "\"sourceEpoch\":42,\"sequence\":1,\"observationRef\"",
               "\"sourceEpoch\":43,\"sequence\":1,\"observationRef\"");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), wrongObservationEpoch, expected),
    std::runtime_error);

  auto wrongReferenceName = valid;
  replaceSecond(wrongReferenceName, "/42/1/OBSERVATION", "/42/2/OBSERVATION");
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), wrongReferenceName, expected),
    std::runtime_error);

  auto deeplyNested = valid;
  std::string nested(65, '[');
  nested.append(65, ']');
  deeplyNested.insert(deeplyNested.size() - 1, ",\"extra\":" + nested);
  BOOST_CHECK_THROW(validateUavTrackingResultContext(
    resultName.toUri(), expected.computeProvider.toUri(), deeplyNested, expected),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(AcceptsAllDemoCaptureSequencesIncludingBinaryColon)
{
  for (uint64_t sequence = 1; sequence <= 100; ++sequence) {
    auto expected = expectation();
    expected.sequence = sequence;
    expected.windowId = "window-" + std::to_string(sequence - 1);
    const auto name = makeUavMultiViewResultName(
      expected.computeProvider, expected.missionId, expected.windowId, sequence, 1);
    const auto json = makeResultJson(expected);
    BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(
      name.toUri(), expected.computeProvider.toUri(), json, expected));
    const auto wrongSequence = makeUavMultiViewResultName(
      expected.computeProvider, expected.missionId, expected.windowId, sequence + 1, 1);
    BOOST_CHECK_THROW(validateUavTrackingResultContext(
      wrongSequence.toUri(), expected.computeProvider.toUri(), json, expected), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(NativeVehicleAdmissionPinsDecisionAndOriginalFrames)
{
  using person_json::Json;
  auto expected = expectation();
  expected.vehicleTracking = UavVehicleTrackingResultExpectation{true,
    "sha256:" + std::string(64, 'e')};
  for (size_t i = 0; i < 3; ++i) {
    expected.sourceProviders[i] = droneIdentity("UAV" + std::to_string(i + 1));
  }
  auto result = Json::parse(makeResultJson(expected));
  result["decisionSchema"] = "uav-vehicle-native-decision-v1";
  result["nativeDecisionDigest"] = expected.vehicleTracking->nativeDecisionDigest;
  result["actualDevice"] = "cpu";
  for (size_t i = 0; i < 3; ++i) {
    auto& frame = result["frames"][i];
    frame["ptsUs"] = 1000;
    frame["imageWidth"] = 640;
    frame["imageHeight"] = 480;
    frame["imageRef"] = {{"name", makeUavObservationDataName(expected.sourceProviders[i],
      "UAV" + std::to_string(i + 1), expected.sourceEpoch, expected.sequence, "FRAME", 1).toUri()},
      {"digest", expected.inputDigests[i]}};
  }
  const auto name = makeUavMultiViewResultName(expected.computeProvider, expected.missionId,
    expected.windowId, expected.sequence, 1).toUri();
  auto check = [&] (const Json& value) {
    validateUavTrackingResultContext(name, expected.computeProvider.toUri(), value.dump(), expected);
  };
  BOOST_CHECK_NO_THROW(check(result));
  auto changed = result;
  changed["nativeDecisionDigest"] = "sha256:" + std::string(64, 'f');
  BOOST_CHECK_THROW(check(changed), std::runtime_error);
  changed = result;
  changed.erase("decisionSchema");
  BOOST_CHECK_THROW(check(changed), std::exception);
  changed = result;
  changed["frames"][0]["imageRef"]["name"] = result["frames"][1]["imageRef"]["name"];
  BOOST_CHECK_THROW(check(changed), std::runtime_error);
  changed = result;
  changed["frames"][0]["imageRef"]["digest"] = expected.inputDigests[1];
  BOOST_CHECK_THROW(check(changed), std::runtime_error);
  for (const auto& invalid : {Json("1000"), Json(-1), Json(1.5), Json(uint64_t(1) << 63)}) {
    changed = result;
    changed["frames"][0]["ptsUs"] = invalid;
    BOOST_CHECK_THROW(check(changed), std::invalid_argument);
  }
  changed = result;
  changed["frames"][0]["imageWidth"] = 0;
  BOOST_CHECK_THROW(check(changed), std::invalid_argument);
  expected.sourceProviders[1] = ndn::Name();
  BOOST_CHECK_THROW(check(result), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LegacyNativeVehicleStillRequiresItsOriginalEvidence)
{
  using person_json::Json;
  auto expected = expectation();
  expected.sourceEpoch = 0;
  expected.sourceIds = {};
  expected.vehicleTracking = UavVehicleTrackingResultExpectation{false, {}};
  Json result = {{"schema", "spec191-tracking-result-v1"}, {"runId", expected.runId},
    {"missionId", expected.missionId}, {"windowId", expected.windowId},
    {"sequence", expected.sequence}, {"terminal", true}, {"actualDevice", "cpu"},
    {"decisionSchema", "uav-vehicle-native-decision-v1"},
    {"nativeDecisionDigest", "sha256:" + std::string(64, 'a')}, {"frames", Json::array()}};
  for (size_t i = 0; i < 3; ++i) {
    const auto camera = "UAV" + std::to_string(i + 1);
    expected.sourceProviders[i] = droneIdentity(camera);
    result["frames"].push_back({{"cameraId", camera}, {"sequence", expected.sequence},
      {"ptsUs", 0}, {"imageWidth", 640}, {"imageHeight", 480},
      {"inputDigest", expected.inputDigests[i]}, {"imageRef", {
        {"name", makeUavEvidenceName(expected.sourceProviders[i], expected.missionId,
          expected.windowId, camera + "-1", 1).toUri()}, {"digest", expected.inputDigests[i]}}}});
  }
  const auto name = makeUavMultiViewResultName(expected.computeProvider, expected.missionId,
    expected.windowId, expected.sequence, 1).toUri();
  BOOST_CHECK_NO_THROW(validateUavTrackingResultContext(name, expected.computeProvider.toUri(),
                                                      result.dump(), expected));
  result["frames"][0]["imageRef"]["name"] = "/unrelated/FRAME";
  BOOST_CHECK_THROW(validateUavTrackingResultContext(name, expected.computeProvider.toUri(),
                                                   result.dump(), expected), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
