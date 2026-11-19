#include "uav-person-backend.hpp"

#include "uav-byte-track-state.hpp"
#include "uav-byte-tracker.hpp"
#include "uav-person-onnx.hpp"
#include "uav-person-tracking.hpp"
#include "uav-person-json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace ndnsf::examples::uav {
namespace {

using byte_track::Detection;
using byte_track::Track;
using person_json::Json;

struct RgbImage
{
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
};

RgbImage
toRgb(const UavRgbImage& image)
{
  return {image.width, image.height, image.rgb};
}

/** Bilinear crop/resize to the frozen 128x256 ReID input. */
std::vector<float>
reidCrop(const RgbImage& image, const std::array<double, 4>& box)
{
  constexpr int width = 128;
  constexpr int height = 256;
  const double x1 = std::clamp(box[0], 0.0, static_cast<double>(image.width));
  const double y1 = std::clamp(box[1], 0.0, static_cast<double>(image.height));
  const double x2 = std::clamp(box[2], 0.0, static_cast<double>(image.width));
  const double y2 = std::clamp(box[3], 0.0, static_cast<double>(image.height));
  if (x2 - x1 < 1 || y2 - y1 < 1) {
    throw std::invalid_argument("ReID crop is empty");
  }
  std::vector<float> result(3 * width * height);
  const double scaleX = (x2 - x1) / width;
  const double scaleY = (y2 - y1) / height;
  constexpr float mean[3] = {0.485F, 0.456F, 0.406F};
  constexpr float stdv[3] = {0.229F, 0.224F, 0.225F};
  for (int dy = 0; dy < height; ++dy) {
    const double sy = y1 + (dy + 0.5) * scaleY - 0.5;
    const int y0 = static_cast<int>(std::clamp(std::floor(sy), y1, y2 - 1));
    const int y3 = std::min<int>(y0 + 1, static_cast<int>(y2) - 1);
    const double wy = std::clamp(sy - y0, 0.0, 1.0);
    for (int dx = 0; dx < width; ++dx) {
      const double sx = x1 + (dx + 0.5) * scaleX - 0.5;
      const int x0 = static_cast<int>(std::clamp(std::floor(sx), x1, x2 - 1));
      const int x3 = std::min<int>(x0 + 1, static_cast<int>(x2) - 1);
      const double wx = std::clamp(sx - x0, 0.0, 1.0);
      for (int channel = 0; channel < 3; ++channel) {
        const auto at = [&] (int x, int y) {
          return static_cast<double>(image.rgb[(static_cast<size_t>(y) * image.width + x) * 3 +
                                             channel]) / 255.0;
        };
        const double top = at(x0, y0) * (1 - wx) + at(x3, y0) * wx;
        const double bottom = at(x0, y3) * (1 - wx) + at(x3, y3) * wx;
        const double value = (top * (1 - wy) + bottom * wy - mean[channel]) / stdv[channel];
        result[static_cast<size_t>(channel) * width * height +
               static_cast<size_t>(dy) * width + dx] = static_cast<float>(value);
      }
    }
  }
  return result;
}

Json
detectionJson(const std::array<int, 4>& box, double confidence, const Json& id,
              const Json& embedding)
{
  return Json{{"class", "person"}, {"byteTrackId", id}, {"box", box},
              {"confidence", confidence}, {"active", !id.is_null()},
              {"embedding", embedding}};
}

std::array<int, 4>
intBox(const std::array<double, 4>& box, uint32_t width, uint32_t height)
{
  std::array<int, 4> result{};
  result[0] = std::clamp(static_cast<int>(std::lround(box[0])), 0, static_cast<int>(width));
  result[1] = std::clamp(static_cast<int>(std::lround(box[1])), 0, static_cast<int>(height));
  result[2] = std::clamp(static_cast<int>(std::lround(box[2])), 0, static_cast<int>(width));
  result[3] = std::clamp(static_cast<int>(std::lround(box[3])), 0, static_cast<int>(height));
  return result;
}

} // namespace

struct UavPersonModelBackend::Impl
{
  UavPersonTrackingProfile profile;
  UavPersonReidSession reid;
  UavPersonDetectorSession detector;

  Impl(const UavPersonTrackingProfile& profileValue,
       const std::filesystem::path& detectorModel,
       const std::filesystem::path& reidModel,
       size_t batch)
    : profile(profileValue)
    , reid(reidModel, static_cast<size_t>(std::min<uint64_t>(batch, 32)))
    , detector(detectorModel, 0.1F, 0.7F,
               static_cast<size_t>(profileValue.limits.at("maxPersonsPerFrame") + 1))
  {
    if (profile.requestedDevice != "cpu") {
      throw std::invalid_argument("native person backend currently supports CPU only");
    }
    if (profile.reidPreprocessId != "osnet-rgb-256x128-imagenet-l2-v1") {
      throw std::invalid_argument("native person backend supports the GAP ReID preprocess only");
    }
  }
};

UavPersonModelBackend::UavPersonModelBackend(const UavPersonTrackingProfile& profile,
                                             const std::filesystem::path& detectorModel,
                                             const std::filesystem::path& reidModel,
                                             size_t reidBatchSize)
{
  if (profile.detectorPreprocessId != "yolo-letterbox640-raw-box-v1") {
    throw std::invalid_argument(
      "native person backend supports the 640 letterbox detector preprocess only");
  }
  m_impl = std::make_unique<Impl>(profile, detectorModel, reidModel, reidBatchSize);
}

UavPersonModelBackend::~UavPersonModelBackend() = default;

const std::string&
UavPersonModelBackend::detectorDigest() const noexcept
{
  return m_impl->detector.modelDigest();
}

const std::string&
UavPersonModelBackend::reidDigest() const noexcept
{
  return m_impl->reid.modelDigest();
}

std::string
UavPersonModelBackend::processWindow(const UavPersonWindowContext& context,
                                     const std::array<ndn::Buffer, 3>& admittedJpegs,
                                     const std::string& committedBackendStateJson,
                                     const std::function<bool()>& stillAuthorized)
{
  const auto& profile = m_impl->profile;
  const auto started = std::chrono::steady_clock::now();
  Json previousCameras;
  if (!committedBackendStateJson.empty()) {
    const auto state = person_json::parseBounded(committedBackendStateJson,
                                                 profile.limits.at("maxSnapshotBytes"));
    person_json::exactKeys(state, {"schema", "cameras"});
    if (person_json::text(state.at("schema")) != "uav-person-backend-state-v3") {
      person_json::reject("unsupported previous backend state");
    }
    person_json::exactKeys(state.at("cameras"), {"UAV1", "UAV2", "UAV3"});
    previousCameras = state.at("cameras");
  }

  Json frames = Json::array();
  Json cameras = Json::object();
  uint64_t reidBatches = 0;
  for (size_t index = 0; index < 3; ++index) {
    if (stillAuthorized && !stillAuthorized()) {
      throw std::runtime_error("person backend cancelled between cameras");
    }
    const auto& expected = context.frames[index];
    const auto rgb = decodePersonJpeg(admittedJpegs[index]);
    if (rgb.width != static_cast<uint32_t>(expected.width) ||
        rgb.height != static_cast<uint32_t>(expected.height)) {
      throw std::invalid_argument("admitted JPEG dimensions disagree with the window context");
    }
    const auto letterboxed = letterboxPersonImage(rgb);
    const auto candidates = m_impl->detector.detect(letterboxed.tensor, stillAuthorized);
    if (candidates.size() > profile.limits.at("maxPersonsPerFrame")) {
      throw std::invalid_argument("person backend exceeded maxPersonsPerFrame");
    }

    // Raw detector observations in original image coordinates.
    std::vector<std::array<double, 4>> rawBoxes;
    std::vector<double> rawScores;
    std::vector<Detection> detections;
    for (const auto& candidate : candidates) {
      std::array<double, 4> box{};
      box[0] = (candidate.box[0] - letterboxed.padX) / letterboxed.scale;
      box[1] = (candidate.box[1] - letterboxed.padY) / letterboxed.scale;
      box[2] = (candidate.box[2] - letterboxed.padX) / letterboxed.scale;
      box[3] = (candidate.box[3] - letterboxed.padY) / letterboxed.scale;
      box[0] = std::clamp(box[0], 0.0, static_cast<double>(rgb.width));
      box[1] = std::clamp(box[1], 0.0, static_cast<double>(rgb.height));
      box[2] = std::clamp(box[2], 0.0, static_cast<double>(rgb.width));
      box[3] = std::clamp(box[3], 0.0, static_cast<double>(rgb.height));
      if (box[2] - box[0] < 1 || box[3] - box[1] < 1) continue;
      Detection detection;
      detection.tlwh = {box[0], box[1], box[2] - box[0], box[3] - box[1]};
      detection.score = candidate.confidence;
      detection.cls = 0;
      detection.index = static_cast<double>(rawBoxes.size());
      rawBoxes.push_back(box);
      rawScores.push_back(candidate.confidence);
      detections.push_back(detection);
    }

    byte_track::UavByteTracker tracker({0.25, 0.1, 0.25, 0.8, 30, true,
      static_cast<size_t>(profile.limits.at("maxBackendTracksPerCamera"))});
    if (!previousCameras.empty()) {
      const auto& previous = previousCameras.at(expected.cameraId);
      tracker.restoreState(previous.at("tracker").dump());
    }
    tracker.update(detections);
    // v3 state carries every current local track, including a newly born,
    // not-yet-activated track.  Emit the same owner set so engine admission
    // does not reject the window for a missing active stock detection.
    std::map<size_t, const Track*> active;
    for (const auto& track : tracker.tracked()) {
      active[static_cast<size_t>(track.index)] = &track;
    }

    // ReID only for detections owned by an active local track.
    std::vector<size_t> activeIndices;
    std::vector<std::array<double, 4>> activeBoxes;
    for (const auto& [rawIndex, track] : active) {
      activeIndices.push_back(rawIndex);
      const auto box = track->tlwh();
      activeBoxes.push_back({box[0], box[1], box[0] + box[2], box[1] + box[3]});
    }
    std::vector<std::vector<float>> features(activeIndices.size());
    const size_t batch = static_cast<size_t>(profile.limits.at("reidBatchSize"));
    for (size_t offset = 0; offset < activeIndices.size(); offset += batch) {
      if (stillAuthorized && !stillAuthorized()) {
        throw std::runtime_error("person backend cancelled before ReID");
      }
      const size_t count = std::min(batch, activeIndices.size() - offset);
      std::vector<float> input;
      input.reserve(count * 3 * 256 * 128);
      for (size_t item = 0; item < count; ++item) {
        const auto crop = reidCrop(toRgb(rgb), activeBoxes[offset + item]);
        input.insert(input.end(), crop.begin(), crop.end());
      }
      const auto embedded = m_impl->reid.embed(input, count, stillAuthorized);
      ++reidBatches;
      for (size_t item = 0; item < count; ++item) {
        features[offset + item].assign(embedded.begin() + item * 512,
                                       embedded.begin() + (item + 1) * 512);
      }
    }
    std::map<size_t, size_t> featureIndex;
    for (size_t item = 0; item < activeIndices.size(); ++item) featureIndex[activeIndices[item]] = item;

    Json outputDetections = Json::array();
    for (size_t raw = 0; raw < rawBoxes.size(); ++raw) {
      const auto owner = active.find(raw);
      if (owner == active.end()) {
        outputDetections.push_back(detectionJson(intBox(rawBoxes[raw], rgb.width, rgb.height),
                                                 rawScores[raw], nullptr, nullptr));
        continue;
      }
      const auto box = owner->second->tlwh();
      const std::array<double, 4> xyxy = {box[0], box[1], box[0] + box[2], box[1] + box[3]};
      Json embedding = Json::array();
      const auto feature = featureIndex.find(raw);
      if (feature != featureIndex.end()) {
        for (const float value : features[feature->second]) embedding.push_back(value);
      }
      outputDetections.push_back(detectionJson(intBox(xyxy, rgb.width, rgb.height),
                                               owner->second->score,
                                               owner->second->trackId,
                                               embedding.empty() ? Json(nullptr) : embedding));
    }
    Json lost = Json::array();
    for (const auto& track : tracker.lost()) lost.push_back(track.trackId);

    frames.push_back(Json{{"cameraId", expected.cameraId}, {"sourceId", expected.sourceId},
      {"sourceEpoch", expected.sourceEpoch}, {"trackerGeneration", expected.trackerGeneration},
      {"sequence", expected.sequence}, {"commonPtsUs", expected.commonPtsUs},
      {"inputDigest", expected.inputDigest}, {"detections", std::move(outputDetections)},
      {"lostByteTrackIds", std::move(lost)}});
    cameras[expected.cameraId] = Json{{"sourceId", expected.sourceId},
      {"sourceEpoch", expected.sourceEpoch}, {"trackerGeneration", expected.trackerGeneration},
      {"tracker", person_json::parseBounded(tracker.stateJson(), profile.limits.at("maxSnapshotBytes"))}};
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  const std::string device = profile.requestedDevice;
  Json result{{"schema", "uav-person-backend-v1"}, {"requestedDevice", device},
    {"actualDetectorDevice", device}, {"detectorModelDevice", device},
    {"detectorInputDevice", device},
    {"actualReidDevice", reidBatches ? device : "not-run"},
    {"reidModelDevice", device}, {"reidInputDevice", reidBatches ? device : "not-run"},
    {"reidInferenceBatches", reidBatches}, {"detectorDigest", profile.detectorDigest},
    {"reidDigest", profile.reidDigest}, {"profileDigest", profile.profileDigest},
    {"frames", std::move(frames)},
    {"backendState", Json{{"schema", "uav-person-backend-state-v3"}, {"cameras", std::move(cameras)}}},
    {"timing", Json{{"modelWindowNs", elapsed}}}, {"runId", context.runId},
    {"missionId", context.missionId}, {"windowId", context.windowId},
    {"sequence", context.sequence}, {"computationDigest", context.computationDigest}};
  const auto text = result.dump();
  if (text.size() > profile.limits.at("maxResultBytes")) {
    throw std::invalid_argument("native person backend result exceeds its byte bound");
  }
  return text;
}

} // namespace ndnsf::examples::uav
