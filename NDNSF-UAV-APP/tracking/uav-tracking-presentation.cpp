#include "uav-tracking-presentation.hpp"

#include "../shared/UavDigest.hpp"
#include "../shared/UavNames.hpp"
#include "../shared/uav-person-json.hpp"
#include "../shared/uav-person-published-frame.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

using Json = person_json::Json;

void
requireDisplay(bool condition, const char* reason)
{
  if (!condition) throw std::invalid_argument(reason);
}

std::string
digest(std::string_view bytes)
{
  ndn::util::Sha256 hash;
  hash << bytes;
  return canonicalSha256Digest(hash.toString());
}

uint64_t
number(const Json& value)
{
  requireDisplay(value.is_number_integer(), "display integer required");
  requireDisplay(value.is_number_unsigned() || value.get<int64_t>() >= 0,
                 "negative display integer");
  return value.get<uint64_t>();
}

double
coordinate(const Json& value)
{
  requireDisplay(value.is_number(), "display coordinate required");
  const auto result = value.get<double>();
  requireDisplay(std::isfinite(result), "nonfinite display coordinate");
  return result;
}

std::string
motionLabel(const Json& track)
{
  if (!track.contains("measurement")) return "motion UNKNOWN";
  const auto& motion = track.at("measurement");
  if (motion.value("speedStatus", "unknown") != "valid" ||
      !motion.contains("speedMps") || motion.at("speedMps").is_null()) {
    return "motion UNKNOWN";
  }
  const auto speed = coordinate(motion.at("speedMps"));
  requireDisplay(speed >= 0, "negative display speed");
  std::ostringstream text;
  text << std::fixed << std::setprecision(1) << speed << " m/s";
  return text.str();
}

} // namespace

std::array<TrackingCameraDisplay, 3>
projectTrackingPresentation(const TrackingPresentation& presentation,
                            const TrackingJobDefinition& definition,
                            uint64_t expectedGeneration,
                            const std::string& expectedResultDigest)
{
  requireDisplay(presentation.runId == definition.runId && presentation.mode == definition.mode &&
                   expectedGeneration != 0 && presentation.generation == expectedGeneration &&
                   presentation.windowIndex < definition.windowCount &&
                   presentation.resultDigest == expectedResultDigest &&
                   presentation.resultJson && digest(*presentation.resultJson) == presentation.resultDigest,
                 "stale or mismatched display bundle");
  const auto result = person_json::parseBounded(*presentation.resultJson, 8 * 1024 * 1024);
  requireDisplay(result.at("runId") == definition.runId &&
                   result.at("missionId") == "tracking-" + definition.runId &&
                   result.at("windowId") == "window-" + std::to_string(presentation.windowIndex),
                 "display result execution mismatch");
  requireDisplay(result.at("schema") == (definition.mode == TrackingMode::PERSON ?
                   "uav-person-tracking-result-v1" : "spec210-tracking-result-v2") &&
                   result.at("terminal") == true, "display result schema or terminal mismatch");
  const auto& frames = result.at("frames");
  requireDisplay(frames.is_array() && frames.size() == 3, "display requires three frames");
  const bool person = definition.mode == TrackingMode::PERSON;
  std::array<TrackingCameraDisplay, 3> output;
  for (size_t i = 0; i < 3; ++i) {
    const auto& image = presentation.frames[i];
    const auto& source = definition.sources[i];
    const auto& frame = frames.at(i);
    requireDisplay(image.cameraId == source.cameraId && image.sourceId == source.sourceId &&
                     image.sourceEpoch == source.sourceEpoch &&
                     image.producerIdentity == source.producerIdentity &&
                     image.sequence == presentation.windowIndex + 1 &&
                     frame.at("cameraId") == image.cameraId &&
                     frame.at("sourceId") == image.sourceId &&
                     number(frame.at("sourceEpoch")) == image.sourceEpoch &&
                     number(frame.at("sequence")) == image.sequence,
                   "display source identity mismatch");
    const auto& reference = person ? frame.at("observation").at("imageRef") : frame.at("imageRef");
    requireDisplay(reference.at("name") == image.frameName &&
                     reference.at("digest") == image.inputDigest &&
                     frame.at("inputDigest") == image.inputDigest &&
                     makeUavObservationDataName(image.producerIdentity, image.cameraId,
                       image.sourceEpoch, image.sequence, "FRAME", 1).toUri() == image.frameName,
                   "display original FRAME reference mismatch");
    if (reference.contains("producerIdentity")) {
      requireDisplay(reference.at("producerIdentity") == image.producerIdentity.toUri(),
                     "display FRAME producer mismatch");
    }
    requireDisplay(image.width > 0 && image.width <= MAX_TRACKING_DISPLAY_WIDTH && image.height > 0 &&
                     image.height <= MAX_TRACKING_DISPLAY_HEIGHT && image.width * image.height <= MAX_TRACKING_DISPLAY_PIXELS &&
                     image.width == number(frame.at(person ? "width" : "imageWidth")) &&
                     image.height == number(frame.at(person ? "height" : "imageHeight")),
                   "display dimensions exceed bounds or disagree with result");
    requireDisplay(image.jpeg && !image.jpeg->empty() &&
                     image.jpeg->size() <= MAX_PERSON_PUBLISHED_FRAME_BYTES &&
                     digest(std::string_view(reinterpret_cast<const char*>(image.jpeg->data()),
                                             image.jpeg->size())) == image.inputDigest,
                   "display JPEG bytes/digest mismatch");
    auto& camera = output[i];
    camera.cameraId = image.cameraId;
    camera.width = image.width;
    camera.height = image.height;
    const auto& tracks = frame.at("tracks");
    requireDisplay(tracks.is_array() && tracks.size() <= (person ? 64 : 300),
                   "display track count exceeds bound");
    for (const auto& track : tracks) {
      const auto& box = track.at("box");
      requireDisplay(box.is_array() && box.size() == 4, "display box shape");
      const auto x1 = coordinate(box.at(0)), y1 = coordinate(box.at(1));
      const auto x2 = coordinate(box.at(2)), y2 = coordinate(box.at(3));
      requireDisplay(x1 >= 0 && y1 >= 0 && x2 > x1 && y2 > y1 &&
                       x2 <= image.width && y2 <= image.height, "display box out of bounds");
      TrackingDisplayBox display;
      display.box = {x1 / image.width, y1 / image.height, x2 / image.width, y2 / image.height};
      const auto& id = track.at("globalId");
      if (id.is_null()) display.label = "ID UNKNOWN";
      else {
        const auto globalId = number(id);
        requireDisplay(globalId != 0, "zero display identity");
        display.globalId = globalId;
        display.label = "ID " + std::to_string(globalId);
      }
      display.label += " | " + (person ? std::string("geometry UNKNOWN") : motionLabel(track));
      if (!person && track.contains("measurement")) {
        const auto& measurement = track.at("measurement");
        if (measurement.contains("imageArrow") && !measurement.at("imageArrow").is_null()) {
          const auto& arrow = measurement.at("imageArrow");
          const auto& origin = arrow.at("originPx");
          const auto& direction = arrow.at("directionPx");
          requireDisplay(origin.is_array() && origin.size() == 2 && direction.is_array() &&
                           direction.size() == 2, "display arrow shape");
          const auto x = coordinate(origin.at(0)), y = coordinate(origin.at(1));
          const auto dx = coordinate(direction.at(0)), dy = coordinate(direction.at(1));
          requireDisplay(x >= 0 && x <= image.width && y >= 0 && y <= image.height &&
                           std::abs(dx) <= 100000 && std::abs(dy) <= 100000,
                         "display arrow bounds");
          display.arrow = std::array<double, 4>{x / image.width, y / image.height,
                                                dx / image.width, dy / image.height};
        }
      }
      camera.boxes.push_back(std::move(display));
    }
  }
  return output;
}

} // namespace ndnsf::examples::uav
