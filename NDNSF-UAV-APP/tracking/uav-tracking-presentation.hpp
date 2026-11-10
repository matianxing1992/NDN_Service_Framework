#ifndef NDNSF_UAV_TRACKING_PRESENTATION_HPP
#define NDNSF_UAV_TRACKING_PRESENTATION_HPP

#include "uav-tracking-config.hpp"

#include <ndn-cxx/encoding/buffer.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

constexpr uint64_t MAX_TRACKING_DISPLAY_WIDTH = 3840;
constexpr uint64_t MAX_TRACKING_DISPLAY_HEIGHT = 2160;
constexpr uint64_t MAX_TRACKING_DISPLAY_PIXELS =
  MAX_TRACKING_DISPLAY_WIDTH * MAX_TRACKING_DISPLAY_HEIGHT;

struct TrackingFrameImage
{
  std::string cameraId;
  std::string sourceId;
  ndn::Name producerIdentity;
  uint64_t sourceEpoch = 0;
  uint64_t sequence = 0;
  std::string frameName;
  std::string inputDigest;
  uint64_t width = 0;
  uint64_t height = 0;
  std::shared_ptr<const ndn::Buffer> jpeg;
};

/** One immutable three-view bundle; it retains the exact verified source JPEGs. */
struct TrackingPresentation
{
  TrackingMode mode = TrackingMode::VEHICLE;
  std::string runId;
  uint64_t generation = 0;
  uint64_t windowIndex = 0;
  std::string resultDigest;
  std::shared_ptr<const std::string> resultJson;
  std::array<TrackingFrameImage, 3> frames;
};

struct TrackingDisplayBox
{
  /** Normalized left/top/right/bottom; GTK scales both image and overlay together. */
  std::array<double, 4> box;
  std::string label;
  uint64_t globalId = 0;
  /** Normalized origin x/y and direction x/y, only when the native result supplies it. */
  std::optional<std::array<double, 4>> arrow;
};

struct TrackingCameraDisplay
{
  std::string cameraId;
  uint64_t width = 0;
  uint64_t height = 0;
  std::vector<TrackingDisplayBox> boxes;
};

/** Pure projection of verified results; GTK separately validates actual decode dimensions. */
std::array<TrackingCameraDisplay, 3>
projectTrackingPresentation(const TrackingPresentation& presentation,
                            const TrackingJobDefinition& definition,
                            uint64_t expectedGeneration,
                            const std::string& expectedResultDigest);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_TRACKING_PRESENTATION_HPP
