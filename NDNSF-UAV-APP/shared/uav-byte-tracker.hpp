#ifndef NDNSF_UAV_BYTE_TRACKER_HPP
#define NDNSF_UAV_BYTE_TRACKER_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav::byte_track {

/** ByteTrack state values admitted by the maintained backend-state codec. */
enum class TrackStatus : uint8_t
{
  NEW = 0,
  TRACKED = 1,
  LOST = 2,
  REMOVED = 3
};

using Vector8 = std::array<double, 8>;
using Matrix8 = std::array<Vector8, 8>;
using Box = std::array<double, 4>; // tlwh

/** One detected person in image coordinates. */
struct Detection
{
  Box tlwh{};
  double score = 0;
  double cls = 0;
  double index = 0;
};

/** Kalman state for one local track; application IDs remain outside this module. */
struct Track
{
  uint64_t trackId = 0;
  bool isActivated = false;
  TrackStatus status = TrackStatus::NEW;
  Box initialTlwh{};
  Vector8 mean{};
  Matrix8 covariance{};
  bool initialized = false;
  double score = 0;
  double cls = 0;
  double index = 0;
  std::optional<double> angle;
  uint64_t startFrame = 0;
  uint64_t frameId = 0;
  uint64_t trackletLength = 0;

  /** Current top-left/width/height estimate; falls back to the initial box. */
  Box
  tlwh() const;
};

/**
 * XYAH constant-velocity Kalman filter matching the frozen ByteTrack contract.
 * The state is (x, y, aspect, height, vx, vy, va, vh); measurements are
 * (x, y, aspect, height).  All operations are deterministic and bounded.
 */
class KalmanFilterXYAH
{
public:
  void
  initiate(const Box& measurement, Vector8& mean, Matrix8& covariance) const;

  void
  predict(Vector8& mean, Matrix8& covariance) const;

  /** Update with a detection; returns false on a singular innovation covariance. */
  bool
  update(Vector8& mean, Matrix8& covariance, const Box& measurement) const;

  /** Projected measurement mean/covariance, exposed for association tests. */
  void
  project(const Vector8& mean, const Matrix8& covariance,
          std::array<double, 4>& projectedMean,
          std::array<std::array<double, 4>, 4>& projectedCovariance) const;
};

struct TrackerConfig
{
  double highThreshold = 0.25;
  double lowThreshold = 0.1;
  double newTrackThreshold = 0.25;
  double matchThreshold = 0.8;
  uint64_t trackBuffer = 30;
  bool fuseScore = true;
  size_t maximumTracks = 512;
};

/**
 * Per-camera ByteTrack continuation.  It owns local track IDs and Kalman state
 * only; global person identity and commit decisions stay in the native engine.
 */
class UavByteTracker
{
public:
  explicit UavByteTracker(TrackerConfig config = {});

  /** Advance one frame; returns confirmed, activated tracks in stable order. */
  std::vector<Track>
  update(const std::vector<Detection>& detections);

  const std::vector<Track>&
  tracked() const noexcept
  {
    return m_tracked;
  }

  const std::vector<Track>&
  lost() const noexcept
  {
    return m_lost;
  }

  uint64_t
  frameId() const noexcept
  {
    return m_frameId;
  }

  uint64_t
  idHighWatermark() const noexcept
  {
    return m_nextId - 1;
  }

  /** Compact v3 tracker state JSON consumed by engine admission. */
  std::string
  stateJson() const;

  /** Restore validated v3 tracker state JSON; failure leaves this tracker unchanged. */
  void
  restoreState(const std::string& state);

private:
  void
  activate(Track& track);

  void
  predict(Track& track) const;

  void
  updateTrack(Track& track, const Detection& detection);

  void
  reActivate(Track& track, const Detection& detection);

  void
  markLost(Track& track);

  void
  markRemoved(Track& track);

  TrackerConfig m_config;
  KalmanFilterXYAH m_filter;
  std::vector<Track> m_tracked;
  std::vector<Track> m_lost;
  std::vector<Track> m_removed;
  uint64_t m_frameId = 0;
  uint64_t m_nextId = 1;
};

} // namespace ndnsf::examples::uav::byte_track

#endif // NDNSF_UAV_BYTE_TRACKER_HPP
