#ifndef NDNSF_TESTS_UAV_PERSON_WINDOW_HPP
#define NDNSF_TESTS_UAV_PERSON_WINDOW_HPP

#include "tests/fixtures/uav-person-profile-golden.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-tracking.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::test::person_fixture {

using namespace examples::uav;
using Json = nlohmann::json;

inline UavPersonWindowContext
windowContext()
{
  UavPersonWindowContext context;
  context.runId = "run";
  context.missionId = "mission";
  context.windowId = "window";
  context.computationDigest = "sha256:" + std::string(64, 'b');
  context.sequence = 1;
  for (size_t i = 0; i < 3; ++i) {
    auto& frame = context.frames[i];
    frame.cameraId = "UAV" + std::to_string(i + 1);
    frame.sourceId = "/source/" + frame.cameraId;
    frame.inputDigest = "sha256:" + std::string(64, 'a');
    frame.sourceEpoch = 1;
    frame.trackerGeneration = 1;
    frame.sequence = 1;
    frame.commonPtsUs = 1000000;
    frame.width = 640;
    frame.height = 480;
    frame.clockAvailable = true;
    frame.clockUncertaintyUs = 1000;
  }
  return context;
}

inline Json
stockTrack(uint64_t id, uint64_t state = 1)
{
  // Literal stock Kalman layout, independently supplied rather than production-serialized.
  return {{"trackId", id}, {"isActivated", true}, {"state", state},
          {"tlwh", {10.0, 20.0, 40.0, 100.0}},
          {"mean", {30.0, 70.0, 0.4, 100.0, 0.0, 0.0, 0.0, 0.0}},
          {"covariance", std::vector<std::vector<double>>(8, std::vector<double>(8, 0.0))},
          {"score", 0.9}, {"class", 0.0}, {"index", 0.0}, {"angle", nullptr},
          {"startFrame", 1}, {"frameId", 1}, {"timeSinceUpdate", 0}, {"trackletLength", 0}};
}

inline Json
backendFixture()
{
  const auto context = windowContext();
  Json wire = {{"schema", "uav-person-backend-v1"}, {"requestedDevice", "cuda:0"},
    {"actualDetectorDevice", "cuda:0"}, {"detectorModelDevice", "cuda:0"},
    {"detectorInputDevice", "cuda:0"}, {"actualReidDevice", "cuda:0"},
    {"reidModelDevice", "cuda:0"}, {"reidInputDevice", "cuda:0"}, {"reidInferenceBatches", 3},
    {"profileDigest", std::string(PERSON_PROFILE_DIGEST)},
    {"detectorDigest", "sha256:" + std::string(64, 'a')},
    {"reidDigest", "sha256:" + std::string(64, 'b')},
    {"runId", "run"}, {"missionId", "mission"}, {"windowId", "window"}, {"sequence", 1},
    {"computationDigest", context.computationDigest}, {"timing", {{"modelWindowNs", 12345}}},
    {"frames", Json::array()},
    {"backendState", {{"schema", "uav-person-backend-state-v1"}, {"cameras", Json::object()}}}};
  std::vector<double> vector(512, 0.0);
  vector[0] = 1.0;
  for (const auto& frame : context.frames) {
    wire["frames"].push_back({{"cameraId", frame.cameraId}, {"sourceId", frame.sourceId},
      {"sourceEpoch", 1}, {"trackerGeneration", 1}, {"sequence", 1}, {"commonPtsUs", 1000000},
      {"inputDigest", frame.inputDigest}, {"lostByteTrackIds", Json::array()},
      {"detections", Json::array({{{"class", "person"}, {"byteTrackId", 7},
        {"box", {10, 20, 50, 120}}, {"confidence", 0.9}, {"active", true},
        {"embedding", vector}}})}});
    wire["backendState"]["cameras"][frame.cameraId] = {{"sourceId", frame.sourceId},
      {"sourceEpoch", 1}, {"trackerGeneration", 1}, {"tracker", {{"frameId", 1},
        {"tracked", Json::array({stockTrack(7)})}, {"lost", Json::array()},
        {"removed", Json::array()}}}};
  }
  return wire;
}

inline Json
compactBackendFixture()
{
  auto wire = backendFixture();
  wire["backendState"]["schema"] = "uav-person-backend-state-v2";
  for (auto& camera : wire["backendState"]["cameras"].items()) {
    auto& tracker = camera.value()["tracker"];
    tracker.erase("removed");
    tracker["removedIds"] = Json::array();
    tracker["idHighWatermark"] = 7;
  }
  return wire;
}

inline Json
person(uint64_t id, double first = 1.0, double second = 0.0, bool feature = true)
{
  std::vector<double> embedding(512, 0.0);
  embedding[0] = first;
  embedding[1] = second;
  Json result = {{"class", "person"}, {"byteTrackId", id}, {"box", {10, 20, 50, 120}},
                 {"confidence", 0.9}, {"active", true}, {"embedding", embedding}};
  if (!feature) result["embedding"] = nullptr;
  return result;
}

inline UavPersonWindowContext
atWindow(uint64_t sequence, int64_t ptsUs)
{
  auto context = windowContext();
  context.sequence = sequence;
  context.windowId = "window-" + std::to_string(sequence);
  for (auto& frame : context.frames) {
    frame.sequence = sequence;
    frame.commonPtsUs = ptsUs;
  }
  return context;
}

inline UavPersonBackendWindow
admitWindow(const UavPersonWindowContext& context, const UavPersonTrackingProfile& profile,
             const std::array<std::vector<Json>, 3>& detections,
             const std::array<std::vector<uint64_t>, 3>& lost = {})
{
  auto wire = backendFixture();
  wire["runId"] = context.runId;
  wire["missionId"] = context.missionId;
  wire["windowId"] = context.windowId;
  wire["sequence"] = context.sequence;
  wire["computationDigest"] = context.computationDigest;
  wire["profileDigest"] = profile.profileDigest;
  for (size_t i = 0; i < 3; ++i) {
    const auto& source = context.frames[i];
    auto& frame = wire["frames"][i];
    frame["sourceId"] = source.sourceId;
    frame["sourceEpoch"] = source.sourceEpoch;
    frame["trackerGeneration"] = source.trackerGeneration;
    frame["sequence"] = source.sequence;
    frame["commonPtsUs"] = source.commonPtsUs;
    frame["inputDigest"] = source.inputDigest;
    frame["detections"] = detections[i];
    frame["lostByteTrackIds"] = lost[i];
    auto& state = wire["backendState"]["cameras"][source.cameraId];
    state["sourceId"] = source.sourceId;
    state["sourceEpoch"] = source.sourceEpoch;
    state["trackerGeneration"] = source.trackerGeneration;
    auto& tracker = state["tracker"];
    tracker["frameId"] = context.sequence;
    tracker["tracked"] = Json::array();
    tracker["lost"] = Json::array();
    for (const auto& detection : detections[i]) {
      if (!detection.at("active").get<bool>()) continue;
      auto track = stockTrack(detection.at("byteTrackId").get<uint64_t>());
      track["frameId"] = context.sequence;
      tracker["tracked"].push_back(std::move(track));
    }
    for (const auto id : lost[i]) {
      auto track = stockTrack(id, 2);
      track["frameId"] = context.sequence > 1 ? context.sequence - 1 : 1;
      tracker["lost"].push_back(std::move(track));
    }
  }
  return parseUavPersonBackendWindow(wire.dump(), context, profile);
}

inline UavPersonTrackingProfile
withLimit(const UavPersonTrackingProfile& profile, const std::string& key, uint64_t value)
{
  auto wire = Json::parse(profile.canonicalJson);
  wire["limits"][key] = value;
  return parseUavPersonTrackingProfile(wire.dump());
}

} // namespace ndnsf::test::person_fixture

#endif // NDNSF_TESTS_UAV_PERSON_WINDOW_HPP
