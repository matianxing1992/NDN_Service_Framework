#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace ndnsf::examples::uav {

enum class UavVideoPipelineState
{
  Idle,
  Running,
  Stopped,
  Failed,
};

struct UavVideoFrame
{
  uint64_t sessionEpoch = 0;
  uint64_t sourceFrameId = 0;
  uint64_t captureOriginNs = 0;
  int64_t codecPts = 0;
  uint64_t codecConfigEpoch = 0;
  bool keyFrame = false;
  std::vector<uint8_t> bytes;
};

struct UavVideoQueueSnapshot
{
  size_t queuedFrames = 0;
  size_t queuedBytes = 0;
  uint64_t droppedFrames = 0;
  std::string lastDropReason;
};

struct UavVideoPipelineCapabilities
{
  std::string backend;
  bool available = false;
  bool preservesPts = false;
  bool boundedLifecycle = false;
  bool headless = false;
  bool directPresentationObservation = false;
  std::string reason;
};

struct UavVideoPipelineFailure
{
  std::string direction;
  std::string code;
  std::string reason;
};

class BoundedLatestFrameQueue
{
public:
  BoundedLatestFrameQueue(size_t maxFrames, size_t maxBytes);

  bool push(UavVideoFrame frame);
  std::optional<UavVideoFrame> popLatest();
  void clear(const std::string& reason = "queue-cleared");
  UavVideoQueueSnapshot snapshot() const;

private:
  void dropOldest(const std::string& reason);

private:
  size_t m_maxFrames;
  size_t m_maxBytes;
  size_t m_queuedBytes = 0;
  uint64_t m_droppedFrames = 0;
  std::string m_lastDropReason;
  std::deque<UavVideoFrame> m_frames;
  mutable std::mutex m_mutex;
};

class LegacyPipeVideoPipeline
{
public:
  using FrameCallback = std::function<void(const UavVideoFrame&)>;

  explicit LegacyPipeVideoPipeline(bool headless);

  UavVideoPipelineCapabilities probeCapabilities() const;
  void startDecode(FrameCallback callback);
  bool submitAccessUnit(const UavVideoFrame& frame);
  void stop();

  UavVideoPipelineState state() const;
  bool isHeadless() const;

private:
  bool m_headless;
  FrameCallback m_callback;
  UavVideoPipelineState m_state = UavVideoPipelineState::Idle;
  mutable std::mutex m_mutex;
};

struct UavVideoCaptureConfig
{
  std::string source = "videotestsrc";
  uint32_t width = 320;
  uint32_t height = 240;
  uint32_t fps = 30;
  uint32_t bitrateKbps = 2000;
  uint32_t keyFrameInterval = 30;
};

enum class UavVideoSampleClassMode
{
  ExactKeyDelta,
  BoundedOpaque,
};

class UavVideoSampleClassSchedule
{
public:
  static UavVideoSampleClassSchedule exactKeyDelta(
    uint32_t fps, size_t hardMaxSources, uint64_t sessionGeneration);
  static UavVideoSampleClassSchedule boundedOpaque(
    uint32_t fps, size_t hardMaxSources, uint64_t sessionGeneration);

  UavVideoSampleClassMode mode() const;
  uint32_t fps() const;
  size_t hardMaxSources() const;
  uint64_t sessionGeneration() const;
  bool hasExactFrameClass() const;
  std::string classFor(uint64_t sampleId) const;
  bool matchesActual(uint64_t sampleId, bool keyFrame) const;

private:
  UavVideoSampleClassSchedule(UavVideoSampleClassMode mode, uint32_t fps,
                              size_t hardMaxSources,
                              uint64_t sessionGeneration);

private:
  UavVideoSampleClassMode m_mode;
  uint32_t m_fps;
  size_t m_hardMaxSources;
  uint64_t m_sessionGeneration;
};

class GStreamerVideoPipeline
{
public:
  using FrameCallback = std::function<void(const UavVideoFrame&)>;

  GStreamerVideoPipeline();
  ~GStreamerVideoPipeline();
  GStreamerVideoPipeline(const GStreamerVideoPipeline&) = delete;
  GStreamerVideoPipeline& operator=(const GStreamerVideoPipeline&) = delete;

  UavVideoPipelineCapabilities probeCapabilities() const;
  void startCapture(const UavVideoCaptureConfig& config, FrameCallback callback);
  void startDecode(FrameCallback callback);
  bool submitAccessUnit(const UavVideoFrame& frame);
  void stop();
  UavVideoPipelineState state() const;
  std::optional<UavVideoPipelineFailure> failure() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::examples::uav
