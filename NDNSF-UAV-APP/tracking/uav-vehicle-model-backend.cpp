#include "uav-vehicle-model-backend.hpp"

#include "../shared/UavDigest.hpp"
#include "../shared/uav-byte-tracker.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace ndnsf::examples::uav {
namespace {

using person_json::Json;

constexpr size_t MAXIMUM_BYTES = 8 * 1024 * 1024;

void
appendDetection(Json& output, const byte_track::Box& box, size_t width, size_t height,
                double score, const std::string& label, std::optional<uint64_t> trackId)
{
  const double maxX = static_cast<double>(width);
  const double maxY = static_cast<double>(height);
  const std::array<int, 4> rounded{
    static_cast<int>(std::nearbyint(std::clamp(box[0], 0.0, maxX))),
    static_cast<int>(std::nearbyint(std::clamp(box[1], 0.0, maxY))),
    static_cast<int>(std::nearbyint(std::clamp(box[0] + box[2], 0.0, maxX))),
    static_cast<int>(std::nearbyint(std::clamp(box[1] + box[3], 0.0, maxY)))};
  if (rounded[0] >= rounded[2] || rounded[1] >= rounded[3]) {
    return;
  }
  output.push_back({{"box", rounded}, {"confidence", score}, {"class", label},
                    {"byteTrackId", trackId ? Json(*trackId) : Json(nullptr)}});
}

std::vector<byte_track::Detection>
sourceDetections(const UavRectLetterboxedImage& input,
                 const std::vector<UavVehicleBoxDetection>& boxes)
{
  const auto width = input.image.sourceWidth;
  const auto height = input.image.sourceHeight;
  const double gain = std::min(static_cast<double>(input.tensorWidth) / width,
                               static_cast<double>(input.tensorHeight) / height);
  const auto resizedWidth = std::max(1.0, std::nearbyint(width * gain));
  const auto resizedHeight = std::max(1.0, std::nearbyint(height * gain));
  const auto gainX = static_cast<float>(resizedWidth / width);
  const auto gainY = static_cast<float>(resizedHeight / height);
  const auto padX = static_cast<float>(std::nearbyint((input.tensorWidth - resizedWidth) / 2 - 0.1));
  const auto padY = static_cast<float>(std::nearbyint((input.tensorHeight - resizedHeight) / 2 - 0.1));
  const auto toX = [&] (float x) {
    return std::clamp((x - padX) / gainX, 0.0F, static_cast<float>(width));
  };
  const auto toY = [&] (float y) {
    return std::clamp((y - padY) / gainY, 0.0F, static_cast<float>(height));
  };
  std::vector<byte_track::Detection> result;
  result.reserve(boxes.size());
  for (const auto& box : boxes) {
    const auto x1 = toX(box.box[0]);
    const auto y1 = toY(box.box[1]);
    const auto x2 = toX(box.box[2]);
    const auto y2 = toY(box.box[3]);
    result.push_back({{x1, y1, x2 - x1, y2 - y1}, box.confidence,
                      static_cast<double>(box.classId), static_cast<double>(result.size())});
  }
  return result;
}

} // namespace

UavVehicleModelBackend::UavVehicleModelBackend(const std::filesystem::path& detectorModel)
  : m_detector(detectorModel, 0.25F)
{
}

VehicleBackendWindow
UavVehicleModelBackend::processWindow(const VehicleWindowContext& context,
                                      const std::array<ndn::Buffer, 3>& admittedJpegs,
                                      const std::string& committedBackendState,
                                      const std::function<bool()>& stillAuthorized)
{
  if (!context.nativeBackend) {
    throw std::invalid_argument("native vehicle backend requires a native window");
  }
  const auto previous = person_json::parseBounded(committedBackendState, MAXIMUM_BYTES);
  person_json::exactKeys(previous, {"schema", "cameras"});
  if (person_json::text(previous.at("schema")) != VEHICLE_NATIVE_BACKEND_STATE_SCHEMA) {
    throw std::invalid_argument("native vehicle tracker state schema mismatch");
  }
  const auto& previousCameras = previous.at("cameras");
  person_json::exactKeys(previousCameras, {"UAV1", "UAV2", "UAV3"});

  const auto& classNames = m_detector.modelClassNames();
  Json frames = Json::array();
  Json cameras = Json::object();
  for (size_t i = 0; i < context.frames.size(); ++i) {
    if (stillAuthorized && !stillAuthorized()) {
      throw std::runtime_error("vehicle backend cancelled before camera inference");
    }
    const auto& expected = context.frames[i];
    const auto& jpeg = admittedJpegs[i];
    if (jpeg.empty() || jpeg.size() > MAXIMUM_BYTES) {
      throw std::invalid_argument("native vehicle JPEG byte bound");
    }
    ndn::util::Sha256 digest;
    digest << ndn::span<const uint8_t>(jpeg.data(), jpeg.size());
    if (canonicalSha256Digest(digest.toString()) != expected.inputDigest) {
      throw std::invalid_argument("native vehicle JPEG digest differs from admitted frame");
    }
    const auto rgb = decodePersonJpeg(jpeg);
    if ((expected.width > 0 && rgb.width != static_cast<uint32_t>(expected.width)) ||
        (expected.height > 0 && rgb.height != static_cast<uint32_t>(expected.height))) {
      throw std::invalid_argument("native vehicle JPEG dimensions differ from admitted frame");
    }
    const auto input = letterboxRectangularImage(rgb);
    const auto detections = sourceDetections(input, m_detector.detect(input, stillAuthorized));
    byte_track::UavByteTracker tracker;
    const auto& old = previousCameras.at(expected.cameraId);
    if (!old.is_null()) {
      tracker.restoreState(old.dump());
    }
    const auto confirmed = tracker.update(detections);
    Json outputs = Json::array();
    for (const auto& track : confirmed) {
      appendDetection(outputs, track.tlwh(), rgb.width, rgb.height, track.score,
                      classNames.at(static_cast<size_t>(track.cls)), track.trackId);
    }
    // Stock ByteTrack passes raw boxes through only when it has no confirmed or pending tracks.
    const bool awaitingConfirmation = std::any_of(tracker.tracked().begin(), tracker.tracked().end(),
      [] (const auto& track) { return !track.isActivated; });
    if (confirmed.empty() && !awaitingConfirmation) {
      for (const auto& detection : detections) {
        appendDetection(outputs, detection.tlwh, rgb.width, rgb.height, detection.score,
                        classNames.at(static_cast<size_t>(detection.cls)), std::nullopt);
      }
    }
    frames.push_back({{"cameraId", expected.cameraId}, {"width", rgb.width},
                      {"height", rgb.height}, {"detections", std::move(outputs)}});
    cameras[expected.cameraId] = person_json::parseBounded(tracker.stateJson(), MAXIMUM_BYTES);
  }
  if (stillAuthorized && !stillAuthorized()) {
    throw std::runtime_error("vehicle backend cancelled before returning candidate");
  }
  const auto wire = Json{{"schema", VEHICLE_NATIVE_BACKEND_WINDOW_SCHEMA},
                         {"runId", context.runId}, {"missionId", context.missionId},
                         {"windowId", context.windowId}, {"sequence", context.sequence},
                         {"actualDevice", "cpu"}, {"frames", std::move(frames)},
                         {"backendState", {{"schema", VEHICLE_NATIVE_BACKEND_STATE_SCHEMA},
                                           {"cameras", std::move(cameras)}}}}.dump();
  return parseUavVehicleBackendWindow(wire, context);
}

} // namespace ndnsf::examples::uav
